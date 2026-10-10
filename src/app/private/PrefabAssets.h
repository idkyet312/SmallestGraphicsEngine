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
//
// materialPrefixes, when given, keeps only primitives whose material name
// starts with one of them ("floor" collision).
static bool ExtractPrefabCollisionTriangles(const std::shared_ptr<SceneNode>& model,
                                            std::vector<float>& triangles,
                                            std::string* emptyPrimitives = nullptr,
                                            const SceneNode* skippedSubtree = nullptr,
                                            const std::vector<std::string>* materialPrefixes = nullptr) {
    triangles.clear();
    size_t primitiveCount = 0;
    size_t emptyCount = 0;
    std::string emptyNames;

    const auto visit = [&](const auto& self,
                           const std::shared_ptr<SceneNode>& node) -> void {
        if (!node || node.get() == skippedSubtree) return;
        const XMMATRIX nodeWorld = XMLoadFloat4x4(&node->globalTransform);
        if (node->mesh) for (const MeshPrimitive& primitive : node->mesh->primitives) {
            if (materialPrefixes) {
                const std::string& material =
                    primitive.material ? primitive.material->name : std::string();
                if (std::none_of(materialPrefixes->begin(), materialPrefixes->end(),
                        [&material](const std::string& prefix) {
                            return material.compare(0, prefix.size(), prefix) == 0;
                        }))
                    continue;
            }
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
                    if (!overrideValue.texture.empty())
                        material->baseColorTexture = GLBImporter::LoadTextureFromFile(
                            overrideValue.texture.string(), g_dx12.device,
                            g_dx12.commandList, material->uploadHeaps);
                    if (overrideValue.hasBaseColor) {
                        material->baseColorFactor.x *= overrideValue.baseColor[0];
                        material->baseColorFactor.y *= overrideValue.baseColor[1];
                        material->baseColorFactor.z *= overrideValue.baseColor[2];
                    }
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

// Flags cutout materials whose vertex normals disagree with their triangle
// faces on more than a fifth of triangles. Bistro's cypress cards measure 50%
// (outward volume normals); its hedges, linde and ivy leaves 0-0.1%. Those
// materials keep authored normals in Forward (alphaCut mode 4).
static void MarkBentNormalCutouts(const std::shared_ptr<SceneNode>& root,
                                  const std::string& prefabId) {
    struct Tally { size_t opposed = 0, total = 0; };
    std::unordered_map<SceneMaterial*, Tally> tallies;
    const auto walk = [&](const auto& self, const std::shared_ptr<SceneNode>& node) -> void {
        if (!node) return;
        if (node->mesh) {
            for (const MeshPrimitive& primitive : node->mesh->primitives) {
                SceneMaterial* material = primitive.material.get();
                if (!material || !material->alphaCutout || material->foliageShading)
                    continue;
                const std::vector<float>& v = primitive.vertices;
                const std::vector<unsigned int>& idx = primitive.indices;
                const size_t vertexCount = v.size() / 12;
                Tally& tally = tallies[material];
                for (size_t t = 0; t + 2 < idx.size(); t += 3) {
                    const size_t a = idx[t], b = idx[t + 1], c = idx[t + 2];
                    if (a >= vertexCount || b >= vertexCount || c >= vertexCount) continue;
                    const XMVECTOR pa = XMVectorSet(v[a * 12], v[a * 12 + 1], v[a * 12 + 2], 0);
                    const XMVECTOR face = XMVector3Cross(
                        XMVectorSubtract(XMVectorSet(v[b * 12], v[b * 12 + 1], v[b * 12 + 2], 0), pa),
                        XMVectorSubtract(XMVectorSet(v[c * 12], v[c * 12 + 1], v[c * 12 + 2], 0), pa));
                    if (XMVectorGetX(XMVector3LengthSq(face)) < 1e-20f) continue;
                    const XMVECTOR normal = XMVectorSet(
                        v[a * 12 + 3] + v[b * 12 + 3] + v[c * 12 + 3],
                        v[a * 12 + 4] + v[b * 12 + 4] + v[c * 12 + 4],
                        v[a * 12 + 5] + v[b * 12 + 5] + v[c * 12 + 5], 0);
                    ++tally.total;
                    if (XMVectorGetX(XMVector3Dot(face, normal)) < 0.0f) ++tally.opposed;
                }
            }
        }
        for (const auto& child : node->children) self(self, child);
    };
    walk(walk, root);
    // Double-sided MASK alone also matches chain-link and grating (MI_Fence,
    // MI_Metal_*, MI_MetalRope across the cooked set), which must not glow
    // green when back-lit. Every cooked leaf material names itself as such.
    const auto namesLeaf = [](std::string name) {
        for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        for (const char* key : { "foliage", "leaf", "leaves", "hedge", "flower" })
            if (name.find(key) != std::string::npos) return true;
        return false;
    };
    const bool thinLeavesOff = std::getenv("SGE_NO_THIN_LEAF") != nullptr;
    for (auto& [material, tally] : tallies) {
        material->thinLeaf = !thinLeavesOff && material->doubleSided &&
            namesLeaf(material->name);
        if (material->thinLeaf)
            SGE_LOG("LogPrefab", EngineLog::Level::Display,
                prefabId + ": " + material->name + " shades as thin leaf");
        if (tally.total == 0) continue;
        const float opposed = static_cast<float>(tally.opposed) / tally.total;
        material->bentNormals = opposed > 0.2f;
        if (material->bentNormals)
            SGE_LOG("LogPrefab", EngineLog::Level::Display,
                prefabId + ": " + material->name + " keeps bent normals (" +
                std::to_string(static_cast<int>(opposed * 100.0f + 0.5f)) +
                "% of " + std::to_string(tally.total) + " triangles oppose their face)");
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
        auto requiredCook = prefab.modelPath;
        requiredCook.replace_extension(".bistro.json");
        if (std::filesystem::exists(requiredCook)) {
            SGE_LOG("LogPrefab", EngineLog::Level::Error,
                "Bistro requires its BC cook; run scripts/Import-Bistro.ps1: " + cookedError);
            return nullptr;
        }
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
    MarkBentNormalCutouts(model, prefab.id);
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
    if (prefab.emissiveScale != 1.0f) {
        // Materials are shared between primitives; scale each one once.
        std::unordered_set<SceneMaterial*> scaled;
        const auto scaleEmission = [&](const auto& self,
                                       const std::shared_ptr<SceneNode>& node) -> void {
            if (!node) return;
            if (node->mesh) {
                for (MeshPrimitive& primitive : node->mesh->primitives) {
                    SceneMaterial* material = primitive.material.get();
                    if (!material || !scaled.insert(material).second) continue;
                    material->emissiveFactor.x *= prefab.emissiveScale;
                    material->emissiveFactor.y *= prefab.emissiveScale;
                    material->emissiveFactor.z *= prefab.emissiveScale;
                }
            }
            for (const auto& child : node->children) self(self, child);
        };
        scaleEmission(scaleEmission, model);
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
    const bool floorCollision = prefab.collision == "floor";
    if (prefab.collision == "mesh" || floorCollision) {
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
        // The cache file is per prefab id, so a mesh <-> floor switch or a new
        // material list has to change the key or the old tree is reused.
        if (floorCollision) {
            std::string filter = "floor";
            for (const std::string& prefix : prefab.collisionMaterials)
                filter += '\n' + prefix;
            cacheKey.buildParamsHash ^= std::hash<std::string>{}(filter) +
                0x9e3779b97f4a7c15ull + (cacheKey.buildParamsHash << 6) +
                (cacheKey.buildParamsHash >> 2);
        }

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
        const bool extracted = ExtractPrefabCollisionTriangles(model, triangles,
            &emptyPrimitives, nullptr,
            floorCollision ? &prefab.collisionMaterials : nullptr);
        const double extractMs = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - extractStarted).count();

        if (!extracted) {
            // Never build an empty tree silently: that would look like "the
            // airport has no collision" rather than "the vertices went away".
            SGE_LOG("LogPrefab", EngineLog::Level::Error,
                (floorCollision
                    ? "Floor collision: no CPU vertices on primitives matching collision.materials: "
                    : "Mesh collision requested but no CPU vertices available: ") +
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
            // A floor subset is legitimately smaller than the whole model; it
            // can only be checked for lying inside it.
            const bool soupOutsideModel =
                soupMinimum.x < entry.boundsMinimum.x - 1e-3f ||
                soupMinimum.y < entry.boundsMinimum.y - 1e-3f ||
                soupMinimum.z < entry.boundsMinimum.z - 1e-3f ||
                soupMaximum.x > entry.boundsMaximum.x + 1e-3f ||
                soupMaximum.y > entry.boundsMaximum.y + 1e-3f ||
                soupMaximum.z > entry.boundsMaximum.z + 1e-3f;
            if (floorCollision ? soupOutsideModel : boundsDrift > 1e-3f) {
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
