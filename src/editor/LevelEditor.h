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
    bool fullReconcile = false;
    // Force the viewport to rebuild its built-in and prefab visuals, loading
    // whatever is not cached yet.
    //
    // The per-edit sync path deliberately skips model loading to stay cheap, so
    // a prefab placed before its .glb reached the cache draws nothing. That
    // normally self-heals a frame later, but a placement that lands while no
    // other rebuild is pending can leave the prop missing until an unrelated
    // edit forces a compile. This is the manual way out.
    bool refreshVisuals = false;
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
    // Replaces the edited level with the one at `path`, resetting undo, ids and
    // every derived-state dirty flag. Returns false and leaves the current level
    // untouched if the file will not load; `status` carries the reason.
    // Shared by the editor's own Load popup and the main menu's editor launcher
    // so there is exactly one definition of what loading a level means.
    bool LoadFrom(const std::filesystem::path& path);
    void BeginPlay();
    void StopPlay();
    bool IsPlaying() const { return playing_; }
    // True while the user is mid-edit (dragging the gizmo, painting foliage, or
    // sculpting terrain). Edit-time synchronization is visual-only; gameplay
    // state is reconciled at explicit Save/Play boundaries.
    bool IsInteracting() const {
        return gizmoWasUsing_ || inspectorEditing_ || foliageStrokeActive_ ||
               terrainStrokeActive_ || splineDragActive_;
    }
    bool ImportInProgress() const { return pendingImport_.valid(); }
    void OpenAssetBrowser() { assetBrowserOpen_ = true; }
    // Whether the editor viewport wants volumetric fog drawn. The editor does
    // not reach into Scene itself, so the caller reads this and applies it.
    bool FogEnabled() const { return fogEnabled_; }
    // Whether the viewport wants the deployment overview's terrain LOD.
    bool BirdseyeEnabled() const { return birdseyeEnabled_; }
    // Whether the viewport wants the walkable navmesh drawn over the ground.
    bool NavmeshEnabled() const { return navmeshEnabled_; }
    bool IsDirty() const { return dirty_; }
    void RefreshAssets();
    const LevelDefinition& Level() const { return level_; }
    LevelDefinition& Level() { return level_; }
    void MarkRuntimeSynchronized() {
        runtimeDirty_ = false;
        transformRuntimeDirty_ = false;
        transformRuntimeEntityId_ = 0;
    }
    bool RuntimeDirty() const { return runtimeDirty_; }
    bool TransformRuntimeDirty() const { return transformRuntimeDirty_; }
    uint64_t TransformRuntimeEntityId() const {
        return transformRuntimeEntityId_;
    }
    bool FoliageRuntimeDirty() const { return foliageRuntimeDirty_; }
    void MarkFoliageRuntimeSynchronized() { foliageRuntimeDirty_ = false; }
    bool TerrainRuntimeDirty() const { return terrainRuntimeDirty_; }
    void MarkTerrainRuntimeSynchronized() { terrainRuntimeDirty_ = false; }
    // Latched derived-state dirtiness for the next Save/Play reconciliation.
    // Wider than FoliageRuntimeDirty(): navmesh and grass also consume houses,
    // humvees, rocks, prefab colliders and terrain.
    bool EnvironmentRuntimeDirty() const { return environmentRuntimeDirty_; }
    void MarkEnvironmentRuntimeSynchronized() {
        environmentRuntimeDirty_ = false;
        foliageRuntimeDirty_ = false;
        terrainRuntimeDirty_ = false;
    }
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
    void MarkChanged(const LevelDefinition& before,
                     uint64_t transformEntityId = 0);
    void TrackItemEdit(const LevelDefinition& before, bool changed,
                       uint64_t transformEntityId = 0);
    void MarkTransformRuntimeDirty(uint64_t entityId);
    void PaintFoliage(DirectX::CXMMATRIX view, DirectX::CXMMATRIX projection,
        const std::function<float(float, float)>& terrainHeight);
    void SplineTool(DirectX::CXMMATRIX view, DirectX::CXMMATRIX projection,
        const std::function<float(float, float)>& terrainHeight);
    // Regenerates every spline run and refreshes the derived-state flags.
    // Called after any edit that moves control points or retunes a run.
    void RebuildSplines(const std::function<float(float, float)>& terrainHeight);
    LevelSplinePath* ActiveSpline();
    const LevelSplinePath* ActiveSpline() const;
    bool TerrainPointUnderMouse(DirectX::CXMMATRIX view,
        DirectX::CXMMATRIX projection,
        const std::function<float(float, float)>& terrainHeight,
        DirectX::XMFLOAT3& point) const;
    bool FoliageChanged(const LevelDefinition& before) const;
    bool TerrainChanged(const LevelDefinition& before) const;
    bool EnvironmentChanged(const LevelDefinition& before) const;
    bool IsFavouritePrefab(const std::string& prefabId) const;
    void ToggleFavouritePrefab(const std::string& prefabId);
    void LoadFavourites();
    void SaveFavourites() const;
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
    bool transformRuntimeDirty_ = false;
    uint64_t transformRuntimeEntityId_ = 0;
    bool foliageRuntimeDirty_ = true;
    bool terrainRuntimeDirty_ = true;
    bool environmentRuntimeDirty_ = true;
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
    // Spline runs. 0 select, 1 draw (append points), 2 edit (drag points).
    int splineTool_ = 0;
    // Prefab repeated along new runs, stored by id rather than registry index:
    // PrefabRegistry::Refresh reorders its vector, so an index silently points
    // at a different asset after any content change.
    std::string splinePrefabId_;
    uint64_t activeSplineId_ = 0;
    // Control point being dragged, and the snapshot taken when the drag began
    // so the whole drag collapses into one undo entry.
    int splineDragPoint_ = -1;
    bool splineDragActive_ = false;
    // Set when splines arrive without a terrain sampler to bake against (level
    // load, undo/redo). Render owns the sampler, so the bake happens there.
    bool splinesNeedBake_ = false;
    LevelDefinition splineDragBefore_;
    float splineSpacing_ = 3.108f;
    float splineYawOffset_ = 0.0f;
    bool splineAlignToPath_ = true;
    bool splineConformToTerrain_ = true;
    bool splinePitchToSlope_ = true;
    bool splineClosed_ = false;
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
    std::vector<std::string> validationErrors_;
    PrefabRegistry prefabRegistry_;
    AssetRegistry assetRegistry_;
    GunAudio audioPreview_;
    bool assetBrowserOpen_ = true;
    // Editor-only fog suppression. Volumetric fog is authored per level and per
    // time of day, so a dense preset hides whatever is being placed at the far
    // end of the map; turning it off is a view setting for the person editing,
    // not a change to the level. Nothing here is serialized, and the scene flag
    // is restored the moment the toggle goes back on.
    bool fogEnabled_ = true;
    // Draws the editor viewport with the deployment overview's terrain topology
    // instead of the gameplay clipmap: uniform 8 m tiles at full tessellation
    // all the way out, rather than exponentially coarser outer rings. Costs
    // more than the clipmap, which is why it is opt-in, but it is the only way
    // to judge distant terrain edits without flying the camera out to them.
    bool birdseyeEnabled_ = false;
    // Draws the walkable navmesh over the terrain so the holes props punch in it
    // are visible while authoring. Off by default: it is an authoring aid, and
    // the fills are opaque enough to obscure the level underneath.
    bool navmeshEnabled_ = false;
    int assetKindTab_ = 3;
    // Prefab ids the user starred. Kept as ids rather than indices because the
    // registry reorders on every Refresh -- an index would silently point at a
    // different prefab after an import. Persisted to assetcache/favourites.json.
    std::vector<std::string> favouritePrefabs_;
    bool favouritesLoaded_ = false;
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
