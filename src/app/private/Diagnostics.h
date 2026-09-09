#pragma once

// Private application implementation; included once by main.cpp in dependency order.

// Draw the walkable navmesh as a translucent overlay, so the holes props punch
// in it are visible while authoring. Same approach as the destruction overlay:
// project to screen and use ImGui's foreground draw list, no new pipeline.
//
// The foreground list does not depth test, so this paints over props standing in
// front of it. That is acceptable for reading blocked ground from above, which is
// what the overlay is for.
static void DrawNavmeshDebug(const XMMATRIX& view, const XMMATRIX& projection) {
    if (!g_navigation.Ready()) return;

    // Rebuilt only at reconcile boundaries, so this is not per-frame work in the
    // usual case -- but the vector is reused across frames regardless.
    static std::vector<XMFLOAT3> triangles;
    g_navigation.DebugWalkableTriangles(triangles);
    if (triangles.size() < 3) return;

    const XMMATRIX viewProj = view * projection;
    const ImGuiIO& io = ImGui::GetIO();
    const float w = io.DisplaySize.x, h = io.DisplaySize.y;
    ImDrawList* dl = ImGui::GetForegroundDrawList();

    auto project = [&](const XMFLOAT3& p, ImVec2& out) -> bool {
        XMVECTOR clip = XMVector3Transform(XMLoadFloat3(&p), viewProj);
        const float cw = XMVectorGetW(clip);
        if (cw <= 0.0001f) return false;
        const float ndcX = XMVectorGetX(clip) / cw, ndcY = XMVectorGetY(clip) / cw;
        out = ImVec2((ndcX * 0.5f + 0.5f) * w, (1.0f - (ndcY * 0.5f + 0.5f)) * h);
        return true;
    };

    // The detail mesh multiplies the polygon count, and every triangle here is
    // six unbatched ImGui vertices. Cap the work so a large island cannot stall
    // the editor; the cap is generous enough for the 122 m levels in practice.
    constexpr size_t kMaxTriangles = 120000;
    const size_t availableTriangles = triangles.size() / 3;
    const size_t triangleCount = (std::min)(availableTriangles, kMaxTriangles);
    // Say so when the cap bites: a silently truncated overlay looks exactly
    // like a navmesh that does not cover the island, which is the bug this
    // overlay exists to reveal.
    if (availableTriangles > kMaxTriangles) {
        static size_t warnedFor = 0;
        if (warnedFor != availableTriangles) {
            warnedFor = availableTriangles;
            SGE_LOG("LogNav", EngineLog::Level::Warning,
                "Navmesh overlay truncated: drawing " +
                std::to_string(kMaxTriangles) + " of " +
                std::to_string(availableTriangles) + " triangles");
        }
    }

    // Lifted slightly so the overlay reads as sitting on the ground rather than
    // fighting the terrain it was built from.
    constexpr float kLift = 0.05f;
    const ImU32 fill = IM_COL32(60, 220, 90, 40);
    const ImU32 edge = IM_COL32(70, 240, 105, 110);

    for (size_t i = 0; i < triangleCount; ++i) {
        ImVec2 corner[3];
        bool visible = true;
        for (int c = 0; c < 3; ++c) {
            XMFLOAT3 p = triangles[i * 3 + c];
            p.y += kLift;
            visible = project(p, corner[c]) && visible;
        }
        // All-or-nothing, matching the destruction overlay's boxes: a partially
        // projected triangle would smear across the screen.
        if (!visible) continue;
        dl->AddTriangleFilled(corner[0], corner[1], corner[2], fill);
        dl->AddTriangle(corner[0], corner[1], corner[2], edge);
    }
}

// Draw Blast/Box3D destruction state as a 2D overlay using ImGui's foreground
// draw list: chunk AABBs coloured by role, bonds (green healthy / red severed),
// and the last hit sphere. No new pipeline needed -- just project to screen.
static void DrawDestructionDebug(Scene& scene) {
    if (!scene.showDestructionDebug || !g_destruction.IsInitialized()) return;
    const DestructionDebugData data = g_destruction.GetDebugData();

    const XMMATRIX viewProj = scene.GetViewMatrix() * scene.GetProjectionMatrix();
    const ImGuiIO& io = ImGui::GetIO();
    const float w = io.DisplaySize.x, h = io.DisplaySize.y;
    ImDrawList* dl = ImGui::GetForegroundDrawList();

    // World -> screen; returns false when behind the camera.
    auto project = [&](const XMFLOAT3& p, ImVec2& out) -> bool {
        XMVECTOR clip = XMVector3Transform(XMLoadFloat3(&p), viewProj);
        const float cw = XMVectorGetW(clip);
        if (cw <= 0.0001f) return false;
        const float ndcX = XMVectorGetX(clip) / cw, ndcY = XMVectorGetY(clip) / cw;
        out = ImVec2((ndcX * 0.5f + 0.5f) * w, (1.0f - (ndcY * 0.5f + 0.5f)) * h);
        return true;
    };

    // Wireframe box from world-space AABB corners.
    auto drawBox = [&](const XMFLOAT3& lo, const XMFLOAT3& hi, ImU32 color) {
        const XMFLOAT3 c[8] = {
            {lo.x,lo.y,lo.z},{hi.x,lo.y,lo.z},{hi.x,hi.y,lo.z},{lo.x,hi.y,lo.z},
            {lo.x,lo.y,hi.z},{hi.x,lo.y,hi.z},{hi.x,hi.y,hi.z},{lo.x,hi.y,hi.z} };
        ImVec2 s[8]; bool ok = true;
        for (int i = 0; i < 8; ++i) ok = project(c[i], s[i]) && ok;
        if (!ok) return;
        const int edges[12][2] = { {0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},
                                   {0,4},{1,5},{2,6},{3,7} };
        for (auto& e : edges) dl->AddLine(s[e[0]], s[e[1]], color, 1.2f);
    };

    // Chunks: yellow = anchored support, cyan = dynamic (falling), grey = static.
    for (const DestructionDebugChunk& chunk : data.chunks) {
        const ImU32 color = chunk.support ? IM_COL32(255, 215, 0, 200)
                          : chunk.dynamic ? IM_COL32(0, 220, 255, 180)
                                          : IM_COL32(150, 150, 150, 110);
        drawBox(chunk.worldMin, chunk.worldMax, color);
    }

    // Bonds coloured by live health: full green -> yellow -> red as it drains.
    // Skip severed bonds whose chunks have drifted apart (once pieces fall their
    // world centres scatter and the lines sprawl across the scene as noise);
    // only show a severed bond while its two chunks are still close.
    for (const DestructionDebugBond& bond : data.bonds) {
        if (bond.broken) {
            const float dx = bond.a.x - bond.b.x, dy = bond.a.y - bond.b.y, dz = bond.a.z - bond.b.z;
            if (dx * dx + dy * dy + dz * dz > 1.5f * 1.5f) continue;
        }
        ImVec2 a, b;
        if (!project(bond.a, a) || !project(bond.b, b)) continue;
        ImU32 color;
        float thickness;
        if (bond.broken) {
            color = IM_COL32(255, 40, 40, 220); thickness = 1.0f;
        } else {
            // Health fraction f: f=1 green (0,255,60), f=0 red (255,40,40).
            const float f = bond.healthFraction;
            const int r = (int)(255 * (1.0f - f) + 40 * f);
            const int g = (int)(60 * (1.0f - f) + 255 * f);
            color = IM_COL32(r, g, 60, 210);
            thickness = 1.5f + f;  // healthier = thicker
        }
        dl->AddLine(a, b, color, thickness);
        // Label weakened (but not broken) bonds with their remaining health.
        if (!bond.broken && bond.healthFraction < 0.99f) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%.2f", bond.health);
            const ImVec2 mid((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
            dl->AddText(mid, IM_COL32(255, 255, 255, 230), buf);
        }
    }

    // Last hit: magenta ring at the impact, radius projected roughly to screen.
    if (data.hasHit) {
        ImVec2 center;
        if (project(data.lastHit, center)) {
            const XMFLOAT3 edge = { data.lastHit.x + data.hitRadius, data.lastHit.y, data.lastHit.z };
            ImVec2 edgePt;
            float pixelRadius = 8.0f;
            if (project(edge, edgePt)) {
                const float dx = edgePt.x - center.x, dy = edgePt.y - center.y;
                pixelRadius = std::max(4.0f, std::sqrt(dx * dx + dy * dy));
            }
            dl->AddCircle(center, pixelRadius, IM_COL32(255, 0, 255, 230), 24, 2.0f);
            dl->AddCircleFilled(center, 4.0f, IM_COL32(255, 0, 255, 255));
        }
    }

    // Stats readout.
    ImGui::SetNextWindowBgAlpha(0.75f);
    if (ImGui::Begin("Blast Debug", &scene.showDestructionDebug,
                     ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("chunks:  %zu", data.chunks.size());
        ImGui::Text("bonds:   %zu", data.bonds.size());
        size_t broken = 0, weakened = 0;
        float minHealth = FLT_MAX, sumHealth = 0.0f;
        for (const auto& bond : data.bonds) {
            if (bond.broken) { ++broken; continue; }
            sumHealth += bond.health;
            minHealth = std::min(minHealth, bond.health);
            if (bond.healthFraction < 0.99f) ++weakened;
        }
        const size_t intact = data.bonds.size() - broken;
        ImGui::Text("severed:  %zu", broken);
        ImGui::Text("weakened: %zu", weakened);
        ImGui::Text("health:   min %.2f  avg %.2f",
                    intact ? minHealth : 0.0f, intact ? sumHealth / intact : 0.0f);
        ImGui::Text("actors:  %u  (dynamic %u)", data.actorCount, data.dynamicActorCount);
        if (data.hasHit)
            ImGui::Text("last hit: %.2f %.2f %.2f  r=%.2f",
                        data.lastHit.x, data.lastHit.y, data.lastHit.z, data.hitRadius);
        ImGui::Separator();
        ImGui::TextColored(ImVec4(1, 0.84f, 0, 1), "yellow = support (anchored)");
        ImGui::TextColored(ImVec4(0, 0.86f, 1, 1), "cyan   = dynamic (falling)");
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1), "grey   = static");
        ImGui::TextColored(ImVec4(0.24f, 1, 0.35f, 1), "bond: green = full health");
        ImGui::TextColored(ImVec4(1, 0.84f, 0.24f, 1), "bond: yellow = damaged");
        ImGui::TextColored(ImVec4(1, 0.24f, 0.24f, 1), "bond: red = severed");
    }
    ImGui::End();
}

// X-ray wire overlay of exact authored Box3D primitives. Drawn through ImGui so
// colliders remain visible inside the skinned corpse and behind nearby foliage.
static void DrawRagdollPhysicsDebug(Scene& scene) {
    if (!scene.showRagdollPhysicsShapes || !g_destruction.IsInitialized()) return;
    const std::vector<RagdollPhysicsDebugShape> shapes =
        g_destruction.GetRagdollPhysicsDebugShapes();
    const XMMATRIX viewProj = scene.GetViewMatrix() * scene.GetProjectionMatrix();
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    constexpr float pi = 3.14159265358979323846f;

    auto project = [&](const RagdollPhysicsDebugShape& shape,
                       const XMFLOAT3& local, ImVec2& screen) {
        const XMVECTOR world = XMVector3TransformCoord(
            XMLoadFloat3(&local), XMLoadFloat4x4(&shape.transform));
        const XMVECTOR clip = XMVector3Transform(world, viewProj);
        const float clipW = XMVectorGetW(clip);
        if (clipW <= 0.0001f) return false;
        const float x = XMVectorGetX(clip) / clipW;
        const float y = XMVectorGetY(clip) / clipW;
        screen = { (x * 0.5f + 0.5f) * display.x,
                   (1.0f - (y * 0.5f + 0.5f)) * display.y };
        return true;
    };
    auto line = [&](const RagdollPhysicsDebugShape& shape,
                    const XMFLOAT3& a, const XMFLOAT3& b, ImU32 color) {
        ImVec2 pa, pb;
        if (project(shape, a, pa) && project(shape, b, pb))
            draw->AddLine(pa, pb, color, 1.8f);
    };
    auto circle = [&](const RagdollPhysicsDebugShape& shape, int plane,
                      float offset, float radius, ImU32 color) {
        constexpr int segments = 24;
        for (int i = 0; i < segments; ++i) {
            const float a = 2.0f*pi*i/segments;
            const float b = 2.0f*pi*(i+1)/segments;
            auto point = [&](float angle) {
                const float c = std::cos(angle)*radius;
                const float s = std::sin(angle)*radius;
                if (plane == 0) return XMFLOAT3(c, s, offset);
                if (plane == 1) return XMFLOAT3(c, offset, s);
                return XMFLOAT3(offset, c, s);
            };
            line(shape, point(a), point(b), color);
        }
    };

    for (const RagdollPhysicsDebugShape& shape : shapes) {
        if (shape.type == RagdollShapeType::Box) {
            const XMFLOAT3& h = shape.halfExtent;
            const XMFLOAT3 c[8] = {
                {-h.x,-h.y,-h.z},{h.x,-h.y,-h.z},{h.x,h.y,-h.z},{-h.x,h.y,-h.z},
                {-h.x,-h.y,h.z},{h.x,-h.y,h.z},{h.x,h.y,h.z},{-h.x,h.y,h.z} };
            const int edges[12][2] = {
                {0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},
                {0,4},{1,5},{2,6},{3,7} };
            for (const auto& edge : edges)
                line(shape, c[edge[0]], c[edge[1]], IM_COL32(0,220,255,235));
        } else if (shape.type == RagdollShapeType::Sphere) {
            for (int plane = 0; plane < 3; ++plane)
                circle(shape, plane, 0.0f, shape.radius,
                       IM_COL32(255,220,30,235));
        } else {
            const ImU32 green = IM_COL32(70,255,90,240);
            const float bottom = -shape.length*0.5f;
            const float top = shape.length*0.5f;
            circle(shape, 1, bottom, shape.radius, green);
            circle(shape, 1, top, shape.radius, green);
            constexpr int sides = 12;
            for (int i = 0; i < sides; ++i) {
                const float angle = 2.0f*pi*i/sides;
                const float x = std::cos(angle)*shape.radius;
                const float z = std::sin(angle)*shape.radius;
                line(shape, {x,bottom,z}, {x,top,z}, green);
            }
            constexpr int arcs = 12;
            for (int plane = 0; plane < 3; ++plane) {
                const float theta = pi*plane/3.0f;
                const float ux = std::cos(theta), uz = std::sin(theta);
                for (int i = 0; i < arcs; ++i) {
                    const float a = pi*i/arcs, b = pi*(i+1)/arcs;
                    auto cap = [&](float angle, float y, float sign) {
                        return XMFLOAT3(ux*std::cos(angle)*shape.radius,
                            y + sign*std::sin(angle)*shape.radius,
                            uz*std::cos(angle)*shape.radius);
                    };
                    line(shape, cap(a, top, 1.0f), cap(b, top, 1.0f), green);
                    line(shape, cap(a, bottom, -1.0f),
                         cap(b, bottom, -1.0f), green);
                }
            }
        }
    }
    draw->AddText(ImVec2(18.0f, 92.0f), IM_COL32(70,255,90,255),
                  "RAGDOLL PHYSICS SHAPES");
}

// ?? timer ????????????????????????????????????????????????????????????????????
class Timer {
    std::chrono::high_resolution_clock::time_point t0;
public:
    void  Start()      { t0 = std::chrono::high_resolution_clock::now(); }
    float GetElapsed() {
        return std::chrono::duration<float>(
            std::chrono::high_resolution_clock::now() - t0).count();
    }
};
static Timer gameTimer;

// Frame times spike randomly and unpredictably in play; the ImGui profiler
// overlay only shows the current frame, so nothing captures what a spike
// actually was once it's passed. This appends one line per spike (frame time
// over 1.5x the 60fps budget) with a timestamp and the slowest CPU/GPU scopes
// from that frame, so a spike caught during normal play leaves a trail.
static void LogFrameSpike(float deltaTimeSeconds) {
    constexpr double kSpikeThresholdMs = (1000.0 / 60.0) * 1.5; // ~25ms
    const double frameMs = double(deltaTimeSeconds) * 1000.0;
    if (frameMs < kSpikeThresholdMs) return;

    std::ofstream log("logs/frame_spikes.log", std::ios::app);
    if (!log) return;

    const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm localNow{};
    localtime_s(&localNow, &now);

    log << std::put_time(&localNow, "%H:%M:%S") << " frame=" << std::fixed
        << std::setprecision(2) << frameMs << "ms";

    auto logTopSamples = [&](const char* label, const std::vector<ProfilerSampleDX12>& samples) {
        std::vector<ProfilerSampleDX12> sorted(samples.begin(), samples.end());
        std::sort(sorted.begin(), sorted.end(),
            [](const ProfilerSampleDX12& a, const ProfilerSampleDX12& b) {
                return a.milliseconds > b.milliseconds;
            });
        log << " " << label << "=[";
        // 5 was too few to attribute a spike: a nested scope's own phases sort
        // below the outer scope plus the always-present ImGui/post entries, so
        // the breakdown that explains the cost is exactly what got cut.
        for (size_t i = 0; i < sorted.size() && i < 12; ++i) {
            if (i) log << ", ";
            log << sorted[i].name << ":" << std::setprecision(2) << sorted[i].milliseconds << "ms";
        }
        log << "]";
    };
    logTopSamples("cpu", g_profiler.CpuSamples());
    logTopSamples("gpu", g_profiler.GpuSamples());
    log << "\n";
}
