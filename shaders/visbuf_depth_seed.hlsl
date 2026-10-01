// Seeds the post-RR depth replay (VisibilityBufferDX12::BeginDepthReplay).
//
// The replay re-rasterises the visibility draws without the camera jitter so
// passes after Ray Reconstruction test against depth that matches RR's
// de-jittered colour. Terrain is most of the raster's cost (mesh shader), and
// its depth is smooth: a sub-pixel shift of it changes almost nothing but its
// silhouettes. So terrain keeps its jittered depth here, and only the other
// draws are replayed on top with the normal depth test.
//
// An object pixel the replayed object no longer covers would otherwise be left
// at the far plane, reading as sky to fog and water. It takes the nearest
// adjacent terrain depth instead, which is what the unjittered sample most
// likely hit. The one-pixel screen border keeps the jittered depth outright:
// the replay's shifted viewport does not rasterise the edge it moved away from.

Texture2D<uint2> visIds : register(t0);
Texture2D<float> jitteredDepth : register(t1);

static const uint kTerrainId = 0xFFFFFFFFu;

float4 VSMain(uint id : SV_VertexID) : SV_Position {
    const float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float PSMain(float4 position : SV_Position) : SV_Depth {
    const int2 p = int2(position.xy);
    uint width, height;
    visIds.GetDimensions(width, height);
    if (p.x == 0 || p.y == 0 || p.x >= (int)width - 1 || p.y >= (int)height - 1)
        return jitteredDepth[p];
    if (visIds[p].x == kTerrainId) return jitteredDepth[p];
    if (visIds[p].x == 0u) return 1.0;
    float depth = 1.0;
    [unroll] for (int y = -1; y <= 1; ++y) {
        [unroll] for (int x = -1; x <= 1; ++x) {
            const int2 q = p + int2(x, y);
            if (visIds[q].x == kTerrainId) depth = min(depth, jitteredDepth[q]);
        }
    }
    return depth;
}
