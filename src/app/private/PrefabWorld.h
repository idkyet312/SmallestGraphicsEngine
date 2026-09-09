#pragma once

// Private application implementation; included once by main.cpp in dependency order.

// Drops one destroyed entity out of the prefab runtime lists in place.
//
// A blast that destroys a prop used to set g_prefabRebuildRequested, which runs
// RebuildPrefabRenderBatches: it reloads every prefab model, re-derives every
// collider and re-initializes every audio player for the whole level. Measured
// at 385 ms and, in one case, 19 s -- the entire cost of the grenade hitch.
//
// Nothing about that work is needed to stop drawing a single prop. Every prefab
// runtime list is keyed by entityId, so the entity is removed directly. Batches
// keep transforms and ids as parallel arrays, so the erase has to hit both at
// the same index or instances would render with another entity's transform.
//
// Returns true if anything was actually removed, so callers can tell a
// no-op (an entity that was never batched) from a real removal.
static bool RemovePrefabEntityFromRuntime(uint64_t entityId) {
    bool removed = false;
    for (PrefabRenderBatch& batch : g_prefabRenderBatches) {
        for (size_t i = batch.entityIds.size(); i-- > 0;) {
            if (batch.entityIds[i] != entityId) continue;
            batch.entityIds.erase(batch.entityIds.begin() + i);
            // transforms and baseTransforms are parallel to entityIds, but a
            // batch can legitimately carry fewer of them (the Cornell box seeds
            // one transform with a synthetic id), so guard each.
            if (i < batch.transforms.size())
                batch.transforms.erase(batch.transforms.begin() + i);
            if (i < batch.baseTransforms.size())
                batch.baseTransforms.erase(batch.baseTransforms.begin() + i);
            removed = true;
        }
    }
    const auto dropById = [&](auto& container) {
        const size_t before = container.size();
        container.erase(
            std::remove_if(container.begin(), container.end(),
                [entityId](const auto& item) {
                    return item.entityId == entityId;
                }),
            container.end());
        if (container.size() != before) removed = true;
    };
    dropById(g_prefabColliders);
    // The body has to be destroyed before the state is dropped, or the handle
    // is lost and the body simulates forever with nothing drawing it.
    for (const PrefabRigidBodyState& state : g_prefabRigidBodies)
        if (state.entityId == entityId)
            g_destruction.DestroyPropBody(state.physicsHandle);
    dropById(g_prefabRigidBodies);
    dropById(g_prefabMeshColliders);
    dropById(g_prefabLightInstances);
    dropById(g_prefabDestructibles);
    dropById(g_prefabSpawnPoints);
    dropById(g_prefabArmoryShops);
    // Boarding points go with the entity for the same reason the counters do:
    // the open travel board holds an index into this list, so a point left
    // behind after its aircraft is gone is both a prompt on empty air and a
    // stale index the panel would read.
    dropById(g_prefabTravelPoints);
    g_meshCollisionEntities.erase(entityId);
    g_prefabHealth.erase(entityId);

    // Audio players index into g_prefabAudioEmitters, so the emitters cannot be
    // erased without invalidating those indices. Stop and drop this entity's
    // players first, then erase the emitters back to front and fix up the
    // indices that shifted.
    for (size_t emitterIndex = g_prefabAudioEmitters.size();
         emitterIndex-- > 0;) {
        if (g_prefabAudioEmitters[emitterIndex].entityId != entityId) continue;
        g_prefabAudioPlayers.erase(
            std::remove_if(g_prefabAudioPlayers.begin(),
                g_prefabAudioPlayers.end(),
                [emitterIndex](const PrefabAudioPlayer& player) {
                    return player.emitterIndex == emitterIndex;
                }),
            g_prefabAudioPlayers.end());
        g_prefabAudioEmitters.erase(
            g_prefabAudioEmitters.begin() + emitterIndex);
        for (PrefabAudioPlayer& player : g_prefabAudioPlayers)
            if (player.emitterIndex > emitterIndex) --player.emitterIndex;
        removed = true;
    }

    // The lights list feeds the clustered renderer, which is only refreshed by
    // a rebuild. Re-push what is left rather than leaving a dead light lit.
    if (removed) RebuildRuntimePrefabLights();
    return removed;
}

static void RebuildRuntimePrefabLights() {
    scene.RebuildDemoLights();
    if (g_ddgiCornellTestMode) {
        scene.clusteredRenderer.clearLights();
        scene.clusteredRenderer.addLight(
            g_ddgiCornellLightPosition, g_ddgiCornellLightColor,
            g_ddgiCornellLightRadius, g_ddgiCornellLightIntensity);
    }
    for (const PrefabLightInstance& light : g_prefabLightInstances)
        scene.clusteredRenderer.addLight(light.position, light.color,
                                         light.radius, light.intensity);
}

// Recompile only the prefab draw list from models already retained by the
// cache. Gameplay components deliberately remain untouched until Save/Play.
// Loading a model is postponed to BeginFrame, where upload commands can be
// recorded without idling or replacing resources referenced by older frames.
static void RebuildEditorPrefabVisualBatches(bool loadMissingModels) {
    if (!IsEditorEditing()) return;

    std::vector<PrefabRenderBatch> visualBatches;
    std::vector<PrefabLightInstance> visualLights;
    for (const PrefabRenderBatch& batch : g_prefabRenderBatches) {
        if (batch.prefabId.rfind("__", 0) == 0 &&
            batch.prefabId.rfind("__editor_", 0) != 0)
            visualBatches.push_back(batch);
    }
    std::unordered_map<std::string, size_t> batches;
    bool missingVisual = false;

    const auto addInstance = [&](const auto& self, const std::string& prefabId,
                                 const XMMATRIX& world, uint64_t entityId,
                                 const nlohmann::json& overrides,
                                 unsigned depth) -> void {
        if (depth > 16) return;
        const PrefabAsset* prefab = g_prefabRegistry.Find(prefabId);
        if (!prefab) return;
        PrefabModelCacheEntry* cachedModel = nullptr;
        const auto cached = g_prefabModelCache.find(prefab->id);
        if (cached != g_prefabModelCache.end())
            cachedModel = &cached->second;
        else if (loadMissingModels)
            cachedModel = LoadPrefabModel(*prefab);
        if (!cachedModel) {
            missingVisual = true;
            return;
        }

        const nlohmann::json components = MergePrefabComponents(
            prefab->components, overrides);
        const bool castShadow = components.contains("staticMesh")
            ? components.at("staticMesh").value("castShadow", prefab->castShadow)
            : prefab->castShadow;
        const bool drawnByRuntimeSystem =
            components.contains("aaTurret") ||
            (IsNvBlastStructurePrefab(prefabId) && scene.useDestruction &&
             g_destruction.IsInitialized());
        if (!drawnByRuntimeSystem) {
            const std::string key = prefabId +
                (castShadow ? "#shadow" : "#noshadow");
            size_t batchIndex = 0;
            const auto found = batches.find(key);
            if (found == batches.end()) {
                batchIndex = visualBatches.size();
                batches.emplace(key, batchIndex);
                PrefabRenderBatch batch;
                batch.prefabId = prefabId;
                batch.model = cachedModel->model;
                batch.baseModel = cachedModel->model;
                batch.castShadow = castShadow;
                batch.automaticLod = prefab->automaticLod && prefab->lods.empty();
                if (batch.automaticLod) batch.lods = cachedModel->automaticLods;
                XMStoreFloat3(&batch.lodCenter, (XMLoadFloat3(&cachedModel->boundsMinimum) +
                    XMLoadFloat3(&cachedModel->boundsMaximum)) * 0.5f);
                batch.lodRadius = XMVectorGetX(XMVector3Length(
                    XMLoadFloat3(&cachedModel->boundsMaximum) - XMLoadFloat3(&cachedModel->boundsMinimum))) * 0.5f;
                for (size_t lodIndex = 0; lodIndex < prefab->lods.size(); ++lodIndex) {
                    const std::string lodId = prefab->id + "#lod" +
                        std::to_string(lodIndex);
                    auto lodCached = g_prefabModelCache.find(lodId);
                    if (lodCached == g_prefabModelCache.end() && loadMissingModels) {
                        PrefabAsset lodPrefab = *prefab;
                        lodPrefab.id = lodId;
                        lodPrefab.modelPath = prefab->lods[lodIndex].path;
                        lodPrefab.modelGuid = prefab->lods[lodIndex].assetGuid;
                        lodPrefab.lods.clear();
                        lodPrefab.automaticLod = false;
                        LoadPrefabModel(lodPrefab);
                        lodCached = g_prefabModelCache.find(lodId);
                    }
                    if (lodCached != g_prefabModelCache.end())
                        batch.lods.push_back({ prefab->lods[lodIndex].distance,
                                               lodCached->second.model });
                }
                visualBatches.push_back(std::move(batch));
            } else {
                batchIndex = found->second;
            }
            PrefabRenderBatch& batch = visualBatches[batchIndex];
            batch.baseTransforms.push_back(world);
            batch.transforms.push_back(world);
            batch.entityIds.push_back(entityId);
        }


        XMFLOAT3 origin;
        XMStoreFloat3(&origin,
            XMVector3TransformCoord(XMVectorZero(), world));
        if (components.contains("light")) {
            const auto& light = components.at("light");
            const auto color = light.value(
                "color", std::vector<float>{ 1.0f, 1.0f, 1.0f });
            if (color.size() == 3) {
                visualLights.push_back({ entityId, origin,
                    { color[0], color[1], color[2] },
                    light.value("intensity", 1.0f),
                    light.value("radius", 5.0f) });
            }
        }

        for (const PrefabChildAsset& child : prefab->children) {
            const XMMATRIX childLocal = XMMatrixScaling(
                child.scale[0], child.scale[1], child.scale[2]) *
                EulerDegreesToMatrix(child.rotation) *
                XMMatrixTranslation(child.position[0], child.position[1],
                                    child.position[2]);
            self(self, child.prefabId, childLocal * world, entityId,
                 nlohmann::json::object(), depth + 1);
        }
    };

    if (g_houseTemplate &&
        (!g_editorWoodHousePreviewModel || !g_editorMetalHousePreviewModel)) {
        const auto meanX = [](const std::shared_ptr<SceneNode>& node) {
            double sum = 0.0;
            size_t count = 0;
            if (node && node->mesh) {
                for (const MeshPrimitive& primitive : node->mesh->primitives) {
                    for (size_t vertex = 0; vertex + 11 < primitive.vertices.size();
                         vertex += 12) {
                        sum += primitive.vertices[vertex];
                        ++count;
                    }
                }
            }
            return count ? static_cast<float>(sum / count) : 0.0f;
        };

        auto woodSource =
            std::make_shared<SceneNode>("EditorWoodHousePreviewSource");
        auto metalSource =
            std::make_shared<SceneNode>("EditorMetalHousePreviewSource");
        for (const std::shared_ptr<SceneNode>& child : g_houseTemplate->children) {
            if (!child || !child->mesh) continue;
            (meanX(child) < 1.0f ? woodSource : metalSource)
                ->AddChild(CloneSceneNodeShallow(child));
        }
        XMFLOAT4X4 identity;
        XMStoreFloat4x4(&identity, XMMatrixIdentity());
        woodSource->UpdateGlobalTransform(identity);
        metalSource->UpdateGlobalTransform(identity);

        // The house template is authored for Blast and contains CPU geometry;
        // DestructionDX12 uploads private render batches without filling these
        // SceneMesh buffer views. Merge once here to create the ordinary GPU
        // mesh that the editor's forward-extension path can actually draw.
        const auto gpuReady = [](const std::shared_ptr<SceneNode>& model) {
            if (!model || !model->mesh || model->mesh->primitives.empty())
                return false;
            return std::all_of(model->mesh->primitives.begin(),
                model->mesh->primitives.end(), [](const MeshPrimitive& primitive) {
                    return primitive.vbv.BufferLocation != 0 &&
                           primitive.ibv.BufferLocation != 0 &&
                           primitive.indexCount != 0;
                });
        };
        if (!g_editorWoodHousePreviewModel) {
            auto candidate = GLBImporter::MergeSceneByMaterial(
                woodSource, g_dx12.device);
            if (gpuReady(candidate))
                g_editorWoodHousePreviewModel = std::move(candidate);
        }
        if (!g_editorMetalHousePreviewModel) {
            auto candidate = GLBImporter::MergeSceneByMaterial(
                metalSource, g_dx12.device);
            if (gpuReady(candidate))
                g_editorMetalHousePreviewModel = std::move(candidate);
        }
    }

    PrefabRenderBatch woodHouses;
    woodHouses.prefabId = "__editor_wood_house";
    woodHouses.model = g_editorWoodHousePreviewModel;
    woodHouses.baseModel = g_editorWoodHousePreviewModel;
    PrefabRenderBatch metalHouses;
    metalHouses.prefabId = "__editor_metal_house";
    metalHouses.model = g_editorMetalHousePreviewModel;
    metalHouses.baseModel = g_editorMetalHousePreviewModel;

    for (const LevelEntity& entity : g_levelEditor.Level().entities) {
        if (!entity.enabled) continue;
        if (entity.type == LevelEntityType::WoodHouse ||
            entity.type == LevelEntityType::MetalHouse) {
            PrefabRenderBatch& batch = entity.type == LevelEntityType::WoodHouse
                ? woodHouses : metalHouses;
            if (!batch.model) {
                // House previews come from the destructible house template,
                // which finishes loading after the editor itself opens. Keep a
                // manual refresh armed until that template is available just
                // as we do for an uncached prefab model above.
                missingVisual = true;
                continue;
            }
            const float sourceX = entity.type == LevelEntityType::WoodHouse
                ? -3.5f : 4.75f;
            const float sourceZ = entity.type == LevelEntityType::WoodHouse
                ? 3.5f : 3.55f;
            const XMMATRIX world = XMMatrixTranslation(-sourceX, 0.0f, -sourceZ) *
                XMMatrixRotationY(XMConvertToRadians(
                    entity.transform.rotation[1])) *
                XMMatrixTranslation(entity.transform.position[0],
                    entity.transform.position[1], entity.transform.position[2]);
            batch.baseTransforms.push_back(world);
            batch.transforms.push_back(world);
            batch.entityIds.push_back(entity.id);
            continue;
        }
        std::string prefabId;
        if (entity.type == LevelEntityType::Prefab) prefabId = entity.prefabId;
        else if (entity.type == LevelEntityType::Rock) prefabId = "rock";
        else continue;
        const Transform& transform = entity.transform;
        const XMMATRIX world = EntityWorldMatrix(transform);
        addInstance(addInstance, prefabId, world, entity.id, entity.overrides, 0);
    }

    if (!woodHouses.transforms.empty())
        visualBatches.push_back(std::move(woodHouses));
    if (!metalHouses.transforms.empty())
        visualBatches.push_back(std::move(metalHouses));

    g_prefabRenderBatches = std::move(visualBatches);
    g_prefabLightInstances = std::move(visualLights);
    RebuildRuntimePrefabLights();
    g_editorVisualRefreshRequested = missingVisual;
    shadowMap.InvalidateCachedCascades();
}


// Releases every simulated prop body and forgets the states.
//
// ClearDerived drops the colliders and render instances these bodies drive, so
// the bodies have to go with them: left alive they would keep stepping in the
// solver with nothing to move, and the next rebuild would spawn a second body
// for the same placement.
static void ReleasePrefabRigidBodies() {
    for (const PrefabRigidBodyState& state : g_prefabRigidBodies)
        g_destruction.DestroyPropBody(state.physicsHandle);
    g_prefabRigidBodies.clear();
}

static void RebuildPrefabRenderBatches() {
    ProfilerDX12::CpuScope prefabBatchProfile(g_profiler, "Editor/PrefabBatches");
    ReleasePrefabRigidBodies();
    g_game.world.Prefabs().ClearDerived();
    // Tracks the same instances ClearDerived just dropped; entity ids are reused
    // across levels, so a stale entry here would silently disable the bounds box
    // for an unrelated prefab.
    g_meshCollisionEntities.clear();
    // Same reasoning: a reused entity id would otherwise make a fresh building
    // skip its clone and cut the shared cached model every instance draws.
    g_blastHoleEntities.clear();
    g_assetRegistry.Refresh();
    g_prefabRegistry.Refresh(kPrefabRoot, kModelRoot);
    g_prefabAudioPlayers.clear();
    std::unordered_map<std::string, size_t> batches;
    // Entity ids of the aircraft this pass actually registered, so the ones it
    // did not can be pruned afterwards.
    std::unordered_set<uint64_t> seenObjectivePlanes;
    // `skipRenderBatch` suppresses only the drawing of an instance, leaving its
    // collision, light, audio, destructible and spawner components registered
    // like any other prefab's. The comm tower needs exactly that: it is drawn
    // from its destruction chunks, so emitting a batch too would render the mast
    // twice -- but skipping the whole instance (as this did before) also dropped
    // its "destructible" health, which left DamagePrefab unable to find it and
    // made the tower impervious to the C4 that is supposed to fell it.
    const auto addInstance = [&](const auto& self, const std::string& prefabId,
                                 const XMMATRIX& world, uint64_t entityId,
                                 const nlohmann::json& overrides,
                                 unsigned depth,
                                 bool skipRenderBatch = false) -> void {
        if (depth > 16) {
            SGE_LOG("LogPrefab", EngineLog::Level::Error,
                "Prefab nesting depth exceeded at: " + prefabId);
            return;
        }
        const PrefabAsset* prefab = g_prefabRegistry.Find(prefabId);
        if (!prefab) {
            SGE_LOG("LogPrefab", EngineLog::Level::Warning,
                "Level entity references missing prefab: " + prefabId);
            return;
        }
        PrefabModelCacheEntry* cachedModel = nullptr;
        {
            ProfilerDX12::CpuScope modelLoadProfile(
                g_profiler, "PrefabRebuild/ModelLoad");
            cachedModel = LoadPrefabModel(*prefab);
        }
        if (!cachedModel) return;
        const nlohmann::json components = MergePrefabComponents(
            prefab->components, overrides);
        const bool castShadow = components.contains("staticMesh")
            ? components.at("staticMesh").value("castShadow", prefab->castShadow)
            : prefab->castShadow;

        // A prefab carrying "aaTurret" becomes a live emplacement rather than
        // scenery: it tracks aircraft and shoots, exactly like the one placed
        // beside the comm tower. Both feed the same vector, so a level can have
        // the scripted gun plus any the designer drops in.
        //
        // Checked here, before the render batch below, because the turret render
        // path draws the model itself -- emitting the prefab batch as well would
        // draw the gun twice, once static and once tracking.
        if (components.contains("aaTurret")) {
            skipRenderBatch = true;
            // The shared `origin` below is computed after the render batch, so
            // take the placement straight off the world matrix here.
            XMFLOAT3 turretOrigin;
            XMStoreFloat3(&turretOrigin,
                          XMVector3TransformCoord(XMVectorZero(), world));
            if (g_game.vehicles.PlaceAATurret(turretOrigin) >=
                    VehicleSystem::kMaxAATurrets) {
                SGE_LOG("LogGameplay", EngineLog::Level::Warning,
                    "AA turret prefab ignored: at the turret cap");
            }
        }

        if (!skipRenderBatch) {
            const std::string batchKey =
                prefabId + (castShadow ? "#shadow" : "#noshadow");
            size_t index = 0;
            auto found = batches.find(batchKey);
            if (found == batches.end()) {
                index = g_prefabRenderBatches.size();
                batches[batchKey] = index;
                PrefabRenderBatch batch;
                batch.prefabId = prefabId;
                batch.model = cachedModel->model;
                batch.baseModel = cachedModel->model;
                batch.castShadow = castShadow;
                batch.automaticLod = prefab->automaticLod && prefab->lods.empty();
                if (batch.automaticLod) batch.lods = cachedModel->automaticLods;
                XMStoreFloat3(&batch.lodCenter, (XMLoadFloat3(&cachedModel->boundsMinimum) +
                    XMLoadFloat3(&cachedModel->boundsMaximum)) * 0.5f);
                batch.lodRadius = XMVectorGetX(XMVector3Length(
                    XMLoadFloat3(&cachedModel->boundsMaximum) - XMLoadFloat3(&cachedModel->boundsMinimum))) * 0.5f;
                for (size_t lodIndex = 0; lodIndex < prefab->lods.size(); ++lodIndex) {
                    PrefabAsset lodPrefab = *prefab;
                    lodPrefab.id = prefab->id + "#lod" + std::to_string(lodIndex);
                    lodPrefab.modelPath = prefab->lods[lodIndex].path;
                    lodPrefab.modelGuid = prefab->lods[lodIndex].assetGuid;
                    lodPrefab.lods.clear();
                    lodPrefab.automaticLod = false;
                    if (PrefabModelCacheEntry* lodModel = LoadPrefabModel(lodPrefab))
                        batch.lods.push_back({ prefab->lods[lodIndex].distance,
                                               lodModel->model });
                }
                g_prefabRenderBatches.push_back(std::move(batch));
            } else index = found->second;
            g_prefabRenderBatches[index].baseTransforms.push_back(world);
            g_prefabRenderBatches[index].transforms.push_back(world);
            g_prefabRenderBatches[index].entityIds.push_back(entityId);
        }

        // Register the timed aircraft objective.
        //
        // This belongs in THIS builder, not RebuildEditorPrefabVisualBatches:
        // that one only refreshes what the editor viewport draws, so a plane
        // registered there rendered but never appeared in g_objectivePlanes
        // during an actual run -- the takeoff simply never happened.
        //
        // Keyed on entity id so a rebuild (an asset edit, a prefab reload)
        // preserves a countdown already in flight rather than restarting it.
        if (prefabId == kObjectivePlanePrefabId) {
            seenObjectivePlanes.insert(entityId);
            const auto existing = std::find_if(
                g_objectivePlanes.begin(), g_objectivePlanes.end(),
                [&](const ObjectivePlaneState& plane) {
                    return plane.entityId == entityId;
                });
            ObjectivePlaneState& plane =
                existing == g_objectivePlanes.end()
                    ? g_objectivePlanes.emplace_back()
                    : *existing;
            plane.entityId = entityId;
            // How far the airframe reaches from its origin, used to rest the
            // wreck on the terrain instead of sinking the origin into it. Taken
            // from the vertical extent below the origin, scaled by the authored
            // placement, and floored so a model whose origin already sits at the
            // belly still clears the ground.
            {
                const XMFLOAT3& minimum = cachedModel->boundsMinimum;
                const float scaleY = XMVectorGetX(XMVector3Length(world.r[1]));
                plane.groundClearance =
                    (std::max)(0.5f, std::abs(minimum.y) * scaleY);

                // Blast reach, from the same bounds. The largest distance the
                // skin sits from the origin on each axis, scaled by the authored
                // placement -- so an explosive that lands anywhere on the
                // airframe counts as a hit on it (see ExplosionHitsObjectivePlane).
                //
                // Max of |min| and |max| per axis rather than half the span:
                // the origin is not necessarily centred in the bounds, and the
                // far side is the one that has to stay reachable.
                const XMFLOAT3& maximum = cachedModel->boundsMaximum;
                const float scaleX = XMVectorGetX(XMVector3Length(world.r[0]));
                const float scaleZ = XMVectorGetX(XMVector3Length(world.r[2]));
                const float reachX =
                    (std::max)(std::abs(minimum.x), std::abs(maximum.x)) * scaleX;
                const float reachZ =
                    (std::max)(std::abs(minimum.z), std::abs(maximum.z)) * scaleZ;
                // One horizontal reach for both axes: the placement may be
                // rotated to any heading, and tracking an oriented box here
                // would buy precision the blast test does not need.
                plane.blastReachHorizontal =
                    (std::max)(1.0f, (std::max)(reachX, reachZ));
                plane.blastReachVertical = (std::max)(1.0f,
                    (std::max)(std::abs(minimum.y), std::abs(maximum.y)) * scaleY);
            }
            XMStoreFloat4x4(&plane.baseTransform, world);
            XMStoreFloat3(&plane.basePosition,
                          XMVector3TransformCoord(XMVectorZero(), world));
            // Which way is "forward" for this model? Authored, not inferred.
            //
            // This was previously guessed from the mesh bounds, on the theory
            // that the fuselage is the longest horizontal axis. That is wrong
            // for any aircraft whose wings are broader than its body is long --
            // measured on this asset, X spans 29.0 m (wingspan) against Z's
            // 22.3 m (fuselage), so the guess picked the wing and the plane
            // taxied sideways. There is no geometric rule that survives every
            // model, so the prefab states it:
            //
            //   "objectivePlane": { "forwardAxis": [0, 0, 1] }
            //
            // Defaults to +Z when absent. Read from the merged components, so a
            // single instance can override it without touching the prefab.
            XMFLOAT3 authoredForward{ 0.0f, 0.0f, 1.0f };
            if (components.contains("objectivePlane")) {
                const nlohmann::json& settings = components.at("objectivePlane");
                const auto axis = settings.find("forwardAxis");
                if (axis != settings.end() && axis->is_array() &&
                    axis->size() == 3) {
                    authoredForward = XMFLOAT3(
                        axis->at(0).get<float>(), axis->at(1).get<float>(),
                        axis->at(2).get<float>());
                }
            }
            XMVECTOR localForward = XMLoadFloat3(&authoredForward);
            if (XMVectorGetX(XMVector3LengthSq(localForward)) < 1e-6f)
                localForward = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
            XMMATRIX orientation = world;
            orientation.r[3] = XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f);
            XMVECTOR forward = XMVector3TransformNormal(localForward, orientation);
            // Flatten to horizontal: the runway run is along the ground, and a
            // model authored with a slight nose-up would otherwise taxi upward.
            forward = XMVectorSetY(forward, 0.0f);
            if (XMVectorGetX(XMVector3LengthSq(forward)) < 1e-6f)
                forward = XMVectorSet(0.0f, 0.0f, 1.0f, 0.0f);
            XMStoreFloat3(&plane.forward, XMVector3Normalize(forward));
        }

        const std::string collision = components.contains("collision")
            ? components.at("collision").value("shape", prefab->collision)
            : prefab->collision;
        // An instance drawn by the destruction system owns its collision there
        // too, as chunk geometry. Adding the prefab's bounds box on top would
        // put a second, coarser collider around the same mast.
        if (collision != "none" && !skipRenderBatch) {
            if (collision == "mesh") {
                if (cachedModel->collisionMesh &&
                    !cachedModel->collisionMesh->Empty()) {
                    CollisionMeshInstance instance;
                    instance.entityId = entityId;
                    ProfilerDX12::CpuScope collisionInitProfile(
                        g_profiler, "PrefabRebuild/CollisionInit");
                    InitializeCollisionMeshInstance(
                        instance, *cachedModel->collisionMesh, world);
                    g_prefabMeshColliders.push_back(instance);
                    g_meshCollisionEntities.insert(entityId);
                } else if (g_prefabMeshFallbackWarnings.insert(prefabId).second) {
                    // The tree failed to build or the prefab was loaded before
                    // mesh collision was requested. The bounds box below still
                    // applies, so the instance is collidable, just coarsely.
                    SGE_LOG("LogPrefab", EngineLog::Level::Warning,
                        "Mesh collision unavailable; using bounds box: " + prefabId);
                }
            }
            const XMFLOAT3& minimum = cachedModel->boundsMinimum;
            const XMFLOAT3& maximum = cachedModel->boundsMaximum;
            const XMFLOAT3 localCenter((minimum.x + maximum.x) * 0.5f,
                (minimum.y + maximum.y) * 0.5f,
                (minimum.z + maximum.z) * 0.5f);
            XMFLOAT3 worldCenter;
            XMStoreFloat3(&worldCenter, XMVector3TransformCoord(
                XMLoadFloat3(&localCenter), world));
            XMFLOAT3 worldX, worldY, worldZ;
            XMStoreFloat3(&worldX, XMVector3TransformNormal(
                XMVectorSet(1, 0, 0, 0), world));
            XMStoreFloat3(&worldY, XMVector3TransformNormal(
                XMVectorSet(0, 1, 0, 0), world));
            XMStoreFloat3(&worldZ, XMVector3TransformNormal(
                XMVectorSet(0, 0, 1, 0), world));
            const float scaleX = XMVectorGetX(XMVector3Length(XMLoadFloat3(&worldX)));
            const float scaleY = XMVectorGetX(XMVector3Length(XMLoadFloat3(&worldY)));
            const float scaleZ = XMVectorGetX(XMVector3Length(XMLoadFloat3(&worldZ)));
            PrefabCollider collider;
            collider.entityId = entityId;
            collider.prefabId = prefabId;
            collider.center = worldCenter;
            collider.halfExtents = XMFLOAT3(
                (maximum.x - minimum.x) * 0.5f * scaleX,
                (maximum.y - minimum.y) * 0.5f * scaleY,
                (maximum.z - minimum.z) * 0.5f * scaleZ);
            collider.yawRadians = std::atan2(worldX.z, worldX.x);
            g_prefabColliders.push_back(std::move(collider));
            // A prefab asking for rigid-body simulation gets one body per
            // placement, sized from the same measured half extents the static
            // collider above just used. The collider stays in the list: it is
            // what the player sweep, the AI and bullet tests read, and the
            // per-frame sync below moves it to wherever the body has got to.
            //
            // Mesh collision is deliberately not supported here -- a moving
            // per-triangle tree would have to be re-fitted every frame, and the
            // props this exists for are boxes anyway.
            if (prefab->rigidBody.enabled && collision == "box") {
                const XMFLOAT3 half = g_prefabColliders.back().halfExtents;
                const uint32_t handle = g_destruction.CreatePropBody(
                    worldCenter, half, g_prefabColliders.back().yawRadians,
                    prefab->rigidBody.density);
                if (handle != 0) {
                    PrefabRigidBodyState state;
                    state.entityId = entityId;
                    state.prefabId = prefabId;
                    state.physicsHandle = handle;
                    state.halfExtents = half;
                    // The body is positioned at the bounds centre, but the model
                    // draws from its origin. Keep the scaled local offset so the
                    // draw transform can subtract it back out.
                    XMStoreFloat3(&state.centerOffset, XMVectorSet(
                        localCenter.x * scaleX, localCenter.y * scaleY,
                        localCenter.z * scaleZ, 0.0f));
                    XMStoreFloat4x4(&state.scaleTransform,
                        XMMatrixScaling(scaleX, scaleY, scaleZ));
                    g_prefabRigidBodies.push_back(std::move(state));
                    // One line per simulated placement, so a headless run can
                    // confirm the bodies exist and are the size the mesh
                    // measured rather than a default guess.
                    SGE_LOG("LogPrefab", EngineLog::Level::Display,
                        "Rigid body prop: " + prefabId + " half extents " +
                        std::to_string(half.x) + " x " +
                        std::to_string(half.y) + " x " +
                        std::to_string(half.z) + ", density " +
                        std::to_string(prefab->rigidBody.density));
                }
            }
        }

        XMFLOAT3 origin;
        XMStoreFloat3(&origin, XMVector3TransformCoord(XMVectorZero(), world));
        if (components.contains("light")) {
            const auto& light = components.at("light");
            const auto color = light.value("color", std::vector<float>{1, 1, 1});
            if (color.size() == 3) g_prefabLightInstances.push_back({ entityId,
                origin, {color[0], color[1], color[2]},
                light.value("intensity", 1.0f), light.value("radius", 5.0f) });
        }
        if (components.contains("audio")) {
            const auto& audio = components.at("audio");
            g_prefabAudioEmitters.push_back({ entityId, origin,
                audio.value("path", ""), audio.value("loop", false),
                audio.value("radius", 15.0f) });
        }
        if (components.contains("destructible")) {
            const float health = components.at("destructible").value(
                "health", 100.0f);
            g_prefabDestructibles.push_back({ entityId, origin, health });
            g_prefabHealth.try_emplace(entityId, health);
        }
        if (components.contains("spawner")) {
            const auto& spawner = components.at("spawner");
            XMFLOAT3 spawnWorldX;
            XMStoreFloat3(&spawnWorldX, XMVector3TransformNormal(
                XMVectorSet(1, 0, 0, 0), world));
            g_prefabSpawnPoints.push_back({ entityId, origin,
                std::atan2(spawnWorldX.z, spawnWorldX.x),
                spawner.value("enemyType", "bandit"),
                spawner.value("count", 1u) });
        }
        // The shop counter. Its reach is authored in metres on the prefab, so a
        // designer who scales a placement up in the editor gets a proportionally
        // larger counter to walk up to rather than the same fixed bubble.
        if (components.contains("armory")) {
            const auto& armory = components.at("armory");
            XMFLOAT3 armoryWorldX;
            XMStoreFloat3(&armoryWorldX, XMVector3TransformNormal(
                XMVectorSet(1, 0, 0, 0), world));
            const float armoryScale = XMVectorGetX(
                XMVector3Length(XMLoadFloat3(&armoryWorldX)));
            PrefabArmoryShop shop;
            shop.entityId = entityId;
            shop.position = origin;
            shop.yawRadians = std::atan2(armoryWorldX.z, armoryWorldX.x);
            shop.radius = armory.value("radius", 3.5f) *
                (armoryScale > 1e-4f ? armoryScale : 1.0f);
            shop.displayName = armory.value("displayName", "ARMORY");
            // One line per counter, so a headless run can confirm the shops
            // exist and stand where the level put them rather than leaving the
            // question to a screenshot.
            SGE_LOG("LogPrefab", EngineLog::Level::Display,
                "Armory shop: " + prefabId + " \"" + shop.displayName +
                "\" at " + std::to_string(shop.position.x) + ", " +
                std::to_string(shop.position.y) + ", " +
                std::to_string(shop.position.z) + ", reach " +
                std::to_string(shop.radius) + "m");
            g_prefabArmoryShops.push_back(std::move(shop));
        }
        // The boarding point. Same scale handling as the counter above: reach
        // is authored in metres, so scaling the aircraft up in the editor grows
        // the area the player can board it from rather than leaving a fixed
        // bubble buried inside a larger hull.
        if (components.contains("travel")) {
            const auto& travel = components.at("travel");
            XMFLOAT3 travelWorldX;
            XMStoreFloat3(&travelWorldX, XMVector3TransformNormal(
                XMVectorSet(1, 0, 0, 0), world));
            const float travelScale = XMVectorGetX(
                XMVector3Length(XMLoadFloat3(&travelWorldX)));
            PrefabTravelPoint point;
            point.entityId = entityId;
            point.position = origin;
            point.yawRadians = std::atan2(travelWorldX.z, travelWorldX.x);
            point.radius = travel.value("radius", 6.0f) *
                (travelScale > 1e-4f ? travelScale : 1.0f);
            point.displayName =
                travel.value("displayName", "BOARD HELICOPTER");
            SGE_LOG("LogPrefab", EngineLog::Level::Display,
                "Travel point: " + prefabId + " \"" + point.displayName +
                "\" at " + std::to_string(point.position.x) + ", " +
                std::to_string(point.position.y) + ", " +
                std::to_string(point.position.z) + ", reach " +
                std::to_string(point.radius) + "m");
            g_prefabTravelPoints.push_back(std::move(point));
        }
        for (const PrefabChildAsset& child : prefab->children) {
            const XMMATRIX childLocal = XMMatrixScaling(child.scale[0], child.scale[1],
                child.scale[2]) * EulerDegreesToMatrix(child.rotation) *
                XMMatrixTranslation(
                child.position[0], child.position[1], child.position[2]);
            self(self, child.prefabId, childLocal * world, entityId,
                 nlohmann::json::object(), depth + 1);
        }
    };
    std::optional<ProfilerDX12::CpuScope> entityLoopProfile;
    entityLoopProfile.emplace(g_profiler, "PrefabRebuild/Entities");
    for (const LevelEntity& entity : g_game.world.Level().entities) {
        if (!entity.enabled) continue;
        std::string prefabId;
        if (entity.type == LevelEntityType::Prefab) prefabId = entity.prefabId;
        else if (entity.type == LevelEntityType::Rock) prefabId = "rock";
        else continue;
        // Prefabs baked into the destruction model draw and collide from their
        // NvBlast chunks. Suppress the intact copy while retaining other
        // components; the comm tower still needs its objective health.
        const bool drawnByDestruction =
            IsNvBlastStructurePrefab(prefabId) && g_destruction.IsInitialized();
        const Transform& t = entity.transform;
        const XMMATRIX world = EntityWorldMatrix(t);
        addInstance(addInstance, prefabId, world, entity.id, entity.overrides, 0,
                    drawnByDestruction);
    }
    // Drop aircraft whose entity no longer exists in this level.
    //
    // The registration above is keyed on entity id so an in-flight countdown
    // survives a rebuild, but nothing removed entries -- and entity ids are
    // reused across levels. A wreck left in this vector from a previous level
    // therefore kept writing its crash transform into whatever instance now
    // held its id, which is how a broken plane ended up welded to the screen.
    // Anything not re-registered by the pass above is gone.
    g_objectivePlanes.erase(
        std::remove_if(g_objectivePlanes.begin(), g_objectivePlanes.end(),
                       [&](const ObjectivePlaneState& plane) {
                           return !seenObjectivePlanes.count(plane.entityId);
                       }),
        g_objectivePlanes.end());
    // AA emplacements. Drawn from a code-built model rather than a prefab batch
    // per entity, because they are placed by gameplay (beside the comm tower,
    // and from the editor's turret prefab) and are not level entities.
    //
    // Each emplacement gets its own batch and its own posed model clone, so a
    // gun tracking the helicopter and one tracking the player visibly point in
    // different directions. A batch carries a single model pointer, so sharing
    // one clone across several transforms would necessarily share the aim too.
    if (!g_game.vehicles.aaTurrets.empty()) {
        const bool createTurret = !g_aaTurretModel;
        // Authored model first; the box-built one is the fallback if the asset
        // is missing or does not carry the traverse node the pose code needs.
        if (createTurret) {
            g_aaTurretModel = LoadAATurretModel();
            if (!g_aaTurretModel) g_aaTurretModel = CreateAATurretModel();
        }
        if (createTurret && g_aaTurretModel)
            FlushStaticBufferUploadsDX12(g_dx12.commandList.Get());
        if (g_aaTurretModel) {
            // Clones are made once and then reused: they are rebuilt only when
            // the turret count changes, not every frame.
            while (g_aaTurretModelInstances.size() <
                   g_game.vehicles.aaTurrets.size()) {
                g_aaTurretModelInstances.push_back(
                    CloneSceneNodeShallow(g_aaTurretModel));
            }
            for (size_t i = 0; i < g_game.vehicles.aaTurrets.size(); ++i) {
                const VehicleSystem::AATurret& turret =
                    g_game.vehicles.aaTurrets[i];
                const std::shared_ptr<SceneNode>& instance =
                    g_aaTurretModelInstances[i];
                if (!instance) continue;
                // Pose this clone to this turret's own solution.
                PoseAATurretModelInstance(instance, turret.pitch, turret.yaw);
                PrefabRenderBatch batch;
                batch.prefabId = "__aa_turret_" + std::to_string(i);
                batch.model = instance;
                batch.baseModel = instance;
                const XMMATRIX world = XMMatrixTranslation(
                    turret.position.x, turret.position.y, turret.position.z);
                batch.baseTransforms.push_back(world);
                batch.transforms.push_back(world);
                // Distinct id per emplacement so hit attribution can tell them
                // apart; the base constant keeps them out of level entity ids.
                batch.entityIds.push_back(kAATurretEntityId + i);
                g_prefabRenderBatches.push_back(std::move(batch));
            }
        }
    }
    if (g_ddgiCornellTestMode) {
        const bool createModel = !g_ddgiCornellModel;
        if (createModel)
            g_ddgiCornellModel = CreateDDGICornellBoxModel();
        if (createModel && g_ddgiCornellModel)
            FlushStaticBufferUploadsDX12(g_dx12.commandList.Get());
        if (g_ddgiCornellModel) {
            PrefabRenderBatch batch;
            batch.prefabId = "__ddgi_cornell_box";
            batch.model = g_ddgiCornellModel;
            batch.baseModel = g_ddgiCornellModel;
            batch.baseTransforms.push_back(XMMatrixIdentity());
            batch.transforms.push_back(XMMatrixIdentity());
            batch.entityIds.push_back(0x434f524e454c4cull);
            g_prefabRenderBatches.push_back(std::move(batch));
        }
    }
    entityLoopProfile.reset();
    {
        ProfilerDX12::CpuScope lightsProfile(
            g_profiler, "PrefabRebuild/Lights");
        RebuildRuntimePrefabLights();
    }
    {
        ProfilerDX12::CpuScope audioProfile(
            g_profiler, "PrefabRebuild/Audio");
        for (size_t emitterIndex = 0;
             emitterIndex < g_prefabAudioEmitters.size(); ++emitterIndex) {
            const PrefabAudioEmitter& emitter =
                g_prefabAudioEmitters[emitterIndex];
            auto player = std::make_unique<GunAudio>();
            if (player->Initialize(emitter.path)) {
                if (emitter.loop) player->SetLoop(true, 0.0f);
                else player->Play(0.7f);
                g_prefabAudioPlayers.push_back(
                    { emitterIndex, std::move(player) });
            }
        }
    }
    g_prefabRebuildRequested = false;
    g_editorVisualRefreshRequested = false;
    shadowMap.InvalidateCachedCascades();
    // Editing is preview-only. A full Save/Play reconciliation latches the
    // environment request before this compile, then consumes it only after the
    // new prefab colliders exist.
    if (g_customLevelMode) {
        if (!IsEditorEditing() || g_editorFullReconcileInFlight)
            g_pendingEnvironmentRebuild = true;
        g_editorFullReconcileInFlight = false;
    }
    if (IsEditorEditing())
        RebuildEditorPrefabVisualBatches(false);
}

static void UpdatePrefabAudio() {
    for (PrefabAudioPlayer& runtime : g_prefabAudioPlayers) {
        if (!runtime.player || runtime.emitterIndex >= g_prefabAudioEmitters.size())
            continue;
        const PrefabAudioEmitter& emitter = g_prefabAudioEmitters[runtime.emitterIndex];
        const float dx = emitter.position.x - scene.camera.Position.x;
        const float dy = emitter.position.y - scene.camera.Position.y;
        const float dz = emitter.position.z - scene.camera.Position.z;
        const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        const float volume = (std::max)(0.0f, 1.0f - distance / emitter.radius);
        if (emitter.loop) runtime.player->SetLoop(true, volume * 0.8f);
        runtime.player->Update();
    }
}

static void UpdatePrefabLods() {
    bool shadowGeometryChanged = false;
    for (PrefabRenderBatch& batch : g_prefabRenderBatches) {
        if (batch.lods.empty() || !batch.baseModel) continue;
        const std::shared_ptr<SceneNode> previousModel = batch.model;
        float nearest = FLT_MAX;
        for (const XMMATRIX& transform : batch.baseTransforms) {
            if (batch.automaticLod) {
                const XMVECTOR center = XMVector3TransformCoord(XMLoadFloat3(&batch.lodCenter), transform);
                const float scale = (std::max)({ XMVectorGetX(XMVector3Length(transform.r[0])),
                    XMVectorGetX(XMVector3Length(transform.r[1])), XMVectorGetX(XMVector3Length(transform.r[2])), 0.001f });
                const float distance = XMVectorGetX(XMVector3Length(center - XMLoadFloat3(&scene.camera.Position)));
                nearest = (std::min)(nearest, (std::max)(0.0f, distance / scale - batch.lodRadius));
                continue;
            }
            XMFLOAT4X4 matrix;
            XMStoreFloat4x4(&matrix, transform);
            const float dx = matrix._41 - scene.camera.Position.x;
            const float dy = matrix._42 - scene.camera.Position.y;
            const float dz = matrix._43 - scene.camera.Position.z;
            nearest = (std::min)(nearest,
                std::sqrt(dx * dx + dy * dy + dz * dz));
        }
        batch.model = batch.baseModel;
        if (!batch.automaticLod || scene.automaticPrefabLod) {
            for (const PrefabRenderBatch::LodModel& lod : batch.lods) {
                const bool wasCoarser = batch.automaticLod &&
                    std::any_of(batch.lods.begin(), batch.lods.end(), [&](const auto& previous) {
                        return previous.model == previousModel && previous.distance >= lod.distance;
                    });
                const float threshold = lod.distance * (wasCoarser ? 0.9f : 1.0f);
                if (nearest >= threshold) batch.model = lod.model;
            }
        }
        shadowGeometryChanged |= batch.castShadow &&
            batch.model != previousModel;
    }
    if (shadowGeometryChanged) shadowMap.InvalidateCachedCascades();
}
