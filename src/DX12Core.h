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
#include <iostream>
#include <vector>
#include <string>
#include <stdexcept>

// DXC (DirectX Shader Compiler) for SM 6.x / Raytracing shaders
#include <dxcapi.h>
#pragma comment(lib, "dxcompiler.lib")

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

using Microsoft::WRL::ComPtr;
using namespace DirectX;

// Number of frames in flight
static const UINT FRAME_COUNT = 2;

// Descriptor heap sizes
static const UINT CBV_SRV_UAV_HEAP_SIZE = 1024;
static const UINT RTV_HEAP_SIZE = 16;
static const UINT DSV_HEAP_SIZE = 8;
static const UINT SAMPLER_HEAP_SIZE = 16;

// DX12 Context structure
struct DX12Context {
    // Device and adapter
    ComPtr<IDXGIFactory6> factory;
    ComPtr<IDXGIAdapter4> adapter;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12Device5> device5;  // For raytracing
    
    // Command infrastructure
    ComPtr<ID3D12CommandQueue> commandQueue;
    ComPtr<ID3D12CommandAllocator> commandAllocators[FRAME_COUNT];
    ComPtr<ID3D12GraphicsCommandList> commandList;
    ComPtr<ID3D12GraphicsCommandList4> commandList4;  // For raytracing
    
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
    bool raytracingSupported = false;
    
    // DXC Compiler for SM 6.x / Raytracing shaders
    ComPtr<IDxcUtils> dxcUtils;
    ComPtr<IDxcCompiler3> dxcCompiler;
    ComPtr<IDxcIncludeHandler> dxcIncludeHandler;
    bool dxcInitialized = false;
    
    // Descriptor heap allocation tracking
    UINT cbvSrvUavHeapOffset = 0;
    UINT samplerHeapOffset = 0;
};

// Global DX12 context
inline DX12Context g_dx12;

// Initialize DXC compiler
inline bool InitDXC() {
    if (g_dx12.dxcInitialized) return true;
    
    HRESULT hr = DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&g_dx12.dxcUtils));
    if (FAILED(hr)) {
        std::cerr << "Failed to create DXC Utils" << std::endl;
        return false;
    }
    
    hr = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&g_dx12.dxcCompiler));
    if (FAILED(hr)) {
        std::cerr << "Failed to create DXC Compiler" << std::endl;
        return false;
    }
    
    hr = g_dx12.dxcUtils->CreateDefaultIncludeHandler(&g_dx12.dxcIncludeHandler);
    if (FAILED(hr)) {
        std::cerr << "Failed to create DXC Include Handler" << std::endl;
        return false;
    }
    
    g_dx12.dxcInitialized = true;
    std::cout << "DXC compiler initialized successfully" << std::endl;
    return true;
}

// Compile shader using DXC (for SM 6.x / raytracing)
inline ComPtr<IDxcBlob> CompileShaderDXC(const std::wstring& filePath, const std::wstring& entryPoint, 
                                          const std::wstring& target, bool debug = false) {
    if (!g_dx12.dxcInitialized && !InitDXC()) {
        return nullptr;
    }
    
    // Load source file
    ComPtr<IDxcBlobEncoding> sourceBlob;
    HRESULT hr = g_dx12.dxcUtils->LoadFile(filePath.c_str(), nullptr, &sourceBlob);
    if (FAILED(hr)) {
        std::wcerr << L"Failed to load shader file: " << filePath << std::endl;
        return nullptr;
    }
    
    // Setup compile arguments
    std::vector<LPCWSTR> args;
    args.push_back(filePath.c_str());
    args.push_back(L"-E");
    args.push_back(entryPoint.c_str());
    args.push_back(L"-T");
    args.push_back(target.c_str());
    
    if (debug) {
        args.push_back(L"-Zi");  // Debug info
        args.push_back(L"-Od");  // Disable optimization
    } else {
        args.push_back(L"-O3");  // Max optimization
    }
    
    // For raytracing
    args.push_back(L"-D");
    args.push_back(L"__DXR__");
    
    DxcBuffer sourceBuffer;
    sourceBuffer.Ptr = sourceBlob->GetBufferPointer();
    sourceBuffer.Size = sourceBlob->GetBufferSize();
    sourceBuffer.Encoding = DXC_CP_ACP;
    
    ComPtr<IDxcResult> result;
    hr = g_dx12.dxcCompiler->Compile(&sourceBuffer, args.data(), (UINT32)args.size(),
                                      g_dx12.dxcIncludeHandler.Get(), IID_PPV_ARGS(&result));
    
    // Check for errors
    ComPtr<IDxcBlobUtf8> errors;
    result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errors), nullptr);
    if (errors && errors->GetStringLength() > 0) {
        std::cerr << "Shader compilation errors:\n" << errors->GetStringPointer() << std::endl;
    }
    
    HRESULT status;
    result->GetStatus(&status);
    if (FAILED(status)) {
        std::wcerr << L"Shader compilation failed: " << filePath << std::endl;
        return nullptr;
    }
    
    ComPtr<IDxcBlob> shaderBlob;
    result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&shaderBlob), nullptr);
    
    return shaderBlob;
}

// Compile raytracing library shader
inline ComPtr<IDxcBlob> CompileRaytracingLibrary(const std::wstring& filePath, bool debug = false) {
    return CompileShaderDXC(filePath, L"", L"lib_6_3", debug);
}

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

// Move to next frame
inline void MoveToNextFrame() {
    const UINT64 currentFenceValue = g_dx12.fenceValues[g_dx12.frameIndex];
    ThrowIfFailed(g_dx12.commandQueue->Signal(g_dx12.fence.Get(), currentFenceValue));
    
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
    
#ifdef _DEBUG
    // Enable debug layer
    ComPtr<ID3D12Debug> debugController;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController)))) {
        debugController->EnableDebugLayer();
        dxgiFactoryFlags |= DXGI_CREATE_FACTORY_DEBUG;
    }
#endif
    
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
        
        if (SUCCEEDED(D3D12CreateDevice(adapter1.Get(), D3D_FEATURE_LEVEL_12_0, _uuidof(ID3D12Device), nullptr))) {
            adapter1.As(&g_dx12.adapter);
            break;
        }
    }
    
    if (!g_dx12.adapter) {
        std::cerr << "No DX12 compatible adapter found" << std::endl;
        return false;
    }
    
    // Create device
    ThrowIfFailed(D3D12CreateDevice(g_dx12.adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&g_dx12.device)));
    
    // Check for raytracing support
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5 = {};
    if (SUCCEEDED(g_dx12.device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5)))) {
        if (options5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_0) {
            g_dx12.raytracingSupported = true;
            g_dx12.device->QueryInterface(IID_PPV_ARGS(&g_dx12.device5));
            std::cout << "DXR (DirectX Raytracing) supported!" << std::endl;
        }
    }
    if (!g_dx12.raytracingSupported) {
        std::cout << "DXR not supported, falling back to compute-based GI" << std::endl;
    }
    
    // Create command queue
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    ThrowIfFailed(g_dx12.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&g_dx12.commandQueue)));
    
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
    depthDesc.Format = DXGI_FORMAT_D32_FLOAT;
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
    
    // Query raytracing command list interface
    if (g_dx12.raytracingSupported) {
        g_dx12.commandList->QueryInterface(IID_PPV_ARGS(&g_dx12.commandList4));
    }
    
    // Create fence
    ThrowIfFailed(g_dx12.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_dx12.fence)));
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
    
    std::cout << "DirectX 12 initialized successfully" << std::endl;
    
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
    depthDesc.Format = DXGI_FORMAT_D32_FLOAT;
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
    
    // Present
    UINT syncInterval = 0; // VSync off for uncapped framerate
    UINT presentFlags = g_dx12.tearingSupported ? DXGI_PRESENT_ALLOW_TEARING : 0;
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

