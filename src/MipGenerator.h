#pragma once
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
        HRESULT hr = D3DCompile(csCode.c_str(), csCode.length(), "generate_mips_cs.hlsl",
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

        loaded = true;
        return true;
    }

    struct alignas(256) MipConstants {
        UINT dstWidth;
        UINT dstHeight;
        float texelSizeX;
        float texelSizeY;
    };

    // texture must already have mip 0 uploaded and be in PIXEL_SHADER_RESOURCE state.
    // Leaves the texture back in PIXEL_SHADER_RESOURCE state when done.
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
        ID3D12GraphicsCommandList* cmdList = BeginComputeCommands();
        for (const PendingMip& request : pending)
            RecordMips(cmdList, request.texture.Get(), request.width,
                       request.height, request.mipLevels);
        pending.clear();
        SubmitComputeCommands();
    }

private:
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

            // Transition source mip to SRV-readable, dest mip to UAV-writable
            D3D12_RESOURCE_BARRIER preBarriers[2] = {};
            preBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            preBarriers[0].Transition.pResource = texture;
            preBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            preBarriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            preBarriers[0].Transition.Subresource = level - 1;

            preBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            preBarriers[1].Transition.pResource = texture;
            preBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            preBarriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            preBarriers[1].Transition.Subresource = level;
            cmdList->ResourceBarrier(2, preBarriers);

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

            // Transition both subresources back to PIXEL_SHADER_RESOURCE
            D3D12_RESOURCE_BARRIER postBarriers[2] = {};
            postBarriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            postBarriers[0].Transition.pResource = texture;
            postBarriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            postBarriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            postBarriers[0].Transition.Subresource = level - 1;

            postBarriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            postBarriers[1].Transition.pResource = texture;
            postBarriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            postBarriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            postBarriers[1].Transition.Subresource = level;
            cmdList->ResourceBarrier(2, postBarriers);

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
