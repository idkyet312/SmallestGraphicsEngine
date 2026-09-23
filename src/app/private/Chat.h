#ifndef APP_PRIVATE_CHAT_H
#define APP_PRIVATE_CHAT_H

// Multiplayer text chat, in the shape players expect from a Battlefield-style
// shooter: T opens a single-line prompt at the bottom left, Enter sends, Escape
// cancels, and recent lines stay on screen for a while after the prompt closes
// so a message can be read without opening anything.
//
// Deliberately not an ImGui window. The log has to be visible while the prompt
// is shut -- that is most of what chat is for -- and a window would need focus,
// a title bar and a position the player could drag off screen. The foreground
// draw list puts it over the HUD and nothing else has to know it exists.

// How long a line stays on screen once it arrives, and how long it takes to
// fade out at the end of that. Generous: a player reading a callout mid-firefight
// is not looking at the corner of the screen the moment it appears.
inline constexpr float kChatLineHoldSeconds = 12.0f;
inline constexpr float kChatLineFadeSeconds = 1.5f;

// Most lines drawn at once. Older ones stay in the log until they fade, but a
// burst of traffic should not climb up over the rest of the HUD.
inline constexpr int kChatVisibleLines = 8;

struct ChatLogEntry {
    std::string text;
    // Seconds since this line arrived, for the fade. Not a timestamp: the game
    // clock is the only time source here and it does not need to be absolute.
    float age = 0.0f;
    bool fromLocalPlayer = false;
};

static std::vector<ChatLogEntry> g_chatLog;
static std::vector<net::ChatLine> g_chatScratch;
static bool g_chatPromptOpen = false;
// Fixed buffer rather than a std::string because this is handed to the wire
// copy, which takes a C string, and because a chat line has a hard limit
// anyway. One over the protocol's limit for the terminator.
static char g_chatInput[net::kMaxChatTextLength + 1] = {};
static size_t g_chatInputLength = 0;
// Set the frame the prompt opens so the keystroke that opened it is not also
// typed into it. WM_CHAR for 't' arrives after the WM_KEYDOWN that opens the
// prompt, which would otherwise put a 't' in every message.
static bool g_chatSwallowNextChar = false;

static bool ChatPromptOpen() { return g_chatPromptOpen; }

// Chat only exists in a session. Asking for it in single player would open a
// prompt that can never send anything.
static bool ChatAvailable() {
    return MultiplayerActive() && IsGameplayScreen();
}

static void OpenChatPrompt() {
    if (!ChatAvailable() || g_chatPromptOpen) return;
    g_chatPromptOpen = true;
    g_textInputCapturesKeys = true;
    g_chatInput[0] = '\0';
    g_chatInputLength = 0;
    g_chatSwallowNextChar = true;
}

static void CloseChatPrompt() {
    g_chatPromptOpen = false;
    g_textInputCapturesKeys = false;
    g_chatSwallowNextChar = false;
    g_chatInput[0] = '\0';
    g_chatInputLength = 0;
}

// Enter: hand the line to the session and shut the prompt. The line is not
// echoed locally -- it comes back from the host like everyone else's, which is
// what keeps the ordering the same on every machine.
static void SubmitChatPrompt() {
    if (!g_chatPromptOpen) return;
    if (g_chatInputLength > 0) g_netSession.SendChat(g_chatInput);
    CloseChatPrompt();
}

// One printable character from WM_CHAR. Backspace is handled here too because
// it arrives as a control character on the same message.
static void ChatPromptChar(unsigned int character) {
    if (!g_chatPromptOpen) return;
    // Only the opening keystroke's own character is swallowed. If it never
    // arrives -- T pressed with a modifier that produces no character -- the
    // first real letter the player types must not be eaten in its place.
    if (g_chatSwallowNextChar) {
        g_chatSwallowNextChar = false;
        if (character == 't' || character == 'T') return;
    }
    if (character == '\b') {
        if (g_chatInputLength > 0) g_chatInput[--g_chatInputLength] = '\0';
        return;
    }
    // Enter and Escape come through WM_KEYDOWN, not here.
    // Printable ASCII only. The HUD font is ImGui's built-in one and ImGui
    // reads text as UTF-8, so a single Latin-1 byte would draw as a broken
    // glyph on every machine that received it.
    if (character < ' ' || character > '~') return;
    if (g_chatInputLength >= net::kMaxChatTextLength) return;
    g_chatInput[g_chatInputLength++] = static_cast<char>(character);
    g_chatInput[g_chatInputLength] = '\0';
}

// Drains whatever the session received and ages what is already on screen.
static void UpdateChat(float dt) {
    if (!MultiplayerActive()) {
        // Leaving a session clears the log rather than carrying someone else's
        // conversation into the next one.
        if (!g_chatLog.empty()) g_chatLog.clear();
        CloseChatPrompt();
        return;
    }
    // The level can end, or the run be abandoned, while the prompt is open.
    // Left open it would keep blanking every held key on the next screen.
    if (g_chatPromptOpen && !ChatAvailable()) CloseChatPrompt();

    g_netSession.DrainChat(g_chatScratch);
    for (const net::ChatLine& line : g_chatScratch) {
        ChatLogEntry entry;
        // Formatted here rather than in the session: this is the layer that
        // knows the game calls them Player-1 and Player-2 on the scoreboard.
        char label[48];
        std::snprintf(label, sizeof(label), "Player-%d",
                      static_cast<int>(line.speaker) + 1);
        entry.text = std::string(label) + ": " + line.text;
        entry.fromLocalPlayer = line.fromLocalPlayer;
        g_chatLog.push_back(std::move(entry));
    }

    // Age everything, then drop what has fully faded. Held at zero while the
    // prompt is open so a conversation does not expire out from under someone
    // who is mid-reply.
    if (!g_chatPromptOpen)
        for (ChatLogEntry& entry : g_chatLog) entry.age += dt;

    const float lifetime = kChatLineHoldSeconds + kChatLineFadeSeconds;
    g_chatLog.erase(
        std::remove_if(g_chatLog.begin(), g_chatLog.end(),
                       [lifetime](const ChatLogEntry& entry) {
                           return entry.age >= lifetime;
                       }),
        g_chatLog.end());

    // Bounded independently of the fade: a burst of traffic should not push the
    // HUD off the top of the screen before any of it has timed out.
    constexpr size_t kMaxLogged = 32;
    if (g_chatLog.size() > kMaxLogged)
        g_chatLog.erase(g_chatLog.begin(),
                        g_chatLog.begin() + (g_chatLog.size() - kMaxLogged));
}

static void DrawChat() {
    if (!MultiplayerActive()) return;
    if (g_chatLog.empty() && !g_chatPromptOpen) return;
    if (!IsGameplayScreen()) return;

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImDrawList* draw = ImGui::GetForegroundDrawList();

    // Bottom left, above where the prompt sits. Sized off the display so the
    // placement holds at any resolution rather than being tuned for one.
    const float margin = display.x * 0.02f;
    const float lineHeight = ImGui::GetTextLineHeight() + 2.0f;
    const float promptHeight = lineHeight + 8.0f;
    const float bottom = display.y * 0.78f;

    // Newest at the bottom, walking upward, so the eye lands on the most
    // recent line where it expects to.
    const size_t shown = (std::min)(g_chatLog.size(),
                                    static_cast<size_t>(kChatVisibleLines));
    float y = bottom - promptHeight;
    for (size_t i = 0; i < shown; ++i) {
        const ChatLogEntry& entry = g_chatLog[g_chatLog.size() - 1 - i];
        float alpha = 1.0f;
        if (!g_chatPromptOpen && entry.age > kChatLineHoldSeconds) {
            alpha = 1.0f - (entry.age - kChatLineHoldSeconds) /
                           kChatLineFadeSeconds;
            alpha = alpha > 0.0f ? (alpha < 1.0f ? alpha : 1.0f) : 0.0f;
        }
        if (alpha <= 0.0f) continue;

        const ImVec2 size = ImGui::CalcTextSize(entry.text.c_str());
        // A panel behind the text rather than an outline: the log sits over
        // terrain, sky and muzzle flash, and none of those can be relied on to
        // contrast with a glyph.
        draw->AddRectFilled(
            ImVec2(margin - 6.0f, y - 1.0f),
            ImVec2(margin + size.x + 6.0f, y + lineHeight - 1.0f),
            IM_COL32(0, 0, 0, static_cast<int>(110 * alpha)), 3.0f);
        // The local player's own lines are tinted so a player can pick their
        // message out of a busy log.
        const ImU32 color = entry.fromLocalPlayer
            ? IM_COL32(150, 220, 255, static_cast<int>(255 * alpha))
            : IM_COL32(235, 235, 235, static_cast<int>(255 * alpha));
        draw->AddText(ImVec2(margin, y), color, entry.text.c_str());
        y -= lineHeight;
    }

    if (!g_chatPromptOpen) return;

    // The prompt itself. A caret is appended rather than blinked: at 60 Hz a
    // blink is one more thing to time, and a static caret still says where the
    // text will land.
    const float promptY = bottom - promptHeight + 4.0f;
    const std::string prompt = std::string("Say: ") + g_chatInput + "_";
    const ImVec2 size = ImGui::CalcTextSize(prompt.c_str());
    draw->AddRectFilled(
        ImVec2(margin - 6.0f, promptY - 3.0f),
        ImVec2(margin + (std::max)(size.x, display.x * 0.25f) + 6.0f,
               promptY + lineHeight + 1.0f),
        IM_COL32(0, 0, 0, 190), 3.0f);
    draw->AddText(ImVec2(margin, promptY), IM_COL32(255, 255, 255, 255),
                  prompt.c_str());
}

#endif // APP_PRIVATE_CHAT_H
