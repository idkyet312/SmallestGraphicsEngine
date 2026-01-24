#ifndef DX11_CORE_H
#define DX11_CORE_H

#include <d3d11.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <string>
#include <vector>
#include <iostream>
#include <fstream>
#include <sstream>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dxgi.lib")

using namespace DirectX;
using Microsoft::WRL::ComPtr;

// Global DX11 context
struct DX11Context {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> swapChain;
    ComPtr<ID3D11RenderTargetView> renderTargetView;
    ComPtr<ID3D11DepthStencilView> depthStencilView;
    ComPtr<ID3D11Texture2D> depthStencilBuffer;
    ComPtr<ID3D11RasterizerState> rasterizerState;
    ComPtr<ID3D11RasterizerState> wireframeState;
    ComPtr<ID3D11RasterizerState> noCullState;
    ComPtr<ID3D11DepthStencilState> depthStencilState;
    ComPtr<ID3D11DepthStencilState> depthDisabledState;
    ComPtr<ID3D11BlendState> blendState;
    ComPtr<ID3D11SamplerState> samplerState;
    ComPtr<ID3D11SamplerState> shadowSamplerState;
    
    unsigned int screenWidth;
    unsigned int screenHeight;
    bool initialized = false;
};

// Global instance
extern DX11Context g_dx11;

inline bool InitDX11(HWND hwnd, unsigned int width, unsigned int height) {
    g_dx11.screenWidth = width;
    g_dx11.screenHeight = height;
    
    // Swap chain description
    DXGI_SWAP_CHAIN_DESC swapChainDesc = {};
    swapChainDesc.BufferCount = 2;
    swapChainDesc.BufferDesc.Width = width;
    swapChainDesc.BufferDesc.Height = height;
    swapChainDesc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    swapChainDesc.BufferDesc.RefreshRate.Numerator = 0;
    swapChainDesc.BufferDesc.RefreshRate.Denominator = 1;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.OutputWindow = hwnd;
    swapChainDesc.SampleDesc.Count = 1;
    swapChainDesc.SampleDesc.Quality = 0;
    swapChainDesc.Windowed = TRUE;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    
    D3D_FEATURE_LEVEL featureLevel = D3D_FEATURE_LEVEL_11_0;
    UINT flags = 0;
#ifdef _DEBUG
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif
    
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
        &featureLevel, 1, D3D11_SDK_VERSION,
        &swapChainDesc, &g_dx11.swapChain, &g_dx11.device,
        nullptr, &g_dx11.context);
    
    if (FAILED(hr)) {
        std::cerr << "Failed to create D3D11 device and swap chain" << std::endl;
        return false;
    }
    
    // Create render target view
    ComPtr<ID3D11Texture2D> backBuffer;
    hr = g_dx11.swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr)) return false;
    
    hr = g_dx11.device->CreateRenderTargetView(backBuffer.Get(), nullptr, &g_dx11.renderTargetView);
    if (FAILED(hr)) return false;
    
    // Create depth stencil texture
    D3D11_TEXTURE2D_DESC depthDesc = {};
    depthDesc.Width = width;
    depthDesc.Height = height;
    depthDesc.MipLevels = 1;
    depthDesc.ArraySize = 1;
    depthDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.SampleDesc.Quality = 0;
    depthDesc.Usage = D3D11_USAGE_DEFAULT;
    depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    
    hr = g_dx11.device->CreateTexture2D(&depthDesc, nullptr, &g_dx11.depthStencilBuffer);
    if (FAILED(hr)) return false;
    
    hr = g_dx11.device->CreateDepthStencilView(g_dx11.depthStencilBuffer.Get(), nullptr, &g_dx11.depthStencilView);
    if (FAILED(hr)) return false;
    
    // Set render target
    g_dx11.context->OMSetRenderTargets(1, g_dx11.renderTargetView.GetAddressOf(), g_dx11.depthStencilView.Get());
    
    // Setup viewport
    D3D11_VIEWPORT viewport = {};
    viewport.TopLeftX = 0;
    viewport.TopLeftY = 0;
    viewport.Width = (float)width;
    viewport.Height = (float)height;
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    g_dx11.context->RSSetViewports(1, &viewport);
    
    // Create rasterizer states
    D3D11_RASTERIZER_DESC rasterDesc = {};
    rasterDesc.FillMode = D3D11_FILL_SOLID;
    rasterDesc.CullMode = D3D11_CULL_BACK;
    rasterDesc.FrontCounterClockwise = FALSE;
    rasterDesc.DepthClipEnable = TRUE;
    g_dx11.device->CreateRasterizerState(&rasterDesc, &g_dx11.rasterizerState);
    
    rasterDesc.FillMode = D3D11_FILL_WIREFRAME;
    g_dx11.device->CreateRasterizerState(&rasterDesc, &g_dx11.wireframeState);
    
    rasterDesc.FillMode = D3D11_FILL_SOLID;
    rasterDesc.CullMode = D3D11_CULL_NONE;
    g_dx11.device->CreateRasterizerState(&rasterDesc, &g_dx11.noCullState);
    
    g_dx11.context->RSSetState(g_dx11.rasterizerState.Get());
    
    // Create depth stencil states
    D3D11_DEPTH_STENCIL_DESC depthStencilDesc = {};
    depthStencilDesc.DepthEnable = TRUE;
    depthStencilDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depthStencilDesc.DepthFunc = D3D11_COMPARISON_LESS;
    g_dx11.device->CreateDepthStencilState(&depthStencilDesc, &g_dx11.depthStencilState);
    
    depthStencilDesc.DepthEnable = FALSE;
    g_dx11.device->CreateDepthStencilState(&depthStencilDesc, &g_dx11.depthDisabledState);
    
    g_dx11.context->OMSetDepthStencilState(g_dx11.depthStencilState.Get(), 0);
    
    // Create sampler state
    D3D11_SAMPLER_DESC samplerDesc = {};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.MipLODBias = 0.0f;
    samplerDesc.MaxAnisotropy = 1;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
    samplerDesc.MinLOD = 0;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    g_dx11.device->CreateSamplerState(&samplerDesc, &g_dx11.samplerState);
    
    // Shadow sampler (comparison)
    D3D11_SAMPLER_DESC shadowSamplerDesc = {};
    shadowSamplerDesc.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
    shadowSamplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_BORDER;
    shadowSamplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_BORDER;
    shadowSamplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
    shadowSamplerDesc.BorderColor[0] = 1.0f;
    shadowSamplerDesc.BorderColor[1] = 1.0f;
    shadowSamplerDesc.BorderColor[2] = 1.0f;
    shadowSamplerDesc.BorderColor[3] = 1.0f;
    shadowSamplerDesc.ComparisonFunc = D3D11_COMPARISON_LESS;
    g_dx11.device->CreateSamplerState(&shadowSamplerDesc, &g_dx11.shadowSamplerState);
    
    g_dx11.initialized = true;
    return true;
}

inline void ResizeDX11(unsigned int width, unsigned int height) {
    if (!g_dx11.initialized || width == 0 || height == 0) return;
    
    g_dx11.screenWidth = width;
    g_dx11.screenHeight = height;
    
    g_dx11.context->OMSetRenderTargets(0, nullptr, nullptr);
    g_dx11.renderTargetView.Reset();
    g_dx11.depthStencilView.Reset();
    g_dx11.depthStencilBuffer.Reset();
    
    HRESULT hr = g_dx11.swapChain->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING);
    if (FAILED(hr)) return;
    
    ComPtr<ID3D11Texture2D> backBuffer;
    hr = g_dx11.swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr)) return;
    
    hr = g_dx11.device->CreateRenderTargetView(backBuffer.Get(), nullptr, &g_dx11.renderTargetView);
    if (FAILED(hr)) return;
    
    D3D11_TEXTURE2D_DESC depthDesc = {};
    depthDesc.Width = width;
    depthDesc.Height = height;
    depthDesc.MipLevels = 1;
    depthDesc.ArraySize = 1;
    depthDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.Usage = D3D11_USAGE_DEFAULT;
    depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    
    hr = g_dx11.device->CreateTexture2D(&depthDesc, nullptr, &g_dx11.depthStencilBuffer);
    if (FAILED(hr)) return;
    
    hr = g_dx11.device->CreateDepthStencilView(g_dx11.depthStencilBuffer.Get(), nullptr, &g_dx11.depthStencilView);
    if (FAILED(hr)) return;
    
    g_dx11.context->OMSetRenderTargets(1, g_dx11.renderTargetView.GetAddressOf(), g_dx11.depthStencilView.Get());
    
    D3D11_VIEWPORT viewport = {};
    viewport.Width = (float)width;
    viewport.Height = (float)height;
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    g_dx11.context->RSSetViewports(1, &viewport);
}

inline void CleanupDX11() {
    if (g_dx11.context) {
        g_dx11.context->ClearState();
    }
    g_dx11.initialized = false;
}

#endif

