cbuffer SSRConstants : register(b0)
{
    float4x4 inverseViewProjection;
    float4x4 viewProjection;
    float4 cameraNear;
    float4 screenParams;
    float4 ssrParams; // max distance, thickness, strength, surface buffer
    // Screen-space ray tracing (NGLighting). x: enabled, y: GI enabled,
    // z: GI strength, w: ray steps.
    float4 ngParams;
    // x: step growth per iteration, y: AO strength, z: AO radius,
    // w: frame index, used to advance the noise sequence.
    float4 ngTrace;
    // x: history blend weight, y: history valid, z: specular enabled,
    // w: AO enabled.
    float4 ngTemporal;
};

Texture2D<float4> sceneColor : register(t0);
Texture2D<float> sceneDepth : register(t1);
Texture2D<float4> surfaceData : register(t2);
Texture2D<float4> ngHistory : register(t3);
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

float3 ReconstructWorld(float2 uv, float depth)
{
    float2 ndc = float2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
    float4 world = mul(float4(ndc, depth, 1.0), inverseViewProjection);
    return world.xyz / max(abs(world.w), 1e-5);
}

float3 ReconstructNormal(float2 uv, float depth)
{
    float3 center = ReconstructWorld(uv, depth);
    float2 texel = screenParams.zw;
    float3 right = ReconstructWorld(
        uv + float2(texel.x, 0.0),
        sceneDepth.SampleLevel(pointClamp, uv + float2(texel.x, 0.0), 0.0));
    float3 down = ReconstructWorld(
        uv + float2(0.0, texel.y),
        sceneDepth.SampleLevel(pointClamp, uv + float2(0.0, texel.y), 0.0));
    float3 normal = normalize(cross(right - center, down - center));
    float3 view = normalize(cameraNear.xyz - center);
    return dot(normal, view) < 0.0 ? -normal : normal;
}

float Hash(float2 p)
{
    return frac(52.9829189 * frac(dot(p, float2(0.06711056, 0.00583715))));
}

bool TraceReflection(float3 origin, float3 direction, float roughness,
                     float2 pixel, out float2 hitUV, out float confidence)
{
    float maxDistance = ssrParams.x;
    float stride = maxDistance / 48.0;
    float jitter = Hash(pixel);
    float previousDelta = -ssrParams.y;
    float previousT = ssrParams.y;
    hitUV = 0.0;
    confidence = 0.0;

    [loop]
    for (uint rayStep = 1; rayStep <= 48; ++rayStep) {
        float t = stride * (rayStep - 0.7 + jitter);
        float3 rayPosition = origin + direction * t;
        float4 clip = mul(float4(rayPosition, 1.0), viewProjection);
        if (clip.w <= 0.0) break;
        float3 ndc = clip.xyz / clip.w;
        float2 uv = ndc.xy * float2(0.5, -0.5) + 0.5;
        if (any(uv <= 0.001) || any(uv >= 0.999)) break;
        float deviceDepth = sceneDepth.SampleLevel(pointClamp, uv, 0.0);
        if (deviceDepth >= 0.99999) {
            previousT = t;
            continue;
        }
        float3 scenePosition = ReconstructWorld(uv, deviceDepth);
        float rayDistance = length(rayPosition - cameraNear.xyz);
        float sceneDistance = length(scenePosition - cameraNear.xyz);
        float delta = rayDistance - sceneDistance;
        float thickness = ssrParams.y * (1.0 + t * 0.025 + roughness);
        // Require a real front-to-back depth crossing. Accepting points merely
        // close to the depth surface made forward-only walls self-reflect.
        if (delta >= 0.0 && previousDelta < 0.0) {
            float lo = previousT;
            float hi = t;
            [unroll]
            for (uint refine = 0; refine < 5; ++refine) {
                float mid = (lo + hi) * 0.5;
                float3 midPosition = origin + direction * mid;
                float4 midClip = mul(float4(midPosition, 1.0), viewProjection);
                float2 midUV = (midClip.xy / midClip.w) *
                               float2(0.5, -0.5) + 0.5;
                float midDepth =
                    sceneDepth.SampleLevel(pointClamp, midUV, 0.0);
                float3 midScene = ReconstructWorld(midUV, midDepth);
                if (length(midPosition - cameraNear.xyz) >=
                    length(midScene - cameraNear.xyz))
                    hi = mid;
                else
                    lo = mid;
            }
            float3 finalPosition = origin + direction * hi;
            float4 finalClip =
                mul(float4(finalPosition, 1.0), viewProjection);
            hitUV = (finalClip.xy / finalClip.w) *
                    float2(0.5, -0.5) + 0.5;
            float hitDepth =
                sceneDepth.SampleLevel(pointClamp, hitUV, 0.0);
            float3 hitPosition = ReconstructWorld(hitUV, hitDepth);
            float finalDelta =
                abs(length(finalPosition - cameraNear.xyz) -
                    length(hitPosition - cameraNear.xyz));
            float edge = min(min(hitUV.x, hitUV.y),
                             min(1.0 - hitUV.x, 1.0 - hitUV.y));
            float pixelTravel =
                length(hitUV * screenParams.xy - pixel);
            float hitValidity = 1.0;
            if (ssrParams.w > 0.5) {
                float3 hitNormal =
                    surfaceData.SampleLevel(pointClamp, hitUV, 0.0).xyz;
                float hitNormalLength = dot(hitNormal, hitNormal);
                hitValidity = step(0.25, hitNormalLength);
                hitNormal *= rsqrt(max(hitNormalLength, 1e-5));
                // Reject backfaces and depth from forward-only geometry that
                // has no matching visibility-buffer surface.
                hitValidity *= smoothstep(
                    0.04, 0.30, dot(hitNormal, -direction));
            }
            confidence = smoothstep(0.0, 0.08, edge) *
                         smoothstep(3.0, 18.0, pixelTravel) *
                         saturate(1.0 - t / maxDistance) *
                         (1.0 - smoothstep(
                             thickness, thickness * 3.0, finalDelta)) *
                         hitValidity;
            if (confidence > 0.001) return true;

            // A rejected hit is still opaque. Forward extensions such as mesh
            // terrain do not write surfaceData, so they cannot provide a valid
            // reflection sample, but the ray must stop at their depth. Continuing
            // here let SSR travel through hills and pick up grass/sky behind them.
            return false;
        }
        previousDelta = delta;
        previousT = t;
    }
    return false;
}

// ---- Screen-space ray tracing (NGLighting) ---------------------------------
//
// Ported from the CC0-licensed NiceGuy-Shaders ReShade pack. Two ideas carry
// over; the rest is this engine's own reconstruction and hit validation.
//
// 1. Geometric step growth. Each march step is longer than the last, so a ray
//    reaches across the scene in ~24 steps instead of the 48 uniform ones the
//    mirror path uses. Precision degrades with distance, which is acceptable
//    because distant indirect light is low-frequency anyway.
// 2. Interleaved gradient noise for ray direction. It decorrelates neighbouring
//    pixels well enough that the temporal pass can resolve the result, which is
//    what makes a stochastic hemisphere gather viable in real time.

// Interleaved gradient noise, animated over the frame index so the temporal
// pass sees a new sequence each frame rather than re-averaging one pattern.
float IGN(float2 pixel, float frame)
{
    float3 magic = float3(0.06711056, 0.00583715, 52.9829189);
    return frac(magic.z * frac(dot(pixel + frame * 5.588238, magic.xy)));
}

// Cosine-weighted hemisphere direction around `normal`. Cosine weighting means
// the sample density already carries the N.L term, so the gather does not have
// to multiply it back in.
float3 CosineHemisphere(float3 normal, float2 uniformPair)
{
    float phi = 6.28318530718 * uniformPair.x;
    float cosTheta = sqrt(max(0.0, 1.0 - uniformPair.y));
    float sinTheta = sqrt(uniformPair.y);
    float3 tangent =
        normalize(cross(abs(normal.y) < 0.95 ? float3(0, 1, 0)
                                             : float3(1, 0, 0), normal));
    float3 bitangent = cross(normal, tangent);
    return normalize(tangent * (cos(phi) * sinTheta) +
                     bitangent * (sin(phi) * sinTheta) +
                     normal * cosTheta);
}

// March one ray. Returns true on a hit, with the hit UV and the distance
// travelled -- the distance is what drives ambient occlusion.
bool TraceNGRay(float3 origin, float3 direction, float noise,
                out float2 hitUV, out float hitDistance)
{
    hitUV = 0.0;
    hitDistance = ssrParams.x;

    // March steps are fixed; the ray budget in ngParams.w counts rays per
    // pixel, which is a separate control.
    const int maxSteps = 24;
    const float growth = max(ngTrace.x, 1.0);
    // Start at a fraction of the thickness so the first sample cannot land back
    // on the originating surface, jittered so the step pattern does not band.
    float stepLength = max(ssrParams.y, 0.02) * (1.0 + noise);
    float travelled = 0.0;

    [loop]
    for (int i = 0; i < maxSteps; ++i) {
        travelled += stepLength;
        if (travelled > ssrParams.x) break;
        float3 rayPosition = origin + direction * travelled;
        float4 clip = mul(float4(rayPosition, 1.0), viewProjection);
        if (clip.w <= 0.0) break;
        float2 uv = (clip.xy / clip.w) * float2(0.5, -0.5) + 0.5;
        if (any(uv <= 0.001) || any(uv >= 0.999)) break;

        float deviceDepth = sceneDepth.SampleLevel(pointClamp, uv, 0.0);
        // Sky writes no geometry, so it can never occlude a ray.
        if (deviceDepth < 0.99999) {
            float3 scenePosition = ReconstructWorld(uv, deviceDepth);
            float rayDistance = length(rayPosition - cameraNear.xyz);
            float sceneDistance = length(scenePosition - cameraNear.xyz);
            float delta = rayDistance - sceneDistance;
            // Accept only a crossing that is within the surface's assumed
            // thickness. Without the upper bound a ray passing far behind an
            // object would register as touching it, which fills concave corners
            // with light from whatever happens to be behind them.
            float thickness = max(ssrParams.y, 0.02) * 4.0 + stepLength;
            if (delta > 0.0 && delta < thickness) {
                hitUV = uv;
                hitDistance = travelled;
                return true;
            }
        }
        stepLength *= growth;
    }
    return false;
}

// Gather indirect light and occlusion over the hemisphere. Both come out of the
// same rays: colour from what each ray hit, occlusion from how far it got.
void GatherNGLighting(float2 uv, float3 world, float3 normal, float roughness,
                      float3 toCamera, float2 pixel,
                      out float3 indirect, out float occlusion)
{
    indirect = 0.0;
    occlusion = 0.0;

    const float frame = ngTrace.w;
    const bool wantSpecular = ngTemporal.z > 0.5;
    const bool wantGI = ngParams.y > 0.5;
    const bool wantAO = ngTemporal.w > 0.5;
    const float aoRadius = max(ngTrace.z, 0.01);
    const float3 origin = world + normal * max(ssrParams.y, 0.02) * 2.0;

    // The specular ray follows a direction the surface itself determines, so it
    // resolves cleanly from a single sample. The diffuse rays are random
    // hemisphere directions, so their result is noisy until many are averaged.
    // Keeping the two separate matters for more than cost: summing them into
    // one accumulator and dividing by a shared weight let the diffuse rays
    // dilute the reflection, so enabling GI visibly dimmed every mirror.
    if (wantSpecular) {
        float n0 = IGN(pixel, frame);
        float n1 = IGN(pixel + 37.0, frame + 13.0);
        float3 mirror = reflect(-toCamera, normal);
        // Jitter by roughness, which turns a sharp reflection into a glossy one
        // without a separate blur pass.
        float3 scatter = CosineHemisphere(normal, float2(n0, n1));
        float3 direction = normalize(lerp(mirror, scatter,
                                          saturate(roughness * roughness)));
        if (dot(direction, normal) <= 0.0) direction = mirror;

        float2 rayHitUV;
        float rayDistance;
        if (TraceNGRay(origin, direction, n0, rayHitUV, rayDistance)) {
            float3 sampleColor =
                min(sceneColor.SampleLevel(linearClamp, rayHitUV, 0.0).rgb, 6.0);
            float edge = min(min(rayHitUV.x, rayHitUV.y),
                             min(1.0 - rayHitUV.x, 1.0 - rayHitUV.y));
            indirect += sampleColor * smoothstep(0.0, 0.06, edge);
            if (wantAO)
                occlusion += saturate(1.0 - rayDistance / aoRadius);
        }
    }

    // Diffuse hemisphere gather. Skipped entirely when neither GI nor AO wants
    // it, so the rays are not traced at all rather than traced and discarded --
    // this is the expensive half of the pass.
    if (!wantGI && !wantAO) {
        occlusion = saturate(occlusion) * ngTrace.y;
        return;
    }

    const int rayCount = clamp((int)ngParams.w, 1, 64);
    float3 diffuseSum = 0.0;
    float diffuseWeight = 0.0;
    float occlusionSum = 0.0;

    [loop]
    for (int i = 0; i < rayCount; ++i) {
        float n0 = IGN(pixel, frame + (i + 1) * 7.0);
        float n1 = IGN(pixel + 37.0, frame + (i + 1) * 13.0);
        float3 direction = CosineHemisphere(normal, float2(n0, n1));

        float2 rayHitUV;
        float rayDistance;
        if (!TraceNGRay(origin, direction, n0, rayHitUV, rayDistance)) {
            // A ray that escapes hit nothing, so it is unoccluded. It still
            // counts toward the average -- dropping misses makes open sky read
            // as fully occluded and turns the whole image grey.
            continue;
        }

        // Occlusion falls off with distance: a hit metres away barely shadows
        // the pixel, one a few centimetres away shadows it heavily.
        if (wantAO)
            occlusionSum += saturate(1.0 - rayDistance / aoRadius);

        if (!wantGI) continue;
        float3 sampleColor =
            min(sceneColor.SampleLevel(linearClamp, rayHitUV, 0.0).rgb, 6.0);
        // Fade contributions toward the screen edge, where there is no data to
        // trace into and the result would otherwise pop as the camera turns.
        float edge = min(min(rayHitUV.x, rayHitUV.y),
                         min(1.0 - rayHitUV.x, 1.0 - rayHitUV.y));
        float weight = smoothstep(0.0, 0.06, edge);
        diffuseSum += sampleColor * weight;
        diffuseWeight += weight;
    }

    if (wantGI && diffuseWeight > 0.0)
        indirect += (diffuseSum / diffuseWeight) * ngParams.z;
    // Misses are unoccluded, so the divisor is the full ray count, not the
    // number that hit.
    occlusion = saturate(occlusion + occlusionSum /
                         max((float)rayCount, 1.0)) * ngTrace.y;
}

float4 PSMain(VSOutput input) : SV_TARGET
{
    float depth = sceneDepth.SampleLevel(pointClamp, input.uv, 0.0);
    if (depth >= 0.99999) return 0.0;
    float3 world = ReconstructWorld(input.uv, depth);
    float3 reconstructed = ReconstructNormal(input.uv, depth);
    float4 stored = surfaceData.SampleLevel(pointClamp, input.uv, 0.0);
    float storedValid = ssrParams.w * step(0.25, dot(stored.xyz, stored.xyz));
    float storedAgreement =
        step(0.60, dot(normalize(stored.xyz + 1e-5), reconstructed));
    storedValid *= storedAgreement;
    // In visibility mode, missing/mismatched surface data means this pixel was
    // drawn later by a forward extension. Its roughness is unknown, so IBL is
    // safer than inventing a glossy SSR surface.
    if (ssrParams.w > 0.5 && storedValid < 0.5) return 0.0;
    float3 normal = normalize(lerp(reconstructed, stored.xyz, storedValid));
    float roughness = lerp(0.22, stored.w, storedValid);

    // SSR is a sharp-reflection solution. Rough materials retain their stable
    // IBL response instead of showing stretched screen-color ghosts.
    float smoothWeight = 1.0 - smoothstep(0.08, 0.50, roughness);
    if (smoothWeight <= 0.001) return 0.0;

    float3 toCamera = normalize(cameraNear.xyz - world);
    float3 reflection = normalize(reflect(-toCamera, normal));
    if (dot(reflection, normal) <= 0.001) return 0.0;
    float thickness = max(ssrParams.y, 0.015);
    float3 origin = world + normal * thickness * 1.5;
    float2 hitUV;
    float confidence;
    if (!TraceReflection(origin, reflection, roughness,
                         input.uv * screenParams.xy, hitUV, confidence))
        return 0.0;

    float lodRadius = roughness * roughness * 3.5;
    float2 texel = screenParams.zw * lodRadius;
    float3 reflected = sceneColor.SampleLevel(linearClamp, hitUV, 0.0).rgb;
    reflected += sceneColor.SampleLevel(
        linearClamp, hitUV + float2(texel.x, 0.0), 0.0).rgb;
    reflected += sceneColor.SampleLevel(
        linearClamp, hitUV - float2(texel.x, 0.0), 0.0).rgb;
    reflected += sceneColor.SampleLevel(
        linearClamp, hitUV + float2(0.0, texel.y), 0.0).rgb;
    reflected += sceneColor.SampleLevel(
        linearClamp, hitUV - float2(0.0, texel.y), 0.0).rgb;
    reflected *= 0.2;
    reflected = min(reflected, 8.0);

    float nDotV = saturate(dot(normal, toCamera));
    float fresnel = 0.04 + 0.96 * pow(1.0 - nDotV, 5.0);
    float weight = ssrParams.z * smoothWeight * confidence *
                   lerp(0.35, 1.0, fresnel);
    return float4(reflected * weight, 0.0);
}

// NGLighting trace. Writes the raw gather into the accumulation buffer: rgb is
// the indirect light minus occlusion, alpha is the depth it was traced against
// so the next frame can tell whether this pixel still shows the same surface.
float4 PSTraceNG(VSOutput input) : SV_TARGET
{
    float depth = sceneDepth.SampleLevel(pointClamp, input.uv, 0.0);
    if (depth >= 0.99999) return float4(0.0, 0.0, 0.0, depth);

    float3 world = ReconstructWorld(input.uv, depth);
    float3 reconstructed = ReconstructNormal(input.uv, depth);
    float4 stored = surfaceData.SampleLevel(pointClamp, input.uv, 0.0);
    float storedValid = ssrParams.w * step(0.25, dot(stored.xyz, stored.xyz));
    float3 normal = normalize(lerp(reconstructed, stored.xyz, storedValid));
    float roughness = lerp(0.22, stored.w, storedValid);
    float3 toCamera = normalize(cameraNear.xyz - world);

    float3 indirect;
    float occlusion;
    GatherNGLighting(input.uv, world, normal, roughness, toCamera,
                     input.uv * screenParams.xy, indirect, occlusion);

    // Occlusion must attenuate light already in the frame, but the composite
    // blends additively, so it is carried as a negative term against the
    // pixel's own colour rather than as a multiply.
    float3 existing = sceneColor.SampleLevel(pointClamp, input.uv, 0.0).rgb;
    float3 current = indirect * ssrParams.z - existing * occlusion;

    // Temporal accumulation. A stochastic gather this sparse is unusable raw,
    // so history carries most of the visible signal. With no motion vectors in
    // this pass, a depth change is the only available disocclusion test: it
    // catches geometry changing under a pixel, but not the camera panning
    // across a surface at constant depth, which is why the blend is capped
    // rather than being a true exponential average.
    if (ngTemporal.y > 0.5) {
        float4 history = ngHistory.SampleLevel(pointClamp, input.uv, 0.0);
        float valid = 1.0 - smoothstep(0.0002, 0.0025, abs(history.a - depth));
        current = lerp(current, history.rgb, ngTemporal.x * valid);
    }
    return float4(current, depth);
}

// NGLighting composite. No tracing: it only adds the accumulated result into
// the scene, which is why the expensive pass runs exactly once per frame.
float4 PSCompositeNG(VSOutput input) : SV_TARGET
{
    float depth = sceneDepth.SampleLevel(pointClamp, input.uv, 0.0);
    if (depth >= 0.99999) return 0.0;
    return float4(ngHistory.SampleLevel(pointClamp, input.uv, 0.0).rgb, 0.0);
}
