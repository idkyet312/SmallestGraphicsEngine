#ifndef SHADER_DX12_H
#define SHADER_DX12_H

#include "RenderViewDX12.h"

#include "ShaderCacheDX12.h"
#include "DX12Core.h"
#include "BindlessHeapDeviceDX12.h"
#include "DXRProbeLayout.h"
#include "MSAADX12.h"
#include "SceneGraph.h"   // SceneMaterial: caches its descriptor slot (see SetObjectMaterial)
#include "PalmWindGPU.h"
#include <fstream>
#include <sstream>
#include <cstdio>
#include <array>
#include <algorithm>
#include <cstddef>

inline constexpr UINT SHADOW_CASCADE_COUNT = 3;
inline std::array<XMMATRIX, SHADOW_CASCADE_COUNT> g_shadowCascadeMatrices = {
    XMMatrixIdentity(), XMMatrixIdentity(), XMMatrixIdentity()
};
inline XMFLOAT4 g_shadowCascadeSplits = { 18.0f, 55.0f, 180.0f, 0.0f };
// Per-cascade shadow texel size in world units and light-space depth range,
// filled by ComputeCascadeMatrices. Shaders scale their depth bias by these so
// one bias constant cannot under-bias the wide far cascade (terrain acne
// bands) while over-biasing the tight near one.
inline XMFLOAT4 g_shadowCascadeTexelWorld = { 0.05f, 0.05f, 0.05f, 0.0f };
inline XMFLOAT4 g_shadowCascadeDepthRange = { 300.0f, 300.0f, 300.0f, 0.0f };

// Spot shadow atlas, filled by ShadowMapDX12::RenderSpotShadows each frame.
//
// Declared here rather than in ShadowMapDX12.h because that header includes
// ForwardRenderer.h, and the light-add functions there have to tag lights with
// an atlas slice. Putting the constants in the header both already include
// keeps the dependency one-directional.
//
// Vehicle headlights and the enemy dropship's searchlight cast; the player's
// own flashlight does not -- it sits at the eye, so everything it lights is
// already facing the camera and its shadows would fall almost entirely behind
// the geometry casting them, paying a full depth pass for a few edge pixels.
//
// One slice per CASTER, not per lamp: a Humvee's two headlights sit 1.5 m
// apart on a shared forward axis, so one frustum covering both reproduces the
// occlusion either would compute alone. Slices: 0 primary Humvee, 1 secondary
// Humvee, 2 dropship searchlight -- assigned in arrival order, so a level
// without the second Humvee gives its slice to whatever registers next.
inline constexpr UINT SPOT_SHADOW_COUNT = 3;
// 2048 against the sun's 4096: these frusta cover a ~40 m cone rather than the
// whole cascade box, so a texel already lands far denser on world geometry.
// Costs 3 x 2048^2 x 4 = 48 MB.
inline constexpr UINT SPOT_SHADOW_SIZE = 2048;
// Written into PointLightData::spotShadowIndex for a light that casts nothing.
// Shaders test against this before touching the atlas.
inline constexpr int SPOT_SHADOW_NONE = -1;
inline std::array<XMMATRIX, SPOT_SHADOW_COUNT> g_spotShadowMatrices = {
    XMMatrixIdentity(), XMMatrixIdentity(), XMMatrixIdentity()
};
// How many slices actually hold a caster this frame. Slices past this are
// stale depth from an earlier frame, so shaders must not sample them.
inline UINT g_spotShadowActiveCount = 0;

// Constant buffer structures (must be 256-byte aligned for DX12)
struct alignas(256) MatrixBufferDX12 {
    XMMATRIX model;
    XMMATRIX view;
    XMMATRIX projection;
    XMMATRIX lightSpaceMatrix;
    XMMATRIX modelView;
    XMMATRIX modelViewProjection;
    XMMATRIX previousViewProjection;
    XMFLOAT4 palmWind;
    XMFLOAT4 palmPrimary;
    XMFLOAT4 palmSecondary;
    XMFLOAT4 palmPreviousPrimary;
    XMFLOAT4 palmPreviousSecondary;
    XMFLOAT4 palmParams;
    XMFLOAT4 palmRoot;
    XMMATRIX previousModel;
};

inline float g_currentModelMaxScale = 1.0f;

// Global multiplier on every authored emissive map, driven from the editor's
// Lighting panel. A single global rather than a per-material field: it exists to
// balance emissive against the rest of the frame's exposure, which is a scene
// decision, and materials keep their own emissiveFactor for relative brightness.
// 1.0 leaves assets exactly as authored.
inline float g_emissiveIntensity = 1.0f;

struct alignas(256) LightBufferDX12 {
    XMFLOAT3 lightPos;
    int lightType;
    XMFLOAT3 lightColor;
    float constant;
    float linear;
    float quadratic;
    float ambientStrength;
    float specularStrength;
    int shininess;
    float shadowBias;
    int enableShadows;
    // 1/shadow-map-size, so the PCF loop needn't call GetDimensions per pixel.
    float shadowTexelSize;
    float ambientLightingIntensity;
    // Scales every authored emissive map in the scene. Lives on the per-frame
    // light buffer rather than the per-draw object buffer because it is one
    // global knob, not a material property -- see g_emissiveIntensity.
    float emissiveIntensity;
};

struct alignas(256) CameraBufferDX12 {
    XMFLOAT3 viewPos;
    float padding;
};

struct alignas(256) ObjectBufferDX12 {
    XMFLOAT3 objectColor;
    float useTexture;      // > 0.5 enabled
    float metalness;
    float roughness;
    float useNormalMap;    // > 0.5 enabled
    float metalRoughMode;  // 0=none, 1=glTF packed, 2=roughness-only
    float opacity;
    float smokeMode = 0.0f; // > 0.5: unlit soft sprite (alpha = opacity*texAlpha)
    float alphaCut = 0.0f;  // -1: alpha blend, 1: foliage, 2: luminance, 3: hard cutout
    // Clip threshold for alphaCut modes 1 and 3.
    float alphaCutoff = 0.20f;
    float ambientScale = 1.0f;
    float occlusionStrength = 0.0f;
    float normalYSign = 1.0f;
    float viewFillStrength = 0.0f;
    // Normal-map dimensions, so the shader's minification fade needn't call
    // GetDimensions per pixel.
    float normalTexW = 1.0f;
    float normalTexH = 1.0f;
    float specularScale = 1.0f;
    float materialType = 0.0f; // 0=ordinary, 1=pool water, 2=ocean, 8/9=glass
    float materialTime = 0.0f; // animated procedural materials
    // > 0.5: add emissiveMap * emissiveFactor after lighting. Takes one of the
    // three padding floats that filled the gap below, so the uint4 that follows
    // keeps its byte-96 start.
    float useEmissiveMap = 0.0f;
    // Binocular see-through for the first-person weapon. 0 leaves the draw
    // fully opaque; above 0 the pixel shader fades it by view distance, so the
    // parts nearest the eye clear the most. Takes the first of the two floats
    // that padded the gap below, which keeps the uint4 on its byte-96 register.
    float viewmodelSeeThrough = 0.0f;
    // HLSL starts uint4 values on a fresh 16-byte cbuffer register. Adding
    // alphaCutoff moved materialTime to byte 80, so the bindless indices now
    // begin at byte 96 rather than immediately after it.
    float bindlessPadding = 0.0f;
    UINT bindlessTextureIndices[4] = {
        BINDLESS_FALLBACK_WHITE, BINDLESS_FALLBACK_NORMAL,
        BINDLESS_FALLBACK_METALROUGH, BINDLESS_FALLBACK_BLACK
    };
    // Tint and intensity for the emissive map. After the uint4 rather than
    // before it: only three floats were free ahead of byte 96, and splitting a
    // float3 across the boundary would have shifted the indices the shader
    // reads at a fixed offset.
    XMFLOAT3 emissiveFactor = { 0.0f, 0.0f, 0.0f };
    float emissivePadding = 0.0f;
    // Shape of the viewmodel see-through fade, live-tunable from the debug UI.
    // Appended past the emissive tail rather than squeezed in ahead of the
    // bindless indices: these start a fresh 16-byte register at byte 128, so
    // neither guarded offset above can shift. Defaults match the weapon's own
    // measured extent -- see ViewmodelSeeThroughAlpha in the pixel shader.
    float seeThroughNear = 0.30f;   // metres; receiver at the eye
    float seeThroughFar = 1.05f;    // metres; muzzle at arm's length
    float seeThroughNearAlpha = 0.25f;  // coverage kept by the nearest geometry
    // Glass Fresnel response, used when materialType selects glass. Takes the
    // float that padded the register above, so nothing shifts.
    float glassClarity = 0.16f;   // opacity looking straight through
    float glassGrazing = 0.86f;   // opacity at glancing angles; keep under 1
    // The scope camera's own focal lengths (1/tan(fov/2) per axis), so the
    // sniper lens can project a view ray into the scope target without knowing
    // that camera's FOV. Two values rather than one ratio because the scope
    // renders a square target while the main view is wide: a single scale
    // cannot be correct on both axes. Takes two of the padding floats above,
    // so no existing offset moves. Zero means "no scope camera this frame" and
    // the lens falls back to the main projection, showing what surrounds it.
    float sniperScopeFocalX = 0.0f;
    float sniperScopeFocalY = 0.0f;
    float sniperScopeUVRotation = 0.0f;
    // Scope orientation debug key. Opens a new 16-byte register at byte 160
    // rather than squeezing into the one above, so every offset asserted
    // below keeps its value. Non-zero draws direction markers over the lens;
    // see the isSniperGlass branch in clustered_dx12_ps.hlsl.
    float sniperScopeDebugMode = 0.0f;
    // Tangent of the half-angle the scope glass subtends from the eye, so the
    // lens can map its own disc onto the whole scope target instead of assuming
    // the glass covers exactly the scope FOV. Takes the first debug padding
    // float, so no offset asserted above moves. Zero means "not measured" and
    // the lens falls back to the focal-length mapping.
    float sniperLensHalfTangent = 0.0f;
    XMFLOAT2 sniperAimTangent{};
    XMFLOAT4 sniperLensAperture{}; // centre tangent.xy, live opacity, reserved
};

static_assert(offsetof(ObjectBufferDX12, bindlessTextureIndices) == 96,
              "ObjectBufferDX12 must match the bindless HLSL cbuffer layout");
static_assert(offsetof(ObjectBufferDX12, emissiveFactor) == 112,
              "emissiveFactor must follow the bindless indices in the HLSL cbuffer");
static_assert(offsetof(ObjectBufferDX12, seeThroughNear) == 128,
              "see-through tuning must start its own 16-byte cbuffer register");
static_assert(offsetof(ObjectBufferDX12, glassClarity) == 140,
              "glass tuning must follow seeThroughNearAlpha in the HLSL cbuffer");
static_assert(offsetof(ObjectBufferDX12, sniperScopeFocalX) == 148,
              "scope focal lengths must take the first glass padding floats");
static_assert(offsetof(ObjectBufferDX12, sniperScopeUVRotation) == 156,
              "scope UV rotation must close the glass cbuffer register");
static_assert(offsetof(ObjectBufferDX12, sniperScopeDebugMode) == 160,
              "scope debug must open a new 16-byte register, not shift the "
              "glass register above it");
static_assert(offsetof(ObjectBufferDX12, sniperLensHalfTangent) == 164,
              "lens half-angle must take the first debug padding float so the "
              "asserted offsets above keep their values");
static_assert(sizeof(ObjectBufferDX12) % 16 == 0,
              "ObjectBufferDX12 must stay 16-byte aligned; HLSL re-pads to 16 "
              "and C++ does not, so a short tail silently desyncs the layout");

// A light in the punctual list. Spotlights ride the same array rather than a
// buffer of their own: the flashlight is the only one, and a second cbuffer
// would mean a root-signature slot plus a matching struct in all five shaders
// that read this one. An all-zero spotDirection means "omnidirectional", which
// is what every existing light already writes by zero-initialising.
struct PointLightDataDX12 {
    XMFLOAT3 position;
    float radius;
    XMFLOAT3 color;
    float intensity;
    // Cone axis, unit length. Zero for a plain point light.
    XMFLOAT3 spotDirection;
    // cos of the half-angle where the cone reaches full brightness...
    float spotCosInner;
    // ...and where it falls to nothing. Ignored while spotDirection is zero.
    float spotCosOuter;
    // Atlas slice this light samples for occlusion, or -1 for no shadow. Taken
    // out of the old spotPadding, so the 64-byte layout below is unchanged.
    int spotShadowIndex;
    XMFLOAT2 spotPadding;
};
// Four float4 rows. HLSL packs cbuffer arrays on 16-byte boundaries, so this
// must stay a multiple of 16 or every light past the first reads shifted data
// in all five shaders that declare a matching PointLightData.
static_assert(sizeof(PointLightDataDX12) == 64,
              "PointLightData must match the HLSL copies in clustered_dx12_ps, "
              "clustered_ps, simple_ps, ddgi_update_cs and visbuf_resolve_cs");

struct alignas(256) PointLightsBufferDX12 {
    int numPointLights;
    // How many spot atlas slices hold live depth this frame. Lights carrying a
    // spotShadowIndex at or above this are sampling a stale slice, so shaders
    // treat them as unshadowed rather than reading last frame's depth.
    int spotShadowCount;
    float padding2;
    float padding3;
    PointLightDataDX12 lights[64];
    // World -> light clip for each atlas slice, transposed on upload like every
    // other matrix that crosses into HLSL. Trails the light array so the
    // existing offsets of numPointLights and lights[] are unchanged.
    XMMATRIX spotShadowMatrices[SPOT_SHADOW_COUNT];
};

// Projected impact decal: bullet holes and scorch marks stamped onto whatever
// the round hit. Oriented by the surface normal, so the projection is a disc in
// the surface plane rather than an axis-aligned box.
struct ImpactDecalDataDX12 {
    XMFLOAT3 position;
    float radius;
    XMFLOAT3 normal;
    // 0..1. Fades the cutout near the end of its lifetime.
    float strength;
};

struct alignas(256) ImpactDecalsBufferDX12 {
    int numDecals;
    float cutoutsEnabled;
    XMFLOAT2 decalPadding;
    ImpactDecalDataDX12 decals[64];
};

struct alignas(256) DDGIBufferDX12 {
    XMFLOAT3 probeGridOrigin;
    float probeSpacing;
    
    int probeCountX;
    int probeCountY;
    int probeCountZ;
    float maxRayDistance;
    
    float normalBias;
    float viewBias;
    float irradianceGamma;
    float giIntensity;
    
    int irradianceTexWidth;
    int irradianceTexHeight;
    int visibilityTexWidth;
    int visibilityTexHeight;
    
    int ddgiEnabled;
    int sparseProbeCount;
    int sparseCellCount;
    float sparseCellSize;
};

// 9 L2 spherical-harmonic coefficients (RGB) approximating diffuse sky
// irradiance from an equirectangular HDRI, cosine-lobe pre-convolved so the
// shader evaluates them with a flat dot product. float4 padding keeps each
// entry 16-byte aligned for HLSL cbuffer packing.
struct alignas(256) SHBufferDX12 {
    XMFLOAT4 shCoeffs[9];
    float skyIntensity;
    float shPadding[3];
};

struct alignas(256) MeshDrawBufferDX12 {
    UINT vertexCount;
    UINT indexCount;
    UINT indexed;
    UINT firstMeshlet;
    UINT meshletCount;
    // Bit 0 enables HZB occlusion; bit 1 disables meshlet-cone backface
    // rejection for a double-sided draw. Packing both here preserves the
    // 13-root-constant contract shared with the grass mesh path.
    UINT cullingFlags;
    UINT screenWidth;
    UINT screenHeight;
    // 0 static draw, 1 apply bone palette (t12) + skin (t13), 2 skinned AND
    // exempt from meshlet culling -- see mesh_as.hlsl. Packed into this field
    // rather than added as a new one because b6 is shared with the grass pass.
    UINT skinningEnabled;
    UINT occlusionMipCount;
    float modelMaxScale;
    UINT instanceCount;
    UINT instancingEnabled;
};

struct alignas(256) ShadowCascadeBufferDX12 {
    XMMATRIX lightViewProjection[SHADOW_CASCADE_COUNT];
    XMFLOAT4 splitDepths;
    XMFLOAT4 texelWorld;
    XMFLOAT4 depthRange;
};

// Transforms consumed directly by the amplification and mesh shaders. Unlike a
// CBV array these records are tightly packed in one root SRV.
struct MeshInstanceDataDX12 {
    XMFLOAT4X4 model;
    float modelMaxScale;
    float padding[3];
};

// Upload buffer helper
template<typename T>
class UploadBuffer {
public:
    ComPtr<ID3D12Resource> resource;
    T* mappedData = nullptr;
    UINT elementCount = 0;
    UINT elementSize = 0;
    
    bool Create(UINT count) {
        elementCount = count;
        elementSize = sizeof(T);
        UINT bufferSize = elementCount * elementSize;
        
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
        
        D3D12_RESOURCE_DESC bufferDesc = {};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width = bufferSize;
        bufferDesc.Height = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels = 1;
        bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        
        HRESULT hr = g_dx12.device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&resource));
        
        if (FAILED(hr)) return false;
        
        // Map and keep mapped
        D3D12_RANGE readRange = { 0, 0 };
        hr = resource->Map(0, &readRange, reinterpret_cast<void**>(&mappedData));
        if (FAILED(hr)) return false;
        
        return true;
    }
    
    void CopyData(UINT index, const T& data) {
        if (mappedData && index < elementCount) {
            memcpy(&mappedData[index], &data, sizeof(T));
        }
    }
    
    D3D12_GPU_VIRTUAL_ADDRESS GetGPUAddress(UINT index = 0) const {
        return resource->GetGPUVirtualAddress() + index * elementSize;
    }
    
    void Release() {
        if (resource && mappedData) {
            resource->Unmap(0, nullptr);
            mappedData = nullptr;
        }
        resource.Reset();
    }
    
    ~UploadBuffer() {
        Release();
    }
};

// Descriptors in one material's SRV table: albedo (t1), normal (t4),
// metal-rough (t5), emissive (t6), in that order. The reservation functions, the
// root-signature range count and the SRV creation block all derive from this --
// they must agree exactly, or materials sample each other's textures.
inline constexpr UINT MATERIAL_SRV_COUNT = 4;

// Maximum draw calls per frame (for per-object constant buffers). Must cover
// every destruction chunk (the Voronoi house alone is ~360 pieces) plus scene
// objects, projectiles and particles -- overflowing clamps draws to one shared
// constant slot and geometry visibly glues itself to the last-drawn object.
// Stress scenes exceed 4,096 forward draws. Overflow clamps to the last slot and
// geometry visibly glues itself to the last-drawn object. 16K costs roughly 16 MiB
// across the double-buffered matrix/object upload heaps and leaves fracture headroom.
static const UINT MAX_DRAW_CALLS_PER_FRAME = 16384;

class ShaderDX12 {
public:
    ComPtr<ID3D12RootSignature> rootSignature;
    ComPtr<ID3D12RootSignature> bindlessRootSignature;
    ComPtr<ID3D12PipelineState> pipelineState;
    ComPtr<ID3D12PipelineState> wireframePipelineState;
    ComPtr<ID3D12PipelineState> transparentPipelineState;
    ComPtr<ID3D12PipelineState> additivePipelineState;
    // Smooth viewmodel fading without temporal filtering: a depth-only pass
    // finds the nearest weapon surface, then an EQUAL-tested alpha pass shades
    // that surface once. This avoids both ordered-dither dots and back-face
    // accumulation from ordinary transparent rendering.
    ComPtr<ID3D12PipelineState> viewmodelDepthPipelineState;
    ComPtr<ID3D12PipelineState> viewmodelBlendPipelineState;
    ComPtr<ID3D12PipelineState> hdrViewmodelDepthPipelineState;
    ComPtr<ID3D12PipelineState> hdrViewmodelBlendPipelineState;
    ComPtr<ID3D12PipelineState> msaaViewmodelDepthPipelineState;
    ComPtr<ID3D12PipelineState> msaaViewmodelBlendPipelineState;
    ComPtr<ID3D12PipelineState> hdrMotionViewmodelDepthPipelineState;
    ComPtr<ID3D12PipelineState> hdrMotionViewmodelBlendPipelineState;
    // Additive, depth-bound but always passing: the first-person muzzle flash
    // belongs to the viewmodel and must not be occluded by world geometry the
    // weapon is standing in.
    ComPtr<ID3D12PipelineState> muzzleFlashPipelineState;
    ComPtr<ID3D12PipelineState> hdrMuzzleFlashPipelineState;
    ComPtr<ID3D12PipelineState> msaaPipelineState;
    ComPtr<ID3D12PipelineState> msaaWireframePipelineState;
    ComPtr<ID3D12PipelineState> msaaTransparentPipelineState;
    ComPtr<ID3D12PipelineState> msaaAdditivePipelineState;
    ComPtr<ID3D12PipelineState> hdrMsaaPipelineState;
    // Grass: same root signature, same pixel shader, but a vertex shader that
    // bends the blades in the wind. Null if grass_vs.hlsl failed to compile, in
    // which case the grass simply is not drawn.
    ComPtr<ID3D12PipelineState> grassPipelineState;
    ComPtr<ID3D12PipelineState> msaaGrassPipelineState;
    ComPtr<ID3D12PipelineState> hdrMsaaGrassPipelineState;
    ComPtr<ID3D12PipelineState> hdrPipelineState;
    ComPtr<ID3D12PipelineState> hdrWireframePipelineState;
    ComPtr<ID3D12PipelineState> hdrTransparentPipelineState;
    ComPtr<ID3D12PipelineState> hdrAdditivePipelineState;
    ComPtr<ID3D12PipelineState> hdrGrassPipelineState;
    // Extension-motion PSOs: compiled with SGE_EXTENSION_MOTION, output to
    // SV_Target0 (colour) and SV_Target1 (motion R16G16_FLOAT).
    ComPtr<ID3D12PipelineState> hdrMotionPipelineState;
    ComPtr<ID3D12PipelineState> hdrMotionTransparentPipelineState;
    ComPtr<ID3D12PipelineState> hdrMotionAdditivePipelineState;
    ComPtr<ID3DBlob> pixelShaderBlob;
    ComPtr<ID3DBlob> bindlessPixelShaderBlob;
    ComPtr<ID3DBlob> bindlessHDRPixelShaderBlob;
    ComPtr<ID3DBlob> bindlessMotionPixelShaderBlob;
    ComPtr<ID3D12PipelineState> bindlessPipelineState;
    ComPtr<ID3D12PipelineState> bindlessWireframePipelineState;
    ComPtr<ID3D12PipelineState> bindlessTransparentPipelineState;
    ComPtr<ID3D12PipelineState> bindlessAdditivePipelineState;
    ComPtr<ID3D12PipelineState> bindlessHDRPipelineState;
    ComPtr<ID3D12PipelineState> bindlessHDRWireframePipelineState;
    ComPtr<ID3D12PipelineState> bindlessHDRTransparentPipelineState;
    ComPtr<ID3D12PipelineState> bindlessHDRAdditivePipelineState;
    ComPtr<ID3D12PipelineState> bindlessHDRMotionPipelineState;
    ComPtr<ID3D12PipelineState> bindlessHDRMotionTransparentPipelineState;
    ComPtr<ID3D12PipelineState> bindlessHDRMotionAdditivePipelineState;
    ComPtr<ID3D12PipelineState> bindlessMSAAPipelineState;
    ComPtr<ID3D12PipelineState> bindlessMSAAWireframePipelineState;
    ComPtr<ID3D12PipelineState> bindlessMSAATransparentPipelineState;
    ComPtr<ID3D12PipelineState> bindlessMSAAAdditivePipelineState;
    bool bindlessPipelineReady = false;
    bool bindlessRequested = false;
    bool currentDrawBindless = false;
    bool forceLegacyNextMaterial = false;
    BindlessHeapDX12* bindlessHeap = nullptr;
    UINT bindlessGlobalTableBase = BINDLESS_INVALID_INDEX;
    enum class DrawPipelineKind { Opaque, Transparent, Additive };
    DrawPipelineKind drawPipelineKind = DrawPipelineKind::Opaque;
    enum class ViewmodelPass { None, Depth, Blend };
    ViewmodelPass viewmodelPass = ViewmodelPass::None;
    // Binocular see-through strength for the first-person weapon, applied to
    // every material set while it is non-zero. Sticky rather than an argument
    // on SetObjectMaterial: the viewmodel is drawn through a dozen call sites
    // (mesh, skinned arms, procedural parts, DrawSceneNode) and several of them
    // build their material internally, so threading a parameter would miss
    // exactly the paths that need it. The renderer brackets the viewmodel with
    // BeginViewmodelSeeThrough/EndViewmodelSeeThrough.
    float viewmodelSeeThrough = 0.0f;
    // Shape of that fade, carried alongside the strength for the same reason.
    // Defaults mirror the shader's documented measurements.
    float viewmodelSeeThroughNear = 0.30f;
    float viewmodelSeeThroughFar = 1.05f;
    float viewmodelSeeThroughNearAlpha = 0.25f;
    // Glass Fresnel response, shared by every draw whose material selects the
    // glass type. Sticky like the see-through tuning above: only one material
    // uses it today, and threading two more arguments through the twenty-plus
    // parameter SetObjectMaterial would cost every unrelated call site.
    float glassClarity = 0.16f;
    float glassGrazing = 0.86f;
    // The scope camera's focal lengths, for the sniper lens. Zero when no
    // scope target was rendered this frame. Sticky for the same reason as the
    // glass tuning above.
    float sniperScopeFocalX = 0.0f;
    float sniperScopeFocalY = 0.0f;
    // Non-zero draws the lens orientation key. Set alongside the focal
    // lengths, so it follows the same sticky per-frame lifetime.
    float sniperScopeDebugMode = 0.0f;
    // Tangent of the scope glass's half-angle from the eye. Same sticky
    // lifetime as the focal lengths it accompanies.
    float sniperLensHalfTangent = 0.0f;
    XMFLOAT2 sniperAimTangent{};
    XMFLOAT4 sniperLensAperture{};
    bool drawWireframe = false;
    bool msaaSupported = false;
    bool msaaEnabled = false;
    bool hdrTargetEnabled = false;
    bool graphicsRootBound = false;
    bool extensionMotionEnabled = false;
    
    // Per-draw-call constant buffers (need enough for all objects)
    UploadBuffer<MatrixBufferDX12> matrixBuffer;
    XMMATRIX previousViewProjection = XMMatrixIdentity();
    PalmWindFrameDX12 palmWindFrame{};
    UploadBuffer<ObjectBufferDX12> objectBuffer;
    
    // Per-frame constant buffers (shared across all draw calls in a frame)
    UploadBuffer<LightBufferDX12> lightBuffer;
    UploadBuffer<CameraBufferDX12> cameraBuffer;
    UploadBuffer<PointLightsBufferDX12> pointLightsBuffer;
    UploadBuffer<ImpactDecalsBufferDX12> impactDecalsBuffer;
    UploadBuffer<DDGIBufferDX12> ddgiBuffer;
    UploadBuffer<SHBufferDX12> shBuffer;
    UploadBuffer<ShadowCascadeBufferDX12> shadowCascadeBuffer;
    std::array<XMFLOAT3, 9> pendingSHCoeffs{};
    float pendingSkyIntensity = 1.0f;
    bool skyIrradianceValid = false;
    
    // Material/object data advances per primitive, while a scene node can share
    // one matrix across several primitives. Keep the matrix slot independent so
    // a material root-signature switch restores the matrix that was actually
    // written instead of an unwritten slot at currentDrawCall.
    UINT currentDrawCall = 0;
    UINT activeMatrixDrawCall = 0;
    // Which per-view slice draws currently record into. 0 (main) unless the
    // scope pass has re-based it, so every existing call site is unaffected.
    UINT currentViewSlot = 0;
    UINT viewDrawCalls[2] = { 0, 0 };
    UINT currentSrvOffset = 0; // For material descriptors

    // The shared CBV/SRV/UAV heap is carved into three regions:
    //   [0, 64)                      globals, owned elsewhere (ImGui font, etc.)
    //   [64, kPersistentSrvEnd)      PERSISTENT: cached per-material descriptors
    //   [kPersistentSrvEnd, end)     per-frame scratch, reset every BeginFrame
    //
    // The persistent region exists because a material's textures never change once
    // it is loaded, yet the descriptors were being recreated on every single draw:
    // the destructible house alone is ~588 textured chunks, so that was ~1,764
    // CreateShaderResourceView calls per frame for descriptors that were bit for
    // bit identical each time. Now each material takes a slot here on its first
    // draw and reuses it forever.
    static constexpr UINT kPersistentSrvBegin = 64;
    static constexpr UINT kPersistentSrvEnd   = 4096;   // room for ~1,008 materials
    UINT persistentSrvOffset = kPersistentSrvBegin;

    // Handles for descriptor slot `index` in the shared heap.
    static void SrvHandlesAt(UINT index, D3D12_CPU_DESCRIPTOR_HANDLE& cpuHandle,
                             D3D12_GPU_DESCRIPTOR_HANDLE& gpuHandle) {
        const UINT descriptorSize = g_dx12.device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        cpuHandle = g_dx12.cbvSrvUavHeap->GetCPUDescriptorHandleForHeapStart();
        gpuHandle = g_dx12.cbvSrvUavHeap->GetGPUDescriptorHandleForHeapStart();
        cpuHandle.ptr += (SIZE_T)index * descriptorSize;
        gpuHandle.ptr += (UINT64)index * descriptorSize;
    }

    // Claim MATERIAL_SRV_COUNT consecutive PERSISTENT descriptors for a material.
    // Returns ~0u when that region is full, in which case the caller falls back
    // to the per-frame path and simply pays the old cost.
    UINT ReservePersistentMaterialSrvs() {
        if (persistentSrvOffset + MATERIAL_SRV_COUNT > kPersistentSrvEnd)
            return ~0u;
        const UINT slot = persistentSrvOffset;
        persistentSrvOffset += MATERIAL_SRV_COUNT;
        return slot;
    }

    // Reserve one material's descriptors (albedo/normal/metal-rough/emissive)
    // from the per-frame scratch region. Returns false when the frame has used
    // them all -- writing past the heap corrupts unrelated allocations (it used
    // to clobber ImGui's font descriptor, making the UI vanish once enough smoke
    // was alive).
    bool ReserveMaterialSrvs(D3D12_CPU_DESCRIPTOR_HANDLE& cpuHandle,
                             D3D12_GPU_DESCRIPTOR_HANDLE& gpuHandle) {
        if (currentSrvOffset + MATERIAL_SRV_COUNT > CBV_SRV_UAV_HEAP_SIZE)
            return false;
        SrvHandlesAt(currentSrvOffset, cpuHandle, gpuHandle);
        currentSrvOffset += MATERIAL_SRV_COUNT;
        return true;
    }

    bool loaded = false;

    ShaderDX12() {}

    // Compile one HLSL file from disk. Same runtime-compile path the main shaders
    // take; factored out so extra pipeline variants (e.g. the grass wind vertex
    // shader) can be built without duplicating it.
    static bool CompileShaderFile(const char* path, const char* target,
                                  UINT compileFlags, ComPtr<ID3DBlob>& outBlob,
                                  const D3D_SHADER_MACRO* defines = nullptr) {
        std::ifstream file(path);
        if (!file.is_open()) {
            std::cerr << "Failed to open shader file: " << path << std::endl;
            return false;
        }
        std::stringstream ss;
        ss << file.rdbuf();
        const std::string code = ss.str();

        ComPtr<ID3DBlob> errorBlob;
        HRESULT hr = ShaderCacheDX12::CompileCached(code.c_str(), code.length(), path,
            defines, D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", target,
            compileFlags, 0, &outBlob, &errorBlob);
        if (FAILED(hr)) {
            std::cerr << "Shader compilation error (" << path << "): "
                      << (errorBlob ? (const char*)errorBlob->GetBufferPointer() : "unknown")
                      << std::endl;
            return false;
        }
        return true;
    }

    bool Load(const char* vertexPath, const char* pixelPath) {
        // Read shader files
        std::ifstream vsFile(vertexPath);
        std::ifstream psFile(pixelPath);
        
        if (!vsFile.is_open() || !psFile.is_open()) {
            std::cerr << "Failed to open shader files: " << vertexPath << ", " << pixelPath << std::endl;
            return false;
        }
        
        std::stringstream vsStream, psStream;
        vsStream << vsFile.rdbuf();
        psStream << psFile.rdbuf();
        
        std::string vsCode = vsStream.str();
        std::string psCode = psStream.str();
        
        // Compile vertex shader (use 5_0 for compatibility)
        ComPtr<ID3DBlob> vsBlob;
        ComPtr<ID3DBlob> errorBlob;
        UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
        compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
        compileFlags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
        
        HRESULT hr = ShaderCacheDX12::CompileCached(vsCode.c_str(), vsCode.length(), vertexPath,
            nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "vs_5_0",
            compileFlags, 0, &vsBlob, &errorBlob);
        
        if (FAILED(hr)) {
            if (errorBlob) {
                std::cerr << "Vertex shader compilation error: " << (char*)errorBlob->GetBufferPointer() << std::endl;
            }
            return false;
        }
        
        // Compile pixel shader
        ComPtr<ID3DBlob> psBlob;
        errorBlob.Reset();
        hr = ShaderCacheDX12::CompileCached(psCode.c_str(), psCode.length(), pixelPath,
            nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "ps_5_0",
            compileFlags, 0, &psBlob, &errorBlob);
        
        if (FAILED(hr)) {
            if (errorBlob) {
                std::cerr << "Pixel shader compilation error: " << (char*)errorBlob->GetBufferPointer() << std::endl;
            }
            return false;
        }
        const D3D_SHADER_MACRO hdrDefines[] = {
            { "SGE_HDR_TARGET", "1" }, { nullptr, nullptr }
        };
        ComPtr<ID3DBlob> hdrPsBlob;
        errorBlob.Reset();
        hr = ShaderCacheDX12::CompileCached(psCode.c_str(), psCode.length(), pixelPath,
            hdrDefines, D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "ps_5_0",
            compileFlags, 0, &hdrPsBlob, &errorBlob);
        if (FAILED(hr)) {
            if (errorBlob) std::cerr << "HDR pixel shader compilation error: "
                << (char*)errorBlob->GetBufferPointer() << std::endl;
            return false;
        }

        // Extension motion PS: same source compiled with SV_Target1 motion
        // output. Needed only for HDR passes (forward extensions), so compile
        // with both SGE_EXTENSION_MOTION and SGE_HDR_TARGET.
        const D3D_SHADER_MACRO motionDefines[] = {
            { "SGE_HDR_TARGET", "1" },
            { "SGE_EXTENSION_MOTION", "1" },
            { nullptr, nullptr }
        };
        ComPtr<ID3DBlob> motionPsBlob;
        errorBlob.Reset();
        HRESULT motionHr = ShaderCacheDX12::CompileCached(
            psCode.c_str(), psCode.length(), pixelPath,
            motionDefines, D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "ps_5_0",
            compileFlags, 0, &motionPsBlob, &errorBlob);
        if (FAILED(motionHr)) {
            if (errorBlob) {
                std::cerr << "Extension-motion pixel shader compilation error: "
                    << (char*)errorBlob->GetBufferPointer() << std::endl;
            }
            // Non-fatal: motion PSOs won't be created; the toggle stays off.
            motionPsBlob.Reset();
        }

        pixelShaderBlob = psBlob;
        
        // Create root signature using version 1.0 for maximum compatibility
        // Root parameters:
        // 0: CBV - Matrix buffer (b0)
        // 1: CBV - Light buffer (b1)
        // 2: CBV - Camera buffer (b2)
        // 3: CBV - Object buffer (b3)
        // 4: CBV - Point lights buffer (b4)
        // 5: CBV - DDGI buffer (b5)
        // 6: Descriptor table - Global SRVs (t0, t2, t3)
        // 7: Descriptor table - Material SRVs (t1, t4, t5)
        
        D3D12_ROOT_PARAMETER rootParams[23] = {};
        
        // CBVs (root descriptors)
        for (int i = 0; i < 6; i++) {
            rootParams[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            rootParams[i].Descriptor.ShaderRegister = i;
            rootParams[i].Descriptor.RegisterSpace = 0;
            rootParams[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }
        
        // SRV descriptor table for global textures
        D3D12_DESCRIPTOR_RANGE globalSrvRanges[9] = {};
        // t0 - shadowMap
        globalSrvRanges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        globalSrvRanges[0].NumDescriptors = 1;
        globalSrvRanges[0].BaseShaderRegister = 0;
        globalSrvRanges[0].RegisterSpace = 0;
        globalSrvRanges[0].OffsetInDescriptorsFromTableStart = 0;
        // t2 - ddgiIrradiance
        globalSrvRanges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        globalSrvRanges[1].NumDescriptors = 1;
        globalSrvRanges[1].BaseShaderRegister = 2;
        globalSrvRanges[1].RegisterSpace = 0;
        globalSrvRanges[1].OffsetInDescriptorsFromTableStart = 1;
        // t3 - ddgiVisibility
        globalSrvRanges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        globalSrvRanges[2].NumDescriptors = 1;
        globalSrvRanges[2].BaseShaderRegister = 3;
        globalSrvRanges[2].RegisterSpace = 0;
        globalSrvRanges[2].OffsetInDescriptorsFromTableStart = 2;
        // t15 - HDR equirectangular environment for specular IBL. t14 is the
        // static-instance root SRV and cannot overlap this descriptor table.
        globalSrvRanges[3].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        globalSrvRanges[3].NumDescriptors = 1;
        globalSrvRanges[3].BaseShaderRegister = 15;
        globalSrvRanges[3].RegisterSpace = 0;
        globalSrvRanges[3].OffsetInDescriptorsFromTableStart = 3;
        // t16 - split-sum GGX BRDF integration LUT.
        globalSrvRanges[4].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        globalSrvRanges[4].NumDescriptors = 1;
        globalSrvRanges[4].BaseShaderRegister = 16;
        globalSrvRanges[4].RegisterSpace = 0;
        globalSrvRanges[4].OffsetInDescriptorsFromTableStart = 4;
        for (UINT i = 0; i < 3; ++i) {
            globalSrvRanges[5 + i].RangeType =
                D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            globalSrvRanges[5 + i].NumDescriptors = 1;
            globalSrvRanges[5 + i].BaseShaderRegister = 17 + i;
            globalSrvRanges[5 + i].RegisterSpace = 0;
            globalSrvRanges[5 + i].OffsetInDescriptorsFromTableStart = 5 + i;
        }
        
        // t21 - spot shadow atlas, for shadow-casting headlights and the enemy
        // helicopter searchlights. Appended last so no existing table offset
        // moves.
        globalSrvRanges[8].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        globalSrvRanges[8].NumDescriptors = 1;
        globalSrvRanges[8].BaseShaderRegister = 21;
        globalSrvRanges[8].RegisterSpace = 0;
        globalSrvRanges[8].OffsetInDescriptorsFromTableStart = 8;

        rootParams[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[6].DescriptorTable.NumDescriptorRanges = _countof(globalSrvRanges);
        rootParams[6].DescriptorTable.pDescriptorRanges = globalSrvRanges;
        rootParams[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        // SRV descriptor table for material textures
        D3D12_DESCRIPTOR_RANGE matSrvRanges[MATERIAL_SRV_COUNT] = {};
        // t1 - Albedo
        matSrvRanges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        matSrvRanges[0].NumDescriptors = 1;
        matSrvRanges[0].BaseShaderRegister = 1;
        matSrvRanges[0].RegisterSpace = 0;
        matSrvRanges[0].OffsetInDescriptorsFromTableStart = 0;
        // t4 - Normal
        matSrvRanges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        matSrvRanges[1].NumDescriptors = 1;
        matSrvRanges[1].BaseShaderRegister = 4;
        matSrvRanges[1].RegisterSpace = 0;
        matSrvRanges[1].OffsetInDescriptorsFromTableStart = 1;
        // t5 - MetallicRoughness
        matSrvRanges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        matSrvRanges[2].NumDescriptors = 1;
        matSrvRanges[2].BaseShaderRegister = 5;
        matSrvRanges[2].RegisterSpace = 0;
        matSrvRanges[2].OffsetInDescriptorsFromTableStart = 2;
        // t22 - Emissive. Appended last so no existing table offset moves.
        //
        // t22 rather than the next number after t5: this signature already fills
        // t0-t21 with no gaps, and several of those are ROOT SRVs declared here
        // rather than textures declared in the pixel shader (t6/t7/t8 are
        // rootParams[9]/[10]/[11]). Reading only the shader's own register
        // declarations makes t6 look free; it is not, and binding here collides
        // at D3D12SerializeRootSignature, which fails every shader at boot.
        matSrvRanges[3].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        matSrvRanges[3].NumDescriptors = 1;
        matSrvRanges[3].BaseShaderRegister = 22;
        matSrvRanges[3].RegisterSpace = 0;
        matSrvRanges[3].OffsetInDescriptorsFromTableStart = 3;

        rootParams[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[7].DescriptorTable.NumDescriptorRanges = _countof(matSrvRanges);
        rootParams[7].DescriptorTable.pDescriptorRanges = matSrvRanges;
        rootParams[7].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        // Mesh-shader draw parameters. Existing raster path ignores these.
        rootParams[8].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rootParams[8].Constants.ShaderRegister = 6;
        rootParams[8].Constants.RegisterSpace = 0;
        // Grass uploads 19 (the 13 wind/fade fields plus the player interaction
        // capsule); terrain uploads 16. Sized for the larger of the two.
        rootParams[8].Constants.Num32BitValues = 19;
        rootParams[8].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParams[9].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        rootParams[9].Descriptor.ShaderRegister = 6;
        rootParams[9].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParams[10].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        rootParams[10].Descriptor.ShaderRegister = 7;
        rootParams[10].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParams[11].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        rootParams[11].Descriptor.ShaderRegister = 8;
        rootParams[11].ShaderVisibility = D3D12_SHADER_VISIBILITY_AMPLIFICATION;
        D3D12_DESCRIPTOR_RANGE occlusionRange = {};
        occlusionRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        occlusionRange.NumDescriptors = 1;
        occlusionRange.BaseShaderRegister = 9;
        occlusionRange.OffsetInDescriptorsFromTableStart = 0;
        rootParams[12].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[12].DescriptorTable.NumDescriptorRanges = 1;
        rootParams[12].DescriptorTable.pDescriptorRanges = &occlusionRange;
        rootParams[12].ShaderVisibility = D3D12_SHADER_VISIBILITY_AMPLIFICATION;
        rootParams[13].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        rootParams[13].Descriptor.ShaderRegister = 10;
        // Error-based terrain LOD measures the same live sculpted height field
        // in the amplification shader before the mesh shader consumes it.
        rootParams[13].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParams[14].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        rootParams[14].Descriptor.ShaderRegister = 11;
        rootParams[14].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        // Sky irradiance SH coefficients (b7) - diffuse IBL ambient term
        rootParams[15].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParams[15].Descriptor.ShaderRegister = 7;
        rootParams[15].Descriptor.RegisterSpace = 0;
        rootParams[15].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        // Skeletal skinning: bone palette (t12) + per-vertex skin weights (t13).
        // Both root SRVs, VISIBILITY_ALL so the mesh shader (and any classic VS
        // reuse) can read them. Non-skinned draws leave them unbound.
        rootParams[16].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        rootParams[16].Descriptor.ShaderRegister = 12;
        rootParams[16].Descriptor.RegisterSpace = 0;
        rootParams[16].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        rootParams[17].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        rootParams[17].Descriptor.ShaderRegister = 13;
        rootParams[17].Descriptor.RegisterSpace = 0;
        rootParams[17].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        // Static mesh instances (t14). Skinned draws keep their per-draw palette
        // path and leave instancing disabled.
        rootParams[18].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        rootParams[18].Descriptor.ShaderRegister = 14;
        rootParams[18].Descriptor.RegisterSpace = 0;
        rootParams[18].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        // Previous-frame bone palette (t20), used with extension motion vectors
        // to skin the previous pose so skinned limbs report proper motion.
        rootParams[19].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        rootParams[19].Descriptor.ShaderRegister = 20;
        rootParams[19].Descriptor.RegisterSpace = 0;
        rootParams[19].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        rootParams[20].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParams[20].Descriptor.ShaderRegister = 8;
        rootParams[20].Descriptor.RegisterSpace = 0;
        rootParams[20].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        // Skinning toggle for the IA raster path (b9). This gets its own root
        // parameter on purpose: b6 is shared by grass, terrain, instancing and
        // the mesh shader, each writing a different struct, so a flag read from
        // there picks up whatever that pass happened to store -- grass puts a
        // world coordinate in that slot, which reads as "skinning on" and
        // shreds every static mesh drawn afterwards.
        rootParams[21].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rootParams[21].Constants.ShaderRegister = 9;
        rootParams[21].Constants.RegisterSpace = 0;
        rootParams[21].Constants.Num32BitValues = 1;
        rootParams[21].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        // Impact decals (b10). A CBV rather than 32-bit constants: the list is
        // up to 64 entries and is rewritten whenever one is spawned or ages out.
        rootParams[22].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        rootParams[22].Descriptor.ShaderRegister = 10;
        rootParams[22].Descriptor.RegisterSpace = 0;
        rootParams[22].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        // Static samplers
        D3D12_STATIC_SAMPLER_DESC staticSamplers[2] = {};
        
        // Shadow sampler (comparison)
        staticSamplers[0].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
        staticSamplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        staticSamplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        staticSamplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        staticSamplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        staticSamplers[0].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
        staticSamplers[0].ShaderRegister = 0;
        staticSamplers[0].RegisterSpace = 0;
        staticSamplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        
        // Regular sampler - anisotropic so grazing-angle surfaces (e.g. walls
        // viewed edge-on) don't alias/moire even with a correct mip chain bound;
        // plain trilinear only fixes minification along the view axis, not the
        // elongated footprint a shallow angle projects onto the texture.
        staticSamplers[1].Filter = D3D12_FILTER_ANISOTROPIC;
        staticSamplers[1].MaxAnisotropy = 16;
        staticSamplers[1].MipLODBias = 0.0f;
        staticSamplers[1].MinLOD = 0.0f;
        staticSamplers[1].MaxLOD = D3D12_FLOAT32_MAX;
        staticSamplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        staticSamplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        staticSamplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        staticSamplers[1].ShaderRegister = 1;
        staticSamplers[1].RegisterSpace = 0;
        staticSamplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        
        D3D12_ROOT_SIGNATURE_DESC rootSigDesc = {};
        // Keep the parameter layout identical for legacy and bindless. The
        // legacy shader never reads b10, but retaining the inert slot prevents
        // material-path switches from changing every later root argument.
        rootSigDesc.NumParameters = 23;
        rootSigDesc.pParameters = rootParams;
        rootSigDesc.NumStaticSamplers = 2;
        rootSigDesc.pStaticSamplers = staticSamplers;
        rootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        
        ComPtr<ID3DBlob> signatureBlob;
        errorBlob.Reset();
        hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signatureBlob, &errorBlob);
        if (FAILED(hr)) {
            if (errorBlob) {
                std::cerr << "Root signature serialization error: " << (char*)errorBlob->GetBufferPointer() << std::endl;
            } else {
                std::cerr << "Root signature serialization failed with HRESULT: 0x" << std::hex << hr << std::dec << std::endl;
            }
            return false;
        }
        
        hr = g_dx12.device->CreateRootSignature(0, signatureBlob->GetBufferPointer(),
            signatureBlob->GetBufferSize(), IID_PPV_ARGS(&rootSignature));
        if (FAILED(hr)) {
            std::cerr << "Failed to create root signature" << std::endl;
            return false;
        }
        
        // Input layout
        D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TANGENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
        };
        
        // Create PSO
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.InputLayout = { inputLayout, _countof(inputLayout) };
        psoDesc.pRootSignature = rootSignature.Get();
        psoDesc.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
        psoDesc.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
        
        psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        // No culling: imported glTF materials are marked doubleSided, and glTF's
        // front-face winding (CCW) is the opposite of DX12's default (CW), so
        // culling here would drop faces on imported models (e.g. the h1.glb house).
        psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
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
        psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        psoDesc.SampleDesc.Count = 1;

        auto createMSAAPipeline = [&psoDesc](
                ComPtr<ID3D12PipelineState>& target) {
            const BOOL previousMultisample =
                psoDesc.RasterizerState.MultisampleEnable;
            psoDesc.SampleDesc.Count = MSAADX12::SampleCount;
            psoDesc.SampleDesc.Quality = 0;
            psoDesc.RasterizerState.MultisampleEnable = TRUE;
            const HRESULT result = g_dx12.device->CreateGraphicsPipelineState(
                &psoDesc, IID_PPV_ARGS(&target));
            psoDesc.SampleDesc.Count = 1;
            psoDesc.SampleDesc.Quality = 0;
            psoDesc.RasterizerState.MultisampleEnable = previousMultisample;
            return SUCCEEDED(result);
        };
        
        hr = g_dx12.device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pipelineState));
        if (FAILED(hr)) {
            std::cerr << "Failed to create pipeline state, HRESULT: 0x" << std::hex << hr << std::dec << std::endl;
            return false;
        }
        psoDesc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        psoDesc.PS = { hdrPsBlob->GetBufferPointer(), hdrPsBlob->GetBufferSize() };
        if (FAILED(g_dx12.device->CreateGraphicsPipelineState(
                &psoDesc, IID_PPV_ARGS(&hdrPipelineState)))) return false;
        const bool hdrMsaaMainSupported =
            createMSAAPipeline(hdrMsaaPipelineState);
        psoDesc.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
        psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        msaaSupported = hdrMsaaMainSupported &&
            createMSAAPipeline(msaaPipelineState);

        auto createViewmodelPassPipelines = [&psoDesc](
                ID3DBlob* pixelShader, DXGI_FORMAT colorFormat,
                UINT renderTargetCount, UINT sampleCount,
                ComPtr<ID3D12PipelineState>& depthTarget,
                ComPtr<ID3D12PipelineState>& blendTarget) {
            const D3D12_GRAPHICS_PIPELINE_STATE_DESC saved = psoDesc;
            psoDesc.PS = { pixelShader->GetBufferPointer(),
                           pixelShader->GetBufferSize() };
            psoDesc.NumRenderTargets = renderTargetCount;
            psoDesc.RTVFormats[0] = colorFormat;
            psoDesc.SampleDesc.Count = sampleCount;
            psoDesc.SampleDesc.Quality = 0;
            psoDesc.RasterizerState.MultisampleEnable = sampleCount > 1;
            psoDesc.BlendState.RenderTarget[0].BlendEnable = FALSE;
            psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = 0;
            if (renderTargetCount > 1) {
                psoDesc.BlendState.RenderTarget[1].BlendEnable = FALSE;
                psoDesc.BlendState.RenderTarget[1].RenderTargetWriteMask = 0;
            }
            psoDesc.DepthStencilState.DepthWriteMask =
                D3D12_DEPTH_WRITE_MASK_ALL;
            psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
            HRESULT result = g_dx12.device->CreateGraphicsPipelineState(
                &psoDesc, IID_PPV_ARGS(&depthTarget));
            if (SUCCEEDED(result)) {
                psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask =
                    D3D12_COLOR_WRITE_ENABLE_ALL;
                psoDesc.BlendState.RenderTarget[0].BlendEnable = TRUE;
                psoDesc.BlendState.RenderTarget[0].SrcBlend =
                    D3D12_BLEND_SRC_ALPHA;
                psoDesc.BlendState.RenderTarget[0].DestBlend =
                    D3D12_BLEND_INV_SRC_ALPHA;
                psoDesc.BlendState.RenderTarget[0].BlendOp =
                    D3D12_BLEND_OP_ADD;
                psoDesc.BlendState.RenderTarget[0].SrcBlendAlpha =
                    D3D12_BLEND_ONE;
                psoDesc.BlendState.RenderTarget[0].DestBlendAlpha =
                    D3D12_BLEND_INV_SRC_ALPHA;
                psoDesc.BlendState.RenderTarget[0].BlendOpAlpha =
                    D3D12_BLEND_OP_ADD;
                if (renderTargetCount > 1)
                    psoDesc.BlendState.RenderTarget[1].RenderTargetWriteMask =
                        D3D12_COLOR_WRITE_ENABLE_ALL;
                psoDesc.DepthStencilState.DepthWriteMask =
                    D3D12_DEPTH_WRITE_MASK_ZERO;
                psoDesc.DepthStencilState.DepthFunc =
                    D3D12_COMPARISON_FUNC_EQUAL;
                result = g_dx12.device->CreateGraphicsPipelineState(
                    &psoDesc, IID_PPV_ARGS(&blendTarget));
            }
            psoDesc = saved;
            return SUCCEEDED(result);
        };

        if (!createViewmodelPassPipelines(
                psBlob.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 1, 1,
                viewmodelDepthPipelineState, viewmodelBlendPipelineState) ||
            !createViewmodelPassPipelines(
                hdrPsBlob.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, 1, 1,
                hdrViewmodelDepthPipelineState,
                hdrViewmodelBlendPipelineState)) return false;
        if (msaaSupported && !createViewmodelPassPipelines(
                psBlob.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, 1,
                MSAADX12::SampleCount, msaaViewmodelDepthPipelineState,
                msaaViewmodelBlendPipelineState)) {
            msaaSupported = false;
        }

        // Alpha-blended material pass. Keep depth testing, disable depth writes
        // so glass reveals opaque geometry behind it.
        psoDesc.BlendState.RenderTarget[0].BlendEnable = TRUE;
        psoDesc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
        psoDesc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        psoDesc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        psoDesc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
        psoDesc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        psoDesc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
        psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        hr = g_dx12.device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&transparentPipelineState));
        if (FAILED(hr)) return false;
        psoDesc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        psoDesc.PS = { hdrPsBlob->GetBufferPointer(), hdrPsBlob->GetBufferSize() };
        if (FAILED(g_dx12.device->CreateGraphicsPipelineState(
                &psoDesc, IID_PPV_ARGS(&hdrTransparentPipelineState)))) return false;
        psoDesc.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
        psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        if (msaaSupported && !createMSAAPipeline(msaaTransparentPipelineState))
            msaaSupported = false;

        // Fire/glow sprites: alpha shapes the source, destination stays visible.
        // This also makes black pixels in conventional VFX sheets disappear.
        psoDesc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
        psoDesc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
        psoDesc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ZERO;
        psoDesc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
        hr = g_dx12.device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&additivePipelineState));
        if (FAILED(hr)) return false;
        psoDesc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        psoDesc.PS = { hdrPsBlob->GetBufferPointer(), hdrPsBlob->GetBufferSize() };
        if (FAILED(g_dx12.device->CreateGraphicsPipelineState(
                &psoDesc, IID_PPV_ARGS(&hdrAdditivePipelineState)))) return false;
        psoDesc.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
        psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        if (msaaSupported && !createMSAAPipeline(msaaAdditivePipelineState))
            msaaSupported = false;

        // Muzzle flash. The gun is a viewmodel but its flash is a world-space
        // quad at the muzzle, so grass between the eye and the barrel wins the
        // depth test and swallows it. DepthFunc ALWAYS lets the flash through
        // without moving it toward the camera -- the quad is ~0.6 m across and
        // sits ~0.7 m from the eye, so pulling it in inflates it across the
        // screen instead of fixing anything.
        {
            const D3D12_COMPARISON_FUNC savedFunc =
                psoDesc.DepthStencilState.DepthFunc;
            psoDesc.DepthStencilState.DepthFunc =
                D3D12_COMPARISON_FUNC_ALWAYS;
            if (FAILED(g_dx12.device->CreateGraphicsPipelineState(
                    &psoDesc, IID_PPV_ARGS(&muzzleFlashPipelineState))))
                muzzleFlashPipelineState.Reset();
            psoDesc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
            psoDesc.PS = { hdrPsBlob->GetBufferPointer(),
                           hdrPsBlob->GetBufferSize() };
            if (FAILED(g_dx12.device->CreateGraphicsPipelineState(
                    &psoDesc, IID_PPV_ARGS(&hdrMuzzleFlashPipelineState))))
                hdrMuzzleFlashPipelineState.Reset();
            psoDesc.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
            psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
            psoDesc.DepthStencilState.DepthFunc = savedFunc;
        }

        psoDesc.BlendState.RenderTarget[0].BlendEnable = FALSE;
        psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        
        // Create wireframe PSO
        psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
        hr = g_dx12.device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&wireframePipelineState));
        if (FAILED(hr)) {
            std::cerr << "Failed to create wireframe pipeline state" << std::endl;
            // Non-fatal
        }
        psoDesc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        psoDesc.PS = { hdrPsBlob->GetBufferPointer(), hdrPsBlob->GetBufferSize() };
        if (FAILED(g_dx12.device->CreateGraphicsPipelineState(
                &psoDesc, IID_PPV_ARGS(&hdrWireframePipelineState)))) return false;
        psoDesc.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
        psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        if (msaaSupported && !createMSAAPipeline(msaaWireframePipelineState))
            msaaSupported = false;

        // Grass PSO: the opaque state, but with the instanced wind vertex shader.
        // Authored template stores height, edge, forward curve, and width profile.
        //
        // Note the FillMode reset -- the wireframe PSO above left it WIREFRAME in
        // the shared desc, and inheriting that would draw the grass as lines.
        psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        D3D12_INPUT_ELEMENT_DESC grassLayout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };
        ComPtr<ID3DBlob> grassVsBlob;
        if (CompileShaderFile("shaders/grass_vs.hlsl", "vs_5_0", compileFlags, grassVsBlob)) {
            psoDesc.InputLayout = { grassLayout, _countof(grassLayout) };
            psoDesc.VS = { grassVsBlob->GetBufferPointer(), grassVsBlob->GetBufferSize() };
            // Cheap grass pixel shader: Lambert + SH + 1-tap shadow, binding a
            // strict subset of the shared root signature. Falls back to the
            // full PS if it fails to compile -- the grass still draws.
            ComPtr<ID3DBlob> grassPsBlob;
            ComPtr<ID3DBlob> hdrGrassPsBlob;
            if (CompileShaderFile("shaders/grass_ps.hlsl", "ps_5_0", compileFlags, grassPsBlob))
                psoDesc.PS = { grassPsBlob->GetBufferPointer(), grassPsBlob->GetBufferSize() };
            else
                std::cerr << "grass_ps.hlsl failed to compile; grass uses the full shader\n";
            CompileShaderFile("shaders/grass_ps.hlsl", "ps_5_0", compileFlags,
                hdrGrassPsBlob, hdrDefines);
            hr = g_dx12.device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&grassPipelineState));
            if (FAILED(hr)) {
                std::cerr << "Failed to create grass pipeline state" << std::endl;
                grassPipelineState.Reset();   // non-fatal: the grass just won't draw
            } else {
                psoDesc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
                psoDesc.PS = hdrGrassPsBlob
                    ? D3D12_SHADER_BYTECODE{ hdrGrassPsBlob->GetBufferPointer(),
                                             hdrGrassPsBlob->GetBufferSize() }
                    : D3D12_SHADER_BYTECODE{ hdrPsBlob->GetBufferPointer(),
                                             hdrPsBlob->GetBufferSize() };
                if (FAILED(g_dx12.device->CreateGraphicsPipelineState(
                        &psoDesc, IID_PPV_ARGS(&hdrGrassPipelineState))))
                    hdrGrassPipelineState.Reset();
                if (msaaSupported &&
                    !createMSAAPipeline(hdrMsaaGrassPipelineState))
                    hdrMsaaGrassPipelineState.Reset();
                psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
                psoDesc.PS = grassPsBlob
                    ? D3D12_SHADER_BYTECODE{ grassPsBlob->GetBufferPointer(),
                                             grassPsBlob->GetBufferSize() }
                    : D3D12_SHADER_BYTECODE{ psBlob->GetBufferPointer(),
                                             psBlob->GetBufferSize() };
                if (msaaSupported && !createMSAAPipeline(msaaGrassPipelineState))
                    msaaSupported = false;
            }
            psoDesc.PS = { pixelShaderBlob->GetBufferPointer(), pixelShaderBlob->GetBufferSize() };
        }
        // Restore the shared desc, in case anything below reuses it.
        psoDesc.InputLayout = { inputLayout, _countof(inputLayout) };
        psoDesc.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };

        // Extension-motion PSOs: opaque HDR output (SV_Target0) + motion
        // vectors (SV_Target1 R16G16_FLOAT). Opaque blend for both targets;
        // motion is additive-overwrite (no blending needed). Only the HDR
        // opaque, transparent, and additive variants are created because
        // forward extensions always render to the HDR target.
        if (motionPsBlob) {
            psoDesc.PS = { motionPsBlob->GetBufferPointer(), motionPsBlob->GetBufferSize() };
            psoDesc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
            psoDesc.RTVFormats[1] = DXGI_FORMAT_R16G16_FLOAT;
            psoDesc.NumRenderTargets = 2;
            psoDesc.BlendState.RenderTarget[0].BlendEnable = FALSE;
            psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
            if (FAILED(g_dx12.device->CreateGraphicsPipelineState(
                    &psoDesc, IID_PPV_ARGS(&hdrMotionPipelineState))))
                hdrMotionPipelineState.Reset();
            if (!createViewmodelPassPipelines(
                    motionPsBlob.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, 2, 1,
                    hdrMotionViewmodelDepthPipelineState,
                    hdrMotionViewmodelBlendPipelineState)) {
                hdrMotionViewmodelDepthPipelineState.Reset();
                hdrMotionViewmodelBlendPipelineState.Reset();
            }

            // Transparent motion variant
            psoDesc.BlendState.RenderTarget[0].BlendEnable = TRUE;
            psoDesc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
            psoDesc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
            psoDesc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
            psoDesc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
            psoDesc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
            psoDesc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
            psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
            if (FAILED(g_dx12.device->CreateGraphicsPipelineState(
                    &psoDesc, IID_PPV_ARGS(&hdrMotionTransparentPipelineState))))
                hdrMotionTransparentPipelineState.Reset();

            // Additive motion variant
            psoDesc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
            psoDesc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
            psoDesc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ZERO;
            psoDesc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
            if (FAILED(g_dx12.device->CreateGraphicsPipelineState(
                    &psoDesc, IID_PPV_ARGS(&hdrMotionAdditivePipelineState))))
                hdrMotionAdditivePipelineState.Reset();

            // Restore PS and RTV to the normal non-motion setup
            psoDesc.PS = { pixelShaderBlob->GetBufferPointer(), pixelShaderBlob->GetBufferSize() };
            psoDesc.NumRenderTargets = 1;
            psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
            psoDesc.BlendState.RenderTarget[0].BlendEnable = FALSE;
            psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        }

        // Optional material-bindless twins. They deliberately reuse the exact
        // root-parameter numbering and fixed-function state above; only the
        // directly-indexed heap flag and SM 6.6 shaders differ.
        bindlessPipelineReady = false;
        if (bindlessHeap && bindlessHeap->Supported() &&
            std::string(pixelPath).find("clustered_dx12_ps.hlsl") !=
                std::string::npos) {
            const std::wstring shaderDirectory =
                ShaderCacheDX12::ExecutableDirectory() + L"shaders";
            const std::string bindlessPS =
                "#define SGE_BINDLESS_MATERIALS 1\n" + psCode;
            const std::string bindlessHDRPS =
                "#define SGE_BINDLESS_MATERIALS 1\n"
                "#define SGE_HDR_TARGET 1\n" + psCode;
            const std::string bindlessMotionPS =
                "#define SGE_BINDLESS_MATERIALS 1\n"
                "#define SGE_HDR_TARGET 1\n"
                "#define SGE_EXTENSION_MOTION 1\n" + psCode;
            ComPtr<ID3DBlob> bindlessVSBlob;
            std::string errors;
            const bool shadersReady =
                ShaderCacheDX12::CompileCachedDXC(
                    vsCode, L"clustered_dx12_vs.hlsl", L"main", L"vs_6_6",
                    shaderDirectory, &bindlessVSBlob, &errors) &&
                ShaderCacheDX12::CompileCachedDXC(
                    bindlessPS, L"clustered_dx12_ps.hlsl", L"main", L"ps_6_6",
                    shaderDirectory, &bindlessPixelShaderBlob, &errors) &&
                ShaderCacheDX12::CompileCachedDXC(
                    bindlessHDRPS, L"clustered_dx12_ps.hlsl", L"main", L"ps_6_6",
                    shaderDirectory, &bindlessHDRPixelShaderBlob, &errors) &&
                ShaderCacheDX12::CompileCachedDXC(
                    bindlessMotionPS, L"clustered_dx12_ps.hlsl", L"main", L"ps_6_6",
                    shaderDirectory, &bindlessMotionPixelShaderBlob, &errors);
            if (!shadersReady) {
                std::cerr << "Bindless forward shader compile failed\n"
                          << errors << std::endl;
            } else {
                D3D12_ROOT_SIGNATURE_DESC bindlessRootDesc = rootSigDesc;
                bindlessRootDesc.Flags =
                    D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT |
                    D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;
                ComPtr<ID3DBlob> bindlessSignature, bindlessSignatureError;
                if (SUCCEEDED(D3D12SerializeRootSignature(
                        &bindlessRootDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                        &bindlessSignature, &bindlessSignatureError)) &&
                    SUCCEEDED(g_dx12.device->CreateRootSignature(
                        0, bindlessSignature->GetBufferPointer(),
                        bindlessSignature->GetBufferSize(),
                        IID_PPV_ARGS(&bindlessRootSignature)))) {
                    enum class BlendKind { Opaque, Transparent, Additive };
                    auto createBindlessPSO = [&](ComPtr<ID3D12PipelineState>& target,
                            ID3DBlob* ps, DXGI_FORMAT format, BlendKind blend,
                            bool wireframe, bool motion, bool multisampled) {
                        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = psoDesc;
                        desc.pRootSignature = bindlessRootSignature.Get();
                        desc.InputLayout = { inputLayout, _countof(inputLayout) };
                        desc.VS = { bindlessVSBlob->GetBufferPointer(),
                                    bindlessVSBlob->GetBufferSize() };
                        desc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
                        desc.RasterizerState.FillMode = wireframe
                            ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
                        desc.RasterizerState.MultisampleEnable = multisampled;
                        desc.SampleDesc.Count = multisampled
                            ? MSAADX12::SampleCount : 1;
                        desc.SampleDesc.Quality = 0;
                        desc.NumRenderTargets = motion ? 2 : 1;
                        desc.RTVFormats[0] = format;
                        desc.RTVFormats[1] = motion
                            ? DXGI_FORMAT_R16G16_FLOAT : DXGI_FORMAT_UNKNOWN;
                        desc.BlendState.RenderTarget[0].BlendEnable =
                            blend != BlendKind::Opaque;
                        desc.DepthStencilState.DepthWriteMask =
                            blend == BlendKind::Opaque
                                ? D3D12_DEPTH_WRITE_MASK_ALL
                                : D3D12_DEPTH_WRITE_MASK_ZERO;
                        if (blend == BlendKind::Transparent) {
                            desc.BlendState.RenderTarget[0].SrcBlend =
                                D3D12_BLEND_SRC_ALPHA;
                            desc.BlendState.RenderTarget[0].DestBlend =
                                D3D12_BLEND_INV_SRC_ALPHA;
                            desc.BlendState.RenderTarget[0].SrcBlendAlpha =
                                D3D12_BLEND_ONE;
                            desc.BlendState.RenderTarget[0].DestBlendAlpha =
                                D3D12_BLEND_INV_SRC_ALPHA;
                        } else if (blend == BlendKind::Additive) {
                            desc.BlendState.RenderTarget[0].SrcBlend =
                                D3D12_BLEND_SRC_ALPHA;
                            desc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
                            desc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ZERO;
                            desc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
                        }
                        return SUCCEEDED(g_dx12.device->CreateGraphicsPipelineState(
                            &desc, IID_PPV_ARGS(&target)));
                    };
                    bool ok = true;
                    ok = ok && createBindlessPSO(bindlessPipelineState,
                        bindlessPixelShaderBlob.Get(), DXGI_FORMAT_R8G8B8A8_UNORM,
                        BlendKind::Opaque, false, false, false);
                    ok = ok && createBindlessPSO(bindlessWireframePipelineState,
                        bindlessPixelShaderBlob.Get(), DXGI_FORMAT_R8G8B8A8_UNORM,
                        BlendKind::Opaque, true, false, false);
                    ok = ok && createBindlessPSO(bindlessTransparentPipelineState,
                        bindlessPixelShaderBlob.Get(), DXGI_FORMAT_R8G8B8A8_UNORM,
                        BlendKind::Transparent, false, false, false);
                    ok = ok && createBindlessPSO(bindlessAdditivePipelineState,
                        bindlessPixelShaderBlob.Get(), DXGI_FORMAT_R8G8B8A8_UNORM,
                        BlendKind::Additive, false, false, false);
                    ok = ok && createBindlessPSO(bindlessHDRPipelineState,
                        bindlessHDRPixelShaderBlob.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
                        BlendKind::Opaque, false, false, false);
                    ok = ok && createBindlessPSO(bindlessHDRWireframePipelineState,
                        bindlessHDRPixelShaderBlob.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
                        BlendKind::Opaque, true, false, false);
                    ok = ok && createBindlessPSO(bindlessHDRTransparentPipelineState,
                        bindlessHDRPixelShaderBlob.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
                        BlendKind::Transparent, false, false, false);
                    ok = ok && createBindlessPSO(bindlessHDRAdditivePipelineState,
                        bindlessHDRPixelShaderBlob.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
                        BlendKind::Additive, false, false, false);
                    ok = ok && createBindlessPSO(bindlessHDRMotionPipelineState,
                        bindlessMotionPixelShaderBlob.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
                        BlendKind::Opaque, false, true, false);
                    ok = ok && createBindlessPSO(bindlessHDRMotionTransparentPipelineState,
                        bindlessMotionPixelShaderBlob.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
                        BlendKind::Transparent, false, true, false);
                    ok = ok && createBindlessPSO(bindlessHDRMotionAdditivePipelineState,
                        bindlessMotionPixelShaderBlob.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT,
                        BlendKind::Additive, false, true, false);
                    ok = ok && createBindlessPSO(bindlessMSAAPipelineState,
                        bindlessPixelShaderBlob.Get(), DXGI_FORMAT_R8G8B8A8_UNORM,
                        BlendKind::Opaque, false, false, true);
                    ok = ok && createBindlessPSO(bindlessMSAAWireframePipelineState,
                        bindlessPixelShaderBlob.Get(), DXGI_FORMAT_R8G8B8A8_UNORM,
                        BlendKind::Opaque, true, false, true);
                    ok = ok && createBindlessPSO(bindlessMSAATransparentPipelineState,
                        bindlessPixelShaderBlob.Get(), DXGI_FORMAT_R8G8B8A8_UNORM,
                        BlendKind::Transparent, false, false, true);
                    ok = ok && createBindlessPSO(bindlessMSAAAdditivePipelineState,
                        bindlessPixelShaderBlob.Get(), DXGI_FORMAT_R8G8B8A8_UNORM,
                        BlendKind::Additive, false, false, true);
                    bindlessPipelineReady = ok;
                } else if (bindlessSignatureError) {
                    std::cerr << "Bindless forward root signature failed: "
                        << (const char*)bindlessSignatureError->GetBufferPointer()
                        << std::endl;
                }
            }
        }

        // Create constant buffers
        // Per-draw-call buffers need enough slots for all objects per frame
        if (!matrixBuffer.Create(FRAME_COUNT * MAX_DRAW_CALLS_PER_FRAME)) return false;
        if (!objectBuffer.Create(FRAME_COUNT * MAX_DRAW_CALLS_PER_FRAME)) return false;
        
        // Per-frame buffers only need FRAME_COUNT slots
        if (!lightBuffer.Create(FRAME_COUNT * RenderViewCountDX12)) return false;
        if (!cameraBuffer.Create(FRAME_COUNT * RenderViewCountDX12)) return false;
        if (!pointLightsBuffer.Create(FRAME_COUNT * RenderViewCountDX12)) return false;
        if (!impactDecalsBuffer.Create(FRAME_COUNT * RenderViewCountDX12)) return false;
        if (!ddgiBuffer.Create(FRAME_COUNT * RenderViewCountDX12)) return false;
        if (!shBuffer.Create(FRAME_COUNT * RenderViewCountDX12)) return false;
        if (!shadowCascadeBuffer.Create(FRAME_COUNT * RenderViewCountDX12)) return false;
        
        loaded = true;
        if (!msaaSupported) {
            msaaPipelineState.Reset();
            msaaWireframePipelineState.Reset();
            msaaTransparentPipelineState.Reset();
            msaaAdditivePipelineState.Reset();
            hdrMsaaPipelineState.Reset();
            msaaGrassPipelineState.Reset();
            hdrMsaaGrassPipelineState.Reset();
        }
        return true;
    }

    void SetMSAAEnabled(bool enabled) {
        msaaEnabled = enabled && msaaSupported;
    }

    void SetHDRTargetEnabled(bool enabled) { hdrTargetEnabled = enabled; }

    void SetExtensionMotionEnabled(bool enabled) { extensionMotionEnabled = enabled; }

    void SetBindlessHeap(BindlessHeapDX12* heap) { bindlessHeap = heap; }
    void SetBindlessActive(bool enabled) { bindlessRequested = enabled; }
    bool BindlessReady() const { return bindlessPipelineReady; }
    bool BindlessDrawActive() const { return currentDrawBindless; }
    void ForceLegacyNextMaterial() { forceLegacyNextMaterial = true; }

    bool IsViewmodelPassActive() const {
        return viewmodelPass != ViewmodelPass::None;
    }

    bool IsViewmodelDepthPrepass() const {
        return viewmodelPass == ViewmodelPass::Depth;
    }

    ID3D12PipelineState* GetViewmodelPipelineState() const {
        const bool depth = viewmodelPass == ViewmodelPass::Depth;
        if (hdrTargetEnabled) {
            if (extensionMotionEnabled) {
                ID3D12PipelineState* motion = depth
                    ? hdrMotionViewmodelDepthPipelineState.Get()
                    : hdrMotionViewmodelBlendPipelineState.Get();
                if (motion) return motion;
            }
            return depth ? hdrViewmodelDepthPipelineState.Get()
                         : hdrViewmodelBlendPipelineState.Get();
        }
        if (msaaEnabled) {
            return depth ? msaaViewmodelDepthPipelineState.Get()
                         : msaaViewmodelBlendPipelineState.Get();
        }
        return depth ? viewmodelDepthPipelineState.Get()
                     : viewmodelBlendPipelineState.Get();
    }

    ID3D12PipelineState* GetPipelineState(bool wireframe = false) const {
        if (IsViewmodelPassActive())
            return GetViewmodelPipelineState();
        if (currentDrawBindless) {
            if (hdrTargetEnabled) {
                if (wireframe && bindlessHDRWireframePipelineState)
                    return bindlessHDRWireframePipelineState.Get();
                if (extensionMotionEnabled && bindlessHDRMotionPipelineState)
                    return bindlessHDRMotionPipelineState.Get();
                return bindlessHDRPipelineState.Get();
            }
            if (msaaEnabled) {
                if (wireframe && bindlessMSAAWireframePipelineState)
                    return bindlessMSAAWireframePipelineState.Get();
                return bindlessMSAAPipelineState.Get();
            }
            if (wireframe && bindlessWireframePipelineState)
                return bindlessWireframePipelineState.Get();
            return bindlessPipelineState.Get();
        }
        if (hdrTargetEnabled) {
            if (wireframe && hdrWireframePipelineState)
                return hdrWireframePipelineState.Get();
            if (extensionMotionEnabled && hdrMotionPipelineState)
                return hdrMotionPipelineState.Get();
            return hdrPipelineState.Get();
        }
        if (msaaEnabled) {
            if (wireframe && msaaWireframePipelineState)
                return msaaWireframePipelineState.Get();
            return msaaPipelineState.Get();
        }
        if (wireframe && wireframePipelineState)
            return wireframePipelineState.Get();
        return pipelineState.Get();
    }

    ID3D12PipelineState* GetTransparentPipelineState() const {
        if (currentDrawBindless) {
            if (hdrTargetEnabled) {
                if (extensionMotionEnabled &&
                    bindlessHDRMotionTransparentPipelineState)
                    return bindlessHDRMotionTransparentPipelineState.Get();
                return bindlessHDRTransparentPipelineState.Get();
            }
            return msaaEnabled ? bindlessMSAATransparentPipelineState.Get()
                               : bindlessTransparentPipelineState.Get();
        }
        if (hdrTargetEnabled) {
            if (extensionMotionEnabled && hdrMotionTransparentPipelineState)
                return hdrMotionTransparentPipelineState.Get();
            return hdrTransparentPipelineState.Get();
        }
        return msaaEnabled
            ? msaaTransparentPipelineState.Get()
            : transparentPipelineState.Get();
    }

    ID3D12PipelineState* GetAdditivePipelineState() const {
        if (IsViewmodelPassActive())
            return GetViewmodelPipelineState();
        if (currentDrawBindless) {
            if (hdrTargetEnabled) {
                if (extensionMotionEnabled &&
                    bindlessHDRMotionAdditivePipelineState)
                    return bindlessHDRMotionAdditivePipelineState.Get();
                return bindlessHDRAdditivePipelineState.Get();
            }
            return msaaEnabled ? bindlessMSAAAdditivePipelineState.Get()
                               : bindlessAdditivePipelineState.Get();
        }
        if (hdrTargetEnabled) {
            if (extensionMotionEnabled && hdrMotionAdditivePipelineState)
                return hdrMotionAdditivePipelineState.Get();
            return hdrAdditivePipelineState.Get();
        }
        return msaaEnabled
            ? msaaAdditivePipelineState.Get()
            : additivePipelineState.Get();
    }

    ID3D12PipelineState* GetGrassPipelineState() const {
        if (hdrTargetEnabled) return hdrGrassPipelineState.Get();
        return msaaEnabled
            ? msaaGrassPipelineState.Get()
            : grassPipelineState.Get();
    }
    
    void Use(bool wireframe = false) {
        if (!loaded) return;
        drawPipelineKind = DrawPipelineKind::Opaque;
        drawWireframe = wireframe;
        EnsureGraphicsRootBound();
        g_dx12.commandList->SetPipelineState(GetPipelineState(wireframe));
    }

    ID3D12PipelineState* GetHDRMSAAGrassPipelineState() const {
        return hdrMsaaGrassPipelineState.Get();
    }

    ID3D12PipelineState* GetHDRMSAAPipelineState() const {
        return hdrMsaaPipelineState.Get();
    }

    void InvalidateGraphicsRootBinding() { graphicsRootBound = false; }

    void EnsureGraphicsRootBound() {
        if (graphicsRootBound) return;
        ID3D12DescriptorHeap* heaps[] = {
            currentDrawBindless && bindlessHeap
                ? bindlessHeap->Heap() : g_dx12.cbvSrvUavHeap.Get(),
            g_dx12.samplerHeap.Get()
        };
        g_dx12.commandList->SetDescriptorHeaps(2, heaps);
        g_dx12.commandList->SetGraphicsRootSignature(currentDrawBindless
            ? bindlessRootSignature.Get() : rootSignature.Get());
        BindFrameConstants();
        if (activeMatrixDrawCall < kDrawCallsPerView)
            g_dx12.commandList->SetGraphicsRootConstantBufferView(
                    0, matrixBuffer.GetGPUAddress(
                        g_dx12.frameIndex * MAX_DRAW_CALLS_PER_FRAME +
                        currentViewSlot * kDrawCallsPerView +
                        activeMatrixDrawCall));
        if (currentDrawBindless && bindlessHeap) {
            if (bindlessGlobalTableBase != BINDLESS_INVALID_INDEX)
                g_dx12.commandList->SetGraphicsRootDescriptorTable(
                    6, bindlessHeap->GpuHandleAt(bindlessGlobalTableBase));
            g_dx12.commandList->SetGraphicsRootDescriptorTable(
                7, bindlessHeap->GpuHandleAt(BINDLESS_FALLBACK_WHITE));
        }
        graphicsRootBound = true;
    }

    void ActivateMaterialBinding(bool enableBindless) {
        const bool desired = enableBindless && bindlessRequested &&
            bindlessPipelineReady && bindlessHeap && bindlessHeap->Initialized() &&
            bindlessGlobalTableBase != BINDLESS_INVALID_INDEX &&
            !IsViewmodelPassActive();
        if (desired != currentDrawBindless) {
            currentDrawBindless = desired;
            graphicsRootBound = false;
        }
        EnsureGraphicsRootBound();
        ID3D12PipelineState* pso = nullptr;
        if (drawPipelineKind == DrawPipelineKind::Transparent)
            pso = GetTransparentPipelineState();
        else if (drawPipelineKind == DrawPipelineKind::Additive)
            pso = GetAdditivePipelineState();
        else
            pso = GetPipelineState(drawWireframe);
        if (pso) g_dx12.commandList->SetPipelineState(pso);
    }

    // Compute passes such as DDGI bind private descriptor heaps. Descriptor-table
    // bindings become invalid whenever that happens, even though the graphics
    // root signature and root CBVs remain intact. Restore the shared graphics
    // heaps and the global texture table before any following graphics dispatch.
    void RebindGraphicsResourceTables() {
        ID3D12DescriptorHeap* heaps[] = {
            currentDrawBindless && bindlessHeap
                ? bindlessHeap->Heap() : g_dx12.cbvSrvUavHeap.Get(),
            g_dx12.samplerHeap.Get()
        };
        g_dx12.commandList->SetDescriptorHeaps(2, heaps);
        if (currentDrawBindless && bindlessHeap &&
            bindlessGlobalTableBase != BINDLESS_INVALID_INDEX) {
            g_dx12.commandList->SetGraphicsRootDescriptorTable(
                6, bindlessHeap->GpuHandleAt(bindlessGlobalTableBase));
            g_dx12.commandList->SetGraphicsRootDescriptorTable(
                7, bindlessHeap->GpuHandleAt(BINDLESS_FALLBACK_WHITE));
        } else {
            g_dx12.commandList->SetGraphicsRootDescriptorTable(
                6, g_dx12.cbvSrvUavHeap->GetGPUDescriptorHandleForHeapStart());
        }
    }

    void BindFrameConstants() {
        g_dx12.commandList->SetGraphicsRootConstantBufferView(1, lightBuffer.GetGPUAddress(ViewFrameIndex()));
        g_dx12.commandList->SetGraphicsRootConstantBufferView(2, cameraBuffer.GetGPUAddress(ViewFrameIndex()));
        g_dx12.commandList->SetGraphicsRootConstantBufferView(4, pointLightsBuffer.GetGPUAddress(ViewFrameIndex()));
        g_dx12.commandList->SetGraphicsRootConstantBufferView(5, ddgiBuffer.GetGPUAddress(ViewFrameIndex()));
        g_dx12.commandList->SetGraphicsRootConstantBufferView(15, shBuffer.GetGPUAddress(ViewFrameIndex()));
        g_dx12.commandList->SetGraphicsRootConstantBufferView(20,
            shadowCascadeBuffer.GetGPUAddress(ViewFrameIndex()));
        // b10 exists in both layouts. It remains inert in legacy shaders and
        // is consumed only by the bindless cutout variants.
        g_dx12.commandList->SetGraphicsRootConstantBufferView(
            22, impactDecalsBuffer.GetGPUAddress(ViewFrameIndex()));
    }

    // Toggles GPU skinning for the conventional IA vertex shader (b9). Every IA
    // draw must set this explicitly -- root constants persist across draws, so
    // leaving it unwritten lets a skinned mesh's flag carry over to the next
    // static one, which would then pose against a stale palette.
    void SetSkinningEnabled(bool enabled) {
        const UINT value = enabled ? 1u : 0u;
        g_dx12.commandList->SetGraphicsRoot32BitConstants(21, 1, &value, 0);
    }

    // Bracket the first-person weapon's draws to fade it by view distance.
    // `strength` is the player's setting already scaled by the ADS raise, so 0
    // leaves every material untouched and the weapon draws exactly as before.
    void BeginViewmodelSeeThrough(float strength, float nearDistance = 0.30f,
                                  float farDistance = 1.05f,
                                  float nearAlpha = 0.25f,
                                  bool depthPrepass = false) {
        const float clamped =
            (std::max)(0.0f, (std::min)(1.0f, strength));
        viewmodelPass = clamped > 0.0f
            ? (depthPrepass ? ViewmodelPass::Depth : ViewmodelPass::Blend)
            : ViewmodelPass::None;
        // The prepass must keep every opaque surface so it can select the
        // nearest one. The second pass applies the actual distance fade.
        viewmodelSeeThrough = depthPrepass ? 0.0f : clamped;
        viewmodelSeeThroughNear = nearDistance;
        viewmodelSeeThroughFar = farDistance;
        viewmodelSeeThroughNearAlpha = nearAlpha;
    }
    // Must be called before anything else is drawn: the flag is sticky, and a
    // viewmodel fade leaking into the world would fade the whole scene.
    void EndViewmodelSeeThrough() {
        viewmodelSeeThrough = 0.0f;
        viewmodelPass = ViewmodelPass::None;
    }

    void UseTransparent() {
        if (!loaded) return;
        drawPipelineKind = DrawPipelineKind::Transparent;
        drawWireframe = false;
        EnsureGraphicsRootBound();
        if (GetTransparentPipelineState())
            g_dx12.commandList->SetPipelineState(GetTransparentPipelineState());
    }

    void UseAdditive() {
        if (!loaded) return;
        drawPipelineKind = DrawPipelineKind::Additive;
        drawWireframe = false;
        EnsureGraphicsRootBound();
        if (GetAdditivePipelineState())
            g_dx12.commandList->SetPipelineState(GetAdditivePipelineState());
    }

    // Additive with the depth test always passing, for the viewmodel's muzzle
    // flash. MSAA and bindless keep the ordinary additive PSO: those paths have
    // their own variant families and the flash is brief enough that being
    // occluded there is better than widening every one of them.
    void UseMuzzleFlash() {
        if (!loaded) return;
        drawPipelineKind = DrawPipelineKind::Additive;
        drawWireframe = false;
        EnsureGraphicsRootBound();
        if (IsViewmodelPassActive()) {
            if (ID3D12PipelineState* pso = GetViewmodelPipelineState())
                g_dx12.commandList->SetPipelineState(pso);
            return;
        }
        ID3D12PipelineState* pso = nullptr;
        if (!msaaEnabled && !currentDrawBindless) {
            pso = hdrTargetEnabled ? hdrMuzzleFlashPipelineState.Get()
                                   : muzzleFlashPipelineState.Get();
        }
        if (!pso) pso = GetAdditivePipelineState();
        if (pso) g_dx12.commandList->SetPipelineState(pso);
    }

    void BindGlobalResources(ID3D12Resource* shadowMap,
                             ID3D12Resource* irradiance = nullptr,
                             ID3D12Resource* visibility = nullptr,
                             ID3D12Resource* environment = nullptr,
                             ID3D12Resource* brdfLUT = nullptr,
                             ID3D12Resource* sparseProbes = nullptr,
                             ID3D12Resource* sparseCells = nullptr,
                             ID3D12Resource* sparseIndices = nullptr,
                             UINT sparseProbeCount = 0,
                             UINT sparseCellCount = 0,
                             UINT sparseIndexCount = 0,
                             ID3D12Resource* spotShadowAtlas = nullptr) {
        UINT descriptorSize = g_dx12.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = g_dx12.cbvSrvUavHeap->GetCPUDescriptorHandleForHeapStart();

        D3D12_SHADER_RESOURCE_VIEW_DESC shadowDesc = {};
        shadowDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        shadowDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        shadowDesc.Format = DXGI_FORMAT_R32_FLOAT;
        shadowDesc.Texture2DArray.MipLevels = 1;
        shadowDesc.Texture2DArray.ArraySize = SHADOW_CASCADE_COUNT;
        g_dx12.device->CreateShaderResourceView(shadowMap, &shadowDesc, cpuHandle);
        cpuHandle.ptr += descriptorSize;

        D3D12_SHADER_RESOURCE_VIEW_DESC colorDesc = {};
        colorDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        colorDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        colorDesc.Format = irradiance
            ? irradiance->GetDesc().Format : DXGI_FORMAT_R16G16B16A16_FLOAT;
        colorDesc.Texture2D.MipLevels = 1;
        g_dx12.device->CreateShaderResourceView(irradiance, &colorDesc, cpuHandle);
        cpuHandle.ptr += descriptorSize;
        colorDesc.Format = visibility
            ? visibility->GetDesc().Format : DXGI_FORMAT_R16G16_FLOAT;
        g_dx12.device->CreateShaderResourceView(visibility, &colorDesc, cpuHandle);
        cpuHandle.ptr += descriptorSize;

        D3D12_SHADER_RESOURCE_VIEW_DESC environmentDesc = {};
        environmentDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        environmentDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        environmentDesc.Format = environment
            ? environment->GetDesc().Format : DXGI_FORMAT_R32G32B32A32_FLOAT;
        environmentDesc.Texture2D.MipLevels = environment
            ? environment->GetDesc().MipLevels : 1;
        g_dx12.device->CreateShaderResourceView(
            environment, &environmentDesc, cpuHandle);
        cpuHandle.ptr += descriptorSize;

        D3D12_SHADER_RESOURCE_VIEW_DESC brdfDesc = {};
        brdfDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        brdfDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        brdfDesc.Format = DXGI_FORMAT_R32G32_FLOAT;
        brdfDesc.Texture2D.MipLevels = 1;
        g_dx12.device->CreateShaderResourceView(brdfLUT, &brdfDesc, cpuHandle);
        cpuHandle.ptr += descriptorSize;

        const ID3D12Resource* sparseResources[3] = {
            sparseProbes, sparseCells, sparseIndices
        };
        const UINT sparseCounts[3] = {
            (std::max)(sparseProbeCount, 1u),
            (std::max)(sparseCellCount, 1u),
            (std::max)(sparseIndexCount, 1u)
        };
        const UINT sparseStrides[3] = {
            sizeof(DXRProbeRecord), sizeof(DXRProbeGridCell), sizeof(UINT)
        };
        for (UINT i = 0; i < 3; ++i) {
            D3D12_SHADER_RESOURCE_VIEW_DESC sparseDesc = {};
            sparseDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            sparseDesc.Shader4ComponentMapping =
                D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sparseDesc.Format = DXGI_FORMAT_UNKNOWN;
            sparseDesc.Buffer.NumElements = sparseCounts[i];
            sparseDesc.Buffer.StructureByteStride = sparseStrides[i];
            g_dx12.device->CreateShaderResourceView(
                const_cast<ID3D12Resource*>(sparseResources[i]),
                &sparseDesc, cpuHandle);
            cpuHandle.ptr += descriptorSize;
        }

        // [8] t21 - spot shadow atlas. A null resource still needs a real view:
        // the range is declared in the root signature, and a bound table with
        // an uninitialised slot is invalid even when nothing samples it.
        D3D12_SHADER_RESOURCE_VIEW_DESC spotDesc = {};
        spotDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        spotDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        spotDesc.Format = DXGI_FORMAT_R32_FLOAT;
        spotDesc.Texture2DArray.MipLevels = 1;
        spotDesc.Texture2DArray.ArraySize = SPOT_SHADOW_COUNT;
        g_dx12.device->CreateShaderResourceView(
            spotShadowAtlas, &spotDesc, cpuHandle);
        cpuHandle.ptr += descriptorSize;

        bindlessGlobalTableBase = BINDLESS_INVALID_INDEX;
        if (bindlessRequested && bindlessHeap && bindlessHeap->Initialized()) {
            // Must match globalSrvRanges: the table copies every declared slot,
            // and stopping short leaves the last range reading a stale entry.
            D3D12_CPU_DESCRIPTOR_HANDLE sources[9] = {};
            D3D12_CPU_DESCRIPTOR_HANDLE source =
                g_dx12.cbvSrvUavHeap->GetCPUDescriptorHandleForHeapStart();
            for (UINT i = 0; i < _countof(sources); ++i) {
                sources[i] = source;
                source.ptr += descriptorSize;
            }
            bindlessGlobalTableBase =
                bindlessHeap->AllocateTransientTable(sources, _countof(sources));
        }

        RebindGraphicsResourceTables();
    }
    
    // Diagnostic: how many descriptors we had to create this frame, and how many
    // draws found theirs already cached.
    UINT srvCreatesThisFrame = 0;
    UINT srvCacheHitsThisFrame = 0;
    // --- Per-view draw slices ---
    //
    // matrixBuffer/objectBuffer are sized FRAME_COUNT * MAX_DRAW_CALLS_PER_FRAME
    // and indexed by frameIndex alone. That is one slice per in-flight frame,
    // which is correct while one camera renders per frame. The sniper scope
    // renders a second camera into the same frame, and its draws would
    // otherwise consume -- and overwrite -- the main view's slice while the
    // main view's own draws are still being recorded.
    //
    // Each view gets its own slice within the frame. Capacity per view is
    // MAX_DRAW_CALLS_PER_FRAME / kViewSlices so the total allocation is
    // unchanged; the main view keeps the low half at the same base address it
    // used before when the scope is inactive.
    static constexpr UINT kViewSlices = 2;
    static constexpr UINT kDrawCallsPerView =
        MAX_DRAW_CALLS_PER_FRAME / kViewSlices;

    // Selects the slice subsequent draws record into. BeginFrame resets the
    // frame; this only re-bases within it, so it must NOT reset per-frame
    // descriptor state -- calling the frame reset twice would hand out
    // scratch descriptor slots already in use by the main view.
    UINT ViewFrameIndex() const {
        return RenderViewFrameIndexDX12(g_dx12.frameIndex, FRAME_COUNT,
            currentViewSlot == 0 ? RenderViewDX12::Main : RenderViewDX12::Scope);
    }

    void BeginView(UINT viewSlot) {
        currentViewSlot = (viewSlot < kViewSlices) ? viewSlot : 0;
        currentDrawCall = 0;
        activeMatrixDrawCall = 0;
        // Root bindings are per-view: the next draw must rebind against this
        // view's slice rather than inherit the other view's constant address.
        graphicsRootBound = false;
    }

    UINT CurrentViewSlot() const { return currentViewSlot; }

    // Draw statistics stay per-view so the scope's draws are not reported as
    // main-view cost.
    UINT ViewDrawCalls(UINT viewSlot) const {
        return viewSlot < kViewSlices ? viewDrawCalls[viewSlot] : 0;
    }

    // Call this at the start of each frame to reset draw call counter
    void BeginFrame() {
        srvCreatesThisFrame = 0;
        srvCacheHitsThisFrame = 0;
        currentDrawCall = 0;
        activeMatrixDrawCall = 0;
        graphicsRootBound = false;
        currentDrawBindless = false;
        bindlessGlobalTableBase = BINDLESS_INVALID_INDEX;
        forceLegacyNextMaterial = false;
        // Both views start the frame empty, and the main view owns slot 0
        // until a scope pass explicitly re-bases.
        viewDrawCalls[0] = 0;
        viewDrawCalls[1] = 0;
        currentViewSlot = 0;
        // The per-frame scratch region starts ABOVE the persistent one -- resetting
        // to 64 here would hand out slots already owned by cached materials and
        // scribble over their descriptors.
        currentSrvOffset = kPersistentSrvEnd;
    }
    
    // Get the buffer index for per-draw-call data.
    // Base is the frame's slice plus this view's sub-slice, so the two views
    // never address the same element within a frame.
    UINT GetDrawCallIndex() const {
        return g_dx12.frameIndex * MAX_DRAW_CALLS_PER_FRAME +
               currentViewSlot * kDrawCallsPerView + currentDrawCall;
    }
    
    void SetMatrices(const XMMATRIX& model, const XMMATRIX& view,
                     const XMMATRIX& proj, const XMMATRIX& lightSpace,
                     XMFLOAT4 palmRoot = {},
                     const XMMATRIX& previousModel = XMMATRIX{}) {
        UINT bufferIndex = GetDrawCallIndex();
        activeMatrixDrawCall = currentDrawCall;
        
        MatrixBufferDX12 data;
        data.model = XMMatrixTranspose(model);
        data.view = XMMatrixTranspose(view);
        data.projection = XMMatrixTranspose(proj);
        data.lightSpaceMatrix = XMMatrixTranspose(lightSpace);
        const XMMATRIX modelView = model * view;
        data.modelView = XMMatrixTranspose(modelView);
        data.modelViewProjection = XMMatrixTranspose(modelView * proj);
        data.previousViewProjection = XMMatrixTranspose(previousViewProjection);
        data.palmWind = palmWindFrame.wind;
        data.palmPrimary = palmWindFrame.primary;
        data.palmSecondary = palmWindFrame.secondary;
        data.palmPreviousPrimary = palmWindFrame.previousPrimary;
        data.palmPreviousSecondary = palmWindFrame.previousSecondary;
        data.palmParams = palmWindFrame.params;
        data.palmRoot = palmRoot;
        bool hasPreviousModel = false;
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) {
                if (previousModel.r[i].m128_f32[j] != 0.0f) { hasPreviousModel = true; break; }
            }
            if (hasPreviousModel) break;
        }
        data.previousModel = XMMatrixTranspose(hasPreviousModel ? previousModel : model);
        g_currentModelMaxScale = (std::max)({
            XMVectorGetX(XMVector3Length(model.r[0])),
            XMVectorGetX(XMVector3Length(model.r[1])),
            XMVectorGetX(XMVector3Length(model.r[2])) });
        matrixBuffer.CopyData(bufferIndex, data);
        g_dx12.commandList->SetGraphicsRootConstantBufferView(0, matrixBuffer.GetGPUAddress(bufferIndex));
        // Per-frame CBVs (b1/b2/b4/b5/b7) are bound once in Use() -- the only
        // point where the root signature (and thus every binding) is reset.
    }

    void SetPreviousViewProjection(const XMMATRIX& matrix) {
        previousViewProjection = matrix;
    }

    void SetPalmWindFrame(const PalmWindFrameDX12& frame) {
        palmWindFrame = frame;
    }
    
    void SetLight(const XMFLOAT3& pos, int type, const XMFLOAT3& color,
                  float constant, float linear, float quadratic,
                  float ambient, float ambientIntensity,
                  float specular, int shininess,
                  float shadowBias, bool enableShadows,
                  // Fallback only; live callers pass the shadow resource's own
                  // width. Keep in step with SHADOW_MAP_SIZE in ShadowMapDX12.h,
                  // which cannot be included here (it includes this header).
                  float shadowTexelSize = 1.0f / 4096.0f) {
        LightBufferDX12 data;
        data.lightPos = pos;
        data.lightType = type;
        data.lightColor = color;
        data.constant = constant;
        data.linear = linear;
        data.quadratic = quadratic;
        data.ambientStrength = ambient;
        data.ambientLightingIntensity = ambientIntensity;
        // Read from the global rather than taken as a parameter: this signature
        // already carries fourteen, and every caller would pass the same value.
        data.emissiveIntensity = g_emissiveIntensity;
        data.specularStrength = specular;
        data.shininess = shininess;
        data.shadowBias = shadowBias;
        data.enableShadows = enableShadows ? 1 : 0;
        data.shadowTexelSize = shadowTexelSize;
        lightBuffer.CopyData(ViewFrameIndex(), data);
        g_dx12.commandList->SetGraphicsRootConstantBufferView(1, lightBuffer.GetGPUAddress(ViewFrameIndex()));
        ShadowCascadeBufferDX12 cascades = {};
        for (UINT i = 0; i < SHADOW_CASCADE_COUNT; ++i)
            cascades.lightViewProjection[i] = XMMatrixTranspose(g_shadowCascadeMatrices[i]);
        cascades.splitDepths = g_shadowCascadeSplits;
        cascades.texelWorld = g_shadowCascadeTexelWorld;
        cascades.depthRange = g_shadowCascadeDepthRange;
        shadowCascadeBuffer.CopyData(ViewFrameIndex(), cascades);
        g_dx12.commandList->SetGraphicsRootConstantBufferView(20,
            shadowCascadeBuffer.GetGPUAddress(ViewFrameIndex()));
    }
    
    void SetCamera(const XMFLOAT3& pos) {
        CameraBufferDX12 data;
        data.viewPos = pos;
        cameraBuffer.CopyData(ViewFrameIndex(), data);
        g_dx12.commandList->SetGraphicsRootConstantBufferView(2, cameraBuffer.GetGPUAddress(ViewFrameIndex()));
    }
    
    void SetObjectColor(const XMFLOAT3& color) {
        ActivateMaterialBinding(false);
        UINT bufferIndex = GetDrawCallIndex();
        
        ObjectBufferDX12 data;
        data.objectColor = color;
        data.useTexture = 0.0f;
        data.metalness = 0.0f;
        data.roughness = 0.5f;
        data.useNormalMap = 0.0f;
        data.metalRoughMode = 0.0f;
        data.opacity = 1.0f;
        data.viewmodelSeeThrough = viewmodelSeeThrough;
        data.seeThroughNear = viewmodelSeeThroughNear;
        data.seeThroughFar = viewmodelSeeThroughFar;
        data.seeThroughNearAlpha = viewmodelSeeThroughNearAlpha;
        data.glassClarity = glassClarity;
        data.glassGrazing = glassGrazing;
        data.sniperScopeFocalX = sniperScopeFocalX;
        data.sniperScopeFocalY = sniperScopeFocalY;
        data.sniperScopeDebugMode = sniperScopeDebugMode;
        data.sniperLensHalfTangent = sniperLensHalfTangent;
        data.sniperAimTangent = sniperAimTangent;
        data.sniperLensAperture = sniperLensAperture;
        data.sniperScopeUVRotation = 0.0f;

        objectBuffer.CopyData(bufferIndex, data);
        g_dx12.commandList->SetGraphicsRootConstantBufferView(3, objectBuffer.GetGPUAddress(bufferIndex));
    }

    // `detailRelief` turns on the macro normal perturbation in terrain_pbr.
    // It rides materialType because the terrain constant buffer is not visible
    // to the pixel shader: 0/3 are the existing ordinary/authored-path values,
    // and +4 sets the relief bit on either of them.
    void SetTerrainMaterial(bool showAuthoredPaths = false,
                            bool detailRelief = false) {
        ActivateMaterialBinding(false);
        const UINT bufferIndex = GetDrawCallIndex();
        ObjectBufferDX12 data = {};
        data.objectColor = XMFLOAT3(1.0f, 1.0f, 1.0f);
        data.metalness = 0.0f;
        data.roughness = 1.0f;
        data.opacity = 1.0f;
        data.ambientScale = 1.0f;
        // Terrain scans use OpenGL normal maps. DX12 texture coordinates use
        // the opposite V direction, so flip tangent-space green.
        data.normalYSign = -1.0f;
        data.materialType = (showAuthoredPaths ? 3.0f : 0.0f) +
                            (detailRelief ? 4.0f : 0.0f);
        objectBuffer.CopyData(bufferIndex, data);
        g_dx12.commandList->SetGraphicsRootConstantBufferView(
            3, objectBuffer.GetGPUAddress(bufferIndex));
    }

    void SetGrassMaterial(const XMFLOAT3& albedo, float roughness,
                          float ambientScale, float directLightScale,
                          float transmissionStrength, float colorVariation,
                          float normalFalloff = 1.0f) {
        ActivateMaterialBinding(false);
        const UINT bufferIndex = GetDrawCallIndex();
        ObjectBufferDX12 data = {};
        data.objectColor = albedo;
        data.roughness = std::clamp(roughness, 0.04f, 1.0f);
        data.opacity = 1.0f;
        data.ambientScale = (std::max)(ambientScale, 0.0f);
        // Grass shader reuses otherwise inactive material slots. This keeps its
        // cheap PSO compatible with the shared root signature and object buffer.
        data.occlusionStrength = (std::max)(directLightScale, 0.0f);
        data.normalYSign = (std::max)(transmissionStrength, 0.0f);
        data.viewFillStrength = (std::max)(colorVariation, 0.0f);
        data.normalTexW = std::clamp(normalFalloff, 0.0f, 1.0f);
        objectBuffer.CopyData(bufferIndex, data);
        g_dx12.commandList->SetGraphicsRootConstantBufferView(
            3, objectBuffer.GetGPUAddress(bufferIndex));
    }
    
    // `cacheOwner`, when given, is the material these textures belong to. Its three
    // descriptors are then created ONCE into the heap's persistent region and
    // reused on every later draw, instead of being rebuilt per draw. Pass it for
    // anything drawn from a long-lived SceneMaterial; leave it null for one-off
    // materials assembled on the fly.
    void SetObjectMaterial(const XMFLOAT3& color, bool useTex, bool useNorm, float metal, float rough,
                          ID3D12Resource* albedo, ID3D12Resource* normal, ID3D12Resource* metalRough,
                          bool roughnessOnly = false, float opacity = 1.0f, bool alphaCut = false,
                          SceneMaterial* cacheOwner = nullptr,
                          bool alphaFromLuminance = false,
                          float ambientScale = 1.0f,
                          float occlusionStrength = 0.0f,
                          float normalYSign = 1.0f,
                          float viewFillStrength = 0.0f,
                           float specularScale = 1.0f,
                           float materialType = 0.0f,
                           float materialTime = 0.0f,
                           float scopeUVRotation = 0.0f) {
        UINT bufferIndex = GetDrawCallIndex();

        ObjectBufferDX12 data;
        data.objectColor = color;
        data.useTexture = useTex ? 1.0f : 0.0f;
        data.useNormalMap = useNorm ? 1.0f : 0.0f;
        data.metalness = metal;
        data.roughness = rough;
        data.metalRoughMode = metalRough ? (roughnessOnly ? 2.0f : 1.0f) : 0.0f;
        data.opacity = opacity;
        // Cutout style and threshold come from the owning material, so callers
        // need not thread two more positional arguments through every draw
        // path. Foliage keeps mode 1 (edge bleed + dark-texel lift); anything
        // else alpha-tested is mode 3 and clips at its authored cutoff.
        const bool foliageShading = !cacheOwner || cacheOwner->foliageShading;
        data.alphaCut = alphaFromLuminance ? 2.0f
                        : (alphaCut ? (foliageShading ? 1.0f : 3.0f)
                                    : (cacheOwner && cacheOwner->alphaBlend
                                        ? -1.0f : 0.0f));
        data.alphaCutoff = cacheOwner ? cacheOwner->alphaCutoff : 0.20f;
        data.ambientScale = ambientScale;
        data.occlusionStrength = occlusionStrength;
        data.normalYSign = normalYSign;
        data.viewFillStrength = viewFillStrength;
        data.specularScale = specularScale;
        data.materialType = materialType;
        data.materialTime = materialTime;
        data.viewmodelSeeThrough = viewmodelSeeThrough;
        data.seeThroughNear = viewmodelSeeThroughNear;
        data.seeThroughFar = viewmodelSeeThroughFar;
        data.seeThroughNearAlpha = viewmodelSeeThroughNearAlpha;
        data.glassClarity = glassClarity;
        data.glassGrazing = glassGrazing;
        data.sniperScopeFocalX = sniperScopeFocalX;
        data.sniperScopeFocalY = sniperScopeFocalY;
        data.sniperScopeDebugMode = sniperScopeDebugMode;
        data.sniperLensHalfTangent = sniperLensHalfTangent;
        data.sniperAimTangent = sniperAimTangent;
        data.sniperLensAperture = sniperLensAperture;
        data.sniperScopeUVRotation = scopeUVRotation;
        if (useNorm && normal) {
            const D3D12_RESOURCE_DESC nd = normal->GetDesc();
            data.normalTexW = (float)nd.Width;
            data.normalTexH = (float)nd.Height;
        }

        // Emissive comes off the material rather than the argument list -- this
        // signature already carries twenty parameters, and every caller that
        // could supply one already passes a cacheOwner.
        ID3D12Resource* const emissive =
            cacheOwner ? cacheOwner->emissiveTexture.Get() : nullptr;
        if (emissive) {
            data.useEmissiveMap = 1.0f;
            // glTF leaves emissiveFactor at black when a model ships an emissive
            // map but no factor. Treat that as "show the map as authored"
            // instead of multiplying it away to nothing.
            const XMFLOAT3& e = cacheOwner->emissiveFactor;
            const bool factorSet = e.x > 0.0f || e.y > 0.0f || e.z > 0.0f;
            data.emissiveFactor = factorSet ? e : XMFLOAT3(1.0f, 1.0f, 1.0f);
        }

        const bool allowBindless = cacheOwner && !forceLegacyNextMaterial;
        forceLegacyNextMaterial = false;
        if (allowBindless && bindlessRequested && bindlessPipelineReady &&
            bindlessHeap && bindlessHeap->Initialized()) {
            const UINT generation = bindlessHeap->Allocator().Generation();
            if (cacheOwner->bindlessGeneration != generation) {
                cacheOwner->InvalidateTextureBindings();
                cacheOwner->bindlessGeneration = generation;
                cacheOwner->bindlessAlbedoIndex = bindlessHeap->RegisterTexture(
                    cacheOwner->baseColorTexture.Get(),
                    BINDLESS_FALLBACK_WHITE);
                cacheOwner->bindlessNormalIndex = bindlessHeap->RegisterTexture(
                    cacheOwner->normalTexture.Get(),
                    BINDLESS_FALLBACK_NORMAL);
                cacheOwner->bindlessMetalRoughIndex =
                    bindlessHeap->RegisterTexture(
                        cacheOwner->metallicRoughnessTexture.Get(),
                        BINDLESS_FALLBACK_METALROUGH);
                // Black is the additive no-op, so a material without an emissive
                // map resolves to adding nothing.
                cacheOwner->bindlessEmissiveIndex =
                    bindlessHeap->RegisterTexture(
                        cacheOwner->emissiveTexture.Get(),
                        BINDLESS_FALLBACK_BLACK);
            }
            data.bindlessTextureIndices[0] = cacheOwner->bindlessAlbedoIndex;
            data.bindlessTextureIndices[1] = cacheOwner->bindlessNormalIndex;
            data.bindlessTextureIndices[2] = cacheOwner->bindlessMetalRoughIndex;
            data.bindlessTextureIndices[3] = cacheOwner->bindlessEmissiveIndex;
        }

        ActivateMaterialBinding(allowBindless);

        objectBuffer.CopyData(bufferIndex, data);
        g_dx12.commandList->SetGraphicsRootConstantBufferView(3, objectBuffer.GetGPUAddress(bufferIndex));

        if (currentDrawBindless) return;

        // Textures. Emissive joins the gate: a material carrying only an
        // emissive map still needs its table bound, or the reticle-style
        // markings that motivated it would never be sampled.
        if (useTex || useNorm || emissive) {
             UINT descriptorSize = g_dx12.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

             D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle;
             D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle;

             // Already cached? Then the descriptors are still valid -- a material's
             // textures never change after load -- so just point the table at them.
             // This is the whole win: no CreateShaderResourceView calls at all.
             if (cacheOwner && cacheOwner->srvHeapSlot != ~0u) {
                 SrvHandlesAt(cacheOwner->srvHeapSlot, cpuHandle, gpuHandle);
                 g_dx12.commandList->SetGraphicsRootDescriptorTable(7, gpuHandle);
                 ++srvCacheHitsThisFrame;
                 return;
             }
             srvCreatesThisFrame += MATERIAL_SRV_COUNT;

             if (cacheOwner) {
                 // First draw of this material: claim a permanent slot and build its
                 // descriptors there, once.
                 const UINT slot = ReservePersistentMaterialSrvs();
                 if (slot != ~0u) {
                     cacheOwner->srvHeapSlot = slot;
                     SrvHandlesAt(slot, cpuHandle, gpuHandle);
                 } else if (!ReserveMaterialSrvs(cpuHandle, gpuHandle)) {
                     return;   // persistent region full AND the frame's scratch is too
                 }
             } else if (!ReserveMaterialSrvs(cpuHandle, gpuHandle)) {
                 return;  // heap full this frame
             }

             // Create SRVs
             // Albedo (t1)
             if (albedo) {
                 g_dx12.device->CreateShaderResourceView(albedo, nullptr, cpuHandle);
             } else {
                 // Create null SRV? or just skip? safer to create null
                 D3D12_SHADER_RESOURCE_VIEW_DESC nullDesc = {};
                 nullDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                 nullDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                 nullDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                 g_dx12.device->CreateShaderResourceView(nullptr, &nullDesc, cpuHandle);
             }
             cpuHandle.ptr += descriptorSize;
             
             // Normal (t4)
             if (normal) {
                 g_dx12.device->CreateShaderResourceView(normal, nullptr, cpuHandle);
             } else {
                 D3D12_SHADER_RESOURCE_VIEW_DESC nullDesc = {};
                 nullDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                 nullDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                 nullDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                 g_dx12.device->CreateShaderResourceView(nullptr, &nullDesc, cpuHandle);
             }
             cpuHandle.ptr += descriptorSize;
             
             // MetalRough (t5)
             if (metalRough) {
                 g_dx12.device->CreateShaderResourceView(metalRough, nullptr, cpuHandle);
             } else {
                 D3D12_SHADER_RESOURCE_VIEW_DESC nullDesc = {};
                 nullDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                 nullDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                 nullDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                 g_dx12.device->CreateShaderResourceView(nullptr, &nullDesc, cpuHandle);
             }
             cpuHandle.ptr += descriptorSize;

             // Emissive (t6). Almost every material leaves this null; the shader
             // gates on useEmissiveMap, so the descriptor is only ever sampled
             // when one was actually bound.
             if (emissive) {
                 g_dx12.device->CreateShaderResourceView(emissive, nullptr, cpuHandle);
             } else {
                 D3D12_SHADER_RESOURCE_VIEW_DESC nullDesc = {};
                 nullDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                 nullDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                 nullDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                 g_dx12.device->CreateShaderResourceView(nullptr, &nullDesc, cpuHandle);
             }

             // Bind table
             g_dx12.commandList->SetGraphicsRootDescriptorTable(7, gpuHandle);
        }
    }

    void SetWaterMaterial(const XMFLOAT3& color, float roughness,
                          float opacity, bool ocean, float time) {
        SetObjectMaterial(
            color, false, false, 0.0f, roughness,
            nullptr, nullptr, nullptr, false, opacity, false, nullptr, false,
            1.0f, 0.0f, 1.0f, 0.0f, ocean ? 0.64f : 0.78f,
            ocean ? 2.0f : 1.0f, time);
    }

    // Unlit soft-sprite material for smoke billboards: samples `smokeTex`'s alpha
    // to shape a translucent puff tinted by `color`, faded by `opacity`. Use with
    // UseTransparent(). Binds the sprite as albedo (t1) and flags smokeMode.
    void SetSmokeMaterial(const XMFLOAT3& color, float opacity, ID3D12Resource* smokeTex) {
        ActivateMaterialBinding(false);
        UINT bufferIndex = GetDrawCallIndex();
        ObjectBufferDX12 data;
        data.objectColor = color;
        data.useTexture = 1.0f;
        data.useNormalMap = 0.0f;
        data.metalness = 0.0f;
        data.roughness = 1.0f;
        data.metalRoughMode = 0.0f;
        data.opacity = opacity;
        data.smokeMode = 1.0f;
        objectBuffer.CopyData(bufferIndex, data);
        g_dx12.commandList->SetGraphicsRootConstantBufferView(3, objectBuffer.GetGPUAddress(bufferIndex));

        UINT descriptorSize = g_dx12.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle;
        D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle;
        if (!ReserveMaterialSrvs(cpuHandle, gpuHandle)) return;  // heap full this frame

        D3D12_SHADER_RESOURCE_VIEW_DESC nullDesc = {};
        nullDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        nullDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        nullDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;

        if (smokeTex) g_dx12.device->CreateShaderResourceView(smokeTex, nullptr, cpuHandle);
        else          g_dx12.device->CreateShaderResourceView(nullptr, &nullDesc, cpuHandle);
        cpuHandle.ptr += descriptorSize;
        g_dx12.device->CreateShaderResourceView(nullptr, &nullDesc, cpuHandle);  // t4
        cpuHandle.ptr += descriptorSize;
        g_dx12.device->CreateShaderResourceView(nullptr, &nullDesc, cpuHandle);  // t5

        g_dx12.commandList->SetGraphicsRootDescriptorTable(7, gpuHandle);
    }

    // Unlit solid glow for tracers. smokeMode=2 selects the emissive shader path;
    // UseAdditive() controls how it is composited over the scene.
    void SetEmissiveMaterial(const XMFLOAT3& color, float opacity) {
        ActivateMaterialBinding(false);
        UINT bufferIndex = GetDrawCallIndex();
        ObjectBufferDX12 data = {};
        data.objectColor = color;
        data.roughness = 1.0f;
        data.opacity = opacity;
        data.smokeMode = 2.0f;
        data.viewmodelSeeThrough = viewmodelSeeThrough;
        data.seeThroughNear = viewmodelSeeThroughNear;
        data.seeThroughFar = viewmodelSeeThroughFar;
        data.seeThroughNearAlpha = viewmodelSeeThroughNearAlpha;
        data.glassClarity = glassClarity;
        data.glassGrazing = glassGrazing;
        data.sniperScopeFocalX = sniperScopeFocalX;
        data.sniperScopeFocalY = sniperScopeFocalY;
        data.sniperScopeDebugMode = sniperScopeDebugMode;
        data.sniperLensHalfTangent = sniperLensHalfTangent;
        data.sniperAimTangent = sniperAimTangent;
        data.sniperLensAperture = sniperLensAperture;
        data.sniperScopeUVRotation = 0.0f;
        objectBuffer.CopyData(bufferIndex, data);
        g_dx12.commandList->SetGraphicsRootConstantBufferView(
            3, objectBuffer.GetGPUAddress(bufferIndex));
    }

    // Call this after each DrawCube/DrawPlane to advance to the next buffer slot
    void NextDrawCall() {
        if (currentViewSlot < kViewSlices)
            ++viewDrawCalls[currentViewSlot];
        currentDrawCall++;
        // Clamped to the per-view capacity, not the whole frame's: overrunning
        // into the next slice would scribble over the other view's matrices.
        if (currentDrawCall >= kDrawCallsPerView) {
            currentDrawCall = kDrawCallsPerView - 1;
        }
    }
    
    void SetPointLights(int numLights, const std::vector<PointLightDataDX12>& lights) {
        PointLightsBufferDX12 data = {};
        data.numPointLights = numLights;
        int count = (numLights < 64) ? numLights : 64;
        for (int i = 0; i < count; i++) {
            data.lights[i] = lights[i];
        }
        data.spotShadowCount = static_cast<int>(g_spotShadowActiveCount);
        for (UINT s = 0; s < SPOT_SHADOW_COUNT; ++s)
            data.spotShadowMatrices[s] = XMMatrixTranspose(g_spotShadowMatrices[s]);
        pointLightsBuffer.CopyData(ViewFrameIndex(), data);
        g_dx12.commandList->SetGraphicsRootConstantBufferView(4, pointLightsBuffer.GetGPUAddress(ViewFrameIndex()));
    }
    
    void SetImpactDecals(const std::vector<ImpactDecalDataDX12>& decals,
                         bool cutoutsEnabled) {
        ImpactDecalsBufferDX12 data = {};
        const int count = (int)((decals.size() < 64) ? decals.size() : 64);
        data.numDecals = count;
        data.cutoutsEnabled = cutoutsEnabled ? 1.0f : 0.0f;
        for (int i = 0; i < count; ++i) data.decals[i] = decals[i];
        impactDecalsBuffer.CopyData(ViewFrameIndex(), data);
    }

    void SetDDGI(bool enabled, float gi_intensity, float normal_bias,
                 float probe_spacing, UINT sparseProbeCount = 0,
                 UINT sparseCellCount = 0, float sparseCellSize = 0.0f) {
        DDGIBufferDX12 data = {};
        data.probeGridOrigin = XMFLOAT3(-27.5f, 0.5f, -27.5f);
        data.probeSpacing = probe_spacing;
        data.probeCountX = 12;
        data.probeCountY = 4;
        data.probeCountZ = 12;
        data.maxRayDistance = 24.0f;
        data.normalBias = normal_bias;
        data.viewBias = 0.01f;
        data.irradianceGamma = 1.0f;
        data.giIntensity = gi_intensity;
        data.irradianceTexWidth = 8;
        data.irradianceTexHeight = 8;
        data.visibilityTexWidth = 16;
        data.visibilityTexHeight = 16;
        data.ddgiEnabled = enabled ? 1 : 0;
        data.sparseProbeCount = static_cast<int>(sparseProbeCount);
        data.sparseCellCount = static_cast<int>(sparseCellCount);
        data.sparseCellSize = sparseCellSize;
        ddgiBuffer.CopyData(ViewFrameIndex(), data);
        g_dx12.commandList->SetGraphicsRootConstantBufferView(5, ddgiBuffer.GetGPUAddress(ViewFrameIndex()));
    }

    // Stores the SH coefficients computed once at load from the sky HDRI.
    // Cheap to call once; SetSH() re-uploads per-frame like the other
    // per-frame CBVs since the upload buffer is double/triple-buffered.
    void SetSkyIrradiance(const std::array<XMFLOAT3, 9>& coeffs, float intensity) {
        pendingSHCoeffs = coeffs;
        pendingSkyIntensity = intensity;
        skyIrradianceValid = true;
    }

    void SetSH() {
        SHBufferDX12 data = {};
        for (int i = 0; i < 9; i++) {
            const XMFLOAT3& c = pendingSHCoeffs[i];
            data.shCoeffs[i] = XMFLOAT4(c.x, c.y, c.z, 0.0f);
        }
        data.skyIntensity = skyIrradianceValid ? pendingSkyIntensity : 0.0f;
        shBuffer.CopyData(ViewFrameIndex(), data);
        g_dx12.commandList->SetGraphicsRootConstantBufferView(15, shBuffer.GetGPUAddress(ViewFrameIndex()));
    }
};

#endif // SHADER_DX12_H
