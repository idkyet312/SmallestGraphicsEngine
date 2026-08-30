struct FogCluster
{
    uint lightCount;
    uint lightIndices[32];
};

struct FogPointLight
{
    float3 position;
    float radius;
    float3 color;
    float intensity;
    // Cone axis, unit length; zero scatters in all directions.
    float3 spotDirection;
    float spotCosInner;
    float spotCosOuter;
    int spotShadowIndex;
    // 1 draws a shaft in the fog, 0 lights surfaces only. Must match GPULight
    // in VolumetricFogDX12.h.
    float volumetric;
    float spotPadding;
};

cbuffer FogConstants : register(b0)
{
    float4x4 inverseViewProjection;
    float4x4 shadowCascadeMatrices[3];
    float4 cameraPositionNear;
    float4 cameraForwardFar;
    float4 sunDirectionDensity;
    float4 sunColorAnisotropy;
    float4 fogParams;             // height falloff, base height, max distance, shadow enabled
    float4 ambientFogColor;
    float4 shadowCascadeSplits;
    uint4 clusterDimsLightCount;  // x, y, z, source light count
    uint4 volumeDims;
    float4 atmosphereParams;      // Rayleigh, Mie, Mie g, aerial density
    float4 cloudParams;           // coverage, density, base height, thickness
    float4 oceanBounds0;          // center.x, surface y, center.z, enabled
    float4 oceanBounds1;          // half x, half z, edge blend width, unused
    uint4 maxVolumeDims;          // allocated froxel volume size (>= volumeDims)
#ifdef SGE_WORLD_CLOUDS
    float4 flyableCloudParams;    // base height, thickness, density, coverage
#else
    // The C++ FogConstants declares this unconditionally. Keeping a placeholder
    // here means everything after it sits at the same offset in both builds --
    // without it the spot matrices below would read the cloud params instead.
    float4 flyableCloudParamsUnused;
#endif
    // Spot shadow atlas transforms; see FogConstants in VolumetricFogDX12.h.
    float4x4 spotShadowMatrices[3];
    uint4 spotShadowCount;        // x = live slices, yzw unused
};

StructuredBuffer<FogCluster> clusters : register(t0);
StructuredBuffer<FogPointLight> pointLights : register(t1);
Texture2DArray<float> shadowMap : register(t2);
Texture2D<float> sceneDepth : register(t3);
Texture3D<float4> fogVolume : register(t4);
Texture2DMS<float, 4> sceneDepthMS : register(t5);
#ifdef SGE_WORLD_CLOUDS
Texture3D<float4> cloudShapeVolume : register(t6);
Texture3D<float4> cloudDetailVolume : register(t7);
#endif
// Spot shadow atlas: one slice per shadow-casting spot light (vehicle
// headlights, enemy helicopter searchlights). Not the player flashlight.
// Outside the cloud guard: the root signature declares it either way, so a
// build without world clouds must still bind the same register.
Texture2DArray<float> spotShadowAtlas : register(t8);
RWTexture3D<float4> fogVolumeOut : register(u0);

SamplerComparisonState shadowSampler : register(s0);
SamplerState linearClampSampler : register(s1);
#ifdef SGE_WORLD_CLOUDS
SamplerState cloudNoiseSampler : register(s2);
#endif

float SliceDepth(float slice)
{
    const float nearZ = cameraPositionNear.w;
    const float farZ = min(cameraForwardFar.w, fogParams.z);
    return nearZ * pow(farZ / nearZ, slice / volumeDims.z);
}

float3 WorldRay(float2 uv)
{
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 world = mul(float4(ndc, 1.0, 1.0), inverseViewProjection);
    world.xyz /= max(abs(world.w), 1e-5);
    return normalize(world.xyz - cameraPositionNear.xyz);
}

// rotation lets the caller decorrelate the tap pattern per froxel, so the filter
// reads as soft gradient rather than as a repeated kernel shape.
float SunVisibility(float3 worldPosition, float rotation)
{
    if (fogParams.w < 0.5)
        return 1.0;
    float viewDepth = dot(worldPosition - cameraPositionNear.xyz,
                          normalize(cameraForwardFar.xyz));
    uint cascade = viewDepth < shadowCascadeSplits.x ? 0u :
                   (viewDepth < shadowCascadeSplits.y ? 1u : 2u);
    float4 projected = mul(float4(worldPosition, 1.0),
                           shadowCascadeMatrices[cascade]);
    projected.xyz /= max(abs(projected.w), 1e-5);
    float2 uv = projected.xy * float2(0.5, -0.5) + 0.5;
    if (any(uv < 0.0) || any(uv > 1.0) || projected.z <= 0.0 || projected.z >= 1.0)
        return 1.0;

    // A single comparison tap makes every shaft edge a hard shadow-texel
    // boundary swept along the view ray, which is what read as straight
    // diagonal streaks radiating from the sun. Raising the froxel grid cannot
    // help: the aliasing is in the shadow lookup, not the froxel spacing.
    // Average a small rotated disc of taps so the boundary becomes a gradient
    // the width of the filter.
    const float2 kTaps[4] = {
        float2( 0.936,  0.352), float2(-0.352,  0.936),
        float2(-0.936, -0.352), float2( 0.352, -0.936)
    };
    float sine, cosine;
    sincos(rotation, sine, cosine);
    float2x2 rotate = float2x2(cosine, -sine, sine, cosine);
    // Cascades cover progressively larger areas, so widen in texels, not world
    // units, to keep the softness visually consistent across cascade splits.
    // Kept narrow deliberately: widening this to 3 texels softened the occluder
    // edge enough that sun-facing views bled a bright veil over the hillside in
    // front of the sun -- the same failure the phase clamp below guards against.
    float radius = 1.6 / 2048.0;
    float visibility = shadowMap.SampleCmpLevelZero(
        shadowSampler, float3(uv, cascade), projected.z - 0.0025);
    [unroll]
    for (uint i = 0; i < 4; ++i)
        visibility += shadowMap.SampleCmpLevelZero(
            shadowSampler,
            float3(uv + mul(kTaps[i], rotate) * radius, cascade),
            projected.z - 0.0025);
    return visibility * 0.2;
}

// Occlusion for a shadow-casting spot light, at a point in the air.
//
// Same atlas and transforms as the surface shading, but this samples FROXELS
// rather than geometry: a beam is visible because the air along it scatters,
// so a froxel sitting behind a wall has to be cut here too, or the shaft draws
// straight through the wall the surface pass correctly darkened.
//
// Single tap, no PCF. The froxel grid is far coarser than the screen, so the
// soft edge a kernel would buy is well below what this volume can resolve.
float SpotShadowVisibilityFog(int shadowIndex, float3 worldPosition)
{
    if (shadowIndex < 0 || shadowIndex >= (int)spotShadowCount.x)
        return 1.0;

    float4 lightClip = mul(float4(worldPosition, 1.0),
                           spotShadowMatrices[shadowIndex]);
    if (lightClip.w <= 0.0)
        return 1.0;
    float3 proj = lightClip.xyz / lightClip.w;
    if (any(abs(proj.xy) > 1.0) || proj.z < 0.0 || proj.z > 1.0)
        return 1.0;

    float2 uv = proj.xy * float2(0.5, -0.5) + 0.5;
    // Looser than the surface bias: a froxel centre can sit a metre or more
    // from the surface that occludes it, and a tight bias makes the shaft
    // flicker as froxels cross the depth boundary between frames.
    float bias = 0.004 + 0.006 * saturate(lightClip.w / 40.0);
    return spotShadowAtlas.SampleCmpLevelZero(
        shadowSampler, float3(uv, (float)shadowIndex), proj.z - bias);
}

float HenyeyGreenstein(float cosineTheta, float g)
{
    float g2 = g * g;
    float denominator = max(1.0 + g2 - 2.0 * g * cosineTheta, 1e-4);
    return (1.0 - g2) /
        max(4.0 * 3.14159265 * pow(denominator, 1.5), 1e-4);
}

#ifdef SGE_WORLD_CLOUDS
// Draine's phase function adds the broad water-droplet lobe that HG misses.
// The fixed parameters approximate a 12 um cloud droplet distribution; the HG
// peak supplies the narrow silver lining while Draine carries the bulk energy.
float DrainePhase(float cosineTheta, float g, float alpha)
{
    const float g2 = g * g;
    const float correction = (1.0 + alpha * cosineTheta * cosineTheta) /
        (1.0 + alpha * (1.0 + 2.0 * g2) / 3.0);
    return HenyeyGreenstein(cosineTheta, g) * correction;
}

float CloudWaterDropletPhase(float cosineTheta)
{
    const float forwardPeak = HenyeyGreenstein(cosineTheta, 0.9904);
    const float dropletBulk = DrainePhase(cosineTheta, 0.4417, 23.38);
    // A froxel represents a finite solid angle, so cap the unresolved forward
    // singularity instead of allowing one sun-facing cell to flash white.
    return min(lerp(forwardPeak, dropletBulk, 0.283), 4.0);
}
#endif

// Jimenez's interleaved gradient noise: a cheap hash whose values decorrelate
// strongly between neighbouring pixels, which is what makes the froxel sampling
// offset read as fine dither rather than as a pattern.
float InterleavedGradientNoise(float2 pixel)
{
    return frac(52.9829189 *
        frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

float FogNoise(float3 p)
{
    // Broad moving density cells. Stable in world space; time only advects them.
    p += float3(ambientFogColor.w * 0.42, 0.0, ambientFogColor.w * 0.19);
    float n = sin(p.x * 0.105 + sin(p.z * 0.071)) *
              sin(p.z * 0.093 - p.y * 0.17);
    n += sin((p.x + p.z) * 0.037 + ambientFogColor.w * 0.31) * 0.55;
    return saturate(n * 0.32 + 0.62);
}

#ifdef SGE_WORLD_CLOUDS
float CloudRemap(float value, float lowIn, float highIn,
                 float lowOut, float highOut)
{
    return lowOut + (value - lowIn) /
        max(highIn - lowIn, 0.0001) * (highOut - lowOut);
}

static const float kCloudExtinctionScale = 0.04;

// The old analytic fog modulation was a small stack of sines, so its peaks and
// valleys inevitably repeated across the island. Combine differently rotated
// and incommensurately scaled samples from the generated 3D volumes instead.
// Each source texture still wraps, but their product has no visible tile period
// within the rendered volume.
float FogDensityTexture(float3 worldPosition)
{
    const float wind = ambientFogColor.w;
    const float3 advected = worldPosition +
        float3(wind * 0.31, wind * 0.04, wind * 0.17);
    const float3 broadUVW = advected * 0.0021;
    const float3 rotatedUVW = float3(
        advected.z * 0.00137 + 17.31,
        -advected.x * 0.00137 + 5.73,
        advected.y * 0.00137 + 11.19);
    const float broad = cloudShapeVolume.SampleLevel(
        cloudNoiseSampler, broadUVW, 0.0).r;
    const float rotated = cloudShapeVolume.SampleLevel(
        cloudNoiseSampler, rotatedUVW, 0.0).g;
    const float fine = cloudDetailVolume.SampleLevel(
        cloudNoiseSampler,
        float3(advected.y, advected.z, -advected.x) * 0.0083 + 23.7,
        0.0).r;
    return saturate(broad * 0.50 + rotated * 0.34 + fine * 0.16);
}

// Density of the world cloud field at a world position. This lives in the
// froxel volume rather than the sky pass, so it is depth-tested against scene
// geometry and the camera can pass through it.
//
// Shape R is Perlin-Worley; GBA are rising-frequency Worley octaves. Detail RGB
// are higher-frequency Worley fields used only after the cheap shape test hits.
float FlyableCloudDensityInternal(float3 worldPosition, bool includeDetail)
{
    const float density = flyableCloudParams.z;
    if (density <= 0.001)
        return 0.0;

    const float base = flyableCloudParams.x;
    const float thickness = max(flyableCloudParams.y, 1.0);
    const float height = worldPosition.y - base;
    if (height < 0.0 || height > thickness)
        return 0.0;

    const float normalizedHeight = height / thickness;
    // A compact cumulus envelope: a relatively level condensation base and a
    // longer crown falloff whose silhouette is broken up by the 3D shape field.
    const float profile = smoothstep(0.0, 0.12, normalizedHeight) *
        (1.0 - smoothstep(0.58, 1.0, normalizedHeight));

    // Do not shrink horizontal weather cells with very thin layers. At the
    // 30 m test thickness the old proportional scale repeated every ~43 m.
    // A world-scale floor plus a rotated second sample keeps the same field
    // coherent across the island while the vertical profile remains thin.
    const float shapePeriod = max(thickness / 0.70, 180.0);
    const float shapeFrequency = 1.0 / shapePeriod;
    const float wind = ambientFogColor.w;
    float3 samplePosition = float3(
        worldPosition.x + wind * 0.85,
        height * 0.85,
        worldPosition.z + wind * 0.32);
    const float3 shapeUVW = samplePosition * shapeFrequency;
    const float4 primaryShape = cloudShapeVolume.SampleLevel(
        cloudNoiseSampler, shapeUVW, 0.0);
    const float4 rotatedShape = cloudShapeVolume.SampleLevel(
        cloudNoiseSampler,
        float3(shapeUVW.z * 0.731 + 13.7,
               -shapeUVW.x * 0.731 + 3.1,
               shapeUVW.y * 0.731 + 9.2), 0.0);
    const float4 shape = lerp(primaryShape, rotatedShape, 0.32);
    const float worleyFBM =
        shape.g * 0.625 + shape.b * 0.25 + shape.a * 0.125;
    float cloud = saturate(CloudRemap(
        shape.r, worleyFBM - 1.0, 1.0, 0.0, 1.0));

    const float threshold = lerp(
        0.88, 0.20, saturate(flyableCloudParams.w));
    cloud = saturate(CloudRemap(cloud, threshold, 1.0, 0.0, 1.0));
    cloud *= profile;
    if (!includeDetail || cloud <= 0.001)
        return cloud * density;

    const float3 primaryDetail = cloudDetailVolume.SampleLevel(
        cloudNoiseSampler, shapeUVW * 8.0, 0.0).rgb;
    const float3 rotatedDetail = cloudDetailVolume.SampleLevel(
        cloudNoiseSampler,
        float3(-shapeUVW.z * 9.13 + 4.7,
               shapeUVW.x * 9.13 + 15.3,
               shapeUVW.y * 9.13 + 2.9), 0.0).rgb;
    const float3 detail = lerp(primaryDetail, rotatedDetail, 0.27);
    float detailFBM = detail.r * 0.625 +
        detail.g * 0.25 + detail.b * 0.125;
    // Inverting erosion toward the crown produces cauliflower tops while the
    // lower half keeps heavier, darker bodies instead of dissolving into fog.
    detailFBM = lerp(detailFBM, 1.0 - detailFBM,
                     saturate(normalizedHeight * 3.0));
    cloud = saturate(CloudRemap(
        cloud, detailFBM * 0.38, 1.0, 0.0, 1.0));
    return cloud * density;
}

float FlyableCloudDensity(float3 worldPosition)
{
    return FlyableCloudDensityInternal(worldPosition, true);
}

float FlyableCloudDensityLowDetail(float3 worldPosition)
{
    return FlyableCloudDensityInternal(worldPosition, false);
}

// Return the portion of a ray segment that is physically inside the horizontal
// cloud slab. Logarithmic froxels become long at distance; applying one density
// sample to the whole cell makes a cell that only grazes the cloud base behave
// like hundreds of metres of cloud when viewed from below.
float2 FlyableCloudRayInterval(float3 ray, float segmentStart,
                              float segmentEnd)
{
    const float base = flyableCloudParams.x;
    const float top = base + max(flyableCloudParams.y, 1.0);
    if (abs(ray.y) < 0.00001)
    {
        const bool inside = cameraPositionNear.y >= base &&
                            cameraPositionNear.y <= top;
        return inside ? float2(segmentStart, segmentEnd)
                      : float2(segmentEnd, segmentStart);
    }

    const float firstPlane = (base - cameraPositionNear.y) / ray.y;
    const float secondPlane = (top - cameraPositionNear.y) / ray.y;
    const float slabStart = min(firstPlane, secondPlane);
    const float slabEnd = max(firstPlane, secondPlane);
    return float2(max(segmentStart, slabStart),
                  min(segmentEnd, slabEnd));
}

float3 FlyableCloudGradientNormal(float3 worldPosition)
{
    const float epsilon = max(flyableCloudParams.y * 0.0125, 0.35);
    const float center = FlyableCloudDensityLowDetail(worldPosition);
    const float3 gradient = float3(
        FlyableCloudDensityLowDetail(
            worldPosition + float3(epsilon, 0.0, 0.0)) - center,
        FlyableCloudDensityLowDetail(
            worldPosition + float3(0.0, epsilon, 0.0)) - center,
        FlyableCloudDensityLowDetail(
            worldPosition + float3(0.0, 0.0, epsilon)) - center);
    const float gradientLength = length(gradient);
    return gradientLength > 0.0001
        ? -gradient / gradientLength : float3(0.0, 1.0, 0.0);
}

// Short secondary march through the same density field toward the sun. This is
// the directional cue a vertical-only shadow cannot provide: lobes facing the
// light stay bright while density behind them falls into Beer-Lambert shadow.
float CloudLightTransmittance(float3 worldPosition, float3 lightDirection)
{
    if (lightDirection.y <= 0.01)
        return 0.0;

    const float thickness = max(flyableCloudParams.y, 1.0);
    const float layerTop = flyableCloudParams.x + thickness;
    float travel = (layerTop - worldPosition.y) /
        max(lightDirection.y, 0.05);
    travel = clamp(travel, 0.0, thickness * 4.0);
    if (travel <= 0.01)
        return 1.0;

    const float stepLength = travel * 0.125;
    float opticalDepth = 0.0;
    [unroll]
    for (uint sampleIndex = 0; sampleIndex < 8; ++sampleIndex)
    {
        // Midpoint samples avoid the systematic bright leak produced by
        // sampling the near face of each large light-march interval.
        const float distanceToSample =
            (float(sampleIndex) + 0.5) * stepLength;
        opticalDepth += FlyableCloudDensityLowDetail(
            worldPosition + lightDirection * distanceToSample) * stepLength;
    }
    return exp(-opticalDepth * kCloudExtinctionScale);
}
#endif

float CloudSunVisibility(float3 worldPosition)
{
    if (atmosphereParams.x <= 0.001 || cloudParams.x <= 0.001)
        return 1.0;
    float2 wind = float2(ambientFogColor.w * 2.1,
                         ambientFogColor.w * 1.0);
    float2 projected = worldPosition.xz +
        normalize(sunDirectionDensity.xz + 1e-4) *
        max(cloudParams.z - worldPosition.y, 0.0);
    float clouds = FogNoise(float3(projected * 0.014 + wind, 7.0));
    float threshold = lerp(0.78, 0.26, cloudParams.x);
    float opticalDepth = saturate((clouds - threshold) * 2.2) *
                         cloudParams.y;
    return exp(-opticalDepth * 0.75);
}

float3 AtmosphereAmbient(float3 ray, float aboveBase)
{
    if (atmosphereParams.x <= 0.001)
        return ambientFogColor.xyz;
    float horizon = pow(1.0 - saturate(ray.y), 2.0);
    float3 rayleighColor = float3(0.46, 0.68, 1.0);
    float3 sky = lerp(rayleighColor, ambientFogColor.xyz,
                      0.35 + horizon * 0.45);
    float3 groundBounce = float3(0.36, 0.44, 0.28) *
        exp(-aboveBase * 0.055);
    float3 daylightAmbient = sky * (0.52 + atmosphereParams.x * 0.26) +
        groundBounce * atmosphereParams.w * 0.34;
    // The Rayleigh colour above is daylight irradiance. Reusing it after the sun
    // drops below the horizon turns even low-density fog into a white-blue veil.
    // Keep daylight and dusk on their untouched path; only true night fades to
    // the authored cool fog tint.
    if (sunDirectionDensity.y < 0.06)
    {
        float nightBlend = 1.0 - smoothstep(
            -0.10, 0.06, sunDirectionDensity.y);
        // At night the weather tint describes hue, not a daylight-strength
        // light source. Keep only a near-black trace so fog still has volume
        // around silhouettes without becoming a self-lit grey veil under NVG.
        return lerp(
            daylightAmbient, ambientFogColor.xyz * 0.035, nightBlend);
    }
    return daylightAmbient;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= volumeDims.x || id.y >= volumeDims.y)
        return;

    float2 uv = (float2(id.xy) + 0.5) / float2(volumeDims.xy);
    float3 ray = WorldRay(uv);
    float viewCos = max(dot(ray, normalize(cameraForwardFar.xyz)), 0.08);
    float3 accumulated = 0.0;
    float transmittance = 1.0;
    const float3 lightDirection = normalize(sunDirectionDensity.xyz);
    // Preserve forward scattering without letting a g=0.9 sun-facing view
    // explode into a white veil that erases trees in front of the sun.
    const float phase = min(HenyeyGreenstein(
        dot(lightDirection, ray), sunColorAnisotropy.w), 1.65);

    // Each froxel takes a single shadow sample, so a shaft edge that falls
    // mid-slice snaps to the froxel boundary and the shaft reads as stepped.
    // Raising the grid resolution shrinks those steps but never removes them.
    // Offsetting the sample position within the slice by a per-pixel amount
    // turns that hard step into noise spread across neighbouring froxels.
    //
    // The offset is deliberately NOT advanced per frame. This pass has no
    // history volume and no reprojection (see the cloud-noise comment below),
    // so a frame-varying jitter has nothing to converge against: every frame
    // re-randomises where inside its slice each froxel samples. While the
    // camera is still that reads as mild shimmer, but under rotation the
    // froxel grid is simultaneously remapped onto new world positions, and the
    // two together made the beam change far more than the camera motion
    // justified -- most visible pitching up and down through a headlight shaft.
    //
    // Frozen per pixel, the dither stays a fixed spatial pattern: still enough
    // to break the slice boundary into a ramp, but stable from frame to frame.
    // If a history volume is ever added, restore the frame term -- it is only
    // useful once something averages it.
    float sliceJitter = InterleavedGradientNoise(float2(id.xy));

    [loop]
    for (uint z = 0; z < volumeDims.z; ++z)
    {
        float nearDepth = SliceDepth((float)z);
        float farDepth = SliceDepth((float)z + 1.0);
        float centerDepth = lerp(nearDepth, farDepth, sliceJitter);
        float stepLength = (farDepth - nearDepth) / viewCos;
        float3 worldPosition = cameraPositionNear.xyz + ray * (centerDepth / viewCos);
#ifdef SGE_WORLD_CLOUDS
        float secondDepth = lerp(
            nearDepth, farDepth, frac(sliceJitter + 0.3333333));
        float thirdDepth = lerp(
            nearDepth, farDepth, frac(sliceJitter + 0.6666667));
        float3 secondPosition =
            cameraPositionNear.xyz + ray * (secondDepth / viewCos);
        float3 thirdPosition =
            cameraPositionNear.xyz + ray * (thirdDepth / viewCos);

        const float segmentStart = nearDepth / viewCos;
        const float segmentEnd = farDepth / viewCos;
        const float2 cloudInterval = FlyableCloudRayInterval(
            ray, segmentStart, segmentEnd);
        const float cloudSegmentLength =
            max(cloudInterval.y - cloudInterval.x, 0.0);
        const float cloudSampleJitter =
            InterleavedGradientNoise(float2(id.xy));
        const float cloudSampleStep = cloudSegmentLength * 0.25;
        const float cloudFirstDistance = cloudInterval.x +
            cloudSampleStep * cloudSampleJitter;
        const float cloudSecondDistance = cloudInterval.x +
            cloudSampleStep * (1.0 + cloudSampleJitter);
        const float cloudThirdDistance = cloudInterval.x +
            cloudSampleStep * (2.0 + cloudSampleJitter);
        const float cloudFourthDistance = cloudInterval.x +
            cloudSampleStep * (3.0 + cloudSampleJitter);
        const float3 cloudFirstPosition = cameraPositionNear.xyz +
            ray * cloudFirstDistance;
        const float3 cloudSecondPosition = cameraPositionNear.xyz +
            ray * cloudSecondDistance;
        const float3 cloudThirdPosition = cameraPositionNear.xyz +
            ray * cloudThirdDistance;
        const float3 cloudFourthPosition = cameraPositionNear.xyz +
            ray * cloudFourthDistance;
        const float3 cloudPosition = cameraPositionNear.xyz +
            ray * ((cloudInterval.x + cloudInterval.y) * 0.5);
#endif

        // Jungle gradient: a broad upper haze plus a softer, denser low ground
        // layer that pools in the valley floor. The ground layer's falloff is kept
        // gentle (2x, not a sharp spike) so it fades smoothly into the upper haze
        // with no hard seam where it cuts off across tall grass and terrain.
        float aboveBase = max(worldPosition.y - fogParams.y, 0.0);
        float heightDensity = exp(-aboveBase * fogParams.x);
        float groundLayer = exp(-aboveBase * (fogParams.x * 2.0)) * 0.9;
#ifdef SGE_WORLD_CLOUDS
        // Texture-driven modulation removes the periodic sine cells. Reduce the
        // former 1.9x near-ground pile-up and bound only the local modulation;
        // the user-facing master density remains unrestricted.
        float densityNoise = lerp(
            0.72, 1.10, FogDensityTexture(worldPosition));
        heightDensity = (heightDensity + groundLayer * 0.55) * densityNoise;
        heightDensity = clamp(heightDensity, 0.03, 1.20);
#else
        float densityNoise = lerp(0.58, 1.28, FogNoise(worldPosition));
        heightDensity = (heightDensity + groundLayer) * densityNoise;
        heightDensity = max(heightDensity, 0.03);
#endif
#ifdef SGE_WORLD_CLOUDS
        // Zero remains exact because this variant can run with fog disabled.
        float extinction = max(sunDirectionDensity.w * heightDensity, 0.0);

        // Four stable strata integrate only the part of this logarithmic cell
        // that intersects the cloud slab. Keeping the jitter spatial (not
        // frame-varying) avoids crawl because this pass has no cloud history
        // buffer in which temporal noise could converge.
        float cloudDensity = 0.0;
        if (flyableCloudParams.z > 0.001 && cloudSegmentLength > 0.0)
            cloudDensity = (
                FlyableCloudDensity(cloudFirstPosition) +
                FlyableCloudDensity(cloudSecondPosition) +
                FlyableCloudDensity(cloudThirdPosition) +
                FlyableCloudDensity(cloudFourthPosition)) * 0.25;

        const float cloudExtinction =
            cloudDensity * kCloudExtinctionScale;
        // Beer-Lambert on the view segment. The same law is evaluated along the
        // light ray below; cloud optical depth uses its exact in-slab distance,
        // while ordinary fog continues to occupy the complete froxel.
        const float fogOpticalDepth = extinction * stepLength;
        const float cloudOpticalDepth =
            cloudExtinction * cloudSegmentLength;
        const float totalOpticalDepth = fogOpticalDepth + cloudOpticalDepth;
        float segmentTransmittance = exp(-totalOpticalDepth);
        float cloudSegmentTransmittance = exp(-cloudOpticalDepth);
#else
        float extinction = max(sunDirectionDensity.w * heightDensity, 0.00001);
        float segmentTransmittance = exp(-extinction * stepLength);
#endif

        // Two shadow evaluations per froxel, at different depths inside the
        // slice. Slice thickness grows exponentially with distance, so a single
        // sample makes a shaft edge crossing a far slice snap to that whole
        // slab -- visible as banding across the shaft. Sampling near the front
        // and back of the slice and averaging turns that step into a ramp.
        // Rotate the PCF disc per froxel and per slice so neighbouring samples
        // do not share a kernel orientation; combined with sliceJitter the
        // residual pattern averages into smooth falloff.
        float rotation = (sliceJitter + float(z) * 0.618034) * 6.2831853;
#ifndef SGE_WORLD_CLOUDS
        float secondDepth = lerp(nearDepth, farDepth, frac(sliceJitter + 0.5));
        float3 secondPosition =
            cameraPositionNear.xyz + ray * (secondDepth / viewCos);
#endif
        float shadow = 0.5 * (
            SunVisibility(worldPosition, rotation) +
            SunVisibility(secondPosition, rotation + 1.5707963));
        shadow *= CloudSunVisibility(worldPosition);
        // Daylight fog must replace attenuated scene energy with sky irradiance.
        // A tiny ambient term made the former pass behave like red/brown smoke.
        float3 lighting = AtmosphereAmbient(ray, aboveBase) +
            sunColorAnisotropy.xyz * phase * shadow * 0.42;

#ifdef SGE_WORLD_CLOUDS
        // Dense interiors transition from forward Mie scattering toward a broad
        // backscatter lobe, approximating energy returned by multiple scattering.
        // Beer-powder restores soft bright edges without flattening dark cores.
        if (cloudDensity > 0.001)
        {
            const float lightTransmittance = CloudLightTransmittance(
                cloudPosition, lightDirection);
            const float cosineTheta = dot(lightDirection, ray);
            const float phaseDepth = saturate(
                lightTransmittance * cloudSegmentTransmittance);
            const float forwardPhase = CloudWaterDropletPhase(cosineTheta);
            const float multipleScatterPhase =
                HenyeyGreenstein(cosineTheta, -0.15) * 2.16;
            const float cloudPhase = lerp(
                multipleScatterPhase, forwardPhase, phaseDepth);
            const float powder = 1.0 -
                cloudSegmentTransmittance * cloudSegmentTransmittance;
            const float beerPowder =
                lightTransmittance * (1.0 + powder);
            const float ambientVisibility = lerp(
                0.32, 0.92, sqrt(lightTransmittance));
            const float3 cloudNormal =
                FlyableCloudGradientNormal(cloudPosition);
            const float lobeLighting = lerp(
                0.58, 1.25,
                saturate(dot(cloudNormal, lightDirection) * 0.5 + 0.5));
            float3 cloudLight =
                AtmosphereAmbient(
                    ray, max(cloudPosition.y - fogParams.y, 0.0)) *
                    ambientVisibility +
                sunColorAnisotropy.xyz * cloudPhase * shadow *
                    beerPowder * lobeLighting * 1.35;
            // Mix participating media by their contribution to optical depth,
            // not by the size of the enclosing froxel. This remains correct for
            // a very thin cloud crossing and for ordinary fog being disabled.
            lighting = lerp(lighting, cloudLight,
                saturate(cloudOpticalDepth / max(totalOpticalDepth, 0.00001)));
        }
#endif
        uint3 clusterCoord = min(
            uint3(
                id.x * clusterDimsLightCount.x / volumeDims.x,
                id.y * clusterDimsLightCount.y / volumeDims.y,
                z * clusterDimsLightCount.z / volumeDims.z),
            clusterDimsLightCount.xyz - 1u);
        uint clusterIndex = clusterCoord.x +
            clusterCoord.y * clusterDimsLightCount.x +
            clusterCoord.z * clusterDimsLightCount.x *
                clusterDimsLightCount.y;
        FogCluster cluster = clusters[clusterIndex];
        uint count = min(cluster.lightCount, 32u);
        [loop]
        for (uint i = 0; i < count; ++i)
        {
            uint lightIndex = cluster.lightIndices[i];
            if (lightIndex >= clusterDimsLightCount.w)
                continue;
            FogPointLight light = pointLights[lightIndex];
            // Lights flagged non-volumetric light surfaces but draw no shaft.
            if (light.volumetric < 0.5)
                continue;
            float3 toLight = light.position - worldPosition;
            float distanceToLight = length(toLight);
            float range = saturate(1.0 - distanceToLight / max(light.radius, 0.001));
            float cone = 1.0;
            if (dot(light.spotDirection, light.spotDirection) > 0.0001)
            {
                // Lit froxels inside the cone are what draw the beam itself --
                // the shaft is visible because the air along it scatters, not
                // because anything solid is being lit.
                float3 fromLight = -toLight / max(distanceToLight, 1e-4);
                float cosAngle = dot(fromLight, normalize(light.spotDirection));
                cone = smoothstep(light.spotCosOuter, light.spotCosInner, cosAngle);
                // Forward scattering: a torch beam is far brighter looked into
                // than across, which is what separates a shaft in the air from
                // a uniformly glowing cone.
                float forwardPhase = saturate(dot(-ray, fromLight));
                cone *= 1.0 + 2.5 * forwardPhase * forwardPhase;
                // Cut the shaft where the beam is blocked, so a shadowed
                // headlight does not fog through the wall it is pointed at.
                if (cone > 0.0)
                    cone *= SpotShadowVisibilityFog(light.spotShadowIndex,
                                                    worldPosition);
            }
            lighting += light.color * light.intensity * range * range * cone * 0.10;
        }

        float3 segmentScattering = lighting * (1.0 - segmentTransmittance);
        accumulated += transmittance * segmentScattering;
        transmittance *= segmentTransmittance;
        fogVolumeOut[uint3(id.xy, z)] = float4(accumulated, transmittance);
    }
}

struct VSOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

VSOutput VSMain(uint vertexId : SV_VertexID)
{
    VSOutput output;
    float2 p = float2((vertexId << 1) & 2, vertexId & 2);
    output.uv = p;
    output.position = float4(p * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}

float LinearDepth(float depth)
{
    float nearZ = cameraPositionNear.w;
    float farZ = cameraForwardFar.w;
    return nearZ * farZ / max(farZ - depth * (farZ - nearZ), 1e-5);
}

// The froxel volume is always allocated at the high-res size, but the compute
// pass only fills volumeDims when the high-res toggle is off. Rescale the
// normalised coordinate into the written region, and inset by half a texel so
// bilinear filtering never reaches past the last written froxel into stale data.
float3 FogVolumeUVW(float2 uv, float slice)
{
    float3 written = float3(volumeDims.xyz);
    float3 allocated = float3(maxVolumeDims.xyz);
    float3 scale = written / allocated;
    float3 halfTexel = 0.5 / allocated;

    // Froxel z accumulates the range [SliceDepth(z), SliceDepth(z+1)] and is
    // stored at texel index z, i.e. texel centre (z + 0.5) / gridZ. Sampling the
    // raw slice coordinate is therefore half a texel too far and bilinear blends
    // in the NEXT froxel. Where the depth buffer stops at terrain, that next
    // froxel is the unoccluded volume behind the hill, which bled full sunlight
    // through the slope. Pull back by half a texel and clamp so the deepest
    // sample can never reach past the froxel the surface actually occupies.
    float sliceTexel = max(slice * written.z - 0.5, 0.0);
    float sliceUV = min(sliceTexel, written.z - 1.0) / allocated.z;

    float2 planar = clamp(uv * scale.xy, halfTexel.xy, scale.xy - halfTexel.xy);
    return float3(planar, clamp(sliceUV, halfTexel.z, scale.z - halfTexel.z));
}

float4 FadeFogAtRange(float4 fog, float viewDepth, float fogFar)
{
    // Fog composites as `scattering + scene * transmittance`. Fade toward that
    // operation's neutral value (0 scattering, 1 transmittance), otherwise
    // fading RGB alone leaves a dark band where the volume ends.
    float fade = 1.0 - smoothstep(fogFar * 0.80, fogFar, viewDepth);
    return float4(fog.rgb * fade, lerp(1.0, fog.a, fade));
}

float4 FogAtViewDepth(float2 uv, float viewDepth)
{
    float nearZ = cameraPositionNear.w;
    float fogFar = min(cameraForwardFar.w, fogParams.z);
    float clampedDepth = min(viewDepth, fogFar);
    float slice = log(max(clampedDepth, nearZ) / nearZ) /
                  log(fogFar / nearZ);
    float4 fog = fogVolume.SampleLevel(
        linearClampSampler, FogVolumeUVW(uv, saturate(slice)), 0.0);
    return FadeFogAtRange(fog, viewDepth, fogFar);
}

// As FogAtViewDepth, but holds the volume's far-range value instead of fading
// to neutral past it. The fade exists so terrain emerging from the end of the
// froxel grid does not show a band, which relies on that geometry being far
// enough away to be hidden. A flat sea has no such luxury: "beyond the fog
// volume" is a single view angle, so the fade draws a hard horizontal line
// across the water with haze on one side and none on the other.
float4 FogAtViewDepthSustained(float2 uv, float viewDepth)
{
    float nearZ = cameraPositionNear.w;
    float fogFar = min(cameraForwardFar.w, fogParams.z);
    float clampedDepth = min(viewDepth, fogFar);
    float slice = log(max(clampedDepth, nearZ) / nearZ) /
                  log(fogFar / nearZ);
    return fogVolume.SampleLevel(
        linearClampSampler, FogVolumeUVW(uv, saturate(slice)), 0.0);
}

float OceanSurfaceViewDepth(float2 uv, out float coverage)
{
    coverage = 0.0;
    if (oceanBounds0.w < 0.5)
        return 0.0;

    float3 ray = WorldRay(uv);
    if (abs(ray.y) < 1e-5)
        return 0.0;
    float distanceAlongRay =
        (oceanBounds0.y - cameraPositionNear.y) / ray.y;
    if (distanceAlongRay <= 0.0)
        return 0.0;

    float3 hit = cameraPositionNear.xyz + ray * distanceAlongRay;
    float2 fromCenter = abs(hit.xz - oceanBounds0.xz);
    float edgeDistance = min(
        oceanBounds1.x - fromCenter.x,
        oceanBounds1.y - fromCenter.y);
    if (edgeDistance <= 0.0)
        return 0.0;

    // Water has no DSV, so opaque seabed depth abruptly becomes depth=1 at the
    // terrain clipmap edge. Intersect the actual ocean footprint instead and
    // softly retire that replacement depth only at the ocean's own map edge.
    coverage = smoothstep(
        0.0, max(oceanBounds1.z, 1e-3), edgeDistance);
    return distanceAlongRay * max(
        dot(ray, normalize(cameraForwardFar.xyz)), 0.08);
}

float4 CompositeFog(float2 uv, float deviceDepth)
{
    const float4 neutralFog = float4(0.0, 0.0, 0.0, 1.0);
    bool hasOpaqueDepth = deviceDepth < 0.99999;
    float opaqueViewDepth = hasOpaqueDepth
        ? LinearDepth(deviceDepth) : cameraForwardFar.w;
#ifdef SGE_WORLD_CLOUDS
    // Unlike height fog, the world-cloud volume must also composite over pixels
    // whose depth is the sky. Returning the neutral value there cut an exact
    // horizon-shaped hole through the cloud whenever the camera entered it.
    float4 opaqueFog = hasOpaqueDepth
        ? FogAtViewDepth(uv, opaqueViewDepth)
        : fogVolume.SampleLevel(
            linearClampSampler, FogVolumeUVW(uv, 1.0), 0.0);
#else
    float4 opaqueFog = hasOpaqueDepth
        ? FogAtViewDepth(uv, opaqueViewDepth) : neutralFog;
#endif

    float oceanCoverage;
    float oceanViewDepth = OceanSurfaceViewDepth(uv, oceanCoverage);
    if (oceanCoverage <= 0.0 ||
        (hasOpaqueDepth && oceanViewDepth >= opaqueViewDepth))
        return opaqueFog;

    float4 oceanFog = FogAtViewDepthSustained(uv, oceanViewDepth);
    return lerp(opaqueFog, oceanFog, oceanCoverage);
}

float4 PSMain(VSOutput input) : SV_TARGET
{
#ifdef SGE_WORLD_CLOUDS
    // Filtered depth invents intermediate surfaces along thin silhouettes.
    // With dense clouds behind them those false depths expose alternating
    // cloud/no-cloud pixels, which is especially obvious in palm fronds.
    float deviceDepth = sceneDepth.Load(int3(int2(input.position.xy), 0));
#else
    float deviceDepth = sceneDepth.SampleLevel(linearClampSampler, input.uv, 0.0);
#endif
    return CompositeFog(input.uv, deviceDepth);
}

float4 PSMainMSAA(VSOutput input) : SV_TARGET
{
    int2 pixel = int2(input.position.xy);
    float deviceDepth = sceneDepthMS.Load(pixel, 0);
    [unroll]
    for (uint sampleIndex = 1; sampleIndex < 4; ++sampleIndex)
        deviceDepth = min(deviceDepth, sceneDepthMS.Load(pixel, sampleIndex));
    return CompositeFog(input.uv, deviceDepth);
}
