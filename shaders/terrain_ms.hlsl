// Terrain mesh shader: one group per visible tile. Generates an (n+1)x(n+1)
// heightfield grid where n = 8 >> LOD quads (the tessellation), plus a skirt
// ring around the perimeter that hides cracks between neighboring tiles of
// different LOD. Height is procedural fBm noise of world XZ, so geometry is
// seamless across tiles and needs no CPU vertex data at all.

cbuffer MatrixBuffer : register(b0) {
    matrix model;
    matrix view;
    matrix projection;
    matrix lightSpaceMatrix;
};

cbuffer TerrainParams : register(b6) {
    uint tilesX;
    uint tilesZ;
    float tileSize;
    float heightScale;
    float lodNear;
    float lodStep;
    float skirtDepth;
    float flattenRadius;
    float islandScaleX;   // per-axis coastline stretch
    float islandScaleZ;
    uint sculptCount;
    float sculptMaxDisplacement;
    int originTileX;   // grid min-corner offset in tiles (0 = centered)
    int originTileZ;
    uint terrainStyle; // 0 = smooth radial coast, 1 = stress island layout
    uint detailRelief; // 1 = extra low/high frequency relief octaves
};

// Must match SculptGPU in TerrainRendererDX12.h (40 bytes).
struct TerrainSculptStamp {
    float3 centerRadius;
    uint operation;
    float value;
    float strength;
    uint textureIndex;
    float rotation;
    float replace;     // 0 = additive relief, 1 = overwrite ground
    float baseHeight;  // world height a replace stamp is built around
};
StructuredBuffer<TerrainSculptStamp> terrainSculpt : register(t10);
ByteAddressBuffer terrainStampAtlas : register(t11);

// Must match TerrainStampLibrary.h -- these index the same atlas buffer, and a
// mismatch reads every layer but the first at the wrong offset.
static const uint kTerrainStampResolution = 512;
static const uint kMaxTerrainStampTextures = 64;

float LoadTerrainStamp(uint layer, uint2 texel) {
    uint element = layer * kTerrainStampResolution * kTerrainStampResolution +
        texel.y * kTerrainStampResolution + texel.x;
    uint packed = terrainStampAtlas.Load((element >> 1) * 4);
    uint value = (element & 1) != 0 ? packed >> 16 : packed & 0xffff;
    return value * (1.0 / 65535.0);
}

// Returns the stamp's displacement in metres (x) and its coverage (y): 1 in
// the middle, feathering to 0 at the border. Replace mode needs coverage
// separately -- flat parts of a heightmap still have to overwrite the ground
// under them, and they carry no relief to signal that.
float2 SampleTerrainStamp(TerrainSculptStamp stamp, float2 xz) {
    if (stamp.textureIndex >= kMaxTerrainStampTextures) return 0.0;
    float sine, cosine;
    sincos(radians(stamp.rotation), sine, cosine);
    float2 delta = xz - stamp.centerRadius.xy;
    float2 local = float2(delta.x * cosine + delta.y * sine,
                          -delta.x * sine + delta.y * cosine);
    float2 uv = local / (stamp.centerRadius.z * 2.0) + 0.5;
    if (any(uv < 0.0) || any(uv > 1.0)) return 0.0;


    float2 samplePosition = uv * (kTerrainStampResolution - 1);
    uint2 p0 = uint2(samplePosition);
    uint2 p1 = min(p0 + 1, kTerrainStampResolution - 1);
    float2 blend = frac(samplePosition);
    float upper = lerp(LoadTerrainStamp(stamp.textureIndex, p0),
                       LoadTerrainStamp(stamp.textureIndex, uint2(p1.x, p0.y)),
                       blend.x);
    float lower = lerp(LoadTerrainStamp(stamp.textureIndex, uint2(p0.x, p1.y)),
                       LoadTerrainStamp(stamp.textureIndex, p1), blend.x);
    float height = lerp(upper, lower, blend.y) * 2.0 - 1.0;
    float edge = 1.0 - smoothstep(0.82, 1.0,
        max(abs(local.x), abs(local.y)) / stamp.centerRadius.z);
    return float2(height * stamp.value * edge, edge);
}

struct TerrainPayload {
    float2 originXZ[32];   // world min-corner of the tile
    float  size[32];       // tile world size (metres)
    uint   lod[32];
    float  morph[32];      // 0..1 blend toward the next-coarser LOD (geomorph)
};

struct OutVertex {
    float4 position : SV_Position;
    float3 fragPos : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 texCoord : TEXCOORD2;
    float4 tangent : TEXCOORD3;
    float4 fragPosLightSpace : TEXCOORD4;
};

uint hashUint(uint value) {
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    value ^= value >> 16;
    return value;
}

float hash21(float2 p) {
    // p contains integer lattice coordinates. Integer avalanche mixing avoids
    // the short periods created by frac(integer * decimal constants).
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
        // Scale by 2.02 and rotate each octave. Keeping every lattice axis
        // aligned made unrelated octaves reinforce into long parallel ridges.
        p = float2(p.x * 1.616 - p.y * 1.212,
                   p.x * 1.212 + p.y * 1.616);
        amp *= 0.5;
    }
    return sum;
}

// A dug-out basin the water pool sits in, so the pool reads as a hole in the
// ground rather than a box on top of it. Centre/reach/depth must match the pool
// spawned in main.cpp (and the CPU mirror in TerrainRendererDX12::HeightAt).
static const float2 kPoolCenter = float2(-22.0, -20.0);
static const float  kPoolRadius = 4.2;   // flat basin floor out to here
static const float  kPoolRim    = 7.0;   // slopes back up to ground by here
static const float  kPoolDepth  = 3.0;   // how far the floor drops below ground

// Island shape. Sea level is y = 0 (where the ocean plane sits), so the land is
// lifted above it and the seabed dropped below it. The terrain grid spans +-128 m,
// so the shore ramp must finish comfortably inside that or the mesh edge shows.
// Must match TerrainRendererDX12::HeightAt.
static const float kLandLift      =  2.5; // how far the island sits above sea level
static const float kSeabed        = -6.0; // sea floor depth past the shore
static const float kBeachStart    = 28.0; // terrain relief starts flattening
static const float kBeachShelf    = 35.0; // broad, nearly flat sand begins
static const float kBeachWaterline = 43.0; // beach finishes just below sea level
// Full seabed depth is reached this far out. The drop used to finish at 52,
// taking the floor from -0.25 to -6.0 in only 9 m -- a visible underwater cliff
// right off the beach. Stretching it to 88 spreads the same 5.75 m over 45 m,
// so the bottom shelves away gradually. The clipmap rings reach ~256 m, so this
// still finishes well inside the drawn terrain.
static const float kShoreOuter    = 88.0;
static const float kBeachHigh     =  0.65;
static const float kBeachLow      = -0.25;

// Flat arena under the four houses arranged around world centre. Applied after
// everything else and faded out before the relocated pool basin.
// kPadHeight is Ground::kBuildingPadY (world/terrain/GroundLevel.h) -- the houses and their
// roofs are all built up from that constant, so if this drifts they end up buried
// in the sand or floating over it. Must match TerrainRendererDX12::HeightAt.
static const float  kPadRadius = 14.0;   // dead flat through all four foundations
static const float  kPadFade   = 18.0;   // blended back into the terrain by here
static const float  kPadHeight = 2.5;    // = kLandLift: island's natural ground level
static const float2 kStressPadCenters[8] = {
    float2(  0.0,   0.0), float2(42.0,   0.0),
    float2(-42.0,   0.0), float2( 0.0,  42.0),
    float2( 42.0,  42.0), float2(-42.0, 42.0),
    float2(  0.0, -42.0), float2(42.0, -42.0)
};

// Applies every live sculpt stamp to an already-shaped ground height. Shared by
// the procedural island and the flat authoring plane so the two cannot drift.
// Must match the sculpt loop in TerrainRendererDX12::HeightAt.
float ApplySculpt(float h, float2 xz) {
    for (uint stampIndex = 0; stampIndex < sculptCount; ++stampIndex) {
        TerrainSculptStamp stamp = terrainSculpt[stampIndex];
        if (stamp.operation == 2) {
            // Additive gives h + relief, replace gives baseHeight + relief, and
            // `replace` cross-fades between them under the same edge feather.
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
    // Flat authoring plane (style bit 2). Returns before the relief, the pool
    // basin and the coast falloff, so the only thing that can move the ground
    // is a sculpt stamp. kLandLift keeps it at the island's natural ground
    // level, which is where the building pads and spawns already sit.
    if ((terrainStyle & 4u) != 0u) return ApplySculpt(kLandLift, xz);
    float h = fbm(xz * 0.08) * heightScale;
    // Optional relief detail. The base fbm runs at a single 0.08 frequency, so
    // everything varies at one scale -- no broad landforms and no fine surface
    // break-up. A low octave adds hills the base cannot express; a high one
    // roughens the surface underfoot. Must match TerrainRendererDX12::HeightAt.
    if (detailRelief != 0u) {
        h += noise2(xz * 0.015) * heightScale * 1.55;
        h += noise2(xz * 0.42) * heightScale * 0.075;
    }
    // Level pad around the origin so the house sits on flat ground.
    float mask = smoothstep(flattenRadius, flattenRadius * 2.0, length(xz));
    h *= mask;
    // Carve the pool basin: 1 at the centre, 0 past the rim -> subtract depth.
    float d = length(xz - kPoolCenter);
    float basin = 1.0 - smoothstep(kPoolRadius, kPoolRim, d);
    h -= kPoolDepth * basin;

    // Island falloff: first remove inland relief into a broad, low beach shelf.
    // The shelf slopes gently through sea level, then a second smooth curve
    // carries the shallow seabed down to full depth. Splitting the old single
    // ramp keeps the beach flat instead of turning the whole coast into a bank.
    // Must match TerrainRendererDX12::HeightAt.
    // Per-axis island size: normalise the coordinate by each axis' scale so the
    // coastline stretches independently on X and Z. Shore thresholds stay at
    // their base radii in this normalised space. Must match HeightAt.
    float2 n = xz / float2(max(0.01, islandScaleX), max(0.01, islandScaleZ));
    float maxScale = max(islandScaleX, islandScaleZ);
    float coastDistance = length(n);
    if (terrainStyle == 1 && maxScale > 1.5) {
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

    float underwater = smoothstep(
        kBeachWaterline, kShoreOuter, coastDistance);
    h = lerp(h, kSeabed, underwater);

    // Building pad, applied LAST so nothing else can dent it. The pool rim was
    // biting into the house footprint and dropping one corner ~2 m; forcing the
    // pad flat here means the houses always sit on genuinely level ground.
    uint padCount = (terrainStyle == 1 && maxScale > 1.5) ? 8 : 1;
    for (uint i = 0; i < padCount; ++i) {
        float pad = 1.0 - smoothstep(
            kPadRadius, kPadFade, length(xz - kStressPadCenters[i]));
        h = lerp(h, kPadHeight, pad);
    }
    return ApplySculpt(h, xz);
}

// k-th vertex along the tile perimeter, counterclockwise, k in [0, 4n).
uint2 PerimeterCoord(uint k, uint n) {
    if (k < n)     return uint2(k, 0);
    if (k < 2 * n) return uint2(n, k - n);
    if (k < 3 * n) return uint2(n - (k - 2 * n), n);
    return uint2(0, n - (k - 3 * n));
}

OutVertex MakeVertex(float2 xz, float y) {
    float3 worldPos = mul(float4(xz.x, y, xz.y, 1), model).xyz;

    // Finite-difference normal from the height function (skirt verts reuse the
    // surface normal of the perimeter vertex above them, which is fine - they
    // are crack fillers, not visible surface).
    float eps = 0.35;
    float hL = TerrainHeight(xz - float2(eps, 0));
    float hR = TerrainHeight(xz + float2(eps, 0));
    float hD = TerrainHeight(xz - float2(0, eps));
    float hU = TerrainHeight(xz + float2(0, eps));
    float3 normal = normalize(float3(hL - hR, 2.0 * eps, hD - hU));
    float3 tangent = normalize(float3(2.0 * eps, hR - hL, 0));

    OutVertex v;
    float4 viewPosition = mul(mul(float4(worldPos, 1), view), projection);
    v.position = viewPosition;
    v.fragPos = worldPos;
    v.normal = normalize(mul(normal, (float3x3)model));
    v.texCoord = xz * 0.2; // match the old floor plane's mud tiling (8 reps / 40 m)
    v.tangent = float4(normalize(mul(tangent, (float3x3)model)), 1.0);
    v.fragPosLightSpace = mul(float4(worldPos, 1), lightSpaceMatrix);
    return v;
}

[outputtopology("triangle")]
[numthreads(128, 1, 1)]
void MSMain(uint3 id : SV_GroupThreadID,
            uint3 groupID : SV_GroupID,
            in payload TerrainPayload payloadData,
            out vertices OutVertex verts[113],
            out indices uint3 tris[192]) {
    float2 tileOrigin = payloadData.originXZ[groupID.x];
    float thisTileSize = payloadData.size[groupID.x];
    uint lod = min(payloadData.lod[groupID.x], 3u);
    float morph = payloadData.morph[groupID.x];
    uint n = 8u >> lod; // quads per side: 8/4/2/1

    uint side = n + 1;
    uint gridVerts = side * side;
    uint skirtVerts = 4 * n;
    uint gridTris = 2 * n * n;
    uint skirtTris = 8 * n;
    SetMeshOutputCounts(gridVerts + skirtVerts, gridTris + skirtTris);

    float quadSize = thisTileSize / (float)n;

    for (uint vi = id.x; vi < gridVerts + skirtVerts; vi += 128) {
        float2 xz;
        float y;
        if (vi < gridVerts) {
            uint gx = vi % side;
            uint gz = vi / side;
            // Geomorph toward the next-coarser grid: blend each vertex's index
            // toward the even (coarse) index by 'morph', so odd vertices slide
            // onto the coarse grid as the tile approaches its LOD switch. Avoids
            // popping. Even indices are unchanged (morph target == self).
            float2 fine = float2(gx, gz);
            float2 coarse = floor(fine * 0.5) * 2.0;
            float2 morphed = lerp(fine, coarse, morph);
            xz = tileOrigin + morphed * quadSize;
            y = TerrainHeight(xz);
        } else {
            uint2 pc = PerimeterCoord(vi - gridVerts, n);
            xz = tileOrigin + float2(pc.x, pc.y) * quadSize;
            y = TerrainHeight(xz) - skirtDepth;
        }
        verts[vi] = MakeVertex(xz, y);
    }

    for (uint pi = id.x; pi < gridTris + skirtTris; pi += 128) {
        if (pi < gridTris) {
            uint quad = pi / 2;
            uint qx = quad % n;
            uint qz = quad / n;
            uint v00 = qz * side + qx;
            uint v10 = v00 + 1;
            uint v01 = v00 + side;
            uint v11 = v01 + 1;
            tris[pi] = (pi & 1) ? uint3(v10, v01, v11) : uint3(v00, v01, v10);
        } else {
            uint sq = (pi - gridTris) / 2;  // skirt quad index in [0, 4n)
            uint kA = sq;
            uint kB = (sq + 1) % (4 * n);
            uint2 pcA = PerimeterCoord(kA, n);
            uint2 pcB = PerimeterCoord(kB, n);
            uint topA = pcA.y * side + pcA.x;
            uint topB = pcB.y * side + pcB.x;
            uint botA = gridVerts + kA;
            uint botB = gridVerts + kB;
            tris[pi] = ((pi - gridTris) & 1)
                ? uint3(topB, botA, botB)
                : uint3(topA, botA, topB);
        }
    }
}
