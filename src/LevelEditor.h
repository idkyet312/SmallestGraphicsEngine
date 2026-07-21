#ifndef LEVEL_EDITOR_H
#define LEVEL_EDITOR_H

#include "LevelDefinition.h"
#include <DirectXMath.h>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

class Camera;

struct LevelEditorActions {
    bool levelChanged = false;
    bool beginPlay = false;
    bool stopPlay = false;
    bool returnToMenu = false;
};

class LevelEditor {
public:
    void NewFromLevelOne();
    void BeginPlay();
    void StopPlay();
    bool IsPlaying() const { return playing_; }
    bool IsDirty() const { return dirty_; }
    const LevelDefinition& Level() const { return level_; }
    LevelDefinition& Level() { return level_; }
    void MarkRuntimeSynchronized() { runtimeDirty_ = false; }
    bool RuntimeDirty() const { return runtimeDirty_; }
    bool FoliageRuntimeDirty() const { return foliageRuntimeDirty_; }
    void MarkFoliageRuntimeSynchronized() { foliageRuntimeDirty_ = false; }
    void OnKeyDown(unsigned key, bool controlDown);
    LevelEditorActions Render(Camera& camera,
        DirectX::CXMMATRIX view, DirectX::CXMMATRIX projection,
        const std::function<float(float, float)>& terrainHeight);

private:
    LevelEntity* Selected();
    const LevelEntity* Selected() const;
    void SelectFromViewport(DirectX::CXMMATRIX view,
                            DirectX::CXMMATRIX projection);
    void PushUndo(const LevelDefinition& before);
    void Undo();
    void Redo();
    void AddEntity(LevelEntityType type);
    void DuplicateSelected();
    void DeleteSelected();
    bool SaveTo(const std::filesystem::path& path);
    bool BrowseSaveAs();
    void RefreshLevelFiles();
    void MarkChanged(const LevelDefinition& before);
    void TrackItemEdit(const LevelDefinition& before, bool changed);
    void PaintFoliage(DirectX::CXMMATRIX view, DirectX::CXMMATRIX projection,
        const std::function<float(float, float)>& terrainHeight);
    bool TerrainPointUnderMouse(DirectX::CXMMATRIX view,
        DirectX::CXMMATRIX projection,
        const std::function<float(float, float)>& terrainHeight,
        DirectX::XMFLOAT3& point) const;
    bool FoliageChanged(const LevelDefinition& before) const;

    LevelDefinition level_ = MakeLevelOneTemplate();
    LevelDefinition playSnapshot_;
    uint64_t selectedId_ = 0;
    uint64_t nextId_ = 1;
    bool playing_ = false;
    bool dirty_ = false;
    bool runtimeDirty_ = true;
    bool foliageRuntimeDirty_ = true;
    bool localSpace_ = false;
    bool snapEnabled_ = true;
    bool terrainSnap_ = true;
    float translationSnap_ = 0.5f;
    float rotationSnap_ = 15.0f;
    int gizmoOperation_ = 0;
    bool gizmoWasUsing_ = false;
    LevelDefinition gizmoBefore_;
    LevelDefinition inspectorBefore_;
    bool inspectorEditing_ = false;
    int foliageTool_ = 0;
    int foliageType_ = 0;
    float brushRadius_ = 2.5f;
    float brushDensity_ = 1.0f;
    float brushSpacing_ = 1.25f;
    float foliageScaleMin_ = 0.8f;
    float foliageScaleMax_ = 1.2f;
    bool foliageStrokeActive_ = false;
    bool foliageStrokeChanged_ = false;
    DirectX::XMFLOAT3 lastFoliageStamp_ = { 100000.0f, 0.0f, 100000.0f };
    LevelDefinition foliageStrokeBefore_;
    uint32_t foliageRandom_ = 0x52a7d91bu;
    std::vector<LevelDefinition> undo_;
    std::vector<LevelDefinition> redo_;
    std::filesystem::path currentPath_;
    std::vector<std::filesystem::path> levelFiles_;
    int loadSelection_ = -1;
    char levelName_[128] = "Level 1 Copy";
    char saveName_[128] = "level_1_copy";
    std::string status_;
};

#endif
