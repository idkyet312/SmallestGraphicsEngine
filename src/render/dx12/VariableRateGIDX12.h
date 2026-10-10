#pragma once

#include "DX12Core.h"
#include "ShaderCacheDX12.h"
#include "ProfilerDX12.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <fstream>
#include <future>
#include <string>
#include <vector>

extern ProfilerDX12 g_profiler;

// The optional GI backend owns all scratch, history, and tables per frame slot.
// Only its compact trace launches primary bounce rays; lighting reads the result.
class VariableRateGIDX12 {
public:
    static constexpr UINT BaseDescriptorCount = 108;
    static constexpr UINT DescriptorCount = 122;
    static constexpr UINT HistoryStride = 48;
    struct Constants {
        UINT tilesX, tilesY, rayBudget, historyValid;
        UINT enabled, debug, padding[2];
    };
    static_assert(sizeof(Constants) == 32, "VariableRateGIConstants layout");
    struct Bindings {
        UINT64 frameConstants;
        D3D12_GPU_VIRTUAL_ADDRESS cache, samples, weights, lights, emissive;
    };

    void Configure(const D3D12_DESCRIPTOR_RANGE* ranges, UINT count,
                   const D3D12_STATIC_SAMPLER_DESC* samplers) {
        baseRanges.assign(ranges, ranges + count);
        std::copy(samplers, samplers + 3, staticSamplers.begin());
    }
    void InvalidateHistory() { historyValid = false; }
    void ResetResources() {
        for (auto& frame : frames) frame = Frame{};
        resourcesAttempted = resourcesReady = historyValid = false;
        width = height = 0;
        statisticsValid = false;
    }
    const char* Status() const { return status.c_str(); }

    bool Ensure(const std::string& source, bool bindless, UINT w, UINT h) {
        auto& p = pipelines[bindless ? 1 : 0];
        if (!p.attempted) BuildPipelines(source, bindless);
        if (p.pending.valid() && p.pending.wait_for(std::chrono::seconds(0)) ==
                std::future_status::ready) {
            Build build = p.pending.get();
            p.pso = std::move(build.pso);
            p.ready = std::all_of(p.pso.begin(), p.pso.end(),
                [](const auto& pso) { return pso != nullptr; });
            if (!build.errors.empty())
                std::ofstream("variable_rate_gi_shader_error.log", std::ios::trunc)
                    << build.errors;
            status = p.ready ? "Ready" : "Shader build failed; using full-rate Lumen";
        }
        if (!p.ready) return false;
        if (!resourcesAttempted) CreateResources(w, h);
        return resourcesReady && width == w && height == h;
    }

    ID3D12DescriptorHeap* Heap(UINT slot) const { return frames[slot].heap.Get(); }
    // Copy source for the bindless transient table: shader-visible heaps are
    // CPU write-only, so CopyDescriptors may not read them.
    ID3D12DescriptorHeap* StagingHeap(UINT slot) const { return frames[slot].staging.Get(); }
    ID3D12PipelineState* ResolvePSO(bool bindless, bool terrain, bool only) const {
        return pipelines[bindless ? 1 : 0].pso[only ? 10 : terrain ? 9 : 8].Get();
    }

    void PrepareDescriptors(UINT slot, ID3D12DescriptorHeap* source) {
        auto& f = frames[slot];
        // A completed submission fence is evidence, not an elapsed-frame guess.
        if (f.statisticsPending && g_dx12.fence->GetCompletedValue() >= f.statisticsFence) {
            D3D12_RANGE read{0, 8};
            void* mapped = nullptr;
            if (SUCCEEDED(f.statistics->Map(0, &read, &mapped))) {
                const auto* counters = static_cast<const UINT*>(mapped);
                measuredRays = counters[0] + counters[1];
                measuredBudget = f.statisticsBudget;
                statisticsValid = true;
                D3D12_RANGE noWrite{0, 0};
                f.statistics->Unmap(0, &noWrite);
            }
            f.statisticsPending = false;
        }
        auto start = f.staging->GetCPUDescriptorHandleForHeapStart();
        g_dx12.device->CopyDescriptorsSimple(BaseDescriptorCount, start,
            source->GetCPUDescriptorHandleForHeapStart(), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto handle = [start](UINT offset) {
            auto h = start;
            h.ptr += SIZE_T(offset) * g_dx12.cbvSrvUavDescriptorSize;
            return h;
        };
        // If rendering resumed on the same slot, do not bind its write resource
        // as previous history. It will be overwritten during accumulation.
        const bool canRead = historyValid && previousSlot != slot;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        srv.Format = DXGI_FORMAT_UNKNOWN;
        srv.Buffer.NumElements = width * height;
        srv.Buffer.StructureByteStride = HistoryStride;
        g_dx12.device->CreateShaderResourceView(canRead ? frames[previousSlot].history.Get() : nullptr,
            &srv, handle(108));
        g_dx12.device->CreateShaderResourceView(f.history.Get(), &srv, handle(109));
        for (UINT i = 110; i < 113; ++i)
            g_dx12.device->CreateShaderResourceView(nullptr, &srv, handle(i));
        srv = {};
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        srv.Texture2D.MipLevels = 1;
        g_dx12.device->CreateShaderResourceView(f.irradiance.Get(), &srv, handle(113));

        const UINT pixels = width * height;
        ID3D12Resource* buffers[] = { f.counters.Get(), f.requests.Get(), f.samples.Get(),
            f.gradients.Get(), f.jobs.Get(), f.history.Get() };
        const UINT counts[] = { 68, pixels, pixels * 4u, tilesX * tilesY, pixels, pixels };
        const UINT strides[] = { 4, 8, 4, 32, 16, HistoryStride };
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        for (UINT i = 0; i < 6; ++i) {
            uav.Buffer.NumElements = counts[i];
            uav.Buffer.StructureByteStride = strides[i];
            g_dx12.device->CreateUnorderedAccessView(buffers[i], nullptr, &uav, handle(114 + i));
        }
        uav = {};
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        uav.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        g_dx12.device->CreateUnorderedAccessView(f.irradiance.Get(), nullptr, &uav, handle(120));
        uav = {};
        uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        uav.Format = DXGI_FORMAT_R32_TYPELESS;
        uav.Buffer.NumElements = 3;
        uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        g_dx12.device->CreateUnorderedAccessView(f.arguments.Get(), nullptr, &uav, handle(121));
        g_dx12.device->CopyDescriptorsSimple(DescriptorCount,
            f.heap->GetCPUDescriptorHandleForHeapStart(), start,
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }

    void Bind(ID3D12GraphicsCommandList* cmd, bool bindless, ID3D12DescriptorHeap* heap,
              D3D12_GPU_DESCRIPTOR_HANDLE table, const Bindings& b, const Constants& constants) {
        cmd->SetDescriptorHeaps(1, &heap);
        cmd->SetComputeRootSignature(pipelines[bindless ? 1 : 0].root.Get());
        cmd->SetComputeRootConstantBufferView(0, b.frameConstants);
        cmd->SetComputeRootDescriptorTable(1, table);
        cmd->SetComputeRoot32BitConstants(3, 8, &constants, 0);
        if (b.cache) cmd->SetComputeRootUnorderedAccessView(4, b.cache);
        if (b.samples) cmd->SetComputeRootUnorderedAccessView(5, b.samples);
        if (b.weights) cmd->SetComputeRootUnorderedAccessView(6, b.weights);
        if (b.lights) cmd->SetComputeRootShaderResourceView(7, b.lights);
        if (b.emissive) cmd->SetComputeRootUnorderedAccessView(8, b.emissive);
    }

    void Dispatch(ID3D12GraphicsCommandList* cmd, UINT slot, bool bindless,
                  ID3D12DescriptorHeap* heap, D3D12_GPU_DESCRIPTOR_HANDLE table,
                  const Bindings& bindings, float budgetFraction) {
        ProfilerDX12::Scope total(g_profiler, "Variable Rate GI", cmd);
        auto& f = frames[slot];
        Constants constants = { tilesX, tilesY,
            std::max(1u, UINT(double(width) * height * std::clamp(budgetFraction, 0.125f, 1.0f))),
            historyValid && previousSlot != slot ? 1u : 0u, 1u, DebugMode(), {0, 0} };
        lastRayBudget = constants.rayBudget;
        Bind(cmd, bindless, heap, table, bindings, constants);
        if (f.argumentsIndirect)
            Transition(cmd, f.arguments.Get(), D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(cmd, f.history.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Transition(cmd, f.irradiance.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const auto& p = pipelines[bindless ? 1 : 0];
        const char* names[] = { "VRRT Reset", "VRRT Gradients", "VRRT Classify", "VRRT Budget",
            "VRRT Compact", "VRRT Trace", "VRRT Accumulate", "VRRT Reconstruct" };
        for (UINT pass = 0; pass < 8; ++pass) {
            ProfilerDX12::Scope scope(g_profiler, names[pass], cmd);
            cmd->SetPipelineState(p.pso[pass].Get());
            if (pass == 5) {
                Transition(cmd, f.arguments.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
                cmd->ExecuteIndirect(dispatchSignature.Get(), 1, f.arguments.Get(), 0, nullptr, 0);
                f.argumentsIndirect = true;
            } else if (pass == 0) cmd->Dispatch(2, 1, 1);
            else if (pass == 1) cmd->Dispatch((tilesX + 7) / 8, (tilesY + 7) / 8, 1);
            else if (pass == 3) cmd->Dispatch(1, 1, 1);
            else cmd->Dispatch(tilesX, tilesY, 1);
            // Each stage consumes different UAV outputs of the preceding one.
            // They stay in UAV state until the explicit history/result reads.
            D3D12_RESOURCE_BARRIER barrier{};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            cmd->ResourceBarrier(1, &barrier);
            if (pass == 6)
                Transition(cmd, f.history.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                           D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        Transition(cmd, f.irradiance.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (!f.statisticsPending) {
            Transition(cmd, f.counters.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                       D3D12_RESOURCE_STATE_COPY_SOURCE);
            cmd->CopyBufferRegion(f.statistics.Get(), 0, f.counters.Get(), 64u * 4u, 8);
            Transition(cmd, f.counters.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                       D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            f.statisticsFence = g_dx12.fenceValues[g_dx12.frameIndex];
            f.statisticsBudget = constants.rayBudget;
            f.statisticsPending = true;
        }
        previousSlot = slot;
        historyValid = true;
        // The lighting consumer needs the same constants/table, already bound.
    }
    UINT LastRayBudget() const { return lastRayBudget; }
    UINT MeasuredRays() const { return measuredRays; }
    UINT MeasuredBudget() const { return measuredBudget; }
    bool StatisticsValid() const { return statisticsValid; }

private:
    struct Build { std::array<ComPtr<ID3D12PipelineState>, 11> pso; std::string errors; };
    struct Pipeline {
        bool attempted = false, ready = false;
        ComPtr<ID3D12RootSignature> root;
        std::array<ComPtr<ID3D12PipelineState>, 11> pso;
        std::future<Build> pending;
    };
    struct Frame {
        ComPtr<ID3D12Resource> counters, requests, samples, gradients, jobs, history, irradiance, arguments, statistics;
        bool statisticsPending = false;
        UINT64 statisticsFence = 0;
        UINT statisticsBudget = 0;
        ComPtr<ID3D12DescriptorHeap> heap, staging;
        bool argumentsIndirect = false;
    };
    std::array<Frame, FRAME_COUNT> frames;
    std::array<Pipeline, 2> pipelines;
    std::vector<D3D12_DESCRIPTOR_RANGE> baseRanges;
    std::array<D3D12_STATIC_SAMPLER_DESC, 3> staticSamplers{};
    ComPtr<ID3D12CommandSignature> dispatchSignature;
    bool resourcesAttempted = false, resourcesReady = false, historyValid = false;
    UINT width = 0, height = 0, tilesX = 0, tilesY = 0, previousSlot = 0, lastRayBudget = 0;
    std::string status = "Not selected";
    UINT measuredRays = 0, measuredBudget = 0;
    bool statisticsValid = false;

    static UINT DebugMode() {
        static const UINT mode = [] {
            char value[8]{};
            return GetEnvironmentVariableA("SGE_VRRT_DEBUG", value, sizeof(value))
                ? UINT(std::clamp(atoi(value), 0, 2)) : 0u;
        }();
        return mode;
    }
    static void Transition(ID3D12GraphicsCommandList* cmd, ID3D12Resource* resource,
                           D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = { resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after };
        cmd->ResourceBarrier(1, &barrier);
    }

    void BuildPipelines(const std::string& source, bool bindless) {
        auto& p = pipelines[bindless ? 1 : 0];
        p.attempted = true;
        if (baseRanges.empty()) { status = "Enhanced root signature unavailable"; return; }
        auto ranges = baseRanges;
        D3D12_DESCRIPTOR_RANGE extra{};
        extra.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        extra.NumDescriptors = 6;
        extra.BaseShaderRegister = 101;
        extra.OffsetInDescriptorsFromTableStart = 108;
        ranges.push_back(extra);
        extra.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        extra.NumDescriptors = 8;
        extra.BaseShaderRegister = 20;
        extra.OffsetInDescriptorsFromTableStart = 114;
        ranges.push_back(extra);
        D3D12_ROOT_PARAMETER params[9]{};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor.ShaderRegister = 0;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable = { UINT(ranges.size()), ranges.data() };
        params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[2].Descriptor.ShaderRegister = 90;
        params[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[3].Constants.ShaderRegister = 6;
        params[3].Constants.Num32BitValues = 8;
        for (UINT i = 4; i < 7; ++i) {
            params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
            params[i].Descriptor.ShaderRegister = 16 + i - 4;
        }
        params[7].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[7].Descriptor.ShaderRegister = 100;
        params[8].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[8].Descriptor.ShaderRegister = 19;
        D3D12_ROOT_SIGNATURE_DESC root{};
        root.NumParameters = 9;
        root.pParameters = params;
        root.NumStaticSamplers = 3;
        root.pStaticSamplers = staticSamplers.data();
        if (bindless) root.Flags = D3D12_ROOT_SIGNATURE_FLAG_CBV_SRV_UAV_HEAP_DIRECTLY_INDEXED;
        ComPtr<ID3DBlob> blob, error;
        if (FAILED(D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) ||
            FAILED(g_dx12.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                IID_PPV_ARGS(&p.root)))) {
            status = "Root signature failed; using full-rate Lumen"; return;
        }
        if (!dispatchSignature) {
            D3D12_INDIRECT_ARGUMENT_DESC argument{};
            argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
            D3D12_COMMAND_SIGNATURE_DESC desc{};
            desc.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
            desc.NumArgumentDescs = 1;
            desc.pArgumentDescs = &argument;
            if (FAILED(g_dx12.device->CreateCommandSignature(&desc, nullptr,
                    IID_PPV_ARGS(&dispatchSignature)))) {
                status = "Indirect signature failed; using full-rate Lumen"; return;
            }
        }
        if (!ShaderCacheDX12::DxcAvailable()) { status = "DXC unavailable; using full-rate Lumen"; return; }
        status = "Compiling shaders; using full-rate Lumen";
        const std::string base = "#define SGE_VARIABLE_RATE_GI 1\n" + source;
        const std::string terrain = "#define SGE_TERRAIN_VISIBILITY 1\n" + base;
        p.pending = std::async(std::launch::async,
            [root = p.root, device = g_dx12.device, bindless, base, terrain] {
                SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
                ShaderCacheDX12::MarkBackgroundThread();
                const wchar_t* entries[] = { L"VRReset", L"VRGradientMain", L"VRClassify", L"VRBalance",
                    L"VRCompact", L"VRTraceMain", L"VRAccumulate", L"VRReconstruct", L"main", L"main", L"main" };
                const std::wstring directory = ShaderCacheDX12::ExecutableDirectory() + L"shaders";
                const wchar_t* profile = bindless ? L"cs_6_6" : L"cs_6_5";
                Build build;
                for (UINT i = 0; i < 11; ++i) {
                    const std::string code = i < 8 || i == 9 ? terrain : i == 10
                        ? "#define SGE_TERRAIN_ONLY_RESOLVE 1\n" + terrain : base;
                    ComPtr<ID3DBlob> shader;
                    std::string errors;
                    if (!ShaderCacheDX12::CompileCachedDXC(code, L"visbuf_resolve_cs.hlsl",
                            entries[i], profile, directory, &shader, &errors)) {
                        build.errors += errors + "\n"; continue;
                    }
                    D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
                    desc.pRootSignature = root.Get();
                    desc.CS = { shader->GetBufferPointer(), shader->GetBufferSize() };
                    if (FAILED(device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&build.pso[i]))))
                        build.errors += "VRRT pipeline " + std::to_string(i) + " creation failed\n";
                }
                return build;
            });
    }

    void CreateResources(UINT w, UINT h) {
        resourcesAttempted = true;
        if (w == 0 || h == 0 || UINT64(w) * h > UINT_MAX / 4u) return;
        width = w; height = h;
        tilesX = (w + 7) / 8; tilesY = (h + 7) / 8;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        auto buffer = [&](ComPtr<ID3D12Resource>& output, UINT64 bytes, bool history, const wchar_t* name) {
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width = bytes; desc.Height = 1; desc.DepthOrArraySize = 1;
            desc.MipLevels = 1; desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            if (FAILED(g_dx12.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
                    &desc, history ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                                   : D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                    nullptr, IID_PPV_ARGS(&output)))) return false;
            output->SetName(name); return true;
        };
        UINT64 pixels = UINT64(w) * h;
        for (auto& f : frames) {
            if (!buffer(f.counters, 68 * 4, false, L"VRRT histogram") ||
                !buffer(f.requests, pixels * 8, false, L"VRRT pixel requests") ||
                !buffer(f.samples, pixels * 16, false, L"VRRT raw samples") ||
                !buffer(f.gradients, UINT64(tilesX) * tilesY * 32, false, L"VRRT gradients") ||
                !buffer(f.jobs, pixels * 16, false, L"VRRT compact jobs") ||
                !buffer(f.history, pixels * HistoryStride, true, L"VRRT diffuse history") ||
                !buffer(f.arguments, sizeof(D3D12_DISPATCH_ARGUMENTS), false, L"VRRT indirect arguments")) {
                status = "Buffer allocation failed; using full-rate Lumen"; return;
            }
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = w; desc.Height = h;
            desc.DepthOrArraySize = 1; desc.MipLevels = 1;
            desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            desc.SampleDesc.Count = 1;
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            if (FAILED(g_dx12.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&f.irradiance)))) {
                status = "Irradiance allocation failed; using full-rate Lumen"; return;
            }
            f.irradiance->SetName(L"VRRT reconstructed irradiance");
            D3D12_HEAP_PROPERTIES readback{};
            readback.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC stats{};
            stats.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            stats.Width = 8; stats.Height = 1; stats.DepthOrArraySize = 1;
            stats.MipLevels = 1; stats.SampleDesc.Count = 1;
            stats.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(g_dx12.device->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE,
                    &stats, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&f.statistics)))) {
                status = "Statistics allocation failed; using full-rate Lumen"; return;
            }
            D3D12_DESCRIPTOR_HEAP_DESC table{};
            table.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            table.NumDescriptors = DescriptorCount;
            table.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            if (FAILED(g_dx12.device->CreateDescriptorHeap(&table, IID_PPV_ARGS(&f.heap)))) {
                status = "Descriptor allocation failed; using full-rate Lumen"; return;
            }
            table.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
            if (FAILED(g_dx12.device->CreateDescriptorHeap(&table, IID_PPV_ARGS(&f.staging)))) {
                status = "Descriptor allocation failed; using full-rate Lumen"; return;
            }
        }
        resourcesReady = true;
        status = "Ready";
    }
};
