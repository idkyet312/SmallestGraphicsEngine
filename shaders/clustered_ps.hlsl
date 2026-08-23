// Clustered Forward Pixel Shader - DX11 HLSL with DDGI

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
    // Cone axis, unit length; zero for a plain omnidirectional point light.
    float3 spotDirection;
    float spotCosInner;
    float spotCosOuter;
    int spotShadowIndex;
    float2 spotPadding;
};

cbuffer PointLightsBuffer : register(b4) {
    int numPointLights;
    // Live spot atlas slices this frame; see PointLightsBufferDX12.
    int spotShadowCount;
    float plPadding2;
    float plPadding3;
    PointLightData pointLights[64];
    float4x4 spotShadowMatrices[3];
};

// DDGI Constant Buffer
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
    float ddgiPadding[3];
};

Texture2D shadowMap : register(t0);
// Spot shadow atlas: one slice per shadow-casting spot light (vehicle
// headlights, enemy helicopter searchlights). Not the player flashlight.
Texture2DArray<float> spotShadowAtlas : register(t21);
Texture2D ddgiIrradiance : register(t2);
Texture2D ddgiVisibility : register(t3);

SamplerComparisonState shadowSampler : register(s0);
SamplerState regularSampler : register(s1);

// Occlusion for one shadow-casting spot light.
//
// Returns 1 where the light reaches and 0 where something blocks it. A light
// with no slice, or one pointing at a slice that holds stale depth this frame,
// returns 1 so it simply behaves as the unshadowed light it was before.
//
// The frustum here is perspective, unlike the sun cascades, so the projective
// divide is real work rather than a no-op: w carries distance from the lamp.
float SpotShadowVisibility(int shadowIndex, float3 worldPos) {
    if (shadowIndex < 0 || shadowIndex >= spotShadowCount) return 1.0;

    float4 lightClip = mul(float4(worldPos, 1.0), spotShadowMatrices[shadowIndex]);
    if (lightClip.w <= 0.0) return 1.0;
    float3 proj = lightClip.xyz / lightClip.w;
    // Outside the rendered cone: the caster frustum is deliberately wider than
    // the lit cone, so anything landing out here is past the beam edge anyway
    // and the cone falloff has already taken it to zero.
    if (any(abs(proj.xy) > 1.0) || proj.z < 0.0 || proj.z > 1.0) return 1.0;

    float2 uv = proj.xy * float2(0.5, -0.5) + 0.5;

    // Slope-independent constant bias scaled by distance from the lamp. A flat
    // constant that clears acne on the ground right under a headlight leaves
    // peter-panning at the far end of the beam, where one texel covers far more
    // world; tying it to w spreads that cost across the range instead.
    float bias = 0.0015 + 0.0025 * saturate(lightClip.w / 40.0);

    uint atlasWidth, atlasHeight, atlasSlices;
    spotShadowAtlas.GetDimensions(atlasWidth, atlasHeight, atlasSlices);
    float2 texel = 1.0 / float2(atlasWidth, atlasHeight);

    // 2x2 rotated-grid PCF. Cheaper than the sun's kernel on purpose: these are
    // small local pools, and a headlight edge that is a little crisp reads as a
    // headlight rather than as an error.
    float visibility = 0.0;
    [unroll]
    for (int y = -1; y <= 1; y += 2) {
        [unroll]
        for (int x = -1; x <= 1; x += 2) {
            float2 offset = float2(x, y) * texel * 0.75;
            visibility += spotShadowAtlas.SampleCmpLevelZero(
                shadowSampler, float3(uv + offset, (float)shadowIndex),
                proj.z - bias);
        }
    }
    return visibility * 0.25;
}

struct PS_INPUT {
    float4 position : SV_POSITION;
    float3 fragPos : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float4 fragPosLightSpace : TEXCOORD2;
};

// DDGI Helper functions
int3 getProbeGridCoord(float3 worldPos) {
    float3 offset = worldPos - probeGridOrigin;
    return int3(
        clamp((int)(offset.x / probeSpacing), 0, probeCountX - 1),
        clamp((int)(offset.y / probeSpacing), 0, probeCountY - 1),
        clamp((int)(offset.z / probeSpacing), 0, probeCountZ - 1)
    );
}

float3 getProbeWorldPosition(int3 probeCoord) {
    return probeGridOrigin + float3(probeCoord) * probeSpacing;
}

int getProbeIndex(int3 probeCoord) {
    return probeCoord.x + probeCoord.z * probeCountX + probeCoord.y * probeCountX * probeCountZ;
}

float2 getProbeIrradianceUV(int3 probeCoord, float3 direction) {
    // Octahedral mapping for direction
    float2 octDir = direction.xy / (abs(direction.x) + abs(direction.y) + abs(direction.z));
    if (direction.z < 0.0) {
        octDir = (1.0 - abs(octDir.yx)) * sign(octDir);
    }
    octDir = octDir * 0.5 + 0.5;
    
    // Calculate probe's position in the atlas
    int probesPerRow = probeCountX * probeCountZ;
    int probeIdx = probeCoord.x + probeCoord.z * probeCountX;
    int probeRow = probeCoord.y;
    
    float2 atlasOffset = float2(
        (float)(probeIdx * irradianceTexWidth),
        (float)(probeRow * irradianceTexHeight)
    );
    
    float totalWidth = (float)(probeCountX * probeCountZ * irradianceTexWidth);
    float totalHeight = (float)(probeCountY * irradianceTexHeight);
    
    float2 uv = (atlasOffset + octDir * float2(irradianceTexWidth, irradianceTexHeight)) / float2(totalWidth, totalHeight);
    return uv;
}

float3 sampleDDGIIrradiance(float3 worldPos, float3 normal) {
    if (ddgiEnabled == 0) return float3(0, 0, 0);
    
    // Get the 8 surrounding probes
    float3 offset = worldPos - probeGridOrigin;
    float3 gridPos = offset / probeSpacing;
    
    int3 baseCoord = int3(floor(gridPos));
    baseCoord = clamp(baseCoord, int3(0, 0, 0), int3(probeCountX - 1, probeCountY - 1, probeCountZ - 1));
    
    float3 alpha = frac(gridPos);
    
    float3 totalIrradiance = float3(0, 0, 0);
    float totalWeight = 0.0;
    
    // Trilinear interpolation over 8 probes
    for (int i = 0; i < 8; i++) {
        int3 probeOffset = int3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        int3 probeCoord = baseCoord + probeOffset;
        
        // Clamp to valid range
        probeCoord = clamp(probeCoord, int3(0, 0, 0), int3(probeCountX - 1, probeCountY - 1, probeCountZ - 1));
        
        float3 probePos = getProbeWorldPosition(probeCoord);
        float3 probeToPoint = worldPos - probePos;
        float3 dir = normalize(probeToPoint);
        
        // Trilinear weights
        float3 triWeights = lerp(1.0 - alpha, alpha, float3(probeOffset));
        float weight = triWeights.x * triWeights.y * triWeights.z;
        
        // Backface test - reduce weight for probes behind the surface
        float backfaceWeight = max(0.0001, dot(dir, normal) * 0.5 + 0.5);
        weight *= backfaceWeight;
        
        // Sample irradiance
        float2 uv = getProbeIrradianceUV(probeCoord, normal);
        float3 irradiance = ddgiIrradiance.SampleLevel(regularSampler, uv, 0).rgb;
        
        // Apply gamma correction
        irradiance = pow(max(irradiance, 0.0), 1.0 / irradianceGamma);
        
        totalIrradiance += irradiance * weight;
        totalWeight += weight;
    }
    
    if (totalWeight > 0.0) {
        totalIrradiance /= totalWeight;
    }
    
    return totalIrradiance * giIntensity;
}

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
    
    // Spotlight cone; a zero direction keeps this an ordinary point light.
    float3 spotDir = pointLights[index].spotDirection;
    if (dot(spotDir, spotDir) > 0.0001) {
        float cosAngle = dot(-lightDir, normalize(spotDir));
        attenuation *= smoothstep(pointLights[index].spotCosOuter,
                                  pointLights[index].spotCosInner, cosAngle);
        // Occlusion, for the casters that have an atlas slice. Only worth the
        // taps where the cone still contributes, so it sits inside the cone
        // test and after the falloff has had its say.
        if (attenuation > 0.0)
            attenuation *= SpotShadowVisibility(
                pointLights[index].spotShadowIndex, fragPos);
    }

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
    
    // Ambient from DDGI or fallback
    float3 ambient = ambientStrength * objectColor;
    
    // Sample DDGI for global illumination
    float3 giContribution = sampleDDGIIrradiance(input.fragPos, normal);
    ambient += giContribution * objectColor;
    
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
    for (int i = 0; i < numPointLights && i < 64; i++) {
        result += calculatePointLight(i, input.fragPos, normal, viewDir) * objectColor;
    }

    // Tonemap to avoid harsh clipping/hue-shift when many lights overlap
    result = result / (result + 1.0);

    return float4(result, 1.0);
}
