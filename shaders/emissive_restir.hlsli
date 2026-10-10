// Emissive triangles as light sources: ReSTIR DI [Bitterli et al. 2020], the
// algorithm NVIDIA ships as RTXDI, run inline in the enhanced resolve.
//
// Bistro's light comes from emissive geometry -- street lamp glass, shop signs
// and ~15k tiny string-light bulb triangles -- not from punctual lights. A
// diffuse GI ray almost never hits a bulb, so before this the lamps glowed but
// lit nothing. Here every pixel draws candidates straight from the emitters:
//
//   1. EMISSIVE_CANDIDATES triangles from a power-proportional alias table
//      (area x emissive factor, built on the CPU at acceleration rebuild), a
//      uniform point on each.
//   2. Resampled importance sampling with target p^ = lum(Le) * G, G =
//      cos_surface * cos_light / d^2, picks one.
//   3. Temporal reuse: last frame's reservoir at the reprojected pixel is
//      merged when it lies on the same object (stable-surface namespace), the
//      same test the Lumen ReSTIR GI path uses. The candidate count it
//      contributes is capped at EMISSIVE_MAX_M.
//   4. One visibility ray to the selected point. An occluded sample is stored
//      with W = 0 so it cannot spread through reuse.
//
// GI ray hits run a cheaper version (EMISSIVE_HIT_CANDIDATES, no reuse, one
// shadow ray), so emissive light also reaches the bounce and the radiance cache.
//
// Storage: two frames of uint4 per pixel (parity indexed, as gi_restir.hlsli):
//   x = light index, y = unorm16 (u, v) point on the triangle,
//   z = asuint(W), w = M | check16 << 16. The check rejects the undefined
//   contents of a freshly allocated buffer.

#ifndef EMISSIVE_RESTIR_HLSLI
#define EMISSIVE_RESTIR_HLSLI

// Mirrors EmissiveTriangleGPU in VisibilityBufferDX12.h (96 bytes).
struct EmissiveTriangle {
    float3 p0;
    uint   materialID;
    float3 e1;
    uint   bindlessMaterialID;
    float3 e2;
    float  area;
    float2 uv0;
    float2 uv1;
    float2 uv2;
    float  pdf;        // probability of picking this triangle
    float  aliasProb;
    uint   alias;
    uint   lightCount; // replicated in every entry; entry 0 is always valid
    float2 pad;
};
StructuredBuffer<EmissiveTriangle> emissiveTriangles : register(t100);
RWStructuredBuffer<uint4> emissiveReservoirs : register(u19);

#define EMISSIVE_CANDIDATES 8u
#define EMISSIVE_HIT_CANDIDATES 4u
#define EMISSIVE_MAX_M 20u

uint EmissivePcg(uint v) {
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float EmissiveRandom(inout uint state) {
    state = EmissivePcg(state);
    return (float)(state >> 8u) * (1.0 / 16777216.0);
}

float EmissiveLuma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

uint EmissiveLightCount() { return emissiveTriangles[0].lightCount; }

uint EmissivePickTriangle(float u0, float u1, uint count) {
    uint i = min((uint)(u0 * (float)count), count - 1u);
    return u1 < emissiveTriangles[i].aliasProb ? i : emissiveTriangles[i].alias;
}

struct EmissivePoint {
    float3 position;
    float3 normal;     // geometric, unnormalized sign irrelevant (two-sided)
    float3 radiance;   // Le, emissiveIntensity applied
    float  pdfArea;    // source pdf in area measure
};

// Uniform point on the triangle from (u, v) in [0,1)^2.
EmissivePoint EmissiveEvaluate(uint index, float u, float v) {
    EmissiveTriangle t = emissiveTriangles[index];
    float su = sqrt(u);
    float b1 = su * (1.0 - v);
    float b2 = su * v;
    EmissivePoint p;
    p.position = t.p0 + t.e1 * b1 + t.e2 * b2;
    p.normal = normalize(cross(t.e1, t.e2));
    p.pdfArea = t.pdf / max(t.area, 1e-8);
#ifdef SGE_BINDLESS_MATERIALS
    MaterialData m = materials[t.bindlessMaterialID];
#else
    MaterialData m = materials[t.materialID];
#endif
    p.radiance = m.emissiveOcclusion.rgb * emissiveIntensity;
#ifdef SGE_BINDLESS_MATERIALS
    if (m.shadingParams.w > 0.0) {
        float2 uv = t.uv0 * (1.0 - b1 - b2) + t.uv1 * b1 + t.uv2 * b2;
        Texture2D<float4> emission = MAT_TEX((uint)m.shadingParams.w - 1);
        // Same decode the GI hit path uses for this texture.
        p.radiance *= emission.SampleLevel(texSampler, uv, 0).rgb;
    }
#endif
    return p;
}

// Geometry term toward an emitter point; also returns the unit direction and
// distance. Zero below the receiver's horizon. Emitters are two-sided.
float EmissiveGeometry(EmissivePoint p, float3 position, float3 normal,
                       out float3 direction, out float distance) {
    float3 toLight = p.position - position;
    float distanceSq = max(dot(toLight, toLight), 1e-6);
    distance = sqrt(distanceSq);
    direction = toLight / distance;
    float cosSurface = dot(normal, direction);
    float cosLight = abs(dot(p.normal, direction));
    if (cosSurface <= 0.0) return 0.0;
    return cosSurface * cosLight / distanceSq;
}

bool EmissiveVisible(float3 position, float3 normal, float3 direction,
                     float distance) {
    RayDesc ray;
    ray.Origin = position + normal * 0.02;
    ray.Direction = direction;
    ray.TMin = 0.0;
    // Stop short of the emitter, whose own triangle is in the TLAS.
    ray.TMax = max(distance - 0.03, 0.0);
    RayQuery<SGE_RAY_FLAGS_SHADOW> query;
    query.TraceRayInline(sceneTLAS, RAY_FLAG_NONE, 0xff, ray);
    query.Proceed();
    return query.CommittedStatus() != COMMITTED_TRIANGLE_HIT;
}

uint EmissiveCheck(uint index, uint packedUV) {
    return EmissivePcg(index * 9781u ^ packedUV) >> 16u;
}

struct EmissiveReservoir {
    uint  index;
    float u, v;
    float wSum;
    float targetSelected;
    uint  M;
};

void EmissiveUpdate(inout EmissiveReservoir r, uint index, float u, float v,
                    float target, float weight, float xi) {
    r.wSum += weight;
    if (weight > 0.0 && xi * r.wSum < weight) {
        r.index = index;
        r.u = u;
        r.v = v;
        r.targetSelected = target;
    }
}

uint EmissiveReservoirIndex(uint2 pixel, uint parity) {
    uint pixels = (uint)screenWidth * (uint)screenHeight;
    return parity * pixels + pixel.y * (uint)screenWidth + pixel.x;
}

// Direct light from emissive triangles at a primary surface, BRDF applied.
// metal/rough follow the resolve's own GGX lobe conventions.
float3 EmissiveDirectLighting(uint2 pixel, float3 position, float3 normal,
                              float3 viewDir, float3 albedo, float metal,
                              float rough, uint surfaceNamespace,
                              bool commit) {
    uint count = EmissiveLightCount();
    if (count == 0u) return 0.0;
    uint rng = EmissivePcg(pixel.x * 1973u + pixel.y * 9277u +
                           enhancedFrameIndex * 26699u + 0x51ed27u);

    EmissiveReservoir r;
    r.index = 0xffffffffu;
    r.u = r.v = 0.0;
    r.wSum = 0.0;
    r.targetSelected = 0.0;
    r.M = EMISSIVE_CANDIDATES;
    [loop]
    for (uint c = 0u; c < EMISSIVE_CANDIDATES; ++c) {
        uint index = EmissivePickTriangle(EmissiveRandom(rng),
                                          EmissiveRandom(rng), count);
        float u = EmissiveRandom(rng), v = EmissiveRandom(rng);
        EmissivePoint p = EmissiveEvaluate(index, u, v);
        float3 direction;
        float distance;
        float target = EmissiveLuma(p.radiance) *
            EmissiveGeometry(p, position, normal, direction, distance);
        EmissiveUpdate(r, index, u, v, target,
                       target / max(p.pdfArea, 1e-12), EmissiveRandom(rng));
    }

    const uint parity = enhancedFrameIndex & 1u;
    float2 currentUV = (float2(pixel) + 0.5) / float2(screenWidth, screenHeight);
    float2 previousUV = currentUV - outputMotion[pixel];
    // Non-committing callers (scope view, debug views) may run at another
    // resolution; a root UAV has no bounds checking, so they skip history.
    if (commit && all(previousUV >= 0.0) && all(previousUV < 1.0)) {
        int2 previousPixel = clamp(
            int2(previousUV * float2(screenWidth, screenHeight)),
            int2(0, 0), int2(screenWidth - 1, screenHeight - 1));
        if (svgfStableSurfaceHistory.Load(int3(previousPixel, 0)).x ==
            surfaceNamespace) {
            uint4 stored = emissiveReservoirs[
                EmissiveReservoirIndex(uint2(previousPixel), parity ^ 1u)];
            uint storedM = stored.w & 0xffffu;
            float storedW = asfloat(stored.z);
            bool valid = stored.x < count && storedM > 0u &&
                storedM <= EMISSIVE_MAX_M &&
                (stored.w >> 16u) == EmissiveCheck(stored.x, stored.y) &&
                (stored.z & 0x7f800000u) != 0x7f800000u && storedW > 0.0;
            if (valid) {
                float u = (float)(stored.y & 0xffffu) / 65535.0;
                float v = (float)(stored.y >> 16u) / 65535.0;
                EmissivePoint p = EmissiveEvaluate(stored.x, u, v);
                float3 direction;
                float distance;
                float target = EmissiveLuma(p.radiance) *
                    EmissiveGeometry(p, position, normal, direction, distance);
                EmissiveUpdate(r, stored.x, u, v, target,
                               target * storedW * (float)storedM,
                               EmissiveRandom(rng));
                r.M += storedM;
            }
        }
    }

    // A single spatial donor, chosen by primary geometry only. Both temporal
    // and spatial inputs are from the completed previous parity; the selected
    // emitter still gets exactly one final visibility ray at this receiver.
    if (commit && emissiveCGNS == 2u) {
        int2 donor;
        if (EmissiveSelectCompatibleNeighbor(pixel, position, normal, donor) &&
            any(donor != int2(previousUV * float2(screenWidth, screenHeight)))) {
            uint4 stored = emissiveReservoirs[
                EmissiveReservoirIndex(uint2(donor), parity ^ 1u)];
            uint storedM = stored.w & 0xffffu;
            float storedW = asfloat(stored.z);
            bool valid = stored.x < count && storedM > 0u &&
                storedM <= EMISSIVE_MAX_M &&
                (stored.w >> 16u) == EmissiveCheck(stored.x, stored.y) &&
                isfinite(storedW) && storedW > 0.0;
            if (valid) {
                float u = (float)(stored.y & 0xffffu) / 65535.0;
                float v = (float)(stored.y >> 16u) / 65535.0;
                EmissivePoint p = EmissiveEvaluate(stored.x, u, v);
                float3 direction;
                float distance;
                float target = EmissiveLuma(p.radiance) *
                    EmissiveGeometry(p, position, normal, direction, distance);
                EmissiveUpdate(r, stored.x, u, v, target,
                    target * storedW * (float)storedM, EmissiveRandom(rng));
                r.M += storedM;
            }
        }
    }

    float3 lighting = 0.0;
    float W = 0.0;
    if (r.index != 0xffffffffu && r.targetSelected > 0.0) {
        W = r.wSum / ((float)r.M * r.targetSelected);
        EmissivePoint p = EmissiveEvaluate(r.index, r.u, r.v);
        float3 L;
        float distance;
        float G = EmissiveGeometry(p, position, normal, L, distance);
        if (G > 0.0 && EmissiveVisible(position, normal, L, distance)) {
            // Same lobes the sun uses in ShadeSurface: Lambert + GGX.
            float3 H = normalize(viewDir + L);
            float NdotL = saturate(dot(normal, L));
            float NdotV = max(dot(normal, viewDir), 1e-4);
            float NdotH = saturate(dot(normal, H));
            float HdotV = saturate(dot(H, viewDir));
            float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metal);
            float3 F = F0 + (1.0 - F0) * pow(1.0 - HdotV, 5.0);
            float a = rough * rough;
            float a2 = a * a;
            float d = NdotH * NdotH * (a2 - 1.0) + 1.0;
            float NDF = a2 / max(3.14159265 * d * d, 1e-6);
            float k = (rough + 1.0) * (rough + 1.0) / 8.0;
            float Gs = (NdotV / (NdotV * (1.0 - k) + k)) *
                       (NdotL / (NdotL * (1.0 - k) + k));
            float3 specular = NDF * Gs * F / max(4.0 * NdotV * NdotL, 1e-4);
            float3 kD = (1.0 - F) * (1.0 - metal);
            // G carries cos_surface; the BRDF terms above must not repeat it.
            lighting = (kD * albedo / 3.14159265 + specular) * p.radiance *
                       G * W;
        } else {
            W = 0.0;
        }
    }

    if (commit) {
        uint M = min(r.M, EMISSIVE_MAX_M);
        uint packedUV = 0u, index = 0u;
        if (r.index != 0xffffffffu && W > 0.0) {
            index = r.index;
            packedUV = (uint)round(saturate(r.u) * 65535.0) |
                       ((uint)round(saturate(r.v) * 65535.0) << 16u);
        } else {
            M = 0u;
        }
        emissiveReservoirs[EmissiveReservoirIndex(pixel, parity)] = uint4(
            index, packedUV, asuint(W),
            M | (EmissiveCheck(index, packedUV) << 16u));
    }
    return lighting;
}

// Light from emissive triangles arriving at a GI ray hit, in the same units
// RayHitSunLight returns (multiplied by albedo, no 1/pi, by its caller), so the
// sun and the lamps keep their relative weight in the bounce.
float3 EmissiveHitIncoming(float3 position, float3 normal) {
    uint count = EmissiveLightCount();
    if (count == 0u) return 0.0;
    uint shadeSequence = enhancedFrameIndex;
#if SGE_VARIABLE_RATE_GI
    // Gradient replay includes the stochastic emitter choices at the hit, not
    // just the primary hemisphere ray, so noise cannot masquerade as a change.
    if (gVRRTOverride) shadeSequence = gVRRTSequence;
#endif
    uint rng = EmissivePcg(asuint(position.x) ^ EmissivePcg(asuint(position.y) ^
        EmissivePcg(asuint(position.z) ^ shadeSequence)));
    EmissiveReservoir r;
    r.index = 0xffffffffu;
    r.u = r.v = 0.0;
    r.wSum = 0.0;
    r.targetSelected = 0.0;
    r.M = EMISSIVE_HIT_CANDIDATES;
    [loop]
    for (uint c = 0u; c < EMISSIVE_HIT_CANDIDATES; ++c) {
        uint index = EmissivePickTriangle(EmissiveRandom(rng),
                                          EmissiveRandom(rng), count);
        float u = EmissiveRandom(rng), v = EmissiveRandom(rng);
        EmissivePoint p = EmissiveEvaluate(index, u, v);
        float3 direction;
        float distance;
        float target = EmissiveLuma(p.radiance) *
            EmissiveGeometry(p, position, normal, direction, distance);
        EmissiveUpdate(r, index, u, v, target,
                       target / max(p.pdfArea, 1e-12), EmissiveRandom(rng));
    }
    if (r.index == 0xffffffffu || r.targetSelected <= 0.0) return 0.0;
    float W = r.wSum / ((float)r.M * r.targetSelected);
    EmissivePoint p = EmissiveEvaluate(r.index, r.u, r.v);
    float3 L;
    float distance;
    float G = EmissiveGeometry(p, position, normal, L, distance);
    if (G <= 0.0 || !EmissiveVisible(position, normal, L, distance)) return 0.0;
    return p.radiance * G * W;
}

#endif
