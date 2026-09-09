#ifndef SGE_VIRTUAL_SHADOW_TYPES
#define SGE_VIRTUAL_SHADOW_TYPES
// Mirrors VirtualShadows::Constants (src/render/VirtualShadowPages.h), which
// carries static_asserts pinning every offset below. Keep the two in step.
struct VirtualShadowConstants {
    // x = active, y = resident page count, z = table size, w = page size
    uint4 config;
    // 1 / PageExtent(level), per level in xyz
    float4 levelScale;
    // Light rotation. Published rather than recomputed per shader so the basis
    // has exactly one definition and cannot drift between C++ and HLSL.
    float4x4 lightRotation;
    // Open-addressed page table: x = key, y = slot + 1 (0 = empty).
    uint4 entries[64];
};
#endif
