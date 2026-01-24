// DDGI Update Compute Shader

#define PI 3.14159265359

// Constants
cbuffer DDGIBuffer : register(b0) {
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

struct PointLightData {
    float3 position;
    float radius;
    float3 color;
    float intensity;
};

cbuffer PointLightsBuffer : register(b1) {
    int numPointLights;
    float plPadding1;
    float plPadding2;
    float plPadding3;
    PointLightData pointLights[64];
};

cbuffer MainLightBuffer : register(b2) {
    float3 mainLightPos;
    int mainLightType;             // 0=Point, 1=Directional
    
    float3 mainLightColor;
    float mainLightIntensity;      
    
    matrix lightSpaceMatrix; // For shadow mapping
    
    float shadowBias;
    int enableShadows;
    float padding1;
    float padding2;
};

RWTexture2D<float4> OutputIrradiance : register(u0);
Texture2D shadowMap : register(t0);
SamplerState shadowSampler : register(s0);

// Helper to get probe position from index
float3 GetProbePosition(int3 probeIndex) {
    return probeGridOrigin + float3(probeIndex) * probeSpacing;
}

// Octahedral mapping helper (vector to UV)
float2 OctEncode(float3 n) {
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    if (n.y < 0) {
        float2 signN = float2(n.x >= 0 ? 1 : -1, n.z >= 0 ? 1 : -1);
        n.xz = (1.0 - abs(n.zx)) * signN;
    }
    return n.xz * 0.5 + 0.5;
}

// Inverse Octahedral mapping (UV to vector)
float3 OctDecode(float2 f) {
    f = f * 2.0 - 1.0;
    float3 n = float3(f.x, 1.0 - abs(f.x) - abs(f.y), f.y);
    float t = saturate(-n.y);
    n.xz += float2(n.x >= 0 ? -t : t, n.z >= 0 ? -t : t); // simplified
    // Correct version:
    // if (n.y < 0) { ... } but we constructed n such that |x|+|y|+|z|=1
    return normalize(n);
}

// Standard Decode
float3 DecodeOctahedralDir(float2 uv) {
    uv = uv * 2.0 - 1.0;
    float3 n = float3(uv.x, 1.0 - abs(uv.x) - abs(uv.y), uv.y);
    if (n.y < 0) {
        float2 signN = float2(n.x >= 0 ? 1 : -1, n.z >= 0 ? 1 : -1);
        n.xz = (1.0 - abs(n.zx)) * signN;
    }
    return normalize(n);
}

// Calculate shadow factor (1.0 = not shadowed, 0.0 = fully shadowed)
float CalculateShadow(float3 worldPos) {
    if (enableShadows == 0) return 1.0;

    float4 lightSpacePos = mul(lightSpaceMatrix, float4(worldPos, 1.0));
    float3 projCoords = lightSpacePos.xyz / lightSpacePos.w;
    
    // Transform to [0,1] range
    projCoords.x = projCoords.x * 0.5 + 0.5;
    projCoords.y = -projCoords.y * 0.5 + 0.5; // Flip Y for DX coordinates
    
    // Check if outside shadow map
    if (projCoords.z > 1.0 || projCoords.x < 0.0 || projCoords.x > 1.0 || projCoords.y < 0.0 || projCoords.y > 1.0)
        return 1.0;
        
    // Simple shadow check
    // Note: Shadow map stores linear depth or projected depth? 
    // Usually standard depth buffer is non-linear but "depth_ps.hlsl" writes nothing, so it's standard depth.
    
    float currentDepth = projCoords.z;
    float shadow = 0.0;
    
    // PCF (Percentage Closer Filtering) 3x3
    // Use texture dimensions if possible but hardcoded for now usually 2048
    float2 texelSize = 1.0 / 2048.0; 
    
    for(int x = -1; x <= 1; ++x) {
        for(int y = -1; y <= 1; ++y) {
            float pcfDepth = shadowMap.SampleLevel(shadowSampler, projCoords.xy + float2(x, y) * texelSize, 0).r;
            shadow += (currentDepth - shadowBias > pcfDepth) ? 0.0 : 1.0;
        }
    }
    shadow /= 9.0;
    
    return shadow;
}

// Pseudo-random number generator for ray direction jittering
float hash(float2 p) {
    return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453);
}

float3 hash3(float2 p) {
    float3 q = float3(dot(p, float2(127.1, 311.7)),
                      dot(p, float2(269.5, 183.3)),
                      dot(p, float2(419.2, 371.9)));
    return frac(sin(q) * 43758.5453);
}

// Generate a cosine-weighted hemisphere sample direction
float3 cosineSampleHemisphere(float2 u, float3 normal) {
    float r = sqrt(u.x);
    float theta = 2.0 * PI * u.y;
    
    float x = r * cos(theta);
    float y = r * sin(theta);
    float z = sqrt(max(0.0, 1.0 - u.x));
    
    // Create orthonormal basis
    float3 up = abs(normal.y) < 0.999 ? float3(0, 1, 0) : float3(1, 0, 0);
    float3 tangent = normalize(cross(up, normal));
    float3 bitangent = cross(normal, tangent);
    
    return normalize(tangent * x + bitangent * y + normal * z);
}

// Estimate a rough surface position by tracing toward a direction and using depth
float3 estimateSurfaceHit(float3 origin, float3 dir, float maxDist) {
    // Simple ray march with shadow map intersection
    float stepSize = maxDist / 8.0;
    float3 pos = origin;
    
    for (int i = 1; i <= 8; i++) {
        pos = origin + dir * (stepSize * i);
        
        // Check if this position is in shadow (means it hit something before the light)
        float4 lightSpacePos = mul(lightSpaceMatrix, float4(pos, 1.0));
        float3 projCoords = lightSpacePos.xyz / lightSpacePos.w;
        projCoords.x = projCoords.x * 0.5 + 0.5;
        projCoords.y = -projCoords.y * 0.5 + 0.5;
        
        if (projCoords.x >= 0 && projCoords.x <= 1 && projCoords.y >= 0 && projCoords.y <= 1) {
            float shadowDepth = shadowMap.SampleLevel(shadowSampler, projCoords.xy, 0).r;
            // If the shadow depth is less than our current depth, we're behind a surface
            if (shadowDepth < projCoords.z - 0.01) {
                // We found an occluder - return the estimated hit position
                return origin + dir * (stepSize * (i - 0.5));
            }
        }
        
        // Simple ground plane check
        if (pos.y < 0.01) {
            float t = -origin.y / dir.y;
            if (t > 0) return origin + dir * t;
        }
    }
    
    return origin + dir * maxDist;
}

// Calculate indirect lighting (bounced light) at probe position for a direction
float3 CalculateIndirectLighting(float3 probePos, float3 sampleDir, float2 randomSeed) {
    float3 indirectLight = float3(0, 0, 0);
    
    // Trace a ray from probe in the sample direction
    float maxTraceDist = maxRayDistance;
    float3 hitPos = estimateSurfaceHit(probePos, sampleDir, maxTraceDist);
    float hitDist = length(hitPos - probePos);
    
    // If we hit something (not max distance), calculate bounced light from that surface
    if (hitDist < maxTraceDist * 0.95) {
        // Estimate surface normal (pointing back toward us for a diffuse surface)
        float3 hitNormal = -sampleDir;
        
        // Ground plane - use up normal
        if (hitPos.y < 0.1) {
            hitNormal = float3(0, 1, 0);
        }
        
        // Calculate direct light at the hit position
        float3 surfaceLight = float3(0, 0, 0);
        
        // Main light contribution at hit surface
        float3 lightDir;
        float mainAtt = 1.0;
        
        if (mainLightType == 0) {
            lightDir = normalize(mainLightPos);
        } else {
            lightDir = normalize(mainLightPos - hitPos);
            float dist = length(mainLightPos - hitPos);
            mainAtt = 1.0 / (1.0 + 0.09 * dist + 0.032 * dist * dist);
        }
        
        float NdotL = max(dot(hitNormal, lightDir), 0.0);
        float shadow = CalculateShadow(hitPos);
        
        // Assume a grey diffuse surface (albedo ~0.5)
        float3 surfaceAlbedo = float3(0.5, 0.5, 0.5);
        
        // Ground is darker
        if (hitPos.y < 0.1) {
            surfaceAlbedo = float3(0.3, 0.3, 0.32);
        }
        
        surfaceLight += mainLightColor * mainLightIntensity * mainAtt * NdotL * shadow * surfaceAlbedo;
        
        // Point lights contribution at hit surface
        for (int i = 0; i < min(numPointLights, 16); i++) {
            float3 plPos = pointLights[i].position;
            float3 plColor = pointLights[i].color;
            float plRadius = pointLights[i].radius;
            float plIntensity = pointLights[i].intensity;
            
            float3 P_to_L = plPos - hitPos;
            float dist = length(P_to_L);
            
            if (dist < plRadius) {
                float3 L = P_to_L / dist;
                float att = saturate(1.0 - dist / plRadius);
                att *= att;
                
                float plNdotL = max(dot(hitNormal, L), 0.0);
                surfaceLight += plColor * plIntensity * att * plNdotL * surfaceAlbedo;
            }
        }
        
        // The bounced light contribution (lambertian BRDF)
        // Light reaching the probe = surfaceLight * (1/PI) * cos(angle) * (1/r^2)
        // But we integrate over hemisphere so just use surfaceLight scaled by distance
        float distFactor = 1.0 / (1.0 + hitDist * hitDist * 0.1);
        indirectLight = surfaceLight * distFactor;
    } else {
        // Sky contribution for rays that don't hit anything
        float skyFactor = saturate(sampleDir.y * 0.5 + 0.5);
        float3 skyColor = lerp(float3(0.1, 0.12, 0.15), float3(0.4, 0.5, 0.7), skyFactor);
        indirectLight = skyColor * 0.1;
    }
    
    return indirectLight;
}

// Calculate combined direct + indirect lighting at probe for direction
float3 CalculateProbeRadiance(float3 probePos, float3 direction, float2 pixelCoord) {
    float3 totalRadiance = float3(0, 0, 0);
    
    // ===== DIRECT LIGHTING =====
    // This is what surfaces facing this direction would receive from lights
    float3 directLight = float3(0, 0, 0);
    
    // Main light
    float3 lightDir;
    float mainAtt = 1.0;
    
    if (mainLightType == 0) {
        lightDir = normalize(mainLightPos);
    } else {
        lightDir = normalize(mainLightPos - probePos);
        float dist = length(mainLightPos - probePos);
        mainAtt = 1.0 / (1.0 + 0.09 * dist + 0.032 * dist * dist);
        if (dist > 25.0) mainAtt = 0.0;
    }
    
    float NdotL = max(dot(direction, lightDir), 0.0);
    float shadow = CalculateShadow(probePos + direction * normalBias);
    directLight += mainLightColor * mainLightIntensity * mainAtt * NdotL * shadow;
    
    // Point lights
    for (int i = 0; i < min(numPointLights, 32); i++) {
        float3 plPos = pointLights[i].position;
        float3 plColor = pointLights[i].color;
        float plRadius = pointLights[i].radius;
        float plIntensity = pointLights[i].intensity;
        
        float3 P_to_L = plPos - probePos;
        float dist = length(P_to_L);
        
        if (dist < plRadius) {
            float3 L = P_to_L / dist;
            float att = saturate(1.0 - dist / plRadius);
            att *= att;
            float plNdotL = max(dot(direction, L), 0.0);
            directLight += plColor * plIntensity * att * plNdotL;
        }
    }
    
    totalRadiance += directLight;
    
    // ===== INDIRECT LIGHTING (bounced light / GI) =====
    // Sample multiple directions in the hemisphere around 'direction' 
    // to gather bounced light
    float3 indirectLight = float3(0, 0, 0);
    
    // Use 4 random samples for indirect light
    float2 seed = pixelCoord + float2(probePos.x, probePos.z);
    for (int s = 0; s < 4; s++) {
        float2 randUV = hash3(seed + float2(s * 13.37, s * 7.13)).xy;
        float3 sampleDir = cosineSampleHemisphere(randUV, direction);
        indirectLight += CalculateIndirectLighting(probePos, sampleDir, randUV);
    }
    indirectLight /= 4.0;
    
    // Add indirect contribution (scaled down since it's bounced)
    totalRadiance += indirectLight * giIntensity;
    
    // Small ambient floor to prevent pure black
    float skyFactor = saturate(direction.y * 0.5 + 0.5);
    totalRadiance += float3(0.01, 0.012, 0.015) * skyFactor;
    
    return totalRadiance;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 gl_GlobalInvocationID : SV_DispatchThreadID, uint3 gl_GroupBoxID : SV_GroupID, uint3 gl_LocalInvocationID : SV_GroupThreadID) {
    // Each group corresponds to one probe for simplicity?
    // Or we map threads to atlas pixels directly.
    
    // Let's map thread ID to pixel coordinate in the atlas
    uint2 texCoord = gl_GlobalInvocationID.xy;
    
    // Determine which probe this pixel calculates
    // Atlas layout:
    // Width = width_in_probes * (irradianceTexWidth + 2)
    // Height = height_in_probes * (irradianceTexHeight + 2)
    
    int probeW = irradianceTexWidth + 2;
    int probeH = irradianceTexHeight + 2;
    
    // Don't process if out of texture bounds (though dispatch should match)
    // We will assume dispatch covers the atlas.
    
    int probeXIndex = texCoord.x / probeW;
    int probeYIndex = texCoord.y / probeH;
    
    // Calculate global probe index
    int probesPerRow = (probeCountX * probeCountZ + (probeCountX * probeCountZ) % (int)sqrt((float)(probeCountX * probeCountY * probeCountZ))) / (int)sqrt((float)(probeCountX * probeCountY * probeCountZ));
    // Actually, reused logic from C++:
    int totalProbes = probeCountX * probeCountY * probeCountZ;
    int atlasWidthProbes = (int)sqrt((float)totalProbes);
    
    int probeIdx = probeYIndex * atlasWidthProbes + probeXIndex;
    
    if (probeIdx >= totalProbes) return;
    
    // Decompose probe index to 3D grid index
    int pz = probeIdx / (probeCountX * probeCountY); // This logic might be wrong depending on how I packed it
    // Wait, simple packing:
    // idx = x + y * probeCountX + z * probeCountX * probeCountY;
    
    int tempIdx = probeIdx;
    int x = tempIdx % probeCountX;
    tempIdx /= probeCountX;
    int y = tempIdx % probeCountY;
    int z = tempIdx / probeCountY;
    
    float3 probePos = GetProbePosition(int3(x, y, z));
    
    // Calculate UV within the probe's texture area (excluding border)
    int localX = texCoord.x % probeW;
    int localY = texCoord.y % probeH;
    
    // Border handling - for now simple clamp or skip
    // Center 8x8 is the data. Border is copied.
    // If inside 8x8
    if (localX >= 1 && localX <= irradianceTexWidth && localY >= 1 && localY <= irradianceTexHeight) {
        float2 uv = float2(localX - 1, localY - 1) / float2(irradianceTexWidth - 1, irradianceTexHeight - 1);
        
        // Map UV to direction (Octahedral)
        float3 direction = DecodeOctahedralDir(uv);
        
        // Calculate combined direct + indirect lighting (true GI)
        float3 irradiance = CalculateProbeRadiance(probePos, direction, float2(texCoord));
        
        OutputIrradiance[texCoord] = float4(irradiance, 1.0);
    } else {
        // Border pixels - extend the mapping for seamless edges
        float2 uv = float2(localX, localY) / float2(probeW - 1, probeH - 1);
        float3 direction = DecodeOctahedralDir(uv);
        float3 irradiance = CalculateProbeRadiance(probePos, direction, float2(texCoord));
        OutputIrradiance[texCoord] = float4(irradiance, 1.0);
    }
}

