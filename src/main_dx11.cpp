#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <iostream>
#include <algorithm>
#include <vector>
#include <chrono>
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>

#include "DX11Core.h"
#include "ClusteredRendererDX11.h"
#include "ShaderDX11.h"
#include "CameraDX12.h"
#include "ModelDX11.h"
#include "DDGI_DX11.h"

// Implementation of setPointLights (needs PointLightData from ClusteredRendererDX11.h)
inline void Shader::setPointLights(int numLights, const std::vector<PointLightData>& lights) {
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (SUCCEEDED(g_dx11.context->Map(pointLightsBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        PointLightsBuffer* data = (PointLightsBuffer*)mapped.pData;
        data->numPointLights = numLights;
        int count = numLights < 64 ? numLights : 64;
        for (int i = 0; i < count; i++) {
            data->lights[i].position = lights[i].position;
            data->lights[i].radius = lights[i].radius;
            data->lights[i].color = lights[i].color;
            data->lights[i].intensity = lights[i].intensity;
        }
        g_dx11.context->Unmap(pointLightsBuffer.Get(), 0);
    }
    g_dx11.context->PSSetConstantBuffers(4, 1, pointLightsBuffer.GetAddressOf());
}

using namespace DirectX;
using Microsoft::WRL::ComPtr;

// Global DX11 Context
DX11Context g_dx11;

// Settings
unsigned int SCR_WIDTH = 1280;
unsigned int SCR_HEIGHT = 720;
unsigned int SHADOW_WIDTH = 2048;
unsigned int SHADOW_HEIGHT = 2048;

Camera camera(XMFLOAT3(0.0f, 5.0f, 10.0f));
float lastX = SCR_WIDTH / 2.0f;
float lastY = SCR_HEIGHT / 2.0f;
bool firstMouse = true;
float deltaTime = 0.0f;
float lastFrame = 0.0f;
bool showUI = true;
bool cameraLocked = true;
bool isFullscreen = false;
RECT windowedRect = {};
DWORD windowedStyle = 0;

// Editable scene parameters
XMFLOAT3 lightPos(-5.0f, 10.0f, -5.0f);
XMFLOAT3 lightTarget(0.0f, 0.0f, 0.0f);
XMFLOAT3 lightUp(0.0f, 1.0f, 0.0f);
XMFLOAT3 cubePosition(0.0f, 1.5f, 0.0f);
XMFLOAT3 cubeScale(1.0f, 1.0f, 1.0f);
XMFLOAT3 cubeRotation(0.0f, 0.0f, 0.0f);
XMFLOAT3 cubeColor(0.7f, 0.7f, 0.7f);
XMFLOAT3 floorColor(0.5f, 0.5f, 0.5f);
XMFLOAT3 clearColor(0.1f, 0.1f, 0.1f);
float cameraFOV = 45.0f;
float cameraNear = 0.1f;
float cameraFar = 100.0f;
float lightOrthoSize = 15.0f;
float lightNear = 1.0f;
float lightFar = 25.0f;

int projectionType = 0;
float orthoSize = 10.0f;

float shadowBias = 0.005f;
bool enableShadows = true;
bool wireframeMode = false;
float ambientStrength = 0.0f;
float specularStrength = 0.5f;
int specularShininess = 32;

int renderMode = 0;
bool showShadowMapOverlay = false;
float overlaySize = 0.25f;

bool showSecondCube = false;
XMFLOAT3 cube2Position(-3.0f, 0.5f, 2.0f);
XMFLOAT3 cube2Scale(0.5f, 0.5f, 0.5f);
XMFLOAT3 cube2Rotation(0.0f, 45.0f, 0.0f);
XMFLOAT3 cube2Color(0.8f, 0.8f, 0.8f);

bool animateLight = false;
bool animateCube = false;
float animationSpeed = 1.0f;

int lightType = 0;
float lightConstant = 1.0f;
float lightLinear = 0.09f;
float lightQuadratic = 0.032f;
XMFLOAT3 lightColor(1.0f, 1.0f, 1.0f);

Model gunModel;

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

ClusteredRenderer clusteredRenderer;
bool useClusteredRendering = true;
bool showClusterDebug = false;
int numDemoLights = 64;
float demoLightRadius = 8.0f;
float demoLightIntensity = 1.5f;
bool animateDemoLights = true;

// DDGI Global Illumination
DDGIRenderer ddgiRenderer;
bool useDDGI = true;

// DX11 Resources
ComPtr<ID3D11Buffer> cubeVB;
ComPtr<ID3D11Buffer> planeVB;
ComPtr<ID3D11Buffer> quadVB;
ComPtr<ID3D11Texture2D> shadowMapTexture;
ComPtr<ID3D11DepthStencilView> shadowMapDSV;
ComPtr<ID3D11ShaderResourceView> shadowMapSRV;

Shader clusteredShader;
Shader depthShader;

// Forward declare message handler from imgui_impl_win32.cpp
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// High resolution timer
class Timer {
public:
    void Start() {
        start = std::chrono::high_resolution_clock::now();
    }
    float GetElapsed() {
        auto now = std::chrono::high_resolution_clock::now();
        return std::chrono::duration<float>(now - start).count();
    }
private:
    std::chrono::high_resolution_clock::time_point start;
};
Timer gameTimer;

bool CreateCubeVB() {
    Vertex vertices[] = {
        // Front face
        { XMFLOAT3(-0.5f, -0.5f, -0.5f), XMFLOAT3(0.0f, 0.0f, -1.0f) },
        { XMFLOAT3(0.5f, 0.5f, -0.5f), XMFLOAT3(0.0f, 0.0f, -1.0f) },
        { XMFLOAT3(0.5f, -0.5f, -0.5f), XMFLOAT3(0.0f, 0.0f, -1.0f) },
        { XMFLOAT3(0.5f, 0.5f, -0.5f), XMFLOAT3(0.0f, 0.0f, -1.0f) },
        { XMFLOAT3(-0.5f, -0.5f, -0.5f), XMFLOAT3(0.0f, 0.0f, -1.0f) },
        { XMFLOAT3(-0.5f, 0.5f, -0.5f), XMFLOAT3(0.0f, 0.0f, -1.0f) },
        // Back face
        { XMFLOAT3(-0.5f, -0.5f, 0.5f), XMFLOAT3(0.0f, 0.0f, 1.0f) },
        { XMFLOAT3(0.5f, -0.5f, 0.5f), XMFLOAT3(0.0f, 0.0f, 1.0f) },
        { XMFLOAT3(0.5f, 0.5f, 0.5f), XMFLOAT3(0.0f, 0.0f, 1.0f) },
        { XMFLOAT3(0.5f, 0.5f, 0.5f), XMFLOAT3(0.0f, 0.0f, 1.0f) },
        { XMFLOAT3(-0.5f, 0.5f, 0.5f), XMFLOAT3(0.0f, 0.0f, 1.0f) },
        { XMFLOAT3(-0.5f, -0.5f, 0.5f), XMFLOAT3(0.0f, 0.0f, 1.0f) },
        // Left face
        { XMFLOAT3(-0.5f, 0.5f, 0.5f), XMFLOAT3(-1.0f, 0.0f, 0.0f) },
        { XMFLOAT3(-0.5f, 0.5f, -0.5f), XMFLOAT3(-1.0f, 0.0f, 0.0f) },
        { XMFLOAT3(-0.5f, -0.5f, -0.5f), XMFLOAT3(-1.0f, 0.0f, 0.0f) },
        { XMFLOAT3(-0.5f, -0.5f, -0.5f), XMFLOAT3(-1.0f, 0.0f, 0.0f) },
        { XMFLOAT3(-0.5f, -0.5f, 0.5f), XMFLOAT3(-1.0f, 0.0f, 0.0f) },
        { XMFLOAT3(-0.5f, 0.5f, 0.5f), XMFLOAT3(-1.0f, 0.0f, 0.0f) },
        // Right face
        { XMFLOAT3(0.5f, 0.5f, 0.5f), XMFLOAT3(1.0f, 0.0f, 0.0f) },
        { XMFLOAT3(0.5f, -0.5f, -0.5f), XMFLOAT3(1.0f, 0.0f, 0.0f) },
        { XMFLOAT3(0.5f, 0.5f, -0.5f), XMFLOAT3(1.0f, 0.0f, 0.0f) },
        { XMFLOAT3(0.5f, -0.5f, -0.5f), XMFLOAT3(1.0f, 0.0f, 0.0f) },
        { XMFLOAT3(0.5f, 0.5f, 0.5f), XMFLOAT3(1.0f, 0.0f, 0.0f) },
        { XMFLOAT3(0.5f, -0.5f, 0.5f), XMFLOAT3(1.0f, 0.0f, 0.0f) },
        // Bottom face
        { XMFLOAT3(-0.5f, -0.5f, -0.5f), XMFLOAT3(0.0f, -1.0f, 0.0f) },
        { XMFLOAT3(0.5f, -0.5f, -0.5f), XMFLOAT3(0.0f, -1.0f, 0.0f) },
        { XMFLOAT3(0.5f, -0.5f, 0.5f), XMFLOAT3(0.0f, -1.0f, 0.0f) },
        { XMFLOAT3(0.5f, -0.5f, 0.5f), XMFLOAT3(0.0f, -1.0f, 0.0f) },
        { XMFLOAT3(-0.5f, -0.5f, 0.5f), XMFLOAT3(0.0f, -1.0f, 0.0f) },
        { XMFLOAT3(-0.5f, -0.5f, -0.5f), XMFLOAT3(0.0f, -1.0f, 0.0f) },
        // Top face
        { XMFLOAT3(-0.5f, 0.5f, -0.5f), XMFLOAT3(0.0f, 1.0f, 0.0f) },
        { XMFLOAT3(0.5f, 0.5f, 0.5f), XMFLOAT3(0.0f, 1.0f, 0.0f) },
        { XMFLOAT3(0.5f, 0.5f, -0.5f), XMFLOAT3(0.0f, 1.0f, 0.0f) },
        { XMFLOAT3(0.5f, 0.5f, 0.5f), XMFLOAT3(0.0f, 1.0f, 0.0f) },
        { XMFLOAT3(-0.5f, 0.5f, -0.5f), XMFLOAT3(0.0f, 1.0f, 0.0f) },
        { XMFLOAT3(-0.5f, 0.5f, 0.5f), XMFLOAT3(0.0f, 1.0f, 0.0f) },
    };
    
    D3D11_BUFFER_DESC bd = {};
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.ByteWidth = sizeof(vertices);
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    
    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = vertices;
    
    return SUCCEEDED(g_dx11.device->CreateBuffer(&bd, &initData, &cubeVB));
}

bool CreatePlaneVB() {
    Vertex vertices[] = {
        { XMFLOAT3(25.0f, -0.5f, 25.0f), XMFLOAT3(0.0f, 1.0f, 0.0f) },
        { XMFLOAT3(-25.0f, -0.5f, -25.0f), XMFLOAT3(0.0f, 1.0f, 0.0f) },
        { XMFLOAT3(-25.0f, -0.5f, 25.0f), XMFLOAT3(0.0f, 1.0f, 0.0f) },
        { XMFLOAT3(25.0f, -0.5f, 25.0f), XMFLOAT3(0.0f, 1.0f, 0.0f) },
        { XMFLOAT3(25.0f, -0.5f, -25.0f), XMFLOAT3(0.0f, 1.0f, 0.0f) },
        { XMFLOAT3(-25.0f, -0.5f, -25.0f), XMFLOAT3(0.0f, 1.0f, 0.0f) },
    };
    
    D3D11_BUFFER_DESC bd = {};
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.ByteWidth = sizeof(vertices);
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    
    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = vertices;
    
    return SUCCEEDED(g_dx11.device->CreateBuffer(&bd, &initData, &planeVB));
}

bool CreateShadowMap() {
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = SHADOW_WIDTH;
    texDesc.Height = SHADOW_HEIGHT;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R32_TYPELESS;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
    
    HRESULT hr = g_dx11.device->CreateTexture2D(&texDesc, nullptr, &shadowMapTexture);
    if (FAILED(hr)) return false;
    
    D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
    dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    
    hr = g_dx11.device->CreateDepthStencilView(shadowMapTexture.Get(), &dsvDesc, &shadowMapDSV);
    if (FAILED(hr)) return false;
    
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    
    hr = g_dx11.device->CreateShaderResourceView(shadowMapTexture.Get(), &srvDesc, &shadowMapSRV);
    return SUCCEEDED(hr);
}

void DrawCube() {
    UINT stride = sizeof(Vertex);
    UINT offset = 0;
    g_dx11.context->IASetVertexBuffers(0, 1, cubeVB.GetAddressOf(), &stride, &offset);
    g_dx11.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_dx11.context->Draw(36, 0);
}

void DrawPlane() {
    UINT stride = sizeof(Vertex);
    UINT offset = 0;
    g_dx11.context->IASetVertexBuffers(0, 1, planeVB.GetAddressOf(), &stride, &offset);
    g_dx11.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_dx11.context->Draw(6, 0);
}

void ToggleFullscreen(HWND hwnd) {
    if (!isFullscreen) {
        // Save current window position and size
        GetWindowRect(hwnd, &windowedRect);
        windowedStyle = GetWindowLong(hwnd, GWL_STYLE);
        
        // Get monitor info
        HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfo(hMon, &mi);
        
        // Set borderless fullscreen
        SetWindowLong(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(hwnd, HWND_TOP,
            mi.rcMonitor.left, mi.rcMonitor.top,
            mi.rcMonitor.right - mi.rcMonitor.left,
            mi.rcMonitor.bottom - mi.rcMonitor.top,
            SWP_FRAMECHANGED | SWP_NOCOPYBITS);
        
        isFullscreen = true;
    } else {
        // Restore windowed mode
        SetWindowLong(hwnd, GWL_STYLE, windowedStyle);
        SetWindowPos(hwnd, HWND_NOTOPMOST,
            windowedRect.left, windowedRect.top,
            windowedRect.right - windowedRect.left,
            windowedRect.bottom - windowedRect.top,
            SWP_FRAMECHANGED | SWP_NOCOPYBITS);
        
        isFullscreen = false;
    }
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam))
        return true;
    
    switch (msg) {
    case WM_SIZE:
        if (g_dx11.device && g_dx11.initialized && wParam != SIZE_MINIMIZED) {
            unsigned int newWidth = LOWORD(lParam);
            unsigned int newHeight = HIWORD(lParam);
            if (newWidth > 0 && newHeight > 0 && (newWidth != SCR_WIDTH || newHeight != SCR_HEIGHT)) {
                SCR_WIDTH = newWidth;
                SCR_HEIGHT = newHeight;
                ResizeDX11(SCR_WIDTH, SCR_HEIGHT);
            }
        }
        return 0;
        
    case WM_MOUSEMOVE:
        if (!cameraLocked && !(showUI && ImGui::GetIO().WantCaptureMouse)) {
            float xpos = (float)GET_X_LPARAM(lParam);
            float ypos = (float)GET_Y_LPARAM(lParam);
            
            if (firstMouse) {
                lastX = xpos;
                lastY = ypos;
                firstMouse = false;
            }
            
            float xoffset = xpos - lastX;
            float yoffset = lastY - ypos;
            
            lastX = xpos;
            lastY = ypos;
            
            camera.ProcessMouseMovement(xoffset, yoffset);
        }
        return 0;
        
    case WM_LBUTTONDOWN:
        if (!ImGui::GetIO().WantCaptureMouse) {
            if (cameraLocked) {
                cameraLocked = false;
                SetCapture(hwnd);
                ShowCursor(FALSE);
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
                proj.position.x += camera.Front.x * 0.5f;
                proj.position.y += camera.Front.y * 0.5f;
                proj.position.z += camera.Front.z * 0.5f;
                XMVECTOR frontVec = XMVector3Normalize(XMLoadFloat3(&camera.Front));
                XMStoreFloat3(&proj.direction, frontVec);
                proj.speed = projectileSpeed;
                proj.lifetime = projectileLifetime;
                proj.active = true;
                projectiles.push_back(proj);
            }
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
                ShowCursor(TRUE);
            }
        } else if (wParam == 'C') {
            cameraLocked = true;
            ReleaseCapture();
            ShowCursor(TRUE);
        } else if (wParam == 'F') {
            camera.FPSMode = !camera.FPSMode;
            if (camera.FPSMode) {
                camera.Position.y = camera.FloorY + camera.PlayerHeight;
            }
        } else if (wParam == VK_F11) {
            ToggleFullscreen(hwnd);
        }
        return 0;
        
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    
    return DefWindowProc(hwnd, msg, wParam, lParam);
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
    }
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    // Create console for debug output
    AllocConsole();
    FILE* pCout;
    freopen_s(&pCout, "CONOUT$", "w", stdout);
    freopen_s(&pCout, "CONOUT$", "w", stderr);
    
    std::cout << "GraphicEngine DX11 Starting..." << std::endl;
    std::cout << "Working directory should contain 'shaders/' folder" << std::endl;
    
    // Register window class
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"GraphicEngineDX11";
    RegisterClassExW(&wc);
    
    // Create window
    RECT rc = { 0, 0, (LONG)SCR_WIDTH, (LONG)SCR_HEIGHT };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    
    HWND hwnd = CreateWindowW(L"GraphicEngineDX11", L"Shadow Mapping Engine - DirectX 11",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, hInstance, nullptr);
    
    if (!hwnd) {
        std::cerr << "Failed to create window" << std::endl;
        MessageBoxA(nullptr, "Failed to create window.", "Window Error", MB_OK | MB_ICONERROR);
        system("pause");
        return -1;
    }
    
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);
    
    // Initialize DirectX 11
    if (!InitDX11(hwnd, SCR_WIDTH, SCR_HEIGHT)) {
        std::cerr << "Failed to initialize DirectX 11" << std::endl;
        MessageBoxA(hwnd, "Failed to initialize DirectX 11.", "DX11 Error", MB_OK | MB_ICONERROR);
        system("pause");
        return -1;
    }
    
    // Setup Dear ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_dx11.device.Get(), g_dx11.context.Get());
    
    // Load shaders
    if (!clusteredShader.load("shaders/clustered_vs.hlsl", "shaders/clustered_ps.hlsl")) {
        std::cerr << "Failed to load clustered shader" << std::endl;
        MessageBoxA(hwnd, "Failed to load clustered shader.\nCheck console for details.", "Shader Error", MB_OK | MB_ICONERROR);
        system("pause");
        return -1;
    }
    
    if (!depthShader.load("shaders/depth_vs.hlsl", "shaders/depth_ps.hlsl")) {
        std::cerr << "Failed to load depth shader" << std::endl;
        MessageBoxA(hwnd, "Failed to load depth shader.\nCheck console for details.", "Shader Error", MB_OK | MB_ICONERROR);
        system("pause");
        return -1;
    }
    
    // Create geometry
    if (!CreateCubeVB() || !CreatePlaneVB()) {
        std::cerr << "Failed to create geometry" << std::endl;
        return -1;
    }
    
    // Create shadow map
    if (!CreateShadowMap()) {
        std::cerr << "Failed to create shadow map" << std::endl;
        return -1;
    }
    
    // Initialize DDGI
    if (!ddgiRenderer.init()) {
        std::cerr << "Failed to initialize DDGI (non-fatal)" << std::endl;
        useDDGI = false;
    } else {
        std::cout << "DDGI initialized with " << ddgiRenderer.getTotalProbeCount() << " probes" << std::endl;
    }
    
    // Load gun model
    gunModel.loadOBJ("models/gun.obj");
    
    // Initialize demo lights
    for (int i = 0; i < numDemoLights; i++) {
        float angle = (float)i / numDemoLights * XM_2PI;
        float radius = 8.0f;
        XMFLOAT3 pos(cosf(angle) * radius, 4.0f, sinf(angle) * radius);
        XMFLOAT3 color;
        color.x = sinf(angle) * 0.5f + 0.5f;
        color.y = sinf(angle + 2.094f) * 0.5f + 0.5f;
        color.z = sinf(angle + 4.189f) * 0.5f + 0.5f;
        clusteredRenderer.addLight(pos, color, demoLightRadius, demoLightIntensity);
    }
    
    gameTimer.Start();
    float lastTime = 0.0f;
    
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
        
        ProcessInput(hwnd);
        camera.Update(deltaTime);
        
        // Update projectiles
        for (auto& proj : projectiles) {
            if (proj.active) {
                proj.position.x += proj.direction.x * proj.speed * deltaTime;
                proj.position.y += proj.direction.y * proj.speed * deltaTime;
                proj.position.z += proj.direction.z * proj.speed * deltaTime;
                proj.lifetime -= deltaTime;
                if (proj.lifetime <= 0.0f) proj.active = false;
            }
        }
        projectiles.erase(std::remove_if(projectiles.begin(), projectiles.end(),
            [](const Projectile& p) { return !p.active; }), projectiles.end());
        
        // Update demo lights animation
        if (animateDemoLights && useClusteredRendering) {
            float time = currentTime * animationSpeed;
            for (int i = 0; i < clusteredRenderer.getLightCount(); i++) {
                float angle = (float)i / clusteredRenderer.getLightCount() * XM_2PI + time;
                float radius = 8.0f;
                clusteredRenderer.lights[i].position = XMFLOAT3(
                    cosf(angle) * radius,
                    4.0f + sinf(time * 2.0f + (float)i) * 1.5f,
                    sinf(angle) * radius
                );
            }
        }
        
        // Animation
        if (animateLight) {
            float time = currentTime * animationSpeed;
            lightPos.x = cosf(time) * 10.0f;
            lightPos.z = sinf(time) * 10.0f;
        }
        
        if (animateCube) {
            cubeRotation.y = fmodf(currentTime * 30.0f * animationSpeed, 360.0f);
        }
        
        // Start ImGui frame
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        
        // ImGui UI
        if (showUI) {
            ImGui::Begin("Scene Controls", &showUI);
            
            ImGui::Text("Controls:");
            ImGui::BulletText("TAB: Toggle UI");
            ImGui::BulletText("C: Lock/Unlock Camera");
            ImGui::BulletText("F: Toggle FPS Walking Mode");
            ImGui::BulletText("Left Click: Lock camera / Shoot");
            
            if (cameraLocked) {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
                if (ImGui::Button("Camera LOCKED (Press C to unlock)")) cameraLocked = false;
                ImGui::PopStyleColor();
            } else {
                ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.8f, 0.2f, 1.0f));
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
            
            if (ImGui::CollapsingHeader("Rendering Settings")) {
                ImGui::ColorEdit3("Floor Color", &floorColor.x);
                ImGui::ColorEdit3("Clear Color", &clearColor.x);
                ImGui::Checkbox("Use Clustered Rendering", &useClusteredRendering);
                ImGui::Checkbox("Enable Shadows", &enableShadows);
                ImGui::Checkbox("Wireframe Mode", &wireframeMode);
                ImGui::DragFloat("Ambient", &ambientStrength, 0.01f, 0.0f, 1.0f);
                ImGui::DragFloat("Specular", &specularStrength, 0.01f, 0.0f, 1.0f);
                
                // DDGI Settings
                ImGui::Separator();
                ImGui::Text("DDGI Global Illumination");
                ImGui::Checkbox("Enable DDGI", &ddgiRenderer.config.enabled);
                if (ddgiRenderer.config.enabled) {
                    ImGui::DragFloat("GI Intensity", &ddgiRenderer.config.giIntensity, 0.1f, 0.0f, 5.0f);
                    ImGui::DragFloat("Normal Bias", &ddgiRenderer.config.normalBias, 0.01f, 0.0f, 1.0f);
                    ImGui::DragFloat("Probe Spacing", &ddgiRenderer.config.probeSpacing, 0.1f, 0.5f, 10.0f);
                    ImGui::Checkbox("Show Probes", &ddgiRenderer.config.showProbes);
                    ImGui::Text("Probes: %d (%dx%dx%d)", 
                        ddgiRenderer.getTotalProbeCount(),
                        ddgiRenderer.config.probeCountX,
                        ddgiRenderer.config.probeCountY,
                        ddgiRenderer.config.probeCountZ);
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
                            XMFLOAT3 pos(cosf(angle) * 8.0f, 4.0f, sinf(angle) * 8.0f);
                            XMFLOAT3 color(sinf(angle)*0.5f+0.5f, sinf(angle+2.094f)*0.5f+0.5f, sinf(angle+4.189f)*0.5f+0.5f);
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
            
            ImGui::Separator();
            ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
            ImGui::Text("Renderer: DirectX 11");
            
            ImGui::End();
        }
        
        // Set wireframe mode
        if (wireframeMode) {
            g_dx11.context->RSSetState(g_dx11.wireframeState.Get());
        } else {
            g_dx11.context->RSSetState(g_dx11.rasterizerState.Get());
        }
        
        // Calculate matrices
        XMMATRIX view = camera.GetViewMatrix();
        XMMATRIX projection;
        if (projectionType == 0) {
            projection = XMMatrixPerspectiveFovLH(XMConvertToRadians(cameraFOV),
                (float)SCR_WIDTH / (float)SCR_HEIGHT, cameraNear, cameraFar);
        } else {
            float aspect = (float)SCR_WIDTH / (float)SCR_HEIGHT;
            projection = XMMatrixOrthographicLH(orthoSize * 2 * aspect, orthoSize * 2, cameraNear, cameraFar);
        }
        
        // Light space matrix
        XMVECTOR lightPosVec = XMLoadFloat3(&lightPos);
        XMVECTOR lightTargetVec = XMLoadFloat3(&lightTarget);
        XMVECTOR lightUpVec = XMLoadFloat3(&lightUp);
        XMMATRIX lightView = XMMatrixLookAtLH(lightPosVec, lightTargetVec, lightUpVec);
        XMMATRIX lightProj = XMMatrixOrthographicLH(lightOrthoSize * 2, lightOrthoSize * 2, lightNear, lightFar);
        XMMATRIX lightSpaceMatrix = lightView * lightProj;
        
        // Update clustered renderer
        clusteredRenderer.setScreenSize((float)SCR_WIDTH, (float)SCR_HEIGHT);
        clusteredRenderer.setCamera(cameraFOV, cameraNear, cameraFar, view, projection);
        if (useClusteredRendering) {
            clusteredRenderer.cullLights();
        }
        
        // 1. Render shadow map
        D3D11_VIEWPORT shadowViewport = {};
        shadowViewport.Width = (float)SHADOW_WIDTH;
        shadowViewport.Height = (float)SHADOW_HEIGHT;
        shadowViewport.MinDepth = 0.0f;
        shadowViewport.MaxDepth = 1.0f;
        g_dx11.context->RSSetViewports(1, &shadowViewport);
        
        g_dx11.context->OMSetRenderTargets(0, nullptr, shadowMapDSV.Get());
        g_dx11.context->ClearDepthStencilView(shadowMapDSV.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
        
        depthShader.use();
        
        // Render floor for shadow
        XMMATRIX model = XMMatrixIdentity();
        depthShader.setMatrices(model, XMMatrixIdentity(), XMMatrixIdentity(), lightSpaceMatrix);
        DrawPlane();
        
        // Render cube for shadow
        model = XMMatrixScaling(cubeScale.x, cubeScale.y, cubeScale.z);
        model = model * XMMatrixRotationX(XMConvertToRadians(cubeRotation.x));
        model = model * XMMatrixRotationY(XMConvertToRadians(cubeRotation.y));
        model = model * XMMatrixRotationZ(XMConvertToRadians(cubeRotation.z));
        model = model * XMMatrixTranslation(cubePosition.x, cubePosition.y, cubePosition.z);
        depthShader.setMatrices(model, XMMatrixIdentity(), XMMatrixIdentity(), lightSpaceMatrix);
        DrawCube();
        
        // Render second cube for shadow
        if (showSecondCube) {
            model = XMMatrixScaling(cube2Scale.x, cube2Scale.y, cube2Scale.z);
            model = model * XMMatrixRotationX(XMConvertToRadians(cube2Rotation.x));
            model = model * XMMatrixRotationY(XMConvertToRadians(cube2Rotation.y));
            model = model * XMMatrixRotationZ(XMConvertToRadians(cube2Rotation.z));
            model = model * XMMatrixTranslation(cube2Position.x, cube2Position.y, cube2Position.z);
            depthShader.setMatrices(model, XMMatrixIdentity(), XMMatrixIdentity(), lightSpaceMatrix);
            DrawCube();
        }
        
        // 2. Render scene
        D3D11_VIEWPORT mainViewport = {};
        mainViewport.Width = (float)SCR_WIDTH;
        mainViewport.Height = (float)SCR_HEIGHT;
        mainViewport.MinDepth = 0.0f;
        mainViewport.MaxDepth = 1.0f;
        g_dx11.context->RSSetViewports(1, &mainViewport);
        
        g_dx11.context->OMSetRenderTargets(1, g_dx11.renderTargetView.GetAddressOf(), g_dx11.depthStencilView.Get());
        
        float clearColorArr[4] = { clearColor.x, clearColor.y, clearColor.z, 1.0f };
        g_dx11.context->ClearRenderTargetView(g_dx11.renderTargetView.Get(), clearColorArr);
        g_dx11.context->ClearDepthStencilView(g_dx11.depthStencilView.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
        
        clusteredShader.use();
        
        // Bind shadow map
        g_dx11.context->PSSetShaderResources(0, 1, shadowMapSRV.GetAddressOf());
        g_dx11.context->PSSetSamplers(0, 1, g_dx11.shadowSamplerState.GetAddressOf());
        g_dx11.context->PSSetSamplers(1, 1, g_dx11.samplerState.GetAddressOf());
        
        // Update and bind DDGI resources with all scene lights
        if (useDDGI && ddgiRenderer.config.enabled) {
            ddgiRenderer.clearGILights();
            
            // Add main light as directional
            XMFLOAT3 lightDir;
            XMStoreFloat3(&lightDir, XMVector3Normalize(XMLoadFloat3(&lightPos)));
            ddgiRenderer.addGILight(lightDir, lightColor, 100.0f, 1.0f, true);
            
            // Add all point lights from clustered renderer
            if (useClusteredRendering) {
                for (const auto& light : clusteredRenderer.lights) {
                    if (light.active) {
                        ddgiRenderer.addGILight(light.position, light.color, light.radius, light.intensity, false);
                    }
                }
            }
            
            ddgiRenderer.updateProbesFromLights();
            ddgiRenderer.bind(2, 3, 5);
        }
        
        // Set common uniforms
        clusteredShader.setLight(lightPos, lightType, lightColor, lightConstant, lightLinear, lightQuadratic,
            ambientStrength, specularStrength, specularShininess, shadowBias, enableShadows);
        clusteredShader.setCamera(camera.Position);
        
        // Set point lights
        if (useClusteredRendering) {
            auto lightData = clusteredRenderer.getPointLightData();
            clusteredShader.setPointLights((int)lightData.size(), lightData);
        } else {
            std::vector<PointLightData> emptyLights;
            clusteredShader.setPointLights(0, emptyLights);
        }
        
        // Render floor
        model = XMMatrixIdentity();
        clusteredShader.setMatrices(model, view, projection, lightSpaceMatrix);
        clusteredShader.setObjectColor(floorColor);
        DrawPlane();
        
        // Render cube
        model = XMMatrixScaling(cubeScale.x, cubeScale.y, cubeScale.z);
        model = model * XMMatrixRotationX(XMConvertToRadians(cubeRotation.x));
        model = model * XMMatrixRotationY(XMConvertToRadians(cubeRotation.y));
        model = model * XMMatrixRotationZ(XMConvertToRadians(cubeRotation.z));
        model = model * XMMatrixTranslation(cubePosition.x, cubePosition.y, cubePosition.z);
        clusteredShader.setMatrices(model, view, projection, lightSpaceMatrix);
        clusteredShader.setObjectColor(cubeColor);
        DrawCube();
        
        // Render second cube
        if (showSecondCube) {
            model = XMMatrixScaling(cube2Scale.x, cube2Scale.y, cube2Scale.z);
            model = model * XMMatrixRotationX(XMConvertToRadians(cube2Rotation.x));
            model = model * XMMatrixRotationY(XMConvertToRadians(cube2Rotation.y));
            model = model * XMMatrixRotationZ(XMConvertToRadians(cube2Rotation.z));
            model = model * XMMatrixTranslation(cube2Position.x, cube2Position.y, cube2Position.z);
            clusteredShader.setMatrices(model, view, projection, lightSpaceMatrix);
            clusteredShader.setObjectColor(cube2Color);
            DrawCube();
        }
        
        // Render point light spheres
        if (useClusteredRendering) {
            for (int i = 0; i < clusteredRenderer.getLightCount(); i++) {
                const auto& light = clusteredRenderer.lights[i];
                if (!light.active) continue;
                model = XMMatrixScaling(0.2f, 0.2f, 0.2f);
                model = model * XMMatrixTranslation(light.position.x, light.position.y, light.position.z);
                clusteredShader.setMatrices(model, view, projection, lightSpaceMatrix);
                XMFLOAT3 lightCol(light.color.x * light.intensity, light.color.y * light.intensity, light.color.z * light.intensity);
                clusteredShader.setObjectColor(lightCol);
                DrawCube();
            }
        }
        
        // Render projectiles
        for (const auto& proj : projectiles) {
            if (proj.active) {
                model = XMMatrixScaling(projectileScale, projectileScale, projectileScale);
                model = model * XMMatrixTranslation(proj.position.x, proj.position.y, proj.position.z);
                clusteredShader.setMatrices(model, view, projection, lightSpaceMatrix);
                clusteredShader.setObjectColor(projectileColor);
                DrawCube();
            }
        }
        
        // Render gun viewmodel
        if (gunModel.loaded && gunModel.visible) {
            g_dx11.context->ClearDepthStencilView(g_dx11.depthStencilView.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
            g_dx11.context->RSSetState(g_dx11.noCullState.Get());  // Disable culling for gun model
            model = gunModel.getModelMatrix(camera.Position, camera.Front, camera.Up);
            clusteredShader.setMatrices(model, view, projection, lightSpaceMatrix);
            clusteredShader.setObjectColor(gunModel.color);
            gunModel.draw();
            g_dx11.context->RSSetState(g_dx11.rasterizerState.Get());  // Restore normal culling
        }
        
        // Unbind shadow map SRV and DDGI before next frame's render to depth
        ID3D11ShaderResourceView* nullSRV = nullptr;
        g_dx11.context->PSSetShaderResources(0, 1, &nullSRV);
        if (useDDGI) {
            ddgiRenderer.unbind(2, 3);
        }
        
        // Render ImGui
        ImGui::Render();
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        
        // Present (uncapped frame rate with tearing allowed)
        g_dx11.swapChain->Present(0, DXGI_PRESENT_ALLOW_TEARING);
    }
    
    // Cleanup
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    
    CleanupDX11();
    
    return (int)msg.wParam;
}

