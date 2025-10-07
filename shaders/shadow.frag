#version 330 core
out vec4 FragColor;

in VS_OUT {
    vec3 FragPos;
    vec3 Normal;
    vec4 FragPosLightSpace;
    vec4 FragPosLight2Space;
} fs_in;

uniform sampler2D shadowMap;
uniform sampler2D shadowMap2;
uniform samplerCube skybox;

uniform vec3 lightPos;
uniform vec3 viewPos;
uniform vec3 objectColor;

// Light properties
uniform int lightType; // 0 = directional, 1 = point
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

// Lighting parameters
uniform float ambientStrength;
uniform float specularStrength;
uniform int shininess;
uniform float shadowBias;
uniform bool enableShadows;

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

float ShadowCalculation2(vec4 fragPosLightSpace)
{
    if (!enableShadows || !enableSecondLight) return 0.0;
    
    vec3 projCoords = fragPosLightSpace.xyz / fragPosLightSpace.w;
    projCoords = projCoords * 0.5 + 0.5;
    float closestDepth = texture(shadowMap2, projCoords.xy).r; 
    float currentDepth = projCoords.z;
    float shadow = currentDepth - shadowBias > closestDepth  ? 1.0 : 0.0;
    if(projCoords.z > 1.0)
        shadow = 0.0;
    return shadow;
}

void main()
{           
    vec3 color = objectColor;
    vec3 normal = normalize(fs_in.Normal);
    vec3 lightColor = vec3(1.0);
    
    // Ambient - can be enhanced by skybox
    vec3 ambient = ambientStrength * lightColor;
    
    // Add skybox ambient lighting (sample skybox in normal direction)
    if (enableSkyboxLighting) {
        vec3 skyColor = texture(skybox, normal).rgb;
        ambient += skyColor * skyboxLightIntensity;
    }
    
    // === AMBIENT OCCLUSION (angle-based) ===
    float ao = 1.0;
    if (enableAO) {
        // Calculate AO based on angle between normal and view direction
        vec3 viewDir = normalize(viewPos - fs_in.FragPos);
        float ndotv = max(dot(normal, viewDir), 0.0);
        
        // Surfaces facing away from camera are more occluded
        // Use power function to control falloff
        ao = pow(ndotv, aoPower);
        
        // Mix between full AO and no AO based on strength
        ao = mix(1.0, ao, aoStrength);
    }
    
    // Apply AO to ambient
    ambient *= ao;
    
    // === FIRST LIGHT ===
    vec3 lightDir;
    float attenuation = 1.0;
    
    if (lightType == 0) {
        // Directional light
        lightDir = normalize(lightPos - fs_in.FragPos);
    } else {
        // Point light with attenuation
        lightDir = normalize(lightPos - fs_in.FragPos);
        float distance = length(lightPos - fs_in.FragPos);
        attenuation = 1.0 / (constant + linear * distance + quadratic * (distance * distance));
    }
    
    float diff = max(dot(lightDir, normal), 0.0);
    vec3 diffuse = diff * lightColor;
    
    // Specular
    vec3 viewDir = normalize(viewPos - fs_in.FragPos);
    vec3 halfwayDir = normalize(lightDir + viewDir);  
    float spec = pow(max(dot(normal, halfwayDir), 0.0), shininess);
    vec3 specular = specularStrength * spec * lightColor;
    
    // Calculate shadow for first light
    float shadow = ShadowCalculation(fs_in.FragPosLightSpace);       
    vec3 lighting = (ambient + (1.0 - shadow) * (diffuse + specular)) * attenuation;
    
    // === SECOND LIGHT (with shadows) ===
    if (enableSecondLight) {
        vec3 light2Dir = normalize(light2Pos - fs_in.FragPos);
        float light2Distance = length(light2Pos - fs_in.FragPos);
        float light2Attenuation = 1.0 / (1.0 + 0.09 * light2Distance + 0.032 * (light2Distance * light2Distance));
        
        // Diffuse
        float light2Diff = max(dot(light2Dir, normal), 0.0);
        vec3 light2Diffuse = light2Diff * light2Color * light2Intensity;
        
        // Specular
        vec3 light2HalfwayDir = normalize(light2Dir + viewDir);
        float light2Spec = pow(max(dot(normal, light2HalfwayDir), 0.0), shininess);
        vec3 light2Specular = specularStrength * light2Spec * light2Color * light2Intensity;
        
        // Calculate shadow for second light
        float shadow2 = ShadowCalculation2(fs_in.FragPosLight2Space);
        
        lighting += (1.0 - shadow2) * (light2Diffuse + light2Specular) * light2Attenuation;
    }
    
    FragColor = vec4(lighting * color, 1.0);
}
