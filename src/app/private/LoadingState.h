#pragma once

#include "TravelDestinations.h"

// Private application implementation; included once by main.cpp in dependency order.

// ?? cold-boot shader compile ????????????????????????????????????????????????
//
// On a cold shader cache the visibility resolve permutations are still on the
// compile workers when boot reaches the menu. The menu comes up anyway, shows
// a progress bar and greys out every row that would enter the game; the
// visibility buffer is finished once the compiles land.
static bool  g_shadersCompiling = false;
static float g_shaderCompileProgress = 0.0f;
static bool  g_bindlessHeapReady = false;

static std::string g_loadingLevelName;
static std::string g_loadingLevelSubtitle;
static std::string g_loadingLevelImagePath;

static bool g_deferLoadingActions = false;
static std::function<void()> g_pendingLoadingAction;

static void SetLoadingLevelPresentation(const std::filesystem::path& levelPath) {
    // Starts through the command line and multiplayer use the same art
    // as boarding; selecting it only in the helicopter would miss those loads.
    const TravelDestination* destination = TravelDestinationForLevel(levelPath);
    if (!destination) return;
    g_loadingLevelName = destination->name;
    g_loadingLevelSubtitle = destination->subtitle;
    g_loadingLevelImagePath = TravelPreviewImagePath(*destination);
}

static void QueueLoadingAction(std::function<void()> action,
                               const std::string& name,
                               const std::filesystem::path& levelPath = {}) {
    if (g_pendingLoadingAction || g_game.loading.Active()) return;
    g_loadingLevelName = name;
    g_loadingLevelSubtitle.clear();
    g_loadingLevelImagePath.clear();
    if (!levelPath.empty()) SetLoadingLevelPresentation(levelPath);
    g_pendingLoadingAction = std::move(action);
}

static void RunPendingLoadingAction() {
    if (!g_pendingLoadingAction) return;
    // The click frame has been presented before any file parsing or scene setup.
    auto action = std::move(g_pendingLoadingAction);
    g_pendingLoadingAction = {};
    action();
}

// Builds what waited on the compiles. Main thread, between frames.
static void FinishDeferredShaderCompiles() {
    g_shadersCompiling = false;
    g_shaderCompileProgress = 1.0f;
    if (!visBuffer.FinishDeferredResolvePipeline())
        scene.useVisibilityBuffer = false;
    g_bindlessMaterialsReady = g_bindlessHeapReady && mainShader.BindlessReady() &&
        visBuffer.BindlessResolveReady() &&
        (!g_useMeshShader || g_meshShader.bindlessReady);
}

// Once per frame, before rendering: polls the workers and finishes when done.
static void PumpDeferredShaderCompiles() {
    if (!g_shadersCompiling) return;
    if (ShaderCacheDX12::BootCompilesPending(&g_shaderCompileProgress)) return;
    FinishDeferredShaderCompiles();
}

// A level start that never went through the greyed-out menu (--level, the
// auto-host and capture hooks) blocks here until the compiles land.
static void WaitForDeferredShaderCompiles() {
    if (!g_shadersCompiling) return;
    BootTimer::Log("Level start waiting for shader compiles");
    while (ShaderCacheDX12::BootCompilesPending(&g_shaderCompileProgress))
        Sleep(50);
    FinishDeferredShaderCompiles();
}

static void BeginLevelLoading(bool armoryOnly = false) {
    WaitForDeferredShaderCompiles();
    levelArmoryLoadOnly = armoryOnly;
    g_uploadHeapRelease.Reset();
    if (armoryOnly) {
        g_game.loading.Begin({ 4u, "Armory stock", "firearm models",
                              LevelLoadStage::Weapons });
    } else {
        g_game.loading.Begin({
            g_emptyLevelMode ? 6u : 12u,
            g_emptyLevelMode ? "Firearms and terrain material"
                             : "Firearms, terrain material and crate model",
            g_emptyLevelMode ? "firearm models + floor material"
                             : "firearm models + Content/Models/h2.glb"
        });
    }
    if (!BeginTextureUploadArenaDX12()) {
        g_game.loading.SetCurrent(
            "Unable to begin pooled texture staging",
            "texture upload arena is still owned by an earlier load");
        g_game.loading.Complete(false);
        SGE_LOG("LogRender", EngineLog::Level::Error,
            "Level load aborted: texture upload arena was not idle");
        return;
    }
    levelLoadingUploadBaseline = GetStaticBufferStatsDX12();
}

static void AdvanceLevelLoading(LevelLoadStage next, const char* label,
                                const char* asset, bool succeeded = true) {
    // Deliberately does not present a frame. Stages run inside the main loop
    // with the direct command list open and recording texture uploads, so
    // forcing a present here would reset that list and throw the uploads away.
    // The loop presents between stages by itself, which is what animates the
    // bar and the wordmark.
    g_game.loading.Advance(next, label, asset, succeeded);
}

static void CompleteLevelLoading(bool succeeded = true) {
    g_game.loading.Complete(succeeded);
}

static float lastX = SCR_WIDTH / 2.0f;
static float lastY = SCR_HEIGHT / 2.0f;
static bool  firstMouse   = true;
static bool  ignoreNextMouseMove = false;
static bool  showUI        = true;
static bool  cameraLocked  = true;
static bool IsEditorEditing() {
    return g_game.session.Screen() == GameScreen::LevelEditor &&
           !g_levelEditor.IsPlaying();
}
static bool IsEditorPlaying() {
    return g_game.session.Screen() == GameScreen::LevelEditor &&
           g_levelEditor.IsPlaying();
}
static bool IsSceneScreen() {
    return g_game.session.IsSceneScreen();
}
static bool IsGameplayScreen() {
    return g_game.session.Screen() == GameScreen::Level1 || IsEditorPlaying();
}
static bool  deathCursorReleased = false;
static bool  squadWipeCursorReleased = false;
static float squadWipeScreenAge = 0.0f;
void RequestLiveDXRDDGIRebuild() {
    g_game.commands.Request(GameCommand::RebuildDDGI);
}
static bool  isFullscreen  = false;
static RECT  windowedRect  = {};
static DWORD windowedStyle = 0;
static float deltaTime     = 0.0f;

static ComPtr<ID3D12DescriptorHeap> imguiSrvHeap;
static constexpr UINT kImGuiDescriptorCount = 512;
struct PrefabThumbnailRuntime {
    size_t sourceHash = 0;
    std::filesystem::path pngPath;
    std::future<std::shared_ptr<PrefabThumbnailMesh>> preparation;
    std::future<bool> cacheWrite;
    std::shared_ptr<PrefabThumbnailMesh> mesh;
    ComPtr<ID3D12Resource> texture;
    ComPtr<ID3D12Resource> vertexBuffer;
    ComPtr<ID3D12Resource> indexBuffer;
    ComPtr<ID3D12Resource> constantBuffer;
    ComPtr<ID3D12Resource> readback;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    std::vector<ComPtr<ID3D12Resource>> uploads;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT readbackFootprint{};
    UINT64 renderFence = 0;
    UINT descriptorSlot = ~0u;
    bool rendered = false;
    // Camera from the side of the model's long axis instead of the elevated
    // three-quarter view: weapons read by silhouette.
    bool sideView = false;
    bool readbackProcessed = false;
};
static std::unordered_map<std::string, PrefabThumbnailRuntime> g_prefabThumbnails;
static UINT g_nextImGuiTextureSlot = 1;
static bool g_thumbnailUploadedThisFrame = false;
static bool g_prefabEditorSmokeEnabled = false;
static bool g_prefabEditorSmokeFinished = false;
