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
};

struct TerrainPayload {
    uint tileId[32];
    uint lod[32];
};

struct OutVertex {
    float4 position : SV_Position;
    float3 fragPos : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 texCoord : TEXCOORD2;
    float4 tangent : TEXCOORD3;
    float4 fragPosLightSpace : TEXCOORD4;
};

float hash21(float2 p) {
    p = frac(p * float2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return frac(p.x * p.y);
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
        p = p * 2.02;
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
// lifted above it and the seabed dropped below it. The terrain grid spans +-64 m,
// so the shore ramp must finish comfortably inside that or the mesh edge shows.
// Must match TerrainRendererDX12::HeightAt.
static const float kLandLift   =  2.5;   // how far the island sits above sea level
static const float kSeabed     = -6.0;   // sea floor depth past the shore
static const float kShoreInner = 34.0;   // solid land out to here
static const float kShoreOuter = 52.0;   // fully underwater by here

// Flat arena under the four houses arranged around world centre. Applied after
// everything else and faded out before the relocated pool basin.
// kPadHeight is Ground::kBuildingPadY (src/GroundLevel.h) -- the houses and their
// roofs are all built up from that constant, so if this drifts they end up buried
// in the sand or floating over it. Must match TerrainRendererDX12::HeightAt.
static const float2 kPadCenter = float2(0.0, 0.0);
static const float  kPadRadius = 14.0;   // dead flat through all four foundations
static const float  kPadFade   = 18.0;   // blended back into the terrain by here
static const float  kPadHeight = 2.5;    // = kLandLift: island's natural ground level

float TerrainHeight(float2 xz) {
    float h = fbm(xz * 0.08) * heightScale;
    // Level pad around the origin so the house sits on flat ground.
    float mask = smoothstep(flattenRadius, flattenRadius * 2.0, length(xz));
    h *= mask;
    // Carve the pool basin: 1 at the centre, 0 past the rim -> subtract depth.
    float d = length(xz - kPoolCenter);
    float basin = 1.0 - smoothstep(kPoolRadius, kPoolRim, d);
    h -= kPoolDepth * basin;

    // Island falloff: the ground is lifted well above the waterline out to
    // kShoreInner, then ramps down past kShoreOuter to a seabed below sea level,
    // so the land ends in a beach and the ocean takes over. The ramp finishes
    // inside the terrain grid's own edge, so the mesh boundary is never visible
    // above water. Must match TerrainRendererDX12::HeightAt.
    float r = length(xz);
    float shore = smoothstep(kShoreInner, kShoreOuter, r);   // 0 inland -> 1 at sea
    h = lerp(h + kLandLift, kSeabed, shore);

    // Building pad, applied LAST so nothing else can dent it. The pool rim was
    // biting into the house footprint and dropping one corner ~2 m; forcing the
    // pad flat here means the houses always sit on genuinely level ground.
    float pad = 1.0 - smoothstep(kPadRadius, kPadFade, length(xz - kPadCenter));
    h = lerp(h, kPadHeight, pad);
    return h;
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
    uint tileId = payloadData.tileId[groupID.x];
    uint lod = min(payloadData.lod[groupID.x], 3u);
    uint n = 8u >> lod; // quads per side: 8/4/2/1

    uint side = n + 1;
    uint gridVerts = side * side;
    uint skirtVerts = 4 * n;
    uint gridTris = 2 * n * n;
    uint skirtTris = 8 * n;
    SetMeshOutputCounts(gridVerts + skirtVerts, gridTris + skirtTris);

    uint tx = tileId % tilesX;
    uint tz = tileId / tilesX;
    float2 tileOrigin = float2(
        ((float)tx - (float)tilesX * 0.5) * tileSize,
        ((float)tz - (float)tilesZ * 0.5) * tileSize);
    float quadSize = tileSize / (float)n;

    for (uint vi = id.x; vi < gridVerts + skirtVerts; vi += 128) {
        float2 xz;
        float y;
        if (vi < gridVerts) {
            uint gx = vi % side;
            uint gz = vi / side;
            xz = tileOrigin + float2(gx, gz) * quadSize;
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
