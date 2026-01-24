// Simple DX12 Pixel Shader

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

struct PS_INPUT {
    float4 position : SV_POSITION;
    float3 fragPos : TEXCOORD0;
    float3 normal : TEXCOORD1;
};

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
    
    // Ambient
    float3 ambient = ambientStrength * objectColor;
    float3 result = ambient;
    
    // Main directional light
    float3 lightDir = normalize(lightPos);
    float diff = max(dot(normal, lightDir), 0.0);
    float3 diffuse = diff * lightColor * objectColor;
    
    float3 halfwayDir = normalize(lightDir + viewDir);
    float spec = pow(max(dot(normal, halfwayDir), 0.0), (float)shininess);
    float3 specular = specularStrength * spec * lightColor;
    
    result += diffuse + specular;
    
    // Point lights
    for (int i = 0; i < numPointLights && i < 64; i++) {
        result += calculatePointLight(i, input.fragPos, normal, viewDir) * objectColor;
    }
    
    return float4(result, 1.0);
}

