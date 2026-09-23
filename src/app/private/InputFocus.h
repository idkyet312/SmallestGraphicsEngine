#pragma once

static HWND g_inputWindow = nullptr;

static bool HasInputFocus() {
    return g_inputWindow && GetForegroundWindow() == g_inputWindow &&
        GetFocus() == g_inputWindow;
}

// Set while a text prompt (the multiplayer chat) owns the keyboard. Every held
// key the game polls goes through FocusedKeyState, so blanking it here stops a
// typed sentence from walking, jumping, reloading or firing -- while movement
// physics and the networked input keep running, just with nothing pressed.
static bool g_textInputCapturesKeys = false;

static SHORT FocusedKeyState(int key) {
    if (g_textInputCapturesKeys) return 0;
    // Async key state is desktop-wide, even when another application is active.
    return HasInputFocus() ? GetAsyncKeyState(key) : 0;
}
