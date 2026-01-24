#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <iostream>
#include <vector>
#include <chrono>
#include <algorithm>
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx12.h>

#include "DX12Core.h"
#include "ShaderDX12.h"
#include "CameraDX12.h" // Camera logic is API-agnostic
#include "ClusteredRendererDX12.h"
#include "SceneGraph.h"
#include "GLBImporter.h"

using namespace DirectX;

// Settings
unsigned int SCR_WIDTH = 1280;
unsigned int SCR_HEIGHT = 720;

Camera camera(XMFLOAT3(0.0f, 5.0f, 10.0f));
float lastX = SCR_WIDTH / 2.0f;
float lastY = SCR_HEIGHT / 2.0f;
bool firstMouse = true;
float deltaTime = 0.0f;
bool showUI = true;
bool cameraLocked = true;
bool isFullscreen = false;
RECT windowedRect = {};
DWORD windowedStyle = 0;

// Scene parameters
XMFLOAT3 lightPos(-5.0f, 10.0f, -5.0f);
XMFLOAT3 lightTarget(0.0f, 0.0f, 0.0f);
XMFLOAT3 lightUp(0.0f, 1.0f, 0.0f);
XMFLOAT3 cubePosition(0.0f, 1.5f, 0.0f);
XMFLOAT3 cubeScale(1.0f, 1.0f, 1.0f);
XMFLOAT3 cubeRotation(0.0f, 0.0f, 0.0f);
XMFLOAT3 cubeColor(0.7f, 0.7f, 0.7f);  // Grey like DX11
XMFLOAT3 floorColor(0.5f, 0.5f, 0.5f);
XMFLOAT3 clearColor(0.1f, 0.1f, 0.1f);
float cameraFOV = 45.0f;
float cameraNear = 0.1f;
float cameraFar = 100.0f;
float lightOrthoSize = 15.0f;
float lightNear = 1.0f;
float lightFar = 25.0f;

float shadowBias = 0.005f;
bool enableShadows = true;
bool wireframeMode = false;
float ambientStrength = 0.0f;
float specularStrength = 0.5f;
int specularShininess = 32;

// Second cube
bool showSecondCube = false;
XMFLOAT3 cube2Position(-3.0f, 0.5f, 2.0f);
XMFLOAT3 cube2Scale(0.5f, 0.5f, 0.5f);
XMFLOAT3 cube2Rotation(0.0f, 45.0f, 0.0f);
XMFLOAT3 cube2Color(0.8f, 0.8f, 0.8f);

// Animation
bool animateLight = false;
bool animateCube = false;
float animationSpeed = 1.0f;

int lightType = 0;
float lightConstant = 1.0f;
float lightLinear = 0.09f;
float lightQuadratic = 0.032f;
XMFLOAT3 lightColor(1.0f, 1.0f, 1.0f);

// Gun viewmodel
struct GunModel {
    bool visible = true;
    bool loaded = false;
    XMFLOAT3 color = XMFLOAT3(0.2f, 0.2f, 0.25f);
    XMFLOAT3 offset = XMFLOAT3(0.12f, -0.2f, 0.5f);  // x=right, y=down, z=forward
    XMFLOAT3 scale = XMFLOAT3(0.08f, 0.08f, 0.3f);
    XMFLOAT3 rotation = XMFLOAT3(0.0f, 180.0f, 0.0f);
};
GunModel gunModel;

// Projectiles
struct Projectile {
    XMFLOAT3 position;
    XMFLOAT3 direction;
    float speed;
    float lifetime;
    bool active;
};
std::vector<Projectile> projectiles;
float projectileSpeed = 50.0f;
float projectileLifetime = 3.0f;
XMFLOAT3 projectileColor(1.0f, 0.8f, 0.0f);
float projectileScale = 0.1f;

// Light Editor Mode
bool lightEditorMode = false;
int selectedLightIndex = -1;
XMFLOAT3 newLightColor(1.0f, 1.0f, 1.0f);
float newLightRadius = 8.0f;
float newLightIntensity = 1.5f;

// Gizmo for moving lights
enum GizmoAxis { GIZMO_NONE = 0, GIZMO_X, GIZMO_Y, GIZMO_Z };
GizmoAxis activeGizmoAxis = GIZMO_NONE;
bool isDraggingGizmo = false;
XMFLOAT3 dragStartPos;
float gizmoSize = 1.0f;

// Clustered rendering
ClusteredRendererDX12 clusteredRenderer;
bool useClusteredRendering = true;

// DDGI Global Illumination settings
bool useDDGI = true;
float giIntensity = 0.5f;
float normalBias = 0.1f;
float probeSpacing = 2.0f;
bool showProbes = false;

// Point lights settings
int numDemoLights = 64;
float demoLightRadius = 8.0f;
float demoLightIntensity = 1.5f;
bool animateDemoLights = false;

// DX12 resources
ShaderDX12 mainShader;
ShaderDX12 depthShader;  // For shadow map rendering
ComPtr<ID3D12Resource> cubeVertexBuffer;
ComPtr<ID3D12Resource> planeVertexBuffer;
D3D12_VERTEX_BUFFER_VIEW cubeVBV = {};
D3D12_VERTEX_BUFFER_VIEW planeVBV = {};

// Shadow map resources
const UINT SHADOW_WIDTH = 2048;
const UINT SHADOW_HEIGHT = 2048;
ComPtr<ID3D12Resource> shadowMap;
ComPtr<ID3D12DescriptorHeap> shadowDsvHeap;
ComPtr<ID3D12DescriptorHeap> shadowSrvHeap;

// Vertex structure
struct Vertex {
    XMFLOAT3 position;
    XMFLOAT3 normal;
    XMFLOAT2 texCoord;
};

// Timer
class Timer {
    std::chrono::high_resolution_clock::time_point startTime;
public:
    void Start() { startTime = std::chrono::high_resolution_clock::now(); }
    float GetElapsed() {
        auto now = std::chrono::high_resolution_clock::now();
        return std::chrono::duration<float>(now - startTime).count();
    }
};
Timer gameTimer;

// ImGui descriptor heap for DX12
ComPtr<ID3D12DescriptorHeap> imguiSrvHeap;

// Use scene graph for loaded models
std::shared_ptr<SceneNode> loadedModelNode;

// Forward declarations
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Create vertex buffer helper
bool CreateVertexBuffer(const std::vector<Vertex>& vertices, ComPtr<ID3D12Resource>& buffer, D3D12_VERTEX_BUFFER_VIEW& vbv) {
    UINT bufferSize = (UINT)(vertices.size() * sizeof(Vertex));
    
    // Create upload heap
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
        IID_PPV_ARGS(&buffer));
    
    if (FAILED(hr)) return false;
    
    // Copy data
    void* mappedData;
    D3D12_RANGE readRange = { 0, 0 };
    hr = buffer->Map(0, &readRange, &mappedData);
    if (FAILED(hr)) return false;
    
    memcpy(mappedData, vertices.data(), bufferSize);
    buffer->Unmap(0, nullptr);
    
    // Create view
    vbv.BufferLocation = buffer->GetGPUVirtualAddress();
    vbv.SizeInBytes = bufferSize;
    vbv.StrideInBytes = sizeof(Vertex);
    
    return true;
}

// Create geometry
bool CreateGeometry() {
    // Cube vertices
    std::vector<Vertex> cubeVertices = {
        // Front face
        {{-0.5f, -0.5f,  0.5f}, { 0.0f,  0.0f,  1.0f}, {0.0f, 0.0f}},
        {{ 0.5f, -0.5f,  0.5f}, { 0.0f,  0.0f,  1.0f}, {1.0f, 0.0f}},
        {{ 0.5f,  0.5f,  0.5f}, { 0.0f,  0.0f,  1.0f}, {1.0f, 1.0f}},
        {{-0.5f, -0.5f,  0.5f}, { 0.0f,  0.0f,  1.0f}, {0.0f, 0.0f}},
        {{ 0.5f,  0.5f,  0.5f}, { 0.0f,  0.0f,  1.0f}, {1.0f, 1.0f}},
        {{-0.5f,  0.5f,  0.5f}, { 0.0f,  0.0f,  1.0f}, {0.0f, 1.0f}},
        // Back face
        {{ 0.5f, -0.5f, -0.5f}, { 0.0f,  0.0f, -1.0f}, {0.0f, 0.0f}},
        {{-0.5f, -0.5f, -0.5f}, { 0.0f,  0.0f, -1.0f}, {1.0f, 0.0f}},
        {{-0.5f,  0.5f, -0.5f}, { 0.0f,  0.0f, -1.0f}, {1.0f, 1.0f}},
        {{ 0.5f, -0.5f, -0.5f}, { 0.0f,  0.0f, -1.0f}, {0.0f, 0.0f}},
        {{-0.5f,  0.5f, -0.5f}, { 0.0f,  0.0f, -1.0f}, {1.0f, 1.0f}},
        {{ 0.5f,  0.5f, -0.5f}, { 0.0f,  0.0f, -1.0f}, {0.0f, 1.0f}},
        // Top face
        {{-0.5f,  0.5f,  0.5f}, { 0.0f,  1.0f,  0.0f}, {0.0f, 0.0f}},
        {{ 0.5f,  0.5f,  0.5f}, { 0.0f,  1.0f,  0.0f}, {1.0f, 0.0f}},
        {{ 0.5f,  0.5f, -0.5f}, { 0.0f,  1.0f,  0.0f}, {1.0f, 1.0f}},
        {{-0.5f,  0.5f,  0.5f}, { 0.0f,  1.0f,  0.0f}, {0.0f, 0.0f}},
        {{ 0.5f,  0.5f, -0.5f}, { 0.0f,  1.0f,  0.0f}, {1.0f, 1.0f}},
        {{-0.5f,  0.5f, -0.5f}, { 0.0f,  1.0f,  0.0f}, {0.0f, 1.0f}},
        // Bottom face
        {{-0.5f, -0.5f, -0.5f}, { 0.0f, -1.0f,  0.0f}, {0.0f, 0.0f}},
        {{ 0.5f, -0.5f, -0.5f}, { 0.0f, -1.0f,  0.0f}, {1.0f, 0.0f}},
        {{ 0.5f, -0.5f,  0.5f}, { 0.0f, -1.0f,  0.0f}, {1.0f, 1.0f}},
        {{-0.5f, -0.5f, -0.5f}, { 0.0f, -1.0f,  0.0f}, {0.0f, 0.0f}},
        {{ 0.5f, -0.5f,  0.5f}, { 0.0f, -1.0f,  0.0f}, {1.0f, 1.0f}},
        {{-0.5f, -0.5f,  0.5f}, { 0.0f, -1.0f,  0.0f}, {0.0f, 1.0f}},
        // Right face
        {{ 0.5f, -0.5f,  0.5f}, { 1.0f,  0.0f,  0.0f}, {0.0f, 0.0f}},
        {{ 0.5f, -0.5f, -0.5f}, { 1.0f,  0.0f,  0.0f}, {1.0f, 0.0f}},
        {{ 0.5f,  0.5f, -0.5f}, { 1.0f,  0.0f,  0.0f}, {1.0f, 1.0f}},
        {{ 0.5f, -0.5f,  0.5f}, { 1.0f,  0.0f,  0.0f}, {0.0f, 0.0f}},
        {{ 0.5f,  0.5f, -0.5f}, { 1.0f,  0.0f,  0.0f}, {1.0f, 1.0f}},
        {{ 0.5f,  0.5f,  0.5f}, { 1.0f,  0.0f,  0.0f}, {0.0f, 1.0f}},
        // Left face
        {{-0.5f, -0.5f, -0.5f}, {-1.0f,  0.0f,  0.0f}, {0.0f, 0.0f}},
        {{-0.5f, -0.5f,  0.5f}, {-1.0f,  0.0f,  0.0f}, {1.0f, 0.0f}},
        {{-0.5f,  0.5f,  0.5f}, {-1.0f,  0.0f,  0.0f}, {1.0f, 1.0f}},
        {{-0.5f, -0.5f, -0.5f}, {-1.0f,  0.0f,  0.0f}, {0.0f, 0.0f}},
        {{-0.5f,  0.5f,  0.5f}, {-1.0f,  0.0f,  0.0f}, {1.0f, 1.0f}},
        {{-0.5f,  0.5f, -0.5f}, {-1.0f,  0.0f,  0.0f}, {0.0f, 1.0f}},
    };
    
    if (!CreateVertexBuffer(cubeVertices, cubeVertexBuffer, cubeVBV)) {
        std::cerr << "Failed to create cube vertex buffer" << std::endl;
        return false;
    }
    
    // Plane vertices
    float planeSize = 25.0f;
    std::vector<Vertex> planeVertices = {
        {{-planeSize, -0.5f,  planeSize}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f}},
        {{ planeSize, -0.5f,  planeSize}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f}},
        {{ planeSize, -0.5f, -planeSize}, {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f}},
        {{-planeSize, -0.5f,  planeSize}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f}},
        {{ planeSize, -0.5f, -planeSize}, {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f}},
        {{-planeSize, -0.5f, -planeSize}, {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f}},
    };
    
    if (!CreateVertexBuffer(planeVertices, planeVertexBuffer, planeVBV)) {
        std::cerr << "Failed to create plane vertex buffer" << std::endl;
        return false;
    }
    
    return true;
}

void DrawCube() {
    g_dx12.commandList->IASetVertexBuffers(0, 1, &cubeVBV);
    g_dx12.commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_dx12.commandList->DrawInstanced(36, 1, 0, 0);
}

void DrawPlane() {
    g_dx12.commandList->IASetVertexBuffers(0, 1, &planeVBV);
    g_dx12.commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_dx12.commandList->DrawInstanced(6, 1, 0, 0);
}

bool CreateShadowMap() {
    // Create shadow map depth texture
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    
    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = SHADOW_WIDTH;
    texDesc.Height = SHADOW_HEIGHT;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R32_TYPELESS;  // Typeless for both DSV and SRV
    texDesc.SampleDesc.Count = 1;
    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    
    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = DXGI_FORMAT_D32_FLOAT;
    clearValue.DepthStencil.Depth = 1.0f;
    
    HRESULT hr = g_dx12.device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &texDesc,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clearValue,
        IID_PPV_ARGS(&shadowMap));
    if (FAILED(hr)) {
        std::cerr << "Failed to create shadow map texture" << std::endl;
        return false;
    }
    
    // Create DSV heap for shadow map
    D3D12_DESCRIPTOR_HEAP_DESC dsvHeapDesc = {};
    dsvHeapDesc.NumDescriptors = 1;
    dsvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    hr = g_dx12.device->CreateDescriptorHeap(&dsvHeapDesc, IID_PPV_ARGS(&shadowDsvHeap));
    if (FAILED(hr)) {
        std::cerr << "Failed to create shadow DSV heap" << std::endl;
        return false;
    }
    
    // Create DSV
    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    g_dx12.device->CreateDepthStencilView(shadowMap.Get(), &dsvDesc, 
        shadowDsvHeap->GetCPUDescriptorHandleForHeapStart());
    
    // Create SRV heap for shadow map (shader visible)
    D3D12_DESCRIPTOR_HEAP_DESC srvHeapDesc = {};
    srvHeapDesc.NumDescriptors = 1;
    srvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = g_dx12.device->CreateDescriptorHeap(&srvHeapDesc, IID_PPV_ARGS(&shadowSrvHeap));
    if (FAILED(hr)) {
        std::cerr << "Failed to create shadow SRV heap" << std::endl;
        return false;
    }
    
    // Create SRV
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    g_dx12.device->CreateShaderResourceView(shadowMap.Get(), &srvDesc,
        shadowSrvHeap->GetCPUDescriptorHandleForHeapStart());
    
    std::cout << "Shadow map created: " << SHADOW_WIDTH << "x" << SHADOW_HEIGHT << std::endl;
    return true;
}

void ToggleFullscreen(HWND hwnd) {
    if (!isFullscreen) {
        GetWindowRect(hwnd, &windowedRect);
        windowedStyle = GetWindowLong(hwnd, GWL_STYLE);
        
        HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfo(hMon, &mi);
        
        SetWindowLong(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(hwnd, HWND_TOP,
            mi.rcMonitor.left, mi.rcMonitor.top,
            mi.rcMonitor.right - mi.rcMonitor.left,
            mi.rcMonitor.bottom - mi.rcMonitor.top,
            SWP_FRAMECHANGED | SWP_NOCOPYBITS);
        
        isFullscreen = true;
    } else {
        SetWindowLong(hwnd, GWL_STYLE, windowedStyle);
        SetWindowPos(hwnd, HWND_NOTOPMOST,
            windowedRect.left, windowedRect.top,
            windowedRect.right - windowedRect.left,
            windowedRect.bottom - windowedRect.top,
            SWP_FRAMECHANGED | SWP_NOCOPYBITS);
        
        isFullscreen = false;
    }
}

void ClipCursorToWindow(HWND hwnd) {
    RECT rect;
    GetClientRect(hwnd, &rect);
    POINT topLeft = { rect.left, rect.top };
    POINT bottomRight = { rect.right, rect.bottom };
    ClientToScreen(hwnd, &topLeft);
    ClientToScreen(hwnd, &bottomRight);
    RECT clipRect = { topLeft.x, topLeft.y, bottomRight.x, bottomRight.y };
    ClipCursor(&clipRect);
}

// Ray-sphere intersection for picking lights
bool RaySphereIntersect(XMFLOAT3 rayOrigin, XMFLOAT3 rayDir, XMFLOAT3 sphereCenter, float sphereRadius, float& t) {
    XMVECTOR o = XMLoadFloat3(&rayOrigin);
    XMVECTOR d = XMVector3Normalize(XMLoadFloat3(&rayDir));
    XMVECTOR c = XMLoadFloat3(&sphereCenter);
    XMVECTOR oc = XMVectorSubtract(o, c);
    
    float a = XMVectorGetX(XMVector3Dot(d, d));
    float b = 2.0f * XMVectorGetX(XMVector3Dot(oc, d));
    XMFLOAT3 ocf;
    XMStoreFloat3(&ocf, oc);
    float c_val = ocf.x * ocf.x + ocf.y * ocf.y + ocf.z * ocf.z - sphereRadius * sphereRadius;
    float discriminant = b * b - 4 * a * c_val;
    
    if (discriminant < 0) return false;
    
    t = (-b - sqrtf(discriminant)) / (2.0f * a);
    return t > 0;
}

// Get ray from screen position
void ScreenToWorldRay(float screenX, float screenY, XMFLOAT3& rayOrigin, XMFLOAT3& rayDir) {
    // Convert to normalized device coordinates
    float ndcX = (2.0f * screenX / SCR_WIDTH) - 1.0f;
    float ndcY = 1.0f - (2.0f * screenY / SCR_HEIGHT);
    
    XMMATRIX view = camera.GetViewMatrix();
    XMMATRIX proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(cameraFOV), 
        (float)SCR_WIDTH / (float)SCR_HEIGHT, cameraNear, cameraFar);
    XMMATRIX invView = XMMatrixInverse(nullptr, view);
    XMMATRIX invProj = XMMatrixInverse(nullptr, proj);
    
    // Near point in clip space
    XMVECTOR nearPoint = XMVectorSet(ndcX, ndcY, 0.0f, 1.0f);
    // Far point in clip space
    XMVECTOR farPoint = XMVectorSet(ndcX, ndcY, 1.0f, 1.0f);
    
    // Transform to world space
    nearPoint = XMVector4Transform(nearPoint, invProj);
    nearPoint = XMVectorDivide(nearPoint, XMVectorSplatW(nearPoint));
    nearPoint = XMVector4Transform(nearPoint, invView);
    
    farPoint = XMVector4Transform(farPoint, invProj);
    farPoint = XMVectorDivide(farPoint, XMVectorSplatW(farPoint));
    farPoint = XMVector4Transform(farPoint, invView);
    
    XMStoreFloat3(&rayOrigin, nearPoint);
    XMVECTOR dir = XMVector3Normalize(XMVectorSubtract(farPoint, nearPoint));
    XMStoreFloat3(&rayDir, dir);
}

// Pick a light with mouse click
int PickLight(float screenX, float screenY) {
    XMFLOAT3 rayOrigin, rayDir;
    ScreenToWorldRay(screenX, screenY, rayOrigin, rayDir);
    
    int closestLight = -1;
    float closestT = FLT_MAX;
    
    for (int i = 0; i < clusteredRenderer.getTotalLightCount(); i++) {
        PointLightDX12* light = clusteredRenderer.getLight(i);
        if (!light || !light->active) continue;
        
        float t;
        if (RaySphereIntersect(rayOrigin, rayDir, light->position, 0.3f, t)) {
            if (t < closestT) {
                closestT = t;
                closestLight = i;
            }
        }
    }
    return closestLight;
}

// Check gizmo axis intersection
GizmoAxis PickGizmoAxis(float screenX, float screenY, XMFLOAT3 gizmoCenter) {
    XMFLOAT3 rayOrigin, rayDir;
    ScreenToWorldRay(screenX, screenY, rayOrigin, rayDir);
    
    // Check each axis (represented as thin cylinders/boxes)
    float closestT = FLT_MAX;
    GizmoAxis closest = GIZMO_NONE;
    float t;
    
    // X axis (red)
    XMFLOAT3 xEnd = XMFLOAT3(gizmoCenter.x + gizmoSize, gizmoCenter.y, gizmoCenter.z);
    if (RaySphereIntersect(rayOrigin, rayDir, XMFLOAT3(gizmoCenter.x + gizmoSize * 0.5f, gizmoCenter.y, gizmoCenter.z), 0.15f, t)) {
        if (t < closestT) { closestT = t; closest = GIZMO_X; }
    }
    
    // Y axis (green)
    if (RaySphereIntersect(rayOrigin, rayDir, XMFLOAT3(gizmoCenter.x, gizmoCenter.y + gizmoSize * 0.5f, gizmoCenter.z), 0.15f, t)) {
        if (t < closestT) { closestT = t; closest = GIZMO_Y; }
    }
    
    // Z axis (blue)
    if (RaySphereIntersect(rayOrigin, rayDir, XMFLOAT3(gizmoCenter.x, gizmoCenter.y, gizmoCenter.z + gizmoSize * 0.5f), 0.15f, t)) {
        if (t < closestT) { closestT = t; closest = GIZMO_Z; }
    }
    
    return closest;
}

// Place a new light at world position
void PlaceLightAtScreenPos(float screenX, float screenY) {
    XMFLOAT3 rayOrigin, rayDir;
    ScreenToWorldRay(screenX, screenY, rayOrigin, rayDir);
    
    // Place light 5 units in front of camera
    float distance = 5.0f;
    XMFLOAT3 newPos;
    newPos.x = rayOrigin.x + rayDir.x * distance;
    newPos.y = rayOrigin.y + rayDir.y * distance;
    newPos.z = rayOrigin.z + rayDir.z * distance;
    
    int newIdx = clusteredRenderer.addLight(newPos, newLightColor, newLightRadius, newLightIntensity);
    if (newIdx >= 0) {
        selectedLightIndex = newIdx;
    }
}

void ProcessInput(HWND hwnd) {
    if (!cameraLocked && !(showUI && ImGui::GetIO().WantCaptureKeyboard)) {
        if (GetAsyncKeyState('W') & 0x8000) camera.ProcessKeyboard('W', deltaTime);
        if (GetAsyncKeyState('S') & 0x8000) camera.ProcessKeyboard('S', deltaTime);
        if (GetAsyncKeyState('A') & 0x8000) camera.ProcessKeyboard('A', deltaTime);
        if (GetAsyncKeyState('D') & 0x8000) camera.ProcessKeyboard('D', deltaTime);
        
        static bool spacePressed = false;
        if (GetAsyncKeyState(VK_SPACE) & 0x8000) {
            if (!spacePressed) {
                camera.Jump();
                spacePressed = true;
            }
        } else {
            spacePressed = false;
        }
        
        if (GetAsyncKeyState(VK_SHIFT) & 0x8000) camera.ProcessKeyboard('Q', deltaTime);
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
        return true;
    
    switch (msg) {
    case WM_SIZE:
        if (g_dx12.device && g_dx12.initialized && wParam != SIZE_MINIMIZED) {
            unsigned int newWidth = LOWORD(lParam);
            unsigned int newHeight = HIWORD(lParam);
            if (newWidth > 0 && newHeight > 0 && (newWidth != SCR_WIDTH || newHeight != SCR_HEIGHT)) {
                WaitForGPU();
                SCR_WIDTH = newWidth;
                SCR_HEIGHT = newHeight;
                ResizeDX12(SCR_WIDTH, SCR_HEIGHT);
                // Re-clip cursor if camera is unlocked
                if (!cameraLocked) {
                    ClipCursorToWindow(hwnd);
                }
            }
        }
        return 0;
    
    case WM_MOVE:
        // Re-clip cursor when window moves if camera is unlocked
        if (!cameraLocked) {
            ClipCursorToWindow(hwnd);
        }
        return 0;
        
    case WM_MOUSEMOVE:
        if (!ImGui::GetIO().WantCaptureMouse) {
            float xpos = (float)GET_X_LPARAM(lParam);
            float ypos = (float)GET_Y_LPARAM(lParam);
            
            // Handle gizmo dragging in light editor mode
            if (lightEditorMode && isDraggingGizmo && selectedLightIndex >= 0) {
                PointLightDX12* light = clusteredRenderer.getLight(selectedLightIndex);
                if (light) {
                    float dragSpeed = 0.02f;
                    float dx = xpos - lastX;
                    float dy = lastY - ypos;
                    
                    XMFLOAT3 pos = light->position;
                    if (activeGizmoAxis == GIZMO_X) {
                        pos.x += dx * dragSpeed;
                    } else if (activeGizmoAxis == GIZMO_Y) {
                        pos.y += dy * dragSpeed;
                    } else if (activeGizmoAxis == GIZMO_Z) {
                        pos.z += dx * dragSpeed;
                    }
                    clusteredRenderer.updateLight(selectedLightIndex, pos);
                }
                lastX = xpos;
                lastY = ypos;
            }
            // Normal camera control when not in editor mode
            else if (!cameraLocked && !lightEditorMode) {
                RECT rect;
                GetClientRect(hwnd, &rect);
                int centerX = (rect.right - rect.left) / 2;
                int centerY = (rect.bottom - rect.top) / 2;
                
                if (firstMouse) {
                    lastX = (float)centerX;
                    lastY = (float)centerY;
                    firstMouse = false;
                }
                
                // Calculate offset from center
                float xoffset = xpos - (float)centerX;
                float yoffset = (float)centerY - ypos;
                
                camera.ProcessMouseMovement(xoffset, yoffset);
                
                // Reset cursor to center for infinite mouse movement
                POINT center = { centerX, centerY };
                ClientToScreen(hwnd, &center);
                SetCursorPos(center.x, center.y);
            }
        }
        return 0;
        
    case WM_LBUTTONDOWN:
        if (!ImGui::GetIO().WantCaptureMouse) {
            float xpos = (float)GET_X_LPARAM(lParam);
            float ypos = (float)GET_Y_LPARAM(lParam);
            
            // Light editor mode - pick lights or gizmo
            if (lightEditorMode) {
                // Check if clicking on gizmo first
                if (selectedLightIndex >= 0) {
                    PointLightDX12* light = clusteredRenderer.getLight(selectedLightIndex);
                    if (light) {
                        GizmoAxis axis = PickGizmoAxis(xpos, ypos, light->position);
                        if (axis != GIZMO_NONE) {
                            activeGizmoAxis = axis;
                            isDraggingGizmo = true;
                            lastX = xpos;
                            lastY = ypos;
                            SetCapture(hwnd);
                            return 0;
                        }
                    }
                }
                
                // Check if clicking on a light
                int pickedLight = PickLight(xpos, ypos);
                if (pickedLight >= 0) {
                    selectedLightIndex = pickedLight;
                } else {
                    // Deselect if clicking on empty space
                    selectedLightIndex = -1;
                }
            }
            // Normal mode
            else if (cameraLocked) {
                cameraLocked = false;
                SetCapture(hwnd);
                ShowCursor(FALSE);
                ClipCursorToWindow(hwnd);  // Lock mouse to window
                RECT rect;
                GetClientRect(hwnd, &rect);
                POINT center = { (rect.right - rect.left) / 2, (rect.bottom - rect.top) / 2 };
                ClientToScreen(hwnd, &center);
                SetCursorPos(center.x, center.y);
                lastX = (float)(rect.right - rect.left) / 2;
                lastY = (float)(rect.bottom - rect.top) / 2;
                firstMouse = true;
            } else {
                // Shoot projectile
                Projectile proj;
                proj.position = camera.Position;
                proj.direction = camera.Front;
                proj.speed = projectileSpeed;
                proj.lifetime = projectileLifetime;
                proj.active = true;
                projectiles.push_back(proj);
            }
        }
        return 0;
        
    case WM_LBUTTONUP:
        if (isDraggingGizmo) {
            isDraggingGizmo = false;
            activeGizmoAxis = GIZMO_NONE;
            ReleaseCapture();
        }
        return 0;
        
    case WM_RBUTTONDOWN:
        if (!ImGui::GetIO().WantCaptureMouse && lightEditorMode) {
            float xpos = (float)GET_X_LPARAM(lParam);
            float ypos = (float)GET_Y_LPARAM(lParam);
            // Right click to place a new light
            PlaceLightAtScreenPos(xpos, ypos);
        }
        return 0;
        
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            PostQuitMessage(0);
        } else if (wParam == VK_TAB) {
            showUI = !showUI;
            if (showUI) {
                cameraLocked = true;
                ReleaseCapture();
                ClipCursor(nullptr);  // Release cursor clip
                ShowCursor(TRUE);
            }
        } else if (wParam == 'C') {
            cameraLocked = true;
            ReleaseCapture();
            ClipCursor(nullptr);  // Release cursor clip
            ShowCursor(TRUE);
        } else if (wParam == 'F') {
            camera.FPSMode = !camera.FPSMode;
            if (camera.FPSMode) {
                camera.Position.y = camera.FloorY + camera.PlayerHeight;
            }
        } else if (wParam == VK_F11) {
            ToggleFullscreen(hwnd);
        } else if (wParam == 'L') {
            // Toggle light editor mode
            lightEditorMode = !lightEditorMode;
            if (lightEditorMode) {
                cameraLocked = true;
                ReleaseCapture();
                ClipCursor(nullptr);
                ShowCursor(TRUE);
            }
            selectedLightIndex = -1;
        } else if (wParam == VK_DELETE) {
            // Delete selected light
            if (lightEditorMode && selectedLightIndex >= 0) {
                clusteredRenderer.removeLight(selectedLightIndex);
                selectedLightIndex = -1;
            }
        }
        return 0;
        
    case WM_DESTROY:
        ClipCursor(nullptr);  // Release cursor clip on exit
        PostQuitMessage(0);
        return 0;
    }
    
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// Helper to display recursive scene graph in ImGui
void ShowSceneGraphNode(SceneNode* node) {
    if (!node) return;
    
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_OpenOnDoubleClick | ImGuiTreeNodeFlags_DefaultOpen;
    if (node->children.empty()) {
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_Bullet;
    }
    
    // Node name and properties
    bool nodeOpen = ImGui::TreeNodeEx((void*)node, flags, "%s", node->name.c_str());
    
    if (nodeOpen) {
        // Transform controls
        ImGui::PushID(node);
        ImGui::DragFloat3("Position", &node->translation.x, 0.1f);
        
        // Convert quaternion to Euler angles for editing - this is simplified, proper implementation needs conversion
        // For now just expose as 4 floats or assume euler if we change SceneNode to use Euler
        // Assuming quaternion for now, let's just show it but maybe not edit it easily without conversion
        // ImGui::DragFloat4("Rotation (Quat)", &node->rotation.x, 0.01f);
        
        ImGui::DragFloat3("Scale", &node->scale.x, 0.1f);
        ImGui::PopID();
        
        // Children
        for (auto& child : node->children) {
            ShowSceneGraphNode(child.get());
        }
        
        ImGui::TreePop();
    }
}

// Recursive draw function for Shadow Pass
void RecursiveDrawShadow(std::shared_ptr<SceneNode> node, const XMMATRIX& lightSpaceMatrix) {
    if (!node) return;
    
    if (node->mesh) {
        XMMATRIX model = XMLoadFloat4x4(&node->globalTransform);
        depthShader.SetMatrices(model, XMMatrixIdentity(), XMMatrixIdentity(), lightSpaceMatrix);
        
        for (const auto& prim : node->mesh->primitives) {
             if (prim.vbv.BufferLocation != 0) {
                 g_dx12.commandList->IASetVertexBuffers(0, 1, &prim.vbv);
                 g_dx12.commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                 
                 if (prim.ibv.BufferLocation != 0) {
                     g_dx12.commandList->IASetIndexBuffer(&prim.ibv);
                     g_dx12.commandList->DrawIndexedInstanced(prim.indexCount, 1, 0, 0, 0);
                 } else {
                     g_dx12.commandList->DrawInstanced((UINT)(prim.vertices.size() / 8), 1, 0, 0);
                 }
             }
        }
    }
    
    for (auto& child : node->children) {
        RecursiveDrawShadow(child, lightSpaceMatrix);
    }
}

// Recursive draw function for SceneGraph
void RecursiveDraw(std::shared_ptr<SceneNode> node, const XMMATRIX& view, const XMMATRIX& projection, const XMMATRIX& lightSpaceMatrix) {
    if (!node) return;
    
    // Draw mesh if exists
    if (node->mesh) {
        XMMATRIX model = XMLoadFloat4x4(&node->globalTransform);
        mainShader.SetMatrices(model, view, projection, lightSpaceMatrix);
        
        // For each primitive
        for (const auto& prim : node->mesh->primitives) {
             if (prim.vbv.BufferLocation != 0) {
                 // Set material/color (default white for now)
                 mainShader.SetObjectColor(XMFLOAT3(1.0f, 1.0f, 1.0f)); 
                 
                 g_dx12.commandList->IASetVertexBuffers(0, 1, &prim.vbv);
                 g_dx12.commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                 
                 if (prim.ibv.BufferLocation != 0) {
                     g_dx12.commandList->IASetIndexBuffer(&prim.ibv);
                     g_dx12.commandList->DrawIndexedInstanced(prim.indexCount, 1, 0, 0, 0);
                 } else {
                     g_dx12.commandList->DrawInstanced((UINT)(prim.vertices.size() / 8), 1, 0, 0);
                 }
             }
        }
    }
    
    for (auto& child : node->children) {
        RecursiveDraw(child, view, projection, lightSpaceMatrix);
    }
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    // Create console
    AllocConsole();
    FILE* pCout;
    freopen_s(&pCout, "CONOUT$", "w", stdout);
    freopen_s(&pCout, "CONOUT$", "w", stderr);
    
    std::cout << "GraphicEngine DX12 Starting..." << std::endl;
    
    // Register window class
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"GraphicEngineDX12";
    RegisterClassExW(&wc);
    
    // Create window
    RECT rc = { 0, 0, (LONG)SCR_WIDTH, (LONG)SCR_HEIGHT };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    
    HWND hwnd = CreateWindowW(L"GraphicEngineDX12", L"Graphics Engine - DirectX 12",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, hInstance, nullptr);
    
    if (!hwnd) {
        std::cerr << "Failed to create window" << std::endl;
        return -1;
    }
    
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);
    
    // Initialize DX12
    try {
        if (!InitDX12(hwnd, SCR_WIDTH, SCR_HEIGHT)) {
            std::cerr << "Failed to initialize DirectX 12" << std::endl;
            MessageBoxA(hwnd, "Failed to initialize DirectX 12.\nMake sure you have a DX12 compatible GPU.", "DX12 Error", MB_OK | MB_ICONERROR);
            return -1;
        }
    } catch (const std::exception& e) {
        std::cerr << "Exception during DX12 init: " << e.what() << std::endl;
        MessageBoxA(hwnd, e.what(), "DX12 Error", MB_OK | MB_ICONERROR);
        return -1;
    }
    
    // Create ImGui descriptor heap
    D3D12_DESCRIPTOR_HEAP_DESC imguiHeapDesc = {};
    imguiHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    imguiHeapDesc.NumDescriptors = 1;
    imguiHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    g_dx12.device->CreateDescriptorHeap(&imguiHeapDesc, IID_PPV_ARGS(&imguiSrvHeap));
    
    // Setup ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX12_Init(g_dx12.device.Get(), FRAME_COUNT,
        DXGI_FORMAT_R8G8B8A8_UNORM,
        imguiSrvHeap.Get(),
        imguiSrvHeap->GetCPUDescriptorHandleForHeapStart(),
        imguiSrvHeap->GetGPUDescriptorHandleForHeapStart());
    
    // Create geometry
    if (!CreateGeometry()) {
        std::cerr << "Failed to create geometry" << std::endl;
        return -1;
    }
    
    // Load shaders - trying DX12 specific shaders first
    if (!mainShader.Load("shaders/clustered_dx12_vs.hlsl", "shaders/clustered_dx12_ps.hlsl")) {
        std::cerr << "Failed to load DX12 specific shaders, trying general clustered shaders..." << std::endl;
        if (!mainShader.Load("shaders/clustered_vs.hlsl", "shaders/clustered_ps.hlsl")) {
            std::cerr << "Failed to load any shaders" << std::endl;
            MessageBoxA(hwnd, "Failed to load shaders.\nCheck console for details.", "Shader Error", MB_OK | MB_ICONERROR);
            return -1;
        }
    }
    std::cout << "Clustered Forward shaders loaded successfully" << std::endl;
    
    // Load model
    std::cout << "Attempting to load models/gun.glb..." << std::endl;
    loadedModelNode = GLBImporter::LoadGLB("models/gun.glb", g_dx12.device, g_dx12.commandList);
    if (loadedModelNode) {
        std::cout << "Model loaded successfully!" << std::endl;
        // Adjust model transform if needed (e.g. scale down)
        // loadedModelNode->scale = XMFLOAT3(0.1f, 0.1f, 0.1f);
        loadedModelNode->UpdateGlobalTransform(loadedModelNode->localTransform);
        gunModel.loaded = true;
    } else {
        std::cout << "Failed to load model or file not found." << std::endl;
    }

    
    // Load depth shader for shadow mapping
    if (!depthShader.Load("shaders/depth_vs.hlsl", "shaders/depth_ps.hlsl")) {
        std::cerr << "Failed to load depth shader - shadows will be disabled" << std::endl;
        enableShadows = false;
    } else {
        std::cout << "Depth shader loaded successfully" << std::endl;
    }
    
    // Create shadow map
    if (enableShadows && !CreateShadowMap()) {
        std::cerr << "Failed to create shadow map - shadows will be disabled" << std::endl;
        enableShadows = false;
    }
    
    // Initialize clustered renderer and demo lights
    clusteredRenderer.init();
    for (int i = 0; i < numDemoLights; i++) {
        float angle = (float)i / numDemoLights * XM_2PI;
        float radius = 8.0f;
        XMFLOAT3 pos(cosf(angle) * radius, 2.0f, sinf(angle) * radius);
        XMFLOAT3 color;
        color.x = sinf(angle) * 0.5f + 0.5f;
        color.y = sinf(angle + 2.094f) * 0.5f + 0.5f;
        color.z = sinf(angle + 4.189f) * 0.5f + 0.5f;
        clusteredRenderer.addLight(pos, color, demoLightRadius, demoLightIntensity);
    }
    
    gameTimer.Start();
    float lastTime = 0.0f;
    
    std::cout << "Controls:" << std::endl;
    std::cout << "  - WASD: Move camera" << std::endl;
    std::cout << "  - Mouse: Look around" << std::endl;
    std::cout << "  - TAB: Toggle UI" << std::endl;
    std::cout << "  - F11: Toggle fullscreen" << std::endl;
    std::cout << "  - ESC: Exit" << std::endl;
    
    // Main loop
    MSG msg = {};
    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            continue;
        }
        
        // Calculate delta time
        float currentTime = gameTimer.GetElapsed();
        deltaTime = currentTime - lastTime;
        lastTime = currentTime;
        
        // Process input
        ProcessInput(hwnd);
        camera.Update(deltaTime);
        
        // Animate lights
        if (animateDemoLights) {
            for (int i = 0; i < clusteredRenderer.getTotalLightCount(); i++) {
                float angle = (float)i / clusteredRenderer.getTotalLightCount() * XM_2PI + currentTime;
                float radius = 8.0f;
                XMFLOAT3 pos(
                    cosf(angle) * radius,
                    2.0f + sinf(currentTime * 2.0f + angle) * 1.0f,
                    sinf(angle) * radius
                );
                clusteredRenderer.updateLight(i, pos);
            }
        }
        
        // Animate light
        if (animateLight) {
            lightPos.x = cosf(currentTime * animationSpeed) * 10.0f;
            lightPos.z = sinf(currentTime * animationSpeed) * 10.0f;
        }
        
        // Animate cube
        if (animateCube) {
            cubeRotation.y = currentTime * 50.0f * animationSpeed;
        }
        
        // Update projectiles
        for (auto& proj : projectiles) {
            if (proj.active) {
                proj.position.x += proj.direction.x * proj.speed * deltaTime;
                proj.position.y += proj.direction.y * proj.speed * deltaTime;
                proj.position.z += proj.direction.z * proj.speed * deltaTime;
                proj.lifetime -= deltaTime;
                if (proj.lifetime <= 0.0f) {
                    proj.active = false;
                }
            }
        }
        
        // Remove inactive projectiles
        projectiles.erase(
            std::remove_if(projectiles.begin(), projectiles.end(),
                [](const Projectile& p) { return !p.active; }),
            projectiles.end());
        
        // Begin frame
        try {
            BeginFrame();
        } catch (const std::exception& e) {
            std::cerr << "BeginFrame error: " << e.what() << std::endl;
            break;
        }
        
        // Clear
        float clearColorArr[4] = { clearColor.x, clearColor.y, clearColor.z, 1.0f };
        ClearRenderTarget(clearColorArr);
        
        // Reset shader draw call counter at start of each frame
        mainShader.BeginFrame();
        depthShader.BeginFrame();
        
        // Setup matrices
        XMMATRIX view = camera.GetViewMatrix();
        XMMATRIX projection = XMMatrixPerspectiveFovLH(
            XMConvertToRadians(cameraFOV),
            (float)SCR_WIDTH / (float)SCR_HEIGHT,
            cameraNear, cameraFar);
        
        // Calculate light space matrix for shadows
        XMVECTOR lightPosVec = XMLoadFloat3(&lightPos);
        XMVECTOR lightTargetVec = XMLoadFloat3(&lightTarget);
        XMVECTOR lightUpVec = XMLoadFloat3(&lightUp);
        XMMATRIX lightView = XMMatrixLookAtLH(lightPosVec, lightTargetVec, lightUpVec);
        XMMATRIX lightProj = XMMatrixOrthographicLH(lightOrthoSize * 2.0f, lightOrthoSize * 2.0f, lightNear, lightFar);
        XMMATRIX lightSpaceMatrix = lightView * lightProj;
        
        // =====================
        // Shadow Pass
        // =====================
        if (enableShadows && shadowMap) {
            // Transition shadow map to depth write
            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = shadowMap.Get();
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            g_dx12.commandList->ResourceBarrier(1, &barrier);
            
            // Set shadow map viewport
            D3D12_VIEWPORT shadowViewport = {};
            shadowViewport.Width = (float)SHADOW_WIDTH;
            shadowViewport.Height = (float)SHADOW_HEIGHT;
            shadowViewport.MinDepth = 0.0f;
            shadowViewport.MaxDepth = 1.0f;
            g_dx12.commandList->RSSetViewports(1, &shadowViewport);
            
            D3D12_RECT shadowScissor = { 0, 0, (LONG)SHADOW_WIDTH, (LONG)SHADOW_HEIGHT };
            g_dx12.commandList->RSSetScissorRects(1, &shadowScissor);
            
            // Clear and set shadow map as render target (depth only)
            D3D12_CPU_DESCRIPTOR_HANDLE shadowDsv = shadowDsvHeap->GetCPUDescriptorHandleForHeapStart();
            g_dx12.commandList->ClearDepthStencilView(shadowDsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
            g_dx12.commandList->OMSetRenderTargets(0, nullptr, FALSE, &shadowDsv);
            
            // Use depth shader
            depthShader.Use(false);
            
            // Render floor shadow
            XMMATRIX model = XMMatrixIdentity();
            depthShader.SetMatrices(model, XMMatrixIdentity(), XMMatrixIdentity(), lightSpaceMatrix);
            DrawPlane();
            depthShader.NextDrawCall();
            
            // Render cube 1 shadow
            model = XMMatrixScaling(cubeScale.x, cubeScale.y, cubeScale.z);
            model = model * XMMatrixRotationX(XMConvertToRadians(cubeRotation.x));
            model = model * XMMatrixRotationY(XMConvertToRadians(cubeRotation.y));
            model = model * XMMatrixRotationZ(XMConvertToRadians(cubeRotation.z));
            model = model * XMMatrixTranslation(cubePosition.x, cubePosition.y, cubePosition.z);
            depthShader.SetMatrices(model, XMMatrixIdentity(), XMMatrixIdentity(), lightSpaceMatrix);
            DrawCube();
            depthShader.NextDrawCall();
            
            // Render cube 2 shadow
            if (showSecondCube) {
                model = XMMatrixScaling(cube2Scale.x, cube2Scale.y, cube2Scale.z);
                model = model * XMMatrixRotationX(XMConvertToRadians(cube2Rotation.x));
                model = model * XMMatrixRotationY(XMConvertToRadians(cube2Rotation.y));
                model = model * XMMatrixRotationZ(XMConvertToRadians(cube2Rotation.z));
                model = model * XMMatrixTranslation(cube2Position.x, cube2Position.y, cube2Position.z);
                depthShader.SetMatrices(model, XMMatrixIdentity(), XMMatrixIdentity(), lightSpaceMatrix);
                DrawCube();
                depthShader.NextDrawCall();
            }

            // Render loaded model shadow (scene prop mode)
            if (loadedModelNode && !gunModel.loaded) {
                 DirectX::XMFLOAT4X4 identity;
                 DirectX::XMStoreFloat4x4(&identity, DirectX::XMMatrixIdentity());
                 
                 // Update transforms for shadow pass
                 loadedModelNode->UpdateLocalTransform();
                 loadedModelNode->UpdateGlobalTransform(identity);
                 
                 RecursiveDrawShadow(loadedModelNode, lightSpaceMatrix);
            }
            
            // Transition shadow map to shader resource
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            g_dx12.commandList->ResourceBarrier(1, &barrier);
            
            // Restore main viewport
            g_dx12.commandList->RSSetViewports(1, &g_dx12.viewport);
            g_dx12.commandList->RSSetScissorRects(1, &g_dx12.scissorRect);
            
            // Restore render target
            D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = GetCPUDescriptorHandle(
                g_dx12.rtvHeap.Get(), g_dx12.rtvDescriptorSize, g_dx12.frameIndex);
            D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = g_dx12.dsvHeap->GetCPUDescriptorHandleForHeapStart();
            g_dx12.commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);
        }
        
        // Use shader
        mainShader.Use(wireframeMode);
        
        // Bind shadow map SRV if shadows are enabled
        if (enableShadows && shadowSrvHeap) {
            ID3D12DescriptorHeap* heaps[] = { shadowSrvHeap.Get() };
            g_dx12.commandList->SetDescriptorHeaps(1, heaps);
            g_dx12.commandList->SetGraphicsRootDescriptorTable(6, shadowSrvHeap->GetGPUDescriptorHandleForHeapStart());
        }
        
        // Set common uniforms
        mainShader.SetLight(lightPos, lightType, lightColor, lightConstant, lightLinear, lightQuadratic,
            ambientStrength, specularStrength, specularShininess, shadowBias, enableShadows);
        mainShader.SetCamera(camera.Position);
        
        // Update clustered renderer camera and cull lights
        clusteredRenderer.setScreenSize((float)SCR_WIDTH, (float)SCR_HEIGHT);
        clusteredRenderer.setCamera(cameraFOV, cameraNear, cameraFar, view, projection);
        clusteredRenderer.cullLights();
        
        // Set point lights from clustered renderer
        std::vector<PointLightDataDX12> lightData = clusteredRenderer.getPointLightData();
        mainShader.SetPointLights((int)lightData.size(), lightData);
        
        // Set DDGI parameters
        mainShader.SetDDGI(useDDGI, giIntensity, normalBias, probeSpacing);
        
        // Render floor
        XMMATRIX model = XMMatrixIdentity();
        mainShader.SetMatrices(model, view, projection, lightSpaceMatrix);
        mainShader.SetObjectColor(floorColor);
        DrawPlane();
        mainShader.NextDrawCall();
        
        // Render cube 1
        model = XMMatrixScaling(cubeScale.x, cubeScale.y, cubeScale.z);
        model = model * XMMatrixRotationX(XMConvertToRadians(cubeRotation.x));
        model = model * XMMatrixRotationY(XMConvertToRadians(cubeRotation.y));
        model = model * XMMatrixRotationZ(XMConvertToRadians(cubeRotation.z));
        model = model * XMMatrixTranslation(cubePosition.x, cubePosition.y, cubePosition.z);
        mainShader.SetMatrices(model, view, projection, lightSpaceMatrix);
        mainShader.SetObjectColor(cubeColor);
        DrawCube();
        mainShader.NextDrawCall();
        
        // Render cube 2
        if (showSecondCube) {
            model = XMMatrixScaling(cube2Scale.x, cube2Scale.y, cube2Scale.z);
            model = model * XMMatrixRotationX(XMConvertToRadians(cube2Rotation.x));
            model = model * XMMatrixRotationY(XMConvertToRadians(cube2Rotation.y));
            model = model * XMMatrixRotationZ(XMConvertToRadians(cube2Rotation.z));
            model = model * XMMatrixTranslation(cube2Position.x, cube2Position.y, cube2Position.z);
            mainShader.SetMatrices(model, view, projection, lightSpaceMatrix);
            mainShader.SetObjectColor(cube2Color);
            DrawCube();
            mainShader.NextDrawCall();
        }
        
        // Render loaded model if it exists and is NOT the gun (scene prop mode)
        if (loadedModelNode && !gunModel.loaded) {
             // Ensure transforms are up to date (e.g. if modified in ImGui)
             // We assume root node, so parent transform is identity
             DirectX::XMFLOAT4X4 identity;
             DirectX::XMStoreFloat4x4(&identity, DirectX::XMMatrixIdentity());
             
             loadedModelNode->UpdateLocalTransform();
             loadedModelNode->UpdateGlobalTransform(identity);
             
             RecursiveDraw(loadedModelNode, view, projection, lightSpaceMatrix);
        }
        
        // Render projectiles
        for (auto& proj : projectiles) {
            if (proj.active) {
                model = XMMatrixScaling(projectileScale, projectileScale, projectileScale);
                model = model * XMMatrixTranslation(proj.position.x, proj.position.y, proj.position.z);
                mainShader.SetMatrices(model, view, projection, lightSpaceMatrix);
                mainShader.SetObjectColor(projectileColor);
                DrawCube();
                mainShader.NextDrawCall();
            }
        }
        
        // Render gun viewmodel (simplified - just a cube for now)
        if (gunModel.visible) {
            // Position gun in front of camera using camera's coordinate system
            XMVECTOR camPos = XMLoadFloat3(&camera.Position);
            XMVECTOR camFront = XMVector3Normalize(XMLoadFloat3(&camera.Front));
            XMVECTOR worldUp = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
            XMVECTOR camRight = XMVector3Normalize(XMVector3Cross(worldUp, camFront));
            XMVECTOR camUp = XMVector3Cross(camFront, camRight);
            
            // Calculate gun position relative to camera
            XMVECTOR gunPos = camPos 
                + camFront * gunModel.offset.z 
                + camRight * gunModel.offset.x 
                + camUp * gunModel.offset.y;
            XMFLOAT3 gunPosition;
            XMStoreFloat3(&gunPosition, gunPos);
            
            // Build rotation matrix from camera orientation
            // This makes the gun follow the camera's view direction
            XMFLOAT3 right, up, front;
            XMStoreFloat3(&right, camRight);
            XMStoreFloat3(&up, camUp);
            XMStoreFloat3(&front, camFront);
            
            // Create rotation matrix from camera basis vectors
            XMMATRIX cameraRotation = XMMATRIX(
                right.x, right.y, right.z, 0.0f,
                up.x, up.y, up.z, 0.0f,
                front.x, front.y, front.z, 0.0f,
                0.0f, 0.0f, 0.0f, 1.0f
            );
            
            // Apply gun's local rotation offset, then camera rotation, then position
            model = XMMatrixScaling(gunModel.scale.x, gunModel.scale.y * 2.0f, gunModel.scale.z * 3.0f);
            model = model * XMMatrixRotationX(XMConvertToRadians(gunModel.rotation.x));
            model = model * XMMatrixRotationY(XMConvertToRadians(gunModel.rotation.y));
            model = model * XMMatrixRotationZ(XMConvertToRadians(gunModel.rotation.z));
            model = model * cameraRotation;
            model = model * XMMatrixTranslation(gunPosition.x, gunPosition.y, gunPosition.z);
            
            if (gunModel.loaded && loadedModelNode) {
                 XMFLOAT4X4 gunWorldTransform;
                 XMStoreFloat4x4(&gunWorldTransform, model);
                 loadedModelNode->UpdateGlobalTransform(gunWorldTransform);
                 RecursiveDraw(loadedModelNode, view, projection, lightSpaceMatrix);
                 // Note: RecursiveDraw sets matrices and draw calls internally
                 // We assume shader state (texture/constants) is compatible
            } else {
                mainShader.SetMatrices(model, view, projection, lightSpaceMatrix);
                mainShader.SetObjectColor(gunModel.color);
                DrawCube();
                mainShader.NextDrawCall();
            }
        }
        
        // Render light spheres
        for (int i = 0; i < clusteredRenderer.getTotalLightCount(); i++) {
            PointLightDX12* light = clusteredRenderer.getLight(i);
            if (!light || !light->active) continue;
            
            // Make selected light bigger
            float lightSize = (lightEditorMode && i == selectedLightIndex) ? 0.35f : 0.2f;
            model = XMMatrixScaling(lightSize, lightSize, lightSize);
            model = model * XMMatrixTranslation(light->position.x, light->position.y, light->position.z);
            mainShader.SetMatrices(model, view, projection, lightSpaceMatrix);
            XMFLOAT3 lightCol(light->color.x * light->intensity, light->color.y * light->intensity, light->color.z * light->intensity);
            mainShader.SetObjectColor(lightCol);
            DrawCube();
            mainShader.NextDrawCall();
        }
        
        // Render gizmo for selected light in editor mode
        if (lightEditorMode && selectedLightIndex >= 0) {
            PointLightDX12* light = clusteredRenderer.getLight(selectedLightIndex);
            if (light && light->active) {
                XMFLOAT3 pos = light->position;
                
                // X axis (red) - horizontal bar
                model = XMMatrixScaling(gizmoSize, 0.05f, 0.05f);
                model = model * XMMatrixTranslation(pos.x + gizmoSize * 0.5f, pos.y, pos.z);
                mainShader.SetMatrices(model, view, projection, lightSpaceMatrix);
                mainShader.SetObjectColor(XMFLOAT3(1.0f, 0.0f, 0.0f));
                DrawCube();
                mainShader.NextDrawCall();
                
                // X axis arrow head
                model = XMMatrixScaling(0.15f, 0.15f, 0.15f);
                model = model * XMMatrixTranslation(pos.x + gizmoSize, pos.y, pos.z);
                mainShader.SetMatrices(model, view, projection, lightSpaceMatrix);
                mainShader.SetObjectColor(XMFLOAT3(1.0f, 0.0f, 0.0f));
                DrawCube();
                mainShader.NextDrawCall();
                
                // Y axis (green) - vertical bar
                model = XMMatrixScaling(0.05f, gizmoSize, 0.05f);
                model = model * XMMatrixTranslation(pos.x, pos.y + gizmoSize * 0.5f, pos.z);
                mainShader.SetMatrices(model, view, projection, lightSpaceMatrix);
                mainShader.SetObjectColor(XMFLOAT3(0.0f, 1.0f, 0.0f));
                DrawCube();
                mainShader.NextDrawCall();
                
                // Y axis arrow head
                model = XMMatrixScaling(0.15f, 0.15f, 0.15f);
                model = model * XMMatrixTranslation(pos.x, pos.y + gizmoSize, pos.z);
                mainShader.SetMatrices(model, view, projection, lightSpaceMatrix);
                mainShader.SetObjectColor(XMFLOAT3(0.0f, 1.0f, 0.0f));
                DrawCube();
                mainShader.NextDrawCall();
                
                // Z axis (blue) - depth bar
                model = XMMatrixScaling(0.05f, 0.05f, gizmoSize);
                model = model * XMMatrixTranslation(pos.x, pos.y, pos.z + gizmoSize * 0.5f);
                mainShader.SetMatrices(model, view, projection, lightSpaceMatrix);
                mainShader.SetObjectColor(XMFLOAT3(0.0f, 0.0f, 1.0f));
                DrawCube();
                mainShader.NextDrawCall();
                
                // Z axis arrow head
                model = XMMatrixScaling(0.15f, 0.15f, 0.15f);
                model = model * XMMatrixTranslation(pos.x, pos.y, pos.z + gizmoSize);
                mainShader.SetMatrices(model, view, projection, lightSpaceMatrix);
                mainShader.SetObjectColor(XMFLOAT3(0.0f, 0.0f, 1.0f));
                DrawCube();
                mainShader.NextDrawCall();
            }
        }
        
        // Render ImGui
        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        
        if (showUI) {
            ImGui::Begin("Scene Controls", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
            
            ImGui::Text("Controls:");
            ImGui::BulletText("TAB: Toggle UI");
            ImGui::BulletText("C: Lock/Unlock Camera");
            ImGui::BulletText("F: Toggle FPS Walking Mode");
            ImGui::BulletText("L: Toggle Light Editor");
            ImGui::BulletText("Left Click: Lock camera / Shoot");
            if (lightEditorMode) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 0.0f, 1.0f));
                ImGui::Text("LIGHT EDITOR MODE ACTIVE");
                ImGui::PopStyleColor();
            }
            if (cameraLocked) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.5f, 0.0f, 1.0f));
                if (ImGui::Button("Camera LOCKED (Click to unlock)")) cameraLocked = false;
                ImGui::PopStyleColor();
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.0f, 1.0f, 0.0f, 1.0f));
                if (ImGui::Button("Camera UNLOCKED (Press C to lock)")) cameraLocked = true;
                ImGui::PopStyleColor();
            }
            ImGui::Separator();
            
            if (ImGui::CollapsingHeader("Camera Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::DragFloat3("Camera Position", &camera.Position.x, 0.1f);
                ImGui::DragFloat("FOV", &cameraFOV, 0.5f, 1.0f, 120.0f);
                ImGui::DragFloat("Near Plane", &cameraNear, 0.01f, 0.01f, 10.0f);
                ImGui::DragFloat("Far Plane", &cameraFar, 1.0f, 10.0f, 500.0f);
                ImGui::DragFloat("Movement Speed", &camera.MovementSpeed, 0.1f, 0.1f, 50.0f);
                ImGui::Checkbox("FPS Walking Mode", &camera.FPSMode);
            }
            
            if (ImGui::CollapsingHeader("Light Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::DragFloat3("Light Position", &lightPos.x, 0.1f);
                ImGui::ColorEdit3("Light Color", &lightColor.x);
                ImGui::Checkbox("Animate Light", &animateLight);
            }
            
            if (ImGui::CollapsingHeader("Cube 1 Settings")) {
                ImGui::DragFloat3("Position##cube1", &cubePosition.x, 0.1f);
                ImGui::DragFloat3("Rotation##cube1", &cubeRotation.x, 1.0f);
                ImGui::DragFloat3("Scale##cube1", &cubeScale.x, 0.1f, 0.1f, 10.0f);
                ImGui::ColorEdit3("Color##cube1", &cubeColor.x);
                ImGui::Checkbox("Animate##cube1", &animateCube);
            }
            
            if (ImGui::CollapsingHeader("Cube 2 Settings")) {
                ImGui::Checkbox("Show Second Cube", &showSecondCube);
                if (showSecondCube) {
                    ImGui::DragFloat3("Position##cube2", &cube2Position.x, 0.1f);
                    ImGui::DragFloat3("Rotation##cube2", &cube2Rotation.x, 1.0f);
                    ImGui::DragFloat3("Scale##cube2", &cube2Scale.x, 0.1f, 0.1f, 10.0f);
                    ImGui::ColorEdit3("Color##cube2", &cube2Color.x);
                }
            }
            
            if (ImGui::CollapsingHeader("Rendering Settings")) {
                ImGui::ColorEdit3("Floor Color", &floorColor.x);
                ImGui::ColorEdit3("Clear Color", &clearColor.x);
                ImGui::Checkbox("Use Clustered Rendering", &useClusteredRendering);
                ImGui::Checkbox("Enable Shadows", &enableShadows);
                ImGui::Checkbox("Wireframe Mode", &wireframeMode);
                ImGui::DragFloat("Ambient", &ambientStrength, 0.01f, 0.0f, 1.0f);
                ImGui::DragFloat("Specular", &specularStrength, 0.01f, 0.0f, 1.0f);
            }
            
            if (ImGui::CollapsingHeader("Light Editor", ImGuiTreeNodeFlags_DefaultOpen)) {
                // Toggle light editor mode
                if (lightEditorMode) {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.8f, 0.0f, 1.0f));
                    if (ImGui::Button("Light Editor: ON (Click to disable)")) {
                        lightEditorMode = false;
                        selectedLightIndex = -1;
                    }
                    ImGui::PopStyleColor();
                    
                    ImGui::Text("Controls:");
                    ImGui::BulletText("Left Click: Select light");
                    ImGui::BulletText("Right Click: Place new light");
                    ImGui::BulletText("Drag Gizmo: Move selected light");
                    ImGui::BulletText("DELETE: Remove selected light");
                } else {
                    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.5f, 0.5f, 0.5f, 1.0f));
                    if (ImGui::Button("Light Editor: OFF (Click to enable)")) {
                        lightEditorMode = true;
                        cameraLocked = true;
                        ReleaseCapture();
                        ClipCursor(nullptr);
                        ShowCursor(TRUE);
                    }
                    ImGui::PopStyleColor();
                }
                
                ImGui::Separator();
                ImGui::Text("New Light Settings:");
                ImGui::ColorEdit3("New Light Color", &newLightColor.x);
                ImGui::DragFloat("New Light Radius", &newLightRadius, 0.5f, 1.0f, 50.0f);
                ImGui::DragFloat("New Light Intensity", &newLightIntensity, 0.1f, 0.1f, 10.0f);
                ImGui::DragFloat("Gizmo Size", &gizmoSize, 0.1f, 0.5f, 5.0f);
                
                if (selectedLightIndex >= 0) {
                    ImGui::Separator();
                    ImGui::Text("Selected Light: %d", selectedLightIndex);
                    PointLightDX12* light = clusteredRenderer.getLight(selectedLightIndex);
                    if (light) {
                        ImGui::DragFloat3("Position##selected", &light->position.x, 0.1f);
                        ImGui::ColorEdit3("Color##selected", &light->color.x);
                        ImGui::DragFloat("Radius##selected", &light->radius, 0.5f, 1.0f, 50.0f);
                        ImGui::DragFloat("Intensity##selected", &light->intensity, 0.1f, 0.1f, 10.0f);
                        
                        if (ImGui::Button("Delete Selected Light")) {
                            clusteredRenderer.removeLight(selectedLightIndex);
                            selectedLightIndex = -1;
                        }
                    }
                } else {
                    ImGui::Text("No light selected");
                }
                
                ImGui::Separator();
                ImGui::Text("Total Lights: %d", clusteredRenderer.getLightCount());
                if (ImGui::Button("Clear All Lights")) {
                    clusteredRenderer.clearLights();
                    selectedLightIndex = -1;
                }
            }
            
            if (ImGui::CollapsingHeader("DDGI Settings")) {
                ImGui::Text("DDGI Global Illumination");
                ImGui::Checkbox("Enable DDGI", &useDDGI);
                if (useDDGI) {
                    ImGui::DragFloat("GI Intensity", &giIntensity, 0.1f, 0.0f, 5.0f);
                    ImGui::DragFloat("Normal Bias", &normalBias, 0.01f, 0.0f, 1.0f);
                    ImGui::DragFloat("Probe Spacing", &probeSpacing, 0.1f, 0.5f, 10.0f);
                    ImGui::Checkbox("Show Probes", &showProbes);
                    ImGui::Text("Probes: 256 (8x4x8)");
                }
                
                if (useClusteredRendering) {
                    ImGui::Separator();
                    ImGui::Text("Clustered Rendering");
                    ImGui::Text("Active Clusters: %d", clusteredRenderer.getActiveClusterCount());
                    ImGui::Text("Total Lights: %d", clusteredRenderer.getLightCount());
                    ImGui::SliderInt("Demo Lights", &numDemoLights, 0, 64);
                    ImGui::DragFloat("Light Radius", &demoLightRadius, 0.5f, 1.0f, 50.0f);
                    ImGui::Checkbox("Animate Lights", &animateDemoLights);
                    
                    static int prevNumLights = numDemoLights;
                    if (numDemoLights != prevNumLights) {
                        clusteredRenderer.clearLights();
                        for (int i = 0; i < numDemoLights; i++) {
                            float angle = (float)i / numDemoLights * XM_2PI;
                            XMFLOAT3 pos(cosf(angle) * 8.0f, 2.0f, sinf(angle) * 8.0f);
                            XMFLOAT3 color;
                            color.x = sinf(angle) * 0.5f + 0.5f;
                            color.y = sinf(angle + 2.094f) * 0.5f + 0.5f;
                            color.z = sinf(angle + 4.189f) * 0.5f + 0.5f;
                            clusteredRenderer.addLight(pos, color, demoLightRadius, demoLightIntensity);
                        }
                        prevNumLights = numDemoLights;
                    }
                }
            }
            
            if (ImGui::CollapsingHeader("Viewmodel (Gun)")) {
                ImGui::Checkbox("Show Gun", &gunModel.visible);
                ImGui::ColorEdit3("Gun Color", &gunModel.color.x);
                ImGui::DragFloat3("Gun Offset", &gunModel.offset.x, 0.01f);
                ImGui::DragFloat3("Gun Scale", &gunModel.scale.x, 0.01f);
                ImGui::DragFloat3("Gun Rotation", &gunModel.rotation.x, 1.0f);
            }
            
            if (ImGui::CollapsingHeader("Scene Graph & Models")) {
                static char modelPath[128] = "models/gun.glb";
                ImGui::InputText("Model Path", modelPath, IM_ARRAYSIZE(modelPath));
                if (ImGui::Button("Load Model")) {
                    std::cout << "Loading model: " << modelPath << std::endl;
                    // Reset model node
                    loadedModelNode = nullptr;
                    // Attempt load
                    loadedModelNode = GLBImporter::LoadGLB(modelPath, g_dx12.device, g_dx12.commandList);
                    if (loadedModelNode) {
                        std::cout << "Model loaded successfully!" << std::endl;
                        
                        // Set to center of scene
                        loadedModelNode->translation = XMFLOAT3(0.0f, 2.0f, 0.0f);
                        loadedModelNode->UpdateLocalTransform();
                        loadedModelNode->UpdateGlobalTransform(loadedModelNode->localTransform);
                        
                        // Detach from gun logic so it stays in scene
                        gunModel.loaded = false;
                    } else {
                        std::cout << "Failed to load model." << std::endl;
                    }
                }
                
                ImGui::Separator();
                ImGui::Text("Scene Graph:");
                
                if (loadedModelNode) {
                     ShowSceneGraphNode(loadedModelNode.get());
                } else {
                    ImGui::Text("No model loaded");
                }
            }
            
            ImGui::Separator();
            ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
            ImGui::Text("Renderer: DirectX 12");
            
            ImGui::End();
        }
        
        ImGui::Render();
        
        // Set ImGui descriptor heap and render
        ID3D12DescriptorHeap* heaps[] = { imguiSrvHeap.Get() };
        g_dx12.commandList->SetDescriptorHeaps(1, heaps);
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_dx12.commandList.Get());
        
        // End frame
        try {
            EndFrame();
        } catch (const std::exception& e) {
            std::cerr << "EndFrame error: " << e.what() << std::endl;
            break;
        }
    }
    
    // Cleanup
    WaitForGPU();
    
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    
    CleanupDX12();
    
    return (int)msg.wParam;
}

