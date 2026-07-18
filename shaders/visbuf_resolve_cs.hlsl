// Visibility Buffer Resolve - Compute Shader
// Reads the visibility buffer (R32_UINT) and depth buffer,
// reconstructs per-pixel surface attributes from structured buffers,
// then applies PBR shading identical to the forward clustered shader.

// ---- Constant Buffers ----

cbuffer FrameConstants : register(b0) {
    matrix viewMatrix;
    matrix projMatrix;
    matrix invViewProj;
    matrix lightViewProj;
    matrix previousViewProj;
    float3 cameraPos;
    float  screenWidth;
    float  screenHeight;
    float  nearPlane;
    float  farPlane;
    float  pad0;
};

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
Texture2D<float>  shadowMapTex : register(t2);

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
StructuredBuffer<ClusterData>   clusters  : register(t6);

struct MaterialData {
    float4 baseColorFactor;
    float4 emissiveOcclusion;
    float4 pbrParams;
    uint4 textureIndices;
};
StructuredBuffer<MaterialData> materials : register(t7);
Texture2D<float4> materialTextures[64] : register(t8);

RWTexture2D<float4> outputColor : register(u0);
RWTexture2D<float2> outputMotion : register(u1);

SamplerState              texSampler    : register(s0);
SamplerComparisonState    shadowSampler : register(s1);

// ---- Helpers ----

float3 ReconstructWorldPos(uint2 pixel, float depth) {
    float2 uv = (float2(pixel) + 0.5) / float2(screenWidth, screenHeight);
    float2 ndc = uv * 2.0 - 1.0;
    ndc.y = -ndc.y; // DX convention: Y up in NDC
    
    float4 clipPos = float4(ndc, depth, 1.0);
    float4 worldPos = mul(clipPos, invViewProj);
    return worldPos.xyz / worldPos.w;
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
    float up = saturate(reflectionDir.y * 0.5 + 0.5);
    float horizon = exp(-abs(reflectionDir.y) * 7.0);
    float3 zenith = float3(0.34, 0.58, 0.86) * skyIntensity;
    float3 horizonColor = float3(0.86, 0.78, 0.62) * skyIntensity;
    float3 ground = float3(0.10, 0.13, 0.09);
    float3 sky = lerp(horizonColor, zenith, smoothstep(0.30, 1.0, up));
    float3 environment = lerp(ground, sky, up);
    environment = lerp(environment, horizonColor, horizon * 0.35);
    float sunGlint = pow(saturate(dot(reflectionDir, normalize(lightPos))),
                         lerp(96.0, 12.0, roughness));
    environment += lightColor * sunGlint * (1.0 - roughness) * 1.8;
    float luminance = dot(environment, float3(0.299, 0.587, 0.114));
    return lerp(environment, luminance.xxx, roughness * 0.35);
}

float ScreenSpaceAO(uint2 pixel, float3 worldPos, float3 normal) {
    static const int2 directions[8] = {
        int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1),
        int2(1, 1), int2(-1, 1), int2(1, -1), int2(-1, -1)
    };
    float occlusion = 0.0;
    [unroll]
    for (uint i = 0; i < 8; ++i) {
        int2 samplePixel = clamp(int2(pixel) + directions[i] * 4,
                                 int2(0, 0), int2(screenWidth - 1, screenHeight - 1));
        float sampleDepth = depthBuffer.Load(int3(samplePixel, 0));
        if (sampleDepth >= 1.0) continue;
        float3 samplePosition = ReconstructWorldPos(samplePixel, sampleDepth);
        float3 delta = samplePosition - worldPos;
        float distanceToSample = length(delta);
        float horizon = saturate((dot(normal, delta / max(distanceToSample, 0.001)) - 0.08) * 3.0);
        occlusion += horizon * saturate(1.0 - distanceToSample / 2.5);
    }
    return saturate(1.0 - occlusion / 8.0);
}

float CalculateShadow(float3 worldPos, float3 normal, float3 lightDir) {
    if (enableShadows == 0) return 1.0;

    float4 lightClip = mul(float4(worldPos, 1.0), lightViewProj);
    if (lightClip.w <= 0.0) return 1.0;
    float3 projected = lightClip.xyz / lightClip.w;
    float2 uv = projected.xy * float2(0.5, -0.5) + 0.5;
    if (projected.z <= 0.0 || projected.z >= 1.0 ||
        any(uv < 0.0) || any(uv > 1.0)) return 1.0;

    uint shadowWidth, shadowHeight;
    shadowMapTex.GetDimensions(shadowWidth, shadowHeight);
    float2 texel = rcp(float2(shadowWidth, shadowHeight));
    float slopeBias = max(shadowBias * (1.0 - saturate(dot(normal, lightDir))),
                          shadowBias * 0.25);
    float visibility = 0.0;
    [unroll]
    for (int y = -1; y <= 1; ++y) {
        [unroll]
        for (int x = -1; x <= 1; ++x) {
            visibility += shadowMapTex.SampleCmpLevelZero(
                shadowSampler, uv + float2(x, y) * texel,
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

float3 calculatePointLight(int index, float3 fragPos, float3 normal, float3 viewDir) {
    float3 lightDir = normalize(pointLights[index].position - fragPos);
    float distance = length(pointLights[index].position - fragPos);
    
    if (distance > pointLights[index].radius) return float3(0, 0, 0);
    
    float attenuation = 1.0 / (1.0 + 0.7 * distance + 1.8 * distance * distance);
    float fadeFactor = saturate(1.0 - distance / pointLights[index].radius);
    fadeFactor *= fadeFactor;
    attenuation *= fadeFactor;
    
    float diff = max(dot(normal, lightDir), 0.0);
    float3 halfDir = normalize(lightDir + viewDir);
    float spec = pow(max(dot(normal, halfDir), 0.0), 32.0);
    
    float3 result = (diff + spec * 0.5) * pointLights[index].color * pointLights[index].intensity * attenuation;
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
    
    // Zero instance ID means no geometry was written.
    if (visValue.x == 0u) {
        outputColor[pixel] = float4(0.1, 0.1, 0.1, 1.0); // background
        outputMotion[pixel] = 0.0;
        return;
    }
    
    uint drawCallID = visValue.x - 1u;
    uint triangleID = visValue.y;
    
    // Load draw call data
    DrawCallData dc = drawCalls[drawCallID];
    
    // Get triangle vertices (in object space)
    float3 p0, p1, p2;
    float3 n0, n1, n2;
    float2 uv0, uv1, uv2;
    GetTriangleVertices(dc, triangleID, p0, p1, p2, n0, n1, n2, uv0, uv1, uv2);
    
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
    float3 localPos = bary.x * p0 + bary.y * p1 + bary.z * p2;
    float3 previousWorldPos = mul(float4(localPos, 1.0), dc.previousModelMatrix).xyz;
    float4 previousClip = mul(float4(previousWorldPos, 1.0), previousViewProj);
    float2 currentUV = (float2(pixel) + 0.5) / float2(screenWidth, screenHeight);
    float2 previousUV = currentUV;
    if (previousClip.w > 0.001) {
        previousUV = (previousClip.xy / previousClip.w) * float2(0.5, -0.5) + 0.5;
    }
    outputMotion[pixel] = currentUV - previousUV;
    
    float3 objNormal = normalize(bary.x * n0 + bary.y * n1 + bary.z * n2);
    // Transform normal to world space
    float3 normal = normalize(mul(objNormal, (float3x3)dc.modelMatrix));
    
    float2 texCoord = bary.x * uv0 + bary.y * uv1 + bary.z * uv2;
    float2 uvDx, uvDy;
    ComputeUVGradients(wp0, wp1, wp2, uv0, uv1, uv2, uvDx, uvDy);
    
    // Material
    MaterialData material = materials[min(dc.materialID, 255u)];
    float3 albedo = dc.objectColor * material.baseColorFactor.rgb;
    float metal = material.pbrParams.w > 0.5 ? material.pbrParams.x : dc.metalness;
    float rough = material.pbrParams.w > 0.5 ? material.pbrParams.y : dc.roughness;
    float materialAO = 1.0;
    if (material.textureIndices.x < 64u) {
        albedo *= materialTextures[material.textureIndices.x].SampleGrad(
            texSampler, texCoord, uvDx, uvDy).rgb;
    }
    if (material.textureIndices.z < 64u) {
        float4 mr = materialTextures[material.textureIndices.z].SampleGrad(
            texSampler, texCoord, uvDx, uvDy);
        materialAO = lerp(1.0, mr.r, saturate(material.emissiveOcclusion.w));
        if (material.textureIndices.w != 0u) rough *= mr.r;
        else { rough *= mr.g; metal *= mr.b; }
    }
    metal = saturate(metal);
    rough = clamp(rough, 0.04, 1.0);

    if (material.textureIndices.y < 64u) {
        float3 tangentNormal = materialTextures[material.textureIndices.y].SampleGrad(
            texSampler, texCoord, uvDx, uvDy).xyz * 2.0 - 1.0;
        tangentNormal.y *= material.pbrParams.z;
        float3 edge1 = wp1 - wp0;
        float3 edge2 = wp2 - wp0;
        float2 deltaUV1 = uv1 - uv0;
        float2 deltaUV2 = uv2 - uv0;
        float uvDet = deltaUV1.x * deltaUV2.y - deltaUV1.y * deltaUV2.x;
        if (abs(uvDet) > 1e-6) {
            float3 tangent = normalize((edge1 * deltaUV2.y - edge2 * deltaUV1.y) / uvDet);
            tangent = normalize(tangent - normal * dot(normal, tangent));
            float3 bitangent = normalize(cross(normal, tangent));
            normal = normalize(tangentNormal.x * tangent +
                               tangentNormal.y * bitangent +
                               tangentNormal.z * normal);
        }
    }
    
    // View direction
    float3 viewDir = normalize(cameraPos - fragPos);
    
    float3 result = 0.0;
    
    // Main light
    float3 L;
    float atten = 1.0;
    
    if (lightType == 0) {
        L = normalize(lightPos);
    } else {
        L = normalize(lightPos - fragPos);
        float dist = length(lightPos - fragPos);
        atten = 1.0 / (attConstant + attLinear * dist + attQuadratic * dist * dist);
    }
    
    float3 V = viewDir;
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

    float ambientOcclusion = ScreenSpaceAO(pixel, fragPos, normal) * materialAO;
    float3 diffuseIBL = SampleSkyIrradiance(normal) * albedo * kD / 3.14159265;
    float3 reflectionIBL = SampleReflectionProbe(reflect(-V, normal), rough);
    float3 specularIBL = reflectionIBL * F * (1.0 - rough * 0.65);
    result += (diffuseIBL + specularIBL) * ambientOcclusion;
    result += ambientStrength * albedo * ambientOcclusion;
    result += material.emissiveOcclusion.rgb;
    
    float3 numerator = NDF * G * F;
    float denominator = 4.0 * NdotV * NdotL + 0.0001;
    float3 specular = numerator / denominator;
    
    float shadowVisibility = CalculateShadow(fragPos, normal, L);
    float3 Lo = (kD * albedo / 3.14159265 + specular) * lightColor *
                NdotL * atten * shadowVisibility;
    result += Lo;
    
    // Clustered point lights: at most 32 relevant lights instead of all 64.
    uint clusterX = min((pixel.x * 16u) / max((uint)screenWidth, 1u), 15u);
    uint clusterY = min((pixel.y * 9u) / max((uint)screenHeight, 1u), 8u);
    float viewDepth = max(abs(mul(float4(fragPos, 1.0), viewMatrix).z), nearPlane);
    float depthScale = log(viewDepth / nearPlane) / log(farPlane / nearPlane);
    uint clusterZ = min((uint)(saturate(depthScale) * 10.0), 9u);
    ClusterData cluster = clusters[clusterX + clusterY * 16u + clusterZ * 144u];
    [loop]
    for (uint listIndex = 0; listIndex < min(cluster.lightCount, 32u); ++listIndex) {
        uint lightIndex = cluster.lightIndices[listIndex];
        if (lightIndex < (uint)numPointLights && lightIndex < 64u)
            result += calculatePointLight(lightIndex, fragPos, normal, viewDir) * albedo;
    }
    
    outputColor[pixel] = float4(result, 1.0);
}
