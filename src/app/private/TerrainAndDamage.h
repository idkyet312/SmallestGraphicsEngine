#pragma once

// Private application implementation; included once by main.cpp in dependency order.

static size_t ActiveBanditSlotCount() {
    if (g_customLevelMode) {
        size_t count = static_cast<size_t>(std::count_if(
            g_game.world.Level().entities.begin(),
            g_game.world.Level().entities.end(), [](const LevelEntity& entity) {
                return entity.enabled && entity.type == LevelEntityType::EnemySpawn;
            }));
        for (const PrefabSpawnPoint& spawn : g_prefabSpawnPoints)
            if (spawn.enemyType == "bandit") count += spawn.count;
        return count;
    }
    return g_stressTestMode ? kStressBanditCount : 4 * kEnemiesPerSpawner;
}

bool DeploymentPlanningActive();

TerrainRendererDX12::Params CurrentTerrainParams() {
    TerrainRendererDX12::Params params;
    params.detailRelief = scene.terrainDetailRelief ? 1u : 0u;
    // Flat mode also silences the detail octaves: they are scaled by
    // heightScale, but leaving them on would still ride any future relief.
    if (scene.terrainFlat) params.detailRelief = 0u;
    if (g_stressTestMode) {
        params.islandScaleX = 2.0f;
        params.islandScaleZ = 2.0f;
        params.terrainStyle = TerrainRendererDX12::kStyleStressIsland;
        const ForwardExtensionQualityTier tier = g_forwardQuality.Tier();
        if (tier == ForwardExtensionQualityTier::Full) {
            // 32x32 uniform grid: 1024 tiles over +/-128 m.
            params.tilesX = 32;
            params.tilesZ = 32;
        } else {
            // Same +/-128 m coverage as the uniform grid, but as a camera-centred
            // clipmap: ring 0 is 8x8 at 8 m, and each outer ring doubles tile
            // size, so 3 rings reach 128 m. 160 tiles instead of 1024, with the
            // height function untouched -- only the sampling topology changes.
            params.terrainStyle |= TerrainRendererDX12::kStyleClipmap;
            params.tilesX = 8u;   // G: ring grid
            params.tilesZ = 3u;   // R: ring count
            params.tileSize = 8.0f;
            if (tier == ForwardExtensionQualityTier::Balanced) {
                params.lodNear = 20.0f;
                params.lodStep = 24.0f;
            } else {
                params.lodNear = 12.0f;
                params.lodStep = 18.0f;
            }
        }
    } else {
        // Camera-centred geoclipmap: concentric LOD rings. Ring 0 is fine tiles
        // around the camera; each outer ring doubles the tile size, so the map
        // has crisp ground underfoot and cheap coarse ground to the horizon at a
        // bounded tile count -- islands can be huge with full near detail.
        //   terrainStyle bit 1 = clipmap; tilesX = ring grid G; tilesZ = ring
        //   count R; tileSize = base (innermost) tile size.
        params.terrainStyle = 2u;   // clipmap mode
        params.islandScaleX = (std::max)(0.5f,
            (std::min)(scene.terrainIslandScaleX, 12.0f));
        params.islandScaleZ = (std::max)(0.5f,
            (std::min)(scene.terrainIslandScaleZ, 12.0f));
        // 1 m base tiles at the mesh shader's 8 quads per side = 0.125 m per
        // vertex in ring 0. Authored heightmap stamps carry detail far finer
        // than the terrain could represent at 0.5 m, so stamped ridges still
        // came out rounded no matter how large the stamp atlas got.
        //
        // n cannot go the other way instead: 8 quads per side is already 192
        // primitives, and D3D12 caps mesh shader output at 256 per group, so
        // the density has to come from smaller tiles rather than denser ones.
        //
        // Shrinking the base tile also shrinks every ring, so the loop below
        // adds two rings to still reach the shore -- 1200 -> 2000 tiles (1.7x).
        // Resolution is better or equal at EVERY distance, never worse: 0.125 m
        // inside 10 m, and by 40 m the rings have doubled back to the same
        // 0.5 m the old ring 0 gave at point-blank range.
        params.tileSize = 1.0f;                 // base ring tile size (full detail)
        // G=20 puts ring 0's half-span at 10 m of 0.125 m/vertex ground, with
        // each outer ring doubling the tile size from there. The clipmap hole
        // occupies the middle half of every outer ring, so G must stay
        // divisible by four -- raising G is also the expensive lever, since
        // ring cost is G^2 while adding a ring is only linear.
        constexpr UINT kGameplayRingGrid = 20u;
        params.tilesX = kGameplayRingGrid;
        // Enough rings so the outermost reaches past the island shore. The
        // deployment overview can request a broader apron around its selectable
        // drop-off ring. Ring r half-span = G/2 * base * 2^r.
        // kShoreOuter matches terrain_ms.hlsl, so the ordinary view still reaches
        // past the point where the seabed finishes sloping down.
        constexpr float kShoreOuter = 88.0f;
        constexpr float kOceanMargin = 40.0f;
        const float maxScale = (std::max)(params.islandScaleX, params.islandScaleZ);
        const float need = kShoreOuter * maxScale + kOceanMargin;
        params.tilesZ = DeploymentPlanner::TerrainRingCount(
            need, kGameplayRingGrid, params.tileSize,
            DeploymentPlanner::MaxTerrainClipmapRings);
        params.originTileX = 0;
        params.originTileZ = 0;
        // Adaptive quality on the ordinary level. The ring count is derived from
        // the island size above and must not be reduced -- dropping a ring pulls
        // the outermost tiles inside the shore and leaves a visible hole where
        // the seabed should be. Tessellation is the safe lever: tightening the
        // LOD thresholds makes each ring step down to coarser tessellation
        // sooner, which cuts mesh-shader output without changing coverage.
        switch (g_forwardQuality.Tier()) {
            case ForwardExtensionQualityTier::Balanced:
                params.lodNear = 36.0f;
                params.lodStep = 26.0f;
                break;
            case ForwardExtensionQualityTier::Performance:
                params.lodNear = 26.0f;
                params.lodStep = 20.0f;
                break;
            default: break;   // Full: keep the 44 m / 34 m defaults
        }
    }

    // The editor's Birdseye toggle borrows this same topology, so the branch is
    // no longer deployment-only. Playtesting from the editor must still show
    // the gameplay clipmap, otherwise the level is being judged on terrain the
    // player will never be given.
    const bool editorBirdseye =
        g_game.session.Screen() == GameScreen::LevelEditor &&
        !g_levelEditor.IsPlaying() && g_levelEditor.BirdseyeEnabled();
    if (DeploymentPlanningActive() || editorBirdseye) {
        constexpr float kShoreOuter = 88.0f;
        constexpr float kOceanMargin = 40.0f;
        const float maxScale = (std::max)(params.islandScaleX,
                                          params.islandScaleZ);
        const float islandRadius = 43.0f * maxScale;
        const float deploymentRadius = g_customLevelMode
            ? g_game.world.Level().deploymentRadius
            : kDefaultDeploymentRadius;
        // The overview's orbit framing is what sets the view radius during
        // planning. The editor camera flies anywhere, so there is no orbit to
        // frame against: it takes the island-and-shore radius alone, which is
        // the same floor the planning path clamps against anyway.
        const float requiredRadius = editorBirdseye
            ? kShoreOuter * maxScale + kOceanMargin
            : (std::max)(
                  kShoreOuter * maxScale + kOceanMargin,
                  DeploymentPlanner::BuildCameraFrame(
                      islandRadius, deploymentRadius).terrainViewRadius);

        // A deployment overview is not a clipmap. Every tile is eight metres
        // wide and the mesh shader emits its complete 8x8 surface grid, giving
        // one-metre terrain vertices from the island centre through the entire
        // requested apron. This is a distinct topology from gameplay and does
        // not inherit exponentially larger outer-ring tiles.
        params.terrainStyle &= ~TerrainRendererDX12::kStyleClipmap;
        params.terrainStyle |=
            TerrainRendererDX12::kStyleDeploymentOverview;
        params.tileSize = DeploymentPlanner::DeploymentTerrainTileSize;
        params.tilesX = DeploymentPlanner::DeploymentTerrainGridSide(
            requiredRadius);
        params.tilesZ = params.tilesX;
        params.originTileX = 0;
        params.originTileZ = 0;
    }
    // Applied after both branches: flat mode is orthogonal to island style and
    // to the clipmap topology, so it ORs onto whichever they selected.
    if (scene.terrainFlat)
        params.terrainStyle |= TerrainRendererDX12::kStyleFlat;
    const bool deploymentOverview =
        (params.terrainStyle & TerrainRendererDX12::kStyleDeploymentOverview) != 0u;
    if (!deploymentOverview &&
        (scene.terrainErrorLOD || g_forceTerrainErrorLOD)) {
        params.terrainStyle |= TerrainRendererDX12::kStyleErrorLOD;
        switch (g_forwardQuality.Tier()) {
            case ForwardExtensionQualityTier::Balanced:    params.lodNear = 3.0f; break;
            case ForwardExtensionQualityTier::Performance: params.lodNear = 6.0f; break;
            default:                                       params.lodNear = 1.5f; break;
        }
        // Error mode combines this viewport height with projection[1][1] to
        // convert the measured world-space deviation into screen pixels.
        params.lodStep = static_cast<float>((std::max)(1u, SCR_HEIGHT));
    }
    return params;
}

// Blasts a sphere-shaped hole out of the military building when a grenade goes
// off against it. The prefab batch shares one model across every instance, so
// the first hit on an entity clones that mesh: without the clone, one grenade
// would hole every copy of the building in the level.
//
// Keyed by entity id, and the clone is kept so later grenades cut the already
// cratered geometry rather than starting from the pristine model.
static constexpr const char* kBlastHolePrefabId = "props/military_building_1";
// Radius of the bite taken out of a wall. Tuned against the building's own
// measured 15.2 x 5.1 x 10.3 m: big enough to read as a breach from outside,
// small enough that one grenade cannot open a whole wall.
static constexpr float kBlastHoleRadius = 1.35f;
static std::unordered_set<uint64_t> g_blastHoleEntities;

static void AddExplosionBuildingHole(const XMFLOAT3& impact, float blastScale) {
    const float radius = kBlastHoleRadius * (std::max)(0.35f, blastScale);
    for (PrefabRenderBatch& batch : g_game.world.Prefabs().renderBatches) {
        if (batch.prefabId != kBlastHolePrefabId || !batch.model) continue;
        for (size_t i = 0; i < batch.transforms.size() &&
                           i < batch.entityIds.size(); ++i) {
            const uint64_t entityId = batch.entityIds[i];
            // The cut runs in the model's own local space so a mesh can be cut
            // again later; bring the world-space impact back through the
            // instance transform rather than moving the geometry.
            XMVECTOR determinant;
            const XMMATRIX inverse =
                XMMatrixInverse(&determinant, batch.transforms[i]);
            if (XMVectorGetX(XMVectorAbs(determinant)) < 1e-8f) continue;
            XMFLOAT3 local;
            XMStoreFloat3(&local, XMVector3TransformCoord(
                XMLoadFloat3(&impact), inverse));

            // Scale the radius into local space too, or a scaled instance
            // would take a hole of the wrong size.
            XMFLOAT3 axis;
            XMStoreFloat3(&axis, XMVector3TransformNormal(
                XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f), batch.transforms[i]));
            const float instanceScale = (std::max)(1e-4f,
                std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z));

            SGE::SphereCut sphere;
            sphere.centreX = local.x;
            sphere.centreY = local.y;
            sphere.centreZ = local.z;
            sphere.radius = radius / instanceScale;

            // Clone on first damage so this instance stops sharing the cached
            // model every other copy of the building is still drawing. The
            // geometry hangs off child nodes -- the root carries no mesh of its
            // own -- so this has to deep-copy the whole subtree, not just the
            // node handed to the batch.
            const bool alreadyCut =
                g_blastHoleEntities.find(entityId) != g_blastHoleEntities.end();
            std::shared_ptr<SceneNode> target = batch.model;
            if (!alreadyCut) {
                std::function<std::shared_ptr<SceneNode>(
                    const std::shared_ptr<SceneNode>&)> cloneNode =
                    [&](const std::shared_ptr<SceneNode>& source)
                        -> std::shared_ptr<SceneNode> {
                    if (!source) return nullptr;
                    auto copy = std::make_shared<SceneNode>(*source);
                    if (source->mesh)
                        copy->mesh = std::make_shared<SceneMesh>(*source->mesh);
                    copy->children.clear();
                    for (const auto& child : source->children) {
                        auto childCopy = cloneNode(child);
                        if (childCopy) copy->AddChild(childCopy);
                    }
                    return copy;
                };
                target = cloneNode(batch.model);
            }
            if (!target) continue;

            // Cut every mesh in the subtree, not just the first one found: a
            // wall and its trim are separate primitives on separate nodes, and
            // a blast that only opened one of them would leave the other
            // hanging in the hole.
            bool cutAnything = false;
            std::function<void(const std::shared_ptr<SceneNode>&)> cutNode =
                [&](const std::shared_ptr<SceneNode>& node) {
                if (!node) return;
                if (node->mesh) {
                    for (MeshPrimitive& primitive : node->mesh->primitives) {
                        std::vector<float> cutVertices;
                        std::vector<unsigned int> cutIndices;
                        if (!SGE::CutSphereFromMesh(primitive.vertices,
                                primitive.indices, sphere, cutVertices,
                                cutIndices))
                            continue;
                        primitive.vertices = std::move(cutVertices);
                        primitive.indices = std::move(cutIndices);
                        // The old GPU buffers describe the uncut geometry, and
                        // the visibility buffer snapshots mesh offsets by slot,
                        // so the primitive re-registers rather than patching in
                        // place.
                        primitive.vertexBuffer.Reset();
                        primitive.indexBuffer.Reset();
                        primitive.visibilityMeshID = UINT_MAX;
                        primitive.indexCount =
                            static_cast<UINT>(primitive.indices.size());
                        GLBImporter::BuildMeshletData(
                            primitive, g_dx12.device.Get());
                        cutAnything = true;
                    }
                }
                for (const auto& child : node->children) cutNode(child);
            };
            cutNode(target);
            if (!cutAnything) continue;

            batch.model = target;
            batch.baseModel = target;
            g_blastHoleEntities.insert(entityId);
            SGE_LOG("LogPrefab", EngineLog::Level::Display,
                "Grenade blew a hole in " + std::string(kBlastHolePrefabId));
        }
    }
}

// blastScale widens the hole for oversized blasts (a called-in missile strike);
// ordinary explosions leave it at 1 and dig the usual grenade-sized crater.
static void AddExplosionTerrainCrater(const XMFLOAT3& impact,
                                      float blastScale = 1.0f) {
    if (!scene.useMeshTerrain || !g_terrain.supported) return;

    // Craters must land on any island size. The sculpt evaluator works in raw
    // world XZ (no island-scale term), so the stamp itself is scale-agnostic --
    // but a fixed 1 m dimple reads as nothing on a stretched island, and the
    // outer clipmap rings use coarse tiles that cannot resolve a small radius.
    // Grow the crater with the island so it stays visible at every scale.
    const TerrainRendererDX12::Params terrainParams = CurrentTerrainParams();
    const float islandScale = (std::max)(1.0f,
        (std::max)(terrainParams.islandScaleX, terrainParams.islandScaleZ));
    // sqrt keeps a 12x island's craters big enough to read without turning the
    // blast into a canyon; clamp so huge islands stay within the AS tile's
    // vertical reach (sculptMaxDisplacement feeds terrain_as.hlsl culling).
    const float craterScale = (std::min)(3.0f, sqrtf(islandScale));

    TerrainSculptStamp crater;
    crater.x = impact.x;
    crater.z = impact.z;
    crater.radius = scene.explosionBlastRadius * craterScale * blastScale;
    // Boolean-style cut rather than a smooth dent: flat floor, steep wall and
    // an ejecta lip, so the hole reads as a shape subtracted from the ground.
    crater.operation = TerrainSculptOperation::Crater;
    // Depth in metres, positive (the Crater op subtracts it). Follows the
    // square root of the width so a strike four times wider digs twice as
    // deep, keeping a big crater a bowl instead of a shaft.
    crater.value = scene.craterDepth * craterScale * sqrtf(blastScale);
    // Wall exponent: >1 holds the floor flat then turns up hard at the rim.
    crater.strength = scene.craterWallSharpness;
    // Fraction of the radius that stays flat floor before the wall starts.
    crater.edgeFalloff = scene.craterFloorFraction;
    // The height the floor is levelled against and the wall climbs back to.
    // Sampled once here rather than per-vertex so the floor comes out flat
    // even when the blast lands on a slope -- that flatness is what makes it
    // read as a cut shape instead of a dent following the hillside.
    // heightScale is applied here rather than taken from CurrentTerrainParams,
    // which leaves it at its default -- sampling without it would level the
    // floor against the wrong height and float or bury the crater.
    TerrainRendererDX12::Params heightParams = terrainParams;
    heightParams.heightScale = scene.terrainHeightScale;
    crater.baseHeight = TerrainRendererDX12::HeightAt(
        heightParams, impact.x, impact.z);
    // At capacity, drop the OLDEST crater rather than rejecting the new one --
    // a barrel that explodes must always leave a hole. SetSculptStamps trims
    // from the front too, so CPU collision and the GPU buffer stay in sync.
    {
        ProfilerDX12::CpuScope craterStampProfile(g_profiler, "Crater/Stamp");
        g_game.world.AddRuntimeTerrainStamp(crater);
        // Incremental: pushing the whole set back would re-walk every heightmap
        // stamp's residency, which on a baked level is what stalled the
        // explosion. Both sides trim the oldest at capacity, so they stay in
        // step.
        g_terrain.AddRuntimeSculptStamp(crater);
    }
    g_terrainDeformedThisFrame = true;
    {
        // Without this the Box3D heightfield keeps the pre-blast ground and
        // debris rests on a surface that is no longer there. Only the crater's
        // own cells are re-sampled; the lip reaches past the radius, so the
        // patch window matches the 1.18x the cut actually spans.
        ProfilerDX12::CpuScope craterPhysicsProfile(
            g_profiler, "Crater/Physics");
        g_destruction.RefreshTerrainRegion(impact.x, impact.z,
                                           crater.radius * 1.18f);
        // Drop any brick foundation the hole just undermined. The crater floor
        // is the new ground under it, and the flat part of the cut is what
        // actually removes support -- past that the wall climbs back to grade,
        // so a slab out there still has ground beneath it.
        g_destruction.UndermineSupports(
            impact, crater.radius * scene.craterFloorFraction,
            crater.baseHeight - crater.value);
    }
    {
        ProfilerDX12::CpuScope craterFoliageProfile(
            g_profiler, "Crater/Foliage");
        // Keep turf over ordinary soil cuts. Once the new crater floor reaches
        // the sand layer (or lower), clear the full visible cut so blades are
        // not left standing over exposed beach or submerged ground.
        if (g_grass.TerrainSandDominant(impact.x, impact.z)) {
            g_grass.AddRuntimeExclusion(
                impact.x, impact.z, crater.radius * 1.18f);
        }
        // Trees still use a tight radius so the blast does not fell every palm
        // out to the crater's ejecta lip.
        g_trees.ApplyExplosion(impact, crater.radius * 0.5f);
    }
}

// Gouges the ground where the BlackHawk went in. Three overlapping stamps laid
// along the heading rather than one circle: the wreck slides in nose-first at
// speed, so the scar it leaves is an elongated furrow, deepest at the point of
// impact and shallowing out ahead of it.
static void AddBlackHawkCrashCraters(const XMFLOAT3& impact, float yaw) {
    if (!scene.useMeshTerrain || !g_terrain.supported) return;

    // Same island-scale reasoning as the explosion crater: a fixed dimple
    // vanishes on a stretched island and the coarse outer clipmap rings cannot
    // resolve a small radius.
    const TerrainRendererDX12::Params terrainParams = CurrentTerrainParams();
    const float islandScale = (std::max)(1.0f,
        (std::max)(terrainParams.islandScaleX, terrainParams.islandScaleZ));
    const float scale = (std::min)(3.0f, sqrtf(islandScale));

    // Forward is (sin, cos), matching the rest of the BlackHawk code.
    const float forwardX = std::sin(yaw);
    const float forwardZ = std::cos(yaw);

    // Offset along the heading, radius, and depth. The middle stamp sits on the
    // impact point and bites deepest; the trailing one is where it first
    // touched down, the leading one where it ploughed to a stop.
    struct Gouge { float along, radius, depth; };
    static constexpr Gouge kGouges[] = {
        { -5.5f, 4.0f, -0.9f },
        {  0.0f, 5.5f, -1.7f },
        {  5.0f, 4.5f, -1.1f },
    };

    for (const Gouge& gouge : kGouges) {
        TerrainSculptStamp stamp;
        stamp.x = impact.x + forwardX * gouge.along * scale;
        stamp.z = impact.z + forwardZ * gouge.along * scale;
        stamp.radius = gouge.radius * scale;
        stamp.operation = TerrainSculptOperation::Add;
        stamp.value = gouge.depth * scale;
        stamp.strength = 1.0f;
        g_game.world.AddRuntimeTerrainStamp(stamp);
        g_terrain.AddRuntimeSculptStamp(stamp);

        // Match ordinary craters: the gouge only strips grass if its new floor
        // exposes the sand layer or drops below it.
        if (g_grass.TerrainSandDominant(stamp.x, stamp.z)) {
            g_grass.AddRuntimeExclusion(
                stamp.x, stamp.z, stamp.radius * 0.6f);
        }
        g_trees.ApplyExplosion({ stamp.x, impact.y, stamp.z },
                               stamp.radius * 0.6f);
    }
    // The three gouges went in above via AddRuntimeSculptStamp, which bumps the
    // revision each time; the buffer uploads once for the frame regardless.
    g_terrainDeformedThisFrame = true;
    {
        // One patch spanning the whole furrow rather than one per gouge: each
        // call rebuilds the collision shape, so three would pay that three
        // times over a region they largely share.
        ProfilerDX12::CpuScope gougePhysicsProfile(
            g_profiler, "Crater/Physics");
        float furthest = 0.0f;
        for (const Gouge& gouge : kGouges) {
            furthest = (std::max)(furthest,
                std::abs(gouge.along) * scale + gouge.radius * scale);
        }
        g_destruction.RefreshTerrainRegion(impact.x, impact.z, furthest);
    }
}

static size_t LiveBanditCount() {
    size_t count = 0;
    for (const auto& bandit : g_bandits)
        if (bandit && !bandit->Dead() && bandit->faction == Faction::Bandit) ++count;
    return count;
}

static size_t LiveMarineCount() {
    size_t count = 0;
    for (const auto& bandit : g_bandits)
        if (bandit && !bandit->Dead() && bandit->faction == Faction::Marine) ++count;
    return count;
}

// While the player is still aboard, enemies attack the transport rather than
// the camera point tucked inside it. A hull-centred aim point makes hits land
// on the craft's collision volume and feed its existing failure health.
static bool OccupiedInsertionVehicleTarget(XMFLOAT3& target) {
    const VehicleSystem& vehicles = g_game.vehicles;
    if (vehicles.blackHawkVisible && vehicles.blackHawkCarryingPlayer) {
        target = vehicles.blackHawkPosition;
        target.y += 2.2f;
        return true;
    }
    if (vehicles.insertionBoatVisible &&
        vehicles.insertionBoatCarryingPlayer) {
        target = vehicles.insertionBoatPosition;
        target.y -= vehicles.insertionBoatSinkOffset;
        return true;
    }
    return false;
}

// Player settings, loaded once at startup and rewritten whenever the player
// changes one. Whether the settings panel is currently open is UI state rather
// than a setting, so it is not persisted.
static GameSettings g_settings;
static bool g_showSettingsMenu = false;

// Multiplayer session state. Declared here rather than in Multiplayer.h,
// because Menus.h is included before Multiplayer.h and has to be able to
// start, report and stop a session -- so the session object and the panel's UI
// state have to exist before either file.
static net::NetSession g_netSession;
static bool g_showMultiplayerMenu = false;
// Edited by the host/join fields. Fixed char buffers rather than std::string
// because that is what ImGui::InputText writes into.
static char g_multiplayerJoinAddress[64] = "127.0.0.1";
static char g_multiplayerPort[8] = "27015";
// Last StartHost/StartClient failure. Kept separate from the live session's
// state so a failed attempt still has something to say after the session has
// gone back to Offline -- otherwise the panel reports "Offline" and the player
// never learns why the connect did not take.
static std::string g_multiplayerStatusError;

// Extraction payout, captured once as the win screen opens. Snapshotted rather
// than recomputed while the screen draws, because the wallet keeps moving --
// re-reading SessionEarned() every frame would be fine today but silently
// wrong the moment anything awards money outside a live run.
static int g_winScreenMissionBonus = 0;
static int64_t g_winScreenPayout = 0;

// Push the settings into the systems that actually consume them.
//
// Camera::MouseSensitivity is a member of the camera object, and the camera is
// wholesale-replaced on level load and editor entry (`scene.camera = Camera(...)`),
// which resets it to the constructor default. So this cannot be a one-time
// assignment at startup -- it has to be reapplied after anything that rebuilds
// the camera, or the player's sensitivity silently reverts when a level loads.
static void ApplyGameSettings() {
    scene.camera.MouseSensitivity = g_settings.mouseSensitivity;
    // Unlike the sensitivity above, these two survive a camera rebuild -- but
    // they are reapplied here anyway so every path that reloads settings ends
    // with the scene agreeing with the file.
    scene.seeThroughWeaponWhenAiming = g_settings.seeThroughWeaponWhenAiming;
    scene.seeThroughWeaponStrength = g_settings.seeThroughWeaponStrength;
}

// Player velocity in full 3D, differenced from the camera position each frame.
// Camera::VerticalVelocity covers only the Y axis, and the AA turret's lead
// solver already wanted a horizontal component it had no way to get -- so the
// strafe/run velocity is tracked here once and shared by every hostile gun.
static DirectX::XMFLOAT3 g_playerVelocity{};
static DirectX::XMFLOAT3 g_previousPlayerPosition{};
static bool g_playerVelocityValid = false;

static void UpdatePlayerVelocity(const DirectX::XMFLOAT3& position, float dt) {
    if (dt <= 0.0f) return;
    if (!g_playerVelocityValid) {
        g_previousPlayerPosition = position;
        g_playerVelocityValid = true;
        return;
    }
    const DirectX::XMFLOAT3 raw{ (position.x - g_previousPlayerPosition.x) / dt,
                                 (position.y - g_previousPlayerPosition.y) / dt,
                                 (position.z - g_previousPlayerPosition.z) / dt };
    g_previousPlayerPosition = position;
    // A teleport -- level load, respawn, entering or leaving a vehicle -- reads
    // as an enormous one-frame velocity. Discard it instead of leading a shot
    // at a point far off the map. 45 m/s sits above any sprint or fall speed
    // but below a genuine jump in position.
    const float speedSq = raw.x * raw.x + raw.y * raw.y + raw.z * raw.z;
    if (speedSq > 45.0f * 45.0f) {
        g_playerVelocity = {};
        return;
    }
    // Exponential smoothing, ~0.16s time constant, frame-rate independent. Raw
    // per-frame deltas are noisy enough that unfiltered lead makes shots jitter
    // around the player rather than track them.
    const float blend = 1.0f - std::exp(-dt / 0.16f);
    g_playerVelocity.x += (raw.x - g_playerVelocity.x) * blend;
    g_playerVelocity.y += (raw.y - g_playerVelocity.y) * blend;
    g_playerVelocity.z += (raw.z - g_playerVelocity.z) * blend;
}

// Aim point that puts a round on `target` given where it is heading. Two
// fixed-point iterations, matching VehicleSystem::AATurretLeadPoint: the flight
// time barely moves once the aim point shifts, so this converges immediately.
static DirectX::XMFLOAT3 LeadTargetPoint(const DirectX::XMFLOAT3& muzzle,
                                         const DirectX::XMFLOAT3& target,
                                         const DirectX::XMFLOAT3& velocity,
                                         float projectileSpeed) {
    DirectX::XMFLOAT3 aim = target;
    if (projectileSpeed <= 0.001f) return aim;
    for (int i = 0; i < 2; ++i) {
        const float dx = aim.x - muzzle.x;
        const float dy = aim.y - muzzle.y;
        const float dz = aim.z - muzzle.z;
        const float t = std::sqrt(dx * dx + dy * dy + dz * dz) / projectileSpeed;
        aim = { target.x + velocity.x * t,
                target.y + velocity.y * t,
                target.z + velocity.z * t };
    }
    return aim;
}

// Nearest position this actor should perceive/aim/shoot at: bandits target the
// nearest of {player, each live marine}; marines target the nearest live bandit
// and never the player. Ties favor the player so behavior is unchanged when no
// marines are alive/nearby.
static DirectX::XMFLOAT3 NearestHostileTarget(const SkinnedEnemy& actor,
                                               const DirectX::XMFLOAT3& playerPosition,
                                               const std::vector<DirectX::XMFLOAT3>& marinePositions,
                                               const std::vector<DirectX::XMFLOAT3>& banditPositions) {
    auto distSq = [&](const DirectX::XMFLOAT3& p) {
        const float dx = p.x - actor.position.x;
        const float dy = p.y - actor.position.y;
        const float dz = p.z - actor.position.z;
        return dx * dx + dy * dy + dz * dz;
    };
    if (actor.faction == Faction::Bandit) {
        DirectX::XMFLOAT3 best = playerPosition;
        float bestDistSq = distSq(best);
        for (const auto& p : marinePositions) {
            const float d = distSq(p);
            if (d < bestDistSq) { bestDistSq = d; best = p; }
        }
        return best;
    }
    // Marine: nearest live bandit, or its own current position (no perceivable
    // target) if none are alive. Candidate positions are torso-height, so the
    // no-target fallback matches -- PerceivePlayer treats a target at its own
    // position as unperceivable either way, but keeping the height consistent
    // avoids a phantom vertical offset in the distance test.
    DirectX::XMFLOAT3 best = actor.position;
    float bestDistSq = FLT_MAX;
    for (const auto& p : banditPositions) {
        const float d = distSq(p);
        if (d < bestDistSq) { bestDistSq = d; best = p; }
    }
    return best;
}

struct UploadHeapReleaseState {
    bool prepared = false;
    size_t totalChunks = 0;
    uint64_t usedBytes = 0;
    std::vector<std::shared_ptr<SceneMaterial>> materials;

    void Reset() {
        prepared = false;
        totalChunks = 0;
        usedBytes = 0;
        materials.clear();
    }
};
static UploadHeapReleaseState g_uploadHeapRelease;

static void CollectMaterialUploadHeaps(
    const std::shared_ptr<SceneNode>& node,
    std::unordered_set<const SceneNode*>& visitedNodes,
    std::unordered_set<const SceneMaterial*>& visitedMaterials) {
    if (!node || !visitedNodes.insert(node.get()).second) return;
    if (node->mesh) {
        for (const MeshPrimitive& primitive : node->mesh->primitives) {
            const auto& material = primitive.material;
            if (!material || !visitedMaterials.insert(material.get()).second)
                continue;
            g_uploadHeapRelease.materials.push_back(material);
        }
    }
    for (const auto& child : node->children)
        CollectMaterialUploadHeaps(child, visitedNodes, visitedMaterials);
}

static void CollectMaterialUploadHeaps(
    const std::shared_ptr<SceneMaterial>& material,
    std::unordered_set<const SceneMaterial*>& visitedMaterials) {
    if (!material || !visitedMaterials.insert(material.get()).second) return;
    g_uploadHeapRelease.materials.push_back(material);
}

static size_t LiveRespawningBanditCount() {
    size_t count = 0;
    for (const auto& bandit : g_bandits)
        if (bandit && !bandit->Dead() && bandit->spawnSlot >= 0) ++count;
    return count;
}
