// Stubs the two GLBImporter statics PalmMeshCutter.cpp calls, so the test
// target can compile/link PalmMeshCutter.cpp directly without pulling in the
// real GLBImporter.cpp (tinygltf/meshoptimizer/tinyexr/StaticBufferDX12/
// MipGenerator/TextureUploadArenaDX12 and the rest of its dependency chain).
// Same technique as tests/GLBImporterPixelStub.cpp for LoadPixelsRGBA.
//
// Both stubs are unreachable in these tests anyway: the tests never
// initialise a real D3D12 device, so PalmMeshCutter's own guards
// (`g_dx12.device`, `g_dx12.commandList` null-checks) skip the calls before
// they would ever run.
#include "GLBImporter.h"

bool GLBImporter::BuildMeshletData(MeshPrimitive&, ID3D12Device*, bool) {
    return false;
}

Microsoft::WRL::ComPtr<ID3D12Resource> GLBImporter::CreateTextureFromRGBA(
    ID3D12Device*, ID3D12GraphicsCommandList*,
    const std::vector<unsigned char>&, int, int,
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>&) {
    return nullptr;
}
