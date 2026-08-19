cbuffer AOConstants : register(b0)
{
    float4x4 inverseViewProjection;
    float4x4 viewProjection;
    float4 cameraNearFar; // xyz camera, w near
    float4 lightDirection;
    float4 aoParams;      // radius, strength, bias, far
    float4 screenParams;  // width, height, inv width, inv height
    float4 filterParams;  // static depth, surface buffer
    float4 contactParams; // grass, direct contact, noise frame, ambient in resolve
    // Resolution of the AO trace target: width, height, 1/width, 1/height.
    // Equals screenParams at full res. When the trace runs at half res these
    // differ, and the distinction matters in two separate places:
    //
    //   * scattering AO taps and stepping finite differences is a TRACE-space
    //     operation and must use traceParams.zw, or the sampling footprint
    //     silently doubles;
    //   * addressing the depth/normal textures is a SOURCE-space operation and
    //     must keep using screenParams.xy, because those textures are always
    //     full resolution.
    //
    // Conflating the two is what produced the horizontal banding in the
    // earlier half-res attempt: with a half-res screenParams a trace pixel
    // centre (j+0.5)/540 maps to source row 1.0, 3.0, 5.0 -- exactly on texel
    // BOUNDARIES rather than centres. A point sampler there resolves to one of
    // two adjacent texels depending on float rounding, so whole scanlines
    // snapped one way and their neighbours the other. The bands were wrong
    // occlusion, which is why they scaled with AO strength, and why rebasing
    // the rotation dither never touched them. TraceUVToSourceUV below re-centres
    // the sample onto a source texel centre and removes the tie entirely.
    float4 traceParams;
};

// Map a trace-space UV to the source-resolution UV whose texel centre it sits
// in. At full res this is the identity. At half res it shifts the sample by
// half a source texel so point-sampled depth/normal fetches land unambiguously
// inside one texel instead of on the boundary between two.
float2 TraceUVToSourceUV(float2 uv)
{
    return uv + 0.5 * (screenParams.zw - traceParams.zw);
}

Texture2D<float> sceneDepth : register(t0);
Texture2DMS<float, 4> sceneDepthMS : register(t1);
Texture2D<float> staticCasterDepth : register(t2);
Texture2D<float4> surfaceData : register(t3);
Texture2D<float> rawAO : register(t4);
Texture2D<float> grassCoverage : register(t5);
Texture2D<float4> aoHistory : register(t6);
Texture2D<float2> motionVectors : register(t7);
Texture2D<float4> bentAO : register(t8);
SamplerState pointClamp : register(s0);
SamplerState linearClamp : register(s1);

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
    output.position =
        float4(p * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}

float LinearDepth(float depth)
{
    const float nearZ = cameraNearFar.w;
    const float farZ = aoParams.w;
    return nearZ * farZ /
        max(farZ - depth * (farZ - nearZ), 1e-5);
}

float3 ReconstructWorld(float2 uv, float depth)
{
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 world = mul(float4(ndc, depth, 1.0), inverseViewProjection);
    return world.xyz / max(abs(world.w), 1e-5);
}

// uv is SOURCE-space here. Trace-space callers pass TraceUVToSourceUV(uv).
float SampleDepth(float2 uv, bool multisampled)
{
    if (!multisampled)
        return sceneDepth.SampleLevel(pointClamp, saturate(uv), 0.0);
    int2 pixel = clamp(int2(saturate(uv) * screenParams.xy),
                       int2(0, 0), int2(screenParams.xy) - 1);
    float depth = sceneDepthMS.Load(pixel, 0);
    [unroll]
    for (uint sampleIndex = 1; sampleIndex < 4; ++sampleIndex)
        depth = min(depth, sceneDepthMS.Load(pixel, sampleIndex));
    return depth;
}

float3 ReconstructNormal(float2 uv, float depth, bool multisampled)
{
    float3 stored = 0.0;
    float storedValid = 0.0;
    if (filterParams.y > 0.5) {
        stored = surfaceData.SampleLevel(pointClamp, uv, 0.0).xyz;
        float depthMatch = 1.0;
        if (filterParams.x > 0.5) {
            // Mesh terrain and other forward extensions update sceneDepth after
            // visibility resolve. Reject a stored normal when it belongs to
            // geometry now hidden behind that forward surface.
            float visibilityDepth =
                staticCasterDepth.SampleLevel(pointClamp, uv, 0.0);
            float sceneLinear = LinearDepth(depth);
            float visibilityLinear = LinearDepth(visibilityDepth);
            float tolerance = max(0.025, sceneLinear * 0.003);
            depthMatch = step(
                abs(sceneLinear - visibilityLinear), tolerance);
        }
        storedValid = depthMatch * step(0.25, dot(stored, stored));
    }
    // Return the stored G-buffer normal before reconstructing one from depth.
    // The reconstruction below costs four SampleDepth calls, and the final line
    // of this function discarded all of it whenever storedValid held -- which is
    // essentially every pixel on the visibility path, where normalRoughness is
    // always bound. BilateralAO calls this 25 times per pixel, so that was ~100
    // wasted depth fetches per pixel. Output is unchanged: this returns exactly
    // what the trailing select returned.
    if (storedValid > 0.5) return normalize(stored);

    float3 center = ReconstructWorld(uv, depth);
    float2 texel = screenParams.zw;
    float depthL = SampleDepth(uv - float2(texel.x, 0.0), multisampled);
    float depthR = SampleDepth(uv + float2(texel.x, 0.0), multisampled);
    float depthU = SampleDepth(uv - float2(0.0, texel.y), multisampled);
    float depthD = SampleDepth(uv + float2(0.0, texel.y), multisampled);
    float3 dxL = center - ReconstructWorld(uv - float2(texel.x, 0.0), depthL);
    float3 dxR = ReconstructWorld(uv + float2(texel.x, 0.0), depthR) - center;
    float3 dyU = center - ReconstructWorld(uv - float2(0.0, texel.y), depthU);
    float3 dyD = ReconstructWorld(uv + float2(0.0, texel.y), depthD) - center;
    float3 dx = length(dxL) < length(dxR) ? dxL : dxR;
    float3 dy = length(dyU) < length(dyD) ? dyU : dyD;
    float3 normal = normalize(cross(dx, dy));
    float3 toCamera = normalize(cameraNearFar.xyz - center);
    normal = dot(normal, toCamera) < 0.0 ? -normal : normal;
    return normal;
}

float InterleavedNoise(float2 pixel)
{
    return frac(52.9829189 *
        frac(dot(pixel, float2(0.06711056, 0.00583715))));
}

float2 EncodeBentNormal(float3 normal)
{
    normal /= abs(normal.x) + abs(normal.y) + abs(normal.z) + 1e-6;
    float2 encoded = normal.xy;
    if (normal.z < 0.0)
        encoded = (1.0 - abs(encoded.yx)) *
            float2(encoded.x >= 0.0 ? 1.0 : -1.0,
                   encoded.y >= 0.0 ? 1.0 : -1.0);
    return encoded * 0.5 + 0.5;
}

float3 DecodeBentNormal(float2 encoded)
{
    float2 f = encoded * 2.0 - 1.0;
    float3 normal = float3(f, 1.0 - abs(f.x) - abs(f.y));
    if (normal.z < 0.0) {
        float2 folded = (1.0 - abs(normal.yx)) *
            float2(normal.x >= 0.0 ? 1.0 : -1.0,
                   normal.y >= 0.0 ? 1.0 : -1.0);
        normal.xy = folded;
    }
    return normalize(normal);
}

// xy = octahedral bent normal, z = visibility, w = linear depth.
float4 HorizonSignal(float2 uv, float deviceDepth, bool multisampled)
{
    if (deviceDepth >= 0.99999)
        return float4(0.5, 0.5, 1.0, aoParams.w);
    float3 center = ReconstructWorld(uv, deviceDepth);
    float3 normal = ReconstructNormal(uv, deviceDepth, multisampled);
    float centerDepth = LinearDepth(deviceDepth);
    float radius = aoParams.x;
    float pixelRadius = clamp(radius * screenParams.y /
        max(centerDepth * 1.15, 0.1), 3.0, 56.0);
    float rotation = InterleavedNoise(
        uv * screenParams.xy + contactParams.z * float2(23.0, 59.0)) *
        6.2831853;
    float2x2 rotate = float2x2(cos(rotation), -sin(rotation),
                               sin(rotation), cos(rotation));
    float occlusion = 0.0;
    float3 openTangentSum = 0.0;
    float openWeightSum = 0.0;

#if SGE_AO_OPTIMIZED
    // The per-step jitter depends only on the pixel and the step index, not on
    // the slice. Recomputing it inside the slice loop evaluated the same four
    // values eight times over; lift them into a per-step table instead.
    float stepScales[4];
    [unroll]
    for (uint jitterStep = 1; jitterStep <= 4; ++jitterStep) {
        stepScales[jitterStep - 1] = (jitterStep - 0.35 + InterleavedNoise(
            uv * screenParams.xy + jitterStep * 17.0 +
            contactParams.z * float2(11.0, 31.0))) / 4.0;
    }
    float inverseRadius = 1.0 / max(radius, 1e-3);
    float maxSampleDistance = radius * 2.0;
#endif

    [unroll]
    for (uint slice = 0; slice < 8; ++slice) {
        float angle = (slice + 0.5) * (6.2831853 / 8.0);
        float2 direction = mul(float2(cos(angle), sin(angle)), rotate);
        float horizon = 0.0;
        [unroll]
        for (uint step = 1; step <= 4; ++step) {
#if SGE_AO_OPTIMIZED
            float stepScale = stepScales[step - 1];
#else
            float stepScale = (step - 0.35 + InterleavedNoise(
                uv * screenParams.xy + step * 17.0 +
                contactParams.z * float2(11.0, 31.0))) / 4.0;
#endif
            float2 sampleUV =
                uv + direction * pixelRadius * stepScale * screenParams.zw;
            if (any(sampleUV <= 0.0) || any(sampleUV >= 1.0)) continue;
            float sampleDepth = SampleDepth(sampleUV, multisampled);
            if (sampleDepth >= 0.99999) continue;
            float3 delta = ReconstructWorld(sampleUV, sampleDepth) - center;
#if SGE_AO_OPTIMIZED
            float squaredDistance = dot(delta, delta);
            // Compare against the squared limit so taps outside the radius
            // never pay for a sqrt, then take the reciprocal length once and
            // reuse it for both the cosine and the falloff.
            if (squaredDistance < 1e-8 ||
                squaredDistance > maxSampleDistance * maxSampleDistance)
                continue;
            float inverseDistance = rsqrt(squaredDistance);
            float distanceToSample = squaredDistance * inverseDistance;
            float cosine = dot(normal, delta * inverseDistance);
            float distanceFalloff =
                saturate(1.0 - distanceToSample * inverseRadius);
#else
            float distanceToSample = length(delta);
            if (distanceToSample < 1e-4 || distanceToSample > radius * 2.0)
                continue;
            float cosine = dot(normal, delta / distanceToSample);
            float distanceFalloff =
                saturate(1.0 - distanceToSample / max(radius, 1e-3));
#endif
            horizon = max(horizon,
                saturate(cosine - aoParams.z) * distanceFalloff);
        }
        occlusion += horizon;

        // Project the screen-space slice into the receiver tangent plane.
        // Accumulating full hemisphere vectors made their large normal terms
        // dominate the directional imbalance: one blocked slice bent the
        // result by only a few degrees and was visually indistinguishable from
        // scalar AO. Accumulate only the open tangent imbalance here, then add
        // the receiver normal once below.
        float2 tangentUV = uv + direction * screenParams.zw * 2.0;
        float3 tangentDelta =
            ReconstructWorld(tangentUV, deviceDepth) - center;
        tangentDelta -= normal * dot(tangentDelta, normal);
        float tangentLength = length(tangentDelta);
        if (tangentLength > 1e-5) {
            float openWeight = saturate(1.0 - horizon);
            openTangentSum += tangentDelta / tangentLength * openWeight;
            openWeightSum += openWeight;
        }
    }
    occlusion *= aoParams.y / 8.0;
    float visibility = saturate(1.0 - occlusion);
    float directionalImbalance =
        length(openTangentSum) / max(openWeightSum, 1.0);
    float3 bentOffset = directionalImbalance > 1e-5
        ? normalize(openTangentSum) *
          min(directionalImbalance * 3.0, 1.0)
        : 0.0;
    float3 bentNormal = normalize(normal + bentOffset);
    return float4(
        EncodeBentNormal(bentNormal), visibility, centerDepth);
}

// Baseline scalar GTAO retained for the toggle-off path. Keep its arithmetic
// independent of bent-normal accumulation so opting out preserves the existing
// image and resource footprint.
float HorizonVisibility(float2 uv, float deviceDepth, bool multisampled)
{
    if (deviceDepth >= 0.99999) return 1.0;
    // Re-centre onto a source texel so the point-sampled depth/normal fetches
    // below are unambiguous at half res (see TraceUVToSourceUV).
    float2 sourceUV = TraceUVToSourceUV(uv);
    float3 center = ReconstructWorld(sourceUV, deviceDepth);
    float3 normal = ReconstructNormal(sourceUV, deviceDepth, multisampled);
    float centerDepth = LinearDepth(deviceDepth);
    float radius = aoParams.x;
    // Radius in TRACE pixels: the taps are stepped with traceParams.zw, so the
    // pixel count and the step size must agree or the world-space footprint
    // changes with trace resolution.
    float pixelRadius = clamp(radius * traceParams.y /
        max(centerDepth * 1.15, 0.1), 3.0, 56.0);
    // Dither on source pixel coordinates so the pattern is resolution-stable.
    float rotation = InterleavedNoise(sourceUV * screenParams.xy) * 6.2831853;
    float2x2 rotate = float2x2(cos(rotation), -sin(rotation),
                               sin(rotation), cos(rotation));
    float occlusion = 0.0;

    [unroll]
    for (uint slice = 0; slice < 8; ++slice) {
        float angle = (slice + 0.5) * (6.2831853 / 8.0);
        float2 direction = mul(float2(cos(angle), sin(angle)), rotate);
        float horizon = 0.0;
        [unroll]
        for (uint step = 1; step <= 4; ++step) {
            float stepScale = (step - 0.35 +
                InterleavedNoise(sourceUV * screenParams.xy + step * 17.0)) / 4.0;
            // Step in trace space, then resolve to a source texel centre.
            float2 sampleUV = TraceUVToSourceUV(
                uv + direction * pixelRadius * stepScale * traceParams.zw);
            if (any(sampleUV <= 0.0) || any(sampleUV >= 1.0)) continue;
            float sampleDepth = SampleDepth(sampleUV, multisampled);
            if (sampleDepth >= 0.99999) continue;
            float3 delta = ReconstructWorld(sampleUV, sampleDepth) - center;
            float distanceToSample = length(delta);
            if (distanceToSample < 1e-4 || distanceToSample > radius * 2.0)
                continue;
            float cosine = dot(normal, delta / distanceToSample);
            float distanceFalloff =
                saturate(1.0 - distanceToSample / max(radius, 1e-3));
            horizon = max(horizon,
                saturate(cosine - aoParams.z) * distanceFalloff);
        }
        occlusion += horizon;
    }
    occlusion *= aoParams.y / 8.0;
    return saturate(1.0 - occlusion);
}

static const float kViewModelMaxDepth = 1.25;

bool IsViewModelDepth(float deviceDepth)
{
    return deviceDepth < 0.99999 &&
           LinearDepth(deviceDepth) < kViewModelMaxDepth;
}

float SampleContactCasterDepth(float2 uv, bool multisampled)
{
    float currentDepth = SampleDepth(uv, multisampled);
    // Do not let first-person gun depth cast into the world.
    if (IsViewModelDepth(currentDepth))
        currentDepth = 1.0;
    if (filterParams.x <= 0.5)
        return currentDepth;

    float casterDepth =
        staticCasterDepth.SampleLevel(pointClamp, saturate(uv), 0.0);
    // Static depth was captured before mesh terrain. Combining it with current
    // depth preserves the view-model exclusion while letting terrain occlude
    // objects hidden behind a hill instead of projecting them through it.
    return min(casterDepth, currentDepth);
}

// surfaceNormal is supplied by the caller, which has already reconstructed it
// for the bilateral filter. Recomputing it here repeated that work for an
// identical result.
float ContactVisibility(float2 uv, float deviceDepth, bool multisampled,
                        float3 surfaceNormal)
{
    if (deviceDepth >= 0.99999 || lightDirection.w <= 0.0 ||
        IsViewModelDepth(deviceDepth)) return 1.0;
    float3 world = ReconstructWorld(uv, deviceDepth);
    const float originOffset = max(0.015, aoParams.x * 0.03);
    world += surfaceNormal * originOffset;
    float3 rayDirection = normalize(lightDirection.xyz);
    const bool linearThickness = filterParams.z > 0.5;
    // A contact effect must stay local. Basing its range on the camera far
    // plane made the default 800 m projection cast hard foliage silhouettes
    // over 3.2 m of terrain. The linear path shares the world-space AO radius,
    // capped at one metre; leave the legacy device-depth path byte-identical.
    float maxDistance = linearThickness
        ? clamp(aoParams.x, 0.25, 1.0)
        : max(aoParams.w * 0.004, 1.5);
    float linearRayDepthStride = 0.0;
    float previousLinearDepthGap = 0.0;
    float previousLinearT = 0.0;
    if (linearThickness) {
        float4 projectedOrigin =
            mul(float4(world, 1.0), viewProjection);
        float4 projectedEnd = mul(
            float4(world + rayDirection * maxDistance, 1.0),
            viewProjection);
        if (projectedOrigin.w > 0.0 && projectedEnd.w > 0.0) {
            float2 originNDC = projectedOrigin.xy / projectedOrigin.w;
            float2 endNDC = projectedEnd.xy / projectedEnd.w;
            float2 pixelDelta = (endNDC - originNDC) *
                screenParams.xy * 0.5;
            // Ten fixed world-space taps can be tens of pixels apart on a
            // nearby receiver but sub-pixel farther away. Limit the projected
            // footprint so adjacent taps stay within two pixels without adding
            // samples; distant rays still retain the full world-space radius.
            const float maxRayPixels = 20.0;
            float projectedPixels = length(pixelDelta);
            float screenScale = min(
                1.0, maxRayPixels / max(projectedPixels, 1e-4));
            maxDistance = max(
                maxDistance * screenScale, originOffset * 4.0);
        }
        // projected.w is view-space Z. A real crossing cannot advance farther
        // in view depth between two taps than the ray itself does; using the
        // whole ray length admitted unrelated foreground surfaces as casters.
        linearRayDepthStride = abs(mul(float4(
            rayDirection * (maxDistance / 10.0), 0.0),
            viewProjection).w);
        previousLinearDepthGap =
            projectedOrigin.w - LinearDepth(deviceDepth);
    }
#if SGE_AO_OPTIMIZED
    // Animate only when temporal accumulation is active (the CPU leaves
    // the frame value at zero otherwise). The stable per-pixel component
    // breaks coherent marching bands; the frame component lets TAA
    // converge those samples instead of preserving a fixed stipple.
    // Loop-invariant: every operand is fixed for the pixel, so this produced
    // an identical value on all ten iterations. Hoisted out of the march.
    float rayJitter = InterleavedNoise(
        uv * screenParams.xy + contactParams.z * float2(19.0, 47.0));
#endif
    [unroll]
    for (uint step = 1; step <= 10; ++step) {
#if !SGE_AO_OPTIMIZED
        // Animate only when temporal accumulation is active (the CPU leaves
        // the frame value at zero otherwise). The stable per-pixel component
        // breaks coherent marching bands; the frame component lets TAA
        // converge those samples instead of preserving a fixed stipple.
        float rayJitter = InterleavedNoise(
            uv * screenParams.xy + contactParams.z * float2(19.0, 47.0));
#endif
        float t = maxDistance * ((step - 0.45 + rayJitter * 0.9) / 10.0);
        float4 projected =
            mul(float4(world + rayDirection * t, 1.0), viewProjection);
        if (projected.w <= 0.0) continue;
        float3 ndc = projected.xyz / projected.w;
        float2 rayUV = ndc.xy * float2(0.5, -0.5) + 0.5;
        if (any(rayUV <= 0.0) || any(rayUV >= 1.0)) break;
        float sampledDepth =
            SampleContactCasterDepth(rayUV, multisampled);
        if (IsViewModelDepth(sampledDepth)) continue;
        bool occluded;
        float shadowT = t;
        if (linearThickness) {
            // Compare in metres, not device depth. On this 0.1/800 projection
            // the legacy fixed epsilon represents ~4 mm at 1 m but ~29 m at
            // 100 m, so its bias and effective hit range change across one flat
            // receiver and end on contours unrelated to caster geometry.
            //
            // projected.w IS view-space Z for a standard LH perspective, so the
            // ray side of the comparison costs no reciprocal.
            float rayLinear = projected.w;
            float occluderLinear = LinearDepth(sampledDepth);
            float depthGap = rayLinear - occluderLinear;
            const float surfaceBias = originOffset * 0.25;
            // This is a finite slab BEHIND the sampled surface: the ray must be
            // past the front face, but still close enough to have crossed it in
            // the current step. The old test used this interval backwards -- it
            // rejected nearby crossings and accepted gaps up to maxDistance,
            // turning unrelated foreground depth into the dotted shadow bands.
            float slabThickness = max(
                originOffset * 2.0, linearRayDepthStride * 1.25);
            bool crossedSurface = previousLinearDepthGap <= surfaceBias &&
                                  depthGap > surfaceBias;
            occluded = crossedSurface && depthGap < slabThickness;
            if (occluded) {
                // Refine the hit continuously between the two samples. A
                // binary result at t would quantise the falloff into ten
                // parallel bands; static per-pixel dither only converted those
                // bands into dots because it never changed between frames.
                float gapRange = max(
                    depthGap - previousLinearDepthGap, 1e-5);
                float crossing = saturate(
                    (surfaceBias - previousLinearDepthGap) / gapRange);
                shadowT = lerp(previousLinearT, t, crossing);
            }
            previousLinearDepthGap = depthGap;
            previousLinearT = t;
        } else {
            float thickness = 0.00035 + t * 0.000035;
            occluded = sampledDepth + thickness < ndc.z;
        }
        if (occluded) {
            float distanceFade = saturate(1.0 - shadowT / maxDistance);
            if (linearThickness) {
                // This post effect multiplies the lit frame rather than only
                // the direct-sun term. Suppress it on surfaces receiving little
                // or no sun, and square the range fade so distant intersections
                // cannot read as detached opaque copies of thin foliage.
                float receiverLight = saturate(
                    dot(surfaceNormal, rayDirection));
                distanceFade *= distanceFade *
                    smoothstep(0.02, 0.20, receiverLight);
            }
            return 1.0 - lightDirection.w * distanceFade;
        }
    }
    return 1.0;
}

float BilateralAO(float2 uv, float centerDepth, float3 centerNormal,
                  bool multisampled)
{
    float sum = 0.0;
    float weightSum = 0.0;
    [unroll]
    for (int y = -2; y <= 2; ++y) {
        [unroll]
        for (int x = -2; x <= 2; ++x) {
            float2 offset = float2(x, y);
            // Step the filter across TRACE texels. At half res a
            // screenParams-spaced 5x5 would span only 2.5 AO samples and blur
            // the signal into mush; trace spacing keeps one tap per AO sample.
            // Depth and normal are still fetched at full resolution, so the
            // bilateral edge test stays as sharp as the composite target.
            float2 sampleUV = uv + offset * traceParams.zw;
            float sampleDeviceDepth = SampleDepth(sampleUV, multisampled);
            float sampleLinearDepth = LinearDepth(sampleDeviceDepth);
            float3 sampleNormal =
                ReconstructNormal(sampleUV, sampleDeviceDepth, multisampled);
            float spatial = exp(-dot(offset, offset) * 0.32);
            float depthWeight = exp(-abs(sampleLinearDepth - centerDepth) *
                                    18.0 / max(centerDepth, 0.25));
            float normalWeight =
                pow(saturate(dot(centerNormal, sampleNormal)), 8.0);
            float weight = spatial * depthWeight * normalWeight;
            // Linear sampler: this is the half-res -> full-res upsample.
            sum += rawAO.SampleLevel(linearClamp, sampleUV, 0.0) * weight;
            weightSum += weight;
        }
    }
    return sum / max(weightSum, 1e-4);
}

float4 BilateralSignal(float2 uv, float centerDepth, float3 centerNormal,
                       bool multisampled)
{
    float visibilitySum = 0.0;
    float3 bentSum = 0.0;
    float weightSum = 0.0;
#if SGE_AO_OPTIMIZED
    // Loop-invariant: one divide instead of twenty-five.
    float depthWeightScale = 18.0 / max(centerDepth, 0.25);
#endif
    [unroll]
    for (int y = -2; y <= 2; ++y) {
        [unroll]
        for (int x = -2; x <= 2; ++x) {
            float2 offset = float2(x, y);
            float2 sampleUV = uv + offset * screenParams.zw;
            float sampleDeviceDepth = SampleDepth(sampleUV, multisampled);
            float sampleLinearDepth = LinearDepth(sampleDeviceDepth);
            float3 sampleNormal =
                ReconstructNormal(sampleUV, sampleDeviceDepth, multisampled);
            float spatial = exp(-dot(offset, offset) * 0.32);
#if SGE_AO_OPTIMIZED
            float depthWeight = exp(-abs(sampleLinearDepth - centerDepth) *
                                    depthWeightScale);
            // x^8 and x^2 as multiply chains. pow() lowers to exp2(log2(x)*n)
            // on a transcendental unit; squaring is exact here and the operand
            // is already saturated, so the result is bit-identical.
            float normalDot = saturate(dot(centerNormal, sampleNormal));
            float normalDot2 = normalDot * normalDot;
            float normalDot4 = normalDot2 * normalDot2;
            float normalWeight = normalDot4 * normalDot4;
#else
            float depthWeight = exp(-abs(sampleLinearDepth - centerDepth) *
                                    18.0 / max(centerDepth, 0.25));
            float normalWeight =
                pow(saturate(dot(centerNormal, sampleNormal)), 8.0);
#endif
            float4 sampleSignal =
                bentAO.SampleLevel(linearClamp, sampleUV, 0.0);
            float3 sampleBent = DecodeBentNormal(sampleSignal.xy);
#if SGE_AO_OPTIMIZED
            float bentDot = saturate(dot(centerNormal, sampleBent));
            float bentWeight = bentDot * bentDot;
#else
            float bentWeight = pow(
                saturate(dot(centerNormal, sampleBent)), 2.0);
#endif
            float weight = spatial * depthWeight * normalWeight * bentWeight;
            visibilitySum += sampleSignal.z * weight;
            bentSum += sampleBent * weight;
            weightSum += weight;
        }
    }
    float inverseWeight = rcp(max(weightSum, 1e-4));
    float3 bentNormal = weightSum > 1e-4
        ? normalize(bentSum * inverseWeight) : centerNormal;
    return float4(EncodeBentNormal(bentNormal),
                  visibilitySum * inverseWeight, centerDepth);
}

// input.uv is TRACE space (this target may be half res). The centre depth is a
// source-texture read, so it is re-centred; HorizonVisibility keeps the trace
// uv because it steps its taps in trace space.
float4 PSGTAO(VSOutput input) : SV_TARGET
{
    float depth = SampleDepth(TraceUVToSourceUV(input.uv), false);
    return HorizonVisibility(input.uv, depth, false).xxxx;
}

float4 PSGTAOMSAA(VSOutput input) : SV_TARGET
{
    float depth = SampleDepth(TraceUVToSourceUV(input.uv), true);
    return HorizonVisibility(input.uv, depth, true).xxxx;
}

float4 PSBentGTAO(VSOutput input) : SV_TARGET
{
    float depth = SampleDepth(input.uv, false);
    return HorizonSignal(input.uv, depth, false);
}

float4 PSBentGTAOMSAA(VSOutput input) : SV_TARGET
{
    float depth = SampleDepth(input.uv, true);
    return HorizonSignal(input.uv, depth, true);
}

float4 TemporalSignal(float2 uv, float4 currentSignal)
{
    float temporalSetting = filterParams.w;
    bool temporalEnabled = abs(temporalSetting) > 0.001;
    bool historyValid = temporalSetting > 0.0;
    if (!temporalEnabled || !historyValid || currentSignal.w >= aoParams.w)
        return currentSignal;

    int2 pixel = clamp(int2(uv * screenParams.xy), int2(0, 0),
                       int2(screenParams.xy) - 1);
    float2 motion = motionVectors.Load(int3(pixel, 0));
    float2 previousUV = uv - motion;
    if (any(previousUV <= 0.0) || any(previousUV >= 1.0))
        return currentSignal;

    float4 previousSignal =
        aoHistory.SampleLevel(linearClamp, previousUV, 0.0);
    float depthTolerance = max(0.035, currentSignal.w * 0.012);
    float depthConfidence = saturate(
        1.0 - abs(previousSignal.w - currentSignal.w) / depthTolerance);
    float3 currentBent = DecodeBentNormal(currentSignal.xy);
    float3 previousBent = DecodeBentNormal(previousSignal.xy);
    float normalConfidence = smoothstep(
        0.72, 0.96, dot(currentBent, previousBent));
    float motionPixels = length(motion * screenParams.xy);
    float motionConfidence = saturate(1.0 - motionPixels * 0.08);

    // Clip reprojected visibility to the current 3x3 signal. This contains
    // disocclusion errors without washing out valid accumulated crevice AO.
    float minimumVisibility = currentSignal.z;
    float maximumVisibility = currentSignal.z;
    [unroll]
    for (int y = -1; y <= 1; ++y) {
        [unroll]
        for (int x = -1; x <= 1; ++x) {
            float2 sampleUV = uv + float2(x, y) * screenParams.zw;
            float visibility =
                bentAO.SampleLevel(pointClamp, sampleUV, 0.0).z;
            minimumVisibility = min(minimumVisibility, visibility);
            maximumVisibility = max(maximumVisibility, visibility);
        }
    }
    previousSignal.z = clamp(
        previousSignal.z, minimumVisibility, maximumVisibility);

    float feedback = abs(temporalSetting) * depthConfidence *
                     normalConfidence * motionConfidence;
    float3 accumulatedBent = normalize(
        lerp(currentBent, previousBent, feedback));
    return float4(EncodeBentNormal(accumulatedBent),
                  lerp(currentSignal.z, previousSignal.z, feedback),
                  currentSignal.w);
}

struct CompositeOutput
{
    float4 multiplier : SV_TARGET0;
    float4 history : SV_TARGET1;
};

float ApplyCoverageAndContact(float2 uv, float depth, bool multisampled,
                              float3 normal, float visibility)
{
    float coverage = contactParams.x > 0.5
        ? grassCoverage.SampleLevel(pointClamp, uv, 0.0) : 0.0;
    float forwardSurface = 0.0;
    float grassSurface = 0.0;
    if (filterParams.x > 0.5) {
        float staticDepth =
            staticCasterDepth.SampleLevel(pointClamp, uv, 0.0);
        float tolerance = max(0.025, LinearDepth(depth) * 0.003);
        float depthChanged = step(
            tolerance, abs(LinearDepth(depth) - LinearDepth(staticDepth)));
        grassSurface = depthChanged * step(0.001, coverage);
        forwardSurface = depthChanged * (1.0 - step(0.001, coverage));
    }
    if (contactParams.w > 0.5) {
        // Visibility-buffer surfaces already consumed the accumulated GTAO in
        // their ambient diffuse term. Retain the post multiplier only for
        // later forward draws and for the covered fraction of MSAA grass.
        float postAOWeight = saturate(
            forwardSurface + grassSurface * coverage);
        visibility = lerp(1.0, visibility, postAOWeight);
    } else {
        // A nearest covered MSAA sample represents only its occupied fraction.
        // Weight the grass AO contribution instead of darkening the background
        // as though a single thin blade covered the entire pixel.
        visibility = lerp(visibility,
            lerp(1.0, visibility, coverage), grassSurface);
    }

    float contactWeight = 1.0;
    if (contactParams.y > 0.5) {
        // Visibility-buffer surfaces received contact visibility inside their
        // direct-sun term. Retain this post fallback only for later forward
        // extensions and for the covered fraction of independently resolved
        // grass, avoiding a second darkening of opaque/foliage lighting.
        contactWeight = saturate(forwardSurface + grassSurface * coverage);
    }
    // Avoid paying for a second ten-step ray on visibility-resolved pixels;
    // contactWeight is zero there because direct lighting already owns it.
    if (contactWeight > 0.001) {
        float contact = ContactVisibility(uv, depth, multisampled, normal);
        visibility *= lerp(1.0, contact, contactWeight);
    }
    return visibility;
}

float4 Composite(float2 uv, bool multisampled)
{
    float depth = SampleDepth(uv, multisampled);
    if (depth >= 0.99999) return 1.0;
    float3 normal = ReconstructNormal(uv, depth, multisampled);
    float visibility = BilateralAO(
        uv, LinearDepth(depth), normal, multisampled);
    float coverage = contactParams.x > 0.5
        ? grassCoverage.SampleLevel(pointClamp, uv, 0.0) : 0.0;
    float forwardSurface = 0.0;
    float grassSurface = 0.0;
    if (filterParams.x > 0.5) {
        float staticDepth =
            staticCasterDepth.SampleLevel(pointClamp, uv, 0.0);
        float tolerance = max(0.025, LinearDepth(depth) * 0.003);
        float depthChanged = step(
            tolerance, abs(LinearDepth(depth) - LinearDepth(staticDepth)));
        grassSurface = depthChanged * step(0.001, coverage);
        forwardSurface = depthChanged * (1.0 - step(0.001, coverage));
    }
    // A nearest covered MSAA sample represents only its occupied fraction.
    // Weight the grass AO contribution instead of darkening the background as
    // though a single thin blade covered the entire pixel.
    visibility = lerp(visibility,
        lerp(1.0, visibility, coverage), grassSurface);

    float contactWeight = 1.0;
    if (contactParams.y > 0.5) {
        // Visibility-buffer surfaces received contact visibility inside their
        // direct-sun term. Retain this post fallback only for later forward
        // extensions and for the covered fraction of independently resolved
        // grass, avoiding a second darkening of opaque/foliage lighting.
        contactWeight = saturate(forwardSurface + grassSurface * coverage);
    }
    // Avoid paying for a second ten-step ray on visibility-resolved pixels;
    // contactWeight is zero there because direct lighting already owns it.
    if (contactWeight > 0.001) {
        float contact = ContactVisibility(uv, depth, multisampled, normal);
        visibility *= lerp(1.0, contact, contactWeight);
    }
    return float4(visibility.xxx, 1.0);
}

CompositeOutput CompositeTemporal(float2 uv, bool multisampled)
{
    CompositeOutput output;
    float depth = SampleDepth(uv, multisampled);
    if (depth >= 0.99999) {
        output.multiplier = 1.0;
        output.history = float4(0.5, 0.5, 1.0, aoParams.w);
        return output;
    }
    float3 normal = ReconstructNormal(uv, depth, multisampled);
    float4 signal = BilateralSignal(
        uv, LinearDepth(depth), normal, multisampled);
    signal = TemporalSignal(uv, signal);
    float visibility = ApplyCoverageAndContact(
        uv, depth, multisampled, normal, signal.z);
    output.multiplier = float4(visibility.xxx, 1.0);
    output.history = signal;
    return output;
}

float4 PSBlurComposite(VSOutput input) : SV_TARGET
{
    return Composite(input.uv, false);
}

float4 PSBlurCompositeMSAA(VSOutput input) : SV_TARGET
{
    return Composite(input.uv, true);
}

CompositeOutput PSBentBlurComposite(VSOutput input)
{
    return CompositeTemporal(input.uv, false);
}

CompositeOutput PSBentBlurCompositeMSAA(VSOutput input)
{
    return CompositeTemporal(input.uv, true);
}
