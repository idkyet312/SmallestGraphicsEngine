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
#include <algorithm>
#include <cmath>
#include <cstdio>

// Forward declare raytracing context
struct RaytracingContext;
extern RaytracingContext g_rt;
extern ProfilerDX12 g_profiler;
extern UINT g_forwardDrawCalls;
extern UINT g_shadowDrawCalls;
extern UINT g_visibilityDrawCalls;
extern UINT g_shadowBatches;
extern UINT g_shadowBatchInstances;
extern UINT g_destructionBatchesThisFrame;
extern UINT g_destructionChunksSubmittedThisFrame;
extern UINT g_destructionCulledThisFrame;
extern UINT g_dxrDDGIProbeCount;
extern UINT g_dxrDDGICellCount;
extern float g_dxrDDGICellSize;
extern MeshShaderDX12 g_meshShader;

// Skinned Bandit enemy (defined in main.cpp) -- surfaced for debug readout.
// BanditDebugText renders a one-line status; defined in main.cpp where the
// SkinnedEnemy type is complete.
void BanditDebugText();
void RequestLiveDXRDDGIRebuild();

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

    if (scene.sniperScopeBlend > 0.01f) {
        const float blend = (std::min)(1.0f, scene.sniperScopeBlend);
        const ImVec2 center(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
        const float radius = (std::min)(io.DisplaySize.x, io.DisplaySize.y) * 0.455f;
        const float outerRadius = std::sqrt(
            io.DisplaySize.x * io.DisplaySize.x + io.DisplaySize.y * io.DisplaySize.y);
        const int shadeAlpha = static_cast<int>(255.0f * blend);
        const ImU32 shade = IM_COL32(0, 0, 0, shadeAlpha);
        constexpr int segments = 128;
        for (int i = 0; i < segments; ++i) {
            const float a0 = 6.2831853f * static_cast<float>(i) / segments;
            const float a1 = 6.2831853f * static_cast<float>(i + 1) / segments;
            const ImVec2 inner0(center.x + std::cos(a0) * radius,
                                center.y + std::sin(a0) * radius);
            const ImVec2 inner1(center.x + std::cos(a1) * radius,
                                center.y + std::sin(a1) * radius);
            const ImVec2 outer0(center.x + std::cos(a0) * outerRadius,
                                center.y + std::sin(a0) * outerRadius);
            const ImVec2 outer1(center.x + std::cos(a1) * outerRadius,
                                center.y + std::sin(a1) * outerRadius);
            draw->AddQuadFilled(inner0, outer0, outer1, inner1, shade);
        }

        const int lineAlpha = static_cast<int>(235.0f * blend);
        const ImU32 reticleShadow = IM_COL32(0, 0, 0, lineAlpha);
        const ImU32 reticleGlow = IM_COL32(145, 255, 170, lineAlpha);
        draw->AddCircle(center, radius, IM_COL32(12, 18, 13, shadeAlpha),
                        segments, 7.0f);
        draw->AddCircle(center, radius - 5.0f,
                        IM_COL32(165, 190, 170, lineAlpha), segments, 1.2f);

        const float edge = radius - 10.0f;
        const float gap = 4.0f;
        const ImVec2 horizontal[2][2] = {
            { ImVec2(center.x - edge, center.y), ImVec2(center.x - gap, center.y) },
            { ImVec2(center.x + gap, center.y), ImVec2(center.x + edge, center.y) }
        };
        const ImVec2 vertical[2][2] = {
            { ImVec2(center.x, center.y - edge), ImVec2(center.x, center.y - gap) },
            { ImVec2(center.x, center.y + gap), ImVec2(center.x, center.y + edge) }
        };
        for (const auto& line : horizontal) {
            draw->AddLine(line[0], line[1], reticleShadow, 3.0f);
            draw->AddLine(line[0], line[1], reticleGlow, 1.0f);
        }
        for (const auto& line : vertical) {
            draw->AddLine(line[0], line[1], reticleShadow, 3.0f);
            draw->AddLine(line[0], line[1], reticleGlow, 1.0f);
        }
        draw->AddCircleFilled(center, 2.2f, reticleShadow, 16);
        draw->AddCircleFilled(center, 1.0f, reticleGlow, 12);

        for (int mark = 1; mark <= 5; ++mark) {
            const float offset = radius * 0.105f * mark;
            const float halfWidth = mark % 2 ? 5.0f : 9.0f;
            draw->AddLine(ImVec2(center.x - halfWidth, center.y + offset),
                          ImVec2(center.x + halfWidth, center.y + offset),
                          reticleShadow, 3.0f);
            draw->AddLine(ImVec2(center.x - halfWidth, center.y + offset),
                          ImVec2(center.x + halfWidth, center.y + offset),
                          reticleGlow, 1.0f);
        }
        for (int mark = 1; mark <= 4; ++mark) {
            const float offset = radius * 0.13f * mark;
            const float halfHeight = mark % 2 ? 4.0f : 7.0f;
            for (float side : { -1.0f, 1.0f }) {
                const float x = center.x + side * offset;
                draw->AddLine(ImVec2(x, center.y - halfHeight),
                              ImVec2(x, center.y + halfHeight), reticleShadow, 3.0f);
                draw->AddLine(ImVec2(x, center.y - halfHeight),
                              ImVec2(x, center.y + halfHeight), reticleGlow, 1.0f);
            }
        }

        const char* zoomLabel = "SVD  4x";
        const ImVec2 labelSize = ImGui::CalcTextSize(zoomLabel);
        draw->AddText(ImVec2(center.x + radius * 0.46f - labelSize.x * 0.5f,
                             center.y + radius * 0.74f),
                      IM_COL32(145, 255, 170, lineAlpha), zoomLabel);
    }

    if (scene.player.godMode) {
        const char* god = "GOD MODE";
        const ImVec2 size = ImGui::CalcTextSize(god);
        draw->AddText(ImVec2((io.DisplaySize.x - size.x) * 0.5f, 24.0f),
                      IM_COL32(255, 220, 65, 245), god);
    }

    if (scene.player.health > 0.0f && scene.sniperScopeBlend < 0.25f) {
        const ImVec2 center(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
        constexpr float gap = 3.0f;
        constexpr float arm = 5.0f;
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
    }

    // Brief red hit flash. Draw first so HUD stays readable above it.
    if (scene.player.damageFlash > 0.0f) {
        const float alpha = (std::min)(0.32f, scene.player.damageFlash * 1.35f);
        draw->AddRectFilled(ImVec2(0.0f, 0.0f), io.DisplaySize,
                            ImGui::GetColorU32(ImVec4(0.75f, 0.0f, 0.0f, alpha)));
    }

    const float maxHealth = (std::max)(1.0f, scene.player.maxHealth);
    const float fraction = (std::max)(
        0.0f, (std::min)(1.0f, scene.player.health / maxHealth));
    const ImVec2 min(24.0f, io.DisplaySize.y - 52.0f);
    const ImVec2 max(min.x + 270.0f, min.y + 26.0f);
    const ImVec2 fillMax(min.x + (max.x - min.x) * fraction, max.y);

    draw->AddRectFilled(ImVec2(min.x - 3.0f, min.y - 3.0f),
                        ImVec2(max.x + 3.0f, max.y + 3.0f), IM_COL32(0, 0, 0, 190), 4.0f);
    const int red = (int)(255.0f * (1.0f - fraction));
    const int green = (int)(220.0f * fraction);
    if (fraction > 0.0f)
        draw->AddRectFilled(min, fillMax, IM_COL32(red, green, 35, 235), 2.0f);
    draw->AddRect(min, max, IM_COL32(255, 255, 255, 210), 2.0f, 0, 1.5f);

    char label[48];
    snprintf(label, sizeof(label), "HEALTH  %.0f / %.0f",
             scene.player.health, maxHealth);
    const ImVec2 textSize = ImGui::CalcTextSize(label);
    draw->AddText(ImVec2(min.x + ((max.x - min.x) - textSize.x) * 0.5f,
                         min.y + ((max.y - min.y) - textSize.y) * 0.5f),
                  IM_COL32(255, 255, 255, 255), label);

    // Ammo readout, bottom-right corner, mirroring the health bar's inset so the
    // two read as one HUD band. Right-aligned: the text grows leftward, keeping
    // the corner margin fixed as the digit count changes. God mode has no ammo
    // to show, so the HUD stays exactly as it was there.
    if (scene.AmmoEnforced()) {
        const int slot = GunModel::SelectedWeapon();
        const int inMag = scene.player.magazine[slot];
        const int spare = scene.player.reserve[slot];
        char ammo[64];
        if (scene.Reloading())
            snprintf(ammo, sizeof(ammo), "RELOADING...");
        else
            snprintf(ammo, sizeof(ammo), "%d / %d", inMag, spare);
        // Red when the magazine is dry, amber at a quarter left, else white.
        const int magSize = (std::max)(1, scene.player.magazineSize[slot]);
        ImU32 tint = IM_COL32(255, 255, 255, 255);
        if (!scene.Reloading() && inMag == 0)
            tint = IM_COL32(255, 70, 55, 255);
        else if (!scene.Reloading() && inMag * 4 <= magSize)
            tint = IM_COL32(255, 200, 60, 255);

        const ImVec2 ammoSize = ImGui::CalcTextSize(ammo);
        // 24 px from the right edge = the health bar's left inset, and the same
        // vertical band, so both sit on one line across the bottom.
        const float ammoRight = io.DisplaySize.x - 24.0f;
        const ImVec2 ammoPos(ammoRight - ammoSize.x,
                             min.y + ((max.y - min.y) - ammoSize.y) * 0.5f);
        draw->AddRectFilled(ImVec2(ammoPos.x - 8.0f, min.y - 3.0f),
                            ImVec2(ammoRight + 8.0f, max.y + 3.0f),
                            IM_COL32(0, 0, 0, 190), 4.0f);
        draw->AddText(ammoPos, tint, ammo);

        if (!scene.Reloading() && inMag == 0 && spare > 0) {
            const char* hint = "PRESS R";
            const ImVec2 hintSize = ImGui::CalcTextSize(hint);
            draw->AddText(ImVec2(ammoRight - hintSize.x, min.y - 22.0f),
                          IM_COL32(255, 200, 60, 235), hint);
        }
    }

    if (scene.player.health <= 0.0f) {
        const char* dead = "YOU DIED";
        const ImVec2 deadSize = ImGui::CalcTextSize(dead);
        draw->AddText(ImVec2((io.DisplaySize.x - deadSize.x) * 0.5f,
                             io.DisplaySize.y * 0.42f),
                      IM_COL32(255, 55, 40, 255), dead);
    }
}

inline void RenderUI(Scene& scene, VisibilityBufferDX12& vb) {
    RenderMovementPad();

    ImGui::Begin("Scene Controls", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

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
    ImGui::Text("Destruction batches: %u  Chunks: %u  Culled: %u",
                g_destructionBatchesThisFrame,
                g_destructionChunksSubmittedThisFrame,
                g_destructionCulledThisFrame);
    ImGui::Text("Destruction cache: item rebuilds %llu  geometry rebuilds %llu",
                static_cast<unsigned long long>(
                    g_destruction.GetRenderItemRebuildCount()),
                static_cast<unsigned long long>(
                    g_destruction.GetBatchGeometryRebuildCount()));
    ImGui::Text("Destruction render cache: %zu actor batches  %zu chunk fallbacks  Worker: %s",
                g_destruction.GetRenderBatches().size(),
                g_destruction.GetRenderItems().size(),
                g_destruction.IsBatchBuildPending() ? "building" : "idle");
    ImGui::Checkbox("God Mode", &scene.player.godMode);
    const StaticBufferStatsDX12 staticStats = GetStaticBufferStatsDX12();
    ImGui::Text("GPU-local static buffers: %u  %.1f MiB  Pending: %u",
                staticStats.resources, staticStats.bytes / (1024.0 * 1024.0),
                staticStats.pendingUploads);
    if (ImGui::CollapsingHeader("CPU / GPU Profiler", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("CPU frame: %.2f ms", g_profiler.CpuFrameMs());
        for (const auto& sample : g_profiler.CpuSamples())
            ImGui::BulletText("%s: %.3f ms", sample.name.c_str(), sample.milliseconds);
        ImGui::Separator();
        if (g_profiler.IsInitialized()) {
            ImGui::Text("GPU frame: %.2f ms", g_profiler.GpuFrameMs());
            ImGui::Text("GPU p95 (%zu/300): %.2f ms",
                        g_profiler.GpuHistorySize(), g_profiler.GpuFrameP95Ms());
            for (const auto& sample : g_profiler.GpuSamples())
                ImGui::BulletText("%s: %.3f ms", sample.name.c_str(), sample.milliseconds);
            ImGui::TextDisabled("GPU results delayed by frames in flight");
        } else {
            ImGui::TextDisabled("GPU timestamp queries unavailable");
        }
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
    if (ImGui::CollapsingHeader("Camera Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::DragFloat3("Camera Position", &scene.camera.Position.x, 0.1f);
        ImGui::DragFloat("FOV",  &scene.cameraFOV,  0.5f, 1.0f, 120.0f);
        ImGui::DragFloat("Near", &scene.cameraNear, 0.01f, 0.01f, 10.0f);
        ImGui::DragFloat("Far",  &scene.cameraFar,  1.0f, 10.0f, 2000.0f);
        ImGui::DragFloat("Speed", &scene.camera.MovementSpeed, 0.1f, 0.1f, 50.0f);
        ImGui::Checkbox("FPS Walking Mode", &scene.camera.FPSMode);
    }

    // -- Light --
    if (ImGui::CollapsingHeader("Light Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
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
    if (ImGui::CollapsingHeader("Cube 1 Settings")) {
        ImGui::DragFloat3("Position##c1", &scene.cube1.position.x, 0.1f);
        ImGui::DragFloat3("Rotation##c1", &scene.cube1.rotation.x, 1.0f);
        ImGui::DragFloat3("Scale##c1",    &scene.cube1.scale.x,    0.1f, 0.1f, 10.0f);
        ImGui::ColorEdit3("Color##c1",    &scene.cube1.color.x);
        ImGui::Checkbox("Animate##c1",    &scene.animateCube);
    }

    // -- Cube 2 --
    if (ImGui::CollapsingHeader("Cube 2 Settings")) {
        ImGui::Checkbox("Show Second Cube", &scene.cube2.visible);
        if (scene.cube2.visible) {
            ImGui::DragFloat3("Position##c2", &scene.cube2.position.x, 0.1f);
            ImGui::DragFloat3("Rotation##c2", &scene.cube2.rotation.x, 1.0f);
            ImGui::DragFloat3("Scale##c2",    &scene.cube2.scale.x,    0.1f, 0.1f, 10.0f);
            ImGui::ColorEdit3("Color##c2",    &scene.cube2.color.x);
        }
    }

    // -- Rendering --
    if (ImGui::CollapsingHeader("Rendering Settings")) {
        ImGui::ColorEdit3("Floor Color", &scene.floor.color.x);
        ImGui::ColorEdit3("Clear Color", &scene.clearColor.x);
        ImGui::Checkbox("Wireframe Mode", &scene.wireframeMode);
        ImGui::Checkbox("Mesh Shader Terrain", &scene.useMeshTerrain);
        if (scene.useMeshTerrain) {
            ImGui::SliderFloat("Terrain Height", &scene.terrainHeightScale, 0.0f, 15.0f);
        }
        ImGui::DragFloat("Specular", &scene.specularStrength, 0.01f, 0.0f, 1.0f);
        ImGui::Checkbox("Show Helicopter", &scene.showHelicopter);
        ImGui::Checkbox("Enable Shadows", &scene.enableShadows);
        if (scene.enableShadows) {
            ImGui::DragFloat("Shadow Bias", &scene.shadowBias, 0.0005f, 0.0f, 0.05f, "%.4f");
            ImGui::DragFloat3("Shadow Center", &scene.shadowCenter.x, 0.1f);
            ImGui::DragFloat("Shadow Size", &scene.shadowOrthoSize, 0.5f, 5.0f, 80.0f);
            ImGui::DragFloat("Shadow Distance", &scene.shadowDistance, 0.5f, 5.0f, 120.0f);
            ImGui::DragFloat("Shadow Far", &scene.shadowFarPlane, 0.5f, 10.0f, 200.0f);
        }

        // -- Rendering Pipeline Selection --
        ImGui::Separator();
        ImGui::Text("Rendering Pipeline");
        
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
            if (scene.useVisibilityBuffer) {
                ImGui::Text("  Pass 1: Visibility rasterise");
                ImGui::Text("  Pass 2: G-Buffer fill (compute)");
                ImGui::Text("  Pass 3: Deferred lighting (compute)");
                ImGui::Text("  Instances: %u  Persistent vertices: %u",
                    vb.currentDrawCall, vb.persistentVertexCount);
                ImGui::Text("  Persistent meshes: %u",
                    static_cast<UINT>(vb.meshes.size()));
                const char* debugViews[] = {
                    "Lit resolve", "Instance / primitive IDs", "Raw depth"
                };
                ImGui::Combo("VB Debug View", &vb.debugViewMode,
                    debugViews, IM_ARRAYSIZE(debugViews));
                ImGui::SliderFloat("VB Exposure", &vb.exposure, 0.25f, 4.0f, "%.2f");
                ImGui::SliderFloat("VB Eye Adaptation", &vb.exposureAdaptation, 0.005f, 0.25f, "%.3f");
                ImGui::SliderFloat("VB Bloom", &vb.bloomStrength, 0.0f, 1.0f, "%.2f");
                ImGui::SliderFloat("VB Vignette", &vb.vignetteStrength, 0.0f, 1.0f, "%.2f");
                ImGui::SliderFloat("VB Film Grain", &vb.grainStrength, 0.0f, 0.08f, "%.3f");
            }
            if (ImGui::Checkbox("Temporal AA (TAA)",
                                &vb.temporalEffectsEnabled))
                vb.InvalidateTemporalHistory();
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
        ImGui::Checkbox("Physical Atmosphere", &scene.enablePhysicalAtmosphere);
        if (scene.enablePhysicalAtmosphere) {
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
            ImGui::SliderFloat("Cloud Coverage",
                               &scene.atmosphereCloudCoverage,
                               0.0f, 1.0f, "%.2f");
            ImGui::SliderFloat("Cloud Density",
                               &scene.atmosphereCloudDensity,
                               0.0f, 1.5f, "%.2f");
            ImGui::DragFloat("Cloud Base",
                             &scene.atmosphereCloudBaseHeight,
                             10.0f, 50.0f, 5000.0f, "%.0f m");
            ImGui::DragFloat("Cloud Thickness",
                             &scene.atmosphereCloudThickness,
                             10.0f, 50.0f, 5000.0f, "%.0f m");
        }
        ImGui::Checkbox("GTAO + Contact Shadows", &scene.enableAmbientOcclusion);
        if (scene.enableAmbientOcclusion) {
            ImGui::DragFloat("AO Radius", &scene.ambientOcclusionRadius,
                             0.01f, 0.01f, 4.0f, "%.2f m");
            ImGui::SliderFloat("AO Strength", &scene.ambientOcclusionStrength,
                               0.0f, 2.5f, "%.2f");
            ImGui::SliderFloat("Contact Shadows", &scene.contactShadowStrength,
                               0.0f, 1.0f, "%.2f");
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
        ImGui::Checkbox("Volumetric Fog", &scene.enableVolumetricFog);
        if (scene.enableVolumetricFog) {
            ImGui::DragFloat("Fog Density", &scene.volumetricFogDensity,
                             0.0005f, 0.0001f, 0.05f, "%.4f");
            ImGui::SliderFloat("Fog Anisotropy", &scene.volumetricFogAnisotropy,
                               0.0f, 0.9f, "%.2f");
            ImGui::ColorEdit3("Fog Tint", &scene.volumetricFogTint.x);
            ImGui::SliderFloat("Fog Height Falloff", &scene.volumetricFogHeightFalloff,
                               0.01f, 0.25f, "%.3f");
            ImGui::DragFloat("Fog Base Height", &scene.volumetricFogBaseHeight,
                             0.1f, -5.0f, 30.0f, "%.1f m");
            ImGui::DragFloat("Fog Distance", &scene.volumetricFogDistance,
                             5.0f, 20.0f, scene.cameraFar, "%.0f m");
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
    if (ImGui::CollapsingHeader("Viewmodel (Gun)")) {
        ImGui::Checkbox("Show Gun",    &scene.gun.visible);
        ImGui::ColorEdit3("Gun Color", &scene.gun.color.x);
        ImGui::DragFloat3("Offset",    &scene.gun.offset.x,   0.01f);
        ImGui::DragFloat3("Scale##gun",&scene.gun.scale.x,     0.01f);
        ImGui::DragFloat3("Rot##gun",  &scene.gun.rotation.x,  1.0f);
        ImGui::Text("Fit: %s", GunModel::SelectedWeaponName());
        ImGui::DragFloat3("Weapon Fit Offset",
                          &GunModel::PlayerOffset().x, 0.005f);
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
    if (g_grass.IsInitialized() && ImGui::CollapsingHeader("Grass & Wind")) {
        ImGui::SeparatorText("Material");
        ImGui::ColorEdit3("Grass Albedo", &g_grass.Albedo().x);
        ImGui::SliderFloat("Grass Roughness", &g_grass.Roughness(), 0.04f, 1.0f);
        ImGui::SliderFloat("Grass Ambient", &g_grass.AmbientScale(), 0.0f, 2.0f);
        ImGui::SliderFloat(
            "Grass Direct Light", &g_grass.DirectLightScale(), 0.0f, 2.0f);
        ImGui::SliderFloat(
            "Grass Transmission", &g_grass.TransmissionStrength(), 0.0f, 1.0f);
        ImGui::SliderFloat(
            "Grass Color Variation", &g_grass.ColorVariation(), 0.0f, 1.5f);
        if (ImGui::Button("Reset Grass Material"))
            g_grass.ResetMaterial();

        ImGui::SeparatorText("Wind & Performance");
        ImGui::DragFloat("Wind Strength", &g_grass.WindStrength(), 0.01f, 0.0f, 1.0f);
        ImGui::DragFloat("Wind Speed",    &g_grass.WindSpeed(),    0.05f, 0.0f, 6.0f);
        // Perf dials: density trims blades per cell (whole tufts, no rebuild);
        // distance shrinks the drawn ring. ~0.6 / 22 is a good perf preset.
        ImGui::SliderFloat("Density", &g_grass.Density(), 0.05f, 1.0f);
        ImGui::DragFloat("Draw Distance", &g_grass.DrawDistance(), 0.5f, 8.0f, 40.0f);
    }

    if (ImGui::CollapsingHeader("Destruction", ImGuiTreeNodeFlags_DefaultOpen)) {
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

    if (ImGui::CollapsingHeader("Palm Trees", ImGuiTreeNodeFlags_DefaultOpen)) {
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
    if (scene.useRaytracing) renderer = "DXR Raytracing";
    else if (scene.useVisibilityBuffer) renderer = "id Tech VB+Deferred";
    ImGui::Text("Renderer: DirectX 12 (%s)", renderer);

    ImGui::End();
}

#endif // ENGINE_UI_H
