#pragma once
#include <string>
#include <memory>
#include <vector>
#include <array>
#include "SceneGraph.h"
#include <d3d12.h>
#include <wrl/client.h>
#include <DirectXMath.h>

struct HDRISunLight {
    DirectX::XMFLOAT3 direction = { 0.0f, 1.0f, 0.0f };
    DirectX::XMFLOAT3 color = { 1.0f, 1.0f, 1.0f };
    float sourceLuminance = 0.0f;
    bool valid = false;
};

class GLBImporter {
public:
    static std::shared_ptr<SceneNode> LoadGLB(const std::string& filepath, Microsoft::WRL::ComPtr<ID3D12Device> device, Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList);
    static Microsoft::WRL::ComPtr<ID3D12Resource> LoadTextureFromFile(const std::string& filepath, Microsoft::WRL::ComPtr<ID3D12Device> device, Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList, std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>& uploadHeaps);

    // Loads an equirectangular EXR as a linear R32G32B32A32_FLOAT texture
    // (single mip). Used for the HDRI sky dome so it keeps full dynamic range.
    static Microsoft::WRL::ComPtr<ID3D12Resource> LoadEXRTextureFromFile(const std::string& filepath, Microsoft::WRL::ComPtr<ID3D12Device> device, Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList, std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>& uploadHeaps);

    // Loads an equirectangular EXR and projects its radiance onto 9 (L2)
    // spherical harmonic coefficients for cheap diffuse ambient/IBL lighting.
    // Returns all-zero coefficients (caller should fall back to a flat
    // ambient term) if the file can't be loaded.
    static std::array<DirectX::XMFLOAT3, 9> ComputeSkyIrradianceSH(const std::string& filepath);

    // Finds the compact brightest region in an equirectangular HDRI and
    // returns its world-space direction and radiance chromaticity. This keeps
    // the analytic directional light aligned with the visible environment sun.
    static HDRISunLight ExtractHDRISunLight(const std::string& filepath,
                                            float targetLuminance = 2.1f);

    // Collapses an entire (static) node hierarchy into one SceneNode per unique
    // material, each holding a single merged draw call. Each primitive's
    // vertices are baked from model-root-relative space (the node's transform
    // is folded into the vertex positions/normals/tangents), so the returned
    // root can still be moved/animated as a rigid whole exactly like the
    // original multi-node tree. Intended for static imports where per-node
    // animation isn't needed, to cut draw-call count for the Colour Pass.
    static std::shared_ptr<SceneNode> MergeSceneByMaterial(const std::shared_ptr<SceneNode>& modelRoot, Microsoft::WRL::ComPtr<ID3D12Device> device);

    // Flattens all primitives into one depth-only mesh. Colour material boundaries
    // are irrelevant to the current shadow shader and would only add draw calls.
    static std::shared_ptr<SceneNode> MergeSceneForDepth(const std::shared_ptr<SceneNode>& modelRoot, Microsoft::WRL::ComPtr<ID3D12Device> device);

    // Uploads conventional vertex/index buffers and builds meshoptimizer
    // meshlets (64 vertices, 124 triangles) for any generated primitive.
    static bool BuildMeshletData(MeshPrimitive& primitive, ID3D12Device* device,
                                 bool buildMeshlets = true);

    // Uploads an in-memory RGBA8 image (row-major, 4 bytes/texel) as a mipped
    // shader texture. Used for procedurally generated material textures.
    static Microsoft::WRL::ComPtr<ID3D12Resource> CreateTextureFromRGBA(
        ID3D12Device* device, ID3D12GraphicsCommandList* commandList,
        const std::vector<unsigned char>& rgba, int width, int height,
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>& uploadHeaps);

    // Decodes an image file to raw RGBA8 without uploading it. Lets callers
    // combine several source images into one texture -- e.g. assets that ship
    // metalness and roughness as separate greyscale maps, which the shader
    // expects packed into one (G = rough, B = metal). Returns false on failure.
    static bool LoadPixelsRGBA(const std::string& filepath,
                               std::vector<unsigned char>& outRGBA,
                               int& outWidth, int& outHeight);
};
