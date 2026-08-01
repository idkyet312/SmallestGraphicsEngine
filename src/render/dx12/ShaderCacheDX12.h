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

} // namespace ShaderCacheDX12

#endif // SHADER_CACHE_DX12_H
