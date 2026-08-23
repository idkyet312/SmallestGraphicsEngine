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
    uint detailRelief; // 1 = extra low/high frequency relief octaves
};

// Payload carries each visible tile's world origin (min corner) + tile size
// directly, so the mesh shader is agnostic to whether the tile came from the
// legacy uniform grid or a camera-centered clipmap ring.
struct TerrainPayload {
    float2 originXZ[32];   // world min-corner of the tile
    float  size[32];       // tile world size (metres)
    uint   lod[32];        // tessellation level 0..3
    float  morph[32];      // 0..1 blend toward the next-coarser LOD (geomorph)
};

// Clipmap is enabled by terrainStyle bit 1 (value & 2). In that mode:
//   tilesX = ring grid size G (tiles per side of each ring, even)
//   tilesZ = ring count R
//   tileSize = base (innermost) tile size
static const uint kClipmapFlag = 2u;
// Matches TerrainRendererDX12::kStylePinnedClipmapOrigin.
static const uint kPinnedOriginFlag = 8u;

groupshared TerrainPayload payloadData;
groupshared uint visibleCount;

bool TileIntersectsFrustum(float3 center, float radius) {
    // Clip inequalities include w, so expanding x/y/z alone is not a
    // conservative sphere test. Extract the six planes from the complete MVP;
    // otherwise tiles at the view edge can be rejected while their geometry is
    // still on screen, especially when the camera is close to a coarse ring.
    float4 planes[6] = {
        float4(modelViewProjection[0][0] + modelViewProjection[0][3],
               modelViewProjection[1][0] + modelViewProjection[1][3],
               modelViewProjection[2][0] + modelViewProjection[2][3],
               modelViewProjection[3][0] + modelViewProjection[3][3]),
        float4(modelViewProjection[0][3] - modelViewProjection[0][0],
               modelViewProjection[1][3] - modelViewProjection[1][0],
               modelViewProjection[2][3] - modelViewProjection[2][0],
               modelViewProjection[3][3] - modelViewProjection[3][0]),
        float4(modelViewProjection[0][1] + modelViewProjection[0][3],
               modelViewProjection[1][1] + modelViewProjection[1][3],
               modelViewProjection[2][1] + modelViewProjection[2][3],
               modelViewProjection[3][1] + modelViewProjection[3][3]),
        float4(modelViewProjection[0][3] - modelViewProjection[0][1],
               modelViewProjection[1][3] - modelViewProjection[1][1],
               modelViewProjection[2][3] - modelViewProjection[2][1],
               modelViewProjection[3][3] - modelViewProjection[3][1]),
        float4(modelViewProjection[0][2], modelViewProjection[1][2],
               modelViewProjection[2][2], modelViewProjection[3][2]),
        float4(modelViewProjection[0][3] - modelViewProjection[0][2],
               modelViewProjection[1][3] - modelViewProjection[1][2],
               modelViewProjection[2][3] - modelViewProjection[2][2],
               modelViewProjection[3][3] - modelViewProjection[3][2])
    };
    [unroll]
    for (uint planeIndex = 0; planeIndex < 6; ++planeIndex) {
        float4 plane = planes[planeIndex];
        float distance = dot(float4(center, 1.0), plane);
        if (distance < -radius * length(plane.xyz)) return false;
    }
    return true;
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
    // ALL rings share one snap origin, quantised to the COARSEST ring's tile
    // size. Snapping each ring to its own grid (floor(viewPos/t)*t) is what
    // tore holes in the terrain: neighbouring rings then round the camera to
    // grids that differ by up to t, so a ring's outer edge and the hole meant
    // to receive it drift apart and open a one-tile seam that slides around as
    // the camera moves. One shared origin keeps every ring boundary and every
    // hole on common tile lines -- verified gap-free and overlap-free for
    // R = 2..8 at random camera positions.
    float snapGrid = base * (float)(1u << (R - 1));
    // kStylePinnedClipmapOrigin: snap to the world origin rather than the
    // camera. The spot shadow pass sets this because it never rebinds viewPos
    // to the light, so an unpinned origin would follow the player and make the
    // terrain under the beam jump a whole coarse tile each time the camera
    // crossed a snap cell. The ring layout is unchanged -- every ring still
    // shares ONE origin, which is what keeps the boundaries seam-free.
    const bool pinnedOrigin = (terrainStyle & kPinnedOriginFlag) != 0u;
    float2 snapCentre = pinnedOrigin ? float2(0.0, 0.0) : viewPos.xz;
    float2 snapped = floor(snapCentre / snapGrid) * snapGrid;
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
        // TerrainHeight adds a 2.5 m land lift and sinks the seabed to -6 m;
        // skirts and sculpt stamps extend that range further. The old
        // [-skirtDepth, heightScale] bound excluded most underwater geometry,
        // so a tile could be culled while the part under the crosshair remained
        // visible.
        const float terrainMin = -6.0 - skirtDepth - sculptMaxDisplacement;
        const float terrainMax =
            heightScale + 2.5 + sculptMaxDisplacement;
        float3 center3 = float3(originXZ.x + tsize * 0.5,
            (terrainMin + terrainMax) * 0.5,
            originXZ.y + tsize * 0.5);
        float3 halfExtent = float3(tsize * 0.5,
            (terrainMax - terrainMin) * 0.5, tsize * 0.5);

        if (TileIntersectsFrustum(center3, length(halfExtent))) {
            float2 center = originXZ + tsize * 0.5;
            float dist = length(center - viewPos.xz);
            // In clipmap mode each ring already sets its resolution by tile
            // size, so keep near tiles at full tessellation; still drop LOD for
            // the coarse outer rings to save triangles.
            float lodF = clamp((dist - lodNear) / lodStep, 0.0, 3.0);
            uint lod = (uint)lodF;
            // Geomorph: as the tile nears the threshold to the next-coarser LOD,
            // morph rises 0->1 over the last 30% of the band so fine vertices
            // slide onto the coarse grid instead of popping.
            float morph = smoothstep(0.7, 1.0, frac(lodF));

            uint slot;
            InterlockedAdd(visibleCount, 1, slot);
            payloadData.originXZ[slot] = originXZ;
            payloadData.size[slot] = tsize;
            payloadData.lod[slot] = lod;
            payloadData.morph[slot] = morph;
        }
    }

    GroupMemoryBarrierWithGroupSync();
    DispatchMesh(visibleCount, 1, 1, payloadData);
}
