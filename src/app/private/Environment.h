#pragma once

// Private application implementation; included once by main.cpp in dependency order.
WaterVolume                 g_water;
WaterVolume                 g_ocean;   // sea ringing the island, surface at y = 0
PalmTrees                   g_trees;
GrassField                  g_grass;

void MatchFoliageMaterialToGrass() {
    const XMFLOAT3 grassAlbedo = g_grass.Albedo();
    const XMFLOAT4 matchedAlbedo(
        grassAlbedo.x, grassAlbedo.y, grassAlbedo.z, 1.0f);
    const auto matchMaterial = [&](const std::shared_ptr<SceneMaterial>& material) {
        if (!material) return;
        material->baseColorFactor = matchedAlbedo;
        material->roughnessFactor = g_grass.Roughness();
        material->ambientScale = g_grass.AmbientScale();
        // Alpha foliage has no ORM or normal map, so these packed material
        // channels carry the grass direct, transmission, and variation controls.
        material->occlusionStrength = g_grass.DirectLightScale();
        material->normalYSign = g_grass.TransmissionStrength();
        material->viewFillStrength = g_grass.ColorVariation();
    };
    for (const auto& material : PalmModel::Materials()) {
        if (material && material->name == "palm_leaf")
            matchMaterial(material);
    }
    if (!g_dandelionModel) return;
    const auto updateMaterial = [&](const auto& self,
                                    const std::shared_ptr<SceneNode>& node) -> void {
        if (!node) return;
        if (node->mesh) {
            for (MeshPrimitive& primitive : node->mesh->primitives) {
                matchMaterial(primitive.material);
            }
        }
        for (const auto& child : node->children)
            self(self, child);
    };
    updateMaterial(updateMaterial, g_dandelionModel);
}

static SkyRendererDX12      skyRenderer;
static CloudNoiseDX12       g_cloudNoise;

static EnvironmentIBLDX12   environmentIBL;
DDGIRendererDX12            g_ddgiRenderer;
static DXRDDGIRenderer       g_dxrDDGI;
ID3D12Resource*             g_skyEnvironmentResource = nullptr;
ID3D12Resource*             g_specularEnvironmentResource = nullptr;
ID3D12Resource*             g_brdfIntegrationResource = nullptr;

// Night gets its own environment map: a sun direction below the horizon dims the
// analytic sky, but the starfield and moon can only come from the HDRI. The
// image-based lighting is baked from that same map, so the irradiance has to be
// recomputed alongside it -- otherwise the island keeps its daylight ambient
// bounce under a night sky.
//
// Separated from ApplyTimeOfDay only because skyRenderer is declared here, well
// after the rest of the lighting helpers.
// Set when the time of day picks a different sky. The swap itself cannot run
// from the UI: it resets the frame's command allocator and drains the GPU, and
// the render loop is midway through recording into that same list -- doing it
// inline reset the allocator out from under the open frame and then waited on a
// fence that frame would never signal, which hung on the spot.
//
// Serviced from the same early-frame point as the DDGI rebuild, before any pass
// binds the resources it replaces.
static bool g_skyEnvironmentSwapPending = false;
// Seconds still to wait before the queued swap runs. The preset's lighting is
// applied immediately; the environment map follows a beat later.
//
// The swap drains every GPU frame slot, decodes a 70 MB EXR and re-prefilters
// the IBL, so it costs a visible stall wherever it lands. Letting the cheap
// lighting change land first means the scene has already gone dark by the time
// that stall happens, instead of the frozen frame being the old daylight one.
static float g_skyEnvironmentSwapDelay = 0.0f;
static constexpr float kSkyEnvironmentSwapDelaySeconds = 1.0f;

static void RequestTimeOfDaySkyEnvironment(TimeOfDay) {
    // Every preset now shares the daylight environment map, so there is never a
    // swap to queue. Kept as a no-op rather than deleted: the time-of-day code
    // calls this from several places, and a preset that wants its own sky again
    // only has to fill this back in.
    g_skyEnvironmentSwapPending = false;
    g_skyEnvironmentSwapDelay = 0.0f;
}

// Runs between frames only. See g_skyEnvironmentSwapPending.
static void ApplyTimeOfDaySkyEnvironment(TimeOfDay) {
    // No-op for the same reason as RequestTimeOfDaySkyEnvironment above: with
    // one environment map there is nothing to upload and no IBL to re-derive.
    // Dropping the night EXR removed this function's cost entirely -- it used
    // to decode a 70 MB file and recompute the irradiance SH mid-run, which is
    // what made switching into Night hitch.
}
ID3D12Resource*             g_ddgiIrradianceResource = nullptr;
ID3D12Resource*             g_spotShadowAtlasResource = nullptr;
ID3D12Resource*             g_ddgiVisibilityResource = nullptr;
ID3D12Resource*             g_dxrDDGIProbeResource = nullptr;
ID3D12Resource*             g_dxrDDGICellResource = nullptr;
ID3D12Resource*             g_dxrDDGIIndexResource = nullptr;
// Published for the UI: DXR Tier 1.1 (inline RayQuery) support, which gates
// the enhanced-visuals tier. Set once the device is up.
bool                        g_inlineRaytracingSupported = false;
bool                        g_bindlessMaterialsReady = false;
bool                        g_bindlessMaterialsActive = false;
UINT                        g_dxrDDGIProbeCount = 0;
UINT                        g_dxrDDGICellCount = 0;
UINT                        g_dxrDDGIIndexCount = 0;
float                       g_dxrDDGICellSize = 0.0f;
static OcclusionDepthDX12   occlusionDepth;
static XMMATRIX             previousHZBViewProjection = XMMatrixIdentity();
static bool                 hzbCaptureActive = false;
static FXAADX12             fxaa;
static NightVisionDX12      nightVision;
// Goggles up/down, toggled with J when NVG is in the gear slot. Ramped rather
// than switched: raising goggles is a hand movement, and an instant cut to full
// green reads as a filter being applied rather than equipment being used.
static bool                 g_nightVisionActive = false;
static float                g_nightVisionBlend = 0.0f;
// Weapon light on/off, toggled with J when the flashlight is in the gear slot.
// Shares the key with the goggles without conflicting: the gear slot holds one
// item, so only one of the two can ever be equipped on a given run.
bool                        g_flashlightActive = false;
// Automatic gain control state, carried between frames because adaptation is a
// temporal effect the per-pixel shader has no memory to run itself. Starts wide
// open, which is where a tube sits before it has seen any light.
// Transient light the tube has to answer for: explosions, flares, lightning.
// Decays on its own, and any source can push it up without knowing about the
// others -- the AGC only sees a total. Separate from the scene's steady ambient
// because these are events, not a light level.
static float                g_nightVisionLightSpike = 0.0f;
// World position of the brightest recent flash, so the spike can fall off with
// distance rather than blinding the player from across the island.
static XMFLOAT3             g_nightVisionSpikeOrigin{};
static float                g_nightVisionGain = 9.0f;
// 0 in usable darkness, 1 when the scene is bright enough to wash the tube out.
static float                g_nightVisionOverload = 0.0f;

// Lightning. Only runs in Storm weather, and only drives light -- the strike
// itself is off-screen above the cloud deck, which is what most lightning looks
// like from under a storm.
static float                g_lightningCooldown = 4.0f;
// Current flash brightness, 0..1. Decays fast; a strike can re-trigger it while
// it is still falling, which is what produces the stutter of a multi-stroke
// flash rather than one clean pulse.
static float                g_lightningFlash = 0.0f;
static int                  g_lightningStrokesLeft = 0;
static float                g_lightningStrokeTimer = 0.0f;
// Ambient the storm flash adds to the scene, before the goggles see it.
static float                g_lightningAmbient = 0.0f;

// Puts lightning back to "storm just started". Called when a storm is selected
// and on level restart, because the state above is global and otherwise carries
// whatever the previous run stranded: a run that ended mid-flash leaves
// g_lightningStrokesLeft > 0, and the strike scheduler below refuses to arm
// while that is set, so the storm goes permanently silent.
//
// The first cooldown is short and fixed rather than the usual 5..16 s roll --
// picking Storm should produce lightning soon enough to confirm it is working.
static void ResetLightning() {
    g_lightningCooldown = 2.0f;
    g_lightningFlash = 0.0f;
    g_lightningStrokesLeft = 0;
    g_lightningStrokeTimer = 0.0f;
    g_lightningAmbient = 0.0f;
}

// Reports a flash of light at a world position. `brightness` is in the same
// units as the scene's ambient level, so 1.0 is roughly full daylight arriving
// at once.
//
// Kept as one entry point so explosions, muzzle flashes and lightning all reach
// the goggles the same way, and so a source only has to say how bright it is and
// where -- the falloff and the decay live here.
static void ReportNightVisionLightFlash(const XMFLOAT3& position,
                                        float brightness) {
    if (brightness <= 0.0f) return;
    // Brightest wins rather than summing: two grenades going off together are
    // not twice as blinding as one, and summing would let a firefight pin the
    // tube shut permanently.
    if (brightness <= g_nightVisionLightSpike) return;
    g_nightVisionLightSpike = brightness;
    g_nightVisionSpikeOrigin = position;
}

// Storm lightning: schedules strikes, runs the multi-stroke flicker, and
// publishes the light it throws.
//
// The flash is a light event rather than geometry -- no bolt is drawn. Under a
// full storm deck the channel is usually hidden in cloud anyway, and what sells
// it is the world going briefly bright and the thunder arriving late.
static void UpdateLightning(float deltaTime) {
    // Storm only. Any other weather winds the state down rather than freezing
    // it, so switching away mid-flash does not leave the scene lit.
    if (scene.weatherState != WeatherState::Storm) {
        g_lightningFlash = (std::max)(0.0f, g_lightningFlash - deltaTime * 4.0f);
        g_lightningStrokesLeft = 0;
        g_lightningStrokeTimer = 0.0f;
        g_lightningAmbient = g_lightningFlash * 0.35f;
        return;
    }

    // Between strikes. Interval is deliberately wide: evenly spaced lightning
    // reads as a strobe, and the wait is what makes each one land.
    g_lightningCooldown -= deltaTime;
    if (g_lightningCooldown <= 0.0f && g_lightningStrokesLeft <= 0) {
        g_lightningCooldown = 5.0f + RandomUnit() * 11.0f;
        // Two to four strokes, the way a real flash flickers rather than
        // pulsing once.
        g_lightningStrokesLeft = 2 + static_cast<int>(RandomUnit() * 3.0f);
        g_lightningStrokeTimer = 0.0f;

        // Thunder: one crack per flash, on the frame the flash begins.
        //
        // Queued here rather than per stroke -- a three-stroke flash fired
        // three overlapping thunderclaps, which read as a stutter rather than
        // one strike. Zero delay, because the strike is meant to be overhead:
        // the earlier speed-of-sound delay put the sound up to 2.6 s after the
        // light, long enough that the two stopped reading as the same event.
        g_pendingExplosionAudio.push_back({
            0.0f, 0.85f, 0.55f + RandomUnit() * 0.12f, false });
    }

    // Stroke sequence.
    if (g_lightningStrokesLeft > 0) {
        g_lightningStrokeTimer -= deltaTime;
        if (g_lightningStrokeTimer <= 0.0f) {
            --g_lightningStrokesLeft;
            g_lightningStrokeTimer = 0.04f + RandomUnit() * 0.09f;
            // Each stroke varies, so the flicker is uneven.
            g_lightningFlash = (std::max)(g_lightningFlash,
                                          0.55f + RandomUnit() * 0.45f);

            // The goggles take the full brunt. Reported at the camera rather
            // than a strike position: the flash lights the whole sky, so there
            // is no direction for it to fall off from, and a distance falloff
            // would wrongly spare a player standing in the open.
            //
            // This is what "brightens it up too much" means in practice -- a
            // storm at night repeatedly whites the tube out, so NVGs become a
            // liability in the weather they would otherwise be most useful in.
            ReportNightVisionLightFlash(scene.camera.Position,
                                        1.5f + RandomUnit() * 1.3f);
        }
    }

    // Decay. Fast enough to read as a flash rather than a light being switched
    // on, but slow enough that the eye catches it.
    g_lightningFlash = (std::max)(0.0f, g_lightningFlash - deltaTime * 5.5f);
    g_lightningAmbient = g_lightningFlash * 0.35f;
}
static VolumetricFogDX12    volumetricFog;
static LightShaftsDX12      lightShafts;
static ScreenSpaceAODX12    screenSpaceAO;
static ScreenSpaceReflectionsDX12 screenSpaceReflections;
static WaterRendererDX12    waterRenderer;

static void QueueWaterBathymetry() {
    if (scene.waterQuality == WaterQuality::Low ||
        !waterRenderer.UltraAvailable()) return;
    const TerrainRendererDX12::Params terrain = CurrentTerrainParams();
    constexpr float kShoreOuter = 88.0f;
    constexpr float kOceanMargin = 40.0f;
    const float halfX = kShoreOuter * terrain.islandScaleX + kOceanMargin;
    const float halfZ = kShoreOuter * terrain.islandScaleZ + kOceanMargin;
    WaterBathymetryDesc desc;
    desc.minimumXZ = {-halfX, -halfZ};
    desc.maximumXZ = {halfX, halfZ};
    desc.resolution = SelectBathymetryResolution(halfX * 2.0f, halfZ * 2.0f);
    const uint64_t scaleX = static_cast<uint64_t>(
        std::lround(terrain.islandScaleX * 1000.0f));
    const uint64_t scaleZ = static_cast<uint64_t>(
        std::lround(terrain.islandScaleZ * 1000.0f));
    const float waterSurfaceY = g_ocean.GetSurfaceY();
    const uint64_t surfaceRevision = static_cast<uint64_t>(
        static_cast<int64_t>(std::lround(waterSurfaceY * 1000.0f)));
    // Runtime crater/gouge stamps update the terrain buffer but deliberately do
    // not advance this revision. Re-running the full shoreline distance solve
    // for a local blast dimple caused the multi-second grenade hitch.
    desc.terrainRevision = TerrainRendererDX12::BathymetryRevision() ^
        (scaleX << 32) ^ (scaleZ << 16) ^ terrain.terrainStyle ^
        (surfaceRevision * 0x9e3779b97f4a7c15ull);
    desc.heightAt = [terrain, waterSurfaceY](float x, float z) {
        return TerrainRendererDX12::HeightAt(terrain, x, z) - waterSurfaceY;
    };
    waterRenderer.QueueBathymetryRebuild(desc);
}
static MSAADX12             msaa;
static GrassMSAADX12        grassMSAA;
static bool                 msaaUsedLastFrame = false;
static BindlessHeapDX12     bindlessHeap;
static VisibilityBufferDX12 visBuffer;
static SniperScopeDX12      g_sniperScope;
static ShadowMapDX12        shadowMap;
static GeometryBuffers      geo;
// Directional shadow state carried to the sniper scope pass.
//
// The scope renders early in the frame -- before the main view's shadow pass
// has run -- so it cannot use this frame's shadow map. The brief calls for one
// shadow render shared by both views, and the scope frustum is a subset of the
// main one, so the map from the PREVIOUS frame is reused instead of rendering
// a second one. Shadows lag the camera by a frame in the lens only; at scope
// magnification, against a shadow map that is itself camera-fitted and updated
// every frame, that is far less visible than the alternative of a scope with
// no shadows at all.
static XMMATRIX g_scopeShadowLightSpace = XMMatrixIdentity();
static ID3D12Resource* g_scopeShadowResource = nullptr;

static PackedGeometry       packed;
static std::shared_ptr<SceneNode> crateModel;
static std::shared_ptr<SceneNode> crateShadowModel;
static std::shared_ptr<SceneNode> wallModel;
static std::shared_ptr<SceneNode> normalWallModel;
static std::shared_ptr<SceneNode> stressWallModel;
bool g_showH2Model = false;
static std::shared_ptr<SceneMaterial> floorMaterial;
// Soft smoke sprite (RGBA, alpha-shaped) for billboard particles, plus the
// upload heap that must outlive the copy.
ComPtr<ID3D12Resource> g_smokeTexture;
ComPtr<ID3D12Resource> g_bloodTexture;
ComPtr<ID3D12Resource> g_muzzleFlashTexture;
ComPtr<ID3D12Resource> g_fireTexture;
ComPtr<ID3D12Resource> g_explosionTexture;
ComPtr<ID3D12Resource> g_explosionCoreTexture;
static std::vector<ComPtr<ID3D12Resource>> g_smokeUploadHeaps;
static std::vector<ComPtr<ID3D12Resource>> g_bloodUploadHeaps;
static std::vector<ComPtr<ID3D12Resource>> g_muzzleFlashUploadHeaps;
static std::vector<ComPtr<ID3D12Resource>> g_fireUploadHeaps;
static std::vector<ComPtr<ID3D12Resource>> g_explosionUploadHeaps;
static std::vector<ComPtr<ID3D12Resource>> g_explosionCoreUploadHeaps;
static bool                 fullLevelAssetsLoaded = false;
static bool                 emptyLevelAssetsLoaded = false;
static StaticBufferStatsDX12 levelLoadingUploadBaseline = {};
static std::future<bool> levelDestructionLoadFuture;
static bool levelDestructionLoadInFlight = false;
