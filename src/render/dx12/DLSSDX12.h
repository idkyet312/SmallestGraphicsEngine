#pragma once

// NVIDIA DLSS Super Resolution through Streamline. At 100% input size it runs
// as DLAA; below 100% it reconstructs the scene into a display-sized target.
//
// Manual-hooking integration. sl.interposer.dll is loaded at runtime and only
// the DXGI factory used to create the swapchain is upgraded to an SL proxy, so
// Present reaches Streamline's per-frame bookkeeping. The D3D12 device stays
// native: DXR, mesh shaders and the bindless heap never see a proxy. Without
// the DLLs, on a non-NVIDIA GPU, or in builds without the SDK every entry point
// is a no-op and Available() is false.

#include <d3d12.h>
#include <dxgi1_6.h>
#include <DirectXMath.h>

struct DLSSFrameInputs {
    ID3D12GraphicsCommandList* cmdList = nullptr;
    // HDR scene colour, read as the jittered input. At native resolution the
    // DLAA result is copied back here; Super Resolution writes output instead.
    // Expected in NON_PIXEL_SHADER_RESOURCE and left there.
    ID3D12Resource* color = nullptr;
    // Scene depth, standard Z (near 0, far 1), in depthState.
    ID3D12Resource* depth = nullptr;
    UINT depthState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    // UV-space motion, current minus previous, NON_PIXEL_SHADER_RESOURCE.
    ID3D12Resource* motion = nullptr;
    // RR guides, all current-frame and at input resolution. Required only
    // when rayReconstruction is true.
    ID3D12Resource* normalRoughness = nullptr;
    ID3D12Resource* diffuseAlbedo = nullptr;
    ID3D12Resource* specularAlbedo = nullptr;
    ID3D12Resource* specularHitDistance = nullptr;
    bool rayReconstruction = false;
    // The resolve strips jitter from the motion vectors whenever RR was
    // requested for the frame. Set on a Super Resolution fallback after a
    // failed RR evaluate, so DLSS is not told those vectors carry jitter.
    bool motionUnjittered = false;
    UINT width = 0;
    UINT height = 0;
    UINT outputWidth = 0;
    UINT outputHeight = 0;
    // Optional display-sized UAV for Super Resolution. Null means native DLAA,
    // whose result is copied back into color as before.
    ID3D12Resource* output = nullptr;
    float screenPercentage = 100.0f;
    // Sub-pixel offset baked into this frame's projection, in pixels
    // (Scene::temporalJitterPixels).
    DirectX::XMFLOAT2 jitterPixels = { 0.0f, 0.0f };
    // Unjittered, row-vector (DirectXMath) matrices.
    DirectX::XMFLOAT4X4 view = {};
    DirectX::XMFLOAT4X4 projection = {};
    float nearPlane = 0.1f;
    float farPlane = 1000.0f;
    float verticalFovRadians = 1.0f;
    // Discard DLSS history: camera cut, level load, teleport.
    bool reset = false;
};

namespace DLSS {

// Presets exposed in the UI. L and M are the DLSS 4.5 second-generation
// transformer models; K is the DLSS 4 transformer, SL's DLAA default.
enum class Preset : int { K = 0, L = 1, M = 2 };

struct Settings {
    bool enabled = false;
    Preset preset = Preset::L;
    float screenPercentage = 100.0f;
    bool rayReconstruction = false;
    // RR upscales at the end of the HDR scene (where Super Resolution runs)
    // from the screen-percentage render size, instead of native mid-frame.
    bool rayReconstructionUpscale = false;
    // Flips the jitter sign handed to DLSS. Only for diagnosing a convention
    // mismatch: a wrong sign reads as a soft, wobbling image on a still view.
    bool invertJitter = false;
};

// What the last Evaluate handed DLSS / RR, for the jitter debug overlay.
struct EvalDebug {
    bool valid = false;
    bool rayReconstruction = false;
    bool superResolution = false;
    bool motionVectorsJittered = false;
    bool reset = false;
    float jitterX = 0.0f, jitterY = 0.0f;   // consts.jitterOffset, render px
    float mvecScaleX = 0.0f, mvecScaleY = 0.0f;
    UINT inputWidth = 0, inputHeight = 0, outputWidth = 0, outputHeight = 0;
    int mode = 0;                            // sl::DLSSMode
    uint32_t frameIndex = 0;
    // Real sizes of every tagged resource (0 = not tagged). Inputs are tagged
    // with the render extent, so any that differ are read wrongly.
    static constexpr int kResourceCount = 8;
    UINT resourceWidth[kResourceCount] = {};
    UINT resourceHeight[kResourceCount] = {};
};
// Order of EvalDebug::resourceWidth/Height.
inline const char* EvalDebugResourceName(int i) {
    static const char* names[EvalDebug::kResourceCount] = {
        "colour", "depth", "motion", "normal+rough", "diffuse albedo",
        "specular albedo", "spec hit dist", "output" };
    return names[i];
}

#if defined(SGE_WITH_STREAMLINE)
// Loads sl.interposer.dll from the exe directory, verifies its signature and
// calls slInit. Call before the swapchain is created.
bool Startup();
// Registers the device and checks DLSS support on its adapter.
void SetDevice(ID3D12Device* device, IDXGIAdapter1* adapter);
// Returns an SL proxy of the factory to create the swapchain from, or the
// factory itself (AddRef'd) when Streamline is not running. Release it after.
IDXGIFactory2* SwapChainFactory(IDXGIFactory2* nativeFactory);
bool Available();
bool RayReconstructionAvailable();
const char* RayReconstructionStatus();
// Whether the last successful evaluate ran Ray Reconstruction.
bool LastEvaluatedRR();
const EvalDebug& LastEvalDebug();
const char* Status();
Settings& GetSettings();
// Returns an SDK-supported render size for the requested percentage. False
// means use native resolution; the SDK could not validate an SR input size.
bool RenderSize(UINT displayWidth, UINT displayHeight, float percentage,
                UINT& renderWidth, UINT& renderHeight);
// Records DLAA or Super Resolution onto inputs.cmdList. Returns false when
// unavailable or evaluation failed; the caller can spatially present color.
bool Evaluate(const DLSSFrameInputs& inputs);
// Frees the DLSS feature's resources, e.g. before a resize.
void ReleaseResources();
void Shutdown();
#else
inline bool Startup() { return false; }
inline void SetDevice(ID3D12Device*, IDXGIAdapter1*) {}
inline IDXGIFactory2* SwapChainFactory(IDXGIFactory2* nativeFactory) {
    if (nativeFactory) nativeFactory->AddRef();
    return nativeFactory;
}
inline bool Available() { return false; }
inline bool RayReconstructionAvailable() { return false; }
inline const char* RayReconstructionStatus() {
    return "Built without the Streamline SDK";
}
inline const char* Status() { return "Built without the Streamline SDK"; }
inline bool LastEvaluatedRR() { return false; }
inline const EvalDebug& LastEvalDebug() { static EvalDebug d; return d; }
inline Settings& GetSettings() { static Settings settings; return settings; }
inline bool RenderSize(UINT width, UINT height, float, UINT& renderWidth,
                       UINT& renderHeight) {
    renderWidth = width;
    renderHeight = height;
    return false;
}
inline bool Evaluate(const DLSSFrameInputs&) { return false; }
inline void ReleaseResources() {}
inline void Shutdown() {}
#endif

} // namespace DLSS
