#pragma once

// Private application implementation; included once by main.cpp in dependency order.

static void UpdateHelicopterHoverAudio() {
    const bool alive = scene.player.godMode || scene.player.health > 0.0f;
    const bool primaryActive = scene.showHelicopter &&
        !g_helicopterDead && !g_helicopterCrashed;
    const bool secondaryActive = scene.showHelicopter &&
        SecondaryHelicopterPresent() &&
        !g_secondaryHelicopterDead && !g_secondaryHelicopterCrashed;
    // The insertion BlackHawk turns its rotor from the moment it comes in
    // until it climbs out of sight, so it feeds the same loop.
    const VehicleSystem& vehicles = g_game.vehicles;
    // A downed wreck has no rotor left to turn, so it drops out of the loop.
    const bool blackHawkActive = vehicles.blackHawkVisible &&
        vehicles.blackHawkPhase != VehicleSystem::BlackHawkPhase::Gone &&
        !vehicles.BlackHawkIsDown();
    const bool active = IsGameplayScreen() && alive &&
        (primaryActive || secondaryActive || blackHawkActive);
    auto distanceToCamera = [](const XMFLOAT3& position) {
        const float dx = position.x - scene.camera.Position.x;
        const float dy = position.y - scene.camera.Position.y;
        const float dz = position.z - scene.camera.Position.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    };
    float distance = primaryActive ? distanceToCamera(g_helicopterPosition) : FLT_MAX;
    if (secondaryActive)
        distance = (std::min)(distance,
                              distanceToCamera(g_secondaryHelicopterPosition));
    if (blackHawkActive) {
        // Riding in the cabin puts the camera inside the airframe, so clamp
        // the falloff distance rather than letting it spike the volume.
        const float blackHawkDistance = vehicles.blackHawkCarryingPlayer
            ? 6.0f
            : (std::max)(4.0f, distanceToCamera(vehicles.blackHawkPosition));
        distance = (std::min)(distance, blackHawkDistance);
    }
    const float volume = 0.72f * (std::max)(0.0f, 1.0f - distance / 95.0f);
    g_helicopterHoverAudio.SetLoop(active, volume, 0.96f);

    // Cockpit alarm below 30% health. Runs until the wreck is on the ground,
    // so it carries through the spiral. Riding along puts the player inside the
    // cabin with it, hence the same clamped falloff as the rotor loop.
    const bool alarming = IsGameplayScreen() && alive && blackHawkActive &&
        vehicles.BlackHawkHealthFraction() < 0.30f;
    float alarmVolume = 0.0f;
    if (alarming) {
        const float alarmDistance = vehicles.blackHawkCarryingPlayer
            ? 3.0f
            : (std::max)(4.0f, distanceToCamera(vehicles.blackHawkPosition));
        // Tighter falloff than the rotor: an in-cabin warning should not carry
        // across the whole island.
        alarmVolume = 0.62f * (std::max)(0.0f, 1.0f - alarmDistance / 55.0f);
    }
    g_blackHawkAlarmAudio.SetLoop(alarming && alarmVolume > 0.001f,
                                  alarmVolume, 1.0f);
}

static void UpdateFireLoopAudio() {
    if (!IsGameplayScreen() || g_game.loading.Active()) {
        g_fireLoopAudio.SetLoop(false);
        return;
    }

    float audibility = 0.0f;
    auto addFire = [&audibility](const XMFLOAT3& position, float strength) {
        const float dx = position.x - scene.camera.Position.x;
        const float dy = position.y - scene.camera.Position.y;
        const float dz = position.z - scene.camera.Position.z;
        const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        const float falloff = (std::max)(0.0f, 1.0f - distance / 38.0f);
        audibility += strength * falloff * falloff;
    };

    for (const FirePatch& fire : scene.firePatches) {
        if (fire.life <= 0.0f) continue;
        const float strength = (std::max)(0.35f,
            fire.radius / (std::max)(0.01f, fire.maxRadius));
        addFire(fire.position, strength);
    }
    for (const BurningMaterial& material : scene.burningMaterials)
        if (material.life > 0.0f) addFire(material.position, 0.75f);
    for (const ExplosiveBarrel& barrel : scene.explosiveBarrels)
        if (barrel.active && barrel.burning) addFire(barrel.position, 0.85f);
    for (const BurningTargetFX& target : scene.burningTargets)
        addFire(target.position, 0.35f * target.intensity);
    if (scene.flamethrowerAudioTime > 0.0f)
        audibility += 2.2f;

    const float volume = 0.62f * (1.0f - std::exp(-0.80f * audibility));
    g_fireLoopAudio.SetLoop(volume > 0.01f, volume, 0.98f);
}

static void SetCursorVisible(bool visible) {
    if (visible) {
        while (ShowCursor(TRUE) < 0) {}
    } else {
        while (ShowCursor(FALSE) >= 0) {}
    }
}

static void OpenMainMenu() {
    ReleasePrefabRigidBodies();
    g_game.world.Prefabs().ClearDerived();
    g_meshCollisionEntities.clear();
    g_objectivePlanes.clear();
    g_objectivePlaneEscaped = false;
    g_game.world.Prefabs().ResetGameplayState();
    g_prefabAudioPlayers.clear();
    g_game.session.SetScreen(GameScreen::MainMenu);
    g_game.session.StopTimer();
    // Bank the wallet on the way out. Extraction already saves, but abandoning
    // a run mid-mission reaches the menu through here instead -- without this,
    // everything earned before quitting would be shown on the menu and then
    // silently lost on the next launch.
    SaveCareer();
    // Always return to the menu's root rather than whatever sub-panel was open
    // when the player last left it.
    g_showSettingsMenu = false;
    g_insertionChoicePending = false;
    g_deploymentZones.clear();
    g_selectedDeploymentZone = -1;
    g_deploymentTargetValid = false;
    showUI = false;
    cameraLocked = true;
    ReleaseCapture();
    SetCursorVisible(true);
}

static const LevelEntity* FirstRuntimeEntity(LevelEntityType type) {
    auto& entities = g_game.world.Level().entities;
    auto it = std::find_if(entities.begin(), entities.end(),
        [&](const LevelEntity& entity) { return entity.enabled && entity.type == type; });
    return it == entities.end() ? nullptr : &*it;
}

static float CurrentPhysicsTerrainExtent() {
    constexpr float kMinimumExtent = 60.0f;
    constexpr float kEntityMargin = 20.0f;
    float extent = kMinimumExtent;
    for (const LevelEntity& entity : g_game.world.Level().entities) {
        if (!entity.enabled) continue;
        extent = (std::max)(extent,
            std::abs(entity.transform.position[0]) + kEntityMargin);
        extent = (std::max)(extent,
            std::abs(entity.transform.position[2]) + kEntityMargin);
    }
    return extent;
}

static void ApplyHumveeLevelPlan(const RuntimeLevelPlan& plan) {
    g_levelPlacesHumvee = !plan.humveeSpawns.empty();
    g_levelHumveeSpawns = plan.humveeSpawns;
    g_humveeGameplay.assign(g_levelHumveeSpawns.size(), {});
    g_activeHumveeIndex = kNoHumvee;
    g_drivingHumvee = false;
    if (plan.humveeSpawns.empty()) return;

    const Transform& primary = plan.humveeSpawns.front();
    g_primaryHumveeSpawn = { primary.position[0],
                             primary.position[1],
                             primary.position[2] };
    g_primaryHumveeYaw = primary.rotation[1];
}

static void InitializeLevelHumveePhysics() {
    g_destruction.ClearVehicles();
    if (g_emptyLevelMode || g_trainingRangeMode || g_baseMode ||
        !g_levelPlacesHumvee)
        return;
    for (size_t index = 0; index < g_levelHumveeSpawns.size(); ++index) {
        const Transform& humvee = g_levelHumveeSpawns[index];
        g_destruction.InitializeVehicle(
            index,
            { humvee.position[0], humvee.position[1], humvee.position[2] },
            XMConvertToRadians(humvee.rotation[1]));
    }
}

static void ApplyRuntimeLevelBasics(bool movePlayer) {
    if (!g_customLevelMode) return;
    const RuntimeLevelPlan plan =
        LevelRuntimeBuilder::Build(g_game.world.Level());
    g_levelPatrolBoatEnabled = plan.patrolBoatEnabled;
    scene.terrainHeightScale = plan.terrainHeightScale;
    scene.terrainFlat = plan.terrainFlat;
    scene.terrainTilesX = plan.terrainTilesX;
    scene.terrainTilesZ = plan.terrainTilesZ;
    scene.terrainIslandScaleX = plan.terrainIslandScaleX;
    scene.terrainIslandScaleZ = plan.terrainIslandScaleZ;
    scene.terrainOriginTileX = plan.terrainOriginTileX;
    scene.terrainOriginTileZ = plan.terrainOriginTileZ;
    g_dxrDDGI.ApplySettings(plan.dxrDDGI);
    scene.useDDGI = plan.dxrDDGI.enabled &&
        g_dxrDDGI.GetStatus().dxrSupported;
    scene.giIntensity = plan.dxrDDGI.intensity;
    scene.giMaxDistance = plan.dxrDDGI.maxRayDistance;
    scene.normalBias = plan.dxrDDGI.normalBias;
    scene.probeSpacing = plan.dxrDDGI.surfaceSpacing;
    scene.explosiveBarrels.clear();
    for (const Transform& transform : plan.explosiveBarrels) {
        scene.explosiveBarrels.push_back({{ transform.position[0],
                                            transform.position[1],
                                            transform.position[2] }});
    }
    scene.weaponPickups.clear();
    // Recorded per level load, so a level without a Humvee entity does not
    // inherit the previous level's vehicle (or the struct default at origin).
    ApplyHumveeLevelPlan(plan);
    if (!plan.humveeSpawns.empty()) {
        const Transform& humvee = plan.humveeSpawns.front();

        // An RPG lying beside the Humvee, collected by walking over it. Offset
        // along the vehicle's right flank rather than a world axis, so it sits
        // next to the door whichever way the Humvee was authored to face -- and
        // clear of the 2.6x1.5 m chassis obstacle pushed into the nav grid.
        const float humveeYaw = XMConvertToRadians(g_primaryHumveeYaw);
        const float rightX = std::cos(humveeYaw);
        const float rightZ = -std::sin(humveeYaw);
        constexpr float kFlankOffset = 4.6f;
        const float pickupX = g_primaryHumveeSpawn.x + rightX * kFlankOffset;
        const float pickupZ = g_primaryHumveeSpawn.z + rightZ * kFlankOffset;

        // Ground it on the terrain instead of the Humvee's own Y: the vehicle
        // spawn sits at chassis centre, which would float the rocket at waist
        // height and put it outside the pickup's vertical range on a slope.
        //
        // Sculpt stamps are pushed into the terrain later in this same load (see
        // the SetSculptStamps call below), and HeightAt reads them from a static.
        // Passing the level's stamps explicitly samples the ground the player
        // will actually walk on rather than the unsculpted surface.
        auto terrainParams = CurrentTerrainParams();
        terrainParams.heightScale = scene.terrainHeightScale;
        const float groundY = TerrainRendererDX12::HeightAt(
            terrainParams, pickupX, pickupZ, g_game.world.TerrainSculpt());

        WeaponPickup rocket;
        // Hovers at roughly knee height. The mesh is centred on its own Y, so
        // this is the centreline of the launcher, not its underside.
        rocket.position = { pickupX, groundY + 0.85f, pickupZ };
        rocket.yawRadians = humveeYaw;
        // One loaded rocket and four spare travel with the instance, as will
        // any attachments when customised pickups are authored later.
        rocket.weapon = scene.player.weapons.CreateInstance(2, 1, 4);
        rocket.active = true;
        scene.weaponPickups.push_back(rocket);
    }
    if (plan.helicopterSpawn) {
        const Transform& helicopter = *plan.helicopterSpawn;
        g_helicopterSpawn = { helicopter.position[0],
                              helicopter.position[1],
                              helicopter.position[2] };
        g_helicopterPosition = g_helicopterSpawn;
        g_helicopterLevelScale = helicopter.scale[0];
        g_helicopterYaw = XMConvertToRadians(helicopter.rotation[1]);
        scene.showHelicopter = true;
    } else { scene.showHelicopter = false; g_helicopterLevelScale = 1.0f; }
    if (movePlayer && plan.playerSpawn) {
            const Transform& player = *plan.playerSpawn;
            scene.camera = Camera({ player.position[0],
                                    player.position[1],
                                    player.position[2] });
            scene.camera.Yaw = player.rotation[1] - 90.0f;
            scene.camera.Pitch = player.rotation[0];
            // The new camera carries the constructor's default sensitivity;
            // restore the player's choice before any input reaches it.
            ApplyGameSettings();
            scene.camera.ProcessMouseMovement(0.0f, 0.0f);
    }
}

// Spline control points are the serialized source of truth; their generated
// prefab entities are deliberately omitted from level JSON. Recreate them only
// after ApplyRuntimeLevelBasics has installed this level's terrain parameters,
// otherwise a custom island's fence would be sampled against the previous map.
static void BakeRuntimeSplineEntities() {
    LevelDefinition& level = g_game.world.Level();
    if (level.splines.empty()) return;

    uint64_t nextId = 1;
    size_t authoredEntityCount = 0;
    for (const LevelEntity& entity : level.entities) {
        if (!entity.overrides.contains(kSplineOwnerKey)) {
            ++authoredEntityCount;
            nextId = (std::max)(nextId, entity.id + 1);
        }
    }
    for (const LevelSplinePath& spline : level.splines)
        nextId = (std::max)(nextId, spline.id + 1);

    auto terrainParams = CurrentTerrainParams();
    terrainParams.heightScale = scene.terrainHeightScale;
    BakeSplineEntities(level, nextId,
        [terrainParams](float x, float z) {
            return TerrainRendererDX12::HeightAt(
                terrainParams, x, z, g_game.world.TerrainSculpt());
        });
    SGE_LOG("LogLevel", EngineLog::Level::Display,
        "Baked " + std::to_string(level.entities.size() - authoredEntityCount) +
        " prefab entities from " + std::to_string(level.splines.size()) +
        " spline run(s)");
}

static void SynchronizeEditorRuntime(bool play);
static void ApplyTerrainSplatMap(const LevelDefinition& level);
static void StartLevelEditor(HWND hwnd,
                             const std::filesystem::path& levelPath = {});
static void StopEditorPlaytest();

static void OpenWinScreen() {
    const MissionReport report = g_game.mission.Finish(
        g_game.session.ElapsedSeconds(), static_cast<uint32_t>(LiveMarineCount()));
    // The grade pays out. Recorded before StopTimer only because nothing here
    // is gated on the timer -- extraction is the one award that lands whatever
    // the run state, and it is what turns the score into money.
    g_winScreenMissionBonus = g_game.money.AwardMissionBonus(report.totalScore);
    g_winScreenPayout = static_cast<int64_t>(g_game.money.SessionEarned());
    // Banked to disk at extraction rather than per award: a career should not
    // survive only if the player quits from the menu, and writing on every kill
    // would put a file open/write in the middle of combat.
    SaveCareer();
    g_game.session.StopTimer();
    g_game.session.SetScreen(GameScreen::WinScreen);
    g_greatJobAudio.Play(2.0f);
    showUI = false;
    cameraLocked = true;
    ReleaseCapture();
    SetCursorVisible(true);
}

// A fresh life or a fresh playtest starts rested. Without this a run that ended
// mid-sprint would begin the next one exhausted and breathing.
static void ResetSprintStamina() {
    g_staminaSeconds = kStaminaMaxSeconds;
    g_staminaRecoveryDelay = 0.0f;
    g_staminaExhausted = false;
    g_breathingCooldown = 0.0f;
}
// Terrain texture arrays, the HDRI sky with its IBL prefilter, and the CPU sky
// analyses. Deferred out of boot: together they measured ~8.6 s of a ~13.4 s
// startup, and the main menu is pure ImGui -- IsSceneScreen() excludes it, so
// nothing on the front end samples terrain, sky or IBL. Boot now reaches the
// menu without paying for them, and the first level start calls this before
// BeginLevelLoading, which is where the wait belongs and where a progress bar
// is already on screen.
//
// Runs at most once. Everything below keeps the ordering the boot path relied
// on: the terrain upload is submitted and waited before the sky work resets the
// same command list, and the HDRI upload, IBL prefilter and cloud-noise bake
// share one recording session (see the comments inside).
static bool g_sceneRenderAssetsReady = false;
// Set by a level start, consumed in the frame loop before BeginFrame opens the
// frame's command list. The build below resets that list and its allocator, so
// running it from the menu button -- which is called from inside the ImGui
// frame, with the list already recording -- reset the allocator out from under
// the frame and failed with E_FAIL (0x80004005). Same hazard, and same fix, as
// the sky swap and the AO resize that share that pre-BeginFrame slot.
static bool g_sceneRenderAssetsPending = false;
static void RequestSceneRenderAssets() {
    if (!g_sceneRenderAssetsReady) g_sceneRenderAssetsPending = true;
}
static void EnsureSceneRenderAssets() {
    g_sceneRenderAssetsPending = false;
    if (g_sceneRenderAssetsReady) return;
    g_sceneRenderAssetsReady = true;

    // At boot this ran with the command list closed and nothing in flight. It
    // now runs from StartLevelOne/StartLevelEditor, after the menu has been
    // presenting frames, so the current allocator still owns commands the GPU
    // is executing and Reset() on it returns E_FAIL. Drain the queue first.
    //
    // Isolated rather than WaitForGPUAllFrames: that leaves every frame slot
    // holding an unsignalled value and requires an immediate MoveToNextFrame
    // handoff, which does not follow here -- the loading screen keeps rendering
    // for many more frames and would eventually wait forever on one of them.
    WaitForDirectQueueIdleIsolated();

    bool terrainReady = false;
    if (g_useMeshShader) {
        // Terrain initialization uploads its PBR texture arrays. Record and
        // submit those copies explicitly; otherwise the following sky reset
        // discards them and every terrain SRV samples zero.
        ThrowIfFailed(g_dx12.commandAllocators[g_dx12.frameIndex]->Reset());
        ThrowIfFailed(g_dx12.commandList->Reset(
            g_dx12.commandAllocators[g_dx12.frameIndex].Get(), nullptr));
        terrainReady = g_terrain.Init(mainShader);
        ThrowIfFailed(g_dx12.commandList->Close());
        {
            ID3D12CommandList* terrainLists[] = { g_dx12.commandList.Get() };
            g_dx12.commandQueue->ExecuteCommandLists(1, terrainLists);
        }
        WaitForGPU();
        DumpDX12DebugMessages();
    }
    if (!terrainReady) {
        scene.useMeshTerrain = false;
        std::cerr << "Mesh shader terrain unavailable; keeping flat floor\n";
    }
    scene.grenadeGroundHeight = [](float x, float z) {
        if (!scene.useMeshTerrain || !g_terrain.supported) return 0.0f;
        auto params = CurrentTerrainParams();
        params.heightScale = scene.terrainHeightScale;
        return TerrainRendererDX12::HeightAt(params, x, z);
    };

    // Mip generator (compute shader) for imported GLB textures
    SGE_LOG("LogRender", EngineLog::Level::Display, "Scene assets: mip generator");
    if (!g_mipGen.Init()) {
        std::cerr << "Mip generator init failed (non-fatal, textures will have no mips)\n";
    }

    // The command list is closed after InitDX12 and stays closed until the first
    // BeginFrame(). skyRenderer.Init() records a CopyTextureRegion for the HDRI
    // upload, so the list must be open while it runs and its work must be flushed
    // (executed + waited) before the list is closed again - otherwise the copy
    // never reaches the GPU and the sky texture stays black.
    // Keep this as one recording session: HDRI upload, IBL prefilter, and
    // cloud-noise generation all record onto the command list opened below.
    // Nothing that presents a frame may run between here and the submit -- it
    // would reset this same list and invalidate the active recording.
    SGE_LOG("LogRender", EngineLog::Level::Display, "Scene assets: HDRI, IBL prefilter, cloud noise");
    ThrowIfFailed(g_dx12.commandAllocators[g_dx12.frameIndex]->Reset());
    ThrowIfFailed(g_dx12.commandList->Reset(g_dx12.commandAllocators[g_dx12.frameIndex].Get(), nullptr));
    if (!skyRenderer.Init()) {
        std::cerr << "HDRI sky init failed (non-fatal)\n";
    }
    g_skyEnvironmentResource = skyRenderer.skyTexture.Get();
    if (environmentIBL.Init(
            g_skyEnvironmentResource, kSkyEnvironmentRotationRadians)) {
        g_specularEnvironmentResource =
            environmentIBL.prefilteredEnvironment.Get();
        g_brdfIntegrationResource = environmentIBL.brdfIntegrationLUT.Get();
        std::cout << "GGX HDRI prefilter and BRDF integration LUT ready\n";
    } else {
        g_specularEnvironmentResource = g_skyEnvironmentResource;
        std::cerr << "Specular IBL prefilter failed (using raw HDRI fallback)\n";
    }
    // Bake the cloud noise onto this same list. It is pure compute into two
    // textures nothing else touches, and the flush below already waits, so it
    // costs one dispatch pair at boot rather than a second submit.
    if (g_cloudNoise.Init()) {
        g_cloudNoise.Generate(g_dx12.commandList.Get());
    } else {
        std::cerr << "Cloud noise generation failed; clouds stay on the 2D path\n";
    }
    ThrowIfFailed(g_dx12.commandList->Close());
    {
        ID3D12CommandList* skyLists[] = { g_dx12.commandList.Get() };
        g_dx12.commandQueue->ExecuteCommandLists(1, skyLists);
    }
    WaitForGPU();
    // Only point the sky at the volumes once the dispatches have actually
    // completed -- sampling a volume still being written gives noise that
    // changes under the camera on the first frames.
    if (g_cloudNoise.Generated()) {
        skyRenderer.SetCloudVolumes(
            g_cloudNoise.ShapeVolume(), g_cloudNoise.DetailVolume());
    }
    g_mipGen.FlushPending();
    DumpDX12DebugMessages();
    SGE_LOG("LogRender", EngineLog::Level::Display, "Scene assets: sky irradiance");
    {
        auto skySH = GLBImporter::ComputeSkyIrradianceSH(
            kSkyEnvironmentPath, kSkyEnvironmentRotationRadians);
        mainShader.SetSkyIrradiance(skySH, 1.0f);
        const HDRISunLight hdriSun =
            GLBImporter::ExtractHDRISunLight(
                kSkyEnvironmentPath, 2.1f,
                kSkyEnvironmentRotationRadians);
        if (hdriSun.valid) {
            std::cout << "HDRI sun analyzed (scene defaults retained): direction=("
                      << hdriSun.direction.x << ", "
                      << hdriSun.direction.y << ", "
                      << hdriSun.direction.z << ") color=("
                      << hdriSun.color.x << ", "
                      << hdriSun.color.y << ", "
                      << hdriSun.color.z << ") sourceLuminance="
                      << hdriSun.sourceLuminance << '\n';
        } else {
            std::cerr << "HDRI sun extraction failed; using scene fallback light\n";
        }
    }

    // The visibility buffer is still initialized at boot, so it bound these two
    // when they were null. Re-point it now that the prefilter and the LUT
    // actually exist, or every VB-resolved surface samples a null environment.
    if (scene.useVisibilityBuffer)
        visBuffer.UpdateEnvironmentMap(
            g_specularEnvironmentResource, g_brdfIntegrationResource);

    // Volumetric fog initialized before the noise was baked; hand it the
    // volumes now.
    if (scene.enableVolumetricFog && g_cloudNoise.Generated())
        volumetricFog.SetCloudVolumes(
            g_cloudNoise.ShapeVolume(), g_cloudNoise.DetailVolume());

    // Second half of the boot MSAA test: terrain and sky exist only now, and a
    // pipeline of theirs without an MSAA variant has to turn MSAA off for the
    // whole scene.
    if (scene.enableMSAA &&
        ((g_terrain.supported && !g_terrain.msaaSupported) ||
         (skyRenderer.initialized && !skyRenderer.msaaSupported))) {
        std::cerr << "4x MSAA unavailable for terrain or sky (non-fatal)\n";
        scene.enableMSAA = false;
    }
}

static void StartLevelOne(HWND hwnd, bool godMode, bool stressTest = false,
                          bool emptyLevel = false,
                          const LevelDefinition* customLevel = nullptr,
                          bool startWithUIAndMobileControls = false) {
    // Terrain textures, the HDRI sky and its IBL are no longer built at boot.
    // Queue them rather than building them here: this runs from the menu button
    // inside the ImGui frame, and the build needs the frame's command list
    // closed. The frame loop picks this up before its next BeginFrame, which is
    // still ahead of the loading screen's first level-load stage.
    RequestSceneRenderAssets();
    if (bindlessHeap.Initialized()) {
        WaitForGPUAllFrames();
        bindlessHeap.ResetForNewScene();
        visBuffer.ResetBindlessMaterials();
    }
    const bool wasCornellTest = g_ddgiCornellTestMode;
    g_ddgiCornellTestMode = customLevel &&
        customLevel->name == "DXR DDGI Cornell Box";
    if (g_ddgiCornellTestMode) {
        if (!wasCornellTest) {
            g_ddgiCornellPreviousTemporalEffects =
                visBuffer.temporalEffectsEnabled;
            g_ddgiCornellPreviousAnimateDemoLights =
                scene.animateDemoLights;
        }
        visBuffer.temporalEffectsEnabled = false;
        visBuffer.InvalidateTemporalHistory();
        scene.animateDemoLights = false;
        scene.ambientStrength = 0.015f;
    } else if (wasCornellTest) {
        visBuffer.temporalEffectsEnabled =
            g_ddgiCornellPreviousTemporalEffects;
        visBuffer.InvalidateTemporalHistory();
        scene.animateDemoLights =
            g_ddgiCornellPreviousAnimateDemoLights;
        scene.ambientStrength = 0.07f;
    }
    g_game.session.SetScreen(GameScreen::Level1);
    g_customLevelMode = !stressTest && !emptyLevel;
    if (customLevel) {
        g_customLevelMode = true;
        g_game.world.ReplaceLevel(*customLevel);
        g_activeCustomLevelName = customLevel->name;
    } else {
        g_activeCustomLevelName.clear();
        g_game.world.ReplaceLevel(
            MakeLevelOneTemplate(),
            g_customLevelMode ? RuntimeTerrainSource::Authored
                              : RuntimeTerrainSource::Empty);
    }
    shadowMap.InvalidateCachedCascades();
    // ApplyRuntimeLevelBasics early-returns for non-custom levels, so refresh
    // vehicle placement here as well to clear parked instances between maps.
    const RuntimeLevelPlan levelPlan =
        LevelRuntimeBuilder::Build(g_game.world.Level());
    ApplyHumveeLevelPlan(levelPlan);
    g_levelPatrolBoatEnabled = levelPlan.patrolBoatEnabled;
    g_terrain.SetSculptStamps(g_game.world.TerrainSculpt());
    // Built-in levels carry no sidecar, so this clears any map left over from a
    // painted custom level rather than letting it bleed across a level change.
    ApplyTerrainSplatMap(customLevel ? *customLevel : LevelDefinition{});
    g_grass.ClearRuntimeExclusions();
    g_game.ResetLevelState();
    g_enemySystem.ResetLevelCounters();
    // ResetLevelState cleared the turret list; drop the posed clones with it so
    // a level with fewer emplacements cannot keep drawing the last one's.
    g_aaTurretModelInstances.clear();
    g_emptyLevelMode = emptyLevel;
    g_stressTestMode = stressTest && !emptyLevel;
    g_trainingRangeMode = g_activeCustomLevelName == "Training Range";
    // A level nobody is delivered to: the player starts on its PlayerSpawn and
    // there is no transport or planning screen. Read from the level's own
    // insertion mode rather than matched on the name as the range above is --
    // "not a run" is a property of the map, and keying it to the string "Base"
    // meant a renamed hub, or a second one, quietly went back to being flown
    // into. The name match stays as a fallback so an existing Base.json that
    // predates the mode still behaves as the hub.
    g_baseMode = g_customLevelMode &&
        (g_game.world.Level().insertionMode == LevelInsertionMode::Spawn ||
         g_activeCustomLevelName == "Base");
    // Re-arm the callout so a restart of the range plays it again.
    g_plantC4Played = false;
    g_impactDecals.clear();
    g_commTowerMusicSwell = false;
    g_plantC4TowerDelay = -1.0f;
    g_exfilHereDelay = -1.0f;
    const bool modeAssetsLoaded = g_emptyLevelMode
        ? emptyLevelAssetsLoaded : fullLevelAssetsLoaded;
    if (modeAssetsLoaded)
        wallModel = g_stressTestMode ? stressWallModel : normalWallModel;
    // The base is not a run, so its clock never starts -- a mission timer
    // ticking up while the player wanders a hub would read as a run they are
    // already failing.
    g_game.session.ResetTimer(modeAssetsLoaded && !g_baseMode);
    if (!modeAssetsLoaded) BeginLevelLoading();
    g_pendingEnvironmentRebuild = modeAssetsLoaded && g_customLevelMode;
    scene.player.godMode = godMode;
    scene.RestorePlayerHealth();
    ResetSprintStamina();
    g_game.world.Prefabs().ResetGameplayState();
    scene.ResetLevelRuntimeState();
    // A tower rigged last run must not start the next one already demolishable.
    g_commTowersRiggedForDemolition.clear();
    scene.selectedGrenade = g_game.mission.Loadout().grenade;
    if (g_emptyLevelMode)
        GunModel::DisableLoadoutRestriction();
    scene.camera = Camera(XMFLOAT3(0.0f, 5.0f, 10.0f));
    // Fresh camera, so the player's sensitivity has to be pushed back in.
    ApplyGameSettings();
    scene.gun.visible = true;
    scene.showHelicopter = !g_emptyLevelMode;
    ApplyRuntimeLevelBasics(true);
    BakeRuntimeSplineEntities();
    if (modeAssetsLoaded)
        scene.rebuildDestructionRequested = true;
    // Re-aim the insertion at the spawn the player actually got. ResetLevelState
    // above ran before the camera was placed, so the route it rebuilt still
    // pointed at the previous position. Deferred rather than done here because
    // on a cold load the model does not exist yet: the flag is consumed once
    // the terrain is up, which covers both the first load and every restart.
    // The base is walked into, not inserted into, so neither transport is armed
    // for it -- a helicopter run would fly in and try to drop the player who is
    // already standing in the hub.
    g_blackHawkInsertionRestartPending = !g_emptyLevelMode && !g_baseMode;
    g_insertionBoatRestartPending = !g_emptyLevelMode && !g_baseMode;
    // Every playable map opens on the deployment fly-through. The authored mode
    // is the initial selection, but the player can pick any of the three routes
    // and one of the perimeter zones before the run and timer begin.
    if (!g_emptyLevelMode && !g_baseMode) {
        BeginDeploymentPlanning();
        g_game.vehicles.DisableBlackHawkInsertion();
        g_game.vehicles.DisableInsertionBoat();
        // Stand the AA gun back up for the new run. ResetLevel cleared it, so a
        // map whose towers were all felled last run gets a fresh emplacement.
        PlaceAATurretNearCommTower();
    } else {
        // Shared with the empty level: no planning screen, so the transports
        // have to be stood down explicitly or a run armed by the previous level
        // would still be flying.
        if (g_baseMode) {
            g_game.vehicles.DisableBlackHawkInsertion();
            g_game.vehicles.DisableInsertionBoat();
        }
        g_insertionChoicePending = false;
        g_deploymentZones.clear();
        g_selectedDeploymentZone = -1;
        g_deploymentTargetValid = false;
    }
    g_game.commands.Set(GameCommand::ResetLevelRuntime, modeAssetsLoaded);
    deathCursorReleased = false;
    // The cursor is re-grabbed for mouse-look below, so the next prompt has to
    // release it again rather than assuming it is already free.
    g_insertionChoiceCursorReleased = false;
    showUI = startWithUIAndMobileControls;
    virtualInput.showPad = startWithUIAndMobileControls;
    cameraLocked = g_insertionChoicePending;
    if (g_insertionChoicePending || startWithUIAndMobileControls) {
        ReleaseCapture();
        SetCursorVisible(true);
        g_insertionChoiceCursorReleased = g_insertionChoicePending;
    } else {
        SetCapture(hwnd);
        SetCursorVisible(false);
    }
    RECT rect;
    GetClientRect(hwnd, &rect);
    POINT center = { (rect.right - rect.left) / 2,
                     (rect.bottom - rect.top) / 2 };
    ClientToScreen(hwnd, &center);
    ignoreNextMouseMove = true;
    SetCursorPos(center.x, center.y);
    lastX = (float)(rect.right - rect.left) * 0.5f;
    lastY = (float)(rect.bottom - rect.top) * 0.5f;
    firstMouse = true;
    g_prefabRebuildRequested = true;
    g_prefabRebuildReason = "level start";
}

// godMode defaults on because every other caller is a direct "load this map"
// action -- a menu button, a startup path, the level browser -- and those have
// always dropped the player in invulnerable to look around. Travel passes
// false: flying out of the hub is the point the player commits to a run, and
// the boarding press already turned god mode off, so re-arming it here would
// undo that a moment before the deploy screen renders the toggle.
static void StartCustomLevel(HWND hwnd, const std::filesystem::path& path,
                             bool godMode = true) {
    LevelLoadResult loaded = LoadLevel(path);
    if (!loaded.ok) {
        g_mainMenuLevelStatus = "Load failed: " + loaded.error;
        return;
    }
    g_mainMenuLevelStatus.clear();
    StartLevelOne(hwnd, godMode, false, false, &loaded.level);
}

// Island 1 -- the campaign map, authored as Islandv10.json. The menu name and
// the file name differ on purpose: the file keeps its authoring history (v10 is
// the tenth revision of the island) while the player sees a level name.
//
// The path is searched rather than hardcoded because the two layouts differ:
// the repo and the packaged build both carry Content/Levels, but the packager
// also stages a flat levels/ copy, and a build/ run resolves against
// build/Content/Levels. Trying each in turn keeps one button working in all of
// them instead of only wherever it was last tested.
static void StartIsland1(HWND hwnd) {
    static constexpr const char* kCandidates[] = {
        "Content/Levels/Islandv10.json",
        "levels/Islandv10.json",
        "build/Content/Levels/Islandv10.json",
    };
    std::error_code error;
    for (const char* candidate : kCandidates) {
        if (!std::filesystem::exists(candidate, error)) continue;
        StartCustomLevel(hwnd, std::filesystem::path(candidate));
        return;
    }
    // Say which file is missing rather than failing silently: a menu button
    // that does nothing when clicked gives the player nothing to act on.
    g_mainMenuLevelStatus =
        "Island 1 not found (Islandv10.json missing from Content/Levels).";
}

// Same candidate walk as StartIsland1: the repo, the packaged flat levels/ copy
// and a build/ run all resolve the training map from one button.
static void StartTrainingRange(HWND hwnd) {
    static constexpr const char* kCandidates[] = {
        "Content/Levels/TrainingRange.json",
        "levels/TrainingRange.json",
        "build/Content/Levels/TrainingRange.json",
    };
    std::error_code error;
    for (const char* candidate : kCandidates) {
        if (!std::filesystem::exists(candidate, error)) continue;
        StartCustomLevel(hwnd, std::filesystem::path(candidate));
        return;
    }
    g_mainMenuLevelStatus =
        "Training Range not found (TrainingRange.json missing from Content/Levels).";
}

// The home base: a walkable hub built around the NATO shelter, with no island,
// no hostiles and no insertion. Same path search as the other menu levels.
//
// The shelter GLB is by far the largest asset in the project (129 MB against a
// few MB for the airport), so the load is timed and the result logged -- the
// cost of this one model is worth knowing rather than guessing at, and the log
// line is what says whether it needs attention.
static void StartBase(HWND hwnd) {
    static constexpr const char* kCandidates[] = {
        "Content/Levels/Base.json",
        "levels/Base.json",
        "build/Content/Levels/Base.json",
    };
    std::error_code error;
    for (const char* candidate : kCandidates) {
        if (!std::filesystem::exists(candidate, error)) continue;
        const auto started = std::chrono::steady_clock::now();
        StartCustomLevel(hwnd, std::filesystem::path(candidate));
        // Measures the synchronous part only. Prefab meshes stream in through
        // the loading screen, so the shelter's own upload is not included here
        // -- the LogAssets timing around the model load covers that.
        const double seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started).count();
        SGE_LOG("LogGameplay", EngineLog::Level::Display,
            "Base level start took " + std::to_string(seconds) + " s");
        return;
    }
    g_mainMenuLevelStatus =
        "Base not found (Base.json missing from Content/Levels).";
}

static std::filesystem::path StartupLevelPath(const char* commandLine) {
    if (!commandLine) return {};
    std::string argument(commandLine);
    const size_t first = argument.find_first_not_of(" \t");
    if (first == std::string::npos) return {};
    argument.erase(0, first);

    constexpr const char* prefix = "--level";
    if (argument.rfind(prefix, 0) != 0) return {};
    argument.erase(0, std::strlen(prefix));
    const size_t value = argument.find_first_not_of(" \t=");
    if (value == std::string::npos) return {};
    argument.erase(0, value);

    if (argument.size() >= 2 && argument.front() == '"' &&
        argument.back() == '"') {
        argument = argument.substr(1, argument.size() - 2);
    }
    return std::filesystem::path(argument);
}

static void StartDDGICornellTest(HWND hwnd) {
    LevelDefinition level;
    level.name = "DXR DDGI Cornell Box";
    level.terrainHeightScale = 0.0f;
    level.dxrDDGI.enabled = true;
    level.dxrDDGI.surfaceSpacing = 0.8f;
    level.dxrDDGI.surfaceOffset = 0.22f;
    level.dxrDDGI.maxProbes = 768;
    level.dxrDDGI.raysPerProbe = 64;
    level.dxrDDGI.probesPerFrame = 24;
    level.dxrDDGI.maxRayDistance = 18.0f;
    level.dxrDDGI.intensity = 1.0f;
    level.dxrDDGI.normalBias = 0.12f;
    level.dxrDDGI.viewBias = 0.04f;
    level.dxrDDGI.hysteresis = 0.94f;
    level.dxrDDGI.multiBounceStrength = 0.45f;
    level.dxrDDGI.showProbes = false;
    LevelEntity player;
    player.id = 0x434f524e454c4c01ull;
    player.type = LevelEntityType::PlayerSpawn;
    player.name = "Cornell Camera";
    player.transform.position[0] = 0.0f;
    player.transform.position[1] = 3.2f;
    player.transform.position[2] = -6.5f;
    player.transform.rotation[1] = 180.0f;
    level.entities.push_back(player);
    // Empty runtime suppresses gameplay actors, foliage, houses, ocean clutter,
    // and stale full-level objects while custom-level mode keeps DDGI enabled.
    StartLevelOne(hwnd, true, false, true, &level, true);
    scene.gun.visible = false;
}

// Native open dialog rooted at Content/Levels. Returns false when the user
// cancels; only a real dialog failure writes g_mainMenuLevelStatus, so a
// deliberate cancel leaves no error text behind. Shared by "play a custom
// level" and the editor launcher so both browse the same way.
static bool BrowseForLevelPath(HWND hwnd, const wchar_t* title,
                               std::filesystem::path& chosen) {
    std::error_code error;
    std::filesystem::create_directories("Content/Levels", error);
    const std::wstring initialDirectory =
        std::filesystem::absolute("Content/Levels", error).wstring();
    wchar_t selected[MAX_PATH] = {};
    OPENFILENAMEW dialog = {};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = hwnd;
    dialog.lpstrFilter = L"Level JSON (*.json)\0*.json\0All files (*.*)\0*.*\0\0";
    dialog.lpstrFile = selected;
    dialog.nMaxFile = static_cast<DWORD>(std::size(selected));
    dialog.lpstrInitialDir = initialDirectory.c_str();
    dialog.lpstrDefExt = L"json";
    dialog.lpstrTitle = title;
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST |
        OFN_NOCHANGEDIR | OFN_DONTADDTORECENT;
    if (GetOpenFileNameW(&dialog)) {
        chosen = std::filesystem::path(selected);
        return true;
    }
    if (const DWORD code = CommDlgExtendedError())
        g_mainMenuLevelStatus = "File browser failed: " + std::to_string(code);
    return false;
}

static void BrowseAndStartCustomLevel(HWND hwnd) {
    std::filesystem::path chosen;
    if (BrowseForLevelPath(hwnd, L"Load Custom Level", chosen))
        StartCustomLevel(hwnd, chosen);
}

static void RestartActiveLevel(HWND hwnd) {
    if (!g_activeCustomLevelName.empty()) {
        // Snapshot the live level, but undo the gameplay damage first: a
        // destructible that was killed last run has entity.enabled == false
        // (CombatSystem::DamagePrefab disables it), and restarting from that
        // snapshot would leave the comm tower -- and every prop destroyed that
        // run -- permanently missing.
        //
        // Restricted to prefab-backed entities because those are the only ones
        // DamagePrefab can disable. No level in Content/Levels currently authors
        // a disabled entity, but scoping it this way means a future one that does
        // is not silently switched back on by a restart.
        LevelDefinition custom = g_game.world.Level();
        for (LevelEntity& entity : custom.entities)
            if (entity.type == LevelEntityType::Prefab ||
                entity.type == LevelEntityType::Rock)
                entity.enabled = true;
        // Carry the current god mode through rather than hardcoding it: a
        // restart should put the player back in the run they were already in.
        StartLevelOne(hwnd, scene.player.godMode, false, false, &custom);
    } else {
        StartLevelOne(hwnd, scene.player.godMode);
    }
}

static void RenderLoadingScreen();
