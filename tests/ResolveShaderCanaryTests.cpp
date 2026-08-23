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
    const std::string cullShaderPath =
        sourceDir + "/shaders/visibility_cull_cs.hlsl";
    const std::string visVertexShaderPath =
        sourceDir + "/shaders/visbuf_vs.hlsl";
    const std::string visPixelShaderPath =
        sourceDir + "/shaders/visbuf_ps.hlsl";
    const std::string grassVertexShaderPath =
        sourceDir + "/shaders/grass_vs.hlsl";
    const std::string grassShadowShaderPath =
        sourceDir + "/shaders/grass_shadow_vs.hlsl";
    const std::string clusteredPixelShaderPath =
        sourceDir + "/shaders/clustered_dx12_ps.hlsl";
    const std::string clusteredVertexShaderPath =
        sourceDir + "/shaders/clustered_dx12_vs.hlsl";
    const std::string fallbackPixelShaderPath =
        sourceDir + "/shaders/clustered_ps.hlsl";
    const std::string fallbackVertexShaderPath =
        sourceDir + "/shaders/clustered_vs.hlsl";
    const std::string simplePixelShaderPath =
        sourceDir + "/shaders/simple_ps.hlsl";
    const std::string ddgiShaderPath =
        sourceDir + "/shaders/ddgi_update_cs.hlsl";
    const std::string volumetricFogShaderPath =
        sourceDir + "/shaders/volumetric_fog.hlsl";
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

    // Full levels select the terrain twin lazily; the empty-level smoke test
    // and the golden default variant above never compile this branch. Keep the
    // runtime FXC contract covered so an error cannot first appear at the final
    // loading-screen stage.
    const std::string terrainCode =
        "#define SGE_TERRAIN_VISIBILITY 1\n" + code;
    ComPtr<ID3DBlob> terrainBlob;
    errors.Reset();
    const HRESULT terrainHr = D3DCompile(
        terrainCode.c_str(), terrainCode.size(), shaderPath.c_str(), nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "cs_5_1", compileFlags, 0,
        &terrainBlob, &errors);
    if (FAILED(terrainHr)) {
        std::cerr << "Terrain resolve failed to compile: "
                  << (errors ? (const char*)errors->GetBufferPointer()
                             : "unknown")
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

    // The visibility culler is compiled through the runtime FXC path rather
    // than CMake. Compile and reflect it here so a C++/HLSL input-contract edit
    // cannot ship with a typo or a shifted resource binding that appears only
    // when the application first reaches the visibility path.
    std::ifstream cullShaderFile(cullShaderPath);
    if (!cullShaderFile.is_open()) {
        std::cerr << "Failed to open " << cullShaderPath << "\n";
        return 1;
    }
    std::stringstream cullShaderStream;
    cullShaderStream << cullShaderFile.rdbuf();
    const std::string cullCode = cullShaderStream.str();

    ComPtr<ID3DBlob> cullBlob;
    errors.Reset();
    const HRESULT cullHr = D3DCompile(
        cullCode.c_str(), cullCode.size(), cullShaderPath.c_str(), nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "cs_5_0", compileFlags, 0,
        &cullBlob, &errors);
    if (FAILED(cullHr)) {
        std::cerr << "Visibility cull shader failed to compile: "
                  << (errors ? (const char*)errors->GetBufferPointer()
                             : "unknown")
                  << "\n";
        return 1;
    }

    ComPtr<ID3D11ShaderReflection> cullReflection;
    const HRESULT cullReflectHr = D3DReflect(
        cullBlob->GetBufferPointer(), cullBlob->GetBufferSize(),
        __uuidof(ID3D11ShaderReflection),
        reinterpret_cast<void**>(cullReflection.GetAddressOf()));
    if (FAILED(cullReflectHr)) {
        std::cerr << "Failed to reflect visibility cull shader\n";
        return 1;
    }
    auto checkCullBinding = [&](const char* name, D3D_SHADER_INPUT_TYPE type,
                                UINT bindPoint) {
        D3D11_SHADER_INPUT_BIND_DESC binding = {};
        const HRESULT bindingHr =
            cullReflection->GetResourceBindingDescByName(name, &binding);
        CHECK(SUCCEEDED(bindingHr));
        if (SUCCEEDED(bindingHr)) {
            CHECK(binding.Type == type);
            CHECK(binding.BindPoint == bindPoint);
            CHECK(binding.BindCount == 1u);
        }
    };
    checkCullBinding("inputCommands", D3D_SIT_STRUCTURED, 0u);
    checkCullBinding("previousDepth", D3D_SIT_TEXTURE, 1u);
    checkCullBinding("visibleCommands", D3D_SIT_UAV_RWBYTEADDRESS, 0u);
    checkCullBinding("visibleCount", D3D_SIT_UAV_RWBYTEADDRESS, 1u);

    // Primary visibility shaders are also runtime FXC inputs. Decals are
    // bindless-only, so the legacy entries must compile without reading the
    // inert b10 compatibility slot shared by both root layouts.
    auto readShader = [](const std::string& path, std::string& source) {
        std::ifstream file(path);
        if (!file.is_open()) return false;
        std::stringstream stream;
        stream << file.rdbuf();
        source = stream.str();
        return true;
    };
    std::string visVertexCode;
    std::string visPixelCode;
    CHECK(readShader(visVertexShaderPath, visVertexCode));
    CHECK(readShader(visPixelShaderPath, visPixelCode));

    ComPtr<ID3DBlob> visVertexBlob;
    errors.Reset();
    const HRESULT visVertexHr = D3DCompile(
        visVertexCode.c_str(), visVertexCode.size(), visVertexShaderPath.c_str(),
        nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "vs_5_0",
        compileFlags, 0, &visVertexBlob, &errors);
    if (FAILED(visVertexHr)) {
        std::cerr << "Visibility vertex shader failed to compile: "
                  << (errors ? (const char*)errors->GetBufferPointer()
                             : "unknown") << "\n";
        return 1;
    }

    for (const char* entry : { "main", "mainAlpha" }) {
        ComPtr<ID3DBlob> visPixelBlob;
        errors.Reset();
        const HRESULT visPixelHr = D3DCompile(
            visPixelCode.c_str(), visPixelCode.size(),
            visPixelShaderPath.c_str(), nullptr,
            D3D_COMPILE_STANDARD_FILE_INCLUDE, entry, "ps_5_0", compileFlags,
            0, &visPixelBlob, &errors);
        if (FAILED(visPixelHr)) {
            std::cerr << "Visibility pixel shader " << entry
                      << " failed to compile: "
                      << (errors ? (const char*)errors->GetBufferPointer()
                                 : "unknown") << "\n";
            return 1;
        }

        ComPtr<ID3D11ShaderReflection> visPixelReflection;
        CHECK(SUCCEEDED(D3DReflect(
            visPixelBlob->GetBufferPointer(), visPixelBlob->GetBufferSize(),
            __uuidof(ID3D11ShaderReflection),
            reinterpret_cast<void**>(visPixelReflection.GetAddressOf()))));
        if (visPixelReflection) {
            D3D11_SHADER_INPUT_BIND_DESC decalsBinding = {};
            CHECK(FAILED(visPixelReflection->GetResourceBindingDescByName(
                "ImpactDecalsBuffer", &decalsBinding)));
        }
    }

    // Grass is runtime FXC too. Its interaction constants are root constants,
    // so both shaders can compile while silently disagreeing with the C++ count;
    // reflect the shared b6 contract and its instance SRV in both variants.
    for (const std::string& grassPath : {
             grassVertexShaderPath, grassShadowShaderPath }) {
        std::string grassCode;
        CHECK(readShader(grassPath, grassCode));
        if (grassCode.empty()) continue;

        ComPtr<ID3DBlob> grassBlob;
        errors.Reset();
        const HRESULT grassHr = D3DCompile(
            grassCode.c_str(), grassCode.size(), grassPath.c_str(), nullptr,
            D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "vs_5_0",
            compileFlags, 0, &grassBlob, &errors);
        if (FAILED(grassHr)) {
            std::cerr << "Grass vertex shader failed to compile: "
                      << (errors ? (const char*)errors->GetBufferPointer()
                                 : "unknown") << "\n";
            ++failures;
            continue;
        }

        ComPtr<ID3D11ShaderReflection> grassReflection;
        CHECK(SUCCEEDED(D3DReflect(
            grassBlob->GetBufferPointer(), grassBlob->GetBufferSize(),
            __uuidof(ID3D11ShaderReflection),
            reinterpret_cast<void**>(grassReflection.GetAddressOf()))));
        if (!grassReflection) continue;

        D3D11_SHADER_INPUT_BIND_DESC grassParamsBinding = {};
        CHECK(SUCCEEDED(grassReflection->GetResourceBindingDescByName(
            "GrassParams", &grassParamsBinding)));
        CHECK(grassParamsBinding.Type == D3D_SIT_CBUFFER);
        CHECK(grassParamsBinding.BindPoint == 6u);

        D3D11_SHADER_INPUT_BIND_DESC bladesBinding = {};
        CHECK(SUCCEEDED(grassReflection->GetResourceBindingDescByName(
            "blades", &bladesBinding)));
        CHECK(bladesBinding.Type == D3D_SIT_STRUCTURED);
        CHECK(bladesBinding.BindPoint == 6u);

        ID3D11ShaderReflectionConstantBuffer* grassParams =
            grassReflection->GetConstantBufferByName("GrassParams");
        CHECK(grassParams != nullptr);
        if (grassParams) {
            D3D11_SHADER_BUFFER_DESC grassParamsDesc = {};
            CHECK(SUCCEEDED(grassParams->GetDesc(&grassParamsDesc)));
            // Nineteen DWORDs occupy five 16-byte HLSL registers.
            CHECK(grassParamsDesc.Size == 80u);
        }
    }

    // These are edited together when clustered/volumetric lights change, but
    // most are runtime-only FXC inputs. DXC accepting the offline SM6 variants
    // does not guarantee that the startup SM5 shaders still compile.
    struct RuntimeShaderCase {
        const std::string* path;
        const char* entry;
        const char* target;
        const D3D_SHADER_MACRO* defines;
    };
    const D3D_SHADER_MACRO hdrDefines[] = {
        { "SGE_HDR_TARGET", "1" }, { nullptr, nullptr }
    };
    const D3D_SHADER_MACRO motionDefines[] = {
        { "SGE_HDR_TARGET", "1" },
        { "SGE_EXTENSION_MOTION", "1" },
        { nullptr, nullptr }
    };
    const D3D_SHADER_MACRO cloudDefines[] = {
        { "SGE_WORLD_CLOUDS", "1" }, { nullptr, nullptr }
    };
    const RuntimeShaderCase runtimeShaders[] = {
        { &clusteredVertexShaderPath, "main", "vs_5_0", nullptr },
        { &fallbackVertexShaderPath, "main", "vs_5_0", nullptr },
        { &clusteredPixelShaderPath, "main", "ps_5_0", nullptr },
        { &clusteredPixelShaderPath, "main", "ps_5_0", hdrDefines },
        { &clusteredPixelShaderPath, "main", "ps_5_0", motionDefines },
        { &fallbackPixelShaderPath, "main", "ps_5_0", nullptr },
        { &fallbackPixelShaderPath, "main", "ps_5_0", hdrDefines },
        { &fallbackPixelShaderPath, "main", "ps_5_0", motionDefines },
        { &simplePixelShaderPath, "main", "ps_5_0", nullptr },
        { &ddgiShaderPath, "CSMain", "cs_5_0", nullptr },
        { &volumetricFogShaderPath, "CSMain", "cs_5_0", nullptr },
        { &volumetricFogShaderPath, "CSMain", "cs_5_0", cloudDefines },
        { &volumetricFogShaderPath, "VSMain", "vs_5_0", nullptr },
        { &volumetricFogShaderPath, "PSMain", "ps_5_0", nullptr },
        { &volumetricFogShaderPath, "PSMain", "ps_5_0", cloudDefines },
        { &volumetricFogShaderPath, "PSMainMSAA", "ps_5_0", nullptr },
        { &volumetricFogShaderPath, "PSMainMSAA", "ps_5_0", cloudDefines },
    };
    for (const RuntimeShaderCase& shader : runtimeShaders) {
        std::string source;
        CHECK(readShader(*shader.path, source));
        if (source.empty()) continue;

        ComPtr<ID3DBlob> runtimeBlob;
        errors.Reset();
        const HRESULT runtimeHr = D3DCompile(
            source.c_str(), source.size(), shader.path->c_str(), shader.defines,
            D3D_COMPILE_STANDARD_FILE_INCLUDE, shader.entry, shader.target,
            compileFlags, 0, &runtimeBlob, &errors);
        if (FAILED(runtimeHr)) {
            std::cerr << *shader.path << " (" << shader.entry << "/"
                      << shader.target << ") failed to compile:\n"
                      << (errors ? (const char*)errors->GetBufferPointer()
                                 : "unknown") << "\n";
            ++failures;
        } else if (shader.defines == nullptr &&
                   (shader.path == &clusteredPixelShaderPath ||
                    shader.path == &fallbackPixelShaderPath)) {
            ComPtr<ID3D11ShaderReflection> runtimeReflection;
            CHECK(SUCCEEDED(D3DReflect(
                runtimeBlob->GetBufferPointer(), runtimeBlob->GetBufferSize(),
                __uuidof(ID3D11ShaderReflection),
                reinterpret_cast<void**>(runtimeReflection.GetAddressOf()))));
            if (runtimeReflection) {
                D3D11_SHADER_INPUT_BIND_DESC spotAtlasBinding = {};
                CHECK(SUCCEEDED(
                    runtimeReflection->GetResourceBindingDescByName(
                        "spotShadowAtlas", &spotAtlasBinding)));
                CHECK(spotAtlasBinding.Type == D3D_SIT_TEXTURE);
                CHECK(spotAtlasBinding.BindPoint == 21u);
            }
        }
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
