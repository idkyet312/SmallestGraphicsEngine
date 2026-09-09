#ifndef SGE_VIRTUAL_SHADOW_SAMPLE
#define SGE_VIRTUAL_SHADOW_SAMPLE
// Sampling for the world-anchored virtual shadow clipmap.
//
// There is no cascade fallback here by design: this system replaces the cascade
// shadow maps rather than refining them, and while it is active the cascade
// pass does not render, so there is nothing to fall back to. A position with no
// resident page at any level reads as lit.
//
// Pages are keyed by absolute lattice coordinate (see VirtualShadowPages.h), so
// the lookup is a pure function of world position: nothing here depends on
// where the viewer is or which way it faces.
#define SGE_VSM_ATLAS_PAGES 4.0
#define SGE_VSM_PAGE_SIZE 1024.0
#define SGE_VSM_ATLAS_SIZE (SGE_VSM_ATLAS_PAGES * SGE_VSM_PAGE_SIZE)
#define SGE_VSM_LEVELS 3
#define SGE_VSM_COORD_BITS 14
#define SGE_VSM_COORD_BIAS 8192
#define SGE_VSM_COORD_MASK 16383u
#define SGE_VSM_TABLE_MASK 63u

// Must match VirtualShadows::Key / Hash exactly; a divergence resolves pages to
// the wrong world square.
uint VirtualShadowKey(uint level, int2 page) {
    return (level << (SGE_VSM_COORD_BITS * 2)) |
           ((uint(page.x + SGE_VSM_COORD_BIAS) & SGE_VSM_COORD_MASK)
                << SGE_VSM_COORD_BITS) |
           (uint(page.y + SGE_VSM_COORD_BIAS) & SGE_VSM_COORD_MASK);
}

uint VirtualShadowHash(uint key) {
    uint h = key * 2654435761u;     // Knuth multiplicative
    h ^= h >> 15;
    return h & SGE_VSM_TABLE_MASK;
}

// Open-addressed lookup. Returns the physical slot, or -1 when the page is not
// resident. The probe stops at the first empty entry, which terminates because
// the table is kept sparse (load factor 0.25).
int VirtualShadowSlot(VirtualShadowConstants pages, uint key) {
    uint index = VirtualShadowHash(key);
    [loop] for (uint probe = 0; probe < 64u; ++probe) {
        uint4 entry = pages.entries[(index + probe) & SGE_VSM_TABLE_MASK];
        if (entry.y == 0) return -1;            // empty: page is not resident
        if (entry.x == key) return int(entry.y - 1);
    }
    return -1;
}

// Finest level with a resident page covering this light-space position.
// Selection follows residency rather than a fixed radius, so the clipmap
// degrades to coarser pages exactly where the finer ones ran out.
bool VirtualShadowLookup(VirtualShadowConstants pages, float2 lightXY,
                         out int slot, out float2 local) {
    slot = -1;
    local = 0.0;
    [unroll] for (uint level = 0; level < SGE_VSM_LEVELS; ++level) {
        float2 cell = lightXY * pages.levelScale[level];
        int2 page = int2(floor(cell));
        int found = VirtualShadowSlot(pages, VirtualShadowKey(level, page));
        if (found >= 0) {
            slot = found;
            local = cell - float2(page);        // position inside the page
            return true;
        }
    }
    return false;
}

// One comparison tap. offsetTexels shifts the sample within the page and is
// clamped to the page interior, so a filter footprint that would leave the page
// never reads a neighbouring page's unrelated depths.
float VirtualShadowTap(Texture2DArray<float> atlas, SamplerComparisonState samp,
                       VirtualShadowConstants pages, float2 lightXY,
                       float2 offsetTexels, float depth) {
    int slot;
    float2 local;
    if (!VirtualShadowLookup(pages, lightXY, slot, local)) return 1.0;

    // The light-space lattice grows upward; rasterized texture rows grow downward.
    local.y = 1.0 - local.y;
    float2 texel = clamp(local * SGE_VSM_PAGE_SIZE + offsetTexels,
                         0.5, SGE_VSM_PAGE_SIZE - 0.5);
    float2 corner = float2(slot % 4, slot / 4) * SGE_VSM_PAGE_SIZE;
    return atlas.SampleCmpLevelZero(
        samp, float3((corner + texel) / SGE_VSM_ATLAS_SIZE, 0), depth);
}

bool VirtualShadowAvailable(VirtualShadowConstants pages) {
    return pages.config.x != 0;
}

// World position -> light space. One definition, driven by the matrix the CPU
// publishes, so the shading passes and the page rasterization cannot disagree
// about where a page is.
//
// The returned z is already the comparison depth. Every page shares one light
// rotation and one depth range, so depth is a global function of light-space z
// and does NOT vary per page -- only xy select which page to read. That is what
// lets a single sample serve whichever level turns out to be resident: the
// rotation matrix published here folds in the depth scale and bias, exactly
// matching the near/far the per-page orthographic projections are built with.
float3 VirtualShadowProject(VirtualShadowConstants pages, float3 worldPos) {
    return mul(float4(worldPos, 1.0), pages.lightRotation).xyz;
}

float VirtualShadowFilter(Texture2DArray<float> atlas, SamplerComparisonState samp,
                          VirtualShadowConstants pages, float2 lightXY,
                          float depth) {
    float result = 0;
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x)
            result += VirtualShadowTap(atlas, samp, pages, lightXY,
                                       float2(x, y) * 1.25, depth);
    return result / 9.0;
}

// The whole shadow term for one world position, for consumers to use in place
// of cascade selection.
//
// There is deliberately no cascade index and no split blending: the clipmap
// picks its own level from residency, and blending between "cascades" here
// would just sample the same page twice. The normal offset is scaled by the
// finest page's texel size, which is the tightest the filter can be while still
// clearing self-shadowing acne.
float VirtualShadowVisibility(Texture2DArray<float> atlas,
                              SamplerComparisonState samp,
                              VirtualShadowConstants pages,
                              float3 worldPos, float3 normal, float3 lightDir) {
    float ndotl = saturate(dot(normal, lightDir));
    // levelScale.x is 1 / finest page extent, so its reciprocal over the page
    // resolution is the world size of one texel at the sharpest level.
    float texelWorld = rcp(max(pages.levelScale.x, 1e-6)) / SGE_VSM_PAGE_SIZE;
    float slope = clamp(sqrt(1.0 - ndotl * ndotl) / max(ndotl, 0.1), 0.0, 8.0);
    float3 offsetPos = worldPos + normal * texelWorld * (1.0 + 2.0 * slope);

    float3 lightSpace = VirtualShadowProject(pages, offsetPos);
    // Depth is already comparable: the published transform folds in the same
    // depth range every page projection was built with.
    return VirtualShadowFilter(atlas, samp, pages, lightSpace.xy, lightSpace.z);
}
#endif
