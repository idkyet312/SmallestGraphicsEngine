#pragma once

// Private application implementation; included once by main.cpp in dependency order.

static void RenderMainMenu(HWND hwnd) {
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

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImDrawList* background = ImGui::GetBackgroundDrawList();
    background->AddRectFilledMultiColor(ImVec2(0, 0), display,
        IM_COL32(10, 18, 21, 255), IM_COL32(25, 40, 40, 255),
        IM_COL32(8, 15, 18, 255), IM_COL32(7, 12, 15, 255));

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

    // Optional photographic backdrop. Missing art is not a failure: the drawn
    // contours below are the fallback, so the menu still has a background on a
    // build that ships without the image.
    const uint64_t menuImage =
        UITextureFromFile("Content/Textures/UI/menu_background.jpg");
    if (menuImage) {
        // Contain, not cover: scale by whichever axis runs out first, so the
        // whole photo is on screen whatever the window shape -- nothing of the
        // island gets cropped away. Placement is handled below.
        // Read the aspect off the texture rather than hardcoding it, so
        // swapping the file for one of a different shape needs no code change.
        const D3D12_RESOURCE_DESC desc =
            g_uiImages["Content/Textures/UI/menu_background.jpg"].texture->GetDesc();
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
    const float scrimStart = kMenuMargin - kMenuBaseMargin;
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

    ImGui::Dummy(ImVec2(0.0f, 14.0f));

    // Settings replace the menu body below the title, keeping the wordmark and
    // accent rule in place so it reads as the same screen rather than a
    // separate one the player has navigated to.
    if (g_showSettingsMenu) {
        RenderSettingsMenu();
        ImGui::End();
        return;
    }

    // The base is the way in: every run now starts by walking out to the
    // helicopter and picking a destination there, so the island is reached
    // through the hub rather than from a menu button beside it. Custom Game
    // stays, because a hand-authored level has no travel card at the base.
    //
    // Island 1 and the Training Range are still reachable -- the travel board
    // flies to the island, the editor loads any level, and --level=<path>
    // starts one directly -- they are just no longer this menu's job.
    // One unbroken column, in the order the player uses it. The section rules
    // are gone with the buttons: a divider every two entries was structure the
    // list is short enough not to need, and it fought the wordmark rule above.
    ImGui::SetWindowFontScale(1.7f);
    if (UIMenuRow("ENTER BASE"))
        StartBase(hwnd);
    if (UIMenuRow("CUSTOM GAME"))
        BrowseAndStartCustomLevel(hwnd);
    if (UIMenuRow("LEVEL EDITOR"))
        ImGui::OpenPopup("Level Editor");
    // Empty terrain with no gameplay actors, foliage, houses or ocean clutter.
    // Renderer work is what is left, so a graphics change can be looked at
    // without a full island's content confusing what is being measured.
    if (UIMenuRow("TEST LEVEL"))
        StartLevelOne(hwnd, true, false, true, nullptr, true);
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
    ImGui::End();
}

// Every wait in the game shows the same thing: the wordmark typing itself in,
// with a blinking dot standing in the slot the next letter will occupy. The
// dot is what says the game is still working -- it keeps moving even when a
// stage stalls for seconds -- and each letter landing on top of it is what
// says progress was actually made. Shared by the boot screen and the level
// loading screen so a wait looks the same wherever the player meets it.
//
// `progress` is 0..1 through whatever is being waited on. Letters are revealed
// across the first `letters/(letters+1)` of it, leaving the last slot's worth
// of progress showing the finished word rather than landing the final letter
// on the very last frame.
static void RenderMilboxWordmark(float progress) {
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    draw->AddRectFilled(ImVec2(0, 0), display, IM_COL32(4, 8, 12, 255));

    // Drawn straight onto the background list rather than into a window: with
    // no readout to lay out there is nothing for a window to hold, and a
    // centred wordmark wants the screen's centre, not a panel's.
    const bool bigFont = g_menuTitleFont != nullptr;
    if (bigFont) ImGui::PushFont(g_menuTitleFont);

    const char* title = "MILBOX";
    const int letters = static_cast<int>(std::strlen(title));
    const float clamped = (std::min)(1.0f, (std::max)(0.0f, progress));
    const float revealed = clamped * static_cast<float>(letters + 1);
    // How many letters have fully landed, and so which slot the dot sits in.
    // At least one: a load opens at 2% progress, which would otherwise put the
    // dot in the first slot and show the player a lone blinking dot with no
    // word attached. The M is the anchor the rest of the word grows from.
    const int filled =
        (std::min)(letters, (std::max)(1, static_cast<int>(revealed)));

    // Hand-tracked, the same way the menu wordmark is: a wordmark wants
    // tracking the font itself does not carry, and drawing one glyph at a time
    // is also the only way to place a dot in a specific letter's slot.
    const float tracking = ImGui::GetFontSize() * 0.14f;
    float wordWidth = 0.0f;
    for (int i = 0; i < letters; ++i) {
        const char glyph[2] = { title[i], '\0' };
        wordWidth += ImGui::CalcTextSize(glyph).x;
        if (i + 1 < letters) wordWidth += tracking;
    }
    // Measured across the whole word, so the letters already on screen stay
    // put as the rest arrive instead of sliding left on every step.
    float x = (display.x - wordWidth) * 0.5f;
    const float y = (display.y - ImGui::GetTextLineHeight()) * 0.5f;

    // Blink driven by wall clock, not frame count -- a load that hitches would
    // otherwise freeze the one element telling the player it is still working.
    const bool dotVisible = std::fmod(ImGui::GetTime(), 1.0) < 0.5;

    for (int i = 0; i < letters; ++i) {
        const char glyph[2] = { title[i], '\0' };
        const float glyphWidth = ImGui::CalcTextSize(glyph).x;
        if (i < filled) {
            draw->AddText(ImVec2(x, y), IM_COL32(236, 240, 236, 255), glyph);
        } else if (i == filled && dotVisible) {
            // Centred in the slot its letter will take, so the dot does not
            // jump sideways at the moment the letter replaces it.
            const float dotWidth = ImGui::CalcTextSize(".").x;
            draw->AddText(ImVec2(x + (glyphWidth - dotWidth) * 0.5f, y),
                          IM_COL32(236, 240, 236, 255), ".");
        }
        x += glyphWidth + tracking;
    }

    const float lineHeight = ImGui::GetTextLineHeight();
    if (bigFont) ImGui::PopFont();

    // A hairline bar under the wordmark, the width of the word and aligned to
    // it, so the two read as one mark rather than a title with a widget parked
    // beneath it. The letters say roughly how far along the load is; this says
    // it precisely, and keeps moving through the stages after the word is
    // already complete.
    const float barWidth = wordWidth;
    const float barHeight = (std::max)(3.0f, lineHeight * 0.045f);
    const float barLeft = (display.x - barWidth) * 0.5f;
    const float barTop = y + lineHeight * 1.35f;
    const ImVec2 barMin(barLeft, barTop);
    const ImVec2 barMax(barLeft + barWidth, barTop + barHeight);
    // The trough is dim rather than absent: without it the bar has no length
    // until it is nearly full, so early progress reads as nothing at all.
    draw->AddRectFilled(barMin, barMax, IM_COL32(255, 255, 255, 38));
    if (clamped > 0.0f) {
        draw->AddRectFilled(barMin,
            ImVec2(barLeft + barWidth * clamped, barMax.y),
            IM_COL32(236, 240, 236, 255));
    }
}

// The player-facing loading screen. Everything the debug screen shows --
// stage index, upload byte counts, D3D12 resource states -- is diagnostic, and
// a player waiting on a level has no use for any of it.
static void RenderPlainLoadingScreen() {
    RenderMilboxWordmark(g_game.loading.Progress());
}

static void RenderLoadingScreen() {
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

static void RenderDeathScreen(HWND hwnd) {
    if (!deathCursorReleased) {
        cameraLocked = true;
        ReleaseCapture();
        SetCursorVisible(true);
        deathCursorReleased = true;
    }
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::GetBackgroundDrawList()->AddRectFilled(
        ImVec2(0, 0), display, IM_COL32(25, 0, 0, 190));
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(380.0f, 230.0f), ImGuiCond_Always);
    ImGui::Begin("Death Screen", nullptr, ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse);
    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    const char* died = "YOU DIED";
    const float deathWidth = ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                         (deathWidth - ImGui::CalcTextSize(died).x) * 0.5f);
    ImGui::TextColored(ImVec4(1.0f, 0.28f, 0.22f, 1.0f), "%s", died);
    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    {
        ImDrawList* deathDraw = ImGui::GetWindowDrawList();
        const ImVec2 cursor = ImGui::GetCursorScreenPos();
        constexpr float ruleWidth = 44.0f;
        const float ruleX = cursor.x + (deathWidth - ruleWidth) * 0.5f;
        deathDraw->AddRectFilled(ImVec2(ruleX, cursor.y),
                                 ImVec2(ruleX + ruleWidth, cursor.y + 2.0f),
                                 IM_COL32(224, 62, 48, 235), 1.0f);
    }
    ImGui::Dummy(ImVec2(0.0f, 18.0f));
    // Restart carries the accent: on a death screen it is what the player
    // almost always wants, and making them find it among equals costs a beat.
    if (UIPrimaryButton(g_activeCustomLevelName.empty() ? "RESTART LEVEL 1" :
            "RESTART CUSTOM LEVEL"))
        RestartActiveLevel(hwnd);
    if (UIMenuButton("MAIN MENU", 40.0f))
        OpenMainMenu();
    ImGui::End();
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
    constexpr float kRowHeight = 46.0f;
    const ImVec2 rowStart = ImGui::GetCursorScreenPos();
    const float rowWidth = ImGui::GetContentRegionAvail().x;
    ImGui::PushStyleColor(ImGuiCol_Header,
                          equipped ? UITheme::kAccentDim
                                   : UITheme::kControlHeld);
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, UITheme::kControlHover);
    ImGui::PushStyleColor(ImGuiCol_HeaderActive, UITheme::kAccentDim);
    // Equipped rows stay highlighted; unaffordable ones are inert so the
    // player cannot commit to something the balance will refuse.
    ImGui::BeginDisabled(!affordable);
    const bool pressed = ImGui::Selectable("##row", equipped,
                                           ImGuiSelectableFlags_None,
                                           ImVec2(0.0f, kRowHeight));
    ImGui::EndDisabled();
    ImGui::PopStyleColor(3);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float textX = rowStart.x + 10.0f;
    // Dim the whole card when it is out of reach, so "cannot afford" reads
    // at a glance instead of only from the price.
    const ImVec4& nameColor = !affordable ? UITheme::kTextDim
                            : equipped    ? UITheme::kAccent
                                          : UITheme::kText;
    draw->AddText(ImVec2(textX, rowStart.y + 6.0f),
                  ImGui::GetColorU32(nameColor), name);

    // Price, right-aligned. Owned items show their status rather than a
    // number -- the money is already spent, and repeating the price would
    // read as a charge that is about to happen again.
    char priceText[32];
    const char* rightText = priceText;
    ImVec4 rightColor = UITheme::kWarning;
    if (equipped) {
        rightText = "EQUIPPED";
        rightColor = UITheme::kAccent;
    } else if (owned) {
        rightText = price > 0 ? "OWNED" : "ISSUED";
        rightColor = UITheme::kTextDim;
    } else {
        MoneySystem::Format(priceText, sizeof(priceText), price);
        if (!affordable) rightColor = ImVec4(0.75f, 0.32f, 0.28f, 1.0f);
    }
    const float rightWidth = ImGui::CalcTextSize(rightText).x;
    draw->AddText(
        ImVec2(rowStart.x + rowWidth - rightWidth - 10.0f,
               rowStart.y + 6.0f),
        ImGui::GetColorU32(rightColor), rightText);

    // Second line: the shop copy, or why the row cannot be taken. The
    // reason displaces the blurb rather than joining it, because a player
    // who cannot afford a row does not need to be sold on it.
    const char* subText = blurb;
    if (equipped && equippedNote) subText = equippedNote;
    else if (!affordable) subText = "Insufficient funds.";
    draw->AddText(ImVec2(textX, rowStart.y + 25.0f),
                  ImGui::GetColorU32(UITheme::kTextDim), subText);

    ImGui::PopID();
    return pressed && affordable;
}

// Deployment fly-through owns every decision that changes the run. Nothing is
// applied until DEPLOY, so restarting always returns to one authoritative plan.
static void RenderInsertionChoiceScreen(HWND hwnd) {
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
    // Unplated columns and fading scrims carry the main menu's identity over
    // the live map while keeping the briefing readable as the island rotates.
    ImDrawList* backdrop = ImGui::GetBackgroundDrawList();
    const float scrimWidth = (std::min)(display.x * 0.48f, 680.0f);
    const float solidWidth = (std::min)(display.x * 0.32f, 454.0f);
    backdrop->AddRectFilled(ImVec2(0, 0), ImVec2(solidWidth, display.y),
        IM_COL32(5, 10, 12, 250));
    backdrop->AddRectFilled(ImVec2(display.x - solidWidth, 0), display,
        IM_COL32(5, 10, 12, 250));
    backdrop->AddRectFilledMultiColor(ImVec2(solidWidth, 0), ImVec2(scrimWidth, display.y),
        IM_COL32(5, 10, 12, 250), IM_COL32(5, 10, 12, 0),
        IM_COL32(5, 10, 12, 0), IM_COL32(5, 10, 12, 250));
    backdrop->AddRectFilledMultiColor(ImVec2(display.x - scrimWidth, 0),
        ImVec2(display.x - solidWidth, display.y),
        IM_COL32(5, 10, 12, 0), IM_COL32(5, 10, 12, 250),
        IM_COL32(5, 10, 12, 250), IM_COL32(5, 10, 12, 0));
    backdrop->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(display.x, 100),
        IM_COL32(0, 0, 0, 190), IM_COL32(0, 0, 0, 190),
        IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0));
    backdrop->AddRectFilledMultiColor(ImVec2(0, display.y * 0.82f), display,
        IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0),
        IM_COL32(0, 0, 0, 170), IM_COL32(0, 0, 0, 170));

    const XMMATRIX viewProjection =
        scene.GetViewMatrix() * scene.GetProjectionMatrix();
    ImDrawList* foreground = ImGui::GetForegroundDrawList();
    const ImVec2 mouse = ImGui::GetMousePos();
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
        const float radius = selected ? 18.0f : 13.0f;
        const float dx = mouse.x - screen.x;
        const float dy = mouse.y - screen.y;
        // 24px stays comfortable at 20 zones: the tightest the markers ever get
        // on screen is 51.9px apart, so no two pick areas can overlap.
        if (selectClick && !missileClickConsumed &&
            dx * dx + dy * dy <= 24.0f * 24.0f) {
            g_selectedDeploymentZone = static_cast<int>(index);
        }
        // White markers. Selection reads as full white against a dimmer grey
        // rather than a hue change, so the two stay apart on the bright sand and
        // water the ring crosses -- the old green lost contrast over foliage.
        const ImU32 fill = selected ? IM_COL32(255, 255, 255, 245)
                                    : IM_COL32(215, 222, 228, 200);
        foreground->AddCircleFilled(screen, radius, fill);
        foreground->AddCircle(screen, radius + 3.0f,
                              IM_COL32(255, 255, 255, 240), 0, 2.0f);
        const std::string number = std::to_string(index + 1);
        const ImVec2 labelSize = ImGui::CalcTextSize(number.c_str());
        foreground->AddText(
            ImVec2(screen.x - labelSize.x * 0.5f,
                   screen.y - labelSize.y * 0.5f),
            IM_COL32(12, 16, 20, 255), number.c_str());
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
            // An authored Humvee's gunner is already represented by the vehicle
            // dot below, drawn a metre away from where he stands -- two dots for
            // one emplacement just read as two enemies. Only gunners on authored
            // Humvees (mountedVehicleIndex >= 0) are skipped: the boat gunner
            // (kBoatGunnerMount) and the stress-test gunner
            // (kStressHumveeGunnerMount) use negative sentinels and have no
            // vehicle dot standing in for them, so they keep their own.
            if (bandit->turretGunner && bandit->mountedVehicleIndex >= 0)
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
            // Dark outline first so the dot survives against the bright sand
            // the zone ring crosses, the same problem the white markers solve
            // with their ring.
            foreground->AddCircleFilled(enemyScreen, 4.5f,
                                        IM_COL32(20, 4, 6, 200));
            foreground->AddCircleFilled(enemyScreen, 3.0f,
                                        IM_COL32(232, 48, 48, 235));
        }

        // Authored Humvees, from the level plan rather than from g_bandits.
        // Their turret gunner is a bandit and would be dotted by the loop above,
        // but only once it exists: the load-stage spawn can fail (the Humvee
        // model may not be resident yet), and the retry lives behind the
        // !g_insertionChoicePending gate in the bandit update, so it cannot run
        // until AFTER the player has deployed. Dotting the vehicle itself is
        // independent of that timing -- the emplacement is on the map whether or
        // not anyone is manning it yet.
        for (const Transform& humvee : g_levelHumveeSpawns) {
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
            foreground->AddCircleFilled(humveeScreen, 6.0f,
                                        IM_COL32(20, 4, 6, 200));
            foreground->AddCircleFilled(humveeScreen, 4.0f,
                                        IM_COL32(232, 48, 48, 235));
            foreground->AddCircle(humveeScreen, 8.5f,
                                  IM_COL32(232, 48, 48, 190), 12, 1.6f);
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
            foreground->AddCircleFilled(turretScreen, 6.0f,
                                        IM_COL32(20, 4, 6, 200));
            foreground->AddCircleFilled(turretScreen, 4.0f,
                                        IM_COL32(232, 48, 48, 235));
            foreground->AddCircle(turretScreen, 8.5f,
                                  IM_COL32(232, 48, 48, 190), 12, 1.6f);
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
            // Dark outline first, as the hostile dots do: the blue would
            // otherwise disappear into the water the perimeter ring crosses.
            foreground->AddCircleFilled(allyScreen, 4.5f,
                                        IM_COL32(4, 10, 24, 200));
            foreground->AddCircleFilled(allyScreen, 3.0f,
                                        IM_COL32(70, 150, 255, 235));
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

    const char* instruction =
        "SELECT A ZONE, TWO WEAPONS, A GRENADE, AND YOUR INSERTION";
    foreground->AddText(
        ImVec2((display.x - ImGui::CalcTextSize(instruction).x) * 0.5f, 23.0f),
        IM_COL32(226, 232, 228, 236), instruction);

    // Mission briefing. Left-hand side, where the zone markers already refuse to
    // take clicks past display.x - 430, so it never fights the planning panel.
    // Drawn only when the level actually carries a tower: on a map without one
    // there is no objective to brief, and a hardcoded dossier would be a lie.
    const uint32_t standingTowers = CountStandingCommTowers();
    if (standingTowers > 0) {
        ImGui::SetNextWindowPos(ImVec2(24.0f, 110.0f),
                                ImGuiCond_Always, ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(430.0f, (std::max)(180.0f, display.y - 146.0f)), ImGuiCond_Always);
        ImGui::Begin("Mission Briefing", nullptr, ImGuiWindowFlags_NoTitleBar |
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBackground);

        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        ImGui::TextColored(ImVec4(0.45f, 0.52f, 0.48f, 1.0f),
                           "CLASSIFIED // EYES ONLY");
        ImGui::SetWindowFontScale(1.0f);
        ImGui::TextColored(UITheme::kText, "OPERATION BLACKOUT");
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        ImGui::TextColored(UITheme::kText, "SITUATION");
        ImGui::TextWrapped(
            "The garrison on this island is not fighting alone. A hardened relay "
            "mast on the ridge ties their patrols to the mainland battery. "
            "Every movement we make is called in the moment it is seen, and the "
            "guns answer within the minute.");
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        ImGui::TextColored(UITheme::kText, "PRIMARY OBJECTIVE");
        if (standingTowers == 1) {
            ImGui::TextWrapped("Destroy the communications tower.");
        } else {
            ImGui::TextWrapped("Destroy all %u communications towers.",
                               standingTowers);
        }
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        ImGui::TextColored(UITheme::kText, "EXECUTION");
        ImGui::TextWrapped(
            "The lattice shrugs off small arms. Plant remote C4 on the mast and "
            "clear the base before you trigger it. Nothing lighter will bring "
            "the structure down.");
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        ImGui::TextColored(UITheme::kText, "EXFIL");
        ImGui::TextWrapped(
            "Hold the island once the mast is down. Without the relay the "
            "battery is firing blind.");
        ImGui::Dummy(ImVec2(0.0f, 8.0f));
        ImGui::Separator();
        ImGui::TextWrapped("Mission grade weights this objective at %d of 100.",
                            MissionSystem::kPrimaryObjectiveScore);
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
        ImGui::End();
    }

    ImGui::SetNextWindowPos(ImVec2(display.x - 24.0f, 110.0f),
                            ImGuiCond_Always, ImVec2(1.0f, 0.0f));
    const float panelHeight = (std::max)(180.0f, display.y - 146.0f);
    ImGui::SetNextWindowSize(ImVec2(430.0f, panelHeight), ImGuiCond_Always);
    ImGui::Begin("Deployment Planning", nullptr, ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBackground);
    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    const char* title = "DEPLOYMENT PLAN";
    ImGui::TextColored(UITheme::kTextDim, "MISSION PREPARATION // 01");
    ImGui::TextColored(UITheme::kText, "%s", title);
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0.0f, 12.0f));
    if (g_selectedDeploymentZone >= 0)
        ImGui::TextColored(UITheme::kText,
            "Zone %d selected", g_selectedDeploymentZone + 1);
    else
        ImGui::TextColored(UITheme::kText,
            "Click a zone marker on the map");
    ImGui::Dummy(ImVec2(0.0f, 7.0f));

    // Every section below is a dropdown. The panel carries the armory, the
    // conditions, the insertion and the intel on one 430 px column, which is far
    // more than fits on screen at once -- collapsing them means the player opens
    // the one department they are actually shopping in instead of scrolling past
    // the other four. Armory and insertion default open because a plan is not
    // valid without them; the rest start closed.
    const auto deploySection = [&](const char* label, bool defaultOpen = false) {
        return ImGui::CollapsingHeader(
            label, defaultOpen ? ImGuiTreeNodeFlags_DefaultOpen : 0);
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
        if (ImGui::Button("Reset diagnostics")) {
            g_deploymentDebugHideWater = false;
            g_deploymentDebugHideAtmosphere = false;
            g_deploymentDebugForceForward = false;
            g_deploymentDebugWaterDepth = false;
            g_deploymentDebugCoverageGuides = false;
            g_deploymentDebugHideAO = false;
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

    MissionLoadout& loadout = g_game.mission.Loadout();
    static constexpr const char* weaponNames[MissionLoadout::kWeaponCount] = {
        "AK47", "Remington 870", "RPG-7", "R700 Sniper",
        "ARC Laser Cutter", "Remote C4", "M2 Flamethrower",
        "Mako Harpoon Gun", "R700 Suppressed", "M4A1", "AK-74", "M9"
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
    if (deploySection("ARMORY", true)) {

    {
        char balanceText[32];
        MoneySystem::Format(balanceText, sizeof(balanceText),
                            g_game.money.Balance());
        // Wallet header. Sits above the departments rather than beside the
        // DEPLOY button because it is the number every price below is checked
        // against -- a player scanning a row for affordability should not have
        // to look to the other end of the panel.
        ImGui::PushStyleColor(ImGuiCol_ChildBg, UITheme::kPanelRaised);
        ImGui::BeginChild("ArmoryWallet", ImVec2(0.0f, 42.0f), true);
        ImGui::TextColored(UITheme::kTextDim, "OPERATING FUNDS");
        ImGui::SameLine();
        const float balanceWidth = ImGui::CalcTextSize(balanceText).x;
        ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - balanceWidth);
        ImGui::TextColored(UITheme::kWarning, "%s", balanceText);
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    const auto armoryRow = [&](const char* name, const char* blurb, int price,
                               bool owned, bool equipped,
                               const char* equippedNote) {
        return DrawArmoryRow(name, blurb, price, owned, equipped, equippedNote);
    };

    // ---- Weapons -----------------------------------------------------------
    // Which slot a purchase fills. A shop row cannot know whether the player
    // means it as their primary or their secondary, so the slot is a mode the
    // player sets first and the rows then fill.
    static int armorySlot = 0;
    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    ImGui::TextColored(UITheme::kTextDim, "SMALL ARMS");
    {
        const auto slotTab = [&](const char* label, int slot) {
            const bool active = armorySlot == slot;
            if (active) {
                ImGui::PushStyleColor(ImGuiCol_Button, UITheme::kAccentDim);
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, UITheme::kAccent);
            }
            char tabText[64];
            const int weapon = loadout.weapons[static_cast<size_t>(slot)];
            std::snprintf(tabText, sizeof(tabText), "%s: %s", label,
                          (weapon >= 0 && weapon < MissionLoadout::kWeaponCount)
                              ? weaponNames[weapon] : "--");
            if (ImGui::Button(tabText, ImVec2(196.0f, 30.0f)))
                armorySlot = slot;
            if (active) ImGui::PopStyleColor(2);
        };
        slotTab("Slot 1", 0);
        ImGui::SameLine();
        slotTab("Slot 2", 1);
    }
    ImGui::TextDisabled("Buying a weapon racks it in the highlighted slot.");
    ImGui::Dummy(ImVec2(0.0f, 4.0f));

    for (int weapon = 0; weapon < MissionLoadout::kWeaponCount; ++weapon) {
        // Same gate the old combo used: hidden and debug weapons are not stock.
        if (!GunModel::WeaponLoaded(weapon)) continue;
        const int price = ArmoryCatalog::WeaponPrice(weapon);
        const bool owned = ArmoryWeaponOwned(weapon);
        // Equipped means "in the slot this shop is currently filling". A weapon
        // held in the other slot still shows as owned and pickable, and taking
        // it swaps the two -- SelectWeapon already does exactly that.
        const bool equipped =
            loadout.weapons[static_cast<size_t>(armorySlot)] == weapon;
        const char* note = loadout.ContainsWeapon(weapon) && !equipped
            ? "Racked in the other slot."
            : ArmoryCatalog::WeaponBlurb(weapon);
        if (armoryRow(weaponNames[weapon], note, price, owned, equipped,
                      "Racked in this slot.")) {
            if (owned || ArmoryPurchase(price)) {
                g_ownedWeapons |= (1u << static_cast<uint32_t>(weapon));
                loadout.SelectWeapon(static_cast<size_t>(armorySlot), weapon);
            }
        }
    }

    // ---- Attachments -------------------------------------------------------
    // Priced per part and owned by attachment id: buying a suppressor once
    // stocks it for every weapon it fits, which is how a quartermaster's shelf
    // works and avoids charging twice for the same part.
    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    ImGui::TextColored(UITheme::kTextDim, "WEAPON ATTACHMENTS");
    const auto drawWeaponAttachments = [&](const char* slotLabel, int weapon) {
        ImGui::PushID(slotLabel);
        ImGui::TextDisabled("%s -- %s", slotLabel,
                            (weapon >= 0 && weapon < MissionLoadout::kWeaponCount)
                                ? weaponNames[weapon] : "--");
        bool anyCompatible = false;
        for (const SGE::AttachmentDefinition& attachment :
             scene.player.weapons.Attachments()) {
            if (!attachment.CompatibleWith(weapon)) continue;
            anyCompatible = true;
            const int price = ArmoryCatalog::AttachmentPrice(
                attachment.suppressesWeapon, attachment.providesRedDot,
                attachment.providesLaser);
            const bool owned = ArmoryAttachmentOwned(attachment.id);
            const bool installed = scene.player.weapons.AttachmentInstalled(
                weapon, attachment.id);
            const char* blurb =
                attachment.suppressesWeapon
                    ? "Quieter report, smaller flash, slightly less recoil."
                : attachment.providesRedDot
                    ? "Clear red aiming point and a tighter sight picture."
                : attachment.providesLaser
                    ? "Visible designator and tighter hip-fire spread."
                    : "Fitted accessory.";
            if (armoryRow(attachment.displayName.c_str(), blurb, price, owned,
                          installed, "Fitted. Select to remove.")) {
                if (installed) {
                    // Removal is free and does not refund: the part is owned,
                    // and taking it off a rail is not selling it back.
                    scene.player.weapons.RemoveAttachment(weapon,
                                                          attachment.slot);
                } else if (owned || ArmoryPurchase(price)) {
                    g_ownedAttachments.insert(attachment.id);
                    scene.player.weapons.EquipAttachment(weapon, attachment.id);
                }
            }
        }
        if (!anyCompatible)
            ImGui::TextDisabled("No attachment rails available.");
        ImGui::PopID();
    };
    drawWeaponAttachments("Slot 1", loadout.weapons[0]);
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    drawWeaponAttachments("Slot 2", loadout.weapons[1]);

    // ---- Grenades ----------------------------------------------------------
    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    ImGui::TextColored(UITheme::kTextDim, "ORDNANCE");
    for (int index = 0; index < ArmoryCatalog::kGrenadeCount; ++index) {
        const GrenadeType type = static_cast<GrenadeType>(index);
        const int price = ArmoryCatalog::kGrenadePrices[index];
        const bool owned = ArmoryGrenadeOwned(type);
        const bool equipped = loadout.grenade == type;
        if (armoryRow(ArmoryCatalog::kGrenadeNames[index],
                      ArmoryCatalog::kGrenadeBlurbs[index], price, owned,
                      equipped, "Carried on this mission.")) {
            if (owned || ArmoryPurchase(price)) {
                g_ownedGrenades |= (1u << static_cast<uint32_t>(index));
                loadout.grenade = type;
                scene.selectedGrenade = type;
            }
        }
    }

    // ---- Gear --------------------------------------------------------------
    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    ImGui::TextColored(UITheme::kTextDim, "FIELD GEAR");
    for (int index = 0; index < ArmoryCatalog::kGearCount; ++index) {
        const GearType type = static_cast<GearType>(index);
        const int price = ArmoryCatalog::kGearPrices[index];
        const bool owned = ArmoryGearOwned(type);
        const bool equipped = loadout.gear == type;
        if (armoryRow(ArmoryCatalog::kGearNames[index],
                      ArmoryCatalog::kGearBlurbs[index], price, owned,
                      equipped, "Carried on this mission.")) {
            if (owned || ArmoryPurchase(price)) {
                g_ownedGear |= (1u << static_cast<uint32_t>(index));
                loadout.gear = type;
            }
        }
    }
    // Usage notes for the equipped item, kept below the shelf rather than on
    // the row: they are about using the gear, not about buying it.
    if (loadout.gear == GearType::NightVisionGoggles) {
        ImGui::TextDisabled("Press J in the field to raise or lower goggles.");
        if (!TimeOfDayIsDark(g_selectedTimeOfDay))
            ImGui::TextDisabled("Little use at this time of day.");
    }
    if (loadout.gear == GearType::Flashlight) {
        ImGui::TextDisabled("Press J in the field to switch the weapon light.");
        ImGui::TextDisabled("Lights where you aim -- and shows enemies where you are.");
        if (!TimeOfDayIsDark(g_selectedTimeOfDay))
            ImGui::TextDisabled("Little use at this time of day.");
    }

    // C4 is demolition kit rather than a weapon pick, so it is carried on every
    // mission without spending a slot or a cent -- otherwise a player who chose
    // two rifles would have no way to take down a demolition objective.
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    ImGui::TextDisabled("Remote C4 is issued free on every mission.");
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
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
    // Difficulty cheats. God mode and the enemy damage scale are debug switches
    // rather than plan choices, so they are drawn from the DEBUG section at the
    // bottom of the panel instead of inline here.
    const auto drawDifficultyControls = [&]() {
    ImGui::SeparatorText("DIFFICULTY");
    // Per-run choice rather than a launcher-level mode. Applied straight to the
    // live player state because StartLevelOne has already run by the time this
    // screen is up -- its godMode parameter set the value this toggle edits.
    //
    // God mode is not only invulnerability: PlayerState::AmmoEnforced() is tied
    // to it, so turning it on also disables magazines, reserves and reloading.
    // That is why the label says supplies rather than just damage.
    if (ImGui::Checkbox("God mode (no damage, unlimited ammo)",
                        &scene.player.godMode)) {
        // Coming back from god mode leaves the magazines in whatever state the
        // unenforced path left them, so restock to a clean loadout.
        if (!scene.player.godMode) scene.player.RestoreAmmo();
    }
    if (scene.player.godMode)
        ImGui::TextDisabled(
            "All weapons stay available and ammo is not tracked.");
    else
        ImGui::TextDisabled(
            "Ammo is limited to your two weapons. Reload with R.");

    // Enemy lethality. Scales incoming fire, grenades and molotovs only --
    // falls, crashes and your own grenades are unaffected, so this stays a
    // statement about the enemy rather than a global fragility slider.
    ImGui::Dummy(ImVec2(0.0f, 2.0f));
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::SliderFloat("Enemy damage##DeploymentEnemyDamage",
                       &scene.enemyDamageMultiplier, 0.25f, 3.0f, "x%.2f");
    {
        // Quote the practical consequence rather than the raw factor: rifle
        // chip damage is the number the player actually feels, and shots-to-kill
        // is what the multiplier really changes.
        const float perShot = 2.4f * scene.enemyDamageMultiplier;
        const int shots = perShot > 0.0f
            ? static_cast<int>(std::ceil(scene.player.maxHealth / perShot))
            : 0;
        if (scene.player.godMode)
            ImGui::TextDisabled("No effect while god mode is on.");
        else
            ImGui::TextDisabled(
                "%.1f damage per rifle hit -- %d to drop you from full health.",
                perShot, shots);
    }
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    };

    // Time of day. Applied live as it is picked rather than waiting for DEPLOY,
    // so the planning fly-through shows the light the run will actually be
    // fought in -- picking Night and then dropping into daylight would make the
    // choice meaningless on the one screen that can preview it.
    // One roll for every condition below: time, weather, fog density and the
    // enemy layout. Sits above them so it reads as covering the whole section,
    // and the controls it drives stay editable afterwards -- a roll is a
    // starting point, not a lock.
    if (deploySection("RANDOMIZE")) {
    ImGui::SetCursorPosX(45.0f);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.42f, 0.28f, 0.10f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                          ImVec4(0.54f, 0.36f, 0.14f, 1.0f));
    if (ImGui::Button("RANDOMIZE CONDITIONS", ImVec2(340.0f, 34.0f)))
        g_lastDeploymentRollSeed = RandomizeDeployment();
    ImGui::PopStyleColor(2);
    if (g_lastDeploymentRollSeed != 0)
        ImGui::TextDisabled("Roll seed %u -- reuse it to replay this layout.",
                            g_lastDeploymentRollSeed);
    else
        ImGui::TextDisabled("Rolls time, weather, fog and enemy positions.");
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    }

    if (deploySection("TIME OF DAY")) {
    const auto timeButton = [&](TimeOfDay time, float width) {
        const bool selected = g_selectedTimeOfDay == time;
        if (selected) {
            // Theme accent, so "this option is chosen" looks the same here as it
            // does everywhere else in the UI.
            ImGui::PushStyleColor(ImGuiCol_Button, UITheme::kAccentDim);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, UITheme::kAccent);
        }
        if (ImGui::Button(TimeOfDayName(time), ImVec2(width, 30.0f)) &&
            !selected) {
            g_selectedTimeOfDay = time;
            ApplyTimeOfDay(time);
        }
        if (selected) ImGui::PopStyleColor(2);
    };
    // Two rows of two: four across a 430 px panel leaves the labels cramped.
    constexpr float kTimeButtonWidth = 166.0f;
    ImGui::SetCursorPosX(45.0f);
    timeButton(TimeOfDay::Noon, kTimeButtonWidth);
    ImGui::SameLine();
    timeButton(TimeOfDay::Afternoon, kTimeButtonWidth);
    ImGui::SetCursorPosX(45.0f);
    timeButton(TimeOfDay::Dusk, kTimeButtonWidth);
    ImGui::SameLine();
    timeButton(TimeOfDay::Night, kTimeButtonWidth);
    ImGui::TextWrapped("%s", TimeOfDayBriefing(g_selectedTimeOfDay));
    }

    if (deploySection("WEATHER")) {
    static constexpr const char* kWeatherNames[] = {
        "Clear", "Cloudy", "Dense Fog", "Rain", "Storm", "Custom"
    };
    int weatherIndex = static_cast<int>(scene.weatherState);
    ImGui::SetNextItemWidth(-1.0f);
    if (ImGui::Combo("State##DeploymentWeather", &weatherIndex,
                     kWeatherNames, IM_ARRAYSIZE(kWeatherNames))) {
        ApplyLiveWeatherState(static_cast<WeatherState>(weatherIndex));
    }
    ImGui::TextWrapped("%s", WeatherStateBriefing(scene.weatherState));
    // Spell out the perception the choice actually buys. This used to say
    // enemies see no better in the dark, which was true when the sight range
    // was a constant and is now the opposite of what happens.
    {
        TimeOfDaySettings preview =
            MakeTimeOfDaySettings(g_selectedTimeOfDay);
        const VolumetricFogSettings& previewFog =
            VolumetricFogFor(g_selectedTimeOfDay);
        preview.enableVolumetricFog = previewFog.enabled;
        preview.volumetricFogDensity = previewFog.density;
        preview.volumetricFogDistance = previewFog.distance;
        const float visibility = TimeOfDayVisibilityFactor(preview);
        ImGui::TextDisabled(
            "Enemy sight range %.0f%% of daylight (~%.0f m).",
            visibility * 100.0f,
            SkinnedEnemy::BaseVisionRange() * visibility);
        if (visibility < 0.5f)
            ImGui::TextDisabled(
                "They still hear gunfire at full range.");
    }
    }

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
    if (deploySection("INSERTION", true)) {

    const auto modeButton = [&](const char* label, LevelInsertionMode mode) {
        const bool selected = g_playerInsertionChoice == mode;
        if (selected) {
            ImGui::PushStyleColor(
                ImGuiCol_Button, ImVec4(0.12f, 0.48f, 0.22f, 1.0f));
            ImGui::PushStyleColor(
                ImGuiCol_ButtonHovered, ImVec4(0.16f, 0.58f, 0.28f, 1.0f));
        }
        ImGui::SetCursorPosX(45.0f);
        if (ImGui::Button(label, ImVec2(340.0f, 36.0f)))
            g_playerInsertionChoice = mode;
        if (selected) ImGui::PopStyleColor(2);
    };
    modeButton("HELICOPTER LANDING", LevelInsertionMode::Helicopter);
    ImGui::Dummy(ImVec2(0.0f, 3.0f));
    modeButton("FAST HELI RAPPEL (-50% AMMO)", LevelInsertionMode::FastRappel);
    ImGui::Dummy(ImVec2(0.0f, 3.0f));
    modeButton("BOAT INSERTION", LevelInsertionMode::Boat);
    ImGui::Dummy(ImVec2(0.0f, 5.0f));

    // Door choice. Only the two helicopter runs have cabin seats, so the row is
    // hidden on a boat insertion rather than shown greyed out -- the panel is
    // already long and a dead control there would just be noise.
    if (g_playerInsertionChoice == LevelInsertionMode::Helicopter ||
        g_playerInsertionChoice == LevelInsertionMode::FastRappel) {
        // Airframe choice, on the same guard as the seat row and for the same
        // reason: it means nothing on a boat insertion. Hidden entirely when
        // only one aircraft loaded, so a missing optional asset shows no
        // control rather than a row with a single dead button.
        if (InsertionAirframeChoiceAvailable()) {
            ImGui::SetCursorPosX(45.0f);
            ImGui::TextDisabled("AIRFRAME");
            const auto airframeButton = [&](const char* label,
                                            InsertionAirframe airframe,
                                            bool sameLine) {
                const bool selected = g_insertionAirframe == airframe;
                if (selected) {
                    ImGui::PushStyleColor(
                        ImGuiCol_Button, ImVec4(0.12f, 0.48f, 0.22f, 1.0f));
                    ImGui::PushStyleColor(
                        ImGuiCol_ButtonHovered, ImVec4(0.16f, 0.58f, 0.28f, 1.0f));
                }
                if (sameLine) ImGui::SameLine();
                else ImGui::SetCursorPosX(45.0f);
                if (ImGui::Button(label, ImVec2(166.0f, 30.0f)))
                    ApplyInsertionAirframe(airframe);
                if (selected) ImGui::PopStyleColor(2);
            };
            airframeButton("UH-60", InsertionAirframe::BlackHawk, false);
            airframeButton("MH-60", InsertionAirframe::NewBlackHawk, true);
            ImGui::Dummy(ImVec2(0.0f, 5.0f));
        }
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
    const int affordableMarines = (std::min)(
        kMaxDeploymentMarines,
        static_cast<int>(g_game.money.Balance() / kDeploymentMarinePrice));
    // Clamp before drawing, not after: an armory purchase made after the squad
    // was picked can put the count out of reach, and charging for marines the
    // player can no longer pay for is the one outcome this must never allow.
    g_deploymentMarineCount =
        (std::min)(g_deploymentMarineCount, affordableMarines);
    ImGui::BeginDisabled(!marinesAvailable || affordableMarines <= 0);
    ImGui::SetCursorPosX(45.0f);
    ImGui::SetNextItemWidth(340.0f);
    ImGui::SliderInt("##DeploymentMarines", &g_deploymentMarineCount,
                     0, (std::max)(1, affordableMarines),
                     g_deploymentMarineCount == 1 ? "%d marine"
                                                  : "%d marines");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Marines loaded aboard your transport, $2,000 each.\n"
            "Charged when you deploy, and spent for good -- they do\n"
            "not carry over to the next mission like a weapon does.\n"
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
    } else if (g_deploymentMarineCount > 0) {
        // Quote the total rather than the unit price: the decision being made
        // here is how big a cheque to write, not what one marine goes for.
        char squadCost[32];
        MoneySystem::Format(squadCost, sizeof(squadCost),
                            g_deploymentMarineCount * kDeploymentMarinePrice);
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

    if (deploySection("OBJECTIVES")) {
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
    if (deploySection("ENEMY INTEL")) {
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
    if (g_scatterEnemiesLastSeed != 0)
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
    if (deploySection("FIRE SUPPORT")) {
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
    // place at the foot of the panel: render diagnostics, the difficulty cheats,
    // the fog tuning sliders and the development weapon stock. Closed by default,
    // and last so it never sits between the player and the DEPLOY button.
    if (deploySection("DEBUG")) {
        if (ImGui::CollapsingHeader("Render diagnostics"))
            drawRenderDiagnostics();
        if (ImGui::CollapsingHeader("Difficulty"))
            drawDifficultyControls();
        if (ImGui::CollapsingHeader("Volumetric fog"))
            drawVolumetricFogControls();
        if (ImGui::CollapsingHeader("Weapon stock"))
            drawDebugWeaponToggle();
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
    }

    ImGui::BeginDisabled(g_selectedDeploymentZone < 0 || !weaponsReady);
    ImGui::SetCursorPosX(45.0f);
    // The one action this whole screen exists to reach, so it carries the accent
    // colour. Every other control here only edits the plan.
    ImGui::PushStyleColor(ImGuiCol_Button, UITheme::kAccentDim);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, UITheme::kAccent);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, UITheme::kAccent);
    const bool deployPressed = ImGui::Button("DEPLOY", ImVec2(340.0f, 48.0f));
    ImGui::PopStyleColor(3);
    if (deployPressed) {
        // Pay for the squad. Charged here rather than on the slider because
        // moving a slider is the player looking at a price, not agreeing to it;
        // this press is the commitment. ArmoryPurchase re-checks the balance,
        // so a squad that became unaffordable between the pick and the press is
        // dropped to nothing rather than deploying for free.
        if (g_deploymentMarineCount > 0) {
            const int squadPrice =
                g_deploymentMarineCount * kDeploymentMarinePrice;
            if (ArmoryPurchase(squadPrice)) {
                SaveCareer();
            } else {
                g_deploymentMarineCount = 0;
            }
        }
        // Repair a loadout that still names a weapon which is no longer
        // selectable -- a debug weapon picked before the toggle was switched
        // off, or a save carrying the retired AK-47. ConfigureLoadout rejects
        // those outright, which would silently leave the previous mission's
        // restriction in force, and the god-mode branch would hand the player a
        // weapon with no viewmodel.
        for (size_t slot = 0; slot < loadout.weapons.size(); ++slot) {
            if (GunModel::WeaponLoaded(loadout.weapons[slot])) continue;
            for (int candidate = 0; candidate <= GunModel::kMaxWeapon;
                 ++candidate) {
                if (!GunModel::WeaponLoaded(candidate) ||
                    loadout.ContainsWeapon(candidate))
                    continue;
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
        // Re-applied on the way out even though picking already applied it: the
        // planning screen is not the only thing that touches the lights, and
        // this is the last point before the run where the choice is authoritative.
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
        // Arm the objective against what this level actually spawned, after any
        // prefab edits made during planning. Must follow ResetRun, which the
        // level load already did -- doing it here means a restart re-counts.
        g_game.mission.SetCommTowerCount(standingTowers);
        // Arm the aircraft here rather than at level load: the countdown has to
        // start when the player actually deploys, or the whole 20 seconds would
        // burn while they were still choosing an insertion point.
        ArmObjectivePlanes();
        // The planning camera teleports to the selected insertion. None of the
        // fly-through's temporal lighting or visibility history is valid there.
        visBuffer.InvalidateTemporalHistory();
        g_game.commands.Request(GameCommand::ResetDDGIHistory);
        SetCapture(hwnd);
        SetCursorVisible(false);
        firstMouse = true;
    }
    ImGui::EndDisabled();
    ImGui::End();
}

static void RenderWinScreen(HWND hwnd) {
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::GetBackgroundDrawList()->AddRectFilled(
        ImVec2(0, 0), display, IM_COL32(0, 22, 12, 215));
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    const float panelHeight = (std::min)(640.0f, display.y - 20.0f);
    ImGui::SetNextWindowSize(ImVec2(580.0f, panelHeight), ImGuiCond_Always);
    ImGui::Begin("Win Screen", nullptr, ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse);
    const MissionReport& report = g_game.mission.Report();
    const MissionLoadout& loadout = g_game.mission.Loadout();
    const MissionRunStats& stats = g_game.mission.Stats();
    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    // Extracting with the mast still up is not a clean run, so the banner says
    // so rather than congratulating a player who skipped the primary objective.
    const bool primaryFailed = report.primaryObjectivePresent &&
                               !report.primaryObjectiveComplete;
    const char* won = primaryFailed ? "OBJECTIVE FAILED" : "MISSION COMPLETE";
    const float winWidth = ImGui::GetContentRegionAvail().x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                         (winWidth - ImGui::CalcTextSize(won).x) * 0.5f);
    ImGui::TextColored(primaryFailed ? ImVec4(1.0f, 0.42f, 0.20f, 1.0f)
                                     : UITheme::kAccent,
                       "%s", won);

    // Rank as the hero element, with the score as a filled track beneath it.
    // The bar is what turns "68 / 100" into something readable at a glance --
    // the number alone gave no sense of how close the next grade was.
    const char* rank = MissionRankName(report.rank);
    ImGui::Dummy(ImVec2(0.0f, 4.0f));
    ImGui::SetWindowFontScale(2.6f);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                         (winWidth - ImGui::CalcTextSize(rank).x) * 0.5f);
    ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.24f, 1.0f), "%s", rank);
    ImGui::SetWindowFontScale(1.0f);

    {
        char scoreText[32];
        snprintf(scoreText, sizeof(scoreText), "%d / 100", report.totalScore);
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                             (winWidth - ImGui::CalcTextSize(scoreText).x) * 0.5f);
        ImGui::TextUnformatted(scoreText);

        ImDrawList* winDraw = ImGui::GetWindowDrawList();
        const ImVec2 cursor = ImGui::GetCursorScreenPos();
        const float trackWidth = winWidth * 0.62f;
        const float trackX = cursor.x + (winWidth - trackWidth) * 0.5f;
        const float scoreFraction = (std::max)(0.0f, (std::min)(1.0f,
            static_cast<float>(report.totalScore) / 100.0f));
        winDraw->AddRectFilled(ImVec2(trackX, cursor.y + 3.0f),
                               ImVec2(trackX + trackWidth, cursor.y + 9.0f),
                               IM_COL32(6, 10, 8, 215), 3.0f);
        if (scoreFraction > 0.0f)
            winDraw->AddRectFilled(
                ImVec2(trackX, cursor.y + 3.0f),
                ImVec2(trackX + trackWidth * scoreFraction, cursor.y + 9.0f),
                IM_COL32(255, 209, 61, 245), 3.0f);
        ImGui::Dummy(ImVec2(0.0f, 14.0f));
    }

    // ---- Payout ------------------------------------------------------------
    //
    // What the run was worth, sitting directly under the grade because the
    // grade is what sets the bonus. Split into "earned in the field" and
    // "bonus from the grade" so it is obvious that shooting things and scoring
    // well are two separate income streams, then the new career balance.
    ImGui::SeparatorText("PAYOUT");
    {
        const int64_t fieldEarnings = g_winScreenPayout - g_winScreenMissionBonus;
        const auto moneyRow = [](const char* label, int64_t amount,
                                 const ImVec4& tint) {
            char text[32];
            MoneySystem::Format(text, sizeof(text), amount);
            ImGui::TextUnformatted(label);
            const float textWidth = ImGui::CalcTextSize(text).x;
            ImGui::SameLine(ImGui::GetContentRegionMax().x - textWidth);
            ImGui::TextColored(tint, "%s", text);
        };
        moneyRow("Field earnings", fieldEarnings,
                 ImVec4(0.86f, 0.90f, 0.88f, 1.0f));
        char bonusLabel[64];
        snprintf(bonusLabel, sizeof(bonusLabel), "Mission bonus (%d x %d)",
                 report.totalScore, MoneySystem::kMissionBonusPerScorePoint);
        moneyRow(bonusLabel, g_winScreenMissionBonus,
                 ImVec4(0.86f, 0.90f, 0.88f, 1.0f));
        ImGui::Separator();
        moneyRow("TOTAL THIS RUN", g_winScreenPayout,
                 ImVec4(1.0f, 0.82f, 0.24f, 1.0f));
        moneyRow("Funds", g_game.money.Balance(), UITheme::kTextDim);
    }

    ImGui::SeparatorText("DEPLOYMENT");
    ImGui::Text("%s + %s", GunModel::WeaponName(loadout.weapons[0]),
                GunModel::WeaponName(loadout.weapons[1]));
    ImGui::TextDisabled("%s | %s | %s", GrenadeTypeName(loadout.grenade),
                        GearTypeName(loadout.gear),
                        LevelInsertionModeName(loadout.insertion));

    const int totalMilliseconds = static_cast<int>(
        report.elapsedSeconds * 1000.0f + 0.5f);
    const int minutes = totalMilliseconds / 60000;
    const int seconds = (totalMilliseconds / 1000) % 60;
    const int milliseconds = totalMilliseconds % 1000;
    ImGui::SeparatorText("SCORE BREAKDOWN");
    // Right-aligns the "N / M" column against the panel edge. The old fixed
    // SameLine(410) was measured for one exact window width and padding, so any
    // change to either left the column floating mid-row.
    const auto scoreColumn = [](int earned, int available) {
        char text[24];
        snprintf(text, sizeof(text), "%d / %d", earned, available);
        const float textWidth = ImGui::CalcTextSize(text).x;
        ImGui::SameLine(ImGui::GetContentRegionMax().x - textWidth);
        // Dim a category that scored nothing, so the ones worth improving stand
        // out from the ones already banked.
        if (earned <= 0) ImGui::TextDisabled("%s", text);
        else ImGui::TextUnformatted(text);
    };
    ImGui::Text("Time             %02d:%02d.%03d", minutes, seconds, milliseconds);
    scoreColumn(report.timeScore, 15);
    ImGui::Text("Accuracy         %.1f%%  (%u / %u)", report.accuracyPercent,
                stats.shotsHit, stats.shotsFired);
    scoreColumn(report.accuracyScore, 20);
    ImGui::Text("Casualties       %u / %u", report.casualties,
                stats.friendliesDeployed);
    scoreColumn(report.casualtyScore, 15);
    ImGui::Text("Optional         %u / %u", report.optionalObjectivesCompleted,
                report.optionalObjectivesTotal);
    scoreColumn(report.optionalScore, 15);
    ImGui::Text("Destruction      %u events", report.destructionEvents);
    scoreColumn(report.destructionScore, 10);
    if (report.objectivePlanesTotal > 0) {
        if (report.objectivePlanesEscaped > 0)
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f),
                "Aircraft         %u / %u downed, %u ESCAPED",
                report.objectivePlanesDestroyed, report.objectivePlanesTotal,
                report.objectivePlanesEscaped);
        else
            ImGui::Text("Aircraft         %u / %u downed",
                        report.objectivePlanesDestroyed,
                        report.objectivePlanesTotal);
    }
    if (report.primaryObjectivePresent) {
        ImGui::Text("Comm towers      %u / %u down", report.commTowersDestroyed,
                    report.commTowersTotal);
    } else {
        ImGui::Text("Comm towers      none on this map");
    }
    scoreColumn(report.primaryScore, MissionSystem::kPrimaryObjectiveScore);

    const auto objective = [](bool complete, const char* label) {
        ImGui::TextColored(complete
                ? ImVec4(0.35f, 1.0f, 0.45f, 1.0f)
                : ImVec4(0.58f, 0.62f, 0.60f, 1.0f),
            "%s  %s", complete ? "[X]" : "[ ]", label);
    };
    if (report.primaryObjectivePresent) {
        ImGui::SeparatorText("PRIMARY OBJECTIVE");
        objective(report.primaryObjectiveComplete,
                  "Operation Blackout - level the comm tower");
    }

    ImGui::SeparatorText("OPTIONAL OBJECTIVES");
    objective(report.usedBothWeapons, "Field versatility - use both weapons");
    objective(report.usedSelectedGrenade, "Grenadier - throw selected grenade");
    objective(report.demolitionObjective, "Demolition - cause 5 destruction events");

    ImGui::Dummy(ImVec2(0.0f, 12.0f));
    // Split the row from the live content width instead of two 215 px halves at
    // a hardcoded inset, so the pair stays centred if the panel is ever resized.
    {
        const float rowWidth = ImGui::GetContentRegionAvail().x;
        const float halfWidth =
            (rowWidth - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
        ImGui::PushStyleColor(ImGuiCol_Button, UITheme::kAccentDim);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, UITheme::kAccent);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, UITheme::kAccent);
        const bool replay = ImGui::Button(
            g_activeCustomLevelName.empty() ? "REPLAY LEVEL 1"
                                            : "REPLAY CUSTOM LEVEL",
            ImVec2(halfWidth, 44.0f));
        ImGui::PopStyleColor(3);
        if (replay) RestartActiveLevel(hwnd);
        ImGui::SameLine();
        // A finished run ends by flying home, not by dropping out to the menu.
        // The base is where the payout above is actually spent, so send the
        // player straight there; the menu is still one Escape away from it.
        if (ImGui::Button("RETURN TO BASE", ImVec2(halfWidth, 44.0f)))
            StartBase(hwnd);
    }
    ImGui::End();
}
