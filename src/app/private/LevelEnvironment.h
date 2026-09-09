#pragma once

// Private application implementation; included once by main.cpp in dependency order.

// Defined below; the navmesh extent needs it before that point.
static float CurrentPhysicsTerrainExtent();

static void RebuildScalableEnvironment() {
    ProfilerDX12::CpuScope environmentProfile(g_profiler, "Editor/Environment");
    auto terrainParams = CurrentTerrainParams();
    terrainParams.heightScale = scene.terrainHeightScale;
    auto terrainSampler = [terrainParams](float x, float z) {
        return TerrainRendererDX12::HeightAt(terrainParams, x, z);
    };
    std::vector<NavigationObstacle> obstacles = {
        { -2.6f,  -1.5f,  2.6f,   1.5f },
        {-29.0f, -27.0f,-15.0f, -13.0f }
    };
    std::vector<GrassField::Exclusion> grassExclusions;
    std::vector<GrassField::AuthoredPatch> grassPatches;
    // Painted clearings apply to built-in levels too, so they are gathered
    // outside the custom-level branch below.
    for (const FoliageClearStamp& stamp : g_game.world.Level().foliageClear) {
        if (stamp.radius <= 0.0f) continue;
        grassExclusions.push_back({
            stamp.x - stamp.radius, stamp.z - stamp.radius,
            stamp.x + stamp.radius, stamp.z + stamp.radius,
            stamp.x, stamp.z, stamp.radius });
    }
    if (g_customLevelMode) {
        obstacles.clear();
        obstacles.push_back({-29.0f, -27.0f, -15.0f, -13.0f});
        for (const LevelEntity& entity : g_game.world.Level().entities) {
            if (!entity.enabled) continue;
            if (entity.type == LevelEntityType::WoodHouse ||
                entity.type == LevelEntityType::MetalHouse) {
                const float yaw = XMConvertToRadians(entity.transform.rotation[1]);
                const float c = std::abs(std::cos(yaw));
                const float s = std::abs(std::sin(yaw));
                const float halfX = c * 4.2f + s * 3.4f;
                const float halfZ = s * 4.2f + c * 3.4f;
                NavigationObstacle house{ entity.transform.position[0] - halfX,
                    entity.transform.position[2] - halfZ,
                    entity.transform.position[0] + halfX,
                    entity.transform.position[2] + halfZ };
                obstacles.push_back(house);
                grassExclusions.push_back({ house.minX - 0.4f, house.minZ - 0.4f,
                    house.maxX + 0.4f, house.maxZ + 0.4f });
            } else if (entity.type == LevelEntityType::Palm) {
                obstacles.push_back({ entity.transform.position[0] - 0.55f,
                    entity.transform.position[2] - 0.55f,
                    entity.transform.position[0] + 0.55f,
                    entity.transform.position[2] + 0.55f });
            } else if (entity.type == LevelEntityType::Humvee) {
                const float yaw = XMConvertToRadians(entity.transform.rotation[1]);
                const float halfX = std::abs(std::cos(yaw)) * 2.6f +
                    std::abs(std::sin(yaw)) * 1.5f;
                const float halfZ = std::abs(std::sin(yaw)) * 2.6f +
                    std::abs(std::cos(yaw)) * 1.5f;
                obstacles.push_back({ entity.transform.position[0] - halfX,
                    entity.transform.position[2] - halfZ,
                    entity.transform.position[0] + halfX,
                    entity.transform.position[2] + halfZ });
            } else if (entity.type == LevelEntityType::GrassPatch) {
                grassPatches.push_back({ entity.transform.position[0],
                    entity.transform.position[2],
                    (std::max)(0.15f, entity.transform.scale[0]),
                    (std::max)(0.05f, entity.transform.scale[1]),
                    static_cast<uint32_t>(entity.id) });
            } else if (entity.type == LevelEntityType::Prefab ||
                       entity.type == LevelEntityType::Rock) {
                // Navigation obstacles are axis-aligned rectangles, so a
                // mesh-collision prefab still contributes its bounds box here
                // and this loop does not filter g_meshCollisionEntities.
                // Consequence for the airport: enemies will not path inside it.
                // Turning the triangle mesh into walkable navmesh needs
                // rectangle decomposition, which is deferred follow-up work --
                // this is the behaviour that already shipped, not a regression.
                //
                // Grass no longer follows that box. The airport's bounds span
                // the whole apron, so excluding by box stripped grass from
                // every metre of open ground inside it; the triangle mesh is
                // asked directly instead, below.
                const bool meshCollision =
                    g_meshCollisionEntities.count(entity.id) > 0;
                for (const PrefabCollider& collider : g_prefabColliders) {
                    if (collider.entityId != entity.id) continue;
                    const float c = std::abs(std::cos(collider.yawRadians));
                    const float s = std::abs(std::sin(collider.yawRadians));
                    const float halfX = c * collider.halfExtents.x +
                                        s * collider.halfExtents.z;
                    const float halfZ = s * collider.halfExtents.x +
                                        c * collider.halfExtents.z;
                    obstacles.push_back({ collider.center.x - halfX,
                        collider.center.z - halfZ, collider.center.x + halfX,
                        collider.center.z + halfZ });
                    if (meshCollision) continue;
                    grassExclusions.push_back({ collider.center.x - halfX,
                        collider.center.z - halfZ, collider.center.x + halfX,
                        collider.center.z + halfZ });
                }
            }
        }
    } else {
    const size_t houseCount = g_stressTestMode ? kStressHouseCount : 4;
    size_t houseIndex = 0;
    const size_t compoundCount = (houseCount + kSpawnersPerCompound - 1) /
        kSpawnersPerCompound;
    for (size_t i = 0; i < compoundCount; ++i) {
        const float x = kStressCompoundCenters[i].x;
        const float z = kStressCompoundCenters[i].z;
        const NavigationObstacle houses[kSpawnersPerCompound] = {
            {x - 4.2f, z + 5.8f, x + 4.2f, z + 12.2f},
            {x + 5.8f, z - 3.4f, x + 12.2f, z + 3.4f},
            {x - 4.2f, z - 12.2f, x + 4.2f, z - 5.8f},
            {x - 12.2f, z - 3.4f, x - 5.8f, z + 3.4f}
        };
        for (size_t side = 0;
             side < kSpawnersPerCompound && houseIndex < houseCount;
             ++side, ++houseIndex) {
            obstacles.push_back(houses[side]);
            constexpr float grassClearance = 0.4f;
            grassExclusions.push_back({
                houses[side].minX - grassClearance,
                houses[side].minZ - grassClearance,
                houses[side].maxX + grassClearance,
                houses[side].maxZ + grassClearance });
        }
    }
    if (g_stressTestMode) {
        obstacles.push_back({39.4f, 1.5f, 44.6f, 4.5f});
        grassExclusions.push_back({39.0f, 1.1f, 45.0f, 4.9f});
    }
    // Expose the material paths authored in terrain_pbr.hlsli. Two narrow
    // crossing clearings connect the four house fronts and give the compound a
    // readable route instead of an uninterrupted foreground grass wall.
    grassExclusions.push_back({-1.15f, -15.0f, 1.15f, 15.0f});
    grassExclusions.push_back({-15.0f, -1.15f, 15.0f, 1.15f});
    for (const PalmSpawn& palm : kPalmSpawns)
        obstacles.push_back(
            { palm.x - 0.55f, palm.z - 0.55f,
              palm.x + 0.55f, palm.z + 0.55f });
    if (g_stressTestMode) {
        for (const PalmSpawn& palm : kStressHousePalmSpawns)
            obstacles.push_back(
                { palm.x - 0.55f, palm.z - 0.55f,
                  palm.x + 0.55f, palm.z + 0.55f });
    }
    }
    // A custom island is authored at its own scale, so a fixed extent cannot
    // cover it: BigIslandv22 stretches to islandScale 3.9 and places props out
    // to |Z| 135 m, well past the 122 m this used to build. Everything beyond
    // the edge simply had no navmesh under it, so enemies fell back to direct
    // steering there and the editor overlay showed bare ground.
    //
    // Follow the island the way the ocean and crater sizing already do
    // (kShoreOuter * islandScale), and take whichever is larger: the island
    // itself, or the spread of the placed entities plus a margin.
    float extent = g_stressTestMode ? 122.0f : 61.0f;
    if (g_customLevelMode) {
        constexpr float kShoreOuter = 88.0f;
        const float islandScale = (std::max)(1.0f,
            (std::max)(terrainParams.islandScaleX, terrainParams.islandScaleZ));
        extent = (std::max)({ 122.0f, kShoreOuter * islandScale,
                              CurrentPhysicsTerrainExtent() });
    }
    {
        ProfilerDX12::CpuScope navmeshProfile(g_profiler, "Editor/Navmesh");
        if (!g_navigation.BuildTerrain(terrainSampler, -extent, extent,
                -extent, extent, obstacles))
            std::cerr << "Recast navigation build failed; Bandits use direct steering\n";
    }

    // Painted ground steers the scatter: paint rock or sand and grass stops
    // growing there. Must use the same world->UV frame as the resolve and the
    // paint brush (kShoreOuter scaled per axis) or paint lands in one place and
    // grass thins out in another. An unpainted level supplies nothing here and
    // keeps the purely procedural scatter.
    {
        const LevelDefinition& grassLevel = g_game.world.Level();
        constexpr float kShoreOuter = 88.0f;
        const size_t expectedSplatBytes =
            static_cast<size_t>(grassLevel.terrainSplatResolution) *
            grassLevel.terrainSplatResolution * 4u;
        if (grassLevel.terrainSplatResolution > 0 &&
            grassLevel.terrainSplatRGBA.size() == expectedSplatBytes) {
            g_grass.SetSplatMap(grassLevel.terrainSplatRGBA.data(),
                                grassLevel.terrainSplatResolution,
                                kShoreOuter * grassLevel.terrainIslandScaleX,
                                kShoreOuter * grassLevel.terrainIslandScaleZ);
        } else {
            g_grass.SetSplatMap(nullptr, 0, 0.0f, 0.0f);
        }
    }
    // Scatter span follows the island the level actually authored, rather than
    // a fixed 100 m. The playable ground is kShoreOuter (88 m) scaled per axis,
    // and islands are stretched independently: islandv3 runs scaleZ = 1.38, so
    // it reaches +/-121 m on Z while a 100 m span only scattered to +/-50 -- the
    // far half of the map came out bare no matter what the splatmap painted.
    //
    // Square span covering the LONGER axis, since BuildBlades scatters a square
    // and the per-tuft material test rejects whatever falls off the island.
    float grassSpan = g_stressTestMode ? 200.0f : 100.0f;
    // 2.4M, tripled from 800k (itself doubled from the original 400k). The
    // material gate was never what thinned the field: measured over a 100 m
    // island, grass is the dominant layer on 88% of the land above the
    // waterline, and since GrassTerrainDensity was remapped to saturate the
    // interior, essentially every tuft there survives it. The gaps were always
    // the blade budget. Coverage by budget, at 8 blades per 0.18 m tuft:
    //
    //   400k  ->  4.0 tufts/m2, 0.50 m apart,  40% of the ground
    //   800k  ->  8.8 tufts/m2, 0.34 m apart,  90%
    //   2.4M  -> 26.4 tufts/m2, 0.19 m apart, 269%
    //
    // Past 100% the tufts overlap, which is the point: overlap is what makes a
    // field read as continuous turf rather than as separate clumps with ground
    // visible between them. It costs ~110 MB of instance buffer at 48 bytes a
    // blade, up from ~37 MB.
    // No turf on the base: it is a hard-standing compound, and the same flat
    // terrain that defeats the dandelion rejection tests keeps every one of
    // these blades alive too. Measured at 28 ms of GPU a frame in the grass
    // pass alone, plus ~110 MB of instance buffer, for a lawn a hub does not
    // want in the first place.
    int grassCount = g_baseMode ? 0 : (g_stressTestMode ? 1600000 : 2400000);
    if (!g_stressTestMode && !g_baseMode) {
        const LevelDefinition& spanLevel = g_game.world.Level();
        constexpr float kShoreOuter = 88.0f;
        const float islandSpan = 2.0f * kShoreOuter * (std::max)(
            spanLevel.terrainIslandScaleX, spanLevel.terrainIslandScaleZ);
        if (islandSpan > grassSpan) {
            // Hold tuft density roughly constant as the area grows, so a long
            // island is not covered by the same budget stretched thin. Capped
            // so a very large authored island cannot blow up the vertex count.
            //
            // The cap scales with the tripled base, preserving the four-times
            // headroom the original 400k/1.6M pair had. Memory is the only
            // thing it trades, and the instance buffer is cheap per blade:
            // 9.6M blades is ~440 MB resident, which a modern GPU carries
            // without complaint.
            //
            // Note the cap governs MEMORY, not draw cost -- those are separate.
            // The cell cull skips draw calls beyond DrawDistance (60 m), but
            // every blade stays resident in the one static instance buffer
            // whether it is submitted or not. On a default 100 m island the
            // 60 m disc covers the whole field, so nothing is culled at all;
            // culling only starts paying on larger maps (about 19% of a
            // scale-1.38 island is inside the disc at any moment).
            const float areaRatio = (islandSpan * islandSpan) /
                                    (grassSpan * grassSpan);
            grassCount = static_cast<int>((std::min)(
                static_cast<float>(grassCount) * areaRatio, 9600000.0f));
            grassSpan = islandSpan;
        }
    }
    // Mesh-collision prefabs are rejected by their triangles rather than their
    // bounds box (see the exclusion loop above). A short vertical ray at the
    // tuft centre answers the only question that matters: is there prefab
    // geometry -- tarmac, a hangar floor, a wall -- at this spot. Concrete gets
    // no grass; the open ground beside it does. The span is deliberately small
    // either side of the terrain height so a roof twenty metres up does not
    // shadow the ground beneath it.
    auto meshBlocked = [&terrainSampler](float x, float z) {
        if (g_prefabMeshColliders.empty()) return false;
        const float y = terrainSampler(x, z);
        const XMFLOAT3 start{ x, y + 1.6f, z };
        const XMFLOAT3 end{ x, y - 0.6f, z };
        CollisionMeshRayHit hit;
        for (const CollisionMeshInstance& instance : g_prefabMeshColliders) {
            if (CollisionMeshInstanceRaycast(instance, start, end, 0.0f, hit))
                return true;
        }
        return false;
    };
    {
        ProfilerDX12::CpuScope grassProfile(g_profiler, "Editor/GrassScatter");
        g_grass.Initialize(terrainSampler, grassSpan, grassCount,
            0.0f, grassExclusions, grassPatches,
            !g_customLevelMode, meshBlocked);
    }
    ScatterDandelions(terrainSampler, grassExclusions);
    g_environmentInitialized = true;
    g_environmentStressMode = g_stressTestMode;
}
