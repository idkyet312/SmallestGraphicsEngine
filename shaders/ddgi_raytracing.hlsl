// DDGI Raytracing Shader for DXR

#define PI 3.14159265359

// Payload struct - keep size minimal for performance
struct RayPayload {
    float3 color;
    float hitT;
};

// Constant buffer - must match C++ struct alignment
cbuffer DDGIConstants : register(b0) {
    float3 probeGridOrigin;
    float probeSpacing;
    
    int probeCountX;
    int probeCountY;
    int probeCountZ;
    int raysPerProbe;
    
    float maxRayDistance;
    float normalBias;
    float hysteresis;
    float giIntensity;
    
    int irradianceTexWidth;
    int irradianceTexHeight;
    int frameNumber;
    int cbPadding;
    
    float3 sunDirection;
    float sunIntensity;
    
    float3 sunColor;
    int numPointLights;
};

// Resources
RWTexture2D<float4> outputIrradiance : register(u0);
Texture2D<float4> prevIrradiance : register(t0);
RaytracingAccelerationStructure scene : register(t1);

// Sampler for previous frame texture
SamplerState linearSampler : register(s0);

// Octahedral decoding (UV to direction)
float3 DecodeOctahedral(float2 uv) {
    uv = uv * 2.0 - 1.0;
    float3 n = float3(uv.x, 1.0 - abs(uv.x) - abs(uv.y), uv.y);
    if (n.y < 0) {
        float2 signN = float2(n.x >= 0 ? 1 : -1, n.z >= 0 ? 1 : -1);
        n.xz = (1.0 - abs(n.zx)) * signN;
    }
    return normalize(n);
}

// Get probe world position from grid index
float3 GetProbePosition(int3 probeIndex) {
    return probeGridOrigin + float3(probeIndex) * probeSpacing;
}

// Pseudo-random for jittering
float Random(float2 seed) {
    return frac(sin(dot(seed, float2(12.9898, 78.233))) * 43758.5453);
}

[shader("raygeneration")]
void RayGen() {
    uint2 launchIndex = DispatchRaysIndex().xy;
    uint2 launchDim = DispatchRaysDimensions().xy;
    
    // Calculate which probe and texel we're computing
    int probeW = irradianceTexWidth;
    int probeH = irradianceTexHeight;
    
    int totalProbes = probeCountX * probeCountY * probeCountZ;
    int atlasWidthProbes = max(1, (int)sqrt((float)totalProbes));
    
    int probeXIdx = launchIndex.x / probeW;
    int probeYIdx = launchIndex.y / probeH;
    int probeIdx = probeYIdx * atlasWidthProbes + probeXIdx;
    
    if (probeIdx >= totalProbes) {
        outputIrradiance[launchIndex] = float4(0, 0, 0, 1);
        return;
    }
    
    // Decompose probe index to 3D grid coordinates
    int x = probeIdx % probeCountX;
    int temp = probeIdx / probeCountX;
    int y = temp % probeCountY;
    int z = temp / probeCountY;
    
    float3 probePos = GetProbePosition(int3(x, y, z));
    
    // Get direction from octahedral UV
    int localX = launchIndex.x % probeW;
    int localY = launchIndex.y % probeH;
    float2 uv = float2(localX + 0.5, localY + 0.5) / float2(probeW, probeH);
    float3 direction = DecodeOctahedral(uv);
    
    // Trace ray and gather radiance
    float3 irradiance = float3(0, 0, 0);
    const int numSamples = 4;
    
    for (int s = 0; s < numSamples; s++) {
        // Add temporal jitter for anti-aliasing
        float jitterScale = 0.05;
        float2 jitter = float2(
            Random(float2(launchIndex.x + s * 13, launchIndex.y + frameNumber)) - 0.5,
            Random(float2(launchIndex.y + s * 17, launchIndex.x + frameNumber)) - 0.5
        ) * jitterScale;
        
        float3 jitteredDir = normalize(direction + float3(jitter.x, 0, jitter.y));
        
        RayDesc ray;
        ray.Origin = probePos + direction * normalBias;
        ray.Direction = jitteredDir;
        ray.TMin = 0.001;
        ray.TMax = maxRayDistance;
        
        RayPayload payload;
        payload.color = float3(0, 0, 0);
        payload.hitT = maxRayDistance;
        
        TraceRay(scene, RAY_FLAG_CULL_BACK_FACING_TRIANGLES, 0xFF, 0, 1, 0, ray, payload);
        
        irradiance += payload.color;
    }
    irradiance /= float(numSamples);
    
    // Temporal blend: mix new value with previous frame for stability
    // hysteresis = 0.95 means keep 95% of previous, add 5% of new
    float4 prevValue = prevIrradiance[launchIndex];
    float3 blendedIrradiance = lerp(irradiance, prevValue.rgb, hysteresis);
    
    outputIrradiance[launchIndex] = float4(blendedIrradiance * giIntensity, 1.0);
}

[shader("miss")]
void Miss(inout RayPayload payload : SV_RayPayload) {
    // Sky color based on ray direction
    float3 dir = WorldRayDirection();
    float skyFactor = saturate(dir.y * 0.5 + 0.5);
    
    // Gradient sky
    float3 horizonColor = float3(0.5, 0.6, 0.7);
    float3 zenithColor = float3(0.2, 0.3, 0.5);
    float3 skyColor = lerp(horizonColor, zenithColor, skyFactor);
    
    // Sun contribution
    float3 sunDir = normalize(sunDirection);
    float sunDot = saturate(dot(dir, sunDir));
    
    // Sun disk
    float sunDisk = pow(sunDot, 128.0) * 2.0;
    // Sun glow
    float sunGlow = pow(sunDot, 4.0) * 0.3;
    
    skyColor += sunColor * sunIntensity * (sunDisk + sunGlow);
    
    // Ground color for downward rays
    if (dir.y < 0) {
        float groundFactor = saturate(-dir.y);
        float3 groundColor = float3(0.2, 0.2, 0.18);
        skyColor = lerp(skyColor, groundColor, groundFactor * 0.5);
    }
    
    payload.color = skyColor;
    payload.hitT = maxRayDistance;
}

[shader("closesthit")]
void ClosestHit(inout RayPayload payload : SV_RayPayload, in BuiltInTriangleIntersectionAttributes attribs) {
    float3 hitPos = WorldRayOrigin() + WorldRayDirection() * RayTCurrent();
    
    // Compute geometric normal from triangle
    // For now, use a simplified normal estimation
    float3 barycentrics = float3(1.0 - attribs.barycentrics.x - attribs.barycentrics.y,
                                  attribs.barycentrics.x,
                                  attribs.barycentrics.y);
    
    // Estimate normal from ray direction (surface faces back to ray)
    float3 normal = -normalize(WorldRayDirection());
    
    // For horizontal surfaces (floor), use up vector
    if (abs(hitPos.y) < 0.1) {
        normal = float3(0, 1, 0);
    }
    
    // Material properties (simplified)
    float3 albedo = float3(0.6, 0.6, 0.58); // Grey diffuse
    
    // Direct sun lighting
    float3 sunDir = normalize(sunDirection);
    float NdotL = saturate(dot(normal, sunDir));
    
    // For simplicity, assume fully lit (no shadow ray trace to avoid recursion issues)
    float shadow = 1.0;
    
    float3 directLight = sunColor * sunIntensity * NdotL * shadow;
    
    // Ambient from sky
    float3 ambient = float3(0.15, 0.15, 0.18);
    
    // Combine lighting
    payload.color = albedo * (directLight + ambient);
    payload.hitT = RayTCurrent();
}

