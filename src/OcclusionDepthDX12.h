#ifndef OCCLUSION_DEPTH_DX12_H
#define OCCLUSION_DEPTH_DX12_H

#include "DX12Core.h"

class OcclusionDepthDX12 {
public:
    ComPtr<ID3D12Resource> previousDepth;
    bool initialized = false;
    bool valid = false;
    UINT width = 0;
    UINT height = 0;
    static constexpr UINT DescriptorSlot = 63;

    bool Init(UINT newWidth, UINT newHeight) {
        width = newWidth;
        height = newHeight;
        valid = false;
        previousDepth.Reset();

        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R32_TYPELESS;
        desc.SampleDesc.Count = 1;
        HRESULT hr = g_dx12.device->CreateCommittedResource(
            &heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&previousDepth));
        if (FAILED(hr)) return false;

        D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_dx12.cbvSrvUavHeap->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += (SIZE_T)DescriptorSlot * g_dx12.cbvSrvUavDescriptorSize;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = DXGI_FORMAT_R32_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;
        g_dx12.device->CreateShaderResourceView(previousDepth.Get(), &srv, cpu);
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

    void Capture(ID3D12GraphicsCommandList* cmd) {
        if (!initialized || !previousDepth || !g_dx12.depthStencilBuffer) return;
        cmd->OMSetRenderTargets(0, nullptr, FALSE, nullptr);

        D3D12_RESOURCE_BARRIER barriers[2] = {};
        barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[0].Transition.pResource = g_dx12.depthStencilBuffer.Get();
        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[1].Transition.pResource = previousDepth.Get();
        barriers[1].Transition.StateBefore = valid
            ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
            : D3D12_RESOURCE_STATE_COPY_DEST;
        barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        barriers[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmd->ResourceBarrier(valid ? 2 : 1, valid ? barriers : &barriers[0]);
        if (!valid) {
            // First frame needs only the source transition; destination already COPY_DEST.
        }
        cmd->CopyResource(previousDepth.Get(), g_dx12.depthStencilBuffer.Get());

        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
        barriers[1].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        cmd->ResourceBarrier(2, barriers);
        valid = true;
    }
};

#endif
