#ifndef RUNTIME_WORLD_H
#define RUNTIME_WORLD_H

#include "LevelDefinition.h"
#include "PrefabRuntime.h"

#include <cstdint>
#include <utility>
#include <vector>

enum class RuntimeTerrainSource {
    Empty,
    Authored,
    Preserve
};

// Ownership boundary for mutable level-derived game state.
//
// LevelDefinition remains the serialized authoring format. RuntimeWorld owns
// the active copy plus transient terrain and compiled prefab state. Editor,
// loading, gameplay, and rendering use this boundary instead of independently
// owning parallel copies.
class RuntimeWorld {
public:
    RuntimeWorld()
        : level_(MakeLevelOneTemplate()),
          terrainSculpt_(level_.terrainSculpt) {}

    const LevelDefinition& Level() const { return level_; }
    LevelDefinition& Level() { return level_; }

    const std::vector<TerrainSculptStamp>& TerrainSculpt() const {
        return terrainSculpt_;
    }
    std::vector<TerrainSculptStamp>& TerrainSculpt() {
        return terrainSculpt_;
    }

    const PrefabRuntimeState& Prefabs() const { return prefabs_; }
    PrefabRuntimeState& Prefabs() { return prefabs_; }

    uint64_t LevelRevision() const { return levelRevision_; }
    uint64_t TransformRevision() const { return transformRevision_; }
    uint64_t TerrainRevision() const { return terrainRevision_; }

    void ReplaceLevel(LevelDefinition level,
                      RuntimeTerrainSource terrainSource =
                          RuntimeTerrainSource::Authored) {
        level_ = std::move(level);
        if (terrainSource == RuntimeTerrainSource::Authored)
            terrainSculpt_ = level_.terrainSculpt;
        else if (terrainSource == RuntimeTerrainSource::Empty)
            terrainSculpt_.clear();
        ++levelRevision_;
        ++transformRevision_;
        ++terrainRevision_;
    }

    void ReplaceFromEditor(const LevelDefinition& level) {
        ReplaceLevel(level, RuntimeTerrainSource::Authored);
    }

    // Cheap edit-time path. Refuses structural changes so callers can fall back
    // to a full rebuild. IDs and ordering are checked before any mutation.
    bool SynchronizeEditorTransforms(const LevelDefinition& edited) {
        if (edited.entities.size() != level_.entities.size()) return false;
        for (size_t i = 0; i < edited.entities.size(); ++i) {
            if (edited.entities[i].id != level_.entities[i].id) return false;
        }
        for (size_t i = 0; i < edited.entities.size(); ++i) {
            level_.entities[i].transform = edited.entities[i].transform;
            level_.entities[i].enabled = edited.entities[i].enabled;
        }
        ++transformRevision_;
        return true;
    }

    void AddRuntimeTerrainStamp(const TerrainSculptStamp& stamp) {
        if (terrainSculpt_.size() >= kMaxTerrainSculptStamps)
            terrainSculpt_.erase(terrainSculpt_.begin());
        terrainSculpt_.push_back(stamp);
        ++terrainRevision_;
    }

private:
    LevelDefinition level_;
    std::vector<TerrainSculptStamp> terrainSculpt_;
    PrefabRuntimeState prefabs_;
    uint64_t levelRevision_ = 0;
    uint64_t transformRevision_ = 0;
    uint64_t terrainRevision_ = 0;
};

#endif
