#pragma once

// Private application implementation; included once by main.cpp in dependency order.

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
    float tile = 8.0f;
    std::vector<VertexPosNormUV> planeVerts = {
        {{-s,0, s},{0,1,0},{0,0}}, {{ s,0, s},{0,1,0},{tile,0}}, {{ s,0,-s},{0,1,0},{tile,tile}},
        {{-s,0, s},{0,1,0},{0,0}}, {{ s,0,-s},{0,1,0},{tile,tile}}, {{-s,0,-s},{0,1,0},{0,tile}},
    };
    if (!CreateVertexBuffer(planeVerts, geo.planeVertexBuffer, geo.planeVBV)) return false;

    // Unit sphere (radius 0.5) for projectiles / debug spheres.
    std::vector<VertexPosNormUV> sphereVerts = BuildSphereVertices();
    if (!CreateVertexBuffer(sphereVerts, geo.sphereVertexBuffer, geo.sphereVBV)) return false;

    std::vector<VertexPosNormUV> capsuleVerts = BuildCapsuleVertices();
    if (!CreateVertexBuffer(capsuleVerts, geo.capsuleVertexBuffer, geo.capsuleVBV)) return false;

    // Unit XY quad (-0.5..0.5) with UV 0..1 for camera-facing smoke billboards.
    std::vector<VertexPosNormUV> quadVerts = {
        {{-0.5f,-0.5f,0},{0,0,1},{0,1}}, {{ 0.5f,-0.5f,0},{0,0,1},{1,1}}, {{ 0.5f, 0.5f,0},{0,0,1},{1,0}},
        {{-0.5f,-0.5f,0},{0,0,1},{0,1}}, {{ 0.5f, 0.5f,0},{0,0,1},{1,0}}, {{-0.5f, 0.5f,0},{0,0,1},{0,0}},
    };
    if (!CreateVertexBuffer(quadVerts, geo.quadVertexBuffer, geo.quadVBV)) return false;

    // Kenney CC0 muzzle flash is one transparent full-frame sprite.
    std::vector<VertexPosNormUV> flashVerts = {
        {{-0.5f,-0.5f,0},{0,0,1},{0,1}}, {{ 0.5f,-0.5f,0},{0,0,1},{1,1}}, {{ 0.5f, 0.5f,0},{0,0,1},{1,0}},
        {{-0.5f,-0.5f,0},{0,0,1},{0,1}}, {{ 0.5f, 0.5f,0},{0,0,1},{1,0}}, {{-0.5f, 0.5f,0},{0,0,1},{0,0}},
    };
    if (!CreateVertexBuffer(flashVerts, geo.flashVertexBuffer, geo.flashVBV)) return false;

    // OpenGameArt CC0 realistic fire sheet: 5x5, 25 frames at 128 px.
    std::vector<VertexPosNormUV> fireVerts;
    fireVerts.reserve(25 * 6);
    for (int frame = 0; frame < 25; ++frame) {
        const int column = frame % 5;
        const int row = frame / 5;
        const float u0 = column / 5.0f, u1 = (column + 1) / 5.0f;
        const float v0 = row / 5.0f, v1 = (row + 1) / 5.0f;
        fireVerts.insert(fireVerts.end(), {
            {{-0.5f,-0.5f,0},{0,0,1},{u0,v1}}, {{ 0.5f,-0.5f,0},{0,0,1},{u1,v1}}, {{ 0.5f, 0.5f,0},{0,0,1},{u1,v0}},
            {{-0.5f,-0.5f,0},{0,0,1},{u0,v1}}, {{ 0.5f, 0.5f,0},{0,0,1},{u1,v0}}, {{-0.5f, 0.5f,0},{0,0,1},{u0,v0}},
        });
    }
    if (!CreateVertexBuffer(fireVerts, geo.fireVertexBuffer, geo.fireVBV)) return false;

    // OpenGameArt CC0 explosion sheet: 4 columns x 4 rows, 16 frames at 128 px.
    std::vector<VertexPosNormUV> explosionVerts;
    explosionVerts.reserve(16 * 6);
    for (int frame = 0; frame < 16; ++frame) {
        const int column = frame % 4;
        const int row = frame / 4;
        const float u0 = column / 4.0f, u1 = (column + 1) / 4.0f;
        const float v0 = row / 4.0f, v1 = (row + 1) / 4.0f;
        explosionVerts.insert(explosionVerts.end(), {
            {{-0.5f,-0.5f,0},{0,0,1},{u0,v1}}, {{ 0.5f,-0.5f,0},{0,0,1},{u1,v1}}, {{ 0.5f, 0.5f,0},{0,0,1},{u1,v0}},
            {{-0.5f,-0.5f,0},{0,0,1},{u0,v1}}, {{ 0.5f, 0.5f,0},{0,0,1},{u1,v0}}, {{-0.5f, 0.5f,0},{0,0,1},{u0,v0}},
        });
    }
    if (!CreateVertexBuffer(explosionVerts, geo.explosionVertexBuffer, geo.explosionVBV)) return false;

    // CC0 white-hot core sheet: 8 columns x 8 rows, 64 frames at 128 px.
    std::vector<VertexPosNormUV> explosionCoreVerts;
    explosionCoreVerts.reserve(64 * 6);
    for (int frame = 0; frame < 64; ++frame) {
        const int column = frame % 8;
        const int row = frame / 8;
        const float u0 = column / 8.0f, u1 = (column + 1) / 8.0f;
        const float v0 = row / 8.0f, v1 = (row + 1) / 8.0f;
        explosionCoreVerts.insert(explosionCoreVerts.end(), {
            {{-0.5f,-0.5f,0},{0,0,1},{u0,v1}}, {{ 0.5f,-0.5f,0},{0,0,1},{u1,v1}}, {{ 0.5f, 0.5f,0},{0,0,1},{u1,v0}},
            {{-0.5f,-0.5f,0},{0,0,1},{u0,v1}}, {{ 0.5f, 0.5f,0},{0,0,1},{u1,v0}}, {{-0.5f, 0.5f,0},{0,0,1},{u0,v0}},
        });
    }
    if (!CreateVertexBuffer(explosionCoreVerts, geo.explosionCoreVertexBuffer,
                            geo.explosionCoreVBV)) return false;
    geo.sphereVertexCount = (UINT)sphereVerts.size();
    geo.capsuleVertexCount = (UINT)capsuleVerts.size();

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
// On-screen movement pad. Buttons in the UI set these each frame; ProcessInput
// drains them. Kept separate from the keyboard path so the pad still works while
// the camera is locked (which is exactly when the mouse is free to click it).
VirtualInput virtualInput;
