#pragma once

// Private application implementation; included once by main.cpp in dependency order.

// Exfil marker. The escape boat sits 62 m offshore, well past the point where
// a small hull reads against open water, so without a marker it is a win
// condition the player cannot find. Drawn only once boarding would actually
// work, so it never promises an exit that is still gated on the objective.
static void DrawEscapeBoatMarker(CXMMATRIX view, CXMMATRIX projection) {
    const VehicleSystem& vehicles = g_game.vehicles;
    if (!vehicles.EscapeBoatReady()) return;
    if (g_game.session.Screen() != GameScreen::Level1) return;
    if (g_emptyLevelMode || !g_game.session.TimerRunning()) return;
    // Same gate the boarding test uses, so the marker and the trigger agree.
    const bool objectiveMet = g_game.mission.Stats().commTowersTotal == 0 ||
                              g_game.mission.CommTowerObjectiveComplete();
    if (!objectiveMet) return;

    const XMFLOAT3 anchor{ vehicles.escapeBoatPosition.x,
                           vehicles.escapeBoatPosition.y + 3.4f,
                           vehicles.escapeBoatPosition.z };
    const XMVECTOR clip = XMVector3Transform(
        XMLoadFloat3(&anchor), view * projection);
    const float w = XMVectorGetW(clip);
    if (w <= 0.01f) return;   // behind the camera

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const ImVec2 screen{
        (XMVectorGetX(clip) / w * 0.5f + 0.5f) * display.x,
        (1.0f - (XMVectorGetY(clip) / w * 0.5f + 0.5f)) * display.y };

    const float dx = scene.camera.Position.x - vehicles.escapeBoatPosition.x;
    const float dz = scene.camera.Position.z - vehicles.escapeBoatPosition.z;
    const float distance = std::sqrt(dx * dx + dz * dz);

    ImDrawList* draw = ImGui::GetForegroundDrawList();
    const ImU32 green = IM_COL32(70, 235, 120, 235);
    draw->AddCircle(screen, 13.0f, green, 0, 2.5f);
    draw->AddCircleFilled(screen, 4.0f, green);
    char label[64];
    std::snprintf(label, sizeof(label), "EXFIL  %.0f m", distance);
    const ImVec2 size = ImGui::CalcTextSize(label);
    // Dark plate behind the text: it is read against open water and sky.
    draw->AddRectFilled(
        ImVec2(screen.x - size.x * 0.5f - 4.0f, screen.y + 17.0f),
        ImVec2(screen.x + size.x * 0.5f + 4.0f, screen.y + 21.0f + size.y),
        IM_COL32(6, 18, 12, 170), 3.0f);
    draw->AddText(ImVec2(screen.x - size.x * 0.5f, screen.y + 19.0f),
                  green, label);
}

// Friendly marker. Marines wear the same fatigues as the bandits they are
// fighting, so at a glance in tall grass there is nothing to tell them apart.
// A small dot over the head is enough: it reads instantly without covering the
// body the player is trying to shoot past.
static void DrawMarineFriendlyMarkers(CXMMATRIX view, CXMMATRIX projection) {
    if (g_bandits.empty()) return;
    if (g_game.session.Screen() != GameScreen::Level1) return;
    const XMMATRIX viewProjection = view * projection;
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    for (const auto& marine : g_bandits) {
        if (!marine || marine->faction != Faction::Marine) continue;
        // Dead allies are no longer a friendly-fire hazard, and a dot hovering
        // over a corpse just clutters the fight.
        if (marine->Dead() || !marine->visible) continue;
        // Just clear of the head: the gun sits at footOffset + 1.48, so this is
        // roughly a head-height above that.
        const XMFLOAT3 anchor{ marine->position.x,
                               marine->position.y + marine->footOffset + 2.05f,
                               marine->position.z };
        const XMVECTOR clip = XMVector3Transform(
            XMLoadFloat3(&anchor), viewProjection);
        const float w = XMVectorGetW(clip);
        if (w <= 0.01f) continue;   // behind the camera
        const ImVec2 screen{
            (XMVectorGetX(clip) / w * 0.5f + 0.5f) * display.x,
            (1.0f - (XMVectorGetY(clip) / w * 0.5f + 0.5f)) * display.y };
        // Shrink with distance so a squad far off does not read as a row of
        // big blobs, with a floor so it never disappears entirely.
        const float dx = marine->position.x - scene.camera.Position.x;
        const float dy = marine->position.y - scene.camera.Position.y;
        const float dz = marine->position.z - scene.camera.Position.z;
        const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        const float radius = (std::max)(2.0f, 5.0f - distance * 0.03f);

        // Friendly blue, with a hard dark edge under everything: what makes a
        // marker read as bright is the contrast beneath it, not the fill.
        const ImU32 tint    = IM_COL32(120, 195, 255, 255);
        const ImU32 shadow  = IM_COL32(0, 0, 0, 235);

        // Nameplate: callsign on top, range under it, dot below both -- the
        // dot stays on the anchor so it still points at the head.
        char range[24];
        std::snprintf(range, sizeof(range), "%.0f m", distance);
        const ImVec2 nameSize = ImGui::CalcTextSize(marine->callsign.c_str());
        const ImVec2 rangeSize = ImGui::CalcTextSize(range);

        const float nameY = screen.y - radius - 34.0f;
        const float rangeY = nameY + nameSize.y + 1.0f;
        const ImVec2 namePos(screen.x - nameSize.x * 0.5f, nameY);
        const ImVec2 rangePos(screen.x - rangeSize.x * 0.5f, rangeY);

        // Offset drop shadow rather than an outline pass: one extra draw per
        // string, and enough to hold the text against sky or grass.
        draw->AddText(ImVec2(namePos.x + 1.0f, namePos.y + 1.0f), shadow,
                      marine->callsign.c_str());
        draw->AddText(namePos, tint, marine->callsign.c_str());
        draw->AddText(ImVec2(rangePos.x + 1.0f, rangePos.y + 1.0f), shadow,
                      range);
        draw->AddText(rangePos, tint, range);

        // Downward triangle pointing at the head. Drawn twice: an outset black
        // copy first as the hard edge, then the blue on top.
        const float half = radius + 1.6f;          // half-width at the top edge
        const float drop = (radius + 1.6f) * 1.7f; // apex below that edge
        const auto triangle = [&](float grow, ImU32 colour) {
            draw->AddTriangleFilled(
                ImVec2(screen.x - half - grow, screen.y - drop * 0.5f - grow),
                ImVec2(screen.x + half + grow, screen.y - drop * 0.5f - grow),
                ImVec2(screen.x, screen.y + drop * 0.5f + grow),
                colour);
        };
        triangle(2.0f, IM_COL32(0, 0, 0, 255));
        triangle(0.0f, tint);
    }
}

// Debug overlay for impact decals. They are a per-pixel volume test with no
// geometry, so a mark that lands on the wrong face or the wrong size leaves
// nothing to inspect in a capture. This draws what the shader is testing:
// the disc in its own surface plane, the normal it was projected along, and
// the depth band either side of that plane.
static void DrawImpactDecalDebug(CXMMATRIX view, CXMMATRIX projection) {
    if (!g_showDecalDebug || g_impactDecals.empty()) return;
    const XMMATRIX viewProjection = view * projection;
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    auto project = [&](const XMFLOAT3& world, ImVec2& out) -> bool {
        const XMVECTOR clip =
            XMVector3Transform(XMLoadFloat3(&world), viewProjection);
        const float w = XMVectorGetW(clip);
        if (w <= 0.01f) return false;
        out.x = (XMVectorGetX(clip) / w * 0.5f + 0.5f) * display.x;
        out.y = (1.0f - (XMVectorGetY(clip) / w * 0.5f + 0.5f)) * display.y;
        return true;
    };

    int index = 0;
    for (const ImpactDecal& decal : g_impactDecals) {
        const float life = decal.age / kImpactDecalLifetime;
        const float strength = life < 0.75f
            ? 1.0f : 1.0f - (life - 0.75f) / 0.25f;
        // Green while at full strength, amber once it starts fading out, so a
        // mark that vanished early is distinguishable from one that never
        // spawned.
        const ImU32 colour = strength > 0.999f
            ? IM_COL32(90, 230, 120, 220)
            : IM_COL32(245, 190, 60, 220);

        const XMVECTOR n = XMLoadFloat3(&decal.normal);
        // Any vector not parallel to the normal gives a basis for the disc.
        XMVECTOR reference = XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
        if (std::fabs(decal.normal.y) > 0.9f)
            reference = XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f);
        const XMVECTOR tangent =
            XMVector3Normalize(XMVector3Cross(reference, n));
        const XMVECTOR bitangent = XMVector3Cross(n, tangent);

        // The disc itself, drawn in the surface plane rather than as a screen
        // circle: a screen circle would hide exactly the orientation bugs this
        // is meant to expose.
        constexpr int kSegments = 20;
        ImVec2 previous;
        bool havePrevious = false;
        bool anyVisible = false;
        for (int i = 0; i <= kSegments; ++i) {
            const float angle = (float)i / kSegments * XM_2PI;
            const XMVECTOR offset = tangent * (std::cos(angle) * decal.radius) +
                                    bitangent * (std::sin(angle) * decal.radius);
            XMFLOAT3 point;
            XMStoreFloat3(&point, XMLoadFloat3(&decal.position) + offset);
            ImVec2 screen;
            if (!project(point, screen)) { havePrevious = false; continue; }
            if (havePrevious) draw->AddLine(previous, screen, colour, 1.5f);
            previous = screen;
            havePrevious = true;
            anyVisible = true;
        }
        if (!anyVisible) { ++index; continue; }

        // Normal, at the same 0.6 * radius the shader uses for its depth band,
        // so the line length shows the volume a pixel has to fall inside.
        const float band = decal.radius * 0.6f;
        XMFLOAT3 tip;
        XMStoreFloat3(&tip, XMLoadFloat3(&decal.position) + n * band);
        ImVec2 centreScreen, tipScreen;
        if (project(decal.position, centreScreen) && project(tip, tipScreen)) {
            draw->AddLine(centreScreen, tipScreen,
                          IM_COL32(120, 195, 255, 235), 2.0f);
            draw->AddCircleFilled(tipScreen, 3.0f, IM_COL32(120, 195, 255, 235));
            char label[48];
            std::snprintf(label, sizeof(label), "%d  r%.3f  s%.2f",
                          index, decal.radius, strength);
            draw->AddText(ImVec2(centreScreen.x + 6.0f, centreScreen.y - 6.0f),
                          colour, label);
        }
        ++index;
    }
}

// Debug overlay: each enemy's vision cone (facing direction, FOV, range),
// color-coded by awareness state so patrol/alert/combat is readable at a
// glance. Projects the cone edges and arc into screen space the same way
// DrawDXRDDGIProbeDebug projects probe markers.
static void DrawEnemyVisionCones(CXMMATRIX view, CXMMATRIX projection) {
    if (!g_showEnemyVisionCones || g_bandits.empty()) return;
    const XMMATRIX viewProjection = view * projection;
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    auto project = [&](const XMFLOAT3& world, ImVec2& out) -> bool {
        const XMVECTOR clip = XMVector3Transform(XMLoadFloat3(&world), viewProjection);
        const float w = XMVectorGetW(clip);
        if (w <= 0.01f) return false;
        out.x = (XMVectorGetX(clip) / w * 0.5f + 0.5f) * display.x;
        out.y = (1.0f - (XMVectorGetY(clip) / w * 0.5f + 0.5f)) * display.y;
        return true;
    };
    for (const auto& bandit : g_bandits) {
        if (!bandit || bandit->Dead() || bandit->turretGunner) continue;
        const ImU32 color = bandit->Awareness() == SkinnedEnemy::AwarenessState::Combat
            ? IM_COL32(235, 60, 55, 200)
            : (bandit->Awareness() == SkinnedEnemy::AwarenessState::Alert
                ? IM_COL32(245, 190, 40, 200)
                : IM_COL32(80, 210, 110, 170));
        const float range = bandit->VisionRange();
        const float halfFov = bandit->VisionHalfFovRadians();
        const XMFLOAT3 apex{ bandit->position.x,
                             bandit->position.y + bandit->footOffset + 1.5f,
                             bandit->position.z };
        constexpr int kArcSegments = 16;
        ImVec2 apexScreen;
        if (!project(apex, apexScreen)) continue;
        ImVec2 previous;
        bool havePrevious = false;
        for (int i = 0; i <= kArcSegments; ++i) {
            const float t = (float)i / (float)kArcSegments;
            const float angle = bandit->yaw - halfFov + t * (2.0f * halfFov);
            const XMFLOAT3 rim{ apex.x + std::sin(angle) * range,
                                apex.y,
                                apex.z + std::cos(angle) * range };
            ImVec2 rimScreen;
            if (!project(rim, rimScreen)) { havePrevious = false; continue; }
            if (i == 0 || i == kArcSegments)
                draw->AddLine(apexScreen, rimScreen, color, 1.5f);
            if (havePrevious) draw->AddLine(previous, rimScreen, color, 1.5f);
            previous = rimScreen;
            havePrevious = true;
        }
    }
}

// Rail lasers and the existing IR/NVG designator share the same world-space
// cast. The attachment is visible red; the equipment designator remains white
// and only appears through the goggle ramp.
//
// A real IR designator emits outside the visible band: to the naked eye there is
// nothing there at all, and through an intensifier the beam is a hard white line
// because the tube's phosphor output is monochrome regardless of input
// wavelength. So this draws only while the goggles are up, and draws white
// rather than the red of a visible-light laser.
//
// The line runs from the muzzle to the point the weapon would actually hit,
// found with the same segment cast the shot itself uses. Drawing it from the
// camera instead would be easier and wrong -- the offset between eye and muzzle
// is exactly what makes a laser useful at close range.
static void UpdateIRLaser() {
    scene.irLaser.visible = false;
    const SGE::ResolvedWeaponStats weaponStats =
        scene.player.ResolveWeaponStats(GunModel::SelectedWeapon());
    const bool visibleRailLaser = weaponStats.laserSight;
    // The IR beam fades with the goggle ramp. A visible laser has no such ramp
    // and stays red even when NVG happens to be raised at the same time.
    const float visibility = visibleRailLaser ? 1.0f :
        (std::min)(1.0f, g_nightVisionBlend);
    if (visibility <= 0.01f) return;
    if (!scene.gun.visible || scene.player.health <= 0.0f) return;
    // Thrown and placed equipment has no barrel to bolt a designator to.
    if (GunModel::C4Selected() || GunModel::HarpoonSelected()) return;
    // The scope's own reticle is the aiming reference when glassing; a beam
    // across the lens would only obscure it.
    if (scene.sniperScopeBlend > 0.25f) return;

    // Origin rides the gun; direction comes from the camera. A designator is
    // zeroed to point of aim, so the dot belongs on the crosshair.
    //
    // Casting along the gun's own forward axis instead is the physically
    // literal reading and plays badly: the view model sways hard when strafing,
    // so holding right swung the barrel left and threw the dot metres off the
    // crosshair at any distance. The sway is a view-model flourish, not real
    // weapon movement -- the bullets ignore it too, since spread is centred on
    // the camera axis. Following it would make the laser disagree with both the
    // crosshair and the shot.
    const XMFLOAT3 muzzle = scene.GetMuzzleWorldPosition();
    const XMFLOAT3 origin = scene.camera.Position;
    constexpr float kRange = 260.0f;

    // Carry the weapon's idle sway into the cast direction, so the far end
    // drifts with the same rhythm the muzzle does and the beam translates as one
    // rigid line. Aiming straight down the camera axis instead pinned the far
    // end still while the near end rode the sway, which made the beam pivot
    // about the dot -- the opposite of how a rail-mounted designator behaves.
    //
    // Rotated in the camera's own frame (yaw about its up, pitch about its
    // right) rather than about world axes, so the drift stays consistent when
    // looking up or down. The sway is capped at +/-0.9 degrees, so the dot stays
    // within a whisker of the crosshair at any usable range.
    const XMVECTOR camFront = XMVector3Normalize(
        XMLoadFloat3(&scene.camera.Front));
    const XMVECTOR camUp = XMVector3Normalize(XMLoadFloat3(&scene.camera.Up));
    XMVECTOR camRight = XMVector3Cross(camUp, camFront);
    if (XMVectorGetX(XMVector3LengthSq(camRight)) < 1e-6f)
        camRight = XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f);
    camRight = XMVector3Normalize(camRight);
    const XMMATRIX swayRotation =
        XMMatrixRotationAxis(camUp, XMConvertToRadians(scene.gunSwayYaw)) *
        XMMatrixRotationAxis(camRight, XMConvertToRadians(scene.gunSwayPitch));
    XMFLOAT3 aimDirection;
    XMStoreFloat3(&aimDirection,
        XMVector3Normalize(XMVector3TransformNormal(camFront, swayRotation)));

    const XMFLOAT3 end{ origin.x + aimDirection.x * kRange,
                        origin.y + aimDirection.y * kRange,
                        origin.z + aimDirection.z * kRange };
    XMFLOAT3 target = end;
    float closestDistanceSq = FLT_MAX;
    auto accept = [&](const XMFLOAT3& hit) {
        const float dx = hit.x - origin.x, dy = hit.y - origin.y,
                    dz = hit.z - origin.z;
        const float distanceSq = dx * dx + dy * dy + dz * dz;
        if (distanceSq < closestDistanceSq) {
            closestDistanceSq = distanceSq;
            target = hit;
        }
    };
    XMFLOAT3 hit;
    if (scene.useDestruction && g_destruction.IsInitialized() &&
        g_destruction.HitTestSegment(origin, end, 0.03f, hit)) accept(hit);
    if (HitTerrainSegment(origin, end, 0.03f, hit)) accept(hit);

    // Published for ForwardRenderer to draw as world geometry rather than a HUD
    // overlay, so the depth buffer clips it against the scene.
    //
    // Start a little ahead of the muzzle: the emitter sits within a few
    // centimetres of the near clip plane, and geometry straddling that plane
    // gets sliced open -- the beam would flare into a wedge across the bottom of
    // the screen. Slid along the muzzle-to-target line so it moves down the beam
    // rather than nudging it off-line.
    const XMVECTOR beamDirection = XMVector3Normalize(
        XMLoadFloat3(&target) - XMLoadFloat3(&muzzle));
    XMFLOAT3 beamStart;
    XMStoreFloat3(&beamStart,
                  XMLoadFloat3(&muzzle) + beamDirection * 0.35f);
    scene.irLaser.start = beamStart;
    scene.irLaser.end = target;
    scene.irLaser.color = visibleRailLaser ?
        XMFLOAT3{ 1.0f, 0.025f, 0.012f } : XMFLOAT3{ 1.0f, 1.0f, 1.0f };
    scene.irLaser.visibility = visibility;
    scene.irLaser.visible = true;
}

// Debug beam down the true shot ray, for checking that what a sight shows and
// where the round goes are the same line.
//
// Deliberately NOT the IR designator above. That one starts at the muzzle and
// carries the viewmodel's idle sway, both of which are presentation: it answers
// "where is the gun pointing". This answers "where does the bullet go", which is
// camera.Position along camera.Front with no sway and no muzzle offset, because
// that is the ray ShootBullet spreads around. Keeping them separate is the whole
// point -- if the two beams diverge, the difference is exactly the presentation
// error, which is what makes this useful for aligning an optic.
static void UpdateAimDebugRay() {
    scene.aimDebugRay.visible = false;
    if (!scene.showAimDebugRay) return;
    if (scene.player.health <= 0.0f) return;

    const XMFLOAT3 origin = scene.camera.Position;
    const XMVECTOR camFront = XMVector3Normalize(
        XMLoadFloat3(&scene.camera.Front));
    XMFLOAT3 direction;
    XMStoreFloat3(&direction, camFront);

    const float range = (std::max)(1.0f, scene.aimDebugRayLength);
    const XMFLOAT3 rayEnd{ origin.x + direction.x * range,
                        origin.y + direction.y * range,
                        origin.z + direction.z * range };

    // Stop at the first thing hit, so the dot marks the surface a shot would
    // strike rather than running through it.
    XMFLOAT3 target = rayEnd;
    float closestDistanceSq = FLT_MAX;
    auto accept = [&](const XMFLOAT3& hit) {
        const float dx = hit.x - origin.x, dy = hit.y - origin.y,
                    dz = hit.z - origin.z;
        const float distanceSq = dx * dx + dy * dy + dz * dz;
        if (distanceSq < closestDistanceSq) {
            closestDistanceSq = distanceSq;
            target = hit;
        }
    };
    XMFLOAT3 hit;
    if (scene.useDestruction && g_destruction.IsInitialized() &&
        g_destruction.HitTestSegment(origin, rayEnd, 0.03f, hit)) accept(hit);
    if (HitTerrainSegment(origin, rayEnd, 0.03f, hit)) accept(hit);

    // Pushed off the eye by more than the near plane: a beam starting exactly
    // at the camera straddles it and smears across the screen.
    XMFLOAT3 beamStart;
    XMStoreFloat3(&beamStart, XMLoadFloat3(&origin) + camFront * 0.35f);
    scene.aimDebugRay.start = beamStart;
    scene.aimDebugRay.end = target;
    // Green, so it reads apart from the red rail laser and the white IR beam
    // when more than one is up at once.
    scene.aimDebugRay.color = XMFLOAT3{ 0.10f, 1.0f, 0.20f };
    scene.aimDebugRay.visibility = 1.0f;
    scene.aimDebugRay.visible = true;
}
