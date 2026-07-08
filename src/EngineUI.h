#ifndef ENGINE_UI_H
#define ENGINE_UI_H

#include <imgui.h>
#include "Scene.h"
#include "VisibilityBufferDX12.h"

// Forward declare raytracing context
struct RaytracingContext;
extern RaytracingContext g_rt;

inline void RenderUI(Scene& scene, VisibilityBufferDX12& vb) {
    ImGui::Begin("Scene Controls", nullptr, ImGuiWindowFlags_AlwaysAutoResize);

    ImGui::Text("Controls:");
    ImGui::BulletText("TAB: Toggle UI");
    ImGui::BulletText("C: Lock/Unlock Camera");
    ImGui::BulletText("F: Toggle FPS Walking Mode");
    ImGui::BulletText("Left Click: Lock camera / Shoot");
    ImGui::Separator();

    // -- Camera --
    if (ImGui::CollapsingHeader("Camera Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::DragFloat3("Camera Position", &scene.camera.Position.x, 0.1f);
        ImGui::DragFloat("FOV",  &scene.cameraFOV,  0.5f, 1.0f, 120.0f);
        ImGui::DragFloat("Near", &scene.cameraNear, 0.01f, 0.01f, 10.0f);
        ImGui::DragFloat("Far",  &scene.cameraFar,  1.0f, 10.0f, 500.0f);
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
        ImGui::DragFloat("Ambient",  &scene.ambientStrength,  0.01f, 0.0f, 1.0f);
        ImGui::DragFloat("Specular", &scene.specularStrength, 0.01f, 0.0f, 1.0f);
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
    }

    ImGui::Separator();
    ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
    
    const char* renderer = "Forward Clustered";
    if (scene.useRaytracing) renderer = "DXR Raytracing";
    else if (scene.useVisibilityBuffer) renderer = "id Tech VB+Deferred";
    ImGui::Text("Renderer: DirectX 12 (%s)", renderer);

    ImGui::End();
}

#endif // ENGINE_UI_H
