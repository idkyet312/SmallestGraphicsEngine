// Anamorphic lens flare, generated at half resolution in its own pass.
//
// Why a separate pass rather than more code inside visbuf_post_cs: the flare
// wants blur. A wide anamorphic streak and soft veiling glare are blurs
// measured in tens of percent of screen width, and a blur that wide is only
// affordable if it runs on a small buffer and can be separated into horizontal
// and vertical stages. Inline in post -- one full-res dispatch, one pixel, no
// neighbourhood -- the only way to fake it was a many-tap gather per pixel, and
// that is both expensive and narrow.
//
// The source is the bloom pyramid, which is already thresholded HDR at half
// resolution with a full mip chain. The flare never reads full-res scene colour
// and never re-thresholds: every highlight it reflects has already been
// selected by the bloom pass.
//
// Passes, in dispatch order:
//   FeatureGenCS -- ghosts, halo and starburst into flare mip 0
//   StreakCS     -- the horizontal anamorphic streak, added on top
//   BlurCS       -- one separable blur so nothing reads as a hard sprite
//
// All three ship at cs_5_0: no wave intrinsics, and every [unroll] loop is
// statically bounded.

Texture2D<float4> bloomInput : register(t0);
Texture2D<float> ghostProfile : register(t1);
Texture2D<float4> flareInput : register(t2);
RWTexture2D<float4> flareOutput : register(u0);
SamplerState linearClamp : register(s0);

cbuffer FlareConstants : register(b0) {
    uint2 flareSize;        // dimensions of the half-res flare target
    float2 sunUV;           // projected sun, deliberately allowed outside [0,1]
    float sunPresence;      // 0..1, already faded by off-screen distance
    float sunEnergy;        // bloom luminance at the source, drives everything
    float3 sunTint;         // light colour, warm
    float ghostIntensity;
    float ghostDispersion;  // per-channel radial split, the chromatic term
    float haloIntensity;
    float starburstIntensity;
    float streakIntensity;
    float streakLength;     // in UV, half-width of the horizontal blur
    float bloomMaxMip;      // highest valid mip in the bloom pyramid
    uint blurDirection;     // BlurCS only: 0 = horizontal, 1 = vertical
    float aspect;           // width / height, keeps circles round
};

float Luminance(float3 color) { return dot(color, float3(0.2126, 0.7152, 0.0722)); }

// Bloom sampling clamps the UV rather than rejecting it. The sun is allowed to
// sit outside the frame, and every consumer here has to cope with that without
// producing a hard boundary -- a rejection test would reintroduce exactly the
// popping this pass exists to remove.
float3 SampleBloom(float2 uv, float mip) {
    return bloomInput.SampleLevel(linearClamp, saturate(uv),
                                  clamp(mip, 0.0, bloomMaxMip)).rgb;
}

// One internal reflection. The aperture profile is an authored texture rather
// than an analytic disc, so the element has irregular glare and a coating ring
// instead of reading as a flat circle.
//
// Dispersion is the point of the per-channel radius: glass bends red, green and
// blue by different amounts, so a real ghost is not a tinted disc but three
// slightly different-sized discs stacked. Sampling the profile at three radii
// is what turns a coloured blob into something that looks refracted.
float3 GhostElement(float2 uv, float2 center, float radius, float dispersion) {
    float2 offset = (uv - center) * float2(aspect, 1.0);
    float distanceToCenter = length(offset);

    float3 element = 0.0;
    // Red widest, blue tightest -- the ordering real crown glass produces.
    const float3 channelScale = float3(1.0 + dispersion, 1.0, 1.0 - dispersion);
    [unroll]
    for (int c = 0; c < 3; ++c) {
        float channelRadius = radius * channelScale[c];
        float normalized = distanceToCenter / max(channelRadius, 1e-4);
        // Sample the profile in the element's own square, fading out past its
        // rim rather than clipping to the texture edge.
        float2 local = offset / max(channelRadius * 2.0, 1e-4) + 0.5;
        float profile = ghostProfile.SampleLevel(linearClamp, saturate(local), 0.0);
        profile = smoothstep(0.012, 0.72, profile);
        float aperture = 1.0 - smoothstep(0.82, 1.06, normalized);
        element[c] = profile * aperture;
    }
    return element;
}

[numthreads(8, 8, 1)]
void FeatureGenCS(uint3 threadID : SV_DispatchThreadID) {
    uint2 pixel = threadID.xy;
    if (pixel.x >= flareSize.x || pixel.y >= flareSize.y) return;
    float2 uv = (float2(pixel) + 0.5) / float2(flareSize);

    float3 result = 0.0;
    // sunPresence already folds in the off-screen ramp and the elevation fade,
    // so a single early-out here covers every reason the flare should be absent
    // and costs nothing on frames where the sun is below the horizon.
    if (sunPresence <= 0.001 || sunEnergy <= 0.001) {
        flareOutput[pixel] = float4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    const float2 toCenter = 0.5 - sunUV;
    const float energy = min(sunEnergy, 12.0);

    // ---- Ghost chain -------------------------------------------------------
    // Eight elements along the optical axis running from the sun through frame
    // centre and out the far side. Scales above 1 overshoot centre, which is
    // what puts ghosts on the opposite side of the frame from the sun.
    //
    // Sized against reference stills rather than from theory: the chain in a
    // real wide-angle flare is dominated by two or three LARGE soft discs --
    // several percent of frame height each -- with smaller elements between
    // them, not by a row of uniformly tiny dots. The big ones are what read as
    // a lens; scaling everything down uniformly reads as dirt on the screen.
    //
    // Nothing is culled for leaving the frame. An earlier version skipped
    // elements whose centre left [0,1] and each one blinked out on its own; the
    // fade below is by distance from centre, which is continuous.
    if (ghostIntensity > 0.0) {
        const float scales[8] = {
            -0.42, 0.22, 0.48, 0.74, 1.02, 1.38, 1.72, 2.15
        };
        const float radii[8] = {
            0.115, 0.026, 0.048, 0.034, 0.082, 0.040, 0.132, 0.068
        };
        const float weights[8] = {
            0.55, 0.90, 0.62, 0.75, 0.70, 0.52, 0.60, 0.34
        };
        // Warm near the source, cooling along the chain: successive coatings
        // reflect progressively shorter wavelengths.
        const float3 tints[8] = {
            float3(1.00, 0.66, 0.36), float3(1.00, 0.86, 0.60),
            float3(0.94, 0.78, 0.48), float3(0.58, 0.74, 0.96),
            float3(0.74, 0.88, 0.72), float3(0.48, 0.64, 0.98),
            float3(0.88, 0.66, 0.94), float3(0.55, 0.70, 0.90)
        };
        float3 ghosts = 0.0;
        [unroll]
        for (int i = 0; i < 8; ++i) {
            float2 center = sunUV + toCenter * scales[i];
            float3 element = GhostElement(uv, center, radii[i], ghostDispersion);
            // Elements far from the optical centre are dimmer: the further off
            // axis a reflection forms, the more of it misses the sensor. The
            // falloff is gentler than it was, because the reference keeps its
            // furthest ghosts clearly visible in the frame corner.
            float2 d = center - 0.5;
            float frameFade = saturate(1.0 - dot(d, d) * 0.75);
            ghosts += element * tints[i] * weights[i] * frameFade;
        }
        result += ghosts * ghostIntensity * energy * 0.16;
    }

    // ---- Veiling glare -----------------------------------------------------
    // The broad warm wash that fills the sky around a sun in frame. This is the
    // largest single feature in reference stills and the one that most says
    // "shot through glass": light scattering off every element in the barrel
    // lifts the whole neighbourhood of the source, well beyond where bloom
    // reaches.
    //
    // Two overlapping falloffs rather than one, because a single exponential
    // either hugs the sun too tightly or flattens into a full-screen fog. The
    // tight lobe carries the bright core, the wide one carries the haze.
    if (haloIntensity > 0.0) {
        float2 offset = (uv - sunUV) * float2(aspect, 1.0);
        float distanceToSun = length(offset);
        float tight = exp(-distanceToSun * 9.0);
        float wide = exp(-distanceToSun * 2.2) * 0.55;
        result += sunTint * (tight + wide) * haloIntensity * energy * 0.075;
    }

    // ---- Halo --------------------------------------------------------------
    // The soft ring that forms around a bright source in a zoom barrel. Built
    // from a coarse bloom mip so it carries the scene's own colour rather than
    // a synthetic tint, pulled inward toward the sun so it hugs the source.
    if (haloIntensity > 0.0) {
        float2 offset = (uv - sunUV) * float2(aspect, 1.0);
        float distanceToSun = length(offset);
        const float haloRadius = 0.28;
        float ring = 1.0 - smoothstep(0.0, 0.16, abs(distanceToSun - haloRadius));
        if (ring > 0.0) {
            // Sample the bloom on the far side of the ring, which is what makes
            // the halo a reflection of the scene rather than a drawn circle.
            float2 sampleUV = sunUV + normalize(offset + 1e-5) *
                              (haloRadius * 0.55) / float2(aspect, 1.0);
            float3 haloColor = SampleBloom(sampleUV, bloomMaxMip - 1.0);
            result += haloColor * ring * haloIntensity * 0.55;
        }
    }

    // ---- Starburst ---------------------------------------------------------
    // Diffraction spikes off the iris blades. Deliberately restrained: BF4's is
    // present but never the subject, so this stays close to the source and
    // falls off fast.
    if (starburstIntensity > 0.0) {
        float2 offset = (uv - sunUV) * float2(aspect, 1.0);
        float distanceToSun = length(offset);
        const float burstRadius = 0.20;
        if (distanceToSun < burstRadius) {
            float angle = atan2(offset.y, offset.x);
            // Odd blade count gives twice as many spikes, which is why real
            // iris diaphragms with 9 blades throw 18 rays.
            const float blades = 9.0;
            float spikes = pow(saturate(abs(cos(angle * blades * 0.5))), 12.0);
            float falloff = pow(saturate(1.0 - distanceToSun / burstRadius), 2.5);
            result += sunTint * spikes * falloff *
                      starburstIntensity * energy * 0.05;
        }
    }

    flareOutput[pixel] = float4(result * sunPresence, 1.0);
}

[numthreads(8, 8, 1)]
void StreakCS(uint3 threadID : SV_DispatchThreadID) {
    uint2 pixel = threadID.xy;
    if (pixel.x >= flareSize.x || pixel.y >= flareSize.y) return;
    float2 uv = (float2(pixel) + 0.5) / float2(flareSize);

    float3 existing = flareInput.SampleLevel(linearClamp, uv, 0.0).rgb;
    if (sunPresence <= 0.001 || streakIntensity <= 0.0) {
        flareOutput[pixel] = float4(existing, 1.0);
        return;
    }

    // The anamorphic streak: the signature of the look. A horizontal-only blur
    // of the bloom pyramid, so every bright object in frame smears sideways the
    // way a cylindrical front element makes it.
    //
    // Taps land on a coarse mip rather than mip 0. That is the whole reason the
    // bloom SRV had to expose its mip chain: 21 taps of an already very blurred
    // mip cover a third of the screen smoothly, where 21 taps of mip 0 would
    // alias into visible banding at the same spacing.
    float3 streak = 0.0;
    float weightSum = 0.0;
    const float mip = max(bloomMaxMip - 2.0, 0.0);
    [unroll]
    for (int i = -10; i <= 10; ++i) {
        float t = float(i) / 10.0;
        float offset = t * streakLength;
        float weight = exp(-t * t * 3.2);
        streak += SampleBloom(uv + float2(offset, 0.0), mip) * weight;
        weightSum += weight;
    }
    streak /= max(weightSum, 1e-4);

    // Cool tint. A real anamorphic streak is blue because the coating on the
    // cylindrical element is tuned for the rest of the spectrum, and it is the
    // single strongest cue that the audience is looking through a lens.
    const float3 streakTint = float3(0.38, 0.62, 1.0);
    flareOutput[pixel] = float4(
        existing + streak * streakTint * streakIntensity * sunPresence, 1.0);
}

[numthreads(8, 8, 1)]
void BlurCS(uint3 threadID : SV_DispatchThreadID) {
    uint2 pixel = threadID.xy;
    if (pixel.x >= flareSize.x || pixel.y >= flareSize.y) return;
    float2 uv = (float2(pixel) + 0.5) / float2(flareSize);
    float2 texel = 1.0 / float2(flareSize);

    // Separable 9-tap gaussian, run twice by the caller with blurDirection
    // flipped. Softens the ghost rims and the starburst so the features sit in
    // light rather than reading as pasted-on sprites; the streak is already
    // smooth and is unharmed by it.
    float2 step = blurDirection == 0u ? float2(texel.x, 0.0)
                                      : float2(0.0, texel.y);
    float3 sum = 0.0;
    float weightSum = 0.0;
    [unroll]
    for (int i = -4; i <= 4; ++i) {
        float weight = exp(-float(i * i) * 0.22);
        sum += flareInput.SampleLevel(
            linearClamp, uv + step * float(i) * 1.5, 0.0).rgb * weight;
        weightSum += weight;
    }
    flareOutput[pixel] = float4(sum / max(weightSum, 1e-4), 1.0);
}
