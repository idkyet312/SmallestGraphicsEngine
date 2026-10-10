// Screen-space heightfield intersection. The input is the undisplaced, visible
// terrain; hidden slopes and off-screen geometry are deliberately not invented.
#include "terrain_visibility_id.hlsli"

cbuffer DisplacementConstants : register(b0) {
    matrix inverseViewProjection;
    matrix viewProjection;
    float3 cameraPosition;
    float strength;
    float2 screenSize;
    float fadeStart;
    float fadeEnd;
    float2 splatInvExtent;
    uint splatEnabled;
    uint authoredPaths;
    uint neutralHeightMask;
    uint marchSteps;
    float maxRayTravel;
    float edgeFadePixels;
};

Texture2D<uint2> sourceVisibility : register(t0);
Texture2D<float> sourceDepth : register(t1);
Texture2D<float2> sourceHeight : register(t2);
Texture2DArray<float4> albedoMap : register(t3);
Texture2DArray<float4> normalMap : register(t4);
Texture2DArray<float4> metalRoughMap : register(t5);
Texture2D<float4> terrainSplatMap : register(t6);
SamplerState texSampler : register(s0);
SamplerState screenSampler : register(s1);

#define viewPos cameraPosition
#define materialType (authoredPaths != 0u ? 3.0f : 0.0f)
#define materialTime 0u
#define normalYSign (-1.0)
#define terrainSplatEnabled splatEnabled
#define terrainSplatInvExtent splatInvExtent
#define terrainSplatSampler screenSampler
#define SGE_TERRAIN_FORWARD_SPLAT 1

uint MatVarHashUint(uint value) {
    value ^= value >> 16; value *= 0x7feb352du;
    value ^= value >> 15; value *= 0x846ca68bu;
    return value ^ (value >> 16);
}
float MatVarHash(int3 cell) {
    uint value = MatVarHashUint(asuint(cell.x) ^
        MatVarHashUint(asuint(cell.y) + MatVarHashUint(asuint(cell.z))) + 0x9e3779b9u);
    return float(value & 0x00ffffffu) / 16777216.0;
}
float MatVarNoise(float3 position) {
    int3 cell = (int3)floor(position);
    float3 blend = frac(position);
    blend = blend * blend * (3.0 - 2.0 * blend);
    float a = lerp(MatVarHash(cell), MatVarHash(cell + int3(1, 0, 0)), blend.x);
    float b = lerp(MatVarHash(cell + int3(0, 1, 0)), MatVarHash(cell + int3(1, 1, 0)), blend.x);
    float c = lerp(MatVarHash(cell + int3(0, 0, 1)), MatVarHash(cell + int3(1, 0, 1)), blend.x);
    float d = lerp(MatVarHash(cell + int3(0, 1, 1)), MatVarHash(cell + int3(1, 1, 1)), blend.x);
    return lerp(lerp(a, b, blend.y), lerp(c, d, blend.y), blend.z);
}
#include "terrain_pbr.hlsli"

float4 FullscreenVS(uint vertex : SV_VertexID) : SV_Position {
    float2 uv = float2((vertex << 1u) & 2u, vertex & 2u);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

float3 WorldPosition(float2 uv, float depth) {
    float4 surfacePoint = mul(float4(uv * float2(2, -2) + float2(-1, 1), depth, 1),
                       inverseViewProjection);
    return surfacePoint.xyz / surfacePoint.w;
}
float3 DecodeNormal(uint packed) {
    float2 f = float2(packed & 65535u, packed >> 16u) / 65535.0 * 2.0 - 1.0;
    float3 normal = float3(f, 1.0 - abs(f.x) - abs(f.y));
    float t = saturate(-normal.z);
    normal.xy += float2(normal.x >= 0 ? -t : t, normal.y >= 0 ? -t : t);
    return normalize(normal);
}
bool Project(float3 surfacePoint, out float2 uv) {
    float4 clip = mul(float4(surfacePoint, 1), viewProjection);
    uv = clip.xy / max(clip.w, 1e-6) * float2(0.5, -0.5) + 0.5;
    return clip.w > 1e-5 && all(uv >= 0.5 / screenSize) &&
           all(uv <= 1.0 - 0.5 / screenSize);
}
float EdgeFade(float2 uv) {
    float2 edge = min(uv, 1.0 - uv) * screenSize;
    return smoothstep(0.0, edgeFadePixels, min(edge.x, edge.y));
}

// Analytic footprints avoid derivatives of neighbouring sky/mesh pixels and
// remain valid inside the material branches and the later ray march.
float3 PlaneNeighbour(float2 uv, float3 surfacePoint, float3 normal) {
    float3 ray = normalize(WorldPosition(uv, 0.5) - cameraPosition);
    float denominator = dot(ray, normal);
    if (abs(denominator) < 0.04) return surfacePoint;
    float travel = dot(surfacePoint - cameraPosition, normal) / denominator;
    return cameraPosition + ray * travel;
}

float2 HeightPS(float4 position : SV_Position) : SV_Target0 {
    uint2 pixel = (uint2)position.xy;
    uint2 vis = sourceVisibility.Load(int3(pixel, 0));
    if (!IsTerrainVisibilityID(vis.x)) return 0.0;
    float2 uv = position.xy / screenSize;
    float depth = sourceDepth.Load(int3(pixel, 0));
    float3 surfacePoint = WorldPosition(uv, depth);
    float3 normal = DecodeNormal(vis.y);
    float distance = length(surfacePoint - cameraPosition);
    float facing = dot(normal, (cameraPosition - surfacePoint) / max(distance, 1e-4));
    float fade = (1.0 - smoothstep(fadeStart, fadeEnd, distance)) *
                 smoothstep(0.08, 0.25, facing) * EdgeFade(uv);
    if (fade * strength < 1e-5) return 0.0;

    float3 dx = PlaneNeighbour(uv + float2(1.0 / screenSize.x, 0), surfacePoint, normal) - surfacePoint;
    float3 dy = PlaneNeighbour(uv + float2(0, 1.0 / screenSize.y), surfacePoint, normal) - surfacePoint;
    float3 projections = TerrainProjectionWeights(normal);
    float4 weights = TerrainLayerWeights(surfacePoint, normal);
    const float scales[4] = { 0.16667, 0.4831, 0.07500, 0.4130 };
    float4 heights = 128.0 / 255.0;
    float4 blendHeights = heights;
    [unroll] for (uint layer = 0; layer < 4; ++layer) {
        if (weights[layer] <= 0.002) continue;
        TriplanarGrads grads;
        grads.zyDx = dx.zy * scales[layer]; grads.zyDy = dy.zy * scales[layer];
        grads.xzDx = dx.xz * scales[layer]; grads.xzDy = dy.xz * scales[layer];
        grads.xyDx = dx.xy * scales[layer]; grads.xyDy = dy.xy * scales[layer];
        float sampled = SampleTerrainArray(metalRoughMap, surfacePoint, projections,
                                             layer, scales[layer], grads).a;
        float blend = TerrainPOMBlendHeight(sampled, layer, neutralHeightMask, projections);
        if (layer == 0) { heights.x = sampled; blendHeights.x = blend; }
        else if (layer == 1) { heights.y = sampled; blendHeights.y = blend; }
        else if (layer == 2) { heights.z = sampled; blendHeights.z = blend; }
        else { heights.w = sampled; blendHeights.w = blend; }
    }
    weights = TerrainHeightBlend(weights, blendHeights);
    const float4 depths = float4(0.035, 0.025, 0.050, 0.080);
    float relief = dot((heights - 128.0 / 255.0) * depths, weights) * strength * fade;
    return float2(relief, 0.080 * strength * fade);
}

bool SurfaceAt(float2 uv, out float3 surfacePoint, out float3 normal, out float2 height) {
    surfacePoint = 0.0; normal = float3(0, 1, 0); height = 0.0;
    if (any(uv < 0.5 / screenSize) || any(uv > 1.0 - 0.5 / screenSize)) return false;
    uint2 pixel = (uint2)(uv * screenSize);
    uint2 vis = sourceVisibility.Load(int3(pixel, 0));
    if (!IsTerrainVisibilityID(vis.x)) return false;
    float2 centre = (float2(pixel) + 0.5) / screenSize;
    surfacePoint = WorldPosition(centre, sourceDepth.Load(int3(pixel, 0)));
    normal = DecodeNormal(vis.y);
    // Do not linearly filter depth across terrain/mesh or terrain/sky edges.
    // The tangent plane supplies subpixel geometry without those false ramps.
    float3 planePoint = PlaneNeighbour(uv, surfacePoint, normal);
    if (length(planePoint - surfacePoint) > maxRayTravel) return false;
    surfacePoint = planePoint;
    // Filtering zero-height sky into the last terrain texel shrinks the relief
    // precisely where it should extend the silhouette. Filter only a complete
    // terrain footprint; edge pixels retain their own height.
    int2 lower = (int2)floor(uv * screenSize - 0.5);
    int2 limit = (int2)screenSize - 1;
    bool complete = IsTerrainVisibilityID(sourceVisibility.Load(int3(clamp(lower, 0, limit), 0)).x) &&
        IsTerrainVisibilityID(sourceVisibility.Load(int3(clamp(lower + int2(1, 0), 0, limit), 0)).x) &&
        IsTerrainVisibilityID(sourceVisibility.Load(int3(clamp(lower + int2(0, 1), 0, limit), 0)).x) &&
        IsTerrainVisibilityID(sourceVisibility.Load(int3(clamp(lower + int2(1, 1), 0, limit), 0)).x);
    height = complete ? sourceHeight.SampleLevel(screenSampler, uv, 0)
                      : sourceHeight.Load(int3(pixel, 0));
    return true;
}

// Pull the ray surfacePoint back to the base surface along its geometric normal.
// Reprojection moves the lookup to another pixel, permitting silhouette growth
// instead of restricting relief to the original rasterized triangle coverage.
bool RayGap(float3 rayPoint, float2 seedUV, out float gap,
            out uint packedNormal, out float2 hitUV) {
    float3 surfacePoint = 0.0, normal = float3(0, 1, 0);
    float2 height = 0.0;
    gap = 0.0; packedNormal = 0u; hitUV = seedUV;
    [loop] for (uint iteration = 0; iteration < 3; ++iteration) {
        if (!SurfaceAt(hitUV, surfacePoint, normal, height)) return false;
        float separation = dot(rayPoint - surfacePoint, normal);
        if (!Project(rayPoint - normal * separation, hitUV)) return false;
    }
    if (!SurfaceAt(hitUV, surfacePoint, normal, height)) return false;
    float separation = dot(rayPoint - surfacePoint, normal);
    float3 basePoint = rayPoint - normal * separation;
    // A jump between unrelated slopes is not a heightfield intersection.
    if (length(basePoint - surfacePoint) > maxRayTravel || height.y < 1e-5) return false;
    gap = separation - height.x;
    packedNormal = sourceVisibility.Load(int3((uint2)(hitUV * screenSize), 0)).y;
    return true;
}

struct DisplacedPixel {
    uint2 visibility : SV_Target0;
    float depth : SV_Depth;
};

DisplacedPixel DisplacePS(float4 position : SV_Position) {
    uint2 pixel = (uint2)position.xy;
    DisplacedPixel result;
    result.visibility = sourceVisibility.Load(int3(pixel, 0));
    result.depth = sourceDepth.Load(int3(pixel, 0));
    float2 uv = position.xy / screenSize;
    if (EdgeFade(uv) <= 0.001 || strength <= 0.0) return result;
    float2 seedUV = uv;
    float3 seedPoint = 0.0, seedNormal = float3(0, 1, 0);
    float2 seedHeight = 0.0;
    bool seeded = SurfaceAt(seedUV, seedPoint, seedNormal, seedHeight);
    if (!seeded) {
        // Bound silhouette extension to eight pixels. This also keeps a nearby
        // foreground mesh from causing an unbounded search behind it.
        const float2 directions[8] = { float2(1,0), float2(-1,0), float2(0,1),
            float2(0,-1), float2(1,1), float2(-1,1), float2(1,-1), float2(-1,-1) };
        [loop] for (uint radius = 1; radius <= 8 && !seeded; radius *= 2) {
            [loop] for (uint direction = 0; direction < 8; ++direction) {
                float2 candidate = uv + directions[direction] * radius / screenSize;
                if (SurfaceAt(candidate, seedPoint, seedNormal, seedHeight) &&
                    seedHeight.y > 1e-5) {
                    seedUV = candidate; seeded = true; break;
                }
            }
        }
    }
    if (!seeded || seedHeight.y < 1e-5) return result;
    float3 ray = normalize(WorldPosition(uv, 0.5) - cameraPosition);
    float incidence = -dot(ray, seedNormal);
    if (incidence <= 0.08) return result;
    float baseTravel = dot(cameraPosition - seedPoint, seedNormal) / incidence;
    float span = min(seedHeight.y / incidence + 0.002, maxRayTravel);
    if (baseTravel <= span) return result;
    float front = baseTravel - span;
    float back = baseTravel + span;
    float lastT = front, lastGap = 0.0;
    bool lastValid = false;
    uint steps = clamp(marchSteps, 8u, 48u);
    [loop] for (uint step = 0; step <= steps; ++step) {
        float t = lerp(front, back, float(step) / steps);
        float gap; uint packed; float2 sourceUV;
        bool valid = RayGap(cameraPosition + ray * t, seedUV, gap, packed, sourceUV);
        if (valid && lastValid && lastGap > 0.0 && gap <= 0.0) {
            float low = lastT, high = t;
            uint hitNormal = packed;
            [loop] for (uint refine = 0; refine < 5; ++refine) {
                float mid = (low + high) * 0.5;
                float midGap; uint midNormal; float2 midUV;
                if (!RayGap(cameraPosition + ray * mid, sourceUV,
                            midGap, midNormal, midUV)) return result;
                if (midGap > 0.0) low = mid;
                else { high = mid; hitNormal = midNormal; }
            }
            float4 clip = mul(float4(cameraPosition + ray * ((low + high) * 0.5), 1),
                              viewProjection);
            float depth = clip.z / clip.w;
            // Other geometry keeps its original depth test. Recessed terrain
            // can move backwards only where terrain originally owned the pixel.
            if (depth > 0.0 && depth < 1.0 &&
                (IsTerrainVisibilityID(result.visibility.x) || depth < result.depth)) {
                result.visibility = uint2(VB_TERRAIN_ID, hitNormal);
                result.depth = depth;
            }
            return result;
        }
        lastT = t; lastGap = gap; lastValid = valid;
    }
    return result;
}
