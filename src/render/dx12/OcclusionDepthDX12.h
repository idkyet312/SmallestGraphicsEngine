#ifndef OCCLUSION_DEPTH_DX12_H
#define OCCLUSION_DEPTH_DX12_H

#include "ShaderCacheDX12.h"
#include "DX12Core.h"
#include <cmath>
#include <sstream>

class OcclusionDepthDX12 {
public:
    ComPtr<ID3D12Resource> previousDepth;
    bool initialized = false;
    bool valid = false;
    bool copyPending = false;
    UINT pendingFrame = 0;
    UINT width = 0;
    UINT height = 0;
    UINT mipCount = 1;
    bool cameraHistoryValid = false;
    XMFLOAT3 previousCameraPosition = {};
    XMFLOAT3 previousCameraForward = {};
    static constexpr UINT DescriptorSlot = 63;
    ComPtr<ID3D12RootSignature> hzbRootSignature;
    ComPtr<ID3D12PipelineState> hzbPipeline;
    ComPtr<ID3D12DescriptorHeap> hzbHeap;

    bool Init(UINT newWidth, UINT newHeight) {
        width = newWidth;
        height = newHeight;
        valid = false;
        copyPending = false;
        cameraHistoryValid = false;
        previousDepth.Reset();
        mipCount = 1;
        for (UINT extent = (std::max)(newWidth, newHeight); extent > 1; extent >>= 1)
            ++mipCount;

        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = static_cast<UINT16>(mipCount);
        desc.Format = DXGI_FORMAT_R32_TYPELESS;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        HRESULT hr = g_dx12.device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(&previousDepth));
        if (FAILED(hr)) return false;

        D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_dx12.cbvSrvUavHeap->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += (SIZE_T)DescriptorSlot * g_dx12.cbvSrvUavDescriptorSize;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = DXGI_FORMAT_R32_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = mipCount;
        g_dx12.device->CreateShaderResourceView(previousDepth.Get(), &srv, cpu);
        if (!InitHZBPipeline() || !CreateHZBDescriptors()) return false;
        initialized = true;
        return true;
    }

    void Resize(UINT newWidth, UINT newHeight) {
        Init(newWidth, newHeight);
    }

    D3D12_GPU_DESCRIPTOR_HANDLE GetGPUHandle() const {
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_dx12.cbvSrvUavHeap->GetGPUDescriptorHandleForHeapStart();
        gpu.ptr += (UINT64)DescriptorSlot * g_dx12.cbvSrvUavDescriptorSize;
        return gpu;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE GetCPUHandle() const {
        D3D12_CPU_DESCRIPTOR_HANDLE cpu =
            g_dx12.cbvSrvUavHeap->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += (SIZE_T)DescriptorSlot * g_dx12.cbvSrvUavDescriptorSize;
        return cpu;
    }

    UINT GetMipCount() const { return mipCount; }

    void InvalidateCameraHistory() { cameraHistoryValid = false; }

    bool CanUseHistory(const XMFLOAT3& position, const XMFLOAT3& forward) {
        bool continuous = cameraHistoryValid;
        if (continuous) {
            const XMVECTOR delta = XMLoadFloat3(&position) - XMLoadFloat3(&previousCameraPosition);
            const float distanceSq = XMVectorGetX(XMVector3LengthSq(delta));
            const float facing = XMVectorGetX(XMVector3Dot(
                XMVector3Normalize(XMLoadFloat3(&forward)),
                XMVector3Normalize(XMLoadFloat3(&previousCameraForward))));
            continuous = distanceSq < 4.0f && facing > 0.80f;
        }
        previousCameraPosition = position;
        previousCameraForward = forward;
        cameraHistoryValid = true;
        return valid && continuous;
    }

    // Direct queue: release resources through COMMON for copy-queue ownership.
    void PrepareCapture(ID3D12GraphicsCommandList* cmd) {
        if (!initialized || !previousDepth || !g_dx12.depthStencilBuffer) return;
        cmd->OMSetRenderTargets(0, nullptr, FALSE, nullptr);

        D3D12_RESOURCE_BARRIER barriers[2] = {};
        barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[0].Transition.pResource = g_dx12.depthStencilBuffer.Get();
        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[1].Transition.pResource = previousDepth.Get();
        barriers[1].Transition.StateBefore = valid
            ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
            : D3D12_RESOURCE_STATE_COMMON;
        barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
        barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd->ResourceBarrier(valid ? 2 : 1, barriers);
        copyPending = true;
        pendingFrame = g_dx12.frameIndex;
    }

    // Copy queue: runs after direct rendering, without stalling CPU.
    void SubmitCopy() {
        if (!copyPending) return;
        WaitForFenceCPU(g_dx12.copyFence.Get(),
                        g_dx12.copyAllocatorFenceValues[pendingFrame]);
        ThrowIfFailed(g_dx12.copyAllocators[pendingFrame]->Reset());
        ThrowIfFailed(g_dx12.copyCommandList->Reset(
            g_dx12.copyAllocators[pendingFrame].Get(), nullptr));

        // COPY command lists require cross-queue resources in COMMON. The copy
        // implicitly promotes them to COPY_SOURCE/COPY_DEST and they decay back
        // to COMMON when this command list completes. Explicit legacy barriers
        // here produce invalid enhanced-barrier layouts on current drivers and
        // can remove/hang the device under a heavy stress-test load.
        D3D12_TEXTURE_COPY_LOCATION destination = {};
        destination.pResource = previousDepth.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destination.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION source = {};
        source.pResource = g_dx12.depthStencilBuffer.Get();
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        source.SubresourceIndex = 0;
        g_dx12.copyCommandList->CopyTextureRegion(
            &destination, 0, 0, 0, &source, nullptr);

        ThrowIfFailed(g_dx12.copyCommandList->Close());

        ThrowIfFailed(g_dx12.copyQueue->Wait(
            g_dx12.fence.Get(), g_dx12.lastDirectFenceValue));
        ID3D12CommandList* lists[] = { g_dx12.copyCommandList.Get() };
        g_dx12.copyQueue->ExecuteCommandLists(1, lists);
        const UINT64 value = ++g_dx12.copyFenceValue;
        ThrowIfFailed(g_dx12.copyQueue->Signal(g_dx12.copyFence.Get(), value));
        g_dx12.copyAllocatorFenceValues[pendingFrame] = value;
        g_dx12.latestCopyFenceValue = value;
        copyPending = false;
        valid = true;
    }

    // Next direct frame: GPU-wait for copy, then restore shader/depth states.
    void FinalizeCapture(ID3D12GraphicsCommandList* cmd) {
        if (!valid || g_dx12.latestCopyFenceValue == 0) return;
        ThrowIfFailed(g_dx12.commandQueue->Wait(
            g_dx12.copyFence.Get(), g_dx12.latestCopyFenceValue));

        std::vector<D3D12_RESOURCE_BARRIER> barriers;
        barriers.reserve(mipCount + 1);
        D3D12_RESOURCE_BARRIER depthBarrier = {};
        depthBarrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        depthBarrier.Transition.pResource = g_dx12.depthStencilBuffer.Get();
        depthBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        depthBarrier.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        depthBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers.push_back(depthBarrier);
        for (UINT mip = 0; mip < mipCount; ++mip) {
            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = previousDepth.Get();
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
            barrier.Transition.StateAfter = mip == 0
                ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            barrier.Transition.Subresource = mip;
            barriers.push_back(barrier);
        }
        cmd->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());

        if (mipCount > 1 && hzbPipeline && hzbRootSignature && hzbHeap) {
            ID3D12DescriptorHeap* heaps[] = { hzbHeap.Get() };
            cmd->SetDescriptorHeaps(1, heaps);
            cmd->SetComputeRootSignature(hzbRootSignature.Get());
            cmd->SetPipelineState(hzbPipeline.Get());
            const UINT descriptorSize = g_dx12.cbvSrvUavDescriptorSize;
            for (UINT mip = 0; mip + 1 < mipCount; ++mip) {
                D3D12_GPU_DESCRIPTOR_HANDLE sourceHandle =
                    hzbHeap->GetGPUDescriptorHandleForHeapStart();
                sourceHandle.ptr += UINT64(mip * 2) * descriptorSize;
                D3D12_GPU_DESCRIPTOR_HANDLE destinationHandle = sourceHandle;
                destinationHandle.ptr += descriptorSize;
                cmd->SetComputeRootDescriptorTable(0, sourceHandle);
                cmd->SetComputeRootDescriptorTable(1, destinationHandle);
                const UINT mipWidth = (std::max)(1u, width >> (mip + 1));
                const UINT mipHeight = (std::max)(1u, height >> (mip + 1));
                cmd->Dispatch((mipWidth + 7) / 8, (mipHeight + 7) / 8, 1);

                D3D12_RESOURCE_BARRIER uav = {};
                uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
                uav.UAV.pResource = previousDepth.Get();
                cmd->ResourceBarrier(1, &uav);
                D3D12_RESOURCE_BARRIER ready = {};
                ready.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                ready.Transition.pResource = previousDepth.Get();
                ready.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
                ready.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
                ready.Transition.Subresource = mip + 1;
                cmd->ResourceBarrier(1, &ready);
            }
            ID3D12DescriptorHeap* mainHeaps[] = {
                g_dx12.cbvSrvUavHeap.Get(), g_dx12.samplerHeap.Get() };
            cmd->SetDescriptorHeaps(2, mainHeaps);
        }
    }

private:
    bool InitHZBPipeline() {
        if (hzbPipeline && hzbRootSignature) return true;
        std::ifstream file("shaders/hzb_cs.hlsl");
        if (!file.is_open()) return false;
        std::stringstream stream;
        stream << file.rdbuf();
        const std::string code = stream.str();
        ComPtr<ID3DBlob> shader;
        ComPtr<ID3DBlob> errors;
        if (FAILED(ShaderCacheDX12::CompileCached(code.data(), code.size(), "shaders/hzb_cs.hlsl",
                nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "cs_5_0",
                D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader, &errors))) {
            if (errors) std::cerr << static_cast<const char*>(errors->GetBufferPointer());
            return false;
        }
        D3D12_DESCRIPTOR_RANGE ranges[2] = {};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 1;
        ranges[0].BaseShaderRegister = 0;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 1;
        ranges[1].BaseShaderRegister = 0;
        D3D12_ROOT_PARAMETER parameters[2] = {};
        for (UINT i = 0; i < 2; ++i) {
            parameters[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            parameters[i].DescriptorTable.NumDescriptorRanges = 1;
            parameters[i].DescriptorTable.pDescriptorRanges = &ranges[i];
            parameters[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }
        D3D12_ROOT_SIGNATURE_DESC root = {};
        root.NumParameters = 2;
        root.pParameters = parameters;
        ComPtr<ID3DBlob> signature;
        if (FAILED(D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1,
                &signature, &errors)) ||
            FAILED(g_dx12.device->CreateRootSignature(0, signature->GetBufferPointer(),
                signature->GetBufferSize(), IID_PPV_ARGS(&hzbRootSignature)))) return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline = {};
        pipeline.pRootSignature = hzbRootSignature.Get();
        pipeline.CS = { shader->GetBufferPointer(), shader->GetBufferSize() };
        return SUCCEEDED(g_dx12.device->CreateComputePipelineState(
            &pipeline, IID_PPV_ARGS(&hzbPipeline)));
    }

    bool CreateHZBDescriptors() {
        if (mipCount <= 1) return true;
        D3D12_DESCRIPTOR_HEAP_DESC heap = {};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.NumDescriptors = (mipCount - 1) * 2;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(g_dx12.device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&hzbHeap))))
            return false;
        D3D12_CPU_DESCRIPTOR_HANDLE handle = hzbHeap->GetCPUDescriptorHandleForHeapStart();
        for (UINT mip = 0; mip + 1 < mipCount; ++mip) {
            D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Format = DXGI_FORMAT_R32_FLOAT;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Texture2D.MostDetailedMip = mip;
            srv.Texture2D.MipLevels = 1;
            g_dx12.device->CreateShaderResourceView(previousDepth.Get(), &srv, handle);
            handle.ptr += g_dx12.cbvSrvUavDescriptorSize;
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
            uav.Format = DXGI_FORMAT_R32_FLOAT;
            uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            uav.Texture2D.MipSlice = mip + 1;
            g_dx12.device->CreateUnorderedAccessView(
                previousDepth.Get(), nullptr, &uav, handle);
            handle.ptr += g_dx12.cbvSrvUavDescriptorSize;
        }
        return true;
    }
};

#endif
