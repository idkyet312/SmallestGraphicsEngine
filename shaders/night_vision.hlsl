// Night vision goggles: an image-intensifier emulation run as the last colour
// pass before the HUD.
//
// The look comes from four things a real intensifier tube does, in this order:
//   1. Gain. The tube amplifies whatever photons arrive, so a night scene that
//      renders near black is lifted until the terrain reads. Gain is applied to
//      luminance, not per channel, because the tube has no colour information to
//      preserve -- everything downstream of the photocathode is monochrome.
//   2. Bloom on the bright end. Sources brighter than the tube can handle spill
//      across neighbouring pixels; this is what makes muzzle flashes and lamps
//      flare so aggressively through goggles.
//   3. Phosphor. The monochrome signal is re-emitted by a green phosphor
//      screen, which is why the whole image is green rather than grey.
//   4. Tube artefacts. Sensor noise (visible because the gain amplifies it),
//      scanlines, and the vignette of looking down a cylindrical eyepiece.
Texture2D sceneTexture : register(t0);
Texture2D<float> sceneDepth : register(t1);
SamplerState linearClamp : register(s0);

cbuffer NightVisionConstants : register(b0) {
    float2 inverseScreenSize;
    // Seconds, wrapped by the CPU. Drives the noise so grain crawls between
    // frames instead of freezing into a static dot pattern.
    float  time;
    // 0 = off, 1 = fully engaged. Ramped by the CPU so raising and lowering the
    // goggles is a visible movement rather than an instant swap.
    float  strength;
    // Automatic gain control. A real tube does not run at a fixed amplification:
    // it backs the gain off as the scene brightens and winds it up in darkness,
    // over roughly a second. The CPU runs that time constant and hands the
    // settled value here, which is what makes stepping from dark treeline into
    // a lit compound bloom out and then recover.
    float  gain;
    // How far the tube is being driven past its handling range. 0 in proper
    // darkness; rises toward 1 in daylight, where an intensifier washes out into
    // a bright featureless field rather than showing a usable image.
    float  overload;
    // Camera near/far, used to linearise the depth buffer so the IR illuminator
    // can fall off with real world distance.
    float  cameraNear;
    float  cameraFar;
};

struct ScreenVertex {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

ScreenVertex VSMain(uint vertexId : SV_VertexID) {
    const float2 position = vertexId == 0 ? float2(-1.0, -1.0)
                          : vertexId == 1 ? float2(-1.0,  3.0)
                                          : float2( 3.0, -1.0);
    ScreenVertex output;
    output.position = float4(position, 0.0, 1.0);
    output.uv = position * float2(0.5, -0.5) + 0.5;
    return output;
}

float Luma(float3 color) {
    return dot(color, float3(0.299, 0.587, 0.114));
}

// Cheap value noise. A hash rather than a texture so the pass stays
// self-contained and needs no extra SRV slot.
float Hash(float2 p) {
    return frac(sin(dot(p, float2(127.1, 311.7))) * 43758.5453);
}

float4 PSMain(ScreenVertex input) : SV_Target {
    const float3 sceneColor = sceneTexture.Sample(linearClamp, input.uv).rgb;
    // Nothing to do when the goggles are stowed. Early out keeps the cost off
    // the frame entirely rather than blending an unchanged image.
    if (strength <= 0.001)
        return float4(sceneColor, 1.0);

    // -- 1. Gain --------------------------------------------------------------
    // Amplify hard, then compress: the tube saturates, so the curve has to roll
    // off or every light source becomes a flat white disc with no shape.
    //
    // Gain arrives from the CPU rather than being a constant here, because
    // automatic gain control is a temporal effect: the tube takes about a second
    // to adapt, and a per-pixel shader has no memory between frames to run that
    // from. A fixed 7.5 made the goggles behave identically in a pitch-dark
    // jungle and a floodlit compound, which is the single least realistic thing
    // an intensifier can do.
    // This pass receives the display-encoded backbuffer, so decoding it and then
    // trying to recreate lost HDR illumination made deep shadows black. Work in
    // the signal we actually have and use a bounded tube response instead. The
    // shoulder keeps fog and sky separated rather than driving both to neon
    // green when automatic gain is wide open.
    const float sceneLuma = Luma(saturate(sceneColor));

    // Feed the resolved luminance directly into the tube. The old neighbour
    // subtraction behaved like an unsharp-mask filter, turning every silhouette
    // and texture boundary into a bright game-like outline.
    const float signal = max(sceneLuma - 0.002, 0.0);

    // Lower drive into a higher ceiling. The old 1.4 drive against a 0.70
    // ceiling put every lit surface deep into the compression knee: a 16x range
    // of scene luminance (0.05 to 0.80) came out spanning barely 2.5x, and
    // above ~0.35 the curve was flat enough that ground, walls and foliage all
    // resolved to the same pale value. Driving the tube less hard and letting it
    // run closer to full scale keeps the midtones on the linear part of the
    // response, which is where the picture's contrast lives.
    const float tubeDrive = signal * gain * 0.8;
    const float kTubeCeiling = 0.95;
    float intensified = tubeDrive / (1.0 + tubeDrive / kTubeCeiling);
    intensified = saturate(intensified + 0.008);

    // Re-expand the tube's compressed range with a gentle toe. Keep enough of
    // the weak signal to read terrain and foliage instead of separating every
    // surface into hard black and bright green bands.
    const float tubeSignal = saturate(
        (intensified - 0.010) / (kTubeCeiling - 0.010));
    intensified = pow(tubeSignal, 1.10);

    // -- 1b. IR illuminator ---------------------------------------------------
    // The goggles' own IR lamp. It is bore-sighted with the tube and rides the
    // head, so whatever you look at is what it lights -- which in screen space
    // is simply a bright pool at the centre falling off toward the edges. That
    // is why an illuminator does not help you scan: the lit patch follows your
    // gaze rather than staying put in the world.
    //
    // Applied to the tube signal, before the phosphor, so the extra light goes
    // through the same response curve the rest of the image does instead of
    // being pasted on as a white overlay.
    //
    // Multiplicative on existing signal rather than additive: a lamp reveals
    // surfaces that are already there. Adding a flat term would light empty sky
    // and distant nothing just as strongly as a wall a metre away.
    {
        // Cone angle: full strength through the middle of the beam, feathering
        // out well before the eyepiece vignette so the two do not fight.
        float2 lampUV = input.uv - 0.5;
        // Aspect-correct so the cone stays round rather than stretching into an
        // oval on a widescreen target.
        lampUV.x *= inverseScreenSize.y / max(inverseScreenSize.x, 1e-6);
        const float lampAngle = 1.0 - smoothstep(0.08, 0.36, length(lampUV));

        // Distance falloff -- what makes this a spotlight rather than a
        // vignette. Reverse-Z aware: whichever end of the depth range is near,
        // linearise to a world distance in metres.
        const float rawDepth = sceneDepth.Sample(linearClamp, input.uv).r;
        const float n = max(cameraNear, 1e-4);
        const float f = max(cameraFar, n + 1e-3);
        // Standard LH perspective depth inverse. Guard the denominator so a
        // cleared depth texel (1.0 = far plane, nothing drawn) resolves to the
        // far plane instead of dividing by zero.
        const float denom = max(f - rawDepth * (f - n), 1e-4);
        float viewDistance = (n * f) / denom;
        if (rawDepth >= 0.9999) viewDistance = f;   // sky: unlit

        // Inverse-square with a soft near clamp, cut off past the lamp's useful
        // throw. A real illuminator lights the first several metres hard and is
        // useless well before the far plane, which is exactly why it gives away
        // position without helping you see distance.
        const float kLampRange = 26.0;       // metres to effective cutoff
        const float kLampReference = 6.0;    // distance at which gain is 1.0
        const float distanceTerm =
            (kLampReference * kLampReference) /
            max(viewDistance * viewDistance, kLampReference * kLampReference);
        const float rangeCutoff = 1.0 - smoothstep(kLampRange * 0.55,
                                                   kLampRange, viewDistance);

        // Weakest where the tube is already saturated. A lamp adds nothing to a
        // surface bright enough to bloom on its own, and pushing there only
        // flattens the highlights the contrast curve above recovered.
        const float lampHeadroom = 1.0 - smoothstep(0.35, 0.85, intensified);
        const float kIlluminatorGain = 1.15;
        intensified = saturate(intensified *
            (1.0 + kIlluminatorGain * lampAngle * distanceTerm *
                   rangeCutoff * lampHeadroom));
    }

    // -- 2. Halo --------------------------------------------------------------
    // Blooming around a bright source is the signature intensifier artefact:
    // charge spreads sideways off the microchannel plate, so a lamp or a muzzle
    // flash grows a soft ROUND disc far larger than an ordinary render bloom.
    //
    // Halo also grows with gain: a tube running wide open in near-total darkness
    // blooms far more off the same light source than one stopped down in
    // twilight, which is why a single torch can wash out a whole dark scene.
    const float haloScale = 1.0 + gain * 0.035;
    float nearHalo = 0.0;
    float farHalo = 0.0;
    [unroll]
    for (int step = 0; step < 8; ++step) {
        const float angle = (step + 0.5) * 0.7853981634;
        const float2 direction = float2(cos(angle), sin(angle));
        const float2 nearOffset = direction *
                                  2.5 * haloScale * inverseScreenSize;
        const float2 farOffset = direction *
                                 10.0 * haloScale * inverseScreenSize;
        nearHalo += max(0.0, Luma(sceneTexture.Sample(
            linearClamp, input.uv + nearOffset).rgb) - 0.80);
        farHalo += max(0.0, Luma(sceneTexture.Sample(
            linearClamp, input.uv + farOffset).rgb) - 0.88);
    }
    intensified = saturate(intensified +
        nearHalo * (0.34 / 8.0) + farHalo * (0.20 / 8.0));

    // Roll the hottest tube response into green instead of letting broad lit
    // surfaces ride at the output ceiling. The knee starts above the useful
    // midrange, preserving terrain detail while taking the white edge off the
    // tower, weapon highlights, and bloomed light sources.
    // Gentle roll-off only. An earlier, much harder knee (0.34 start, 0.58 cut)
    // was tuned against a saturated-green highlight that looked hot; the pale
    // mint highlight below carries brightness far better, so cutting the top end
    // that hard now just makes the image muddy instead of bright and washed.
    const float highlightKnee = smoothstep(0.60, 1.0, intensified);
    intensified *= lerp(1.0, 0.86, highlightKnee);

    // -- 3. Phosphor ----------------------------------------------------------
    // P43 image-intensifier phosphor green. Real phosphor is darker and less
    // saturated in a weak part of the image, then shifts toward its bright green
    // emission as signal rises. That small luminance-dependent variation keeps
    // the picture from reading as one flat green overlay while staying visibly
    // monochrome.
    // Blue-green tube rather than the traditional P43 green: shadows sit in a
    // deep teal and the bright end washes to a pale cyan-white. Modern white
    // phosphor and digital intensifiers read far closer to this than to the
    // saturated green of older tubes, and the cooler cast keeps foliage and
    // terrain separable instead of collapsing everything into one hue.
    //
    // Blue leads green at the top end while green leads at the bottom, so the
    // image shifts cooler as it brightens rather than simply getting lighter.
    const float3 kPhosphorShadow = float3(0.10, 0.34, 0.38);
    const float3 kPhosphorHighlight = float3(0.60, 0.92, 1.00);
    // Widened to the full range. Saturating the shadow-to-highlight blend at
    // 0.66 meant everything above that shared one colour, so the brightest
    // two-thirds of the picture stopped varying in hue as well as in level.
    const float phosphorBlend = smoothstep(0.04, 0.95, intensified);
    const float3 phosphor = lerp(
        kPhosphorShadow, kPhosphorHighlight, phosphorBlend);
    float3 nightColor = intensified * phosphor;

    // Daylight overload. Point an intensifier at a lit scene and it does not
    // show a nice green picture -- the tube is driven far past saturation and
    // the image washes out into a bright, low-contrast field with the detail
    // burned out of it. The deployment screen already warns the goggles are of
    // "little use at this time of day"; this is what makes that true in play
    // rather than only in the briefing text.
    if (overload > 0.001) {
        // Crush contrast toward the tube's saturated output and lift the floor,
        // so the picture goes flat and milky instead of merely bright.
        const float washed = lerp(intensified, 0.82, overload * 0.75);
        nightColor = lerp(
            nightColor, washed * kPhosphorHighlight, overload);
        nightColor += overload * 0.16 * kPhosphorHighlight;
    }

    // -- 4. Tube artefacts ----------------------------------------------------
    // Noise scales with darkness: amplifying a weak signal amplifies its noise
    // too, so shadows are grainy while well-lit areas are relatively clean.
    const float2 noiseUV = input.uv / max(inverseScreenSize, 1e-6);
    // Updating in discrete steps gives the grain the restless crawl of an
    // intensifier tube without dissolving into smooth shimmer at high FPS.
    const float noiseFrame = floor(time * 24.0);
    const float2 frameOffset = float2(
        noiseFrame * 17.0, noiseFrame * 47.0);
    const float fineGrain =
        Hash(floor(noiseUV) + frameOffset) - 0.5;
    const float mediumGrain =
        Hash(floor(noiseUV * 0.50) - frameOffset * 0.37) - 0.5;
    const float coarseGrain =
        Hash(floor(noiseUV * 0.18) + frameOffset * 0.11) - 0.5;
    const float grain = fineGrain * 0.45 +
                        mediumGrain * 0.35 + coarseGrain * 0.20;
    const float grainStrength = lerp(0.135, 0.045, intensified);
    nightColor += grain * grainStrength * kPhosphorHighlight;

    // Real phosphor screens also have a faint fixed unevenness beneath the
    // moving sensor noise. Large cells make that texture survive downsampling
    // in screenshots without reading as a second scanline pattern.
    const float fixedPattern = Hash(
        floor(noiseUV * 0.22) + float2(19.0, 73.0)) - 0.5;
    nightColor *= 1.0 + fixedPattern * 0.11 *
                  (1.0 - intensified * 0.35);

    // Scanlines: a subtle horizontal ripple, kept shallow so it reads as tube
    // texture instead of a CRT filter.
    const float scanline =
        0.97 + 0.03 * sin(input.uv.y * 1400.0 + time * 6.0);
    nightColor *= scanline;

    // Eyepiece vignette. Circular in aspect-corrected space so it stays round
    // on a widescreen target rather than stretching into an oval.
    float2 centered = input.uv - 0.5;
    centered.x *= inverseScreenSize.y / max(inverseScreenSize.x, 1e-6);
    const float radius = length(centered);
    // A single intensifier tube has a circular field of view, not a widescreen
    // rectangle. Keep the central 76% of the screen height clear, then feather
    // to black at a radius of half the screen height. Aspect correction above
    // keeps that boundary physically round on ultrawide and 16:9 displays.
    // Tighter circle with a harder edge: an eyepiece shows a well-defined disc
    // of image inside an opaque tube wall, not a broad gradual falloff. The old
    // 0.38-0.50 feather spent a quarter of the radius fading, which reads as a
    // soft screen-space vignette rather than the edge of an optic.
    const float vignette = 1.0 - smoothstep(0.34, 0.42, radius);
    nightColor *= vignette;

    nightColor = saturate(nightColor);
    // Blend against the untouched scene so the ramp in and out is continuous.
    return float4(lerp(sceneColor, nightColor, saturate(strength)), 1.0);
}
