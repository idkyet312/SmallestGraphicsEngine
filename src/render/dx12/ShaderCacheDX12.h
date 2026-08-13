#ifndef SHADER_CACHE_DX12_H
#define SHADER_CACHE_DX12_H

// Disk cache for runtime-compiled HLSL.
//
// Every post-FX renderer compiles its shaders with D3DCompile at startup, which
// is the bulk of the pre-menu boot time (screen-space AO alone issues 7 compiles
// and costs ~6 s). None of that output changes between runs unless the shader
// source does, so the compiled blob is stored on disk and reused.
//
// The mesh/terrain/water shaders take a different route: CMake precompiles them
// with dxc into .cso files (see the add_custom_command block in CMakeLists.txt)
// and they load through ReadCompiledShaderDX12(). That works well there because
// those are a fixed set of shader-model-6 targets. The renderers here compile
// shader model 5.x through FXC with per-call entry points, flags and defines, so
// caching at runtime covers them all uniformly instead of needing one
// hand-maintained CMake rule per permutation.
//
// Invalidation is by content hash, not timestamp: the key mixes the source text
// with everything else that changes the generated code. Editing a shader,
// flipping a define or changing optimisation level misses the cache and
// recompiles; an unchanged shader always hits.

#include <d3dcompiler.h>
#include <windows.h>
// Ships in the Windows SDK (10.0.19041 and later). Needed for the SM6 path:
// FXC cannot compile inline raytracing at any profile.
#include <dxcapi.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace ShaderCacheDX12 {

// FNV-1a. Not cryptographic -- this only needs to separate shader variants, and
// a collision would have to match on the entire source text plus every compile
// parameter to matter.
inline uint64_t HashBytes(const void* data, size_t size, uint64_t seed) {
    const unsigned char* bytes = static_cast<const unsigned char*>(data);
    uint64_t hash = seed;
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

inline uint64_t HashString(const char* text, uint64_t seed) {
    if (!text) return HashBytes("", 0, seed);
    return HashBytes(text, std::strlen(text), seed);
}

// Directory holding the executable, used to locate the shaders/ tree.
inline std::wstring ExecutableDirectory() {
    wchar_t modulePath[MAX_PATH] = {};
    const DWORD length = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
        const std::wstring executablePath(modulePath, length);
        const size_t slash = executablePath.find_last_of(L"\\/");
        if (slash != std::wstring::npos)
            return executablePath.substr(0, slash + 1);
    }
    return L"";
}

// Combined hash of every shader include file, computed once per process.
//
// This is a correctness requirement, not an optimisation. Eight shaders pull in
// these headers via D3D_COMPILE_STANDARD_FILE_INCLUDE, which resolves them from
// disk during compilation -- their text never appears in the source string the
// caller hands us, so without this an edit to terrain_pbr.hlsli would keep
// serving a stale blob compiled against the previous version. It matters all the
// more because the cache lives outside the build tree and survives a clean
// rebuild.
//
// Any include added later must be listed here.
inline uint64_t IncludeHash() {
    static const uint64_t hash = [] {
        static const wchar_t* kIncludes[] = {
            L"shaders/agx_tonemap.hlsli",
            L"shaders/color_grade.hlsli",
            L"shaders/foliage_brdf.hlsli",
            L"shaders/palm_wind.hlsli",
            L"shaders/terrain_pbr.hlsli",
        };
        const std::wstring base = ExecutableDirectory();
        uint64_t combined = 1469598103934665603ull;
        for (const wchar_t* relative : kIncludes) {
            std::ifstream file(base + relative, std::ios::binary);
            if (!file) {
                // Missing include: fold in the name alone. The compile will fail
                // (or the shader does not need it), and a name-only hash keeps
                // the key stable rather than aliasing onto the present case.
                combined = HashBytes(relative,
                    wcslen(relative) * sizeof(wchar_t), combined);
                continue;
            }
            std::stringstream contents;
            contents << file.rdbuf();
            const std::string text = contents.str();
            combined = HashBytes(text.data(), text.size(), combined);
        }
        return combined;
    }();
    return hash;
}

// Where cached blobs live. %LOCALAPPDATA% keeps them across clean rebuilds of
// build/, so wiping the build directory does not cost a slow boot. Falls back to
// a directory beside the executable when LOCALAPPDATA is unset.
inline const std::wstring& CacheDirectory() {
    static const std::wstring directory = [] {
        std::wstring root;
        if (const wchar_t* localAppData = _wgetenv(L"LOCALAPPDATA")) {
            root = localAppData;
            if (!root.empty() && root.back() != L'\\' && root.back() != L'/')
                root += L'\\';
            root += L"SmallestGraphicsEngine";
            CreateDirectoryW(root.c_str(), nullptr);
            root += L"\\shadercache";
        } else {
            root = ExecutableDirectory() + L"shadercache";
        }
        CreateDirectoryW(root.c_str(), nullptr);
        return root;
    }();
    return directory;
}

inline std::wstring CachePath(uint64_t key) {
    wchar_t name[40] = {};
    std::swprintf(name, 40, L"\\%016llx.dxil",
                  static_cast<unsigned long long>(key));
    return CacheDirectory() + name;
}

// Compile `source`, reusing a cached blob when one matches the exact inputs.
//
// Drop-in for a D3DCompile call: same arguments, same out-params, same HRESULT
// contract. On a cache hit no compiler runs and `errors` is left untouched.
//
// Every failure mode degrades to a plain compile: a missing, truncated or
// corrupt entry fails D3DReadFileToBlob and falls through. Only successful
// compiles are written back, and a failed write is ignored -- the cache is an
// optimisation, never authoritative state.
inline HRESULT CompileCached(const void* source, size_t sourceSize,
                             const char* sourceName,
                             const D3D_SHADER_MACRO* defines,
                             ID3DInclude* include, const char* entry,
                             const char* target, UINT flags1, UINT flags2,
                             ID3DBlob** blob, ID3DBlob** errors) {
    if (!blob) return E_INVALIDARG;

    uint64_t key = HashBytes(source, sourceSize, IncludeHash());
    key = HashString(entry, key);
    key = HashString(target, key);
    key = HashBytes(&flags1, sizeof(flags1), key);
    key = HashBytes(&flags2, sizeof(flags2), key);
    for (const D3D_SHADER_MACRO* macro = defines; macro && macro->Name; ++macro) {
        key = HashString(macro->Name, key);
        key = HashString(macro->Definition, key);
    }

    const std::wstring path = CachePath(key);
    if (SUCCEEDED(D3DReadFileToBlob(path.c_str(), blob)) && *blob)
        return S_OK;

    const HRESULT hr = D3DCompile(source, sourceSize, sourceName, defines,
                                  include, entry, target, flags1, flags2,
                                  blob, errors);
    if (SUCCEEDED(hr) && *blob)
        D3DWriteBlobToFile(*blob, path.c_str(), TRUE);
    return hr;
}

// ---------------------------------------------------------------------------
// DXC (shader model 6) compilation.
//
// The renderers above target shader model 5.x through FXC (D3DCompile), which
// cannot compile inline raytracing at all -- RayQuery needs SM 6.5. Rather than
// migrate every runtime-compiled shader to DXC, this compiles the specific
// shaders that need SM6 while leaving the FXC path untouched for everything
// else. That keeps the default frame on exactly the compiler it has always
// used, which matters because the resolve shader runs for every pixel.
//
// dxcompiler.dll is loaded lazily and only when an SM6 shader is actually
// requested, so a machine without it still boots and simply reports the
// enhanced tier as unavailable.
// ---------------------------------------------------------------------------

// Returns true when dxcompiler.dll loaded and exposes DxcCreateInstance.
inline bool DxcAvailable() {
    static const bool available = [] {
        const HMODULE module = LoadLibraryW(L"dxcompiler.dll");
        return module != nullptr &&
               GetProcAddress(module, "DxcCreateInstance") != nullptr;
    }();
    return available;
}

// Compile `source` with DXC at an SM6 profile (e.g. "cs_6_5"), caching the DXIL
// on disk under the same scheme as CompileCached.
//
// `errorText` receives the compiler diagnostics on failure. Returns false when
// DXC is unavailable, which callers treat as "this optional feature is off"
// rather than a fatal error.
inline bool CompileCachedDXC(const std::string& source,
                             const wchar_t* sourceName,
                             const wchar_t* entry, const wchar_t* target,
                             const std::wstring& includeDirectory,
                             ID3DBlob** blob, std::string* errorText) {
    if (!blob) return false;
    *blob = nullptr;

    // Key on the same inputs FXC uses, plus a salt so an SM6 blob can never be
    // confused with an FXC blob compiled from identical text.
    uint64_t key = HashBytes(source.data(), source.size(), IncludeHash());
    key = HashBytes(entry, wcslen(entry) * sizeof(wchar_t), key);
    key = HashBytes(target, wcslen(target) * sizeof(wchar_t), key);
    key = HashString("dxc-sm6", key);

    const std::wstring path = CachePath(key);
    if (SUCCEEDED(D3DReadFileToBlob(path.c_str(), blob)) && *blob)
        return true;

    if (!DxcAvailable()) {
        if (errorText)
            *errorText = "dxcompiler.dll not available; SM6 shaders disabled";
        return false;
    }

    const HMODULE module = LoadLibraryW(L"dxcompiler.dll");
    if (!module) return false;
    const auto createInstance = reinterpret_cast<DxcCreateInstanceProc>(
        GetProcAddress(module, "DxcCreateInstance"));
    if (!createInstance) return false;

    Microsoft::WRL::ComPtr<IDxcCompiler> compiler;
    Microsoft::WRL::ComPtr<IDxcLibrary> library;
    if (FAILED(createInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler))) ||
        FAILED(createInstance(CLSID_DxcLibrary, IID_PPV_ARGS(&library))))
        return false;

    Microsoft::WRL::ComPtr<IDxcBlobEncoding> sourceBlob;
    // 65001 = UTF-8.
    if (FAILED(library->CreateBlobWithEncodingOnHeapCopy(
            source.data(), static_cast<UINT32>(source.size()), 65001,
            &sourceBlob)))
        return false;

    // Resolves #include from the shaders/ directory, matching how the FXC path
    // uses D3D_COMPILE_STANDARD_FILE_INCLUDE.
    Microsoft::WRL::ComPtr<IDxcIncludeHandler> includeHandler;
    library->CreateIncludeHandler(&includeHandler);

    const std::wstring includeArg = L"-I" + includeDirectory;
    const wchar_t* arguments[] = { L"-O3", includeArg.c_str() };

    Microsoft::WRL::ComPtr<IDxcOperationResult> operation;
    const HRESULT hr = compiler->Compile(
        sourceBlob.Get(), sourceName, entry, target,
        arguments, _countof(arguments), nullptr, 0,
        includeHandler.Get(), &operation);
    if (FAILED(hr) || !operation) return false;

    HRESULT status = E_FAIL;
    operation->GetStatus(&status);
    if (FAILED(status)) {
        if (errorText) {
            Microsoft::WRL::ComPtr<IDxcBlobEncoding> errorBlob;
            if (SUCCEEDED(operation->GetErrorBuffer(&errorBlob)) && errorBlob) {
                errorText->assign(
                    static_cast<const char*>(errorBlob->GetBufferPointer()),
                    errorBlob->GetBufferSize());
            }
        }
        return false;
    }

    Microsoft::WRL::ComPtr<IDxcBlob> dxil;
    if (FAILED(operation->GetResult(&dxil)) || !dxil) return false;
    // Copy into an ID3DBlob so callers stay on a single blob type.
    if (FAILED(D3DCreateBlob(dxil->GetBufferSize(), blob)) || !*blob)
        return false;
    memcpy((*blob)->GetBufferPointer(), dxil->GetBufferPointer(),
           dxil->GetBufferSize());
    D3DWriteBlobToFile(*blob, path.c_str(), TRUE);
    return true;
}

} // namespace ShaderCacheDX12

#endif // SHADER_CACHE_DX12_H
