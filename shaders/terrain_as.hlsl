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
    float islandScale;
    uint sculptCount;
    float sculptMaxDisplacement;
};

struct TerrainPayload {
    uint tileId[32];
    uint lod[32];
};

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

        float verticalReach = heightScale + sculptMaxDisplacement;
        float3 center3 = float3(worldX + tileSize * 0.5,
            (verticalReach - skirtDepth) * 0.5,
            worldZ + tileSize * 0.5);
        float3 halfExtent = float3(tileSize * 0.5,
            (verticalReach + skirtDepth) * 0.5, tileSize * 0.5);

        if (TileIntersectsFrustum(center3, length(halfExtent))) {
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
