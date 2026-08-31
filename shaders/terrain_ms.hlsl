// Terrain mesh shader: one group per visible tile. Generates an (n+1)x(n+1)
// heightfield grid where n = 8 >> LOD quads. The opt-in error LOD collapses
// fine boundary vertices onto a coarser neighbour's grid; the legacy path keeps
// its full skirt ring byte-for-byte, while stitched tiles retain skirts only at
// the terrain's exposed outer edge.

cbuffer MatrixBuffer : register(b0) {
    matrix model;
    matrix view;
    matrix projection;
    matrix lightSpaceMatrix;
};

#include "terrain_height.hlsli"

struct TerrainPayload {
    float2 originXZ[32];   // world min-corner of the tile
    float  size[32];       // tile world size (metres)
    uint   lod[32];
    float  morph[32];      // 0..1 blend toward the next-coarser LOD (geomorph)
    uint   topology[32];   // seam exponents + exposed outer-edge mask
};

struct OutVertex {
    float4 position : SV_Position;
    float3 fragPos : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 texCoord : TEXCOORD2;
    float4 tangent : TEXCOORD3;
    float4 fragPosLightSpace : TEXCOORD4;
};

// k-th vertex along the tile perimeter, counterclockwise, k in [0, 4n).
uint2 PerimeterCoord(uint k, uint n) {
    if (k < n)     return uint2(k, 0);
    if (k < 2 * n) return uint2(n, k - n);
    if (k < 3 * n) return uint2(n - (k - 2 * n), n);
    return uint2(0, n - (k - 3 * n));
}

uint SelectedOuterEdge(uint mask, uint ordinal) {
    [unroll]
    for (uint edge = 0; edge < 4; ++edge) {
        if ((mask & (1u << edge)) == 0u) continue;
        if (ordinal == 0u) return edge;
        --ordinal;
    }
    return 0u;
}

// Edge order matches terrain_as.hlsl: right, left, up, down.
uint2 EdgeCoord(uint edge, uint k, uint n) {
    if (edge == 0u) return uint2(n, k);
    if (edge == 1u) return uint2(0, n - k);
    if (edge == 2u) return uint2(n - k, n);
    return uint2(k, 0);
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
            out vertices OutVertex verts[117],
            out indices uint3 tris[192]) {
    float2 tileOrigin = payloadData.originXZ[groupID.x];
    float thisTileSize = payloadData.size[groupID.x];
    uint lod = min(payloadData.lod[groupID.x], 3u);
    float morph = payloadData.morph[groupID.x];
    uint topology = payloadData.topology[groupID.x];
    const bool stitched = (terrainStyle & 16u) != 0u;
    uint outerMask = stitched ? ((topology >> 16u) & 0xFu) : 0xFu;
    uint n = 8u >> lod; // quads per side: 8/4/2/1

    uint side = n + 1;
    uint gridVerts = side * side;
    uint outerEdgeCount = stitched ? countbits(outerMask) : 4u;
    uint skirtVerts = stitched ? outerEdgeCount * (n + 1u) : 4u * n;
    uint gridTris = 2 * n * n;
    uint skirtTris = outerEdgeCount * 2u * n;
    SetMeshOutputCounts(gridVerts + skirtVerts, gridTris + skirtTris);

    float quadSize = thisTileSize / (float)n;

    for (uint vi = id.x; vi < gridVerts + skirtVerts; vi += 128) {
        float2 xz;
        float y;
        if (vi < gridVerts) {
            uint gx = vi % side;
            uint gz = vi / side;
            if (stitched) {
                uint exponent = 0u;
                if (gx == n) exponent = (topology >> 0u) & 0xFu;
                else if (gx == 0u) exponent = (topology >> 4u) & 0xFu;
                else if (gz == n) exponent = (topology >> 8u) & 0xFu;
                else if (gz == 0u) exponent = (topology >> 12u) & 0xFu;
                if (exponent > 0u) {
                    uint step = 1u << min(exponent, 3u);
                    if (gx == 0u || gx == n) gz -= gz % step;
                    else gx -= gx % step;
                }
            }
            // Geomorph toward the next-coarser grid: blend each vertex's index
            // toward the even (coarse) index by 'morph', so odd vertices slide
            // onto the coarse grid as the tile approaches its LOD switch. Avoids
            // popping. Even indices are unchanged (morph target == self).
            float2 fine = float2(gx, gz);
            float2 coarse = floor(fine * 0.5) * 2.0;
            // Boundary vertices are governed by the seam ratio. Letting each
            // tile morph its edge independently would briefly separate two
            // otherwise stitched grids because their error bands differ.
            bool boundary = gx == 0u || gx == n || gz == 0u || gz == n;
            float vertexMorph = stitched && boundary ? 0.0 : morph;
            float2 morphed = lerp(fine, coarse, vertexMorph);
            xz = tileOrigin + morphed * quadSize;
            y = TerrainHeight(xz);
        } else {
            uint skirtIndex = vi - gridVerts;
            uint2 pc;
            if (stitched) {
                uint edgeOrdinal = skirtIndex / (n + 1u);
                uint edgeVertex = skirtIndex % (n + 1u);
                pc = EdgeCoord(SelectedOuterEdge(outerMask, edgeOrdinal),
                               edgeVertex, n);
            } else {
                pc = PerimeterCoord(skirtIndex, n);
            }
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
            uint skirtTriangle = pi - gridTris;
            uint skirtQuad = skirtTriangle / 2u;
            uint2 pcA, pcB;
            uint botA, botB;
            if (stitched) {
                uint edgeOrdinal = skirtQuad / n;
                uint edgeSegment = skirtQuad % n;
                uint edge = SelectedOuterEdge(outerMask, edgeOrdinal);
                pcA = EdgeCoord(edge, edgeSegment, n);
                pcB = EdgeCoord(edge, edgeSegment + 1u, n);
                botA = gridVerts + edgeOrdinal * (n + 1u) + edgeSegment;
                botB = botA + 1u;
            } else {
                uint kA = skirtQuad;
                uint kB = (skirtQuad + 1u) % (4u * n);
                pcA = PerimeterCoord(kA, n);
                pcB = PerimeterCoord(kB, n);
                botA = gridVerts + kA;
                botB = gridVerts + kB;
            }
            uint topA = pcA.y * side + pcA.x;
            uint topB = pcB.y * side + pcB.x;
            tris[pi] = ((pi - gridTris) & 1)
                ? uint3(topB, botA, botB)
                : uint3(topA, botA, topB);
        }
    }
}
