// Streamline / DLSS implementation. See DLSSDX12.h for the integration shape.
//
// Kept in its own translation unit because sl_security.h defines non-inline
// functions, and because only this file needs the Streamline headers.

#include "DLSSDX12.h"

#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss.h>
#include <sl_dlss_d.h>
#include <sl_dlss_g.h>
#include <sl_reflex.h>
#include <sl_pcl.h>
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
    PFun_slGetNativeInterface* getNativeInterface = nullptr;
    PFun_slGetNewFrameToken* getNewFrameToken = nullptr;
    PFun_slSetConstants* setConstants = nullptr;
    PFun_slSetTagForFrame* setTagForFrame = nullptr;
    PFun_slEvaluateFeature* evaluateFeature = nullptr;
    PFun_slFreeResources* freeResources = nullptr;
    PFun_slGetFeatureFunction* getFeatureFunction = nullptr;
    PFun_slSetFeatureLoaded* setFeatureLoaded = nullptr;
    PFun_slReflexSetOptions* reflexSetOptions = nullptr;
    PFun_slReflexGetState* reflexGetState = nullptr;
    PFun_slReflexSleep* reflexSleep = nullptr;
    PFun_slPCLSetMarker* pclSetMarker = nullptr;
    PFun_slPCLSetOptions* pclSetOptions = nullptr;
    PFun_slPCLGetState* pclGetState = nullptr;
    PFun_slDLSSGSetOptions* fgSetOptions = nullptr;
    PFun_slDLSSGGetState* fgGetState = nullptr;
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
    ComPtr<ID3D12Device> queueDeviceProxy;
    std::string status = "Not started";
    Settings settings;
    bool reflexSupported = false;
    bool reflexLowLatency = false;
    bool pclSupported = false;
    bool fgSupported = false;
    bool fgLoaded = false;
    bool fgEnabled = false;
    bool fgInputs = false;
    bool fgFault = false;
    uint32_t fgPresentedFrames = 0;  // presents since the window started
    uint32_t fgAppFrames = 0;        // rendered frames in that window
    UINT pclMessage = 0;
    std::string reflexStatus = "Not started";
    std::string fgStatus = "Not started";
    ReflexMode appliedReflex = ReflexMode::Off;
    bool reflexOptionsSet = false;
    sl::FrameToken* frame = nullptr;
    uint32_t currentFrameIndex = 0;
    bool constantsSet = false;

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

void FeatureLog(const std::string& message) {
    FILE* file = nullptr;
    if (fopen_s(&file, "nvidia_features.log", "a") == 0 && file) {
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

template <typename T>
bool FeatureFunction(sl::Feature feature, const char* name, T*& out) {
    void* address = nullptr;
    if (S().api.getFeatureFunction(feature, name, address) != sl::Result::eOk)
        return false;
    out = reinterpret_cast<T*>(address);
    return out != nullptr;
}

bool FeatureSupported(sl::Feature feature, const sl::AdapterInfo& adapter,
                      std::string& status) {
    const sl::Result result = S().api.isFeatureSupported(feature, adapter);
    if (result == sl::Result::eOk) return true;
    switch (result) {
    case sl::Result::eErrorOSOutOfDate: status = "Windows update required"; break;
    case sl::Result::eErrorDriverOutOfDate: status = "NVIDIA driver update required"; break;
    case sl::Result::eErrorOSDisabledHWS: status = "Enable hardware-accelerated GPU scheduling in Windows"; break;
    default: status = "Unsupported adapter or runtime (sl::Result " +
        std::to_string(static_cast<int>(result)) + ")"; break;
    }
    return false;
}

void ApplyReflexOptions() {
    State& s = S();
    if (!s.reflexSupported) return;
    ReflexMode mode = s.reflexLowLatency ? s.settings.reflex : ReflexMode::Off;
    if (s.fgLoaded && mode == ReflexMode::Off) mode = ReflexMode::On;
    if (s.reflexOptionsSet && s.appliedReflex == mode) return;
    sl::ReflexOptions options{};
    options.mode = static_cast<sl::ReflexMode>(mode);
    const auto result = s.api.reflexSetOptions(options);
    s.reflexOptionsSet = result == sl::Result::eOk;
    if (s.reflexOptionsSet) {
        s.appliedReflex = mode;
        s.reflexStatus = !s.reflexLowLatency ? "Low latency unavailable on this adapter" :
            mode == ReflexMode::Off ? "Off" :
            mode == ReflexMode::OnWithBoost ? "On + Boost" :
            s.settings.reflex == ReflexMode::Off ? "On (required by Frame Generation)" : "On";
    } else s.reflexStatus = "Reflex options failed (sl::Result " +
        std::to_string(static_cast<int>(result)) + ")";
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
        !Resolve(s.module, "slGetNativeInterface", a.getNativeInterface) ||
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
    Resolve(s.module, "slSetFeatureLoaded", a.setFeatureLoaded);

    static const sl::Feature features[] = {
        sl::kFeatureDLSS, sl::kFeatureDLSS_RR, sl::kFeatureReflex,
        sl::kFeaturePCL, sl::kFeatureDLSS_G };
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
    // Loading DLSS-G changes swap-chain creation even with interpolation off.
    // Unload before creating the default swap chain to preserve its cost.
    if (a.setFeatureLoaded)
        a.setFeatureLoaded(sl::kFeatureDLSS_G, false);
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
    s.reflexSupported = FeatureSupported(sl::kFeatureReflex, info, s.reflexStatus) &&
        FeatureFunction(sl::kFeatureReflex, "slReflexSetOptions", s.api.reflexSetOptions) &&
        FeatureFunction(sl::kFeatureReflex, "slReflexGetState", s.api.reflexGetState) &&
        FeatureFunction(sl::kFeatureReflex, "slReflexSleep", s.api.reflexSleep);
    if (s.reflexSupported) {
        sl::ReflexState state{};
        s.reflexLowLatency = s.api.reflexGetState(state) == sl::Result::eOk &&
            state.lowLatencyAvailable;
        ApplyReflexOptions();
    }
    std::string pclStatus;
    s.pclSupported = FeatureSupported(sl::kFeaturePCL, info, pclStatus) &&
        FeatureFunction(sl::kFeaturePCL, "slPCLSetMarker", s.api.pclSetMarker) &&
        FeatureFunction(sl::kFeaturePCL, "slPCLSetOptions", s.api.pclSetOptions) &&
        FeatureFunction(sl::kFeaturePCL, "slPCLGetState", s.api.pclGetState);
    if (s.pclSupported) {
        sl::PCLOptions options{};
        options.idThread = GetCurrentThreadId();
        sl::PCLState state{};
        s.pclSupported = s.api.pclSetOptions(options) == sl::Result::eOk &&
            s.api.pclGetState(state) == sl::Result::eOk;
        if (s.pclSupported) s.pclMessage = state.statsWindowMessage;
    }
    s.fgSupported = s.api.setFeatureLoaded &&
        FeatureSupported(sl::kFeatureDLSS_G, info, s.fgStatus);
    if (s.fgSupported && (!s.reflexLowLatency || !s.pclSupported)) {
        s.fgSupported = false;
        s.fgStatus = "Frame Generation requires Reflex and PCL";
    } else if (s.fgSupported) s.fgStatus = "Off (2x Frame Generation available)";
    FeatureLog("Reflex: " + s.reflexStatus);
    FeatureLog("Frame Generation: " + s.fgStatus);
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
        s.fgSupported = false;
        s.fgStatus = "Swap-chain proxy unavailable";
    }
    return factory;
}

HRESULT CreateCommandQueue(ID3D12Device* device, const D3D12_COMMAND_QUEUE_DESC* desc,
                           REFIID iid, void** queue) {
    State& s = S();
    if (!s.initialized) return device->CreateCommandQueue(desc, iid, queue);
    if (!s.queueDeviceProxy) {
        device->AddRef();
        ID3D12Device* upgraded = device;
        s.api.upgradeInterface(reinterpret_cast<void**>(&upgraded));
        s.queueDeviceProxy.Attach(upgraded);
    }
    void* created = nullptr;
    const HRESULT hr = s.queueDeviceProxy->CreateCommandQueue(desc, iid, &created);
    if (FAILED(hr)) return hr;
    // SDK queue proxies borrow their parent device proxy without AddRef.
    // Keep that parent alive, and return native queues to the engine so every
    // operation except the mandatory creation hook retains native semantics.
    const auto result = s.api.getNativeInterface(created, queue);
    static_cast<IUnknown*>(created)->Release();
    return result == sl::Result::eOk ? S_OK : E_FAIL;
}

bool Available() { return S().supported; }
bool RayReconstructionAvailable() { return S().rrSupported; }
const char* RayReconstructionStatus() { return S().rrStatus.c_str(); }
const char* Status() { return S().status.c_str(); }
bool LastEvaluatedRR() { return S().lastEvaluatedRR; }
static EvalDebug g_evalDebug;
const EvalDebug& LastEvalDebug() { return g_evalDebug; }
Settings& GetSettings() { return S().settings; }

bool ReflexAvailable() { return S().reflexLowLatency; }
const char* ReflexStatus() { return S().initialized ? S().reflexStatus.c_str() : Status(); }
bool FrameGenerationAvailable() { return S().fgSupported; }
const char* FrameGenerationStatus() { return S().initialized ? S().fgStatus.c_str() : Status(); }
bool FrameGenerationLoaded() { return S().fgLoaded; }
bool FrameGenerationInputsReady() { return S().fgInputs && !S().fgFault; }

void MarkLatency(LatencyMarker marker) {
    State& s = S();
    if (!s.pclSupported || !s.frame) return;
    static const sl::PCLMarker markers[] = {
        sl::PCLMarker::eSimulationStart, sl::PCLMarker::eSimulationEnd,
        sl::PCLMarker::eRenderSubmitStart, sl::PCLMarker::eRenderSubmitEnd,
        sl::PCLMarker::ePresentStart, sl::PCLMarker::ePresentEnd,
        sl::PCLMarker::eTriggerFlash, sl::PCLMarker::ePCLatencyPing };
    const auto result = s.api.pclSetMarker(markers[static_cast<int>(marker)], *s.frame);
    if (result != sl::Result::eOk) {
        s.reflexStatus = "Latency marker failed (sl::Result " +
            std::to_string(static_cast<int>(result)) + ")";
        s.fgFault = true;
    }
}

void HandleLatencyMessage(UINT message) {
    if (message == WM_LBUTTONDOWN) MarkLatency(LatencyMarker::TriggerFlash);
    if (S().pclMessage && message == S().pclMessage) MarkLatency(LatencyMarker::Ping);
}

void BeginAppFrame() {
    State& s = S();
    s.frame = nullptr;
    s.constantsSet = false;
    s.fgInputs = false;
    if (!s.initialized) return;
    s.currentFrameIndex = s.frameIndex++;
    if (s.api.getNewFrameToken(s.frame, &s.currentFrameIndex) != sl::Result::eOk)
        s.frame = nullptr;
    ApplyReflexOptions();
    if (s.reflexSupported && s.frame && s.reflexOptionsSet) {
        const auto result = s.api.reflexSleep(*s.frame);
        if (result != sl::Result::eOk)
            s.reflexStatus = "Reflex sleep failed (sl::Result " +
                std::to_string(static_cast<int>(result)) + ")";
    }
    MarkLatency(LatencyMarker::SimulationStart);
}

void SuspendFrameGeneration() {
    State& s = S();
    if (!s.fgLoaded || !s.api.fgSetOptions) return;
    // DLSS-G warns on a second slDLSSGSetOptions in one frame. A freshly
    // loaded plugin starts in eOff, so only an enabled mode needs turning off.
    if (s.fgEnabled) {
        sl::DLSSGOptions options{};
        s.api.fgSetOptions(sl::ViewportHandle(0u), options);
        if (!s.fgFault) s.fgStatus = "Suspended; waiting for valid gameplay inputs";
    }
    s.fgEnabled = false;
    s.fgInputs = false;
    if (s.frame) {
        const sl::ResourceTag tags[] = {
            { nullptr, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent },
            { nullptr, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent },
            { nullptr, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent },
            { nullptr, sl::kBufferTypeUIColorAndAlpha, sl::ResourceLifecycle::eValidUntilPresent } };
        s.api.setTagForFrame(*s.frame, sl::ViewportHandle(0u), tags, 4, nullptr);
    }
}

bool LoadFrameGeneration(bool load) {
    State& s = S();
    if (load == s.fgLoaded) return true;
    if (!s.initialized || !s.api.setFeatureLoaded || (load && !s.fgSupported)) return false;
    SuspendFrameGeneration();
    const auto result = s.api.setFeatureLoaded(sl::kFeatureDLSS_G, load);
    if (result != sl::Result::eOk) {
        s.fgStatus = "Frame Generation plugin transition failed (sl::Result " +
            std::to_string(static_cast<int>(result)) + ")";
        return false;
    }
    s.fgLoaded = load;
    s.fgFault = false;
    s.api.fgSetOptions = nullptr;
    s.api.fgGetState = nullptr;
    if (load && (!FeatureFunction(sl::kFeatureDLSS_G, "slDLSSGSetOptions", s.api.fgSetOptions) ||
                 !FeatureFunction(sl::kFeatureDLSS_G, "slDLSSGGetState", s.api.fgGetState))) {
        s.api.setFeatureLoaded(sl::kFeatureDLSS_G, false);
        s.fgLoaded = false;
        s.fgStatus = "Frame Generation plugin functions unavailable";
        return false;
    }
    s.fgStatus = load ? "Waiting for gameplay with DLSS SR/DLAA or upscaling RR" : "Off";
    FeatureLog("Frame Generation: " + s.fgStatus);
    ApplyReflexOptions();
    return true;
}

void TagFrameGenerationInputs(const DLSSFrameInputs& in) {
    State& s = S();
    if (!s.fgLoaded || !s.settings.frameGeneration || s.fgFault ||
        !s.frame || !s.constantsSet || !in.cmdList || !in.depth || !in.motion) return;
    sl::Resource depth(sl::ResourceType::eTex2d, in.depth, in.depthState);
    sl::Resource motion(sl::ResourceType::eTex2d, in.motion,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    const sl::Extent extent{ 0, 0, in.width, in.height };
    // Depth is reused by forward draws; async compute can overwrite motion
    // next frame. Copy both on the presenting queue instead of sharing them
    // with the plugin's queue or introducing another cross-queue fence.
    const sl::ResourceTag tags[] = {
        { &depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eOnlyValidNow, &extent },
        { &motion, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eOnlyValidNow, &extent } };
    s.fgInputs = s.api.setTagForFrame(*s.frame, sl::ViewportHandle(0u), tags, 2,
                                    in.cmdList) == sl::Result::eOk;
    if (!s.fgInputs) s.fgStatus = "Frame Generation depth/motion tagging failed";
}

bool TagFrameGenerationColor(ID3D12GraphicsCommandList* list,
                            ID3D12Resource* hudless, ID3D12Resource* ui,
                            UINT width, UINT height) {
    State& s = S();
    if (!s.fgLoaded || !s.fgInputs || !s.settings.frameGeneration ||
        !s.settings.enabled || s.fgFault || !hudless || !ui) {
        SuspendFrameGeneration();
        return false;
    }
    sl::DLSSGState state{};
    if (s.api.fgGetState(sl::ViewportHandle(0u), state, nullptr) != sl::Result::eOk ||
        width < state.minWidthOrHeight || height < state.minWidthOrHeight ||
        !s.reflexOptionsSet || s.appliedReflex == ReflexMode::Off) {
        SuspendFrameGeneration();
        s.fgStatus = "Frame Generation unavailable at this resolution or Reflex state";
        return false;
    }
    const UINT readState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    sl::Resource color(sl::ResourceType::eTex2d, hudless, readState);
    sl::Resource overlay(sl::ResourceType::eTex2d, ui, readState);
    const sl::Extent extent{ 0, 0, width, height };
    const sl::ResourceTag tags[] = {
        { &color, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent, &extent },
        { &overlay, sl::kBufferTypeUIColorAndAlpha, sl::ResourceLifecycle::eValidUntilPresent, &extent } };
    if (s.api.setTagForFrame(*s.frame, sl::ViewportHandle(0u), tags, 2, list) != sl::Result::eOk) {
        SuspendFrameGeneration();
        s.fgStatus = "Frame Generation color tagging failed";
        return false;
    }
    sl::DLSSGOptions options{};
    options.mode = sl::DLSSGMode::eOn;
    options.numFramesToGenerate = 1;
    if (s.api.fgSetOptions(sl::ViewportHandle(0u), options) != sl::Result::eOk) {
        SuspendFrameGeneration();
        s.fgStatus = "Frame Generation options failed";
        return false;
    }
    // numFramesActuallyPresented counts since the previous GetState, so the
    // call above consumed part of the last frame's presents. FinishAppFrame
    // judges 2x from both calls; presents before enabling were not generated.
    if (!s.fgEnabled) {
        s.fgPresentedFrames = 0;
        s.fgAppFrames = 0;
        s.fgStatus = "2x requested; awaiting presentation";
    } else s.fgPresentedFrames += state.numFramesActuallyPresented;
    s.fgEnabled = true;
    return true;
}

void FinishAppFrame() {
    State& s = S();
    if (s.fgLoaded && s.fgEnabled) {
        sl::DLSSGState state{};
        const auto result = s.api.fgGetState(sl::ViewportHandle(0u), state, nullptr);
        if (result != sl::Result::eOk || state.status != sl::DLSSGStatus::eOk) {
            SuspendFrameGeneration();
            s.fgFault = true;
            s.fgStatus = "Frame Generation disabled after runtime error (result " +
                std::to_string(static_cast<int>(result)) + ", status " +
                std::to_string(static_cast<uint32_t>(state.status)) + "); toggle off/on to retry";
            FeatureLog(s.fgStatus);
            return;
        }
        // SL presents on its own pacing thread, so a single frame's split
        // between the two GetState calls drifts; judge over a short window.
        s.fgPresentedFrames += state.numFramesActuallyPresented;
        if (++s.fgAppFrames >= 16) {
            s.fgStatus = s.fgPresentedFrames * 2 >= s.fgAppFrames * 3
                ? "2x Frame Generation active" : "On; waiting for interpolated frames";
            s.fgPresentedFrames = 0;
            s.fgAppFrames = 0;
        }
    }
}

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
               !in.specularHitDistance ||
               (!in.output && (in.width != in.outputWidth ||
                               in.height != in.outputHeight)))) {
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
        options.mode = mode;
        options.outputWidth = outputWidth;
        options.outputHeight = outputHeight;
        options.colorBuffersHDR = sl::Boolean::eTrue;
        options.normalRoughnessMode =
            sl::DLSSDNormalRoughnessMode::ePacked;
        // Capture A/B only: SGE_RR_PRESET=<sl::DLSSDPreset value>. Every
        // mode's slot: upscaling RR runs as Quality/Balanced, not DLAA.
        char rrPresetText[8] = {};
        if (GetEnvironmentVariableA("SGE_RR_PRESET", rrPresetText,
                                    sizeof(rrPresetText)) > 0) {
            const auto preset =
                static_cast<sl::DLSSDPreset>(atoi(rrPresetText));
            options.dlaaPreset = preset;
            options.qualityPreset = preset;
            options.balancedPreset = preset;
            options.performancePreset = preset;
            options.ultraPerformancePreset = preset;
            options.ultraQualityPreset = preset;
        }
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

    sl::FrameToken* frame = s.frame;
    const uint32_t frameIndex = s.currentFrameIndex;
    if (!frame) return false;

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
    // projection, so this frame's jitter is inside the vector -- except under
    // Ray Reconstruction, where the resolve subtracts it: RR left static-camera
    // frames moving at every edge with jittered vectors (SR/DLAA did not).
    // Upscaling RR is handed unjittered vectors too (in.motionUnjittered).
    consts.motionVectorsJittered =
        ((rr && !superResolution) || in.motionUnjittered)
        ? sl::Boolean::eFalse : sl::Boolean::eTrue;

    g_evalDebug.valid = true;
    g_evalDebug.rayReconstruction = rr;
    g_evalDebug.superResolution = superResolution;
    g_evalDebug.motionVectorsJittered =
        consts.motionVectorsJittered == sl::Boolean::eTrue;
    g_evalDebug.reset = consts.reset == sl::Boolean::eTrue;
    g_evalDebug.jitterX = consts.jitterOffset.x;
    g_evalDebug.jitterY = consts.jitterOffset.y;
    g_evalDebug.mvecScaleX = consts.mvecScale.x;
    g_evalDebug.mvecScaleY = consts.mvecScale.y;
    g_evalDebug.inputWidth = in.width;
    g_evalDebug.inputHeight = in.height;
    g_evalDebug.outputWidth = outputWidth;
    g_evalDebug.outputHeight = outputHeight;
    g_evalDebug.mode = static_cast<int>(mode);
    g_evalDebug.frameIndex = frameIndex;
    {
        ID3D12Resource* tagged[EvalDebug::kResourceCount] = {
            in.color, in.depth, in.motion,
            rr ? in.normalRoughness : nullptr, rr ? in.diffuseAlbedo : nullptr,
            rr ? in.specularAlbedo : nullptr,
            rr ? in.specularHitDistance : nullptr,
            superResolution ? in.output : s.output.Get() };
        for (int i = 0; i < EvalDebug::kResourceCount; ++i) {
            const D3D12_RESOURCE_DESC desc = tagged[i]
                ? tagged[i]->GetDesc() : D3D12_RESOURCE_DESC{};
            g_evalDebug.resourceWidth[i] = static_cast<UINT>(desc.Width);
            g_evalDebug.resourceHeight[i] = desc.Height;
        }
    }
    if (!s.constantsSet && s.api.setConstants(consts, *frame, viewport) != sl::Result::eOk) {
        Log("slSetConstants failed");
        return false;
    }
    s.constantsSet = true;

    const uint32_t readState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    sl::Resource depth(sl::ResourceType::eTex2d, in.depth, in.depthState);
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
    SuspendFrameGeneration();
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
    s.queueDeviceProxy.Reset();
    s.initialized = false;
    s.supported = false;
    s.rrSupported = false;
    s.reflexSupported = s.reflexLowLatency = s.pclSupported = false;
    s.fgSupported = s.fgLoaded = s.fgEnabled = false;
    s.frame = nullptr;
    // sl.interposer.dll stays loaded: the swapchain is an SL proxy whose code
    // lives in it, and g_dx12 releases the swapchain after this runs.
    // Unloading here made that final Release an access violation.
}

} // namespace DLSS
