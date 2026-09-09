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
void RenderBootScreen() {
    // Step count drives the reveal rather than a wall clock, so the word takes
    // exactly as long to arrive as boot does on this machine instead of
    // completing early on a fast one or being cut off on a slow one.
    RenderMilboxWordmark(
        static_cast<float>(g_boot.stepIndex) /
        static_cast<float>((std::max)(1u, g_boot.stepCount)));
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
