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
    float  cloudVolumetric;  // 1 = march the 3D volumes, 0 = 2D fallback
    float3 cloudPadding;
};

Texture2D skyEquirectangular : register(t0);
// Baked once at startup by cloud_noise_gen.hlsl. Shape carries the cloud form
// (R Perlin-Worley, GBA Worley), detail erodes its edges into wisps.
Texture3D cloudShapeVolume : register(t1);
Texture3D cloudDetailVolume : register(t2);
SamplerState skySampler : register(s0);
SamplerState cloudSampler : register(s1);

#include "color_grade.hlsli"

struct PSInput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

// AgX (Punchy), shared with the clustered and grass passes. The EXR is linear
// HDR and the backbuffer is 8-bit non-sRGB, so tonemap + gamma happen here --
// tonemapAgXPunchy returns display-encoded colour with the grade applied, so
// this pass must not gamma-correct or grade again on top of it.
//
// Was Narkowicz ACES while everything below the horizon was already AgX, which
// left the sky and the terrain it meets rolling off differently: ACES clips
// bright sun and sky to white far harder, so the horizon showed a seam and the
// sun disc blew out where the terrain held detail.
#include "agx_tonemap.hlsli"

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

float WrappedAngle(float angle) {
    return atan2(sin(angle), cos(angle));
}

float DistantIslandProfile(float azimuth, float center, float width,
                           float height, float seed) {
    const float local = WrappedAngle(azimuth - center) / width;
    const float footprint = 1.0 - smoothstep(0.0, 1.0, abs(local));
    const float broad = ValueNoise(float2(local * 2.8 + seed, seed * 1.73));
    const float detail = ValueNoise(float2(local * 8.5 - seed, seed * 4.11));
    const float ridge = 0.50 + broad * 0.34 + detail * 0.16;
    return height * footprint * ridge;
}

float4 DistantIslandSilhouettes(float3 ray, float3 skyColor) {
    // Fixed world azimuths keep the islands stable while the player turns.
    // Different widths/heights avoid a repeated billboard look around the
    // horizon. Heights are angular because this pass works from view rays.
    const float azimuth = atan2(ray.z, ray.x);
    float profile = 0.0;
    profile = max(profile, DistantIslandProfile(
        azimuth, -2.62, 0.27, 0.044, 2.1));
    profile = max(profile, DistantIslandProfile(
        azimuth, -1.08, 0.18, 0.026, 7.4));
    profile = max(profile, DistantIslandProfile(
        azimuth,  0.68, 0.31, 0.052, 4.8));
    profile = max(profile, DistantIslandProfile(
        azimuth,  2.12, 0.22, 0.033, 9.7));

    // Match the visible edge of main.cpp's 8192 m square ocean. Camera elevation
    // pushes that edge downward in the view; a fixed y mask made islands float
    // whenever the player climbed a hill.
    const float2 oceanDirection =
        ray.xz / max(length(ray.xz), 0.0001);
    const float oceanHalfSpan = 4096.0;
    const float edgeX = oceanDirection.x >= 0.0
        ? oceanHalfSpan - cameraPosition.x
        : -oceanHalfSpan - cameraPosition.x;
    const float edgeZ = oceanDirection.y >= 0.0
        ? oceanHalfSpan - cameraPosition.z
        : -oceanHalfSpan - cameraPosition.z;
    const float distanceX = abs(oceanDirection.x) > 0.0001
        ? edgeX / oceanDirection.x : 100000.0;
    const float distanceZ = abs(oceanDirection.y) > 0.0001
        ? edgeZ / oceanDirection.y : 100000.0;
    const float oceanEdgeDistance = max(min(distanceX, distanceZ), 1.0);
    const float seaLevelDelta = -cameraPosition.y;
    const float horizonY = seaLevelDelta / sqrt(
        oceanEdgeDistance * oceanEdgeDistance +
        seaLevelDelta * seaLevelDelta);

    const float edgeWidth = max(fwidth(ray.y) * 1.4, 0.00035);
    // Slight overlap lets the ocean hide the base without exposing a gap.
    const float islandY = ray.y - horizonY + 0.0035;
    const float aboveWater = smoothstep(
        horizonY - 0.010, horizonY + 0.001, ray.y);
    const float belowRidge =
        1.0 - smoothstep(profile - edgeWidth, profile + edgeWidth, islandY);
    const float footprint = smoothstep(0.001, 0.004, profile);
    const float mask = aboveWater * belowRidge * footprint;

    // Atmospheric blue-grey keeps these distant, while a warm trace preserves
    // sunset coherence. Sky contribution prevents a pasted-on black cutout.
    const float daylight = saturate(sunDirection.y * 3.0 + 0.20);
    const float3 silhouetteTint = lerp(
        float3(0.105, 0.058, 0.040),
        float3(0.045, 0.074, 0.084), daylight);
    const float3 islandColor =
        silhouetteTint + min(skyColor, 0.55) * 0.045;
    return float4(islandColor, mask);
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

    // Night: once the sun is below the horizon it cannot light the atmosphere,
    // so the in-scattering has to fall away with it. Without this the analytic
    // sky keeps adding a lit horizon band on top of the night HDRI and the
    // result reads as dusk no matter how dark the environment map is.
    //
    // Fades over the last few degrees above the horizon rather than switching at
    // zero, so dusk still gets its warm scatter and only true night loses it.
    const float sunUp = smoothstep(-0.10, 0.06, sunDirection.y);
    // A little residual airglow keeps the night sky from going flat black where
    // the HDRI itself is dark, but it is scaled far below the daylight term.
    const float scatterScale = lerp(0.002, 1.0, sunUp);

    // The HDRI passes through `source * extinction`, and at night the thinned
    // atmosphere makes extinction ~1, so the environment map's own bright
    // horizon survives untouched -- then AgX's punchy curve lifts it further
    // into the pale band that reads as dusk. Scaling the source with the same
    // sun fade is what actually darkens it; the scattering fix above only
    // removed the analytic half of the problem.
    //
    // Applied to both sides of the blend so the horizon comes down whether the
    // physical path is mixed in or not.
    const float3 attenuated = source * lerp(0.004, 1.0, sunUp);

    float3 physical = attenuated * extinction +
                      scattering * (2.4 + horizon * 1.8) * scatterScale;
    return lerp(attenuated, physical, physicalBlend);
}

// -- Volumetric clouds --------------------------------------------------------
// Follows Schneider & Vos, "The Real-time Volumetric Cloudscapes of Horizon
// Zero Dawn" (SIGGRAPH 2015), reduced to what a test needs: 3D shape and detail
// volumes, height gradients per cloud type, cheap/expensive stepping and a cone
// light march. No weather map (coverage is a uniform) and no temporal
// reprojection -- see the note at the march loop for what that costs.

// Remaps a value from one range to another, the presentation's workhorse for
// carving one noise field into another.
float Remap(float value, float lowIn, float highIn, float lowOut, float highOut) {
    return lowOut +
        (value - lowIn) / max(highIn - lowIn, 0.0001) * (highOut - lowOut);
}

// Vertical density profile per cloud type. The same 3D noise reads as a flat
// stratus deck, a billowing cumulus or a tall cumulonimbus purely by which
// altitudes are allowed to hold density.
float HeightGradient(float height, float cloudType) {
    // Stratus: thin and low. Cumulus: mid, rounded. Cumulonimbus: full column.
    const float stratus = saturate(Remap(height, 0.0, 0.10, 0.0, 1.0)) *
                          saturate(Remap(height, 0.15, 0.25, 1.0, 0.0));
    const float cumulus = saturate(Remap(height, 0.02, 0.22, 0.0, 1.0)) *
                          saturate(Remap(height, 0.60, 0.90, 1.0, 0.0));
    const float cumulonimbus = saturate(Remap(height, 0.0, 0.12, 0.0, 1.0)) *
                               saturate(Remap(height, 0.85, 1.0, 1.0, 0.0));
    // cloudType 0..0.5 blends stratus->cumulus, 0.5..1 cumulus->cumulonimbus.
    const float lower = lerp(stratus, cumulus, saturate(cloudType * 2.0));
    return lerp(lower, cumulonimbus, saturate((cloudType - 0.5) * 2.0));
}

// Density at a world point. `cheap` skips the detail volume, which is the
// optimisation the whole march is built around: most samples are in empty air
// and only need to answer "is there anything here at all".
float SampleCloudDensity(float3 p, float baseHeight, float thickness,
                         float coverage, float2 wind, bool cheap) {
    const float height = saturate((p.y - baseHeight) / thickness);
    // Wind shears with altitude, so the deck leans rather than sliding rigidly.
    float3 samplePos = p;
    samplePos.xz += wind * (1.0 + height * 0.4);

    // Base shape. R is the Perlin-Worley form; the Worley octaves in GBA carve
    // it into billows.
    const float4 shape = cloudShapeVolume.SampleLevel(
        cloudSampler, samplePos * 0.00035, 0.0);
    const float billows = shape.g * 0.625 + shape.b * 0.25 + shape.a * 0.125;
    float density = Remap(shape.r, billows - 1.0, 1.0, 0.0, 1.0);

    // Cloud type varies across the sky so one frame holds several forms rather
    // than a uniform deck. A weather map would author this; a second low
    // frequency noise stands in for the test.
    const float cloudType = saturate(
        ValueNoise(p.xz * 0.00008 + 41.0) * 1.6 - 0.25);
    density *= HeightGradient(height, cloudType);

    // Coverage. Subtracting then rescaling keeps edges soft instead of
    // clipping the field flat.
    const float coverageThreshold = lerp(0.92, 0.30, coverage);
    density = saturate(Remap(density, coverageThreshold, 1.0, 0.0, 1.0));

    if (cheap || density <= 0.0) return density;

    // Detail erosion. Worley subtracted from the edges turns round blobs into
    // wisps, and inverting it near the cloud top gives the wispier crown real
    // clouds have.
    const float3 detail = cloudDetailVolume.SampleLevel(
        cloudSampler, samplePos * 0.0028, 0.0).rgb;
    float detailFBM = detail.r * 0.625 + detail.g * 0.25 + detail.b * 0.125;
    detailFBM = lerp(detailFBM, 1.0 - detailFBM, saturate(height * 4.0));
    density = saturate(Remap(density, detailFBM * 0.45, 1.0, 0.0, 1.0));
    return density;
}

// Henyey-Greenstein phase. Forward scattering is what puts the bright silver
// rim on a cloud you are looking through toward the sun.
float HenyeyGreenstein(float cosAngle, float g) {
    const float g2 = g * g;
    return (1.0 - g2) /
        (4.0 * 3.14159265 * pow(max(1.0 + g2 - 2.0 * g * cosAngle, 0.0001), 1.5));
}

float4 RaymarchVolumetricClouds(float3 ray) {
    const float baseHeight = max(cloudParams.z, cameraPosition.y + 20.0);
    const float thickness = max(cloudParams.w, 50.0);
    const float coverage = cloudParams.x;

    const float nearT = max((baseHeight - cameraPosition.y) / ray.y, 0.0);
    const float farT = max(
        (baseHeight + thickness - cameraPosition.y) / ray.y, nearT + 1.0);

    // Sample count scales with how obliquely the ray crosses the slab: straight
    // up is a short traverse, toward the horizon is a long one. The
    // presentation ramps 64 at the zenith to 128 at the horizon; halved here
    // because this test has no temporal reprojection to amortise the cost over
    // 16 frames, and a full-rate 128-step march is several milliseconds.
    const int sampleCount = (int)lerp(64.0, 32.0, saturate(ray.y * 1.4));
    const float stepLength = (farT - nearT) / float(sampleCount);
    const float2 wind = float2(time * 12.0, time * 5.5);

    const float sunAmount = saturate(sunDirection.y * 1.8 + 0.25);
    const float nightCloudScale =
        lerp(0.004, 1.0, smoothstep(-0.10, 0.06, sunDirection.y));
    const float3 shadowColor = lerp(float3(0.20, 0.28, 0.40),
                                    float3(0.42, 0.48, 0.56), sunAmount);
    const float3 sunColor = lerp(float3(1.0, 0.43, 0.16),
                                 float3(1.0, 0.92, 0.74), sunAmount);
    const float cosAngle = dot(ray, sunDirection);
    // Two lobes: a strong forward one for the silver lining, a weak backward
    // one so clouds away from the sun are not flatly dark.
    const float phase = max(HenyeyGreenstein(cosAngle, 0.72),
                            HenyeyGreenstein(cosAngle, -0.15) * 0.35);

    // Cone offsets for the light march. Spreading the six taps into a cone
    // toward the sun rather than a straight line approximates light arriving
    // from the whole solid angle of the cloud, which is what softens
    // self-shadowing instead of leaving hard bands.
    const float3 coneOffsets[6] = {
        float3( 0.16, 0.15,  0.16), float3(-0.19, 0.15,  0.13),
        float3( 0.14, 0.15, -0.18), float3(-0.16, 0.15, -0.15),
        float3( 0.02, 0.30,  0.02), float3(-0.02, 0.60, -0.02)
    };

    float3 accumulated = 0.0;
    float transmittance = 1.0;
    int emptySamples = 0;
    bool cheapMarch = true;

    for (int i = 0; i < sampleCount; ++i) {
        const float t = nearT + (float(i) + 0.5) * stepLength;
        const float3 p = cameraPosition + ray * t;

        // Cheap/expensive switching. While the ray is in empty air it only
        // samples the shape volume; the first hit switches to full detail plus
        // lighting, and several consecutive empty expensive samples switch back.
        if (cheapMarch) {
            const float cheapDensity = SampleCloudDensity(
                p, baseHeight, thickness, coverage, wind, true);
            if (cheapDensity > 0.0) {
                // Switch to full sampling and evaluate this same point as an
                // expensive sample below. Deliberately not stepping the loop
                // index backwards: mutating the counter to re-test a point
                // that just returned non-zero is how a march like this hangs.
                cheapMarch = false;
                emptySamples = 0;
            } else {
                continue;
            }
        }

        const float density = SampleCloudDensity(
            p, baseHeight, thickness, coverage, wind, false);
        if (density <= 0.0) {
            if (++emptySamples > 8) { cheapMarch = true; }
            continue;
        }
        emptySamples = 0;

        // Light march: six cone taps toward the sun, accumulating the density
        // between this point and the light.
        float lightDensity = 0.0;
        [unroll] for (int c = 0; c < 6; ++c) {
            const float3 conePos = p + sunDirection * (float(c + 1) * 90.0) +
                                   coneOffsets[c] * (float(c + 1) * 90.0);
            lightDensity += SampleCloudDensity(
                conePos, baseHeight, thickness, coverage, wind, true);
        }

        // Beer-Powder. Beer alone darkens cloud edges, which is backwards --
        // thin edges scatter light forward and read brighter. The powder term
        // restores that.
        const float beer = exp(-lightDensity * 0.85);
        const float powder = 1.0 - exp(-lightDensity * 2.0);
        const float lightEnergy = 2.0 * beer * powder * phase;

        const float3 lighting =
            lerp(shadowColor, sunColor, saturate(lightEnergy)) *
            nightCloudScale;
        const float segmentAlpha =
            1.0 - exp(-density * stepLength * 0.0055 * cloudParams.y);
        accumulated += transmittance * segmentAlpha * lighting;
        transmittance *= 1.0 - segmentAlpha;
        // Nothing behind a fully opaque cloud can contribute.
        if (transmittance < 0.01) break;
    }
    return float4(accumulated, saturate(1.0 - transmittance));
}

float4 RaymarchClouds(float3 ray) {
    if (atmosphereParams.x <= 0.001 || ray.y <= 0.015 ||
        cloudParams.x <= 0.001 || cloudParams.y <= 0.001)
        return 0.0;

    if (cloudVolumetric > 0.5)
        return RaymarchVolumetricClouds(ray);

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
    // Matches PhysicalSky's sunUp fade, so cloud and sky brightness fall off
    // together instead of leaving lit clouds over an unlit sky.
    const float nightCloudScale =
        lerp(0.004, 1.0, smoothstep(-0.10, 0.06, sunDirection.y));
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
        // Unlit clouds still rendered at their daylight shadow colour, which put
        // pale grey slabs across a night sky. Fade them down with the sun on the
        // same curve the atmosphere uses.
        lighting *= nightCloudScale;
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
    const float4 distantIslands = DistantIslandSilhouettes(ray, hdr);
    hdr = lerp(hdr, distantIslands.rgb, distantIslands.a);

    // The night EXR contains a photographic horizon many stops brighter than
    // its star field. Exposure alone cannot tame it reliably because that band
    // is still HDR and the final tone mapper rolls it back toward white. Cap the
    // fully-composited night sky to a blue-black ceiling; daylight and dusk have
    // a zero blend and remain byte-identical through this branch.
    const float nightBlend = 1.0 - smoothstep(-0.10, 0.06, sunDirection.y);
    const float3 nightCeiling = float3(0.035, 0.045, 0.065);
    hdr = lerp(hdr, min(hdr, nightCeiling), nightBlend);

    float3 color;
#ifdef SGE_HDR_TARGET
    color = hdr * exposure;
#else
    // AgX already returns display-encoded, graded colour, so no pow(1/2.2) and
    // no second ApplySceneColorGrade here -- either would double-apply.
    color = tonemapAgXPunchy(max(hdr * exposure, 0.0));
#endif
    return float4(color, 1.0);
}
