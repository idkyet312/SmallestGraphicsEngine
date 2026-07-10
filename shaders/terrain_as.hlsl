// Terrain amplification shader: one thread per terrain tile. Frustum-culls
// each tile's AABB and picks a tessellation LOD from camera distance, then
// dispatches one mesh shader group per visible tile.

cbuffer MatrixBuffer : register(b0) {
    matrix model;
    matrix view;
    matrix projection;
    matrix lightSpaceMatrix;
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
};

struct TerrainPayload {
    uint tileId[32];
    uint lod[32];
};

groupshared TerrainPayload payloadData;
groupshared uint visibleCount;

bool TileIntersectsFrustum(float3 bbMin, float3 bbMax) {
    bool outsideLeft = true, outsideRight = true;
    bool outsideBottom = true, outsideTop = true;
    bool outsideNear = true, outsideFar = true;
    [unroll]
    for (uint i = 0; i < 8; ++i) {
        float3 p = float3(
            (i & 1) ? bbMax.x : bbMin.x,
            (i & 2) ? bbMax.y : bbMin.y,
            (i & 4) ? bbMax.z : bbMin.z);
        float4 c = mul(mul(float4(p, 1), view), projection);
        outsideLeft   = outsideLeft   && c.x < -c.w;
        outsideRight  = outsideRight  && c.x >  c.w;
        outsideBottom = outsideBottom && c.y < -c.w;
        outsideTop    = outsideTop    && c.y >  c.w;
        outsideNear   = outsideNear   && c.z < 0.0;
        outsideFar    = outsideFar    && c.z > c.w;
    }
    return !(outsideLeft || outsideRight || outsideBottom ||
             outsideTop || outsideNear || outsideFar);
}

[numthreads(32, 1, 1)]
void ASMain(uint threadID : SV_GroupThreadID, uint3 groupID : SV_GroupID) {
    if (threadID == 0) visibleCount = 0;
    GroupMemoryBarrierWithGroupSync();

    uint tileId = groupID.x * 32 + threadID;
    uint tileCount = tilesX * tilesZ;

    if (tileId < tileCount) {
        uint tx = tileId % tilesX;
        uint tz = tileId / tilesX;
        float worldX = ((float)tx - (float)tilesX * 0.5) * tileSize;
        float worldZ = ((float)tz - (float)tilesZ * 0.5) * tileSize;

        float3 bbMin = float3(worldX, -skirtDepth, worldZ);
        float3 bbMax = float3(worldX + tileSize, heightScale, worldZ + tileSize);

        if (TileIntersectsFrustum(bbMin, bbMax)) {
            float2 center = float2(worldX + tileSize * 0.5, worldZ + tileSize * 0.5);
            float dist = length(center - viewPos.xz);
            uint lod = (uint)clamp((dist - lodNear) / lodStep, 0.0, 3.0);

            uint slot;
            InterlockedAdd(visibleCount, 1, slot);
            payloadData.tileId[slot] = tileId;
            payloadData.lod[slot] = lod;
        }
    }

    GroupMemoryBarrierWithGroupSync();
    DispatchMesh(visibleCount, 1, 1, payloadData);
}
