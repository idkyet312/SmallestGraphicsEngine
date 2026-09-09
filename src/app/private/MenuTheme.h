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

// Settings panel. Drawn instead of the menu body rather than as a popup over
// it, so the one column of controls stays the whole screen's subject.
//
// Every control writes the INI on release rather than on every frame the value
// changes: dragging a slider produces a value per frame, and rewriting the file
// at that rate would be pointless disk churn for a value the player has not
// settled on yet.
static void RenderSettingsMenu() {
    UISectionLabel("MOUSE");
    ImGui::Dummy(ImVec2(0.0f, 6.0f));

    ImGui::TextColored(UITheme::kTextDim, "SENSITIVITY");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    // Logarithmic: the useful range is bunched at the low end, where a linear
    // slider would put every usable value in its first fifth and make fine
    // adjustment impossible.
    if (ImGui::SliderFloat("##sensitivity", &g_settings.mouseSensitivity,
                           GameSettings::kMinSensitivity,
                           GameSettings::kMaxSensitivity,
                           "%.3f", ImGuiSliderFlags_Logarithmic)) {
        // Apply live so the player can feel the change while dragging, which
        // is the only way to judge a sensitivity.
        ApplyGameSettings();
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        g_settings.Clamp();
        ApplyGameSettings();
        SaveGameSettings(g_settings);
    }
    ImGui::TextColored(UITheme::kTextDim,
                       "Lower is slower and steadier. Default %.2f.",
                       GameSettings::kDefaultSensitivity);

    ImGui::Dummy(ImVec2(0.0f, 18.0f));
    UISectionLabel("WEAPON");
    ImGui::Dummy(ImVec2(0.0f, 6.0f));

    if (ImGui::Checkbox("See through weapon when aiming",
                        &g_settings.seeThroughWeaponWhenAiming)) {
        ApplyGameSettings();
        SaveGameSettings(g_settings);
    }
    ImGui::TextColored(UITheme::kTextDim,
                       "Approximates what your off eye sees past the gun.");

    // The slider only means anything once the effect is on, so it is disabled
    // rather than hidden -- a control that vanishes makes the checkbox above it
    // look like it did nothing.
    ImGui::BeginDisabled(!g_settings.seeThroughWeaponWhenAiming);
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    ImGui::TextColored(UITheme::kTextDim, "AMOUNT");
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if (ImGui::SliderFloat("##seethrough",
                           &g_settings.seeThroughWeaponStrength,
                           GameSettings::kMinSeeThroughStrength,
                           GameSettings::kMaxSeeThroughStrength, "%.2f")) {
        // Live, like the sensitivity above: this is a look, and the only way to
        // judge it is to aim with it.
        ApplyGameSettings();
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        g_settings.Clamp();
        ApplyGameSettings();
        SaveGameSettings(g_settings);
    }
    ImGui::TextColored(UITheme::kTextDim,
                       "Higher clears more of the weapon. Default %.2f.",
                       GameSettings::kDefaultSeeThroughStrength);
    ImGui::EndDisabled();

    ImGui::Dummy(ImVec2(0.0f, 18.0f));
    UISectionLabel("DEBUG");
    ImGui::Dummy(ImVec2(0.0f, 6.0f));

    if (ImGui::Checkbox("Detailed loading screen",
                        &g_settings.debugLoadingScreen)) {
        // No ApplyGameSettings: the loading screen reads the flag directly
        // every frame it draws, so there is nothing to push anywhere.
        SaveGameSettings(g_settings);
    }
    ImGui::TextColored(UITheme::kTextDim,
                       "Off shows just LOADING. On shows stage timings, "
                       "uploads and GPU state.");

    ImGui::Dummy(ImVec2(0.0f, 16.0f));
    if (UIMenuButton("RESET TO DEFAULTS", 38.0f)) {
        g_settings.ResetToDefaults();
        ApplyGameSettings();
        SaveGameSettings(g_settings);
    }

    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    // Saving already happened at the point of change, so backing out is not a
    // "cancel" -- say BACK rather than implying unsaved edits are being kept.
    if (UIPrimaryButton("BACK", 44.0f)) {
        SaveGameSettings(g_settings);
        g_showSettingsMenu = false;
    }
    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    ImGui::TextColored(UITheme::kTextDim, "Saved to %s", GameSettingsPath());
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

// The menu's typefaces, rasterized at the sizes they are actually drawn at.
// Both stay null when no font file is found, and every use falls back to the
// built-in font -- the menu is then exactly what it was before.
static ImFont* g_menuTitleFont = nullptr;
static ImFont* g_menuBodyFont = nullptr;

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
