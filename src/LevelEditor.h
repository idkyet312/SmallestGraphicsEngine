#ifndef LEVEL_EDITOR_H
#define LEVEL_EDITOR_H

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "LevelDefinition.h"
#include "PrefabRegistry.h"
#include "AssetRegistry.h"
#include "GunAudio.h"
#include <DirectXMath.h>
#include <filesystem>
#include <functional>
#include <future>
#include <string>
#include <vector>

class Camera;

struct LevelEditorActions {
    bool levelChanged = false;
    bool beginPlay = false;
    bool stopPlay = false;
    bool returnToMenu = false;
    bool rebuildDXRDDGI = false;
    bool resetDXRDDGIHistory = false;
};

struct LevelDXRDDGIStatus {
    bool supported = false;
    bool updatesActive = false;
    uint32_t probeCount = 0;
    uint32_t raysPerFrame = 0;
    uint64_t gpuMemoryBytes = 0;
    std::string cacheStatus = "Not built";
};

class LevelEditor {
public:
    void NewFromLevelOne();
    void BeginPlay();
    void StopPlay();
    bool IsPlaying() const { return playing_; }
    bool ImportInProgress() const { return pendingImport_.valid(); }
    void OpenAssetBrowser() { assetBrowserOpen_ = true; }
    bool IsDirty() const { return dirty_; }
    void RefreshAssets();
    const LevelDefinition& Level() const { return level_; }
    LevelDefinition& Level() { return level_; }
    void MarkRuntimeSynchronized() { runtimeDirty_ = false; }
    bool RuntimeDirty() const { return runtimeDirty_; }
    bool FoliageRuntimeDirty() const { return foliageRuntimeDirty_; }
    void MarkFoliageRuntimeSynchronized() { foliageRuntimeDirty_ = false; }
    bool TerrainRuntimeDirty() const { return terrainRuntimeDirty_; }
    void MarkTerrainRuntimeSynchronized() { terrainRuntimeDirty_ = false; }
    bool DXRDDGIRuntimeDirty() const { return dxrDDGIRuntimeDirty_; }
    bool DXRDDGILayoutDirty() const { return dxrDDGILayoutDirty_; }
    void MarkDXRDDGIRuntimeSynchronized() {
        dxrDDGIRuntimeDirty_ = false;
        dxrDDGILayoutDirty_ = false;
    }
    void SetDXRDDGIStatus(const LevelDXRDDGIStatus& status) {
        dxrDDGIStatus_ = status;
    }
    void OnKeyDown(unsigned key, bool controlDown);
    LevelEditorActions Render(Camera& camera,
        DirectX::CXMMATRIX view, DirectX::CXMMATRIX projection,
        const std::function<float(float, float)>& terrainHeight,
        const std::function<uint64_t(const PrefabAsset&)>& thumbnailTexture = {});

private:
    struct AssetFileChange {
        std::filesystem::path path;
        bool beforeExists = false;
        bool afterExists = false;
        std::string before;
        std::string after;
    };
    struct PendingImportResult {
        PrefabSaveResult result;
        std::filesystem::path savedPrefab;
        PrefabRegistry prefabRegistry;
        AssetRegistry assetRegistry;
    };
    LevelEntity* Selected();
    const LevelEntity* Selected() const;
    void SelectFromViewport(DirectX::CXMMATRIX view,
                            DirectX::CXMMATRIX projection);
    void PushUndo(const LevelDefinition& before);
    void Undo();
    void Redo();
    AssetFileChange CaptureAssetBefore(const std::filesystem::path& path) const;
    void FinishAssetChange(AssetFileChange change);
    void UndoAsset();
    void RedoAsset();
    void AddEntity(LevelEntityType type);
    void AddPrefab(const PrefabAsset& prefab, const Camera& camera,
        const std::function<float(float, float)>& terrainHeight);
    void DuplicateSelected();
    void DeleteSelected();
    bool SaveTo(const std::filesystem::path& path);
    bool BrowseSaveAs();
    bool BrowseImportModel();
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
    bool TerrainChanged(const LevelDefinition& before) const;
    void SculptTerrain(DirectX::CXMMATRIX view, DirectX::CXMMATRIX projection,
        const std::function<float(float, float)>& terrainHeight);

    LevelDefinition level_ = MakeLevelOneTemplate();
    LevelDefinition playSnapshot_;
    uint64_t selectedId_ = 0;
    uint64_t nextId_ = 1;
    bool playing_ = false;
    bool dirty_ = false;
    bool runtimeDirty_ = true;
    bool foliageRuntimeDirty_ = true;
    bool terrainRuntimeDirty_ = true;
    bool dxrDDGIRuntimeDirty_ = true;
    bool dxrDDGILayoutDirty_ = true;
    LevelDXRDDGIStatus dxrDDGIStatus_;
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
    int terrainTool_ = 0;
    float terrainBrushRadius_ = 3.0f;
    float terrainBrushStrength_ = 0.45f;
    float terrainBrushSpacing_ = 1.0f;
    float terrainFlattenHeight_ = 0.0f;
    bool terrainStrokeActive_ = false;
    bool terrainStrokeChanged_ = false;
    DirectX::XMFLOAT3 lastTerrainStamp_ = { 100000.0f, 0.0f, 100000.0f };
    LevelDefinition terrainStrokeBefore_;
    std::vector<LevelDefinition> undo_;
    std::vector<LevelDefinition> redo_;
    std::filesystem::path currentPath_;
    std::vector<std::filesystem::path> levelFiles_;
    int loadSelection_ = -1;
    char levelName_[128] = "Level 1 Copy";
    char saveName_[128] = "level_1_copy";
    std::string status_;
    PrefabRegistry prefabRegistry_;
    AssetRegistry assetRegistry_;
    GunAudio audioPreview_;
    bool assetBrowserOpen_ = true;
    int assetKindTab_ = 3;
    int selectedPrefab_ = -1;
    int prefabDraftIndex_ = -1;
    PrefabAsset prefabDraft_;
    char assetFilter_[128] = {};
    char prefabAudioPath_[260] = {};
    char prefabMaterialMesh_[128] = {};
    int prefabMaterialTextureSelection_ = 0;
    int prefabLodModelSelection_ = 0;
    float prefabNewLodDistance_ = 25.0f;
    int prefabChildSelection_ = 0;
    std::future<PendingImportResult> pendingImport_;
    std::vector<AssetFileChange> pendingImportChanges_;
    std::vector<std::vector<AssetFileChange>> assetUndo_;
    std::vector<std::vector<AssetFileChange>> assetRedo_;
};

#endif
