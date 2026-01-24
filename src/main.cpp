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
XMFLOAT3 cubeColor(1.0f, 0.0f, 1.0f);  // Pink/magenta like DX11
XMFLOAT3 floorColor(0.4f, 0.4f, 0.4f);
XMFLOAT3 clearColor(0.1f, 0.1f, 0.15f);
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
    XMFLOAT3 color = XMFLOAT3(0.3f, 0.3f, 0.35f);
    XMFLOAT3 offset = XMFLOAT3(0.3f, -0.25f, 0.5f);
    XMFLOAT3 scale = XMFLOAT3(0.15f, 0.15f, 0.15f);
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
bool animateDemoLights = true;

// DX12 resources
ShaderDX12 mainShader;
ComPtr<ID3D12Resource> cubeVertexBuffer;
ComPtr<ID3D12Resource> planeVertexBuffer;
D3D12_VERTEX_BUFFER_VIEW cubeVBV = {};
D3D12_VERTEX_BUFFER_VIEW planeVBV = {};

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
    float planeSize = 20.0f;
    std::vector<Vertex> planeVertices = {
        {{-planeSize, 0.0f,  planeSize}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f}},
        {{ planeSize, 0.0f,  planeSize}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f}},
        {{ planeSize, 0.0f, -planeSize}, {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f}},
        {{-planeSize, 0.0f,  planeSize}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f}},
        {{ planeSize, 0.0f, -planeSize}, {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f}},
        {{-planeSize, 0.0f, -planeSize}, {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f}},
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

void ProcessInput(HWND hwnd) {
    if (!cameraLocked && !(showUI && ImGui::GetIO().WantCaptureKeyboard)) {
        if (GetAsyncKeyState('W') & 0x8000) camera.ProcessKeyboard('W', deltaTime);
        if (GetAsyncKeyState('S') & 0x8000) camera.ProcessKeyboard('S', deltaTime);
        if (GetAsyncKeyState('A') & 0x8000) camera.ProcessKeyboard('A', deltaTime);
        if (GetAsyncKeyState('D') & 0x8000) camera.ProcessKeyboard('D', deltaTime);
        if (GetAsyncKeyState(VK_SPACE) & 0x8000) camera.ProcessKeyboard(' ', deltaTime);
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
                proj.direction = camera.Front;
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
    
    // Load shaders - using the real clustered shaders
    if (!mainShader.Load("shaders/clustered_vs.hlsl", "shaders/clustered_ps.hlsl")) {
        std::cerr << "Failed to load clustered shaders, trying DX12 specific shaders..." << std::endl;
        if (!mainShader.Load("shaders/clustered_dx12_vs.hlsl", "shaders/clustered_dx12_ps.hlsl")) {
            std::cerr << "Failed to load any shaders" << std::endl;
            MessageBoxA(hwnd, "Failed to load shaders.\nCheck console for details.", "Shader Error", MB_OK | MB_ICONERROR);
            return -1;
        }
    }
    std::cout << "Clustered Forward shaders loaded successfully" << std::endl;
    
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
        
        // Setup matrices
        XMMATRIX view = camera.GetViewMatrix();
        XMMATRIX projection = XMMatrixPerspectiveFovLH(
            XMConvertToRadians(cameraFOV),
            (float)SCR_WIDTH / (float)SCR_HEIGHT,
            cameraNear, cameraFar);
        XMMATRIX lightSpaceMatrix = XMMatrixIdentity(); // TODO: Shadow mapping
        
        // Use shader
        mainShader.Use(wireframeMode);
        
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
        
        // Render cube 1
        model = XMMatrixScaling(cubeScale.x, cubeScale.y, cubeScale.z);
        model = model * XMMatrixRotationX(XMConvertToRadians(cubeRotation.x));
        model = model * XMMatrixRotationY(XMConvertToRadians(cubeRotation.y));
        model = model * XMMatrixRotationZ(XMConvertToRadians(cubeRotation.z));
        model = model * XMMatrixTranslation(cubePosition.x, cubePosition.y, cubePosition.z);
        mainShader.SetMatrices(model, view, projection, lightSpaceMatrix);
        mainShader.SetObjectColor(cubeColor);
        DrawCube();
        
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
        }
        
        // Render projectiles
        for (auto& proj : projectiles) {
            if (proj.active) {
                model = XMMatrixScaling(projectileScale, projectileScale, projectileScale);
                model = model * XMMatrixTranslation(proj.position.x, proj.position.y, proj.position.z);
                mainShader.SetMatrices(model, view, projection, lightSpaceMatrix);
                mainShader.SetObjectColor(projectileColor);
                DrawCube();
            }
        }
        
        // Render gun viewmodel (simplified - just a cube for now)
        if (gunModel.visible) {
            // Position gun in front of camera
            XMVECTOR camPos = XMLoadFloat3(&camera.Position);
            XMVECTOR camFront = XMLoadFloat3(&camera.Front);
            XMVECTOR camRight = XMVector3Cross(XMLoadFloat3(&camera.Up), camFront);
            XMVECTOR camUp = XMLoadFloat3(&camera.Up);
            
            XMVECTOR gunPos = camPos + camFront * gunModel.offset.z + camRight * gunModel.offset.x + camUp * gunModel.offset.y;
            XMFLOAT3 gunPosition;
            XMStoreFloat3(&gunPosition, gunPos);
            
            model = XMMatrixScaling(gunModel.scale.x, gunModel.scale.y * 2.0f, gunModel.scale.z * 3.0f);
            model = model * XMMatrixRotationY(XMConvertToRadians(gunModel.rotation.y) + camera.Yaw * 0.0174533f);
            model = model * XMMatrixTranslation(gunPosition.x, gunPosition.y, gunPosition.z);
            mainShader.SetMatrices(model, view, projection, lightSpaceMatrix);
            mainShader.SetObjectColor(gunModel.color);
            DrawCube();
        }
        
        // Render light spheres
        for (int i = 0; i < clusteredRenderer.getTotalLightCount(); i++) {
            PointLightDX12* light = clusteredRenderer.getLight(i);
            if (!light || !light->active) continue;
            model = XMMatrixScaling(0.2f, 0.2f, 0.2f);
            model = model * XMMatrixTranslation(light->position.x, light->position.y, light->position.z);
            mainShader.SetMatrices(model, view, projection, lightSpaceMatrix);
            XMFLOAT3 lightCol(light->color.x * light->intensity, light->color.y * light->intensity, light->color.z * light->intensity);
            mainShader.SetObjectColor(lightCol);
            DrawCube();
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
            ImGui::BulletText("Left Click: Lock camera / Shoot");
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
                
                // DDGI Settings
                ImGui::Separator();
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

