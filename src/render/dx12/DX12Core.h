#ifndef DX12_CORE_H
#define DX12_CORE_H

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <algorithm>
#include <iostream>
#include <fstream>
#include <cstdlib>
#include <vector>
#include <string>
#include <stdexcept>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;
using namespace DirectX;

// Number of frames in flight
static const UINT FRAME_COUNT = 2;

// Descriptor heap sizes.
// CBV_SRV_UAV: slots 0..63 are reserved for global resources (shadow map, DDGI);
// material descriptors are handed out from 64 upward, 3 per textured draw. Smoke
// billboards are textured draws too, so a couple of bursts can each cost hundreds
// of descriptors -- keep plenty of headroom. Overflow is clamped in ShaderDX12,
// but a heap this size means we don't hit the clamp and start dropping sprites.
static const UINT CBV_SRV_UAV_HEAP_SIZE = 32768;
static const UINT RTV_HEAP_SIZE = 16;
static const UINT DSV_HEAP_SIZE = 8;
static const UINT SAMPLER_HEAP_SIZE = 16;

// DX12 Context structure
struct DX12Context {
    // Device and adapter
    ComPtr<IDXGIFactory6> factory;
    ComPtr<IDXGIAdapter4> adapter;
    ComPtr<ID3D12Device> device;
    
    // Command infrastructure
    ComPtr<ID3D12CommandQueue> commandQueue;
    ComPtr<ID3D12CommandAllocator> commandAllocators[FRAME_COUNT];
    ComPtr<ID3D12GraphicsCommandList> commandList;
    ComPtr<ID3D12CommandQueue> computeQueue;
    ComPtr<ID3D12CommandAllocator> computeAllocator;              // legacy shared allocator
    ComPtr<ID3D12CommandAllocator> computeAllocators[FRAME_COUNT]; // async path, one per frame slot
    ComPtr<ID3D12GraphicsCommandList> computeCommandList;
    ComPtr<ID3D12CommandQueue> copyQueue;
    ComPtr<ID3D12CommandAllocator> copyAllocators[FRAME_COUNT];
    ComPtr<ID3D12GraphicsCommandList> copyCommandList;
    
    // Swap chain
    ComPtr<IDXGISwapChain4> swapChain;
    ComPtr<ID3D12Resource> renderTargets[FRAME_COUNT];
    ComPtr<ID3D12Resource> depthStencilBuffer;
    
    // Descriptor heaps
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    ComPtr<ID3D12DescriptorHeap> dsvHeap;
    ComPtr<ID3D12DescriptorHeap> cbvSrvUavHeap;
    ComPtr<ID3D12DescriptorHeap> samplerHeap;
    
    UINT rtvDescriptorSize = 0;
    UINT dsvDescriptorSize = 0;
    UINT cbvSrvUavDescriptorSize = 0;
    UINT samplerDescriptorSize = 0;
    
    // Synchronization
    ComPtr<ID3D12Fence> fence;
    UINT64 fenceValues[FRAME_COUNT] = {};
    HANDLE fenceEvent = nullptr;
    ComPtr<ID3D12Fence> computeFence;
    UINT64 computeFenceValue = 0;
    // Last value signaled by SubmitComputeFrame(); the consumer side waits on
    // this via GraphicsWaitCompute() rather than at submit time.
    UINT64 computeCompletedFenceValue = 0;
    // Escape hatch for bisecting async-compute artefacts: when true,
    // SubmitComputeFrame behaves like the legacy path and makes the graphics
    // queue wait immediately. SGE_SERIAL_COMPUTE=1 sets this at startup.
    bool forceSerialCompute = false;
    ComPtr<ID3D12Fence> copyFence;
    UINT64 copyFenceValue = 0;
    UINT64 copyAllocatorFenceValues[FRAME_COUNT] = {};
    UINT64 latestCopyFenceValue = 0;
    UINT64 lastDirectFenceValue = 0;
    
    // Frame management
    UINT frameIndex = 0;
    UINT currentBackBufferIndex = 0;
    
    // Screen dimensions
    UINT screenWidth = 0;
    UINT screenHeight = 0;
    
    // Viewport and scissor rect
    D3D12_VIEWPORT viewport = {};
    D3D12_RECT scissorRect = {};
    
    bool initialized = false;
    bool tearingSupported = false;
    bool debugLayerEnabled = false;   // D3D12 validation active (env/_DEBUG)
    bool dredEnabled = false;         // DRED breadcrumbs/page faults active
    // Present sync interval: 0 uncapped, 1 every vblank, 2+ divides the refresh
    // rate. Mirrored here from the scene settings so Present() stays free of
    // any scene dependency.
    UINT syncInterval = 0;
    
    // Descriptor heap allocation tracking
    UINT cbvSrvUavHeapOffset = 0;
    UINT samplerHeapOffset = 0;
};

// Global DX12 context
inline DX12Context g_dx12;

// Helper function to check HRESULT
inline void ThrowIfFailed(HRESULT hr, const char* message = "DX12 Error") {
    if (FAILED(hr)) {
        std::cerr << message << " HRESULT: 0x" << std::hex << hr << std::dec << std::endl;
        throw std::runtime_error(message);
    }
}

// Get descriptor handle
inline D3D12_CPU_DESCRIPTOR_HANDLE GetCPUDescriptorHandle(
    ID3D12DescriptorHeap* heap, UINT descriptorSize, UINT index) {
    D3D12_CPU_DESCRIPTOR_HANDLE handle = heap->GetCPUDescriptorHandleForHeapStart();
    handle.ptr += (SIZE_T)(index * descriptorSize);
    return handle;
}

inline D3D12_GPU_DESCRIPTOR_HANDLE GetGPUDescriptorHandle(
    ID3D12DescriptorHeap* heap, UINT descriptorSize, UINT index) {
    D3D12_GPU_DESCRIPTOR_HANDLE handle = heap->GetGPUDescriptorHandleForHeapStart();
    handle.ptr += (SIZE_T)(index * descriptorSize);
    return handle;
}

// Wait for GPU to finish
inline void WaitForGPU() {
    if (!g_dx12.commandQueue || !g_dx12.fence) return;
    
    const UINT64 currentFenceValue = g_dx12.fenceValues[g_dx12.frameIndex];
    ThrowIfFailed(g_dx12.commandQueue->Signal(g_dx12.fence.Get(), currentFenceValue));
    
    if (g_dx12.fence->GetCompletedValue() < currentFenceValue) {
        ThrowIfFailed(g_dx12.fence->SetEventOnCompletion(currentFenceValue, g_dx12.fenceEvent));
        WaitForSingleObjectEx(g_dx12.fenceEvent, INFINITE, FALSE);
    }
    
    g_dx12.fenceValues[g_dx12.frameIndex]++;
}

// Drain EVERY frame slot, not just the current one.
//
// WaitForGPU() signals and waits on fenceValues[frameIndex] alone, so it only
// proves the current slot is idle -- work submitted from the other FRAME_COUNT-1
// slots can still be executing. That is fine for ordinary per-frame pacing, but
// it is not enough before final-releasing a resource: a staging heap freed while
// an older slot's CopyTextureRegion still reads it page-faults the GPU
// (DEVICE_HUNG / "referenced by GPU operations in-flight" corruption).
//
// Signal one value above the highest across all slots and wait for it. Because
// the queue executes in submission order, that value retiring means every
// previously submitted command list on this queue has completed.
inline void WaitForGPUAllFrames() {
    if (!g_dx12.commandQueue || !g_dx12.fence) return;

    UINT64 target = 0;
    for (UINT i = 0; i < FRAME_COUNT; ++i)
        target = (std::max)(target, g_dx12.fenceValues[i]);
    ++target;

    ThrowIfFailed(g_dx12.commandQueue->Signal(g_dx12.fence.Get(), target));
    if (g_dx12.fence->GetCompletedValue() < target) {
        ThrowIfFailed(g_dx12.fence->SetEventOnCompletion(target, g_dx12.fenceEvent));
        WaitForSingleObjectEx(g_dx12.fenceEvent, INFINITE, FALSE);
    }

    // Keep every slot at/above the drained value so later per-frame waits and
    // MoveToNextFrame() never signal a value the fence has already passed.
    for (UINT i = 0; i < FRAME_COUNT; ++i)
        g_dx12.fenceValues[i] = target + 1;
}

inline void WaitForFenceCPU(ID3D12Fence* fence, UINT64 value) {
    if (!fence || value == 0 || fence->GetCompletedValue() >= value) return;
    ThrowIfFailed(fence->SetEventOnCompletion(value, g_dx12.fenceEvent));
    WaitForSingleObjectEx(g_dx12.fenceEvent, INFINITE, FALSE);
}

inline ID3D12GraphicsCommandList* BeginComputeCommands() {
    WaitForFenceCPU(g_dx12.computeFence.Get(), g_dx12.computeFenceValue);
    ThrowIfFailed(g_dx12.computeAllocator->Reset());
    ThrowIfFailed(g_dx12.computeCommandList->Reset(g_dx12.computeAllocator.Get(), nullptr));
    return g_dx12.computeCommandList.Get();
}

// ---- Async compute primitives (Phase 2a) ----
//
// The legacy Begin/Submit pair above serialises: Begin blocks the CPU on the
// compute fence, Submit makes the graphics queue wait immediately. The pair
// below keeps the same list but schedules against per-frame allocators and a
// fence pair, so compute work can overlap the graphics queue:
//
//   BeginComputeFrame()           -- no CPU wait; frame pacing already
//                                    guarantees this slot's allocator retired
//   ComputeWaitGraphics(value)    -- compute waits on a graphics signal when
//                                    it consumes a graphics-queued resource
//   SubmitComputeFrame()          -- close/execute/signal; graphics does NOT
//                                    wait here
//   GraphicsWaitCompute()         -- graphics waits at the actual consumer
//
// forceSerialCompute collapses Submit back to the legacy behaviour for
// bisection.

inline ID3D12GraphicsCommandList* BeginComputeFrame() {
    ThrowIfFailed(g_dx12.computeAllocators[g_dx12.frameIndex]->Reset());
    ThrowIfFailed(g_dx12.computeCommandList->Reset(
        g_dx12.computeAllocators[g_dx12.frameIndex].Get(), nullptr));
    return g_dx12.computeCommandList.Get();
}

// Make the compute queue wait until the graphics queue reaches `value`.
// Pass the value the graphics queue signaled after its producer pass.
inline void ComputeWaitGraphics(UINT64 graphicsFenceValue) {
    if (graphicsFenceValue == 0) return;
    ThrowIfFailed(g_dx12.computeQueue->Wait(g_dx12.fence.Get(), graphicsFenceValue));
}

// Close + execute + signal. Graphics keeps running unless forceSerialCompute
// is set; consumers call GraphicsWaitCompute() where they need the result.
inline UINT64 SubmitComputeFrame() {
    ThrowIfFailed(g_dx12.computeCommandList->Close());
    ID3D12CommandList* lists[] = { g_dx12.computeCommandList.Get() };
    g_dx12.computeQueue->ExecuteCommandLists(1, lists);
    const UINT64 value = ++g_dx12.computeFenceValue;
    ThrowIfFailed(g_dx12.computeQueue->Signal(g_dx12.computeFence.Get(), value));
    g_dx12.computeCompletedFenceValue = value;
    if (g_dx12.forceSerialCompute)
        ThrowIfFailed(g_dx12.commandQueue->Wait(g_dx12.computeFence.Get(), value));
    return value;
}

// Graphics queue waits on the last submitted compute batch. Call at the
// point the graphics queue first samples a compute-produced resource.
inline void GraphicsWaitCompute() {
    if (g_dx12.computeCompletedFenceValue == 0 || g_dx12.forceSerialCompute) return;
    ThrowIfFailed(g_dx12.commandQueue->Wait(
        g_dx12.computeFence.Get(), g_dx12.computeCompletedFenceValue));
}

// Legacy: Submit async compute and make subsequent direct work wait on the GPU.
inline UINT64 SubmitComputeCommands() {
    ThrowIfFailed(g_dx12.computeCommandList->Close());
    ID3D12CommandList* lists[] = { g_dx12.computeCommandList.Get() };
    g_dx12.computeQueue->ExecuteCommandLists(1, lists);
    const UINT64 value = ++g_dx12.computeFenceValue;
    ThrowIfFailed(g_dx12.computeQueue->Signal(g_dx12.computeFence.Get(), value));
    ThrowIfFailed(g_dx12.commandQueue->Wait(g_dx12.computeFence.Get(), value));
    return value;
}

// Move to next frame
inline void MoveToNextFrame() {
    const UINT64 currentFenceValue = g_dx12.fenceValues[g_dx12.frameIndex];
    ThrowIfFailed(g_dx12.commandQueue->Signal(g_dx12.fence.Get(), currentFenceValue));
    g_dx12.lastDirectFenceValue = currentFenceValue;
    
    g_dx12.frameIndex = g_dx12.swapChain->GetCurrentBackBufferIndex();
    
    if (g_dx12.fence->GetCompletedValue() < g_dx12.fenceValues[g_dx12.frameIndex]) {
        ThrowIfFailed(g_dx12.fence->SetEventOnCompletion(g_dx12.fenceValues[g_dx12.frameIndex], g_dx12.fenceEvent));
        WaitForSingleObjectEx(g_dx12.fenceEvent, INFINITE, FALSE);
    }
    
    g_dx12.fenceValues[g_dx12.frameIndex] = currentFenceValue + 1;
}

// Initialize DX12
inline bool InitDX12(HWND hwnd, UINT width, UINT height) {
    g_dx12.screenWidth = width;
    g_dx12.screenHeight = height;
    
    UINT dxgiFactoryFlags = 0;

    // Debug layer: always on in _DEBUG, and opt-in for Release via the env var
    // SGE_D3D12_DEBUG=1 so a shipped build can still validate a crash on demand.
    bool enableD3D12Debug = false;
#ifdef _DEBUG
    enableD3D12Debug = true;
#endif
    {
        char envValue[8] = {};
        size_t envLen = 0;
        if (getenv_s(&envLen, envValue, sizeof(envValue), "SGE_D3D12_DEBUG") == 0 &&
            envLen > 0 && envValue[0] == '1')
            enableD3D12Debug = true;
    }
    ComPtr<ID3D12Debug> debugController;
    if (enableD3D12Debug &&
        SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
        debugController->EnableDebugLayer();
        dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
    }
    g_dx12.debugLayerEnabled = enableD3D12Debug;

    // Device Removed Extended Data. A DEVICE_HUNG/DEVICE_REMOVED only tells us
    // *that* the GPU stopped; the auto-breadcrumbs tell us which command-list
    // operation was in flight, and the page-fault record tells us whether a
    // resource was freed out from under in-flight work.
    //
    // Deliberately NOT gated on enableD3D12Debug: the debug layer slows loading
    // enough to shift TDR timing and can mask the very race we are chasing.
    // Must be set before D3D12CreateDevice to take effect.
    {
        // On by default. DRED's cost is negligible next to a crash that cannot
        // be diagnosed: without it a device-removed report is just an HRESULT,
        // with no indication of which GPU operation died. Set
        // SGE_D3D12_DRED=0 to opt out.
        char dredValue[8] = {};
        size_t dredLen = 0;
        bool enableDRED = true;
        if (getenv_s(&dredLen, dredValue, sizeof(dredValue), "SGE_D3D12_DRED") == 0 &&
            dredLen > 0 && dredValue[0] == '0')
            enableDRED = false;

        ComPtr<ID3D12DeviceRemovedExtendedDataSettings1> dredSettings;
        if (enableDRED &&
            SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dredSettings)))) {
            dredSettings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dredSettings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            g_dx12.dredEnabled = true;
        }
    }

    // Create DXGI Factory
    ThrowIfFailed(CreateDXGIFactory2(dxgiFactoryFlags, IID_PPV_ARGS(&g_dx12.factory)));
    
    // Check for tearing support
    ComPtr<IDXGIFactory5> factory5;
    if (SUCCEEDED(g_dx12.factory.As(&factory5))) {
        BOOL tearingSupport = FALSE;
        if (SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearingSupport, sizeof(tearingSupport)))) {
            g_dx12.tearingSupported = (tearingSupport == TRUE);
        }
    }
    
    // Find hardware adapter
    ComPtr<IDXGIAdapter1> adapter1;
    for (UINT i = 0; g_dx12.factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter1)) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 desc;
        adapter1->GetDesc1(&desc);
        
        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;

        // Virtual display adapters (Parsec, Sunshine, IDD mirror drivers) are
        // not flagged SOFTWARE, so the loop above happily picks one when it
        // enumerates first. They have no real 3D engine, and a heavy load-time
        // submission on one hangs the display driver (nvlddmkm event 153 /
        // LiveKernelEvent 141) instead of rendering. Skip anything with no
        // dedicated video memory -- a real discrete GPU always reports some.
        if (desc.DedicatedVideoMemory == 0) {
            std::wcerr << L"Skipping adapter with no dedicated VRAM: "
                       << desc.Description << L"\n";
            continue;
        }

        if (SUCCEEDED(D3D12CreateDevice(adapter1.Get(), D3D_FEATURE_LEVEL_12_0, _uuidof(ID3D12Device), nullptr))) {
            adapter1.As(&g_dx12.adapter);
            std::wcerr << L"Selected GPU adapter: " << desc.Description
                       << L" (" << (desc.DedicatedVideoMemory >> 20)
                       << L" MB VRAM)\n";
            break;
        }
    }

    if (!g_dx12.adapter) {
        std::cerr << "No DX12 compatible adapter found" << std::endl;
        return false;
    }
    
    // Create device
    ThrowIfFailed(D3D12CreateDevice(g_dx12.adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&g_dx12.device)));

    // Disable break-on-error so an offscreen automated run doesn't halt in a
    // debugger trap. Register a callback (Win10 1903+) that writes validation
    // messages straight to a log file, since this app's own console can't be
    // captured externally - this is the only reliable way to see them. Runs
    // whenever the debug layer is on (always in _DEBUG, or SGE_D3D12_DEBUG=1).
    if (enableD3D12Debug) {
        ComPtr<ID3D12InfoQueue1> infoQueue1;
        if (SUCCEEDED(g_dx12.device.As(&infoQueue1))) {
            infoQueue1->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, FALSE);
            infoQueue1->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
            DWORD cookie = 0;
            infoQueue1->RegisterMessageCallback(
                [](D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity,
                   D3D12_MESSAGE_ID, LPCSTR pDescription, void*) {
                    if (severity <= D3D12_MESSAGE_SEVERITY_WARNING) {
                        std::ofstream log("d3d12_debug.log", std::ios::app);
                        log << pDescription << "\n";
                    }
                },
                D3D12_MESSAGE_CALLBACK_FLAG_NONE, nullptr, &cookie);
        }
    }

    // Create command queue
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    ThrowIfFailed(g_dx12.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&g_dx12.commandQueue)));

    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
    ThrowIfFailed(g_dx12.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&g_dx12.computeQueue)));
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
    ThrowIfFailed(g_dx12.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&g_dx12.copyQueue)));
    
    // Create swap chain
    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.BufferCount = FRAME_COUNT;
    swapChainDesc.Width = width;
    swapChainDesc.Height = height;
    swapChainDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.Flags = g_dx12.tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    
    ComPtr<IDXGISwapChain1> swapChain1;
    ThrowIfFailed(g_dx12.factory->CreateSwapChainForHwnd(
        g_dx12.commandQueue.Get(), hwnd, &swapChainDesc, nullptr, nullptr, &swapChain1));
    
    // Disable Alt+Enter fullscreen
    ThrowIfFailed(g_dx12.factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER));
    
    ThrowIfFailed(swapChain1.As(&g_dx12.swapChain));
    g_dx12.frameIndex = g_dx12.swapChain->GetCurrentBackBufferIndex();
    
    // Create descriptor heaps
    // RTV heap
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = RTV_HEAP_SIZE;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(g_dx12.device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&g_dx12.rtvHeap)));
    g_dx12.rtvDescriptorSize = g_dx12.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    
    // DSV heap
    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
    dsvHeapDesc.NumDescriptors = DSV_HEAP_SIZE;
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    dsvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    ThrowIfFailed(g_dx12.device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&g_dx12.dsvHeap)));
    g_dx12.dsvDescriptorSize = g_dx12.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    
    // CBV/SRV/UAV heap (shader visible)
    D3D12_DESCRIPTOR_HEAP_DESC cbvSrvUavHeapDesc = {};
    cbvSrvUavHeapDesc.NumDescriptors = CBV_SRV_UAV_HEAP_SIZE;
    cbvSrvUavHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    cbvSrvUavHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(g_dx12.device->CreateDescriptorHeap(&cbvSrvUavHeapDesc, IID_PPV_ARGS(&g_dx12.cbvSrvUavHeap)));
    g_dx12.cbvSrvUavDescriptorSize = g_dx12.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    
    // Sampler heap (shader visible)
    D3D12_DESCRIPTOR_HEAP_DESC samplerHeapDesc = {};
    samplerHeapDesc.NumDescriptors = SAMPLER_HEAP_SIZE;
    samplerHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    samplerHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ThrowIfFailed(g_dx12.device->CreateDescriptorHeap(&samplerHeapDesc, IID_PPV_ARGS(&g_dx12.samplerHeap)));
    g_dx12.samplerDescriptorSize = g_dx12.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
    
    // Create render target views
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_dx12.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < FRAME_COUNT; i++) {
        ThrowIfFailed(g_dx12.swapChain->GetBuffer(i, IID_PPV_ARGS(&g_dx12.renderTargets[i])));
        g_dx12.device->CreateRenderTargetView(g_dx12.renderTargets[i].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += g_dx12.rtvDescriptorSize;
    }
    
    // Create depth stencil buffer
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    
    D3D12_RESOURCE_DESC depthDesc = {};
    depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depthDesc.Width = width;
    depthDesc.Height = height;
    depthDesc.DepthOrArraySize = 1;
    depthDesc.MipLevels = 1;
    depthDesc.Format = DXGI_FORMAT_R32_TYPELESS;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    
    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = DXGI_FORMAT_D32_FLOAT;
    clearValue.DepthStencil.Depth = 1.0f;
    clearValue.DepthStencil.Stencil = 0;
    
    ThrowIfFailed(g_dx12.device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &depthDesc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearValue,
        IID_PPV_ARGS(&g_dx12.depthStencilBuffer)));
    
    // Create DSV
    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dsvDesc.Flags = D3D12_DSV_FLAG_NONE;
    g_dx12.device->CreateDepthStencilView(g_dx12.depthStencilBuffer.Get(), &dsvDesc, 
        g_dx12.dsvHeap->GetCPUDescriptorHandleForHeapStart());
    
    // Create command allocators
    for (UINT i = 0; i < FRAME_COUNT; i++) {
        ThrowIfFailed(g_dx12.device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_dx12.commandAllocators[i])));
    }
    
    // Create command list
    ThrowIfFailed(g_dx12.device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_dx12.commandAllocators[0].Get(), nullptr,
        IID_PPV_ARGS(&g_dx12.commandList)));
    ThrowIfFailed(g_dx12.commandList->Close());

    ThrowIfFailed(g_dx12.device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&g_dx12.computeAllocator)));
    for (UINT i = 0; i < FRAME_COUNT; ++i) {
        ThrowIfFailed(g_dx12.device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_COMPUTE,
            IID_PPV_ARGS(&g_dx12.computeAllocators[i])));
    }
    ThrowIfFailed(g_dx12.device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_COMPUTE, g_dx12.computeAllocator.Get(), nullptr,
        IID_PPV_ARGS(&g_dx12.computeCommandList)));
    ThrowIfFailed(g_dx12.computeCommandList->Close());
    g_dx12.forceSerialCompute =
        GetEnvironmentVariableW(L"SGE_SERIAL_COMPUTE", nullptr, 0) != 0;

    for (UINT i = 0; i < FRAME_COUNT; ++i) {
        ThrowIfFailed(g_dx12.device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_COPY, IID_PPV_ARGS(&g_dx12.copyAllocators[i])));
    }
    ThrowIfFailed(g_dx12.device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_COPY, g_dx12.copyAllocators[0].Get(), nullptr,
        IID_PPV_ARGS(&g_dx12.copyCommandList)));
    ThrowIfFailed(g_dx12.copyCommandList->Close());
    
    // Create fence
    ThrowIfFailed(g_dx12.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_dx12.fence)));
    ThrowIfFailed(g_dx12.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_dx12.computeFence)));
    ThrowIfFailed(g_dx12.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_dx12.copyFence)));
    g_dx12.fenceValues[g_dx12.frameIndex] = 1;
    
    g_dx12.fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!g_dx12.fenceEvent) {
        std::cerr << "Failed to create fence event" << std::endl;
        return false;
    }
    
    // Setup viewport and scissor rect
    g_dx12.viewport.TopLeftX = 0.0f;
    g_dx12.viewport.TopLeftY = 0.0f;
    g_dx12.viewport.Width = (float)width;
    g_dx12.viewport.Height = (float)height;
    g_dx12.viewport.MinDepth = 0.0f;
    g_dx12.viewport.MaxDepth = 1.0f;
    
    g_dx12.scissorRect.left = 0;
    g_dx12.scissorRect.top = 0;
    g_dx12.scissorRect.right = (LONG)width;
    g_dx12.scissorRect.bottom = (LONG)height;
    
    g_dx12.initialized = true;
    
    // Report the chosen GPU here rather than at selection time: this runs after
    // the log file is open, so it actually lands in logs/GraphicEngine.log.
    // Which adapter won matters -- picking a virtual display adapter over the
    // real GPU is the difference between rendering and a driver TDR.
    {
        DXGI_ADAPTER_DESC1 chosen = {};
        if (g_dx12.adapter && SUCCEEDED(g_dx12.adapter->GetDesc1(&chosen))) {
            char name[128] = {};
            WideCharToMultiByte(CP_UTF8, 0, chosen.Description, -1,
                                name, sizeof(name) - 1, nullptr, nullptr);
            std::cout << "GPU adapter: " << name << " ("
                      << (chosen.DedicatedVideoMemory >> 20) << " MB VRAM)"
                      << std::endl;
        }
    }

    std::cout << "DirectX 12 initialized: direct + compute + copy queues" << std::endl;
    
    return true;
}

// Resize DX12 resources
inline void ResizeDX12(UINT width, UINT height) {
    if (!g_dx12.initialized || width == 0 || height == 0) return;
    
    // Wait for GPU to finish
    WaitForGPU();
    
    // Release render targets
    for (UINT i = 0; i < FRAME_COUNT; i++) {
        g_dx12.renderTargets[i].Reset();
        g_dx12.fenceValues[i] = g_dx12.fenceValues[g_dx12.frameIndex];
    }
    g_dx12.depthStencilBuffer.Reset();
    
    // Resize swap chain
    DXGI_SWAP_CHAIN_DESC1 desc;
    g_dx12.swapChain->GetDesc1(&desc);
    ThrowIfFailed(g_dx12.swapChain->ResizeBuffers(
        FRAME_COUNT, width, height, desc.Format, desc.Flags));
    
    g_dx12.frameIndex = g_dx12.swapChain->GetCurrentBackBufferIndex();
    
    // Recreate render targets
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_dx12.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    for (UINT i = 0; i < FRAME_COUNT; i++) {
        ThrowIfFailed(g_dx12.swapChain->GetBuffer(i, IID_PPV_ARGS(&g_dx12.renderTargets[i])));
        g_dx12.device->CreateRenderTargetView(g_dx12.renderTargets[i].Get(), nullptr, rtvHandle);
        rtvHandle.ptr += g_dx12.rtvDescriptorSize;
    }
    
    // Recreate depth stencil buffer
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    
    D3D12_RESOURCE_DESC depthDesc = {};
    depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    depthDesc.Width = width;
    depthDesc.Height = height;
    depthDesc.DepthOrArraySize = 1;
    depthDesc.MipLevels = 1;
    depthDesc.Format = DXGI_FORMAT_R32_TYPELESS;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    
    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = DXGI_FORMAT_D32_FLOAT;
    clearValue.DepthStencil.Depth = 1.0f;
    clearValue.DepthStencil.Stencil = 0;
    
    ThrowIfFailed(g_dx12.device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &depthDesc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearValue,
        IID_PPV_ARGS(&g_dx12.depthStencilBuffer)));
    
    // Recreate DSV
    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dsvDesc.Flags = D3D12_DSV_FLAG_NONE;
    g_dx12.device->CreateDepthStencilView(g_dx12.depthStencilBuffer.Get(), &dsvDesc, 
        g_dx12.dsvHeap->GetCPUDescriptorHandleForHeapStart());
    
    // Update viewport and scissor rect
    g_dx12.screenWidth = width;
    g_dx12.screenHeight = height;
    g_dx12.viewport.Width = (float)width;
    g_dx12.viewport.Height = (float)height;
    g_dx12.scissorRect.right = (LONG)width;
    g_dx12.scissorRect.bottom = (LONG)height;
}

// Cleanup DX12
inline void CleanupDX12() {
    WaitForGPU();
    if (g_dx12.computeQueue && g_dx12.computeFence) {
        const UINT64 value = ++g_dx12.computeFenceValue;
        ThrowIfFailed(g_dx12.computeQueue->Signal(g_dx12.computeFence.Get(), value));
        WaitForFenceCPU(g_dx12.computeFence.Get(), value);
    }
    if (g_dx12.copyQueue && g_dx12.copyFence) {
        const UINT64 value = ++g_dx12.copyFenceValue;
        ThrowIfFailed(g_dx12.copyQueue->Signal(g_dx12.copyFence.Get(), value));
        WaitForFenceCPU(g_dx12.copyFence.Get(), value);
    }
    
    if (g_dx12.fenceEvent) {
        CloseHandle(g_dx12.fenceEvent);
        g_dx12.fenceEvent = nullptr;
    }
    
    g_dx12.initialized = false;
}

// Begin frame - prepare for rendering
inline void BeginFrame() {
    ThrowIfFailed(g_dx12.commandAllocators[g_dx12.frameIndex]->Reset());
    ThrowIfFailed(g_dx12.commandList->Reset(g_dx12.commandAllocators[g_dx12.frameIndex].Get(), nullptr));
    
    // Transition render target to render target state
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = g_dx12.renderTargets[g_dx12.frameIndex].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    g_dx12.commandList->ResourceBarrier(1, &barrier);
    
    // Set descriptor heaps
    ID3D12DescriptorHeap* heaps[] = { g_dx12.cbvSrvUavHeap.Get(), g_dx12.samplerHeap.Get() };
    g_dx12.commandList->SetDescriptorHeaps(2, heaps);
    
    // Set viewport and scissor rect
    g_dx12.commandList->RSSetViewports(1, &g_dx12.viewport);
    g_dx12.commandList->RSSetScissorRects(1, &g_dx12.scissorRect);
}

// Prints and clears any queued D3D12 validation-layer messages (Debug builds only)
inline void DumpDX12DebugMessages() {
#ifdef _DEBUG
    ComPtr<ID3D12InfoQueue> infoQueue;
    if (FAILED(g_dx12.device.As(&infoQueue))) return;

    UINT64 count = infoQueue->GetNumStoredMessages();
    std::ofstream debugLog("d3d12_debug.log", std::ios::app);
    for (UINT64 i = 0; i < count; i++) {
        SIZE_T msgLen = 0;
        infoQueue->GetMessage(i, nullptr, &msgLen);
        if (msgLen == 0) continue;
        std::vector<char> buf(msgLen);
        D3D12_MESSAGE* msg = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
        infoQueue->GetMessage(i, msg, &msgLen);
        std::cerr << "[D3D12] " << msg->pDescription << std::endl;
        if (debugLog)
            debugLog << "[D3D12] " << msg->pDescription << '\n';
    }
    infoQueue->ClearStoredMessages();
#endif
}

// Decode a DEVICE_REMOVED/DEVICE_HUNG into something actionable.
//
// GetDeviceRemovedReason() only gives the category. DRED adds the two things
// that actually identify the culprit: the auto-breadcrumb trail (which GPU op
// each command list reached before it stopped) and the page-fault record (which
// virtual address was touched, and whether it belongs to a recently freed
// allocation -- the signature of a resource released while still in flight).
//
// Unlike DumpDX12DebugMessages this is NOT _DEBUG-only: the hang reproduces in
// Release, which is exactly where we need the trail.
inline void DumpDX12DeviceRemovedData(std::ostream& out) {
    if (!g_dx12.device) return;

    ComPtr<ID3D12DeviceRemovedExtendedData1> dred;
    if (FAILED(g_dx12.device.As(&dred))) {
        out << "[DRED] unavailable (not enabled? set SGE_D3D12_DRED=1)\n";
        return;
    }

    static const char* kOpNames[] = {
        "SetMarker", "BeginEvent", "EndEvent", "DrawInstanced",
        "DrawIndexedInstanced", "ExecuteIndirect", "Dispatch",
        "CopyBufferRegion", "CopyTextureRegion", "CopyResource", "CopyTiles",
        "ResolveSubresource", "ClearRenderTargetView",
        "ClearUnorderedAccessView", "ClearDepthStencilView",
        "ResourceBarrier", "ExecuteBundle", "Present", "ResolveQueryData",
        "BeginSubmission", "EndSubmission", "DecodeFrame", "ProcessFrames",
        "AtomicCopyBufferUint", "AtomicCopyBufferUint64", "ResolveSubresourceRegion",
        "WriteBufferImmediate", "DecodeFrame1", "SetProtectedResourceSession",
        "DecodeFrame2", "ProcessFrames1", "BuildRaytracingAccelerationStructure",
        "EmitRaytracingAccelerationStructurePostbuildInfo",
        "CopyRaytracingAccelerationStructure", "DispatchRays",
        "InitializeMetaCommand", "ExecuteMetaCommand", "EstimateMotion",
        "ResolveMotionVectorHeap", "SetPipelineState1",
        "InitializeExtensionCommand", "ExecuteExtensionCommand", "DispatchMesh",
        "EncodeFrame", "ResolveEncoderOutputMetadata",
    };
    auto opName = [](UINT op) -> const char* {
        return op < _countof(kOpNames) ? kOpNames[op] : "Unknown";
    };

    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 breadcrumbs = {};
    if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput1(&breadcrumbs))) {
        out << "[DRED] --- auto-breadcrumbs ---\n";
        for (const auto* node = breadcrumbs.pHeadAutoBreadcrumbNode;
             node; node = node->pNext) {
            const UINT executed =
                node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
            // A node whose executed count equals its op count finished cleanly;
            // the first node that stopped short is the hang site.
            if (executed == node->BreadcrumbCount) continue;

            out << "[DRED] cmdlist=\""
                << (node->pCommandListDebugNameA ? node->pCommandListDebugNameA : "?")
                << "\" queue=\""
                << (node->pCommandQueueDebugNameA ? node->pCommandQueueDebugNameA : "?")
                << "\" completed " << executed << " of "
                << node->BreadcrumbCount << " ops\n";

            // Print a window around the stall so the failing op has context.
            const UINT first = executed > 4 ? executed - 4 : 0;
            const UINT last = (std::min)(node->BreadcrumbCount, executed + 4);
            for (UINT i = first; i < last; ++i) {
                out << "[DRED]   " << (i == executed ? ">> " : "   ") << i
                    << ' ' << opName(node->pCommandHistory[i]) << '\n';
            }
        }
    }

    D3D12_DRED_PAGE_FAULT_OUTPUT pageFault = {};
    if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&pageFault))) {
        out << "[DRED] --- page fault VA = 0x" << std::hex
            << pageFault.PageFaultVA << std::dec << " ---\n";
        auto dumpAllocations = [&](const char* label,
                                   const D3D12_DRED_ALLOCATION_NODE* head) {
            for (const auto* n = head; n; n = n->pNext)
                out << "[DRED]   " << label << ": "
                    << (n->ObjectNameA ? n->ObjectNameA : "<unnamed>") << '\n';
        };
        dumpAllocations("existing", pageFault.pHeadExistingAllocationNode);
        dumpAllocations("recently freed", pageFault.pHeadRecentFreedAllocationNode);
    }
    out.flush();
}

// End frame - present
inline void EndFrame() {
    // Transition render target to present state
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = g_dx12.renderTargets[g_dx12.frameIndex].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    g_dx12.commandList->ResourceBarrier(1, &barrier);
    
    ThrowIfFailed(g_dx12.commandList->Close());
    
    // Execute command list
    ID3D12CommandList* commandLists[] = { g_dx12.commandList.Get() };
    g_dx12.commandQueue->ExecuteCommandLists(1, commandLists);
    
    // Present. ALLOW_TEARING is only legal on an unsynchronised present, so it
    // has to come off the moment vsync is on -- passing both fails Present.
    const UINT syncInterval = g_dx12.syncInterval;
    const UINT presentFlags =
        (syncInterval == 0 && g_dx12.tearingSupported) ? DXGI_PRESENT_ALLOW_TEARING : 0;
    ThrowIfFailed(g_dx12.swapChain->Present(syncInterval, presentFlags));
    
    MoveToNextFrame();
}

// Clear render target and depth stencil
inline void ClearRenderTarget(const float* clearColor) {
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = GetCPUDescriptorHandle(
        g_dx12.rtvHeap.Get(), g_dx12.rtvDescriptorSize, g_dx12.frameIndex);
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = g_dx12.dsvHeap->GetCPUDescriptorHandleForHeapStart();
    
    g_dx12.commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
    g_dx12.commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    
    g_dx12.commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);
}

#endif // DX12_CORE_H

