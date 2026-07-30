cbuffer SkyBuffer : register(b0) {
    float3 cameraForward;
    float tanHalfFov;
    float3 cameraRight;
    float aspectRatio;
    float3 cameraUp;
    float environmentRotation;
    float3 sunDirection;
    float exposure;
    float3 cameraPosition;
    float time;
    float4 atmosphereParams; // Rayleigh, Mie, Mie g, aerial density
    float4 cloudParams;      // coverage, density, base height, thickness
};

Texture2D skyEquirectangular : register(t0);
SamplerState skySampler : register(s0);

#include "color_grade.hlsli"

struct PSInput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

// Narkowicz ACES approximation; the EXR is linear HDR and the backbuffer is
// 8-bit non-sRGB, so tonemap + gamma happen here.
float3 TonemapACES(float3 x) {
    return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14));
}

float Hash21(float2 p) {
    p = frac(p * float2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return frac(p.x * p.y);
}

float ValueNoise(float2 p) {
    float2 cell = floor(p);
    float2 f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(Hash21(cell), Hash21(cell + float2(1.0, 0.0)), f.x),
                lerp(Hash21(cell + float2(0.0, 1.0)),
                     Hash21(cell + 1.0), f.x), f.y);
}

float CloudNoise(float2 p) {
    float result = 0.0;
    float amplitude = 0.54;
    [unroll] for (uint octave = 0; octave < 3; ++octave) {
        result += ValueNoise(p) * amplitude;
        p = mul(float2x2(1.62, -1.17, 1.17, 1.62), p) + 13.7;
        amplitude *= 0.48;
    }
    return result;
}

float3 PhysicalSky(float3 ray, float3 source) {
    const float rayleighStrength = atmosphereParams.x;
    if (rayleighStrength <= 0.001)
        return source;

    const float mu = clamp(dot(ray, normalize(sunDirection)), -1.0, 1.0);
    const float g = clamp(atmosphereParams.z, 0.0, 0.92);
    const float rayleighPhase = 0.0596831 * (1.0 + mu * mu);
    const float mieDenominator =
        max(1.0 + g * g - 2.0 * g * mu, 1e-4);
    const float miePhase = (1.0 - g * g) /
        max(12.56637 * pow(mieDenominator, 1.5), 1e-4);
    const float horizon = pow(1.0 - saturate(ray.y), 2.25);
    const float sunHeight = saturate(sunDirection.y * 2.2 + 0.18);
    const float airMass = 1.0 / max(0.10, ray.y + 0.18);
    const float3 betaRayleigh = float3(0.22, 0.46, 1.0);
    const float3 sunsetTint = lerp(float3(1.0, 0.34, 0.08),
                                   float3(1.0, 0.82, 0.58), sunHeight);
    const float3 extinction = exp(
        -(betaRayleigh * rayleighStrength * 0.075 +
          atmosphereParams.y * 0.018) * airMass);
    const float3 scattering =
        betaRayleigh * rayleighPhase * rayleighStrength *
            (0.42 + horizon * 1.35) +
        sunsetTint * miePhase * atmosphereParams.y *
            (0.75 + horizon * 0.65);

    // Keep HDRI detail while replacing its flat horizon response with
    // wavelength-dependent atmospheric extinction and in-scattering.
    const float physicalBlend = saturate(
        0.22 * rayleighStrength + horizon * atmosphereParams.w * 0.46);
    float3 physical = source * extinction + scattering * (2.4 + horizon * 1.8);
    return lerp(source, physical, physicalBlend);
}

float4 RaymarchClouds(float3 ray) {
    if (atmosphereParams.x <= 0.001 || ray.y <= 0.015 ||
        cloudParams.x <= 0.001 || cloudParams.y <= 0.001)
        return 0.0;

    const float baseHeight = max(cloudParams.z, cameraPosition.y + 20.0);
    const float thickness = max(cloudParams.w, 50.0);
    float nearT = max((baseHeight - cameraPosition.y) / ray.y, 0.0);
    float farT = max((baseHeight + thickness - cameraPosition.y) / ray.y,
                     nearT + 1.0);
    const float stepLength = (farT - nearT) / 6.0;
    const float2 wind = float2(time * 0.0035, time * 0.0017);
    float3 accumulated = 0.0;
    float transmittance = 1.0;
    const float sunAmount = saturate(sunDirection.y * 1.8 + 0.25);
    const float3 shadowColor = lerp(float3(0.20, 0.28, 0.40),
                                    float3(0.42, 0.48, 0.56), sunAmount);
    const float3 sunColor = lerp(float3(1.0, 0.43, 0.16),
                                 float3(1.0, 0.92, 0.74), sunAmount);

    [unroll] for (uint stepIndex = 0; stepIndex < 6; ++stepIndex) {
        float t = nearT + (stepIndex + 0.5) * stepLength;
        float3 p = cameraPosition + ray * t;
        float height = saturate((p.y - baseHeight) / thickness);
        float verticalShape = smoothstep(0.0, 0.16, height) *
                              (1.0 - smoothstep(0.68, 1.0, height));
        float broad = CloudNoise(p.xz * 0.00042 + wind);
        float detail = ValueNoise(p.xz * 0.00155 - wind * 1.7 + 19.0);
        float coverageThreshold = lerp(0.78, 0.30, cloudParams.x);
        float density = saturate(
            broad * 0.74 + detail * 0.26 - coverageThreshold);
        density *= verticalShape * cloudParams.y;
        float segmentAlpha = 1.0 - exp(-density * 0.72);
        float lightMarch = ValueNoise(
            (p.xz + sunDirection.xz * 180.0) * 0.00055 + wind);
        float3 lighting = lerp(shadowColor, sunColor,
                               0.38 + 0.62 * lightMarch);
        accumulated += transmittance * segmentAlpha * lighting;
        transmittance *= 1.0 - segmentAlpha;
    }
    return float4(accumulated, 1.0 - transmittance);
}

float4 main(PSInput input) : SV_Target {
    float2 ndc = input.uv * 2.0 - 1.0;
    ndc.y = -ndc.y;
    float3 ray = normalize(cameraForward +
                           cameraRight * ndc.x * aspectRatio * tanHalfFov +
                           cameraUp * ndc.y * tanHalfFov);

    float2 skyUV = float2(atan2(ray.z, ray.x) * 0.159154943 + 0.5,
                          acos(clamp(ray.y, -1.0, 1.0)) * 0.318309886);
    // DirectX +Y yaw rotates the environment clockwise when viewed from above.
    // Sampling the source at world longitude + yaw applies that rotation.
    skyUV.x = frac(skyUV.x + environmentRotation * 0.159154943);

    // Pick the mip by how fast longitude changes per pixel. Near the poles a
    // single pixel spans a huge U range, so a coarse mip (whose texels already
    // average many longitudes) removes the radial smear. Deriving LOD from the
    // world-space ray avoids the atan2 wrap seam that fools automatic Sample()
    // gradients into selecting a garbage mip along the +/-180 longitude line.
    uint texW, texH, mipCount;
    skyEquirectangular.GetDimensions(0, texW, texH, mipCount);
    float horiz = length(float2(ray.x, ray.z));           // cos(latitude): ->0 at poles
    float3 dRayX = ddx(ray), dRayY = ddy(ray);
    float angleDeriv = max(length(dRayX), length(dRayY)); // radians of view swept per pixel
    float uTexelsPerPixel = angleDeriv / max(horiz, 1e-3) * 0.159154943 * texW;
    float lod = clamp(log2(max(uTexelsPerPixel, 1.0)), 0.0, (float)(mipCount - 1));
    float3 hdr = skyEquirectangular.SampleLevel(skySampler, skyUV, lod).rgb;
    hdr = PhysicalSky(ray, hdr);
    float4 clouds = RaymarchClouds(ray);
    hdr = hdr * (1.0 - clouds.a * 0.72) + clouds.rgb;

    float3 color;
#ifdef SGE_HDR_TARGET
    color = hdr * exposure;
#else
    color = TonemapACES(hdr * exposure);
    color = pow(color, 1.0 / 2.2);
    color = ApplySceneColorGrade(color);
#endif
    return float4(color, 1.0);
}
