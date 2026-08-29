#pragma once
#include <cstdint>
#include <string>
#include <memory>
#include <vector>
#include <array>
#include "SceneGraph.h"
#include "SkinnedTypes.h"
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
    static std::shared_ptr<SceneNode> LoadGLB(
        const std::string& filepath,
        Microsoft::WRL::ComPtr<ID3D12Device> device,
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList,
        bool immediateMipUpload = false);

    // Same load, but also fills a Skeleton from the file's first glTF skin (bone
    // names, parents, inverse-bind matrices and bind-pose locals). Primitives
    // carrying JOINTS_0/WEIGHTS_0 get their skin buffers either way; this is
    // what a caller needs to drive them, since posing requires the hierarchy.
    // outSkeleton is left empty for unskinned files.
    static std::shared_ptr<SceneNode> LoadGLBSkinned(
        const std::string& filepath,
        Microsoft::WRL::ComPtr<ID3D12Device> device,
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList,
        Skeleton& outSkeleton);
    static Microsoft::WRL::ComPtr<ID3D12Resource> LoadTextureFromFile(const std::string& filepath, Microsoft::WRL::ComPtr<ID3D12Device> device, Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList, std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>& uploadHeaps);
    static Microsoft::WRL::ComPtr<ID3D12Resource> LoadTextureFromMemory(
        const unsigned char* data, size_t size,
        Microsoft::WRL::ComPtr<ID3D12Device> device,
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList,
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>& uploadHeaps);
    static Microsoft::WRL::ComPtr<ID3D12Resource> LoadEmbeddedTextureRGBA256(
        const unsigned char* rgba, int width, int height,
        Microsoft::WRL::ComPtr<ID3D12Device> device,
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList,
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>& uploadHeaps);
    static Microsoft::WRL::ComPtr<ID3D12Resource> LoadTextureSingleMip(
        const std::string& filepath,
        Microsoft::WRL::ComPtr<ID3D12Device> device,
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList,
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>& uploadHeaps);

    // Loads an equirectangular EXR as a linear R32G32B32A32_FLOAT texture
    // (single mip). Used for the HDRI sky dome so it keeps full dynamic range.
    static Microsoft::WRL::ComPtr<ID3D12Resource> LoadEXRTextureFromFile(const std::string& filepath, Microsoft::WRL::ComPtr<ID3D12Device> device, Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList, std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>& uploadHeaps);

    // Loads an equirectangular EXR and projects its radiance onto 9 (L2)
    // spherical harmonic coefficients for cheap diffuse ambient/IBL lighting.
    // Returns all-zero coefficients (caller should fall back to a flat
    // ambient term) if the file can't be loaded.
    static std::array<DirectX::XMFLOAT3, 9> ComputeSkyIrradianceSH(
        const std::string& filepath, float environmentRotationRadians = 0.0f);

    // Finds the compact brightest region in an equirectangular HDRI and
    // returns its world-space direction and radiance chromaticity. This keeps
    // the analytic directional light aligned with the visible environment sun.
    static HDRISunLight ExtractHDRISunLight(const std::string& filepath,
                                            float targetLuminance = 2.1f,
                                            float environmentRotationRadians = 0.0f);

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

    // Decodes a heightmap without quantising its authored 16-bit grayscale
    // values through the normal RGBA8 texture path.
    static bool LoadPixelsGray16(const std::string& filepath,
                                 std::vector<uint16_t>& outGray,
                                 int& outWidth, int& outHeight);

    // Same, for an encoded image already in memory -- e.g. a texture embedded
    // inside an FBX, whose authored path points at the exporter's machine and
    // cannot be resolved on disk.
    static bool LoadPixelsRGBAFromMemory(const unsigned char* data, size_t size,
                                         std::vector<unsigned char>& outRGBA,
                                         int& outWidth, int& outHeight);

private:
    // Shared body of LoadGLB/LoadGLBSkinned. outSkeleton is null for the plain
    // static load, which also lets that path use the cooked asset cache.
    static std::shared_ptr<SceneNode> LoadGLBInternal(
        const std::string& filepath,
        Microsoft::WRL::ComPtr<ID3D12Device> device,
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList,
        Skeleton* outSkeleton, bool immediateMipUpload);
};
