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
extern UINT g_destructionBatchesThisFrame;
extern UINT g_destructionChunksSubmittedThisFrame;
extern UINT g_destructionCulledThisFrame;
extern MeshShaderDX12 g_meshShader;

// Skinned Bandit enemy (defined in main.cpp) -- surfaced for debug readout.
// BanditDebugText renders a one-line status; defined in main.cpp where the
// SkinnedEnemy type is complete.
void BanditDebugText();

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

    if (scene.playerGodMode) {
        const char* god = "GOD MODE";
        const ImVec2 size = ImGui::CalcTextSize(god);
        draw->AddText(ImVec2((io.DisplaySize.x - size.x) * 0.5f, 24.0f),
                      IM_COL32(255, 220, 65, 245), god);
    }

    if (scene.playerHealth > 0.0f) {
        const ImVec2 center(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
        constexpr float gap = 3.0f;
        constexpr float arm = 5.0f;
        const ImU32 outline = IM_COL32(0, 0, 0, 190);
        const ImU32 reticle = IM_COL32(235, 235, 225, 220);
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
    if (scene.playerDamageFlash > 0.0f) {
        const float alpha = (std::min)(0.32f, scene.playerDamageFlash * 1.35f);
        draw->AddRectFilled(ImVec2(0.0f, 0.0f), io.DisplaySize,
                            ImGui::GetColorU32(ImVec4(0.75f, 0.0f, 0.0f, alpha)));
    }

    const float maxHealth = (std::max)(1.0f, scene.playerMaxHealth);
    const float fraction = (std::max)(0.0f, (std::min)(1.0f, scene.playerHealth / maxHealth));
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
    snprintf(label, sizeof(label), "HEALTH  %.0f / %.0f", scene.playerHealth, maxHealth);
    const ImVec2 textSize = ImGui::CalcTextSize(label);
    draw->AddText(ImVec2(min.x + ((max.x - min.x) - textSize.x) * 0.5f,
                         min.y + ((max.y - min.y) - textSize.y) * 0.5f),
                  IM_COL32(255, 255, 255, 255), label);

    if (scene.playerHealth <= 0.0f) {
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
    ImGui::Text("Mesh dispatches: %u  Meshlets: %u",
                g_meshShader.dispatchesThisFrame, g_meshShader.meshletsThisFrame);
    ImGui::Text("Destruction batches: %u  Chunks: %u  Culled: %u",
                g_destructionBatchesThisFrame,
                g_destructionChunksSubmittedThisFrame,
                g_destructionCulledThisFrame);
    ImGui::Checkbox("God Mode", &scene.playerGodMode);
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
        ImGui::DragFloat("Ambient",  &scene.ambientStrength,  0.01f, 0.0f, 1.0f);
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
            if (ImGui::Checkbox("id Tech VB + Deferred", &vbEnabled)) {
                scene.useVisibilityBuffer = vbEnabled;
                if (vbEnabled) {
                    scene.useRaytracing = false;
                    g_rt.enabled = false;
                }
            }
            if (scene.useVisibilityBuffer) {
                ImGui::Text("  Pass 1: Visibility rasterise");
                ImGui::Text("  Pass 2: G-Buffer fill (compute)");
                ImGui::Text("  Pass 3: Deferred lighting (compute)");
                ImGui::Text("  Draw Calls: %u  Vertices: %u", vb.currentDrawCall, vb.currentVertexOffset);
            }
        } else {
            ImGui::TextDisabled("VB Pipeline: Not Available");
        }

        ImGui::Checkbox("4x MSAA (Forward only)", &scene.enableMSAA);
        ImGui::Checkbox("FXAA", &scene.enableFXAA);
        if (scene.enableMSAA &&
            (scene.useVisibilityBuffer || scene.useRaytracing)) {
            ImGui::TextDisabled("MSAA inactive outside Forward renderer");
        }
        if (!scene.useVisibilityBuffer && !scene.useRaytracing) {
            ImGui::Text("Active: Forward Clustered");
        }

        // -- DDGI --
        ImGui::Separator();
        ImGui::Text("DDGI Global Illumination");
        ImGui::Checkbox("Enable DDGI", &scene.useDDGI);
        if (scene.useDDGI) {
            ImGui::DragFloat("GI Intensity", &scene.giIntensity, 0.1f, 0.0f, 5.0f);
            ImGui::DragFloat("Normal Bias",  &scene.normalBias,  0.01f, 0.0f, 1.0f);
            ImGui::DragFloat("Probe Spacing",&scene.probeSpacing, 0.1f, 0.5f, 10.0f);
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
        ImGui::Checkbox("Auto Fire", &scene.autoFire);
        ImGui::DragFloat("Fire Interval", &scene.fireInterval, 0.005f, 0.02f, 1.0f, "%.3f s");
    }

    // -- Grass / wind --
    if (g_grass.IsInitialized() && ImGui::CollapsingHeader("Grass & Wind")) {
        ImGui::DragFloat("Wind Strength", &g_grass.WindStrength(), 0.01f, 0.0f, 1.0f);
        ImGui::DragFloat("Wind Speed",    &g_grass.WindSpeed(),    0.05f, 0.0f, 6.0f);
        // Perf dials: density trims blades per cell (whole tufts, no rebuild);
        // distance shrinks the drawn ring. ~0.6 / 22 is a good perf preset.
        ImGui::SliderFloat("Density", &g_grass.Density(), 0.05f, 1.0f);
        ImGui::DragFloat("Draw Distance", &g_grass.DrawDistance(), 0.5f, 8.0f, 40.0f);
        ImGui::SeparatorText("Grass Shadows");
        ImGui::Checkbox("Cast Grass Shadows", &g_grass.CastShadows());
        if (g_grass.CastShadows()) {
            float shadowPercent = g_grass.ShadowDensity() * 100.0f;
            if (ImGui::SliderFloat("Shadow Amount", &shadowPercent,
                                   0.0f, 100.0f, "%.0f%%",
                                   ImGuiSliderFlags_AlwaysClamp))
                g_grass.ShadowDensity() = shadowPercent * 0.01f;
            const size_t shadowBudget = static_cast<size_t>(
                g_grass.PlantedCount() * g_grass.Density() *
                g_grass.ShadowDensity());
            ImGui::Text("Shadow blade budget: ~%zu", shadowBudget);
        }
    }

    if (ImGui::CollapsingHeader("Destruction", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Enable Destructible Wall", &scene.useDestruction);
        ImGui::DragFloat("Damage Radius", &scene.destructionDamageRadius, 0.1f, 0.25f, 8.0f);
        ImGui::DragFloat("Damage", &scene.destructionDamage, 0.1f, 0.1f, 10.0f);
        ImGui::DragFloat("Bullet Impulse", &scene.destructionBulletImpulse, 5.0f, 0.0f, 1000.0f);
        ImGui::Text("Wall: %u chunks  %u actors", g_destruction.GetChunkCount(), g_destruction.GetActorCount());
        ImGui::Checkbox("Blast Debug Draw", &scene.showDestructionDebug);
        if (ImGui::Button("Rebuild Wall")) scene.rebuildDestructionRequested = true;
    }

    if (ImGui::CollapsingHeader("Palm Trees", ImGuiTreeNodeFlags_DefaultOpen)) {
        // Absolute damage vs a section's 45 health: 15 => ~3 hits to sever.
        ImGui::DragFloat("Tree Damage/Shot", &scene.treeDamagePerShot, 0.5f, 1.0f, 60.0f);
        ImGui::Text("Shoot a trunk to fell the tree above the hit.");
    }

    ImGui::Separator();
    ImGui::TextColored(scene.playerHealth > 30.0f ? ImVec4(0.3f, 1.0f, 0.35f, 1.0f)
                                                   : ImVec4(1.0f, 0.2f, 0.12f, 1.0f),
                       "Health: %.0f / %.0f", scene.playerHealth, scene.playerMaxHealth);
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
