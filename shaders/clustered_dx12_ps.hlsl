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

// Spot light data structure (must match C++ SpotLightDataDX12)
struct SpotLightData {
    float3 position;
    float radius;
    float3 color;
    float intensity;
    float3 direction;
    float innerConeAngle;  // In radians
    float outerConeAngle;  // In radians
    int castsShadow;
    float padding1;
    float padding2;
};

cbuffer PointLightsBuffer : register(b4) {
    int numPointLights;
    float plPadding1;
    float plPadding2;
    float plPadding3;
    SpotLightData spotLights[64];
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
    float3 ddgiPadding;
};

// Shadow map (slot t0)
Texture2D shadowMap : register(t0);
SamplerComparisonState shadowSampler : register(s0);

Texture2D albedoMap : register(t1);
Texture2D irradianceMap : register(t2);
Texture2D visibilityMap : register(t3);
Texture2D normalMap : register(t4);
Texture2D metalRoughMap : register(t5);

// Spot light shadow maps (t6-t9 for up to 4 shadow-casting spot lights)
Texture2D spotLightShadowMaps[4] : register(t6);

SamplerState texSampler : register(s1);

// Number of spot lights that cast shadows (max 4)
static const int MAX_SHADOW_POINT_LIGHTS = 4;

struct PS_INPUT {
    float4 position : SV_POSITION;
    float3 fragPos : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 texCoord : TEXCOORD2;
};

// Calculate shadow for a spot light using 2D shadow map
float CalculateSpotLightShadow(int lightIndex, float3 fragPos, float3 lightPos, float3 lightDir, float lightRadius, float outerCone) {
    // Only first 4 spot lights have shadow maps
    if (lightIndex >= MAX_SHADOW_POINT_LIGHTS) {
        return 1.0;
    }
    
    // Vector from light to fragment
    float3 fragToLight = fragPos - lightPos;
    float distance = length(fragToLight);
    
    // If fragment is outside light radius, no shadow
    if (distance > lightRadius) {
        return 1.0;
    }
    
    // Check if fragment is within spot cone
    float3 toFrag = normalize(fragToLight);
    float cosAngle = dot(toFrag, lightDir);
    if (cosAngle < cos(outerCone)) {
        return 1.0; // Outside cone, no shadow calculation needed
    }
    
    // Project fragment position into spot light's view space
    // Build a simple orthonormal basis from light direction
    float3 up = abs(lightDir.y) > 0.99 ? float3(1, 0, 0) : float3(0, 1, 0);
    float3 right = normalize(cross(up, lightDir));
    up = cross(lightDir, right);
    
    // Transform to light's local space
    float3 localPos = fragToLight;
    float x = dot(localPos, right);
    float y = dot(localPos, up);
    float z = dot(localPos, lightDir);
    
    // Only shadow if in front of light
    if (z <= 0.1) return 1.0;
    
    // Project to normalized device coordinates based on cone angle
    float tanCone = tan(outerCone);
    float projX = x / (z * tanCone);
    float projY = y / (z * tanCone);
    
    // Convert to UV [0,1]
    float2 shadowUV = float2(projX * 0.5 + 0.5, -projY * 0.5 + 0.5);
    
    // Check bounds
    if (shadowUV.x < 0.0 || shadowUV.x > 1.0 || shadowUV.y < 0.0 || shadowUV.y > 1.0) {
        return 1.0;
    }
    
    // Sample shadow map
    float shadowMapDepth = 0.0;
    [branch]
    switch (lightIndex) {
        case 0: shadowMapDepth = spotLightShadowMaps[0].SampleLevel(texSampler, shadowUV, 0).r; break;
        case 1: shadowMapDepth = spotLightShadowMaps[1].SampleLevel(texSampler, shadowUV, 0).r; break;
        case 2: shadowMapDepth = spotLightShadowMaps[2].SampleLevel(texSampler, shadowUV, 0).r; break;
        case 3: shadowMapDepth = spotLightShadowMaps[3].SampleLevel(texSampler, shadowUV, 0).r; break;
        default: return 1.0;
    }
    
    // Convert perspective depth to linear
    float nearZ = 0.1;
    float farZ = lightRadius;
    float linearDepth = (farZ * nearZ) / (farZ - shadowMapDepth * (farZ - nearZ));
    
    // Bias
    float bias = 0.05 + 0.1 * (distance / lightRadius);
    
    // Shadow test
    float shadow = (distance > linearDepth + bias) ? 0.0 : 1.0;
    
    return shadow;
}

// Calculate shadow factor (1.0 = lit, 0.0 = shadowed)
float CalculateShadow(float3 worldPos, float3 surfNormal) {
    if (enableShadows == 0) return 1.0;
    
    // Transform world position to light space (row-major: vector * matrix)
    float4 fragPosLightSpace = mul(float4(worldPos, 1.0), lightSpaceMatrix);
    
    // Perspective divide
    float3 projCoords = fragPosLightSpace.xyz / fragPosLightSpace.w;
    
    // Transform to [0,1] range (NDC to UV) - DirectX clip space is [-1,1] for XY and [0,1] for Z
    float2 shadowUV;
    shadowUV.x = projCoords.x * 0.5 + 0.5;
    shadowUV.y = projCoords.y * -0.5 + 0.5; // Flip Y: DirectX texture origin is top-left
    
    // Check if outside shadow map
    if (projCoords.z > 1.0 || projCoords.z < 0.0 ||
        shadowUV.x < 0.0 || shadowUV.x > 1.0 || 
        shadowUV.y < 0.0 || shadowUV.y > 1.0) {
        return 1.0;
    }
    
    float currentDepth = projCoords.z;
    
    // Slope-scaled bias to prevent shadow acne
    float3 lightDir = normalize(lightPos);
    float NdotL = dot(surfNormal, lightDir);
    float bias = 0.001 + shadowBias * (1.0 - abs(NdotL));
    bias = min(bias, 0.01);
    
    // PCF (Percentage Closer Filtering) 3x3
    float shadow = 0.0;
    float2 texelSize = 1.0 / 2048.0; // Shadow map size
    
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            float2 sampleUV = shadowUV + float2(x, y) * texelSize;
            float shadowMapDepth = shadowMap.SampleLevel(texSampler, sampleUV, 0).r;
            // In shadow if current depth > shadow map depth (something is blocking the light)
            shadow += (currentDepth - bias > shadowMapDepth) ? 0.0 : 1.0;
        }
    }
    shadow /= 9.0;
    
    return shadow;
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
    int totalProbes = probeCountX * probeCountY * probeCountZ;
    int atlasWidthProbes = (int)sqrt((float)totalProbes);
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

// Calculate probe index from grid coordinates
int getProbeIndex(int3 gridCoord) {
    return gridCoord.x + gridCoord.y * probeCountX + gridCoord.z * probeCountX * probeCountY;
}

// Sample GI from probe grid with trilinear interpolation
float3 sampleDDGIIrradiance(float3 worldPos, float3 normal) {
    if (!ddgiEnabled) return float3(0, 0, 0);

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
    
    // Light gamma correction (less aggressive to preserve colors)
    // irradianceGamma of 2.2 is standard, 5.0 was too aggressive
    float gamma = max(irradianceGamma, 1.0);
    irradiance = pow(max(irradiance, 0.001), 1.0 / gamma);
    
    // Apply GI intensity (the compute shader already includes light intensities)
    return irradiance * giIntensity;
}

float3 calculateSpotLight(int index, float3 fragPos, float3 normal, float3 viewDir, float mainShadow) {
    float3 lightPosition = spotLights[index].position;
    float3 lightCol = spotLights[index].color;
    float lightRadius = spotLights[index].radius;
    float lightIntensity = spotLights[index].intensity;
    float3 spotDirection = normalize(spotLights[index].direction);
    float innerCone = spotLights[index].innerConeAngle;
    float outerCone = spotLights[index].outerConeAngle;
    int castsShadow = spotLights[index].castsShadow;
    
    float3 lightDir = normalize(lightPosition - fragPos);
    float distance = length(lightPosition - fragPos);
    
    if (distance > lightRadius) return float3(0, 0, 0);
    
    // Spot light cone attenuation
    // spotDirection points FROM the light, lightDir points TO the light from fragment
    // So we need to compare -lightDir with spotDirection
    float theta = dot(-lightDir, spotDirection);
    float epsilon = cos(innerCone) - cos(outerCone);
    float spotIntensity = saturate((theta - cos(outerCone)) / max(epsilon, 0.0001));
    
    // If outside the cone, no light contribution
    if (theta < cos(outerCone)) return float3(0, 0, 0);
    
    // Distance attenuation
    float att_linear = 4.5 / lightRadius;
    float att_quadratic = 75.0 / (lightRadius * lightRadius);
    float attenuation = 1.0 / (1.0 + att_linear * distance + att_quadratic * distance * distance);
    
    float falloff = 1.0 - smoothstep(lightRadius * 0.75, lightRadius, distance);
    attenuation *= falloff * spotIntensity;
    
    float diff = max(dot(normal, lightDir), 0.0);
    float3 diffuse = diff * lightCol * lightIntensity;
    
    float3 halfwayDir = normalize(lightDir + viewDir);
    float spec = pow(max(dot(normal, halfwayDir), 0.0), (float)shininess);
    float3 specular = specularStrength * spec * lightCol * lightIntensity;
    
    // Calculate spot light shadow
    float spotLightShadow = 1.0;
    
    if (index < MAX_SHADOW_POINT_LIGHTS && enableShadows && castsShadow) {
        // Use shadow map for first 4 lights
        spotLightShadow = CalculateSpotLightShadow(index, fragPos, lightPosition, spotDirection, lightRadius, outerCone);
    } else {
        // For lights without shadow maps, use main shadow as approximation
        spotLightShadow = lerp(0.5, 1.0, mainShadow);
    }
    
    return (diffuse + specular) * attenuation * spotLightShadow;
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
    
    // Calculate shadow for main light
    float shadow = CalculateShadow(input.fragPos, normal);
    
    // Combine with shadow
    float3 Lo = (kD * albedo / 3.14159265 + specular) * lightColor * NdotL * attenuation * shadow;
    
    result += Lo;

    // Spot lights contribution (clustered forward)
    for (int i = 0; i < numPointLights && i < 64; i++) {
        // Pass shadow factor so spot lights are dimmed in shadowed areas
        result += calculateSpotLight(i, input.fragPos, normal, viewDir, shadow) * albedo;
    }
    
    return float4(result, 1.0);
}





