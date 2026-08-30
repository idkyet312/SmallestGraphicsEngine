#include "LevelEditor.h"

#include "EntityTransform.h"
#include "CameraDX12.h"
#include "TerrainStampLibrary.h"
#include "TerrainStampBake.h"
#include <imgui.h>
#include <ImGuizmo.h>
#include <stb_image.h>
// stb_image_write declares stbi_zlib_compress only alongside its implementation,
// which lives in GLBImporter.cpp. The terrain bake needs the deflate step for
// its 16-bit PNG, so declare the symbol rather than instantiating a second copy
// of the library here.
extern "C" unsigned char* stbi_zlib_compress(
    unsigned char* data, int data_len, int* out_len, int quality);
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#pragma comment(lib, "comdlg32.lib")

using namespace DirectX;

namespace {

// Canonical asset roots, matching what AssetRegistry scans. PrefabRegistry's
// own defaults ("prefabs"/"models") describe a layout this project does not
// use -- refreshing without these scanned two empty directories, so the
// content browser's Prefabs tab came up empty and no prefab could be placed.
const std::filesystem::path kPrefabRoot = "Content/Prefabs";
const std::filesystem::path kModelRoot = "Content/Models";

// assetKindTab_ indexes the AssetKind tab array positionally (0=Model,
// 1=Texture, 2=Audio, 3=Prefab, 4=Level). Favourites is not an AssetKind, so
// it takes the next index after that array.
constexpr int kFavouritesTab = 5;

// The built-in entity types offered by the Hierarchy's Create row. Listed on
// the Favourites tab too, because these are the everyday placeables and hunting
// for them at the bottom of the Hierarchy is the slow part of dressing a level.
// They are engine enum values rather than data-driven prefabs, so they are
// always present and cannot be unstarred.
// Height a built-in type sits at above the terrain under it. Helicopter and
// PlayerSpawn deliberately float and are excluded from terrain snapping
// altogether (they keep whatever AddEntity gave them), which is why they are
// absent here -- callers must test TerrainSnaps() first.
float TerrainSnapOffset(LevelEntityType type) {
    if (type == LevelEntityType::ExplosiveBarrel) return 0.75f;
    if (type == LevelEntityType::Humvee) return 3.45f;
    return 0.0f;
}

// Whether dropping/dragging this type should stick it to the ground. Mirrors
// the gizmo's terrain-snap exclusions so a dropped entity lands exactly where
// dragging it with the gizmo would put it.
bool TerrainSnaps(LevelEntityType type) {
    return type != LevelEntityType::Helicopter &&
           type != LevelEntityType::PlayerSpawn;
}

const LevelEntityType kBuiltInSpawnTypes[] = {
    LevelEntityType::WoodHouse, LevelEntityType::MetalHouse,
    LevelEntityType::Palm, LevelEntityType::ExplosiveBarrel,
    LevelEntityType::EnemySpawn, LevelEntityType::AllySpawn,
    LevelEntityType::Humvee, LevelEntityType::Helicopter,
    LevelEntityType::PlayerSpawn, LevelEntityType::GrassPatch,
    LevelEntityType::Dandelion, LevelEntityType::Rock };

// The complete set of collision shapes the runtime understands. Shared by the
// prefab authoring panel and the per-entity override row so the two can never
// drift, and so neither offers a value the loader would reject.
const char* const kCollisionShapes[] = { "none", "box", "mesh" };

int CollisionShapeIndex(const std::string& shape) {
    for (int i = 0; i < IM_ARRAYSIZE(kCollisionShapes); ++i)
        if (shape == kCollisionShapes[i]) return i;
    return 0;
}

// Cells per side of the heightmap preview drawn inside the placement square.
// 64 matches the 0.5 m/vertex the clipmap's innermost ring renders across a
// 32 m stamp, so the preview shows the relief the terrain will actually
// produce rather than a smoother blob. ~4k quads on the foreground draw list,
// still negligible next to the editor's entity gizmos.
constexpr int kStampPreviewGrid = 64;

// Decodes a stamp PNG down to a kStampPreviewGrid^2 grid of 0..1 heights.
//
// Deliberately separate from TerrainRendererDX12's 256^2 atlas: that lives on
// the render side behind DX12 headers, and the editor only needs a coarse grid
// for the on-screen preview. Box-filtered rather than point-sampled so a thin
// ridge in a 4K source does not vanish between preview taps.
bool LoadStampPreview(const std::string& filename, std::vector<float>& out) {
    out.assign(static_cast<size_t>(kStampPreviewGrid) * kStampPreviewGrid, 0.5f);
    if (!IsTerrainStampFilename(filename)) return false;
    const std::string path = (TerrainStampDirectory() / filename).string();
    int width = 0, height = 0, components = 0;
    // 16-bit for the authored heightmaps; stb widens 8-bit sources for free, so
    // a stamp exported at 8 bits still previews instead of showing nothing.
    stbi_us* pixels = stbi_load_16(path.c_str(), &width, &height,
                                   &components, 1);
    if (!pixels || width <= 0 || height <= 0) {
        if (pixels) stbi_image_free(pixels);
        return false;
    }
    for (int y = 0; y < kStampPreviewGrid; ++y) {
        const int y0 = static_cast<int>(static_cast<int64_t>(y) * height /
                                        kStampPreviewGrid);
        const int y1 = (std::max)(y0 + 1,
            static_cast<int>(static_cast<int64_t>(y + 1) * height /
                             kStampPreviewGrid));
        for (int x = 0; x < kStampPreviewGrid; ++x) {
            const int x0 = static_cast<int>(static_cast<int64_t>(x) * width /
                                            kStampPreviewGrid);
            const int x1 = (std::max)(x0 + 1,
                static_cast<int>(static_cast<int64_t>(x + 1) * width /
                                 kStampPreviewGrid));
            uint64_t sum = 0;
            uint32_t count = 0;
            for (int sy = y0; sy < y1 && sy < height; ++sy) {
                for (int sx = x0; sx < x1 && sx < width; ++sx) {
                    sum += pixels[static_cast<size_t>(sy) * width + sx];
                    ++count;
                }
            }
            out[static_cast<size_t>(y) * kStampPreviewGrid + x] =
                count ? static_cast<float>(sum) / count / 65535.0f : 0.5f;
        }
    }
    stbi_image_free(pixels);
    return true;
}

// Rotation order lives in EntityTransform.h, shared with the runtime, because
// it has to match ImGuizmo's decompose exactly -- see the note there.
XMMATRIX EntityMatrix(const LevelEntity& entity) {
    return EntityWorldMatrix(entity.transform);
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
    case LevelEntityType::Prefab: return 1.5f;
    default: return 0.9f;
    }
}

bool SupportsScale(LevelEntityType type) {
    return type == LevelEntityType::Palm || type == LevelEntityType::EnemySpawn ||
           type == LevelEntityType::AllySpawn ||
           type == LevelEntityType::Helicopter || type == LevelEntityType::GrassPatch ||
           type == LevelEntityType::Dandelion || type == LevelEntityType::Rock ||
           type == LevelEntityType::Prefab;
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
    case LevelEntityType::AllySpawn: return IM_COL32(75, 130, 255, 230);
    case LevelEntityType::WoodHouse:
    case LevelEntityType::MetalHouse: return IM_COL32(255, 170, 70, 230);
    case LevelEntityType::Palm: return IM_COL32(70, 220, 100, 230);
    case LevelEntityType::ExplosiveBarrel: return IM_COL32(255, 80, 30, 230);
    case LevelEntityType::Humvee: return IM_COL32(90, 180, 90, 230);
    case LevelEntityType::Helicopter: return IM_COL32(170, 130, 255, 230);
    case LevelEntityType::GrassPatch: return IM_COL32(95, 210, 70, 180);
    case LevelEntityType::Dandelion: return IM_COL32(35, 170, 75, 220);
    case LevelEntityType::Rock: return IM_COL32(145, 145, 135, 230);
    case LevelEntityType::Prefab: return IM_COL32(90, 185, 255, 230);
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

bool DrawPrefabOverrides(LevelEntity& entity, const PrefabAsset& prefab) {
    bool changed = false;
    const nlohmann::json merged = MergePrefabComponents(prefab.components,
                                                        entity.overrides);
    // collision.shape is always offered, even when the prefab defines no
    // collision component. Every other property describes something the prefab
    // already has, but collision is the one an entity may want to *add*: a
    // prefab authored without it could otherwise never be given per-triangle
    // collision from the level editor, because the row that enables it would
    // never be drawn. The synthesized default matches what the runtime assumes
    // for a prefab with no collision component.
    nlohmann::json effective = merged;
    if (!effective.contains("collision") ||
        !effective.at("collision").is_object() ||
        !effective.at("collision").contains("shape")) {
        effective["collision"]["shape"] =
            prefab.collision.empty() ? std::string("none") : prefab.collision;
    }

    const char* currentComponent = nullptr;
    for (const PrefabPropertyDescriptor& property : PrefabPropertyMetadata()) {
        if (!effective.contains(property.component) ||
            !effective.at(property.component).is_object() ||
            !effective.at(property.component).contains(property.field)) continue;
        if (!currentComponent || std::strcmp(currentComponent,
                                              property.component) != 0) {
            currentComponent = property.component;
            ImGui::SeparatorText(currentComponent);
        }
        ImGui::PushID(property.component);
        ImGui::PushID(property.field);
        bool overridden = entity.overrides.contains(property.component) &&
            entity.overrides.at(property.component).is_object() &&
            entity.overrides.at(property.component).contains(property.field);
        if (ImGui::Checkbox("##override", &overridden)) {
            if (overridden)
                entity.overrides[property.component][property.field] =
                    effective.at(property.component).at(property.field);
            else {
                entity.overrides[property.component].erase(property.field);
                if (entity.overrides[property.component].empty())
                    entity.overrides.erase(property.component);
            }
            changed = true;
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!overridden);
        nlohmann::json& value = overridden
            ? entity.overrides[property.component][property.field]
            : const_cast<nlohmann::json&>(
                effective.at(property.component).at(property.field));
        try {
            if (property.type == PrefabPropertyType::Boolean) {
                bool edited = value.get<bool>();
                if (ImGui::Checkbox(property.field, &edited) && overridden) {
                    value = edited; changed = true;
                }
            } else if (property.type == PrefabPropertyType::Number) {
                float edited = value.get<float>();
                if (ImGui::DragFloat(property.field, &edited, 0.05f,
                        property.minimum, property.maximum) && overridden) {
                    value = edited; changed = true;
                }
            } else if (property.type == PrefabPropertyType::Integer) {
                int edited = value.get<int>();
                if (ImGui::DragInt(property.field, &edited, 1.0f,
                        static_cast<int>(property.minimum),
                        static_cast<int>(property.maximum)) && overridden) {
                    value = edited; changed = true;
                }
            } else if (property.type == PrefabPropertyType::Color3) {
                float edited[3] = { value.at(0).get<float>(),
                    value.at(1).get<float>(), value.at(2).get<float>() };
                if (ImGui::ColorEdit3(property.field, edited) && overridden) {
                    value = { edited[0], edited[1], edited[2] }; changed = true;
                }
            } else if (std::strcmp(property.component, "collision") == 0 &&
                       std::strcmp(property.field, "shape") == 0) {
                // A three-value enum typed by hand invites silent typos: the
                // loader falls back to the bounds box for anything it does not
                // recognise, so "Mesh" or "mesh " would look like mesh collision
                // simply failing to work.
                int index = CollisionShapeIndex(value.get<std::string>());
                if (ImGui::Combo(property.field, &index, kCollisionShapes,
                                 IM_ARRAYSIZE(kCollisionShapes)) && overridden) {
                    value = kCollisionShapes[index]; changed = true;
                }
            } else {
                char edited[260] = {};
                strncpy_s(edited, value.get<std::string>().c_str(), _TRUNCATE);
                if (ImGui::InputText(property.field, edited, sizeof(edited)) &&
                    overridden) {
                    value = edited; changed = true;
                }
            }
        } catch (...) {
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f),
                "%s has invalid type", property.field);
        }
        ImGui::EndDisabled();
        ImGui::PopID();
        ImGui::PopID();
    }
    return changed;
}

} // namespace

void LevelEditor::NewFromLevelOne() {
    level_ = MakeLevelOneTemplate();
    strncpy_s(levelName_, level_.name.c_str(), _TRUNCATE);
    selectedId_ = level_.entities.empty() ? 0 : level_.entities.front().id;
    nextId_ = 1;
    for (const auto& entity : level_.entities) nextId_ = (std::max)(nextId_, entity.id + 1);
    for (const auto& spline : level_.splines)
        nextId_ = (std::max)(nextId_, spline.id + 1);
    undo_.clear(); redo_.clear(); currentPath_.clear();
    dirty_ = false; runtimeDirty_ = true; foliageRuntimeDirty_ = true;
    transformRuntimeDirty_ = false; transformRuntimeEntityId_ = 0;
    terrainRuntimeDirty_ = true; dxrDDGIRuntimeDirty_ = true;
    environmentRuntimeDirty_ = true;
    dxrDDGILayoutDirty_ = true; playing_ = false;
    splinesNeedBake_ = true;
    status_ = "New level created from Level 1";
    RefreshLevelFiles();
    prefabRegistry_.Refresh(kPrefabRoot, kModelRoot);
    assetRegistry_.Refresh();
}

void LevelEditor::NewFlat() {
    level_ = MakeFlatLevelTemplate();
    strncpy_s(levelName_, level_.name.c_str(), _TRUNCATE);
    selectedId_ = level_.entities.empty() ? 0 : level_.entities.front().id;
    nextId_ = 1;
    for (const auto& entity : level_.entities)
        nextId_ = (std::max)(nextId_, entity.id + 1);
    // Spline ids share the entity id space: a baked segment records its
    // owner id, so a collision would make a re-bake delete the wrong run.
    for (const auto& spline : level_.splines)
        nextId_ = (std::max)(nextId_, spline.id + 1);
    undo_.clear(); redo_.clear(); currentPath_.clear();
    dirty_ = false; runtimeDirty_ = true; foliageRuntimeDirty_ = true;
    transformRuntimeDirty_ = false; transformRuntimeEntityId_ = 0;
    terrainRuntimeDirty_ = true; dxrDDGIRuntimeDirty_ = true;
    environmentRuntimeDirty_ = true;
    dxrDDGILayoutDirty_ = true; playing_ = false;
    splinesNeedBake_ = true;
    status_ = "New flat level created";
    RefreshLevelFiles();
    prefabRegistry_.Refresh(kPrefabRoot, kModelRoot);
    assetRegistry_.Refresh();
}

void LevelEditor::RefreshAssets() {
    assetRegistry_.Refresh();
    prefabRegistry_.Refresh(kPrefabRoot, kModelRoot);
}

bool LevelEditor::IsFavouritePrefab(const std::string& prefabId) const {
    return std::find(favouritePrefabs_.begin(), favouritePrefabs_.end(),
                     prefabId) != favouritePrefabs_.end();
}

void LevelEditor::ToggleFavouritePrefab(const std::string& prefabId) {
    if (prefabId.empty()) return;
    const auto it = std::find(favouritePrefabs_.begin(),
                              favouritePrefabs_.end(), prefabId);
    if (it == favouritePrefabs_.end()) favouritePrefabs_.push_back(prefabId);
    else favouritePrefabs_.erase(it);
    SaveFavourites();
}

// Favourites are a UI preference, not level data, so they live beside the asset
// cache rather than in the level file -- starring a prefab must not mark the
// level dirty or end up in someone else's level when it is shared.
void LevelEditor::LoadFavourites() {
    favouritePrefabs_.clear();
    try {
        std::ifstream stream("assetcache/favourites.json");
        if (!stream) return;
        nlohmann::json root;
        stream >> root;
        if (!root.is_object()) return;
        const auto found = root.find("prefabs");
        if (found == root.end() || !found->is_array()) return;
        for (const nlohmann::json& item : *found)
            if (item.is_string()) favouritePrefabs_.push_back(item.get<std::string>());
    } catch (const std::exception&) {
        // A corrupt or half-written favourites file must never stop the editor
        // opening; losing the stars is recoverable, not being able to edit is not.
        favouritePrefabs_.clear();
    }
}

void LevelEditor::SaveFavourites() const {
    try {
        std::error_code error;
        std::filesystem::create_directories("assetcache", error);
        nlohmann::json root = nlohmann::json::object();
        root["schemaVersion"] = 1u;
        root["prefabs"] = favouritePrefabs_;
        std::ofstream stream("assetcache/favourites.json");
        if (stream) stream << root.dump(2);
    } catch (const std::exception&) {
        // Best-effort: a failed write costs the user their stars next session,
        // which is not worth interrupting an edit over.
    }
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

void LevelEditor::MarkTransformRuntimeDirty(uint64_t entityId) {
    if (entityId == 0) return;
    if (!runtimeDirty_) {
        runtimeDirty_ = true;
        transformRuntimeDirty_ = true;
        transformRuntimeEntityId_ = entityId;
        return;
    }
    if (!transformRuntimeDirty_) return;
    if (transformRuntimeEntityId_ != entityId) {
        transformRuntimeDirty_ = false;
        transformRuntimeEntityId_ = 0;
    }
}

void LevelEditor::MarkChanged(const LevelDefinition& before,
                              uint64_t transformEntityId) {
    PushUndo(before);
    dirty_ = true;
    if (transformEntityId != 0) {
        MarkTransformRuntimeDirty(transformEntityId);
    } else {
        runtimeDirty_ = true;
        transformRuntimeDirty_ = false;
        transformRuntimeEntityId_ = 0;
    }
    foliageRuntimeDirty_ = foliageRuntimeDirty_ || FoliageChanged(before);
    terrainRuntimeDirty_ = terrainRuntimeDirty_ || TerrainChanged(before);
    environmentRuntimeDirty_ =
        environmentRuntimeDirty_ || EnvironmentChanged(before);
}

void LevelEditor::TrackItemEdit(const LevelDefinition& before, bool changed,
                                uint64_t transformEntityId) {
    if (ImGui::IsItemActivated()) {
        inspectorBefore_ = before;
        inspectorEditing_ = true;
    }
    if (changed) {
        dirty_ = true;
        if (transformEntityId != 0) {
            MarkTransformRuntimeDirty(transformEntityId);
        } else {
            runtimeDirty_ = true;
            transformRuntimeDirty_ = false;
            transformRuntimeEntityId_ = 0;
        }
        foliageRuntimeDirty_ = foliageRuntimeDirty_ || FoliageChanged(before);
        terrainRuntimeDirty_ = terrainRuntimeDirty_ || TerrainChanged(before);
        environmentRuntimeDirty_ =
            environmentRuntimeDirty_ || EnvironmentChanged(before);
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
    transformRuntimeDirty_ = false; transformRuntimeEntityId_ = 0;
    foliageRuntimeDirty_ = foliageRuntimeDirty_ || FoliageChanged(before);
    terrainRuntimeDirty_ = terrainRuntimeDirty_ || TerrainChanged(before);
    environmentRuntimeDirty_ =
        environmentRuntimeDirty_ || EnvironmentChanged(before);
}

void LevelEditor::Redo() {
    if (redo_.empty() || playing_) return;
    const LevelDefinition before = level_;
    undo_.push_back(level_);
    level_ = std::move(redo_.back());
    redo_.pop_back();
    if (!Selected() && !level_.entities.empty()) selectedId_ = level_.entities.front().id;
    dirty_ = true; runtimeDirty_ = true;
    transformRuntimeDirty_ = false; transformRuntimeEntityId_ = 0;
    foliageRuntimeDirty_ = foliageRuntimeDirty_ || FoliageChanged(before);
    terrainRuntimeDirty_ = terrainRuntimeDirty_ || TerrainChanged(before);
    environmentRuntimeDirty_ =
        environmentRuntimeDirty_ || EnvironmentChanged(before);
}

LevelEditor::AssetFileChange LevelEditor::CaptureAssetBefore(
        const std::filesystem::path& path) const {
    AssetFileChange change;
    change.path = path;
    change.beforeExists = std::filesystem::is_regular_file(path);
    if (change.beforeExists) {
        std::ifstream stream(path, std::ios::binary);
        change.before.assign(std::istreambuf_iterator<char>(stream), {});
    }
    return change;
}

void LevelEditor::FinishAssetChange(AssetFileChange change) {
    change.afterExists = std::filesystem::is_regular_file(change.path);
    if (change.afterExists) {
        std::ifstream stream(change.path, std::ios::binary);
        change.after.assign(std::istreambuf_iterator<char>(stream), {});
    }
    assetUndo_.push_back({ std::move(change) });
    if (assetUndo_.size() > 50) assetUndo_.erase(assetUndo_.begin());
    assetRedo_.clear();
}

void LevelEditor::UndoAsset() {
    if (assetUndo_.empty() || playing_) return;
    auto operation = std::move(assetUndo_.back());
    assetUndo_.pop_back();
    for (const AssetFileChange& change : operation) {
        if (!change.beforeExists) {
            std::error_code error;
            std::filesystem::remove(change.path, error);
        } else {
            if (change.path.has_parent_path())
                std::filesystem::create_directories(change.path.parent_path());
            std::ofstream stream(change.path, std::ios::binary | std::ios::trunc);
            stream.write(change.before.data(),
                         static_cast<std::streamsize>(change.before.size()));
        }
    }
    assetRedo_.push_back(std::move(operation));
    RefreshAssets();
    status_ = "Undid asset change";
}

void LevelEditor::RedoAsset() {
    if (assetRedo_.empty() || playing_) return;
    auto operation = std::move(assetRedo_.back());
    assetRedo_.pop_back();
    for (const AssetFileChange& change : operation) {
        if (!change.afterExists) {
            std::error_code error;
            std::filesystem::remove(change.path, error);
        } else {
            if (change.path.has_parent_path())
                std::filesystem::create_directories(change.path.parent_path());
            std::ofstream stream(change.path, std::ios::binary | std::ios::trunc);
            stream.write(change.after.data(),
                         static_cast<std::streamsize>(change.after.size()));
        }
    }
    assetUndo_.push_back(std::move(operation));
    RefreshAssets();
    status_ = "Redid asset change";
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
    if (type == LevelEntityType::PlayerSpawn ||
        type == LevelEntityType::Helicopter) {
        for (auto& existing : level_.entities)
            if (existing.type == type) existing.enabled = false;
        if (type == LevelEntityType::PlayerSpawn) entity.transform.position[1] = 5.0f;
        else entity.transform.position[1] = 14.0f;
    } else if (type == LevelEntityType::Humvee) {
        entity.transform.position[1] = 3.45f;
    }
    level_.entities.push_back(entity);
    selectedId_ = entity.id;
    MarkChanged(before);
}

void LevelEditor::AddPrefab(const PrefabAsset& prefab, const Camera& camera,
        const std::function<float(float, float)>& terrainHeight) {
    if (!prefab.error.empty()) { status_ = prefab.error; return; }
    const LevelDefinition before = level_;
    LevelEntity entity;
    entity.id = nextId_++;
    entity.type = LevelEntityType::Prefab;
    entity.prefabId = prefab.id;
    entity.name = prefab.name;
    entity.transform.position[0] = camera.Position.x + camera.Front.x * 5.0f;
    entity.transform.position[2] = camera.Position.z + camera.Front.z * 5.0f;
    entity.transform.position[1] = terrainHeight
        ? terrainHeight(entity.transform.position[0], entity.transform.position[2])
        : 0.0f;
    std::copy(prefab.defaultScale, prefab.defaultScale + 3, entity.transform.scale);
    level_.entities.push_back(std::move(entity));
    selectedId_ = level_.entities.back().id;
    MarkChanged(before);
    status_ = "Added prefab " + prefab.name;
}

void LevelEditor::DuplicateSelected() {
    LevelEntity* source = Selected();
    if (!source || source->type == LevelEntityType::PlayerSpawn ||
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
    std::filesystem::create_directories("Content/Levels", error);
    const std::wstring initialDirectory =
        std::filesystem::absolute("Content/Levels", error).wstring();
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

bool LevelEditor::BrowseImportModel() {
    wchar_t selected[MAX_PATH] = {};
    OPENFILENAMEW dialog = {};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = GetActiveWindow();
    dialog.lpstrFilter = L"3D Models (*.fbx;*.glb;*.gltf)\0*.fbx;*.glb;*.gltf\0\0";
    dialog.lpstrFile = selected;
    dialog.nMaxFile = static_cast<DWORD>(std::size(selected));
    dialog.lpstrTitle = L"Import Model as Prefab";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST |
        OFN_NOCHANGEDIR | OFN_DONTADDTORECENT;
    if (!GetOpenFileNameW(&dialog)) {
        if (const DWORD code = CommDlgExtendedError())
            status_ = "Import browser failed: " + std::to_string(code);
        return false;
    }
    const std::filesystem::path source(selected);
    std::string safeStem = source.stem().string();
    for (char& c : safeStem) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (!(std::isalnum(u) || c == '-' || c == '_')) c = '_';
    }
    if (safeStem.empty()) safeStem = "imported_model";
    const std::filesystem::path importDirectory =
        std::filesystem::path("Content/Models/Imported") / safeStem;
    pendingImportChanges_.clear();
    pendingImportChanges_.push_back(CaptureAssetBefore(
        std::filesystem::path("Content/Prefabs/Imported") / (safeStem + ".json")));
    pendingImportChanges_.push_back(CaptureAssetBefore(
        importDirectory / source.filename()));
    const std::vector<std::string> sidecars = {
        ".png", ".jpg", ".jpeg", ".tga", ".dds", ".bmp", ".bin", ".mtl" };
    std::error_code scanError;
    for (std::filesystem::directory_iterator it(source.parent_path(), scanError), end;
         !scanError && it != end; it.increment(scanError)) {
        std::string extension = it->path().extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (std::find(sidecars.begin(), sidecars.end(), extension) != sidecars.end())
            pendingImportChanges_.push_back(CaptureAssetBefore(
                importDirectory / it->path().filename()));
    }
    pendingImport_ = std::async(std::launch::async, [source]() {
        PendingImportResult output;
        output.result = PrefabRegistry::ImportModel(source, output.savedPrefab);
        if (output.result.ok) {
            output.assetRegistry.Refresh(true);
            output.prefabRegistry.Refresh(kPrefabRoot, kModelRoot);
        }
        return output;
    });
    status_ = "Importing " + source.filename().string() + " in background...";
    return true;
}

bool LevelEditor::LoadFrom(const std::filesystem::path& path) {
    LevelLoadResult result = LoadLevel(path);
    if (!result.ok) {
        status_ = "Load failed: " + result.error;
        return false;
    }
    level_ = std::move(result.level);
    currentPath_ = path;
    strncpy_s(levelName_, level_.name.c_str(), _TRUNCATE);
    // A level with no entities would make front() undefined behaviour. Levels
    // always author a PlayerSpawn, but a hand-edited file need not.
    selectedId_ = level_.entities.empty() ? 0 : level_.entities.front().id;
    nextId_ = 1;
    for (const auto& entity : level_.entities)
        nextId_ = (std::max)(nextId_, entity.id + 1);
    // Spline ids share the entity id space: a baked segment records its
    // owner id, so a collision would make a re-bake delete the wrong run.
    for (const auto& spline : level_.splines)
        nextId_ = (std::max)(nextId_, spline.id + 1);
    undo_.clear();
    redo_.clear();
    dirty_ = false;
    runtimeDirty_ = true;
    transformRuntimeDirty_ = false;
    transformRuntimeEntityId_ = 0;
    foliageRuntimeDirty_ = true;
    terrainRuntimeDirty_ = true;
    environmentRuntimeDirty_ = true;
    // Segments are not persisted; regenerate them once Render supplies a
    // terrain sampler.
    splinesNeedBake_ = true;
    status_ = "Loaded " + currentPath_.string();
    return true;
}

void LevelEditor::RefreshLevelFiles() {
    levelFiles_.clear();
    std::error_code error;
    std::filesystem::create_directories("Content/Levels", error);
    for (std::filesystem::directory_iterator it("Content/Levels", error), end;
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
    transformRuntimeDirty_ = false;
    transformRuntimeEntityId_ = 0;
}

void LevelEditor::OnKeyDown(unsigned key, bool controlDown) {
    if (playing_) return;
    if (controlDown && key == 'Z') Undo();
    else if (controlDown && key == 'Y') Redo();
    else if (controlDown && key == 'D') DuplicateSelected();
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
    // Cleared areas change what the scatter produces, so they must trigger the
    // same rebuild as adding or removing a foliage entity.
    if (before.foliageClear.size() != level_.foliageClear.size()) return true;
    for (size_t i = 0; i < before.foliageClear.size(); ++i) {
        const FoliageClearStamp& a = before.foliageClear[i];
        const FoliageClearStamp& b = level_.foliageClear[i];
        if (a.x != b.x || a.z != b.z || a.radius != b.radius) return true;
    }
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

// True when an edit touched something RebuildScalableEnvironment reads. The bit
// remains latched while authoring and is consumed at Save/Play; viewport preview
// synchronization never consumes it.
//
// Deliberately WIDER than FoliageChanged(): the environment build also takes
// houses, humvees, rocks and prefab colliders as navmesh obstacles and grass
// exclusions, none of which are IsFoliage() types. Under-reporting here would
// silently leave enemies pathing through a newly placed building, so anything
// the rebuild reads is listed. PlayerSpawn / EnemySpawn / AllySpawn /
// ExplosiveBarrel / Helicopter contribute nothing to it.
bool LevelEditor::EnvironmentChanged(const LevelDefinition& before) const {
    if (FoliageChanged(before) || TerrainChanged(before)) return true;

    const auto affectsEnvironment = [](LevelEntityType type) {
        switch (type) {
        case LevelEntityType::WoodHouse:
        case LevelEntityType::MetalHouse:
        case LevelEntityType::Humvee:
        case LevelEntityType::Rock:
        case LevelEntityType::Prefab:
            return true;
        default:
            return false;
        }
    };
    const auto collect = [&](const LevelDefinition& level) {
        std::vector<const LevelEntity*> result;
        for (const LevelEntity& entity : level.entities)
            if (affectsEnvironment(entity.type)) result.push_back(&entity);
        return result;
    };
    const auto a = collect(before);
    const auto b = collect(level_);
    if (a.size() != b.size()) return true;
    for (size_t i = 0; i < a.size(); ++i) {
        if (a[i]->id != b[i]->id || a[i]->type != b[i]->type ||
            a[i]->enabled != b[i]->enabled ||
            a[i]->prefabId != b[i]->prefabId ||
            std::memcmp(&a[i]->transform, &b[i]->transform, sizeof(Transform)) != 0)
            return true;
    }
    return false;
}

bool LevelEditor::TerrainChanged(const LevelDefinition& before) const {
    if (before.terrainHeightScale != level_.terrainHeightScale ||
        before.terrainFlat != level_.terrainFlat ||
        before.terrainTilesX != level_.terrainTilesX ||
        before.terrainTilesZ != level_.terrainTilesZ ||
        before.terrainIslandScaleX != level_.terrainIslandScaleX ||
        before.terrainIslandScaleZ != level_.terrainIslandScaleZ ||
        before.terrainOriginTileX != level_.terrainOriginTileX ||
        before.terrainOriginTileZ != level_.terrainOriginTileZ ||
        // Revision, not the pixel buffer: this runs per frame, and comparing a
        // megabyte of splat texels here would cost more than the paint itself.
        // The counter rides through undo snapshots like any other field.
        before.terrainSplatRevision != level_.terrainSplatRevision ||
        before.terrainSplatResolution != level_.terrainSplatResolution ||
        before.terrainSculpt.size() != level_.terrainSculpt.size()) return true;
    for (size_t i = 0; i < before.terrainSculpt.size(); ++i) {
        const TerrainSculptStamp& a = before.terrainSculpt[i];
        const TerrainSculptStamp& b = level_.terrainSculpt[i];
        if (a.x != b.x || a.z != b.z || a.radius != b.radius ||
            a.operation != b.operation || a.value != b.value ||
            a.strength != b.strength || a.texture != b.texture ||
            a.rotation != b.rotation || a.replace != b.replace ||
            a.baseHeight != b.baseHeight) return true;
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
            transformRuntimeDirty_ = false;
            transformRuntimeEntityId_ = 0;
            foliageRuntimeDirty_ = true;
            environmentRuntimeDirty_ = true;
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
        foliageTool_ == 1 ? IM_COL32(90, 255, 90, 230) :
        (foliageTool_ == 3 ? IM_COL32(235, 170, 60, 230)
                           : IM_COL32(255, 80, 60, 230)),
        ImDrawFlags_None, 2.0f);
    // Show what is already cleared while the tool is active, so the user can
    // see coverage rather than guessing where they have been.
    if (foliageTool_ == 3) {
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        for (const FoliageClearStamp& stamp : level_.foliageClear) {
            ImVec2 disc[25];
            int discCount = 0;
            for (int i = 0; i <= 24; ++i) {
                const float angle = XM_2PI * static_cast<float>(i) / 24.0f;
                const float px = stamp.x + std::cos(angle) * stamp.radius;
                const float pz = stamp.z + std::sin(angle) * stamp.radius;
                const XMVECTOR clip = XMVector3Transform(
                    XMVectorSet(px, terrainHeight(px, pz) + 0.04f, pz, 1.0f),
                    viewProjection);
                const float w = XMVectorGetW(clip);
                if (w <= 0.01f) continue;
                disc[discCount++] = ImVec2(
                    (XMVectorGetX(clip) / w * 0.5f + 0.5f) * display.x,
                    (1.0f - (XMVectorGetY(clip) / w * 0.5f + 0.5f)) * display.y);
            }
            if (discCount > 1)
                draw->AddPolyline(disc, discCount, IM_COL32(235, 170, 60, 110),
                                  ImDrawFlags_None, 1.5f);
        }
    }

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
    // Tool 3: suppress the auto-scattered ground cover. Unlike Erase, which
    // deletes placed entities, this records a persistent circle that the
    // scatter honours -- procedural grass has no entities to delete.
    if (foliageTool_ == 3) {
        // Merge into an existing stamp when the brush is dragged over ground
        // already cleared, instead of stacking hundreds of overlapping circles
        // that would all be tested per blade.
        for (FoliageClearStamp& stamp : level_.foliageClear) {
            const float dx = stamp.x - hit.x;
            const float dz = stamp.z - hit.z;
            const float distance = std::sqrt(dx * dx + dz * dz);
            if (distance + brushRadius_ <= stamp.radius) return;  // covered
            if (distance <= stamp.radius) {
                const float grown = distance + brushRadius_;
                if (grown > stamp.radius) {
                    stamp.radius = grown;
                    foliageStrokeChanged_ = true;
                }
                return;
            }
        }
        if (level_.foliageClear.size() >= kMaxFoliageClearStamps) {
            status_ = "Ground cover clear limit reached (" +
                std::to_string(kMaxFoliageClearStamps) +
                "). Undo or Restore Ground Cover.";
            return;
        }
        FoliageClearStamp stamp;
        stamp.x = hit.x;
        stamp.z = hit.z;
        stamp.radius = brushRadius_;
        level_.foliageClear.push_back(stamp);
        foliageStrokeChanged_ = true;
        return;
    }

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

LevelSplinePath* LevelEditor::ActiveSpline() {
    for (LevelSplinePath& spline : level_.splines)
        if (spline.id == activeSplineId_) return &spline;
    return nullptr;
}

const LevelSplinePath* LevelEditor::ActiveSpline() const {
    for (const LevelSplinePath& spline : level_.splines)
        if (spline.id == activeSplineId_) return &spline;
    return nullptr;
}

void LevelEditor::RebuildSplines(
        const std::function<float(float, float)>& terrainHeight) {
    BakeSplineEntities(level_, nextId_, terrainHeight);
    // A fence run is world geometry, so the same derived state a terrain edit
    // invalidates has to be rebuilt here too.
    runtimeDirty_ = true;
    environmentRuntimeDirty_ = true;
    dxrDDGIRuntimeDirty_ = true;
    dxrDDGILayoutDirty_ = true;
    transformRuntimeDirty_ = false;
    transformRuntimeEntityId_ = 0;
}

void LevelEditor::SplineTool(CXMMATRIX view, CXMMATRIX projection,
    const std::function<float(float, float)>& terrainHeight) {
    // Commit a control-point drag as one undo entry, matching the foliage and
    // terrain stroke pattern.
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && splineDragActive_) {
        PushUndo(splineDragBefore_);
        dirty_ = true;
        RebuildSplines(terrainHeight);
        splineDragActive_ = false;
        splineDragPoint_ = -1;
    }
    if (splineTool_ == 0 || foliageTool_ != 0 || terrainTool_ != 0) return;

    ImDrawList* draw = ImGui::GetForegroundDrawList();
    const XMMATRIX viewProjection = view * projection;
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    // World to screen. Returns false behind the camera, where the projected
    // point would mirror to the wrong side of the screen.
    const auto project = [&](float x, float y, float z, ImVec2& out) {
        const XMVECTOR clip =
            XMVector3Transform(XMVectorSet(x, y, z, 1.0f), viewProjection);
        const float w = XMVectorGetW(clip);
        if (w <= 0.01f) return false;
        out = ImVec2((XMVectorGetX(clip) / w * 0.5f + 0.5f) * display.x,
                     (1.0f - (XMVectorGetY(clip) / w * 0.5f + 0.5f)) * display.y);
        return true;
    };

    // Draw every run, highlighting the active one.
    for (const LevelSplinePath& spline : level_.splines) {
        const bool active = spline.id == activeSplineId_;
        const std::vector<std::array<float, 3>> curve =
            SampleSplineCurve(spline, 12);
        std::vector<ImVec2> screen;
        screen.reserve(curve.size());
        for (const std::array<float, 3>& point : curve) {
            // Lift the overlay off the ground so it stays readable against the
            // terrain, as the foliage brush ring does.
            const float y = (spline.conformToTerrain && terrainHeight)
                ? terrainHeight(point[0], point[2]) + 0.06f
                : point[1] + 0.06f;
            ImVec2 projected;
            if (project(point[0], y, point[2], projected))
                screen.push_back(projected);
        }
        if (screen.size() > 1)
            draw->AddPolyline(screen.data(), (int)screen.size(),
                active ? IM_COL32(90, 200, 255, 235)
                       : IM_COL32(150, 150, 160, 160),
                ImDrawFlags_None, active ? 2.5f : 1.5f);
        for (size_t i = 0; i < spline.points.size(); ++i) {
            const float* position = spline.points[i].position;
            const float y = (spline.conformToTerrain && terrainHeight)
                ? terrainHeight(position[0], position[2]) + 0.06f
                : position[1] + 0.06f;
            ImVec2 projected;
            if (!project(position[0], y, position[2], projected)) continue;
            const bool dragging = active && (int)i == splineDragPoint_;
            draw->AddCircleFilled(projected, dragging ? 7.0f : 5.0f,
                active ? IM_COL32(255, 220, 90, 245)
                       : IM_COL32(140, 140, 150, 170));
            draw->AddCircle(projected, dragging ? 7.0f : 5.0f,
                            IM_COL32(20, 20, 20, 200));
        }
    }

    if (ImGui::GetIO().WantCaptureMouse || ImGuizmo::IsOver()) return;

    XMFLOAT3 hit;
    const bool overTerrain =
        TerrainPointUnderMouse(view, projection, terrainHeight, hit);

    if (splineTool_ == 1) {
        // Draw: each click appends a control point to the active run, creating
        // one on the first click.
        if (overTerrain) {
            ImVec2 preview;
            if (project(hit.x, hit.y + 0.06f, hit.z, preview))
                draw->AddCircle(preview, 6.0f, IM_COL32(90, 255, 140, 230), 0, 2.0f);
        }
        if (overTerrain && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            const LevelDefinition before = level_;
            LevelSplinePath* spline = ActiveSpline();
            if (!spline) {
                LevelSplinePath created;
                created.id = nextId_++;
                created.name =
                    "Spline " + std::to_string(level_.splines.size() + 1);
                created.prefabId = splinePrefabId_;
                created.spacing = splineSpacing_;
                created.yawOffset = splineYawOffset_;
                created.alignToPath = splineAlignToPath_;
                created.conformToTerrain = splineConformToTerrain_;
                created.pitchToSlope = splinePitchToSlope_;
                created.closed = splineClosed_;
                level_.splines.push_back(std::move(created));
                activeSplineId_ = level_.splines.back().id;
                spline = &level_.splines.back();
            }
            LevelSplinePoint point;
            point.position[0] = hit.x;
            point.position[1] = hit.y;
            point.position[2] = hit.z;
            spline->points.push_back(point);
            const size_t placed = spline->points.size();
            PushUndo(before);
            dirty_ = true;
            RebuildSplines(terrainHeight);
            status_ = "Spline points: " + std::to_string(placed);
        }
        return;
    }

    // Edit: grab the nearest control point of the active run and drag it.
    LevelSplinePath* spline = ActiveSpline();
    if (!spline) return;
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        float bestDistance = 14.0f;  // pixels; generous enough to grab easily
        int best = -1;
        for (size_t i = 0; i < spline->points.size(); ++i) {
            const float* position = spline->points[i].position;
            const float y = (spline->conformToTerrain && terrainHeight)
                ? terrainHeight(position[0], position[2]) + 0.06f
                : position[1] + 0.06f;
            ImVec2 projected;
            if (!project(position[0], y, position[2], projected)) continue;
            const float dx = projected.x - mouse.x;
            const float dy = projected.y - mouse.y;
            const float distance = std::sqrt(dx * dx + dy * dy);
            if (distance < bestDistance) {
                bestDistance = distance;
                best = (int)i;
            }
        }
        if (best >= 0) {
            splineDragPoint_ = best;
            splineDragActive_ = true;
            splineDragBefore_ = level_;
        }
    }
    if (splineDragActive_ && splineDragPoint_ >= 0 &&
        splineDragPoint_ < (int)spline->points.size() && overTerrain &&
        ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        float* position = spline->points[(size_t)splineDragPoint_].position;
        position[0] = hit.x;
        position[1] = hit.y;
        position[2] = hit.z;
        // Re-bake live so the run follows the point being dragged. The undo
        // entry was captured once, at drag start.
        BakeSplineEntities(level_, nextId_, terrainHeight);
        runtimeDirty_ = true;
    }
}

namespace {
// Must match TerrainRendererDX12::Params::tileSize and the GPU-safe clamp in
// main.cpp's CurrentTerrainParams (kMaxTilesPerAxis).
constexpr float kEditorTileSize = 8.0f;
constexpr int kEditorMaxTiles = 48;

// World-space min corner of the current tile grid, mirroring the terrain
// shaders: origin = (originTile - tiles/2) * tileSize.
void GridMinCorner(const LevelDefinition& level, float& minX, float& minZ) {
    minX = (static_cast<float>(level.terrainOriginTileX) -
        level.terrainTilesX * 0.5f) * kEditorTileSize;
    minZ = (static_cast<float>(level.terrainOriginTileZ) -
        level.terrainTilesZ * 0.5f) * kEditorTileSize;
}
} // namespace

void LevelEditor::ExtendTerrain(int direction) {
    const LevelDefinition before = level_;
    // Growing +X/+Z just adds a column/row on the max side (origin unchanged,
    // since the min corner stays put). Growing -X/-Z adds on the min side and
    // shifts the origin down by one tile so existing land keeps its world spot.
    switch (direction) {
        case 0: // +X
            if (static_cast<int>(level_.terrainTilesX) >= kEditorMaxTiles) return;
            level_.terrainTilesX += 1;
            break;
        case 1: // -X
            if (static_cast<int>(level_.terrainTilesX) >= kEditorMaxTiles) return;
            level_.terrainTilesX += 1;
            level_.terrainOriginTileX -= 1;
            break;
        case 2: // +Z
            if (static_cast<int>(level_.terrainTilesZ) >= kEditorMaxTiles) return;
            level_.terrainTilesZ += 1;
            break;
        case 3: // -Z
            if (static_cast<int>(level_.terrainTilesZ) >= kEditorMaxTiles) return;
            level_.terrainTilesZ += 1;
            level_.terrainOriginTileZ -= 1;
            break;
        default: return;
    }
    MarkChanged(before);
    status_ = "Terrain extended.";
}

void LevelEditor::ExtendTerrainInteraction(CXMMATRIX view, CXMMATRIX projection) {
    if (terrainTool_ != 4) return;

    float minX, minZ;
    GridMinCorner(level_, minX, minZ);
    const float maxX = minX + level_.terrainTilesX * kEditorTileSize;
    const float maxZ = minZ + level_.terrainTilesZ * kEditorTileSize;
    const XMMATRIX viewProj = view * projection;
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImDrawList* draw = ImGui::GetBackgroundDrawList();

    // Project a world point (y=0 ground plane) to screen. Returns false when the
    // point is at/behind the near plane OR projects far outside the viewport:
    // dividing by a near-zero w yields million-pixel coordinates that overflow
    // ImGui's draw-list clipping and crash. A generous off-screen margin lets
    // partly-visible grid lines still draw, but rejects the degenerate cases.
    auto project = [&](float wx, float wz, ImVec2& out) -> bool {
        const XMVECTOR raw = XMVector4Transform(
            XMVectorSet(wx, 0.0f, wz, 1.0f), viewProj);
        const float w = XMVectorGetW(raw);
        if (w <= 0.05f) return false;   // near/behind camera
        const float ndcX = XMVectorGetX(raw) / w;
        const float ndcY = XMVectorGetY(raw) / w;
        const float sx = (ndcX * 0.5f + 0.5f) * display.x;
        const float sy = (1.0f - (ndcY * 0.5f + 0.5f)) * display.y;
        // Clamp to a bounded off-screen margin so ImGui never sees wild coords.
        const float margin = 8192.0f;
        if (sx < -margin || sx > display.x + margin ||
            sy < -margin || sy > display.y + margin) return false;
        out = ImVec2(sx, sy);
        return true;
    };

    // Draw the grid lines so the tiles are visible (Unreal-style).
    const ImU32 gridCol = IM_COL32(90, 200, 255, 90);
    for (uint32_t i = 0; i <= level_.terrainTilesX; ++i) {
        const float wx = minX + i * kEditorTileSize;
        ImVec2 a, b;
        if (project(wx, minZ, a) && project(wx, maxZ, b))
            draw->AddLine(a, b, gridCol, 1.0f);
    }
    for (uint32_t j = 0; j <= level_.terrainTilesZ; ++j) {
        const float wz = minZ + j * kEditorTileSize;
        ImVec2 a, b;
        if (project(minX, wz, a) && project(maxX, wz, b))
            draw->AddLine(a, b, gridCol, 1.0f);
    }

    // One clickable "add tile" band just past each edge, drawn as a filled strip
    // at the grid's mid-line on that side. Hovering highlights; click extends.
    const float midX = (minX + maxX) * 0.5f;
    const float midZ = (minZ + maxZ) * 0.5f;
    const float t = kEditorTileSize;
    struct Edge { int dir; float cx, cz; const char* label; };
    const Edge edges[4] = {
        { 0, maxX + t * 0.5f, midZ, "+X" },
        { 1, minX - t * 0.5f, midZ, "-X" },
        { 2, midX, maxZ + t * 0.5f, "+Z" },
        { 3, midX, minZ - t * 0.5f, "-Z" },
    };
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const bool blocked = ImGui::GetIO().WantCaptureMouse;
    for (const Edge& e : edges) {
        ImVec2 center;
        if (!project(e.cx, e.cz, center)) continue;
        const float r = 16.0f;
        const bool hovered = !blocked &&
            std::abs(mouse.x - center.x) < r && std::abs(mouse.y - center.y) < r;
        const ImU32 col = hovered ? IM_COL32(120, 255, 140, 235)
                                  : IM_COL32(90, 200, 255, 170);
        draw->AddRectFilled(ImVec2(center.x - r, center.y - r * 0.7f),
                            ImVec2(center.x + r, center.y + r * 0.7f), col, 3.0f);
        draw->AddText(ImVec2(center.x - 7.0f, center.y - 7.0f),
                      IM_COL32(10, 20, 10, 255), e.label);
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
            ExtendTerrain(e.dir);
    }
}

void LevelEditor::SculptTerrain(CXMMATRIX view, CXMMATRIX projection,
    const std::function<float(float, float)>& terrainHeight) {
    const bool released = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
    if (released && terrainStrokeActive_) {
        if (terrainStrokeChanged_) {
            PushUndo(terrainStrokeBefore_);
            dirty_ = true;
            runtimeDirty_ = true;
            transformRuntimeDirty_ = false;
            transformRuntimeEntityId_ = 0;
            terrainRuntimeDirty_ = true;
            environmentRuntimeDirty_ = true;
            dxrDDGIRuntimeDirty_ = true;
            dxrDDGILayoutDirty_ = true;
        }
        terrainStrokeActive_ = false;
        terrainStrokeChanged_ = false;
    }
    // Tools: 1 raise, 2 lower, 3 flatten, 6 heightmap stamp. An allowlist, not
    // a denylist: select/grow/paint must not silently push height stamps.
    if ((terrainTool_ != 1 && terrainTool_ != 2 && terrainTool_ != 3 &&
         terrainTool_ != 6) ||
        ImGui::GetIO().WantCaptureMouse || ImGuizmo::IsOver())
        return;
    XMFLOAT3 hit;
    if (!TerrainPointUnderMouse(view, projection, terrainHeight, hit)) return;

    ImDrawList* draw = ImGui::GetForegroundDrawList();
    const XMMATRIX viewProjection = view * projection;
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const auto project = [&](float px, float pz, ImVec2& screen) {
        const float py = terrainHeight(px, pz) + 0.06f;
        const XMVECTOR clip = XMVector3Transform(
            XMVectorSet(px, py, pz, 1.0f), viewProjection);
        const float w = XMVectorGetW(clip);
        if (w <= 0.01f) return false;
        screen = ImVec2((XMVectorGetX(clip) / w * 0.5f + 0.5f) * display.x,
            (1.0f - (XMVectorGetY(clip) / w * 0.5f + 0.5f)) * display.y);
        return true;
    };
    if (terrainTool_ == 6) {
        const float angle = XMConvertToRadians(terrainStampRotation_);
        const float cosine = std::cos(angle), sine = std::sin(angle);
        // Decode the selected stamp once, not per frame: the cursor moves every
        // frame but the shape only changes when the combo does.
        if (!terrainStampNames_.empty()) {
            const std::string& selected =
                terrainStampNames_[terrainStampSelection_];
            if (selected != stampPreviewName_) {
                stampPreviewName_ = selected;
                stampPreviewValid_ =
                    LoadStampPreview(selected, stampPreviewHeights_);
            }
        } else {
            stampPreviewValid_ = false;
        }

        // Stamp-local (u, v) in 0..1 -> world XZ, sharing the rotation and
        // extent that SampleHeightStamp uses so what is drawn is where the
        // relief actually lands.
        const auto stampToWorld = [&](float u, float v, float& px, float& pz) {
            const float localX = (u * 2.0f - 1.0f) * terrainStampRadius_;
            const float localZ = (v * 2.0f - 1.0f) * terrainStampRadius_;
            px = hit.x + localX * cosine - localZ * sine;
            pz = hit.z + localX * sine + localZ * cosine;
        };
        // Same edge feather as the sculpt sampler, so the preview fades out
        // exactly where the stamp stops affecting the ground.
        const auto edgeAt = [&](float u, float v) {
            float e = ((std::max)(std::abs(u * 2.0f - 1.0f),
                                  std::abs(v * 2.0f - 1.0f)) - 0.82f) / 0.18f;
            e = e < 0.0f ? 0.0f : (e > 1.0f ? 1.0f : e);
            return 1.0f - e * e * (3.0f - 2.0f * e);
        };

        if (stampPreviewValid_) {
            // The preview is drawn at the height the stamp would produce, not
            // flat on the ground: a crater reads as a crater only if the sheet
            // actually dips. Projecting the sculpted height needs its own
            // projector, since `project` deliberately hugs the terrain.
            const auto projectAt = [&](float px, float pz, float py,
                                       ImVec2& screen) {
                const XMVECTOR clip = XMVector3Transform(
                    XMVectorSet(px, py, pz, 1.0f), viewProjection);
                const float w = XMVectorGetW(clip);
                if (w <= 0.01f) return false;
                screen = ImVec2(
                    (XMVectorGetX(clip) / w * 0.5f + 0.5f) * display.x,
                    (1.0f - (XMVectorGetY(clip) / w * 0.5f + 0.5f)) * display.y);
                return true;
            };
            const int grid = kStampPreviewGrid;
            // One height + screen position per grid corner, shared by the four
            // quads that meet there.
            const int stride = grid + 1;
            std::vector<ImVec2> points(static_cast<size_t>(stride) * stride);
            std::vector<uint8_t> valid(points.size(), 0);
            std::vector<float> relief(points.size(), 0.0f);
            for (int gy = 0; gy <= grid; ++gy) {
                for (int gx = 0; gx <= grid; ++gx) {
                    const float u = static_cast<float>(gx) / grid;
                    const float v = static_cast<float>(gy) / grid;
                    float px = 0.0f, pz = 0.0f;
                    stampToWorld(u, v, px, pz);
                    // Cell centres hold the samples; clamp so the outer ring of
                    // corners reuses the nearest cell instead of reading out.
                    const int cx = (std::min)(grid - 1, gx);
                    const int cy = (std::min)(grid - 1, gy);
                    const float normalized = stampPreviewHeights_[
                        static_cast<size_t>(cy) * grid + cx] * 2.0f - 1.0f;
                    const float edge = edgeAt(u, v);
                    const float displacement =
                        normalized * terrainStampHeight_ * edge;
                    const float ground = terrainHeight(px, pz);
                    // Mirror the sculpt blend so the preview shows replace mode
                    // levelling the ground, not just relief floating over it.
                    const float blend = edge * terrainStampReplace_;
                    const float base = hit.y + terrainStampBaseOffset_;
                    const float y = ground + displacement +
                                    (base - ground) * blend;
                    const size_t index =
                        static_cast<size_t>(gy) * stride + gx;
                    relief[index] = y - ground;
                    valid[index] = projectAt(px, pz, y + 0.05f,
                                             points[index]) ? 1u : 0u;
                }
            }
            // Cut-away (below ground) reads red, built-up reads green, and the
            // untouched middle stays the panel's purple, so the sign of the
            // stamp is visible before it is committed.
            const auto shade = [&](float delta) {
                const float scale = (std::max)(1.0f,
                    std::abs(terrainStampHeight_));
                float t = delta / scale;
                t = t < -1.0f ? -1.0f : (t > 1.0f ? 1.0f : t);
                const float up = t > 0.0f ? t : 0.0f;
                const float down = t < 0.0f ? -t : 0.0f;
                return IM_COL32(
                    static_cast<int>(150.0f + 90.0f * down - 60.0f * up),
                    static_cast<int>(95.0f + 150.0f * up - 40.0f * down),
                    static_cast<int>(235.0f - 150.0f * (up + down)),
                    110);
            };
            for (int gy = 0; gy < grid; ++gy) {
                for (int gx = 0; gx < grid; ++gx) {
                    const size_t a = static_cast<size_t>(gy) * stride + gx;
                    const size_t b = a + 1;
                    const size_t c = a + stride + 1;
                    const size_t d = a + stride;
                    if (!valid[a] || !valid[b] || !valid[c] || !valid[d])
                        continue;
                    const float average = (relief[a] + relief[b] + relief[c] +
                                           relief[d]) * 0.25f;
                    draw->AddQuadFilled(points[a], points[b], points[c],
                                        points[d], shade(average));
                }
            }
            // Wireframe over the fill: flat regions have no shading gradient to
            // read, and the lines are what make the surface legible there.
            // Fixed line count, not a fixed step: at grid 64 a step of 3 would
            // draw 22 lines per axis and read as a solid haze over the fill.
            const int wireStep = (std::max)(1, grid / 8);
            for (int g = 0; g <= grid; g += wireStep) {
                for (int step = 0; step < grid; ++step) {
                    const size_t rowA =
                        static_cast<size_t>(g) * stride + step;
                    if (valid[rowA] && valid[rowA + 1])
                        draw->AddLine(points[rowA], points[rowA + 1],
                                      IM_COL32(210, 175, 255, 90), 1.0f);
                    const size_t colA =
                        static_cast<size_t>(step) * stride + g;
                    if (valid[colA] && valid[colA + stride])
                        draw->AddLine(points[colA], points[colA + stride],
                                      IM_COL32(210, 175, 255, 90), 1.0f);
                }
            }
        }

        static constexpr float corners[5][2] = {
            {-1.0f, -1.0f}, {1.0f, -1.0f}, {1.0f, 1.0f},
            {-1.0f, 1.0f}, {-1.0f, -1.0f}
        };
        ImVec2 outline[5];
        int outlineCount = 0;
        for (const auto& corner : corners) {
            const float localX = corner[0] * terrainStampRadius_;
            const float localZ = corner[1] * terrainStampRadius_;
            const float px = hit.x + localX * cosine - localZ * sine;
            const float pz = hit.z + localX * sine + localZ * cosine;
            if (project(px, pz, outline[outlineCount])) ++outlineCount;
        }
        if (outlineCount > 1)
            draw->AddPolyline(outline, outlineCount,
                IM_COL32(185, 110, 255, 235), ImDrawFlags_None, 2.0f);
    } else {
        ImVec2 ring[33];
        int ringCount = 0;
        for (int i = 0; i <= 32; ++i) {
            const float angle = XM_2PI * static_cast<float>(i) / 32.0f;
            const float px = hit.x + std::cos(angle) * terrainBrushRadius_;
            const float pz = hit.z + std::sin(angle) * terrainBrushRadius_;
            if (project(px, pz, ring[ringCount])) ++ringCount;
        }
        if (ringCount > 1) draw->AddPolyline(ring, ringCount,
            terrainTool_ == 2 ? IM_COL32(255, 95, 70, 235) :
            (terrainTool_ == 3 ? IM_COL32(80, 180, 255, 235) :
                                IM_COL32(230, 190, 70, 235)),
            ImDrawFlags_None, 2.0f);
    }

    if (terrainTool_ == 6) {
        if (!ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
            terrainStampNames_.empty()) return;
        if (level_.terrainSculpt.size() >= kMaxTerrainSculptStamps) {
            status_ = "Terrain sculpt limit reached (" +
                std::to_string(kMaxTerrainSculptStamps) +
                " stamps). Undo or Clear Sculpt.";
            return;
        }
        terrainStrokeActive_ = true;
        terrainStrokeChanged_ = true;
        terrainStrokeBefore_ = level_;
        TerrainSculptStamp stamp;
        stamp.x = hit.x;
        stamp.z = hit.z;
        stamp.radius = terrainStampRadius_;
        stamp.operation = TerrainSculptOperation::Heightmap;
        stamp.value = terrainStampHeight_;
        stamp.texture = terrainStampNames_[terrainStampSelection_];
        stamp.rotation = terrainStampRotation_;
        stamp.replace = terrainStampReplace_;
        // Snapshot the ground under the cursor now. Recomputing it later would
        // fold in whatever stamps land afterwards and make this one drift.
        stamp.baseHeight = hit.y + terrainStampBaseOffset_;
        level_.terrainSculpt.push_back(std::move(stamp));
        return;
    }

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
    // The editor deliberately refuses instead of evicting: authored strokes are
    // the user's work, so silently dropping the oldest one would destroy it.
    // Runtime craters (AddExplosionTerrainCrater) are transient and do evict.
    if (level_.terrainSculpt.size() >= kMaxTerrainSculptStamps) {
        status_ = "Terrain sculpt limit reached (" +
            std::to_string(kMaxTerrainSculptStamps) +
            " stamps). Undo or Clear Sculpt.";
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

bool LevelEditor::EnsureTerrainSplatMap() {
    // 512 across the island is ~0.35 m per texel on the default radius: finer
    // than the triplanar blend can resolve, and only 1 MB per undo snapshot.
    constexpr uint32_t kSplatResolution = 512;
    const size_t expected =
        static_cast<size_t>(kSplatResolution) * kSplatResolution * 4u;
    if (level_.terrainSplatResolution == kSplatResolution &&
        level_.terrainSplatRGBA.size() == expected)
        return true;
    // Zero-filled: every texel starts "unpainted", which the resolve reads as
    // "use the procedural weights". Painting is purely additive from there.
    level_.terrainSplatRGBA.assign(expected, 0u);
    level_.terrainSplatResolution = kSplatResolution;
    return true;
}

void LevelEditor::PaintTerrain(CXMMATRIX view, CXMMATRIX projection,
    const std::function<float(float, float)>& terrainHeight,
    float islandHalfExtentX, float islandHalfExtentZ) {
    // Stroke end and undo sit above the tool gate, mirroring SculptTerrain: a
    // stroke that ends after the user switches tools must still be committed.
    const bool released = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
    if (released && terrainStrokeActive_) {
        if (terrainStrokeChanged_) {
            PushUndo(terrainStrokeBefore_);
            dirty_ = true;
            runtimeDirty_ = true;
            transformRuntimeDirty_ = false;
            transformRuntimeEntityId_ = 0;
            terrainRuntimeDirty_ = true;
            // Painted material gates the grass scatter (GrassField::SetSplatMap),
            // so a paint stroke must still rebuild the environment even though
            // it leaves height and DDGI alone.
            environmentRuntimeDirty_ = true;
            // Painting changes surface appearance only -- no height, no
            // occlusion -- so unlike sculpting it does not invalidate the DDGI
            // probe layout. Re-lighting the level on every brush stroke would
            // be a heavy and pointless rebuild.
        }
        terrainStrokeActive_ = false;
        terrainStrokeChanged_ = false;
    }
    if (terrainTool_ != 5 || ImGui::GetIO().WantCaptureMouse ||
        ImGuizmo::IsOver())
        return;
    if (islandHalfExtentX <= 1e-4f || islandHalfExtentZ <= 1e-4f) return;

    XMFLOAT3 hit;
    if (!TerrainPointUnderMouse(view, projection, terrainHeight, hit)) return;

    // Brush ring, tinted by the layer being painted so the target material is
    // readable without consulting the panel.
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
    if (ringCount > 1) {
        static const ImU32 kLayerColors[4] = {
            IM_COL32( 90, 210,  90, 235),   // grass
            IM_COL32(200, 130,  60, 235),   // dirt
            IM_COL32(230, 215, 140, 235),   // sand
            IM_COL32(225, 225, 235, 235)    // rock
        };
        draw->AddPolyline(ring, ringCount,
            kLayerColors[terrainPaintLayer_ & 3], ImDrawFlags_None, 2.0f);
    }

    if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) return;
    if (!terrainStrokeActive_) {
        terrainStrokeActive_ = true;
        terrainStrokeChanged_ = false;
        terrainStrokeBefore_ = level_;
        lastTerrainStamp_ = { 100000.0f, 0.0f, 100000.0f };
    }
    // Squared-distance spacing against the sentinel start, exactly as sculpt
    // does, so the first stamp of a stroke always lands.
    const float dx = hit.x - lastTerrainStamp_.x;
    const float dz = hit.z - lastTerrainStamp_.z;
    if (dx * dx + dz * dz < terrainBrushSpacing_ * terrainBrushSpacing_) return;
    if (!EnsureTerrainSplatMap()) return;
    lastTerrainStamp_ = hit;

    const int resolution = static_cast<int>(level_.terrainSplatResolution);
    // Same world->UV frame as the resolve: u = x / (2 * halfExtent) + 0.5.
    const float invExtentX = 0.5f / islandHalfExtentX;
    const float invExtentZ = 0.5f / islandHalfExtentZ;
    const float centerU = hit.x * invExtentX + 0.5f;
    const float centerV = hit.z * invExtentZ + 0.5f;
    // The brush is a world-space circle, so its texel radius differs per axis
    // whenever the island is stretched (islandScaleX != islandScaleZ).
    const float radiusU = terrainBrushRadius_ * invExtentX * resolution;
    const float radiusV = terrainBrushRadius_ * invExtentZ * resolution;
    if (radiusU < 0.01f || radiusV < 0.01f) return;

    const int minX = (std::max)(0, static_cast<int>(std::floor(
        centerU * resolution - radiusU)));
    const int maxX = (std::min)(resolution - 1, static_cast<int>(std::ceil(
        centerU * resolution + radiusU)));
    const int minY = (std::max)(0, static_cast<int>(std::floor(
        centerV * resolution - radiusV)));
    const int maxY = (std::min)(resolution - 1, static_cast<int>(std::ceil(
        centerV * resolution + radiusV)));

    const int layer = terrainPaintLayer_ & 3;
    bool wrote = false;
    for (int y = minY; y <= maxY; ++y) {
        for (int x = minX; x <= maxX; ++x) {
            // Normalised elliptical distance, so a stretched island still gets
            // a round brush in world space.
            const float ndx = ((x + 0.5f) - centerU * resolution) / radiusU;
            const float ndy = ((y + 0.5f) - centerV * resolution) / radiusV;
            const float distance = std::sqrt(ndx * ndx + ndy * ndy);
            if (distance > 1.0f) continue;
            // Smooth falloff to the rim. The resolve derives coverage from the
            // painted channels, so a feathered edge blends back into the
            // procedural weights rather than cutting a hard boundary.
            const float falloff = 1.0f - distance * distance;
            const float deposit = terrainPaintStrength_ * falloff;
            if (deposit <= 0.0f) continue;
            uint8_t* texel = &level_.terrainSplatRGBA[
                (static_cast<size_t>(y) * resolution + x) * 4u];
            const int target = static_cast<int>(deposit * 255.0f + 0.5f);
            // Accumulate toward the target rather than overwriting, so repeated
            // passes build up like a real brush and a light touch never erases
            // heavier paint already laid down.
            if (target > texel[layer]) {
                texel[layer] = static_cast<uint8_t>(target);
                wrote = true;
            }
            // The painted channels are normalised in the shader, so competing
            // layers must be pulled down or the blend would just average them.
            for (int other = 0; other < 4; ++other) {
                if (other == layer) continue;
                const int reduced = static_cast<int>(
                    texel[other] * (1.0f - deposit));
                if (reduced < texel[other]) {
                    texel[other] = static_cast<uint8_t>(reduced);
                    wrote = true;
                }
            }
        }
    }
    if (wrote) {
        ++level_.terrainSplatRevision;
        terrainStrokeChanged_ = true;
    }
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

LevelEditorActions LevelEditor::Render(Camera& camera, CXMMATRIX view,
    CXMMATRIX projection,
    const std::function<float(float, float)>& terrainHeight,
    const std::function<uint64_t(const PrefabAsset&)>& thumbnailTexture) {
    LevelEditorActions actions;
    // Levels store spline control points, not the segments they produce, so a
    // freshly loaded level has to bake them before anything reads entities.
    if (splinesNeedBake_) {
        splinesNeedBake_ = false;
        if (!level_.splines.empty()) RebuildSplines(terrainHeight);
    }
    if (pendingImport_.valid() && pendingImport_.wait_for(
            std::chrono::seconds(0)) == std::future_status::ready) {
        PendingImportResult imported = pendingImport_.get();
        if (!imported.result.ok)
            status_ = "Import failed: " + imported.result.error;
        else {
            for (AssetFileChange& change : pendingImportChanges_) {
                change.afterExists = std::filesystem::is_regular_file(change.path);
                if (change.afterExists) {
                    std::ifstream stream(change.path, std::ios::binary);
                    change.after.assign(std::istreambuf_iterator<char>(stream), {});
                }
            }
            assetUndo_.push_back(std::move(pendingImportChanges_));
            assetRedo_.clear();
            assetRegistry_ = std::move(imported.assetRegistry);
            prefabRegistry_ = std::move(imported.prefabRegistry);
            selectedPrefab_ = -1;
            const auto& assets = prefabRegistry_.Assets();
            for (size_t i = 0; i < assets.size(); ++i)
                if (assets[i].definitionPath == imported.savedPrefab)
                    selectedPrefab_ = static_cast<int>(i);
            status_ = "Imported model and created " +
                imported.savedPrefab.string();
        }
    }
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
    bool showInsertionRadiusPreview = false;
    ImGuizmo::SetRect(0.0f, 0.0f, display.x, display.y);

    ImGui::SetNextWindowPos(ImVec2(0, 0), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(display.x, 54), ImGuiCond_FirstUseEver);
    ImGui::Begin("Level Editor Toolbar", nullptr, ImGuiWindowFlags_NoCollapse);
    if (ImGui::Button("New From Level 1")) ImGui::OpenPopup("Confirm New Level");
    ImGui::SameLine();
    if (ImGui::Button("New Flat")) ImGui::OpenPopup("Confirm Flat Level");
    ImGui::SameLine();
    if (ImGui::Button("Save")) {
        const bool saved = currentPath_.empty()
            ? BrowseSaveAs()
            : SaveTo(currentPath_);
        actions.fullReconcile |= saved;
    }
    ImGui::SameLine();
    if (ImGui::Button("Save As"))
        actions.fullReconcile |= BrowseSaveAs();
    ImGui::SameLine();
    if (ImGui::Button("Validate Map")) {
        LevelDefinition candidate = level_;
        candidate.name = levelName_;
        LevelValidationResult validation = ValidateLevel(candidate);
        validationErrors_ = std::move(validation.errors);
        status_ = validation.ok
            ? "Map validation passed."
            : "Map validation failed with " +
                std::to_string(validationErrors_.size()) + " issue(s).";
        ImGui::OpenPopup("Map Validation");
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Check whether the current map can be saved and loaded.");
    ImGui::SameLine();
    if (ImGui::Button("Load")) { RefreshLevelFiles(); ImGui::OpenPopup("Load Level"); }
    ImGui::SameLine();
    if (ImGui::Button("Assets")) assetBrowserOpen_ = !assetBrowserOpen_;
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) actions.refreshVisuals = true;
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Refresh all editor visuals, including built-in houses and\n"
            "prefab models. Use this when an item is in the hierarchy\n"
            "but is not drawn in the viewport.");
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
    ImGui::Checkbox("Fog", &fogEnabled_);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Volumetric fog in the editor viewport.\n"
            "A view setting only -- it is not saved with the level and does\n"
            "not change the authored weather. Turn it off to see far props\n"
            "through a dense preset.");
    ImGui::SameLine();
    ImGui::Checkbox("Birdseye", &birdseyeEnabled_);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Deployment-overview terrain LOD in the editor viewport.\n"
            "Uniform 8 m tiles at full tessellation to the horizon instead\n"
            "of the gameplay clipmap's coarser outer rings, so distant\n"
            "terrain reads accurately. Costs more GPU; a view setting only,\n"
            "not saved with the level.");
    ImGui::SameLine();
    ImGui::Checkbox("Navmesh", &navmeshEnabled_);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Walkable navmesh overlay: where enemies can path.\n"
            "Props punch holes in it, so this is how to check that a\n"
            "container or barrack actually blocks the route you think.\n"
            "Shows the mesh as of the last Save, Play or Rebuild -- ordinary\n"
            "edits are preview-only and do not rebuild it.\n"
            "A view setting only; it is not saved with the level.");
    if (navmeshEnabled_) {
        ImGui::SameLine();
        // Reuses the full reconcile the Save/Play boundaries already run: the
        // navmesh has no rebuild of its own, and its obstacle list is gathered
        // from the prefab colliders that reconcile refreshes.
        if (ImGui::Button("Rebuild Navmesh")) actions.fullReconcile = true;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Rebuilds the navmesh so it picks up the props you just moved.\n"
                "This is the full environment reconcile (~1.3 s): navmesh, grass\n"
                "and trees all rebuild, not the navmesh alone.");
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s%s", level_.name.c_str(), dirty_ ? " *" : "");

    if (ImGui::BeginPopupModal("Confirm New Level", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(dirty_ ? "Discard unsaved changes?" : "Create fresh Level 1 copy?");
        if (ImGui::Button("Create")) {
            NewFromLevelOne();
            actions.levelChanged = true;
            actions.fullReconcile = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (ImGui::BeginPopupModal("Confirm Flat Level", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted(dirty_
            ? "Discard unsaved changes and start a flat level?"
            : "Start an empty flat level?");
        ImGui::TextDisabled(
            "Level plane at ground height. No island, coast, pool or props --\n"
            "just a player spawn to sculpt around.");
        if (ImGui::Button("Create")) {
            NewFlat();
            actions.levelChanged = true;
            actions.fullReconcile = true;
            ImGui::CloseCurrentPopup();
        }
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
            if (LoadFrom(levelFiles_[loadSelection_])) {
                actions.levelChanged = true;
                actions.fullReconcile = true;
                ImGui::CloseCurrentPopup();
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    if (!validationErrors_.empty())
        ImGui::SetNextWindowSize(ImVec2(620.0f, 400.0f), ImGuiCond_Appearing);
    const ImGuiWindowFlags validationWindowFlags = validationErrors_.empty()
        ? ImGuiWindowFlags_AlwaysAutoResize : ImGuiWindowFlags_None;
    if (ImGui::BeginPopupModal("Map Validation", nullptr,
                               validationWindowFlags)) {
        if (validationErrors_.empty()) {
            ImGui::TextColored(ImVec4(0.3f, 0.9f, 0.45f, 1.0f),
                               "Map is valid and ready to save.");
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.3f, 1.0f),
                               "Map has %zu validation issue(s):",
                               validationErrors_.size());
            ImGui::Separator();
            ImGui::BeginChild("##MapValidationErrors", ImVec2(0.0f, -38.0f),
                              true);
            for (const std::string& error : validationErrors_) {
                ImGui::Bullet();
                ImGui::SameLine();
                ImGui::TextWrapped("%s", error.c_str());
            }
            ImGui::EndChild();
        }
        if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::End();

    if (assetBrowserOpen_) {
        // Loaded lazily on first browser open rather than in the constructor:
        // LevelEditor is a global, and reading a file during static init would
        // run before the working directory is settled.
        if (!favouritesLoaded_) {
            LoadFavourites();
            favouritesLoaded_ = true;
        }
        ImGui::SetNextWindowPos(ImVec2(display.x - 430.0f, 490.0f),
                                ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(420.0f, display.y - 500.0f),
                                 ImGuiCond_FirstUseEver);
        ImGui::Begin("Content Browser", &assetBrowserOpen_);
        ImGui::BeginDisabled(pendingImport_.valid());
        if (ImGui::Button("Import Model...")) BrowseImportModel();
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Refresh")) {
            prefabRegistry_.Refresh(kPrefabRoot, kModelRoot);
            assetRegistry_.Refresh(true);
            selectedPrefab_ = -1;
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(assetUndo_.empty());
        if (ImGui::Button("Undo Asset")) UndoAsset();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(assetRedo_.empty());
        if (ImGui::Button("Redo Asset")) RedoAsset();
        ImGui::EndDisabled();
        if (ImGui::BeginTabBar("AssetKinds")) {
            const AssetKind kinds[] = { AssetKind::Model, AssetKind::Texture,
                AssetKind::Audio, AssetKind::Prefab, AssetKind::Level };
            for (int tab = 0; tab < IM_ARRAYSIZE(kinds); ++tab) {
                if (ImGui::BeginTabItem(AssetRegistry::KindName(kinds[tab]),
                                        nullptr)) {
                    assetKindTab_ = tab;
                    ImGui::EndTabItem();
                }
            }
            // Appended last so the existing 0-3 tab indices keep their meaning:
            // the prefab branch below tests == 3 and the asset branch maps 0/1/2
            // positionally. Inserting Favourites earlier would silently
            // repoint every one of those.
            if (ImGui::BeginTabItem("Favourites", nullptr)) {
                assetKindTab_ = kFavouritesTab;
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(-1.0f);
        ImGui::InputTextWithHint("##AssetFilter", "Filter assets...",
                                 assetFilter_, sizeof(assetFilter_));
        const std::string filter = assetFilter_;
        if (assetKindTab_ == 3) {
        const auto& prefabs = prefabRegistry_.Assets();
        for (size_t i = 0; i < prefabs.size(); ++i) {
            const PrefabAsset& prefab = prefabs[i];
            std::string searchable = prefab.name + " " + prefab.id + " " +
                                     prefab.modelPath.string();
            std::transform(searchable.begin(), searchable.end(), searchable.begin(),
                [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            std::string loweredFilter = filter;
            std::transform(loweredFilter.begin(), loweredFilter.end(),
                loweredFilter.begin(), [](unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });
            if (!loweredFilter.empty() &&
                searchable.find(loweredFilter) == std::string::npos) continue;
            ImGui::PushID(static_cast<int>(i));
            const bool selected = selectedPrefab_ == static_cast<int>(i);
            const std::string label = prefab.error.empty()
                ? prefab.name + (prefab.generated ? "  [model]" : "  [prefab]") +
                    (prefab.warnings.empty() ? "" : "  [WARN]")
                : prefab.name + "  [INVALID]";
            const ImVec4 placeholder = prefab.error.empty()
                ? (prefab.generated ? ImVec4(0.20f, 0.42f, 0.72f, 1.0f)
                                    : ImVec4(0.22f, 0.62f, 0.42f, 1.0f))
                : ImVec4(0.75f, 0.18f, 0.14f, 1.0f);
            const bool favourite = IsFavouritePrefab(prefab.id);
            ImGui::PushStyleColor(ImGuiCol_Text, favourite
                ? ImVec4(1.0f, 0.82f, 0.25f, 1.0f)
                : ImVec4(0.45f, 0.45f, 0.45f, 1.0f));
            if (ImGui::Button(favourite ? "*" : "-", ImVec2(22.0f, 42.0f)))
                ToggleFavouritePrefab(prefab.id);
            ImGui::PopStyleColor();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip(favourite ? "Remove from Favourites"
                                            : "Add to Favourites");
            ImGui::SameLine();
            const uint64_t thumbnail = thumbnailTexture && prefab.error.empty()
                ? thumbnailTexture(prefab) : 0;
            if (thumbnail)
                ImGui::Image((ImTextureID)(intptr_t)thumbnail,
                             ImVec2(42.0f, 42.0f));
            else
                ImGui::ColorButton("##thumbnail", placeholder,
                    ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                    ImVec2(42.0f, 42.0f));
            ImGui::SameLine();
            if (ImGui::Selectable(label.c_str(), selected,
                                  ImGuiSelectableFlags_AllowDoubleClick)) {
                selectedPrefab_ = static_cast<int>(i);
                prefabDraftIndex_ = -1;
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
                    prefab.error.empty()) AddPrefab(prefab, camera, terrainHeight);
            }
            if (prefab.error.empty() && ImGui::BeginDragDropSource()) {
                ImGui::SetDragDropPayload("SGE_PREFAB_ID", prefab.id.c_str(),
                    prefab.id.size() + 1);
                ImGui::Text("Place %s", prefab.name.c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted(prefab.id.c_str());
                ImGui::TextWrapped("%s", prefab.error.empty()
                    ? prefab.modelPath.string().c_str() : prefab.error.c_str());
                for (const std::string& warning : prefab.warnings)
                    ImGui::TextColored(ImVec4(1.0f, 0.72f, 0.2f, 1.0f),
                        "%s", warning.c_str());
                ImGui::EndTooltip();
            }
            ImGui::PopID();
        }
        const bool validSelection = selectedPrefab_ >= 0 &&
            selectedPrefab_ < static_cast<int>(prefabs.size()) &&
            prefabs[selectedPrefab_].error.empty();
        ImGui::BeginDisabled(!validSelection);
        if (ImGui::Button("Add Selected", ImVec2(-1.0f, 0.0f)))
            AddPrefab(prefabs[selectedPrefab_], camera, terrainHeight);
        ImGui::EndDisabled();
        if (validSelection) {
            if (prefabDraftIndex_ != selectedPrefab_) {
                prefabDraftIndex_ = selectedPrefab_;
                prefabDraft_ = prefabs[selectedPrefab_];
                strncpy_s(prefabAudioPath_, prefabDraft_.audio.path.string().c_str(),
                          _TRUNCATE);
            }
            ImGui::SeparatorText("Prefab Settings");
            ImGui::TextWrapped("%s", prefabDraft_.modelPath.string().c_str());
            ImGui::DragFloat("Target size", &prefabDraft_.targetSize, 0.1f,
                             0.0f, 1000.0f, "%.2f m");
            ImGui::DragFloat3("Default scale", prefabDraft_.defaultScale, 0.05f,
                              0.01f, 100.0f);
            ImGui::Checkbox("Cast shadow", &prefabDraft_.castShadow);
            ImGui::Checkbox("Use materials", &prefabDraft_.useMaterials);
            int collisionIndex = CollisionShapeIndex(prefabDraft_.collision);
            if (ImGui::Combo("Collision", &collisionIndex, kCollisionShapes,
                             IM_ARRAYSIZE(kCollisionShapes)))
                prefabDraft_.collision = kCollisionShapes[collisionIndex];

            ImGui::SeparatorText("Components");
            ImGui::Checkbox("Light", &prefabDraft_.light.enabled);
            if (prefabDraft_.light.enabled) {
                ImGui::ColorEdit3("Light color", prefabDraft_.light.color);
                ImGui::DragFloat("Light intensity", &prefabDraft_.light.intensity,
                                 0.1f, 0.0f, 100000.0f);
                ImGui::DragFloat("Light radius", &prefabDraft_.light.radius,
                                 0.1f, 0.01f, 10000.0f);
            }
            ImGui::Checkbox("Audio emitter", &prefabDraft_.audio.enabled);
            if (prefabDraft_.audio.enabled) {
                ImGui::InputTextWithHint("Audio path", "Content/Audio/loop.wav",
                    prefabAudioPath_, sizeof(prefabAudioPath_));
                ImGui::Checkbox("Audio loop", &prefabDraft_.audio.loop);
                ImGui::DragFloat("Audio radius", &prefabDraft_.audio.radius,
                                 0.1f, 0.01f, 10000.0f);
            }
            ImGui::Checkbox("Destructible", &prefabDraft_.destructible.enabled);
            if (prefabDraft_.destructible.enabled)
                ImGui::DragFloat("Health", &prefabDraft_.destructible.health,
                                 1.0f, 0.01f, 1000000.0f);
            ImGui::Checkbox("Spawner", &prefabDraft_.spawner.enabled);
            if (prefabDraft_.spawner.enabled) {
                char enemyType[96] = {};
                strncpy_s(enemyType, prefabDraft_.spawner.enemyType.c_str(), _TRUNCATE);
                if (ImGui::InputText("Enemy type", enemyType, sizeof(enemyType)))
                    prefabDraft_.spawner.enemyType = enemyType;
                int count = static_cast<int>(prefabDraft_.spawner.count);
                if (ImGui::DragInt("Spawn count", &count, 1.0f, 1, 1024))
                    prefabDraft_.spawner.count = static_cast<uint32_t>(count);
            }

            ImGui::SeparatorText("Variant / Nesting");
            const char* basePreview = prefabDraft_.basePrefabId.empty()
                ? "None" : prefabDraft_.basePrefabId.c_str();
            if (ImGui::BeginCombo("Extends", basePreview)) {
                if (ImGui::Selectable("None", prefabDraft_.basePrefabId.empty()))
                    prefabDraft_.basePrefabId.clear();
                for (const PrefabAsset& candidate : prefabs) {
                    if (candidate.id == prefabDraft_.id || !candidate.error.empty()) continue;
                    if (ImGui::Selectable(candidate.id.c_str(),
                            prefabDraft_.basePrefabId == candidate.id))
                        prefabDraft_.basePrefabId = candidate.id;
                }
                ImGui::EndCombo();
            }
            for (size_t childIndex = 0; childIndex < prefabDraft_.children.size();) {
                PrefabChildAsset& child = prefabDraft_.children[childIndex];
                ImGui::PushID(static_cast<int>(childIndex));
                ImGui::TextUnformatted(child.prefabId.c_str());
                ImGui::DragFloat3("Position", child.position, 0.05f);
                ImGui::DragFloat3("Rotation", child.rotation, 0.5f);
                ImGui::DragFloat3("Scale", child.scale, 0.05f, 0.01f, 100.0f);
                const bool remove = ImGui::SmallButton("Remove child");
                ImGui::PopID();
                if (remove) prefabDraft_.children.erase(
                    prefabDraft_.children.begin() + childIndex);
                else ++childIndex;
            }
            if (!prefabs.empty()) {
                prefabChildSelection_ = (std::min)(prefabChildSelection_,
                    static_cast<int>(prefabs.size()) - 1);
                if (ImGui::BeginCombo("Child prefab",
                        prefabs[prefabChildSelection_].id.c_str())) {
                    for (size_t i = 0; i < prefabs.size(); ++i) {
                        if (prefabs[i].id == prefabDraft_.id || !prefabs[i].error.empty())
                            continue;
                        if (ImGui::Selectable(prefabs[i].id.c_str(),
                                prefabChildSelection_ == static_cast<int>(i)))
                            prefabChildSelection_ = static_cast<int>(i);
                    }
                    ImGui::EndCombo();
                }
                if (ImGui::Button("Add child") &&
                    prefabs[prefabChildSelection_].id != prefabDraft_.id)
                    prefabDraft_.children.push_back(
                        { prefabs[prefabChildSelection_].id });
            }

            ImGui::SeparatorText("Material Overrides");
            for (size_t overrideIndex = 0;
                 overrideIndex < prefabDraft_.materialOverrides.size();) {
                const PrefabMaterialOverride& material =
                    prefabDraft_.materialOverrides[overrideIndex];
                ImGui::PushID(static_cast<int>(overrideIndex));
                ImGui::Text("%s -> %s", material.mesh.c_str(),
                    material.texture.generic_string().c_str());
                const bool remove = ImGui::SmallButton("Remove material");
                ImGui::PopID();
                if (remove) prefabDraft_.materialOverrides.erase(
                    prefabDraft_.materialOverrides.begin() + overrideIndex);
                else ++overrideIndex;
            }
            const auto textures = assetRegistry_.Assets(AssetKind::Texture);
            if (!textures.empty()) {
                prefabMaterialTextureSelection_ = (std::min)(
                    prefabMaterialTextureSelection_, static_cast<int>(textures.size()) - 1);
                ImGui::InputTextWithHint("Mesh / material", "MaterialName",
                    prefabMaterialMesh_, sizeof(prefabMaterialMesh_));
                if (ImGui::BeginCombo("Override texture", textures[
                        prefabMaterialTextureSelection_]->path.filename().string().c_str())) {
                    for (size_t i = 0; i < textures.size(); ++i)
                        if (ImGui::Selectable(textures[i]->path.generic_string().c_str(),
                                prefabMaterialTextureSelection_ == static_cast<int>(i)))
                            prefabMaterialTextureSelection_ = static_cast<int>(i);
                    ImGui::EndCombo();
                }
                ImGui::BeginDisabled(prefabMaterialMesh_[0] == '\0');
                if (ImGui::Button("Add material override"))
                    prefabDraft_.materialOverrides.push_back({
                        prefabMaterialMesh_,
                        textures[prefabMaterialTextureSelection_]->path });
                ImGui::EndDisabled();
            }

            ImGui::SeparatorText("LODs");
            for (size_t lodIndex = 0; lodIndex < prefabDraft_.lods.size();) {
                PrefabLodAsset& lod = prefabDraft_.lods[lodIndex];
                ImGui::PushID(static_cast<int>(lodIndex));
                ImGui::TextUnformatted(lod.path.generic_string().c_str());
                ImGui::DragFloat("Distance", &lod.distance, 0.5f, 0.0f, 100000.0f);
                const bool remove = ImGui::SmallButton("Remove LOD");
                ImGui::PopID();
                if (remove) prefabDraft_.lods.erase(
                    prefabDraft_.lods.begin() + lodIndex);
                else ++lodIndex;
            }
            const auto models = assetRegistry_.Assets(AssetKind::Model);
            if (!models.empty()) {
                prefabLodModelSelection_ = (std::min)(prefabLodModelSelection_,
                    static_cast<int>(models.size()) - 1);
                if (ImGui::BeginCombo("LOD model", models[
                        prefabLodModelSelection_]->path.filename().string().c_str())) {
                    for (size_t i = 0; i < models.size(); ++i)
                        if (ImGui::Selectable(models[i]->path.generic_string().c_str(),
                                prefabLodModelSelection_ == static_cast<int>(i)))
                            prefabLodModelSelection_ = static_cast<int>(i);
                    ImGui::EndCombo();
                }
                ImGui::DragFloat("New LOD distance", &prefabNewLodDistance_,
                                 0.5f, 0.0f, 100000.0f);
                if (ImGui::Button("Add LOD")) {
                    const AssetRecord* modelAsset = models[prefabLodModelSelection_];
                    prefabDraft_.lods.push_back({ modelAsset->path,
                        modelAsset->guid, prefabNewLodDistance_ });
                    std::sort(prefabDraft_.lods.begin(), prefabDraft_.lods.end(),
                        [](const PrefabLodAsset& a, const PrefabLodAsset& b) {
                            return a.distance < b.distance;
                        });
                }
            }
            if (ImGui::Button(prefabDraft_.generated
                    ? "Create Editable Prefab" : "Save Prefab Settings")) {
                prefabDraft_.audio.path = prefabAudioPath_;
                std::filesystem::path destination = prefabDraft_.definitionPath;
                if (prefabDraft_.generated) {
                    const std::string stem = SanitizeFileName(prefabDraft_.name);
                    prefabDraft_.id = "custom/" + stem;
                    prefabDraft_.generated = false;
                    destination = std::filesystem::path("Content/Prefabs/Created") /
                                  (stem + ".json");
                }
                AssetFileChange assetChange = CaptureAssetBefore(destination);
                const PrefabSaveResult saved = PrefabRegistry::Save(
                    prefabDraft_, destination);
                if (saved.ok) {
                    FinishAssetChange(std::move(assetChange));
                    status_ = "Saved prefab " + destination.string();
                    const std::string savedId = prefabDraft_.id;
                    prefabRegistry_.Refresh(kPrefabRoot, kModelRoot);
                    selectedPrefab_ = -1;
                    const auto& refreshed = prefabRegistry_.Assets();
                    for (size_t i = 0; i < refreshed.size(); ++i)
                        if (refreshed[i].id == savedId)
                            selectedPrefab_ = static_cast<int>(i);
                    prefabDraftIndex_ = -1;
                } else status_ = "Prefab save failed: " + saved.error;
            }
        }
        if (!prefabRegistry_.LastError().empty())
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "%s",
                prefabRegistry_.LastError().c_str());
        } else if (assetKindTab_ == kFavouritesTab) {
            std::string loweredFilter = filter;
            std::transform(loweredFilter.begin(), loweredFilter.end(),
                loweredFilter.begin(), [](unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });
            const auto matches = [&](const std::string& text) {
                if (loweredFilter.empty()) return true;
                std::string lowered = text;
                std::transform(lowered.begin(), lowered.end(), lowered.begin(),
                    [](unsigned char c) {
                        return static_cast<char>(std::tolower(c));
                    });
                return lowered.find(loweredFilter) != std::string::npos;
            };

            ImGui::BeginChild("FavouriteRows", ImVec2(0.0f, 0.0f), false);
            ImGui::SeparatorText("Entities");
            // Built-in types spawn through AddEntity, not AddPrefab -- they are
            // engine enum values with bespoke runtime handling, not data-driven
            // assets, so there is no PrefabAsset to hand to AddPrefab here.
            for (LevelEntityType type : kBuiltInSpawnTypes) {
                const char* name = LevelEntityTypeName(type);
                if (!matches(name)) continue;
                ImGui::PushID(name);
                // Reuses the Hierarchy's per-type colour so a type reads the
                // same here as it does in the outliner and the viewport.
                ImGui::ColorButton("##entityswatch",
                    ImGui::ColorConvertU32ToFloat4(TypeColor(type, false)),
                    ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                    ImVec2(28.0f, 28.0f));
                ImGui::SameLine();
                if (ImGui::Selectable(name, false,
                                      ImGuiSelectableFlags_AllowDoubleClick,
                                      ImVec2(0.0f, 28.0f))) {
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
                        AddEntity(type);
                }
                // Entity types travel as their LevelEntityTypeName string, which
                // ParseLevelEntityType turns back into the enum at the drop.
                // A separate payload type from SGE_PREFAB_ID because the drop
                // handler has to call AddEntity, not AddPrefab.
                if (ImGui::BeginDragDropSource()) {
                    ImGui::SetDragDropPayload("SGE_ENTITY_TYPE", name,
                                              std::strlen(name) + 1);
                    ImGui::Text("Place %s", name);
                    ImGui::EndDragDropSource();
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Double-click or drag to place %s", name);
                ImGui::PopID();
            }

            ImGui::SeparatorText("Prefabs");
            const auto& prefabs = prefabRegistry_.Assets();
            size_t shown = 0;
            for (size_t i = 0; i < prefabs.size(); ++i) {
                const PrefabAsset& prefab = prefabs[i];
                if (!IsFavouritePrefab(prefab.id)) continue;
                if (!matches(prefab.name + " " + prefab.id)) continue;
                ++shown;
                ImGui::PushID(static_cast<int>(i));
                ImGui::PushStyleColor(ImGuiCol_Text,
                                      ImVec4(1.0f, 0.82f, 0.25f, 1.0f));
                if (ImGui::Button("*", ImVec2(22.0f, 42.0f)))
                    ToggleFavouritePrefab(prefab.id);
                ImGui::PopStyleColor();
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Remove from Favourites");
                ImGui::SameLine();
                const uint64_t thumbnail =
                    thumbnailTexture && prefab.error.empty()
                        ? thumbnailTexture(prefab) : 0;
                if (thumbnail)
                    ImGui::Image((ImTextureID)(intptr_t)thumbnail,
                                 ImVec2(42.0f, 42.0f));
                else
                    ImGui::ColorButton("##thumbnail",
                        ImVec4(0.22f, 0.62f, 0.42f, 1.0f),
                        ImGuiColorEditFlags_NoTooltip |
                            ImGuiColorEditFlags_NoDragDrop,
                        ImVec2(42.0f, 42.0f));
                ImGui::SameLine();
                const std::string label = prefab.error.empty()
                    ? prefab.name : prefab.name + "  [INVALID]";
                if (ImGui::Selectable(label.c_str(), false,
                                      ImGuiSelectableFlags_AllowDoubleClick)) {
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
                        prefab.error.empty())
                        AddPrefab(prefab, camera, terrainHeight);
                }
                // Same payload the Prefabs tab publishes, so a favourite can be
                // dragged into the viewport exactly like any other prefab.
                if (prefab.error.empty() && ImGui::BeginDragDropSource()) {
                    ImGui::SetDragDropPayload("SGE_PREFAB_ID", prefab.id.c_str(),
                        prefab.id.size() + 1);
                    ImGui::Text("Place %s", prefab.name.c_str());
                    ImGui::EndDragDropSource();
                }
                if (ImGui::IsItemHovered())
                    ImGui::SetTooltip("Double-click or drag to place %s",
                                      prefab.name.c_str());
                ImGui::PopID();
            }
            // Starred ids outliving their prefab is normal (the file was deleted
            // or renamed), so say so rather than silently showing nothing.
            if (shown == 0) {
                if (favouritePrefabs_.empty())
                    ImGui::TextDisabled(
                        "No favourite prefabs. Star one on the Prefabs tab.");
                else
                    ImGui::TextDisabled(
                        "No favourite prefabs match, or the starred prefabs are missing.");
            }
            ImGui::EndChild();
        } else {
            const AssetKind kind = assetKindTab_ == 0 ? AssetKind::Model :
                assetKindTab_ == 1 ? AssetKind::Texture :
                assetKindTab_ == 2 ? AssetKind::Audio : AssetKind::Level;
            std::string loweredFilter = filter;
            std::transform(loweredFilter.begin(), loweredFilter.end(),
                loweredFilter.begin(), [](unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });
            const auto assets = assetRegistry_.Assets(kind);
            ImGui::BeginChild("AssetRows", ImVec2(0.0f, 0.0f), false);
            for (const AssetRecord* asset : assets) {
                std::string searchable = asset->path.generic_string();
                std::transform(searchable.begin(), searchable.end(),
                    searchable.begin(), [](unsigned char c) {
                        return static_cast<char>(std::tolower(c));
                    });
                if (!loweredFilter.empty() &&
                    searchable.find(loweredFilter) == std::string::npos) continue;
                ImGui::PushID(asset->path.generic_string().c_str());
                if (kind == AssetKind::Audio) {
                    if (ImGui::SmallButton("Play")) {
                        if (audioPreview_.Initialize(asset->path.string()))
                            audioPreview_.Play(0.8f);
                    }
                    ImGui::SameLine();
                } else {
                    const ImVec4 color = kind == AssetKind::Texture
                        ? ImVec4(0.55f, 0.32f, 0.68f, 1.0f)
                        : kind == AssetKind::Model
                            ? ImVec4(0.20f, 0.42f, 0.72f, 1.0f)
                            : ImVec4(0.72f, 0.52f, 0.18f, 1.0f);
                    ImGui::ColorButton("##asset", color,
                        ImGuiColorEditFlags_NoTooltip |
                        ImGuiColorEditFlags_NoDragDrop, ImVec2(18.0f, 18.0f));
                    ImGui::SameLine();
                }
                ImGui::TextUnformatted(asset->path.filename().string().c_str());
                if (!asset->missingDependencies.empty()) {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(1.0f, 0.58f, 0.18f, 1.0f),
                                       "[MISSING DEP]");
                    if (ImGui::IsItemHovered()) {
                        ImGui::BeginTooltip();
                        for (const std::string& missing : asset->missingDependencies)
                            ImGui::TextUnformatted(missing.c_str());
                        ImGui::EndTooltip();
                    }
                }
                if (kind == AssetKind::Model) {
                    const PrefabAsset* generatedPrefab = nullptr;
                    for (const PrefabAsset& prefab : prefabRegistry_.Assets())
                        if (prefab.error.empty() &&
                            prefab.modelPath.lexically_normal() ==
                                asset->path.lexically_normal()) {
                            generatedPrefab = &prefab;
                            break;
                        }
                    if (generatedPrefab && ImGui::BeginDragDropSource()) {
                        ImGui::SetDragDropPayload("SGE_PREFAB_ID",
                            generatedPrefab->id.c_str(), generatedPrefab->id.size() + 1);
                        ImGui::Text("Place %s", generatedPrefab->name.c_str());
                        ImGui::EndDragDropSource();
                    }
                }
                if (kind == AssetKind::Texture && asset->width > 0)
                    ImGui::TextDisabled("%d x %d  %.1f KB", asset->width,
                        asset->height, static_cast<double>(asset->size) / 1024.0);
                else
                    ImGui::TextDisabled("%.1f KB  %s",
                        static_cast<double>(asset->size) / 1024.0,
                        asset->path.generic_string().c_str());
                ImGui::PopID();
            }
            if (!assetRegistry_.LastError().empty())
                ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.3f, 1.0f), "%s",
                    assetRegistry_.LastError().c_str());
            ImGui::EndChild();
        }
        ImGui::End();
    }

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
    ImGui::InvisibleButton("##PrefabDropTarget", ImVec2(-1.0f, 32.0f));
    const ImVec2 dropMin = ImGui::GetItemRectMin();
    ImGui::GetWindowDrawList()->AddRectFilled(dropMin, ImGui::GetItemRectMax(),
        IM_COL32(35, 52, 68, 180), 4.0f);
    ImGui::GetWindowDrawList()->AddText(
        ImVec2(dropMin.x + 8.0f, dropMin.y + 8.0f),
        IM_COL32(180, 210, 235, 255), "Drop prefab here");
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(
                "SGE_PREFAB_ID")) {
            const char* prefabId = static_cast<const char*>(payload->Data);
            if (const PrefabAsset* prefab = prefabRegistry_.Find(prefabId))
                AddPrefab(*prefab, camera, terrainHeight);
        }
        ImGui::EndDragDropTarget();
    }
    ImGui::SeparatorText("Create");
    // Shares kBuiltInSpawnTypes with the Content Browser's Favourites tab so the
    // two lists cannot drift apart when a new entity type is added.
    for (auto type : kBuiltInSpawnTypes) {
        if (ImGui::SmallButton(LevelEntityTypeName(type))) AddEntity(type);
        if (type != LevelEntityType::PlayerSpawn) ImGui::SameLine();
    }
    ImGui::Separator();
    const bool cannotDuplicate = !Selected() ||
        Selected()->type == LevelEntityType::PlayerSpawn ||
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
    {
        // How the player arrives. "Player choice" offers both BlackHawk runs
        // and the boat when the level starts.
        const LevelDefinition insertionBefore = level_;
        const char* modes[] = {
            "Helicopter", "Boat", "Fast helicopter rappel", "Player choice" };
        int mode = static_cast<int>(level_.insertionMode);
        const bool insertionChanged =
            ImGui::Combo("Insertion", &mode, modes, IM_ARRAYSIZE(modes));
        if (insertionChanged)
            level_.insertionMode = static_cast<LevelInsertionMode>(mode);
        TrackItemEdit(insertionBefore, insertionChanged);
    }
    {
        const LevelDefinition insertionBefore = level_;
        const bool insertionChanged = ImGui::SliderFloat(
            "Insertion radius", &level_.deploymentRadius,
            kMinDeploymentRadius, kMaxDeploymentRadius, "%.1f m");
        TrackItemEdit(insertionBefore, insertionChanged);
        showInsertionRadiusPreview = ImGui::IsItemActive();
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Distance from island centre to every selectable "
                              "deployment/drop-off point.");
    }
    {
        const LevelDefinition boatBefore = level_;
        const bool boatChanged = ImGui::Checkbox(
            "Patrol boat", &level_.patrolBoatEnabled);
        TrackItemEdit(boatBefore, boatChanged);
    }
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
        if (entity->type == LevelEntityType::Prefab) {
            ImGui::TextDisabled("Prefab: %s", entity->prefabId.c_str());
            const PrefabAsset* prefab = prefabRegistry_.Find(entity->prefabId);
            if (prefab) {
                ImGui::TextWrapped("Model: %s", prefab->modelPath.string().c_str());
                before = level_;
                changed = DrawPrefabOverrides(*entity, *prefab);
                if (changed) MarkChanged(before);
            }
            else ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f),
                "Missing prefab. Refresh Content Browser.");
        }
        before = level_;
        changed = ImGui::DragFloat3("Position", entity->transform.position, 0.1f);
        TrackItemEdit(before, changed, entity->id);
        if (entity->type == LevelEntityType::WoodHouse ||
            entity->type == LevelEntityType::MetalHouse) {
            before = level_;
            changed = ImGui::DragFloat("Yaw", &entity->transform.rotation[1], 1.0f);
            TrackItemEdit(before, changed, entity->id);
            ImGui::TextDisabled("House scale locked for destruction physics");
        } else {
            before = level_;
            changed = ImGui::DragFloat3("Rotation", entity->transform.rotation, 1.0f);
            TrackItemEdit(before, changed, entity->id);
            ImGui::BeginDisabled(!SupportsScale(entity->type));
            before = level_;
            changed = ImGui::DragFloat3("Scale", entity->transform.scale, 0.05f, 0.01f, 100.0f);
            TrackItemEdit(before, changed, entity->id);
            ImGui::EndDisabled();
            if (!SupportsScale(entity->type))
                ImGui::TextDisabled("Scale locked for gameplay physics");
        }
        if (entity->type == LevelEntityType::EnemySpawn) {
            ImGui::SeparatorText("Loadout");
            // Index 0 is "Random", which is stored by REMOVING the override
            // rather than writing a sentinel -- so a spawner left alone keeps an
            // empty overrides object and the level file is unchanged.
            // The editor stays free of the gameplay headers (SkinnedEnemy.h
            // pulls in DX12, the FBX importer and navigation), so the loadout is
            // handled as the plain strings the level format actually stores.
            // kEnemyWeaponIds must match ParseBanditWeapon in SkinnedEnemy.h.
            const char* weaponNames[] = { "Random", "Rifle", "Shotgun", "Sniper" };
            const char* weaponIds[] = { "rifle", "shotgun", "sniper" };
            int selection = 0;
            const auto found = entity->overrides.find("enemyWeapon");
            if (found != entity->overrides.end() && found->is_string()) {
                const std::string current = found->get<std::string>();
                for (int i = 0; i < IM_ARRAYSIZE(weaponIds); ++i)
                    if (current == weaponIds[i]) selection = i + 1;
            }
            const LevelDefinition weaponBefore = level_;
            if (ImGui::Combo("Weapon", &selection, weaponNames,
                             IM_ARRAYSIZE(weaponNames))) {
                if (selection == 0) entity->overrides.erase("enemyWeapon");
                else entity->overrides["enemyWeapon"] = weaponIds[selection - 1];
                // Not a transform edit, so no entity id: this changes what
                // spawns, which the runtime only reads at the next spawn.
                TrackItemEdit(weaponBefore, true);
            }
            ImGui::TextDisabled(
                "Sniper: long range, laser warning, 80 hp.\n"
                "Shotgun: closes fast, 130 hp.\n"
                "Random: whatever the level roll gives.");
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
    if (ImGui::RadioButton("Select", &foliageTool_, 0)) {
        terrainTool_ = 0; splineTool_ = 0;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Paint (B)", &foliageTool_, 1)) {
        terrainTool_ = 0; splineTool_ = 0;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Erase", &foliageTool_, 2)) {
        terrainTool_ = 0; splineTool_ = 0;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Clear Cover", &foliageTool_, 3)) {
        terrainTool_ = 0; splineTool_ = 0;
    }
    const char* foliageTypes[] = { "Grass", "Dandelion", "Trees" };
    ImGui::Combo("Foliage", &foliageType_, foliageTypes, IM_ARRAYSIZE(foliageTypes));
    ImGui::SliderFloat("Radius", &brushRadius_, 0.5f, 12.0f, "%.1f m");
    ImGui::SliderFloat("Density", &brushDensity_, 0.1f, 3.0f, "%.2f");
    ImGui::SliderFloat("Spacing", &brushSpacing_, 0.25f, 10.0f, "%.2f m");
    ImGui::DragFloatRange2("Random scale", &foliageScaleMin_, &foliageScaleMax_,
        0.01f, 0.2f, 3.0f, "%.2f", "%.2f");
    foliageScaleMax_ = (std::max)(foliageScaleMin_, foliageScaleMax_);
    ImGui::TextWrapped("Hold LMB on terrain. Erase affects selected foliage type. Trees use destructible palms.");

    if (foliageTool_ == 3) {
        ImGui::SeparatorText("Ground Cover");
        ImGui::Text("Cleared areas: %zu / %zu", level_.foliageClear.size(),
                    kMaxFoliageClearStamps);
        ImGui::BeginDisabled(level_.foliageClear.empty());
        if (ImGui::Button("Restore Ground Cover")) {
            const LevelDefinition before = level_;
            level_.foliageClear.clear();
            MarkChanged(before);
            foliageRuntimeDirty_ = true;
            environmentRuntimeDirty_ = true;
        }
        ImGui::EndDisabled();
        ImGui::TextWrapped(
            "Removes the automatically scattered grass and dandelions, which "
            "are not entities and so cannot be deleted with Erase. Overlapping "
            "strokes merge. Saved with the level.");
    }
    ImGui::End();

    ImGui::SetNextWindowPos(ImVec2(600, 70), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(300, 380), ImGuiCond_FirstUseEver);
    ImGui::Begin("Spline Tool");
    if (ImGui::RadioButton("Select##spline", &splineTool_, 0)) {
        foliageTool_ = 0; terrainTool_ = 0;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Draw", &splineTool_, 1)) {
        foliageTool_ = 0; terrainTool_ = 0;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("Edit Points", &splineTool_, 2)) {
        foliageTool_ = 0; terrainTool_ = 0;
    }

    // Prefab repeated along the run. Listed by id so the choice survives a
    // registry refresh reordering its vector.
    {
        const std::vector<PrefabAsset>& assets = prefabRegistry_.Assets();
        std::string preview = splinePrefabId_.empty() ? "(none)" : splinePrefabId_;
        for (const PrefabAsset& asset : assets)
            if (asset.id == splinePrefabId_) { preview = asset.name; break; }
        if (ImGui::BeginCombo("Prefab", preview.c_str())) {
            for (const PrefabAsset& asset : assets) {
                if (!asset.error.empty()) continue;
                const bool chosen = asset.id == splinePrefabId_;
                if (ImGui::Selectable(asset.name.c_str(), chosen))
                    splinePrefabId_ = asset.id;
                if (chosen) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }

    // Defaults for runs created from here on. An existing run is retuned
    // below, against its own stored values.
    ImGui::SliderFloat("Spacing", &splineSpacing_, 0.25f, 25.0f, "%.3f m");
    ImGui::SliderFloat("Yaw offset", &splineYawOffset_, -180.0f, 180.0f, "%.1f deg");
    ImGui::Checkbox("Align to path", &splineAlignToPath_);
    ImGui::Checkbox("Conform to terrain", &splineConformToTerrain_);
    ImGui::Checkbox("Pitch to slope", &splinePitchToSlope_);
    ImGui::Checkbox("Closed loop", &splineClosed_);

    ImGui::SeparatorText("Runs");
    if (level_.splines.empty()) {
        ImGui::TextDisabled("None yet");
    } else {
        for (LevelSplinePath& spline : level_.splines) {
            ImGui::PushID((int)spline.id);
            const bool active = spline.id == activeSplineId_;
            if (ImGui::RadioButton("##active", active)) activeSplineId_ = spline.id;
            ImGui::SameLine();
            ImGui::Text("%s (%zu pts)", spline.name.c_str(), spline.points.size());
            ImGui::PopID();
        }
    }

    LevelSplinePath* active = ActiveSpline();
    if (active) {
        ImGui::SeparatorText("Active Run");
        const LevelDefinition before = level_;
        bool changed = false;
        changed |= ImGui::SliderFloat("Segment pitch", &active->spacing,
                                      0.25f, 25.0f, "%.3f m");
        changed |= ImGui::SliderFloat("Run yaw", &active->yawOffset,
                                      -180.0f, 180.0f, "%.1f deg");
        changed |= ImGui::Checkbox("Align##run", &active->alignToPath);
        changed |= ImGui::Checkbox("Conform##run", &active->conformToTerrain);
        changed |= ImGui::Checkbox("Pitch##run", &active->pitchToSlope);
        changed |= ImGui::Checkbox("Closed##run", &active->closed);
        // Coalesces a slider drag into one undo entry.
        TrackItemEdit(before, changed);
        if (changed) RebuildSplines(terrainHeight);

        if (ImGui::Button("Remove Last Point") && !active->points.empty()) {
            const LevelDefinition removeBefore = level_;
            active->points.pop_back();
            PushUndo(removeBefore);
            dirty_ = true;
            RebuildSplines(terrainHeight);
        }
        ImGui::SameLine();
        if (ImGui::Button("Delete Run")) {
            const LevelDefinition deleteBefore = level_;
            const uint64_t doomed = activeSplineId_;
            level_.splines.erase(
                std::remove_if(level_.splines.begin(), level_.splines.end(),
                               [doomed](const LevelSplinePath& spline) {
                                   return spline.id == doomed;
                               }),
                level_.splines.end());
            activeSplineId_ = 0;
            PushUndo(deleteBefore);
            dirty_ = true;
            RebuildSplines(terrainHeight);
        }
    }
    if (ImGui::Button("New Run")) {
        // Clearing the active id makes the next Draw click start a fresh run.
        activeSplineId_ = 0;
        splineTool_ = 1;
        foliageTool_ = 0;
        terrainTool_ = 0;
    }
    ImGui::TextWrapped(
        "Draw: click the ground to add points. Edit Points: drag a point to "
        "reshape the run. Segments are regenerated from the points, so the "
        "curve stays editable after saving.");
    ImGui::End();

    ImGui::SetNextWindowPos(ImVec2(305, 325), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(320, 430), ImGuiCond_FirstUseEver);
    ImGui::Begin("Terrain Sculpt");
    if (!terrainStampLibraryScanned_) {
        terrainStampNames_ = DiscoverTerrainStampNames();
        terrainStampSelection_ = (std::min)(terrainStampSelection_,
            (std::max)(0, static_cast<int>(terrainStampNames_.size()) - 1));
        terrainStampLibraryScanned_ = true;
    }
    if (ImGui::RadioButton("Select##terrain", &terrainTool_, 0)) { foliageTool_ = 0; splineTool_ = 0; }
    ImGui::SameLine();
    if (ImGui::RadioButton("Raise", &terrainTool_, 1)) { foliageTool_ = 0; splineTool_ = 0; }
    ImGui::SameLine();
    if (ImGui::RadioButton("Lower", &terrainTool_, 2)) { foliageTool_ = 0; splineTool_ = 0; }
    if (ImGui::RadioButton("Flatten", &terrainTool_, 3)) { foliageTool_ = 0; splineTool_ = 0; }
    ImGui::SameLine();
    if (ImGui::RadioButton("Grow", &terrainTool_, 4)) { foliageTool_ = 0; splineTool_ = 0; }
    ImGui::SameLine();
    if (ImGui::RadioButton("Paint", &terrainTool_, 5)) { foliageTool_ = 0; splineTool_ = 0; }
    ImGui::SameLine();
    if (ImGui::RadioButton("Stamp", &terrainTool_, 6)) { foliageTool_ = 0; splineTool_ = 0; }
    if (terrainTool_ != 6) {
        ImGui::SliderFloat("Brush radius", &terrainBrushRadius_, 0.5f, 15.0f, "%.1f m");
        ImGui::SliderFloat("Strength", &terrainBrushStrength_, 0.05f, 2.0f, "%.2f");
        ImGui::SliderFloat("Stroke spacing", &terrainBrushSpacing_, 0.2f, 8.0f, "%.2f m");
    } else {
        ImGui::SeparatorText("Heightmap Stamp");
        const std::string previewName = terrainStampNames_.empty() ?
            "No stamps found" :
            TerrainStampDisplayName(terrainStampNames_[terrainStampSelection_]);
        if (ImGui::BeginCombo("Shape", previewName.c_str())) {
            for (int i = 0; i < static_cast<int>(terrainStampNames_.size()); ++i) {
                const bool selected = i == terrainStampSelection_;
                const std::string label = TerrainStampDisplayName(terrainStampNames_[i]);
                if (ImGui::Selectable(label.c_str(), selected))
                    terrainStampSelection_ = i;
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        if (ImGui::Button("Refresh Stamps")) terrainStampLibraryScanned_ = false;
        ImGui::SameLine();
        ImGui::TextDisabled("%zu found", terrainStampNames_.size());
        // Flat 2D read of the same grid drawn in the viewport. The in-world
        // preview is the one that matters for placement, but it is only visible
        // while the cursor is over terrain -- this stays up while picking.
        if (!terrainStampNames_.empty()) {
            const std::string& selected =
                terrainStampNames_[terrainStampSelection_];
            if (selected != stampPreviewName_) {
                stampPreviewName_ = selected;
                stampPreviewValid_ =
                    LoadStampPreview(selected, stampPreviewHeights_);
            }
        }
        if (stampPreviewValid_) {
            const float side = (std::min)(120.0f,
                ImGui::GetContentRegionAvail().x);
            const ImVec2 origin = ImGui::GetCursorScreenPos();
            ImDrawList* panel = ImGui::GetWindowDrawList();
            // The thumbnail is ~120 px wide, so drawing all 64 rows would make
            // every cell sub-pixel. Step down to whole pixels and sample the
            // grid, rather than emitting rects the panel cannot show.
            const int thumbStep = (std::max)(1,
                kStampPreviewGrid / (std::max)(1, static_cast<int>(side / 3.0f)));
            const int thumbCells = kStampPreviewGrid / thumbStep;
            const float cell = side / thumbCells;
            for (int ty = 0; ty < thumbCells; ++ty) {
                for (int tx = 0; tx < thumbCells; ++tx) {
                    const int gy = ty * thumbStep, gx = tx * thumbStep;
                    const float level = stampPreviewHeights_[
                        static_cast<size_t>(gy) * kStampPreviewGrid + gx];
                    const int tone = static_cast<int>(level * 255.0f);
                    panel->AddRectFilled(
                        ImVec2(origin.x + tx * cell, origin.y + ty * cell),
                        ImVec2(origin.x + (tx + 1) * cell,
                               origin.y + (ty + 1) * cell),
                        IM_COL32(tone, tone, tone, 255));
                }
            }
            panel->AddRect(origin, ImVec2(origin.x + side, origin.y + side),
                           IM_COL32(185, 110, 255, 235));
            ImGui::Dummy(ImVec2(side, side));
        } else if (!terrainStampNames_.empty()) {
            ImGui::TextDisabled("Preview unavailable for this stamp.");
        }
        ImGui::SliderFloat("Stamp radius", &terrainStampRadius_, 1.0f, 64.0f, "%.1f m");
        ImGui::SliderFloat("Stamp height", &terrainStampHeight_, -32.0f, 32.0f, "%.1f m");
        ImGui::SliderFloat("Rotation", &terrainStampRotation_, 0.0f, 360.0f, "%.0f deg");

        // Additive vs replace. Two presets plus the raw slider: full replace and
        // pure additive are what get used, but the in-between blend is genuinely
        // useful for calming procedural noise without erasing it.
        ImGui::SeparatorText("Blend");
        if (ImGui::RadioButton("Additive", terrainStampReplace_ <= 0.0f))
            terrainStampReplace_ = 0.0f;
        ImGui::SameLine();
        if (ImGui::RadioButton("Replace", terrainStampReplace_ >= 1.0f))
            terrainStampReplace_ = 1.0f;
        ImGui::SliderFloat("Replace amount", &terrainStampReplace_, 0.0f, 1.0f,
                           "%.2f");
        ImGui::BeginDisabled(terrainStampReplace_ <= 0.0f);
        ImGui::SliderFloat("Base offset", &terrainStampBaseOffset_,
                           -32.0f, 32.0f, "%.1f m");
        ImGui::EndDisabled();
        ImGui::TextWrapped(terrainStampReplace_ > 0.0f
            ? "Click terrain to place one 16-bit heightmap stamp. Replace "
              "overwrites the ground inside the square with the stamp, built "
              "around the height under the cursor plus Base offset."
            : "Click terrain to place one 16-bit heightmap stamp. Additive "
              "lays the stamp's relief on top of the existing ground.");
    }
    ImGui::Text("Stamps: %zu / %zu", level_.terrainSculpt.size(),
                kMaxTerrainSculptStamps);
    ImGui::BeginDisabled(level_.terrainSculpt.empty());
    if (ImGui::Button("Clear Sculpt")) {
        const LevelDefinition before = level_;
        level_.terrainSculpt.clear();
        MarkChanged(before);
    }
    ImGui::SameLine();
    // Every stamp is evaluated per terrain vertex, five times over (height plus
    // four finite-difference normal samples), so the sculpt loop grows with the
    // stack and dominates the terrain raster once it is deep. Baking resolves
    // the stack once into a single heightmap stamp: the terrain looks the same
    // and the loop runs one iteration.
    if (ImGui::Button("Bake to One Stamp")) {
        const LevelDefinition before = level_;
        // One bake target per level, so baking two levels does not have the
        // second overwrite the first's heightmap. Stamp filenames must be
        // simple names inside the stamp directory (IsTerrainStampFilename),
        // so anything unusual in the level name is folded to an underscore.
        std::string safeLevel;
        for (char c : level_.name) {
            const unsigned char u = static_cast<unsigned char>(c);
            safeLevel.push_back(
                (std::isalnum(u) || c == '-' || c == '_') ? c : '_');
        }
        if (safeLevel.empty()) safeLevel = "level";
        if (safeLevel.size() > 64) safeLevel.resize(64);
        const std::string bakeName = "HM_Baked_" + safeLevel + ".png";
        TerrainSculptStamp baked;
        const TerrainBakeResult result = BakeTerrainSculptToStamp(
            level_.terrainSculpt, terrainHeight,
            bakeName, &stbi_zlib_compress, baked);
        if (!result.ok) {
            status_ = "Bake failed: " + result.error;
        } else {
            level_.terrainSculpt.assign(1, baked);
            MarkChanged(before);
            char message[192];
            std::snprintf(message, sizeof(message),
                "Baked %zu stamps into %s (%.2f m/texel). Undo to restore them.",
                result.bakedStamps, result.texture.c_str(),
                result.metresPerTexel);
            status_ = message;
        }
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip(
            "Replace every sculpt stamp with one baked heightmap covering the\n"
            "same ground. Detail finer than the bake's metres-per-texel is\n"
            "averaged away; the stamps stay in the undo stack.");
    ImGui::EndDisabled();
    if (terrainTool_ != 6)
        ImGui::TextWrapped("Hold LMB on terrain. Flatten uses height where stroke starts.");

    if (terrainTool_ == 5) {
        ImGui::SeparatorText("Material Paint");
        const char* kLayers[] = { "Grass", "Dirt", "Sand", "Rock" };
        ImGui::Combo("Layer", &terrainPaintLayer_, kLayers,
                     IM_ARRAYSIZE(kLayers));
        ImGui::SliderFloat("Paint opacity", &terrainPaintStrength_, 0.05f, 1.0f,
                           "%.2f");
        if (level_.terrainSplatResolution > 0)
            ImGui::Text("Splatmap: %ux%u", level_.terrainSplatResolution,
                        level_.terrainSplatResolution);
        else
            ImGui::TextDisabled("Splatmap: none (created on first stroke)");
        ImGui::BeginDisabled(level_.terrainSplatResolution == 0);
        if (ImGui::Button("Clear Painting")) {
            const LevelDefinition before = level_;
            level_.terrainSplatRGBA.clear();
            level_.terrainSplatResolution = 0;
            ++level_.terrainSplatRevision;
            MarkChanged(before);
        }
        ImGui::EndDisabled();
        ImGui::TextWrapped(
            "Painted weights override the procedural slope/height blend. "
            "Unpainted ground keeps its automatic materials. Saved beside the "
            "level as <name>_splat.png.");
    }

    ImGui::SeparatorText("Island Builder");
    {
        const LevelDefinition before = level_;
        if (ImGui::Checkbox("Flat plane", &level_.terrainFlat))
            MarkChanged(before);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Replace the procedural island with a level "
                              "plane.\nNo relief, pool or coastline -- sculpt "
                              "stamps only.");
    }
    // Island size = the land radius. The tile grid auto-grows to fit it (see
    // CurrentTerrainParams), so this one slider stretches the whole island and
    // the ground + surrounding ocean follow. Manual Extend buttons below add
    // extra ground/ocean beyond the auto-fit if you want a bigger sea.
    // Per-axis island size (independent -> wide ovals or long strips). The
    // clipmap terrain keeps full detail near the camera at any size, so this
    // can go large.
    {
        const LevelDefinition before = level_;
        bool changed = ImGui::SliderFloat("Island width (X)",
            &level_.terrainIslandScaleX, 0.5f, 12.0f, "%.2f x");
        TrackItemEdit(before, changed);
    }
    {
        const LevelDefinition before = level_;
        bool changed = ImGui::SliderFloat("Island depth (Z)",
            &level_.terrainIslandScaleZ, 0.5f, 12.0f, "%.2f x");
        TrackItemEdit(before, changed);
    }
    // Link toggle: drag either slider with this on to scale both together.
    {
        const LevelDefinition before = level_;
        if (ImGui::Button("Make Square")) {
            const float s = (std::max)(level_.terrainIslandScaleX,
                                       level_.terrainIslandScaleZ);
            level_.terrainIslandScaleX = s;
            level_.terrainIslandScaleZ = s;
            MarkChanged(before);
        }
    }

    ImGui::SeparatorText("Extend Terrain");
    ImGui::Text("Grid: %u x %u tiles", level_.terrainTilesX, level_.terrainTilesZ);
    // Directional growth: add a row/column on one side. The 'Grow' tool also
    // lets you click the on-screen edge markers to do the same thing.
    if (ImGui::Button("-Z")) ExtendTerrain(3);
    ImGui::SameLine();
    if (ImGui::Button("-X")) ExtendTerrain(1);
    ImGui::SameLine();
    if (ImGui::Button("+X")) ExtendTerrain(0);
    ImGui::SameLine();
    if (ImGui::Button("+Z")) ExtendTerrain(2);
    ImGui::TextWrapped("Pick the Grow tool to see the tile grid and click an "
                       "edge marker, or use these buttons. Max 48 tiles/side.");
    ImGui::End();

    ImGui::Begin("DXR Lumen Lite");
    LevelDXRDDGISettings& ddgi = level_.dxrDDGI;
    const LevelDefinition ddgiBefore = level_;
    bool ddgiChanged = false;
    ddgiChanged |= ImGui::Checkbox("Enable", &ddgi.enabled);
    ddgiChanged |= ImGui::DragFloat("Surface spacing", &ddgi.surfaceSpacing,
        0.1f, 0.25f, 50.0f, "%.2f m");
    ddgiChanged |= ImGui::DragFloat("Surface offset", &ddgi.surfaceOffset,
        0.02f, 0.0f, 5.0f, "%.2f m");
    // Bounded by the same constant the level validator enforces. These were
    // previously inconsistent -- the slider went to 16384 while the validator
    // rejected anything over 2048, so the editor would happily author a probe
    // count that made the saved level fail to load.
    int maximumProbes = static_cast<int>(ddgi.maxProbes);
    if (ImGui::DragInt("Maximum probes", &maximumProbes, 8.0f, 1,
                       static_cast<int>(kMaxDDGIProbes))) {
        ddgi.maxProbes = static_cast<uint32_t>(maximumProbes);
        ddgiChanged = true;
    }
    // Hard-capped at 64 by the renderer: the irradiance atlas has an 8x8
    // directional interior, and more rays than texels would race writing the
    // same ones. The slider stops where the clamp does so the number shown is
    // the number used -- it previously accepted values up to 256 that
    // ApplySettings silently reduced.
    int raysPerProbe = static_cast<int>(ddgi.raysPerProbe);
    if (ImGui::DragInt("Rays per probe", &raysPerProbe, 1.0f, 8, 64)) {
        ddgi.raysPerProbe = static_cast<uint32_t>(raysPerProbe);
        ddgiChanged = true;
    }
    if (ddgi.raysPerProbe > 64u)
        ImGui::TextDisabled("  Clamped to 64 (8x8 directional atlas).");
    int probesPerFrame = static_cast<int>(ddgi.probesPerFrame);
    if (ImGui::DragInt("Probes per frame", &probesPerFrame, 1.0f, 1,
                       static_cast<int>(ddgi.maxProbes))) {
        ddgi.probesPerFrame = static_cast<uint32_t>(probesPerFrame);
        ddgiChanged = true;
    }
    ddgiChanged |= ImGui::DragFloat("Ray distance",
        &ddgi.maxRayDistance, 0.5f, 1.0f, 200.0f, "%.1f m");
    ddgiChanged |= ImGui::SliderFloat("GI intensity", &ddgi.intensity,
        0.0f, 5.0f);
    ddgiChanged |= ImGui::SliderFloat("Normal bias", &ddgi.normalBias,
        0.0f, 2.0f);
    ddgiChanged |= ImGui::SliderFloat("View bias", &ddgi.viewBias,
        0.0f, 2.0f);
    ddgiChanged |= ImGui::SliderFloat("Hysteresis", &ddgi.hysteresis,
        0.0f, 0.999f, "%.3f");
    ddgiChanged |= ImGui::SliderFloat("Multi-bounce strength",
        &ddgi.multiBounceStrength, 0.0f, 1.0f);
    ddgiChanged |= ImGui::Checkbox("Show probes", &ddgi.showProbes);
    if (ddgiChanged) {
        const LevelDXRDDGISettings& oldDDGI = ddgiBefore.dxrDDGI;
        dxrDDGILayoutDirty_ |=
            oldDDGI.enabled != ddgi.enabled ||
            oldDDGI.surfaceSpacing != ddgi.surfaceSpacing ||
            oldDDGI.surfaceOffset != ddgi.surfaceOffset ||
            oldDDGI.maxProbes != ddgi.maxProbes;
        MarkChanged(ddgiBefore);
        dxrDDGIRuntimeDirty_ = true;
        actions.levelChanged = true;
    }
    if (ImGui::Button("Rebuild Layout"))
        actions.rebuildDXRDDGI = true;
    ImGui::SameLine();
    if (ImGui::Button("Reset History"))
        actions.resetDXRDDGIHistory = true;
    ImGui::Separator();
    ImGui::Text("DXR support: %s",
        dxrDDGIStatus_.supported ? "Yes" : "No");
    ImGui::Text("Probe updates: %s",
        dxrDDGIStatus_.updatesActive ? "Active" : "Inactive");
    ImGui::Text("Probes: %u", dxrDDGIStatus_.probeCount);
    ImGui::Text("Rays/frame: %u", dxrDDGIStatus_.raysPerFrame);
    ImGui::Text("Cache: %s", dxrDDGIStatus_.cacheStatus.c_str());
    ImGui::Text("GPU memory: %.2f MiB",
        static_cast<double>(dxrDDGIStatus_.gpuMemoryBytes) / (1024.0 * 1024.0));
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
                TerrainSnaps(entity->type)) {
                entity->transform.position[1] = terrainHeight(
                    entity->transform.position[0], entity->transform.position[2]) +
                    TerrainSnapOffset(entity->type);
            }
            if (entity->type == LevelEntityType::WoodHouse ||
                entity->type == LevelEntityType::MetalHouse) {
                entity->transform.rotation[1] = rotation[1];
            } else {
                std::copy(rotation, rotation + 3, entity->transform.rotation);
                if (SupportsScale(entity->type))
                    std::copy(scale, scale + 3, entity->transform.scale);
            }
            MarkTransformRuntimeDirty(entity->id);
        } else if (gizmoWasUsing_) {
            MarkChanged(gizmoBefore_, entity->id);
            dxrDDGIRuntimeDirty_ = true;
            dxrDDGILayoutDirty_ = true;
        }
        gizmoWasUsing_ = usingGizmo;
    }

    if (const ImGuiPayload* dragging = ImGui::GetDragDropPayload();
        dragging && (dragging->IsDataType("SGE_PREFAB_ID") ||
                     dragging->IsDataType("SGE_ENTITY_TYPE"))) {
        const bool draggingEntityType = dragging->IsDataType("SGE_ENTITY_TYPE");
        ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
        ImGui::SetNextWindowSize(display);
        ImGui::SetNextWindowBgAlpha(0.0f);
        const ImGuiWindowFlags dropFlags = ImGuiWindowFlags_NoDecoration |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoBringToFrontOnFocus;
        ImGui::Begin("##ScenePrefabDropTarget", nullptr, dropFlags);
        ImGui::InvisibleButton("##SceneDropSurface", ImGui::GetContentRegionAvail());

        // Ghost preview. Dropping used to be blind: the prefab only appeared on
        // release, so placing anything precise meant drop, look, undo, retry.
        // Draw the footprint snapped to the terrain under the cursor for the
        // whole drag, so the drop lands where the marker already is.
        {
            XMFLOAT3 ghost;
            if (TerrainPointUnderMouse(view, projection, terrainHeight, ghost)) {
                // Only a prefab payload names a prefab; an entity-type payload
                // carries a LevelEntityTypeName, which would never resolve here.
                const PrefabAsset* dragged = nullptr;
                if (dragging->Data && !draggingEntityType)
                    dragged = prefabRegistry_.Find(
                        static_cast<const char*>(dragging->Data));
                // Footprint radius from the prefab's authored size, so a tower
                // reads as a tower and a rock as a rock. Half of targetSize is
                // the model's own half-span, which is what it will occupy.
                const float radius = (dragged && dragged->targetSize > 0.0f)
                    ? (std::max)(0.6f, dragged->targetSize * 0.5f)
                    : 1.5f;
                const XMMATRIX ghostViewProjection = view * projection;
                const ImVec2 displaySize = ImGui::GetIO().DisplaySize;
                ImDrawList* ghostDraw = ImGui::GetForegroundDrawList();

                // Project a world point to screen. Returns false behind the eye,
                // where the perspective divide flips the result.
                const auto project = [&](float wx, float wy, float wz,
                                         ImVec2& out) {
                    const XMVECTOR clip = XMVector3Transform(
                        XMVectorSet(wx, wy, wz, 1.0f), ghostViewProjection);
                    const float w = XMVectorGetW(clip);
                    if (w <= 0.01f) return false;
                    out = ImVec2(
                        (XMVectorGetX(clip) / w * 0.5f + 0.5f) * displaySize.x,
                        (1.0f - (XMVectorGetY(clip) / w * 0.5f + 0.5f)) *
                            displaySize.y);
                    return true;
                };

                // Base ring, following the ground rather than sitting on a flat
                // disc, so the preview reads correctly on a slope.
                ImVec2 ring[33];
                int ringCount = 0;
                for (int i = 0; i <= 32; ++i) {
                    const float angle = XM_2PI * static_cast<float>(i) / 32.0f;
                    const float px = ghost.x + std::cos(angle) * radius;
                    const float pz = ghost.z + std::sin(angle) * radius;
                    ImVec2 screen;
                    if (project(px, terrainHeight(px, pz) + 0.05f, pz, screen))
                        ring[ringCount++] = screen;
                }
                const ImU32 ghostColour = IM_COL32(90, 185, 255, 235);
                if (ringCount > 1)
                    ghostDraw->AddPolyline(ring, ringCount, ghostColour,
                                           ImDrawFlags_None, 2.0f);

                // Vertical extent, so tall props announce their height before
                // they are committed.
                const float height = (dragged && dragged->targetSize > 0.0f)
                    ? dragged->targetSize : 2.0f;
                ImVec2 base, top;
                const bool haveBase = project(ghost.x, ghost.y, ghost.z, base);
                if (haveBase &&
                    project(ghost.x, ghost.y + height, ghost.z, top)) {
                    ghostDraw->AddLine(base, top,
                                       IM_COL32(90, 185, 255, 150), 1.5f);
                    ghostDraw->AddCircleFilled(base, 4.0f, ghostColour);
                }

                // Readout of the exact drop point, so a placement can be matched
                // to a coordinate without committing it first.
                const char* ghostName = dragged ? dragged->name.c_str()
                    : (draggingEntityType && dragging->Data
                        ? static_cast<const char*>(dragging->Data) : nullptr);
                if (ghostName && haveBase) {
                    char label[128];
                    std::snprintf(label, sizeof(label), "%s  %.1f, %.1f, %.1f",
                                  ghostName, ghost.x, ghost.y, ghost.z);
                    ghostDraw->AddText(ImVec2(base.x + 10.0f, base.y + 6.0f),
                                       ghostColour, label);
                }
            }
        }

        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(
                    "SGE_PREFAB_ID")) {
                const char* prefabId = static_cast<const char*>(payload->Data);
                if (const PrefabAsset* prefab = prefabRegistry_.Find(prefabId)) {
                    XMFLOAT3 hit;
                    const bool hasHit = TerrainPointUnderMouse(
                        view, projection, terrainHeight, hit);
                    AddPrefab(*prefab, camera, terrainHeight);
                    if (hasHit) {
                        if (LevelEntity* placed = Selected()) {
                            placed->transform.position[0] = hit.x;
                            placed->transform.position[1] = hit.y;
                            placed->transform.position[2] = hit.z;
                        }
                    }
                }
            }
            // Built-in types dropped from the Favourites tab. AddEntity applies
            // the type's own defaults (scale, single-instance handling for
            // PlayerSpawn/Helicopter), then the drop point overrides XZ.
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(
                    "SGE_ENTITY_TYPE")) {
                LevelEntityType dropped = LevelEntityType::Rock;
                if (payload->Data &&
                    ParseLevelEntityType(
                        static_cast<const char*>(payload->Data), dropped)) {
                    XMFLOAT3 hit;
                    const bool hasHit = TerrainPointUnderMouse(
                        view, projection, terrainHeight, hit);
                    AddEntity(dropped);
                    if (hasHit) {
                        if (LevelEntity* placed = Selected()) {
                            placed->transform.position[0] = hit.x;
                            placed->transform.position[2] = hit.z;
                            // Ground-hugging types sit on the terrain with their
                            // authored offset. Helicopter and PlayerSpawn keep
                            // the altitude AddEntity just gave them (read off
                            // the entity rather than duplicated here, so the two
                            // cannot drift), matching the gizmo's snap
                            // exclusions.
                            if (TerrainSnaps(dropped))
                                placed->transform.position[1] =
                                    hit.y + TerrainSnapOffset(dropped);
                        }
                    }
                }
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::End();
    }

    SculptTerrain(view, projection, terrainHeight);
    // Must match the resolve's world->UV frame exactly (see RenderVBDraw's
    // SetTerrainSplatExtent call): kShoreOuter is the coastline radius from
    // terrain_ms.hlsl, scaled per axis. If these two drift apart, paint lands
    // in one place in the editor and renders in another.
    {
        constexpr float kShoreOuter = 88.0f;
        PaintTerrain(view, projection, terrainHeight,
                     kShoreOuter * level_.terrainIslandScaleX,
                     kShoreOuter * level_.terrainIslandScaleZ);
    }
    ExtendTerrainInteraction(view, projection);
    PaintFoliage(view, projection, terrainHeight);
    SplineTool(view, projection, terrainHeight);
    if (foliageTool_ == 0 && terrainTool_ == 0 && splineTool_ == 0)
        SelectFromViewport(view, projection);
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    XMMATRIX viewProjection = view * projection;
    if (showInsertionRadiusPreview && terrainHeight) {
        constexpr int ringSegments = 96;
        ImVec2 ring[ringSegments + 1];
        bool visible[ringSegments + 1] = {};
        const auto projectInsertionPoint = [&](float x, float z,
                                               ImVec2& screen) {
            const float y = terrainHeight(x, z) + 0.15f;
            const XMVECTOR clip = XMVector3Transform(
                XMVectorSet(x, y, z, 1.0f), viewProjection);
            const float w = XMVectorGetW(clip);
            if (w <= 0.01f) return false;
            const float sx = (XMVectorGetX(clip) / w * 0.5f + 0.5f) * display.x;
            const float sy =
                (1.0f - (XMVectorGetY(clip) / w * 0.5f + 0.5f)) * display.y;
            if (!std::isfinite(sx) || !std::isfinite(sy)) return false;
            screen = ImVec2(sx, sy);
            return true;
        };

        for (int i = 0; i <= ringSegments; ++i) {
            const float angle = XM_2PI * static_cast<float>(i) /
                                static_cast<float>(ringSegments);
            const float x = std::sin(angle) * level_.deploymentRadius;
            const float z = std::cos(angle) * level_.deploymentRadius;
            visible[i] = projectInsertionPoint(x, z, ring[i]);
        }
        const ImU32 ringColour = IM_COL32(70, 220, 255, 235);
        for (int i = 0; i < ringSegments; ++i)
            if (visible[i] && visible[i + 1])
                draw->AddLine(ring[i], ring[i + 1], ringColour, 2.5f);

        ImVec2 labelPoint{};
        bool haveLabelPoint = false;
        constexpr int deploymentZoneCount = 20;
        for (int i = 0; i < deploymentZoneCount; ++i) {
            const float angle = XM_2PI * static_cast<float>(i) /
                                static_cast<float>(deploymentZoneCount);
            ImVec2 point;
            if (!projectInsertionPoint(
                    std::sin(angle) * level_.deploymentRadius,
                    std::cos(angle) * level_.deploymentRadius, point))
                continue;
            draw->AddCircleFilled(point, 4.5f, IM_COL32(255, 205, 70, 245));
            if (!haveLabelPoint) {
                labelPoint = point;
                haveLabelPoint = true;
            }
        }
        if (haveLabelPoint) {
            char label[64] = {};
            std::snprintf(label, sizeof(label), "Insertion radius %.1f m",
                          level_.deploymentRadius);
            draw->AddText(ImVec2(labelPoint.x + 8.0f, labelPoint.y - 18.0f),
                          IM_COL32(210, 245, 255, 255), label);
        }
    }
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
