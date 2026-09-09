#pragma once

// Private application implementation; included once by main.cpp in dependency order.

static bool ComputePrefabModelBounds(const std::shared_ptr<SceneNode>& model,
                                     XMFLOAT3& minimum, XMFLOAT3& maximum) {
    minimum = XMFLOAT3(FLT_MAX, FLT_MAX, FLT_MAX);
    maximum = XMFLOAT3(-FLT_MAX, -FLT_MAX, -FLT_MAX);
    const auto visit = [&](const auto& self,
                           const std::shared_ptr<SceneNode>& node) -> void {
        if (!node) return;
        const XMMATRIX nodeWorld = XMLoadFloat4x4(&node->globalTransform);
        if (node->mesh) for (const MeshPrimitive& primitive : node->mesh->primitives) {
            for (size_t i = 0; i + 11 < primitive.vertices.size(); i += 12) {
                XMFLOAT3 point;
                XMStoreFloat3(&point, XMVector3TransformCoord(XMVectorSet(
                    primitive.vertices[i], primitive.vertices[i + 1],
                    primitive.vertices[i + 2], 1.0f), nodeWorld));
                minimum.x = (std::min)(minimum.x, point.x);
                minimum.y = (std::min)(minimum.y, point.y);
                minimum.z = (std::min)(minimum.z, point.z);
                maximum.x = (std::max)(maximum.x, point.x);
                maximum.y = (std::max)(maximum.y, point.y);
                maximum.z = (std::max)(maximum.z, point.z);
            }
        }
        for (const auto& child : node->children) self(self, child);
    };
    visit(visit, model);
    return minimum.x != FLT_MAX;
}

// Flattens the model's triangles into the 9-floats-per-triangle soup
// BuildCollisionMesh consumes, in the same space ComputePrefabModelBounds
// measures.
//
// Must be called at exactly the same point as that function: LoadPrefabModel's
// targetSize normalization mutates the root TRS and every globalTransform before
// bounds are taken, so extracting earlier would silently bake a differently
// scaled tree (R1). The stage-2 bounds assertion exists to catch a reordering.
//
// Returns false when no primitive carried CPU-side vertices -- a GPU-only
// optimization upstream would otherwise yield a silently empty tree (R2).
static bool ExtractPrefabCollisionTriangles(const std::shared_ptr<SceneNode>& model,
                                            std::vector<float>& triangles,
                                            std::string* emptyPrimitives = nullptr) {
    triangles.clear();
    size_t primitiveCount = 0;
    size_t emptyCount = 0;
    std::string emptyNames;

    const auto visit = [&](const auto& self,
                           const std::shared_ptr<SceneNode>& node) -> void {
        if (!node) return;
        const XMMATRIX nodeWorld = XMLoadFloat4x4(&node->globalTransform);
        if (node->mesh) for (const MeshPrimitive& primitive : node->mesh->primitives) {
            ++primitiveCount;
            const size_t vertexCount = primitive.vertices.size() / 12;
            if (vertexCount == 0) {
                ++emptyCount;
                if (emptyNames.size() < 200) {
                    if (!emptyNames.empty()) emptyNames += ", ";
                    emptyNames += node->name;
                }
                continue;
            }

            // Position is the first 3 of the 12 interleaved floats per vertex,
            // matching ComputePrefabModelBounds.
            const auto transformed = [&](size_t vertex, XMFLOAT3& out) {
                const size_t base = vertex * 12;
                XMStoreFloat3(&out, XMVector3TransformCoord(XMVectorSet(
                    primitive.vertices[base], primitive.vertices[base + 1],
                    primitive.vertices[base + 2], 1.0f), nodeWorld));
            };
            const auto append = [&](size_t a, size_t b, size_t c) {
                if (a >= vertexCount || b >= vertexCount || c >= vertexCount) return;
                XMFLOAT3 v0, v1, v2;
                transformed(a, v0); transformed(b, v1); transformed(c, v2);
                const float values[9] = { v0.x, v0.y, v0.z, v1.x, v1.y, v1.z,
                                          v2.x, v2.y, v2.z };
                triangles.insert(triangles.end(), values, values + 9);
            };

            if (!primitive.indices.empty()) {
                for (size_t i = 0; i + 2 < primitive.indices.size(); i += 3)
                    append(primitive.indices[i], primitive.indices[i + 1],
                           primitive.indices[i + 2]);
            } else {
                // Non-indexed primitives are drawn as sequential triples.
                for (size_t i = 0; i + 2 < vertexCount; i += 3)
                    append(i, i + 1, i + 2);
            }
        }
        for (const auto& child : node->children) self(self, child);
    };
    visit(visit, model);

    if (emptyPrimitives && emptyCount > 0) {
        *emptyPrimitives = std::to_string(emptyCount) + " of " +
            std::to_string(primitiveCount) + " primitives had no CPU vertices (" +
            emptyNames + ")";
    }
    return !triangles.empty();
}

static void ApplyPrefabMaterialOverrides(const PrefabAsset& prefab,
                                         const std::shared_ptr<SceneNode>& model) {
    for (const PrefabMaterialOverride& overrideValue : prefab.materialOverrides) {
        const auto visit = [&](const auto& self,
                               const std::shared_ptr<SceneNode>& node) -> void {
            if (!node) return;
            if (node->mesh) {
                for (MeshPrimitive& primitive : node->mesh->primitives) {
                    const bool matchesMesh = node->name == overrideValue.mesh ||
                        node->mesh->name == overrideValue.mesh;
                    const bool matchesMaterial = primitive.material &&
                        primitive.material->name == overrideValue.mesh;
                    if (!matchesMesh && !matchesMaterial) continue;
                    auto material = primitive.material
                        ? std::make_shared<SceneMaterial>(*primitive.material)
                        : std::make_shared<SceneMaterial>();
                    material->baseColorTexture = GLBImporter::LoadTextureFromFile(
                        overrideValue.texture.string(), g_dx12.device,
                        g_dx12.commandList, material->uploadHeaps);
                    // The copy inherits the source material's cached bindings,
                    // which point at the *old* albedo. Drop all of them.
                    material->InvalidateTextureBindings();
                    primitive.material = std::move(material);
                }
            }
            for (const auto& child : node->children) self(self, child);
        };
        visit(visit, model);
    }
}

static PrefabModelCacheEntry* LoadPrefabModel(const PrefabAsset& prefab) {
    auto cached = g_prefabModelCache.find(prefab.id);
    if (cached != g_prefabModelCache.end()) return &cached->second;
    const std::string extension = prefab.modelPath.extension().string();
    std::string cookedError;
    std::shared_ptr<SceneNode> model = CookedAssetLoader::LoadForSource(
        prefab.modelPath, g_dx12.device, g_dx12.commandList, &cookedError);
    if (model) {
        SGE_LOG("LogPrefab", EngineLog::Level::Display,
            "Loaded cooked asset for " + prefab.id);
    } else {
        if (!cookedError.empty()) {
            SGE_LOG("LogPrefab", EngineLog::Level::Warning,
                "Cooked asset unavailable for " + prefab.id + ": " +
                cookedError + "; using source importer");
        }
        if (_stricmp(extension.c_str(), ".fbx") == 0) {
            model = FBXImporter::Load(prefab.modelPath.string(), g_dx12.device,
                g_dx12.commandList, 1.0f, false, prefab.useMaterials, false,
                true);
        } else {
            // Editor refreshes can import after the staged GPU mip flush, so
            // upload their complete mip chains on this direct command list.
            model = GLBImporter::LoadGLB(prefab.modelPath.string(),
                g_dx12.device, g_dx12.commandList, IsEditorEditing());
        }
    }
    if (!model) {
        SGE_LOG("LogPrefab", EngineLog::Level::Error,
            "Failed to load " + prefab.id + " from " + prefab.modelPath.string());
        return nullptr;
    }
    if (!prefab.useMaterials) {
        const auto stripMaterials = [](const auto& self,
                                       const std::shared_ptr<SceneNode>& node) -> void {
            if (!node) return;
            if (node->mesh) {
                for (MeshPrimitive& primitive : node->mesh->primitives)
                    primitive.material = std::make_shared<SceneMaterial>();
            }
            for (const auto& child : node->children) self(self, child);
        };
        stripMaterials(stripMaterials, model);
    }
    if (prefab.forceDoubleSided) {
        const auto forceDoubleSided = [](const auto& self,
                                         const std::shared_ptr<SceneNode>& node) -> void {
            if (!node) return;
            if (node->mesh) {
                for (MeshPrimitive& primitive : node->mesh->primitives)
                    if (primitive.material) primitive.material->doubleSided = true;
            }
            for (const auto& child : node->children) self(self, child);
        };
        forceDoubleSided(forceDoubleSided, model);
    }
    if (prefab.transparencyPass != "auto") {
        const WaterTransparencyMode waterMode =
            prefab.transparencyPass == "afterWater"
                ? WaterTransparencyMode::AfterWater
                : WaterTransparencyMode::BeforeWater;
        const auto setWaterTransparency = [waterMode](
            const auto& self, const std::shared_ptr<SceneNode>& node) -> void {
            if (!node) return;
            if (node->mesh) {
                for (MeshPrimitive& primitive : node->mesh->primitives)
                    if (primitive.material)
                        primitive.material->waterTransparency = waterMode;
            }
            for (const auto& child : node->children) self(self, child);
        };
        setWaterTransparency(setWaterTransparency, model);
    }
    if (prefab.materialAmbientScale != 1.0f ||
        prefab.materialViewFillStrength != 0.0f) {
        const auto tuneMaterials = [&](const auto& self,
                                       const std::shared_ptr<SceneNode>& node) -> void {
            if (!node) return;
            if (node->mesh) {
                for (MeshPrimitive& primitive : node->mesh->primitives) {
                    if (!primitive.material) continue;
                    primitive.material->ambientScale = prefab.materialAmbientScale;
                    primitive.material->viewFillStrength =
                        prefab.materialViewFillStrength;
                }
            }
            for (const auto& child : node->children) self(self, child);
        };
        tuneMaterials(tuneMaterials, model);
    }
    model->translation = XMFLOAT3(0.0f, 0.0f, 0.0f);
    model->rotation = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
    model->scale = XMFLOAT3(1.0f, 1.0f, 1.0f);
    XMFLOAT4X4 identity;
    XMStoreFloat4x4(&identity, XMMatrixIdentity());
    model->UpdateGlobalTransform(identity);
    if (prefab.targetSize > 0.0f) {
        XMFLOAT3 minimum;
        XMFLOAT3 maximum;
        if (ComputePrefabModelBounds(model, minimum, maximum)) {
            const float span = (std::max)({ maximum.x - minimum.x,
                maximum.y - minimum.y, maximum.z - minimum.z, 0.001f });
            const float normalizedScale = prefab.targetSize / span;
            const XMFLOAT3 anchor((minimum.x + maximum.x) * 0.5f, minimum.y,
                                  (minimum.z + maximum.z) * 0.5f);
            model->scale = XMFLOAT3(normalizedScale, normalizedScale,
                                    normalizedScale);
            model->translation = XMFLOAT3(-anchor.x * normalizedScale,
                                          -anchor.y * normalizedScale,
                                          -anchor.z * normalizedScale);
            model->UpdateGlobalTransform(identity);
        }
    }
    ApplyPrefabMaterialOverrides(prefab, model);
    PrefabModelCacheEntry entry;
    entry.model = model;
    if (!ComputePrefabModelBounds(model, entry.boundsMinimum, entry.boundsMaximum)) {
        entry.boundsMinimum = XMFLOAT3(-0.5f, 0.0f, -0.5f);
        entry.boundsMaximum = XMFLOAT3(0.5f, 1.0f, 0.5f);
        SGE_LOG("LogPrefab", EngineLog::Level::Warning,
            "Model has no renderable bounds; using unit box: " + prefab.id);
    }

    // Per-triangle collision, built here so it shares the post-normalization
    // space the bounds above were measured in. Only prefabs that ask for it pay
    // the build; everything else keeps the bounds box and is untouched.
    if (prefab.collision == "mesh") {
        // The cache is keyed on the source model plus the transform the tree was
        // baked in, so a targetSize edit or a re-export invalidates it without
        // any manual cleanup.
        const CollisionMeshBuildParams buildParams;
        CollisionCache::Key cacheKey;
        cacheKey.sourcePath = prefab.modelPath;
        cacheKey.prefabId = prefab.id;
        cacheKey.transformHash = CollisionCache::HashTransform(
            prefab.targetSize, model->scale, model->translation);
        cacheKey.buildParamsHash = CollisionCache::HashBuildParams(buildParams);

        auto collisionMesh = std::make_shared<CollisionMesh>();
        std::string cacheError;
        const auto loadStarted = std::chrono::steady_clock::now();
        if (CollisionCache::Load(cacheKey, *collisionMesh, &cacheError)) {
            const double loadMs = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - loadStarted).count();
            SGE_LOG("LogPrefab", EngineLog::Level::Display,
                "Collision mesh " + prefab.id + ": loaded " +
                std::to_string(collisionMesh->TriangleCount()) + " tris, " +
                std::to_string(collisionMesh->nodes.size()) + " nodes from cache in " +
                std::to_string(static_cast<int>(loadMs)) + " ms, " +
                std::to_string(collisionMesh->MemoryBytes() / (1024 * 1024)) + " MB");
            entry.collisionMesh = std::move(collisionMesh);
        } else {
        SGE_LOG("LogPrefab", EngineLog::Level::Display,
            "Collision mesh " + prefab.id + ": building (" + cacheError + ")");
        std::vector<float> triangles;
        std::string emptyPrimitives;
        const auto extractStarted = std::chrono::steady_clock::now();
        const bool extracted =
            ExtractPrefabCollisionTriangles(model, triangles, &emptyPrimitives);
        const double extractMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - extractStarted).count();

        if (!extracted) {
            // Never build an empty tree silently: that would look like "the
            // airport has no collision" rather than "the vertices went away".
            SGE_LOG("LogPrefab", EngineLog::Level::Error,
                "Mesh collision requested but no CPU vertices available: " +
                prefab.id + (emptyPrimitives.empty() ? "" : " -- " + emptyPrimitives));
        } else {
            if (!emptyPrimitives.empty()) {
                SGE_LOG("LogPrefab", EngineLog::Level::Warning,
                    "Mesh collision skipped some primitives: " + prefab.id +
                    " -- " + emptyPrimitives);
            }

            // The soup's own AABB must match the bounds computed from the same
            // traversal. A mismatch means the two ran in different spaces, which
            // is the wrong-space failure (R1) and would put collision geometry
            // somewhere other than the visible model.
            XMFLOAT3 soupMinimum(FLT_MAX, FLT_MAX, FLT_MAX);
            XMFLOAT3 soupMaximum(-FLT_MAX, -FLT_MAX, -FLT_MAX);
            for (size_t i = 0; i + 2 < triangles.size(); i += 3) {
                soupMinimum.x = (std::min)(soupMinimum.x, triangles[i]);
                soupMinimum.y = (std::min)(soupMinimum.y, triangles[i + 1]);
                soupMinimum.z = (std::min)(soupMinimum.z, triangles[i + 2]);
                soupMaximum.x = (std::max)(soupMaximum.x, triangles[i]);
                soupMaximum.y = (std::max)(soupMaximum.y, triangles[i + 1]);
                soupMaximum.z = (std::max)(soupMaximum.z, triangles[i + 2]);
            }
            const float boundsDrift = (std::max)({
                std::abs(soupMinimum.x - entry.boundsMinimum.x),
                std::abs(soupMinimum.y - entry.boundsMinimum.y),
                std::abs(soupMinimum.z - entry.boundsMinimum.z),
                std::abs(soupMaximum.x - entry.boundsMaximum.x),
                std::abs(soupMaximum.y - entry.boundsMaximum.y),
                std::abs(soupMaximum.z - entry.boundsMaximum.z) });
            if (boundsDrift > 1e-3f) {
                SGE_LOG("LogPrefab", EngineLog::Level::Error,
                    "Collision soup bounds differ from model bounds by " +
                    std::to_string(boundsDrift) + " for " + prefab.id +
                    " -- extraction ran in the wrong space");
            }

            CollisionMeshBuildStats stats;
            const size_t sourceTriangles = triangles.size() / 9;
            if (BuildCollisionMesh(std::move(triangles), *collisionMesh,
                                   buildParams, &stats)) {
                std::string saveError;
                if (!CollisionCache::Save(cacheKey, *collisionMesh, &saveError)) {
                    // A cache that cannot be written is a slow next start, not a
                    // broken one: the tree in hand is still correct.
                    SGE_LOG("LogPrefab", EngineLog::Level::Warning,
                        "Collision mesh not cached for " + prefab.id + ": " +
                        saveError);
                }
                entry.collisionMesh = std::move(collisionMesh);
                SGE_LOG("LogPrefab", EngineLog::Level::Display,
                    "Collision mesh " + prefab.id + ": " +
                    std::to_string(stats.triangleCount) + " tris (" +
                    std::to_string(stats.degenerateSkipped) + " degenerate of " +
                    std::to_string(sourceTriangles) + "), " +
                    std::to_string(stats.nodeCount) + " nodes, " +
                    std::to_string(stats.leafCount) + " leaves, depth " +
                    std::to_string(stats.maxDepth) + ", max leaf " +
                    std::to_string(stats.maxLeafTriangles) + ", extract " +
                    std::to_string(static_cast<int>(extractMs)) + " ms, build " +
                    std::to_string(static_cast<int>(stats.buildMilliseconds)) +
                    " ms, " + std::to_string(entry.collisionMesh->MemoryBytes() /
                                             (1024 * 1024)) + " MB");
            } else {
                SGE_LOG("LogPrefab", EngineLog::Level::Error,
                    "Collision mesh build produced no usable triangles: " + prefab.id);
            }
        }
        }
    }

    if (prefab.automaticLod && prefab.lods.empty()) {
        for (int tier = 0; tier < 2; ++tier) {
            size_t original = 0, reduced = 0;
            auto lod = BuildAutomaticLod(model, g_dx12.device.Get(),
                tier == 0 ? 0.5f : 0.2f, tier == 0 ? 0.002f : 0.005f,
                original, reduced);
            if (lod && reduced < original) {
                entry.automaticLods.push_back({ tier == 0 ? 40.0f : 100.0f, lod });
                SGE_LOG("LogPrefab", EngineLog::Level::Display,
                    "Automatic LOD " + prefab.id + " tier " + std::to_string(tier + 1) +
                    ": " + std::to_string(original) + " -> " + std::to_string(reduced) + " triangles");
            }
        }
    }
    auto inserted = g_prefabModelCache.emplace(prefab.id, std::move(entry));
    SGE_LOG("LogPrefab", EngineLog::Level::Display,
        "Loaded " + prefab.id + " from " + prefab.modelPath.string());
    return &inserted.first->second;
}
