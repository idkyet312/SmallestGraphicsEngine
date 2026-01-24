#ifndef SHADER_DX12_H
#define SHADER_DX12_H

#include "DX12Core.h"
#include <fstream>
#include <sstream>

// Constant buffer structures (must be 256-byte aligned for DX12)
struct alignas(256) MatrixBufferDX12 {
    XMMATRIX model;
    XMMATRIX view;
    XMMATRIX projection;
    XMMATRIX lightSpaceMatrix;
};

struct alignas(256) LightBufferDX12 {
    XMFLOAT3 lightPos;
    int lightType;
    XMFLOAT3 lightColor;
    float constant;
    float linear;
    float quadratic;
    float ambientStrength;
    float specularStrength;
    int shininess;
    float shadowBias;
    int enableShadows;
    float padding[1];
};

struct alignas(256) CameraBufferDX12 {
    XMFLOAT3 viewPos;
    float padding;
};

struct alignas(256) ObjectBufferDX12 {
    XMFLOAT3 objectColor;
    float padding;
};

struct PointLightDataDX12 {
    XMFLOAT3 position;
    float radius;
    XMFLOAT3 color;
    float intensity;
};

struct alignas(256) PointLightsBufferDX12 {
    int numPointLights;
    float padding1;
    float padding2;
    float padding3;
    PointLightDataDX12 lights[64];
};

struct alignas(256) DDGIBufferDX12 {
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
    float ddgiPadding[3];
};

// Upload buffer helper
template<typename T>
class UploadBuffer {
public:
    ComPtr<ID3D12Resource> resource;
    T* mappedData = nullptr;
    UINT elementCount = 0;
    UINT elementSize = 0;
    
    bool Create(UINT count) {
        elementCount = count;
        elementSize = sizeof(T);
        UINT bufferSize = elementCount * elementSize;
        
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
        
        D3D12_RESOURCE_DESC bufferDesc = {};
        bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bufferDesc.Width = bufferSize;
        bufferDesc.Height = 1;
        bufferDesc.DepthOrArraySize = 1;
        bufferDesc.MipLevels = 1;
        bufferDesc.Format = DXGI_FORMAT_UNKNOWN;
        bufferDesc.SampleDesc.Count = 1;
        bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        
        HRESULT hr = g_dx12.device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&resource));
        
        if (FAILED(hr)) return false;
        
        // Map and keep mapped
        D3D12_RANGE readRange = { 0, 0 };
        hr = resource->Map(0, &readRange, reinterpret_cast<void**>(&mappedData));
        if (FAILED(hr)) return false;
        
        return true;
    }
    
    void CopyData(UINT index, const T& data) {
        if (mappedData && index < elementCount) {
            memcpy(&mappedData[index], &data, sizeof(T));
        }
    }
    
    D3D12_GPU_VIRTUAL_ADDRESS GetGPUAddress(UINT index = 0) const {
        return resource->GetGPUVirtualAddress() + index * elementSize;
    }
    
    void Release() {
        if (resource && mappedData) {
            resource->Unmap(0, nullptr);
            mappedData = nullptr;
        }
        resource.Reset();
    }
    
    ~UploadBuffer() {
        Release();
    }
};

// Maximum draw calls per frame (for per-object constant buffers)
static const UINT MAX_DRAW_CALLS_PER_FRAME = 256;

class ShaderDX12 {
public:
    ComPtr<ID3D12RootSignature> rootSignature;
    ComPtr<ID3D12PipelineState> pipelineState;
    ComPtr<ID3D12PipelineState> wireframePipelineState;
    
    // Per-draw-call constant buffers (need enough for all objects)
    UploadBuffer<MatrixBufferDX12> matrixBuffer;
    UploadBuffer<ObjectBufferDX12> objectBuffer;
    
    // Per-frame constant buffers (shared across all draw calls in a frame)
    UploadBuffer<LightBufferDX12> lightBuffer;
    UploadBuffer<CameraBufferDX12> cameraBuffer;
    UploadBuffer<PointLightsBufferDX12> pointLightsBuffer;
    UploadBuffer<DDGIBufferDX12> ddgiBuffer;
    
    // Current draw call index within frame
    UINT currentDrawCall = 0;
    
    bool loaded = false;
    
    ShaderDX12() {}
    
    bool Load(const char* vertexPath, const char* pixelPath) {
        // Read shader files
        std::ifstream vsFile(vertexPath);
        std::ifstream psFile(pixelPath);
        
        if (!vsFile.is_open() || !psFile.is_open()) {
            std::cerr << "Failed to open shader files: " << vertexPath << ", " << pixelPath << std::endl;
            return false;
        }
        
        std::stringstream vsStream, psStream;
        vsStream << vsFile.rdbuf();
        psStream << psFile.rdbuf();
        
        std::string vsCode = vsStream.str();
        std::string psCode = psStream.str();
        
        // Compile vertex shader (use 5_0 for compatibility)
        ComPtr<ID3DBlob> vsBlob;
        ComPtr<ID3DBlob> errorBlob;
        UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
        compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
        compileFlags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
        
        HRESULT hr = D3DCompile(vsCode.c_str(), vsCode.length(), vertexPath,
            nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "vs_5_0",
            compileFlags, 0, &vsBlob, &errorBlob);
        
        if (FAILED(hr)) {
            if (errorBlob) {
                std::cerr << "Vertex shader compilation error: " << (char*)errorBlob->GetBufferPointer() << std::endl;
            }
            return false;
        }
        
        // Compile pixel shader
        ComPtr<ID3DBlob> psBlob;
        errorBlob.Reset();
        hr = D3DCompile(psCode.c_str(), psCode.length(), pixelPath,
            nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "ps_5_0",
            compileFlags, 0, &psBlob, &errorBlob);
        
        if (FAILED(hr)) {
            if (errorBlob) {
                std::cerr << "Pixel shader compilation error: " << (char*)errorBlob->GetBufferPointer() << std::endl;
            }
            return false;
        }
        
        // Create root signature using version 1.0 for maximum compatibility
        // Root parameters:
        // 0: CBV - Matrix buffer (b0)
        // 1: CBV - Light buffer (b1)
        // 2: CBV - Camera buffer (b2)
        // 3: CBV - Object buffer (b3)
        // 4: CBV - Point lights buffer (b4)
        // 5: CBV - DDGI buffer (b5)
        // 6: Descriptor table - SRVs (t0, t2, t3 for shadowMap, ddgiIrradiance, ddgiVisibility)
        
        D3D12_ROOT_PARAMETER rootParams[7] = {};
        
        // CBVs (root descriptors)
        for (int i = 0; i < 6; i++) {
            rootParams[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            rootParams[i].Descriptor.ShaderRegister = i;
            rootParams[i].Descriptor.RegisterSpace = 0;
            rootParams[i].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }
        
        // SRV descriptor table for textures
        D3D12_DESCRIPTOR_RANGE srvRanges[3] = {};
        // t0 - shadowMap
        srvRanges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRanges[0].NumDescriptors = 1;
        srvRanges[0].BaseShaderRegister = 0;
        srvRanges[0].RegisterSpace = 0;
        srvRanges[0].OffsetInDescriptorsFromTableStart = 0;
        // t2 - ddgiIrradiance
        srvRanges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRanges[1].NumDescriptors = 1;
        srvRanges[1].BaseShaderRegister = 2;
        srvRanges[1].RegisterSpace = 0;
        srvRanges[1].OffsetInDescriptorsFromTableStart = 1;
        // t3 - ddgiVisibility
        srvRanges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRanges[2].NumDescriptors = 1;
        srvRanges[2].BaseShaderRegister = 3;
        srvRanges[2].RegisterSpace = 0;
        srvRanges[2].OffsetInDescriptorsFromTableStart = 2;
        
        rootParams[6].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        rootParams[6].DescriptorTable.NumDescriptorRanges = 3;
        rootParams[6].DescriptorTable.pDescriptorRanges = srvRanges;
        rootParams[6].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        
        // Static samplers
        D3D12_STATIC_SAMPLER_DESC staticSamplers[2] = {};
        
        // Shadow sampler (comparison)
        staticSamplers[0].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
        staticSamplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        staticSamplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        staticSamplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        staticSamplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        staticSamplers[0].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
        staticSamplers[0].ShaderRegister = 0;
        staticSamplers[0].RegisterSpace = 0;
        staticSamplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        
        // Regular sampler
        staticSamplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        staticSamplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        staticSamplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        staticSamplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        staticSamplers[1].ShaderRegister = 1;
        staticSamplers[1].RegisterSpace = 0;
        staticSamplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        
        D3D12_ROOT_SIGNATURE_DESC rootSigDesc = {};
        rootSigDesc.NumParameters = 7;
        rootSigDesc.pParameters = rootParams;
        rootSigDesc.NumStaticSamplers = 2;
        rootSigDesc.pStaticSamplers = staticSamplers;
        rootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        
        ComPtr<ID3DBlob> signatureBlob;
        errorBlob.Reset();
        hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signatureBlob, &errorBlob);
        if (FAILED(hr)) {
            if (errorBlob) {
                std::cerr << "Root signature serialization error: " << (char*)errorBlob->GetBufferPointer() << std::endl;
            } else {
                std::cerr << "Root signature serialization failed with HRESULT: 0x" << std::hex << hr << std::dec << std::endl;
            }
            return false;
        }
        
        hr = g_dx12.device->CreateRootSignature(0, signatureBlob->GetBufferPointer(),
            signatureBlob->GetBufferSize(), IID_PPV_ARGS(&rootSignature));
        if (FAILED(hr)) {
            std::cerr << "Failed to create root signature" << std::endl;
            return false;
        }
        
        // Input layout
        D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
        };
        
        // Create PSO
        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.InputLayout = { inputLayout, _countof(inputLayout) };
        psoDesc.pRootSignature = rootSignature.Get();
        psoDesc.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
        psoDesc.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };
        
        psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
        psoDesc.RasterizerState.FrontCounterClockwise = FALSE;
        psoDesc.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
        psoDesc.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
        psoDesc.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
        psoDesc.RasterizerState.DepthClipEnable = TRUE;
        
        psoDesc.BlendState.AlphaToCoverageEnable = FALSE;
        psoDesc.BlendState.IndependentBlendEnable = FALSE;
        psoDesc.BlendState.RenderTarget[0].BlendEnable = FALSE;
        psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        
        psoDesc.DepthStencilState.DepthEnable = TRUE;
        psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        psoDesc.DepthStencilState.StencilEnable = FALSE;
        
        psoDesc.SampleMask = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        psoDesc.SampleDesc.Count = 1;
        
        hr = g_dx12.device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pipelineState));
        if (FAILED(hr)) {
            std::cerr << "Failed to create pipeline state, HRESULT: 0x" << std::hex << hr << std::dec << std::endl;
            return false;
        }
        
        // Create wireframe PSO
        psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_WIREFRAME;
        hr = g_dx12.device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&wireframePipelineState));
        if (FAILED(hr)) {
            std::cerr << "Failed to create wireframe pipeline state" << std::endl;
            // Non-fatal
        }
        
        // Create constant buffers
        // Per-draw-call buffers need enough slots for all objects per frame
        if (!matrixBuffer.Create(FRAME_COUNT * MAX_DRAW_CALLS_PER_FRAME)) return false;
        if (!objectBuffer.Create(FRAME_COUNT * MAX_DRAW_CALLS_PER_FRAME)) return false;
        
        // Per-frame buffers only need FRAME_COUNT slots
        if (!lightBuffer.Create(FRAME_COUNT)) return false;
        if (!cameraBuffer.Create(FRAME_COUNT)) return false;
        if (!pointLightsBuffer.Create(FRAME_COUNT)) return false;
        if (!ddgiBuffer.Create(FRAME_COUNT)) return false;
        
        loaded = true;
        return true;
    }
    
    void Use(bool wireframe = false) {
        if (!loaded) return;
        
        g_dx12.commandList->SetGraphicsRootSignature(rootSignature.Get());
        g_dx12.commandList->SetPipelineState(wireframe ? wireframePipelineState.Get() : pipelineState.Get());
    }
    
    // Call this at the start of each frame to reset draw call counter
    void BeginFrame() {
        currentDrawCall = 0;
    }
    
    // Get the buffer index for per-draw-call data
    UINT GetDrawCallIndex() const {
        return g_dx12.frameIndex * MAX_DRAW_CALLS_PER_FRAME + currentDrawCall;
    }
    
    void SetMatrices(const XMMATRIX& model, const XMMATRIX& view, const XMMATRIX& proj, const XMMATRIX& lightSpace) {
        UINT bufferIndex = GetDrawCallIndex();
        
        MatrixBufferDX12 data;
        data.model = XMMatrixTranspose(model);
        data.view = XMMatrixTranspose(view);
        data.projection = XMMatrixTranspose(proj);
        data.lightSpaceMatrix = XMMatrixTranspose(lightSpace);
        matrixBuffer.CopyData(bufferIndex, data);
        g_dx12.commandList->SetGraphicsRootConstantBufferView(0, matrixBuffer.GetGPUAddress(bufferIndex));
    }
    
    void SetLight(const XMFLOAT3& pos, int type, const XMFLOAT3& color, 
                  float constant, float linear, float quadratic,
                  float ambient, float specular, int shininess,
                  float shadowBias, bool enableShadows) {
        LightBufferDX12 data;
        data.lightPos = pos;
        data.lightType = type;
        data.lightColor = color;
        data.constant = constant;
        data.linear = linear;
        data.quadratic = quadratic;
        data.ambientStrength = ambient;
        data.specularStrength = specular;
        data.shininess = shininess;
        data.shadowBias = shadowBias;
        data.enableShadows = enableShadows ? 1 : 0;
        lightBuffer.CopyData(g_dx12.frameIndex, data);
        g_dx12.commandList->SetGraphicsRootConstantBufferView(1, lightBuffer.GetGPUAddress(g_dx12.frameIndex));
    }
    
    void SetCamera(const XMFLOAT3& pos) {
        CameraBufferDX12 data;
        data.viewPos = pos;
        cameraBuffer.CopyData(g_dx12.frameIndex, data);
        g_dx12.commandList->SetGraphicsRootConstantBufferView(2, cameraBuffer.GetGPUAddress(g_dx12.frameIndex));
    }
    
    void SetObjectColor(const XMFLOAT3& color) {
        UINT bufferIndex = GetDrawCallIndex();
        
        ObjectBufferDX12 data;
        data.objectColor = color;
        objectBuffer.CopyData(bufferIndex, data);
        g_dx12.commandList->SetGraphicsRootConstantBufferView(3, objectBuffer.GetGPUAddress(bufferIndex));
    }
    
    // Call this after each DrawCube/DrawPlane to advance to the next buffer slot
    void NextDrawCall() {
        currentDrawCall++;
        if (currentDrawCall >= MAX_DRAW_CALLS_PER_FRAME) {
            currentDrawCall = MAX_DRAW_CALLS_PER_FRAME - 1; // Clamp to avoid overflow
        }
    }
    
    void SetPointLights(int numLights, const std::vector<PointLightDataDX12>& lights) {
        PointLightsBufferDX12 data = {};
        data.numPointLights = numLights;
        int count = (numLights < 64) ? numLights : 64;
        for (int i = 0; i < count; i++) {
            data.lights[i] = lights[i];
        }
        pointLightsBuffer.CopyData(g_dx12.frameIndex, data);
        g_dx12.commandList->SetGraphicsRootConstantBufferView(4, pointLightsBuffer.GetGPUAddress(g_dx12.frameIndex));
    }
    
    void SetDDGI(bool enabled, float gi_intensity, float normal_bias, float probe_spacing) {
        DDGIBufferDX12 data = {};
        data.probeGridOrigin = XMFLOAT3(-7.0f, 0.5f, -7.0f);
        data.probeSpacing = probe_spacing;
        data.probeCountX = 8;
        data.probeCountY = 4;
        data.probeCountZ = 8;
        data.maxRayDistance = 20.0f;
        data.normalBias = normal_bias;
        data.viewBias = 0.01f;
        data.irradianceGamma = 5.0f;
        data.giIntensity = gi_intensity;
        data.irradianceTexWidth = 8;
        data.irradianceTexHeight = 8;
        data.visibilityTexWidth = 16;
        data.visibilityTexHeight = 16;
        data.ddgiEnabled = enabled ? 1 : 0;
        ddgiBuffer.CopyData(g_dx12.frameIndex, data);
        g_dx12.commandList->SetGraphicsRootConstantBufferView(5, ddgiBuffer.GetGPUAddress(g_dx12.frameIndex));
    }
};

#endif // SHADER_DX12_H

