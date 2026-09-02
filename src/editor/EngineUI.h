#ifndef ENGINE_UI_H
#define ENGINE_UI_H

#include <imgui.h>
#include "Scene.h"
#include "VisibilityBufferDX12.h"
#include "DestructionDX12.h"
#include "VirtualInput.h"
#include "GrassField.h"
#include "ProfilerDX12.h"
#include "MeshShaderDX12.h"
#include "StaticBufferDX12.h"
#include "GunModel.h"   // SelectedWeapon() -- indexes the HUD ammo readout
#include "ArmsModel.h"  // arms placement controls, tuned against the weapon
#include "GunAudio.h"   // AudioDevice/AudioBus -- the Audio Mix sliders
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>

// Forward declare raytracing context
struct RaytracingContext;
extern RaytracingContext g_rt;
extern ProfilerDX12 g_profiler;
// Shift-sprint state for the HUD's sprint indicator. Owned by main.cpp.
extern bool g_playerSprinting;
// Sprint stamina for the HUD's sprint meter, all owned by main.cpp.
// `g_staminaSeconds` counts sprint seconds remaining out of `kStaminaMaxSeconds`;
// `g_staminaExhausted` is set when it empties and stays set until enough has
// rebuilt to sprint again, which is what turns the meter red.
extern float g_staminaSeconds;
extern bool g_staminaExhausted;
extern const float kStaminaMaxSecondsUI;
// Wireframe overlay for the prefab volumes the player collides against.
extern bool g_showCollisionDebug;
extern UINT g_forwardDrawCalls;
extern UINT g_shadowDrawCalls;
extern UINT g_visibilityDrawCalls;
extern UINT g_shadowBatches;
extern UINT g_shadowBatchInstances;
extern UINT g_shadowCachedFarCascades;
extern UINT g_shadowRefreshedFarCascades;
extern UINT g_destructionBatchesThisFrame;
extern UINT g_destructionChunksSubmittedThisFrame;
extern UINT g_destructionCulledThisFrame;
// True when the device reports DXR Tier 1.1 (inline RayQuery), which the
// enhanced-visuals tier requires. Published from main so the UI does not need
// the renderer object.
extern bool g_inlineRaytracingSupported;
extern bool g_bindlessMaterialsReady;
extern bool g_bindlessMaterialsActive;
// True while terrain is being shaded through the visibility resolve this
// frame rather than the forward pass. Defined in main.cpp.
extern bool g_terrainInVisibilityBuffer;
// True while destruction chunks are shaded by the visibility resolve rather
// than redrawn in the forward extensions pass. Defined in main.cpp.
extern bool g_destructionInVisibilityBuffer;
extern UINT g_dxrDDGIProbeCount;
extern UINT g_dxrDDGICellCount;
extern float g_dxrDDGICellSize;
extern MeshShaderDX12 g_meshShader;

// Skinned Bandit enemy (defined in main.cpp) -- surfaced for debug readout.
// BanditDebugText renders a one-line status; defined in main.cpp where the
// SkinnedEnemy type is complete.
void BanditDebugText();
extern bool g_showEnemyVisionCones;
void RequestLiveDXRDDGIRebuild();
void MatchFoliageMaterialToGrass();
void ApplyLiveWeatherState(WeatherState state);

// Comm-tower objective status for the HUD. Returns false when the level has no
// tower (or it has already been destroyed), so the readout only appears on maps
// that actually carry one. Defined in main.cpp, which owns the prefab runtime.
bool CommTowerObjectiveStatus(float& health, float& maxHealth);

// Aircraft objective status for the HUD. Unlike the tower this one moves, so it
// reports the airframe's current world position for a tracking marker alongside
// its health. `down` is set once it has been shot down and is crashing or
// landed. Returns false when there is nothing to mark. Defined in main.cpp.
bool ObjectivePlaneStatus(DirectX::XMFLOAT3& position, float& health,
                          float& maxHealth, bool& down);

// On-screen dual-stick controls: analog movement and analog camera look.
inline void RenderMovementPad() {
    virtualInput.moveX = virtualInput.moveY = 0.0f;
    virtualInput.down = virtualInput.shoot = false;
    virtualInput.lookX = virtualInput.lookY = 0.0f;

    if (!virtualInput.showPad) return;

    constexpr float controlWidth = 155.0f;

    // A pad button counts as "held" while the cursor sits on it AND the left
    // mouse is down. IsItemActive() alone drops the moment the cursor stops
    // moving over the button, so holding still stopped the movement -- hover +
    // IsMouseDown stays true with a stationary cursor.
    auto isHeld = []() {
        return ImGui::IsItemHovered() && ImGui::IsMouseDown(ImGuiMouseButton_Left);
    };

    auto thumbstick = [&](const char* id) {
        constexpr float stickSize = 116.0f;
        constexpr float stickRadius = 48.0f;
        constexpr float knobRadius = 17.0f;
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                             (controlWidth - stickSize) * 0.5f);
        ImGui::InvisibleButton(id, ImVec2(stickSize, stickSize),
                               ImGuiButtonFlags_MouseButtonLeft);
        const bool active = ImGui::IsItemActive();
        const ImVec2 stickMin = ImGui::GetItemRectMin();
        const ImVec2 center(stickMin.x + stickSize * 0.5f,
                            stickMin.y + stickSize * 0.5f);
        ImVec2 value(0.0f, 0.0f);
        if (active) {
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            value.x = (mouse.x - center.x) / stickRadius;
            value.y = (mouse.y - center.y) / stickRadius;
            const float length = std::sqrt(value.x * value.x + value.y * value.y);
            if (length > 1.0f) { value.x /= length; value.y /= length; }
            constexpr float deadZone = 0.12f;
            if (length <= deadZone) {
                value = ImVec2(0.0f, 0.0f);
            } else {
                const float clamped = (std::min)(1.0f, length);
                const float scale = ((clamped - deadZone) / (1.0f - deadZone)) / clamped;
                value.x *= scale; value.y *= scale;
            }
        }
        ImDrawList* draw = ImGui::GetWindowDrawList();
        draw->AddCircleFilled(center, stickRadius, IM_COL32(25, 28, 34, 220), 40);
        draw->AddCircle(center, stickRadius, IM_COL32(130, 140, 155, 255), 40, 2.0f);
        draw->AddLine(ImVec2(center.x-stickRadius+9.0f, center.y),
                      ImVec2(center.x+stickRadius-9.0f, center.y), IM_COL32(75,82,94,180));
        draw->AddLine(ImVec2(center.x, center.y-stickRadius+9.0f),
                      ImVec2(center.x, center.y+stickRadius-9.0f), IM_COL32(75,82,94,180));
        const ImVec2 knob(center.x + value.x * stickRadius,
                          center.y + value.y * stickRadius);
        draw->AddCircleFilled(knob, knobRadius,
            active ? IM_COL32(86,156,255,255) : IM_COL32(105,112,124,255), 32);
        draw->AddCircle(knob, knobRadius, IM_COL32(205,218,235,255), 32, 1.5f);
        return value;
    };

    // Movement belongs under left thumb. Keep it above bottom-left health bar.
    ImGui::SetNextWindowSize(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImVec2(20.0f, ImGui::GetIO().DisplaySize.y - 88.0f),
                            ImGuiCond_Always, ImVec2(0.0f, 1.0f));
    ImGui::Begin("Movement Stick", nullptr,
                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove);
    ImGui::TextUnformatted("Move");
    const ImVec2 moveStick = thumbstick("Move thumbstick");
    virtualInput.moveX = moveStick.x;
    virtualInput.moveY = -moveStick.y;
    ImGui::End();

    // Camera and action controls stay under right thumb.
    ImGui::SetNextWindowSize(ImVec2(0, 0), ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - 20.0f,
                                   ImGui::GetIO().DisplaySize.y - 20.0f),
                            ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    ImGui::Begin("Look / Actions", &virtualInput.showPad,
                 ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove);
    ImGui::TextUnformatted("Look");
    const ImVec2 lookStick = thumbstick("Look thumbstick");
    virtualInput.lookX = lookStick.x;
    virtualInput.lookY = -lookStick.y;

    ImGui::Separator();

    // Jump fires once per click; the camera ignores it unless FPS mode is on.
    if (ImGui::Button("Jump", ImVec2(74.0f, 34.0f))) virtualInput.jump = true;
    ImGui::SameLine();
    ImGui::Button("Down##fly", ImVec2(74.0f, 34.0f));
    if (isHeld()) virtualInput.down = true;

    ImGui::Button("Shoot", ImVec2(controlWidth, 34.0f));
    if (isHeld()) virtualInput.shoot = true;

    ImGui::SetNextItemWidth(controlWidth);
    ImGui::SliderFloat("Look Speed", &virtualInput.lookSpeed, 20.0f, 600.0f, "%.0f");

    ImGui::End();
}

inline void RenderPlayerHUD(const Scene& scene) {
    const ImGuiIO& io = ImGui::GetIO();
    ImDrawList* draw = ImGui::GetForegroundDrawList();

    if (g_showEnemyVisionCones) {
        draw->AddText(ImVec2(16.0f, io.DisplaySize.y - 28.0f),
                      IM_COL32(210, 235, 210, 220),
                      "Enemy vision cones: ON  (N to toggle)");
    }

    // ---- Compass strip -----------------------------------------------------
    //
    // A sliding heading tape across the top: cardinals every 45 degrees, numeric
    // ticks every 15, all scrolling past a fixed centre mark that reads the
    // camera's yaw. Ends fade out so the tape looks continuous rather than
    // clipped, which is what sells it as a strip rather than a list.
    //
    // Camera::Yaw is degrees, 0 along +X and increasing counter-clockwise, while
    // a compass runs clockwise from north. ScreenHeading below converts once so
    // every tick after it is plain compass degrees.
    if (scene.player.health > 0.0f && scene.sniperScopeBlend < 0.25f) {
        constexpr float kStripWidth = 420.0f;
        constexpr float kDegreesAcross = 120.0f;   // span visible end to end
        const float pixelsPerDegree = kStripWidth / kDegreesAcross;
        const float centerX = io.DisplaySize.x * 0.5f;
        const float stripLeft = centerX - kStripWidth * 0.5f;
        const float stripRight = centerX + kStripWidth * 0.5f;
        constexpr float kStripY = 26.0f;

        // Camera yaw -> compass heading. +Z is north here, matching the way
        // VehicleSystem measures its bearings (see PlaceEscapeBoatOnBearing).
        float heading = 90.0f - scene.camera.Yaw;
        heading = std::fmod(heading, 360.0f);
        if (heading < 0.0f) heading += 360.0f;

        // Alpha falls off toward both ends. Everything on the tape multiplies by
        // this, so a tick never pops in at full brightness at the edge.
        const auto edgeFade = [&](float x) {
            const float distance = std::abs(x - centerX) / (kStripWidth * 0.5f);
            const float t = 1.0f - (std::max)(0.0f, (std::min)(1.0f, distance));
            return (std::min)(1.0f, t * 2.6f);   // flat in the middle, fast at ends
        };

        static constexpr const char* kCardinals[8] = {
            "N", "NE", "E", "SE", "S", "SW", "W", "NW" };

        // Walk 15-degree steps across the visible span. Rounded outward by one
        // step on each side so a label sliding in is drawn before its centre
        // reaches the edge.
        const int firstStep =
            static_cast<int>(std::floor((heading - kDegreesAcross * 0.5f) / 15.0f));
        const int lastStep =
            static_cast<int>(std::ceil((heading + kDegreesAcross * 0.5f) / 15.0f));
        for (int step = firstStep; step <= lastStep; ++step) {
            const float tickHeading = static_cast<float>(step) * 15.0f;
            // Shortest signed angle from the current heading, so the tape wraps
            // through 360 without the ticks jumping the width of the strip.
            float delta = tickHeading - heading;
            delta = std::fmod(delta + 540.0f, 360.0f) - 180.0f;
            const float x = centerX + delta * pixelsPerDegree;
            if (x < stripLeft - 20.0f || x > stripRight + 20.0f) continue;

            const float fade = edgeFade(x);
            if (fade <= 0.01f) continue;

            // A cardinal every 45 degrees; plain degree numbers between.
            const int normalized = ((step * 15) % 360 + 360) % 360;
            const bool isCardinal = (normalized % 45) == 0;
            if (isCardinal) {
                const char* label = kCardinals[(normalized / 45) % 8];
                const ImVec2 size = ImGui::CalcTextSize(label);
                // The cardinal nearest the centre is picked out in amber -- the
                // one piece of colour on the tape, so "which way am I facing"
                // resolves without reading any number.
                const bool focused = std::abs(delta) < 22.5f;
                const ImU32 tint = focused
                    ? IM_COL32(255, 186, 62, static_cast<int>(255.0f * fade))
                    : IM_COL32(236, 240, 242, static_cast<int>(235.0f * fade));
                draw->AddText(ImVec2(x - size.x * 0.5f, kStripY), tint, label);
                draw->AddLine(ImVec2(x, kStripY + size.y + 1.0f),
                              ImVec2(x, kStripY + size.y + 6.0f),
                              tint, 1.4f);
            } else {
                char label[8];
                snprintf(label, sizeof(label), "%d", normalized);
                const ImVec2 size = ImGui::CalcTextSize(label);
                const ImU32 tint =
                    IM_COL32(176, 186, 192, static_cast<int>(200.0f * fade));
                draw->AddText(ImVec2(x - size.x * 0.5f, kStripY + 1.0f),
                              tint, label);
                draw->AddLine(ImVec2(x, kStripY + size.y + 2.0f),
                              ImVec2(x, kStripY + size.y + 5.0f), tint, 1.0f);
            }
        }
    }

    // ---- Objective ---------------------------------------------------------
    //
    // Two parts: a task line under the compass saying what to do, and a marker
    // block in the top-left corner naming it as the primary objective. Only
    // drawn on levels that carry a tower, and hidden behind the sniper scope
    // like the rest of the HUD so it cannot sit on top of the reticle.
    {
        float towerHealth = 0.0f, towerMaxHealth = 0.0f;
        if (scene.sniperScopeBlend < 0.25f &&
            CommTowerObjectiveStatus(towerHealth, towerMaxHealth)) {
            const float towerFraction = (std::max)(0.0f, (std::min)(1.0f,
                towerHealth / (std::max)(1.0f, towerMaxHealth)));

            // The task, centred under the compass. An instruction rather than a
            // label: "REACH THE COMMS TOWER" tells a new player what to do,
            // where "OBJECTIVE :: COMM TOWER" only named a thing.
            const char* task = "REACH THE COMMS TOWER";
            const ImVec2 taskSize = ImGui::CalcTextSize(task);
            const float taskY = 62.0f;
            draw->AddText(ImVec2((io.DisplaySize.x - taskSize.x) * 0.5f, taskY),
                          IM_COL32(226, 232, 236, 235), task);

            // Slim progress rule beneath, spanning a fixed width so it reads as
            // a gauge rather than as an underline of the text.
            constexpr float barWidth = 300.0f;
            constexpr float barHeight = 3.0f;
            const ImVec2 barMin((io.DisplaySize.x - barWidth) * 0.5f,
                                taskY + taskSize.y + 5.0f);
            const ImVec2 barMax(barMin.x + barWidth, barMin.y + barHeight);
            draw->AddRectFilled(barMin, barMax, IM_COL32(255, 255, 255, 40), 1.5f);
            if (towerFraction > 0.0f)
                draw->AddRectFilled(barMin,
                    ImVec2(barMin.x + barWidth * towerFraction, barMax.y),
                    IM_COL32(236, 240, 242, 225), 1.5f);

            // Top-left marker. Off-white diamond + "PRIMARY OBJECTIVE" over the
            // task restated in sentence case, matching the reference layout.
            const ImU32 markerColour = IM_COL32(236, 240, 242, 245);
            constexpr float kMarkerX = 30.0f;
            constexpr float kMarkerY = 84.0f;
            const float diamond = 6.0f;
            const ImVec2 centre(kMarkerX + diamond, kMarkerY + 7.0f);
            // Hollow diamond, drawn as a rotated square outline.
            const ImVec2 points[4] = {
                ImVec2(centre.x, centre.y - diamond),
                ImVec2(centre.x + diamond, centre.y),
                ImVec2(centre.x, centre.y + diamond),
                ImVec2(centre.x - diamond, centre.y) };
            draw->AddPolyline(points, 4, markerColour, ImDrawFlags_Closed, 1.6f);
            draw->AddCircleFilled(centre, 1.8f, markerColour, 8);

            draw->AddText(ImVec2(kMarkerX + diamond * 2.0f + 10.0f, kMarkerY),
                          markerColour, "PRIMARY OBJECTIVE");
            draw->AddText(ImVec2(kMarkerX, kMarkerY + 20.0f),
                          IM_COL32(226, 232, 236, 235),
                          "Reach the comms tower");
        }
    }

    // ---- Aircraft objective ------------------------------------------------
    //
    // A world-space tracking marker rather than the tower's fixed corner block:
    // the plane taxis, climbs and falls, so the marker has to follow it. Drawn
    // as a diamond over the airframe with its health directly beneath, and
    // clamped to the screen edge with an arrow while it is off-view -- otherwise
    // the objective simply vanishes the moment the player looks away from it,
    // which is most of the time on a runway approach.
    {
        DirectX::XMFLOAT3 planePosition{};
        float planeHealth = 0.0f, planeMaxHealth = 0.0f;
        bool planeDown = false;
        if (scene.sniperScopeBlend < 0.25f &&
            ObjectivePlaneStatus(planePosition, planeHealth, planeMaxHealth,
                                 planeDown)) {
            const DirectX::XMMATRIX viewProjection =
                scene.GetViewMatrix() * scene.GetProjectionMatrix();
            // Marker sits above the airframe so it does not cover the model.
            DirectX::XMFLOAT3 marked = planePosition;
            marked.y += 6.0f;
            const DirectX::XMVECTOR clip = DirectX::XMVector3Transform(
                DirectX::XMLoadFloat3(&marked), viewProjection);
            const float w = DirectX::XMVectorGetW(clip);
            // Behind the camera: w flips sign, so the projected point mirrors to
            // the wrong side. Negate into a direction that still points the right
            // way for the edge clamp below.
            const bool behind = w <= 0.01f;
            const float safeW = behind ? -w : w;
            float sx = (DirectX::XMVectorGetX(clip) / safeW * 0.5f + 0.5f) *
                       io.DisplaySize.x;
            float sy = (1.0f - (DirectX::XMVectorGetY(clip) / safeW * 0.5f +
                                0.5f)) * io.DisplaySize.y;
            if (behind) {
                sx = io.DisplaySize.x - sx;
                sy = io.DisplaySize.y - sy;
            }

            constexpr float kEdgePad = 46.0f;
            const float clampedX = (std::max)(kEdgePad,
                (std::min)(io.DisplaySize.x - kEdgePad, sx));
            const float clampedY = (std::max)(kEdgePad,
                (std::min)(io.DisplaySize.y - kEdgePad, sy));
            const bool offscreen =
                behind || clampedX != sx || clampedY != sy;
            const ImVec2 at(clampedX, clampedY);

            // Amber while it is a live threat, dimmed once it is down: the
            // marker stays long enough to show the wreck, but stops shouting.
            const ImU32 colour = planeDown
                ? IM_COL32(150, 158, 162, 200)
                : IM_COL32(255, 196, 78, 245);

            if (offscreen) {
                // An arrow pointing off toward it, instead of a diamond sitting
                // on the screen edge pretending to be the aircraft's position.
                const float dx = sx - io.DisplaySize.x * 0.5f;
                const float dy = sy - io.DisplaySize.y * 0.5f;
                const float angle = std::atan2(dy, dx);
                const float ca = std::cos(angle), sa = std::sin(angle);
                constexpr float kArrow = 9.0f;
                const ImVec2 tip(at.x + ca * kArrow, at.y + sa * kArrow);
                const ImVec2 left(at.x + (-ca * 0.6f - sa) * kArrow,
                                  at.y + (-sa * 0.6f + ca) * kArrow);
                const ImVec2 right(at.x + (-ca * 0.6f + sa) * kArrow,
                                   at.y + (-sa * 0.6f - ca) * kArrow);
                draw->AddTriangleFilled(tip, left, right, colour);
            } else {
                constexpr float kDiamond = 7.0f;
                const ImVec2 points[4] = {
                    ImVec2(at.x, at.y - kDiamond),
                    ImVec2(at.x + kDiamond, at.y),
                    ImVec2(at.x, at.y + kDiamond),
                    ImVec2(at.x - kDiamond, at.y) };
                draw->AddPolyline(points, 4, colour, ImDrawFlags_Closed, 1.8f);
            }

            // Health bar under the marker, in the same place whether the marker
            // is a diamond or an edge arrow, so the readout never jumps.
            const float fraction = (std::max)(0.0f, (std::min)(1.0f,
                planeHealth / (std::max)(1.0f, planeMaxHealth)));
            constexpr float kBarWidth = 54.0f;
            constexpr float kBarHeight = 3.0f;
            const ImVec2 barMin(at.x - kBarWidth * 0.5f, at.y + 13.0f);
            const ImVec2 barMax(barMin.x + kBarWidth, barMin.y + kBarHeight);
            draw->AddRectFilled(barMin, barMax, IM_COL32(0, 0, 0, 130), 1.5f);
            if (fraction > 0.0f)
                draw->AddRectFilled(barMin,
                    ImVec2(barMin.x + kBarWidth * fraction, barMax.y),
                    colour, 1.5f);

            const char* label = planeDown ? "AIRCRAFT DOWN" : "AIRCRAFT";
            const ImVec2 labelSize = ImGui::CalcTextSize(label);
            draw->AddText(ImVec2(at.x - labelSize.x * 0.5f, barMax.y + 3.0f),
                          colour, label);
        }
    }

    if (scene.sniperScopeBlend > 0.01f) {
        const float blend = (std::min)(1.0f, scene.sniperScopeBlend);
        const ImVec2 center(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
        const float radius = (std::min)(io.DisplaySize.x, io.DisplaySize.y) * 0.455f;
        const float outerRadius = std::sqrt(
            io.DisplaySize.x * io.DisplaySize.x + io.DisplaySize.y * io.DisplaySize.y);
        const int shadeAlpha = static_cast<int>(255.0f * blend);
        const ImU32 shade = IM_COL32(0, 0, 0, shadeAlpha);
        constexpr int segments = 160;

        // Everything outside the lens is solid black, with no falloff. Looking
        // through a real scope the tube wall is simply opaque -- the eye sees
        // the bright circle of glass and nothing else, so a soft vignette or a
        // grey surround reads as a HUD overlay instead of an optic.
        //
        // Drawn as ONE thick stroked ring rather than a fan of quads from the
        // lens edge out to the corners. Each quad in such a fan carries its own
        // anti-aliased edge, and the shared edges between neighbours do not
        // cancel: they leave 160 faint radial seams seen as streaks seemingly
        // radiating out of the scope. A single polyline has no interior edges.
        const float ringRadius = (radius + outerRadius) * 0.5f;
        const float ringThickness = outerRadius - radius;
        draw->AddCircle(center, ringRadius, shade, segments, ringThickness);

        // Only a slight darkening right at the glass edge, over the last few
        // percent of the radius. Enough to round the transition into the tube
        // without washing the image the way a broad gradient does.
        constexpr int kEdgeBands = 6;
        const float edgeStart = radius * 0.93f;
        for (int band = 0; band < kEdgeBands; ++band) {
            const float t0 = (float)band / kEdgeBands;
            const float t1 = (float)(band + 1) / kEdgeBands;
            const float r0 = edgeStart + (radius - edgeStart) * t0;
            const float r1 = edgeStart + (radius - edgeStart) * t1;
            const int bandAlpha = (int)(170.0f * t1 * t1 * blend);
            if (bandAlpha <= 1) continue;
            for (int i = 0; i < segments; ++i) {
                const float a0 = 6.2831853f * (float)i / segments;
                const float a1 = 6.2831853f * (float)(i + 1) / segments;
                draw->AddQuadFilled(
                    ImVec2(center.x + std::cos(a0) * r0,
                           center.y + std::sin(a0) * r0),
                    ImVec2(center.x + std::cos(a0) * r1,
                           center.y + std::sin(a0) * r1),
                    ImVec2(center.x + std::cos(a1) * r1,
                           center.y + std::sin(a1) * r1),
                    ImVec2(center.x + std::cos(a1) * r0,
                           center.y + std::sin(a1) * r0),
                    IM_COL32(0, 0, 0, bandAlpha));
            }
        }

        // Tube edge: a hard dark rim with a faint lit lip inside it, which is
        // all the housing that is actually visible down the eyepiece.
        draw->AddCircle(center, radius, IM_COL32(6, 7, 8, shadeAlpha),
                        segments, 6.0f);
        draw->AddCircle(center, radius - 3.5f,
                        IM_COL32(120, 126, 130,
                                 static_cast<int>(110.0f * blend)),
                        segments, 1.4f);

        const int lineAlpha = static_cast<int>(225.0f * blend);
        const ImU32 reticleShadow = IM_COL32(0, 0, 0,
                                             static_cast<int>(150.0f * blend));
        const ImU32 reticleLine = IM_COL32(26, 28, 30, lineAlpha);

        // Clipped to the glass so no line runs out over the tube.
        draw->PushClipRect(
            ImVec2(center.x - radius, center.y - radius),
            ImVec2(center.x + radius, center.y + radius), true);

        // Plain crosshair: two thin dark hairlines spanning the full lens,
        // uninterrupted through the middle. Etched glass reads as fine dark
        // lines against the image, not bright ones -- the pale reticle and
        // thick duplex posts this replaces looked like a drawn-on HUD.
        const float span = radius;
        draw->AddLine(ImVec2(center.x - span, center.y + 1.0f),
                      ImVec2(center.x + span, center.y + 1.0f),
                      reticleShadow, 2.2f);
        draw->AddLine(ImVec2(center.x + 1.0f, center.y - span),
                      ImVec2(center.x + 1.0f, center.y + span),
                      reticleShadow, 2.2f);
        draw->AddLine(ImVec2(center.x - span, center.y),
                      ImVec2(center.x + span, center.y), reticleLine, 1.3f);
        draw->AddLine(ImVec2(center.x, center.y - span),
                      ImVec2(center.x, center.y + span), reticleLine, 1.3f);

        // Sparse graduation ticks along both hairlines, short and unlabelled.
        for (int mark = 1; mark <= 6; ++mark) {
            const float offset = radius * 0.13f * mark;
            const float tick = (mark % 2) ? 3.5f : 6.0f;
            for (int sidei = 0; sidei < 2; ++sidei) {
                const float side = sidei ? 1.0f : -1.0f;
                const float x = center.x + side * offset;
                draw->AddLine(ImVec2(x, center.y - tick),
                              ImVec2(x, center.y + tick), reticleLine, 1.2f);
                const float y = center.y + side * offset;
                draw->AddLine(ImVec2(center.x - tick, y),
                              ImVec2(center.x + tick, y), reticleLine, 1.2f);
            }
        }

        draw->PopClipRect();
    }

    if (scene.player.godMode) {
        const char* god = "GOD MODE";
        const ImVec2 size = ImGui::CalcTextSize(god);
        const float godX = (io.DisplaySize.x - size.x) * 0.5f;
        // Clear of the centred stack above it: compass tape at y = 26..45, the
        // objective task line at y = 62 and its progress rule just under. All
        // three are centred, so this has to sit below the lot.
        const float godY = 92.0f;
        draw->AddText(ImVec2(godX, godY), IM_COL32(255, 220, 65, 245), god);
    }

    if (scene.player.health > 0.0f && scene.sniperScopeBlend < 0.25f) {
        const ImVec2 center(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
        // Four independent arms around a fixed centre. The resting gap is the
        // floor; scene.crosshairSpread pushes all four outward together as
        // movement, firing and recoil degrade the shot (see the bloom target in
        // the frame update). A still, crouched player sees the tight cross.
        constexpr float restGap = 3.0f;
        constexpr float arm = 5.0f;
        const float gap = restGap + (std::max)(0.0f, scene.crosshairSpread);
        // Fade the reticle out as the sights come up: the weapon's own sights
        // become the aiming reference, and leaving a crosshair floating over
        // them reads as a double sight picture.
        const float reticleFade = 1.0f - (std::min)(1.0f, scene.adsBlend * 1.35f);
        const int outlineAlpha = static_cast<int>(190.0f * reticleFade);
        const int reticleAlpha = static_cast<int>(220.0f * reticleFade);
        const ImU32 outline = IM_COL32(0, 0, 0, outlineAlpha);
        const ImU32 reticle = IM_COL32(235, 235, 225, reticleAlpha);
        const ImVec2 segments[4][2] = {
            { ImVec2(center.x - gap - arm, center.y), ImVec2(center.x - gap, center.y) },
            { ImVec2(center.x + gap, center.y), ImVec2(center.x + gap + arm, center.y) },
            { ImVec2(center.x, center.y - gap - arm), ImVec2(center.x, center.y - gap) },
            { ImVec2(center.x, center.y + gap), ImVec2(center.x, center.y + gap + arm) }
        };
        for (const auto& segment : segments)
            draw->AddLine(segment[0], segment[1], outline, 3.0f);
        for (const auto& segment : segments)
            draw->AddLine(segment[0], segment[1], reticle, 1.0f);

        // The optic dot replaces the fading hip reticle as the rifle reaches
        // the shoulder. It is projected at the point of aim so parallax from
        // the decorative view-model sway cannot make the sight lie.
        const SGE::ResolvedWeaponStats weaponStats =
            scene.player.ResolveWeaponStats(GunModel::SelectedWeapon());
        if (weaponStats.redDotSight && scene.adsBlend > 0.10f) {
            const float fade = (std::min)(1.0f,
                (scene.adsBlend - 0.10f) / 0.65f);
            const int haloAlpha = static_cast<int>(70.0f * fade);
            const int coreAlpha = static_cast<int>(245.0f * fade);
            draw->AddCircleFilled(center, 3.6f,
                IM_COL32(255, 25, 12, haloAlpha), 16);
            draw->AddCircleFilled(center, 1.25f,
                IM_COL32(255, 52, 28, coreAlpha), 12);
        }
    }

    // Hit marker: four diagonal ticks forming an X over the crosshair. Drawn
    // outside the reticle block above and without its ADS fade -- a confirmed
    // hit has to register while scoped, which is exactly when the crosshair
    // itself is gone.
    if (scene.hitMarkerTime > 0.0f && scene.hitMarkerDuration > 0.0f) {
        const ImVec2 center(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
        // 1 at the moment of the hit, falling to 0. Quadratic so the flash is
        // bright immediately and spends its life fading rather than sitting.
        const float life = (std::min)(1.0f,
            scene.hitMarkerTime / scene.hitMarkerDuration);
        const float fade = life * life;
        // Snaps in tight then relaxes outward as it fades, which reads as an
        // impact rather than a static overlay.
        const float inner = 4.0f + (1.0f - life) * 3.0f;
        const float outer = inner + 5.0f;

        const int markerAlpha = static_cast<int>(255.0f * fade);
        const int shadowAlpha = static_cast<int>(200.0f * fade);
        const ImU32 markerColor = scene.hitMarkerLethal
            ? IM_COL32(255, 70, 60, markerAlpha)      // kill
            : IM_COL32(255, 255, 255, markerAlpha);   // hit
        const ImU32 markerShadow = IM_COL32(0, 0, 0, shadowAlpha);

        // Four diagonals, one per quadrant -- an X, so it never overdraws the
        // crosshair's own horizontal and vertical arms.
        const ImVec2 ticks[4][2] = {
            { ImVec2(center.x - outer, center.y - outer),
              ImVec2(center.x - inner, center.y - inner) },
            { ImVec2(center.x + inner, center.y - inner),
              ImVec2(center.x + outer, center.y - outer) },
            { ImVec2(center.x - outer, center.y + outer),
              ImVec2(center.x - inner, center.y + inner) },
            { ImVec2(center.x + inner, center.y + inner),
              ImVec2(center.x + outer, center.y + outer) }
        };
        for (const auto& tick : ticks)
            draw->AddLine(tick[0], tick[1], markerShadow, 3.6f);
        for (const auto& tick : ticks)
            draw->AddLine(tick[0], tick[1], markerColor, 1.8f);
    }

    // Hit feedback. Drawn before the HUD so the bar and reticle stay readable
    // on top of it.
    //
    // A flat full-screen rectangle was the whole of this: the same wash however
    // hard the hit landed, and no clue where it came from. Three layers now do
    // the work -- an edge vignette that leaves the centre of the screen clear,
    // a directional wedge pointing at whatever hit you, and a low-health pulse
    // that ramps in as you approach death.
    {
        const PlayerState& player = scene.player;
        const float severity = player.damageFlashSeverity;
        const ImVec2 screen = io.DisplaySize;
        const ImVec2 centre(screen.x * 0.5f, screen.y * 0.5f);

        // Vignette: banded rings from the edge inward, strongest at the border
        // and fading to nothing well before the crosshair. Keeping the middle
        // clear is what lets this be much stronger than the old flat wash
        // without blinding the player at the moment they most need to see.
        if (player.damageFlash > 0.0f) {
            const float fade = (std::min)(1.0f, player.damageFlash * 2.4f);
            const float peak = (0.30f + severity * 0.55f) * fade;
            constexpr int kBands = 7;
            // Widest band reaches ~30% of the screen's short side inward.
            const float depth = (std::min)(screen.x, screen.y) * 0.30f;
            for (int band = 0; band < kBands; ++band) {
                const float t = static_cast<float>(band) / (kBands - 1);
                const float inset = depth * t;
                // Quadratic falloff so the ramp is soft near the middle and
                // concentrated at the edge, which reads as a vignette rather
                // than a series of visible rings.
                const float alpha = peak * (1.0f - t) * (1.0f - t);
                if (alpha <= 0.004f) continue;
                draw->AddRect(
                    ImVec2(inset, inset),
                    ImVec2(screen.x - inset, screen.y - inset),
                    ImGui::GetColorU32(ImVec4(0.62f, 0.02f, 0.02f, alpha)),
                    0.0f, 0, depth / kBands * 2.0f);
            }
            // A light centre wash only for hits hard enough to warrant it, so a
            // grenade still whites out the view while a rifle graze does not.
            if (severity > 0.45f) {
                const float wash = (severity - 0.45f) * 0.30f * fade;
                draw->AddRectFilled(
                    ImVec2(0.0f, 0.0f), screen,
                    ImGui::GetColorU32(ImVec4(0.70f, 0.0f, 0.0f, wash)));
            }
        }

        // Directional indicator: a wedge at the screen edge on the bearing the
        // damage came from, so the player can turn to face it. Outlives the
        // flash (see hitIndicator) because a marker that vanishes in a fifth of
        // a second cannot be acted on.
        if (player.hitIndicator > 0.0f &&
            (player.lastHitDirX != 0.0f || player.lastHitDirZ != 0.0f)) {
            // Bearing of the hit relative to where the player is looking.
            const float forwardX = scene.camera.Front.x;
            const float forwardZ = scene.camera.Front.z;
            const float forwardLen =
                std::sqrt(forwardX * forwardX + forwardZ * forwardZ);
            if (forwardLen > 1e-4f) {
                const float fx = forwardX / forwardLen;
                const float fz = forwardZ / forwardLen;
                // Screen-right on the XZ plane.
                //
                // Must match the camera's own handedness, which is not the
                // textbook one: Camera::ProcessKeyboard computes
                // cross(front, up) and then SUBTRACTS it to strafe right, so
                // that cross product points screen-LEFT. Deriving right as
                // (-fz, fx) -- the usual formula -- put every hit marker on the
                // wrong side of the screen.
                const float rx = fz;
                const float rz = -fx;
                const float along = player.lastHitDirX * fx +
                                    player.lastHitDirZ * fz;
                const float lateral = player.lastHitDirX * rx +
                                      player.lastHitDirZ * rz;
                // Screen angle: 0 is straight up (dead ahead), growing
                // clockwise, so a hit from the right puts the wedge on the
                // right.
                const float angle = std::atan2(lateral, along);
                const float radius = (std::min)(screen.x, screen.y) * 0.30f;
                const float alpha =
                    (std::min)(1.0f, player.hitIndicator) * 0.85f;
                const float spread = 0.20f;   // radians, half-width of the wedge
                const auto pointAt = [&](float a, float r) {
                    return ImVec2(centre.x + std::sin(a) * r,
                                  centre.y - std::cos(a) * r);
                };
                const ImU32 colour =
                    ImGui::GetColorU32(ImVec4(0.95f, 0.13f, 0.10f, alpha));
                draw->AddTriangleFilled(
                    pointAt(angle, radius * 1.24f),
                    pointAt(angle - spread, radius * 0.94f),
                    pointAt(angle + spread, radius * 0.94f), colour);
            }
        }

        // Low health: a steady dark-red vignette with a heartbeat throb. Unlike
        // the hit flash this does not fade -- it stays until health recovers,
        // so the state is readable without watching the HP bar.
        if (player.lowHealthPulse > 0.001f) {
            const float beat =
                0.72f + 0.28f * std::sin(static_cast<float>(ImGui::GetTime()) *
                                         (4.2f + player.lowHealthPulse * 2.6f));
            const float peak = player.lowHealthPulse * 0.44f * beat;
            constexpr int kBands = 6;
            const float depth = (std::min)(screen.x, screen.y) * 0.26f;
            for (int band = 0; band < kBands; ++band) {
                const float t = static_cast<float>(band) / (kBands - 1);
                const float inset = depth * t;
                const float alpha = peak * (1.0f - t) * (1.0f - t);
                if (alpha <= 0.004f) continue;
                draw->AddRect(
                    ImVec2(inset, inset),
                    ImVec2(screen.x - inset, screen.y - inset),
                    ImGui::GetColorU32(ImVec4(0.44f, 0.0f, 0.0f, alpha)),
                    0.0f, 0, depth / kBands * 2.0f);
            }
        }
    }

    // ---- Bottom HUD band ---------------------------------------------------
    //
    // Health left, ammo right, both sitting on one baseline with a shared
    // margin. The old bar centred "HP 100 / 100" inside its own fill, which
    // meant the number sat on a colour that changed underneath it and became
    // unreadable around half health. Label and value now live above the bar, on
    // the plate, where nothing moves behind them.
    constexpr float kHudMargin = 28.0f;
    constexpr float kBarWidth = 232.0f;
    constexpr float kBarHeight = 7.0f;
    const float baseline = io.DisplaySize.y - 34.0f;

    const float maxHealth = (std::max)(1.0f, scene.player.maxHealth);
    const float fraction = (std::max)(
        0.0f, (std::min)(1.0f, scene.player.health / maxHealth));
    const float chipValue = scene.healthChip < 0.0f
        ? scene.player.health : scene.healthChip;
    const float chipFraction = (std::max)(fraction, (std::min)(1.0f,
        chipValue / maxHealth));

    const ImVec2 barMin(kHudMargin, baseline);
    const ImVec2 barMax(barMin.x + kBarWidth, barMin.y + kBarHeight);

    // The hudPlate lambda that used to sit here is gone with the panels it
    // backed: this layout puts bright text and bars straight onto the scene, the
    // way the reference does. Contrast now comes from each element's own weight
    // -- the bar's dark track, the oversized figures -- rather than from a wash
    // behind everything.

    // Label row above the bar. "HEALTH" in dim caps, the number in bright
    // right-aligned figures -- a hierarchy the old single centred string had no
    // room to express.
    const ImU32 dimText = IM_COL32(131, 146, 135, 235);
    const ImU32 brightText = IM_COL32(226, 234, 225, 250);

    // Medical cross + value on one row above the bar. The cross is what makes
    // the block readable without a "HEALTH" caption -- an icon carries the
    // meaning in less space and in any language.
    {
        const float crossY = baseline - 26.0f;
        const float armThickness = 4.0f;
        const float armLength = 14.0f;
        const ImVec2 crossCentre(barMin.x + armLength * 0.5f,
                                 crossY + armLength * 0.5f);
        const ImU32 crossTint = fraction <= 0.30f
            ? IM_COL32(255, 92, 76, 255) : IM_COL32(238, 244, 246, 250);
        draw->AddRectFilled(
            ImVec2(crossCentre.x - armLength * 0.5f,
                   crossCentre.y - armThickness * 0.5f),
            ImVec2(crossCentre.x + armLength * 0.5f,
                   crossCentre.y + armThickness * 0.5f), crossTint, 1.0f);
        draw->AddRectFilled(
            ImVec2(crossCentre.x - armThickness * 0.5f,
                   crossCentre.y - armLength * 0.5f),
            ImVec2(crossCentre.x + armThickness * 0.5f,
                   crossCentre.y + armLength * 0.5f), crossTint, 1.0f);

        // The value at 1.5x, left-aligned beside the cross rather than
        // right-aligned over the bar: it is a status figure, not a gauge label.
        char healthValue[32];
        snprintf(healthValue, sizeof(healthValue), "%.0f", scene.player.health);
        constexpr float kHealthFontScale = 1.5f;
        const float healthFontSize = ImGui::GetFontSize() * kHealthFontScale;
        const ImVec2 healthBase = ImGui::CalcTextSize(healthValue);
        draw->AddText(ImGui::GetFont(), healthFontSize,
                      ImVec2(barMin.x + armLength + 12.0f,
                             crossY + armLength * 0.5f -
                                 healthBase.y * kHealthFontScale * 0.5f),
                      crossTint, healthValue);
    }

    // Track, then the draining chip, then the live fill. Order matters: the
    // chip must sit under the fill so the two never disagree at the seam.
    //
    // No backplate on this block: the reference HUD lets the bar and figure sit
    // directly on the scene, and the bar's own dark track already gives the fill
    // something to read against.
    draw->AddRectFilled(ImVec2(barMin.x - 1.0f, barMin.y - 1.0f),
                        ImVec2(barMax.x + 1.0f, barMax.y + 1.0f),
                        IM_COL32(2, 4, 3, 165), 3.0f);
    // The chip stays a muted red -- it is the one thing on the bar that has to
    // read as "damage just taken" rather than "current state", and a white ghost
    // behind a white fill would be invisible.
    if (chipFraction > fraction) {
        draw->AddRectFilled(barMin,
            ImVec2(barMin.x + kBarWidth * chipFraction, barMax.y),
            IM_COL32(198, 86, 72, 150), 2.5f);
    }
    // Plain off-white fill. The old green-to-red gradient carried the same
    // information the bar's own length already does, and it made the HUD read as
    // a status widget rather than an overlay. Danger is signalled by the number
    // going red and by the low-health vignette, both of which are already there.
    if (fraction > 0.0f) {
        draw->AddRectFilled(barMin,
            ImVec2(barMin.x + kBarWidth * fraction, barMax.y),
            IM_COL32(238, 242, 236, 245), 2.5f);
    }

    // Sprint row: a runner glyph and one continuous stamina bar under the
    // health bar.
    //
    // This was three segments, which read as a discrete "how many sprints do I
    // have" gauge; stamina is a continuous budget, so one bar states that
    // directly. It is kept visually distinct from the health bar above it by
    // being shorter and thinner rather than by being split -- two full-width
    // bars of the same weight would read as one control.
    {
        const float sprintY = barMax.y + 7.0f;
        const bool alive = scene.player.health > 0.0f;
        const float staminaFraction = alive
            ? (std::max)(0.0f, (std::min)(1.0f,
                  g_staminaSeconds / (std::max)(0.01f, kStaminaMaxSecondsUI)))
            : 0.0f;
        const bool sprinting = g_playerSprinting && alive;
        const bool spent = g_staminaExhausted && alive;
        const ImU32 litTint = IM_COL32(126, 186, 214, 235);
        // Exhaustion is the one state worth colouring: the player has lost the
        // ability to sprint and the breathing they can hear needs a matching
        // visual, or it reads as an unexplained sound.
        const ImU32 spentTint = IM_COL32(214, 108, 92, 235);
        const ImU32 readyTint = IM_COL32(255, 255, 255, 96);
        const ImU32 dimTint = IM_COL32(255, 255, 255, 38);
        const ImU32 fillTint = spent ? spentTint
                             : (sprinting ? litTint : readyTint);

        // Small runner: head, torso, and two legs mid-stride.
        const float glyphX = barMin.x + 2.0f;
        const ImU32 glyphTint = spent ? spentTint
                              : (sprinting ? litTint : IM_COL32(255, 255, 255, 70));
        draw->AddCircleFilled(ImVec2(glyphX + 3.0f, sprintY + 1.0f), 2.0f,
                              glyphTint, 8);
        draw->AddLine(ImVec2(glyphX + 3.0f, sprintY + 3.5f),
                      ImVec2(glyphX + 2.0f, sprintY + 8.0f), glyphTint, 1.6f);
        draw->AddLine(ImVec2(glyphX + 2.0f, sprintY + 8.0f),
                      ImVec2(glyphX + 6.0f, sprintY + 11.0f), glyphTint, 1.4f);
        draw->AddLine(ImVec2(glyphX + 2.0f, sprintY + 8.0f),
                      ImVec2(glyphX - 2.0f, sprintY + 11.0f), glyphTint, 1.4f);

        // Same left edge and overall width the three segments spanned, so the
        // row keeps its place in the layout and nothing below it moves.
        const float trackLeft = barMin.x + 20.0f;
        const float trackWidth = kBarWidth * 0.62f;
        const ImVec2 trackMin(trackLeft, sprintY + 3.0f);
        const ImVec2 trackMax(trackLeft + trackWidth, sprintY + 6.0f);
        draw->AddRectFilled(trackMin, trackMax, dimTint, 1.5f);
        if (staminaFraction > 0.0f) {
            draw->AddRectFilled(trackMin,
                ImVec2(trackMin.x + trackWidth * staminaFraction, trackMax.y),
                fillTint, 1.5f);
        }
    }

    // ---- Weapon readout ----------------------------------------------------
    //
    // Bottom-right, borderless: weapon name on top, the magazine count as one
    // oversized figure with the reserve beside it behind a thin divider, and a
    // status row underneath carrying fire mode and equipment.
    //
    // No panel box. An earlier pass drew a bordered frame here; the reference
    // layout lets the figures sit directly on the scene, which reads as lighter
    // and stops the corner from looking like a separate application window. The
    // text is bright enough, and short enough, to hold on its own.
    {
        const int slot = GunModel::SelectedWeapon();
        const float right = io.DisplaySize.x - kHudMargin;
        const float rowY = baseline - 26.0f;   // shares the health row's baseline

        // --- Weapon name, above everything, right-aligned ---
        const char* weaponName = GunModel::WeaponName(slot);
        const ImVec2 nameSize = ImGui::CalcTextSize(weaponName);
        draw->AddText(ImVec2(right - nameSize.x, rowY - 18.0f),
                      IM_COL32(226, 234, 238, 240), weaponName);

        if (scene.AmmoEnforced()) {
            const int inMag = scene.player.Magazine(slot);
            const int spare = scene.player.Reserve(slot);
            const int magSize = (std::max)(1, scene.player.MagazineSize(slot));

            ImU32 magTint = IM_COL32(240, 245, 247, 255);
            if (inMag == 0) magTint = IM_COL32(255, 70, 55, 255);
            else if (inMag * 4 <= magSize) magTint = IM_COL32(255, 200, 60, 255);

            // Reserve first, so the magazine figure can be positioned relative
            // to it -- the big number grows leftward as digits are added, and
            // the reserve column has to stay put while it does.
            char spareText[24];
            snprintf(spareText, sizeof(spareText), "%d", spare);
            const ImVec2 spareSize = ImGui::CalcTextSize(spareText);
            const float spareX = right - spareSize.x;

            // Thin divider between magazine and reserve, standing in for the
            // reference's "|" separator without depending on glyph metrics.
            const float dividerX = spareX - 12.0f;

            char magText[16];
            snprintf(magText, sizeof(magText), "%d", inMag);
            constexpr float kMagFontScale = 2.6f;
            const float magFontSize = ImGui::GetFontSize() * kMagFontScale;
            const ImVec2 magBase = ImGui::CalcTextSize(magText);
            const float magWidth = magBase.x * kMagFontScale;
            const float magHeight = magBase.y * kMagFontScale;
            const float magY = rowY - magHeight * 0.32f;
            draw->AddText(ImGui::GetFont(), magFontSize,
                          ImVec2(dividerX - 12.0f - magWidth, magY),
                          magTint, magText);

            draw->AddLine(ImVec2(dividerX, magY + magHeight * 0.30f),
                          ImVec2(dividerX, magY + magHeight * 0.78f),
                          IM_COL32(150, 164, 170, 160), 1.2f);
            draw->AddText(
                ImVec2(spareX, magY + magHeight * 0.44f),
                IM_COL32(160, 172, 178, 235), spareText);

            // Reload sweeps a rule directly under the figures. Kept off the
            // numbers themselves so the counts never disappear mid-reload.
            if (scene.Reloading()) {
                const float reloadTotal = (std::max)(0.01f,
                    scene.player.ReloadTime((std::max)(0,
                        scene.player.reloadingSlot)));
                const float progress = 1.0f - (std::max)(0.0f, (std::min)(1.0f,
                    scene.player.reloadTimer / reloadTotal));
                const float ruleY = magY + magHeight + 1.0f;
                const float ruleLeft = right - 150.0f;
                draw->AddRectFilled(ImVec2(ruleLeft, ruleY),
                                    ImVec2(right, ruleY + 2.0f),
                                    IM_COL32(255, 255, 255, 40), 1.0f);
                draw->AddRectFilled(
                    ImVec2(ruleLeft, ruleY),
                    ImVec2(ruleLeft + (right - ruleLeft) * progress, ruleY + 2.0f),
                    IM_COL32(255, 200, 60, 245), 1.0f);
            }
        }

        // --- Status row: fire mode, then carried equipment ---
        // Right-aligned and built right-to-left, so each entry keeps its place
        // as the ones beside it change width.
        {
            const float statusY = baseline + kBarHeight - 4.0f;
            const ImU32 statusDim = IM_COL32(150, 164, 170, 225);
            const ImU32 statusLit = IM_COL32(226, 234, 238, 245);
            float cursorX = right;

            // C4 is carried on top of the two chosen weapons, so it is shown as
            // an equipment entry rather than a weapon. Lit when it is in hand.
            const bool c4Held = slot == GunModel::kRemoteChargeWeapon;
            const char* c4Label = "C4";
            const ImVec2 c4Size = ImGui::CalcTextSize(c4Label);
            cursorX -= c4Size.x;
            draw->AddText(ImVec2(cursorX, statusY),
                          c4Held ? statusLit : statusDim, c4Label);
            // Marker dot, filled when held.
            cursorX -= 12.0f;
            draw->AddCircleFilled(ImVec2(cursorX + 3.0f, statusY + 7.0f), 2.6f,
                                  c4Held ? statusLit : statusDim, 10);

            // Selected grenade. Grenades are cooldown-gated rather than counted
            // in this engine, so this shows readiness, not a stock number --
            // dim while the throw cooldown is still running.
            const char* grenadeLabel =
                scene.selectedGrenade == GrenadeType::Molotov ? "MOLOTOV" :
                scene.selectedGrenade == GrenadeType::Vortex ? "VORTEX" : "FRAG";
            const bool grenadeReady = scene.grenadeCooldown <= 0.0f;
            const ImVec2 grenadeSize = ImGui::CalcTextSize(grenadeLabel);
            cursorX -= grenadeSize.x + 10.0f;
            draw->AddText(ImVec2(cursorX, statusY),
                          grenadeReady ? statusLit : statusDim, grenadeLabel);
            cursorX -= 12.0f;
            draw->AddCircleFilled(ImVec2(cursorX + 3.0f, statusY + 7.0f), 2.6f,
                                  grenadeReady ? statusLit : statusDim, 10);

            // Fire mode, furthest left. Only the AK is automatic; everything
            // else in the rack fires one round per pull.
            const char* fireMode = (slot == 0 || slot == 6) ? "AUTO" : "SEMI";
            const ImVec2 modeSize = ImGui::CalcTextSize(fireMode);
            cursorX -= modeSize.x + 12.0f;
            draw->AddText(ImVec2(cursorX, statusY), statusDim, fireMode);
        }
    }

    // The centred grenade chip that used to sit here is gone: the weapon panel's
    // equipment row now carries the selected grenade alongside the rest of the
    // loadout, so a separate readout in the middle of the screen was saying the
    // same thing twice and taking the centre of the view to do it.

    if (scene.player.health <= 0.0f) {
        // Banded across the full width rather than floating unbacked: at 1x font
        // scale the bare string was small and easy to miss against a busy scene.
        const char* dead = "YOU DIED";
        const ImVec2 deadSize = ImGui::CalcTextSize(dead);
        const float bandY = io.DisplaySize.y * 0.42f;
        draw->AddRectFilled(ImVec2(0.0f, bandY - 22.0f),
                            ImVec2(io.DisplaySize.x, bandY + deadSize.y + 22.0f),
                            IM_COL32(10, 4, 4, 190));
        draw->AddLine(ImVec2(0.0f, bandY - 22.0f),
                      ImVec2(io.DisplaySize.x, bandY - 22.0f),
                      IM_COL32(196, 44, 32, 190), 1.5f);
        draw->AddLine(ImVec2(0.0f, bandY + deadSize.y + 22.0f),
                      ImVec2(io.DisplaySize.x, bandY + deadSize.y + 22.0f),
                      IM_COL32(196, 44, 32, 190), 1.5f);
        draw->AddText(ImVec2((io.DisplaySize.x - deadSize.x) * 0.5f, bandY),
                      IM_COL32(255, 78, 62, 255), dead);
    }
}

// Flips every RT effect at once, for A/B comparison.
//
// scene.enhancedVisuals alone only stops the enhanced resolve from running; the
// individual toggles keep whatever state they had. This remembers them, clears
// them, and restores them on the way back, so turning RT off and on returns to
// the exact configuration rather than to the defaults.
//
// Lives here rather than inside RenderUI so the checkbox and the keyboard
// shortcut drive the same state -- function-local statics would give the two
// entry points separate saved configurations, and whichever ran second would
// restore the wrong one.
inline bool& AllRTEffectsSavedFlag() {
    static bool saved = false;
    return saved;
}

inline void ToggleAllRTEffects(Scene& scene, VisibilityBufferDX12& vb,
                               bool enable) {
    static bool savedRTShadows = false;
    static bool savedRTReflections = false;
    static bool savedProbeMissGI = false;
    static bool savedReflClassify = false;
    static bool savedRayClassify = false;
    static bool savedSvgfTemporal = false;
    static bool savedSvgfAtrous = false;
    bool& saved = AllRTEffectsSavedFlag();

    if (!enable) {
        savedRTShadows = scene.enhancedRTShadows;
        savedRTReflections = scene.enhancedRTReflections;
        savedProbeMissGI = vb.enhancedProbeMissGIActive;
        savedReflClassify = vb.enhancedReflectionClassifyActive;
        savedRayClassify = scene.enhancedRayClassify;
        savedSvgfTemporal = vb.svgfTemporalEnabled;
        savedSvgfAtrous = vb.svgfAtrousEnabled;
        saved = true;
        scene.enhancedRTShadows = false;
        scene.enhancedRTReflections = false;
        vb.enhancedProbeMissGIActive = false;
        scene.enhancedVisuals = false;
        return;
    }

    scene.enhancedVisuals = true;
    if (saved) {
        scene.enhancedRTShadows = savedRTShadows;
        scene.enhancedRTReflections = savedRTReflections;
        vb.enhancedProbeMissGIActive = savedProbeMissGI;
        vb.enhancedReflectionClassifyActive = savedReflClassify;
        scene.enhancedRayClassify = savedRayClassify;
        vb.svgfTemporalEnabled = savedSvgfTemporal;
        vb.svgfAtrousEnabled = savedSvgfAtrous;
    } else {
        // Turned on before it was ever turned off, so there is no previous
        // configuration to return to. Without this the switch would read "(on)"
        // while every effect stayed off, which looks exactly like RT being
        // broken. Enable the full set, which is what the switch says it does.
        scene.enhancedRTShadows = true;
        scene.enhancedRTReflections = true;
        vb.enhancedProbeMissGIActive = true;
        vb.enhancedReflectionClassifyActive = true;
        scene.enhancedRayClassify = true;
        vb.svgfTemporalEnabled = true;
        vb.svgfAtrousEnabled = true;
    }
}

// Profiler lives in its own window rather than inside Scene Controls. Persists
// for the session so it survives UI rebuilds.
inline bool g_showProfilerWindow = false;

// The CPU/GPU profiler readout. Lives in its own function so it can render
// either inline in Scene Controls or as a standalone window -- comparing pass
// timings while changing settings is hard when both fight for the same panel.
inline void ProfilerPanelBody() {
    // The four numbers you actually read first, before any per-pass detail:
    // how fast the frame is, how long it took wall-clock, and how that splits
    // across the two timelines. Frame ms is ImGui's own delta rather than
    // CPU+GPU: the two overlap by design (the GPU works a frame behind), so
    // adding them would overstate the frame, and neither alone accounts for
    // present or vsync wait. Which of CPU/GPU sits nearest the frame total is
    // what tells you which side is the limiter.
    {
        const ImGuiIO& io = ImGui::GetIO();
        const double cpuMs = g_profiler.CpuFrameMs();
        const double gpuMs = g_profiler.IsInitialized()
            ? g_profiler.GpuFrameMs() : 0.0;

        // Scene Controls already prints FPS and frame ms just above its
        // inline copy of this panel, so those two rows are the standalone
        // window's job only -- otherwise the same numbers appear twice a
        // few lines apart.
        if (g_showProfilerWindow) {
            ImGui::Text("FPS      %6.1f", io.Framerate);
            ImGui::Text("Frame    %6.2f ms",
                        io.Framerate > 0.0f ? 1000.0f / io.Framerate : 0.0f);
        }
        ImGui::Text("CPU      %6.2f ms", cpuMs);
        if (g_profiler.IsInitialized())
            ImGui::Text("GPU      %6.2f ms", gpuMs);
        else
            ImGui::TextDisabled("GPU         -- ms");
    }
    ImGui::Separator();

    // Adaptive Forward Extensions quality. Off by default: while disabled
    // the tier is pinned to Full and nothing about the frame changes.
    {
        bool adaptive = g_forwardQuality.Enabled();
        if (ImGui::Checkbox("Adaptive Forward Quality", &adaptive))
            g_forwardQuality.SetEnabled(adaptive);
        if (adaptive)
            ImGui::Text("  Tier: %s   smoothed FE: %.2f ms   destruction q: %.2f",
                        ForwardQualityController::TierName(g_forwardQuality.Tier()),
                        g_forwardQuality.SmoothedMs(),
                        g_destruction.GetQualityScale());
    }
    // CPU frame total is in the header block above.
    // Palms drive Forward Extensions' pixel cost, so show how many survive
    // the frustum test next to the timings that they move.
    ImGui::Text("Palms drawn: %d / %d",
                g_palmDrawStats.drawn, g_palmDrawStats.considered);
    ImGui::Text("Prefabs drawn: %d / %d",
                g_prefabDrawStats.drawn, g_prefabDrawStats.considered);
    for (const auto& sample : g_profiler.CpuSamples())
        ImGui::BulletText("%s: %.3f ms", sample.name.c_str(), sample.milliseconds);
    ImGui::Separator();
    if (g_profiler.IsInitialized()) {
        // GPU frame total is in the header block above; p95 stays here next
        // to the per-pass breakdown it helps explain.
        ImGui::Text("GPU p95 (%zu/300): %.2f ms",
                    g_profiler.GpuHistorySize(), g_profiler.GpuFrameP95Ms());
        // FE/* scopes nest inside Forward Extensions, so indent them and
        // show their share of it -- their times are already counted in the
        // parent and must not be added to the frame total again.
        float forwardExtensionsMs = 0.0f;
        for (const auto& sample : g_profiler.GpuSamples())
            if (sample.name == "Forward Extensions")
                forwardExtensionsMs = sample.milliseconds;
        float feAccounted = 0.0f;
        for (const auto& sample : g_profiler.GpuSamples()) {
            const bool nested = sample.name.rfind("FE/", 0) == 0 ||
                                sample.name == "Terrain";
            if (!nested) {
                ImGui::BulletText("%s: %.3f ms", sample.name.c_str(),
                                  sample.milliseconds);
                continue;
            }
            feAccounted += sample.milliseconds;
            ImGui::Indent(16.0f);
            if (forwardExtensionsMs > 0.0001f)
                ImGui::BulletText("%s: %.3f ms  (%.0f%%)", sample.name.c_str(),
                                  sample.milliseconds,
                                  100.0f * sample.milliseconds / forwardExtensionsMs);
            else
                ImGui::BulletText("%s: %.3f ms", sample.name.c_str(),
                                  sample.milliseconds);
            ImGui::Unindent(16.0f);
        }
        if (forwardExtensionsMs > 0.0001f) {
            ImGui::Indent(16.0f);
            // What the sub-scopes do not explain: the viewmodel, ropes,
            // helicopters, muzzle/beam effects and per-pass state changes.
            ImGui::TextDisabled("FE/unscoped: %.3f ms",
                                forwardExtensionsMs - feAccounted);
            ImGui::Unindent(16.0f);
        }
        ImGui::TextDisabled("GPU results delayed by frames in flight");
    } else {
        ImGui::TextDisabled("GPU timestamp queries unavailable");
    }
}

// Standalone profiler window. Toggled by g_showProfilerWindow.
inline void DrawProfilerWindow() {
    if (!g_showProfilerWindow) return;
    ImGui::SetNextWindowSize(ImVec2(460.0f, 620.0f), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Profiler", &g_showProfilerWindow))
        ProfilerPanelBody();
    ImGui::End();
}

// -- Settings search --------------------------------------------------------
// The panel has grown past a dozen collapsing sections, so finding one slider
// means remembering which header it lives under. The filter matches section
// titles and, inside a matching section, leaves the contents untouched -- so a
// hit still gives the control with its usual neighbours and context.
inline char g_uiSearch[64] = "";

inline std::string UILowerCopy(const char* text) {
    std::string out(text ? text : "");
    for (char& c : out) c = (char)tolower((unsigned char)c);
    return out;
}

inline bool UISearchActive() { return g_uiSearch[0] != 0; }

inline bool UISearchMatches(const char* label) {
    if (!UISearchActive()) return true;
    return UILowerCopy(label).find(UILowerCopy(g_uiSearch)) != std::string::npos;
}

// Collapsing header that honours the search box: hidden when it does not match,
// and forced open when it does, so a hit is visible without another click.
inline bool UISearchHeader(const char* label,
                           ImGuiTreeNodeFlags flags = 0) {
    if (UISearchActive()) {
        if (!UISearchMatches(label)) return false;
        ImGui::SetNextItemOpen(true, ImGuiCond_Always);
    }
    return ImGui::CollapsingHeader(label, flags);
}

inline void RenderUI(Scene& scene, VisibilityBufferDX12& vb) {
    struct RTDebugSettings {
        bool active = false;
        bool useVisibilityBuffer = true;
        bool useRaytracing = false;
        bool validationMode = false;
        bool enhancedVisuals = false;
        bool enhancedRTShadows = true;
        bool enhancedRayClassify = true;
        bool enhancedRTReflections = false;
        float enhancedConfidenceThreshold = 0.35f;
        float enhancedReflectionRoughnessCut = 0.35f;
        bool svgfTemporalEnabled = false;
        UINT svgfMaxAccumFrames = 32;
        bool svgfAtrousEnabled = false;
        UINT svgfAtrousIterations = 5;
        bool temporalEffectsEnabled = false;
        bool enableFXAA = false;
        float bloomStrength = 0.16f;
        float grainStrength = 0.012f;
        float motionBlurStrength = 0.0f;
        int debugViewMode = 0;
    };
    static RTDebugSettings rtDebug;

    struct RTXSelfTestState {
        bool running = false;
        bool passed = false;
        bool failed = false;
        UINT framesObserved = 0;
        UINT consecutiveRuntimeFrames = 0;
        UINT gpuAtrousMask = 0;
        bool gpuCompositeSeen = false;
    };
    static RTXSelfTestState rtxTest;

    auto saveRTDebugSettings = [&]() {
        rtDebug.useVisibilityBuffer = scene.useVisibilityBuffer;
        rtDebug.useRaytracing = scene.useRaytracing;
        rtDebug.validationMode = vb.validationMode;
        rtDebug.enhancedVisuals = scene.enhancedVisuals;
        rtDebug.enhancedRTShadows = scene.enhancedRTShadows;
        rtDebug.enhancedRayClassify = scene.enhancedRayClassify;
        rtDebug.enhancedRTReflections = scene.enhancedRTReflections;
        rtDebug.enhancedConfidenceThreshold =
            scene.enhancedConfidenceThreshold;
        rtDebug.enhancedReflectionRoughnessCut =
            vb.enhancedReflectionRoughnessCut;
        rtDebug.svgfTemporalEnabled = vb.svgfTemporalEnabled;
        rtDebug.svgfMaxAccumFrames = vb.svgfMaxAccumFrames;
        rtDebug.svgfAtrousEnabled = vb.svgfAtrousEnabled;
        rtDebug.svgfAtrousIterations = vb.svgfAtrousIterations;
        rtDebug.temporalEffectsEnabled = vb.temporalEffectsEnabled;
        rtDebug.enableFXAA = scene.enableFXAA;
        rtDebug.bloomStrength = vb.bloomStrength;
        rtDebug.grainStrength = vb.grainStrength;
        rtDebug.motionBlurStrength = vb.motionBlurStrength;
        rtDebug.debugViewMode = vb.debugViewMode;
    };

    auto applyRTDebugSettings = [&]() {
        scene.useVisibilityBuffer = true;
        scene.useRaytracing = false;
        g_rt.enabled = false;
        vb.validationMode = false;
        scene.enhancedVisuals = true;
        scene.enhancedRTShadows = false;
        scene.enhancedRayClassify = false;
        scene.enhancedRTReflections = true;
        scene.enhancedConfidenceThreshold = 1.0f;
        vb.enhancedReflectionRoughnessCut = 1.0f;
        vb.svgfTemporalEnabled = true;
        vb.svgfMaxAccumFrames = 64;
        vb.svgfAtrousEnabled = true;
        vb.svgfAtrousIterations = VisibilityBufferDX12::kSVGFAtrousMaxIterations;
        vb.temporalEffectsEnabled = false;
        scene.enableFXAA = false;
        vb.bloomStrength = 0.0f;
        vb.grainStrength = 0.0f;
        vb.motionBlurStrength = 0.0f;
    };

    auto restoreRTDebugSettings = [&]() {
        scene.useVisibilityBuffer = rtDebug.useVisibilityBuffer;
        scene.useRaytracing = rtDebug.useRaytracing;
        g_rt.enabled = rtDebug.useRaytracing;
        vb.validationMode = rtDebug.validationMode;
        scene.enhancedVisuals = rtDebug.enhancedVisuals;
        scene.enhancedRTShadows = rtDebug.enhancedRTShadows;
        scene.enhancedRayClassify = rtDebug.enhancedRayClassify;
        scene.enhancedRTReflections = rtDebug.enhancedRTReflections;
        scene.enhancedConfidenceThreshold =
            rtDebug.enhancedConfidenceThreshold;
        vb.enhancedReflectionRoughnessCut =
            rtDebug.enhancedReflectionRoughnessCut;
        vb.svgfTemporalEnabled = rtDebug.svgfTemporalEnabled;
        vb.svgfMaxAccumFrames = rtDebug.svgfMaxAccumFrames;
        vb.svgfAtrousEnabled = rtDebug.svgfAtrousEnabled;
        vb.svgfAtrousIterations = rtDebug.svgfAtrousIterations;
        vb.temporalEffectsEnabled = rtDebug.temporalEffectsEnabled;
        scene.enableFXAA = rtDebug.enableFXAA;
        vb.bloomStrength = rtDebug.bloomStrength;
        vb.grainStrength = rtDebug.grainStrength;
        vb.motionBlurStrength = rtDebug.motionBlurStrength;
        vb.debugViewMode = rtDebug.debugViewMode;
    };

    bool enhancedHeapsReady = true;
    bool atrousHeapsReady = true;
    for (UINT frame = 0; frame < FRAME_COUNT; ++frame) {
        enhancedHeapsReady &= vb.enhancedComputeDescHeaps[frame] != nullptr;
        atrousHeapsReady &= vb.svgfAtrousDescHeaps[frame] != nullptr &&
                           vb.svgfCompositeDescHeaps[frame] != nullptr;
    }
    const bool historyResourcesReady =
        vb.svgfHistoryColor[0] && vb.svgfHistoryColor[1] &&
        vb.svgfHistoryMoments[0] && vb.svgfHistoryMoments[1] &&
        vb.svgfReflectionSrc && vb.stableTriangleDataBuffer &&
        vb.svgfStableSurfaceCurrent && vb.svgfStableSurfaceHistory;
    const bool atrousObjectsReady = vb.svgfAtrousPipelineReady &&
        vb.svgfAtrousPSO && vb.svgfAtrousRootSig &&
        vb.svgfCompositePSO && vb.svgfCompositeRootSig &&
        vb.svgfAtrousConstantBuffer && vb.svgfCompositeConstantBuffer &&
        vb.svgfAtrousScratch[0] && vb.svgfAtrousScratch[1] &&
        atrousHeapsReady;

    if (rtxTest.running) {
        ++rtxTest.framesObserved;
        const bool runtimeFrameGood =
            vb.enhancedResolveExecutedLastFrame &&
            vb.svgfMotionVectorsEnabledLastFrame &&
            vb.svgfTemporalExecutedLastFrame &&
            vb.svgfAtrousExecutedLastFrame &&
            vb.svgfCompositeExecutedLastFrame &&
            vb.svgfAtrousDispatchesLastFrame ==
                VisibilityBufferDX12::kSVGFAtrousMaxIterations;
        rtxTest.consecutiveRuntimeFrames = runtimeFrameGood
            ? rtxTest.consecutiveRuntimeFrames + 1u : 0u;

        for (const auto& sample : g_profiler.GpuSamples()) {
            for (UINT iter = 0;
                 iter < VisibilityBufferDX12::kSVGFAtrousMaxIterations;
                 ++iter) {
                if (sample.name == "SVGF Atrous " + std::to_string(iter))
                    rtxTest.gpuAtrousMask |= 1u << iter;
            }
            if (sample.name == "SVGF Composite")
                rtxTest.gpuCompositeSeen = true;
        }

        const bool staticChecksReady = g_inlineRaytracingSupported &&
            vb.EnhancedVisualsReady() && vb.enhancedTLASAddress != 0 &&
            enhancedHeapsReady && historyResourcesReady && atrousObjectsReady;
        if (staticChecksReady &&
            rtxTest.consecutiveRuntimeFrames >= 64u &&
            rtxTest.gpuAtrousMask == 0x1fu &&
            rtxTest.gpuCompositeSeen) {
            rtxTest.running = false;
            rtxTest.passed = true;
        } else if (rtxTest.framesObserved >= 240u) {
            rtxTest.running = false;
            rtxTest.failed = true;
        }
    }

    RenderMovementPad();

    ImGui::Begin("Scene Controls", nullptr, ImGuiWindowFlags_AlwaysAutoResize);
    // First control in the panel: move the profiler out to its own window so
    // timings stay visible while scrolling this one.
    ImGui::Checkbox("Profiler in separate window", &g_showProfilerWindow);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Show the CPU/GPU profiler as its own panel instead of a "
            "section of this one. Useful when watching a pass timing "
            "while toggling the settings that change it.");
    ImGui::Separator();

    // Frame cost, front and centre: the CPU-side systems here (water waves, grass
    // wind) are easy to scale past what the frame can pay for, and without a
    // number on screen that only shows up as a vague feeling of sluggishness.
    ImGui::Text("%.1f FPS  (%.2f ms)", ImGui::GetIO().Framerate,
                1000.0f / ImGui::GetIO().Framerate);
    const UINT totalDrawCalls = g_forwardDrawCalls + g_shadowDrawCalls +
                                g_visibilityDrawCalls;
    ImGui::Text("Draw calls: %u  (Forward %u, Shadow %u, Visibility %u)",
                totalDrawCalls, g_forwardDrawCalls, g_shadowDrawCalls,
                g_visibilityDrawCalls);
    ImGui::Text("Mesh dispatches: %u  Batches: %u  Instances: %u  Meshlets: %u",
                g_meshShader.dispatchesThisFrame, g_meshShader.batchesThisFrame,
                g_meshShader.instancesThisFrame, g_meshShader.meshletsThisFrame);
    ImGui::Text("Shadow instance batches: %u  Instances: %u",
                g_shadowBatches, g_shadowBatchInstances);
    ImGui::Text("Far shadow cache: %u reused  %u refreshed",
                g_shadowCachedFarCascades, g_shadowRefreshedFarCascades);
    ImGui::Text("Destruction batches: %u  Chunks: %u  Culled: %u",
                g_destructionBatchesThisFrame,
                g_destructionChunksSubmittedThisFrame,
                g_destructionCulledThisFrame);
    // One-shot capture: samples the destruction counters and the FE/Destruction
    // GPU time for 120 frames, then writes deltas to destruction_probe.log.
    // Rebuild counts are cumulative, so the per-frame delta is what says whether
    // merged geometry is being rebuilt continuously or only on real fracture.
    {
        static bool probing = false;
        static int probeFrames = 0;
        static uint64_t lastItemRebuilds = 0;
        static uint64_t lastGeoRebuilds = 0;
        static std::string probeLog;
        if (ImGui::Button("Probe destruction (120 frames)") && !probing) {
            probing = true;
            probeFrames = 0;
            probeLog.clear();
            lastItemRebuilds = g_destruction.GetRenderItemRebuildCount();
            lastGeoRebuilds = g_destruction.GetBatchGeometryRebuildCount();
        }
        if (probing) {
            const uint64_t itemRebuilds = g_destruction.GetRenderItemRebuildCount();
            const uint64_t geoRebuilds = g_destruction.GetBatchGeometryRebuildCount();
            char line[256];
            snprintf(line, sizeof(line),
                     "%3d batches=%u/%u culledBatches=%u zeroRadius=%u "
                     "maxRadius=%.2f chunks=%u culled=%u "
                     "itemRebuild+=%llu geoRebuild+=%llu feDestructionMs=%.3f\n",
                     probeFrames,
                     g_destructionBatchesThisFrame,
                     g_destructionBatchCount,
                     g_destructionCulledBatchesThisFrame,
                     g_destructionZeroRadiusBatches,
                     g_destructionMaxBatchRadius,
                     g_destructionChunksSubmittedThisFrame,
                     g_destructionCulledThisFrame,
                     static_cast<unsigned long long>(itemRebuilds - lastItemRebuilds),
                     static_cast<unsigned long long>(geoRebuilds - lastGeoRebuilds),
                     g_profiler.GpuScopeMs("FE/Destruction"));
            probeLog += line;
            lastItemRebuilds = itemRebuilds;
            lastGeoRebuilds = geoRebuilds;
            if (++probeFrames >= 120) {
                probing = false;
                if (FILE* f = fopen("destruction_probe.log", "w")) {
                    fwrite(probeLog.data(), 1, probeLog.size(), f);
                    fclose(f);
                }
            }
        }
        if (probing) ImGui::SameLine(), ImGui::Text("probing %d/120", probeFrames);
    }
    ImGui::Text("Destruction cache: item rebuilds %llu  geometry rebuilds %llu",
                static_cast<unsigned long long>(
                    g_destruction.GetRenderItemRebuildCount()),
                static_cast<unsigned long long>(
                    g_destruction.GetBatchGeometryRebuildCount()));
    ImGui::Text("Destruction render cache: %zu actor batches  %zu chunk fallbacks  Worker: %s",
                g_destruction.GetRenderBatches().size(),
                g_destruction.GetRenderItems().size(),
                g_destruction.IsBatchBuildPending() ? "building" : "idle");
    // Section search. Sits at the top of the panel so it is the first thing
    // reached, and filters the collapsing sections below by title.
    ImGui::SetNextItemWidth(-90.0f);
    ImGui::InputTextWithHint("##uisearch", "Search settings...",
                             g_uiSearch, IM_ARRAYSIZE(g_uiSearch));
    ImGui::SameLine();
    if (ImGui::Button("Clear##uisearch")) g_uiSearch[0] = 0;
    if (UISearchActive())
        ImGui::TextDisabled("Filtering sections by \"%s\"", g_uiSearch);
    ImGui::Separator();

    ImGui::Checkbox("God Mode", &scene.player.godMode);
    if (ImGui::Button(scene.showRagdollPhysicsShapes
            ? "Hide Ragdoll Physics Shapes"
            : "Show Ragdoll Physics Shapes"))
        scene.showRagdollPhysicsShapes = !scene.showRagdollPhysicsShapes;
    const StaticBufferStatsDX12 staticStats = GetStaticBufferStatsDX12();
    ImGui::Text("GPU-local static buffers: %u  %.1f MiB  Pending: %u",
                staticStats.resources, staticStats.bytes / (1024.0 * 1024.0),
                staticStats.pendingUploads);
    // Rendered here only while the standalone Profiler window is closed, so
    // the readout exists in exactly one place at a time.
    if (!g_showProfilerWindow &&
        ImGui::CollapsingHeader("CPU / GPU Profiler",
                                ImGuiTreeNodeFlags_DefaultOpen)) {
        ProfilerPanelBody();
    }
    BanditDebugText();
    ImGui::Separator();

    ImGui::Checkbox("Show Movement Pad", &virtualInput.showPad);
    ImGui::Separator();

    ImGui::Text("Controls:");
    ImGui::BulletText("TAB: Toggle UI");
    ImGui::BulletText("C: Lock/Unlock Camera");
    ImGui::BulletText("F: Grab / throw enemy");
    ImGui::BulletText("E: Enter / exit Humvee");
    ImGui::BulletText("Humvee: W/S camera-relative drive, Space brake");
    ImGui::BulletText("Humvee mouse: Orbit camera / aim turret");
    ImGui::BulletText("Humvee LMB: Fire turret");
    ImGui::BulletText("V: Toggle FPS Walking Mode");
    ImGui::BulletText("Z: Meshlet Wireframe");
    ImGui::BulletText("Left Click: Lock camera / Shoot");
    ImGui::BulletText("R: Reload (ammo is unlimited in God Mode)");
    ImGui::Separator();

    // -- Camera --
    if (UISearchHeader("Camera Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::DragFloat3("Camera Position", &scene.camera.Position.x, 0.1f);
        ImGui::DragFloat("FOV",  &scene.cameraFOV,  0.5f, 1.0f, 120.0f);
        ImGui::DragFloat("Near", &scene.cameraNear, 0.01f, 0.01f, 10.0f);
        ImGui::DragFloat("Far",  &scene.cameraFar,  1.0f, 10.0f, 2000.0f);
        ImGui::DragFloat("Speed", &scene.camera.MovementSpeed, 0.1f, 0.1f, 50.0f);
        ImGui::Checkbox("FPS Walking Mode", &scene.camera.FPSMode);
    }

    // -- Audio mix --
    // One slider per bus in the submix graph, plus the master and the reverb
    // return. These write straight through to the live voices, so the effect is
    // audible while dragging rather than on the next sound played.
    if (UISearchHeader("Audio Mix")) {
        float master = AudioDevice::MasterVolume();
        if (ImGui::SliderFloat("Master", &master, 0.0f, 1.0f, "%.2f"))
            AudioDevice::SetMasterVolume(master);

        struct BusRow { const char* label; AudioBus bus; };
        static constexpr BusRow kRows[] = {
            { "Weapons",  AudioBus::Weapons },
            { "Voices",   AudioBus::Voices },
            { "Ambience", AudioBus::Ambience },
            { "UI",       AudioBus::UI },
            { "Music",    AudioBus::Music },
        };
        for (const BusRow& row : kRows) {
            float value = AudioDevice::BusVolume(row.bus);
            if (ImGui::SliderFloat(row.label, &value, 0.0f, 1.0f, "%.2f"))
                AudioDevice::SetBusVolume(row.bus, value);
        }

        float reverb = AudioDevice::ReverbVolume();
        if (ImGui::SliderFloat("Reverb Return", &reverb, 0.0f, 1.0f, "%.2f"))
            AudioDevice::SetReverbVolume(reverb);
        if (!AudioDevice::ReverbReady())
            ImGui::TextDisabled("Reverb submix unavailable");
        else
            ImGui::TextDisabled("Wet level per sound comes from X3DAudio distance");
    }

    // -- Light --
    if (UISearchHeader("Light Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::DragFloat3("Light Position", &scene.lightPos.x, 0.1f);
        ImGui::ColorEdit3("Light Color", &scene.lightColor.x);
        ImGui::SliderFloat("Directional Intensity",
                           &scene.directionalLightIntensity,
                           0.0f, 20.0f, "%.2f");
        ImGui::SliderFloat("Base Ambient",
                           &scene.ambientLightingIntensity,
                           0.0f, 2.0f, "%.3f");
        ImGui::Checkbox("Animate Light", &scene.animateLight);
    }

    // -- Cube 1 --
    if (UISearchHeader("Cube 1 Settings")) {
        ImGui::DragFloat3("Position##c1", &scene.cube1.position.x, 0.1f);
        ImGui::DragFloat3("Rotation##c1", &scene.cube1.rotation.x, 1.0f);
        ImGui::DragFloat3("Scale##c1",    &scene.cube1.scale.x,    0.1f, 0.1f, 10.0f);
        ImGui::ColorEdit3("Color##c1",    &scene.cube1.color.x);
        ImGui::Checkbox("Animate##c1",    &scene.animateCube);
    }

    // -- Cube 2 --
    if (UISearchHeader("Cube 2 Settings")) {
        ImGui::Checkbox("Show Second Cube", &scene.cube2.visible);
        if (scene.cube2.visible) {
            ImGui::DragFloat3("Position##c2", &scene.cube2.position.x, 0.1f);
            ImGui::DragFloat3("Rotation##c2", &scene.cube2.rotation.x, 1.0f);
            ImGui::DragFloat3("Scale##c2",    &scene.cube2.scale.x,    0.1f, 0.1f, 10.0f);
            ImGui::ColorEdit3("Color##c2",    &scene.cube2.color.x);
        }
    }

    // -- Rendering --
    if (UISearchHeader("Rendering Settings")) {
        ImGui::ColorEdit3("Floor Color", &scene.floor.color.x);
        ImGui::ColorEdit3("Clear Color", &scene.clearColor.x);
        ImGui::Checkbox("Wireframe Mode", &scene.wireframeMode);
        // Material residency, shown unconditionally rather than inside the
        // bindless sub-panel. Every one of these caps degrades silently -- a
        // full texture array still registers the material and just renders it
        // untextured -- so a scene that quietly exceeds one looks like assets
        // failing to load rather than a budget being hit. Surfacing the numbers
        // here is what makes "the characters went white" diagnosable.
        {
            const UINT legacyTextures = vb.MaterialTextureCount();
            const UINT legacyTextureCap = vb.MaterialTextureCapacity();
            const UINT rejected = vb.RejectedTextureCount();
            const bool texturesFull = legacyTextures >= legacyTextureCap;
            const bool materialsFull =
                vb.MaterialCount() >= VB_MAX_LEGACY_MATERIALS ||
                vb.BindlessMaterialCount() >= VB_MAX_MATERIALS;
            if (texturesFull || materialsFull || rejected > 0) {
                ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f),
                    "  Material budget EXHAUSTED -- surfaces render untextured");
            }
            ImGui::Text("  Materials: legacy %u/%u, bindless %u/%u",
                vb.MaterialCount(), VB_MAX_LEGACY_MATERIALS,
                vb.BindlessMaterialCount(), VB_MAX_MATERIALS);
            ImGui::Text("  Legacy textures: %u/%u  rejected: %u",
                legacyTextures, legacyTextureCap, rejected);
        }
        ImGui::Checkbox("Player Collision Volumes", &g_showCollisionDebug);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Wireframe overlay of the prefab volumes the player collides "
                "against.\n"
                "Amber: bounds box, and it is what segment queries hit.\n"
                "Grey: bounds box superseded by a triangle mesh for those "
                "queries.\n"
                "Green: world bounds of a triangle mesh (its broadphase).");
        ImGui::Checkbox("Mesh Shader Terrain", &scene.useMeshTerrain);
        if (scene.useMeshTerrain) {
            ImGui::SliderFloat("Terrain Height", &scene.terrainHeightScale, 0.0f, 15.0f);
            ImGui::Checkbox("Terrain Detail Relief",
                            &scene.terrainDetailRelief);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Low-frequency octave for broad landforms, high-frequency "
                    "for surface break-up, plus macro normal perturbation that "
                    "holds past the close-range detail fade. "
                    "Changes the heightfield, so collision moves with it.");
            ImGui::Checkbox("Terrain Error LOD", &scene.terrainErrorLOD);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Opt-in projected geometric-error LOD. Keeps visible relief "
                    "and craters dense while coarsening flat ground, and stitches "
                    "fine tile edges to coarser neighbours instead of hiding "
                    "internal cracks with skirts.");
        }
        ImGui::DragFloat("Specular", &scene.specularStrength, 0.01f, 0.0f, 1.0f);

        // VSync. 0 is uncapped; 1+ waits that many vblanks, so 2 is half the
        // refresh rate and 3 a third. The label spells the divisor out because
        // a bare "2" reads like "more vsync" rather than "half framerate".
        {
            const char* vsyncLabel =
                scene.vsyncInterval == 0 ? "Off (uncapped)" :
                scene.vsyncInterval == 1 ? "On (every vblank)" :
                scene.vsyncInterval == 2 ? "On (1/2 refresh)" :
                scene.vsyncInterval == 3 ? "On (1/3 refresh)" :
                                           "On (1/4 refresh)";
            // The main loop mirrors this into g_dx12.syncInterval each frame.
            ImGui::SliderInt("VSync", &scene.vsyncInterval, 0, 4, vsyncLabel);
            if (scene.vsyncInterval == 0 && !g_dx12.tearingSupported)
                ImGui::TextDisabled("  Tearing unsupported; driver may still cap");
        }

        ImGui::Checkbox("Show Helicopter", &scene.showHelicopter);
        ImGui::Checkbox("BlackHawk Ride Marker", &scene.showBlackHawkRideMarker);
        if (scene.showBlackHawkRideMarker) {
            DirectX::XMFLOAT3 local{};
            const DirectX::XMFLOAT3 ride = BlackHawkRideDebugInfo(local);
            const DirectX::XMFLOAT3 mesh = BlackHawkRideMeshPosition();
            const DirectX::XMFLOAT3 centre = BlackHawkModelCentre();
            ImGui::Text("  empty mesh pos: %.3f, %.3f, %.3f",
                        mesh.x, mesh.y, mesh.z);
            ImGui::Text("  model centre/minY: %.3f, %.3f, %.3f  scale %.4f",
                        centre.x, centre.y, centre.z, BlackHawkModelScale());
            ImGui::Text("  local: side %.2f  fwd %.2f  up %.2f",
                        local.x, local.z, local.y);
            ImGui::Text("  world: %.2f, %.2f, %.2f", ride.x, ride.y, ride.z);
        }
        ImGui::Checkbox("Spotlight Fog Shafts", &scene.spotlightVolumetric);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "On: the Humvee spotlight glows in the volumetric fog.\n"
                "Off (default): it lights surfaces only.");
        ImGui::Checkbox("Enable Shadows", &scene.enableShadows);
        if (scene.enableShadows) {
            ImGui::Checkbox("Cache Far Cascades",
                            &scene.cacheFarShadowCascades);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Keeps cascades 2-3 cached while cascade 1 "
                                  "and moving shadow casters remain dynamic.");
            ImGui::DragFloat("Shadow Bias", &scene.shadowBias, 0.0005f, 0.0f, 0.05f, "%.4f");
            ImGui::DragFloat3("Shadow Center", &scene.shadowCenter.x, 0.1f);
            ImGui::DragFloat("Shadow Size", &scene.shadowOrthoSize, 0.5f, 5.0f, 80.0f);
            ImGui::DragFloat("Shadow Distance", &scene.shadowDistance, 0.5f, 5.0f, 120.0f);
            ImGui::DragFloat("Shadow Far", &scene.shadowFarPlane, 0.5f, 10.0f, 200.0f);
        }

        // -- Rendering Pipeline Selection --
        ImGui::Separator();
        ImGui::Text("Rendering Pipeline");

        if (g_inlineRaytracingSupported && vb.EnhancedVisualsReady()) {
            if (ImGui::Checkbox("RT/SVGF Capture Mode", &rtDebug.active)) {
                if (rtDebug.active) {
                    saveRTDebugSettings();
                    applyRTDebugSettings();
                    vb.debugViewMode = 0;
                } else {
                    restoreRTDebugSettings();
                }
                vb.InvalidateTemporalHistory();
                vb.svgfHistoryValid = false;
            }
            if (rtDebug.active) {
                ImGui::TextDisabled(
                    "  Clean capture: RT shadows/TAA/FXAA/post noise off");
                ImGui::TextDisabled(
                    "  Reflections + 64-frame SVGF + 5 a-trous passes on");
                ImGui::TextDisabled(
                    "  Hold still in Lit, then select debug view 5 or 6");
            }
        }

        if (ImGui::CollapsingHeader(
                "RTX / SVGF Self-Test", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::TextDisabled(
                "Recommended map: Custom Levels > RTSVGFTest.json");
            if (ImGui::Button("Run 64-frame RTX Test")) {
                if (!rtDebug.active) {
                    saveRTDebugSettings();
                    rtDebug.active = true;
                }
                applyRTDebugSettings();
                vb.debugViewMode = 0;
                vb.InvalidateTemporalHistory();
                vb.svgfHistoryValid = false;
                rtxTest = {};
                rtxTest.running = true;
            }
            if (rtxTest.running) {
                ImGui::SameLine();
                if (ImGui::Button("Stop Test")) {
                    rtxTest.running = false;
                    rtxTest.failed = true;
                }
            }

            if (rtxTest.running) {
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
                    "RUNNING: %u/64 consecutive frames",
                    (std::min)(rtxTest.consecutiveRuntimeFrames, 64u));
                ImGui::ProgressBar(
                    (std::min)(rtxTest.consecutiveRuntimeFrames / 64.0f, 1.0f),
                    ImVec2(-1.0f, 0.0f));
            } else if (rtxTest.passed) {
                ImGui::TextColored(ImVec4(0.25f, 1.0f, 0.35f, 1.0f),
                    "PASS: enhanced + temporal + 5 a-trous + composite");
            } else if (rtxTest.failed) {
                ImGui::TextColored(ImVec4(1.0f, 0.25f, 0.2f, 1.0f),
                    "FAIL: inspect the first unchecked stage below");
            } else {
                ImGui::TextDisabled(
                    "Applies capture mode and verifies GPU timestamps.");
            }

            auto testLine = [](bool ready, const char* label) {
                ImGui::TextColored(
                    ready ? ImVec4(0.25f, 1.0f, 0.35f, 1.0f)
                          : ImVec4(1.0f, 0.35f, 0.25f, 1.0f),
                    "%s %s", ready ? "[OK]" : "[--]", label);
            };
            testLine(g_inlineRaytracingSupported, "DXR Tier 1.1");
            testLine(vb.EnhancedVisualsReady(), "SM6.5 enhanced resolve PSO");
            testLine(vb.enhancedTLASAddress != 0, "TLAS address registered");
            testLine(enhancedHeapsReady,
                     "Enhanced descriptors for both frame slots");
            testLine(historyResourcesReady,
                     "SVGF history, stable IDs, moments, and reflection resources");
            testLine(vb.persistentAuthoredTriangleCount > 0,
                     "Persistent destruction triangle IDs uploaded");
            testLine(atrousObjectsReady,
                     "A-trous + composite shaders, PSOs, CBs, and heaps");
            testLine(vb.enhancedResolveExecutedLastFrame,
                     "Enhanced resolve recorded last frame");
            testLine(vb.svgfMotionVectorsEnabledLastFrame,
                     "Motion vectors generated for SVGF");
            if (vb.debugViewMode == 5 || vb.debugViewMode == 6) {
                testLine(vb.svgfHistoryValid,
                         "Temporal history valid (paused in debug view)");
            } else {
                testLine(vb.svgfTemporalExecutedLastFrame &&
                             vb.svgfHistoryValid,
                         "Fused temporal history recorded last frame");
            }

            const bool atrousDispatchCountGood =
                vb.svgfAtrousExecutedLastFrame &&
                vb.svgfAtrousDispatchesLastFrame ==
                    VisibilityBufferDX12::kSVGFAtrousMaxIterations;
            ImGui::TextColored(
                atrousDispatchCountGood
                    ? ImVec4(0.25f, 1.0f, 0.35f, 1.0f)
                    : ImVec4(1.0f, 0.35f, 0.25f, 1.0f),
                "%s A-trous recorded last frame: %u/5 dispatches",
                atrousDispatchCountGood ? "[OK]" : "[--]",
                vb.svgfAtrousDispatchesLastFrame);
            testLine(vb.svgfCompositeExecutedLastFrame,
                     "Composite recorded last frame");
            testLine(rtxTest.gpuAtrousMask == 0x1fu,
                     "GPU timestamps returned for A-trous 0,1,2,3,4");
            testLine(rtxTest.gpuCompositeSeen,
                     "GPU timestamp returned for composite");

            if (ImGui::Button("Lit##rtxtest")) vb.debugViewMode = 0;
            ImGui::SameLine();
            if (ImGui::Button("Temporal##rtxtest")) vb.debugViewMode = 5;
            ImGui::SameLine();
            if (ImGui::Button("A-Trous##rtxtest")) {
                vb.svgfAtrousDiagnosticMode = 0;
                vb.debugViewMode = 6;
            }
            const char* atrousDiagnostics[] = {
                "Filtered reflection", "Variance", "Centre-tap share",
                "Normal acceptance", "Depth acceptance",
                "Luminance acceptance", "Temporal history count"
            };
            int atrousDiagnostic =
                static_cast<int>(vb.svgfAtrousDiagnosticMode);
            if (ImGui::Combo("A-Trous Diagnostic", &atrousDiagnostic,
                    atrousDiagnostics, IM_ARRAYSIZE(atrousDiagnostics))) {
                vb.svgfAtrousDiagnosticMode =
                    static_cast<UINT>((std::max)(0, atrousDiagnostic));
                vb.debugViewMode = 6;
            }
            if (vb.svgfAtrousDiagnosticMode == 2)
                ImGui::TextDisabled("  red=center only  green=neighbours accepted");
            else if (vb.svgfAtrousDiagnosticMode >= 3 &&
                     vb.svgfAtrousDiagnosticMode <= 5)
                ImGui::TextDisabled("  black=rejected  white=accepted");
            else if (vb.svgfAtrousDiagnosticMode == 6)
                ImGui::TextDisabled("  red=fresh history  green=64 frames");
            ImGui::TextDisabled(
                "PIX: expand Visibility Buffer > VB Resolve for child markers.");
        }
        
        // Raytracing option
        if (g_rt.supported) {
            if (ImGui::Checkbox("DXR Raytracing", &scene.useRaytracing)) {
                if (scene.useRaytracing) {
                    scene.useVisibilityBuffer = false; // Mutually exclusive
                    g_rt.enabled = true;
                } else {
                    g_rt.enabled = false;
                }
            }
            if (scene.useRaytracing) {
                ImGui::Text("  Primary rays + shadow rays");
                ImGui::Text("  Real-time TLAS rebuild");
            }
        } else {
            ImGui::TextDisabled("DXR Raytracing: Not Supported");
        }

        // VB option
        if (vb.initialized) {
            bool vbEnabled = scene.useVisibilityBuffer;
            if (ImGui::Checkbox("id Tech VB + Deferred (M)", &vbEnabled)) {
                scene.useVisibilityBuffer = vbEnabled;
                if (vbEnabled) {
                    scene.useRaytracing = false;
                    g_rt.enabled = false;
                }
            }
            ImGui::Checkbox("VB/Forward Parity Mode", &vb.validationMode);
            if (vb.validationMode)
                ImGui::TextDisabled("  MSAA / fog / FXAA / TAA / bloom disabled");
            if (vb.bindlessHeap && vb.bindlessHeap->Supported() &&
                g_bindlessMaterialsReady) {
                ImGui::Checkbox("Bindless Materials (SM 6.6)",
                                &scene.bindlessMaterials);
                ImGui::SameLine();
                ImGui::TextDisabled(g_bindlessMaterialsActive
                    ? "(active)" : "(opt-in; inactive on this path)");
                const BindlessDescriptorAllocator& allocator =
                    vb.bindlessHeap->Allocator();
                ImGui::Text("  Persistent: %u/%u  Transient peak: %u/%u",
                    allocator.PersistentCount(), allocator.PersistentCapacity(),
                    allocator.TransientPeak(), allocator.TransientCapacity());
                ImGui::Text("  Cache hits: %llu  Creates: %llu  Overflows: %llu",
                    static_cast<unsigned long long>(allocator.CacheHits()),
                    static_cast<unsigned long long>(allocator.DescriptorCreations()),
                    static_cast<unsigned long long>(allocator.OverflowCount()));
                ImGui::Text("  VB bindless materials: %u/%u",
                    vb.BindlessMaterialCount(), VB_MAX_MATERIALS);
                if (scene.bindlessMaterials && vb.RejectedTextureCount() > 0) {
                    ImGui::TextColored(ImVec4(1.0f, 0.80f, 0.35f, 1.0f),
                        "  Legacy dropped %u maps; bindless now shows authored PBR",
                        vb.RejectedTextureCount());
                }
                if (vb.BindlessTransientOverflowed())
                    ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f),
                        "  Current frame transient descriptor overflow");
            } else {
                scene.bindlessMaterials = false;
                std::string reason = vb.bindlessHeap
                    ? vb.bindlessHeap->Caps().Reason()
                    : "bindless heap was not initialized";
                if (vb.bindlessHeap && vb.bindlessHeap->Supported())
                    reason = "one or more SM 6.6 material PSOs failed";
                ImGui::TextDisabled("Bindless Materials: unavailable (%s)",
                                    reason.c_str());
            }
            if (scene.useVisibilityBuffer) {
                ImGui::Text("  Pass 1: Visibility rasterise");
                ImGui::Text("  Pass 2: G-Buffer fill (compute)");
                ImGui::Text("  Pass 3: Deferred lighting (compute)");
                ImGui::Text("  Instances: %u  Persistent vertices: %u",
                    vb.currentDrawCall, vb.persistentVertexCount);
                ImGui::Text("  Persistent meshes: %u",
                    static_cast<UINT>(vb.meshes.size()));
                // Geometry pool occupancy. Destruction recycles its merged
                // batches through here, so a climbing "used" with no free
                // ranges is what running out looks like -- and running out is
                // silent otherwise: registration just fails and the chunks stop
                // drawing.
                {
                    const float vertexUse = 100.0f *
                        static_cast<float>(vb.persistentVertexCount) /
                        static_cast<float>(VB_MAX_VERTICES);
                    ImGui::Text("  Geometry pool: %u/%u verts (%.1f%%)  "
                        "free %zu  quarantined %zu",
                        vb.persistentVertexCount, VB_MAX_VERTICES, vertexUse,
                        vb.geometryPool.FreeRangeCount(),
                        vb.geometryPool.QuarantinedRangeCount());
                    if (vb.GeometryRegistrationFailures())
                        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.35f, 1.0f),
                            "  Geometry pool FULL: %llu registrations dropped "
                            "(geometry missing from visibility pass)",
                            static_cast<unsigned long long>(
                                vb.GeometryRegistrationFailures()));
                }
                // Texture-array occupancy: the fixed materialTextures[64] array
                // is the wall a bindless heap would remove, so show how close
                // this scene actually is to it rather than guessing.
                {
                    const UINT used = vb.MaterialTextureCount();
                    const UINT capacity = vb.MaterialTextureCapacity();
                    const UINT rejected = vb.RejectedTextureCount();
                    if (rejected > 0) {
                        // Overflow is otherwise invisible: the material still
                        // registers and just renders untextured, which reads as
                        // an authoring mistake rather than a capacity limit.
                        ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.35f, 1.0f),
                            "  Textures: %u/%u FULL - %u dropped (needs %u)",
                            used, capacity, rejected, capacity + rejected);
                    } else if (used * 10u >= capacity * 8u) {
                        ImGui::TextColored(ImVec4(1.0f, 0.80f, 0.35f, 1.0f),
                            "  Textures: %u/%u (near limit)", used, capacity);
                    } else {
                        ImGui::Text("  Textures: %u/%u", used, capacity);
                    }
                    ImGui::Text("  Materials: %u/%u",
                        vb.MaterialCount(), vb.MaterialCapacity());
                }
                const char* debugViews[] = {
                    "Lit resolve", "Instance / primitive IDs", "Raw depth",
                    "Edge mask", "RT reflection rays", "SVGF denoiser",
                    "SVGF a-trous output", "Terrain layer weights"
                };
                ImGui::Combo("VB Debug View", &vb.debugViewMode,
                    debugViews, IM_ARRAYSIZE(debugViews));
                if (vb.debugViewMode == 7) {
                    ImGui::TextDisabled(
                        "  green=grass  orange=dirt  blue=sand  white=rock");
                    ImGui::TextDisabled(
                        "  Terrain pixels only; needs the terrain resolve.");
                }
                if (vb.debugViewMode == 3 && vb.EnhancedVisualsReady())
                    ImGui::Text("  Edge fraction: %.1f%% of sampled pixels",
                                vb.EnhancedRayFraction() * 100.0f);
                if (vb.debugViewMode == 4) {
                    if (!vb.EnhancedVisualsReady()) {
                        ImGui::TextDisabled("  Needs the SM6.5 resolve");
                    } else if (!scene.enhancedRTReflections) {
                        ImGui::TextDisabled("  RT Reflections is off: all black");
                    } else {
                        ImGui::TextDisabled("  green=ray hit  blue=ray missed");
                        ImGui::TextDisabled("  black=not eligible (rough/foliage)");
                        ImGui::TextDisabled("  red ramp=sample index, must cycle");
                    }
                }
                if (vb.debugViewMode == 5) {
                    if (!vb.EnhancedVisualsReady()) {
                        ImGui::TextDisabled("  Needs the SM6.5 resolve");
                    } else if (!scene.enhancedRTReflections) {
                        ImGui::TextDisabled("  RT Reflections is off: all black");
                    } else {
                        ImGui::TextDisabled("  Denoised reflection colour");
                        ImGui::TextDisabled("  black=no hit  coloured=denoised");
                    }
                }
                ImGui::SliderFloat("VB Exposure", &vb.exposure, 0.25f, 4.0f, "%.2f");
                ImGui::SliderFloat("VB Eye Adaptation", &vb.exposureAdaptation, 0.005f, 0.25f, "%.3f");
                ImGui::SliderFloat("VB Bloom", &vb.bloomStrength, 0.0f, 1.0f, "%.2f");
                ImGui::SliderFloat("VB Vignette", &vb.vignetteStrength, 0.0f, 1.0f, "%.2f");
                ImGui::SliderFloat("VB Film Grain", &vb.grainStrength, 0.0f, 0.08f, "%.3f");
            }
            // Ray-traced tier on top of the visibility buffer. Reports why it
            // is unavailable rather than silently doing nothing -- the two
            // requirements (Tier 1.1 hardware, a DXC-compiled SM6.5 resolve)
            // fail independently.
            ImGui::Separator();
            if (!g_inlineRaytracingSupported) {
                ImGui::TextDisabled("Enhanced Visuals: needs DXR Tier 1.1");
            } else if (!vb.EnhancedVisualsReady()) {
                ImGui::TextDisabled("Enhanced Visuals: SM6.5 resolve unavailable");
                ImGui::TextDisabled("  (dxcompiler.dll missing or compile failed)");
            } else {
                bool rtEffectsOn = scene.enhancedVisuals;
                if (ImGui::Checkbox("All RT Effects", &rtEffectsOn)) {
                    // SetEnhancedVisuals already clears svgfHistoryValid when
                    // the active state or the reflection toggle changes, so the
                    // history reset comes for free on the next frame.
                    ToggleAllRTEffects(scene, vb, rtEffectsOn);
                }
                ImGui::SameLine();
                ImGui::TextDisabled(rtEffectsOn ? "(on) [F5]" : "(off) [F5]");
                ImGui::Checkbox("Enhanced Visuals (RT)", &scene.enhancedVisuals);
                if (scene.enhancedVisuals) {
                    if (vb.validationMode) {
                        ImGui::TextDisabled("  Suspended: parity mode active");
                    } else if (!scene.useVisibilityBuffer) {
                        ImGui::TextDisabled("  Needs the VB path");
                    }
                    ImGui::Checkbox("  RT Sun Shadows", &scene.enhancedRTShadows);
                    ImGui::Checkbox("  RT Reflections (noisy)",
                                    &scene.enhancedRTReflections);
                    if (scene.enhancedRTReflections) {
                        ImGui::SliderFloat("  Refl Roughness Cut",
                            &vb.enhancedReflectionRoughnessCut,
                            0.05f, 1.0f, "%.2f");
                        ImGui::TextDisabled(
                            "  1 GGX ray/pixel/frame: noisy by design.");
                    }
                    // RT GI, reflection classification and SVGF are siblings of
                    // RT Reflections, not children of it. They were nested
                    // inside its `if`, which hid and disabled all three
                    // whenever reflections were switched off -- GI in
                    // particular traces its own bounce ray and has nothing to
                    // do with the reflection path.
                    {
                        ImGui::Checkbox("  RT GI (probe misses)",
                                        &vb.enhancedProbeMissGIActive);
                        if (vb.enhancedProbeMissGIActive) {
                            // Quality, not cost: every eligible pixel traces at
                            // any non-zero value. See the shader for why the
                            // ray-budget version was reverted.
                            ImGui::SliderFloat("  RT GI Strength",
                                &vb.enhancedProbeMissGIStrength,
                                0.0f, 1.0f, "%.2f");
                            if (vb.enhancedProbeMissGIStrength <= 0.001f)
                                ImGui::TextDisabled(
                                    "  Rays only where probes miss.");
                            else if (vb.enhancedProbeMissGIStrength >= 0.999f)
                                ImGui::TextDisabled(
                                    "  Full RT GI: every pixel traces.");
                            else
                                ImGui::TextDisabled(
                                    "  Blending probe and traced bounce "
                                    "(same ray cost as 1.00).");
                            // Whole-pass cost, not the GI rays alone: the
                            // resolve is one GPU scope covering all lighting,
                            // so there is no timestamp around the GI trace by
                            // itself. Useful as a before/after when toggling
                            // RT GI off and on -- the delta is the ray cost --
                            // rather than as an absolute GI figure.
                            ImGui::TextDisabled(
                                "  VB Resolve: %.2f ms (whole pass, GI %.1f%% "
                                "of rays)",
                                g_profiler.GpuScopeMs("VB Resolve"),
                                vb.EnhancedGIRayFraction() * 100.0f);
                        }
                        // This one really does gate reflection rays, so it stays
                        // tied to the reflection toggle.
                        if (scene.enhancedRTReflections)
                            ImGui::Checkbox("  Refl Ray Classification",
                                            &vb.enhancedReflectionClassifyActive);
                        if (scene.enhancedRTReflections &&
                            vb.enhancedReflectionClassifyActive) {
                            ImGui::SliderFloat("  Refl Confidence Cut",
                                &vb.enhancedReflectionConfidenceCut,
                                0.05f, 1.0f, "%.2f");
                            ImGui::TextDisabled(
                                "  Probe keeps rough/face-on pixels;");
                            ImGui::TextDisabled(
                                "  rays go to grazing near-mirror ones.");
                        }
                        ImGui::Checkbox("  SVGF Temporal Accumulation",
                                        &vb.svgfTemporalEnabled);
                        if (vb.svgfTemporalEnabled) {
                            ImGui::SliderInt("    Max Accum Frames",
                                (int*)&vb.svgfMaxAccumFrames,
                                4, 64);
                            if (vb.svgfHistoryValid)
                                ImGui::TextDisabled(
                                    "    Reprojects + blends via 1/N EMA");
                            else
                                ImGui::TextDisabled(
                                    "    Waiting for first history frame");
                            ImGui::Checkbox("    SVGF A-Trous Spatial Filter",
                                            &vb.svgfAtrousEnabled);
                            if (vb.svgfAtrousEnabled) {
                                ImGui::SliderInt("      A-Trous Iterations",
                                    (int*)&vb.svgfAtrousIterations, 1, 5);
                                ImGui::TextDisabled(
                                    "      Variance-driven edge stopping.");
                                ImGui::TextDisabled(
                                    "      Lets Max Accum Frames come down.");
                            }
                        }
                    }
                    // Off = the acceleration structure is a load-time snapshot,
                    // so moving actors cast shadows from where they used to be
                    // and appear in no reflection. The toggle is the bisection
                    // tool for telling a stale-TLAS artefact from a shading one.
                    ImGui::Checkbox("  Per-Frame TLAS Refit",
                                    &scene.enhancedTLASRefit);
                    ImGui::TextDisabled(
                        scene.enhancedTLASRefit
                            ? "  Moving actors traced where they are."
                            : "  Static snapshot: movers frozen at load pose.");
                    ImGui::Checkbox("  Ray Classification",
                                    &scene.enhancedRayClassify);
                    if (scene.enhancedRayClassify) {
                        ImGui::SliderFloat("  RT Threshold",
                            &scene.enhancedConfidenceThreshold,
                            0.05f, 1.0f, "%.2f");
                        ImGui::TextDisabled(
                            "  Cheap tier resolves most pixels; rays go");
                        ImGui::TextDisabled(
                            "  only where confidence is below this.");
                    } else {
                        ImGui::TextDisabled("  Tracing every lit pixel (slow)");
                    }
                    // The headline "is classification earning its keep"
                    // number. Sampled from scanlines every 30 frames, so it
                    // settles rather than tracking instantly.
                    ImGui::Text("  Rays: %.1f%% of sampled pixels",
                                scene.enhancedRayFraction * 100.0f);
                    // Split by type: the combined figure saturates once the
                    // shadow gate traces most lit pixels, which hides whether
                    // reflection classification is changing anything.
                    ImGui::TextDisabled(
                        "    shadow %.1f%%  reflection %.1f%%  GI %.1f%%",
                        vb.EnhancedShadowRayFraction() * 100.0f,
                        vb.EnhancedReflectionRayFraction() * 100.0f,
                        vb.EnhancedGIRayFraction() * 100.0f);
                }
            }
            ImGui::Separator();

            if (ImGui::Checkbox("Temporal AA (TAA)",
                                &vb.temporalEffectsEnabled))
                vb.InvalidateTemporalHistory();
            // Exact instance+primitive history validity. The visibility buffer
            // knows which triangle produced each pixel, so temporal reuse can
            // prove correspondence instead of inferring it from depth/normal.
            ImGui::Checkbox("Surface-ID Temporal Validity",
                            &vb.surfaceIDTemporalEnabled);
            if (vb.surfaceIDTemporalEnabled) {
                ImGui::Checkbox("  History Debug View", &vb.historyDebugView);
                if (vb.historyDebugView)
                    ImGui::TextDisabled(
                        "  green=reused  red=rejected  blue=offscreen");
            }
            // Edge AA shades 2 sub-pixel samples on silhouette edges and
            // averages. TAA is off by default, so this is the first AA the VB
            // path has. ~2.5% of pixels (silhouettes only).
            ImGui::Checkbox("Edge AA (N=2)", &vb.edgeAAEnabled);
            ImGui::Checkbox("Extension Motion Vectors",
                            &vb.extensionMotionVectors);
            // Terrain rasterizes IDs into the visibility buffer and is shaded
            // in the resolve instead of the forward pass. On by default; turn
            // it off to fall back to the forward terrain path, which remains
            // the parity reference.
            {
                bool terrainInVB = vb.TerrainVisibilityRequested();
                if (ImGui::Checkbox("Terrain in Visibility Buffer",
                                    &terrainInVB))
                    vb.SetTerrainVisibilityRequested(terrainInVB);
                if (terrainInVB) {
                    // Every resolve tier has a terrain PSO, so this only trips
                    // when that tier's PSO failed to build or terrain has not
                    // published its layer arrays yet.
                    if (!vb.TerrainVisibilityReady())
                        ImGui::TextDisabled(
                            "  unavailable (no terrain PSO for this resolve "
                            "tier)");
                    else if (!g_terrainInVisibilityBuffer)
                        ImGui::TextDisabled("  waiting for terrain draw");
                    else
                        ImGui::TextDisabled("  terrain shaded in VB Resolve");
                }
            }
            // Destruction chunks are always registered into the visibility
            // buffer; this decides whether the forward extensions pass also
            // redraws them. That redraw measured 6.9 ms -- 84% of the pass --
            // so it is on by default.
            {
                bool destructionInVB = vb.DestructionVisibilityRequested();
                if (ImGui::Checkbox("Destruction Chunks in Visibility Buffer",
                                    &destructionInVB))
                    vb.SetDestructionVisibilityRequested(destructionInVB);
                if (destructionInVB) {
                    if (!g_destructionInVisibilityBuffer)
                        ImGui::TextDisabled(
                            "  forward redraw still active (chunks not fully "
                            "registered)");
                    else
                        ImGui::TextDisabled("  chunks shaded in VB Resolve");
                }
                // The direct measurement: in hybrid mode with the toggle on,
                // "forward drawn" should read 0.
                ImGui::TextDisabled("  chunk primitives: %u forward drawn, "
                                    "%u owned by VB",
                                    g_destructionForwardPrimitivesDrawn,
                                    g_destructionVisibilityOwnedPrimitives);
            }
            if (vb.temporalEffectsEnabled) {
                ImGui::SliderFloat("TAA History Weight", &vb.taaFeedback,
                                   0.70f, 0.95f, "%.2f");
                if (!scene.useVisibilityBuffer || vb.validationMode)
                    ImGui::TextDisabled("  TAA inactive outside normal VB mode");
            }
        } else {
            ImGui::TextDisabled("VB Pipeline: Not Available (%s)",
                vb.initError.empty() ? "initialization failed" : vb.initError.c_str());
        }

        ImGui::Checkbox("4x MSAA (Forward only)", &scene.enableMSAA);
        ImGui::Checkbox("4x MSAA Grass (VB)", &scene.enableGrassMSAA);
        if (scene.enableGrassMSAA && !scene.useVisibilityBuffer)
            ImGui::TextDisabled("Grass MSAA active only in Visibility Buffer");
        ImGui::Checkbox("FXAA", &scene.enableFXAA);

        static constexpr const char* kWeatherNames[] = {
            "Clear", "Cloudy", "Dense Fog", "Rain", "Storm", "Custom"
        };
        int weatherIndex = static_cast<int>(scene.weatherState);
        if (ImGui::Combo("Weather State", &weatherIndex, kWeatherNames,
                         IM_ARRAYSIZE(kWeatherNames))) {
            ApplyLiveWeatherState(static_cast<WeatherState>(weatherIndex));
        }
        ImGui::TextDisabled("%s", WeatherStateBriefing(scene.weatherState));

        ImGui::Checkbox("Physical Atmosphere", &scene.enablePhysicalAtmosphere);
        if (scene.enablePhysicalAtmosphere) {
            ImGui::Checkbox("Sky Clouds: 3D Quality",
                            &scene.enableVolumetricClouds);
            if (!scene.enableVolumetricClouds)
                ImGui::TextDisabled("Sky clouds use the legacy 2D fallback");
            ImGui::SliderFloat("Rayleigh", &scene.atmosphereRayleighStrength,
                               0.0f, 2.0f, "%.2f");
            ImGui::SliderFloat("Mie Haze", &scene.atmosphereMieStrength,
                               0.0f, 2.0f, "%.2f");
            ImGui::SliderFloat("Mie Directionality",
                               &scene.atmosphereMieAnisotropy,
                               0.0f, 0.92f, "%.2f");
            ImGui::SliderFloat("Aerial Perspective",
                               &scene.atmosphereAerialDensity,
                               0.0f, 2.0f, "%.2f");
            // Sky clouds and world clouds are separate systems with their own
            // base heights. Group each under its own header so the two bases
            // are visibly distinct rather than reading as one "Cloud Base".
            ImGui::SeparatorText("Sky Clouds (distant, painted on the sky)");
            bool weatherChanged = ImGui::SliderFloat(
                "Sky Cloud Coverage", &scene.atmosphereCloudCoverage,
                0.0f, 1.0f, "%.2f");
            weatherChanged |= ImGui::SliderFloat(
                "Sky Cloud Density", &scene.atmosphereCloudDensity,
                0.0f, 1.5f, "%.2f");
            weatherChanged |= ImGui::DragFloat(
                "Sky Cloud Base", &scene.atmosphereCloudBaseHeight,
                10.0f, 50.0f, 5000.0f, "%.0f m");
            weatherChanged |= ImGui::DragFloat(
                "Sky Cloud Thickness", &scene.atmosphereCloudThickness,
                10.0f, 50.0f, 5000.0f, "%.0f m");
            if (scene.enableFlyableClouds)
                ImGui::TextDisabled(
                    "Suppressed while World Volumetric Clouds is on");
            if (weatherChanged)
                ApplyLiveWeatherState(WeatherState::Custom);
        }
        ImGui::SeparatorText("World Clouds (volumetric, fly-through)");
        if (ImGui::Checkbox("World Volumetric Clouds",
                            &scene.enableFlyableClouds))
            ApplyLiveWeatherState(WeatherState::Custom);
        if (scene.enableFlyableClouds) {
            ImGui::TextDisabled("Replaces sky clouds; depth-occluded and fly-through");
            bool weatherChanged = ImGui::DragFloat(
                "World Cloud Base", &scene.flyableCloudBaseHeight,
                1.0f, 0.0f, 0.0f, "%.1f m");
            weatherChanged |= ImGui::DragFloat(
                "World Cloud Thickness", &scene.flyableCloudThickness,
                1.0f, 0.0f, 0.0f, "%.1f m");
            weatherChanged |= ImGui::DragFloat(
                "World Cloud Density", &scene.flyableCloudDensity,
                0.01f, 0.0f, 0.0f, "%.3f");
            weatherChanged |= ImGui::DragFloat(
                "World Cloud Coverage", &scene.flyableCloudCoverage,
                0.01f, 0.0f, 0.0f, "%.3f");
            if (weatherChanged)
                ApplyLiveWeatherState(WeatherState::Custom);
            const float cloudTop = scene.flyableCloudBaseHeight +
                (std::max)(scene.flyableCloudThickness, 1.0f);
            if (scene.camera.Position.y >= scene.flyableCloudBaseHeight &&
                scene.camera.Position.y <= cloudTop)
                ImGui::TextDisabled("Camera is inside the cloud volume");
        }
        // The low haze that hugs the horizon is height fog, not a cloud layer,
        // so neither cloud base moves it. Its controls live in the fog section
        // further down; mirror the two that shape the horizon band here so all
        // three layer bases can be dialled from one place.
        ImGui::SeparatorText("Ground Haze (height fog, hugs the horizon)");
        {
            bool hazeChanged = ImGui::DragFloat(
                "Haze Base Height", &scene.volumetricFogBaseHeight,
                0.5f, -5.0f, 2000.0f, "%.1f m");
            hazeChanged |= ImGui::SliderFloat(
                "Haze Height Falloff", &scene.volumetricFogHeightFalloff,
                0.001f, 0.25f, "%.3f");
            hazeChanged |= ImGui::DragFloat(
                "Haze Density", &scene.volumetricFogDensity,
                0.0005f, 0.0001f, 0.05f, "%.4f");
            if (!scene.enableVolumetricFog)
                ImGui::TextDisabled("Volumetric Fog is off");
            if (hazeChanged)
                ApplyLiveWeatherState(WeatherState::Custom);
        }
        ImGui::Checkbox("GTAO + Contact Shadows", &scene.enableAmbientOcclusion);
        if (scene.enableAmbientOcclusion) {
            ImGui::Checkbox("  Optimized AO Shader",
                            &scene.optimizedAmbientOcclusion);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "A/B the GTAO/contact-shadow arithmetic optimizations.\n"
                    "Off compiles the original pre-optimization shader\n"
                    "(bit-identical bytecode); on uses hoisted loop\n"
                    "invariants, multiply chains for pow(), and rsqrt\n"
                    "distance math. Both should look the same -- watch the\n"
                    "'GTAO + Contact Shadows' GPU timer above for the cost.");
            ImGui::TextDisabled(
                scene.optimizedAmbientOcclusion
                    ? "  Optimized shader variant"
                    : "  Original (pre-optimization) shader variant");
            ImGui::Checkbox("  Half-Resolution AO Trace",
                            &scene.halfResolutionAO);
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(
                    "Trace AO at half resolution (quarter the pixels) and\n"
                    "upsample in the full-res bilateral composite.\n"
                    "This is the real win: the trace is bound by scattered\n"
                    "depth fetches, so quartering them cuts the pass cost.\n"
                    "Ignored while temporal bent-normal GTAO is on, which\n"
                    "keeps a full-resolution history.");
            if (scene.halfResolutionAO && scene.temporalBentNormalGTAO)
                ImGui::TextDisabled(
                    "  Half-res ignored: temporal bent normals are on");
            ImGui::DragFloat("AO Radius", &scene.ambientOcclusionRadius,
                             0.01f, 0.01f, 4.0f, "%.2f m");
            ImGui::SliderFloat("AO Strength", &scene.ambientOcclusionStrength,
                               0.0f, 5.0f, "%.2f");
            ImGui::SliderFloat("Contact Shadows", &scene.contactShadowStrength,
                               0.0f, 1.0f, "%.2f");
            ImGui::Checkbox("  Linear-Depth Occluder Test",
                            &scene.contactShadowLinearDepth);
            ImGui::TextDisabled(
                scene.contactShadowLinearDepth
                    ? "  Screen-capped AO range; refined crossings."
                    : "  Device-depth slab: doubles on flat ground.");
            ImGui::Checkbox("  Temporal Bent-Normal GTAO",
                            &scene.temporalBentNormalGTAO);
            if (scene.temporalBentNormalGTAO) {
                if (!scene.useVisibilityBuffer)
                    ImGui::TextDisabled(
                        "  Requires Visibility Buffer motion vectors.");
                else if (vb.BentNormalGTAOAppliedLastResolve())
                    ImGui::TextDisabled(
                        "  Active: bent sky/GI and accumulated diffuse AO.");
                else
                    ImGui::TextDisabled(
                        "  Warming up temporal history (one frame).");
                const char* bentDebugModes[] = {
                    "Lit", "Bent Direction", "AO Visibility",
                    "Temporal Confidence"
                };
                int bentDebugMode = static_cast<int>(
                    vb.bentNormalGTAODebugMode);
                if (ImGui::Combo("  Bent GTAO Diagnostic", &bentDebugMode,
                        bentDebugModes, IM_ARRAYSIZE(bentDebugModes))) {
                    bentDebugMode = (std::max)(0, (std::min)(3,
                        bentDebugMode));
                    vb.bentNormalGTAODebugMode =
                        static_cast<VisibilityBufferDX12::
                            BentNormalGTAODebugMode>(bentDebugMode);
                }
                if (bentDebugMode == 1)
                    ImGui::TextDisabled("  RGB = world-space bent direction.");
                else if (bentDebugMode == 2)
                    ImGui::TextDisabled("  White = open; black = occluded.");
                else if (bentDebugMode == 3)
                    ImGui::TextDisabled("  Green = accepted; red = rejected.");
            }
            ImGui::Checkbox("  Grass Depth in GTAO + Contact",
                            &scene.grassInScreenSpaceAO);
            if (scene.grassInScreenSpaceAO &&
                (!scene.enableGrassMSAA || !scene.useVisibilityBuffer))
                ImGui::TextDisabled(
                    "  Requires 4x MSAA Grass and Visibility Buffer.");
        }
        ImGui::Checkbox("Screen-Space Reflections",
                        &scene.enableScreenSpaceReflections);
        if (scene.enableScreenSpaceReflections) {
            ImGui::SliderFloat("SSR Strength",
                               &scene.screenSpaceReflectionStrength,
                               0.0f, 1.5f, "%.2f");
            ImGui::DragFloat("SSR Distance",
                             &scene.screenSpaceReflectionDistance,
                             0.5f, 5.0f, 150.0f, "%.1f m");
            ImGui::DragFloat("SSR Thickness",
                             &scene.screenSpaceReflectionThickness,
                             0.005f, 0.01f, 0.5f, "%.3f m");
        }
        const char* waterQualityNames[] = { "Low", "High", "Ultra" };
        int waterQuality = static_cast<int>(scene.waterQuality);
        if (ImGui::Combo("Tropical Water Quality", &waterQuality,
                         waterQualityNames, _countof(waterQualityNames)))
            scene.waterQuality = static_cast<WaterQuality>(waterQuality);
        if (scene.waterQuality == WaterQuality::High) {
            ImGui::TextDisabled(
                "High: analytic shoreward swell with swash and shoaling surf");
            if (ImGui::TreeNodeEx("High Wave Controls",
                                  ImGuiTreeNodeFlags_DefaultOpen)) {
                // Multipliers over the authored spectrum, so the relative shape
                // of swell against chop survives every slider position.
                ImGui::SliderFloat("Wave Height",
                    &scene.highWaterWaveHeight, 0.0f, 3.0f, "%.2fx");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "Scales every wave's amplitude. Steepness is clamped\n"
                        "automatically so tall waves sharpen rather than\n"
                        "folding through themselves.");
                ImGui::SliderFloat("Wave Scale",
                    &scene.highWaterWaveScale, 0.35f, 2.5f, "%.2fx");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "Stretches wavelengths. Longer waves also travel\n"
                        "faster, because deep-water speed follows sqrt(g/k).");
                ImGui::SliderFloat("Wave Speed",
                    &scene.highWaterWaveSpeed, 0.0f, 2.5f, "%.2fx");
                ImGui::SliderFloat("Choppiness",
                    &scene.highWaterChoppiness, 0.0f, 2.0f, "%.2f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "Gerstner steepness: how far crests pinch toward\n"
                        "peaks. Clamped against the wave height above.");
                ImGui::SliderFloat("Micro Detail",
                    &scene.highWaterMicroDetail, 0.0f, 3.0f, "%.2fx");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "Per-pixel capillary ripples. Fades with distance on\n"
                        "its own so raising this cannot alias the horizon.");
                ImGui::SliderFloat("Foam Strength",
                    &scene.highWaterFoamStrength, 0.0f, 2.5f, "%.2f");
                ImGui::SliderFloat("Shore Refraction",
                    &scene.highWaterShoreRefraction, 0.0f, 1.0f, "%.2f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "How far waves turn to face the coast, over the same\n"
                        "signed-distance bathymetry Ultra refracts against.\n"
                        "The open ocean keeps its offshore swell direction:\n"
                        "each train only starts bending once the depth drops\n"
                        "below about half its own wavelength, so long swell\n"
                        "turns first and short chop stays deep-water until it\n"
                        "is nearly ashore. Carries the wavefront irregularity\n"
                        "too, so the bent crests arrive ragged rather than as\n"
                        "concentric arcs.\n\n"
                        "Visual only: CPU buoyancy cannot sample the GPU\n"
                        "bathymetry, so in the shoaling band floating objects\n"
                        "sit on the unrefracted surface. Keep it low where a\n"
                        "boat has to look right, or use Ultra, which reads its\n"
                        "heights back from the GPU.");
                ImGui::SliderFloat("Shore Flatten",
                    &scene.highWaterShoreFlatten, 0.0f, 1.0f, "%.2f");
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "How the waves shoal as the bed rises, on the same\n"
                        "per-train depth ramp: wavelengths compress, crests\n"
                        "steepen and gain height, then cap at the 0.39 x depth\n"
                        "breaker limit so swell cannot run into the beach at\n"
                        "full deep-water height.\n\n"
                        "Safe to leave on. Unlike refraction this only lowers\n"
                        "the surface, and only in shallows. It is also what\n"
                        "stops the shallows showing hard polygonal facets --\n"
                        "neighbouring crests folding through each other where\n"
                        "the water is too shallow to hold them, which makes\n"
                        "the clipmap triangles visible.");
                if (ImGui::Button("Reset High Waves")) {
                    scene.highWaterWaveHeight = 1.0f;
                    scene.highWaterWaveScale = 1.0f;
                    scene.highWaterWaveSpeed = 1.0f;
                    scene.highWaterChoppiness = 1.0f;
                    scene.highWaterMicroDetail = 1.0f;
                    scene.highWaterFoamStrength = 1.0f;
                    scene.highWaterShoreRefraction = 0.0f;
                    scene.highWaterShoreFlatten = 0.30f;
                }
                ImGui::TreePop();
            }
        }
        if (scene.waterQuality == WaterQuality::Ultra) {
            ImGui::TextDisabled(
                "Ultra: spectral swell, terrain-aware surf and underwater medium");
            if (ImGui::TreeNodeEx("Ultra Wave Controls",
                                  ImGuiTreeNodeFlags_DefaultOpen)) {
                ImGui::SliderFloat("Wave Height",
                    &scene.ultraWaterWaveHeight, 0.0f, 3.0f, "%.2fx");
                ImGui::SliderFloat("Wave Scale",
                    &scene.ultraWaterWaveScale, 0.35f, 2.5f, "%.2fx");
                ImGui::SliderFloat("Wave Speed",
                    &scene.ultraWaterWaveSpeed, 0.0f, 2.5f, "%.2fx");
                ImGui::SliderAngle("Wave Direction",
                    &scene.ultraWaterDirection, -180.0f, 180.0f);
                ImGui::SliderFloat("Choppiness",
                    &scene.ultraWaterChoppiness, 0.0f, 2.0f, "%.2f");
                ImGui::SliderFloat("Surf Strength",
                    &scene.ultraWaterSurfStrength, 0.0f, 2.5f, "%.2f");
                ImGui::SliderFloat("Foam Strength",
                    &scene.ultraWaterFoamStrength, 0.0f, 2.5f, "%.2f");
                ImGui::SliderFloat("Coast Damping",
                    &scene.ultraWaterCoastDamping, 0.25f, 3.0f, "%.2f");
                if (ImGui::Button("Reset Ultra Waves")) {
                    scene.ultraWaterWaveHeight = 1.0f;
                    scene.ultraWaterWaveScale = 1.0f;
                    scene.ultraWaterWaveSpeed = 1.0f;
                    scene.ultraWaterDirection = 0.0f;
                    scene.ultraWaterChoppiness = 1.0f;
                    scene.ultraWaterSurfStrength = 1.0f;
                    scene.ultraWaterFoamStrength = 1.0f;
                    scene.ultraWaterCoastDamping = 1.0f;
                    scene.ultraWaterRefreshRequested = true;
                }
                ImGui::SameLine();
                if (ImGui::Button("Refresh Ultra Water"))
                    scene.ultraWaterRefreshRequested = true;
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "Reload terrain bathymetry and restart surf/foam state");
                ImGui::TreePop();
            }
        }
        const char* shaftModes[] = { "Volumetric", "Faux (screen-space)", "Off" };
        int shaftMode = static_cast<int>(scene.lightShaftMode);
        if (ImGui::Combo("Light Shafts", &shaftMode, shaftModes, 3))
            scene.lightShaftMode =
                static_cast<Scene::LightShaftMode>(shaftMode);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Volumetric: marches a froxel grid. Occludes against any\n"
                "geometry and works with the sun off screen, but the grid is\n"
                "coarse, so brightness can bleed across a silhouette.\n\n"
                "Faux: radial blur from the sun with a per-tap depth test.\n"
                "No grid, so no bleeding or blockiness, but the sun must be\n"
                "on screen.");
        if (scene.lightShaftMode == Scene::LightShaftMode::Faux) {
            ImGui::SliderFloat("Shaft Intensity", &scene.lightShaftIntensity,
                               0.0f, 3.0f, "%.2f");
            ImGui::SliderFloat("Shaft Length", &scene.lightShaftDensity,
                               0.1f, 1.5f, "%.2f");
            ImGui::SliderFloat("Shaft Decay", &scene.lightShaftDecay,
                               0.80f, 1.0f, "%.3f");
            ImGui::SliderFloat("Shaft Exposure", &scene.lightShaftExposure,
                               0.0f, 1.0f, "%.2f");
        }
        if (ImGui::SliderFloat("Rain", &scene.rainIntensity,
                               0.0f, 1.0f, "%.2f"))
            ApplyLiveWeatherState(WeatherState::Custom);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Rainfall, 0 clear to 1 downpour. Scales the number of drops\n"
                "rather than fading them, so light rain is sparse instead of\n"
                "transparent. Costs nothing at 0.");
        if (scene.rainIntensity > 0.0f &&
            ImGui::DragFloat2("Wind", &scene.windVelocity.x,
                              0.05f, -8.0f, 8.0f, "%.2f m/s"))
            ApplyLiveWeatherState(WeatherState::Custom);
        if (ImGui::Checkbox("Volumetric Fog", &scene.enableVolumetricFog))
            ApplyLiveWeatherState(WeatherState::Custom);
        if (scene.enableVolumetricFog) {
            if (scene.lightShaftMode == Scene::LightShaftMode::Volumetric) {
                ImGui::Checkbox("High-Res Light Shafts",
                                &scene.volumetricFogHighRes);
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip(
                        "Doubles the fog grid to 128x72x96 (8x froxels).\n"
                        "Sharpens sun shafts through foliage at a GPU cost.");
            }
            bool weatherChanged = ImGui::DragFloat(
                "Fog Density", &scene.volumetricFogDensity,
                0.0005f, 0.0001f, 0.05f, "%.4f");
            weatherChanged |= ImGui::SliderFloat(
                "Fog Anisotropy", &scene.volumetricFogAnisotropy,
                0.0f, 0.9f, "%.2f");
            weatherChanged |= ImGui::ColorEdit3(
                "Fog Tint", &scene.volumetricFogTint.x);
            weatherChanged |= ImGui::SliderFloat(
                "Fog Height Falloff", &scene.volumetricFogHeightFalloff,
                0.001f, 0.25f, "%.3f");
            // Ranges match the mirrored haze sliders in the cloud section, so
            // the same value is reachable from either place.
            weatherChanged |= ImGui::DragFloat(
                "Fog Base Height", &scene.volumetricFogBaseHeight,
                0.5f, -5.0f, 2000.0f, "%.1f m");
            weatherChanged |= ImGui::DragFloat(
                "Fog Distance", &scene.volumetricFogDistance,
                5.0f, 20.0f, scene.cameraFar, "%.0f m");
            if (weatherChanged)
                ApplyLiveWeatherState(WeatherState::Custom);
        }
        if (scene.enableMSAA &&
            (scene.useVisibilityBuffer || scene.useRaytracing)) {
            ImGui::TextDisabled("MSAA inactive outside Forward renderer");
        }
        if (!scene.useVisibilityBuffer && !scene.useRaytracing) {
            ImGui::Text("Active: Forward Clustered");
        }

        // -- DDGI --
        ImGui::Separator();
        const bool sparseDXR = g_dxrDDGIProbeCount > 0;
        ImGui::Text(sparseDXR ? "DXR Sparse DDGI" : "Legacy Grid DDGI");
        if (ImGui::Checkbox("Enable DDGI", &scene.useDDGI))
            RequestLiveDXRDDGIRebuild();
        if (scene.useDDGI) {
            if (sparseDXR) {
                scene.giIntensity =
                    std::clamp(scene.giIntensity, 0.0f, 2.0f);
                scene.normalBias =
                    std::clamp(scene.normalBias, 0.02f, 0.30f);
            }
            ImGui::DragFloat("GI Intensity", &scene.giIntensity,
                             0.05f, 0.0f, sparseDXR ? 2.0f : 5.0f);
            ImGui::DragFloat("Normal Bias", &scene.normalBias,
                             0.005f, sparseDXR ? 0.02f : 0.0f,
                             sparseDXR ? 0.30f : 1.0f);
            if (sparseDXR) {
                ImGui::Text("Probes: %u  Hash cells: %u",
                            g_dxrDDGIProbeCount, g_dxrDDGICellCount);
                ImGui::Text("Cell size: %.2f m", g_dxrDDGICellSize);
                ImGui::DragFloat("Ray Distance", &scene.giMaxDistance,
                                 0.5f, 1.0f, 200.0f, "%.1f m");
                ImGui::DragFloat("Probe Spacing", &scene.probeSpacing,
                                 0.1f, 0.25f, 50.0f, "%.2f m");
                if (ImGui::IsItemDeactivatedAfterEdit())
                    RequestLiveDXRDDGIRebuild();
                ImGui::TextDisabled("Spacing rebuilds layout when released");
            } else {
                ImGui::DragFloat("Probe Spacing", &scene.probeSpacing,
                                 0.1f, 0.5f, 10.0f);
            }
            ImGui::Checkbox("Show Probes",   &scene.showProbes);
        }

        // -- Clustered Lights --
        ImGui::Separator();
        ImGui::Text("Clustered Lights");
        ImGui::Text("Active Clusters: %d", scene.clusteredRenderer.getActiveClusterCount());
        ImGui::Text("Total Lights: %d",    scene.clusteredRenderer.getLightCount());
        static int prevNum = scene.numDemoLights;
        ImGui::SliderInt("Demo Lights", &scene.numDemoLights, 0, 64);
        ImGui::DragFloat("Light Radius", &scene.demoLightRadius, 0.5f, 1.0f, 50.0f);
        ImGui::Checkbox("Animate Lights", &scene.animateDemoLights);
        if (scene.numDemoLights != prevNum) {
            scene.RebuildDemoLights();
            prevNum = scene.numDemoLights;
        }
    }

    // -- Gun --
    if (UISearchHeader("Viewmodel (Gun)")) {
        ImGui::Checkbox("Show Gun",    &scene.gun.visible);
        ImGui::ColorEdit3("Gun Color", &scene.gun.color.x);
        ImGui::DragFloat3("Offset",    &scene.gun.offset.x,   0.01f);
        ImGui::DragFloat3("Scale##gun",&scene.gun.scale.x,     0.01f);
        ImGui::DragFloat3("Rot##gun",  &scene.gun.rotation.x,  1.0f);
        ImGui::Text("Fit: %s", GunModel::SelectedWeaponName());
        ImGui::DragFloat3("Weapon Fit Offset",
                          &GunModel::PlayerOffset().x, 0.005f);
        // Degrees, and per weapon like the offset above it: an imported mesh
        // whose authored forward axis differs from the engine's needs turning
        // before it sits in the hands, not just sliding.
        ImGui::DragFloat3("Weapon Fit Rot",
                          &GunModel::PlayerFitRotation().x, 1.0f);

        // Attachment placement. These are gun-local like the fit offsets above,
        // not screen-relative: they walk the sight along the receiver rail, and
        // are per weapon because each host mesh carries its rail at a different
        // height. Shown only when an optic is actually fitted -- there is
        // nothing on screen to move otherwise.
        const SGE::ResolvedWeaponStats viewWeaponStats =
            scene.player.ResolveWeaponStats(GunModel::SelectedWeapon());
        if (viewWeaponStats.redDotSight) {
            ImGui::SeparatorText("Red Dot Sight");
            ImGui::SliderFloat("Sight X", &GunModel::PlayerOpticOffset().x,
                               -0.30f, 0.30f, "%.3f");
            ImGui::SliderFloat("Sight Y", &GunModel::PlayerOpticOffset().y,
                               -0.20f, 0.50f, "%.3f");
            ImGui::SliderFloat("Sight Z", &GunModel::PlayerOpticOffset().z,
                               -1.00f, 1.00f, "%.3f");
            ImGui::SliderFloat("Sight Scale", &GunModel::PlayerOpticScale(),
                               0.10f, 4.00f, "%.2f");
            ImGui::DragFloat3("Sight Rot", &GunModel::PlayerOpticRotation().x,
                              1.0f, -180.0f, 180.0f, "%.1f deg");
            // The emissive dot is placed apart from the sight body, so moving
            // the mount leaves it behind until it is re-centred in the glass.
            ImGui::SliderFloat3("Reticle Pos",
                                &GunModel::PlayerReticleOffset().x,
                                -1.00f, 1.00f, "%.3f");
            // The tuned dot sits near the bottom of this range, so the floor is
            // well below it -- a slider pinned at its own minimum cannot be
            // taken any finer.
            ImGui::SliderFloat("Reticle Size", &GunModel::ReticleSize(),
                               0.0002f, 0.05f, "%.4f");
            if (ImGui::Button("Reset Sight##optic")) {
                GunModel::PlayerOpticOffset() =
                    DirectX::XMFLOAT3(0.012f, 0.140f, 0.376f);
                GunModel::PlayerOpticRotation() =
                    DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f);
                GunModel::PlayerOpticScale() = 1.42f;
                GunModel::PlayerReticleOffset() =
                    DirectX::XMFLOAT3(0.0f, 0.182f, -0.600f);
                GunModel::ReticleSize() = 0.001f;
            }
            ImGui::SameLine();
            // The dot only lines up with the crosshair while aiming, so the
            // alignment these sliders exist to fix is only visible in ADS.
            ImGui::TextDisabled("hold RMB to check alignment");
        }

        ImGui::Checkbox("Auto Fire", &scene.autoFire);
        ImGui::DragFloat("Fire Interval", &scene.fireInterval, 0.005f, 0.02f, 1.0f, "%.3f s");

        // Sighted weapon position. Hold right mouse while dragging these to see
        // the alignment update live -- X is the one that decides whether the
        // sights sit on the crosshair or off to one side.
        ImGui::SeparatorText("Aim Down Sights");
        ImGui::DragFloat("ADS X", &scene.adsOffsetX, 0.001f, -0.30f, 0.30f, "%.3f");
        ImGui::DragFloat("ADS Y", &scene.adsOffsetY, 0.001f, -0.40f, 0.20f, "%.3f");
        ImGui::DragFloat("ADS Z", &scene.adsOffsetZ, 0.005f,  0.05f, 1.00f, "%.3f");
        ImGui::DragFloat("ADS FOV", &scene.adsFOV, 0.5f, 15.0f, 60.0f, "%.1f deg");
        ImGui::Text("blend %.2f%s", scene.adsBlend,
                    scene.adsActive ? "  (aiming)" : "");

        // The arms ride the weapon's transform, so these offsets are relative to
        // the gun, not the camera: they position the hands ON the weapon.
        ImGui::SeparatorText("Arms");
        if (ArmsModel::Loaded()) {
            ImGui::Checkbox("Show Arms", &ArmsModel::Visible());
            ImGui::SameLine();
            ImGui::Checkbox("Hide Head", &ArmsModel::HideHead());
            ImGui::SameLine();
            ImGui::Checkbox("Hide Free Hand", &ArmsModel::HideFreeHand());
            // Mirrors the body so the rifle is held in the other hand. Re-solve
            // the alignment after toggling: the grip hand moves across.
            if (ImGui::Checkbox("Mirror", &ArmsModel::MirrorX()))
                ArmsModel::RealignHandsToWeapon();
            ImGui::SameLine();
            // Which wrist the gun is aligned to. Mirroring swaps which side of
            // the screen each rig-named hand appears on, so this needs to flip
            // with it.
            if (ImGui::Checkbox("Grip: Left Bone", &ArmsModel::GripUsesLeftHand()))
                ArmsModel::RebindGripBone();
            // Free nudge on top of the solved placement. Not re-solved on drag:
            // this is the manual override for when the solved position is close
            // but wants a touch of adjustment.
            ImGui::DragFloat3("Arms Offset", &ArmsModel::Offset().x, 0.005f);
            // Yaw/pitch/roll in degrees, applied in the model's own space. This
            // is what turns the arms to face down the barrel. Rotation and scale
            // both feed the transform the alignment measures through, so both
            // re-solve on change -- otherwise the hand slides off the weapon as
            // the body turns under it.
            if (ImGui::DragFloat3("Arms Rot", &ArmsModel::Rotation().x, 1.0f))
                ArmsModel::RealignHandsToWeapon();
            if (ImGui::DragFloat("Arms Scale", &ArmsModel::Scale(), 0.01f, 0.2f,
                                 3.0f, "%.2f"))
                ArmsModel::RealignHandsToWeapon();
            if (ImGui::Button("Reset Rotation")) {
                ArmsModel::Rotation() = DirectX::XMFLOAT3(0.0f, 0.0f, 0.0f);
                ArmsModel::RealignHandsToWeapon();
            }
            // The idle is held on a single frame by default so the pose can be
            // aligned against the fixed weapon; scrub PoseTime to pick which
            // moment of the clip to align to, then enable Animate.
            ImGui::Checkbox("Animate", &ArmsModel::Animate());
            ImGui::SameLine();
            ImGui::Text("IDLE + RUN %.0f%%  t=%.2fs",
                ArmsModel::RunBlendWeight() * 100.0f,
                ArmsModel::RunAnimation().time);
            ImGui::DragFloat("Full Run Speed",
                &ArmsModel::RunSpeedThreshold(), 0.1f, 0.0f, 30.0f,
                "%.1f m/s");
            ImGui::SliderFloat("Run Loop Blend",
                &ArmsModel::RunAnimation().loopBlendDuration,
                0.0f, 0.5f, "%.2f s");
            ImGui::SliderFloat("Run Back Offset",
                &ArmsModel::RunBackOffset(), 0.0f, 1.5f, "%.2f");
            if (!ArmsModel::Animate()) {
                const float duration = ArmsModel::Animation().clip
                    ? ArmsModel::Animation().clip->duration : 1.0f;
                ImGui::SliderFloat("Pose Time", &ArmsModel::PoseTime(), 0.0f,
                                   duration, "%.2f s");
            }
            // The point on the weapon the aligned hand is pinned to. Dragging
            // this re-solves immediately, so the hand can be walked along the
            // rifle until it sits on the handguard properly.
            ImGui::SeparatorText("Weapon Grip Point");
            if (ImGui::DragFloat3("Grip Target", &ArmsModel::WeaponGrip().x, 0.005f))
                ArmsModel::RealignHandsToWeapon();
            if (ImGui::Button("Snap Hands To Weapon"))
                ArmsModel::RealignHandsToWeapon();

            // With the idle playing the hand both moves and turns, so the weapon
            // tracks its full motion rather than hanging at a fixed spot.
            ImGui::SeparatorText("Weapon Follows Hand");
            ImGui::Checkbox("Follow Grip Hand", &ArmsModel::WeaponFollowsHand());
            if (ArmsModel::WeaponFollowsHand()) {
                // Fixed corrections on top of what the hand contributes.
                ImGui::DragFloat3("Follow Offset", &ArmsModel::FollowOffset().x, 0.005f);
                ImGui::DragFloat3("Weapon Rot Fix", &ArmsModel::FollowRotation().x, 1.0f);
            }
        } else {
            ImGui::TextDisabled("arms model not loaded");
        }
    }

    // -- Grass / wind --
    if (g_grass.IsInitialized() && UISearchHeader("Grass & Wind")) {
        ImGui::SeparatorText("Material");
        bool foliageMaterialChanged =
            ImGui::ColorEdit3("Grass Albedo", &g_grass.Albedo().x);
        foliageMaterialChanged |= ImGui::SliderFloat(
            "Grass Roughness", &g_grass.Roughness(), 0.04f, 1.0f);
        foliageMaterialChanged |= ImGui::SliderFloat(
            "Grass Ambient", &g_grass.AmbientScale(), 0.0f, 2.0f);
        foliageMaterialChanged |= ImGui::SliderFloat(
            "Grass Direct Light", &g_grass.DirectLightScale(), 0.0f, 2.0f);
        foliageMaterialChanged |= ImGui::SliderFloat(
            "Grass Transmission", &g_grass.TransmissionStrength(), 0.0f, 1.0f);
        foliageMaterialChanged |= ImGui::SliderFloat(
            "Grass Color Variation", &g_grass.ColorVariation(), 0.0f, 1.5f);
        foliageMaterialChanged |= ImGui::SliderFloat(
            "Grass Normal Falloff", &g_grass.NormalFalloff(), 0.0f, 1.0f);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "How strongly a blade's facing darkens it.\n"
                "1 = physical: blades turned away from the sun fall to\n"
                "ambient, which splits the field into a bright and a dark\n"
                "half. 0 = every blade lit as if it faced the sun, which\n"
                "removes the split but is not physical.\n"
                "Shadows and sky ambient still vary at any setting.");
        if (ImGui::Button("Reset Grass Material")) {
            g_grass.ResetMaterial();
            foliageMaterialChanged = true;
        }
        if (foliageMaterialChanged) MatchFoliageMaterialToGrass();

        ImGui::SeparatorText("Wind & Performance");
        ImGui::DragFloat("Wind Strength", &g_grass.WindStrength(), 0.01f, 0.0f, 1.0f);
        ImGui::DragFloat("Wind Speed",    &g_grass.WindSpeed(),    0.05f, 0.0f, 6.0f);
        ImGui::SeparatorText("Foliage Interaction");
        ImGui::Checkbox("Player Grass Interaction", &g_grass.PlayerPushEnabled());
        ImGui::BeginDisabled(!g_grass.PlayerPushEnabled());
        ImGui::DragFloat("Push Strength", &g_grass.PlayerPushStrength(),
                         0.02f, 0.0f, 1.5f, "%.2f");
        ImGui::DragFloat("Push Radius", &g_grass.PlayerPushRadius(),
                         0.05f, 0.25f, 3.0f, "%.2f m");
        ImGui::EndDisabled();
        // Perf dials: density trims blades per cell (whole tufts, no rebuild);
        // distance shrinks the drawn ring. ~0.6 / 22 is a good perf preset.
        ImGui::SliderFloat("Density", &g_grass.Density(), 0.05f, 1.0f);
        ImGui::DragFloat("Draw Distance", &g_grass.DrawDistance(), 0.5f, 8.0f, 80.0f);
    }

    if (UISearchHeader("Destruction", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Enable Destructible Wall", &scene.useDestruction);
        ImGui::DragFloat("Damage Radius", &scene.destructionDamageRadius, 0.1f, 0.25f, 8.0f);
        ImGui::DragFloat("Damage", &scene.destructionDamage, 0.1f, 0.1f, 10.0f);
        ImGui::DragFloat("Bullet Impulse", &scene.destructionBulletImpulse, 5.0f, 0.0f, 1000.0f);
        ImGui::Text("Wall: %u chunks  %u actors", g_destruction.GetChunkCount(), g_destruction.GetActorCount());
        ImGui::Text("Debris LOD: %u world-only  %u frozen/merged",
                    g_destruction.GetCollisionLodActorCount(),
                    g_destruction.GetFrozenActorCount());
        ImGui::Checkbox("Blast Debug Draw", &scene.showDestructionDebug);
        if (ImGui::Button("Rebuild Wall")) scene.rebuildDestructionRequested = true;
        ImGui::SameLine();
        if (ImGui::Button("Collapse Stress Benchmark"))
            g_destruction.StartCollapseStressBenchmark();
        const DestructionStressStats stress = g_destruction.GetStressStats();
        if (stress.running || stress.sampledFrames > 0) {
            ImGui::Text("Stress: %s  %.1fs  %u frames",
                        stress.running ? "running" : "complete",
                        stress.elapsedSeconds, stress.sampledFrames);
            ImGui::Text("Trigger %.2f ms  Update avg/peak %.2f / %.2f ms",
                        stress.triggerMilliseconds,
                        stress.averageUpdateMilliseconds,
                        stress.peakUpdateMilliseconds);
            ImGui::Text("Frame avg/peak %.2f / %.2f ms",
                        stress.averageFrameMilliseconds,
                        stress.peakFrameMilliseconds);
            ImGui::Text("Physics peak %.2f ms  Render rebuild peak %.2f ms",
                        stress.peakPhysicsMilliseconds,
                        stress.peakRenderRebuildMilliseconds);
            ImGui::Text("Peak actors/awake %u / %u  Rebuilds %llu  Tiny %u",
                        stress.peakActors, stress.peakAwakeActors,
                        static_cast<unsigned long long>(stress.renderRebuilds),
                        stress.tinyParticles);
        }
    }

    if (UISearchHeader("Palm Trees", ImGuiTreeNodeFlags_DefaultOpen)) {
        // Absolute damage vs a section's 30 health: 15 => 2 hits to sever.
        ImGui::DragFloat("Tree Damage/Shot", &scene.treeDamagePerShot, 0.5f, 1.0f, 60.0f);
        ImGui::Text("Shoot a trunk to fell the tree above the hit.");
    }

    ImGui::Separator();
    ImGui::TextColored(scene.player.health > 30.0f ? ImVec4(0.3f, 1.0f, 0.35f, 1.0f)
                                                   : ImVec4(1.0f, 0.2f, 0.12f, 1.0f),
                       "Health: %.0f / %.0f",
                       scene.player.health, scene.player.maxHealth);
    ImGui::SameLine();
    if (ImGui::SmallButton("Restore")) scene.RestorePlayerHealth();
    ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
    
    const char* renderer = "Forward Clustered";
    // Keep capture-critical settings locked while still allowing the debug-view
    // selector to move from Lit to the temporal and a-trous inspection views.
    if (rtDebug.active)
        applyRTDebugSettings();
    if (scene.useRaytracing) renderer = "DXR Raytracing";
    else if (scene.useVisibilityBuffer) renderer = "id Tech VB+Deferred";
    ImGui::Text("Renderer: DirectX 12 (%s)", renderer);

    ImGui::End();

    // Outside Scene Controls' Begin/End so it is a real sibling window.
    DrawProfilerWindow();
}

#endif // ENGINE_UI_H
