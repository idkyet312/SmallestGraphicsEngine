#ifndef LEVEL_DEFINITION_H
#define LEVEL_DEFINITION_H

#include <array>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>

enum class LevelEntityType {
    PlayerSpawn,
    WoodHouse,
    MetalHouse,
    Palm,
    ExplosiveBarrel,
    EnemySpawn,
    AllySpawn,
    Humvee,
    Helicopter,
    GrassPatch,
    Dandelion,
    Rock,
    Prefab
};

struct Transform {
    float position[3] = { 0.0f, 0.0f, 0.0f };
    float rotation[3] = { 0.0f, 0.0f, 0.0f };
    float scale[3] = { 1.0f, 1.0f, 1.0f };
};

struct LevelEntity {
    uint64_t id = 0;
    LevelEntityType type = LevelEntityType::EnemySpawn;
    std::string name;
    // Registry ID for generic data-driven entities. Built-in entity types leave
    // this empty; Prefab entities resolve it through PrefabRegistry.
    std::string prefabId;
    // Per-instance component values. Stored verbatim and recursively merged on
    // top of prefab defaults at runtime.
    nlohmann::json overrides = nlohmann::json::object();
    Transform transform;
    bool enabled = true;
};

// One authored control point of a spline. Stored in world space, like every
// other authored position in the level.
struct LevelSplinePoint {
    float position[3] = { 0.0f, 0.0f, 0.0f };
};

// A run of prefab instances placed along a curve (fences, walls, barricades).
// The control points are what gets saved, not the segments they produce, so the
// run stays re-editable after a save/load round trip and the level file holds a
// handful of points instead of hundreds of entities. Segments are regenerated
// from these on load; see BakeSplineEntities.
struct LevelSplinePath {
    uint64_t id = 0;
    std::string name;
    // Registry ID of the prefab repeated along the curve.
    std::string prefabId;
    // Metres between consecutive segments, measured along the curve. Defaults to
    // the chain-link fence's 3.108 m footprint; other props override it.
    float spacing = 3.108f;
    // Degrees added to the tangent yaw, for assets not modelled along +X.
    float yawOffset = 0.0f;
    bool alignToPath = true;
    bool conformToTerrain = true;
    // Tilt each segment to match the ground slope so a solid base hugs the
    // terrain instead of stepping. Wrong for free-standing posts, hence a flag.
    bool pitchToSlope = true;
    bool closed = false;
    std::vector<LevelSplinePoint> points;
};

enum class TerrainSculptOperation : uint32_t {
    Add = 0,
    Flatten = 1,
    Heightmap = 2
};

struct TerrainSculptStamp {
    float x = 0.0f;
    float z = 0.0f;
    float radius = 2.0f;
    TerrainSculptOperation operation = TerrainSculptOperation::Add;
    float value = 0.0f;
    float strength = 1.0f;
    // Filename within TerrainStampDirectory. Kept relative so authored levels
    // remain portable between the source tree and packaged Content folder.
    std::string texture;
    float rotation = 0.0f;
    // Heightmap stamps only. 0 = additive (the stamp's relief is added on top
    // of whatever ground is already there), 1 = replace (inside the stamp the
    // ground *becomes* baseHeight + relief, erasing the procedural terrain).
    // Values between cross-fade, so a half-replace stamp flattens the noise
    // without fully cutting the island out.
    float replace = 0.0f;
    // World height the replace target is built around: the terrain height
    // sampled where the stamp was placed. Stored rather than recomputed so a
    // replace stamp stays put when a later stamp changes the ground under it.
    float baseHeight = 0.0f;
    // Where the border feather starts, as a fraction of the half-width: 0.82
    // means the outer 18% ramps the stamp's influence to nothing. Lower values
    // spread that ramp inward over more ground, which is what stops a stamp
    // whose relief is still tall at its border from cutting a straight edge
    // into the terrain. 1 disables the feather entirely (a hard square edge).
    //
    // Defaulted to the value that used to be hardcoded, so levels authored
    // before this was tunable load and render unchanged.
    float edgeFalloff = 0.82f;
};

// A circle where the automatically scattered ground cover is suppressed.
//
// Procedural grass and dandelions are not level entities -- they are generated
// at environment build from a density function -- so the foliage Erase tool,
// which deletes entities, cannot touch them. These stamps are the only way to
// clear auto-scattered cover, and they persist with the level.
struct FoliageClearStamp {
    float x = 0.0f;
    float z = 0.0f;
    float radius = 3.0f;
};

// Bounds the exclusion test, which is linear per blade at scatter time.
inline constexpr size_t kMaxFoliageClearStamps = 512;

// Hard cap on live sculpt stamps. Bounds the GPU upload buffer
// (kMaxTerrainSculptStamps * 32B) and, more importantly, the per-sample cost:
// both TerrainRendererDX12::HeightAt and terrain_ms.hlsl's TerrainHeight loop
// every stamp for every height query, with no spatial acceleration. Raising
// this scales that loop linearly -- 1024 stamps is ~8x the Level-1 budget and
// still only a 32 KB buffer.
inline constexpr size_t kMaxTerrainSculptStamps = 1024;
// Interactive radial/heightmap brushes are capped at 64 m in the editor, but a
// baked heightmap encloses the complete sculpt stack and can legitimately span
// a whole island. The shader stores radius as a float and supports this larger
// footprint without increasing the per-vertex loop cost.
inline constexpr float kMaxBakedTerrainStampRadius = 4096.0f;

// Upper bound on the DDGI probe count a level may request.
//
// This is a budget ceiling, not a hardware or format limit: DXRDDGIRenderer
// sizes the irradiance and visibility atlases from the ACTUAL probe count
// (CreateAtlases lays probes out in a ceil(sqrt(n)) grid of 10x10 and 18x18
// texel tiles), so nothing in the GPU path assumes a maximum.
//
// It was 2048, which is under one probe per 8m tile on a 128x128m island at the
// default 3.0m spacing -- so large outdoor levels silently ran out of probes,
// most of the ground fell outside the layout, and SampleSparseDDGI returned zero
// there. Those pixels then fell through to one stochastic GI ray each, which is
// far noisier and more expensive than the probes would have been.
//
// 8192 costs roughly 35 MB of atlas across both double-buffered targets, which
// is in line with what the visibility buffer already owns. Raise it further only
// with that arithmetic redone -- the atlases grow linearly in probe count.
inline constexpr uint32_t kMaxDDGIProbes = 8192;

struct LevelDXRDDGISettings {
    bool enabled = false;
    float surfaceSpacing = 3.0f;
    float surfaceOffset = 0.35f;
    uint32_t maxProbes = 2048;
    uint32_t raysPerProbe = 64;
    uint32_t probesPerFrame = 16;
    float maxRayDistance = 24.0f;
    float intensity = 0.45f;
    float normalBias = 0.18f;
    float viewBias = 0.05f;
    float hysteresis = 0.95f;
    float multiBounceStrength = 0.35f;
    bool showProbes = false;
};

// How the player arrives at the start of a level.
enum class LevelInsertionMode : uint32_t {
    // Flown in by the BlackHawk. The historical behaviour, and the default for
    // levels saved before this field existed.
    Helicopter = 0,
    // Brought in by the insertion boat instead. For maps that start on water.
    Boat = 1,
    // A double-speed BlackHawk run that holds above the spawn while the player
    // rappels down, instead of landing.
    FastRappel = 2,
    // The player picks between both helicopter runs and the boat before the
    // level starts. Only maps that set this offer the choice.
    PlayerChoice = 3
};

inline constexpr float kDefaultDeploymentRadius = 34.0f;
inline constexpr float kMinDeploymentRadius = 5.0f;
inline constexpr float kMaxDeploymentRadius = 600.0f;

struct LevelDefinition {
    uint32_t schemaVersion = 1;
    std::string name = "Untitled Level";
    // Which insertion delivers the player. PlayerChoice puts the map's arrival
    // up to the player; the other modes settle it in the level file.
    LevelInsertionMode insertionMode = LevelInsertionMode::Helicopter;
    // World-space radius of the selectable insertion/drop-off ring. It is not
    // multiplied by island scale: an authored 120 m radius stays exactly 120 m.
    float deploymentRadius = kDefaultDeploymentRadius;
    // The autonomous armed patrol boat is level scenery/gameplay, separate from
    // the insertion and extraction boats selected by mission flow.
    bool patrolBoatEnabled = true;
    float terrainHeightScale = 3.057f;
    // Flat authoring mode: suppress every procedural landform -- the fbm relief,
    // the pool basin carved near the origin, and the beach/seabed coast falloff
    // -- leaving a level plane at kLandLift for sculpting from scratch.
    //
    // A separate flag rather than just terrainHeightScale = 0 because the pool
    // and the coastline are not scaled by it: zeroing the height alone still
    // leaves a 3 m crater near spawn and an island-shaped drop-off at the shore.
    bool terrainFlat = false;
    // Island builder: terrain drawn extent (tile grid) and coastline scale. The
    // ocean is procedurally ringed around the land, so growing tiles + island
    // scale together makes a bigger island with more open water around it.
    uint32_t terrainTilesX = 16;
    uint32_t terrainTilesZ = 16;
    // Per-axis island size: the land coastline stretches independently along X
    // and Z, so the island can be a wide oval or a long strip, not just a
    // uniform disc. 1.0 = the original Level 1 radius on that axis.
    float terrainIslandScaleX = 1.0f;
    float terrainIslandScaleZ = 1.0f;
    // Grid min-corner offset in tiles from the origin. Zero keeps the grid
    // centered (legacy behaviour: origin = -tiles/2 .. +tiles/2). Extending an
    // edge in the editor grows one dimension and shifts this so the new tiles
    // appear on the clicked side instead of forcing symmetric growth.
    int32_t terrainOriginTileX = 0;
    int32_t terrainOriginTileZ = 0;
    std::vector<TerrainSculptStamp> terrainSculpt;
    // Circles where auto-scattered grass and dandelions are suppressed. Empty
    // on every existing level, which therefore scatters exactly as before.
    std::vector<FoliageClearStamp> foliageClear;
    // Hand-painted terrain layer weights, stored as an RGBA8 PNG sidecar beside
    // the level JSON (<Name>_splat.png). Channels are grass/dirt/sand/rock; an
    // all-zero texel means "use the procedural weights", so a level without a
    // sidecar renders exactly as it did before this feature existed.
    //
    // 0 = no splatmap. The pixels live in memory as well as on disk because the
    // editor's undo snapshots the whole LevelDefinition; at 512x512 that is 1 MB
    // per undo entry.
    uint32_t terrainSplatResolution = 0;
    std::vector<uint8_t> terrainSplatRGBA;
    // Bumped on every painted stroke. TerrainChanged() compares this instead of
    // the pixel buffer so the per-frame equality check stays O(1) rather than a
    // megabyte memcmp, and it survives undo snapshots like any other field.
    uint32_t terrainSplatRevision = 0;
    LevelDXRDDGISettings dxrDDGI;
    std::vector<LevelEntity> entities;
    // Authored prefab runs. Empty on every existing level, which therefore
    // loads and saves exactly as before.
    std::vector<LevelSplinePath> splines;
};

struct LevelValidationResult {
    bool ok = false;
    std::vector<std::string> errors;
};

struct LevelLoadResult {
    bool ok = false;
    LevelDefinition level;
    std::string error;
};

struct LevelSaveResult {
    bool ok = false;
    std::string error;
};

const char* LevelEntityTypeName(LevelEntityType type);
bool ParseLevelEntityType(const std::string& text, LevelEntityType& type);
const char* LevelInsertionModeName(LevelInsertionMode mode);
bool ParseLevelInsertionMode(const std::string& text, LevelInsertionMode& mode);
LevelDefinition MakeLevelOneTemplate();
// Empty level on a flat plane: one player spawn, no island and no props.
LevelDefinition MakeFlatLevelTemplate();
LevelValidationResult ValidateLevel(const LevelDefinition& level);
LevelLoadResult LoadLevel(const std::filesystem::path& path);
LevelSaveResult SaveLevel(const LevelDefinition& level,
                          const std::filesystem::path& path);

// Where a level's painted terrain weights live: the level path with its
// extension replaced by "_splat.png". Exposed so the editor can report and
// delete the sidecar without duplicating the naming rule.
std::filesystem::path TerrainSplatSidecarPath(
    const std::filesystem::path& levelPath);

// Samples a Catmull-Rom curve through a spline's control points. Interpolating,
// so the authored points lie exactly on the curve. Returns an empty vector for
// fewer than two points.
//
// samplesPerSpan controls only the resolution of the polyline used for drawing
// and for measuring arc length; it is not the segment spacing.
std::vector<std::array<float, 3>> SampleSplineCurve(const LevelSplinePath& spline,
                                                    int samplesPerSpan = 16);

// One prefab instance the spline wants placed: where it goes, how it is turned,
// and the small along-path fit needed for its far end to meet the next one.
// Angles are degrees, matching Transform.
struct SplineSegmentPlacement {
    float position[3] = { 0.0f, 0.0f, 0.0f };
    float yawDegrees = 0.0f;
    float pitchDegrees = 0.0f;
    float lengthScale = 1.0f;
};

// Divides the curve into approximately spline.spacing-long runs and returns one
// placement per segment. Adjacent placements share exact endpoints; lengthScale
// fits an asset whose local +X footprint equals spacing to the sampled chord.
//
// Spacing is measured by arc length rather than curve parameter: sampling a
// Catmull-Rom at uniform t bunches points on tight turns and stretches them on
// straights, which shows up as fence segments overlapping and then gapping.
//
// terrainHeight may be null, in which case the control points' own heights are
// interpolated and conformToTerrain/pitchToSlope have no effect.
std::vector<SplineSegmentPlacement> EvaluateSplineSegments(
    const LevelSplinePath& spline,
    const std::function<float(float, float)>& terrainHeight);

// Key under which a baked segment records the spline that produced it, so a
// re-bake can replace exactly its own segments and leave hand-placed props
// alone.
inline constexpr const char* kSplineOwnerKey = "splineOwner";

// Regenerates every spline's segments into level.entities: drops entities
// previously baked from splines, then appends fresh ones. Hand-placed entities
// are untouched. nextId is advanced past the ids it hands out.
void BakeSplineEntities(LevelDefinition& level, uint64_t& nextId,
                        const std::function<float(float, float)>& terrainHeight);

#endif
