#pragma once
#include "ShaderCacheDX12.h"
#include "DX12Core.h"
#include "ShaderDX12.h" // for UploadBuffer<T>
#include <fstream>
#include <sstream>
#include <algorithm>

// Generates a full mip chain for an already-uploaded (mip 0 filled) DEFAULT-heap
// texture using a compute shader box-filter downsample, one dispatch per mip level.
class MipGenerator {
public:
    ComPtr<ID3D12RootSignature> rootSig;
    ComPtr<ID3D12PipelineState> pso;
    ComPtr<ID3D12DescriptorHeap> srvUavHeap; // shader-visible; each texture's dispatches get their own slice
    ComPtr<ID3D12CommandAllocator> graphicsHandoffAllocator;
    ComPtr<ID3D12GraphicsCommandList> graphicsHandoffList;
    ComPtr<ID3D12Fence> graphicsHandoffFence;
    UINT64 graphicsHandoffFenceValue = 0;
    UINT nextDescriptorSlot = 0;
    bool loaded = false;
    struct PendingMip {
        ComPtr<ID3D12Resource> texture;
        UINT width = 0;
        UINT height = 0;
        UINT16 mipLevels = 1;
    };
    std::vector<PendingMip> pending;

    // Total heap size: 2 descriptors per mip-level dispatch. All GenerateMips calls
    // made before the recording command list is executed share this heap, so it
    // must be large enough to cover every texture loaded in one batch (e.g. a
    // whole GLB's worth of materials) without any two calls' slots overlapping -
    // reusing/resetting the offset per call was corrupting earlier textures'
    // dispatches once the GPU actually ran them.
    static const UINT MAX_DESCRIPTORS = 4096;

    // A whole level can enqueue thousands of mip dispatches. Keeping them in
    // one command list gives the driver no short completion boundary and can
    // trip Windows' GPU watchdog on content-heavy levels. The texel limit caps
    // shader work while the dispatch limit also bounds command-list length.
    static constexpr UINT MAX_BATCH_DISPATCHES = 128;
    static constexpr UINT64 MAX_BATCH_DESTINATION_TEXELS =
        16ull * 1024ull * 1024ull;

    bool Init() {
        std::ifstream csFile("shaders/generate_mips_cs.hlsl");
        if (!csFile.is_open()) {
            std::cerr << "Failed to open generate_mips_cs.hlsl" << std::endl;
            return false;
        }
        std::stringstream ss;
        ss << csFile.rdbuf();
        std::string csCode = ss.str();

        UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
        compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
        compileFlags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
        ComPtr<ID3DBlob> csBlob, errorBlob;
        HRESULT hr = ShaderCacheDX12::CompileCached(csCode.c_str(), csCode.length(), "generate_mips_cs.hlsl",
            nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "cs_5_0",
            compileFlags, 0, &csBlob, &errorBlob);
        if (FAILED(hr)) {
            if (errorBlob) std::cerr << "MipGen CS error: " << (char*)errorBlob->GetBufferPointer() << std::endl;
            return false;
        }

        // Root sig: b0 CBV (mip constants), descriptor table [t0 SRV, u0 UAV]
        D3D12_DESCRIPTOR_RANGE ranges[2] = {};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 1;
        ranges[0].BaseShaderRegister = 0;
        ranges[0].OffsetInDescriptorsFromTableStart = 0;

        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 1;
        ranges[1].BaseShaderRegister = 0;
        ranges[1].OffsetInDescriptorsFromTableStart = 1;

        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor.ShaderRegister = 0;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 2;
        params[1].DescriptorTable.pDescriptorRanges = ranges;
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_STATIC_SAMPLER_DESC sampler = {};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.MipLODBias = 0.0f;
        sampler.MinLOD = 0.0f;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.ShaderRegister = 0;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rootSigDesc = {};
        rootSigDesc.NumParameters = 2;
        rootSigDesc.pParameters = params;
        rootSigDesc.NumStaticSamplers = 1;
        rootSigDesc.pStaticSamplers = &sampler;

        ComPtr<ID3DBlob> sigBlob;
        errorBlob.Reset();
        hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errorBlob);
        if (FAILED(hr)) {
            if (errorBlob) std::cerr << "MipGen root sig error: " << (char*)errorBlob->GetBufferPointer() << std::endl;
            return false;
        }
        hr = g_dx12.device->CreateRootSignature(0, sigBlob->GetBufferPointer(), sigBlob->GetBufferSize(),
            IID_PPV_ARGS(&rootSig));
        if (FAILED(hr)) return false;

        D3D12_COMPUTE_PIPELINE_STATE_DESC cpsoDesc = {};
        cpsoDesc.pRootSignature = rootSig.Get();
        cpsoDesc.CS = { csBlob->GetBufferPointer(), csBlob->GetBufferSize() };
        hr = g_dx12.device->CreateComputePipelineState(&cpsoDesc, IID_PPV_ARGS(&pso));
        if (FAILED(hr)) {
            std::cerr << "Failed to create MipGen compute PSO" << std::endl;
            return false;
        }

        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NumDescriptors = MAX_DESCRIPTORS;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = g_dx12.device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&srvUavHeap));
        if (FAILED(hr)) return false;

        hr = g_dx12.device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&graphicsHandoffAllocator));
        if (FAILED(hr)) return false;
        hr = g_dx12.device->CreateCommandList(
            0, D3D12_COMMAND_LIST_TYPE_DIRECT, graphicsHandoffAllocator.Get(), nullptr,
            IID_PPV_ARGS(&graphicsHandoffList));
        if (FAILED(hr)) return false;
        if (FAILED(graphicsHandoffList->Close())) return false;
        hr = g_dx12.device->CreateFence(
            0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&graphicsHandoffFence));
        if (FAILED(hr)) return false;

        loaded = true;
        return true;
    }

    struct alignas(256) MipConstants {
        UINT dstWidth;
        UINT dstHeight;
        float texelSizeX;
        float texelSizeY;
    };

    // Texture arrives in NON_PIXEL_SHADER_RESOURCE so every state used by the
    // dedicated compute list is legal for that queue.
    void GenerateMips(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* texture,
                       UINT width, UINT height, UINT16 mipLevels) {
        if (!loaded || mipLevels <= 1) return;
        (void)cmdList;
        PendingMip request;
        request.texture = texture;
        request.width = width;
        request.height = height;
        request.mipLevels = mipLevels;
        pending.push_back(std::move(request));
    }

    // Runs queued downsampling on the compute queue. Future direct submissions
    // wait on its fence, while CPU initialization continues.
    void FlushPending() {
        if (!loaded || pending.empty()) return;

        size_t batchBegin = 0;
        size_t batchCount = 0;
        while (batchBegin < pending.size()) {
            size_t batchEnd = batchBegin;
            UINT batchDispatches = 0;
            UINT64 batchDestinationTexels = 0;
            while (batchEnd < pending.size()) {
                const PendingMip& request = pending[batchEnd];
                const UINT requestDispatches = request.mipLevels - 1;
                const UINT64 requestDestinationTexels =
                    EstimateDestinationTexels(request);
                const bool batchHasWork = batchEnd != batchBegin;
                const bool exceedsLimit = batchHasWork &&
                    (batchDispatches + requestDispatches >
                         MAX_BATCH_DISPATCHES ||
                     static_cast<UINT64>(batchDispatches +
                         requestDispatches) * 2ull > MAX_DESCRIPTORS ||
                     batchDestinationTexels + requestDestinationTexels >
                         MAX_BATCH_DESTINATION_TEXELS);
                if (exceedsLimit) break;

                batchDispatches += requestDispatches;
                batchDestinationTexels += requestDestinationTexels;
                ++batchEnd;
            }

            ID3D12GraphicsCommandList* cmdList = BeginComputeCommands();
            // BeginComputeCommands waits for the preceding batch, so descriptor
            // slices are no longer in flight and the fixed heap can be reused.
            nextDescriptorSlot = 0;
            for (size_t i = batchBegin; i < batchEnd; ++i) {
                const PendingMip& request = pending[i];
                RecordMips(cmdList, request.texture.Get(), request.width,
                           request.height, request.mipLevels);
            }
            SubmitComputeCommands();
            batchBegin = batchEnd;
            ++batchCount;
        }

        std::cout << "MipGenerator: " << pending.size() << " textures in "
                  << batchCount << " compute batches\n";

        // Compute queues cannot transition to PIXEL_SHADER_RESOURCE. Perform
        // that final ownership/state handoff on a small direct command list.
        WaitForFenceCPU(graphicsHandoffFence.Get(), graphicsHandoffFenceValue);
        ThrowIfFailed(graphicsHandoffAllocator->Reset());
        ThrowIfFailed(graphicsHandoffList->Reset(graphicsHandoffAllocator.Get(), nullptr));
        std::vector<D3D12_RESOURCE_BARRIER> barriers(pending.size());
        for (size_t i = 0; i < pending.size(); ++i) {
            barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barriers[i].Transition.pResource = pending[i].texture.Get();
            barriers[i].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            barriers[i].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            barriers[i].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        }
        graphicsHandoffList->ResourceBarrier((UINT)barriers.size(), barriers.data());
        ThrowIfFailed(graphicsHandoffList->Close());
        ID3D12CommandList* handoffLists[] = { graphicsHandoffList.Get() };
        g_dx12.commandQueue->ExecuteCommandLists(1, handoffLists);
        const UINT64 value = ++graphicsHandoffFenceValue;
        ThrowIfFailed(g_dx12.commandQueue->Signal(graphicsHandoffFence.Get(), value));
        pending.clear();
    }

private:
    static UINT64 EstimateDestinationTexels(const PendingMip& request) {
        UINT width = request.width;
        UINT height = request.height;
        UINT64 texels = 0;
        for (UINT16 level = 1; level < request.mipLevels; ++level) {
            width = (std::max)(1u, width / 2);
            height = (std::max)(1u, height / 2);
            texels += static_cast<UINT64>(width) * height;
        }
        return texels;
    }

    void RecordMips(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* texture,
                    UINT width, UINT height, UINT16 mipLevels) {

        // Allocated on the heap and intentionally leaked for the app's lifetime:
        // the upload buffer must stay alive until the GPU executes these commands,
        // and mip generation is a one-time load-time cost per texture.
        auto* cb = new UploadBuffer<MipConstants>();
        cb->Create(mipLevels - 1);

        UINT descSize = g_dx12.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        ID3D12DescriptorHeap* heaps[] = { srvUavHeap.Get() };
        cmdList->SetDescriptorHeaps(1, heaps);
        cmdList->SetPipelineState(pso.Get());
        cmdList->SetComputeRootSignature(rootSig.Get());

        UINT srcW = width, srcH = height;
        for (UINT16 level = 1; level < mipLevels; level++) {
            if (nextDescriptorSlot + 2 > MAX_DESCRIPTORS) {
                std::cerr << "MipGenerator: descriptor heap exhausted, skipping remaining mips\n";
                break;
            }
            UINT dstW = std::max(1u, srcW / 2);
            UINT dstH = std::max(1u, srcH / 2);

            // Source is already compute-readable. Only destination changes.
            D3D12_RESOURCE_BARRIER preBarrier = {};
            preBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            preBarrier.Transition.pResource = texture;
            preBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            preBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            preBarrier.Transition.Subresource = level;
            cmdList->ResourceBarrier(1, &preBarrier);

            D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = srvUavHeap->GetCPUDescriptorHandleForHeapStart();
            UINT tableOffset = nextDescriptorSlot;
            nextDescriptorSlot += 2;
            cpuHandle.ptr += (SIZE_T)tableOffset * descSize;

            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Texture2D.MostDetailedMip = level - 1;
            srvDesc.Texture2D.MipLevels = 1;
            g_dx12.device->CreateShaderResourceView(texture, &srvDesc, cpuHandle);

            D3D12_CPU_DESCRIPTOR_HANDLE uavCpu = cpuHandle;
            uavCpu.ptr += descSize;
            D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
            uavDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            uavDesc.Texture2D.MipSlice = level;
            g_dx12.device->CreateUnorderedAccessView(texture, nullptr, &uavDesc, uavCpu);

            MipConstants mc = {};
            mc.dstWidth = dstW;
            mc.dstHeight = dstH;
            mc.texelSizeX = 1.0f / (float)srcW;
            mc.texelSizeY = 1.0f / (float)srcH;
            cb->CopyData(level - 1, mc);

            cmdList->SetComputeRootConstantBufferView(0, cb->GetGPUAddress(level - 1));
            D3D12_GPU_DESCRIPTOR_HANDLE tableStart = srvUavHeap->GetGPUDescriptorHandleForHeapStart();
            tableStart.ptr += (UINT64)tableOffset * descSize;
            cmdList->SetComputeRootDescriptorTable(1, tableStart);

            UINT groupsX = (dstW + 7) / 8;
            UINT groupsY = (dstH + 7) / 8;
            cmdList->Dispatch(groupsX, groupsY, 1);

            // UAV barrier so the next dispatch's SRV read sees this write
            D3D12_RESOURCE_BARRIER uavBarrier = {};
            uavBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            uavBarrier.UAV.pResource = texture;
            cmdList->ResourceBarrier(1, &uavBarrier);

            D3D12_RESOURCE_BARRIER postBarrier = {};
            postBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            postBarrier.Transition.pResource = texture;
            postBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            postBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            postBarrier.Transition.Subresource = level;
            cmdList->ResourceBarrier(1, &postBarrier);

            srcW = dstW;
            srcH = dstH;
        }

        // Restore the engine's own descriptor heaps: SetDescriptorHeaps above
        // replaced them for the compute dispatches, and leaving our heap bound
        // corrupts every SetGraphicsRootDescriptorTable call recorded afterwards
        // in this command list (driver access violation once executed).
        ID3D12DescriptorHeap* mainHeaps[] = { g_dx12.cbvSrvUavHeap.Get(), g_dx12.samplerHeap.Get() };
        cmdList->SetDescriptorHeaps(2, mainHeaps);
    }
};

extern MipGenerator g_mipGen;
