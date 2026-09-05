#pragma once

#include "GLBImporter.h"
#include <meshoptimizer.h>

// Generated once during prefab loading. Preserve the hierarchy and material
// seams; collision continues to use the original model.
inline std::shared_ptr<SceneNode> BuildAutomaticLod(
    const std::shared_ptr<SceneNode>& source, ID3D12Device* device,
    float ratio, float error, size_t& sourceTriangles, size_t& lodTriangles) {
    if (!source) return {};
    auto result = std::make_shared<SceneNode>(*source);
    result->parent = nullptr;
    result->children.clear();
    if (source->mesh) {
        result->mesh = std::make_shared<SceneMesh>();
        result->mesh->name = source->mesh->name;
        for (const MeshPrimitive& original : source->mesh->primitives) {
            sourceTriangles += original.indices.size() / 3;
            MeshPrimitive reduced;
            bool simplified = false;
            if (original.skin.empty() && original.indices.size() >= 300 &&
                !original.vertices.empty()) {
                reduced.indices.resize(original.indices.size());
                const size_t target = size_t(original.indices.size() * ratio) / 3 * 3;
                const size_t count = meshopt_simplify(reduced.indices.data(),
                    original.indices.data(), original.indices.size(),
                    original.vertices.data(), original.vertices.size() / 12,
                    12 * sizeof(float), target, error, meshopt_SimplifyLockBorder);
                if (count >= 3 && count < original.indices.size()) {
                    reduced.indices.resize(count);
                    reduced.vertices.resize(original.vertices.size());
                    const size_t vertices = meshopt_optimizeVertexFetch(
                        reduced.vertices.data(), reduced.indices.data(), count,
                        original.vertices.data(), original.vertices.size() / 12,
                        12 * sizeof(float));
                    reduced.vertices.resize(vertices * 12);
                    reduced.material = original.material;
                    reduced.materialIndex = original.materialIndex;
                    simplified = GLBImporter::BuildMeshletData(reduced, device);
                }
            }
            // Small, skinned, seam-constrained or failed primitives retain the
            // source geometry instead of dropping surfaces to meet a budget.
            lodTriangles += simplified ? reduced.indices.size() / 3 : original.indices.size() / 3;
            result->mesh->primitives.push_back(simplified ? std::move(reduced) : original);
        }
    }
    for (const auto& child : source->children)
        result->AddChild(BuildAutomaticLod(child, device, ratio, error,
            sourceTriangles, lodTriangles));
    return result;
}
