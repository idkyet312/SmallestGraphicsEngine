#pragma once

// The single source of truth for the island's building ground level.
//
// The terrain is an island: its surface is lifted above sea level (y = 0), and a
// flat pad is stamped under the houses. Everything that sits ON that pad -- the
// wall geometry, the door openings, the roofs -- has to be built at this height,
// or it ends up buried in the sand or floating over it.
//
// This value must stay in lockstep with kPadHeight in shaders/terrain_ms.hlsl and
// its CPU mirror in TerrainRendererDX12::HeightAt. Those two cannot include this
// header (one is HLSL, the other is the definition of the terrain itself), so they
// carry the literal with a comment pointing back here.
//
// It exists because this number was previously written out by hand in several
// places, and raising the island left the roofs and door headers behind at the old
// ground level.
namespace Ground {
    inline constexpr float kBuildingPadY = 2.5f;
}
