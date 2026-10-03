#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "MipGenerator.h"

#include <iostream>

namespace {
int failures = 0;
#define CHECK(condition) do { if (!(condition)) { \
    std::cerr << __LINE__ << " CHECK failed: " #condition << '\n'; \
    ++failures; } } while (false)

ComPtr<ID3D12Resource> CreateTexture() {
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = 4;
    desc.Height = 4;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 3;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> texture;
    ThrowIfFailed(g_dx12.device->CreateCommittedResource(&heap,
        D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
        IID_PPV_ARGS(&texture)));
    return texture;
}
}

int main() {
    try {
        ComPtr<IDXGIFactory4> factory;
        ComPtr<IDXGIAdapter> warp;
        ThrowIfFailed(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        ThrowIfFailed(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)));
        ThrowIfFailed(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0,
            IID_PPV_ARGS(&g_dx12.device)));
        D3D12_COMMAND_QUEUE_DESC queue{};
        queue.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        ThrowIfFailed(g_dx12.device->CreateCommandQueue(&queue,
            IID_PPV_ARGS(&g_dx12.commandQueue)));
        queue.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        ThrowIfFailed(g_dx12.device->CreateCommandQueue(&queue,
            IID_PPV_ARGS(&g_dx12.computeQueue)));
        ThrowIfFailed(g_dx12.device->CreateCommandAllocator(queue.Type,
            IID_PPV_ARGS(&g_dx12.computeAllocator)));
        ThrowIfFailed(g_dx12.device->CreateCommandList(0, queue.Type,
            g_dx12.computeAllocator.Get(), nullptr,
            IID_PPV_ARGS(&g_dx12.computeCommandList)));
        ThrowIfFailed(g_dx12.computeCommandList->Close());
        ThrowIfFailed(g_dx12.device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
            IID_PPV_ARGS(&g_dx12.computeFence)));
        g_dx12.fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!g_dx12.fenceEvent) return 2;

        D3D12_DESCRIPTOR_HEAP_DESC descriptors{};
        descriptors.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        descriptors.NumDescriptors = 4;
        descriptors.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        ThrowIfFailed(g_dx12.device->CreateDescriptorHeap(&descriptors,
            IID_PPV_ARGS(&g_dx12.cbvSrvUavHeap)));
        descriptors.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
        descriptors.NumDescriptors = 1;
        ThrowIfFailed(g_dx12.device->CreateDescriptorHeap(&descriptors,
            IID_PPV_ARGS(&g_dx12.samplerHeap)));

        MipGenerator generator;
        if (!generator.Init()) return 2;
        ComPtr<ID3D12Fence> gate;
        ThrowIfFailed(g_dx12.device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
            IID_PPV_ARGS(&gate)));
        // Hold the handoff on the GPU while the importer drops its last owner.
        // This makes the early-release window deterministic on software WARP.
        ThrowIfFailed(g_dx12.commandQueue->Wait(gate.Get(), 1));
        auto texture = CreateTexture();
        generator.GenerateMips(nullptr, texture.Get(), 4, 4, 3);
        texture.Reset();
        generator.FlushPending();
        CHECK(generator.pending.empty());
        CHECK(generator.inFlight.size() == 1);
        generator.ReleaseCompletedTextures();
        CHECK(generator.inFlight.size() == 1);

        ThrowIfFailed(gate->Signal(1));
        WaitForFenceCPU(generator.graphicsHandoffFence.Get(),
            generator.graphicsHandoffFenceValue);
        generator.ReleaseCompletedTextures();
        CHECK(generator.inFlight.empty());
        CHECK(g_dx12.device->GetDeviceRemovedReason() == S_OK);

        // A subsequent flush must retire the previous owners without putting
        // the old requests back into pending or generating their mips twice.
        texture = CreateTexture();
        generator.GenerateMips(nullptr, texture.Get(), 4, 4, 3);
        texture.Reset();
        generator.FlushPending();
        WaitForFenceCPU(generator.graphicsHandoffFence.Get(),
            generator.graphicsHandoffFenceValue);
        texture = CreateTexture();
        generator.GenerateMips(nullptr, texture.Get(), 4, 4, 3);
        texture.Reset();
        generator.FlushPending();
        CHECK(generator.pending.empty());
        WaitForFenceCPU(generator.graphicsHandoffFence.Get(),
            generator.graphicsHandoffFenceValue);
        generator.ReleaseCompletedTextures();
        CHECK(generator.inFlight.empty());
        CHECK(g_dx12.device->GetDeviceRemovedReason() == S_OK);
        CloseHandle(g_dx12.fenceEvent);
        g_dx12.fenceEvent = nullptr;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
    return failures ? 1 : 0;
}
