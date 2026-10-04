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

#if defined(SGE_HIGH_QUALITY_LENS)
// A bounded source footprint keeps neighbouring lamps and coarse bloom mips
// from changing every ghost's colour. Outside the sensor, use the sun's own
// radiance instead of stretching the last column of bloom across the lens.
float3 SolarSource() {
    uint w, h;
    bloomInput.GetDimensions(w, h);
    float2 texel = 1.0 / float2(w, h);
    float3 source = SampleBloom(sunUV, 1.0) * 0.4;
    source += SampleBloom(sunUV + float2(texel.x, 0.0) * 2.0, 1.0) * 0.15;
    source += SampleBloom(sunUV - float2(texel.x, 0.0) * 2.0, 1.0) * 0.15;
    source += SampleBloom(sunUV + float2(0.0, texel.y) * 2.0, 1.0) * 0.15;
    source += SampleBloom(sunUV - float2(0.0, texel.y) * 2.0, 1.0) * 0.15;
    float outside = max(max(-sunUV.x, sunUV.x - 1.0),
                        max(-sunUV.y, sunUV.y - 1.0));
    source = lerp(source, max(sunTint, 0.0) * sunEnergy,
                  smoothstep(0.0, 0.06, outside));
    // Bound HDR energy smoothly so an exposed disc cannot blow out the frame.
    return source * (12.0 / (12.0 + max(Luminance(source), 0.0)));
}

float3 CoatedGhost(float2 uv, float2 center, float radius, float rotation,
                   float defocus, float rimWeight) {
    float2 p = (uv - center) * float2(aspect, 1.0);
    float r = length(p);
    float footprint = 1.25 / float(flareSize.y);
    float softness = max(footprint, radius * defocus);
    float dispersion = min(ghostDispersion, 0.03);
    // Most pixels miss the aperture entirely; avoid the profile read and
    // the angular work across the rest of the half-resolution target.
    if (r > radius * (1.0 + dispersion) + softness)
        return 0.0;
    float angle = atan2(p.y, p.x) + rotation;
    const float sector = 0.78539816;
    float polygon = cos(0.39269908) /
        cos(fmod(angle + 6.28318531, sector) - 0.39269908);
    float3 result = 0.0;
    float2 local = p / max(radius * 2.0, 1e-4) + 0.5;
    float textureDetail = ghostProfile.SampleLevel(linearClamp, local, 0.0);
    [unroll]
    for (int c = 0; c < 3; ++c) {
        // Defocused reflections overlap spectrally; a large split turns each
        // aperture into three separate rainbow outlines.
        float scale = 1.0 + (1.0 - float(c)) * dispersion;
        // Defocus rounds blade corners and spreads the rim over several
        // pixels, so dispersion reads as a coating instead of a rainbow outline.
        float edge = radius * scale * lerp(polygon, 1.0, 0.35);
        float coverage = 1.0 - smoothstep(edge - softness, edge + softness, r);
        float q = r / max(edge, 1e-4);
        float rimWidth = max(0.16 + defocus, footprint / max(edge, 1e-4));
        float rim = exp(-pow((q - 0.84) / rimWidth, 2.0));
        float interior = exp(-q * q * 2.0);
        // Most elements are diffuse filled reflections; only one carries a
        // faint coating rim, so the chain cannot read as repeated outlines.
        result[c] = coverage * (interior * 0.14 + rim * rimWeight * 0.035) *
                    lerp(0.9, 1.1, saturate(textureDetail));
    }
    return result;
}

float3 QualityFeatures(float2 uv) {
    float3 source = SolarSource();
    float3 result = 0.0;
    float2 axis = 0.5 - sunUV;
    const float scales[4] = { 0.32, 1.06, 1.78, 2.24 };
    const float radii[4] = { 0.022, 0.073, 0.155, 0.036 };
    const float weights[4] = { 0.65, 0.52, 0.16, 0.38 };
    const float defocus[4] = { 0.12, 0.22, 0.38, 0.16 };
    const float rims[4] = { 0.0, 0.6, 0.0, 0.0 };
    const float3 coatings[4] = {
        float3(1.0, 0.86, 0.64), float3(0.68, 0.82, 0.94),
        float3(0.88, 0.74, 0.84), float3(0.66, 0.82, 0.96)
    };
    float axisFade = smoothstep(0.025, 0.20, length(axis * float2(aspect, 1.0)));
    [unroll]
    for (int i = 0; i < 4; ++i) {
        float2 center = sunUV + axis * scales[i];
        float2 fromAxis = center - 0.5;
        float attenuation = 1.0 / (1.0 + dot(fromAxis, fromAxis) * 2.0);
        result += CoatedGhost(uv, center, radii[i], float(i) * 0.19,
                              defocus[i], rims[i]) * coatings[i] * weights[i] *
            attenuation * source * ghostIntensity * axisFade * 0.45;
    }
    float2 p = (uv - sunUV) * float2(aspect, 1.0);
    float r = length(p);
    // A bright compact core and faint shoulder preserve silhouettes around
    // the source instead of adding an almost uniform amber sheet.
    float glare = exp(-r * r * 180.0) * 0.11 + exp(-r * 7.0) * 0.016;
    result += source * glare * haloIntensity;
    float3 ringRadii = float3(0.243, 0.245, 0.247);
    float3 ringDelta = (r - ringRadii) / max(0.060, 1.5 / float(flareSize.y));
    float3 ring = exp(-ringDelta * ringDelta) * exp(-r * 2.0);
    // A halo's brightest arc faces the optical centre. Breaking its uniform
    // circumference keeps it from reading as a circle drawn over the scene.
    float2 opticalAxis = axis * float2(aspect, 1.0);
    float arc = 0.35 + 0.65 * saturate(0.5 + 0.5 *
        dot(p, opticalAxis) / max(r * length(opticalAxis), 1e-5));
    result += source * float3(0.85, 0.91, 1.0) * ring * arc * haloIntensity * 0.008;

    // Nine blades produce eighteen opposite rays. Cartesian distances avoid
    // angular aliasing at the centre, and pixel footprints soften narrow rays.
    float spikes = 0.0;
    float footprint = 1.0 / float(flareSize.y);
    [unroll]
    for (int blade = 0; blade < 9; ++blade) {
        float angle = float(blade) * 0.34906585 + 0.12;
        float2 direction = float2(cos(angle), sin(angle));
        float along = abs(dot(p, direction));
        float across = abs(dot(p, float2(-direction.y, direction.x)));
        float rayWidth = footprint + 0.0012 + along * 0.012;
        spikes += exp(-pow(across / rayWidth, 2.0)) *
                  exp(-along * 24.0) * (1.0 - exp(-r * 80.0));
    }
    result += source * spikes * starburstIntensity * 0.045;
    return result * sunPresence;
}
#endif

[numthreads(8, 8, 1)]
void FeatureGenCS(uint3 threadID : SV_DispatchThreadID) {
    uint2 pixel = threadID.xy;
    if (pixel.x >= flareSize.x || pixel.y >= flareSize.y) return;
    float2 uv = (float2(pixel) + 0.5) / float2(flareSize);

#if defined(SGE_HIGH_QUALITY_LENS)
    flareOutput[pixel] = float4(
        sunPresence > 0.001 && sunEnergy > 0.001 ? QualityFeatures(uv) : 0.0, 1.0);
#else
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
#endif
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

#if defined(SGE_HIGH_QUALITY_LENS)
    float2 p = (uv - sunUV) * float2(aspect, 1.0);
    float extent = max(streakLength * aspect, 1e-4);
    float along = abs(p.x) / extent;
    float envelope = exp(-along * 3.5) * (1.0 - smoothstep(0.75, 1.0, along));
    float coreWidth = max(1.5 / float(flareSize.y), 0.002);
    float core = exp(-pow(p.y / coreWidth, 2.0));
    float shoulder = exp(-abs(p.y) / (coreWidth * 3.5)) * 0.12;
    float3 streak = SolarSource() * float3(0.32, 0.60, 1.0) *
        (core + shoulder) * envelope * streakIntensity * sunPresence * 0.19;
    flareOutput[pixel] = float4(existing + streak, 1.0);
#else
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
#endif
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
#if defined(SGE_HIGH_QUALITY_LENS)
    // A smaller reconstruction filter retains aperture rims and diffraction
    // rays that a broad blur erases at half resolution.
    float3 sum = flareInput.SampleLevel(linearClamp, uv, 0.0).rgb * 0.5;
    sum += flareInput.SampleLevel(linearClamp, uv - step * 0.75, 0.0).rgb * 0.25;
    sum += flareInput.SampleLevel(linearClamp, uv + step * 0.75, 0.0).rgb * 0.25;
    flareOutput[pixel] = float4(sum, 1.0);
#else
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
#endif
}
