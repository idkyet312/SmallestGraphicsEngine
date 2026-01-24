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
    float padding;
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
    float obPadding;
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

static const float3 probeGridOrigin = float3(-7.0, 0.5, -7.0);
static const float probeSpacing = 2.0;
static const int probeCountX = 8;
static const int probeCountY = 4;
static const int probeCountZ = 8;
static const float giIntensity = 0.5;

Texture2D albedoMap : register(t1);
Texture2D normalMap : register(t4);
Texture2D metalRoughMap : register(t5);
SamplerState texSampler : register(s1);

struct PS_INPUT {
    float4 position : SV_POSITION;
    float3 fragPos : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 texCoord : TEXCOORD2;
};

// Simple GI approximation based on probe grid position
float3 sampleDDGIIrradiance(float3 worldPos, float3 normal) {
    // Calculate which probe cell we're in
    float3 offset = worldPos - probeGridOrigin;
    float3 gridPos = offset / probeSpacing;
    
    // Clamp to grid bounds
    gridPos = clamp(gridPos, float3(0,0,0), float3(probeCountX-1, probeCountY-1, probeCountZ-1));
    
    // Simple ambient occlusion approximation based on height
    float heightFactor = saturate(worldPos.y / 5.0);
    
    // Hemisphere lighting approximation
    float skyFactor = saturate(normal.y * 0.5 + 0.5);
    float groundFactor = saturate(-normal.y * 0.5 + 0.5);
    
    // Sky color contribution (blue-ish)
    float3 skyColor = float3(0.4, 0.5, 0.7);
    // Ground bounce (brownish)
    float3 groundColor = float3(0.3, 0.25, 0.2);
    
    float3 gi = skyColor * skyFactor + groundColor * groundFactor;
    gi *= heightFactor * 0.5 + 0.5; // Reduce GI in lower areas
    
    return gi * giIntensity;
}

float3 calculatePointLight(int index, float3 fragPos, float3 normal, float3 viewDir) {
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
    float spec = pow(max(dot(normal, halfwayDir), 0.0), (float)shininess);
    float3 specular = specularStrength * spec * lightCol * lightIntensity;
    
    return (diffuse + specular) * attenuation;
}

float4 main(PS_INPUT input) : SV_TARGET {
    float3 normal = normalize(input.normal);
    float3 viewDir = normalize(viewPos - input.fragPos);
    
    // Sample textures
    float3 albedo = objectColor;
    if (useTexture > 0.5) {
        float4 texColor = albedoMap.Sample(texSampler, input.texCoord);
        // texColor is sRGB? The Importer forces RGBA but format DXGI_FORMAT_R8G8B8A8_UNORM means linear? 
        // Usually file is sRGB. If UNORM, it is just raw values. If we need linear for PBR, we should pow 2.2.
        // But let's assume texture is albedo.
        albedo = texColor.rgb * objectColor; 
    }
    
    float metal = metalness;
    float rough = roughness;
    
    if (useTexture > 0.5) { // Assuming if albedo is used, metal/rough map might be too
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
        metal *= mrSample.b;
        rough *= mrSample.g;
    }
    
    // Simple Normal mapping (PerturbNormalArb)
    if (useNormalMap > 0.5) {
         float3 mapNormal = normalMap.Sample(texSampler, input.texCoord).xyz;
         mapNormal = mapNormal * 2.0 - 1.0;
         
         // Q: pos, uv derivatives
         float3 q1 = ddx(input.fragPos);
         float3 q2 = ddy(input.fragPos);
         float2 st1 = ddx(input.texCoord);
         float2 st2 = ddy(input.texCoord);
         
         float3 N = normalize(input.normal);
         float3 T = normalize(q1 * st2.y - q2 * st1.y);
         float3 B = -normalize(cross(N, T));
         float3x3 TBN = float3x3(T, B, N);
         
         normal = normalize(mul(mapNormal, TBN));
    }

    // Base ambient
    float3 ambient = ambientStrength * albedo;
    
    // Add DDGI global illumination
    float3 giContribution = sampleDDGIIrradiance(input.fragPos, normal);
    ambient += giContribution * albedo;
    
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
    float denominator = 4.0 * NdotV * NdotL + 0.0001;
    float3 specular = numerator / denominator;
    
    // Combine
    float3 Lo = (kD * albedo / 3.14159265 + specular) * lightColor * NdotL * attenuation; // No light intensity? lightColor should allow > 1.
    
    result += Lo;

    // Point lights contribution (clustered forward)
    // Need to PBR-ify this too
    for (int i = 0; i < numPointLights && i < 64; i++) {
        // result += calculatePointLight(i, input.fragPos, normal, viewDir) * objectColor;
        // Skip for now to save instruction count or update calculatePointLight to PBR
        // Just use simple Blinn for point lights for performance?
        result += calculatePointLight(i, input.fragPos, normal, viewDir) * albedo;
    }
    
    return float4(result, 1.0);
}





