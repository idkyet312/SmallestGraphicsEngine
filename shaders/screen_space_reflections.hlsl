cbuffer SSRConstants : register(b0)
{
    float4x4 inverseViewProjection;
    float4x4 viewProjection;
    float4 cameraNear;
    float4 screenParams;
    float4 ssrParams; // max distance, thickness, strength, surface buffer
};

Texture2D<float4> sceneColor : register(t0);
Texture2D<float> sceneDepth : register(t1);
Texture2D<float4> surfaceData : register(t2);
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
