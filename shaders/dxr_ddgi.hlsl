// DXR Tier 1.0 sparse-probe update library. One launch maps to one directional
// texel, so rays never race while writing an octahedral probe tile.
RaytracingAccelerationStructure StaticScene : register(t0);

struct ProbeData {
    float3 position;
    float radius;
    float3 normal;
    uint state;
    uint2 stableId;
    uint lastUpdatedFrame;
    uint padding;
};
StructuredBuffer<ProbeData> probes : register(t1);
Texture2D<float4> previousIrradiance : register(t4);
RWTexture2D<float4> currentIrradiance : register(u0);
RWTexture2D<float2> currentDistanceMoments : register(u1);

cbuffer ProbeConstants : register(b0) {
    uint startProbe;
    uint probeCount;
    uint raysPerProbe;
    uint atlasColumns;
    uint irradianceTileSize;
    uint frameIndex;
    float hysteresis;
    float multiBounceStrength;
    float maxRayDistance;
    float surfaceBias;
    float3 sunDirection;
    float sunIntensity;
    float3 sunColor;
    float skyIntensity;
    float3 pointLightPosition;
    float pointLightRadius;
    float3 pointLightColor;
    float pointLightIntensity;
};

struct RadiancePayload {
    float3 radiance;
    float distance;
};
struct ShadowPayload { uint visible; };

float3 FibonacciDirection(uint index, uint count, uint scramble) {
    const float golden = 2.39996323;
    float y = 1.0 - 2.0 * ((index + 0.5) / max((float)count, 1.0));
    float radius = sqrt(max(0.0, 1.0 - y * y));
    float phi = golden * (index + scramble * 0.6180339);
    return float3(cos(phi) * radius, y, sin(phi) * radius);
}

float2 OctEncode(float3 direction) {
    direction /= max(abs(direction.x) + abs(direction.y) +
                     abs(direction.z), 1e-5);
    if (direction.y < 0.0) {
        float2 signs = float2(direction.x >= 0.0 ? 1.0 : -1.0,
                              direction.z >= 0.0 ? 1.0 : -1.0);
        direction.xz = (1.0 - abs(direction.zx)) * signs;
    }
    return direction.xz * 0.5 + 0.5;
}

float3 OctDecode(float2 encoded) {
    float2 f = encoded * 2.0 - 1.0;
    float3 direction = float3(f.x, 1.0 - abs(f.x) - abs(f.y), f.y);
    if (direction.y < 0.0) {
        float2 oldXZ = direction.xz;
        direction.xz = (1.0 - abs(oldXZ.yx)) *
            float2(oldXZ.x >= 0.0 ? 1.0 : -1.0,
                   oldXZ.y >= 0.0 ? 1.0 : -1.0);
    }
    return normalize(direction);
}

[shader("raygeneration")]
void ProbeRayGen() {
    uint2 launch = DispatchRaysIndex().xy;
    uint probeIndex = startProbe + launch.y;
    if (launch.y >= probeCount || launch.x >= raysPerProbe) return;
    ProbeData probe = probes[probeIndex];
    if (probe.state == 2) return;
    uint tileInterior = irradianceTileSize - 2;
    uint2 directionTexel = uint2(
        launch.x % tileInterior, launch.x / tileInterior);
    float3 direction = OctDecode(
        (float2(directionTexel) + 0.5) / tileInterior);
    RayDesc ray;
    ray.Origin = probe.position + probe.normal * surfaceBias;
    ray.Direction = direction;
    ray.TMin = 0.02;
    ray.TMax = maxRayDistance;
    RadiancePayload payload;
    payload.radiance = float3(0.0, 0.0, 0.0);
    payload.distance = maxRayDistance;
    TraceRay(StaticScene, RAY_FLAG_NONE, 0xff, 0, 0, 0, ray, payload);

    uint2 probeTile = uint2(probeIndex % atlasColumns,
                            probeIndex / atlasColumns);
    uint2 tile = probeTile * irradianceTileSize;
    float2 octUV = OctEncode(direction);
    uint2 texel = tile + 1 + min(
        uint2(octUV * tileInterior), tileInterior - 1);
    float3 previous = previousIrradiance[texel].rgb;
    float3 sampleValue = min(payload.radiance, previous * 8.0 + 24.0);
    sampleValue += previous * multiBounceStrength;
    currentIrradiance[texel] =
        float4(lerp(sampleValue, previous, hysteresis), 1.0);
    const uint visibilityTileSize = 18;
    const uint visibilityInterior = 16;
    uint2 visibilityTexel = probeTile * visibilityTileSize + 1 + min(
        uint2(octUV * visibilityInterior), visibilityInterior - 1);
    currentDistanceMoments[visibilityTexel] =
        float2(payload.distance, payload.distance * payload.distance);
}

[shader("miss")]
void RadianceMiss(inout RadiancePayload payload) {
    float horizon = saturate(WorldRayDirection().y * 0.5 + 0.5);
    payload.radiance = lerp(float3(0.06, 0.08, 0.12),
        float3(0.36, 0.52, 0.82), horizon) * skyIntensity;
    payload.distance = maxRayDistance;
}

[shader("miss")]
void ShadowMiss(inout ShadowPayload payload) { payload.visible = 1; }

[shader("closesthit")]
void SurfaceClosestHit(inout RadiancePayload payload,
                       BuiltInTriangleIntersectionAttributes attributes) {
    // DXR still traces the real static BLAS/TLAS. Until material-local hit
    // records are added, use a stable diffuse approximation at the real hit.
    float3 normal = normalize(-WorldRayDirection());
    float3 baseColor = float3(0.72, 0.70, 0.66);
    float3 hit = WorldRayOrigin() + WorldRayDirection() * RayTCurrent();
    float3 lightDirection = normalize(-sunDirection);
    float lightDistance = maxRayDistance;
    float3 directRadiance = sunColor * sunIntensity;
    if (pointLightIntensity > 0.0 && pointLightRadius > 0.0) {
        float3 toLight = pointLightPosition - hit;
        lightDistance = length(toLight);
        lightDirection = toLight / max(lightDistance, 1e-4);
        float range = saturate(1.0 - lightDistance / pointLightRadius);
        float attenuation = range * range /
            max(1.0 + 4.5 * lightDistance / pointLightRadius +
                75.0 * lightDistance * lightDistance /
                    (pointLightRadius * pointLightRadius), 1e-4);
        directRadiance = pointLightColor * pointLightIntensity * attenuation;
    }
    ShadowPayload shadow = { 0 };
    RayDesc shadowRay;
    shadowRay.Origin = hit + normal * surfaceBias;
    shadowRay.Direction = lightDirection;
    shadowRay.TMin = 0.02;
    shadowRay.TMax = min(maxRayDistance, max(lightDistance - 0.04, 0.02));
    TraceRay(StaticScene,
        RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
        RAY_FLAG_SKIP_CLOSEST_HIT_SHADER, 0xff, 0, 0, 1,
        shadowRay, shadow);
    float diffuse = saturate(dot(normal, lightDirection));
    payload.radiance = baseColor * directRadiance *
                       diffuse * shadow.visible;
    payload.distance = RayTCurrent();
}
