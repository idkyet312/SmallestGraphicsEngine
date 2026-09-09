#pragma once

// Private application implementation; included once by main.cpp in dependency order.

static void BeginLevelLoading() {
    g_uploadHeapRelease.Reset();
    g_game.loading.Begin({
        g_emptyLevelMode ? 6u : 12u,
        g_emptyLevelMode ? "Terrain material"
                         : "Terrain material and crate model",
        g_emptyLevelMode ? "floor material" : "Content/Models/h2.glb"
    });
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
    bool readbackProcessed = false;
};
static std::unordered_map<std::string, PrefabThumbnailRuntime> g_prefabThumbnails;
static UINT g_nextImGuiTextureSlot = 1;
static bool g_thumbnailUploadedThisFrame = false;
static bool g_prefabEditorSmokeEnabled = false;
static bool g_prefabEditorSmokeFinished = false;
