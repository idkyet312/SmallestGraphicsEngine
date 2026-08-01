cbuffer WaterConstants : register(b0)
{
    matrix viewProjection;
    matrix previousViewProjection;
    matrix inverseViewProjection;
    float4 cameraTime;
    float4 previousCameraTime;
    float4 screenParams;
    float4 volume0;       // center.x, surfaceY, center.z, isOcean
    float4 volume1;       // halfX, halfZ, oceanHalfSpan, quality
    float4 clipmapParams; // snappedCenter.xz, micro strength, roughness
    float4 opticalParams; // foam depth, crest threshold, max refraction px, SSR strength
    float4 absorption;
    float4 shallowScatter;
    float4 deepScatter;
    float4 lightDirection;
    float4 lightColor;
    float4 waves[4];      // direction.xy, amplitude, wavelength
    float4 waveExtra[4];  // steepness, unused...
};

Texture2D<float4> sceneColor : register(t0);
#ifdef WATER_DEPTH_MSAA
Texture2DMS<float> sceneDepth : register(t1);
#else
Texture2D<float> sceneDepth : register(t1);
#endif
Texture2D<float4> environmentMap : register(t2);
SamplerState linearClamp : register(s0);
SamplerState pointClamp : register(s1);

struct VSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 texCoord : TEXCOORD;
    float4 tangent : TANGENT;
};

struct VSOutput
{
    float4 position : SV_POSITION;
    float3 worldPosition : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float crest : TEXCOORD2;
    float4 currentClip : TEXCOORD3;
    float4 previousClip : TEXCOORD4;
};

void EvaluateOcean(float2 baseXZ, float time, out float3 position,
                   out float3 normal, out float crest)
{
    const float gravity = 9.81;
    position = float3(baseXZ.x, volume0.y, baseXZ.y);
    float3 tangentX = float3(1.0, 0.0, 0.0);
    float3 tangentZ = float3(0.0, 0.0, 1.0);
    float compression = 0.0;

    [unroll]
    for (uint i = 0; i < 4; ++i) {
        float2 direction = normalize(waves[i].xy);
        float amplitude = waves[i].z;
        float wavelength = max(waves[i].w, 0.1);
        float steepness = waveExtra[i].x;
        float k = 6.28318530718 / wavelength;
        float omega = sqrt(gravity * k);
        float phase = k * dot(direction, baseXZ) - omega * time;
        float sine = sin(phase);
        float cosine = cos(phase);
        float horizontal = steepness * amplitude;

        position.xz += direction * horizontal * cosine;
        position.y += amplitude * sine;

        float common = horizontal * k * sine;
        tangentX += float3(
            -direction.x * direction.x * common,
             direction.x * amplitude * k * cosine,
            -direction.x * direction.y * common);
        tangentZ += float3(
            -direction.x * direction.y * common,
             direction.y * amplitude * k * cosine,
            -direction.y * direction.y * common);
        compression += common;
    }

    normal = normalize(cross(tangentZ, tangentX));
    crest = saturate(compression * 1.8 + (position.y - volume0.y) * 1.25);
}

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    float3 currentPosition;
    float3 previousPosition;
    float3 currentNormal;
    float3 previousNormal;
    float currentCrest;
    float previousCrest;

    if (volume0.w > 0.5) {
        float2 currentXZ = input.position.xz + clipmapParams.xy;
        float2 previousXZ = input.position.xz + previousCameraTime.xy;
        EvaluateOcean(currentXZ, cameraTime.w, currentPosition,
                      currentNormal, currentCrest);
        EvaluateOcean(previousXZ, previousCameraTime.w, previousPosition,
                      previousNormal, previousCrest);
    } else {
        currentPosition = input.position;
        previousPosition = input.position;
        currentNormal = normalize(input.normal);
        previousNormal = currentNormal;
        currentCrest = saturate(
            (input.position.y - volume0.y) / max(opticalParams.y, 0.01));
        previousCrest = currentCrest;
    }

    output.worldPosition = currentPosition;
    output.normal = currentNormal;
    output.crest = currentCrest;
    output.currentClip = mul(float4(currentPosition, 1.0), viewProjection);
    output.previousClip =
        mul(float4(previousPosition, 1.0), previousViewProjection);
    output.position = output.currentClip;
    return output;
}

float LoadOpaqueDepth(float2 uv)
{
    int2 pixel = int2(clamp(
        uv * screenParams.xy, 0.0, screenParams.xy - 1.0));
#ifdef WATER_DEPTH_MSAA
    float depth = sceneDepth.Load(pixel, 0);
    [unroll]
    for (uint sampleIndex = 1; sampleIndex < 4; ++sampleIndex)
        depth = min(depth, sceneDepth.Load(pixel, sampleIndex));
    return depth;
#else
    return sceneDepth.Load(int3(pixel, 0));
#endif
}

float3 ReconstructWorld(float2 uv, float depth)
{
    float2 ndc = uv * float2(2.0, -2.0) + float2(-1.0, 1.0);
    float4 world = mul(float4(ndc, depth, 1.0), inverseViewProjection);
    return world.xyz / max(abs(world.w), 1e-5);
}

float2 ProjectUV(float3 worldPosition)
{
    float4 clipPosition = mul(float4(worldPosition, 1.0), viewProjection);
    return (clipPosition.xy / max(clipPosition.w, 1e-5)) *
        float2(0.5, -0.5) + 0.5;
}

float Hash21(float2 p)
{
    p = frac(p * float2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return frac(p.x * p.y);
}

float SmoothNoise(float2 p)
{
    float2 cell = floor(p);
    float2 f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    return lerp(
        lerp(Hash21(cell), Hash21(cell + float2(1.0, 0.0)), f.x),
        lerp(Hash21(cell + float2(0.0, 1.0)),
             Hash21(cell + float2(1.0, 1.0)), f.x), f.y);
}

float3 SampleEnvironment(float3 direction, float roughness)
{
    direction = normalize(direction);
    float2 uv = float2(
        atan2(direction.z, direction.x) * 0.159154943 + 0.5,
        acos(clamp(direction.y, -1.0, 1.0)) * 0.318309886);
    uint width, height, mipCount;
    environmentMap.GetDimensions(0, width, height, mipCount);
    float lod = roughness * roughness *
        max((float)mipCount - 1.0, 0.0);
    return environmentMap.SampleLevel(linearClamp, uv, lod).rgb;
}

bool TraceWaterReflection(float3 origin, float3 direction, float roughness,
                          float2 pixel, out float2 hitUV, out float confidence)
{
    const uint maxSteps = 24;
    uint stepCount = volume1.w > 0.5 ? maxSteps : 10;
    float maxDistance = volume1.w > 0.5 ? 85.0 : 32.0;
    float stride = maxDistance / stepCount;
    float previousDelta = -0.08;
    float previousT = 0.08;
    float jitter = Hash21(pixel);
    hitUV = 0.0;
    confidence = 0.0;

    [loop]
    for (uint rayStep = 1; rayStep <= maxSteps; ++rayStep) {
        if (rayStep > stepCount) break;
        float t = stride * (rayStep - 0.72 + jitter);
        float3 rayPosition = origin + direction * t;
        float4 clipPosition =
            mul(float4(rayPosition, 1.0), viewProjection);
        if (clipPosition.w <= 0.0) break;
        float2 uv = (clipPosition.xy / clipPosition.w) *
            float2(0.5, -0.5) + 0.5;
        if (any(uv <= 0.002) || any(uv >= 0.998)) break;
        float depth = LoadOpaqueDepth(uv);
        if (depth >= 0.99999) {
            previousT = t;
            continue;
        }
        float3 scenePosition = ReconstructWorld(uv, depth);
        float delta = length(rayPosition - cameraTime.xyz) -
                      length(scenePosition - cameraTime.xyz);
        if (delta >= 0.0 && previousDelta < 0.0) {
            float lo = previousT;
            float hi = t;
            [unroll]
            for (uint refine = 0; refine < 4; ++refine) {
                float mid = (lo + hi) * 0.5;
                float3 midPosition = origin + direction * mid;
                float2 midUV = ProjectUV(midPosition);
                float3 midScene =
                    ReconstructWorld(midUV, LoadOpaqueDepth(midUV));
                if (length(midPosition - cameraTime.xyz) >=
                    length(midScene - cameraTime.xyz))
                    hi = mid;
                else
                    lo = mid;
            }
            hitUV = ProjectUV(origin + direction * hi);
            float edge = min(min(hitUV.x, hitUV.y),
                             min(1.0 - hitUV.x, 1.0 - hitUV.y));
            confidence = smoothstep(0.0, 0.075, edge) *
                saturate(1.0 - hi / maxDistance) *
                (1.0 - roughness * 0.7);
            return confidence > 0.001;
        }
        previousDelta = delta;
        previousT = t;
    }
    return false;
}

float3 ToneMapWater(float3 color)
{
    color = max(color, 0.0);
    color = color / (1.0 + color);
    return pow(color, 1.0 / 2.2);
}

struct PSOutput
{
    float4 color : SV_Target0;
#ifdef WATER_MOTION_OUTPUT
    float2 motion : SV_Target1;
#endif
};

PSOutput PSMain(VSOutput input)
{
    PSOutput output;
    float2 uv = input.position.xy * screenParams.zw;
    float opaqueDepth = LoadOpaqueDepth(uv);

    // Manual depth test permits sampling the same opaque depth surface while
    // keeping water out of the main depth buffer.
    clip(opaqueDepth - input.position.z - 0.00001);
    if (volume0.w > 0.5) {
        float2 fromCenter = abs(input.worldPosition.xz - volume0.xz);
        clip(volume1.xy - fromCenter);
    }

    float3 viewDirection = normalize(cameraTime.xyz - input.worldPosition);
    float3 normal = normalize(input.normal);

    // Derivative-filtered capillary detail. Fine octaves disappear before they
    // alias, leaving stable broad swell toward the horizon.
    float2 p = input.worldPosition.xz;
    float2 micro = 0.0;
    micro += normalize(float2(0.91, 0.41)) *
        cos(dot(p, normalize(float2(0.91, 0.41))) * 1.65 +
            cameraTime.w * 1.15) * 0.030;
    micro += normalize(float2(-0.37, 0.93)) *
        cos(dot(p, normalize(float2(-0.37, 0.93))) * 3.10 -
            cameraTime.w * 1.72) * 0.017;
    micro += normalize(float2(0.58, -0.81)) *
        cos(dot(p, normalize(float2(0.58, -0.81))) * 6.20 +
            cameraTime.w * 2.35) * 0.008;
    float footprint = max(length(ddx(p)), length(ddy(p)));
    float microFade = 1.0 - smoothstep(0.22, 1.35, footprint);
    normal = normalize(normal +
        float3(-micro.x, 0.0, -micro.y) *
        clipmapParams.z * microFade);

    float waterDistance = length(input.worldPosition - cameraTime.xyz);
    float thickness;
    if (opaqueDepth < 0.99999) {
        float3 opaqueWorld = ReconstructWorld(uv, opaqueDepth);
        thickness = length(opaqueWorld - cameraTime.xyz) - waterDistance;
    } else {
        // No seabed behind this pixel. The terrain clipmap stops well short of
        // the ocean mesh (~256 m of terrain against 420 m of water), so beyond
        // its edge there is nothing to measure against. Substituting a fixed
        // distance here made thickness jump straight to its clamped maximum the
        // instant a pixel crossed the terrain boundary, and since thickness
        // drives absorption, the shallow/deep colour blend, and foam, that
        // discontinuity showed up as hard-edged blue and tan patches on the
        // distant water.
        //
        // Ramp with horizontal distance from the shore instead: water gets
        // deeper as it leaves the island, which is both physically sensible and
        // continuous, so the seam has nothing to key off. The scale is chosen so
        // the ramp saturates near the same depth the real seabed reaches.
        float2 fromShore = input.worldPosition.xz - clipmapParams.xy;
        thickness = 6.0 + smoothstep(0.0, 90.0, length(fromShore)) * 24.0;
    }
    thickness = clamp(thickness, 0.02, 30.0);

    // Convert only the horizontal surface slope into a bounded pixel offset.
    // Projecting the full one-metre normal and multiplying that UV displacement
    // by a pixel count caused nearby sand to stretch hundreds of pixels.
    float2 projectedSlope =
        ProjectUV(input.worldPosition + float3(normal.x, 0.0, normal.z)) - uv;
    float2 projectedSlopePixels = projectedSlope * screenParams.xy;
    float projectedLength = length(projectedSlopePixels);
    float2 refractionDirection = projectedLength > 1e-4
        ? projectedSlopePixels / projectedLength
        : 0.0;
    float slopeStrength = saturate(length(normal.xz) * 3.0);
    float refractionPixels = min(
        opticalParams.z,
        lerp(0.35, volume0.w > 0.5 ? 2.5 : 3.5,
             smoothstep(0.08, 4.0, thickness)) * slopeStrength);
    refractionPixels *= smoothstep(0.06, 0.55, thickness);
    float2 refractedUV = clamp(
        uv + refractionDirection * refractionPixels * screenParams.zw,
        screenParams.zw * 1.5, 1.0 - screenParams.zw * 1.5);
    float refractedDepth = LoadOpaqueDepth(refractedUV);
    bool rejectRefraction =
        refractedDepth >= 0.99999 && opaqueDepth < 0.99999;
    if (refractedDepth < 0.99999) {
        float refractedDistance = length(
            ReconstructWorld(refractedUV, refractedDepth) - cameraTime.xyz);
        float refractedThickness = refractedDistance - waterDistance;
        rejectRefraction = rejectRefraction ||
            refractedThickness <= 0.01 ||
            abs(refractedThickness - thickness) >
                max(0.40, thickness * 1.25);
    }
    if (rejectRefraction)
        refractedUV = uv;

    float3 behind = sceneColor.SampleLevel(
        linearClamp, refractedUV, 0.0).rgb;
    float3 transmittance = exp(-absorption.rgb * thickness);
    float depthBlend = smoothstep(0.35, 8.0, thickness);
    float3 scatterColor =
        lerp(shallowScatter.rgb, deepScatter.rgb, depthBlend);
    float3 transmitted = behind * transmittance +
        scatterColor * (1.0 - transmittance);

    float nDotV = saturate(dot(normal, viewDirection));
    const float dielectricF0 = 0.02037;
    float fresnel = dielectricF0 +
        (1.0 - dielectricF0) * pow(1.0 - nDotV, 5.0);
    float shallowWater = 1.0 - smoothstep(0.18, 1.35, thickness);
    float roughness = clamp(
        clipmapParams.w + footprint * 0.018 + shallowWater * 0.05,
        0.07, 0.24);
    float3 reflectionDirection = reflect(-viewDirection, normal);
    float3 reflection = SampleEnvironment(
        reflectionDirection, roughness);

    float2 reflectionUV;
    float reflectionConfidence;
    if (TraceWaterReflection(
            input.worldPosition + normal * 0.06,
            reflectionDirection, roughness,
            input.position.xy, reflectionUV, reflectionConfidence)) {
        float3 screenReflection = sceneColor.SampleLevel(
            linearClamp, reflectionUV, 0.0).rgb;
        reflection = lerp(
            reflection, screenReflection,
            reflectionConfidence * opticalParams.w);
    }

    // GGX sun path. Analytic wave normals plus roughness keep this continuous
    // instead of exposing individual triangles as white wedges.
    float3 lightDir = normalize(lightDirection.xyz);
    float3 halfVector = normalize(viewDirection + lightDir);
    float nDotL = saturate(dot(normal, lightDir));
    float nDotH = saturate(dot(normal, halfVector));
    float vDotH = saturate(dot(viewDirection, halfVector));
    float alpha = roughness * roughness;
    float alpha2 = alpha * alpha;
    float denominator =
        nDotH * nDotH * (alpha2 - 1.0) + 1.0;
    float distribution = alpha2 /
        max(3.14159265 * denominator * denominator, 1e-5);
    float sunFresnel = dielectricF0 +
        (1.0 - dielectricF0) * pow(1.0 - vDotH, 5.0);
    float sunVisibility =
        nDotV / max(nDotV * (1.0 - roughness) + roughness, 1e-4);
    float3 sunSpecular = lightColor.rgb *
        distribution * sunFresnel * sunVisibility * nDotL *
        lerp(0.018, 0.075, 1.0 - shallowWater);

    float foamNoise = SmoothNoise(
        p * 0.47 + float2(cameraTime.w * 0.055, -cameraTime.w * 0.038));
    foamNoise = smoothstep(0.48, 0.78, foamNoise);
    // Foam is a narrow broken line where the water is only centimetres deep,
    // not a white coating over the full shallow shelf.
    float shorelineBand = exp(
        -pow((thickness - 0.13) / max(opticalParams.x * 0.18, 0.07), 2.0));
    float crestFoam = smoothstep(
        opticalParams.y, opticalParams.y + 0.28, input.crest) *
        foamNoise;
    float foam = saturate(
        shorelineBand * foamNoise * 0.72 + crestFoam * 0.58);
    float3 foamColor = float3(0.66, 0.76, 0.72);

    float reflectionWeight =
        saturate(fresnel * 0.94 + 0.035 + roughness * 0.06);
    reflectionWeight *= lerp(0.18, 1.0,
        smoothstep(0.22, 2.2, thickness));
    float3 color = lerp(transmitted, reflection, reflectionWeight);
    color += sunSpecular;
    color = lerp(color, foamColor, foam * 0.82);

#ifndef WATER_HDR_TARGET
    color = ToneMapWater(color);
#endif
    output.color = float4(color, 1.0);
#ifdef WATER_MOTION_OUTPUT
    float2 currentUV = (input.currentClip.xy / input.currentClip.w) *
        float2(0.5, -0.5) + 0.5;
    float2 previousUV = currentUV;
    if (input.previousClip.w > 0.001)
        previousUV = (input.previousClip.xy / input.previousClip.w) *
            float2(0.5, -0.5) + 0.5;
    output.motion = currentUV - previousUV;
#endif
    return output;
}
