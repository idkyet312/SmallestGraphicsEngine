#pragma once

// Private application implementation; included once by main.cpp in dependency order.

// ---- Shared UI theme -------------------------------------------------------
//
// The HUD is hand-drawn with ImDrawList in a dark plate + pale text palette
// (see RenderPlayerHUD), while every menu ran on stock StyleColorsDark. The two
// did not look like the same game. These constants are the HUD's own colours
// promoted to a shared vocabulary, and ApplyEngineUITheme pushes them into
// ImGui so windows, buttons and sliders inherit them.
//
// Kept as ImVec4 rather than IM_COL32 because ImGuiStyle stores floats; the
// HUD's integer literals are the same values over 255.
namespace UITheme {
// Near-black with a green cast, matching the HUD's IM_COL32(8, 12, 10, 180)
// backing plates.
inline const ImVec4 kPanel        = ImVec4(0.031f, 0.047f, 0.039f, 0.960f);
inline const ImVec4 kPanelRaised  = ImVec4(0.055f, 0.075f, 0.063f, 1.000f);
inline const ImVec4 kControl      = ImVec4(0.086f, 0.114f, 0.098f, 1.000f);
inline const ImVec4 kControlHover = ImVec4(0.137f, 0.180f, 0.153f, 1.000f);
inline const ImVec4 kControlHeld  = ImVec4(0.196f, 0.255f, 0.216f, 1.000f);
// The HUD's healthy-green, used as the single accent so a highlighted control
// and a full health bar are recognisably the same colour.
inline const ImVec4 kAccent       = ImVec4(0.149f, 0.698f, 0.322f, 1.000f);
inline const ImVec4 kAccentDim    = ImVec4(0.110f, 0.463f, 0.227f, 1.000f);
inline const ImVec4 kText         = ImVec4(0.886f, 0.918f, 0.882f, 1.000f);
inline const ImVec4 kTextDim      = ImVec4(0.514f, 0.573f, 0.529f, 1.000f);
inline const ImVec4 kBorder       = ImVec4(0.259f, 0.318f, 0.278f, 0.725f);
inline const ImVec4 kWarning      = ImVec4(1.000f, 0.784f, 0.235f, 1.000f);
} // namespace UITheme

// Applies the theme above to the global ImGui style. Called once at init.
static void ApplyEngineUITheme() {
    ImGuiStyle& style = ImGui::GetStyle();
    ImGui::StyleColorsDark(&style);

    // Geometry first. Softer corners and a consistent rhythm of padding do more
    // for "modern" than any colour change -- stock ImGui's 0-radius frames and
    // tight 4 px spacing are what read as a debug tool.
    style.WindowRounding    = 8.0f;
    style.ChildRounding     = 6.0f;
    style.FrameRounding     = 5.0f;
    style.PopupRounding     = 6.0f;
    style.ScrollbarRounding = 6.0f;
    style.GrabRounding      = 5.0f;
    style.TabRounding       = 5.0f;

    style.WindowPadding     = ImVec2(18.0f, 16.0f);
    style.FramePadding      = ImVec2(12.0f, 7.0f);
    style.ItemSpacing       = ImVec2(10.0f, 9.0f);
    style.ItemInnerSpacing  = ImVec2(8.0f, 6.0f);
    style.ScrollbarSize     = 12.0f;
    style.GrabMinSize       = 11.0f;

    // Hairline borders. A 1 px edge on panels separates them from the scene
    // behind without the heavy frames stock ImGui draws.
    style.WindowBorderSize  = 1.0f;
    style.FrameBorderSize   = 1.0f;
    style.PopupBorderSize   = 1.0f;
    style.ChildBorderSize   = 1.0f;

    // Titles and most labels centre in these panels, which are all fixed-size
    // and centred themselves.
    style.WindowTitleAlign  = ImVec2(0.5f, 0.5f);
    style.ButtonTextAlign   = ImVec2(0.5f, 0.5f);

    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg]             = UITheme::kPanel;
    colors[ImGuiCol_ChildBg]              = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
    colors[ImGuiCol_PopupBg]              = UITheme::kPanelRaised;
    colors[ImGuiCol_Border]               = UITheme::kBorder;
    colors[ImGuiCol_BorderShadow]         = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);

    colors[ImGuiCol_Text]                 = UITheme::kText;
    colors[ImGuiCol_TextDisabled]         = UITheme::kTextDim;

    colors[ImGuiCol_FrameBg]              = UITheme::kControl;
    colors[ImGuiCol_FrameBgHovered]       = UITheme::kControlHover;
    colors[ImGuiCol_FrameBgActive]        = UITheme::kControlHeld;

    colors[ImGuiCol_TitleBg]              = UITheme::kPanel;
    colors[ImGuiCol_TitleBgActive]        = UITheme::kPanelRaised;
    colors[ImGuiCol_TitleBgCollapsed]     = UITheme::kPanel;
    colors[ImGuiCol_MenuBarBg]            = UITheme::kPanelRaised;

    colors[ImGuiCol_Button]               = UITheme::kControl;
    colors[ImGuiCol_ButtonHovered]        = UITheme::kControlHover;
    colors[ImGuiCol_ButtonActive]         = UITheme::kControlHeld;

    colors[ImGuiCol_Header]               = UITheme::kAccentDim;
    colors[ImGuiCol_HeaderHovered]        = UITheme::kControlHover;
    colors[ImGuiCol_HeaderActive]         = UITheme::kAccent;

    colors[ImGuiCol_CheckMark]            = UITheme::kAccent;
    colors[ImGuiCol_SliderGrab]           = UITheme::kAccentDim;
    colors[ImGuiCol_SliderGrabActive]     = UITheme::kAccent;

    colors[ImGuiCol_Separator]            = UITheme::kBorder;
    colors[ImGuiCol_SeparatorHovered]     = UITheme::kAccentDim;
    colors[ImGuiCol_SeparatorActive]      = UITheme::kAccent;

    colors[ImGuiCol_ScrollbarBg]          = ImVec4(0.024f, 0.035f, 0.031f, 0.6f);
    colors[ImGuiCol_ScrollbarGrab]        = UITheme::kControlHover;
    colors[ImGuiCol_ScrollbarGrabHovered] = UITheme::kControlHeld;
    colors[ImGuiCol_ScrollbarGrabActive]  = UITheme::kAccentDim;

    colors[ImGuiCol_Tab]                  = UITheme::kControl;
    colors[ImGuiCol_TabHovered]           = UITheme::kControlHover;
    colors[ImGuiCol_ResizeGrip]           = UITheme::kBorder;
    colors[ImGuiCol_ResizeGripHovered]    = UITheme::kAccentDim;
    colors[ImGuiCol_ResizeGripActive]     = UITheme::kAccent;

    colors[ImGuiCol_PlotHistogram]        = UITheme::kAccent;
    colors[ImGuiCol_PlotHistogramHovered] = UITheme::kWarning;
}

// A centred section label with a hairline rule either side -- the menus' one
// piece of structure, so groups of buttons stop reading as an undifferentiated
// stack. Width is the content region, so it tracks whatever panel it is in.
static void UISectionLabel(const char* label) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float width = ImGui::GetContentRegionAvail().x;
    const float startX = ImGui::GetCursorScreenPos().x;
    const ImVec2 textSize = ImGui::CalcTextSize(label);
    const float y = ImGui::GetCursorScreenPos().y + textSize.y * 0.5f;
    const float sideWidth =
        (std::max)(0.0f, (width - textSize.x) * 0.5f - 10.0f);
    const ImU32 rule = ImGui::GetColorU32(UITheme::kBorder);
    if (sideWidth > 4.0f) {
        draw->AddLine(ImVec2(startX, y),
                      ImVec2(startX + sideWidth, y), rule, 1.0f);
        draw->AddLine(ImVec2(startX + width - sideWidth, y),
                      ImVec2(startX + width, y), rule, 1.0f);
    }
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (width - textSize.x) * 0.5f);
    ImGui::TextColored(UITheme::kTextDim, "%s", label);
}

// Full-width button that keeps the menus on one rhythm instead of every call
// site repeating a hand-computed SetCursorPosX and a magic width.
static bool UIMenuButton(const char* label, float height = 46.0f) {
    return ImGui::Button(label, ImVec2(ImGui::GetContentRegionAvail().x, height));
}

// As above, but drawn in the accent colour for the one primary action on a
// screen. Exactly one per panel -- if everything is emphasised, nothing is.
static bool UIPrimaryButton(const char* label, float height = 52.0f) {
    ImGui::PushStyleColor(ImGuiCol_Button, UITheme::kAccentDim);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, UITheme::kAccent);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, UITheme::kAccent);
    const bool pressed = UIMenuButton(label, height);
    ImGui::PopStyleColor(3);
    return pressed;
}

// BF3-style menu entry: no button chrome, just the label. The selected row
// inverts -- a light bar with dark text -- which is what carries the highlight
// on a photographic background where a dark plate would disappear against the
// dark half of the image and glow against the bright half.
//
// Hover is the selection: these menus are a single column with no keyboard
// focus of their own, so whatever the mouse is over is what Enter would take.
static bool UIMenuRow(const char* label, float height = 34.0f) {
    const float width = ImGui::GetContentRegionAvail().x;
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    // InvisibleButton takes the input so the row stays one hit target the full
    // width of the column, rather than only where the glyphs happen to fall.
    const bool pressed = ImGui::InvisibleButton(label, ImVec2(width, height));
    const bool hovered = ImGui::IsItemHovered();

    ImDrawList* draw = ImGui::GetWindowDrawList();
    if (hovered) {
        draw->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + height),
                            IM_COL32(228, 234, 230, 232));
    }
    // Trim the label at the first '#' so ImGui's ##id suffix stays out of the
    // drawn text while still giving each row a unique id.
    const char* labelEnd = label;
    while (*labelEnd && !(labelEnd[0] == '#' && labelEnd[1] == '#')) ++labelEnd;
    const ImVec2 textSize = ImGui::CalcTextSize(label, labelEnd);
    draw->AddText(ImVec2(origin.x + 14.0f,
                         origin.y + (height - textSize.y) * 0.5f),
                  hovered ? IM_COL32(12, 16, 14, 255)
                          : IM_COL32(226, 232, 228, 236),
                  label, labelEnd);
    return pressed;
}

// ---- Settings screen -------------------------------------------------------
//
// A full-screen page rather than a column swapped into the menu body: once
// there are more than a handful of options, one long scrolling column buries
// most of them below the fold. Category tabs across the top, label-left /
// control-right rows, and a description panel for whatever the cursor is over
// -- the layout players already know from every shooter's options page.

// From Multiplayer.h, included after this file. The pause screen's copy of the
// page has to warn that the world keeps running behind it.
static bool MultiplayerActive();

// The menu's typefaces, rasterized at the sizes they are actually drawn at.
// Both stay null when no font file is found, and every use falls back to the
// built-in font -- the menu is then exactly what it was before. Declared here
// rather than beside LoadMenuFonts because the settings header uses them.
static ImFont* g_menuTitleFont = nullptr;
static ImFont* g_menuBodyFont = nullptr;

namespace SettingsUI {
constexpr float kRowHeight = 56.0f;
constexpr float kRowGap = 6.0f;
constexpr int kTabCount = 5;
static const char* const kTabs[kTabCount] = {
    "CONTROLS", "GAMEPLAY", "AUDIO", "VIDEO", "INTERFACE",
};
static const char* const kTabBlurbs[kTabCount] = {
    "Mouse feel and look direction.",
    "How aiming and the weapon behave.",
    "Master volume and the individual mix buses.",
    "Window mode, frame pacing and field of view.",
    "Crosshair and loading screen detail.",
};

// Persists across opens so BACK and back in lands on the same page.
static int s_tab = 0;
// Whatever row the cursor is over this frame. Reset at the top of the page and
// read by the description panel, which is drawn after the list.
static const char* s_hoverTitle = nullptr;
static const char* s_hoverDesc = nullptr;
static char s_hoverDefault[64] = {};

struct Row {
    ImVec2 min;
    float width;
    bool hovered;
};

// Draws a row's plate and label, records it as the described row when
// hovered, and leaves the cursor where the control goes (right half).
// Colours go through GetColorU32 so a BeginDisabled around the row dims the
// label and plate along with the control.
static Row BeginRow(const char* label, const char* description,
                    bool indent = false) {
    ImGui::PushID(label);
    Row row;
    row.min = ImGui::GetCursorScreenPos();
    row.width = ImGui::GetContentRegionAvail().x;
    const ImVec2 max(row.min.x + row.width, row.min.y + kRowHeight);
    row.hovered = ImGui::IsWindowHovered(
                      ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
                  ImGui::IsMouseHoveringRect(row.min, max);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(row.min, max,
                        ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f,
                                                  row.hovered ? 0.085f : 0.03f)),
                        4.0f);
    if (row.hovered) {
        draw->AddRectFilled(row.min, ImVec2(row.min.x + 3.0f, max.y),
                            ImGui::GetColorU32(UITheme::kAccent));
        s_hoverTitle = label;
        s_hoverDesc = description;
        s_hoverDefault[0] = '\0';
    }

    const float textX = row.min.x + (indent ? 40.0f : 20.0f);
    const float textY = row.min.y + (kRowHeight - ImGui::GetFontSize()) * 0.5f;
    if (indent) {
        // A short tick in front of a bus shows it hangs off the row above
        // without a tree control's chrome.
        draw->AddLine(ImVec2(row.min.x + 22.0f, row.min.y + kRowHeight * 0.5f),
                      ImVec2(row.min.x + 32.0f, row.min.y + kRowHeight * 0.5f),
                      ImGui::GetColorU32(UITheme::kTextDim), 1.0f);
    }
    draw->AddText(ImVec2(textX, textY),
                  ImGui::GetColorU32(row.hovered ? ImVec4(1, 1, 1, 1)
                                                 : UITheme::kText),
                  label);

    ImGui::SetCursorScreenPos(ImVec2(
        row.min.x + row.width * 0.5f,
        row.min.y + (kRowHeight - ImGui::GetFrameHeight()) * 0.5f));
    return row;
}

static float ControlWidth(const Row& row) { return row.width * 0.5f - 20.0f; }

// Moves the cursor under the row. The Dummy is what tells the child window
// the content reaches this far, so the list scrolls to the last row.
static void EndRow(const Row& row) {
    ImGui::SetCursorScreenPos(
        ImVec2(row.min.x, row.min.y + kRowHeight + kRowGap));
    ImGui::Dummy(ImVec2(row.width, 0.0f));
    ImGui::PopID();
}

// Pill switch in place of a checkbox. A 13 px tick box reads as a debug
// panel; a switch with ON/OFF beside it reads at a glance from across a room.
static bool Toggle(bool* value) {
    const ImVec2 size(52.0f, 26.0f);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool pressed = ImGui::InvisibleButton("##toggle", size);
    if (pressed) *value = !*value;
    const bool hovered = ImGui::IsItemHovered();

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float radius = size.y * 0.5f;
    const ImVec4 track = *value
        ? (hovered ? UITheme::kAccent : UITheme::kAccentDim)
        : ImVec4(1.0f, 1.0f, 1.0f, hovered ? 0.22f : 0.14f);
    draw->AddRectFilled(p, ImVec2(p.x + size.x, p.y + size.y),
                        ImGui::GetColorU32(track), radius);
    const float knobX = *value ? p.x + size.x - radius : p.x + radius;
    draw->AddCircleFilled(ImVec2(knobX, p.y + radius), radius - 4.0f,
                          ImGui::GetColorU32(ImVec4(1, 1, 1, 1)), 24);

    const char* state = *value ? "ON" : "OFF";
    const ImVec2 stateSize = ImGui::CalcTextSize(state);
    draw->AddText(ImVec2(p.x - stateSize.x - 14.0f,
                         p.y + (size.y - stateSize.y) * 0.5f),
                  ImGui::GetColorU32(*value ? UITheme::kText
                                            : UITheme::kTextDim),
                  state);
    return pressed;
}

// Switch row, right-aligned in the row. Returns true on the frame it flips.
static bool ToggleRow(const char* label, const char* description,
                      bool* value) {
    const Row row = BeginRow(label, description);
    ImGui::SetCursorScreenPos(ImVec2(row.min.x + row.width - 20.0f - 52.0f,
                                     row.min.y + (kRowHeight - 26.0f) * 0.5f));
    const bool changed = Toggle(value);
    EndRow(row);
    return changed;
}

struct SliderResult {
    bool changed;   // value moved this frame -- apply live
    bool released;  // drag finished with an edit -- clamp and save
};

static SliderResult SliderRow(const char* label, const char* description,
                              const char* id, float* value, float minValue,
                              float maxValue, const char* format,
                              ImGuiSliderFlags flags = 0,
                              const char* defaultFormat = nullptr,
                              float defaultValue = 0.0f, bool indent = false) {
    const Row row = BeginRow(label, description, indent);
    if (row.hovered && defaultFormat)
        std::snprintf(s_hoverDefault, sizeof(s_hoverDefault), defaultFormat,
                      defaultValue);
    ImGui::SetNextItemWidth(ControlWidth(row));
    // Accent fill up to the value, drawn on the channel under the slider so
    // the frame's own text stays on top. A bare grab on an empty track makes
    // the level hard to read at a glance; a filled bar does not.
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->ChannelsSplit(2);
    draw->ChannelsSetCurrent(1);
    SliderResult result;
    result.changed =
        ImGui::SliderFloat(id, value, minValue, maxValue, format, flags);
    result.released = ImGui::IsItemDeactivatedAfterEdit();
    const ImVec2 itemMin = ImGui::GetItemRectMin();
    const ImVec2 itemMax = ImGui::GetItemRectMax();
    float fraction = (flags & ImGuiSliderFlags_Logarithmic) && minValue > 0.0f
        ? std::log(*value / minValue) / std::log(maxValue / minValue)
        : (*value - minValue) / (maxValue - minValue);
    fraction = (std::max)(0.0f, (std::min)(1.0f, fraction));
    draw->ChannelsSetCurrent(0);
    if (fraction > 0.0f)
        draw->AddRectFilled(itemMin,
                            ImVec2(itemMin.x + (itemMax.x - itemMin.x) * fraction,
                                   itemMax.y),
                            ImGui::GetColorU32(ImVec4(UITheme::kAccent.x,
                                                      UITheme::kAccent.y,
                                                      UITheme::kAccent.z,
                                                      0.30f)),
                            4.0f);
    draw->ChannelsMerge();
    EndRow(row);
    return result;
}

// "<  VALUE  >" picker for a setting with named alternatives, where a switch
// would leave one of the two options nameless.
static bool SelectorRow(const char* label, const char* description,
                        const char* valueText) {
    const Row row = BeginRow(label, description);
    const float width = ControlWidth(row);
    const float h = ImGui::GetFrameHeight();
    bool pressed = false;

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
    if (ImGui::ArrowButton("##prev", ImGuiDir_Left)) pressed = true;
    ImGui::SameLine();
    const ImVec2 mid = ImGui::GetCursorScreenPos();
    const float midWidth = (std::max)(0.0f, width - h * 2.0f);
    if (ImGui::InvisibleButton("##value", ImVec2(midWidth, h))) pressed = true;
    ImGui::SameLine();
    if (ImGui::ArrowButton("##next", ImGuiDir_Right)) pressed = true;
    ImGui::PopStyleVar();

    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(mid, ImVec2(mid.x + midWidth, mid.y + h),
                        ImGui::GetColorU32(ImGuiCol_FrameBg));
    const ImVec2 textSize = ImGui::CalcTextSize(valueText);
    draw->AddText(ImVec2(mid.x + (midWidth - textSize.x) * 0.5f,
                         mid.y + (h - textSize.y) * 0.5f),
                  ImGui::GetColorU32(ImVec4(1, 1, 1, 1)), valueText);
    EndRow(row);
    return pressed;
}

// Small keycap for the footer and tab strip hints.
static float KeyCap(ImDrawList* draw, ImVec2 pos, const char* key,
                    float height) {
    const ImVec2 textSize = ImGui::CalcTextSize(key);
    const float width = (std::max)(height, textSize.x + 16.0f);
    draw->AddRect(pos, ImVec2(pos.x + width, pos.y + height),
                  IM_COL32(255, 255, 255, 90), 4.0f, 0, 1.0f);
    draw->AddText(ImVec2(pos.x + (width - textSize.x) * 0.5f,
                         pos.y + (height - textSize.y) * 0.5f),
                  IM_COL32(255, 255, 255, 190), key);
    return width;
}

// Hand-tracked wordmark, the same treatment as MILBOX and PAUSED.
static void Wordmark(ImVec2 pos, const char* text) {
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float tracking = ImGui::GetFontSize() * 0.14f;
    float x = pos.x;
    for (const char* c = text; *c; ++c) {
        const char glyph[2] = { *c, '\0' };
        draw->AddText(ImVec2(x, pos.y), IM_COL32(255, 255, 255, 255), glyph);
        x += ImGui::CalcTextSize(glyph).x + tracking;
    }
}

static void ControlsTab() {
    // Logarithmic: the useful range is bunched at the low end, where a linear
    // slider would put every usable value in its first fifth and make fine
    // adjustment impossible. Applied live because a sensitivity can only be
    // judged while it moves.
    const SliderResult sensitivity = SliderRow(
        "Mouse Sensitivity", "Lower is slower and steadier.",
        "##sensitivity", &g_settings.mouseSensitivity,
        GameSettings::kMinSensitivity, GameSettings::kMaxSensitivity, "%.3f",
        ImGuiSliderFlags_Logarithmic, "Default  %.2f",
        GameSettings::kDefaultSensitivity);
    if (sensitivity.changed) ApplyGameSettings();
    if (sensitivity.released) {
        g_settings.Clamp();
        ApplyGameSettings();
        SaveGameSettings(g_settings);
    }

    if (ToggleRow("Invert Mouse Y", "Down becomes up, up becomes down.",
                  &g_settings.invertMouseY)) {
        ApplyGameSettings();
        SaveGameSettings(g_settings);
    }
}

static void GameplayTab() {
    if (SelectorRow("Aiming Model",
                    g_settings.realisticAiming
                        ? "Realistic: the gun leads and the camera follows. "
                          "A fast turn swings your body."
                        : "Classic: camera and muzzle are one line. Where you "
                          "look is where you shoot.",
                    g_settings.realisticAiming ? "REALISTIC" : "CLASSIC")) {
        g_settings.realisticAiming = !g_settings.realisticAiming;
        ApplyGameSettings();
        SaveGameSettings(g_settings);
    }

    if (ToggleRow("See Through Weapon When Aiming",
                  "Approximates what your off eye sees past the gun.",
                  &g_settings.seeThroughWeaponWhenAiming)) {
        ApplyGameSettings();
        SaveGameSettings(g_settings);
    }

    // Disabled rather than hidden -- a row that vanishes makes the switch
    // above it look like it did nothing.
    ImGui::BeginDisabled(!g_settings.seeThroughWeaponWhenAiming);
    const SliderResult amount = SliderRow(
        "See-Through Amount", "Higher clears more of the weapon.",
        "##seethrough", &g_settings.seeThroughWeaponStrength,
        GameSettings::kMinSeeThroughStrength,
        GameSettings::kMaxSeeThroughStrength, "%.2f", 0, "Default  %.2f",
        GameSettings::kDefaultSeeThroughStrength, true);
    if (amount.changed) ApplyGameSettings();
    if (amount.released) {
        g_settings.Clamp();
        ApplyGameSettings();
        SaveGameSettings(g_settings);
    }
    ImGui::EndDisabled();
}

static void AudioTab() {
    // Master first and the buses indented under it, because that is the shape
    // of the submix graph: master trims everything including the reverb tail,
    // while the buses are independent of it and of each other.
    struct VolumeRow {
        const char* label;
        const char* id;
        const char* description;
        float* value;
        AudioBus bus;
        bool master;
    };
    const VolumeRow rows[] = {
        { "Master Volume", "##volmaster",
          "Trims everything, reverb tail included.",
          &g_settings.masterVolume, AudioBus::Weapons, true },
        { "Weapons", "##volweapons", "Gunfire, reloads and explosions.",
          &g_settings.weaponsVolume, AudioBus::Weapons, false },
        { "Voices", "##volvoices", "Callouts and dialogue.",
          &g_settings.voicesVolume, AudioBus::Voices, false },
        { "Ambience", "##volambience", "Wind, water and the world around you.",
          &g_settings.ambienceVolume, AudioBus::Ambience, false },
        { "Music", "##volmusic", "Menu and mission music.",
          &g_settings.musicVolume, AudioBus::Music, false },
        { "Interface", "##volui", "Menu clicks and HUD cues.",
          &g_settings.uiVolume, AudioBus::UI, false },
    };
    for (const VolumeRow& row : rows) {
        // Shown 0-100 rather than 0.00-1.00: a volume is the one control
        // players already read as a percentage. The slider edits a scaled
        // copy because ImGui formats whatever value it is given.
        float percent = *row.value * 100.0f;
        const SliderResult result = SliderRow(
            row.label, row.description, row.id, &percent, 0.0f, 100.0f,
            "%.0f%%", ImGuiSliderFlags_AlwaysClamp, nullptr, 0.0f,
            !row.master);
        if (result.changed) {
            *row.value = percent / 100.0f;
            if (row.master) AudioDevice::SetMasterVolume(*row.value);
            else            AudioDevice::SetBusVolume(row.bus, *row.value);
        }
        if (result.released) {
            g_settings.Clamp();
            ApplyGameSettings();
            SaveGameSettings(g_settings);
        }
    }
}

static void VideoTab() {
    if (ToggleRow("Fullscreen (Borderless)",
                  "On: borderless fullscreen. Off: windowed.",
                  &g_settings.fullscreen)) {
        // Only records the choice. ToggleFullscreen lives in
        // WindowAndGeometry.h, included after this file; the main loop moves
        // the window to whatever g_settings.fullscreen says each frame.
        SaveGameSettings(g_settings);
    }

    if (ToggleRow("VSync", "Locks frame rate to display refresh.",
                  &g_settings.vsync)) {
        ApplyGameSettings();
        SaveGameSettings(g_settings);
    }

    const SliderResult fov = SliderRow(
        "Field of View", "Hip-fire vertical field of view.", "##fov",
        &g_settings.fieldOfView, GameSettings::kMinFieldOfView,
        GameSettings::kMaxFieldOfView, "%.0f degrees", 0, "Default  %.0f",
        GameSettings::kDefaultFieldOfView);
    if (fov.changed) ApplyGameSettings();
    if (fov.released) {
        g_settings.Clamp();
        ApplyGameSettings();
        SaveGameSettings(g_settings);
    }

    const SliderResult bob = SliderRow(
        "Camera Bob", "Head bob while walking and running. Zero is off.",
        "##camerabob", &g_settings.cameraBob, GameSettings::kMinCameraBob,
        GameSettings::kMaxCameraBob, "%.2fx", 0, "Default  %.2fx",
        GameSettings::kDefaultCameraBob);
    if (bob.changed) ApplyGameSettings();
    if (bob.released) {
        g_settings.Clamp();
        ApplyGameSettings();
        SaveGameSettings(g_settings);
    }
}

static void InterfaceTab() {
    if (ToggleRow("Show Crosshair",
                  "Off leaves the weapon's own sights as the only aiming "
                  "reference. Optics are unaffected.",
                  &g_settings.showCrosshair)) {
        ApplyGameSettings();
        SaveGameSettings(g_settings);
    }

    if (ToggleRow("Detailed Loading Screen",
                  "Off shows just LOADING. On shows stage timings, uploads "
                  "and GPU state.",
                  &g_settings.debugLoadingScreen)) {
        // No ApplyGameSettings: the loading screen reads the flag directly
        // every frame it draws, so there is nothing to push anywhere.
        SaveGameSettings(g_settings);
    }
}
} // namespace SettingsUI

// Shared by the deployment rack and the in-world counter. The weapon ID owns
// the value, so swapping loadout slots never swaps the player's preference.
static void DrawWeaponCameraShakeSlider(int weapon) {
    if (weapon < 0 ||
        weapon >= static_cast<int>(g_settings.weaponCameraShake.size())) return;
    if (weapon == GunModel::kLaserWeapon ||
        weapon == GunModel::kFlamethrowerWeapon ||
        weapon == GunModel::kRemoteChargeWeapon ||
        weapon == GunModel::kTargetDesignatorWeapon) {
        ImGui::TextDisabled("This tool has no firing camera shake.");
        return;
    }
    ImGui::PushID(weapon);
    ImGui::TextColored(UITheme::kTextDim, "FIRING CAMERA SHAKE  /  %s",
                       GunModel::WeaponName(weapon));
    float percent = g_settings.weaponCameraShake[static_cast<size_t>(weapon)] *
                    100.0f;
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::SliderFloat("##camera_shake", &percent, 0.0f, 3000.0f,
                           "%.0f%%", ImGuiSliderFlags_AlwaysClamp)) {
        g_settings.weaponCameraShake[static_cast<size_t>(weapon)] =
            percent / 100.0f;
        ApplyGameSettings();
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        g_settings.Clamp();
        ApplyGameSettings();
        SaveGameSettings(g_settings);
    }
    ImGui::TextDisabled("Visual shot kick only; aim recoil is unchanged.");
    ImGui::PopID();
}

// Every control writes the INI on release rather than on every frame the value
// changes: dragging a slider produces a value per frame, and rewriting the file
// at that rate would be pointless disk churn for a value the player has not
// settled on yet.
//
// `openFlag` is the caller's own "settings are showing" state, cleared by
// BACK. Passed in rather than hardcoded because two screens open this page --
// the main menu and the pause screen -- and each has to close its own flag;
// sharing one would leave the main menu displaying settings because a paused
// player happened to open them. ESC closes it too, in WindowInput.h.
static void RenderSettingsMenu(bool& openFlag = g_showSettingsMenu) {
    using namespace SettingsUI;
    const ImVec2 display = ImGui::GetIO().DisplaySize;

    // Near-opaque: this page is the whole screen's subject. The pause copy
    // leaves a trace of the frame behind so it still reads as "in a mission".
    ImGui::GetBackgroundDrawList()->AddRectFilledMultiColor(
        ImVec2(0, 0), display,
        IM_COL32(6, 10, 9, 236), IM_COL32(12, 20, 18, 236),
        IM_COL32(4, 7, 6, 248), IM_COL32(3, 5, 5, 248));

    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowSize(display, ImGuiCond_Always);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
        ImGuiWindowFlags_NoSavedSettings;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##SettingsScreen", nullptr, flags);
    ImGui::PopStyleVar(3);
    ImGui::SetWindowFontScale(g_menuBodyFont ? 1.0f : 1.15f);
    ImDrawList* draw = ImGui::GetWindowDrawList();

    // Content is capped at 1280 and centred, so an ultrawide does not stretch
    // a label a metre away from its control.
    const float side = (std::max)(32.0f, (display.x - 1280.0f) * 0.5f);
    const float contentWidth = display.x - side * 2.0f;
    const float right = side + contentWidth;
    const float top = (std::max)(24.0f, display.y * 0.05f);

    s_hoverTitle = nullptr;
    s_hoverDesc = nullptr;
    s_hoverDefault[0] = '\0';

    // ---- Header -----------------------------------------------------------
    if (g_menuTitleFont) ImGui::PushFont(g_menuTitleFont);
    else ImGui::SetWindowFontScale(4.0f);
    const float titleHeight = ImGui::GetFontSize();
    Wordmark(ImVec2(side, top), "SETTINGS");
    if (g_menuTitleFont) ImGui::PopFont();
    ImGui::SetWindowFontScale(g_menuBodyFont ? 1.0f : 1.15f);

    // The pause screen's warning, carried over: multiplayer does not stop for
    // this page either, and a player tweaking audio needs to know that.
    if (&openFlag == &g_showPauseSettings && MultiplayerActive()) {
        const char* warning = "MULTIPLAYER  -  GAME STILL LIVE";
        const ImVec2 size = ImGui::CalcTextSize(warning);
        draw->AddText(ImVec2(right - size.x,
                             top + (titleHeight - size.y) * 0.5f),
                      ImGui::GetColorU32(UITheme::kWarning), warning);
    }

    const float ruleY = top + titleHeight + 14.0f;
    draw->AddRectFilled(ImVec2(side, ruleY), ImVec2(right, ruleY + 1.0f),
                        IM_COL32(255, 255, 255, 50));

    // ---- Tabs -------------------------------------------------------------
    if (!ImGui::GetIO().WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_Q, false))
            s_tab = (s_tab + kTabCount - 1) % kTabCount;
        if (ImGui::IsKeyPressed(ImGuiKey_E, false))
            s_tab = (s_tab + 1) % kTabCount;
    }

    const float tabsY = ruleY + 14.0f;
    const float tabHeight = 44.0f;
    const float capHeight = 26.0f;
    float tabX = side;
    tabX += KeyCap(draw, ImVec2(tabX, tabsY + (tabHeight - capHeight) * 0.5f),
                   "Q", capHeight) + 12.0f;
    for (int i = 0; i < kTabCount; ++i) {
        const ImVec2 textSize = ImGui::CalcTextSize(kTabs[i]);
        const ImVec2 tabMin(tabX, tabsY);
        const ImVec2 tabMax(tabX + textSize.x + 40.0f, tabsY + tabHeight);
        ImGui::SetCursorScreenPos(tabMin);
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##tab",
                                   ImVec2(tabMax.x - tabMin.x, tabHeight)))
            s_tab = i;
        const bool hovered = ImGui::IsItemHovered();
        ImGui::PopID();

        const bool selected = s_tab == i;
        if (hovered && !selected)
            draw->AddRectFilled(tabMin, tabMax, IM_COL32(255, 255, 255, 14),
                                4.0f);
        if (selected)
            draw->AddRectFilled(ImVec2(tabMin.x + 12.0f, tabMax.y - 3.0f),
                                ImVec2(tabMax.x - 12.0f, tabMax.y),
                                ImGui::GetColorU32(UITheme::kAccent));
        draw->AddText(ImVec2(tabMin.x + 20.0f,
                             tabMin.y + (tabHeight - textSize.y) * 0.5f),
                      selected  ? IM_COL32(255, 255, 255, 255)
                      : hovered ? IM_COL32(255, 255, 255, 200)
                                : IM_COL32(255, 255, 255, 120),
                      kTabs[i]);
        tabX = tabMax.x + 4.0f;
    }
    KeyCap(draw, ImVec2(tabX + 8.0f, tabsY + (tabHeight - capHeight) * 0.5f),
           "E", capHeight);
    draw->AddRectFilled(ImVec2(side, tabsY + tabHeight),
                        ImVec2(right, tabsY + tabHeight + 1.0f),
                        IM_COL32(255, 255, 255, 22));

    // ---- Body -------------------------------------------------------------
    const float footerHeight = 76.0f;
    const float footerTop = display.y - footerHeight;
    const float bodyTop = tabsY + tabHeight + 24.0f;
    const float bodyHeight = (std::max)(80.0f, footerTop - 16.0f - bodyTop);
    const float gap = 32.0f;
    const float listWidth = contentWidth * 0.62f;
    const float panelWidth = contentWidth - listWidth - gap;

    ImGui::SetCursorScreenPos(ImVec2(side, bodyTop));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(8.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 4.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10.0f, 8.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, 14.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_GrabRounding, 3.0f);
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(1.0f, 1.0f, 1.0f, 0.07f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgHovered,
                          ImVec4(1.0f, 1.0f, 1.0f, 0.11f));
    ImGui::PushStyleColor(ImGuiCol_FrameBgActive,
                          ImVec4(1.0f, 1.0f, 1.0f, 0.15f));
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(1.0f, 1.0f, 1.0f, 0.07f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          ImVec4(1.0f, 1.0f, 1.0f, 0.16f));
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, UITheme::kAccentDim);
    ImGui::BeginChild("##settingsList", ImVec2(listWidth, bodyHeight),
                      ImGuiChildFlags_None, ImGuiWindowFlags_NoBackground);
    switch (s_tab) {
    case 0: ControlsTab();  break;
    case 1: GameplayTab();  break;
    case 2: AudioTab();     break;
    case 3: VideoTab();     break;
    default: InterfaceTab(); break;
    }
    ImGui::EndChild();
    ImGui::PopStyleColor(6);
    ImGui::PopStyleVar(5);

    // Description panel: what the hovered row does, so the rows themselves
    // can stay one line tall.
    if (panelWidth > 160.0f) {
        ImGui::SetCursorScreenPos(ImVec2(side + listWidth + gap, bodyTop));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(1.0f, 1.0f, 1.0f, 0.035f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(1.0f, 1.0f, 1.0f, 0.10f));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24.0f, 22.0f));
        ImGui::BeginChild("##settingsInfo",
                          ImVec2(panelWidth, (std::min)(bodyHeight, 320.0f)),
                          ImGuiChildFlags_Borders,
                          ImGuiWindowFlags_NoScrollbar);
        ImGui::PushTextWrapPos(0.0f);
        const char* heading = s_hoverTitle ? s_hoverTitle : kTabs[s_tab];
        const char* body = s_hoverTitle ? s_hoverDesc : kTabBlurbs[s_tab];
        ImGui::TextColored(ImVec4(1, 1, 1, 1), "%s", heading);
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        {
            const ImVec2 at = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddRectFilled(
                at, ImVec2(at.x + 32.0f, at.y + 2.0f),
                ImGui::GetColorU32(UITheme::kAccent));
            ImGui::Dummy(ImVec2(32.0f, 2.0f));
        }
        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        if (body) ImGui::TextColored(UITheme::kTextDim, "%s", body);
        if (s_hoverDefault[0]) {
            ImGui::Dummy(ImVec2(0.0f, 10.0f));
            ImGui::TextColored(UITheme::kAccent, "%s", s_hoverDefault);
        }
        if (!s_hoverTitle) {
            ImGui::Dummy(ImVec2(0.0f, 10.0f));
            ImGui::TextColored(UITheme::kTextDim,
                               "Hover a setting for details.");
        }
        ImGui::PopTextWrapPos();
        ImGui::EndChild();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(2);
    }

    // ---- Footer -----------------------------------------------------------
    draw->AddRectFilled(ImVec2(side, footerTop), ImVec2(right, footerTop + 1.0f),
                        IM_COL32(255, 255, 255, 40));
    const float buttonHeight = 44.0f;
    const float buttonY = footerTop + (footerHeight - buttonHeight) * 0.5f;

    ImGui::SetCursorScreenPos(ImVec2(
        side, footerTop + (footerHeight - ImGui::GetTextLineHeight()) * 0.5f));
    ImGui::TextColored(UITheme::kTextDim, "Saved to %s", GameSettingsPath());

    const float backWidth = 160.0f;
    const float resetWidth = 220.0f;
    const float backX = right - backWidth;
    const float resetX = backX - 12.0f - resetWidth;

    // ESC hint left of the buttons. Saving already happened at the point of
    // change, so leaving is BACK, not a cancel that implies unsaved edits.
    {
        const char* hint = "BACK";
        const ImVec2 hintSize = ImGui::CalcTextSize(hint);
        const float hintX = resetX - 28.0f - hintSize.x;
        draw->AddText(ImVec2(hintX, buttonY + (buttonHeight - hintSize.y) * 0.5f),
                      IM_COL32(255, 255, 255, 150), hint);
        const float capWidth = ImGui::CalcTextSize("ESC").x + 16.0f;
        KeyCap(draw, ImVec2(hintX - 10.0f - capWidth,
                            buttonY + (buttonHeight - capHeight) * 0.5f),
               "ESC", capHeight);
    }

    ImGui::SetCursorScreenPos(ImVec2(resetX, buttonY));
    if (ImGui::Button("RESET TO DEFAULTS", ImVec2(resetWidth, buttonHeight))) {
        g_settings.ResetToDefaults();
        ApplyGameSettings();
        SaveGameSettings(g_settings);
    }
    ImGui::SetCursorScreenPos(ImVec2(backX, buttonY));
    ImGui::PushStyleColor(ImGuiCol_Button, UITheme::kAccentDim);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, UITheme::kAccent);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, UITheme::kAccent);
    if (ImGui::Button("BACK", ImVec2(backWidth, buttonHeight))) {
        SaveGameSettings(g_settings);
        openFlag = false;
    }
    ImGui::PopStyleColor(3);

    ImGui::End();
}

// Defined in Multiplayer.h, which is included after this file because it needs
// g_bandits and the camera. The menu only needs to be able to call it.
static void ShutdownMultiplayer();

// Also from Multiplayer.h. The pause screen has to know whether time is shared
// with other machines, because that decides both whether it may stop the world
// and whether it has to warn the player that it cannot.
static bool MultiplayerActive();

// Multiplayer panel. Drawn in place of the menu body exactly like the settings
// panel above, for the same reason: one column of controls reads better as the
// whole screen than as a popup floating over a list it has nothing to do with.
//
// Both sides can connect from here, before either player has loaded a level --
// the session is polled every frame from any screen, so a handshake completes
// while both players are still sitting on this panel.
static void RenderMultiplayerMenu() {
    const net::Role role = g_netSession.CurrentRole();
    const bool offline = role == net::Role::Offline;

    UISectionLabel("SESSION");
    ImGui::Dummy(ImVec2(0.0f, 6.0f));

    // Status first: it is the only part of the panel that matters once a
    // session is live, and it is what the player is waiting on while joining.
    if (!offline) {
        if (role == net::Role::Host && g_netSession.OverSteam()) {
            // No port: the session is reached by SteamID, and that id is the
            // only thing a friend can act on. It is shown rather than merely
            // alluded to because "Join Game" on a profile launches whatever
            // app the AppID names -- and while this build borrows 480, that
            // reaches only a friend who already has the game running. Pasting
            // the address into ADDRESS is the path that always works.
            ImGui::TextColored(UITheme::kAccent, "Hosting on Steam");
            const std::string invite = g_netSession.ConnectAddress();
            if (invite.empty()) {
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextColored(UITheme::kWarning,
                                   "Steam did not report an account id, so "
                                   "there is no address to hand out.");
                ImGui::PopTextWrapPos();
            } else {
                // ImGui edits through the buffer even when it is read-only,
                // so the session's string is copied rather than exposed.
                char shown[64] = {};
                std::snprintf(shown, sizeof(shown), "%s", invite.c_str());
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                ImGui::InputText("##mpinvite", shown, sizeof(shown),
                                 ImGuiInputTextFlags_ReadOnly);
                if (ImGui::SmallButton("COPY INVITE"))
                    ImGui::SetClipboardText(invite.c_str());
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextColored(UITheme::kTextDim,
                                   "Send this to a friend to paste into "
                                   "ADDRESS. Join Game on your Steam profile "
                                   "only reaches them while they already have "
                                   "the game running.");
                ImGui::PopTextWrapPos();
            }
        } else if (role == net::Role::Host) {
            ImGui::TextColored(UITheme::kAccent, "Hosting on port %s",
                               g_multiplayerPort);
        } else if (g_netSession.LocalId() == net::kInvalidPlayerId) {
            // The client is connected at the socket level but has not been
            // given an id yet, so it is mid-handshake rather than in.
            //
            // Asked of the session rather than read off the menu's own field:
            // a join started from the command line never touched that buffer,
            // and showing its default made the panel name the wrong host.
            const std::string target = g_netSession.ConnectAddress();
            if (g_netSession.OverSteam())
                ImGui::TextColored(UITheme::kTextDim, "Connecting to %s...",
                                   target.c_str());
            else
                ImGui::TextColored(UITheme::kTextDim, "Connecting to %s:%d...",
                                   target.c_str(),
                                   static_cast<int>(g_netSession.Port()));
        } else {
            ImGui::TextColored(UITheme::kAccent, "Connected as player %d",
                               static_cast<int>(g_netSession.LocalId()) + 1);
        }

        // Who is actually in. A host used to see only "Hosting on port 27015"
        // and had no way to tell a friend had arrived short of starting a level
        // and looking for them, so the wait was indistinguishable from a
        // handshake that never completed.
        //
        // Skipped mid-handshake: a client with no id yet holds no slots, and
        // reporting "1 player" there would name the wrong session.
        if (g_netSession.LocalId() != net::kInvalidPlayerId) {
            const int count = static_cast<int>(g_netSession.PlayerCount());
            ImGui::Dummy(ImVec2(0.0f, 10.0f));
            ImGui::TextColored(count > 1 ? UITheme::kAccent : UITheme::kTextDim,
                               "%d player%s connected", count,
                               count == 1 ? "" : "s");
            for (net::PlayerId id = 0; id < net::kMaxPlayers; ++id) {
                if (!g_netSession.PlayerActive(id)) continue;
                const bool self = id == g_netSession.LocalId();
                ImGui::TextColored(self ? UITheme::kTextDim : UITheme::kAccent,
                                   "   PLAYER-%d%s",
                                   static_cast<int>(id) + 1,
                                   self ? "  (you)" : "");
            }
            if (count == 1)
                ImGui::TextColored(UITheme::kTextDim,
                                   "   waiting for someone to join...");
        }
    } else if (!g_multiplayerStatusError.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s",
                           g_multiplayerStatusError.c_str());
        ImGui::PopTextWrapPos();
    } else {
        ImGui::TextColored(UITheme::kTextDim, "Offline");
    }

    ImGui::Dummy(ImVec2(0.0f, 18.0f));

    // The port is shared by both halves: you host on it or you join to it, and
    // having two fields that must agree is one more thing to get wrong.
    ImGui::TextColored(UITheme::kTextDim, "PORT");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    ImGui::BeginDisabled(!offline);
    ImGui::InputText("##mpport", g_multiplayerPort, sizeof(g_multiplayerPort),
                     ImGuiInputTextFlags_CharsDecimal);

    ImGui::Dummy(ImVec2(0.0f, 12.0f));
    ImGui::TextColored(UITheme::kTextDim, "ADDRESS");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    ImGui::InputTextWithHint("##mpaddress", "127.0.0.1",
                             g_multiplayerJoinAddress,
                             sizeof(g_multiplayerJoinAddress));
    ImGui::TextColored(UITheme::kTextDim,
                       "The host's address. 127.0.0.1 is this machine, and "
                       "steam:<id> joins over Steam.");
    ImGui::EndDisabled();

    // atoi rather than a parse with error reporting: the field is digits-only
    // and an empty one falling back to the default port is the right answer.
    const int parsedPort = std::atoi(g_multiplayerPort);
    const uint16_t port = (parsedPort > 0 && parsedPort <= 65535)
                              ? static_cast<uint16_t>(parsedPort)
                              : uint16_t{27015};

    ImGui::Dummy(ImVec2(0.0f, 18.0f));

    if (offline) {
        // Steam first when it is there: it is the only one of the two that a
        // friend outside this network can actually reach, and it needs nothing
        // typed. The IP button stays for LAN and for a machine with no Steam.
        const bool steamReady = net::SteamTransportAvailable();
        if (steamReady) {
            if (UIPrimaryButton("HOST ON STEAM", 44.0f)) {
                g_multiplayerStatusError.clear();
                std::string error;
                if (!g_netSession.StartHost(port, &error, /*overSteam=*/true))
                    g_multiplayerStatusError = "Could not host: " + error;
            }
            ImGui::Dummy(ImVec2(0.0f, 8.0f));
        }
        if (steamReady ? UIMenuButton("HOST ON THIS NETWORK", 44.0f)
                       : UIPrimaryButton("HOST", 44.0f)) {
            g_multiplayerStatusError.clear();
            std::string error;
            if (!g_netSession.StartHost(port, &error))
                g_multiplayerStatusError = "Could not host: " + error;
        }
        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        if (UIMenuButton("JOIN", 44.0f)) {
            g_multiplayerStatusError.clear();
            std::string error;
            if (!g_netSession.StartClient(g_multiplayerJoinAddress, port,
                                          &error))
                g_multiplayerStatusError = "Could not join: " + error;
        }
    } else {
        if (UIMenuButton("DISCONNECT", 44.0f)) {
            ShutdownMultiplayer();
            g_multiplayerStatusError.clear();
        }
    }

    ImGui::Dummy(ImVec2(0.0f, 18.0f));
    if (UIPrimaryButton("BACK", 44.0f))
        g_showMultiplayerMenu = false;

    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    // The level is the host's to pick now: a client is told which map to load
    // and follows, on join and on every change after it.
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(UITheme::kTextDim,
                       "Stay connected while the host starts a level. Everyone "
                       "else loads the same map automatically.");
    ImGui::PopTextWrapPos();
}

// Resolved UI images, keyed by path. Uploading a texture costs a
// descriptor slot out of the ImGui heap, so each image is loaded once and the
// result -- including the failure -- is cached: a missing PNG must not retry
// its file open every frame the screen is up.
struct UIImage {
    Microsoft::WRL::ComPtr<ID3D12Resource> texture;
    std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> uploads;
    UINT descriptorSlot = ~0u;
    bool attempted = false;
};
static std::unordered_map<std::string, UIImage> g_uiImages;

// Returns an ImGui texture handle for a UI image, or 0 when there is no art.
// Zero is a normal answer, not an error -- a travel card draws a placeholder
// for it, and the menu simply keeps its drawn backdrop.
static uint64_t UITextureFromFile(const char* imagePath) {
    if (!imagePath || !imguiSrvHeap || !g_dx12.device) return 0;
    UIImage& entry = g_uiImages[imagePath];
    if (!entry.attempted) {
        entry.attempted = true;
        std::error_code error;
        if (std::filesystem::exists(imagePath, error) &&
            g_nextImGuiTextureSlot < kImGuiDescriptorCount) {
            entry.texture = GLBImporter::LoadTextureSingleMip(imagePath,
                g_dx12.device, g_dx12.commandList, entry.uploads);
            if (entry.texture) {
                entry.descriptorSlot = g_nextImGuiTextureSlot++;
                const UINT stride =
                    g_dx12.device->GetDescriptorHandleIncrementSize(
                        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                D3D12_CPU_DESCRIPTOR_HANDLE cpu =
                    imguiSrvHeap->GetCPUDescriptorHandleForHeapStart();
                cpu.ptr += static_cast<SIZE_T>(entry.descriptorSlot) * stride;
                D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
                srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                srv.Shader4ComponentMapping =
                    D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                srv.Texture2D.MipLevels = 1;
                g_dx12.device->CreateShaderResourceView(
                    entry.texture.Get(), &srv, cpu);
            }
        }
    }
    if (entry.descriptorSlot == ~0u) return 0;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu =
        imguiSrvHeap->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += static_cast<UINT64>(entry.descriptorSlot) *
        g_dx12.device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return gpu.ptr;
}

// Rasterizing at the display size is the whole point: the wordmark used to be
// the 13px built-in bitmap blown up 5.2x, which is why its edges were soft and
// its stems uneven. A glyph baked at ~68px has real curves at that size.
static void LoadMenuFonts() {
    ImGuiIO& io = ImGui::GetIO();
    // Monospace faces, in preference order, matching the terminal look the
    // menu already had. Cascadia ships with Windows Terminal and VS; Consolas
    // is on every Windows install, so the last entry effectively always hits.
    const char* candidates[] = {
        "C:/Windows/Fonts/CascadiaMono.ttf",
        "C:/Windows/Fonts/CascadiaCode.ttf",
        "C:/Windows/Fonts/consola.ttf",
    };
    const char* path = nullptr;
    for (const char* candidate : candidates) {
        if (std::filesystem::exists(candidate)) { path = candidate; break; }
    }
    if (!path) return;

    // Oversampled horizontally: these are thin, wide-tracked letterforms, and
    // the extra horizontal samples are what keep the vertical stems from
    // picking up the ragged edges visible before.
    ImFontConfig cfg;
    cfg.OversampleH = 3;
    cfg.OversampleV = 2;
    cfg.PixelSnapH = false;

    // Body first so it becomes the default font for every other ImGui window;
    // the previous default was the 13px built-in, so this sharpens the rows,
    // the editor and the HUD panels alike.
    g_menuBodyFont = io.Fonts->AddFontFromFileTTF(path, 18.0f, &cfg);
    g_menuTitleFont = io.Fonts->AddFontFromFileTTF(path, 68.0f, &cfg);
    if (g_menuBodyFont) io.FontDefault = g_menuBodyFont;
}
