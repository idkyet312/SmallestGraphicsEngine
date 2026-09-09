#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <DbgHelp.h>
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "comdlg32.lib")
#include <iostream>
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <filesystem>
#include <functional>
#include <future>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx12.h>
#include <ImGuizmo.h>
#include <stb_image_write.h>

#include "DX12Core.h"
#include "ProfilerDX12.h"
#include "EngineLogger.h"
#include "GroundLevel.h"
#include "ShaderDX12.h"
#include "DDGI_DX12.h"
#include "VisibilityBufferDX12.h"
#include "StaticBufferDX12.h"
#include "TextureUploadArenaDX12.h"
#include "Scene.h"
#include "TimeOfDay.h"
#include "ForwardRenderer.h"
#include "ForwardQualityDX12.h"
#include "VBDrawRenderer.h"
#include "RaytracingDX12.h"
#include "DXRDDGIRenderer.h"
#include "VirtualInput.h"
#include "EngineUI.h"
#include "GLBImporter.h"
#include "CookedAssetLoader.h"
#include "MipGenerator.h"
#include "ShadowMapDX12.h"
#include "MeshShaderDX12.h"
#include "TerrainRendererDX12.h"
#include "SkyRendererDX12.h"
#include "EnvironmentIBLDX12.h"
#include "OcclusionDepthDX12.h"
#include "FXAADX12.h"
#include "NightVisionDX12.h"
#include "VolumetricFogDX12.h"
#include "LightShaftsDX12.h"
#include "ScreenSpaceAODX12.h"
#include "ScreenSpaceReflectionsDX12.h"
#include "MSAADX12.h"
#include "GrassMSAADX12.h"
#include "SniperScopeDX12.h"
#include "CollisionMesh.h"
#include "SphereMeshCut.h"
#include "CollisionMeshCache.h"
#include "DestructionDX12.h"
#include "FBXImporter.h"
#include "SkinnedFBXImporter.h"
#include "SkinnedEnemy.h"
#include "T3DPhysicsAsset.h"
#include "GunAudio.h"
#include "WaterVolume.h"
#include "WaterRendererDX12.h"
#include "RainRendererDX12.h"
#include "CloudNoiseDX12.h"
#include "PalmTrees.h"
#include "LevelDefinition.h"
#include "EntityTransform.h"
#include "LevelEditor.h"
#include "PrefabRegistry.h"
#include "PrefabRuntime.h"
#include "AutomaticLod.h"
#include "PrefabColliders.h"
#include "RuntimeWorld.h"
#include "GameSession.h"
#include "FixedStepClock.h"
#include "RenderCoordinator.h"
#include "LevelRuntimeBuilder.h"
#include "CombatSystem.h"
#include "EnemySystem.h"
#include "VehicleSystem.h"
#include "RopeSwing.h"
#include "DeploymentPlanner.h"
#include "ArmoryCatalog.h"
#include "GameRuntime.h"
#include "DeferredReleaseQueue.h"
#include "AssetRegistry.h"
#include "AssetWatcher.h"
#include "PrefabThumbnailGenerator.h"
#include "GameSettings.h"
#include "BulletPenetration.h"

using namespace DirectX;

// Application modules are ordered to preserve declarations and static initialization.
#include "private/AppState.h"
#include "private/TerrainAndDamage.h"
#include "private/VehicleModels.h"
#include "private/Deployment.h"
#include "private/InsertionVehicles.h"
#include "private/EnemyVehicles.h"
#include "private/EnemySpawning.h"
#include "private/Combat.h"
#include "private/Environment.h"
#include "private/ProbeScene.h"
#include "private/WorldOverlays.h"
#include "private/LoadingState.h"
#include "private/ScopeView.h"
#include "private/PrefabThumbnails.h"
#include "private/Vegetation.h"
#include "private/PrefabAssets.h"
#include "private/SceneModels.h"
#include "private/PrefabWorld.h"
#include "private/WorldCollision.h"
#include "private/Objectives.h"
#include "private/LevelEnvironment.h"
#include "private/LevelSession.h"
#include "private/MenuTheme.h"
#include "private/Menus.h"
#include "private/AssetMaterials.h"
#include "private/DestructibleModels.h"
#include "private/EditorRuntime.h"
#include "private/Diagnostics.h"
#include "private/WindowAndGeometry.h"
#include "private/PlayerMovement.h"
#include "private/PlayerInteraction.h"
#include "private/VehicleCombat.h"
#include "private/WindowInput.h"
#include "private/Boot.h"


int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR commandLine, int nCmdShow) {
    SetUnhandledExceptionFilter(WriteCrashDump);
    // std::rand() defaults to seed 1, so every launch replayed the identical
    // sequence: the BlackHawk's first insertion always drew the same roll and
    // failed at the same second. Everything else using rand() (muzzle spread,
    // smoke jitter, audio pitch) was repeating per launch for the same reason.
    //
    // The warm-up matters: MSVC's generator returns almost the same first value
    // for adjacent seeds, so a time() seed alone leaves the first draw
    // effectively constant across launches -- and the BlackHawk's failure roll
    // is the first draw. Discarding a few values decorrelates it.
    std::srand(static_cast<unsigned>(std::time(nullptr)));
    for (int warmUp = 0; warmUp < 16; ++warmUp) (void)std::rand();
    AllocConsole();
    FILE* fp;
    freopen_s(&fp, "CONOUT$", "w", stdout);
    freopen_s(&fp, "CONOUT$", "w", stderr);

    EngineLog::ScopedSession logSession("GraphicEngine");
    g_assetWatcher.Start();

    std::cout << "GraphicEngine DX12 Starting..." << std::endl;

    // Window
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"GraphicEngineDX12";
    RegisterClassExW(&wc);

    DEVMODEW displayMode = {};
    displayMode.dmSize = sizeof(displayMode);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &displayMode)) {
        SCR_WIDTH = displayMode.dmPelsWidth;
        SCR_HEIGHT = displayMode.dmPelsHeight;
    }

    const LONG windowedWidth =
        static_cast<LONG>((std::min)(SCR_WIDTH, 1600u));
    const LONG windowedHeight =
        static_cast<LONG>((std::min)(SCR_HEIGHT, 900u));
    RECT rc = { 0, 0, windowedWidth, windowedHeight };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    const int windowX =
        (static_cast<int>(SCR_WIDTH) - (rc.right - rc.left)) / 2;
    const int windowY =
        (static_cast<int>(SCR_HEIGHT) - (rc.bottom - rc.top)) / 2;
    HWND hwnd = CreateWindowW(L"GraphicEngineDX12", L"Graphics Engine - DirectX 12",
        WS_OVERLAPPEDWINDOW, windowX, windowY,
        rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) { std::cerr << "Window creation failed\n"; return -1; }
    ShowWindow(hwnd, nCmdShow);
    ToggleFullscreen(hwnd);
    UpdateWindow(hwnd);

    // DX12
    try {
        if (!InitDX12(hwnd, SCR_WIDTH, SCR_HEIGHT)) {
            MessageBoxA(hwnd, "Failed to init DX12.", "Error", MB_OK | MB_ICONERROR);
            return -1;
        }
    } catch (const std::exception& e) {
        MessageBoxA(hwnd, e.what(), "DX12 Error", MB_OK | MB_ICONERROR);
        return -1;
    }

    if (!g_profiler.Init(g_dx12.device.Get(), g_dx12.commandQueue.Get()))
        std::cerr << "GPU profiler unavailable; CPU profiling remains active\n";
    g_profileDumpEnabled =
        GetEnvironmentVariableA("SGE_PROFILE_DUMP", nullptr, 0) > 0;
    g_forceTerrainErrorLOD =
        GetEnvironmentVariableA("SGE_TERRAIN_ERROR_LOD", nullptr, 0) > 0;
    scene.cacheFarShadowCascades =
        GetEnvironmentVariableA("SGE_CACHE_FAR_SHADOWS", nullptr, 0) > 0;
    g_gunAudio.Initialize("Content/Audio/rifle_shot.wav");
    g_rpgFireAudio.Initialize("Content/Audio/rpg_fire.wav");
    // Lives under build/Sounds like the RPG explosion above, not models/audio --
    // that tree is CMake-synced from source, this one ships in the build dir.
    // Missing audio degrades to silence (Play() no-ops when nothing loaded).
    g_reloadAudio.Initialize("Content/Audio/dragon-studio-gun-reload-2-504027.mp3");
    g_explosionAudio.Initialize("Content/Audio/RocketExplosion3.mp3");
    g_grenadeExplosionAudio.Initialize("Content/Audio/explosion.ogg");
    g_fireLoopAudio.Initialize("Content/Audio/Fire/fireplace_loop.wav",
                               AudioBus::Ambience);
    g_fireIgnitionAudio.Initialize("Content/Audio/Fire/ignition.ogg",
                                   AudioBus::Ambience);
    g_destructionBreakAudio[0].Initialize(
        "Content/Audio/Destruction/impact_03_wood.ogg");
    g_destructionBreakAudio[1].Initialize(
        "Content/Audio/Destruction/break_02_rock.ogg");
    g_destructionBreakAudio[2].Initialize(
        "Content/Audio/Destruction/break_03_wood.ogg");
    g_destructionImpactAudio[0].Initialize(
        "Content/Audio/Destruction/impact_01.ogg");
    g_destructionImpactAudio[1].Initialize(
        "Content/Audio/Destruction/impact_02_rock.ogg");
    g_destructionImpactAudio[2].Initialize(
        "Content/Audio/Destruction/impact_03_wood.ogg");
    scene.explosionAudioCallback = [](const XMFLOAT3& position, float size,
                                      bool grenade) {
        // Every explosion is also a light source. Routed through the audio
        // callback because it is the one hook that already fires for all of
        // them -- grenades, barrels, vehicles, the AA gun going up -- with the
        // position and size the flash needs.
        //
        // Scaled by size so a grenade is a bright pop and a fuel-air blast
        // whites the tube out completely.
        ReportNightVisionLightFlash(position,
                                    0.55f + (std::min)(2.6f, size * 0.30f));
        const float dx = position.x - scene.camera.Position.x;
        const float dy = position.y - scene.camera.Position.y;
        const float dz = position.z - scene.camera.Position.z;
        const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        const float reach = 55.0f + size * 4.0f;
        const float volume = (std::max)(0.0f, 1.0f - distance / reach);
        if (volume <= 0.01f) return;
        const float pitch = 0.92f + ((float)std::rand() / RAND_MAX) * 0.10f;
        // Sound travels noticeably slower than light. Cap delay so distant
        // gameplay feedback stays responsive while still selling scale.
        g_pendingExplosionAudio.push_back({
            (std::min)(0.35f, distance / 343.0f), volume, pitch, grenade });
    };
    scene.fireIgnitionAudioCallback = [](const XMFLOAT3& position) {
        const float dx = position.x - scene.camera.Position.x;
        const float dy = position.y - scene.camera.Position.y;
        const float dz = position.z - scene.camera.Position.z;
        const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        const float volume = (std::max)(0.0f, 1.0f - distance / 42.0f);
        if (volume <= 0.01f) return;
        const float pitch = 0.94f +
            ((float)std::rand() / (float)RAND_MAX) * 0.12f;
        g_fireIgnitionAudio.Play(volume, pitch);
    };
    g_hitAudio.Initialize("Content/Audio/bullet_flesh_hit.mp3");
    g_metalHitAudio.Initialize(
        "Content/Audio/Bullet/floraphonic-metal-hit-95-200424.mp3");
    // Enemy vocals ride the Voices bus so they can be mixed against gunfire --
    // a shout that is buried under a firefight is a cue the player never gets.
    g_banditSpottedAudio1.Initialize("Content/Audio/bandit_spotted_01.wav",
                                     AudioBus::Voices);
    g_banditSpottedAudio2.Initialize("Content/Audio/bandit_spotted_02.wav",
                                     AudioBus::Voices);
    g_readyToDropAudio.Initialize(
        "Content/Audio/Voicelines/Commander/ReadyToDrop.mp3", AudioBus::Voices);
    g_menuMusicAudio.Initialize("Content/Audio/Music/testbackground.wav",
                                AudioBus::Music);
    g_exfilHereAudio.Initialize(
        "Content/Audio/Voicelines/Commander/ExfilsHere.mp3", AudioBus::Voices);
    g_greatJobAudio.Initialize(
        "Content/Audio/Voicelines/Commander/GreatJobSoldier.mp3",
        AudioBus::Voices);
    g_plantC4TowerAudio.Initialize(
        "Content/Audio/Voicelines/Commander/Plantc4CommTower.mp3",
        AudioBus::Voices);
    g_banditAttackAudio.Initialize("Content/Audio/bandit_attack.wav",
                                   AudioBus::Voices);
    g_banditDeathAudio.Initialize("Content/Audio/bandit_death.wav",
                                  AudioBus::Voices);
    g_banditHitVoiceAudio.Initialize("Content/Audio/bandit_hit_voice.wav",
                                     AudioBus::Voices);
    g_helicopterHoverAudio.Initialize("Content/Audio/helicopter_hover_loop.mp3",
                                      AudioBus::Ambience);
    g_blackHawkAlarmAudio.Initialize(
        "Content/Audio/freesound_community-siren-alert-96052.mp3");
    // Grass footsteps. Add a Grass03.wav and bump kFootstepVariantCount to
    // widen the set -- PlayFootstep picks across whatever is loaded here.
    g_footstepAudio[0].Initialize("Content/Audio/Footsteps/Grass01.wav",
                                  AudioBus::Ambience);
    g_footstepAudio[1].Initialize("Content/Audio/Footsteps/Grass02.wav",
                                  AudioBus::Ambience);
    // Exhaustion breathing. Ambience, alongside the footsteps: it is body
    // sound, not a weapon or a voice line.
    g_breathingAudio.Initialize("Content/Audio/breathing_tired.ogg",
                                AudioBus::Ambience);

    // ImGui
    D3D12_DESCRIPTOR_HEAP_DESC ihd = {};
    ihd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    ihd.NumDescriptors = kImGuiDescriptorCount;
    ihd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    g_dx12.device->CreateDescriptorHeap(&ihd, IID_PPV_ARGS(&imguiSrvHeap));

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    // Replaces the bare StyleColorsDark that used to sit here, so every menu,
    // popup and editor panel inherits the HUD's palette instead of ImGui's
    // default grey.
    ApplyEngineUITheme();
    // Player settings before anything reads them. A missing file is the normal
    // first run: the defaults already in g_settings stand, and the file appears
    // the first time a setting is changed.
    LoadGameSettings(g_settings);
    ApplyGameSettings();
    // Career wallet, same story: no file means a fresh career at zero rather
    // than an error. Only the balance is restored -- kit is hired per mission,
    // so there is no ownership to carry in. A wallet written by an older build
    // still has its item section; LoadMoney simply ignores it now.
    LoadMoney(g_game.money);
    LoadMenuFonts();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX12_Init(g_dx12.device.Get(), FRAME_COUNT, DXGI_FORMAT_R8G8B8A8_UNORM,
        imguiSrvHeap.Get(),
        imguiSrvHeap->GetCPUDescriptorHandleForHeapStart(),
        imguiSrvHeap->GetGPUDescriptorHandleForHeapStart());

    // ImGui is live now, so the remaining init can draw progress. stepCount is
    // the number of BootStep() calls below, counted from a boot trace rather
    // than by eye -- it now paces the wordmark reveal, so an overestimate
    // leaves the word unfinished as the menu appears rather than just
    // mis-scaling a bar.
    g_boot.active = true;
    g_boot.hwnd = hwnd;
    g_boot.stepCount = 21;
    g_boot.startedAt = std::chrono::steady_clock::now();

    // Geometry
    BootStep("Building geometry...");
    if (!CreateAllGeometry()) { std::cerr << "Geometry creation failed\n"; return -1; }

    BootStep("Initializing bindless material heap...");
    ThrowIfFailed(g_dx12.commandAllocators[g_dx12.frameIndex]->Reset());
    ThrowIfFailed(g_dx12.commandList->Reset(
        g_dx12.commandAllocators[g_dx12.frameIndex].Get(), nullptr));
    const bool bindlessHeapReady = bindlessHeap.Init();
    ThrowIfFailed(g_dx12.commandList->Close());
    {
        ID3D12CommandList* bindlessLists[] = { g_dx12.commandList.Get() };
        g_dx12.commandQueue->ExecuteCommandLists(1, bindlessLists);
    }
    WaitForGPU();
    DumpDX12DebugMessages();
    std::cout << (bindlessHeapReady ? "Bindless material heap ready\n"
                                    : "Bindless material tier unavailable\n");

    // Shaders - the DX12-specific pair supports albedo/normal/metal-roughness texture
    // sampling (needed for imported GLB materials); the plain "clustered_*" pair is
    // color-only and silently ignores textures, so it's only a fallback.
    BootStep("Compiling shaders...");
    mainShader.SetBindlessHeap(&bindlessHeap);
    if (!mainShader.Load("shaders/clustered_dx12_vs.hlsl", "shaders/clustered_dx12_ps.hlsl")) {
        std::cerr << "Trying fallback shaders...\n";
        if (!mainShader.Load("shaders/clustered_vs.hlsl", "shaders/clustered_ps.hlsl")) {
            MessageBoxA(hwnd, "Shader load failed.", "Shader Error", MB_OK | MB_ICONERROR);
            return -1;
        }
    }
    std::cout << "Shaders loaded\n";
    if (!g_particleRenderer.Init())
        std::cerr << "GPU particle renderer unavailable; using draw fallback\n";
    if (!g_rainRenderer.Init())
        std::cerr << "Rain renderer unavailable (non-fatal)\n";
    if (!g_collisionDebugRenderer.Init())
        std::cerr << "Collision debug overlay unavailable (non-fatal)\n";

    BootStep("Initializing mesh shader pipeline...");
    g_useMeshShader = g_meshShader.Init(mainShader);
    std::cout << (g_useMeshShader
        ? "Mesh shader path enabled\n"
        : "Mesh shader path unavailable; using raster fallback\n");

    BootStep("Initializing occlusion depth...");
    if (!occlusionDepth.Init(SCR_WIDTH, SCR_HEIGHT)) {
        std::cerr << "Meshlet occlusion depth init failed (non-fatal)\n";
    }
    BootStep("Initializing FXAA...");
    if (!fxaa.Init(SCR_WIDTH, SCR_HEIGHT)) {
        std::cerr << "FXAA init failed (non-fatal)\n";
        scene.enableFXAA = false;
    }
    BootStep("Initializing night vision...");
    if (!nightVision.Init(SCR_WIDTH, SCR_HEIGHT))
        std::cerr << "Night vision init failed (non-fatal)\n";
    BootStep("Initializing volumetric fog...");
    BootStep("Initializing light shafts...");
    if (!lightShafts.Init())
        std::cerr << "Light shafts init failed (non-fatal)\n";
    if (!volumetricFog.Init()) {
        std::cerr << "Volumetric fog init failed (non-fatal)\n";
        scene.enableVolumetricFog = false;
    }
    // The cloud volumes are baked in EnsureSceneRenderAssets, which now runs
    // after this, so the fog is pointed at them there rather than here.
    BootStep("Initializing ambient occlusion...");
    if (!screenSpaceAO.Init()) {
        std::cerr << "Screen-space AO init failed (non-fatal)\n";
        scene.enableAmbientOcclusion = false;
    }
    BootStep("Initializing screen-space reflections...");
    if (!screenSpaceReflections.Init()) {
        std::cerr << "Screen-space reflections init failed (non-fatal)\n";
        scene.enableScreenSpaceReflections = false;
    }
    BootStep("Initializing water renderer...");
    if (!waterRenderer.Init(SCR_WIDTH, SCR_HEIGHT)) {
        std::cerr << "Tropical water renderer init failed (non-fatal)\n";
    } else if (!waterRenderer.UltraAvailable()) {
        std::cerr << "Ultra water unavailable: "
                  << waterRenderer.UltraFailureReason()
                  << "; High quality remains active\n";
    }
    char ultraWaterDebug[8] = {};
    if (GetEnvironmentVariableA(
            "SGE_WATER_DEBUG", ultraWaterDebug,
            static_cast<DWORD>(sizeof(ultraWaterDebug))) > 0)
        waterRenderer.SetUltraDiagnosticMode(
            static_cast<UINT>(std::strtoul(ultraWaterDebug, nullptr, 10)));
    BootStep("Initializing MSAA...");
    // Only the boot-time pipelines can be judged here. Terrain and sky are not
    // built yet, so their MSAA support is re-checked in EnsureSceneRenderAssets
    // once they exist -- testing them now would read a default-false "supported"
    // and leave MSAA on over a pipeline that cannot do it.
    const bool msaaPipelinesReady =
        mainShader.msaaSupported &&
        (!g_useMeshShader || g_meshShader.msaaSupported);
    if (!msaa.Init(SCR_WIDTH, SCR_HEIGHT) || !msaaPipelinesReady) {
        std::cerr << "4x MSAA unavailable (non-fatal)\n";
        scene.enableMSAA = false;
    }

    // Visibility buffer (id Tech path)
    // Init uploads the 3D colour LUT. Record and submit that work now; the
    // command list has been closed since the sky upload above.
    BootStep("Initializing visibility buffer...");
    visBuffer.SetBindlessHeap(&bindlessHeap);
    ThrowIfFailed(g_dx12.commandAllocators[g_dx12.frameIndex]->Reset());
    ThrowIfFailed(g_dx12.commandList->Reset(
        g_dx12.commandAllocators[g_dx12.frameIndex].Get(), nullptr));
    const bool visibilityBufferReady = visBuffer.Init(SCR_WIDTH, SCR_HEIGHT);
    ThrowIfFailed(g_dx12.commandList->Close());
    {
        ID3D12CommandList* visibilityInitLists[] = { g_dx12.commandList.Get() };
        g_dx12.commandQueue->ExecuteCommandLists(1, visibilityInitLists);
    }
    WaitForGPU();
    DumpDX12DebugMessages();
    if (!visibilityBufferReady) {
        std::cerr << "VB init failed (non-fatal)\n";
        scene.useVisibilityBuffer = false;
    } else {
        visBuffer.UpdateEnvironmentMap(
            g_specularEnvironmentResource, g_brdfIntegrationResource);
        // Reclaim mesh slots as destruction retires merged batch nodes.
        // Registration is upload-once, so without this every fracture rebuild
        // consumed fresh geometry storage until the pool ran dry and chunks
        // silently stopped rasterising into the visibility buffer.
        g_releaseVisibilityGeometry =
            [](const std::shared_ptr<SceneNode>& node) {
                if (!node || !node->mesh) return;
                for (MeshPrimitive& primitive : node->mesh->primitives)
                    visBuffer.ReleasePrimitive(&primitive);
            };
        std::cout << "Visibility Buffer ready\n";
    }

    // Scope targets are allocated after the main VB and post pipelines, not
    // before them: the scope is a consumer of that pipeline, and allocating
    // its targets first would let a scope-only failure sit in the middle of
    // the main renderer's own initialization.
    //
    // Failure is contained to the scope. The R700 keeps ordinary ADS with its
    // authored glass; the main renderer is never disabled or altered.
    BootStep("Initializing sniper scope...");
    scene.sniperPictureInPicture = g_sniperScope.Init(
        g_dx12.device.Get(), SniperScopeOptics::Resolution, SniperScopeOptics::Resolution);
    if (scene.sniperPictureInPicture && visibilityBufferReady && !visBuffer.InitScopeView()) {
        scene.sniperPictureInPicture = false;
        std::cerr << "Scope visibility resources unavailable; standard ADS remains active\n";
    }
    std::cout << (scene.sniperPictureInPicture
        ? "Sniper picture-in-picture scope ready\n"
        : "Sniper scope target unavailable; standard ADS remains active\n");
    g_bindlessMaterialsReady = bindlessHeapReady && mainShader.BindlessReady() &&
        visBuffer.BindlessResolveReady() &&
        (!g_useMeshShader || g_meshShader.bindlessReady);

    BootStep("Initializing grass MSAA...");
    if (!visibilityBufferReady || !mainShader.GetHDRMSAAGrassPipelineState() ||
        !grassMSAA.Init(SCR_WIDTH, SCR_HEIGHT)) {
        std::cerr << "Visibility grass 4x MSAA unavailable (non-fatal)\n";
        scene.enableGrassMSAA = false;
    } else {
        std::cout << "Visibility grass 4x MSAA ready\n";
    }

    BootStep("Initializing shadow maps...");
    if (!shadowMap.Init()) {
        std::cerr << "Shadow map init failed (non-fatal)\n";
        scene.enableShadows = false;
    }
    // Hand the spot atlas to the passes that sample it: t92 in the visibility
    // resolve, t20 in the forward path. Null when shadow init failed, which
    // both descriptor writers handle by leaving a valid null view in the slot.
    visBuffer.spotShadowAtlasResource = shadowMap.GetSpotResource();
    g_spotShadowAtlasResource = shadowMap.GetSpotResource();

    BootStep("Initializing DDGI probes...");
    if (!g_ddgiRenderer.init(g_dx12.device.Get())) {
        std::cerr << "DDGI init failed (non-fatal)\n";
        scene.useDDGI = false;
    } else {
        g_ddgiRenderer.RegisterShadowMap(shadowMap.GetResource());
        g_ddgiIrradianceResource = g_ddgiRenderer.irradianceTexture.Get();
        g_ddgiVisibilityResource = g_ddgiRenderer.visibilityTexture.Get();
        if (visibilityBufferReady) {
            visBuffer.UpdateDDGIResources(
                g_ddgiIrradianceResource, g_ddgiVisibilityResource);
        }
        std::cout << "DDGI ready\n";
    }

    // Raytracing (DXR path)
    BootStep("Building raytracing acceleration structures...");
    g_dxrDDGI.Initialize(g_dx12.device.Get());
    g_inlineRaytracingSupported =
        g_dxrDDGI.GetStatus().inlineRaytracingSupported;
    std::cout << "DXR inline raytracing (Tier 1.1): "
              << (g_inlineRaytracingSupported ? "supported" : "unavailable")
              << "\n";
    if (!InitRaytracing(geo)) {
        std::cerr << "DXR init failed (non-fatal)\n";
        scene.useRaytracing = false;
    } else {
        std::cout << "DXR Raytracing ready\n";
    }

    // Scene lights
    BootStep("Initializing scene lights...");
    scene.InitLights();

    // Last boot frame, so the bar reaches 100% instead of stopping short.
    BootStep("Ready");
    g_boot.active = false;

    // Timer
    gameTimer.Start();
    float lastTime = 0.0f;

    std::cout << "Controls: WASD, Mouse, TAB=UI, F11=Fullscreen, ESC=Exit\n";

    const std::filesystem::path startupLevel = StartupLevelPath(commandLine);
    if (!startupLevel.empty()) StartCustomLevel(hwnd, startupLevel);

    // Deterministic renderer smoke path for GPU validation and crash dumps.
    bool visibilityTestPending = false;
    UINT visibilityTestForwardFrames = 0;
    bool visibilitySmokeEnabled = false;
    bool visibilitySmokeReported = false;
    UINT visibilityCpuReportFrames = 0;
    const bool visibilityBenchmark =
        GetEnvironmentVariableA("SGE_VISIBILITY_BENCHMARK", nullptr, 0) > 0;
    const bool visibilityForwardOnly =
        GetEnvironmentVariableA("SGE_VISIBILITY_FORWARD_ONLY", nullptr, 0) > 0;
    const bool temporalGTAOSmokeTest =
        GetEnvironmentVariableA("SGE_TEMPORAL_GTAO_TEST", nullptr, 0) > 0;
    UINT visibilityBenchmarkVBFrames = 0;
    UINT visibilityBenchmarkForwardSamples = 0;
    UINT visibilityBenchmarkVBSamples = 0;
    double visibilityBenchmarkForwardMs = 0.0;
    double visibilityBenchmarkVBMs = 0.0;
    bool visibilityBenchmarkComplete = false;
    const bool particleBenchmark =
        GetEnvironmentVariableA("SGE_PARTICLE_BENCHMARK", nullptr, 0) > 0;
    UINT particleBenchmarkFrames = 0;
    UINT particleBenchmarkSamples = 0;
    double particleBenchmarkCpuMs = 0.0;
    double particleBenchmarkGpuMs = 0.0;
    const bool terrainLODBenchmark =
        GetEnvironmentVariableA("SGE_TERRAIN_LOD_BENCHMARK", nullptr, 0) > 0;
    UINT terrainLODBenchmarkFrames = 0;
    bool terrainLODBenchmarkComplete = false;
    std::vector<double> terrainLODScopeSamples;
    std::vector<double> terrainLODFrameSamples;
    std::vector<double> terrainLODShadowSamples;
    const bool molotovSmokeTest =
        GetEnvironmentVariableA("SGE_MOLOTOV_TEST", nullptr, 0) > 0;
    bool molotovSmokeInjected = false;
    UINT molotovSmokeFrames = 0;
    size_t molotovSmokePeakPatches = 0;
    const bool vortexSmokeTest =
        GetEnvironmentVariableA("SGE_VORTEX_TEST", nullptr, 0) > 0;
    bool vortexSmokeInjected = false;
    UINT vortexSmokeFrames = 0;
    size_t vortexSmokePeakFX = 0;
    uint32_t vortexSmokeActorsBefore = 0;
    uint32_t vortexSmokePeakActors = 0;
    size_t vortexSmokePeakBarrelBodies = 0;
    if (GetEnvironmentVariableA("SGE_VISIBILITY_TEST", nullptr, 0) > 0) {
        visibilitySmokeEnabled = true;
        std::ofstream("visibility_smoke.log", std::ios::trunc)
            << "starting\n";
        char visibilityDebugMode[8] = {};
        int requestedVisibilityDebugMode = -1;
        if (GetEnvironmentVariableA("SGE_VISIBILITY_DEBUG", visibilityDebugMode,
                static_cast<DWORD>(sizeof(visibilityDebugMode))) > 0) {
            requestedVisibilityDebugMode = (std::max)(0, (std::min)(7,
                atoi(visibilityDebugMode)));
        }
        scene.useVisibilityBuffer = false;
        if (temporalGTAOSmokeTest) {
            scene.enableAmbientOcclusion = true;
            scene.temporalBentNormalGTAO = true;
            // SGE_AO_BASELINE=1 runs the pre-optimization shader variant so an
            // automated A/B can measure both without driving the UI.
            if (GetEnvironmentVariableA("SGE_AO_BASELINE", nullptr, 0) > 0)
                scene.optimizedAmbientOcclusion = false;
            // SGE_AO_HALF_RES=1 traces AO at half resolution. Half res applies
            // to the scalar path only, so drop temporal bent normals with it --
            // that is the configuration the toggle is meant to measure.
            if (GetEnvironmentVariableA("SGE_AO_HALF_RES", nullptr, 0) > 0) {
                scene.halfResolutionAO = true;
                scene.temporalBentNormalGTAO = false;
            }
            char bentDebugMode[8] = {};
            if (GetEnvironmentVariableA(
                    "SGE_BENT_GTAO_DEBUG", bentDebugMode,
                    static_cast<DWORD>(sizeof(bentDebugMode))) > 0) {
                const UINT mode = static_cast<UINT>((std::max)(0,
                    (std::min)(3, atoi(bentDebugMode))));
                visBuffer.bentNormalGTAODebugMode =
                    static_cast<VisibilityBufferDX12::
                        BentNormalGTAODebugMode>(mode);
            }
        }
        visibilityTestPending = true;
        const bool emptyVisibilityTest =
            GetEnvironmentVariableA("SGE_VISIBILITY_TEST_FULL", nullptr, 0) == 0;
        const bool stressVisibilityTest =
            GetEnvironmentVariableA("SGE_VISIBILITY_TEST_STRESS", nullptr, 0) > 0;
        // SGE_VISIBILITY_TEST_GODMODE=1 reproduces the "LEVEL 1 - GOD MODE"
        // menu button, which starts with the full debug UI and mobile pad
        // visible. That extra per-frame UI work is not exercised by the plain
        // smoke-test path, so a fault that only appears there is otherwise
        // invisible to automated runs.
        const bool godModeUI =
            GetEnvironmentVariableA("SGE_VISIBILITY_TEST_GODMODE", nullptr, 0) > 0;
        StartLevelOne(hwnd, true, stressVisibilityTest, emptyVisibilityTest,
                      nullptr, godModeUI);
        // Level startup may restore renderer defaults. Apply the requested
        // diagnostic afterwards so unattended tests exercise the intended view.
        if (requestedVisibilityDebugMode >= 0)
            visBuffer.debugViewMode = requestedVisibilityDebugMode;
        // SGE_FORCE_NIGHT=1 pins the run to the only dark preset. Headlights
        // and searchlights early-out unless TimeOfDayIsDark, so a smoke test
        // that lands on the Afternoon default exercises none of that code.
        // Applied after StartLevelOne for the same reason as the line above.
        if (GetEnvironmentVariableA("SGE_FORCE_NIGHT", nullptr, 0) > 0) {
            g_selectedTimeOfDay = TimeOfDay::Night;
            ApplyTimeOfDay(g_selectedTimeOfDay);
        }
        // SGE_SSRT=1 forces the NGLighting screen-space tracer on. It is opt-in
        // and off by default, so an unattended run exercises none of its
        // passes -- including the accumulation ping-pong, which only has a
        // second buffer state to get wrong once it has run a frame.
        if (GetEnvironmentVariableA("SGE_SSRT", nullptr, 0) > 0) {
            scene.enableScreenSpaceReflections = true;
            scene.screenSpaceRTEnabled = true;
        }
        // Allows the unattended visibility smoke to compile and execute the
        // opt-in solar-disc, occlusion and dirt branches without changing the
        // normal launch default.
        if (GetEnvironmentVariableA("SGE_SUN_LENS", nullptr, 0) > 0)
            scene.enableSunLens = true;
        if (GetEnvironmentVariableA("SGE_SPOT_SHADOW_CACHE", nullptr, 0) > 0)
            scene.cacheSpotShadows = true;
        if (GetEnvironmentVariableA("SGE_PREFAB_SMOKE_TEST", nullptr, 0) > 0) {
            g_prefabRuntimeSmokeEnabled = true;
            LevelEntity rock;
            rock.id = 9000001;
            rock.type = LevelEntityType::Prefab;
            rock.name = "Prefab Smoke Rock";
            rock.prefabId = "rock";
            rock.transform.position[0] = 6.0f;
            rock.transform.position[2] = 6.0f;
            g_game.world.Level().entities.push_back(std::move(rock));
            g_prefabRebuildRequested = true;
            g_prefabRebuildReason = "prefab smoke test";
            SGE_LOG("LogPrefab", EngineLog::Level::Display,
                "Prefab smoke test injected rock instance");
        }
    }
    // SGE_COLLISION_TEST=1 loads the Training Range, which places the airport
    // prefab, and exits once its collision tree has been built. The BVH stats
    // land in the log during LoadPrefabModel, so this measures the real model
    // without needing anyone to sit at the menu.
    char r700Test[8]{};
    if (GetEnvironmentVariableA("SGE_R700_TEST", r700Test, sizeof(r700Test)) > 0) {
        GunModel::SelectedWeapon() = std::atoi(r700Test) == 8 ? 8 : 3;
        scene.holdAimDownSights = true;
    }

    if (GetEnvironmentVariableA("SGE_COLLISION_TEST", nullptr, 0) > 0) {
        g_collisionSmokeEnabled = true;
        // SGE_COLLISION_TEST_LEVEL names a level to load instead of the Training
        // Range, so the material budgets can be measured on a dense map rather
        // than on the near-empty one the collision checks themselves need.
        char levelName[260] = {};
        if (GetEnvironmentVariableA("SGE_COLLISION_TEST_LEVEL", levelName,
                static_cast<DWORD>(sizeof(levelName))) > 0) {
            const std::filesystem::path level =
                std::filesystem::path("Content/Levels") / levelName;
            std::error_code error;
            if (std::filesystem::exists(level, error)) {
                StartCustomLevel(hwnd, level);
                SGE_LOG("LogPrefab", EngineLog::Level::Display,
                    "Collision smoke test started (" + level.string() + ")");
            } else {
                SGE_LOG("LogPrefab", EngineLog::Level::Error,
                    "Collision smoke level not found: " + level.string());
                StartTrainingRange(hwnd);
            }
        } else {
            StartTrainingRange(hwnd);
            SGE_LOG("LogPrefab", EngineLog::Level::Display,
                "Collision smoke test started (Training Range)");
        }
    }
    // SGE_SHOTGUN_TEST=1 loads the repro level and fires buckshot from the
    // frame loop.
    if (GetEnvironmentVariableA("SGE_SHOTGUN_TEST", nullptr, 0) > 0) {
        g_shotgunSmokeEnabled = true;
        // SGE_SHOTGUN_TEST_WEAPON overrides the weapon index so the same loop
        // can empty a rifle magazine rather than only firing buckshot.
        char smokeWeapon[32] = {};
        GunModel::SelectedWeapon() =
            GetEnvironmentVariableA("SGE_SHOTGUN_TEST_WEAPON", smokeWeapon,
                                    sizeof(smokeWeapon)) > 0
                ? std::atoi(smokeWeapon)
                : 1;   // shotgun
        // SGE_SHOTGUN_TEST_LEVEL names a level to load instead of the repro
        // one, so the same volley logic can be pointed at real shipping
        // geometry (the Base's NATO shelter) rather than only the synthetic
        // helideck. Same override shape SGE_COLLISION_TEST_LEVEL already uses.
        char shotgunLevel[MAX_PATH] = {};
        std::error_code shotgunError;
        if (GetEnvironmentVariableA("SGE_SHOTGUN_TEST_LEVEL", shotgunLevel,
                                    sizeof(shotgunLevel)) > 0) {
            const std::filesystem::path named(shotgunLevel);
            if (std::filesystem::exists(named, shotgunError))
                StartCustomLevel(hwnd, named);
            else
                SGE_LOG("LogGameplay", EngineLog::Level::Error,
                    std::string("Shotgun smoke level not found: ") + shotgunLevel);
        } else {
        static constexpr const char* kCandidates[] = {
            "Content/Levels/ShotgunRepro.json",
            "levels/ShotgunRepro.json",
            "build/Content/Levels/ShotgunRepro.json",
        };
        for (const char* candidate : kCandidates) {
            if (!std::filesystem::exists(candidate, shotgunError)) continue;
            StartCustomLevel(hwnd, std::filesystem::path(candidate));
            break;
        }
        }
        SGE_LOG("LogGameplay", EngineLog::Level::Display,
            "Shotgun smoke test started");
    }
    // SGE_TRAVEL_TEST=1 starts the Base, which places the transport Black Hawk,
    // and hands the frame loop the staged checks below.
    if (GetEnvironmentVariableA("SGE_TRAVEL_TEST", nullptr, 0) > 0) {
        g_travelSmokeEnabled = true;
        StartBase(hwnd);
        SGE_LOG("LogGameplay", EngineLog::Level::Display,
            "Travel smoke test started (Base)");
    }
    if (GetEnvironmentVariableA("SGE_PREFAB_EDITOR_TEST", nullptr, 0) > 0) {
        g_prefabEditorSmokeEnabled = true;
        // SGE_EDITOR_LEVEL=<path> opens the editor on an authored level instead
        // of the Level 1 template. Without it there is no headless way to
        // reproduce a bug that only happens in a specific saved map.
        char editorLevel[MAX_PATH] = {};
        const DWORD editorLevelLength = GetEnvironmentVariableA(
            "SGE_EDITOR_LEVEL", editorLevel,
            static_cast<DWORD>(std::size(editorLevel)));
        const bool haveEditorLevel =
            editorLevelLength > 0 && editorLevelLength < std::size(editorLevel);
        StartLevelEditor(hwnd, haveEditorLevel
            ? std::filesystem::path(editorLevel) : std::filesystem::path());
        // SGE_EDITOR_PLAY=1 drops straight into a playtest, which is what arms
        // the objective timers.
        if (GetEnvironmentVariableA("SGE_EDITOR_PLAY", nullptr, 0) > 0)
            g_game.commands.Request(GameCommand::EditorBeginPlay);
        else
            g_levelEditor.OpenAssetBrowser();
        SGE_LOG("LogPrefab", EngineLog::Level::Display,
            "Prefab editor RTT smoke test started");
    }
    if (GetEnvironmentVariableA("SGE_DXR_DDGI_TEST", nullptr, 0) > 0) {
        if (g_game.session.Screen() != GameScreen::LevelEditor)
            StartLevelEditor(hwnd);
        LevelDXRDDGISettings& test = g_levelEditor.Level().dxrDDGI;
        test.enabled = true;
        test.showProbes = true;
        test.maxProbes = 256;
        test.raysPerProbe = 32;
        test.probesPerFrame = 8;
        SynchronizeEditorRuntime(false);
        SGE_LOG("LogRenderer", EngineLog::Level::Display,
            "DXR DDGI smoke test enabled");
    }

    // ?? main loop ????????????????????????????????????????????????????????????
    MSG msg = {};
    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            continue;
        }

        float now = gameTimer.GetElapsed();
        deltaTime = now - lastTime;
        lastTime  = now;
        // A level load, GPU drain or alt-tab stalls the loop for seconds, and
        // that whole stall lands in one frame's delta. Feeding it raw into
        // fixed-rate systems teleports them -- the BlackHawk's health drain
        // burned most of its life on the first frame after a reload, crashing
        // the bird seconds into a run that should last 7-20. Clamp to a very
        // long frame: above any legitimate one, far below a stall. The
        // lastTime resets after the reset/load blocks still help the frame
        // *after* a stall; this covers the stall frame itself.
        deltaTime = (std::min)(deltaTime, 0.1f);

        if (g_dx12.fence)
            g_retiredPrefabResources.Collect(
                g_dx12.fence->GetCompletedValue());

        if (IsEditorEditing() && !g_levelEditor.ImportInProgress() &&
            g_assetWatcher.ConsumeChange()) {
            const bool assetsChanged = g_assetRegistry.Refresh();
            if (assetsChanged ||
                g_prefabRegistry.Refresh(kPrefabRoot, kModelRoot)) {
                g_levelEditor.RefreshAssets();
                RetiredPrefabResources retired;
                retired.renderBatches =
                    std::move(g_game.world.Prefabs().renderBatches);
                retired.models = std::move(g_prefabModelCache);
                g_retiredPrefabResources.Retire(
                    g_dx12.lastDirectFenceValue, std::move(retired));
                ReleasePrefabRigidBodies();
                g_game.world.Prefabs().ClearDerived();
                g_prefabRebuildRequested = true;
                g_prefabRebuildReason = "asset change detected";
                SGE_LOG("LogPrefab", EngineLog::Level::Display,
                    "Asset change detected; prefab cache reloading");
            }
        }

        if (g_game.commands.Consume(GameCommand::EditorStopPlay)) {
            StopEditorPlaytest();
        }
        if (g_game.commands.Consume(GameCommand::EditorBeginPlay)) {
            BeginEditorPlaytest(hwnd);
        }
        if (g_game.commands.Consume(GameCommand::EditorReturnToMenu)) {
            g_customLevelMode = false;
            g_editorFullReconcileRequested = false;
            scene.useDestruction = g_editorPreviousDestructionEnabled;
            OpenMainMenu();
        }
        if (g_editorFullReconcileRequested && IsEditorEditing() &&
            !g_game.loading.Active()) {
            g_editorFullReconcileRequested = false;
            SynchronizeEditorRuntime(false);
        }
        if (g_pendingEnvironmentRebuild && IsSceneScreen() &&
            !g_game.loading.Active() && g_environmentInitialized) {
            WaitForGPU();
            RebuildScalableEnvironment();
            if (g_trees.IsInitialized()) ResetPalmTrees();
            g_pendingEnvironmentRebuild = false;
            // Consume the editor's request here rather than in the prefab
            // rebuild: the flag has to survive until the environment has
            // actually been rebuilt, or a latched dirty bit would re-trigger
            // the 1.3 s rebuild on every subsequent frame.
            g_levelEditor.MarkEnvironmentRuntimeSynchronized();
        }
        if (IsEditorEditing() && !g_game.loading.Active() &&
            g_levelEditor.RuntimeDirty()) {
            // Every ordinary edit is preview-only. Save/Play are the explicit
            // boundaries that rebuild collision, destruction, navmesh, grass,
            // audio and spawners.
            bool synchronized = false;
            if (g_levelEditor.TransformRuntimeDirty()) {
                synchronized = SynchronizeEditorRuntimeLight(
                    g_levelEditor.TransformRuntimeEntityId());
            }
            if (!synchronized) SynchronizeEditorRuntimeVisual();
            g_levelEditor.MarkRuntimeSynchronized();
        }

        if (IsEditorEditing() && !cameraLocked &&
            !ImGui::GetIO().WantCaptureKeyboard) {
            const float speed = ((GetAsyncKeyState(VK_SHIFT) & 0x8000) ? 3.0f : 1.0f)
                * g_editorCameraSpeed;
            if (GetAsyncKeyState('W') & 0x8000) scene.camera.ProcessKeyboard('W', deltaTime, speed);
            if (GetAsyncKeyState('S') & 0x8000) scene.camera.ProcessKeyboard('S', deltaTime, speed);
            if (GetAsyncKeyState('A') & 0x8000) scene.camera.ProcessKeyboard('A', deltaTime, speed);
            if (GetAsyncKeyState('D') & 0x8000) scene.camera.ProcessKeyboard('D', deltaTime, speed);
            if (GetAsyncKeyState(VK_SPACE) & 0x8000)
                scene.camera.Position.y += scene.camera.MovementSpeed * speed * deltaTime;
            if (GetAsyncKeyState('Q') & 0x8000)
                scene.camera.Position.y -= scene.camera.MovementSpeed * speed * deltaTime;
        }

        UpdateHelicopterHoverAudio();
        UpdateFireLoopAudio();

        g_profiler.BeginCpuFrame();
        {
        ProfilerDX12::CpuScope updateProfile(g_profiler, "Update");

        // Front-end score runs outside the gameplay gate below: the main menu is
        // not a gameplay screen, so anything inside that branch never ticks
        // there and the music would only ever start once a level was running.
        g_menuMusicAudio.Update();
        // Outside the gameplay gate below: the win screen is not a gameplay
        // screen, so a voice started there would never be reclaimed.
        g_greatJobAudio.Update();
        UpdateMenuMusic();

        if (IsGameplayScreen() && !g_game.loading.Active() &&
            (scene.player.godMode || scene.player.health > 0.0f)) {

        if (g_game.commands.Consume(GameCommand::ResetLevelRuntime)) {
            // Restart is requested from ImGui after the previous frame's enemy
            // draws were recorded. Drain GPU use before destroying their buffers.
            WaitForGPU();
            g_bandits.clear();
            g_heldBandit = nullptr;
            // Lightning ticks only while alive and off the loading screen, so a
            // run that ended mid-flash freezes its stroke counter and the next
            // storm never schedules a strike. Restart clears it either way.
            ResetLightning();
            if (!g_emptyLevelMode) g_water.ResetSurface();
            if (!g_emptyLevelMode) g_ocean.ResetSurface();
            if (!g_emptyLevelMode && g_trees.IsInitialized()) ResetPalmTrees();
            if (!g_emptyLevelMode && g_environmentInitialized &&
                g_environmentStressMode != g_stressTestMode)
                RebuildScalableEnvironment();
            if (!g_emptyLevelMode) {
                for (size_t i = 0; i < ActiveBanditSlotCount(); ++i)
                    if (!SpawnBandit()) break;
                // The reset cleared g_bandits, which holds both factions, so
                // the allies have to be rebuilt here too -- the load-stage
                // spawn only runs on a cold load, not on a restart.
                SpawnMarinesFromLevel();
                // Restart path: re-scatter so each run gets its own layout
                // rather than inheriting the one the cold load produced.
                ScatterEnemiesOnNavmesh();
                g_game.commands.Set(GameCommand::RespawnTurretGunner,
                    !g_customLevelMode ||
                    FirstRuntimeEntity(LevelEntityType::Humvee) != nullptr);
                g_game.commands.Set(GameCommand::RespawnBoatGunner,
                    g_levelPatrolBoatEnabled);
            } else {
                g_game.commands.Set(GameCommand::RespawnTurretGunner, false);
                g_game.commands.Set(GameCommand::RespawnBoatGunner, false);
            }
            // Do not charge restart/reset stalls to completion time.
            lastTime = gameTimer.GetElapsed();
        }

        g_game.session.Tick(deltaTime);

        ProcessInput(hwnd);
        UpdateDeploymentPlanningCamera(deltaTime);
        // Player throws happen during input. Create the rigid body before
        // Scene::Update so manual projectile gravity never runs for one frame.
        SyncGrenadePhysicsBodies(false);

        // Walking collision: ground level follows the mesh-shader terrain at
        // the camera's XZ so gravity settles the player onto the hills. Skipped
        // while riding the insertion helicopter, where the seat drives the
        // camera and terrain/world collision would drag the player out of it.
        const bool ridingBlackHawk = g_game.vehicles.blackHawkCarryingPlayer;
        const bool deploymentPlanning = g_insertionChoicePending;
        if (deploymentPlanning) {
            scene.camera.FloorY = 0.0f;
        } else if (ridingBlackHawk) {
            // Keep the floor under the cabin so gravity has nothing to pull on.
            scene.camera.FloorY =
                scene.camera.Position.y - scene.camera.PlayerHeight;
        } else if (scene.useMeshTerrain && g_terrain.supported) {
            auto terrainParams = CurrentTerrainParams();
            terrainParams.heightScale = scene.terrainHeightScale;
            scene.camera.FloorY = TerrainRendererDX12::HeightAt(
                terrainParams, scene.camera.Position.x, scene.camera.Position.z);
        } else {
            scene.camera.FloorY = 0.0f;
        }

        // Hand the camera the local sea surface so it can decide whether the
        // player is swimming. Riding a vehicle or planning deployment keeps the
        // player dry regardless of where they are, so the surface is parked
        // out of reach in those cases rather than special-cased in the camera.
        if (!g_emptyLevelMode && !ridingBlackHawk && !deploymentPlanning &&
            !g_game.vehicles.insertionBoatCarryingPlayer && !g_drivingHumvee) {
            scene.camera.WaterSurfaceY =
                g_ocean.GetSurfaceY() +
                g_ocean.WaveHeightAt(scene.camera.Position.x,
                                     scene.camera.Position.z);
        } else {
            scene.camera.WaterSurfaceY = Camera::NoWater;
        }

        // Resolve wall/roof collision BEFORE gravity runs (scene.Update ->
        // camera.Update). Step-up raises FloorY onto brick/roof tops so the
        // ground snap in the same frame stands the player on them instead of
        // falling through. Uses last frame's body transforms -- fine for
        // standing, and avoids a one-frame lag that would drop the player.
        if (!ridingBlackHawk && !deploymentPlanning) {
            if (scene.useDestruction && g_destruction.IsInitialized()) {
                g_destruction.ResolvePlayerCollision(scene.camera.Position,
                    scene.camera.FloorY, 0.35f, scene.camera.PlayerHeight,
                    g_drivingHumvee ? g_activeHumveeIndex : kNoHumvee);
            }
            ResolvePlayerWorldObjectCollisions(
                scene.camera.Position, scene.camera.FloorY,
                0.35f, scene.camera.PlayerHeight);
            ResolvePlayerPrefabCollisions(scene.camera.Position,
                                          scene.camera.FloorY, 0.35f,
                                          scene.camera.PlayerHeight);
            if (!g_emptyLevelMode)
                ResolvePlayerBlackHawkCollision(
                    scene.camera.Position, scene.camera.FloorY,
                    0.35f, scene.camera.PlayerHeight);
        }

        // Held grenades follow the final collision-resolved camera pose. Scene
        // still advances their fuse below, so waiting too long remains lethal.
        UpdateHeldGrenade();
        // Before scene.Update, so the ambient lift below is in place for the
        // frame the flash is drawn on rather than one frame late.
        UpdateLightning(deltaTime);
        scene.Update(deltaTime, now);
        scene.burningTargets.clear();
        if (molotovSmokeTest && !molotovSmokeInjected && IsSceneScreen() &&
            !g_game.loading.Active()) {
            const XMFLOAT3& eye = scene.camera.Position;
            const XMFLOAT3& front = scene.camera.Front;
            scene.SpawnMolotovFire({
                eye.x + front.x * 4.5f, eye.y + front.y * 4.5f,
                eye.z + front.z * 4.5f });
            scene.selectedGrenade = GrenadeType::Molotov;
            molotovSmokeInjected = true;
        }
        if (molotovSmokeInjected)
            molotovSmokePeakPatches = (std::max)(
                molotovSmokePeakPatches, scene.firePatches.size());
        if (vortexSmokeTest && !vortexSmokeInjected && IsSceneScreen() &&
            !g_game.loading.Active() && g_destruction.IsInitialized()) {
            const XMFLOAT3 center = { 0.0f, 3.0f, 0.0f };
            vortexSmokeActorsBefore = g_destruction.GetDebugData().actorCount;
            scene.SpawnVortexFX(center);
            const float orbitCenterY = center.y + scene.vortexRadius * 0.35f;
            const float capture = scene.vortexRadius * 1.35f;
            for (ExplosiveBarrel& barrel : scene.explosiveBarrels) {
                const float dx = barrel.position.x - center.x;
                const float dy = barrel.position.y - orbitCenterY;
                const float dz = barrel.position.z - center.z;
                if (!barrel.active || barrel.held ||
                    dx * dx + dy * dy + dz * dz > capture * capture)
                    continue;
                if (EnsureExplosiveBarrelBody(barrel)) {
                    barrel.thrown = true;
                    barrel.vortexHoldTime = scene.vortexDuration;
                    barrel.vortexCenter = { center.x, orbitCenterY, center.z };
                }
            }
            g_destruction.StartVortex(
                center, scene.vortexRadius, scene.vortexDuration);
            scene.selectedGrenade = GrenadeType::Vortex;
            vortexSmokeInjected = true;
        }
        if (vortexSmokeInjected) {
            vortexSmokePeakFX = (std::max)(
                vortexSmokePeakFX, scene.vortexFX.size());
            vortexSmokePeakActors = (std::max)(vortexSmokePeakActors,
                g_destruction.GetDebugData().actorCount);
            const size_t activeBarrelBodies = static_cast<size_t>(std::count_if(
                scene.explosiveBarrels.begin(), scene.explosiveBarrels.end(),
                [](const ExplosiveBarrel& barrel) {
                    return barrel.physicsHandle != 0;
                }));
            vortexSmokePeakBarrelBodies = (std::max)(
                vortexSmokePeakBarrelBodies, activeBarrelBodies);
        }
        UpdateMolotovFireDamage();
        // Repeatable submission benchmark: 800 persistent smoke cards (runtime
        // cap). Isolates particle draw overhead from spawn/simulation cost.
        if (particleBenchmark && IsSceneScreen() && !g_game.loading.Active()) {
            scene.impactParticles.clear();
            scene.impactParticles.reserve(800);
            const XMVECTOR camera = XMLoadFloat3(&scene.camera.Position);
            const XMVECTOR forward = XMVector3Normalize(
                XMLoadFloat3(&scene.camera.Front));
            const XMVECTOR right = XMVector3Normalize(XMVector3Cross(
                XMLoadFloat3(&scene.camera.Up), forward));
            const XMVECTOR up = XMVector3Normalize(XMVector3Cross(forward, right));
            for (UINT i = 0; i < 800; ++i) {
                const float x = (static_cast<float>(i % 40) - 19.5f) * 0.42f;
                const float y = (static_cast<float>((i / 40) % 25) - 12.0f) * 0.28f;
                const float z = 7.0f + static_cast<float>(i % 7) * 0.12f;
                ImpactParticle particle;
                XMStoreFloat3(&particle.position,
                    camera + forward * z + right * x + up * y);
                particle.velocity = {};
                particle.maxLife = 1000.0f;
                particle.life = 800.0f;
                particle.size = 0.16f;
                particle.growth = 0.0f;
                particle.color = { 0.34f, 0.31f, 0.27f };
                scene.impactParticles.push_back(particle);
            }
        }
        // Vehicles and the deployment fly-through move the camera without the
        // player taking a step. The tracker measures raw camera displacement;
        // left enabled, either motion reads as a sprint in the viewmodel.
        const bool nonLocomotionCameraMotion = g_insertionChoicePending ||
            g_drivingHumvee || g_game.vehicles.blackHawkCarryingPlayer;
        const float playerHorizontalSpeed = g_game.playerMovement.Update(
            scene.ViewmodelAnchorPosition(), deltaTime,
            !nonLocomotionCameraMotion);
        // Each weapon can carry its own nudge on top of the shared grip point,
        // so the body offset is re-solved when the selection changes. A weapon
        // whose nudge is still zero resolves to exactly the shared value, which
        // is why switching does not move the arms until one is tuned.
        ArmsModel::SetGripWeapon(GunModel::SelectedWeapon());
        // Sprint is suppressed alongside the speed reading: riding a vehicle or
        // the deployment fly-through moves the camera without the player taking
        // a step, and a held shift there would otherwise sprint the legs.
        ArmsModel::Update(deltaTime, playerHorizontalSpeed, scene.adsBlend,
                          g_playerSprinting && !nonLocomotionCameraMotion);

        // Crosshair bloom target. Three contributions, summed then capped:
        //
        //   movement -- scaled by measured speed against sprint pace, so walking
        //               opens it a little and sprinting opens it a lot;
        //   recoil   -- gunRecoilKick is already the per-shot impulse decaying
        //               back to zero, which is exactly the shape wanted here, so
        //               firing needs no separate timer;
        //   crouch   -- a bonus, not a penalty: bracing tightens the group.
        //
        // Aiming down sights collapses it entirely. The reticle is faded out at
        // that point anyway, and a bloom underneath the weapon's own sights
        // would be describing a spread the sighted shot does not have.
        {
            constexpr float kMaxSpread = 15.0f;
            constexpr float kSprintReferenceSpeed = 7.5f;  // sprint m/s
            const float speedFraction = nonLocomotionCameraMotion ? 0.0f :
                (std::min)(1.0f, playerHorizontalSpeed / kSprintReferenceSpeed);
            float target = speedFraction * 8.5f;
            // gunRecoilKick is in degrees of viewmodel pitch; 0.85 px per degree
            // puts a rifle burst near the movement contribution rather than
            // swamping it.
            target += scene.gunRecoilKick * 0.85f;
            if (scene.camera.IsCrouching) target *= 0.55f;
            const float sighted =
                (std::max)(0.0f, (std::min)(1.0f, scene.adsBlend));
            target *= (1.0f - sighted);
            const SGE::ResolvedWeaponStats weaponStats =
                scene.player.ResolveWeaponStats(GunModel::SelectedWeapon());
            target *= weaponStats.hipSpreadMultiplier +
                (weaponStats.adsSpreadMultiplier -
                 weaponStats.hipSpreadMultiplier) * sighted;
            scene.crosshairSpreadTarget =
                (std::min)(kMaxSpread, (std::max)(0.0f, target));
        }
        if (!g_emptyLevelMode) {
            UpdateHelicopter(deltaTime);
            // Before the patrol update, which yields the airframe while a
            // reinforcement wave owns it.
            UpdateReinforcementDropship(deltaTime);
            UpdateSecondaryHelicopter(deltaTime);
            UpdateEnemyHelicopterDamageSmoke(deltaTime);
            UpdateBoat(deltaTime);
            // Aim the insertion once the level is actually up. Waits for the
            // model and for loading to finish, so the drop-off is taken from
            // the settled player spawn. The arming frame uses zero delta so a
            // load-sized step cannot advance a run that only just started.
            bool armedInsertionThisFrame = false;
            // Hold the run until a PlayerChoice map has its answer, then arm
            // only the craft that was picked -- the other one does not fly at
            // all, so the level opens with a single insertion, not a convoy.
            if (g_blackHawkInsertionRestartPending && g_blackHawkModel &&
                !g_insertionChoicePending && !g_game.loading.Active()) {
                g_blackHawkInsertionRestartPending = false;
                const LevelInsertionMode mode = ResolvedInsertionMode();
                if (mode == LevelInsertionMode::Helicopter ||
                    mode == LevelInsertionMode::FastRappel) {
                    // Before the run is aimed: the seat offset feeds both the
                    // ride point and the rappel rope, and the rope is hung from
                    // the pose the very first update produces.
                    ApplyBlackHawkSeatSide();
                    StartBlackHawkInsertionAtPlayerSpawn();
                    // Face out of the door once the seat is occupied. Deferred
                    // rather than set here: the aircraft's yaw is settled by the
                    // first UpdateBlackHawk, not by the call that arms the run.
                    g_blackHawkRideFacingPending = true;
                }
                else
                    g_game.vehicles.DisableBlackHawkInsertion();
                armedInsertionThisFrame = true;
                // The old wreck's triangles describe a helicopter that no
                // longer exists; drop them so the fresh run is not blocked by
                // the ghost of the previous crash.
                g_blackHawkCollisionMesh.Clear();
            }
            // Terrain under the wreck, so it stops on the actual ground rather
            // than the drop-off's elevation. Sampled before the update so the
            // impact test this frame uses a height for where it already is.
            {
                VehicleSystem& vehicles = g_game.vehicles;
                float crashGround = 0.0f;
                if (scene.useMeshTerrain && g_terrain.supported) {
                    auto params = CurrentTerrainParams();
                    params.heightScale = scene.terrainHeightScale;
                    crashGround = TerrainRendererDX12::HeightAt(
                        params, vehicles.blackHawkPosition.x,
                        vehicles.blackHawkPosition.z);
                }
                vehicles.blackHawkCrashGroundY = crashGround;
            }
            g_game.vehicles.UpdateBlackHawk(
                armedInsertionThisFrame ? 0.0f : deltaTime);
            // Between the airframe and the player: the rope needs the pose the
            // update just produced, and the player is then placed from the rope.
            // Shares the arming guard, so a load-sized delta cannot be dumped
            // into a 16-iteration solver on the frame the run starts.
            UpdateBlackHawkRope(armedInsertionThisFrame ? 0.0f : deltaTime);
            SpinBlackHawkRotor();
            RidePlayerInBlackHawk(deltaTime);
            UpdateBlackHawkCrashEffects(deltaTime);

            // Insertion boat, armed and stepped the same way so a load-sized
            // delta cannot advance a run that only just started.
            bool armedBoatRunThisFrame = false;
            if (g_insertionBoatRestartPending && g_insertionBoatModel &&
                !g_insertionChoicePending && !g_game.loading.Active()) {
                g_insertionBoatRestartPending = false;
                if (ResolvedInsertionMode() == LevelInsertionMode::Boat)
                    StartInsertionBoatRunAtPlayerSpawn();
                else
                    g_game.vehicles.DisableInsertionBoat();
                armedBoatRunThisFrame = true;
            }
            if (g_insertionBoatModel) {
                constexpr uint64_t kInsertionBoatWaterQueryId =
                    0x494e53455254424full;
                if (scene.waterQuality == WaterQuality::Ultra &&
                    g_game.vehicles.insertionBoatVisible) {
                    WaterHeightResult surface;
                    if (waterRenderer.TryGetHeightResult(
                            kInsertionBoatWaterQueryId, surface))
                        g_game.vehicles.insertionBoatWaterY = surface.height;
                    WaterHeightQuery query;
                    query.objectId = kInsertionBoatWaterQueryId;
                    query.worldXZ = {
                        g_game.vehicles.insertionBoatPosition.x,
                        g_game.vehicles.insertionBoatPosition.z};
                    waterRenderer.QueueHeightQuery(query);
                }
                const XMFLOAT3 previousBoatPosition =
                    g_game.vehicles.insertionBoatPosition;
                g_game.vehicles.UpdateInsertionBoat(
                    armedBoatRunThisFrame ? 0.0f : deltaTime);
                RidePlayerInInsertionBoat();
                UpdateInsertionBoatDamageEffects(deltaTime);
                if (scene.waterQuality == WaterQuality::Ultra &&
                    g_game.vehicles.insertionBoatVisible &&
                    deltaTime > 1e-4f) {
                    const XMFLOAT3& boat =
                        g_game.vehicles.insertionBoatPosition;
                    const float vx =
                        (boat.x - previousBoatPosition.x) / deltaTime;
                    const float vz =
                        (boat.z - previousBoatPosition.z) / deltaTime;
                    if (vx * vx + vz * vz > 0.04f) {
                        WaterInteraction wake;
                        wake.worldXZ = {boat.x, boat.z};
                        wake.radius = 1.35f;
                        wake.heightImpulse = 0.018f;
                        wake.velocityImpulse = {-vx * 0.012f, -vz * 0.012f};
                        wake.type = WaterInteractionType::Wake;
                        waterRenderer.QueueInteraction(wake);
                    }
                }
            }
            // After both transports, so whichever one raised the flag this
            // frame is honoured. Outside the g_insertionBoatModel guard on
            // purpose: a helicopter insertion must still land its squad on a
            // build where the boat asset failed to load.
            DropDeploymentMarines();
            UpdateExplosiveBarrels(deltaTime);
            // After the aircraft update, so the gun leads this frame's pose
            // rather than one that is already a frame stale.
            UpdateAATurret(deltaTime);
        }
        // Listener follows the camera, so every PlayAt this frame pans against
        // where the player actually is. Written before any audio is triggered
        // below, or a shot fired this frame would be placed against last
        // frame's listener -- audible as a lag when turning quickly.
        {
            const float listenerPos[3] = { scene.camera.Position.x,
                                           scene.camera.Position.y,
                                           scene.camera.Position.z };
            const float listenerFront[3] = { scene.camera.Front.x,
                                             scene.camera.Front.y,
                                             scene.camera.Front.z };
            const float listenerUp[3] = { scene.camera.Up.x,
                                          scene.camera.Up.y,
                                          scene.camera.Up.z };
            AudioDevice::SetListener(listenerPos, listenerFront, listenerUp);
        }
        g_gunAudio.Update();
        g_rpgFireAudio.Update();
        for (GunAudio& step : g_footstepAudio) step.Update();
        g_breathingAudio.Update();
        g_reloadAudio.Update();
        for (PendingExplosionAudio& sound : g_pendingExplosionAudio)
            sound.delay -= deltaTime;
        for (const PendingExplosionAudio& sound : g_pendingExplosionAudio) {
            if (sound.delay <= 0.0f)
                (sound.grenade ? g_grenadeExplosionAudio : g_explosionAudio)
                    .Play(sound.volume, sound.pitch);
        }
        g_pendingExplosionAudio.erase(
            std::remove_if(g_pendingExplosionAudio.begin(),
                           g_pendingExplosionAudio.end(),
                [](const PendingExplosionAudio& sound) { return sound.delay <= 0.0f; }),
            g_pendingExplosionAudio.end());
        g_explosionAudio.Update();
        g_grenadeExplosionAudio.Update();
        g_fireLoopAudio.Update();
        g_fireIgnitionAudio.Update();
        g_destructionBreakAudioCooldown = (std::max)(
            0.0f, g_destructionBreakAudioCooldown - deltaTime);
        g_destructionImpactAudioCooldown = (std::max)(
            0.0f, g_destructionImpactAudioCooldown - deltaTime);
        for (GunAudio& sound : g_destructionBreakAudio) sound.Update();
        for (GunAudio& sound : g_destructionImpactAudio) sound.Update();
        g_hitAudio.Update();
        g_metalHitAudio.Update();
        g_banditSpottedAudio1.Update();
        g_banditSpottedAudio2.Update();
        g_readyToDropAudio.Update();
        g_plantC4TowerAudio.Update();
        g_exfilHereAudio.Update();
        // Fires once, then parks at -1 so it cannot retrigger.
        if (g_exfilHereDelay >= 0.0f) {
            g_exfilHereDelay -= deltaTime;
            if (g_exfilHereDelay <= 0.0f) {
                g_exfilHereAudio.Play(2.0f);
                g_exfilHereDelay = -1.0f;
            }
        }
        // Fires once, then parks at -1 so it cannot retrigger.
        if (g_plantC4TowerDelay >= 0.0f) {
            g_plantC4TowerDelay -= deltaTime;
            if (g_plantC4TowerDelay <= 0.0f) {
                // 2x gain: this line is authored quieter than the other
                // commander callouts, so unity leaves it buried under them.
                g_plantC4TowerAudio.Play(2.0f);
                g_plantC4TowerDelay = -1.0f;
            }
        }
        g_banditAttackAudio.Update();
        g_banditDeathAudio.Update();
        g_banditHitVoiceAudio.Update();
        g_enemySystem.TickCooldowns(deltaTime);
        g_playerPainCooldown = (std::max)(0.0f, g_playerPainCooldown - deltaTime);

        if (scene.rebuildDestructionRequested && wallModel) {
            scene.rebuildDestructionRequested = false;
            // Re-init frees the old chunk vertex/index buffers. The GPU may
            // still be rendering last frame's chunk meshes, so drain it first
            // or those buffers get destroyed in flight and crash.
            WaitForGPU();
            // Rebuild the destructible set from the level as it stands now.
            // normalWallModel/stressWallModel are baked once at asset-load time
            // from whatever level was current then, so re-initializing straight
            // from them would resurrect the previous level's comm towers (at the
            // previous level's coordinates) and miss this one's. Re-cloning the
            // house template and re-running the arrangement is what the editor's
            // SynchronizeEditorRuntime already does for the same reason.
            if (g_customLevelMode && g_houseTemplate) {
                wallModel = CloneSceneTree(g_houseTemplate);
                ArrangeHousesInCross(wallModel, g_stressTestMode);
            }
            g_destruction.Initialize(wallModel, g_dx12.device.Get(), 1, 1, 1);
            // Re-init rebuilds physics with a flat ground; restore the terrain
            // heightfield collider so debris keeps colliding with real ground.
            auto tp = CurrentTerrainParams();
            tp.heightScale = scene.terrainHeightScale;
            g_destruction.SetTerrainSampler([tp](float x, float z) {
                return TerrainRendererDX12::HeightAt(tp, x, z);
            }, CurrentPhysicsTerrainExtent());
            if (!g_emptyLevelMode) {
                g_destruction.SetSplashCallback([](float x, float z, float s) {
                    g_water.Splash(x, z, s);
                });
                // Skipped on the training range: the Humvee is hidden there, and
                // a physics body without a model is collision the player cannot
                // see and cannot explain.
                //
                // Also skipped in a custom level that places no Humvee entity.
                // primaryHumveeSpawn keeps its {0, 3.45, 0} default in that
                // case, so without this check every such level got an
                // unrequested Humvee sitting at the world origin.
                InitializeLevelHumveePhysics();
            }
        }
        // Dead Bandits stay attached to their ragdolls. No mid-level respawns.
        if (!g_emptyLevelMode && g_banditLoaded &&
            !g_insertionChoicePending) {
            ProfilerDX12::CpuScope banditProfile(g_profiler, "Bandit Update");
            if (g_game.commands.Pending(GameCommand::RespawnTurretGunner)) {
                g_game.commands.Set(GameCommand::RespawnTurretGunner,
                    !SpawnLevelHumveeTurretGunners());
            }
            if (g_game.commands.Pending(GameCommand::RespawnBoatGunner)) {
                g_game.commands.Set(GameCommand::RespawnBoatGunner,
                    !SpawnBoatTurretGunner());
            }
            if (g_heldBandit && g_heldBandit->Dead()) g_heldBandit = nullptr;
            static std::unordered_map<SkinnedEnemy*, float> banditUpdateDebt;
            bool burnedBanditDied = false;
            bool coverQuerySpent = false;
            // Gathered once per frame so each actor's target can be resolved
            // without rescanning g_bandits per actor: bandits aim at the
            // nearest of {player, live marine}, marines aim at the nearest
            // live bandit.
            std::vector<XMFLOAT3> liveMarinePositions;
            std::vector<XMFLOAT3> liveBanditPositions;
            XMFLOAT3 insertionVehicleTarget{};
            const bool insertionVehicleOccupied =
                OccupiedInsertionVehicleTarget(insertionVehicleTarget);
            for (const auto& b : g_bandits) {
                if (!b || b->Dead()) continue;
                // Torso, not feet. position.y is the ground the actor stands
                // on, and the LOS raycast starts at the shooter's chest -- a
                // ray from chest height down to ground level dives into the
                // terrain over any real distance, so every actor-vs-actor
                // sight test failed and the aim-up could never complete. The
                // player never hit this because camera.Position is eye height.
                const XMFLOAT3 torso{
                    b->position.x, b->position.y + b->footOffset + 1.35f,
                    b->position.z };
                if (b->faction == Faction::Marine) liveMarinePositions.push_back(torso);
                else liveBanditPositions.push_back(torso);
            }
            for (auto& bandit : g_bandits) {
                if (!bandit) continue;
                if (bandit->UpdateBurning(
                        deltaTime, scene.molotovDamagePerSecond))
                    burnedBanditDied = true;
                if (bandit->Burning()) {
                    scene.burningTargets.push_back({
                        { bandit->position.x,
                          bandit->position.y + bandit->footOffset + 1.0f,
                          bandit->position.z },
                        1.25f, bandit->BurnFraction(), now });
                    if (bandit->ConsumeBurnSpreadEvent())
                        scene.SpawnCarriedFire(bandit->position);
                }
                if (bandit->Dead()) continue;
                // Pushed before the held/turret early-outs below so every live
                // actor picks up live slider edits, not just the ones that
                // reach the general movement path.
                bandit->leftArmReach = g_banditLeftArmReach;
                bandit->headTorsoYawOffsetDegrees = g_banditHeadYawOffsetDegrees;
                bandit->gunScale = g_banditGunScale;
                bandit->gunGripForward = g_banditGunGripForward;
                bandit->gunGripRise = g_banditGunGripRise;
                bandit->gunRearGripForward = g_banditGunRearGripForward;
                bandit->gunRearGripInboard = g_banditGunRearGripInboard;
                bandit->gunRearGripDrop = g_banditGunRearGripDrop;
                bandit->gunForeGripLateral = g_banditGunForeGripLateral;
                bandit->gunForeGripRise = g_banditGunForeGripRise;
                if (bandit.get() == g_heldBandit) {
                    const XMFLOAT3& eye = scene.camera.Position;
                    const XMFLOAT3& front = scene.camera.Front;
                    const XMFLOAT3 holdPosition = {
                        eye.x + front.x * 2.15f,
                        eye.y + front.y * 2.15f - bandit->footOffset - 1.15f,
                        eye.z + front.z * 2.15f };
                    const float facingYaw = std::atan2(-front.x, -front.z);
                    bandit->HoldAt(deltaTime, holdPosition, facingYaw);
                    continue;
                }
                // Marines loiter near the player instead of near their own
                // spawn point, so they end up close enough to notice a bandit
                // and tag along as the player moves through the level. The
                // leash drops the moment one engages: following forces yaw
                // toward the player, which both aims the marine at the wrong
                // thing and keeps its vision cone off the bandit, so a leashed
                // marine could never hold an aim long enough to fire.
                if (bandit->faction == Faction::Marine) {
                    if (bandit->Awareness() ==
                        SkinnedEnemy::AwarenessState::Combat)
                        bandit->leashPosition.reset();
                    else
                        bandit->leashPosition = scene.camera.Position;
                }
                // Shooting at the insertion craft needs the same perception a
                // shot at anything else does: close enough to make it out, and
                // an unobstructed line to it.
                //
                // This used to be occupancy plus faction alone, so the moment
                // the craft carried the player every bandit on the island
                // opened up on it -- through fog, through hillsides, at night,
                // from the far shore. That made the weather and time-of-day
                // choice worthless on approach, which is the one stretch where
                // cover matters most: the player cannot shoot back or break
                // away from the flight path.
                //
                // Aircraft read from further off than a man on foot: they are
                // large, loud and skylined, so the range gets a multiplier
                // rather than reusing the infantry figure unchanged. It is
                // still scaled by g_enemyVisionScale, which already folds in
                // both the light level and the fog density -- so dense fog or
                // a night insertion genuinely hides the approach.
                bool attackingInsertionVehicle =
                    insertionVehicleOccupied &&
                    bandit->faction == Faction::Bandit;
                if (attackingInsertionVehicle) {
                    const float vdx = insertionVehicleTarget.x - bandit->position.x;
                    const float vdy = insertionVehicleTarget.y - bandit->position.y;
                    const float vdz = insertionVehicleTarget.z - bandit->position.z;
                    const float distanceSq = vdx * vdx + vdy * vdy + vdz * vdz;
                    const float spotRange =
                        bandit->VisionRange() * kInsertionVehicleSpotRangeScale;
                    attackingInsertionVehicle = distanceSq <= spotRange * spotRange &&
                        BanditHasLineOfSight(*bandit, insertionVehicleTarget);
                }
                const XMFLOAT3 target = attackingInsertionVehicle
                    ? insertionVehicleTarget
                    : NearestHostileTarget(
                        *bandit, scene.camera.Position,
                        liveMarinePositions, liveBanditPositions);
                if (attackingInsertionVehicle)
                    bandit->ForceCombatTarget(target);
                const float cameraDx = bandit->position.x - scene.camera.Position.x;
                const float cameraDz = bandit->position.z - scene.camera.Position.z;
                const float cameraDistanceSq = cameraDx * cameraDx + cameraDz * cameraDz;
                const float updateInterval = cameraDistanceSq > 55.0f * 55.0f
                    ? (1.0f / 15.0f)
                    : (cameraDistanceSq > 30.0f * 30.0f ? (1.0f / 30.0f) : 0.0f);
                float& updateDebt = banditUpdateDebt[bandit.get()];
                updateDebt += deltaTime;
                const bool updateBandit = updateInterval == 0.0f ||
                    updateDebt >= updateInterval;
                const float banditDeltaTime = updateBandit ? updateDebt : 0.0f;
                if (updateBandit) updateDebt = 0.0f;
                if (updateBandit && !coverQuerySpent &&
                    bandit->NeedsCoverQuery(target)) {
                    XMFLOAT3 coverPosition;
                    if (QueryBanditCover(
                            *bandit, target, coverPosition)) {
                        // Bandits hold longer the more hurt they are, a term
                        // tuned around 100 max health. Marines keep a flat hold
                        // instead: they are outnumbered and cannot use cover as
                        // well as the player, and 7-10s is long enough to
                        // actually shoot from rather than popping out mid-burst.
                        //
                        // This used to be forced -- at the marine's old 400 max
                        // health the scaled term went negative and collapsed
                        // onto SetCoverTarget's 2.5s floor. Marines are 100 now,
                        // so the bandit formula would work; the flat hold is
                        // kept deliberately, as an ally behaviour choice rather
                        // than a workaround.
                        const float holdTime =
                            bandit->faction == Faction::Marine
                            ? 7.0f + RandomUnit() * 3.0f
                            : 3.25f +
                              (100.0f - (std::max)(0.0f, bandit->health)) * 0.035f +
                              RandomUnit() * 1.5f;
                        bandit->SetCoverTarget(coverPosition, holdTime);
                    } else {
                        bandit->MarkCoverQueryFailed(0.8f + RandomUnit() * 0.7f);
                    }
                    coverQuerySpent = true;
                }
                if (updateBandit && bandit->turretGunner) {
                    XMFLOAT3 mount{};
                    if (bandit->mountedVehicleIndex == kBoatGunnerMount) {
                        mount = BoatTurretMountWorld();
                    } else if (bandit->mountedVehicleIndex ==
                               kStressHumveeGunnerMount) {
                        mount = {
                            g_secondaryHumveePosition.x + g_humveeTurretLocal.x,
                            g_humveeTurretLocal.y + 3.45f,
                            g_secondaryHumveePosition.z + g_humveeTurretLocal.z };
                    } else {
                        mount = HumveeTurretMountWorld(
                            static_cast<size_t>(bandit->mountedVehicleIndex));
                    }
                    const bool playerControlsTurret = g_drivingHumvee &&
                        bandit->mountedVehicleIndex >= 0 &&
                        g_activeHumveeIndex == static_cast<size_t>(
                            bandit->mountedVehicleIndex);
                    if (!playerControlsTurret &&
                        bandit->mountedVehicleIndex >= 0) {
                        UpdateHumveeTurretAimAt(
                            static_cast<size_t>(bandit->mountedVehicleIndex),
                            target, banditDeltaTime);
                    }
                    bandit->UpdateMounted(
                        banditDeltaTime, mount,
                        playerControlsTurret &&
                        g_activeHumveeIndex < g_humveeGameplay.size()
                            ? g_humveeGameplay[g_activeHumveeIndex].aimPoint
                            : target);
                } else if (updateBandit && bandit->Rappelling()) {
                    // On the dropship rope: descend toward the terrain under the
                    // actor and skip the ground AI entirely until it lands.
                    // Update() would snap position.y to the ground on its first
                    // frame, which is exactly the descent this replaces.
                    bandit->UpdateRappel(
                        banditDeltaTime,
                        GroundHeightAt(bandit->position.x, bandit->position.z));
                } else if (updateBandit) {
                    float groundY = 0.0f;
                    if (scene.useMeshTerrain && g_terrain.supported) {
                        auto tp = CurrentTerrainParams();
                        tp.heightScale = scene.terrainHeightScale;
                        groundY = TerrainRendererDX12::HeightAt(
                            tp, bandit->position.x, bandit->position.z);
                    }
                    if (g_boatModel && !g_boatSunk) {
                        const BoatPlatformPose boatPose =
                            CurrentBoatPlatformPose();
                        if (BoatDeckSupports(
                                bandit->position, bandit->position.y,
                                boatPose, 0.20f, 0.48f))
                            groundY = (std::max)(groundY, BoatDeckY(boatPose));
                    }
                    // Prefab surfaces the heightfield cannot describe: a
                    // watchtower deck, a container roof, a walkway. Only ever
                    // raises the ground, so an actor on open terrain is
                    // unaffected.
                    float prefabSurfaceY = 0.0f;
                    const bool onPrefabSurface = PrefabSurfaceSupports(
                        bandit->position, bandit->position.y,
                        kBanditPrefabSupportDrop, prefabSurfaceY);
                    if (onPrefabSurface)
                        groundY = (std::max)(groundY, prefabSurfaceY);
                    const XMFLOAT3 preMovePosition = bandit->position;
                    bandit->Update(banditDeltaTime, target, groundY);
                    // Keep an actor on the platform it started the frame on.
                    // The navmesh is terrain-only, so steering happily walks a
                    // bandit off a watchtower deck; without this it would path
                    // straight over the edge and fall. Cancelling the move
                    // leaves it at the ledge facing the player rather than
                    // teleporting it back to the middle.
                    if (onPrefabSurface && !bandit->Dead() && !bandit->Held()) {
                        float steppedSurfaceY = 0.0f;
                        const bool stillSupported = PrefabSurfaceSupports(
                            bandit->position, prefabSurfaceY,
                            kBanditLedgeProbeDrop, steppedSurfaceY);
                        // A drop larger than a step down is a ledge, not a
                        // ramp or a stair tread.
                        if (!stillSupported ||
                            prefabSurfaceY - steppedSurfaceY >
                                kBanditMaxStepDown) {
                            bandit->position.x = preMovePosition.x;
                            bandit->position.z = preMovePosition.z;
                            bandit->position.y = prefabSurfaceY;
                        }
                    }
                    // After the ledge check, so standing on a prop is decided
                    // before the walls of that same prop push sideways.
                    ResolveBanditPrefabCollisions(*bandit);
                    ResolveBanditHumveeCollision(*bandit);
                }
                XMFLOAT3 shotOrigin, shotDirection;
                // Terrain LOS is deliberately expensive. Test only when a shot
                // is actually ready, not every frame of the two-second aim pause.
                const bool hasLineOfSight =
                    !bandit->NeedsLineOfSightCheck() ||
                    BanditHasLineOfSight(*bandit, target);

                // Grenade throw: mid-range only, on its own cooldown, and gated
                // on the same line of sight the rifle uses so nobody lobs
                // through a wall. Mounted gunners keep to the turret. Patrol/
                // Alert actors have no confirmed target to throw at.
                //
                // Bandits throw at the player, so a dead/godmode player disarms
                // them. Marines throw at bandits, which is independent of player
                // state -- gate each on what its own throw actually needs.
                const bool marineThrower = bandit->faction == Faction::Marine;
                const bool throwerReady = marineThrower
                    ? true
                    : (!scene.player.godMode && scene.player.health > 0.0f);
                if (!bandit->Dead() && !bandit->turretGunner && throwerReady &&
                    !attackingInsertionVehicle &&
                    bandit->Awareness() == SkinnedEnemy::AwarenessState::Combat) {
                    bandit->grenadeCooldown -= deltaTime;
                    if (bandit->grenadeCooldown <= 0.0f) {
                        const float gx = bandit->position.x - target.x;
                        const float gz = bandit->position.z - target.z;
                        const float range = std::sqrt(gx * gx + gz * gz);
                        const bool inBand = range >= kBanditGrenadeMinRange &&
                                            range <= kBanditGrenadeMaxRange;
                        // The blast damages everyone in radius, so an ally must
                        // check its own side of the impact before committing.
                        // Without this a marine will cheerfully frag the player
                        // or a squadmate who is pushing the same target.
                        const bool blastSafe =
                            !marineThrower ||
                            AllyGrenadeBlastIsSafe(*bandit, target);
                        if (inBand && hasLineOfSight && blastSafe) {
                            const float chance = marineThrower
                                ? kMarineGrenadeChance : kBanditGrenadeChance;
                            if (RandomUnit() < chance)
                                BanditThrowGrenade(
                                    *bandit, target, !marineThrower);
                            // Re-arm whether or not the roll passed, so a
                            // thrower that declines does not retry every frame.
                            const float cooldownMin = marineThrower
                                ? kMarineGrenadeCooldownMin
                                : kBanditGrenadeCooldownMin;
                            const float cooldownMax = marineThrower
                                ? kMarineGrenadeCooldownMax
                                : kBanditGrenadeCooldownMax;
                            bandit->grenadeCooldown = cooldownMin +
                                RandomUnit() * (cooldownMax - cooldownMin);
                        }
                    }
                }
                // Only the player's velocity is tracked, so lead only the shots
                // actually aimed at the player. NearestHostileTarget also
                // returns marine and bandit positions, and the insertion craft
                // is a separate target again -- leading those with the player's
                // velocity would push the aim somewhere meaningless.
                const bool targetIsPlayer =
                    !attackingInsertionVehicle &&
                    target.x == scene.camera.Position.x &&
                    target.y == scene.camera.Position.y &&
                    target.z == scene.camera.Position.z;
                const XMFLOAT3 leadVelocity =
                    targetIsPlayer ? g_playerVelocity : XMFLOAT3{ 0.0f, 0.0f, 0.0f };
                const bool playerControlsMountedTurret = g_drivingHumvee &&
                    bandit->turretGunner &&
                    bandit->mountedVehicleIndex >= 0 &&
                    g_activeHumveeIndex == static_cast<size_t>(
                        bandit->mountedVehicleIndex);
                const bool fired = !playerControlsMountedTurret &&
                    bandit->TryFireAt(
                        deltaTime, target, hasLineOfSight,
                        shotOrigin, shotDirection,
                        leadVelocity, scene.projectileSpeed);
                // Spotted/attack shouts are positional: hearing which direction
                // you were called out from is the whole point of the cue.
                if (bandit->ConsumeSpottedEvent() && g_banditVoiceCooldown <= 0.0f) {
                    const XMFLOAT3& p = bandit->position;
                    const float pitch = 0.96f + ((float)std::rand() / RAND_MAX) * 0.08f;
                    if (std::rand() & 1)
                        g_banditSpottedAudio1.PlayAt(p.x, p.y, p.z, 0.78f, pitch,
                                                     kBanditVoiceRange);
                    else
                        g_banditSpottedAudio2.PlayAt(p.x, p.y, p.z, 0.78f, pitch,
                                                     kBanditVoiceRange);
                    g_banditVoiceCooldown = 3.5f;
                }
                if (bandit->ConsumeAttackEvent() && g_banditVoiceCooldown <= 0.0f) {
                    const XMFLOAT3& p = bandit->position;
                    const float pitch = 0.96f + ((float)std::rand() / RAND_MAX) * 0.08f;
                    g_banditAttackAudio.PlayAt(p.x, p.y, p.z, 0.78f, pitch,
                                               kBanditVoiceRange);
                    g_banditVoiceCooldown = 4.5f;
                }
                if (fired) {
                    if (bandit->IsShotgunner()) {
                        // Cone of individually weak pellets. Overlapping hits at
                        // point-blank are what make it lethal; at range the cone
                        // is wide enough that most pellets miss entirely.
                        const XMVECTOR forward =
                            XMVector3Normalize(XMLoadFloat3(&shotDirection));
                        XMVECTOR up = std::fabs(XMVectorGetY(forward)) > 0.95f
                            ? XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f)
                            : XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
                        const XMVECTOR right =
                            XMVector3Normalize(XMVector3Cross(up, forward));
                        up = XMVector3Cross(forward, right);
                        for (int pellet = 0; pellet < kBanditShotgunPellets; ++pellet) {
                            const float spreadRight =
                                (RandomUnit() * 2.0f - 1.0f) * kBanditShotgunSpread;
                            const float spreadUp =
                                (RandomUnit() * 2.0f - 1.0f) * kBanditShotgunSpread;
                            XMFLOAT3 pelletDirection;
                            XMStoreFloat3(&pelletDirection, XMVector3Normalize(
                                forward + right * spreadRight + up * spreadUp));
                            scene.SpawnHostileProjectile(
                                shotOrigin, pelletDirection, 1.6f);
                        }
                    } else if (bandit->IsSniper()) {
                        // One heavy, fast round. Damage is high because the five
                        // second laser gave the player every chance to not be
                        // standing there.
                        scene.SpawnHostileProjectile(
                            shotOrigin, shotDirection, 18.0f, 2.2f);
                    } else if (bandit->faction == Faction::Bandit) {
                        scene.SpawnHostileProjectile(shotOrigin, shotDirection);
                    } else {
                        // Marine rifle shot: behaves like a player shot for
                        // hit-testing -- damages bandits only, never the player.
                        scene.SpawnPlayerProjectile(shotOrigin, shotDirection);
                    }
                    // Any actor's gunfire is audible to the AI, not just the
                    // player's. Without this a marine could only ever notice a
                    // bandit inside its 160-degree vision cone -- and a
                    // following marine faces the player, so bandits shooting
                    // from its flank went unheard. Marines now turn and engage
                    // when a firefight starts nearby.
                    g_enemyNoiseEvents.push_back(
                        { shotOrigin, SkinnedEnemy::AlertBroadcastRadius() });
                    // Positional: an enemy shooting from the left is heard on
                    // the left. The distance falloff that used to be computed
                    // here by hand now comes from the emitter's rolloff curve.
                    //
                    // Half volume: several enemies firing at once stacked into a
                    // wall of sound that buried the player's own weapon and the
                    // voice cues. Quieter enemy fire keeps a firefight legible.
                    const float pitch =
                        0.88f + ((float)std::rand() / RAND_MAX) * 0.08f;
                    g_gunAudio.PlayAt(shotOrigin.x, shotOrigin.y, shotOrigin.z,
                                      0.29f, pitch, 70.0f);
                }
            }
            if (burnedBanditDied) PlayBanditDeathEvents();
            UpdateHelicopterRotorKills();
            // Clear after every enemy has had a chance to read this frame's
            // noise/alert events (pushed earlier in the frame by player shots,
            // or below by projectile-vs-bandit hits), not before -- clearing
            // up front wiped a shot's own noise event before any bandit ever
            // saw it, since ShootPlayerWeapon runs earlier in the frame.
            g_enemyNoiseEvents.clear();
            g_enemyAlertEvents.clear();
        }
        if (!g_emptyLevelMode) {
        g_water.Update(deltaTime);
        g_ocean.Update(deltaTime);
        g_trees.SetWind(g_grass.WindStrength(), g_grass.WindSpeed());
        const bool primaryHelicopterActive =
            scene.showHelicopter && !g_helicopterDead && !g_helicopterCrashed;
        const bool secondaryHelicopterActive =
            scene.showHelicopter && SecondaryHelicopterPresent() &&
            !g_secondaryHelicopterDead && !g_secondaryHelicopterCrashed;
        // SGE_HELI_TRACE=1 reports what each airframe is doing once a second.
        // The draw gates and the transforms are in different places, so a
        // helicopter drawn somewhere it should not be is only diagnosable by
        // reading the state that actually feeds the matrix.
        static bool heliTrace =
            GetEnvironmentVariableA("SGE_HELI_TRACE", nullptr, 0) > 0;
        if (heliTrace) {
            static float heliTraceTimer = 0.0f;
            heliTraceTimer += deltaTime;
            if (heliTraceTimer >= 1.0f) {
                heliTraceTimer = 0.0f;
                const VehicleSystem& v = g_game.vehicles;
                std::cout
                    << "[heli] show=" << scene.showHelicopter
                    << " levelScale=" << g_helicopterLevelScale
                    << " modelScale=" << g_helicopterModelScale
                    << "\n  primary  drawn=" << (scene.showHelicopter ? 1 : 0)
                    << " dead=" << g_helicopterDead
                    << " crashed=" << g_helicopterCrashed
                    << " pos=" << g_helicopterPosition.x << ","
                    << g_helicopterPosition.y << ","
                    << g_helicopterPosition.z
                    << "\n  second   drawn=" << (SecondaryHelicopterVisible() ? 1 : 0)
                    << " present=" << (SecondaryHelicopterPresent() ? 1 : 0)
                    << " dead=" << g_secondaryHelicopterDead
                    << " pos=" << g_secondaryHelicopterPosition.x << ","
                    << g_secondaryHelicopterPosition.y << ","
                    << g_secondaryHelicopterPosition.z
                    << "\n  blackhawk vis=" << v.blackHawkVisible
                    << " phase=" << static_cast<int>(v.blackHawkPhase)
                    << " route=" << v.blackHawkRouteValid
                    << " pos=" << v.blackHawkPosition.x << ","
                    << v.blackHawkPosition.y << ","
                    << v.blackHawkPosition.z << "\n";
            }
        }
        // The insertion BlackHawk is the one that actually comes down onto the
        // island; the attack birds hold altitude. Gated on height so the
        // high-altitude run in does not drag a wash across everything under its
        // flight path.
        const bool blackHawkWashActive =
            g_game.vehicles.blackHawkVisible &&
            (g_game.vehicles.blackHawkPosition.y -
             g_game.vehicles.blackHawkGroundY) <= kBlackHawkWashHeight;
        // Palms carry two wash sources. While the BlackHawk is low it takes the
        // secondary slot: it is the closest rotor to the trees by a wide margin,
        // and the second attack bird is usually elsewhere on the map.
        const bool palmSecondaryIsBlackHawk =
            blackHawkWashActive && !secondaryHelicopterActive;
        g_trees.SetHelicopterWind(
            g_helicopterPosition, primaryHelicopterActive,
            palmSecondaryIsBlackHawk ? g_game.vehicles.blackHawkPosition
                                     : g_secondaryHelicopterPosition,
            palmSecondaryIsBlackHawk || secondaryHelicopterActive);
        g_trees.Update(deltaTime);
        for (const XMFLOAT3& firePosition : g_trees.GetBurningPositions()) {
            if (scene.burningTargets.size() >= 48) break;
            scene.burningTargets.push_back(
                { firePosition, 2.2f, 1.0f, now });
        }
        // Grass takes one wash source, so pick whichever downdraft the player
        // is standing closest to.
        const XMVECTOR camera = XMLoadFloat3(&scene.camera.Position);
        const auto distanceTo = [&camera](const XMFLOAT3& p) {
            return XMVectorGetX(XMVector3LengthSq(XMLoadFloat3(&p) - camera));
        };
        XMFLOAT3 grassHelicopter = g_helicopterPosition;
        float grassHelicopterDistance = primaryHelicopterActive
            ? distanceTo(g_helicopterPosition) : FLT_MAX;
        if (secondaryHelicopterActive) {
            const float secondaryDistance =
                distanceTo(g_secondaryHelicopterPosition);
            if (secondaryDistance < grassHelicopterDistance) {
                grassHelicopter = g_secondaryHelicopterPosition;
                grassHelicopterDistance = secondaryDistance;
            }
        }
        if (blackHawkWashActive) {
            const float blackHawkDistance =
                distanceTo(g_game.vehicles.blackHawkPosition);
            if (blackHawkDistance < grassHelicopterDistance) {
                grassHelicopter = g_game.vehicles.blackHawkPosition;
                grassHelicopterDistance = blackHawkDistance;
            }
        }
        g_grass.SetHelicopterWind(
            grassHelicopter,
            primaryHelicopterActive || secondaryHelicopterActive ||
            blackHawkWashActive);
        // Grass parts around the player's FEET, not the camera: the eye sits a
        // body-height above the blades being pushed, and feeding it in would
        // drag the parted ring along whenever the player looks around while
        // crouching. Grounded alone is not enough: it is also true on roofs and
        // vehicle decks, so require the feet to be close to the terrain itself.
        const XMFLOAT3 playerFeetPosition(
            scene.camera.Position.x,
            scene.camera.Position.y - scene.camera.PlayerHeight,
            scene.camera.Position.z);
        const float playerTerrainY = GroundHeightAt(
            playerFeetPosition.x, playerFeetPosition.z);
        const bool playerTouchesTerrain =
            scene.camera.FPSMode && scene.camera.IsGrounded &&
            !scene.camera.IsSwimming &&
            std::abs(playerFeetPosition.y - playerTerrainY) <= 0.45f;
        g_grass.SetPlayerPush(playerFeetPosition,
                              playerTouchesTerrain);
        g_grass.Update(deltaTime);
        }
        UpdatePlayerVelocity(scene.camera.Position, deltaTime);
        if (scene.useDestruction && g_destruction.IsInitialized()) {
            g_destruction.SetEnemyTarget(scene.camera.Position);
            // Enemy throws happen after Scene::Update. Capture them before this
            // frame's fixed physics steps as well.
            SyncGrenadePhysicsBodies(false);
            {
                ProfilerDX12::CpuScope profile(g_profiler, "Destruction Update");
                g_game.physicsClock.Accumulate(deltaTime);
                float physicsStep = 0.0f;
                // physicsClock hands out up to 4 steps after a long frame, and
                // each Update() already scales its own cost down under load,
                // so without a ceiling here a single hitch could still stack
                // four of them into one frame. This caps the whole scope --
                // the number the profiler reports as "Destruction Update", and
                // the one measured at up to 17.5 ms before any of this.
                //
                // The adaptive quality inside Update() is what normally keeps
                // the total under this, so hitting the cap should be rare;
                // it exists so the very first spike of a collapse cannot land
                // in full before the controller has seen it.
                const auto destructionBegin = std::chrono::steady_clock::now();
                constexpr double kDestructionFrameBudgetMs = 6.0;
                while (g_game.physicsClock.Consume(physicsStep)) {
                    g_destruction.Update(physicsStep);
                    if (std::chrono::duration<double, std::milli>(
                            std::chrono::steady_clock::now() -
                            destructionBegin).count() >=
                        kDestructionFrameBudgetMs) {
                        // Drop the rest of the backlog rather than deferring
                        // it into the next frame, which is what turns one slow
                        // frame into a run of them.
                        g_game.physicsClock.Reset();
                        break;
                    }
                }
            }
            SyncGrenadePhysicsBodies(true);
            const std::vector<uint32_t> grenadeContactEvents =
                g_destruction.DrainGrenadeContactEvents();
            for (const DestructionBurningPoint& fire :
                 g_destruction.GetBurningChunkPoints()) {
                if (scene.burningTargets.size() >= 72) break;
                scene.burningTargets.push_back({
                    fire.position, fire.size, fire.intensity, now });
            }
            if (!g_emptyLevelMode) {
                UpdateHumveeImpacts(deltaTime);
                UpdateHumveeChaseCamera(deltaTime);
                UpdateHumveeTurretAim(deltaTime);
                UpdateWeaponPickups(deltaTime);
                UpdateFootsteps(deltaTime);
            }
            if (!g_emptyLevelMode && g_banditLoaded) {
                const std::vector<DestructionDebrisHazard> debris =
                    g_destruction.GetDangerousDebris(2.5f);
                for (const DestructionDebrisHazard& piece : debris) {
                    for (auto& bandit : g_bandits) {
                        if (!bandit || bandit->Dead()) continue;
                        XMFLOAT3 impact;
                        if (!bandit->ApplyDebrisImpact(piece, &impact)) continue;

                        XMVECTOR normalVector = -XMLoadFloat3(&piece.velocity);
                        normalVector = XMVector3Normalize(normalVector);
                        XMFLOAT3 normal;
                        XMStoreFloat3(&normal, normalVector);
                        scene.SpawnBloodBurst(impact, normal);
                        const float pitch =
                            0.9f + ((float)std::rand() / RAND_MAX) * 0.2f;
                        g_hitAudio.Play(0.72f * 0.3f, pitch);
                        if (!bandit->Dead() && g_banditPainCooldown <= 0.0f) {
                            g_banditHitVoiceAudio.Play(
                                BanditVoiceVolume(impact, 0.72f), pitch);
                            g_banditPainCooldown = 0.45f;
                        }
                        break; // one moving chunk damages one character per frame
                    }
                }
                PlayBanditDeathEvents();
                for (auto& bandit : g_bandits)
                    if (bandit && bandit->Dead()) bandit->SyncRagdoll();
            }
            for (const EnemyShot& shot : g_destruction.DrainEnemyShots()) {
                scene.SpawnHostileProjectile(shot.origin, shot.direction);
                // Half volume, matching the infantry fire above -- these are the
                // same enemies shooting, just routed through the destruction
                // system's shot queue.
                const float pitch = 0.88f + ((float)std::rand() / RAND_MAX) * 0.08f;
                g_gunAudio.PlayAt(shot.origin.x, shot.origin.y, shot.origin.z,
                                  0.275f, pitch, 70.0f);
            }
            // Smoke and a rate-limited break sound at actual fracture points.
            const auto breakPoints = g_destruction.DrainBreakPoints();
            if (g_game.session.TimerRunning() && !breakPoints.empty()) {
                g_game.mission.RecordDestruction(
                    static_cast<uint32_t>(breakPoints.size()));
                // One award for the whole batch rather than one per fracture:
                // a collapsing building drains dozens of break points in a
                // single frame, and a popup per chunk would bury the HUD.
                g_game.money.Award(MoneyEvent::PropDestroyed,
                                   static_cast<int>(breakPoints.size()));
            }
            for (const XMFLOAT3& bp : breakPoints) {
                scene.SpawnSmokeBurst(bp, 0.5f, 0.4f);
                if (g_destructionBreakAudioCooldown <= 0.0f) {
                    const float dx = bp.x - scene.camera.Position.x;
                    const float dy = bp.y - scene.camera.Position.y;
                    const float dz = bp.z - scene.camera.Position.z;
                    const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                    const float volume = 0.78f * (std::max)(
                        0.0f, 1.0f - distance / 48.0f);
                    if (volume > 0.015f) {
                        const float pitch = 0.88f +
                            ((float)std::rand() / RAND_MAX) * 0.24f;
                        g_destructionBreakAudio[std::rand() %
                            g_destructionBreakAudio.size()].Play(volume, pitch);
                        g_destructionBreakAudioCooldown = 0.085f;
                    }
                }
            }
            const std::vector<DestructionCollisionSoundEvent> collisionSounds =
                g_destruction.DrainCollisionSoundEvents();
            if (g_destructionImpactAudioCooldown <= 0.0f &&
                !collisionSounds.empty()) {
                const DestructionCollisionSoundEvent* loudest = nullptr;
                float loudestVolume = 0.0f;
                for (const DestructionCollisionSoundEvent& hit : collisionSounds) {
                    const float dx = hit.position.x - scene.camera.Position.x;
                    const float dy = hit.position.y - scene.camera.Position.y;
                    const float dz = hit.position.z - scene.camera.Position.z;
                    const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
                    const float speed = (std::min)(1.0f,
                        (std::max)(0.0f, (hit.approachSpeed - 3.0f) / 10.0f));
                    const float volume = (0.14f + speed * 0.58f) *
                        (std::max)(0.0f, 1.0f - distance / 36.0f);
                    if (volume <= loudestVolume) continue;
                    loudest = &hit;
                    loudestVolume = volume;
                }
                if (loudest && loudestVolume > 0.015f) {
                    const float pitch = 0.86f +
                        ((float)std::rand() / RAND_MAX) * 0.28f;
                    g_destructionImpactAudio[std::rand() %
                        g_destructionImpactAudio.size()].Play(
                            loudestVolume, pitch);
                    g_destructionImpactAudioCooldown = 0.075f;
                }
            }
            for (const TinyDebrisParticle& tiny :
                 g_destruction.DrainTinyDebrisParticles()) {
                ImpactParticle particle;
                particle.position = tiny.position;
                particle.velocity = tiny.velocity;
                particle.maxLife = particle.life = 0.8f;
                particle.size = tiny.size;
                particle.growth = -tiny.size * 0.75f;
                particle.color = { 0.36f, 0.31f, 0.25f };
                particle.spark = true;
                scene.impactParticles.push_back(particle);
            }
            if (scene.impactParticles.size() > 800)
                scene.impactParticles.erase(scene.impactParticles.begin(),
                    scene.impactParticles.begin() +
                    (scene.impactParticles.size() - 800));
            UpdateHarpoonAttachments();
            for (auto& projectile : scene.projectiles) {
                const auto recordAccuracyHit = [&]() {
                    if (projectile.playerOwned &&
                        !projectile.accuracyHitRecorded &&
                        g_game.session.TimerRunning()) {
                        projectile.accuracyHitRecorded = true;
                        g_game.mission.RecordHit();
                    }
                };
                if (projectile.grenade && projectile.active && !projectile.held) {
                    if ((projectile.molotov || projectile.vortex ||
                         projectile.impactFuse) &&
                        std::find(grenadeContactEvents.begin(),
                                  grenadeContactEvents.end(),
                                  projectile.grenadePhysicsHandle) !=
                            grenadeContactEvents.end()) {
                        projectile.active = false;
                        projectile.detonate = true;
                    }
                    XMFLOAT3 impact;
                    XMFLOAT3 normal;
                    if (projectile.active && HitGrenadeCollision(
                            projectile, 0.11f, impact, normal,
                            projectile.grenadePhysicsHandle == 0)) {
                        if (projectile.molotov || projectile.vortex ||
                            projectile.impactFuse) {
                            projectile.position = impact;
                            projectile.active = false;
                            projectile.detonate = true;
                        } else if (projectile.grenadePhysicsHandle != 0) {
                            g_destruction.ResolveGrenadeBodyCollision(
                                projectile.grenadePhysicsHandle,
                                impact, normal);
                            DestructionBodyPose pose;
                            if (g_destruction.GetGrenadeBodyPose(
                                    projectile.grenadePhysicsHandle, pose)) {
                                projectile.position = pose.position;
                                projectile.rotation = pose.rotation;
                                projectile.velocity = pose.linearVelocity;
                            }
                        } else {
                            BounceGrenade(projectile, impact, normal);
                        }
                    }
                }
                if (projectile.rocket && projectile.active) {
                    XMFLOAT3 impact = projectile.position;
                    bool struck = false;
                    bool hostileTargetStruck = false;
                    const float radius = 0.22f;
                    size_t barrelIndex = 0;
                    XMFLOAT3 candidate;
                    if (g_banditLoaded) {
                        for (const auto& bandit : g_bandits) {
                            if (bandit && bandit->BlocksProjectile(
                                    projectile.previousPosition, projectile.position,
                                    radius)) {
                                struck = true;
                                hostileTargetStruck =
                                    bandit->faction == Faction::Bandit;
                                break;
                            }
                        }
                    }
                    if (!struck && HitHelicopterSegment(
                            projectile.previousPosition, projectile.position,
                            radius, candidate)) {
                        impact = candidate;
                        struck = true;
                        hostileTargetStruck = true;
                    }
                    if (!struck && HitSecondaryHelicopterSegment(
                            projectile.previousPosition, projectile.position,
                            radius, candidate)) {
                        impact = candidate;
                        struck = true;
                        hostileTargetStruck = true;
                    }
                    if (!struck && HitBoatSegment(
                            projectile.previousPosition, projectile.position,
                            radius, candidate)) {
                        impact = candidate;
                        struck = true;
                        hostileTargetStruck = true;
                        if (projectile.rocket)
                            DamageBoat(kRocketHelicopterDamage, candidate);
                    }
                    if (!struck && HitExplosiveBarrelSegment(
                            projectile.previousPosition, projectile.position,
                            radius, barrelIndex, candidate)) {
                        impact = candidate;
                        struck = true;
                    }
                    if (!struck && g_destruction.HitTestSegment(
                            projectile.previousPosition, projectile.position,
                            radius, candidate)) {
                        impact = candidate;
                        struck = true;
                    }
                    if (!struck && g_trees.BlocksSegment(
                            projectile.previousPosition, projectile.position, radius))
                        struck = true;
                    if (!struck && HitPrefabColliderSegment(
                            projectile.previousPosition, projectile.position,
                            radius, candidate)) {
                        impact = candidate;
                        struck = true;
                    }
                    if (!struck && HitTerrainSegment(
                            projectile.previousPosition, projectile.position,
                            radius, candidate)) {
                        impact = candidate;
                        struck = true;
                    }
                    if (struck) {
                        if (hostileTargetStruck) recordAccuracyHit();
                        projectile.position = impact;
                        projectile.active = false;
                        projectile.detonate = true;
                    }
                }
                if (projectile.grenade || projectile.rocket) {
                    // Grenades use fuse; rockets detonate on first solid impact.
                    if (projectile.detonate) {
                        const XMFLOAT3 center = projectile.position;
                        if (projectile.grenadePhysicsHandle != 0) {
                            g_destruction.DestroyGrenadeBody(
                                projectile.grenadePhysicsHandle);
                            projectile.grenadePhysicsHandle = 0;
                        }
                        const bool c4Blast = projectile.remoteCharge;
                        // A called-in strike is the same blast as a frag, just
                        // scaled up: one dial widens debris, enemy reach, FX
                        // and crater together so the explosion stays coherent.
                        // Every explosion is the default radius times its own
                        // factor: a called-in strike scales up, a thrown frag
                        // scales down. C4 keeps its authored absolute radius.
                        const float blastScale = projectile.missile
                            ? (std::max)(0.1f, scene.missileBlastScale)
                            : (std::max)(0.1f, scene.grenadeBlastScale);
                        const float blastRadius = c4Blast
                            ? 5.4f : scene.explosionBlastRadius * blastScale;
                        // Kill radius is its own tunable and is NOT scaled by
                        // the visual factor -- shrinking a grenade's burst must
                        // not quietly shrink what it kills.
                        const float enemyRadius = c4Blast
                            ? 8.5f : scene.grenadeEnemyRadius *
                                (projectile.missile ? blastScale : 1.0f);
                        const float enemyDamage = c4Blast
                            ? 650.0f : scene.grenadeEnemyDamage;
                        const float enemyPush = c4Blast
                            ? 14.0f : scene.grenadeEnemyPush;
                        const float blastDamage = c4Blast
                            ? 1000000.0f : scene.grenadeDamage;
                        // Structural fence health uses real weapon damage. A
                        // standard frag reaches its 500-health transition in
                        // one direct blast; legacy house fracture still uses
                        // the existing all-or-nothing explosion path.
                        const float destructionBlastDamage = c4Blast
                            ? blastDamage : enemyDamage;
                        const float blastImpulse = c4Blast
                            ? 230.0f : scene.grenadeImpulse;
                        if (projectile.molotov) {
                            scene.SpawnMolotovFire(center);
                            if (!g_emptyLevelMode) {
                                for (ExplosiveBarrel& barrel :
                                     scene.explosiveBarrels) {
                                    if (!barrel.active || barrel.burning) continue;
                                    const float dx = barrel.position.x - center.x;
                                    const float dy = barrel.position.y - center.y;
                                    const float dz = barrel.position.z - center.z;
                                    if (dx * dx + dy * dy + dz * dz > 2.4f * 2.4f)
                                        continue;
                                    barrel.burning = true;
                                    barrel.fuse = 3.0f;
                                    barrel.fireFxCooldown = 0.0f;
                                }
                            }
                            projectile.active = false;
                            projectile.detonate = false;
                            continue;
                        }
                        if (projectile.vortex) {
                            scene.SpawnVortexFX(center);
                            if (scene.explosionAudioCallback)
                                scene.explosionAudioCallback(
                                    center, scene.vortexRadius * 0.55f, true);
                            for (ExplosiveBarrel& barrel :
                                 scene.explosiveBarrels) {
                                if (!barrel.active || barrel.held) continue;
                                const float dx = barrel.position.x - center.x;
                                const float orbitCenterY = center.y +
                                    scene.vortexRadius * 0.35f;
                                const float dy = barrel.position.y - orbitCenterY;
                                const float dz = barrel.position.z - center.z;
                                const float capture = scene.vortexRadius * 1.35f;
                                if (dx * dx + dy * dy + dz * dz >
                                    capture * capture)
                                    continue;
                                if (EnsureExplosiveBarrelBody(barrel)) {
                                    barrel.thrown = true;
                                    barrel.vortexHoldTime = scene.vortexDuration;
                                    barrel.vortexCenter = {
                                        center.x, orbitCenterY, center.z };
                                }
                            }
                            if (scene.useDestruction &&
                                g_destruction.IsInitialized()) {
                                g_destruction.StartVortex(
                                    center, scene.vortexRadius,
                                    scene.vortexDuration);
                            }
                            if (!g_emptyLevelMode && g_trees.IsInitialized())
                                g_trees.StartVortex(
                                    center, scene.vortexRadius,
                                    scene.vortexDuration);
                            // Vortex is not a demolition charge: it tears
                            // ordinary props apart but leaves the tower alone.
                            DamagePrefabsInRadius(
                                center, scene.vortexRadius, 1000000.0f, false,
                                !projectile.hostile);
                            projectile.active = false;
                            projectile.detonate = false;
                            continue;
                        }
                        // The rocket digs the same hole as a frag. It already
                        // shares this whole blast block and the same blastScale,
                        // so it only ever missed the ground and wall cuts
                        // because this one gate named the grenade alone.
                        if (projectile.grenade || projectile.rocket) {
                            AddExplosionTerrainCrater(center, blastScale);
                            AddExplosionBuildingHole(center, blastScale);
                        }
                        {
                            ProfilerDX12::CpuScope blastFXProfile(
                                g_profiler, "Blast/FX");
                            scene.SpawnExplosionFX(
                                { center.x, center.y + 0.6f, center.z },
                                blastRadius * 1.6f, c4Blast ? 1.05f : 0.9f,
                                projectile.grenade);
                        }
                        if (!g_emptyLevelMode)
                        for (size_t i = 0; i < scene.explosiveBarrels.size(); ++i) {
                            const ExplosiveBarrel& barrel = scene.explosiveBarrels[i];
                            if (!barrel.active) continue;
                            const float dx = barrel.position.x - center.x;
                            const float dy = barrel.position.y - center.y;
                            const float dz = barrel.position.z - center.z;
                            if (dx*dx + dy*dy + dz*dz <=
                                blastRadius * blastRadius)
                                DetonateBarrel(i);
                        }
                        if (g_banditLoaded) {
                            for (auto& bandit : g_bandits) {
                                if (bandit) bandit->ApplyExplosion(
                                    center, enemyRadius,
                                    enemyDamage, enemyPush);
                            }
                            PlayBanditDeathEvents();
                        }
                        if (!g_helicopterDead && g_helicopterModel) {
                            const float dx = g_helicopterPosition.x - center.x;
                            const float dy = g_helicopterPosition.y - center.y;
                            const float dz = g_helicopterPosition.z - center.z;
                            const float distance = std::sqrt(dx*dx + dy*dy + dz*dz);
                            const float reach = enemyRadius + 5.0f;
                            if (distance < reach) {
                                const float falloff = 1.0f - distance / reach;
                                DamageHelicopter(
                                    projectile.rocket ? kRocketHelicopterDamage
                                                      : enemyDamage * falloff,
                                    g_helicopterPosition);
                            }
                        }
                        if (SecondaryHelicopterPresent() &&
                            !g_secondaryHelicopterDead && g_helicopterModel) {
                            const float dx = g_secondaryHelicopterPosition.x - center.x;
                            const float dy = g_secondaryHelicopterPosition.y - center.y;
                            const float dz = g_secondaryHelicopterPosition.z - center.z;
                            const float distance = std::sqrt(dx*dx + dy*dy + dz*dz);
                            const float reach = enemyRadius + 5.0f;
                            if (distance < reach) {
                                const float falloff = 1.0f - distance / reach;
                                DamageSecondaryHelicopter(
                                    projectile.rocket ? kRocketHelicopterDamage
                                                      : enemyDamage * falloff,
                                    g_secondaryHelicopterPosition);
                            }
                        }
                        if (!g_boatDead && g_boatModel) {
                            const float dx = g_boatPosition.x - center.x;
                            const float dy = g_boatPosition.y - center.y;
                            const float dz = g_boatPosition.z - center.z;
                            const float distance = std::sqrt(dx*dx + dy*dy + dz*dz);
                            const float reach = enemyRadius + 5.0f;
                            if (distance < reach) {
                                const float falloff = 1.0f - distance / reach;
                                DamageBoat(
                                    projectile.rocket ? kRocketHelicopterDamage
                                                      : enemyDamage * falloff,
                                    g_boatPosition);
                            }
                        }
                        // The AA emplacement is a blast target as well, so a
                        // planted charge is a valid answer to it rather than
                        // grinding its 900 HP down with rifle fire. Only the
                        // barrel chain reaction used to reach it; a charge stuck
                        // to the gun itself did nothing. C4 is a demolition and
                        // one is enough, while a frag still only chips the
                        // mount -- same split the comm tower uses below.
                        for (size_t ti = 0;
                             ti < g_game.vehicles.aaTurrets.size(); ++ti) {
                            if (!g_game.vehicles.aaTurrets[ti].Active()) continue;
                            const XMFLOAT3 turret =
                                g_game.vehicles.aaTurrets[ti].position;
                            const float tx = turret.x - center.x;
                            const float ty = turret.y - center.y;
                            const float tz = turret.z - center.z;
                            const float distance =
                                std::sqrt(tx * tx + ty * ty + tz * tz);
                            // Measured to the mount origin, so carry the mount
                            // height as slack: a charge stuck to the barrel
                            // sits ~1.85 m above the point we compare against.
                            const float reach = enemyRadius +
                                VehicleSystem::AATurretMountHeight;
                            if (distance < reach) {
                                const float falloff = 1.0f - distance / reach;
                                // A charge planted on the gun kills outright:
                                // full health with no falloff, since falloff
                                // measured from the mount origin would leave a
                                // charge stuck to the barrel ~18% short of the
                                // 900 HP it needs. Past that the blast still
                                // reaches, but only as scaled splash.
                                const bool planted =
                                    c4Blast && distance <
                                        VehicleSystem::AATurretBarrelLength;
                                const float damage = planted
                                    ? VehicleSystem::AATurretMaxHealth
                                    : (c4Blast
                                           ? VehicleSystem::AATurretMaxHealth
                                           : enemyDamage) * falloff;
                                DamageAATurret(ti, damage, turret);
                            }
                        }
                        // Grenades hurt the player too. Previously only enemies
                        // took blast damage, because every grenade in the game
                        // was thrown BY the player -- enemy grenades made the
                        // radius matter in both directions. Own grenades still
                        // count: standing on your own throw should cost you.
                        {
                            const float ex = scene.camera.Position.x - center.x;
                            const float ey = scene.camera.Position.y - center.y;
                            const float ez = scene.camera.Position.z - center.z;
                            const float playerRange =
                                std::sqrt(ex * ex + ey * ey + ez * ez);
                            const float reach = enemyRadius;
                            if (playerRange < reach) {
                                const float falloff = 1.0f - playerRange / reach;
                                const float blast =
                                    (c4Blast ? 100.0f
                                             : scene.grenadePlayerDamage) * falloff;
                                // Only an enemy throw answers to the difficulty
                                // multiplier. Standing on your own grenade is a
                                // mistake the player made, and scaling it would
                                // punish them for a dial about enemy lethality.
                                if (projectile.hostile)
                                    scene.DamagePlayerFromEnemyAt(blast, center);
                                else
                                    scene.DamagePlayer(blast);
                            }
                        }
                        // Run after Bandit damage. Newly killed enemies have
                        // ragdolls now, so same blast launches their limbs too.
                        g_destruction.ApplyExplosion(
                            center, blastRadius, destructionBlastDamage,
                            blastImpulse);
                        {
                            ProfilerDX12::CpuScope ragdollProfile(
                                g_profiler, "Blast/Ragdoll");
                            g_destruction.ApplyRagdollExplosion(
                                center, enemyRadius,
                                c4Blast ? 175.0f : scene.grenadeEnemyImpulse);
                        }
                        // Only a remote charge can fell the comm tower; frag
                        // grenades and rockets leave it standing.
                        {
                            ProfilerDX12::CpuScope prefabDamageProfile(
                                g_profiler, "Blast/PrefabDamage");
                            DamagePrefabsInRadius(
                                center, blastRadius, blastDamage, c4Blast,
                                !projectile.hostile);
                        }
                        // A charge stuck to the mast is a demolition, and one
                        // charge is enough. The radius pass above cannot do it:
                        // it measures to the entity origin, which for a 26 m
                        // tower is the base, so a charge planted anywhere up the
                        // mast falls outside blastRadius and is skipped. Reuse
                        // the mast-volume test that authorised the demolition in
                        // the first place -- if the charge rigged the tower, that
                        // same charge brings it down.
                        if (c4Blast) {
                            const uint64_t riggedTower = CommTowerEntityAt(center);
                            if (riggedTower != 0)
                                DamagePrefabEntity(riggedTower, blastDamage,
                                                   center, true);
                        }
                        // The aircraft needs the same volume treatment, and for
                        // the same reason: the radius pass measures to the
                        // origin, and this airframe reaches ~14.5 m from it
                        // against a 5.4 m C4 blast, so explosives that visibly
                        // went off against a wing did nothing at all.
                        //
                        // C4 destroys it outright, the way a planted charge
                        // fells a rigged tower. A rocket takes half its health,
                        // matching what one does to the helicopter -- so two
                        // rockets bring the plane down and it stays a target
                        // worth carrying a launcher for, rather than a one-shot.
                        // Frag grenades are deliberately left out: they carry
                        // per-bond damage meant for soft props and should not
                        // chip away at an objective airframe.
                        if (c4Blast || projectile.rocket) {
                            const uint64_t hitPlane =
                                ObjectivePlaneHitByExplosion(center, blastRadius);
                            if (hitPlane != 0) {
                                const float planeDamage = c4Blast
                                    ? 1000000.0f
                                    : ObjectivePlaneRocketDamage(hitPlane);
                                DamagePrefabEntity(hitPlane, planeDamage, center,
                                                   c4Blast, !projectile.hostile);
                            }
                        }
                        projectile.active = false;
                        projectile.detonate = false;
                    }
                    continue;
                }
                if (!projectile.active) continue;

                if (projectile.remoteCharge) {
                    constexpr float chargeRadius = 0.16f;
                    XMFLOAT3 chargeHit = projectile.position;
                    bool stuck = false;
                    uint64_t ignoredEntity = 0;
                    size_t ignoredBarrel = 0;
                    if (g_destruction.HitTestSegment(
                            projectile.previousPosition, projectile.position,
                            chargeRadius, chargeHit)) {
                        stuck = true;
                    } else if (HitPrefabColliderSegment(
                            projectile.previousPosition, projectile.position,
                            chargeRadius, chargeHit, &ignoredEntity)) {
                        stuck = true;
                    } else if (HitHelicopterSegment(
                            projectile.previousPosition, projectile.position,
                            chargeRadius, chargeHit) ||
                               HitSecondaryHelicopterSegment(
                            projectile.previousPosition, projectile.position,
                            chargeRadius, chargeHit) ||
                               HitBoatSegment(
                            projectile.previousPosition, projectile.position,
                            chargeRadius, chargeHit) ||
                               HitExplosiveBarrelSegment(
                            projectile.previousPosition, projectile.position,
                            chargeRadius, ignoredBarrel, chargeHit)) {
                        stuck = true;
                    } else if (!g_emptyLevelMode && g_trees.BlocksSegment(
                            projectile.previousPosition, projectile.position,
                            chargeRadius)) {
                        chargeHit = projectile.position;
                        stuck = true;
                    } else if (HitTerrainSegment(
                            projectile.previousPosition, projectile.position,
                            chargeRadius, chargeHit)) {
                        stuck = true;
                    } else if (!scene.useMeshTerrain &&
                               projectile.position.y <= scene.grenadeGroundY) {
                        chargeHit.y = scene.grenadeGroundY;
                        stuck = true;
                    }
                    if (stuck) {
                        const XMFLOAT3 normal{
                            -projectile.direction.x,
                            -projectile.direction.y,
                            -projectile.direction.z };
                        scene.StickRemoteCharge(chargeHit, normal);
                        projectile.active = false;
                    }
                    continue;
                }
                const auto stopProjectileAt = [&](const XMFLOAT3& impact,
                                                   bool destructibleHit = false) {
                    if (projectile.harpoon) {
                        projectile.position = impact;
                        g_destruction.PinHarpoonRagdolls(
                            projectile.harpoonId, impact, projectile.direction,
                            destructibleHit);
                        scene.PinHarpoon(
                            impact, projectile.direction, projectile.harpoonId);
                    }
                    projectile.active = false;
                };

                XMFLOAT3 hit;
                // Collision uses the bullet's own small radius so it must
                // actually reach the surface before it registers -- the wider
                // damage radius only governs how far the fracture spreads once
                // the bullet has struck. Otherwise the wall breaks at a distance.
                const float bulletRadius = projectile.flame
                    ? 0.24f
                    : std::max(0.12f, scene.projectileScale * 0.5f);
                // Water no longer stops a round: it only marks where the round
                // went in. Detected up front, outside the impact chain, so a
                // surface crossing never consumes the frame's collision test --
                // the shot still goes on to hit the seabed, a submerged target
                // or the player underneath. Harpoons are the one exception and
                // anchor at the entry point; the chain below handles that.
                XMFLOAT3 waterEntry{};
                bool waterEntryValid = false;
                bool oceanEntry = false;
                if (!g_emptyLevelMode) {
                    if (g_water.ShootSurface(projectile.previousPosition,
                                             projectile.position, waterEntry)) {
                        waterEntryValid = true;
                    } else if (g_ocean.ShootSurface(projectile.previousPosition,
                                                    projectile.position,
                                                    waterEntry, 0.28f)) {
                        waterEntryValid = true;
                        oceanEntry = true;
                    }
                    if (waterEntryValid) {
                        // ShootSurface already rings the surface sim itself, so
                        // no Splash() call here -- that would double the wave.
                        // This is the visible spray on top of it.
                        scene.SpawnWaterSplash(waterEntry, 1.0f);
                        if (oceanEntry &&
                            scene.waterQuality == WaterQuality::Ultra) {
                            WaterInteraction event;
                            event.worldXZ = {waterEntry.x, waterEntry.z};
                            event.radius = 0.42f;
                            event.heightImpulse = 0.065f;
                            event.velocityImpulse = {
                                projectile.direction.x * 0.08f,
                                projectile.direction.z * 0.08f};
                            event.type = WaterInteractionType::Splash;
                            waterRenderer.QueueInteraction(event);
                        }
                    }
                }

                struct BanditProjectileHit {
                    SkinnedEnemy* enemy = nullptr;
                    XMFLOAT3 position = {};
                    std::string bone;
                    bool killed = false;
                };
                std::vector<BanditProjectileHit> banditHits;
                // Player shots damage any enemy. Hostile shots damage marines
                // and the held human shield -- never other bandits.
                if (g_banditLoaded) {
                    for (auto& bandit : g_bandits) {
                        if (!bandit) continue;
                        if (projectile.hostile) {
                            // Bandit-fired shot. Marines are valid targets: they
                            // are who the bandits (and the Humvee gunner) are
                            // shooting at. Previously this whole loop was gated
                            // on g_heldBandit, so a hostile round that struck an
                            // ally fell through to the block-only pass below --
                            // it was consumed by the body and dealt no damage,
                            // which made marines look invulnerable.
                            //
                            // Fellow bandits still only block. They fire past
                            // each other constantly, and enabling that friendly
                            // fire would have them wipe out their own squad.
                            const bool shield = bandit.get() == g_heldBandit;
                            if (!shield && bandit->faction != Faction::Marine)
                                continue;
                        } else if (bandit->faction != Faction::Bandit) {
                            // Player- or marine-fired shot: never hits a marine
                            // (no friendly fire), only ever damages bandits.
                            continue;
                        }
                        XMFLOAT3 banditHit = projectile.position;
                        std::string banditBone;
                        const bool hitBandit = projectile.harpoon
                            ? bandit->HitByHarpoon(
                                projectile.previousPosition, projectile.position,
                                projectile.direction, bulletRadius, &banditHit,
                                &banditBone)
                            : bandit->Shoot(
                                projectile.previousPosition, projectile.position,
                                projectile.direction, bulletRadius, &banditHit,
                                nullptr, 20.0f * projectile.damageMultiplier,
                                true);
                        if (hitBandit) {
                            const bool killed = bandit->Dead();
                            banditHits.push_back({ bandit.get(), banditHit,
                                                  std::move(banditBone), killed });
                            if (killed && bandit.get() == g_heldBandit)
                                g_heldBandit = nullptr;
                            if (!projectile.harpoon) break;
                        }
                    }
                }
                if (!banditHits.empty()) {
                    recordAccuracyHit();
                    // Hit marker, red if this round put someone down. One
                    // projectile can strike several bodies (a harpoon skewers a
                    // line of them), so a kill anywhere in the group makes the
                    // whole flash lethal.
                    if (projectile.playerOwned) {
                        bool lethal = false;
                        for (const BanditProjectileHit& banditHit : banditHits)
                            lethal = lethal || banditHit.killed;
                        scene.TriggerHitMarker(lethal);
                    }
                    for (BanditProjectileHit& banditHit : banditHits) {
                        if (projectile.laser)
                            scene.StopLaserBeamAt(banditHit.position);
                        if ((projectile.laser || projectile.flame) &&
                            banditHit.enemy && !banditHit.enemy->Dead())
                            banditHit.enemy->Ignite(6.5f);
                        if (projectile.harpoon && banditHit.enemy &&
                            banditHit.enemy->Dead() &&
                            projectile.harpoonPiercedCount < 6) {
                            const float offset =
                                projectile.harpoonPiercedCount * 0.58f;
                            if (g_destruction.AttachRagdollToHarpoon(
                                    banditHit.enemy->RagdollId(),
                                    projectile.harpoonId,
                                    banditHit.position, offset,
                                    banditHit.bone)) {
                                ++projectile.harpoonPiercedCount;
                                // Give the carried ragdoll enough forward flight
                                // to reach and pin against geometry behind it.
                                projectile.lifetime = (std::max)(
                                    projectile.lifetime, 1.25f);
                            }
                        }
                        const XMFLOAT3 normal(
                            -projectile.direction.x, -projectile.direction.y,
                            -projectile.direction.z);
                        scene.SpawnBloodBurst(banditHit.position, normal);
                    }
                    const float hitPitch =
                        g_fleshHitPitchMin + ((float)std::rand() / RAND_MAX) *
                        (g_fleshHitPitchMax - g_fleshHitPitchMin);
                    g_hitAudio.Play(0.72f * 0.3f, hitPitch);
                    if (!banditHits.front().killed &&
                        g_banditPainCooldown <= 0.0f) {
                        const float painPitch =
                            0.96f + ((float)std::rand() / RAND_MAX) * 0.08f;
                        g_banditHitVoiceAudio.Play(
                            BanditVoiceVolume(
                                banditHits.front().position, 0.82f), painPitch);
                        g_banditPainCooldown = 0.45f;
                    }
                    PlayBanditDeathEvents();
                    if (projectile.harpoon) {
                        scene.ShowHarpoonTether(banditHits.back().position);
                        g_destruction.MoveHarpoonRagdolls(
                            projectile.harpoonId, projectile.position,
                            projectile.direction);
                        // Kill and penetrate every enemy in this flight segment.
                        // Next solid impact freezes all carried ragdolls there.
                        continue;
                    }
                    // Penetration: a round with budget left carries on through
                    // the body instead of dying in it, so a lined-up pair takes
                    // one bullet. Same mechanism the harpoon branch above uses
                    // -- continue the flight rather than deactivate -- just
                    // driven by the weapon's penetrationPower instead of being
                    // hardcoded to one weapon.
                    if (TryPenetrate(projectile, kPenetrationCostFlesh,
                                     kPenetrationFalloffFlesh)) {
                        // Deliberately no position nudge here. Bodies are
                        // resolved from a hit list already gathered for this
                        // segment, not re-tested, so the round cannot strike
                        // the same enemy twice on the way through.
                        continue;
                    }
                    projectile.active = false;
                    continue;
                }
                // Bodies that stop a hostile round without taking damage. Any
                // marine hit was already resolved above and took the projectile
                // with it, so what reaches here is a bandit shielding another
                // bandit: the round is absorbed, but no friendly fire.
                if (projectile.hostile && g_banditLoaded) {
                    bool blockedByBandit = false;
                    for (const auto& bandit : g_bandits) {
                        if (bandit && bandit->BlocksProjectile(
                                projectile.previousPosition, projectile.position,
                                bulletRadius)) {
                            blockedByBandit = true;
                            break;
                        }
                    }
                    if (blockedByBandit) {
                        projectile.active = false;
                        continue;
                    }
                }
                // Tested before the airframe: the rope hangs outboard of the
                // hull, so a shot reaching it should cut it rather than be
                // swallowed by the fuselage behind. Only a hostile round on an
                // occupied descent counts -- see NotifyBlackHawkRopeCut.
                XMFLOAT3 ropeHit;
                if (projectile.hostile &&
                    g_game.vehicles.blackHawkRope &&
                    g_game.vehicles.BlackHawkIsRappelling() &&
                    g_blackHawkRope.Shoot(
                        projectile.previousPosition, projectile.position,
                        projectile.direction, bulletRadius, 40.0f, ropeHit)) {
                    const XMFLOAT3 normal(-projectile.direction.x,
                                          -projectile.direction.y,
                                          -projectile.direction.z);
                    scene.SpawnBulletImpact(ropeHit, normal);
                    // Shoot only cuts when a link was the closest thing struck;
                    // a graze that merely shoved the chain leaves IsCut false.
                    if (g_blackHawkRope.IsCut())
                        HandleBlackHawkRopeCut();
                    stopProjectileAt(ropeHit);
                    continue;
                }
                XMFLOAT3 insertionVehicleHit;
                if (projectile.hostile &&
                    HitOccupiedInsertionBlackHawkSegment(
                        projectile.previousPosition, projectile.position,
                        bulletRadius, insertionVehicleHit)) {
                    const XMFLOAT3 normal(-projectile.direction.x,
                                          -projectile.direction.y,
                                          -projectile.direction.z);
                    scene.SpawnBulletImpact(insertionVehicleHit, normal);
                    // Rounds on the airframe: the player is inside it, so this
                    // is the loudest metal hit in the game.
                    PlayMetalHitAudio(insertionVehicleHit, 1.0f);
                    g_game.vehicles.DamageInsertionBlackHawkFromEnemyFire(
                        2.4f * projectile.damageMultiplier);
                    stopProjectileAt(insertionVehicleHit);
                    continue;
                }
                if (projectile.hostile &&
                    HitOccupiedInsertionBoatSegment(
                        projectile.previousPosition, projectile.position,
                        bulletRadius, insertionVehicleHit)) {
                    const XMFLOAT3 normal(-projectile.direction.x,
                                          -projectile.direction.y,
                                          -projectile.direction.z);
                    scene.SpawnBulletImpact(insertionVehicleHit, normal);
                    PlayMetalHitAudio(insertionVehicleHit, 1.0f);
                    g_game.vehicles.DamageInsertionBoatFromEnemyFire(
                        2.4f * projectile.damageMultiplier);
                    stopProjectileAt(insertionVehicleHit);
                    continue;
                }
                XMFLOAT3 helicopterHit;
                if (!projectile.hostile && HitHelicopterSegment(
                        projectile.previousPosition, projectile.position,
                        bulletRadius, helicopterHit)) {
                    recordAccuracyHit();
                    // Hardware hits get the plain marker. Never the lethal one:
                    // an airframe dies to accumulated damage rather than a
                    // single killing round, so red here would fire on whichever
                    // bullet happened to be last and read as arbitrary.
                    if (projectile.playerOwned) scene.TriggerHitMarker(false);
                    const XMFLOAT3 normal(-projectile.direction.x,
                                          -projectile.direction.y,
                                          -projectile.direction.z);
                    scene.SpawnBulletImpact(helicopterHit, normal);
                    // Player rounds on the gunship's hull -- same sheet metal.
                    PlayMetalHitAudio(helicopterHit, 0.85f);
                    DamageHelicopter(34.0f * projectile.damageMultiplier, helicopterHit);
                    if (projectile.laser) scene.StopLaserBeamAt(helicopterHit);
                    if (projectile.harpoon) scene.ShowHarpoonTether(helicopterHit);
                    stopProjectileAt(helicopterHit);
                    continue;
                }
                if (!projectile.hostile && HitSecondaryHelicopterSegment(
                        projectile.previousPosition, projectile.position,
                        bulletRadius, helicopterHit)) {
                    recordAccuracyHit();
                    if (projectile.playerOwned) scene.TriggerHitMarker(false);
                    const XMFLOAT3 normal(-projectile.direction.x,
                                          -projectile.direction.y,
                                          -projectile.direction.z);
                    scene.SpawnBulletImpact(helicopterHit, normal);
                    PlayMetalHitAudio(helicopterHit, 0.85f);
                    DamageSecondaryHelicopter(
                        34.0f * projectile.damageMultiplier, helicopterHit);
                    if (projectile.laser) scene.StopLaserBeamAt(helicopterHit);
                    if (projectile.harpoon) scene.ShowHarpoonTether(helicopterHit);
                    stopProjectileAt(helicopterHit);
                    continue;
                }
                XMFLOAT3 turretHit;
                size_t shotTurretIndex = 0;
                if (!projectile.hostile && HitAATurretSegment(
                        projectile.previousPosition, projectile.position,
                        bulletRadius, turretHit, shotTurretIndex)) {
                    recordAccuracyHit();
                    if (projectile.playerOwned) scene.TriggerHitMarker(false);
                    const XMFLOAT3 normal(-projectile.direction.x,
                                          -projectile.direction.y,
                                          -projectile.direction.z);
                    scene.SpawnBulletImpact(turretHit, normal);
                    // Armoured mount: rings like the comm tower's lattice.
                    PlayMetalHitAudio(turretHit, 0.9f);
                    DamageAATurret(shotTurretIndex,
                                   34.0f * projectile.damageMultiplier, turretHit);
                    if (projectile.laser) scene.StopLaserBeamAt(turretHit);
                    if (projectile.harpoon) scene.ShowHarpoonTether(turretHit);
                    stopProjectileAt(turretHit);
                    continue;
                }
                XMFLOAT3 boatHit;
                if (!projectile.hostile && HitBoatSegment(
                        projectile.previousPosition, projectile.position,
                        bulletRadius, boatHit)) {
                    recordAccuracyHit();
                    if (projectile.playerOwned) scene.TriggerHitMarker(false);
                    const XMFLOAT3 normal(-projectile.direction.x,
                                          -projectile.direction.y,
                                          -projectile.direction.z);
                    scene.SpawnBulletImpact(boatHit, normal);
                    PlayMetalHitAudio(boatHit, 0.85f);
                    DamageBoat(34.0f * projectile.damageMultiplier, boatHit);
                    if (projectile.laser) scene.StopLaserBeamAt(boatHit);
                    if (projectile.harpoon) scene.ShowHarpoonTether(boatHit);
                    stopProjectileAt(boatHit);
                    continue;
                }
                size_t barrelIndex = 0;
                XMFLOAT3 barrelHit;
                if (HitExplosiveBarrelSegment(
                        projectile.previousPosition, projectile.position,
                        bulletRadius, barrelIndex, barrelHit)) {
                    ExplosiveBarrel& barrel = scene.explosiveBarrels[barrelIndex];
                    const bool carryingRagdolls = projectile.harpoon &&
                        projectile.harpoonPiercedCount > 0;
                    if (!carryingRagdolls)
                        barrel.hits += static_cast<int>(
                            std::ceil(projectile.damageMultiplier));
                    const XMFLOAT3 normal(-projectile.direction.x,
                                          -projectile.direction.y,
                                          -projectile.direction.z);
                    scene.SpawnBulletImpact(barrelHit, normal);
                    if (projectile.laser) scene.StopLaserBeamAt(barrelHit);
                    if (projectile.harpoon && !carryingRagdolls) {
                        scene.ShowHarpoonTether(barrelHit);
                        XMVECTOR pull = XMLoadFloat3(&scene.camera.Position) -
                            XMLoadFloat3(&barrel.position);
                        if (XMVectorGetX(XMVector3LengthSq(pull)) > 1e-5f)
                            pull = XMVector3Normalize(
                                pull + XMVectorSet(0.0f, 0.12f, 0.0f, 0.0f));
                        XMFLOAT3 velocity;
                        XMStoreFloat3(&velocity, pull * 18.0f);
                        // Preserve barrel rule: Box3D bodies exist only while a
                        // vortex owns them. Harpoon flight uses manual velocity.
                        if (barrel.vortexHoldTime > 0.0f &&
                            barrel.physicsHandle != 0) {
                            g_destruction.SetExplosiveBarrelVelocity(
                                barrel.physicsHandle, velocity);
                        } else {
                            DestroyExplosiveBarrelBody(barrel);
                            barrel.velocity = velocity;
                        }
                        barrel.held = false;
                        barrel.thrown = true;
                        if (g_heldBarrelIndex == barrelIndex)
                            g_heldBarrelIndex = SIZE_MAX;
                    }
                    if (projectile.flame && !barrel.burning) {
                        barrel.burning = true;
                        barrel.fuse = 3.0f;
                        barrel.fireFxCooldown = 0.0f;
                    }
                    if (barrel.hits >= 4) {
                        DetonateBarrel(barrelIndex);
                    } else if (barrel.hits == 2 && !barrel.burning) {
                        barrel.burning = true;
                        barrel.fuse = 3.0f;
                        barrel.fireFxCooldown = 0.0f;
                    }
                    stopProjectileAt(barrelHit);
                    continue;
                }
                // Seeded with the negated shot direction, which is what every
                // prefab impact used before triangle collision existed. A mesh
                // hit replaces it with the real surface normal, so decals lie
                // flat on an angled hangar wall instead of facing the shooter.
                XMFLOAT3 normal(-projectile.direction.x,
                                -projectile.direction.y,
                                -projectile.direction.z);
                // A taxiing or airborne aircraft first, because its authored
                // collider stayed on the apron when it rolled: without this the
                // round tests against empty ground and flies through the plane.
                // Checked ahead of the static colliders so the airframe wins
                // over whatever it happens to be flying above.
                {
                    XMFLOAT3 planeHit;
                    const uint64_t flyingPlane = HitObjectivePlaneSegment(
                        projectile.previousPosition, projectile.position,
                        bulletRadius, planeHit);
                    if (flyingPlane != 0) {
                        projectile.position = planeHit;
                        scene.SpawnBulletImpact(planeHit, normal);
                        if (projectile.laser) scene.StopLaserBeamAt(planeHit);
                        if (projectile.harpoon) scene.ShowHarpoonTether(planeHit);
                        // Fire does not stick to it: the aircraft is a player
                        // objective and IgniteMaterial is filtered against it in
                        // the burning-material tick anyway.
                        if (!projectile.harpoon ||
                            projectile.harpoonPiercedCount == 0)
                            DamagePrefabEntity(flyingPlane,
                                34.0f * projectile.damageMultiplier, planeHit,
                                projectile.remoteCharge, projectile.playerOwned);
                        stopProjectileAt(planeHit);
                        continue;
                    }
                }
                uint64_t prefabEntityId = 0;
                if (HitPrefabColliderSegment(projectile.previousPosition,
                        projectile.position, bulletRadius, hit,
                        &prefabEntityId, &normal)) {
                    projectile.position = hit;
                    scene.SpawnBulletImpact(hit, normal);
                    if (projectile.laser) scene.StopLaserBeamAt(hit);
                    if (projectile.harpoon) scene.ShowHarpoonTether(hit);
                    if (projectile.laser || projectile.flame) {
                        // Steel lattice does not burn, and fire deals it no
                        // damage, so skip the flames rather than painting a
                        // fire on a mast that will never lose health to it.
                        // The direct hit below still damages it.
                        XMFLOAT3 ignoredTowerBase{};
                        if (!FindCommTower(prefabEntityId, ignoredTowerBase))
                            scene.IgniteMaterial(prefabEntityId, hit, 1.65f);
                    }
                    if (!projectile.harpoon ||
                        projectile.harpoonPiercedCount == 0)
                        DamagePrefabEntity(prefabEntityId,
                            34.0f * projectile.damageMultiplier, hit,
                            projectile.remoteCharge, projectile.playerOwned);
                    stopProjectileAt(hit);
                    continue;
                }
                XMFLOAT3 projectileForceDirection = projectile.direction;
                if (projectile.harpoon) {
                    XMVECTOR pull = XMLoadFloat3(&scene.camera.Position) -
                        XMLoadFloat3(&projectile.position);
                    if (XMVectorGetX(XMVector3LengthSq(pull)) > 1e-5f)
                        XMStoreFloat3(&projectileForceDirection,
                                      XMVector3Normalize(pull));
                }
                if (g_destruction.HitTestSegment(projectile.previousPosition, projectile.position,
                                                 bulletRadius, hit,
                                                 projectile.harpoon
                                                     ? projectile.harpoonId : 0)) {
                    std::cout << "Projectile hit wall at " << hit.x << ", "
                              << hit.y << ", " << hit.z << "\n";
                    // Sampled here, before any damage below can tear the sheet
                    // off and leave the cached hit chunk pointing elsewhere.
                    const bool metalSheetHit = g_destruction.IsMetalSheetAt(hit);
                    // Objective geometry does not chip, and only a remote charge
                    // damages it at all. Rounds ring off the lattice and stop
                    // there -- the mast comes apart in one piece when the charge
                    // it is rigged with goes off, not from small-arms fire.
                    const bool protectedHit =
                        g_destruction.IsProtectedChunkAt(hit);
                    const uint64_t protectedOwner =
                        protectedHit ? CommTowerEntityAt(hit) : 0;
                    if (protectedHit) {
                        if (protectedOwner != 0 && projectile.remoteCharge &&
                            (!projectile.harpoon ||
                             projectile.harpoonPiercedCount == 0)) {
                            DamagePrefabEntity(
                                protectedOwner,
                                34.0f * projectile.damageMultiplier, hit,
                                /*fromRemoteCharge=*/true);
                        }
                        // A round that is not a charge still rings off the steel.
                        if (!projectile.remoteCharge)
                            PlayMetalHitAudio(hit, 0.9f);
                    } else if (projectile.flame) {
                        g_destruction.IgniteChunkAt(hit);
                    }
                    if (protectedHit) {
                        // No chunk chipping, no impulse: the lattice stands rigid
                        // until its health is gone.
                    } else if (projectile.laser) {
                        g_destruction.DestroyChunkAt(
                            hit, scene.destructionDamageRadius);
                    } else if (!projectile.harpoon ||
                               projectile.harpoonPiercedCount == 0) {
                        g_destruction.ApplyRadialDamage(
                            hit, scene.destructionDamageRadius,
                            projectile.harpoon ? 2.5f :
                            34.0f * projectile.damageMultiplier);
                    }
                    if (protectedHit) {
                        // Nothing to pull or shove: see above.
                    } else if (projectile.harpoon &&
                        projectile.harpoonPiercedCount == 0) {
                        scene.ShowHarpoonTether(hit);
                        g_destruction.ApplyHarpoonPull(
                            hit, scene.camera.Position, 105.0f, 1.15f);
                    } else if (!projectile.harpoon) {
                        g_destruction.ApplyImpulse(hit, projectile.direction,
                                                   scene.destructionBulletImpulse *
                                                       projectile.damageMultiplier,
                                                   scene.destructionDamageRadius);
                    }
                    // Impact FX: spark burst + hole decal. Surface normal is
                    // approximated as facing back along the bullet's travel.
                    const XMFLOAT3 normal(-projectile.direction.x,
                                          -projectile.direction.y,
                                          -projectile.direction.z);
                    // Corrugated iron rings when it is struck, unlike the wood
                    // and rock that share this path. Queried before the damage
                    // above can tear the sheet loose and invalidate the chunk.
                    // Protected hits already rang inside DamagePrefabEntity, so
                    // only the roof-sheet case needs the sound here.
                    if (metalSheetHit) PlayMetalHitAudio(hit, 0.9f);
                    // One small dust puff right at the hit; the bigger cloud
                    // comes from the actual fracture (DrainBreakPoints).
                    scene.SpawnSmokeBurst(hit, 0.3f, 0.1f);
                    // The mark the puff used to stand in for. Small and varied
                    // so a burst into one wall does not read as a row of
                    // identical discs.
                    SpawnImpactDecal(hit, normal,
                                     0.05f + RandomUnit() * 0.035f,
                                     bulletRadius,
                                     /*attachToDestructible=*/true);
                    // A wisp curling out of the fresh hole. Separate from the
                    // dust puff above: that fires once at the impact, this
                    // keeps the mark alive for a beat afterwards.
                    scene.SpawnBulletHoleSmoke(hit, normal, 1.0f);
                    if (projectile.laser) {
                        scene.StopLaserBeamAt(hit);
                        scene.SpawnSmokeBurst(hit, 0.46f, 0.22f);
                    } else if (projectile.flame) {
                        scene.SpawnSmokeBurst(hit, 0.22f, 0.10f);
                    }
                    // Penetration, but only through thin corrugated sheeting.
                    // Wood, rock and the rest of this path are solid, and
                    // protected chunks are objective geometry that must never
                    // become shoot-through -- the mast has to come down to the
                    // charge rigged on it, not to rifle fire drilling past.
                    //
                    // The damage above has already landed, so a penetrating
                    // round both chips the sheet and reaches what is behind it.
                    if (metalSheetHit && !protectedHit &&
                        TryPenetrate(projectile, kPenetrationCostSheet,
                                     kPenetrationFalloffSheet)) {
                        // Step just past the surface before flying on. Without
                        // this the round resumes from the impact point, the
                        // next segment test resolves the same chunk again, and
                        // one sheet eats the entire budget in a few frames.
                        const XMVECTOR through =
                            XMLoadFloat3(&hit) +
                            XMLoadFloat3(&projectile.direction) *
                                (bulletRadius + 0.05f);
                        XMStoreFloat3(&projectile.position, through);
                        projectile.previousPosition = projectile.position;
                        continue;
                    }
                    stopProjectileAt(hit, true);
                } else if (!g_emptyLevelMode && g_water.ShootFloaters(projectile.previousPosition,
                                                 projectile.position,
                                                 projectileForceDirection,
                                                 bulletRadius,
                                                 projectile.harpoon ? 85.0f :
                                                 scene.destructionBulletImpulse)) {
                    // Bullet knocked a crate floating in the pool.
                    const XMFLOAT3 normal(-projectile.direction.x,
                                          -projectile.direction.y,
                                          -projectile.direction.z);
                    scene.SpawnBulletImpact(projectile.position, normal);
                    if (projectile.harpoon)
                        scene.ShowHarpoonTether(projectile.position);
                    stopProjectileAt(projectile.position);
                } else if (XMFLOAT3 treeHit;
                           !g_emptyLevelMode && g_trees.Shoot(projectile.previousPosition, projectile.position,
                                         projectileForceDirection,
                                         bulletRadius,
                                         projectile.harpoon &&
                                             projectile.harpoonPiercedCount > 0
                                             ? 0.0f
                                             : scene.treeDamagePerShot *
                                               projectile.damageMultiplier,
                                         treeHit)) {
                    // Chewed a palm trunk; enough rounds and it snaps and topples.
                    const XMFLOAT3 normal(-projectile.direction.x,
                                          -projectile.direction.y,
                                          -projectile.direction.z);
                    scene.SpawnBulletImpact(treeHit, normal);
                    scene.SpawnSmokeBurst(treeHit, 0.25f, 0.1f);
                    if (projectile.laser) scene.StopLaserBeamAt(treeHit);
                    if (projectile.harpoon) scene.ShowHarpoonTether(treeHit);
                    if (projectile.laser || projectile.flame) {
                        g_trees.IgniteNear(treeHit, 0.85f);
                    }
                    stopProjectileAt(treeHit);
                } else if (waterEntryValid && projectile.harpoon) {
                    // Tether weapons still anchor at the surface: a harpoon
                    // line has to start somewhere the rope can attach.
                    projectile.position = waterEntry;
                    scene.ShowHarpoonTether(waterEntry);
                    stopProjectileAt(waterEntry);
                } else if (XMFLOAT3 terrainHit;
                           HitTerrainSegment(projectile.previousPosition,
                                             projectile.position,
                                             bulletRadius, terrainHit)) {
                    projectile.position = terrainHit;
                    // Dirt kicks up where a round lands. Same small burst the
                    // tree branch uses, so ground hits read at the same weight
                    // as every other world impact instead of landing silently.
                    // Player fire only: incoming rounds already telegraph
                    // themselves, and smoking every one of them would fog the
                    // ground during a firefight and cost fill for nothing.
                    if (!projectile.hostile) {
                        scene.SpawnSmokeBurst(terrainHit, 0.25f, 0.1f);
                    }
                    if (projectile.laser) scene.StopLaserBeamAt(terrainHit);
                    if (projectile.harpoon) scene.ShowHarpoonTether(terrainHit);
                    if (projectile.flame) scene.SpawnCarriedFire(terrainHit);
                    stopProjectileAt(terrainHit);
                } else if (projectile.hostile) {
                    // Player is tested last. Any world or character mesh in
                    // front consumes the shot first, preventing wall penetration.
                    if (scene.HitPlayerProjectile(projectile) &&
                        g_playerPainCooldown <= 0.0f) {
                        // Rounds landing on the player were silent: enemies grunt
                        // when hit and the player did not, so incoming fire read
                        // as weightless. Same flesh sample the enemies use,
                        // pitched down and played loud -- it is your own body, so
                        // it is the closest sound in the mix.
                        const float painPitch =
                            0.78f + ((float)std::rand() / RAND_MAX) * 0.10f;
                        g_hitAudio.Play(0.95f, painPitch);
                        // Short lockout so a burst or a shotgun spread does not
                        // stack into a single clipped roar.
                        g_playerPainCooldown = 0.18f;
                    }
                }
            }
        }
        // Exfil. Reaching the boat ends the run, but only once the mission is
        // actually done -- otherwise the boat would be a way to skip the
        // objective it was meant to make harder. Levels with no tower authored
        // have nothing to gate on, so there the boat is a straight way out.
        if (g_game.session.Screen() == GameScreen::Level1 &&
            !g_emptyLevelMode && g_game.session.TimerRunning() &&
            scene.player.health > 0.0f) {
            const bool objectiveMet =
                g_game.mission.Stats().commTowersTotal == 0 ||
                g_game.mission.CommTowerObjectiveComplete();
            // Guarantee an exfil exists the moment the run becomes winnable --
            // but ONLY on a level that authored no aircraft.
            //
            // Where there is an aircraft, resolving it is what calls the boat
            // in (OnObjectivePlaneResolved), and the exfil must not exist before
            // then: that is the whole point of tying the way out to the run's
            // objective. A level with no aircraft has nothing to tie it to and
            // the boat is the ONLY way to end a mission, so without this such a
            // map would strand the player on a cleared island with no finish.
            //
            // Rolled here rather than at level load so a planeless map's exfil
            // is as unpredictable as an aircraft-summoned one.
            // PlaceEscapeBoatOnBearing is idempotent, so the roll only takes
            // effect on the first frame this fires -- every later frame is a
            // no-op and the boat stays put.
            const bool levelHasAircraft =
                g_game.mission.Stats().objectivePlanesTotal > 0;
            if (objectiveMet && !levelHasAircraft &&
                !g_game.vehicles.EscapeBoatReady())
                g_game.vehicles.PlaceEscapeBoatOnBearing(
                    RandomUnit() * XM_2PI, 0.0f, CurrentEscapeBoatDistance());
            g_game.vehicles.UpdateEscapeBoat(deltaTime);
            if (objectiveMet && g_game.vehicles.EscapeBoatReady() &&
                g_game.vehicles.PlayerCanBoardEscapeBoat(scene.camera.Position)) {
                SGE_LOG("LogGameplay", EngineLog::Level::Display,
                    "Player reached the escape boat -- exfil complete");
                OpenWinScreen();
            }
        }

        // Clearing the field no longer ends the run on its own. The boat is the
        // only way out: killing every bandit and downing the helicopter leaves
        // the player standing on a quiet island that still has to be left. That
        // makes the exfil run the last act of every mission rather than an
        // optional flourish, and it is why the escape boat is spawned with the
        // first reinforcement wave instead of at the end.
        }
        }

        // Sky environment swap. Must land here, before BeginFrame opens the
        // frame's command list: the swap drains every frame slot, then records
        // the replacement texture upload on its private list. Done with a list
        // already open it reset the allocator out from under the frame and then
        // waited on a fence that frame would never signal -- an instant hang on
        // picking Night.
        //
        // Held for a second after the preset's lighting has been applied, so the
        // scene visibly goes dark first and the swap's stall lands on an already
        // night-lit frame rather than freezing on the old daylight one.
        if (g_skyEnvironmentSwapPending) {
            // Clamped: a load-sized frame would otherwise burn the whole delay
            // in one step and put the stall right back where it started.
            g_skyEnvironmentSwapDelay -= (std::min)(deltaTime, 0.05f);
            if (g_skyEnvironmentSwapDelay <= 0.0f) {
                g_skyEnvironmentSwapPending = false;
                g_skyEnvironmentSwapDelay = 0.0f;
                // The swap releases the old sky texture, which frames still in
                // flight may be sampling. Drain here rather than inside the swap:
                // this call sits in the frame
                // loop, so the frame that follows re-syncs the fence through its
                // own MoveToNextFrame() -- the precondition WaitForGPUAllFrames
                // silently requires.
                WaitForGPUAllFrames();
                ApplyTimeOfDaySkyEnvironment(g_selectedTimeOfDay);
            }
        }

        // Toggling half-resolution AO resizes the trace target, which releases
        // a texture that frames still in flight may be sampling. Drain here for
        // the same reason as the sky swap above: this sits in the frame loop,
        // so the frame that follows re-syncs the fence through its own
        // MoveToNextFrame() -- the precondition WaitForGPUAllFrames requires.
        if (screenSpaceAO.TraceResolutionChangePending()) {
            WaitForGPUAllFrames();
            screenSpaceAO.ApplyTraceResolutionChange();
        }

        // Terrain, sky and IBL for a level start requested from the menu. Must
        // land here, before BeginFrame opens the frame's command list, for the
        // same reason as the two blocks above: the build resets that list and
        // its allocator. It runs before the loading screen's first stage, so
        // nothing samples these while they are still missing.
        if (g_sceneRenderAssetsPending) EnsureSceneRenderAssets();

        // ?? begin frame ??
        try { BeginFrame(); }
        catch (const std::exception& e) {
            std::ofstream("engine_runtime_error.log", std::ios::app)
                << "BeginFrame: " << e.what() << '\n';
            std::cerr << "BeginFrame: " << e.what() << "\n"; break;
        }
        bindlessHeap.BeginFrame(g_dx12.frameIndex);
        if (IsSceneScreen() && g_prefabRebuildRequested) {
            // Rebuilding prefab batches recreates model/GPU resources. If the
            // previous frame's command list still references the old ones, the
            // driver dereferences freed memory (access violation deep in the
            // D3D12/driver DLL) - exactly the crash seen when duplicating a rock.
            // Idle the GPU first so nothing in flight points at what we replace.
            {
                ProfilerDX12::CpuScope rebuildWaitProfile(
                    g_profiler, "PrefabRebuild/WaitGPU");
                WaitForGPU();
            }
            SGE_LOG("LogPrefab", EngineLog::Level::Display,
                std::string("Prefab rebuild running, requested by: ") +
                    (g_prefabRebuildReason ? g_prefabRebuildReason
                                           : "unknown"));
            {
                ProfilerDX12::CpuScope rebuildProfile(
                    g_profiler, "PrefabRebuild/Batches");
                RebuildPrefabRenderBatches();
            }
            if (g_ddgiCornellTestMode) {
                g_dxrDDGI.MarkLayoutDirty();
                RebuildDXRDDGIProbeLayout(true);
            }
        } else if (IsEditorEditing() && !g_game.loading.Active() &&
                   g_editorVisualRefreshRequested) {
            // Use the same lightweight whole-viewport sync as an ordinary
            // structural edit. This refreshes built-in editor visuals such as
            // houses, barrels and vehicles as well as loading missing prefabs.
            SynchronizeEditorRuntimeVisual(true);
        }
        // Editor buttons are processed here, before any scene pass binds the
        // old probe atlases. Rebuilding from the late ImGui phase destroyed
        // resources still referenced by the open frame command list.
        if (IsSceneScreen() &&
            g_game.commands.Consume(GameCommand::RebuildDDGI)) {
            if (g_game.session.Screen() == GameScreen::LevelEditor) {
                g_game.world.ReplaceLevel(
                    g_levelEditor.Level(), RuntimeTerrainSource::Preserve);
                scene.useDDGI = g_game.world.Level().dxrDDGI.enabled &&
                    g_dxrDDGI.GetStatus().dxrSupported;
                scene.probeSpacing =
                    g_game.world.Level().dxrDDGI.surfaceSpacing;
            } else {
                g_game.world.Level().dxrDDGI.enabled = scene.useDDGI;
                g_game.world.Level().dxrDDGI.surfaceSpacing =
                    std::clamp(scene.probeSpacing, 0.25f, 50.0f);
                g_game.world.Level().dxrDDGI.maxRayDistance =
                    std::clamp(scene.giMaxDistance, 1.0f, 200.0f);
            }
            g_dxrDDGI.ApplySettings(g_game.world.Level().dxrDDGI);
            if (g_game.world.Level().dxrDDGI.enabled &&
                g_dxrDDGI.GetStatus().dxrSupported) {
                g_dxrDDGI.MarkLayoutDirty();
                if (!RebuildDXRDDGIProbeLayout(true)) {
                    g_game.world.Level().dxrDDGI.enabled = false;
                    scene.useDDGI = false;
                    g_dxrDDGI.ApplySettings(g_game.world.Level().dxrDDGI);
                }
            } else {
                scene.useDDGI = false;
                g_dxrDDGI.ResetHistory();
            }
            if (g_game.session.Screen() == GameScreen::LevelEditor)
                g_levelEditor.MarkDXRDDGIRuntimeSynchronized();
        }
        if (g_game.commands.Consume(GameCommand::ResetDDGIHistory)) {
            g_dxrDDGI.ResetHistory();
        }
        if (g_game.session.Screen() == GameScreen::LevelEditor &&
            g_levelEditor.DXRDDGIRuntimeDirty()) {
            if (g_levelEditor.DXRDDGILayoutDirty())
                g_dxrDDGI.MarkLayoutDirty();
            RebuildDXRDDGIProbeLayout(false);
            g_levelEditor.MarkDXRDDGIRuntimeSynchronized();
        }
        static uint32_t dxrDDGIFrame = 0;
        if (g_game.world.Level().dxrDDGI.enabled) {
            if (std::abs(g_game.world.Level().dxrDDGI.maxRayDistance -
                         scene.giMaxDistance) > 0.001f) {
                g_game.world.Level().dxrDDGI.maxRayDistance =
                    std::clamp(scene.giMaxDistance, 1.0f, 200.0f);
                g_dxrDDGI.ApplySettings(g_game.world.Level().dxrDDGI);
            }
            if (g_dxrDDGI.HistoryDirty()) g_dxrDDGI.ResetHistory();
            ComPtr<ID3D12GraphicsCommandList4> dxrDDGICommands;
            if (SUCCEEDED(g_dx12.commandList.As(&dxrDDGICommands))) {
                const XMFLOAT3 raySunDirection(
                    -scene.lightPos.x, -scene.lightPos.y, -scene.lightPos.z);
                g_dxrDDGI.UpdateProbes(
                    dxrDDGICommands.Get(), ++dxrDDGIFrame,
                    raySunDirection, scene.lightColor,
                    g_ddgiCornellTestMode
                        ? 0.0f : scene.directionalLightIntensity,
                    g_ddgiCornellTestMode ? 0.0f : 1.0f,
                    g_ddgiCornellTestMode
                        ? g_ddgiCornellLightPosition : XMFLOAT3{},
                    g_ddgiCornellTestMode
                        ? g_ddgiCornellLightColor : XMFLOAT3{},
                    g_ddgiCornellTestMode
                        ? g_ddgiCornellLightRadius : 0.0f,
                    g_ddgiCornellTestMode
                        ? g_ddgiCornellLightIntensity : 0.0f);
            }
            g_ddgiIrradianceResource = g_dxrDDGI.IrradianceAtlas();
            g_ddgiVisibilityResource = g_dxrDDGI.VisibilityAtlas();
            visBuffer.UpdateDDGIResources(
                g_ddgiIrradianceResource, g_ddgiVisibilityResource);
            if (dxrDDGIFrame == 30 &&
                GetEnvironmentVariableA(
                    "SGE_DXR_DDGI_TEST", nullptr, 0) > 0) {
                g_game.commands.Request(GameCommand::RebuildDDGI);
                SGE_LOG("LogRenderer", EngineLog::Level::Display,
                    "DXR DDGI smoke rebuild queued");
            }
        }
        if (g_prefabRuntimeSmokeEnabled && !g_prefabRuntimeSmokeChecked &&
            !g_prefabRebuildRequested) {
            const auto batch = std::find_if(g_prefabRenderBatches.begin(),
                g_prefabRenderBatches.end(), [](const PrefabRenderBatch& value) {
                    return value.prefabId == "rock" && value.castShadow &&
                           !value.transforms.empty();
                });
            const auto collider = std::find_if(g_prefabColliders.begin(),
                g_prefabColliders.end(), [](const PrefabCollider& value) {
                    return value.entityId == 9000001;
                });
            bool collisionHit = false;
            if (collider != g_prefabColliders.end()) {
                const XMFLOAT3 start(collider->center.x -
                    collider->halfExtents.x * 2.0f, collider->center.y,
                    collider->center.z);
                const XMFLOAT3 end(collider->center.x +
                    collider->halfExtents.x * 2.0f, collider->center.y,
                    collider->center.z);
                collisionHit = PrefabColliderIntersectsSegment(
                    *collider, start, end, 0.0f);
            }
            const bool passed = batch != g_prefabRenderBatches.end() &&
                collider != g_prefabColliders.end() && collisionHit;
            SGE_LOG("LogPrefab", passed ? EngineLog::Level::Display
                                         : EngineLog::Level::Error,
                passed ? "Prefab runtime smoke passed: rendered, shadowed, collidable"
                       : "Prefab runtime smoke failed");
            g_prefabRuntimeSmokeChecked = true;
        }
        if (IsSceneScreen()) UpdatePrefabAudio();
        // Before UpdatePrefabLods: LOD selection reads the draw transform, so
        // the aircraft has to be moved for this frame first or it would pick a
        // level of detail for where it was rather than where it is.
        if (IsSceneScreen()) UpdateObjectivePlanes(deltaTime);
        // Same ordering reason as the aircraft above: a shoved container has
        // to reach its new pose before LOD selection reads the transform.
        if (IsSceneScreen()) UpdatePrefabRigidBodies();
        if (IsSceneScreen()) UpdatePrefabLods();
        occlusionDepth.FinalizeCapture(g_dx12.commandList.Get());
        // Adaptive Forward Extensions quality. Driven from the delayed GPU
        // timestamp resolved for an earlier frame, which is the only measurement
        // of the pass that exists -- the current frame has not run yet. Feeds
        // the destruction controller as a ceiling; terrain reads the tier where
        // it builds its params.
        g_forwardQuality.Update(g_profiler.GpuScopeMs("Forward Extensions"));
        {
            float ceiling = 1.0f;
            switch (g_forwardQuality.Tier()) {
                case ForwardExtensionQualityTier::Balanced:    ceiling = 0.55f; break;
                case ForwardExtensionQualityTier::Performance: ceiling = 0.15f; break;
                default: break;
            }
            g_destruction.SetAdaptiveQualityCeiling(ceiling);
        }
        // Resolve decal parents after physics and projectile hits so their
        // world-space volumes use the same pose the chunk renders with.
        UpdateImpactDecals(deltaTime);
        visBuffer.SetImpactDecals(BuildImpactDecalGPUList(),
                                  g_impactDecalsEnabled &&
                                      g_impactDecalCutouts);
        g_profiler.BeginGpuFrame(g_dx12.frameIndex, g_dx12.commandList.Get());

        float cc[4] = { scene.clearColor.x, scene.clearColor.y, scene.clearColor.z, 1.0f };
        if (visibilityTestPending &&
            g_game.session.Screen() == GameScreen::Level1 &&
            !g_game.loading.Active()) {
            ++visibilityTestForwardFrames;
            if (visibilityTestForwardFrames == 1) {
                std::ofstream("visibility_smoke.log", std::ios::app)
                    << "forward-active\n";
            }
            if (!visibilityBenchmark && !visibilityForwardOnly &&
                visibilityTestForwardFrames >= 120) {
                scene.useVisibilityBuffer = true;
                visibilityTestPending = false;
                std::cerr << "VISIBILITY_TEST: toggled after 120 forward frames\n";
                std::ofstream("visibility_smoke.log", std::ios::app)
                    << "toggled\n";
                // Terrain and destruction both route through the visibility
                // buffer by default now. These two variables force the forward
                // fallback instead, so the parity path stays reachable in an
                // unattended run -- the toggles are otherwise UI-only.
                if (GetEnvironmentVariableA("SGE_VISIBILITY_NO_TERRAIN",
                                            nullptr, 0) > 0) {
                    visBuffer.SetTerrainVisibilityRequested(false);
                    std::ofstream("visibility_smoke.log", std::ios::app)
                        << "terrain-vb-disabled\n";
                }
                if (GetEnvironmentVariableA("SGE_VISIBILITY_NO_DESTRUCTION",
                                            nullptr, 0) > 0) {
                    visBuffer.SetDestructionVisibilityRequested(false);
                    std::ofstream("visibility_smoke.log", std::ios::app)
                        << "destruction-vb-disabled\n";
                }
            }
        }
        const bool deploymentForceForward =
            DeploymentPlanningActive() && g_deploymentDebugForceForward;
        const bool deploymentHideAtmosphere =
            DeploymentPlanningActive() && g_deploymentDebugHideAtmosphere;
        // Editor fog toggle. Suppressed at draw time rather than by clearing
        // scene.enableVolumetricFog: that flag is copied back into the
        // per-time-of-day store by ApplyLiveWeatherState, so writing it here
        // would bake a view preference into the level's authored weather.
        // Playtesting from inside the editor shows the level as shipped, so
        // the override lifts the moment PLAY is pressed.
        const bool editorHideFog =
            g_game.session.Screen() == GameScreen::LevelEditor &&
            !g_levelEditor.IsPlaying() && !g_levelEditor.FogEnabled();
        const RenderPath renderPath = RenderCoordinator::Choose({
            scene.useRaytracing && !deploymentForceForward,
            scene.useVisibilityBuffer && !deploymentForceForward,
            g_rt.initialized,
            visBuffer.initialized
        });
        const bool usingRaytracing = renderPath == RenderPath::Raytracing;
        const bool usingVisibility =
            renderPath == RenderPath::VisibilityBuffer;
        // Only the visibility path writes this flag, so clear it on every other
        // path. Left stale at true, the forward renderer would skip its terrain
        // draw on a frame where nothing rasterized terrain at all.
        if (!usingVisibility) g_terrainInVisibilityBuffer = false;
        visBuffer.PrepareBindlessFrame(g_dx12.frameIndex,
            scene.bindlessMaterials && g_bindlessMaterialsReady &&
            usingVisibility);
        const bool bindlessMaterialsActive = scene.bindlessMaterials &&
            g_bindlessMaterialsReady &&
            (!usingVisibility || (visBuffer.BindlessResolveReady() &&
                visBuffer.BindlessFrameReady(g_dx12.frameIndex)));
        g_bindlessMaterialsActive = bindlessMaterialsActive;
        mainShader.SetBindlessActive(bindlessMaterialsActive);
        visBuffer.SetBindlessActive(bindlessMaterialsActive && usingVisibility);
        // A crater changes the height field under pixels the camera is not
        // moving past, so reprojection alone would happily reuse history from
        // the ground that used to be there.
        if (g_terrainDeformedThisFrame) {
            visBuffer.NotifyTerrainDeformed();
            // Staggered: dropping every cached cascade in the same frame as the
            // explosion is a measured multi-hundred-ms GPU spike, and a crater
            // does not need the distant cascades updated on that exact frame.
            shadowMap.InvalidateCachedCascadesStaggered();
            g_terrainDeformedThisFrame = false;
        }
        visBuffer.aoTemporalMotionVectors = usingVisibility &&
            scene.enableAmbientOcclusion && scene.temporalBentNormalGTAO;
        const bool bentNormalGTAORequested = usingVisibility &&
            scene.enableAmbientOcclusion && scene.temporalBentNormalGTAO;
        const bool bentNormalHistoryValid = bentNormalGTAORequested &&
            screenSpaceAO.HasTemporalBentNormalHistory(
                visBuffer.postFrameIndex);
        visBuffer.SetBentNormalGTAOHistory(
            bentNormalHistoryValid
                ? screenSpaceAO.GetTemporalBentNormalHistory() : nullptr,
            bentNormalGTAORequested, bentNormalHistoryValid);
        static bool visibilityWasActive = false;
        if (usingVisibility != visibilityWasActive) {
            visBuffer.InvalidateTemporalHistory();
            visibilityWasActive = usingVisibility;
        }
        scene.temporalJitterPixels = usingVisibility
            ? visBuffer.GetTemporalJitterPixels()
            : XMFLOAT2(0.0f, 0.0f);
        // Enhanced visuals: ray-traced tier layered on the visibility buffer.
        // Needs the static TLAS, which is normally built as a side effect of
        // enabling probe GI -- build it here too so the two features are
        // independent, and only once, since the acceleration structure is
        // static geometry that does not change per frame.
        {
            const auto& ddgiStatus = g_dxrDDGI.GetStatus();
            const bool wantEnhanced = scene.enhancedVisuals && usingVisibility &&
                !visBuffer.validationMode && ddgiStatus.dxrSupported &&
                ddgiStatus.inlineRaytracingSupported &&
                visBuffer.EnhancedVisualsReady();
            static bool enhancedSceneBuilt = false;
            if (wantEnhanced && !enhancedSceneBuilt &&
                !g_game.loading.Active() && IsSceneScreen()) {
                // Returns false when there is no static geometry yet; retry
                // next frame rather than latching a failure.
                enhancedSceneBuilt = BuildDXRDDGIAccelerationScene();
            } else if (wantEnhanced && enhancedSceneBuilt &&
                       scene.enhancedTLASRefit && !g_game.loading.Active() &&
                       IsSceneScreen()) {
                // Transforms only -- see RefitDXRDDGIAccelerationScene. Skipped
                // on the frame of the full build, which already placed every
                // instance at its current transform.
                ProfilerDX12::Scope refitScope(g_profiler, "TLAS Refit",
                                               g_dx12.commandList.Get());
                RefitDXRDDGIAccelerationScene();
            }
            if (!IsSceneScreen()) enhancedSceneBuilt = false;
            visBuffer.SetEnhancedVisuals(
                wantEnhanced, scene.enhancedRTShadows,
                scene.enhancedRayClassify, scene.enhancedConfidenceThreshold,
                g_dxrDDGI.Scene().TLASAddress(),
                scene.enhancedRTReflections);
            scene.enhancedRayFraction = wantEnhanced
                ? visBuffer.EnhancedRayFraction() : 0.0f;
        }
        const bool visibilityDebugActive =
            usingVisibility && visBuffer.debugViewMode != 0;
        const bool bentGTAODiagnosticActive = usingVisibility &&
            scene.enableAmbientOcclusion && scene.temporalBentNormalGTAO &&
            visBuffer.BentNormalGTAODiagnosticActive();
        // Validation is a cross-renderer comparison mode. Keep both sides on
        // the same baseline while M toggles ownership: no MSAA, fog, or FXAA.
        const bool visibilityParityValidation =
            !usingRaytracing && visBuffer.initialized && visBuffer.validationMode;
        const bool commonHDRValidationTarget =
            usingVisibility || visibilityParityValidation;
        const bool msaaActive =
            scene.enableMSAA && msaa.initialized &&
            !usingRaytracing && !usingVisibility && !visibilityParityValidation;
        const bool grassMSAAActive =
            usingVisibility && !visibilityDebugActive &&
            !bentGTAODiagnosticActive &&
            scene.enableGrassMSAA && grassMSAA.initialized &&
            mainShader.GetHDRMSAAGrassPipelineState() != nullptr;
        const bool waterPassEnabled = !usingRaytracing &&
            !g_emptyLevelMode && waterRenderer.initialized &&
            (g_ocean.IsInitialized() || g_water.IsInitialized()) &&
            !(DeploymentPlanningActive() && g_deploymentDebugHideWater);
        // The MSAA forward path resolves before its single-sample water pass,
        // so its sorted transparency queue is flushed immediately before that
        // resolve. Other paths can split the same queue around water.
        const bool splitWaterTransparency =
            waterPassEnabled && !msaaActive && !bentGTAODiagnosticActive;
        // Record the secondary view before the primary transparency queue
        // opens, so scope-camera glass can never leak into the main replay.
        mainShader.BeginFrame();
        mainShader.SetPalmWindFrame(g_trees.GetWindFrame());
        g_meshShader.BeginFrame();
        mainShader.SetPreviousViewProjection(previousHZBViewProjection);
        RenderSniperScopeTexture(now);

        BeginWaterTransparencySplitDX12(scene, false);
        mainShader.SetMSAAEnabled(msaaActive);
        g_meshShader.SetMSAAEnabled(msaaActive);
        g_terrain.SetMSAAEnabled(msaaActive);
        skyRenderer.SetMSAAEnabled(msaaActive);
        if (msaaActive) msaa.BindAndClear(cc);
        else ClearRenderTarget(cc);
        {
            ProfilerDX12::Scope profile(g_profiler, "Sky", g_dx12.commandList.Get());
            if (commonHDRValidationTarget) {
                visBuffer.BeginHDRBackground(g_dx12.commandList.Get());
                skyRenderer.SetHDRTargetEnabled(true);
            }
            const XMFLOAT4 skyCloudParams = deploymentHideAtmosphere
                ? XMFLOAT4(0.0f, 0.0f,
                           scene.atmosphereCloudBaseHeight,
                           scene.atmosphereCloudThickness)
                : scene.enableFlyableClouds
                ? XMFLOAT4(0.0f, 0.0f,
                           scene.atmosphereCloudBaseHeight,
                           scene.atmosphereCloudThickness)
                : XMFLOAT4(scene.atmosphereCloudCoverage,
                           scene.atmosphereCloudDensity,
                           scene.atmosphereCloudBaseHeight,
                           scene.atmosphereCloudThickness);
            const bool sunLensEnabled =
                scene.enableSunLens && !deploymentHideAtmosphere;
            skyRenderer.SetSunLens(
                sunLensEnabled, scene.lightColor,
                scene.sunAngularRadiusDegrees, scene.sunDiscIntensity,
                scene.sunHaloIntensity);
            visBuffer.SetSunLens(
                sunLensEnabled,
                scene.GetViewMatrix() * scene.GetUnjitteredProjectionMatrix(),
                scene.camera.Position, scene.lightPos, scene.lightColor,
                scene.directionalLightIntensity);
            skyRenderer.Render(
                scene.camera, scene.EffectiveCameraFOV(), scene.lightPos, now,
                scene.enablePhysicalAtmosphere,
                scene.enableVolumetricClouds && !deploymentHideAtmosphere,
                XMFLOAT4(scene.atmosphereRayleighStrength,
                         scene.atmosphereMieStrength,
                         scene.atmosphereMieAnisotropy,
                         scene.atmosphereAerialDensity),
                skyCloudParams);
            if (commonHDRValidationTarget) {
                skyRenderer.SetHDRTargetEnabled(false);
                visBuffer.EndHDRBackground(g_dx12.commandList.Get());
            }
        }

        hzbCaptureActive = IsSceneScreen() &&
            !g_game.loading.Active() && !usingRaytracing && !msaaActive &&
            (g_useMeshShader || usingVisibility);
        if (!hzbCaptureActive) occlusionDepth.InvalidateCameraHistory();
        const bool hzbHistoryUsable = hzbCaptureActive &&
            occlusionDepth.CanUseHistory(
                scene.camera.Position, scene.camera.Front) &&
            !msaaUsedLastFrame;
        D3D12_GPU_DESCRIPTOR_HANDLE bindlessOcclusionHandle = {};
        if (g_bindlessMaterialsActive) {
            bindlessOcclusionHandle =
                bindlessHeap.GpuHandleAt(BINDLESS_FALLBACK_WHITE);
            const D3D12_CPU_DESCRIPTOR_HANDLE source =
                occlusionDepth.GetCPUHandle();
            const UINT index = bindlessHeap.AllocateTransientTable(&source, 1);
            if (index != BINDLESS_INVALID_INDEX)
                bindlessOcclusionHandle = bindlessHeap.GpuHandleAt(index);
        }
        g_meshShader.SetOcclusionDepth(
            occlusionDepth.GetGPUHandle(), hzbHistoryUsable,
            occlusionDepth.GetMipCount(), bindlessOcclusionHandle);

        if (IsSceneScreen() && g_game.loading.Active()) {
        if (g_game.loading.Stage() == LevelLoadStage::WorldAssets) {
            LoadFloorMudMaterial();
            if (!g_emptyLevelMode) {
                std::cout << "Loading models/h2.glb...\n";
                crateModel = GLBImporter::LoadGLB(
                    "Content/Models/h2.glb", g_dx12.device, g_dx12.commandList);
            }
            AdvanceLevelLoading(LevelLoadStage::Destruction,
                "Destruction geometry and Blast actors",
                "procedural house chunks + roof",
                g_emptyLevelMode || crateModel != nullptr);
        } else if (g_game.loading.Stage() == LevelLoadStage::Destruction) {
            if (!levelDestructionLoadInFlight) {
                if (g_destruction.IsInitialized()) WaitForGPU();
                // Build authored scene nodes on the render thread because their
                // texture uploads record into this frame's direct command list.
                auto houseTemplate = CreateDestructibleWallModel();
                ApplyHouseTextures(houseTemplate, g_dx12.device.Get(),
                    g_dx12.commandList.Get());
                AppendRoofChunksToDestructionModel(houseTemplate);
                g_houseTemplate = CloneSceneTree(houseTemplate);
                normalWallModel = CloneSceneTree(houseTemplate);
                stressWallModel = CloneSceneTree(houseTemplate);
                const bool customLayout = g_customLevelMode;
                g_customLevelMode = false;
                ArrangeHousesInCross(normalWallModel, false);
                ArrangeHousesInCross(stressWallModel, true);
                g_customLevelMode = customLayout;
                if (customLayout) {
                    wallModel = CloneSceneTree(houseTemplate);
                    ArrangeHousesInCross(wallModel, false);
                } else wallModel = g_stressTestMode ? stressWallModel : normalWallModel;

                // Chunk meshlets, Blast actors, Box3D bodies, and merged actor
                // geometry are CPU/device creation work. D3D12 device creation
                // is thread-safe; static uploads enter the mutex-protected queue.
                // Keeping it off the render thread makes resource telemetry and
                // the loading screen update while thousands of chunks build.
                const std::shared_ptr<SceneNode> destructionModel = wallModel;
                ComPtr<ID3D12Device> destructionDevice = g_dx12.device;
                g_game.loading.SetCurrent(
                    "Build meshlet SRVs, Blast actors and depth-only batch",
                    g_stressTestMode
                        ? "DestructionDX12: stressWallModel"
                        : "DestructionDX12: normalWallModel");
                levelDestructionLoadFuture = std::async(std::launch::async,
                    [destructionModel, destructionDevice]() {
                        return g_destruction.Initialize(destructionModel,
                            destructionDevice.Get(), 1, 1, 1);
                    });
                levelDestructionLoadInFlight = true;
            } else if (levelDestructionLoadFuture.wait_for(
                    std::chrono::seconds(0)) == std::future_status::ready) {
                const bool initialized = levelDestructionLoadFuture.get();
                levelDestructionLoadInFlight = false;
                if (g_emptyLevelMode && initialized) {
                    auto tp = CurrentTerrainParams();
                    tp.heightScale = scene.terrainHeightScale;
                    g_destruction.SetTerrainSampler([tp](float x, float z) {
                        return TerrainRendererDX12::HeightAt(tp, x, z);
                    }, CurrentPhysicsTerrainExtent());
                }
                AdvanceLevelLoading(
                    g_emptyLevelMode ? LevelLoadStage::Weapons
                                     : LevelLoadStage::Environment,
                    g_emptyLevelMode ? "Player weapon"
                                     : "Water, ocean, trees and scalable environment",
                    g_emptyLevelMode ? "AK47"
                        : (g_stressTestMode ? "stress environment"
                                            : "level environment"),
                    initialized);
            }
        } else if (g_game.loading.Stage() == LevelLoadStage::Environment) {
            // Pool of water beside the house (on the clear -X side) with a
            // handful of wooden crates dropped in to bob on the surface.
            // Pool sunk into a dug-out terrain basin (see TerrainHeight): the
            // surface spans the whole basin (out to the rim) and sits just below
            // ground level, so it reads as a filled hole in the ground. The pool
            // gets the terrain height sampler so its rigid floor is the actual
            // sloped basin -- crates and debris collide with the terrain.
            {
                auto tp = CurrentTerrainParams();
                tp.heightScale = scene.terrainHeightScale;
                auto terrainSampler = [tp](float x, float z) {
                    return TerrainRendererDX12::HeightAt(tp, x, z);
                };
                g_water.Initialize({ -22.0f, -1.85f, -20.0f }, { 14.0f, 2.7f, 14.0f },
                                   terrainSampler);

                // The sea. A single big wave surface at y = 0 (sea level), spanning
                // far past the terrain grid so it runs to the horizon in every
                // direction. No terrain sampler: nothing floats in it, so it gets
                // the cheap flat tank rather than a heightfield basin.
                // At the editor's maximum insertion radius the deployment
                // camera orbits 1.33 km from the island. A +/-1.6 km ocean left
                // its edge only a few hundred metres behind that camera and the
                // square boundary entered the view. +/-4.096 km covers the
                // complete deployment far plane at every supported radius.
                constexpr float kSeaSpan = 8192.0f;
                constexpr float kSeaDepth = 12.0f;
                // Near-shore reflections and wave/terrain intersections need
                // enough vertices to avoid exposing individual ocean triangles.
                // Half-rate updates keep the 128-cell surface inexpensive.
                g_ocean.SetGridResolution(128);
                g_ocean.SetOceanProfile();
                // The grid costs 16k CPU wave evaluations per write; the swell
                // reads the same at half rate. The pool stays at full rate for
                // its splash ripples.
                g_ocean.SetUpdateInterval(2);
                g_ocean.Initialize({ 0.0f, -kSeaDepth * 0.5f, 0.0f },
                                   { kSeaSpan, kSeaDepth, kSeaSpan });
                // House debris collides with the real terrain, not a flat plane.
                g_destruction.SetTerrainSampler(
                    terrainSampler, CurrentPhysicsTerrainExtent());
                // Debris/ragdolls splash the pool when they break the surface.
                g_destruction.SetSplashCallback([](float x, float z, float s) {
                    g_water.Splash(x, z, s);
                });
                // Skipped on the training range: the Humvee is hidden there, and
                // a physics body without a model is collision the player cannot
                // see and cannot explain.
                //
                // Also skipped in a custom level that places no Humvee entity.
                // primaryHumveeSpawn keeps its {0, 3.45, 0} default in that
                // case, so without this check every such level got an
                // unrequested Humvee sitting at the world origin.
                InitializeLevelHumveePhysics();

                // Palm grove ringing the pool. Shoot through a trunk and the tree
                // snaps at that height and topples away from you.
                g_trees.SetTerrainSampler(terrainSampler);
                ResetPalmTrees();

                LoadDandelionModel();
                g_prefabRebuildReason = "island scale changed";
                g_prefabRebuildRequested = true;
                RebuildScalableEnvironment();
            }
            // Same pool AABB for the destruction sim so house debris shoved into
            // the water floats too (surface at max.y).
            g_destruction.SetWaterRegion({ -29.0f, -3.2f, -27.0f },
                                         { -15.0f, -0.5f, -13.0f });
            if (crateModel) {
                if (auto merged = GLBImporter::MergeSceneByMaterial(crateModel, g_dx12.device)) {
                    crateModel = merged;
                }
                crateModel->UpdateGlobalTransform(crateModel->localTransform);
                crateShadowModel = GLBImporter::MergeSceneForDepth(
                    crateModel, g_dx12.device);
                size_t materialDraws = crateModel->mesh ? crateModel->mesh->primitives.size() : 0;
                std::cout << "h2 model loaded: " << materialDraws
                          << " material draw(s)\n";
            } else {
                std::cerr << "Failed to load h2.glb, falling back to procedural cube\n";
            }

            AdvanceLevelLoading(LevelLoadStage::Weapons,
                "Weapon and explosive barrel",
                "AK47 + models/Barrel Explosive/barrel.FBX");
        } else if (g_game.loading.Stage() == LevelLoadStage::Weapons) {

            // The AK47 view model. Loaded here so its texture uploads land in the
            // same command list the flush below submits.
            GunModel::Load();
            // The first-person arms share that window for the same reason, and
            // are drawn in the weapon's own local space.
            ArmsModel::Load();

            if (g_emptyLevelMode) {
                AdvanceLevelLoading(LevelLoadStage::GPUFinalize,
                    "Drain graphics uploads and generate texture mips",
                    "direct queue + compute queue", GunModel::Loaded());
            } else {
                // Authored explosive barrel. Source mesh is 2.08 m high; 0.72 keeps
                // its in-game size aligned with existing 1.5 m gameplay collision.
                const std::filesystem::path barrelSource =
                    "Content/Models/Barrel Explosive/barrel.FBX";
                std::string barrelCookError;
                g_explosiveBarrelModel = CookedAssetLoader::LoadForSource(
                    barrelSource, g_dx12.device, g_dx12.commandList,
                    &barrelCookError);
                if (g_explosiveBarrelModel) {
                    g_explosiveBarrelModel->scale =
                        XMFLOAT3(0.72f, 0.72f, 0.72f);
                    g_explosiveBarrelModel->UpdateLocalTransform();
                    XMFLOAT4X4 identity;
                    XMStoreFloat4x4(&identity, XMMatrixIdentity());
                    g_explosiveBarrelModel->UpdateGlobalTransform(identity);
                    std::cout << "Explosive barrel cooked asset ready\n";
                } else {
                    g_explosiveBarrelModel = FBXImporter::Load(
                        barrelSource.string(), g_dx12.device,
                        g_dx12.commandList, 0.72f, false, true);
                }
                if (g_explosiveBarrelModel) {
                    // Asset provides only diffuse + normal maps. Treating its
                    // unmasked surface as smooth metal creates broad white IBL
                    // glare over painted/rusted areas. Keep it dielectric and
                    // substantially rough, without changing other FBX assets.
                    const auto fixBarrelMaterial = [&](const auto& self,
                                                       const std::shared_ptr<SceneNode>& node)
                        -> void {
                        if (!node) return;
                        if (node->mesh) for (auto& primitive : node->mesh->primitives) {
                            if (!primitive.material) continue;
                            primitive.material->metallicFactor = 0.0f;
                            primitive.material->roughnessFactor = 0.88f;
                            primitive.material->metallicRoughnessTexture.Reset();
                            primitive.material->roughnessOnlyTexture = false;
                        }
                        for (const auto& child : node->children) self(self, child);
                    };
                    fixBarrelMaterial(fixBarrelMaterial, g_explosiveBarrelModel);
                    g_explosiveBarrelShadowModel = GLBImporter::MergeSceneForDepth(
                        g_explosiveBarrelModel, g_dx12.device);
                    std::cout << "Explosive barrel FBX ready\n";
                } else
                    std::cerr << "Explosive barrel FBX failed; using procedural fallback\n";

                // C4 brick. The GLB carries its own PBR maps (colour, normal,
                // metallic-roughness) embedded, unlike the FBX beside it, which
                // declares material slots but binds no textures at all and so
                // loads as untextured white.
                //
                // Geometry is authored in metres -- 0.268 x 0.083 x 0.189 m,
                // a real charge lying flat -- with no node scaling, so it needs
                // no conversion and no scale of its own.
                g_c4Model = GLBImporter::LoadGLB(
                    "Content/Models/C4/C4_bomb/source/c4.glb",
                    g_dx12.device, g_dx12.commandList);
                if (g_c4Model) {
                    XMFLOAT4X4 c4Identity;
                    XMStoreFloat4x4(&c4Identity, XMMatrixIdentity());
                    g_c4Model->UpdateGlobalTransform(c4Identity);

                    // Pin the material to fully opaque plastic. The asset omits
                    // baseColorFactor and alphaMode entirely, so those come from
                    // whatever the importer left in place -- and any alpha below
                    // 0.999 routes the brick through the transparent pipeline,
                    // which is what made it read as blue glass. The GLB also
                    // ships normalTexture.scale = 0, so its normal map
                    // contributes nothing and only tints the shading.
                    const auto fixC4Material = [&](const auto& self,
                                                   const std::shared_ptr<SceneNode>& node)
                        -> void {
                        if (!node) return;
                        if (node->mesh) for (auto& primitive : node->mesh->primitives) {
                            if (!primitive.material) continue;
                            auto& m = primitive.material;
                            m->baseColorFactor = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
                            m->alphaCutout = false;
                            m->alphaFromLuminance = false;
                            // Moulded plastic case: dielectric, fairly rough.
                            m->metallicFactor = 0.0f;
                            m->roughnessFactor = 0.85f;
                            m->roughnessOnlyTexture = false;
                        }
                        for (const auto& child : node->children) self(self, child);
                    };
                    fixC4Material(fixC4Material, g_c4Model);
                    std::cout << "C4 model ready" << std::endl;
                } else
                    std::cerr << "C4 GLB failed; using procedural fallback"
                              << std::endl;

                AdvanceLevelLoading(LevelLoadStage::Humvee,
                    "Humvee model, bounds and shadow mesh",
                    "Content/Models/Humvee/humvee.fbx",
                    g_explosiveBarrelModel != nullptr);
            }
        } else if (g_game.loading.Stage() == LevelLoadStage::Humvee) {
            // The base is a loadout hub, not a playable map: no combat vehicles
            // are placed and no squad is spawned there. Importing the Humvee
            // anyway cost roughly a second of the base's load for a model that
            // is never drawn, so skip it. Every consumer already null-checks
            // g_humveeModel, so leaving it empty is the same state as a failed
            // import, which the level has always tolerated.
            if (!g_baseMode) {
                g_humveeModel = FBXImporter::Load(
                    "Content/Models/Humvee/humvee.fbx",
                    g_dx12.device, g_dx12.commandList, 1.0f, false, true);
                if (g_humveeModel) {
                    for (const auto& child : g_humveeModel->children)
                        if (child && child->name == "HumveeTurret") {
                            g_humveeTurretNode = child;
                            break;
                    }
                    ConfigureHumveeBounds();
                    g_humveeShadowModel = GLBImporter::MergeSceneForDepth(
                        g_humveeModel, g_dx12.device);
                    std::cout << "Humvee FBX ready at center\n";
                } else {
                    std::cerr << "Humvee FBX failed to load\n";
                }
            }

            AdvanceLevelLoading(LevelLoadStage::Helicopter,
                "OH-1 import, geometry LOD and rotor setup",
                "Content/Models/OH-1_fbx/OH-1.fbx",
                g_baseMode || g_humveeModel != nullptr);
        } else if (g_game.loading.Stage() == LevelLoadStage::Helicopter) {
          // Same reasoning as the Humvee stage above: the base parks no OH-1,
          // and this stage carries the geometry LOD reduction, which was the
          // single most expensive step in the base's load.
          if (!g_baseMode) {
            const std::string helicopterModelPath =
                ResolveTexturePath("Content/Models/OH-1_fbx/OH-1.fbx");
            std::cout << "OH-1 asset: " << helicopterModelPath << "\n";
            g_helicopterModel = FBXImporter::Load(
                helicopterModelPath,
                g_dx12.device, g_dx12.commandList, 1.0f, false, true, true);
            if (g_helicopterModel) {
                for (const auto& child : g_helicopterModel->children) {
                    if (!child) continue;
                    if (child->name == "OH1MainRotor")
                        g_helicopterMainRotorNode = child;
                    else if (child->name == "OH1TailRotor")
                        g_helicopterTailRotorNode = child;
                }
                // Second airframe for the reinforcement dropship. Shares all
                // geometry with the original (see CloneSceneNodeShallow) and
                // costs only the node tree, but owns its own rotor transforms
                // so the two aircraft spin independently.
                g_secondaryHelicopterModel =
                    CloneSceneNodeShallow(g_helicopterModel);
                if (g_secondaryHelicopterModel) {
                    for (const auto& child :
                             g_secondaryHelicopterModel->children) {
                        if (!child) continue;
                        if (child->name == "OH1MainRotor")
                            g_secondaryHelicopterMainRotorNode = child;
                        else if (child->name == "OH1TailRotor")
                            g_secondaryHelicopterTailRotorNode = child;
                    }
                }
                ConfigureHelicopterBounds();
                std::cout << "OH-1 helicopter ready above center\n";
            } else {
                std::cerr << "OH-1 helicopter FBX failed to load\n";
            }
            if (ApplyDarkGreenToHumvee())
                std::cout << "Humvee dark green fallback applied to untextured materials\n";
            else
                std::cout << "Humvee using embedded base colour texture\n";
          }

            AdvanceLevelLoading(LevelLoadStage::Boat,
                "Military boat import, bounds and patrol setup",
                kBoatModelPath,
                g_baseMode || g_helicopterModel != nullptr);
        } else if (g_game.loading.Stage() == LevelLoadStage::Boat) {
            g_boatModel.reset();
            g_boatShadowModel.reset();
            if (g_levelPatrolBoatEnabled) {
                g_boatModel = GLBImporter::LoadGLB(
                    kBoatModelPath,
                    g_dx12.device, g_dx12.commandList);
                if (g_boatModel) {
                    ConfigureBoatBounds();
                    ConfigureBoatMaterials(g_boatModel);
                    g_boatShadowModel = GLBImporter::MergeSceneForDepth(
                        g_boatModel, g_dx12.device);
                    g_boatCenter = { 0.0f, 0.0f, 0.0f };
                    g_boatPosition = g_boatCenter;
                    std::cout << "Military boat GLB ready, patrolling island\n";
                } else {
                    std::cerr << "Military boat GLB failed to load\n";
                }
            }

            // Insertion boat: a second instance of the same hull, posed by its
            // own run rather than the patrol circuit. Loaded separately so the
            // two never share a scene node (one transform per node).
            g_insertionBoatModel = GLBImporter::LoadGLB(
                kBoatModelPath,
                g_dx12.device, g_dx12.commandList);
            if (g_insertionBoatModel) {
                ConfigureInsertionBoatBounds();
                ConfigureBoatMaterials(g_insertionBoatModel);
                g_insertionBoatShadowModel = GLBImporter::MergeSceneForDepth(
                    g_insertionBoatModel, g_dx12.device);
                // Same exclusion as the level-start arming: a hub is walked
                // into, so no transport is armed for it. This runs after
                // StartLevelOne on a cold load, so without g_baseMode here the
                // asset load would re-arm the run that level start stood down.
                g_insertionBoatRestartPending = !g_emptyLevelMode && !g_baseMode;
                std::cout << "Insertion boat GLB ready\n";
            } else {
                std::cerr << "Insertion boat GLB failed to load\n";
            }

            // Both airframes load here. The second is optional: a missing GLB
            // leaves its slot null and ApplyInsertionAirframe falls back to the
            // BlackHawk, so a content drop that omits it degrades to the
            // historical single-aircraft behaviour instead of failing the load.
            g_blackHawkAirframeModel[0] = GLBImporter::LoadGLBSkinned(
                "Content/Models/BlackHawk/blackhawk.glb",
                g_dx12.device, g_dx12.commandList,
                g_blackHawkAirframeSkeleton[0]);
            // Plain loader: this GLB's skin binds no geometry, so the skinned
            // path would buy an unused palette. Its node hierarchy is what
            // matters, and LoadGLB keeps that -- ApplyInsertionAirframe finds
            // the 'Bone' node the rotor disc hangs from and spins it there.
            g_blackHawkAirframeModel[1] = GLBImporter::LoadGLB(
                "Content/Models/NewBlackHawk/NewBlackHawk.glb",
                g_dx12.device, g_dx12.commandList);
            if (g_blackHawkAirframeModel[1])
                std::cout << "Second insertion airframe GLB ready\n";
            else
                std::cerr << "Second insertion airframe GLB missing; "
                             "only the BlackHawk will be offered\n";

            ApplyInsertionAirframe(g_insertionAirframe);
            if (g_blackHawkModel) {
                // No merged depth proxy: merging bakes the bind pose into flat
                // geometry, which would leave the shadow's blades frozen while
                // the lit ones turn. The shadow pass skins the real model.
                g_blackHawkShadowModel.reset();
                // The run itself is armed by the pending flag once loading
                // finishes, so the drop-off is taken from the settled spawn.
                // Starting it here would aim at whatever the camera was mid-load.
                // Not on the hub: it has no insertion, and this stage lands
                // after StartLevelOne stood the run down on a cold load.
                g_blackHawkInsertionRestartPending = !g_baseMode;
                if (g_baseMode)
                    std::cout << "BlackHawk GLB ready, parked (no insertion)\n";
                else
                    std::cout << "BlackHawk GLB ready, inbound to player spawn\n";
            } else {
                std::cerr << "BlackHawk GLB failed to load\n";
            }

            AdvanceLevelLoading(LevelLoadStage::BanditModel,
                "Bandit mesh, skeleton, clips and physics asset",
                "Content/Models/MilitaryMercenaryBandit/SK_Bandit.FBX",
                g_helicopterModel != nullptr);
        } else if (g_game.loading.Stage() == LevelLoadStage::BanditModel) {

            // Skinned Bandit enemy: mesh + walk/idle/run clips. Texture uploads
            // ride the same command list flushed just below.
            //
            // The base spawns neither enemies nor allies, so both skinned
            // imports (mesh, skeleton, three clips and a physics asset each)
            // were the largest single cost in its load. BanditSpawn below
            // already no-ops on an invalid model, so skipping the import needs
            // no further guarding.
            if (!g_baseMode) {
                const std::string banditDir = "Content/Models/MilitaryMercenaryBandit/";
                const std::string animDir = banditDir + "Animations/Demo/";
                std::vector<std::string> clips = {
                    animDir + "ThirdPersonIdle.FBX",
                    animDir + "ThirdPersonWalk.FBX",
                    animDir + "ThirdPersonRun.FBX",
                };
                SkinnedModel bm = SkinnedFBXImporter::Load(
                    banditDir + "SK_Bandit.FBX", clips, g_dx12.device, g_dx12.commandList);
                bm.ragdoll = T3DPhysicsAsset::Load(banditDir + "Phy_Bandit_PhysicsAsset.T3D");
                if (bm.valid) {
                    g_banditModel = std::move(bm);
                } else {
                    std::cerr << "Bandit squad failed to load\n";
                }

                // Same mesh/clip pipeline, recolored textures: the player's two
                // marine allies.
                const std::string marineDir = "Content/Models/MarineAlly/";
                const std::string marineAnimDir = marineDir + "Animations/Demo/";
                std::vector<std::string> marineClips = {
                    marineAnimDir + "ThirdPersonIdle.FBX",
                    marineAnimDir + "ThirdPersonWalk.FBX",
                    marineAnimDir + "ThirdPersonRun.FBX",
                };
                SkinnedModel mm = SkinnedFBXImporter::Load(
                    marineDir + "SK_Bandit.FBX", marineClips, g_dx12.device, g_dx12.commandList);
                mm.ragdoll = T3DPhysicsAsset::Load(marineDir + "Phy_Bandit_PhysicsAsset.T3D");
                if (mm.valid) {
                    g_marineModel = std::move(mm);
                } else {
                    std::cerr << "Marine allies failed to load\n";
                }
            }
            AdvanceLevelLoading(LevelLoadStage::BanditSpawn,
                "Spawn squad and turret gunners",
                g_stressTestMode ? "stress squad: enemies + 2 gunners"
                                 : "level squad: enemies + Humvee gunners",
                g_baseMode || g_banditModel.valid);
        } else if (g_game.loading.Stage() == LevelLoadStage::BanditSpawn) {
            if (g_banditModel.valid) {
                for (size_t i = 0; i < ActiveBanditSlotCount(); ++i)
                    if (!SpawnBandit()) break;
                SpawnLevelHumveeTurretGunners();
                if (g_levelPatrolBoatEnabled) SpawnBoatTurretGunner();
                // Before the player deploys: the squad exists and the navmesh
                // is built, so this is the last point at which the opening
                // layout can still be changed.
                ScatterEnemiesOnNavmesh();
                g_banditLoaded = true;
                std::cout << "Bandit squad ready: " << LiveBanditCount()
                          << " live enemies\n";
            }
            if (g_marineModel.valid) {
                SpawnMarinesFromLevel();
                std::cout << "Marine allies ready: " << LiveMarineCount()
                          << " live marines\n";
            }
            AdvanceLevelLoading(LevelLoadStage::GPUFinalize,
                "Drain graphics uploads and generate texture mips",
                "direct queue + compute queue", g_banditLoaded);
        } else if (g_game.loading.Stage() == LevelLoadStage::GPUFinalize) {

            // Flush the load/mip-generation commands now and print any D3D12
            // validation errors before continuing, so mip-related bugs surface
            // immediately instead of silently corrupting later frames.
            // Earlier stages were submitted as ordinary loading-screen frames.
            // Drain them before mip generation and staging-resource release.
            WaitForDirectQueueIdleIsolated();
            g_mipGen.FlushPending();
            // Mip handoff is submitted by FlushPending. Wait once, then free
            // texture staging resources retained by imported scene materials.
            WaitForDirectQueueIdleIsolated();
            AdvanceLevelLoading(LevelLoadStage::SubmitUploads,
                "Submit final staging copies",
                "direct command list");
        } else if (g_game.loading.Stage() == LevelLoadStage::SubmitUploads) {
            // Do not release anything in this frame. Static/texture uploads are
            // flushed below the loading state machine, and EndFrame is what
            // actually submits this command list. A fence wait here cannot see
            // commands that have only been recorded, so releasing their upload
            // heaps now leaves CopyTextureRegion holding freed GPU addresses.
            // Advance only; the next frame performs the drain and release.
            AdvanceLevelLoading(LevelLoadStage::ReleaseUploads,
                "Release staging heaps and validate DX12 device",
                "material upload heaps");
        } else if (g_game.loading.Stage() == LevelLoadStage::ReleaseUploads) {
            if (!g_uploadHeapRelease.prepared) {
                // SubmitUploads spent a whole frame submitting the last copy
                // batch. Drain all earlier direct-queue work once before
                // releasing the arena, without changing frame-slot fences.
                SGE_LOG("LogDX12", EngineLog::Level::Display,
                    "Texture upload release: waiting for isolated direct queue drain");
                WaitForDirectQueueIdleIsolated();
                SGE_LOG("LogDX12", EngineLog::Level::Display,
                    "Texture upload release: isolated direct queue drain complete");

                std::unordered_set<const SceneNode*> visitedNodes;
                std::unordered_set<const SceneMaterial*> visitedMaterials;
                const auto collectNode = [&](const std::shared_ptr<SceneNode>& node) {
                    CollectMaterialUploadHeaps(
                        node, visitedNodes, visitedMaterials);
                };
                collectNode(g_helicopterModel);
                collectNode(g_humveeModel);
                collectNode(g_boatModel);
                collectNode(g_insertionBoatModel);
                collectNode(g_blackHawkModel);
                collectNode(g_explosiveBarrelModel);
                collectNode(g_dandelionModel);
                for (const auto& entry : g_prefabModelCache)
                    collectNode(entry.second.model);
                collectNode(g_banditModel.node);
                for (const auto& material : g_banditModel.materialKeepAlive)
                    CollectMaterialUploadHeaps(material, visitedMaterials);
                collectNode(crateModel);
                collectNode(wallModel);
                for (const auto& rejected : g_rejectedUploadModels)
                    collectNode(rejected);

                SGE_LOG("LogDX12", EngineLog::Level::Display,
                    "Texture upload release: clearing " +
                    std::to_string(g_uploadHeapRelease.materials.size()) +
                    " compatibility material owners");

                // Pooled level uploads are owned by the arena, so these clears
                // only discard compatibility references. Keeping the arena
                // alive until after every owner is empty ensures none of the
                // backing resources can final-release in this loop.
                for (const auto& material : g_uploadHeapRelease.materials)
                    material->uploadHeaps.clear();
                SGE_LOG("LogDX12", EngineLog::Level::Display,
                    "Texture upload release: material owners cleared");
                ArmsModel::ReleaseUploadHeaps();
                SGE_LOG("LogDX12", EngineLog::Level::Display,
                    "Texture upload release: arms owners cleared");
                g_terrain.ReleaseUploadHeaps();
                SGE_LOG("LogDX12", EngineLog::Level::Display,
                    "Texture upload release: terrain owners cleared");
                g_rejectedUploadModels.clear();
                SGE_LOG("LogDX12", EngineLog::Level::Display,
                    "Texture upload release: rejected models cleared");

                const TextureUploadArenaStatsDX12 stats =
                    GetTextureUploadArenaStatsDX12();
                g_uploadHeapRelease.totalChunks = stats.chunkCount;
                g_uploadHeapRelease.usedBytes = stats.usedBytes;
                BeginTextureUploadArenaReleaseDX12();
                g_uploadHeapRelease.prepared = true;
                SGE_LOG("LogDX12", EngineLog::Level::Display,
                    "Texture upload release: arena contains " +
                    std::to_string(stats.chunkCount) + " chunks, " +
                    std::to_string(stats.usedBytes) + " requested bytes");
            }

            // One final COM release per rendered frame keeps Windows messages
            // and the loading UI moving even if the driver spends time
            // returning a large pooled allocation to the OS.
            const TextureUploadArenaStatsDX12 beforeRelease =
                GetTextureUploadArenaStatsDX12();
            SGE_LOG("LogDX12", EngineLog::Level::Display,
                "Texture upload release: releasing chunk " +
                std::to_string(beforeRelease.releasedChunks + 1) + " / " +
                std::to_string(g_uploadHeapRelease.totalChunks));
            ReleaseOneTextureUploadArenaChunkDX12();
            SGE_LOG("LogDX12", EngineLog::Level::Display,
                "Texture upload release: chunk released");
            if (!TextureUploadArenaReleaseCompleteDX12()) {
                const TextureUploadArenaStatsDX12 stats =
                    GetTextureUploadArenaStatsDX12();
                const size_t released = g_uploadHeapRelease.totalChunks -
                    stats.chunkCount;
                g_game.loading.SetCurrent(
                    "Release staging heaps and validate DX12 device",
                    std::to_string(released) + " / " +
                    std::to_string(g_uploadHeapRelease.totalChunks) +
                    " pooled chunks | " +
                    std::to_string(g_uploadHeapRelease.usedBytes / (1024 * 1024)) +
                    " MiB staged");
            } else {
                SGE_LOG("LogDX12", EngineLog::Level::Display,
                    "Texture upload release: finalizing empty arena");
                EndTextureUploadArenaReleaseDX12();
                SGE_LOG("LogDX12", EngineLog::Level::Display,
                    "Texture upload release: arena reset complete");
                DumpDX12DebugMessages();
                SGE_LOG("LogDX12", EngineLog::Level::Display,
                    "Texture upload release: debug messages drained");
                // First Level 1 load is complete. Start timing after load/GPU
                // waits and staging cleanup, not from the menu click.
                if (g_emptyLevelMode) {
                    emptyLevelAssetsLoaded = true;
                } else if (g_baseMode) {
                    // The base deliberately skips the Humvee, the OH-1 and the
                    // bandit/marine skinned meshes, so its load leaves the full
                    // set incomplete. Latching fullLevelAssetsLoaded here would
                    // tell the next level those imports had already happened and
                    // StartLevelOne would skip BeginLevelLoading entirely --
                    // the player would fly out of the hub into a map with no
                    // vehicles and no enemies.
                    emptyLevelAssetsLoaded = true;
                } else {
                    fullLevelAssetsLoaded = true;
                    emptyLevelAssetsLoaded = true;
                }
                SGE_LOG("LogDX12", EngineLog::Level::Display,
                    "Texture upload release: querying device health");
                const HRESULT deviceStatus = g_dx12.device
                    ? g_dx12.device->GetDeviceRemovedReason()
                    : E_POINTER;
                SGE_LOG("LogDX12", EngineLog::Level::Display,
                    "Texture upload release: device health query returned " +
                    std::to_string(static_cast<long>(deviceStatus)));
                CompleteLevelLoading(deviceStatus == S_OK);
                SGE_LOG("LogDX12", EngineLog::Level::Display,
                    "Texture upload release: loading marked complete");
                g_uploadHeapRelease.Reset();
                SGE_LOG("LogDX12", EngineLog::Level::Display,
                    "Texture upload release: compatibility state reset");
                // Build the sparse probe layout now the level's geometry exists.
                if (!g_emptyLevelMode && g_game.world.Level().dxrDDGI.enabled)
                    RequestLiveDXRDDGIRebuild();
                g_game.session.StartTimer();
                lastTime = gameTimer.GetElapsed();
            }
        }
        }

        // Recast the IR designator from the settled camera and weapon pose.
        //
        // Must be here, before any render pass. It used to sit down with the
        // night-vision gain block, which runs AFTER RenderForward has already
        // drawn the frame -- so the beam published each frame was not consumed
        // until the next one, and it visibly trailed the muzzle whenever the
        // player moved. The gun itself is posed inside the render pass from
        // GetGunBaseMatrix, so the beam has to be computed before that too.
        UpdateIRLaser();
        UpdateAimDebugRay();

        // ?? render ??
        // Main menu draws only sky plus UI. Level geometry, weapons, enemies,
        // shadows, and particles are never submitted behind the menu.
        // Loaders and destruction rebuilds enqueue immutable geometry in DEFAULT
        // heaps. Record all copies before any pass consumes those resources.
        const uint32_t submittedUploads =
            FlushStaticBufferUploadsDX12(g_dx12.commandList.Get());
        if (g_game.loading.Active())
            g_game.loading.RecordSubmittedUploads(submittedUploads);

        // NVG needs some real pre-tonemap surface signal to amplify. Temporarily
        // add the requested world-ambient boost while goggles are raised,
        // ramped with the eyepiece transition. Keep the lift restrained at 0.02
        // for every time-of-day preset. Restore the authored value before fog so
        // the extra fill reveals geometry without lighting a screen-sized slab
        // of atmosphere.
        const float authoredAmbientLightingIntensity =
            scene.ambientLightingIntensity;
        const bool nvgWorldAmbientBoosted =
            IsSceneScreen() && !g_game.loading.Active() &&
            g_game.mission.Loadout().gear == GearType::NightVisionGoggles &&
            scene.player.health > 0.0f &&
            (g_nightVisionActive || g_nightVisionBlend > 0.001f);
        if (nvgWorldAmbientBoosted) {
            const float ambientBoostBlend = (std::max)(
                g_nightVisionBlend, g_nightVisionActive ? 0.18f : 0.0f);
            constexpr float ambientBoost = 0.02f;
            scene.ambientLightingIntensity +=
                ambientBoost * ambientBoostBlend;
        }
        // Lightning lights the world for everyone, goggles or not. Added after
        // the NVG boost and restored by the same path below, so the two stack
        // without either needing to know about the other.
        const bool lightningLit = g_lightningAmbient > 0.0005f;
        if (lightningLit)
            scene.ambientLightingIntensity += g_lightningAmbient;

        // Goggles see through most of the fog. Volumetric fog is lit atmosphere,
        // and the intensifier's gain amplifies that slab far more than the
        // surfaces behind it -- a haze that reads as light depth by eye becomes
        // a bright wall through the tube, flattening the image into a flat green
        // field. Near-IR also scatters far less in air than visible light, which
        // is why intensifiers see through haze better than the naked eye.
        //
        // A remnant is kept rather than cutting to zero: with no fog at all the
        // scene loses its aerial depth cue entirely and distant terrain sits as
        // flat as the near ground, which reads as a rendering bug rather than as
        // clear air.
        //
        // Scaled by the goggle ramp so the fog returns as they come down,
        // rather than snapping back the instant the blend leaves 1.
        const float authoredVolumetricFogDensity = scene.volumetricFogDensity;
        const bool nvgFogThinned = nvgWorldAmbientBoosted;
        if (nvgFogThinned) {
            // 0.06, not 0.18: at 0.18 the remaining slab was still enough for
            // the tube gain to pull it into a visible wedge of brightening in
            // front of the muzzle, which is worse than having no fog at all.
            // This is a trace, just enough for distance to fall off.
            constexpr float kNVGFogRetained = 0.06f;
            const float thinBlend = (std::max)(
                g_nightVisionBlend, g_nightVisionActive ? 0.18f : 0.0f);
            scene.volumetricFogDensity *=
                1.0f - (1.0f - kNVGFogRetained) * thinBlend;
        }

        XMMATRIX fogLightSpace = XMMatrixIdentity();
        ID3D12Resource* fogShadowResource = nullptr;
        bool renderedScene = false;
        // The Humvee spotlight is added before clustered-light culling and
        // removed after the last consumer. Its caster list is rebuilt from the
        // current vehicle pose before the depth pass, leaving no player or
        // helicopter spotlight in the frame.
        g_spotShadowCasters.clear();
        const int headlightsInLightList = AddVehicleHeadlights(scene);
        if (IsSceneScreen() && !g_game.loading.Active() &&
            usingRaytracing) {
            ProfilerDX12::Scope profile(g_profiler, "Raytracing", g_dx12.commandList.Get());
            RenderRaytracing(scene);
        } else if (IsSceneScreen() && !g_game.loading.Active() &&
                   usingVisibility) {
            // Each pass gets its own sibling scope. ProfilerDX12::Scope is a flat
            // begin/end timestamp pair with no nesting or child subtraction, so a
            // scope that encloses another double-counts it: this block used to
            // wrap the shadow render AND the forward extensions inside
            // "Visibility Buffer", which reported 11.5 ms + 10.4 ms against a
            // 16.4 ms frame. The explicit sub-blocks bound each scope's lifetime
            // so the destructor (which emits the end timestamp) fires before the
            // next scope is constructed.
            XMMATRIX lightSpace = XMMatrixIdentity();
            ID3D12Resource* shadowResource = nullptr;
            if (scene.enableShadows && shadowMap.initialized &&
                (scene.lightType == 0 || !g_spotShadowCasters.empty())) {
                ProfilerDX12::Scope profile(g_profiler, "Shadow", g_dx12.commandList.Get());
                lightSpace = shadowMap.Render(
                    scene, geo, g_prefabRenderBatches,
                    (!g_emptyLevelMode && g_showH2Model)
                        ? (crateShadowModel ? crateShadowModel : crateModel)
                        : nullptr,
                    (!g_emptyLevelMode && g_banditLoaded) ? &g_bandits : nullptr,
                    &mainShader);
                shadowResource = shadowMap.GetResource();
                // Handed to next frame's scope pass, which runs before this one.
                g_scopeShadowLightSpace = lightSpace;
                g_scopeShadowResource = shadowResource;
            }
            {
                ProfilerDX12::Scope profile(g_profiler, "Visibility Buffer", g_dx12.commandList.Get());
                RenderVBDraw(scene, mainShader, visBuffer, geo, packed,
                    lightSpace, shadowResource, &occlusionDepth,
                    hzbHistoryUsable, previousHZBViewProjection, floorMaterial,
                    (!g_emptyLevelMode && g_showH2Model) ? crateModel : nullptr);
            }
            fogLightSpace = lightSpace;
            fogShadowResource = shadowResource;
            renderedScene = !visibilityDebugActive;
            mainShader.SetHDRTargetEnabled(true);
            g_meshShader.SetHDRTargetEnabled(true);
            g_terrain.SetHDRTargetEnabled(true);
            const bool jitterExtensions = visBuffer.extensionMotionVectors;
            // RenderForward draws terrain, floor and foliage, none of which
            // have motion PSOs, and only the colour RTV is bound here. Leaving
            // extension motion on would hand those draws a 2-RT PSO against 1
            // bound target, and D3D12 silently drops every such draw -- the
            // terrain disappears. It is enabled later, only around the bandit
            // draws that own motion PSOs.
            mainShader.SetExtensionMotionEnabled(false);
            g_meshShader.SetExtensionMotionEnabled(false);
            if (!visibilityDebugActive && !bentGTAODiagnosticActive) {
                BeginWaterTransparencySplitDX12(
                    scene, true, splitWaterTransparency);
                ProfilerDX12::Scope extensions(
                    g_profiler, "Forward Extensions", g_dx12.commandList.Get());
                RenderForward(scene, mainShader, geo, g_prefabRenderBatches,
                    crateModel, floorMaterial,
                    lightSpace, shadowResource, true, !grassMSAAActive,
                    jitterExtensions);
            }
        } else if (IsSceneScreen() && !g_game.loading.Active()) {
            XMMATRIX lightSpace = XMMatrixIdentity();
            ID3D12Resource* shadowResource = nullptr;
            if (scene.enableShadows && shadowMap.initialized &&
                (scene.lightType == 0 || !g_spotShadowCasters.empty())) {
                ProfilerDX12::Scope profile(g_profiler, "Shadow", g_dx12.commandList.Get());
                lightSpace = shadowMap.Render(
                    scene, geo, g_prefabRenderBatches,
                    (!g_emptyLevelMode && g_showH2Model)
                        ? (crateShadowModel ? crateShadowModel : crateModel)
                        : nullptr,
                    (!g_emptyLevelMode && g_banditLoaded) ? &g_bandits : nullptr,
                    &mainShader);
                shadowResource = shadowMap.GetResource();
                // Handed to next frame's scope pass, which runs before this one.
                g_scopeShadowLightSpace = lightSpace;
                g_scopeShadowResource = shadowResource;
            }
            fogLightSpace = lightSpace;
            fogShadowResource = shadowResource;
            renderedScene = true;
            if (msaaActive) msaa.Bind();
            if (visibilityParityValidation) {
                visBuffer.BeginForwardExtensions(g_dx12.commandList.Get());
                mainShader.SetHDRTargetEnabled(true);
                g_meshShader.SetHDRTargetEnabled(true);
                g_terrain.SetHDRTargetEnabled(true);
                // Same reasoning as the hybrid path above: RenderForward's
                // draws have no motion PSOs and only the colour RTV is bound,
                // so motion selection stays off here.
                mainShader.SetExtensionMotionEnabled(false);
                g_meshShader.SetExtensionMotionEnabled(false);
            }
            {
                BeginWaterTransparencySplitDX12(
                    scene, true, splitWaterTransparency);
                ProfilerDX12::Scope profile(g_profiler, "Forward", g_dx12.commandList.Get());
                RenderForward(scene, mainShader, geo, g_prefabRenderBatches,
                    crateModel, floorMaterial, lightSpace, shadowResource);
            }
        }

        // Hybrid visibility and pure forward both establish global forward
        // resources before skinned/transparent extension passes.
        // Passes below draw after the visibility resolve. When extension motion
        // vectors are off, TAA cannot reproject them and the sub-pixel jitter
        // never cancels -- it reads as shaking. The toggle restores the jittered
        // projection because the matching motion vectors make the jitter sum to
        // zero again.
        const XMMATRIX extensionProj = visBuffer.extensionMotionVectors
            ? scene.GetProjectionMatrix()
            : scene.GetUnjitteredProjectionMatrix();
        if (renderedScene && !bentGTAODiagnosticActive &&
            !g_emptyLevelMode && g_banditLoaded) {
            ProfilerDX12::Scope profile(
                g_profiler, "Bandits", g_dx12.commandList.Get());
            // Bandits and their guns are the draws that own motion PSOs, so
            // the motion RTV and the matching PSO selection are switched on
            // only around them. The rest of this pass keeps a single render
            // target and its ordinary single-RT pipelines.
            const bool banditMotion = visBuffer.extensionMotionVectors;
            mainShader.SetExtensionMotionEnabled(banditMotion);
            g_meshShader.SetExtensionMotionEnabled(banditMotion);
            visBuffer.BeginMotionDraws(g_dx12.commandList.Get());
            for (auto& bandit : g_bandits) {
                if (!bandit) continue;
                bandit->Draw(mainShader, scene.GetViewMatrix(),
                             extensionProj, fogLightSpace);
                if (bandit->HasGunPose() && GunModel::Loaded()) {
                    mainShader.Use(false);
                    DrawMeshAt(GunModel::Mesh(), mainShader,
                        bandit->GunWorldMatrix(), scene.GetViewMatrix(),
                        extensionProj, fogLightSpace, true);
                }
                DrawSniperLaser(*bandit, scene, mainShader, geo, fogLightSpace);
            }
            visBuffer.EndMotionDraws(g_dx12.commandList.Get());
            mainShader.SetExtensionMotionEnabled(false);
            g_meshShader.SetExtensionMotionEnabled(false);
            mainShader.Use(scene.wireframeMode);
        }
        if (renderedScene && !bentGTAODiagnosticActive &&
            !TransparencyQueueActiveDX12()) {
            ProfilerDX12::Scope profile(
                g_profiler, "Impact Particles", g_dx12.commandList.Get());
            RenderImpactBillboards(scene, mainShader, geo, fogLightSpace);
        }

        if (renderedScene && !bentGTAODiagnosticActive && msaaActive &&
            TransparencyQueueActiveDX12()) {
            {
                ProfilerDX12::Scope profile(
                    g_profiler, "Transparency", g_dx12.commandList.Get());
                RenderWaterTransparencyPhaseDX12(
                    scene, mainShader, WaterTransparencyPhaseDX12::All,
                    fogShadowResource);
            }
            {
                ProfilerDX12::Scope profile(
                    g_profiler, "Impact Particles", g_dx12.commandList.Get());
                mainShader.InvalidateGraphicsRootBinding();
                RenderImpactBillboards(
                    scene, mainShader, geo, fogLightSpace,
                    WaterTransparencyPhaseDX12::All);
            }
            EndWaterTransparencySplitDX12();
        }

        // Collision wireframe last among the scene passes: it is an overlay, so
        // it should sit on top of everything solid while still depth-testing
        // against it.
        if (renderedScene && g_showCollisionDebug &&
            g_collisionDebugRenderer.initialized) {
            ProfilerDX12::Scope profile(
                g_profiler, "Collision Debug", g_dx12.commandList.Get());
            BuildCollisionDebugLines();
            if (g_collisionDebugRenderer.Render(scene,
                    mainShader.hdrTargetEnabled, mainShader.msaaEnabled)) {
                // Own root signature and heap bindings, as with the particle
                // renderer: whatever draws next must rebind the main ones.
                mainShader.InvalidateGraphicsRootBinding();
                mainShader.Use(scene.wireframeMode);
            }
        }

        if (renderedScene && !bentGTAODiagnosticActive && grassMSAAActive) {
            ProfilerDX12::Scope profile(
                g_profiler, "Grass 4x MSAA", g_dx12.commandList.Get());
            grassMSAA.Begin(g_dx12.commandList.Get());
            RenderGrassForward(scene, mainShader, scene.GetViewMatrix(),
                extensionProj, fogLightSpace, fogShadowResource,
                mainShader.GetHDRMSAAGrassPipelineState());

            // Resolve the independently sampled grass against opaque scene
            // depth, then reopen HDR for shared AO and fog.
            visBuffer.EndForwardExtensions(g_dx12.commandList.Get());
            grassMSAA.Composite(g_dx12.commandList.Get(),
                visBuffer.GetOutputResource(), visBuffer.GetMotionResource(),
                g_dx12.depthStencilBuffer.Get());
            visBuffer.BeginForwardExtensions(g_dx12.commandList.Get());
        }

        if (msaaActive) {
            ProfilerDX12::Scope profile(
                g_profiler, "MSAA Resolve", g_dx12.commandList.Get());
            msaa.ResolveToBackBuffer();
        }

        // Suppressed at draw time rather than by clearing
        // scene.enableAmbientOcclusion, so the planning screen never writes a
        // diagnostic into a setting gameplay reads back.
        const bool deploymentHideAO =
            DeploymentPlanningActive() && g_deploymentDebugHideAO;
        if (renderedScene && scene.enableAmbientOcclusion &&
            !deploymentHideAO && screenSpaceAO.initialized) {
            ProfilerDX12::Scope profile(
                g_profiler, "GTAO + Contact Shadows", g_dx12.commandList.Get());
            ID3D12Resource* aoDepth = msaaActive
                ? msaa.GetDepthResource() : g_dx12.depthStencilBuffer.Get();
            bool aoDepthMSAA = msaaActive;
            bool aoDepthAlreadyReadable = false;
            if (scene.grassInScreenSpaceAO && grassMSAAActive &&
                grassMSAA.GetCombinedDepthResource()) {
                aoDepth = grassMSAA.GetCombinedDepthResource();
                aoDepthMSAA = false;
                aoDepthAlreadyReadable = true;
            }
            screenSpaceAO.Render(scene, aoDepth, aoDepthMSAA,
                commonHDRValidationTarget ? visBuffer.GetOutputRTV()
                                          : D3D12_CPU_DESCRIPTOR_HANDLE{},
                commonHDRValidationTarget,
                usingVisibility ? visBuffer.GetVisibilityDepthResource() : nullptr,
                usingVisibility ? visBuffer.GetNormalRoughnessResource() : nullptr,
                aoDepthAlreadyReadable,
                scene.grassInScreenSpaceAO && grassMSAAActive
                    ? grassMSAA.GetCoverageResource() : nullptr,
                usingVisibility,
                usingVisibility && scene.temporalBentNormalGTAO
                    ? visBuffer.postFrameIndex : 0u,
                usingVisibility ? visBuffer.GetMotionResource() : nullptr,
                usingVisibility && visBuffer.extensionMotionVectors
                    ? D3D12_RESOURCE_STATE_RENDER_TARGET
                    : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                usingVisibility && scene.temporalBentNormalGTAO,
                usingVisibility &&
                    visBuffer.BentNormalGTAOAppliedLastResolve());
        }

        if (renderedScene && !bentGTAODiagnosticActive &&
            commonHDRValidationTarget &&
            scene.enableScreenSpaceReflections &&
            screenSpaceReflections.initialized) {
            ProfilerDX12::Scope profile(
                g_profiler, "Screen-Space Reflections",
                g_dx12.commandList.Get());
            screenSpaceReflections.Render(
                scene, visBuffer.GetOutputResource(), visBuffer.GetOutputRTV(),
                g_dx12.depthStencilBuffer.Get(),
                usingVisibility ? visBuffer.GetNormalRoughnessResource()
                                : nullptr);
        }

        const D3D12_CPU_DESCRIPTOR_HANDLE transparencyRTV =
            commonHDRValidationTarget
                ? visBuffer.GetOutputRTV()
                : GetCPUDescriptorHandle(
                    g_dx12.rtvHeap.Get(), g_dx12.rtvDescriptorSize,
                    g_dx12.frameIndex);
        bool combinedTransparencyDepthBound = false;
        const auto bindSplitTransparencyTarget = [&]() {
            D3D12_CPU_DESCRIPTOR_HANDLE transparencyDSV =
                g_dx12.dsvHeap->GetCPUDescriptorHandleForHeapStart();
            if (grassMSAAActive && grassMSAA.GetCombinedDepthResource()) {
                const D3D12_CPU_DESCRIPTOR_HANDLE combinedDSV =
                    grassMSAA.BeginCombinedDepthTest(
                        g_dx12.commandList.Get());
                if (combinedDSV.ptr) {
                    transparencyDSV = combinedDSV;
                    combinedTransparencyDepthBound = true;
                }
            }
            g_dx12.commandList->OMSetRenderTargets(
                1, &transparencyRTV, FALSE, &transparencyDSV);
            g_dx12.commandList->RSSetViewports(1, &g_dx12.viewport);
            g_dx12.commandList->RSSetScissorRects(1, &g_dx12.scissorRect);
        };
        const auto releaseSplitTransparencyDepth = [&]() {
            if (!combinedTransparencyDepthBound) return;
            grassMSAA.EndCombinedDepthTest(g_dx12.commandList.Get());
            combinedTransparencyDepthBound = false;
            // Later passes sample the combined texture, while rain and other
            // raster extensions expect the ordinary scene DSV to be bound.
            const D3D12_CPU_DESCRIPTOR_HANDLE sceneDSV =
                g_dx12.dsvHeap->GetCPUDescriptorHandleForHeapStart();
            g_dx12.commandList->OMSetRenderTargets(
                1, &transparencyRTV, FALSE, &sceneDSV);
        };

        if (renderedScene && WaterTransparencySplitActiveDX12()) {
            bindSplitTransparencyTarget();
            {
                ProfilerDX12::Scope profile(
                    g_profiler, "Transparency Before Water",
                    g_dx12.commandList.Get());
                RenderWaterTransparencyPhaseDX12(
                    scene, mainShader,
                    WaterTransparencyPhaseDX12::BeforeWater,
                    fogShadowResource);
            }
            {
                ProfilerDX12::Scope profile(
                    g_profiler, "Impact Particles Before Water",
                    g_dx12.commandList.Get());
                mainShader.InvalidateGraphicsRootBinding();
                RenderImpactBillboards(
                    scene, mainShader, geo, fogLightSpace,
                    WaterTransparencyPhaseDX12::BeforeWater);
            }
            releaseSplitTransparencyDepth();
        }

        if (renderedScene && !bentGTAODiagnosticActive &&
            waterPassEnabled) {
            ProfilerDX12::Scope profile(
                g_profiler, "Tropical Water",
                g_dx12.commandList.Get());
            ID3D12Resource* waterTarget = commonHDRValidationTarget
                ? visBuffer.GetOutputResource()
                : g_dx12.renderTargets[g_dx12.frameIndex].Get();
            D3D12_CPU_DESCRIPTOR_HANDLE waterRTV =
                commonHDRValidationTarget
                ? visBuffer.GetOutputRTV()
                : GetCPUDescriptorHandle(
                    g_dx12.rtvHeap.Get(),
                    g_dx12.rtvDescriptorSize,
                    g_dx12.frameIndex);
            ID3D12Resource* waterDepth = msaaActive
                ? msaa.GetDepthResource()
                : g_dx12.depthStencilBuffer.Get();
            D3D12_RESOURCE_STATES waterDepthState =
                D3D12_RESOURCE_STATE_DEPTH_WRITE;
            if (grassMSAAActive &&
                grassMSAA.GetCombinedDepthResource()) {
                waterDepth = grassMSAA.GetCombinedDepthResource();
                waterDepthState =
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            }
            if (scene.ultraWaterRefreshRequested) {
                waterRenderer.RefreshUltraWater();
                scene.ultraWaterRefreshRequested = false;
            }
            // Push the High-path wave sliders into the ocean volume before it
            // is drawn or queried. They live on the settings rather than being
            // read straight from Scene by the renderer so that buoyancy, which
            // samples the same settings on the CPU, cannot drift from the
            // surface being drawn.
            {
                OceanWaveSettings oceanWaves = g_ocean.GetOceanWaveSettings();
                if (oceanWaves.heightScale != scene.highWaterWaveHeight ||
                    oceanWaves.lengthScale != scene.highWaterWaveScale ||
                    oceanWaves.speedScale != scene.highWaterWaveSpeed) {
                    oceanWaves.heightScale = scene.highWaterWaveHeight;
                    oceanWaves.lengthScale = scene.highWaterWaveScale;
                    oceanWaves.speedScale = scene.highWaterWaveSpeed;
                    g_ocean.SetOceanWaveSettings(oceanWaves);
                }
            }
            QueueWaterBathymetry();
            waterRenderer.Render(
                scene, g_ocean, g_water,
                waterTarget, waterRTV, waterDepth,
                g_specularEnvironmentResource,
                commonHDRValidationTarget,
                usingVisibility ? visBuffer.GetMotionResource()
                                : nullptr,
                usingVisibility ? visBuffer.GetMotionRTV()
                                : D3D12_CPU_DESCRIPTOR_HANDLE{},
                waterDepthState, usingVisibility ? &g_profiler : nullptr);
        }

        if (renderedScene && TransparencyQueueActiveDX12()) {
            bindSplitTransparencyTarget();
            const WaterTransparencyPhaseDX12 phase =
                WaterTransparencySplitActiveDX12()
                    ? WaterTransparencyPhaseDX12::AfterWater
                    : WaterTransparencyPhaseDX12::All;
            {
                ProfilerDX12::Scope profile(
                    g_profiler, "Transparency After Water",
                    g_dx12.commandList.Get());
                RenderWaterTransparencyPhaseDX12(
                    scene, mainShader, phase,
                    fogShadowResource);
            }
            {
                // The matching same-side half. Opposite-side particles were
                // included in the water scene copy above and are refracted;
                // these remain sharp because no water lies between them and
                // the camera.
                ProfilerDX12::Scope profile(
                    g_profiler, "Impact Particles After Water",
                    g_dx12.commandList.Get());
                mainShader.InvalidateGraphicsRootBinding();
                RenderImpactBillboards(
                    scene, mainShader, geo, fogLightSpace,
                    phase);
            }
            releaseSplitTransparencyDepth();
            EndWaterTransparencySplitDX12();
        }

        // Rain. After the opaque scene and the water so it depth-tests against
        // both, and before the fog so the fog tints it like anything else out
        // there -- rain drawn after the fog would hang in front of a fogged
        // hillside instead of receding into it.
        if (renderedScene && !bentGTAODiagnosticActive &&
            scene.rainIntensity > 0.001f && g_rainRenderer.initialized) {
            ProfilerDX12::Scope profile(
                g_profiler, "Rain", g_dx12.commandList.Get());
            const bool hdrTarget = commonHDRValidationTarget;
            const bool rainMSAA = !hdrTarget && msaaActive;
            // Bind colour + depth explicitly. The water pass above leaves a
            // target bound with no depth buffer, and rain has to test against
            // the scene or it falls through walls and terrain.
            //
            // On the visibility path this must go through EndMotionDraws
            // rather than a hand-rolled OMSetRenderTargets. When extension
            // motion vectors are on, two RTVs are bound, and D3D12 silently
            // drops any draw whose PSO render-target count disagrees with the
            // binding -- a single-RT pass like this one produces no pixels and
            // no error. EndMotionDraws is the engine's own "restore colour
            // only" and is a no-op when the toggle is off.
            if (rainMSAA) {
                msaa.Bind();
            } else if (hdrTarget) {
                visBuffer.EndMotionDraws(g_dx12.commandList.Get());
                g_dx12.commandList->RSSetViewports(1, &g_dx12.viewport);
                g_dx12.commandList->RSSetScissorRects(1, &g_dx12.scissorRect);
            } else {
                D3D12_CPU_DESCRIPTOR_HANDLE rainRTV = GetCPUDescriptorHandle(
                    g_dx12.rtvHeap.Get(), g_dx12.rtvDescriptorSize,
                    g_dx12.frameIndex);
                D3D12_CPU_DESCRIPTOR_HANDLE rainDSV =
                    g_dx12.dsvHeap->GetCPUDescriptorHandleForHeapStart();
                g_dx12.commandList->OMSetRenderTargets(
                    1, &rainRTV, FALSE, &rainDSV);
                g_dx12.commandList->RSSetViewports(1, &g_dx12.viewport);
                g_dx12.commandList->RSSetScissorRects(1, &g_dx12.scissorRect);
            }
            g_rainRenderer.Render(
                scene, gameTimer.GetElapsed(), scene.rainIntensity,
                { scene.windVelocity.x, 0.0f, scene.windVelocity.y },
                hdrTarget, rainMSAA, g_dx12.frameIndex);
        }

        // Restore whenever either lift was applied -- gating this on the NVG
        // flag alone would leave a lightning flash permanently baked into the
        // scene's ambient on any frame the goggles were stowed.
        if (nvgWorldAmbientBoosted || lightningLit)
            scene.ambientLightingIntensity =
                authoredAmbientLightingIntensity;

        const bool visibilityValidation = visibilityParityValidation;
        // Shared post-render pass. Normal forward composites to the resolved
        // swapchain target. Visibility and parity-forward composite to HDR.
        // Light shafts are suppressed under the goggles. A shaft is a cone of
        // lit atmosphere, and the tube gain amplifies it far harder than the
        // surfaces around it -- it came through as a bright wedge hanging in
        // front of the muzzle rather than as a god ray. Thinning the fog alone
        // did not fix it because the shaft pass has its own scattering term.
        const bool nvgSuppressShafts = nvgWorldAmbientBoosted &&
            g_nightVisionBlend > 0.35f;
        const bool volumetricShafts =
            scene.lightShaftMode == Scene::LightShaftMode::Volumetric &&
            !nvgSuppressShafts;
        const bool fauxShafts =
            scene.lightShaftMode == Scene::LightShaftMode::Faux &&
            !nvgSuppressShafts;
        const bool renderFroxelVolume =
            ((scene.enableVolumetricFog && volumetricShafts) ||
             scene.enableFlyableClouds) && !editorHideFog;
        if (renderedScene && !bentGTAODiagnosticActive &&
            !deploymentHideAtmosphere &&
            renderFroxelVolume && volumetricFog.initialized) {
            ProfilerDX12::Scope profile(
                g_profiler, "Volumetric Fog / Clouds",
                g_dx12.commandList.Get());
            ID3D12Resource* fogDepth =
                msaaActive ? msaa.GetDepthResource()
                           : g_dx12.depthStencilBuffer.Get();
            bool fogDepthMSAA = msaaActive;
            bool fogDepthAlreadyReadable = false;
            if (grassMSAAActive && grassMSAA.GetCombinedDepthResource()) {
                fogDepth = grassMSAA.GetCombinedDepthResource();
                fogDepthMSAA = false;
                fogDepthAlreadyReadable = true;
            }
            volumetricFog.Render(scene, g_ocean, fogLightSpace, fogShadowResource,
                fogDepth, fogDepthMSAA,
                commonHDRValidationTarget ? visBuffer.GetOutputRTV()
                                          : D3D12_CPU_DESCRIPTOR_HANDLE{},
                commonHDRValidationTarget, fogDepthAlreadyReadable);
        }
        // Restore the authored density now the fog pass has consumed it. Must be
        // after volumetricFog.Render, not beside the ambient restore above --
        // that runs before this point, so restoring there put the full density
        // back before the fog ever read the thinned value.
        if (nvgFogThinned)
            scene.volumetricFogDensity = authoredVolumetricFogDensity;
        // Faux shafts need the HDR target: they read the scene colour they are
        // about to add onto, so the visibility buffer's output is both source
        // and destination. Only run on the HDR path where that texture exists.
        if (renderedScene && !bentGTAODiagnosticActive && fauxShafts &&
            lightShafts.initialized &&
            commonHDRValidationTarget) {
            ProfilerDX12::Scope profile(
                g_profiler, "Light Shafts", g_dx12.commandList.Get());
            ID3D12Resource* shaftDepth = g_dx12.depthStencilBuffer.Get();
            bool shaftDepthReadable = false;
            if (grassMSAAActive && grassMSAA.GetCombinedDepthResource()) {
                shaftDepth = grassMSAA.GetCombinedDepthResource();
                shaftDepthReadable = true;
            }
            lightShafts.Render(scene, g_dx12.commandList.Get(), shaftDepth,
                visBuffer.GetOutputRTV(), shaftDepthReadable);
        }
        if (renderedScene && usingVisibility &&
            !bentGTAODiagnosticActive &&
            scene.waterQuality == WaterQuality::Ultra) {
            ProfilerDX12::Scope profile(
                g_profiler, "Water Underwater Composite",
                g_dx12.commandList.Get());
            ID3D12Resource* underwaterDepth =
                g_dx12.depthStencilBuffer.Get();
            D3D12_RESOURCE_STATES underwaterDepthState =
                D3D12_RESOURCE_STATE_DEPTH_WRITE;
            if (grassMSAAActive && grassMSAA.GetCombinedDepthResource()) {
                underwaterDepth = grassMSAA.GetCombinedDepthResource();
                underwaterDepthState =
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            }
            waterRenderer.RenderUnderwater(
                scene, g_ocean, visBuffer.GetOutputResource(),
                visBuffer.GetOutputRTV(), underwaterDepth,
                g_specularEnvironmentResource, underwaterDepthState);
        }
        // Every pass that reads the clustered light list has now run.
        RemoveVehicleHeadlights(scene, headlightsInLightList);

        if (commonHDRValidationTarget) {
            ProfilerDX12::Scope profile(
                g_profiler, "Common HDR Post", g_dx12.commandList.Get());
            visBuffer.EndForwardExtensions(g_dx12.commandList.Get());
            if (usingVisibility && !visBuffer.validationMode &&
                !bentGTAODiagnosticActive)
                visBuffer.UpdateExposure(g_dx12.commandList.Get());
            // TAA reprojection remains valid during ordinary camera movement;
            // HZB history is intentionally stricter and must not gate it.
            visBuffer.PostProcess(g_dx12.commandList.Get(), usingVisibility);
            visBuffer.CopyToBackBuffer(g_dx12.commandList.Get());
            if (usingVisibility)
                visBuffer.TransitionBuffersForUpload(g_dx12.commandList.Get());
            mainShader.SetHDRTargetEnabled(false);
            g_meshShader.SetHDRTargetEnabled(false);
            g_terrain.SetHDRTargetEnabled(false);
            mainShader.SetExtensionMotionEnabled(false);
            g_meshShader.SetExtensionMotionEnabled(false);
        }

        // Preserve this frame's depth for next-frame amplification-shader
        // occlusion tests before UI rendering changes descriptor heaps.
        if (hzbCaptureActive) {
            ProfilerDX12::Scope profile(g_profiler, "Occlusion Depth", g_dx12.commandList.Get());
            occlusionDepth.PrepareCapture(g_dx12.commandList.Get());
        }

        if (scene.enableFXAA && fxaa.initialized && !visibilityValidation &&
            !bentGTAODiagnosticActive && g_nightVisionBlend <= 0.001f) {
            ProfilerDX12::Scope profile(g_profiler, "FXAA", g_dx12.commandList.Get());
            fxaa.Apply(g_dx12.commandList.Get());
        }

        // Night vision runs before ImGui so the HUD stays legible through it.
        // FXAA is deliberately bypassed while the tube is visible: its grain
        // hides small edge stair-steps, while prefiltering the source destroyed
        // the fine silhouettes and surface detail the goggles are meant to find.
        //
        // The ramp is driven here rather than in ProcessInput because that
        // function returns early while the UI has the keyboard or the camera is
        // locked -- the goggles would freeze half-raised on any frame the menu
        // was open. Dropping is quicker than raising: pulling goggles off the
        // eye is a faster motion than settling them onto it.
        {
            const bool nvgEquipped =
                g_game.mission.Loadout().gear == GearType::NightVisionGoggles;
            // Losing the goggles (death, or a loadout without them) stows them
            // rather than leaving the screen green.
            if (!nvgEquipped || scene.player.health <= 0.0f)
                g_nightVisionActive = false;
            // Same for the weapon light: a dead player's rifle should not go on
            // lighting the ground it fell on.
            if (g_game.mission.Loadout().gear != GearType::Flashlight ||
                scene.player.health <= 0.0f)
                g_flashlightActive = false;
            const float rampRate = g_nightVisionActive ? 4.0f : 7.0f;
            const float target = g_nightVisionActive ? 1.0f : 0.0f;
            g_nightVisionBlend +=
                std::clamp(target - g_nightVisionBlend,
                           -rampRate * deltaTime, rampRate * deltaTime);
            g_nightVisionBlend = std::clamp(g_nightVisionBlend, 0.0f, 1.0f);

            // Automatic gain control. A real intensifier constantly retunes its
            // amplification to the light reaching it, over roughly a second --
            // it is why goggles bloom out when you step into a lit room and then
            // settle, and why they wind up to maximum in true darkness.
            //
            // Driven from the scene's own lighting rather than a GPU luminance
            // readback: a readback would cost a compute pass and land a frame or
            // more late, and the ambient level plus the directional sun is
            // already exactly the quantity the tube would be responding to.
            {
                // Scene brightness as the tube sees it. The directional term is
                // weighted down because at night the sun is below the horizon
                // and contributes almost nothing, while ambient is what actually
                // fills the scene.
                float sceneBrightness =
                    scene.ambientLightingIntensity +
                    scene.directionalLightIntensity * 0.04f;
                // A muzzle flash is a genuine light source going off in the
                // player's face. Real goggles bloom hard off their own weapon,
                // and the AGC ducks for a moment afterwards.
                if (scene.muzzleFlashTime > 0.0f) sceneBrightness += 0.55f;

                // Transient flashes -- explosions, lightning -- decay fast.
                // Faster than the AGC recovers, so the tube is still stopped
                // down for a moment after the light itself has gone, which is
                // the after-image that makes a blast actually cost you.
                g_nightVisionLightSpike = (std::max)(0.0f,
                    g_nightVisionLightSpike - deltaTime * 3.2f);
                if (g_nightVisionLightSpike > 0.0f) {
                    // Inverse-square falloff, floored so a distant flash still
                    // registers faintly rather than snapping to nothing.
                    const float fx =
                        g_nightVisionSpikeOrigin.x - scene.camera.Position.x;
                    const float fy =
                        g_nightVisionSpikeOrigin.y - scene.camera.Position.y;
                    const float fz =
                        g_nightVisionSpikeOrigin.z - scene.camera.Position.z;
                    const float distanceSq = fx * fx + fy * fy + fz * fz;
                    // 8 m rather than a wider reference: at 18 m a grenade
                    // going off 40 m away still pinned the tube shut as hard as
                    // one at your feet, because the flash is bright enough to
                    // saturate the gain curve well past the falloff. Tight
                    // enough that a blast across the compound dims the image
                    // instead of blinding you.
                    constexpr float kReferenceRange = 8.0f;
                    const float falloff = kReferenceRange * kReferenceRange /
                        (kReferenceRange * kReferenceRange + distanceSq);
                    sceneBrightness += g_nightVisionLightSpike * falloff;
                }

                // Target gain: wide open in darkness, stopped down in light.
                // The curve is inverse rather than linear because the tube is
                // compensating for brightness, so halving the light roughly
                // doubles the gain until it hits the tube's ceiling.
                // Calibrated against the authored presets, whose brightness by
                // this measure is Night 0.005, Dusk 0.68, Afternoon 0.91,
                // Noon 1.10. The constant sets where the curve sits: too small
                // and even Night stops down below the old fixed 7.5, making
                // nights darker than before the AGC existed.
                constexpr float kMaxGain = 11.0f;
                constexpr float kMinGain = 1.35f;
                const float targetGain = std::clamp(
                    0.115f / (sceneBrightness + 0.008f), kMinGain, kMaxGain);
                // Asymmetric time constant, like the real thing: protecting the
                // tube from a sudden bright source is fast, recovering
                // sensitivity afterwards is slow. That asymmetry is what makes
                // walking out of a lit area leave you briefly blind.
                const float agcRate =
                    targetGain < g_nightVisionGain ? 6.5f : 1.15f;
                g_nightVisionGain +=
                    (targetGain - g_nightVisionGain) *
                    (std::min)(1.0f, agcRate * deltaTime);

                // Overload: how far past the tube's usable range the scene is.
                // Daylight drives this toward 1 and washes the image out.
                // Onset sits above Dusk (0.68) so twilight is degraded but still
                // usable -- dusk is when goggles start earning their place, and
                // washing them out there would be backwards. Full overload lands
                // past Afternoon, so only real daylight makes them useless,
                // which is what the deployment briefing promises.
                constexpr float kOverloadOnset = 0.72f;
                constexpr float kOverloadFull = 1.05f;
                g_nightVisionOverload = std::clamp(
                    (sceneBrightness - kOverloadOnset) /
                        (kOverloadFull - kOverloadOnset),
                    0.0f, 1.0f);
            }

            if (g_nightVisionBlend > 0.001f && nightVision.initialized &&
                !visibilityValidation && !bentGTAODiagnosticActive) {
                ProfilerDX12::Scope profile(
                    g_profiler, "Night Vision", g_dx12.commandList.Get());
                // Wrapped: the shader feeds this to sin(), which loses its
                // grain-scrambling precision once the value grows large.
                const float noiseTime =
                    std::fmod(gameTimer.GetElapsed(), 1000.0f);
                nightVision.Apply(g_dx12.commandList.Get(), noiseTime,
                                  g_nightVisionBlend, g_nightVisionGain,
                                  g_nightVisionOverload,
                                  scene.cameraNear,
                                  scene.EffectiveCameraFarPlane());
            }
        }

        // Ensure ImGui renders to the swapchain backbuffer (VB path changes OM target)
        {
            D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = GetCPUDescriptorHandle(
                g_dx12.rtvHeap.Get(), g_dx12.rtvDescriptorSize, g_dx12.frameIndex);
            D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = g_dx12.dsvHeap->GetCPUDescriptorHandleForHeapStart();
            g_dx12.commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);
            g_dx12.commandList->RSSetViewports(1, &g_dx12.viewport);
            g_dx12.commandList->RSSetScissorRects(1, &g_dx12.scissorRect);
        }

        // ?? ImGui ??
        {
        ProfilerDX12::Scope profile(g_profiler, "ImGui", g_dx12.commandList.Get());
        g_forwardDrawCalls = mainShader.currentDrawCall +
            g_particleRenderer.DrawCallsThisFrame();
        // The visibility path renders shadows too (main.cpp's visibility branch
        // calls shadowMap.Render), so gating this on !usingVisibility made the
        // HUD report "Shadow 0" while three cascades were actually drawing.
        g_shadowDrawCalls = (!usingRaytracing &&
            scene.enableShadows && shadowMap.initialized && scene.lightType == 0)
            ? shadowMap.depthShader.currentDrawCall : 0;
        g_shadowBatches = g_shadowDrawCalls
            ? shadowMap.depthShader.batchesThisFrame : 0;
        g_shadowBatchInstances = g_shadowDrawCalls
            ? shadowMap.depthShader.instancesThisFrame : 0;
        g_shadowCachedFarCascades = (!usingRaytracing &&
            scene.enableShadows && scene.lightType == 0)
            ? shadowMap.CachedCascadesThisFrame() : 0;
        g_shadowRefreshedFarCascades = (!usingRaytracing &&
            scene.enableShadows && scene.lightType == 0)
            ? shadowMap.RefreshedCascadesThisFrame() : 0;
        g_visibilityDrawCalls = usingVisibility ? visBuffer.currentDrawCall : 0;
        if (visibilitySmokeEnabled && usingVisibility &&
            !visibilitySmokeReported &&
            (!temporalGTAOSmokeTest ||
             visBuffer.BentNormalGTAOAppliedLastResolve())) {
            std::ofstream("visibility_smoke.log", std::ios::app)
                << "active draws=" << visBuffer.currentDrawCall
                << " materials=" << visBuffer.materialCount
                << " textures=" << visBuffer.materialTextureCount
                << " vertices=" << visBuffer.persistentVertexCount
                << " indices=" << visBuffer.persistentIndexCount
                << " debug=" << visBuffer.debugViewMode
                << " validation=" << (visBuffer.validationMode ? 1 : 0)
                << " spot_cache=" << (scene.cacheSpotShadows ? 1 : 0)
                << " spot_cached=" << shadowMap.cachedSpotSlicesThisFrame
                << " spot_refreshed=" << shadowMap.refreshedSpotSlicesThisFrame
                << " bent_gtao="
                << (visBuffer.BentNormalGTAOAppliedLastResolve() ? 1 : 0)
                << " bent_debug="
                << static_cast<UINT>(visBuffer.bentNormalGTAODebugMode)
                << '\n';
            visibilitySmokeReported = true;
        }
        // SGE_PROFILE_DUMP=1 writes one frame of GPU pass timings to disk after
        // the scene has settled. The profiler is otherwise UI-only, which makes
        // per-pass numbers unreadable from an automated/headless run.
        if (g_profileDumpEnabled && !g_profileDumpWritten &&
            IsSceneScreen() && !g_game.loading.Active()) {
            if (++g_profileDumpFrame > 240) {
                std::ofstream dump("profile_dump.log", std::ios::trunc);
                dump << "GPU frame: " << g_profiler.GpuFrameMs()
                     << " ms\nCPU frame: " << g_profiler.CpuFrameMs()
                     << " ms\nCPU wait: " << g_profiler.CpuFrameWaitMs()
                     << " ms (present + fence)\nCPU wall: "
                     << g_profiler.CpuFrameWallMs()
                     << " ms\n--- GPU passes ---\n";
                double total = 0.0;
                for (const auto& sample : g_profiler.GpuSamples()) {
                    dump << sample.name << ": " << sample.milliseconds << " ms\n";
                    total += sample.milliseconds;
                }
                dump << "--- sum of passes: " << total << " ms ---\n";
                g_profileDumpWritten = true;
            }
        }
        // Repeatable terrain A/B: run the same settled Level-1 view once with
        // SGE_TERRAIN_ERROR_LOD absent and once with it set. Separate launches
        // avoid changing topology inside an in-flight frame, while identical
        // warm-up/sample counts keep shader caches and GPU clocks comparable.
        if (terrainLODBenchmark && !terrainLODBenchmarkComplete &&
            g_game.session.Screen() == GameScreen::Level1 &&
            !g_game.loading.Active()) {
            ++terrainLODBenchmarkFrames;
            if (terrainLODBenchmarkFrames > 240) {
                const double vbTerrain = g_profiler.GpuScopeMs("VB Terrain");
                const double forwardTerrain = g_profiler.GpuScopeMs("Terrain");
                const double terrainMs = vbTerrain > 0.0 ? vbTerrain : forwardTerrain;
                if (terrainMs > 0.0 && g_profiler.GpuFrameMs() > 0.0) {
                    terrainLODScopeSamples.push_back(terrainMs);
                    terrainLODFrameSamples.push_back(g_profiler.GpuFrameMs());
                    terrainLODShadowSamples.push_back(
                        g_profiler.GpuScopeMs("Shadow"));
                }
            }
            if (terrainLODScopeSamples.size() >= 300) {
                const char* filename = g_forceTerrainErrorLOD
                    ? "terrain_lod_adaptive.log" : "terrain_lod_baseline.log";
                std::ofstream log(filename, std::ios::trunc);
                log << "mode=" << (g_forceTerrainErrorLOD ? "adaptive" : "baseline")
                    << '\n' << "samples=" << terrainLODScopeSamples.size() << '\n';
                auto writeStats = [&log](const char* label,
                                         const std::vector<double>& source) {
                    std::vector<double> values = source;
                    std::sort(values.begin(), values.end());
                    double sum = 0.0;
                    for (double value : values) sum += value;
                    const size_t p50 = (values.size() - 1) / 2;
                    const size_t p95 = static_cast<size_t>(
                        static_cast<double>(values.size() - 1) * 0.95);
                    log << label << "_avg_ms=" << sum / values.size() << '\n'
                        << label << "_p50_ms=" << values[p50] << '\n'
                        << label << "_p95_ms=" << values[p95] << '\n';
                };
                writeStats("terrain", terrainLODScopeSamples);
                writeStats("frame", terrainLODFrameSamples);
                writeStats("shadow", terrainLODShadowSamples);
                terrainLODBenchmarkComplete = true;
                PostQuitMessage(0);
            }
        }
        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        g_thumbnailUploadedThisFrame = false;
        if (g_game.loading.Active()) {
            RenderLoadingScreen();
        } else if (g_game.session.Screen() == GameScreen::MainMenu) {
            RenderMainMenu(hwnd);
            // Same gap the editor branch had: the menu never calls RenderUI(),
            // so the profiler window it owns was unreachable here. The menu is
            // where the VRAM readout is worth the most -- it is the one screen
            // with no level resident, so whatever it reports is the floor every
            // level load builds on top of.
            if (ImGui::IsKeyPressed(ImGuiKey_F8, false))
                g_showProfilerWindow = !g_showProfilerWindow;
            DrawProfilerWindow();
        } else if (g_game.session.Screen() == GameScreen::WinScreen) {
            RenderWinScreen(hwnd);
        } else if (g_game.session.Screen() == GameScreen::LevelEditor) {
            const DXRDDGIRenderer::Status& ddgiStatus = g_dxrDDGI.GetStatus();
            g_levelEditor.SetDXRDDGIStatus({
                ddgiStatus.dxrSupported, ddgiStatus.updatesActive,
                ddgiStatus.probeCount,
                ddgiStatus.raysPerFrame, ddgiStatus.gpuMemoryBytes,
                ddgiStatus.cacheStatus
            });
            LevelEditorActions actions;
            {
                ProfilerDX12::CpuScope editorUIProfile(g_profiler, "Editor/UI");
                actions = g_levelEditor.Render(scene.camera,
                    scene.GetViewMatrix(), scene.GetProjectionMatrix(),
                    [](float x, float z) {
                        if (!scene.useMeshTerrain || !g_terrain.supported) return 0.0f;
                        auto params = CurrentTerrainParams();
                        params.heightScale = scene.terrainHeightScale;
                        return TerrainRendererDX12::HeightAt(params, x, z);
                    }, PrefabThumbnailTexture);
            }
            g_game.commands.Set(
                GameCommand::EditorBeginPlay, actions.beginPlay);
            g_game.commands.Set(
                GameCommand::EditorStopPlay, actions.stopPlay);
            g_game.commands.Set(
                GameCommand::EditorReturnToMenu, actions.returnToMenu);
            g_editorFullReconcileRequested |= actions.fullReconcile;
            // Manual viewport refresh. Re-scans the prefab and asset registries
            // so a model added on disk since the editor opened is picked up,
            // then asks for the visual rebuild that actually loads what is not
            // cached -- the per-edit sync path skips loading to stay cheap, and
            // that is what leaves a freshly placed house invisible.
            //
            // Only the flag is set here. The rebuild itself has to run at the
            // top of the next frame, before any pass binds the resources it
            // replaces: doing it from this late ImGui phase destroys models the
            // open command list still references.
            if (actions.refreshVisuals) {
                g_assetRegistry.Refresh();
                g_prefabRegistry.Refresh(kPrefabRoot, kModelRoot);
                g_editorVisualRefreshRequested = true;
            }
            if (actions.rebuildDXRDDGI)
                g_game.commands.Request(GameCommand::RebuildDDGI);
            if (actions.resetDXRDDGIHistory)
                g_game.commands.Request(GameCommand::ResetDDGIHistory);
            // The editor branch never calls RenderUI(), so the profiler window
            // it owns was unreachable while editing -- the one screen where the
            // per-stage editor timings actually matter. F8 toggles it here.
            if (ImGui::IsKeyPressed(ImGuiKey_F8, false))
                g_showProfilerWindow = !g_showProfilerWindow;
            DrawProfilerWindow();
            DrawDXRDDGIProbeDebug(
                scene.GetViewMatrix(), scene.GetProjectionMatrix());
            // Authoring aid, so it is hidden during a playtest: that view has to
            // show the level the way the player will get it.
            if (!g_levelEditor.IsPlaying() && g_levelEditor.NavmeshEnabled())
                DrawNavmeshDebug(
                    scene.GetViewMatrix(), scene.GetProjectionMatrix());
            if (g_levelEditor.IsPlaying()) {
                RenderPlayerHUD(scene);
                DrawArmoryShopPrompt(
                    scene.GetViewMatrix(), scene.GetProjectionMatrix());
                RenderArmoryShopPanel(hwnd);
                DrawTravelPrompt(
                    scene.GetViewMatrix(), scene.GetProjectionMatrix());
                RenderTravelPanel(hwnd);
                DrawEnemyVisionCones(scene.GetViewMatrix(), scene.GetProjectionMatrix());
            }
        } else {
            if (!g_insertionChoicePending) {
                RenderPlayerHUD(scene);
                DrawEscapeBoatMarker(
                    scene.GetViewMatrix(), scene.GetProjectionMatrix());
                DrawMarineFriendlyMarkers(
                    scene.GetViewMatrix(), scene.GetProjectionMatrix());
                DrawImpactDecalDebug(
                    scene.GetViewMatrix(), scene.GetProjectionMatrix());
                DrawWeaponPickupPrompt(
                    scene.GetViewMatrix(), scene.GetProjectionMatrix());
                DrawArmoryShopPrompt(
                    scene.GetViewMatrix(), scene.GetProjectionMatrix());
                RenderArmoryShopPanel(hwnd);
                DrawTravelPrompt(
                    scene.GetViewMatrix(), scene.GetProjectionMatrix());
                RenderTravelPanel(hwnd);
                DrawEnemyVisionCones(
                    scene.GetViewMatrix(), scene.GetProjectionMatrix());
            }
            if (g_ddgiCornellTestMode) {
                const DXRDDGIRenderer::Status& status = g_dxrDDGI.GetStatus();
                ImGui::SetNextWindowPos(ImVec2(18.0f, 18.0f), ImGuiCond_Always);
                ImGui::SetNextWindowBgAlpha(0.82f);
                ImGui::Begin("DDGI Cornell Status", nullptr,
                    ImGuiWindowFlags_AlwaysAutoResize |
                    ImGuiWindowFlags_NoMove |
                    ImGuiWindowFlags_NoCollapse |
                    ImGuiWindowFlags_NoInputs);
                ImGui::TextColored(
                    status.updatesActive ? ImVec4(0.25f, 1.0f, 0.35f, 1.0f)
                                         : ImVec4(1.0f, 0.35f, 0.2f, 1.0f),
                    status.updatesActive ? "DXR DDGI: UPDATING"
                                         : "DXR DDGI: UNAVAILABLE");
                ImGui::Text("Probes: %u   Rays/frame: %u",
                    status.probeCount, status.raysPerFrame);
                ImGui::TextDisabled("Low ambient exposes indirect bounce.");
                ImGui::End();
                DrawDXRDDGIProbeDebug(
                    scene.GetViewMatrix(), scene.GetProjectionMatrix());
            }
            if (!scene.player.godMode && scene.player.health <= 0.0f) {
                RenderDeathScreen(hwnd);
            } else if (g_insertionChoicePending) {
                // Takes over the frame the same way the death screen does: the
                // insertion runs are held until this is answered.
                RenderInsertionChoiceScreen(hwnd);
            } else {
                if (showUI) RenderUI(scene, visBuffer);
                DrawDestructionDebug(scene);
                DrawRagdollPhysicsDebug(scene);
            }
        }
        ImGui::Render();

        ID3D12DescriptorHeap* heaps[] = { imguiSrvHeap.Get() };
        g_dx12.commandList->SetDescriptorHeaps(1, heaps);
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_dx12.commandList.Get());
        }

        // ?? end frame ??
        g_profiler.EndGpuFrame(g_dx12.commandList.Get());
        // Present reads the interval off g_dx12, so mirror the scene setting
        // here rather than only when the slider moves -- that way a value
        // loaded with a level applies too.
        g_dx12.syncInterval = static_cast<UINT>(
            (std::max)(0, (std::min)(4, scene.vsyncInterval)));
        try { EndFrame(); }
        catch (const std::exception& e) {
            const HRESULT removedReason = g_dx12.device
                ? g_dx12.device->GetDeviceRemovedReason() : S_OK;
            g_profiler.EndCpuFrame();
            std::cerr << "EndFrame: " << e.what()
                      << " deviceRemovedReason=0x" << std::hex
                      << static_cast<unsigned long>(removedReason) << std::dec << "\n";
            {
                std::ofstream crashLog("engine_runtime_error.log", std::ios::app);
                crashLog << "EndFrame: " << e.what() << " deviceRemovedReason=0x"
                         << std::hex << static_cast<unsigned long>(removedReason)
                         << std::dec << '\n';
                // DEVICE_REMOVED/DEVICE_HUNG says only that the GPU stopped.
                // The breadcrumbs name the op it stopped on.
                if (FAILED(removedReason)) {
                    DumpDX12DeviceRemovedData(crashLog);
                    DumpDX12DeviceRemovedData(std::cerr);
                }
            }
            DumpDX12DebugMessages();
            break;
        }
        occlusionDepth.SubmitCopy();
        if (hzbCaptureActive)
            previousHZBViewProjection =
                scene.GetViewMatrix() * scene.GetProjectionMatrix();
        msaaUsedLastFrame = msaaActive;
        g_profiler.EndCpuFrame();
        LogFrameSpike(deltaTime);
        if (molotovSmokeInjected && ++molotovSmokeFrames >= 240) {
            std::ofstream smoke("molotov_smoke.log", std::ios::trunc);
            smoke << "peak_patches=" << molotovSmokePeakPatches << '\n'
                  << "active_patches=" << scene.firePatches.size() << '\n'
                  << "particles=" << scene.impactParticles.size() << '\n';
            PostQuitMessage(molotovSmokePeakPatches >= 10 ? 0 : 3);
        }
        if (vortexSmokeInjected && ++vortexSmokeFrames >= 60 &&
            scene.vortexFX.empty()) {
            const size_t remainingBarrelBodies = static_cast<size_t>(std::count_if(
                scene.explosiveBarrels.begin(), scene.explosiveBarrels.end(),
                [](const ExplosiveBarrel& barrel) {
                    return barrel.physicsHandle != 0;
                }));
            std::ofstream smoke("vortex_smoke.log", std::ios::trunc);
            smoke << "peak_fx=" << vortexSmokePeakFX << '\n'
                  << "actors_before=" << vortexSmokeActorsBefore << '\n'
                  << "peak_actors=" << vortexSmokePeakActors << '\n'
                  << "peak_barrel_bodies=" << vortexSmokePeakBarrelBodies << '\n'
                  << "remaining_barrel_bodies=" << remainingBarrelBodies << '\n'
                  << "active_fx=" << scene.vortexFX.size() << '\n';
            PostQuitMessage(
                vortexSmokePeakFX > 0 && vortexSmokePeakBarrelBodies > 0 &&
                remainingBarrelBodies == 0 ? 0 : 3);
        }
        if (particleBenchmark && IsSceneScreen() && !g_game.loading.Active()) {
            ++particleBenchmarkFrames;
            if (particleBenchmarkFrames > 120) {
                double cpuMs = -1.0;
                double gpuMs = -1.0;
                for (const ProfilerSampleDX12& sample : g_profiler.CpuSamples())
                    if (sample.name == "Impact Particles") cpuMs = sample.milliseconds;
                for (const ProfilerSampleDX12& sample : g_profiler.GpuSamples())
                    if (sample.name == "Impact Particles") gpuMs = sample.milliseconds;
                if (cpuMs >= 0.0 && gpuMs >= 0.0) {
                    particleBenchmarkCpuMs += cpuMs;
                    particleBenchmarkGpuMs += gpuMs;
                    ++particleBenchmarkSamples;
                }
            }
            if (particleBenchmarkSamples >= 300) {
                std::ofstream benchmark("particle_benchmark.log", std::ios::trunc);
                benchmark << "particles=800\n"
                          << "samples=" << particleBenchmarkSamples << '\n'
                          << "cpu_ms=" << particleBenchmarkCpuMs /
                                particleBenchmarkSamples << '\n'
                          << "gpu_ms=" << particleBenchmarkGpuMs /
                                particleBenchmarkSamples << '\n'
                          << "forward_draws=" << g_forwardDrawCalls << '\n';
                PostQuitMessage(0);
            }
        }
        // Stage-2 gate. The BVH statistics are logged by LoadPrefabModel; what
        // is checked here is that the tree actually reached the cache and that
        // a ray fired straight down through the airport's own bounds hits
        // geometry, which is the cheapest proof the triangles landed in the
        // right space.
        if (g_collisionSmokeEnabled && !g_collisionSmokeChecked &&
            !g_prefabRebuildRequested) {
            const auto cached = g_prefabModelCache.find("props/airport");
            // A level without the airport still runs the budget report below;
            // only the airport-specific geometry checks are skipped.
            const bool airportPresent = cached != g_prefabModelCache.end();
            const bool built = airportPresent &&
                               cached->second.collisionMesh &&
                               !cached->second.collisionMesh->Empty();
            bool hit = false;
            // World-space height of the surface the segment helper finds, reused
            // below as the capsule probe's footing.
            float probeFloorY = 0.0f;
            if (built) {
                const CollisionMesh& mesh = *cached->second.collisionMesh;
                const CollisionMesh::Node& root = mesh.nodes[0];
                // Straight down the middle of the model, from above the roof to
                // below the floor: any closed structure must be struck.
                const XMFLOAT3 start(
                    (root.boundsMin[0] + root.boundsMax[0]) * 0.5f,
                    root.boundsMax[1] + 1.0f,
                    (root.boundsMin[2] + root.boundsMax[2]) * 0.5f);
                const XMFLOAT3 end(start.x, root.boundsMin[1] - 1.0f, start.z);
                CollisionMeshRayHit result;
                hit = CollisionMeshRaycast(mesh, start, end, 0.0f, result);
                if (hit) {
                    SGE_LOG("LogPrefab", EngineLog::Level::Display,
                        "Collision probe hit at y=" + std::to_string(result.point.y) +
                        " normal y=" + std::to_string(result.normal.y) +
                        " tri=" + std::to_string(result.triangle));
                }
            }
            // R9: the same cast through the shared segment helper, which is the
            // path bullets take. If the bounds box still shadowed the mesh, this
            // would report a hit on the box's flat top rather than on geometry,
            // so compare the two against each other.
            bool helperAgrees = false;
            bool instanced = false;
            if (built) {
                const auto placed = std::find_if(g_prefabMeshColliders.begin(),
                    g_prefabMeshColliders.end(),
                    [&](const CollisionMeshInstance& value) {
                        return value.mesh == cached->second.collisionMesh.get();
                    });
                instanced = placed != g_prefabMeshColliders.end();
                if (instanced) {
                    const XMFLOAT3 start(
                        (placed->worldBoundsMin.x + placed->worldBoundsMax.x) * 0.5f,
                        placed->worldBoundsMax.y + 5.0f,
                        (placed->worldBoundsMin.z + placed->worldBoundsMax.z) * 0.5f);
                    const XMFLOAT3 end(start.x, placed->worldBoundsMin.y - 5.0f,
                                       start.z);
                    XMFLOAT3 point{};
                    XMFLOAT3 normal(0.0f, 0.0f, 0.0f);
                    uint64_t entity = 0;
                    if (HitPrefabColliderSegment(start, end, 0.02f, point,
                                                 &entity, &normal)) {
                        // A box hit leaves the seeded normal untouched; only the
                        // triangle path writes one.
                        const float length = std::sqrt(
                            normal.x * normal.x + normal.y * normal.y +
                            normal.z * normal.z);
                        helperAgrees = length > 0.5f &&
                                       point.y < placed->worldBoundsMax.y - 0.001f;
                        probeFloorY = point.y;
                        SGE_LOG("LogPrefab", EngineLog::Level::Display,
                            "Segment helper hit entity " + std::to_string(entity) +
                            " at y=" + std::to_string(point.y) +
                            " (box top y=" + std::to_string(placed->worldBoundsMax.y) +
                            ") normal length " + std::to_string(length));
                    }
                }
            }
            // Exercise the debug overlay's line builder here too: it reads the
            // same collections, so a smoke run confirms it produces geometry
            // without needing someone to tick the checkbox by hand.
            {
                const bool previous = g_showCollisionDebug;
                g_showCollisionDebug = true;
                BuildCollisionDebugLines();
                SGE_LOG("LogPrefab", EngineLog::Level::Display,
                    "Collision debug overlay queued " +
                    std::to_string(g_collisionDebugRenderer.LineCount()) +
                    " lines for " + std::to_string(g_prefabColliders.size()) +
                    " boxes and " + std::to_string(g_prefabMeshColliders.size()) +
                    " mesh instances");
                g_showCollisionDebug = previous;
                g_collisionDebugRenderer.Clear();
            }
            // Player capsule against the real airport: dropping onto the model
            // from above must report a floor to stand on rather than sliding
            // off sideways, which is what walking inside depends on.
            if (built && instanced) {
                const auto placed = std::find_if(g_prefabMeshColliders.begin(),
                    g_prefabMeshColliders.end(),
                    [&](const CollisionMeshInstance& value) {
                        return value.mesh == cached->second.collisionMesh.get();
                    });
                // Feet on whatever surface the downward cast just found, rather
                // than a hardcoded height: the source model can be re-exported
                // at a different elevation, and a fixed probe would silently
                // start testing thin air.
                const XMFLOAT3 probe(
                    (placed->worldBoundsMin.x + placed->worldBoundsMax.x) * 0.5f,
                    probeFloorY,
                    (placed->worldBoundsMin.z + placed->worldBoundsMax.z) * 0.5f);
                const CollisionMeshPushout stand =
                    CollisionMeshInstanceResolveCapsule(*placed, probe, 0.35f,
                                                        1.8f, kPlayerStepHeight);
                SGE_LOG("LogPrefab", EngineLog::Level::Display,
                    std::string("Player capsule probe: touched=") +
                    (stand.touched ? "yes" : "no") + " floor=" +
                    (stand.hasFloor ? std::to_string(stand.floorY) : "none") +
                    " push=(" + std::to_string(stand.displacement.x) + ", " +
                    std::to_string(stand.displacement.y) + ", " +
                    std::to_string(stand.displacement.z) + ")");
            }
            // Material and descriptor budgets with the airport loaded. Both cap
            // silently -- material overflow falls back to id 0 and the heap to a
            // fallback descriptor -- so a scene that quietly exceeds them looks
            // like "other models lost their textures" rather than an error.
            {
                std::string budgets = "Material budget: VB materials " +
                    std::to_string(visBuffer.BindlessMaterialCount()) + "/" +
                    std::to_string(VB_MAX_MATERIALS) + ", rejected textures " +
                    std::to_string(visBuffer.RejectedTextureCount());
                if (visBuffer.bindlessHeap) {
                    const BindlessDescriptorAllocator& allocator =
                        visBuffer.bindlessHeap->Allocator();
                    budgets += ", persistent descriptors " +
                        std::to_string(allocator.PersistentCount()) + "/" +
                        std::to_string(allocator.PersistentCapacity()) +
                        ", overflows " +
                        std::to_string(allocator.OverflowCount());
                }
                const bool overBudget =
                    visBuffer.BindlessMaterialCount() >= VB_MAX_MATERIALS ||
                    (visBuffer.bindlessHeap &&
                     visBuffer.bindlessHeap->Allocator().OverflowCount() > 0);
                SGE_LOG("LogPrefab", overBudget ? EngineLog::Level::Error
                                                : EngineLog::Level::Display,
                    budgets);
            }
            const bool passed = !airportPresent ||
                                (built && hit && instanced && helperAgrees);
            SGE_LOG("LogPrefab", passed ? EngineLog::Level::Display
                                        : EngineLog::Level::Error,
                passed ? "Collision smoke passed: airport BVH built, instanced, "
                         "queryable, and not shadowed by its bounds box"
                       : "Collision smoke failed");
            g_collisionSmokeChecked = true;
            // SGE_COLLISION_TEST_STAY=1 keeps the level running after the checks
            // so counters that only fill in once geometry has actually rendered
            // (materials, descriptors) can be read from a real frame.
            if (GetEnvironmentVariableA("SGE_COLLISION_TEST_STAY", nullptr, 0) == 0)
                PostQuitMessage(passed ? 0 : 4);
        }
        // Shotgun repro. Deliberately not gated on the prefab rebuild: a
        // destroyed prop requests one, and waiting for it to settle is what
        // stops the firing before the interesting frames.
        //
        // Each trigger pull is logged with the aim point and the live collider
        // counts, so a crash names the shot and the geometry state it died on
        // rather than leaving the log ending at a startup line.
        // SGE_SHOTGUN_TEST_INTERVAL/_SHOTS/_WEAPON reshape the volley loop into
        // the pattern the crash report actually describes: a player standing
        // still and emptying a rifle magazine into one wall, rather than three
        // spaced buckshot volleys from rotating positions. Sustained same-spot
        // fire is what accumulates impacts, so it is what has to be measured.
        static const UINT kSmokeInterval = []() -> UINT {
            char value[32] = {};
            if (GetEnvironmentVariableA("SGE_SHOTGUN_TEST_INTERVAL", value,
                                        sizeof(value)) == 0) return 90;
            const int parsed = std::atoi(value);
            return parsed > 0 ? static_cast<UINT>(parsed) : 90;
        }();
        static const UINT kSmokeShotCap = []() -> UINT {
            char value[32] = {};
            if (GetEnvironmentVariableA("SGE_SHOTGUN_TEST_SHOTS", value,
                                        sizeof(value)) == 0) return 24;
            const int parsed = std::atoi(value);
            return parsed > 0 ? static_cast<UINT>(parsed) : 24;
        }();
        // Nothing is fired until the level has actually finished loading and the
        // prefab rebuild has settled. A short interval otherwise starts the
        // volleys during [LevelLoad] stage 8, so the test measures a load race
        // rather than the shooting path it is meant to cover -- and the mesh
        // colliders it aims at may not be compiled yet.
        const bool shotgunSmokeReady =
            (fullLevelAssetsLoaded || emptyLevelAssetsLoaded) &&
            !g_prefabRebuildRequested;
        if (g_shotgunSmokeEnabled && shotgunSmokeReady &&
            ++g_shotgunSmokeFrames > kSmokeInterval) {
            g_shotgunSmokeFrames = 0;
            if (g_shotgunSmokeShots >= kSmokeShotCap) {
                SGE_LOG("LogGameplay", EngineLog::Level::Display,
                    "Shotgun smoke passed: " + std::to_string(kSmokeShotCap) +
                    " buckshot volleys resolved");
                PostQuitMessage(0);
            } else {
                ++g_shotgunSmokeShots;
                // Rotate through the three surfaces named in the report: the
                // helideck's mesh collider, the flat terrain, and the new
                // helicopter's mesh collider.
                const char* aimedAt = "terrain";
                XMFLOAT3 eye{ 0.0f, 2.0f, 0.0f };
                XMFLOAT3 front{ 0.0f, -1.0f, 0.0f };
                // When a level is supplied (SGE_SHOTGUN_TEST_LEVEL), the fixed
                // coordinates above are meaningless -- they were measured
                // against the repro level's helideck. Walk the mesh colliders
                // that actually loaded instead and shoot each one from outside
                // its own bounds, so every triangle mesh in the level is fired
                // into rather than whatever happens to sit at (6,3,0).
                if (!g_prefabMeshColliders.empty()) {
                    // SGE_SHOTGUN_TEST_FIXED=1 pins every shot on the first
                    // mesh collider instead of rotating, so the rounds pile
                    // into one wall the way a held trigger does.
                    static const bool fixedTarget =
                        GetEnvironmentVariableA("SGE_SHOTGUN_TEST_FIXED",
                                                nullptr, 0) > 0;
                    const CollisionMeshInstance& target =
                        g_prefabMeshColliders[fixedTarget ? 0
                            : (g_shotgunSmokeShots - 1) %
                              g_prefabMeshColliders.size()];
                    const XMFLOAT3 centre{
                        (target.worldBoundsMin.x + target.worldBoundsMax.x) * 0.5f,
                        (target.worldBoundsMin.y + target.worldBoundsMax.y) * 0.5f,
                        (target.worldBoundsMin.z + target.worldBoundsMax.z) * 0.5f };
                    // Shoot from the player spawn at the collider's centre
                    // rather than from a synthetic standoff aimed down -X. The
                    // standoff missed the geometry entirely on the Base (every
                    // round flew past and the projectile list only grew), which
                    // made the test look like it was resolving hits when it was
                    // resolving nothing at all. Aiming at a point known to be
                    // inside the mesh is what guarantees the segment crosses a
                    // surface.
                    aimedAt = "mesh collider";
                    // Stand a little outside the collider's own bounds on the
                    // shortest axis and look straight in, so the muzzle is
                    // clear of the geometry but the segment still crosses it.
                    const float halfX =
                        (target.worldBoundsMax.x - target.worldBoundsMin.x) * 0.5f;
                    const float halfZ =
                        (target.worldBoundsMax.z - target.worldBoundsMin.z) * 0.5f;
                    if (halfX <= halfZ) {
                        eye = XMFLOAT3(centre.x + halfX + 2.0f, centre.y, centre.z);
                    } else {
                        eye = XMFLOAT3(centre.x, centre.y, centre.z + halfZ + 2.0f);
                    }
                    const XMVECTOR toTarget = XMVector3Normalize(
                        XMLoadFloat3(&centre) - XMLoadFloat3(&eye));
                    XMStoreFloat3(&front, toTarget);
                } else {
                switch (g_shotgunSmokeShots % 3) {
                case 1:
                    aimedAt = "helideck";
                    eye = XMFLOAT3(6.0f, 3.0f, 0.0f);
                    front = XMFLOAT3(0.0f, -1.0f, 0.0f);
                    break;
                case 2:
                    aimedAt = "helicopter";
                    eye = XMFLOAT3(26.0f, 2.0f, 0.0f);
                    front = XMFLOAT3(-1.0f, 0.0f, 0.0f);
                    break;
                default:
                    break;
                }
                }
                scene.camera.Position = eye;
                scene.camera.Front = front;
                SGE_LOG("LogGameplay", EngineLog::Level::Display,
                    "Shotgun smoke: volley " +
                    std::to_string(g_shotgunSmokeShots) + " at " + aimedAt +
                    ", boxes=" + std::to_string(g_prefabColliders.size()) +
                    ", meshes=" + std::to_string(g_prefabMeshColliders.size()) +
                    ", projectiles=" +
                    std::to_string(scene.projectiles.size()));
                ShootPlayerWeapon();
                SGE_LOG("LogGameplay", EngineLog::Level::Display,
                    "Shotgun smoke: volley " +
                    std::to_string(g_shotgunSmokeShots) + " fired, projectiles=" +
                    std::to_string(scene.projectiles.size()));
            }
        }
        // Travel smoke stages. Each waits for the prefab rebuild to settle and
        // then exercises the real entry points rather than a copy of them, so a
        // regression in the shipping path is what this fails on.
        if (g_travelSmokeEnabled && !g_prefabRebuildRequested &&
            ++g_travelSmokeFrames > 8) {
            if (g_travelSmokeStage == 0) {
                // Stage 1: the placement compiled into a boarding point at all.
                if (g_prefabTravelPoints.empty()) {
                    SGE_LOG("LogGameplay", EngineLog::Level::Error,
                        "Travel smoke failed: no travel points compiled");
                    PostQuitMessage(5);
                } else {
                    const PrefabTravelPoint& point = g_prefabTravelPoints[0];
                    SGE_LOG("LogGameplay", EngineLog::Level::Display,
                        "Travel smoke stage 1: " +
                        std::to_string(g_prefabTravelPoints.size()) +
                        " travel point(s), first \"" + point.displayName +
                        "\" reach " + std::to_string(point.radius) + "m");
                    // Stand the camera just inside the boarding reach. Standing
                    // outside it is the control the next stage checks against.
                    scene.camera.Position = XMFLOAT3(
                        point.position.x + point.radius * 0.5f,
                        point.position.y + 1.7f,
                        point.position.z);
                    g_travelSmokeStage = 1;
                }
            } else if (g_travelSmokeStage == 1) {
                // Stage 2: proximity resolves, and E opens the board. Both go
                // through the functions the key handler calls.
                // Not named "near": windef.h still defines that as a macro.
                const bool inReach = NearbyTravelPoint() != nullptr;
                const bool opened = OpenNearbyTravelScreen();
                SGE_LOG("LogGameplay",
                    (inReach && opened) ? EngineLog::Level::Display
                                        : EngineLog::Level::Error,
                    std::string("Travel smoke stage 2: nearby=") +
                    (inReach ? "yes" : "no") + " opened=" +
                    (opened ? "yes" : "no"));
                if (!inReach || !opened) PostQuitMessage(6);
                else g_travelSmokeStage = 2;
            } else if (g_travelSmokeStage == 2) {
                // Stage 3: every destination resolves to a level file on disk.
                // A card that cannot find its map is the failure this catches,
                // and it is checked for all of them rather than the one flown.
                bool allResolved = true;
                for (const TravelDestination& destination :
                        kTravelDestinations) {
                    std::error_code error;
                    bool found = false;
                    for (const char* candidate : destination.levelCandidates)
                        found = found ||
                            std::filesystem::exists(candidate, error);
                    if (!found) allResolved = false;
                    SGE_LOG("LogGameplay", found ? EngineLog::Level::Display
                                                 : EngineLog::Level::Error,
                        std::string("Travel smoke stage 3: destination \"") +
                        destination.name + (found ? "\" resolved"
                                                  : "\" MISSING level file"));
                }
                if (!allResolved) PostQuitMessage(7);
                else g_travelSmokeStage = 3;
            } else if (g_travelSmokeStage == 3) {
                // Stage 4: fly. Training Range is the cheapest destination to
                // load and is not the map already running, so a successful swap
                // is unambiguous.
                SGE_LOG("LogGameplay", EngineLog::Level::Display,
                    "Travel smoke stage 4: departing");
                TravelToDestination(hwnd, kTravelDestinations[1]);
                g_travelSmokeStage = 4;
                g_travelSmokeFrames = 0;
            } else if (g_travelSmokeStage == 4 && g_travelSmokeFrames > 30) {
                // Stage 5: the swap landed. The board must be closed and the new
                // level's own prefabs compiled -- the Training Range has no
                // boarding point, so the list emptying is the proof the level
                // actually changed.
                //
                // The cursor is deliberately not asserted free here. The
                // Training Range is a player_choice map, so StartLevelOne hands
                // it to the deployment screen on arrival; whether mouse-look is
                // live on landing belongs to the destination level's insertion
                // mode, not to this feature. What travel owes is that the board
                // itself released its own hold, which is g_travelCursorReleased
                // going back down.
                const bool closed = !g_travelScreenOpen;
                const bool boardReleased = !g_travelCursorReleased;
                const bool swapped = g_prefabTravelPoints.empty();
                const bool passed = closed && boardReleased && swapped;
                SGE_LOG("LogGameplay", passed ? EngineLog::Level::Display
                                              : EngineLog::Level::Error,
                    std::string("Travel smoke stage 5: boardClosed=") +
                    (closed ? "yes" : "no") + " boardCursorReleased=" +
                    (boardReleased ? "yes" : "no") + " levelSwapped=" +
                    (swapped ? "yes" : "no"));
                SGE_LOG("LogGameplay", passed ? EngineLog::Level::Display
                                              : EngineLog::Level::Error,
                    passed ? "Travel smoke passed: boarding point opens, "
                             "destinations resolve, and departure loads"
                           : "Travel smoke failed");
                g_travelSmokeStage = 5;
                PostQuitMessage(passed ? 0 : 8);
            }
        }
        if (g_prefabEditorSmokeEnabled && !g_prefabEditorSmokeFinished) {
            for (auto& [id, thumbnail] : g_prefabThumbnails) {
                if (!thumbnail.cacheWrite.valid() ||
                    thumbnail.cacheWrite.wait_for(std::chrono::seconds(0)) !=
                        std::future_status::ready) continue;
                const bool saved = thumbnail.cacheWrite.get();
                SGE_LOG("LogPrefab", saved ? EngineLog::Level::Display
                                            : EngineLog::Level::Error,
                    saved ? "Prefab editor RTT smoke test passed: " +
                                thumbnail.pngPath.string()
                          : "Prefab editor RTT smoke test failed to cache PNG");
                g_prefabEditorSmokeFinished = true;
                PostQuitMessage(saved ? 0 : 2);
                break;
            }
        }
        // Budget snapshot once geometry has actually rendered, which is the only
        // point the material and descriptor counters mean anything.
        if (g_collisionSmokeEnabled && g_collisionSmokeChecked &&
            ++g_collisionBudgetFrames == 240) {
            std::string budgets = "Material budget after render: VB materials " +
                std::to_string(visBuffer.BindlessMaterialCount()) + "/" +
                std::to_string(VB_MAX_MATERIALS) +
                ", legacy materials " +
                std::to_string(visBuffer.MaterialCount()) +
                ", legacy textures " +
                std::to_string(visBuffer.MaterialTextureCount()) + "/" +
                std::to_string(visBuffer.MaterialTextureCapacity()) +
                ", rejected textures " +
                std::to_string(visBuffer.RejectedTextureCount()) +
                ", bindless " + (scene.bindlessMaterials ? "on" : "OFF") +
                ", forward draw slot " +
                std::to_string(mainShader.currentDrawCall) + "/" +
                std::to_string(MAX_DRAW_CALLS_PER_FRAME);
            if (visBuffer.bindlessHeap) {
                const BindlessDescriptorAllocator& allocator =
                    visBuffer.bindlessHeap->Allocator();
                budgets += ", persistent descriptors " +
                    std::to_string(allocator.PersistentCount()) + "/" +
                    std::to_string(allocator.PersistentCapacity()) +
                    ", overflows " + std::to_string(allocator.OverflowCount());
            }
            SGE_LOG("LogPrefab", EngineLog::Level::Display, budgets);
            PostQuitMessage(0);
        }
        if (visibilitySmokeEnabled && ++visibilityCpuReportFrames >= 300) {
            visibilityCpuReportFrames = 0;
            std::ofstream cpuLog("visibility_cpu.log", std::ios::trunc);
            // CPU work, the blocked time, and the wall clock they add back up
            // to -- logged together so the split can be checked rather than
            // trusted.
            cpuLog << "cpu_frame_ms=" << g_profiler.CpuFrameMs() << '\n'
                   << "cpu_wait_ms=" << g_profiler.CpuFrameWaitMs() << '\n'
                   << "cpu_wall_ms=" << g_profiler.CpuFrameWallMs() << '\n'
                   << "gpu_frame_ms=" << g_profiler.GpuFrameMs() << '\n';
            for (const ProfilerSampleDX12& sample : g_profiler.CpuSamples())
                cpuLog << sample.name << '=' << sample.milliseconds << '\n';
            cpuLog << "visibility_draws=" << g_visibilityDrawCalls << '\n'
                   << "forward_draws=" << g_forwardDrawCalls << '\n'
                   << "destruction_batches="
                   << g_destruction.GetRenderBatches().size() << '\n'
                   << "destruction_fallbacks="
                   << g_destruction.GetRenderItems().size() << '\n'
                   << "destruction_awake="
                   << g_destruction.GetAwakeActorCount() << '\n'
                   << "destruction_low_motion="
                   << g_destruction.GetLowMotionActorCount() << '\n'
                   << "destruction_spatial_batches="
                   << g_destruction.GetSpatialBatchCount() << '\n'
            // Whether the forward extensions pass is still redrawing chunks the
            // visibility resolve already shaded, and what that redraw costs.
                   << "destruction_in_vb="
                   << (g_destructionInVisibilityBuffer ? 1 : 0) << '\n'
                   << "destruction_forward_primitives="
                   << g_destructionForwardPrimitivesDrawn << '\n'
                   << "destruction_vb_owned_primitives="
                   << g_destructionVisibilityOwnedPrimitives << '\n'
            // Chunk-flicker diagnostics. An ownership flip hands the whole
            // house between the visibility and forward passes and wipes
            // temporal history, so a rising count is the flicker itself.
            // Registration failures are what force those flips.
                   << "destruction_ownership_flips="
                   << g_destructionOwnershipFlips << '\n'
                   << "destruction_reg_fail_frames="
                   << g_destructionRegistrationFailFrames << '\n'
                   << "destruction_chunks_seen="
                   << g_destructionChunksSeenThisFrame << '\n'
                   << "destruction_chunks_registered="
                   << g_destructionChunksRegisteredThisFrame << '\n'
                   << "destruction_frustum_only_commands="
                   << g_destructionFrustumOnlyCommands << '\n'
            // Leading indicator for the geometry-pool leak: non-zero means the
            // pool ran dry and chunks stopped registering.
                   << "vb_geometry_reg_failures="
                   << visBuffer.GeometryRegistrationFailures() << '\n'
                   << "vb_geometry_vertex_high_water="
                   << visBuffer.GeometryVertexHighWater() << '\n'
                   << "vb_geometry_free_ranges="
                   << visBuffer.GeometryFreeRangeCount() << '\n'
                   << "fe_destruction_gpu_ms="
                   << g_profiler.GpuScopeMs("FE/Destruction") << '\n'
                   << "forward_extensions_gpu_ms="
                   << g_profiler.GpuScopeMs("Forward Extensions") << '\n'
                   << "visibility_buffer_gpu_ms="
                   << g_profiler.GpuScopeMs("Visibility Buffer") << '\n'
                   << "gtao_contact_shadows_gpu_ms="
                   << g_profiler.GpuScopeMs("GTAO + Contact Shadows") << '\n'
                   << "gpu_frame_ms=" << g_profiler.GpuFrameMs() << '\n';
            cpuLog << "spot_cache=" << (scene.cacheSpotShadows ? 1 : 0) << '\n'
                   << "spot_cached=" << shadowMap.cachedSpotSlicesThisFrame << '\n'
                   << "spot_refreshed=" << shadowMap.refreshedSpotSlicesThisFrame << '\n';
        }

        if (visibilityBenchmark && !visibilityBenchmarkComplete &&
            g_game.session.Screen() == GameScreen::Level1 &&
            !g_game.loading.Active()) {
            const double gpuMs = g_profiler.GpuFrameMs();
            if (!usingVisibility) {
                if (visibilityTestForwardFrames > 120 && gpuMs > 0.0) {
                    visibilityBenchmarkForwardMs += gpuMs;
                    ++visibilityBenchmarkForwardSamples;
                }
                if (visibilityBenchmarkForwardSamples >= 300) {
                    const double average = visibilityBenchmarkForwardMs /
                        visibilityBenchmarkForwardSamples;
                    std::ofstream("visibility_benchmark.log", std::ios::trunc)
                        << "forward_gpu_ms=" << average << '\n';
                    scene.useVisibilityBuffer = true;
                    visibilityTestPending = false;
                    visibilityBenchmarkVBFrames = 0;
                }
            } else {
                ++visibilityBenchmarkVBFrames;
                if (visibilityBenchmarkVBFrames > 120 && gpuMs > 0.0) {
                    visibilityBenchmarkVBMs += gpuMs;
                    ++visibilityBenchmarkVBSamples;
                }
                if (visibilityBenchmarkVBSamples >= 300) {
                    const double forwardAverage = visibilityBenchmarkForwardMs /
                        visibilityBenchmarkForwardSamples;
                    const double vbAverage = visibilityBenchmarkVBMs /
                        visibilityBenchmarkVBSamples;
                    const bool keepVisibility = vbAverage < forwardAverage;
                    std::ofstream log("visibility_benchmark.log", std::ios::app);
                    log << "visibility_gpu_ms=" << vbAverage << '\n'
                        << "delta_gpu_ms=" << (vbAverage - forwardAverage) << '\n'
                        << "decision=" << (keepVisibility ? "keep" : "disable")
                        << '\n';
                    scene.useVisibilityBuffer = keepVisibility;
                    visibilityBenchmarkComplete = true;
                }
            }
        }
    }

    WaitForGPU();
    g_assetWatcher.Stop();
    waterRenderer.Shutdown();
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_destruction.Shutdown();
    g_banditHitVoiceAudio.Shutdown();
    g_banditDeathAudio.Shutdown();
    g_banditAttackAudio.Shutdown();
    g_greatJobAudio.Shutdown();
    g_exfilHereAudio.Shutdown();
    g_plantC4TowerAudio.Shutdown();
    g_menuMusicAudio.Shutdown();
    g_readyToDropAudio.Shutdown();
    g_banditSpottedAudio2.Shutdown();
    g_banditSpottedAudio1.Shutdown();
    g_metalHitAudio.Shutdown();
    g_hitAudio.Shutdown();
    g_grenadeExplosionAudio.Shutdown();
    g_explosionAudio.Shutdown();
    g_reloadAudio.Shutdown();
    g_rpgFireAudio.Shutdown();
    for (GunAudio& step : g_footstepAudio) step.Shutdown();
    g_breathingAudio.Shutdown();
    g_gunAudio.Shutdown();
    // Last: every effect above shares this one device, so it can only be torn
    // down once they have all released their voices.
    AudioDevice::Shutdown();
    g_profiler.Shutdown();
    CleanupDX12();
    return (int)msg.wParam;
}
