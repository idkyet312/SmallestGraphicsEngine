#include "LevelEditor.h"

#include "CameraDX12.h"
#include <imgui.h>
#include <ImGuizmo.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#pragma comment(lib, "comdlg32.lib")

using namespace DirectX;

namespace {

XMMATRIX EntityMatrix(const LevelEntity& entity) {
    const auto& t = entity.transform;
    return XMMatrixScaling(t.scale[0], t.scale[1], t.scale[2]) *
        XMMatrixRotationRollPitchYaw(XMConvertToRadians(t.rotation[0]),
                                     XMConvertToRadians(t.rotation[1]),
                                     XMConvertToRadians(t.rotation[2])) *
        XMMatrixTranslation(t.position[0], t.position[1], t.position[2]);
}

float PickRadius(LevelEntityType type) {
    switch (type) {
    case LevelEntityType::WoodHouse:
    case LevelEntityType::MetalHouse: return 5.0f;
    case LevelEntityType::Helicopter: return 5.0f;
    case LevelEntityType::Humvee: return 2.6f;
    case LevelEntityType::Palm: return 1.3f;
    case LevelEntityType::GrassPatch: return 2.0f;
    case LevelEntityType::Dandelion: return 0.55f;
    case LevelEntityType::Rock: return 1.5f;
    default: return 0.9f;
    }
}

bool SupportsScale(LevelEntityType type) {
    return type == LevelEntityType::Palm || type == LevelEntityType::EnemySpawn ||
           type == LevelEntityType::Helicopter || type == LevelEntityType::GrassPatch ||
           type == LevelEntityType::Dandelion || type == LevelEntityType::Rock;
}

bool IsFoliage(LevelEntityType type) {
    return type == LevelEntityType::Palm || type == LevelEntityType::GrassPatch ||
           type == LevelEntityType::Dandelion;
}

ImU32 TypeColor(LevelEntityType type, bool selected) {
    if (selected) return IM_COL32(255, 220, 55, 255);
    switch (type) {
    case LevelEntityType::PlayerSpawn: return IM_COL32(70, 220, 255, 230);
    case LevelEntityType::EnemySpawn: return IM_COL32(255, 75, 75, 230);
    case LevelEntityType::WoodHouse:
    case LevelEntityType::MetalHouse: return IM_COL32(255, 170, 70, 230);
    case LevelEntityType::Palm: return IM_COL32(70, 220, 100, 230);
    case LevelEntityType::ExplosiveBarrel: return IM_COL32(255, 80, 30, 230);
    case LevelEntityType::Humvee: return IM_COL32(90, 180, 90, 230);
    case LevelEntityType::Helicopter: return IM_COL32(170, 130, 255, 230);
    case LevelEntityType::GrassPatch: return IM_COL32(95, 210, 70, 180);
    case LevelEntityType::Dandelion: return IM_COL32(35, 170, 75, 220);
    case LevelEntityType::Rock: return IM_COL32(145, 145, 135, 230);
    }
    return IM_COL32_WHITE;
}

std::string SanitizeFileName(std::string value) {
    for (char& c : value) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (!(std::isalnum(u) || c == '-' || c == '_')) c = '_';
    }
    while (!value.empty() && value.back() == '_') value.pop_back();
    return value.empty() ? "untitled_level" : value;
}

} // namespace

void LevelEditor::NewFromLevelOne() {
    level_ = MakeLevelOneTemplate();
    strncpy_s(levelName_, level_.name.c_str(), _TRUNCATE);
    selectedId_ = level_.entities.empty() ? 0 : level_.entities.front().id;
    nextId_ = 1;
    for (const auto& entity : level_.entities) nextId_ = (std::max)(nextId_, entity.id + 1);
    undo_.clear(); redo_.clear(); currentPath_.clear();
    dirty_ = false; runtimeDirty_ = true; foliageRuntimeDirty_ = true;
    terrainRuntimeDirty_ = true; playing_ = false;
    status_ = "New level created from Level 1";
    RefreshLevelFiles();
}

LevelEntity* LevelEditor::Selected() {
    auto it = std::find_if(level_.entities.begin(), level_.entities.end(),
        [&](const LevelEntity& e) { return e.id == selectedId_; });
    return it == level_.entities.end() ? nullptr : &*it;
}

const LevelEntity* LevelEditor::Selected() const {
    auto it = std::find_if(level_.entities.begin(), level_.entities.end(),
        [&](const LevelEntity& e) { return e.id == selectedId_; });
    return it == level_.entities.end() ? nullptr : &*it;
}

void LevelEditor::PushUndo(const LevelDefinition& before) {
    undo_.push_back(before);
    if (undo_.size() > 100) undo_.erase(undo_.begin());
    redo_.clear();
}

void LevelEditor::MarkChanged(const LevelDefinition& before) {
    PushUndo(before);
    dirty_ = true;
    runtimeDirty_ = true;
    foliageRuntimeDirty_ = foliageRuntimeDirty_ || FoliageChanged(before);
    terrainRuntimeDirty_ = terrainRuntimeDirty_ || TerrainChanged(before);
}

void LevelEditor::TrackItemEdit(const LevelDefinition& before, bool changed) {
    if (ImGui::IsItemActivated()) {
        inspectorBefore_ = before;
        inspectorEditing_ = true;
    }
    if (changed) {
        dirty_ = true;
        runtimeDirty_ = true;
        foliageRuntimeDirty_ = foliageRuntimeDirty_ || FoliageChanged(before);
        terrainRuntimeDirty_ = terrainRuntimeDirty_ || TerrainChanged(before);
    }
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        PushUndo(inspectorEditing_ ? inspectorBefore_ : before);
        inspectorEditing_ = false;
    }
}

void LevelEditor::Undo() {
    if (undo_.empty() || playing_) return;
    const LevelDefinition before = level_;
    redo_.push_back(level_);
    level_ = std::move(undo_.back());
    undo_.pop_back();
    if (!Selected() && !level_.entities.empty()) selectedId_ = level_.entities.front().id;
    dirty_ = true; runtimeDirty_ = true;
    foliageRuntimeDirty_ = foliageRuntimeDirty_ || FoliageChanged(before);
    terrainRuntimeDirty_ = terrainRuntimeDirty_ || TerrainChanged(before);
}

void LevelEditor::Redo() {
    if (redo_.empty() || playing_) return;
    const LevelDefinition before = level_;
    undo_.push_back(level_);
    level_ = std::move(redo_.back());
    redo_.pop_back();
    if (!Selected() && !level_.entities.empty()) selectedId_ = level_.entities.front().id;
    dirty_ = true; runtimeDirty_ = true;
    foliageRuntimeDirty_ = foliageRuntimeDirty_ || FoliageChanged(before);
    terrainRuntimeDirty_ = terrainRuntimeDirty_ || TerrainChanged(before);
}

void LevelEditor::AddEntity(LevelEntityType type) {
    const LevelDefinition before = level_;
    LevelEntity entity;
    entity.id = nextId_++;
    entity.type = type;
    entity.name = std::string(LevelEntityTypeName(type)) + " " +
                  std::to_string(entity.id);
    if (type == LevelEntityType::Palm) entity.transform.scale[1] = 12.0f;
    if (type == LevelEntityType::GrassPatch) {
        entity.transform.scale[0] = entity.transform.scale[2] = 2.5f;
    }
    if (type == LevelEntityType::PlayerSpawn || type == LevelEntityType::Humvee ||
        type == LevelEntityType::Helicopter) {
        for (auto& existing : level_.entities)
            if (existing.type == type) existing.enabled = false;
        if (type == LevelEntityType::PlayerSpawn) entity.transform.position[1] = 5.0f;
        else if (type == LevelEntityType::Humvee) entity.transform.position[1] = 3.45f;
        else entity.transform.position[1] = 14.0f;
    }
    level_.entities.push_back(entity);
    selectedId_ = entity.id;
    MarkChanged(before);
}

void LevelEditor::DuplicateSelected() {
    LevelEntity* source = Selected();
    if (!source || source->type == LevelEntityType::PlayerSpawn ||
        source->type == LevelEntityType::Humvee ||
        source->type == LevelEntityType::Helicopter) return;
    const LevelDefinition before = level_;
    LevelEntity copy = *source;
    copy.id = nextId_++;
    copy.name += " Copy";
    copy.transform.position[0] += 1.0f;
    copy.transform.position[2] += 1.0f;
    level_.entities.push_back(copy);
    selectedId_ = copy.id;
    MarkChanged(before);
}

void LevelEditor::DeleteSelected() {
    const LevelEntity* selected = Selected();
    if (!selected || selected->type == LevelEntityType::PlayerSpawn) return;
    const LevelDefinition before = level_;
    level_.entities.erase(std::remove_if(level_.entities.begin(), level_.entities.end(),
        [&](const LevelEntity& entity) { return entity.id == selectedId_; }),
        level_.entities.end());
    selectedId_ = level_.entities.empty() ? 0 : level_.entities.front().id;
    MarkChanged(before);
}

bool LevelEditor::SaveTo(const std::filesystem::path& path) {
    level_.name = levelName_;
    const LevelSaveResult result = SaveLevel(level_, path);
    if (!result.ok) { status_ = "Save failed: " + result.error; return false; }
    currentPath_ = path;
    dirty_ = false;
    status_ = "Saved " + path.string() + ". Available under Custom Levels on main menu.";
    RefreshLevelFiles();
    return true;
}

bool LevelEditor::BrowseSaveAs() {
    std::error_code error;
    std::filesystem::create_directories("levels", error);
    const std::wstring initialDirectory =
        std::filesystem::absolute("levels", error).wstring();
    std::wstring suggested = std::filesystem::path(
        SanitizeFileName(saveName_) + ".json").wstring();
    wchar_t selected[MAX_PATH] = {};
    wcsncpy_s(selected, suggested.c_str(), _TRUNCATE);
    OPENFILENAMEW dialog = {};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = GetActiveWindow();
    dialog.lpstrFilter = L"Level JSON (*.json)\0*.json\0\0";
    dialog.lpstrFile = selected;
    dialog.nMaxFile = static_cast<DWORD>(std::size(selected));
    dialog.lpstrInitialDir = initialDirectory.c_str();
    dialog.lpstrDefExt = L"json";
    dialog.lpstrTitle = L"Save Level As";
    dialog.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST |
        OFN_NOCHANGEDIR | OFN_DONTADDTORECENT;
    if (!GetSaveFileNameW(&dialog)) {
        if (const DWORD code = CommDlgExtendedError())
            status_ = "Save browser failed: " + std::to_string(code);
        return false;
    }
    const std::filesystem::path path(selected);
    strncpy_s(saveName_, path.stem().string().c_str(), _TRUNCATE);
    return SaveTo(path);
}

void LevelEditor::RefreshLevelFiles() {
    levelFiles_.clear();
    std::error_code error;
    std::filesystem::create_directories("levels", error);
    for (std::filesystem::directory_iterator it("levels", error), end;
         !error && it != end; it.increment(error)) {
        if (it->is_regular_file() && it->path().extension() == ".json")
            levelFiles_.push_back(it->path());
    }
    std::sort(levelFiles_.begin(), levelFiles_.end());
    if (loadSelection_ >= static_cast<int>(levelFiles_.size())) loadSelection_ = -1;
}

void LevelEditor::BeginPlay() {
    playSnapshot_ = level_;
    playing_ = true;
}

void LevelEditor::StopPlay() {
    if (!playing_) return;
    level_ = playSnapshot_;
    playing_ = false;
    runtimeDirty_ = true;
}

void LevelEditor::OnKeyDown(unsigned key, bool controlDown) {
    if (playing_) return;
    if (controlDown && key == 'Z') Undo();
    else if (controlDown && key == 'Y') Redo();
    else if (key == 'W') gizmoOperation_ = 0;
    else if (key == 'E') gizmoOperation_ = 1;
    else if (key == 'R') gizmoOperation_ = 2;
    else if (key == 'B') {
        foliageTool_ = foliageTool_ == 1 ? 0 : 1;
        terrainTool_ = 0;
    }
    else if (key == 0x2E) DeleteSelected();
}

bool LevelEditor::FoliageChanged(const LevelDefinition& before) const {
    const auto collect = [](const LevelDefinition& level) {
        std::vector<const LevelEntity*> result;
        for (const LevelEntity& entity : level.entities)
            if (IsFoliage(entity.type)) result.push_back(&entity);
        return result;
    };
    const auto a = collect(before);
    const auto b = collect(level_);
    if (a.size() != b.size()) return true;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i]->id != b[i]->id || a[i]->type != b[i]->type ||
            a[i]->enabled != b[i]->enabled ||
            std::memcmp(&a[i]->transform, &b[i]->transform, sizeof(Transform)) != 0)
            return true;
    }
    return false;
}

bool LevelEditor::TerrainChanged(const LevelDefinition& before) const {
    if (before.terrainHeightScale != level_.terrainHeightScale ||
        before.terrainSculpt.size() != level_.terrainSculpt.size()) return true;
    for (size_t i = 0; i < before.terrainSculpt.size(); ++i) {
        const TerrainSculptStamp& a = before.terrainSculpt[i];
        const TerrainSculptStamp& b = level_.terrainSculpt[i];
        if (a.x != b.x || a.z != b.z || a.radius != b.radius ||
            a.operation != b.operation || a.value != b.value ||
            a.strength != b.strength) return true;
    }
    return false;
}

bool LevelEditor::TerrainPointUnderMouse(CXMMATRIX view, CXMMATRIX projection,
    const std::function<float(float, float)>& terrainHeight, XMFLOAT3& point) const {
    if (!terrainHeight) return false;
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    if (display.x <= 0.0f || display.y <= 0.0f) return false;
    const float x = mouse.x / display.x * 2.0f - 1.0f;
    const float y = 1.0f - mouse.y / display.y * 2.0f;
    const XMMATRIX inverse = XMMatrixInverse(nullptr, view * projection);
    const XMVECTOR origin = XMVector3TransformCoord(XMVectorSet(x, y, 0.0f, 1.0f), inverse);
    const XMVECTOR farPoint = XMVector3TransformCoord(XMVectorSet(x, y, 1.0f, 1.0f), inverse);
    const XMVECTOR direction = XMVector3Normalize(farPoint - origin);
    XMFLOAT3 rayOrigin, rayDirection;
    XMStoreFloat3(&rayOrigin, origin);
    XMStoreFloat3(&rayDirection, direction);
    float previousT = 0.0f;
    float previousDelta = rayOrigin.y - terrainHeight(rayOrigin.x, rayOrigin.z);
    for (float t = 1.0f; t <= 500.0f; t += 1.0f) {
        const float px = rayOrigin.x + rayDirection.x * t;
        const float py = rayOrigin.y + rayDirection.y * t;
        const float pz = rayOrigin.z + rayDirection.z * t;
        const float delta = py - terrainHeight(px, pz);
        if (delta <= 0.0f && previousDelta > 0.0f) {
            float low = previousT, high = t;
            for (int i = 0; i < 10; ++i) {
                const float mid = (low + high) * 0.5f;
                const float mx = rayOrigin.x + rayDirection.x * mid;
                const float my = rayOrigin.y + rayDirection.y * mid;
                const float mz = rayOrigin.z + rayDirection.z * mid;
                if (my > terrainHeight(mx, mz)) low = mid; else high = mid;
            }
            const float hit = (low + high) * 0.5f;
            point.x = rayOrigin.x + rayDirection.x * hit;
            point.z = rayOrigin.z + rayDirection.z * hit;
            point.y = terrainHeight(point.x, point.z);
            return true;
        }
        previousT = t;
        previousDelta = delta;
    }
    return false;
}

void LevelEditor::PaintFoliage(CXMMATRIX view, CXMMATRIX projection,
    const std::function<float(float, float)>& terrainHeight) {
    const bool released = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
    if (released && foliageStrokeActive_) {
        if (foliageStrokeChanged_) {
            PushUndo(foliageStrokeBefore_);
            dirty_ = true;
            runtimeDirty_ = true;
            foliageRuntimeDirty_ = true;
        }
        foliageStrokeActive_ = false;
        foliageStrokeChanged_ = false;
    }
    if (foliageTool_ == 0 || terrainTool_ != 0 ||
        ImGui::GetIO().WantCaptureMouse || ImGuizmo::IsOver()) return;

    XMFLOAT3 hit;
    if (!TerrainPointUnderMouse(view, projection, terrainHeight, hit)) return;
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    XMMATRIX viewProjection = view * projection;
    ImVec2 ring[33];
    int ringCount = 0;
    for (int i = 0; i <= 32; ++i) {
        const float angle = XM_2PI * static_cast<float>(i) / 32.0f;
        const float px = hit.x + std::cos(angle) * brushRadius_;
        const float pz = hit.z + std::sin(angle) * brushRadius_;
        const float py = terrainHeight(px, pz) + 0.04f;
        const XMVECTOR clip = XMVector3Transform(XMVectorSet(px, py, pz, 1.0f), viewProjection);
        const float w = XMVectorGetW(clip);
        if (w <= 0.01f) continue;
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        ring[ringCount++] = ImVec2((XMVectorGetX(clip) / w * 0.5f + 0.5f) * display.x,
            (1.0f - (XMVectorGetY(clip) / w * 0.5f + 0.5f)) * display.y);
    }
    if (ringCount > 1) draw->AddPolyline(ring, ringCount,
        foliageTool_ == 1 ? IM_COL32(90, 255, 90, 230) : IM_COL32(255, 80, 60, 230),
        ImDrawFlags_None, 2.0f);

    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) return;
    if (!foliageStrokeActive_) {
        foliageStrokeActive_ = true;
        foliageStrokeChanged_ = false;
        foliageStrokeBefore_ = level_;
        lastFoliageStamp_ = { 100000.0f, 0.0f, 100000.0f };
    }
    const float dx = hit.x - lastFoliageStamp_.x;
    const float dz = hit.z - lastFoliageStamp_.z;
    if (dx * dx + dz * dz < brushSpacing_ * brushSpacing_) return;
    lastFoliageStamp_ = hit;

    const LevelEntityType type = foliageType_ == 0 ? LevelEntityType::GrassPatch :
        (foliageType_ == 1 ? LevelEntityType::Dandelion : LevelEntityType::Palm);
    if (foliageTool_ == 2) {
        const size_t oldSize = level_.entities.size();
        level_.entities.erase(std::remove_if(level_.entities.begin(), level_.entities.end(),
            [&](const LevelEntity& entity) {
                if (entity.type != type) return false;
                const float ex = entity.transform.position[0] - hit.x;
                const float ez = entity.transform.position[2] - hit.z;
                return ex * ex + ez * ez <= brushRadius_ * brushRadius_;
            }), level_.entities.end());
        foliageStrokeChanged_ = foliageStrokeChanged_ || level_.entities.size() != oldSize;
        if (!Selected()) selectedId_ = 0;
        return;
    }

    auto random01 = [&]() {
        foliageRandom_ = foliageRandom_ * 1664525u + 1013904223u;
        return static_cast<float>((foliageRandom_ >> 8) & 0xFFFFFFu) / 16777216.0f;
    };
    int count = 1;
    if (type == LevelEntityType::Dandelion)
        count = std::clamp(static_cast<int>(std::ceil(brushRadius_ * brushRadius_ *
            brushDensity_ * 0.45f)), 1, 48);
    else if (type == LevelEntityType::Palm)
        count = std::clamp(static_cast<int>(std::ceil(brushRadius_ * brushRadius_ *
            brushDensity_ * 0.055f)), 1, 8);
    for (int i = 0; i < count; ++i) {
        const float angle = random01() * XM_2PI;
        const float distance = type == LevelEntityType::GrassPatch ? 0.0f :
            std::sqrt(random01()) * brushRadius_;
        LevelEntity entity;
        entity.id = nextId_++;
        entity.type = type;
        entity.name = std::string(type == LevelEntityType::Palm ? "Tree" :
            (type == LevelEntityType::Dandelion ? "Dandelion" : "Grass Patch")) + " " +
            std::to_string(entity.id);
        entity.transform.position[0] = hit.x + std::cos(angle) * distance;
        entity.transform.position[2] = hit.z + std::sin(angle) * distance;
        entity.transform.position[1] = terrainHeight(entity.transform.position[0],
            entity.transform.position[2]);
        const float scale = foliageScaleMin_ + random01() *
            ((std::max)(foliageScaleMin_, foliageScaleMax_) - foliageScaleMin_);
        if (type == LevelEntityType::GrassPatch) {
            entity.transform.scale[0] = entity.transform.scale[2] = brushRadius_;
            entity.transform.scale[1] = brushDensity_;
        } else if (type == LevelEntityType::Dandelion) {
            entity.transform.scale[0] = entity.transform.scale[1] =
                entity.transform.scale[2] = scale;
            entity.transform.rotation[1] = random01() * 360.0f;
        } else {
            entity.transform.scale[1] = 12.0f * scale;
            entity.transform.rotation[1] = random01() * 360.0f;
            entity.transform.rotation[2] = (random01() - 0.5f) * 1.4f;
        }
        level_.entities.push_back(std::move(entity));
    }
    foliageStrokeChanged_ = true;
}

void LevelEditor::SculptTerrain(CXMMATRIX view, CXMMATRIX projection,
    const std::function<float(float, float)>& terrainHeight) {
    const bool released = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
    if (released && terrainStrokeActive_) {
        if (terrainStrokeChanged_) {
            PushUndo(terrainStrokeBefore_);
            dirty_ = true;
            runtimeDirty_ = true;
            terrainRuntimeDirty_ = true;
        }
        terrainStrokeActive_ = false;
        terrainStrokeChanged_ = false;
    }
    if (terrainTool_ == 0 || ImGui::GetIO().WantCaptureMouse || ImGuizmo::IsOver())
        return;
    XMFLOAT3 hit;
    if (!TerrainPointUnderMouse(view, projection, terrainHeight, hit)) return;

    ImDrawList* draw = ImGui::GetForegroundDrawList();
    const XMMATRIX viewProjection = view * projection;
    ImVec2 ring[33];
    int ringCount = 0;
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    for (int i = 0; i <= 32; ++i) {
        const float angle = XM_2PI * static_cast<float>(i) / 32.0f;
        const float px = hit.x + std::cos(angle) * terrainBrushRadius_;
        const float pz = hit.z + std::sin(angle) * terrainBrushRadius_;
        const float py = terrainHeight(px, pz) + 0.06f;
        const XMVECTOR clip = XMVector3Transform(
            XMVectorSet(px, py, pz, 1.0f), viewProjection);
        const float w = XMVectorGetW(clip);
        if (w <= 0.01f) continue;
        ring[ringCount++] = ImVec2((XMVectorGetX(clip) / w * 0.5f + 0.5f) * display.x,
            (1.0f - (XMVectorGetY(clip) / w * 0.5f + 0.5f)) * display.y);
    }
    if (ringCount > 1) draw->AddPolyline(ring, ringCount,
        terrainTool_ == 2 ? IM_COL32(255, 95, 70, 235) :
        (terrainTool_ == 3 ? IM_COL32(80, 180, 255, 235) :
                            IM_COL32(230, 190, 70, 235)),
        ImDrawFlags_None, 2.0f);

    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) return;
    if (!terrainStrokeActive_) {
        terrainStrokeActive_ = true;
        terrainStrokeChanged_ = false;
        terrainStrokeBefore_ = level_;
        terrainFlattenHeight_ = hit.y;
        lastTerrainStamp_ = { 100000.0f, 0.0f, 100000.0f };
    }
    const float dx = hit.x - lastTerrainStamp_.x;
    const float dz = hit.z - lastTerrainStamp_.z;
    if (dx * dx + dz * dz < terrainBrushSpacing_ * terrainBrushSpacing_) return;
    if (level_.terrainSculpt.size() >= 256) {
        status_ = "Terrain sculpt limit reached (256 stamps). Undo or Clear Sculpt.";
        return;
    }
    lastTerrainStamp_ = hit;
    TerrainSculptStamp stamp;
    stamp.x = hit.x;
    stamp.z = hit.z;
    stamp.radius = terrainBrushRadius_;
    if (terrainTool_ == 3) {
        stamp.operation = TerrainSculptOperation::Flatten;
        stamp.value = terrainFlattenHeight_;
        stamp.strength = (std::min)(1.0f, terrainBrushStrength_ * 0.35f);
    } else {
        stamp.operation = TerrainSculptOperation::Add;
        stamp.value = terrainTool_ == 2 ? -terrainBrushStrength_ : terrainBrushStrength_;
        stamp.strength = 1.0f;
    }
    level_.terrainSculpt.push_back(stamp);
    terrainStrokeChanged_ = true;
}

void LevelEditor::SelectFromViewport(CXMMATRIX view, CXMMATRIX projection) {
    if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
        ImGui::GetIO().WantCaptureMouse || ImGuizmo::IsOver()) return;
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    if (display.x <= 0.0f || display.y <= 0.0f) return;
    const float x = mouse.x / display.x * 2.0f - 1.0f;
    const float y = 1.0f - mouse.y / display.y * 2.0f;
    XMMATRIX inverse = XMMatrixInverse(nullptr, view * projection);
    XMVECTOR nearPoint = XMVector3TransformCoord(XMVectorSet(x, y, 0.0f, 1.0f), inverse);
    XMVECTOR farPoint = XMVector3TransformCoord(XMVectorSet(x, y, 1.0f, 1.0f), inverse);
    XMVECTOR direction = XMVector3Normalize(farPoint - nearPoint);
    float closest = std::numeric_limits<float>::max();
    uint64_t hitId = 0;
    for (const LevelEntity& entity : level_.entities) {
        if (!entity.enabled) continue;
        XMVECTOR center = XMVectorSet(entity.transform.position[0],
            entity.transform.position[1], entity.transform.position[2], 1.0f);
        XMVECTOR offset = center - nearPoint;
        const float along = XMVectorGetX(XMVector3Dot(offset, direction));
        if (along < 0.0f) continue;
        const XMVECTOR nearest = nearPoint + direction * along;
        const float distanceSq = XMVectorGetX(XMVector3LengthSq(center - nearest));
        const float radius = PickRadius(entity.type);
        if (distanceSq <= radius * radius && along < closest) {
            closest = along; hitId = entity.id;
        }
    }
    if (hitId) selectedId_ = hitId;
}

LevelEditorActions LevelEditor::Render(Camera&, CXMMATRIX view,
    CXMMATRIX projection,
    const std::function<float(float, float)>& terrainHeight) {
    LevelEditorActions actions;
    if (playing_) {
        ImGui::SetNextWindowPos(ImVec2(12, 12), ImGuiCond_Always);
        ImGui::Begin("Playtest", nullptr, ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoCollapse);
        ImGui::TextUnformatted("PLAYTEST");
        if (ImGui::Button("STOP (Esc)")) actions.stopPlay = true;
        ImGui::End();
        return actions;
    }

    ImGuizmo::BeginFrame();
    ImGuizmo::SetOrthographic(false);
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGuizmo::SetRect(0.0f, 0.0f, display.x, display.y);

    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(display.x, 54), ImGuiCond_FirstUseEver);
    ImGui::Begin("Level Editor Toolbar", nullptr, ImGuiWindowFlags_NoCollapse);
    if (ImGui::Button("New From Level 1")) ImGui::OpenPopup("Confirm New Level");
    ImGui::SameLine();
    if (ImGui::Button("Save")) {
        if (currentPath_.empty()) BrowseSaveAs();
        else SaveTo(currentPath_);
    }
    ImGui::SameLine();
    if (ImGui::Button("Save As")) BrowseSaveAs();
    ImGui::SameLine();
    if (ImGui::Button("Load")) { RefreshLevelFiles(); ImGui::OpenPopup("Load Level"); }
    ImGui::SameLine();
    ImGui::BeginDisabled(undo_.empty());
    if (ImGui::Button("Undo")) Undo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(redo_.empty());
    if (ImGui::Button("Redo")) Redo();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("PLAY")) actions.beginPlay = true;
    ImGui::SameLine();
    if (ImGui::Button("Main Menu")) {
        if (dirty_) ImGui::OpenPopup("Unsaved Level");
        else actions.returnToMenu = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s%s", level_.name.c_str(), dirty_ ? " *" : "");

    if (ImGui::BeginPopupModal("Confirm New Level", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(dirty_ ? "Discard unsaved changes?" : "Create fresh Level 1 copy?");
        if (ImGui::Button("Create")) { NewFromLevelOne(); actions.levelChanged = true; ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("Unsaved Level", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Discard unsaved changes and return to menu?");
        if (ImGui::Button("Discard")) { actions.returnToMenu = true; ImGui::CloseCurrentPopup(); }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("Load Level", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        if (dirty_) ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.2f, 1.0f),
            "Loading discards unsaved changes.");
        if (levelFiles_.empty()) ImGui::TextDisabled("No levels/*.json files");
        for (size_t i = 0; i < levelFiles_.size(); ++i) {
            const bool selected = loadSelection_ == static_cast<int>(i);
            if (ImGui::Selectable(levelFiles_[i].filename().string().c_str(), selected))
                loadSelection_ = static_cast<int>(i);
        }
        ImGui::BeginDisabled(loadSelection_ < 0);
        if (ImGui::Button(dirty_ ? "Discard and Load" : "Load")) {
            LevelLoadResult result = LoadLevel(levelFiles_[loadSelection_]);
            if (result.ok) {
                level_ = std::move(result.level); currentPath_ = levelFiles_[loadSelection_];
                strncpy_s(levelName_, level_.name.c_str(), _TRUNCATE);
                selectedId_ = level_.entities.front().id; nextId_ = 1;
                for (const auto& entity : level_.entities) nextId_ = (std::max)(nextId_, entity.id + 1);
                undo_.clear(); redo_.clear(); dirty_ = false; runtimeDirty_ = true;
                foliageRuntimeDirty_ = true;
                terrainRuntimeDirty_ = true;
                status_ = "Loaded " + currentPath_.string(); actions.levelChanged = true;
                ImGui::CloseCurrentPopup();
            } else status_ = "Load failed: " + result.error;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::End();

    ImGui::SetNextWindowPos(ImVec2(10, 70), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(285, display.y - 90), ImGuiCond_FirstUseEver);
    ImGui::Begin("Hierarchy");
    const LevelDefinition levelNameBefore = level_;
    const bool levelNameChanged = ImGui::InputText("Level", levelName_, sizeof(levelName_));
    if (levelNameChanged) level_.name = levelName_;
    TrackItemEdit(levelNameBefore, levelNameChanged);
    for (const LevelEntity& entity : level_.entities) {
        ImGui::PushID(static_cast<int>(entity.id));
        const bool selected = selectedId_ == entity.id;
        if (ImGui::Selectable(entity.name.c_str(), selected)) selectedId_ = entity.id;
        ImGui::PopID();
    }
    ImGui::SeparatorText("Create");
    const LevelEntityType types[] = { LevelEntityType::WoodHouse, LevelEntityType::MetalHouse,
        LevelEntityType::Palm, LevelEntityType::ExplosiveBarrel, LevelEntityType::EnemySpawn,
        LevelEntityType::Humvee, LevelEntityType::Helicopter, LevelEntityType::PlayerSpawn,
        LevelEntityType::GrassPatch, LevelEntityType::Dandelion, LevelEntityType::Rock };
    for (auto type : types) {
        if (ImGui::SmallButton(LevelEntityTypeName(type))) AddEntity(type);
        if (type != LevelEntityType::PlayerSpawn) ImGui::SameLine();
    }
    ImGui::Separator();
    const bool cannotDuplicate = !Selected() ||
        Selected()->type == LevelEntityType::PlayerSpawn ||
        Selected()->type == LevelEntityType::Humvee ||
        Selected()->type == LevelEntityType::Helicopter;
    ImGui::BeginDisabled(cannotDuplicate);
    if (ImGui::Button("Duplicate")) DuplicateSelected();
    ImGui::EndDisabled();
    ImGui::SameLine();
    const bool cannotDelete = !Selected() || Selected()->type == LevelEntityType::PlayerSpawn;
    ImGui::BeginDisabled(cannotDelete);
    if (ImGui::Button("Delete")) DeleteSelected();
    ImGui::EndDisabled();
    if (!status_.empty()) { ImGui::Separator(); ImGui::TextWrapped("%s", status_.c_str()); }
    ImGui::End();

    ImGui::SetNextWindowPos(ImVec2(display.x - 310, 70), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(300, 410), ImGuiCond_FirstUseEver);
    ImGui::Begin("Inspector");
    LevelDefinition terrainBefore = level_;
    const bool terrainChanged = ImGui::DragFloat("Terrain Height",
        &level_.terrainHeightScale, 0.1f, 0.0f, 50.0f);
    TrackItemEdit(terrainBefore, terrainChanged);
    ImGui::Separator();
    LevelEntity* entity = Selected();
    if (entity) {
        LevelDefinition before = level_;
        char entityName[128] = {};
        strncpy_s(entityName, entity->name.c_str(), _TRUNCATE);
        bool changed = ImGui::InputText("Name", entityName, sizeof(entityName));
        if (changed) entity->name = entityName;
        TrackItemEdit(before, changed);
        before = level_;
        changed = ImGui::Checkbox("Enabled", &entity->enabled);
        TrackItemEdit(before, changed);
        before = level_;
        changed = ImGui::DragFloat3("Position", entity->transform.position, 0.1f);
        TrackItemEdit(before, changed);
        if (entity->type == LevelEntityType::WoodHouse ||
            entity->type == LevelEntityType::MetalHouse) {
            before = level_;
            changed = ImGui::DragFloat("Yaw", &entity->transform.rotation[1], 1.0f);
            TrackItemEdit(before, changed);
            ImGui::TextDisabled("House scale locked for destruction physics");
        } else {
            before = level_;
            changed = ImGui::DragFloat3("Rotation", entity->transform.rotation, 1.0f);
            TrackItemEdit(before, changed);
            ImGui::BeginDisabled(!SupportsScale(entity->type));
            before = level_;
            changed = ImGui::DragFloat3("Scale", entity->transform.scale, 0.05f, 0.01f, 100.0f);
            TrackItemEdit(before, changed);
            ImGui::EndDisabled();
            if (!SupportsScale(entity->type))
                ImGui::TextDisabled("Scale locked for gameplay physics");
        }
        ImGui::SeparatorText("Gizmo");
        if (ImGui::RadioButton("Move (W)", gizmoOperation_ == 0)) gizmoOperation_ = 0;
        ImGui::SameLine();
        if (ImGui::RadioButton("Rotate (E)", gizmoOperation_ == 1)) gizmoOperation_ = 1;
        ImGui::SameLine();
        ImGui::BeginDisabled(!SupportsScale(entity->type));
        if (ImGui::RadioButton("Scale (R)", gizmoOperation_ == 2)) gizmoOperation_ = 2;
        ImGui::EndDisabled();
        ImGui::Checkbox("Local space", &localSpace_);
        ImGui::Checkbox("Snap", &snapEnabled_);
        ImGui::Checkbox("Snap to terrain", &terrainSnap_);
        if (gizmoOperation_ == 1)
            ImGui::DragFloat("Degrees", &rotationSnap_, 1.0f, 1.0f, 90.0f);
        else ImGui::DragFloat("Grid", &translationSnap_, 0.1f, 0.05f, 10.0f);
    } else ImGui::TextDisabled("No selection");
    ImGui::End();

    ImGui::SetNextWindowPos(ImVec2(305, 70), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(285, 245), ImGuiCond_FirstUseEver);
    ImGui::Begin("Foliage Painter");
    if (ImGui::RadioButton("Select", &foliageTool_, 0)) terrainTool_ = 0;
    ImGui::SameLine();
    if (ImGui::RadioButton("Paint (B)", &foliageTool_, 1)) terrainTool_ = 0;
    ImGui::SameLine();
    if (ImGui::RadioButton("Erase", &foliageTool_, 2)) terrainTool_ = 0;
    const char* foliageTypes[] = { "Grass", "Dandelion", "Trees" };
    ImGui::Combo("Foliage", &foliageType_, foliageTypes, IM_ARRAYSIZE(foliageTypes));
    ImGui::SliderFloat("Radius", &brushRadius_, 0.5f, 12.0f, "%.1f m");
    ImGui::SliderFloat("Density", &brushDensity_, 0.1f, 3.0f, "%.2f");
    ImGui::SliderFloat("Spacing", &brushSpacing_, 0.25f, 10.0f, "%.2f m");
    ImGui::DragFloatRange2("Random scale", &foliageScaleMin_, &foliageScaleMax_,
        0.01f, 0.2f, 3.0f, "%.2f", "%.2f");
    foliageScaleMax_ = (std::max)(foliageScaleMin_, foliageScaleMax_);
    ImGui::TextWrapped("Hold LMB on terrain. Erase affects selected foliage type. Trees use destructible palms.");
    ImGui::End();

    ImGui::SetNextWindowPos(ImVec2(305, 325), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(285, 225), ImGuiCond_FirstUseEver);
    ImGui::Begin("Terrain Sculpt");
    if (ImGui::RadioButton("Select##terrain", &terrainTool_, 0)) foliageTool_ = 0;
    ImGui::SameLine();
    if (ImGui::RadioButton("Raise", &terrainTool_, 1)) foliageTool_ = 0;
    ImGui::SameLine();
    if (ImGui::RadioButton("Lower", &terrainTool_, 2)) foliageTool_ = 0;
    if (ImGui::RadioButton("Flatten", &terrainTool_, 3)) foliageTool_ = 0;
    ImGui::SliderFloat("Brush radius", &terrainBrushRadius_, 0.5f, 15.0f, "%.1f m");
    ImGui::SliderFloat("Strength", &terrainBrushStrength_, 0.05f, 2.0f, "%.2f");
    ImGui::SliderFloat("Stroke spacing", &terrainBrushSpacing_, 0.2f, 8.0f, "%.2f m");
    ImGui::Text("Stamps: %zu / 256", level_.terrainSculpt.size());
    ImGui::BeginDisabled(level_.terrainSculpt.empty());
    if (ImGui::Button("Clear Sculpt")) {
        const LevelDefinition before = level_;
        level_.terrainSculpt.clear();
        MarkChanged(before);
    }
    ImGui::EndDisabled();
    ImGui::TextWrapped("Hold LMB on terrain. Flatten uses height where stroke starts.");
    ImGui::End();

    entity = Selected();
    if (entity && entity->enabled && foliageTool_ == 0 && terrainTool_ == 0) {
        XMFLOAT4X4 world, viewMatrix, projectionMatrix;
        XMStoreFloat4x4(&world, EntityMatrix(*entity));
        XMStoreFloat4x4(&viewMatrix, view);
        XMStoreFloat4x4(&projectionMatrix, projection);
        ImGuizmo::OPERATION operation = gizmoOperation_ == 0 ? ImGuizmo::TRANSLATE :
            (gizmoOperation_ == 1 ? ImGuizmo::ROTATE : ImGuizmo::SCALE);
        if (!SupportsScale(entity->type) && operation == ImGuizmo::SCALE)
            operation = ImGuizmo::ROTATE;
        float snap[3] = { translationSnap_, translationSnap_, translationSnap_ };
        if (operation == ImGuizmo::ROTATE) snap[0] = snap[1] = snap[2] = rotationSnap_;
        ImGuizmo::Manipulate(&viewMatrix._11, &projectionMatrix._11, operation,
            localSpace_ ? ImGuizmo::LOCAL : ImGuizmo::WORLD, &world._11, nullptr,
            snapEnabled_ ? snap : nullptr);
        const bool usingGizmo = ImGuizmo::IsUsing();
        if (usingGizmo && !gizmoWasUsing_) gizmoBefore_ = level_;
        if (usingGizmo) {
            float translation[3], rotation[3], scale[3];
            ImGuizmo::DecomposeMatrixToComponents(&world._11, translation, rotation, scale);
            std::copy(translation, translation + 3, entity->transform.position);
            if (terrainSnap_ && operation == ImGuizmo::TRANSLATE && terrainHeight &&
                entity->type != LevelEntityType::Helicopter &&
                entity->type != LevelEntityType::PlayerSpawn) {
                float offset = 0.0f;
                if (entity->type == LevelEntityType::ExplosiveBarrel) offset = 0.75f;
                else if (entity->type == LevelEntityType::Humvee) offset = 3.45f;
                entity->transform.position[1] = terrainHeight(
                    entity->transform.position[0], entity->transform.position[2]) + offset;
            }
            if (entity->type == LevelEntityType::WoodHouse ||
                entity->type == LevelEntityType::MetalHouse) {
                entity->transform.rotation[1] = rotation[1];
            } else {
                std::copy(rotation, rotation + 3, entity->transform.rotation);
                if (SupportsScale(entity->type))
                    std::copy(scale, scale + 3, entity->transform.scale);
            }
        } else if (gizmoWasUsing_) MarkChanged(gizmoBefore_);
        gizmoWasUsing_ = usingGizmo;
    }

    SculptTerrain(view, projection, terrainHeight);
    PaintFoliage(view, projection, terrainHeight);
    if (foliageTool_ == 0 && terrainTool_ == 0)
        SelectFromViewport(view, projection);
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    XMMATRIX viewProjection = view * projection;
    for (const LevelEntity& marker : level_.entities) {
        if (!marker.enabled) continue;
        XMVECTOR clip = XMVector3Transform(XMVectorSet(marker.transform.position[0],
            marker.transform.position[1] + 0.8f, marker.transform.position[2], 1.0f),
            viewProjection);
        const float w = XMVectorGetW(clip);
        if (w <= 0.01f) continue;
        const ImVec2 point((XMVectorGetX(clip) / w * 0.5f + 0.5f) * display.x,
            (1.0f - (XMVectorGetY(clip) / w * 0.5f + 0.5f)) * display.y);
        draw->AddCircleFilled(point, selectedId_ == marker.id ? 7.0f : 4.0f,
            TypeColor(marker.type, selectedId_ == marker.id));
    }
    actions.levelChanged = actions.levelChanged || runtimeDirty_;
    return actions;
}
