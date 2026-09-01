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

#include "terrain_height.hlsli"

// Payload carries each visible tile's world origin (min corner) + tile size
// directly, so the mesh shader is agnostic to whether the tile came from the
// legacy uniform grid or a camera-centered clipmap ring.
struct TerrainPayload {
    float2 originXZ[32];   // world min-corner of the tile
    float  size[32];       // tile world size (metres)
    uint   lod[32];        // tessellation level 0..3
    float  morph[32];      // 0..1 blend toward the next-coarser LOD (geomorph)
    uint   topology[32];   // seam exponents + exposed outer-edge mask
};

// Clipmap is enabled by terrainStyle bit 1 (value & 2). In that mode:
//   tilesX = ring grid size G (tiles per side of each ring, even)
//   tilesZ = ring count R
//   tileSize = base (innermost) tile size
static const uint kClipmapFlag = 2u;
// Matches TerrainRendererDX12::kStyleDeploymentOverview.
static const uint kDeploymentOverviewFlag = 8u;
static const uint kErrorLODFlag = 16u;

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
                        out float2 originXZ, out float sizeOut,
                        out uint ringOut, out uint lxOut, out uint lzOut) {
    originXZ = float2(0, 0); sizeOut = base;
    ringOut = lxOut = lzOut = 0;
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
    // Normal rendering follows its bound viewpoint. The deployment overview
    // instead pins every terrain pass to the island: its camera can sit
    // kilometres away, and following it would waste the fine rings on empty
    // air while also giving the colour and shadow passes different coverage.
    const bool islandCentered =
        (terrainStyle & kDeploymentOverviewFlag) != 0u;
    float2 clipmapCenter = islandCentered ? float2(0.0, 0.0) : viewPos.xz;
    float2 snapped = floor(clipmapCenter / snapGrid) * snapGrid;
    originXZ = snapped + (float2(lx, lz) - (float)(G / 2)) * t;
    sizeOut = t;
    ringOut = ring;
    lxOut = lx;
    lzOut = lz;
    return true;
}

// Finds the selected clipmap tile immediately across an edge. The search is
// fine-to-coarse, so points in an outer ring's central hole resolve to the finer
// ring that actually covers them.
bool ResolveClipmapTileAt(float2 samplePoint, uint G, uint R, float base,
                          out float2 originXZ, out float sizeOut,
                          out uint ringOut, out uint lxOut, out uint lzOut) {
    float snapGrid = base * (float)(1u << (R - 1));
    const bool islandCentered =
        (terrainStyle & kDeploymentOverviewFlag) != 0u;
    float2 clipmapCenter = islandCentered ? float2(0.0, 0.0) : viewPos.xz;
    float2 snapped = floor(clipmapCenter / snapGrid) * snapGrid;

    [loop]
    for (uint ring = 0; ring < R; ++ring) {
        float t = base * (float)(1u << ring);
        float2 lower = snapped - (float)(G / 2) * t;
        int2 cell = int2(floor((samplePoint - lower) / t));
        if (any(cell < 0) || any(cell >= (int)G)) continue;
        if (ring > 0) {
            uint q0 = G / 4, q1 = G - q0;
            if (cell.x >= (int)q0 && cell.x < (int)q1 &&
                cell.y >= (int)q0 && cell.y < (int)q1)
                continue;
        }
        originXZ = lower + float2(cell) * t;
        sizeOut = t;
        ringOut = ring;
        lxOut = (uint)cell.x;
        lzOut = (uint)cell.y;
        return true;
    }
    originXZ = 0.0; sizeOut = base;
    ringOut = lxOut = lzOut = 0;
    return false;
}

bool ResolveTerrainTileAt(float2 samplePoint, out float2 originXZ,
                          out float sizeOut, out uint ringOut,
                          out uint lxOut, out uint lzOut) {
    if ((terrainStyle & kClipmapFlag) != 0u)
        return ResolveClipmapTileAt(samplePoint, tilesX, tilesZ, tileSize,
                                    originXZ, sizeOut, ringOut, lxOut, lzOut);

    float2 lower = float2(
        ((float)originTileX - (float)tilesX * 0.5) * tileSize,
        ((float)originTileZ - (float)tilesZ * 0.5) * tileSize);
    int2 cell = int2(floor((samplePoint - lower) / tileSize));
    if (any(cell < 0) || cell.x >= (int)tilesX || cell.y >= (int)tilesZ) {
        originXZ = 0.0; sizeOut = tileSize;
        ringOut = lxOut = lzOut = 0;
        return false;
    }
    originXZ = lower + float2(cell) * tileSize;
    sizeOut = tileSize;
    ringOut = 0;
    lxOut = (uint)cell.x;
    lzOut = (uint)cell.y;
    return true;
}

bool TouchesFinerRing(uint ring, uint lx, uint lz, uint G) {
    if (ring == 0) return false;
    uint q0 = G / 4, q1 = G - q0;
    bool vertical = (lx == q0 - 1 || lx == q1) && lz >= q0 && lz < q1;
    bool horizontal = (lz == q0 - 1 || lz == q1) && lx >= q0 && lx < q1;
    return vertical || horizontal;
}

// Four cross-sections of the actual live height field estimate how much the
// 8x8 surface would deviate after each 2x reduction. Additive/replace stamps
// also contribute a conservative bound so a small crater between sample lines
// cannot disappear into a coarse tile.
float3 TerrainTileErrors(float2 originXZ, float sizeOut) {
    float3 errors = 0.0;
    [unroll]
    for (uint lineIndex = 0; lineIndex < 4; ++lineIndex) {
        float samples[9];
        [unroll]
        for (uint i = 0; i <= 8; ++i) {
            float along = (float)i * (1.0 / 8.0);
            float across = (lineIndex & 1u) != 0u ? 0.75 : 0.25;
            float2 uv = lineIndex < 2u ? float2(along, across)
                                  : float2(across, along);
            samples[i] = TerrainHeight(originXZ + uv * sizeOut);
        }
        [unroll]
        for (uint level = 1; level <= 3; ++level) {
            uint stride = 1u << level;
            [unroll]
            for (uint i = 1; i < 8; ++i) {
                if ((i % stride) == 0u) continue;
                uint lo = (i / stride) * stride;
                uint hi = min(8u, lo + stride);
                float t = (float)(i - lo) / (float)(hi - lo);
                float deviation = abs(samples[i] - lerp(samples[lo], samples[hi], t));
                errors[level - 1] = max(errors[level - 1], deviation);
            }
        }
    }

    float2 tileMin = originXZ;
    float2 tileMax = originXZ + sizeOut;
    float sculptError = 0.0;
    for (uint stampIndex = 0; stampIndex < sculptCount; ++stampIndex) {
        TerrainSculptStamp stamp = terrainSculpt[stampIndex];
        // A crater cut reaches past its radius for the ejecta lip (1.18), so it
        // needs the wider test -- culling it at the radius would leave the rim
        // tiles un-subdivided and notch the lip.
        float radiusScale = 1.0;
        if (stamp.operation == 2u) radiusScale = 1.4143;
        else if (stamp.operation == 3u) radiusScale = 1.18;
        float radius = stamp.centerRadius.z * radiusScale;
        float2 nearest = clamp(stamp.centerRadius.xy, tileMin, tileMax);
        if (length(nearest - stamp.centerRadius.xy) > radius) continue;
        float candidate;
        if (stamp.operation == 0u) candidate = abs(stamp.value);
        else if (stamp.operation == 1u) candidate = sculptMaxDisplacement;
        else if (stamp.operation == 3u)
            // The wall spans the full depth over a short distance, so the tile
            // has to carry that whole drop as its error or the cut face gets
            // tessellated into a smooth ramp and stops reading as a cut.
            candidate = abs(stamp.value) + abs(stamp.baseHeight);
        else
            candidate = abs(stamp.value) + abs(stamp.baseHeight) * stamp.replace;
        sculptError = max(sculptError, candidate);
    }
    errors = max(errors, sculptError.xxx);

    // The four lines deliberately under-sample the highest procedural octave.
    // This small analytic floor prevents diagonal fine relief from aliasing away
    // without forcing the broad, flat seabed to remain dense.
    float detailFloor = heightScale * (detailRelief != 0u ? 0.012 : 0.006);
    errors = max(errors, detailFloor * float3(0.5, 1.0, 2.0));
    return errors;
}

float MorphForRejectedLOD(float projectedError) {
    float threshold = max(0.25, lodNear);
    return 1.0 - saturate((projectedError - threshold) / (threshold * 0.5));
}

uint SelectErrorLOD(float2 originXZ, float sizeOut, uint ring,
                    uint lx, uint lz, out float morphOut) {
    float3 errors = TerrainTileErrors(originXZ, sizeOut);
    float2 center = originXZ + sizeOut * 0.5;
    float distanceToTile = max(sizeOut * 0.5, length(center - viewPos.xz));
    float pixelsPerMetre = abs(projection[1][1]) * max(1.0, lodStep) * 0.5 /
                           distanceToTile;
    float3 projected = errors * pixelsPerMetre;
    float threshold = max(0.25, lodNear);
    uint maxLOD = ((terrainStyle & kClipmapFlag) != 0u &&
                   TouchesFinerRing(ring, lx, lz, tilesX)) ? 2u : 3u;

    morphOut = 0.0;
    if (maxLOD >= 3u && projected.z <= threshold) return 3u;
    if (maxLOD >= 2u && projected.y <= threshold) {
        if (maxLOD >= 3u) morphOut = MorphForRejectedLOD(projected.z);
        return 2u;
    }
    if (projected.x <= threshold) {
        morphOut = MorphForRejectedLOD(projected.y);
        return 1u;
    }
    morphOut = MorphForRejectedLOD(projected.x);
    return 0u;
}

uint SeamExponent(float thisSpacing, float neighbourSpacing) {
    if (neighbourSpacing <= thisSpacing * 1.01) return 0u;
    uint ratio = max(1u, (uint)round(neighbourSpacing / thisSpacing));
    uint exponent = 0u;
    [unroll]
    while (exponent < 3u && (1u << exponent) < ratio) ++exponent;
    return exponent;
}

uint BuildTileTopology(float2 originXZ, float sizeOut, uint lod) {
    static const float2 directions[4] = {
        float2(1, 0), float2(-1, 0), float2(0, 1), float2(0, -1)
    };
    float2 center = originXZ + sizeOut * 0.5;
    float thisSpacing = sizeOut / (float)(8u >> lod);
    float epsilon = max(0.0001, tileSize * 0.001);
    uint packed = 0u;
    uint outerMask = 0u;
    [unroll]
    for (uint edge = 0; edge < 4; ++edge) {
        float2 neighbourOrigin;
        float neighbourSize;
        uint neighbourRing, neighbourX, neighbourZ;
        float2 samplePoint = center + directions[edge] * (sizeOut * 0.5 + epsilon);
        if (!ResolveTerrainTileAt(samplePoint, neighbourOrigin, neighbourSize,
                                  neighbourRing, neighbourX, neighbourZ)) {
            outerMask |= 1u << edge;
            continue;
        }
        float neighbourMorph;
        uint neighbourLOD = SelectErrorLOD(neighbourOrigin, neighbourSize,
                                           neighbourRing, neighbourX, neighbourZ,
                                           neighbourMorph);
        float neighbourSpacing = neighbourSize / (float)(8u >> neighbourLOD);
        packed |= SeamExponent(thisSpacing, neighbourSpacing) << (edge * 4u);
    }
    return packed | (outerMask << 16u);
}

[numthreads(32, 1, 1)]
void ASMain(uint threadID : SV_GroupThreadID, uint3 groupID : SV_GroupID) {
    if (threadID == 0) visibleCount = 0;
    GroupMemoryBarrierWithGroupSync();

    uint tileId = groupID.x * 32 + threadID;
    const bool clipmap = (terrainStyle & kClipmapFlag) != 0u;

    float2 originXZ; float tsize; bool valid = false;
    uint ring = 0, tileX = 0, tileZ = 0;
    if (clipmap) {
        valid = ResolveClipmapTile(tileId, tilesX, tilesZ, tileSize,
                                   originXZ, tsize, ring, tileX, tileZ);
    } else {
        uint tileCount = tilesX * tilesZ;
        if (tileId < tileCount) {
            uint tx = tileId % tilesX;
            uint tz = tileId / tilesX;
            originXZ = float2(
                ((float)tx + (float)originTileX - (float)tilesX * 0.5) * tileSize,
                ((float)tz + (float)originTileZ - (float)tilesZ * 0.5) * tileSize);
            tsize = tileSize;
            tileX = tx;
            tileZ = tz;
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
            const bool deploymentOverview =
                (terrainStyle & kDeploymentOverviewFlag) != 0u;
            const bool errorLOD =
                !deploymentOverview && (terrainStyle & kErrorLODFlag) != 0u;
            float lodF = deploymentOverview || errorLOD
                ? 0.0 : clamp((dist - lodNear) / lodStep, 0.0, 3.0);
            float morph = 0.0;
            uint lod = deploymentOverview ? 0u
                : (errorLOD
                    ? SelectErrorLOD(originXZ, tsize, ring, tileX, tileZ, morph)
                    : (uint)lodF);
            // Geomorph: as the tile nears the threshold to the next-coarser LOD,
            // morph rises 0->1 over the last 30% of the band so fine vertices
            // slide onto the coarse grid instead of popping.
            if (!deploymentOverview && !errorLOD)
                morph = smoothstep(0.7, 1.0, frac(lodF));

            uint slot;
            InterlockedAdd(visibleCount, 1, slot);
            payloadData.originXZ[slot] = originXZ;
            payloadData.size[slot] = tsize;
            payloadData.lod[slot] = lod;
            payloadData.morph[slot] = morph;
            payloadData.topology[slot] = errorLOD
                ? BuildTileTopology(originXZ, tsize, lod) : 0u;
        }
    }

    GroupMemoryBarrierWithGroupSync();
    DispatchMesh(visibleCount, 1, 1, payloadData);
}
