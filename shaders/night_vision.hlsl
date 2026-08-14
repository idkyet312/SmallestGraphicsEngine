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
    const float sceneLuma = Luma(sceneColor);
    const float kGain = 7.5;
    float intensified = sceneLuma * kGain;
    intensified = intensified / (1.0 + intensified);
    // Lift the floor. A real tube always shows some glow, never true black, and
    // this is what keeps unlit geometry from disappearing into the background.
    intensified = saturate(intensified * 1.06 + 0.05);

    // -- 2. Highlight bloom ---------------------------------------------------
    // Four taps in a rotated cross, wide enough to read as a halo at 1080p.
    // Only the part of each tap above the bloom threshold contributes, so the
    // terrain does not smear -- just the sources that would overload the tube.
    const float2 bloomStep = inverseScreenSize * 4.0;
    float bloom = 0.0;
    bloom += max(0.0, Luma(sceneTexture.Sample(
        linearClamp, input.uv + bloomStep * float2( 1.0,  0.0)).rgb) - 0.55);
    bloom += max(0.0, Luma(sceneTexture.Sample(
        linearClamp, input.uv + bloomStep * float2(-1.0,  0.0)).rgb) - 0.55);
    bloom += max(0.0, Luma(sceneTexture.Sample(
        linearClamp, input.uv + bloomStep * float2( 0.0,  1.0)).rgb) - 0.55);
    bloom += max(0.0, Luma(sceneTexture.Sample(
        linearClamp, input.uv + bloomStep * float2( 0.0, -1.0)).rgb) - 0.55);
    intensified = saturate(intensified + bloom * 0.55);

    // -- 3. Phosphor ----------------------------------------------------------
    // P43-ish green. The red channel is not zero: a pure (0,g,0) image loses all
    // shape in the highlights, and the tiny red/blue admixture keeps bright
    // areas reading as bright rather than clipping to one flat green.
    const float3 kPhosphor = float3(0.18, 1.0, 0.28);
    float3 nightColor = intensified * kPhosphor;

    // -- 4. Tube artefacts ----------------------------------------------------
    // Noise scales with darkness: amplifying a weak signal amplifies its noise
    // too, so shadows are grainy while well-lit areas are relatively clean.
    const float2 noiseUV = input.uv * float2(1920.0, 1080.0);
    const float grain =
        Hash(floor(noiseUV) + frac(time) * 137.0) - 0.5;
    nightColor += grain * 0.16 * (1.0 - intensified * 0.65);

    // Scanlines: a subtle horizontal ripple, kept shallow so it reads as tube
    // texture instead of a CRT filter.
    const float scanline =
        0.94 + 0.06 * sin(input.uv.y * 1400.0 + time * 6.0);
    nightColor *= scanline;

    // Eyepiece vignette. Circular in aspect-corrected space so it stays round
    // on a widescreen target rather than stretching into an oval.
    float2 centered = input.uv - 0.5;
    centered.x *= inverseScreenSize.y / max(inverseScreenSize.x, 1e-6);
    const float radius = length(centered);
    const float vignette = smoothstep(0.75, 0.32, radius);
    nightColor *= vignette;

    nightColor = saturate(nightColor);
    // Blend against the untouched scene so the ramp in and out is continuous.
    return float4(lerp(sceneColor, nightColor, saturate(strength)), 1.0);
}
