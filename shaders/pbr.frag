#version 330 core
out vec4 FragColor;

in VS_OUT {
    vec3 FragPos;
    vec3 Normal;
    vec2 TexCoords;
    vec4 FragPosLightSpace;
    vec4 FragPosLight2Space;
    mat3 TBN;
} fs_in;

uniform sampler2D shadowMap;
uniform sampler2D shadowMap2;
uniform samplerCube skybox;

// Material textures
struct MaterialMaps {
    sampler2D albedoMap;
    sampler2D normalMap;
    sampler2D metallicMap;
    sampler2D roughnessMap;
    sampler2D aoMap;
    
    bool hasAlbedoMap;
    bool hasNormalMap;
    bool hasMetallicMap;
    bool hasRoughnessMap;
    bool hasAOMap;
};
uniform MaterialMaps material;

uniform vec3 lightPos;
uniform vec3 viewPos;
uniform vec3 objectColor;

// Light properties
uniform int lightType;
uniform float constant;
uniform float linear;
uniform float quadratic;

// Second light
uniform bool enableSecondLight;
uniform vec3 light2Pos;
uniform vec3 light2Color;
uniform float light2Intensity;

// Skybox lighting
uniform bool enableSkyboxLighting;
uniform float skyboxLightIntensity;

// Ambient Occlusion
uniform bool enableAO;
uniform float aoStrength;
uniform float aoPower;

// PBR Material properties
uniform bool usePBR;
uniform float metallic;
uniform float roughness;
uniform float ao;

// Shadow parameters
uniform float shadowBias;
uniform bool enableShadows;

const float PI = 3.14159265359;

// Shadow calculation functions
float ShadowCalculation(vec4 fragPosLightSpace)
{
    if (!enableShadows) return 0.0;
    
    vec3 projCoords = fragPosLightSpace.xyz / fragPosLightSpace.w;
    projCoords = projCoords * 0.5 + 0.5;
    float closestDepth = texture(shadowMap, projCoords.xy).r; 
    float currentDepth = projCoords.z;
    float shadow = currentDepth - shadowBias > closestDepth  ? 1.0 : 0.0;
    if(projCoords.z > 1.0)
        shadow = 0.0;
    return shadow;
}

float ShadowCalculation2(vec4 fragPosLight2Space)
{
    if (!enableShadows) return 0.0;
    
    vec3 projCoords = fragPosLight2Space.xyz / fragPosLight2Space.w;
    projCoords = projCoords * 0.5 + 0.5;
    float closestDepth = texture(shadowMap2, projCoords.xy).r; 
    float currentDepth = projCoords.z;
    float shadow = currentDepth - shadowBias > closestDepth ? 1.0 : 0.0;
    if(projCoords.z > 1.0)
        shadow = 0.0;
    return shadow;
}

// PBR Functions
float DistributionGGX(vec3 N, vec3 H, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;
    
    float nom = a2;
    float denom = (NdotH2 * (a2 - 1.0) + 1.0);
    denom = PI * denom * denom;
    
    return nom / denom;
}

float GeometrySchlickGGX(float NdotV, float roughness)
{
    float r = (roughness + 1.0);
    float k = (r * r) / 8.0;
    
    float nom = NdotV;
    float denom = NdotV * (1.0 - k) + k;
    
    return nom / denom;
}

float GeometrySmith(vec3 N, vec3 V, vec3 L, float roughness)
{
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float ggx2 = GeometrySchlickGGX(NdotV, roughness);
    float ggx1 = GeometrySchlickGGX(NdotL, roughness);
    
    return ggx1 * ggx2;
}

vec3 fresnelSchlick(float cosTheta, vec3 F0)
{
    return F0 + (1.0 - F0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

void main()
{           
    // Sample material textures
    vec3 albedo = material.hasAlbedoMap ? texture(material.albedoMap, fs_in.TexCoords).rgb : objectColor;
    float metallicValue = material.hasMetallicMap ? texture(material.metallicMap, fs_in.TexCoords).r : metallic;
    float roughnessValue = material.hasRoughnessMap ? texture(material.roughnessMap, fs_in.TexCoords).r : roughness;
    float aoValue = material.hasAOMap ? texture(material.aoMap, fs_in.TexCoords).r : ao;
    
    // Get normal from normal map or use vertex normal
    vec3 normal;
    if (material.hasNormalMap) {
        normal = texture(material.normalMap, fs_in.TexCoords).rgb;
        normal = normal * 2.0 - 1.0;  // Transform from [0,1] to [-1,1]
        normal = normalize(fs_in.TBN * normal);
    } else {
        normal = normalize(fs_in.Normal);
    }
    
    vec3 viewDir = normalize(viewPos - fs_in.FragPos);
    
    // Ambient Occlusion (angle-based)
    float aoFactor = 1.0;
    if (enableAO) {
        float ndotv = max(dot(normal, viewDir), 0.0);
        aoFactor = pow(ndotv, aoPower);
        aoFactor = mix(1.0, aoFactor, aoStrength);
    }
    
    vec3 Lo = vec3(0.0);
    
    if (usePBR) {
        // PBR Workflow
        // Calculate reflectance at normal incidence
        vec3 F0 = vec3(0.04); 
        F0 = mix(F0, albedo, metallicValue);
        
        // === FIRST LIGHT (PBR) ===
        vec3 L = normalize(lightPos - fs_in.FragPos);
        vec3 H = normalize(viewDir + L);
        float distance = length(lightPos - fs_in.FragPos);
        float attenuation = 1.0;
        
        if (lightType == 1) {
            // Point light attenuation
            attenuation = 1.0 / (constant + linear * distance + quadratic * (distance * distance));
        }
        
        vec3 radiance = vec3(1.0) * attenuation;
        
        // Cook-Torrance BRDF
        float NDF = DistributionGGX(normal, H, roughnessValue);
        float G = GeometrySmith(normal, viewDir, L, roughnessValue);
        vec3 F = fresnelSchlick(max(dot(H, viewDir), 0.0), F0);
        
        vec3 numerator = NDF * G * F;
        float denominator = 4.0 * max(dot(normal, viewDir), 0.0) * max(dot(normal, L), 0.0) + 0.0001;
        vec3 specular = numerator / denominator;
        
        vec3 kS = F;
        vec3 kD = vec3(1.0) - kS;
        kD *= 1.0 - metallicValue;
        
        float NdotL = max(dot(normal, L), 0.0);
        
        // Shadow
        float shadow = ShadowCalculation(fs_in.FragPosLightSpace);
        
        Lo += (kD * albedo / PI + specular) * radiance * NdotL * (1.0 - shadow);
        
        // === SECOND LIGHT (PBR) ===
        if (enableSecondLight) {
            L = normalize(light2Pos - fs_in.FragPos);
            H = normalize(viewDir + L);
            distance = length(light2Pos - fs_in.FragPos);
            attenuation = 1.0 / (1.0 + 0.09 * distance + 0.032 * (distance * distance));
            
            radiance = light2Color * light2Intensity * attenuation;
            
            NDF = DistributionGGX(normal, H, roughnessValue);
            G = GeometrySmith(normal, viewDir, L, roughnessValue);
            F = fresnelSchlick(max(dot(H, viewDir), 0.0), F0);
            
            numerator = NDF * G * F;
            denominator = 4.0 * max(dot(normal, viewDir), 0.0) * max(dot(normal, L), 0.0) + 0.0001;
            specular = numerator / denominator;
            
            kS = F;
            kD = vec3(1.0) - kS;
            kD *= 1.0 - metallicValue;
            
            NdotL = max(dot(normal, L), 0.0);
            
            float shadow2 = ShadowCalculation2(fs_in.FragPosLight2Space);
            
            Lo += (kD * albedo / PI + specular) * radiance * NdotL * (1.0 - shadow2);
        }
        
        // Ambient lighting
        vec3 ambient = vec3(0.03) * albedo * aoValue;
        
        // Skybox ambient
        if (enableSkyboxLighting) {
            vec3 skyColor = texture(skybox, normal).rgb;
            ambient += skyColor * skyboxLightIntensity * albedo * aoValue;
        }
        
        ambient *= aoFactor;
        
        vec3 finalColor = ambient + Lo;
        
        // HDR tonemapping (simple Reinhard)
        finalColor = finalColor / (finalColor + vec3(1.0));
        
        // Gamma correction
        finalColor = pow(finalColor, vec3(1.0/2.2));
        
        FragColor = vec4(finalColor, 1.0);
        
    } else {
        // Original Blinn-Phong lighting (fallback)
        vec3 lightColor = vec3(1.0);
        vec3 ambient = vec3(0.15) * albedo;
        
        if (enableSkyboxLighting) {
            vec3 skyColor = texture(skybox, normal).rgb;
            ambient += skyColor * skyboxLightIntensity;
        }
        
        ambient *= aoFactor;
        
        // First light
        vec3 lightDir;
        float attenuation = 1.0;
        
        if (lightType == 0) {
            lightDir = normalize(lightPos - fs_in.FragPos);
        } else {
            lightDir = normalize(lightPos - fs_in.FragPos);
            float distance = length(lightPos - fs_in.FragPos);
            attenuation = 1.0 / (constant + linear * distance + quadratic * (distance * distance));
        }
        
        float diff = max(dot(lightDir, normal), 0.0);
        vec3 diffuse = diff * lightColor;
        
        vec3 halfwayDir = normalize(lightDir + viewDir);  
        float spec = pow(max(dot(normal, halfwayDir), 0.0), 32);
        vec3 specular = 0.5 * spec * lightColor;
        
        float shadow = ShadowCalculation(fs_in.FragPosLightSpace);       
        vec3 lighting = (ambient + (1.0 - shadow) * (diffuse + specular)) * attenuation;
        
        // Second light
        if (enableSecondLight) {
            vec3 light2Dir = normalize(light2Pos - fs_in.FragPos);
            float light2Distance = length(light2Pos - fs_in.FragPos);
            float light2Attenuation = 1.0 / (1.0 + 0.09 * light2Distance + 0.032 * (light2Distance * light2Distance));
            
            float light2Diff = max(dot(light2Dir, normal), 0.0);
            vec3 light2Diffuse = light2Diff * light2Color * light2Intensity;
            
            vec3 light2HalfwayDir = normalize(light2Dir + viewDir);
            float light2Spec = pow(max(dot(normal, light2HalfwayDir), 0.0), 32);
            vec3 light2Specular = 0.5 * light2Spec * light2Color * light2Intensity;
            
            float shadow2 = ShadowCalculation2(fs_in.FragPosLight2Space);
            
            lighting += (1.0 - shadow2) * (light2Diffuse + light2Specular) * light2Attenuation;
        }
        
        FragColor = vec4(lighting * albedo, 1.0);
    }
}
