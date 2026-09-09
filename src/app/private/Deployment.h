#pragma once

// Private application implementation; included once by main.cpp in dependency order.

// These masks are what the player has hired FOR THIS MISSION, not what they own
// -- see ClearMissionRentals. "Owned" is kept as the name only because the
// storefront reads it on every row and the meaning at the point of use is the
// same question: has this already been paid for, or does selecting it charge?
//
// An item priced at zero is standard issue, not a rental: it is available from
// the first launch and never appears as something to buy.
static bool ArmoryWeaponOwned(int weapon) {
    if (weapon < 0 || weapon >= MissionLoadout::kWeaponCount) return true;
    if (ArmoryCatalog::WeaponPrice(weapon) == 0) return true;
    return (g_ownedWeapons & (1u << static_cast<uint32_t>(weapon))) != 0u;
}

static bool ArmoryGrenadeOwned(GrenadeType grenade) {
    const int index = static_cast<int>(grenade);
    if (index < 0 || index >= ArmoryCatalog::kGrenadeCount) return true;
    if (ArmoryCatalog::kGrenadePrices[index] == 0) return true;
    return (g_ownedGrenades & (1u << static_cast<uint32_t>(index))) != 0u;
}

static bool ArmoryGearOwned(GearType gear) {
    const int index = static_cast<int>(gear);
    if (index < 0 || index >= ArmoryCatalog::kGearCount) return true;
    if (ArmoryCatalog::kGearPrices[index] == 0) return true;
    return (g_ownedGear & (1u << static_cast<uint32_t>(index))) != 0u;
}

static bool ArmoryAttachmentOwned(const std::string& id) {
    return g_ownedAttachments.find(id) != g_ownedAttachments.end();
}

// Drops every rental, so the next trip to the armory charges for the kit again.
// Called as the deploy screen opens rather than as a run ends: a mission that is
// restarted from its own planning screen must not bill twice for the same
// deployment, and opening the screen is the one point that is reached exactly
// once per attempt at buying a loadout.
//
// Standard-issue items are unaffected -- they are priced at zero, so the Owned
// helpers above return true for them without consulting these masks at all.
static void ClearMissionRentals() {
    g_ownedWeapons = 0;
    g_ownedGrenades = 0;
    g_ownedGear = 0;
    g_ownedAttachments.clear();
    // Clearing the paid-for flags is only half of it: the loadout keeps whatever
    // was equipped last mission, so without this the player would still be
    // holding a weapon they no longer have a rental for and would deploy with it
    // free. Reset to standard issue, which is the kit that costs nothing.
    MissionLoadout& loadout = g_game.mission.Loadout();
    loadout.weapons = { MissionLoadout::kDefaultPrimaryWeapon, 1 };
    loadout.grenade = GrenadeType::Frag;
    loadout.gear = GearType::None;
    // Attachments hang off the weapon instances rather than the loadout, so
    // they survive the reset above and have to be stripped from every rail.
    for (int weapon = 0; weapon < MissionLoadout::kWeaponCount; ++weapon)
        for (uint8_t slot = 0;
             slot < static_cast<uint8_t>(SGE::AttachmentSlot::Count); ++slot)
            scene.player.weapons.RemoveAttachment(
                weapon, static_cast<SGE::AttachmentSlot>(slot));

    // Kit hired at the base was bought FOR this deployment, so it survives the
    // reset above and comes back paid for. Restored after the wipe rather than
    // guarded around it so a slot the player never outfitted at the counter
    // still falls back to standard issue.
    if (!g_baseKitPending) return;
    g_baseKitPending = false;
    loadout = g_baseKitLoadout;
    g_ownedWeapons = g_baseKitWeapons;
    g_ownedGrenades = g_baseKitGrenades;
    g_ownedGear = g_baseKitGear;
    g_ownedAttachments = g_baseKitAttachments;
    // Re-fit the rails as they were left at the counter. The loop above
    // stripped every weapon, so the parts have to be put back on. Driven by
    // what was actually FITTED rather than by the owned set: a part bought and
    // then taken off again is still owned, and re-fitting from ownership would
    // put it back on a rail the player deliberately cleared.
    for (const auto& [weapon, id] : g_baseKitFitted)
        scene.player.weapons.EquipAttachment(weapon, id);
}

// Saves the wallet. Rentals are deliberately NOT persisted: kit is hired for one
// mission, so what was carried last time says nothing about what the player has
// now, and writing it would make it survive the restart that is supposed to
// clear it. The balance is the only career state.
static void SaveCareer() {
    SaveMoney(g_game.money);
}

// Single purchase point, so every buy button in the storefront goes through the
// same balance check. Returns false and changes nothing when the player cannot
// afford it, which is what keeps the buttons honest -- they are disabled on the
// same condition, and this is the backstop if that ever drifts.
static bool ArmoryPurchase(int price) {
    if (price <= 0) return true;
    MoneySystem& wallet = g_game.money;
    if (wallet.Balance() < price) return false;
    // Spending is not a negative award: Award() would queue a HUD popup and
    // subtract from session earnings, making a purchase look like a penalty on
    // the extraction report. SetBalance moves the money without touching the
    // earned totals, which is exactly what buying something is.
    wallet.SetBalance(wallet.Balance() - price, wallet.TotalEarned());
    return true;
}
// Missile strike armed from the deployment map. While armed, a click on the
// map picks the impact point instead of a landing zone, so the two picking
// modes never compete for the same click.
static bool g_missileStrikeArmed = false;
// Mouse-wheel zoom on the deployment overview, as a multiplier on the orbit
// rig BuildCameraFrame composes. Applied to radius and height together so the
// framing scales instead of skewing: pulling in low over the island would
// otherwise flatten the view into a horizon shot.
//
// 1.0 is the authored composition. The range keeps the whole island on screen
// at the far end and stops the near end from pushing the camera through the
// terrain.
static float g_deploymentZoom = 1.0f;
static constexpr float kDeploymentZoomMin = 0.45f;
static constexpr float kDeploymentZoomMax = 2.2f;
// Manual orbit: right-drag turns and tilts the map, and the automatic rotation
// resumes after a pause once the button is released.
//
// The auto orbit reads its angle from g_deploymentFlythroughTime, so a manual
// turn is stored as an offset onto that rather than by writing the time back.
// Rewriting the time would make the resume jump, since the drag and the clock
// advance at unrelated rates.
// Driven from ImGui, not from Win32 capture. The deployment screen releases the
// pointer so its panels and zone markers stay clickable, and the ImGui backend
// takes capture for itself on any button press -- between them the capture
// changed hands the instant a drag began, and a WM_CAPTURECHANGED handler that
// treated that as "drag cancelled" tore the drag down before a single
// WM_MOUSEMOVE could act on it.
//
// ImGui already owns the pointer on this screen and tracks the drag itself, so
// reading it there sidesteps the capture fight entirely and keeps one owner.
static bool g_deploymentOrbitDragging = false;
static float g_deploymentOrbitOffset = 0.0f;
static float g_deploymentOrbitElevationOffset = 0.0f;
static float g_deploymentOrbitResumeDelay = 0.0f;
// Drag deltas banked by the UI pass for the camera update to consume. The UI
// runs after ImGui::NewFrame (where MouseDelta becomes valid) and the camera
// update runs before it, so the values cross the frame boundary here.
static float g_deploymentOrbitPendingDrag = 0.0f;
static float g_deploymentOrbitPendingTilt = 0.0f;
// Flick momentum. Releasing mid-drag hands the map the speed it was turning at
// and lets friction bleed it off, so the globe carries on and coasts to a stop
// rather than freezing the instant the button comes up.
static float g_deploymentOrbitVelocity = 0.0f;
// Seconds of stillness after the spin has died before the map turns itself
// again.
static constexpr float kDeploymentOrbitResumeSeconds = 3.0f;
// Radians of orbit per pixel dragged. A full turn is a little over a
// screen-width of travel at 1080p.
static constexpr float kDeploymentOrbitRadiansPerPixel = 0.0032f;
static constexpr float kDeploymentOrbitElevationRadiansPerPixel = 0.0024f;
static constexpr float kDeploymentOrbitMinElevation = 1.0f * XM_PI / 180.0f;
static constexpr float kDeploymentOrbitMaxElevation = 78.0f * XM_PI / 180.0f;
// Friction on the flick, as the fraction of speed kept per second. 0.12 leaves
// a hard flick coasting for roughly two seconds before it falls under the stop
// threshold -- long enough to read as weight, short enough not to fight a
// player trying to line up a zone.
static constexpr float kDeploymentOrbitDamping = 0.12f;
// Below this the spin is imperceptible; stopping there is what lets the resume
// countdown start instead of trailing an infinitely small drift.
static constexpr float kDeploymentOrbitMinSpin = 0.04f;
// Ceiling on a flick, so a fast swipe across the screen cannot launch the map
// into a blur that takes seconds to settle.
static constexpr float kDeploymentOrbitMaxSpin = 6.0f;
// Deployment-only diagnostics. They never mutate the authored weather or
// renderer settings, so leaving the planning screen restores gameplay simply
// by making DeploymentPlanningActive() false.
static bool g_deploymentDebugHideWater = false;
static bool g_deploymentDebugHideAtmosphere = false;
static bool g_deploymentDebugForceForward = false;
static bool g_deploymentDebugWaterDepth = false;
static bool g_deploymentDebugCoverageGuides = false;
// Skips the GTAO + contact shadow pass while planning. AO darkens creases and
// the lee of every prop, which is exactly where a drop-off marker sits on a
// map read from above -- turning it off makes the terrain shape itself legible.
static bool g_deploymentDebugHideAO = false;

static void CancelDeploymentPlanning() {
    g_insertionChoicePending = false;
    g_insertionChoiceCursorReleased = false;
    g_deploymentZones.clear();
    g_selectedDeploymentZone = -1;
    g_deploymentTarget = {};
    g_deploymentTargetValid = false;
    g_deploymentFlythroughTime = 0.0f;
    // A cancelled plan never puts a transport in the air, so a drop banked from
    // a previous attempt must not survive into the next one.
    g_marineDropPending = false;
    g_deploymentZoom = 1.0f;
    g_deploymentOrbitDragging = false;
    g_deploymentOrbitOffset = 0.0f;
    g_deploymentOrbitElevationOffset = 0.0f;
    g_deploymentOrbitResumeDelay = 0.0f;
    g_deploymentOrbitVelocity = 0.0f;
    g_deploymentOrbitPendingDrag = 0.0f;
    g_deploymentOrbitPendingTilt = 0.0f;
    // A run that never reaches a helicopter seat must not leave the one-shot
    // armed for whatever insertion comes next.
    g_blackHawkRideFacingPending = false;
}

bool DeploymentPlanningActive() { return g_insertionChoicePending; }
int DeploymentWaterDebugMode() {
    return g_insertionChoicePending && g_deploymentDebugWaterDepth ? 1 : 0;
}
const std::vector<XMFLOAT3>& DeploymentZonePositions() {
    return g_deploymentZones;
}
int SelectedDeploymentZoneIndex() { return g_selectedDeploymentZone; }

// Radius of the insertion ring this run deployed from. A custom level authors
// its own; the stock island uses the historical 34 m.
//
// The exfil is placed against this rather than a fixed distance, so the way out
// always sits outside the ring the player came in on however wide it was
// authored (see VehicleSystem::EscapeBoatDistanceForRing).
static float CurrentDeploymentRadius() {
    return g_customLevelMode ? g_game.world.Level().deploymentRadius
                             : kDefaultDeploymentRadius;
}

// Where this run's exfil sits: out past its own insertion ring, never inside
// the shelf the beach slopes down.
static float CurrentEscapeBoatDistance() {
    return VehicleSystem::EscapeBoatDistanceForRing(CurrentDeploymentRadius());
}

// Sends the BlackHawk on an insertion run that ends at the player's spawn. It
// runs in from behind the direction the player is facing, so the approach
// crosses their view before it flares and sets down on top of them.
static void StartBlackHawkInsertionAtPlayerSpawn() {
    const XMFLOAT3 spawn = g_deploymentTargetValid
        ? g_deploymentTarget : scene.camera.Position;
    auto params = CurrentTerrainParams();
    params.heightScale = scene.terrainHeightScale;
    // Never below the water plane, so the skids stay dry on a low spawn.
    const float groundY = (std::max)(0.0f,
        TerrainRendererDX12::HeightAt(params, spawn.x, spawn.z));
    // The craft starts outside the selected perimeter point and flies inward.
    const float facing =
        DeploymentPlanner::HeadingTowardIslandCenter(spawn);
    // Only called when this level inserts by helicopter, so the run always
    // carries the player. See ResolvedInsertionMode.
    const bool fastRappel =
        ResolvedInsertionMode() == LevelInsertionMode::FastRappel;
    // Fast insertion trades preparation time for supplies. StartLevelOne has
    // just restored the full loadout, and this arming function runs once per
    // attempt, so restarts cannot compound the reduction.
    if (fastRappel) scene.player.HalveAmmo();
    g_game.vehicles.BeginBlackHawkInsertion(
        { spawn.x, groundY, spawn.z }, groundY, facing, fastRappel);
}

static void UpdateBlackHawkPalette();

// Turns the blades to the current g_blackHawkRotorSpin angle, by whichever
// route the loaded airframe rigged its rotor: the skinned UH-60 reposes its
// bone through the palette, the NewBlackHawk rotates the node its disc hangs
// from. Exactly one of the two is ever set up (see ApplyInsertionAirframe).
static void SpinBlackHawkRotor() {
    if (g_newBlackHawkRotorNode) {
        // The mast is the rig's local Y, and the disc measures 13.5 x 0.7 x
        // 13.2 about it, so Y is the axis that keeps the blades in their plane.
        XMStoreFloat4(&g_newBlackHawkRotorNode->rotation,
            XMQuaternionRotationAxis(XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f),
                                     std::fmod(g_blackHawkRotorSpin, XM_2PI)));
        // The draw reads globalTransform, not the local rotation just written,
        // so the hierarchy has to be flushed or the disc never moves on screen.
        if (g_blackHawkModel) g_blackHawkModel->RefreshHierarchy();
        return;
    }
    UpdateBlackHawkPalette();
}

// Copies the current palette into this frame's upload buffer and hands back its
// GPU address for the skinning root SRV at t12. One buffer per in-flight frame,
// so a palette the GPU is still reading is never overwritten. Returns 0 when
// the model has no rig, which leaves the draw on the static path.
D3D12_GPU_VIRTUAL_ADDRESS UploadBlackHawkPalette() {
    if (g_blackHawkPalette.empty()) return 0;

    const UINT bytes = (UINT)(g_blackHawkPalette.size() * sizeof(XMFLOAT4X4));
    if (!g_blackHawkPaletteBuffer[0] || g_blackHawkPaletteBytes != bytes) {
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = bytes;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        for (UINT i = 0; i < FRAME_COUNT; ++i) {
            g_blackHawkPaletteMapped[i] = nullptr;
            if (FAILED(g_dx12.device->CreateCommittedResource(
                    &heap, D3D12_HEAP_FLAG_NONE, &desc,
                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                    IID_PPV_ARGS(&g_blackHawkPaletteBuffer[i]))))
                return 0;
            D3D12_RANGE none{ 0, 0 };
            if (FAILED(g_blackHawkPaletteBuffer[i]->Map(
                    0, &none, &g_blackHawkPaletteMapped[i])))
                return 0;
        }
        g_blackHawkPaletteBytes = bytes;
    }

    const UINT frame = g_dx12.frameIndex % FRAME_COUNT;
    if (!g_blackHawkPaletteMapped[frame]) return 0;
    memcpy(g_blackHawkPaletteMapped[frame], g_blackHawkPalette.data(), bytes);
    return g_blackHawkPaletteBuffer[frame]->GetGPUVirtualAddress();
}

// Keeps the player strapped in the cabin for the flight in, then sets them down
// on the drop-off the moment the skids touch. Gravity and ground collision are
// suppressed while riding, otherwise the player falls straight through the floor
// of the aircraft.
static void RidePlayerInBlackHawk(float cabinDeltaTime) {
    VehicleSystem& vehicles = g_game.vehicles;
    // A cut rope means the player is already falling under their own gravity.
    // Pinning the camera here would suspend them in mid-air and undo the cut, so
    // the ride code stands aside and lets the normal fall carry on.
    if (vehicles.blackHawkRopeCut && !vehicles.blackHawkCarryingPlayer &&
        !vehicles.blackHawkDroppedPlayer) {
        vehicles.blackHawkBailedOut = false;
        return;
    }
    if (vehicles.blackHawkCarryingPlayer) {
        // Rappelling owns the player's position outright -- they are on a rope
        // outside the aircraft, not standing on its deck -- so the cabin walk
        // is bypassed entirely while it runs.
        if (BlackHawkRappelActive()) {
            const XMFLOAT3 centre = BlackHawkRappelPlayerWorldPosition();
            scene.camera.Position = { centre.x,
                                      centre.y + scene.camera.PlayerHeight * 0.5f,
                                      centre.z };
            scene.camera.VerticalVelocity = 0.0f;
            scene.camera.IsGrounded = true;
            scene.camera.FloorY =
                scene.camera.Position.y - scene.camera.PlayerHeight;
            g_blackHawkCabinLocalValid = false;
            return;
        }

        // Seed the walk from the authored seat the first time the player
        // boards, so the ride still starts in the door they chose.
        if (!g_blackHawkCabinLocalValid) {
            g_blackHawkCabinLocalValid = true;
            g_blackHawkCabinLocal = {
                vehicles.blackHawkRideSide,
                vehicles.blackHawkRideHeight,
                vehicles.blackHawkRideForward };
        }

        // Walk the cabin. Input is applied in the AIRCRAFT's frame, not the
        // world's: the camera yaw the player is looking with is a world angle,
        // and the aircraft is turning underneath them, so moving along a world
        // direction would drift them across the deck whenever the bird banked.
        // Rotating the look direction out of the airframe's yaw converts
        // "forward from where I am looking" into a cabin-relative direction
        // that stays put. The airframe's +Z is world (sin, cos) and its +X is
        // world (cos, -sin), so undoing that maps a world heading onto the
        // deck. Take the camera's own Front rather than rebuilding it from
        // Yaw: deriving the sines here by hand is what previously mirrored the
        // basis about the aircraft's X axis, reversing W/S in the doorways and
        // A/D along the nose.
        const float heliSin = std::sin(vehicles.blackHawkYaw);
        const float heliCos = std::cos(vehicles.blackHawkYaw);
        const float worldFrontX = scene.camera.Front.x;
        const float worldFrontZ = scene.camera.Front.z;
        const float flatLength = std::sqrt(
            worldFrontX * worldFrontX + worldFrontZ * worldFrontZ);
        float moveX = 0.0f, moveZ = 0.0f;
        // Looking straight up or down flattens to nothing, leaving no heading
        // to walk along. Skip rather than divide by ~0 and fling the player.
        if (flatLength > 1e-4f) {
            const float flatX = worldFrontX / flatLength;
            const float flatZ = worldFrontZ / flatLength;
            const float forwardX = flatX * heliCos - flatZ * heliSin;
            const float forwardZ = flatX * heliSin + flatZ * heliCos;
            // Strafe matches the ground path, which walks D along
            // -cross(front, Up); in this basis that is (forwardZ, -forwardX).
            const float rightX = forwardZ;
            const float rightZ = -forwardX;
            if (GetAsyncKeyState('W') & 0x8000) { moveX += forwardX; moveZ += forwardZ; }
            if (GetAsyncKeyState('S') & 0x8000) { moveX -= forwardX; moveZ -= forwardZ; }
            if (GetAsyncKeyState('D') & 0x8000) { moveX += rightX; moveZ += rightZ; }
            if (GetAsyncKeyState('A') & 0x8000) { moveX -= rightX; moveZ -= rightZ; }
        }
        const float moveLength = std::sqrt(moveX * moveX + moveZ * moveZ);
        if (moveLength > 1e-4f) {
            // Normal walking, the same speed and modifiers as on the ground:
            // Camera::MovementSpeed with the 1.5x sprint and 0.55x crouch the
            // ground path applies. The cabin is small enough that a full sprint
            // crosses it in under a second, but moving at a different rate in
            // here than everywhere else is what reads as wrong.
            const bool cabinCrouching =
                (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            const bool cabinSprinting =
                (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            const float cabinMultiplier = cabinCrouching ? 0.55f
                : (cabinSprinting ? 1.5f : 1.0f);
            const float step = scene.camera.MovementSpeed * cabinMultiplier *
                               cabinDeltaTime / moveLength;
            g_blackHawkCabinLocal.x += moveX * step;
            g_blackHawkCabinLocal.z += moveZ * step;
        }
        // Deck extents, measured from the authored seat rather than from the
        // model origin, so they stay centred on the cabin whichever side the
        // player boarded.
        const float centreX = 0.0f;
        const float centreZ = vehicles.blackHawkRideForward;

        // Jump, in cabin space. Fires on the press rather than the hold so it
        // matches the ground jump, and only with both feet on the deck --
        // otherwise the player could climb the air out of the aircraft.
        const bool jumpDown = (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
        if (jumpDown && !g_blackHawkCabinJumpHeld && g_blackHawkCabinGrounded) {
            g_blackHawkCabinVertVel = scene.camera.JumpStrength;
            g_blackHawkCabinGrounded = false;
        }
        g_blackHawkCabinJumpHeld = jumpDown;

        // Same gravity as the world, integrated against the deck instead of the
        // terrain. Landing resets to a clean stand rather than leaving a tiny
        // residual velocity that would re-trigger the fall next frame.
        if (!g_blackHawkCabinGrounded) {
            g_blackHawkCabinVertVel -= scene.camera.Gravity * cabinDeltaTime;
            g_blackHawkCabinHeight += g_blackHawkCabinVertVel * cabinDeltaTime;
            if (g_blackHawkCabinHeight <= 0.0f) {
                g_blackHawkCabinHeight = 0.0f;
                g_blackHawkCabinVertVel = 0.0f;
                g_blackHawkCabinGrounded = true;
            }
        }

        // Off the edge of the deck: step out and the aircraft stops holding the
        // player up. This is the self-directed exit -- walking out of an open
        // door mid-flight -- as opposed to the scripted E bail-out below, so it
        // hands the player to the ordinary world fall exactly where they are,
        // carrying their cabin momentum rather than teleporting them anywhere.
        //
        // Only checked while standing on the deck. Jumping across the cabin
        // briefly crosses the same air, and dropping the player for that would
        // make the deck impossible to jump on at all.
        const bool pastSide =
            std::fabs(g_blackHawkCabinLocal.x - centreX) >
                kCabinHalfWidth + kCabinEdgeGrace;
        const bool pastEnd =
            std::fabs(g_blackHawkCabinLocal.z - centreZ) >
                kCabinHalfLength + kCabinEdgeGrace;
        if (g_blackHawkCabinGrounded && (pastSide || pastEnd)) {
            // Hand over at the player's current world position, which the
            // block below has already been computing all along, so there is no
            // jump in the view at the moment control changes hands.
            vehicles.BailOutOfBlackHawk();
            vehicles.blackHawkBailedOut = false;
            g_blackHawkCabinLocalValid = false;
            g_blackHawkCabinHeight = 0.0f;
            g_blackHawkCabinVertVel = 0.0f;
            g_blackHawkCabinGrounded = true;
            scene.camera.IsGrounded = false;
            scene.camera.VerticalVelocity = 0.0f;
            return;
        }

        // Hard walls just outside the walkable deck. The grace band above is
        // what the player actually falls through; this only stops a mid-air
        // jump from carrying them clean out through the fuselage.
        const float wallX = kCabinHalfWidth + kCabinEdgeGrace;
        const float wallZ = kCabinHalfLength + kCabinEdgeGrace;
        g_blackHawkCabinLocal.x = (std::max)(centreX - wallX,
            (std::min)(centreX + wallX, g_blackHawkCabinLocal.x));
        g_blackHawkCabinLocal.z = (std::max)(centreZ - wallZ,
            (std::min)(centreZ + wallZ, g_blackHawkCabinLocal.z));
        g_blackHawkCabinLocal.y =
            vehicles.blackHawkRideHeight + g_blackHawkCabinHeight;

        // Cabin-local back to world through the same full roll/pitch/yaw the
        // airframe is drawn with, so the player rides the bank and the nose-down
        // rather than floating level while the aircraft tilts around them.
        const XMVECTOR offset = XMVectorSet(g_blackHawkCabinLocal.x,
                                            g_blackHawkCabinLocal.y,
                                            g_blackHawkCabinLocal.z, 0.0f);
        const XMMATRIX orientation = XMMatrixRotationRollPitchYaw(
            vehicles.blackHawkPitch, vehicles.blackHawkYaw,
            vehicles.blackHawkRoll);
        XMFLOAT3 rotated{};
        XMStoreFloat3(&rotated,
                      XMVector3TransformNormal(offset, orientation));
        const XMFLOAT3 centre = {
            vehicles.blackHawkPosition.x + rotated.x,
            vehicles.blackHawkPosition.y + rotated.y,
            vehicles.blackHawkPosition.z + rotated.z };

        // The attach point is the player's centre, but the camera is the eye,
        // which rides PlayerHeight above the feet -- so lift by half a body to
        // put the middle of the player on the authored spot.
        scene.camera.Position = { centre.x,
                                  centre.y + scene.camera.PlayerHeight * 0.5f,
                                  centre.z };
        // The cabin owns the vertical, so the world fall stays parked: its
        // velocity is zeroed and the floor is kept under the feet. Grounded
        // tracks the DECK rather than being pinned true, so a jump in here
        // still reads as airborne to everything downstream -- the weapon bob
        // and the landing thud key off this flag.
        scene.camera.VerticalVelocity = 0.0f;
        scene.camera.IsGrounded = g_blackHawkCabinGrounded;
        scene.camera.FloorY = scene.camera.Position.y - scene.camera.PlayerHeight;
        // Look out of the door the player is sitting in, once, at the start of
        // the ride. The airframe's forward is (sin, cos), so its right-hand
        // side is (cos, -sin); camera yaw is atan2(z, x) in degrees, which
        // makes the starboard view simply -yaw, and the port view that plus a
        // half turn. Level the pitch too: whatever the player was looking at on
        // the deployment map has nothing to do with where they now sit.
        if (g_blackHawkRideFacingPending) {
            g_blackHawkRideFacingPending = false;
            const float facing =
                -XMConvertToDegrees(vehicles.blackHawkYaw) +
                (g_playerRidesLeftSeat ? 180.0f : 0.0f);
            scene.camera.Yaw = facing;
            scene.camera.Pitch = 0.0f;
            scene.camera.ProcessMouseMovement(0.0f, 0.0f);
        }
        return;
    }
    // Left the aircraft: the next boarding re-seats from the authored door
    // rather than resuming wherever this ride left the player standing.
    g_blackHawkCabinLocalValid = false;
    // Bailing out mid-flight leaves the player where the aircraft is, stepped
    // out the right-hand door and falling. Unlike the scripted drop-off this
    // must not teleport them to the landing zone -- the whole point is to get
    // out early, wherever the bird happens to be.
    if (vehicles.blackHawkBailedOut) {
        vehicles.blackHawkBailedOut = false;
        const float rightX = std::cos(vehicles.blackHawkYaw);
        const float rightZ = -std::sin(vehicles.blackHawkYaw);
        constexpr float clearance = 2.5f;
        const XMFLOAT3 centre =
            vehicles.blackHawkFastRappel &&
                vehicles.blackHawkRappelProgress > 0.0f
            ? BlackHawkRappelPlayerWorldPosition()
            : vehicles.BlackHawkRidePosition();
        scene.camera.Position = {
            centre.x + rightX * clearance,
            centre.y + scene.camera.PlayerHeight * 0.5f,
            centre.z + rightZ * clearance };
        // Hand them over to gravity: clearing IsGrounded lets the normal fall
        // and landing code take it from here.
        scene.camera.VerticalVelocity = 0.0f;
        scene.camera.IsGrounded = false;
        return;
    }
    // Thrown clear of the wreck on impact. Uses the crash site rather than the
    // planned drop-off, which the bird never reached.
    if (vehicles.blackHawkJustCrashed && vehicles.blackHawkDroppedPlayer) {
        const float rightX = std::cos(vehicles.blackHawkYaw);
        const float rightZ = -std::sin(vehicles.blackHawkYaw);
        constexpr float thrownClear = 4.0f;
        scene.camera.Position = {
            vehicles.blackHawkPosition.x + rightX * thrownClear,
            vehicles.blackHawkCrashGroundY + scene.camera.PlayerHeight,
            vehicles.blackHawkPosition.z + rightZ * thrownClear };
        scene.camera.VerticalVelocity = 0.0f;
        scene.camera.IsGrounded = true;
        return;
    }
    if (vehicles.blackHawkDroppedPlayer) {
        // The bird reached the drop-off under power, so whatever the player
        // loaded aboard comes off with them. The crash branch above returns
        // before this point, which is what loses the squad with the airframe.
        g_marineDropPending = true;
        g_marineDropOrigin = { vehicles.blackHawkDropOff.x,
                               vehicles.blackHawkGroundY,
                               vehicles.blackHawkDropOff.z };
        if (vehicles.blackHawkFastRappel) {
            const XMFLOAT3 anchor = vehicles.BlackHawkRidePosition();
            scene.camera.Position = {
                anchor.x,
                vehicles.blackHawkGroundY + scene.camera.PlayerHeight,
                anchor.z };
            scene.camera.VerticalVelocity = 0.0f;
            scene.camera.IsGrounded = true;
            return;
        }
        // Step out the right-hand door, clear of the hull, on the same side the
        // player rode on. Forward is (sin, cos), so right is (cos, -sin).
        const float rightX = std::cos(vehicles.blackHawkYaw);
        const float rightZ = -std::sin(vehicles.blackHawkYaw);
        constexpr float exitDistance = 5.0f;
        // Released at the airframe's own height, not teleported to the ground:
        // the bird holds a hover well above the deck, so stepping out is a drop.
        // Gravity owns the rest -- leaving IsGrounded set would have the player
        // walk on air until something else cleared it.
        scene.camera.Position = {
            vehicles.blackHawkDropOff.x + rightX * exitDistance,
            vehicles.blackHawkPosition.y + scene.camera.PlayerHeight,
            vehicles.blackHawkDropOff.z + rightZ * exitDistance };
        scene.camera.VerticalVelocity = 0.0f;
        scene.camera.IsGrounded = false;
    }
}

// The deployment screen always resolves PlayerChoice before either run arms.
static LevelInsertionMode ResolvedInsertionMode() {
    // The level has the final say when it authors Spawn. The choice below is the
    // loadout's, carried over from the last deployment screen, and a hub never
    // opens one to clear it -- so without this a run flown to the island leaves
    // Helicopter behind and the hub reads it as "fly the player in".
    if (g_customLevelMode &&
        g_game.world.Level().insertionMode == LevelInsertionMode::Spawn)
        return LevelInsertionMode::Spawn;
    return g_playerInsertionChoice;
}

// Bullet-on-sheet-metal impact, attenuated by distance from the ear and pitched
// randomly within the metal-hit window. Shared by the vehicle-hull and
// metal-roof hits so both sound like the same material being struck.
static void PlayMetalHitAudio(const XMFLOAT3& position, float volumeScale = 1.0f) {
    const float dx = position.x - scene.camera.Position.x;
    const float dy = position.y - scene.camera.Position.y;
    const float dz = position.z - scene.camera.Position.z;
    const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    // Sheet metal rings; audible a good way off, but not across the island.
    constexpr float kReach = 65.0f;
    const float volume = (std::max)(0.0f, 1.0f - distance / kReach) * volumeScale;
    if (volume <= 0.01f) return;
    const float pitch = kMetalHitPitchMin +
        ((float)std::rand() / (float)RAND_MAX) *
            (kMetalHitPitchMax - kMetalHitPitchMin);
    g_metalHitAudio.Play(volume, pitch);
}


// Both defined with the other scene helpers, below.
static void ApplyTimeOfDay(TimeOfDay time);
void ApplyLiveWeatherState(WeatherState state);
// Defined next to the sky renderer, which is declared later than ApplyTimeOfDay.
static void RequestTimeOfDaySkyEnvironment(TimeOfDay time);

static void BeginDeploymentPlanning() {
    auto params = CurrentTerrainParams();
    params.heightScale = scene.terrainHeightScale;
    const float deploymentRadius = g_customLevelMode
        ? g_game.world.Level().deploymentRadius
        : kDefaultDeploymentRadius;
    // 20 points around the ring rather than 8, so the coast offers a real choice
    // of approach instead of eight fixed doors.
    //
    // The planning camera scales with this ring, keeping marker spacing stable
    // when an editor-authored radius is much larger than the historical 34 m.
    constexpr uint32_t deploymentZoneCount = 20;
    g_deploymentZones = DeploymentPlanner::BuildPerimeterZones(
        deploymentRadius, deploymentRadius, deploymentZoneCount,
        [&](float x, float z) {
            return (std::max)(0.0f,
                TerrainRendererDX12::HeightAt(params, x, z));
        });
    g_selectedDeploymentZone = -1;
    g_deploymentTarget = {};
    g_deploymentTargetValid = false;
    g_deploymentFlythroughTime = 0.0f;
    // Last mission's kit does not come back. Everything in the armory is hired
    // for one deployment, so this screen opens with nothing paid for and the
    // storefront quotes a price on every row again.
    ClearMissionRentals();
    // The squad is hired the same way and cannot be inherited either, or a
    // restart would deploy marines the player was never charged for.
    g_deploymentMarineCount = 0;
    LevelInsertionMode authored = g_customLevelMode
        ? g_game.world.Level().insertionMode
        : LevelInsertionMode::Helicopter;
    // Both of these mean "no single craft is named here", and this screen has to
    // open on one: PlayerChoice defers to the player, and Spawn belongs to a map
    // that is never planned for at all. Reaching here with Spawn means a level
    // set it and still ran the planning screen, so fall back rather than arm an
    // insertion the mode does not describe.
    if (authored == LevelInsertionMode::PlayerChoice ||
        authored == LevelInsertionMode::Spawn)
        authored = LevelInsertionMode::Helicopter;
    g_playerInsertionChoice = authored;
    scene.selectedGrenade = g_game.mission.Loadout().grenade;
    g_insertionChoicePending = true;
    // Re-arm the commander callout for this deployment; the screen itself plays
    // it once it is actually visible.
    g_readyToDropPlayed = false;
    g_game.session.StopTimer();
    scene.camera.FPSMode = false;
    // Open the planning fly-through in the light the last choice selected, so
    // restarting a night mission does not put the player back over a sunlit
    // island with NIGHT still highlighted.
    ApplyTimeOfDay(g_selectedTimeOfDay);
}

// The front-end score follows screen state rather than being started and
// stopped at every transition that leads into or out of a menu. One place
// decides, so a path nobody thought of -- quitting to menu, restarting, backing
// out of deployment -- cannot leave the music running under gameplay or drop it
// on a screen that should have it.
static void UpdateMenuMusic() {
    // The score runs under play on every map now, not just the range.
    const bool inLevel = g_game.session.Screen() == GameScreen::Level1;
    // Plays through the loading screen too: the score carries across the gap
    // rather than cutting out and back in around it.
    const bool wanted =
        g_game.session.Screen() == GameScreen::MainMenu ||
        g_insertionChoicePending ||
        inLevel;
    // Three levels. The front end carries the track on its own so it sits
    // forward; under gameplay it drops back to leave room for gunfire and
    // callouts; felling the comm tower opens it up to full.
    //
    // Tracked as its own value rather than folded into the start/stop test
    // below, because the volume changes while the music is already playing --
    // the early-out on an unchanged play state would otherwise never apply it.
    // The swell is scoped to the run that earned it: the flag survives until the
    // next level load, so without the screen test a tower felled on any map
    // would follow the player back to the main menu and play it at full.
    const float target =
        (g_commTowerMusicSwell && inLevel) ? 1.0f
        : inLevel                          ? kInGameMusicVolume
                                           : kMenuMusicVolume;
    // The deployment callout takes the track back to the top, so the planning
    // screen opens on the intro under the commander's voice rather than
    // wherever the menu loop had wandered to. StopLoop first: SetLoop on a live
    // voice only retargets its gain -- the buffer is resubmitted solely when the
    // voice does not exist, so without destroying it the track would carry on
    // from where it was.
    const bool restartNow = g_menuMusicRestartRequested;
    g_menuMusicRestartRequested = false;
    if (wanted && restartNow) {
        g_menuMusicAudio.StopLoop();
        g_menuMusicAudio.SetLoop(true, target);
        g_menuMusicLevel = target;
        g_menuMusicPlaying = true;
        return;
    }
    if (wanted == g_menuMusicPlaying) {
        // SetLoop on a live voice retargets its gain without restarting it, so
        // the swell rides the track rather than cutting it back to the top.
        if (wanted && target != g_menuMusicLevel) {
            g_menuMusicAudio.SetLoop(true, target);
            g_menuMusicLevel = target;
        }
        return;
    }
    // kMenuMusicVolume is the authored level for this track. It is the source
    // gain, not the Music bus, so the player's music slider still scales from
    // here rather than fighting a level baked into the mix.
    if (wanted) g_menuMusicAudio.SetLoop(true, target);
    else        g_menuMusicAudio.StopLoop();
    g_menuMusicLevel = target;
    g_menuMusicPlaying = wanted;
}

static void UpdateDeploymentPlanningCamera(float deltaTime) {
    if (!g_insertionChoicePending) {
        scene.cameraFarOverride = 0.0f;
        return;
    }
    if (g_game.loading.Active()) {
        scene.cameraFarOverride = 0.0f;
        return;
    }
    // The automatic orbit only advances when the player is not steering it, the
    // flick has coasted to a stop, and the pause after that has run out.
    // Holding the clock still (rather than letting it run and snapping back) is
    // what makes the resume continue from wherever the map was left.
    const float step = (std::max)(0.0f, deltaTime);
    // Banked by RenderInsertionChoiceScreen last frame; applied whether or not
    // the button is still down so the final sliver of a drag is never dropped.
    const float dragged = g_deploymentOrbitPendingDrag;
    g_deploymentOrbitPendingDrag = 0.0f;
    const float tilted = g_deploymentOrbitPendingTilt;
    g_deploymentOrbitPendingTilt = 0.0f;
    if (dragged != 0.0f) {
        g_deploymentOrbitOffset =
            std::fmod(g_deploymentOrbitOffset + dragged, XM_2PI);
    }
    g_deploymentOrbitElevationOffset += tilted;
    if (g_deploymentOrbitDragging) {
        // Turn this frame's travel into a speed for the flick.
        if (step > 1e-5f) {
            const float sampled = dragged / step;
            // Smoothed so the release picks up the gesture's speed rather than
            // whatever the final frame happened to catch -- a flick that eases
            // off in its last few milliseconds should still throw the map.
            g_deploymentOrbitVelocity +=
                (sampled - g_deploymentOrbitVelocity) * 0.35f;
            g_deploymentOrbitVelocity = std::clamp(
                g_deploymentOrbitVelocity,
                -kDeploymentOrbitMaxSpin, kDeploymentOrbitMaxSpin);
        }
        g_deploymentOrbitResumeDelay = kDeploymentOrbitResumeSeconds;
    } else if (std::fabs(g_deploymentOrbitVelocity) >
               kDeploymentOrbitMinSpin) {
        // Coasting. Exponential decay so the slowdown is framerate independent
        // -- a linear subtraction would stop sooner on a fast machine.
        g_deploymentOrbitOffset += g_deploymentOrbitVelocity * step;
        g_deploymentOrbitVelocity *=
            std::pow(kDeploymentOrbitDamping, step);
        g_deploymentOrbitOffset = std::fmod(g_deploymentOrbitOffset, XM_2PI);
        // The wait starts once the map is still, not at the moment of release,
        // so a long spin is not eaten by the countdown running underneath it.
        g_deploymentOrbitResumeDelay = kDeploymentOrbitResumeSeconds;
    } else if (g_deploymentOrbitResumeDelay > 0.0f) {
        g_deploymentOrbitVelocity = 0.0f;
        g_deploymentOrbitResumeDelay =
            (std::max)(0.0f, g_deploymentOrbitResumeDelay - step);
    } else {
        g_deploymentFlythroughTime += step;
    }
    auto params = CurrentTerrainParams();
    const float islandRadius = 43.0f * (std::max)(
        params.islandScaleX, params.islandScaleZ);
    const float deploymentRadius = g_customLevelMode
        ? g_game.world.Level().deploymentRadius
        : kDefaultDeploymentRadius;
    const DeploymentPlanner::CameraFrame frame =
        DeploymentPlanner::BuildCameraFrame(islandRadius, deploymentRadius);
    // Scroll zoom rides on top of the authored rig. The far plane is derived
    // from the unzoomed orbit, so scale it by the same factor when pulling out
    // or the ocean and the far shore start clipping at the back of the frame.
    const float zoom = std::clamp(g_deploymentZoom,
                                  kDeploymentZoomMin, kDeploymentZoomMax);
    scene.cameraFarOverride = frame.farPlane * (std::max)(1.0f, zoom);
    const float angle =
        g_deploymentFlythroughTime * 0.13f + g_deploymentOrbitOffset;
    constexpr float lookAtHeight = 4.0f;
    const float baseHorizontal = frame.orbitRadius * zoom;
    const float baseVertical = frame.height * zoom - lookAtHeight;
    const float orbitDistance = std::sqrt(
        baseHorizontal * baseHorizontal + baseVertical * baseVertical);
    const float defaultElevation = std::atan2(baseVertical, baseHorizontal);
    g_deploymentOrbitElevationOffset = std::clamp(
        g_deploymentOrbitElevationOffset,
        kDeploymentOrbitMinElevation - defaultElevation,
        kDeploymentOrbitMaxElevation - defaultElevation);
    const float elevation =
        defaultElevation + g_deploymentOrbitElevationOffset;
    const float horizontalRadius = std::cos(elevation) * orbitDistance;
    scene.camera.Position = {
        std::sin(angle) * horizontalRadius,
        lookAtHeight + std::sin(elevation) * orbitDistance,
        std::cos(angle) * horizontalRadius };
    const XMFLOAT3 toCenter{
        -scene.camera.Position.x,
        lookAtHeight - scene.camera.Position.y,
        -scene.camera.Position.z };
    const float horizontal = std::sqrt(
        toCenter.x * toCenter.x + toCenter.z * toCenter.z);
    scene.camera.Yaw = XMConvertToDegrees(
        std::atan2(toCenter.z, toCenter.x));
    scene.camera.Pitch = XMConvertToDegrees(
        std::atan2(toCenter.y, horizontal));
    scene.camera.ProcessMouseMovement(0.0f, 0.0f);
}
