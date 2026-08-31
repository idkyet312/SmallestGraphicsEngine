#ifndef SGE_TERRAIN_HEIGHT_HLSLI
#define SGE_TERRAIN_HEIGHT_HLSLI

cbuffer TerrainParams : register(b6) {
    uint tilesX;
    uint tilesZ;
    float tileSize;
    float heightScale;
    float lodNear;
    float lodStep;
    float skirtDepth;
    float flattenRadius;
    float islandScaleX;
    float islandScaleZ;
    uint sculptCount;
    float sculptMaxDisplacement;
    int originTileX;
    int originTileZ;
    uint terrainStyle;
    uint detailRelief;
};

// Must match SculptGPU in TerrainRendererDX12.h (44 bytes).
struct TerrainSculptStamp {
    float3 centerRadius;
    uint operation;
    float value;
    float strength;
    uint textureIndex;
    float rotation;
    float replace;
    float baseHeight;
    float edgeFalloff;
};
StructuredBuffer<TerrainSculptStamp> terrainSculpt : register(t10);
ByteAddressBuffer terrainStampAtlas : register(t11);

static const uint kTerrainStampResolution = 512;
static const uint kMaxTerrainStampTextures = 64;
static const uint kTerrainStampBakeResolution = 4096;
static const uint kTerrainStampBakeLayer = kMaxTerrainStampTextures;

uint TerrainStampResolution(uint layer) {
    return layer == kTerrainStampBakeLayer
        ? kTerrainStampBakeResolution : kTerrainStampResolution;
}

float LoadTerrainStamp(uint layer, uint2 texel) {
    uint base = layer == kTerrainStampBakeLayer
        ? kMaxTerrainStampTextures * kTerrainStampResolution *
              kTerrainStampResolution
        : layer * kTerrainStampResolution * kTerrainStampResolution;
    uint side = TerrainStampResolution(layer);
    uint element = base + texel.y * side + texel.x;
    uint packed = terrainStampAtlas.Load((element >> 1) * 4);
    uint value = (element & 1) != 0 ? packed >> 16 : packed & 0xffff;
    return value * (1.0 / 65535.0);
}

// Returns displacement in metres and the replace-stamp coverage separately.
float2 SampleTerrainStamp(TerrainSculptStamp stamp, float2 xz) {
    if (stamp.textureIndex > kTerrainStampBakeLayer) return 0.0;
    float sine, cosine;
    sincos(radians(stamp.rotation), sine, cosine);
    float2 delta = xz - stamp.centerRadius.xy;
    float2 local = float2(delta.x * cosine + delta.y * sine,
                          -delta.x * sine + delta.y * cosine);
    float2 uv = local / (stamp.centerRadius.z * 2.0) + 0.5;
    if (any(uv < 0.0) || any(uv > 1.0)) return 0.0;

    uint side = TerrainStampResolution(stamp.textureIndex);
    float2 samplePosition = uv * (side - 1);
    uint2 p0 = uint2(samplePosition);
    uint2 p1 = min(p0 + 1, side - 1);
    float2 blend = frac(samplePosition);
    float upper = lerp(LoadTerrainStamp(stamp.textureIndex, p0),
                       LoadTerrainStamp(stamp.textureIndex, uint2(p1.x, p0.y)),
                       blend.x);
    float lower = lerp(LoadTerrainStamp(stamp.textureIndex, uint2(p0.x, p1.y)),
                       LoadTerrainStamp(stamp.textureIndex, p1), blend.x);
    float height = lerp(upper, lower, blend.y) * 2.0 - 1.0;
    float falloffStart = min(max(stamp.edgeFalloff, 0.0), 0.999);
    float edge = 1.0 - smoothstep(falloffStart, 1.0,
        max(abs(local.x), abs(local.y)) / stamp.centerRadius.z);
    return float2(height * stamp.value * edge, edge);
}

uint hashUint(uint value) {
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return value;
}

float hash21(float2 p) {
    uint2 cell = asuint(int2(p));
    uint value = hashUint(cell.x ^ (hashUint(cell.y) + 0x9e3779b9u));
    return float(value & 0x00ffffffu) * (1.0 / 16777216.0);
}

float noise2(float2 p) {
    float2 i = floor(p);
    float2 f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    return lerp(lerp(hash21(i), hash21(i + float2(1, 0)), f.x),
                lerp(hash21(i + float2(0, 1)), hash21(i + 1), f.x), f.y);
}

float fbm(float2 p) {
    float sum = 0.0;
    float amp = 0.5;
    [unroll]
    for (int i = 0; i < 5; ++i) {
        sum += noise2(p) * amp;
        p = float2(p.x * 1.616 - p.y * 1.212,
                   p.x * 1.212 + p.y * 1.616);
        amp *= 0.5;
    }
    return sum;
}

static const float2 kPoolCenter = float2(-22.0, -20.0);
static const float  kPoolRadius = 4.2;
static const float  kPoolRim = 7.0;
static const float  kPoolDepth = 3.0;
static const float kLandLift = 2.5;
static const float kSeabed = -6.0;
static const float kBeachStart = 28.0;
static const float kBeachShelf = 35.0;
static const float kBeachWaterline = 43.0;
static const float kShoreOuter = 88.0;
static const float kBeachHigh = 0.65;
static const float kBeachLow = -0.25;
static const float kPadRadius = 14.0;
static const float kPadFade = 18.0;
static const float kPadHeight = 2.5;
static const float2 kStressPadCenters[8] = {
    float2(  0.0,   0.0), float2(42.0,   0.0),
    float2(-42.0,   0.0), float2( 0.0,  42.0),
    float2( 42.0,  42.0), float2(-42.0, 42.0),
    float2(  0.0, -42.0), float2(42.0, -42.0)
};

float ApplySculpt(float h, float2 xz) {
    for (uint stampIndex = 0; stampIndex < sculptCount; ++stampIndex) {
        TerrainSculptStamp stamp = terrainSculpt[stampIndex];
        if (stamp.operation == 2) {
            float2 sampled = SampleTerrainStamp(stamp, xz);
            h += sampled.x + (stamp.baseHeight - h) * (sampled.y * stamp.replace);
            continue;
        }
        float weight = saturate(1.0 -
            length(xz - stamp.centerRadius.xy) / stamp.centerRadius.z);
        weight = weight * weight * (3.0 - 2.0 * weight);
        if (stamp.operation == 0)
            h += stamp.value * weight;
        else
            h = lerp(h, stamp.value, saturate(stamp.strength * weight));
    }
    return h;
}

float TerrainHeight(float2 xz) {
    if ((terrainStyle & 4u) != 0u) return ApplySculpt(kLandLift, xz);
    float h = fbm(xz * 0.08) * heightScale;
    if (detailRelief != 0u) {
        h += noise2(xz * 0.015) * heightScale * 1.55;
        h += noise2(xz * 0.42) * heightScale * 0.075;
    }
    float mask = smoothstep(flattenRadius, flattenRadius * 2.0, length(xz));
    h *= mask;
    float d = length(xz - kPoolCenter);
    float basin = 1.0 - smoothstep(kPoolRadius, kPoolRim, d);
    h -= kPoolDepth * basin;

    float2 n = xz / float2(max(0.01, islandScaleX), max(0.01, islandScaleZ));
    float maxScale = max(islandScaleX, islandScaleZ);
    float coastDistance = length(n);
    if ((terrainStyle & 1u) != 0u && maxScale > 1.5) {
        float2 warped = n + float2(
            sin(n.y * 0.055) * 7.0 + sin((n.x + n.y) * 0.025) * 4.0,
            sin(n.x * 0.047) * 6.0 - sin((n.x - n.y) * 0.031) * 3.0);
        coastDistance = length(warped * float2(0.92, 1.06));
        float northwestBay = 1.0 - smoothstep(0.0, 22.0,
            length(n - float2(-55.0, 15.0)));
        float southeastHeadland = 1.0 - smoothstep(0.0, 26.0,
            length(n - float2(35.0, -55.0)));
        coastDistance += northwestBay * 13.0;
        coastDistance -= southeastHeadland * 11.0;
    }
    float land = h + kLandLift;
    float beachBlend = smoothstep(kBeachStart, kBeachShelf, coastDistance);
    float beachT = smoothstep(kBeachShelf, kBeachWaterline, coastDistance);
    float beachHeight = lerp(kBeachHigh, kBeachLow, beachT);
    h = lerp(land, beachHeight, beachBlend);
    float underwater = smoothstep(kBeachWaterline, kShoreOuter, coastDistance);
    h = lerp(h, kSeabed, underwater);

    uint padCount = ((terrainStyle & 1u) != 0u && maxScale > 1.5) ? 8 : 1;
    for (uint i = 0; i < padCount; ++i) {
        float pad = 1.0 - smoothstep(
            kPadRadius, kPadFade, length(xz - kStressPadCenters[i]));
        h = lerp(h, kPadHeight, pad);
    }
    return ApplySculpt(h, xz);
}

#endif
