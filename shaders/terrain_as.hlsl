// Terrain amplification shader: one thread per terrain tile. Frustum-culls
// each tile's AABB and picks a tessellation LOD from camera distance, then
// dispatches one mesh shader group per visible tile.

cbuffer MatrixBuffer : register(b0) {
    matrix model;
    matrix view;
    matrix projection;
    matrix lightSpaceMatrix;
    matrix modelView;
    matrix modelViewProjection;
};

cbuffer CameraBuffer : register(b2) {
    float3 viewPos;
    float cameraPadding;
};

// Root param 8 (8 x 32-bit constants at b6), repurposed for terrain params.
cbuffer TerrainParams : register(b6) {
    uint tilesX;
    uint tilesZ;
    float tileSize;
    float heightScale;
    float lodNear;        // distance where LOD starts to drop
    float lodStep;        // distance per LOD level after lodNear
    float skirtDepth;
    float flattenRadius;  // level pad around origin for the house
    float islandScaleX;   // per-axis coastline stretch
    float islandScaleZ;
    uint sculptCount;
    float sculptMaxDisplacement;
    int originTileX;   // grid min-corner offset in tiles (0 = centered)
    int originTileZ;
    uint terrainStyle; // 0 = smooth radial coast, 1 = stress island layout
};

// Payload carries each visible tile's world origin (min corner) + tile size
// directly, so the mesh shader is agnostic to whether the tile came from the
// legacy uniform grid or a camera-centered clipmap ring.
struct TerrainPayload {
    float2 originXZ[32];   // world min-corner of the tile
    float  size[32];       // tile world size (metres)
    uint   lod[32];        // tessellation level 0..3
};

// Clipmap is enabled by terrainStyle bit 1 (value & 2). In that mode:
//   tilesX = ring grid size G (tiles per side of each ring, even)
//   tilesZ = ring count R
//   tileSize = base (innermost) tile size
static const uint kClipmapFlag = 2u;

groupshared TerrainPayload payloadData;
groupshared uint visibleCount;

bool TileIntersectsFrustum(float3 center, float radius) {
    float4 clip = mul(float4(center, 1), modelViewProjection);
    float radiusX = radius * abs(projection[0][0]);
    float radiusY = radius * abs(projection[1][1]);
    float radiusZ = radius *
        (abs(projection[2][2]) + abs(projection[2][3]));
    return clip.x + radiusX >= -clip.w && clip.x - radiusX <= clip.w &&
           clip.y + radiusY >= -clip.w && clip.y - radiusY <= clip.w &&
           clip.z + radiusZ >= 0.0 && clip.z - radiusZ <= clip.w + radiusZ;
}

// Resolve a linear clipmap tile index into its world min-corner + tile size.
// Ring 0 is a solid GxG block of base-size tiles centred on the camera; each
// outer ring r doubles the tile size and is a hollow GxG block (its inner
// G/2 x G/2 region is covered by the finer ring inside it). Ring origins snap
// to their own tile grid so tiles stay stable as the camera moves.
bool ResolveClipmapTile(uint id, uint G, uint R, float base,
                        out float2 originXZ, out float sizeOut) {
    originXZ = float2(0, 0); sizeOut = base;
    uint ring0 = G * G;
    uint ringN = ring0 - (G / 2) * (G / 2);   // hollow ring tile count
    // Which ring does this id fall in?
    uint ring; uint local;
    if (id < ring0) { ring = 0; local = id; }
    else {
        uint rem = id - ring0;
        ring = 1 + rem / ringN;
        local = rem % ringN;
        if (ring >= R) return false;
    }
    // Map 'local' to a (lx,lz) in [0,G), skipping the central hole for ring>0.
    uint lx, lz;
    if (ring == 0) { lx = local % G; lz = local / G; }
    else {
        // Walk the GxG block in row-major order but skip the inner quarter.
        uint q0 = G / 4, q1 = G - G / 4;   // hole spans [q0,q1) on both axes
        uint idx = local, r = 0, c = 0, seen = 0;
        // Closed-form: count non-hole cells per row.
        uint holeW = q1 - q0;
        [loop] for (uint row = 0; row < G; ++row) {
            uint rowCells = (row >= q0 && row < q1) ? (G - holeW) : G;
            if (idx < rowCells) {
                lz = row;
                if (row >= q0 && row < q1) {
                    // skip the hole columns
                    lx = (idx < q0) ? idx : (idx - q0 + q1);
                } else lx = idx;
                seen = 1; break;
            }
            idx -= rowCells;
        }
        if (seen == 0) return false;
    }
    float t = base * (float)(1u << ring);
    // Snap the ring's centre to its tile grid around the camera.
    float halfSpan = (float)G * 0.5 * t;
    float2 snapped = floor(viewPos.xz / t) * t;
    originXZ = snapped + (float2(lx, lz) - (float)(G / 2)) * t;
    sizeOut = t;
    return true;
}

[numthreads(32, 1, 1)]
void ASMain(uint threadID : SV_GroupThreadID, uint3 groupID : SV_GroupID) {
    if (threadID == 0) visibleCount = 0;
    GroupMemoryBarrierWithGroupSync();

    uint tileId = groupID.x * 32 + threadID;
    const bool clipmap = (terrainStyle & kClipmapFlag) != 0u;

    float2 originXZ; float tsize; bool valid = false;
    if (clipmap) {
        valid = ResolveClipmapTile(tileId, tilesX, tilesZ, tileSize,
                                   originXZ, tsize);
    } else {
        uint tileCount = tilesX * tilesZ;
        if (tileId < tileCount) {
            uint tx = tileId % tilesX;
            uint tz = tileId / tilesX;
            originXZ = float2(
                ((float)tx + (float)originTileX - (float)tilesX * 0.5) * tileSize,
                ((float)tz + (float)originTileZ - (float)tilesZ * 0.5) * tileSize);
            tsize = tileSize;
            valid = true;
        }
    }

    if (valid) {
        float verticalReach = heightScale + sculptMaxDisplacement;
        float3 center3 = float3(originXZ.x + tsize * 0.5,
            (verticalReach - skirtDepth) * 0.5,
            originXZ.y + tsize * 0.5);
        float3 halfExtent = float3(tsize * 0.5,
            (verticalReach + skirtDepth) * 0.5, tsize * 0.5);

        if (TileIntersectsFrustum(center3, length(halfExtent))) {
            float2 center = originXZ + tsize * 0.5;
            float dist = length(center - viewPos.xz);
            // In clipmap mode each ring already sets its resolution by tile
            // size, so keep near tiles at full tessellation; still drop LOD for
            // the coarse outer rings to save triangles.
            uint lod = (uint)clamp((dist - lodNear) / lodStep, 0.0, 3.0);

            uint slot;
            InterlockedAdd(visibleCount, 1, slot);
            payloadData.originXZ[slot] = originXZ;
            payloadData.size[slot] = tsize;
            payloadData.lod[slot] = lod;
        }
    }

    GroupMemoryBarrierWithGroupSync();
    DispatchMesh(visibleCount, 1, 1, payloadData);
}
