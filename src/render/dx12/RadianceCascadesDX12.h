#pragma once

#include "DX12Core.h"
#include "ShaderCacheDX12.h"
#include "ProfilerDX12.h"
#include <array>
#include <chrono>
#include <fstream>
#include <future>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

extern ProfilerDX12 g_profiler;

// Private to the visibility renderer. No resources or PSOs are created until
// selected, and every in-flight frame owns its atlas and descriptor tables.
class RadianceCascadesDX12 {
public:
    static constexpr UINT LevelCount = 4;
    static constexpr UINT ProbeSpacing = 16;
    static constexpr UINT BaseDescriptorCount = 108;
    static constexpr UINT DescriptorCount = 119;
    // [0..3] trace passes, one per level; [4] the resolve.
    static constexpr UINT PassCount = LevelCount + 1;
    static constexpr UINT ResolvePass = LevelCount;

    void Configure(const D3D12_DESCRIPTOR_RANGE* ranges, UINT count,
                   const D3D12_STATIC_SAMPLER_DESC* samplers) {
        baseRanges.assign(ranges, ranges + count);
        for (UINT i = 0; i < 3; ++i) staticSamplers[i] = samplers[i];
    }

    // Four permutations of the full resolve take minutes on a cold cache, so
    // they build off-thread and the frame keeps Lumen until they land.
    bool Ensure(const std::string& source, bool bindless, UINT width, UINT height) {
        const UINT tier = bindless ? 1u : 0u;
        auto& p = pipelines[tier];
        if (!p.attempted) BuildPipelines(source, bindless);
        if (p.pending.valid()) {
            if (p.pending.wait_for(std::chrono::seconds(0)) !=
                std::future_status::ready)
                return false;
            Build build = p.pending.get();
            if (!build.errors.empty()) {
                std::ofstream("radiance_cascades_shader_error.log",
                              std::ios::trunc) << build.errors;
                std::cerr << build.errors << "\n";
            }
            p.trace = build.pso[0];
            p.resolve = build.pso[1];
            p.terrain = build.pso[2];
            p.terrainOnly = build.pso[3];
            p.ready = p.trace && p.resolve && p.terrain && p.terrainOnly;
            status = p.ready ? "Ready" : build.status;
        }
        if (!p.ready) return false;
        if (!resourcesAttempted) CreateResources(width, height);
        return resourcesReady && resourceWidth == width && resourceHeight == height;
    }

    void ResetResources() {
        for (auto& frame : frames) frame = Frame{};
        resourcesAttempted = resourcesReady = false;
        resourceWidth = resourceHeight = 0;
    }

    const char* Status() const { return status.c_str(); }
    ID3D12RootSignature* Root(bool bindless) const {
        return pipelines[bindless ? 1 : 0].root.Get();
    }
    ID3D12PipelineState* ResolvePSO(bool bindless, bool terrain, bool only) const {
        const auto& p = pipelines[bindless ? 1 : 0];
        return only ? p.terrainOnly.Get() : terrain ? p.terrain.Get() : p.resolve.Get();
    }
    ID3D12DescriptorHeap* Heap(UINT slot, UINT pass) const {
        return frames[slot].heaps[pass].Get();
    }

    void PrepareDescriptors(UINT slot, ID3D12DescriptorHeap* source) {
        auto& frame = frames[slot];
        for (UINT pass = 0; pass <= LevelCount; ++pass) {
            auto start = frame.heaps[pass]->GetCPUDescriptorHandleForHeapStart();
            g_dx12.device->CopyDescriptorsSimple(BaseDescriptorCount, start,
                source->GetCPUDescriptorHandleForHeapStart(),
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            auto handle = [start](UINT index) {
                auto h = start;
                h.ptr += static_cast<SIZE_T>(index) * g_dx12.cbvSrvUavDescriptorSize;
                return h;
            };
            D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            srv.Texture2DArray.MipLevels = 1;
            for (UINT level = 0; level < LevelCount; ++level) {
                srv.Texture2DArray.ArraySize = Directions(level);
                // Only the next, completed atlas is readable in a trace pass.
                // The consumer reads irradiance, so all its directional SRVs are null.
                auto* input = pass + 1 == level ? frame.atlases[level].Get() : nullptr;
                g_dx12.device->CreateShaderResourceView(input, &srv,
                    handle(BaseDescriptorCount + level));
            }
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Texture2D.MipLevels = 1;
            ID3D12Resource* guides[] = { frame.irradiance.Get(),
                frame.positions.Get(), frame.normals.Get() };
            for (UINT guide = 0; guide < 3; ++guide) {
                srv.Format = guide == 0 ? DXGI_FORMAT_R16G16B16A16_FLOAT
                                       : DXGI_FORMAT_R32G32B32A32_FLOAT;
                g_dx12.device->CreateShaderResourceView(
                    pass == LevelCount ? guides[guide] : nullptr, &srv,
                    handle(BaseDescriptorCount + 4 + guide));
            }
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
            uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
            uav.Texture2DArray.ArraySize = Directions(pass < LevelCount ? pass : 0);
            g_dx12.device->CreateUnorderedAccessView(
                pass < LevelCount ? frame.atlases[pass].Get() : nullptr,
                nullptr, &uav, handle(BaseDescriptorCount + 7));
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            for (UINT guide = 0; guide < 3; ++guide) {
                uav.Format = guide == 0 ? DXGI_FORMAT_R16G16B16A16_FLOAT
                                       : DXGI_FORMAT_R32G32B32A32_FLOAT;
                g_dx12.device->CreateUnorderedAccessView(
                    pass == 0 ? guides[guide] : nullptr, nullptr, &uav,
                    handle(BaseDescriptorCount + 8 + guide));
            }
        }
    }

    // The resolve shares the cascade root signature: the base table plus the
    // guide SRVs, and b6 with the trace-only fields zeroed.
    void BindResolve(ID3D12GraphicsCommandList* cmd, bool bindless,
                     ID3D12DescriptorHeap* heap, UINT64 frameConstants,
                     D3D12_GPU_DESCRIPTOR_HANDLE table) {
        cmd->SetDescriptorHeaps(1, &heap);
        cmd->SetComputeRootSignature(Root(bindless));
        cmd->SetComputeRootConstantBufferView(0, frameConstants);
        cmd->SetComputeRootDescriptorTable(1, table);
        UINT constants[] = { 0u, ProbeSpacing, LevelCount, 1u };
        cmd->SetComputeRoot32BitConstants(3, 4, constants, 0);
    }

    void Dispatch(ID3D12GraphicsCommandList* cmd, UINT slot, UINT64 frameConstants,
                  bool bindless, ID3D12DescriptorHeap* bindlessHeap,
                  const std::array<D3D12_GPU_DESCRIPTOR_HANDLE, LevelCount + 1>& tables) {
        ProfilerDX12::Scope profile(g_profiler, "Radiance Cascades GI", cmd);
        auto& frame = frames[slot];
        for (int level = LevelCount - 1; level >= 0; --level) {
            const char* names[] = { "Radiance Cascade 0", "Radiance Cascade 1",
                                    "Radiance Cascade 2", "Radiance Cascade 3" };
            ProfilerDX12::Scope cascade(g_profiler, names[level], cmd);
            Transition(cmd, frame.atlases[level].Get(), true);
            if (level == 0) {
                Transition(cmd, frame.irradiance.Get(), true);
                Transition(cmd, frame.positions.Get(), true);
                Transition(cmd, frame.normals.Get(), true);
            }
            ID3D12DescriptorHeap* heap = bindless ? bindlessHeap : frame.heaps[level].Get();
            cmd->SetDescriptorHeaps(1, &heap);
            cmd->SetComputeRootSignature(Root(bindless));
            cmd->SetComputeRootConstantBufferView(0, frameConstants);
            cmd->SetComputeRootDescriptorTable(1, tables[level]);
            UINT constants[] = { static_cast<UINT>(level), ProbeSpacing, LevelCount, DebugMode() };
            cmd->SetComputeRoot32BitConstants(3, 4, constants, 0);
            cmd->SetPipelineState(pipelines[bindless ? 1 : 0].trace.Get());
            UINT spacing = ProbeSpacing << level;
            cmd->Dispatch((resourceWidth + spacing - 1) / spacing,
                          (resourceHeight + spacing - 1) / spacing,
                          (Directions(level) + 63) / 64);
            // This transition completes the writes before the next cascade's
            // interpolation reads them; all passes stay on the graphics list.
            Transition(cmd, frame.atlases[level].Get(), false);
            if (level == 0) {
                Transition(cmd, frame.irradiance.Get(), false);
                Transition(cmd, frame.positions.Get(), false);
                Transition(cmd, frame.normals.Get(), false);
            }
        }
    }

private:
    struct Build {
        ComPtr<ID3D12PipelineState> pso[4];
        std::string errors, status;
    };
    struct Pipelines {
        bool attempted = false, ready = false;
        ComPtr<ID3D12RootSignature> root;
        ComPtr<ID3D12PipelineState> trace, resolve, terrain, terrainOnly;
        std::future<Build> pending;
    } pipelines[2];
    struct Frame {
        std::array<ComPtr<ID3D12Resource>, LevelCount> atlases;
        ComPtr<ID3D12Resource> irradiance, positions, normals;
        std::array<ComPtr<ID3D12DescriptorHeap>, LevelCount + 1> heaps;
    };
    std::array<Frame, FRAME_COUNT> frames;
    std::vector<D3D12_DESCRIPTOR_RANGE> baseRanges;
    D3D12_STATIC_SAMPLER_DESC staticSamplers[3]{};
    bool resourcesAttempted = false, resourcesReady = false;
    UINT resourceWidth = 0, resourceHeight = 0;
    std::string status = "Not selected";

    static UINT Directions(UINT level) { return 16u << (2u * level); }
    // SGE_RC_DEBUG=N swaps the level-0 irradiance for a diagnostic (see
    // RadianceCascadeMain). The resolve only tests rcEnabled != 0.
    static UINT DebugMode() {
        static const UINT mode = [] {
            char value[8] = {};
            return GetEnvironmentVariableA("SGE_RC_DEBUG", value, sizeof(value))
                ? 1u + static_cast<UINT>(atoi(value)) : 1u;
        }();
        return mode;
    }
    static void Transition(ID3D12GraphicsCommandList* cmd, ID3D12Resource* resource,
                           bool write) {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = write ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                                              : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        barrier.Transition.StateAfter = write ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS
                                             : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        cmd->ResourceBarrier(1, &barrier);
    }

    void BuildPipelines(const std::string& source, bool bindless) {
        auto& p = pipelines[bindless ? 1 : 0];
        p.attempted = true;
        if (baseRanges.empty()) { status = "Enhanced root signature unavailable"; return; }
        auto ranges = baseRanges;
        D3D12_DESCRIPTOR_RANGE extra = {};
        extra.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        extra.NumDescriptors = 7;
        extra.BaseShaderRegister = 93;
        extra.OffsetInDescriptorsFromTableStart = BaseDescriptorCount;
        ranges.push_back(extra);
        extra.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        extra.NumDescriptors = 4;
        extra.BaseShaderRegister = 11;
        extra.OffsetInDescriptorsFromTableStart = BaseDescriptorCount + 7;
        ranges.push_back(extra);
        D3D12_ROOT_PARAMETER params[4] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor.ShaderRegister = 0;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = static_cast<UINT>(ranges.size());
        params[1].DescriptorTable.pDescriptorRanges = ranges.data();
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[2].Descriptor.ShaderRegister = 90;
        params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[3].Constants.ShaderRegister = 6;
        params[3].Constants.Num32BitValues = 4;
        D3D12_ROOT_SIGNATURE_DESC root = {};
        root.NumParameters = 4;
        root.pParameters = params;
        root.NumStaticSamplers = 3;
        root.pStaticSamplers = staticSamplers;
        if (bindless) root.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;
        ComPtr<ID3DBlob> blob, error;
        if (FAILED(D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1,
                &blob, &error)) || FAILED(g_dx12.device->CreateRootSignature(0,
                blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&p.root)))) {
            status = "Cascade root signature creation failed";
            return;
        }
        if (!ShaderCacheDX12::DxcAvailable()) {
            status = "dxcompiler.dll unavailable; using Lumen";
            return;
        }
        status = "Compiling shaders; using Lumen";
        const std::string base = "#define SGE_RADIANCE_CASCADES 1\n" + source;
        const std::string terrain = "#define SGE_TERRAIN_VISIBILITY 1\n" + base;
        // Owned by value: the build outlives this call and may outlive a
        // later Ensure that switches tiers.
        p.pending = std::async(std::launch::async,
            [root = p.root, device = g_dx12.device,
             bindless, base, terrain]() {
            const std::wstring directory =
                ShaderCacheDX12::ExecutableDirectory() + L"shaders";
            const wchar_t* profile = bindless ? L"cs_6_6" : L"cs_6_5";
            const std::string sources[4] = { terrain, base, terrain,
                "#define SGE_TERRAIN_ONLY_RESOLVE 1\n" + terrain };
            const wchar_t* entries[4] = { L"RadianceCascadeMain", L"main",
                                          L"main", L"main" };
            Build result;
            std::string errors[4];
            std::thread threads[4];
            for (int i = 0; i < 4; ++i) {
                threads[i] = std::thread([&, i] {
                    SetThreadPriority(GetCurrentThread(),
                                      THREAD_PRIORITY_BELOW_NORMAL);
                    ShaderCacheDX12::MarkBackgroundThread();
                    ComPtr<ID3DBlob> shader;
                    if (!ShaderCacheDX12::CompileCachedDXC(sources[i],
                            L"visbuf_resolve_cs.hlsl", entries[i], profile,
                            directory, &shader, &errors[i])) {
                        if (errors[i].empty()) errors[i] = "compile failed";
                        return;
                    }
                    D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
                    desc.pRootSignature = root.Get();
                    desc.CS = { shader->GetBufferPointer(), shader->GetBufferSize() };
                    if (FAILED(device->CreateComputePipelineState(&desc,
                            IID_PPV_ARGS(&result.pso[i])))) {
                        result.pso[i].Reset();
                        errors[i] = "PSO creation failed";
                    }
                });
            }
            for (auto& thread : threads) thread.join();
            static const char* names[4] = { "trace", "resolve",
                                            "terrain resolve", "terrain-only resolve" };
            for (int i = 0; i < 4; ++i)
                if (!errors[i].empty())
                    result.errors += std::string("Radiance cascades ") +
                                     names[i] + ": " + errors[i] + "\n";
            result.status = result.errors.empty()
                ? "Ready" : "Cascade shader build failed; using Lumen";
            return result;
        });
    }

    void CreateResources(UINT width, UINT height) {
        resourcesAttempted = true;
        if (width == 0 || height == 0) return;
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        auto texture = [&](ComPtr<ID3D12Resource>& output, UINT w, UINT h,
                           UINT layers, DXGI_FORMAT format) {
            D3D12_RESOURCE_DESC desc = {};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = w;
            desc.Height = h;
            desc.DepthOrArraySize = static_cast<UINT16>(layers);
            desc.MipLevels = 1;
            desc.Format = format;
            desc.SampleDesc.Count = 1;
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            return SUCCEEDED(g_dx12.device->CreateCommittedResource(&heap,
                D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                nullptr, IID_PPV_ARGS(&output)));
        };
        for (auto& frame : frames) {
            for (UINT level = 0; level < LevelCount; ++level) {
                UINT spacing = ProbeSpacing << level;
                if (!texture(frame.atlases[level], (width + spacing - 1) / spacing,
                        (height + spacing - 1) / spacing, Directions(level),
                        DXGI_FORMAT_R16G16B16A16_FLOAT)) {
                    status = "Cascade atlas allocation failed; using Lumen";
                    return;
                }
            }
            UINT w = (width + ProbeSpacing - 1) / ProbeSpacing;
            UINT h = (height + ProbeSpacing - 1) / ProbeSpacing;
            if (!texture(frame.irradiance, w, h, 1, DXGI_FORMAT_R16G16B16A16_FLOAT) ||
                !texture(frame.positions, w, h, 1, DXGI_FORMAT_R32G32B32A32_FLOAT) ||
                !texture(frame.normals, w, h, 1, DXGI_FORMAT_R32G32B32A32_FLOAT)) {
                status = "Cascade guide allocation failed; using Lumen";
                return;
            }
            D3D12_DESCRIPTOR_HEAP_DESC desc = {};
            desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            desc.NumDescriptors = DescriptorCount;
            desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            for (auto& table : frame.heaps) {
                if (FAILED(g_dx12.device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&table)))) {
                    status = "Cascade descriptor allocation failed; using Lumen";
                    return;
                }
            }
        }
        resourceWidth = width;
        resourceHeight = height;
        resourcesReady = true;
        status = "Ready";
    }
};
