#ifndef SHADER_DX11_H
#define SHADER_DX11_H

#include "DX11Core.h"
#include <DirectXMath.h>
#include <string>
#include <fstream>
#include <sstream>

using namespace DirectX;

extern DX11Context g_dx11;

// Forward declaration
struct PointLightData;

// Constant buffer structures
struct MatrixBuffer {
    XMMATRIX model;
    XMMATRIX view;
    XMMATRIX projection;
    XMMATRIX lightSpaceMatrix;
};

struct LightBuffer {
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
    float padding;
};

struct CameraBuffer {
    XMFLOAT3 viewPos;
    float padding;
};

struct ObjectBuffer {
    XMFLOAT3 objectColor;
    float padding;
};

struct ShaderPointLightData {
    XMFLOAT3 position;
    float radius;
    XMFLOAT3 color;
    float intensity;
};

struct PointLightsBuffer {
    int numPointLights;
    float padding1;
    float padding2;
    float padding3;
    ShaderPointLightData lights[64];
};

class Shader {
public:
    ComPtr<ID3D11VertexShader> vertexShader;
    ComPtr<ID3D11PixelShader> pixelShader;
    ComPtr<ID3D11InputLayout> inputLayout;
    ComPtr<ID3D11Buffer> matrixBuffer;
    ComPtr<ID3D11Buffer> lightBuffer;
    ComPtr<ID3D11Buffer> cameraBuffer;
    ComPtr<ID3D11Buffer> objectBuffer;
    ComPtr<ID3D11Buffer> pointLightsBuffer;
    
    bool loaded = false;
    
    Shader() {}
    
    Shader(const char* vertexPath, const char* pixelPath) {
        load(vertexPath, pixelPath);
    }
    
    bool load(const char* vertexPath, const char* pixelPath) {
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
        
        // Compile vertex shader
        ComPtr<ID3DBlob> vsBlob;
        ComPtr<ID3DBlob> errorBlob;
        
        HRESULT hr = D3DCompile(vsCode.c_str(), vsCode.length(), vertexPath, nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
            "main", "vs_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &vsBlob, &errorBlob);
        
        if (FAILED(hr)) {
            if (errorBlob) {
                std::cerr << "Vertex shader compilation error: " << (char*)errorBlob->GetBufferPointer() << std::endl;
            }
            return false;
        }
        
        hr = g_dx11.device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vertexShader);
        if (FAILED(hr)) return false;
        
        // Compile pixel shader
        ComPtr<ID3DBlob> psBlob;
        hr = D3DCompile(psCode.c_str(), psCode.length(), pixelPath, nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
            "main", "ps_5_0", D3DCOMPILE_ENABLE_STRICTNESS, 0, &psBlob, &errorBlob);
        
        if (FAILED(hr)) {
            if (errorBlob) {
                std::cerr << "Pixel shader compilation error: " << (char*)errorBlob->GetBufferPointer() << std::endl;
            }
            return false;
        }
        
        hr = g_dx11.device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &pixelShader);
        if (FAILED(hr)) return false;
        
        // Create input layout
        D3D11_INPUT_ELEMENT_DESC layout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        
        hr = g_dx11.device->CreateInputLayout(layout, 2, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &inputLayout);
        if (FAILED(hr)) return false;
        
        // Create constant buffers
        D3D11_BUFFER_DESC bufferDesc = {};
        bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
        bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        
        bufferDesc.ByteWidth = sizeof(MatrixBuffer);
        g_dx11.device->CreateBuffer(&bufferDesc, nullptr, &matrixBuffer);
        
        bufferDesc.ByteWidth = sizeof(LightBuffer);
        g_dx11.device->CreateBuffer(&bufferDesc, nullptr, &lightBuffer);
        
        bufferDesc.ByteWidth = sizeof(CameraBuffer);
        g_dx11.device->CreateBuffer(&bufferDesc, nullptr, &cameraBuffer);
        
        bufferDesc.ByteWidth = sizeof(ObjectBuffer);
        g_dx11.device->CreateBuffer(&bufferDesc, nullptr, &objectBuffer);
        
        bufferDesc.ByteWidth = sizeof(PointLightsBuffer);
        g_dx11.device->CreateBuffer(&bufferDesc, nullptr, &pointLightsBuffer);
        
        loaded = true;
        return true;
    }
    
    void use() {
        if (!loaded) return;
        g_dx11.context->IASetInputLayout(inputLayout.Get());
        g_dx11.context->VSSetShader(vertexShader.Get(), nullptr, 0);
        g_dx11.context->PSSetShader(pixelShader.Get(), nullptr, 0);
    }
    
    void setMatrices(const XMMATRIX& model, const XMMATRIX& view, const XMMATRIX& projection, const XMMATRIX& lightSpace) {
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(g_dx11.context->Map(matrixBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            MatrixBuffer* data = (MatrixBuffer*)mapped.pData;
            data->model = XMMatrixTranspose(model);
            data->view = XMMatrixTranspose(view);
            data->projection = XMMatrixTranspose(projection);
            data->lightSpaceMatrix = XMMatrixTranspose(lightSpace);
            g_dx11.context->Unmap(matrixBuffer.Get(), 0);
        }
        g_dx11.context->VSSetConstantBuffers(0, 1, matrixBuffer.GetAddressOf());
        g_dx11.context->PSSetConstantBuffers(0, 1, matrixBuffer.GetAddressOf());
    }
    
    void setLight(const XMFLOAT3& pos, int type, const XMFLOAT3& color, float constant, float linear, float quadratic,
                  float ambient, float specular, int shininess, float shadowBias, bool enableShadows) {
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(g_dx11.context->Map(lightBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            LightBuffer* data = (LightBuffer*)mapped.pData;
            data->lightPos = pos;
            data->lightType = type;
            data->lightColor = color;
            data->constant = constant;
            data->linear = linear;
            data->quadratic = quadratic;
            data->ambientStrength = ambient;
            data->specularStrength = specular;
            data->shininess = shininess;
            data->shadowBias = shadowBias;
            data->enableShadows = enableShadows ? 1 : 0;
            g_dx11.context->Unmap(lightBuffer.Get(), 0);
        }
        g_dx11.context->PSSetConstantBuffers(1, 1, lightBuffer.GetAddressOf());
    }
    
    void setCamera(const XMFLOAT3& viewPos) {
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(g_dx11.context->Map(cameraBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            CameraBuffer* data = (CameraBuffer*)mapped.pData;
            data->viewPos = viewPos;
            g_dx11.context->Unmap(cameraBuffer.Get(), 0);
        }
        g_dx11.context->PSSetConstantBuffers(2, 1, cameraBuffer.GetAddressOf());
    }
    
    void setObjectColor(const XMFLOAT3& color) {
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(g_dx11.context->Map(objectBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            ObjectBuffer* data = (ObjectBuffer*)mapped.pData;
            data->objectColor = color;
            g_dx11.context->Unmap(objectBuffer.Get(), 0);
        }
        g_dx11.context->PSSetConstantBuffers(3, 1, objectBuffer.GetAddressOf());
    }
    
    void setPointLights(int numLights, const std::vector<PointLightData>& lights);
};

#endif

