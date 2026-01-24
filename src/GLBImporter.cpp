#define TINYGLTF_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
// Define these to avoid tinygltf including them if they are not found, 
// but we hope vcpkg provided them or we need to provide them. 
// Actually tinygltf header usually includes them.
#include <tiny_gltf.h>

#include "GLBImporter.h"
#include <iostream>
#include <vector>

using namespace DirectX;

// Helper to copy data from GLTF buffer to vector
template<typename T>
void CopyBufferData(const tinygltf::Model& model, int accessorIndex, std::vector<T>& outData) {
    if (accessorIndex < 0) return;
    const tinygltf::Accessor& accessor = model.accessors[accessorIndex];
    const tinygltf::BufferView& bufferView = model.bufferViews[accessor.bufferView];
    const tinygltf::Buffer& buffer = model.buffers[bufferView.buffer];
    
    const unsigned char* dataStart = &buffer.data[bufferView.byteOffset + accessor.byteOffset];
    size_t dataStep = accessor.ByteStride(bufferView);
    
    outData.reserve(accessor.count);
    for (size_t i = 0; i < accessor.count; i++) {
        const T* val = reinterpret_cast<const T*>(dataStart + i * dataStep);
        outData.push_back(*val);
    }
}

// Helper to load texture
ComPtr<ID3D12Resource> CreateTexture(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList,
    const tinygltf::Image& image, std::vector<ComPtr<ID3D12Resource>>& uploadHeaps) {
    if (image.width == 0 || image.height == 0) return nullptr;

    D3D12_RESOURCE_DESC textureDesc = {};
    textureDesc.MipLevels = 1;
    textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    textureDesc.Width = image.width;
    textureDesc.Height = image.height;
    textureDesc.Flags = D3D12_RESOURCE_FLAG_NONE;
    textureDesc.DepthOrArraySize = 1;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.SampleDesc.Quality = 0;
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    ComPtr<ID3D12Resource> texture;
    if (FAILED(device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &textureDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(&texture)))) {
        return nullptr;
    }

    // Prepare data (force RGBA)
    std::vector<unsigned char> rgba;
    const unsigned char* pSource = image.image.data();
    if (image.component == 3) {
        rgba.resize(image.width * image.height * 4);
        for (size_t i = 0; i < (size_t)(image.width * image.height); i++) {
            rgba[i * 4 + 0] = pSource[i * 3 + 0];
            rgba[i * 4 + 1] = pSource[i * 3 + 1];
            rgba[i * 4 + 2] = pSource[i * 3 + 2];
            rgba[i * 4 + 3] = 255;
        }
        pSource = rgba.data();
    }
    // assume 4 component or 3 component forced to 4. 

    UINT64 uploadBufferSize;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint;
    device->GetCopyableFootprints(&textureDesc, 0, 1, 0, &footprint, nullptr, nullptr, &uploadBufferSize);

    D3D12_HEAP_PROPERTIES uploadHeapProps = {};
    uploadHeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC uploadDesc = {};
    uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    uploadDesc.Width = uploadBufferSize;
    uploadDesc.Height = 1;
    uploadDesc.DepthOrArraySize = 1;
    uploadDesc.MipLevels = 1;
    uploadDesc.Format = DXGI_FORMAT_UNKNOWN;
    uploadDesc.SampleDesc.Count = 1;
    uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    uploadDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

    ComPtr<ID3D12Resource> uploadHeap;
    if (FAILED(device->CreateCommittedResource(
        &uploadHeapProps, D3D12_HEAP_FLAG_NONE, &uploadDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&uploadHeap)))) {
        return nullptr;
    }

    uploadHeaps.push_back(uploadHeap);

    BYTE* pData;
    uploadHeap->Map(0, nullptr, (void**)&pData);

    BYTE* pDest = pData + footprint.Offset;
    for (UINT h = 0; h < (UINT)image.height; ++h) {
        memcpy(pDest + h * footprint.Footprint.RowPitch,
            pSource + h * image.width * 4,
            image.width * 4);
    }
    uploadHeap->Unmap(0, nullptr);

    D3D12_TEXTURE_COPY_LOCATION dst = {};
    dst.pResource = texture.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;

    D3D12_TEXTURE_COPY_LOCATION src = {};
    src.pResource = uploadHeap.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = footprint;

    cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = texture.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

    cmdList->ResourceBarrier(1, &barrier);

    return texture;
}

// Convert GLTF mesh to SceneMesh
std::shared_ptr<SceneMesh> ProcessMesh(const tinygltf::Model& model, const tinygltf::Mesh& gltfMesh, ID3D12Device* device, const std::vector<std::shared_ptr<SceneMaterial>>& materials) {
    auto sceneMesh = std::make_shared<SceneMesh>();
    sceneMesh->name = gltfMesh.name;
    
    for (const auto& primitive : gltfMesh.primitives) {
        MeshPrimitive meshPrim;
        meshPrim.materialIndex = primitive.material;
        if (meshPrim.materialIndex >= 0 && meshPrim.materialIndex < materials.size()) {
            meshPrim.material = materials[meshPrim.materialIndex];
        }

        // Get accessors
        int posIdx = -1;
        int normIdx = -1;
        int texIdx = -1;
        
        if (primitive.attributes.find("POSITION") != primitive.attributes.end())
            posIdx = primitive.attributes.at("POSITION");
        if (primitive.attributes.find("NORMAL") != primitive.attributes.end())
            normIdx = primitive.attributes.at("NORMAL");
        if (primitive.attributes.find("TEXCOORD_0") != primitive.attributes.end())
            texIdx = primitive.attributes.at("TEXCOORD_0");
            
        // Extract data
        std::vector<XMFLOAT3> positions;
        std::vector<XMFLOAT3> normals;
        std::vector<XMFLOAT2> texCoords;
        
        if (posIdx >= 0) CopyBufferData(model, posIdx, positions);
        if (normIdx >= 0) CopyBufferData(model, normIdx, normals);
        if (texIdx >= 0) CopyBufferData(model, texIdx, texCoords);
        
        // Resize to match positions
        if (normals.empty() && !positions.empty()) normals.resize(positions.size(), XMFLOAT3(0, 1, 0));
        if (texCoords.empty() && !positions.empty()) texCoords.resize(positions.size(), XMFLOAT2(0, 0));
        
        // Interleave data
        for (size_t i = 0; i < positions.size(); i++) {
            meshPrim.vertices.push_back(positions[i].x);
            meshPrim.vertices.push_back(positions[i].y);
            meshPrim.vertices.push_back(positions[i].z);
            
            meshPrim.vertices.push_back(normals[i].x);
            meshPrim.vertices.push_back(normals[i].y);
            meshPrim.vertices.push_back(normals[i].z);
            
            meshPrim.vertices.push_back(texCoords[i].x);
            meshPrim.vertices.push_back(texCoords[i].y);
        }
        
        // Indices
        if (primitive.indices >= 0) {
            const tinygltf::Accessor& accessor = model.accessors[primitive.indices];
            const tinygltf::BufferView& bufferView = model.bufferViews[accessor.bufferView];
            const tinygltf::Buffer& buffer = model.buffers[bufferView.buffer];
            
            const unsigned char* dataStart = &buffer.data[bufferView.byteOffset + accessor.byteOffset];
            size_t dataStep = accessor.ByteStride(bufferView);
            
            for (size_t i = 0; i < accessor.count; i++) {
                if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT) {
                    const unsigned short* val = reinterpret_cast<const unsigned short*>(dataStart + i * dataStep);
                    meshPrim.indices.push_back(*val);
                } else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT) {
                    const unsigned int* val = reinterpret_cast<const unsigned int*>(dataStart + i * dataStep);
                    meshPrim.indices.push_back(*val);
                } else if (accessor.componentType == TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE) {
                    const unsigned char* val = reinterpret_cast<const unsigned char*>(dataStart + i * dataStep);
                    meshPrim.indices.push_back(*val);
                }
            }
        }
        
        meshPrim.indexCount = (UINT)meshPrim.indices.size();
        
        // Create Buffers (Upload Heap for simplicity)
        if (device) {
            // Vertex Buffer
            UINT vbSize = (UINT)(meshPrim.vertices.size() * sizeof(float));
            D3D12_HEAP_PROPERTIES heapProps = {};
            heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
            heapProps.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
            heapProps.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;
            
            D3D12_RESOURCE_DESC bufferDesc = {};
            bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bufferDesc.Width = vbSize;
            bufferDesc.Height = 1;
            bufferDesc.DepthOrArraySize = 1;
            bufferDesc.MipLevels = 1;
            bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
            bufferDesc.SampleDesc.Count = 1;
            bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            bufferDesc.Flags = D3D12_RESOURCE_FLAG_NONE;

            HRESULT hr = device->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&meshPrim.vertexBuffer));
                
            if (SUCCEEDED(hr)) {
                void* mappedData;
                meshPrim.vertexBuffer->Map(0, nullptr, &mappedData);
                memcpy(mappedData, meshPrim.vertices.data(), vbSize);
                meshPrim.vertexBuffer->Unmap(0, nullptr);
                
                meshPrim.vbv.BufferLocation = meshPrim.vertexBuffer->GetGPUVirtualAddress();
                meshPrim.vbv.SizeInBytes = vbSize;
                meshPrim.vbv.StrideInBytes = 8 * sizeof(float); // 3 pos + 3 norm + 2 uv
            }
            
            // Index Buffer
            if (!meshPrim.indices.empty()) {
                UINT ibSize = (UINT)(meshPrim.indices.size() * sizeof(unsigned int));
                bufferDesc.Width = ibSize;
                
                hr = device->CreateCommittedResource(
                    &heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                    IID_PPV_ARGS(&meshPrim.indexBuffer));
                    
                if (SUCCEEDED(hr)) {
                    void* mappedData;
                    meshPrim.indexBuffer->Map(0, nullptr, &mappedData);
                    memcpy(mappedData, meshPrim.indices.data(), ibSize);
                    meshPrim.indexBuffer->Unmap(0, nullptr);
                    
                    meshPrim.ibv.BufferLocation = meshPrim.indexBuffer->GetGPUVirtualAddress();
                    meshPrim.ibv.SizeInBytes = ibSize;
                    meshPrim.ibv.Format = DXGI_FORMAT_R32_UINT;
                }
            }
        }
        
        sceneMesh->primitives.push_back(meshPrim);
    }
    
    return sceneMesh;
}

void ProcessNode(const tinygltf::Model& model, int nodeIndex, SceneNode* parentNode, std::vector<std::shared_ptr<SceneMesh>>& meshes) {
    const tinygltf::Node& gltfNode = model.nodes[nodeIndex];
    auto newNode = std::make_shared<SceneNode>(gltfNode.name);
    
    // Transform
    if (!gltfNode.matrix.empty()) {
        // Decompose or use as is. For simplicity, we assume translation/rotation/scale or matrix.
        // If matrix is provided, we might need to decompose if we want TRS.
        // For this simple implementation, if matrix exists, we just set the local transform directly.
        // But SceneNode splits TRS. 
        // Let's check for T, R, S.
    }
    
    if (!gltfNode.translation.empty()) {
        newNode->translation = XMFLOAT3((float)gltfNode.translation[0], (float)gltfNode.translation[1], (float)gltfNode.translation[2]);
    }
    if (!gltfNode.rotation.empty()) {
        newNode->rotation = XMFLOAT4((float)gltfNode.rotation[0], (float)gltfNode.rotation[1], (float)gltfNode.rotation[2], (float)gltfNode.rotation[3]);
    }
    if (!gltfNode.scale.empty()) {
        newNode->scale = XMFLOAT3((float)gltfNode.scale[0], (float)gltfNode.scale[1], (float)gltfNode.scale[2]);
    }
    
    // Mesh
    if (gltfNode.mesh >= 0) {
        newNode->mesh = meshes[gltfNode.mesh];
    }
    
    parentNode->AddChild(newNode);
    
    for (int childIndex : gltfNode.children) {
        ProcessNode(model, childIndex, newNode.get(), meshes);
    }
}

std::shared_ptr<SceneNode> GLBImporter::LoadGLB(const std::string& filepath, ComPtr<ID3D12Device> device, ComPtr<ID3D12GraphicsCommandList> commandList) {
    tinygltf::Model model;
    tinygltf::TinyGLTF loader;
    std::string err;
    std::string warn;

    bool ret = loader.LoadBinaryFromFile(&model, &err, &warn, filepath);
    if (!warn.empty()) std::cout << "GLB Warning: " << warn << std::endl;
    if (!err.empty()) std::cerr << "GLB Error: " << err << std::endl;
    if (!ret) return nullptr;

    // Load Materials
    std::vector<std::shared_ptr<SceneMaterial>> materials;
    for (const auto& mat : model.materials) {
        auto sceneMat = std::make_shared<SceneMaterial>();
        sceneMat->name = mat.name;

        // Factors
        if (mat.pbrMetallicRoughness.baseColorFactor.size() == 4) {
            sceneMat->baseColorFactor = XMFLOAT4(
                (float)mat.pbrMetallicRoughness.baseColorFactor[0],
                (float)mat.pbrMetallicRoughness.baseColorFactor[1],
                (float)mat.pbrMetallicRoughness.baseColorFactor[2],
                (float)mat.pbrMetallicRoughness.baseColorFactor[3]);
        }
        sceneMat->roughnessFactor = (float)mat.pbrMetallicRoughness.roughnessFactor;
        sceneMat->metallicFactor = (float)mat.pbrMetallicRoughness.metallicFactor;

        // Textures
        // Base Color
        int baseColorIdx = mat.pbrMetallicRoughness.baseColorTexture.index;
        if (baseColorIdx >= 0) {
            int imgIdx = model.textures[baseColorIdx].source;
            if (imgIdx >= 0 && imgIdx < model.images.size())
                sceneMat->baseColorTexture = CreateTexture(device.Get(), commandList.Get(), model.images[imgIdx], sceneMat->uploadHeaps);
        }

        // Metallic Roughness
        int mrIdx = mat.pbrMetallicRoughness.metallicRoughnessTexture.index;
        if (mrIdx >= 0) {
            int imgIdx = model.textures[mrIdx].source;
            if (imgIdx >= 0 && imgIdx < model.images.size())
                sceneMat->metallicRoughnessTexture = CreateTexture(device.Get(), commandList.Get(), model.images[imgIdx], sceneMat->uploadHeaps);
        }

        // Normal
        int nIdx = mat.normalTexture.index;
        if (nIdx >= 0) {
            int imgIdx = model.textures[nIdx].source;
            if (imgIdx >= 0 && imgIdx < model.images.size())
                sceneMat->normalTexture = CreateTexture(device.Get(), commandList.Get(), model.images[imgIdx], sceneMat->uploadHeaps);
        }

        materials.push_back(sceneMat);
    }

    // Convert Meshes
    std::vector<std::shared_ptr<SceneMesh>> convertedMeshes;
    for (const auto& mesh : model.meshes) {
        convertedMeshes.push_back(ProcessMesh(model, mesh, device.Get(), materials));
    }

    // Process Scene
    auto rootNode = std::make_shared<SceneNode>("ModelRoot");
    const tinygltf::Scene& scene = model.scenes[model.defaultScene > -1 ? model.defaultScene : 0];

    for (int nodeIndex : scene.nodes) {
        ProcessNode(model, nodeIndex, rootNode.get(), convertedMeshes);
    }

    // Initial update
    rootNode->UpdateGlobalTransform(rootNode->localTransform);

    return rootNode;
}

