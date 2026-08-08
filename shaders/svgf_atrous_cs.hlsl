// SVGF à-trous spatial filter — Phase 5c
//
// One wavelet iteration per dispatch. The caller binds either the temporal
// reflection signal (iteration 0) or the preceding scratch texture (later
// iterations) at t0, then binds the other scratch texture at u0. Variance is
// carried in alpha and filtered with squared weights.

cbuffer AtrousConstants : register(b0) {
    uint  screenWidth;
    uint  screenHeight;
    float sigmaDepth;
    float sigmaNormal;
    float sigmaLuminance;
    uint  iterationIndex;
    uint2 padding;
};

Texture2D<float4> inputReflection : register(t0); // rgb=signal, a=variance
Texture2D<float>  depthBuffer     : register(t1); // device depth
Texture2D<float4> normalRoughness : register(t2);
RWTexture2D<float4> outputReflection : register(u0);

static const int2 kOffsets[25] = {
    int2(-2, -2), int2(-1, -2), int2(0, -2), int2(1, -2), int2(2, -2),
    int2(-2, -1), int2(-1, -1), int2(0, -1), int2(1, -1), int2(2, -1),
    int2(-2,  0), int2(-1,  0), int2(0,  0), int2(1,  0), int2(2,  0),
    int2(-2,  1), int2(-1,  1), int2(0,  1), int2(1,  1), int2(2,  1),
    int2(-2,  2), int2(-1,  2), int2(0,  2), int2(1,  2), int2(2,  2)
};

static const float kKernel[5] = { 1.0 / 16.0, 1.0 / 4.0, 3.0 / 8.0,
                                  1.0 / 4.0, 1.0 / 16.0 };

float Luminance(float3 color) {
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID) {
    uint2 pixel = dispatchThreadID.xy;
    if (pixel.x >= screenWidth || pixel.y >= screenHeight) return;

    const uint stride = 1u << min(iterationIndex, 4u);
    const float4 centre = inputReflection.Load(int3(pixel, 0));
    const float centreDepth = depthBuffer.Load(int3(pixel, 0));
    const float3 centreNormal = normalize(
        normalRoughness.Load(int3(pixel, 0)).xyz);
    const float centreLuminance = Luminance(centre.rgb);

    const int2 size = int2(screenWidth, screenHeight);
    const int2 px = int2(pixel);
    const float depthDx = abs(
        depthBuffer.Load(int3(clamp(px + int2(1, 0), 0, size - 1), 0)) -
        depthBuffer.Load(int3(clamp(px - int2(1, 0), 0, size - 1), 0)));
    const float depthDy = abs(
        depthBuffer.Load(int3(clamp(px + int2(0, 1), 0, size - 1), 0)) -
        depthBuffer.Load(int3(clamp(px - int2(0, 1), 0, size - 1), 0)));
    const float depthTolerance = sigmaDepth *
        (max(depthDx, depthDy) * stride + 1e-5);
    const float luminanceTolerance =
        sigmaLuminance * sqrt(max(centre.a, 0.0)) + 1e-4;

    float3 colourSum = 0.0;
    float weightSum = 0.0;
    float varianceSum = 0.0;
    float varianceWeightSum = 0.0;

    [unroll]
    for (uint i = 0; i < 25; ++i) {
        const int2 offset = kOffsets[i];
        const int2 tapPixel = clamp(px + offset * int(stride), 0, size - 1);
        const float4 tap = inputReflection.Load(int3(tapPixel, 0));
        const float tapDepth = depthBuffer.Load(int3(tapPixel, 0));
        const float3 tapNormal = normalize(
            normalRoughness.Load(int3(tapPixel, 0)).xyz);

        const float kernelWeight =
            kKernel[offset.x + 2] * kKernel[offset.y + 2];
        const float depthWeight = exp(
            -abs(centreDepth - tapDepth) / depthTolerance);
        const float normalWeight = pow(
            saturate(dot(centreNormal, tapNormal)), sigmaNormal);
        const float luminanceWeight = exp(
            -abs(centreLuminance - Luminance(tap.rgb)) /
             luminanceTolerance);
        const float weight = kernelWeight * depthWeight * normalWeight *
                             luminanceWeight;

        colourSum += tap.rgb * weight;
        weightSum += weight;
        const float varianceWeight = weight * weight;
        varianceSum += max(tap.a, 0.0) * varianceWeight;
        varianceWeightSum += varianceWeight;
    }

    outputReflection[pixel] = float4(
        max(colourSum / max(weightSum, 1e-8), 0.0),
        varianceSum / max(varianceWeightSum, 1e-8));
}
