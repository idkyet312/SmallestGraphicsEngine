#ifndef VISIBILITY_BUFFER_DX12_H
#define VISIBILITY_BUFFER_DX12_H

#include "ShaderCacheDX12.h"
#include "DX12Core.h"
#include "ShaderDX12.h"
#include "SceneGraph.h"
#include "ProfilerDX12.h"
#include <DirectXPackedVector.h>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Defined in main.cpp. Same pattern as ForwardRenderer.h / IdTechRenderer.h:
// the SVGF passes time themselves, and this header is included before the
// definition.
extern ProfilerDX12 g_profiler;

// Instance IDs occupy a full uint in the visibility target. Keep this aligned
// with ShaderDX12's per-frame matrix capacity.
static const UINT VB_MAX_DRAW_CALLS = MAX_DRAW_CALLS_PER_FRAME;
// Maximum total vertices across all draw calls
static const UINT VB_MAX_VERTICES = 1024 * 1024;
// Maximum total indices across all draw calls
static const UINT VB_MAX_INDICES = 1024 * 1024 * 3;
static const UINT VB_MAX_TRIANGLES = VB_MAX_INDICES / 3;
static const UINT VB_CLUSTER_X = 16;
static const UINT VB_CLUSTER_Y = 9;
static const UINT VB_CLUSTER_Z = 10;
static const UINT VB_CLUSTER_COUNT = VB_CLUSTER_X * VB_CLUSTER_Y * VB_CLUSTER_Z;
static const UINT VB_MAX_LIGHTS_PER_CLUSTER = 32;
static const UINT VB_MAX_MATERIALS = 256;
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
    UINT     padding[2];
    XMFLOAT4 palmWind;
    XMFLOAT4 palmPrimary;
    XMFLOAT4 palmSecondary;
    XMFLOAT4 palmPreviousPrimary;
    XMFLOAT4 palmPreviousSecondary;
    XMFLOAT4 palmParams;
};

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
};

struct alignas(256) VBExposureConstants {
    UINT inputWidth;
    UINT inputHeight;
    float adaptationRate;
    float middleGray;
};

class VisibilityBufferDX12 {
public:
    // Full-width instance and primitive IDs (R32G32_UINT).
    ComPtr<ID3D12Resource> visBufferRT;
    // Previous frame's IDs. This is what makes temporal reuse exact: a pixel
    // whose instance+primitive match last frame's is provably the same surface,
    // rather than "depth and normal look similar". Screen-space heuristics
    // cannot express that, which is the whole point.
    ComPtr<ID3D12Resource> visBufferHistory;
    bool surfaceHistoryValid = false;
    // Use exact surface IDs for temporal validity instead of the depth
    // heuristic. On by default: it is strictly better information, costs one
    // texture fetch, and the fallback stays for the frame after a resize.
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
    ComPtr<ID3D12DescriptorHeap> visRtvHeap;    // RTV for visibility pass
    ComPtr<ID3D12DescriptorHeap> visSrvUavHeap; // SRV/UAV for compute resolve

    // Depth buffer SRV for the compute pass (reads main depth)
    // We'll create a SRV for the engine's existing depth buffer

    // Lighting stays HDR until the dedicated cinematic post pass.
    ComPtr<ID3D12Resource> outputTexture;
    ComPtr<ID3D12DescriptorHeap> outputRtvHeap;
    ComPtr<ID3D12Resource> presentTexture;
    ComPtr<ID3D12Resource> motionTexture;
    ComPtr<ID3D12DescriptorHeap> motionRtvHeap;
    ComPtr<ID3D12Resource> normalRoughnessTexture;
    ComPtr<ID3D12Resource> bloomTexture;
    // Depth immediately after visibility resolve. Forward extensions can then
    // be detected and rejected from history when they lack motion vectors.
    ComPtr<ID3D12Resource> visibilityDepthTexture;
    ComPtr<ID3D12Resource> historyTextures[2];
    ComPtr<ID3D12Resource> exposureState;
    ComPtr<ID3D12Resource> colorLUT;
    ComPtr<ID3D12Resource> colorLUTUpload;

    // Visibility pass PSO + root signature
    ComPtr<ID3D12RootSignature> visPassRootSig;
    ComPtr<ID3D12PipelineState> visPassPSO;
    ComPtr<ID3D12PipelineState> visPassDoubleSidedPSO;
    ComPtr<ID3D12PipelineState> visPassAlphaPSO;
    ComPtr<ID3D12PipelineState> visPassAlphaDoubleSidedPSO;

    // Compute resolve PSO + root signature
    ComPtr<ID3D12RootSignature> resolveRootSig;
    ComPtr<ID3D12PipelineState> resolvePSO;

    // Enhanced-visuals resolve: same shader source compiled at cs_6_5 with
    // SGE_ENHANCED_VISUALS, adding inline RayQuery. Null when unavailable
    // (no DXC, no Tier 1.1, or a compile failure), which is the signal the
    // enhanced tier cannot be enabled.
    ComPtr<ID3D12RootSignature> enhancedResolveRootSig;
    ComPtr<ID3D12PipelineState> enhancedResolvePSO;
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
    D3D12_GPU_VIRTUAL_ADDRESS enhancedHeapTLASAddresses[FRAME_COUNT] = {};
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
    bool enhancedProbeMissGIActive = true;
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
    ComPtr<ID3D12Resource> svgfStableSurfaceCurrent;
    ComPtr<ID3D12Resource> svgfStableSurfaceHistory;
    // SVGF à-trous spatial filter (Phase 5c). Multi-iteration wavelet
    // filter applied to the specular IBL signal after the temporal pass
    // converges it in time. Ping-pong scratch pair for the iterations.
    bool svgfAtrousEnabled = true;
    UINT svgfAtrousIterations = 5;
    UINT svgfAtrousDiagnosticMode = 0;
    static constexpr UINT kSVGFAtrousMaxIterations = 5;
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
    ComPtr<ID3D12DescriptorHeap> postDescHeap;
    // Descriptors per parity in postDescHeap: 8 SRVs (hdr, motion, history,
    // exposure, LUT, depth, visDepth, bloom) + 2 surface-ID SRVs + 2 UAVs
    // (present, history). The heap holds two of these, one per history parity.
    static constexpr UINT kPostDescriptorsPerParity = 12;
    ComPtr<ID3D12RootSignature> bloomRootSig;
    ComPtr<ID3D12PipelineState> bloomDownsamplePSO;
    ComPtr<ID3D12PipelineState> bloomUpsamplePSO;
    ComPtr<ID3D12DescriptorHeap> bloomDescHeap;
    ComPtr<ID3D12RootSignature> exposureRootSig;
    ComPtr<ID3D12PipelineState> exposureResetPSO;
    ComPtr<ID3D12PipelineState> exposureAccumulatePSO;
    ComPtr<ID3D12PipelineState> exposureFinalizePSO;
    ComPtr<ID3D12DescriptorHeap> exposureDescHeap;

    // GPU-visible structured buffers
    ComPtr<ID3D12Resource> drawCallBuffer;       // StructuredBuffer<DrawCallData>
    ComPtr<ID3D12Resource> drawCallUpload;
    ComPtr<ID3D12Resource> vertexDataBuffer;     // StructuredBuffer<PackedVertex>
    ComPtr<ID3D12Resource> vertexDataUpload;
    ComPtr<ID3D12Resource> indexDataBuffer;       // StructuredBuffer<uint>
    ComPtr<ID3D12Resource> indexDataUpload;
    ComPtr<ID3D12Resource> stableTriangleDataBuffer;
    ComPtr<ID3D12Resource> stableTriangleDataUpload;
    // Per-geometry binding from a raytracing hit to this buffer's persistent
    // geometry. Written only when the acceleration structure is rebuilt, which
    // is a rare, already-GPU-drained event, so it lives on an upload heap and
    // is written in place rather than staged through a copy.
    ComPtr<ID3D12Resource> hitGeometryBuffer;
    UINT hitGeometryCount = 0;
    ComPtr<ID3D12Resource> clusterDataBuffer;     // StructuredBuffer<ClusterData>
    ComPtr<ID3D12Resource> clusterDataUpload;
    ComPtr<ID3D12Resource> materialDataBuffer;
    VBMaterialData* mappedMaterials = nullptr;

    // Upload buffer for frame constants
    UploadBuffer<VBFrameConstants> frameConstantBuffer;
    UploadBuffer<VBPostConstants> postConstantBuffer;
    UploadBuffer<VBExposureConstants> exposureConstantBuffer;

    // Descriptor heap for compute pass SRVs/UAVs
    ComPtr<ID3D12DescriptorHeap> computeDescHeap;

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
    std::unordered_map<const SceneMaterial*, UINT> materialLookup;
    std::unordered_map<ID3D12Resource*, UINT> materialTextureLookup;
    UINT materialCount = 1;
    UINT materialTextureCount = 0;
    // Distinct textures turned away because the fixed 64-slot array was full.
    // A set, not a counter: the useful number is how many UNIQUE textures did
    // not fit, since that is how much larger the array would have to be (or how
    // much a bindless heap would buy).
    std::unordered_set<ID3D12Resource*> materialTexturesRejected;
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
    UINT postFrameIndex = 0;
    float exposure = 1.15f;
    float bloomStrength = 0.16f;
    float vignetteStrength = 0.50f;
    float grainStrength = 0.0f;
    float taaFeedback = 0.86f;
    bool temporalEffectsEnabled = false;
    bool temporalHistoryValid = false;
    bool exposureReadable = false;
    float exposureAdaptation = 0.05f;
    float motionBlurStrength = 0.0f;
    PalmWindFrameDX12 palmWindFrame{};
    float focusDistance = 8.0f;
    float aperture = 0.0f;
    float currentNearPlane = 0.1f;
    float currentFarPlane = 1000.0f;
    int debugViewMode = 0; // 0=lit, 1=instance/primitive IDs, 2=depth, 3=edge mask
    bool validationMode = false;
    UINT bloomMipCount = 1;
    UINT bloomWidth = 1;
    UINT bloomHeight = 1;

    UINT width = 0;
    UINT height = 0;
    bool initialized = false;
    std::string initError;

    XMFLOAT2 GetTemporalJitterPixels() const {
        if (!temporalEffectsEnabled || validationMode) return { 0.0f, 0.0f };
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

    void InvalidateTemporalHistory() { temporalHistoryValid = false; }

    bool Init(UINT screenWidth, UINT screenHeight) {
        width = screenWidth;
        height = screenHeight;

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
        if (!require(CreateStructuredBuffers(), "structured buffers")) return false;
        if (!require(CreateComputeDescriptorHeap(), "compute descriptors")) return false;
        if (!require(CreateVisPassPipeline(), "visibility shaders")) return false;
        if (!require(CreateResolvePipeline(), "resolve shader")) return false;
        if (!require(CreateBloomPipeline(), "bloom pyramid shaders")) return false;
        if (!require(CreatePostPipeline(), "post-process shader")) return false;
        if (!require(CreateExposurePipeline(), "exposure shaders")) return false;

        if (!require(frameConstantBuffer.Create(FRAME_COUNT), "frame constants")) return false;
        if (!require(postConstantBuffer.Create(FRAME_COUNT), "post constants")) return false;
        if (!require(exposureConstantBuffer.Create(FRAME_COUNT), "exposure constants")) return false;

        cpuDrawCalls.resize(VB_MAX_DRAW_CALLS);
        cpuVertices.resize(VB_MAX_VERTICES);
        cpuIndices.resize(VB_MAX_INDICES);
        cpuStableTriangleIDs.resize(VB_MAX_TRIANGLES);
        cpuClusters.resize(VB_CLUSTER_COUNT);
        previousModels.resize(VB_MAX_DRAW_CALLS);
        if (mappedMaterials) mappedMaterials[0] = VBMaterialData{};

        initialized = true;
        std::cout << "Visibility Buffer initialized (" << width << "x" << height << ")" << std::endl;
        return true;
    }

    void BeginFrame() {
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
            data.emissiveOcclusion.w = material->occlusionStrength;
            data.pbrParams = XMFLOAT4(material->metallicFactor,
                material->roughnessFactor, material->normalYSign, 1.0f);
            data.shadingParams = XMFLOAT4(material->ambientScale,
                material->viewFillStrength, 0.7f, 0.0f);
        };
        auto found = materialLookup.find(material);
        if (found != materialLookup.end()) {
            if (mappedMaterials)
                updateParameters(mappedMaterials[found->second]);
            return found->second;
        }
        if (materialCount >= VB_MAX_MATERIALS) return 0;

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

    // Upload-once mesh registration. Instances reference this immutable geometry
    // every frame instead of duplicating vertices per draw.
    UINT RegisterMesh(const float* vertexData, UINT vertexCount,
                      const UINT* indexData, UINT indexCount,
                      UINT vertexStrideFloats = 8,
                      const UINT* stableTriangleIDs = nullptr,
                      UINT stableTriangleCount = 0,
                      UINT stableTriangleNamespace = 0) {
        const UINT triangleCount = indexData && indexCount > 0
            ? indexCount / 3u : vertexCount / 3u;
        if (!vertexData || vertexCount == 0 ||
            persistentVertexCount + vertexCount > VB_MAX_VERTICES ||
            persistentIndexCount + indexCount > VB_MAX_INDICES ||
            persistentTriangleCount + triangleCount > VB_MAX_TRIANGLES ||
            (stableTriangleIDs && stableTriangleCount != triangleCount)) {
            return VB_INVALID_MESH;
        }

        VBMeshData mesh;
        mesh.vertexOffset = persistentVertexCount;
        mesh.vertexCount = vertexCount;
        mesh.indexOffset = persistentIndexCount;
        mesh.indexCount = indexCount;
        mesh.hasIndices = (indexData && indexCount > 0) ? 1u : 0u;
        mesh.stableTriangleOffset = persistentTriangleCount;
        mesh.stableTriangleNamespace = stableTriangleNamespace;

        for (UINT i = 0; i < vertexCount; ++i) {
            const float* v = vertexData + i * vertexStrideFloats;
            VBPackedVertex& pv = cpuVertices[persistentVertexCount + i];
            pv.d0 = XMFLOAT4(v[0], v[1], v[2], v[3]);
            pv.d1 = XMFLOAT4(v[4], v[5], v[6], v[7]);
        }
        if (mesh.hasIndices) {
            memcpy(cpuIndices.data() + persistentIndexCount, indexData,
                   indexCount * sizeof(UINT));
        }
        for (UINT triangle = 0; triangle < triangleCount; ++triangle) {
            cpuStableTriangleIDs[persistentTriangleCount + triangle] =
                stableTriangleIDs ? stableTriangleIDs[triangle] : triangle;
        }

        persistentVertexCount += vertexCount;
        persistentIndexCount += indexCount;
        persistentTriangleCount += triangleCount;
        if (stableTriangleIDs)
            persistentAuthoredTriangleCount += triangleCount;
        meshes.push_back(mesh);
        geometryDirty = true;
        return static_cast<UINT>(meshes.size() - 1);
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
            primitive->stableTriangleNamespace);
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
    // because RegisterMesh is upload-once -- these offsets never move for the
    // lifetime of a mesh.
    bool MeshGeometryBinding(UINT meshID, UINT& vertexOffset,
                             UINT& indexOffset, UINT& hasIndices) const {
        if (meshID == VB_INVALID_MESH || meshID >= meshes.size()) return false;
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
        // Upload draw calls
        if (drawCallDirtyMin != UINT_MAX) {
            const UINT64 offset = static_cast<UINT64>(drawCallDirtyMin) *
                sizeof(VBDrawCallData);
            const UINT64 size = static_cast<UINT64>(
                drawCallDirtyMax - drawCallDirtyMin + 1) *
                sizeof(VBDrawCallData);
            void* mapped = nullptr;
            D3D12_RANGE readRange = { 0, 0 };
            drawCallUpload->Map(0, &readRange, &mapped);
            memcpy(static_cast<uint8_t*>(mapped) + offset,
                cpuDrawCalls.data() + drawCallDirtyMin, size);
            drawCallUpload->Unmap(0, nullptr);

            cmdList->CopyBufferRegion(drawCallBuffer.Get(), offset,
                drawCallUpload.Get(), offset, size);
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
            clusterDataUpload->Map(0, &readRange, &mapped);
            memcpy(mapped, cpuClusters.data(),
                   cpuClusters.size() * sizeof(VBClusterData));
            clusterDataUpload->Unmap(0, nullptr);
            cmdList->CopyBufferRegion(clusterDataBuffer.Get(), 0,
                clusterDataUpload.Get(), 0,
                cpuClusters.size() * sizeof(VBClusterData));
        }

        if (geometryDirty && persistentVertexCount > 0) {
            void* mapped = nullptr;
            D3D12_RANGE readRange = { 0, 0 };
            vertexDataUpload->Map(0, &readRange, &mapped);
            memcpy(mapped, cpuVertices.data(), persistentVertexCount * sizeof(VBPackedVertex));
            vertexDataUpload->Unmap(0, nullptr);

            cmdList->CopyBufferRegion(vertexDataBuffer.Get(), 0,
                vertexDataUpload.Get(), 0, persistentVertexCount * sizeof(VBPackedVertex));
        }

        if (geometryDirty && persistentIndexCount > 0) {
            void* mapped = nullptr;
            D3D12_RANGE readRange = { 0, 0 };
            indexDataUpload->Map(0, &readRange, &mapped);
            memcpy(mapped, cpuIndices.data(), persistentIndexCount * sizeof(UINT));
            indexDataUpload->Unmap(0, nullptr);

            cmdList->CopyBufferRegion(indexDataBuffer.Get(), 0,
                indexDataUpload.Get(), 0, persistentIndexCount * sizeof(UINT));
        }

        if (geometryDirty && persistentTriangleCount > 0) {
            void* mapped = nullptr;
            D3D12_RANGE readRange = { 0, 0 };
            stableTriangleDataUpload->Map(0, &readRange, &mapped);
            memcpy(mapped, cpuStableTriangleIDs.data(),
                persistentTriangleCount * sizeof(UINT));
            stableTriangleDataUpload->Unmap(0, nullptr);

            cmdList->CopyBufferRegion(stableTriangleDataBuffer.Get(), 0,
                stableTriangleDataUpload.Get(), 0,
                persistentTriangleCount * sizeof(UINT));
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
        if (geometryDirty) {
            barriers[2] = barriers[0];
            barriers[2].Transition.pResource = vertexDataBuffer.Get();
            barriers[3] = barriers[0];
            barriers[3].Transition.pResource = indexDataBuffer.Get();
            barriers[4] = barriers[0];
            barriers[4].Transition.pResource = stableTriangleDataBuffer.Get();
            barrierCount = 5;
            geometryUploaded = true;
            geometryDirty = false;
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
        D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = g_dx12.dsvHeap->GetCPUDescriptorHandleForHeapStart();
        cmdList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

        // Set render targets: vis buffer + main depth buffer
        cmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

        // Set viewport / scissor
        cmdList->RSSetViewports(1, &g_dx12.viewport);
        cmdList->RSSetScissorRects(1, &g_dx12.scissorRect);

        // Set pipeline
        ID3D12DescriptorHeap* heaps[] = { computeDescHeap.Get() };
        cmdList->SetDescriptorHeaps(1, heaps);
        cmdList->SetGraphicsRootSignature(visPassRootSig.Get());
        cmdList->SetGraphicsRootShaderResourceView(3,
            drawCallBuffer->GetGPUVirtualAddress());
        D3D12_GPU_DESCRIPTOR_HANDLE defaultTexture =
            computeDescHeap->GetGPUDescriptorHandleForHeapStart();
        defaultTexture.ptr += static_cast<UINT64>(
            g_dx12.cbvSrvUavDescriptorSize) * 8u;
        cmdList->SetGraphicsRootDescriptorTable(2, defaultTexture);
        cmdList->SetPipelineState(visPassPSO.Get());
    }

    void SetVisPassDraw(ID3D12GraphicsCommandList* cmdList, UINT drawCallID,
                        UINT materialID, bool doubleSided, bool alphaCutout,
                        bool alphaFromLuminance) {
        const UINT constants[4] = { drawCallID, alphaCutout ? 1u : 0u,
            alphaFromLuminance ? 1u : 0u, 0u };
        cmdList->SetGraphicsRoot32BitConstants(1, 4, constants, 0);

        UINT textureIndex = 0;
        if (materialID < materialCount &&
            mappedMaterials[materialID].textureIndices[0] < VB_MAX_MATERIAL_TEXTURES)
            textureIndex = mappedMaterials[materialID].textureIndices[0];
        D3D12_GPU_DESCRIPTOR_HANDLE texture =
            computeDescHeap->GetGPUDescriptorHandleForHeapStart();
        texture.ptr += static_cast<UINT64>(g_dx12.cbvSrvUavDescriptorSize) *
            (8u + textureIndex);
        cmdList->SetGraphicsRootDescriptorTable(2, texture);
        cmdList->SetPipelineState(alphaCutout
            ? (doubleSided ? visPassAlphaDoubleSidedPSO.Get()
                           : visPassAlphaPSO.Get())
            : (doubleSided ? visPassDoubleSidedPSO.Get()
                           : visPassPSO.Get()));
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
        UINT bufferIndex = g_dx12.frameIndex * MAX_DRAW_CALLS_PER_FRAME + drawIndex;

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

    // Run the compute resolve pass
    void Resolve(ID3D12GraphicsCommandList* cmdList,
                 const XMMATRIX& view, const XMMATRIX& proj,
                 const XMMATRIX& lightViewProj,
                 const XMMATRIX& previousViewProj,
                 const XMFLOAT3& cameraPos,
                 float nearPlane, float farPlane,
                 const LightBufferDX12& lightData,
                 const PointLightsBufferDX12& pointLightData) {
        currentNearPlane = nearPlane;
        currentFarPlane = farPlane;
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

        // Also transition depth buffer to SRV for reading
        {
            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = g_dx12.depthStencilBuffer.Get();
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            cmdList->ResourceBarrier(1, &barrier);
        }

        // Upload frame constants
        VBFrameConstants fc;
        fc.viewMatrix = XMMatrixTranspose(view);
        fc.projMatrix = XMMatrixTranspose(proj);
        XMMATRIX invVP = XMMatrixInverse(nullptr, view * proj);
        fc.invViewProj = XMMatrixTranspose(invVP);
        for (UINT i = 0; i < SHADOW_CASCADE_COUNT; ++i)
            fc.shadowCascadeMatrices[i] = XMMatrixTranspose(g_shadowCascadeMatrices[i]);
        fc.previousViewProj = XMMatrixTranspose(previousViewProj);
        fc.shadowCascadeSplits = g_shadowCascadeSplits;
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
        fc.edgeAAEnabled = edgeAAEnabled ? 1u : 0u;
        fc.palmWind = palmWindFrame.wind;
        fc.palmPrimary = palmWindFrame.primary;
        fc.palmSecondary = palmWindFrame.secondary;
        fc.palmPreviousPrimary = palmWindFrame.previousPrimary;
        fc.palmPreviousSecondary = palmWindFrame.previousSecondary;
        fc.palmParams = palmWindFrame.params;
        frameConstantBuffer.CopyData(g_dx12.frameIndex, fc);

        const UINT frameSlot = g_dx12.frameIndex % FRAME_COUNT;
        ID3D12DescriptorHeap* enhancedDescHeap =
            enhancedComputeDescHeaps[frameSlot].Get();

        // A toggle can leave old history describing samples from a different
        // mode. Invalidate before uploading b5 so the shader sees the reset on
        // the first frame after the transition.
        if (svgfTemporalEnabled != svgfTemporalEnabledLastFrame) {
            svgfHistoryValid = false;
            svgfTemporalEnabledLastFrame = svgfTemporalEnabled;
        }

        // Set compute pipeline. The enhanced variant is used only when it built
        // successfully AND the caller asked for it this frame; otherwise this
        // is bit-for-bit the original path.
        const bool useEnhanced = enhancedVisualsActive && enhancedPipelineReady &&
                                 enhancedResolvePSO && enhancedDescHeap;
        enhancedResolveExecutedLastFrame = useEnhanced;
        if (useEnhanced) {
            UpdateEnhancedConstants(frameSlot);
            cmdList->SetComputeRootSignature(enhancedResolveRootSig.Get());
            cmdList->SetPipelineState(enhancedResolvePSO.Get());
        } else {
            cmdList->SetComputeRootSignature(resolveRootSig.Get());
            cmdList->SetPipelineState(resolvePSO.Get());
        }

        // Debug views inspect the latest completed history without advancing
        // it. Only normal shading commits a new ping-pong side.
        const bool svgfWillWriteHistory =
            useEnhanced && svgfTemporalEnabled && debugViewMode == 0;
        svgfTemporalExecutedLastFrame = svgfWillWriteHistory;
        UINT svgfHistoryRead = svgfHistoryPing;
        UINT svgfHistoryWrite = svgfHistoryPing ^ 1u;
        if (svgfWillWriteHistory) {
            svgfHistoryPing ^= 1;
            svgfHistoryRead = svgfHistoryPing ^ 1u;
            svgfHistoryWrite = svgfHistoryPing;
        }
        if (useEnhanced && svgfTemporalEnabled)
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

        // Set descriptor heap
        ID3D12DescriptorHeap* heaps[] = {
            useEnhanced ? enhancedDescHeap : computeDescHeap.Get() };
        cmdList->SetDescriptorHeaps(1, heaps);

        // Bind root parameters
        // b0 - frame constants
        cmdList->SetComputeRootConstantBufferView(0,
            frameConstantBuffer.GetGPUAddress(g_dx12.frameIndex));

        // b1 - light buffer (we'll upload via a temporary inline approach)
        // Actually, we reuse the mainShader's lightBuffer
        // For simplicity, create inline CBVs using the mainShader's addresses
        // We'll pass these from outside. For now, bind the descriptor table.

        // Descriptor table at root param 1 (SRVs + UAV)
        cmdList->SetComputeRootDescriptorTable(1,
            useEnhanced
                ? enhancedDescHeap->GetGPUDescriptorHandleForHeapStart()
                : computeDescHeap->GetGPUDescriptorHandleForHeapStart());

        // Dispatch (GPU-driven via ExecuteIndirect)
        UINT groupsX = (width + 7) / 8;
        UINT groupsY = (height + 7) / 8;
        if (mappedResolveDispatchArgs) {
            mappedResolveDispatchArgs->ThreadGroupCountX = groupsX;
            mappedResolveDispatchArgs->ThreadGroupCountY = groupsY;
            mappedResolveDispatchArgs->ThreadGroupCountZ = 1;
        }
        if (resolveDispatchSignature && resolveDispatchArgsBuffer) {
            cmdList->ExecuteIndirect(resolveDispatchSignature.Get(), 1, resolveDispatchArgsBuffer.Get(), 0, nullptr, 0);
        } else {
            cmdList->Dispatch(groupsX, groupsY, 1);
        }

        // Sample how much of the screen went to RT. Only meaningful when the
        // enhanced resolve actually wrote the mask this frame.
        if (useEnhanced) UpdateRayMaskStatistic(cmdList);

        // Transition SVGF write-side history back to SRV for next frame.
        if (svgfWillWriteHistory) {
            D3D12_RESOURCE_BARRIER stableToCopy[2] = {};
            for (UINT i = 0; i < 2; ++i) {
                stableToCopy[i].Type =
                    D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                stableToCopy[i].Transition.Subresource =
                    D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            }
            stableToCopy[0].Transition.pResource =
                svgfStableSurfaceCurrent.Get();
            stableToCopy[0].Transition.StateBefore =
                D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            stableToCopy[0].Transition.StateAfter =
                D3D12_RESOURCE_STATE_COPY_SOURCE;
            stableToCopy[1].Transition.pResource =
                svgfStableSurfaceHistory.Get();
            stableToCopy[1].Transition.StateBefore =
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            stableToCopy[1].Transition.StateAfter =
                D3D12_RESOURCE_STATE_COPY_DEST;
            cmdList->ResourceBarrier(2, stableToCopy);
            cmdList->CopyResource(svgfStableSurfaceHistory.Get(),
                                  svgfStableSurfaceCurrent.Get());
            std::swap(stableToCopy[0].Transition.StateBefore,
                      stableToCopy[0].Transition.StateAfter);
            std::swap(stableToCopy[1].Transition.StateBefore,
                      stableToCopy[1].Transition.StateAfter);
            cmdList->ResourceBarrier(2, stableToCopy);

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
        const UINT atrousIterationCount = std::clamp(
            svgfAtrousIterations, 1u, kSVGFAtrousMaxIterations);
        ID3D12DescriptorHeap* atrousDescHeap =
            svgfAtrousDescHeaps[frameSlot].Get();
        ID3D12DescriptorHeap* compositeDescHeap =
            svgfCompositeDescHeaps[frameSlot].Get();
        const bool atrousRan =
            useEnhanced && svgfTemporalEnabled && svgfAtrousEnabled &&
            (debugViewMode == 0 || debugViewMode == 6) &&
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
                g_dx12.device->CreateShaderResourceView(g_dx12.depthStencilBuffer.Get(), &depthSrv, h1);

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
                    g_dx12.depthStencilBuffer.Get(), &depthSrv, h7);
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
                    g_dx12.depthStencilBuffer.Get(), &depthSrv, h13);
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

        // Snapshot this frame's surface IDs for next frame's temporal validity
        // test. The resolve above has finished reading visBufferRT, so nothing
        // races the copy.
        CaptureSurfaceHistory(cmdList);

        // Preserve visibility depth before forward-only animated/alpha-tested
        // geometry modifies it. Post uses the difference as a reactive mask.
        {
            D3D12_RESOURCE_BARRIER barriers[3] = {};
            barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[0].Transition.pResource = g_dx12.depthStencilBuffer.Get();
            barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barriers[1] = barriers[0];
            barriers[1].Transition.pResource = visibilityDepthTexture.Get();
            barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            cmdList->ResourceBarrier(2, barriers);
            cmdList->CopyResource(visibilityDepthTexture.Get(),
                                  g_dx12.depthStencilBuffer.Get());
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
        cmdList->RSSetViewports(1, &g_dx12.viewport);
        cmdList->RSSetScissorRects(1, &g_dx12.scissorRect);
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
        const bool useMotion = extensionMotionVectors;
        UINT barrierCount = useMotion ? 3u : 2u;
        D3D12_RESOURCE_BARRIER barriers[3] = {};
        barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[0].Transition.pResource = outputTexture.Get();
        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[1] = barriers[0];
        barriers[1].Transition.pResource = g_dx12.depthStencilBuffer.Get();
        barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        if (useMotion) {
            barriers[2].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[2].Transition.pResource = motionTexture.Get();
            barriers[2].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            barriers[2].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barriers[2].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        }
        cmdList->ResourceBarrier(barrierCount, barriers);

        // Clear motion to zero up front. Only the passes that own a motion PSO
        // (skinned actors, viewmodel) bind the second RTV via
        // BeginMotionDraws; everything else in this pass -- terrain, water,
        // SSR, fog, light shafts -- still has single-RT PSOs, and D3D12 drops
        // a draw whose PSO render-target count disagrees with the bound
        // targets. Geometry disappearing is the visible symptom. Pixels left
        // uncovered keep the zero written here, which reads as "no motion".
        if (useMotion) {
            const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            cmdList->ClearRenderTargetView(GetMotionRTV(), zero, 0, nullptr);
        }

        // Default to colour-only so untouched passes keep working.
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = GetOutputRTV();
        D3D12_CPU_DESCRIPTOR_HANDLE dsv =
            g_dx12.dsvHeap->GetCPUDescriptorHandleForHeapStart();
        cmdList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        cmdList->RSSetViewports(1, &g_dx12.viewport);
        cmdList->RSSetScissorRects(1, &g_dx12.scissorRect);
    }

    // Bind colour + motion for draws that use an extension-motion PSO. No-op
    // when the toggle is off, so callers can bracket unconditionally.
    void BeginMotionDraws(ID3D12GraphicsCommandList* cmdList) {
        if (!extensionMotionVectors) return;
        D3D12_CPU_DESCRIPTOR_HANDLE rtvs[2] = { GetOutputRTV(), GetMotionRTV() };
        D3D12_CPU_DESCRIPTOR_HANDLE dsv =
            g_dx12.dsvHeap->GetCPUDescriptorHandleForHeapStart();
        cmdList->OMSetRenderTargets(2, rtvs, FALSE, &dsv);
    }

    // Restore colour-only for the single-RT passes that follow.
    void EndMotionDraws(ID3D12GraphicsCommandList* cmdList) {
        if (!extensionMotionVectors) return;
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = GetOutputRTV();
        D3D12_CPU_DESCRIPTOR_HANDLE dsv =
            g_dx12.dsvHeap->GetCPUDescriptorHandleForHeapStart();
        cmdList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    }

    void EndForwardExtensions(ID3D12GraphicsCommandList* cmdList) {
        const bool useMotion = extensionMotionVectors;
        UINT barrierCount = useMotion ? 3u : 2u;
        D3D12_RESOURCE_BARRIER barriers[3] = {};
        barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[0].Transition.pResource = outputTexture.Get();
        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[1] = barriers[0];
        barriers[1].Transition.pResource = g_dx12.depthStencilBuffer.Get();
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
    ID3D12Resource* GetNormalRoughnessResource() const {
        return normalRoughnessTexture.Get();
    }
    ID3D12Resource* GetVisibilityDepthResource() const {
        return visibilityDepthTexture.Get();
    }

    void PostProcess(ID3D12GraphicsCommandList* cmdList, bool allowHistory) {
        const UINT historyIndex = postFrameIndex & 1u;
        if (!validationMode && debugViewMode == 0 && bloomStrength > 0.0f)
            RenderBloom(cmdList);
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
        constants.outputWidth = width;
        constants.outputHeight = height;
        constants.exposure = validationMode ? 1.0f : exposure;
        constants.bloomStrength = validationMode ? 0.0f : bloomStrength;
        constants.vignetteStrength = validationMode ? 0.0f : vignetteStrength;
        constants.grainStrength = validationMode ? 0.0f : grainStrength;
        constants.frameIndex = postFrameIndex++;
        constants.historyValid = (temporalEffectsEnabled && !validationMode &&
            allowHistory && temporalHistoryValid) ? 1u : 0u;
        constants.taaFeedback = (temporalEffectsEnabled && !validationMode)
            ? taaFeedback : 0.0f;
        constants.motionBlurStrength = (temporalEffectsEnabled &&
            !validationMode) ? motionBlurStrength : 0.0f;
        constants.focusDistance = focusDistance;
        // Depth of field disabled. It blurred the entire game view whenever
        // parity validation was off.
        constants.aperture = 0.0f;
        constants.nearPlane = currentNearPlane;
        constants.farPlane = currentFarPlane;
        constants.debugViewMode = static_cast<UINT>(debugViewMode);
        constants.validationMode = validationMode ? 1u : 0u;
        // Exact surface correspondence is available once a history frame has
        // been captured. Suppressed in parity mode, which compares against the
        // Forward renderer and must not gain a temporal advantage.
        constants.surfaceHistoryValid =
            (surfaceHistoryValid && surfaceIDTemporalEnabled && !validationMode)
                ? 1u : 0u;
        constants.historyDebugView = historyDebugView ? 1u : 0u;
        postConstantBuffer.CopyData(g_dx12.frameIndex, constants);

        cmdList->SetComputeRootSignature(postRootSig.Get());
        cmdList->SetPipelineState(postPSO.Get());
        ID3D12DescriptorHeap* heaps[] = { postDescHeap.Get() };
        cmdList->SetDescriptorHeaps(1, heaps);
        cmdList->SetComputeRootConstantBufferView(0,
            postConstantBuffer.GetGPUAddress(g_dx12.frameIndex));
        D3D12_GPU_DESCRIPTOR_HANDLE table =
            postDescHeap->GetGPUDescriptorHandleForHeapStart();
        table.ptr += (UINT64)g_dx12.cbvSrvUavDescriptorSize * historyIndex *
                     kPostDescriptorsPerParity;
        cmdList->SetComputeRootDescriptorTable(1, table);
        cmdList->Dispatch((width + 7) / 8, (height + 7) / 8, 1);

        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        cmdList->ResourceBarrier(2, barriers);
        D3D12_RESOURCE_BARRIER depth = {};
        depth.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        depth.Transition.pResource = g_dx12.depthStencilBuffer.Get();
        depth.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        depth.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        depth.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &depth);
        temporalHistoryValid = temporalEffectsEnabled;
    }

    // Copy the resolved output to the back buffer
    void CopyToBackBuffer(ID3D12GraphicsCommandList* cmdList) {
        ID3D12Resource* backBuffer = g_dx12.renderTargets[g_dx12.frameIndex].Get();

        // Back buffer is already in RENDER_TARGET state from BeginFrame
        // Transition it to COPY_DEST
        {
            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = backBuffer;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            cmdList->ResourceBarrier(1, &barrier);
        }

        cmdList->CopyResource(backBuffer, presentTexture.Get());

        // Transition back to RENDER_TARGET for ImGui
        {
            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = backBuffer;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            cmdList->ResourceBarrier(1, &barrier);
        }
    }

    void Resize(UINT newWidth, UINT newHeight) {
        if (newWidth == width && newHeight == height) return;
        width = newWidth;
        height = newHeight;

        visBufferRT.Reset();
        visBufferHistory.Reset();
        surfaceHistoryValid = false;
        outputTexture.Reset();
        presentTexture.Reset();
        motionTexture.Reset();
        motionRtvHeap.Reset();
        normalRoughnessTexture.Reset();
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
        // Screen-sized like the rest; without this the enhanced resolve would
        // keep writing its mask at the old dimensions after a window resize.
        rayMaskTexture.Reset();
        rayMaskReadback.Reset();

        CreateVisBufferRT();
        CreateOutputTexture();
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
        UpdatePostDescriptors();
        UpdateExposureDescriptors();
    }

private:
    bool MotionVectorsRequired() const {
        // TAA owns post-process history, but SVGF independently needs the
        // visibility motion buffer to reproject reflection history. Capture
        // mode deliberately disables TAA, so tying this data to the TAA switch
        // pins SVGF history to screen space as soon as the camera moves.
        return temporalEffectsEnabled ||
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

        // Surface-ID history: last frame's instance/primitive IDs.
        //
        // A straight copy of visBufferRT rather than a second render target
        // with alternating RTVs -- the visibility pass and every consumer keep
        // referring to one resource, and the copy is ~8 MB of bandwidth once
        // per frame. Cheaper in complexity than ping-ponging the RTV, and the
        // GPU copy does not stall anything.
        desc.Flags = D3D12_RESOURCE_FLAG_NONE;
        if (FAILED(g_dx12.device->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                IID_PPV_ARGS(&visBufferHistory)))) {
            std::cerr << "Failed to create visibility history buffer\n";
            return false;
        }
        surfaceHistoryValid = false;

        return true;
    }

    // Copies this frame's IDs into the history texture, for next frame's
    // temporal validity test. Runs after the resolve has consumed the current
    // vis buffer, so the copy never races a reader.
    void CaptureSurfaceHistory(ID3D12GraphicsCommandList* cmdList) {
        if (!visBufferRT || !visBufferHistory) return;

        D3D12_RESOURCE_BARRIER barriers[2] = {};
        for (UINT i = 0; i < 2; ++i) {
            barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[i].Transition.Subresource =
                D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        }
        barriers[0].Transition.pResource = visBufferRT.Get();
        barriers[0].Transition.StateBefore =
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barriers[1].Transition.pResource = visBufferHistory.Get();
        barriers[1].Transition.StateBefore =
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        cmdList->ResourceBarrier(2, barriers);

        cmdList->CopyResource(visBufferHistory.Get(), visBufferRT.Get());

        std::swap(barriers[0].Transition.StateBefore,
                  barriers[0].Transition.StateAfter);
        std::swap(barriers[1].Transition.StateBefore,
                  barriers[1].Transition.StateAfter);
        cmdList->ResourceBarrier(2, barriers);
        surfaceHistoryValid = true;
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
        hr = g_dx12.device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr,
            IID_PPV_ARGS(&presentTexture));
        if (FAILED(hr)) {
            std::cerr << "Failed to create VB present texture" << std::endl;
            return false;
        }

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
            g_dx12.depthStencilBuffer->GetDesc();
        hr = g_dx12.device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &depthSnapshotDesc,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
            IID_PPV_ARGS(&visibilityDepthTexture));
        if (FAILED(hr)) return false;

        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        for (UINT i = 0; i < 2; ++i) {
            hr = g_dx12.device->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                IID_PPV_ARGS(&historyTextures[i]));
            if (FAILED(hr)) return false;
        }

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
        // SVGF writes a separate persistent key so destruction can regroup the
        // same source triangles without invalidating their temporal identity.
        desc.Format = DXGI_FORMAT_R32G32_UINT;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        hr = g_dx12.device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&svgfStableSurfaceCurrent));
        if (FAILED(hr)) return false;
        desc.Flags = D3D12_RESOURCE_FLAG_NONE;
        hr = g_dx12.device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
            IID_PPV_ARGS(&svgfStableSurfaceHistory));
        if (FAILED(hr)) return false;
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
        return SUCCEEDED(g_dx12.device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
            IID_PPV_ARGS(&bloomTexture)));
    }

    bool CreateStructuredBuffers() {
        auto CreateDefaultAndUpload = [](UINT64 size,
                                          ComPtr<ID3D12Resource>& defaultBuf,
                                          ComPtr<ID3D12Resource>& uploadBuf) -> bool {
            D3D12_HEAP_PROPERTIES defaultHeap = {};
            defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
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

            HRESULT hr = g_dx12.device->CreateCommittedResource(
                &defaultHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&defaultBuf));
            if (FAILED(hr)) return false;

            hr = g_dx12.device->CreateCommittedResource(
                &uploadHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&uploadBuf));
            if (FAILED(hr)) return false;

            return true;
        };

        if (!CreateDefaultAndUpload(VB_MAX_DRAW_CALLS * sizeof(VBDrawCallData),
                                     drawCallBuffer, drawCallUpload))
            return false;

        if (!CreateDefaultAndUpload(VB_MAX_VERTICES * sizeof(VBPackedVertex),
                                     vertexDataBuffer, vertexDataUpload))
            return false;

        if (!CreateDefaultAndUpload(VB_MAX_INDICES * sizeof(UINT),
                                     indexDataBuffer, indexDataUpload))
            return false;

        if (!CreateDefaultAndUpload(VB_MAX_TRIANGLES * sizeof(UINT),
                                     stableTriangleDataBuffer,
                                     stableTriangleDataUpload))
            return false;

        if (!CreateDefaultAndUpload(VB_CLUSTER_COUNT * sizeof(VBClusterData),
                                     clusterDataBuffer, clusterDataUpload))
            return false;

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

    bool CreateComputeDescriptorHeap() {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.NumDescriptors = 86;
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        HRESULT hr = g_dx12.device->CreateDescriptorHeap(
            &heapDesc, IID_PPV_ARGS(&computeDescHeap));
        if (FAILED(hr)) {
            std::cerr << "Failed to create visibility compute descriptor heap\n";
            return false;
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
        D3D12_ROOT_PARAMETER visParams[4] = {};

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

        D3D12_STATIC_SAMPLER_DESC alphaSampler = {};
        alphaSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        alphaSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        alphaSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        alphaSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        alphaSampler.MaxLOD = D3D12_FLOAT32_MAX;
        alphaSampler.ShaderRegister = 0;
        alphaSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC visRootSigDesc = {};
        visRootSigDesc.NumParameters = 4;
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
        psoDesc.RasterizerState.FrontCounterClockwise = FALSE;
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
            UINT  svgfPad2;
        } constants;
        static_assert(sizeof(EnhancedConstants) == 80,
                      "EnhancedVisualsBuffer C++ mirror is out of sync");
        constants.rtShadows = enhancedRTShadowsActive ? 1u : 0u;
        constants.rayClassify = enhancedRayClassifyActive ? 1u : 0u;
        constants.shadowRayLength = enhancedShadowRayLength;
        constants.confidenceThreshold = enhancedConfidenceThreshold;
        constants.rtReflections = enhancedRTReflectionsActive ? 1u : 0u;
        constants.reflectionRayLength = enhancedReflectionRayLength;
        constants.reflectionRoughnessCut = enhancedReflectionRoughnessCut;
        // Rotates the sampling sequence so consecutive frames draw different
        // samples; this is the variance a temporal denoiser resolves.
        constants.frameIndex = enhancedReflectionFrameCounter++;
        constants.reflectionOcclusion = enhancedReflectionOcclusion;
        constants.reflectionClassify = enhancedReflectionClassifyActive ? 1u : 0u;
        constants.reflectionConfidenceCut = enhancedReflectionConfidenceCut;
        constants.probeMissGI = enhancedProbeMissGIActive ? 1u : 0u;
        constants.svgfTemporalEnable = svgfTemporalEnabled ? 1u : 0u;
        constants.svgfMaxAccum = svgfMaxAccumFrames;
        constants.svgfAtrousEnable = svgfAtrousEnabled ? 1u : 0u;
        constants.svgfAtrousIters = std::clamp(
            svgfAtrousIterations, 1u, kSVGFAtrousMaxIterations);
        constants.svgfHistoryValid = svgfHistoryValid ? 1u : 0u;
        constants.probeMissGIStrength = enhancedProbeMissGIStrength;
        constants.hitGeometryCount = hitGeometryCount;
        constants.svgfPad2 = 0u;
        const UINT64 constantOffset =
            static_cast<UINT64>(frameSlot) * 256ull;
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
    void CreateEnhancedResolvePipeline(const std::string& csCode,
                                       const D3D12_DESCRIPTOR_RANGE* baseRanges,
                                       const D3D12_STATIC_SAMPLER_DESC* samplers) {
        enhancedPipelineReady = false;
        if (!ShaderCacheDX12::DxcAvailable()) {
            std::cout << "Enhanced visuals: dxcompiler.dll unavailable\n";
            return;
        }

        // Prepend the define rather than passing -D so the cache key (which
        // hashes the source text) separates the two variants automatically.
        const std::string enhancedSource =
            "#define SGE_ENHANCED_VISUALS 1\n" + csCode;

        const std::wstring shaderDirectory =
            ShaderCacheDX12::ExecutableDirectory() + L"shaders";
        ComPtr<ID3DBlob> csBlob;
        std::string errors;
        if (!ShaderCacheDX12::CompileCachedDXC(
                enhancedSource, L"visbuf_resolve_cs.hlsl", L"main", L"cs_6_5",
                shaderDirectory, &csBlob, &errors)) {
            std::cerr << "Enhanced visuals: resolve compile failed\n";
            if (!errors.empty()) {
                std::cerr << errors << std::endl;
                std::ofstream log("enhanced_resolve_shader_error.log",
                                  std::ios::trunc);
                log << errors;
            }
            return;
        }

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
        D3D12_DESCRIPTOR_RANGE ranges[16] = {};
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

        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor.ShaderRegister = 0;
        params[0].Descriptor.RegisterSpace = 0;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = _countof(ranges);
        params[1].DescriptorTable.pDescriptorRanges = ranges;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rootSigDesc = {};
        rootSigDesc.NumParameters = 2;
        rootSigDesc.pParameters = params;
        rootSigDesc.NumStaticSamplers = 2;
        rootSigDesc.pStaticSamplers = samplers;

        ComPtr<ID3DBlob> sigBlob, sigError;
        if (FAILED(D3D12SerializeRootSignature(&rootSigDesc,
                D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &sigError))) {
            if (sigError)
                std::cerr << "Enhanced resolve root sig: "
                          << (const char*)sigError->GetBufferPointer() << "\n";
            return;
        }
        if (FAILED(g_dx12.device->CreateRootSignature(
                0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(),
                IID_PPV_ARGS(&enhancedResolveRootSig))))
            return;

        D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.pRootSignature = enhancedResolveRootSig.Get();
        psoDesc.CS = { csBlob->GetBufferPointer(), csBlob->GetBufferSize() };
        if (FAILED(g_dx12.device->CreateComputePipelineState(
                &psoDesc, IID_PPV_ARGS(&enhancedResolvePSO)))) {
            std::cerr << "Enhanced visuals: resolve PSO creation failed\n";
            enhancedResolveRootSig.Reset();
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

    // Builds the enhanced heap: a copy of the default resolve heap's 86
    // descriptors, then TLAS / ray mask / enhanced constants appended.
    //
    // Updates just the current frame heap's SVGF slots (89-93). Normal shading
    // supplies previous/current ping indices; read-only debug views instead
    // keep the latest completed side bound without advancing history.
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
                svgfHistoryColor[readIndex].Get(), &srv, h);
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
                svgfHistoryMoments[readIndex].Get(), &srv, h);
        }

        // [91] t82 - vis buffer history (static, but refresh for safety)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
            srv.Format = DXGI_FORMAT_R32G32_UINT;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1;
            D3D12_CPU_DESCRIPTOR_HANDLE h = handle;
            h.ptr += (UINT64)descSize * 91;
            g_dx12.device->CreateShaderResourceView(
                visBufferHistory.Get(), &srv, h);
        }

        // [92] u4 - svgf colour write (current frame's ping)
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
            uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            D3D12_CPU_DESCRIPTOR_HANDLE h = handle;
            h.ptr += (UINT64)descSize * 92;
            g_dx12.device->CreateUnorderedAccessView(
                svgfHistoryColor[writeIndex].Get(), nullptr, &uav, h);
        }

        // [93] u5 - svgf moments write
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
            uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            D3D12_CPU_DESCRIPTOR_HANDLE h = handle;
            h.ptr += (UINT64)descSize * 93;
            g_dx12.device->CreateUnorderedAccessView(
                svgfHistoryMoments[writeIndex].Get(), nullptr, &uav, h);
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
            heapDesc.NumDescriptors = 99;
            heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
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
                static_cast<UINT64>(frameSlot) * 256ull;
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
                svgfHistoryColor[svgfHistoryPing].Get(), &srv, h);
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
                svgfHistoryMoments[svgfHistoryPing].Get(), &srv, h);
        }

        // [91] t82 - vis buffer history (SRV, surface-ID validation)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
            srv.Format = DXGI_FORMAT_R32G32_UINT;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1;
            D3D12_CPU_DESCRIPTOR_HANDLE h = handle;
            h.ptr += (UINT64)descSize * 91;
            g_dx12.device->CreateShaderResourceView(
                visBufferHistory.Get(), &srv, h);
        }

        // [92] u4 - svgf colour write (UAV, write side of ping-pong)
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
            uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            D3D12_CPU_DESCRIPTOR_HANDLE h = handle;
            h.ptr += (UINT64)descSize * 92;
            g_dx12.device->CreateUnorderedAccessView(
                svgfHistoryColor[svgfHistoryPing ^ 1u].Get(), nullptr, &uav, h);
        }

        // [93] u5 - svgf moments write (UAV, write side of ping-pong)
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
            uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            D3D12_CPU_DESCRIPTOR_HANDLE h = handle;
            h.ptr += (UINT64)descSize * 93;
            g_dx12.device->CreateUnorderedAccessView(
                svgfHistoryMoments[svgfHistoryPing ^ 1u].Get(), nullptr, &uav, h);
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
            srv.Buffer.NumElements = VB_MAX_TRIANGLES;
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
                svgfStableSurfaceHistory.Get(), &srv, h);
        }

        // [97] u7 - stable key emitted by this enhanced resolve.
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
            uav.Format = DXGI_FORMAT_R32G32_UINT;
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            D3D12_CPU_DESCRIPTOR_HANDLE h = handle;
            h.ptr += (UINT64)descSize * 97;
            g_dx12.device->CreateUnorderedAccessView(
                svgfStableSurfaceCurrent.Get(), nullptr, &uav, h);
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
        enhancedHeapTLASAddresses[frameSlot] = enhancedTLASAddress;
    }

public:
    // Maximum hit-geometry entries. One per BLAS geometry, not per instance,
    // matching how DXRScene emits hit records.
    static const UINT VB_MAX_HIT_GEOMETRY = 4096;
    // Byte stride of one hit-geometry entry. Must equal both
    // sizeof(DXRScene::HitGeometryData) and the shader's HitGeometry; asserted
    // in UploadHitGeometry, where the real type is visible.
    //
    // 9 x 4 bytes: vertexOffset, indexOffset, hasIndices, materialID, valid,
    // fallbackColor[3], hasFallbackColor.
    static const UINT kHitGeometryStride = 36;

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
        enhancedTLASAddress = tlasAddress;
        const UINT frameSlot = g_dx12.frameIndex % FRAME_COUNT;
        // The current frame slot is idle by the time its allocator is reused,
        // so only its descriptors may be rewritten. Other slots refresh when
        // they become current rather than while the GPU may still read them.
        if (!enhancedComputeDescHeaps[frameSlot] ||
            enhancedHeapTLASAddresses[frameSlot] != enhancedTLASAddress)
            RefreshEnhancedDescriptors(frameSlot);
        enhancedVisualsActive = wantActive;
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

    bool CreateResolvePipeline() {
        // Read and compile compute shader
        std::ifstream csFile("shaders/visbuf_resolve_cs.hlsl");
        if (!csFile.is_open()) {
            std::cerr << "Failed to open visbuf_resolve_cs.hlsl" << std::endl;
            return false;
        }

        std::stringstream csSS;
        csSS << csFile.rdbuf();
        std::string csCode = csSS.str();

        UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
        compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
        compileFlags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

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

        D3D12_DESCRIPTOR_RANGE ranges[3] = {};
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

        D3D12_ROOT_PARAMETER resolveParams[2] = {};

        // b0 - frame constants (root CBV)
        resolveParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        resolveParams[0].Descriptor.ShaderRegister = 0;
        resolveParams[0].Descriptor.RegisterSpace = 0;
        resolveParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        // Descriptor table
        resolveParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        resolveParams[1].DescriptorTable.NumDescriptorRanges = 3;
        resolveParams[1].DescriptorTable.pDescriptorRanges = ranges;
        resolveParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        // Static samplers
        D3D12_STATIC_SAMPLER_DESC staticSamplers[2] = {};

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

        D3D12_ROOT_SIGNATURE_DESC resolveRootSigDesc = {};
        resolveRootSigDesc.NumParameters = 2;
        resolveRootSigDesc.pParameters = resolveParams;
        resolveRootSigDesc.NumStaticSamplers = 2;
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
        CreateEnhancedResolvePipeline(csCode, ranges, staticSamplers);
        CreateSVGFAtrousPipeline();

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
            g_dx12.device->CreateShaderResourceView(g_dx12.depthStencilBuffer.Get(), &srvDesc, cpuHandle);
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
            srvDesc.Buffer.NumElements = VB_MAX_VERTICES;
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
            srvDesc.Buffer.NumElements = VB_MAX_INDICES;
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
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC environment = {};
            environment.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            environment.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            environment.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            environment.Texture2D.MipLevels = 1;
            g_dx12.device->CreateShaderResourceView(nullptr, &environment, cpuHandle);
            cpuHandle.ptr += descSize;
        }

        // [73] t73 - split-sum GGX BRDF integration LUT.
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC brdf = {};
            brdf.Format = DXGI_FORMAT_R32G32_FLOAT;
            brdf.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            brdf.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            brdf.Texture2D.MipLevels = 1;
            g_dx12.device->CreateShaderResourceView(nullptr, &brdf, cpuHandle);
            cpuHandle.ptr += descSize;
        }

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
        // These will be created in UpdateLightDescriptors
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
        heap.NumDescriptors = (VB_BLOOM_MAX_MIPS * 2u - 1u) * 2u;
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
    }

    void RenderBloom(ID3D12GraphicsCommandList* cmdList) {
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
            constants.sourceWidth =
                mip == 0 ? width : mipSize(bloomWidth, mip - 1u);
            constants.sourceHeight =
                mip == 0 ? height : mipSize(bloomHeight, mip - 1u);
            constants.destinationWidth = mipSize(bloomWidth, mip);
            constants.destinationHeight = mipSize(bloomHeight, mip);
            constants.threshold = mip == 0 ? 1.0f : 0.0f;
            constants.softKnee = 0.5f;
            constants.scatter = 0.72f;
            dispatchPass(mip, mip, constants, bloomDownsamplePSO.Get());
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
        // t0..t9: the original eight plus current/previous surface IDs.
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 10;
        ranges[0].BaseShaderRegister = 0;
        ranges[0].OffsetInDescriptorsFromTableStart = 0;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 2;
        ranges[1].BaseShaderRegister = 0;
        ranges[1].OffsetInDescriptorsFromTableStart = 10;

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
        D3D12_STATIC_SAMPLER_DESC lutSampler = {};
        lutSampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        lutSampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        lutSampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        lutSampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        lutSampler.ShaderRegister = 0;
        lutSampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        lutSampler.MinLOD = 0.0f;
        lutSampler.MaxLOD = D3D12_FLOAT32_MAX;
        rootDesc.NumStaticSamplers = 1;
        rootDesc.pStaticSamplers = &lutSampler;
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

        D3D12_DESCRIPTOR_HEAP_DESC heap = {};
        heap.NumDescriptors = kPostDescriptorsPerParity * 2u;
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
        for (UINT parity = 0; parity < 2; ++parity) {
            D3D12_CPU_DESCRIPTOR_HANDLE handle =
                postDescHeap->GetCPUDescriptorHandleForHeapStart();
            handle.ptr += (SIZE_T)descriptorSize * parity *
                          kPostDescriptorsPerParity;

            D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
            srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1;
            g_dx12.device->CreateShaderResourceView(outputTexture.Get(), &srv, handle);
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
                g_dx12.depthStencilBuffer.Get(), &depthSrv, handle);
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
            bloomSrv.Texture2D.MipLevels = 1;
            g_dx12.device->CreateShaderResourceView(
                bloomTexture.Get(), &bloomSrv, handle);
            handle.ptr += descriptorSize;
            // t8/t9 - current and previous surface IDs, for exact temporal
            // validity. Both are R32G32_UINT: instance in .x, primitive in .y.
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
                visBufferHistory.Get(), &idSrv, handle);
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
        if (!computeDescHeap) return;
        D3D12_CPU_DESCRIPTOR_HANDLE handle =
            computeDescHeap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(g_dx12.cbvSrvUavDescriptorSize) * 72u;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = environmentResource
            ? environmentResource->GetDesc().Format : DXGI_FORMAT_R32G32B32A32_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = environmentResource
            ? environmentResource->GetDesc().MipLevels : 1;
        g_dx12.device->CreateShaderResourceView(environmentResource, &srv, handle);
        handle.ptr += g_dx12.cbvSrvUavDescriptorSize;
        D3D12_SHADER_RESOURCE_VIEW_DESC brdf = {};
        brdf.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        brdf.Format = DXGI_FORMAT_R32G32_FLOAT;
        brdf.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        brdf.Texture2D.MipLevels = 1;
        g_dx12.device->CreateShaderResourceView(brdfResource, &brdf, handle);
    }

    // Update the shadow map SRV in the compute descriptor heap
    void UpdateShadowMapDescriptor(ID3D12Resource* shadowMapResource) {
        if (!computeDescHeap) return;
        
        UINT descSize = g_dx12.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = computeDescHeap->GetCPUDescriptorHandleForHeapStart();
        cpuHandle.ptr += 2 * descSize; // slot [2] = t2

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2DArray.MipLevels = 1;
        srvDesc.Texture2DArray.ArraySize = SHADOW_CASCADE_COUNT;

        if (shadowMapResource) {
            g_dx12.device->CreateShaderResourceView(shadowMapResource, &srvDesc, cpuHandle);
        } else {
            g_dx12.device->CreateShaderResourceView(nullptr, &srvDesc, cpuHandle);
        }
    }
};

#endif // VISIBILITY_BUFFER_DX12_H
