// Streamline / DLSS implementation. See DLSSDX12.h for the integration shape.
//
// Kept in its own translation unit because sl_security.h defines non-inline
// functions, and because only this file needs the Streamline headers.

#include "DLSSDX12.h"

#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss.h>
#include <sl_dlss_d.h>
#include <sl_security.h>

#include <wrl/client.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")

using Microsoft::WRL::ComPtr;
using namespace DirectX;

namespace DLSS {
namespace {

struct Api {
    PFun_slInit* init = nullptr;
    PFun_slShutdown* shutdown = nullptr;
    PFun_slIsFeatureSupported* isFeatureSupported = nullptr;
    PFun_slIsFeatureLoaded* isFeatureLoaded = nullptr;
    PFun_slSetD3DDevice* setD3DDevice = nullptr;
    PFun_slUpgradeInterface* upgradeInterface = nullptr;
    PFun_slGetNewFrameToken* getNewFrameToken = nullptr;
    PFun_slSetConstants* setConstants = nullptr;
    PFun_slSetTagForFrame* setTagForFrame = nullptr;
    PFun_slEvaluateFeature* evaluateFeature = nullptr;
    PFun_slFreeResources* freeResources = nullptr;
    PFun_slGetFeatureFunction* getFeatureFunction = nullptr;
    PFun_slDLSSSetOptions* dlssSetOptions = nullptr;
    PFun_slDLSSGetOptimalSettings* dlssGetOptimalSettings = nullptr;
    PFun_slDLSSDSetOptions* dlssdSetOptions = nullptr;
    PFun_slDLSSDGetOptimalSettings* dlssdGetOptimalSettings = nullptr;
};

struct State {
    HMODULE module = nullptr;
    Api api;
    bool initialized = false;   // slInit succeeded
    bool supported = false;     // DLSS usable on the selected adapter
    bool rrSupported = false;
    std::string rrStatus = "Not started";
    ID3D12Device* device = nullptr;
    std::string status = "Not started";
    Settings settings;

    ComPtr<ID3D12Resource> output;
    UINT outputWidth = 0;
    UINT outputHeight = 0;
    DXGI_FORMAT outputFormat = DXGI_FORMAT_UNKNOWN;

    uint32_t frameIndex = 0;
    bool havePrevious = false;
    XMFLOAT4X4 previousViewProjection = {};
    // DLSS and DLSS-RR keep separate histories. Switching between them hands
    // the incoming feature whatever it accumulated before it went idle.
    bool lastEvaluatedRR = false;

    bool optionsSet = false;
    Preset appliedPreset = Preset::K;
    UINT appliedWidth = 0;
    UINT appliedHeight = 0;
    UINT appliedOutputWidth = 0;
    UINT appliedOutputHeight = 0;
    sl::DLSSMode appliedMode = sl::DLSSMode::eOff;
    bool externalOutputReadable = false;
};

State& S() {
    static State state;
    return state;
}

// Also appended to dlss.log in the working directory: the game's console is
// not capturable, and "why is DLSS off" must be answerable from a file.
void Log(const std::string& message) {
    // Per-frame failures repeat; record each distinct message once in a row.
    if (S().status == message) return;
    S().status = message;
    std::fprintf(stderr, "[DLSS] %s\n", message.c_str());
    FILE* file = nullptr;
    if (fopen_s(&file, "dlss.log", "a") == 0 && file) {
        std::fprintf(file, "%s\n", message.c_str());
        std::fclose(file);
    }
}

std::wstring ExeDirectory() {
    wchar_t path[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    std::wstring dir(path, length);
    const size_t slash = dir.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : dir.substr(0, slash);
}

template <typename T>
bool Resolve(HMODULE module, const char* name, T*& out) {
    out = reinterpret_cast<T*>(GetProcAddress(module, name));
    return out != nullptr;
}

sl::float4x4 ToSL(const XMFLOAT4X4& m) {
    sl::float4x4 out;
    for (uint32_t r = 0; r < 4; ++r)
        out.setRow(r, sl::float4(m.m[r][0], m.m[r][1], m.m[r][2], m.m[r][3]));
    return out;
}

sl::float4x4 ToSL(FXMMATRIX m) {
    XMFLOAT4X4 stored;
    XMStoreFloat4x4(&stored, m);
    return ToSL(stored);
}

sl::DLSSPreset ToSL(Preset preset) {
    switch (preset) {
    case Preset::L: return sl::DLSSPreset::ePresetL;
    case Preset::M: return sl::DLSSPreset::ePresetM;
    default:        return sl::DLSSPreset::ePresetK;
    }
}

sl::DLSSMode ModeForPercentage(float percentage) {
    if (percentage >= 99.5f) return sl::DLSSMode::eDLAA;
    if (percentage < 40.0f) return sl::DLSSMode::eUltraPerformance;
    if (percentage < 58.0f) return sl::DLSSMode::eMaxPerformance;
    if (percentage < 69.0f) return sl::DLSSMode::eBalanced;
    if (percentage < 85.0f) return sl::DLSSMode::eMaxQuality;
    return sl::DLSSMode::eUltraQuality;
}

void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &barrier);
}

// DLSS writes to a UAV of the output size and format. The scene colour target
// is also its input, so the result lands here and is copied back.
bool EnsureOutput(ID3D12Resource* color) {
    State& s = S();
    const D3D12_RESOURCE_DESC colorDesc = color->GetDesc();
    if (s.output && s.outputWidth == colorDesc.Width &&
        s.outputHeight == colorDesc.Height &&
        s.outputFormat == colorDesc.Format)
        return true;

    s.output.Reset();
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = colorDesc.Width;
    desc.Height = colorDesc.Height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = colorDesc.Format;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    if (FAILED(s.device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
            IID_PPV_ARGS(&s.output)))) {
        Log("Failed to create the DLSS output texture");
        return false;
    }
    s.output->SetName(L"DLSS Output");
    s.outputWidth = static_cast<UINT>(colorDesc.Width);
    s.outputHeight = colorDesc.Height;
    s.outputFormat = colorDesc.Format;
    return true;
}

} // namespace

bool Startup() {
    State& s = S();
    if (s.initialized) return true;

    const std::wstring dir = ExeDirectory();
    const std::wstring interposer = dir + L"\\sl.interposer.dll";
    if (GetFileAttributesW(interposer.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Log("sl.interposer.dll not found next to the executable");
        return false;
    }
    if (!sl::security::verifyEmbeddedSignature(interposer.c_str())) {
        Log("sl.interposer.dll failed signature verification");
        return false;
    }
    s.module = LoadLibraryW(interposer.c_str());
    if (!s.module) {
        Log("LoadLibrary(sl.interposer.dll) failed");
        return false;
    }

    Api& a = s.api;
    if (!Resolve(s.module, "slInit", a.init) ||
        !Resolve(s.module, "slShutdown", a.shutdown) ||
        !Resolve(s.module, "slIsFeatureSupported", a.isFeatureSupported) ||
        !Resolve(s.module, "slIsFeatureLoaded", a.isFeatureLoaded) ||
        !Resolve(s.module, "slSetD3DDevice", a.setD3DDevice) ||
        !Resolve(s.module, "slUpgradeInterface", a.upgradeInterface) ||
        !Resolve(s.module, "slGetNewFrameToken", a.getNewFrameToken) ||
        !Resolve(s.module, "slSetConstants", a.setConstants) ||
        !Resolve(s.module, "slSetTagForFrame", a.setTagForFrame) ||
        !Resolve(s.module, "slEvaluateFeature", a.evaluateFeature) ||
        !Resolve(s.module, "slFreeResources", a.freeResources) ||
        !Resolve(s.module, "slGetFeatureFunction", a.getFeatureFunction)) {
        Log("sl.interposer.dll is missing an expected export");
        FreeLibrary(s.module);
        s.module = nullptr;
        return false;
    }

    static const sl::Feature features[] = {
        sl::kFeatureDLSS, sl::kFeatureDLSS_RR };
    static const std::wstring pluginDir = dir;
    static const wchar_t* pluginPaths[] = { pluginDir.c_str() };
    sl::Preferences pref{};
    pref.showConsole = false;
    // SGE_SL_LOG=1 writes Streamline's own log next to the exe.
    char slLog[4] = {};
    size_t slLogLen = 0;
    const bool verboseLog =
        getenv_s(&slLogLen, slLog, sizeof(slLog), "SGE_SL_LOG") == 0 &&
        slLogLen > 0 && slLog[0] == '1';
    pref.logLevel = verboseLog ? sl::LogLevel::eVerbose : sl::LogLevel::eOff;
    static const std::wstring logDir = dir;
    if (verboseLog) pref.pathToLogsAndData = logDir.c_str();
    pref.pathsToPlugins = pluginPaths;
    pref.numPathsToPlugins = 1;
    pref.featuresToLoad = features;
    pref.numFeaturesToLoad = static_cast<uint32_t>(
        sizeof(features) / sizeof(features[0]));
    pref.engine = sl::EngineType::eCustom;
    pref.engineVersion = "SmallestGraphicsEngine";
    pref.projectId = "3f1c2b7e-5d0a-4c8e-9a61-7b2d4e8f0c15";
    pref.renderAPI = sl::RenderAPI::eD3D12;
    // Manual hooking keeps the engine's device native. CL state tracking is
    // left ON (eDisableCLStateTracking cleared) so the command list's heaps,
    // root signature and PSO are restored after slEvaluateFeature; the passes
    // after DLSS were written assuming nothing between them rebinds state.
    // slSetTagForFrame is rejected (eErrorInvalidIntegration) without
    // eUseFrameBasedResourceTagging.
    pref.flags = sl::PreferenceFlags::eUseManualHooking |
                 sl::PreferenceFlags::eUseFrameBasedResourceTagging |
                 sl::PreferenceFlags::eAllowOTA |
                 sl::PreferenceFlags::eLoadDownloadedPlugins;

    const sl::Result result = a.init(pref, sl::kSDKVersion);
    if (result != sl::Result::eOk) {
        Log("slInit failed, sl::Result " +
            std::to_string(static_cast<int>(result)));
        FreeLibrary(s.module);
        s.module = nullptr;
        return false;
    }
    s.initialized = true;
    Log("Streamline initialised");
    return true;
}

void SetDevice(ID3D12Device* device, IDXGIAdapter1* adapter) {
    State& s = S();
    if (!s.initialized || !device) return;
    s.device = device;
    sl::Result result = s.api.setD3DDevice(device);
    if (result != sl::Result::eOk) {
        Log("slSetD3DDevice failed, sl::Result " +
            std::to_string(static_cast<int>(result)));
        return;
    }

    DXGI_ADAPTER_DESC1 desc = {};
    sl::AdapterInfo info{};
    if (adapter && SUCCEEDED(adapter->GetDesc1(&desc))) {
        info.deviceLUID = reinterpret_cast<uint8_t*>(&desc.AdapterLuid);
        info.deviceLUIDSizeInBytes = sizeof(LUID);
    }
    result = s.api.isFeatureSupported(sl::kFeatureDLSS, info);
    if (result != sl::Result::eOk) {
        Log("DLSS not supported on this adapter, sl::Result " +
            std::to_string(static_cast<int>(result)));
        return;
    }
    bool loaded = false;
    if (s.api.isFeatureLoaded(sl::kFeatureDLSS, loaded) != sl::Result::eOk ||
        !loaded) {
        Log("DLSS plugin did not load");
        return;
    }
    void* setOptions = nullptr;
    if (s.api.getFeatureFunction(sl::kFeatureDLSS, "slDLSSSetOptions",
                                 setOptions) != sl::Result::eOk ||
        !setOptions) {
        Log("slDLSSSetOptions unavailable");
        return;
    }
    s.api.dlssSetOptions = reinterpret_cast<PFun_slDLSSSetOptions*>(setOptions);
    void* optimalSettings = nullptr;
    if (s.api.getFeatureFunction(sl::kFeatureDLSS,
            "slDLSSGetOptimalSettings", optimalSettings) == sl::Result::eOk)
        s.api.dlssGetOptimalSettings =
            reinterpret_cast<PFun_slDLSSGetOptimalSettings*>(optimalSettings);
    s.supported = true;
    Log("DLSS ready");
    result = s.api.isFeatureSupported(sl::kFeatureDLSS_RR, info);
    if (result != sl::Result::eOk) {
        s.rrStatus = "Ray Reconstruction unsupported (sl::Result " +
            std::to_string(static_cast<int>(result)) + ")";
        return;
    }
    loaded = false;
    if (s.api.isFeatureLoaded(sl::kFeatureDLSS_RR, loaded) !=
            sl::Result::eOk || !loaded) {
        s.rrStatus = "Ray Reconstruction plugin did not load";
        return;
    }
    void* rrSetOptions = nullptr;
    if (s.api.getFeatureFunction(sl::kFeatureDLSS_RR,
            "slDLSSDSetOptions", rrSetOptions) != sl::Result::eOk ||
        !rrSetOptions) {
        s.rrStatus = "slDLSSDSetOptions unavailable";
        return;
    }
    s.api.dlssdSetOptions =
        reinterpret_cast<PFun_slDLSSDSetOptions*>(rrSetOptions);
    void* rrOptimalSettings = nullptr;
    if (s.api.getFeatureFunction(sl::kFeatureDLSS_RR,
            "slDLSSDGetOptimalSettings", rrOptimalSettings) ==
            sl::Result::eOk)
        s.api.dlssdGetOptimalSettings =
            reinterpret_cast<PFun_slDLSSDGetOptimalSettings*>(rrOptimalSettings);
    s.rrSupported = s.api.dlssdGetOptimalSettings != nullptr;
    s.rrStatus = s.rrSupported ? "Ray Reconstruction ready" :
        "slDLSSDGetOptimalSettings unavailable";
}

IDXGIFactory2* SwapChainFactory(IDXGIFactory2* nativeFactory) {
    if (!nativeFactory) return nullptr;
    nativeFactory->AddRef();
    State& s = S();
    if (!s.initialized) return nativeFactory;
    // The proxy takes over this reference; on failure the pointer is left
    // as the native factory, which is still a valid reference to release.
    IDXGIFactory2* factory = nativeFactory;
    if (s.api.upgradeInterface(reinterpret_cast<void**>(&factory)) !=
        sl::Result::eOk) {
        Log("slUpgradeInterface(factory) failed");
        s.supported = false;
    }
    return factory;
}

bool Available() { return S().supported; }
bool RayReconstructionAvailable() { return S().rrSupported; }
const char* RayReconstructionStatus() { return S().rrStatus.c_str(); }
const char* Status() { return S().status.c_str(); }
Settings& GetSettings() { return S().settings; }

bool RenderSize(UINT displayWidth, UINT displayHeight, float percentage,
                UINT& renderWidth, UINT& renderHeight) {
    renderWidth = displayWidth;
    renderHeight = displayHeight;
    State& s = S();
    if (!s.supported || !s.api.dlssGetOptimalSettings ||
        !displayWidth || !displayHeight || percentage >= 99.5f)
        return false;
    sl::DLSSOptions options{};
    options.mode = ModeForPercentage(percentage);
    options.outputWidth = displayWidth;
    options.outputHeight = displayHeight;
    sl::DLSSOptimalSettings optimal{};
    if (s.api.dlssGetOptimalSettings(options, optimal) != sl::Result::eOk ||
        !optimal.renderWidthMin || !optimal.renderHeightMin ||
        !optimal.renderWidthMax || !optimal.renderHeightMax)
        return false;
    const float scale = (std::max)(0.33f,
        (std::min)(0.99f, percentage * 0.01f));
    const UINT requestedWidth = (std::max)(1u,
        static_cast<UINT>(displayWidth * scale + 0.5f));
    const UINT requestedHeight = (std::max)(1u,
        static_cast<UINT>(displayHeight * scale + 0.5f));
    renderWidth = (std::max)(optimal.renderWidthMin,
        (std::min)(optimal.renderWidthMax, requestedWidth));
    renderHeight = (std::max)(optimal.renderHeightMin,
        (std::min)(optimal.renderHeightMax, requestedHeight));
    renderWidth = (std::min)(renderWidth, displayWidth);
    renderHeight = (std::min)(renderHeight, displayHeight);
    return renderWidth < displayWidth && renderHeight < displayHeight;
}

bool Evaluate(const DLSSFrameInputs& in) {
    State& s = S();
    if (!s.supported || !s.settings.enabled || !in.cmdList || !in.color ||
        !in.depth || !in.motion || in.width == 0 || in.height == 0)
        return false;
    const bool rr = in.rayReconstruction;
    if (rr && (!s.rrSupported || !in.normalRoughness ||
               !in.diffuseAlbedo || !in.specularAlbedo ||
               !in.specularHitDistance || in.output ||
               in.width != in.outputWidth ||
               in.height != in.outputHeight)) {
        s.rrStatus = "Ray Reconstruction missing support or native guides";
        return false;
    }
    const bool superResolution = in.output != nullptr;
    if (superResolution && (!in.outputWidth || !in.outputHeight ||
        in.output->GetDesc().Width != in.outputWidth ||
        in.output->GetDesc().Height != in.outputHeight))
        return false;
    if (!superResolution && !EnsureOutput(in.color)) return false;

    const sl::DLSSMode mode = superResolution
        ? ModeForPercentage(in.screenPercentage) : sl::DLSSMode::eDLAA;
    const UINT outputWidth = superResolution ? in.outputWidth : in.width;
    const UINT outputHeight = superResolution ? in.outputHeight : in.height;

    const sl::ViewportHandle viewport(0u);

    if (!rr && (!s.optionsSet || s.appliedPreset != s.settings.preset ||
        s.appliedWidth != in.width || s.appliedHeight != in.height ||
        s.appliedOutputWidth != outputWidth ||
        s.appliedOutputHeight != outputHeight || s.appliedMode != mode)) {
        sl::DLSSOptions options{};
        options.mode = mode;
        options.outputWidth = outputWidth;
        options.outputHeight = outputHeight;
        options.colorBuffersHDR = sl::Boolean::eTrue;
        // Exposure is computed after this pass, from the image DLSS produces.
        options.useAutoExposure = sl::Boolean::eTrue;
        const sl::DLSSPreset preset = ToSL(s.settings.preset);
        options.dlaaPreset = preset;
        options.qualityPreset = preset;
        options.balancedPreset = preset;
        options.performancePreset = preset;
        options.ultraPerformancePreset = preset;
        options.ultraQualityPreset = preset;
        const sl::Result result = s.api.dlssSetOptions(viewport, options);
        if (result != sl::Result::eOk) {
            Log("slDLSSSetOptions failed, sl::Result " +
                std::to_string(static_cast<int>(result)));
            return false;
        }
        s.optionsSet = true;
        s.appliedPreset = s.settings.preset;
        s.appliedWidth = in.width;
        s.appliedHeight = in.height;
        s.appliedOutputWidth = outputWidth;
        s.appliedOutputHeight = outputHeight;
        s.appliedMode = mode;
        s.havePrevious = false;
    }

    if (rr) {
        sl::DLSSDOptions options{};
        options.mode = sl::DLSSMode::eDLAA;
        options.outputWidth = outputWidth;
        options.outputHeight = outputHeight;
        options.colorBuffersHDR = sl::Boolean::eTrue;
        options.normalRoughnessMode =
            sl::DLSSDNormalRoughnessMode::ePacked;
        options.worldToCameraView = ToSL(in.view);
        const XMMATRIX viewMatrix = XMLoadFloat4x4(&in.view);
        options.cameraViewToWorld = ToSL(
            XMMatrixInverse(nullptr, viewMatrix));
        const sl::Result optionResult =
            s.api.dlssdSetOptions(viewport, options);
        if (optionResult != sl::Result::eOk) {
            s.rrStatus = "slDLSSDSetOptions failed, sl::Result " +
                std::to_string(static_cast<int>(optionResult));
            Log(s.rrStatus);
            return false;
        }
    }

    sl::FrameToken* frame = nullptr;
    const uint32_t frameIndex = s.frameIndex++;
    if (s.api.getNewFrameToken(frame, &frameIndex) != sl::Result::eOk ||
        !frame)
        return false;

    const XMMATRIX view = XMLoadFloat4x4(&in.view);
    const XMMATRIX projection = XMLoadFloat4x4(&in.projection);
    const XMMATRIX viewProjection = view * projection;
    const XMMATRIX previousViewProjection = s.havePrevious
        ? XMLoadFloat4x4(&s.previousViewProjection) : viewProjection;
    const XMMATRIX clipToPrevClip =
        XMMatrixInverse(nullptr, viewProjection) * previousViewProjection;
    const XMMATRIX cameraToWorld = XMMatrixInverse(nullptr, view);
    XMFLOAT4X4 camera;
    XMStoreFloat4x4(&camera, cameraToWorld);

    sl::Constants consts{};
    consts.cameraViewToClip = ToSL(projection);
    consts.clipToCameraView = ToSL(XMMatrixInverse(nullptr, projection));
    consts.clipToLensClip = ToSL(XMMatrixIdentity());
    consts.clipToPrevClip = ToSL(clipToPrevClip);
    consts.prevClipToClip = ToSL(XMMatrixInverse(nullptr, clipToPrevClip));
    const float jitterSign = s.settings.invertJitter ? -1.0f : 1.0f;
    consts.jitterOffset = sl::float2(jitterSign * in.jitterPixels.x,
                                     jitterSign * in.jitterPixels.y);
    // Engine motion is current minus previous UV; DLSS wants the vector that
    // leads from the current pixel to its previous position, normalised.
    consts.mvecScale = sl::float2(-1.0f, -1.0f);
    consts.cameraPinholeOffset = sl::float2(0.0f, 0.0f);
    // Row-vector camera-to-world: rows are right, up, forward, position.
    consts.cameraRight = sl::float3(camera._11, camera._12, camera._13);
    consts.cameraUp = sl::float3(camera._21, camera._22, camera._23);
    consts.cameraFwd = sl::float3(camera._31, camera._32, camera._33);
    consts.cameraPos = sl::float3(camera._41, camera._42, camera._43);
    consts.cameraNear = in.nearPlane;
    consts.cameraFar = in.farPlane;
    consts.cameraFOV = in.verticalFovRadians;
    consts.cameraAspectRatio =
        static_cast<float>(in.width) / static_cast<float>(in.height);
    consts.depthInverted = sl::Boolean::eFalse;
    consts.cameraMotionIncluded = sl::Boolean::eTrue;
    consts.motionVectors3D = sl::Boolean::eFalse;
    consts.reset = (in.reset || !s.havePrevious ||
                    rr != s.lastEvaluatedRR) ? sl::Boolean::eTrue
                                             : sl::Boolean::eFalse;
    consts.orthographicProjection = sl::Boolean::eFalse;
    consts.motionVectorsDilated = sl::Boolean::eFalse;
    // The resolve writes pixel-centre UV minus the unjittered previous
    // projection, so this frame's jitter is inside the vector.
    consts.motionVectorsJittered = sl::Boolean::eTrue;

    if (s.api.setConstants(consts, *frame, viewport) != sl::Result::eOk) {
        Log("slSetConstants failed");
        return false;
    }

    const uint32_t readState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    sl::Resource depth(sl::ResourceType::eTex2d, in.depth, readState);
    sl::Resource motion(sl::ResourceType::eTex2d, in.motion, readState);
    sl::Resource color(sl::ResourceType::eTex2d, in.color, readState);
    if (superResolution && s.externalOutputReadable) {
        Transition(in.cmdList, in.output,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        s.externalOutputReadable = false;
    }
    sl::Resource output(sl::ResourceType::eTex2d,
                        superResolution ? in.output : s.output.Get(),
                        D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const sl::Extent inputExtent{ 0u, 0u, in.width, in.height };
    const sl::Extent outputExtent{ 0u, 0u, outputWidth, outputHeight };
    const sl::ResourceTag tags[] = {
        sl::ResourceTag(&depth, sl::kBufferTypeDepth,
                        sl::ResourceLifecycle::eValidUntilEvaluate,
                        &inputExtent),
        sl::ResourceTag(&motion, sl::kBufferTypeMotionVectors,
                        sl::ResourceLifecycle::eValidUntilEvaluate,
                        &inputExtent),
        sl::ResourceTag(&color, sl::kBufferTypeScalingInputColor,
                        sl::ResourceLifecycle::eValidUntilEvaluate,
                        &inputExtent),
        sl::ResourceTag(&output, sl::kBufferTypeScalingOutputColor,
                        sl::ResourceLifecycle::eValidUntilEvaluate,
                        &outputExtent),
    };
    sl::Resource normals(sl::ResourceType::eTex2d,
                         in.normalRoughness, readState);
    sl::Resource albedo(sl::ResourceType::eTex2d,
                        in.diffuseAlbedo, readState);
    sl::Resource specularAlbedo(sl::ResourceType::eTex2d,
                                in.specularAlbedo, readState);
    sl::Resource hitDistance(sl::ResourceType::eTex2d,
                             in.specularHitDistance, readState);
    const sl::ResourceTag rrTags[] = {
        tags[0], tags[1], tags[2], tags[3],
        sl::ResourceTag(&normals, sl::kBufferTypeNormalRoughness,
                        sl::ResourceLifecycle::eValidUntilEvaluate,
                        &inputExtent),
        sl::ResourceTag(&albedo, sl::kBufferTypeAlbedo,
                        sl::ResourceLifecycle::eValidUntilEvaluate,
                        &inputExtent),
        sl::ResourceTag(&specularAlbedo, sl::kBufferTypeSpecularAlbedo,
                        sl::ResourceLifecycle::eValidUntilEvaluate,
                        &inputExtent),
        sl::ResourceTag(&hitDistance, sl::kBufferTypeSpecularHitDistance,
                        sl::ResourceLifecycle::eValidUntilEvaluate,
                        &inputExtent),
    };
    const sl::Result tagResult =
        s.api.setTagForFrame(*frame, viewport,
                             rr ? rrTags : tags, rr ? 8 : 4, in.cmdList);
    if (tagResult != sl::Result::eOk) {
        Log("slSetTagForFrame failed, sl::Result " +
            std::to_string(static_cast<int>(tagResult)));
        return false;
    }

    const sl::BaseStructure* inputs[] = { &viewport };
    const sl::Result result = s.api.evaluateFeature(
        rr ? sl::kFeatureDLSS_RR : sl::kFeatureDLSS,
        *frame, inputs, 1, in.cmdList);
    if (result != sl::Result::eOk) {
        if (rr) s.rrStatus = "Ray Reconstruction evaluation failed, sl::Result " +
            std::to_string(static_cast<int>(result));
        Log("slEvaluateFeature failed, sl::Result " +
            std::to_string(static_cast<int>(result)));
        return false;
    }
    // Only a frame DLSS consumed becomes the next frame's previous: a failed
    // RR attempt followed by the DLAA fallback must not reproject onto itself.
    XMStoreFloat4x4(&s.previousViewProjection, viewProjection);
    s.havePrevious = true;
    s.lastEvaluatedRR = rr;

    if (superResolution) {
        Transition(in.cmdList, in.output,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        s.externalOutputReadable = true;
        if (s.status != "DLSS Super Resolution active")
            Log("DLSS Super Resolution active");
        return true;
    }
    Transition(in.cmdList, s.output.Get(),
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
               D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(in.cmdList, in.color,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
               D3D12_RESOURCE_STATE_COPY_DEST);
    in.cmdList->CopyResource(in.color, s.output.Get());
    Transition(in.cmdList, in.color, D3D12_RESOURCE_STATE_COPY_DEST,
               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(in.cmdList, s.output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (rr) {
        s.rrStatus = "Ray Reconstruction active";
        if (s.status != s.rrStatus) Log(s.rrStatus);
    } else if (s.status != "DLSS active") Log("DLSS active");
    return true;
}

void ReleaseResources() {
    State& s = S();
    if (s.supported && s.api.freeResources)
        s.api.freeResources(sl::kFeatureDLSS, sl::ViewportHandle(0u));
    if (s.rrSupported && s.api.freeResources)
        s.api.freeResources(sl::kFeatureDLSS_RR, sl::ViewportHandle(0u));
    s.output.Reset();
    s.optionsSet = false;
    s.havePrevious = false;
    s.externalOutputReadable = false;
}

void Shutdown() {
    State& s = S();
    if (!s.initialized) return;
    ReleaseResources();
    s.api.shutdown();
    s.initialized = false;
    s.supported = false;
    s.rrSupported = false;
    // sl.interposer.dll stays loaded: the swapchain is an SL proxy whose code
    // lives in it, and g_dx12 releases the swapchain after this runs.
    // Unloading here made that final Release an access violation.
}

} // namespace DLSS
