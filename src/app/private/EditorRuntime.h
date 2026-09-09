#pragma once

// Private application implementation; included once by main.cpp in dependency order.

// Cheap per-frame sync used while authoring. It updates only viewport-facing
// transforms and lights: gameplay-derived collision, audio, spawners, physics,
// navmesh and grass remain latched dirty until Save/Play.
// Pushes a level's painted terrain weights to the GPU, skipping the upload when
// nothing changed. UploadTerrainSplatMap recreates the texture and drains the
// GPU, so calling it on every editor sync would stall a frame per sync; the
// revision counter makes a repeat call free.
//
// Resolution is part of the identity check because clearing a level's painting
// leaves the revision alone but drops the resolution to zero.
static void ApplyTerrainSplatMap(const LevelDefinition& level) {
    // SGE_SPLAT_TEST owns the splatmap for the whole session; letting a level
    // load overwrite its checkerboard would defeat the diagnostic.
    static const bool splatTestActive =
        GetEnvironmentVariableA("SGE_SPLAT_TEST", nullptr, 0) > 0;
    if (splatTestActive) return;
    static uint32_t appliedRevision = 0;
    static uint32_t appliedResolution = 0;
    static bool appliedOnce = false;
    const size_t expected = static_cast<size_t>(level.terrainSplatResolution) *
                            level.terrainSplatResolution * 4u;
    const bool usable = level.terrainSplatResolution > 0 &&
                        level.terrainSplatRGBA.size() == expected;
    const uint32_t revision = usable ? level.terrainSplatRevision : 0u;
    const uint32_t resolution = usable ? level.terrainSplatResolution : 0u;
    if (appliedOnce && revision == appliedRevision &&
        resolution == appliedResolution)
        return;
    appliedOnce = true;
    appliedRevision = revision;
    appliedResolution = resolution;
    // Staged, not uploaded: this runs outside the frame's command-list
    // recording, so the GPU copy is deferred to FlushPendingSplatUpload().
    g_terrain.StageTerrainSplatMap(
        usable ? level.terrainSplatRGBA.data() : nullptr, resolution);
}

static bool SynchronizeEditorRuntimeLight(uint64_t changedEntityId = 0) {
    if (g_game.session.Screen() != GameScreen::LevelEditor) return false;
    ProfilerDX12::CpuScope syncLightProfile(g_profiler, "Editor/SyncLight");
    const LevelDefinition& edited = g_levelEditor.Level();
    const LevelDefinition& runtime = g_game.world.Level();
    if (edited.entities.size() != runtime.entities.size()) return false;

    struct TransformDelta {
        uint64_t entityId = 0;
        LevelEntityType type = LevelEntityType::EnemySpawn;
        XMFLOAT4X4 matrix{};
    };
    std::vector<TransformDelta> deltas;
    bool refreshLevelBasics = false;

    const auto matrixFor = [](const Transform& transform) {
        return EntityWorldMatrix(transform);
    };
    const auto supportsFastTransform = [](LevelEntityType type) {
        switch (type) {
        case LevelEntityType::PlayerSpawn:
        case LevelEntityType::EnemySpawn:
        case LevelEntityType::AllySpawn:
        case LevelEntityType::ExplosiveBarrel:
        case LevelEntityType::Humvee:
        case LevelEntityType::Helicopter:
        case LevelEntityType::Rock:
        case LevelEntityType::Prefab:
            return true;
        default:
            return false;
        }
    };

    for (size_t index = 0; index < edited.entities.size(); ++index) {
        const LevelEntity& before = runtime.entities[index];
        const LevelEntity& after = edited.entities[index];
        if (before.id != after.id || before.type != after.type) return false;
        if (before.enabled != after.enabled || before.name != after.name ||
            before.prefabId != after.prefabId || before.overrides != after.overrides)
            return false;
        if (std::memcmp(&before.transform, &after.transform,
                        sizeof(Transform)) == 0)
            continue;
        if (changedEntityId != 0 && after.id != changedEntityId) return false;
        if (!supportsFastTransform(after.type)) return false;

        if (after.type == LevelEntityType::Prefab ||
            after.type == LevelEntityType::Rock) {
            bool hasRenderInstance = false;
            for (const PrefabRenderBatch& batch : g_prefabRenderBatches) {
                if (std::find(batch.entityIds.begin(), batch.entityIds.end(),
                              after.id) != batch.entityIds.end()) {
                    hasRenderInstance = true;
                    break;
                }
            }
            // Destruction-backed comm towers and AA-turret prefabs deliberately
            // have no ordinary batch. They still need the full compile path.
            if (!hasRenderInstance) return false;
        } else if (after.type == LevelEntityType::ExplosiveBarrel ||
                   after.type == LevelEntityType::Humvee ||
                   after.type == LevelEntityType::Helicopter) {
            refreshLevelBasics = true;
        }

        XMVECTOR determinant;
        const XMMATRIX inverseBefore = XMMatrixInverse(
            &determinant, matrixFor(before.transform));
        const float determinantValue = XMVectorGetX(determinant);
        if (!std::isfinite(determinantValue) ||
            std::abs(determinantValue) <= 1e-8f)
            return false;
        TransformDelta delta;
        delta.entityId = after.id;
        delta.type = after.type;
        XMStoreFloat4x4(&delta.matrix,
            inverseBefore * matrixFor(after.transform));
        deltas.push_back(delta);
    }

    if (changedEntityId != 0 && deltas.empty()) {
        // A snapped drag can settle back on its starting value. It is still a
        // valid transform-only commit and must not fall into the heavy rebuild.
        const auto found = std::find_if(edited.entities.begin(),
            edited.entities.end(), [&](const LevelEntity& entity) {
                return entity.id == changedEntityId;
            });
        if (found == edited.entities.end() || !supportsFastTransform(found->type))
            return false;
    }

    // Structural edits require the full synchronization path. RuntimeWorld
    // validates the whole entity shape before applying any transform.
    if (!g_game.world.SynchronizeEditorTransforms(edited)) return false;

    bool prefabLightsMoved = false;
    for (const TransformDelta& delta : deltas) {
        if (delta.type != LevelEntityType::Prefab &&
            delta.type != LevelEntityType::Rock)
            continue;
        const PrefabTransformUpdateResult result =
            ApplyPrefabEntityTransformDelta(g_game.world.Prefabs(),
                delta.entityId, XMLoadFloat4x4(&delta.matrix),
                PrefabTransformUpdateScope::VisualsOnly);
        prefabLightsMoved |= result.lights != 0;
    }
    if (refreshLevelBasics) ApplyRuntimeLevelBasics(false);
    if (prefabLightsMoved) RebuildRuntimePrefabLights();
    shadowMap.InvalidateCachedCascades();
    return true;
}

static void SynchronizeEditorRuntimeVisual(bool loadMissingModels = false) {
    if (g_game.session.Screen() != GameScreen::LevelEditor) return;
    ProfilerDX12::CpuScope syncVisualProfile(g_profiler, "Editor/SyncVisual");
    // Only the editor writes stamp PNGs while running, so it is the only place
    // that needs the on-disk re-check. In play that check is a filesystem stat
    // per stamp on every crater.
    TerrainRendererDX12::SetStampHotReloadEnabled(true);
    g_game.world.ReplaceFromEditor(g_levelEditor.Level());
    g_terrain.SetSculptStamps(g_game.world.TerrainSculpt());
    ApplyTerrainSplatMap(g_levelEditor.Level());
    ApplyRuntimeLevelBasics(false);
    RebuildEditorPrefabVisualBatches(loadMissingModels);
    shadowMap.InvalidateCachedCascades();
}

static void SynchronizeEditorRuntime(bool play) {
    if (g_game.session.Screen() != GameScreen::LevelEditor) return;
    ProfilerDX12::CpuScope syncRuntimeProfile(g_profiler, "Editor/SyncRuntime");
    // See SynchronizeEditorRuntimeVisual: authoring re-checks stamps on disk,
    // play does not. Testing a level from the editor keeps it on, which is
    // what makes a re-bake show up without leaving the editor.
    TerrainRendererDX12::SetStampHotReloadEnabled(true);
    g_editorFullReconcileInFlight = true;
    // The prefab compile must run first because navmesh/grass consume its newly
    // compiled colliders. RebuildPrefabRenderBatches queues the environment pass
    // for the following frame once those derived objects are ready.
    g_pendingEnvironmentRebuild = false;
    shadowMap.InvalidateCachedCascades();
    g_game.world.ReplaceFromEditor(g_levelEditor.Level());
    g_terrain.SetSculptStamps(g_game.world.TerrainSculpt());
    ApplyTerrainSplatMap(g_levelEditor.Level());
    g_grass.ClearRuntimeExclusions();
    g_customLevelMode = true;
    {
        ProfilerDX12::CpuScope assetRefreshProfile(g_profiler, "Editor/AssetRefresh");
        g_assetRegistry.Refresh();
        g_prefabRegistry.Refresh(kPrefabRoot, kModelRoot);
    }
    ApplyRuntimeLevelBasics(play);
    g_prefabRebuildRequested = true;
    g_prefabRebuildReason = "editor runtime sync";
    if (g_houseTemplate) {
        WaitForGPU();
        wallModel = CloneSceneTree(g_houseTemplate);
        ArrangeHousesInCross(wallModel, false);
        if (wallModel && g_dx12.device)
            g_destruction.Initialize(wallModel, g_dx12.device.Get(), 1, 1, 1);
    }
    if (g_trees.IsInitialized()) ResetPalmTrees();
    if (g_destruction.IsInitialized()) {
        auto tp = CurrentTerrainParams();
        tp.heightScale = scene.terrainHeightScale;
        g_destruction.SetTerrainSampler([tp](float x, float z) {
            return TerrainRendererDX12::HeightAt(tp, x, z);
        }, CurrentPhysicsTerrainExtent());
        InitializeLevelHumveePhysics();
    }
    g_levelEditor.MarkRuntimeSynchronized();
}

// `levelPath` empty starts a fresh Level 1 template; otherwise the editor opens
// on that file. Loading happens before StartLevelOne so the runtime world is
// built from the level the user actually chose, not from the template it would
// otherwise have to reconcile away a frame later.
static void StartLevelEditor(HWND hwnd, const std::filesystem::path& levelPath) {
    // The editor is a scene screen, so it needs the same terrain and sky the
    // boot path used to build. StartLevelOne is not on this route. Queued for
    // the same reason as there: this can be reached from inside an ImGui frame.
    RequestSceneRenderAssets();
    const bool loaded = !levelPath.empty() && g_levelEditor.LoadFrom(levelPath);
    if (!levelPath.empty() && !loaded) {
        // Falling back to the template silently would look like the chosen file
        // loaded and then turned into Level 1. Say what happened; the editor's
        // own status line carries the parser's reason.
        SGE_LOG("LogLevel", EngineLog::Level::Warning,
            "Editor load failed, opening template instead: " +
            levelPath.string());
    }
    if (!loaded) g_levelEditor.NewFromLevelOne();
    StartLevelOne(hwnd, true);
    // StartLevelOne opens the mission deployment planner. The editor has no
    // deployment screen, so carrying that state across would keep its orbit
    // camera and make ProcessInput reject every playtest movement command.
    CancelDeploymentPlanning();
    g_game.session.SetScreen(GameScreen::LevelEditor);
    g_customLevelMode = true;
    g_editorPreviousDestructionEnabled = scene.useDestruction;
    scene.useDestruction = false;
    g_game.world.ReplaceFromEditor(g_levelEditor.Level());
    g_pendingEnvironmentRebuild = false;
    g_editorFullReconcileRequested = false;
    g_editorFullReconcileInFlight = false;
    g_editorVisualRefreshRequested = false;
    g_game.session.StopTimer();
    showUI = false;
    cameraLocked = true;
    scene.camera.FPSMode = false;
    ReleaseCapture();
    SetCursorVisible(true);
    g_prefabRebuildRequested = true;
    g_prefabRebuildReason = "editor stop play";
}

static void BeginEditorPlaytest(HWND hwnd) {
    if (g_game.session.Screen() != GameScreen::LevelEditor ||
        g_levelEditor.IsPlaying()) return;
    g_editorCameraSnapshot = scene.camera;
    g_editorFullReconcileRequested = false;
    g_levelEditor.BeginPlay();
    scene.useDestruction = g_editorPreviousDestructionEnabled;
    // A playtest always begins at the authored PlayerSpawn. Do not allow a
    // stale mission-planning flag to take camera ownership or block input.
    CancelDeploymentPlanning();
    g_game.world.Prefabs().ResetGameplayState();
    scene.ResetLevelRuntimeState();
    SynchronizeEditorRuntime(true);
    scene.player.godMode = true;
    scene.RestorePlayerHealth();
    ResetSprintStamina();
    g_game.commands.Request(GameCommand::ResetLevelRuntime);
    // Arm the aircraft objective for the playtest. The normal game does this at
    // the deployment screen, which a playtest deliberately skips -- without it
    // an authored plane would just sit on the runway and the designer could
    // never see the takeoff they placed it for.
    ArmObjectivePlanes();
    g_game.session.StopTimer();
    cameraLocked = false;
    scene.camera.FPSMode = true;
    SetCapture(hwnd);
    SetCursorVisible(false);
}

static void StopEditorPlaytest() {
    if (!IsEditorPlaying()) return;
    // Previous frame can still reference enemy vertex/index buffers. Drain GPU
    // before destroying Bandits or rebuilding editor destruction resources.
    WaitForGPU();
    g_levelEditor.StopPlay();
    scene.useDestruction = false;
    g_heldBandit = nullptr;
    g_bandits.clear();
    g_game.world.Prefabs().ResetGameplayState();
    scene.ResetLevelRuntimeState();
    g_game.commands.Set(GameCommand::ResetLevelRuntime, false);
    g_game.commands.Set(GameCommand::RespawnTurretGunner, false);
    g_game.commands.Set(GameCommand::RespawnBoatGunner, false);
    g_game.session.StopTimer();
    scene.camera.FPSMode = false;
    scene.camera = g_editorCameraSnapshot;
    scene.camera.FPSMode = false;
    // Snapshot predates any sensitivity change made while playing, so reapply
    // rather than restoring whatever the camera had when it was captured.
    ApplyGameSettings();
    cameraLocked = true;
    ReleaseCapture();
    SetCursorVisible(true);
    SynchronizeEditorRuntime(false);
}
