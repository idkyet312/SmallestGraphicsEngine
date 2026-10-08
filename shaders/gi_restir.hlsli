// ReSTIR GI for the Lumen bounce [Ouyang et al. 2021], temporal + spatial,
// run inline in the resolve.
//
// Each pixel still traces one cosine ray. Its hit becomes a candidate in a
// per-pixel reservoir that also merges last frame's reservoir at the
// reprojected pixel and a few of its neighbours there. Because every reused
// reservoir comes from LAST frame's buffer, the resolve never reads a texel
// another thread is writing, and no extra pass is needed. Selection favours
// bright directions, so a doorway or a sunlit patch seen by one pixel's ray
// is shared with the pixels around it on later frames.
//
// Target function p^ = luminance(L) * cos / pi, so a fresh cosine-sampled
// candidate weighs luminance(L) and a one-sample reservoir reproduces the
// plain Lumen estimate exactly. Reused samples are reconnected to the new
// shading point with the distance part of the solid-angle Jacobian; the
// cosine at the sample is unknown (cache hits never read the hit normal) and
// taken as equal. No visibility ray is cast for reused samples, which is the
// standard biased variant: it can leak at contact corners. Reused radiance is
// re-read from the radiance cache when one is active, so lighting changes
// reach old samples.
//
// Storage, two frames of pixels, 24 bytes each:
//   restirSample[i] = sample position (or direction for sky), RGB9E5 radiance
//   restirWeight[i] = asuint(W), f16 hit distance | M << 16 | sky << 31

#ifndef GI_RESTIR_HLSLI
#define GI_RESTIR_HLSLI

RWStructuredBuffer<uint4> restirSample : register(u17);
RWStructuredBuffer<uint2> restirWeight : register(u18);

// Temporal history is capped at this many candidates. Higher values converge
// further but keep a stale sample alive longer after a lighting change.
#define RESTIR_MAX_M 20u
#define RESTIR_SPATIAL_RADIUS 16.0
// Reconnection Jacobians outside this range are rejected rather than clamped:
// they come from neighbours whose view of the sample differs too much.
#define RESTIR_MAX_JACOBIAN 4.0

struct GISample {
    float3 position;   // hit point, or the ray direction when sky
    float3 radiance;   // sanitized, before giIntensity
    float  distance;   // origin-to-hit distance (0 for sky)
    bool   sky;
};

struct GIReservoir {
    GISample s;
    float wSum;
    float W;
    uint  M;
};

uint RestirPackRGB9E5(float3 rgb) {
    const float kMax = 65408.0;  // (2^9 - 1) / 2^9 * 2^16
    rgb = clamp(rgb, 0.0, kMax);
    float maxc = max(rgb.r, max(rgb.g, rgb.b));
    int exponent = max(-16, (int)floor(log2(max(maxc, 1e-30)))) + 1 + 15;
    float scale = exp2((float)(exponent - 15 - 9));
    if ((uint)round(maxc / scale) == 512u) {
        exponent += 1;
        scale *= 2.0;
    }
    uint3 m = (uint3)round(rgb / scale);
    return m.r | (m.g << 9u) | (m.b << 18u) | ((uint)exponent << 27u);
}

float3 RestirUnpackRGB9E5(uint v) {
    float scale = exp2((float)((int)(v >> 27u) - 15 - 9));
    return float3(v & 0x1ffu, (v >> 9u) & 0x1ffu, (v >> 18u) & 0x1ffu) * scale;
}

float RestirLuma(float3 c) { return dot(c, float3(0.2126, 0.7152, 0.0722)); }

uint RestirIndex(uint2 pixel, uint parity) {
    uint pixels = (uint)screenWidth * (uint)screenHeight;
    return parity * pixels + pixel.y * (uint)screenWidth + pixel.x;
}

GIReservoir RestirLoad(uint index) {
    uint4 a = restirSample[index];
    uint2 b = restirWeight[index];
    GIReservoir r;
    r.s.position = asfloat(a.xyz);
    r.s.radiance = RestirUnpackRGB9E5(a.w);
    r.s.distance = f16tof32(b.y);
    r.s.sky = (b.y >> 31u) != 0u;
    r.M = (b.y >> 16u) & 0x7fffu;
    r.W = asfloat(b.x);
    r.wSum = 0.0;
    // A corrupt or never-written texel must read as empty, not as a weight.
    if (!(r.W >= 0.0 && r.W < 1e20)) r.M = 0u;
    return r;
}

void RestirStore(uint index, GIReservoir r) {
    restirSample[index] = uint4(asuint(r.s.position),
                                RestirPackRGB9E5(r.s.radiance));
    restirWeight[index] = uint2(asuint(r.W),
        (f32tof16(min(r.s.distance, 60000.0)) & 0xffffu) |
        (min(r.M, 0x7fffu) << 16u) | (r.s.sky ? 0x80000000u : 0u));
}

// Streaming reservoir update; `xi` in [0, 1).
bool RestirUpdate(inout GIReservoir r, GISample s, float w, float xi) {
    r.wSum += w;
    if (w > 0.0 && xi * r.wSum < w) {
        r.s = s;
        return true;
    }
    return false;
}

// Direction from the shading point to the sample, and the cosine there.
float3 RestirDirection(GISample s, float3 worldPos, out float distanceSq) {
    if (s.sky) {
        distanceSq = 0.0;
        return s.position;
    }
    float3 d = s.position - worldPos;
    distanceSq = dot(d, d);
    return d * rsqrt(max(distanceSq, 1e-8));
}

float RestirTargetPdf(GISample s, float3 worldPos, float3 normal) {
    float distanceSq;
    float3 dir = RestirDirection(s, worldPos, distanceSq);
    return RestirLuma(s.radiance) * saturate(dot(normal, dir)) * (1.0 / 3.14159265);
}

#endif // GI_RESTIR_HLSLI
