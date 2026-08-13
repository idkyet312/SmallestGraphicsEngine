struct FogCluster
{
    uint lightCount;
    uint lightIndices[32];
};

struct FogPointLight
{
    float3 position;
    float radius;
    float3 color;
    float intensity;
};

cbuffer FogConstants : register(b0)
{
    float4x4 inverseViewProjection;
    float4x4 shadowCascadeMatrices[3];
    float4 cameraPositionNear;
    float4 cameraForwardFar;
    float4 sunDirectionDensity;
    float4 sunColorAnisotropy;
    float4 fogParams;             // height falloff, base height, max distance, shadow enabled
    float4 ambientFogColor;
    float4 shadowCascadeSplits;
    uint4 clusterDimsLightCount;  // x, y, z, source light count
    uint4 volumeDims;
    float4 atmosphereParams;      // Rayleigh, Mie, Mie g, aerial density
    float4 cloudParams;           // coverage, density, base height, thickness
    float4 oceanBounds0;          // center.x, surface y, center.z, enabled
    float4 oceanBounds1;          // half x, half z, edge blend width, unused
    uint4 maxVolumeDims;          // allocated froxel volume size (>= volumeDims)
};

StructuredBuffer<FogCluster> clusters : register(t0);
StructuredBuffer<FogPointLight> pointLights : register(t1);
Texture2DArray<float> shadowMap : register(t2);
Texture2D<float> sceneDepth : register(t3);
Texture3D<float4> fogVolume : register(t4);
Texture2DMS<float, 4> sceneDepthMS : register(t5);
RWTexture3D<float4> fogVolumeOut : register(u0);

SamplerComparisonState shadowSampler : register(s0);
SamplerState linearClampSampler : register(s1);

float SliceDepth(float slice)
{
    const float nearZ = cameraPositionNear.w;
    const float farZ = min(cameraForwardFar.w, fogParams.z);
    return nearZ * pow(farZ / nearZ, slice / volumeDims.z);
}

float3 WorldRay(float2 uv)
{
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 world = mul(float4(ndc, 1.0, 1.0), inverseViewProjection);
    world.xyz /= max(abs(world.w), 1e-5);
    return normalize(world.xyz - cameraPositionNear.xyz);
}

// rotation lets the caller decorrelate the tap pattern per froxel, so the filter
// reads as soft gradient rather than as a repeated kernel shape.
float SunVisibility(float3 worldPosition, float rotation)
{
    if (fogParams.w < 0.5)
        return 1.0;
    float viewDepth = dot(worldPosition - cameraPositionNear.xyz,
                          normalize(cameraForwardFar.xyz));
    uint cascade = viewDepth < shadowCascadeSplits.x ? 0u :
                   (viewDepth < shadowCascadeSplits.y ? 1u : 2u);
    float4 projected = mul(float4(worldPosition, 1.0),
                           shadowCascadeMatrices[cascade]);
    projected.xyz /= max(abs(projected.w), 1e-5);
    float2 uv = projected.xy * float2(0.5, -0.5) + 0.5;
    if (any(uv < 0.0) || any(uv > 1.0) || projected.z <= 0.0 || projected.z >= 1.0)
        return 1.0;

    // A single comparison tap makes every shaft edge a hard shadow-texel
    // boundary swept along the view ray, which is what read as straight
    // diagonal streaks radiating from the sun. Raising the froxel grid cannot
    // help: the aliasing is in the shadow lookup, not the froxel spacing.
    // Average a small rotated disc of taps so the boundary becomes a gradient
    // the width of the filter.
    const float2 kTaps[4] = {
        float2( 0.936,  0.352), float2(-0.352,  0.936),
        float2(-0.936, -0.352), float2( 0.352, -0.936)
    };
    float sine, cosine;
    sincos(rotation, sine, cosine);
    float2x2 rotate = float2x2(cosine, -sine, sine, cosine);
    // Cascades cover progressively larger areas, so widen in texels, not world
    // units, to keep the softness visually consistent across cascade splits.
    // Kept narrow deliberately: widening this to 3 texels softened the occluder
    // edge enough that sun-facing views bled a bright veil over the hillside in
    // front of the sun -- the same failure the phase clamp below guards against.
    float radius = 1.6 / 2048.0;
    float visibility = shadowMap.SampleCmpLevelZero(
        shadowSampler, float3(uv, cascade), projected.z - 0.0025);
    [unroll]
    for (uint i = 0; i < 4; ++i)
        visibility += shadowMap.SampleCmpLevelZero(
            shadowSampler,
            float3(uv + mul(kTaps[i], rotate) * radius, cascade),
            projected.z - 0.0025);
    return visibility * 0.2;
}

float HenyeyGreenstein(float cosineTheta, float g)
{
    float g2 = g * g;
    float denominator = max(1.0 + g2 - 2.0 * g * cosineTheta, 1e-4);
    return (1.0 - g2) /
        max(4.0 * 3.14159265 * pow(denominator, 1.5), 1e-4);
}

// Jimenez's interleaved gradient noise: a cheap hash whose values decorrelate
// strongly between neighbouring pixels, which is what makes the froxel sampling
// offset read as fine dither rather than as a pattern.
float InterleavedGradientNoise(float2 pixel)
{
    return frac(52.9829189 *
        frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

float FogNoise(float3 p)
{
    // Broad moving density cells. Stable in world space; time only advects them.
    p += float3(ambientFogColor.w * 0.42, 0.0, ambientFogColor.w * 0.19);
    float n = sin(p.x * 0.105 + sin(p.z * 0.071)) *
              sin(p.z * 0.093 - p.y * 0.17);
    n += sin((p.x + p.z) * 0.037 + ambientFogColor.w * 0.31) * 0.55;
    return saturate(n * 0.32 + 0.62);
}

float CloudSunVisibility(float3 worldPosition)
{
    if (atmosphereParams.x <= 0.001 || cloudParams.x <= 0.001)
        return 1.0;
    float2 wind = float2(ambientFogColor.w * 2.1,
                         ambientFogColor.w * 1.0);
    float2 projected = worldPosition.xz +
        normalize(sunDirectionDensity.xz + 1e-4) *
        max(cloudParams.z - worldPosition.y, 0.0);
    float clouds = FogNoise(float3(projected * 0.014 + wind, 7.0));
    float threshold = lerp(0.78, 0.26, cloudParams.x);
    float opticalDepth = saturate((clouds - threshold) * 2.2) *
                         cloudParams.y;
    return exp(-opticalDepth * 0.75);
}

float3 AtmosphereAmbient(float3 ray, float aboveBase)
{
    if (atmosphereParams.x <= 0.001)
        return ambientFogColor.xyz;
    float horizon = pow(1.0 - saturate(ray.y), 2.0);
    float3 rayleighColor = float3(0.46, 0.68, 1.0);
    float3 sky = lerp(rayleighColor, ambientFogColor.xyz,
                      0.35 + horizon * 0.45);
    float3 groundBounce = float3(0.36, 0.44, 0.28) *
        exp(-aboveBase * 0.055);
    float3 daylightAmbient = sky * (0.52 + atmosphereParams.x * 0.26) +
        groundBounce * atmosphereParams.w * 0.34;
    // The Rayleigh colour above is daylight irradiance. Reusing it after the sun
    // drops below the horizon turns even low-density fog into a white-blue veil.
    // Keep daylight and dusk on their untouched path; only true night fades to
    // the authored cool fog tint.
    if (sunDirectionDensity.y < 0.06)
    {
        float nightBlend = 1.0 - smoothstep(
            -0.10, 0.06, sunDirectionDensity.y);
        return lerp(
            daylightAmbient, ambientFogColor.xyz * 0.12, nightBlend);
    }
    return daylightAmbient;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= volumeDims.x || id.y >= volumeDims.y)
        return;

    float2 uv = (float2(id.xy) + 0.5) / float2(volumeDims.xy);
    float3 ray = WorldRay(uv);
    float viewCos = max(dot(ray, normalize(cameraForwardFar.xyz)), 0.08);
    float3 accumulated = 0.0;
    float transmittance = 1.0;
    const float3 lightDirection = normalize(sunDirectionDensity.xyz);
    // Preserve forward scattering without letting a g=0.9 sun-facing view
    // explode into a white veil that erases trees in front of the sun.
    const float phase = min(HenyeyGreenstein(
        dot(lightDirection, ray), sunColorAnisotropy.w), 1.65);

    // Each froxel takes a single shadow sample, so a shaft edge that falls
    // mid-slice snaps to the froxel boundary and the shaft reads as stepped.
    // Raising the grid resolution shrinks those steps but never removes them.
    // Offsetting the sample position within the slice, by a per-pixel amount
    // that also changes each frame, turns the hard step into noise that the
    // accumulation and frame-to-frame persistence average into a smooth edge.
    float sliceJitter = frac(
        InterleavedGradientNoise(float2(id.xy)) + float(volumeDims.w) * 0.618034);

    [loop]
    for (uint z = 0; z < volumeDims.z; ++z)
    {
        float nearDepth = SliceDepth((float)z);
        float farDepth = SliceDepth((float)z + 1.0);
        float centerDepth = lerp(nearDepth, farDepth, sliceJitter);
        float stepLength = (farDepth - nearDepth) / viewCos;
        float3 worldPosition = cameraPositionNear.xyz + ray * (centerDepth / viewCos);

        // Jungle gradient: a broad upper haze plus a softer, denser low ground
        // layer that pools in the valley floor. The ground layer's falloff is kept
        // gentle (2x, not a sharp spike) so it fades smoothly into the upper haze
        // with no hard seam where it cuts off across tall grass and terrain.
        float aboveBase = max(worldPosition.y - fogParams.y, 0.0);
        float heightDensity = exp(-aboveBase * fogParams.x);
        float groundLayer = exp(-aboveBase * (fogParams.x * 2.0)) * 0.9;
        float densityNoise = lerp(0.58, 1.28, FogNoise(worldPosition));
        heightDensity = (heightDensity + groundLayer) * densityNoise;
        heightDensity = max(heightDensity, 0.03);
        float extinction = max(sunDirectionDensity.w * heightDensity, 0.00001);
        float segmentTransmittance = exp(-extinction * stepLength);

        // Two shadow evaluations per froxel, at different depths inside the
        // slice. Slice thickness grows exponentially with distance, so a single
        // sample makes a shaft edge crossing a far slice snap to that whole
        // slab -- visible as banding across the shaft. Sampling near the front
        // and back of the slice and averaging turns that step into a ramp.
        // Rotate the PCF disc per froxel and per slice so neighbouring samples
        // do not share a kernel orientation; combined with sliceJitter the
        // residual pattern averages into smooth falloff.
        float rotation = (sliceJitter + float(z) * 0.618034) * 6.2831853;
        float secondDepth = lerp(nearDepth, farDepth, frac(sliceJitter + 0.5));
        float3 secondPosition =
            cameraPositionNear.xyz + ray * (secondDepth / viewCos);
        float shadow = 0.5 * (
            SunVisibility(worldPosition, rotation) +
            SunVisibility(secondPosition, rotation + 1.5707963));
        shadow *= CloudSunVisibility(worldPosition);
        // Daylight fog must replace attenuated scene energy with sky irradiance.
        // A tiny ambient term made the former pass behave like red/brown smoke.
        float3 lighting = AtmosphereAmbient(ray, aboveBase) +
            sunColorAnisotropy.xyz * phase * shadow * 0.42;
        uint3 clusterCoord = min(
            uint3(
                id.x * clusterDimsLightCount.x / volumeDims.x,
                id.y * clusterDimsLightCount.y / volumeDims.y,
                z * clusterDimsLightCount.z / volumeDims.z),
            clusterDimsLightCount.xyz - 1u);
        uint clusterIndex = clusterCoord.x +
            clusterCoord.y * clusterDimsLightCount.x +
            clusterCoord.z * clusterDimsLightCount.x *
                clusterDimsLightCount.y;
        FogCluster cluster = clusters[clusterIndex];
        uint count = min(cluster.lightCount, 32u);
        [loop]
        for (uint i = 0; i < count; ++i)
        {
            uint lightIndex = cluster.lightIndices[i];
            if (lightIndex >= clusterDimsLightCount.w)
                continue;
            FogPointLight light = pointLights[lightIndex];
            float3 toLight = light.position - worldPosition;
            float distanceToLight = length(toLight);
            float range = saturate(1.0 - distanceToLight / max(light.radius, 0.001));
            lighting += light.color * light.intensity * range * range * 0.10;
        }

        float3 segmentScattering = lighting * (1.0 - segmentTransmittance);
        accumulated += transmittance * segmentScattering;
        transmittance *= segmentTransmittance;
        fogVolumeOut[uint3(id.xy, z)] = float4(accumulated, transmittance);
    }
}

struct VSOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

VSOutput VSMain(uint vertexId : SV_VertexID)
{
    VSOutput output;
    float2 p = float2((vertexId << 1) & 2, vertexId & 2);
    output.uv = p;
    output.position = float4(p * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}

float LinearDepth(float depth)
{
    float nearZ = cameraPositionNear.w;
    float farZ = cameraForwardFar.w;
    return nearZ * farZ / max(farZ - depth * (farZ - nearZ), 1e-5);
}

// The froxel volume is always allocated at the high-res size, but the compute
// pass only fills volumeDims when the high-res toggle is off. Rescale the
// normalised coordinate into the written region, and inset by half a texel so
// bilinear filtering never reaches past the last written froxel into stale data.
float3 FogVolumeUVW(float2 uv, float slice)
{
    float3 written = float3(volumeDims.xyz);
    float3 allocated = float3(maxVolumeDims.xyz);
    float3 scale = written / allocated;
    float3 halfTexel = 0.5 / allocated;

    // Froxel z accumulates the range [SliceDepth(z), SliceDepth(z+1)] and is
    // stored at texel index z, i.e. texel centre (z + 0.5) / gridZ. Sampling the
    // raw slice coordinate is therefore half a texel too far and bilinear blends
    // in the NEXT froxel. Where the depth buffer stops at terrain, that next
    // froxel is the unoccluded volume behind the hill, which bled full sunlight
    // through the slope. Pull back by half a texel and clamp so the deepest
    // sample can never reach past the froxel the surface actually occupies.
    float sliceTexel = max(slice * written.z - 0.5, 0.0);
    float sliceUV = min(sliceTexel, written.z - 1.0) / allocated.z;

    float2 planar = clamp(uv * scale.xy, halfTexel.xy, scale.xy - halfTexel.xy);
    return float3(planar, clamp(sliceUV, halfTexel.z, scale.z - halfTexel.z));
}

float4 FadeFogAtRange(float4 fog, float viewDepth, float fogFar)
{
    // Fog composites as `scattering + scene * transmittance`. Fade toward that
    // operation's neutral value (0 scattering, 1 transmittance), otherwise
    // fading RGB alone leaves a dark band where the volume ends.
    float fade = 1.0 - smoothstep(fogFar * 0.80, fogFar, viewDepth);
    return float4(fog.rgb * fade, lerp(1.0, fog.a, fade));
}

float4 FogAtViewDepth(float2 uv, float viewDepth)
{
    float nearZ = cameraPositionNear.w;
    float fogFar = min(cameraForwardFar.w, fogParams.z);
    float clampedDepth = min(viewDepth, fogFar);
    float slice = log(max(clampedDepth, nearZ) / nearZ) /
                  log(fogFar / nearZ);
    float4 fog = fogVolume.SampleLevel(
        linearClampSampler, FogVolumeUVW(uv, saturate(slice)), 0.0);
    return FadeFogAtRange(fog, viewDepth, fogFar);
}

float OceanSurfaceViewDepth(float2 uv, out float coverage)
{
    coverage = 0.0;
    if (oceanBounds0.w < 0.5)
        return 0.0;

    float3 ray = WorldRay(uv);
    if (abs(ray.y) < 1e-5)
        return 0.0;
    float distanceAlongRay =
        (oceanBounds0.y - cameraPositionNear.y) / ray.y;
    if (distanceAlongRay <= 0.0)
        return 0.0;

    float3 hit = cameraPositionNear.xyz + ray * distanceAlongRay;
    float2 fromCenter = abs(hit.xz - oceanBounds0.xz);
    float edgeDistance = min(
        oceanBounds1.x - fromCenter.x,
        oceanBounds1.y - fromCenter.y);
    if (edgeDistance <= 0.0)
        return 0.0;

    // Water has no DSV, so opaque seabed depth abruptly becomes depth=1 at the
    // terrain clipmap edge. Intersect the actual ocean footprint instead and
    // softly retire that replacement depth only at the ocean's own map edge.
    coverage = smoothstep(
        0.0, max(oceanBounds1.z, 1e-3), edgeDistance);
    return distanceAlongRay * max(
        dot(ray, normalize(cameraForwardFar.xyz)), 0.08);
}

float4 CompositeFog(float2 uv, float deviceDepth)
{
    const float4 neutralFog = float4(0.0, 0.0, 0.0, 1.0);
    bool hasOpaqueDepth = deviceDepth < 0.99999;
    float opaqueViewDepth = hasOpaqueDepth
        ? LinearDepth(deviceDepth) : cameraForwardFar.w;
    float4 opaqueFog = hasOpaqueDepth
        ? FogAtViewDepth(uv, opaqueViewDepth) : neutralFog;

    float oceanCoverage;
    float oceanViewDepth = OceanSurfaceViewDepth(uv, oceanCoverage);
    if (oceanCoverage <= 0.0 ||
        (hasOpaqueDepth && oceanViewDepth >= opaqueViewDepth))
        return opaqueFog;

    float4 oceanFog = FogAtViewDepth(uv, oceanViewDepth);
    return lerp(opaqueFog, oceanFog, oceanCoverage);
}

float4 PSMain(VSOutput input) : SV_TARGET
{
    float deviceDepth = sceneDepth.SampleLevel(linearClampSampler, input.uv, 0.0);
    return CompositeFog(input.uv, deviceDepth);
}

float4 PSMainMSAA(VSOutput input) : SV_TARGET
{
    int2 pixel = int2(input.position.xy);
    float deviceDepth = sceneDepthMS.Load(pixel, 0);
    [unroll]
    for (uint sampleIndex = 1; sampleIndex < 4; ++sampleIndex)
        deviceDepth = min(deviceDepth, sceneDepthMS.Load(pixel, sampleIndex));
    return CompositeFog(input.uv, deviceDepth);
}
