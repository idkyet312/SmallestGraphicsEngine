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
#include "BootTimer.h"
#include <atomic>

#include <climits>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

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
            L"shaders/virtual_shadow_types.hlsli",
            L"shaders/virtual_shadow_sample.hlsli",
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

// Boot diagnostics: every compile request reports HIT (blob read from the
// cache) or COMPILE (compiler ran) with its time and the variant defines, and
// keeps running totals. A cold cache after a shader edit is what makes the
// first launch slow; these lines show which variants pay for it.
struct CompileStats {
    std::atomic<unsigned> hits{0}, compiles{0}, failures{0};
    std::atomic<long long> compileMicroseconds{0};
};
inline CompileStats& Stats() { static CompileStats* stats = new CompileStats; return *stats; }

// "visbuf_resolve_cs.hlsl [SGE_TERRAIN_VISIBILITY SGE_ENHANCED_VISUALS] cs_6_5"
inline std::string DescribeShader(const char* sourceName, const char* source,
                                  size_t sourceSize,
                                  const D3D_SHADER_MACRO* defines,
                                  const char* target) {
    std::string label = sourceName ? sourceName : "?";
    std::string variants;
    // Variants are built by prepending "#define NAME 1" lines to the source.
    for (size_t at = 0; source && at + 8 < sourceSize &&
                        std::strncmp(source + at, "#define ", 8) == 0;) {
        size_t nameEnd = at + 8;
        while (nameEnd < sourceSize && source[nameEnd] != ' ' &&
               source[nameEnd] != '\n' && source[nameEnd] != '\r')
            ++nameEnd;
        variants += (variants.empty() ? "" : " ") +
                    std::string(source + at + 8, nameEnd - at - 8);
        const char* lineEnd = static_cast<const char*>(
            std::memchr(source + at, '\n', sourceSize - at));
        if (!lineEnd) break;
        at = static_cast<size_t>(lineEnd - source) + 1;
    }
    for (const D3D_SHADER_MACRO* macro = defines; macro && macro->Name; ++macro)
        variants += (variants.empty() ? "" : " ") + std::string(macro->Name);
    if (!variants.empty()) label += " [" + variants + "]";
    if (target) label += std::string(" ") + target;
    return label;
}

// `tag` overrides the HIT/COMPILE/FAILED column, e.g. for a request that
// waited on a worker's compile.
inline void ReportCompile(const std::string& label, bool hit, bool ok,
                          double milliseconds, const char* tag = nullptr) {
    CompileStats& stats = Stats();
    if (hit) ++stats.hits;
    else if (ok) ++stats.compiles;
    else ++stats.failures;
    if (!hit)
        stats.compileMicroseconds += static_cast<long long>(milliseconds * 1000.0);
    // Cache hits are near-free; only log the slow or notable ones.
    if (hit && milliseconds < 50.0) return;
    if (!tag)
        tag = hit ? "shader HIT    " : ok ? "shader COMPILE" : "shader FAILED ";
    char text[160];
    std::snprintf(text, sizeof(text),
                  "%s %7.0f ms  (hits %u, compiled %u, compile total %.1f s)  ",
                  tag, milliseconds, stats.hits.load(), stats.compiles.load(),
                  stats.compileMicroseconds.load() / 1.0e6);
    BootTimer::Log(text + label);
}

inline std::string Narrow(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(),
        static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(size > 0 ? size : 0), '\0');
    if (size > 0)
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                            &out[0], size, nullptr, nullptr);
    return out;
}

inline std::wstring Widen(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(),
        static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(size > 0 ? size : 0), L'\0');
    if (size > 0)
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                            &out[0], size);
    return out;
}

// Cache keys. Shared by the request path and the prewarm so both agree on
// which blob a given source maps to.
inline uint64_t KeyFXC(const void* source, size_t sourceSize, const char* entry,
                       const char* target, UINT flags1, UINT flags2,
                       const D3D_SHADER_MACRO* defines) {
    uint64_t key = HashBytes(source, sourceSize, IncludeHash());
    key = HashString(entry, key);
    key = HashString(target, key);
    key = HashBytes(&flags1, sizeof(flags1), key);
    key = HashBytes(&flags2, sizeof(flags2), key);
    for (const D3D_SHADER_MACRO* macro = defines; macro && macro->Name; ++macro) {
        key = HashString(macro->Name, key);
        key = HashString(macro->Definition, key);
    }
    return key;
}

// Same inputs as FXC, plus a salt so an SM6 blob can never be confused with an
// FXC blob compiled from identical text.
inline uint64_t KeyDXC(const std::string& source, const wchar_t* entry,
                       const wchar_t* target) {
    uint64_t key = HashBytes(source.data(), source.size(), IncludeHash());
    key = HashBytes(entry, wcslen(entry) * sizeof(wchar_t), key);
    key = HashBytes(target, wcslen(target) * sizeof(wchar_t), key);
    return HashString("dxc-sm6", key);
}

// ---------------------------------------------------------------------------
// Shader compile workers.
//
// Measured cold boot (2026-10-02, Ryzen 5 5500, 6C/12T): 142 s of a 150 s boot
// was the visibility-buffer resolve permutations -- 21 compiles of one 176 KB
// shader, mostly back to back on the main thread. FXC and DXC are both
// free-threaded: three FXC resolve variants took 27.7 s serially, 15.4 s on
// three threads and 15.6 s as three separate processes. In-process threads
// give the same throughput as Unreal-style ShaderCompileWorker processes
// without the IPC.
//
// Two pieces:
//  - A pool of worker threads that runs compiles, critical and longest first.
//  - A manifest of every compile request seen in recent runs, stored as a
//    recipe: the source file, the "#define" prefix the caller prepended, entry,
//    profile and flags. At boot -- before the D3D12 device exists -- every
//    recipe is rebuilt against the current shader files, and each whose blob
//    is missing from the cache is queued on the workers. When the renderer
//    later asks for that shader it either hits the cache or waits for the
//    worker already compiling it; the same key never compiles twice.
//
// The manifest is advisory. A recipe whose rebuilt source no longer matches
// what the renderer asks for produces a key nobody requests, and the request
// compiles inline exactly as it did before workers existed. Correctness always
// rests on the content hash of the source the caller actually passed.
// ---------------------------------------------------------------------------

namespace detail {

// One compile, possibly queued on a worker. Whoever flips `claimed` first runs
// it; everybody else waits on `done`.
struct CompileJob {
    std::atomic<bool> claimed{false};
    // BootTimer milliseconds when a thread started compiling; 0 while queued.
    std::atomic<double> startMs{0.0};
    std::promise<void> promise;
    std::shared_future<void> done{promise.get_future().share()};
};

// Boot-critical compiles the boot screen waits on, with the estimate its
// progress bar is weighted by.
struct TrackedCompile {
    std::shared_ptr<CompileJob> job;
    double estimateMs;
};
struct BootTracker {
    std::mutex mutex;
    std::vector<TrackedCompile> jobs;
    std::shared_future<void> planned;  // prewarm planning finished
};
inline BootTracker& Tracker() {
    static BootTracker* tracker = new BootTracker;
    return *tracker;
}
inline void Track(const std::shared_ptr<CompileJob>& job, double estimateMs) {
    std::lock_guard<std::mutex> lock(Tracker().mutex);
    for (const TrackedCompile& tracked : Tracker().jobs)
        if (tracked.job == job) return;
    Tracker().jobs.push_back({ job, estimateMs });
}

// Without a recorded time: roughly a resolve permutation under full load.
constexpr double kDefaultCompileEstimateMs = 20000.0;

struct InFlightTable {
    std::mutex mutex;
    std::unordered_map<uint64_t, std::shared_ptr<CompileJob>> jobs;
};
// Leaked on purpose, like every object here a worker can touch: a worker may
// still be inside the compiler while static destructors run at exit.
inline InFlightTable& InFlight() {
    static InFlightTable* table = new InFlightTable;
    return *table;
}

inline void FinishJob(uint64_t key, const std::shared_ptr<CompileJob>& job) {
    {
        std::lock_guard<std::mutex> lock(InFlight().mutex);
        auto it = InFlight().jobs.find(key);
        if (it != InFlight().jobs.end() && it->second == job)
            InFlight().jobs.erase(it);
    }
    job->promise.set_value();
}

// Set on worker threads. A request made from a worker is background work, so
// it never marks its recipe boot-critical.
inline bool& IsWorkerThread() {
    static thread_local bool worker = false;
    return worker;
}

} // namespace detail

// Claims the compile of `key` for the calling thread and returns the job to
// hand to detail::FinishJob once the blob is written. Returns null when another
// thread was already compiling it: that compile has finished by the time this
// returns, so the caller re-reads the cache (a miss then means it failed).
inline std::shared_ptr<detail::CompileJob> ClaimCompile(uint64_t key) {
    std::shared_ptr<detail::CompileJob> job;
    {
        std::lock_guard<std::mutex> lock(detail::InFlight().mutex);
        std::shared_ptr<detail::CompileJob>& slot = detail::InFlight().jobs[key];
        if (!slot) {
            slot = std::make_shared<detail::CompileJob>();
            slot->claimed = true;
            slot->startMs = BootTimer::MillisecondsNow();
            return slot;
        }
        job = slot;
    }
    // Queued on a worker but not started yet: run it here rather than wait
    // behind whatever the workers are busy with.
    if (!job->claimed.exchange(true)) {
        job->startMs = BootTimer::MillisecondsNow();
        return job;
    }
    job->done.wait();
    return nullptr;
}

class CompileWorkers {
public:
    static CompileWorkers& Get() {
        static CompileWorkers* workers = new CompileWorkers;
        return *workers;
    }

    // Higher `priority` runs first; within a priority the longest estimate
    // runs first, so the slowest permutation starts at once instead of
    // becoming the tail everyone waits on.
    void Submit(int priority, double estimateMs, std::function<void()> run) {
        Start();
        {
            std::lock_guard<std::mutex> lock(mutex);
            queue.push(Job{ priority, estimateMs, nextSequence++, std::move(run) });
            ++pending;
        }
        wake.notify_one();
    }

    unsigned ThreadCount() { Start(); return threadCount; }

private:
    struct Job {
        int priority;
        double estimateMs;
        uint64_t sequence;
        std::function<void()> run;
    };
    struct Order {
        bool operator()(const Job& a, const Job& b) const {
            if (a.priority != b.priority) return a.priority < b.priority;
            if (a.estimateMs != b.estimateMs) return a.estimateMs < b.estimateMs;
            return a.sequence > b.sequence;
        }
    };

    // One thread is left for the main thread. SGE_SHADER_WORKERS overrides.
    void Start() {
        std::call_once(started, [this] {
            unsigned count = std::thread::hardware_concurrency();
            count = count > 1 ? count - 1 : 1;
            char text[16] = {};
            if (GetEnvironmentVariableA("SGE_SHADER_WORKERS", text,
                                        sizeof(text)) > 0) {
                const int forced = std::atoi(text);
                if (forced > 0) count = static_cast<unsigned>(forced);
            }
            threadCount = count;
            for (unsigned i = 0; i < count; ++i)
                std::thread([this] { Run(); }).detach();
        });
    }

    void Run() {
        detail::IsWorkerThread() = true;
        // Below the main thread, so device creation and asset loading keep
        // their core while the workers soak up the rest.
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        SetThreadDescription(GetCurrentThread(), L"ShaderCompileWorker");
        for (;;) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(mutex);
                wake.wait(lock, [this] { return !queue.empty(); });
                job = queue.top();
                queue.pop();
            }
            job.run();
            std::lock_guard<std::mutex> lock(mutex);
            --pending;
        }
    }

    std::once_flag started;
    std::mutex mutex;
    std::condition_variable wake;
    std::priority_queue<Job, std::vector<Job>, Order> queue;
    uint64_t nextSequence = 0;
    unsigned pending = 0;
    unsigned threadCount = 0;
};

// How to rebuild one compile request from the files on disk.
struct Recipe {
    bool dxc = false;
    // The caller read the file in text mode (CRLF folded) rather than binary.
    bool textMode = true;
    bool standardInclude = false;
    std::string file;          // path the file is read from
    std::string sourceName;    // name handed to the compiler
    std::string includeDirectory;  // DXC -I
    std::string prefix;        // text the caller put before the file contents
    std::string entry, target;
    unsigned flags1 = 0, flags2 = 0;
    std::vector<std::pair<std::string, std::string>> defines;
    // Bookkeeping, not part of the identity.
    unsigned lastRun = 0;
    double lastMs = 0.0;
    bool critical = false;

    uint64_t Identity() const {
        uint64_t id = 1469598103934665603ull;
        const auto mix = [&id](const std::string& text) {
            id = HashBytes(text.data(), text.size(), id);
            id = HashBytes("\x1f", 1, id);
        };
        mix(dxc ? "dxc" : "fxc");
        mix(textMode ? "t" : "b");
        mix(standardInclude ? "i" : "-");
        mix(file); mix(sourceName); mix(includeDirectory); mix(prefix);
        mix(entry); mix(target);
        mix(std::to_string(flags1)); mix(std::to_string(flags2));
        for (const auto& define : defines) { mix(define.first); mix(define.second); }
        return id;
    }
};

namespace detail {

inline bool ReadWholeFile(const std::string& path, bool textMode,
                          std::string& out) {
    std::ifstream file(path, textMode ? std::ios::in
                                      : std::ios::in | std::ios::binary);
    if (!file) return false;
    std::stringstream contents;
    contents << file.rdbuf();
    out = contents.str();
    return true;
}

// Fills recipe.prefix/textMode when `source` is the file's contents with text
// prepended. Anything else (embedded source, edited text) is not recordable.
inline bool MatchFile(Recipe& recipe, const char* source, size_t sourceSize) {
    std::string text;
    for (const bool textMode : { true, false }) {
        if (!ReadWholeFile(recipe.file, textMode, text) || text.empty() ||
            text.size() > sourceSize ||
            std::memcmp(source + sourceSize - text.size(), text.data(),
                        text.size()) != 0)
            continue;
        recipe.textMode = textMode;
        recipe.prefix.assign(source, sourceSize - text.size());
        return true;
    }
    return false;
}

struct Manifest {
    std::mutex mutex;
    bool loaded = false;
    bool dirty = false;
    unsigned run = 0;
    std::unordered_map<uint64_t, Recipe> recipes;        // by Identity()
    std::unordered_map<uint64_t, uint64_t> identityByKey;  // cache key -> recipe
};
inline Manifest& TheManifest() {
    static Manifest* manifest = new Manifest;
    return *manifest;
}
inline std::atomic<bool>& BootPhase() {
    static std::atomic<bool>* boot = new std::atomic<bool>(true);
    return *boot;
}
inline std::wstring ManifestPath() { return CacheDirectory() + L"\\manifest.v1"; }

// Recipes not requested for this many runs are dropped, so a removed
// permutation stops costing a worker after a shader edit.
constexpr unsigned kManifestKeepRuns = 8;

// Length-prefixed fields ("5:hello"), one recipe per line: the prefixes hold
// newlines, so nothing delimiter-based would survive them.
inline void PutField(std::string& out, const std::string& field) {
    out += std::to_string(field.size());
    out += ':';
    out += field;
}
inline bool GetField(const std::string& in, size_t& at, std::string& field) {
    size_t colon = in.find(':', at);
    if (colon == std::string::npos) return false;
    const unsigned long long length =
        std::strtoull(in.c_str() + at, nullptr, 10);
    if (colon + 1 + length > in.size()) return false;
    field.assign(in, colon + 1, static_cast<size_t>(length));
    at = colon + 1 + static_cast<size_t>(length);
    return true;
}

// Caller holds the manifest mutex.
inline void LoadManifestLocked(Manifest& manifest) {
    if (manifest.loaded) return;
    manifest.loaded = true;
    std::ifstream file(ManifestPath(), std::ios::binary);
    std::string data;
    if (file) {
        std::stringstream contents;
        contents << file.rdbuf();
        data = contents.str();
    }
    size_t at = 0;
    std::string field;
    if (GetField(data, at, field) && field == "SGEMANIFEST1" &&
        GetField(data, at, field)) {
        manifest.run = static_cast<unsigned>(std::strtoul(field.c_str(), nullptr, 10));
        while (at < data.size() && data[at] == '\n') ++at;
        while (at < data.size()) {
            Recipe recipe;
            std::string kind, mode, include, flags1, flags2, defineCount,
                lastRun, lastMs, critical;
            bool ok = GetField(data, at, kind) && GetField(data, at, mode) &&
                GetField(data, at, include) && GetField(data, at, recipe.file) &&
                GetField(data, at, recipe.sourceName) &&
                GetField(data, at, recipe.includeDirectory) &&
                GetField(data, at, recipe.prefix) &&
                GetField(data, at, recipe.entry) &&
                GetField(data, at, recipe.target) &&
                GetField(data, at, flags1) && GetField(data, at, flags2) &&
                GetField(data, at, defineCount);
            const unsigned long count =
                ok ? std::strtoul(defineCount.c_str(), nullptr, 10) : 0;
            for (unsigned long i = 0; ok && i < count; ++i) {
                std::pair<std::string, std::string> define;
                ok = GetField(data, at, define.first) &&
                     GetField(data, at, define.second);
                recipe.defines.push_back(std::move(define));
            }
            ok = ok && GetField(data, at, lastRun) && GetField(data, at, lastMs) &&
                 GetField(data, at, critical);
            if (!ok) break;  // truncated or foreign: keep what parsed
            while (at < data.size() && data[at] == '\n') ++at;
            recipe.dxc = kind == "dxc";
            recipe.textMode = mode == "t";
            recipe.standardInclude = include == "i";
            recipe.flags1 = static_cast<unsigned>(std::strtoul(flags1.c_str(), nullptr, 10));
            recipe.flags2 = static_cast<unsigned>(std::strtoul(flags2.c_str(), nullptr, 10));
            recipe.lastRun = static_cast<unsigned>(std::strtoul(lastRun.c_str(), nullptr, 10));
            recipe.lastMs = std::atof(lastMs.c_str());
            recipe.critical = critical == "1";
            manifest.recipes.emplace(recipe.Identity(), std::move(recipe));
        }
    }
    ++manifest.run;
}

// Caller holds the manifest mutex. Writes beside the target and swaps it in,
// so a second running instance (multiplayer tests) never reads a torn file.
inline void SaveManifestLocked(Manifest& manifest) {
    std::string out;
    PutField(out, "SGEMANIFEST1");
    PutField(out, std::to_string(manifest.run));
    out += '\n';
    for (auto it = manifest.recipes.begin(); it != manifest.recipes.end();) {
        const Recipe& recipe = it->second;
        if (recipe.lastRun + kManifestKeepRuns < manifest.run) {
            it = manifest.recipes.erase(it);
            continue;
        }
        PutField(out, recipe.dxc ? "dxc" : "fxc");
        PutField(out, recipe.textMode ? "t" : "b");
        PutField(out, recipe.standardInclude ? "i" : "-");
        PutField(out, recipe.file);
        PutField(out, recipe.sourceName);
        PutField(out, recipe.includeDirectory);
        PutField(out, recipe.prefix);
        PutField(out, recipe.entry);
        PutField(out, recipe.target);
        PutField(out, std::to_string(recipe.flags1));
        PutField(out, std::to_string(recipe.flags2));
        PutField(out, std::to_string(recipe.defines.size()));
        for (const auto& define : recipe.defines) {
            PutField(out, define.first);
            PutField(out, define.second);
        }
        PutField(out, std::to_string(recipe.lastRun));
        PutField(out, std::to_string(static_cast<long long>(recipe.lastMs)));
        PutField(out, recipe.critical ? "1" : "0");
        out += '\n';
        ++it;
    }
    const std::wstring path = ManifestPath();
    wchar_t suffix[32] = {};
    std::swprintf(suffix, 32, L".%lu.tmp", GetCurrentProcessId());
    const std::wstring temporary = path + suffix;
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        if (!file) return;
        file.write(out.data(), static_cast<std::streamsize>(out.size()));
        if (!file) return;
    }
    if (MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING))
        manifest.dirty = false;
    else
        DeleteFileW(temporary.c_str());
}

// Notes that `key` was requested, under the recipe that rebuilds it.
inline void RecordRequest(Recipe recipe, uint64_t key, const char* source,
                          size_t sourceSize) {
    if (!MatchFile(recipe, source, sourceSize)) return;
    const uint64_t identity = recipe.Identity();
    const bool critical = BootPhase().load() && !IsWorkerThread();
    Manifest& manifest = TheManifest();
    std::lock_guard<std::mutex> lock(manifest.mutex);
    LoadManifestLocked(manifest);
    manifest.identityByKey[key] = identity;
    auto it = manifest.recipes.find(identity);
    if (it == manifest.recipes.end()) {
        recipe.lastRun = manifest.run;
        recipe.critical = critical;
        manifest.recipes.emplace(identity, std::move(recipe));
        manifest.dirty = true;
    } else if (it->second.lastRun != manifest.run) {
        // First request this run decides criticality, so a variant that moved
        // off the boot path stops jumping the worker queue.
        it->second.lastRun = manifest.run;
        it->second.critical = critical;
        manifest.dirty = true;
    } else if (critical && !it->second.critical) {
        it->second.critical = true;
        manifest.dirty = true;
    }
    // During boot one save at EndBootPhase covers everything; afterwards
    // requests are rare (lazy pipelines), so save as they come.
    if (manifest.dirty && !BootPhase().load()) SaveManifestLocked(manifest);
}

inline void RecordCompileTime(uint64_t key, double milliseconds) {
    Manifest& manifest = TheManifest();
    std::lock_guard<std::mutex> lock(manifest.mutex);
    auto byKey = manifest.identityByKey.find(key);
    if (byKey == manifest.identityByKey.end()) return;
    auto it = manifest.recipes.find(byKey->second);
    if (it == manifest.recipes.end()) return;
    it->second.lastMs = milliseconds;
    manifest.dirty = true;
}

// Plain DXC compile, no cache. False when DXC is unavailable or the shader
// fails to compile; `errorText` receives the diagnostics.
inline bool RunDXC(const std::string& source, const wchar_t* sourceName,
                   const wchar_t* entry, const wchar_t* target,
                   const std::wstring& includeDirectory, ID3DBlob** blob,
                   std::string* errorText);

inline bool DxcLoaded();

inline void RunRecipeJob(const Recipe& recipe, const std::string& source,
                         uint64_t key, const std::shared_ptr<CompileJob>& job) {
    if (job->claimed.exchange(true)) return;  // a request got there first
    const double start = BootTimer::MillisecondsNow();
    job->startMs = start;
    Microsoft::WRL::ComPtr<ID3DBlob> blob;
    std::string label;
    if (recipe.dxc) {
        const std::wstring name = Widen(recipe.sourceName);
        const std::wstring entry = Widen(recipe.entry);
        const std::wstring target = Widen(recipe.target);
        label = DescribeShader(recipe.sourceName.c_str(), source.data(),
                               source.size(), nullptr, recipe.target.c_str()) +
                " (DXC)";
        if (DxcLoaded())
            RunDXC(source, name.c_str(), entry.c_str(), target.c_str(),
                   Widen(recipe.includeDirectory), blob.GetAddressOf(), nullptr);
    } else {
        std::vector<D3D_SHADER_MACRO> macros;
        for (const auto& define : recipe.defines)
            macros.push_back({ define.first.c_str(), define.second.c_str() });
        macros.push_back({ nullptr, nullptr });
        label = DescribeShader(recipe.sourceName.c_str(), source.data(),
                               source.size(), macros.data(),
                               recipe.target.c_str());
        Microsoft::WRL::ComPtr<ID3DBlob> errors;
        D3DCompile(source.data(), source.size(), recipe.sourceName.c_str(),
                   macros.data(),
                   recipe.standardInclude ? D3D_COMPILE_STANDARD_FILE_INCLUDE
                                          : nullptr,
                   recipe.entry.c_str(), recipe.target.c_str(), recipe.flags1,
                   recipe.flags2, blob.GetAddressOf(), errors.GetAddressOf());
    }
    if (blob) D3DWriteBlobToFile(blob.Get(), CachePath(key).c_str(), TRUE);
    const double milliseconds = BootTimer::MillisecondsNow() - start;
    if (blob) RecordCompileTime(key, milliseconds);
    // A failure is not reported here: the request that needs this shader
    // recompiles it inline and reports the errors where they are handled.
    if (blob) ReportCompile(label + " [worker]", false, true, milliseconds);
    FinishJob(key, job);
}

// Rebuilds every recipe from the current files and queues the ones whose blob
// is missing. Runs on a worker so the main thread never waits on file reads.
inline void PlanPrewarm() {
    std::vector<Recipe> recipes;
    {
        Manifest& manifest = TheManifest();
        std::lock_guard<std::mutex> lock(manifest.mutex);
        LoadManifestLocked(manifest);
        for (const auto& entry : manifest.recipes)
            recipes.push_back(entry.second);
    }
    std::unordered_map<std::string, std::string> files;
    unsigned queued = 0, cached = 0, missingSource = 0, deferred = 0;
    for (const Recipe& recipe : recipes) {
        // Only what boot itself asked for last time. Everything else was
        // requested lazily (a tier switched on later, a background terrain
        // set) and compiles when it is needed, not on every cold boot.
        if (!recipe.critical) { ++deferred; continue; }
        const std::string fileKey = recipe.file + (recipe.textMode ? "|t" : "|b");
        auto file = files.find(fileKey);
        if (file == files.end()) {
            std::string text;
            if (!ReadWholeFile(recipe.file, recipe.textMode, text)) text.clear();
            file = files.emplace(fileKey, std::move(text)).first;
        }
        if (file->second.empty()) { ++missingSource; continue; }
        std::string source = recipe.prefix + file->second;

        uint64_t key = 0;
        if (recipe.dxc) {
            key = KeyDXC(source, Widen(recipe.entry).c_str(),
                         Widen(recipe.target).c_str());
        } else {
            std::vector<D3D_SHADER_MACRO> macros;
            for (const auto& define : recipe.defines)
                macros.push_back({ define.first.c_str(), define.second.c_str() });
            macros.push_back({ nullptr, nullptr });
            key = KeyFXC(source.data(), source.size(), recipe.entry.c_str(),
                         recipe.target.c_str(), recipe.flags1, recipe.flags2,
                         macros.data());
        }
        if (GetFileAttributesW(CachePath(key).c_str()) != INVALID_FILE_ATTRIBUTES) {
            ++cached;
            continue;
        }
        std::shared_ptr<CompileJob> job;
        {
            std::lock_guard<std::mutex> lock(InFlight().mutex);
            std::shared_ptr<CompileJob>& slot = InFlight().jobs[key];
            if (slot) continue;  // already being compiled
            slot = job = std::make_shared<CompileJob>();
        }
        {
            Manifest& manifest = TheManifest();
            std::lock_guard<std::mutex> lock(manifest.mutex);
            manifest.identityByKey[key] = recipe.Identity();
        }
        ++queued;
        Track(job, recipe.lastMs > 0.0 ? recipe.lastMs : kDefaultCompileEstimateMs);
        CompileWorkers::Get().Submit(1, recipe.lastMs,
            [recipe, source = std::move(source), key, job] {
                RunRecipeJob(recipe, source, key, job);
            });
    }
    char text[224];
    std::snprintf(text, sizeof(text),
                  "Shader workers: %u threads; prewarm queued %u of %zu known "
                  "variants (%u cached, %u lazy, %u missing source)",
                  CompileWorkers::Get().ThreadCount(), queued, recipes.size(),
                  cached, deferred, missingSource);
    BootTimer::Log(text);
}

// Queues one boot-critical compile the caller knows it will need, ahead of
// the prewarm. Joins the job when the key is already queued or running.
inline void SubmitCritical(Recipe recipe, std::string source, uint64_t key) {
    if (GetFileAttributesW(CachePath(key).c_str()) != INVALID_FILE_ATTRIBUTES)
        return;
    std::shared_ptr<CompileJob> job;
    {
        std::lock_guard<std::mutex> lock(InFlight().mutex);
        std::shared_ptr<CompileJob>& slot = InFlight().jobs[key];
        if (!slot) slot = std::make_shared<CompileJob>();
        job = slot;
    }
    double estimate = kDefaultCompileEstimateMs;
    {
        Manifest& manifest = TheManifest();
        std::lock_guard<std::mutex> lock(manifest.mutex);
        auto byKey = manifest.identityByKey.find(key);
        if (byKey != manifest.identityByKey.end()) {
            auto it = manifest.recipes.find(byKey->second);
            if (it != manifest.recipes.end() && it->second.lastMs > 0.0)
                estimate = it->second.lastMs;
        }
    }
    Track(job, estimate);
    // A second queue entry for a job already queued at lower priority is
    // harmless: whichever runs first claims it, the other returns at once.
    if (!job->claimed.load())
        CompileWorkers::Get().Submit(2, estimate,
            [recipe = std::move(recipe), source = std::move(source), key, job] {
                RunRecipeJob(recipe, source, key, job);
            });
}

} // namespace detail

// Call once, as early in boot as possible: the workers start compiling every
// variant the last runs asked for while the device and assets come up.
inline void StartPrewarm() {
    auto planned = std::make_shared<std::promise<void>>();
    {
        std::lock_guard<std::mutex> lock(detail::Tracker().mutex);
        detail::Tracker().planned = planned->get_future().share();
    }
    CompileWorkers::Get().Submit(INT_MAX, 0.0, [planned] {
        detail::PlanPrewarm();
        planned->set_value();
    });
}

// A thread doing lazy, non-boot work (e.g. a background pipeline build): its
// requests never mark a recipe boot-critical.
inline void MarkBackgroundThread() { detail::IsWorkerThread() = true; }

// FXC compile with no defines and the standard include handler, queued as
// boot-critical. The result lands in the cache; the later CompileCached call
// with the same arguments hits it or waits for it.
inline void SubmitFXC(const std::string& source, const char* sourceName,
                      const char* entry, const char* target, UINT flags1,
                      UINT flags2) {
    Recipe recipe;
    recipe.file = recipe.sourceName = sourceName;
    recipe.standardInclude = true;
    recipe.entry = entry;
    recipe.target = target;
    recipe.flags1 = flags1;
    recipe.flags2 = flags2;
    const uint64_t key = KeyFXC(source.data(), source.size(), entry, target,
                                flags1, flags2, nullptr);
    detail::SubmitCritical(std::move(recipe), source, key);
}

// DXC counterpart of SubmitFXC; same arguments as CompileCachedDXC.
inline void SubmitDXC(const std::string& source, const wchar_t* sourceName,
                      const wchar_t* entry, const wchar_t* target,
                      const std::wstring& includeDirectory) {
    Recipe recipe;
    recipe.dxc = true;
    recipe.sourceName = Narrow(sourceName);
    recipe.includeDirectory = Narrow(includeDirectory);
    recipe.file = recipe.includeDirectory + "\\" + recipe.sourceName;
    recipe.entry = Narrow(entry);
    recipe.target = Narrow(target);
    detail::SubmitCritical(std::move(recipe), source, KeyDXC(source, entry, target));
}

// Boot-critical compile progress, 0..1, weighted by each job's recorded
// compile time; a running job counts its elapsed share, capped short of done.
// Returns false once nothing tracked is outstanding (and forgets the batch).
// The first call waits for the prewarm plan, which takes milliseconds.
inline bool BootCompilesPending(float* progress, unsigned* remaining = nullptr) {
    detail::BootTracker& tracker = detail::Tracker();
    std::shared_future<void> planned;
    {
        std::lock_guard<std::mutex> lock(tracker.mutex);
        planned = tracker.planned;
    }
    if (planned.valid()) planned.wait();

    std::lock_guard<std::mutex> lock(tracker.mutex);
    const double now = BootTimer::MillisecondsNow();
    double total = 0.0, finished = 0.0;
    unsigned outstanding = 0;
    for (const detail::TrackedCompile& tracked : tracker.jobs) {
        total += tracked.estimateMs;
        if (tracked.job->done.wait_for(std::chrono::seconds(0)) ==
            std::future_status::ready) {
            finished += tracked.estimateMs;
            continue;
        }
        ++outstanding;
        const double start = tracked.job->startMs.load();
        if (start > 0.0)
            finished += tracked.estimateMs *
                (std::min)(0.95, (now - start) / tracked.estimateMs);
    }
    if (progress) *progress = total > 0.0 ? static_cast<float>(finished / total) : 1.0f;
    if (remaining) *remaining = outstanding;
    if (outstanding == 0) tracker.jobs.clear();
    return outstanding > 0;
}

// Call when boot is done. Requests after this are background work as far as
// the worker queue is concerned, and the manifest is saved.
inline void EndBootPhase() {
    detail::BootPhase() = false;
    detail::Manifest& manifest = detail::TheManifest();
    std::lock_guard<std::mutex> lock(manifest.mutex);
    detail::LoadManifestLocked(manifest);
    // A variant boot no longer asks for (the predicted tier changed) stops
    // being prewarmed; it compiles when something requests it.
    for (auto& entry : manifest.recipes) {
        if (entry.second.critical && entry.second.lastRun != manifest.run) {
            entry.second.critical = false;
            manifest.dirty = true;
        }
    }
    if (manifest.dirty) detail::SaveManifestLocked(manifest);
}

// Saves compile times and lazily requested variants gathered since boot.
inline void SaveManifest() {
    detail::Manifest& manifest = detail::TheManifest();
    std::lock_guard<std::mutex> lock(manifest.mutex);
    if (manifest.loaded && manifest.dirty) detail::SaveManifestLocked(manifest);
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

    const uint64_t key = KeyFXC(source, sourceSize, entry, target, flags1,
                                flags2, defines);
    // Recordable only with an include handler a worker can recreate.
    if (sourceName && entry && target &&
        (include == nullptr || include == D3D_COMPILE_STANDARD_FILE_INCLUDE)) {
        Recipe recipe;
        recipe.file = sourceName;
        recipe.sourceName = sourceName;
        recipe.standardInclude = include == D3D_COMPILE_STANDARD_FILE_INCLUDE;
        recipe.entry = entry;
        recipe.target = target;
        recipe.flags1 = flags1;
        recipe.flags2 = flags2;
        for (const D3D_SHADER_MACRO* macro = defines; macro && macro->Name; ++macro)
            recipe.defines.emplace_back(macro->Name,
                                        macro->Definition ? macro->Definition : "");
        detail::RecordRequest(std::move(recipe), key,
                              static_cast<const char*>(source), sourceSize);
    }

    const std::wstring path = CachePath(key);
    const double start = BootTimer::MillisecondsNow();
    const std::string label = DescribeShader(
        sourceName, static_cast<const char*>(source), sourceSize, defines, target);
    if (SUCCEEDED(D3DReadFileToBlob(path.c_str(), blob)) && *blob) {
        ReportCompile(label, true, true, BootTimer::MillisecondsNow() - start);
        return S_OK;
    }
    const std::shared_ptr<detail::CompileJob> job = ClaimCompile(key);
    if (!job && SUCCEEDED(D3DReadFileToBlob(path.c_str(), blob)) && *blob) {
        ReportCompile(label, true, true, BootTimer::MillisecondsNow() - start,
                      "shader WAITED ");
        return S_OK;
    }
    BootTimer::Log("shader compiling... " + label);

    const HRESULT hr = D3DCompile(source, sourceSize, sourceName, defines,
                                  include, entry, target, flags1, flags2,
                                  blob, errors);
    if (SUCCEEDED(hr) && *blob)
        D3DWriteBlobToFile(*blob, path.c_str(), TRUE);
    const double milliseconds = BootTimer::MillisecondsNow() - start;
    if (SUCCEEDED(hr) && *blob) detail::RecordCompileTime(key, milliseconds);
    ReportCompile(label, false, SUCCEEDED(hr) && *blob, milliseconds);
    if (job) detail::FinishJob(key, job);
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

inline bool detail::DxcLoaded() { return DxcAvailable(); }

inline bool detail::RunDXC(const std::string& source, const wchar_t* sourceName,
                           const wchar_t* entry, const wchar_t* target,
                           const std::wstring& includeDirectory, ID3DBlob** blob,
                           std::string* errorText) {
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
    return true;
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

    const uint64_t key = KeyDXC(source, entry, target);
    if (sourceName && entry && target) {
        Recipe recipe;
        recipe.dxc = true;
        recipe.sourceName = Narrow(sourceName);
        recipe.includeDirectory = Narrow(includeDirectory);
        recipe.file = recipe.includeDirectory + "\\" + recipe.sourceName;
        recipe.entry = Narrow(entry);
        recipe.target = Narrow(target);
        detail::RecordRequest(std::move(recipe), key, source.data(),
                              source.size());
    }

    const std::wstring path = CachePath(key);
    const double start = BootTimer::MillisecondsNow();
    const std::string label =
        DescribeShader(Narrow(sourceName ? sourceName : L"").c_str(),
                       source.data(), source.size(), nullptr,
                       Narrow(target ? target : L"").c_str()) + " (DXC)";
    if (SUCCEEDED(D3DReadFileToBlob(path.c_str(), blob)) && *blob) {
        ReportCompile(label, true, true, BootTimer::MillisecondsNow() - start);
        return true;
    }
    const std::shared_ptr<detail::CompileJob> job = ClaimCompile(key);
    if (!job && SUCCEEDED(D3DReadFileToBlob(path.c_str(), blob)) && *blob) {
        ReportCompile(label, true, true, BootTimer::MillisecondsNow() - start,
                      "shader WAITED ");
        return true;
    }
    BootTimer::Log("shader compiling... " + label);
    const bool ok = detail::RunDXC(source, sourceName, entry, target,
                                   includeDirectory, blob, errorText);
    if (ok) D3DWriteBlobToFile(*blob, path.c_str(), TRUE);
    const double milliseconds = BootTimer::MillisecondsNow() - start;
    if (ok) detail::RecordCompileTime(key, milliseconds);
    ReportCompile(label, false, ok, milliseconds);
    if (job) detail::FinishJob(key, job);
    return ok;
}

} // namespace ShaderCacheDX12

#endif // SHADER_CACHE_DX12_H
