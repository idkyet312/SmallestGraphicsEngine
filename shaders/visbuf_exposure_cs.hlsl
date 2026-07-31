Texture2D<float4> hdrInput : register(t0);
RWByteAddressBuffer exposureState : register(u0); // sum, count, exposure Q16

cbuffer ExposureConstants : register(b0) {
    uint2 inputSize;
    float adaptationRate;
    float middleGray;
};

groupshared uint groupSum[256];
groupshared uint groupCount[256];

[numthreads(1, 1, 1)]
void Reset(uint3 unused : SV_DispatchThreadID) {
    exposureState.Store(0, 0);
    exposureState.Store(4, 0);
}

[numthreads(16, 16, 1)]
void Accumulate(uint3 dispatchID : SV_DispatchThreadID,
                uint groupIndex : SV_GroupIndex) {
    uint2 pixel = dispatchID.xy * 8u + 4u;
    uint encoded = 0;
    uint valid = 0;
    if (all(pixel < inputSize)) {
        float3 color = hdrInput.Load(int3(pixel, 0)).rgb;
        float luminance = dot(color, float3(0.2126, 0.7152, 0.0722));
        // Log luminance prevents a few emissive pixels dominating exposure.
        float logLuminance = log2(max(luminance, 1e-5));
        encoded = (uint)round(saturate((logLuminance + 12.0) / 20.0) * 4095.0);
        valid = 1;
    }
    groupSum[groupIndex] = encoded;
    groupCount[groupIndex] = valid;
    GroupMemoryBarrierWithGroupSync();
    for (uint stride = 128; stride > 0; stride >>= 1) {
        if (groupIndex < stride) {
            groupSum[groupIndex] += groupSum[groupIndex + stride];
            groupCount[groupIndex] += groupCount[groupIndex + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (groupIndex == 0) {
        exposureState.InterlockedAdd(0, groupSum[0]);
        exposureState.InterlockedAdd(4, groupCount[0]);
    }
}

[numthreads(1, 1, 1)]
void Finalize(uint3 unused : SV_DispatchThreadID) {
    uint count = exposureState.Load(4);
    if (count == 0) return;
    float encodedMean = exposureState.Load(0) / (float)count / 4095.0;
    float averageLuminance = exp2(encodedMean * 20.0 - 12.0);
    float targetExposure = clamp(middleGray / max(averageLuminance, 1e-4), 0.05, 16.0);
    float previousExposure = exposureState.Load(8) / 65536.0;
    if (previousExposure <= 0.0) previousExposure = 1.0;
    float adapted = lerp(previousExposure, targetExposure, saturate(adaptationRate));
    exposureState.Store(8, (uint)round(adapted * 65536.0));
}
