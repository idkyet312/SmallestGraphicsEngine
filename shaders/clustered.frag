#version 330 core
out vec4 FragColor;

in VS_OUT {
    vec3 FragPos;
    vec3 Normal;
    vec4 FragPosLightSpace;
} fs_in;

// Shadow mapping
uniform sampler2D shadowMap;

// Point lights array (simple approach - max 32 lights)
#define MAX_POINT_LIGHTS 32
uniform int numPointLights;
uniform vec3 pointLightPositions[MAX_POINT_LIGHTS];
uniform vec3 pointLightColors[MAX_POINT_LIGHTS];
uniform float pointLightRadii[MAX_POINT_LIGHTS];
uniform float pointLightIntensities[MAX_POINT_LIGHTS];

// Camera
uniform vec3 viewPos;

// Main light (directional/point with shadows)
uniform vec3 lightPos;
uniform int lightType; // 0 = directional, 1 = point
uniform vec3 lightColor;
uniform float constant;
uniform float linear;
uniform float quadratic;

// Material/Object
uniform vec3 objectColor;
uniform float ambientStrength;
uniform float specularStrength;
uniform int shininess;

// Shadow
uniform float shadowBias;
uniform bool enableShadows;

float ShadowCalculation(vec4 fragPosLightSpace)
{
    if (!enableShadows) return 0.0;
    
    vec3 projCoords = fragPosLightSpace.xyz / fragPosLightSpace.w;
    projCoords = projCoords * 0.5 + 0.5;
    
    if(projCoords.z > 1.0) return 0.0;
    
    float closestDepth = texture(shadowMap, projCoords.xy).r; 
    float currentDepth = projCoords.z;
    
    // PCF soft shadows
    float shadow = 0.0;
    vec2 texelSize = 1.0 / textureSize(shadowMap, 0);
    for(int x = -1; x <= 1; ++x) {
        for(int y = -1; y <= 1; ++y) {
            float pcfDepth = texture(shadowMap, projCoords.xy + vec2(x, y) * texelSize).r;
            shadow += currentDepth - shadowBias > pcfDepth ? 1.0 : 0.0;
        }
    }
    shadow /= 9.0;
    
    return shadow;
}

// Calculate lighting from a point light
vec3 calculatePointLight(int index, vec3 fragPos, vec3 normal, vec3 viewDir) {
    vec3 lightPosition = pointLightPositions[index];
    vec3 lightCol = pointLightColors[index];
    float lightRadius = pointLightRadii[index];
    float lightIntensity = pointLightIntensities[index];
    
    vec3 lightDir = normalize(lightPosition - fragPos);
    float distance = length(lightPosition - fragPos);
    
    // Skip if outside light radius
    if (distance > lightRadius) return vec3(0.0);
    
    // Attenuation based on radius
    float att_linear = 4.5 / lightRadius;
    float att_quadratic = 75.0 / (lightRadius * lightRadius);
    float attenuation = 1.0 / (1.0 + att_linear * distance + att_quadratic * distance * distance);
    
    // Smooth falloff at edge of radius
    float falloff = 1.0 - smoothstep(lightRadius * 0.75, lightRadius, distance);
    attenuation *= falloff;
    
    // Diffuse
    float diff = max(dot(normal, lightDir), 0.0);
    vec3 diffuse = diff * lightCol * lightIntensity;
    
    // Specular (Blinn-Phong)
    vec3 halfwayDir = normalize(lightDir + viewDir);
    float spec = pow(max(dot(normal, halfwayDir), 0.0), float(shininess));
    vec3 specular = specularStrength * spec * lightCol * lightIntensity;
    
    return (diffuse + specular) * attenuation;
}

void main()
{
    vec3 color = objectColor;
    vec3 normal = normalize(fs_in.Normal);
    vec3 viewDir = normalize(viewPos - fs_in.FragPos);
    
    // Ambient lighting
    vec3 ambient = ambientStrength * lightColor;
    
    // Main directional/point light contribution
    vec3 mainLightDir;
    float mainAttenuation = 1.0;
    
    if (lightType == 0) {
        // Directional light
        mainLightDir = normalize(lightPos - fs_in.FragPos);
    } else {
        // Point light with attenuation
        mainLightDir = normalize(lightPos - fs_in.FragPos);
        float distance = length(lightPos - fs_in.FragPos);
        mainAttenuation = 1.0 / (constant + linear * distance + quadratic * distance * distance);
    }
    
    float mainDiff = max(dot(mainLightDir, normal), 0.0);
    vec3 mainDiffuse = mainDiff * lightColor;
    
    vec3 mainHalfway = normalize(mainLightDir + viewDir);
    float mainSpec = pow(max(dot(normal, mainHalfway), 0.0), float(shininess));
    vec3 mainSpecular = specularStrength * mainSpec * lightColor;
    
    // Shadow from main light
    float shadow = ShadowCalculation(fs_in.FragPosLightSpace);
    vec3 mainLighting = (1.0 - shadow) * (mainDiffuse + mainSpecular) * mainAttenuation;
    
    // Additional point lights contribution
    vec3 pointLighting = vec3(0.0);
    for (int i = 0; i < numPointLights && i < MAX_POINT_LIGHTS; i++) {
        pointLighting += calculatePointLight(i, fs_in.FragPos, normal, viewDir);
    }
    
    // Combine all lighting
    vec3 finalLighting = ambient + mainLighting + pointLighting;
    vec3 result = finalLighting * color;
    
    FragColor = vec4(result, 1.0);
}

