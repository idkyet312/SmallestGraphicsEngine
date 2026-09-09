#pragma once

// Private application implementation; included once by main.cpp in dependency order.

// ?? entry point ??????????????????????????????????????????????????????????????
// Unhandled-exception hook: write a minidump next to the exe (dumps/crash.dmp)
// so a crash leaves a debuggable artifact instead of just an event-log entry.
static LONG WINAPI WriteCrashDump(EXCEPTION_POINTERS* info) {
    CreateDirectoryA("dumps", nullptr);
    HANDLE report = CreateFileA("dumps/crash.txt", GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (report != INVALID_HANDLE_VALUE) {
        const DWORD exceptionCode = info && info->ExceptionRecord
            ? info->ExceptionRecord->ExceptionCode : 0;
        const void* exceptionAddress = info && info->ExceptionRecord
            ? info->ExceptionRecord->ExceptionAddress : nullptr;
        char line[256] = {};
        const int length = _snprintf_s(line, sizeof(line), _TRUNCATE,
            "thread=%lu exception=0x%08lX address=%p\r\n",
            GetCurrentThreadId(), exceptionCode, exceptionAddress);
        DWORD written = 0;
        if (length > 0)
            WriteFile(report, line, static_cast<DWORD>(length), &written, nullptr);
        CloseHandle(report);
    }

    HANDLE file = CreateFileA("dumps/crash.dmp", GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei = {};
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = info;
        mei.ClientPointers = FALSE;
        // A normal minidump already includes thread contexts and stack memory.
        // Full-memory capture made a 5.5-GiB process write a 6.45-GiB dump, so a
        // crash looked like an indefinite loading hang before it finally exited.
        const MINIDUMP_TYPE dumpType = static_cast<MINIDUMP_TYPE>(
            MiniDumpNormal | MiniDumpWithThreadInfo |
            MiniDumpWithUnloadedModules | MiniDumpWithProcessThreadData |
            MiniDumpWithModuleHeaders);
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                          dumpType, &mei, nullptr, nullptr);
        CloseHandle(file);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

// ?? boot loading screen ????????????????????????????????????????????????????
//
// Startup does ~30 blocking init steps (shader compiles, HDRI prefilter, DXR
// BLAS builds) between CreateWindow and the message loop. Without this the
// window stays blank and "Not Responding" for the whole time. BootStep()
// presents one ImGui frame per step so the user sees progress, and pumps the
// message queue so Windows keeps considering the window alive.
namespace {

struct BootProgress {
    bool         active = false;
    HWND         hwnd = nullptr;
    uint32_t     stepIndex = 0;
    uint32_t     stepCount = 1;
    std::string  label;
    std::chrono::steady_clock::time_point startedAt{};
};

BootProgress g_boot;

// The same wordmark the level loading screen shows, typed in against boot
// progress. The player gets a title card, not a status readout: the step
// label, the counter, the bar and the elapsed clock were all diagnostic, and
// the console log still carries every one of them for debugging a slow boot.
//
// The diagnostic screen is not gone, only gated: DebugLoadingScreen brings it
// back, the same flag RenderLoadingScreen() reads for level loads. Boot draws
// through this function rather than that one, so it needs its own branch --
// without it the setting appeared to do nothing at startup.
void RenderBootScreen() {
    if (!g_settings.debugLoadingScreen) {
        // Step count drives the reveal rather than a wall clock, so the word
        // takes exactly as long to arrive as boot does on this machine instead
        // of completing early on a fast one or being cut off on a slow one.
        RenderMilboxWordmark(
            static_cast<float>(g_boot.stepIndex) /
            static_cast<float>((std::max)(1u, g_boot.stepCount)));
        return;
    }

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::GetBackgroundDrawList()->AddRectFilled(
        ImVec2(0, 0), display, IM_COL32(4, 8, 12, 255));

    const float width = 620.0f;
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f),
        ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(width, 330.0f), ImGuiCond_Always);
    // NoSavedSettings, unlike the level screen: boot runs before the main loop
    // and has no business writing window geometry into imgui.ini.
    ImGui::Begin("Booting", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings);

    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    const char* title = "GRAPHICS ENGINE";
    ImGui::SetCursorPosX((width - ImGui::CalcTextSize(title).x) * 0.5f);
    ImGui::TextUnformatted(title);
    ImGui::Dummy(ImVec2(0.0f, 14.0f));

    // g_boot, not g_game.loading: the level loading controller is never
    // Begin()-ed during boot, so its progress, label and records would all read
    // as idle placeholders. Everything this screen needs is already in g_boot.
    const float progress = (std::min)(1.0f,
        static_cast<float>(g_boot.stepIndex) /
        static_cast<float>((std::max)(1u, g_boot.stepCount)));
    char progressText[64];
    std::snprintf(progressText, sizeof(progressText), "%u / %u  (%.0f%%)",
        g_boot.stepIndex, g_boot.stepCount, progress * 100.0f);
    ImGui::ProgressBar(progress, ImVec2(-1.0f, 26.0f), progressText);

    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    ImGui::Text("%s", g_boot.label.c_str());

    const double elapsedMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - g_boot.startedAt).count();
    ImGui::TextDisabled("Elapsed: %.1f s", elapsedMs / 1000.0);

    // Absolute totals rather than the level screen's baseline-relative figures:
    // levelLoadingUploadBaseline is only assigned in BeginLevelLoading(), so
    // during boot it is still zero and subtracting it would mean nothing.
    ImGui::Separator();
    const StaticBufferStatsDX12 uploads = GetStaticBufferStatsDX12();
    ImGui::Text("GPU buffers: %u resources | %.2f MiB created",
        uploads.resources,
        static_cast<double>(uploads.bytes) / (1024.0 * 1024.0));
    ImGui::Text("Uploads: %u pending", uploads.pendingUploads);

    const StaticBufferDiagnosticDX12 resource = GetStaticBufferDiagnosticDX12();
    const char* resourceState = "D3D12_RESOURCE_STATE_UNKNOWN";
    if (resource.finalState == D3D12_RESOURCE_STATE_INDEX_BUFFER)
        resourceState = "D3D12_RESOURCE_STATE_INDEX_BUFFER";
    else if (resource.finalState == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
        resourceState = "D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE";
    else if (resource.finalState ==
        (D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER |
         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE))
        resourceState = "D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | NON_PIXEL_SHADER_RESOURCE";
    else if (resource.finalState == D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER)
        resourceState = "D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER";
    ImGui::Text("DX12 resource #%llu: %s  %.2f KiB",
        static_cast<unsigned long long>(resource.serial), resource.label.c_str(),
        static_cast<double>(resource.bytes) / 1024.0);
    ImGui::TextDisabled("  COPY_DEST -> %s [0x%X]", resourceState,
        static_cast<unsigned>(resource.finalState));

    const HRESULT gpuHealth = g_dx12.device
        ? g_dx12.device->GetDeviceRemovedReason() : E_POINTER;
    if (gpuHealth == S_OK)
        ImGui::TextColored(ImVec4(0.35f, 1.0f, 0.45f, 1.0f), "GPU: OK");
    else
        ImGui::TextColored(ImVec4(1.0f, 0.25f, 0.2f, 1.0f),
            "GPU: REMOVED 0x%08lX", static_cast<unsigned long>(gpuHealth));

    ImGui::End();
}

// Draws one boot frame with `label` as the current step. Safe to call before
// the scene exists: it only clears the backbuffer and renders ImGui.
void BootStep(const char* label) {
    if (!g_boot.active) return;

    g_boot.label = label ? label : "";
    std::cout << "[Boot] " << g_boot.label << std::endl;

    // Keep the window responsive. Boot init is single-threaded, so this is the
    // only chance Windows gets to see the message queue drained.
    MSG msg;
    while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    BeginFrame();
    const float clearColor[4] = { 0.016f, 0.031f, 0.047f, 1.0f };
    ClearRenderTarget(clearColor);

    ID3D12DescriptorHeap* heaps[] = { imguiSrvHeap.Get() };
    g_dx12.commandList->SetDescriptorHeaps(1, heaps);

    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    RenderBootScreen();
    ImGui::Render();
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_dx12.commandList.Get());

    EndFrame();

    ++g_boot.stepIndex;
}

} // namespace
