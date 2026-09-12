#pragma once

static HWND g_inputWindow = nullptr;

static bool HasInputFocus() {
    return g_inputWindow && GetForegroundWindow() == g_inputWindow &&
        GetFocus() == g_inputWindow;
}

static SHORT FocusedKeyState(int key) {
    // Async key state is desktop-wide, even when another application is active.
    return HasInputFocus() ? GetAsyncKeyState(key) : 0;
}
