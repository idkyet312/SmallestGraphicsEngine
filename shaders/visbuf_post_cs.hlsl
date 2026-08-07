Texture2D<float4> hdrInput : register(t0);
Texture2D<float2> motionInput : register(t1);
Texture2D<float4> historyInput : register(t2);
ByteAddressBuffer exposureState : register(t3);
Texture3D<float4> colorLUT : register(t4);
Texture2D<float> sceneDepth : register(t5);
Texture2D<float> visibilityDepth : register(t6);
Texture2D<float4> bloomInput : register(t7);
// Surface identity, this frame and last. R32G32_UINT: .x is drawCallID+1 (0 =
// background), .y is SV_PrimitiveID. Together they name an exact triangle.
Texture2D<uint2> surfaceIDs : register(t8);
Texture2D<uint2> surfaceIDHistory : register(t9);
RWTexture2D<float4> ldrOutput : register(u0);
RWTexture2D<float4> historyOutput : register(u1);
SamplerState lutSampler : register(s0);

#include "color_grade.hlsli"

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
    uint validationMode;
    // Surface-ID temporal validity. When on, history is accepted or rejected
    // by exact instance+primitive match instead of the depth heuristic below.
    uint surfaceHistoryValid;
    // Debug: visualise why history was accepted or rejected.
    uint historyDebugView;
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
    // Match clustered_dx12_ps.hlsl exactly. Validation mode must compare
    // renderer ownership/lighting, not two different display transforms.
    color = pow(saturate(color), 1.35);
    color = luma + 1.4 * (color - luma);
    return saturate(mul(agxOut, color));
}

float3 TonemapSkyACES(float3 color) {
    color = saturate((color * (2.51 * color + 0.03)) /
                     (color * (2.43 * color + 0.59) + 0.14));
    return pow(color, 1.0 / 2.2);
}

float3 Bloom(uint2 pixel) {
    float2 uv = (float2(pixel) + 0.5) / float2(outputSize);
    return bloomInput.SampleLevel(lutSampler, uv, 0.0).rgb;
}

float LinearizeDepth(float depth) {
    return nearPlane * farPlane /
        max(farPlane - depth * (farPlane - nearPlane), 1e-4);
}

float3 CinematicInput(uint2 pixel) {
    int2 dimensions = int2(outputSize);
    float3 color = hdrInput.Load(int3(pixel, 0)).rgb;

    if (motionBlurStrength > 0.0) {
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

    float3 history = historyInput.SampleLevel(lutSampler, previousUV, 0.0).rgb;
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
    float motionConfidence = saturate(1.0 -
        length(motion * float2(outputSize)) * 0.035);
    float currentLuma = Luminance(currentColor);
    float historyLuma = Luminance(history);
    // Contact shadows move independently of visibility-buffer motion vectors.
    // Reject meaningful luminance changes instead of accumulating old shadow
    // positions into several dark silhouettes.
    float luminanceConfidence = saturate(1.0 -
        abs(currentLuma - historyLuma) /
        max(max(currentLuma, historyLuma) * 0.18, 0.025));
    // Is this the same surface as last frame?
    //
    // The depth test below is a heuristic: "the depth here is roughly what the
    // visibility pass resolved, so this is probably not a forward extension
    // sitting in front of stale history." It rejects trees, rotor blades and
    // skinned actors wholesale, because those lack per-object motion.
    //
    // Surface IDs answer the question directly instead. The visibility buffer
    // already records which instance and which triangle produced every pixel,
    // so an exact match is proof of correspondence rather than an inference
    // from two values that merely correlate with it. Where that proof is
    // available it replaces the heuristic entirely.
    float extensionConfidence;
    if (surfaceHistoryValid != 0) {
        uint2 currentID = surfaceIDs.Load(int3(pixel, 0));
        int2 previousPixel = int2(previousUV * float2(outputSize));
        uint2 previousID = surfaceIDHistory.Load(int3(previousPixel, 0));
        // Background (id 0) has no surface to match, so fall back to accepting
        // it -- sky history is reprojected separately and is safe to keep.
        bool background = currentID.x == 0u;

        // Instance match is the load-bearing test. Primitive equality is NOT
        // required: a flat wall is many triangles, and a sub-pixel camera shift
        // slides a pixel across a shared edge every frame. Demanding an exact
        // triangle match rejected history on every large flat surface, which
        // showed up as a shimmer that looked like TAA "moving too much".
        bool sameInstance = currentID.x == previousID.x;

        // Within an instance, accept a neighbouring triangle too. Scanning the
        // 4-neighbourhood for the exact primitive distinguishes "the pixel
        // crossed a triangle edge on the same mesh" (reuse) from "a different
        // part of the mesh folded over itself" (reject) without needing stored
        // barycentrics.
        bool samePrimitive = currentID.y == previousID.y;
        if (sameInstance && !samePrimitive) {
            [unroll]
            for (int i = 0; i < 4; ++i) {
                const int2 offsets[4] = {
                    int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1) };
                int2 tap = clamp(previousPixel + offsets[i], int2(0, 0),
                                 int2(outputSize) - 1);
                uint2 neighbour = surfaceIDHistory.Load(int3(tap, 0));
                if (neighbour.x == currentID.x && neighbour.y == currentID.y) {
                    samePrimitive = true;
                    break;
                }
            }
        }
        extensionConfidence =
            (background || (sameInstance && samePrimitive)) ? 1.0 : 0.0;
    } else {
        // Trees, rotor blades, skinned actors, and other forward extensions do
        // not yet emit per-object motion. Reject their stale underlying VB
        // history.
        float currentDepth = sceneDepth.Load(int3(pixel, 0));
        float resolvedDepth = visibilityDepth.Load(int3(pixel, 0));
        extensionConfidence = currentDepth + 1e-5 < resolvedDepth ? 0.0 : 1.0;
    }
    return lerp(currentColor, history,
                taaFeedback * motionConfidence * luminanceConfidence *
                extensionConfidence);
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

    // History-validity debug view. Temporal quality is a motion property, so
    // this is the only practical way to see whether the exact surface test is
    // firing or silently falling back:
    //   green  = surface matched, history reused
    //   red    = surface mismatch, history rejected (expect this at
    //            disocclusion edges only)
    //   blue   = reprojected off-screen, no history available
    //   grey   = background
    //
    // Deliberately does NOT gate on historyValid (the TAA colour-history flag).
    // Surface correspondence is a property of the visibility buffer, not of
    // TAA: the IDs are captured every frame whether or not TAA consumes them.
    // Gating on it painted the whole screen blue with TAA off, which said
    // nothing about whether the surface test actually works.
    if (historyDebugView != 0u) {
        float2 uvDebug = (float2(pixel) + 0.5) / float2(outputSize);
        float2 motionDebug = motionInput.Load(int3(pixel, 0));
        float2 previousUVDebug = uvDebug - motionDebug;
        float3 marker;
        uint2 currentID = surfaceIDs.Load(int3(pixel, 0));
        if (currentID.x == 0u) {
            marker = float3(0.25, 0.25, 0.25);
        } else if (surfaceHistoryValid == 0u) {
            // No captured ID history yet (first frame, or the feature is off).
            marker = float3(0.6, 0.4, 0.0);
        } else if (any(previousUVDebug <= 0.0) || any(previousUVDebug >= 1.0)) {
            marker = float3(0.1, 0.2, 1.0);
        } else {
            // Mirrors the acceptance rule in TemporalResolve, including the
            // neighbour scan -- a debug view that reports a stricter test than
            // the shader actually applies is worse than none.
            int2 previousPixel = int2(previousUVDebug * float2(outputSize));
            uint2 previousID = surfaceIDHistory.Load(int3(previousPixel, 0));
            bool sameInstance = currentID.x == previousID.x;
            bool samePrimitive = currentID.y == previousID.y;
            if (sameInstance && !samePrimitive) {
                [unroll]
                for (int i = 0; i < 4; ++i) {
                    const int2 offsets[4] = {
                        int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1) };
                    int2 tap = clamp(previousPixel + offsets[i], int2(0, 0),
                                     int2(outputSize) - 1);
                    uint2 neighbour = surfaceIDHistory.Load(int3(tap, 0));
                    if (neighbour.x == currentID.x &&
                        neighbour.y == currentID.y) {
                        samePrimitive = true;
                        break;
                    }
                }
            }
            marker = (sameInstance && samePrimitive)
                ? float3(0.1, 0.9, 0.2) : float3(1.0, 0.1, 0.1);
        }
        ldrOutput[pixel] = float4(marker, 1.0);
        return;
    }
    float autoExposure = 1.0;
    if (validationMode == 0u)
        autoExposure = exposureState.Load(8) / 65536.0;
    if (autoExposure <= 0.0) autoExposure = 1.0;
    // Forward sky uses ACES + display gamma. The VB background is stored as
    // linear HDR, so reproduce that exact transform for parity captures.
    float rawDepth = sceneDepth.Load(int3(pixel, 0));
    bool validationSky = validationMode != 0u && rawDepth >= 0.9999;
    float3 color = validationSky
        ? TonemapSkyACES(hdr)
        : TonemapAgX((hdr + Bloom(pixel) * bloomStrength)
                     * exposure * autoExposure);
    color = ApplySceneColorGrade(color);
    float lutScale = 15.0 / 16.0;
    float lutOffset = 0.5 / 16.0;
    if (validationMode == 0u) {
        color = colorLUT.SampleLevel(lutSampler,
            saturate(color) * lutScale + lutOffset, 0).rgb;
    }
    float2 uv = (float2(pixel) + 0.5) / float2(outputSize);
    float2 centered = uv * 2.0 - 1.0;
    float vignette = smoothstep(1.35, 0.35, dot(centered, centered));
    color *= lerp(1.0, vignette, vignetteStrength);
    color = saturate(color + (Hash12(float2(pixel) + frameIndex * 17.0) - 0.5)
                     * grainStrength);
    ldrOutput[pixel] = float4(color, 1.0);
}
