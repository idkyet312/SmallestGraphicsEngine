// Clustered Forward Pixel Shader - DX11 HLSL

cbuffer MatrixBuffer : register(b0) {
    matrix model;
    matrix view;
    matrix projection;
    matrix lightSpaceMatrix;
};

cbuffer LightBuffer : register(b1) {
    float3 lightPos;
    int lightType;          // 16 bytes
    float3 lightColor;
    float attConstant;      // 16 bytes
    float attLinear;
    float attQuadratic;
    float ambientStrength;
    float specularStrength; // 16 bytes
    int shininess;
    float shadowBias;
    int enableShadows;
    float padding;          // 16 bytes
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
    float3 plPadding;
    PointLightData pointLights[32];
};

Texture2D shadowMap : register(t0);
SamplerComparisonState shadowSampler : register(s0);
SamplerState regularSampler : register(s1);

struct PS_INPUT {
    float4 position : SV_POSITION;
    float3 fragPos : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float4 fragPosLightSpace : TEXCOORD2;
};

float ShadowCalculation(float4 fragPosLightSpace, float3 normal, float3 lightDir) {
    if (enableShadows == 0) return 0.0;
    
    // Perspective divide
    float3 projCoords = fragPosLightSpace.xyz / fragPosLightSpace.w;
    
    // Transform to [0,1] range for DX (note: DX uses 0-1 depth range)
    projCoords.x = projCoords.x * 0.5 + 0.5;
    projCoords.y = -projCoords.y * 0.5 + 0.5; // Flip Y for DX
    
    // Check if outside shadow map
    if (projCoords.z > 1.0 || projCoords.x < 0.0 || projCoords.x > 1.0 || 
        projCoords.y < 0.0 || projCoords.y > 1.0)
        return 0.0;
    
    float currentDepth = projCoords.z;
    
    // PCF soft shadows
    float shadow = 0.0;
    float2 texelSize;
    uint width, height;
    shadowMap.GetDimensions(width, height);
    texelSize = 1.0 / float2(width, height);
    
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            float2 offset = float2(x, y) * texelSize;
            shadow += shadowMap.SampleCmpLevelZero(shadowSampler, projCoords.xy + offset, currentDepth - shadowBias);
        }
    }
    shadow /= 9.0;
    
    return 1.0 - shadow; // Invert because SampleCmp returns 1 when NOT in shadow
}

float3 calculatePointLight(int index, float3 fragPos, float3 normal, float3 viewDir) {
    float3 lightPosition = pointLights[index].position;
    float3 lightCol = pointLights[index].color;
    float lightRadius = pointLights[index].radius;
    float lightIntensity = pointLights[index].intensity;
    
    float3 lightDir = normalize(lightPosition - fragPos);
    float distance = length(lightPosition - fragPos);
    
    // Skip if outside light radius
    if (distance > lightRadius) return float3(0, 0, 0);
    
    // Attenuation based on radius
    float att_linear = 4.5 / lightRadius;
    float att_quadratic = 75.0 / (lightRadius * lightRadius);
    float attenuation = 1.0 / (1.0 + att_linear * distance + att_quadratic * distance * distance);
    
    // Smooth falloff at edge of radius
    float falloff = 1.0 - smoothstep(lightRadius * 0.75, lightRadius, distance);
    attenuation *= falloff;
    
    // Diffuse
    float diff = max(dot(normal, lightDir), 0.0);
    float3 diffuse = diff * lightCol * lightIntensity;
    
    // Specular (Blinn-Phong)
    float3 halfwayDir = normalize(lightDir + viewDir);
    float spec = pow(max(dot(normal, halfwayDir), 0.0), (float)shininess);
    float3 specular = specularStrength * spec * lightCol * lightIntensity;
    
    return (diffuse + specular) * attenuation;
}

float4 main(PS_INPUT input) : SV_TARGET {
    float3 normal = normalize(input.normal);
    float3 viewDir = normalize(viewPos - input.fragPos);
    
    // Ambient
    float3 ambient = ambientStrength * objectColor;
    
    // Main light contribution
    float3 result = ambient;
    
    // Main directional/point light with shadows
    float3 lightDir;
    float attenuation = 1.0;
    
    if (lightType == 0) {
        // Directional light
        lightDir = normalize(lightPos - float3(0, 0, 0));
    } else {
        // Point light
        lightDir = normalize(lightPos - input.fragPos);
        float distance = length(lightPos - input.fragPos);
        attenuation = 1.0 / (attConstant + attLinear * distance + attQuadratic * distance * distance);
    }
    
    // Diffuse
    float diff = max(dot(normal, lightDir), 0.0);
    float3 diffuse = diff * lightColor * objectColor;
    
    // Specular
    float3 halfwayDir = normalize(lightDir + viewDir);
    float spec = pow(max(dot(normal, halfwayDir), 0.0), (float)shininess);
    float3 specular = specularStrength * spec * lightColor;
    
    // Shadow
    float shadow = ShadowCalculation(input.fragPosLightSpace, normal, lightDir);
    
    result += (1.0 - shadow) * (diffuse + specular) * attenuation;
    
    // Point lights contribution (clustered)
    for (int i = 0; i < numPointLights && i < 32; i++) {
        result += calculatePointLight(i, input.fragPos, normal, viewDir) * objectColor;
    }
    
    return float4(result, 1.0);
}

