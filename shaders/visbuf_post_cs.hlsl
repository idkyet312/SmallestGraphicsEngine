Texture2D<float4> hdrInput : register(t0);
Texture2D<float2> motionInput : register(t1);
Texture2D<float4> historyInput : register(t2);
ByteAddressBuffer exposureState : register(t3);
Texture3D<float4> colorLUT : register(t4);
Texture2D<float> sceneDepth : register(t5);
Texture2D<float> visibilityDepth : register(t6);
Texture2D<float4> bloomInput : register(t7);
// The visibility buffer stays local to the current draw so resolve can address
// geometry directly. Post translates it to the authored identity consumed by
// temporal effects: namespace in .x, stable triangle ID in .y, with zero kept
// for background.
Texture2D<uint2> surfaceIDs : register(t8);
Texture2D<uint2> stableSurfaceHistory : register(t9);

struct DrawCallData {
    float4x4 modelMatrix;
    float4x4 previousModelMatrix;
    float3   objectColor;
    float    useTexture;
    float    metalness;
    float    roughness;
    float    useNormalMap;
    uint     materialID;
    uint     vertexOffset;
    uint     indexOffset;
    uint     indexCount;
    uint     hasIndices;
    uint     flags;
    float4   palmWindRoot;
};
StructuredBuffer<DrawCallData> drawCalls : register(t10);
StructuredBuffer<uint> stableTriangleIDs : register(t11);
Texture2D<float> lensDirtTexture : register(t12);
Texture2D<float> sunFlareTexture : register(t13);
Texture2D<float> lensGhostTexture : register(t14);
Texture2D<float4> flareInput : register(t15);
RWTexture2D<float4> ldrOutput : register(u0);
RWTexture2D<float4> historyOutput : register(u1);
RWTexture2D<uint2> stableSurfaceOutput : register(u2);
SamplerState lutSampler : register(s0);
SamplerState dirtSampler : register(s1);

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
    // Generate the current authored identity even when colour history is not
    // valid yet, so this frame can seed the next one.
    uint surfaceIdentityEnabled;
    // Lens simulation. Dirt modulates bloom through an optical grime mask so
    // bright sources light up the grime on the front element; aberration, the
    // anamorphic streak and the ghost chain are separate lens artefacts driven
    // from the same bloom buffer.
    float lensDirtStrength;
    float lensDirtScale;
    float chromaticAberration;
    float lensFlareStrength;
    // xy = projected sun UV (may fall outside 0..1), z = continuous presence
    // that ramps to zero across a margin beyond the frame edge,
    // w = elevation-scaled directional-light intensity.
    float4 sunLensPosition;
    float4 sunLensColor;
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

#if defined(SGE_DLSS_SR)
float SceneDepthAt(uint2 pixel) {
    const float2 uv = (float2(pixel) + 0.5) / float2(outputSize);
    return sceneDepth.SampleLevel(lutSampler, uv, 0.0);
}
#endif

// Explicit mip: the post bloom SRV now exposes the whole pyramid rather than
// mip 0 alone, so a caller can ask for a progressively wider blur for free.
float3 BloomAt(float2 uv, float mip) {
    return bloomInput.SampleLevel(lutSampler, saturate(uv), mip).rgb;
}


// Purpose-built CC0 optical dirt, sampled once across the frame. It stays in
// lens space while the scene moves and only becomes visible when bloom gives
// it energy. Tiling this texture creates repeated constellations of dust that
// immediately read as a screen overlay, so scale is a restrained centre zoom.
float LensDirtMask(float2 uv) {
    float2 p = (uv - 0.5) * max(lensDirtScale, 0.01) + 0.5;
    float dirt = lensDirtTexture.SampleLevel(dirtSampler, p, 0.0);
    // Reject the asset's near-black plate while keeping both faint haze and
    // compact dust. A gentle edge bias prevents centre-frame specks from
    // competing with the target the player is aiming at.
    dirt = smoothstep(0.018, 0.42, dirt);
    float2 centered = uv * 2.0 - 1.0;
    float edgeBias = 0.45 + 0.55 * saturate(dot(centered, centered) * 0.65);
    return dirt * edgeBias;
}

// The assembled flare, generated by visbuf_flare_cs at half resolution.
//
// Ghosts, halo, starburst and the anamorphic streak all used to be built here,
// per pixel, at full resolution. They now arrive pre-blurred in one buffer: a
// wide streak and soft veiling glare need a neighbourhood this shader does not
// have, and faking them with many taps was both narrow and expensive.
//
// Bilinear upsample is enough. Everything in the buffer is low-frequency by
// construction, so there is nothing here for a sharper filter to recover.
float3 SampleFlare(float2 uv) {
    return flareInput.SampleLevel(lutSampler, saturate(uv), 0.0).rgb;
}

// How much of the solar disc the depth buffer says is unoccluded.
//
// A 5x5 kernel rather than 3x3: with only nine taps a thin occluder -- a wire,
// a railing, a branch -- swings the average in whole ninths, and because this
// scales the entire flare the result was a visible full-screen flicker as the
// camera moved. Twenty-five taps make each occluder worth 4% instead of 11%,
// which turns that step into a ramp.
//
// An off-screen sun cannot be depth-tested at all: there is no pixel to sample,
// and reading a clamped edge texel would ask an unrelated part of the frame
// whether the sun is blocked. Rather than returning 0 -- which snapped the
// flare off at the frame boundary, the exact artefact the continuous presence
// term exists to avoid -- confidence in the depth test fades out as the sun
// leaves, and the result blends toward unoccluded. Light entering the barrel
// from just outside the frame is not blocked by anything the depth buffer
// knows about.
#if defined(SGE_HIGH_QUALITY_LENS)
float SkyCoverage(float2 uv, uint2 dimensions) {
    float2 p = uv * float2(dimensions) - 0.5;
    int2 base = int2(floor(p));
    float2 f = frac(p);
    int2 last = int2(dimensions) - 1;
    // Filter the binary coverage, not depth: interpolating a foreground depth
    // with sky before thresholding keeps almost the whole edge pixel blocked.
    float a = sceneDepth.Load(int3(clamp(base, int2(0, 0), last), 0)) >= 0.9999;
    float b = sceneDepth.Load(int3(clamp(base + int2(1, 0), int2(0, 0), last), 0)) >= 0.9999;
    float c = sceneDepth.Load(int3(clamp(base + int2(0, 1), int2(0, 0), last), 0)) >= 0.9999;
    float d = sceneDepth.Load(int3(clamp(base + int2(1, 1), int2(0, 0), last), 0)) >= 0.9999;
    return lerp(lerp(a, b, f.x), lerp(c, d, f.x), f.y);
}
#endif

float SunDepthVisibility(float2 sunUV) {
#if defined(SGE_HIGH_QUALITY_LENS)
    uint w, h;
    sceneDepth.GetDimensions(w, h);
    float radius = max(sunLensColor.w, 1.5 / float(h));
    float2 discRadius = radius * float2(float(outputSize.y) / float(outputSize.x), 1.0);
    float visibility = 0.0;
    // A static equal-area disc stays stable in motion and follows the solar
    // angular radius at every render scale, including DLSS.
    [unroll]
    for (int i = 0; i < 12; ++i) {
        float r = sqrt((float(i) + 0.5) / 12.0);
        float angle = float(i) * 2.39996323;
        float2 tap = sunUV + float2(cos(angle), sin(angle)) * r * discRadius;
        float outside = max(max(-tap.x, tap.x - 1.0), max(-tap.y, tap.y - 1.0));
        visibility += lerp(SkyCoverage(tap, uint2(w, h)), 1.0,
                           smoothstep(0.0, 0.06, outside));
    }
    return visibility / 12.0;
#else
    float2 texel = 1.0 / float2(outputSize);
#if defined(SGE_DLSS_SR)
    uint depthWidth, depthHeight;
    sceneDepth.GetDimensions(depthWidth, depthHeight);
    texel = 1.0 / float2(depthWidth, depthHeight);
#endif
    float visibility = 0.0;
    [unroll]
    for (int y = -2; y <= 2; ++y) {
        [unroll]
        for (int x = -2; x <= 2; ++x) {
            float depth = sceneDepth.SampleLevel(
                lutSampler, saturate(sunUV + float2(x, y) * texel * 2.0), 0.0);
            visibility += depth >= 0.9999 ? 1.0 : 0.0;
        }
    }
    visibility *= (1.0 / 25.0);

    // 0 when the sun is comfortably inside the frame, 1 once it is a little way
    // outside. Matches the margin SetSunLens fades presence across.
    float overshoot = max(max(-sunUV.x, sunUV.x - 1.0),
                          max(-sunUV.y, sunUV.y - 1.0));
    float offScreen = smoothstep(0.0, 0.06, overshoot);
    return lerp(visibility, 1.0, offScreen);
#endif
}

float LinearizeDepth(float depth) {
    return nearPlane * farPlane /
        max(farPlane - depth * (farPlane - nearPlane), 1e-4);
}

float3 CinematicInput(uint2 pixel) {
    int2 dimensions = int2(outputSize);
#if defined(SGE_DLSS_SR)
    // The input can be render-sized on a failed DLSS evaluation or display-
    // sized after a successful one. Normalized sampling covers both.
    float2 sceneUV = (float2(pixel) + 0.5) / float2(outputSize);
    float3 color = hdrInput.SampleLevel(lutSampler, sceneUV, 0.0).rgb;
#else
    float3 color = hdrInput.Load(int3(pixel, 0)).rgb;
#endif

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

#if defined(SGE_DLSS_SR)
    float depth = SceneDepthAt(pixel);
#else
    float depth = sceneDepth.Load(int3(pixel, 0));
#endif
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

uint2 StableSurfaceID(uint2 pixel) {
    uint2 rawID = surfaceIDs.Load(int3(pixel, 0));
    if (rawID.x == 0u) return uint2(0u, 0u);

    DrawCallData dc = drawCalls[rawID.x - 1u];
    return uint2(asuint(dc.useNormalMap),
                 stableTriangleIDs[asuint(dc.useTexture) + rawID.y]);
}

float3 TemporalResolve(uint2 pixel, float3 currentColor, uint2 currentID) {
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
        int2 previousPixel = int2(previousUV * float2(outputSize));
        uint2 previousID = stableSurfaceHistory.Load(int3(previousPixel, 0));
        // Background (id 0) has no surface to match, so fall back to accepting
        // it -- sky history is reprojected separately and is safe to keep.
        bool background = currentID.x == 0u;

        // Namespace match is the load-bearing test. Triangle equality is NOT
        // required: a flat wall is many triangles, and a sub-pixel camera shift
        // slides a pixel across a shared edge every frame. Demanding an exact
        // triangle match rejected history on every large flat surface, which
        // showed up as a shimmer that looked like TAA "moving too much".
        bool sameNamespace = currentID.x == previousID.x;

        // Within an instance, accept a neighbouring triangle too. Scanning the
        // 4-neighbourhood for the exact primitive distinguishes "the pixel
        // crossed a triangle edge on the same mesh" (reuse) from "a different
        // part of the mesh folded over itself" (reject) without needing stored
        // barycentrics.
        bool samePrimitive = currentID.y == previousID.y;
        if (sameNamespace && !samePrimitive) {
            [unroll]
            for (int i = 0; i < 4; ++i) {
                const int2 offsets[4] = {
                    int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1) };
                int2 tap = clamp(previousPixel + offsets[i], int2(0, 0),
                                 int2(outputSize) - 1);
                uint2 neighbour = stableSurfaceHistory.Load(int3(tap, 0));
                if (neighbour.x == currentID.x && neighbour.y == currentID.y) {
                    samePrimitive = true;
                    break;
                }
            }
        }
        extensionConfidence =
            (background || (sameNamespace && samePrimitive)) ? 1.0 : 0.0;
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

// NaN/Inf test on the bit pattern. FXC compiles this without IEEE strictness
// and folds isnan() (x != x) to false, so the intrinsic silently never fires.
bool NonFinite(float3 v) {
    return any((asuint(v) & 0x7FFFFFFFu) >= 0x7F800000u);
}

[numthreads(8, 8, 1)]
void main(uint3 threadID : SV_DispatchThreadID) {
    uint2 pixel = threadID.xy;
    if (any(pixel >= outputSize)) return;
    uint2 currentID = uint2(0u, 0u);
    if (surfaceIdentityEnabled != 0u) {
        currentID = StableSurfaceID(pixel);
        stableSurfaceOutput[pixel] = currentID;
    }
    if (debugViewMode != 0u) {
#if defined(SGE_DLSS_SR)
        float4 debugColor = hdrInput.SampleLevel(
            lutSampler, (float2(pixel) + 0.5) / float2(outputSize), 0.0);
#else
        float4 debugColor = hdrInput.Load(int3(pixel, 0));
#endif
        historyOutput[pixel] = debugColor;
        ldrOutput[pixel] = debugColor;
        return;
    }
    // A NaN/Inf scene pixel would otherwise enter TAA history (lerp keeps the
    // NaN at any weight) and persist, and the tonemap renders NaN as black.
    float3 current = CinematicInput(pixel);
    if (NonFinite(current)) current = 0.0;
    float3 hdr = TemporalResolve(pixel, current, currentID);
    if (NonFinite(hdr)) hdr = current;
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
            uint2 previousID = stableSurfaceHistory.Load(int3(previousPixel, 0));
            bool sameNamespace = currentID.x == previousID.x;
            bool samePrimitive = currentID.y == previousID.y;
            if (sameNamespace && !samePrimitive) {
                [unroll]
                for (int i = 0; i < 4; ++i) {
                    const int2 offsets[4] = {
                        int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1) };
                    int2 tap = clamp(previousPixel + offsets[i], int2(0, 0),
                                     int2(outputSize) - 1);
                    uint2 neighbour = stableSurfaceHistory.Load(int3(tap, 0));
                    if (neighbour.x == currentID.x &&
                        neighbour.y == currentID.y) {
                        samePrimitive = true;
                        break;
                    }
                }
            }
            marker = (sameNamespace && samePrimitive)
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
#if defined(SGE_DLSS_SR)
    float rawDepth = SceneDepthAt(pixel);
#else
    float rawDepth = sceneDepth.Load(int3(pixel, 0));
#endif
    bool validationSky = validationMode != 0u && rawDepth >= 0.9999;
    float2 lensUV = (float2(pixel) + 0.5) / float2(outputSize);

    // Lens artefacts are additive energy on top of the scene, gathered before
    // the tonemap so they roll off through the same curve as everything else
    // and cannot clip on their own.
    float3 bloomColor = Bloom(pixel);
    float3 lens = bloomColor * bloomStrength;
    if (validationMode == 0u) {
        float sunResponse = 0.0;
        float sunEnergy = 0.0;
        float3 sunBloom = 0.0;
#if defined(SGE_HIGH_QUALITY_LENS)
        float sunVisibility = 0.0;
        if (sunLensPosition.z > 0.0 &&
            (lensFlareStrength > 0.0 || lensDirtStrength > 0.0))
            sunVisibility = SunDepthVisibility(sunLensPosition.xy);
#endif
        // sunLensPosition.z is a continuous presence, not an on-screen flag:
        // it ramps down across a margin outside the frame so the flare fades
        // as the sun leaves rather than switching off at the boundary. Testing
        // it against a threshold here would put that pop straight back.
        if (sunLensPosition.z > 0.0 &&
            (lensFlareStrength > 0.0 || lensDirtStrength > 0.0)) {
            sunBloom = BloomAt(saturate(sunLensPosition.xy), 0.0);
            sunEnergy = min(Luminance(sunBloom), 8.0);
            const float sourcePresent = smoothstep(0.08, 1.6, sunEnergy);
            const float intensityScale =
                saturate(sunLensPosition.w * (1.0 / 6.0));
            sunResponse = sunLensPosition.z *
#if defined(SGE_HIGH_QUALITY_LENS)
                          sunVisibility *
#else
                          SunDepthVisibility(sunLensPosition.xy) *
#endif
                          sourcePresent * intensityScale;
        }
        // The flare buffer already carries its own presence and intensity from
        // the generating pass, so it is added directly rather than being scaled
        // by sunResponse a second time. Occlusion is the exception: the pass
        // has no depth buffer, so the sun-blocked term is applied here.
        float3 flare = 0.0;
        if (lensFlareStrength > 0.0) {
            flare = SampleFlare(lensUV) *
#if defined(SGE_HIGH_QUALITY_LENS)
                    sunVisibility;
#else
                    max(SunDepthVisibility(sunLensPosition.xy), 0.0);
#endif
            lens += flare;
        }
        // Dirt is lit BY the bloom rather than drawn over the image: grime is
        // still invisible in a dark frame. Local bloom reveals dirt around any
        // bright source, direct solar glare lights the whole front element, and
        // the flare lights it too -- a streak sweeping across a dirty lens
        // picking up the grime it crosses is a large part of the look.
        if (lensDirtStrength > 0.0) {
            float dirt = LensDirtMask(lensUV);
            float3 dirtLight = bloomColor + flare * 0.85 +
                max(sunLensColor.rgb, 0.0) * (sunEnergy * sunResponse * 0.18);
#if defined(SGE_HIGH_QUALITY_LENS)
            // Dust catches local glare; distant solar energy should not light
            // every speck equally across an otherwise dark frame.
            float2 delta = (lensUV - sunLensPosition.xy) *
                float2(float(outputSize.x) / float(outputSize.y), 1.0);
            dirtLight = bloomColor * 0.7 + flare * 0.85 +
                max(sunLensColor.rgb, 0.0) * (sunEnergy * sunResponse *
                exp(-length(delta) * 4.0) * 0.08);
#endif
            lens += dirtLight * dirt * lensDirtStrength * 1.65;
        }
    }

    float3 color = validationSky
        ? TonemapSkyACES(hdr)
        : TonemapAgX((hdr + lens) * exposure * autoExposure);
    color = ApplySceneColorGrade(color);
    float lutScale = 15.0 / 16.0;
    float lutOffset = 0.5 / 16.0;
    if (validationMode == 0u) {
        color = colorLUT.SampleLevel(lutSampler,
            saturate(color) * lutScale + lutOffset, 0).rgb;
    }
    float2 uv = lensUV;
    float2 centered = uv * 2.0 - 1.0;

    // Lateral chromatic aberration: the three channels focus at slightly
    // different radii, so red and blue are resampled along the vector from the
    // frame centre. The offset scales with radius because the effect is zero on
    // the optical axis and worst at the corners. Green is left in place, which
    // keeps luminance where it was and makes the fringing read as colour rather
    // than as a blur.
    if (chromaticAberration > 0.0 && validationMode == 0u) {
        float2 direction = centered * chromaticAberration * 0.01;
#if defined(SGE_HIGH_QUALITY_LENS)
        float radiusSquared = saturate(dot(centered, centered) * 0.5);
#endif
        float2 texel = 1.0 / float2(outputSize);
#if defined(SGE_HIGH_QUALITY_LENS)
        // Express dispersion in display pixels: changing resolution should
        // not turn subtle lens fringing into a many-pixel colour smear.
        float2 radial = centered * float2(float(outputSize.x) / float(outputSize.y), 1.0);
        direction = radial / max(length(radial), 1e-5) * texel *
            min(chromaticAberration * 1.25, 3.0) * radiusSquared * radiusSquared;
#endif
        // Sample the same HDR path so the fringes are tonemapped consistently
        // with the pixel they surround.
#if defined(SGE_HIGH_QUALITY_LENS)
        float3 shiftedR = hdrInput.SampleLevel(
            lutSampler, saturate(uv - direction), 0.0).rgb;
        float3 shiftedB = hdrInput.SampleLevel(
            lutSampler, saturate(uv + direction), 0.0).rgb;
#else
        float3 shiftedR = hdrInput.SampleLevel(
            lutSampler, saturate(uv - direction * texel * outputSize.y), 0.0).rgb;
        float3 shiftedB = hdrInput.SampleLevel(
            lutSampler, saturate(uv + direction * texel * outputSize.y), 0.0).rgb;
#endif
        if (NonFinite(shiftedR)) shiftedR = hdr;
        if (NonFinite(shiftedB)) shiftedB = hdr;
        float3 fringeR = TonemapAgX((shiftedR + lens) * exposure * autoExposure);
        float3 fringeB = TonemapAgX((shiftedB + lens) * exposure * autoExposure);
        fringeR = ApplySceneColorGrade(fringeR);
        fringeB = ApplySceneColorGrade(fringeB);
#if defined(SGE_HIGH_QUALITY_LENS)
        // Match the green channel's display transform before combining them.
        fringeR = colorLUT.SampleLevel(lutSampler,
            saturate(fringeR) * lutScale + lutOffset, 0).rgb;
        fringeB = colorLUT.SampleLevel(lutSampler,
            saturate(fringeB) * lutScale + lutOffset, 0).rgb;
#endif
        color = float3(fringeR.r, color.g, fringeB.b);
    }

    float vignette = smoothstep(1.35, 0.35, dot(centered, centered));
    color *= lerp(1.0, vignette, vignetteStrength);
    color = saturate(color + (Hash12(float2(pixel) + frameIndex * 17.0) - 0.5)
                     * grainStrength);
    ldrOutput[pixel] = float4(color, 1.0);
}
