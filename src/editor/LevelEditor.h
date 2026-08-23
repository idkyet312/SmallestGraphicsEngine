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
    // Discards the current level for an empty flat plane with one spawn.
    void NewFlat();
    void BeginPlay();
    void StopPlay();
    bool IsPlaying() const { return playing_; }
    // True while the user is mid-edit (dragging the gizmo, painting foliage, or
    // sculpting terrain). The runtime sync (asset refresh + prefab reload + GPU
    // rebuild) is heavy and must not run every drag frame - it lags and races
    // GPU work. Callers defer the heavy sync until interaction settles.
    bool IsInteracting() const {
        return gizmoWasUsing_ || foliageStrokeActive_ || terrainStrokeActive_;
    }
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
    // Terrain tool 5: paint layer weights into the level's splatmap. Shares the
    // sculpt brush's stroke/undo/spacing machinery but writes texels instead of
    // height stamps. islandHalfExtent is the same world->UV frame the resolve
    // uses, so the editor and the shader cannot disagree about where paint lands.
    void PaintTerrain(DirectX::CXMMATRIX view, DirectX::CXMMATRIX projection,
        const std::function<float(float, float)>& terrainHeight,
        float islandHalfExtentX, float islandHalfExtentZ);
    // Allocates the splatmap on first paint. Returns false if the level has no
    // usable island extent to map against.
    bool EnsureTerrainSplatMap();
    // Grow the terrain tile grid by one row/column on a side (0=+X,1=-X,2=+Z,
    // 3=-Z), shifting the grid origin so the new tiles land on that side.
    void ExtendTerrain(int direction);
    // Draw the tile grid + highlighted extendable edges; click an edge to grow.
    void ExtendTerrainInteraction(DirectX::CXMMATRIX view,
        DirectX::CXMMATRIX projection);

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
    std::vector<std::string> terrainStampNames_;
    bool terrainStampLibraryScanned_ = false;
    int terrainStampSelection_ = 0;
    float terrainStampRadius_ = 16.0f;
    float terrainStampHeight_ = 6.0f;
    float terrainStampRotation_ = 0.0f;
    // 0 = the stamp's relief is added to the ground, 1 = it replaces it.
    float terrainStampReplace_ = 0.0f;
    // Lifts or sinks a replace stamp's target plane relative to the ground the
    // cursor is on, so a plateau can sit above the terrain it overwrites.
    float terrainStampBaseOffset_ = 0.0f;
    // Downsampled grayscale of the selected stamp, used to draw the heightmap
    // inside the placement square. Cached by filename: decoding a 4K 16-bit PNG
    // every frame the cursor moves would stall the editor.
    std::string stampPreviewName_;
    std::vector<float> stampPreviewHeights_;  // kStampPreviewGrid^2, 0..1
    bool stampPreviewValid_ = false;
    // Which layer the paint brush writes: 0 grass, 1 dirt, 2 sand, 3 rock.
    int terrainPaintLayer_ = 3;
    // 0..1 weight deposited at the brush centre, feathering to 0 at the rim.
    float terrainPaintStrength_ = 1.0f;
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
