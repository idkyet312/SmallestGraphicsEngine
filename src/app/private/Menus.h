#pragma once

// Private application implementation; included once by main.cpp in dependency order.

// Bottom-right progress bar while a cold boot's shaders compile, the way games
// present a first-launch shader compile. Foreground list, so it sits above
// the menu column and the settings page alike.
static void RenderShaderCompileProgress() {
    if (!g_shadersCompiling) return;
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    const float clamped =
        (std::min)(1.0f, (std::max)(0.0f, g_shaderCompileProgress));
    const float margin = 36.0f;
    const char* caption = "FIRST TIME COMPILING SHADERS - MAY TAKE A MINUTE";
    const ImVec2 captionSize = ImGui::CalcTextSize(caption);
    // Never narrower than the caption plus room for the percentage.
    const float barWidth = (std::max)(captionSize.x + 64.0f,
                                      (std::min)(display.x * 0.4f, 460.0f));
    const float barHeight = 4.0f;
    const float barLeft = display.x - margin - barWidth;
    const float barTop = display.y - margin - barHeight;
    draw->AddText(ImVec2(barLeft, barTop - captionSize.y - 8.0f),
                  IM_COL32(226, 232, 228, 236), caption);
    char percent[16];
    std::snprintf(percent, sizeof(percent), "%d%%",
                  static_cast<int>(clamped * 100.0f));
    const ImVec2 percentSize = ImGui::CalcTextSize(percent);
    draw->AddText(ImVec2(barLeft + barWidth - percentSize.x,
                         barTop - percentSize.y - 8.0f),
                  IM_COL32(226, 232, 228, 180), percent);
    draw->AddRectFilled(ImVec2(barLeft, barTop),
                        ImVec2(barLeft + barWidth, barTop + barHeight),
                        IM_COL32(255, 255, 255, 38));
    if (clamped > 0.0f)
        draw->AddRectFilled(ImVec2(barLeft, barTop),
                            ImVec2(barLeft + barWidth * clamped,
                                   barTop + barHeight),
                            IM_COL32(236, 240, 236, 255));
}

// Shared by the menu and both loading screens so entering the game keeps
// the same backdrop and column shading throughout startup and level loads.
static void RenderFrontEndBackdrop(ImVec2 display, float columnOffset,
                                   const char* imagePath = nullptr) {
    ImDrawList* background = ImGui::GetBackgroundDrawList();
    background->AddRectFilledMultiColor(ImVec2(0, 0), display,
        IM_COL32(10, 18, 21, 255), IM_COL32(25, 40, 40, 255),
        IM_COL32(8, 15, 18, 255), IM_COL32(7, 12, 15, 255));

    // Optional photographic backdrop. Missing art is not a failure: the drawn
    // contours below are the fallback, so the menu still has a background on a
    // build that ships without the image.
    const char* backdropPath = imagePath && *imagePath
        ? imagePath : "Content/Textures/UI/menu_background.jpg";
    uint64_t menuImage = UITextureFromFile(backdropPath);
    if (!menuImage && imagePath && *imagePath) {
        backdropPath = "Content/Textures/UI/menu_background.jpg";
        menuImage = UITextureFromFile(backdropPath);
    }
    if (menuImage) {
        // Contain, not cover: scale by whichever axis runs out first, so the
        // whole photo is on screen whatever the window shape -- nothing of the
        // island gets cropped away. Placement is handled below.
        // Read the aspect off the texture rather than hardcoding it, so
        // swapping the file for one of a different shape needs no code change.
        const D3D12_RESOURCE_DESC desc =
            g_uiImages[backdropPath].texture->GetDesc();
        const float imageAspect = static_cast<float>(desc.Width) /
            (std::max)(1.0f, static_cast<float>(desc.Height));
        const float screenAspect = display.x / (std::max)(display.y, 1.0f);
        ImVec2 size = screenAspect > imageAspect
            ? ImVec2(display.y * imageAspect, display.y)
            : ImVec2(display.x, display.x / imageAspect);
        // Centred. The menu column is what moves against it, not the photo.
        const ImVec2 origin((display.x - size.x) * 0.5f,
                            (display.y - size.y) * 0.5f);

        // Fill whatever the contained image leaves over with the image itself
        // rather than the gradient: a stretched, darkened copy behind it covers
        // the bars edge to edge, so the screen reads as one photograph with a
        // sharp centre instead of art floating on a plate.
        const ImVec2 fillSize = screenAspect > imageAspect
            ? ImVec2(display.x, display.x / imageAspect)
            : ImVec2(display.y * imageAspect, display.y);
        // Centred on the same point as the sharp copy, so the two line up and
        // the filler reads as an extension of the photo rather than a second,
        // offset picture.
        const ImVec2 fillOrigin((display.x - fillSize.x) * 0.5f,
                                (display.y - fillSize.y) * 0.5f);
        background->AddImage((ImTextureID)menuImage, fillOrigin,
                             ImVec2(fillOrigin.x + fillSize.x,
                                    fillOrigin.y + fillSize.y),
                             ImVec2(0, 0), ImVec2(1, 1),
                             IM_COL32(70, 78, 80, 255));
        // Sits under the sharp copy, so the seam between them is a step in
        // brightness on continuous content, not a hard edge against a panel.
        background->AddImage((ImTextureID)menuImage, origin,
                             ImVec2(origin.x + size.x, origin.y + size.y));
    }

    // Contours give the front end an identity without loading a game scene.
    // Skipped when the photo loaded -- they were the backdrop, not an overlay.
    if (!menuImage) {
        const ImVec2 center(display.x * 0.76f, display.y * 0.48f);
        const float radius = (std::min)(display.x * 0.32f, display.y * 0.48f);
        for (float x = 0; x < display.x; x += 64.0f)
            background->AddLine(ImVec2(x, 0), ImVec2(x, display.y), IM_COL32(130, 170, 150, 10));
        for (float y = 0; y < display.y; y += 64.0f)
            background->AddLine(ImVec2(0, y), ImVec2(display.x, y), IM_COL32(130, 170, 150, 10));
        for (int ring = 0; ring < 15; ++ring) {
            for (int point = 0; point <= 128; ++point) {
                const float angle = point * 6.2831853f / 128.0f;
                const float r = radius * (0.22f + ring * 0.058f) *
                    (1.0f + 0.10f * std::sin(angle * 3 + ring * 0.16f) +
                     0.06f * std::cos(angle * 7 - ring * 0.12f));
                background->PathLineTo(ImVec2(center.x + std::cos(angle) * r,
                                             center.y + std::sin(angle) * r * 0.78f));
            }
            background->PathStroke(IM_COL32(123, 166, 149, 32), 0, 1.0f);
        }
        background->AddCircle(center, radius * 0.55f, IM_COL32(165, 193, 147, 50), 96);
        background->AddLine(ImVec2(center.x - 18, center.y), ImVec2(center.x + 18, center.y), IM_COL32(187, 211, 164, 100));
        background->AddLine(ImVec2(center.x, center.y - 18), ImVec2(center.x, center.y + 18), IM_COL32(187, 211, 164, 100));
    }

    // Left-to-right scrim under the menu column. The rows are unplated, so this
    // is what keeps them readable over whatever the left third of the backdrop
    // happens to be -- which is the whole reason a photo can be dropped in
    // behind them. Fades out well before the column ends so it reads as shading
    // on the art rather than as a panel with an edge.
    // Offset with the column so its solid end stays under the text and the
    // fade always begins past it, rather than the column sliding out from
    // under the only thing keeping it readable.
    const float scrimWidth = (std::min)(display.x * 0.62f, 900.0f);
    const float scrimStart = columnOffset;
    background->AddRectFilledMultiColor(
        ImVec2(scrimStart, 0), ImVec2(scrimStart + scrimWidth, display.y),
        IM_COL32(0, 0, 0, 205), IM_COL32(0, 0, 0, 0),
        IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 205));  // UL, UR, LR, LL
    // The offset leaves the scrim starting inside the screen, so hold its
    // solid tone across the strip to its left; without it the edge of the
    // shading reads as a visible vertical seam.
    if (scrimStart > 0.0f)
        background->AddRectFilled(ImVec2(0, 0), ImVec2(scrimStart, display.y),
                                  IM_COL32(0, 0, 0, 205));
    // A vignette top and bottom, the other half of the film look.
    background->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(display.x, display.y * 0.16f),
        IM_COL32(0, 0, 0, 150), IM_COL32(0, 0, 0, 150),
        IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0));
    background->AddRectFilledMultiColor(ImVec2(0, display.y * 0.82f), ImVec2(display.x, display.y),
        IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0),
        IM_COL32(0, 0, 0, 170), IM_COL32(0, 0, 0, 170));
}

static void RenderMilboxWordmark(float progress, const char* imagePath = nullptr,
                                 const char* destination = nullptr,
                                 const char* subtitle = nullptr);

static void RenderMainMenu(HWND hwnd) {
    // Capture the shared loading layout after boot, when normal frame capture
    // is available; real loading stages can be too short to inspect reliably.
    static const std::string loadingCapturePath = [] {
        char path[MAX_PATH] = {};
        GetEnvironmentVariableA("SGE_LOADING_CAPTURE_PATH", path, sizeof(path));
        return std::string(path);
    }();
    if (!loadingCapturePath.empty()) {
        static int frames = 0;
        char value[32] = {};
        const float progress = GetEnvironmentVariableA(
            "SGE_LOADING_CAPTURE_PROGRESS", value, sizeof(value)) > 0
            ? std::strtof(value, nullptr) : 0.65f;
        static const std::string levelPath = [] {
            char level[MAX_PATH] = {};
            GetEnvironmentVariableA("SGE_LOADING_CAPTURE_LEVEL", level, sizeof(level));
            if (*level) {
                g_loadingLevelName = std::filesystem::path(level).stem().string();
                SetLoadingLevelPresentation(level);
            }
            return std::string(level);
        }();
        RenderMilboxWordmark(progress, g_loadingLevelImagePath.c_str(),
            levelPath.empty() ? nullptr : g_loadingLevelName.c_str(),
            g_loadingLevelSubtitle.c_str());
        if (++frames == 4) g_frameCapturePath = loadingCapturePath;
        else if (frames > 4 && g_frameCapturePath.empty()) PostQuitMessage(0);
        return;
    }
    RenderShaderCompileProgress();
    // A load started from the menu takes the whole screen. The loading screen
    // paints to the background draw list, and every ImGui window renders above
    // that list, so drawing the menu first left the wordmark showing through
    // behind a live menu column -- the rows still lit up under the cursor
    // while the level was loading. Nothing else in the menu may run either:
    // the buttons below would happily start a second load on top of the one
    // already in flight.
    if (g_game.loading.Active()) {
        RenderLoadingScreen();
        return;
    }
    // Test hook: SGE_AUTO_SETTINGS=<tab> boots straight into that settings
    // page, so the layout can be captured without driving the mouse.
    static bool autoSettingsChecked = false;
    if (!autoSettingsChecked) {
        autoSettingsChecked = true;
        char tab[8] = {};
        if (GetEnvironmentVariableA("SGE_AUTO_SETTINGS", tab, sizeof(tab)) > 0) {
            SettingsUI::s_tab = (std::max)(0, (std::min)(
                std::atoi(tab), SettingsUI::kTabCount - 1));
            g_showSettingsMenu = true;
        }
    }
    // Settings are their own full-screen page, drawn instead of the menu.
    if (g_showSettingsMenu) {
        RenderSettingsMenu(g_showSettingsMenu);
        return;
    }

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    // The menu column's geometry, needed up here because the backdrop is
    // positioned against it -- the photo starts where the text ends.
    const float kMenuBaseMargin =
        (std::max)(24.0f, (std::min)(display.x * 0.115f, 200.0f));
    const float kMenuWidth = (std::min)(460.0f, display.x - kMenuBaseMargin * 2);
    // Nudged right of the base margin so the column clears the dark left edge
    // of the backdrop. Scales with width, and is clamped so a narrow window
    // cannot push the column off the right.
    const float kMenuMargin = (std::max)(0.0f, (std::min)(
        kMenuBaseMargin + display.x * 0.019f, display.x - kMenuWidth));

    RenderFrontEndBackdrop(display, kMenuMargin - kMenuBaseMargin);
    // The menu sits in the left third over the art, with no panel behind it --
    // the background is the screen, and a plate floating on top of it would be
    // the thing the eye lands on instead of the image.
    const float menuHeight = (std::min)(860.0f, display.y - 32.0f);
    ImGui::SetNextWindowPos(ImVec2(kMenuMargin, (display.y - menuHeight) * 0.5f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(kMenuWidth, menuHeight), ImGuiCond_Always);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoScrollbar;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 24));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 2));
    ImGui::Begin("Main Menu", nullptr, flags);
    ImGui::PopStyleVar(3);
    // Scaling a baked font is what softened the text before, so the real fonts
    // are drawn at 1.0 and only the built-in fallback is scaled up.
    ImGui::SetWindowFontScale(g_menuBodyFont ? 1.0f : 1.3f);

    // Title block. Wordmark, then a hairline rule marking the left edge and
    // the width the rows below occupy.
    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    const float contentWidth = ImGui::GetContentRegionAvail().x;
    const char* title = "MILBOX";
    // Flat white wordmark, hard against the left margin. No centring: the
    // column is a left edge the title, the rows and the rule all share.
    if (g_menuTitleFont) ImGui::PushFont(g_menuTitleFont);
    else ImGui::SetWindowFontScale(5.2f);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 12.0f);
    // Letter-spaced by hand: a wordmark wants tracking the font itself does
    // not carry, and one glyph at a time is the only way ImGui offers it.
    {
        ImVec2 pen = ImGui::GetCursorScreenPos();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const float tracking = ImGui::GetFontSize() * 0.14f;
        float x = pen.x;
        for (const char* c = title; *c; ++c) {
            const char glyph[2] = { *c, '\0' };
            draw->AddText(ImVec2(x, pen.y), IM_COL32(255, 255, 255, 255), glyph);
            x += ImGui::CalcTextSize(glyph).x + tracking;
        }
        // Reserve the space the hand-drawn text occupies so the rule and rows
        // below lay out under it rather than on top of it.
        ImGui::Dummy(ImVec2(x - pen.x, ImGui::GetTextLineHeight()));
    }
    if (g_menuTitleFont) ImGui::PopFont();
    // Back to body scale for everything under the wordmark.
    ImGui::SetWindowFontScale(g_menuBodyFont ? 1.0f : 1.15f);

    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 cursor = ImGui::GetCursorScreenPos();
        // Runs the width of the column, under the wordmark, marking the left
        // edge the rows below line up on.
        const float ruleX = cursor.x + 14.0f;
        draw->AddRectFilled(ImVec2(ruleX, cursor.y),
                            ImVec2(cursor.x + contentWidth, cursor.y + 1.0f),
                            IM_COL32(255, 255, 255, 60));
    }
    ImGui::Dummy(ImVec2(0.0f, 14.0f));

    // Career balance, directly under the wordmark. Drawn above the settings
    // early-out below so the number stays on screen while settings are open --
    // it belongs to the frame of the menu rather than to the deploy list.
    {
        char balanceText[32];
        MoneySystem::Format(balanceText, sizeof(balanceText),
                            g_game.money.Balance());
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 14.0f);
        // The HUD accent green rather than a money amber, so the balance reads
        // as part of the same palette as everything else on the screen.
        ImGui::TextColored(UITheme::kAccent, "%s  ", balanceText);
        ImGui::SameLine(0.0f, 6.0f);
        ImGui::TextColored(ImVec4(0.60f, 0.65f, 0.62f, 1.0f), "FUNDS");
    }

    // Career rank on the line below, with the bar to the next level. Same
    // placement argument as the balance: it is part of the menu frame, so it
    // stays visible with settings open.
    {
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 14.0f);
        // White rather than the warning amber every other accent uses: the rank
        // reads with the bar and the XP line under it, and those are white.
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "%s",
                           g_game.rank.RankLabel());
        ImGui::SameLine(0.0f, 8.0f);
        ImGui::TextColored(ImVec4(0.60f, 0.65f, 0.62f, 1.0f), "LVL %d",
                           g_game.rank.Level());

        const int level = g_game.rank.Level();
        const float fraction =
            static_cast<float>(g_game.rank.XpIntoLevel()) /
            static_cast<float>(g_game.rank.XpForNextLevel());
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 14.0f);
        const ImVec2 barPos = ImGui::GetCursorScreenPos();
        constexpr float kMenuBarWidth = 188.0f;
        constexpr float kMenuBarHeight = 5.0f;
        ImDrawList* rankDraw = ImGui::GetWindowDrawList();
        rankDraw->AddRectFilled(barPos,
                                ImVec2(barPos.x + kMenuBarWidth,
                                       barPos.y + kMenuBarHeight),
                                IM_COL32(255, 255, 255, 34));
        rankDraw->AddRectFilled(barPos,
                                ImVec2(barPos.x + kMenuBarWidth * fraction,
                                       barPos.y + kMenuBarHeight),
                                IM_COL32(255, 255, 255, 220));
        ImGui::Dummy(ImVec2(kMenuBarWidth, kMenuBarHeight));

        // The remaining number, not the total: at the cap there is nothing left
        // to earn toward, so it says so rather than showing a full bar with a
        // meaningless "0 XP TO LVL 51" under it.
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 14.0f);
        if (level >= RankSystem::kMaxLevel) {
            ImGui::TextColored(ImVec4(0.60f, 0.65f, 0.62f, 1.0f),
                               "MAX RANK  -  %lld KILLS",
                               static_cast<long long>(g_game.rank.LifetimeKills()));
        } else {
            char remainingText[32];
            RankSystem::Format(remainingText, sizeof(remainingText),
                               g_game.rank.XpForNextLevel() -
                                   g_game.rank.XpIntoLevel());
            ImGui::TextColored(ImVec4(0.60f, 0.65f, 0.62f, 1.0f),
                               "%s XP TO LVL %d", remainingText, level + 1);
        }
    }

    ImGui::Dummy(ImVec2(0.0f, 14.0f));

    if (g_showMultiplayerMenu) {
        RenderMultiplayerMenu();
        ImGui::End();
        return;
    }

    // The base is the way in: every run now starts by walking out to the
    // helicopter and picking a destination there, so the island is reached
    // through the hub rather than from a menu button beside it. Custom Game
    // stays, because a hand-authored level has no travel card at the base.
    //
    // Island 1 is still reachable without a row of its own -- the travel board
    // flies there, the editor loads any level, and --level=<path> starts one
    // directly. The Training Range is back on the menu because it is practice
    // rather than a destination: walking out to the hub's helicopter to reach
    // a firing range is a detour through fiction nobody wants on the way to
    // trying a weapon out.
    // One unbroken column, in the order the player uses it. The section rules
    // are gone with the buttons: a divider every two entries was structure the
    // list is short enough not to need, and it fought the wordmark rule above.
    ImGui::SetWindowFontScale(1.7f);
    // Every row that enters the game waits for a cold boot's shader compile;
    // settings and quit do not need the renderer.
    const bool canPlay = !g_shadersCompiling;
    if (UIMenuRow("ENTER BASE", 34.0f, canPlay))
        StartBase(hwnd);
    if (UIMenuRow("TRAINING RANGE", 34.0f, canPlay))
        StartTrainingRange(hwnd);
    if (UIMenuRow("CUSTOM GAME", 34.0f, canPlay))
        BrowseAndStartCustomLevel(hwnd);
    if (UIMenuRow("LEVEL EDITOR", 34.0f, canPlay))
        ImGui::OpenPopup("Level Editor");
    // Empty terrain with no gameplay actors, foliage, houses or ocean clutter.
    // Renderer work is what is left, so a graphics change can be looked at
    // without a full island's content confusing what is being measured.
    if (UIMenuRow("TEST LEVEL", 34.0f, canPlay))
        StartLevelOne(hwnd, true, false, true, nullptr, true);
    // Above SETTINGS rather than below it: a session has to be joined before a
    // level is started, so it belongs with the rows that start a run, not with
    // the ones that configure the game.
    if (UIMenuRow("MULTIPLAYER", 34.0f, canPlay))
        g_showMultiplayerMenu = true;
    if (UIMenuRow("SETTINGS"))
        g_showSettingsMenu = true;
    if (UIMenuRow("QUIT"))
        PostQuitMessage(0);
    ImGui::SetWindowFontScale(g_menuBodyFont ? 1.0f : 1.15f);

    // Kept below the list rather than beside Custom Game: it reports a level
    // that failed to load, and the browser is the only row that can produce
    // one, but it needs the full column width to wrap in.
    if (!g_mainMenuLevelStatus.empty()) {
        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 14.0f);
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f), "%s",
                           g_mainMenuLevelStatus.c_str());
        ImGui::PopTextWrapPos();
    }

    // Ask before entering the editor rather than always opening the Level 1
    // template: loading was previously only reachable from inside the editor,
    // so opening an existing level meant first sitting through a full build of
    // a template you were about to discard.
    const ImVec2 editorPopupCenter(ImGui::GetIO().DisplaySize.x * 0.5f,
                                   ImGui::GetIO().DisplaySize.y * 0.5f);
    ImGui::SetNextWindowPos(editorPopupCenter, ImGuiCond_Appearing,
                            ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Level Editor", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Start a new level, or load an existing one?");
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        if (ImGui::Button("New", ImVec2(120.0f, 0.0f))) {
            ImGui::CloseCurrentPopup();
            StartLevelEditor(hwnd);
        }
        ImGui::SameLine();
        if (ImGui::Button("Load...", ImVec2(120.0f, 0.0f))) {
            // Close first: GetOpenFileNameW runs a nested modal message loop, so
            // the popup would otherwise stay painted behind the file dialog and
            // still be open when it returns.
            ImGui::CloseCurrentPopup();
            std::filesystem::path chosen;
            if (BrowseForLevelPath(hwnd, L"Open Level in Editor", chosen)) {
                g_mainMenuLevelStatus.clear();
                StartLevelEditor(hwnd, chosen);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel", ImVec2(120.0f, 0.0f)))
            ImGui::CloseCurrentPopup();
        if (!g_mainMenuLevelStatus.empty())
            ImGui::TextWrapped("%s", g_mainMenuLevelStatus.c_str());
        ImGui::EndPopup();
    }
    // Career reset, pinned to the bottom corner of the column. Deliberately not
    // a row in the list above: it undoes every mission ever flown, and a row
    // sitting between TEST LEVEL and QUIT is a row that gets hit by accident.
    // Two clicks for the same reason -- the first arms it, the second wipes.
    {
        static bool resetArmed = false;
        const float footerHeight = ImGui::GetFrameHeightWithSpacing();

        // Funds grant, on the footer row above the reset. It belongs down here
        // with the other career-wide action rather than in the destination
        // list, for the same reason the reset does: the list is for places to
        // go, and a row between TEST LEVEL and QUIT is a row hit by accident.
        constexpr int64_t kMenuFundsGrant = 500000;
        ImGui::SetCursorPosY(ImGui::GetWindowHeight() -
                             footerHeight * 2.0f - 10.0f);
        ImGui::SetCursorPosX(14.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              ImVec4(1.0f, 1.0f, 1.0f, 0.10f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                              ImVec4(1.0f, 1.0f, 1.0f, 0.16f));
        ImGui::PushStyleColor(ImGuiCol_Text, UITheme::kAccent);
        if (ImGui::Button("+$500,000")) {
            // SetBalance rather than a payout event: this is money that was
            // never earned, so it must not land in career earnings and inflate
            // the totals the extraction report and the rank screen read from.
            g_game.money.SetBalance(
                g_game.money.Balance() + kMenuFundsGrant,
                g_game.money.TotalEarned());
            // Written straight through, same as the reset beside it: a balance
            // the player can see has to survive killing the process.
            SaveCareer();
        }
        ImGui::PopStyleColor(4);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Adds %lld to funds. Career earnings unchanged.",
                              static_cast<long long>(kMenuFundsGrant));

        ImGui::SetCursorPosY(ImGui::GetWindowHeight() - footerHeight - 10.0f);
        ImGui::SetCursorPosX(14.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              ImVec4(1.0f, 1.0f, 1.0f, 0.10f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                              ImVec4(1.0f, 1.0f, 1.0f, 0.16f));
        ImGui::PushStyleColor(ImGuiCol_Text,
                              resetArmed ? ImVec4(1.0f, 0.45f, 0.35f, 1.0f)
                                         : ImVec4(0.52f, 0.56f, 0.54f, 1.0f));
        if (ImGui::Button(resetArmed ? "CONFIRM RESET" : "RESET PROGRESS")) {
            if (resetArmed) {
                g_game.money.ResetCareer();
                g_game.rank.ResetCareer();
                // Written straight through: a player who resets and then kills
                // the process from the taskbar must not find the old career
                // back on the next launch.
                SaveCareer();
                resetArmed = false;
            } else {
                resetArmed = true;
            }
        }
        ImGui::PopStyleColor(4);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Experience back to 0, funds back to %lld.",
                              static_cast<long long>(
                                  MoneySystem::kStartingBalance));
        if (resetArmed) {
            ImGui::SameLine(0.0f, 8.0f);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                  ImVec4(1.0f, 1.0f, 1.0f, 0.10f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                                  ImVec4(1.0f, 1.0f, 1.0f, 0.16f));
            ImGui::PushStyleColor(ImGuiCol_Text,
                                  ImVec4(0.52f, 0.56f, 0.54f, 1.0f));
            if (ImGui::Button("CANCEL")) resetArmed = false;
            ImGui::PopStyleColor(4);
        }
    }

    ImGui::End();
}

// The same tracked title and unplated column as the main menu. Keep the
// wordmark complete while progress moves below it, so even a short load has
// the game's identity and a long one has a readable destination.
static void RenderMilboxWordmark(float progress, const char* imagePath,
                                 const char* destination, const char* subtitle) {
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const float baseMargin =
        (std::max)(24.0f, (std::min)(display.x * 0.115f, 200.0f));
    const float columnWidth = (std::max)(1.0f,
        (std::min)(460.0f, display.x - baseMargin * 2.0f));
    const float margin = (std::max)(0.0f, (std::min)(
        baseMargin + display.x * 0.019f, display.x - columnWidth));
    RenderFrontEndBackdrop(display, margin - baseMargin, imagePath);
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    const float left = margin + 14.0f;
    const float width = (std::max)(1.0f,
        (std::min)(columnWidth - 28.0f, display.x - left - 24.0f));
    const float clamped = (std::min)(1.0f, (std::max)(0.0f, progress));

    ImFont* titleFont = g_menuTitleFont ? g_menuTitleFont : ImGui::GetFont();
    ImFont* bodyFont = g_menuBodyFont ? g_menuBodyFont : ImGui::GetFont();
    float titleSize = 68.0f;
    const float bodySize = 18.0f;
    const char* title = "MILBOX";
    float tracking = titleSize * 0.14f;
    float wordWidth = 0.0f;
    for (const char* c = title; *c; ++c) {
        const char glyph[2] = { *c, '\0' };
        wordWidth += titleFont->CalcTextSizeA(titleSize, 1e6f, 0.0f, glyph).x;
        if (c[1]) wordWidth += tracking;
    }
    // A narrow window should keep the whole title in the column.
    const float titleScale = (std::min)(1.0f, width / (std::max)(wordWidth, 1.0f));
    titleSize *= titleScale;
    tracking *= titleScale;
    const bool hasDestination = destination && *destination;
    const bool hasSubtitle = hasDestination && subtitle && *subtitle;
    const float destinationHeight = hasDestination
        ? bodyFont->CalcTextSizeA(bodySize, 1e6f, width, destination).y : 0.0f;
    const float subtitleHeight = hasSubtitle
        ? bodyFont->CalcTextSizeA(bodySize, 1e6f, width, subtitle).y : 0.0f;
    const float blockHeight = titleSize + 88.0f +
        (hasDestination ? destinationHeight + 24.0f : 0.0f) +
        (hasSubtitle ? subtitleHeight + 8.0f : 0.0f);
    const float top = (std::max)(24.0f, (display.y - blockHeight) * 0.5f);

    float x = left;
    for (const char* c = title; *c; ++c) {
        const char glyph[2] = { *c, '\0' };
        draw->AddText(titleFont, titleSize, ImVec2(x, top),
                      IM_COL32(255, 255, 255, 255), glyph);
        x += titleFont->CalcTextSizeA(titleSize, 1e6f, 0.0f, glyph).x + tracking;
    }
    float y = top + titleSize + 16.0f;
    draw->AddRectFilled(ImVec2(left, y), ImVec2(left + width, y + 1.0f),
                        IM_COL32(255, 255, 255, 60));
    y += 24.0f;
    if (hasDestination) {
        draw->AddText(bodyFont, bodySize, ImVec2(left, y),
            ImGui::GetColorU32(UITheme::kText), destination, nullptr, width);
        y += destinationHeight + 8.0f;
        if (hasSubtitle) {
            draw->AddText(bodyFont, bodySize, ImVec2(left, y),
                IM_COL32(166, 181, 169, 255), subtitle, nullptr, width);
            y += subtitleHeight + 8.0f;
        }
        y += 16.0f;
    }

    const char* caption = hasDestination ? "LOADING MAP" : "LOADING";
    draw->AddText(bodyFont, bodySize, ImVec2(left, y),
                  IM_COL32(166, 181, 169, 255), caption);
    // Keep a sign of life when one asset holds the progress bar still.
    const float captionWidth = bodyFont->CalcTextSizeA(bodySize, 1e6f, 0.0f, caption).x;
    const int activeDot = static_cast<int>(std::fmod(ImGui::GetTime() * 3.0, 3.0));
    for (int dot = 0; dot < 3; ++dot)
        draw->AddCircleFilled(ImVec2(left + captionWidth + 12.0f + dot * 7.0f,
                                     y + bodySize * 0.6f), 1.5f,
            dot == activeDot ? ImGui::GetColorU32(UITheme::kText)
                             : IM_COL32(166, 181, 169, 80));
    char percent[16];
    std::snprintf(percent, sizeof(percent), "%d%%",
                  static_cast<int>(clamped * 100.0f));
    const float percentWidth = bodyFont->CalcTextSizeA(bodySize, 1e6f, 0.0f, percent).x;
    draw->AddText(bodyFont, bodySize, ImVec2(left + width - percentWidth, y),
                  ImGui::GetColorU32(UITheme::kText), percent);
    y += bodySize + 14.0f;
    draw->AddRectFilled(ImVec2(left, y), ImVec2(left + width, y + 2.0f),
                        IM_COL32(255, 255, 255, 38));
    if (clamped > 0.0f)
        draw->AddRectFilled(ImVec2(left, y),
            ImVec2(left + width * clamped, y + 2.0f),
            ImGui::GetColorU32(UITheme::kAccent));
}

// The player-facing loading screen. Everything the debug screen shows --
// stage index, upload byte counts, D3D12 resource states -- is diagnostic, and
// a player waiting on a level has no use for any of it.
static void RenderPlainLoadingScreen() {
    RenderMilboxWordmark(g_game.loading.Progress(),
        g_loadingLevelImagePath.c_str(), g_loadingLevelName.c_str(),
        g_loadingLevelSubtitle.c_str());
}

static void RenderLoadingScreen() {
    if (g_pendingLoadingAction) {
        RenderMilboxWordmark(0.0f, g_loadingLevelImagePath.c_str(),
            g_loadingLevelName.c_str(), g_loadingLevelSubtitle.c_str());
        return;
    }
    if (!g_settings.debugLoadingScreen) {
        RenderPlainLoadingScreen();
        return;
    }
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::GetBackgroundDrawList()->AddRectFilled(
        ImVec2(0, 0), display, IM_COL32(4, 8, 12, 238));
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f),
        ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(680.0f, 450.0f), ImGuiCond_Always);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoInputs;
    ImGui::Begin("Loading Level", nullptr, flags);
    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    const char* title =
        g_game.session.Screen() == GameScreen::LevelEditor ? "LOADING LEVEL EDITOR" :
        (g_emptyLevelMode ? "LOADING EMPTY"
        : (g_stressTestMode ? "LOADING STRESS TEST" :
           (!g_activeCustomLevelName.empty() ? "LOADING CUSTOM LEVEL" : "LOADING LEVEL 1")));
    ImGui::SetCursorPosX((680.0f - ImGui::CalcTextSize(title).x) * 0.5f);
    ImGui::TextUnformatted(title);
    ImGui::Dummy(ImVec2(0.0f, 6.0f));

    char progressText[64];
    std::snprintf(progressText, sizeof(progressText), "Task %u / %u  (%.0f%%)",
        g_game.loading.TaskIndex(), g_game.loading.TaskCount(),
        g_game.loading.Progress() * 100.0f);
    ImGui::ProgressBar(g_game.loading.Progress(), ImVec2(-1.0f, 25.0f),
        progressText);
    ImGui::Text("Current: %s", g_game.loading.Label().c_str());
    ImGui::TextDisabled("Asset:   %s", g_game.loading.Asset().c_str());

    const double totalMs = g_game.loading.TotalElapsedMilliseconds();
    const double taskMs = g_game.loading.TaskElapsedMilliseconds();
    ImGui::Text("Elapsed: %.2f s total | %.2f s current task",
        totalMs / 1000.0, taskMs / 1000.0);

    const StaticBufferStatsDX12 uploads = GetStaticBufferStatsDX12();
    const uint64_t uploadBytes = uploads.bytes >= levelLoadingUploadBaseline.bytes
        ? uploads.bytes - levelLoadingUploadBaseline.bytes : 0;
    const uint32_t uploadResources =
        uploads.resources >= levelLoadingUploadBaseline.resources
        ? uploads.resources - levelLoadingUploadBaseline.resources : 0;
    ImGui::Text("GPU buffers: %u resources | %.2f MiB queued/created",
        uploadResources, static_cast<double>(uploadBytes) / (1024.0 * 1024.0));
    ImGui::Text("Uploads: %u submitted total | %u last frame | %u pending",
        g_game.loading.SubmittedUploads(),
        g_game.loading.LastSubmittedUploads(),
        uploads.pendingUploads);

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
    ImGui::Separator();
    ImGui::Text("DX12 resource #%llu: %s  %.2f KiB",
        static_cast<unsigned long long>(resource.serial), resource.label.c_str(),
        static_cast<double>(resource.bytes) / 1024.0);
    ImGui::TextDisabled(
        "CreateCommittedResource(DEFAULT, COPY_DEST) + UPLOAD(GENERIC_READ)");
    ImGui::TextDisabled(
        "COPY queue: CopyBufferRegion | DIRECT queue: final barrier/render");
    ImGui::TextDisabled("CopyBufferRegion -> ResourceBarrier(");
    ImGui::TextDisabled("  COPY_DEST -> %s [0x%X])", resourceState,
        static_cast<unsigned>(resource.finalState));

    const HRESULT gpuHealth = g_dx12.device
        ? g_dx12.device->GetDeviceRemovedReason() : E_POINTER;
    if (gpuHealth == S_OK)
        ImGui::TextColored(ImVec4(0.35f, 1.0f, 0.45f, 1.0f), "GPU: OK");
    else
        ImGui::TextColored(ImVec4(1.0f, 0.25f, 0.2f, 1.0f),
            "GPU: REMOVED 0x%08lX", static_cast<unsigned long>(gpuHealth));

    ImGui::Separator();
    ImGui::TextUnformatted("Recent tasks");
    const auto& loadRecords = g_game.loading.Records();
    if (loadRecords.empty()) {
        ImGui::TextDisabled("Waiting for first task...");
    } else {
        for (auto it = loadRecords.rbegin(); it != loadRecords.rend(); ++it) {
            ImGui::TextColored(it->succeeded
                ? ImVec4(0.55f, 0.95f, 0.6f, 1.0f)
                : ImVec4(1.0f, 0.45f, 0.3f, 1.0f),
                "%s  %s  %.1f ms", it->succeeded ? "OK" : "FAIL",
                it->label.c_str(), it->milliseconds);
        }
    }
    ImGui::End();
}

// Drawn over the live scene while the local player is downed in a session.
//
// Deliberately does NOT touch cameraLocked or release the cursor the way
// RenderDeathScreen does: cameraLocked short-circuits ProcessInput, which would
// take mouse-look away from a player whose only remaining activity is looking
// around for the teammate coming to pick them up.
static void RenderDownedOverlay() {
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    // Much lighter than the death screen's wash: the player still needs to read
    // the world through it to see who is coming.
    draw->AddRectFilled(ImVec2(0, 0), display, IM_COL32(70, 0, 0, 90));

    ImDrawList* front = ImGui::GetForegroundDrawList();
    const char* title = "YOU ARE DOWN";
    const ImVec2 titleSize = ImGui::CalcTextSize(title);
    const float centreX = display.x * 0.5f;
    const float titleY = display.y * 0.34f;
    front->AddText(ImVec2(centreX - titleSize.x * 0.5f, titleY),
                   IM_COL32(255, 78, 62, 245), title);

    const bool beingRevived = scene.player.reviveProgress > 0.0f;
    const char* subtitle = beingRevived ? "HOLD ON" : "WAIT FOR A TEAMMATE";
    const ImVec2 subtitleSize = ImGui::CalcTextSize(subtitle);
    front->AddText(ImVec2(centreX - subtitleSize.x * 0.5f, titleY + 22.0f),
                   IM_COL32(226, 232, 228, 210), subtitle);

    // The progress bar only appears once someone is actually working on you,
    // so an empty bar never sits there implying a revive that is not happening.
    if (!beingRevived) return;
    constexpr float kBarWidth = 220.0f;
    constexpr float kBarHeight = 6.0f;
    const float barX = centreX - kBarWidth * 0.5f;
    const float barY = titleY + 48.0f;
    front->AddRectFilled(ImVec2(barX, barY),
                         ImVec2(barX + kBarWidth, barY + kBarHeight),
                         IM_COL32(18, 22, 20, 200), 2.0f);
    front->AddRectFilled(
        ImVec2(barX, barY),
        ImVec2(barX + kBarWidth * scene.player.reviveProgress,
               barY + kBarHeight),
        IM_COL32(120, 230, 150, 240), 2.0f);
}

struct LastDeployment {
    bool valid = false;
    std::string levelName;
    std::string levelFile;
    net::LevelKind levelKind = net::LevelKind::None;
    MissionLoadout loadout{};
    std::array<SGE::WeaponInstance, MissionLoadout::kWeaponCount> weapons{};
    uint32_t ownedWeapons = 0;
    uint32_t ownedGrenades = 0;
    uint32_t ownedGear = 0;
    std::unordered_set<std::string> ownedAttachments;
    XMFLOAT3 target{};
    int marines = 0;
    LevelInsertionMode insertion = LevelInsertionMode::Helicopter;
    InsertionAirframe airframe = InsertionAirframe::NewBlackHawk;
    bool leftSeat = false;
    TimeOfDay timeOfDay = TimeOfDay::Afternoon;
    WeatherState weather = WeatherState::Cloudy;
    float enemyDamageMultiplier = 1.0f;
};

static LastDeployment g_lastDeployment;

static bool CanReplayLastDeployment() {
    return g_lastDeployment.valid &&
        g_lastDeployment.levelName == g_activeCustomLevelName &&
        g_lastDeployment.levelFile == g_activeLevelFile &&
        g_lastDeployment.levelKind == g_activeLevelKind;
}

static void RestartWithPlan(HWND hwnd, net::RestartPlanMode requestedPlan) {
    if (g_deferLoadingActions) {
        QueueLoadingAction([=] { RestartWithPlan(hwnd, requestedPlan); },
                           g_loadingLevelName);
        return;
    }
    const net::RestartPlanMode plan = CanReplayLastDeployment()
        ? requestedPlan : net::RestartPlanMode::None;
    if (MultiplayerActive()) {
        if (g_netSession.CurrentRole() != net::Role::Host) return;
        g_netSession.RestartHostLevel(plan);
    }
    g_deploymentRestartPending = plan;
    RestartActiveLevel(hwnd);
}

static void RenderDeathScreen(HWND hwnd) {
    if (!deathCursorReleased) {
        // First frame of this death: the latch is re-armed per level start,
        // so it counts each death once.
        if (!MultiplayerActive()) ++g_singlePlayerScore.deaths;
        cameraLocked = true;
        ReleaseCapture();
        SetCursorVisible(true);
        deathCursorReleased = true;
    }
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    // Keep the scene visible through the pause menu's scrim so this screen
    // belongs to the same menu system, even over a bright sky.
    ImDrawList* backdrop = ImGui::GetBackgroundDrawList();
    backdrop->AddRectFilledMultiColor(ImVec2(0, 0), display,
        IM_COL32(8, 14, 16, 214), IM_COL32(20, 32, 32, 214),
        IM_COL32(6, 12, 14, 226), IM_COL32(5, 9, 12, 226));
    backdrop->AddRectFilledMultiColor(
        ImVec2(0, 0), ImVec2(display.x, display.y * 0.16f),
        IM_COL32(0, 0, 0, 150), IM_COL32(0, 0, 0, 150),
        IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0));
    backdrop->AddRectFilledMultiColor(
        ImVec2(0, display.y * 0.82f), ImVec2(display.x, display.y),
        IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0),
        IM_COL32(0, 0, 0, 170), IM_COL32(0, 0, 0, 170));

    const float width = (std::min)(460.0f, display.x - 48.0f);
    const float height = (std::min)(360.0f, display.y - 48.0f);
    ImGui::SetNextWindowPos(ImVec2((display.x - width) * 0.5f,
                                   (display.y - height) * 0.5f),
                            ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 24));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 2));
    ImGui::Begin("Death Screen", nullptr, flags);
    ImGui::PopStyleVar(3);
    ImGui::SetWindowFontScale(g_menuBodyFont ? 1.0f : 1.3f);

    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    const float deathWidth = ImGui::GetContentRegionAvail().x;
    if (g_menuTitleFont) ImGui::PushFont(g_menuTitleFont);
    else ImGui::SetWindowFontScale(4.4f);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 12.0f);
    {
        const char* title = "YOU DIED";
        const ImVec2 pen = ImGui::GetCursorScreenPos();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const float tracking = ImGui::GetFontSize() * 0.14f;
        float x = pen.x;
        for (const char* c = title; *c; ++c) {
            const char glyph[2] = { *c, '\0' };
            draw->AddText(ImVec2(x, pen.y), IM_COL32(255, 255, 255, 255), glyph);
            x += ImGui::CalcTextSize(glyph).x + tracking;
        }
        ImGui::Dummy(ImVec2(x - pen.x, ImGui::GetTextLineHeight()));
    }
    if (g_menuTitleFont) ImGui::PopFont();
    ImGui::SetWindowFontScale(g_menuBodyFont ? 1.0f : 1.15f);

    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    {
        ImDrawList* deathDraw = ImGui::GetWindowDrawList();
        const ImVec2 cursor = ImGui::GetCursorScreenPos();
        deathDraw->AddRectFilled(ImVec2(cursor.x + 14.0f, cursor.y),
                                 ImVec2(cursor.x + deathWidth, cursor.y + 1.0f),
                                 IM_COL32(255, 255, 255, 60));
    }
    ImGui::Dummy(ImVec2(0.0f, 14.0f));
    ImGui::SetWindowFontScale(1.7f);
    if (!MultiplayerActive() ||
        g_netSession.CurrentRole() == net::Role::Host) {
        if (UIMenuRow("QUICK RESTART"))
            RestartWithPlan(hwnd, net::RestartPlanMode::Quick);
        if (UIMenuRow("CHANGE PLAN"))
            RestartWithPlan(hwnd, net::RestartPlanMode::ChangePlan);
    } else {
        ImGui::SetWindowFontScale(g_menuBodyFont ? 1.0f : 1.15f);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 14.0f);
        ImGui::TextDisabled("Waiting for host...");
    }
    ImGui::SetWindowFontScale(g_menuBodyFont ? 1.0f : 1.15f);
    ImGui::Dummy(ImVec2(0.0f, 18.0f));
    {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 cursor = ImGui::GetCursorScreenPos();
        draw->AddRectFilled(ImVec2(cursor.x + 14.0f, cursor.y),
                            ImVec2(cursor.x + deathWidth, cursor.y + 1.0f),
                            IM_COL32(255, 255, 255, 40));
    }
    ImGui::Dummy(ImVec2(0.0f, 14.0f));
    ImGui::SetWindowFontScale(1.7f);
    if (UIMenuRow("MAIN MENU"))
        OpenMainMenu();
    ImGui::SetWindowFontScale(g_menuBodyFont ? 1.0f : 1.15f);
    ImGui::End();
}

// Squad wipe screen: shown when all players are downed in multiplayer.
// Only shown after a 2-second delay so it doesn't flash on/off during rapid events.
static void RenderSquadWipeScreen(HWND hwnd) {
    if (!squadWipeCursorReleased) {
        cameraLocked = true;
        ReleaseCapture();
        SetCursorVisible(true);
        squadWipeCursorReleased = true;
    }
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::GetBackgroundDrawList()->AddRectFilled(
        ImVec2(0, 0), display, IM_COL32(30, 12, 9, 220));
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(400.0f, 315.0f), ImGuiCond_Always);
    ImGui::Begin("Squad Wipe Screen", nullptr, ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse);
    ImGui::Dummy(ImVec2(0.0f, 12.0f));
    const char* wiped = "SQUAD WIPED";
    const float panelWidth = ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                         (panelWidth - ImGui::CalcTextSize(wiped).x) * 0.5f);
    ImGui::TextColored(ImVec4(1.0f, 0.24f, 0.18f, 1.0f), "%s", wiped);
    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    {
        ImDrawList* wipeDraw = ImGui::GetWindowDrawList();
        const ImVec2 cursor = ImGui::GetCursorScreenPos();
        constexpr float ruleWidth = 52.0f;
        const float ruleX = cursor.x + (panelWidth - ruleWidth) * 0.5f;
        wipeDraw->AddRectFilled(ImVec2(ruleX, cursor.y),
                                ImVec2(ruleX + ruleWidth, cursor.y + 2.0f),
                                IM_COL32(240, 80, 60, 240), 1.0f);
    }
    ImGui::Dummy(ImVec2(0.0f, 20.0f));

    // The host restarts the shared level; each client replays its own kit.
    const bool isHost = g_netSession.CurrentRole() == net::Role::Host;
    if (isHost) {
        if (UIPrimaryButton("QUICK RESTART"))
            RestartWithPlan(hwnd, net::RestartPlanMode::Quick);
        if (UIMenuButton("CHANGE PLAN", 40.0f))
            RestartWithPlan(hwnd, net::RestartPlanMode::ChangePlan);
    } else {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                             (panelWidth - ImGui::CalcTextSize("Waiting for host...").x) * 0.5f);
        ImGui::TextColored(ImVec4(0.85f, 0.85f, 0.85f, 1.0f), "Waiting for host...");
        ImGui::Dummy(ImVec2(0.0f, 14.0f));
    }

    if (UIMenuButton("LEAVE TO MENU", 40.0f)) {
        ShutdownMultiplayer();
        OpenMainMenu();
    }
    ImGui::End();
}

// Scoreboard overlay, shown when TAB is held during gameplay. Lists every
// session player in multiplayer, or the local run's tally in single player.
static void RenderScoreboard() {
    if ((FocusedKeyState(VK_TAB) & 0x8000) == 0) return;

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(380.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.85f);
    if (ImGui::Begin("Scoreboard", nullptr, ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {

        ImGui::TextColored(UITheme::kTextDim, "SCOREBOARD");
        ImGui::Separator();

        // Display headers
        ImGui::TextColored(UITheme::kTextDim, "%-16s %6s %6s %7s",
                          "PLAYER", "KILLS", "DEATHS", "REVIVES");
        ImGui::Separator();

        static std::vector<net::ScoreboardEntry> entries;
        if (MultiplayerActive()) {
            g_netSession.GetScoreboard(entries);
        } else {
            entries.assign(1, g_singlePlayerScore);
            entries[0].id = 0;
        }

        if (entries.empty()) {
            ImGui::TextColored(UITheme::kTextDim, "No session");
        } else {
            for (const auto& entry : entries) {
                if (entry.id >= net::kMaxPlayers) continue;
                char name[24];
                const bool you = !MultiplayerActive() ||
                                 entry.id == g_netSession.LocalId();
                std::snprintf(name, sizeof(name), "PLAYER-%d%s",
                              static_cast<int>(entry.id) + 1,
                              you ? " (you)" : "");
                ImGui::Text("%-16s %6u %6u %7u", name,
                            entry.kills, entry.deaths, entry.revives);
            }
        }
        ImGui::End();
    }
}

// `text` cut to fit `maxWidth` in the current font, ending in "..." when it was
// shortened. Returns `text` itself when it already fits, otherwise `buffer`.
static const char* EllipsizeToWidth(const char* text, float maxWidth,
                                    char* buffer, size_t bufferSize) {
    if (ImGui::CalcTextSize(text).x <= maxWidth || bufferSize < 4) return text;
    const float dotsWidth = ImGui::CalcTextSize("...").x;
    size_t length = (std::min)(std::strlen(text), bufferSize - 4);
    while (length > 0 &&
           ImGui::CalcTextSize(text, text + length).x + dotsWidth > maxWidth)
        --length;
    while (length > 0 && text[length - 1] == ' ') --length;
    std::memcpy(buffer, text, length);
    std::memcpy(buffer + length, "...", 4);
    return buffer;
}

// One product row. Returns true when the row was activated, which the caller
// turns into a purchase and/or an equip.
//
// `owned` and `equipped` are passed in rather than derived here because the
// departments store their selection differently -- weapons in two slots,
// grenade and gear as a single enum each -- and pushing that back to the caller
// keeps this drawing code identical for all of them. It is shared by the deploy
// screen and the in-world armory counter so a rifle is presented and priced the
// same way in both.
static bool DrawArmoryRow(const char* name, const char* blurb, int price,
                          bool owned, bool equipped,
                          const char* equippedNote) {
    ImGui::PushID(name);
    const bool affordable = owned || g_game.money.Balance() >= price;
    // The row is one selectable spanning the full width with the text drawn
    // over it, so the whole card is the click target rather than a button
    // tucked at one end.
    constexpr float kRowHeight = 52.0f;
    const ImVec2 rowStart = ImGui::GetCursorScreenPos();
    const float rowWidth = ImGui::GetContentRegionAvail().x;
    const ImVec4 rowBase = equipped ? ImVec4(0.18f, 0.18f, 0.18f, 1.0f)
                                    : ImVec4(0.075f, 0.082f, 0.075f, 1.0f);
    ImGui::PushStyleColor(ImGuiCol_Header,
                          rowBase);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered,
                          equipped ? ImVec4(0.25f, 0.25f, 0.25f, 1.0f)
                                   : UITheme::kControlHover);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,
                          equipped ? ImVec4(0.31f, 0.31f, 0.31f, 1.0f)
                                   : UITheme::kAccentDim);
    // Equipped rows stay highlighted; unaffordable ones are inert so the
    // player cannot commit to something the balance will refuse.
    ImGui::BeginDisabled(!affordable);
    const bool pressed = ImGui::Selectable("##row", equipped,
                                           ImGuiSelectableFlags_None,
                                           ImVec2(0.0f, kRowHeight));
    ImGui::EndDisabled();
    ImGui::PopStyleColor(3);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRect(rowStart, ImVec2(rowStart.x + rowWidth,
                                   rowStart.y + kRowHeight),
                  IM_COL32(108, 116, 100, affordable ? 150 : 70), 0.0f, 0, 1.0f);
    draw->AddRectFilled(rowStart, ImVec2(rowStart.x + 3.0f,
                                         rowStart.y + kRowHeight),
                        equipped ? IM_COL32(255, 255, 255, 255)
                                 : IM_COL32(118, 128, 107, 150));
    draw->PushClipRect(rowStart, ImVec2(rowStart.x + rowWidth,
                                        rowStart.y + kRowHeight), true);
    const float textX = rowStart.x + 10.0f;
    // Dim the whole card when it is out of reach, so "cannot afford" reads
    // at a glance instead of only from the price.
    const ImVec4& nameColor = !affordable ? UITheme::kTextDim
                            : equipped    ? ImVec4(1.0f, 1.0f, 1.0f, 1.0f)
                                          : UITheme::kText;

    // Price, right-aligned. Owned items show their status rather than a
    // number -- the money is already spent, and repeating the price would
    // read as a charge that is about to happen again.
    char priceText[32];
    const char* rightText = priceText;
    ImVec4 rightColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    if (equipped) {
        rightText = "EQUIPPED";
        rightColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
    } else if (owned) {
        rightText = price > 0 ? "OWNED" : "ISSUED";
        rightColor = UITheme::kTextDim;
    } else {
        MoneySystem::Format(priceText, sizeof(priceText), price);
        if (!affordable) rightColor = ImVec4(0.75f, 0.32f, 0.28f, 1.0f);
    }
    const float rightWidth = ImGui::CalcTextSize(rightText).x;
    draw->PushClipRect(rowStart,
        ImVec2((std::max)(rowStart.x, rowStart.x + rowWidth - rightWidth - 24.0f),
               rowStart.y + kRowHeight), true);
    draw->AddText(ImVec2(textX + 2.0f, rowStart.y + 7.0f),
                  ImGui::GetColorU32(nameColor), name);
    draw->PopClipRect();
    draw->AddText(
        ImVec2(rowStart.x + rowWidth - rightWidth - 12.0f,
               rowStart.y + 7.0f),
        ImGui::GetColorU32(rightColor), rightText);

    // Second line: the shop copy, or why the row cannot be taken. The
    // reason displaces the blurb rather than joining it, because a player
    // who cannot afford a row does not need to be sold on it.
    const char* subText = blurb;
    if (equipped && equippedNote) subText = equippedNote;
    else if (!affordable) subText = "Insufficient funds.";
    // Ellipsized rather than clipped: a sentence sliced mid-word at the card
    // edge reads as a layout fault.
    char subBuffer[256];
    subText = EllipsizeToWidth(subText,
        rowStart.x + rowWidth - (textX + 2.0f) - 12.0f,
        subBuffer, sizeof(subBuffer));
    draw->AddText(ImVec2(textX + 2.0f, rowStart.y + 29.0f),
                  ImGui::GetColorU32(UITheme::kTextDim), subText);
    draw->PopClipRect();

    ImGui::PopID();
    return pressed && affordable;
}

static void SaveLastDeployment() {
    LastDeployment& last = g_lastDeployment;
    last.valid = true;
    last.levelName = g_activeCustomLevelName;
    last.levelFile = g_activeLevelFile;
    last.levelKind = g_activeLevelKind;
    last.loadout = g_game.mission.Loadout();
    for (int weapon = 0; weapon < MissionLoadout::kWeaponCount; ++weapon)
        last.weapons[static_cast<size_t>(weapon)] =
            *scene.player.weapons.Instance(weapon);
    last.ownedWeapons = g_ownedWeapons;
    last.ownedGrenades = g_ownedGrenades;
    last.ownedGear = g_ownedGear;
    last.ownedAttachments = g_ownedAttachments;
    last.target = g_deploymentTarget;
    last.marines = g_deploymentMarineCount;
    last.insertion = g_playerInsertionChoice;
    last.airframe = g_insertionAirframe;
    last.leftSeat = g_playerRidesLeftSeat;
    last.timeOfDay = g_selectedTimeOfDay;
    last.weather = scene.weatherState;
    last.enemyDamageMultiplier = scene.enemyDamageMultiplier;
}

static void RestoreLastDeployment() {
    const LastDeployment& last = g_lastDeployment;
    g_game.mission.Loadout() = last.loadout;
    for (int weapon = 0; weapon < MissionLoadout::kWeaponCount; ++weapon)
        scene.player.weapons.SetInstance(
            last.weapons[static_cast<size_t>(weapon)]);
    g_ownedWeapons = last.ownedWeapons;
    g_ownedGrenades = last.ownedGrenades;
    g_ownedGear = last.ownedGear;
    g_ownedAttachments = last.ownedAttachments;
    g_deploymentMarineCount = last.marines;
    g_playerInsertionChoice = last.insertion;
    ApplyInsertionAirframe(last.airframe);
    g_playerRidesLeftSeat = last.leftSeat;
    g_selectedTimeOfDay = last.timeOfDay;
    scene.enemyDamageMultiplier = last.enemyDamageMultiplier;
    ApplyLiveWeatherState(last.weather);
    // Usually the old zone is already in the rebuilt ring. Keep its original
    // marker; add the exact world position only if the ring has moved.
    g_selectedDeploymentZone = -1;
    for (size_t index = 0; index < g_deploymentZones.size(); ++index) {
        const XMFLOAT3& zone = g_deploymentZones[index];
        const float dx = zone.x - last.target.x;
        const float dy = zone.y - last.target.y;
        const float dz = zone.z - last.target.z;
        if (dx * dx + dy * dy + dz * dz < 0.0001f) {
            g_selectedDeploymentZone = static_cast<int>(index);
            break;
        }
    }
    if (g_selectedDeploymentZone < 0) {
        g_deploymentZones.push_back(last.target);
        g_selectedDeploymentZone = static_cast<int>(g_deploymentZones.size()) - 1;
    }
}

static void CommitDeployment(HWND hwnd, bool replayPaidPlan) {
    MissionLoadout& loadout = g_game.mission.Loadout();
    // A squad pre-filled from the base counter bypasses the deploy slider's
    // disabled state, so never charge for marines with no mesh to spawn.
    if (!g_marineModel.valid) g_deploymentMarineCount = 0;
    if (replayPaidPlan) {
        // The saved squad was paid for on the previous attempt. Only newly
        // added marines need another purchase when the player changes plans.
        const int extraMarines = g_deploymentMarineCount - g_lastDeployment.marines;
        if (extraMarines > 0) {
            if (ArmoryPurchase(extraMarines * kDeploymentMarinePrice))
                SaveCareer();
            else g_deploymentMarineCount = g_lastDeployment.marines;
        }
    } else {
        char autoMarines[16] = {};
        if (GetEnvironmentVariableA("SGE_AUTO_MARINES", autoMarines,
                                    sizeof(autoMarines)) > 0) {
            g_deploymentMarineCount = (std::clamp)(
                std::atoi(autoMarines), 0, kMaxDeploymentMarines);
            SGE_LOG("LogGameplay", EngineLog::Level::Display,
                "Auto-marines: " + std::to_string(g_deploymentMarineCount));
        } else if (g_deploymentMarineCount > 0) {
            const int squadPrice =
                g_deploymentMarineCount * kDeploymentMarinePrice;
            if (ArmoryPurchase(squadPrice)) SaveCareer();
            else g_deploymentMarineCount = 0;
        }
    }
    for (size_t slot = 0; slot < loadout.weapons.size(); ++slot) {
        if (GunModel::WeaponLoaded(loadout.weapons[slot])) continue;
        for (int candidate = 0; candidate <= GunModel::kMaxWeapon;
             ++candidate) {
            if (!GunModel::WeaponLoaded(candidate) ||
                loadout.ContainsWeapon(candidate)) continue;
            loadout.SelectWeapon(slot, candidate);
            break;
        }
    }
    if (scene.player.godMode) {
        GunModel::DisableLoadoutRestriction();
        GunModel::SelectedWeapon() = loadout.weapons[0];
    } else {
        GunModel::ConfigureLoadout(loadout.weapons[0], loadout.weapons[1]);
    }
    scene.selectedGrenade = loadout.grenade;
    ApplyTimeOfDay(g_selectedTimeOfDay);
    g_deploymentTarget = g_deploymentZones[
        static_cast<size_t>(g_selectedDeploymentZone)];
    g_deploymentTargetValid = true;
    g_insertionChoicePending = false;
    g_insertionChoiceCursorReleased = false;
    cameraLocked = false;
    scene.camera.FPSMode = true;
    scene.camera.Position = {
        g_deploymentTarget.x,
        g_deploymentTarget.y + scene.camera.PlayerHeight,
        g_deploymentTarget.z };
    scene.camera.FloorY = g_deploymentTarget.y;
    scene.camera.VerticalVelocity = 0.0f;
    scene.camera.IsGrounded = true;
    g_game.session.ResetTimer(true);
    g_game.mission.SetCommTowerCount(CountStandingCommTowers());
    ArmObjectivePlanes();
    visBuffer.InvalidateTemporalHistory();
    g_game.commands.Request(GameCommand::ResetDDGIHistory);
    SetCapture(hwnd);
    SetCursorVisible(false);
    firstMouse = true;
    g_replayPlanActive = false;
    SaveLastDeployment();
    // What the run is actually fought in, so a replay can be compared with the
    // attempt it replays: conditions plus a fingerprint of the enemy layout.
    {
        uint64_t layout = 1469598103934665603ull;
        uint32_t live = 0;
        for (const auto& bandit : g_bandits) {
            if (!bandit || bandit->Dead() || bandit->faction != Faction::Bandit)
                continue;
            ++live;
            const int32_t qx = static_cast<int32_t>(std::lround(bandit->position.x * 10.0f));
            const int32_t qz = static_cast<int32_t>(std::lround(bandit->position.z * 10.0f));
            layout = (layout ^ static_cast<uint32_t>(qx)) * 1099511628211ull;
            layout = (layout ^ static_cast<uint32_t>(qz)) * 1099511628211ull;
        }
        char line[256];
        std::snprintf(line, sizeof(line),
            "Deploy conditions: %s, %s, fog %.6f, scatter seed %u, "
            "%u bandits, layout %016llx, replay %d",
            TimeOfDayName(g_selectedTimeOfDay), WeatherStateName(scene.weatherState),
            VolumetricFogFor(g_selectedTimeOfDay).density, g_scatterEnemiesSeed,
            live, static_cast<unsigned long long>(layout), replayPaidPlan ? 1 : 0);
        SGE_LOG("LogGameplay", EngineLog::Level::Display, line);
    }
}

// The planning map's grid: 50 m squares lettered west to east and numbered
// north to south (+Z is north), starting at the north-west corner of a square
// that covers the whole island and a margin of sea.
static constexpr float kDeploymentGridSpacing = 50.0f;

static float DeploymentGridExtent() {
    const TerrainRendererDX12::Params terrain = CurrentTerrainParams();
    const float reach = 88.0f * (std::max)(terrain.islandScaleX,
                                           terrain.islandScaleZ);
    return std::ceil(reach / kDeploymentGridSpacing) * kDeploymentGridSpacing +
           kDeploymentGridSpacing;
}

// "E5 4471 2290": the square, then an eight-figure reference to the metre
// against a fixed false origin.
static void FormatDeploymentGrid(float x, float z, char* out, size_t size) {
    const float extent = DeploymentGridExtent();
    const int column = (std::max)(0, (std::min)(25, static_cast<int>(
        std::floor((x + extent) / kDeploymentGridSpacing))));
    const int row = (std::max)(0, static_cast<int>(
        std::floor((extent - z) / kDeploymentGridSpacing))) + 1;
    const auto figures = [](float metres, int falseOrigin) {
        return ((static_cast<int>(std::floor(metres)) + falseOrigin) % 10000 +
                10000) % 10000;
    };
    std::snprintf(out, size, "%c%d %04d %04d", 'A' + column, row,
                  figures(x, 44710), figures(z, 22900));
}

// ---- Loadout picker -----------------------------------------------------------
//
// The planning screen's armory is a loadout card: one tile per slot (primary,
// secondary, ordnance, gear). Clicking a tile opens this near full-screen
// chooser for that slot -- the choices down the left as cards, the focused one
// previewed large in the middle with its overview, and its stats, rail
// attachments and the confirm button on the right. Buying and equipping stay
// one action, as on the old storefront rows.

static std::shared_ptr<SceneMesh> WeaponPreviewModel(int weapon) {
    switch (weapon) {
    case 0: return GunModel::Mesh();
    case 1: return GunModel::ShotgunMesh();
    case 2: return GunModel::RPGMesh();
    case 3: case 8: return GunModel::R700Mesh();
    case 5: {
        static std::shared_ptr<SceneNode> source;
        static std::shared_ptr<SceneMesh> mesh;
        static bool attempted = false;
        static int readyFrame = 0;
        // The base hub can skip world props; keep a private charge for its picker.
        if (!g_c4Model && !attempted) {
            attempted = true;
            source = GLBImporter::LoadGLB("Content/Models/C4/C4_bomb/source/c4.glb",
                g_dx12.device, g_dx12.commandList);
        }
        const auto model = g_c4Model ? g_c4Model : source;
        if (!model) return nullptr;
        if (!mesh || model != source) {
            source = model;
            const auto flattened = GLBImporter::MergeSceneByMaterial(model, g_dx12.device);
            if (!flattened) return nullptr;
            mesh = std::make_shared<SceneMesh>();
            const auto collect = [&](const auto& self,
                                     const std::shared_ptr<SceneNode>& node) -> void {
                if (!node) return;
                if (node->mesh)
                    for (const auto& primitive : node->mesh->primitives) {
                        mesh->primitives.push_back(primitive);
                        // Match the gameplay charge's opaque moulded plastic.
                        if (primitive.material) {
                            auto m = std::make_shared<SceneMaterial>(*primitive.material);
                            const bool display =
                                m->name == "c4_panorama_control_panel";
                            m->baseColorFactor = display
                                ? XMFLOAT4(0.018f, 0.045f, 0.026f, 1.0f)
                                : XMFLOAT4(1, 1, 1, 1);
                            m->alphaBlend = m->alphaCutout = m->alphaFromLuminance = false;
                            m->metallicFactor = 0.0f;
                            m->roughnessFactor = display ? 0.55f : 0.85f;
                            m->roughnessOnlyTexture = false;
                            mesh->primitives.back().material = std::move(m);
                        }
                    }
                for (const auto& child : node->children) self(self, child);
            };
            collect(collect, flattened);
            // The normal frame upload flush precedes UI rendering. Let it
            // upload this new geometry before caching an immutable snapshot.
            readyFrame = ImGui::GetFrameCount() + 1;
        }
        return ImGui::GetFrameCount() >= readyFrame ? mesh : nullptr;
    }
    case 7: return GunModel::HarpoonGunMesh();
    case 9: return GunModel::M4Mesh();
    case 10: return GunModel::AK74Mesh();
    case 11: return GunModel::M9Mesh();
    case 12: return GunModel::KrissMesh();
    case 13: return GunModel::GarandMesh();
    default: return nullptr;
    }
}

// Immutable snapshots share the inspector's materials and lighting. Each icon
// is drawn once, then sampled by every loadout card without another model pass.
static uint64_t WeaponThumbnail(int weapon) {
    const auto model = WeaponPreviewModel(weapon);
    if (!model) return 0;
    struct Snapshot {
        WeaponPreviewDX12 renderer;
        uint64_t texture = 0;
        Snapshot() { renderer.Width = 384; renderer.Height = 216; }
    };
    static std::unordered_map<const SceneMesh*, Snapshot> snapshots;
    auto& snapshot = snapshots[model.get()];
    if (!snapshot.texture) {
        auto& geometry = g_weaponPreviewMeshes[model.get()];
        if (geometry.Prepare(model)) {
            ImVec2 uvMax;
            // The M9 export's muzzle runs opposite the other normalised guns.
            snapshot.texture = snapshot.renderer.Render(geometry, weapon == 11 ? XM_PI : 0.0f,
                weapon == 5 ? 0.85f : 0.0f, 1.0f, ImVec2(384, 216), uvMax, true);
        }
    }
    return snapshot.texture;
}

static void DrawWeaponPreview(int weapon, ImVec2 min, ImVec2 max) {
    static int previousWeapon = -1, previousSlot = -1;
    static float yaw = 0.18f, pitch = 0.12f, zoom = 1.0f;
    if (previousWeapon != weapon || previousSlot != g_loadoutPickerSlot) {
        previousWeapon = weapon;
        previousSlot = g_loadoutPickerSlot;
        yaw = 0.18f; pitch = weapon == 5 ? 0.85f : 0.12f; zoom = 1.0f;
        // Capture the same model at distinct poses without driving the pointer.
        char pose[96] = {};
        float poseYaw, posePitch, poseZoom;
        if (GetEnvironmentVariableA("SGE_WEAPON_PREVIEW_POSE", pose, sizeof(pose)) > 0 &&
            sscanf_s(pose, "%f,%f,%f", &poseYaw, &posePitch, &poseZoom) == 3 &&
            std::isfinite(poseYaw) && std::isfinite(posePitch) && std::isfinite(poseZoom)) {
            yaw = std::remainder(poseYaw, XM_2PI);
            pitch = (std::clamp)(posePitch, -1.35f, 1.35f);
            zoom = (std::clamp)(poseZoom, 0.55f, 3.0f);
        }
    }
    const ImVec2 size((std::max)(1.0f, max.x - min.x),
                      (std::max)(1.0f, max.y - min.y));
    ImGui::SetCursorScreenPos(min);
    ImGui::SetNextItemAllowOverlap();
    ImGui::InvisibleButton("##WeaponOrbit", size);
    ImGui::SetItemKeyOwner(ImGuiKey_MouseWheelY);
    const bool hovered = ImGui::IsItemHovered();
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        const ImVec2 delta = ImGui::GetIO().MouseDelta;
        yaw = std::remainder(yaw - delta.x * 0.01f, XM_2PI);
        pitch = (std::clamp)(pitch + delta.y * 0.01f, -1.35f, 1.35f);
    }
    if (hovered) {
        zoom = (std::clamp)(zoom * std::pow(0.88f, ImGui::GetIO().MouseWheel), 0.55f, 3.0f);
    }
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(min, max, IM_COL32(33, 38, 46, 255));
    const auto model = WeaponPreviewModel(weapon);
    const char* status = "3D PREVIEW UNAVAILABLE";
    if (model) {
        auto& geometry = g_weaponPreviewMeshes[model.get()];
        if (geometry.Prepare(model)) {
            ImVec2 uvMax;
            const uint64_t texture = g_weaponPreview.Render(geometry, yaw, pitch, zoom,
                size, uvMax, false, weapon == 5 ? 0.85f : 0.12f);
            if (texture) {
                draw->AddImage((ImTextureID)(intptr_t)texture, min, max, ImVec2(0, 0), uvMax);
                status = nullptr;
            }
        } else if (!geometry.failed) {
            status = "LOADING 3D MODEL...";
        }
    }
    if (status) {
        const ImVec2 textSize = ImGui::CalcTextSize(status);
        draw->AddText(ImVec2(min.x + (size.x - textSize.x) * 0.5f,
                            min.y + (size.y - textSize.y) * 0.5f),
                      IM_COL32(140, 152, 144, 230), status);
    }
    draw->AddText(ImVec2(min.x + 12.0f, max.y - ImGui::GetTextLineHeight() - 12.0f),
                  IM_COL32(180, 192, 184, 230), "DRAG TO ROTATE  //  SCROLL TO ZOOM");
    ImGui::SetCursorScreenPos(ImVec2((std::max)(min.x, max.x - 126.0f), min.y + 10.0f));
    if (ImGui::Button("RESET VIEW", ImVec2(116.0f, 28.0f))) {
        yaw = 0.18f; pitch = weapon == 5 ? 0.85f : 0.12f; zoom = 1.0f;
    }
}

// Ammunition and role, for the card sub-lines and the preview header.
static const char* WeaponCalibre(int weapon) {
    static constexpr const char* kCalibres[MissionLoadout::kWeaponCount] = {
        "7.62x39mm", "12 gauge", "PG-7V rocket", ".308 Winchester",
        "Cutting beam", "Remote charge", "Fuel", "Barbed spear",
        ".308 Winchester", "5.56 NATO", "5.45x39mm", "9mm Parabellum",
        ".45 ACP", ".30-06 Springfield", "Laser designator" };
    return weapon >= 0 && weapon < MissionLoadout::kWeaponCount
        ? kCalibres[weapon] : "";
}

static const char* WeaponRole(int weapon) {
    static constexpr const char* kRoles[MissionLoadout::kWeaponCount] = {
        "ASSAULT RIFLE", "PUMP SHOTGUN", "ROCKET LAUNCHER", "SNIPER RIFLE",
        "LASER CUTTER", "DEMOLITION", "FLAMETHROWER", "HARPOON GUN",
        "SNIPER RIFLE", "CARBINE", "ASSAULT RIFLE", "SIDEARM", "SUBMACHINE GUN",
        "BATTLE RIFLE", "TARGET DESIGNATOR" };
    return weapon >= 0 && weapon < MissionLoadout::kWeaponCount
        ? kRoles[weapon] : "";
}

// Preserve the snapshot's aspect ratio so short weapons and C4 remain complete.
static void DrawFittedThumbnail(ImDrawList* draw, uint64_t texture,
                                ImVec2 min, ImVec2 max) {
    const float width = max.x - min.x, height = max.y - min.y;
    draw->AddRectFilled(min, max, IM_COL32(33, 38, 46, 255));
    const ImVec2 centre((min.x + max.x) * 0.5f, (min.y + max.y) * 0.5f);
    if (texture) {
        const float fit = (std::min)(width / 384.0f, height / 216.0f);
        const ImVec2 half(384.0f * fit * 0.5f, 216.0f * fit * 0.5f);
        draw->AddImage((ImTextureID)(intptr_t)texture,
            ImVec2(centre.x - half.x, centre.y - half.y),
            ImVec2(centre.x + half.x, centre.y + half.y));
    } else {
        const float side = (std::min)(width, height);
        draw->AddRect(ImVec2(centre.x - side * 0.5f, centre.y - side * 0.5f),
                      ImVec2(centre.x + side * 0.5f, centre.y + side * 0.5f),
                      IM_COL32(120, 132, 124, 50), 0.0f, 0, 1.0f);
    }
}

// What the picker edits. The deploy screen points it at the mission loadout;
// the base armory counter at the kit the player is carrying, which has its own
// rules for racking a weapon (a fresh magazine on purchase, a swap when the
// weapon is already in the other slot).
struct LoadoutPickerTarget {
    int weapons[2] = { -1, -1 };
    // Ordnance and gear slots. Null where there are none to pick.
    MissionLoadout* mission = nullptr;
    // Racks `weapon` into `slot` once it is paid for; `bought` is true on the
    // purchase itself.
    std::function<void(int slot, int weapon, bool bought)> equipWeapon;
    // After an attachment is fitted or removed.
    std::function<void()> kitChanged;
    float top = 104.0f;
};

static void RenderLoadoutPicker(const LoadoutPickerTarget& target, ImVec2 display) {
    if (g_loadoutPickerSlot < 0) return;
    const int slot = g_loadoutPickerSlot;
    const bool weaponSlot = slot <= 1;
    if (!weaponSlot && !target.mission) { g_loadoutPickerSlot = -1; return; }

    struct Choice {
        int id;
        const char* name;
        const char* sub;
        const char* blurb;
        int price;
        bool owned;
        bool equipped;
        const char* note;
    };
    std::vector<Choice> choices;
    if (weaponSlot) {
        for (int weapon = 0; weapon < MissionLoadout::kWeaponCount; ++weapon) {
            if (!GunModel::WeaponLoaded(weapon)) continue;
            const bool equipped = target.weapons[slot] == weapon;
            const bool otherSlot = !equipped && target.weapons[1 - slot] == weapon;
            choices.push_back({ weapon, GunModel::WeaponName(weapon),
                WeaponCalibre(weapon), ArmoryCatalog::WeaponBlurb(weapon),
                ArmoryCatalog::WeaponPrice(weapon), ArmoryWeaponOwned(weapon),
                equipped, otherSlot ? (slot == 0 ? "IN SECONDARY" : "IN PRIMARY")
                                    : nullptr });
        }
    } else if (slot == 2) {
        for (int index = 0; index < ArmoryCatalog::kGrenadeCount; ++index) {
            const GrenadeType type = static_cast<GrenadeType>(index);
            choices.push_back({ index, ArmoryCatalog::kGrenadeNames[index],
                "Hand-thrown", ArmoryCatalog::kGrenadeBlurbs[index],
                ArmoryCatalog::kGrenadePrices[index], ArmoryGrenadeOwned(type),
                target.mission->grenade == type, nullptr });
        }
    } else {
        for (int index = 0; index < ArmoryCatalog::kGearCount; ++index) {
            const GearType type = static_cast<GearType>(index);
            choices.push_back({ index, ArmoryCatalog::kGearNames[index],
                "Field equipment", ArmoryCatalog::kGearBlurbs[index],
                ArmoryCatalog::kGearPrices[index], ArmoryGearOwned(type),
                target.mission->gear == type, nullptr });
        }
    }
    if (choices.empty()) { g_loadoutPickerSlot = -1; return; }
    const Choice* focus = nullptr;
    for (const Choice& choice : choices)
        if (choice.id == g_loadoutPickerFocus) focus = &choice;
    if (!focus)
        for (const Choice& choice : choices)
            if (choice.equipped) focus = &choice;
    if (!focus) focus = &choices.front();
    g_loadoutPickerFocus = focus->id;

    static constexpr const char* kSlotTitles[4] = {
        "PRIMARY WEAPON", "SECONDARY WEAPON", "ORDNANCE", "FIELD GEAR" };
    static constexpr const char* kPickTitles[4] = {
        "PICK PRIMARY WEAPON", "PICK SECONDARY WEAPON", "PICK ORDNANCE",
        "PICK FIELD GEAR" };
    static constexpr const char* kPickHints[4] = {
        "Select a primary weapon for this mission",
        "Select a secondary weapon for this mission",
        "Select the grenade you carry in",
        "Select the gear you carry in" };

    const ImU32 textColor = IM_COL32(240, 244, 240, 255);
    const ImU32 dimColor = IM_COL32(140, 152, 144, 230);
    const ImU32 accent = ImGui::GetColorU32(UITheme::kAccent);
    const ImU32 amber = IM_COL32(255, 180, 70, 255);
    ImFont* titleFont = g_menuTitleFont ? g_menuTitleFont : ImGui::GetFont();
    const float lineHeight = ImGui::GetTextLineHeight();

    // Opened this frame: bring it in front of the planning panels once.
    static int focusedSlot = -1;
    if (focusedSlot != slot) {
        ImGui::SetNextWindowFocus();
        focusedSlot = slot;
    }
    ImGui::SetNextWindowPos(ImVec2(24.0f, target.top), ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(display.x - 48.0f, display.y - target.top - 40.0f),
                             ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.030f, 0.045f, 0.050f, 0.82f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.47f, 0.52f, 0.49f, 0.6f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0f, 18.0f));
    ImGui::Begin("##LoadoutPicker", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);

    const float height = ImGui::GetContentRegionAvail().y;
    const float leftWidth = 400.0f;
    const float rightWidth = 420.0f;

    // ---- Choices -------------------------------------------------------------
    ImGui::BeginChild("##PickerChoices", ImVec2(leftWidth, height), false,
                      ImGuiWindowFlags_NoBackground);
    {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        draw->AddText(titleFont, 30.0f, origin, textColor, kPickTitles[slot]);
        draw->AddText(ImVec2(origin.x, origin.y + 36.0f), dimColor, kPickHints[slot]);
        ImGui::Dummy(ImVec2(0.0f, 36.0f + lineHeight + 14.0f));
        ImGui::BeginChild("##PickerList", ImVec2(0.0f, height - 140.0f), false,
                          ImGuiWindowFlags_NoBackground);
        ImDrawList* listDraw = ImGui::GetWindowDrawList();
        constexpr float kCardHeight = 96.0f;
        for (const Choice& choice : choices) {
            ImGui::PushID(choice.id);
            const float width = ImGui::GetContentRegionAvail().x;
            const ImVec2 min = ImGui::GetCursorScreenPos();
            const ImVec2 max(min.x + width, min.y + kCardHeight);
            if (ImGui::InvisibleButton("##card", ImVec2(width, kCardHeight)))
                g_loadoutPickerFocus = choice.id;
            const bool hovered = ImGui::IsItemHovered();
            const bool focused = choice.id == focus->id;
            listDraw->AddRectFilled(min, max, focused
                ? IM_COL32(22, 52, 40, 190)
                : hovered ? IM_COL32(26, 34, 32, 190) : IM_COL32(14, 20, 20, 190));
            listDraw->AddRect(min, max, focused ? accent
                : IM_COL32(120, 132, 124, hovered ? 160 : 90), 0.0f, 0,
                focused ? 2.0f : 1.0f);
            if (focused)
                listDraw->AddRectFilled(min, ImVec2(min.x + 4.0f, max.y), accent);
            const float textX = min.x + 16.0f;
            listDraw->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 1.25f,
                              ImVec2(textX, min.y + 14.0f), textColor, choice.name);
            listDraw->AddText(ImVec2(textX, min.y + 20.0f + lineHeight * 1.25f),
                              dimColor, choice.sub);
            char status[32];
            ImU32 statusColor = dimColor;
            if (choice.equipped) {
                std::snprintf(status, sizeof(status), "EQUIPPED");
                statusColor = accent;
            } else if (choice.note) {
                std::snprintf(status, sizeof(status), "%s", choice.note);
            } else if (choice.owned) {
                std::snprintf(status, sizeof(status), "%s",
                              choice.price > 0 ? "OWNED" : "ISSUED");
            } else {
                MoneySystem::Format(status, sizeof(status), choice.price);
                statusColor = g_game.money.Balance() >= choice.price
                    ? textColor : IM_COL32(200, 90, 80, 255);
            }
            listDraw->AddText(ImVec2(textX, max.y - lineHeight - 12.0f),
                              statusColor, status);
            if (weaponSlot)
                DrawFittedThumbnail(listDraw, WeaponThumbnail(choice.id),
                    ImVec2(min.x + width * 0.52f, min.y + 6.0f),
                    ImVec2(max.x - 36.0f, max.y - 6.0f));
            // Radio mark: filled when this is what the slot carries.
            const ImVec2 mark(max.x - 20.0f, min.y + 20.0f);
            if (choice.equipped) {
                listDraw->AddCircleFilled(mark, 9.0f, accent);
                listDraw->AddLine(ImVec2(mark.x - 4.0f, mark.y),
                                  ImVec2(mark.x - 1.0f, mark.y + 3.0f),
                                  IM_COL32(8, 14, 12, 255), 2.0f);
                listDraw->AddLine(ImVec2(mark.x - 1.0f, mark.y + 3.0f),
                                  ImVec2(mark.x + 4.0f, mark.y - 3.0f),
                                  IM_COL32(8, 14, 12, 255), 2.0f);
            } else {
                listDraw->AddCircle(mark, 9.0f, IM_COL32(200, 210, 204, 200), 0, 1.5f);
            }
            ImGui::Dummy(ImVec2(0.0f, 4.0f));
            ImGui::PopID();
        }
        ImGui::EndChild();
        // BACK, bottom-left like the reference ops screens.
        ImGui::SetCursorPosY(height - 52.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
        if (ImGui::Button("<  BACK", ImVec2(140.0f, 44.0f))) {
            g_loadoutPickerSlot = -1;
            g_loadoutPickerFocus = -1;
        }
        ImGui::PopStyleVar();
    }
    ImGui::EndChild();

    // ---- Preview -------------------------------------------------------------
    ImGui::SameLine(0.0f, 20.0f);
    const float centreWidth = ImGui::GetContentRegionAvail().x - rightWidth - 20.0f;
    ImGui::BeginChild("##PickerPreview", ImVec2(centreWidth, height), true,
        ImGuiWindowFlags_NoScrollWithMouse);
    {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        draw->AddText(origin, dimColor, kSlotTitles[slot]);
        draw->AddText(titleFont, 52.0f, ImVec2(origin.x, origin.y + lineHeight + 4.0f),
                      textColor, focus->name);
        char subLine[96];
        if (weaponSlot)
            std::snprintf(subLine, sizeof(subLine), "%s  //  %s",
                          WeaponCalibre(focus->id), WeaponRole(focus->id));
        else
            std::snprintf(subLine, sizeof(subLine), "%s", focus->sub);
        draw->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 1.15f,
                      ImVec2(origin.x, origin.y + lineHeight + 64.0f),
                      dimColor, subLine);
        const float imageTop = origin.y + lineHeight + 100.0f;
        const float imageBottom = origin.y + ImGui::GetContentRegionAvail().y * 0.62f;
        if (weaponSlot)
            DrawWeaponPreview(focus->id, ImVec2(origin.x, imageTop),
                              ImVec2(origin.x + width, imageBottom));
        ImGui::SetCursorScreenPos(ImVec2(origin.x, imageBottom + 18.0f));
        ImGui::Separator();
        ImGui::TextColored(ImVec4(1.0f, 0.70f, 0.24f, 1.0f), "OVERVIEW");
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(UITheme::kText, "%s", focus->blurb);
        ImGui::PopTextWrapPos();
        if (weaponSlot) {
            const SGE::ResolvedWeaponStats stats = scene.player.weapons.Resolve(focus->id);
            ImGui::Dummy(ImVec2(0.0f, 6.0f));
            ImGui::TextColored(ImVec4(1.0f, 0.70f, 0.24f, 1.0f), "AMMUNITION");
            ImGui::TextColored(UITheme::kText, "%s", WeaponCalibre(focus->id));
            if (stats.magazineCapacity > 0)
                ImGui::TextColored(UITheme::kTextDim,
                    "%d-round magazine, %d carried in reserve, %.1f s reload.",
                    stats.magazineCapacity, stats.maximumReserve,
                    stats.reloadSeconds);
        }
    }
    ImGui::EndChild();

    // ---- Stats, attachments, confirm -----------------------------------------
    ImGui::SameLine(0.0f, 20.0f);
    ImGui::BeginChild("##PickerDetails", ImVec2(rightWidth, height), false,
                      ImGuiWindowFlags_NoBackground);
    {
        const float detailHeight = height - 84.0f;
        ImGui::BeginChild("##PickerDetailScroll", ImVec2(0.0f, detailHeight), true);
        if (weaponSlot) {
            ImGui::TextColored(UITheme::kText, "WEAPON STATS");
            ImGui::Separator();
            // Each bar is the weapon's figure against the best in the armory,
            // so the bars compare the actual choices on offer.
            float maxDamage = 0.01f, maxRate = 0.01f, maxAccuracy = 0.01f,
                  maxControl = 0.01f, maxMagazine = 1.0f, maxHandling = 0.01f;
            const auto accuracyOf = [](const SGE::ResolvedWeaponStats& s) {
                return 1.0f / (std::max)(0.05f,
                    s.hipSpreadMultiplier * 0.5f + s.adsSpreadMultiplier * 0.5f);
            };
            const auto controlOf = [](const SGE::ResolvedWeaponStats& s) {
                return 1.0f / (std::max)(0.2f, s.recoilPitchDegrees + s.recoilYawDegrees);
            };
            for (const Choice& choice : choices) {
                const SGE::ResolvedWeaponStats s = scene.player.weapons.Resolve(choice.id);
                maxDamage = (std::max)(maxDamage, s.damageMultiplier);
                maxRate = (std::max)(maxRate, 60.0f / (std::max)(0.01f, s.fireIntervalSeconds));
                maxAccuracy = (std::max)(maxAccuracy, accuracyOf(s));
                maxControl = (std::max)(maxControl, controlOf(s));
                maxMagazine = (std::max)(maxMagazine, static_cast<float>(s.magazineCapacity));
                maxHandling = (std::max)(maxHandling,
                    1.0f / (std::max)(0.2f, s.reloadSeconds));
            }
            const SGE::ResolvedWeaponStats s = scene.player.weapons.Resolve(focus->id);
            const float rpm = 60.0f / (std::max)(0.01f, s.fireIntervalSeconds);
            char values[6][32];
            std::snprintf(values[0], sizeof(values[0]), "x%.2f", s.damageMultiplier);
            std::snprintf(values[1], sizeof(values[1]), "%.0f", accuracyOf(s) / maxAccuracy * 100.0f);
            std::snprintf(values[2], sizeof(values[2]), "%.0f", controlOf(s) / maxControl * 100.0f);
            std::snprintf(values[3], sizeof(values[3]), "%.0f RPM", rpm);
            std::snprintf(values[4], sizeof(values[4]), "%d", s.magazineCapacity);
            std::snprintf(values[5], sizeof(values[5]), "%.1f s", s.reloadSeconds);
            // Reserve the widest value for every row, including its units, so
            // bars align and never extend underneath the numbers.
            float valueWidth = 0.0f;
            for (const auto& value : values)
                valueWidth = (std::max)(valueWidth, ImGui::CalcTextSize(value).x);
            const auto bar = [&](const char* label, float fraction, const char* value) {
                ImDrawList* draw = ImGui::GetWindowDrawList();
                const ImVec2 at = ImGui::GetCursorScreenPos();
                const float width = ImGui::GetContentRegionAvail().x;
                draw->AddText(at, IM_COL32(210, 218, 214, 255), label);
                const float barLeft = at.x + 120.0f;
                const float barRight = (std::max)(barLeft, at.x + width - valueWidth - 16.0f);
                const float barY = at.y + lineHeight * 0.5f;
                draw->AddRectFilled(ImVec2(barLeft, barY - 3.0f),
                                    ImVec2(barRight, barY + 3.0f),
                                    IM_COL32(255, 255, 255, 30));
                draw->AddRectFilled(ImVec2(barLeft, barY - 3.0f),
                    ImVec2(barLeft + (barRight - barLeft) *
                               (std::min)(1.0f, (std::max)(0.0f, fraction)),
                           barY + 3.0f),
                    IM_COL32(245, 248, 245, 245));
                const ImVec2 valueSize = ImGui::CalcTextSize(value);
                draw->AddText(ImVec2(at.x + width - valueSize.x, at.y),
                              IM_COL32(250, 252, 250, 255), value);
                ImGui::Dummy(ImVec2(width, lineHeight + 8.0f));
            };
            bar("DAMAGE", s.damageMultiplier / maxDamage, values[0]);
            bar("ACCURACY", accuracyOf(s) / maxAccuracy, values[1]);
            bar("CONTROL", controlOf(s) / maxControl, values[2]);
            bar("FIRE RATE", rpm / maxRate, values[3]);
            bar("MAGAZINE", s.magazineCapacity / maxMagazine, values[4]);
            bar("HANDLING", (1.0f / (std::max)(0.2f, s.reloadSeconds)) / maxHandling, values[5]);

            // Rail attachments for the previewed weapon. Bought once per part
            // and fitted per weapon, exactly as the old rail-systems rows.
            ImGui::Dummy(ImVec2(0.0f, 8.0f));
            int compatible = 0, fitted = 0;
            for (const SGE::AttachmentDefinition& attachment :
                 scene.player.weapons.Attachments())
                if (attachment.CompatibleWith(focus->id)) {
                    ++compatible;
                    if (scene.player.weapons.AttachmentInstalled(focus->id, attachment.id))
                        ++fitted;
                }
            ImGui::TextColored(UITheme::kText, "ATTACHMENTS");
            if (compatible > 0) {
                ImGui::SameLine();
                const std::string count = std::to_string(fitted) + "/" +
                    std::to_string(compatible) + " FITTED";
                ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x -
                                     ImGui::CalcTextSize(count.c_str()).x);
                ImGui::TextColored(UITheme::kAccent, "%s", count.c_str());
            }
            ImGui::Separator();
            if (compatible == 0)
                ImGui::TextDisabled("No attachment rails on this weapon.");
            for (const SGE::AttachmentDefinition& attachment :
                 scene.player.weapons.Attachments()) {
                if (!attachment.CompatibleWith(focus->id)) continue;
                const int price = ArmoryCatalog::AttachmentPrice(
                    attachment.suppressesWeapon, attachment.providesRedDot,
                    attachment.providesLaser);
                const bool owned = ArmoryAttachmentOwned(attachment.id);
                const bool installed = scene.player.weapons.AttachmentInstalled(
                    focus->id, attachment.id);
                const char* slotName = attachment.suppressesWeapon ? "BARREL"
                    : attachment.providesRedDot ? "OPTIC"
                    : attachment.providesLaser ? "TACTICAL" : "RAIL";
                const char* blurb =
                    attachment.suppressesWeapon
                        ? "Quieter report, smaller flash, less recoil."
                    : attachment.providesRedDot
                        ? "Red aiming point, tighter sight picture."
                    : attachment.providesLaser
                        ? "Visible designator, tighter hip-fire."
                        : "Fitted accessory.";
                ImGui::PushID(attachment.id.c_str());
                const float width = ImGui::GetContentRegionAvail().x;
                const ImVec2 min = ImGui::GetCursorScreenPos();
                const ImVec2 max(min.x + width, min.y + 58.0f);
                const bool affordable = owned || g_game.money.Balance() >= price;
                ImGui::BeginDisabled(!installed && !affordable);
                const bool pressed = ImGui::InvisibleButton("##attachment",
                                                            ImVec2(width, 58.0f));
                ImGui::EndDisabled();
                ImDrawList* draw = ImGui::GetWindowDrawList();
                draw->AddRectFilled(min, max, ImGui::IsItemHovered()
                    ? IM_COL32(26, 34, 32, 190) : IM_COL32(14, 20, 20, 190));
                draw->AddRect(min, max, installed ? accent : IM_COL32(120, 132, 124, 90),
                              0.0f, 0, installed ? 1.6f : 1.0f);
                draw->AddText(ImVec2(min.x + 10.0f, min.y + 8.0f), dimColor, slotName);
                draw->AddText(ImVec2(min.x + 100.0f, min.y + 8.0f), textColor,
                              attachment.displayName.c_str());
                char clipped[96];
                draw->AddText(ImVec2(min.x + 100.0f, min.y + 12.0f + lineHeight),
                              dimColor, EllipsizeToWidth(blurb, width - 110.0f,
                                                         clipped, sizeof(clipped)));
                char status[32];
                if (installed) std::snprintf(status, sizeof(status), "FITTED");
                else if (owned) std::snprintf(status, sizeof(status), "OWNED");
                else MoneySystem::Format(status, sizeof(status), price);
                draw->AddText(ImVec2(min.x + 10.0f, min.y + 12.0f + lineHeight),
                              installed ? accent : textColor, status);
                if (pressed) {
                    if (installed) {
                        scene.player.weapons.RemoveAttachment(focus->id, attachment.slot);
                        if (target.kitChanged) target.kitChanged();
                    } else if (owned || ArmoryPurchase(price)) {
                        g_ownedAttachments.insert(attachment.id);
                        scene.player.weapons.EquipAttachment(focus->id, attachment.id);
                        if (target.kitChanged) target.kitChanged();
                    }
                }
                ImGui::Dummy(ImVec2(0.0f, 2.0f));
                ImGui::PopID();
            }
        } else {
            ImGui::TextColored(UITheme::kText, "DETAILS");
            ImGui::Separator();
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(UITheme::kTextDim, "%s", focus->blurb);
            ImGui::PopTextWrapPos();
            if (slot == 3 && focus->id != 0) {
                ImGui::Dummy(ImVec2(0.0f, 6.0f));
                ImGui::TextDisabled("Press J in the field to use it.");
                if (!TimeOfDayIsDark(g_selectedTimeOfDay))
                    ImGui::TextDisabled("Little use at this time of day.");
            }
        }
        ImGui::EndChild();

        // Confirm: equip, buying first when the choice is not owned yet.
        const bool affordable = focus->owned || g_game.money.Balance() >= focus->price;
        char confirm[48];
        if (focus->equipped) {
            std::snprintf(confirm, sizeof(confirm), "EQUIPPED");
        } else if (focus->owned) {
            std::snprintf(confirm, sizeof(confirm), "CONFIRM");
        } else {
            char price[24];
            MoneySystem::Format(price, sizeof(price), focus->price);
            std::snprintf(confirm, sizeof(confirm), "BUY & CONFIRM  %s", price);
        }
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
        ImGui::PushStyleColor(ImGuiCol_Button, UITheme::kAccentDim);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, UITheme::kAccent);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, UITheme::kAccent);
        ImGui::BeginDisabled(!affordable);
        const bool confirmPressed = ImGui::Button(confirm, ImVec2(-1.0f, 62.0f));
        ImGui::EndDisabled();
        ImGui::PopStyleColor(3);
        ImGui::PopStyleVar();
        if (confirmPressed) {
            if (focus->owned || ArmoryPurchase(focus->price)) {
                if (weaponSlot) {
                    g_ownedWeapons |= (1u << static_cast<uint32_t>(focus->id));
                    if (target.equipWeapon)
                        target.equipWeapon(slot, focus->id, !focus->owned);
                } else if (slot == 2) {
                    g_ownedGrenades |= (1u << static_cast<uint32_t>(focus->id));
                    target.mission->grenade = static_cast<GrenadeType>(focus->id);
                    scene.selectedGrenade = target.mission->grenade;
                } else {
                    g_ownedGear |= (1u << static_cast<uint32_t>(focus->id));
                    target.mission->gear = static_cast<GearType>(focus->id);
                }
                // The base counter banks its kit on every change, ordnance and
                // gear included, or the flight out would land without them.
                if (!weaponSlot && target.kitChanged) target.kitChanged();
                g_loadoutPickerSlot = -1;
                g_loadoutPickerFocus = -1;
            }
        }
        (void)amber;
    }
    ImGui::EndChild();
    ImGui::End();
}

// One clickable plate of the loadout card. Clicking it opens `pickerSlot`'s
// picker (RenderLoadoutPicker). Returns the plate so the caller can draw into
// it. Shared by the deploy screen and the base armory counter.
static std::pair<ImVec2, ImVec2> DrawLoadoutTile(const char* id, float width,
                                                 float height, int pickerSlot) {
    const ImVec2 min = ImGui::GetCursorScreenPos();
    const ImVec2 max(min.x + width, min.y + height);
    if (ImGui::InvisibleButton(id, ImVec2(width, height))) {
        g_loadoutPickerSlot = pickerSlot;
        g_loadoutPickerFocus = -1;
    }
    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(min, max, hovered ? IM_COL32(26, 34, 32, 190)
                                          : IM_COL32(14, 20, 20, 190));
    draw->AddRect(min, max, hovered ? ImGui::GetColorU32(UITheme::kAccent)
                                    : IM_COL32(120, 132, 124, 110),
                  0.0f, 0, hovered ? 1.6f : 1.0f);
    if (hovered) {
        // Chevron: this opens something.
        const ImU32 textColor = IM_COL32(240, 244, 240, 255);
        const ImVec2 c(max.x - 14.0f, (min.y + max.y) * 0.5f);
        draw->AddLine(ImVec2(c.x - 3.0f, c.y - 5.0f), ImVec2(c.x + 2.0f, c.y),
                      textColor, 1.6f);
        draw->AddLine(ImVec2(c.x + 2.0f, c.y), ImVec2(c.x - 3.0f, c.y + 5.0f),
                      textColor, 1.6f);
    }
    return std::make_pair(min, max);
}

// The primary or secondary plate: weapon name, calibre, fitted attachments and
// a side-on thumbnail.
static void DrawLoadoutWeaponTile(int slot, int weapon, float width) {
    const ImU32 textColor = IM_COL32(240, 244, 240, 255);
    const ImU32 dimColor = IM_COL32(140, 152, 144, 230);
    const float lineHeight = ImGui::GetTextLineHeight();
    const auto [min, max] = DrawLoadoutTile(
        slot == 0 ? "##PrimaryTile" : "##SecondaryTile", width, 92.0f, slot);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float textX = min.x + 12.0f;
    draw->AddText(ImVec2(textX, min.y + 8.0f), dimColor,
                  slot == 0 ? "PRIMARY" : "SECONDARY");
    const bool valid = weapon >= 0 && weapon < MissionLoadout::kWeaponCount;
    draw->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 1.25f,
                  ImVec2(textX, min.y + 12.0f + lineHeight), textColor,
                  valid ? GunModel::WeaponName(weapon) : "--");
    draw->AddText(ImVec2(textX, min.y + 16.0f + lineHeight * 2.25f),
                  dimColor, valid ? WeaponCalibre(weapon) : "");
    // Fitted attachments, as a list under the calibre.
    std::string fitted;
    if (valid)
        for (const SGE::AttachmentDefinition& attachment :
             scene.player.weapons.Attachments())
            if (scene.player.weapons.AttachmentInstalled(weapon, attachment.id)) {
                if (!fitted.empty()) fitted += ", ";
                fitted += attachment.displayName;
            }
    if (!fitted.empty()) {
        char clipped[96];
        draw->AddText(ImVec2(textX, max.y - lineHeight - 6.0f),
                      ImGui::GetColorU32(UITheme::kAccent),
                      EllipsizeToWidth(("+ " + fitted).c_str(),
                                       width * 0.5f, clipped, sizeof(clipped)));
    }
    if (valid)
        DrawFittedThumbnail(draw, WeaponThumbnail(weapon),
                            ImVec2(min.x + width * 0.55f, min.y + 4.0f),
                            ImVec2(max.x - 26.0f, max.y - 4.0f));
    ImGui::Dummy(ImVec2(0.0f, 2.0f));
}

// The ordnance and field gear plates, side by side under the weapon plates.
// Clicking one opens its picker (slots 2 and 3).
static void DrawLoadoutKitTiles(const MissionLoadout& loadout, float width) {
    const ImU32 textColor = IM_COL32(240, 244, 240, 255);
    const ImU32 dimColor = IM_COL32(140, 152, 144, 230);
    const float lineHeight = ImGui::GetTextLineHeight();
    const float halfWidth = (width - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    const int grenade = static_cast<int>(loadout.grenade);
    const int gear = static_cast<int>(loadout.gear);
    const auto smallTile = [&](const char* id, const char* label,
                               const char* value, int pickerSlot) {
        const auto [min, max] = DrawLoadoutTile(id, halfWidth, 64.0f, pickerSlot);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddText(ImVec2(min.x + 12.0f, min.y + 8.0f), dimColor, label);
        char clipped[64];
        draw->AddText(ImVec2(min.x + 12.0f, min.y + 14.0f + lineHeight), textColor,
                      EllipsizeToWidth(value, halfWidth - 34.0f, clipped,
                                       sizeof(clipped)));
    };
    smallTile("##OrdnanceTile", "ORDNANCE",
              grenade >= 0 && grenade < ArmoryCatalog::kGrenadeCount
                  ? ArmoryCatalog::kGrenadeNames[grenade] : "--", 2);
    ImGui::SameLine();
    smallTile("##GearTile", "FIELD GEAR",
              gear >= 0 && gear < ArmoryCatalog::kGearCount
                  ? ArmoryCatalog::kGearNames[gear] : "--", 3);
}

// Shared by the screens' capture test hooks: once the named variable holds a
// path, the presented frame (UI included) is written there four seconds after
// the screen first draws, and the game quits.
struct UICaptureHook {
    double start = -1.0;
    int state = 0;
};
static void RunUICaptureHook(UICaptureHook& hook, const char* variable) {
    char path[MAX_PATH] = {};
    if (hook.state == 2 ||
        GetEnvironmentVariableA(variable, path, sizeof(path)) == 0)
        return;
    if (hook.start < 0.0) hook.start = ImGui::GetTime();
    if (hook.state == 0 && ImGui::GetTime() - hook.start > 4.0) {
        g_frameCapturePath = path;
        hook.state = 1;
    } else if (hook.state == 1 && g_frameCapturePath.empty()) {
        PostQuitMessage(0);
        hook.state = 2;
    }
}

// Change Plan opens this map with the last committed choices restored. Quick
// Restart commits those choices as soon as the level is ready.
static void RenderInsertionChoiceScreen(HWND hwnd) {
    if (g_deploymentRestartPending != net::RestartPlanMode::None) {
        const net::RestartPlanMode mode = g_deploymentRestartPending;
        g_deploymentRestartPending = net::RestartPlanMode::None;
        RestoreLastDeployment();
        g_replayPlanActive = true;
        if (mode == net::RestartPlanMode::Quick) {
            CommitDeployment(hwnd, true);
            return;
        }
    }
    // Free the pointer so the buttons can be clicked, the way the death screen
    // does. Recaptured below once the choice is made.
    if (!g_insertionChoiceCursorReleased) {
        cameraLocked = true;
        ReleaseCapture();
        SetCursorVisible(true);
        g_insertionChoiceCursorReleased = true;
    }
    // Commander callout, fired here rather than where the planning state is set:
    // that happens behind the loading screen, so the line would play to nobody
    // and be half over by the time the screen appeared. The guard keeps it to
    // one play per deployment no matter how long the player spends planning.
    if (!g_readyToDropPlayed) {
        g_readyToDropAudio.Play(1.0f);
        g_readyToDropPlayed = true;
        g_menuMusicRestartRequested = true;
    }
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    // Keep capture timing alive when the picker replaces the planning screen.
    static UICaptureHook captureHook;
    RunUICaptureHook(captureHook, "SGE_UI_CAPTURE_PATH");
    if (g_loadoutPickerSlot >= 0) {
        g_briefingTypingAudio.StopLoop();
        // Host orders still reach a client inspecting its loadout.
        if (g_squadDeployRequested && g_selectedDeploymentZone >= 0) {
            g_squadDeployRequested = false;
            CommitDeployment(hwnd, g_replayPlanActive);
            return;
        }
        auto& loadout = g_game.mission.Loadout();
        LoadoutPickerTarget target;
        target.weapons[0] = loadout.weapons[0];
        target.weapons[1] = loadout.weapons[1];
        target.mission = &loadout;
        target.top = 24.0f;
        target.equipWeapon = [&loadout](int slot, int weapon, bool) {
            loadout.SelectWeapon(static_cast<size_t>(slot), weapon);
        };
        RenderLoadoutPicker(target, display);
        return;
    }
    // Unplated columns and fading scrims carry the main menu's identity over
    // the live map while keeping the briefing readable as the island rotates.
    const uint32_t standingTowers = CountStandingCommTowers();
    const uint32_t objectivePlanes = CountObjectivePlanes();
    const bool showMissionBriefing = !g_deploymentBriefingUnderstood &&
        (standingTowers > 0 || objectivePlanes > 0);
    ImGui::SetNextWindowPos(ImVec2(display.x - 24.0f, 110.0f),
                            ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("##DeploymentArmoryToggle", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBackground);
    if (ImGui::Button(g_deploymentArmoryVisible ? "HIDE ARMORY >" : "< SHOW ARMORY",
                      ImVec2(160.0f, 30.0f)))
        g_deploymentArmoryVisible = !g_deploymentArmoryVisible;
    ImGui::End();
    ImGui::PopStyleVar();
    ImDrawList* backdrop = ImGui::GetBackgroundDrawList();
    const float scrimWidth = (std::min)(display.x * 0.48f, 680.0f);
    const float solidWidth = (std::min)(display.x * 0.32f, 454.0f);
    if (showMissionBriefing) {
        backdrop->AddRectFilled(ImVec2(0, 0), ImVec2(solidWidth, display.y),
            IM_COL32(5, 10, 12, 70));
        backdrop->AddRectFilledMultiColor(ImVec2(solidWidth, 0), ImVec2(scrimWidth, display.y),
            IM_COL32(5, 10, 12, 70), IM_COL32(5, 10, 12, 0),
            IM_COL32(5, 10, 12, 0), IM_COL32(5, 10, 12, 70));
    }
    if (g_deploymentArmoryVisible) {
        backdrop->AddRectFilled(ImVec2(display.x - solidWidth, 0), display,
            IM_COL32(5, 10, 12, 70));
        backdrop->AddRectFilledMultiColor(ImVec2(display.x - scrimWidth, 0),
            ImVec2(display.x - solidWidth, display.y),
            IM_COL32(5, 10, 12, 0), IM_COL32(5, 10, 12, 70),
            IM_COL32(5, 10, 12, 70), IM_COL32(5, 10, 12, 0));
    }
    backdrop->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(display.x, 100),
        IM_COL32(0, 0, 0, 190), IM_COL32(0, 0, 0, 190),
        IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0));
    backdrop->AddRectFilledMultiColor(ImVec2(0, display.y * 0.82f), display,
        IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0),
        IM_COL32(0, 0, 0, 170), IM_COL32(0, 0, 0, 170));

    // Development controls stay off the player's screen; Ctrl+Shift+D brings
    // them back (see g_deploymentDevTools).
    {
        const ImGuiIO& io = ImGui::GetIO();
        if (io.KeyCtrl && io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_D, false))
            g_deploymentDevTools = !g_deploymentDevTools;
    }

    const XMMATRIX viewProjection =
        scene.GetViewMatrix() * scene.GetProjectionMatrix();
    ImDrawList* foreground = ImGui::GetForegroundDrawList();
    const ImVec2 mouse = ImGui::GetMousePos();
    const auto projectToScreen = [&](const XMFLOAT3& world, ImVec2& screen) {
        const XMVECTOR clip = XMVector3Transform(XMLoadFloat3(&world),
                                                 viewProjection);
        const float w = XMVectorGetW(clip);
        if (w <= 0.01f) return false;
        screen = { (XMVectorGetX(clip) / w * 0.5f + 0.5f) * display.x,
                   (1.0f - (XMVectorGetY(clip) / w * 0.5f + 0.5f)) * display.y };
        return true;
    };
    const char* operationName = standingTowers > 0 ? "OPERATION BLACKOUT"
        : objectivePlanes > 0 ? "OPERATION GROUNDSWELL" : "OPERATION ORDER";

    // ---- Map furniture ------------------------------------------------------
    // A tactical overlay over the live island: a grid draped on the ground,
    // a frame, a north arrow and a scale bar. Background list, so the markers
    // and every panel sit above it; clipped to the open map between the
    // side columns.
    const ImVec2 mapMin(showMissionBriefing ? solidWidth + 20.0f : 18.0f, 108.0f);
    const ImVec2 mapMax(g_deploymentArmoryVisible
        ? display.x - solidWidth - 20.0f : display.x - 18.0f, display.y - 44.0f);
    const ImU32 furniture = IM_COL32(210, 222, 214, 150);
    {
        const TerrainRendererDX12::Params gridTerrain = CurrentTerrainParams();
        const auto groundPoint = [&](float x, float z, ImVec2& screen) {
            const float y = (std::max)(0.0f,
                TerrainRendererDX12::HeightAt(gridTerrain, x, z)) + 0.5f;
            return projectToScreen(XMFLOAT3{ x, y, z }, screen);
        };
        constexpr float kGridSpacing = kDeploymentGridSpacing;
        const float extent = DeploymentGridExtent();
        // The whole screen between the header and footer: the panels are
        // translucent, so the grid carries on under them like a full-screen map.
        backdrop->PushClipRect(ImVec2(0.0f, 92.0f),
                               ImVec2(display.x, display.y - 30.0f), true);
        for (float line = -extent; line <= extent + 0.5f; line += kGridSpacing) {
            for (int axis = 0; axis < 2; ++axis) {
                constexpr int kSegments = 48;
                ImVec2 previous{};
                bool previousVisible = false;
                for (int segment = 0; segment <= kSegments; ++segment) {
                    const float along = -extent + 2.0f * extent *
                        static_cast<float>(segment) / kSegments;
                    ImVec2 current{};
                    const bool visible = axis == 0
                        ? groundPoint(line, along, current)
                        : groundPoint(along, line, current);
                    if (visible && previousVisible)
                        backdrop->AddLine(previous, current,
                                          IM_COL32(210, 230, 220, 60), 1.0f);
                    previous = current;
                    previousVisible = visible;
                }
            }
        }
        backdrop->PopClipRect();
        // Square letters along the top of the map frame and numbers down its
        // left side, like a printed map's margin. Each sits where the centre
        // line of its column or row crosses that edge on screen, so the labels
        // slide as the map turns and always name the squares beneath them --
        // the same names FormatDeploymentGrid prints.
        const float topEdge = mapMin.y + 14.0f;
        const float leftEdge = mapMin.x + 14.0f;
        const ImU32 labelColor = IM_COL32(230, 238, 232, 210);
        const int squares = static_cast<int>(2.0f * extent / kGridSpacing + 0.5f);
        for (int square = 0; square < squares && square < 26; ++square) {
            const float centre = -extent + (square + 0.5f) * kGridSpacing;
            // Trace the centre line and take the first crossing of the edge
            // inside the frame. axis 0: column centre (x fixed), crossing the
            // top edge; axis 1: row centre (z fixed), crossing the left edge.
            for (int axis = 0; axis < 2; ++axis) {
                constexpr int kSegments = 96;
                ImVec2 previous{};
                bool previousVisible = false;
                bool placed = false;
                for (int segment = 0; segment <= kSegments; ++segment) {
                    const float along = -extent + 2.0f * extent *
                        static_cast<float>(segment) / kSegments;
                    ImVec2 current{};
                    const bool visible = axis == 0
                        ? groundPoint(centre, along, current)
                        : groundPoint(along, -centre, current);
                    if (visible && previousVisible) {
                        const float a = axis == 0 ? previous.y - topEdge
                                                  : previous.x - leftEdge;
                        const float b = axis == 0 ? current.y - topEdge
                                                  : current.x - leftEdge;
                        if ((a <= 0.0f) != (b <= 0.0f)) {
                            const float t = a / (a - b);
                            const ImVec2 at(previous.x + (current.x - previous.x) * t,
                                            previous.y + (current.y - previous.y) * t);
                            const bool inside = axis == 0
                                ? at.x > mapMin.x + 24.0f && at.x < mapMax.x - 24.0f
                                : at.y > mapMin.y + 24.0f && at.y < mapMax.y - 24.0f;
                            if (inside) {
                                char text[4];
                                if (axis == 0)
                                    std::snprintf(text, sizeof(text), "%c",
                                                  'A' + square);
                                else
                                    std::snprintf(text, sizeof(text), "%d",
                                                  square + 1);
                                const ImVec2 size = ImGui::CalcTextSize(text);
                                backdrop->AddText(
                                    ImVec2(at.x - size.x * 0.5f,
                                           at.y - size.y * 0.5f),
                                    labelColor, text);
                                placed = true;
                                break;
                            }
                        }
                    }
                    previous = current;
                    previousVisible = visible;
                }
                // Tilted toward the horizon the grid can stop short of the
                // frame edge. Then the label goes just beyond the grid's own
                // north (letters) or west (numbers) edge instead.
                if (!placed) {
                    const float margin = kGridSpacing * 0.3f;
                    ImVec2 at{};
                    const bool visible = axis == 0
                        ? groundPoint(centre, extent + margin, at)
                        : groundPoint(-extent - margin, -centre, at);
                    if (visible && at.x > mapMin.x + 12.0f && at.x < mapMax.x - 12.0f &&
                        at.y > mapMin.y + 12.0f && at.y < mapMax.y - 12.0f) {
                        char text[4];
                        if (axis == 0)
                            std::snprintf(text, sizeof(text), "%c", 'A' + square);
                        else
                            std::snprintf(text, sizeof(text), "%d", square + 1);
                        const ImVec2 size = ImGui::CalcTextSize(text);
                        backdrop->AddText(ImVec2(at.x - size.x * 0.5f,
                                                 at.y - size.y * 0.5f),
                                          labelColor, text);
                    }
                }
            }
        }
    }
    // Corner brackets around the map.
    {
        constexpr float kArm = 26.0f;
        const ImVec2 corners[4] = { mapMin, ImVec2(mapMax.x, mapMin.y),
                                    mapMax, ImVec2(mapMin.x, mapMax.y) };
        for (int corner = 0; corner < 4; ++corner) {
            const ImVec2 c = corners[corner];
            const float sx = (corner == 0 || corner == 3) ? 1.0f : -1.0f;
            const float sy = corner < 2 ? 1.0f : -1.0f;
            backdrop->AddLine(c, ImVec2(c.x + sx * kArm, c.y), furniture, 1.5f);
            backdrop->AddLine(c, ImVec2(c.x, c.y + sy * kArm), furniture, 1.5f);
        }
    }
    // North arrow (+Z is grid north) and a 50 m scale bar, both measured off
    // the island centre through the live camera, so they turn and shrink with
    // the map.
    {
        ImVec2 centre{}, north{}, east{};
        if (projectToScreen(XMFLOAT3{ 0.0f, 0.0f, 0.0f }, centre) &&
            projectToScreen(XMFLOAT3{ 0.0f, 0.0f, 50.0f }, north) &&
            projectToScreen(XMFLOAT3{ 50.0f, 0.0f, 0.0f }, east)) {
            float nx = north.x - centre.x, ny = north.y - centre.y;
            const float nLength = std::sqrt(nx * nx + ny * ny);
            if (nLength > 1.0f) {
                nx /= nLength; ny /= nLength;
                const ImVec2 hub(mapMax.x - 44.0f, mapMin.y + 50.0f);
                backdrop->AddCircle(hub, 22.0f, furniture, 32, 1.0f);
                const ImVec2 tip(hub.x + nx * 18.0f, hub.y + ny * 18.0f);
                const ImVec2 tail(hub.x - nx * 10.0f, hub.y - ny * 10.0f);
                const ImVec2 side(-ny * 6.0f, nx * 6.0f);
                backdrop->AddTriangleFilled(tip,
                    ImVec2(tail.x + side.x, tail.y + side.y),
                    ImVec2(tail.x - side.x, tail.y - side.y),
                    IM_COL32(236, 240, 236, 220));
                const ImVec2 nSize = ImGui::CalcTextSize("N");
                backdrop->AddText(
                    ImVec2(hub.x + nx * 34.0f - nSize.x * 0.5f,
                           hub.y + ny * 34.0f - nSize.y * 0.5f),
                    IM_COL32(236, 240, 236, 230), "N");
            }
            const float ex = east.x - centre.x, ey = east.y - centre.y;
            const float barLength = std::sqrt(ex * ex + ey * ey);
            if (barLength > 8.0f) {
                const ImVec2 start((mapMin.x + mapMax.x - barLength) * 0.5f,
                                   mapMax.y - 22.0f);
                const ImVec2 end(start.x + barLength, start.y);
                backdrop->AddLine(start, end, furniture, 2.0f);
                backdrop->AddLine(start, ImVec2(start.x, start.y - 6.0f), furniture, 2.0f);
                backdrop->AddLine(end, ImVec2(end.x, end.y - 6.0f), furniture, 2.0f);
                const ImVec2 middle(start.x + barLength * 0.5f, start.y);
                backdrop->AddLine(middle, ImVec2(middle.x, middle.y - 4.0f),
                                  furniture, 2.0f);
                const auto tickLabel = [&](ImVec2 at, const char* text) {
                    const ImVec2 size = ImGui::CalcTextSize(text);
                    backdrop->AddText(ImVec2(at.x - size.x * 0.5f, at.y - 8.0f - size.y),
                                      furniture, text);
                };
                tickLabel(start, "0");
                tickLabel(middle, "25");
                tickLabel(end, "50 M");
            }
        }
    }
    // Map symbols, APP-6 style: hostile is a red diamond, friendly a blue
    // rectangle. Dark outline first so each survives the bright sand.
    const auto drawHostile = [&](ImVec2 at, float radius, bool emplacement) {
        const auto diamond = [&](float r, ImU32 color) {
            foreground->AddQuadFilled(ImVec2(at.x, at.y - r), ImVec2(at.x + r, at.y),
                                      ImVec2(at.x, at.y + r), ImVec2(at.x - r, at.y),
                                      color);
        };
        diamond(radius + 2.0f, IM_COL32(20, 4, 6, 210));
        diamond(radius, IM_COL32(232, 48, 48, 235));
        if (emplacement) {
            const float r = radius + 5.0f;
            foreground->AddQuad(ImVec2(at.x, at.y - r), ImVec2(at.x + r, at.y),
                                ImVec2(at.x, at.y + r), ImVec2(at.x - r, at.y),
                                IM_COL32(232, 48, 48, 190), 1.6f);
        }
    };
    const auto drawFriendly = [&](ImVec2 at) {
        foreground->AddRectFilled(ImVec2(at.x - 6.0f, at.y - 4.5f),
                                  ImVec2(at.x + 6.0f, at.y + 4.5f),
                                  IM_COL32(4, 10, 24, 210));
        foreground->AddRectFilled(ImVec2(at.x - 4.5f, at.y - 3.0f),
                                  ImVec2(at.x + 4.5f, at.y + 3.0f),
                                  IM_COL32(70, 150, 255, 235));
    };
    // Markers sit under both side panels now that the briefing occupies the
    // left, so a click is only a zone pick when ImGui itself is not taking it.
    // WantCaptureMouse covers whichever panels are actually on screen, which a
    // hardcoded screen-edge margin did not.
    const bool selectClick = ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                             !ImGui::GetIO().WantCaptureMouse;

    // Right-drag turns and tilts the map by hand. Read here rather than in
    // WndProc: the ImGui backend takes Win32 capture on any button press and
    // this screen
    // releases it every frame for its own panels, so a raw-capture drag was
    // cancelled the moment it started (see g_deploymentOrbitDragging).
    //
    // Gated on the same WantCaptureMouse test the marker pick above uses, so a
    // drag that begins on a side panel belongs to that panel's widgets and
    // never turns the map. ImGui latches the drag once it starts, so crossing
    // over a panel mid-drag keeps turning.
    {
        ImGuiIO& io = ImGui::GetIO();
        const bool overPanel = io.WantCaptureMouse;
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Right) && !overPanel) {
            g_deploymentOrbitDragging = true;
            // Grabbing a coasting map stops it dead, the way catching a spun
            // globe does.
            g_deploymentOrbitVelocity = 0.0f;
            g_deploymentOrbitPendingDrag = 0.0f;
            g_deploymentOrbitPendingTilt = 0.0f;
        }
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Right))
            g_deploymentOrbitDragging = false;
        if (g_deploymentOrbitDragging &&
            ImGui::IsMouseDragging(ImGuiMouseButton_Right)) {
            // MouseDelta is already a per-frame value, which is exactly what
            // the flick needs -- no per-message accumulation to reconcile.
            g_deploymentOrbitPendingDrag +=
                io.MouseDelta.x * kDeploymentOrbitRadiansPerPixel;
            // Dragging up rotates the map down toward the horizon, the way
            // pushing the far edge of a physical model away from you tips it.
            // Elevation has no flick momentum so releasing always leaves the
            // chosen framing.
            g_deploymentOrbitPendingTilt +=
                io.MouseDelta.y * kDeploymentOrbitElevationRadiansPerPixel;
        }
    }

    // Missile targeting takes the click while armed, so it never competes with
    // zone selection. Unproject the cursor through the same view-projection the
    // markers are drawn with, then walk the ray into the terrain with the sweep
    // the projectile system already uses -- picking against the real height
    // field rather than a flat plane, so a strike called on a hillside lands on
    // the slope instead of punching through it.
    bool missileClickConsumed = false;
    if (g_missileStrikeArmed && selectClick) {
        XMVECTOR determinant;
        const XMMATRIX inverseViewProjection =
            XMMatrixInverse(&determinant, viewProjection);
        if (!XMVector4Equal(determinant, XMVectorZero())) {
            const float ndcX = (mouse.x / display.x) * 2.0f - 1.0f;
            const float ndcY = 1.0f - (mouse.y / display.y) * 2.0f;
            const XMVECTOR nearClip = XMVectorSet(ndcX, ndcY, 0.0f, 1.0f);
            const XMVECTOR farClip = XMVectorSet(ndcX, ndcY, 1.0f, 1.0f);
            XMVECTOR nearWorld =
                XMVector4Transform(nearClip, inverseViewProjection);
            XMVECTOR farWorld =
                XMVector4Transform(farClip, inverseViewProjection);
            const float nearW = XMVectorGetW(nearWorld);
            const float farW = XMVectorGetW(farWorld);
            if (std::abs(nearW) > 1e-6f && std::abs(farW) > 1e-6f) {
                nearWorld = XMVectorScale(nearWorld, 1.0f / nearW);
                farWorld = XMVectorScale(farWorld, 1.0f / farW);
                XMFLOAT3 rayStart, rayEnd;
                XMStoreFloat3(&rayStart, nearWorld);
                XMStoreFloat3(&rayEnd, farWorld);
                XMFLOAT3 impact;
                if (HitTerrainSegment(rayStart, rayEnd, 0.0f, impact)) {
                    // Fired from the camera itself, so the round leaves from
                    // the viewpoint the player is looking through and flies out
                    // to where they clicked.
                    LaunchMissileStrike(scene.camera.Position, impact);
                    g_missileStrikeArmed = false;
                    missileClickConsumed = true;
                }
            }
        }
    }

    for (size_t index = 0; index < g_deploymentZones.size(); ++index) {
        XMFLOAT3 marker = g_deploymentZones[index];
        marker.y += 1.3f;
        const XMVECTOR clip = XMVector3Transform(
            XMLoadFloat3(&marker), viewProjection);
        const float w = XMVectorGetW(clip);
        if (w <= 0.01f) continue;
        const ImVec2 screen{
            (XMVectorGetX(clip) / w * 0.5f + 0.5f) * display.x,
            (1.0f - (XMVectorGetY(clip) / w * 0.5f + 0.5f)) * display.y };
        const bool selected = static_cast<int>(index) ==
                              g_selectedDeploymentZone;
        const float dx = mouse.x - screen.x;
        const float dy = mouse.y - screen.y;
        // 24px stays comfortable at 20 zones: the tightest the markers ever get
        // on screen is 51.9px apart, so no two pick areas can overlap.
        const bool hovered = !ImGui::GetIO().WantCaptureMouse &&
                             dx * dx + dy * dy <= 24.0f * 24.0f;
        if (selectClick && !missileClickConsumed && hovered) {
            g_selectedDeploymentZone = static_cast<int>(index);
            g_menuSelectionChanged = true;
        }
        // Landing-zone markers: a dark disc with a white ring, numbered. The
        // chosen one turns insertion-blue with crosshair ticks and an
        // "INSERTION ZONE" tag, so it reads by shape and colour on sand and
        // water alike.
        const ImU32 insertionBlue = IM_COL32(90, 170, 255, 255);
        const float radius = selected ? 15.0f : 12.0f;
        foreground->AddCircleFilled(screen, radius,
            selected ? IM_COL32(10, 34, 62, 240)
                     : IM_COL32(8, 14, 16, hovered ? 230 : 190));
        foreground->AddCircle(screen, radius,
            selected ? insertionBlue
                     : IM_COL32(255, 255, 255, hovered ? 255 : 210), 0,
            selected || hovered ? 2.4f : 1.6f);
        char number[8];
        std::snprintf(number, sizeof(number), "%02zu", index + 1);
        const ImVec2 labelSize = ImGui::CalcTextSize(number);
        foreground->AddText(
            ImVec2(screen.x - labelSize.x * 0.5f,
                   screen.y - labelSize.y * 0.5f),
            IM_COL32(236, 240, 236, 255), number);
        if (selected) {
            const float inner = radius + 4.0f;
            const float outer = radius + 12.0f;
            foreground->AddLine(ImVec2(screen.x, screen.y - inner),
                                ImVec2(screen.x, screen.y - outer), insertionBlue, 2.0f);
            foreground->AddLine(ImVec2(screen.x, screen.y + inner),
                                ImVec2(screen.x, screen.y + outer), insertionBlue, 2.0f);
            foreground->AddLine(ImVec2(screen.x - inner, screen.y),
                                ImVec2(screen.x - outer, screen.y), insertionBlue, 2.0f);
            foreground->AddLine(ImVec2(screen.x + inner, screen.y),
                                ImVec2(screen.x + outer, screen.y), insertionBlue, 2.0f);
        }
        if (selected || hovered) {
            char tag[16];
            std::snprintf(tag, sizeof(tag), "LZ %02zu", index + 1);
            const char* kind = selected ? "INSERTION ZONE" : "LANDING ZONE";
            const ImVec2 kindSize = ImGui::CalcTextSize(kind);
            const ImVec2 tagSize = ImGui::CalcTextSize(tag);
            const ImVec2 tagPos(screen.x + radius + 16.0f,
                                screen.y - (kindSize.y + tagSize.y + 2.0f) * 0.5f);
            foreground->AddRectFilled(
                ImVec2(tagPos.x - 6.0f, tagPos.y - 4.0f),
                ImVec2(tagPos.x + (std::max)(kindSize.x, tagSize.x) + 6.0f,
                       tagPos.y + kindSize.y + tagSize.y + 6.0f),
                IM_COL32(8, 14, 16, 220));
            foreground->AddText(tagPos,
                selected ? insertionBlue : IM_COL32(150, 162, 154, 230), kind);
            foreground->AddText(ImVec2(tagPos.x, tagPos.y + kindSize.y + 2.0f),
                                IM_COL32(240, 244, 240, 255), tag);
        }
    }

    // Enemy positions, as small red dots. Same projection as the zone markers
    // above, drawn after them so a bandit standing on a zone does not hide the
    // marker the player has to click.
    //
    // Deliberately no click handling and no depth test: these are intel on the
    // planning map, not interactive, and a dot that vanished behind terrain
    // would read as "no enemy there" rather than "enemy behind a hill".
    if (g_showEnemyDotsOnDeployScreen) {
        for (const auto& bandit : g_bandits) {
            if (!bandit || bandit->Dead()) continue;
            // Bandits only. Marines are friendly and a red dot would read as a
            // threat the player then plans around for no reason.
            if (bandit->faction != Faction::Bandit) continue;
            // A gunner already covered by a vehicle dot below does not get a
            // second one -- two dots a metre apart read as two enemies rather
            // than one emplacement. That is every authored Humvee gunner
            // (mountedVehicleIndex >= 0), and now the boat gunner too, since the
            // patrol boat draws its own dot further down. The stress-test gunner
            // (kStressHumveeGunnerMount) still has nothing standing in for it,
            // so it keeps its own.
            if (bandit->turretGunner && bandit->mountedVehicleIndex >= 0)
                continue;
            if (bandit->turretGunner &&
                bandit->mountedVehicleIndex == kBoatGunnerMount &&
                g_levelPatrolBoatEnabled && g_boatModel)
                continue;

            XMFLOAT3 spot = bandit->position;
            spot.y += 1.1f;
            const XMVECTOR enemyClip = XMVector3Transform(
                XMLoadFloat3(&spot), viewProjection);
            const float enemyW = XMVectorGetW(enemyClip);
            if (enemyW <= 0.01f) continue;
            const ImVec2 enemyScreen{
                (XMVectorGetX(enemyClip) / enemyW * 0.5f + 0.5f) * display.x,
                (1.0f - (XMVectorGetY(enemyClip) / enemyW * 0.5f + 0.5f)) *
                    display.y };
            // Hostile diamond, outlined so it survives the bright sand the
            // zone ring crosses.
            drawHostile(enemyScreen, 4.0f, false);
        }

        // Authored Humvees, from the level plan rather than from g_bandits.
        // Their turret gunner is a bandit and would be dotted by the loop above,
        // but only once it exists: the load-stage spawn can fail (the Humvee
        // model may not be resident yet), and the retry lives behind the
        // !g_insertionChoicePending gate in the bandit update, so it cannot run
        // until AFTER the player has deployed. Dotting the vehicle itself is
        // independent of that timing -- the emplacement is on the map whether or
        // not anyone is manning it yet.
        for (size_t index = 0; index < g_levelHumveeSpawns.size(); ++index) {
            if (!HumveeAlive(index)) continue;
            const Transform& humvee = g_levelHumveeSpawns[index];
            XMFLOAT3 spot{ humvee.position[0],
                           humvee.position[1] + 2.2f,
                           humvee.position[2] };
            const XMVECTOR humveeClip = XMVector3Transform(
                XMLoadFloat3(&spot), viewProjection);
            const float humveeW = XMVectorGetW(humveeClip);
            if (humveeW <= 0.01f) continue;
            const ImVec2 humveeScreen{
                (XMVectorGetX(humveeClip) / humveeW * 0.5f + 0.5f) * display.x,
                (1.0f - (XMVectorGetY(humveeClip) / humveeW * 0.5f + 0.5f)) *
                    display.y };
            drawHostile(humveeScreen, 5.0f, true);
        }

        // AA emplacements are static hostiles held in their own vector rather
        // than in g_bandits, so the loop above never saw them. They matter more
        // to insertion planning than a foot patrol does -- they are exactly what
        // shoots the helicopter down -- so they get the same dot, drawn slightly
        // larger with a ring to read as an emplacement rather than a man.
        for (const auto& turret : g_game.vehicles.aaTurrets) {
            if (!turret.Active()) continue;
            XMFLOAT3 spot = turret.position;
            spot.y += 1.6f;
            const XMVECTOR turretClip = XMVector3Transform(
                XMLoadFloat3(&spot), viewProjection);
            const float turretW = XMVectorGetW(turretClip);
            if (turretW <= 0.01f) continue;
            const ImVec2 turretScreen{
                (XMVectorGetX(turretClip) / turretW * 0.5f + 0.5f) * display.x,
                (1.0f - (XMVectorGetY(turretClip) / turretW * 0.5f + 0.5f)) *
                    display.y };
            drawHostile(turretScreen, 5.0f, true);
        }

        // Patrol boat. The one hostile on the map that is not where it will be
        // by the time the player lands: it circles the island continuously, and
        // it keeps circling while this screen is open, so the dot moves as the
        // map turns. That is the point of showing it -- an approach that is
        // clear now may have a gunboat sitting on it in thirty seconds.
        //
        // Position rather than spawn: g_boatPosition only leaves the island
        // centre once UpdateBoat has run, so a dot taken from g_boatCenter
        // would sit inland until the first tick.
        if (g_levelPatrolBoatEnabled && g_boatModel &&
            !g_game.vehicles.boatDead && !g_game.vehicles.boatSunk) {
            XMFLOAT3 spot = g_boatPosition;
            spot.y += 2.6f;
            const XMVECTOR boatClip = XMVector3Transform(
                XMLoadFloat3(&spot), viewProjection);
            const float boatW = XMVectorGetW(boatClip);
            if (boatW > 0.01f) {
                const ImVec2 boatScreen{
                    (XMVectorGetX(boatClip) / boatW * 0.5f + 0.5f) * display.x,
                    (1.0f - (XMVectorGetY(boatClip) / boatW * 0.5f + 0.5f)) *
                        display.y };
                drawHostile(boatScreen, 5.0f, true);
                // Heading tick. The other vehicle dots mark fixed emplacements,
                // where a direction would mean nothing; this one is under way,
                // and which way it is going is what decides whether a landing
                // site is about to be overlooked.
                const float tickX = std::sin(g_boatYaw);
                const float tickZ = std::cos(g_boatYaw);
                XMFLOAT3 ahead{ g_boatPosition.x + tickX * 14.0f,
                                spot.y,
                                g_boatPosition.z + tickZ * 14.0f };
                const XMVECTOR aheadClip = XMVector3Transform(
                    XMLoadFloat3(&ahead), viewProjection);
                const float aheadW = XMVectorGetW(aheadClip);
                if (aheadW > 0.01f) {
                    const ImVec2 aheadScreen{
                        (XMVectorGetX(aheadClip) / aheadW * 0.5f + 0.5f) *
                            display.x,
                        (1.0f - (XMVectorGetY(aheadClip) / aheadW * 0.5f + 0.5f))
                            * display.y };
                    foreground->AddLine(boatScreen, aheadScreen,
                                        IM_COL32(232, 48, 48, 200), 1.8f);
                }
            }
        }
    }

    // Friendly marines, in blue. Drawn after the hostiles so a marine standing
    // near a bandit reads as present rather than being buried under the red
    // dot -- knowing where your own squad already is matters most exactly where
    // the fighting is. Same projection, and no depth test for the same reason:
    // a friendly hidden behind a hill is still a friendly you planned around.
    if (g_showAllyDotsOnDeployScreen) {
        for (const auto& marine : g_bandits) {
            if (!marine || marine->Dead()) continue;
            if (marine->faction != Faction::Marine) continue;

            XMFLOAT3 spot = marine->position;
            spot.y += 1.1f;
            const XMVECTOR allyClip = XMVector3Transform(
                XMLoadFloat3(&spot), viewProjection);
            const float allyW = XMVectorGetW(allyClip);
            if (allyW <= 0.01f) continue;
            const ImVec2 allyScreen{
                (XMVectorGetX(allyClip) / allyW * 0.5f + 0.5f) * display.x,
                (1.0f - (XMVectorGetY(allyClip) / allyW * 0.5f + 0.5f)) *
                    display.y };
            // Dark outline first, as the hostile symbols do: the blue would
            // otherwise disappear into the water the perimeter ring crosses.
            drawFriendly(allyScreen);
        }
    }

    // Objective callouts: an amber diamond on each tower or aircraft with a
    // labelled leader, and a dashed route from the chosen landing zone to the
    // first one. The route is on the background list so every marker sits on
    // top of it.
    {
        XMFLOAT3 routeTarget{};
        bool hasRoute = false;
        for (const LevelEntity& entity : g_game.world.Level().entities) {
            if (!entity.enabled || entity.type != LevelEntityType::Prefab) continue;
            const bool tower = entity.prefabId == kCommTowerPrefabId;
            const bool plane = entity.prefabId == kObjectivePlanePrefabId;
            if (!tower && !plane) continue;
            const XMFLOAT3 objectivePoint{ entity.transform.position[0],
                                       entity.transform.position[1] + 3.0f,
                                       entity.transform.position[2] };
            ImVec2 at{};
            if (!projectToScreen(objectivePoint, at)) continue;
            if (!hasRoute) { routeTarget = objectivePoint; hasRoute = true; }
            const ImU32 amber = IM_COL32(255, 170, 50, 255);
            const float r = 11.0f;
            foreground->AddQuadFilled(ImVec2(at.x, at.y - r), ImVec2(at.x + r, at.y),
                                      ImVec2(at.x, at.y + r), ImVec2(at.x - r, at.y),
                                      IM_COL32(40, 22, 6, 220));
            foreground->AddQuad(ImVec2(at.x, at.y - r), ImVec2(at.x + r, at.y),
                                ImVec2(at.x, at.y + r), ImVec2(at.x - r, at.y),
                                amber, 2.0f);
            foreground->AddCircleFilled(at, 3.0f, amber);
            const ImVec2 elbow(at.x + 18.0f, at.y - 26.0f);
            foreground->AddLine(ImVec2(at.x + r * 0.7f, at.y - r * 0.7f), elbow,
                                amber, 1.4f);
            const char* kind = "PRIMARY OBJECTIVE";
            const char* name = tower ? "COMMS TOWER" : "TRANSPORT AIRCRAFT";
            const ImVec2 kindSize = ImGui::CalcTextSize(kind);
            const ImVec2 nameSize = ImGui::CalcTextSize(name);
            const ImVec2 box(elbow.x + 4.0f, elbow.y - kindSize.y - 4.0f);
            foreground->AddRectFilled(
                ImVec2(box.x - 6.0f, box.y - 4.0f),
                ImVec2(box.x + (std::max)(kindSize.x, nameSize.x) + 6.0f,
                       box.y + kindSize.y + nameSize.y + 6.0f),
                IM_COL32(8, 12, 14, 215));
            foreground->AddText(box, amber, kind);
            foreground->AddText(ImVec2(box.x, box.y + kindSize.y + 2.0f),
                                IM_COL32(240, 244, 240, 255), name);
        }
        if (hasRoute && g_selectedDeploymentZone >= 0 &&
            g_selectedDeploymentZone < static_cast<int>(g_deploymentZones.size())) {
            XMFLOAT3 from = g_deploymentZones[
                static_cast<size_t>(g_selectedDeploymentZone)];
            from.y += 1.3f;
            ImVec2 a{}, b{};
            if (projectToScreen(from, a) && projectToScreen(routeTarget, b)) {
                const float dx = b.x - a.x, dy = b.y - a.y;
                const float length = std::sqrt(dx * dx + dy * dy);
                constexpr float kDash = 9.0f, kGap = 7.0f;
                for (float t = 0.0f; t < length; t += kDash + kGap) {
                    const float t1 = (std::min)(length, t + kDash);
                    backdrop->AddLine(
                        ImVec2(a.x + dx * t / length, a.y + dy * t / length),
                        ImVec2(a.x + dx * t1 / length, a.y + dy * t1 / length),
                        IM_COL32(90, 170, 255, 220), 2.0f);
                }
            }
        }
    }

    if (g_deploymentDebugCoverageGuides) {
        const TerrainRendererDX12::Params guideParams =
            CurrentTerrainParams();
        const auto projectGuidePoint = [&](float x, float z,
                                           ImVec2& screen) {
            const XMFLOAT3 world{
                x, TerrainRendererDX12::HeightAt(guideParams, x, z) + 1.5f, z };
            const XMVECTOR clip = XMVector3Transform(
                XMLoadFloat3(&world), viewProjection);
            const float w = XMVectorGetW(clip);
            if (w <= 0.01f) return false;
            screen = {
                (XMVectorGetX(clip) / w * 0.5f + 0.5f) * display.x,
                (1.0f - (XMVectorGetY(clip) / w * 0.5f + 0.5f)) *
                    display.y };
            return true;
        };
        const auto drawEllipse = [&](float radiusX, float radiusZ,
                                     ImU32 color) {
            constexpr int segments = 128;
            ImVec2 previous{};
            bool previousVisible = false;
            for (int segment = 0; segment <= segments; ++segment) {
                const float angle = XM_2PI *
                    static_cast<float>(segment) / segments;
                ImVec2 current{};
                const bool currentVisible = projectGuidePoint(
                    std::cos(angle) * radiusX,
                    std::sin(angle) * radiusZ, current);
                if (previousVisible && currentVisible)
                    foreground->AddLine(previous, current, color, 2.0f);
                previous = current;
                previousVisible = currentVisible;
            }
        };
        const auto drawCoverageSquare = [&](float halfSpan, ImU32 color) {
            const XMFLOAT2 corners[4] = {
                {-halfSpan, -halfSpan}, { halfSpan, -halfSpan},
                { halfSpan,  halfSpan}, {-halfSpan,  halfSpan} };
            constexpr int edgeSegments = 32;
            for (int edge = 0; edge < 4; ++edge) {
                const XMFLOAT2 a = corners[edge];
                const XMFLOAT2 b = corners[(edge + 1) & 3];
                ImVec2 previous{};
                bool previousVisible = false;
                for (int segment = 0; segment <= edgeSegments; ++segment) {
                    const float t = static_cast<float>(segment) / edgeSegments;
                    ImVec2 current{};
                    const bool currentVisible = projectGuidePoint(
                        a.x + (b.x - a.x) * t,
                        a.y + (b.y - a.y) * t, current);
                    if (previousVisible && currentVisible)
                        foreground->AddLine(previous, current, color, 2.0f);
                    previous = current;
                    previousVisible = currentVisible;
                }
            }
        };

        drawEllipse(43.0f * guideParams.islandScaleX,
                    43.0f * guideParams.islandScaleZ,
                    IM_COL32(255, 208, 64, 245));
        drawEllipse(88.0f * guideParams.islandScaleX,
                    88.0f * guideParams.islandScaleZ,
                    IM_COL32(64, 224, 255, 245));
        drawCoverageSquare(
            guideParams.tilesX * guideParams.tileSize * 0.5f,
            IM_COL32(255, 80, 215, 245));
    }

    // ---- Header bar -----------------------------------------------------------
    // Operation name on the left, then the plan at a glance in labelled
    // columns: objective, difficulty, insertion, landing zone, time and
    // weather. An unset entry shows in amber; that replaced the old "select a
    // zone, two weapons..." instruction line. Background list, so the panels'
    // combo popups still open over it.
    // Clickable columns, recorded while drawing and turned into dropdowns
    // once the header is laid out (see the window after this block).
    struct HeaderMenu { ImVec2 min, max; int id; };
    HeaderMenu headerMenus[8];
    int headerMenuCount = 0;
    {
        constexpr float kHeaderHeight = 92.0f;
        backdrop->AddRectFilled(ImVec2(0.0f, 0.0f),
                                ImVec2(display.x, kHeaderHeight),
                                IM_COL32(6, 10, 12, 170));
        backdrop->AddLine(ImVec2(0.0f, kHeaderHeight),
                          ImVec2(display.x, kHeaderHeight),
                          IM_COL32(120, 132, 124, 110), 1.0f);

        const ImU32 labelColor = IM_COL32(140, 152, 144, 230);
        const ImU32 valueColor = IM_COL32(240, 244, 240, 255);
        const ImU32 unsetColor = IM_COL32(255, 200, 60, 255);
        const float textHeight = ImGui::GetTextLineHeight();

        // Operation block.
        float x = 42.0f;
        backdrop->AddText(ImVec2(x, 16.0f), labelColor, "OPERATION");
        const char* operationTitle = operationName;
        if (std::strncmp(operationTitle, "OPERATION ", 10) == 0)
            operationTitle += 10;
        ImFont* titleFont = g_menuTitleFont ? g_menuTitleFont : ImGui::GetFont();
        constexpr float kTitleSize = 34.0f;
        backdrop->AddText(titleFont, kTitleSize, ImVec2(x, 16.0f + textHeight),
                          valueColor, operationTitle);
        x += (std::max)(300.0f,
            titleFont->CalcTextSizeA(kTitleSize, FLT_MAX, 0.0f,
                                     operationTitle).x + 48.0f);

        // Columns. Each is a dim label over a bright value and an optional
        // dim detail line, separated by hairline rules.
        // A column with a `menu` id is clickable: it gets a caret after its
        // label and its rectangle is recorded for the dropdown pass.
        const auto column = [&](const char* label, const char* value,
                                ImU32 color, const char* detail, int menu) {
            backdrop->AddLine(ImVec2(x - 22.0f, 18.0f),
                              ImVec2(x - 22.0f, kHeaderHeight - 18.0f),
                              IM_COL32(120, 132, 124, 90), 1.0f);
            backdrop->AddText(ImVec2(x, 18.0f), labelColor, label);
            backdrop->AddText(ImVec2(x, 22.0f + textHeight), color, value);
            const float labelWidth = ImGui::CalcTextSize(label).x;
            float width = (std::max)(labelWidth, ImGui::CalcTextSize(value).x);
            if (detail) {
                backdrop->AddText(ImVec2(x, 26.0f + textHeight * 2.0f),
                                  labelColor, detail);
                width = (std::max)(width, ImGui::CalcTextSize(detail).x);
            }
            if (menu >= 0) {
                const ImVec2 caret(x + labelWidth + 10.0f, 18.0f + textHeight * 0.5f);
                backdrop->AddTriangleFilled(ImVec2(caret.x - 4.0f, caret.y - 2.0f),
                                            ImVec2(caret.x + 4.0f, caret.y - 2.0f),
                                            ImVec2(caret.x, caret.y + 3.0f),
                                            labelColor);
                width = (std::max)(width, labelWidth + 16.0f);
                if (headerMenuCount < IM_ARRAYSIZE(headerMenus))
                    headerMenus[headerMenuCount++] = {
                        ImVec2(x - 12.0f, 10.0f),
                        ImVec2(x + width + 12.0f, kHeaderHeight - 10.0f), menu };
            }
            x += width + 44.0f;
        };

        if (standingTowers > 0 || objectivePlanes > 0) {
            column("OBJECTIVE",
                   standingTowers > 0 ? "DESTROY COMMS TOWER"
                                      : "DESTROY TRANSPORT AIRCRAFT",
                   IM_COL32(255, 180, 70, 255),
                   standingTowers > 0 ? "Cut the relay to the battery"
                                      : "Before it leaves the runway", -1);
        }

        const float threat = scene.enemyDamageMultiplier;
        const char* difficulty = threat < 0.75f ? "EASY"
            : threat < 1.25f ? "NORMAL" : threat < 2.0f ? "HARD" : "EXTREME";
        const ImU32 difficultyColor = threat < 0.75f ? IM_COL32(120, 220, 140, 255)
            : threat < 1.25f ? valueColor
            : threat < 2.0f ? IM_COL32(255, 120, 80, 255)
                            : IM_COL32(255, 70, 60, 255);
        char threatText[32];
        std::snprintf(threatText, sizeof(threatText), "Enemy damage x%.2f", threat);
        column("DIFFICULTY", difficulty, difficultyColor, threatText, 0);

        const bool helicopter =
            g_playerInsertionChoice != LevelInsertionMode::Boat;
        const char* insertionText =
            g_playerInsertionChoice == LevelInsertionMode::Boat ? "BOAT"
            : g_playerInsertionChoice == LevelInsertionMode::FastRappel
                ? "FAST ROPE" : "HELO";
        column("INSERTION", insertionText, valueColor,
               helicopter ? (g_insertionAirframe == InsertionAirframe::NewBlackHawk
                                 ? "MH-60" : "UH-60")
                          : "Small craft", 1);

        char zoneText[16] = "NOT SET";
        char zoneGrid[32] = "Select on the map";
        if (g_selectedDeploymentZone >= 0 &&
            g_selectedDeploymentZone < static_cast<int>(g_deploymentZones.size())) {
            std::snprintf(zoneText, sizeof(zoneText), "LZ %02d",
                          g_selectedDeploymentZone + 1);
            const XMFLOAT3& zone = g_deploymentZones[
                static_cast<size_t>(g_selectedDeploymentZone)];
            char grid[24];
            FormatDeploymentGrid(zone.x, zone.z, grid, sizeof(grid));
            std::snprintf(zoneGrid, sizeof(zoneGrid), "Grid %s", grid);
        }
        column("LANDING ZONE", zoneText,
               g_selectedDeploymentZone >= 0 ? valueColor : unsetColor, zoneGrid, 2);

        char timeText[24];
        std::snprintf(timeText, sizeof(timeText), "%s",
                      TimeOfDayName(g_selectedTimeOfDay));
        for (char* c = timeText; *c; ++c)
            *c = static_cast<char>(std::toupper(static_cast<unsigned char>(*c)));
        SYSTEMTIME utc{};
        GetSystemTime(&utc);
        static constexpr const char* kMonths[12] = {
            "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
            "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };
        char dtg[32];
        std::snprintf(dtg, sizeof(dtg), "DTG %02u%02u%02uZ %s %02u",
                      utc.wDay, utc.wHour, utc.wMinute,
                      kMonths[(std::max)(1, (std::min)(12, int(utc.wMonth))) - 1],
                      utc.wYear % 100u);
        column("TIME", timeText, valueColor, dtg, 3);

        static constexpr const char* kWeatherLabels[] = {
            "CLEAR", "CLOUDY", "DENSE FOG", "RAIN", "STORM", "CUSTOM" };
        const int weather = static_cast<int>(scene.weatherState);
        column("WEATHER",
               weather >= 0 && weather < IM_ARRAYSIZE(kWeatherLabels)
                   ? kWeatherLabels[weather] : "--",
               valueColor, nullptr, 4);
    }

    // Header dropdowns. A transparent window over the header turns each
    // recorded column into a click target that opens its menu just below it.
    // Being an ImGui window, it also sets WantCaptureMouse, so a click here is
    // never taken as a map pick.
    {
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(display.x, 92.0f), ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
        ImGui::Begin("##DeploymentHeaderMenus", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
            ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
        ImGui::PopStyleVar();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        // Test hook: SGE_AUTO_HEADER_MENU=<id> opens that dropdown once
        // (0 difficulty, 1 insertion, 2 landing zone, 3 time, 4 weather), so
        // the menus can be captured without driving the mouse. Held back two
        // seconds: the side panels take focus the first frame they appear,
        // and a focus change closes any open popup.
        static int autoHeaderMenu = [] {
            char text[8] = {};
            return GetEnvironmentVariableA("SGE_AUTO_HEADER_MENU", text,
                                           sizeof(text)) > 0 ? std::atoi(text) : -1;
        }();
        static int autoHeaderDelay = 120;
        if (autoHeaderMenu >= 0 && autoHeaderDelay > 0) --autoHeaderDelay;
        for (int i = 0; i < headerMenuCount; ++i) {
            const HeaderMenu& menu = headerMenus[i];
            ImGui::PushID(menu.id);
            ImGui::SetCursorScreenPos(menu.min);
            if (ImGui::InvisibleButton("##column",
                    ImVec2(menu.max.x - menu.min.x, menu.max.y - menu.min.y)) ||
                (autoHeaderMenu == menu.id && autoHeaderDelay == 0)) {
                ImGui::OpenPopup("##menu");
                autoHeaderMenu = -1;
            }
            const bool open = ImGui::IsPopupOpen("##menu");
            if (ImGui::IsItemHovered() || open) {
                draw->AddRectFilled(menu.min, menu.max,
                                    IM_COL32(255, 255, 255, open ? 22 : 12));
                draw->AddRect(menu.min, menu.max,
                              IM_COL32(170, 180, 172, open ? 200 : 120), 0.0f, 0, 1.0f);
            }
            ImGui::SetNextWindowPos(ImVec2(menu.min.x, menu.max.y + 4.0f));
            ImGui::SetNextWindowSizeConstraints(
                ImVec2((std::max)(220.0f, menu.max.x - menu.min.x), 0.0f),
                ImVec2(FLT_MAX, display.y * 0.6f));
            if (ImGui::BeginPopup("##menu")) {
                const auto option = [](const char* text, bool selected) {
                    return ImGui::Selectable(text, selected, 0, ImVec2(0.0f, 24.0f));
                };
                switch (menu.id) {
                case 0: { // Difficulty: presets on the enemy damage dial.
                    struct Preset { const char* name; float multiplier; };
                    static constexpr Preset kPresets[] = {
                        { "EASY", 0.5f }, { "NORMAL", 1.0f },
                        { "HARD", 1.5f }, { "EXTREME", 2.5f } };
                    for (const Preset& preset : kPresets) {
                        const bool current =
                            std::abs(scene.enemyDamageMultiplier - preset.multiplier) < 0.01f;
                        char text[48];
                        std::snprintf(text, sizeof(text), "%-9s x%.2f damage",
                                      preset.name, preset.multiplier);
                        if (option(text, current))
                            scene.enemyDamageMultiplier = preset.multiplier;
                    }
                    // The other half of the bargain: a harder run pays more.
                    ImGui::Separator();
                    ImGui::TextColored(CurrentRewardMultiplier() > 1.0f
                                           ? UITheme::kWarning : UITheme::kTextDim,
                                       "Cash and experience  x%.2f",
                                       CurrentRewardMultiplier());
                    break;
                }
                case 1: { // Insertion method.
                    const auto mode = [&](const char* text, LevelInsertionMode value) {
                        if (option(text, g_playerInsertionChoice == value))
                            g_playerInsertionChoice = value;
                    };
                    mode("HELICOPTER LANDING", LevelInsertionMode::Helicopter);
                    mode("FAST ROPE  (-50% AMMO)", LevelInsertionMode::FastRappel);
                    mode("BOAT", LevelInsertionMode::Boat);
                    break;
                }
                case 2: // Landing zone, with each one's grid reference.
                    for (size_t zone = 0; zone < g_deploymentZones.size(); ++zone) {
                        char grid[24];
                        FormatDeploymentGrid(g_deploymentZones[zone].x,
                                             g_deploymentZones[zone].z,
                                             grid, sizeof(grid));
                        char text[48];
                        std::snprintf(text, sizeof(text), "LZ %02zu    %s",
                                      zone + 1, grid);
                        if (option(text, static_cast<int>(zone) ==
                                         g_selectedDeploymentZone))
                            g_selectedDeploymentZone = static_cast<int>(zone);
                    }
                    break;
                case 3: // Time of day, applied live like the panel's buttons.
                    for (const TimeOfDay time : { TimeOfDay::Noon, TimeOfDay::Afternoon,
                                                  TimeOfDay::Dusk, TimeOfDay::Night }) {
                        char text[24];
                        std::snprintf(text, sizeof(text), "%s", TimeOfDayName(time));
                        for (char* c = text; *c; ++c)
                            *c = static_cast<char>(
                                std::toupper(static_cast<unsigned char>(*c)));
                        if (option(text, g_selectedTimeOfDay == time) &&
                            g_selectedTimeOfDay != time) {
                            g_selectedTimeOfDay = time;
                            ApplyTimeOfDay(time);
                        }
                    }
                    break;
                default: { // Weather. Custom is reached through fog tuning only.
                    static constexpr const char* kStates[] = {
                        "CLEAR", "CLOUDY", "DENSE FOG", "RAIN", "STORM" };
                    for (int state = 0; state < IM_ARRAYSIZE(kStates); ++state) {
                        if (option(kStates[state],
                                   static_cast<int>(scene.weatherState) == state))
                            ApplyLiveWeatherState(static_cast<WeatherState>(state));
                    }
                    break;
                }
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        ImGui::End();
    }

    // ---- Footer bar -----------------------------------------------------------
    // Controls this screen answers to, and the squad link when in a session.
    {
        const float top = display.y - 30.0f;
        backdrop->AddRectFilled(ImVec2(0.0f, top), display,
                                IM_COL32(6, 10, 12, 170));
        backdrop->AddLine(ImVec2(0.0f, top), ImVec2(display.x, top),
                          IM_COL32(120, 132, 124, 110), 1.0f);
        const float textY = top + (30.0f - ImGui::GetTextLineHeight()) * 0.5f;
        float x = 42.0f;
        const auto hint = [&](const char* key, const char* action) {
            const ImVec2 keySize = ImGui::CalcTextSize(key);
            backdrop->AddRect(ImVec2(x - 5.0f, textY - 2.0f),
                              ImVec2(x + keySize.x + 5.0f, textY + keySize.y + 2.0f),
                              IM_COL32(170, 180, 172, 200), 2.0f, 0, 1.0f);
            backdrop->AddText(ImVec2(x, textY), IM_COL32(236, 240, 236, 255), key);
            x += keySize.x + 14.0f;
            backdrop->AddText(ImVec2(x, textY), IM_COL32(150, 162, 154, 230), action);
            x += ImGui::CalcTextSize(action).x + 34.0f;
        };
        hint("LMB", "SELECT LANDING ZONE");
        hint("RMB", "DRAG TO ROTATE MAP");
        if (g_deploymentDevTools)
            hint("CTRL+SHIFT+D", "HIDE DEV TOOLS");
        // Framed plates behind the two columns, so the briefing and the plan
        // read as instrument panels either side of the map.
        const auto plate = [&](float left, float right, float top = 104.0f) {
            const ImVec2 min(left, top);
            const ImVec2 max(right, display.y - 40.0f);
            backdrop->AddRectFilled(min, max, IM_COL32(8, 13, 16, 150));
            backdrop->AddRect(min, max, IM_COL32(120, 132, 124, 120), 0.0f, 0, 1.0f);
            backdrop->AddRectFilled(min, ImVec2(right, min.y + 3.0f),
                                    IM_COL32(120, 132, 124, 160));
        };
        if (showMissionBriefing)
            plate(18.0f, 460.0f);
        if (g_deploymentArmoryVisible)
            plate(display.x - 460.0f, display.x - 18.0f, 146.0f);
        if (MultiplayerActive()) {
            const char* link = "SQUAD COMMS  CONNECTED";
            backdrop->AddText(
                ImVec2(display.x - 42.0f - ImGui::CalcTextSize(link).x, textY),
                IM_COL32(110, 220, 140, 255), link);
        }
    }

    // Map legend, a boxed key in the bottom-left corner of the map listing
    // only what is drawn.
    {
        struct LegendEntry { int symbol; const char* label; };
        LegendEntry entries[6];
        int entryCount = 0;
        if (standingTowers > 0 || objectivePlanes > 0) {
            entries[entryCount++] = { 4, "Objective" };
            if (g_selectedDeploymentZone >= 0)
                entries[entryCount++] = { 5, "Insertion route" };
        }
        entries[entryCount++] = { 0, "Landing zone" };
        if (g_showEnemyDotsOnDeployScreen) {
            entries[entryCount++] = { 1, "Hostile" };
            entries[entryCount++] = { 2, "Emplacement / vehicle" };
        }
        if (g_showAllyDotsOnDeployScreen && LiveMarineCount() > 0)
            entries[entryCount++] = { 3, "Friendly" };
        const float lineHeight = ImGui::GetTextLineHeight() + 6.0f;
        constexpr float kSymbolWidth = 26.0f;
        float width = ImGui::CalcTextSize("LEGEND").x;
        for (int i = 0; i < entryCount; ++i)
            width = (std::max)(width, kSymbolWidth +
                                      ImGui::CalcTextSize(entries[i].label).x);
        const float padding = 10.0f;
        const ImVec2 boxMin(mapMin.x + 14.0f,
            mapMax.y - 14.0f - padding * 2.0f - lineHeight * (entryCount + 1));
        const ImVec2 boxMax(boxMin.x + width + padding * 2.0f, mapMax.y - 14.0f);
        foreground->AddRectFilled(boxMin, boxMax, IM_COL32(6, 10, 12, 160));
        foreground->AddRect(boxMin, boxMax, IM_COL32(120, 132, 124, 140), 0.0f, 0, 1.0f);
        float y = boxMin.y + padding;
        foreground->AddText(ImVec2(boxMin.x + padding, y),
                            IM_COL32(150, 162, 154, 230), "LEGEND");
        y += lineHeight;
        for (int i = 0; i < entryCount; ++i) {
            const ImVec2 symbol(boxMin.x + padding + 9.0f,
                                y + ImGui::GetTextLineHeight() * 0.5f);
            switch (entries[i].symbol) {
            case 0:
                foreground->AddCircleFilled(symbol, 6.0f, IM_COL32(8, 14, 16, 200));
                foreground->AddCircle(symbol, 6.0f, IM_COL32(255, 255, 255, 220),
                                      0, 1.4f);
                break;
            case 1: drawHostile(symbol, 4.0f, false); break;
            case 2: drawHostile(symbol, 3.0f, true); break;
            case 3: drawFriendly(symbol); break;
            case 4:
                foreground->AddQuad(ImVec2(symbol.x, symbol.y - 6.0f),
                                    ImVec2(symbol.x + 6.0f, symbol.y),
                                    ImVec2(symbol.x, symbol.y + 6.0f),
                                    ImVec2(symbol.x - 6.0f, symbol.y),
                                    IM_COL32(255, 170, 50, 255), 1.8f);
                break;
            default:
                foreground->AddLine(ImVec2(symbol.x - 9.0f, symbol.y),
                                    ImVec2(symbol.x - 3.0f, symbol.y),
                                    IM_COL32(90, 170, 255, 230), 2.0f);
                foreground->AddLine(ImVec2(symbol.x + 2.0f, symbol.y),
                                    ImVec2(symbol.x + 8.0f, symbol.y),
                                    IM_COL32(90, 170, 255, 230), 2.0f);
                break;
            }
            foreground->AddText(ImVec2(boxMin.x + padding + kSymbolWidth, y),
                                IM_COL32(210, 222, 214, 230), entries[i].label);
            y += lineHeight;
        }
    }

    // Grid and elevation under the cursor, bottom-right of the map: the same
    // terrain sweep the missile pick uses, so the readout is the real ground.
    {
        char gridLine[48] = "GRID  --";
        char elevationLine[32] = "ELEV  --";
        const bool overMap = !ImGui::GetIO().WantCaptureMouse &&
            mouse.x > mapMin.x && mouse.x < mapMax.x &&
            mouse.y > mapMin.y && mouse.y < mapMax.y;
        XMVECTOR determinant;
        const XMMATRIX inverseViewProjection =
            XMMatrixInverse(&determinant, viewProjection);
        if (overMap && !XMVector4Equal(determinant, XMVectorZero())) {
            const float ndcX = (mouse.x / display.x) * 2.0f - 1.0f;
            const float ndcY = 1.0f - (mouse.y / display.y) * 2.0f;
            XMVECTOR nearWorld = XMVector4Transform(
                XMVectorSet(ndcX, ndcY, 0.0f, 1.0f), inverseViewProjection);
            XMVECTOR farWorld = XMVector4Transform(
                XMVectorSet(ndcX, ndcY, 1.0f, 1.0f), inverseViewProjection);
            const float nearW = XMVectorGetW(nearWorld);
            const float farW = XMVectorGetW(farWorld);
            if (std::abs(nearW) > 1e-6f && std::abs(farW) > 1e-6f) {
                XMFLOAT3 rayStart, rayEnd, impact;
                XMStoreFloat3(&rayStart, XMVectorScale(nearWorld, 1.0f / nearW));
                XMStoreFloat3(&rayEnd, XMVectorScale(farWorld, 1.0f / farW));
                if (HitTerrainSegment(rayStart, rayEnd, 0.0f, impact)) {
                    char grid[24];
                    FormatDeploymentGrid(impact.x, impact.z, grid, sizeof(grid));
                    std::snprintf(gridLine, sizeof(gridLine), "GRID  %s", grid);
                    std::snprintf(elevationLine, sizeof(elevationLine),
                                  "ELEV  %.0f M", (std::max)(0.0f, impact.y));
                }
            }
        }
        const ImVec2 gridSize = ImGui::CalcTextSize(gridLine);
        const ImVec2 elevationSize = ImGui::CalcTextSize(elevationLine);
        const float width = (std::max)(gridSize.x, elevationSize.x);
        const ImVec2 boxMax(mapMax.x - 14.0f, mapMax.y - 14.0f);
        const ImVec2 boxMin(boxMax.x - width - 20.0f,
                            boxMax.y - gridSize.y - elevationSize.y - 24.0f);
        foreground->AddRectFilled(boxMin, boxMax, IM_COL32(6, 10, 12, 160));
        foreground->AddRect(boxMin, boxMax, IM_COL32(120, 132, 124, 140), 0.0f, 0, 1.0f);
        foreground->AddText(ImVec2(boxMin.x + 10.0f, boxMin.y + 8.0f),
                            IM_COL32(220, 232, 224, 240), gridLine);
        foreground->AddText(
            ImVec2(boxMin.x + 10.0f, boxMin.y + 12.0f + gridSize.y),
            IM_COL32(220, 232, 224, 240), elevationLine);
    }

    // Acknowledging the briefing frees the left side for map interaction.
    // Drawn only when the level actually carries an objective: on a map with
    // neither a mast nor an aircraft there is nothing to brief, and a hardcoded
    // dossier would be a lie. Each objective the map authors writes its own
    // dossier, so the airfield does not inherit the relay operation's copy.
    if (showMissionBriefing) {
        // Start on the first visible frame, after the loading screen releases it.
        if (g_deploymentBriefingStartTime < 0.0)
            g_deploymentBriefingStartTime = ImGui::GetTime();
        const double elapsed = ImGui::GetTime() - g_deploymentBriefingStartTime;
        int lettersRemaining = static_cast<int>((std::min)(elapsed * 72.0, 100000.0));
        bool briefingTyping = false;
        const auto briefingText = [&](const char* text) {
            const int length = static_cast<int>(std::strlen(text));
            const int visible = (std::min)(lettersRemaining, length);
            briefingTyping |= visible < length;
            lettersRemaining -= visible;
            if (visible > 0)
                ImGui::TextWrapped("%.*s", visible, text);
        };
        ImGui::SetNextWindowPos(ImVec2(24.0f, 110.0f),
                                ImGuiCond_Always, ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(430.0f, (std::max)(180.0f, display.y - 146.0f)), ImGuiCond_Always);
        ImGui::Begin("Mission Briefing", nullptr, ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBackground |
            ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        // Reference reading, not a decision: the column is kept quiet so the
        // armory on the other side carries the screen. Dim section labels,
        // softened body copy, no amber -- amber marks the objective on the top
        // bar and the map, and repeating it here diluted that.
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        ImGui::TextColored(UITheme::kTextDim, "MISSION BRIEFING");
        ImGui::Dummy(ImVec2(0.0f, 10.0f));
        // Keep acknowledgement reachable even when the dossier needs scrolling.
        const float understoodHeight = 38.0f;
        ImGui::BeginChild("MissionBriefingScroll",
            ImVec2(0.0f, -(understoodHeight + ImGui::GetStyle().ItemSpacing.y)),
            false, ImGuiWindowFlags_NoBackground);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.70f, 0.74f, 0.71f, 1.0f));
        int paragraph = 0;
        const auto heading = [&](const char* text) {
            if (lettersRemaining <= 0) return;
            if (paragraph++ > 0) ImGui::Dummy(ImVec2(0.0f, 10.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, UITheme::kTextDim);
            briefingText(text);
            ImGui::PopStyleColor();
            ImGui::Dummy(ImVec2(0.0f, 1.0f));
        };
        if (standingTowers > 0) {
            heading("SITUATION");
            briefingText(
                "The garrison on this island is not fighting alone. A hardened relay "
                "mast on the ridge ties their patrols to the mainland battery. "
                "Every movement we make is called in the moment it is seen, and the "
                "guns answer within the minute.");
            // The top bar already states a single-mast objective; only a count
            // it cannot show earns a paragraph here.
            if (standingTowers > 1) {
                heading("MISSION");
                char mission[96];
                std::snprintf(mission, sizeof(mission),
                    "Destroy all %u communications towers.", standingTowers);
                briefingText(mission);
            }
            heading("EXECUTION");
            briefingText(
                "The lattice shrugs off small arms. Plant remote C4 on the mast and "
                "clear the base before you trigger it. Nothing lighter will bring "
                "the structure down.");
            heading("EXFILTRATION");
            briefingText(
                "Hold the island once the mast is down. Without the relay the "
                "battery is firing blind.");
        }
        // The airfield dossier. The deadline quoted here is the real one:
        // kObjectivePlaneHoldSeconds is what the runtime counts down before the
        // aircraft starts its roll, so the briefing and the simulation cannot
        // disagree about how long the player has.
        if (objectivePlanes > 0) {
            if (standingTowers > 0 && lettersRemaining > 0) {
                ImGui::Dummy(ImVec2(0.0f, 10.0f));
                ImGui::Separator();
                paragraph = 0;
            }
            heading("SITUATION");
            briefingText(
                "The strip on the north side of this island is not a civilian "
                "field. A transport is standing on the apron with its ground "
                "crew around it, loaded and fuelled, and the garrison is "
                "holding the perimeter until it is away. Whatever is in the "
                "hold leaves the theatre the moment those wheels come up.");
            if (objectivePlanes > 1) {
                heading("MISSION");
                char mission[96];
                std::snprintf(mission, sizeof(mission),
                    "Destroy all %u transport aircraft on the ground.", objectivePlanes);
                briefingText(mission);
            }
            heading("TIME ON TARGET");
            char deadline[512];
            std::snprintf(deadline, sizeof(deadline),
                "The aircraft begins its takeoff roll %d seconds after you are "
                "on the ground, and it is clear of the map about %d seconds "
                "after that. It can still be brought down in the air, but the "
                "moment it leaves the map the mission is a failure -- the "
                "clock is the objective as much as the airframe is.",
                static_cast<int>(kObjectivePlaneHoldSeconds),
                static_cast<int>(kObjectivePlaneTakeoffSeconds));
            briefingText(deadline);
            heading("EXECUTION");
            briefingText(
                "The airframe is thin-skinned but large: rifle fire will not "
                "put it down in the time you have. Bring the RPG and shoot it "
                "on the apron, or stick remote C4 to the fuselage and clear the "
                "blast before you trigger it. The fuel silo and the barrels on "
                "the car park will do the rest of the work if the aircraft is "
                "close enough to them.");
            heading("ENEMY FORCES");
            briefingText(
                "Emplaced turret over the approach, a watch tower on the "
                "perimeter and vehicle patrols working the apron roads. Expect "
                "the field to come alive the moment the first shot is heard, "
                "and expect the crew to try to get the aircraft moving early.");
            heading("EXFILTRATION");
            briefingText(
                "Break contact once the wreck is burning. There is nothing on "
                "this field worth holding after the transport is down.");
        }
        ImGui::PopStyleColor();
        // Enemy force estimate, counted from what is actually on the map.
        if (lettersRemaining > 0) {
            uint32_t infantry = 0;
            for (const auto& bandit : g_bandits)
                if (bandit && !bandit->Dead() &&
                    bandit->faction == Faction::Bandit && !bandit->turretGunner)
                    ++infantry;
            uint32_t vehicles = static_cast<uint32_t>(g_levelHumveeSpawns.size());
            if (g_levelPatrolBoatEnabled && g_boatModel &&
                !g_game.vehicles.boatDead && !g_game.vehicles.boatSunk)
                ++vehicles;
            uint32_t airDefence = 0;
            for (const auto& turret : g_game.vehicles.aaTurrets)
                if (turret.Active()) ++airDefence;
            ImGui::Dummy(ImVec2(0.0f, 18.0f));
            ImGui::TextColored(UITheme::kTextDim, "ENEMY FORCE ESTIMATE");
            ImGui::Dummy(ImVec2(0.0f, 4.0f));
            // Three equal columns, figure over label. The figures are the one
            // thing on this side worth a glance mid-plan, so they get the
            // display face; everything else stays body size.
            ImFont* figureFont = g_menuTitleFont ? g_menuTitleFont : ImGui::GetFont();
            constexpr float kFigureSize = 26.0f;
            const float columnWidth = ImGui::GetContentRegionAvail().x / 3.0f;
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            const float figureHeight =
                figureFont->CalcTextSizeA(kFigureSize, FLT_MAX, 0.0f, "0").y;
            ImDrawList* draw = ImGui::GetWindowDrawList();
            const auto estimate = [&](int column, const char* label, uint32_t count) {
                char figure[16];
                std::snprintf(figure, sizeof(figure), "%u", count);
                const float x = origin.x + columnWidth * column;
                draw->AddText(figureFont, kFigureSize, ImVec2(x, origin.y),
                              ImGui::GetColorU32(UITheme::kText), figure);
                draw->AddText(ImVec2(x, origin.y + figureHeight + 2.0f),
                              ImGui::GetColorU32(UITheme::kTextDim), label);
            };
            estimate(0, "INFANTRY", infantry);
            estimate(1, "VEHICLES", vehicles);
            estimate(2, "AIR DEFENCE", airDefence);
            ImGui::Dummy(ImVec2(columnWidth * 3.0f,
                                figureHeight + ImGui::GetTextLineHeight() + 4.0f));
        }
        ImGui::EndChild();
        g_briefingTypingAudio.SetLoop(briefingTyping, 0.8f);
        if (ImGui::Button("UNDERSTOOD",
                ImVec2(ImGui::GetContentRegionAvail().x, understoodHeight))) {
            g_deploymentBriefingUnderstood = true;
            g_briefingTypingAudio.StopLoop();
        }
        ImGui::End();
    } else {
        g_briefingTypingAudio.StopLoop();
    }

    MissionLoadout& loadout = g_game.mission.Loadout();
    bool deployPressed = false;
    const auto renderArmory = [&] {
    ImGui::SetNextWindowPos(ImVec2(display.x - 24.0f, 152.0f),
                            ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    const float panelHeight = (std::max)(180.0f, display.y - 188.0f);
    ImGui::SetNextWindowSize(ImVec2(430.0f, panelHeight), ImGuiCond_Always);
    ImGui::Begin("Deployment Planning", nullptr, ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBackground);
    // Hierarchy on this column: the armory first, DEPLOY second, everything
    // else quiet. The chosen LZ is already on the top bar and on the DEPLOY
    // button, so the header only speaks up when no zone is picked yet.
    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    ImGui::TextColored(UITheme::kTextDim, "MISSION PREPARATION");
    if (g_selectedDeploymentZone < 0 ||
        g_selectedDeploymentZone >= static_cast<int>(g_deploymentZones.size())) {
        ImGui::SameLine();
        const char* prompt = "SELECT A LANDING ZONE";
        ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x -
                             ImGui::CalcTextSize(prompt).x);
        ImGui::TextColored(UITheme::kWarning, "%s", prompt);
    }
    ImGui::Dummy(ImVec2(0.0f, 10.0f));

    // DEPLOY stays pinned to the foot of the panel, the way an ops screen keeps
    // its commit action in one place; everything above it scrolls in a child.
    const bool hostingSession = MultiplayerActive() &&
        g_netSession.CurrentRole() == net::Role::Host;
    const float deployAreaHeight = 76.0f + (hostingSession ? 50.0f : 0.0f) +
        (scene.player.godMode ? 26.0f : 0.0f);
    ImGui::BeginChild("DeploymentPlanScroll", ImVec2(0.0f, -deployAreaHeight),
                      false, ImGuiWindowFlags_NoBackground);

    // Every section below is a dropdown. The panel carries the armory, the
    // conditions, the insertion and the intel on one 430 px column, which is far
    // more than fits on screen at once -- collapsing them means the player opens
    // the one department they are actually shopping in instead of scrolling past
    // the other four. Armory and insertion default open because a plan is not
    // valid without them; the rest start closed.
    //
    // Headers are secondary: no plate, dim label, a hairline underneath. The
    // armory above them is drawn as the page's headline and the accent belongs
    // to DEPLOY, so these must read as the quiet extras they are.
    const auto deploySection = [&](const char* label, bool defaultOpen = false) {
        ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, UITheme::kControlHover);
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, UITheme::kControlHeld);
        ImGui::PushStyleColor(ImGuiCol_Text, UITheme::kTextDim);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.0f);
        const bool open = ImGui::CollapsingHeader(
            label, defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0);
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor(4);
        const ImVec2 lineMin(ImGui::GetItemRectMin().x, ImGui::GetItemRectMax().y);
        ImGui::GetWindowDrawList()->AddLine(
            lineMin, ImVec2(ImGui::GetItemRectMax().x, lineMin.y),
            IM_COL32(120, 132, 124, 60), 1.0f);
        return open;
    };

    // Render diagnostics. Drawn from the DEBUG section at the foot of the panel
    // rather than above the armory, where it was the first thing the player met.
    const auto drawRenderDiagnostics = [&]() {
        ImGui::Checkbox("Hide water", &g_deploymentDebugHideWater);
        ImGui::Checkbox("Hide fog and clouds",
                        &g_deploymentDebugHideAtmosphere);
        ImGui::Checkbox("Force Forward renderer",
                        &g_deploymentDebugForceForward);
        if (ImGui::Checkbox("Water depth diagnostic",
                            &g_deploymentDebugWaterDepth) &&
            g_deploymentDebugWaterDepth) {
            g_deploymentDebugHideWater = false;
        }
        ImGui::Checkbox("Show terrain coverage guides",
                        &g_deploymentDebugCoverageGuides);
        ImGui::Checkbox("Hide ambient occlusion",
                        &g_deploymentDebugHideAO);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Skips GTAO and contact shadows while planning.\n"
                "Flattens the shading so terrain shape and zone markers\n"
                "read clearly from above. Gameplay AO is untouched.");
        ImGui::Checkbox("Draw grass on the overview",
                        &g_deploymentDebugShowGrass);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "The planning camera normally skips the grass field.\n"
                "On, the draw distance is raised to cover the whole\n"
                "field from up here -- every cell submitted at once --\n"
                "so the blades' footprint and their terrain-material\n"
                "boundary can be read from above. Costs frames.");

        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        // Unlike the toggles above, this one is not planning-only: it writes the
        // scene setting the renderer reads everywhere, so it stays off into the
        // mission until it is turned back on. Off falls back to the cascades.
        if (scene.enableShadows) {
            ImGui::Checkbox("Virtual shadow maps", &scene.virtualShadowMaps);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Off: cascade shadow maps take the sun back.\n"
                    "This is a scene setting, not a planning-only one --\n"
                    "it carries into the mission.");
        } else {
            ImGui::TextDisabled("Shadows disabled in scene settings");
        }
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        // Page budget. Not a quality slider: a page that is not resident is not
        // a softer shadow, it is no shadow at all, so this is here to answer
        // "is that missing shadow a coverage hole?" on the planning camera,
        // which sits far enough out to run the atlas dry. Capacity is the
        // physical atlas (16 slots), so it cannot ask for pages that do not exist.
        if (scene.enableShadows && scene.virtualShadowMaps) {
            ImGui::SliderInt("Virtual shadow pages",
                             &scene.virtualShadowPageBudget,
                             1, static_cast<int>(VirtualShadows::Capacity));
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "How many of the 16 atlas pages the sun may hold.\n"
                    "The first 4 pages are coarse coverage around the viewer;\n"
                    "page 5 adds fine coverage and page 6 adds middle coverage.\n"
                    "Below 4 pages, coverage is not guaranteed.\n"
                    "Below coverage, unmapped ground reads as unshadowed.");
            if (g_vsmUnavailable)
                ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.35f, 1.0f),
                                   "VSM unavailable: cascades in use");
            else
                ImGui::Text("Pages: %u resident, %u refreshed, %u reused",
                            g_vsmResident, g_vsmRefreshed, g_vsmReused);
            ImGui::Checkbox("Show virtual shadow pages in world",
                            &scene.showVirtualShadowPages);
        } else {
            ImGui::TextDisabled("Virtual shadow pages: VSM off");
        }
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        ImGui::Checkbox("Hide all UI (clean screenshot)",
                        &g_deploymentDebugHideUI);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Drops every ImGui element -- this panel included.\n"
                "The windows are still there invisibly and still take\n"
                "clicks where they sat, so turn the map from the middle\n"
                "of the screen, clear of the two side columns.\n"
                "F9 brings the UI back (it is the only way back).");
        if (ImGui::Button("Reset diagnostics")) {
            g_deploymentDebugHideWater = false;
            g_deploymentDebugHideAtmosphere = false;
            g_deploymentDebugForceForward = false;
            g_deploymentDebugWaterDepth = false;
            g_deploymentDebugCoverageGuides = false;
            g_deploymentDebugHideAO = false;
            scene.virtualShadowPageBudget =
                Scene::kDefaultVirtualShadowPageBudget;
            scene.showVirtualShadowPages = false;
            g_deploymentDebugHideUI = false;
            g_deploymentDebugShowGrass = false;
        }

        const TerrainRendererDX12::Params diagnosticTerrain =
            CurrentTerrainParams();
        const float coverage = diagnosticTerrain.tilesX *
            diagnosticTerrain.tileSize * 0.5f;
        const float vertexSpacing = diagnosticTerrain.tileSize / 8.0f;
        const double maximumSurfaceTriangles =
            static_cast<double>(diagnosticTerrain.tilesX) *
            diagnosticTerrain.tilesZ * 128.0;
        ImGui::Text("Terrain: %ux%u tiles, %.1f m each",
                    diagnosticTerrain.tilesX, diagnosticTerrain.tilesZ,
                    diagnosticTerrain.tileSize);
        ImGui::Text("Uniform LOD 0: %.2f m vertices, +/-%.0f m coverage",
                    vertexSpacing, coverage);
        ImGui::Text("Maximum surface geometry: %.2f M triangles",
                    maximumSurfaceTriangles / 1000000.0);
        ImGui::Text("Camera far plane: %.0f m", scene.EffectiveCameraFarPlane());
        ImGui::TextDisabled(
            "Depth debug: green=submerged, red=dry, magenta=no depth.");
        ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.25f, 1.0f),
                           "Yellow: waterline (43 m reference)");
        ImGui::TextColored(ImVec4(0.25f, 0.88f, 1.0f, 1.0f),
                           "Cyan: full-depth seabed (88 m reference)");
        ImGui::TextColored(ImVec4(1.0f, 0.32f, 0.86f, 1.0f),
                           "Pink: actual terrain grid edge");
    };

    static constexpr const char* weaponNames[MissionLoadout::kWeaponCount] = {
        "AK47", "Remington 870", "RPG-7", "R700 Sniper",
        "ARC Laser Cutter", "Remote C4", "M2 Flamethrower",
        "Mako Harpoon Gun", "R700 Suppressed", "M4A1", "AK-74", "M9", "Kriss Vector",
        "M1 Garand", "Missile Target Designator"
    };

    // ---- Armory storefront -------------------------------------------------
    // Three combo boxes used to hand out every weapon, grenade and gear item
    // for free, which left the career balance with nothing to spend on. This is
    // that same picker rebuilt as a shop front: one column of product rows per
    // department, each showing what the item does and what it costs, with the
    // wallet pinned above them.
    //
    // Buy and equip are deliberately the same click. A two-step shop (buy, then
    // go back and equip) is a second inventory screen to build and a second
    // place for the loadout to disagree with what was paid for; here a row is
    // either owned-and-equippable or a price you can afford, and pressing it
    // does whichever applies.
    // The armory is the headline of this column -- always open, titled in the
    // display face with the wallet beside it -- so it is the first thing read
    // after the map. It used to be one collapsing header among four.
    {
    // Loadout card: one tile per slot. A tile shows what the slot carries;
    // clicking it opens that slot's full-screen picker (RenderLoadoutPicker),
    // where buying, equipping and rail attachments happen.
    {
        char balanceText[32];
        MoneySystem::Format(balanceText, sizeof(balanceText),
                            g_game.money.Balance());
        ImFont* titleFont = g_menuTitleFont ? g_menuTitleFont : ImGui::GetFont();
        constexpr float kTitleSize = 34.0f;
        constexpr float kBalanceSize = 22.0f;
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const float width = ImGui::GetContentRegionAvail().x;
        const ImVec2 titleSize =
            titleFont->CalcTextSizeA(kTitleSize, FLT_MAX, 0.0f, "ARMORY");
        const ImVec2 balanceSize =
            titleFont->CalcTextSizeA(kBalanceSize, FLT_MAX, 0.0f, balanceText);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddText(titleFont, kTitleSize, origin, IM_COL32(240, 244, 240, 255),
                      "ARMORY");
        // Wallet sits on the title's baseline, right-aligned.
        const float baseline = origin.y + titleSize.y;
        draw->AddText(titleFont, kBalanceSize,
                      ImVec2(origin.x + width - balanceSize.x,
                             baseline - balanceSize.y - 2.0f),
                      IM_COL32(240, 244, 240, 255), balanceText);
        const char* fundsLabel = "FUNDS";
        const ImVec2 fundsSize = ImGui::CalcTextSize(fundsLabel);
        draw->AddText(ImVec2(origin.x + width - balanceSize.x - fundsSize.x - 10.0f,
                             baseline - fundsSize.y - 5.0f),
                      ImGui::GetColorU32(UITheme::kTextDim), fundsLabel);
        ImGui::Dummy(ImVec2(width, titleSize.y + 4.0f));
        draw->AddLine(ImVec2(origin.x, origin.y + titleSize.y + 2.0f),
                      ImVec2(origin.x + width, origin.y + titleSize.y + 2.0f),
                      ImGui::GetColorU32(UITheme::kAccentDim), 1.5f);
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
    }
    {
        const float width = ImGui::GetContentRegionAvail().x;
        for (int slotIndex = 0; slotIndex < 2; ++slotIndex)
            DrawLoadoutWeaponTile(slotIndex,
                loadout.weapons[static_cast<size_t>(slotIndex)], width);
        DrawLoadoutKitTiles(loadout, width);
    }
    // C4 is demolition kit rather than a weapon pick, so it is carried on every
    // mission without spending a slot or a cent -- otherwise a player who chose
    // two rifles would have no way to take down a demolition objective.
    ImGui::TextDisabled("Remote C4 is issued free on every mission.");
    ImGui::Dummy(ImVec2(0.0f, 14.0f));
    }

    // Stocking the development weapons is a debug switch, not a purchase, so it
    // is drawn from the DEBUG section. Flipping it repopulates the armory rows
    // next frame.
    const auto drawDebugWeaponToggle = [&]() {
        bool debugWeapons = GunModel::DebugWeaponsEnabled();
        if (ImGui::Checkbox("Debug weapons (laser cutter, flamethrower, harpoon)",
                            &debugWeapons))
            GunModel::SetDebugWeaponsEnabled(debugWeapons);
        ImGui::TextDisabled(debugWeapons
            ? "Laser cutter, flamethrower and harpoon gun are stocked."
            : "Development weapons are hidden from the armory and weapon cycle.");
    };
    // God mode is a cheat, so it stays in the DEBUG section at the foot of the
    // panel. The enemy-damage dial is not -- it is a real plan choice that now
    // decides what the run pays, so it has its own section up with the rest of
    // the loadout, where a player will actually meet it.
    const auto drawGodModeControls = [&]() {
    // Per-run choice rather than a launcher-level mode. Applied straight to the
    // live player state because StartLevelOne has already run by the time this
    // screen is up -- its godMode parameter set the value this toggle edits.
    //
    // God mode is not only invulnerability: PlayerState::AmmoEnforced() is tied
    // to it, so turning it on also disables magazines, reserves and reloading.
    // That is why the label says supplies rather than just damage.
    // In a session god mode belongs to the host and a client mirrors it every
    // frame, so a client's checkbox would flip back on the next tick. Disabled
    // rather than hidden, so the player can still see which way it is set.
    const bool setByHost =
        g_netSession.CurrentRole() == net::Role::Client;
    ImGui::BeginDisabled(setByHost);
    if (ImGui::Checkbox("God mode (no damage, unlimited ammo)",
                        &scene.player.godMode)) {
        // Coming back from god mode leaves the magazines in whatever state the
        // unenforced path left them, so restock to a clean loadout.
        if (!scene.player.godMode) scene.player.RestoreAmmo();
    }
    ImGui::EndDisabled();
    if (setByHost)
        ImGui::TextDisabled("Set by the host for everyone in the session.");
    if (scene.player.godMode)
        ImGui::TextDisabled(
            "All weapons stay available and ammo is not tracked.");
    else
        ImGui::TextDisabled(
            "Ammo is limited to your two weapons. Reload with R.");
    ImGui::TextDisabled("Rewards are capped at x1.00 while god mode is on.");
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    };

    const auto drawDifficultyControls = [&]() {
    // Enemy lethality. Scales incoming fire, grenades and molotovs only --
    // falls, crashes and your own grenades are unaffected, so this stays a
    // statement about the enemy rather than a global fragility slider.
    ImGui::SetCursorPosX(45.0f);
    ImGui::SetNextItemWidth(340.0f);
    ImGui::SliderFloat("##DeploymentEnemyDamage",
                       &scene.enemyDamageMultiplier, 0.25f, 3.0f,
                       "ENEMY DAMAGE  x%.2f");
    {
        // Quote the practical consequence rather than the raw factor: rifle
        // chip damage is the number the player actually feels, and shots-to-kill
        // is what the multiplier really changes.
        const float perShot = 2.4f * scene.enemyDamageMultiplier;
        const int shots = perShot > 0.0f
            ? static_cast<int>(std::ceil(scene.player.maxHealth / perShot))
            : 0;
        ImGui::SetCursorPosX(45.0f);
        if (scene.player.godMode)
            ImGui::TextColored(UITheme::kTextDim,
                               "No effect on you while god mode is on.");
        else
            ImGui::TextColored(UITheme::kTextDim,
                "%.1f damage per rifle hit -- %d to drop you from full health.",
                perShot, shots);

        // The other half of the bargain, and the reason this is not a debug
        // switch: a harder run is worth more. Coloured only when it is actually
        // above the tuned baseline, so the default reads as neutral rather than
        // as a bonus the player is already collecting.
        const float rewards = CurrentRewardMultiplier();
        ImGui::SetCursorPosX(45.0f);
        ImGui::TextColored(rewards > 1.0f ? UITheme::kWarning : UITheme::kTextDim,
                           "Cash and experience  x%.2f", rewards);
    }
    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    };


    // Time of day. Applied live as it is picked rather than waiting for DEPLOY,
    // so the planning fly-through shows the light the run will actually be
    // fought in -- picking Night and then dropping into daylight would make the
    // choice meaningless on the one screen that can preview it.
    // Volumetric fog for the selected time. Edits apply live for the same reason
    // the time buttons do: this is the one screen that previews the run's light,
    // so the fog has to be tunable against what is actually on screen. Kept as a
    // lambda so it can be drawn down in the debug section, where per-parameter
    // scattering sliders belong, rather than in the middle of the weather pick.
    const auto drawVolumetricFogControls = [&]() {
        char fogHeader[64];
        std::snprintf(fogHeader, sizeof(fogHeader), "VOLUMETRIC FOG (%s)",
                      TimeOfDayName(g_selectedTimeOfDay));
        ImGui::SeparatorText(fogHeader);

        VolumetricFogSettings& fog = VolumetricFogFor(g_selectedTimeOfDay);
        bool fogChanged =
            ImGui::Checkbox("Enable Volumetric Fog##TodFog", &fog.enabled);
        if (fog.enabled) {
            ImGui::SetNextItemWidth(-1.0f);
            fogChanged |= ImGui::DragFloat(
                "Density##TodFog", &fog.density,
                0.0001f, 0.0001f, 0.05f, "%.4f");
            ImGui::SetNextItemWidth(-1.0f);
            fogChanged |= ImGui::SliderFloat(
                "Anisotropy##TodFog", &fog.anisotropy,
                0.0f, 0.9f, "%.2f");
            ImGui::SetNextItemWidth(-1.0f);
            fogChanged |= ImGui::SliderFloat(
                "Height Falloff##TodFog", &fog.heightFalloff,
                0.01f, 0.25f, "%.3f");
            ImGui::SetNextItemWidth(-1.0f);
            fogChanged |= ImGui::DragFloat(
                "Base Height##TodFog", &fog.baseHeight,
                0.1f, -5.0f, 30.0f, "%.1f m");
            ImGui::SetNextItemWidth(-1.0f);
            fogChanged |= ImGui::DragFloat(
                "Distance##TodFog", &fog.distance,
                5.0f, 20.0f, scene.cameraFar, "%.0f m");
            fogChanged |= ImGui::ColorEdit3("Tint##TodFog", &fog.tint.x);
        }

        char fogReset[64];
        std::snprintf(fogReset, sizeof(fogReset), "Reset %s fog",
                      TimeOfDayName(g_selectedTimeOfDay));
        if (ImGui::Button(fogReset)) {
            fog = MakeDefaultVolumetricFogSettings(g_selectedTimeOfDay);
            fogChanged = true;
        }
        if (fogChanged) {
            scene.weatherState = WeatherState::Custom;
            ApplyVolumetricFogSettings(fog);
            ApplyLiveWeatherState(WeatherState::Custom);
        }
    };
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    // Insertion method and time, weather and difficulty are picked from the
    // header dropdowns. The panel keeps only the armory, the marines riding
    // along and the randomizer; the rest sits behind the dev tools.
    if (deploySection("SQUAD", true)) {

    // Door choice, a dev control now. Only the two helicopter runs have cabin
    // seats, so the row is hidden on a boat insertion.
    if (g_deploymentDevTools &&
        (g_playerInsertionChoice == LevelInsertionMode::Helicopter ||
         g_playerInsertionChoice == LevelInsertionMode::FastRappel)) {
        ImGui::SetCursorPosX(45.0f);
        ImGui::TextDisabled("SEAT");
        const auto seatButton = [&](const char* label, bool leftSeat,
                                    bool sameLine) {
            const bool selected = g_playerRidesLeftSeat == leftSeat;
            if (selected) {
                ImGui::PushStyleColor(
                    ImGuiCol_Button, ImVec4(0.12f, 0.48f, 0.22f, 1.0f));
                ImGui::PushStyleColor(
                    ImGuiCol_ButtonHovered, ImVec4(0.16f, 0.58f, 0.28f, 1.0f));
            }
            if (sameLine) ImGui::SameLine();
            else ImGui::SetCursorPosX(45.0f);
            if (ImGui::Button(label, ImVec2(166.0f, 30.0f)))
                g_playerRidesLeftSeat = leftSeat;
            if (selected) ImGui::PopStyleColor(2);
        };
        seatButton("LEFT DOOR", true, false);
        seatButton("RIGHT DOOR", false, true);
        ImGui::Dummy(ImVec2(0.0f, 5.0f));
    }

    // Squad size. Sits under the insertion controls because that is what it
    // depends on: the marines ride the transport chosen above, and they only
    // reach the ground if it does.
    ImGui::SetCursorPosX(45.0f);
    ImGui::TextDisabled("MARINE SQUAD");
    // Greyed out rather than hidden when the ally mesh is missing: a slider that
    // vanishes reads as a feature that does not exist, where a dead one plus the
    // line below says what is actually wrong.
    const bool marinesAvailable = g_marineModel.valid;
    // The slider stops where the wallet does, so the player cannot dial up a
    // squad they will only be told about at DEPLOY. Balance is re-read every
    // frame because the storefront above can spend it after this point.
    const int prepaidMarines = g_replayPlanActive
        ? g_lastDeployment.marines : 0;
    const int affordableMarines = (std::min)(kMaxDeploymentMarines,
        prepaidMarines + static_cast<int>(
            g_game.money.Balance() / kDeploymentMarinePrice));
    // Clamp before drawing, not after: an armory purchase made after the squad
    // was picked can put the count out of reach, and charging for marines the
    // player can no longer pay for is the one outcome this must never allow.
    g_deploymentMarineCount =
        (std::min)(g_deploymentMarineCount, affordableMarines);
    ImGui::BeginDisabled(!marinesAvailable || affordableMarines <= 0);
    ImGui::SetCursorPosX(45.0f);
    ImGui::SetNextItemWidth(340.0f);
    // Neutral grab: a green block here competed with DEPLOY for the accent.
    ImGui::PushStyleColor(ImGuiCol_SliderGrab, ImVec4(0.42f, 0.47f, 0.44f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, UITheme::kText);
    ImGui::SliderInt("##DeploymentMarines", &g_deploymentMarineCount,
                     0, (std::max)(1, affordableMarines),
                     g_deploymentMarineCount == 1 ? "%d marine"
                                                  : "%d marines");
    ImGui::PopStyleColor(2);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Marines loaded aboard your transport, $2,000 each.\n"
            "Charged when you deploy. A restart reuses the paid squad;\n"
            "only additional marines cost more on Change Plan.\n"
            "They only reach the ground if the transport does: a\n"
            "downed helicopter or a sunk boat takes the squad with it.");
    ImGui::EndDisabled();
    if (!marinesAvailable) {
        ImGui::SetCursorPosX(45.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.2f, 1.0f),
                           "Marine asset unavailable");
    } else if (affordableMarines <= 0) {
        ImGui::SetCursorPosX(45.0f);
        ImGui::TextColored(ImVec4(0.75f, 0.32f, 0.28f, 1.0f),
                           "Cannot afford a marine ($2,000 each)");
    } else if (g_deploymentMarineCount > prepaidMarines) {
        // Quote the total rather than the unit price: the decision being made
        // here is how big a cheque to write, not what one marine goes for.
        char squadCost[32];
        MoneySystem::Format(squadCost, sizeof(squadCost),
                            (g_deploymentMarineCount - prepaidMarines) *
                            kDeploymentMarinePrice);
        ImGui::SetCursorPosX(45.0f);
        ImGui::TextColored(UITheme::kWarning, "%s on deploy", squadCost);
        ImGui::SetCursorPosX(45.0f);
        // The risk is the mechanic, so state it on the screen rather than
        // leaving it to a tooltip nobody hovers.
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.25f, 1.0f),
                           "Lost with the transport if it goes down");
    }
    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    }

    // One roll for time, weather, fog density and the enemy layout. The
    // controls it drives stay editable afterwards -- a roll is a starting
    // point, not a lock. Below the squad: it is the least-used control here.
    if (deploySection("RANDOMIZE")) {
    ImGui::SetCursorPosX(45.0f);
    if (ImGui::Button("RANDOMIZE CONDITIONS", ImVec2(340.0f, 34.0f)))
        g_lastDeploymentRollSeed = RandomizeDeployment();
    if (g_deploymentDevTools && g_lastDeploymentRollSeed != 0)
        ImGui::TextDisabled("Roll seed %u -- reuse it to replay this layout.",
                            g_lastDeploymentRollSeed);
    else
        ImGui::TextDisabled("Rolls time, weather, fog and enemy positions.");
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    }

    if (g_deploymentDevTools && deploySection("OBJECTIVES")) {
        if (standingTowers > 0) {
            ImGui::TextColored(UITheme::kTextDim, "PRIMARY");
            ImGui::TextColored(UITheme::kText,
                standingTowers == 1 ? "Destroy the communications tower"
                                    : "Destroy all communications towers");
            ImGui::Dummy(ImVec2(0.0f, 5.0f));
        }
        ImGui::TextColored(UITheme::kTextDim, "OPTIONAL");
        ImGui::TextDisabled("Use both selected weapons");
        ImGui::TextDisabled("Throw your selected grenade");
        ImGui::TextDisabled("Cause at least %u destruction events",
                            MissionSystem::kDemolitionObjectiveEvents);
        ImGui::Dummy(ImVec2(0.0f, 5.0f));
    }

    // Enemy intel + the scatter control. Lives on the planning screen rather
    // than only in the debug panel because this is the one moment where seeing
    // the layout and re-rolling it actually changes a decision -- once DEPLOY is
    // pressed the run has started.
    if (g_deploymentDevTools && deploySection("ENEMY INTEL")) {
    uint32_t liveBandits = 0;
    for (const auto& bandit : g_bandits)
        if (bandit && !bandit->Dead() && bandit->faction == Faction::Bandit)
            ++liveBandits;
    // The squad spawns during the async load, which is still running when this
    // screen first opens on a cold start. Report that rather than showing a
    // confident "0 hostiles" that is merely early.
    const bool squadReady = g_banditLoaded && !g_game.loading.Active();
    if (squadReady)
        ImGui::Text("%u hostile%s on the island", liveBandits,
                    liveBandits == 1 ? "" : "s");
    else
        ImGui::TextDisabled("Locating hostiles...");
    ImGui::Checkbox("Show hostiles on map", &g_showEnemyDotsOnDeployScreen);
    // Only offer the friendly toggle when there is a squad to show. A level
    // with no marines would otherwise carry a checkbox that changes nothing.
    if (const size_t liveMarines = LiveMarineCount()) {
        ImGui::Text("%zu friendly marine%s", liveMarines,
                    liveMarines == 1 ? "" : "s");
        ImGui::Checkbox("Show marines on map", &g_showAllyDotsOnDeployScreen);
    }
    // Randomising before the squad exists would move nothing and still report a
    // seed, which reads as a scatter that silently did nothing.
    ImGui::BeginDisabled(!squadReady || !g_navigation.Ready());
    ImGui::SetCursorPosX(45.0f);
    if (ImGui::Button("RANDOMISE ENEMY POSITIONS", ImVec2(340.0f, 32.0f))) {
        // Force the scatter for this press regardless of the persistent test
        // toggle: the button is an explicit request, not a mode.
        const bool wasEnabled = g_scatterEnemiesOnNavmesh;
        g_scatterEnemiesOnNavmesh = true;
        ScatterEnemiesOnNavmesh();
        g_scatterEnemiesOnNavmesh = wasEnabled;
    }
    ImGui::EndDisabled();
    if (g_deploymentDevTools && g_scatterEnemiesLastSeed != 0)
        ImGui::TextDisabled("Last layout seed: %u", g_scatterEnemiesLastSeed);
    if (squadReady && !g_navigation.Ready())
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.25f, 1.0f),
                           "Navmesh unavailable, cannot randomise");
    ImGui::Dummy(ImVec2(0.0f, 5.0f));
    }

    const bool weaponsReady = loadout.Valid() &&
        GunModel::WeaponLoaded(loadout.weapons[0]) &&
        GunModel::WeaponLoaded(loadout.weapons[1]);
    if (!weaponsReady)
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.2f, 1.0f),
                           "Selected weapon asset is unavailable");
    // Fire support. Outside the DEPLOY disable block on purpose: calling in a
    // strike does not need a landing zone picked or a valid loadout, and
    // greying it out with DEPLOY would read as though the two were one action.
    if (g_deploymentDevTools && deploySection("FIRE SUPPORT")) {
    ImGui::SetCursorPosX(45.0f);
    if (g_missileStrikeArmed) {
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(150, 45, 30, 255));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(190, 60, 40, 255));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(210, 70, 45, 255));
        if (ImGui::Button("CANCEL STRIKE", ImVec2(340.0f, 32.0f)))
            g_missileStrikeArmed = false;
        ImGui::PopStyleColor(3);
        ImGui::SetCursorPosX(45.0f);
        ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.25f, 1.0f),
                           "Click the map to set the impact point");
    } else {
        if (ImGui::Button("LAUNCH MISSILE", ImVec2(340.0f, 32.0f)))
            g_missileStrikeArmed = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Call in an impact-fused round. Arms targeting; the next\n"
                "click on the map is the impact point, not a zone pick.");
    }

    // Unaimed counterpart to LAUNCH MISSILE: four rounds walked across random
    // dry land. Left enabled while a strike is armed -- the two do not share
    // the map click, so arming one does not block the other.
    ImGui::Dummy(ImVec2(0.0f, 2.0f));
    ImGui::SetCursorPosX(45.0f);
    {
        // Re-arming mid-salvo would throw away the rounds still queued, so the
        // button holds until the last one is away. That is a fraction of a
        // second at the authored interval, not a cooldown.
        const bool firing = WartornBarrageActive();
        ImGui::BeginDisabled(firing);
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(95, 60, 25, 255));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(130, 82, 34, 255));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, IM_COL32(155, 98, 40, 255));
        if (ImGui::Button("RANDOM WARTORN", ImVec2(340.0f, 32.0f)))
            QueueWartornBarrage();
        ImGui::PopStyleColor(3);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Walk a four-round barrage across the island at random.\n"
                "Same round and same blast scale as a called strike, but\n"
                "the impact points are picked for you -- spread apart and\n"
                "kept on dry land.");
    }

    // Ongoing bombardment. The two buttons above are single events the player
    // fires from this screen; this is a mode that outlives it, so it is a
    // checkbox rather than a button and it survives the deploy.
    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    ImGui::SetCursorPosX(45.0f);
    ImGui::Checkbox("ONGOING BOMBARDMENT", &g_bombardmentEnabled);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Keeps shelling the island for the whole mission: one round\n"
            "on a random point every interval, starting once you are on\n"
            "the ground. Rounds are aimed without regard for where you\n"
            "are -- each one is marked on the ground and whistles as it\n"
            "falls, and you have about four seconds to leave the ring.");
    if (g_bombardmentEnabled) {
        ImGui::SetCursorPosX(45.0f);
        ImGui::SetNextItemWidth(340.0f);
        // Floor of 4 s rather than 0: the round is in the air for 4 s, and an
        // interval under that would put a second one up before the first has
        // landed, which the single inbound marker cannot describe.
        ImGui::SliderFloat("##BombardmentInterval", &g_bombardmentInterval,
                           4.0f, 60.0f, "Every %.0f s");
        ImGui::SetCursorPosX(45.0f);
        // The interval alone does not say how dangerous this is; the lethal
        // ring is the other half, and it moves with the blast slider below.
        ImGui::TextColored(
            ImVec4(1.0f, 0.62f, 0.25f, 1.0f),
            "Lethal ring %.0f m -- rounds do not avoid you",
            scene.grenadeEnemyRadius * scene.missileBlastScale);
    }

    // Blast size. Applies to the called-in strike only, so widening it does not
    // turn every hand grenade into an airstrike. Editable while armed -- the
    // scale is read at detonation, so a change still lands on a round already
    // in flight.
    ImGui::Dummy(ImVec2(0.0f, 2.0f));
    ImGui::SetCursorPosX(45.0f);
    ImGui::SetNextItemWidth(340.0f);
    ImGui::SliderFloat("##MissileBlastScale", &scene.missileBlastScale,
                       0.5f, 8.0f, "Blast x%.2f");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Scales the strike's blast: debris, enemy reach, explosion\n"
            "FX and the terrain crater all widen together.");
    {
        // The multiplier alone says nothing about how much ground the strike
        // covers, so quote the radius that actually kills.
        const float lethalRadius =
            scene.grenadeEnemyRadius * scene.missileBlastScale;
        ImGui::SetCursorPosX(45.0f);
        ImGui::TextColored(ImVec4(0.62f, 0.66f, 0.72f, 1.0f),
                           "%.1f m lethal radius", lethalRadius);
    }
    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    }

    // Everything that is a development switch rather than a plan choice, in one
    // place at the foot of the panel: render diagnostics, god mode, the fog
    // tuning sliders and the development weapon stock. Closed by default, and
    // last so it never sits between the player and the DEPLOY button.
    if (g_deploymentDevTools && deploySection("DEBUG")) {
        if (ImGui::CollapsingHeader("Render diagnostics"))
            drawRenderDiagnostics();
        if (ImGui::CollapsingHeader("Enemy damage (fine)"))
            drawDifficultyControls();
        if (ImGui::CollapsingHeader("God mode"))
            drawGodModeControls();
        if (ImGui::CollapsingHeader("Volumetric fog"))
            drawVolumetricFogControls();
        if (ImGui::CollapsingHeader("Weapon stock"))
            drawDebugWeaponToggle();
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
    }
    ImGui::EndChild();

    // God mode lives under the hidden dev tools, so say when it is still on:
    // it caps the payout and the player would otherwise never know why.
    if (scene.player.godMode)
        ImGui::TextColored(UITheme::kWarning, "GOD MODE ACTIVE  //  REWARDS CAPPED");

    ImGui::BeginDisabled(g_selectedDeploymentZone < 0 || !weaponsReady);
    // The one action this whole screen exists to reach, so it carries the accent
    // colour. Every other control here only edits the plan.
    // Second in the column's hierarchy, after the armory: a deep green plate
    // with the accent on its border and label, lighting to the full accent on
    // hover. A solid accent block outshouted the loadout it commits.
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.05f, 0.20f, 0.10f, 0.92f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, UITheme::kAccentDim);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, UITheme::kAccent);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
    // SGE_AUTO_DEPLOY=host|client|1 presses DEPLOY once, for that session role
    // (1: any), so an unattended two-instance run can deploy one player and
    // not the other -- which is how the aircraft countdown starting on the
    // first deploy anywhere is checked.
    static bool autoDeployed = false;
    // SGE_AUTO_RANDOMIZE=<seed> presses RANDOMIZE CONDITIONS with that seed
    // once the squad and navmesh are up; auto-deploy waits for it.
    static bool autoRandomized = false;
    bool autoRandomizeWaiting = false;
    if (!autoRandomized) {
        char seedText[16] = {};
        if (GetEnvironmentVariableA("SGE_AUTO_RANDOMIZE", seedText,
                                    sizeof(seedText)) > 0) {
            if (g_banditLoaded && !g_game.loading.Active() &&
                g_navigation.Ready()) {
                g_lastDeploymentRollSeed = RandomizeDeployment(
                    static_cast<unsigned int>(std::strtoul(seedText, nullptr, 10)));
                autoRandomized = true;
            } else {
                autoRandomizeWaiting = true;
            }
        }
    }
    bool autoDeploy = false;
    bool autoSquad = false;
    char autoRole[16] = {};
    if (!autoDeployed && weaponsReady && !g_deploymentZones.empty() &&
        !autoRandomizeWaiting &&
        GetEnvironmentVariableA("SGE_AUTO_DEPLOY", autoRole,
                                sizeof(autoRole)) > 0) {
        const bool client = g_netSession.CurrentRole() == net::Role::Client;
        autoDeploy = std::strcmp(autoRole, "1") == 0 ||
            (std::strcmp(autoRole, "client") == 0 && client) ||
            (std::strcmp(autoRole, "host") == 0 && !client);
        // squad: the host presses DEPLOY SQUAD instead.
        autoSquad = std::strcmp(autoRole, "squad") == 0 && !client &&
                    MultiplayerActive();
        if (autoDeploy || autoSquad) {
            // SGE_AUTO_DEPLOY_ZONE=N picks zone N, numbered as the screen
            // shows them (from 1).
            char zoneText[16] = {};
            if (GetEnvironmentVariableA("SGE_AUTO_DEPLOY_ZONE", zoneText,
                                        sizeof(zoneText)) > 0) {
                const int zone = std::atoi(zoneText) - 1;
                if (zone >= 0 &&
                    zone < static_cast<int>(g_deploymentZones.size()))
                    g_selectedDeploymentZone = zone;
            }
            if (g_selectedDeploymentZone < 0) g_selectedDeploymentZone = 0;
            autoDeployed = true;
            SGE_LOG("LogGameplay", EngineLog::Level::Display,
                std::string("Auto-deploy (") + autoRole + ")");
        }
    }
    // Full-width commit button: DEPLOY in the title face over a line saying
    // exactly what pressing it does.
    const float deployWidth = ImGui::GetContentRegionAvail().x;
    const ImVec2 deployOrigin = ImGui::GetCursorScreenPos();
    deployPressed =
        ImGui::Button("##Deploy", ImVec2(deployWidth, 64.0f)) || autoDeploy;
    {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const bool ready = g_selectedDeploymentZone >= 0 && weaponsReady;
        draw->AddRect(deployOrigin,
                      ImVec2(deployOrigin.x + deployWidth, deployOrigin.y + 64.0f),
                      ready ? IM_COL32(120, 255, 160, 200)
                            : IM_COL32(120, 132, 124, 120), 0.0f, 0, 1.5f);
        ImFont* font = g_menuTitleFont ? g_menuTitleFont : ImGui::GetFont();
        constexpr float kSize = 30.0f;
        const char* label = "DEPLOY";
        const ImVec2 labelSize = font->CalcTextSizeA(kSize, FLT_MAX, 0.0f, label);
        draw->AddText(font, kSize,
            ImVec2(deployOrigin.x + (deployWidth - labelSize.x) * 0.5f,
                   deployOrigin.y + 6.0f),
            IM_COL32(240, 255, 244, ready ? 255 : 140), label);
        char detail[64];
        if (g_selectedDeploymentZone >= 0) {
            const char* method =
                g_playerInsertionChoice == LevelInsertionMode::Boat ? "BOAT"
                : g_playerInsertionChoice == LevelInsertionMode::FastRappel
                    ? "FAST ROPE" : "HELO";
            std::snprintf(detail, sizeof(detail), "INSERT BY %s AT LZ %02d",
                          method, g_selectedDeploymentZone + 1);
        } else {
            std::snprintf(detail, sizeof(detail), "SELECT A LANDING ZONE");
        }
        const ImVec2 detailSize = ImGui::CalcTextSize(detail);
        draw->AddText(
            ImVec2(deployOrigin.x + (deployWidth - detailSize.x) * 0.5f,
                   deployOrigin.y + 64.0f - detailSize.y - 8.0f),
            IM_COL32(210, 240, 220, ready ? 230 : 120), detail);
    }
    // The host can take the whole squad in with it: every client still on
    // this screen deploys at the same moment, in the same aircraft.
    if (hostingSession) {
        const bool squadPressed = ImGui::Button(
            "DEPLOY SQUAD (CLIENTS RIDE WITH YOU)", ImVec2(deployWidth, 40.0f));
        if ((squadPressed || autoSquad) && g_selectedDeploymentZone >= 0) {
            const XMFLOAT3& dropOff = g_deploymentZones[
                static_cast<size_t>(g_selectedDeploymentZone)];
            g_netSession.PublishSquadDeploy(
                static_cast<uint8_t>(g_playerInsertionChoice),
                static_cast<uint8_t>(g_insertionAirframe),
                g_playerRidesLeftSeat, dropOff.x, dropOff.y, dropOff.z);
            SGE_LOG("LogNet", EngineLog::Level::Display,
                "Squad deploy ordered");
            deployPressed = true;
        }
    }
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(3);
    ImGui::EndDisabled();
    ImGui::End();
    };
    if (g_deploymentArmoryVisible) renderArmory();
    // Squad orders must still reach clients while their armory is hidden.
    if (g_squadDeployRequested && g_selectedDeploymentZone >= 0) {
        g_squadDeployRequested = false;
        deployPressed = true;
    }
    if (deployPressed) CommitDeployment(hwnd, g_replayPlanActive);

    // Test hook: SGE_AUTO_LOADOUT_PICKER=<slot> (0 primary, 1 secondary,
    // 2 ordnance, 3 gear) opens that picker once after two seconds, so it can
    // be captured without driving the mouse.
    {
        static int autoPicker = [] {
            char text[8] = {};
            return GetEnvironmentVariableA("SGE_AUTO_LOADOUT_PICKER", text,
                                           sizeof(text)) > 0 ? std::atoi(text) : -1;
        }();
        static double autoPickerStart = -1.0;
        if (autoPicker >= 0 && autoPickerStart < 0.0)
            autoPickerStart = ImGui::GetTime();
        if (autoPicker >= 0 && ImGui::GetTime() - autoPickerStart > 2.0) {
            g_loadoutPickerSlot = autoPicker;
            g_loadoutPickerFocus = -1;
            char weapon[8] = {};
            if (GetEnvironmentVariableA("SGE_AUTO_LOADOUT_WEAPON", weapon, sizeof(weapon)) > 0)
                g_loadoutPickerFocus = std::atoi(weapon);
            autoPicker = -1;
        }
    }
}

// The extraction report, laid out as a campaign debrief: the result as the
// hero line top-left, the conditions it was fought in top-right, and the run
// broken into tabbed plates underneath -- SUMMARY for the at-a-glance read,
// SCORE for where the grade came from, PAYOUT for what it turned into. The
// right of the screen is left open so the island the run was fought on shows
// through.
//
// It is also the only animated screen in the game: figures count up, plates
// arrive on a short stagger, and the career bar sweeps from where the run
// started to where it ended. All of it hangs off g_winScreenAge, and all of it
// is presentation -- the numbers were banked in OpenWinScreen, and nothing
// drawn here can change them.
static void RenderWinScreen(HWND hwnd) {
    const ImGuiIO& io = ImGui::GetIO();
    const ImVec2 display = io.DisplaySize;
    // Real seconds and clamped, so a hitch on the first frame does not skip the
    // whole animation. Same contract as the HUD's floating payouts.
    g_winScreenAge += (std::min)(0.1f, io.DeltaTime);

    const MissionReport& report = g_game.mission.Report();
    const MissionLoadout& loadout = g_game.mission.Loadout();
    const MissionRunStats& stats = g_game.mission.Stats();
    const bool missionFailed = !g_missionFailReason.empty();
    const bool primaryFailed = missionFailed ||
        (report.primaryObjectivePresent && !report.primaryObjectiveComplete);

    // ---- Animation helpers -------------------------------------------------
    //
    // One eased 0..1 ramp per section, started at a stagger so the report
    // assembles itself instead of appearing all at once.
    constexpr float kStageSeconds = 0.42f;
    const auto stage = [&](float delay) {
        const float t = (std::max)(0.0f,
            (std::min)(1.0f, (g_winScreenAge - delay) / kStageSeconds));
        return 1.0f - (1.0f - t) * (1.0f - t);
    };
    // Counts a figure up over its stage. Snaps to the target at the end rather
    // than easing into it asymptotically -- a payout that stopped one short of
    // what the wallet says would be a bug report.
    const auto countUp = [&](int64_t target, float delay) -> int64_t {
        const float t = stage(delay);
        if (t >= 1.0f) return target;
        return static_cast<int64_t>(static_cast<double>(target) * t);
    };
    const auto fade = [](ImU32 colour, float t) {
        const float alpha = static_cast<float>(
            (colour >> IM_COL32_A_SHIFT) & 0xFFu) * (std::max)(0.0f, (std::min)(1.0f, t));
        return (colour & ~IM_COL32_A_MASK) |
               (static_cast<ImU32>(alpha) << IM_COL32_A_SHIFT);
    };

    // The deploy screen's palette.
    const ImU32 kLabel = IM_COL32(140, 152, 144, 230);
    const ImU32 kValue = IM_COL32(236, 240, 236, 255);
    const ImU32 kHair = IM_COL32(120, 132, 124, 60);
    const ImU32 kPanelFill = IM_COL32(8, 13, 16, 178);
    const ImU32 kPanelEdge = IM_COL32(120, 132, 124, 95);
    const ImU32 kTileFill = IM_COL32(16, 24, 27, 215);
    const ImU32 kGreen = IM_COL32(110, 220, 140, 255);
    const ImU32 kRed = IM_COL32(237, 107, 77, 255);
    const ImU32 kAmber = IM_COL32(255, 178, 61, 255);
    const ImU32 kGold = IM_COL32(214, 176, 84, 255);
    const ImU32 kBlue = IM_COL32(140, 210, 240, 235);
    const float entry = stage(0.0f);

    ImFont* titleFont = g_menuTitleFont ? g_menuTitleFont : ImGui::GetFont();
    ImFont* bodyFont = ImGui::GetFont();
    const float bodySize = ImGui::GetFontSize();
    const auto widthOf = [](ImFont* font, float size, const char* text) {
        return font->CalcTextSizeA(size, FLT_MAX, 0.0f, text).x;
    };

    // ---- Backdrop ----------------------------------------------------------
    //
    // Dark down the left where the plates sit, thinning to the right so the
    // island stays visible. A failed primary tints it red.
    {
        ImDrawList* backdrop = ImGui::GetBackgroundDrawList();
        const int a = static_cast<int>(entry * 255.0f);
        const ImU32 deep = primaryFailed ? IM_COL32(30, 6, 4, 0) : IM_COL32(3, 6, 8, 0);
        const ImU32 left = (deep & ~IM_COL32_A_MASK) |
                           (static_cast<ImU32>(a * 215 / 255) << IM_COL32_A_SHIFT);
        const ImU32 right = (deep & ~IM_COL32_A_MASK) |
                            (static_cast<ImU32>(a * 70 / 255) << IM_COL32_A_SHIFT);
        backdrop->AddRectFilledMultiColor(ImVec2(0, 0), display, left, right, right, left);
        // Top and bottom bands, for the header and the action row.
        const ImU32 band = (deep & ~IM_COL32_A_MASK) |
                           (static_cast<ImU32>(a * 150 / 255) << IM_COL32_A_SHIFT);
        const ImU32 clear = deep;
        backdrop->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(display.x, 170.0f),
                                          band, band, clear, clear);
        backdrop->AddRectFilledMultiColor(ImVec2(0, display.y - 180.0f), display,
                                          clear, clear, band, band);
    }

    ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowSize(display, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    ImGui::Begin("Win Screen", nullptr,
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    ImDrawList* draw = ImGui::GetWindowDrawList();

    constexpr float kMargin = 72.0f;
    constexpr float kPad = 18.0f;
    constexpr float kGap = 18.0f;
    constexpr float kHeadingSize = 22.0f;
    const float left = kMargin;
    const float right = display.x - kMargin;

    // A plate: translucent fill, hairline edge, title in the heading face and
    // an optional counter right-aligned beside it. Returns where its content
    // starts. A failed primary rules every plate red along the top.
    const auto plate = [&](ImVec2 min, ImVec2 max, const char* title,
                           const char* counter, float t) {
        draw->AddRectFilled(min, max, fade(kPanelFill, t));
        draw->AddRect(min, max, fade(kPanelEdge, t));
        if (primaryFailed)
            draw->AddRectFilled(min, ImVec2(max.x, min.y + 2.0f), fade(kRed, t));
        draw->AddText(titleFont, kHeadingSize, ImVec2(min.x + kPad, min.y + 12.0f),
                      fade(kValue, t), title);
        if (counter)
            draw->AddText(titleFont, kHeadingSize,
                ImVec2(max.x - kPad - widthOf(titleFont, kHeadingSize, counter),
                       min.y + 12.0f), fade(kValue, t), counter);
        return min.y + 12.0f + kHeadingSize + 12.0f;
    };
    // Label left, value right, hairline under: the PERFORMANCE / PAYOUT row.
    const auto valueRow = [&](float x0, float x1, float y, float height,
                              const char* label, const char* value,
                              ImU32 valueColour, bool rule, float t) {
        const float textY = y + (height - bodySize) * 0.5f;
        draw->AddText(bodyFont, bodySize, ImVec2(x0, textY), fade(kLabel, t), label);
        draw->AddText(bodyFont, bodySize,
            ImVec2(x1 - widthOf(bodyFont, bodySize, value), textY),
            fade(valueColour, t), value);
        if (rule)
            draw->AddLine(ImVec2(x0, y + height), ImVec2(x1, y + height),
                          fade(kHair, t), 1.0f);
    };

    // ---- Header ------------------------------------------------------------
    const char* operationTitle = report.commTowersTotal > 0 ? "BLACKOUT"
        : report.objectivePlanesTotal > 0 ? "GROUNDSWELL" : "FIELD EXERCISE";
    {
        char operationLine[128];
        std::snprintf(operationLine, sizeof(operationLine), "OPERATION %s",
                      operationTitle);
        draw->AddText(titleFont, 28.0f, ImVec2(left, 30.0f), fade(kLabel, entry),
                      operationLine);
        if (missionFailed) {
            char reason[96];
            std::snprintf(reason, sizeof(reason), "//  %s", g_missionFailReason.c_str());
            draw->AddText(titleFont, 28.0f,
                ImVec2(left + widthOf(titleFont, 28.0f, operationLine) + 16.0f, 30.0f),
                fade(kRed, entry), reason);
        }
        draw->AddText(titleFont, 64.0f, ImVec2(left, 58.0f),
                      fade(primaryFailed ? kRed : kValue, entry),
                      primaryFailed ? "MISSION FAILED" : "MISSION COMPLETE");

        // Top right: time on mission over the date and difficulty, and to its
        // left the conditions the run was fought in.
        constexpr float kClockWidth = 210.0f;
        const float clockX = right - kClockWidth;
        const int totalSeconds = static_cast<int>(report.elapsedSeconds + 0.5f);
        char clock[32];
        if (totalSeconds >= 3600)
            std::snprintf(clock, sizeof(clock), "%d:%02d:%02d", totalSeconds / 3600,
                          (totalSeconds / 60) % 60, totalSeconds % 60);
        else
            std::snprintf(clock, sizeof(clock), "%02d:%02d", totalSeconds / 60,
                          totalSeconds % 60);
        draw->AddText(titleFont, 36.0f, ImVec2(clockX, 28.0f), fade(kValue, entry), clock);
        SYSTEMTIME local{};
        GetLocalTime(&local);
        static constexpr const char* kMonths[12] = {
            "JAN", "FEB", "MAR", "APR", "MAY", "JUN",
            "JUL", "AUG", "SEP", "OCT", "NOV", "DEC" };
        char date[32];
        std::snprintf(date, sizeof(date), "%s %02u, %u",
                      kMonths[(std::max)(1, (std::min)(12, int(local.wMonth))) - 1],
                      local.wDay, local.wYear);
        draw->AddText(bodyFont, bodySize, ImVec2(clockX, 72.0f), fade(kLabel, entry), date);
        char difficulty[48];
        std::snprintf(difficulty, sizeof(difficulty), "DIFFICULTY x%.2f",
                      g_winScreenDifficulty);
        draw->AddText(bodyFont, bodySize, ImVec2(clockX, 72.0f + bodySize + 6.0f),
                      fade(kLabel, entry), difficulty);
        draw->AddLine(ImVec2(clockX - 24.0f, 30.0f), ImVec2(clockX - 24.0f, 112.0f),
                      fade(kPanelEdge, entry), 1.0f);

        static constexpr const char* kWeatherLabels[] = {
            "CLEAR", "CLOUDY", "DENSE FOG", "RAIN", "STORM", "CUSTOM" };
        const int weather = static_cast<int>(scene.weatherState);
        const char* weatherText =
            weather >= 0 && weather < IM_ARRAYSIZE(kWeatherLabels)
                ? kWeatherLabels[weather] : "--";
        char timeOfDay[32];
        std::snprintf(timeOfDay, sizeof(timeOfDay), "%s",
                      TimeOfDayName(g_selectedTimeOfDay));
        for (char* c = timeOfDay; *c; ++c)
            *c = static_cast<char>(std::toupper(static_cast<unsigned char>(*c)));
        const float conditionsWidth = (std::max)(widthOf(bodyFont, bodySize, weatherText),
                                                 widthOf(titleFont, 28.0f, timeOfDay));
        const float iconX = clockX - 48.0f - conditionsWidth - 16.0f - 64.0f;
        const ImVec2 iconMin(iconX, 34.0f);
        const ImVec2 iconMax(iconX + 64.0f, 98.0f);
        draw->AddRectFilled(iconMin, iconMax, fade(kPanelFill, entry));
        draw->AddRect(iconMin, iconMax, fade(kPanelEdge, entry));
        // Sun for clear skies, cloud for the rest.
        const ImVec2 iconCentre((iconMin.x + iconMax.x) * 0.5f,
                                (iconMin.y + iconMax.y) * 0.5f);
        const ImU32 glyph = fade(IM_COL32(220, 226, 220, 230), entry);
        if (weather == 0) {
            draw->AddCircle(iconCentre, 9.0f, glyph, 0, 2.0f);
            for (int ray = 0; ray < 8; ++ray) {
                const float angle = ray * 3.14159265f / 4.0f;
                const ImVec2 dir(std::cos(angle), std::sin(angle));
                draw->AddLine(ImVec2(iconCentre.x + dir.x * 13.0f, iconCentre.y + dir.y * 13.0f),
                              ImVec2(iconCentre.x + dir.x * 18.0f, iconCentre.y + dir.y * 18.0f),
                              glyph, 2.0f);
            }
        } else {
            const ImVec2 c = ImVec2(iconCentre.x, iconCentre.y + 3.0f);
            draw->PathArcTo(ImVec2(c.x - 9.0f, c.y + 1.0f), 7.0f, 1.57f, 4.2f);
            draw->PathArcTo(ImVec2(c.x + 1.0f, c.y - 5.0f), 10.0f, 3.5f, 6.0f);
            draw->PathArcTo(ImVec2(c.x + 11.0f, c.y + 2.0f), 6.0f, 4.9f, 7.85f);
            draw->PathStroke(glyph, ImDrawFlags_Closed, 2.0f);
            if (weather == 3 || weather == 4)
                for (int drop = -1; drop <= 1; ++drop)
                    draw->AddLine(ImVec2(c.x + drop * 8.0f, c.y + 13.0f),
                                  ImVec2(c.x + drop * 8.0f - 3.0f, c.y + 20.0f),
                                  glyph, 1.5f);
        }
        draw->AddText(bodyFont, bodySize, ImVec2(iconMax.x + 16.0f, 38.0f),
                      fade(kLabel, entry), weatherText);
        draw->AddText(titleFont, 28.0f, ImVec2(iconMax.x + 16.0f, 40.0f + bodySize + 4.0f),
                      fade(kValue, entry), timeOfDay);

        draw->AddLine(ImVec2(0.0f, 130.0f), ImVec2(display.x, 130.0f),
                      fade(kHair, entry), 1.0f);
    }

    // ---- Tabs --------------------------------------------------------------
    //
    // Q and E cycle them, as the reference debriefs do, and each is clickable.
    static constexpr const char* kTabs[] = { "SUMMARY", "SCORE", "PAYOUT" };
    constexpr int kTabCount = IM_ARRAYSIZE(kTabs);
    if (ImGui::IsKeyPressed(ImGuiKey_Q, false))
        g_winScreenTab = (g_winScreenTab + kTabCount - 1) % kTabCount;
    if (ImGui::IsKeyPressed(ImGuiKey_E, false))
        g_winScreenTab = (g_winScreenTab + 1) % kTabCount;
    constexpr float kTabY = 146.0f;
    constexpr float kTabHeight = 40.0f;
    constexpr float kTabWidth = 128.0f;
    {
        const auto keyBox = [&](float x, const char* key) {
            const ImVec2 min(x, kTabY + 9.0f);
            const ImVec2 max(x + 22.0f, kTabY + 31.0f);
            draw->AddRect(min, max, fade(kLabel, entry));
            const float w = widthOf(bodyFont, bodySize, key);
            draw->AddText(bodyFont, bodySize,
                ImVec2(min.x + (22.0f - w) * 0.5f, min.y + (22.0f - bodySize) * 0.5f),
                fade(kLabel, entry), key);
        };
        keyBox(left - 36.0f, "Q");
        for (int i = 0; i < kTabCount; ++i) {
            const ImVec2 min(left + kTabWidth * i, kTabY);
            const ImVec2 max(min.x + kTabWidth, kTabY + kTabHeight);
            ImGui::SetCursorScreenPos(min);
            ImGui::PushID(i);
            if (ImGui::InvisibleButton("##WinTab", ImVec2(kTabWidth, kTabHeight)))
                g_winScreenTab = i;
            const bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();
            const bool active = g_winScreenTab == i;
            draw->AddRectFilled(min, max, fade(active ? IM_COL32(32, 76, 102, 225)
                                                      : hovered ? IM_COL32(28, 38, 42, 210)
                                                                : IM_COL32(8, 13, 16, 190),
                                               entry));
            draw->AddRect(min, max, fade(kPanelEdge, entry));
            if (active)
                draw->AddRectFilled(ImVec2(min.x, max.y - 2.0f), max,
                                    fade(IM_COL32(140, 210, 240, 255), entry));
            const float w = widthOf(bodyFont, bodySize, kTabs[i]);
            draw->AddText(bodyFont, bodySize,
                ImVec2(min.x + (kTabWidth - w) * 0.5f,
                       min.y + (kTabHeight - bodySize) * 0.5f),
                fade(active ? kValue : kLabel, entry), kTabs[i]);
        }
        keyBox(left + kTabWidth * kTabCount + 14.0f, "E");
    }

    // ---- Layout ------------------------------------------------------------
    constexpr float kActionHeight = 54.0f;
    const float actionY = display.y - 72.0f - kActionHeight;
    const float contentTop = kTabY + kTabHeight + kGap;
    const float contentBottom = actionY - 26.0f;
    const float leftColumn = (std::min)(580.0f, (right - left) * 0.36f);

    // Run figures shared by more than one tab.
    net::ScoreboardEntry you = g_singlePlayerScore;
    static std::vector<net::ScoreboardEntry> board;
    board.clear();
    if (MultiplayerActive()) {
        g_netSession.GetScoreboard(board);
        for (const net::ScoreboardEntry& e : board)
            if (e.id == g_netSession.LocalId()) you = e;
    }
    char scoreCounter[24];
    std::snprintf(scoreCounter, sizeof(scoreCounter), "%lld / 100",
                  static_cast<long long>(countUp(report.totalScore, 0.30f)));

    if (g_winScreenTab == 0) {
        // ---- Objectives ----------------------------------------------------
        struct ObjectiveLine { bool done; bool primary; std::string label; };
        std::vector<ObjectiveLine> objectives;
        // Keyed to the towers rather than to primaryObjectivePresent, which
        // also covers aircraft: the airframes have their own line.
        if (report.commTowersTotal > 0)
            objectives.push_back({ report.commTowersDestroyed >= report.commTowersTotal,
                                   true, "Level the comm tower" });
        if (report.objectivePlanesTotal > 0) {
            char aircraft[96];
            std::snprintf(aircraft, sizeof(aircraft), "Down the aircraft  (%u of %u)%s",
                          report.objectivePlanesDestroyed, report.objectivePlanesTotal,
                          report.objectivePlanesEscaped > 0 ? "  ESCAPED" : "");
            objectives.push_back({ report.objectivePlanesEscaped == 0 &&
                                       report.objectivePlanesDestroyed ==
                                           report.objectivePlanesTotal,
                                   true, aircraft });
        }
        objectives.push_back({ !missionFailed, true, "Exfiltrate the island" });
        objectives.push_back({ report.usedBothWeapons, false, "Versatility: use both weapons" });
        objectives.push_back({ report.usedSelectedGrenade, false, "Grenadier: throw your grenade" });
        objectives.push_back({ report.demolitionObjective, false, "Demolition: 5 destruction events" });
        int done = 0;
        for (const ObjectiveLine& line : objectives) done += line.done ? 1 : 0;
        char counter[16];
        std::snprintf(counter, sizeof(counter), "%d/%d", done,
                      static_cast<int>(objectives.size()));

        constexpr float kObjectiveRow = 38.0f;
        const float objectivesT = stage(0.18f);
        const ImVec2 objMin(left, contentTop);
        const ImVec2 objMax(left + leftColumn,
            contentTop + 46.0f + kObjectiveRow * objectives.size() + 8.0f);
        float y = plate(objMin, objMax, "MISSION OBJECTIVES", counter, objectivesT);
        for (size_t i = 0; i < objectives.size(); ++i) {
            const ObjectiveLine& line = objectives[i];
            const float t = stage(0.26f + 0.07f * static_cast<float>(i));
            const float box = 24.0f;
            const ImVec2 boxMin(objMin.x + kPad, y + (kObjectiveRow - box) * 0.5f);
            const ImVec2 boxMax(boxMin.x + box, boxMin.y + box);
            if (line.done) {
                draw->AddRectFilled(boxMin, boxMax, fade(IM_COL32(36, 104, 58, 235), t));
                draw->AddLine(ImVec2(boxMin.x + 6.0f, boxMin.y + box * 0.52f),
                              ImVec2(boxMin.x + box * 0.42f, boxMax.y - 7.0f),
                              fade(kGreen, t), 2.2f);
                draw->AddLine(ImVec2(boxMin.x + box * 0.42f, boxMax.y - 7.0f),
                              ImVec2(boxMax.x - 6.0f, boxMin.y + 7.0f),
                              fade(kGreen, t), 2.2f);
            } else if (line.primary) {
                // A missed primary is a failure, not an open task.
                draw->AddRectFilled(boxMin, boxMax, fade(IM_COL32(96, 30, 22, 230), t));
                draw->AddLine(ImVec2(boxMin.x + 7.0f, boxMin.y + 7.0f),
                              ImVec2(boxMax.x - 7.0f, boxMax.y - 7.0f), fade(kRed, t), 2.0f);
                draw->AddLine(ImVec2(boxMax.x - 7.0f, boxMin.y + 7.0f),
                              ImVec2(boxMin.x + 7.0f, boxMax.y - 7.0f), fade(kRed, t), 2.0f);
            } else {
                draw->AddRect(boxMin, boxMax, fade(kLabel, t), 0.0f, 0, 1.4f);
            }
            draw->AddText(bodyFont, bodySize,
                ImVec2(boxMax.x + 14.0f, y + (kObjectiveRow - bodySize) * 0.5f),
                fade(line.done ? kValue : kLabel, t), line.label.c_str());
            if (i + 1 < objectives.size())
                draw->AddLine(ImVec2(boxMax.x + 14.0f, y + kObjectiveRow),
                              ImVec2(objMax.x - kPad, y + kObjectiveRow),
                              fade(kHair, t), 1.0f);
            y += kObjectiveRow;
        }

        // ---- Bottom row: rewards, grade, career ----------------------------
        constexpr float kBottomHeight = 168.0f;
        const float bottomTop = contentBottom - kBottomHeight;
        const float rowWidth = right - left;
        const float rewardsWidth = rowWidth * 0.36f;
        const float gradeWidth = rowWidth * 0.30f;
        const float careerWidth = rowWidth - rewardsWidth - gradeWidth - kGap * 2.0f;

        // ---- Performance ---------------------------------------------------
        const float middleTop = objMax.y + kGap;
        const float middleBottom = (std::max)(middleTop + 200.0f, bottomTop - kGap);
        const float perfWidth = leftColumn;
        {
            const float t = stage(0.42f);
            const ImVec2 min(left, middleTop);
            const ImVec2 max(left + perfWidth, middleBottom);
            float rowY = plate(min, max, "PERFORMANCE", nullptr, t);
            const float rowHeight = (std::min)(34.0f,
                (max.y - rowY - 10.0f) / 6.0f);
            char text[48];
            const auto row = [&](const char* label, const char* value) {
                valueRow(min.x + kPad, max.x - kPad, rowY, rowHeight, label, value,
                         kValue, false, t);
                rowY += rowHeight;
            };
            row("PLAY TIME", [&] {
                const int s = static_cast<int>(report.elapsedSeconds + 0.5f);
                std::snprintf(text, sizeof(text), "%02d:%02d:%02d", s / 3600,
                              (s / 60) % 60, s % 60);
                return text; }());
            std::snprintf(text, sizeof(text), "%lld",
                          static_cast<long long>(countUp(you.kills, 0.50f)));
            row("TOTAL KILLS", text);
            std::snprintf(text, sizeof(text), "%.0f%%", report.accuracyPercent);
            row("ACCURACY", text);
            std::snprintf(text, sizeof(text), "%u / %u", stats.shotsHit, stats.shotsFired);
            row("ROUNDS ON TARGET", text);
            std::snprintf(text, sizeof(text), "%u", report.destructionEvents);
            row("DESTRUCTION", text);
            std::snprintf(text, sizeof(text), "%u", you.deaths);
            row("TIMES DOWNED", text);
        }

        // Rewards: three tiles, icon over figure over caption.
        {
            const float t = stage(0.64f);
            const ImVec2 min(left, bottomTop);
            const ImVec2 max(left + rewardsWidth, contentBottom);
            const float tileTop = plate(min, max, "REWARDS", nullptr, t);
            const float tileGap = 10.0f;
            const float tileWidth = (rewardsWidth - kPad * 2.0f - tileGap * 2.0f) / 3.0f;
            const auto tile = [&](int index, const char* glyph, const char* value,
                                  const char* caption, ImU32 valueColour, float delay) {
                const float tt = stage(delay);
                const ImVec2 tMin(min.x + kPad + index * (tileWidth + tileGap), tileTop);
                const ImVec2 tMax(tMin.x + tileWidth, max.y - 14.0f);
                draw->AddRectFilled(tMin, tMax, fade(kTileFill, tt));
                draw->AddRect(tMin, tMax, fade(kPanelEdge, tt));
                const float cx = (tMin.x + tMax.x) * 0.5f;
                const ImVec2 hex(cx, tMin.y + 26.0f);
                ImVec2 points[6];
                for (int p = 0; p < 6; ++p) {
                    const float angle = 3.14159265f / 6.0f + p * 3.14159265f / 3.0f;
                    points[p] = ImVec2(hex.x + std::cos(angle) * 19.0f,
                                       hex.y + std::sin(angle) * 19.0f);
                }
                draw->AddConvexPolyFilled(points, 6, fade(IM_COL32(30, 40, 42, 255), tt));
                draw->AddPolyline(points, 6, fade(IM_COL32(200, 208, 200, 220), tt),
                                  ImDrawFlags_Closed, 2.0f);
                float w = widthOf(titleFont, 20.0f, glyph);
                draw->AddText(titleFont, 20.0f, ImVec2(cx - w * 0.5f, hex.y - 10.0f),
                              fade(kValue, tt), glyph);
                w = widthOf(titleFont, 24.0f, value);
                draw->AddText(titleFont, 24.0f, ImVec2(cx - w * 0.5f, tMin.y + 52.0f),
                              fade(valueColour, tt), value);
                w = widthOf(bodyFont, bodySize, caption);
                draw->AddText(bodyFont, bodySize,
                    ImVec2(cx - w * 0.5f, tMax.y - bodySize - 8.0f),
                    fade(kLabel, tt), caption);
            };
            char xp[32], xpValue[40];
            RankSystem::Format(xp, sizeof(xp), countUp(g_winScreenXpEarned, 0.70f));
            std::snprintf(xpValue, sizeof(xpValue), "+%s", xp);
            tile(0, "XP", xpValue, g_winScreenRewardMultiplier > 1.0f
                     ? "EXPERIENCE (BOOSTED)" : "EXPERIENCE",
                 g_winScreenRewardMultiplier > 1.0f ? kAmber : kValue, 0.70f);
            char cash[32], cashValue[40];
            MoneySystem::Format(cash, sizeof(cash), countUp(g_winScreenPayout, 0.78f));
            std::snprintf(cashValue, sizeof(cashValue), "+%s", cash);
            tile(1, "$", cashValue, "PAYOUT", kValue, 0.78f);
            char funds[32];
            MoneySystem::Format(funds, sizeof(funds),
                                countUp(g_game.money.Balance(), 0.86f));
            tile(2, "$$", funds, "FUNDS AT BASE", kValue, 0.86f);
        }
        // Grade: the letter, the score and how far it sat from the next mark.
        {
            const float t = stage(0.72f);
            const ImVec2 min(left + rewardsWidth + kGap, bottomTop);
            const ImVec2 max(min.x + gradeWidth, contentBottom);
            const float top = plate(min, max, "MISSION GRADE", nullptr, t);
            const char* grade = MissionRankName(report.rank);
            const ImVec2 boxMin(min.x + kPad, top);
            const ImVec2 boxMax(boxMin.x + 92.0f, max.y - 14.0f);
            draw->AddRectFilled(boxMin, boxMax, fade(kTileFill, t));
            draw->AddRect(boxMin, boxMax, fade(kPanelEdge, t));
            const float gw = widthOf(titleFont, 68.0f, grade);
            draw->AddText(titleFont, 68.0f,
                ImVec2((boxMin.x + boxMax.x - gw) * 0.5f,
                       (boxMin.y + boxMax.y) * 0.5f - 36.0f),
                fade(primaryFailed ? kRed : kValue, t), grade);
            const float x0 = boxMax.x + 18.0f;
            const float x1 = max.x - kPad;
            draw->AddText(bodyFont, bodySize, ImVec2(x0, top + 2.0f), fade(kLabel, t), "SCORE");
            draw->AddText(titleFont, 28.0f, ImVec2(x0, top + bodySize + 6.0f),
                          fade(kValue, t), scoreCounter);
            // Grade thresholds as ticks: "how far off an A was I" is the
            // question a bar is there to answer and a bare number cannot.
            const float barY = top + bodySize + 46.0f;
            const float fill = (std::max)(0.0f, (std::min)(1.0f,
                static_cast<float>(report.totalScore) / 100.0f)) * stage(0.80f);
            draw->AddRectFilled(ImVec2(x0, barY), ImVec2(x1, barY + 6.0f),
                                fade(IM_COL32(255, 255, 255, 30), t));
            if (fill > 0.0f)
                draw->AddRectFilled(ImVec2(x0, barY),
                                    ImVec2(x0 + (x1 - x0) * fill, barY + 6.0f),
                                    fade(primaryFailed ? kRed : kBlue, t));
            for (int threshold : { 45, 60, 75, 90 }) {
                const float tickX = x0 + (x1 - x0) * (threshold / 100.0f);
                draw->AddRectFilled(ImVec2(tickX, barY - 3.0f),
                                    ImVec2(tickX + 1.0f, barY + 9.0f),
                                    fade(IM_COL32(255, 255, 255, 90), t));
            }
            draw->AddText(bodyFont, bodySize, ImVec2(x0, barY + 18.0f),
                          fade(kLabel, t), "Breakdown under SCORE  [E]");
        }
        // Career: rank, the level bar sweeping across the run, promotion.
        {
            const float t = stage(0.80f);
            const ImVec2 min(left + rewardsWidth + gradeWidth + kGap * 2.0f, bottomTop);
            const ImVec2 max(min.x + careerWidth, contentBottom);
            const float top = plate(min, max, "CAREER", nullptr, t);
            const float x0 = min.x + kPad;
            const float x1 = max.x - kPad;
            const char* rankLabel = g_game.rank.RankLabel();
            draw->AddText(titleFont, 26.0f, ImVec2(x0, top), fade(kValue, t), rankLabel);
            char level[24];
            std::snprintf(level, sizeof(level), "LVL %d", g_game.rank.Level());
            draw->AddText(bodyFont, bodySize,
                ImVec2(x1 - widthOf(bodyFont, bodySize, level), top + 6.0f),
                fade(kLabel, t), level);
            const float sweep = stage(0.92f);
            const float span = static_cast<float>(g_game.rank.XpForNextLevel());
            const float endFraction = (std::max)(0.0f, (std::min)(1.0f,
                static_cast<float>(g_game.rank.XpIntoLevel()) / span));
            // Where the bar stood when the run armed. A promotion sweeps from
            // empty instead, which is what the crossing actually looked like.
            const float startFraction =
                g_winScreenLevelAfter > g_winScreenLevelBefore
                    ? 0.0f
                    : (std::max)(0.0f, (std::min)(1.0f,
                          static_cast<float>(g_game.rank.XpIntoLevel() -
                                             g_winScreenXpEarned) / span));
            const float fraction = startFraction + (endFraction - startFraction) * sweep;
            const float barY = top + 40.0f;
            draw->AddRectFilled(ImVec2(x0, barY), ImVec2(x1, barY + 8.0f),
                                fade(IM_COL32(255, 255, 255, 34), t));
            if (startFraction > 0.0f)
                draw->AddRectFilled(ImVec2(x0, barY),
                    ImVec2(x0 + (x1 - x0) * startFraction, barY + 8.0f),
                    fade(IM_COL32(70, 140, 92, 235), t));
            if (fraction > startFraction)
                draw->AddRectFilled(ImVec2(x0 + (x1 - x0) * startFraction, barY),
                    ImVec2(x0 + (x1 - x0) * fraction, barY + 8.0f),
                    fade(kGreen, t));
            char remaining[64];
            if (g_game.rank.Level() < RankSystem::kMaxLevel) {
                char xp[32];
                RankSystem::Format(xp, sizeof(xp),
                    g_game.rank.XpForNextLevel() - g_game.rank.XpIntoLevel());
                std::snprintf(remaining, sizeof(remaining), "%s XP to level %d", xp,
                              g_game.rank.Level() + 1);
            } else {
                std::snprintf(remaining, sizeof(remaining), "Maximum rank reached");
            }
            draw->AddText(bodyFont, bodySize, ImVec2(x0, barY + 18.0f),
                          fade(kLabel, t), remaining);
            // The stinger, last and loudest. Pulsed rather than static so it
            // reads as an event on a screen that is otherwise settling down.
            if (g_winScreenTierAfter != g_winScreenTierBefore ||
                g_winScreenLevelAfter > g_winScreenLevelBefore) {
                const float promoT = stage(1.15f) * (0.78f + 0.22f * std::sin(
                    static_cast<float>(ImGui::GetTime()) * 3.4f));
                char promo[96];
                if (g_winScreenTierAfter != g_winScreenTierBefore)
                    std::snprintf(promo, sizeof(promo), "PROMOTED  %s  ->  %s",
                                  PlayerRankName(g_winScreenTierBefore),
                                  PlayerRankName(g_winScreenTierAfter));
                else
                    std::snprintf(promo, sizeof(promo), "LEVEL UP  %d  ->  %d",
                                  g_winScreenLevelBefore, g_winScreenLevelAfter);
                draw->AddText(bodyFont, bodySize,
                    ImVec2(x0, barY + 26.0f + bodySize), fade(kAmber, promoT), promo);
            }
        }
    } else if (g_winScreenTab == 1) {
        // ---- Score breakdown -------------------------------------------------
        //
        // Each category with what it scored out of what it was worth, over a
        // bar. A category that banked nothing is dimmed, so the ones worth
        // improving stand out.
        const float t = stage(0.0f);
        const ImVec2 min(left, contentTop);
        const ImVec2 max(left + (std::min)(960.0f, right - left),
                         (std::min)(contentBottom, contentTop + 46.0f + 6.0f * 70.0f + 4.0f));
        float y = plate(min, max, "SCORE BREAKDOWN", scoreCounter, t);
        const float x0 = min.x + kPad;
        const float x1 = max.x - kPad;
        const float rowHeight = (std::min)(70.0f, (max.y - y - 14.0f) / 6.0f);
        int row = 0;
        const auto scoreRow = [&](const char* label, const char* detail,
                                  int earned, int available) {
            const float rt = stage(0.05f + 0.06f * row++);
            char value[32];
            std::snprintf(value, sizeof(value), "%d / %d", earned, available);
            const ImU32 tint = earned <= 0 ? kLabel : kValue;
            draw->AddText(titleFont, 22.0f, ImVec2(x0, y + 6.0f), fade(tint, rt), label);
            draw->AddText(titleFont, 22.0f,
                ImVec2(x1 - widthOf(titleFont, 22.0f, value), y + 6.0f),
                fade(tint, rt), value);
            if (detail && *detail)
                draw->AddText(bodyFont, bodySize,
                    ImVec2(x0 + widthOf(titleFont, 22.0f, label) + 18.0f, y + 10.0f),
                    fade(kLabel, rt), detail);
            const float fraction = available > 0
                ? (std::max)(0.0f, (std::min)(1.0f,
                      static_cast<float>(earned) / static_cast<float>(available))) * rt
                : 0.0f;
            const float barY = y + 40.0f;
            draw->AddRectFilled(ImVec2(x0, barY), ImVec2(x1, barY + 6.0f),
                                fade(IM_COL32(255, 255, 255, 30), rt));
            if (fraction > 0.0f)
                draw->AddRectFilled(ImVec2(x0, barY),
                    ImVec2(x0 + (x1 - x0) * fraction, barY + 6.0f),
                    fade(IM_COL32(236, 242, 246, 235), rt));
            y += rowHeight;
        };
        char detail[96];
        scoreRow("TIME", nullptr, report.timeScore, 15);
        std::snprintf(detail, sizeof(detail), "%.1f%%  //  %u of %u rounds on target",
                      report.accuracyPercent, stats.shotsHit, stats.shotsFired);
        scoreRow("ACCURACY", detail, report.accuracyScore, 20);
        std::snprintf(detail, sizeof(detail), "%u of %u marines lost",
                      report.casualties, stats.friendliesDeployed);
        scoreRow("CASUALTIES", detail, report.casualtyScore, 15);
        std::snprintf(detail, sizeof(detail), "%u of %u complete",
                      report.optionalObjectivesCompleted,
                      report.optionalObjectivesTotal);
        scoreRow("OPTIONAL OBJECTIVES", detail, report.optionalScore, 15);
        std::snprintf(detail, sizeof(detail), "%u events", report.destructionEvents);
        scoreRow("DESTRUCTION", detail, report.destructionScore, 10);
        // Named for what this map actually authored. The primary score covers
        // masts and aircraft together, so a fixed "comm towers" line would read
        // as 0 of 0 on the airfield while the score beside it said 25.
        if (report.commTowersTotal > 0 && report.objectivePlanesTotal > 0)
            std::snprintf(detail, sizeof(detail), "%u of %u targets down",
                          report.commTowersDestroyed + report.objectivePlanesDestroyed,
                          report.commTowersTotal + report.objectivePlanesTotal);
        else if (report.commTowersTotal > 0)
            std::snprintf(detail, sizeof(detail), "%u of %u comm towers down",
                          report.commTowersDestroyed, report.commTowersTotal);
        else if (report.objectivePlanesTotal > 0)
            std::snprintf(detail, sizeof(detail), "%u of %u aircraft down",
                          report.objectivePlanesDestroyed,
                          report.objectivePlanesTotal);
        else
            std::snprintf(detail, sizeof(detail), "no primary target on this map");
        scoreRow("PRIMARY OBJECTIVE", detail, report.primaryScore,
                 MissionSystem::kPrimaryObjectiveScore);
    } else {
        // ---- Payout, progression, kit ----------------------------------------
        //
        // Cash and experience on separate plates: cash is spent on the next
        // deployment and experience is not, and running them together made the
        // grade bonus look like it was counted twice.
        const float plateWidth = (std::min)(520.0f, (right - left - kGap * 2.0f) / 3.0f);
        constexpr float kRow = 38.0f;
        const float plateBottom = contentTop + 46.0f + kRow * 4.0f + 14.0f;
        const auto column = [&](int index) {
            return ImVec2(left + index * (plateWidth + kGap), contentTop);
        };
        {
            const float t = stage(0.0f);
            const ImVec2 min = column(0);
            const ImVec2 max(min.x + plateWidth, plateBottom);
            float y = plate(min, max, "PAYOUT", nullptr, t);
            char text[32];
            const auto money = [&](const char* label, int64_t amount, float delay,
                                   ImU32 colour, bool rule) {
                MoneySystem::Format(text, sizeof(text), countUp(amount, delay));
                valueRow(min.x + kPad, max.x - kPad, y, kRow, label, text, colour,
                         rule, t);
                y += kRow;
            };
            money("Field earnings", g_winScreenPayout - g_winScreenMissionBonus,
                  0.05f, kValue, true);
            char bonusLabel[64];
            std::snprintf(bonusLabel, sizeof(bonusLabel), "Mission bonus (%d x %d)",
                          report.totalScore, MoneySystem::kMissionBonusPerScorePoint);
            money(bonusLabel, g_winScreenMissionBonus, 0.12f, kValue, true);
            money("TOTAL THIS RUN", g_winScreenPayout, 0.20f, kGreen, true);
            money("Funds", g_game.money.Balance(), 0.20f, kLabel, false);
        }
        {
            const float t = stage(0.08f);
            const ImVec2 min = column(1);
            const ImVec2 max(min.x + plateWidth, plateBottom);
            float y = plate(min, max, "PROGRESSION", nullptr, t);
            const auto xpRow = [&](const char* label, int64_t amount, float delay,
                                   ImU32 colour, bool rule) {
                char number[32];
                RankSystem::Format(number, sizeof(number), countUp(amount, delay));
                char value[48];
                std::snprintf(value, sizeof(value), "%s XP", number);
                valueRow(min.x + kPad, max.x - kPad, y, kRow, label, value, colour,
                         rule, t);
                y += kRow;
            };
            xpRow("Field experience", g_winScreenXpEarned - g_winScreenXpBonus,
                  0.12f, kValue, true);
            char bonusLabel[64];
            std::snprintf(bonusLabel, sizeof(bonusLabel), "Mission bonus (%d x %d)",
                          report.totalScore, RankSystem::kMissionBonusPerScorePoint);
            xpRow(bonusLabel, g_winScreenXpBonus, 0.20f, kValue, true);
            xpRow("TOTAL THIS RUN", g_winScreenXpEarned, 0.28f, kGreen, true);
            char multiplier[16];
            std::snprintf(multiplier, sizeof(multiplier), "x%.2f",
                          g_winScreenRewardMultiplier);
            valueRow(min.x + kPad, max.x - kPad, y, kRow, "Reward multiplier",
                     multiplier,
                     g_winScreenRewardMultiplier > 1.0f ? kAmber : kValue, false, t);
        }
        {
            const float t = stage(0.16f);
            const ImVec2 min = column(2);
            const ImVec2 max(min.x + plateWidth, plateBottom);
            float y = plate(min, max, "DEPLOYED WITH", nullptr, t);
            const auto kit = [&](const char* label, const char* value, bool rule) {
                valueRow(min.x + kPad, max.x - kPad, y, kRow, label, value, kValue,
                         rule, t);
                y += kRow;
            };
            kit("Primary", GunModel::WeaponName(loadout.weapons[0]), true);
            kit("Secondary", GunModel::WeaponName(loadout.weapons[1]), true);
            kit("Ordnance", GrenadeTypeName(loadout.grenade), true);
            kit("Field gear", GearTypeName(loadout.gear), false);
        }
    }

    // ---- Actions -----------------------------------------------------------
    //
    // Outlined secondaries bottom-left, the gold commit bottom-right with a
    // chevron, as the reference does it. RETURN TO BASE is the commit: a
    // finished run ends by flying home, where the payout is actually spent.
    {
        const float actionT = stage(0.30f);
        const auto action = [&](const char* id, ImVec2 min, float width,
                                const char* label, bool primary, bool enabled) {
            ImGui::SetCursorScreenPos(min);
            const bool pressed =
                ImGui::InvisibleButton(id, ImVec2(width, kActionHeight)) && enabled;
            const bool hovered = enabled && ImGui::IsItemHovered();
            const ImVec2 max(min.x + width, min.y + kActionHeight);
            const float t = actionT * (enabled ? 1.0f : 0.45f);
            if (primary) {
                draw->AddRectFilled(min, max, fade(hovered ? IM_COL32(86, 70, 28, 240)
                                                           : IM_COL32(50, 41, 18, 220), t));
                draw->AddRect(min, max, fade(kGold, t), 0.0f, 0, 2.0f);
            } else {
                draw->AddRectFilled(min, max, fade(hovered ? IM_COL32(30, 40, 44, 225)
                                                           : IM_COL32(8, 13, 16, 190), t));
                draw->AddRect(min, max, fade(hovered ? IM_COL32(220, 226, 220, 220)
                                                     : IM_COL32(200, 206, 200, 150), t),
                              0.0f, 0, 1.5f);
            }
            const float size = primary ? 28.0f : 22.0f;
            const float chevron = primary ? 40.0f : 0.0f;
            const float w = widthOf(titleFont, size, label);
            draw->AddText(titleFont, size,
                ImVec2(min.x + (width - chevron - w) * 0.5f,
                       min.y + (kActionHeight - size) * 0.5f),
                fade(kValue, t), label);
            if (primary) {
                const ImVec2 c(max.x - 32.0f, (min.y + max.y) * 0.5f);
                draw->AddLine(ImVec2(c.x - 4.0f, c.y - 8.0f), ImVec2(c.x + 4.0f, c.y),
                              fade(kValue, t), 2.0f);
                draw->AddLine(ImVec2(c.x + 4.0f, c.y), ImVec2(c.x - 4.0f, c.y + 8.0f),
                              fade(kValue, t), 2.0f);
            }
            return pressed;
        };
        const bool hostControls = !(MultiplayerActive() &&
            g_netSession.CurrentRole() != net::Role::Host);
        if (action("##QuickRestart", ImVec2(left, actionY), 230.0f,
                   "REPLAY MISSION", false, hostControls))
            RestartWithPlan(hwnd, net::RestartPlanMode::Quick);
        if (action("##ChangePlan", ImVec2(left + 246.0f, actionY), 230.0f,
                   "CHANGE PLAN", false, hostControls))
            RestartWithPlan(hwnd, net::RestartPlanMode::ChangePlan);
        if (!hostControls)
            draw->AddText(bodyFont, bodySize,
                ImVec2(left, actionY + kActionHeight + 8.0f), fade(kLabel, actionT),
                "The host chooses whether to replay");
        if (action("##ReturnToBase", ImVec2(right - 300.0f, actionY), 300.0f,
                   "RETURN TO BASE", true, true))
            StartBase(hwnd);
    }
    ImGui::End();

    // Test hook: SGE_WIN_CAPTURE_PATH=<file.ppm> captures this screen four
    // seconds after it opens, then quits. SGE_WIN_TAB picks the tab.
    {
        static const int forcedTab = [] {
            char text[8] = {};
            return GetEnvironmentVariableA("SGE_WIN_TAB", text, sizeof(text)) > 0
                ? std::atoi(text) : -1;
        }();
        if (forcedTab >= 0 && forcedTab < kTabCount && g_winScreenAge < 0.2f)
            g_winScreenTab = forcedTab;
        static UICaptureHook captureHook;
        RunUICaptureHook(captureHook, "SGE_WIN_CAPTURE_PATH");
    }
}

// In-game pause. ESC used to quit the run outright; this is what it opens
// instead, and leaving is now a button that says so.
//
// Built from the main menu's own vocabulary rather than a new look: the same
// wordmark treatment with hand-tracked letters, the same hairline rule under
// it, the same UIMenuRow entries, and the same RenderSettingsMenu panel drawn
// in place of the row list. The screen the player pauses into should read as
// the screen they started from.
//
// What differs is the backdrop. The main menu owns the whole screen and can
// fill it with art; a pause screen has a live frame behind it that the player
// needs to still recognise, so this is a scrim over the game rather than a
// replacement for it.
static void RenderPauseMenu(HWND hwnd) {
    // Settings take the whole screen, the same page the main menu opens. The
    // pause screen's own flag, so BACK returns to these rows and the main
    // menu is left exactly as the player left it.
    if (g_showPauseSettings) {
        RenderSettingsMenu(g_showPauseSettings);
        return;
    }
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImDrawList* backdrop = ImGui::GetBackgroundDrawList();
    // Dark enough to carry white text over a bright desert or a muzzle flash,
    // sheer enough to leave the world readable underneath -- the player is
    // pausing in a place, and blacking it out loses where they were.
    backdrop->AddRectFilledMultiColor(ImVec2(0, 0), display,
        IM_COL32(8, 14, 16, 214), IM_COL32(20, 32, 32, 214),
        IM_COL32(6, 12, 14, 226), IM_COL32(5, 9, 12, 226));
    // The same top and bottom vignette the main menu and the win screen use,
    // which is what makes a scrim read as a screen rather than a grey sheet.
    backdrop->AddRectFilledMultiColor(
        ImVec2(0, 0), ImVec2(display.x, display.y * 0.16f),
        IM_COL32(0, 0, 0, 150), IM_COL32(0, 0, 0, 150),
        IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0));
    backdrop->AddRectFilledMultiColor(
        ImVec2(0, display.y * 0.82f), ImVec2(display.x, display.y),
        IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0),
        IM_COL32(0, 0, 0, 170), IM_COL32(0, 0, 0, 170));

    // Centred rather than the main menu's left column. There is no art to sit
    // beside here -- the backdrop is the player's own frame -- so a column
    // hugging the left edge would read as a panel that had slid off centre.
    const float width = (std::min)(460.0f, display.x - 48.0f);
    const float height = (std::min)(620.0f, display.y - 48.0f);
    ImGui::SetNextWindowPos(ImVec2((display.x - width) * 0.5f,
                                   (display.y - height) * 0.5f),
                            ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBackground |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 24));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 2));
    ImGui::Begin("Pause Menu", nullptr, flags);
    ImGui::PopStyleVar(3);
    // Baked fonts draw at 1.0; only the built-in fallback is scaled. Same rule
    // as the main menu, and for the same reason -- scaling a baked font is
    // what softens the text.
    ImGui::SetWindowFontScale(g_menuBodyFont ? 1.0f : 1.3f);

    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    const float contentWidth = ImGui::GetContentRegionAvail().x;

    // Wordmark, tracked by hand exactly as the main menu draws MILBOX. One
    // glyph at a time is the only way ImGui offers letter-spacing.
    if (g_menuTitleFont) ImGui::PushFont(g_menuTitleFont);
    else ImGui::SetWindowFontScale(4.4f);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 12.0f);
    {
        const char* title = "PAUSED";
        ImVec2 pen = ImGui::GetCursorScreenPos();
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const float tracking = ImGui::GetFontSize() * 0.14f;
        float x = pen.x;
        for (const char* c = title; *c; ++c) {
            const char glyph[2] = { *c, '\0' };
            draw->AddText(ImVec2(x, pen.y), IM_COL32(255, 255, 255, 255), glyph);
            x += ImGui::CalcTextSize(glyph).x + tracking;
        }
        ImGui::Dummy(ImVec2(x - pen.x, ImGui::GetTextLineHeight()));
    }
    if (g_menuTitleFont) ImGui::PopFont();
    ImGui::SetWindowFontScale(g_menuBodyFont ? 1.0f : 1.15f);

    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 cursor = ImGui::GetCursorScreenPos();
        draw->AddRectFilled(ImVec2(cursor.x + 14.0f, cursor.y),
                            ImVec2(cursor.x + contentWidth, cursor.y + 1.0f),
                            IM_COL32(255, 255, 255, 60));
    }
    ImGui::Dummy(ImVec2(0.0f, 14.0f));

    // Multiplayer keeps running behind this screen, and saying nothing about
    // that would be the screen lying: a player who reads "PAUSED" and walks
    // away comes back dead. The warning is part of the frame, above the
    // settings early-out, so it stays visible in the settings panel too.
    if (MultiplayerActive()) {
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 14.0f);
        ImGui::TextColored(UITheme::kWarning, "MULTIPLAYER  -  GAME STILL LIVE");
        ImGui::Dummy(ImVec2(0.0f, 12.0f));
    }

    ImGui::SetWindowFontScale(1.7f);
    if (UIMenuRow("RESUME"))
        TogglePauseMenu(hwnd);
    if (UIMenuRow("SETTINGS"))
        g_showPauseSettings = true;
    ImGui::SetWindowFontScale(g_menuBodyFont ? 1.0f : 1.15f);

    ImGui::Dummy(ImVec2(0.0f, 18.0f));
    {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 cursor = ImGui::GetCursorScreenPos();
        draw->AddRectFilled(ImVec2(cursor.x + 14.0f, cursor.y),
                            ImVec2(cursor.x + contentWidth, cursor.y + 1.0f),
                            IM_COL32(255, 255, 255, 40));
    }
    ImGui::Dummy(ImVec2(0.0f, 14.0f));

    // Leaving is separated from the rows above by a rule and named for what it
    // costs. OpenMainMenu banks the wallet on the way out, so progress is not
    // lost, but the run itself is -- and an unlabelled row next to RESUME is
    // how the old ESC behaviour surprised people in the first place.
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 14.0f);
    ImGui::TextColored(UITheme::kTextDim, "Abandons the current mission.");
    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    ImGui::SetWindowFontScale(1.7f);
    if (UIMenuRow("EXIT TO MENU")) {
        // Clear the pause before leaving so the cursor state OpenMainMenu sets
        // is the one that survives, rather than a resume path fighting it.
        g_gamePaused = false;
        g_showPauseSettings = false;
        OpenMainMenu();
    }
    ImGui::SetWindowFontScale(g_menuBodyFont ? 1.0f : 1.15f);

    ImGui::End();
}
