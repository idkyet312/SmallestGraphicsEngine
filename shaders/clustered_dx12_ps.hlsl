// Clustered Forward Pixel Shader - DX12 Compatible

cbuffer MatrixBuffer : register(b0) {
    matrix model;
    matrix view;
    matrix projection;
    matrix lightSpaceMatrix;
};

cbuffer LightBuffer : register(b1) {
    float3 lightPos;
    int lightType;
    float3 lightColor;
    float attConstant;
    float attLinear;
    float attQuadratic;
    float ambientStrength;
    float specularStrength;
    int shininess;
    float shadowBias;
    int enableShadows;
    float shadowTexelSize;   // 1/shadow-map-size, precomputed on the CPU
};

cbuffer CameraBuffer : register(b2) {
    float3 viewPos;
    float cameraPadding;
};

cbuffer ObjectBuffer : register(b3) {
    float3 objectColor;
    float useTexture;
    float metalness;
    float roughness;
    float useNormalMap;
    float metalRoughMode;
    float opacity;
    float smokeMode;         // > 0.5: unlit soft sprite, alpha = opacity * texAlpha
    float alphaCut;          // 1: alpha cutout; 2: luminance cutout for hair
    float ambientScale;
    float occlusionStrength;
    float normalYSign;
    float viewFillStrength;
    float normalTexW;        // normal-map dimensions, precomputed on the CPU
    float normalTexH;
};

struct PointLightData {
    float3 position;
    float radius;
    float3 color;
    float intensity;
};

cbuffer PointLightsBuffer : register(b4) {
    int numPointLights;
    float plPadding1;
    float plPadding2;
    float plPadding3;
    PointLightData pointLights[64];
};

cbuffer DDGIBuffer : register(b5) {
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

// 9 L2 spherical-harmonic coefficients (RGB, cosine-lobe pre-convolved)
// approximating diffuse sky irradiance from the equirectangular sky HDRI.
cbuffer SHBuffer : register(b7) {
    float4 shCoeffs[9];
    float skyIntensity;
    float3 shPadding;
};

cbuffer ShadowCascadeBuffer : register(b8) {
    matrix shadowCascadeMatrices[3];
    float4 shadowCascadeSplits;
    float4 shadowCascadeTexelWorld;
    float4 shadowCascadeDepthRange;
};

#ifdef SGE_TERRAIN_PBR
Texture2DArray albedoMap : register(t1);
#else
Texture2D albedoMap : register(t1);
#endif
Texture2D irradianceMap : register(t2);
Texture2D visibilityMap : register(t3);
#ifdef SGE_TERRAIN_PBR
Texture2DArray normalMap : register(t4);
Texture2DArray metalRoughMap : register(t5);
#else
Texture2D normalMap : register(t4);
Texture2D metalRoughMap : register(t5);
#endif
Texture2DArray<float> shadowMap : register(t0);
Texture2D<float4> environmentMap : register(t15);
Texture2D<float2> brdfIntegrationLUT : register(t16);
struct SparseProbeData {
    float3 position; float radius;
    float3 normal; uint state;
    uint2 stableId; uint lastUpdatedFrame; uint padding;
};
struct SparseProbeCell {
    int3 coordinate; uint offset;
    uint count; uint3 padding;
};
StructuredBuffer<SparseProbeData> sparseProbes : register(t17);
StructuredBuffer<SparseProbeCell> sparseProbeCells : register(t18);
StructuredBuffer<uint> sparseProbeIndices : register(t19);
SamplerComparisonState shadowSampler : register(s0);
SamplerState texSampler : register(s1);

struct PS_INPUT {
    float4 position : SV_POSITION;
    float3 fragPos : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 texCoord : TEXCOORD2;
    float4 tangent : TEXCOORD3;
    float4 fragPosLightSpace : TEXCOORD4;
};

// Smooth 3D value noise from integer avalanche hashing (same mixer as
// terrain_ms.hlsl's hash21). A plane-wave sin() here striped every large flat
// dielectric surface -- most visibly the terrain -- with diagonal roughness
// bands ~6 m apart.
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

float CalculateShadow(float3 worldPos, float3 normal, float3 lightDir) {
    if (enableShadows == 0) return 1.0;

    float viewDepth = mul(float4(worldPos, 1.0), view).z;
    uint cascade = viewDepth < shadowCascadeSplits.x ? 0u :
                   (viewDepth < shadowCascadeSplits.y ? 1u : 2u);

    // Normal-offset + per-cascade slope-scaled bias. A single [0,1]-depth bias
    // constant cannot fit all three cascades (different world extents), which
    // left grazing-angle acne bands across distant terrain at low sun.
    float ndotl = saturate(dot(normal, lightDir));
    float texelWorld = shadowCascadeTexelWorld[cascade];
    float3 samplePos = worldPos + normal * texelWorld * 1.8;
    float4 fragPosLightSpace = mul(float4(samplePos, 1.0),
                                   shadowCascadeMatrices[cascade]);

    float3 projCoords = fragPosLightSpace.xyz / fragPosLightSpace.w;
    float2 shadowUV = projCoords.xy * float2(0.5, -0.5) + 0.5;

    if (projCoords.z <= 0.0 || projCoords.z >= 1.0 ||
        shadowUV.x < 0.0 || shadowUV.x > 1.0 ||
        shadowUV.y < 0.0 || shadowUV.y > 1.0) {
        return 1.0;
    }

    float2 texelSize = shadowTexelSize.xx;

    float slope = clamp(sqrt(1.0 - ndotl * ndotl) / max(ndotl, 0.1), 0.0, 8.0);
    float bias = texelWorld * (1.0 + 2.0 * slope) /
                 max(shadowCascadeDepthRange[cascade], 1e-3);
    float depth = projCoords.z - bias;

    float visibility = 0.0;
    [unroll]
    for (int y = -1; y <= 1; y++) {
        [unroll]
        for (int x = -1; x <= 1; x++) {
            visibility += shadowMap.SampleCmpLevelZero(
                shadowSampler,
                float3(shadowUV + float2(x, y) * texelSize, cascade), depth);
        }
    }

    return visibility / 9.0;
}

// Octahedral encoding helper (direction to UV)
float2 OctEncode(float3 n) {
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    if (n.y < 0) {
        float2 signN = float2(n.x >= 0 ? 1 : -1, n.z >= 0 ? 1 : -1);
        n.xz = (1.0 - abs(n.zx)) * signN;
    }
    return n.xz * 0.5 + 0.5;
}

// Sample irradiance from a single probe
float3 sampleProbeIrradiance(int probeIndex, float3 direction) {
    // Calculate atlas dimensions
    int totalProbes = sparseProbeCount > 0 ? sparseProbeCount :
                      probeCountX * probeCountY * probeCountZ;
    int atlasWidthProbes = sparseProbeCount > 0
        ? (int)ceil(sqrt((float)totalProbes))
        : (int)sqrt((float)totalProbes);
    if (atlasWidthProbes < 1) atlasWidthProbes = 1;
    int atlasHeightProbes = (totalProbes + atlasWidthProbes - 1) / atlasWidthProbes;
    
    // Calculate probe position in atlas
    int probeX = probeIndex % atlasWidthProbes;
    int probeY = probeIndex / atlasWidthProbes;
    
    // Each probe tile size (with border)
    int tileWidth = irradianceTexWidth + 2;
    int tileHeight = irradianceTexHeight + 2;
    
    // Full atlas texture size
    int atlasWidth = atlasWidthProbes * tileWidth;
    int atlasHeight = atlasHeightProbes * tileHeight;
    
    // Get octahedral UV for direction
    float2 octUV = OctEncode(direction);
    
    // Map to probe's tile area (excluding border)
    float2 probeUV;
    probeUV.x = (probeX * tileWidth + 1 + octUV.x * irradianceTexWidth) / (float)atlasWidth;
    probeUV.y = (probeY * tileHeight + 1 + octUV.y * irradianceTexHeight) / (float)atlasHeight;
    
    return irradianceMap.SampleLevel(texSampler, probeUV, 0).rgb;
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

float DDGIProbeVisibility(int probeIndex, float3 probeToPoint,
                          float distanceToPoint) {
    int atlasWidthProbes = max((int)ceil(sqrt((float)sparseProbeCount)), 1);
    int probeX = probeIndex % atlasWidthProbes;
    int probeY = probeIndex / atlasWidthProbes;
    float2 octUV = OctEncode(normalize(probeToPoint));
    uint atlasWidth, atlasHeight;
    visibilityMap.GetDimensions(atlasWidth, atlasHeight);
    float2 uv = float2(
        (probeX * (visibilityTexWidth + 2) + 1 +
         octUV.x * visibilityTexWidth) / atlasWidth,
        (probeY * (visibilityTexHeight + 2) + 1 +
         octUV.y * visibilityTexHeight) / atlasHeight);
    float2 moments = visibilityMap.SampleLevel(texSampler, uv, 0).rg;
    if (moments.y <= 1e-5 || distanceToPoint <= moments.x) return 1.0;
    float variance = max(moments.y - moments.x * moments.x, 0.001);
    float delta = distanceToPoint - moments.x;
    return saturate(variance / (variance + delta * delta));
}

float3 sampleSparseDDGI(float3 worldPos, float3 normal) {
    float3 biasedPos = worldPos + normal * normalBias;
    int3 center = (int3)floor(biasedPos / sparseCellSize);
    float3 sum = 0.0;
    float weightSum = 0.0;
    uint accepted = 0;
    [loop] for (int z = -1; z <= 1 && accepted < 8; ++z)
    [loop] for (int y = -1; y <= 1 && accepted < 8; ++y)
    [loop] for (int x = -1; x <= 1 && accepted < 8; ++x) {
        int3 coordinate = center + int3(x, y, z);
        uint slot = DDGICellHash(coordinate) & (sparseCellCount - 1);
        [loop] for (uint probe = 0; probe < (uint)sparseCellCount; ++probe) {
            SparseProbeCell cell = sparseProbeCells[slot];
            if (cell.count == 0) break;
            if (all(cell.coordinate == coordinate)) {
                [loop] for (uint i = 0; i < cell.count && accepted < 8; ++i) {
                    uint index = sparseProbeIndices[cell.offset + i];
                    SparseProbeData data = sparseProbes[index];
                    if (data.state == 2) continue;
                    float3 delta = biasedPos - data.position;
                    float distanceToProbe = length(delta);
                    float normalWeight = saturate(dot(normal, data.normal) *
                                                  0.5 + 0.5);
                    float visibility = DDGIProbeVisibility(index, delta,
                                                           distanceToProbe);
                    float weight = normalWeight * visibility /
                                   max(distanceToProbe * distanceToProbe, 0.04);
                    sum += sampleProbeIrradiance(index, normal) * weight;
                    weightSum += weight;
                    ++accepted;
                }
                break;
            }
            slot = (slot + 1) & (sparseCellCount - 1);
        }
    }
    return weightSum > 1e-5 ? sum / weightSum * giIntensity : 0.0;
}

// Calculate probe index from grid coordinates
int getProbeIndex(int3 gridCoord) {
    return gridCoord.x + gridCoord.y * probeCountX + gridCoord.z * probeCountX * probeCountY;
}

// Sample GI from probe grid with trilinear interpolation
float3 sampleDDGIIrradiance(float3 worldPos, float3 normal) {
    if (!ddgiEnabled) return float3(0, 0, 0);
    if (sparseProbeCount > 0 && sparseCellCount > 0)
        return sampleSparseDDGI(worldPos, normal);

    // Apply normal bias to avoid self-shadowing
    float3 biasedPos = worldPos + normal * normalBias;
    
    // Calculate grid-relative position
    float3 offset = biasedPos - probeGridOrigin;
    float3 gridPosF = offset / probeSpacing;
    
    // Check if outside grid
    if (gridPosF.x < 0 || gridPosF.y < 0 || gridPosF.z < 0 ||
        gridPosF.x >= probeCountX || gridPosF.y >= probeCountY || gridPosF.z >= probeCountZ) {
        // Outside grid - return small ambient based on normal
        float skyFactor = saturate(normal.y * 0.5 + 0.5);
        return float3(0.02, 0.025, 0.03) * skyFactor * giIntensity;
    }
    
    // Get base probe indices for trilinear interpolation
    int3 baseProbe = int3(floor(gridPosF));
    baseProbe = clamp(baseProbe, int3(0, 0, 0), int3(probeCountX - 2, probeCountY - 2, probeCountZ - 2));
    
    // Interpolation weights
    float3 weights = frac(gridPosF);
    
    // Sample 8 surrounding probes and interpolate
    float3 irradiance = float3(0, 0, 0);
    float totalWeight = 0.0;
    
    for (int dz = 0; dz <= 1; dz++) {
        for (int dy = 0; dy <= 1; dy++) {
            for (int dx = 0; dx <= 1; dx++) {
                int3 probeCoord = baseProbe + int3(dx, dy, dz);
                
                // Clamp to valid range
                probeCoord = clamp(probeCoord, int3(0, 0, 0), int3(probeCountX - 1, probeCountY - 1, probeCountZ - 1));
                
                int probeIndex = getProbeIndex(probeCoord);
                
                // Calculate trilinear weight
                float wx = dx == 0 ? (1.0 - weights.x) : weights.x;
                float wy = dy == 0 ? (1.0 - weights.y) : weights.y;
                float wz = dz == 0 ? (1.0 - weights.z) : weights.z;
                float weight = wx * wy * wz;
                
                // Apply cosine weighting (prefer probes in the direction we're sampling)
                float3 probePos = probeGridOrigin + float3(probeCoord) * probeSpacing;
                float3 dirToProbe = normalize(probePos - biasedPos);
                float cosWeight = saturate(dot(dirToProbe, normal) * 0.5 + 0.5);
                weight *= cosWeight;
                
                if (weight > 0.0001) {
                    float3 probeSample = sampleProbeIrradiance(probeIndex, normal);
                    irradiance += probeSample * weight;
                    totalWeight += weight;
                }
            }
        }
    }
    
    if (totalWeight > 0.0001) {
        irradiance /= totalWeight;
    }
    
    // Apply gamma correction (probes store linear, but we apply a gamma to brighten)
    irradiance = pow(max(irradiance, 0.0), 1.0 / irradianceGamma);
    
    return irradiance * giIntensity;
}

// Evaluates the L2 SH irradiance in the given normal direction. Coefficients
// already include the cosine-lobe convolution, so this is a flat dot product
// (Ramamoorthi/Hanrahan's SH diffuse irradiance formula, factors folded in
// on the CPU side) rather than a full BRDF integral.
float3 sampleSkyIrradiance(float3 normal) {
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

// Roughness-aware specular IBL from the same Quarry 01 HDRI rendered as sky.
float3 sampleReflectionProbe(float3 reflectionDir, float rough) {
    reflectionDir = normalize(reflectionDir);
    float2 uv = float2(atan2(reflectionDir.z, reflectionDir.x) * 0.159154943 + 0.5,
                       acos(clamp(reflectionDir.y, -1.0, 1.0)) * 0.318309886);
    uint width, height, mipCount;
    environmentMap.GetDimensions(0, width, height, mipCount);
    float lod = rough * rough * max((float)mipCount - 1.0, 0.0);
    return environmentMap.SampleLevel(texSampler, uv, lod).rgb * skyIntensity;
}

float3 calculatePointLight(int index, float3 fragPos, float3 normal, float3 viewDir, float rough) {
    float3 lightPosition = pointLights[index].position;
    float3 lightCol = pointLights[index].color;
    float lightRadius = pointLights[index].radius;
    float lightIntensity = pointLights[index].intensity;
    
    float3 lightDir = normalize(lightPosition - fragPos);
    float distance = length(lightPosition - fragPos);
    
    if (distance > lightRadius) return float3(0, 0, 0);
    
    float att_linear = 4.5 / lightRadius;
    float att_quadratic = 75.0 / (lightRadius * lightRadius);
    float attenuation = 1.0 / (1.0 + att_linear * distance + att_quadratic * distance * distance);
    
    float falloff = 1.0 - smoothstep(lightRadius * 0.75, lightRadius, distance);
    attenuation *= falloff;
    
    float diff = max(dot(normal, lightDir), 0.0);
    float3 diffuse = diff * lightCol * lightIntensity;
    
    float3 halfwayDir = normalize(lightDir + viewDir);
    float pointShininess = lerp(8.0, 128.0, saturate(1.0 - rough));
    float spec = pow(max(dot(normal, halfwayDir), 0.0), pointShininess);
    float3 specular = specularStrength * saturate(1.0 - rough) * spec * lightCol * lightIntensity;
    
    return (diffuse + specular) * attenuation;
}

// AgX tone mapping (Troy Sobotka's AgX, minimal 6th-order polynomial fit by
// Benjamin Wrensch) with the "Punchy" look for extra contrast + saturation.
// Takes linear HDR, returns display-ready sRGB (already gamma-encoded).
float3 agxDefaultContrastApprox(float3 x) {
    float3 x2 = x * x;
    float3 x4 = x2 * x2;
    return  15.5     * x4 * x2
          - 40.14    * x4 * x
          + 31.96    * x4
          - 6.868    * x2 * x
          + 0.4298   * x2
          + 0.1191   * x
          - 0.00232;
}

float3 tonemapAgXPunchy(float3 color) {
    // Input transform (sRGB primaries -> AgX working space).
    const float3x3 agxIn = float3x3(
        0.842479062253094,  0.0423282422610123, 0.0423756549057051,
        0.0784335999999992, 0.878468636469772,  0.0784336,
        0.0792237451477643, 0.0791661274605434, 0.879142973793104);
    const float3x3 agxOut = float3x3(
         1.19687900512017,   -0.0528968517574562, -0.0529716355144438,
        -0.0980208811401368,  1.15190312990417,   -0.0980434501171241,
        -0.0990297440797205, -0.0989611768448433,  1.15107367264116);

    const float minEv = -12.47393;
    const float maxEv =  4.026069;

    color = mul(agxIn, color);
    color = clamp(log2(max(color, 1e-10)), minEv, maxEv);
    color = (color - minEv) / (maxEv - minEv);
    color = agxDefaultContrastApprox(color);

    // Punchy look: lift saturation and gamma for a bolder image.
    const float3 lw = float3(0.2126, 0.7152, 0.0722);
    float luma = dot(color, lw);
    color = pow(color, 1.35);                 // punchy contrast
    color = luma + 1.4 * (color - luma);      // punchy saturation

    color = mul(agxOut, color);
    return saturate(color);                   // already display-encoded
}

float3 FinalizeOutput(float3 color) {
#ifdef SGE_HDR_TARGET
    return max(color, 0.0);
#else
    return tonemapAgXPunchy(max(color, 0.0));
#endif
}

#ifdef SGE_TERRAIN_PBR
#include "terrain_pbr.hlsli"
#endif

float4 main(PS_INPUT input) : SV_TARGET {
    // Solid unlit emissive geometry. Additive PSO turns opacity into glow weight.
    if (smokeMode > 1.5) {
        return float4(FinalizeOutput(objectColor), opacity);
    }

    // Unlit soft smoke sprite: sample the puff texture, tint by objectColor, and
    // let its alpha (times opacity) shape a soft translucent billboard. Skips all
    // lighting/fog so smoke reads as a light-scattering volume, not a lit surface.
    if (smokeMode > 0.5) {
#ifdef SGE_TERRAIN_PBR
        float4 smoke = albedoMap.Sample(texSampler, float3(input.texCoord, 0));
#else
        float4 smoke = albedoMap.Sample(texSampler, input.texCoord);
#endif
        float a = smoke.a * opacity;
        if (a <= 0.003) discard;
        // Tone-map/encode to match the rest of the frame's output.
        float3 c = smoke.rgb * objectColor;
        c = FinalizeOutput(c);
        return float4(c, a);
    }

    float3 normal = normalize(input.normal);
    float3 viewDir = normalize(viewPos - input.fragPos);

    // Sample textures
    float3 albedo = objectColor;
#ifdef SGE_TERRAIN_PBR
    TerrainPBR terrain = SampleTerrainPBR(input.fragPos, normal);
    // Terrain albedo uses an sRGB SRV, so hardware has already decoded it.
    albedo = max(terrain.albedo, 0.0) * objectColor;
#else
    if (useTexture > 0.5) {
        float4 texColor = albedoMap.Sample(texSampler, input.texCoord);
        // Alpha cutout for foliage cards (palm fronds), opt-in per material.
        // clip() disables early-Z for the draw, which is expensive scene-wide --
        // an unconditional clip here once pushed heavy-overdraw frames past the
        // GPU watchdog (device removed). Only foliage pays for it now.
        // Bandit hair atlas stores bright strands on black. A low threshold
        // keeps grey mip-filtered background and turns eyelash cards into solid
        // black strips across the face, so retain only authored strand coverage.
        if (alphaCut > 1.5) clip(max(texColor.r, max(texColor.g, texColor.b)) - 0.62);
        // Preserve thin palm rachises and leaflet stems. Higher cutoff detached
        // opaque leaf clusters from their nearly transparent connecting pixels.
        else if (alphaCut > 0.5) clip(texColor.a - 0.20);
        // Textures are uploaded as UNORM, so decode authored sRGB before lighting.
        albedo = pow(max(texColor.rgb, 0.0), 2.2) * objectColor;
    }
#endif
    float metal = metalness;
    float rough = roughness;
    float ambientOcclusion = 1.0;

#ifdef SGE_TERRAIN_PBR
    metal = 0.0;
    rough = terrain.roughness;
    normal = terrain.normal;
#else
    if (metalRoughMode > 0.5) {
        // Check if metalRoughMap is bound? We don't have a flag for it specifically, assuming bundled with material
        // But for GLB, MetalRough is usually packed. B=Metal, G=Roughness.
        // Let's sample if useTexture is true? Or useNormalMap flag?
        // Actually, let's just sample it. If not bound, it returns 0.
        // We can use a flag for it, but I didn't add one.
        // Let's rely on 'useTexture' for now.
        float4 mrSample = metalRoughMap.Sample(texSampler, input.texCoord);
        // GLTF: Blue = Metal, Green = Roughness
        // If the map is present (not black/empty), use it?
        // But we don't know if it is present in shader. 
        // We can check if metalness/roughness factors are different than defaults?
        // For now, let's just use the factors as multipliers.
        // GLTF spec says: metallicFactor * metallicTexture.b
        // roughnessFactor * roughnessTexture.g
        
        // Since we don't know if valid texture is bound, this might read garbage (0 or 1 or black) if we bound a null SRV.
        // If null SRV, it likely returns 0.
        // So metal = metalness * 0 = 0.
        // This is bad if we wanted 1.0 (default).
        // I'll stick to simple constants if no specific flag.
        // But I will enable it:
        if (metalRoughMode < 1.5) {
            metal *= mrSample.b;
            rough *= mrSample.g;
            ambientOcclusion = lerp(1.0, mrSample.r, occlusionStrength);
        } else {
            // Standalone roughness maps are authored as final roughness, not a
            // glTF multiplier. Never let dark texels turn metal mirror-smooth.
            rough = max(rough, mrSample.g);
        }
    }
#endif
    rough = clamp(rough, 0.045, 1.0); // avoid alpha->0 specular-aliasing spike
    
    // Normal mapping through a stable vertex tangent frame. Imported meshes
    // without tangents get UV-derived tangents generated at load time.
#ifndef SGE_TERRAIN_PBR
    if (useNormalMap > 0.5) {
         float normalMipBias = 1.5;
         float normalStrength = 0.70;
         float3 mapNormal = normalMap.SampleBias(texSampler, input.texCoord, normalMipBias).xyz * 2.0 - 1.0;
         mapNormal.y *= normalYSign;
         float normalMipLength = saturate(length(mapNormal));
         mapNormal.xy *= normalMipLength;
         mapNormal.z = sqrt(saturate(1.0 - dot(mapNormal.xy, mapNormal.xy)));

         float3 N = normalize(input.normal);
         float3 T = normalize(input.tangent.xyz - N * dot(N, input.tangent.xyz));
         float3 B = normalize(cross(N, T) * input.tangent.w);
         float3x3 TBN = float3x3(T, B, N);

         float3 mappedNormal = normalize(mul(mapNormal, TBN));
         float2 normalMapSize = float2(normalTexW, normalTexH);
         float normalFootprint = max(length(ddx(input.texCoord) * normalMapSize), length(ddy(input.texCoord) * normalMapSize));
         float minifyFade = 1.0 - saturate((log2(max(normalFootprint, 1.0)) - 1.0) * 0.25);
         // Normal maps become unstable near silhouettes: a small tangent-space
         // perturbation can rotate the shading normal below the light horizon.
         // Fade them out early and smoothly toward the geometric normal.
         float grazingFade = smoothstep(0.25, 0.65,
             saturate(abs(dot(N, viewDir))));
         normal = normalize(lerp(N, mappedNormal, normalStrength * grazingFade * minifyFade));
    }
#endif

    // Forward imports use a no-cull PSO so foliage cards, rotor blades, and
    // mixed-winding assets remain visible. Orient the shading normal toward
    // the visible side; otherwise back faces light inside-out.
    if (dot(normal, viewDir) < 0.0)
        normal = -normal;

    if (metal < 0.25) {
        float materialVariation = MatVarNoise(input.fragPos * 0.35);
        rough = clamp(rough * lerp(0.88, 1.10, materialVariation), 0.045, 1.0);
    }
    
    // Geometric specular AA: widen GGX for high screen-space normal variance.
    float3 dndx = ddx(normal);
    float3 dndy = ddy(normal);
    float normalVariance = dot(dndx, dndx) + dot(dndy, dndy);
    rough = clamp(sqrt(rough * rough + min(normalVariance * 0.25, 0.25)), 0.045, 1.0);

    // Base ambient
    float3 diffuseAlbedo = albedo * (1.0 - metal);
    float3 ambient = ambientStrength * diffuseAlbedo * ambientScale;
    
    // Add DDGI global illumination
    float3 giContribution = sampleDDGIIrradiance(input.fragPos, normal);
    ambient += giContribution * diffuseAlbedo * ambientScale;

    // Add sky IBL (diffuse irradiance from the HDRI, via SH)
    float3 skyContribution = sampleSkyIrradiance(normal);
    ambient += skyContribution * diffuseAlbedo * ambientScale;
    ambient *= ambientOcclusion;
    float3 result = ambient;
    
    // Main directional/point light
    float3 lightDir;
    float attenuation = 1.0;
    
    if (lightType == 0) {
        // Directional light
        lightDir = normalize(lightPos);
    } else {
        // Point light
        lightDir = normalize(lightPos - input.fragPos);
        float distance = length(lightPos - input.fragPos);
        attenuation = 1.0 / (attConstant + attLinear * distance + attQuadratic * distance * distance);
    }
    
    // Cook-Torrance BRDF (Simplified for PBR) or just Blinn-Phong?
    // User wants "like blender", which is PBR (Principled BSDF).
    // Let's do a simple PBR implementation.
    
    float3 V = viewDir;
    float3 L = lightDir;
    float3 H = normalize(V + L);
    float NdotL = max(dot(normal, L), 0.0);
    float NdotV = max(dot(normal, V), 0.0);
    float NdotH = max(dot(normal, H), 0.0);
    float HdotV = max(dot(H, V), 0.0);
    
    // Fresnel (Schlick)
    float3 F0 = float3(0.04, 0.04, 0.04); 
    F0 = lerp(F0, albedo, metal);
    float3 F = F0 + (1.0 - F0) * pow(1.0 - HdotV, 5.0);
    
    // NDF (GGX)
    float alpha = rough * rough;
    float alpha2 = alpha * alpha;
    float NdotH2 = NdotH * NdotH;
    float num = alpha2;
    float denom = (NdotH2 * (alpha2 - 1.0) + 1.0);
    denom = 3.14159265 * denom * denom;
    float NDF = num / max(denom, 0.000001);
    
    // Geometry (Smith)
    float k = (rough + 1.0) * (rough + 1.0) / 8.0;
    float ggx1 = NdotV / (NdotV * (1.0 - k) + k);
    float ggx2 = NdotL / (NdotL * (1.0 - k) + k);
    float G = ggx1 * ggx2;
    
    float3 kS = F;
    float3 kD = float3(1.0, 1.0, 1.0) - kS;
    kD *= 1.0 - metal;
    
    float3 numerator = NDF * G * F;
    float denominator = max(4.0 * NdotV * NdotL, 0.001);
    // Skinned enemy outfits share a positive view-fill tag. Their mostly
    // cloth/polymer surfaces need much less HDRI grazing response than guns
    // and hard-surface assets, or the bright horizon creates a silver rim.
    float characterSpecularScale = viewFillStrength > 0.25 ? 0.38 : 1.0;
    float3 specular = numerator / denominator * characterSpecularScale;
    
    // Combine
    float shadowVisibility = CalculateShadow(input.fragPos, normal, lightDir);
    // Shadow map blocks direct sun only. Sky IBL and bounced DDGI remain,
    // preventing sun-facing occluders from crushing entire facades to black.
    // Material-local camera fill is applied after scene shadowing. Imported
    // character previews use a soft frontal studio light; applying this before
    // the shadow term crushed it back to black whenever the sun was behind him.
    // Keep authored view fill present at silhouettes. The old 0.35 floor made
    // grazing surfaces lose most of their only indirect-light fallback.
    float frontFill = 0.65 + 0.35 * saturate(dot(normal, viewDir));
    result += diffuseAlbedo * max(viewFillStrength, 0.0) * frontFill;
    float3 Lo = (kD * albedo / 3.14159265 + specular) * lightColor * NdotL * attenuation * shadowVisibility; // No light intensity? lightColor should allow > 1.
    
    result += Lo;

    // Point lights contribution (clustered forward)
    // Need to PBR-ify this too
    for (int i = 0; i < numPointLights && i < 64; i++) {
        // result += calculatePointLight(i, input.fragPos, normal, viewDir) * objectColor;
        // Skip for now to save instruction count or update calculatePointLight to PBR
        // Just use simple Blinn for point lights for performance?
        result += calculatePointLight(i, input.fragPos, normal, viewDir, rough) * albedo;
    }

    float3 reflectionDir = reflect(-viewDir, normal);
    // Split-sum specular IBL: GGX-prefiltered HDRI radiance multiplied by the
    // preintegrated Fresnel/visibility response for this NdotV and roughness.
    float3 probeColor = sampleReflectionProbe(reflectionDir, rough);
    float2 environmentBRDF = brdfIntegrationLUT.SampleLevel(
        texSampler, float2(NdotV, rough), 0.0);
    result += probeColor * (F0 * environmentBRDF.x + environmentBRDF.y) *
              ambientOcclusion * characterSpecularScale;

    // AgX (Punchy) tone mapping; returns display-encoded sRGB.
    result = FinalizeOutput(result);
    return float4(result, opacity);
}





