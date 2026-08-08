// Visibility Buffer Resolve - Compute Shader
// Reads the visibility buffer (R32_UINT) and depth buffer,
// reconstructs per-pixel surface attributes from structured buffers,
// then applies PBR shading identical to the forward clustered shader.

// ---- Constant Buffers ----

cbuffer FrameConstants : register(b0) {
    matrix viewMatrix;
    matrix projMatrix;
    matrix invViewProj;
    matrix shadowCascadeMatrices[3];
    matrix previousViewProj;
    float4 shadowCascadeSplits;
    float3 cameraPos;
    float  screenWidth;
    float  screenHeight;
    float  nearPlane;
    float  farPlane;
    uint   debugViewMode;
    uint   enableMotionVectors;
    uint   edgeAAEnabled;
    uint2  framePadding;
    float4 palmWind;
    float4 palmPrimary;
    float4 palmSecondary;
    float4 palmPreviousPrimary;
    float4 palmPreviousSecondary;
    float4 palmParams;
};

#include "palm_wind.hlsli"
#include "foliage_brdf.hlsli"

cbuffer LightBuffer : register(b1) {
    float3 lightPos;
    int    lightType;
    float3 lightColor;
    float  attConstant;
    float  attLinear;
    float  attQuadratic;
    float  ambientStrength;
    float  specularStrength;
    int    shininess;
    float  shadowBias;
    int    enableShadows;
    float  lbPadding;
    float  ambientLightingIntensity;
};

struct PointLightData {
    float3 position;
    float  radius;
    float3 color;
    float  intensity;
};

cbuffer PointLightsBuffer : register(b2) {
    int            numPointLights;
    float          plPad1;
    float          plPad2;
    float          plPad3;
    PointLightData pointLights[64];
};

// ---- Per-draw-call data ----

struct DrawCallData {
    float4x4 modelMatrix;
    float4x4 previousModelMatrix;
    float3   objectColor;
    float    useTexture;
    float    metalness;
    float    roughness;
    float    useNormalMap;
    uint     materialID;
    uint     vertexOffset;  // offset into global vertex buffer
    uint     indexOffset;   // offset into global index buffer
    uint     indexCount;
    uint     hasIndices;    // 1 if indexed, 0 if non-indexed
    uint     flags;         // bit 0 double-sided, bit 1 alpha cutout, bit 2 luminance cutout
    float4   palmWindRoot;
};

// ---- Vertex data stored as float4 pairs ----
// Each vertex = 2 float4s:
//   float4(pos.x, pos.y, pos.z, norm.x)
//   float4(norm.y, norm.z, uv.x, uv.y)

struct PackedVertex {
    float4 d0; // pos.xyz, normal.x
    float4 d1; // normal.yz, uv.xy
};

// ---- Resources ----

Texture2D<uint2>  visBuffer    : register(t0);
Texture2D<float>  depthBuffer  : register(t1);
Texture2DArray<float> shadowMapTex : register(t2);

StructuredBuffer<DrawCallData> drawCalls : register(t3);
StructuredBuffer<PackedVertex> vertices  : register(t4);
StructuredBuffer<uint>         indices   : register(t5);

struct ClusterData {
    uint lightCount;
    uint lightIndices[32];
    uint3 padding;
};

cbuffer SkySHBuffer : register(b3) {
    float4 shCoeffs[9];
    float skyIntensity;
    float3 shPadding;
};

cbuffer DDGIBuffer : register(b4) {
    float3 probeGridOrigin;
    float probeSpacing;
    int probeCountX;
    int probeCountY;
    int probeCountZ;
    float maxRayDistance;
    float normalBias;
    float viewBias;
    float irradianceGamma;
    float giIntensity;
    int irradianceTexWidth;
    int irradianceTexHeight;
    int visibilityTexWidth;
    int visibilityTexHeight;
    int ddgiEnabled;
    int sparseProbeCount;
    int sparseCellCount;
    float sparseCellSize;
};
StructuredBuffer<ClusterData>   clusters  : register(t6);

struct MaterialData {
    float4 baseColorFactor;
    float4 emissiveOcclusion;
    float4 pbrParams;
    float4 shadingParams;
    uint4 textureIndices;
};
StructuredBuffer<MaterialData> materials : register(t7);
Texture2D<float4> materialTextures[64] : register(t8);
Texture2D<float4> environmentMap : register(t72);
Texture2D<float2> brdfIntegrationLUT : register(t73);
Texture2D<float4> ddgiIrradianceMap : register(t74);
Texture2D<float2> ddgiVisibilityMap : register(t75);
struct SparseProbeData {
    float3 position; float radius;
    float3 normal; uint state;
    uint2 stableId; uint lastUpdatedFrame; uint padding;
};
struct SparseProbeCell {
    int3 coordinate; uint offset;
    uint count; uint3 padding;
};
StructuredBuffer<SparseProbeData> sparseProbes : register(t76);
StructuredBuffer<SparseProbeCell> sparseProbeCells : register(t77);
StructuredBuffer<uint> sparseProbeIndices : register(t78);

RWTexture2D<float4> outputColor : register(u0);
RWTexture2D<float2> outputMotion : register(u1);
RWTexture2D<float4> outputNormalRoughness : register(u2);

#if SGE_ENHANCED_VISUALS
// Static-geometry TLAS, shared with the probe GI path. Bound only for the
// enhanced variant; the default resolve never references t79 and its root
// signature does not declare it.
RaytracingAccelerationStructure sceneTLAS : register(t79);

cbuffer EnhancedVisualsBuffer : register(b5) {
    uint  enhancedRTShadows;        // master switch for the RT shadow term
    uint  enhancedRayClassify;      // spend rays only on low-confidence pixels
    float enhancedShadowRayLength;  // TMax for shadow rays, world units
    float enhancedConfidenceThreshold;
};

// Per-pixel record of where the rays went, for the debug view and the
// ray-fraction readback. One uint per pixel: 0 = cheap tier resolved it,
// 1 = classified as needing RT and traced.
RWTexture2D<uint> outputRayMask : register(u3);
#endif

SamplerState              texSampler    : register(s0);
SamplerComparisonState    shadowSampler : register(s1);

// ---- Helpers ----

// Smooth 3D value noise from integer avalanche hashing (same mixer as
// terrain_ms.hlsl's hash21). A plane-wave sin() here striped every large flat
// dielectric surface -- most visibly the terrain -- with diagonal roughness
// bands ~6 m apart. Identical to clustered_dx12_ps.hlsl.
uint MatVarHashUint(uint value) {
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return value;
}

float MatVarHash(int3 cell) {
    uint value = MatVarHashUint(asuint(cell.x) ^
        MatVarHashUint(asuint(cell.y) + MatVarHashUint(asuint(cell.z))) +
        0x9e3779b9u);
    return float(value & 0x00ffffffu) * (1.0 / 16777216.0);
}

float MatVarNoise(float3 position) {
    int3 cell = (int3)floor(position);
    float3 blend = frac(position);
    blend = blend * blend * (3.0 - 2.0 * blend);
    float n00 = lerp(MatVarHash(cell), MatVarHash(cell + int3(1, 0, 0)), blend.x);
    float n10 = lerp(MatVarHash(cell + int3(0, 1, 0)),
                     MatVarHash(cell + int3(1, 1, 0)), blend.x);
    float n01 = lerp(MatVarHash(cell + int3(0, 0, 1)),
                     MatVarHash(cell + int3(1, 0, 1)), blend.x);
    float n11 = lerp(MatVarHash(cell + int3(0, 1, 1)),
                     MatVarHash(cell + int3(1, 1, 1)), blend.x);
    return lerp(lerp(n00, n10, blend.y), lerp(n01, n11, blend.y), blend.z);
}

float3 ReconstructWorldPosOffset(uint2 pixel, float2 offset, float depth) {
    float2 uv = (float2(pixel) + offset) / float2(screenWidth, screenHeight);
    float2 ndc = uv * 2.0 - 1.0;
    ndc.y = -ndc.y;
    
    float4 clipPos = float4(ndc, depth, 1.0);
    float4 worldPos = mul(clipPos, invViewProj);
    return worldPos.xyz / worldPos.w;
}

float3 ReconstructWorldPos(uint2 pixel, float depth) {
    return ReconstructWorldPosOffset(pixel, float2(0.5, 0.5), depth);
}

float3 SampleSkyIrradiance(float3 normal) {
    float3 result = shCoeffs[0].rgb * 0.282095;
    result += shCoeffs[1].rgb * 0.488603 * normal.y;
    result += shCoeffs[2].rgb * 0.488603 * normal.z;
    result += shCoeffs[3].rgb * 0.488603 * normal.x;
    result += shCoeffs[4].rgb * 1.092548 * normal.x * normal.y;
    result += shCoeffs[5].rgb * 1.092548 * normal.y * normal.z;
    result += shCoeffs[6].rgb * 0.315392 * (3.0 * normal.z * normal.z - 1.0);
    result += shCoeffs[7].rgb * 1.092548 * normal.x * normal.z;
    result += shCoeffs[8].rgb * 0.546274 * (normal.x * normal.x - normal.y * normal.y);
    return max(result, 0.0) * skyIntensity;
}

float3 SampleReflectionProbe(float3 reflectionDir, float roughness) {
    reflectionDir = normalize(reflectionDir);
    float2 uv = float2(atan2(reflectionDir.z, reflectionDir.x) * 0.159154943 + 0.5,
                       acos(clamp(reflectionDir.y, -1.0, 1.0)) * 0.318309886);
    uint width, height, mipCount;
    environmentMap.GetDimensions(0, width, height, mipCount);
    float lod = roughness * roughness * max((float)mipCount - 1.0, 0.0);
    return environmentMap.SampleLevel(texSampler, uv, lod).rgb * skyIntensity;
}

float2 DDGIOctEncode(float3 direction) {
    direction /= max(abs(direction.x) + abs(direction.y) +
                     abs(direction.z), 1e-5);
    if (direction.y < 0.0) {
        float2 signDirection = float2(direction.x >= 0.0 ? 1.0 : -1.0,
                                      direction.z >= 0.0 ? 1.0 : -1.0);
        direction.xz = (1.0 - abs(direction.zx)) * signDirection;
    }
    return direction.xz * 0.5 + 0.5;
}

float3 SampleDDGIProbe(int probeIndex, float3 normal) {
    int totalProbes = sparseProbeCount > 0 ? sparseProbeCount :
                      probeCountX * probeCountY * probeCountZ;
    int atlasProbeWidth = sparseProbeCount > 0
        ? max((int)ceil(sqrt((float)totalProbes)), 1)
        : max((int)sqrt((float)totalProbes), 1);
    int probeX = probeIndex % atlasProbeWidth;
    int probeY = probeIndex / atlasProbeWidth;
    int tileWidth = irradianceTexWidth + 2;
    int tileHeight = irradianceTexHeight + 2;
    int atlasHeight = (totalProbes + atlasProbeWidth - 1) / atlasProbeWidth;
    float2 octUV = DDGIOctEncode(normalize(normal));
    float2 atlasUV = float2(
        (probeX * tileWidth + 1.5 +
         octUV.x * (irradianceTexWidth - 1)) /
            (atlasProbeWidth * tileWidth),
        (probeY * tileHeight + 1.5 +
         octUV.y * (irradianceTexHeight - 1)) /
            (atlasHeight * tileHeight));
    return ddgiIrradianceMap.SampleLevel(texSampler, atlasUV, 0.0).rgb;
}

uint DDGIHash(uint value) {
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    return value ^ (value >> 16);
}

uint DDGICellHash(int3 coordinate) {
    uint hash = DDGIHash(asuint(coordinate.x));
    hash ^= DDGIHash(asuint(coordinate.y) + 0x9e3779b9u);
    hash ^= DDGIHash(asuint(coordinate.z) + 0x85ebca6bu);
    return DDGIHash(hash);
}

float SparseProbeVisibility(uint probeIndex, float3 delta, float distance) {
    int atlasColumns = max((int)ceil(sqrt((float)sparseProbeCount)), 1);
    int probeX = probeIndex % atlasColumns;
    int probeY = probeIndex / atlasColumns;
    uint atlasWidth, atlasHeight;
    ddgiVisibilityMap.GetDimensions(atlasWidth, atlasHeight);
    float2 octUV = DDGIOctEncode(normalize(delta));
    float2 uv = float2(
        (probeX * (visibilityTexWidth + 2) + 1.5 +
         octUV.x * (visibilityTexWidth - 1)) / atlasWidth,
        (probeY * (visibilityTexHeight + 2) + 1.5 +
         octUV.y * (visibilityTexHeight - 1)) / atlasHeight);
    float2 moments = ddgiVisibilityMap.SampleLevel(texSampler, uv, 0.0).rg;
    if (moments.y <= 1e-5 || distance <= moments.x) return 1.0;
    float variance = max(moments.y - moments.x * moments.x, 0.001);
    float difference = distance - moments.x;
    return saturate(variance / (variance + difference * difference));
}

float3 SampleSparseDDGI(float3 worldPos, float3 normal) {
    float3 biased = worldPos + normal * normalBias;
    int3 center = (int3)floor(biased / sparseCellSize);
    uint nearestIndices[8];
    float nearestDistanceSq[8];
    uint nearestCount = 0;

    [loop] for (int z = -1; z <= 1; ++z)
    [loop] for (int y = -1; y <= 1; ++y)
    [loop] for (int x = -1; x <= 1; ++x) {
        int3 coordinate = center + int3(x, y, z);
        uint slot = DDGICellHash(coordinate) & (sparseCellCount - 1);
        [loop] for (uint search = 0;
                    search < (uint)sparseCellCount; ++search) {
            SparseProbeCell cell = sparseProbeCells[slot];
            if (cell.count == 0) break;
            if (all(cell.coordinate == coordinate)) {
                [loop] for (uint i = 0; i < cell.count; ++i) {
                    uint index = sparseProbeIndices[cell.offset + i];
                    SparseProbeData probe = sparseProbes[index];
                    if (probe.state == 2) continue;
                    float distanceSq =
                        dot(biased - probe.position, biased - probe.position);
                    uint insertAt = nearestCount;
                    if (nearestCount < 8) {
                        ++nearestCount;
                    } else {
                        if (distanceSq >= nearestDistanceSq[7]) continue;
                        insertAt = 7;
                    }
                    [loop] while (insertAt > 0 &&
                                  distanceSq < nearestDistanceSq[insertAt - 1]) {
                        nearestDistanceSq[insertAt] =
                            nearestDistanceSq[insertAt - 1];
                        nearestIndices[insertAt] = nearestIndices[insertAt - 1];
                        --insertAt;
                    }
                    nearestDistanceSq[insertAt] = distanceSq;
                    nearestIndices[insertAt] = index;
                }
                break;
            }
            slot = (slot + 1) & (sparseCellCount - 1);
        }
    }

    float3 irradiance = 0.0;
    float totalWeight = 0.0;
    [loop] for (uint i = 0; i < nearestCount; ++i) {
        uint index = nearestIndices[i];
        SparseProbeData probe = sparseProbes[index];
        float3 delta = biased - probe.position;
        float distance = sqrt(nearestDistanceSq[i]);
        float alignment = saturate(dot(normal, probe.normal));
        float normalWeight = 0.04 + 0.96 * alignment * alignment;
        float rangeWeight = saturate(
            1.0 - distance / max(sparseCellSize * 1.75, 0.1));
        rangeWeight *= rangeWeight;
        float weight = normalWeight * rangeWeight *
            SparseProbeVisibility(index, delta, distance) /
            max(nearestDistanceSq[i], 0.09);
        irradiance += SampleDDGIProbe(index, normal) * weight;
        totalWeight += weight;
    }
    return totalWeight > 1e-5
        ? irradiance / totalWeight * giIntensity : 0.0;
}

float3 SampleDDGIIrradiance(float3 worldPos, float3 normal) {
    if (ddgiEnabled == 0) return 0.0;
    if (sparseProbeCount > 0 && sparseCellCount > 0)
        return SampleSparseDDGI(worldPos, normal);
    float3 gridPosition =
        (worldPos + normal * normalBias - probeGridOrigin) / probeSpacing;
    if (any(gridPosition < 0.0) ||
        gridPosition.x > probeCountX - 1 ||
        gridPosition.y > probeCountY - 1 ||
        gridPosition.z > probeCountZ - 1)
        return 0.0;
    int3 base = clamp((int3)floor(gridPosition), 0,
                      int3(probeCountX - 2, probeCountY - 2,
                           probeCountZ - 2));
    float3 fraction = saturate(gridPosition - base);
    float3 irradiance = 0.0;
    float totalWeight = 0.0;
    [unroll]
    for (int z = 0; z < 2; ++z) {
        [unroll]
        for (int y = 0; y < 2; ++y) {
            [unroll]
            for (int x = 0; x < 2; ++x) {
                int3 coordinate = base + int3(x, y, z);
                float3 cornerWeight = lerp(1.0 - fraction, fraction,
                                           float3(x, y, z));
                float weight = cornerWeight.x * cornerWeight.y * cornerWeight.z;
                int index = coordinate.x + coordinate.y * probeCountX +
                            coordinate.z * probeCountX * probeCountY;
                irradiance += SampleDDGIProbe(index, normal) * weight;
                totalWeight += weight;
            }
        }
    }
    irradiance /= max(totalWeight, 1e-4);
    irradiance = pow(max(irradiance, 0.0),
                     1.0 / max(irradianceGamma, 0.1));
    return irradiance * giIntensity;
}

float3 RenderProceduralSky(uint2 pixel) {
    float2 uv = (float2(pixel) + 0.5) / float2(screenWidth, screenHeight);
    float2 ndc = uv * 2.0 - 1.0;
    ndc.y = -ndc.y;
    float4 farWorld = mul(float4(ndc, 1.0, 1.0), invViewProj);
    float3 direction = normalize(farWorld.xyz / farWorld.w - cameraPos);
    float up = saturate(direction.y * 0.5 + 0.5);
    float horizon = exp(-abs(direction.y) * 6.0);
    float3 zenith = float3(0.18, 0.42, 0.82) * max(skyIntensity, 0.5);
    float3 horizonColor = float3(0.80, 0.68, 0.50) * max(skyIntensity, 0.5);
    float3 ground = float3(0.035, 0.055, 0.08);
    float3 sky = lerp(ground, lerp(horizonColor, zenith, smoothstep(0.48, 1.0, up)), up);
    sky = lerp(sky, horizonColor, horizon * 0.35);
    float sun = pow(saturate(dot(direction, normalize(lightPos))), 1200.0);
    float halo = pow(saturate(dot(direction, normalize(lightPos))), 24.0);
    return sky + lightColor * (sun * 18.0 + halo * 0.22);
}

#if SGE_ENHANCED_VISUALS
// Inline ray-traced sun shadow. Only compiled into the SM6.5 variant of this
// shader -- FXC cannot compile RayQuery at any profile, so the default build
// never sees this code.
//
// Returns sun visibility in 0..1 against the static TLAS.
//
// A miss is deliberately NOT treated as proof the pixel is lit: the TLAS holds
// static geometry only, so a dynamic actor could still be casting here. The
// caller therefore blends this against the cascade term rather than replacing
// it -- see the call site, where `min()` keeps whichever occluder either source
// found.
float RayTracedShadow(float3 worldPos, float3 normal, float3 lightDir) {
    if (enhancedRTShadows == 0) return 1.0;

    RayDesc ray;
    // Offset along the normal to avoid self-intersection at the origin. Scaled
    // by grazing angle for the same reason the cascade path slope-scales bias.
    float grazing = 1.0 - saturate(dot(normal, lightDir));
    ray.Origin = worldPos + normal * (0.02 + 0.10 * grazing);
    ray.Direction = lightDir;
    ray.TMin = 0.0;
    ray.TMax = enhancedShadowRayLength;

    RayQuery<RAY_FLAG_ACCEPT_FIRST_HIT_AND_END_SEARCH |
             RAY_FLAG_SKIP_CLOSEST_HIT_SHADER |
             RAY_FLAG_CULL_NON_OPAQUE> query;
    query.TraceRayInline(sceneTLAS, RAY_FLAG_NONE, 0xff, ray);
    query.Proceed();

    // Occluded by static geometry, or nothing in the way as far as the TLAS
    // knows. Either way the caller decides how much to trust it.
    return query.CommittedStatus() == COMMITTED_TRIANGLE_HIT ? 0.0 : 1.0;
}
#endif

float CalculateShadow(float3 worldPos, float3 normal, float3 lightDir) {
    if (enableShadows == 0) return 1.0;

    float viewDepth = mul(float4(worldPos, 1.0), viewMatrix).z;
    uint cascade = viewDepth < shadowCascadeSplits.x ? 0u :
                   (viewDepth < shadowCascadeSplits.y ? 1u : 2u);
    float4 lightClip = mul(float4(worldPos, 1.0),
                           shadowCascadeMatrices[cascade]);
    if (lightClip.w <= 0.0) return 1.0;
    float3 projected = lightClip.xyz / lightClip.w;
    float2 uv = projected.xy * float2(0.5, -0.5) + 0.5;
    if (projected.z <= 0.0 || projected.z >= 1.0 ||
        any(uv < 0.0) || any(uv > 1.0)) return 1.0;

    uint shadowWidth, shadowHeight, shadowLayers;
    shadowMapTex.GetDimensions(shadowWidth, shadowHeight, shadowLayers);
    float2 texel = rcp(float2(shadowWidth, shadowHeight));
    float slopeBias = max(shadowBias * (1.0 - saturate(dot(normal, lightDir))),
                          shadowBias * 0.25);
    float visibility = 0.0;
    [unroll]
    for (int y = -1; y <= 1; ++y) {
        [unroll]
        for (int x = -1; x <= 1; ++x) {
            visibility += shadowMapTex.SampleCmpLevelZero(
                shadowSampler, float3(uv + float2(x, y) * texel, cascade),
                projected.z - slopeBias);
        }
    }
    return visibility / 9.0;
}

void GetTriangleVertices(DrawCallData dc, uint triangleID,
                         out float3 p0, out float3 p1, out float3 p2,
                         out float3 n0, out float3 n1, out float3 n2,
                         out float2 uv0, out float2 uv1, out float2 uv2) {
    uint i0, i1, i2;
    
    if (dc.hasIndices) {
        i0 = indices[dc.indexOffset + triangleID * 3 + 0];
        i1 = indices[dc.indexOffset + triangleID * 3 + 1];
        i2 = indices[dc.indexOffset + triangleID * 3 + 2];
    } else {
        i0 = triangleID * 3 + 0;
        i1 = triangleID * 3 + 1;
        i2 = triangleID * 3 + 2;
    }
    
    PackedVertex v0 = vertices[dc.vertexOffset + i0];
    PackedVertex v1 = vertices[dc.vertexOffset + i1];
    PackedVertex v2 = vertices[dc.vertexOffset + i2];
    
    p0  = v0.d0.xyz; p1  = v1.d0.xyz; p2  = v2.d0.xyz;
    n0  = float3(v0.d0.w, v0.d1.xy);
    n1  = float3(v1.d0.w, v1.d1.xy);
    n2  = float3(v2.d0.w, v2.d1.xy);
    uv0 = v0.d1.zw; uv1 = v1.d1.zw; uv2 = v2.d1.zw;
}

// Compute barycentric coordinates for point p in screen-space triangle (p0,p1,p2)
// given their world-space positions and the model+viewProj matrices.
float3 ComputeBarycentrics(float3 worldPos, float3 wp0, float3 wp1, float3 wp2) {
    // Use the triangle edge vectors in world space
    float3 v0 = wp1 - wp0;
    float3 v1 = wp2 - wp0;
    float3 v2 = worldPos - wp0;
    
    float d00 = dot(v0, v0);
    float d01 = dot(v0, v1);
    float d11 = dot(v1, v1);
    float d20 = dot(v2, v0);
    float d21 = dot(v2, v1);
    
    float denom = d00 * d11 - d01 * d01;
    
    if (abs(denom) < 1e-10) {
        return float3(1.0/3.0, 1.0/3.0, 1.0/3.0);
    }
    
    float v = (d11 * d20 - d01 * d21) / denom;
    float w = (d00 * d21 - d01 * d20) / denom;
    float u = 1.0 - v - w;
    
    return float3(u, v, w);
}

void ComputeUVGradients(float3 wp0, float3 wp1, float3 wp2,
                        float2 uv0, float2 uv1, float2 uv2,
                        out float2 uvDx, out float2 uvDy) {
    float4 c0 = mul(mul(float4(wp0, 1.0), viewMatrix), projMatrix);
    float4 c1 = mul(mul(float4(wp1, 1.0), viewMatrix), projMatrix);
    float4 c2 = mul(mul(float4(wp2, 1.0), viewMatrix), projMatrix);
    float2 s0 = (c0.xy / max(c0.w, 1e-6) * float2(0.5, -0.5) + 0.5)
              * float2(screenWidth, screenHeight);
    float2 s1 = (c1.xy / max(c1.w, 1e-6) * float2(0.5, -0.5) + 0.5)
              * float2(screenWidth, screenHeight);
    float2 s2 = (c2.xy / max(c2.w, 1e-6) * float2(0.5, -0.5) + 0.5)
              * float2(screenWidth, screenHeight);
    float2 e0 = s1 - s0;
    float2 e1 = s2 - s0;
    float determinant = e0.x * e1.y - e0.y * e1.x;
    if (abs(determinant) < 1e-6) {
        uvDx = 0.0;
        uvDy = 0.0;
        return;
    }
    float invDet = rcp(determinant);
    float3 baryDx = float3((e0.y - e1.y) * invDet,
                           e1.y * invDet, -e0.y * invDet);
    float3 baryDy = float3((e1.x - e0.x) * invDet,
                          -e1.x * invDet, e0.x * invDet);
    uvDx = baryDx.x * uv0 + baryDx.y * uv1 + baryDx.z * uv2;
    uvDy = baryDy.x * uv0 + baryDy.y * uv1 + baryDy.z * uv2;
}

// ---- Point Light Calculation (matching forward shader) ----

float3 calculatePointLight(int index, float3 fragPos, float3 normal,
                           float3 viewDir, float rough) {
    float3 lightPosition = pointLights[index].position;
    float lightRadius = pointLights[index].radius;
    float3 lightDir = normalize(lightPosition - fragPos);
    float distance = length(lightPosition - fragPos);
    
    if (distance > lightRadius) return 0.0;
    
    float attenuation = 1.0 / (1.0 + (4.5 / lightRadius) * distance +
        (75.0 / (lightRadius * lightRadius)) * distance * distance);
    attenuation *= 1.0 - smoothstep(lightRadius * 0.75, lightRadius, distance);
    
    float diff = max(dot(normal, lightDir), 0.0);
    float3 radiance = pointLights[index].color * pointLights[index].intensity;
    float3 halfDir = normalize(lightDir + viewDir);
    float pointShininess = lerp(8.0, 128.0, saturate(1.0 - rough));
    float spec = pow(max(dot(normal, halfDir), 0.0), pointShininess);
    float3 specular = specularStrength * saturate(1.0 - rough) * spec * radiance;
    return (diff * radiance + specular) * attenuation;
}

struct Surface {
    float3 fragPos;
    float3 normal;
    float3 viewDir;
    float3 albedo;
    float  metal;
    float  rough;
    float  materialAO;
    float  foliageCoverage;
    bool   isFoliage;
    MaterialData material;
    DrawCallData dc;
};

Surface EvaluateSurface(float3 fragPos,
                        DrawCallData dc, bool isFoliage,
                        float3 wp0, float3 wp1, float3 wp2,
                        float3 n0, float3 n1, float3 n2,
                        float2 uv0, float2 uv1, float2 uv2,
                        float3 bary) {
    float3 viewDir = normalize(cameraPos - fragPos);
    float3 objNormal = normalize(bary.x * n0 + bary.y * n1 + bary.z * n2);
    // Transform normal to world space
    float3 normal = normalize(mul(objNormal, (float3x3)dc.modelMatrix));
    if ((dc.flags & 1u) != 0u && dot(normal, viewDir) < 0.0)
        normal = -normal;

    float2 texCoord = bary.x * uv0 + bary.y * uv1 + bary.z * uv2;
    float2 uvDx, uvDy;
    ComputeUVGradients(wp0, wp1, wp2, uv0, uv1, uv2, uvDx, uvDy);

    // Material
    MaterialData material = materials[min(dc.materialID, 255u)];
    float3 albedo = dc.objectColor * material.baseColorFactor.rgb;
    float metal = material.pbrParams.w > 0.5 ? material.pbrParams.x : dc.metalness;
    float rough = material.pbrParams.w > 0.5 ? material.pbrParams.y : dc.roughness;
    float materialAO = 1.0;
    float foliageCoverage = 1.0;
    if (material.textureIndices.x < 64u) {
        uint albedoTextureIndex = material.textureIndices.x;
        float4 authoredSample = materialTextures[albedoTextureIndex].SampleGrad(
            texSampler, texCoord, uvDx, uvDy);
        if (isFoliage) {
            foliageCoverage = authoredSample.a;
            uint texWidth, texHeight, texLevels;
            materialTextures[albedoTextureIndex].GetDimensions(
                0, texWidth, texHeight, texLevels);
            float2 texel = 1.0 / max(float2(texWidth, texHeight), 1.0);
            float4 neighbor0 = materialTextures[albedoTextureIndex].SampleGrad(
                texSampler, texCoord + float2(texel.x, 0.0), uvDx, uvDy);
            float4 neighbor1 = materialTextures[albedoTextureIndex].SampleGrad(
                texSampler, texCoord - float2(texel.x, 0.0), uvDx, uvDy);
            float4 neighbor2 = materialTextures[albedoTextureIndex].SampleGrad(
                texSampler, texCoord + float2(0.0, texel.y), uvDx, uvDy);
            float4 neighbor3 = materialTextures[albedoTextureIndex].SampleGrad(
                texSampler, texCoord - float2(0.0, texel.y), uvDx, uvDy);
            float totalCoverage = authoredSample.a + neighbor0.a + neighbor1.a +
                                  neighbor2.a + neighbor3.a;
            float3 coveredColor =
                (authoredSample.rgb * authoredSample.a +
                 neighbor0.rgb * neighbor0.a +
                 neighbor1.rgb * neighbor1.a +
                 neighbor2.rgb * neighbor2.a +
                 neighbor3.rgb * neighbor3.a) /
                max(totalCoverage, 1e-4);
            float edgeBlend =
                1.0 - smoothstep(0.34, 0.88, authoredSample.a);
            authoredSample.rgb = lerp(
                authoredSample.rgb, coveredColor, edgeBlend * 0.88);
        }
        albedo *= pow(max(authoredSample.rgb, 0.0), 2.2);
        if (isFoliage) {
            float leafLum = dot(albedo, float3(0.299, 0.587, 0.114));
            float dark = 1.0 - smoothstep(0.02, 0.12, leafLum);
            // Keep alpha-card shadow recovery tied to the selected foliage
            // albedo; a fixed green lift made ferns ignore the grass controls.
            float3 foliageBase =
                dc.objectColor * material.baseColorFactor.rgb;
            albedo = lerp(albedo, foliageBase * 0.72, dark * 0.72);
        }
    }
    if (isFoliage) {
        float variation = MatVarNoise(fragPos * 1.7);
        float variationStrength = max(material.shadingParams.y, 0.0);
        float3 tint = lerp(
            1.0.xxx,
            lerp(float3(0.90, 1.03, 0.92),
                 float3(1.06, 1.00, 0.78), variation),
            variationStrength);
        float brightness = lerp(
            1.0, lerp(0.88, 1.10, variation), variationStrength);
        albedo = saturate(albedo * tint * brightness);
    }
    if (material.textureIndices.z < 64u) {
        float4 mr = materialTextures[material.textureIndices.z].SampleGrad(
            texSampler, texCoord, uvDx, uvDy);
        materialAO = lerp(1.0, mr.r, saturate(material.emissiveOcclusion.w));
        if (material.textureIndices.w != 0u) {
            rough = clamp(mr.g, 0.08, 1.0);
            materialAO = lerp(1.0, mr.r,
                saturate(material.emissiveOcclusion.w));
        } else {
            rough *= mr.g;
            metal *= mr.b;
        }
    }
    metal = saturate(metal);
    rough = clamp(rough, 0.04, 1.0);
    if (isFoliage) {
        metal = 0.0;
    }

    if (material.textureIndices.y < 64u) {
        float3 tangentNormal = materialTextures[material.textureIndices.y].SampleGrad(
            texSampler, texCoord, uvDx, uvDy).xyz * 2.0 - 1.0;
        tangentNormal.y *= material.pbrParams.z;
        float tangentLength = saturate(length(tangentNormal));
        tangentNormal.xy *= tangentLength;
        tangentNormal.z = sqrt(saturate(1.0 - dot(tangentNormal.xy,
                                                  tangentNormal.xy)));
        float3 edge1 = wp1 - wp0;
        float3 edge2 = wp2 - wp0;
        float2 deltaUV1 = uv1 - uv0;
        float2 deltaUV2 = uv2 - uv0;
        float uvDet = deltaUV1.x * deltaUV2.y - deltaUV1.y * deltaUV2.x;
        if (abs(uvDet) > 1e-6) {
            float3 tangent = normalize((edge1 * deltaUV2.y - edge2 * deltaUV1.y) / uvDet);
            tangent = normalize(tangent - normal * dot(normal, tangent));
            float3 bitangent = normalize(cross(normal, tangent));
            float3 mappedNormal = normalize(tangentNormal.x * tangent +
                                            tangentNormal.y * bitangent +
                                            tangentNormal.z * normal);
            float grazingFade = smoothstep(0.25, 0.65, saturate(abs(dot(
                normal, normalize(cameraPos - fragPos)))));
            normal = normalize(lerp(normal, mappedNormal,
                material.shadingParams.z * grazingFade));
        }
    }
    
    if ((dc.flags & 1u) != 0u && dot(normal, viewDir) < 0.0)
        normal = -normal;

    if (metal < 0.25) {
        float variation = MatVarNoise(fragPos * 0.35);
        rough = clamp(rough * lerp(0.88, 1.10, variation), 0.04, 1.0);
    }
    
    Surface surface;
    surface.fragPos = fragPos;
    surface.normal = normal;
    surface.viewDir = viewDir;
    surface.albedo = albedo;
    surface.metal = metal;
    surface.rough = rough;
    surface.materialAO = materialAO;
    surface.foliageCoverage = foliageCoverage;
    surface.isFoliage = isFoliage;
    surface.material = material;
    surface.dc = dc;
    return surface;
}

float3 ShadeSurface(uint2 pixel, Surface surface) {
    float3 result = 0.0;
    
    // Main light
    float3 L;
    float atten = 1.0;
    
    if (lightType == 0) {
        L = normalize(lightPos);
    } else {
        L = normalize(lightPos - surface.fragPos);
        float dist = length(lightPos - surface.fragPos);
        atten = 1.0 / (attConstant + attLinear * dist + attQuadratic * dist * dist);
    }
    
    float3 V = surface.viewDir;
    float3 H = normalize(V + L);
    float signedNdotL = dot(surface.normal, L);
    float NdotL = surface.isFoliage
        ? FoliageWrappedDiffuse(signedNdotL)
        : max(signedNdotL, 0.0);
    float NdotV = max(dot(surface.normal, V), 0.0);
    float NdotH = max(dot(surface.normal, H), 0.0);
    float HdotV = max(dot(H, V), 0.0);
    
    // Fresnel (Schlick)
    float3 F0 = float3(0.04, 0.04, 0.04);
    F0 = lerp(F0, surface.albedo, surface.metal);
    float3 F = F0 + (1.0 - F0) * pow(1.0 - HdotV, 5.0);
    
    // NDF (GGX)
    float alpha = surface.rough * surface.rough;
    float alpha2 = alpha * alpha;
    float NdotH2 = NdotH * NdotH;
    float num = alpha2;
    float denom = (NdotH2 * (alpha2 - 1.0) + 1.0);
    denom = 3.14159265 * denom * denom;
    float NDF = num / max(denom, 0.000001);
    
    // Geometry (Smith)
    float k = (surface.rough + 1.0) * (surface.rough + 1.0) / 8.0;
    float ggx1 = NdotV / (NdotV * (1.0 - k) + k);
    float ggx2 = NdotL / (NdotL * (1.0 - k) + k);
    float G = ggx1 * ggx2;
    
    float3 kS = F;
    float3 kD = float3(1.0, 1.0, 1.0) - kS;
    kD *= 1.0 - surface.metal;

    float ambientOcclusion = surface.materialAO;
    float3 diffuseAlbedo = surface.albedo * (1.0 - surface.metal);
    float ambientScale = surface.material.shadingParams.x;
    float3 diffuseIBL = SampleSkyIrradiance(surface.normal) * diffuseAlbedo * ambientScale;
    float3 diffuseGI = SampleDDGIIrradiance(surface.fragPos, surface.normal) *
        diffuseAlbedo * ambientScale;
    float3 reflectionIBL = SampleReflectionProbe(reflect(-V, surface.normal), surface.rough);
    float2 environmentBRDF = brdfIntegrationLUT.SampleLevel(
        texSampler, float2(NdotV, surface.rough), 0.0);
    float foliageSpecularScale = surface.isFoliage ? 0.12 : 1.0;
    float3 specularIBL = reflectionIBL *
        (F0 * environmentBRDF.x + environmentBRDF.y) *
        foliageSpecularScale;
    result += (diffuseIBL + diffuseGI + specularIBL) * ambientOcclusion *
              ambientLightingIntensity;
    result += ambientStrength * diffuseAlbedo * ambientScale *
              ambientOcclusion * ambientLightingIntensity;
    result += surface.material.emissiveOcclusion.rgb;
    
    float3 numerator = NDF * G * F;
    float denominator = 4.0 * NdotV * NdotL + 0.0001;
    float3 specular = numerator / denominator * foliageSpecularScale;
    
    float shadowVisibility = CalculateShadow(surface.fragPos, surface.normal, L);
#if SGE_ENHANCED_VISUALS
    // ---- Cheap tier first, rays only where it is uncertain ----
    //
    // The cascade lookup above is the cheap tier. It is confident in the middle
    // of a lit region and in the middle of a shadowed one; it is least reliable
    // in the penumbra, at grazing angles (where slope bias is doing the most
    // work), and past the last cascade split. Confidence here is exactly that:
    // distance from the 0/1 rails, damped at grazing incidence.
    //
    // Only low-confidence pixels get a ray, which is what keeps this affordable
    // -- on a typical frame it is a small fraction of the screen rather than
    // all of it.
    {
        float rail = min(shadowVisibility, 1.0 - shadowVisibility) * 2.0;
        float grazing = saturate(dot(surface.normal, L));
        float cascadeConfidence = saturate(rail + 0.35 * grazing);
        // Past the final split the cascades have nothing to say at all.
        float viewDepth = mul(float4(surface.fragPos, 1.0), viewMatrix).z;
        if (viewDepth >= shadowCascadeSplits.z) cascadeConfidence = 0.0;

        bool trace = enableShadows != 0 && enhancedRTShadows != 0 &&
                     (enhancedRayClassify == 0 ||
                      cascadeConfidence < enhancedConfidenceThreshold);
        if (trace) {
            float traced = RayTracedShadow(surface.fragPos, surface.normal, L);
            // Combine with min(): the TLAS is static-only, so the cascade term
            // still carries every dynamic caster, and taking the darker of the
            // two keeps those shadows rather than erasing them.
            float combined = min(shadowVisibility, traced);
            // Ease in near the threshold so the classification boundary is not
            // a visible edge, but commit fully below it. The earlier
            // 1 - confidence/threshold ramp discarded most of the traced result
            // on the very pixels it had just paid to trace -- at threshold 1.0
            // a confidence-0.9 pixel kept only 10% of its ray, which is why RT
            // shadows were barely visible while still costing full price.
            float blend = smoothstep(
                enhancedConfidenceThreshold,
                enhancedConfidenceThreshold * 0.6, cascadeConfidence);
            shadowVisibility = lerp(shadowVisibility, combined, blend);
            outputRayMask[pixel] = 1u;  // cleared to 0 at entry
        }
    }
#endif
    // Shadow map blocks direct sun only. IBL/DDGI are already low-frequency
    // indirect terms and must stay present on occluded building/actor sides.
    float frontFill = 0.65 + 0.35 * saturate(dot(surface.normal, surface.viewDir));
    result += diffuseAlbedo *
              (surface.isFoliage ? 0.0 : surface.material.shadingParams.y) * frontFill *
              ambientLightingIntensity;
    float3 Lo = (kD * surface.albedo / 3.14159265 + specular) * lightColor *
                NdotL * atten * shadowVisibility;
    result += Lo * (surface.isFoliage
        ? max(surface.material.emissiveOcclusion.w, 0.0) : 1.0);
    if (surface.isFoliage) {
        result += EvaluateFoliageTransmission(
            surface.albedo, surface.normal, surface.viewDir, L, lightColor,
            surface.foliageCoverage, atten, shadowVisibility) *
            max(surface.material.pbrParams.z, 0.0);
        result += EvaluateFoliageSkyScatter(
            surface.albedo, SampleSkyIrradiance(surface.normal),
            SampleSkyIrradiance(-surface.normal), ambientLightingIntensity);
    }
    
    // Clustered point lights: at most 32 relevant lights instead of all 64.
    uint clusterX = min((pixel.x * 16u) / max((uint)screenWidth, 1u), 15u);
    uint clusterY = min((pixel.y * 9u) / max((uint)screenHeight, 1u), 8u);
    float viewDepth = max(abs(mul(float4(surface.fragPos, 1.0), viewMatrix).z), nearPlane);
    float depthScale = log(viewDepth / nearPlane) / log(farPlane / nearPlane);
    uint clusterZ = min((uint)(saturate(depthScale) * 10.0), 9u);
    ClusterData cluster = clusters[clusterX + clusterY * 16u + clusterZ * 144u];
    [loop]
    for (uint listIndex = 0; listIndex < min(cluster.lightCount, 32u); ++listIndex) {
        uint lightIndex = cluster.lightIndices[listIndex];
        if (lightIndex < (uint)numPointLights && lightIndex < 64u)
            result += calculatePointLight(lightIndex, surface.fragPos, surface.normal,
                                          surface.viewDir, surface.rough) * surface.albedo;
    }
    
    return result;
}

// ---- Main ----

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID) {
    uint2 pixel = dispatchThreadID.xy;
    
    if (pixel.x >= (uint)screenWidth || pixel.y >= (uint)screenHeight) {
        return;
    }
    
    uint2 visValue = visBuffer.Load(int3(pixel, 0));

#if SGE_ENHANCED_VISUALS
    // Clear up front so every early-out below leaves a defined value. The mask
    // is read back for the ray-fraction statistic, and a stale value from a
    // previous frame would quietly inflate it.
    outputRayMask[pixel] = 0u;
#endif

    if (debugViewMode == 2u) {
        float rawDepth = depthBuffer.Load(int3(pixel, 0));
        outputColor[pixel] = float4(rawDepth.xxx, 1.0);
        outputNormalRoughness[pixel] = float4(0.0, 0.0, 0.0, 1.0);
        if (enableMotionVectors != 0u) outputMotion[pixel] = 0.0;
        return;
    }

    if (debugViewMode == 3u) {
        if (visValue.x == 0u) {
            outputColor[pixel] = float4(0.0, 0.0, 0.0, 1.0);
        } else {
            bool isEdge = false;
            const int2 offsets[4] = { int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1) };
            [unroll]
            for (int i = 0; i < 4; ++i) {
                int2 tap = clamp(int2(pixel) + offsets[i], int2(0, 0),
                                 int2(screenWidth - 1, screenHeight - 1));
                if (visBuffer.Load(int3(tap, 0)).x != visValue.x) {
                    isEdge = true;
                    break;
                }
            }
            if (isEdge) {
                outputColor[pixel] = float4(1.0, 1.0, 1.0, 1.0);
#if SGE_ENHANCED_VISUALS
                outputRayMask[pixel] = 1u;
#endif
            } else {
                outputColor[pixel] = float4(0.2, 0.2, 0.2, 1.0);
            }
        }
        outputNormalRoughness[pixel] = float4(0.0, 0.0, 0.0, 1.0);
        if (enableMotionVectors != 0u) outputMotion[pixel] = 0.0;
        return;
    }

    if (debugViewMode == 1u) {
        if (visValue.x == 0u) {
            outputColor[pixel] = float4(0.0, 0.0, 0.0, 1.0);
        } else {
            uint key = visValue.x * 1664525u + visValue.y * 1013904223u;
            float3 idColor = float3(key & 255u, (key >> 8u) & 255u,
                                    (key >> 16u) & 255u) / 255.0;
            outputColor[pixel] = float4(idColor, 1.0);
        }
        outputNormalRoughness[pixel] = float4(0.0, 0.0, 0.0, 1.0);
        if (enableMotionVectors != 0u) outputMotion[pixel] = 0.0;
        return;
    }
    
    // Zero instance ID means no geometry was written.
    if (visValue.x == 0u) {
        outputNormalRoughness[pixel] = float4(0.0, 0.0, 0.0, 1.0);
        // HDR sky was rasterized into outputColor before this compute pass.
        // Preserve it instead of replacing it with a mismatched procedural sky.
        if (enableMotionVectors != 0u) {
            // Reproject a far-plane point so camera rotation moves sky history
            // instead of pinning the previous sky to screen coordinates.
            float3 farWorld = ReconstructWorldPos(pixel, 1.0);
            float4 previousClip = mul(float4(farWorld, 1.0), previousViewProj);
            float2 currentUV = (float2(pixel) + 0.5) /
                               float2(screenWidth, screenHeight);
            float2 previousUV = currentUV;
            if (previousClip.w > 0.001)
                previousUV = (previousClip.xy / previousClip.w) *
                             float2(0.5, -0.5) + 0.5;
            outputMotion[pixel] = currentUV - previousUV;
        }
        return;
    }
    
    uint drawCallID = visValue.x - 1u;
    uint triangleID = visValue.y;
    
    // Load draw call data
    DrawCallData dc = drawCalls[drawCallID];
    const bool isFoliage =
        (dc.flags & 2u) != 0u && (dc.flags & 4u) == 0u;
    
    // Get triangle vertices (in object space)
    float3 p0, p1, p2;
    float3 n0, n1, n2;
    float2 uv0, uv1, uv2;
    GetTriangleVertices(dc, triangleID, p0, p1, p2, n0, n1, n2, uv0, uv1, uv2);
    const float3 originalP0 = p0;
    const float3 originalP1 = p1;
    const float3 originalP2 = p2;

    float3 tangentDummy = float3(1.0, 0.0, 0.0);
    ApplyPalmWind(p0, n0, tangentDummy, dc.palmWindRoot, palmWind,
                  palmPrimary, palmSecondary, palmParams);
    tangentDummy = float3(1.0, 0.0, 0.0);
    ApplyPalmWind(p1, n1, tangentDummy, dc.palmWindRoot, palmWind,
                  palmPrimary, palmSecondary, palmParams);
    tangentDummy = float3(1.0, 0.0, 0.0);
    ApplyPalmWind(p2, n2, tangentDummy, dc.palmWindRoot, palmWind,
                  palmPrimary, palmSecondary, palmParams);
    
    // Transform to world space
    float3 wp0 = mul(float4(p0, 1.0), dc.modelMatrix).xyz;
    float3 wp1 = mul(float4(p1, 1.0), dc.modelMatrix).xyz;
    float3 wp2 = mul(float4(p2, 1.0), dc.modelMatrix).xyz;
    
    // Reconstruct world position from depth
    float depth = depthBuffer.Load(int3(pixel, 0));
    float3 worldPos = ReconstructWorldPos(pixel, depth);
    
    // Compute barycentrics
    float3 bary = ComputeBarycentrics(worldPos, wp0, wp1, wp2);
    
    // Interpolate attributes
    float3 fragPos = bary.x * wp0 + bary.y * wp1 + bary.z * wp2;
    if (enableMotionVectors != 0u) {
        float4 previousWind = palmWind;
        previousWind.x = palmWind.y;
        float3 previousP0 = ApplyPalmWindPosition(
            originalP0, dc.palmWindRoot,
            previousWind, palmPreviousPrimary, palmPreviousSecondary, palmParams);
        float3 previousP1 = ApplyPalmWindPosition(
            originalP1, dc.palmWindRoot,
            previousWind, palmPreviousPrimary, palmPreviousSecondary, palmParams);
        float3 previousP2 = ApplyPalmWindPosition(
            originalP2, dc.palmWindRoot,
            previousWind, palmPreviousPrimary, palmPreviousSecondary, palmParams);
        float3 previousLocalPos = bary.x * previousP0 +
            bary.y * previousP1 + bary.z * previousP2;
        float3 previousWorldPos = mul(
            float4(previousLocalPos, 1.0), dc.previousModelMatrix).xyz;
        float4 previousClip = mul(float4(previousWorldPos, 1.0), previousViewProj);
        float2 currentUV = (float2(pixel) + 0.5) / float2(screenWidth, screenHeight);
        float2 previousUV = currentUV;
        if (previousClip.w > 0.001) {
            previousUV = (previousClip.xy / previousClip.w) * float2(0.5, -0.5) + 0.5;
        }
        outputMotion[pixel] = currentUV - previousUV;
    }
    
#if SGE_ENHANCED_VISUALS
    // Edge AA: shade 2 sub-pixel samples on silhouette edges and average.
    // Interior pixels are unchanged. Motion vectors and normal/roughness stay
    // with the centre sample — an averaged normal across a silhouette corrupts
    // the deferred/temporal consumers that expect one surface per pixel.
    bool edgeAAApplied = false;
    if (edgeAAEnabled != 0u) {
        bool isEdge = false;
        const int2 neighOffsets[4] = {
            int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1)
        };
        [unroll]
        for (int i = 0; i < 4; ++i) {
            int2 tap = clamp(int2(pixel) + neighOffsets[i],
                             int2(0, 0),
                             int2(screenWidth - 1, screenHeight - 1));
            if (visBuffer.Load(int3(tap, 0)).x != visValue.x) {
                isEdge = true;
                break;
            }
        }
        
        if (isEdge) {
            const float2 sampleOffsets[2] = {
                float2(0.25, 0.25), float2(0.75, 0.75)
            };
            float3 sampleColors[2];
            
            [unroll]
            for (uint s = 0; s < 2; ++s) {
                float3 worldPosS = ReconstructWorldPosOffset(
                    pixel, sampleOffsets[s], depth);
                
                float3 baryS = ComputeBarycentrics(worldPosS, wp0, wp1, wp2);
                float3 baryUsed = baryS;
                float3 wp0s = wp0, wp1s = wp1, wp2s = wp2;
                DrawCallData dcs = dc;
                bool isFoliageS = isFoliage;
                float3 n0s = n0, n1s = n1, n2s = n2;
                float2 uv0s = uv0, uv1s = uv1, uv2s = uv2;
                
                if (any(baryS < 0.0)) {
                    bool found = false;
                    [unroll]
                    for (int n = 0; n < 4 && !found; ++n) {
                        int2 tap = clamp(int2(pixel) + neighOffsets[n],
                                         int2(0, 0),
                                         int2(screenWidth - 1,
                                              screenHeight - 1));
                        uint2 visNeighbour = visBuffer.Load(int3(tap, 0));
                        if (visNeighbour.x != 0u &&
                            visNeighbour.x != visValue.x) {
                            uint nbrDrawCallID = visNeighbour.x - 1u;
                            uint nbrTriangleID = visNeighbour.y;
                            DrawCallData dcn = drawCalls[nbrDrawCallID];
                            
                            float3 np0, np1, np2;
                            float3 nn0, nn1, nn2;
                            float2 nuv0, nuv1, nuv2;
                            GetTriangleVertices(
                                dcn, nbrTriangleID,
                                np0, np1, np2, nn0, nn1, nn2,
                                nuv0, nuv1, nuv2);
                            
                            float3 tnDummy = float3(1.0, 0.0, 0.0);
                            ApplyPalmWind(np0, nn0, tnDummy,
                                          dcn.palmWindRoot, palmWind,
                                          palmPrimary, palmSecondary,
                                          palmParams);
                            tnDummy = float3(1.0, 0.0, 0.0);
                            ApplyPalmWind(np1, nn1, tnDummy,
                                          dcn.palmWindRoot, palmWind,
                                          palmPrimary, palmSecondary,
                                          palmParams);
                            tnDummy = float3(1.0, 0.0, 0.0);
                            ApplyPalmWind(np2, nn2, tnDummy,
                                          dcn.palmWindRoot, palmWind,
                                          palmPrimary, palmSecondary,
                                          palmParams);
                            
                            float3 nwp0 = mul(float4(np0, 1.0),
                                              dcn.modelMatrix).xyz;
                            float3 nwp1 = mul(float4(np1, 1.0),
                                              dcn.modelMatrix).xyz;
                            float3 nwp2 = mul(float4(np2, 1.0),
                                              dcn.modelMatrix).xyz;
                            
                            float3 nBary = ComputeBarycentrics(
                                worldPosS, nwp0, nwp1, nwp2);
                            if (all(nBary >= 0.0)) {
                                baryUsed = nBary;
                                wp0s = nwp0; wp1s = nwp1; wp2s = nwp2;
                                dcs = dcn;
                                isFoliageS = (dcn.flags & 2u) != 0u &&
                                             (dcn.flags & 4u) == 0u;
                                n0s = nn0; n1s = nn1; n2s = nn2;
                                uv0s = nuv0; uv1s = nuv1; uv2s = nuv2;
                                found = true;
                            }
                        }
                    }
                    if (!found) {
                        baryUsed = saturate(baryS);
                        baryUsed /= max(baryUsed.x + baryUsed.y +
                                        baryUsed.z, 1e-10);
                    }
                }
                
                float3 fragPosS = baryUsed.x * wp0s +
                                  baryUsed.y * wp1s +
                                  baryUsed.z * wp2s;
                Surface surfaceS = EvaluateSurface(
                    fragPosS, dcs, isFoliageS,
                    wp0s, wp1s, wp2s, n0s, n1s, n2s,
                    uv0s, uv1s, uv2s, baryUsed);
                sampleColors[s] = ShadeSurface(pixel, surfaceS);
            }
            
            float3 finalColor = (sampleColors[0] + sampleColors[1]) * 0.5;
            outputColor[pixel] = float4(finalColor, 1.0);
            edgeAAApplied = true;
        }
    }
#endif // SGE_ENHANCED_VISUALS
    
    // Centre surface always evaluated: normal/roughness and motion feed
    // deferred/temporal consumers that expect one surface per pixel.
    Surface surface = EvaluateSurface(fragPos, dc, isFoliage,
                                       wp0, wp1, wp2, n0, n1, n2,
                                       uv0, uv1, uv2, bary);
    outputNormalRoughness[pixel] = float4(surface.normal, surface.rough);

#if SGE_ENHANCED_VISUALS
    if (!edgeAAApplied) {
        float3 result = ShadeSurface(pixel, surface);
        outputColor[pixel] = float4(result, 1.0);
    }
#else
    float3 result = ShadeSurface(pixel, surface);
    outputColor[pixel] = float4(result, 1.0);
#endif
}
