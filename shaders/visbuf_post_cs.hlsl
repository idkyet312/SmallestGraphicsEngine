Texture2D<float4> hdrInput : register(t0);
Texture2D<float2> motionInput : register(t1);
Texture2D<float4> historyInput : register(t2);
ByteAddressBuffer exposureState : register(t3);
Texture3D<float4> colorLUT : register(t4);
Texture2D<float> sceneDepth : register(t5);
RWTexture2D<float4> ldrOutput : register(u0);
RWTexture2D<float4> historyOutput : register(u1);
SamplerState lutSampler : register(s0);

cbuffer PostConstants : register(b0) {
    uint2 outputSize;
    float exposure;
    float bloomStrength;
    float vignetteStrength;
    float grainStrength;
    uint frameIndex;
    uint historyValid;
    float taaFeedback;
    float motionBlurStrength;
    float focusDistance;
    float aperture;
    float nearPlane;
    float farPlane;
    uint debugViewMode;
    float postPadding;
};

float Luminance(float3 color) { return dot(color, float3(0.2126, 0.7152, 0.0722)); }

float Hash12(float2 p) {
    float3 p3 = frac(float3(p.xyx) * 0.1031);
    p3 += dot(p3, p3.yzx + 33.33);
    return frac((p3.x + p3.y) * p3.z);
}

float3 AgXContrast(float3 x) {
    float3 x2 = x * x;
    float3 x4 = x2 * x2;
    return 15.5 * x4 * x2 - 40.14 * x4 * x + 31.96 * x4
         - 6.868 * x2 * x + 0.4298 * x2 + 0.1191 * x - 0.00232;
}

float3 TonemapAgX(float3 color) {
    const float3x3 agxIn = float3x3(
        0.8424790623, 0.0423282423, 0.0423756549,
        0.0784336000, 0.8784686365, 0.0784336000,
        0.0792237451, 0.0791661275, 0.8791429738);
    const float3x3 agxOut = float3x3(
         1.1968790051, -0.0528968518, -0.0529716355,
        -0.0980208811,  1.1519031299, -0.0980434501,
        -0.0990297441, -0.0989611768,  1.1510736726);
    const float minEv = -12.47393;
    const float maxEv = 4.026069;
    color = mul(agxIn, max(color, 1e-10));
    color = (clamp(log2(color), minEv, maxEv) - minEv) / (maxEv - minEv);
    color = AgXContrast(color);
    float luma = Luminance(color);
    color = pow(saturate(color), 1.25);
    color = luma + 1.25 * (color - luma);
    return saturate(mul(agxOut, color));
}

float3 Bloom(uint2 pixel) {
    static const int2 offsets[9] = {
        int2(0, 0), int2(-2, 0), int2(2, 0), int2(0, -2), int2(0, 2),
        int2(-1, -1), int2(1, -1), int2(-1, 1), int2(1, 1)
    };
    static const float weights[9] = {
        0.20, 0.075, 0.075, 0.075, 0.075, 0.125, 0.125, 0.125, 0.125
    };
    float3 bloom = 0.0;
    [unroll]
    for (uint i = 0; i < 9; ++i) {
        int2 p = clamp(int2(pixel) + offsets[i], int2(0, 0), int2(outputSize) - 1);
        float3 c = hdrInput.Load(int3(p, 0)).rgb;
        bloom += c * saturate((Luminance(c) - 1.0) * 0.5) * weights[i];
    }
    return bloom;
}

float LinearizeDepth(float depth) {
    return nearPlane * farPlane /
        max(farPlane - depth * (farPlane - nearPlane), 1e-4);
}

float3 CinematicInput(uint2 pixel) {
    int2 dimensions = int2(outputSize);
    float3 color = hdrInput.Load(int3(pixel, 0)).rgb;

    float2 velocityPixels = motionInput.Load(int3(pixel, 0)) * float2(outputSize);
    float blurLength = min(length(velocityPixels) * motionBlurStrength, 12.0);
    if (blurLength > 0.5) {
        float2 direction = velocityPixels / max(length(velocityPixels), 1e-4);
        float3 motionColor = color * 0.28;
        [unroll]
        for (int tap = 1; tap <= 2; ++tap) {
            float offset = blurLength * (tap / 2.0);
            int2 a = clamp(int2(float2(pixel) + direction * offset), 0, dimensions - 1);
            int2 b = clamp(int2(float2(pixel) - direction * offset), 0, dimensions - 1);
            motionColor += (hdrInput.Load(int3(a, 0)).rgb +
                            hdrInput.Load(int3(b, 0)).rgb) * 0.18;
        }
        color = motionColor;
    }

    float depth = sceneDepth.Load(int3(pixel, 0));
    float viewDepth = LinearizeDepth(depth);
    float coc = min(abs(viewDepth - focusDistance) * aperture /
                    max(viewDepth, 0.1) * outputSize.y, 6.0);
    if (depth < 0.9999 && coc > 0.75) {
        static const float2 disk[8] = {
            float2(1, 0), float2(-1, 0), float2(0, 1), float2(0, -1),
            float2(0.707, 0.707), float2(-0.707, 0.707),
            float2(0.707, -0.707), float2(-0.707, -0.707)
        };
        float3 defocused = color * 0.2;
        [unroll]
        for (int i = 0; i < 8; ++i) {
            int2 p = clamp(int2(float2(pixel) + disk[i] * coc), 0, dimensions - 1);
            defocused += hdrInput.Load(int3(p, 0)).rgb * 0.1;
        }
        color = defocused;
    }
    return color;
}

float3 TemporalResolve(uint2 pixel, float3 currentColor) {
    if (historyValid == 0) return currentColor;
    float2 uv = (float2(pixel) + 0.5) / float2(outputSize);
    float2 motion = motionInput.Load(int3(pixel, 0));
    float2 previousUV = uv - motion;
    if (any(previousUV <= 0.0) || any(previousUV >= 1.0)) return currentColor;

    int2 previousPixel = clamp(int2(previousUV * float2(outputSize)),
                               int2(0, 0), int2(outputSize) - 1);
    float3 history = historyInput.Load(int3(previousPixel, 0)).rgb;
    float3 neighborhoodMin = currentColor;
    float3 neighborhoodMax = currentColor;
    [unroll]
    for (int y = -1; y <= 1; ++y) {
        [unroll]
        for (int x = -1; x <= 1; ++x) {
            int2 p = clamp(int2(pixel) + int2(x, y), int2(0, 0),
                           int2(outputSize) - 1);
            float3 c = hdrInput.Load(int3(p, 0)).rgb;
            neighborhoodMin = min(neighborhoodMin, c);
            neighborhoodMax = max(neighborhoodMax, c);
        }
    }
    history = clamp(history, neighborhoodMin, neighborhoodMax);
    float motionConfidence = saturate(1.0 - length(motion * float2(outputSize)) * 0.08);
    return lerp(currentColor, history, taaFeedback * motionConfidence);
}

[numthreads(8, 8, 1)]
void main(uint3 threadID : SV_DispatchThreadID) {
    uint2 pixel = threadID.xy;
    if (any(pixel >= outputSize)) return;
    if (debugViewMode != 0u) {
        float4 debugColor = hdrInput.Load(int3(pixel, 0));
        historyOutput[pixel] = debugColor;
        ldrOutput[pixel] = debugColor;
        return;
    }
    float3 hdr = TemporalResolve(pixel, CinematicInput(pixel));
    historyOutput[pixel] = float4(hdr, 1.0);
    float autoExposure = exposureState.Load(8) / 65536.0;
    if (autoExposure <= 0.0) autoExposure = 1.0;
    float3 color = TonemapAgX((hdr + Bloom(pixel) * bloomStrength)
                              * exposure * autoExposure);
    float lutScale = 15.0 / 16.0;
    float lutOffset = 0.5 / 16.0;
    color = colorLUT.SampleLevel(lutSampler,
        saturate(color) * lutScale + lutOffset, 0).rgb;
    float2 uv = (float2(pixel) + 0.5) / float2(outputSize);
    float2 centered = uv * 2.0 - 1.0;
    float vignette = smoothstep(1.35, 0.35, dot(centered, centered));
    color *= lerp(1.0, vignette, vignetteStrength);
    color = saturate(color + (Hash12(float2(pixel) + frameIndex * 17.0) - 0.5)
                     * grainStrength);
    ldrOutput[pixel] = float4(color, 1.0);
}
