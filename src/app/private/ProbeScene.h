#pragma once

// Private application implementation; included once by main.cpp in dependency order.

static uint64_t DXRDDGIHash(uint64_t hash, uint64_t value) {
    hash ^= value;
    return hash * 1099511628211ull;
}

static void DXRDDGIHashFloat(uint64_t& hash, float value) {
    uint32_t bits = 0;
    memcpy(&bits, &value, sizeof(bits));
    hash = DXRDDGIHash(hash, bits);
}

static uint64_t DXRDDGIStringHash(const std::string& value) {
    uint64_t hash = 1469598103934665603ull;
    for (unsigned char character : value)
        hash = DXRDDGIHash(hash, character);
    return hash;
}

static void AppendDXRDDGINodeTriangles(
    const std::shared_ptr<SceneNode>& node, CXMMATRIX instanceWorld,
    uint64_t entityId, uint64_t& geometryHash,
    std::vector<DXRProbeTriangle>& triangles, uint32_t& primitiveOrdinal) {
    if (!node) return;
    const XMMATRIX world = XMLoadFloat4x4(&node->globalTransform) *
                           instanceWorld;
    if (node->mesh) {
        for (const MeshPrimitive& primitive : node->mesh->primitives) {
            const uint32_t ordinal = primitiveOrdinal++;
            const size_t vertexCount = primitive.vertices.size() / 12u;
            const size_t indexCount = primitive.indices.empty()
                ? vertexCount : primitive.indices.size();
            for (size_t i = 0; i + 2 < indexCount; i += 3) {
                const uint32_t indices[3] = {
                    primitive.indices.empty() ? static_cast<uint32_t>(i) :
                                               primitive.indices[i],
                    primitive.indices.empty() ? static_cast<uint32_t>(i + 1) :
                                               primitive.indices[i + 1],
                    primitive.indices.empty() ? static_cast<uint32_t>(i + 2) :
                                               primitive.indices[i + 2]
                };
                if (indices[0] >= vertexCount || indices[1] >= vertexCount ||
                    indices[2] >= vertexCount)
                    continue;
                DXRProbeTriangle triangle;
                XMFLOAT3* points[3] = {
                    &triangle.a, &triangle.b, &triangle.c
                };
                for (uint32_t corner = 0; corner < 3; ++corner) {
                    const size_t base = static_cast<size_t>(indices[corner]) * 12u;
                    XMStoreFloat3(points[corner], XMVector3TransformCoord(
                        XMVectorSet(primitive.vertices[base],
                                    primitive.vertices[base + 1],
                                    primitive.vertices[base + 2], 1.0f), world));
                    DXRDDGIHashFloat(geometryHash, points[corner]->x);
                    DXRDDGIHashFloat(geometryHash, points[corner]->y);
                    DXRDDGIHashFloat(geometryHash, points[corner]->z);
                }
                triangle.sourceId = DXRProbeLayout::Hash64(entityId ^
                    (static_cast<uint64_t>(ordinal) << 32u) ^
                    static_cast<uint64_t>(i / 3u));
                triangles.push_back(triangle);
            }
        }
    }
    for (const auto& child : node->children)
        AppendDXRDDGINodeTriangles(child, instanceWorld, entityId,
            geometryHash, triangles, primitiveOrdinal);
}

static void AppendDXRDDGITerrain(std::vector<DXRProbeTriangle>& triangles,
                                 uint64_t& geometryHash) {
    auto params = CurrentTerrainParams();
    params.heightScale = scene.terrainHeightScale;
    const float width = params.tilesX * params.tileSize;
    const float depth = params.tilesZ * params.tileSize;
    const float step = (std::max)(2.0f,
        g_game.world.Level().dxrDDGI.surfaceSpacing);
    const uint32_t columns = static_cast<uint32_t>(std::ceil(width / step));
    const uint32_t rows = static_cast<uint32_t>(std::ceil(depth / step));
    // Grid min corner honours the tile origin offset so GI probes track an
    // edge-extended (off-center) island instead of the old centered rectangle.
    const float startX = (static_cast<float>(params.originTileX) -
        params.tilesX * 0.5f) * params.tileSize;
    const float startZ = (static_cast<float>(params.originTileZ) -
        params.tilesZ * 0.5f) * params.tileSize;
    auto point = [&](uint32_t x, uint32_t z) {
        const float px = startX + (std::min)(width, x * step);
        const float pz = startZ + (std::min)(depth, z * step);
        return XMFLOAT3(px, TerrainRendererDX12::HeightAt(params, px, pz), pz);
    };
    for (uint32_t z = 0; z < rows; ++z) {
        for (uint32_t x = 0; x < columns; ++x) {
            const XMFLOAT3 p00 = point(x, z);
            const XMFLOAT3 p10 = point(x + 1, z);
            const XMFLOAT3 p01 = point(x, z + 1);
            const XMFLOAT3 p11 = point(x + 1, z + 1);
            DXRProbeTriangle pair[2] = {
                { p00, p01, p10, DXRProbeLayout::Hash64(
                    0x5445525241494eull ^ (static_cast<uint64_t>(z) << 32u) ^ x) },
                { p10, p01, p11, DXRProbeLayout::Hash64(
                    0x5445525241494eull ^ (static_cast<uint64_t>(z) << 32u) ^ x ^
                    0x80000000ull) }
            };
            for (const DXRProbeTriangle& triangle : pair) {
                const XMFLOAT3 points[3] = {
                    triangle.a, triangle.b, triangle.c
                };
                for (const XMFLOAT3& p : points) {
                    DXRDDGIHashFloat(geometryHash, p.x);
                    DXRDDGIHashFloat(geometryHash, p.y);
                    DXRDDGIHashFloat(geometryHash, p.z);
                }
                triangles.push_back(triangle);
            }
        }
    }
}

static void BuildDXRDDGINodeScene(
    const std::shared_ptr<SceneNode>& node, uint64_t meshNamespace,
    const std::vector<XMMATRIX>& worlds, const std::vector<uint64_t>& entityIds,
    uint32_t& nodeOrdinal, ID3D12GraphicsCommandList4* commandList,
    std::vector<DXRScene::Instance>& instances) {
    if (!node) return;
    const uint32_t ordinal = nodeOrdinal++;
    if (node->mesh) {
        std::vector<DXRScene::Geometry> geometries;
        uint64_t sourceHash = 1469598103934665603ull;
        for (const MeshPrimitive& primitive : node->mesh->primitives) {
            if (!primitive.vertexBuffer || primitive.vertices.size() < 36)
                continue;
            DXRScene::Geometry geometry;
            geometry.vertexAddress =
                primitive.vertexBuffer->GetGPUVirtualAddress();
            geometry.vertexCount =
                static_cast<uint32_t>(primitive.vertices.size() / 12u);
            geometry.vertexStride = 12u * sizeof(float);
            geometry.indexAddress = primitive.indexBuffer
                ? primitive.indexBuffer->GetGPUVirtualAddress() : 0;
            geometry.indexCount = primitive.indexCount;
            geometry.indexFormat = primitive.ibv.Format;
            geometry.opaque = !primitive.material ||
                              !primitive.material->alphaCutout;
            // Snapshot the material into the hit record so the DXR closest-hit
            // shader shades with the real base colour instead of a constant.
            if (primitive.material) {
                geometry.baseColor[0] = primitive.material->baseColorFactor.x;
                geometry.baseColor[1] = primitive.material->baseColorFactor.y;
                geometry.baseColor[2] = primitive.material->baseColorFactor.z;
                geometry.baseColor[3] = primitive.material->baseColorFactor.w;
                geometry.metallic = primitive.material->metallicFactor;
                geometry.roughness = primitive.material->roughnessFactor;
            }
            // Bind this geometry to the visibility buffer's persistent copy of
            // the same mesh, so the inline RayQuery path can shade a hit from
            // the real triangle instead of approximating it with sky.
            //
            // Read rather than register: this runs during an acceleration
            // rebuild, which is not a frame, and RegisterPrimitive would upload
            // geometry outside the renderer's own ordering. A primitive the
            // rasterizer has not drawn yet simply stays invalid until the next
            // rebuild picks it up -- those rays keep today's approximation.
            UINT vbVertexOffset = 0, vbIndexOffset = 0, vbHasIndices = 0;
            if (visBuffer.MeshGeometryBinding(primitive.visibilityMeshID,
                    vbVertexOffset, vbIndexOffset, vbHasIndices)) {
                geometry.vbVertexOffset = vbVertexOffset;
                geometry.vbIndexOffset = vbIndexOffset;
                geometry.vbHasIndices = vbHasIndices;
                geometry.vbMeshValid = true;
                if (primitive.material) {
                    UINT materialID = 0;
                    UINT bindlessMaterialID = 0;
                    visBuffer.RegisterRaytracingMaterial(
                        primitive.material.get(), materialID,
                        bindlessMaterialID);
                    geometry.vbMaterialID = materialID;
                    geometry.vbBindlessMaterialID = bindlessMaterialID;
                }
            }
            geometries.push_back(geometry);
            sourceHash = DXRDDGIHash(sourceHash, geometry.vertexCount);
            sourceHash = DXRDDGIHash(sourceHash, geometry.indexCount);
            for (float component : primitive.vertices)
                DXRDDGIHashFloat(sourceHash, component);
            for (uint32_t index : primitive.indices)
                sourceHash = DXRDDGIHash(sourceHash, index);
        }
        if (!geometries.empty()) {
            const uint64_t meshId = DXRProbeLayout::Hash64(
                meshNamespace ^ ordinal);
            if (g_dxrDDGI.Scene().BuildMeshBLAS(commandList, meshId,
                                                sourceHash, geometries)) {
                const XMMATRIX nodeWorld =
                    XMLoadFloat4x4(&node->globalTransform);
                for (size_t i = 0; i < worlds.size(); ++i) {
                    DXRScene::Instance instance;
                    instance.meshId = meshId;
                    instance.entityId = i < entityIds.size() ? entityIds[i] : i;
                    XMStoreFloat4x4(&instance.transform,
                                    nodeWorld * worlds[i]);
                    instances.push_back(instance);
                }
            }
        }
    }
    for (const auto& child : node->children)
        BuildDXRDDGINodeScene(child, meshNamespace, worlds, entityIds,
            nodeOrdinal, commandList, instances);
}

// Transform-only counterpart to BuildDXRDDGINodeScene, for the per-frame TLAS
// refit.
//
// This MUST visit nodes and instances in exactly the order the build did, since
// the refit matches transforms to instance descriptors positionally. The two
// traversals are therefore kept structurally identical: same recursion order,
// same `worlds` loop, same skip conditions. The one condition that cannot be
// mirrored is `BuildMeshBLAS` succeeding -- so instead of re-testing it, this
// checks that a BLAS for the mesh id already exists, which is exactly what a
// successful build left behind.
//
// If the two ever diverge the refit is rejected on a count mismatch rather than
// silently shuffling transforms between instances, which is why the caller
// treats a false return as "fall back to the static TLAS" and not as an error.
static void GatherDXRDDGINodeTransforms(
    const std::shared_ptr<SceneNode>& node, uint64_t meshNamespace,
    const std::vector<XMMATRIX>& worlds, uint32_t& nodeOrdinal,
    std::vector<XMFLOAT4X4>& transforms) {
    if (!node) return;
    const uint32_t ordinal = nodeOrdinal++;
    if (node->mesh) {
        // Mirrors the build's "did any primitive qualify" test. A mesh whose
        // primitives were all skipped produced no BLAS and therefore no
        // instances, so it must contribute none here either.
        bool anyGeometry = false;
        for (const MeshPrimitive& primitive : node->mesh->primitives) {
            if (!primitive.vertexBuffer || primitive.vertices.size() < 36)
                continue;
            anyGeometry = true;
            break;
        }
        if (anyGeometry) {
            const uint64_t meshId = DXRProbeLayout::Hash64(
                meshNamespace ^ ordinal);
            if (g_dxrDDGI.Scene().HasMesh(meshId)) {
                const XMMATRIX nodeWorld =
                    XMLoadFloat4x4(&node->globalTransform);
                for (size_t i = 0; i < worlds.size(); ++i) {
                    XMFLOAT4X4 matrix;
                    XMStoreFloat4x4(&matrix, nodeWorld * worlds[i]);
                    transforms.push_back(matrix);
                }
            }
        }
    }
    for (const auto& child : node->children)
        GatherDXRDDGINodeTransforms(child, meshNamespace, worlds, nodeOrdinal,
                                    transforms);
}

// Per-frame TLAS transform refit. Cheap enough to run unconditionally: it walks
// the prefab batches, rebuilds the instance transform list, and issues a
// PERFORM_UPDATE over the existing acceleration structure. No BLAS work, no
// shader-table rewrite, no GPU stall.
//
// Returns false when the gathered list does not match the built instance count
// -- geometry was added or removed since the build, which needs the full
// rebuild path. The caller keeps tracing the previous structure in that case,
// which is stale but valid.
static bool RefitDXRDDGIAccelerationScene() {
    ComPtr<ID3D12GraphicsCommandList4> commandList;
    if (!g_dxrDDGI.Scene().Supported() ||
        g_dxrDDGI.Scene().RefitInstanceCount() == 0 ||
        FAILED(g_dx12.commandList.As(&commandList)))
        return false;

    std::vector<XMFLOAT4X4> transforms;
    transforms.reserve(g_dxrDDGI.Scene().RefitInstanceCount());
    for (const PrefabRenderBatch& batch : g_prefabRenderBatches) {
        uint32_t nodeOrdinal = 0;
        GatherDXRDDGINodeTransforms(batch.baseModel,
            DXRDDGIStringHash(batch.prefabId), batch.baseTransforms,
            nodeOrdinal, transforms);
    }
    if (wallModel && !g_ddgiCornellTestMode) {
        uint32_t nodeOrdinal = 0;
        const std::vector<XMMATRIX> worlds{ XMMatrixIdentity() };
        GatherDXRDDGINodeTransforms(wallModel, 0x484f555345ull, worlds,
            nodeOrdinal, transforms);
    }
    // Terrain is the last instance the build appends, and it never moves -- but
    // its identity transform still has to be re-emitted so the positional match
    // with the descriptor list holds through to the end of the list.
    //
    // Mirrors the build's own condition rather than inferring terrain's presence
    // from a count difference: an off-by-one from any other cause would look
    // identical to "terrain is missing" and would silently shift every
    // transform by one instance.
    if (scene.useMeshTerrain && g_terrain.supported && !g_ddgiCornellTestMode &&
        g_dxrDDGI.Scene().HasMesh(DXRScene::TerrainMeshId())) {
        XMFLOAT4X4 identity;
        XMStoreFloat4x4(&identity, XMMatrixIdentity());
        transforms.push_back(identity);
    }

    return g_dxrDDGI.Scene().RefitTLAS(commandList.Get(), transforms,
                                       g_dx12.frameIndex);
}

static bool BuildDXRDDGIAccelerationScene() {
    ComPtr<ID3D12GraphicsCommandList4> commandList;
    if (!g_dxrDDGI.Scene().Supported() ||
        FAILED(g_dx12.commandList.As(&commandList)))
        return false;
    // BLAS/TLAS resources and the persistently-mapped shader table are
    // replaced below. Any still-in-flight frame may be tracing against the
    // old ones (DispatchRays or the enhanced RayQuery resolve), so drain all
    // frame slots before touching any of it. Rare event; the sync is cheap
    // relative to a scene rebuild.
    WaitForGPUAllFrames();
    std::vector<DXRScene::Instance> instances;
    for (const PrefabRenderBatch& batch : g_prefabRenderBatches) {
        uint32_t nodeOrdinal = 0;
        BuildDXRDDGINodeScene(batch.baseModel,
            DXRDDGIStringHash(batch.prefabId), batch.baseTransforms,
            batch.entityIds, nodeOrdinal, commandList.Get(), instances);
    }
    if (wallModel && !g_ddgiCornellTestMode) {
        uint32_t nodeOrdinal = 0;
        const std::vector<XMMATRIX> worlds{ XMMatrixIdentity() };
        const std::vector<uint64_t> ids{ 0x484f555345ull };
        BuildDXRDDGINodeScene(wallModel, 0x484f555345ull, worlds, ids,
            nodeOrdinal, commandList.Get(), instances);
    }
    if (scene.useMeshTerrain && g_terrain.supported &&
        !g_ddgiCornellTestMode) {
        auto params = CurrentTerrainParams();
        params.heightScale = scene.terrainHeightScale;
        const float width = params.tilesX * params.tileSize;
        const float depth = params.tilesZ * params.tileSize;
        const float step = (std::max)(2.0f,
            g_game.world.Level().dxrDDGI.surfaceSpacing);
        const uint32_t columns =
            static_cast<uint32_t>(std::ceil(width / step));
        const uint32_t rows =
            static_cast<uint32_t>(std::ceil(depth / step));
        std::vector<XMFLOAT3> vertices;
        std::vector<uint32_t> indices;
        vertices.reserve((columns + 1u) * (rows + 1u));
        uint64_t terrainHash = 1469598103934665603ull;
        const float terrainStartX = (static_cast<float>(params.originTileX) -
            params.tilesX * 0.5f) * params.tileSize;
        const float terrainStartZ = (static_cast<float>(params.originTileZ) -
            params.tilesZ * 0.5f) * params.tileSize;
        for (uint32_t z = 0; z <= rows; ++z) {
            for (uint32_t x = 0; x <= columns; ++x) {
                const float px = terrainStartX +
                    (std::min)(width, x * step);
                const float pz = terrainStartZ +
                    (std::min)(depth, z * step);
                XMFLOAT3 vertex(
                    px, TerrainRendererDX12::HeightAt(params, px, pz), pz);
                vertices.push_back(vertex);
                DXRDDGIHashFloat(terrainHash, vertex.x);
                DXRDDGIHashFloat(terrainHash, vertex.y);
                DXRDDGIHashFloat(terrainHash, vertex.z);
            }
        }
        indices.reserve(columns * rows * 6u);
        for (uint32_t z = 0; z < rows; ++z) {
            for (uint32_t x = 0; x < columns; ++x) {
                const uint32_t a = z * (columns + 1u) + x;
                const uint32_t b = a + 1u;
                const uint32_t c = a + columns + 1u;
                const uint32_t d = c + 1u;
                indices.insert(indices.end(), { a, c, b, b, c, d });
            }
        }
        if (g_dxrDDGI.Scene().BuildTerrainBLAS(
                commandList.Get(), terrainHash, vertices, indices)) {
            DXRScene::Instance terrain;
            terrain.meshId = DXRScene::TerrainMeshId();
            terrain.entityId = 0;
            XMStoreFloat4x4(&terrain.transform, XMMatrixIdentity());
            instances.push_back(terrain);
        }
    }
    const bool updated = g_dxrDDGI.UpdateTLAS(commandList.Get(), instances);
    // Publish the per-geometry hit bindings the TLAS build just produced, so
    // the inline RayQuery path can shade a hit from the real triangle. Safe to
    // write in place here: WaitForGPUAllFrames above drained every frame slot.
    if (updated)
        visBuffer.UploadHitGeometry(g_dxrDDGI.Scene().HitGeometry());
    return updated;
}

static bool RebuildDXRDDGIProbeLayout(bool force) {
    g_dxrDDGI.ApplySettings(g_game.world.Level().dxrDDGI);
    scene.giMaxDistance = g_game.world.Level().dxrDDGI.maxRayDistance;
    if (!g_game.world.Level().dxrDDGI.enabled) return false;
    if (!force && !g_dxrDDGI.LayoutDirty()) return true;
    // Placed after the early-outs so the scope only covers a real rebuild; the
    // no-op calls that return above would otherwise flood the sample with 0 ms.
    ProfilerDX12::CpuScope probeLayoutProfile(g_profiler, "Editor/DXRProbeLayout");
    // Probe/atlas buffers may still be referenced by an older in-flight frame.
    // Drain EVERY frame slot, not just the current one: the DXR shader table
    // is persistently mapped and rewritten by the scene build below, so any
    // still-running DispatchRays from a previous slot would read it mid-write
    // (this crashed the live DDGI toggle).
    WaitForGPUAllFrames();
    g_dxrDDGI.ReleaseCompletedUploads();
    std::vector<DXRProbeTriangle> triangles;
    uint64_t geometryHash = 1469598103934665603ull;
    for (const PrefabRenderBatch& batch : g_prefabRenderBatches) {
        for (size_t i = 0; i < batch.baseTransforms.size(); ++i) {
            const uint64_t entityId = i < batch.entityIds.size()
                ? batch.entityIds[i]
                : DXRProbeLayout::Hash64(DXRDDGIStringHash(batch.prefabId) ^ i);
            uint32_t primitiveOrdinal = 0;
            AppendDXRDDGINodeTriangles(batch.baseModel, batch.baseTransforms[i],
                entityId, geometryHash, triangles, primitiveOrdinal);
        }
    }
    if (wallModel && !g_ddgiCornellTestMode) {
        uint32_t primitiveOrdinal = 0;
        AppendDXRDDGINodeTriangles(wallModel, XMMatrixIdentity(),
            0x484f555345ull, geometryHash, triangles, primitiveOrdinal);
    }
    if (scene.useMeshTerrain && g_terrain.supported &&
        !g_ddgiCornellTestMode)
        AppendDXRDDGITerrain(triangles, geometryHash);
    const std::filesystem::path cache = std::filesystem::path("Content/Levels") /
        ".ddgi" / (std::to_string(geometryHash) + ".ddgi");
    if (!g_dxrDDGI.BuildProbeLayout(triangles, geometryHash, cache)) {
        std::cerr << "[DDGI] BuildProbeLayout failed (triangles="
                  << triangles.size() << ")" << std::endl;
        return false;
    }
    if (!g_dxrDDGI.UploadProbeBuffers(g_dx12.commandList.Get())) {
        std::cerr << "[DDGI] UploadProbeBuffers failed" << std::endl;
        return false;
    }
    g_dxrDDGIProbeResource = g_dxrDDGI.ProbeBuffer();
    g_dxrDDGICellResource = g_dxrDDGI.CellBuffer();
    g_dxrDDGIIndexResource = g_dxrDDGI.IndexBuffer();
    g_dxrDDGIProbeCount =
        static_cast<UINT>(g_dxrDDGI.Layout().probes.size());
    g_dxrDDGICellCount =
        static_cast<UINT>(g_dxrDDGI.Layout().cells.size());
    g_dxrDDGIIndexCount =
        static_cast<UINT>(g_dxrDDGI.Layout().cellProbeIndices.size());
    g_dxrDDGICellSize = g_dxrDDGI.Layout().cellSize;
    g_ddgiIrradianceResource = g_dxrDDGI.IrradianceAtlas();
    g_ddgiVisibilityResource = g_dxrDDGI.VisibilityAtlas();
    visBuffer.UpdateDDGIResources(
        g_ddgiIrradianceResource, g_ddgiVisibilityResource);
    visBuffer.UpdateSparseDDGIResources(
        g_dxrDDGIProbeResource, g_dxrDDGIProbeCount,
        g_dxrDDGICellResource, g_dxrDDGICellCount,
        g_dxrDDGIIndexResource, g_dxrDDGIIndexCount);
    BuildDXRDDGIAccelerationScene();
    g_dxrDDGI.ResetHistory();
    return true;
}

static void DrawDXRDDGIProbeDebug(CXMMATRIX view, CXMMATRIX projection) {
    if (!g_game.world.Level().dxrDDGI.showProbes && !scene.showProbes) return;
    const auto& probes = g_dxrDDGI.GetDebugProbes();
    if (probes.empty()) return;
    const XMMATRIX viewProjection = view * projection;
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    for (const DXRProbeRecord& probe : probes) {
        const XMVECTOR clip = XMVector3Transform(
            XMLoadFloat3(&probe.position), viewProjection);
        const float w = XMVectorGetW(clip);
        if (w <= 0.01f) continue;
        const float x = (XMVectorGetX(clip) / w * 0.5f + 0.5f) * display.x;
        const float y = (1.0f - (XMVectorGetY(clip) / w * 0.5f + 0.5f)) *
                        display.y;
        const ImU32 color = probe.state == DXRProbeState::Valid
            ? IM_COL32(70, 235, 100, 220)
            : (probe.state == DXRProbeState::Rejected
                ? IM_COL32(245, 65, 55, 220)
                : IM_COL32(245, 180, 45, 220));
        draw->AddCircleFilled(ImVec2(x, y), 3.5f, color);
        draw->AddCircle(ImVec2(x, y), 5.0f, IM_COL32(0, 0, 0, 180));
    }
}
