#pragma once

#include "DX12Core.h"
#include "FBXImporter.h"
#include "GLBImporter.h"
#include <algorithm>
#include <cfloat>
#include <filesystem>
#include <iostream>

inline std::shared_ptr<SceneNode>& GetRoofModel() {
    static std::shared_ptr<SceneNode> model;
    return model;
}

inline std::string ResolveRoofAsset(const std::string& relative) {
    for (const std::string& candidate : { relative, "build/" + relative, "../" + relative,
                                          "../../build/" + relative }) {
        if (std::filesystem::exists(candidate)) return candidate;
    }
    return relative;
}

inline void EnsureRoofModelLoaded() {
    static bool attempted = false;
    if (attempted) return;
    attempted = true;

    const std::string modelPath = ResolveRoofAsset("models/MetalRoof/Metalroof.fbx");
    std::cout << "Loading " << modelPath << "...\n";
    auto sheet = FBXImporter::Load(modelPath, g_dx12.device, g_dx12.commandList, 0.1f, false);
    if (!sheet || !sheet->mesh) {
        std::cerr << "Failed to load Metalroof.fbx\n";
        return;
    }

    float minX = FLT_MAX, minY = FLT_MAX, minZ = FLT_MAX;
    float maxX = -FLT_MAX, maxY = -FLT_MAX, maxZ = -FLT_MAX;
    for (const MeshPrimitive& primitive : sheet->mesh->primitives) {
        for (size_t v = 0; v + 11 < primitive.vertices.size(); v += 12) {
            minX = (std::min)(minX, primitive.vertices[v]);
            minY = (std::min)(minY, primitive.vertices[v + 1]);
            minZ = (std::min)(minZ, primitive.vertices[v + 2]);
            maxX = (std::max)(maxX, primitive.vertices[v]);
            maxY = (std::max)(maxY, primitive.vertices[v + 1]);
            maxZ = (std::max)(maxZ, primitive.vertices[v + 2]);
        }
    }
    const float width = maxX - minX, depth = maxZ - minZ;
    if (width < 0.001f || depth < 0.001f) return;

    // Use Poly Haven's real glTF-compatible PBR set on imported sheet geometry.
    const std::string pbr = "models/polyhaven/corrugated_iron/";
    for (MeshPrimitive& primitive : sheet->mesh->primitives) {
        if (!primitive.material) primitive.material = std::make_shared<SceneMaterial>();
        SceneMaterial& material = *primitive.material;
        material.baseColorTexture = GLBImporter::LoadTextureFromFile(
            ResolveRoofAsset(pbr + "corrugated_iron_diff_2k.jpg"),
            g_dx12.device, g_dx12.commandList, material.uploadHeaps);
        material.normalTexture = GLBImporter::LoadTextureFromFile(
            ResolveRoofAsset(pbr + "corrugated_iron_nor_dx_2k.jpg"),
            g_dx12.device, g_dx12.commandList, material.uploadHeaps);
        material.metallicRoughnessTexture = GLBImporter::LoadTextureFromFile(
            ResolveRoofAsset(pbr + "corrugated_iron_arm_2k.jpg"),
            g_dx12.device, g_dx12.commandList, material.uploadHeaps);
        material.baseColorFactor = { 1, 1, 1, 1 };
        material.metallicFactor = 1.0f;
        material.roughnessFactor = 1.0f;
        material.roughnessOnlyTexture = false;
    }

    auto root = std::make_shared<SceneNode>("SplitRoofFBX");
    auto addBuilding = [&](const char* prefix, float x0, float x1, float z0, float z1,
                           float eaveY, float ridgeY, int sections, int slopeRows) {
        const float halfRun = (x1 - x0) * 0.5f;
        const float rise = ridgeY - eaveY;
        const float slopeLength = std::sqrt(halfRun * halfRun + rise * rise);
        const float pitch = std::atan2(rise, halfRun);
        const float overhang = 0.28f;
        const float fullZ0 = z0 - overhang, fullZ1 = z1 + overhang;
        for (int side = 0; side < 2; ++side) {
            const float angle = side == 0 ? pitch : 3.14159265f - pitch;
            const DirectX::XMVECTOR q = DirectX::XMQuaternionRotationRollPitchYaw(0, 0, angle);
            for (int row = 0; row < slopeRows; ++row) for (int section = 0; section < sections; ++section) {
                const float sectionZ0 = fullZ0 + (fullZ1 - fullZ0) * section / sections;
                const float sectionZ1 = fullZ0 + (fullZ1 - fullZ0) * (section + 1) / sections;
                auto node = std::make_shared<SceneNode>(std::string(prefix) + "RoofPart@" +
                    std::to_string((side * slopeRows + row) * sections + section));
                node->mesh = sheet->mesh;
                const float pieceSlopeLength = slopeLength / slopeRows;
                const float sx = pieceSlopeLength / width;
                const float sz = (sectionZ1 - sectionZ0) / depth;
                const float sy = (std::min)(sx, sz);
                node->scale = { sx, sy, sz };
                DirectX::XMStoreFloat4(&node->rotation, q);

                // Anchor imported minimum corner exactly at each eave/section.
                const DirectX::XMMATRIX sr = DirectX::XMMatrixScaling(sx, sy, sz) *
                    DirectX::XMMatrixRotationQuaternion(q);
                const DirectX::XMVECTOR sourceAnchor = DirectX::XMVectorSet(minX, minY, minZ, 1);
                DirectX::XMFLOAT3 transformed;
                DirectX::XMStoreFloat3(&transformed,
                    DirectX::XMVector3TransformCoord(sourceAnchor, sr));
                const float eaveX = side == 0 ? x0 : x1;
                const float rowStart = row * pieceSlopeLength;
                const float rowX = eaveX + std::cos(angle) * rowStart;
                const float rowY = eaveY + std::sin(angle) * rowStart;
                node->translation = { rowX - transformed.x, rowY - transformed.y,
                                      sectionZ0 - transformed.z };
                root->AddChild(node);
            }
        }
    };

    addBuilding("Wood", -7.0f, 0.0f, 1.0f, 6.0f, 3.4f, 4.4f, 2, 1);
    addBuilding("Metal", 2.0f, 7.5f, 1.2f, 5.9f, 2.85f, 3.70f, 4, 2);

    DirectX::XMFLOAT4X4 identity;
    DirectX::XMStoreFloat4x4(&identity, DirectX::XMMatrixIdentity());
    root->UpdateGlobalTransform(identity);
    GetRoofModel() = root;
    std::cout << "Roof model loaded: 4 wood + 16 metal pieces\n";
}

inline void AppendRoofChunksToDestructionModel(const std::shared_ptr<SceneNode>& destructionRoot) {
    if (!destructionRoot) return;
    EnsureRoofModelLoaded();
    const auto& roofRoot = GetRoofModel();
    if (!roofRoot) return;
    for (const auto& existing : destructionRoot->children) {
        if (existing && existing->name.rfind("ImportedRoof@", 0) == 0) return;
    }

    int id = 100000; // separate from procedural plank-group IDs
    for (const auto& instance : roofRoot->children) {
        if (!instance || !instance->mesh) continue;
        auto chunk = std::make_shared<SceneNode>("ImportedRoof@" + std::to_string(id++));
        chunk->mesh = std::make_shared<SceneMesh>();
        const DirectX::XMMATRIX world = DirectX::XMLoadFloat4x4(&instance->globalTransform);
        const DirectX::XMMATRIX normalMatrix = DirectX::XMMatrixTranspose(DirectX::XMMatrixInverse(nullptr, world));
        for (const MeshPrimitive& source : instance->mesh->primitives) {
            MeshPrimitive primitive = source;
            primitive.vbv = {}; primitive.ibv = {};
            primitive.vertexBuffer.Reset(); primitive.indexBuffer.Reset();
            primitive.meshletDescBuffer.Reset(); primitive.meshletBoundsBuffer.Reset();
            primitive.meshletVertexIndexBuffer.Reset(); primitive.meshletTriangleBuffer.Reset();
            primitive.meshletCount = 0;
            for (size_t v = 0; v + 11 < primitive.vertices.size(); v += 12) {
                DirectX::XMVECTOR p = DirectX::XMVectorSet(
                    primitive.vertices[v], primitive.vertices[v + 1], primitive.vertices[v + 2], 1);
                DirectX::XMVECTOR n = DirectX::XMVectorSet(
                    primitive.vertices[v + 3], primitive.vertices[v + 4], primitive.vertices[v + 5], 0);
                DirectX::XMVECTOR t = DirectX::XMVectorSet(
                    primitive.vertices[v + 8], primitive.vertices[v + 9], primitive.vertices[v + 10], 0);
                p = DirectX::XMVector3TransformCoord(p, world);
                n = DirectX::XMVector3Normalize(DirectX::XMVector3TransformNormal(n, normalMatrix));
                t = DirectX::XMVector3Normalize(DirectX::XMVector3TransformNormal(t, world));
                DirectX::XMFLOAT3 pf, nf, tf;
                DirectX::XMStoreFloat3(&pf, p); DirectX::XMStoreFloat3(&nf, n); DirectX::XMStoreFloat3(&tf, t);
                primitive.vertices[v] = pf.x; primitive.vertices[v + 1] = pf.y; primitive.vertices[v + 2] = pf.z;
                primitive.vertices[v + 3] = nf.x; primitive.vertices[v + 4] = nf.y; primitive.vertices[v + 5] = nf.z;
                primitive.vertices[v + 8] = tf.x; primitive.vertices[v + 9] = tf.y; primitive.vertices[v + 10] = tf.z;
            }
            chunk->mesh->primitives.push_back(std::move(primitive));
        }
        destructionRoot->AddChild(chunk);
    }
}
