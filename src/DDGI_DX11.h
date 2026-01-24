#ifndef DDGI_DX11_H
#define DDGI_DX11_H

#include "DX11Core.h"
#include <DirectXMath.h>
#include <vector>
#include <random>

using namespace DirectX;
using Microsoft::WRL::ComPtr;

extern DX11Context g_dx11;

// DDGI Configuration
struct DDGIConfig {
    int probeCountX = 8;
    int probeCountY = 4;
    int probeCountZ = 8;
    float probeSpacing = 2.0f;
    XMFLOAT3 probeGridOrigin = XMFLOAT3(-7.0f, 0.5f, -7.0f);
    
    int irradianceTexWidth = 8;   // Per probe
    int irradianceTexHeight = 8;
    int visibilityTexWidth = 16;  // Per probe
    int visibilityTexHeight = 16;
    
    int raysPerProbe = 64;
    float maxRayDistance = 20.0f;
    float normalBias = 0.1f;
    float viewBias = 0.1f;
    float hysteresis = 0.97f;     // Temporal blending
    float irradianceGamma = 5.0f;
    
    bool enabled = true;
    bool showProbes = false;
    float probeVisualScale = 0.1f;
    float giIntensity = 1.0f;
};

// Probe ray direction generation (Fibonacci sphere)
inline std::vector<XMFLOAT3> GenerateProbeRayDirections(int numRays) {
    std::vector<XMFLOAT3> directions;
    directions.reserve(numRays);
    
    float goldenRatio = (1.0f + sqrtf(5.0f)) / 2.0f;
    float angleIncrement = XM_2PI / goldenRatio;
    
    for (int i = 0; i < numRays; i++) {
        float t = (float)i / (float)numRays;
        float inclination = acosf(1.0f - 2.0f * t);
        float azimuth = angleIncrement * i;
        
        float sinInc = sinf(inclination);
        XMFLOAT3 dir;
        dir.x = sinInc * cosf(azimuth);
        dir.y = cosf(inclination);
        dir.z = sinInc * sinf(azimuth);
        directions.push_back(dir);
    }
    
    return directions;
}

// DDGI Constant buffer for shaders
struct DDGIConstantBuffer {
    XMFLOAT3 probeGridOrigin;
    float probeSpacing;
    
    int probeCountX;
    int probeCountY;
    int probeCountZ;
    float maxRayDistance;
    
    float normalBias;
    float viewBias;
    float irradianceGamma;
    float giIntensity;
    
    int irradianceTexWidth;
    int irradianceTexHeight;
    int visibilityTexWidth;
    int visibilityTexHeight;
    
    int ddgiEnabled;
    float padding[3];
};

class DDGIRenderer {
public:
    DDGIConfig config;
    
    // Textures for probe data (atlas format)
    ComPtr<ID3D11Texture2D> irradianceTexture;
    ComPtr<ID3D11ShaderResourceView> irradianceSRV;
    ComPtr<ID3D11RenderTargetView> irradianceRTV;
    
    ComPtr<ID3D11Texture2D> visibilityTexture;
    ComPtr<ID3D11ShaderResourceView> visibilitySRV;
    ComPtr<ID3D11RenderTargetView> visibilityRTV;
    
    // Previous frame textures for temporal blending
    ComPtr<ID3D11Texture2D> prevIrradianceTexture;
    ComPtr<ID3D11ShaderResourceView> prevIrradianceSRV;
    
    ComPtr<ID3D11Texture2D> prevVisibilityTexture;
    ComPtr<ID3D11ShaderResourceView> prevVisibilitySRV;
    
    // Constant buffer
    ComPtr<ID3D11Buffer> ddgiConstantBuffer;
    
    // Ray directions buffer
    ComPtr<ID3D11Buffer> rayDirectionsBuffer;
    ComPtr<ID3D11ShaderResourceView> rayDirectionsSRV;
    
    std::vector<XMFLOAT3> rayDirections;
    
    bool initialized = false;
    int frameCount = 0;
    
    DDGIRenderer() {}
    
    ~DDGIRenderer() {
        cleanup();
    }
    
    bool init() {
        if (initialized) return true;
        
        // Generate ray directions
        rayDirections = GenerateProbeRayDirections(config.raysPerProbe);
        
        // Calculate atlas dimensions
        int atlasWidth = config.probeCountX * config.probeCountZ * config.irradianceTexWidth;
        int atlasHeight = config.probeCountY * config.irradianceTexHeight;
        
        // Create irradiance texture (RGBA16F for HDR)
        D3D11_TEXTURE2D_DESC texDesc = {};
        texDesc.Width = atlasWidth;
        texDesc.Height = atlasHeight;
        texDesc.MipLevels = 1;
        texDesc.ArraySize = 1;
        texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        texDesc.SampleDesc.Count = 1;
        texDesc.Usage = D3D11_USAGE_DEFAULT;
        texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        
        HRESULT hr = g_dx11.device->CreateTexture2D(&texDesc, nullptr, &irradianceTexture);
        if (FAILED(hr)) return false;
        
        hr = g_dx11.device->CreateShaderResourceView(irradianceTexture.Get(), nullptr, &irradianceSRV);
        if (FAILED(hr)) return false;
        
        hr = g_dx11.device->CreateRenderTargetView(irradianceTexture.Get(), nullptr, &irradianceRTV);
        if (FAILED(hr)) return false;
        
        // Create previous irradiance texture
        hr = g_dx11.device->CreateTexture2D(&texDesc, nullptr, &prevIrradianceTexture);
        if (FAILED(hr)) return false;
        
        hr = g_dx11.device->CreateShaderResourceView(prevIrradianceTexture.Get(), nullptr, &prevIrradianceSRV);
        if (FAILED(hr)) return false;
        
        // Create visibility texture
        int visAtlasWidth = config.probeCountX * config.probeCountZ * config.visibilityTexWidth;
        int visAtlasHeight = config.probeCountY * config.visibilityTexHeight;
        
        texDesc.Width = visAtlasWidth;
        texDesc.Height = visAtlasHeight;
        texDesc.Format = DXGI_FORMAT_R16G16_FLOAT; // Distance and distance^2
        
        hr = g_dx11.device->CreateTexture2D(&texDesc, nullptr, &visibilityTexture);
        if (FAILED(hr)) return false;
        
        hr = g_dx11.device->CreateShaderResourceView(visibilityTexture.Get(), nullptr, &visibilitySRV);
        if (FAILED(hr)) return false;
        
        hr = g_dx11.device->CreateRenderTargetView(visibilityTexture.Get(), nullptr, &visibilityRTV);
        if (FAILED(hr)) return false;
        
        // Create previous visibility texture
        hr = g_dx11.device->CreateTexture2D(&texDesc, nullptr, &prevVisibilityTexture);
        if (FAILED(hr)) return false;
        
        hr = g_dx11.device->CreateShaderResourceView(prevVisibilityTexture.Get(), nullptr, &prevVisibilitySRV);
        if (FAILED(hr)) return false;
        
        // Create constant buffer
        D3D11_BUFFER_DESC cbDesc = {};
        cbDesc.ByteWidth = sizeof(DDGIConstantBuffer);
        cbDesc.Usage = D3D11_USAGE_DYNAMIC;
        cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        
        hr = g_dx11.device->CreateBuffer(&cbDesc, nullptr, &ddgiConstantBuffer);
        if (FAILED(hr)) return false;
        
        // Create ray directions buffer
        D3D11_BUFFER_DESC rayBufDesc = {};
        rayBufDesc.ByteWidth = (UINT)(rayDirections.size() * sizeof(XMFLOAT4)); // Pad to float4
        rayBufDesc.Usage = D3D11_USAGE_DEFAULT;
        rayBufDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        rayBufDesc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
        rayBufDesc.StructureByteStride = sizeof(XMFLOAT4);
        
        std::vector<XMFLOAT4> paddedDirs;
        for (const auto& d : rayDirections) {
            paddedDirs.push_back(XMFLOAT4(d.x, d.y, d.z, 0.0f));
        }
        
        D3D11_SUBRESOURCE_DATA initData = {};
        initData.pSysMem = paddedDirs.data();
        
        hr = g_dx11.device->CreateBuffer(&rayBufDesc, &initData, &rayDirectionsBuffer);
        if (FAILED(hr)) return false;
        
        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_UNKNOWN;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        srvDesc.Buffer.FirstElement = 0;
        srvDesc.Buffer.NumElements = (UINT)rayDirections.size();
        
        hr = g_dx11.device->CreateShaderResourceView(rayDirectionsBuffer.Get(), &srvDesc, &rayDirectionsSRV);
        if (FAILED(hr)) return false;
        
        // Clear textures to initial values
        float clearIrradiance[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
        g_dx11.context->ClearRenderTargetView(irradianceRTV.Get(), clearIrradiance);
        
        float clearVisibility[4] = { config.maxRayDistance, config.maxRayDistance * config.maxRayDistance, 0.0f, 0.0f };
        g_dx11.context->ClearRenderTargetView(visibilityRTV.Get(), clearVisibility);
        
        initialized = true;
        return true;
    }
    
    void cleanup() {
        irradianceTexture.Reset();
        irradianceSRV.Reset();
        irradianceRTV.Reset();
        prevIrradianceTexture.Reset();
        prevIrradianceSRV.Reset();
        visibilityTexture.Reset();
        visibilitySRV.Reset();
        visibilityRTV.Reset();
        prevVisibilityTexture.Reset();
        prevVisibilitySRV.Reset();
        ddgiConstantBuffer.Reset();
        rayDirectionsBuffer.Reset();
        rayDirectionsSRV.Reset();
        initialized = false;
    }
    
    void updateConstantBuffer() {
        if (!initialized) return;
        
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(g_dx11.context->Map(ddgiConstantBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            DDGIConstantBuffer* cb = (DDGIConstantBuffer*)mapped.pData;
            cb->probeGridOrigin = config.probeGridOrigin;
            cb->probeSpacing = config.probeSpacing;
            cb->probeCountX = config.probeCountX;
            cb->probeCountY = config.probeCountY;
            cb->probeCountZ = config.probeCountZ;
            cb->maxRayDistance = config.maxRayDistance;
            cb->normalBias = config.normalBias;
            cb->viewBias = config.viewBias;
            cb->irradianceGamma = config.irradianceGamma;
            cb->giIntensity = config.giIntensity;
            cb->irradianceTexWidth = config.irradianceTexWidth;
            cb->irradianceTexHeight = config.irradianceTexHeight;
            cb->visibilityTexWidth = config.visibilityTexWidth;
            cb->visibilityTexHeight = config.visibilityTexHeight;
            cb->ddgiEnabled = config.enabled ? 1 : 0;
            g_dx11.context->Unmap(ddgiConstantBuffer.Get(), 0);
        }
    }
    
    XMFLOAT3 getProbePosition(int x, int y, int z) const {
        return XMFLOAT3(
            config.probeGridOrigin.x + x * config.probeSpacing,
            config.probeGridOrigin.y + y * config.probeSpacing,
            config.probeGridOrigin.z + z * config.probeSpacing
        );
    }
    
    int getTotalProbeCount() const {
        return config.probeCountX * config.probeCountY * config.probeCountZ;
    }
    
    // Bind DDGI resources for sampling in main shader
    void bind(int irradianceSlot = 2, int visibilitySlot = 3, int cbSlot = 5) {
        if (!initialized || !config.enabled) return;
        
        updateConstantBuffer();
        
        g_dx11.context->PSSetShaderResources(irradianceSlot, 1, irradianceSRV.GetAddressOf());
        g_dx11.context->PSSetShaderResources(visibilitySlot, 1, visibilitySRV.GetAddressOf());
        g_dx11.context->PSSetConstantBuffers(cbSlot, 1, ddgiConstantBuffer.GetAddressOf());
    }
    
    void unbind(int irradianceSlot = 2, int visibilitySlot = 3) {
        ID3D11ShaderResourceView* nullSRV = nullptr;
        g_dx11.context->PSSetShaderResources(irradianceSlot, 1, &nullSRV);
        g_dx11.context->PSSetShaderResources(visibilitySlot, 1, &nullSRV);
    }
    
    // Structure to hold light info for GI computation
    struct GILight {
        XMFLOAT3 position;
        XMFLOAT3 color;
        float radius;
        float intensity;
        bool isDirectional;
    };
    
    std::vector<GILight> giLights;
    
    void addGILight(const XMFLOAT3& pos, const XMFLOAT3& color, float radius, float intensity, bool directional = false) {
        GILight light;
        light.position = pos;
        light.color = color;
        light.radius = radius;
        light.intensity = intensity;
        light.isDirectional = directional;
        giLights.push_back(light);
    }
    
    void clearGILights() {
        giLights.clear();
    }
    
    // Update probes by computing GI from all scene lights
    void updateProbesFromLights() {
        if (!initialized || !config.enabled) return;
        
        frameCount++;
        
        // Only update every 30 frames to avoid performance issues
        if (frameCount % 30 != 0) return;
        
        // Simple approach: clear with average light color
        float totalR = 0.0f, totalG = 0.0f, totalB = 0.0f;
        float totalIntensity = 0.0f;
        
        for (const auto& light : giLights) {
            float intensity = light.intensity;
            if (!light.isDirectional) {
                intensity *= 0.5f; // Reduce point light contribution
            }
            totalR += light.color.x * intensity;
            totalG += light.color.y * intensity;
            totalB += light.color.z * intensity;
            totalIntensity += intensity;
        }
        
        if (totalIntensity > 0.0f) {
            totalR /= totalIntensity;
            totalG /= totalIntensity;
            totalB /= totalIntensity;
        }
        
        // Apply GI intensity
        float gi = config.giIntensity * 0.3f;
        float clearColor[4] = { totalR * gi, totalG * gi, totalB * gi, 1.0f };
        
        g_dx11.context->ClearRenderTargetView(irradianceRTV.Get(), clearColor);
    }
    
    // Legacy update function - now calls the new one
    void updateProbes(const XMFLOAT3& mainLightDir, const XMFLOAT3& mainLightColor, float ambientStrength) {
        // Add main light as directional if not already in list
        clearGILights();
        addGILight(mainLightDir, mainLightColor, 100.0f, 1.0f, true);
        updateProbesFromLights();
    }
};

#endif

