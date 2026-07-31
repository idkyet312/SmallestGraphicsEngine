#ifndef MODEL_DX11_H
#define MODEL_DX11_H

#include "DX11Core.h"
#include <DirectXMath.h>
#include <vector>
#include <string>
#include <fstream>
#include <sstream>
#include <iostream>
#include <cmath>

using namespace DirectX;

extern DX11Context g_dx11;

struct Vertex {
    XMFLOAT3 Position;
    XMFLOAT3 Normal;
};

class Model {
public:
    std::vector<Vertex> vertices;
    std::vector<unsigned int> indices;
    ComPtr<ID3D11Buffer> vertexBuffer;
    ComPtr<ID3D11Buffer> indexBuffer;
    bool loaded = false;
    
    // Transform relative to camera
    XMFLOAT3 offset = XMFLOAT3(0.170f, -0.140f, 0.490f);
    XMFLOAT3 scale = XMFLOAT3(0.5f, 0.5f, 0.5f);
    XMFLOAT3 rotation = XMFLOAT3(0.0f, 180.0f, 0.0f);
    XMFLOAT3 color = XMFLOAT3(0.2f, 0.2f, 0.25f);
    bool visible = true;
    
    Model() {}
    
    ~Model() {}
    
    bool loadOBJ(const std::string& path) {
        std::vector<XMFLOAT3> temp_positions;
        std::vector<XMFLOAT3> temp_normals;
        std::vector<unsigned int> positionIndices, normalIndices;
        
        std::ifstream file(path);
        if (!file.is_open()) {
            std::cerr << "Failed to open OBJ file: " << path << std::endl;
            return false;
        }
        
        std::string line;
        while (std::getline(file, line)) {
            std::istringstream iss(line);
            std::string prefix;
            iss >> prefix;
            
            if (prefix == "v") {
                XMFLOAT3 pos;
                iss >> pos.x >> pos.y >> pos.z;
                temp_positions.push_back(pos);
            }
            else if (prefix == "vn") {
                XMFLOAT3 normal;
                iss >> normal.x >> normal.y >> normal.z;
                temp_normals.push_back(normal);
            }
            else if (prefix == "f") {
                std::string vertex;
                std::vector<unsigned int> facePositions;
                std::vector<unsigned int> faceNormals;
                
                while (iss >> vertex) {
                    unsigned int posIdx = 0, texIdx = 0, normIdx = 0;
                    
                    size_t firstSlash = vertex.find('/');
                    if (firstSlash == std::string::npos) {
                        posIdx = std::stoi(vertex);
                    } else {
                        posIdx = std::stoi(vertex.substr(0, firstSlash));
                        size_t secondSlash = vertex.find('/', firstSlash + 1);
                        if (secondSlash == std::string::npos) {
                            texIdx = std::stoi(vertex.substr(firstSlash + 1));
                        } else {
                            std::string texPart = vertex.substr(firstSlash + 1, secondSlash - firstSlash - 1);
                            if (!texPart.empty()) {
                                texIdx = std::stoi(texPart);
                            }
                            normIdx = std::stoi(vertex.substr(secondSlash + 1));
                        }
                    }
                    
                    facePositions.push_back(posIdx);
                    faceNormals.push_back(normIdx);
                }
                
                // Triangulate the face
                for (size_t i = 1; i + 1 < facePositions.size(); i++) {
                    positionIndices.push_back(facePositions[0]);
                    positionIndices.push_back(facePositions[i]);
                    positionIndices.push_back(facePositions[i + 1]);
                    
                    normalIndices.push_back(faceNormals[0]);
                    normalIndices.push_back(faceNormals[i]);
                    normalIndices.push_back(faceNormals[i + 1]);
                }
            }
        }
        file.close();
        
        // Build vertex array
        vertices.clear();
        indices.clear();
        
        for (size_t i = 0; i < positionIndices.size(); i++) {
            Vertex vertex;
            vertex.Position = temp_positions[positionIndices[i] - 1];
            
            if (normalIndices[i] > 0 && normalIndices[i] <= temp_normals.size()) {
                vertex.Normal = temp_normals[normalIndices[i] - 1];
            } else {
                vertex.Normal = XMFLOAT3(0.0f, 1.0f, 0.0f);
            }
            
            vertices.push_back(vertex);
            indices.push_back(static_cast<unsigned int>(i));
        }
        
        // If no normals were provided, calculate them
        if (temp_normals.empty()) {
            calculateNormals();
        }
        
        setupMesh();
        loaded = true;
        std::cout << "Loaded OBJ: " << path << " (" << vertices.size() << " vertices)" << std::endl;
        return true;
    }
    
    void calculateNormals() {
        for (size_t i = 0; i + 2 < vertices.size(); i += 3) {
            XMVECTOR v0 = XMLoadFloat3(&vertices[i].Position);
            XMVECTOR v1 = XMLoadFloat3(&vertices[i + 1].Position);
            XMVECTOR v2 = XMLoadFloat3(&vertices[i + 2].Position);
            
            XMVECTOR edge1 = XMVectorSubtract(v1, v0);
            XMVECTOR edge2 = XMVectorSubtract(v2, v0);
            XMVECTOR normal = XMVector3Normalize(XMVector3Cross(edge1, edge2));
            
            XMFLOAT3 normalF;
            XMStoreFloat3(&normalF, normal);
            vertices[i].Normal = normalF;
            vertices[i + 1].Normal = normalF;
            vertices[i + 2].Normal = normalF;
        }
    }
    
    void setupMesh() {
        D3D11_BUFFER_DESC bufferDesc = {};
        bufferDesc.Usage = D3D11_USAGE_DEFAULT;
        bufferDesc.ByteWidth = (UINT)(sizeof(Vertex) * vertices.size());
        bufferDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        
        D3D11_SUBRESOURCE_DATA initData = {};
        initData.pSysMem = vertices.data();
        
        g_dx11.device->CreateBuffer(&bufferDesc, &initData, &vertexBuffer);
        
        bufferDesc.ByteWidth = (UINT)(sizeof(unsigned int) * indices.size());
        bufferDesc.BindFlags = D3D11_BIND_INDEX_BUFFER;
        initData.pSysMem = indices.data();
        
        g_dx11.device->CreateBuffer(&bufferDesc, &initData, &indexBuffer);
    }
    
    XMMATRIX getModelMatrix(const XMFLOAT3& cameraPos, const XMFLOAT3& cameraFront, const XMFLOAT3& cameraUp) {
        XMVECTOR frontVec = XMLoadFloat3(&cameraFront);
        XMVECTOR upVec = XMLoadFloat3(&cameraUp);
        XMVECTOR rightVec = XMVector3Normalize(XMVector3Cross(upVec, frontVec));
        XMVECTOR upCorrected = XMVector3Normalize(XMVector3Cross(frontVec, rightVec));
        
        XMFLOAT3 right, up;
        XMStoreFloat3(&right, rightVec);
        XMStoreFloat3(&up, upCorrected);
        
        // Calculate world position based on camera
        XMFLOAT3 worldPos;
        worldPos.x = cameraPos.x + right.x * offset.x + up.x * offset.y + cameraFront.x * offset.z;
        worldPos.y = cameraPos.y + right.y * offset.x + up.y * offset.y + cameraFront.y * offset.z;
        worldPos.z = cameraPos.z + right.z * offset.x + up.z * offset.y + cameraFront.z * offset.z;
        
        // Calculate yaw and pitch from camera front
        float yaw = atan2f(cameraFront.x, cameraFront.z);
        float frontLength = sqrtf(cameraFront.x * cameraFront.x + cameraFront.z * cameraFront.z);
        float pitch = atan2f(cameraFront.y, frontLength);
        
        XMMATRIX model = XMMatrixIdentity();
        model = XMMatrixMultiply(model, XMMatrixScaling(scale.x, scale.y, scale.z));
        
        // Apply additional user-defined rotation offsets
        model = XMMatrixMultiply(model, XMMatrixRotationX(XMConvertToRadians(rotation.x)));
        model = XMMatrixMultiply(model, XMMatrixRotationY(XMConvertToRadians(rotation.y)));
        model = XMMatrixMultiply(model, XMMatrixRotationZ(XMConvertToRadians(rotation.z)));
        
        // Apply camera orientation
        model = XMMatrixMultiply(model, XMMatrixRotationX(-pitch));
        model = XMMatrixMultiply(model, XMMatrixRotationY(yaw));
        
        model = XMMatrixMultiply(model, XMMatrixTranslation(worldPos.x, worldPos.y, worldPos.z));
        
        return model;
    }
    
    void draw() {
        if (!loaded || !visible) return;
        
        UINT stride = sizeof(Vertex);
        UINT offset = 0;
        g_dx11.context->IASetVertexBuffers(0, 1, vertexBuffer.GetAddressOf(), &stride, &offset);
        g_dx11.context->IASetIndexBuffer(indexBuffer.Get(), DXGI_FORMAT_R32_UINT, 0);
        g_dx11.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        g_dx11.context->DrawIndexed((UINT)indices.size(), 0, 0);
    }
};

#endif

