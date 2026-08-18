// Visibility-buffer tile classification for the split terrain resolve.
//
// The resolve runs as two dispatches -- a generic half that skips the reserved
// terrain ID and a terrain half that keeps only that ID -- so each half has a
// PSO sized for its own register pressure. Without classification both halves
// still sweep the whole screen, and every pixel one half owns is a
// load-and-return for the other. On a frame where terrain covers a third of the
// screen that is two thirds of the terrain dispatch spent doing nothing.
//
// This pass reduces the visibility buffer over the same 8x8 tiles the resolve
// dispatches in, and appends each tile's coordinate to the lists of the halves
// that actually have work in it. The halves then dispatch indirectly over their
// own list, so each is proportional to its coverage instead of to the screen.
//
// A tile straddling a terrain edge lands in both lists. That is correct rather
// than wasteful: both halves genuinely have pixels there, and the per-pixel ID
// test inside the resolve still decides each pixel individually. The lists
// overlap; the pixels they shade never do.

cbuffer TileClassifyConstants : register(b0) {
    uint screenWidth;
    uint screenHeight;
    uint tilesX;          // ceil(screenWidth  / 8)
    uint tilesY;          // ceil(screenHeight / 8)
};

Texture2D<uint2> visBuffer : register(t0);

// Tile coordinates packed as (x | y << 16). Two separate append lists rather
// than one list with a per-tile mask, because each half reads only its own and
// a shared list would make every thread group test a mask it mostly fails.
RWStructuredBuffer<uint> genericTiles : register(u0);
RWStructuredBuffer<uint> terrainTiles : register(u1);

// D3D12_DISPATCH_ARGUMENTS x2: [0..2] generic, [3..5] terrain. Written straight
// into the buffer ExecuteIndirect sources, so the counts never round-trip
// through the CPU and no readback stalls the frame.
RWByteAddressBuffer dispatchArgs : register(u2);

#define VB_TERRAIN_ID 0xFFFFFFFFu

// One group per tile, one thread per pixel in that tile -- the same 8x8 shape
// the resolve uses, so a tile here is exactly a thread group there.
groupshared uint gsHasGeneric;
groupshared uint gsHasTerrain;

// Seeds both argument records to (0, 1, 1) before classification counts into
// them. Only ThreadGroupCountX is atomically incremented below, so Y and Z must
// already be 1 -- a zeroed buffer would dispatch nothing. One thread total; run
// as its own dispatch so it is ordered before the counting pass.
[numthreads(1, 1, 1)]
void ResetArgs(uint3 threadID : SV_DispatchThreadID) {
    dispatchArgs.Store3(0,  uint3(0u, 1u, 1u));   // generic
    dispatchArgs.Store3(12, uint3(0u, 1u, 1u));   // terrain
}

[numthreads(8, 8, 1)]
void main(uint3 groupID : SV_GroupID, uint3 groupThreadID : SV_GroupThreadID,
          uint groupIndex : SV_GroupIndex) {
    if (groupIndex == 0) {
        gsHasGeneric = 0u;
        gsHasTerrain = 0u;
    }
    GroupMemoryBarrierWithGroupSync();

    const uint2 pixel = groupID.xy * 8u + groupThreadID.xy;
    if (pixel.x < screenWidth && pixel.y < screenHeight) {
        const uint id = visBuffer.Load(int3(pixel, 0)).x;
        // Background (id 0) goes to the generic half: it owns the sky path and
        // the motion-vector write for empty pixels, so a tile of pure sky still
        // needs a generic thread group.
        if (id == VB_TERRAIN_ID) gsHasTerrain = 1u;
        else                     gsHasGeneric = 1u;
    }
    GroupMemoryBarrierWithGroupSync();

    if (groupIndex != 0) return;

    const uint packed = groupID.x | (groupID.y << 16u);
    if (gsHasGeneric != 0u) {
        uint slot;
        dispatchArgs.InterlockedAdd(0, 1u, slot);
        genericTiles[slot] = packed;
    }
    if (gsHasTerrain != 0u) {
        uint slot;
        dispatchArgs.InterlockedAdd(12, 1u, slot);
        terrainTiles[slot] = packed;
    }
}
