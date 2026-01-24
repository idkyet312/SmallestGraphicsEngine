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
    float objectPadding;
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

// DDGI settings passed via unused light buffer fields for now
// In a full implementation, this would be a separate cbuffer
static const float3 probeGridOrigin = float3(-7.0, 0.5, -7.0);
static const float probeSpacing = 2.0;
static const int probeCountX = 8;
static const int probeCountY = 4;
static const int probeCountZ = 8;
static const float giIntensity = 0.5;

struct PS_INPUT {
    float4 position : SV_POSITION;
    float3 fragPos : TEXCOORD0;
    float3 normal : TEXCOORD1;
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
    
    // Base ambient
    float3 ambient = ambientStrength * objectColor;
    
    // Add DDGI global illumination
    float3 giContribution = sampleDDGIIrradiance(input.fragPos, normal);
    ambient += giContribution * objectColor;
    
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
    
    // Diffuse
    float diff = max(dot(normal, lightDir), 0.0);
    float3 diffuse = diff * lightColor * objectColor;
    
    // Specular (Blinn-Phong)
    float3 halfwayDir = normalize(lightDir + viewDir);
    float spec = pow(max(dot(normal, halfwayDir), 0.0), (float)shininess);
    float3 specular = specularStrength * spec * lightColor;
    
    result += (diffuse + specular) * attenuation;
    
    // Point lights contribution (clustered forward)
    for (int i = 0; i < numPointLights && i < 64; i++) {
        result += calculatePointLight(i, input.fragPos, normal, viewDir) * objectColor;
    }
    
    return float4(result, 1.0);
}

