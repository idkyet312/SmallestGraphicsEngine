#pragma once

// Private application implementation; included once by main.cpp in dependency order.

static void ApplyVirtualInput() {
    if (!HasInputFocus()) return;
    Camera& cam = scene.camera;
    if (virtualInput.moveY > 0.0f) cam.ProcessKeyboard('W', deltaTime,  virtualInput.moveY);
    if (virtualInput.moveY < 0.0f) cam.ProcessKeyboard('S', deltaTime, -virtualInput.moveY);
    if (virtualInput.moveX < 0.0f) cam.ProcessKeyboard('A', deltaTime, -virtualInput.moveX);
    if (virtualInput.moveX > 0.0f) cam.ProcessKeyboard('D', deltaTime,  virtualInput.moveX);
    if (virtualInput.down)    cam.ProcessKeyboard('Q', deltaTime);
    if (virtualInput.jump)    cam.ProcessKeyboard(' ', deltaTime);

    if (virtualInput.lookX != 0.0f || virtualInput.lookY != 0.0f) {
        constexpr float thumbstickLookMultiplier = 3.0f;
        cam.ProcessMouseMovement(
            virtualInput.lookX * virtualInput.lookSpeed * thumbstickLookMultiplier * deltaTime,
            virtualInput.lookY * virtualInput.lookSpeed * thumbstickLookMultiplier * deltaTime);
    }

    if (virtualInput.shoot) {
        scene.fireCooldown -= deltaTime;
        if (scene.fireCooldown <= 0.0f && ShootPlayerWeapon())
            scene.fireCooldown = PlayerFireInterval();
    }

    // Jump is a one-shot: consume it here so a single click is a single jump.
    // The held/analog flags are rebuilt from scratch by the pad in RenderUI,
    // which runs later in the frame -- don't clear them here or they'd be wiped
    // before ever being applied.
    virtualInput.jump = false;
}

static void ProcessInput(HWND) {
    scene.weaponAdsFOV = scene.player.ResolveWeaponStats(
        GunModel::SelectedWeapon()).adsFovDegrees;
    if (g_insertionChoicePending) {
        scene.c4DetonateHeld = false;
        scene.UpdateSniperScope(false, deltaTime);
        scene.UpdateAimDownSights(false, deltaTime);
        return;
    }
    // Right mouse raises every weapon through the same ADS path. The R700 also
    // drives its secondary scope camera, but magnification stays on the physical
    // lens rather than replacing the player's main view.
    const bool rightMouseHeld =
        (FocusedKeyState(VK_RBUTTON) & 0x8000) != 0;
    const bool c4DetonateRequested = IsGameplayScreen() &&
        scene.player.health > 0.0f && !g_drivingHumvee &&
        !cameraLocked && !ImGui::GetIO().WantCaptureMouse &&
        GunModel::C4Selected() && rightMouseHeld;
    if (c4DetonateRequested && !scene.c4DetonateHeld) {
        // Record which towers were rigged before the charges are cleared.
        // DetonateRemoteCharges empties scene.remoteCharges immediately, but the
        // blast it queues is not resolved until the projectile pass later this
        // frame -- by which time the charge that authorised the demolition is
        // already gone. See CommTowerDamageAllowed.
        MarkCommTowersRiggedForDemolition();
        scene.DetonateRemoteCharges();
    }
    scene.c4DetonateHeld = c4DetonateRequested;

    // In mouse-walk mode the right button is the throttle, so it must not also
    // raise the sights -- otherwise every step forward would be taken aiming.
    const bool aimRequested = IsGameplayScreen() &&
        scene.player.health > 0.0f &&
        !g_drivingHumvee && !cameraLocked && !g_mouseWalkTestMode &&
        !(showUI && ImGui::GetIO().WantCaptureMouse) &&
        !GunModel::C4Selected() && rightMouseHeld;
    const bool scopeRequested = aimRequested && GunModel::R700Selected();
    // The last argument lets the debug ADS hold raise the scope, but only on a
    // rifle that actually has one.
    scene.UpdateSniperScope(scopeRequested, deltaTime,
                            GunModel::R700Selected());
    scene.UpdateAimDownSights(aimRequested, deltaTime);
    // Virtual controls are ImGui widgets, so WantCaptureKeyboard/cameraLocked
    // must not suppress the input those widgets produced on the previous frame.
    if (!g_drivingHumvee) ApplyVirtualInput();

    if (cameraLocked || (showUI && ImGui::GetIO().WantCaptureKeyboard)) return;

    // Ejected (F8): the camera flies free while the player body -- and with it
    // the weapon and arms -- stays parked where it was. Shift accelerates,
    // Space/Q lift and drop, matching the level editor's fly camera. Returning
    // early keeps walking, gravity, sliding and shooting out of the way.
    if (scene.ejected) {
        const float speed = ((FocusedKeyState(VK_SHIFT) & 0x8000) ? 3.0f : 1.0f);
        if (FocusedKeyState('W') & 0x8000) scene.camera.ProcessKeyboard('W', deltaTime, speed);
        if (FocusedKeyState('S') & 0x8000) scene.camera.ProcessKeyboard('S', deltaTime, speed);
        if (FocusedKeyState('A') & 0x8000) scene.camera.ProcessKeyboard('A', deltaTime, speed);
        if (FocusedKeyState('D') & 0x8000) scene.camera.ProcessKeyboard('D', deltaTime, speed);
        const float lift = scene.camera.MovementSpeed * speed * deltaTime;
        if (FocusedKeyState(VK_SPACE) & 0x8000) scene.camera.Position.y += lift;
        if (FocusedKeyState('Q') & 0x8000)      scene.camera.Position.y -= lift;
        return;
    }

    if (g_drivingHumvee) {
        if (g_activeHumveeIndex >= g_humveeGameplay.size()) {
            g_drivingHumvee = false;
            g_activeHumveeIndex = kNoHumvee;
            return;
        }
        HumveeGameplayState& state = g_humveeGameplay[g_activeHumveeIndex];
        state.turretFireCooldown = (std::max)(
            0.0f, state.turretFireCooldown - deltaTime);
        const float throttle =
            ((FocusedKeyState('W') & 0x8000) ? 1.0f : 0.0f) -
            ((FocusedKeyState('S') & 0x8000) ? 1.0f : 0.0f);
        const float manualSteering =
            ((FocusedKeyState('A') & 0x8000) ? 1.0f : 0.0f) -
            ((FocusedKeyState('D') & 0x8000) ? 1.0f : 0.0f);
        float steering = manualSteering * 0.45f;
        XMFLOAT4X4 vehiclePose;
        XMFLOAT3 vehicleForward;
        if (g_destruction.GetVehicleTransform(
                g_activeHumveeIndex, vehiclePose, nullptr, &vehicleForward)) {
            XMVECTOR desiredVector = XMVectorSet(
                scene.camera.Front.x, 0.0f, scene.camera.Front.z, 0.0f);
            XMVECTOR vehicleVector = XMVectorSet(
                vehicleForward.x, 0.0f, vehicleForward.z, 0.0f);
            if (XMVectorGetX(XMVector3LengthSq(desiredVector)) > 0.001f &&
                XMVectorGetX(XMVector3LengthSq(vehicleVector)) > 0.001f) {
                desiredVector = XMVector3Normalize(desiredVector);
                vehicleVector = XMVector3Normalize(vehicleVector);
                const float dot = (std::max)(-1.0f, (std::min)(1.0f,
                    XMVectorGetX(XMVector3Dot(vehicleVector, desiredVector))));
                const float cross = vehicleForward.z * XMVectorGetX(desiredVector) -
                                    vehicleForward.x * XMVectorGetZ(desiredVector);
                const float headingError = std::atan2(cross, dot);
                steering += headingError * 1.45f;
            }
        }
        steering = (std::max)(-1.0f, (std::min)(1.0f, steering));
        g_destruction.SetVehicleInput(
            g_activeHumveeIndex, throttle, steering,
            (FocusedKeyState(VK_SPACE) & 0x8000) != 0);
        for (size_t index = 0; index < g_destruction.VehicleCount(); ++index)
            if (index != g_activeHumveeIndex)
                g_destruction.SetVehicleInput(index, 0.0f, 0.0f, true);
        if ((FocusedKeyState(VK_LBUTTON) & 0x8000) &&
            !ImGui::GetIO().WantCaptureMouse)
            FireHumveeTurret();
        return;
    }
    for (size_t index = 0; index < g_destruction.VehicleCount(); ++index)
        g_destruction.SetVehicleInput(index, 0.0f, 0.0f, true);
    // Riding the insertion helicopter in: the seat owns the camera position, so
    // walking, crouching, sliding and jumping stay disabled until the drop-off.
    // Looking and the weapon block below keep running, so the player can fire
    // out of the door on the way in.
    const bool ridingBlackHawk = g_game.vehicles.blackHawkCarryingPlayer;
    const bool controlDown = !ridingBlackHawk && scene.camera.FPSMode &&
        (FocusedKeyState(VK_CONTROL) & 0x8000);
    // An exhausted player keeps the key but loses the speed: the meter has to
    // rebuild past kStaminaRecoveredToSprint before shift means anything again.
    // Gating here rather than at the movement call keeps one authority for
    // "is this a sprint", which the viewmodel and the stamina drain both read.
    const bool sprinting = !ridingBlackHawk && scene.camera.FPSMode &&
        !g_staminaExhausted &&
        (FocusedKeyState(VK_SHIFT) & 0x8000);
    // Mouse-walk mode: the right button reads as forward, exactly as if W were
    // held. Gated on the same UI capture the fire path uses, so dragging a debug
    // window does not also march the player across the map.
    //
    // C4 detonation deliberately keeps this button as well: with the charge
    // equipped, a press both walks and detonates. That is intended -- the
    // detonator is worth more than a clean movement binding in one mode.
    const bool mouseWalkForward = g_mouseWalkTestMode &&
        (FocusedKeyState(VK_RBUTTON) & 0x8000) != 0 &&
        !ImGui::GetIO().WantCaptureMouse;
    const float forwardInput =
        (((FocusedKeyState('W') & 0x8000) || mouseWalkForward) ? 1.0f : 0.0f) -
        ((FocusedKeyState('S') & 0x8000) ? 1.0f : 0.0f);
    const float strafeInput =
        ((FocusedKeyState('A') & 0x8000) ? 1.0f : 0.0f) -
        ((FocusedKeyState('D') & 0x8000) ? 1.0f : 0.0f);
    static bool controlWasDown = false;
    if (controlDown && !controlWasDown && sprinting)
        scene.camera.StartSlide(forwardInput, strafeInput);
    controlWasDown = controlDown;

    const bool crouching = controlDown || scene.camera.IsSliding;
    scene.camera.SetCrouching(crouching, deltaTime, scene.player.downed);
    // Everything the player is asking for this frame, captured as data before
    // it is applied. Only the local player fills this from the keyboard; a
    // networked player will receive the same struct instead (see PlayerInput).
    PlayerInput playerInput;
    playerInput.forward = forwardInput;
    playerInput.strafe = strafeInput;
    playerInput.yaw = scene.camera.Yaw;
    playerInput.pitch = scene.camera.Pitch;
    playerInput.deltaTime = deltaTime;
    playerInput.Set(PlayerInput::Crouch, crouching);
    playerInput.Set(PlayerInput::Sprint, sprinting);
    // Sprint is 1.5x walk (7.5 m/s against a 5.0 m/s Camera::MovementSpeed).
    // Was 2.0x, which outran the traversal the island is built for.
    constexpr float kSprintMovementScale = 1.5f;
    const float movementMultiplier = crouching ? 0.55f :
        (sprinting ? kSprintMovementScale : 1.0f);
    // Only a sprint that is actually moving the player faster counts for the
    // viewmodel. Crouching wins over shift here (0.55x), so shift held while
    // crouched must NOT speed the legs up -- the multiplier above is the
    // authority on whether this is a sprint, not the key state alone.
    g_playerSprinting = sprinting && !crouching;
    playerInput.movementMultiplier = movementMultiplier;
    // Space rises, crouch dives. While swimming the crouch gate is lifted:
    // crouching underwater means "swim down", so it must not also block the
    // ascend key the way it blocks jumping on land.
    const bool jumpHeld = !ridingBlackHawk &&
        (scene.camera.IsSwimming || !crouching) &&
        (FocusedKeyState(VK_SPACE) & 0x8000) != 0;
    playerInput.Set(PlayerInput::Jump,
                    jumpHeld && !scene.camera.IsSwimming);
    playerInput.Set(PlayerInput::Swim, jumpHeld && scene.camera.IsSwimming);
    playerInput.Set(PlayerInput::SwimDown,
                    !ridingBlackHawk && scene.camera.IsSwimming && controlDown);
    // Sliding and riding suppress movement but not the look/jump state above,
    // matching the previous behaviour exactly.
    // A downed player joins that list: they keep the look above, because
    // watching for a teammate is the only thing left to do, but they do not
    // get to crawl.
    if (scene.camera.IsSliding || ridingBlackHawk || scene.player.downed) {
        playerInput.forward = 0.0f;
        playerInput.strafe = 0.0f;
    }
    // Revive is a hold, so it is sampled here with the other held keys rather
    // than from the edge-triggered WM_KEYDOWN chain below -- which is also
    // gated on !g_emptyLevelMode, and the empty test level is exactly where
    // multiplayer gets tested. Only set when there is actually someone to pick
    // up, so holding E beside a Humvee does not transmit a revive intent.
    {
        net::PlayerId reviveTarget = net::kInvalidPlayerId;
        const bool nearDowned = NearbyDownedPlayer(&reviveTarget) != nullptr;
        const bool reviveHeld =
            nearDowned && (FocusedKeyState('E') & 0x8000) != 0;
        playerInput.Set(PlayerInput::Revive, reviveHeld);
        if (MultiplayerActive()) {
            g_netSession.ReportReviveIntent(
                reviveHeld ? reviveTarget : net::kInvalidPlayerId, reviveHeld);
        }
    }

    scene.camera.ApplyInput(playerInput);
    // Published for the multiplayer layer, which sends it after the local
    // player has finished moving this frame.
    g_localPlayerInput = playerInput;

    // Auto-fire: while the mouse is held (and not interacting with the UI),
    // keep shooting on a fixed interval instead of one shot per click.
    scene.fireCooldown -= deltaTime;
    const bool mouseHeld = (FocusedKeyState(VK_LBUTTON) & 0x8000) != 0;
    if (!mouseHeld) g_suppressFireUntilMouseRelease = false;
    if (scene.autoFire && mouseHeld && !g_suppressFireUntilMouseRelease &&
        !ImGui::GetIO().WantCaptureMouse && !scene.player.downed &&
        scene.fireCooldown <= 0.0f && ShootPlayerWeapon()) {
        scene.fireCooldown = PlayerFireInterval();
    }

    // Reload: press R. Held keys are fine here because BeginReload rejects a
    // second start while one is already running.
    scene.UpdateReload(deltaTime);
    if ((FocusedKeyState('R') & 0x8000) &&
        scene.BeginReload(GunModel::SelectedWeapon()))
        PlayReloadSound();

    // Grenade: press G to lob one. Cooldown debounces the held key.
    scene.grenadeCooldown -= deltaTime;
    if ((FocusedKeyState('G') & 0x8000) && scene.grenadeCooldown <= 0.0f &&
        !scene.player.downed) {
        const size_t projectileStart = scene.projectiles.size();
        scene.ThrowGrenade();
        for (size_t index = projectileStart; index < scene.projectiles.size(); ++index)
            scene.projectiles[index].playerOwned = true;
        if (g_game.session.TimerRunning())
            g_game.mission.RecordGrenadeThrown();
        scene.grenadeCooldown = 0.6f;
    }
}

// ?? window proc ??????????????????????????????????????????????????????????????
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_SETFOCUS) {
        g_inputWindow = hwnd;
        firstMouse = true;
        ignoreNextMouseMove = false;
        if (!cameraLocked) {
            SetCapture(hwnd);
            SetCursorVisible(false);
        }
    } else if (msg == WM_KILLFOCUS) {
        g_inputWindow = nullptr;
        if (GetCapture() == hwnd) ReleaseCapture();
        SetCursorVisible(true);
        firstMouse = true;
        ignoreNextMouseMove = false;
        if (IsEditorEditing()) cameraLocked = true;
        virtualInput.moveX = virtualInput.moveY = 0.0f;
        virtualInput.lookX = virtualInput.lookY = 0.0f;
        virtualInput.down = virtualInput.jump = virtualInput.shoot = false;
    }

    // Focus messages still reach ImGui so it can clear its own held state.
    if (!HasInputFocus() &&
        ((msg >= WM_KEYFIRST && msg <= WM_KEYLAST) ||
         (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST)))
        return DefWindowProc(hwnd, msg, wParam, lParam);

    // RMB over the editor viewport means "return control to the scene". A
    // focused InputText/slider leaves ActiveId set and therefore keeps
    // WantCaptureKeyboard true even after mouse-look starts. Removing ImGui
    // window focus clears that active widget; clicks over panels stay owned by
    // ImGui so their context menus and editing behaviour are unchanged.
    if (msg == WM_RBUTTONDOWN && IsEditorEditing() &&
        ImGui::GetCurrentContext() && !ImGui::GetIO().WantCaptureMouse)
        ImGui::SetWindowFocus(nullptr);

    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) return true;

    // ImGui gets first chance to consume menu clicks. Any unconsumed gameplay
    // mouse input stops here so it cannot rotate camera, fire, or change guns.
    const bool blocksGameplayMouse =
        g_game.session.Screen() == GameScreen::MainMenu ||
        g_game.session.Screen() == GameScreen::WinScreen ||
        // A downed player keeps their mouse: they can still look around while
        // waiting to be picked up, which is most of what there is to do down
        // there. Only actual death takes the mouse away.
        (g_game.session.Screen() == GameScreen::Level1 &&
         !scene.player.godMode && scene.player.health <= 0.0f &&
         !scene.player.downed);
    if (blocksGameplayMouse &&
        (msg == WM_MOUSEMOVE || msg == WM_MOUSEWHEEL ||
         msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP ||
         msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP))
        return 0;

    if (IsEditorEditing() &&
        (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP))
        return 0;
    // Editor mouse wheel: over the 3D viewport it scales fly-camera speed
    // (Unreal style); over an ImGui panel let ImGui handle it (e.g. sliders).
    if (IsEditorEditing() && msg == WM_MOUSEWHEEL) {
        if (!ImGui::GetIO().WantCaptureMouse) {
            const int wheel = GET_WHEEL_DELTA_WPARAM(wParam);
            const float step = wheel > 0 ? 1.15f : (1.0f / 1.15f);
            g_editorCameraSpeed = (std::max)(0.1f,
                (std::min)(g_editorCameraSpeed * step, 10.0f));
        }
        return 0;
    }

    switch (msg) {
    case WM_SIZE:
        if (g_dx12.device && g_dx12.initialized && wParam != SIZE_MINIMIZED) {
            unsigned w = LOWORD(lParam), h = HIWORD(lParam);
            if (w > 0 && h > 0 && (w != SCR_WIDTH || h != SCR_HEIGHT)) {
                WaitForGPU();
                SCR_WIDTH = w; SCR_HEIGHT = h;
                ResizeDX12(SCR_WIDTH, SCR_HEIGHT);
                if (occlusionDepth.initialized) occlusionDepth.Resize(SCR_WIDTH, SCR_HEIGHT);
                if (fxaa.initialized) fxaa.Resize(SCR_WIDTH, SCR_HEIGHT);
                if (nightVision.initialized)
                    nightVision.Resize(SCR_WIDTH, SCR_HEIGHT);
                if (msaa.initialized) msaa.Resize(SCR_WIDTH, SCR_HEIGHT);
                if (visBuffer.initialized) visBuffer.Resize(SCR_WIDTH, SCR_HEIGHT);
                if (grassMSAA.initialized) grassMSAA.Resize(SCR_WIDTH, SCR_HEIGHT);
                if (waterRenderer.initialized)
                    waterRenderer.Resize(SCR_WIDTH, SCR_HEIGHT);
                if (g_rt.initialized) ResizeRaytracing(SCR_WIDTH, SCR_HEIGHT);
            }
        }
        return 0;

    case WM_MOUSEWHEEL:
        // Editor camera-speed scrolling is handled earlier; here it zooms the
        // deployment overview, and otherwise cycles the weapon during gameplay.
        if (!ImGui::GetIO().WantCaptureMouse) {
            const int wheel = GET_WHEEL_DELTA_WPARAM(wParam);
            if (DeploymentPlanningActive()) {
                // Multiplicative so each notch covers the same proportion of
                // the range whether pulled in or out; scrolling up moves the
                // camera closer, matching every map view.
                const float notches =
                    static_cast<float>(wheel) / static_cast<float>(WHEEL_DELTA);
                g_deploymentZoom = std::clamp(
                    g_deploymentZoom * std::pow(1.0f / 1.12f, notches),
                    kDeploymentZoomMin, kDeploymentZoomMax);
            } else {
                GunModel::CycleWeapon(wheel);
                scene.fireCooldown = 0.0f;
            }
        }
        return 0;

    case WM_MOUSEMOVE:
        if (!cameraLocked && !(showUI && ImGui::GetIO().WantCaptureMouse)) {
            if (ignoreNextMouseMove) {
                // This move was generated by our own SetCursorPos recenter below,
                // not real user input - skip it so it can't be misread as a delta.
                ignoreNextMouseMove = false;
                return 0;
            }

            float xpos = (float)GET_X_LPARAM(lParam);
            float ypos = (float)GET_Y_LPARAM(lParam);

            RECT r; GetClientRect(hwnd, &r);
            float centerX = (float)(r.right - r.left) / 2.0f;
            float centerY = (float)(r.bottom - r.top) / 2.0f;

            if (firstMouse) { lastX = centerX; lastY = centerY; firstMouse = false; }
            else {
                float dx = xpos - lastX;
                float dy = lastY - ypos; // screen Y grows downward; flip so moving the mouse up looks up
                if (dx != 0.0f || dy != 0.0f) {
                    const float scopeScale = scene.ScopeLookScale();
                    scene.camera.ProcessMouseMovement(dx * scopeScale, dy * scopeScale);
                }
            }

            // Re-center the cursor every move so it never reaches the screen
            // edge and clamps, which would otherwise cap how far you can turn.
            POINT c = { (LONG)centerX, (LONG)centerY };
            ClientToScreen(hwnd, &c);
            ignoreNextMouseMove = true;
            SetCursorPos(c.x, c.y);
            lastX = centerX; lastY = centerY;
        }
        return 0;

    case WM_RBUTTONDOWN:
        // Deployment right-drag is read from ImGui in
        // RenderInsertionChoiceScreen; Win32 capture remains editor-only.
        if (IsEditorEditing() && !ImGui::GetIO().WantCaptureMouse) {
            cameraLocked = false;
            SetCapture(hwnd);
            SetCursorVisible(false);
            firstMouse = true;
        }
        return 0;

    case WM_RBUTTONUP:
        if (IsEditorEditing()) {
            cameraLocked = true;
            ReleaseCapture();
            SetCursorVisible(true);
        }
        return 0;

    case WM_LBUTTONDOWN:
        if (!ImGui::GetIO().WantCaptureMouse) {
            if (g_drivingHumvee) {
                FireHumveeTurret();
                return 0;
            } else if (HeldBarrel()) {
                ThrowHeldBarrel();
                g_suppressFireUntilMouseRelease = true;
            } else if (g_heldBandit && g_heldBandit->Held()) {
                GrabOrThrowBandit();
                g_suppressFireUntilMouseRelease = true;
            } else if (cameraLocked && !g_insertionChoicePending) {
                // Click-to-capture, but never on the deployment screen: there the
                // click is picking a zone marker off the map, and grabbing the
                // pointer would snap it to the centre mid-selection and hide it.
                // The screen releases the cursor deliberately and re-captures it
                // itself on DEPLOY.
                cameraLocked = false;
                SetCapture(hwnd); ShowCursor(FALSE);
                RECT r; GetClientRect(hwnd, &r);
                POINT c = { (r.right-r.left)/2, (r.bottom-r.top)/2 };
                ClientToScreen(hwnd, &c);
                ignoreNextMouseMove = true;
                SetCursorPos(c.x, c.y);
                lastX = (float)(r.right-r.left)/2;
                lastY = (float)(r.bottom-r.top)/2;
                firstMouse = true;
            } else if (!scene.autoFire && !g_insertionChoicePending) {
                // Auto-fire handles shooting in ProcessInput while held; only
                // fire on click when auto-fire is off.
                ShootPlayerWeapon();
            }
        }
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            if (IsEditorPlaying())
                g_game.commands.Request(GameCommand::EditorStopPlay);
            else if (IsEditorEditing()) {
                if (!g_levelEditor.IsDirty()) OpenMainMenu();
            }
            else if (g_game.session.Screen() != GameScreen::MainMenu)
                OpenMainMenu();
            else PostQuitMessage(0);
        }
        else if (IsEditorEditing()) {
            const bool controlDown = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            if (!(FocusedKeyState(VK_RBUTTON) & 0x8000))
                g_levelEditor.OnKeyDown(static_cast<unsigned>(wParam), controlDown);
        }
        else if (wParam == VK_TAB) {
            showUI = !showUI;
            if (showUI) {
                cameraLocked = true;
                ReleaseCapture(); ShowCursor(TRUE);
            } else {
                // Hiding the UI: capture and re-center the mouse so the next
                // WM_MOUSEMOVE delta is computed from the window center instead
                // of wherever the cursor happened to be over the UI, which
                // otherwise causes the camera to snap-rotate on the first move.
                cameraLocked = false;
                SetCapture(hwnd); ShowCursor(FALSE);
                RECT r; GetClientRect(hwnd, &r);
                POINT c = { (r.right - r.left) / 2, (r.bottom - r.top) / 2 };
                ClientToScreen(hwnd, &c);
                ignoreNextMouseMove = true;
                SetCursorPos(c.x, c.y);
                lastX = (float)(r.right - r.left) / 2;
                lastY = (float)(r.bottom - r.top) / 2;
                firstMouse = true;
            }
        }
        else if (wParam == 'C')    { cameraLocked = true; ReleaseCapture(); ShowCursor(TRUE); }
        else if (!g_emptyLevelMode && wParam == 'F' &&
                 !(lParam & 0x40000000)) { GrabOrThrowObject(); }
        else if (wParam == 'V' && !(lParam & 0x40000000)) {
            scene.camera.FPSMode = !scene.camera.FPSMode;
        }
        else if (wParam == 'N' && !(lParam & 0x40000000)) {
            g_showEnemyVisionCones = !g_showEnemyVisionCones;
        }
        // J works the gear slot's item, but only when it was actually brought
        // on the mission: the slot is the whole point of the choice, so neither
        // goggles nor a weapon light can be conjured mid-run without it. One
        // key serves both because the slot only ever holds one of them.
        else if (wParam == 'J' && !(lParam & 0x40000000)) {
            const GearType equippedGear = g_game.mission.Loadout().gear;
            if (equippedGear == GearType::NightVisionGoggles)
                g_nightVisionActive = !g_nightVisionActive;
            else if (equippedGear == GearType::Flashlight)
                g_flashlightActive = !g_flashlightActive;
        }
        // F5: flip every RT effect at once. Shares ToggleAllRTEffects with the
        // "All RT Effects" checkbox, so the two agree on the saved
        // configuration and either can restore what the other stored.
        //
        // A function key rather than a letter: this fires during gameplay, so
        // it must not collide with movement or weapon handling, and it is safe
        // to press mid-combat.
        //
        // The repeat filter matters here: without it, holding the key would
        // toggle every frame and the saved configuration would be overwritten
        // with the cleared one.
        else if (wParam == VK_F5 && !(lParam & 0x40000000)) {
            if (g_inlineRaytracingSupported && visBuffer.EnhancedVisualsReady())
                ToggleAllRTEffects(scene, visBuffer, !scene.enhancedVisuals);
        }
        else if (scene.player.godMode && wParam == 'B' &&
                 !(lParam & 0x40000000)) {
            switch (scene.selectedGrenade) {
            case GrenadeType::Frag:
                scene.selectedGrenade = GrenadeType::Molotov;
                break;
            case GrenadeType::Molotov:
                scene.selectedGrenade = GrenadeType::Vortex;
                break;
            default:
                scene.selectedGrenade = GrenadeType::Frag;
                break;
            }
        }
        else if (wParam == 'M' && !(lParam & 0x40000000)) {
            if (visBuffer.initialized) {
                scene.useVisibilityBuffer = !scene.useVisibilityBuffer;
                if (scene.useVisibilityBuffer) {
                    scene.useRaytracing = false;
                    g_rt.enabled = false;
                }
            }
        }
        else if (!g_emptyLevelMode && wParam == 'E' &&
                 !(lParam & 0x40000000)) {
            // Riding an insertion vehicle takes priority: E jumps out. Only one
            // can be carrying the player, so the chain short-circuits on it.
            // A weapon pickup underfoot comes next -- a rocket is often left
            // beside the Humvee, and reaching for it should not put the player
            // in the driver's seat. Otherwise E keeps its usual job of getting
            // in/out of the Humvee.
            //
            // An open armory counter takes the key back to close itself, so E
            // is symmetric: the same key that opened the shop leaves it.
            //
            // The travel board behaves the same way: E opens it and E leaves
            // it, and it sits after the armory in the chain so a counter placed
            // beside a helicopter still wins the key at its own range.
            //
            // A downed teammate underfoot takes the key before any of that.
            // The revive itself is a hold sampled in ProcessInput, so nothing
            // happens here -- this only stops the same press from ALSO putting
            // the player in a Humvee parked beside the body.
            if (NearbyDownedPlayer()) { /* handled as a held key above */ }
            else if (g_armoryShopOpen) CloseArmoryShop(hwnd);
            else if (g_travelScreenOpen) CloseTravelScreen(hwnd);
            else if (!g_game.vehicles.BailOutOfBlackHawk() &&
                !g_game.vehicles.BailOutOfInsertionBoat() &&
                !CollectNearbyWeaponPickup() &&
                !OpenNearbyArmoryShop() &&
                !OpenNearbyTravelScreen())
                ToggleHumveeDriving();
        }
        // Bit 30 = key was already down (autorepeat); toggle once per press.
        else if (wParam == 'Z' && !(lParam & 0x40000000)) {
            scene.meshletWireframe = !scene.meshletWireframe;
        }
        else if (wParam == VK_F8 && !(lParam & 0x40000000)) {
            // Unreal-style eject: detach the camera from the player so the view
            // model can be flown around and inspected from outside.
            scene.ToggleEjectedCamera();
        }
        else if (wParam == VK_F10 && !(lParam & 0x40000000)) {
            g_mouseWalkTestMode = !g_mouseWalkTestMode;
            // Drop any sights already raised on the way in, so the mode does not
            // start stuck at the ADS FOV with no button left to lower it.
            if (g_mouseWalkTestMode) {
                scene.UpdateAimDownSights(false, 0.0f);
                scene.UpdateSniperScope(false, 0.0f);
            }
            SGE_LOG("LogGameplay", EngineLog::Level::Display,
                std::string("Mouse-walk test mode ") +
                (g_mouseWalkTestMode ? "ON (RMB walks forward, ADS off)"
                                     : "OFF"));
        }
        else if (wParam == VK_F11) { ToggleFullscreen(hwnd); }
        return 0;

    case WM_DESTROY:
        std::ofstream("engine_runtime_error.log", std::ios::app)
            << "WM_DESTROY\n";
        // Last chance to bank the wallet: closing the window mid-run does not
        // pass through extraction or the main menu, which are the other two
        // save points.
        SaveCareer();
        PostQuitMessage(0); return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}
