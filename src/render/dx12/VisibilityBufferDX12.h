#ifndef VISIBILITY_BUFFER_DX12_H
#define VISIBILITY_BUFFER_DX12_H

#include "ShaderCacheDX12.h"
#include "DX12Core.h"
#include "ShaderDX12.h"
#include "BindlessHeapDeviceDX12.h"
#include "SceneGraph.h"
#include "ProfilerDX12.h"
#include "TerrainScreenDisplacementDX12.h"
#include "VisibilityGeometryPool.h"
#include "DLSSDX12.h"
#include "BootTimer.h"
#include "RadianceCascadesDX12.h"
#include "VariableRateGIDX12.h"
#include "GIRadianceCacheDX12.h"
#include <DirectXPackedVector.h>

// Scales VisibilityBufferDX12::exposure. A global like g_emissiveIntensity, so
// the time-of-day code (compiled before the renderer object) can set it: auto
// exposure normalises to mid grey, so brighter lights alone cannot brighten a
// level, only this can.
inline float g_exposureScale = 1.0f;
#include <stb_image.h>
#include <algorithm>
#include <memory>
#include <cassert>
#include <fstream>
#include <future>
#include <optional>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// Defined in main.cpp. Same pattern as ForwardRenderer.h / VBDrawRenderer.h:
// the SVGF passes time themselves, and this header is included before the
// definition.
extern ProfilerDX12 g_profiler;

// Instance IDs occupy a full uint in the visibility target. Keep this aligned
// with ShaderDX12's per-frame matrix capacity.
static const UINT VB_MAX_DRAW_CALLS = MAX_DRAW_CALLS_PER_FRAME;
// Persistent imported geometry now exceeds the original 1M-vertex budget on
// the airport levels. Keep enough headroom for their props and destruction
// batches so registration cannot fall through to a missing visibility draw.
static const UINT VB_MAX_VERTICES = 2 * 1024 * 1024;
// Maximum total indices across all draw calls
static const UINT VB_MAX_INDICES = VB_MAX_VERTICES * 3;
static const UINT VB_MAX_TRIANGLES = VB_MAX_INDICES / 3;
static const UINT VB_CLUSTER_X = 16;
static const UINT VB_CLUSTER_Y = 9;
static const UINT VB_CLUSTER_Z = 10;
static const UINT VB_CLUSTER_COUNT = VB_CLUSTER_X * VB_CLUSTER_Y * VB_CLUSTER_Z;
static const UINT VB_MAX_LIGHTS_PER_CLUSTER = 32;
// Raised from 256 once bindless removed the per-material descriptor-table cost.
// The record is 96 bytes, so 4096 records is a 384 KB buffer -- cheap next to
// silently dropping materials past the limit. VBMaterialData's shader-side
// layout is unchanged, so the default FXC resolve is unaffected by this.
static const UINT VB_MAX_MATERIALS = 4096;
// What the *legacy* resolve can actually address. Its shader clamps the fetch
// to 255 (widening that clamp would change the default variant's DXBC, which
// the resolve canary test pins), so registering beyond this would not raise the
// ceiling -- it would silently shade materials 256+ as material 255. Bindless
// uses the full VB_MAX_MATERIALS.
static const UINT VB_MAX_LEGACY_MATERIALS = 256;
// Legacy tier only: the fixed-size t8..t71 texture table the FXC resolve reads.
// Bindless materials index the bindless heap directly and are not bound by this.
static const UINT VB_MAX_MATERIAL_TEXTURES = 64;
static const UINT VB_BLOOM_MAX_MIPS = 6;

// Must match the compute shader's DrawCallData
struct VBDrawCallData {
    XMFLOAT4X4 modelMatrix;
    XMFLOAT4X4 previousModelMatrix;
    XMFLOAT3   objectColor;
    float      useTexture;
    float      metalness;
    float      roughness;
    float      useNormalMap;
    UINT       materialID;
    UINT       vertexOffset;
    UINT       indexOffset;
    UINT       indexCount;
    UINT       hasIndices;
    UINT       flags; // bit 0 double-sided, bit 1 alpha cutout, bit 2 luminance cutout
    XMFLOAT4   palmWindRoot;
};

struct VBMeshData {
    UINT vertexOffset = 0;
    UINT vertexCount = 0;
    UINT indexOffset = 0;
    UINT indexCount = 0;
    UINT hasIndices = 0;
    UINT stableTriangleOffset = 0;
    UINT stableTriangleNamespace = 0;
    // Allocated span, which is >= the used *Count when the mesh reuses a
    // recycled range. Release returns the capacity, not the smaller live count,
    // so repeated reuse cannot shrink a range toward zero. CPU-side only --
    // the GPU addresses geometry through the offsets above.
    UINT vertexCapacity = 0;
    UINT indexCapacity = 0;
    UINT triangleCapacity = 0;
};

struct VBClusterData {
    UINT lightCount = 0;
    UINT lightIndices[VB_MAX_LIGHTS_PER_CLUSTER] = {};
    UINT padding[3] = {};
};

struct VBMaterialData {
    XMFLOAT4 baseColorFactor = { 1, 1, 1, 1 };
    XMFLOAT4 emissiveOcclusion = { 0, 0, 0, 0 };
    XMFLOAT4 pbrParams = { 0, 0.5f, 1, 0 };
    XMFLOAT4 shadingParams = { 1, 0, 0.7f, 0 };
    UINT textureIndices[4] = { UINT_MAX, UINT_MAX, UINT_MAX, 0 };
};

static const UINT VB_INVALID_MESH = UINT_MAX;

// Must match the compute shader's PackedVertex (two float4s).
struct VBPackedVertex {
    XMFLOAT4 d0; // pos.xyz, normal.x
    XMFLOAT4 d1; // normal.yz, uv.xy
};

// Must match FrameConstants in compute shader (256-byte aligned)
struct alignas(256) VBFrameConstants {
    XMMATRIX viewMatrix;
    XMMATRIX projMatrix;
    XMMATRIX invViewProj;
    XMMATRIX shadowCascadeMatrices[SHADOW_CASCADE_COUNT];
    XMMATRIX previousViewProj;
    XMFLOAT4 shadowCascadeSplits;
    XMFLOAT3 cameraPos;
    float    screenWidth;
    float    screenHeight;
    float    nearPlane;
    float    farPlane;
    UINT     debugViewMode;
    UINT     enableMotionVectors;
    UINT     edgeAAEnabled;
    float    contactShadowStrength;
    float    contactShadowMaxDistance;
    UINT     contactShadowLinearDepth;
    UINT     contactShadowNoiseFrame;
    UINT     bentNormalGTAOEnabled;
    UINT     bentNormalGTAOFlags;
    XMFLOAT4 palmWind;
    XMFLOAT4 palmPrimary;
    XMFLOAT4 palmSecondary;
    XMFLOAT4 palmPreviousPrimary;
    XMFLOAT4 palmPreviousSecondary;
    XMFLOAT4 palmParams;
    // Terrain-in-visibility parameters. Appended after every pre-existing field
    // so the default resolve's cbuffer layout is untouched -- that shader only
    // declares these fields when SGE_TERRAIN_VISIBILITY is defined, and its
    // DXBC is pinned byte-for-byte by ResolveShaderCanaryTests.
    float    terrainMaterialType;
    float    terrainNormalYSign;
    UINT     terrainVisibilityEnabled;
    // Was terrainPadding. Non-zero only when the level supplied a splatmap, so
    // a level without one costs a branch rather than a texture fetch.
    UINT     terrainSplatEnabled;
    // Terrain rasterizes with the projection the forward extensions pass uses
    // (unjittered unless extensionMotionVectors is on), because they share the
    // depth buffer. Its world position must therefore be reconstructed with the
    // matching inverse, not the jittered invViewProj the draw-call path needs.
    XMMATRIX terrainInvViewProj;
    // 1 / full island extent per axis, mapping world XZ to splat UV. Appended
    // after the matrix so its 16-byte alignment is preserved.
    XMFLOAT2 terrainSplatInvExtent;
    // Reuses half of the splat padding; following VSM constants keep their offset.
    UINT     terrainPOMEnabled;
    UINT     terrainNeutralHeightBlendMask;
    VirtualShadows::Constants virtualShadows;
};
static_assert(offsetof(VBFrameConstants, terrainPOMEnabled) ==
              offsetof(VBFrameConstants, terrainSplatInvExtent) + sizeof(XMFLOAT2),
              "Terrain POM must occupy the existing splat padding");

struct alignas(256) VBPostConstants {
    UINT outputWidth;
    UINT outputHeight;
    float exposure;
    float bloomStrength;
    float vignetteStrength;
    float grainStrength;
    UINT frameIndex;
    UINT historyValid;
    float taaFeedback;
    float motionBlurStrength;
    float focusDistance;
    float aperture;
    float nearPlane;
    float farPlane;
    UINT debugViewMode;
    UINT validationMode;
    UINT surfaceHistoryValid;
    UINT historyDebugView;
    UINT surfaceIdentityEnabled;
    float lensDirtStrength;
    float lensDirtScale;
    float chromaticAberration;
    float lensFlareStrength;
    // HLSL starts a float4 on a 16-byte boundary. The scalars above total 92
    // bytes, so the shader pads to 96 before sunLensPosition -- without this
    // the C++ side packs it at 92 and every sun value the flare reads is
    // shifted one float, which renders as no flare at all rather than as an
    // obviously wrong one. The static_asserts below pin both offsets.
    float sunLensPadding;
    // xy = projected sun UV (may fall outside 0..1), z = continuous presence
    // that ramps to zero across a margin beyond the frame edge,
    // w = elevation-scaled directional-light intensity.
    XMFLOAT4 sunLensPosition;
    XMFLOAT4 sunLensColor;
};

static_assert(offsetof(VBPostConstants, sunLensPosition) == 96,
              "sunLensPosition must match the HLSL float4 boundary at 96");
static_assert(offsetof(VBPostConstants, sunLensColor) == 112,
              "sunLensColor must match the HLSL float4 boundary at 112");

struct alignas(256) VBExposureConstants {
    UINT inputWidth;
    UINT inputHeight;
    float adaptationRate;
    float middleGray;
};

class VisibilityBufferDX12 {
public:
    UINT geometryVertexCapacity = VB_MAX_VERTICES;
    UINT geometryIndexCapacity = VB_MAX_INDICES;
    UINT geometryTriangleCapacity = VB_MAX_TRIANGLES;
    UINT geometryGeneration = 0;
    bool texturedEmissionEnabled = false;
    // Descriptor-table sizes include the final t92 spot shadow atlas slot.
    // Keep allocation and copy counts tied to these values: omitting the last
    // descriptor makes bindless sample an uninitialized heap entry.
    static constexpr UINT kResolveDescriptorCount = 92;
    static constexpr UINT kEnhancedResolveDescriptorCount = 108;
    // Root UAV u16 (Lumen radiance cache) on the enhanced resolve root
    // signatures, after the CBV, the table and the t90 root SRV.
    static constexpr UINT kEnhancedRadianceCacheRootParameter = 3;
    // Root UAVs u17/u18 (Lumen ReSTIR reservoirs), right after the cache.
    static constexpr UINT kEnhancedReSTIRRootParameter = 4;
    // Root SRV t100 (emissive triangle lights) and root UAV u19 (their ReSTIR
    // DI reservoirs), after the ReSTIR GI pair. emissive_restir.hlsli.
    static constexpr UINT kEnhancedEmissiveRootParameter = 6;
    // Heap index of the spot shadow atlas (t92). Sits one past the terrain
    // splatmap, which was the previous last slot.
    static constexpr UINT kSpotShadowAtlasSlot = 91;
    // Set once at startup from ShadowMapDX12's atlas; the resource is created
    // there and lives for the process, so this never needs re-pointing.
    ID3D12Resource* spotShadowAtlasResource = nullptr;

    // Full-width instance and primitive IDs (R32G32_UINT).
    ComPtr<ID3D12Resource> visBufferRT;
    bool surfaceHistoryValid = false;
    // Use authored namespace/triangle keys for temporal validity instead of
    // the depth heuristic. The fallback stays for the frame after invalidation.
    bool surfaceIDTemporalEnabled = true;
    // Debug view colouring pixels by why history was kept or rejected.
    bool historyDebugView = false;
    // Edge AA: shade 2 sub-pixel samples on silhouette edges and average.
    // Off by default; interior pixels are unchanged.
    bool edgeAAEnabled = false;
    // Write motion vectors from forward extension passes (bandits, guns,
    // impact billboards) into motionTexture so temporal consumers (TAA, SVGF)
    // can reproject them. Off by default; takes a second RTV and a PSO
    // variant with SGE_EXTENSION_MOTION compiled in.
    bool extensionMotionVectors = false;
    // GTAO can request primary-surface motion without enabling cinematic TAA.
    bool aoTemporalMotionVectors = false;
    // ---- Terrain in the visibility buffer ----
    // Manual toggle, off by default. When on AND the terrain-enabled resolve
    // PSO built AND terrain bound its texture arrays, terrain rasterizes IDs
    // into the visibility buffer and is shaded in the resolve; otherwise it
    // stays on the forward path, which remains the parity reference.
    // On by default: the resolve shades terrain more cheaply than the forward
    // pass redraws it, and TerrainVisibilityReady still gates every hard
    // prerequisite, so an unsupported setup falls back to forward on its own.
    bool terrainVisibilityRequested = true;
    // Set per frame by the renderer once terrain has actually rasterized into
    // the visibility buffer, so the resolve only takes the terrain branch when
    // there are terrain IDs to decode.
    bool terrainVisibilityActiveThisFrame = false;
    // Destruction chunks are registered into the visibility buffer regardless;
    // this decides whether the forward extensions pass still redraws them. On
    // by default -- that redraw was measured at 6.9 ms, 84% of the pass.
    bool destructionVisibilityRequested = true;
    // Mirrors the forward terrain material parameters so the resolve reproduces
    // the same layer weights (built-in footpaths) and normal-map handedness.
    float terrainMaterialType = 0.0f;
    float terrainNormalYSign = 1.0f;
    bool terrainPOM = false;
    TerrainScreenDisplacementDX12 terrainScreenDisplacement;
    bool terrainScreenDisplacementPrepared = false;
    bool terrainScreenDisplacementActiveThisFrame = false;
    bool terrainScreenDisplacementWasActive = false;
    float terrainScreenDisplacementStrength = 1.0f;
    UINT terrainScreenDisplacementSteps = 24;
    UINT terrainNeutralHeightBlendMask = 0;
    // Non-owning terrain layer arrays, supplied by TerrainRendererDX12.
    ID3D12Resource* terrainAlbedoArray = nullptr;
    ID3D12Resource* terrainNormalArray = nullptr;
    ID3D12Resource* terrainMetalRoughArray = nullptr;
    // Per-level painted layer weights, also owned by TerrainRendererDX12. Null
    // on every level that has no sidecar, which is the common case; the t91
    // slot then holds a null SRV and the shader takes the procedural path.
    ID3D12Resource* terrainSplatMap = nullptr;
    // Half the island's full XZ extent per axis, from the terrain params. The
    // shader needs the reciprocal; it is computed at upload rather than here so
    // a zero scale cannot divide by zero mid-frame.
    float terrainSplatExtentX = 0.0f;
    float terrainSplatExtentZ = 0.0f;
    bool terrainDescriptorsWritten = false;
    // Projection terrain rasterized with this frame, and whether it was set.
    // Invalid on any frame terrain did not draw, in which case Resolve falls
    // back to the ordinary inverse.
    XMMATRIX terrainProjection = XMMatrixIdentity();
    bool terrainProjectionValid = false;
    // Non-owning previous-frame GTAO history. ScreenSpaceAODX12 retains the
    // resource and supplies it before visibility resolve.
    ID3D12Resource* bentNormalGTAOHistory = nullptr;
    bool bentNormalGTAORequested = false;
    bool bentNormalGTAOHistoryValid = false;
    bool bentNormalGTAOAppliedLastResolve = false;
    enum class BentNormalGTAODebugMode : UINT {
        Lit = 0,
        Direction = 1,
        Visibility = 2,
        TemporalConfidence = 3
    };
    BentNormalGTAODebugMode bentNormalGTAODebugMode =
        BentNormalGTAODebugMode::Lit;
    ComPtr<ID3D12DescriptorHeap> visRtvHeap;    // RTV for visibility pass
    ComPtr<ID3D12DescriptorHeap> visSrvUavHeap; // SRV/UAV for compute resolve
    // Throwaway ID target for the post-RR depth replay (see BeginDepthReplay).
    // Created on first use, so it costs nothing unless RR runs.
    ComPtr<ID3D12Resource> depthReplayRT;
    ComPtr<ID3D12DescriptorHeap> depthReplayRtvHeap;
    // Fullscreen pass that seeds the replay with terrain's jittered depth, so
    // the (mesh-shader, most expensive) terrain draw is not replayed.
    ComPtr<ID3D12RootSignature> depthSeedRootSig;
    ComPtr<ID3D12PipelineState> depthSeedPSO;
    ComPtr<ID3D12DescriptorHeap> depthSeedHeap;
    bool depthSeedTried = false;
    // Upscaling RR: guides and motion for forward-drawn pixels; see
    // rr_forward_guides_cs.hlsl.
    ComPtr<ID3D12RootSignature> rrForwardGuidesRootSig;
    ComPtr<ID3D12PipelineState> rrForwardGuidesPSO;
    ComPtr<ID3D12DescriptorHeap> rrForwardGuidesHeap;
    bool rrForwardGuidesTried = false;
    // Motion-vector debug overlay (motion_debug_cs.hlsl). 0 off, 1 over the
    // scene, 2 motion only. F7 cycles it; SGE_MOTION_DEBUG sets it at start.
    UINT motionDebugMode = 0;
    ComPtr<ID3D12RootSignature> motionDebugRootSig;
    ComPtr<ID3D12PipelineState> motionDebugPSO;
    ComPtr<ID3D12DescriptorHeap> motionDebugHeap;
    bool motionDebugTried = false;

    // Jitter debug (shown with the motion overlay). Eight motion pixels are
    // copied right before DLSS / RR evaluates -- the vectors it actually
    // received -- and read back once the GPU has finished that frame.
    static constexpr UINT kJitterProbeCount = 8;
    struct JitterProbeFrame {
        bool valid = false;
        UINT64 frame = 0;
        UINT renderWidth = 0, renderHeight = 0;
        XMFLOAT2 jitterCurrent = { 0.0f, 0.0f };   // render px
        XMFLOAT2 jitterPrevious = { 0.0f, 0.0f };  // in the previous VP
        XMFLOAT2 resolveStripUV = { 0.0f, 0.0f };  // subtracted by the resolve
        bool previousVPFresh = true;
        bool rr = false, rrUpscale = false, forwardGuides = false;
        UINT mvMode = 0;
        DLSS::EvalDebug dlss;
        XMFLOAT2 probeUV[kJitterProbeCount] = {};    // where (0..1)
        XMFLOAT2 motionUV[kJitterProbeCount] = {};   // what was read
    };
    static constexpr float kJitterProbePositions[kJitterProbeCount][2] = {
        { 0.50f, 0.50f }, { 0.86f, 0.33f }, { 0.12f, 0.30f }, { 0.30f, 0.78f },
        { 0.50f, 0.42f }, { 0.50f, 0.05f }, { 0.60f, 0.85f }, { 0.75f, 0.62f } };
    ComPtr<ID3D12Resource> jitterProbeReadback[FRAME_COUNT];
    JitterProbeFrame jitterProbePending[FRAME_COUNT];
    JitterProbeFrame jitterProbeLatest;
    XMFLOAT2 lastResolveStripUV = { 0.0f, 0.0f };
    float mipBiasApplied = 0.0f;

    // Depth buffer SRV for the compute pass (reads main depth)
    // We'll create a SRV for the engine's existing depth buffer

    // Lighting stays HDR until the dedicated cinematic post pass.
    ComPtr<ID3D12Resource> outputTexture;
    ComPtr<ID3D12Resource> dlssUpscaledTexture;
    ComPtr<ID3D12DescriptorHeap> outputRtvHeap;
    ComPtr<ID3D12Resource> presentTexture;
    ComPtr<ID3D12Resource> motionTexture;
    ComPtr<ID3D12DescriptorHeap> motionRtvHeap;
    ComPtr<ID3D12Resource> normalRoughnessTexture;
    ComPtr<ID3D12Resource> rrDiffuseAlbedo;
    ComPtr<ID3D12Resource> rrSpecularAlbedo;
    ComPtr<ID3D12Resource> rrSpecularHitDistance;
    bool rayReconstructionActive = false;
    // RR runs as the upscaler at the end of the HDR scene rather than native
    // mid-frame; selects the jittered motion-vector convention.
    bool rayReconstructionUpscaleActive = false;
    // Settings toggles for PrepareRRForwardGuides / PrepareSRForwardMotion.
    bool rrForwardGuidesEnabled = true;
    bool srForwardMotionEnabled = true;
    ComPtr<ID3D12Resource> bloomTexture;
    // Two half-res targets: the flare passes ping-pong between them, because a
    // single texture cannot be bound as SRV and UAV in the same dispatch.
    ComPtr<ID3D12Resource> flareTexture;
    ComPtr<ID3D12Resource> flareScratch;
    // Depth immediately after visibility resolve. Forward extensions can then
    // be detected and rejected from history when they lack motion vectors.
    ComPtr<ID3D12Resource> visibilityDepthTexture;
    ComPtr<ID3D12Resource> historyTextures[2];
    ComPtr<ID3D12Resource> exposureState;
    ComPtr<ID3D12Resource> colorLUT;
    ComPtr<ID3D12Resource> colorLUTUpload;
    ComPtr<ID3D12Resource> lensDirtTexture;
    ComPtr<ID3D12Resource> lensDirtUpload;
    ComPtr<ID3D12Resource> sunFlareTexture;
    ComPtr<ID3D12Resource> sunFlareUpload;
    ComPtr<ID3D12Resource> lensGhostTexture;
    ComPtr<ID3D12Resource> lensGhostUpload;

    // Visibility pass PSO + root signature
    ComPtr<ID3D12RootSignature> visPassRootSig;
    ComPtr<ID3D12PipelineState> visPassPSO;
    ComPtr<ID3D12PipelineState> visPassDoubleSidedPSO;
    ComPtr<ID3D12PipelineState> visPassAlphaPSO;
    ComPtr<ID3D12PipelineState> visPassAlphaDoubleSidedPSO;
    ComPtr<ID3D12RootSignature> bindlessVisPassRootSig;
    ComPtr<ID3D12PipelineState> bindlessVisPassPSO;
    ComPtr<ID3D12PipelineState> bindlessVisPassDoubleSidedPSO;
    ComPtr<ID3D12PipelineState> bindlessVisPassAlphaPSO;
    ComPtr<ID3D12PipelineState> bindlessVisPassAlphaDoubleSidedPSO;
    bool bindlessVisPassReady = false;

    // Compute resolve PSO + root signature
    ComPtr<ID3D12RootSignature> resolveRootSig;
    ComPtr<ID3D12PipelineState> resolvePSO;
    ComPtr<ID3D12PipelineState> virtualShadowResolvePSO;
    // Terrain-enabled twin of resolvePSO: the same source compiled by the same
    // compiler with SGE_TERRAIN_VISIBILITY defined, sharing resolveRootSig. A
    // separate PSO rather than a branch in the default shader, so the default
    // FXC output stays byte-for-byte identical (ResolveShaderCanaryTests).
    ComPtr<ID3D12PipelineState> terrainResolvePSO;
    // Terrain twins of the other three resolve tiers. Each shares its tier's
    // root signature and descriptor heap and differs only by the added
    // SGE_TERRAIN_VISIBILITY define, so terrain works on every path the frame
    // might actually select rather than only the FXC default.
    ComPtr<ID3D12PipelineState> enhancedTerrainResolvePSO;
    ComPtr<ID3D12PipelineState> bindlessTerrainResolvePSO;
    ComPtr<ID3D12PipelineState> bindlessEnhancedTerrainResolvePSO;

    // Terrain-only half of the split resolve, one per tier. Compiled from the
    // same source with SGE_TERRAIN_ONLY_RESOLVE added, so it shades the
    // reserved terrain ID and returns on everything else -- the mirror image of
    // the PSOs above, which now skip that ID.
    //
    // Two dispatches instead of one because register allocation is per-PSO: a
    // combined shader is allocated for the triplanar path even on pixels that
    // never run it, which costs occupancy across the whole screen. Each half
    // shares its tier's root signature and heap, so the split adds a dispatch
    // and no bindings.
    //
    // Null is a supported state: TerrainVisibilityReady() requires both halves,
    // so a tier missing either one keeps terrain on the forward path rather
    // than rasterizing IDs nothing will shade.
    ComPtr<ID3D12PipelineState> terrainOnlyResolvePSO;
    ComPtr<ID3D12PipelineState> enhancedTerrainOnlyResolvePSO;
    ComPtr<ID3D12PipelineState> bindlessTerrainOnlyResolvePSO;
    ComPtr<ID3D12PipelineState> bindlessEnhancedTerrainOnlyResolvePSO;

    // Tile-classified twins of the two split halves, compiled with
    // SGE_RESOLVE_TILE_LIST so SV_GroupID indexes a tile list instead of naming
    // a screen position. Only the split halves get classified variants: the
    // unsplit resolve (terrain off) has nothing to classify, and leaving its
    // PSOs alone keeps the default path exactly as it was.
    //
    // Null when classification is unavailable for that tier, in which case the
    // split still runs from the full-screen PSOs above.
    ComPtr<ID3D12PipelineState> terrainResolveTiledPSO;
    ComPtr<ID3D12PipelineState> enhancedTerrainResolveTiledPSO;
    ComPtr<ID3D12PipelineState> bindlessTerrainResolveTiledPSO;
    ComPtr<ID3D12PipelineState> bindlessEnhancedTerrainResolveTiledPSO;
    ComPtr<ID3D12PipelineState> terrainOnlyResolveTiledPSO;
    ComPtr<ID3D12PipelineState> enhancedTerrainOnlyResolveTiledPSO;
    ComPtr<ID3D12PipelineState> bindlessTerrainOnlyResolveTiledPSO;
    ComPtr<ID3D12PipelineState> bindlessEnhancedTerrainOnlyResolveTiledPSO;

    // Enhanced-visuals resolve: same shader source compiled at cs_6_5 with
    // SGE_ENHANCED_VISUALS, adding inline RayQuery. Null when unavailable
    // (no DXC, no Tier 1.1, or a compile failure), which is the signal the
    // enhanced tier cannot be enabled.
    ComPtr<ID3D12RootSignature> enhancedResolveRootSig;
    ComPtr<ID3D12PipelineState> enhancedResolvePSO;

    // Bindless resolve variants: the same two shaders again with
    // SGE_BINDLESS_MATERIALS, at cs_6_6 (ResourceDescriptorHeap needs 6.6).
    // Their root signatures carry CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED and keep
    // every existing root parameter number, so only the material texture access
    // differs. Null when DXC, SM 6.6 or Tier 3 is missing -- which is exactly
    // the condition that keeps the bindless toggle unavailable.
    ComPtr<ID3D12RootSignature> bindlessResolveRootSig;
    ComPtr<ID3D12PipelineState> bindlessResolvePSO;
    ComPtr<ID3D12RootSignature> bindlessEnhancedResolveRootSig;
    ComPtr<ID3D12PipelineState> bindlessEnhancedResolvePSO;
    bool bindlessResolveReady = false;
    bool bindlessEnhancedResolveReady = false;
    // Not owned. Supplied by the renderer before Init so the resolve pipelines
    // can be gated on real adapter support rather than built and then found
    // unusable.
    BindlessHeapDX12* bindlessHeap = nullptr;
    // Per-pixel record of which pixels were routed to RT (u3), plus the
    // readback used to report the ray fraction to the UI.
    ComPtr<ID3D12Resource> rayMaskTexture;
    ComPtr<ID3D12Resource> rayMaskReadback;
    ComPtr<ID3D12Resource> enhancedConstantBuffer;
    void* enhancedConstantMapped = nullptr;
    bool enhancedPipelineReady = false;
    // Mirrors the default compute heap and appends TLAS / ray mask / enhanced
    // constants, so the default layout's hardcoded slot indices stay valid.
    ComPtr<ID3D12DescriptorHeap> enhancedComputeDescHeaps[FRAME_COUNT];
    // The history resource ping-pongs every frame. A heap per frame slot keeps
    // t86 updates away from descriptors an earlier frame may still consume.
    ComPtr<ID3D12DescriptorHeap> bentNormalComputeDescHeaps[FRAME_COUNT];
    D3D12_GPU_VIRTUAL_ADDRESS enhancedHeapTLASAddresses[FRAME_COUNT] = {};
    // Bumped when the RR guides are (re)created; a slot whose descriptors
    // predate it still holds null or freed guide UAVs.
    UINT rrGuideGeneration = 0;
    UINT enhancedHeapRRGeneration[FRAME_COUNT] = {};
    // Owned by the scene-asset loader; kept so a heap rebuild rebinds them.
    ID3D12Resource* environmentMapResource = nullptr;
    ID3D12Resource* brdfLUTResource = nullptr;
    // Set per frame by the caller from scene.enhancedVisuals. Kept separate
    // from enhancedPipelineReady (a capability) so the UI can toggle freely
    // without rebuilding anything.
    bool enhancedVisualsActive = false;
    bool enhancedRTShadowsActive = true;
    bool enhancedRayClassifyActive = true;
    float enhancedConfidenceThreshold = 0.35f;
    float enhancedShadowRayLength = 220.0f;
    // Stochastic ray-traced reflections. One GGX-importance-sampled ray per
    // pixel per frame, rotated by frame index -- deliberately noisy, because
    // the temporal denoiser (Phase 5b) is what resolves it. The Scene startup
    // setting enables this through SetEnhancedVisuals each frame.
    bool enhancedRTReflectionsActive = false;
    float enhancedReflectionRayLength = 120.0f;
    // Above this roughness the GGX lobe is wide enough that one sample per
    // frame is mostly variance and the environment probe is already close.
    float enhancedReflectionRoughnessCut = 0.52f;
    // Radiance scale applied to an occluded reflection hit. Without a
    // hit-shading path this stands in for "something blocked the sky here".
    float enhancedReflectionOcclusion = 0.25f;
    // Reflection ray classification: trace only where the environment probe is
    // expected to be wrong. Rough and face-on pixels score high confidence and
    // keep the probe; grazing near-mirror pixels score low and get a ray.
    //
    // On by default: this is the cheap-tier-first structure the whole ray
    // budget rests on, and measurement backs it -- the classified path traced
    // 8.9% of pixels at 1.76 ms where the unclassified one traced 35.7% at
    // 6.22 ms on the same scene.
    bool enhancedReflectionClassifyActive = true;
    // Trace only where confidence in the probe is below this. 0.8 was measured
    // when a ray hit returned dimmed sky rather than real surface colour, and
    // 0.5 was rejected then for handing the probe pixels a mirror should have
    // traced. Now that hits shade from the actual geometry, a traced pixel is
    // worth more than it was and a probe pixel costs more by comparison, so the
    // tighter cut is worth spending fewer rays on: they go to the grazing
    // near-mirror pixels where the probe is most obviously wrong.
    float enhancedReflectionConfidenceCut = 0.5f;
    // Trace a diffuse bounce where the sparse probe grid reports a miss. Those
    // pixels otherwise keep sky ambient only and lose all bounce light.
    // On by default now that ray hits shade from real geometry: before that a
    // traced bounce returned dimmed sky and was not worth the ray. Falls back
    // to the probe grid wherever the acceleration structure has no binding.
    bool enhancedProbeMissGIActive = false;
    // 0 = fill probe misses only (cheapest, rays only where the grid failed),
    // 1 = full RT GI (every pixel traces, probes unused).
    // How much of the GI comes from rays rather than probes. 0 traces only
    // where the probe grid has nothing; 1 traces every pixel and ignores the
    // grid. Ray cost is the same at any non-zero value -- measured under 0.1 ms
    // between 0.5 and 1.0 -- so this is a quality dial rather than a budget:
    // lower values let the converged probe grid carry more of the irradiance
    // and the single-sample ray less, which is the quieter image where the grid
    // has good data.
    float enhancedProbeMissGIStrength = 1.0f;
    // Full-resolution Lumen traces each pixel; probes are its radiance cache
    // at hits. The opt-in shares irradiance on continuous surfaces.
    bool lumenGIActive = false;
    bool radianceCascadesGIRequested = false;
    bool radianceCascadesGIActive = false;
    // World radiance cache for Lumen hits (settings LumenRadianceCache).
    // Mode is this frame's b5 value: 0 off, 1 on, 2 clear then on.
    bool giRadianceCacheRequested = false;
    UINT giRadianceCacheMode = 0;
    // ReSTIR GI for the Lumen bounce (settings LumenReSTIR). Mode is this
    // frame's b5 value: 0 off, 1 with last frame's reservoirs, 2 without.
    bool emissiveCGNSRequested = false;
    bool emissiveCGNSHistoryValid = false;
    UINT emissiveCGNSMode = 0;
    bool lumenReSTIRRequested = false;
    UINT lumenReSTIRMode = 0;
    bool restirHistoryValid = false;
    // Two frames of per-pixel reservoirs (gi_restir.hlsli): 16 + 8 bytes.
    ComPtr<ID3D12Resource> restirSampleBuffer;
    ComPtr<ID3D12Resource> restirWeightBuffer;
    UINT restirWidth = 0, restirHeight = 0;
    bool restirAllocationFailed = false;
private:
    RadianceCascadesDX12 radianceCascades;
    VariableRateGIDX12 variableRateGI;
    bool variableRateGIRequested = false;
    bool variableRateGIActive = false;
    float variableRateGIBudget = 0.5f;
    GIRadianceCacheDX12 giRadianceCache;
    ResolveEntryPipelineDX12 rayQueryWarmup{ L"RayQueryWarmupMain",
                                             "Ray query warm-up" };
public:
    bool lumenGIHalfResolutionActive = false;
    bool lumenGIHalfResolutionSupported = false;
    // Player-facing reflection roughness cutoff (settings menu). Used instead
    // of the editor's enhancedReflectionRoughnessCut while RR or Lumen GI is
    // active -- the modes the menu drives.
    float settingsReflectionRoughnessCut = 1.0f;
    // Camera jitter the visibility pass rasterized with this frame, in render
    // pixels. Set by the frame loop; the resolve strips it from the motion
    // vectors while Ray Reconstruction is active.
    XMFLOAT2 rrMotionJitterPixels = { 0.0f, 0.0f };
    // Jitter of the previous frame's rendered projection (render pixels).
    XMFLOAT2 rrPreviousJitterPixels = { 0.0f, 0.0f };
    // Upscaling RR motion convention: 2 unjittered (default), 0 legacy.
    UINT rrUpscaleMotionMode = 2;
    UINT enhancedReflectionFrameCounter = 0;
    // SVGF temporal accumulation for RT reflections. Ping-pong history pair:
    // colour (E[x]), moments (E[x^2]) + sample count, one side read (SRV) and
    // the other side written (UAV), swapped every frame. Enabled during the
    // current RT/SVGF tuning pass.
    bool svgfTemporalEnabled = true;
    UINT svgfMaxAccumFrames = 32;
    UINT svgfHistoryPing = 0;
    bool svgfHistoryValid = false;
    bool svgfTemporalEnabledLastFrame = false;
    ComPtr<ID3D12Resource> svgfHistoryColor[2];
    ComPtr<ID3D12Resource> svgfHistoryMoments[2];
    // Shared authored surface identity. The write side is current-frame UAV;
    // the other side remains the previous-frame SRV until post has consumed it.
    ComPtr<ID3D12Resource> svgfStableSurfaceCurrent;
    ComPtr<ID3D12Resource> svgfStableSurfaceHistory;
    UINT stableSurfaceWriteIndex = 0;
    bool stableSurfaceIdentityActive = false;
    bool stableSurfaceIdentityActiveThisFrame = false;
    UINT stableSurfaceModeSignature = ~0u;
    // SVGF à-trous spatial filter (Phase 5c). Multi-iteration wavelet
    // filter applied to the specular IBL signal after the temporal pass
    // converges it in time. Ping-pong scratch pair for the iterations.
    bool svgfAtrousEnabled = true;
    UINT svgfAtrousIterations = 5;
    UINT svgfAtrousDiagnosticMode = 0;
    static constexpr UINT kSVGFAtrousMaxIterations = 5;
    // Player opt-in (settings FastGIDenoise): caps the passes the editor's
    // svgfAtrousIterations asks for. Measured on Base in motion, 3 passes:
    // GPU frame 14.37 -> 12.71 ms, image within run-to-run noise of 5 passes.
    static constexpr UINT kSVGFFastAtrousIterations = 3;
    bool svgfFastAtrous = false;
    UINT SVGFAtrousIterationCount() const {
        const UINT requested = std::clamp(svgfAtrousIterations, 1u,
                                          kSVGFAtrousMaxIterations);
        return svgfFastAtrous ? (std::min)(requested, kSVGFFastAtrousIterations)
                              : requested;
    }
    ComPtr<ID3D12Resource> svgfReflectionSrc;      // specular IBL from resolve
    ComPtr<ID3D12Resource> svgfAtrousScratch[2];   // ping-pong for à-trous
    ComPtr<ID3D12RootSignature> svgfAtrousRootSig;
    ComPtr<ID3D12PipelineState> svgfAtrousPSO;
    ComPtr<ID3D12DescriptorHeap> svgfAtrousDescHeaps[FRAME_COUNT];
    ComPtr<ID3D12Resource> svgfAtrousConstantBuffer;
    void* svgfAtrousConstantMapped = nullptr;
    ComPtr<ID3D12RootSignature> svgfCompositeRootSig;
    ComPtr<ID3D12PipelineState> svgfCompositePSO;
    ComPtr<ID3D12DescriptorHeap> svgfCompositeDescHeaps[FRAME_COUNT];
    ComPtr<ID3D12Resource> svgfCompositeConstantBuffer;
    void* svgfCompositeConstantMapped = nullptr;
    bool svgfAtrousPipelineReady = false;
    bool svgfAtrousPipelineTried = false;
    // Last-frame execution facts for the in-app RTX/SVGF self-test. These are
    // set where commands are recorded, so the UI can distinguish an enabled
    // checkbox from a pass that actually reached the command list.
    bool enhancedResolveExecutedLastFrame = false;
    bool svgfTemporalExecutedLastFrame = false;
    bool svgfAtrousExecutedLastFrame = false;
    bool svgfCompositeExecutedLastFrame = false;
    bool svgfMotionVectorsEnabledLastFrame = false;
    UINT svgfAtrousDispatchesLastFrame = 0;
    // TLAS this frame. Re-registered whenever it changes, since the
    // acceleration structure is rebuilt as geometry streams in.
    D3D12_GPU_VIRTUAL_ADDRESS enhancedTLASAddress = 0;
    // Ray-fraction statistic. Sampled from a few scanlines every N frames
    // rather than reduced over the full mask -- it only needs to be indicative,
    // and a full-screen reduction would cost more than the rays it measures.
    static constexpr UINT kRayMaskSampleRows = 8;
    static constexpr UINT kRayMaskSampleInterval = 30;
    UINT rayMaskRowPitch = 0;
    UINT rayMaskFrameCounter = 0;
    bool rayMaskCopyPending = false;
    float rayMaskFraction = 0.0f;
    // Split by ray type: bit 0 shadow, bit 1 reflection. The combined figure
    // saturates once the shadow gate traces most lit pixels, which hides
    // whether reflection classification is doing anything at all.
    float rayMaskShadowFraction = 0.0f;
    float rayMaskReflectionFraction = 0.0f;
    float rayMaskGIFraction = 0.0f;
    ComPtr<ID3D12RootSignature> postRootSig;
    ComPtr<ID3D12PipelineState> postPSO;
    ComPtr<ID3D12PipelineState> postUpscalePSO;
    ComPtr<ID3D12PipelineState> postQualityLensPSO;
    ComPtr<ID3D12PipelineState> postUpscaleQualityLensPSO;
    ComPtr<ID3D12DescriptorHeap> postDescHeap;
    // Two source choices (render input or DLSS output), each with four history
    // parity combinations. Immutable descriptors stay safe across frame slots.
    static constexpr UINT kPostDescriptorsPerVariant = 19;
    static constexpr UINT kPostDescriptorVariantCount = 8;
    ComPtr<ID3D12RootSignature> bloomRootSig;
    ComPtr<ID3D12PipelineState> bloomDownsamplePSO;
    ComPtr<ID3D12PipelineState> bloomUpsamplePSO;
    ComPtr<ID3D12DescriptorHeap> bloomDescHeap;
    ComPtr<ID3D12RootSignature> flareRootSig;
    ComPtr<ID3D12PipelineState> flareFeaturePSO;
    ComPtr<ID3D12PipelineState> flareStreakPSO;
    ComPtr<ID3D12PipelineState> flareBlurPSO;
    ComPtr<ID3D12PipelineState> flareQualityFeaturePSO;
    ComPtr<ID3D12PipelineState> flareQualityStreakPSO;
    ComPtr<ID3D12PipelineState> flareQualityBlurPSO;
    ComPtr<ID3D12DescriptorHeap> flareDescHeap;
    bool flarePipelineReady = false;
    bool qualityLensPipelineReady = false;
    ComPtr<ID3D12RootSignature> exposureRootSig;
    ComPtr<ID3D12PipelineState> exposureResetPSO;
    ComPtr<ID3D12PipelineState> exposureAccumulatePSO;
    ComPtr<ID3D12PipelineState> exposureFinalizePSO;
    ComPtr<ID3D12DescriptorHeap> exposureDescHeap;

    // GPU-visible structured buffers
    ComPtr<ID3D12Resource> drawCallBuffer;       // StructuredBuffer<DrawCallData>
    ComPtr<ID3D12Resource> drawCallUpload[FRAME_COUNT];
    ComPtr<ID3D12Resource> vertexDataBuffer;     // StructuredBuffer<PackedVertex>
    ComPtr<ID3D12Resource> vertexDataUpload[FRAME_COUNT];
    ComPtr<ID3D12Resource> indexDataBuffer;       // StructuredBuffer<uint>
    ComPtr<ID3D12Resource> indexDataUpload[FRAME_COUNT];
    ComPtr<ID3D12Resource> stableTriangleDataBuffer;
    ComPtr<ID3D12Resource> stableTriangleDataUpload[FRAME_COUNT];
    // Per-geometry binding from a raytracing hit to this buffer's persistent
    // geometry. Written only when the acceleration structure is rebuilt, which
    // is a rare, already-GPU-drained event, so it lives on an upload heap and
    // is written in place rather than staged through a copy.
    ComPtr<ID3D12Resource> hitGeometryBuffer;
    UINT hitGeometryCount = 0;
    // Emissive triangles as lights (emissive_restir.hlsli), written on
    // acceleration rebuilds like hitGeometryBuffer. Entry 0 is always valid and
    // carries the count, so an empty scene reads lightCount 0 and returns.
    ComPtr<ID3D12Resource> emissiveTriangleBuffer;
    UINT emissiveTriangleCount = 0;
    // Two frames of per-pixel reservoirs, sized lazily to the render size. The
    // placeholder keeps u19 bound to something valid until they exist.
    ComPtr<ID3D12Resource> emissiveReservoirBuffer;
    ComPtr<ID3D12Resource> emissiveReservoirPlaceholder;
    UINT emissiveReservoirWidth = 0, emissiveReservoirHeight = 0;
    bool emissiveReservoirAllocationFailed = false;
    ComPtr<ID3D12Resource> clusterDataBuffer;     // StructuredBuffer<ClusterData>
    ComPtr<ID3D12Resource> clusterDataUpload[FRAME_COUNT];
    ComPtr<ID3D12Resource> materialDataBuffer;
    VBMaterialData* mappedMaterials = nullptr;

    // Upload buffer for frame constants
    UploadBuffer<VBFrameConstants> frameConstantBuffer;
    UploadBuffer<VBPostConstants> postConstantBuffer;
    UploadBuffer<VBExposureConstants> exposureConstantBuffer;

    // Descriptor heap for compute pass SRVs/UAVs
    ComPtr<ID3D12DescriptorHeap> computeDescHeap;
    ComPtr<ID3D12DescriptorHeap> rasterViewHeaps[FRAME_COUNT];
    ComPtr<ID3D12DescriptorHeap> resolveViewHeaps[FRAME_COUNT];
    UploadBuffer<VBMaterialData> legacyMaterialSnapshots;

    // CPU-side staging data
    std::vector<VBDrawCallData> cpuDrawCalls;
    std::vector<VBPackedVertex> cpuVertices;
    std::vector<UINT>           cpuIndices;
    std::vector<UINT>           cpuStableTriangleIDs;
    std::vector<VBClusterData>  cpuClusters;
    std::vector<XMFLOAT4X4>     previousModels;
    std::unordered_map<uint64_t, XMFLOAT4X4> previousModelByInstance;
    std::vector<VBMeshData>     meshes;
    std::unordered_map<const MeshPrimitive*, UINT> primitiveMeshLookup;
    // Transient geometry -- destruction re-merges its chunk batches every time
    // a fracture changes the chunk set -- returns its storage to this pool
    // instead of leaking it. See VisibilityGeometryPool.h.
    VisibilityGeometryPool geometryPool{ geometryVertexCapacity, geometryIndexCapacity,
                                         geometryTriangleCapacity, FRAME_COUNT };
    std::vector<UINT> freeMeshSlots;
    // Slot indices released this frame. Held back one frame before joining
    // freeMeshSlots, for the same reason the geometry ranges are quarantined:
    // releases happen during the update phase, so an index handed straight back
    // could be claimed by a different primitive while draw calls recorded for
    // the previous frame still reference it.
    std::vector<UINT> retiredMeshSlots;
    // Meshes registered as transient, so ReleasePrimitive knows a slot is
    // recyclable and UploadBuffers knows the geometry can change in place.
    std::unordered_set<UINT> transientMeshSlots;
    // Registrations turned away because the geometry pool was full. Exposed so
    // exhaustion is visible in the UI instead of silently dropping geometry.
    UINT64 geometryRegistrationFailures = 0;
    std::unordered_map<const SceneMaterial*, UINT> materialLookup;
    std::unordered_map<ID3D12Resource*, UINT> materialTextureLookup;
    UINT materialCount = 1;
    UINT materialTextureCount = 0;
    // Distinct textures turned away because the fixed 64-slot array was full.
    // A set, not a counter: the useful number is how many UNIQUE textures did
    // not fit, since that is how much larger the array would have to be (or how
    // much a bindless heap would buy).
    std::unordered_set<ID3D12Resource*> materialTexturesRejected;

    // Bindless tier. Kept in a parallel buffer rather than reusing the legacy
    // records because textureIndices means something different in each: a slot
    // in the 64-entry t8..t71 table for legacy, an absolute bindless heap index
    // for bindless. Sharing one buffer would make a stale record from the other
    // tier sample an arbitrary texture rather than fail visibly.
    ComPtr<ID3D12Resource> bindlessMaterialDataBuffer;
    VBMaterialData* mappedBindlessMaterials = nullptr;
    std::unordered_map<const SceneMaterial*, UINT> bindlessMaterialLookup;
    UINT bindlessMaterialCount = 1;
    // Set once per frame from the scene toggle. Registration consults this so a
    // material registered while bindless is off does not poison the bindless
    // table, and vice versa.
    bool bindlessActive = false;
    bool bindlessTransientOverflowLastFrame = false;
    UINT bindlessResolveTableBases[FRAME_COUNT] = {
        BINDLESS_INVALID_INDEX, BINDLESS_INVALID_INDEX
    };

    UINT currentDrawCall = 0;
    UINT previousDrawCount = 0;
    UINT drawCallDirtyMin = UINT_MAX;
    UINT drawCallDirtyMax = 0;
    UINT persistentVertexCount = 0;
    UINT persistentIndexCount = 0;
    UINT persistentTriangleCount = 0;
    UINT persistentAuthoredTriangleCount = 0;
    bool geometryUploaded = false;
    bool geometryDirty = false;
    // Element spans written since the last successful geometry upload. Only
    // these are copied: re-sending the whole high-water prefix cost ~6 ms of
    // GPU copy per registration (~48 MB on the full level), and destruction
    // registers meshes every one to three frames.
    struct DirtySpan {
        UINT begin = UINT_MAX;
        UINT end = 0;
        void Add(UINT offset, UINT count) {
            if (count == 0) return;
            begin = (std::min)(begin, offset);
            end = (std::max)(end, offset + count);
        }
        bool Empty() const { return begin >= end; }
        void Clear() { begin = UINT_MAX; end = 0; }
    };
    DirtySpan dirtyVertices;
    DirtySpan dirtyIndices;
    DirtySpan dirtyTriangles;
    UINT postFrameIndex = 0;
    float exposure = 1.15f;
    float bloomStrength = 0.16f;
    // Build bloom from the DLSS output rather than the jittered render frame.
    bool bloomFromUpscaled = true;
    float vignetteStrength = 0.50f;
    float grainStrength = 0.0f;
    // Lens artefacts. Dirt, streaks and flare draw energy from bloom;
    // aberration resamples the HDR source directly. The scene-level sun-lens
    // toggle gates all of them, independently of these authored strengths.
    // Tuned for the sparse fullscreen CC0 mask: visible in solar glare, absent
    // from an ordinarily exposed frame.
    float lensDirtStrength = 0.01f;
    float lensDirtScale = 0.50f;
    float chromaticAberration = 0.08f;
    // The apertures are deliberately low-opacity; strength provides the bold
    // Battlefield-style response without turning them into painted rings.
    float lensFlareStrength = 1.34f;
    bool highQualityLensEnabled = true;
    float sunLensRadiusUV = 0.0f;
    bool sunLensSystemEnabled = false;
    XMFLOAT4 sunLensPosition = { 0.5f, 0.5f, 0.0f, 0.0f };
    XMFLOAT4 sunLensColor = { 1.0f, 0.92f, 0.70f, 0.0f };
    float taaFeedback = 0.86f;
    bool temporalEffectsEnabled = false;
    // DLSS owns temporal accumulation this frame. Keeps the sub-pixel jitter
    // and motion vectors TAA would have used, but the post pass's own history
    // blend is bypassed so the image is not accumulated twice.
    bool dlssActive = false;
    // Set wherever engine history is invalidated; consumed by the DLSS pass.
    bool dlssHistoryReset = true;
    bool temporalHistoryValid = false;
    bool exposureReadable = false;
    float exposureAdaptation = 0.05f;
    float motionBlurStrength = 0.0f;
    PalmWindFrameDX12 palmWindFrame{};
    // Impact cutout volumes for the bindless primary-visibility pass.
    UploadBuffer<ImpactDecalsBufferDX12> impactDecalsBuffer;
    ImpactDecalsBufferDX12 impactDecalsCPU{};
    float focusDistance = 8.0f;
    float aperture = 0.0f;
    float currentNearPlane = 0.1f;
    float currentFarPlane = 1000.0f;
    int debugViewMode = 0; // 0=lit; 1..7 are the UI diagnostic views.
    bool validationMode = false;
    UINT bloomMipCount = 1;
    UINT bloomWidth = 1;
    UINT bloomHeight = 1;
    UINT flareWidth = 1;
    UINT flareHeight = 1;
    // Battlefield-style flare controls, tuned against reference stills rather
    // than toward restraint. The flare in those shots is a large, obvious
    // feature -- a bright warm wash around the sun, a chain of big soft discs
    // running across the frame, and a cool horizontal streak -- so defaults
    // that keep it subliminal are simply the wrong look. The master Lens Flare
    // slider scales all of it for anyone who wants it quieter.
    float flareStreakIntensity = 0.78f;
    // In UV, the half-width of the horizontal blur. 0.369 spans well over half
    // the frame, which is where the streak stops reading as a smear and starts
    // reading as a cylindrical front element.
    float flareStreakLength = 0.369f;
    float flareGhostIntensity = 1.23f;
    // Per-channel radial split. Above roughly 0.12 the three channels separate
    // far enough to read as three discs rather than one refracted element.
    float flareGhostDispersion = 0.200f;
    // Carries both the veiling wash and the ring, and the wash is the single
    // largest feature in the reference -- hence the highest default here.
    float flareHaloIntensity = 2.00f;
    float flareStarburstIntensity = 1.63f;

    // Updates the display-lens source without coupling this renderer to Scene.
    // The unjittered projection matches the sky pass; TAA jitter belongs to
    // geometry and would otherwise make the flare swim by a sub-pixel.
    void SetSunLens(bool enabled, const XMMATRIX& viewProjection,
                    const XMFLOAT3& cameraPosition,
                    const XMFLOAT3& lightDirection,
                    const XMFLOAT3& lightColor, float lightIntensity) {
        sunLensSystemEnabled = enabled;
        sunLensColor = {
            lightColor.x, lightColor.y, lightColor.z, 0.0f
        };
        sunLensPosition = { 0.5f, 0.5f, 0.0f, 0.0f };
        if (!enabled) return;

        const XMVECTOR direction =
            XMVector3Normalize(XMLoadFloat3(&lightDirection));
        const XMVECTOR sunWorld = XMVectorAdd(
            XMLoadFloat3(&cameraPosition), XMVectorScale(direction, 4000.0f));
        XMFLOAT4 projected = {};
        XMStoreFloat4(&projected,
            XMVector3Transform(sunWorld, viewProjection));
        if (projected.w <= 0.0f) return;

        const float u = projected.x / projected.w * 0.5f + 0.5f;
        const float v = -projected.y / projected.w * 0.5f + 0.5f;
        const float elevation = XMVectorGetY(direction);
        const float elevationT = (std::max)(0.0f, (std::min)(1.0f,
            (elevation + 0.08f) / 0.14f));
        const float elevationFade =
            elevationT * elevationT * (3.0f - 2.0f * elevationT);
        // Presence ramps down across a margin outside the frame rather than
        // switching off at the edge. A binary on-screen test made the whole
        // flare vanish in a single frame the moment the sun crossed a boundary,
        // which is precisely where a real anamorphic flare is at its strongest:
        // the streak and the ghost chain both survive the source leaving the
        // sensor, because the light still enters the barrel.
        //
        // kOffScreenMargin is in UV, so 0.35 is roughly a third of a frame past
        // the edge. Beyond that the sun is far enough off-axis that no internal
        // reflection would reach the sensor and presence is genuinely zero.
        // Consumers must clamp their sample UVs: this position is deliberately
        // allowed outside [0,1].
        constexpr float kOffScreenMargin = 0.35f;
        const float overshoot = (std::max)({
            0.0f, -u, u - 1.0f, -v, v - 1.0f });
        const float edgeT = 1.0f - (std::min)(1.0f, overshoot / kOffScreenMargin);
        const float offScreenFade = edgeT * edgeT * (3.0f - 2.0f * edgeT);
        sunLensPosition = {
            u, v, offScreenFade * (elevationFade > 0.0f ? 1.0f : 0.0f),
            (std::max)(0.0f, lightIntensity) * elevationFade
        };
    }

    UINT width = 0;
    UINT height = 0;
    UINT displayWidth = 0;
    UINT displayHeight = 0;

    ID3D12Resource* DLSSUpscaledResource() const {
        return dlssUpscaledTexture.Get();
    }

    struct ScopeViewStorage {
        UINT width = 1024, height = 1024;
        ComPtr<ID3D12Resource> visBufferRT;
        ComPtr<ID3D12Resource> outputTexture;
        ComPtr<ID3D12Resource> motionTexture;
        ComPtr<ID3D12Resource> normalRoughnessTexture;
        ComPtr<ID3D12Resource> rayMaskTexture;
        ComPtr<ID3D12Resource> svgfReflectionSrc;
        ComPtr<ID3D12Resource> svgfStableSurfaceCurrent;
        ComPtr<ID3D12Resource> drawCallBuffer;
        ComPtr<ID3D12Resource> clusterDataBuffer;
        ComPtr<ID3D12DescriptorHeap> visRtvHeap;
        ComPtr<ID3D12DescriptorHeap> outputRtvHeap;
        ComPtr<ID3D12DescriptorHeap> computeDescHeap;
        ComPtr<ID3D12Resource> drawCallUpload[FRAME_COUNT];
        ComPtr<ID3D12Resource> clusterDataUpload[FRAME_COUNT];
        ComPtr<ID3D12Resource> vertexDataUpload[FRAME_COUNT];
        ComPtr<ID3D12Resource> indexDataUpload[FRAME_COUNT];
        ComPtr<ID3D12Resource> stableTriangleDataUpload[FRAME_COUNT];
        ComPtr<ID3D12DescriptorHeap> rasterViewHeaps[FRAME_COUNT];
        ComPtr<ID3D12DescriptorHeap> resolveViewHeaps[FRAME_COUNT];
        ComPtr<ID3D12DescriptorHeap> enhancedComputeDescHeaps[FRAME_COUNT];
        std::vector<VBDrawCallData> cpuDrawCalls;
        std::vector<VBClusterData> cpuClusters;
        std::vector<XMFLOAT4X4> previousModels;
        std::unordered_map<uint64_t, XMFLOAT4X4> previousModelByInstance;
        UINT currentDrawCall = 0, previousDrawCount = 0;
        UINT drawCallDirtyMin = UINT_MAX, drawCallDirtyMax = 0;
        UINT bindlessResolveTableBases[FRAME_COUNT]{};
        XMMATRIX terrainProjection = XMMatrixIdentity();
        bool terrainProjectionValid = false;
        bool terrainVisibilityActiveThisFrame = false;
        bool terrainDescriptorsWritten = false;
        float currentNearPlane = 0.1f, currentFarPlane = 1000.0f;
    };
    std::unique_ptr<ScopeViewStorage> scopeView;

    UINT ViewFrameIndex() const {
        return RenderViewFrameIndexDX12(g_dx12.frameIndex, FRAME_COUNT,
            ScopeSurfaceBound() ? RenderViewDX12::Scope : RenderViewDX12::Main);
    }
    ID3D12DescriptorHeap* RasterViewHeap() const {
        return rasterViewHeaps[g_dx12.frameIndex % FRAME_COUNT].Get();
    }
    ID3D12DescriptorHeap* ResolveViewHeap() const {
        return resolveViewHeaps[g_dx12.frameIndex % FRAME_COUNT].Get();
    }

    bool InitScopeView() {
        if (!initialized) return false;
        auto view = std::make_unique<ScopeViewStorage>();
        const auto buffer = [&](UINT64 bytes, D3D12_HEAP_TYPE type,
                                ComPtr<ID3D12Resource>& out) {
            D3D12_HEAP_PROPERTIES heap{}; heap.Type = type;
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width = bytes; desc.Height = 1;
            desc.DepthOrArraySize = desc.MipLevels = 1;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            return SUCCEEDED(g_dx12.device->CreateCommittedResource(&heap,
                D3D12_HEAP_FLAG_NONE, &desc,
                type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ
                                            : D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr, IID_PPV_ARGS(&out)));
        };
        const auto texture = [&](DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags,
                                 D3D12_RESOURCE_STATES initial,
                                 const wchar_t* name, ComPtr<ID3D12Resource>& out) {
            D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = view->width; desc.Height = view->height;
            desc.DepthOrArraySize = desc.MipLevels = 1;
            desc.Format = format; desc.SampleDesc.Count = 1; desc.Flags = flags;
            if (FAILED(g_dx12.device->CreateCommittedResource(&heap,
                    D3D12_HEAP_FLAG_NONE, &desc, initial, nullptr,
                    IID_PPV_ARGS(&out)))) return false;
            out->SetName(name);
            return true;
        };
        const auto srv = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        const auto uav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        const auto rt = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        const auto ua = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (!texture(DXGI_FORMAT_R32G32_UINT, rt, srv,
                     L"Scope visibility", view->visBufferRT) ||
            !texture(DXGI_FORMAT_R16G16B16A16_FLOAT, rt | ua, srv,
                     L"Scope HDR world", view->outputTexture) ||
            !texture(DXGI_FORMAT_R16G16_FLOAT, ua, srv,
                     L"Scope motion scratch", view->motionTexture) ||
            !texture(DXGI_FORMAT_R16G16B16A16_FLOAT, ua, srv,
                     L"Scope surface", view->normalRoughnessTexture) ||
            !texture(DXGI_FORMAT_R8_UINT, ua, uav,
                     L"Scope ray classification", view->rayMaskTexture) ||
            !texture(DXGI_FORMAT_R16G16B16A16_FLOAT, ua, uav,
                     L"Scope reflection signal", view->svgfReflectionSrc) ||
            !texture(DXGI_FORMAT_R32G32_UINT, ua, uav,
                     L"Scope surface identity", view->svgfStableSurfaceCurrent)) return false;
        if (!buffer(VB_MAX_DRAW_CALLS * sizeof(VBDrawCallData),
                    D3D12_HEAP_TYPE_DEFAULT, view->drawCallBuffer) ||
            !buffer(VB_CLUSTER_COUNT * sizeof(VBClusterData),
                    D3D12_HEAP_TYPE_DEFAULT, view->clusterDataBuffer)) return false;
        D3D12_DESCRIPTOR_HEAP_DESC desc{};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; desc.NumDescriptors = 1;
        if (FAILED(g_dx12.device->CreateDescriptorHeap(&desc,
                IID_PPV_ARGS(&view->visRtvHeap))) ||
            FAILED(g_dx12.device->CreateDescriptorHeap(&desc,
                IID_PPV_ARGS(&view->outputRtvHeap)))) return false;
        g_dx12.device->CreateRenderTargetView(view->visBufferRT.Get(), nullptr,
            view->visRtvHeap->GetCPUDescriptorHandleForHeapStart());
        g_dx12.device->CreateRenderTargetView(view->outputTexture.Get(), nullptr,
            view->outputRtvHeap->GetCPUDescriptorHandleForHeapStart());
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.NumDescriptors = kResolveDescriptorCount;
        if (FAILED(g_dx12.device->CreateDescriptorHeap(&desc,
                IID_PPV_ARGS(&view->computeDescHeap)))) return false;
        for (UINT frame = 0; frame < FRAME_COUNT; ++frame) {
            if (!buffer(VB_MAX_DRAW_CALLS * sizeof(VBDrawCallData),
                        D3D12_HEAP_TYPE_UPLOAD, view->drawCallUpload[frame]) ||
                !buffer(VB_CLUSTER_COUNT * sizeof(VBClusterData),
                        D3D12_HEAP_TYPE_UPLOAD, view->clusterDataUpload[frame]) ||
                !buffer(geometryVertexCapacity * sizeof(VBPackedVertex),
                        D3D12_HEAP_TYPE_UPLOAD, view->vertexDataUpload[frame]) ||
                !buffer(geometryIndexCapacity * sizeof(UINT),
                        D3D12_HEAP_TYPE_UPLOAD, view->indexDataUpload[frame]) ||
                !buffer(geometryTriangleCapacity * sizeof(UINT),
                        D3D12_HEAP_TYPE_UPLOAD, view->stableTriangleDataUpload[frame])) return false;
            desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            desc.NumDescriptors = kEnhancedResolveDescriptorCount;
            if (FAILED(g_dx12.device->CreateDescriptorHeap(&desc,
                    IID_PPV_ARGS(&view->enhancedComputeDescHeaps[frame])))) return false;
            desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            if (FAILED(g_dx12.device->CreateDescriptorHeap(&desc,
                    IID_PPV_ARGS(&view->resolveViewHeaps[frame])))) return false;
            desc.NumDescriptors = kResolveDescriptorCount;
            if (FAILED(g_dx12.device->CreateDescriptorHeap(&desc,
                    IID_PPV_ARGS(&view->rasterViewHeaps[frame])))) return false;
            view->bindlessResolveTableBases[frame] = BINDLESS_INVALID_INDEX;
        }
        view->cpuDrawCalls.resize(VB_MAX_DRAW_CALLS);
        view->cpuClusters.resize(VB_CLUSTER_COUNT);
        view->previousModels.resize(VB_MAX_DRAW_CALLS);
        scopeView = std::move(view);
        return true;
    }

    void SwapScopeViewStorage() {
        // Swap only view-dependent state. The large immutable geometry pools
        // and material registry are shared by both cameras.
        using std::swap;
        swap(width, scopeView->width);
        swap(height, scopeView->height);
        swap(visBufferRT, scopeView->visBufferRT);
        swap(outputTexture, scopeView->outputTexture);
        swap(motionTexture, scopeView->motionTexture);
        swap(normalRoughnessTexture, scopeView->normalRoughnessTexture);
        swap(rayMaskTexture, scopeView->rayMaskTexture);
        swap(svgfReflectionSrc, scopeView->svgfReflectionSrc);
        swap(svgfStableSurfaceCurrent, scopeView->svgfStableSurfaceCurrent);
        swap(drawCallBuffer, scopeView->drawCallBuffer);
        swap(clusterDataBuffer, scopeView->clusterDataBuffer);
        swap(visRtvHeap, scopeView->visRtvHeap);
        swap(outputRtvHeap, scopeView->outputRtvHeap);
        swap(computeDescHeap, scopeView->computeDescHeap);
        swap(cpuDrawCalls, scopeView->cpuDrawCalls);
        swap(cpuClusters, scopeView->cpuClusters);
        swap(previousModels, scopeView->previousModels);
        swap(previousModelByInstance, scopeView->previousModelByInstance);
        swap(currentDrawCall, scopeView->currentDrawCall);
        swap(previousDrawCount, scopeView->previousDrawCount);
        swap(drawCallDirtyMin, scopeView->drawCallDirtyMin);
        swap(drawCallDirtyMax, scopeView->drawCallDirtyMax);
        swap(terrainProjection, scopeView->terrainProjection);
        swap(terrainProjectionValid, scopeView->terrainProjectionValid);
        swap(terrainVisibilityActiveThisFrame, scopeView->terrainVisibilityActiveThisFrame);
        swap(terrainDescriptorsWritten, scopeView->terrainDescriptorsWritten);
        swap(currentNearPlane, scopeView->currentNearPlane);
        swap(currentFarPlane, scopeView->currentFarPlane);
        for (UINT frame = 0; frame < FRAME_COUNT; ++frame) {
            swap(drawCallUpload[frame], scopeView->drawCallUpload[frame]);
            swap(clusterDataUpload[frame], scopeView->clusterDataUpload[frame]);
            swap(vertexDataUpload[frame], scopeView->vertexDataUpload[frame]);
            swap(indexDataUpload[frame], scopeView->indexDataUpload[frame]);
            swap(stableTriangleDataUpload[frame], scopeView->stableTriangleDataUpload[frame]);
            swap(rasterViewHeaps[frame], scopeView->rasterViewHeaps[frame]);
            swap(resolveViewHeaps[frame], scopeView->resolveViewHeaps[frame]);
            swap(enhancedComputeDescHeaps[frame], scopeView->enhancedComputeDescHeaps[frame]);
            swap(bindlessResolveTableBases[frame], scopeView->bindlessResolveTableBases[frame]);
        }
    }

    void SnapshotLegacyMaterials() {
        const UINT first = ViewFrameIndex() * VB_MAX_MATERIALS;
        std::memcpy(legacyMaterialSnapshots.mappedData + first, mappedMaterials,
                    materialCount * sizeof(VBMaterialData));
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Buffer.FirstElement = first;
        srv.Buffer.NumElements = VB_MAX_MATERIALS;
        srv.Buffer.StructureByteStride = sizeof(VBMaterialData);
        auto handle = computeDescHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += 7ull * g_dx12.cbvSrvUavDescriptorSize;
        g_dx12.device->CreateShaderResourceView(
            legacyMaterialSnapshots.resource.Get(), &srv, handle);
    }

    void PatchScopeDescriptors() {
        const auto handle = [&](UINT slot) {
            auto result = computeDescHeap->GetCPUDescriptorHandleForHeapStart();
            result.ptr += static_cast<SIZE_T>(slot) * g_dx12.cbvSrvUavDescriptorSize;
            return result;
        };
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;
        srv.Format = DXGI_FORMAT_R32G32_UINT;
        g_dx12.device->CreateShaderResourceView(visBufferRT.Get(), &srv, handle(0));
        srv.Format = DXGI_FORMAT_R32_FLOAT;
        g_dx12.device->CreateShaderResourceView(ActiveDepthBuffer(), &srv, handle(1));
        srv = {};
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Buffer.NumElements = VB_MAX_DRAW_CALLS;
        srv.Buffer.StructureByteStride = sizeof(VBDrawCallData);
        g_dx12.device->CreateShaderResourceView(drawCallBuffer.Get(), &srv, handle(3));
        srv.Buffer.NumElements = VB_CLUSTER_COUNT;
        srv.Buffer.StructureByteStride = sizeof(VBClusterData);
        g_dx12.device->CreateShaderResourceView(clusterDataBuffer.Get(), &srv, handle(6));
        ID3D12Resource* outputs[] = { outputTexture.Get(), motionTexture.Get(), normalRoughnessTexture.Get() };
        for (UINT i = 0; i < 3; ++i) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            uav.Format = outputs[i]->GetDesc().Format;
            g_dx12.device->CreateUnorderedAccessView(outputs[i], nullptr, &uav, handle(79 + i));
        }
        WriteBentNormalHistoryDescriptor(computeDescHeap.Get(), 86, nullptr);
    }

    // --- Per-view surface binding (sniper scope) ---
    //
    // Every camera-dependent pass below used to read the global depth buffer,
    // DSV heap, viewport and swapchain backbuffer directly. That is correct
    // while exactly one view renders per frame. The scope renders a second
    // camera into its own square target, so those four reads become
    // indirections through the currently bound surface.
    //
    // Default-constructed to the globals, so a caller that never binds a
    // surface -- which is every existing main-view call site -- gets the
    // identical resources it had before, and its submission sequence is
    // unchanged.
    struct ViewSurface {
        ID3D12Resource* depthBuffer = nullptr;         // null => global
        D3D12_CPU_DESCRIPTOR_HANDLE depthDSV{};
        bool hasDepthDSV = false;
        const D3D12_VIEWPORT* viewport = nullptr;      // null => global
        const D3D12_RECT* scissor = nullptr;
        ID3D12Resource* destination = nullptr;         // null => backbuffer
        UINT viewWidth = 0;
        UINT viewHeight = 0;
        bool isScope = false;
    };
    ViewSurface boundSurface;

    void BindViewSurface(const ViewSurface& surface) {
        assert(!ScopeSurfaceBound());
        assert(!surface.isScope || scopeView);
        if (surface.isScope) {
            g_dx12.device->CopyDescriptorsSimple(kResolveDescriptorCount,
                scopeView->computeDescHeap->GetCPUDescriptorHandleForHeapStart(),
                computeDescHeap->GetCPUDescriptorHandleForHeapStart(),
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            SwapScopeViewStorage();
        }
        boundSurface = surface;
        if (surface.isScope) {
            PatchScopeDescriptors();
            if (bindlessActive && !PrepareBindlessFrame(g_dx12.frameIndex, true))
                bindlessActive = false;
        }
    }
    void UnbindViewSurface() {
        if (ScopeSurfaceBound()) SwapScopeViewStorage();
        boundSurface = ViewSurface{};
    }
    bool ScopeSurfaceBound() const { return boundSurface.isScope; }

    ID3D12Resource* ActiveDepthBuffer() const {
        return boundSurface.depthBuffer ? boundSurface.depthBuffer
                                        : g_dx12.depthStencilBuffer.Get();
    }
    D3D12_CPU_DESCRIPTOR_HANDLE ActiveDSV() const {
        return boundSurface.hasDepthDSV
            ? boundSurface.depthDSV
            : g_dx12.dsvHeap->GetCPUDescriptorHandleForHeapStart();
    }
    const D3D12_VIEWPORT& ActiveViewport() const {
        return boundSurface.viewport ? *boundSurface.viewport : g_dx12.viewport;
    }
    const D3D12_RECT& ActiveScissor() const {
        return boundSurface.scissor ? *boundSurface.scissor : g_dx12.scissorRect;
    }
    bool initialized = false;
    std::string initError;

    XMFLOAT2 GetTemporalJitterPixels() const {
        if (!(temporalEffectsEnabled || dlssActive) || validationMode)
            return { 0.0f, 0.0f };
        // Super Resolution needs more phases than TAA: each output pixel sees
        // ratio^2 fewer render samples. NVIDIA's guidance is 8 * ratio^2.
        // Ray Reconstruction wants at least 32 at any ratio, DLAA included
        // (DLSS-RR Integration Guide 3.6). rayReconstructionActive is last
        // frame's value here; a toggle settles one frame later.
        if (dlssActive && width && height &&
            (width < displayWidth || rayReconstructionActive)) {
            const float ratio = static_cast<float>(displayWidth) /
                                static_cast<float>(width);
            const UINT minimumPhases = rayReconstructionActive ? 32u : 8u;
            const UINT phases = std::clamp(
                static_cast<UINT>(8.0f * ratio * ratio + 0.5f),
                minimumPhases, 128u);
            const auto halton = [](UINT index, UINT base) {
                float f = 1.0f, r = 0.0f;
                for (; index > 0; index /= base) {
                    f /= static_cast<float>(base);
                    r += f * static_cast<float>(index % base);
                }
                return r;
            };
            const UINT index = (postFrameIndex % phases) + 1u;
            return { halton(index, 2u) - 0.5f, halton(index, 3u) - 0.5f };
        }
        // Eight-sample Halton(2,3), centered on pixel. Sequence repeats only
        // after covering complementary sub-pixel locations.
        static constexpr XMFLOAT2 sequence[8] = {
            { 0.0f,       -0.1666667f },
            { -0.25f,      0.1666667f },
            { 0.25f,      -0.3888889f },
            { -0.375f,    -0.0555556f },
            { 0.125f,      0.2777778f },
            { -0.125f,    -0.2777778f },
            { 0.375f,      0.0555556f },
            { -0.4375f,    0.3888889f }
        };
        return sequence[postFrameIndex & 7u];
    }

    void InvalidateTemporalHistory() {
        dlssHistoryReset = true;
        temporalHistoryValid = false;
        surfaceHistoryValid = false;
        svgfHistoryValid = false;
        restirHistoryValid = false;
        emissiveCGNSHistoryValid = false;
        variableRateGI.InvalidateHistory();
    }

    ID3D12Resource* StableSurfaceResource(UINT index) const {
        if (ScopeSurfaceBound()) return svgfStableSurfaceCurrent.Get();
        return index == 0u ? svgfStableSurfaceCurrent.Get()
                           : svgfStableSurfaceHistory.Get();
    }

    bool StableSurfaceIdentityRequired(bool enhancedResolve) const {
        if (validationMode || debugViewMode != 0 ||
            BentNormalGTAODiagnosticActive())
            return false;
        // ReSTIR validates reused reservoirs against last frame's surface
        // identity exactly as SVGF does, and runs under RR where SVGF does not.
        return (surfaceIDTemporalEnabled && temporalEffectsEnabled) ||
               historyDebugView ||
               (enhancedResolve && svgfTemporalEnabled) ||
               (enhancedResolve && lumenGIActive && lumenReSTIRRequested) ||
               (enhancedResolve && emissiveCGNSRequested);
    }

    UINT StableSurfaceModeSignature(bool visibilityPath,
                                    bool enhancedResolve) const {
        return (visibilityPath ? 1u : 0u) |
               (surfaceIDTemporalEnabled ? 1u << 1u : 0u) |
               (temporalEffectsEnabled ? 1u << 2u : 0u) |
               (historyDebugView ? 1u << 3u : 0u) |
               (enhancedResolve ? 1u << 4u : 0u) |
               (svgfTemporalEnabled ? 1u << 5u : 0u) |
               (validationMode ? 1u << 6u : 0u) |
               (debugViewMode != 0 ? 1u << 7u : 0u) |
               (BentNormalGTAODiagnosticActive() ? 1u << 8u : 0u);
    }

    void PrepareStableSurfaceHistory(bool active, UINT modeSignature) {
        if (ScopeSurfaceBound()) return;
        if (active != stableSurfaceIdentityActive ||
            modeSignature != stableSurfaceModeSignature) {
            surfaceHistoryValid = false;
            svgfHistoryValid = false;
            stableSurfaceIdentityActive = active;
            stableSurfaceModeSignature = modeSignature;
        }
        stableSurfaceIdentityActiveThisFrame = active;
    }

    bool Init(UINT screenWidth, UINT screenHeight,
              UINT outputWidth = 0, UINT outputHeight = 0) {
        width = screenWidth;
        height = screenHeight;
        displayWidth = outputWidth ? outputWidth : screenWidth;
        displayHeight = outputHeight ? outputHeight : screenHeight;

        initError.clear();
        auto require = [&](bool success, const char* stage) {
            if (success) return true;
            initError = stage;
            std::ofstream log("visibility_buffer_error.log", std::ios::trunc);
            log << "Visibility buffer initialization failed: " << stage << '\n';
            return false;
        };
        if (!require(CreateVisBufferRT(), "visibility target")) return false;
        if (!require(CreateOutputTexture(), "output textures")) return false;
        if (!require(CreateColorLUT(), "colour LUT")) return false;
        if (!require(CreateLensTexture(
                "Content/Textures/Lens/lens_dirt_pack.png", "Lens dirt",
                lensDirtTexture, lensDirtUpload), "lens dirt texture"))
            return false;
        if (!require(CreateLensTexture(
                "Content/Textures/Lens/sun_flare_mask.png", "Sun flare",
                sunFlareTexture, sunFlareUpload), "sun flare texture"))
            return false;
        if (!require(CreateLensTexture(
                "Content/Textures/Lens/lens_ghost_mask.png", "Lens ghost",
                lensGhostTexture, lensGhostUpload), "lens ghost texture"))
            return false;
        if (!require(CreateStructuredBuffers(), "structured buffers")) return false;
        if (!require(CreateComputeDescriptorHeap(), "compute descriptors")) return false;
        {
            BootTimer::Scope step("VB: visibility pass shaders");
            if (!require(CreateVisPassPipeline(), "visibility shaders")) return false;
        }
        if (deferResolvePipeline) {
            // Cold boot: the resolve permutations are still on the shader
            // compile workers. Everything else is built; the menu comes up and
            // FinishDeferredResolvePipeline runs once the compiles land.
            resolvePipelineDeferred = true;
            BootTimer::Log("VB: resolve shaders deferred until compiled");
        } else {
            BootTimer::Scope step("VB: resolve shaders (all variants)");
            if (!require(CreateResolvePipeline(), "resolve shader")) return false;
        }
        // Best-effort: not wrapped in require(), because the split resolve is
        // correct without classification -- just full-screen.
        {
            BootTimer::Scope step("VB: tile classify + bloom shaders");
            if (CreateTileClassifyPipeline()) CreateTileClassifyResources();
            if (!require(CreateBloomPipeline(), "bloom pyramid shaders")) return false;
        }
        BootTimer::Scope postStep("VB: flare, post, exposure shaders");
        // Best-effort: the scene renders correctly without a flare, so a
        // missing or broken flare shader must not take the renderer down.
        if (!CreateFlarePipeline())
            std::cerr << "Lens flare pass unavailable (non-fatal)\n";
        if (!require(CreatePostPipeline(), "post-process shader")) return false;
        if (!require(CreateExposurePipeline(), "exposure shaders")) return false;

        if (!require(frameConstantBuffer.Create(FRAME_COUNT * RenderViewCountDX12), "frame constants")) return false;
        if (!require(impactDecalsBuffer.Create(FRAME_COUNT * RenderViewCountDX12), "impact decals")) return false;
        if (!require(postConstantBuffer.Create(FRAME_COUNT), "post constants")) return false;
        if (!require(exposureConstantBuffer.Create(FRAME_COUNT), "exposure constants")) return false;

        cpuDrawCalls.resize(VB_MAX_DRAW_CALLS);
        cpuVertices.resize(geometryVertexCapacity);
        cpuIndices.resize(geometryIndexCapacity);
        cpuStableTriangleIDs.resize(geometryTriangleCapacity);
        cpuClusters.resize(VB_CLUSTER_COUNT);
        previousModels.resize(VB_MAX_DRAW_CALLS);
        if (mappedMaterials) mappedMaterials[0] = VBMaterialData{};

        initialized = true;
        std::cout << "Visibility Buffer initialized (" << width << "x" << height << ")" << std::endl;
        return true;
    }

    void BeginFrame() {
        // Returns quarantined geometry ranges to the free list once every
        // in-flight frame has finished reading them.
        if (!ScopeSurfaceBound()) geometryPool.BeginFrame();
        // Slot indices released during the last frame's update are safe to
        // reuse now: this frame's draw calls have not been recorded yet, and
        // the previous frame's are done referencing them.
        if (!retiredMeshSlots.empty()) {
            freeMeshSlots.insert(freeMeshSlots.end(),
                retiredMeshSlots.begin(), retiredMeshSlots.end());
            retiredMeshSlots.clear();
        }
        terrainProjectionValid = false;
        if (previousModelByInstance.size() >
            static_cast<size_t>(VB_MAX_DRAW_CALLS) * 4u)
            previousModelByInstance.clear();
        previousDrawCount = currentDrawCall;
        currentDrawCall = 0;
        drawCallDirtyMin = UINT_MAX;
        drawCallDirtyMax = 0;
    }

    void SetCluster(UINT clusterIndex, UINT lightCount, const int* lightIndices) {
        if (clusterIndex >= cpuClusters.size()) return;
        VBClusterData& cluster = cpuClusters[clusterIndex];
        cluster.lightCount = (std::min)(lightCount, VB_MAX_LIGHTS_PER_CLUSTER);
        for (UINT i = 0; i < cluster.lightCount; ++i)
            cluster.lightIndices[i] = static_cast<UINT>(lightIndices[i]);
    }

    UINT RegisterMaterial(const SceneMaterial* material) {
        if (!material) return 0;
        const auto updateParameters = [material](VBMaterialData& data) {
            data.baseColorFactor = material->baseColorFactor;
            data.emissiveOcclusion.x = material->emissiveFactor.x;
            data.emissiveOcclusion.y = material->emissiveFactor.y;
            data.emissiveOcclusion.z = material->emissiveFactor.z;
            data.emissiveOcclusion.w = material->occlusionStrength;
            data.pbrParams = XMFLOAT4(material->metallicFactor,
                material->roughnessFactor, material->normalYSign, 1.0f);
            data.shadingParams = XMFLOAT4(material->ambientScale,
                material->viewFillStrength,
                material->foliageShading ? material->foliageShadowLift : 0.7f, 0.0f);
        };
        auto found = materialLookup.find(material);
        if (found != materialLookup.end()) {
            if (mappedMaterials)
                updateParameters(mappedMaterials[found->second]);
            return found->second;
        }
        if (materialCount >= VB_MAX_LEGACY_MATERIALS) return 0;

        VBMaterialData data;
        updateParameters(data);
        auto addTexture = [&](ID3D12Resource* texture) -> UINT {
            if (!texture) return UINT_MAX;
            auto foundTexture = materialTextureLookup.find(texture);
            if (foundTexture != materialTextureLookup.end())
                return foundTexture->second;
            if (materialTextureCount >= VB_MAX_MATERIAL_TEXTURES) {
                // The array is full. The material still registers, it just
                // renders untextured -- a silent quality loss that looks like
                // an authoring mistake rather than a capacity limit, so count
                // the rejects and surface them in the UI.
                materialTexturesRejected.insert(texture);
                return UINT_MAX;
            }
            const UINT textureIndex = materialTextureCount++;
            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = texture;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            g_dx12.commandList->ResourceBarrier(1, &barrier);
            if (bindlessHeap)
                bindlessHeap->MarkTextureComputeReadable(texture);
            UINT descriptorSize = g_dx12.cbvSrvUavDescriptorSize;
            D3D12_CPU_DESCRIPTOR_HANDLE handle =
                computeDescHeap->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += (SIZE_T)descriptorSize * (8 + textureIndex);
            D3D12_RESOURCE_DESC resource = texture->GetDesc();
            D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
            srv.Format = resource.Format;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = resource.MipLevels;
            g_dx12.device->CreateShaderResourceView(texture, &srv, handle);
            if (ScopeSurfaceBound()) {
                auto primary = scopeView->computeDescHeap->GetCPUDescriptorHandleForHeapStart();
                primary.ptr += static_cast<SIZE_T>(descriptorSize) * (8 + textureIndex);
                g_dx12.device->CopyDescriptorsSimple(1, primary, handle,
                    D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            }
            materialTextureLookup.emplace(texture, textureIndex);
            return textureIndex;
        };
        data.textureIndices[0] = addTexture(material->baseColorTexture.Get());
        data.textureIndices[1] = addTexture(material->normalTexture.Get());
        data.textureIndices[2] = addTexture(material->metallicRoughnessTexture.Get());
        data.textureIndices[3] = material->roughnessOnlyTexture ? 1u : 0u;

        const UINT id = materialCount++;
        mappedMaterials[id] = data;
        materialLookup.emplace(material, id);
        return id;
    }

    // Bindless counterpart to RegisterMaterial. Writes absolute bindless heap
    // indices into textureIndices instead of 0..63 table slots, and caches them
    // on the material so repeat draws are a hash lookup rather than three
    // descriptor registrations.
    //
    // `heap` must outlive the frame. Returns 0 (the default material) when
    // bindless is unavailable so callers never need a null check.
    UINT RegisterMaterialBindless(SceneMaterial* material,
                                  BindlessHeapDX12& heap) {
        if (!material || !heap.Initialized() || !mappedBindlessMaterials) return 0;

        const auto updateParameters = [this, material](VBMaterialData& data) {
            data.baseColorFactor = material->baseColorFactor;
            data.emissiveOcclusion.x = material->emissiveFactor.x;
            data.emissiveOcclusion.y = material->emissiveFactor.y;
            data.emissiveOcclusion.z = material->emissiveFactor.z;
            data.emissiveOcclusion.w = material->occlusionStrength;
            data.pbrParams = XMFLOAT4(material->metallicFactor,
                material->roughnessFactor, material->normalYSign, 1.0f);
            data.shadingParams = XMFLOAT4(material->ambientScale,
                material->viewFillStrength,
                material->foliageShading ? material->foliageShadowLift : 0.7f, 0.0f);
            // Zero preserves the old constant-emission path. Index+1 keeps
            // absence distinct from a valid bindless slot zero.
            if (texturedEmissionEnabled && material->emissiveTexture &&
                material->bindlessEmissiveIndex != BINDLESS_INVALID_INDEX)
                data.shadingParams.w = float(material->bindlessEmissiveIndex + 1u);
        };

        // The material's cached indices are only meaningful for the allocator
        // generation that issued them. After a scene reset the generation moves
        // on and every surviving material re-registers here.
        const UINT generation = heap.Allocator().Generation();
        if (material->bindlessGeneration != generation) {
            material->InvalidateTextureBindings();
            material->bindlessGeneration = generation;
            const auto needsComputeTransition = [this](ID3D12Resource* texture) {
                return texture && materialTextureLookup.find(texture) ==
                    materialTextureLookup.end();
            };
            material->bindlessAlbedoIndex =
                heap.RegisterTexture(material->baseColorTexture.Get(),
                    BINDLESS_FALLBACK_WHITE,
                    needsComputeTransition(material->baseColorTexture.Get()));
            material->bindlessNormalIndex =
                heap.RegisterTexture(material->normalTexture.Get(),
                    BINDLESS_FALLBACK_NORMAL,
                    needsComputeTransition(material->normalTexture.Get()));
            material->bindlessMetalRoughIndex =
                heap.RegisterTexture(material->metallicRoughnessTexture.Get(),
                    BINDLESS_FALLBACK_METALROUGH,
                    needsComputeTransition(
                        material->metallicRoughnessTexture.Get()));
            material->bindlessEmissiveIndex = heap.RegisterTexture(
                material->emissiveTexture.Get(), BINDLESS_FALLBACK_BLACK,
                needsComputeTransition(material->emissiveTexture.Get()));
        }
        if (texturedEmissionEnabled && material->emissiveTexture)
            material->bindlessEmissiveIndex = heap.RegisterTexture(
                material->emissiveTexture.Get(), BINDLESS_FALLBACK_BLACK, true);

        auto found = bindlessMaterialLookup.find(material);
        if (found != bindlessMaterialLookup.end()) {
            const UINT recordIndex =
                ViewFrameIndex() * VB_MAX_MATERIALS +
                found->second;
            VBMaterialData& record = mappedBindlessMaterials[recordIndex];
            updateParameters(record);
            // Live-tunable material edits can swap a texture mid-session, so
            // refresh the indices too rather than trusting the first write.
            record.textureIndices[0] = material->bindlessAlbedoIndex;
            record.textureIndices[1] = material->bindlessNormalIndex;
            record.textureIndices[2] = material->bindlessMetalRoughIndex;
            // This is material state too, not immutable texture identity. Every
            // frame slot needs it: leaving a newly visited slot at zero makes a
            // packed roughness-only map's B=0 channel erase metallic for that
            // frame, so the surface alternates between metallic and diffuse.
            record.textureIndices[3] = material->roughnessOnlyTexture ? 1u : 0u;
            return found->second;
        }

        if (bindlessMaterialCount >= VB_MAX_MATERIALS) return 0;

        VBMaterialData data;
        updateParameters(data);
        data.textureIndices[0] = material->bindlessAlbedoIndex;
        data.textureIndices[1] = material->bindlessNormalIndex;
        data.textureIndices[2] = material->bindlessMetalRoughIndex;
        data.textureIndices[3] = material->roughnessOnlyTexture ? 1u : 0u;

        const UINT id = bindlessMaterialCount++;
        const UINT recordIndex =
            ViewFrameIndex() * VB_MAX_MATERIALS + id;
        mappedBindlessMaterials[recordIndex] = data;
        bindlessMaterialLookup.emplace(material, id);
        return id;
    }

    UINT RegisterMaterialForCurrentPath(SceneMaterial* material) {
        if (bindlessActive && bindlessHeap && BindlessResolveReady())
            return RegisterMaterialBindless(material, *bindlessHeap);
        return RegisterMaterial(material);
    }

    void SetBindlessHeap(BindlessHeapDX12* heap) { bindlessHeap = heap; }
    void SetBindlessActive(bool active) { bindlessActive = active; }
    bool BindlessActive() const { return bindlessActive; }
    bool BindlessVisPassActive() const {
        return bindlessActive && BindlessResolveReady() && bindlessHeap &&
               bindlessHeap->Initialized();
    }
    bool BindlessResolveReady() const {
        return bindlessResolveReady && bindlessVisPassReady;
    }
    bool BindlessEnhancedResolveReady() const {
        return bindlessEnhancedResolveReady;
    }
    UINT BindlessMaterialCount() const { return bindlessMaterialCount; }
    bool BindlessTransientOverflowed() const {
        return bindlessTransientOverflowLastFrame;
    }
    bool PrepareBindlessFrame(UINT frameSlot, bool enabled) {
        const UINT slot = frameSlot % FRAME_COUNT;
        bindlessResolveTableBases[slot] = BINDLESS_INVALID_INDEX;
        bindlessTransientOverflowLastFrame = false;
        if (!enabled || !bindlessHeap || !bindlessHeap->Initialized())
            return false;
        // Reserve for the largest resolve tier; BuildBindlessResolveTable then
        // copies either the standard or enhanced prefix into this range.
        bindlessResolveTableBases[slot] =
            bindlessHeap->Allocator().AllocateTransient(
                kEnhancedResolveDescriptorCount);
        bindlessTransientOverflowLastFrame =
            bindlessResolveTableBases[slot] == BINDLESS_INVALID_INDEX;
        return !bindlessTransientOverflowLastFrame;
    }
    bool BindlessFrameReady(UINT frameSlot) const {
        return bindlessResolveTableBases[frameSlot % FRAME_COUNT] !=
            BINDLESS_INVALID_INDEX;
    }
    // ---- Terrain in the visibility buffer ----

    // Manual toggle. Flipping it invalidates temporal history: the same pixels
    // switch between the forward and resolve shading paths, and any residual
    // history across that boundary shows up as a smear.
    void SetTerrainVisibilityRequested(bool requested) {
        if (terrainVisibilityRequested == requested) return;
        terrainVisibilityRequested = requested;
        InvalidateTemporalHistory();
    }
    bool TerrainVisibilityRequested() const {
        return terrainVisibilityRequested;
    }

    // Route destruction chunks through the visibility buffer instead of
    // re-drawing them in the forward extensions pass. Same history caveat as
    // terrain: the affected pixels change shading path, so any carried-over
    // temporal history smears across the switch.
    void SetDestructionVisibilityRequested(bool requested) {
        if (destructionVisibilityRequested == requested) return;
        destructionVisibilityRequested = requested;
        InvalidateTemporalHistory();
    }
    bool DestructionVisibilityRequested() const {
        return destructionVisibilityRequested;
    }

    // True only when every prerequisite holds: the toggle is on, terrain
    // supplied its layer arrays, and the tier this frame will actually resolve
    // on has a terrain PSO.
    //
    // The renderer uses this to decide whether to skip the forward terrain
    // draw, so it has to agree exactly with the PSO choice inside Resolve --
    // hence the shared TerrainResolvePSOForTier lookup and the tier
    // predicates duplicated from Resolve's own selection. Disagreement in
    // either direction is a visible bug: terrain drawn twice, or missing.
    bool TerrainVisibilityReady() const {
        if (!terrainVisibilityRequested) return false;
        if (!terrainAlbedoArray || !terrainNormalArray ||
            !terrainMetalRoughArray) return false;
        // Mirrors Resolve exactly, including the enhanced qualifier: with
        // enhanced on, the bindless tier is chosen only when its *enhanced*
        // variant is ready, otherwise Resolve falls back to enhanced-only.
        // Dropping that clause here would let this claim a tier Resolve does
        // not select, and terrain would disappear rather than double-draw.
        bool willUseBindless = false, willUseEnhanced = false;
        SelectedResolveTier(willUseBindless, willUseEnhanced);
        // Both halves of the split dispatch are required. A tier with only the
        // generic half would rasterize terrain IDs that nothing ever shades,
        // leaving terrain-shaped holes; forward terrain is the correct fallback.
        return TerrainResolvePSOForTier(willUseBindless, willUseEnhanced) !=
                   nullptr &&
               TerrainOnlyResolvePSOForTier(willUseBindless, willUseEnhanced) !=
                   nullptr;
    }

    // Terrain layer arrays, owned by TerrainRendererDX12. Descriptors are
    // written once and reused; the arrays are created at load and never
    // reallocated, so there is no per-frame descriptor work here.
    void SetTerrainTextures(ID3D12Resource* albedo, ID3D12Resource* normal,
                            ID3D12Resource* metalRough,
                            ID3D12Resource* splat = nullptr) {
        if (terrainAlbedoArray == albedo && terrainNormalArray == normal &&
            terrainMetalRoughArray == metalRough && terrainSplatMap == splat)
            return;
        terrainAlbedoArray = albedo;
        terrainNormalArray = normal;
        terrainMetalRoughArray = metalRough;
        terrainSplatMap = splat;
        terrainDescriptorsWritten = false;
    }

    // Half the island's XZ extent, matching the terrain mesh. Kept separate
    // from the texture setter because it changes with terrain params rather
    // than with the painted texture, and it must not force a descriptor
    // rewrite: descriptors describe the resource, not the world mapping.
    void SetTerrainSplatExtent(float halfExtentX, float halfExtentZ) {
        terrainSplatExtentX = halfExtentX;
        terrainSplatExtentZ = halfExtentZ;
    }

    void SetTerrainMaterialParams(float materialType, float normalYSign) {
        terrainMaterialType = materialType;
        terrainNormalYSign = normalYSign;
    }

    // The projection terrain actually rasterized with this frame. Recorded by
    // the raster pass so Resolve can invert the same matrix; without it the
    // resolve would reconstruct terrain from the jittered projection that the
    // draw-call geometry used, offsetting terrain by the TAA jitter.
    void SetTerrainProjection(const XMMATRIX& projection) {
        terrainProjection = projection;
        terrainProjectionValid = true;
    }

    // Terrain deformation changes the surface under otherwise-static pixels, so
    // the accumulated history no longer describes what is there.
    void NotifyTerrainDeformed() { InvalidateTemporalHistory(); }

    // The terrain PSO belonging to one resolve tier, or null if that tier has
    // none. Single source of truth for the tier lookup: Resolve() uses it to
    // pick the PSO and TerrainVisibilityReady() uses it to decide whether the
    // forward terrain draw can be skipped. Two copies of this mapping would
    // eventually disagree, and disagreement means terrain drawn twice or not
    // at all.
    ID3D12PipelineState* TerrainResolvePSOForTier(bool bindless,
                                                  bool enhanced) const {
        if (bindless)
            return enhanced ? bindlessEnhancedTerrainResolvePSO.Get()
                            : bindlessTerrainResolvePSO.Get();
        return enhanced ? enhancedTerrainResolvePSO.Get()
                        : terrainResolvePSO.Get();
    }

    // Terrain-only half of the same tier. Kept beside the lookup above so the
    // two halves can never be mapped to different tiers.
    ID3D12PipelineState* TerrainOnlyResolvePSOForTier(bool bindless,
                                                      bool enhanced) const {
        if (bindless)
            return enhanced ? bindlessEnhancedTerrainOnlyResolvePSO.Get()
                            : bindlessTerrainOnlyResolvePSO.Get();
        return enhanced ? enhancedTerrainOnlyResolvePSO.Get()
                        : terrainOnlyResolvePSO.Get();
    }

    // Tile-classified twins of the two lookups above. Null means this tier has
    // no classified variant, so the split runs full-screen on it.
    ID3D12PipelineState* TerrainResolveTiledPSOForTier(bool bindless,
                                                       bool enhanced) const {
        if (bindless)
            return enhanced ? bindlessEnhancedTerrainResolveTiledPSO.Get()
                            : bindlessTerrainResolveTiledPSO.Get();
        return enhanced ? enhancedTerrainResolveTiledPSO.Get()
                        : terrainResolveTiledPSO.Get();
    }

    ID3D12PipelineState* TerrainOnlyResolveTiledPSOForTier(bool bindless,
                                                           bool enhanced) const {
        if (bindless)
            return enhanced ? bindlessEnhancedTerrainOnlyResolveTiledPSO.Get()
                            : bindlessTerrainOnlyResolveTiledPSO.Get();
        return enhanced ? enhancedTerrainOnlyResolveTiledPSO.Get()
                        : terrainOnlyResolveTiledPSO.Get();
    }

    // ---- Resolve permutations: what boot builds, what waits for use ----
    //
    // Measured cold boot (2026-10-02, 6C/12T): 21 resolve permutations, 673 s
    // of compile CPU across 11 workers, 86 s wall -- and the two slowest
    // (FXC terrain-only, 86 s each under load) belong to a tier an RT session
    // never selects. A frame only uses one tier's terrain set, so boot builds
    // the base PSO of every tier (tier selection gates on those) plus the
    // terrain set of the tier the settings predict. Any other tier's set is
    // compiled in the background the first time that tier is selected; until
    // it lands, that tier's terrain lookups return null and terrain draws
    // forward -- the existing fallback for a missing terrain PSO.
    enum ResolveTier : int {
        ResolveTierFXC = 0,
        ResolveTierEnhanced,
        ResolveTierBindless,
        ResolveTierBindlessEnhanced,
        ResolveTierCount
    };
    static int ResolveTierIndex(bool bindless, bool enhanced) {
        if (bindless)
            return enhanced ? ResolveTierBindlessEnhanced : ResolveTierBindless;
        return enhanced ? ResolveTierEnhanced : ResolveTierFXC;
    }
    static const char* ResolveTierName(int tier) {
        static const char* names[ResolveTierCount] = {
            "default", "enhanced", "bindless", "bindless enhanced" };
        return tier >= 0 && tier < ResolveTierCount ? names[tier] : "?";
    }

    // Which tier the next frame resolves on. Mirrors Resolve's selection,
    // including the enhanced qualifier on bindless: with enhanced on, the
    // bindless tier is chosen only when its enhanced variant is ready.
    void SelectedResolveTier(bool& bindless, bool& enhanced) const {
        enhanced = enhancedVisualsActive && enhancedPipelineReady &&
                   enhancedResolvePSO;
        bindless = bindlessActive && BindlessResolveReady() &&
                   (!enhanced || BindlessEnhancedResolveReady()) &&
                   bindlessHeap && bindlessHeap->Initialized();
    }

    // Set before Init on a cold boot: Init skips the resolve pipeline, and the
    // renderer must not select the visibility path until ResolvePipelineReady.
    bool deferResolvePipeline = false;
    bool ResolvePipelineReady() const {
        return !resolvePipelineDeferred && resolvePSO;
    }

    // Builds the resolve pipeline Init skipped. Main thread, outside any
    // command-list recording; with the compiles done these are cache hits plus
    // driver PSO builds. On failure the visibility path stays unavailable.
    bool FinishDeferredResolvePipeline() {
        if (!resolvePipelineDeferred) return ResolvePipelineReady();
        resolvePipelineDeferred = false;
        BootTimer::Scope step("VB: resolve shaders (all variants, deferred)");
        if (!CreateResolvePipeline()) {
            std::cerr << "Visibility resolve pipeline failed; staying forward\n";
            resolvePSO.Reset();
            return false;
        }
        return true;
    }

    // Boot hint from the player settings (RT quality, Lumen, RR, bindless),
    // set before Init. A wrong guess costs a background compile, not a bug.
    void SetPredictedResolveTier(bool enhanced, bool bindless) {
        predictedResolveEnhanced = enhanced;
        predictedResolveBindless = bindless;
    }

    // Queues every boot-critical resolve permutation on the shader compile
    // workers. Called before Init so the compiles run while the boot screen
    // shows progress; Init then reads them from the cache.
    void QueueBootResolveCompiles() {
        if (!PrepareResolveSources()) return;
        const auto fxc = [](const std::string& source) {
            ShaderCacheDX12::SubmitFXC(source, "shaders/visbuf_resolve_cs.hlsl",
                                       "main", "cs_5_1", ResolveFxcFlags(), 0);
        };
        const std::wstring directory =
            ShaderCacheDX12::ExecutableDirectory() + L"shaders";
        const auto dxc = [&directory](int tier, const std::string& source) {
            ShaderCacheDX12::SubmitDXC(source, L"visbuf_resolve_cs.hlsl",
                                       L"main", ResolveTierProfile(tier),
                                       directory);
        };
        fxc(resolveBaseSource);
        fxc(resolveSourceVSM);
        const bool dxcAvailable = ShaderCacheDX12::DxcAvailable();
        const bool bindlessSupported = bindlessHeap && bindlessHeap->Supported();
        if (dxcAvailable) {
            dxc(ResolveTierEnhanced, ResolveTierSource(ResolveTierEnhanced));
            if (bindlessSupported) {
                dxc(ResolveTierBindless, ResolveTierSource(ResolveTierBindless));
                dxc(ResolveTierBindlessEnhanced,
                    ResolveTierSource(ResolveTierBindlessEnhanced));
            }
        }
        const int tier = ResolveTierIndex(
            predictedResolveBindless && dxcAvailable && bindlessSupported,
            predictedResolveEnhanced && dxcAvailable);
        std::string terrain[4];
        TerrainSetSources(ResolveTierSource(tier), terrain);
        for (const std::string& source : terrain) {
            if (tier == ResolveTierFXC) fxc(source);
            else dxc(tier, source);
        }
    }

    // Installs finished background terrain sets and requests the set of the
    // tier now in use. Main thread, once per frame, before anything asks
    // TerrainVisibilityReady -- so the forward-terrain decision and Resolve
    // always see the same PSOs within a frame.
    void PumpDeferredResolvePipelines() {
        for (int tier = 0; tier < ResolveTierCount; ++tier) {
            std::future<TerrainSetBuild>& pending = terrainSetPending[tier];
            if (!pending.valid() ||
                pending.wait_for(std::chrono::seconds(0)) !=
                    std::future_status::ready)
                continue;
            TerrainSetBuild build = pending.get();
            InstallTerrainSet(tier, build);
            BootTimer::Log(std::string("Terrain resolve variants ready (") +
                           ResolveTierName(tier) + ")" +
                           (build.pso[0] ? "" : " -- unavailable, terrain stays forward"));
        }
        // A tier must want terrain for a second of consecutive resolves before
        // it earns a build: the first frames of a level can pass through a
        // tier (enhanced not yet active, say) that the session never settles on.
        if (!initialized || missingTerrainTierFrames < 60) return;
        const int tier = missingTerrainTier;
        if (tier < 0 || tier >= ResolveTierCount || terrainSetRequested[tier])
            return;
        terrainSetRequested[tier] = true;
        ID3D12RootSignature* rootSig = ResolveTierRootSig(tier);
        if (!rootSig) return;
        BootTimer::Log(std::string("Terrain resolve variants (") +
                       ResolveTierName(tier) + ") requested; compiling in background");
        // A detached thread holding its own references, so quitting mid-compile
        // neither blocks on it nor leaves it reading a destroyed renderer.
        auto promise = std::make_shared<std::promise<TerrainSetBuild>>();
        terrainSetPending[tier] = promise->get_future();
        std::thread([promise, tier, source = ResolveTierSource(tier),
                     root = ComPtr<ID3D12RootSignature>(rootSig),
                     device = g_dx12.device] {
            promise->set_value(BuildTerrainSet(tier, source, root, device, true));
        }).detach();
    }

    void SetBentNormalGTAOHistory(ID3D12Resource* history,
                                 bool requested, bool historyValid) {
        bentNormalGTAOHistory = history;
        bentNormalGTAORequested = requested;
        bentNormalGTAOHistoryValid = historyValid && history;
    }
    bool BentNormalGTAOAppliedLastResolve() const {
        return bentNormalGTAOAppliedLastResolve;
    }
    bool BentNormalGTAODiagnosticActive() const {
        return bentNormalGTAODebugMode != BentNormalGTAODebugMode::Lit;
    }
    void FlushBindlessTextureTransitions(ID3D12GraphicsCommandList* cmdList) {
        if (bindlessActive && bindlessHeap)
            bindlessHeap->FlushTextureTransitions(cmdList);
    }

    // Drops bindless material records so they re-register against the new
    // allocator generation. Called at scene teardown, after the GPU is idle.
    void ResetBindlessMaterials() {
        bindlessMaterialLookup.clear();
        bindlessMaterialCount = 1;
        if (mappedBindlessMaterials) {
            for (UINT frame = 0; frame < FRAME_COUNT; ++frame)
                mappedBindlessMaterials[frame * VB_MAX_MATERIALS] =
                    VBMaterialData{};
        }
    }

    // Upload-once mesh registration. Instances reference this immutable geometry
    // every frame instead of duplicating vertices per draw.
    // Caller owns the level-load GPU drain. Build every replacement first so
    // an allocation failure keeps the previous pool and registrations usable.
    bool SetGeometryCapacity(UINT vertices, UINT indices, UINT triangles) {
        if (vertices == geometryVertexCapacity && indices == geometryIndexCapacity &&
            triangles == geometryTriangleCapacity) return true;
        if (!initialized || ScopeSurfaceBound() || !vertices || !indices || !triangles)
            return false;
        std::array<ComPtr<ID3D12Resource>, 3> buffers;
        std::array<std::array<ComPtr<ID3D12Resource>, FRAME_COUNT>, 3> uploads, scopeUploads;
        const UINT64 bytes[3] = { UINT64(vertices) * sizeof(VBPackedVertex),
            UINT64(indices) * sizeof(UINT), UINT64(triangles) * sizeof(UINT) };
        auto create = [](UINT64 size, D3D12_HEAP_TYPE type,
                         ComPtr<ID3D12Resource>& resource) {
            D3D12_HEAP_PROPERTIES heap{}; heap.Type = type;
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width = size; desc.Height = 1; desc.DepthOrArraySize = 1;
            desc.MipLevels = 1; desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            return SUCCEEDED(g_dx12.device->CreateCommittedResource(&heap,
                D3D12_HEAP_FLAG_NONE, &desc, type == D3D12_HEAP_TYPE_UPLOAD
                    ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST,
                nullptr, IID_PPV_ARGS(&resource)));
        };
        for (UINT b = 0; b < 3; ++b) {
            if (!create(bytes[b], D3D12_HEAP_TYPE_DEFAULT, buffers[b])) return false;
            for (UINT f = 0; f < FRAME_COUNT; ++f) {
                if (!create(bytes[b], D3D12_HEAP_TYPE_UPLOAD, uploads[b][f])) return false;
                if (scopeView && !create(bytes[b], D3D12_HEAP_TYPE_UPLOAD, scopeUploads[b][f]))
                    return false;
            }
        }
        std::vector<VBPackedVertex> vertexMirror(vertices);
        std::vector<UINT> indexMirror(indices), triangleMirror(triangles);
        vertexDataBuffer = std::move(buffers[0]);
        indexDataBuffer = std::move(buffers[1]);
        stableTriangleDataBuffer = std::move(buffers[2]);
        for (UINT f = 0; f < FRAME_COUNT; ++f) {
            vertexDataUpload[f] = std::move(uploads[0][f]);
            indexDataUpload[f] = std::move(uploads[1][f]);
            stableTriangleDataUpload[f] = std::move(uploads[2][f]);
            if (scopeView) {
                scopeView->vertexDataUpload[f] = std::move(scopeUploads[0][f]);
                scopeView->indexDataUpload[f] = std::move(scopeUploads[1][f]);
                scopeView->stableTriangleDataUpload[f] = std::move(scopeUploads[2][f]);
            }
        }
        cpuVertices.swap(vertexMirror); cpuIndices.swap(indexMirror);
        cpuStableTriangleIDs.swap(triangleMirror);
        geometryVertexCapacity = vertices; geometryIndexCapacity = indices;
        geometryTriangleCapacity = triangles;
        geometryPool = VisibilityGeometryPool(vertices, indices, triangles, FRAME_COUNT);
        primitiveMeshLookup.clear(); meshes.clear(); freeMeshSlots.clear();
        retiredMeshSlots.clear(); transientMeshSlots.clear();
        persistentVertexCount = persistentIndexCount = persistentTriangleCount = 0;
        persistentAuthoredTriangleCount = 0; geometryRegistrationFailures = 0;
        geometryDirty = geometryUploaded = false;
        dirtyVertices.Clear(); dirtyIndices.Clear(); dirtyTriangles.Clear();
        previousModelByInstance.clear(); previousDrawCount = currentDrawCall = 0;
        hitGeometryCount = 0;
        ++geometryGeneration;
        InvalidateTemporalHistory();
        UpdateComputeDescriptors(); UpdatePostDescriptors();
        for (UINT f = 0; f < FRAME_COUNT; ++f) RefreshEnhancedDescriptors(f);
        if (scopeView) {
            SwapScopeViewStorage();
            UpdateComputeDescriptors(); UpdatePostDescriptors();
            for (UINT f = 0; f < FRAME_COUNT; ++f) RefreshEnhancedDescriptors(f);
            PatchScopeDescriptors();
            SwapScopeViewStorage();
        }
        return true;
    }

    UINT RegisterMesh(const float* vertexData, UINT vertexCount,
                      const UINT* indexData, UINT indexCount,
                      UINT vertexStrideFloats = 8,
                      const UINT* stableTriangleIDs = nullptr,
                      UINT stableTriangleCount = 0,
                      UINT stableTriangleNamespace = 0,
                      bool transient = false) {
        const UINT triangleCount = indexData && indexCount > 0
            ? indexCount / 3u : vertexCount / 3u;
        if (!vertexData || vertexCount == 0 ||
            (stableTriangleIDs && stableTriangleCount != triangleCount))
            return VB_INVALID_MESH;

        VBGeometryRange range;
        if (!geometryPool.Allocate(vertexCount, indexCount, triangleCount,
                                   range)) {
            ++geometryRegistrationFailures;
            return VB_INVALID_MESH;
        }
        // The upload path copies a contiguous prefix, so track the furthest
        // extent the pool has handed out.
        persistentVertexCount = geometryPool.VertexHighWater();
        persistentIndexCount = geometryPool.IndexHighWater();
        persistentTriangleCount = geometryPool.TriangleHighWater();

        const UINT vertexOffset = range.vertexOffset;
        const UINT indexOffset = range.indexOffset;
        const UINT triangleOffset = range.triangleOffset;

        VBMeshData mesh;
        mesh.vertexOffset = vertexOffset;
        mesh.vertexCount = vertexCount;
        mesh.indexOffset = indexOffset;
        mesh.indexCount = indexCount;
        mesh.hasIndices = (indexData && indexCount > 0) ? 1u : 0u;
        mesh.stableTriangleOffset = triangleOffset;
        mesh.stableTriangleNamespace = stableTriangleNamespace;
        mesh.vertexCapacity = range.vertexCapacity;
        mesh.indexCapacity = range.indexCapacity;
        mesh.triangleCapacity = range.triangleCapacity;

        for (UINT i = 0; i < vertexCount; ++i) {
            const float* v = vertexData + i * vertexStrideFloats;
            VBPackedVertex& pv = cpuVertices[vertexOffset + i];
            pv.d0 = XMFLOAT4(v[0], v[1], v[2], v[3]);
            pv.d1 = XMFLOAT4(v[4], v[5], v[6], v[7]);
        }
        if (mesh.hasIndices)
            memcpy(cpuIndices.data() + indexOffset, indexData,
                   indexCount * sizeof(UINT));
        for (UINT triangle = 0; triangle < triangleCount; ++triangle) {
            cpuStableTriangleIDs[triangleOffset + triangle] =
                stableTriangleIDs ? stableTriangleIDs[triangle] : triangle;
        }

        if (stableTriangleIDs)
            persistentAuthoredTriangleCount += triangleCount;

        UINT meshID;
        if (!freeMeshSlots.empty()) {
            meshID = freeMeshSlots.back();
            freeMeshSlots.pop_back();
            meshes[meshID] = mesh;
        } else {
            meshes.push_back(mesh);
            meshID = static_cast<UINT>(meshes.size() - 1);
        }
        if (transient) transientMeshSlots.insert(meshID);
        else transientMeshSlots.erase(meshID);
        dirtyVertices.Add(vertexOffset, vertexCount);
        if (mesh.hasIndices) dirtyIndices.Add(indexOffset, indexCount);
        dirtyTriangles.Add(triangleOffset, triangleCount);
        geometryDirty = true;
        return meshID;
    }

    // Hand a transient mesh's storage back for reuse. Only meshes registered
    // with transient=true are recyclable: permanent scene geometry keeps its
    // slot for the lifetime of the level, and a stale draw call referencing a
    // recycled permanent slot would sample another mesh's vertices.
    void ReleaseMesh(UINT meshID) {
        if (meshID == VB_INVALID_MESH || meshID >= meshes.size()) return;
        if (transientMeshSlots.find(meshID) == transientMeshSlots.end()) return;
        const VBMeshData& mesh = meshes[meshID];
        // The GPU may still be reading this range for frames already in flight,
        // so the pool quarantines it until enough frames have retired.
        VBGeometryRange range;
        range.vertexOffset = mesh.vertexOffset;
        range.vertexCapacity = mesh.vertexCapacity;
        range.indexOffset = mesh.indexOffset;
        range.indexCapacity = mesh.indexCapacity;
        range.triangleOffset = mesh.stableTriangleOffset;
        range.triangleCapacity = mesh.triangleCapacity;
        geometryPool.Release(range);
        transientMeshSlots.erase(meshID);
        // Zero the record so a draw call that survives one frame too long
        // renders nothing rather than reading a half-overwritten range.
        meshes[meshID] = VBMeshData{};
        // The slot INDEX needs the same quarantine as the storage range. This
        // runs from destruction's update, which is before the frame's
        // BeginFrame -- handing the index straight back would let a different
        // primitive claim it this frame while draw calls recorded for the
        // previous frame still name it, so their geometry would swap under the
        // GPU. Released here, reusable a full frame later.
        retiredMeshSlots.push_back(meshID);
    }

    UINT64 GeometryRegistrationFailures() const {
        return geometryRegistrationFailures;
    }

    // Pool occupancy, for diagnosing registration failures. Storage that keeps
    // climbing across destruction rebuilds means ranges are being stranded
    // rather than recycled; a free-range count that climbs alongside it means
    // fragmentation instead.
    UINT GeometryVertexHighWater() const {
        return geometryPool.VertexHighWater();
    }
    size_t GeometryFreeRangeCount() const {
        return geometryPool.FreeRangeCount();
    }

    // Drop a primitive's registration and recycle its storage. Called by the
    // destruction system when it retires a merged batch node.
    void ReleasePrimitive(MeshPrimitive* primitive) {
        if (!primitive) return;
        auto found = primitiveMeshLookup.find(primitive);
        if (found == primitiveMeshLookup.end()) return;
        ReleaseMesh(found->second);
        primitiveMeshLookup.erase(found);
        primitive->visibilityMeshID = VB_INVALID_MESH;
    }

    UINT RegisterPrimitive(MeshPrimitive* primitive) {
        if (!primitive || primitive->vertices.empty()) return VB_INVALID_MESH;
        auto found = primitiveMeshLookup.find(primitive);
        if (found != primitiveMeshLookup.end()) {
            // Spatial batches replace their merged SceneNode after a cell
            // changes. The allocator can reuse the old MeshPrimitive address,
            // but a freshly constructed primitive has not been registered yet.
            // Do not bind the recycled address to the old material bucket's
            // geometry.
            if (primitive->visibilityMeshID == found->second)
                return found->second;
            primitiveMeshLookup.erase(found);
        }
        const UINT mesh = RegisterMesh(primitive->vertices.data(),
            static_cast<UINT>(primitive->vertices.size() / 12),
            primitive->indices.empty() ? nullptr : primitive->indices.data(),
            static_cast<UINT>(primitive->indices.size()), 12,
            primitive->stableTriangleIDs.empty()
                ? nullptr : primitive->stableTriangleIDs.data(),
            static_cast<UINT>(primitive->stableTriangleIDs.size()),
            primitive->stableTriangleNamespace,
            primitive->transientGeometry);
        if (mesh != VB_INVALID_MESH) {
            primitiveMeshLookup.emplace(primitive, mesh);
            primitive->visibilityMeshID = mesh;
        }
        return mesh;
    }

    // Persistent geometry residency for a registered mesh.
    //
    // Exposed for the raytracing hit path: the TLAS is built from
    // MeshPrimitive::vertexBuffer while the resolve reads the global packed
    // buffers addressed by these offsets, so a ray hit needs this to reach the
    // same triangle. Safe to snapshot at acceleration-structure build time
    // because RegisterMesh is upload-once for permanent geometry -- those
    // offsets never move for the lifetime of a mesh.
    //
    // Transient meshes are explicitly refused. Destruction recycles their slots
    // and storage as fractures rebuild the merged batches, so an offset
    // snapshotted here would go stale and point a ray hit at whatever geometry
    // later claimed the range. Those rays keep the sky approximation instead,
    // which is the same fallback a not-yet-registered primitive already gets.
    bool MeshGeometryBinding(UINT meshID, UINT& vertexOffset,
                             UINT& indexOffset, UINT& hasIndices) const {
        if (meshID == VB_INVALID_MESH || meshID >= meshes.size()) return false;
        if (transientMeshSlots.find(meshID) != transientMeshSlots.end())
            return false;
        const VBMeshData& mesh = meshes[meshID];
        vertexOffset = mesh.vertexOffset;
        indexOffset = mesh.indexOffset;
        hasIndices = mesh.hasIndices;
        return true;
    }

    // Material slot a SceneMaterial already occupies, without registering one.
    // Returns false before the material has been seen by RegisterMaterial,
    // which is the common case during an early acceleration-structure build.
    bool ExistingMaterialID(const SceneMaterial* material, UINT& id) const {
        const auto found = materialLookup.find(material);
        if (found == materialLookup.end()) return false;
        id = found->second;
        return true;
    }

    // Register both material record encodings used by enhanced RayQuery hit
    // shading.  The hit-geometry table is scene-lived while the renderer can
    // switch between legacy and bindless resolves every frame, so storing only
    // whichever ID happened to be active when the TLAS was built makes the
    // other mode interpret texture indices in the wrong address space.
    void RegisterRaytracingMaterial(SceneMaterial* material,
                                    UINT& legacyID,
                                    UINT& bindlessID) {
        legacyID = RegisterMaterial(material);
        bindlessID = 0;
        if (material && bindlessHeap && bindlessHeap->Initialized() &&
            BindlessResolveReady())
            bindlessID = RegisterMaterialBindless(material, *bindlessHeap);
    }

    // Register only mutable instance/material data for this frame.
    UINT RegisterInstance(UINT meshID, const XMMATRIX& modelMatrix,
                          const XMFLOAT3& color, float metalness, float roughness,
                          UINT materialID = 0, UINT flags = 0,
                          uint64_t instanceKey = 0,
                          XMFLOAT4 palmWindRoot = {}) {
        if (currentDrawCall >= VB_MAX_DRAW_CALLS || meshID >= meshes.size())
            return UINT_MAX;

        UINT dcID = currentDrawCall;
        VBDrawCallData& dc = cpuDrawCalls[dcID];
        VBDrawCallData next = {};
        const VBMeshData& mesh = meshes[meshID];

        XMMATRIX transposed = XMMatrixTranspose(modelMatrix);
        XMStoreFloat4x4(&next.modelMatrix, transposed);
        const bool motionVectorsRequired = MotionVectorsRequired();
        auto previous = motionVectorsRequired && instanceKey
            ? previousModelByInstance.find(instanceKey)
            : previousModelByInstance.end();
        if (!motionVectorsRequired) {
            next.previousModelMatrix = next.modelMatrix;
        } else if (previous != previousModelByInstance.end()) {
            next.previousModelMatrix = previous->second;
        } else if (instanceKey == 0 && dcID < previousDrawCount) {
            next.previousModelMatrix = previousModels[dcID];
        } else {
            next.previousModelMatrix = next.modelMatrix;
        }
        previousModels[dcID] = next.modelMatrix;
        if (motionVectorsRequired && instanceKey)
            previousModelByInstance[instanceKey] = next.modelMatrix;

        next.objectColor = color;
        // These two legacy float fields are unused by the resolve. Preserve
        // the cbuffer layout, but carry the enhanced-only stable surface map
        // metadata in their exact uint bit patterns.
        UINT stableNamespace = mesh.stableTriangleNamespace;
        if (stableNamespace == 0u) {
            uint64_t identity = instanceKey != 0
                ? instanceKey
                : (static_cast<uint64_t>(meshID + 1u) << 32u) |
                      static_cast<uint64_t>(materialID + 1u);
            stableNamespace = static_cast<UINT>(identity ^ (identity >> 32u));
            if (stableNamespace == 0u) stableNamespace = 1u;
        }
        memcpy(&next.useTexture, &mesh.stableTriangleOffset, sizeof(UINT));
        next.metalness = metalness;
        next.roughness = roughness;
        memcpy(&next.useNormalMap, &stableNamespace, sizeof(UINT));
        next.materialID = materialID;
        next.vertexOffset = mesh.vertexOffset;
        next.indexOffset = mesh.indexOffset;
        next.indexCount = mesh.indexCount;
        next.hasIndices = mesh.hasIndices;
        next.flags = flags;
        next.palmWindRoot = palmWindRoot;
        if (dcID >= previousDrawCount ||
            memcmp(&dc, &next, sizeof(VBDrawCallData)) != 0) {
            dc = next;
            drawCallDirtyMin = (std::min)(drawCallDirtyMin, dcID);
            drawCallDirtyMax = (std::max)(drawCallDirtyMax, dcID);
        }

        currentDrawCall++;
        return dcID;
    }

    // Upload all CPU-side data to GPU before the resolve pass
    void UploadBuffers(ID3D12GraphicsCommandList* cmdList) {
        const UINT frameSlot = g_dx12.frameIndex % FRAME_COUNT;

        // Upload draw calls
        if (drawCallDirtyMin != UINT_MAX) {
            const UINT64 offset = static_cast<UINT64>(drawCallDirtyMin) *
                sizeof(VBDrawCallData);
            const UINT64 size = static_cast<UINT64>(
                drawCallDirtyMax - drawCallDirtyMin + 1) *
                sizeof(VBDrawCallData);
            void* mapped = nullptr;
            D3D12_RANGE readRange = { 0, 0 };
            // Map fails under memory pressure and leaves `mapped` null; the
            // memcpy would then write straight to address 0. Skip just this
            // upload rather than returning -- the geometry barriers below still
            // have to run, and the dirty range is not cleared here, so the
            // draw calls missed this frame are re-sent on the next one.
            if (SUCCEEDED(drawCallUpload[frameSlot]->Map(0, &readRange,
                                                         &mapped)) && mapped) {
                memcpy(static_cast<uint8_t*>(mapped) + offset,
                    cpuDrawCalls.data() + drawCallDirtyMin, size);
                drawCallUpload[frameSlot]->Unmap(0, nullptr);

                cmdList->CopyBufferRegion(drawCallBuffer.Get(), offset,
                    drawCallUpload[frameSlot].Get(), offset, size);
            }
        }

        // Geometry changes only when a mesh is added. DEFAULT buffers stay SRVs
        // between frames, eliminating per-instance vertex/index uploads.
        if (geometryDirty && geometryUploaded) {
            D3D12_RESOURCE_BARRIER geometryToCopy[3] = {};
            for (UINT i = 0; i < 3; ++i) {
                geometryToCopy[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                geometryToCopy[i].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                geometryToCopy[i].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                geometryToCopy[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            }
            geometryToCopy[0].Transition.pResource = vertexDataBuffer.Get();
            geometryToCopy[1].Transition.pResource = indexDataBuffer.Get();
            geometryToCopy[2].Transition.pResource =
                stableTriangleDataBuffer.Get();
            cmdList->ResourceBarrier(3, geometryToCopy);
        }


        // CPU cluster construction already exists in ClusteredRendererDX12.
        // Upload its compact light lists instead of scanning every light per pixel.
        {
            void* mapped = nullptr;
            D3D12_RANGE readRange = { 0, 0 };
            if (SUCCEEDED(clusterDataUpload[frameSlot]->Map(0, &readRange,
                                                            &mapped)) &&
                mapped) {
                memcpy(mapped, cpuClusters.data(),
                       cpuClusters.size() * sizeof(VBClusterData));
                clusterDataUpload[frameSlot]->Unmap(0, nullptr);
                cmdList->CopyBufferRegion(clusterDataBuffer.Get(), 0,
                    clusterDataUpload[frameSlot].Get(), 0,
                    cpuClusters.size() * sizeof(VBClusterData));
            }
        }

        // A geometry upload that could not be mapped leaves its DEFAULT buffer
        // holding the previous frame's contents and, more importantly, never
        // transitioned into COPY_DEST. The barrier block below is keyed off
        // geometryDirty, so this flag keeps the pass dirty: the transitions are
        // skipped for a frame and the dirty spans are kept and retried on the
        // next one, rather than promoting buffers that were never written.
        bool geometryUploadFailed = false;

        // Copies [span.begin, span.end) of one CPU mirror into its DEFAULT
        // buffer through this frame's upload buffer, at the same offset in all
        // three. Only freshly allocated ranges are ever in a span, and the pool
        // quarantines released ranges, so no in-flight frame reads them.
        auto uploadSpan = [&](const DirtySpan& span, const void* source,
                              UINT elementSize, ID3D12Resource* upload,
                              ID3D12Resource* destination) {
            if (span.Empty()) return;
            const UINT64 offset = UINT64(span.begin) * elementSize;
            const UINT64 size = UINT64(span.end - span.begin) * elementSize;
            void* mapped = nullptr;
            D3D12_RANGE readRange = { 0, 0 };
            if (SUCCEEDED(upload->Map(0, &readRange, &mapped)) && mapped) {
                memcpy(static_cast<uint8_t*>(mapped) + offset,
                       static_cast<const uint8_t*>(source) + offset, size);
                D3D12_RANGE written = { SIZE_T(offset), SIZE_T(offset + size) };
                upload->Unmap(0, &written);
                cmdList->CopyBufferRegion(destination, offset, upload, offset,
                                          size);
            } else {
                geometryUploadFailed = true;
            }
        };
        if (geometryDirty) {
            uploadSpan(dirtyVertices, cpuVertices.data(), sizeof(VBPackedVertex),
                       vertexDataUpload[frameSlot].Get(), vertexDataBuffer.Get());
            uploadSpan(dirtyIndices, cpuIndices.data(), sizeof(UINT),
                       indexDataUpload[frameSlot].Get(), indexDataBuffer.Get());
            uploadSpan(dirtyTriangles, cpuStableTriangleIDs.data(), sizeof(UINT),
                       stableTriangleDataUpload[frameSlot].Get(),
                       stableTriangleDataBuffer.Get());
        }

        // Barriers: transition structured buffers from copy dest to SRV
        D3D12_RESOURCE_BARRIER barriers[5] = {};
        barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[0].Transition.pResource = drawCallBuffer.Get();
        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

        barriers[1] = barriers[0];
        barriers[1].Transition.pResource = clusterDataBuffer.Get();

        UINT barrierCount = 2;
        if (geometryDirty && !geometryUploadFailed) {
            barriers[2] = barriers[0];
            barriers[2].Transition.pResource = vertexDataBuffer.Get();
            barriers[3] = barriers[0];
            barriers[3].Transition.pResource = indexDataBuffer.Get();
            barriers[4] = barriers[0];
            barriers[4].Transition.pResource = stableTriangleDataBuffer.Get();
            barrierCount = 5;
            geometryUploaded = true;
            geometryDirty = false;
            dirtyVertices.Clear();
            dirtyIndices.Clear();
            dirtyTriangles.Clear();
        }
        cmdList->ResourceBarrier(barrierCount, barriers);
    }

    // Transition structured buffers back to copy dest for next frame
    void TransitionBuffersForUpload(ID3D12GraphicsCommandList* cmdList) {
        D3D12_RESOURCE_BARRIER barriers[2] = {};
        barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[0].Transition.pResource = drawCallBuffer.Get();
        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

        barriers[1] = barriers[0];
        barriers[1].Transition.pResource = clusterDataBuffer.Get();
        cmdList->ResourceBarrier(2, barriers);
    }

    // Begin the visibility pass: clear VB RT, set render targets
    void BeginVisibilityPass(ID3D12GraphicsCommandList* cmdList) {
        // Transition vis buffer to render target
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = visBufferRT.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &barrier);

        // Zero means background. Stored instance IDs are biased by one.
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = visRtvHeap->GetCPUDescriptorHandleForHeapStart();
        const float clearValue[4] = {};
        cmdList->ClearRenderTargetView(rtvHandle, clearValue, 0, nullptr);

        // Also clear main depth
        D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = ActiveDSV();
        cmdList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

        // Set render targets: vis buffer + main depth buffer
        cmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

        // Set viewport / scissor
        cmdList->RSSetViewports(1, &ActiveViewport());
        cmdList->RSSetScissorRects(1, &ActiveScissor());

        g_dx12.device->CopyDescriptorsSimple(kResolveDescriptorCount,
            RasterViewHeap()->GetCPUDescriptorHandleForHeapStart(),
            computeDescHeap->GetCPUDescriptorHandleForHeapStart(),
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        BindVisPassPipeline(cmdList);
    }

    // Root signature, heaps, bindings and default PSO shared by the visibility
    // pass and its post-RR depth replay. The descriptor copy above stays with
    // the pass: it is a CPU write, and repeating it mid-frame would change
    // what the already-recorded raster reads when the GPU runs it.
    void BindVisPassPipeline(ID3D12GraphicsCommandList* cmdList) {
        // Set pipeline
        const bool useBindless = BindlessVisPassActive();
        ID3D12DescriptorHeap* heaps[] = {
            useBindless ? bindlessHeap->Heap() : RasterViewHeap()
        };
        cmdList->SetDescriptorHeaps(1, heaps);
        cmdList->SetGraphicsRootSignature(useBindless
            ? bindlessVisPassRootSig.Get() : visPassRootSig.Get());
        BindImpactDecalCutouts(cmdList);
        cmdList->SetGraphicsRootShaderResourceView(3,
            drawCallBuffer->GetGPUVirtualAddress());
        D3D12_GPU_DESCRIPTOR_HANDLE defaultTexture = useBindless
            ? bindlessHeap->GpuHandleAt(BINDLESS_FALLBACK_WHITE)
            : RasterViewHeap()->GetGPUDescriptorHandleForHeapStart();
        if (!useBindless)
            defaultTexture.ptr += static_cast<UINT64>(
                g_dx12.cbvSrvUavDescriptorSize) * 8u;
        cmdList->SetGraphicsRootDescriptorTable(2, defaultTexture);
        cmdList->SetPipelineState(useBindless
            ? bindlessVisPassPSO.Get() : visPassPSO.Get());
    }

    // Post-RR depth replay. The visibility pass rasterises with the jittered
    // projection; RR hands back colour with that jitter resolved, but every
    // pass after it (forward extensions, grass, particles, water, fog, GTAO,
    // SSR, sun lens, next frame's HZB) depth-tests or samples the jittered
    // depth with unjittered matrices, so their silhouettes moved by the jitter
    // every frame. The jitter is a pure screen-space offset (it is added to the
    // projection's clip-space x/y in proportion to w), so re-rasterising the
    // same recorded draws into a viewport shifted by -jitter produces the
    // unjittered depth without touching any matrix or culled command stream.
    // IDs go to a scratch target: the post pass still reads visBufferRT.
    bool EnsureDepthSeedPipeline() {
        if (depthSeedPSO) return true;
        if (depthSeedTried) return false;
        depthSeedTried = true;
        std::ifstream file("shaders/visbuf_depth_seed.hlsl");
        if (!file.is_open()) return false;
        std::stringstream source;
        source << file.rdbuf();
        const std::string code = source.str();
        UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
        ComPtr<ID3DBlob> vsBlob, psBlob, errorBlob;
        if (FAILED(ShaderCacheDX12::CompileCached(code.c_str(), code.length(),
                "shaders/visbuf_depth_seed.hlsl", nullptr,
                D3D_COMPILE_STANDARD_FILE_INCLUDE, "VSMain", "vs_5_0", flags,
                0, &vsBlob, &errorBlob)) ||
            FAILED(ShaderCacheDX12::CompileCached(code.c_str(), code.length(),
                "shaders/visbuf_depth_seed.hlsl", nullptr,
                D3D_COMPILE_STANDARD_FILE_INCLUDE, "PSMain", "ps_5_0", flags,
                0, &psBlob, &errorBlob))) {
            if (errorBlob)
                std::cerr << "Depth seed shader error: "
                          << (char*)errorBlob->GetBufferPointer() << std::endl;
            return false;
        }
        D3D12_DESCRIPTOR_RANGE range = {};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 2;
        D3D12_ROOT_PARAMETER param = {};
        param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        param.DescriptorTable.NumDescriptorRanges = 1;
        param.DescriptorTable.pDescriptorRanges = &range;
        param.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
        rootDesc.NumParameters = 1;
        rootDesc.pParameters = &param;
        ComPtr<ID3DBlob> sigBlob;
        errorBlob.Reset();
        if (FAILED(D3D12SerializeRootSignature(&rootDesc,
                D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errorBlob)) ||
            FAILED(g_dx12.device->CreateRootSignature(0,
                sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(),
                IID_PPV_ARGS(&depthSeedRootSig))))
            return false;
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso = {};
        pso.pRootSignature = depthSeedRootSig.Get();
        pso.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
        pso.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
        pso.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso.RasterizerState.DepthClipEnable = TRUE;
        pso.BlendState.RenderTarget[0].RenderTargetWriteMask =
            D3D12_COLOR_WRITE_ENABLE_ALL;
        pso.DepthStencilState.DepthEnable = TRUE;
        pso.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        pso.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        pso.SampleMask = UINT_MAX;
        pso.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso.NumRenderTargets = 0;
        pso.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        pso.SampleDesc.Count = 1;
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NumDescriptors = 2 * FRAME_COUNT;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(g_dx12.device->CreateGraphicsPipelineState(
                &pso, IID_PPV_ARGS(&depthSeedPSO))) ||
            FAILED(g_dx12.device->CreateDescriptorHeap(
                &heapDesc, IID_PPV_ARGS(&depthSeedHeap)))) {
            depthSeedPSO.Reset();
            depthSeedHeap.Reset();
            return false;
        }
        return true;
    }

    // Writes terrain's jittered depth (and adjacent-terrain fill for object
    // pixels) into the bound depth buffer; see visbuf_depth_seed.hlsl. Depth
    // must be bound for writing and visibilityDepthTexture must still hold the
    // visibility pass's depth.
    void SeedDepthReplay(ID3D12GraphicsCommandList* cmdList,
                         D3D12_CPU_DESCRIPTOR_HANDLE dsv) {
        D3D12_RESOURCE_BARRIER barriers[2] = {};
        for (UINT i = 0; i < 2; ++i) {
            barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[i].Transition.pResource = i == 0
                ? visBufferRT.Get() : visibilityDepthTexture.Get();
            barriers[i].Transition.StateBefore =
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            barriers[i].Transition.StateAfter =
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            barriers[i].Transition.Subresource =
                D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        }
        cmdList->ResourceBarrier(2, barriers);

        const UINT stride = g_dx12.cbvSrvUavDescriptorSize;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu =
            depthSeedHeap->GetCPUDescriptorHandleForHeapStart();
        D3D12_GPU_DESCRIPTOR_HANDLE gpu =
            depthSeedHeap->GetGPUDescriptorHandleForHeapStart();
        cpu.ptr += static_cast<SIZE_T>(g_dx12.frameIndex) * 2u * stride;
        gpu.ptr += static_cast<UINT64>(g_dx12.frameIndex) * 2u * stride;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        srv.Format = DXGI_FORMAT_R32G32_UINT;
        g_dx12.device->CreateShaderResourceView(visBufferRT.Get(), &srv, cpu);
        cpu.ptr += stride;
        srv.Format = DXGI_FORMAT_R32_FLOAT;
        g_dx12.device->CreateShaderResourceView(
            visibilityDepthTexture.Get(), &srv, cpu);

        ID3D12DescriptorHeap* heaps[] = { depthSeedHeap.Get() };
        cmdList->SetDescriptorHeaps(1, heaps);
        cmdList->SetGraphicsRootSignature(depthSeedRootSig.Get());
        cmdList->SetPipelineState(depthSeedPSO.Get());
        cmdList->SetGraphicsRootDescriptorTable(0, gpu);
        cmdList->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
        cmdList->RSSetViewports(1, &ActiveViewport());
        cmdList->RSSetScissorRects(1, &ActiveScissor());
        cmdList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmdList->DrawInstanced(3, 1, 0, 0);

        for (UINT i = 0; i < 2; ++i) {
            std::swap(barriers[i].Transition.StateBefore,
                      barriers[i].Transition.StateAfter);
        }
        cmdList->ResourceBarrier(2, barriers);
    }

    bool EnsureRRForwardGuidesPipeline() {
        if (rrForwardGuidesPSO) return true;
        if (rrForwardGuidesTried) return false;
        rrForwardGuidesTried = true;
        std::ifstream file("shaders/rr_forward_guides_cs.hlsl");
        if (!file.is_open()) return false;
        std::stringstream source;
        source << file.rdbuf();
        const std::string code = source.str();
        UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
        ComPtr<ID3DBlob> csBlob, errorBlob;
        if (FAILED(ShaderCacheDX12::CompileCached(code.c_str(), code.length(),
                "shaders/rr_forward_guides_cs.hlsl", nullptr,
                D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "cs_5_0", flags,
                0, &csBlob, &errorBlob))) {
            if (errorBlob)
                std::cerr << "RR forward guides shader error: "
                          << (char*)errorBlob->GetBufferPointer() << std::endl;
            return false;
        }
        D3D12_DESCRIPTOR_RANGE ranges[2] = {};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 5;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 5;
        ranges[1].OffsetInDescriptorsFromTableStart = 5;
        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable.NumDescriptorRanges = 2;
        params[0].DescriptorTable.pDescriptorRanges = ranges;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants.Num32BitValues = 40;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
        rootDesc.NumParameters = 2;
        rootDesc.pParameters = params;
        ComPtr<ID3DBlob> sigBlob;
        errorBlob.Reset();
        if (FAILED(D3D12SerializeRootSignature(&rootDesc,
                D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errorBlob)) ||
            FAILED(g_dx12.device->CreateRootSignature(0,
                sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(),
                IID_PPV_ARGS(&rrForwardGuidesRootSig))))
            return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso = {};
        pso.pRootSignature = rrForwardGuidesRootSig.Get();
        pso.CS = { csBlob->GetBufferPointer(), csBlob->GetBufferSize() };
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NumDescriptors = 10 * FRAME_COUNT;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(g_dx12.device->CreateComputePipelineState(
                &pso, IID_PPV_ARGS(&rrForwardGuidesPSO))) ||
            FAILED(g_dx12.device->CreateDescriptorHeap(
                &heapDesc, IID_PPV_ARGS(&rrForwardGuidesHeap)))) {
            rrForwardGuidesPSO.Reset();
            rrForwardGuidesHeap.Reset();
            return false;
        }
        return true;
    }

    bool EnsureMotionDebugPipeline() {
        if (motionDebugPSO) return true;
        if (motionDebugTried) return false;
        motionDebugTried = true;
        std::ifstream file("shaders/motion_debug_cs.hlsl");
        if (!file.is_open()) return false;
        std::stringstream source;
        source << file.rdbuf();
        const std::string code = source.str();
        UINT flags = D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;
        ComPtr<ID3DBlob> csBlob, errorBlob;
        if (FAILED(ShaderCacheDX12::CompileCached(code.c_str(), code.length(),
                "shaders/motion_debug_cs.hlsl", nullptr,
                D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "cs_5_0", flags,
                0, &csBlob, &errorBlob))) {
            if (errorBlob)
                std::cerr << "Motion debug shader error: "
                          << (char*)errorBlob->GetBufferPointer() << std::endl;
            return false;
        }
        D3D12_DESCRIPTOR_RANGE ranges[2] = {};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 1;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 1;
        ranges[1].OffsetInDescriptorsFromTableStart = 1;
        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[0].DescriptorTable.NumDescriptorRanges = 2;
        params[0].DescriptorTable.pDescriptorRanges = ranges;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[1].Constants.Num32BitValues = 8;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
        rootDesc.NumParameters = 2;
        rootDesc.pParameters = params;
        ComPtr<ID3DBlob> sigBlob;
        errorBlob.Reset();
        if (FAILED(D3D12SerializeRootSignature(&rootDesc,
                D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errorBlob)) ||
            FAILED(g_dx12.device->CreateRootSignature(0,
                sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(),
                IID_PPV_ARGS(&motionDebugRootSig))))
            return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso = {};
        pso.pRootSignature = motionDebugRootSig.Get();
        pso.CS = { csBlob->GetBufferPointer(), csBlob->GetBufferSize() };
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NumDescriptors = 2 * FRAME_COUNT;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(g_dx12.device->CreateComputePipelineState(
                &pso, IID_PPV_ARGS(&motionDebugPSO))) ||
            FAILED(g_dx12.device->CreateDescriptorHeap(
                &heapDesc, IID_PPV_ARGS(&motionDebugHeap)))) {
            motionDebugPSO.Reset();
            motionDebugHeap.Reset();
            return false;
        }
        return true;
    }

    // Paints motionTexture over presentTexture, after PostProcess and before
    // CopyToBackBuffer. Runs after DLSS / RR, so it shows the vectors they
    // were given without switching them off.
    void DrawMotionDebug(ID3D12GraphicsCommandList* cmdList) {
        if (motionDebugMode == 0 || !motionTexture || !presentTexture ||
            !EnsureMotionDebugPipeline())
            return;
        ProfilerDX12::Scope profile(g_profiler, "Motion Debug", cmdList);
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = presentTexture.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &barrier);

        const UINT stride = g_dx12.cbvSrvUavDescriptorSize;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu =
            motionDebugHeap->GetCPUDescriptorHandleForHeapStart();
        D3D12_GPU_DESCRIPTOR_HANDLE gpu =
            motionDebugHeap->GetGPUDescriptorHandleForHeapStart();
        cpu.ptr += static_cast<SIZE_T>(g_dx12.frameIndex) * 2u * stride;
        gpu.ptr += static_cast<UINT64>(g_dx12.frameIndex) * 2u * stride;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        srv.Format = DXGI_FORMAT_R16G16_FLOAT;
        g_dx12.device->CreateShaderResourceView(motionTexture.Get(), &srv, cpu);
        cpu.ptr += stride;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        g_dx12.device->CreateUnorderedAccessView(presentTexture.Get(), nullptr,
                                                 &uav, cpu);

        struct Constants {
            float renderSize[2];
            float displaySize[2];
            UINT mode;
            float arrowGain;
            float tileSize;
            float fullScalePixels;
        } constants = {};
        static_assert(sizeof(Constants) == 8 * 4, "Motion debug constants");
        constants.renderSize[0] = static_cast<float>(width);
        constants.renderSize[1] = static_cast<float>(height);
        constants.displaySize[0] = static_cast<float>(displayWidth);
        constants.displaySize[1] = static_cast<float>(displayHeight);
        constants.mode = motionDebugMode;
        constants.arrowGain = 8.0f;
        constants.tileSize = 32.0f;
        constants.fullScalePixels = 16.0f;

        ID3D12DescriptorHeap* heaps[] = { motionDebugHeap.Get() };
        cmdList->SetDescriptorHeaps(1, heaps);
        cmdList->SetComputeRootSignature(motionDebugRootSig.Get());
        cmdList->SetPipelineState(motionDebugPSO.Get());
        cmdList->SetComputeRootDescriptorTable(0, gpu);
        cmdList->SetComputeRoot32BitConstants(1, 8, &constants, 0);
        cmdList->Dispatch((displayWidth + 7) / 8, (displayHeight + 7) / 8, 1);

        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        cmdList->ResourceBarrier(1, &barrier);
    }

    // Records the motion probes for this frame and harvests the slot's
    // previous use, which BeginFrame's fence wait has already completed.
    // Call with motionTexture in NON_PIXEL_SHADER_RESOURCE.
    void CaptureJitterProbe(ID3D12GraphicsCommandList* cmdList,
                            JitterProbeFrame meta) {
        const UINT slot = g_dx12.frameIndex % FRAME_COUNT;
        ComPtr<ID3D12Resource>& readback = jitterProbeReadback[slot];
        if (readback && jitterProbePending[slot].valid) {
            JitterProbeFrame done = jitterProbePending[slot];
            void* mapped = nullptr;
            D3D12_RANGE range = { 0, kJitterProbeCount * 512u };
            if (SUCCEEDED(readback->Map(0, &range, &mapped)) && mapped) {
                for (UINT i = 0; i < kJitterProbeCount; ++i) {
                    const auto* half = reinterpret_cast<const uint16_t*>(
                        static_cast<const uint8_t*>(mapped) + i * 512u);
                    done.motionUV[i] = {
                        DirectX::PackedVector::XMConvertHalfToFloat(half[0]),
                        DirectX::PackedVector::XMConvertHalfToFloat(half[1]) };
                }
                D3D12_RANGE none = { 0, 0 };
                readback->Unmap(0, &none);
                jitterProbeLatest = done;
            }
        }
        jitterProbePending[slot].valid = false;
        if (!motionTexture || width == 0 || height == 0) return;
        if (!readback) {
            D3D12_HEAP_PROPERTIES heap = {};
            heap.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC desc = {};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width = kJitterProbeCount * 512u;
            desc.Height = 1;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(g_dx12.device->CreateCommittedResource(
                    &heap, D3D12_HEAP_FLAG_NONE, &desc,
                    D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                    IID_PPV_ARGS(&readback))))
                return;
        }
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = motionTexture.Get();
        barrier.Transition.StateBefore =
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &barrier);
        for (UINT i = 0; i < kJitterProbeCount; ++i) {
            const UINT x = (std::min)(width - 1, static_cast<UINT>(
                kJitterProbePositions[i][0] * static_cast<float>(width)));
            const UINT y = (std::min)(height - 1, static_cast<UINT>(
                kJitterProbePositions[i][1] * static_cast<float>(height)));
            meta.probeUV[i] = { (x + 0.5f) / static_cast<float>(width),
                                (y + 0.5f) / static_cast<float>(height) };
            D3D12_TEXTURE_COPY_LOCATION dst = {};
            dst.pResource = readback.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint.Offset = i * 512u;
            dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R16G16_FLOAT, 1, 1, 1,
                                              D3D12_TEXTURE_DATA_PITCH_ALIGNMENT };
            D3D12_TEXTURE_COPY_LOCATION src = {};
            src.pResource = motionTexture.Get();
            src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.SubresourceIndex = 0;
            const D3D12_BOX box = { x, y, 0, x + 1, y + 1, 1 };
            cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
        }
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        cmdList->ResourceBarrier(1, &barrier);
        meta.valid = true;
        meta.renderWidth = width;
        meta.renderHeight = height;
        meta.resolveStripUV = lastResolveStripUV;
        meta.rr = rayReconstructionActive;
        meta.rrUpscale = rayReconstructionUpscaleActive;
        meta.mvMode = rrUpscaleMotionMode;
        jitterProbePending[slot] = meta;
    }
    // DLSS::Evaluate runs after the probe copy; attach what it was handed.
    void AnnotateJitterProbe(const DLSS::EvalDebug& dlss) {
        jitterProbePending[g_dx12.frameIndex % FRAME_COUNT].dlss = dlss;
    }

    // Upscaling RR only, right before it evaluates. `grassDepth` is the grass
    // MSAA combined depth (PIXEL_SHADER_RESOURCE, left there) or null;
    // `grassWindMotion` is GrassMSAADX12's wind motion (NON_PIXEL_SHADER_
    // RESOURCE) or null. The
    // matrices are the ones this frame and the previous one rendered with.
    // Returns false when the pass is unavailable; RR then runs on the
    // resolve's guides alone.
    bool PrepareRRForwardGuides(ID3D12GraphicsCommandList* cmdList,
                                ID3D12Resource* grassDepth,
                                ID3D12Resource* grassWindMotion,
                                const XMMATRIX& viewProjection,
                                const XMMATRIX& previousViewProjection) {
        if (!rrForwardGuidesEnabled ||
            !rayReconstructionUpscaleActive || ScopeSurfaceBound() ||
            !rrDiffuseAlbedo || !rrSpecularAlbedo || !rrSpecularHitDistance)
            return false;
        return PrepareForwardFixup(cmdList, grassDepth, grassWindMotion,
                                   viewProjection, previousViewProjection, true);
    }

    // Super Resolution: the same forward-pixel motion, no RR guides. The grass
    // composite's (2, 2) reactive marker otherwise reaches DLSS as a real
    // vector and every grass pixel loses its history.
    bool PrepareSRForwardMotion(ID3D12GraphicsCommandList* cmdList,
                                ID3D12Resource* grassDepth,
                                ID3D12Resource* grassWindMotion,
                                const XMMATRIX& viewProjection,
                                const XMMATRIX& previousViewProjection) {
        if (!srForwardMotionEnabled || !dlssActive ||
            rayReconstructionActive || ScopeSurfaceBound())
            return false;
        return PrepareForwardFixup(cmdList, grassDepth, grassWindMotion,
                                   viewProjection, previousViewProjection, false);
    }

    bool PrepareForwardFixup(ID3D12GraphicsCommandList* cmdList,
                             ID3D12Resource* grassDepth,
                             ID3D12Resource* grassWindMotion,
                             const XMMATRIX& viewProjection,
                             const XMMATRIX& previousViewProjection,
                             bool writeGuides) {
        if (!EnsureRRForwardGuidesPipeline()) return false;
        ProfilerDX12::Scope profile(g_profiler, writeGuides
            ? "RR Forward Guides" : "DLSS Forward Motion", cmdList);
        // Motion-only binds null views for the guides; they are not written.
        ID3D12Resource* uavs[5] = { motionTexture.Get(),
            normalRoughnessTexture.Get(), rrDiffuseAlbedo.Get(),
            rrSpecularAlbedo.Get(), rrSpecularHitDistance.Get() };
        if (!writeGuides)
            for (UINT i = 1; i < 5; ++i) uavs[i] = nullptr;
        const UINT uavCount = writeGuides ? 5u : 1u;
        D3D12_RESOURCE_BARRIER barriers[6] = {};
        for (UINT i = 0; i < uavCount; ++i) {
            barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[i].Transition.pResource = uavs[i];
            barriers[i].Transition.StateBefore =
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            barriers[i].Transition.StateAfter =
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            barriers[i].Transition.Subresource =
                D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        }
        UINT barrierCount = uavCount;
        if (grassDepth) {
            barriers[barrierCount] = barriers[0];
            barriers[barrierCount].Transition.pResource = grassDepth;
            barriers[barrierCount].Transition.StateBefore =
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            barriers[barrierCount].Transition.StateAfter =
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            ++barrierCount;
        }
        cmdList->ResourceBarrier(barrierCount, barriers);

        const UINT stride = g_dx12.cbvSrvUavDescriptorSize;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu =
            rrForwardGuidesHeap->GetCPUDescriptorHandleForHeapStart();
        D3D12_GPU_DESCRIPTOR_HANDLE gpu =
            rrForwardGuidesHeap->GetGPUDescriptorHandleForHeapStart();
        cpu.ptr += static_cast<SIZE_T>(g_dx12.frameIndex) * 10u * stride;
        gpu.ptr += static_cast<UINT64>(g_dx12.frameIndex) * 10u * stride;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        srv.Format = DXGI_FORMAT_R32_FLOAT;
        ID3D12Resource* depths[3] = { visibilityDepthTexture.Get(),
            ActiveDepthBuffer(), grassDepth ? grassDepth : ActiveDepthBuffer() };
        for (ID3D12Resource* depth : depths) {
            g_dx12.device->CreateShaderResourceView(depth, &srv, cpu);
            cpu.ptr += stride;
        }
        srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        g_dx12.device->CreateShaderResourceView(outputTexture.Get(), &srv, cpu);
        cpu.ptr += stride;
        // Null when grass is not drawn: the shader then reads zero wind.
        srv.Format = DXGI_FORMAT_R16G16_FLOAT;
        g_dx12.device->CreateShaderResourceView(
            grassDepth ? grassWindMotion : nullptr, &srv, cpu);
        cpu.ptr += stride;
        const DXGI_FORMAT uavFormats[5] = { DXGI_FORMAT_R16G16_FLOAT,
            DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT,
            DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16_FLOAT };
        for (UINT i = 0; i < 5; ++i) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            uav.Format = uavFormats[i];
            g_dx12.device->CreateUnorderedAccessView(uavs[i], nullptr, &uav, cpu);
            cpu.ptr += stride;
        }

        struct Constants {
            XMFLOAT4X4 invViewProj;
            XMFLOAT4X4 previousViewProj;
            float resolution[2];
            float writeGuides;
            float padding;
            float motionJitterUV[2];
            float padding2[2];
        } constants = {};
        static_assert(sizeof(Constants) == 40 * 4, "RR guide constants");
        // Same jitter the resolve stripped from its own vectors this frame,
        // so forward and visibility-buffer pixels share one convention.
        constants.motionJitterUV[0] = lastResolveStripUV.x;
        constants.motionJitterUV[1] = lastResolveStripUV.y;
        constants.writeGuides = writeGuides ? 1.0f : 0.0f;
        XMStoreFloat4x4(&constants.invViewProj,
                        XMMatrixInverse(nullptr, viewProjection));
        XMStoreFloat4x4(&constants.previousViewProj, previousViewProjection);
        constants.resolution[0] = static_cast<float>(width);
        constants.resolution[1] = static_cast<float>(height);

        ID3D12DescriptorHeap* heaps[] = { rrForwardGuidesHeap.Get() };
        cmdList->SetDescriptorHeaps(1, heaps);
        cmdList->SetComputeRootSignature(rrForwardGuidesRootSig.Get());
        cmdList->SetPipelineState(rrForwardGuidesPSO.Get());
        cmdList->SetComputeRootDescriptorTable(0, gpu);
        cmdList->SetComputeRoot32BitConstants(1, 40, &constants, 0);
        cmdList->Dispatch((width + 7) / 8, (height + 7) / 8, 1);

        for (UINT i = 0; i < uavCount; ++i)
            std::swap(barriers[i].Transition.StateBefore,
                      barriers[i].Transition.StateAfter);
        cmdList->ResourceBarrier(uavCount, barriers);
        return true;
    }

    // seedTerrain: fill terrain from the visibility pass's depth instead of
    // expecting the caller to replay the terrain draw. Reports whether it did.
    bool BeginDepthReplay(ID3D12GraphicsCommandList* cmdList,
                          const XMFLOAT2& jitterPixels, bool seedTerrain,
                          bool& terrainSeeded) {
        terrainSeeded = false;
        if (ScopeSurfaceBound()) return false;
        if (!depthReplayRT) {
            D3D12_HEAP_PROPERTIES heapProps = {};
            heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC desc = {};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = width;
            desc.Height = height;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = DXGI_FORMAT_R32G32_UINT;
            desc.SampleDesc.Count = 1;
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            D3D12_CLEAR_VALUE clearValue = {};
            clearValue.Format = DXGI_FORMAT_R32G32_UINT;
            D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
            rtvHeapDesc.NumDescriptors = 1;
            rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            if (FAILED(g_dx12.device->CreateCommittedResource(
                    &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                    D3D12_RESOURCE_STATE_RENDER_TARGET, &clearValue,
                    IID_PPV_ARGS(&depthReplayRT))) ||
                FAILED(g_dx12.device->CreateDescriptorHeap(
                    &rtvHeapDesc, IID_PPV_ARGS(&depthReplayRtvHeap)))) {
                depthReplayRT.Reset();
                depthReplayRtvHeap.Reset();
                return false;
            }
            g_dx12.device->CreateRenderTargetView(depthReplayRT.Get(), nullptr,
                depthReplayRtvHeap->GetCPUDescriptorHandleForHeapStart());
        }

        // EndForwardExtensions left depth readable for RR.
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = ActiveDepthBuffer();
        barrier.Transition.StateBefore =
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &barrier);

        const D3D12_CPU_DESCRIPTOR_HANDLE rtv =
            depthReplayRtvHeap->GetCPUDescriptorHandleForHeapStart();
        const D3D12_CPU_DESCRIPTOR_HANDLE dsv = ActiveDSV();
        // The shifted viewport stops rasterising the last column/row on the
        // side it moved away from (measured: the right column and bottom row
        // flickered to far depth whenever the jitter was positive). Keep a
        // one-pixel border of the visibility pass's depth instead of clearing
        // it; the replay still overwrites it wherever it draws nearer.
        if (seedTerrain && EnsureDepthSeedPipeline()) {
            SeedDepthReplay(cmdList, dsv);
            terrainSeeded = true;
        } else {
            const D3D12_RECT& scissor = ActiveScissor();
            const D3D12_RECT interior = { scissor.left + 1, scissor.top + 1,
                                          scissor.right - 1, scissor.bottom - 1 };
            cmdList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0,
                                           1, &interior);
        }
        cmdList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        D3D12_VIEWPORT viewport = ActiveViewport();
        viewport.TopLeftX -= jitterPixels.x;
        viewport.TopLeftY -= jitterPixels.y;
        cmdList->RSSetViewports(1, &viewport);
        cmdList->RSSetScissorRects(1, &ActiveScissor());
        BindVisPassPipeline(cmdList);
        return true;
    }

    // Returns depth to the state BeginForwardExtensions expects.
    void EndDepthReplay(ID3D12GraphicsCommandList* cmdList) {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = ActiveDepthBuffer();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        barrier.Transition.StateAfter =
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &barrier);
        cmdList->RSSetViewports(1, &ActiveViewport());
        SnapshotVisibilityDepth(cmdList);
    }

    void SetVisPassDraw(ID3D12GraphicsCommandList* cmdList, UINT drawCallID,
                        UINT materialID, bool doubleSided, bool alphaCutout,
                        bool alphaFromLuminance) {
        const bool useBindless = BindlessVisPassActive();
        UINT albedoIndex = BINDLESS_FALLBACK_WHITE;
        if (useBindless && materialID < bindlessMaterialCount) {
            const UINT recordIndex =
                ViewFrameIndex() * VB_MAX_MATERIALS + materialID;
            albedoIndex = mappedBindlessMaterials[recordIndex].textureIndices[0];
        }
        const UINT constants[4] = { drawCallID, alphaCutout ? 1u : 0u,
            alphaFromLuminance ? 1u : 0u, albedoIndex };
        cmdList->SetGraphicsRoot32BitConstants(1, 4, constants, 0);

        if (useBindless) {
            cmdList->SetPipelineState(alphaCutout
                ? (doubleSided ? bindlessVisPassAlphaDoubleSidedPSO.Get()
                               : bindlessVisPassAlphaPSO.Get())
                : (doubleSided ? bindlessVisPassDoubleSidedPSO.Get()
                               : bindlessVisPassPSO.Get()));
            return;
        }

        UINT textureIndex = 0;
        if (materialID < materialCount &&
            mappedMaterials[materialID].textureIndices[0] < VB_MAX_MATERIAL_TEXTURES)
            textureIndex = mappedMaterials[materialID].textureIndices[0];
        D3D12_GPU_DESCRIPTOR_HANDLE texture =
            RasterViewHeap()->GetGPUDescriptorHandleForHeapStart();
        texture.ptr += static_cast<UINT64>(g_dx12.cbvSrvUavDescriptorSize) *
            (8u + textureIndex);
        cmdList->SetGraphicsRootDescriptorTable(2, texture);
        cmdList->SetPipelineState(alphaCutout
            ? (doubleSided ? visPassAlphaDoubleSidedPSO.Get()
                           : visPassAlphaPSO.Get())
            : (doubleSided ? visPassDoubleSidedPSO.Get()
                           : visPassPSO.Get()));
    }

    void SetImpactDecals(const std::vector<ImpactDecalDataDX12>& decals,
                         bool cutoutsEnabled) {
        const int count = (int)((decals.size() < 64) ? decals.size() : 64);
        impactDecalsCPU.numDecals = count;
        impactDecalsCPU.cutoutsEnabled = cutoutsEnabled ? 1.0f : 0.0f;
        for (int i = 0; i < count; ++i) impactDecalsCPU.decals[i] = decals[i];
    }

    void BindImpactDecalCutouts(ID3D12GraphicsCommandList* cmdList) {
        if (!impactDecalsBuffer.resource) return;
        impactDecalsBuffer.CopyData(ViewFrameIndex(), impactDecalsCPU);
        cmdList->SetGraphicsRootConstantBufferView(
            4, impactDecalsBuffer.GetGPUAddress(ViewFrameIndex()));
    }

    void SetPalmWindFrame(const PalmWindFrameDX12& frame) {
        palmWindFrame = frame;
    }

    // Set matrices for the current draw (reuses the matrix CBV at slot 0)
    void SetVisPassMatrices(ID3D12GraphicsCommandList* cmdList,
                            const XMMATRIX& model, const XMMATRIX& view,
                            const XMMATRIX& proj, const XMMATRIX& lightSpace,
                            ShaderDX12& matrixSource, UINT drawIndex) {
        // We reuse the existing matrix buffer from ShaderDX12
        UINT bufferIndex = g_dx12.frameIndex * MAX_DRAW_CALLS_PER_FRAME +
            matrixSource.CurrentViewSlot() * ShaderDX12::kDrawCallsPerView + drawIndex;

        MatrixBufferDX12 data = {};
        data.model = XMMatrixTranspose(model);
        data.view = XMMatrixTranspose(view);
        data.projection = XMMatrixTranspose(proj);
        data.lightSpaceMatrix = XMMatrixTranspose(lightSpace);
        data.palmWind = matrixSource.palmWindFrame.wind;
        data.palmPrimary = matrixSource.palmWindFrame.primary;
        data.palmSecondary = matrixSource.palmWindFrame.secondary;
        data.palmPreviousPrimary = matrixSource.palmWindFrame.previousPrimary;
        data.palmPreviousSecondary = matrixSource.palmWindFrame.previousSecondary;
        data.palmParams = matrixSource.palmWindFrame.params;
        matrixSource.matrixBuffer.CopyData(bufferIndex, data);

        cmdList->SetGraphicsRootConstantBufferView(0,
            matrixSource.matrixBuffer.GetGPUAddress(bufferIndex));
    }

    void EndVisibilityPass(ID3D12GraphicsCommandList* cmdList) {
        // Transition vis buffer to SRV for compute
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = visBufferRT.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &barrier);
    }

    // Indirect dispatch for compute resolve
    ComPtr<ID3D12CommandSignature> resolveDispatchSignature;
    ComPtr<ID3D12Resource> resolveDispatchArgsBuffer;
    D3D12_DISPATCH_ARGUMENTS* mappedResolveDispatchArgs = nullptr;

    // ---- Tile classification for the split terrain resolve ----
    //
    // Only built and only run when terrain resolves through the visibility
    // buffer. Without it both halves of the split sweep the whole screen and
    // each pays a load-and-return for every pixel the other owns; with it each
    // half dispatches over its own tile list and is proportional to coverage.
    //
    // Every one of these may be null: classification is best-effort, and
    // TileClassificationReady() gates its use so a failure anywhere falls back
    // to the full-screen dispatch rather than dropping terrain.
    ComPtr<ID3D12RootSignature> tileClassifyRootSig;
    ComPtr<ID3D12PipelineState> tileClassifyPSO;
    ComPtr<ID3D12PipelineState> tileClassifyResetPSO;
    ComPtr<ID3D12DescriptorHeap> tileClassifyDescHeap;
    // Tile lists, one per half. Sized for the full tile grid because a frame
    // where every tile straddles a terrain edge puts every tile in both lists.
    ComPtr<ID3D12Resource> genericTileListBuffer;
    ComPtr<ID3D12Resource> terrainTileListBuffer;
    // Two D3D12_DISPATCH_ARGUMENTS records written by the GPU: [0] generic,
    // [1] terrain. A DEFAULT-heap UAV, unlike the CPU-mapped upload buffer
    // above, so the counts never round-trip through the CPU.
    ComPtr<ID3D12Resource> classifiedDispatchArgsBuffer;
    ComPtr<ID3D12Resource> tileClassifyConstantBuffer;
    uint8_t* mappedTileClassifyConstants = nullptr;
    UINT tileClassifyTilesX = 0;
    UINT tileClassifyTilesY = 0;
    bool tileClassifyReady = false;
    // Set per frame by Resolve() so the profiler overlay can report whether the
    // split actually ran classified or fell back to full-screen.
    bool tileClassifiedLastFrame = false;
    bool tileClassifyPathLogged = false;

    // Run the compute resolve pass
    void Resolve(ID3D12GraphicsCommandList* cmdList,
                 const XMMATRIX& view, const XMMATRIX& proj,
                 const XMMATRIX& lightViewProj,
                 const XMMATRIX& previousViewProj,
                 const XMFLOAT3& cameraPos,
                 float nearPlane, float farPlane,
                 float contactShadowStrength,
                 float contactShadowMaxDistance,
                 bool contactShadowLinearDepth,
                 const LightBufferDX12& lightData,
                 const PointLightsBufferDX12& pointLightData) {
        currentNearPlane = nearPlane;
        currentFarPlane = farPlane;
        const UINT frameSlot = g_dx12.frameIndex % FRAME_COUNT;
        SnapshotLegacyMaterials();
        if (enhancedVisualsActive && enhancedPipelineReady)
            RefreshEnhancedDescriptors(frameSlot);
        ID3D12DescriptorHeap* enhancedDescHeap =
            enhancedComputeDescHeaps[frameSlot].Get();
        const bool useEnhanced = enhancedVisualsActive && enhancedPipelineReady &&
                                 enhancedResolvePSO && enhancedDescHeap;
        enhancedResolveExecutedLastFrame = useEnhanced;
        PrepareStableSurfaceHistory(
            StableSurfaceIdentityRequired(useEnhanced),
            StableSurfaceModeSignature(true, useEnhanced));
        ID3D12DescriptorHeap* standardDescHeap = computeDescHeap.Get();
        bool bentNormalHistoryActive = !ScopeSurfaceBound() && bentNormalGTAORequested &&
            bentNormalGTAOHistoryValid && bentNormalGTAOHistory &&
            !validationMode && debugViewMode == 0;
        if (bentNormalHistoryActive && !useEnhanced) {
            ID3D12DescriptorHeap* bentHeap =
                PrepareBentNormalResolveHeap(frameSlot);
            if (bentHeap)
                standardDescHeap = bentHeap;
            else
                bentNormalHistoryActive = false;
        }
        if (useEnhanced) {
            WriteBentNormalHistoryDescriptor(enhancedDescHeap, 99,
                bentNormalHistoryActive ? bentNormalGTAOHistory : nullptr);
        }
        bentNormalGTAOAppliedLastResolve = bentNormalHistoryActive;
        // Everything from here to the first dispatch: resource transitions for
        // the resolve's inputs and outputs, plus the frame-constant upload.
        // Scoped because these barriers force the depth buffer out of
        // DEPTH_WRITE and several render targets into UAV, which on a tiled GPU
        // means a real flush -- cost that otherwise showed up only as the gap
        // between "VB Resolve" and the dispatches nested inside it.
        std::optional<ProfilerDX12::Scope> setupScope;
        setupScope.emplace(g_profiler, "VB Resolve Setup", cmdList);
        if (bentNormalHistoryActive) {
            D3D12_RESOURCE_BARRIER historyBarrier = {};
            historyBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            historyBarrier.Transition.pResource = bentNormalGTAOHistory;
            historyBarrier.Transition.StateBefore =
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            historyBarrier.Transition.StateAfter =
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            historyBarrier.Transition.Subresource =
                D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            cmdList->ResourceBarrier(1, &historyBarrier);
        }
        // Transition HDR, motion-vector, and surface outputs to UAV.
        {
            D3D12_RESOURCE_BARRIER barriers[3] = {};
            for (UINT i = 0; i < 3; ++i) {
                barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barriers[i].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                barriers[i].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                barriers[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            }
            barriers[0].Transition.pResource = outputTexture.Get();
            barriers[1].Transition.pResource = motionTexture.Get();
            barriers[2].Transition.pResource = normalRoughnessTexture.Get();
            cmdList->ResourceBarrier(3, barriers);
        }
        if (rayReconstructionActive) {
            ID3D12Resource* guides[] = { rrDiffuseAlbedo.Get(),
                rrSpecularAlbedo.Get(), rrSpecularHitDistance.Get() };
            D3D12_RESOURCE_BARRIER barriers[3] = {};
            for (UINT i = 0; i < 3; ++i) {
                barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barriers[i].Transition.pResource = guides[i];
                barriers[i].Transition.StateBefore =
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                barriers[i].Transition.StateAfter =
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                barriers[i].Transition.Subresource =
                    D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            }
            cmdList->ResourceBarrier(3, barriers);
        }

        // Also transition depth buffer to SRV for reading
        {
            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = ActiveDepthBuffer();
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            cmdList->ResourceBarrier(1, &barrier);
        }

        // Upload frame constants
        VBFrameConstants fc = {};
        fc.viewMatrix = XMMatrixTranspose(view);
        fc.projMatrix = XMMatrixTranspose(proj);
        XMMATRIX invVP = XMMatrixInverse(nullptr, view * proj);
        fc.invViewProj = XMMatrixTranspose(invVP);
        const XMMATRIX terrainProj = terrainProjectionValid
            ? terrainProjection : proj;
        fc.terrainInvViewProj = XMMatrixTranspose(
            XMMatrixInverse(nullptr, view * terrainProj));
        for (UINT i = 0; i < SHADOW_CASCADE_COUNT; ++i)
            fc.shadowCascadeMatrices[i] = XMMatrixTranspose(g_shadowCascadeMatrices[i]);
        fc.previousViewProj = XMMatrixTranspose(previousViewProj);
        fc.shadowCascadeSplits = g_shadowCascadeSplits;
        fc.virtualShadows = g_virtualShadowConstants;
        fc.cameraPos = cameraPos;
        fc.screenWidth = (float)width;
        fc.screenHeight = (float)height;
        fc.nearPlane = nearPlane;
        fc.farPlane = farPlane;
        fc.debugViewMode = static_cast<UINT>(debugViewMode);
        const bool motionVectorsRequired = MotionVectorsRequired();
        fc.enableMotionVectors = motionVectorsRequired ? 1u : 0u;
        svgfMotionVectorsEnabledLastFrame =
            motionVectorsRequired && enhancedVisualsActive &&
            enhancedRTReflectionsActive && svgfTemporalEnabled;
        // RR replaces edge AA on the main view only; the scope has no DLSS.
        fc.edgeAAEnabled = (ScopeSurfaceBound() ||
            (!rayReconstructionActive && edgeAAEnabled)) ? 1u : 0u;
        fc.contactShadowStrength = contactShadowStrength;
        fc.contactShadowMaxDistance = contactShadowMaxDistance;
        fc.contactShadowLinearDepth = contactShadowLinearDepth ? 1u : 0u;
        fc.contactShadowNoiseFrame = (temporalEffectsEnabled || dlssActive)
            ? postFrameIndex : 0u;
        fc.bentNormalGTAOEnabled = bentNormalHistoryActive ? 1u : 0u;
        const UINT bentDebugMode = static_cast<UINT>(
            bentNormalGTAODebugMode);
        fc.bentNormalGTAOFlags = bentNormalHistoryActive
            ? 1u | ((bentDebugMode & 3u) << 1u) : 0u;
        fc.palmWind = palmWindFrame.wind;
        fc.palmPrimary = palmWindFrame.primary;
        fc.palmSecondary = palmWindFrame.secondary;
        fc.palmPreviousPrimary = palmWindFrame.previousPrimary;
        fc.palmPreviousSecondary = palmWindFrame.previousSecondary;
        fc.palmParams = palmWindFrame.params;
        // Terrain constants are written unconditionally: the default resolve
        // does not declare these cbuffer fields, so the bytes are simply
        // ignored there, and the terrain variant always finds them populated.
        fc.terrainMaterialType = terrainMaterialType;
        fc.terrainNormalYSign = terrainNormalYSign;
        fc.terrainVisibilityEnabled =
            terrainVisibilityActiveThisFrame ? 1u : 0u;
        // Painted weights only apply where a splatmap exists and the island has
        // a real extent; a zero extent would map every texel to the same UV.
        const bool splatUsable = terrainSplatMap != nullptr &&
            terrainSplatExtentX > 1e-4f && terrainSplatExtentZ > 1e-4f;
        fc.terrainSplatEnabled = splatUsable ? 1u : 0u;
        fc.terrainSplatInvExtent = splatUsable
            ? XMFLOAT2(0.5f / terrainSplatExtentX, 0.5f / terrainSplatExtentZ)
            : XMFLOAT2(0.0f, 0.0f);
        // The screen-space pass has already intersected the relief. Applying
        // texture-space POM here would displace it a second time.
        fc.terrainPOMEnabled = terrainPOM && !terrainScreenDisplacementActiveThisFrame ? 1u : 0u;
        fc.terrainNeutralHeightBlendMask = terrainNeutralHeightBlendMask;
        frameConstantBuffer.CopyData(ViewFrameIndex(), fc);

        // A toggle can leave old history describing samples from a different
        // mode. Invalidate before uploading b5 so the shader sees the reset on
        // the first frame after the transition.
        if (!ScopeSurfaceBound() && svgfTemporalEnabled != svgfTemporalEnabledLastFrame) {
            svgfHistoryValid = false;
            svgfTemporalEnabledLastFrame = svgfTemporalEnabled;
        }

        // Four selections: legacy lit, legacy enhanced, bindless lit, bindless
        // enhanced. Bindless requires the material table to have been populated
        // through the bindless path this frame, so the flag is the renderer's
        // per-frame decision rather than the raw scene toggle.
        bool useBindless = bindlessActive && BindlessResolveReady() &&
                           (!useEnhanced || BindlessEnhancedResolveReady()) &&
                           bindlessHeap && bindlessHeap->Initialized();
        // The scope must not reproject against the main view's history. The
        // SVGF history textures are one set shared by every view, so what they
        // hold is main-camera radiance at main-camera screen positions -- and
        // a 15-degree frustum agrees with that camera on no pixel. Telling the
        // shader the history is invalid makes the lens shade from scratch,
        // which is exactly what a view with no history of its own should do.
        //
        // Saved and restored around the pass rather than assigned, because the
        // main view resolves later in the same frame and its own history is
        // still perfectly good.
        const bool savedSVGFHistoryValid = svgfHistoryValid;
        if (ScopeSurfaceBound()) svgfHistoryValid = false;
        // The cache is used only once its refresh pass exists: that pass is
        // what clears and evicts, so a table without it would never let go of
        // a stale entry. The clear runs on the main view, before its resolve.
        giRadianceCacheMode = 0;
        if (useEnhanced && lumenGIActive && giRadianceCacheRequested &&
            giRadianceCache.EnsureBuffer() &&
            giRadianceCache.EnsurePipeline(
                ResolveTierSource(useBindless ? ResolveTierBindlessEnhanced
                                              : ResolveTierEnhanced),
                useBindless,
                useBindless ? bindlessEnhancedResolveRootSig.Get()
                            : enhancedResolveRootSig.Get()))
            giRadianceCacheMode =
                giRadianceCache.ClearThisFrame() && !ScopeSurfaceBound() ? 2u
                                                                        : 1u;
        // ReSTIR: main view, full-resolution Lumen only (cascades and the
        // half-resolution share replace the per-pixel ray it resamples).
        lumenReSTIRMode = 0;
        if (useEnhanced && lumenGIActive && lumenReSTIRRequested &&
            !radianceCascadesGIRequested && !variableRateGIRequested && !lumenGIHalfResolutionActive &&
            !ScopeSurfaceBound() && EnsureReSTIRBuffers(width, height))
            lumenReSTIRMode = restirHistoryValid ? 1u : 2u;
        else if (!ScopeSurfaceBound())
            restirHistoryValid = false;  // a gap in the reservoir chain
        if (useEnhanced && !ScopeSurfaceBound())
            EnsureEmissiveReservoirs(width, height);
        if (!ScopeSurfaceBound()) {
            const bool active = useEnhanced && emissiveCGNSRequested &&
                debugViewMode == 0 && emissiveReservoirBuffer;
            emissiveCGNSMode = active
                ? (emissiveCGNSHistoryValid && surfaceHistoryValid ? 2u : 1u) : 0u;
            if (!active) emissiveCGNSHistoryValid = false;
        }
        if (useEnhanced) {
            UpdateEnhancedConstants(frameSlot);
        }

        // Debug views inspect the latest completed history without advancing
        // it. Only normal shading commits a new ping-pong side.
        //
        // The scope is excluded for a stronger reason. svgfHistoryPing and the
        // history textures it selects are shared by every view and indexed by
        // frame parity alone. The scope resolves earlier in the same frame
        // than the main view, so letting it advance the ping-pong would flip
        // the side the main view then reads AND fill it with radiance from the
        // scope's frustum. Per-view history is the real fix and a larger
        // change; until then the scope neither reads nor writes it, which
        // costs the lens its temporal accumulation and costs the main view
        // nothing.
        const bool svgfWillWriteHistory =
            useEnhanced && svgfTemporalEnabled && !rayReconstructionActive &&
            debugViewMode == 0 &&
            !ScopeSurfaceBound();
        // Reports whether the temporal pass ran, for the debug panel. Left to
        // the main view: the scope always answers false now, and the panel is
        // asking about the view on screen.
        if (!ScopeSurfaceBound())
            svgfTemporalExecutedLastFrame = svgfWillWriteHistory;
        UINT svgfHistoryRead = svgfHistoryPing;
        UINT svgfHistoryWrite = svgfHistoryPing ^ 1u;
        if (svgfWillWriteHistory) {
            svgfHistoryPing ^= 1;
            svgfHistoryRead = svgfHistoryPing ^ 1u;
            svgfHistoryWrite = svgfHistoryPing;
        }
        if (useEnhanced)
            RefreshSVGFDescriptors(frameSlot, svgfHistoryRead,
                                   svgfHistoryWrite);

        // Transition SVGF write-side history to UAV before the enhanced
        // resolve reads the previous frame's history and writes the current.
        if (svgfWillWriteHistory) {
            D3D12_RESOURCE_BARRIER svgfBarriers[2] = {};
            for (UINT i = 0; i < 2; ++i) {
                svgfBarriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                svgfBarriers[i].Transition.StateBefore =
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                svgfBarriers[i].Transition.StateAfter =
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                svgfBarriers[i].Transition.Subresource =
                    D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            }
            svgfBarriers[0].Transition.pResource =
                svgfHistoryColor[svgfHistoryWrite].Get();
            svgfBarriers[1].Transition.pResource =
                svgfHistoryMoments[svgfHistoryWrite].Get();
            cmdList->ResourceBarrier(2, svgfBarriers);
        }

        UINT bindlessTableBase = BINDLESS_INVALID_INDEX;
        if (useBindless) {
            bindlessTableBase = BuildBindlessResolveTable(
                useEnhanced ? enhancedDescHeap : standardDescHeap,
                useEnhanced ? kEnhancedResolveDescriptorCount
                            : kResolveDescriptorCount,
                frameSlot);
            if (bindlessTableBase == BINDLESS_INVALID_INDEX) {
                bindlessTransientOverflowLastFrame = true;
            } else {
                bindlessTransientOverflowLastFrame = false;
            }
        } else {
            bindlessTransientOverflowLastFrame = false;
        }

        ID3D12RootSignature* selectedRoot = useBindless
            ? (useEnhanced ? bindlessEnhancedResolveRootSig.Get()
                           : bindlessResolveRootSig.Get())
            : (useEnhanced ? enhancedResolveRootSig.Get()
                           : resolveRootSig.Get());
        ID3D12PipelineState* selectedPSO = useBindless
            ? (useEnhanced ? bindlessEnhancedResolvePSO.Get()
                           : bindlessResolvePSO.Get())
            : (useEnhanced ? enhancedResolvePSO.Get() :
                (g_virtualShadowConstants.config[0] && virtualShadowResolvePSO
                    ? virtualShadowResolvePSO.Get() : resolvePSO.Get()));
        // Each tier has a terrain twin sharing its root signature and heap, so
        // terrain resolves on whichever variant the frame actually selected.
        // TerrainVisibilityReady() applies the same tier lookup, so the draw
        // and the resolve can never disagree about who owns terrain.
        ID3D12PipelineState* terrainPSO = TerrainResolvePSOForTier(
            useBindless, useEnhanced);
        ID3D12PipelineState* terrainOnlyPSO = TerrainOnlyResolvePSOForTier(
            useBindless, useEnhanced);
        // Both halves or neither. The generic half skips the reserved ID, so
        // running it without the terrain half would leave terrain unshaded --
        // matching TerrainVisibilityReady(), which keeps terrain on the forward
        // path unless this tier has the pair.
        const bool useTerrainResolve =
            terrainVisibilityActiveThisFrame && terrainPSO && terrainOnlyPSO;
        // Terrain wanted on this tier but its set was never built: note it for
        // PumpDeferredResolvePipelines. Only real resolve frames with terrain
        // bound count, so menu and loading frames cannot trigger a build.
        if (!ScopeSurfaceBound() && terrainVisibilityRequested &&
            terrainAlbedoArray && terrainNormalArray && terrainMetalRoughArray &&
            !(terrainPSO && terrainOnlyPSO)) {
            const int tier = ResolveTierIndex(useBindless, useEnhanced);
            if (tier == missingTerrainTier) ++missingTerrainTierFrames;
            else { missingTerrainTier = tier; missingTerrainTierFrames = 1; }
        } else {
            missingTerrainTierFrames = 0;
        }
        if (useTerrainResolve) {
            selectedPSO = terrainPSO;
            // The arrays are created once at level load and never reallocated,
            // so this writes on the first terrain frame and after a resize
            // rebuilds the heap -- not every frame. The enhanced and bindless
            // heaps are rewritten per frame by their own prepare paths.
            if (!terrainDescriptorsWritten) {
                WriteTerrainDescriptors(computeDescHeap.Get(), 87);
                terrainDescriptorsWritten = true;
            }
        }

        // Radiance cascades replace the Lumen estimate on the main view only:
        // their atlases and tables belong to the frame slot, and the scope
        // resolves earlier in the same frame.
        bool useRadianceCascades = false;
        std::array<D3D12_GPU_DESCRIPTOR_HANDLE,
                   RadianceCascadesDX12::PassCount> cascadeTables{};
        if (radianceCascadesGIRequested && lumenGIActive && useEnhanced &&
            !ScopeSurfaceBound()) {
            useRadianceCascades = radianceCascades.Ensure(
                ResolveTierSource(useBindless ? ResolveTierBindlessEnhanced
                                              : ResolveTierEnhanced),
                useBindless, width, height);
            if (useRadianceCascades) {
                // After every per-frame rewrite of the enhanced heap above.
                radianceCascades.PrepareDescriptors(frameSlot, enhancedDescHeap);
                for (UINT pass = 0; pass < RadianceCascadesDX12::PassCount &&
                                    useRadianceCascades; ++pass) {
                    ID3D12DescriptorHeap* heap =
                        radianceCascades.Heap(frameSlot, pass);
                    if (!useBindless) {
                        cascadeTables[pass] =
                            heap->GetGPUDescriptorHandleForHeapStart();
                        continue;
                    }
                    const UINT base = AllocateBindlessResolveTable(
                        heap, RadianceCascadesDX12::DescriptorCount);
                    useRadianceCascades = base != BINDLESS_INVALID_INDEX;
                    if (useRadianceCascades)
                        cascadeTables[pass] = bindlessHeap->GpuHandleAt(base);
                }
            }
        }
        bool useVariableRateGI = false;
        D3D12_GPU_DESCRIPTOR_HANDLE variableRateTable{};
        if (variableRateGIRequested && lumenGIActive && useEnhanced &&
            !useRadianceCascades && !ScopeSurfaceBound() && debugViewMode == 0) {
            useVariableRateGI = variableRateGI.Ensure(
                ResolveTierSource(useBindless ? ResolveTierBindlessEnhanced : ResolveTierEnhanced),
                useBindless, width, height);
            if (useVariableRateGI) {
                variableRateGI.PrepareDescriptors(frameSlot, enhancedDescHeap);
                if (useBindless) {
                    const UINT base = AllocateBindlessResolveTable(variableRateGI.StagingHeap(frameSlot),
                        VariableRateGIDX12::DescriptorCount);
                    useVariableRateGI = base != BINDLESS_INVALID_INDEX;
                    if (useVariableRateGI) variableRateTable = bindlessHeap->GpuHandleAt(base);
                } else variableRateTable = variableRateGI.Heap(frameSlot)->GetGPUDescriptorHandleForHeapStart();
            }
        }
        if (!ScopeSurfaceBound()) {
            variableRateGIActive = useVariableRateGI;
            if (!useVariableRateGI) variableRateGI.InvalidateHistory();
            radianceCascadesGIActive = useRadianceCascades;
        }
        // This frame writes every pixel's reservoir, so next frame may read.
        if (lumenReSTIRMode != 0u) restirHistoryValid = true;

        if (!useBindless) {
            g_dx12.device->CopyDescriptorsSimple(
                useEnhanced ? kEnhancedResolveDescriptorCount : kResolveDescriptorCount,
                ResolveViewHeap()->GetCPUDescriptorHandleForHeapStart(),
                (useEnhanced ? enhancedDescHeap : standardDescHeap)
                    ->GetCPUDescriptorHandleForHeapStart(),
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }
        // SM 6.6 directly-indexed root signatures require the heap to be set
        // first so the driver captures the correct heap base in the signature.
        ID3D12DescriptorHeap* selectedHeap = useBindless
            ? bindlessHeap->Heap()
            : ResolveViewHeap();
        ID3D12DescriptorHeap* heaps[] = { selectedHeap };
        cmdList->SetDescriptorHeaps(1, heaps);
        cmdList->SetComputeRootSignature(selectedRoot);
        cmdList->SetPipelineState(selectedPSO);

        // Bind root parameters
        // b0 - frame constants
        cmdList->SetComputeRootConstantBufferView(0,
            frameConstantBuffer.GetGPUAddress(ViewFrameIndex()));

        // b1 - light buffer (we'll upload via a temporary inline approach)
        // Actually, we reuse the mainShader's lightBuffer
        // For simplicity, create inline CBVs using the mainShader's addresses
        // We'll pass these from outside. For now, bind the descriptor table.

        // Descriptor table at root param 1 (SRVs + UAV)
        cmdList->SetComputeRootDescriptorTable(1,
            useBindless
                ? bindlessHeap->GpuHandleAt(bindlessTableBase)
                : (useEnhanced
                    ? ResolveViewHeap()->GetGPUDescriptorHandleForHeapStart()
                    : ResolveViewHeap()->GetGPUDescriptorHandleForHeapStart()));
        // u16 Lumen radiance cache and u17/u18 ReSTIR reservoirs; every
        // enhanced root signature declares them. Unallocated until first
        // enabled, and never read while their modes are 0.
        if (useEnhanced) BindEnhancedRootUAVs(cmdList);

        // Dispatch (GPU-driven via ExecuteIndirect)
        UINT groupsX = (width + 7) / 8;
        UINT groupsY = (height + 7) / 8;
        if (mappedResolveDispatchArgs && !ScopeSurfaceBound()) {
            mappedResolveDispatchArgs->ThreadGroupCountX = groupsX;
            mappedResolveDispatchArgs->ThreadGroupCountY = groupsY;
            mappedResolveDispatchArgs->ThreadGroupCountZ = 1;
        }

        // Tile classification. Only worth running when the resolve is actually
        // split -- with terrain off there is one dispatch and nothing to
        // separate -- and only when this tier has classified PSOs for both
        // halves. Anything missing falls back to two full-screen dispatches,
        // which is correct but pays the sweep.
        ID3D12PipelineState* tiledGenericPSO = TerrainResolveTiledPSOForTier(
            useBindless, useEnhanced);
        ID3D12PipelineState* tiledTerrainPSO =
            TerrainOnlyResolveTiledPSOForTier(useBindless, useEnhanced);
        const bool useTileClassification =
            // The cascade tier builds no tile-classified permutations.
            !useRadianceCascades && !useVariableRateGI &&
            !ScopeSurfaceBound() && useTerrainResolve && tileClassifyReady && tiledGenericPSO &&
            tiledTerrainPSO && tileClassifyPSO && tileClassifyResetPSO &&
            genericTileListBuffer && terrainTileListBuffer &&
            classifiedDispatchArgsBuffer && mappedTileClassifyConstants;
        tileClassifiedLastFrame = useTileClassification;
        // One-shot record of which path the split actually took, for
        // verification. Written once rather than per frame so it costs nothing
        // after the first terrain frame.
        if (useTerrainResolve && !tileClassifyPathLogged) {
            tileClassifyPathLogged = true;
            std::ofstream("tile_classify.log", std::ios::app)
                << (useTileClassification ? "classified" : "full-screen")
                << " tiles=" << tileClassifyTilesX << "x"
                << tileClassifyTilesY
                << " bindless=" << (useBindless ? 1 : 0)
                << " enhanced=" << (useEnhanced ? 1 : 0) << "\n";
        }

        // Setup ends here: everything after this point is dispatch work that
        // has a scope of its own.
        setupScope.reset();

        // Relight (or clear) the radiance cache before this frame's lookups.
        // Uses the resolve's own root signature and bindings; main view only.
        if (giRadianceCacheMode != 0u && !ScopeSurfaceBound()) {
            giRadianceCache.Refresh(cmdList, useBindless);
            cmdList->SetPipelineState(selectedPSO);
        }
        // One-thread ray-query dispatch ahead of the resolve. Measured on Base
        // (4060, 1080p): when the generic resolve was the first ray-query
        // pipeline dispatched in the command list it took 3.02 ms; after any
        // earlier ray-query dispatch -- even one whose threads exit at once --
        // 1.87 ms, and the GPU frame dropped 1.4 ms. An empty pipeline with no
        // ray-query code did not help. SGE_NO_RQ_WARMUP=1 skips it for A/B.
        static const bool kNoRayQueryWarmup =
            GetEnvironmentVariableA("SGE_NO_RQ_WARMUP", nullptr, 0) > 0;
        if (useEnhanced && !kNoRayQueryWarmup &&
            rayQueryWarmup.Ensure(
                ResolveTierSource(useBindless ? ResolveTierBindlessEnhanced
                                              : ResolveTierEnhanced),
                useBindless, selectedRoot)) {
            cmdList->SetPipelineState(rayQueryWarmup.PSO(useBindless));
            cmdList->Dispatch(1, 1, 1);
            cmdList->SetPipelineState(selectedPSO);
        }

        // The traces read the same depth, visibility IDs and TLAS as the
        // resolve, so they run here, after its input transitions. The resolve
        // then binds the cascade root signature to read the guides.
        if (useRadianceCascades) {
            const UINT64 constants =
                frameConstantBuffer.GetGPUAddress(ViewFrameIndex());
            ID3D12DescriptorHeap* sharedHeap =
                useBindless ? bindlessHeap->Heap() : nullptr;
            radianceCascades.Dispatch(cmdList, frameSlot, constants,
                                      useBindless, sharedHeap, cascadeTables);
            radianceCascades.BindResolve(cmdList, useBindless,
                useBindless ? sharedHeap
                    : radianceCascades.Heap(frameSlot,
                                            RadianceCascadesDX12::ResolvePass),
                constants, cascadeTables[RadianceCascadesDX12::ResolvePass],
                giRadianceCache.Address(),
                restirSampleBuffer ? restirSampleBuffer->GetGPUVirtualAddress() : 0,
                restirWeightBuffer ? restirWeightBuffer->GetGPUVirtualAddress() : 0,
                EmissiveTriangleAddress(), EmissiveReservoirAddress());
            selectedPSO = radianceCascades.ResolvePSO(
                useBindless, useTerrainResolve, false);
            terrainOnlyPSO = radianceCascades.ResolvePSO(useBindless, true, true);
            cmdList->SetPipelineState(selectedPSO);
        }

        if (useVariableRateGI) {
            VariableRateGIDX12::Bindings bindings = {
                frameConstantBuffer.GetGPUAddress(ViewFrameIndex()), giRadianceCache.Address(),
                restirSampleBuffer ? restirSampleBuffer->GetGPUVirtualAddress() : 0,
                restirWeightBuffer ? restirWeightBuffer->GetGPUVirtualAddress() : 0,
                EmissiveTriangleAddress(), EmissiveReservoirAddress()
            };
            variableRateGI.Dispatch(cmdList, frameSlot, useBindless,
                useBindless ? bindlessHeap->Heap() : variableRateGI.Heap(frameSlot),
                variableRateTable, bindings, variableRateGIBudget);
            selectedPSO = variableRateGI.ResolvePSO(useBindless, useTerrainResolve, false);
            terrainOnlyPSO = variableRateGI.ResolvePSO(useBindless, true, true);
            cmdList->SetPipelineState(selectedPSO);
        }

        if (useTileClassification) {
            ProfilerDX12::Scope classifyScope(
                g_profiler, "VB Tile Classify", cmdList);

            struct TileClassifyConstants {
                UINT screenWidth;
                UINT screenHeight;
                UINT tilesX;
                UINT tilesY;
            } constants = { width, height, tileClassifyTilesX,
                            tileClassifyTilesY };
            std::memcpy(mappedTileClassifyConstants, &constants,
                        sizeof(constants));

            ID3D12DescriptorHeap* classifyHeaps[] = {
                tileClassifyDescHeap.Get() };
            cmdList->SetDescriptorHeaps(1, classifyHeaps);
            cmdList->SetComputeRootSignature(tileClassifyRootSig.Get());
            cmdList->SetComputeRootConstantBufferView(0,
                tileClassifyConstantBuffer->GetGPUVirtualAddress());
            cmdList->SetComputeRootDescriptorTable(1,
                tileClassifyDescHeap->GetGPUDescriptorHandleForHeapStart());

            // Seed both argument records to (0, 1, 1); the counting pass only
            // increments X, so a zeroed buffer would dispatch nothing.
            cmdList->SetPipelineState(tileClassifyResetPSO.Get());
            cmdList->Dispatch(1, 1, 1);

            // The counting pass must see the seeded values, so unlike the two
            // resolve halves these dispatches do touch the same memory and a
            // UAV barrier is required between them.
            D3D12_RESOURCE_BARRIER argsBarrier = {};
            argsBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            argsBarrier.UAV.pResource = classifiedDispatchArgsBuffer.Get();
            cmdList->ResourceBarrier(1, &argsBarrier);

            cmdList->SetPipelineState(tileClassifyPSO.Get());
            cmdList->Dispatch(tileClassifyTilesX, tileClassifyTilesY, 1);

            // The lists and the counts are written here and consumed by
            // ExecuteIndirect below, so both need to land first.
            D3D12_RESOURCE_BARRIER listBarriers[3] = {};
            for (int i = 0; i < 3; ++i)
                listBarriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            listBarriers[0].UAV.pResource = genericTileListBuffer.Get();
            listBarriers[1].UAV.pResource = terrainTileListBuffer.Get();
            listBarriers[2].UAV.pResource = classifiedDispatchArgsBuffer.Get();
            cmdList->ResourceBarrier(3, listBarriers);

            // The resolve reads the lists through root SRVs while
            // ExecuteIndirect sources its counts from the argument buffer.
            // Transition every classify output to the state of its consumer.
            D3D12_RESOURCE_BARRIER toResolve[3] = {};
            for (int i = 0; i < 3; ++i) {
                toResolve[i].Type =
                    D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                toResolve[i].Transition.StateBefore =
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                toResolve[i].Transition.Subresource =
                    D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            }
            toResolve[0].Transition.pResource = genericTileListBuffer.Get();
            toResolve[0].Transition.StateAfter =
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            toResolve[1].Transition.pResource = terrainTileListBuffer.Get();
            toResolve[1].Transition.StateAfter =
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            toResolve[2].Transition.pResource =
                classifiedDispatchArgsBuffer.Get();
            toResolve[2].Transition.StateAfter =
                D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
            cmdList->ResourceBarrier(3, toResolve);

            // Restore the resolve's own heap and bindings, which the classify
            // pass just displaced.
            ID3D12DescriptorHeap* resolveHeaps[] = { selectedHeap };
            cmdList->SetDescriptorHeaps(1, resolveHeaps);
            cmdList->SetComputeRootSignature(selectedRoot);
            cmdList->SetComputeRootConstantBufferView(0,
                frameConstantBuffer.GetGPUAddress(ViewFrameIndex()));
            cmdList->SetComputeRootDescriptorTable(1,
                useBindless
                    ? bindlessHeap->GpuHandleAt(bindlessTableBase)
                    : (useEnhanced
                        ? ResolveViewHeap()->GetGPUDescriptorHandleForHeapStart()
                        : ResolveViewHeap()->GetGPUDescriptorHandleForHeapStart()));
            if (useEnhanced) BindEnhancedRootUAVs(cmdList);

            selectedPSO = tiledGenericPSO;
        }

        {
            // The generic half is the resolve's single most expensive dispatch:
            // it decodes the visibility ID, refetches and interpolates vertex
            // attributes, samples every material texture, and runs the full
            // lighting/shadow/IBL chain. Scoped on its own so its share is
            // visible against the terrain half and the SVGF passes rather than
            // hiding inside one aggregate "VB Resolve" number.
            ProfilerDX12::Scope genericResolveScope(
                g_profiler, "VB Shade Generic", cmdList);
            if (useTileClassification) {
                // Generic half over its own tile list: record 0 of the args
                // buffer, and the list it names bound at t90.
                cmdList->SetComputeRootShaderResourceView(2,
                    genericTileListBuffer->GetGPUVirtualAddress());
                cmdList->SetPipelineState(selectedPSO);
                cmdList->ExecuteIndirect(resolveDispatchSignature.Get(), 1,
                                         classifiedDispatchArgsBuffer.Get(), 0,
                                         nullptr, 0);
            } else if (!ScopeSurfaceBound() && resolveDispatchSignature && resolveDispatchArgsBuffer) {
                cmdList->ExecuteIndirect(resolveDispatchSignature.Get(), 1, resolveDispatchArgsBuffer.Get(), 0, nullptr, 0);
            } else {
                cmdList->Dispatch(groupsX, groupsY, 1);
            }
        }

        // Terrain half of the split. Same root signature, same heap, same
        // bindings, same thread-group count -- only the PSO changes, so this is
        // a pipeline swap and a dispatch, with no rebinding.
        //
        // No UAV barrier between the halves. The two shade disjoint pixel sets
        // (one returns on the reserved ID, the other returns on everything
        // else), so they never write the same texel and the results are
        // order-independent. A barrier here would serialise two dispatches that
        // are free to overlap, which is exactly the occupancy the split is
        // meant to buy. The barriers that follow this block already cover the
        // combined writes before anything reads them.
        if (useTerrainResolve) {
            ProfilerDX12::Scope terrainResolveScope(
                g_profiler, "VB Terrain Resolve", cmdList);
            if (useTileClassification) {
                // Terrain half over its own list: record 1, at byte offset
                // sizeof(D3D12_DISPATCH_ARGUMENTS) into the same buffer.
                cmdList->SetComputeRootShaderResourceView(2,
                    terrainTileListBuffer->GetGPUVirtualAddress());
                cmdList->SetPipelineState(tiledTerrainPSO);
                cmdList->ExecuteIndirect(
                    resolveDispatchSignature.Get(), 1,
                    classifiedDispatchArgsBuffer.Get(),
                    sizeof(D3D12_DISPATCH_ARGUMENTS), nullptr, 0);
            } else {
                cmdList->SetPipelineState(terrainOnlyPSO);
                if (!ScopeSurfaceBound() && resolveDispatchSignature && resolveDispatchArgsBuffer) {
                    cmdList->ExecuteIndirect(resolveDispatchSignature.Get(), 1,
                                             resolveDispatchArgsBuffer.Get(), 0,
                                             nullptr, 0);
                } else {
                    cmdList->Dispatch(groupsX, groupsY, 1);
                }
            }
        }

        // Finish both disjoint halves before any future spatial reservoir read.
        // No barrier is inserted between generic and terrain shading.
        if (!ScopeSurfaceBound() && emissiveCGNSMode != 0u) {
            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            barrier.UAV.pResource = emissiveReservoirBuffer.Get();
            cmdList->ResourceBarrier(1, &barrier);
            emissiveCGNSHistoryValid = true;
        }

        // Return all classifier outputs to UNORDERED_ACCESS for next frame.
        // The lists become UAVs again alongside the indirect argument buffer,
        // before the reset and classify passes overwrite them.
        if (useTileClassification) {
            D3D12_RESOURCE_BARRIER toClassify[3] = {};
            for (int i = 0; i < 3; ++i) {
                toClassify[i].Type =
                    D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                toClassify[i].Transition.StateAfter =
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                toClassify[i].Transition.Subresource =
                    D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            }
            toClassify[0].Transition.pResource =
                genericTileListBuffer.Get();
            toClassify[0].Transition.StateBefore =
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            toClassify[1].Transition.pResource =
                terrainTileListBuffer.Get();
            toClassify[1].Transition.StateBefore =
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            toClassify[2].Transition.pResource =
                classifiedDispatchArgsBuffer.Get();
            toClassify[2].Transition.StateBefore =
                D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT;
            cmdList->ResourceBarrier(3, toClassify);
        }

        if (bentNormalHistoryActive) {
            D3D12_RESOURCE_BARRIER historyBarrier = {};
            historyBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            historyBarrier.Transition.pResource = bentNormalGTAOHistory;
            historyBarrier.Transition.StateBefore =
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            historyBarrier.Transition.StateAfter =
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            historyBarrier.Transition.Subresource =
                D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            cmdList->ResourceBarrier(1, &historyBarrier);
        }

        // Sample how much of the screen went to RT. Only meaningful when the
        // enhanced resolve actually wrote the mask this frame.
        if (useEnhanced && !ScopeSurfaceBound()) UpdateRayMaskStatistic(cmdList);

        // Transition SVGF colour/moment history back to SRV for next frame.
        // Stable-surface roles remain unchanged until post has read the same
        // previous frame and finished writing the current authored keys.
        if (svgfWillWriteHistory) {
            D3D12_RESOURCE_BARRIER svgfBarriers[2] = {};
            for (UINT i = 0; i < 2; ++i) {
                svgfBarriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                svgfBarriers[i].Transition.StateBefore =
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                svgfBarriers[i].Transition.StateAfter =
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                svgfBarriers[i].Transition.Subresource =
                    D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            }
            svgfBarriers[0].Transition.pResource =
                svgfHistoryColor[svgfHistoryWrite].Get();
            svgfBarriers[1].Transition.pResource =
                svgfHistoryMoments[svgfHistoryWrite].Get();
            cmdList->ResourceBarrier(2, svgfBarriers);
            svgfHistoryValid = true;
        }

        // Phase 5c: SVGF à-trous spatial filter. Multi-iteration wavelet
        // applied to the specular IBL signal, then composited back into the
        // lit output. Only runs when the enhanced resolve ran with both
        // temporal and spatial SVGF enabled.
        const UINT atrousIterationCount = SVGFAtrousIterationCount();
        ID3D12DescriptorHeap* atrousDescHeap =
            svgfAtrousDescHeaps[frameSlot].Get();
        ID3D12DescriptorHeap* compositeDescHeap =
            svgfCompositeDescHeaps[frameSlot].Get();
        const bool atrousWanted =
            !ScopeSurfaceBound() && useEnhanced && !rayReconstructionActive &&
            svgfTemporalEnabled && svgfAtrousEnabled &&
            (debugViewMode == 0 || debugViewMode == 6);
        // First frame that needs the spatial filter compiles it; a failed
        // build is not retried.
        if (atrousWanted && !svgfAtrousPipelineTried) {
            svgfAtrousPipelineTried = true;
            CreateSVGFAtrousPipeline();
            atrousDescHeap = svgfAtrousDescHeaps[frameSlot].Get();
            compositeDescHeap = svgfCompositeDescHeaps[frameSlot].Get();
        }
        const bool atrousRan = atrousWanted &&
            svgfAtrousPipelineReady && svgfAtrousPSO && svgfAtrousRootSig &&
            atrousDescHeap && svgfCompositePSO && svgfCompositeRootSig &&
            compositeDescHeap;
        svgfAtrousExecutedLastFrame = atrousRan;
        svgfCompositeExecutedLastFrame = atrousRan;
        svgfAtrousDispatchesLastFrame = atrousRan
            ? atrousIterationCount : 0u;
        if (atrousRan) {
            const UINT descSize = g_dx12.cbvSrvUavDescriptorSize;

            // Transition reflectionSrc from UAV (resolve write) to SRV (atrous read).
            // Scratch textures stay in UAV state between frames; the iteration
            // barriers publish only the texture the next dispatch reads.
            {
                D3D12_RESOURCE_BARRIER reflectionToSRV = {};
                reflectionToSRV.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                reflectionToSRV.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                reflectionToSRV.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                reflectionToSRV.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                reflectionToSRV.Transition.pResource = svgfReflectionSrc.Get();
                cmdList->ResourceBarrier(1, &reflectionToSRV);
            }

            // Build atrous descriptor heap.
            // [0..3] t0..t3: reflectionSrc, depth, normalRoughness, svgfMoments
            // [4..5] u0..u1: scratchA, scratchB
            {
                D3D12_CPU_DESCRIPTOR_HANDLE h =
                    atrousDescHeap->GetCPUDescriptorHandleForHeapStart();

                D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
                srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                srv.Texture2D.MipLevels = 1;

                // [0] t0: reflectionSrc
                g_dx12.device->CreateShaderResourceView(svgfReflectionSrc.Get(), &srv, h);

                // [1] t1: depth buffer
                D3D12_CPU_DESCRIPTOR_HANDLE h1 = h; h1.ptr += (UINT64)descSize;
                D3D12_SHADER_RESOURCE_VIEW_DESC depthSrv = {};
                depthSrv.Format = DXGI_FORMAT_R32_FLOAT;
                depthSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                depthSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                depthSrv.Texture2D.MipLevels = 1;
                g_dx12.device->CreateShaderResourceView(ActiveDepthBuffer(), &depthSrv, h1);

                // [2] t2: normalRoughness
                D3D12_CPU_DESCRIPTOR_HANDLE h2 = h; h2.ptr += (UINT64)descSize * 2;
                g_dx12.device->CreateShaderResourceView(normalRoughnessTexture.Get(), &srv, h2);

                // [3] t3: svgfHistoryMoments (current write-side, just written by resolve)
                D3D12_CPU_DESCRIPTOR_HANDLE h3 = h; h3.ptr += (UINT64)descSize * 3;
                g_dx12.device->CreateShaderResourceView(svgfHistoryMoments[svgfHistoryPing].Get(), &srv, h3);

                // [4] u0: scratchA
                D3D12_CPU_DESCRIPTOR_HANDLE h4 = h; h4.ptr += (UINT64)descSize * 4;
                D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
                uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                g_dx12.device->CreateUnorderedAccessView(svgfAtrousScratch[0].Get(), nullptr, &uav, h4);

                // [5] u1: scratchB
                D3D12_CPU_DESCRIPTOR_HANDLE h5 = h; h5.ptr += (UINT64)descSize * 5;
                g_dx12.device->CreateUnorderedAccessView(svgfAtrousScratch[1].Get(), nullptr, &uav, h5);

                // Second descriptor set [6..11], identical except that t0 reads
                // scratchA and u0 writes scratchB. Iterations alternate between
                // the two sets, which is what actually chains them: the shader
                // binds one SRV (t0) and one UAV (u0), so without a distinct
                // table per parity every iteration would read the same source
                // and write the same target, and N iterations would collapse to
                // the effect of one.
                D3D12_CPU_DESCRIPTOR_HANDLE h6 = h; h6.ptr += (UINT64)descSize * 6;
                g_dx12.device->CreateShaderResourceView(
                    svgfAtrousScratch[0].Get(), &srv, h6);
                D3D12_CPU_DESCRIPTOR_HANDLE h7 = h; h7.ptr += (UINT64)descSize * 7;
                g_dx12.device->CreateShaderResourceView(
                    ActiveDepthBuffer(), &depthSrv, h7);
                D3D12_CPU_DESCRIPTOR_HANDLE h8 = h; h8.ptr += (UINT64)descSize * 8;
                g_dx12.device->CreateShaderResourceView(
                    normalRoughnessTexture.Get(), &srv, h8);
                D3D12_CPU_DESCRIPTOR_HANDLE h9 = h; h9.ptr += (UINT64)descSize * 9;
                g_dx12.device->CreateShaderResourceView(
                    svgfHistoryMoments[svgfHistoryPing].Get(), &srv, h9);
                D3D12_CPU_DESCRIPTOR_HANDLE h10 = h; h10.ptr += (UINT64)descSize * 10;
                g_dx12.device->CreateUnorderedAccessView(
                    svgfAtrousScratch[1].Get(), nullptr, &uav, h10);
                D3D12_CPU_DESCRIPTOR_HANDLE h11 = h; h11.ptr += (UINT64)descSize * 11;
                g_dx12.device->CreateUnorderedAccessView(
                    svgfAtrousScratch[0].Get(), nullptr, &uav, h11);

                // Third set [12..17]: t0 reads scratchB, u0 writes scratchA.
                D3D12_CPU_DESCRIPTOR_HANDLE h12 = h; h12.ptr += (UINT64)descSize * 12;
                g_dx12.device->CreateShaderResourceView(
                    svgfAtrousScratch[1].Get(), &srv, h12);
                D3D12_CPU_DESCRIPTOR_HANDLE h13 = h; h13.ptr += (UINT64)descSize * 13;
                g_dx12.device->CreateShaderResourceView(
                    ActiveDepthBuffer(), &depthSrv, h13);
                D3D12_CPU_DESCRIPTOR_HANDLE h14 = h; h14.ptr += (UINT64)descSize * 14;
                g_dx12.device->CreateShaderResourceView(
                    normalRoughnessTexture.Get(), &srv, h14);
                D3D12_CPU_DESCRIPTOR_HANDLE h15 = h; h15.ptr += (UINT64)descSize * 15;
                g_dx12.device->CreateShaderResourceView(
                    svgfHistoryMoments[svgfHistoryPing].Get(), &srv, h15);
                D3D12_CPU_DESCRIPTOR_HANDLE h16 = h; h16.ptr += (UINT64)descSize * 16;
                g_dx12.device->CreateUnorderedAccessView(
                    svgfAtrousScratch[0].Get(), nullptr, &uav, h16);
                D3D12_CPU_DESCRIPTOR_HANDLE h17 = h; h17.ptr += (UINT64)descSize * 17;
                g_dx12.device->CreateUnorderedAccessView(
                    svgfAtrousScratch[1].Get(), nullptr, &uav, h17);
            }

            // Atrous iterations. stride = 1, 2, 4, 8, 16 for 5 iterations.
            {
                struct AtrousConstants {
                    UINT screenWidth;
                    UINT screenHeight;
                    float sigmaDepth;
                    float sigmaNormal;
                    float sigmaLuminance;
                    UINT iterationIndex;
                    UINT diagnosticMode;
                    UINT iterationCount;
                    float maxAccumFrames;
                    UINT pad0;
                    UINT pad1;
                    UINT pad2;
                };

                cmdList->SetComputeRootSignature(svgfAtrousRootSig.Get());
                cmdList->SetPipelineState(svgfAtrousPSO.Get());
                ID3D12DescriptorHeap* atrousHeaps[] = { atrousDescHeap };
                cmdList->SetDescriptorHeaps(1, atrousHeaps);
                const D3D12_GPU_DESCRIPTOR_HANDLE atrousTableBase =
                    atrousDescHeap->GetGPUDescriptorHandleForHeapStart();

                for (UINT iter = 0; iter < atrousIterationCount; ++iter) {
                    // Set 0 (offset 0)  : reflectionSrc -> scratchA
                    // Set 1 (offset 6)  : scratchA      -> scratchB
                    // Set 2 (offset 12) : scratchB      -> scratchA
                    // Iteration 0 consumes the resolve output; after that the
                    // sets alternate so each pass reads what the previous wrote.
                    const UINT setIndex = (iter == 0) ? 0u : (2u - (iter & 1u));
                    // Sets 0 and 2 write scratchA, set 1 writes scratchB.
                    const UINT writeIdx = (setIndex == 1u) ? 1u : 0u;
                    D3D12_GPU_DESCRIPTOR_HANDLE table = atrousTableBase;
                    table.ptr += (UINT64)descSize * 6ull * setIndex;
                    cmdList->SetComputeRootDescriptorTable(1, table);
                    std::string iterName = "SVGF Atrous " + std::to_string(iter);
                    ProfilerDX12::Scope atrousScope(g_profiler, iterName.c_str(), cmdList);

                    AtrousConstants ac = {};
                    ac.screenWidth = width;
                    ac.screenHeight = height;
                    ac.sigmaDepth = 1.0f;
                    ac.sigmaNormal = 128.0f;
                    ac.sigmaLuminance = 4.0f;
                    ac.iterationIndex = iter;
                    ac.diagnosticMode = debugViewMode == 6
                        ? svgfAtrousDiagnosticMode : 0u;
                    ac.iterationCount = atrousIterationCount;
                    ac.maxAccumFrames =
                        static_cast<float>((std::max)(svgfMaxAccumFrames, 1u));
                    ac.pad0 = 0u;
                    ac.pad1 = 0u;
                    ac.pad2 = 0u;
                    const UINT64 constantOffset =
                        (static_cast<UINT64>(frameSlot) *
                             kSVGFAtrousMaxIterations + iter) * 256ull;
                    memcpy(static_cast<BYTE*>(svgfAtrousConstantMapped) +
                               constantOffset,
                           &ac, sizeof(ac));
                    cmdList->SetComputeRootConstantBufferView(0,
                        svgfAtrousConstantBuffer->GetGPUVirtualAddress() +
                            constantOffset);

                    cmdList->Dispatch(groupsX, groupsY, 1);

                    // This iteration wrote `written`; the next reads it as an
                    // SRV, so it needs a real state transition, not just a UAV
                    // barrier. A UAV barrier only orders UAV-to-UAV access; it
                    // does not move the resource into a shader-readable state,
                    // and reading a UAV-state texture through an SRV is
                    // undefined. The buffer the next pass writes is transitioned
                    // back to UNORDERED_ACCESS in the same call.
                    const UINT written = writeIdx;
                    if (iter + 1 < atrousIterationCount) {
                        const UINT nextWrite = written ^ 1u;
                        D3D12_RESOURCE_BARRIER iterBarriers[2] = {};
                        iterBarriers[0].Type =
                            D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                        iterBarriers[0].Transition.pResource =
                            svgfAtrousScratch[written].Get();
                        iterBarriers[0].Transition.StateBefore =
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                        iterBarriers[0].Transition.StateAfter =
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                        iterBarriers[0].Transition.Subresource =
                            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                        iterBarriers[1] = iterBarriers[0];
                        iterBarriers[1].Transition.pResource =
                            svgfAtrousScratch[nextWrite].Get();
                        iterBarriers[1].Transition.StateBefore =
                            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                        iterBarriers[1].Transition.StateAfter =
                            D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                        // Iteration 0 wrote scratchA while scratchB was never
                        // transitioned out of UNORDERED_ACCESS, so only the
                        // first barrier applies on that boundary.
                        cmdList->ResourceBarrier(iter == 0 ? 1u : 2u,
                                                 iterBarriers);
                    }
                }
            }

            // Which scratch holds the result, derived from the same set
            // selection the loop used: set 0 and set 2 write scratchA, set 1
            // writes scratchB. Iteration 0 uses set 0; thereafter
            // setIndex = 2 - (iter & 1), so odd iterations use set 1.
            // => iteration 0 lands in A, and after that odd->B, even->A.
            const UINT lastIter = atrousIterationCount - 1u;
            const UINT finalIdx =
                (lastIter == 0u) ? 0u : ((lastIter & 1u) ? 1u : 0u);
            const UINT compOutIdx = finalIdx ^ 1u;
            {
                // finalIdx was written by the last iteration and never
                // transitioned, so it is still UNORDERED_ACCESS: move it to
                // SRV for the composite to read.
                D3D12_RESOURCE_BARRIER atrousOutBarriers[2] = {};
                atrousOutBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                atrousOutBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                atrousOutBarriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                atrousOutBarriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                atrousOutBarriers[0].Transition.pResource = svgfAtrousScratch[finalIdx].Get();
                UINT atrousOutCount = 1u;
                // The composite writes the *other* scratch as a UAV. With more
                // than one iteration the per-iteration barriers left it in
                // NON_PIXEL_SHADER_RESOURCE, so it has to come back. With a
                // single iteration it was never moved and is already UAV.
                if (atrousIterationCount > 1u) {
                    atrousOutBarriers[1] = atrousOutBarriers[0];
                    atrousOutBarriers[1].Transition.pResource =
                        svgfAtrousScratch[compOutIdx].Get();
                    atrousOutBarriers[1].Transition.StateBefore =
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                    atrousOutBarriers[1].Transition.StateAfter =
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                    atrousOutCount = 2u;
                }
                cmdList->ResourceBarrier(atrousOutCount, atrousOutBarriers);
            }

            // Transition outputTexture from UAV (resolve write) to SRV (composite read).
            {
                D3D12_RESOURCE_BARRIER outToSRV = {};
                outToSRV.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                outToSRV.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                outToSRV.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                outToSRV.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                outToSRV.Transition.pResource = outputTexture.Get();
                cmdList->ResourceBarrier(1, &outToSRV);
            }

            // Build composite descriptor heap.
            // [0] t0: outputTexture   [1] t1: srcReflection
            // [2] t2: filteredReflection   [3] u0: compositeOutput (compOutIdx scratch)
            {
                D3D12_CPU_DESCRIPTOR_HANDLE h =
                    compositeDescHeap->GetCPUDescriptorHandleForHeapStart();

                D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
                srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                srv.Texture2D.MipLevels = 1;

                // [0] t0: outputTexture (lit result)
                g_dx12.device->CreateShaderResourceView(outputTexture.Get(), &srv, h);

                // [1] t1: reflectionSrc
                D3D12_CPU_DESCRIPTOR_HANDLE h1 = h; h1.ptr += (UINT64)descSize;
                g_dx12.device->CreateShaderResourceView(svgfReflectionSrc.Get(), &srv, h1);

                // [2] t2: filtered reflection (from atrous final iteration)
                D3D12_CPU_DESCRIPTOR_HANDLE h2 = h; h2.ptr += (UINT64)descSize * 2;
                g_dx12.device->CreateShaderResourceView(svgfAtrousScratch[finalIdx].Get(), &srv, h2);

                // [3] u0: composite output (other scratch)
                D3D12_CPU_DESCRIPTOR_HANDLE h3 = h; h3.ptr += (UINT64)descSize * 3;
                D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
                uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
                uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                g_dx12.device->CreateUnorderedAccessView(svgfAtrousScratch[compOutIdx].Get(), nullptr, &uav, h3);
            }

            // Composite dispatch.
            {
                ProfilerDX12::Scope compScope(g_profiler, "SVGF Composite", cmdList);

                struct CompositeConstants {
                    UINT screenWidth;
                    UINT screenHeight;
                    UINT debugViewMode;
                    UINT pad0;
                };
                CompositeConstants cc = {};
                cc.screenWidth = width;
                cc.screenHeight = height;
                cc.debugViewMode = (UINT)debugViewMode;
                cc.pad0 = 0u;
                const UINT64 constantOffset =
                    static_cast<UINT64>(frameSlot) * 256ull;
                memcpy(static_cast<BYTE*>(svgfCompositeConstantMapped) +
                           constantOffset,
                       &cc, sizeof(cc));

                cmdList->SetComputeRootSignature(svgfCompositeRootSig.Get());
                cmdList->SetPipelineState(svgfCompositePSO.Get());
                ID3D12DescriptorHeap* compHeaps[] = { compositeDescHeap };
                cmdList->SetDescriptorHeaps(1, compHeaps);
                cmdList->SetComputeRootConstantBufferView(0,
                    svgfCompositeConstantBuffer->GetGPUVirtualAddress() +
                        constantOffset);
                cmdList->SetComputeRootDescriptorTable(1,
                    compositeDescHeap->GetGPUDescriptorHandleForHeapStart());

                cmdList->Dispatch(groupsX, groupsY, 1);
            }

            // Copy composite output back to outputTexture.
            {
                D3D12_RESOURCE_BARRIER copyBarriers[2] = {};
                copyBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                copyBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                copyBarriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                copyBarriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                copyBarriers[0].Transition.pResource = svgfAtrousScratch[compOutIdx].Get();

                copyBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                copyBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                copyBarriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
                copyBarriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                copyBarriers[1].Transition.pResource = outputTexture.Get();
                cmdList->ResourceBarrier(2, copyBarriers);

                cmdList->CopyResource(outputTexture.Get(), svgfAtrousScratch[compOutIdx].Get());

                // Transition back: scratch to SRV (for next frame), outputTexture stays COPY_DEST
                // but the existing barrier below transitions it to SRV.
                copyBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
                copyBarriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                cmdList->ResourceBarrier(1, copyBarriers);

                // Transition reflectionSrc back to UAV for next frame's resolve write.
                D3D12_RESOURCE_BARRIER reflBarrier = {};
                reflBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                reflBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                reflBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                reflBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                reflBarrier.Transition.pResource = svgfReflectionSrc.Get();
                cmdList->ResourceBarrier(1, &reflBarrier);

                // Normalise both scratch textures to UNORDERED_ACCESS so the
                // next frame starts from a known state whatever the iteration
                // count was. Without this the end state depends on parity, and
                // a barrier whose StateBefore does not match the actual state
                // is a validation error that only appears at some iteration
                // counts -- the kind of bug that survives testing at N=5 and
                // fires the first time someone picks N=4.
                D3D12_RESOURCE_BARRIER resetBarriers[2] = {};
                resetBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                resetBarriers[0].Transition.StateBefore =
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                resetBarriers[0].Transition.StateAfter =
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                resetBarriers[0].Transition.Subresource =
                    D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                resetBarriers[0].Transition.pResource =
                    svgfAtrousScratch[finalIdx].Get();
                resetBarriers[1] = resetBarriers[0];
                resetBarriers[1].Transition.pResource =
                    svgfAtrousScratch[compOutIdx].Get();
                cmdList->ResourceBarrier(2, resetBarriers);
            }
        }

        // Keep linear HDR, motion vectors, and surface data as SRVs.
        // When the atrous pass ran, outputTexture was used as COPY_DEST rather
        // than UAV; the StateBefore must reflect that or the D3D runtime
        // validates the barrier as a no-op and the texture stays in COPY_DEST.
        {
            D3D12_RESOURCE_BARRIER barriers[3] = {};
            for (UINT i = 0; i < 3; ++i) {
                barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barriers[i].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                barriers[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            }
            barriers[0].Transition.StateBefore = atrousRan
                ? D3D12_RESOURCE_STATE_COPY_DEST
                : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            barriers[0].Transition.pResource = outputTexture.Get();
            barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            barriers[1].Transition.pResource = motionTexture.Get();
            barriers[2].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            barriers[2].Transition.pResource = normalRoughnessTexture.Get();
            cmdList->ResourceBarrier(3, barriers);
        }
        if (rayReconstructionActive) {
            ID3D12Resource* guides[] = { rrDiffuseAlbedo.Get(),
                rrSpecularAlbedo.Get(), rrSpecularHitDistance.Get() };
            D3D12_RESOURCE_BARRIER barriers[3] = {};
            for (UINT i = 0; i < 3; ++i) {
                barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                barriers[i].Transition.pResource = guides[i];
                barriers[i].Transition.StateBefore =
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                barriers[i].Transition.StateAfter =
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                barriers[i].Transition.Subresource =
                    D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            }
            cmdList->ResourceBarrier(3, barriers);
        }

        // Preserve primary depth for the post reactive mask.
        if (!ScopeSurfaceBound()) SnapshotVisibilityDepth(cmdList);

        // Hand the main view back its history flag. The scope forced it false
        // for its own shading above and, having written no history, has no
        // opinion on whether the shared textures are valid -- only the view
        // that fills them does.
        if (ScopeSurfaceBound()) svgfHistoryValid = savedSVGFHistoryValid;
    }

    // Copies the scene depth (NON_PIXEL_SHADER_RESOURCE, left there) into
    // visibilityDepthTexture: GTAO's static-caster depth and the post
    // reactive mask. Retaken after the post-RR depth replay, or those two
    // compare jittered against unjittered depth and misclassify every pixel
    // on a steep depth gradient.
    void SnapshotVisibilityDepth(ID3D12GraphicsCommandList* cmdList) {
        {
            // A full-screen depth CopyResource plus four transitions. Small per
            // pixel but not free at high resolution, and it ran inside the
            // aggregate "VB Resolve" with no scope of its own.
            ProfilerDX12::Scope depthCopyScope(
                g_profiler, "VB Depth Snapshot", cmdList);
            D3D12_RESOURCE_BARRIER barriers[3] = {};
            barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[0].Transition.pResource = ActiveDepthBuffer();
            barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barriers[1] = barriers[0];
            barriers[1].Transition.pResource = visibilityDepthTexture.Get();
            barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            cmdList->ResourceBarrier(2, barriers);
            cmdList->CopyResource(visibilityDepthTexture.Get(),
                                  ActiveDepthBuffer());
            barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            cmdList->ResourceBarrier(2, barriers);
        }
    }

    void UpdateExposure(ID3D12GraphicsCommandList* cmdList) {
        if (exposureReadable) {
            D3D12_RESOURCE_BARRIER transition = {};
            transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            transition.Transition.pResource = exposureState.Get();
            transition.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            transition.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            transition.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            cmdList->ResourceBarrier(1, &transition);
        }

        VBExposureConstants constants = {};
        constants.inputWidth = width;
        constants.inputHeight = height;
        constants.adaptationRate = exposureAdaptation;
        constants.middleGray = 0.18f;
        exposureConstantBuffer.CopyData(g_dx12.frameIndex, constants);

        cmdList->SetComputeRootSignature(exposureRootSig.Get());
        ID3D12DescriptorHeap* heaps[] = { exposureDescHeap.Get() };
        cmdList->SetDescriptorHeaps(1, heaps);
        cmdList->SetComputeRootConstantBufferView(0,
            exposureConstantBuffer.GetGPUAddress(g_dx12.frameIndex));
        cmdList->SetComputeRootDescriptorTable(1,
            exposureDescHeap->GetGPUDescriptorHandleForHeapStart());

        D3D12_RESOURCE_BARRIER uav = {};
        uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        uav.UAV.pResource = exposureState.Get();
        cmdList->SetPipelineState(exposureResetPSO.Get());
        cmdList->Dispatch(1, 1, 1);
        cmdList->ResourceBarrier(1, &uav);
        cmdList->SetPipelineState(exposureAccumulatePSO.Get());
        cmdList->Dispatch((width + 127) / 128, (height + 127) / 128, 1);
        cmdList->ResourceBarrier(1, &uav);
        cmdList->SetPipelineState(exposureFinalizePSO.Get());
        cmdList->Dispatch(1, 1, 1);
        cmdList->ResourceBarrier(1, &uav);

        D3D12_RESOURCE_BARRIER transition = {};
        transition.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        transition.Transition.pResource = exposureState.Get();
        transition.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        transition.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        transition.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &transition);
        exposureReadable = true;
    }

    // Composite forward-only materials into the same linear HDR image produced
    // by the visibility resolve. Post-processing must run after this range.
    void BeginHDRBackground(ID3D12GraphicsCommandList* cmdList) {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = outputTexture.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &barrier);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = GetOutputRTV();
        // Never preserve previous-frame HDR contents. If sky initialization
        // fails or its draw is skipped, background resolve intentionally leaves
        // untouched pixels alone, so an uncleared target becomes feedback.
        const float clearColor[4] = {};
        cmdList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);
        cmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        cmdList->RSSetViewports(1, &ActiveViewport());
        cmdList->RSSetScissorRects(1, &ActiveScissor());
    }

    void EndHDRBackground(ID3D12GraphicsCommandList* cmdList) {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = outputTexture.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &barrier);
    }

    void BeginForwardExtensions(ID3D12GraphicsCommandList* cmdList) {
        const bool useMotion = extensionMotionVectors && !ScopeSurfaceBound();
        UINT barrierCount = useMotion ? 3u : 2u;
        D3D12_RESOURCE_BARRIER barriers[3] = {};
        barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[0].Transition.pResource = outputTexture.Get();
        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[1] = barriers[0];
        barriers[1].Transition.pResource = ActiveDepthBuffer();
        barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        if (useMotion) {
            barriers[2].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[2].Transition.pResource = motionTexture.Get();
            barriers[2].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            barriers[2].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barriers[2].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        }
        cmdList->ResourceBarrier(barrierCount, barriers);

        // Preserve the visibility pass's dense motion field. Skinned draws
        // overwrite their own pixels through BeginMotionDraws; clearing here
        // would erase camera motion for every background pixel.

        // Default to colour-only so untouched passes keep working.
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = GetOutputRTV();
        D3D12_CPU_DESCRIPTOR_HANDLE dsv =
            ActiveDSV();
        cmdList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        cmdList->RSSetViewports(1, &ActiveViewport());
        cmdList->RSSetScissorRects(1, &ActiveScissor());
    }

    // Bind colour + motion for draws that use an extension-motion PSO. No-op
    // when the toggle is off, so callers can bracket unconditionally.
    void BeginMotionDraws(ID3D12GraphicsCommandList* cmdList) {
        if (!extensionMotionVectors || ScopeSurfaceBound()) return;
        D3D12_CPU_DESCRIPTOR_HANDLE rtvs[2] = { GetOutputRTV(), GetMotionRTV() };
        D3D12_CPU_DESCRIPTOR_HANDLE dsv =
            ActiveDSV();
        cmdList->OMSetRenderTargets(2, rtvs, FALSE, &dsv);
    }

    // Restore colour-only for the single-RT passes that follow.
    void EndMotionDraws(ID3D12GraphicsCommandList* cmdList) {
        if (!extensionMotionVectors || ScopeSurfaceBound()) return;
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = GetOutputRTV();
        D3D12_CPU_DESCRIPTOR_HANDLE dsv =
            ActiveDSV();
        cmdList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    }

    void EndForwardExtensions(ID3D12GraphicsCommandList* cmdList) {
        const bool useMotion = extensionMotionVectors && !ScopeSurfaceBound();
        UINT barrierCount = useMotion ? 3u : 2u;
        D3D12_RESOURCE_BARRIER barriers[3] = {};
        barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[0].Transition.pResource = outputTexture.Get();
        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[1] = barriers[0];
        barriers[1].Transition.pResource = ActiveDepthBuffer();
        barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        if (useMotion) {
            barriers[2].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[2].Transition.pResource = motionTexture.Get();
            barriers[2].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barriers[2].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            barriers[2].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        }
        cmdList->ResourceBarrier(barrierCount, barriers);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE GetOutputRTV() const {
        return outputRtvHeap->GetCPUDescriptorHandleForHeapStart();
    }

    ID3D12Resource* GetOutputResource() const { return outputTexture.Get(); }
    ID3D12Resource* GetMotionResource() const { return motionTexture.Get(); }
    D3D12_CPU_DESCRIPTOR_HANDLE GetMotionRTV() const {
        return motionRtvHeap
            ? motionRtvHeap->GetCPUDescriptorHandleForHeapStart()
            : D3D12_CPU_DESCRIPTOR_HANDLE{};
    }
    ID3D12Resource* GetRRDiffuseAlbedo() const {
        return rrDiffuseAlbedo.Get();
    }
    ID3D12Resource* GetRRSpecularAlbedo() const {
        return rrSpecularAlbedo.Get();
    }
    ID3D12Resource* GetRRSpecularHitDistance() const {
        return rrSpecularHitDistance.Get();
    }
    bool EnsureRayReconstructionGuides() {
        if (rrDiffuseAlbedo && rrSpecularAlbedo && rrSpecularHitDistance)
            return true;
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        auto create = [&](ComPtr<ID3D12Resource>& target) {
            return SUCCEEDED(g_dx12.device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                nullptr, IID_PPV_ARGS(&target)));
        };
        if (!create(rrDiffuseAlbedo) || !create(rrSpecularAlbedo))
            return false;
        desc.Format = DXGI_FORMAT_R16_FLOAT;
        if (!create(rrSpecularHitDistance)) return false;
        ++rrGuideGeneration;
        return true;
    }
    ID3D12Resource* GetNormalRoughnessResource() const {
        return normalRoughnessTexture.Get();
    }
    ID3D12Resource* GetVisibilityDepthResource() const {
        return visibilityDepthTexture.Get();
    }

    void PostProcess(ID3D12GraphicsCommandList* cmdList, bool allowHistory,
                     bool dlssUpscaled = false) {
        const bool scaledInput = width != displayWidth || height != displayHeight;
        const UINT historyIndex = postFrameIndex & 1u;
        const bool preserveDebugOutput =
            debugViewMode != 0 || BentNormalGTAODiagnosticActive();
        PrepareStableSurfaceHistory(
            allowHistory && !scaledInput && StableSurfaceIdentityRequired(
                enhancedResolveExecutedLastFrame),
            StableSurfaceModeSignature(
                allowHistory, allowHistory && enhancedResolveExecutedLastFrame));
        // Every lens artefact is driven from the bloom buffer, so the bloom
        // chain has to run whenever any of them is enabled -- not just when
        // bloom itself is visible. Skipping it would leave the previous
        // frame's mips in place and light the dirt from stale highlights.
        const bool lensEffectsActive = sunLensSystemEnabled &&
            (lensDirtStrength > 0.0f || lensFlareStrength > 0.0f);
        const bool bloomChainRan = !validationMode && !preserveDebugOutput &&
            (bloomStrength > 0.0f || lensEffectsActive);
        if (bloomChainRan)
            RenderBloom(cmdList, dlssUpscaled && dlssUpscaledTexture &&
                                     bloomFromUpscaled);
        // The flare consumes the bloom pyramid, so it has to follow it. Gated
        // on the flare strength alone: dirt and the anamorphic term in post do
        // not need this buffer, and skipping the four dispatches is the whole
        // saving when the lens system is off.
        const bool flarePassRan = bloomChainRan && lensEffectsActive &&
            lensFlareStrength > 0.0f;
        if (flarePassRan)
            RenderFlare(cmdList);
        ReportFlareDiagnostics(flarePassRan, bloomChainRan);
        D3D12_RESOURCE_BARRIER barriers[2] = {};
        barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[0].Transition.pResource = presentTexture.Get();
        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[1] = barriers[0];
        barriers[1].Transition.pResource = historyTextures[historyIndex].Get();
        barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        cmdList->ResourceBarrier(2, barriers);

        VBPostConstants constants = {};
        constants.outputWidth = displayWidth;
        constants.outputHeight = displayHeight;
        constants.exposure = validationMode ? 1.0f : exposure * g_exposureScale;
        constants.bloomStrength = validationMode ? 0.0f : bloomStrength;
        constants.vignetteStrength = validationMode ? 0.0f : vignetteStrength;
        constants.grainStrength = validationMode ? 0.0f : grainStrength;
        constants.frameIndex = postFrameIndex++;
        constants.historyValid = (temporalEffectsEnabled && !dlssActive &&
            !scaledInput &&
            !validationMode &&
            !preserveDebugOutput && allowHistory && temporalHistoryValid)
                ? 1u : 0u;
        constants.taaFeedback = (temporalEffectsEnabled && !dlssActive &&
            !scaledInput &&
            !validationMode)
            ? taaFeedback : 0.0f;
        constants.motionBlurStrength = (temporalEffectsEnabled &&
            !validationMode && !scaledInput) ? motionBlurStrength : 0.0f;
        constants.focusDistance = focusDistance;
        // Depth of field disabled. It blurred the entire game view whenever
        // parity validation was off.
        constants.aperture = 0.0f;
        constants.nearPlane = currentNearPlane;
        constants.farPlane = currentFarPlane;
        constants.debugViewMode = preserveDebugOutput
            ? (std::max)(1u, static_cast<UINT>(debugViewMode)) : 0u;
        constants.validationMode = validationMode ? 1u : 0u;
        // Exact surface correspondence is available once a history frame has
        // been captured. Suppressed in parity mode, which compares against the
        // Forward renderer and must not gain a temporal advantage.
        constants.surfaceHistoryValid =
            (surfaceHistoryValid && stableSurfaceIdentityActiveThisFrame &&
             (surfaceIDTemporalEnabled || historyDebugView) && !validationMode)
                ? 1u : 0u;
        constants.historyDebugView = historyDebugView && !scaledInput ? 1u : 0u;
        constants.surfaceIdentityEnabled =
            stableSurfaceIdentityActiveThisFrame ? 1u : 0u;
        // Bloom-derived artefacts are only valid when the chain ran this frame.
        constants.lensDirtStrength =
            bloomChainRan && sunLensSystemEnabled ? lensDirtStrength : 0.0f;
        constants.lensDirtScale = lensDirtScale;
        // Gated on the flare pass actually having run this frame, not merely on
        // the slider: post samples the flare buffer unconditionally, and on a
        // frame where the passes were skipped that buffer still holds the last
        // frame it did run, which would smear a stale flare over the image.
        constants.lensFlareStrength = flarePassRan ? lensFlareStrength : 0.0f;
        // Aberration resamples the HDR input directly, so it does not depend on
        // the bloom chain -- only on being in a normal (non-parity) view.
        constants.chromaticAberration =
            (validationMode || preserveDebugOutput || !sunLensSystemEnabled)
                ? 0.0f : chromaticAberration;
        constants.sunLensPosition =
            (validationMode || preserveDebugOutput)
                ? XMFLOAT4(0.5f, 0.5f, 0.0f, 0.0f)
                : sunLensPosition;
        constants.sunLensColor = sunLensColor;
        // The existing unused component carries the projected solar radius;
        // no cbuffer offsets or descriptor tables change for the new variant.
        if (highQualityLensEnabled)
            constants.sunLensColor.w = sunLensRadiusUV;
        postConstantBuffer.CopyData(g_dx12.frameIndex, constants);

        cmdList->SetComputeRootSignature(postRootSig.Get());
        cmdList->SetPipelineState(highQualityLensEnabled && qualityLensPipelineReady && !validationMode &&
            !preserveDebugOutput && sunLensSystemEnabled
            ? (scaledInput ? postUpscaleQualityLensPSO.Get() : postQualityLensPSO.Get())
            : (scaledInput ? postUpscalePSO.Get() : postPSO.Get()));
        ID3D12DescriptorHeap* heaps[] = { postDescHeap.Get() };
        cmdList->SetDescriptorHeaps(1, heaps);
        cmdList->SetComputeRootConstantBufferView(0,
            postConstantBuffer.GetGPUAddress(g_dx12.frameIndex));
        D3D12_GPU_DESCRIPTOR_HANDLE table =
            postDescHeap->GetGPUDescriptorHandleForHeapStart();
        const UINT descriptorVariant =
            (dlssUpscaled ? 4u : 0u) +
            historyIndex * 2u + stableSurfaceWriteIndex;
        table.ptr += (UINT64)g_dx12.cbvSrvUavDescriptorSize *
                     descriptorVariant * kPostDescriptorsPerVariant;
        cmdList->SetComputeRootDescriptorTable(1, table);
        cmdList->Dispatch((displayWidth + 7) / 8,
                          (displayHeight + 7) / 8, 1);

        if (stableSurfaceIdentityActiveThisFrame) {
            D3D12_RESOURCE_BARRIER stableBarriers[2] = {};
            for (UINT i = 0; i < 2; ++i) {
                stableBarriers[i].Type =
                    D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                stableBarriers[i].Transition.Subresource =
                    D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            }
            stableBarriers[0].Transition.pResource =
                StableSurfaceResource(stableSurfaceWriteIndex);
            stableBarriers[0].Transition.StateBefore =
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            stableBarriers[0].Transition.StateAfter =
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            stableBarriers[1].Transition.pResource =
                StableSurfaceResource(stableSurfaceWriteIndex ^ 1u);
            stableBarriers[1].Transition.StateBefore =
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            stableBarriers[1].Transition.StateAfter =
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            cmdList->ResourceBarrier(2, stableBarriers);
            stableSurfaceWriteIndex ^= 1u;
            surfaceHistoryValid = true;
        } else {
            surfaceHistoryValid = false;
        }

        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        cmdList->ResourceBarrier(2, barriers);
        D3D12_RESOURCE_BARRIER depth = {};
        depth.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        depth.Transition.pResource = ActiveDepthBuffer();
        depth.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        depth.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        depth.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &depth);
        // Debug colours are not scene radiance. Never seed lit TAA history
        // with them; the first frame after returning to Lit starts cleanly.
        temporalHistoryValid = temporalEffectsEnabled && !preserveDebugOutput;
    }

    // Generic destination copy. The scope's lens texture is not a swapchain
    // image, so the destination and its surrounding states are parameters
    // rather than the backbuffer's fixed RENDER_TARGET bracket.
    void CopyToDestination(ID3D12GraphicsCommandList* cmdList,
                           ID3D12Resource* destination,
                           D3D12_RESOURCE_STATES destinationState) {
        if (!destination) return;

        if (destinationState != D3D12_RESOURCE_STATE_COPY_DEST) {
            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = destination;
            barrier.Transition.StateBefore = destinationState;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            cmdList->ResourceBarrier(1, &barrier);
        }

        cmdList->CopyResource(destination, presentTexture.Get());

        if (destinationState != D3D12_RESOURCE_STATE_COPY_DEST) {
            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = destination;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            barrier.Transition.StateAfter = destinationState;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            cmdList->ResourceBarrier(1, &barrier);
        }
    }

    // Copies the resolve output -- linear radiance, before any post -- into an
    // external target.
    //
    // The sniper scope uses this instead of CopyToDestination. Post is a
    // main-camera presentation layer (flare, dirt, exposure adaptation, tone
    // map) and applying it per-view made the lens disagree with the frame that
    // contains it. Taking the image at the resolve leaves the scope with the
    // full visibility-buffer lighting and lets the main view's single tone map
    // cover the lens as well.
    //
    // outputTexture lives in NON_PIXEL_SHADER_RESOURCE between passes, so
    // unlike presentTexture it has to be walked to COPY_SOURCE and back.
    void CopyResolveOutputTo(ID3D12GraphicsCommandList* cmdList,
                             ID3D12Resource* destination,
                             D3D12_RESOURCE_STATES destinationState) {
        if (!destination || !outputTexture) return;

        D3D12_RESOURCE_BARRIER toCopy[2] = {};
        UINT barrierCount = 0;
        toCopy[barrierCount].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toCopy[barrierCount].Transition.pResource = outputTexture.Get();
        toCopy[barrierCount].Transition.StateBefore =
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        toCopy[barrierCount].Transition.StateAfter =
            D3D12_RESOURCE_STATE_COPY_SOURCE;
        toCopy[barrierCount].Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        ++barrierCount;
        if (destinationState != D3D12_RESOURCE_STATE_COPY_DEST) {
            toCopy[barrierCount].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            toCopy[barrierCount].Transition.pResource = destination;
            toCopy[barrierCount].Transition.StateBefore = destinationState;
            toCopy[barrierCount].Transition.StateAfter =
                D3D12_RESOURCE_STATE_COPY_DEST;
            toCopy[barrierCount].Transition.Subresource =
                D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            ++barrierCount;
        }
        cmdList->ResourceBarrier(barrierCount, toCopy);

        cmdList->CopyResource(destination, outputTexture.Get());

        D3D12_RESOURCE_BARRIER fromCopy[2] = {};
        barrierCount = 0;
        fromCopy[barrierCount].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        fromCopy[barrierCount].Transition.pResource = outputTexture.Get();
        fromCopy[barrierCount].Transition.StateBefore =
            D3D12_RESOURCE_STATE_COPY_SOURCE;
        fromCopy[barrierCount].Transition.StateAfter =
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        fromCopy[barrierCount].Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        ++barrierCount;
        if (destinationState != D3D12_RESOURCE_STATE_COPY_DEST) {
            fromCopy[barrierCount].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            fromCopy[barrierCount].Transition.pResource = destination;
            fromCopy[barrierCount].Transition.StateBefore =
                D3D12_RESOURCE_STATE_COPY_DEST;
            fromCopy[barrierCount].Transition.StateAfter = destinationState;
            fromCopy[barrierCount].Transition.Subresource =
                D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            ++barrierCount;
        }
        cmdList->ResourceBarrier(barrierCount, fromCopy);
    }

    // Main-view wrapper. Unchanged behaviour: the backbuffer arrives in
    // RENDER_TARGET from BeginFrame and must be returned to it for ImGui.
    void CopyToBackBuffer(ID3D12GraphicsCommandList* cmdList) {
        CopyToDestination(cmdList,
                          g_dx12.renderTargets[g_dx12.frameIndex].Get(),
                          D3D12_RESOURCE_STATE_RENDER_TARGET);
    }

    void Resize(UINT newWidth, UINT newHeight,
                UINT newDisplayWidth = 0, UINT newDisplayHeight = 0) {
        const UINT targetDisplayWidth = newDisplayWidth ? newDisplayWidth : newWidth;
        const UINT targetDisplayHeight = newDisplayHeight ? newDisplayHeight : newHeight;
        if (newWidth == width && newHeight == height &&
            targetDisplayWidth == displayWidth &&
            targetDisplayHeight == displayHeight) return;
        width = newWidth;
        height = newHeight;
        displayWidth = targetDisplayWidth;
        displayHeight = targetDisplayHeight;

        terrainScreenDisplacement.ResetResources();
        terrainScreenDisplacementPrepared = false;
        terrainScreenDisplacementActiveThisFrame = false;
        visBufferRT.Reset();
        surfaceHistoryValid = false;
        radianceCascades.ResetResources();
        variableRateGI.ResetResources();
        variableRateGIActive = false;
        emissiveCGNSHistoryValid = false;
        outputTexture.Reset();
        dlssUpscaledTexture.Reset();
        presentTexture.Reset();
        motionTexture.Reset();
        motionRtvHeap.Reset();
        normalRoughnessTexture.Reset();
        rrDiffuseAlbedo.Reset();
        rrSpecularAlbedo.Reset();
        rrSpecularHitDistance.Reset();
        bloomTexture.Reset();
        visibilityDepthTexture.Reset();
        historyTextures[0].Reset();
        historyTextures[1].Reset();
        svgfHistoryColor[0].Reset();
        svgfHistoryColor[1].Reset();
        svgfHistoryMoments[0].Reset();
        svgfHistoryMoments[1].Reset();
        svgfStableSurfaceCurrent.Reset();
        svgfStableSurfaceHistory.Reset();
        restirSampleBuffer.Reset();
        restirWeightBuffer.Reset();
        restirWidth = restirHeight = 0;
        emissiveReservoirBuffer.Reset();
        emissiveReservoirWidth = emissiveReservoirHeight = 0;
        emissiveReservoirAllocationFailed = false;
        restirAllocationFailed = false;
        restirHistoryValid = false;
        stableSurfaceWriteIndex = 0;
        stableSurfaceIdentityActive = false;
        stableSurfaceIdentityActiveThisFrame = false;
        stableSurfaceModeSignature = ~0u;
        // À-trous scratch and the reflection source are screen-sized too, so
        // they must be released here or the next Init recreates everything
        // else at the new resolution while these keep the old dimensions --
        // the dispatch then reads and writes out of bounds.
        svgfReflectionSrc.Reset();
        svgfAtrousScratch[0].Reset();
        svgfAtrousScratch[1].Reset();
        svgfHistoryPing = 0;
        svgfHistoryValid = false;

        exposureState.Reset();
        temporalHistoryValid = false;
        exposureReadable = false;
        visRtvHeap.Reset();
        outputRtvHeap.Reset();
        depthReplayRT.Reset();
        depthReplayRtvHeap.Reset();
        // Screen-sized like the rest; without this the enhanced resolve would
        // keep writing its mask at the old dimensions after a window resize.
        rayMaskTexture.Reset();
        rayMaskReadback.Reset();

        CreateVisBufferRT();
        CreateOutputTexture();
        // The tile grid is derived from the resolution and the classify heap's
        // t0 points at the visibility buffer CreateVisBufferRT just replaced,
        // so both the buffers and their descriptors must be rebuilt here.
        CreateTileClassifyResources();
        if (enhancedPipelineReady) {
            CreateRayMaskResources();
            // Each frame heap points at destroyed screen-sized resources, so
            // all slots must be rebuilt as they become current.
            for (UINT frame = 0; frame < FRAME_COUNT; ++frame) {
                enhancedComputeDescHeaps[frame].Reset();
                enhancedHeapTLASAddresses[frame] = 0;
            }
        }
        if (svgfAtrousPipelineReady) {
            D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
            heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            bool heapRebuildFailed = false;
            for (UINT frame = 0; frame < FRAME_COUNT; ++frame) {
                svgfAtrousDescHeaps[frame].Reset();
                heapDesc.NumDescriptors = 18;
                if (FAILED(g_dx12.device->CreateDescriptorHeap(
                        &heapDesc,
                        IID_PPV_ARGS(&svgfAtrousDescHeaps[frame])))) {
                    heapRebuildFailed = true;
                    break;
                }
                svgfCompositeDescHeaps[frame].Reset();
                heapDesc.NumDescriptors = 4;
                if (FAILED(g_dx12.device->CreateDescriptorHeap(
                        &heapDesc,
                        IID_PPV_ARGS(&svgfCompositeDescHeaps[frame])))) {
                    heapRebuildFailed = true;
                    break;
                }
            }
            if (heapRebuildFailed)
                svgfAtrousPipelineReady = false;
        }
        UpdateComputeDescriptors();
        UpdateBloomDescriptors();
        UpdateFlareDescriptors();
        UpdatePostDescriptors();
        UpdateExposureDescriptors();
    }

private:
    UINT BuildBindlessResolveTable(ID3D12DescriptorHeap* sourceHeap,
                                   UINT descriptorCount, UINT frameSlot) {
        if (!bindlessHeap || !bindlessHeap->Initialized() || !sourceHeap ||
            descriptorCount <= 7)
            return BINDLESS_INVALID_INDEX;

        std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> sources(descriptorCount);
        D3D12_CPU_DESCRIPTOR_HANDLE source =
            sourceHeap->GetCPUDescriptorHandleForHeapStart();
        for (UINT i = 0; i < descriptorCount; ++i) {
            sources[i] = source;
            source.ptr += g_dx12.cbvSrvUavDescriptorSize;
        }
        const UINT base = bindlessResolveTableBases[frameSlot % FRAME_COUNT];
        if (base == BINDLESS_INVALID_INDEX) return base;
        bindlessHeap->CopyTransientTable(base, sources.data(), descriptorCount);
        WriteBindlessMaterialRecords(base);
        return base;
    }

    // Same table in a fresh transient range: the cascade passes need one per
    // pass, alive together, on top of the reserved resolve table.
    UINT AllocateBindlessResolveTable(ID3D12DescriptorHeap* sourceHeap,
                                      UINT descriptorCount) {
        if (!bindlessHeap || !bindlessHeap->Initialized() || !sourceHeap ||
            descriptorCount <= 7)
            return BINDLESS_INVALID_INDEX;
        std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> sources(descriptorCount);
        D3D12_CPU_DESCRIPTOR_HANDLE source =
            sourceHeap->GetCPUDescriptorHandleForHeapStart();
        for (UINT i = 0; i < descriptorCount; ++i) {
            sources[i] = source;
            source.ptr += g_dx12.cbvSrvUavDescriptorSize;
        }
        const UINT base = bindlessHeap->AllocateTransientTable(
            sources.data(), descriptorCount);
        if (base != BINDLESS_INVALID_INDEX) WriteBindlessMaterialRecords(base);
        return base;
    }

    void WriteBindlessMaterialRecords(UINT base) {
        // Slot 7 is t7. Point it at this frame's bindless material-record slice;
        // all other entries mirror the already-refreshed legacy/enhanced table.
        D3D12_SHADER_RESOURCE_VIEW_DESC materials = {};
        materials.Format = DXGI_FORMAT_UNKNOWN;
        materials.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        materials.Shader4ComponentMapping =
            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        materials.Buffer.FirstElement = ViewFrameIndex() * VB_MAX_MATERIALS;
        materials.Buffer.NumElements = VB_MAX_MATERIALS;
        materials.Buffer.StructureByteStride = sizeof(VBMaterialData);
        g_dx12.device->CreateShaderResourceView(
            bindlessMaterialDataBuffer.Get(), &materials,
            bindlessHeap->CpuHandleAt(base + 7));
    }

    // Writes the three terrain layer-array SRVs at `slot`, `slot+1`, `slot+2`.
    // A null resource still gets a typed descriptor: an unwritten slot inside a
    // bound table is undefined behaviour, while a null SRV reads as zero, which
    // the terrain shader already handles through its fallback colours.
    void WriteTerrainDescriptors(ID3D12DescriptorHeap* heap, UINT slot) {
        if (!heap) return;
        ID3D12Resource* const arrays[3] = {
            terrainAlbedoArray, terrainNormalArray, terrainMetalRoughArray
        };
        // Albedo is sRGB in the forward path; matching it here is what keeps
        // visibility terrain the same colour as forward terrain.
        const DXGI_FORMAT formats[3] = {
            DXGI_FORMAT_R8G8B8A8_UNORM_SRGB,
            DXGI_FORMAT_R8G8B8A8_UNORM,
            DXGI_FORMAT_R8G8B8A8_UNORM
        };
        D3D12_CPU_DESCRIPTOR_HANDLE handle =
            heap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(
            g_dx12.cbvSrvUavDescriptorSize) * slot;
        for (UINT i = 0; i < 3; ++i) {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
            srv.Format = formats[i];
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            srv.Shader4ComponentMapping =
                D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            if (arrays[i]) {
                const D3D12_RESOURCE_DESC desc = arrays[i]->GetDesc();
                srv.Texture2DArray.MipLevels = desc.MipLevels;
                srv.Texture2DArray.ArraySize = desc.DepthOrArraySize;
            } else {
                srv.Texture2DArray.MipLevels = 1;
                srv.Texture2DArray.ArraySize = 1;
            }
            g_dx12.device->CreateShaderResourceView(arrays[i], &srv, handle);
            handle.ptr += g_dx12.cbvSrvUavDescriptorSize;
        }

        // The splatmap slot sits three above the layer arrays on every tier
        // (default 87->90, enhanced 100->103), so it is written from the same
        // running handle. It must hold a valid descriptor even when no level
        // has painted terrain: a bound table with an untouched slot is
        // undefined behaviour, not a null read. A null SRV with a real
        // Format/ViewDimension is the documented way to express "no texture",
        // and it samples as zero -- which the shader reads as "unpainted".
        WriteTerrainSplatDescriptor(handle);
    }

    // Writes the t91 splatmap descriptor at an already-offset handle. UNORM,
    // not sRGB: these are blend weights, and an sRGB view would curve them.
    void WriteTerrainSplatDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE handle) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        g_dx12.device->CreateShaderResourceView(terrainSplatMap, &srv, handle);
    }

    // Writes the t92 spot shadow atlas descriptor. A null resource still needs
    // a valid view: the root signature declares the range, and the debug layer
    // rejects a bound table with an uninitialised slot even when the shader
    // never samples it (which is the case whenever nothing is casting).
    void WriteSpotShadowAtlasDescriptor(ID3D12DescriptorHeap* heap, UINT slot) {
        if (!heap) return;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Format = DXGI_FORMAT_R32_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2DArray.MipLevels = 1;
        srv.Texture2DArray.ArraySize = SPOT_SHADOW_COUNT;
        D3D12_CPU_DESCRIPTOR_HANDLE handle =
            heap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(
            g_dx12.cbvSrvUavDescriptorSize) * slot;
        g_dx12.device->CreateShaderResourceView(
            spotShadowAtlasResource, &srv, handle);
    }

    void WriteBentNormalHistoryDescriptor(ID3D12DescriptorHeap* heap,
                                          UINT slot,
                                          ID3D12Resource* history) {
        if (!heap) return;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE handle =
            heap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(
            g_dx12.cbvSrvUavDescriptorSize) * slot;
        g_dx12.device->CreateShaderResourceView(history, &srv, handle);
    }

    ID3D12DescriptorHeap* PrepareBentNormalResolveHeap(UINT frameSlot) {
        ID3D12DescriptorHeap* heap =
            bentNormalComputeDescHeaps[frameSlot % FRAME_COUNT].Get();
        if (!heap || !computeDescHeap) return nullptr;
        // Slots 0..85 include this frame's CBVs, refreshed immediately before
        // Resolve. Only the current frame slot is rewritten.
        g_dx12.device->CopyDescriptorsSimple(86,
            heap->GetCPUDescriptorHandleForHeapStart(),
            computeDescHeap->GetCPUDescriptorHandleForHeapStart(),
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        WriteBentNormalHistoryDescriptor(heap, 86, bentNormalGTAOHistory);
        // Slots 87..89 sit above the copied range, so they are written here as
        // well. A bound descriptor table must have every slot it declares
        // defined, even when this frame does not run the terrain variant.
        WriteTerrainDescriptors(heap, 87);
        return heap;
    }

    bool MotionVectorsRequired() const {
        if (ScopeSurfaceBound()) return false;
        // TAA owns post-process history, but SVGF independently needs the
        // visibility motion buffer to reproject reflection history. Capture
        // mode deliberately disables TAA, so tying this data to the TAA switch
        // pins SVGF history to screen space as soon as the camera moves.
        return temporalEffectsEnabled || dlssActive || aoTemporalMotionVectors ||
            (enhancedVisualsActive && enhancedRTReflectionsActive &&
             svgfTemporalEnabled);
    }

    bool CreateVisBufferRT() {
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R32G32_UINT;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_CLEAR_VALUE clearValue = {};
        clearValue.Format = DXGI_FORMAT_R32G32_UINT;

        HRESULT hr = g_dx12.device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, &clearValue,
            IID_PPV_ARGS(&visBufferRT));
        if (FAILED(hr)) {
            std::cerr << "Failed to create visibility buffer RT" << std::endl;
            return false;
        }

        // Create RTV heap
        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
        rtvHeapDesc.NumDescriptors = 1;
        rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hr = g_dx12.device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&visRtvHeap));
        if (FAILED(hr)) return false;

        // Create RTV
        D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
        rtvDesc.Format = DXGI_FORMAT_R32G32_UINT;
        rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        g_dx12.device->CreateRenderTargetView(visBufferRT.Get(), &rtvDesc,
            visRtvHeap->GetCPUDescriptorHandleForHeapStart());

        surfaceHistoryValid = false;

        return true;
    }

    bool CreateOutputTexture() {
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        HRESULT hr = g_dx12.device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
            IID_PPV_ARGS(&outputTexture));
        if (FAILED(hr)) {
            std::cerr << "Failed to create VB output texture" << std::endl;
            return false;
        }

        D3D12_DESCRIPTOR_HEAP_DESC outputHeap = {};
        outputHeap.NumDescriptors = 1;
        outputHeap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hr = g_dx12.device->CreateDescriptorHeap(
            &outputHeap, IID_PPV_ARGS(&outputRtvHeap));
        if (FAILED(hr)) return false;
        D3D12_RENDER_TARGET_VIEW_DESC outputRtv = {};
        outputRtv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        outputRtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        g_dx12.device->CreateRenderTargetView(outputTexture.Get(),
            &outputRtv, outputRtvHeap->GetCPUDescriptorHandleForHeapStart());

        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        desc.Width = displayWidth;
        desc.Height = displayHeight;
        hr = g_dx12.device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr,
            IID_PPV_ARGS(&presentTexture));
        if (FAILED(hr)) {
            std::cerr << "Failed to create VB present texture" << std::endl;
            return false;
        }

        if (width != displayWidth || height != displayHeight) {
            desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            hr = g_dx12.device->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                IID_PPV_ARGS(&dlssUpscaledTexture));
            if (FAILED(hr)) return false;
        }

        desc.Width = width;
        desc.Height = height;

        desc.Format = DXGI_FORMAT_R16G16_FLOAT;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        hr = g_dx12.device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
            IID_PPV_ARGS(&motionTexture));
        if (FAILED(hr)) return false;
        D3D12_DESCRIPTOR_HEAP_DESC motionHeap = {};
        motionHeap.NumDescriptors = 1;
        motionHeap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hr = g_dx12.device->CreateDescriptorHeap(
            &motionHeap, IID_PPV_ARGS(&motionRtvHeap));
        if (FAILED(hr)) return false;
        D3D12_RENDER_TARGET_VIEW_DESC motionRtv = {};
        motionRtv.Format = DXGI_FORMAT_R16G16_FLOAT;
        motionRtv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        g_dx12.device->CreateRenderTargetView(
            motionTexture.Get(), &motionRtv,
            motionRtvHeap->GetCPUDescriptorHandleForHeapStart());

        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        hr = g_dx12.device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
            IID_PPV_ARGS(&normalRoughnessTexture));
        if (FAILED(hr)) return false;

        D3D12_RESOURCE_DESC depthSnapshotDesc =
            ActiveDepthBuffer()->GetDesc();
        hr = g_dx12.device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &depthSnapshotDesc,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
            IID_PPV_ARGS(&visibilityDepthTexture));
        if (FAILED(hr)) return false;

        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.Width = displayWidth;
        desc.Height = displayHeight;
        for (UINT i = 0; i < 2; ++i) {
            hr = g_dx12.device->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                IID_PPV_ARGS(&historyTextures[i]));
            if (FAILED(hr)) return false;
        }

        desc.Width = width;
        desc.Height = height;

        // SVGF temporal accumulation: ping-pong history for denoised colour
        // (E[x]) and moments (E[x^2] + sample count in alpha).
        // Created in SRV state; the Resolve transitions the write-side to UAV
        // each frame and back to SRV afterwards.
        for (UINT i = 0; i < 2; ++i) {
            hr = g_dx12.device->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                IID_PPV_ARGS(&svgfHistoryColor[i]));
            if (FAILED(hr)) return false;
            hr = g_dx12.device->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                IID_PPV_ARGS(&svgfHistoryMoments[i]));
            if (FAILED(hr)) return false;
        }
        svgfHistoryPing = 0;
        svgfHistoryValid = false;

        // The visibility target keeps local SV_PrimitiveID for vertex lookup.
        // These UAV-capable peers ping-pong the authored identity so destruction
        // can regroup source triangles without invalidating temporal history.
        desc.Format = DXGI_FORMAT_R32G32_UINT;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        hr = g_dx12.device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&svgfStableSurfaceCurrent));
        if (FAILED(hr)) return false;
        hr = g_dx12.device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
            IID_PPV_ARGS(&svgfStableSurfaceHistory));
        if (FAILED(hr)) return false;
        stableSurfaceWriteIndex = 0;
        stableSurfaceIdentityActive = false;
        stableSurfaceIdentityActiveThisFrame = false;
        stableSurfaceModeSignature = ~0u;
        surfaceHistoryValid = false;
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        // SVGF à-trous: specular IBL output from the resolve + ping-pong scratch.
        // The reflection src is written by the enhanced resolve as a UAV and
        // read by the à-trous pass as an SRV. Scratch textures alternate state
        // per iteration then the final result is read by the composite pass.
        hr = g_dx12.device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&svgfReflectionSrc));
        if (FAILED(hr)) return false;
        for (UINT i = 0; i < 2; ++i) {
            hr = g_dx12.device->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                IID_PPV_ARGS(&svgfAtrousScratch[i]));
            if (FAILED(hr)) return false;
        }

        D3D12_RESOURCE_DESC exposureDesc = {};
        exposureDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        exposureDesc.Width = 3 * sizeof(UINT);
        exposureDesc.Height = 1;
        exposureDesc.DepthOrArraySize = 1;
        exposureDesc.MipLevels = 1;
        exposureDesc.SampleDesc.Count = 1;
        exposureDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        exposureDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        hr = g_dx12.device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &exposureDesc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&exposureState));
        if (FAILED(hr)) return false;
        return CreateBloomTexture();
    }

    bool CreateBloomTexture() {
        bloomWidth = (std::max)(1u, width / 2u);
        bloomHeight = (std::max)(1u, height / 2u);
        bloomMipCount = 1;
        UINT mipWidth = bloomWidth;
        UINT mipHeight = bloomHeight;
        while (bloomMipCount < VB_BLOOM_MAX_MIPS &&
               (mipWidth > 1u || mipHeight > 1u)) {
            mipWidth = (std::max)(1u, mipWidth / 2u);
            mipHeight = (std::max)(1u, mipHeight / 2u);
            ++bloomMipCount;
        }

        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = bloomWidth;
        desc.Height = bloomHeight;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = static_cast<UINT16>(bloomMipCount);
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(g_dx12.device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                IID_PPV_ARGS(&bloomTexture))))
            return false;
        // Sized from the bloom target, so it follows every resize for free.
        return CreateFlareTextures();
    }


    // Half of the bloom target, so a quarter of screen area. The flare is all
    // low-frequency -- wide blurs, soft ghosts -- so resolution buys nothing
    // here, and the small buffer is what makes a 21-tap streak affordable.
    //
    // Two textures rather than one: every pass reads the previous result while
    // writing the next, and a resource cannot be SRV and UAV in one dispatch.
    bool CreateFlareTextures() {
        flareWidth = (std::max)(1u, bloomWidth / 2u);
        flareHeight = (std::max)(1u, bloomHeight / 2u);

        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = flareWidth;
        desc.Height = flareHeight;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(g_dx12.device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                IID_PPV_ARGS(&flareTexture))))
            return false;
        return SUCCEEDED(g_dx12.device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
            IID_PPV_ARGS(&flareScratch)));
    }

    bool CreateStructuredBuffers() {
        auto CreateUpload = [](UINT64 size,
                               ComPtr<ID3D12Resource>& uploadBuf) -> bool {
            D3D12_HEAP_PROPERTIES uploadHeap = {};
            uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

            D3D12_RESOURCE_DESC bufDesc = {};
            bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bufDesc.Width = size;
            bufDesc.Height = 1;
            bufDesc.DepthOrArraySize = 1;
            bufDesc.MipLevels = 1;
            bufDesc.Format = DXGI_FORMAT_UNKNOWN;
            bufDesc.SampleDesc.Count = 1;
            bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

            return SUCCEEDED(g_dx12.device->CreateCommittedResource(
                &uploadHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&uploadBuf)));
        };

        auto CreateDefaultAndUpload = [&CreateUpload](UINT64 size,
                                          ComPtr<ID3D12Resource>& defaultBuf,
                                          ComPtr<ID3D12Resource>& uploadBuf) -> bool {
            D3D12_HEAP_PROPERTIES defaultHeap = {};
            defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

            D3D12_RESOURCE_DESC bufDesc = {};
            bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bufDesc.Width = size;
            bufDesc.Height = 1;
            bufDesc.DepthOrArraySize = 1;
            bufDesc.MipLevels = 1;
            bufDesc.Format = DXGI_FORMAT_UNKNOWN;
            bufDesc.SampleDesc.Count = 1;
            bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

            HRESULT hr = g_dx12.device->CreateCommittedResource(
                &defaultHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&defaultBuf));
            if (FAILED(hr)) return false;

            return CreateUpload(size, uploadBuf);
        };

        if (!CreateDefaultAndUpload(VB_MAX_DRAW_CALLS * sizeof(VBDrawCallData),
                                     drawCallBuffer, drawCallUpload[0]))
            return false;

        if (!CreateDefaultAndUpload(geometryVertexCapacity * sizeof(VBPackedVertex),
                                     vertexDataBuffer, vertexDataUpload[0]))
            return false;

        if (!CreateDefaultAndUpload(geometryIndexCapacity * sizeof(UINT),
                                     indexDataBuffer, indexDataUpload[0]))
            return false;

        if (!CreateDefaultAndUpload(geometryTriangleCapacity * sizeof(UINT),
                                     stableTriangleDataBuffer,
                                     stableTriangleDataUpload[0]))
            return false;

        // Destruction can replace geometry, draw metadata and authored triangle
        // keys every frame. Keep every upload source tied to the fenced frame
        // slot so CPU writes cannot race the preceding frame's queued copy.
        for (UINT frame = 1; frame < FRAME_COUNT; ++frame) {
            if (!CreateUpload(VB_MAX_DRAW_CALLS * sizeof(VBDrawCallData),
                              drawCallUpload[frame]) ||
                !CreateUpload(geometryVertexCapacity * sizeof(VBPackedVertex),
                              vertexDataUpload[frame]) ||
                !CreateUpload(geometryIndexCapacity * sizeof(UINT),
                              indexDataUpload[frame]) ||
                !CreateUpload(geometryTriangleCapacity * sizeof(UINT),
                              stableTriangleDataUpload[frame]))
                return false;
        }

        if (!CreateDefaultAndUpload(VB_CLUSTER_COUNT * sizeof(VBClusterData),
                                     clusterDataBuffer, clusterDataUpload[0]))
            return false;

        for (UINT frame = 1; frame < FRAME_COUNT; ++frame)
            if (!CreateUpload(VB_CLUSTER_COUNT * sizeof(VBClusterData),
                              clusterDataUpload[frame])) return false;

        // Hit-geometry bindings for the raytracing hit path. Upload heap only:
        // it is written on acceleration rebuilds, never per frame, so the extra
        // copy a default heap would need buys nothing.
        {
            D3D12_HEAP_PROPERTIES uploadHeap = {};
            uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC bufDesc = {};
            bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            // Matches DXRScene::HitGeometryData and the shader's HitGeometry.
            // Sized from the constant rather than the C++ type so this header
            // does not have to include DXRScene.h; UploadHitGeometry asserts
            // the two agree.
            bufDesc.Width =
                static_cast<UINT64>(VB_MAX_HIT_GEOMETRY) * kHitGeometryStride;
            bufDesc.Height = 1;
            bufDesc.DepthOrArraySize = 1;
            bufDesc.MipLevels = 1;
            bufDesc.Format = DXGI_FORMAT_UNKNOWN;
            bufDesc.SampleDesc.Count = 1;
            bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(g_dx12.device->CreateCommittedResource(
                    &uploadHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                    IID_PPV_ARGS(&hitGeometryBuffer))))
                return false;
            bufDesc.Width = static_cast<UINT64>(kMaxEmissiveTriangles) *
                            sizeof(EmissiveTriangleGPU);
            if (FAILED(g_dx12.device->CreateCommittedResource(
                    &uploadHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                    IID_PPV_ARGS(&emissiveTriangleBuffer))))
                return false;
            emissiveTriangleBuffer->SetName(L"Emissive triangle lights");
            UploadEmissiveTriangles({});
            D3D12_HEAP_PROPERTIES defaultHeap = {};
            defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
            bufDesc.Width = 16;
            bufDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            if (FAILED(g_dx12.device->CreateCommittedResource(
                    &defaultHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                    IID_PPV_ARGS(&emissiveReservoirPlaceholder))))
                return false;
            bufDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
        }

        D3D12_HEAP_PROPERTIES materialHeap = {};
        materialHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC materialDesc = {};
        materialDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        materialDesc.Width = VB_MAX_MATERIALS * sizeof(VBMaterialData);
        materialDesc.Height = 1;
        materialDesc.DepthOrArraySize = 1;
        materialDesc.MipLevels = 1;
        materialDesc.SampleDesc.Count = 1;
        materialDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        HRESULT hr = g_dx12.device->CreateCommittedResource(
            &materialHeap, D3D12_HEAP_FLAG_NONE, &materialDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&materialDataBuffer));
        if (FAILED(hr)) return false;
        D3D12_RANGE noRead = { 0, 0 };
        hr = materialDataBuffer->Map(0, &noRead,
            reinterpret_cast<void**>(&mappedMaterials));
        if (FAILED(hr)) return false;

        // Parallel bindless material table. Allocated unconditionally -- it is
        // 640 KB across two frame slots, and allocating it up front means
        // toggling bindless at runtime
        // never has to create a resource mid-frame.
        D3D12_RESOURCE_DESC bindlessMaterialDesc = materialDesc;
        bindlessMaterialDesc.Width = static_cast<UINT64>(FRAME_COUNT * RenderViewCountDX12) *
            VB_MAX_MATERIALS * sizeof(VBMaterialData);
        hr = g_dx12.device->CreateCommittedResource(
            &materialHeap, D3D12_HEAP_FLAG_NONE, &bindlessMaterialDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&bindlessMaterialDataBuffer));
        if (FAILED(hr)) return false;
        hr = bindlessMaterialDataBuffer->Map(0, &noRead,
            reinterpret_cast<void**>(&mappedBindlessMaterials));
        if (FAILED(hr)) return false;
        for (UINT frame = 0; frame < FRAME_COUNT * RenderViewCountDX12; ++frame)
            mappedBindlessMaterials[frame * VB_MAX_MATERIALS] =
                VBMaterialData{};

        return true;
    }

    bool CreateColorLUT() {
        constexpr UINT LUTSize = 16;
        D3D12_RESOURCE_DESC texture = {};
        texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        texture.Width = LUTSize;
        texture.Height = LUTSize;
        texture.DepthOrArraySize = LUTSize;
        texture.MipLevels = 1;
        texture.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        texture.SampleDesc.Count = 1;
        D3D12_HEAP_PROPERTIES defaultHeap = {};
        defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
        HRESULT hr = g_dx12.device->CreateCommittedResource(
            &defaultHeap, D3D12_HEAP_FLAG_NONE, &texture,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&colorLUT));
        if (FAILED(hr)) return false;

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
        UINT rows = 0;
        UINT64 rowSize = 0;
        UINT64 uploadSize = 0;
        g_dx12.device->GetCopyableFootprints(
            &texture, 0, 1, 0, &footprint, &rows, &rowSize, &uploadSize);
        D3D12_RESOURCE_DESC buffer = {};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = uploadSize;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES uploadHeap = {};
        uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
        hr = g_dx12.device->CreateCommittedResource(
            &uploadHeap, D3D12_HEAP_FLAG_NONE, &buffer,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&colorLUTUpload));
        if (FAILED(hr)) return false;

        BYTE* mapped = nullptr;
        D3D12_RANGE noRead = { 0, 0 };
        hr = colorLUTUpload->Map(0, &noRead, reinterpret_cast<void**>(&mapped));
        if (FAILED(hr)) return false;
        mapped += footprint.Offset;
        for (UINT z = 0; z < LUTSize; ++z) {
            for (UINT y = 0; y < LUTSize; ++y) {
                BYTE* row = mapped + (SIZE_T)z * footprint.Footprint.RowPitch * LUTSize
                    + (SIZE_T)y * footprint.Footprint.RowPitch;
                for (UINT x = 0; x < LUTSize; ++x) {
                    float r = x / float(LUTSize - 1);
                    float g = y / float(LUTSize - 1);
                    float b = z / float(LUTSize - 1);
                    row[x * 4 + 0] = (BYTE)roundf(r * 255.0f);
                    row[x * 4 + 1] = (BYTE)roundf(g * 255.0f);
                    row[x * 4 + 2] = (BYTE)roundf(b * 255.0f);
                    row[x * 4 + 3] = 255;
                }
            }
        }
        colorLUTUpload->Unmap(0, nullptr);

        D3D12_TEXTURE_COPY_LOCATION source = {};
        source.pResource = colorLUTUpload.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION destination = {};
        destination.pResource = colorLUT.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destination.SubresourceIndex = 0;
        g_dx12.commandList->CopyTextureRegion(
            &destination, 0, 0, 0, &source, nullptr);
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = colorLUT.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        g_dx12.commandList->ResourceBarrier(1, &barrier);
        return true;
    }


    // Authored optical grime, loaded from Content/Textures/Lens. Replaces an
    // earlier procedural mask whose radial sin() ridge term drew a visible
    // concentric bullseye on screen. The sparse captured mask has no repeating
    // structure for the eye to lock onto, which is the whole point.
    //
    // Single channel: the mask only ever modulates bloom, so colour would be
    // three quarters wasted bandwidth on a full-screen fetch.
    //
    // A missing or unreadable file is not fatal. The texture is created either
    // way and filled with black, which makes the dirt term contribute nothing
    // and leaves the rest of post working -- the alternative, failing Init,
    // would take the whole renderer down over a cosmetic asset.
    bool CreateLensTexture(const char* relativePath, const char* label,
                           ComPtr<ID3D12Resource>& outputTexture,
                           ComPtr<ID3D12Resource>& outputUpload) {
        int width = 0, height = 0, channels = 0;
        std::string path = relativePath;
        stbi_uc* pixels = stbi_load(path.c_str(), &width, &height, &channels, 1);
        if (!pixels) {
            // Try next to the executable, matching how shaders resolve: the
            // working directory and the exe directory are not always the same.
            // Convert through UTF-8 rather than narrowing each wchar_t: a
            // truncating copy mangles any install path with a non-ASCII
            // character, which turns into a silently missing texture.
            const std::wstring exeDir = ShaderCacheDX12::ExecutableDirectory();
            std::string narrow;
            const int narrowSize = WideCharToMultiByte(
                CP_UTF8, 0, exeDir.c_str(), (int)exeDir.size(),
                nullptr, 0, nullptr, nullptr);
            if (narrowSize > 0) {
                narrow.resize((size_t)narrowSize);
                WideCharToMultiByte(CP_UTF8, 0, exeDir.c_str(), (int)exeDir.size(),
                                    narrow.data(), narrowSize, nullptr, nullptr);
            }
            path = narrow + path;
            pixels = stbi_load(path.c_str(), &width, &height, &channels, 1);
        }
        const bool loaded = pixels != nullptr;
        if (!loaded) {
            width = 4;
            height = 4;
            std::cout << label << " texture missing; effect disabled\n";
        }

        D3D12_RESOURCE_DESC texture = {};
        texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texture.Width = (UINT64)width;
        texture.Height = (UINT)height;
        texture.DepthOrArraySize = 1;
        texture.MipLevels = 1;
        texture.Format = DXGI_FORMAT_R8_UNORM;
        texture.SampleDesc.Count = 1;
        D3D12_HEAP_PROPERTIES defaultHeap = {};
        defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
        HRESULT hr = g_dx12.device->CreateCommittedResource(
            &defaultHeap, D3D12_HEAP_FLAG_NONE, &texture,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&outputTexture));
        if (FAILED(hr)) {
            if (pixels) stbi_image_free(pixels);
            return false;
        }

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint = {};
        UINT rows = 0;
        UINT64 rowSize = 0;
        UINT64 uploadSize = 0;
        g_dx12.device->GetCopyableFootprints(
            &texture, 0, 1, 0, &footprint, &rows, &rowSize, &uploadSize);
        D3D12_RESOURCE_DESC buffer = {};
        buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer.Width = uploadSize;
        buffer.Height = 1;
        buffer.DepthOrArraySize = 1;
        buffer.MipLevels = 1;
        buffer.SampleDesc.Count = 1;
        buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES uploadHeap = {};
        uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;
        hr = g_dx12.device->CreateCommittedResource(
            &uploadHeap, D3D12_HEAP_FLAG_NONE, &buffer,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&outputUpload));
        if (FAILED(hr)) {
            if (pixels) stbi_image_free(pixels);
            return false;
        }

        BYTE* mapped = nullptr;
        D3D12_RANGE noRead = { 0, 0 };
        hr = outputUpload->Map(0, &noRead, reinterpret_cast<void**>(&mapped));
        if (FAILED(hr)) {
            if (pixels) stbi_image_free(pixels);
            return false;
        }
        mapped += footprint.Offset;
        // Row by row: the upload heap's pitch is alignment-padded and is not
        // the source's tightly packed width.
        for (int y = 0; y < height; ++y) {
            BYTE* row = mapped + (SIZE_T)y * footprint.Footprint.RowPitch;
            if (loaded) memcpy(row, pixels + (SIZE_T)y * width, (size_t)width);
            else memset(row, 0, (size_t)width);
        }
        outputUpload->Unmap(0, nullptr);
        if (pixels) stbi_image_free(pixels);

        D3D12_TEXTURE_COPY_LOCATION source = {};
        source.pResource = outputUpload.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint = footprint;
        D3D12_TEXTURE_COPY_LOCATION destination = {};
        destination.pResource = outputTexture.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destination.SubresourceIndex = 0;
        g_dx12.commandList->CopyTextureRegion(
            &destination, 0, 0, 0, &source, nullptr);
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = outputTexture.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter =
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        g_dx12.commandList->ResourceBarrier(1, &barrier);
        if (loaded)
            std::cout << label << " texture: " << width << "x" << height
                      << " from " << path << "\n";
        return true;
    }

    bool CreateComputeDescriptorHeap() {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        // [90] t91 terrain splatmap. Slot 90 is the first free index: t90 is a
        // root SRV, not a table entry, so it consumes no heap slot.
        heapDesc.NumDescriptors = kResolveDescriptorCount;
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        HRESULT hr = g_dx12.device->CreateDescriptorHeap(
            &heapDesc, IID_PPV_ARGS(&computeDescHeap));
        if (FAILED(hr)) {
            std::cerr << "Failed to create visibility compute descriptor heap\n";
            return false;
        }
        for (UINT frame = 0; frame < FRAME_COUNT; ++frame) {
            if (FAILED(g_dx12.device->CreateDescriptorHeap(
                    &heapDesc,
                    IID_PPV_ARGS(&bentNormalComputeDescHeaps[frame])))) {
                std::cerr << "Failed to create bent-normal resolve heap\n";
                return false;
            }
        }
        if (!legacyMaterialSnapshots.Create(FRAME_COUNT * RenderViewCountDX12 * VB_MAX_MATERIALS))
            return false;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        for (UINT frame = 0; frame < FRAME_COUNT; ++frame) {
            heapDesc.NumDescriptors = kResolveDescriptorCount;
            if (FAILED(g_dx12.device->CreateDescriptorHeap(&heapDesc,
                    IID_PPV_ARGS(&rasterViewHeaps[frame])))) return false;
            heapDesc.NumDescriptors = kEnhancedResolveDescriptorCount;
            if (FAILED(g_dx12.device->CreateDescriptorHeap(&heapDesc,
                    IID_PPV_ARGS(&resolveViewHeaps[frame])))) return false;
        }
        UpdateComputeDescriptors();
        return true;
    }

    bool CreateVisPassPipeline() {
        // Read and compile shaders
        std::ifstream vsFile("shaders/visbuf_vs.hlsl");
        std::ifstream psFile("shaders/visbuf_ps.hlsl");
        if (!vsFile.is_open() || !psFile.is_open()) {
            std::cerr << "Failed to open visibility buffer shader files" << std::endl;
            return false;
        }

        std::stringstream vsSS, psSS;
        vsSS << vsFile.rdbuf();
        psSS << psFile.rdbuf();
        std::string vsCode = vsSS.str();
        std::string psCode = psSS.str();

        UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
        compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
        compileFlags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

        ComPtr<ID3DBlob> vsBlob, psBlob, alphaPsBlob, errorBlob;

        HRESULT hr = ShaderCacheDX12::CompileCached(vsCode.c_str(), vsCode.length(),
            "shaders/visbuf_vs.hlsl",
            nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "vs_5_0",
            compileFlags, 0, &vsBlob, &errorBlob);
        if (FAILED(hr)) {
            if (errorBlob) std::cerr << "VB VS error: " << (char*)errorBlob->GetBufferPointer() << std::endl;
            return false;
        }

        errorBlob.Reset();
        hr = ShaderCacheDX12::CompileCached(psCode.c_str(), psCode.length(),
            "shaders/visbuf_ps.hlsl",
            nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "ps_5_0",
            compileFlags, 0, &psBlob, &errorBlob);
        if (FAILED(hr)) {
            if (errorBlob) std::cerr << "VB PS error: " << (char*)errorBlob->GetBufferPointer() << std::endl;
            return false;
        }

        errorBlob.Reset();
        hr = ShaderCacheDX12::CompileCached(psCode.c_str(), psCode.length(),
            "shaders/visbuf_ps.hlsl",
            nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "mainAlpha", "ps_5_0",
            compileFlags, 0, &alphaPsBlob, &errorBlob);
        if (FAILED(hr)) {
            if (errorBlob) std::cerr << "VB alpha PS error: "
                << (char*)errorBlob->GetBufferPointer() << std::endl;
            return false;
        }

        // Root signature for vis pass:
        // 0: CBV - MatrixBuffer (b0)
        // 1: Root constants - draw/material flags (b1), 4 UINT values
        // 2: Alpha-test base colour (t0)
        // 3: Persistent per-instance data (t1); VS reads model by drawCallID
        // 4: Impact decal cutout volumes (b10)
        D3D12_ROOT_PARAMETER visParams[5] = {};

        visParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        visParams[0].Descriptor.ShaderRegister = 0;
        visParams[0].Descriptor.RegisterSpace = 0;
        visParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

        visParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        visParams[1].Constants.ShaderRegister = 1;
        visParams[1].Constants.RegisterSpace = 0;
        visParams[1].Constants.Num32BitValues = 4;
        visParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_DESCRIPTOR_RANGE alphaRange = {};
        alphaRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        alphaRange.NumDescriptors = 1;
        alphaRange.BaseShaderRegister = 0;
        alphaRange.OffsetInDescriptorsFromTableStart = 0;
        visParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        visParams[2].DescriptorTable.NumDescriptorRanges = 1;
        visParams[2].DescriptorTable.pDescriptorRanges = &alphaRange;
        visParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        visParams[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        visParams[3].Descriptor.ShaderRegister = 1;
        visParams[3].Descriptor.RegisterSpace = 0;
        visParams[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

        visParams[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        visParams[4].Descriptor.ShaderRegister = 10;
        visParams[4].Descriptor.RegisterSpace = 0;
        visParams[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC alphaSampler = {};
        alphaSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        alphaSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        alphaSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        alphaSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        alphaSampler.MaxLOD = D3D12_FLOAT32_MAX;
        alphaSampler.ShaderRegister = 0;
        alphaSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC visRootSigDesc = {};
        // b10 is an inert compatibility slot for legacy and is read only by
        // the bindless cutout shader. Both paths keep the same parameter map.
        visRootSigDesc.NumParameters = 5;
        visRootSigDesc.pParameters = visParams;
        visRootSigDesc.NumStaticSamplers = 1;
        visRootSigDesc.pStaticSamplers = &alphaSampler;
        visRootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> sigBlob;
        errorBlob.Reset();
        hr = D3D12SerializeRootSignature(&visRootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errorBlob);
        if (FAILED(hr)) {
            if (errorBlob) std::cerr << "VB root sig error: " << (char*)errorBlob->GetBufferPointer() << std::endl;
            return false;
        }

        hr = g_dx12.device->CreateRootSignature(0, sigBlob->GetBufferPointer(),
            sigBlob->GetBufferSize(), IID_PPV_ARGS(&visPassRootSig));
        if (FAILED(hr)) return false;

        // Input layout - same as main shader
        D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
        };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.InputLayout = { inputLayout, _countof(inputLayout) };
        psoDesc.pRootSignature = visPassRootSig.Get();
        psoDesc.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
        psoDesc.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };

        psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
        // Imported glTF/FBX geometry is CCW-outward (glTF spec), matching the
        // mesh-shader path. Culling with the DX12 default (CW front) would
        // discard front faces and keep back faces, so solid props render
        // inside-out. Inherited by the bindless vis PSOs created below.
        psoDesc.RasterizerState.FrontCounterClockwise = TRUE;
        psoDesc.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
        psoDesc.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
        psoDesc.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
        psoDesc.RasterizerState.DepthClipEnable = TRUE;

        psoDesc.BlendState.AlphaToCoverageEnable = FALSE;
        psoDesc.BlendState.IndependentBlendEnable = FALSE;
        psoDesc.BlendState.RenderTarget[0].BlendEnable = FALSE;
        psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        psoDesc.DepthStencilState.DepthEnable = TRUE;
        psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        psoDesc.DepthStencilState.StencilEnable = FALSE;

        psoDesc.SampleMask = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = DXGI_FORMAT_R32G32_UINT;
        psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        psoDesc.SampleDesc.Count = 1;

        hr = g_dx12.device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&visPassPSO));
        if (FAILED(hr)) {
            std::cerr << "Failed to create vis pass PSO, HRESULT: 0x" << std::hex << hr << std::dec << std::endl;
            return false;
        }
        psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        hr = g_dx12.device->CreateGraphicsPipelineState(
            &psoDesc, IID_PPV_ARGS(&visPassDoubleSidedPSO));
        if (FAILED(hr)) return false;

        psoDesc.PS = { alphaPsBlob->GetBufferPointer(), alphaPsBlob->GetBufferSize() };
        psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
        hr = g_dx12.device->CreateGraphicsPipelineState(
            &psoDesc, IID_PPV_ARGS(&visPassAlphaPSO));
        if (FAILED(hr)) return false;
        psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        hr = g_dx12.device->CreateGraphicsPipelineState(
            &psoDesc, IID_PPV_ARGS(&visPassAlphaDoubleSidedPSO));
        if (FAILED(hr)) return false;

        bindlessVisPassReady = false;
        if (bindlessHeap && bindlessHeap->Supported() &&
            ShaderCacheDX12::DxcAvailable()) {
            const std::wstring shaderDirectory =
                ShaderCacheDX12::ExecutableDirectory() + L"shaders";
            const std::string bindlessVS =
                "#define SGE_BINDLESS_MATERIALS 1\n" + vsCode;
            const std::string bindlessPS =
                "#define SGE_BINDLESS_MATERIALS 1\n" + psCode;
            ComPtr<ID3DBlob> bindlessVSBlob, bindlessPSBlob,
                bindlessAlphaPSBlob;
            std::string errors;
            const bool shadersReady =
                ShaderCacheDX12::CompileCachedDXC(
                    bindlessVS, L"visbuf_vs.hlsl", L"main", L"vs_6_6",
                    shaderDirectory, &bindlessVSBlob, &errors) &&
                ShaderCacheDX12::CompileCachedDXC(
                    bindlessPS, L"visbuf_ps.hlsl", L"main", L"ps_6_6",
                    shaderDirectory, &bindlessPSBlob, &errors) &&
                ShaderCacheDX12::CompileCachedDXC(
                    bindlessPS, L"visbuf_ps.hlsl", L"mainAlpha", L"ps_6_6",
                    shaderDirectory, &bindlessAlphaPSBlob, &errors);
            if (!shadersReady) {
                std::cerr << "Bindless visibility shader compile failed\n"
                          << errors << std::endl;
            } else {
                D3D12_ROOT_SIGNATURE_DESC bindlessRootDesc = visRootSigDesc;
                bindlessRootDesc.Flags =
                    D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
                    D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;
                ComPtr<ID3DBlob> bindlessSig, bindlessSigError;
                if (SUCCEEDED(D3D12SerializeRootSignature(
                        &bindlessRootDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                        &bindlessSig, &bindlessSigError)) &&
                    SUCCEEDED(g_dx12.device->CreateRootSignature(
                        0, bindlessSig->GetBufferPointer(),
                        bindlessSig->GetBufferSize(),
                        IID_PPV_ARGS(&bindlessVisPassRootSig)))) {
                    psoDesc.pRootSignature = bindlessVisPassRootSig.Get();
                    psoDesc.VS = { bindlessVSBlob->GetBufferPointer(),
                                   bindlessVSBlob->GetBufferSize() };
                    psoDesc.PS = { bindlessPSBlob->GetBufferPointer(),
                                   bindlessPSBlob->GetBufferSize() };
                    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
                    bool ok = SUCCEEDED(g_dx12.device->CreateGraphicsPipelineState(
                        &psoDesc, IID_PPV_ARGS(&bindlessVisPassPSO)));
                    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
                    ok = ok && SUCCEEDED(g_dx12.device->CreateGraphicsPipelineState(
                        &psoDesc, IID_PPV_ARGS(&bindlessVisPassDoubleSidedPSO)));
                    psoDesc.PS = { bindlessAlphaPSBlob->GetBufferPointer(),
                                   bindlessAlphaPSBlob->GetBufferSize() };
                    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
                    ok = ok && SUCCEEDED(g_dx12.device->CreateGraphicsPipelineState(
                        &psoDesc, IID_PPV_ARGS(&bindlessVisPassAlphaPSO)));
                    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
                    ok = ok && SUCCEEDED(g_dx12.device->CreateGraphicsPipelineState(
                        &psoDesc, IID_PPV_ARGS(&bindlessVisPassAlphaDoubleSidedPSO)));
                    bindlessVisPassReady = ok;
                } else if (bindlessSigError) {
                    std::cerr << "Bindless visibility root signature failed: "
                        << (const char*)bindlessSigError->GetBufferPointer()
                        << std::endl;
                }
            }
        }

        std::cout << "Visibility pass pipeline created" << std::endl;
        return true;
    }

    // Uploads the per-frame enhanced constants (b5).
    void UpdateEnhancedConstants(UINT frameSlot) {
        if (!enhancedConstantMapped) return;
        // Field-for-field mirror of EnhancedVisualsBuffer (b5) in
        // visbuf_resolve_cs.hlsl. Append only -- inserting shifts every field
        // after it and silently corrupts unrelated state.
        struct EnhancedConstants {
            UINT  rtShadows;
            UINT  rayClassify;
            float shadowRayLength;
            float confidenceThreshold;
            UINT  rtReflections;
            float reflectionRayLength;
            float reflectionRoughnessCut;
            UINT  frameIndex;
            float reflectionOcclusion;
            UINT  reflectionClassify;
            float reflectionConfidenceCut;
            UINT  probeMissGI;
            UINT  svgfTemporalEnable;
            UINT  svgfMaxAccum;
            UINT  svgfAtrousEnable;
            UINT  svgfAtrousIters;
            UINT  svgfHistoryValid;
            float probeMissGIStrength;
            // Entries in the hit-geometry table. Zero disables real hit
            // shading, so a scene whose acceleration structure has not been
            // rebuilt since this feature landed keeps the sky approximation.
            UINT  hitGeometryCount;
            UINT  rrGuideEnable;
            UINT  lumenGI;
            float rrMotionJitterU;
            float rrMotionJitterV;
            UINT  raySanitize;
            UINT  lumenGIHalfResolution;
            float mipGradScale;
            float detailMipGradScale;
            UINT  lumenHitShadeMode;
            UINT  giRadianceCache;
            UINT  lumenReSTIR;
            UINT  lumenReSTIRDebug;
            UINT  lumenReSTIRSpatialTaps;
            UINT  emissiveCGNS;
            UINT  emissiveCGNSPadding[3];
        } constants;
        static_assert(sizeof(EnhancedConstants) == 144,
                      "EnhancedVisualsBuffer C++ mirror is out of sync");
        constants.rtShadows = enhancedRTShadowsActive ? 1u : 0u;
        constants.rayClassify = enhancedRayClassifyActive ? 1u : 0u;
        constants.shadowRayLength = enhancedShadowRayLength;
        constants.confidenceThreshold = enhancedConfidenceThreshold;
        constants.rtReflections = enhancedRTReflectionsActive ? 1u : 0u;
        constants.reflectionRayLength = enhancedReflectionRayLength;
        // Ultra: every roughness gets a reflection ray; RR resolves the noise.
        constants.reflectionRoughnessCut =
            (rayReconstructionActive || lumenGIActive)
                ? settingsReflectionRoughnessCut
                : enhancedReflectionRoughnessCut;
        // Rotates the sampling sequence so consecutive frames draw different
        // samples; this is the variance a temporal denoiser resolves.
        constants.frameIndex = (ScopeSurfaceBound() ? enhancedReflectionFrameCounter : enhancedReflectionFrameCounter++);
        constants.reflectionOcclusion = enhancedReflectionOcclusion;
        // Off under RR, like shadow classification: jitter moves pixels across
        // the confidence threshold, flipping them between probe and ray shading
        // from frame to frame, and RR is the denoiser for the full signal.
        // Capture A/B: SGE_RR_REFLECTION_CLASSIFY keeps classification on
        // under RR.
        static const bool kRRKeepReflectionClassify =
            GetEnvironmentVariableA("SGE_RR_REFLECTION_CLASSIFY", nullptr,
                                    0) > 0;
        constants.reflectionClassify = (enhancedReflectionClassifyActive &&
            (!rayReconstructionActive || kRRKeepReflectionClassify)) ? 1u : 0u;
        constants.reflectionConfidenceCut = enhancedReflectionConfidenceCut;
        constants.probeMissGI = enhancedProbeMissGIActive ? 1u : 0u;
        constants.svgfTemporalEnable = (svgfTemporalEnabled &&
            !rayReconstructionActive && !ScopeSurfaceBound()) ? 1u : 0u;
        constants.svgfMaxAccum = svgfMaxAccumFrames;
        constants.svgfAtrousEnable = (svgfAtrousEnabled &&
            !rayReconstructionActive && !ScopeSurfaceBound()) ? 1u : 0u;
        constants.svgfAtrousIters = SVGFAtrousIterationCount();
        constants.svgfHistoryValid = svgfHistoryValid ? 1u : 0u;
        constants.probeMissGIStrength = enhancedProbeMissGIStrength;
        constants.hitGeometryCount = hitGeometryCount;
        // Guides are main-view sized; the scope resolve must not write them.
        constants.rrGuideEnable = (rayReconstructionActive &&
            !ScopeSurfaceBound() &&
            rrDiffuseAlbedo && rrSpecularAlbedo &&
            rrSpecularHitDistance) ? 1u : 0u;
        constants.lumenGI = lumenGIActive ? 1u : 0u;
        constants.lumenGIHalfResolution =
            (lumenGIHalfResolutionActive && !ScopeSurfaceBound()) ? 1u : 0u;
        // Stage-0 Lumen cost split (A/B only): SGE_LUMEN_HIT_SHADE=1 drops the
        // GI hit's sun shadow ray, =2 drops GI hit shading entirely.
        static const UINT kLumenHitShadeMode = [] {
            char text[8] = {};
            return GetEnvironmentVariableA("SGE_LUMEN_HIT_SHADE", text,
                                           sizeof(text)) > 0
                ? static_cast<UINT>(std::clamp(atoi(text), 0, 2)) : 0u;
        }();
        constants.lumenHitShadeMode = kLumenHitShadeMode;
        constants.giRadianceCache = giRadianceCacheMode;
        constants.lumenReSTIR = lumenReSTIRMode;
        static const UINT kReSTIRDebug = [] {
            char text[8] = {};
            return GetEnvironmentVariableA("SGE_RESTIR_DEBUG", text,
                                           sizeof(text)) > 0
                ? static_cast<UINT>(atoi(text)) : 0u;
        }();
        constants.lumenReSTIRDebug = kReSTIRDebug;
        static const UINT kReSTIRTaps = [] {
            char text[8] = {};
            return GetEnvironmentVariableA("SGE_RESTIR_TAPS", text,
                                           sizeof(text)) > 0
                ? static_cast<UINT>(std::clamp(atoi(text), 0, 8)) : 1u;
        }();
        constants.lumenReSTIRSpatialTaps = kReSTIRTaps;
        constants.emissiveCGNS = ScopeSurfaceBound() ? 0u : emissiveCGNSMode;
        std::fill(std::begin(constants.emissiveCGNSPadding),
                  std::end(constants.emissiveCGNSPadding), 0u);
        // DLSS mip bias (DLSS Programming Guide 3.5): while DLSS / RR is the
        // temporal resolve, textures are sampled at display detail with
        // bias = log2(render / display) - 1 (-1 DLAA, -2 at 50%).
        // SGE_MIP_BIAS=<bias> forces a value, SGE_MIP_BIAS=off disables it.
        {
            float bias = (dlssActive && displayWidth > 0 && width > 0)
                ? std::log2(static_cast<float>(width) /
                            static_cast<float>(displayWidth)) - 1.0f
                : 0.0f;
            char text[16] = {};
            if (GetEnvironmentVariableA("SGE_MIP_BIAS", text, sizeof(text)) > 0)
                bias = text[0] == 'o' ? 0.0f : static_cast<float>(atof(text));
            constants.mipGradScale = std::exp2(bias);
            mipBiasApplied = bias;
            // Normal / roughness maps stay unbiased unless forced:
            // SGE_MIP_BIAS_DETAIL=<bias> (A/B).
            float detailBias = 0.0f;
            if (GetEnvironmentVariableA("SGE_MIP_BIAS_DETAIL", text,
                                        sizeof(text)) > 0)
                detailBias = static_cast<float>(atof(text));
            constants.detailMipGradScale = std::exp2(detailBias);
        }
        // Under Ray Reconstruction the resolve writes unjittered vectors
        // (pixels -> UV; the projection's NDC y flip and the UV y flip cancel,
        // so both are +). Native RR: the previous VP was stored unjittered, so
        // only this frame's jitter is inside the vector.
        const bool rrMotion = rayReconstructionActive &&
                              !rayReconstructionUpscaleActive &&
                              !ScopeSurfaceBound() && width > 0 && height > 0;
        constants.rrMotionJitterU = rrMotion
            ? rrMotionJitterPixels.x / static_cast<float>(width) : 0.0f;
        constants.rrMotionJitterV = rrMotion
            ? rrMotionJitterPixels.y / static_cast<float>(height) : 0.0f;
        // Upscaling RR: the previous VP keeps its jitter (restored for the
        // HZB), so the vector holds current - previous jitter. Flagged
        // "jittered", DLSS subtracts only the current offset (DLSS PG 3.6.2),
        // leaving -previous: up to half a pixel of random misregistration a
        // frame, which RR accumulated as blur (door sharpness 42 -> 68 at
        // 100%, still-frame delta 1.31 -> 0.11 once removed). Strip both and
        // flag unjittered. SGE_RR_MV_MODE=0 restores the old vectors.
        if (rayReconstructionUpscaleActive && rrUpscaleMotionMode != 0 &&
            !ScopeSurfaceBound() && width > 0 && height > 0) {
            constants.rrMotionJitterU =
                (rrMotionJitterPixels.x - rrPreviousJitterPixels.x) /
                static_cast<float>(width);
            constants.rrMotionJitterV =
                (rrMotionJitterPixels.y - rrPreviousJitterPixels.y) /
                static_cast<float>(height);
        }
        lastResolveStripUV = { constants.rrMotionJitterU,
                               constants.rrMotionJitterV };
        // Firefly/NaN guard on traced samples; SGE_NO_RAY_SANITIZE is the
        // capture A/B switch.
        static const bool kNoRaySanitize =
            GetEnvironmentVariableA("SGE_NO_RAY_SANITIZE", nullptr, 0) > 0;
        constants.raySanitize = kNoRaySanitize ? 0u : 1u;
        const UINT64 constantOffset =
            static_cast<UINT64>(ViewFrameIndex()) * 256ull;
        memcpy(static_cast<BYTE*>(enhancedConstantMapped) + constantOffset,
               &constants, sizeof(constants));
    }

    // Builds the SM 6.5 resolve variant. Mirrors the root signature of the
    // default resolve and extends it with the TLAS SRV (t79), the ray-mask UAV
    // (u3) and the enhanced constants (b5).
    //
    // Every failure path leaves enhancedPipelineReady false and returns without
    // disturbing the default PSO, so an old driver or a missing dxcompiler.dll
    // costs nothing but the feature.
    // Bindless twin of the default (non-enhanced) resolve. Same descriptor
    // ranges and same two root parameters as the FXC PSO -- only the compiler
    // (DXC at cs_6_6), the define, and the directly-indexed-heap root flag
    // differ. Failure leaves bindlessResolveReady false, which keeps the
    // bindless toggle unavailable rather than breaking the frame.
    // ---- Resolve permutation sources and terrain sets ----
    // See the notes above ResolveTier for why terrain sets are split out.

    // visbuf_resolve_cs.hlsl as read once per process, optionally prefixed
    // with SGE_TERRAIN_PACKED_REUSE; the default FXC permutation.
    std::string resolveBaseSource;
    // The base with SGE_VIRTUAL_SHADOWS: every other permutation builds on it.
    std::string resolveSourceVSM;
    bool resolveSourcesPrepared = false;
    bool resolvePipelineDeferred = false;
    bool predictedResolveEnhanced = false;
    bool predictedResolveBindless = false;

    // One tier's terrain set: [0] generic half, [1] terrain-only half,
    // [2]/[3] their tile-classified twins.
    struct TerrainSetBuild {
        ComPtr<ID3D12PipelineState> pso[4];
        std::string errors;
    };
    std::future<TerrainSetBuild> terrainSetPending[ResolveTierCount];
    bool terrainSetRequested[ResolveTierCount] = {};
    // Tier whose terrain set Resolve found missing, and for how many
    // consecutive resolves.
    int missingTerrainTier = -1;
    unsigned missingTerrainTierFrames = 0;

    static UINT ResolveFxcFlags() {
        UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
        flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
        flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
        return flags;
    }

    static const wchar_t* ResolveTierProfile(int tier) {
        // ResourceDescriptorHeap needs 6.6; inline RayQuery needs 6.5.
        return (tier == ResolveTierBindless ||
                tier == ResolveTierBindlessEnhanced) ? L"cs_6_6" : L"cs_6_5";
    }

    bool PrepareResolveSources() {
        if (resolveSourcesPrepared) return true;
        std::ifstream csFile("shaders/visbuf_resolve_cs.hlsl");
        if (!csFile.is_open()) return false;
        std::stringstream csSS;
        csSS << csFile.rdbuf();
        resolveBaseSource = csSS.str();
        if (GetEnvironmentVariableA("SGE_TERRAIN_PACKED_REUSE", nullptr, 0) > 0)
            resolveBaseSource =
                "#define SGE_TERRAIN_PACKED_REUSE 1\n" + resolveBaseSource;
        resolveSourceVSM = "#define SGE_VIRTUAL_SHADOWS 1\n" + resolveBaseSource;
        D3D12_FEATURE_DATA_D3D12_OPTIONS1 waveOptions = {};
        lumenGIHalfResolutionSupported = SUCCEEDED(
            g_dx12.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1,
                &waveOptions, sizeof(waveOptions))) && waveOptions.WaveOps;
        resolveSourcesPrepared = true;
        return true;
    }

    // Source of one tier's base permutation. The tier builders and the boot
    // compile queue both take it from here, so they hash to the same blobs.
    void BindEnhancedRootUAVs(ID3D12GraphicsCommandList* cmd) {
        if (giRadianceCache.Address() != 0)
            cmd->SetComputeRootUnorderedAccessView(
                kEnhancedRadianceCacheRootParameter, giRadianceCache.Address());
        if (restirSampleBuffer && restirWeightBuffer) {
            cmd->SetComputeRootUnorderedAccessView(
                kEnhancedReSTIRRootParameter,
                restirSampleBuffer->GetGPUVirtualAddress());
            cmd->SetComputeRootUnorderedAccessView(
                kEnhancedReSTIRRootParameter + 1,
                restirWeightBuffer->GetGPUVirtualAddress());
        }
        if (EmissiveTriangleAddress() != 0)
            cmd->SetComputeRootShaderResourceView(
                kEnhancedEmissiveRootParameter, EmissiveTriangleAddress());
        if (EmissiveReservoirAddress() != 0)
            cmd->SetComputeRootUnorderedAccessView(
                kEnhancedEmissiveRootParameter + 1, EmissiveReservoirAddress());
    }

    // Light list as bound this frame: the real one only while reservoirs exist
    // for it to write, else the reserved empty last entry (count 0).
    D3D12_GPU_VIRTUAL_ADDRESS EmissiveTriangleAddress() const {
        if (!emissiveTriangleBuffer) return 0;
        return emissiveTriangleBuffer->GetGPUVirtualAddress() +
            (emissiveReservoirBuffer ? 0ull
                : static_cast<UINT64>(kMaxEmissiveTriangles - 1u) *
                  sizeof(EmissiveTriangleGPU));
    }
    D3D12_GPU_VIRTUAL_ADDRESS EmissiveReservoirAddress() const {
        ID3D12Resource* buffer = emissiveReservoirBuffer
            ? emissiveReservoirBuffer.Get() : emissiveReservoirPlaceholder.Get();
        return buffer ? buffer->GetGPUVirtualAddress() : 0;
    }

    // Two frames of uint4 per pixel. Same lifetime rules as the ReSTIR GI
    // buffers: created here, released only by Resize() after its GPU wait.
    bool EnsureEmissiveReservoirs(UINT w, UINT h) {
        if (emissiveReservoirBuffer)
            return emissiveReservoirWidth == w && emissiveReservoirHeight == h;
        if (emissiveReservoirAllocationFailed || w == 0 || h == 0 ||
            emissiveTriangleCount == 0)
            return false;
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = static_cast<UINT64>(w) * h * 2ull * 16ull;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(g_dx12.device->CreateCommittedResource(&heap,
                D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                IID_PPV_ARGS(&emissiveReservoirBuffer)))) {
            emissiveReservoirAllocationFailed = true;
            return false;
        }
        emissiveReservoirBuffer->SetName(L"Emissive ReSTIR DI reservoirs");
        emissiveReservoirWidth = w;
        emissiveReservoirHeight = h;
        return true;
    }

    // Lazily sized to the render resolution; Resize() releases them after its
    // GPU wait, so they are only ever created here, never replaced in flight.
    bool EnsureReSTIRBuffers(UINT w, UINT h) {
        if (restirSampleBuffer)
            return restirWidth == w && restirHeight == h;
        if (restirAllocationFailed || w == 0 || h == 0) return false;
        const UINT64 pixels = static_cast<UINT64>(w) * h * 2ull;
        auto create = [&](ComPtr<ID3D12Resource>& out, UINT64 bytes,
                          const wchar_t* label) {
            D3D12_HEAP_PROPERTIES heap = {};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC desc = {};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width = bytes;
            desc.Height = 1;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            if (FAILED(g_dx12.device->CreateCommittedResource(&heap,
                    D3D12_HEAP_FLAG_NONE, &desc,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                    IID_PPV_ARGS(&out))))
                return false;
            out->SetName(label);
            return true;
        };
        if (!create(restirSampleBuffer, pixels * 16ull, L"Lumen ReSTIR samples") ||
            !create(restirWeightBuffer, pixels * 8ull, L"Lumen ReSTIR weights")) {
            restirSampleBuffer.Reset();
            restirWeightBuffer.Reset();
            restirAllocationFailed = true;
            return false;
        }
        restirWidth = w;
        restirHeight = h;
        restirHistoryValid = false;
        return true;
    }

    std::string ResolveTierSource(int tier) const {
        if (tier == ResolveTierFXC) return resolveSourceVSM;
        if (tier == ResolveTierBindless)
            return "#define SGE_BINDLESS_MATERIALS 1\n" + resolveSourceVSM;
        std::string source =
            std::string("#define SGE_LUMEN_HALF_RES_SUPPORTED ") +
            (lumenGIHalfResolutionSupported ? "1\n" : "0\n") +
            "#define SGE_ENHANCED_VISUALS 1\n" + resolveSourceVSM;
        if (tier == ResolveTierBindlessEnhanced)
            source = "#define SGE_BINDLESS_MATERIALS 1\n" + source;
        return source;
    }

    static void TerrainSetSources(const std::string& base, std::string out[4]) {
        out[0] = "#define SGE_TERRAIN_VISIBILITY 1\n" + base;
        out[1] = "#define SGE_TERRAIN_ONLY_RESOLVE 1\n" + out[0];
        out[2] = "#define SGE_RESOLVE_TILE_LIST 1\n" + out[0];
        out[3] = "#define SGE_RESOLVE_TILE_LIST 1\n" + out[1];
    }

    // Thread-safe: touches no members.
    static bool CompileResolveVariant(int tier, const std::string& source,
                                      ComPtr<ID3DBlob>& blob,
                                      std::string& errors) {
        if (tier == ResolveTierFXC) {
            ComPtr<ID3DBlob> errorBlob;
            const HRESULT hr = ShaderCacheDX12::CompileCached(
                source.c_str(), source.size(), "shaders/visbuf_resolve_cs.hlsl",
                nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "cs_5_1",
                ResolveFxcFlags(), 0, &blob, &errorBlob);
            if (FAILED(hr)) {
                if (errorBlob)
                    errors.assign(static_cast<const char*>(
                                      errorBlob->GetBufferPointer()),
                                  errorBlob->GetBufferSize());
                return false;
            }
            return true;
        }
        return ShaderCacheDX12::CompileCachedDXC(
            source, L"visbuf_resolve_cs.hlsl", L"main", ResolveTierProfile(tier),
            ShaderCacheDX12::ExecutableDirectory() + L"shaders", &blob, &errors);
    }

    ID3D12RootSignature* ResolveTierRootSig(int tier) const {
        switch (tier) {
        case ResolveTierFXC:              return resolveRootSig.Get();
        case ResolveTierEnhanced:         return enhancedResolveRootSig.Get();
        case ResolveTierBindless:         return bindlessResolveRootSig.Get();
        case ResolveTierBindlessEnhanced: return bindlessEnhancedResolveRootSig.Get();
        default:                          return nullptr;
        }
    }

    void TerrainSetSlots(int tier, ComPtr<ID3D12PipelineState>* slots[4]) {
        switch (tier) {
        case ResolveTierEnhanced:
            slots[0] = &enhancedTerrainResolvePSO;
            slots[1] = &enhancedTerrainOnlyResolvePSO;
            slots[2] = &enhancedTerrainResolveTiledPSO;
            slots[3] = &enhancedTerrainOnlyResolveTiledPSO;
            break;
        case ResolveTierBindless:
            slots[0] = &bindlessTerrainResolvePSO;
            slots[1] = &bindlessTerrainOnlyResolvePSO;
            slots[2] = &bindlessTerrainResolveTiledPSO;
            slots[3] = &bindlessTerrainOnlyResolveTiledPSO;
            break;
        case ResolveTierBindlessEnhanced:
            slots[0] = &bindlessEnhancedTerrainResolvePSO;
            slots[1] = &bindlessEnhancedTerrainOnlyResolvePSO;
            slots[2] = &bindlessEnhancedTerrainResolveTiledPSO;
            slots[3] = &bindlessEnhancedTerrainOnlyResolveTiledPSO;
            break;
        default:
            slots[0] = &terrainResolvePSO;
            slots[1] = &terrainOnlyResolvePSO;
            slots[2] = &terrainResolveTiledPSO;
            slots[3] = &terrainOnlyResolveTiledPSO;
            break;
        }
    }

    // The predicted tier, downgraded to what actually built: the same
    // readiness checks SelectedResolveTier applies at run time.
    int BootTerrainTier() const {
        const bool enhanced = predictedResolveEnhanced && enhancedPipelineReady &&
                              enhancedResolvePSO;
        const bool bindless = predictedResolveBindless && BindlessResolveReady() &&
                              (!enhanced || BindlessEnhancedResolveReady());
        return ResolveTierIndex(bindless, enhanced);
    }

    // Compiles and builds one tier's terrain set, all four permutations
    // concurrently. Static and fed by value, so a background build owns
    // everything it reads. `background` drops the threads below normal
    // priority and keeps their requests out of the boot-critical prewarm.
    static TerrainSetBuild BuildTerrainSet(int tier, const std::string& base,
                                           ComPtr<ID3D12RootSignature> rootSig,
                                           ComPtr<ID3D12Device> device,
                                           bool background) {
        TerrainSetBuild result;
        if (!rootSig || !device) {
            result.errors = "tier has no root signature";
            return result;
        }
        std::string sources[4];
        TerrainSetSources(base, sources);
        std::string errors[4];
        std::thread threads[4];
        for (int i = 0; i < 4; ++i) {
            threads[i] = std::thread([&, i] {
                if (background) {
                    SetThreadPriority(GetCurrentThread(),
                                      THREAD_PRIORITY_BELOW_NORMAL);
                    ShaderCacheDX12::MarkBackgroundThread();
                }
                ComPtr<ID3DBlob> blob;
                if (!CompileResolveVariant(tier, sources[i], blob, errors[i])) {
                    if (errors[i].empty()) errors[i] = "compile failed";
                    return;
                }
                D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
                desc.pRootSignature = rootSig.Get();
                desc.CS = { blob->GetBufferPointer(), blob->GetBufferSize() };
                if (FAILED(device->CreateComputePipelineState(
                        &desc, IID_PPV_ARGS(&result.pso[i])))) {
                    result.pso[i].Reset();
                    errors[i] = "PSO creation failed";
                }
            });
        }
        for (std::thread& thread : threads) thread.join();
        // Both halves or neither: the generic half skips terrain IDs, so alone
        // it would leave terrain unshaded. The tiled twins are only an
        // optimisation of the pair and also come as a pair.
        if (!result.pso[0] || !result.pso[1]) {
            for (auto& pso : result.pso) pso.Reset();
        } else if (!result.pso[2] || !result.pso[3]) {
            result.pso[2].Reset();
            result.pso[3].Reset();
        }
        static const char* names[4] = {
            "terrain", "terrain-only", "tiled terrain", "tiled terrain-only" };
        for (int i = 0; i < 4; ++i)
            if (!errors[i].empty())
                result.errors += std::string(ResolveTierName(tier)) + " " +
                                 names[i] + " resolve: " + errors[i] + "\n";
        return result;
    }

    void InstallTerrainSet(int tier, TerrainSetBuild& build) {
        ComPtr<ID3D12PipelineState>* slots[4] = {};
        TerrainSetSlots(tier, slots);
        for (int i = 0; i < 4; ++i) *slots[i] = build.pso[i];
        if (build.errors.empty()) return;
        std::cerr << build.errors << "(non-fatal; terrain stays forward or "
                                     "full-screen on this tier)\n";
        std::ofstream log("terrain_resolve_shader_error.log", std::ios::trunc);
        log << build.errors;
    }

    void CreateBindlessResolvePipeline(const std::string& csCode,
                                       const D3D12_DESCRIPTOR_RANGE* ranges,
                                       const D3D12_STATIC_SAMPLER_DESC* samplers,
                                       const D3D12_ROOT_PARAMETER* params) {
        bindlessResolveReady = false;
        if (!ShaderCacheDX12::DxcAvailable()) {
            std::cout << "Bindless materials: dxcompiler.dll unavailable\n";
            return;
        }

        (void)csCode;
        const std::string source = ResolveTierSource(ResolveTierBindless);
        ComPtr<ID3DBlob> csBlob;
        std::string errors;
        if (!CompileResolveVariant(ResolveTierBindless, source, csBlob, errors)) {
            std::cerr << "Bindless materials: resolve compile failed\n";
            if (!errors.empty()) {
                std::cerr << errors << std::endl;
                std::ofstream log("bindless_resolve_shader_error.log",
                                  std::ios::trunc);
                log << errors;
            }
            return;
        }

        D3D12_ROOT_SIGNATURE_DESC rootSigDesc = {};
        rootSigDesc.NumParameters = 3;
        rootSigDesc.pParameters = params;
        rootSigDesc.NumStaticSamplers = 3;  // s0 wrap, s1 shadow cmp, s2 clamp (terrain splat)
        rootSigDesc.pStaticSamplers = samplers;
        rootSigDesc.Flags =
            D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;

        ComPtr<ID3DBlob> sigBlob, sigError;
        if (FAILED(D3D12SerializeRootSignature(&rootSigDesc,
                D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &sigError))) {
            if (sigError)
                std::cerr << "Bindless resolve root sig: "
                          << (const char*)sigError->GetBufferPointer() << "\n";
            return;
        }
        if (FAILED(g_dx12.device->CreateRootSignature(
                0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(),
                IID_PPV_ARGS(&bindlessResolveRootSig))))
            return;

        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.pRootSignature = bindlessResolveRootSig.Get();
        psoDesc.CS = { csBlob->GetBufferPointer(), csBlob->GetBufferSize() };
        if (FAILED(g_dx12.device->CreateComputePipelineState(
                &psoDesc, IID_PPV_ARGS(&bindlessResolvePSO)))) {
            std::cerr << "Bindless materials: resolve PSO creation failed\n";
            bindlessResolveRootSig.Reset();
            return;
        }
        bindlessResolveReady = true;
        // This tier's terrain set is built by BuildTerrainSet, at boot or on
        // first use -- see the resolve permutation notes above it.
    }

    // `bindless` selects the SGE_BINDLESS_MATERIALS variant, which compiles at
    // cs_6_6 (ResourceDescriptorHeap needs 6.6) and adds
    // CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED to the root signature. Everything else
    // -- descriptor layout, root parameter numbering, samplers -- is identical,
    // so the two variants stay in lockstep by construction instead of by two
    // copies that drift.
    void CreateEnhancedResolvePipeline(const std::string& csCode,
                                       const D3D12_DESCRIPTOR_RANGE* baseRanges,
                                       const D3D12_STATIC_SAMPLER_DESC* samplers,
                                       bool bindless = false) {
        const char* tierName = bindless ? "Bindless enhanced visuals"
                                        : "Enhanced visuals";
        if (bindless) bindlessEnhancedResolveReady = false;
        else enhancedPipelineReady = false;
        if (!ShaderCacheDX12::DxcAvailable()) {
            std::cout << tierName << ": dxcompiler.dll unavailable\n";
            return;
        }

        // Defines are prepended rather than passed as -D so the cache key (which
        // hashes the source text) separates the variants automatically.
        // ResolveTierSource is shared with the boot compile queue.
        (void)csCode;
        const std::string enhancedSource = ResolveTierSource(
            bindless ? ResolveTierBindlessEnhanced : ResolveTierEnhanced);

        // The enhanced variant gets its OWN descriptor heap, mirroring the
        // default layout in slots [0..85] and appending the feature-specific
        // descriptors after it. Widening the shared heap in place would mean
        // renumbering every hardcoded slot index the default resolve depends
        // on -- exactly the kind of churn that could regress the default path.
        //
        //   [0..78]  t0..t78  as the default resolve
        //   [79..81] u0..u2   as the default resolve
        //   [82..85] b1..b4   as the default resolve
        //   [86]     t79      TLAS                     (Phase 5)
        //   [87]     u3       ray mask                 (Phase 5)
        //   [88]     b5       enhanced constants       (Phase 5)
        //   [89]     t80      svgf history colour      (Phase 5b)
        //   [90]     t81      svgf history moments     (Phase 5b)
        //   [91]     t82      vis buffer history       (Phase 5b)
        //   [92]     u4       svgf colour write        (Phase 5b)
        //   [93]     u5       svgf moments write       (Phase 5b)
        //   [94]     u6       reflection src           (Phase 5c)
        //   [95]     t83      local-to-stable triangle map
        //   [96]     t84      stable surface history
        //   [97]     u7       current stable surfaces
        //   [98]     t85      raytracing hit geometry bindings
        //   [99]     t86      bent-normal GTAO history
        D3D12_DESCRIPTOR_RANGE ranges[23] = {};
        ranges[0] = baseRanges[0];                          // t0..t78
        ranges[1] = baseRanges[1];                          // u0..u2 @ 79
        ranges[2] = baseRanges[2];                          // b1..b4 @ 82

        ranges[3].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[3].NumDescriptors = 1;
        ranges[3].BaseShaderRegister = 79;                  // t79 TLAS
        ranges[3].RegisterSpace = 0;
        ranges[3].OffsetInDescriptorsFromTableStart = 86;

        ranges[4].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[4].NumDescriptors = 1;
        ranges[4].BaseShaderRegister = 3;                   // u3 ray mask
        ranges[4].RegisterSpace = 0;
        ranges[4].OffsetInDescriptorsFromTableStart = 87;

        ranges[5].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
        ranges[5].NumDescriptors = 1;
        ranges[5].BaseShaderRegister = 5;                   // b5 constants
        ranges[5].RegisterSpace = 0;
        ranges[5].OffsetInDescriptorsFromTableStart = 88;

        ranges[6].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[6].NumDescriptors = 1;
        ranges[6].BaseShaderRegister = 80;                  // t80 svgfHistoryColor
        ranges[6].RegisterSpace = 0;
        ranges[6].OffsetInDescriptorsFromTableStart = 89;

        ranges[7].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[7].NumDescriptors = 1;
        ranges[7].BaseShaderRegister = 81;                  // t81 svgfHistoryMoments
        ranges[7].RegisterSpace = 0;
        ranges[7].OffsetInDescriptorsFromTableStart = 90;

        ranges[8].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[8].NumDescriptors = 1;
        ranges[8].BaseShaderRegister = 82;                  // t82 visBufferHistory
        ranges[8].RegisterSpace = 0;
        ranges[8].OffsetInDescriptorsFromTableStart = 91;

        ranges[9].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[9].NumDescriptors = 1;
        ranges[9].BaseShaderRegister = 4;                   // u4 svgfHistoryColorWrite
        ranges[9].RegisterSpace = 0;
        ranges[9].OffsetInDescriptorsFromTableStart = 92;

        ranges[10].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[10].NumDescriptors = 1;
        ranges[10].BaseShaderRegister = 5;                  // u5 svgfHistoryMomentsWrite
        ranges[10].RegisterSpace = 0;
        ranges[10].OffsetInDescriptorsFromTableStart = 93;

        ranges[11].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[11].NumDescriptors = 1;
        ranges[11].BaseShaderRegister = 6;                  // u6 outputReflectionSrc
        ranges[11].RegisterSpace = 0;
        ranges[11].OffsetInDescriptorsFromTableStart = 94;

        ranges[12].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[12].NumDescriptors = 1;
        ranges[12].BaseShaderRegister = 83;
        ranges[12].RegisterSpace = 0;
        ranges[12].OffsetInDescriptorsFromTableStart = 95;

        ranges[13].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[13].NumDescriptors = 1;
        ranges[13].BaseShaderRegister = 84;
        ranges[13].RegisterSpace = 0;
        ranges[13].OffsetInDescriptorsFromTableStart = 96;

        ranges[14].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[14].NumDescriptors = 1;
        ranges[14].BaseShaderRegister = 7;
        ranges[14].RegisterSpace = 0;
        ranges[14].OffsetInDescriptorsFromTableStart = 97;

        ranges[15].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[15].NumDescriptors = 1;
        ranges[15].BaseShaderRegister = 85;                 // t85 hitGeometry
        ranges[15].RegisterSpace = 0;
        ranges[15].OffsetInDescriptorsFromTableStart = 98;

        ranges[16].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[16].NumDescriptors = 1;
        ranges[16].BaseShaderRegister = 86;                 // t86 bent GTAO
        ranges[16].RegisterSpace = 0;
        ranges[16].OffsetInDescriptorsFromTableStart = 99;

        //   [100..102] t87..t89 terrain triplanar layer arrays
        ranges[17].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[17].NumDescriptors = 3;
        ranges[17].BaseShaderRegister = 87;
        ranges[17].RegisterSpace = 0;
        ranges[17].OffsetInDescriptorsFromTableStart = 100;

        //   [103] t91 terrain splatmap
        ranges[18].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[18].NumDescriptors = 1;
        ranges[18].BaseShaderRegister = 91;
        ranges[18].RegisterSpace = 0;
        ranges[18].OffsetInDescriptorsFromTableStart = 103;

        //   [104] t92 spot shadow atlas
        ranges[19].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[19].NumDescriptors = 1;
        ranges[19].BaseShaderRegister = 92;
        ranges[19].RegisterSpace = 0;
        ranges[19].OffsetInDescriptorsFromTableStart = 104;

        for (UINT i = 0; i < 3; ++i) {
            ranges[20 + i].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
            ranges[20 + i].NumDescriptors = 1;
            ranges[20 + i].BaseShaderRegister = 8 + i;
            ranges[20 + i].RegisterSpace = 0;
            ranges[20 + i].OffsetInDescriptorsFromTableStart = 105 + i;
        }
        // Radiance cascades extend this exact table past its last slot.
        static_assert(RadianceCascadesDX12::BaseDescriptorCount ==
                      kEnhancedResolveDescriptorCount,
                      "cascade tables must start where the enhanced table ends");
        if (!bindless)
            radianceCascades.Configure(ranges, _countof(ranges), samplers);
        if (!bindless)
            variableRateGI.Configure(ranges, _countof(ranges), samplers);

        D3D12_ROOT_PARAMETER params[8] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor.ShaderRegister = 0;
        params[0].Descriptor.RegisterSpace = 0;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = _countof(ranges);
        params[1].DescriptorTable.pDescriptorRanges = ranges;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        // t90 tile list, matching the default tier. See CreateResolvePipeline.
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[2].Descriptor.ShaderRegister = 90;
        params[2].Descriptor.RegisterSpace = 0;
        params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        // u16 Lumen radiance cache. A root UAV, so the descriptor table, its
        // heap counts and the bindless transient copy are all unchanged.
        params[kEnhancedRadianceCacheRootParameter].ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[kEnhancedRadianceCacheRootParameter].Descriptor.ShaderRegister =
            16;
        params[kEnhancedRadianceCacheRootParameter].ShaderVisibility =
            D3D12_SHADER_VISIBILITY_ALL;
        // u17/u18 ReSTIR reservoirs, root UAVs for the same reason.
        for (UINT i = 0; i < 2; ++i) {
            params[kEnhancedReSTIRRootParameter + i].ParameterType =
                D3D12_ROOT_PARAMETER_TYPE_UAV;
            params[kEnhancedReSTIRRootParameter + i].Descriptor.ShaderRegister =
                17 + i;
            params[kEnhancedReSTIRRootParameter + i].ShaderVisibility =
                D3D12_SHADER_VISIBILITY_ALL;
        }
        // t100 emissive triangle lights, u19 their ReSTIR DI reservoirs.
        params[kEnhancedEmissiveRootParameter].ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[kEnhancedEmissiveRootParameter].Descriptor.ShaderRegister = 100;
        params[kEnhancedEmissiveRootParameter].ShaderVisibility =
            D3D12_SHADER_VISIBILITY_ALL;
        params[kEnhancedEmissiveRootParameter + 1].ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[kEnhancedEmissiveRootParameter + 1].Descriptor.ShaderRegister = 19;
        params[kEnhancedEmissiveRootParameter + 1].ShaderVisibility =
            D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rootSigDesc = {};
        // The CBV, the descriptor table, the t90 tile-list root SRV, the u16
        // radiance-cache root UAV, the u17/u18 ReSTIR root UAVs and the
        // t100/u19 emissive-light pair.
        rootSigDesc.NumParameters = _countof(params);
        rootSigDesc.pParameters = params;
        rootSigDesc.NumStaticSamplers = 3;  // s0 wrap, s1 shadow cmp, s2 clamp (terrain splat)
        rootSigDesc.pStaticSamplers = samplers;
        // The flag that makes ResourceDescriptorHeap[] legal in the shader.
        // Samplers stay static, so SAMPLER_HEAP_DIRECTLY_INDEXED is not needed.
        if (bindless)
            rootSigDesc.Flags =
                D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;

        ComPtr<ID3D12RootSignature>& targetRootSig =
            bindless ? bindlessEnhancedResolveRootSig : enhancedResolveRootSig;
        ComPtr<ID3D12PipelineState>& targetPSO =
            bindless ? bindlessEnhancedResolvePSO : enhancedResolvePSO;

        ComPtr<ID3DBlob> sigBlob, sigError;
        if (FAILED(D3D12SerializeRootSignature(&rootSigDesc,
                D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &sigError))) {
            if (sigError)
                std::cerr << tierName << " resolve root sig: "
                          << (const char*)sigError->GetBufferPointer() << "\n";
            return;
        }
        if (FAILED(g_dx12.device->CreateRootSignature(
                0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(),
                IID_PPV_ARGS(&targetRootSig))))
            return;

        // Only the base permutation is built here: the tier selection gates on
        // it. This tier's terrain set (the two halves of the split dispatch and
        // their tile-classified twins) comes from BuildTerrainSet -- at boot
        // when the settings predict this tier, otherwise in the background the
        // first time the tier is actually selected.
        {
            const int tier =
                bindless ? ResolveTierBindlessEnhanced : ResolveTierEnhanced;
            ComPtr<ID3DBlob> blob;
            std::string errors;
            if (!CompileResolveVariant(tier, enhancedSource, blob, errors)) {
                std::cerr << tierName << ": resolve compile failed\n";
                if (!errors.empty()) {
                    std::cerr << errors << std::endl;
                    std::ofstream log(bindless
                                          ? "bindless_enhanced_resolve_shader_error.log"
                                          : "enhanced_resolve_shader_error.log",
                                      std::ios::trunc);
                    log << errors;
                }
                targetRootSig.Reset();
                return;
            }
            D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
            desc.pRootSignature = targetRootSig.Get();
            desc.CS = { blob->GetBufferPointer(), blob->GetBufferSize() };
            const double start = BootTimer::MillisecondsNow();
            if (FAILED(g_dx12.device->CreateComputePipelineState(
                    &desc, IID_PPV_ARGS(&targetPSO)))) {
                targetPSO.Reset();
                std::cerr << tierName << ": resolve PSO creation failed\n";
                targetRootSig.Reset();
                return;
            }
            char text[64];
            std::snprintf(text, sizeof(text), "driver PSO build %6.0f ms  ",
                          BootTimer::MillisecondsNow() - start);
            BootTimer::Log(text + std::string(tierName) + " resolve");
        }

        // The bindless variant shares the enhanced tier's descriptor heap and
        // constant buffer, both already built by the non-bindless call. It
        // needs nothing further, so it reports ready here rather than falling
        // through to the enhanced-only resource creation below.
        if (bindless) {
            bindlessEnhancedResolveReady = true;
            return;
        }

        // 256-byte aligned upload CBV for the enhanced constants, persistently
        // mapped like the other per-frame buffers here.
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC bufferDesc = {};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width = 256ull * FRAME_COUNT;
        bufferDesc.Height = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels = 1;
        bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(g_dx12.device->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&enhancedConstantBuffer)))) {
            enhancedResolvePSO.Reset();
            enhancedResolveRootSig.Reset();
            return;
        }
        D3D12_RANGE noRead = { 0, 0 };
        enhancedConstantBuffer->Map(0, &noRead, &enhancedConstantMapped);

        if (!CreateRayMaskResources()) {
            enhancedResolvePSO.Reset();
            enhancedResolveRootSig.Reset();
            return;
        }

        enhancedPipelineReady = true;
        std::cout << "Enhanced visuals: SM6.5 resolve ready (inline RayQuery)\n";
    }

    // Phase 5c: SVGF à-trous spatial filter. Multi-iteration cross-bilateral
    // wavelet applied to the specular IBL signal after the temporal pass.
    // Compiled via DXC at cs_6_5; failure leaves atrousPipelineReady false
    // and the toggle is harmless.
    void CreateSVGFAtrousPipeline() {
        svgfAtrousPipelineReady = false;
        if (!ShaderCacheDX12::DxcAvailable()) {
            std::cout << "SVGF à-trous: dxcompiler.dll unavailable\n";
            return;
        }

        // Compile the à-trous shader
        const std::wstring shaderDirectory =
            ShaderCacheDX12::ExecutableDirectory() + L"shaders";
        auto loadShaderSource = [](const std::wstring& path,
                                   std::string& source) {
            std::ifstream file(path, std::ios::binary);
            if (!file) return false;
            std::stringstream stream;
            stream << file.rdbuf();
            source = stream.str();
            return !source.empty();
        };

        std::string atrousSource;
        if (!loadShaderSource(
                shaderDirectory + L"\\svgf_atrous_cs.hlsl", atrousSource)) {
            std::cerr << "SVGF a-trous: shader source unavailable\n";
            return;
        }
        ComPtr<ID3DBlob> atrousBlob;
        std::string errors;
        if (!ShaderCacheDX12::CompileCachedDXC(
                atrousSource, L"svgf_atrous_cs.hlsl",
                L"main", L"cs_6_5", shaderDirectory, &atrousBlob, &errors)) {
            std::cerr << "SVGF à-trous: compile failed\n";
            if (!errors.empty()) {
                std::cerr << errors << std::endl;
                std::ofstream log("svgf_atrous_error.log", std::ios::trunc);
                log << errors;
            }
            return;
        }

        // Compile the composite shader
        std::string compositeSource;
        if (!loadShaderSource(
                shaderDirectory + L"\\svgf_atrous_composite_cs.hlsl",
                compositeSource)) {
            std::cerr << "SVGF a-trous composite: shader source unavailable\n";
            return;
        }
        ComPtr<ID3DBlob> compositeBlob;
        errors.clear();
        if (!ShaderCacheDX12::CompileCachedDXC(
                compositeSource,
                L"svgf_atrous_composite_cs.hlsl",
                L"main", L"cs_6_5", shaderDirectory, &compositeBlob, &errors)) {
            std::cerr << "SVGF à-trous composite: compile failed\n";
            if (!errors.empty()) {
                std::cerr << errors << std::endl;
                std::ofstream log("svgf_composite_error.log", std::ios::trunc);
                log << errors;
            }
            return;
        }

        // À-trous root signature: root CBV b0 + descriptor table
        //   t0: reflectionSrc       t1: depthBuffer
        //   t2: normalRoughness     t3: historyMoments
        //   u0: scratchA            u1: scratchB
        {
            D3D12_DESCRIPTOR_RANGE atrousRanges[6] = {};
            atrousRanges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            atrousRanges[0].NumDescriptors = 4;
            atrousRanges[0].BaseShaderRegister = 0;  // t0..t3
            atrousRanges[0].RegisterSpace = 0;
            atrousRanges[0].OffsetInDescriptorsFromTableStart = 0;

            atrousRanges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
            atrousRanges[1].NumDescriptors = 2;
            atrousRanges[1].BaseShaderRegister = 0;  // u0..u1
            atrousRanges[1].RegisterSpace = 0;
            atrousRanges[1].OffsetInDescriptorsFromTableStart = 4;

            D3D12_ROOT_PARAMETER atrousParams[2] = {};
            atrousParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            atrousParams[0].Descriptor.ShaderRegister = 0;
            atrousParams[0].Descriptor.RegisterSpace = 0;
            atrousParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            atrousParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            atrousParams[1].DescriptorTable.NumDescriptorRanges = 2;
            atrousParams[1].DescriptorTable.pDescriptorRanges = atrousRanges;
            atrousParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

            D3D12_STATIC_SAMPLER_DESC atrousSampler = {};
            atrousSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
            atrousSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            atrousSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            atrousSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            atrousSampler.ShaderRegister = 0;
            atrousSampler.RegisterSpace = 0;
            atrousSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

            D3D12_ROOT_SIGNATURE_DESC atrousSigDesc = {};
            atrousSigDesc.NumParameters = 2;
            atrousSigDesc.pParameters = atrousParams;
            atrousSigDesc.NumStaticSamplers = 1;
            atrousSigDesc.pStaticSamplers = &atrousSampler;

            ComPtr<ID3DBlob> sigBlob, sigError;
            if (FAILED(D3D12SerializeRootSignature(&atrousSigDesc,
                    D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &sigError))) {
                if (sigError) std::cerr << "SVGF à-trous root sig: "
                    << (const char*)sigError->GetBufferPointer() << "\n";
                return;
            }
            if (FAILED(g_dx12.device->CreateRootSignature(
                    0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(),
                    IID_PPV_ARGS(&svgfAtrousRootSig))))
                return;

            D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
            psoDesc.pRootSignature = svgfAtrousRootSig.Get();
            psoDesc.CS = { atrousBlob->GetBufferPointer(), atrousBlob->GetBufferSize() };
            if (FAILED(g_dx12.device->CreateComputePipelineState(
                    &psoDesc, IID_PPV_ARGS(&svgfAtrousPSO)))) {
                std::cerr << "SVGF à-trous: PSO creation failed\n";
                svgfAtrousRootSig.Reset();
                return;
            }
        }

        // À-trous descriptor heaps: one mutable heap per frame slot.
        {
            D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
            // Three 6-descriptor sets, one per ping-pong parity:
            //   [0..5]   reflectionSrc -> scratchA   (first iteration)
            //   [6..11]  scratchA      -> scratchB
            //   [12..17] scratchB      -> scratchA
            heapDesc.NumDescriptors = 18;
            heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            for (UINT frame = 0; frame < FRAME_COUNT; ++frame) {
                if (FAILED(g_dx12.device->CreateDescriptorHeap(
                        &heapDesc,
                        IID_PPV_ARGS(&svgfAtrousDescHeaps[frame]))))
                    return;
            }
        }

        // À-trous constant upload buffer
        {
            D3D12_HEAP_PROPERTIES heapProps = {};
            heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC bufferDesc = {};
            bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bufferDesc.Width = 256ull * FRAME_COUNT *
                               kSVGFAtrousMaxIterations;
            bufferDesc.Height = 1;
            bufferDesc.DepthOrArraySize = 1;
            bufferDesc.MipLevels = 1;
            bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
            bufferDesc.SampleDesc.Count = 1;
            bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(g_dx12.device->CreateCommittedResource(
                    &heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                    IID_PPV_ARGS(&svgfAtrousConstantBuffer))))
                return;
            D3D12_RANGE noRead = { 0, 0 };
            svgfAtrousConstantBuffer->Map(0, &noRead, &svgfAtrousConstantMapped);
        }

        // Composite root signature: root CBV b0 + descriptor table
        //   t0: outputTexture   t1: srcReflection   t3: filteredReflection
        //   u0: compositeOutput (scratch[!(finalIdx)] as UAV)
        {
            D3D12_DESCRIPTOR_RANGE compositeRanges[3] = {};
            compositeRanges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            compositeRanges[0].NumDescriptors = 3;
            compositeRanges[0].BaseShaderRegister = 0;  // t0..t2
            compositeRanges[0].RegisterSpace = 0;
            compositeRanges[0].OffsetInDescriptorsFromTableStart = 0;

            compositeRanges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
            compositeRanges[1].NumDescriptors = 1;
            compositeRanges[1].BaseShaderRegister = 0;  // u0
            compositeRanges[1].RegisterSpace = 0;
            compositeRanges[1].OffsetInDescriptorsFromTableStart = 3;

            D3D12_ROOT_PARAMETER compositeParams[2] = {};
            compositeParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            compositeParams[0].Descriptor.ShaderRegister = 0;
            compositeParams[0].Descriptor.RegisterSpace = 0;
            compositeParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            compositeParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            compositeParams[1].DescriptorTable.NumDescriptorRanges = 2;
            compositeParams[1].DescriptorTable.pDescriptorRanges = compositeRanges;
            compositeParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

            D3D12_ROOT_SIGNATURE_DESC compositeSigDesc = {};
            compositeSigDesc.NumParameters = 2;
            compositeSigDesc.pParameters = compositeParams;
            compositeSigDesc.NumStaticSamplers = 0;
            compositeSigDesc.pStaticSamplers = nullptr;

            ComPtr<ID3DBlob> sigBlob, sigError;
            if (FAILED(D3D12SerializeRootSignature(&compositeSigDesc,
                    D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &sigError))) {
                if (sigError) std::cerr << "SVGF composite root sig: "
                    << (const char*)sigError->GetBufferPointer() << "\n";
                return;
            }
            if (FAILED(g_dx12.device->CreateRootSignature(
                    0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(),
                    IID_PPV_ARGS(&svgfCompositeRootSig))))
                return;

            D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
            psoDesc.pRootSignature = svgfCompositeRootSig.Get();
            psoDesc.CS = { compositeBlob->GetBufferPointer(), compositeBlob->GetBufferSize() };
            if (FAILED(g_dx12.device->CreateComputePipelineState(
                    &psoDesc, IID_PPV_ARGS(&svgfCompositePSO)))) {
                std::cerr << "SVGF composite: PSO creation failed\n";
                svgfCompositeRootSig.Reset();
                return;
            }
        }

        // Composite descriptor heaps: one mutable heap per frame slot.
        {
            D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
            heapDesc.NumDescriptors = 4;
            heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            for (UINT frame = 0; frame < FRAME_COUNT; ++frame) {
                if (FAILED(g_dx12.device->CreateDescriptorHeap(
                        &heapDesc,
                        IID_PPV_ARGS(&svgfCompositeDescHeaps[frame]))))
                    return;
            }
        }

        // Composite constant upload buffer
        {
            D3D12_HEAP_PROPERTIES heapProps = {};
            heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC bufferDesc = {};
            bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bufferDesc.Width = 256ull * FRAME_COUNT;
            bufferDesc.Height = 1;
            bufferDesc.DepthOrArraySize = 1;
            bufferDesc.MipLevels = 1;
            bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
            bufferDesc.SampleDesc.Count = 1;
            bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(g_dx12.device->CreateCommittedResource(
                    &heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                    IID_PPV_ARGS(&svgfCompositeConstantBuffer))))
                return;
            D3D12_RANGE noRead = { 0, 0 };
            svgfCompositeConstantBuffer->Map(0, &noRead, &svgfCompositeConstantMapped);
        }

        svgfAtrousPipelineReady = true;
        std::cout << "SVGF à-trous: spatial filter ready (" << svgfAtrousIterations
                  << " iterations)\n";
    }

    // Ray-mask texture (u3) plus the staging buffer used to read back what
    // fraction of the screen was routed to RT.
    bool CreateRayMaskResources() {
        D3D12_HEAP_PROPERTIES defaultHeap = {};
        defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8_UINT;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(g_dx12.device->CreateCommittedResource(
                &defaultHeap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                IID_PPV_ARGS(&rayMaskTexture))))
            return false;

        // Readback of a single scanline, sampled every few frames. The
        // statistic only needs to be indicative -- copying the whole mask each
        // frame would cost more bandwidth than the rays it is measuring.
        rayMaskRowPitch =
            (width + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u) &
            ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u);
        D3D12_HEAP_PROPERTIES readbackHeap = {};
        readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC bufferDesc = {};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width = (UINT64)rayMaskRowPitch * kRayMaskSampleRows;
        bufferDesc.Height = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels = 1;
        bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(g_dx12.device->CreateCommittedResource(
                &readbackHeap, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&rayMaskReadback))))
            return false;
        rayMaskCopyPending = false;
        rayMaskFrameCounter = 0;
        return true;
    }

    // Copies a few scanlines of the mask for the CPU to sample, and reduces
    // whatever the *previous* copy left in the readback buffer.
    //
    // Never maps a resource the GPU might still be writing: the copy issued
    // this frame is read some frames later, by which point the frame fence has
    // long since passed it. That is why this is a statistic and not a
    // synchronisation point.
    void UpdateRayMaskStatistic(ID3D12GraphicsCommandList* cmdList) {
        if (!rayMaskTexture || !rayMaskReadback) return;

        // Reduce the previous copy first, before overwriting it.
        if (rayMaskCopyPending &&
            rayMaskFrameCounter % kRayMaskSampleInterval == 0) {
            D3D12_RANGE readRange = {
                0, (SIZE_T)rayMaskRowPitch * kRayMaskSampleRows };
            void* mapped = nullptr;
            if (SUCCEEDED(rayMaskReadback->Map(0, &readRange, &mapped)) && mapped) {
                const auto* bytes = static_cast<const uint8_t*>(mapped);
                uint32_t traced = 0, shadowTraced = 0, reflectionTraced = 0;
                uint32_t giTraced = 0;
                uint32_t total = 0;
                for (UINT row = 0; row < kRayMaskSampleRows; ++row) {
                    const uint8_t* line = bytes + (size_t)row * rayMaskRowPitch;
                    for (UINT x = 0; x < width; ++x) {
                        const uint8_t mask = line[x];
                        traced += mask != 0 ? 1u : 0u;
                        // Bit 0 shadow, bit 1 reflection. Reported apart
                        // because the shadow gate can trace most lit pixels,
                        // which saturates the combined figure and makes the
                        // reflection fraction unreadable.
                        shadowTraced += (mask & 1u) ? 1u : 0u;
                        reflectionTraced += (mask & 2u) ? 1u : 0u;
                        giTraced += (mask & 4u) ? 1u : 0u;
                        ++total;
                    }
                }
                D3D12_RANGE noWrite = { 0, 0 };
                rayMaskReadback->Unmap(0, &noWrite);
                rayMaskFraction = total ? (float)traced / (float)total : 0.0f;
                rayMaskShadowFraction =
                    total ? (float)shadowTraced / (float)total : 0.0f;
                rayMaskReflectionFraction =
                    total ? (float)reflectionTraced / (float)total : 0.0f;
                rayMaskGIFraction =
                    total ? (float)giTraced / (float)total : 0.0f;
            }
            rayMaskCopyPending = false;
        }

        ++rayMaskFrameCounter;
        if (rayMaskFrameCounter % kRayMaskSampleInterval != 0) return;

        // Sample rows spread down the screen rather than a contiguous band, so
        // the statistic is not dominated by whatever happens to be at the top.
        D3D12_RESOURCE_BARRIER toCopy = {};
        toCopy.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        toCopy.Transition.pResource = rayMaskTexture.Get();
        toCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        toCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        toCopy.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &toCopy);

        for (UINT row = 0; row < kRayMaskSampleRows; ++row) {
            const UINT sourceY =
                (UINT)((uint64_t)height * (row * 2u + 1u) /
                       (kRayMaskSampleRows * 2u));
            D3D12_TEXTURE_COPY_LOCATION source = {};
            source.pResource = rayMaskTexture.Get();
            source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            source.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION destination = {};
            destination.pResource = rayMaskReadback.Get();
            destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            destination.PlacedFootprint.Offset =
                (UINT64)rayMaskRowPitch * row;
            destination.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8_UINT;
            destination.PlacedFootprint.Footprint.Width = width;
            destination.PlacedFootprint.Footprint.Height = 1;
            destination.PlacedFootprint.Footprint.Depth = 1;
            destination.PlacedFootprint.Footprint.RowPitch = rayMaskRowPitch;
            D3D12_BOX box = {};
            box.left = 0;
            box.right = width;
            box.top = sourceY;
            box.bottom = sourceY + 1u;
            box.front = 0;
            box.back = 1;
            cmdList->CopyTextureRegion(&destination, 0, 0, 0, &source, &box);
        }

        toCopy.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        toCopy.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        cmdList->ResourceBarrier(1, &toCopy);
        rayMaskCopyPending = true;
    }

    // Builds the enhanced heap: a copy of the default resolve heap's original
    // 86 descriptors, then TLAS / ray mask / enhanced constants appended.
    //
    // Updates just the current frame heap's SVGF slots. Normal shading supplies
    // colour/moment ping indices; authored identity uses the post-owned roles.
    void RefreshSVGFDescriptors(UINT frameSlot, UINT readIndex,
                                UINT writeIndex) {
        if (frameSlot >= FRAME_COUNT ||
            !enhancedComputeDescHeaps[frameSlot] || !svgfHistoryColor[0])
            return;
        const UINT descSize = g_dx12.cbvSrvUavDescriptorSize;
        D3D12_CPU_DESCRIPTOR_HANDLE handle =
            enhancedComputeDescHeaps[frameSlot]
                ->GetCPUDescriptorHandleForHeapStart();

        // [89] t80 - svgf history colour read (previous frame's ping)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
            srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1;
            D3D12_CPU_DESCRIPTOR_HANDLE h = handle;
            h.ptr += (UINT64)descSize * 89;
            g_dx12.device->CreateShaderResourceView(
                (ScopeSurfaceBound() ? nullptr : svgfHistoryColor[readIndex].Get()), &srv, h);
        }

        // [90] t81 - svgf history moments read
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
            srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1;
            D3D12_CPU_DESCRIPTOR_HANDLE h = handle;
            h.ptr += (UINT64)descSize * 90;
            g_dx12.device->CreateShaderResourceView(
                (ScopeSurfaceBound() ? nullptr : svgfHistoryMoments[readIndex].Get()), &srv, h);
        }

        // [91] t82 - obsolete raw-ID history binding kept null so the enhanced
        // root layout and all later hardcoded slots remain unchanged.
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
            srv.Format = DXGI_FORMAT_R32G32_UINT;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1;
            D3D12_CPU_DESCRIPTOR_HANDLE h = handle;
            h.ptr += (UINT64)descSize * 91;
            g_dx12.device->CreateShaderResourceView(nullptr, &srv, h);
        }

        // [92] u4 - svgf colour write (current frame's ping)
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
            uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            D3D12_CPU_DESCRIPTOR_HANDLE h = handle;
            h.ptr += (UINT64)descSize * 92;
            g_dx12.device->CreateUnorderedAccessView(
                (ScopeSurfaceBound() ? nullptr : svgfHistoryColor[writeIndex].Get()), nullptr, &uav, h);
        }

        // [93] u5 - svgf moments write
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
            uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            D3D12_CPU_DESCRIPTOR_HANDLE h = handle;
            h.ptr += (UINT64)descSize * 93;
            g_dx12.device->CreateUnorderedAccessView(
                (ScopeSurfaceBound() ? nullptr : svgfHistoryMoments[writeIndex].Get()), nullptr, &uav, h);
        }

        // [96]/[97] - the same previous/current authored identity pair post
        // consumes later on this command list.
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
            srv.Format = DXGI_FORMAT_R32G32_UINT;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping =
                D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1;
            D3D12_CPU_DESCRIPTOR_HANDLE h = handle;
            h.ptr += (UINT64)descSize * 96;
            g_dx12.device->CreateShaderResourceView(
                (ScopeSurfaceBound() ? nullptr : StableSurfaceResource(stableSurfaceWriteIndex ^ 1u)), &srv, h);
        }
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
            uav.Format = DXGI_FORMAT_R32G32_UINT;
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            D3D12_CPU_DESCRIPTOR_HANDLE h = handle;
            h.ptr += (UINT64)descSize * 97;
            g_dx12.device->CreateUnorderedAccessView(
                StableSurfaceResource(stableSurfaceWriteIndex), nullptr,
                &uav, h);
        }
    }

    // Called after the default heap is populated and whenever the TLAS address
    // changes. Copying rather than sharing keeps the default layout's
    // hardcoded indices untouched.
    void RefreshEnhancedDescriptors(UINT frameSlot) {
        if (frameSlot >= FRAME_COUNT || !enhancedPipelineReady ||
            !computeDescHeap)
            return;

        if (!enhancedComputeDescHeaps[frameSlot]) {
            D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
            heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            // [103] t91 terrain splatmap, appended above the terrain arrays.
            heapDesc.NumDescriptors = kEnhancedResolveDescriptorCount;
            heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            if (FAILED(g_dx12.device->CreateDescriptorHeap(
                    &heapDesc,
                    IID_PPV_ARGS(&enhancedComputeDescHeaps[frameSlot]))))
                return;
        }

        ID3D12DescriptorHeap* enhancedHeap =
            enhancedComputeDescHeaps[frameSlot].Get();

        const UINT descSize = g_dx12.cbvSrvUavDescriptorSize;
        // Mirror the default heap. CopyDescriptorsSimple needs a non-shader-
        // visible source, so this copies through the CPU handle of the shader-
        // visible heap, which is legal for CBV_SRV_UAV heaps.
        g_dx12.device->CopyDescriptorsSimple(86,
            enhancedHeap->GetCPUDescriptorHandleForHeapStart(),
            computeDescHeap->GetCPUDescriptorHandleForHeapStart(),
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        D3D12_CPU_DESCRIPTOR_HANDLE handle =
            enhancedHeap->GetCPUDescriptorHandleForHeapStart();

        // [100..102] t87..t89 - terrain layer arrays. The enhanced resolve
        // never takes the terrain branch, but its root signature declares the
        // range, so the slots still have to hold valid descriptors.
        WriteTerrainDescriptors(enhancedHeap, 100);
        // [104] t92 spot shadow atlas, one past the splatmap at [103].
        WriteSpotShadowAtlasDescriptor(enhancedHeap, 104);

        // [86] t79 - TLAS. The slot must hold a valid descriptor even when
        // there is no acceleration structure yet: the runtime requires every
        // slot in a bound table to be populated, and an untouched slot is
        // garbage rather than null. A zero Location is the documented way to
        // express a null TLAS binding.
        {
            D3D12_CPU_DESCRIPTOR_HANDLE tlasHandle = handle;
            tlasHandle.ptr += (UINT64)descSize * 86;
            D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
            srv.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.RaytracingAccelerationStructure.Location = enhancedTLASAddress;
            g_dx12.device->CreateShaderResourceView(nullptr, &srv, tlasHandle);
        }

        // [87] u3 - ray mask
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
            uav.Format = DXGI_FORMAT_R8_UINT;
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            D3D12_CPU_DESCRIPTOR_HANDLE maskHandle = handle;
            maskHandle.ptr += (UINT64)descSize * 87;
            g_dx12.device->CreateUnorderedAccessView(
                rayMaskTexture.Get(), nullptr, &uav, maskHandle);
        }

        // [88] b5 - enhanced constants
        {
            D3D12_CONSTANT_BUFFER_VIEW_DESC cbv = {};
            cbv.BufferLocation =
                enhancedConstantBuffer->GetGPUVirtualAddress() +
                static_cast<UINT64>(ViewFrameIndex()) * 256ull;
            cbv.SizeInBytes = 256;
            D3D12_CPU_DESCRIPTOR_HANDLE cbvHandle = handle;
            cbvHandle.ptr += (UINT64)descSize * 88;
            g_dx12.device->CreateConstantBufferView(&cbv, cbvHandle);
        }

        // [89] t80 - svgf history colour (SRV, read side of ping-pong)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
            srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1;
            D3D12_CPU_DESCRIPTOR_HANDLE h = handle;
            h.ptr += (UINT64)descSize * 89;
            g_dx12.device->CreateShaderResourceView(
                (ScopeSurfaceBound() ? nullptr : svgfHistoryColor[svgfHistoryPing].Get()), &srv, h);
        }

        // [90] t81 - svgf history moments (SRV, read side of ping-pong)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
            srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1;
            D3D12_CPU_DESCRIPTOR_HANDLE h = handle;
            h.ptr += (UINT64)descSize * 90;
            g_dx12.device->CreateShaderResourceView(
                (ScopeSurfaceBound() ? nullptr : svgfHistoryMoments[svgfHistoryPing].Get()), &srv, h);
        }

        // [91] t82 - obsolete raw-ID history binding, intentionally null.
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
            srv.Format = DXGI_FORMAT_R32G32_UINT;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1;
            D3D12_CPU_DESCRIPTOR_HANDLE h = handle;
            h.ptr += (UINT64)descSize * 91;
            g_dx12.device->CreateShaderResourceView(nullptr, &srv, h);
        }

        // [92] u4 - svgf colour write (UAV, write side of ping-pong)
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
            uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            D3D12_CPU_DESCRIPTOR_HANDLE h = handle;
            h.ptr += (UINT64)descSize * 92;
            g_dx12.device->CreateUnorderedAccessView(
                (ScopeSurfaceBound() ? nullptr : svgfHistoryColor[svgfHistoryPing ^ 1u].Get()), nullptr, &uav, h);
        }

        // [93] u5 - svgf moments write (UAV, write side of ping-pong)
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
            uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            D3D12_CPU_DESCRIPTOR_HANDLE h = handle;
            h.ptr += (UINT64)descSize * 93;
            g_dx12.device->CreateUnorderedAccessView(
                (ScopeSurfaceBound() ? nullptr : svgfHistoryMoments[svgfHistoryPing ^ 1u].Get()), nullptr, &uav, h);
        }

        // [94] u6 - reflection source (UAV, specular IBL from resolve)
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
            uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            D3D12_CPU_DESCRIPTOR_HANDLE h = handle;
            h.ptr += (UINT64)descSize * 94;
            g_dx12.device->CreateUnorderedAccessView(
                svgfReflectionSrc.Get(), nullptr, &uav, h);
        }

        // [95] t83 - current primitive index to persistent triangle ID.
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
            srv.Format = DXGI_FORMAT_UNKNOWN;
            srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Buffer.FirstElement = 0;
            srv.Buffer.NumElements = geometryTriangleCapacity;
            srv.Buffer.StructureByteStride = sizeof(UINT);
            D3D12_CPU_DESCRIPTOR_HANDLE h = handle;
            h.ptr += (UINT64)descSize * 95;
            g_dx12.device->CreateShaderResourceView(
                stableTriangleDataBuffer.Get(), &srv, h);
        }

        // [96] t84 - last committed stable surface key.
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
            srv.Format = DXGI_FORMAT_R32G32_UINT;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1;
            D3D12_CPU_DESCRIPTOR_HANDLE h = handle;
            h.ptr += (UINT64)descSize * 96;
            g_dx12.device->CreateShaderResourceView(
                (ScopeSurfaceBound() ? nullptr : StableSurfaceResource(stableSurfaceWriteIndex ^ 1u)), &srv, h);
        }

        // [97] u7 - stable key emitted by this enhanced resolve.
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
            uav.Format = DXGI_FORMAT_R32G32_UINT;
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            D3D12_CPU_DESCRIPTOR_HANDLE h = handle;
            h.ptr += (UINT64)descSize * 97;
            g_dx12.device->CreateUnorderedAccessView(
                StableSurfaceResource(stableSurfaceWriteIndex), nullptr,
                &uav, h);
        }

        // [98] t85 - per-geometry raytracing hit bindings. NumElements follows
        // the uploaded count so an out-of-range hit index reads nothing rather
        // than a stale entry from a previous scene; the shader range-checks
        // against hitGeometryCount as well.
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
            srv.Format = DXGI_FORMAT_UNKNOWN;
            srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Buffer.FirstElement = 0;
            srv.Buffer.NumElements = (std::max)(hitGeometryCount, 1u);
            srv.Buffer.StructureByteStride = kHitGeometryStride;
            D3D12_CPU_DESCRIPTOR_HANDLE h = handle;
            h.ptr += (UINT64)descSize * 98;
            g_dx12.device->CreateShaderResourceView(
                hitGeometryBuffer.Get(), &srv, h);
        }
        // [99] t86 - common bent-normal GTAO history. It is refreshed again
        // for the current frame immediately before dispatch when history is
        // valid; a null descriptor keeps the inactive branch well-defined.
        WriteBentNormalHistoryDescriptor(
            enhancedHeap, 99, nullptr);
        ID3D12Resource* rrResources[] = { rrDiffuseAlbedo.Get(),
            rrSpecularAlbedo.Get(), rrSpecularHitDistance.Get() };
        for (UINT i = 0; i < 3; ++i) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
            uav.Format = i == 2 ? DXGI_FORMAT_R16_FLOAT :
                DXGI_FORMAT_R16G16B16A16_FLOAT;
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            D3D12_CPU_DESCRIPTOR_HANDLE h = handle;
            h.ptr += static_cast<UINT64>(descSize) * (105 + i);
            g_dx12.device->CreateUnorderedAccessView(
                rrResources[i], nullptr, &uav, h);
        }
        enhancedHeapTLASAddresses[frameSlot] = enhancedTLASAddress;
        enhancedHeapRRGeneration[frameSlot] = rrGuideGeneration;
    }

public:
    // One emissive triangle as a light. Mirrors EmissiveTriangle in
    // emissive_restir.hlsli field for field (96 bytes).
    struct EmissiveTriangleGPU {
        float p0[3];
        uint32_t materialID;
        float e1[3];
        uint32_t bindlessMaterialID;
        float e2[3];
        float area;
        float uv0[2];
        float uv1[2];
        float uv2[2];
        float pdf;
        float aliasProb;
        uint32_t alias;
        uint32_t lightCount;
        float pad[2];
    };
    static_assert(sizeof(EmissiveTriangleGPU) == 96,
                  "EmissiveTriangleGPU must match the shader's 96-byte stride");
    // The last entry is reserved as the always-empty list (lightCount 0) bound
    // while no reservoirs exist.
    static const UINT kMaxEmissiveTriangles = 65536;

    // Builds the power-proportional alias table (Vose) over `triangles`, whose
    // pdf field holds each triangle's unnormalised power on entry, and writes
    // the list. Called on acceleration rebuilds, which drain every frame slot
    // first, so the upload heap is written in place. An empty list disables
    // emissive lighting.
    void UploadEmissiveTriangles(std::vector<EmissiveTriangleGPU> triangles) {
        if (!emissiveTriangleBuffer) return;
        if (triangles.size() > kMaxEmissiveTriangles - 1u)
            triangles.resize(kMaxEmissiveTriangles - 1u);
        const UINT count = static_cast<UINT>(triangles.size());
        double total = 0.0;
        for (const EmissiveTriangleGPU& t : triangles) total += t.pdf;
        if (count == 0 || total <= 0.0) {
            triangles.clear();
        } else {
            std::vector<double> scaled(count);
            std::vector<UINT> underfull, overfull;
            for (UINT i = 0; i < count; ++i) {
                triangles[i].pdf = static_cast<float>(triangles[i].pdf / total);
                scaled[i] = triangles[i].pdf * static_cast<double>(count);
                (scaled[i] < 1.0 ? underfull : overfull).push_back(i);
            }
            while (!underfull.empty() && !overfull.empty()) {
                const UINT lo = underfull.back();
                underfull.pop_back();
                const UINT hi = overfull.back();
                triangles[lo].aliasProb = static_cast<float>(scaled[lo]);
                triangles[lo].alias = hi;
                scaled[hi] -= 1.0 - scaled[lo];
                if (scaled[hi] < 1.0) {
                    overfull.pop_back();
                    underfull.push_back(hi);
                }
            }
            for (UINT i : overfull) { triangles[i].aliasProb = 1.0f; triangles[i].alias = i; }
            for (UINT i : underfull) { triangles[i].aliasProb = 1.0f; triangles[i].alias = i; }
            for (EmissiveTriangleGPU& t : triangles) t.lightCount = count;
        }
        const UINT written = static_cast<UINT>(triangles.size());
        const EmissiveTriangleGPU empty{};
        void* mapped = nullptr;
        D3D12_RANGE readRange = { 0, 0 };
        if (FAILED(emissiveTriangleBuffer->Map(0, &readRange, &mapped)) || !mapped)
            return;
        auto* entries = static_cast<EmissiveTriangleGPU*>(mapped);
        if (written)
            memcpy(entries, triangles.data(), written * sizeof(EmissiveTriangleGPU));
        else
            entries[0] = empty;
        entries[kMaxEmissiveTriangles - 1u] = empty;
        emissiveTriangleBuffer->Unmap(0, nullptr);
        emissiveTriangleCount = written;
    }

    // Maximum hit-geometry entries. One per BLAS geometry, not per instance,
    // matching how DXRScene emits hit records.
    static const UINT VB_MAX_HIT_GEOMETRY = 4096;
    // Byte stride of one hit-geometry entry. Must equal both
    // sizeof(DXRScene::HitGeometryData) and the shader's HitGeometry; asserted
    // in UploadHitGeometry, where the real type is visible.
    //
    // 10 x 4 bytes: vertexOffset, indexOffset, hasIndices, legacy material ID,
    // bindless material ID, valid, fallbackColor[3], hasFallbackColor.
    static const UINT kHitGeometryStride = 40;

    // Uploads the per-geometry hit bindings produced by the acceleration
    // rebuild. Entry N must describe the same geometry as DXRScene hit record
    // N; the shader indexes both by
    // CommittedInstanceContributionToHitGroupIndex() + CommittedGeometryIndex().
    //
    // Called only on an acceleration-structure rebuild, which drains every
    // frame slot first, so writing this upload-heap buffer in place cannot race
    // an in-flight resolve.
    template <typename HitGeometryEntry>
    void UploadHitGeometry(const std::vector<HitGeometryEntry>& entries) {
        static_assert(sizeof(HitGeometryEntry) == kHitGeometryStride,
                      "Hit geometry entry must match the shader's stride");
        hitGeometryCount = 0;
        if (!hitGeometryBuffer || entries.empty()) return;
        const UINT count =
            (std::min)(static_cast<UINT>(entries.size()), VB_MAX_HIT_GEOMETRY);
        void* mapped = nullptr;
        D3D12_RANGE readRange = { 0, 0 };
        if (FAILED(hitGeometryBuffer->Map(0, &readRange, &mapped)) || !mapped)
            return;
        memcpy(mapped, entries.data(),
               static_cast<size_t>(count) * sizeof(HitGeometryEntry));
        hitGeometryBuffer->Unmap(0, nullptr);
        hitGeometryCount = count;
        // The descriptor is rebuilt with the rest of the enhanced heap; force
        // that refresh so a stale element count cannot outlive this upload.
        for (UINT i = 0; i < FRAME_COUNT; ++i)
            enhancedHeapTLASAddresses[i] = 0;
    }

    // True once real per-geometry bindings exist. The shader also checks each
    // entry's valid flag, so this is only the coarse gate.
    bool HitGeometryReady() const { return hitGeometryCount > 0; }

    // Called per frame with the current toggle state and TLAS. Rebuilding the
    // descriptors only when the TLAS moves keeps this close to free.
    void SetEnhancedVisuals(bool active, bool rtShadows, bool rayClassify,
                            float confidenceThreshold,
                            D3D12_GPU_VIRTUAL_ADDRESS tlasAddress,
                            bool rtReflections = false) {
        const bool rtReflectionsChanged =
            enhancedRTReflectionsActive != rtReflections;
        enhancedRTShadowsActive = rtShadows;
        enhancedRayClassifyActive = rayClassify;
        enhancedConfidenceThreshold = confidenceThreshold;
        enhancedRTReflectionsActive = rtReflections;
        // Without a TLAS there is nothing to trace against, so the enhanced
        // path would just be a slower way to get the same image.
        const bool wantActive = active && tlasAddress != 0;
        if (rtReflectionsChanged || wantActive != enhancedVisualsActive)
            svgfHistoryValid = false;
        // A reallocated TLAS means the scene was rebuilt (level change or a
        // large streaming step): cached surfaces may no longer exist.
        if (tlasAddress != enhancedTLASAddress) giRadianceCache.RequestClear();
        enhancedTLASAddress = tlasAddress;
        const UINT frameSlot = g_dx12.frameIndex % FRAME_COUNT;
        // The current frame slot is idle by the time its allocator is reused,
        // so only its descriptors may be rewritten. Other slots refresh when
        // they become current rather than while the GPU may still read them.
        if (!enhancedComputeDescHeaps[frameSlot] ||
            enhancedHeapTLASAddresses[frameSlot] != enhancedTLASAddress ||
            enhancedHeapRRGeneration[frameSlot] != rrGuideGeneration)
            RefreshEnhancedDescriptors(frameSlot);
        enhancedVisualsActive = wantActive;
    }

    void SetLumenGI(bool on) {
        if (on != lumenGIActive) svgfHistoryValid = false;
        lumenGIActive = on;
    }

    // Replaces the Lumen estimate, so it only takes effect while Lumen is
    // active. Until its pipelines finish compiling, the frame stays on Lumen.
    void SetRadianceCascadesGI(bool on) {
        if (on != radianceCascadesGIRequested) svgfHistoryValid = false;
        radianceCascadesGIRequested = on;
    }
    const char* RadianceCascadesStatus() const {
        return radianceCascades.Status();
    }

    // Starts empty whenever it is switched on: entries from an earlier session
    // could describe geometry and lighting that no longer exist.
    void SetLumenRadianceCache(bool on) {
        if (on != giRadianceCacheRequested) {
            svgfHistoryValid = false;
            if (on) giRadianceCache.RequestClear();
        }
        giRadianceCacheRequested = on;
    }
    const char* LumenRadianceCacheStatus() const {
        return giRadianceCache.Status();
    }

    bool VariableRateGIActive() const { return variableRateGIActive; }
    const char* VariableRateGIStatus() const { return variableRateGI.Status(); }
    UINT VariableRateGIRayBudget() const { return variableRateGI.LastRayBudget(); }
    UINT VariableRateGIMeasuredRays() const { return variableRateGI.MeasuredRays(); }
    UINT VariableRateGIMeasuredBudget() const { return variableRateGI.MeasuredBudget(); }
    bool VariableRateGIStatisticsValid() const { return variableRateGI.StatisticsValid(); }

    void SetVariableRateGI(bool on, float budget) {
        if (on != variableRateGIRequested) InvalidateTemporalHistory();
        variableRateGIRequested = on;
        variableRateGIBudget = std::clamp(budget, 0.125f, 1.0f);
    }

    void SetEmissiveCGNS(bool on) {
        if (on != emissiveCGNSRequested) InvalidateTemporalHistory();
        emissiveCGNSRequested = on;
    }

    void SetLumenReSTIR(bool on) {
        if (on != lumenReSTIRRequested) {
            svgfHistoryValid = false;
            restirHistoryValid = false;
        }
        lumenReSTIRRequested = on;
    }

    bool EnhancedVisualsReady() const { return enhancedPipelineReady; }

    // Material/texture residency, for judging whether the fixed-size binding
    // model is actually a constraint yet. MaterialTextureCount() saturates at
    // VB_MAX_MATERIAL_TEXTURES; RejectedTextureCount() is how many distinct
    // textures were turned away after that, which is the number that says how
    // much a bindless heap would buy.
    UINT MaterialTextureCount() const { return materialTextureCount; }
    UINT MaterialTextureCapacity() const { return VB_MAX_MATERIAL_TEXTURES; }
    UINT RejectedTextureCount() const {
        return static_cast<UINT>(materialTexturesRejected.size());
    }
    UINT MaterialCount() const { return materialCount; }
    UINT MaterialCapacity() const { return VB_MAX_MATERIALS; }
    // Fraction of sampled pixels routed to RT last time the statistic updated.
    // 0..1; indicative rather than exact (see UpdateRayMaskStatistic).
    float EnhancedRayFraction() const { return rayMaskFraction; }
    float EnhancedShadowRayFraction() const { return rayMaskShadowFraction; }
    float EnhancedReflectionRayFraction() const {
        return rayMaskReflectionFraction;
    }
    float EnhancedGIRayFraction() const { return rayMaskGIFraction; }

private:

    // Builds the tile-classification pass. Best-effort throughout: any failure
    // leaves tileClassifyReady false, and the split resolve falls back to two
    // full-screen dispatches, which is correct but pays the sweep cost.
    //
    // Called after the resolve pipeline so a classification failure can never
    // prevent the resolve itself from coming up.
    bool CreateTileClassifyPipeline() {
        tileClassifyReady = false;

        std::ifstream file("shaders/visbuf_tile_classify_cs.hlsl");
        if (!file.is_open()) {
            std::cerr << "Tile classify: shader missing (non-fatal; resolve "
                         "stays full-screen)\n";
            return false;
        }
        std::stringstream ss;
        ss << file.rdbuf();
        const std::string code = ss.str();

        UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
        compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
        compileFlags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

        // 0: CBV b0 (screen/tile dimensions)
        // 1: table u0..u2 (generic list, terrain list, dispatch args)
        // 2: SRV t0 (visibility buffer) as a root SRV -- one texture, no table.
        D3D12_DESCRIPTOR_RANGE uavRange = {};
        uavRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        uavRange.NumDescriptors = 3;
        uavRange.BaseShaderRegister = 0;
        uavRange.OffsetInDescriptorsFromTableStart = 0;

        D3D12_DESCRIPTOR_RANGE srvRange = {};
        srvRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRange.NumDescriptors = 1;
        srvRange.BaseShaderRegister = 0;
        srvRange.OffsetInDescriptorsFromTableStart = 3;

        D3D12_DESCRIPTOR_RANGE ranges[2] = { uavRange, srvRange };

        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor.ShaderRegister = 0;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 2;
        params[1].DescriptorTable.pDescriptorRanges = ranges;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
        rootDesc.NumParameters = 2;
        rootDesc.pParameters = params;

        ComPtr<ID3DBlob> sigBlob, sigError;
        if (FAILED(D3D12SerializeRootSignature(&rootDesc,
                D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &sigError))) {
            std::cerr << "Tile classify: root signature serialize failed\n";
            return false;
        }
        if (FAILED(g_dx12.device->CreateRootSignature(0,
                sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(),
                IID_PPV_ARGS(&tileClassifyRootSig)))) {
            std::cerr << "Tile classify: root signature creation failed\n";
            return false;
        }

        // Both entry points share the source and the root signature.
        struct EntryBuild {
            const char* entry;
            ComPtr<ID3D12PipelineState>* target;
        } builds[] = {
            { "main",      &tileClassifyPSO },
            { "ResetArgs", &tileClassifyResetPSO },
        };
        for (const EntryBuild& build : builds) {
            ComPtr<ID3DBlob> blob, errors;
            if (FAILED(ShaderCacheDX12::CompileCached(
                    code.c_str(), code.length(),
                    "shaders/visbuf_tile_classify_cs.hlsl", nullptr,
                    D3D_COMPILE_STANDARD_FILE_INCLUDE, build.entry, "cs_5_1",
                    compileFlags, 0, &blob, &errors))) {
                std::cerr << "Tile classify: " << build.entry
                          << " compile failed (non-fatal)\n";
                if (errors) {
                    std::cerr << static_cast<const char*>(
                        errors->GetBufferPointer()) << std::endl;
                }
                tileClassifyRootSig.Reset();
                return false;
            }
            D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
            desc.pRootSignature = tileClassifyRootSig.Get();
            desc.CS = { blob->GetBufferPointer(), blob->GetBufferSize() };
            if (FAILED(g_dx12.device->CreateComputePipelineState(
                    &desc, IID_PPV_ARGS(build.target->GetAddressOf())))) {
                std::cerr << "Tile classify: " << build.entry
                          << " PSO creation failed (non-fatal)\n";
                tileClassifyRootSig.Reset();
                return false;
            }
        }

        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NumDescriptors = 4;          // u0, u1, u2, t0
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(g_dx12.device->CreateDescriptorHeap(
                &heapDesc, IID_PPV_ARGS(&tileClassifyDescHeap)))) {
            std::cerr << "Tile classify: descriptor heap creation failed\n";
            tileClassifyRootSig.Reset();
            return false;
        }

        // 256-byte aligned constant buffer, persistently mapped.
        {
            D3D12_HEAP_PROPERTIES cbProps = {};
            cbProps.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC cbDesc = {};
            cbDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            cbDesc.Width = 256;
            cbDesc.Height = 1;
            cbDesc.DepthOrArraySize = 1;
            cbDesc.MipLevels = 1;
            cbDesc.Format = DXGI_FORMAT_UNKNOWN;
            cbDesc.SampleDesc.Count = 1;
            cbDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(g_dx12.device->CreateCommittedResource(
                    &cbProps, D3D12_HEAP_FLAG_NONE, &cbDesc,
                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                    IID_PPV_ARGS(&tileClassifyConstantBuffer)))) {
                std::cerr << "Tile classify: constant buffer creation failed\n";
                tileClassifyRootSig.Reset();
                return false;
            }
            D3D12_RANGE noRead = { 0, 0 };
            if (FAILED(tileClassifyConstantBuffer->Map(0, &noRead,
                    reinterpret_cast<void**>(&mappedTileClassifyConstants)))) {
                std::cerr << "Tile classify: constant buffer map failed\n";
                tileClassifyRootSig.Reset();
                return false;
            }
        }

        tileClassifyReady = true;
        std::cout << "Visibility resolve tile classification ready\n";
        // The engine reopens stdout onto its own console window, so build-time
        // status is not capturable from a redirected run. Mirror it to a file
        // the same way the visibility smoke test reports.
        std::ofstream("tile_classify.log", std::ios::trunc)
            << "pipeline ready\n";
        return true;
    }

    // Allocates the tile lists and the GPU-written dispatch args for the
    // current resolution, and writes their descriptors. Split from pipeline
    // creation because it is the only size-dependent part, so a resize rebuilds
    // just this.
    //
    // Safe to call when the pipeline failed to build: it returns immediately,
    // leaving the full-screen fallback in place.
    bool CreateTileClassifyResources() {
        if (!tileClassifyReady || !tileClassifyDescHeap) return false;

        tileClassifyTilesX = (width + 7u) / 8u;
        tileClassifyTilesY = (height + 7u) / 8u;
        const UINT tileCount = tileClassifyTilesX * tileClassifyTilesY;
        if (tileCount == 0) return false;

        genericTileListBuffer.Reset();
        terrainTileListBuffer.Reset();
        classifiedDispatchArgsBuffer.Reset();

        D3D12_HEAP_PROPERTIES defaultProps = {};
        defaultProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        auto createBuffer = [&](UINT64 bytes, ComPtr<ID3D12Resource>& target) {
            D3D12_RESOURCE_DESC desc = {};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width = bytes;
            desc.Height = 1;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = DXGI_FORMAT_UNKNOWN;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            return SUCCEEDED(g_dx12.device->CreateCommittedResource(
                &defaultProps, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                IID_PPV_ARGS(&target)));
        };

        // Worst case is every tile in both lists, which happens when terrain
        // edges cross the whole screen. Sizing for it means the append can
        // never overflow, so no bounds check is needed in the shader.
        const UINT64 listBytes = UINT64(tileCount) * sizeof(UINT);
        if (!createBuffer(listBytes, genericTileListBuffer) ||
            !createBuffer(listBytes, terrainTileListBuffer) ||
            !createBuffer(sizeof(D3D12_DISPATCH_ARGUMENTS) * 2,
                          classifiedDispatchArgsBuffer)) {
            std::cerr << "Tile classify: buffer allocation failed "
                         "(non-fatal; resolve stays full-screen)\n";
            tileClassifyReady = false;
            return false;
        }

        const UINT descSize = g_dx12.device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE handle =
            tileClassifyDescHeap->GetCPUDescriptorHandleForHeapStart();

        auto writeListUAV = [&](ID3D12Resource* resource) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
            uav.Format = DXGI_FORMAT_UNKNOWN;
            uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            uav.Buffer.NumElements = tileCount;
            uav.Buffer.StructureByteStride = sizeof(UINT);
            g_dx12.device->CreateUnorderedAccessView(resource, nullptr, &uav,
                                                     handle);
            handle.ptr += descSize;
        };
        writeListUAV(genericTileListBuffer.Get());   // u0
        writeListUAV(terrainTileListBuffer.Get());   // u1

        // u2: raw buffer, so the shader can InterlockedAdd into the two
        // ThreadGroupCountX fields at byte offsets 0 and 12.
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
            uav.Format = DXGI_FORMAT_R32_TYPELESS;
            uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            uav.Buffer.NumElements =
                (sizeof(D3D12_DISPATCH_ARGUMENTS) * 2) / 4;
            uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
            g_dx12.device->CreateUnorderedAccessView(
                classifiedDispatchArgsBuffer.Get(), nullptr, &uav, handle);
            handle.ptr += descSize;
        }

        // t0: the visibility buffer this pass reduces.
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
            srv.Format = DXGI_FORMAT_R32G32_UINT;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping =
                D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1;
            g_dx12.device->CreateShaderResourceView(visBufferRT.Get(), &srv,
                                                    handle);
        }

        return true;
    }

    bool CreateResolvePipeline() {
        // Read and compile compute shader
        if (!PrepareResolveSources()) {
            std::cerr << "Failed to open visbuf_resolve_cs.hlsl" << std::endl;
            return false;
        }
        std::string csCode = resolveBaseSource;
        const UINT compileFlags = ResolveFxcFlags();

        ComPtr<ID3DBlob> csBlob, errorBlob;
        HRESULT hr = ShaderCacheDX12::CompileCached(csCode.c_str(), csCode.length(),
            "shaders/visbuf_resolve_cs.hlsl",
            nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "cs_5_1",
            compileFlags, 0, &csBlob, &errorBlob);
        if (FAILED(hr)) {
            if (errorBlob) {
                const char* message = static_cast<const char*>(errorBlob->GetBufferPointer());
                std::cerr << "VB CS error: " << message << std::endl;
                std::ofstream log("visibility_buffer_shader_error.log", std::ios::trunc);
                log.write(message, static_cast<std::streamsize>(errorBlob->GetBufferSize()));
            }
            return false;
        }

        // Root signature for compute resolve:
        // 0: CBV (b0) - FrameConstants
        // 1: Descriptor table - SRVs (t0..t5) + UAV (u0) + CBVs (b1, b2)
        //
        // We'll put everything in a single descriptor table for simplicity.
        // Layout in the heap:
        //   [0] t0 - visBuffer SRV
        //   [1] t1 - depthBuffer SRV
        //   [2] t2 - (unused/shadow placeholder)
        //   [3] t3 - drawCalls SRV
        //   [4] t4 - vertices SRV
        //   [5] t5 - indices SRV
        //   [6] t6 - clustered light lists
        //   [7] u0 - output UAV
        //   [8] b1 - light buffer CBV
        //   [9] b2 - point lights CBV

        D3D12_DESCRIPTOR_RANGE ranges[7] = {};
        // SRVs t0..t78: frame/geometry, materials, IBL, DDGI, sparse lookup.
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 79;
        ranges[0].BaseShaderRegister = 0;
        ranges[0].RegisterSpace = 0;
        ranges[0].OffsetInDescriptorsFromTableStart = 0;

        // UAVs u0..u2 (HDR + motion vectors + world normal/roughness)
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 3;
        ranges[1].BaseShaderRegister = 0;
        ranges[1].RegisterSpace = 0;
        ranges[1].OffsetInDescriptorsFromTableStart = 79;

        // CBVs b1..b4 (lights, point lights, sky SH, DDGI)
        ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
        ranges[2].NumDescriptors = 4;
        ranges[2].BaseShaderRegister = 1;
        ranges[2].RegisterSpace = 0;
        ranges[2].OffsetInDescriptorsFromTableStart = 82;

        // Previous-frame bent-normal GTAO history. Kept outside t0..t78 so the
        // established material/IBL layout and every existing heap offset stay
        // unchanged when the toggle is off.
        ranges[3].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[3].NumDescriptors = 1;
        ranges[3].BaseShaderRegister = 86;
        ranges[3].RegisterSpace = 0;
        ranges[3].OffsetInDescriptorsFromTableStart = 86;

        // Terrain triplanar layer arrays (t87..t89). Declared in the root
        // signature unconditionally so the terrain-enabled resolve PSO can share
        // this root signature; the default resolve shader never declares those
        // registers, and an unreferenced range costs nothing at dispatch.
        ranges[4].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[4].NumDescriptors = 3;
        ranges[4].BaseShaderRegister = 87;
        ranges[4].RegisterSpace = 0;
        ranges[4].OffsetInDescriptorsFromTableStart = 87;

        // [90] t91 terrain splatmap. Appended after t87..t89 so no existing
        // table offset moves. t90 is a root SRV and occupies no heap slot.
        ranges[5].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[5].NumDescriptors = 1;
        ranges[5].BaseShaderRegister = 91;
        ranges[5].RegisterSpace = 0;
        ranges[5].OffsetInDescriptorsFromTableStart = 90;

        // [91] t92 spot shadow atlas. Appended again for the same reason.
        ranges[6].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[6].NumDescriptors = 1;
        ranges[6].BaseShaderRegister = 92;
        ranges[6].RegisterSpace = 0;
        ranges[6].OffsetInDescriptorsFromTableStart = kSpotShadowAtlasSlot;

        D3D12_ROOT_PARAMETER resolveParams[3] = {};

        // b0 - frame constants (root CBV)
        resolveParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        resolveParams[0].Descriptor.ShaderRegister = 0;
        resolveParams[0].Descriptor.RegisterSpace = 0;
        resolveParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        // Descriptor table
        resolveParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        resolveParams[1].DescriptorTable.NumDescriptorRanges = _countof(ranges);
        resolveParams[1].DescriptorTable.pDescriptorRanges = ranges;
        resolveParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        // t90: this half's classified tile list. A root SRV rather than a
        // table entry because all four resolve tiers pack their tables at fixed
        // offsets -- inserting a range would shift every offset after it,
        // terrain's t87..t89 included, on every tier at once.
        //
        // Declared on all four signatures even though only the classified PSO
        // variants read it. An unread root SRV costs nothing and keeps one
        // signature per tier, so the classified and full-screen PSOs remain
        // interchangeable at dispatch time.
        resolveParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        resolveParams[2].Descriptor.ShaderRegister = 90;
        resolveParams[2].Descriptor.RegisterSpace = 0;
        resolveParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        // Static samplers
        D3D12_STATIC_SAMPLER_DESC staticSamplers[3] = {};

        // Regular sampler s0
        staticSamplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        staticSamplers[0].MipLODBias = 0.0f;
        staticSamplers[0].MinLOD = 0.0f;
        staticSamplers[0].MaxLOD = D3D12_FLOAT32_MAX;
        staticSamplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        staticSamplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        staticSamplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        staticSamplers[0].ShaderRegister = 0;
        staticSamplers[0].RegisterSpace = 0;
        staticSamplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        // Shadow comparison sampler s1
        staticSamplers[1].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
        staticSamplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        staticSamplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        staticSamplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        staticSamplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        staticSamplers[1].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
        staticSamplers[1].ShaderRegister = 1;
        staticSamplers[1].RegisterSpace = 0;
        staticSamplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        // Clamp sampler s2, for the terrain splatmap. s0 wraps, which would
        // repeat painted weights across the map edge at the coastline; the
        // splatmap covers the island exactly once and must clamp.
        staticSamplers[2].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        staticSamplers[2].MipLODBias = 0.0f;
        staticSamplers[2].MinLOD = 0.0f;
        staticSamplers[2].MaxLOD = D3D12_FLOAT32_MAX;
        staticSamplers[2].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        staticSamplers[2].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        staticSamplers[2].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        staticSamplers[2].ShaderRegister = 2;
        staticSamplers[2].RegisterSpace = 0;
        staticSamplers[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC resolveRootSigDesc = {};
        resolveRootSigDesc.NumParameters = 3;
        resolveRootSigDesc.pParameters = resolveParams;
        resolveRootSigDesc.NumStaticSamplers = _countof(staticSamplers);
        resolveRootSigDesc.pStaticSamplers = staticSamplers;

        ComPtr<ID3DBlob> sigBlob;
        errorBlob.Reset();
        hr = D3D12SerializeRootSignature(&resolveRootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errorBlob);
        if (FAILED(hr)) {
            if (errorBlob) std::cerr << "VB resolve root sig error: " << (char*)errorBlob->GetBufferPointer() << std::endl;
            return false;
        }

        hr = g_dx12.device->CreateRootSignature(0, sigBlob->GetBufferPointer(),
            sigBlob->GetBufferSize(), IID_PPV_ARGS(&resolveRootSig));
        if (FAILED(hr)) return false;

        // Compute PSO
        D3D12_COMPUTE_PIPELINE_STATE_DESC cpsoDesc = {};
        cpsoDesc.pRootSignature = resolveRootSig.Get();
        cpsoDesc.CS = { csBlob->GetBufferPointer(), csBlob->GetBufferSize() };

        hr = g_dx12.device->CreateComputePipelineState(&cpsoDesc, IID_PPV_ARGS(&resolvePSO));
        if (FAILED(hr)) {
            std::cerr << "Failed to create VB resolve compute PSO" << std::endl;
            return false;
        }

        // Preserve the default FXC blob; optional variants carry the appended
        // page table and branch only when the current frame publishes pages.
        csCode = "#define SGE_VIRTUAL_SHADOWS 1\n" + csCode;
        ComPtr<ID3DBlob> vsmBlob, vsmErrors;
        if (SUCCEEDED(ShaderCacheDX12::CompileCached(csCode.c_str(), csCode.size(),
                "shaders/visbuf_resolve_cs.hlsl", nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
                "main", "cs_5_1", compileFlags, 0, &vsmBlob, &vsmErrors))) {
            auto vsmDesc = cpsoDesc;
            vsmDesc.CS = {vsmBlob->GetBufferPointer(), vsmBlob->GetBufferSize()};
            if (FAILED(g_dx12.device->CreateComputePipelineState(&vsmDesc,
                    IID_PPV_ARGS(&virtualShadowResolvePSO)))) return false;
        } else {
            if (vsmErrors) std::cerr << (const char*)vsmErrors->GetBufferPointer();
            return false;
        }

        // ---- Enhanced (SM 6.5) resolve variant ----
        //
        // Built alongside the FXC PSO above rather than replacing it. The
        // default frame keeps running the exact shader it always has, compiled
        // by the same compiler; the enhanced variant is a second PSO selected
        // at dispatch time. That containment is deliberate: this shader runs
        // for every pixel, so a DXC codegen difference must not be able to
        // regress the default path.
        //
        // Failure here is non-fatal and simply leaves enhanced visuals
        // unavailable (no DXC, no Tier 1.1, or a compile error).
        {
            BootTimer::Scope step("VB: enhanced (RT) resolve variants");
            CreateEnhancedResolvePipeline(csCode, ranges, staticSamplers);
        }
        // The SVGF à-trous pipeline is built on first use (see the dispatch):
        // Ray Reconstruction replaces it, so an RR session never compiles it.

        // ---- Bindless (SM 6.6) resolve variants ----
        //
        // Two more PSOs, lit and enhanced, compiled from the same source with
        // SGE_BINDLESS_MATERIALS. Four selections exist in total and the
        // dispatch picks between them; none of them can perturb the FXC PSO
        // above, which is what keeps "bindless off" identical to before.
        //
        // Gated on the adapter actually reporting SM 6.6 and Tier 3: without
        // both, ResourceDescriptorHeap[] is not merely slow but invalid.
        if (bindlessHeap && bindlessHeap->Supported()) {
            BootTimer::Scope step("VB: bindless resolve variants");
            CreateBindlessResolvePipeline(csCode, ranges, staticSamplers,
                                          resolveParams);
            CreateEnhancedResolvePipeline(csCode, ranges, staticSamplers, true);
        }

        // Terrain set of the tier the settings predict. The other tiers'
        // sets are built in the background on first use (see
        // PumpDeferredResolvePipelines); with the boot compile queue these
        // are normally cache hits plus driver PSO builds.
        {
            const int tier = BootTerrainTier();
            BootTimer::Scope step(std::string("VB: terrain resolve variants (") +
                                  ResolveTierName(tier) + ")");
            TerrainSetBuild build = BuildTerrainSet(
                tier, ResolveTierSource(tier), ResolveTierRootSig(tier),
                g_dx12.device, false);
            InstallTerrainSet(tier, build);
            terrainSetRequested[tier] = true;
        }

        // Create indirect dispatch command signature + args buffer
        {
            D3D12_INDIRECT_ARGUMENT_DESC arg = {};
            arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;

            D3D12_COMMAND_SIGNATURE_DESC sigDesc = {};
            sigDesc.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
            sigDesc.NumArgumentDescs = 1;
            sigDesc.pArgumentDescs = &arg;

            hr = g_dx12.device->CreateCommandSignature(&sigDesc, nullptr, IID_PPV_ARGS(&resolveDispatchSignature));
            if (FAILED(hr)) return false;

            D3D12_HEAP_PROPERTIES heapProps = {};
            heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

            D3D12_RESOURCE_DESC bufDesc = {};
            bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bufDesc.Width = sizeof(D3D12_DISPATCH_ARGUMENTS);
            bufDesc.Height = 1;
            bufDesc.DepthOrArraySize = 1;
            bufDesc.MipLevels = 1;
            bufDesc.Format = DXGI_FORMAT_UNKNOWN;
            bufDesc.SampleDesc.Count = 1;
            bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

            hr = g_dx12.device->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &bufDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&resolveDispatchArgsBuffer));
            if (FAILED(hr)) return false;

            D3D12_RANGE rr = { 0, 0 };
            hr = resolveDispatchArgsBuffer->Map(0, &rr, reinterpret_cast<void**>(&mappedResolveDispatchArgs));
            if (FAILED(hr)) return false;
        }

        std::cout << "Visibility buffer resolve pipeline created" << std::endl;
        return true;
    }

    void UpdateComputeDescriptors() {
        UINT descSize = g_dx12.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = computeDescHeap->GetCPUDescriptorHandleForHeapStart();

        // [0] t0 - visBuffer SRV (R32G32_UINT)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = DXGI_FORMAT_R32G32_UINT;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Texture2D.MipLevels = 1;
            g_dx12.device->CreateShaderResourceView(visBufferRT.Get(), &srvDesc, cpuHandle);
            cpuHandle.ptr += descSize;
        }

        // [1] t1 - depthBuffer SRV (R32_FLOAT from D32 typeless)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Texture2D.MipLevels = 1;
            g_dx12.device->CreateShaderResourceView(ActiveDepthBuffer(), &srvDesc, cpuHandle);
            cpuHandle.ptr += descSize;
        }

        // [2] t2 - shadow map placeholder (null SRV)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Texture2DArray.MipLevels = 1;
            srvDesc.Texture2DArray.ArraySize = SHADOW_CASCADE_COUNT;
            g_dx12.device->CreateShaderResourceView(nullptr, &srvDesc, cpuHandle);
            cpuHandle.ptr += descSize;
        }

        // [3] t3 - drawCalls SRV (structured buffer)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = DXGI_FORMAT_UNKNOWN;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Buffer.FirstElement = 0;
            srvDesc.Buffer.NumElements = VB_MAX_DRAW_CALLS;
            srvDesc.Buffer.StructureByteStride = sizeof(VBDrawCallData);
            g_dx12.device->CreateShaderResourceView(drawCallBuffer.Get(), &srvDesc, cpuHandle);
            cpuHandle.ptr += descSize;
        }

        // [4] t4 - vertices SRV (structured buffer)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = DXGI_FORMAT_UNKNOWN;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Buffer.FirstElement = 0;
            srvDesc.Buffer.NumElements = geometryVertexCapacity;
            srvDesc.Buffer.StructureByteStride = sizeof(VBPackedVertex);
            g_dx12.device->CreateShaderResourceView(vertexDataBuffer.Get(), &srvDesc, cpuHandle);
            cpuHandle.ptr += descSize;
        }

        // [5] t5 - indices SRV (structured buffer)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = DXGI_FORMAT_UNKNOWN;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Buffer.FirstElement = 0;
            srvDesc.Buffer.NumElements = geometryIndexCapacity;
            srvDesc.Buffer.StructureByteStride = sizeof(UINT);
            g_dx12.device->CreateShaderResourceView(indexDataBuffer.Get(), &srvDesc, cpuHandle);
            cpuHandle.ptr += descSize;
        }

        // [6] t6 - clustered light lists
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = DXGI_FORMAT_UNKNOWN;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Buffer.FirstElement = 0;
            srvDesc.Buffer.NumElements = VB_CLUSTER_COUNT;
            srvDesc.Buffer.StructureByteStride = sizeof(VBClusterData);
            g_dx12.device->CreateShaderResourceView(clusterDataBuffer.Get(), &srvDesc, cpuHandle);
            cpuHandle.ptr += descSize;
        }

        // [7] t7 - persistent material table
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = DXGI_FORMAT_UNKNOWN;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Buffer.NumElements = VB_MAX_MATERIALS;
            srvDesc.Buffer.StructureByteStride = sizeof(VBMaterialData);
            g_dx12.device->CreateShaderResourceView(
                materialDataBuffer.Get(), &srvDesc, cpuHandle);
            cpuHandle.ptr += descSize;
        }

        // [8..71] t8..t71 - material texture array, initialized to null.
        for (UINT i = 0; i < VB_MAX_MATERIAL_TEXTURES; ++i) {
            D3D12_SHADER_RESOURCE_VIEW_DESC nullSrv = {};
            nullSrv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            nullSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            nullSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            nullSrv.Texture2D.MipLevels = 1;
            g_dx12.device->CreateShaderResourceView(nullptr, &nullSrv, cpuHandle);
            cpuHandle.ptr += descSize;
        }

        // [72] t72 - HDR environment map for specular IBL.
        // [73] t73 - split-sum GGX BRDF integration LUT.
        // Whatever UpdateEnvironmentMap last bound (null before then). Resize
        // rebuilds this heap, and writing null here dropped both until the next
        // level load: the zero LUT made the multiscatter term divide by zero,
        // so every sun highlight blew out after a DLSS/RR resolution change.
        WriteEnvironmentDescriptors(cpuHandle);
        cpuHandle.ptr += 2u * descSize;

        // [74] t74 - DDGI irradiance atlas.
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
            srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1;
            g_dx12.device->CreateShaderResourceView(nullptr, &srv, cpuHandle);
            cpuHandle.ptr += descSize;
        }

        // [75] t75 - DDGI visibility atlas.
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
            srv.Format = DXGI_FORMAT_R16G16_FLOAT;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1;
            g_dx12.device->CreateShaderResourceView(nullptr, &srv, cpuHandle);
            cpuHandle.ptr += descSize;
        }

        // [76..78] t76..t78 - sparse probes, hash cells, flattened indices.
        {
            const UINT strides[3] = {
                sizeof(DXRProbeRecord), sizeof(DXRProbeGridCell), sizeof(UINT)
            };
            for (UINT i = 0; i < 3; ++i) {
                D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
                srv.Format = DXGI_FORMAT_UNKNOWN;
                srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                srv.Shader4ComponentMapping =
                    D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                srv.Buffer.NumElements = 1;
                srv.Buffer.StructureByteStride = strides[i];
                g_dx12.device->CreateShaderResourceView(nullptr, &srv, cpuHandle);
                cpuHandle.ptr += descSize;
            }
        }

        // [79] u0 - linear HDR output UAV
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
            uavDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            g_dx12.device->CreateUnorderedAccessView(outputTexture.Get(), nullptr, &uavDesc, cpuHandle);
            cpuHandle.ptr += descSize;
        }

        // [80] u1 - screen-space motion vectors
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
            uavDesc.Format = DXGI_FORMAT_R16G16_FLOAT;
            uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            g_dx12.device->CreateUnorderedAccessView(
                motionTexture.Get(), nullptr, &uavDesc, cpuHandle);
            cpuHandle.ptr += descSize;
        }

        // [81] u2 - world normal.xyz and final roughness
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
            uavDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            g_dx12.device->CreateUnorderedAccessView(
                normalRoughnessTexture.Get(), nullptr, &uavDesc, cpuHandle);
            cpuHandle.ptr += descSize;
        }

        // [82] b1 - light buffer CBV
        // [83] b2 - point lights CBV
        // [84] b3 - sky SH CBV
        // [85] b4 - DDGI CBV
        // Null CBVs are replaced by UpdateLightDescriptors before resolve.
        for (UINT slot = 82; slot < 86; ++slot) {
            auto handle = computeDescHeap->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += static_cast<SIZE_T>(descSize) * slot;
            g_dx12.device->CreateConstantBufferView(nullptr, handle);
        }
        WriteSpotShadowAtlasDescriptor(computeDescHeap.Get(), kSpotShadowAtlasSlot);

        // [86] t86 - bent-normal GTAO history. The per-frame resolve heap
        // overwrites this null descriptor only while valid history is active.
        WriteBentNormalHistoryDescriptor(computeDescHeap.Get(), 86, nullptr);

        // [87..89] t87..t89 - terrain triplanar layer arrays, plus [90] t91
        // splatmap. The root signature declares these ranges unconditionally,
        // so the slots must hold valid descriptors from the start; terrain
        // overwrites them with the real arrays on its first frame.
        WriteTerrainDescriptors(computeDescHeap.Get(), 87);
    }

    bool CreateExposurePipeline() {
        std::ifstream file("shaders/visbuf_exposure_cs.hlsl");
        if (!file.is_open()) return false;
        std::stringstream stream;
        stream << file.rdbuf();
        const std::string source = stream.str();

        D3D12_DESCRIPTOR_RANGE ranges[2] = {};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 1;
        ranges[0].BaseShaderRegister = 0;
        ranges[0].OffsetInDescriptorsFromTableStart = 0;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 1;
        ranges[1].BaseShaderRegister = 0;
        ranges[1].OffsetInDescriptorsFromTableStart = 1;
        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor.ShaderRegister = 0;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 2;
        params[1].DescriptorTable.pDescriptorRanges = ranges;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC root = {};
        root.NumParameters = 2;
        root.pParameters = params;
        ComPtr<ID3DBlob> rootBlob, errors;
        HRESULT hr = D3D12SerializeRootSignature(&root,
            D3D_ROOT_SIGNATURE_VERSION_1, &rootBlob, &errors);
        if (FAILED(hr)) return false;
        hr = g_dx12.device->CreateRootSignature(0, rootBlob->GetBufferPointer(),
            rootBlob->GetBufferSize(), IID_PPV_ARGS(&exposureRootSig));
        if (FAILED(hr)) return false;

        auto createPSO = [&](const char* entry,
                             ComPtr<ID3D12PipelineState>& result) -> bool {
            ComPtr<ID3DBlob> shader;
            errors.Reset();
            HRESULT compile = ShaderCacheDX12::CompileCached(source.data(), source.size(),
                "visbuf_exposure_cs.hlsl", nullptr,
                D3D_COMPILE_STANDARD_FILE_INCLUDE, entry, "cs_5_0",
                D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
                0, &shader, &errors);
            if (FAILED(compile)) {
                if (errors) std::cerr << "VB exposure CS error: "
                    << (char*)errors->GetBufferPointer() << std::endl;
                return false;
            }
            D3D12_COMPUTE_PIPELINE_STATE_DESC pso = {};
            pso.pRootSignature = exposureRootSig.Get();
            pso.CS = { shader->GetBufferPointer(), shader->GetBufferSize() };
            return SUCCEEDED(g_dx12.device->CreateComputePipelineState(
                &pso, IID_PPV_ARGS(&result)));
        };
        if (!createPSO("Reset", exposureResetPSO) ||
            !createPSO("Accumulate", exposureAccumulatePSO) ||
            !createPSO("Finalize", exposureFinalizePSO)) return false;

        D3D12_DESCRIPTOR_HEAP_DESC heap = {};
        heap.NumDescriptors = 2;
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = g_dx12.device->CreateDescriptorHeap(&heap,
            IID_PPV_ARGS(&exposureDescHeap));
        if (FAILED(hr)) return false;
        UpdateExposureDescriptors();
        return true;
    }

    void UpdateExposureDescriptors() {
        if (!exposureDescHeap) return;
        UINT size = g_dx12.cbvSrvUavDescriptorSize;
        D3D12_CPU_DESCRIPTOR_HANDLE handle =
            exposureDescHeap->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC hdr = {};
        hdr.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        hdr.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        hdr.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        hdr.Texture2D.MipLevels = 1;
        g_dx12.device->CreateShaderResourceView(outputTexture.Get(), &hdr, handle);
        handle.ptr += size;
        D3D12_UNORDERED_ACCESS_VIEW_DESC state = {};
        state.Format = DXGI_FORMAT_R32_TYPELESS;
        state.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        state.Buffer.NumElements = 3;
        state.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        g_dx12.device->CreateUnorderedAccessView(
            exposureState.Get(), nullptr, &state, handle);
    }

    // Root 32-bit constants rather than a constant buffer, matching the bloom
    // pass: the flare parameters change every frame and are far cheaper to push
    // inline than to version across frames in an upload heap.
    //
    // Layout must match FlareConstants in visbuf_flare_cs.hlsl exactly, field
    // for field. HLSL packs a float3 onto its own 16-byte boundary, so sunTint
    // is preceded by the two pad floats that fill out the register holding
    // sunPresence and sunEnergy.
    struct FlareDispatchConstants {
        UINT flareWidth;
        UINT flareHeight;
        float sunU;
        float sunV;
        float sunPresence;
        float sunEnergy;
        float pad0;
        float pad1;
        float sunTint[3];
        float ghostIntensity;
        float ghostDispersion;
        float haloIntensity;
        float starburstIntensity;
        float streakIntensity;
        float streakLength;
        float bloomMaxMip;
        UINT blurDirection;
        float aspect;
    };

    static constexpr UINT kFlareDescriptorsPerPass = 4;
    static constexpr UINT kFlarePassCount = 4;

    bool CreateFlarePipeline() {
        flarePipelineReady = false;
        std::ifstream csFile("shaders/visbuf_flare_cs.hlsl");
        if (!csFile.is_open()) {
            std::cerr << "VB flare: shaders/visbuf_flare_cs.hlsl not found\n";
            return false;
        }
        std::stringstream stream;
        stream << csFile.rdbuf();
        const std::string source = stream.str();

        const UINT flags = D3DCOMPILE_ENABLE_STRICTNESS |
                           D3DCOMPILE_OPTIMIZATION_LEVEL3;
        ComPtr<ID3DBlob> feature, streak, blur, errors;
        auto compile = [&](const char* entry,
                           ComPtr<ID3DBlob>& blob) -> bool {
            errors.Reset();
            HRESULT hr = ShaderCacheDX12::CompileCached(
                source.data(), source.size(), "shaders/visbuf_flare_cs.hlsl",
                nullptr, nullptr, entry, "cs_5_0", flags, 0, &blob, &errors);
            if (FAILED(hr)) {
                std::cerr << "VB flare " << entry << " error: "
                    << (errors ? (const char*)errors->GetBufferPointer()
                               : "unknown") << std::endl;
                return false;
            }
            return true;
        };
        if (!compile("FeatureGenCS", feature)) return false;
        if (!compile("StreakCS", streak)) return false;
        if (!compile("BlurCS", blur)) return false;

        D3D12_DESCRIPTOR_RANGE ranges[2] = {};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 3;   // bloom, ghost profile, previous flare
        ranges[0].BaseShaderRegister = 0;
        ranges[0].OffsetInDescriptorsFromTableStart = 0;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 1;
        ranges[1].BaseShaderRegister = 0;
        ranges[1].OffsetInDescriptorsFromTableStart = 3;
        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.ShaderRegister = 0;
        params[0].Constants.Num32BitValues =
            sizeof(FlareDispatchConstants) / sizeof(UINT);
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 2;
        params[1].DescriptorTable.pDescriptorRanges = ranges;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_STATIC_SAMPLER_DESC sampler = {};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW =
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.ShaderRegister = 0;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        sampler.MinLOD = 0.0f;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        D3D12_ROOT_SIGNATURE_DESC root = {};
        root.NumParameters = 2;
        root.pParameters = params;
        root.NumStaticSamplers = 1;
        root.pStaticSamplers = &sampler;
        ComPtr<ID3DBlob> rootBlob;
        errors.Reset();
        HRESULT hr = D3D12SerializeRootSignature(
            &root, D3D_ROOT_SIGNATURE_VERSION_1, &rootBlob, &errors);
        if (FAILED(hr)) return false;
        hr = g_dx12.device->CreateRootSignature(
            0, rootBlob->GetBufferPointer(), rootBlob->GetBufferSize(),
            IID_PPV_ARGS(&flareRootSig));
        if (FAILED(hr)) return false;

        D3D12_COMPUTE_PIPELINE_STATE_DESC pso = {};
        pso.pRootSignature = flareRootSig.Get();
        pso.CS = { feature->GetBufferPointer(), feature->GetBufferSize() };
        if (FAILED(g_dx12.device->CreateComputePipelineState(
                &pso, IID_PPV_ARGS(&flareFeaturePSO))))
            return false;
        pso.CS = { streak->GetBufferPointer(), streak->GetBufferSize() };
        if (FAILED(g_dx12.device->CreateComputePipelineState(
                &pso, IID_PPV_ARGS(&flareStreakPSO))))
            return false;
        pso.CS = { blur->GetBufferPointer(), blur->GetBufferSize() };
        if (FAILED(g_dx12.device->CreateComputePipelineState(
                &pso, IID_PPV_ARGS(&flareBlurPSO))))
            return false;

        // Four dispatches, each needing its own SRV/UAV set because the
        // ping-pong swaps which texture is read and which is written.
        D3D12_DESCRIPTOR_HEAP_DESC heap = {};
        heap.NumDescriptors = kFlareDescriptorsPerPass * kFlarePassCount;
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(g_dx12.device->CreateDescriptorHeap(
                &heap, IID_PPV_ARGS(&flareDescHeap))))
            return false;
        UpdateFlareDescriptors();
        flarePipelineReady = true;
        const D3D_SHADER_MACRO qualityDefines[] = {
            { "SGE_HIGH_QUALITY_LENS", "1" }, { nullptr, nullptr }
        };
        auto createQuality = [&](const char* entry,
                                 ComPtr<ID3D12PipelineState>& pipeline) {
            ComPtr<ID3DBlob> blob;
            HRESULT result = ShaderCacheDX12::CompileCached(
                source.data(), source.size(), "shaders/visbuf_flare_cs.hlsl",
                qualityDefines, nullptr, entry, "cs_5_0", flags, 0, &blob, &errors);
            if (FAILED(result)) {
                if (errors) std::cerr << "VB quality lens " << entry << ": "
                    << (const char*)errors->GetBufferPointer() << std::endl;
                return false;
            }
            pso.CS = { blob->GetBufferPointer(), blob->GetBufferSize() };
            return SUCCEEDED(g_dx12.device->CreateComputePipelineState(
                &pso, IID_PPV_ARGS(&pipeline)));
        };
        if (!createQuality("FeatureGenCS", flareQualityFeaturePSO) ||
            !createQuality("StreakCS", flareQualityStreakPSO) ||
            !createQuality("BlurCS", flareQualityBlurPSO))
            std::cerr << "High quality lens unavailable; keeping the established lens pass\n";
        return true;
    }

    // One table per dispatch: {bloom, ghost profile, source flare} + {dest}.
    // Passes alternate between the two flare targets, so pass N reads what pass
    // N-1 wrote.
    void UpdateFlareDescriptors() {
        if (!flareDescHeap || !flareTexture || !flareScratch ||
            !bloomTexture || !lensGhostTexture)
            return;
        const UINT descriptorSize =
            g_dx12.device->GetDescriptorHandleIncrementSize(
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE base =
            flareDescHeap->GetCPUDescriptorHandleForHeapStart();

        for (UINT pass = 0; pass < kFlarePassCount; ++pass) {
            D3D12_CPU_DESCRIPTOR_HANDLE handle = base;
            handle.ptr += (SIZE_T)descriptorSize * pass *
                          kFlareDescriptorsPerPass;
            // Even passes write flareTexture and read flareScratch; odd passes
            // do the reverse. FeatureGenCS (pass 0) ignores its source SRV but
            // still needs one bound to satisfy the root signature.
            ID3D12Resource* source =
                (pass % 2u == 0u) ? flareScratch.Get() : flareTexture.Get();
            ID3D12Resource* destination =
                (pass % 2u == 0u) ? flareTexture.Get() : flareScratch.Get();

            D3D12_SHADER_RESOURCE_VIEW_DESC bloomSrv = {};
            bloomSrv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            bloomSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            bloomSrv.Shader4ComponentMapping =
                D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            bloomSrv.Texture2D.MostDetailedMip = 0;
            bloomSrv.Texture2D.MipLevels = (std::max)(1u, bloomMipCount);
            g_dx12.device->CreateShaderResourceView(
                bloomTexture.Get(), &bloomSrv, handle);
            handle.ptr += descriptorSize;

            D3D12_SHADER_RESOURCE_VIEW_DESC profileSrv = {};
            profileSrv.Format = DXGI_FORMAT_R8_UNORM;
            profileSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            profileSrv.Shader4ComponentMapping =
                D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            profileSrv.Texture2D.MipLevels = 1;
            g_dx12.device->CreateShaderResourceView(
                lensGhostTexture.Get(), &profileSrv, handle);
            handle.ptr += descriptorSize;

            D3D12_SHADER_RESOURCE_VIEW_DESC flareSrv = {};
            flareSrv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            flareSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            flareSrv.Shader4ComponentMapping =
                D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            flareSrv.Texture2D.MipLevels = 1;
            g_dx12.device->CreateShaderResourceView(source, &flareSrv, handle);
            handle.ptr += descriptorSize;

            D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
            uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            g_dx12.device->CreateUnorderedAccessView(
                destination, nullptr, &uav, handle);
        }
    }

    bool CreateBloomPipeline() {
        std::ifstream csFile("shaders/visbuf_bloom_cs.hlsl");
        if (!csFile.is_open()) return false;
        std::stringstream stream;
        stream << csFile.rdbuf();
        const std::string source = stream.str();
        const UINT flags = D3DCOMPILE_ENABLE_STRICTNESS |
                           D3DCOMPILE_OPTIMIZATION_LEVEL3;
        ComPtr<ID3DBlob> downsample, upsample, errors;
        HRESULT hr = ShaderCacheDX12::CompileCached(
            source.data(), source.size(), "shaders/visbuf_bloom_cs.hlsl",
            nullptr, nullptr, "Downsample", "cs_5_0", flags, 0,
            &downsample, &errors);
        if (FAILED(hr)) {
            if (errors) std::cerr << "VB bloom downsample CS error: "
                << (char*)errors->GetBufferPointer() << std::endl;
            return false;
        }
        errors.Reset();
        hr = ShaderCacheDX12::CompileCached(
            source.data(), source.size(), "shaders/visbuf_bloom_cs.hlsl",
            nullptr, nullptr, "Upsample", "cs_5_0", flags, 0,
            &upsample, &errors);
        if (FAILED(hr)) {
            if (errors) std::cerr << "VB bloom upsample CS error: "
                << (char*)errors->GetBufferPointer() << std::endl;
            return false;
        }

        D3D12_DESCRIPTOR_RANGE ranges[2] = {};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 1;
        ranges[0].BaseShaderRegister = 0;
        ranges[0].OffsetInDescriptorsFromTableStart = 0;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 1;
        ranges[1].BaseShaderRegister = 0;
        ranges[1].OffsetInDescriptorsFromTableStart = 1;
        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.ShaderRegister = 0;
        params[0].Constants.Num32BitValues = 8;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 2;
        params[1].DescriptorTable.pDescriptorRanges = ranges;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_STATIC_SAMPLER_DESC sampler = {};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW =
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.ShaderRegister = 0;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        sampler.MinLOD = 0.0f;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        D3D12_ROOT_SIGNATURE_DESC root = {};
        root.NumParameters = 2;
        root.pParameters = params;
        root.NumStaticSamplers = 1;
        root.pStaticSamplers = &sampler;
        ComPtr<ID3DBlob> rootBlob;
        hr = D3D12SerializeRootSignature(
            &root, D3D_ROOT_SIGNATURE_VERSION_1, &rootBlob, &errors);
        if (FAILED(hr)) return false;
        hr = g_dx12.device->CreateRootSignature(
            0, rootBlob->GetBufferPointer(), rootBlob->GetBufferSize(),
            IID_PPV_ARGS(&bloomRootSig));
        if (FAILED(hr)) return false;

        D3D12_COMPUTE_PIPELINE_STATE_DESC pso = {};
        pso.pRootSignature = bloomRootSig.Get();
        pso.CS = {
            downsample->GetBufferPointer(), downsample->GetBufferSize()
        };
        hr = g_dx12.device->CreateComputePipelineState(
            &pso, IID_PPV_ARGS(&bloomDownsamplePSO));
        if (FAILED(hr)) return false;
        pso.CS = {
            upsample->GetBufferPointer(), upsample->GetBufferSize()
        };
        hr = g_dx12.device->CreateComputePipelineState(
            &pso, IID_PPV_ARGS(&bloomUpsamplePSO));
        if (FAILED(hr)) return false;

        D3D12_DESCRIPTOR_HEAP_DESC heap = {};
        // Down + up passes, plus kBloomUpscaledSourcePass.
        heap.NumDescriptors = VB_BLOOM_MAX_MIPS * 2u * 2u;
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = g_dx12.device->CreateDescriptorHeap(
            &heap, IID_PPV_ARGS(&bloomDescHeap));
        if (FAILED(hr)) return false;
        UpdateBloomDescriptors();
        return true;
    }

    void UpdateBloomDescriptors() {
        if (!bloomDescHeap || !bloomTexture || !outputTexture) return;
        const UINT descriptorSize =
            g_dx12.device->GetDescriptorHandleIncrementSize(
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto writePass = [&](UINT pass, ID3D12Resource* source,
                             UINT sourceMip, UINT destinationMip) {
            D3D12_CPU_DESCRIPTOR_HANDLE handle =
                bloomDescHeap->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += static_cast<SIZE_T>(descriptorSize) * pass * 2u;
            D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
            srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping =
                D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MostDetailedMip = sourceMip;
            srv.Texture2D.MipLevels = 1;
            g_dx12.device->CreateShaderResourceView(source, &srv, handle);
            handle.ptr += descriptorSize;
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
            uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            uav.Texture2D.MipSlice = destinationMip;
            g_dx12.device->CreateUnorderedAccessView(
                bloomTexture.Get(), nullptr, &uav, handle);
        };

        for (UINT mip = 0; mip < bloomMipCount; ++mip) {
            writePass(mip,
                mip == 0 ? outputTexture.Get() : bloomTexture.Get(),
                mip == 0 ? 0u : mip - 1u, mip);
        }
        for (int mip = static_cast<int>(bloomMipCount) - 2;
             mip >= 0; --mip) {
            const UINT pass = bloomMipCount +
                (bloomMipCount - 2u - static_cast<UINT>(mip));
            writePass(pass, bloomTexture.Get(),
                      static_cast<UINT>(mip + 1),
                      static_cast<UINT>(mip));
        }
        if (dlssUpscaledTexture)
            writePass(kBloomUpscaledSourcePass, dlssUpscaledTexture.Get(),
                      0u, 0u);
    }

    // Four dispatches: features, streak, then a separable blur in both axes.
    // Each writes one of the two half-res targets and reads the other, so the
    // result of the last pass lives in flareScratch (pass 3 is odd).
    // Why the flare is or is not on screen, printed once a second to the
    // console. Every link in the chain is reported, because a flare that fails
    // to appear can die at any of them -- the pipeline failing to build, the
    // per-frame gate skipping the dispatches, the sun projecting off-axis, or
    // the source simply having no energy -- and they are indistinguishable from
    // a black screen. Enabled with SGE_FLARE_DEBUG=1.
    void ReportFlareDiagnostics(bool passRan, bool bloomRan) const {
        static bool checkedEnv = false;
        static bool enabled = false;
        if (!checkedEnv) {
            checkedEnv = true;
            enabled = GetEnvironmentVariableA("SGE_FLARE_DEBUG", nullptr, 0) > 0;
        }
        if (!enabled) return;
        static ULONGLONG lastReport = 0;
        const ULONGLONG now = GetTickCount64();
        if (now - lastReport < 1000) return;
        lastReport = now;
        std::cout
            << "[flare] pipeline=" << (flarePipelineReady ? 1 : 0)
            << " sunLensOn=" << (sunLensSystemEnabled ? 1 : 0)
            << " bloomRan=" << (bloomRan ? 1 : 0)
            << " passRan=" << (passRan ? 1 : 0)
            << " master=" << lensFlareStrength
            << " sunUV=(" << sunLensPosition.x << ", " << sunLensPosition.y
            << ") presence=" << sunLensPosition.z
            << " energy=" << sunLensPosition.w
            << " streak=" << flareStreakIntensity
            << " ghost=" << flareGhostIntensity
            << " size=" << flareWidth << "x" << flareHeight
            << " bloomMips=" << bloomMipCount
            << std::endl;
    }

    void RenderFlare(ID3D12GraphicsCommandList* cmdList) {
        if (!flarePipelineReady || !flareTexture || !flareScratch ||
            !flareDescHeap || !flareRootSig)
            return;

        ProfilerDX12::Scope profile(g_profiler, "Lens Flare", cmdList);

        const UINT descriptorSize =
            g_dx12.device->GetDescriptorHandleIncrementSize(
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        auto transition = [&](ID3D12Resource* resource,
                              D3D12_RESOURCE_STATES before,
                              D3D12_RESOURCE_STATES after) {
            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = resource;
            barrier.Transition.StateBefore = before;
            barrier.Transition.StateAfter = after;
            barrier.Transition.Subresource =
                D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            cmdList->ResourceBarrier(1, &barrier);
        };

        FlareDispatchConstants constants = {};
        constants.flareWidth = flareWidth;
        constants.flareHeight = flareHeight;
        constants.sunU = sunLensPosition.x;
        constants.sunV = sunLensPosition.y;
        constants.sunPresence = sunLensPosition.z;
        // The bloom pyramid is the only source of intensity, so the flare
        // scales with what the sky actually renders rather than with a
        // authored constant: a sun behind cloud produces a weaker flare on its
        // own, with no extra occlusion logic.
        constants.sunEnergy = (std::max)(0.0f, sunLensPosition.w);
        constants.sunTint[0] = sunLensColor.x;
        constants.sunTint[1] = sunLensColor.y;
        constants.sunTint[2] = sunLensColor.z;
        constants.ghostIntensity = flareGhostIntensity * lensFlareStrength;
        constants.ghostDispersion = flareGhostDispersion;
        constants.haloIntensity = flareHaloIntensity * lensFlareStrength;
        constants.starburstIntensity =
            flareStarburstIntensity * lensFlareStrength;
        constants.streakIntensity = flareStreakIntensity * lensFlareStrength;
        constants.streakLength = flareStreakLength;
        constants.bloomMaxMip =
            static_cast<float>((std::max)(1u, bloomMipCount) - 1u);
        constants.blurDirection = 0;
        constants.aspect = static_cast<float>(width) /
                           (std::max)(1.0f, static_cast<float>(height));

        ID3D12DescriptorHeap* heaps[] = { flareDescHeap.Get() };
        cmdList->SetDescriptorHeaps(1, heaps);
        cmdList->SetComputeRootSignature(flareRootSig.Get());

        const UINT groupsX = (flareWidth + 7u) / 8u;
        const UINT groupsY = (flareHeight + 7u) / 8u;

        auto dispatch = [&](UINT pass, ID3D12PipelineState* pso) {
            // Even passes write flareTexture, odd write flareScratch -- the
            // same alternation UpdateFlareDescriptors baked into the tables.
            ID3D12Resource* destination =
                (pass % 2u == 0u) ? flareTexture.Get() : flareScratch.Get();
            transition(destination,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            cmdList->SetPipelineState(pso);
            cmdList->SetComputeRoot32BitConstants(
                0, sizeof(FlareDispatchConstants) / sizeof(UINT),
                &constants, 0);
            D3D12_GPU_DESCRIPTOR_HANDLE table =
                flareDescHeap->GetGPUDescriptorHandleForHeapStart();
            table.ptr += (UINT64)descriptorSize * pass *
                         kFlareDescriptorsPerPass;
            cmdList->SetComputeRootDescriptorTable(1, table);
            cmdList->Dispatch(groupsX, groupsY, 1);
            transition(destination, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        };

        const bool qualityLens = highQualityLensEnabled && qualityLensPipelineReady;
        dispatch(0, qualityLens ? flareQualityFeaturePSO.Get() : flareFeaturePSO.Get());
        dispatch(1, qualityLens ? flareQualityStreakPSO.Get() : flareStreakPSO.Get());
        constants.blurDirection = 0;
        dispatch(2, qualityLens ? flareQualityBlurPSO.Get() : flareBlurPSO.Get());
        constants.blurDirection = 1;
        dispatch(3, qualityLens ? flareQualityBlurPSO.Get() : flareBlurPSO.Get());
    }

    // Where RenderFlare leaves its result. Four passes alternate targets
    // starting at flareTexture, so an even pass count finishes on the odd
    // buffer. Derived rather than hardcoded so adding or removing a pass
    // cannot silently point post at a stale buffer.
    ID3D12Resource* FlareResultResource() const {
        return (kFlarePassCount % 2u == 0u) ? flareScratch.Get()
                                            : flareTexture.Get();
    }

    // Descriptor slot of the first downsample when it reads the DLSS output
    // instead of the jittered render-resolution frame. Past the last
    // down/up pass, so it never collides with them.
    static constexpr UINT kBloomUpscaledSourcePass = VB_BLOOM_MAX_MIPS * 2u - 1u;

    // fromUpscaled: build the pyramid from the DLSS output. The render-res
    // frame is jittered, so sub-pixel bulbs land on different pixels each
    // frame and every halo pulsed with it; the upscaled image has that jitter
    // resolved. Bistro at night, still camera, mean frame-to-frame luma change:
    // 0.170 jittered source, 0.056 upscaled, 0.059 with bloom off.
    void RenderBloom(ID3D12GraphicsCommandList* cmdList, bool fromUpscaled) {
        if (!bloomTexture || !bloomDescHeap || !bloomRootSig ||
            bloomMipCount == 0) return;
        struct BloomDispatchConstants {
            UINT sourceWidth;
            UINT sourceHeight;
            UINT destinationWidth;
            UINT destinationHeight;
            float threshold;
            float softKnee;
            float scatter;
            float padding;
        };
        auto mipSize = [](UINT base, UINT mip) {
            return (std::max)(1u, base >> mip);
        };
        auto transitionMip = [&](UINT mip, D3D12_RESOURCE_STATES before,
                                 D3D12_RESOURCE_STATES after) {
            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = bloomTexture.Get();
            barrier.Transition.StateBefore = before;
            barrier.Transition.StateAfter = after;
            barrier.Transition.Subresource = mip;
            cmdList->ResourceBarrier(1, &barrier);
        };
        const UINT descriptorSize =
            g_dx12.device->GetDescriptorHandleIncrementSize(
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto dispatchPass = [&](UINT pass, UINT destinationMip,
                                const BloomDispatchConstants& constants,
                                ID3D12PipelineState* pso) {
            transitionMip(destinationMip,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            cmdList->SetPipelineState(pso);
            cmdList->SetComputeRoot32BitConstants(
                0, 8, &constants, 0);
            D3D12_GPU_DESCRIPTOR_HANDLE table =
                bloomDescHeap->GetGPUDescriptorHandleForHeapStart();
            table.ptr += static_cast<UINT64>(descriptorSize) * pass * 2u;
            cmdList->SetComputeRootDescriptorTable(1, table);
            cmdList->Dispatch(
                (constants.destinationWidth + 7u) / 8u,
                (constants.destinationHeight + 7u) / 8u, 1);
            transitionMip(destinationMip,
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        };

        cmdList->SetComputeRootSignature(bloomRootSig.Get());
        ID3D12DescriptorHeap* heaps[] = { bloomDescHeap.Get() };
        cmdList->SetDescriptorHeaps(1, heaps);
        for (UINT mip = 0; mip < bloomMipCount; ++mip) {
            BloomDispatchConstants constants = {};
            const bool upscaledSource = mip == 0 && fromUpscaled;
            constants.sourceWidth = mip != 0 ? mipSize(bloomWidth, mip - 1u)
                : upscaledSource ? displayWidth : width;
            constants.sourceHeight = mip != 0 ? mipSize(bloomHeight, mip - 1u)
                : upscaledSource ? displayHeight : height;
            constants.destinationWidth = mipSize(bloomWidth, mip);
            constants.destinationHeight = mipSize(bloomHeight, mip);
            constants.threshold = mip == 0 ? 1.0f : 0.0f;
            constants.softKnee = 0.5f;
            constants.scatter = 0.72f;
            dispatchPass(upscaledSource ? kBloomUpscaledSourcePass : mip, mip,
                         constants, bloomDownsamplePSO.Get());
        }
        for (int mip = static_cast<int>(bloomMipCount) - 2;
             mip >= 0; --mip) {
            const UINT destinationMip = static_cast<UINT>(mip);
            const UINT pass = bloomMipCount +
                (bloomMipCount - 2u - destinationMip);
            BloomDispatchConstants constants = {};
            constants.sourceWidth =
                mipSize(bloomWidth, destinationMip + 1u);
            constants.sourceHeight =
                mipSize(bloomHeight, destinationMip + 1u);
            constants.destinationWidth =
                mipSize(bloomWidth, destinationMip);
            constants.destinationHeight =
                mipSize(bloomHeight, destinationMip);
            constants.scatter = 0.72f;
            dispatchPass(pass, destinationMip, constants,
                         bloomUpsamplePSO.Get());
        }
    }

    bool CreatePostPipeline() {
        std::ifstream csFile("shaders/visbuf_post_cs.hlsl");
        if (!csFile.is_open()) return false;
        std::stringstream stream;
        stream << csFile.rdbuf();
        const std::string source = stream.str();

        ComPtr<ID3DBlob> shaderBlob, errorBlob;
        HRESULT hr = ShaderCacheDX12::CompileCached(source.data(), source.size(),
            "shaders/visbuf_post_cs.hlsl",
            nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "cs_5_0",
            D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
            0, &shaderBlob, &errorBlob);
        if (FAILED(hr)) {
            if (errorBlob) std::cerr << "VB post CS error: "
                << (char*)errorBlob->GetBufferPointer() << std::endl;
            return false;
        }

        D3D12_DESCRIPTOR_RANGE ranges[2] = {};
        // t0..t11: post inputs, raw visibility, previous authored identity,
        // current draw metadata, and the local-to-authored triangle map.
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 16;  // t0..t11 + 3 lens masks + flare
        ranges[0].BaseShaderRegister = 0;
        ranges[0].OffsetInDescriptorsFromTableStart = 0;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 3;
        ranges[1].BaseShaderRegister = 0;
        ranges[1].OffsetInDescriptorsFromTableStart = 16;

        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor.ShaderRegister = 0;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 2;
        params[1].DescriptorTable.pDescriptorRanges = ranges;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
        rootDesc.pParameters = params;
        rootDesc.NumParameters = 2;
        D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
        D3D12_STATIC_SAMPLER_DESC& lutSampler = samplers[0];
        lutSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        lutSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        lutSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        lutSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        lutSampler.ShaderRegister = 0;
        lutSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        lutSampler.MinLOD = 0.0f;
        lutSampler.MaxLOD = D3D12_FLOAT32_MAX;
        // The dirt asset is composed for the whole frame; repeating it makes
        // the dust pattern visibly synthetic when the scale is adjusted.
        D3D12_STATIC_SAMPLER_DESC& dirtSampler = samplers[1];
        dirtSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        dirtSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        dirtSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        dirtSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        dirtSampler.ShaderRegister = 1;
        dirtSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        dirtSampler.MinLOD = 0.0f;
        dirtSampler.MaxLOD = D3D12_FLOAT32_MAX;
        rootDesc.NumStaticSamplers = 2;
        rootDesc.pStaticSamplers = samplers;
        ComPtr<ID3DBlob> rootBlob;
        hr = D3D12SerializeRootSignature(&rootDesc, D3D_ROOT_SIGNATURE_VERSION_1,
            &rootBlob, &errorBlob);
        if (FAILED(hr)) return false;
        hr = g_dx12.device->CreateRootSignature(0, rootBlob->GetBufferPointer(),
            rootBlob->GetBufferSize(), IID_PPV_ARGS(&postRootSig));
        if (FAILED(hr)) return false;

        D3D12_COMPUTE_PIPELINE_STATE_DESC pso = {};
        pso.pRootSignature = postRootSig.Get();
        pso.CS = { shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize() };
        hr = g_dx12.device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&postPSO));
        if (FAILED(hr)) return false;

        const D3D_SHADER_MACRO srDefines[] = {
            { "SGE_DLSS_SR", "1" }, { nullptr, nullptr }
        };
        ComPtr<ID3DBlob> upscaleBlob;
        hr = ShaderCacheDX12::CompileCached(source.data(), source.size(),
            "shaders/visbuf_post_cs.hlsl", srDefines,
            D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "cs_5_0",
            D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
            0, &upscaleBlob, &errorBlob);
        if (FAILED(hr)) {
            if (errorBlob) std::cerr << "VB upscaled post CS error: "
                << (char*)errorBlob->GetBufferPointer() << std::endl;
            return false;
        }
        pso.CS = { upscaleBlob->GetBufferPointer(),
                   upscaleBlob->GetBufferSize() };
        hr = g_dx12.device->CreateComputePipelineState(
            &pso, IID_PPV_ARGS(&postUpscalePSO));
        if (FAILED(hr)) return false;

        auto createQualityPost = [&](bool upscale,
                                     ComPtr<ID3D12PipelineState>& pipeline) {
            const D3D_SHADER_MACRO defines[] = {
                { "SGE_HIGH_QUALITY_LENS", "1" },
                { upscale ? "SGE_DLSS_SR" : nullptr, "1" },
                { nullptr, nullptr }
            };
            ComPtr<ID3DBlob> blob;
            HRESULT result = ShaderCacheDX12::CompileCached(
                source.data(), source.size(), "shaders/visbuf_post_cs.hlsl", defines,
                D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "cs_5_0",
                D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3,
                0, &blob, &errorBlob);
            if (FAILED(result)) {
                if (errorBlob) std::cerr << "VB quality lens post: "
                    << (const char*)errorBlob->GetBufferPointer() << std::endl;
                return false;
            }
            pso.CS = { blob->GetBufferPointer(), blob->GetBufferSize() };
            return SUCCEEDED(g_dx12.device->CreateComputePipelineState(
                &pso, IID_PPV_ARGS(&pipeline)));
        };
        qualityLensPipelineReady = flareQualityFeaturePSO && flareQualityStreakPSO &&
            flareQualityBlurPSO && createQualityPost(false, postQualityLensPSO) &&
            createQualityPost(true, postUpscaleQualityLensPSO);
        if (!qualityLensPipelineReady)
            std::cerr << "High quality lens unavailable; keeping the established post pass\n";

        D3D12_DESCRIPTOR_HEAP_DESC heap = {};
        heap.NumDescriptors = kPostDescriptorsPerVariant *
                              kPostDescriptorVariantCount;
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = g_dx12.device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&postDescHeap));
        if (FAILED(hr)) return false;
        UpdatePostDescriptors();
        return true;
    }

    void UpdatePostDescriptors() {
        if (!postDescHeap) return;
        UINT descriptorSize = g_dx12.device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        for (UINT sourceMode = 0; sourceMode < 2; ++sourceMode) {
        for (UINT parity = 0; parity < 2; ++parity) {
          for (UINT stableWrite = 0; stableWrite < 2; ++stableWrite) {
            D3D12_CPU_DESCRIPTOR_HANDLE handle =
                postDescHeap->GetCPUDescriptorHandleForHeapStart();
            const UINT variant = sourceMode * 4u + parity * 2u + stableWrite;
            handle.ptr += (SIZE_T)descriptorSize * variant *
                          kPostDescriptorsPerVariant;

            D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
            srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1;
            g_dx12.device->CreateShaderResourceView(
                sourceMode && dlssUpscaledTexture
                    ? dlssUpscaledTexture.Get() : outputTexture.Get(),
                &srv, handle);
            handle.ptr += descriptorSize;
            srv.Format = DXGI_FORMAT_R16G16_FLOAT;
            g_dx12.device->CreateShaderResourceView(motionTexture.Get(), &srv, handle);
            handle.ptr += descriptorSize;
            srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            g_dx12.device->CreateShaderResourceView(
                historyTextures[parity ^ 1u].Get(), &srv, handle);
            handle.ptr += descriptorSize;
            D3D12_SHADER_RESOURCE_VIEW_DESC exposureSrv = {};
            exposureSrv.Format = DXGI_FORMAT_R32_TYPELESS;
            exposureSrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            exposureSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            exposureSrv.Buffer.NumElements = 3;
            exposureSrv.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
            g_dx12.device->CreateShaderResourceView(
                exposureState.Get(), &exposureSrv, handle);
            handle.ptr += descriptorSize;
            D3D12_SHADER_RESOURCE_VIEW_DESC lutSrv = {};
            lutSrv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            lutSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
            lutSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            lutSrv.Texture3D.MipLevels = 1;
            g_dx12.device->CreateShaderResourceView(colorLUT.Get(), &lutSrv, handle);
            handle.ptr += descriptorSize;
            D3D12_SHADER_RESOURCE_VIEW_DESC depthSrv = {};
            depthSrv.Format = DXGI_FORMAT_R32_FLOAT;
            depthSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            depthSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            depthSrv.Texture2D.MipLevels = 1;
            g_dx12.device->CreateShaderResourceView(
                ActiveDepthBuffer(), &depthSrv, handle);
            handle.ptr += descriptorSize;
            g_dx12.device->CreateShaderResourceView(
                visibilityDepthTexture.Get(), &depthSrv, handle);
            handle.ptr += descriptorSize;
            D3D12_SHADER_RESOURCE_VIEW_DESC bloomSrv = {};
            bloomSrv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            bloomSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            bloomSrv.Shader4ComponentMapping =
                D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            bloomSrv.Texture2D.MostDetailedMip = 0;
            // The whole pyramid, not just mip 0. RenderBloom builds every mip
            // and then upsamples back down into mip 0, so pinning MipLevels to
            // 1 threw away a ready-made set of progressively wider blurs. The
            // coarse mips are what make wide veiling glare and a long
            // anamorphic streak affordable -- a few fetches instead of a
            // full-res multi-tap blur.
            bloomSrv.Texture2D.MipLevels = (std::max)(1u, bloomMipCount);
            g_dx12.device->CreateShaderResourceView(
                bloomTexture.Get(), &bloomSrv, handle);
            handle.ptr += descriptorSize;
            // t8 is raw visibility for local geometry addressing. t9 is the
            // prior authored key, independent of draw and primitive ordering.
            D3D12_SHADER_RESOURCE_VIEW_DESC idSrv = {};
            idSrv.Format = DXGI_FORMAT_R32G32_UINT;
            idSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            idSrv.Shader4ComponentMapping =
                D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            idSrv.Texture2D.MipLevels = 1;
            g_dx12.device->CreateShaderResourceView(
                visBufferRT.Get(), &idSrv, handle);
            handle.ptr += descriptorSize;
            g_dx12.device->CreateShaderResourceView(
                StableSurfaceResource(stableWrite ^ 1u), &idSrv, handle);
            handle.ptr += descriptorSize;

            D3D12_SHADER_RESOURCE_VIEW_DESC drawSrv = {};
            drawSrv.Format = DXGI_FORMAT_UNKNOWN;
            drawSrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            drawSrv.Shader4ComponentMapping =
                D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            drawSrv.Buffer.NumElements = VB_MAX_DRAW_CALLS;
            drawSrv.Buffer.StructureByteStride = sizeof(VBDrawCallData);
            g_dx12.device->CreateShaderResourceView(
                drawCallBuffer.Get(), &drawSrv, handle);
            handle.ptr += descriptorSize;

            D3D12_SHADER_RESOURCE_VIEW_DESC stableTriangleSrv = {};
            stableTriangleSrv.Format = DXGI_FORMAT_UNKNOWN;
            stableTriangleSrv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            stableTriangleSrv.Shader4ComponentMapping =
                D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            stableTriangleSrv.Buffer.NumElements = geometryTriangleCapacity;
            stableTriangleSrv.Buffer.StructureByteStride = sizeof(UINT);
            g_dx12.device->CreateShaderResourceView(
                stableTriangleDataBuffer.Get(), &stableTriangleSrv, handle);
            handle.ptr += descriptorSize;

            // t12 lens dirt. Lens SRVs stay at the end of the range so the
            // established post inputs keep their bindings.
            D3D12_SHADER_RESOURCE_VIEW_DESC dirtSrv = {};
            dirtSrv.Format = DXGI_FORMAT_R8_UNORM;
            dirtSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            dirtSrv.Shader4ComponentMapping =
                D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            dirtSrv.Texture2D.MipLevels = 1;
            g_dx12.device->CreateShaderResourceView(
                lensDirtTexture.Get(), &dirtSrv, handle);
            handle.ptr += descriptorSize;

            // t13 sun flare scattering mask.
            g_dx12.device->CreateShaderResourceView(
                sunFlareTexture.Get(), &dirtSrv, handle);
            handle.ptr += descriptorSize;

            // t14 internal-reflection profile. UAV descriptors follow the
            // lens masks while retaining their shader registers u0..u2.
            g_dx12.device->CreateShaderResourceView(
                lensGhostTexture.Get(), &dirtSrv, handle);
            handle.ptr += descriptorSize;

            // t15 the assembled half-res flare. Last SRV in the range so the
            // UAVs below keep u0..u2; FlareResultResource names whichever of
            // the two ping-pong targets the final pass wrote.
            D3D12_SHADER_RESOURCE_VIEW_DESC flareSrv = {};
            flareSrv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            flareSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            flareSrv.Shader4ComponentMapping =
                D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            flareSrv.Texture2D.MipLevels = 1;
            g_dx12.device->CreateShaderResourceView(
                FlareResultResource(), &flareSrv, handle);
            handle.ptr += descriptorSize;

            D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
            uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            g_dx12.device->CreateUnorderedAccessView(
                presentTexture.Get(), nullptr, &uav, handle);
            handle.ptr += descriptorSize;
            uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            g_dx12.device->CreateUnorderedAccessView(
                historyTextures[parity].Get(), nullptr, &uav, handle);
            handle.ptr += descriptorSize;
            uav.Format = DXGI_FORMAT_R32G32_UINT;
            g_dx12.device->CreateUnorderedAccessView(
                StableSurfaceResource(stableWrite), nullptr, &uav, handle);
          }
        }
        }
    }

public:
    // Call this each frame before resolve to update the light CBV descriptors
    void UpdateLightDescriptors(D3D12_GPU_VIRTUAL_ADDRESS lightBufferAddr,
                                D3D12_GPU_VIRTUAL_ADDRESS pointLightsAddr,
                                D3D12_GPU_VIRTUAL_ADDRESS shBufferAddr,
                                D3D12_GPU_VIRTUAL_ADDRESS ddgiBufferAddr) {
        UINT descSize = g_dx12.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = computeDescHeap->GetCPUDescriptorHandleForHeapStart();
        cpuHandle.ptr += 82 * descSize;

        // [7] b1 - light buffer CBV
        {
            D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
            cbvDesc.BufferLocation = lightBufferAddr;
            cbvDesc.SizeInBytes = sizeof(LightBufferDX12);
            g_dx12.device->CreateConstantBufferView(&cbvDesc, cpuHandle);
            cpuHandle.ptr += descSize;
        }

        // [8] b2 - point lights CBV
        {
            D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
            cbvDesc.BufferLocation = pointLightsAddr;
            cbvDesc.SizeInBytes = sizeof(PointLightsBufferDX12);
            g_dx12.device->CreateConstantBufferView(&cbvDesc, cpuHandle);
            cpuHandle.ptr += descSize;
        }

        // [11] b3 - preconvolved HDRI spherical harmonics
        {
            D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
            cbvDesc.BufferLocation = shBufferAddr;
            cbvDesc.SizeInBytes = sizeof(SHBufferDX12);
            g_dx12.device->CreateConstantBufferView(&cbvDesc, cpuHandle);
            cpuHandle.ptr += descSize;
        }

        // b4 - DDGI grid parameters
        {
            D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
            cbvDesc.BufferLocation = ddgiBufferAddr;
            cbvDesc.SizeInBytes = sizeof(DDGIBufferDX12);
            g_dx12.device->CreateConstantBufferView(&cbvDesc, cpuHandle);
        }
    }

    void UpdateDDGIResources(ID3D12Resource* irradianceResource,
                             ID3D12Resource* visibilityResource) {
        if (!computeDescHeap) return;
        D3D12_CPU_DESCRIPTOR_HANDLE handle =
            computeDescHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(g_dx12.cbvSrvUavDescriptorSize) * 74u;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;
        srv.Format = irradianceResource
            ? irradianceResource->GetDesc().Format : DXGI_FORMAT_R16G16B16A16_FLOAT;
        g_dx12.device->CreateShaderResourceView(irradianceResource, &srv, handle);
        handle.ptr += g_dx12.cbvSrvUavDescriptorSize;
        srv.Format = visibilityResource
            ? visibilityResource->GetDesc().Format : DXGI_FORMAT_R16G16_FLOAT;
        g_dx12.device->CreateShaderResourceView(visibilityResource, &srv, handle);
    }

    void UpdateSparseDDGIResources(ID3D12Resource* probes, UINT probeCount,
                                   ID3D12Resource* cells, UINT cellCount,
                                   ID3D12Resource* indices, UINT indexCount) {
        if (!computeDescHeap) return;
        D3D12_CPU_DESCRIPTOR_HANDLE handle =
            computeDescHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(
            g_dx12.cbvSrvUavDescriptorSize) * 76u;
        ID3D12Resource* resources[3] = { probes, cells, indices };
        const UINT counts[3] = {
            (std::max)(probeCount, 1u), (std::max)(cellCount, 1u),
            (std::max)(indexCount, 1u)
        };
        const UINT strides[3] = {
            sizeof(DXRProbeRecord), sizeof(DXRProbeGridCell), sizeof(UINT)
        };
        for (UINT i = 0; i < 3; ++i) {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
            srv.Format = DXGI_FORMAT_UNKNOWN;
            srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            srv.Shader4ComponentMapping =
                D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Buffer.NumElements = counts[i];
            srv.Buffer.StructureByteStride = strides[i];
            g_dx12.device->CreateShaderResourceView(resources[i], &srv, handle);
            handle.ptr += g_dx12.cbvSrvUavDescriptorSize;
        }
    }

    void UpdateEnvironmentMap(ID3D12Resource* environmentResource,
                              ID3D12Resource* brdfResource) {
        environmentMapResource = environmentResource;
        brdfLUTResource = brdfResource;
        if (!computeDescHeap) return;
        D3D12_CPU_DESCRIPTOR_HANDLE handle =
            computeDescHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(g_dx12.cbvSrvUavDescriptorSize) * 72u;
        WriteEnvironmentDescriptors(handle);
    }

    // t72 environment and t73 BRDF LUT, at `handle` and the slot after it.
    void WriteEnvironmentDescriptors(D3D12_CPU_DESCRIPTOR_HANDLE handle) {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = environmentMapResource
            ? environmentMapResource->GetDesc().Format
            : DXGI_FORMAT_R32G32B32A32_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = environmentMapResource
            ? environmentMapResource->GetDesc().MipLevels : 1;
        g_dx12.device->CreateShaderResourceView(environmentMapResource, &srv,
                                                handle);
        handle.ptr += g_dx12.cbvSrvUavDescriptorSize;
        D3D12_SHADER_RESOURCE_VIEW_DESC brdf = {};
        brdf.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        brdf.Format = DXGI_FORMAT_R32G32_FLOAT;
        brdf.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        brdf.Texture2D.MipLevels = 1;
        g_dx12.device->CreateShaderResourceView(brdfLUTResource, &brdf, handle);
    }

    // Update the shadow map SRV in the compute descriptor heap
    void UpdateShadowMapDescriptor(ID3D12Resource* shadowMapResource) {
        if (!computeDescHeap) return;

        // Spot atlas rides along on the same per-frame call. The resource
        // itself never changes once ShadowMapDX12 has created it, but this is
        // the first point each frame where the heap is known to exist, and
        // rewriting a descriptor to the same resource costs nothing.
        WriteSpotShadowAtlasDescriptor(computeDescHeap.Get(),
                                       kSpotShadowAtlasSlot);

        UINT descSize = g_dx12.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = computeDescHeap->GetCPUDescriptorHandleForHeapStart();
        cpuHandle.ptr += 2 * descSize; // slot [2] = t2

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2DArray.MipLevels = 1;
        srvDesc.Texture2DArray.ArraySize = shadowMapResource
            ? shadowMapResource->GetDesc().DepthOrArraySize : SHADOW_CASCADE_COUNT;

        if (shadowMapResource) {
            g_dx12.device->CreateShaderResourceView(shadowMapResource, &srvDesc, cpuHandle);
        } else {
            g_dx12.device->CreateShaderResourceView(nullptr, &srvDesc, cpuHandle);
        }
    }
};

#endif // VISIBILITY_BUFFER_DX12_H
