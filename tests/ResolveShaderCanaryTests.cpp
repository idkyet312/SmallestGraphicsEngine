// Guards the default (FXC, non-bindless) visibility resolve against accidental
// change.
//
// The resolve shader carries several variants behind #if guards -- enhanced
// visuals, and now bindless materials. The contract those guards rely on is
// that with every feature *off*, FXC emits exactly the DXBC it emitted before
// the feature existed. That is what catches a refactor which looks harmless in
// HLSL but silently reshapes the default pipeline every pixel goes through.
//
// This test enforces that contract the only way that actually holds over time:
// it records the current DXBC in a golden file and fails when the bytes move.
// An intentional change to the default path is expected to fail this once; the
// fix is to review the diff and refresh the golden file (delete it and re-run,
// or pass --update), not to weaken the check.
//
// Historical note: the in-shader comments cite 42020 bytes. That was the size
// when they were written; the shader has grown since (89,556 at the time this
// test was added). The invariant was never the literal number -- it is
// "unchanged with the feature off" -- so this test pins the bytes rather than a
// hard-coded length that goes stale the next time the shader legitimately grows.

#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <windows.h>
#include <wrl/client.h>

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;

static int failures = 0;
#define CHECK(value) do { if (!(value)) { \
    std::cerr << __FILE__ << ':' << __LINE__ << " CHECK failed: " #value "\n"; \
    ++failures; } } while (false)

int main(int argc, char** argv) {
    const std::string sourceDir = SGE_SOURCE_DIR;
    const std::string shaderPath = sourceDir + "/shaders/visbuf_resolve_cs.hlsl";
    const std::string postShaderPath =
        sourceDir + "/shaders/visbuf_post_cs.hlsl";
    const std::string goldenPath =
        sourceDir + "/tests/golden/visbuf_resolve_cs.fxc.size";

    bool update = false;
    for (int i = 1; i < argc; ++i)
        if (std::string(argv[i]) == "--update") update = true;

    std::ifstream shaderFile(shaderPath);
    if (!shaderFile.is_open()) {
        std::cerr << "Failed to open " << shaderPath << "\n";
        return 1;
    }
    std::stringstream shaderStream;
    shaderStream << shaderFile.rdbuf();
    const std::string code = shaderStream.str();

    // Exactly the arguments VisibilityBufferDX12::CreateResolvePipeline uses for
    // the default variant. Any divergence here would make the test guard a
    // shader the engine never builds.
    const UINT compileFlags =
        D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3;

    ComPtr<ID3DBlob> blob;
    ComPtr<ID3DBlob> errors;
    const HRESULT hr = D3DCompile(code.c_str(), code.size(), shaderPath.c_str(),
        nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "cs_5_1",
        compileFlags, 0, &blob, &errors);
    if (FAILED(hr)) {
        std::cerr << "Default resolve failed to compile: "
                  << (errors ? (const char*)errors->GetBufferPointer() : "unknown")
                  << "\n";
        return 1;
    }

    const size_t size = blob->GetBufferSize();
    const char* bytes = static_cast<const char*>(blob->GetBufferPointer());

    std::ifstream postShaderFile(postShaderPath);
    if (!postShaderFile.is_open()) {
        std::cerr << "Failed to open " << postShaderPath << "\n";
        return 1;
    }
    std::stringstream postShaderStream;
    postShaderStream << postShaderFile.rdbuf();
    const std::string postCode = postShaderStream.str();

    ComPtr<ID3DBlob> postBlob;
    errors.Reset();
    const HRESULT postHr = D3DCompile(
        postCode.c_str(), postCode.size(), postShaderPath.c_str(), nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "cs_5_0", compileFlags, 0,
        &postBlob, &errors);
    if (FAILED(postHr)) {
        std::cerr << "Post resolve failed to compile: "
                  << (errors ? (const char*)errors->GetBufferPointer()
                             : "unknown")
                  << "\n";
        return 1;
    }

    ComPtr<ID3D11ShaderReflection> reflection;
    const HRESULT reflectHr = D3DReflect(
        postBlob->GetBufferPointer(), postBlob->GetBufferSize(),
        __uuidof(ID3D11ShaderReflection),
        reinterpret_cast<void**>(reflection.GetAddressOf()));
    if (FAILED(reflectHr)) {
        std::cerr << "Failed to reflect post resolve shader\n";
        return 1;
    }

    auto checkBinding = [&](const char* name, D3D_SHADER_INPUT_TYPE type,
                            UINT bindPoint) {
        D3D11_SHADER_INPUT_BIND_DESC binding = {};
        const HRESULT bindingHr =
            reflection->GetResourceBindingDescByName(name, &binding);
        CHECK(SUCCEEDED(bindingHr));
        if (SUCCEEDED(bindingHr)) {
            CHECK(binding.Type == type);
            CHECK(binding.BindPoint == bindPoint);
            CHECK(binding.BindCount == 1u);
        }
    };
    checkBinding("stableSurfaceHistory", D3D_SIT_TEXTURE, 9u);
    checkBinding("drawCalls", D3D_SIT_STRUCTURED, 10u);
    checkBinding("stableTriangleIDs", D3D_SIT_STRUCTURED, 11u);
    checkBinding("stableSurfaceOutput", D3D_SIT_UAV_RWTYPED, 2u);

    D3D11_SHADER_BUFFER_DESC postConstants = {};
    ID3D11ShaderReflectionConstantBuffer* postConstantBuffer =
        reflection->GetConstantBufferByName("PostConstants");
    CHECK(postConstantBuffer != nullptr);
    if (postConstantBuffer) {
        CHECK(SUCCEEDED(postConstantBuffer->GetDesc(&postConstants)));
        CHECK(postConstants.Size == 80u);
    }

    // The golden file stores the DXBC itself, so this catches a change that
    // happens to preserve the length -- reordered arithmetic usually does.
    std::ifstream golden(goldenPath, std::ios::binary);
    if (!golden.is_open() || update) {
        std::ofstream out(goldenPath, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            std::cerr << "Cannot write golden file " << goldenPath
                      << " (missing tests/golden/ directory?)\n";
            return 1;
        }
        out.write(bytes, (std::streamsize)size);
        std::cout << "Recorded golden default resolve DXBC (" << size
                  << " bytes)\n";
        return 0;
    }

    std::vector<char> expected((std::istreambuf_iterator<char>(golden)),
                                std::istreambuf_iterator<char>());
    CHECK(expected.size() == size);
    if (expected.size() == size)
        CHECK(memcmp(expected.data(), bytes, size) == 0);

    if (failures == 0) {
        std::cout << "ResolveShaderCanaryTests passed (" << size
                  << " bytes, unchanged)\n";
        return 0;
    }
    std::cerr << "\nThe default visibility resolve DXBC changed.\n"
                 "Expected " << expected.size() << " bytes, got " << size << ".\n"
                 "If this was intentional, review the diff and re-run with "
                 "--update to refresh tests/golden/.\n";
    return 1;
}
