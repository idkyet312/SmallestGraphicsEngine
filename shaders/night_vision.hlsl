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
    float2 nightVisionPadding;
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

    // Four same-resolution neighbours provide a tiny local adaptation window.
    // It restores edge and texture contrast that tone mapping compressed, but
    // cannot draw a world-space light volume or the geometric shapes produced by
    // the earlier point/ambient-light workarounds.
    const float2 dx = float2(inverseScreenSize.x, 0.0);
    const float2 dy = float2(0.0, inverseScreenSize.y);
    const float neighbourLuma = 0.25 * (
        Luma(sceneTexture.Sample(linearClamp, input.uv + dx).rgb) +
        Luma(sceneTexture.Sample(linearClamp, input.uv - dx).rgb) +
        Luma(sceneTexture.Sample(linearClamp, input.uv + dy).rgb) +
        Luma(sceneTexture.Sample(linearClamp, input.uv - dy).rgb));
    const float localDetail = sceneLuma - neighbourLuma;
    const float signal = max(sceneLuma + localDetail * 1.35 - 0.002, 0.0);

    const float tubeDrive = signal * gain * 1.4;
    const float kTubeCeiling = 0.70;
    float intensified = tubeDrive / (1.0 + tubeDrive / kTubeCeiling);
    intensified += clamp(localDetail * gain * 0.48, -0.075, 0.075);
    intensified = saturate(intensified + 0.008);

    // -- 2. Halo --------------------------------------------------------------
    // Blooming around a bright source is the signature intensifier artefact:
    // charge spreads sideways off the microchannel plate, so a lamp or a muzzle
    // flash grows a soft ROUND disc far larger than an ordinary render bloom.
    //
    // Keep this local to truly hot sources. A wide two-ring kernel spread the
    // already bright sky and fog across silhouettes, reading as lens blur over
    // the entire image instead of tube bloom around individual lamps.
    //
    // Halo also grows with gain: a tube running wide open in near-total darkness
    // blooms far more off the same light source than one stopped down in
    // twilight, which is why a single torch can wash out a whole dark scene.
    const float haloScale = 1.0 + gain * 0.035;
    float halo = 0.0;
    [unroll]
    for (int step = 0; step < 8; ++step) {
        const float angle = (step + 0.5) * 0.7853981634;
        const float2 offset = float2(cos(angle), sin(angle)) *
                              1.75 * haloScale * inverseScreenSize;
        halo += max(0.0, Luma(sceneTexture.Sample(
            linearClamp, input.uv + offset).rgb) - 0.78);
    }
    intensified = saturate(intensified + halo * (0.22 / 8.0));

    // -- 3. Phosphor ----------------------------------------------------------
    // P43 image-intensifier phosphor green. Small red/blue components retain
    // highlight shape on an RGB display without turning the image cyan or lime.
    const float3 kPhosphor = float3(0.12, 1.0, 0.16);
    float3 nightColor = intensified * kPhosphor;

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
        nightColor = lerp(nightColor, washed * kPhosphor, overload);
        nightColor += overload * 0.16 * kPhosphor;
    }

    // -- 4. Tube artefacts ----------------------------------------------------
    // Noise scales with darkness: amplifying a weak signal amplifies its noise
    // too, so shadows are grainy while well-lit areas are relatively clean.
    const float2 noiseUV = input.uv * float2(1920.0, 1080.0);
    const float grain =
        Hash(floor(noiseUV) + frac(time) * 137.0) - 0.5;
    nightColor += grain * 0.065 * (1.0 - intensified * 0.72);

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
    const float vignette = 1.0 - smoothstep(0.38, 0.50, radius);
    nightColor *= vignette;

    nightColor = saturate(nightColor);
    // Blend against the untouched scene so the ramp in and out is continuous.
    return float4(lerp(sceneColor, nightColor, saturate(strength)), 1.0);
}
