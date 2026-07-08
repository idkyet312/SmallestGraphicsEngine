#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <iostream>
#include <chrono>
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx12.h>

#include "DX12Core.h"
#include "ShaderDX12.h"
#include "VisibilityBufferDX12.h"
#include "Scene.h"
#include "ForwardRenderer.h"
#include "IdTechRenderer.h"
#include "RaytracingDX12.h"
#include "EngineUI.h"
#include "GLBImporter.h"
#include "MipGenerator.h"

using namespace DirectX;

// ?? globals ??????????????????????????????????????????????????????????????????
static unsigned int SCR_WIDTH  = 1280;
static unsigned int SCR_HEIGHT = 720;

static Scene               scene;
static ShaderDX12           mainShader;
static VisibilityBufferDX12 visBuffer;
static GeometryBuffers      geo;
static PackedGeometry       packed;
static std::shared_ptr<SceneNode> crateModel;
static bool                 crateLoadAttempted = false;

static float lastX = SCR_WIDTH / 2.0f;
static float lastY = SCR_HEIGHT / 2.0f;
static bool  firstMouse   = true;
static bool  ignoreNextMouseMove = false;
static bool  showUI        = true;
static bool  cameraLocked  = true;
static bool  isFullscreen  = false;
static RECT  windowedRect  = {};
static DWORD windowedStyle = 0;
static float deltaTime     = 0.0f;

static ComPtr<ID3D12DescriptorHeap> imguiSrvHeap;

// ?? timer ????????????????????????????????????????????????????????????????????
class Timer {
    std::chrono::high_resolution_clock::time_point t0;
public:
    void  Start()      { t0 = std::chrono::high_resolution_clock::now(); }
    float GetElapsed() {
        return std::chrono::duration<float>(
            std::chrono::high_resolution_clock::now() - t0).count();
    }
};
static Timer gameTimer;

// ?? forward decls ????????????????????????????????????????????????????????????
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

// ?? geometry creation ????????????????????????????????????????????????????????
static bool CreateAllGeometry() {
    std::vector<VertexPosNormUV> cubeVerts = {
        // Front
        {{-0.5f,-0.5f, 0.5f},{ 0, 0, 1},{0,0}}, {{ 0.5f,-0.5f, 0.5f},{ 0, 0, 1},{1,0}},
        {{ 0.5f, 0.5f, 0.5f},{ 0, 0, 1},{1,1}}, {{-0.5f,-0.5f, 0.5f},{ 0, 0, 1},{0,0}},
        {{ 0.5f, 0.5f, 0.5f},{ 0, 0, 1},{1,1}}, {{-0.5f, 0.5f, 0.5f},{ 0, 0, 1},{0,1}},
        // Back
        {{ 0.5f,-0.5f,-0.5f},{ 0, 0,-1},{0,0}}, {{-0.5f,-0.5f,-0.5f},{ 0, 0,-1},{1,0}},
        {{-0.5f, 0.5f,-0.5f},{ 0, 0,-1},{1,1}}, {{ 0.5f,-0.5f,-0.5f},{ 0, 0,-1},{0,0}},
        {{-0.5f, 0.5f,-0.5f},{ 0, 0,-1},{1,1}}, {{ 0.5f, 0.5f,-0.5f},{ 0, 0,-1},{0,1}},
        // Top
        {{-0.5f, 0.5f, 0.5f},{ 0, 1, 0},{0,0}}, {{ 0.5f, 0.5f, 0.5f},{ 0, 1, 0},{1,0}},
        {{ 0.5f, 0.5f,-0.5f},{ 0, 1, 0},{1,1}}, {{-0.5f, 0.5f, 0.5f},{ 0, 1, 0},{0,0}},
        {{ 0.5f, 0.5f,-0.5f},{ 0, 1, 0},{1,1}}, {{-0.5f, 0.5f,-0.5f},{ 0, 1, 0},{0,1}},
        // Bottom
        {{-0.5f,-0.5f,-0.5f},{ 0,-1, 0},{0,0}}, {{ 0.5f,-0.5f,-0.5f},{ 0,-1, 0},{1,0}},
        {{ 0.5f,-0.5f, 0.5f},{ 0,-1, 0},{1,1}}, {{-0.5f,-0.5f,-0.5f},{ 0,-1, 0},{0,0}},
        {{ 0.5f,-0.5f, 0.5f},{ 0,-1, 0},{1,1}}, {{-0.5f,-0.5f, 0.5f},{ 0,-1, 0},{0,1}},
        // Right
        {{ 0.5f,-0.5f, 0.5f},{ 1, 0, 0},{0,0}}, {{ 0.5f,-0.5f,-0.5f},{ 1, 0, 0},{1,0}},
        {{ 0.5f, 0.5f,-0.5f},{ 1, 0, 0},{1,1}}, {{ 0.5f,-0.5f, 0.5f},{ 1, 0, 0},{0,0}},
        {{ 0.5f, 0.5f,-0.5f},{ 1, 0, 0},{1,1}}, {{ 0.5f, 0.5f, 0.5f},{ 1, 0, 0},{0,1}},
        // Left
        {{-0.5f,-0.5f,-0.5f},{-1, 0, 0},{0,0}}, {{-0.5f,-0.5f, 0.5f},{-1, 0, 0},{1,0}},
        {{-0.5f, 0.5f, 0.5f},{-1, 0, 0},{1,1}}, {{-0.5f,-0.5f,-0.5f},{-1, 0, 0},{0,0}},
        {{-0.5f, 0.5f, 0.5f},{-1, 0, 0},{1,1}}, {{-0.5f, 0.5f,-0.5f},{-1, 0, 0},{0,1}},
    };
    if (!CreateVertexBuffer(cubeVerts, geo.cubeVertexBuffer, geo.cubeVBV)) return false;

    float s = 20.0f;
    std::vector<VertexPosNormUV> planeVerts = {
        {{-s,0, s},{0,1,0},{0,0}}, {{ s,0, s},{0,1,0},{1,0}}, {{ s,0,-s},{0,1,0},{1,1}},
        {{-s,0, s},{0,1,0},{0,0}}, {{ s,0,-s},{0,1,0},{1,1}}, {{-s,0,-s},{0,1,0},{0,1}},
    };
    if (!CreateVertexBuffer(planeVerts, geo.planeVertexBuffer, geo.planeVBV)) return false;

    BuildPackedGeometry(cubeVerts, planeVerts, packed);
    return true;
}

// ?? fullscreen toggle ????????????????????????????????????????????????????????
static void ToggleFullscreen(HWND hwnd) {
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

// ?? input ????????????????????????????????????????????????????????????????????
static void ProcessInput(HWND) {
    if (cameraLocked || (showUI && ImGui::GetIO().WantCaptureKeyboard)) return;
    if (GetAsyncKeyState('W') & 0x8000) scene.camera.ProcessKeyboard('W', deltaTime);
    if (GetAsyncKeyState('S') & 0x8000) scene.camera.ProcessKeyboard('S', deltaTime);
    if (GetAsyncKeyState('A') & 0x8000) scene.camera.ProcessKeyboard('A', deltaTime);
    if (GetAsyncKeyState('D') & 0x8000) scene.camera.ProcessKeyboard('D', deltaTime);
    if (GetAsyncKeyState(VK_SPACE) & 0x8000) scene.camera.ProcessKeyboard(' ', deltaTime);
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000) scene.camera.ProcessKeyboard('Q', deltaTime);
}

// ?? window proc ??????????????????????????????????????????????????????????????
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) return true;

    switch (msg) {
    case WM_SIZE:
        if (g_dx12.device && g_dx12.initialized && wParam != SIZE_MINIMIZED) {
            unsigned w = LOWORD(lParam), h = HIWORD(lParam);
            if (w > 0 && h > 0 && (w != SCR_WIDTH || h != SCR_HEIGHT)) {
                WaitForGPU();
                SCR_WIDTH = w; SCR_HEIGHT = h;
                ResizeDX12(SCR_WIDTH, SCR_HEIGHT);
                if (visBuffer.initialized) visBuffer.Resize(SCR_WIDTH, SCR_HEIGHT);
                if (g_rt.initialized) ResizeRaytracing(SCR_WIDTH, SCR_HEIGHT);
            }
        }
        return 0;

    case WM_MOUSEMOVE:
        if (!cameraLocked && !(showUI && ImGui::GetIO().WantCaptureMouse)) {
            if (ignoreNextMouseMove) {
                // This move was generated by our own SetCursorPos recenter below,
                // not real user input - skip it so it can't be misread as a delta.
                ignoreNextMouseMove = false;
                return 0;
            }

            float xpos = (float)GET_X_LPARAM(lParam);
            float ypos = (float)GET_Y_LPARAM(lParam);

            RECT r; GetClientRect(hwnd, &r);
            float centerX = (float)(r.right - r.left) / 2.0f;
            float centerY = (float)(r.bottom - r.top) / 2.0f;

            if (firstMouse) { lastX = centerX; lastY = centerY; firstMouse = false; }
            else {
                float dx = xpos - lastX;
                float dy = lastY - ypos; // screen Y grows downward; flip so moving the mouse up looks up
                if (dx != 0.0f || dy != 0.0f) scene.camera.ProcessMouseMovement(dx, dy);
            }

            // Re-center the cursor every move so it never reaches the screen
            // edge and clamps, which would otherwise cap how far you can turn.
            POINT c = { (LONG)centerX, (LONG)centerY };
            ClientToScreen(hwnd, &c);
            ignoreNextMouseMove = true;
            SetCursorPos(c.x, c.y);
            lastX = centerX; lastY = centerY;
        }
        return 0;

    case WM_LBUTTONDOWN:
        if (!ImGui::GetIO().WantCaptureMouse) {
            if (cameraLocked) {
                cameraLocked = false;
                SetCapture(hwnd); ShowCursor(FALSE);
                RECT r; GetClientRect(hwnd, &r);
                POINT c = { (r.right-r.left)/2, (r.bottom-r.top)/2 };
                ClientToScreen(hwnd, &c);
                ignoreNextMouseMove = true;
                SetCursorPos(c.x, c.y);
                lastX = (float)(r.right-r.left)/2;
                lastY = (float)(r.bottom-r.top)/2;
                firstMouse = true;
            } else {
                scene.ShootProjectile();
            }
        }
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) PostQuitMessage(0);
        else if (wParam == VK_TAB) {
            showUI = !showUI;
            if (showUI) {
                cameraLocked = true;
                ReleaseCapture(); ShowCursor(TRUE);
            } else {
                // Hiding the UI: capture and re-center the mouse so the next
                // WM_MOUSEMOVE delta is computed from the window center instead
                // of wherever the cursor happened to be over the UI, which
                // otherwise causes the camera to snap-rotate on the first move.
                cameraLocked = false;
                SetCapture(hwnd); ShowCursor(FALSE);
                RECT r; GetClientRect(hwnd, &r);
                POINT c = { (r.right - r.left) / 2, (r.bottom - r.top) / 2 };
                ClientToScreen(hwnd, &c);
                ignoreNextMouseMove = true;
                SetCursorPos(c.x, c.y);
                lastX = (float)(r.right - r.left) / 2;
                lastY = (float)(r.bottom - r.top) / 2;
                firstMouse = true;
            }
        }
        else if (wParam == 'C')    { cameraLocked = true; ReleaseCapture(); ShowCursor(TRUE); }
        else if (wParam == 'F')    { scene.camera.FPSMode = !scene.camera.FPSMode; }
        else if (wParam == VK_F11) { ToggleFullscreen(hwnd); }
        return 0;

    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// ?? entry point ??????????????????????????????????????????????????????????????
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    AllocConsole();
    FILE* fp;
    freopen_s(&fp, "CONOUT$", "w", stdout);
    freopen_s(&fp, "CONOUT$", "w", stderr);

    std::cout << "GraphicEngine DX12 Starting..." << std::endl;

    // Window
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"GraphicEngineDX12";
    RegisterClassExW(&wc);

    RECT rc = { 0, 0, (LONG)SCR_WIDTH, (LONG)SCR_HEIGHT };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowW(L"GraphicEngineDX12", L"Graphics Engine - DirectX 12",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) { std::cerr << "Window creation failed\n"; return -1; }
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    // DX12
    try {
        if (!InitDX12(hwnd, SCR_WIDTH, SCR_HEIGHT)) {
            MessageBoxA(hwnd, "Failed to init DX12.", "Error", MB_OK | MB_ICONERROR);
            return -1;
        }
    } catch (const std::exception& e) {
        MessageBoxA(hwnd, e.what(), "DX12 Error", MB_OK | MB_ICONERROR);
        return -1;
    }

    // ImGui
    D3D12_DESCRIPTOR_HEAP_DESC ihd = {};
    ihd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    ihd.NumDescriptors = 1;
    ihd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    g_dx12.device->CreateDescriptorHeap(&ihd, IID_PPV_ARGS(&imguiSrvHeap));

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX12_Init(g_dx12.device.Get(), FRAME_COUNT, DXGI_FORMAT_R8G8B8A8_UNORM,
        imguiSrvHeap.Get(),
        imguiSrvHeap->GetCPUDescriptorHandleForHeapStart(),
        imguiSrvHeap->GetGPUDescriptorHandleForHeapStart());

    // Geometry
    if (!CreateAllGeometry()) { std::cerr << "Geometry creation failed\n"; return -1; }

    // Shaders - the DX12-specific pair supports albedo/normal/metal-roughness texture
    // sampling (needed for imported GLB materials); the plain "clustered_*" pair is
    // color-only and silently ignores textures, so it's only a fallback.
    if (!mainShader.Load("shaders/clustered_dx12_vs.hlsl", "shaders/clustered_dx12_ps.hlsl")) {
        std::cerr << "Trying fallback shaders...\n";
        if (!mainShader.Load("shaders/clustered_vs.hlsl", "shaders/clustered_ps.hlsl")) {
            MessageBoxA(hwnd, "Shader load failed.", "Shader Error", MB_OK | MB_ICONERROR);
            return -1;
        }
    }
    std::cout << "Shaders loaded\n";

    // Mip generator (compute shader) for imported GLB textures
    if (!g_mipGen.Init()) {
        std::cerr << "Mip generator init failed (non-fatal, textures will have no mips)\n";
    }

    // Visibility buffer (id Tech path)
    if (!visBuffer.Init(SCR_WIDTH, SCR_HEIGHT)) {
        std::cerr << "VB init failed (non-fatal)\n";
        scene.useVisibilityBuffer = false;
    } else {
        std::cout << "Visibility Buffer ready\n";
    }

    // Raytracing (DXR path)
    if (!InitRaytracing(geo)) {
        std::cerr << "DXR init failed (non-fatal)\n";
        scene.useRaytracing = false;
    } else {
        std::cout << "DXR Raytracing ready\n";
    }

    // Scene lights
    scene.InitLights();

    // Timer
    gameTimer.Start();
    float lastTime = 0.0f;

    std::cout << "Controls: WASD, Mouse, TAB=UI, F11=Fullscreen, ESC=Exit\n";

    // ?? main loop ????????????????????????????????????????????????????????????
    MSG msg = {};
    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            continue;
        }

        float now = gameTimer.GetElapsed();
        deltaTime = now - lastTime;
        lastTime  = now;

        ProcessInput(hwnd);
        scene.Update(deltaTime, now);

        // ?? begin frame ??
        try { BeginFrame(); }
        catch (const std::exception& e) { std::cerr << "BeginFrame: " << e.what() << "\n"; break; }

        float cc[4] = { scene.clearColor.x, scene.clearColor.y, scene.clearColor.z, 1.0f };
        ClearRenderTarget(cc);

        mainShader.BeginFrame();

        if (!crateLoadAttempted) {
            crateLoadAttempted = true;
            std::cout << "Loading models/h1.glb...\n";
            crateModel = GLBImporter::LoadGLB("models/h1.glb", g_dx12.device, g_dx12.commandList);
            if (crateModel) {
                crateModel->UpdateGlobalTransform(crateModel->localTransform);
                std::cout << "h1 model loaded\n";
            } else {
                std::cerr << "Failed to load h1.glb, falling back to procedural cube\n";
            }

            // Flush the load/mip-generation commands now and print any D3D12
            // validation errors before continuing, so mip-related bugs surface
            // immediately instead of silently corrupting later frames.
            ThrowIfFailed(g_dx12.commandList->Close());
            ID3D12CommandList* loadLists[] = { g_dx12.commandList.Get() };
            g_dx12.commandQueue->ExecuteCommandLists(1, loadLists);
            WaitForGPU();
            DumpDX12DebugMessages();
            ThrowIfFailed(g_dx12.commandAllocators[g_dx12.frameIndex]->Reset());
            ThrowIfFailed(g_dx12.commandList->Reset(g_dx12.commandAllocators[g_dx12.frameIndex].Get(), nullptr));
            // Reset() drops all descriptor heap bindings and the RTV/DSV binding
            // set by BeginFrame()/ClearRenderTarget() earlier this frame, so redo
            // that setup (minus the PRESENT->RENDER_TARGET barrier, which only
            // applies once - the target is already in RENDER_TARGET state).
            ID3D12DescriptorHeap* mainHeaps[] = { g_dx12.cbvSrvUavHeap.Get(), g_dx12.samplerHeap.Get() };
            g_dx12.commandList->SetDescriptorHeaps(2, mainHeaps);
            g_dx12.commandList->RSSetViewports(1, &g_dx12.viewport);
            g_dx12.commandList->RSSetScissorRects(1, &g_dx12.scissorRect);
            ClearRenderTarget(cc);
            mainShader.BeginFrame();
        }

        // ?? render ??
        if (scene.useRaytracing && g_rt.initialized) {
            RenderRaytracing(scene);
        } else if (scene.useVisibilityBuffer && visBuffer.initialized) {
            RenderIdTech(scene, mainShader, visBuffer, geo, packed);
        } else {
            RenderForward(scene, mainShader, geo, crateModel);
        }

        // Ensure ImGui renders to the swapchain backbuffer (VB path changes OM target)
        {
            D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = GetCPUDescriptorHandle(
                g_dx12.rtvHeap.Get(), g_dx12.rtvDescriptorSize, g_dx12.frameIndex);
            D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = g_dx12.dsvHeap->GetCPUDescriptorHandleForHeapStart();
            g_dx12.commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);
            g_dx12.commandList->RSSetViewports(1, &g_dx12.viewport);
            g_dx12.commandList->RSSetScissorRects(1, &g_dx12.scissorRect);
        }

        // ?? ImGui ??
        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        if (showUI) RenderUI(scene, visBuffer);
        ImGui::Render();

        ID3D12DescriptorHeap* heaps[] = { imguiSrvHeap.Get() };
        g_dx12.commandList->SetDescriptorHeaps(1, heaps);
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_dx12.commandList.Get());

        // ?? end frame ??
        try { EndFrame(); }
        catch (const std::exception& e) { std::cerr << "EndFrame: " << e.what() << "\n"; break; }
    }

    WaitForGPU();
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDX12();
    return (int)msg.wParam;
}

