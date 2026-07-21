#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <DbgHelp.h>
#pragma comment(lib, "dbghelp.lib")
#pragma comment(lib, "comdlg32.lib")
#include <iostream>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <functional>
#include <future>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx12.h>
#include <ImGuizmo.h>

#include "DX12Core.h"
#include "ProfilerDX12.h"
#include "GroundLevel.h"
#include "ShaderDX12.h"
#include "DDGI_DX12.h"
#include "VisibilityBufferDX12.h"
#include "StaticBufferDX12.h"
#include "Scene.h"
#include "ForwardRenderer.h"
#include "IdTechRenderer.h"
#include "RaytracingDX12.h"
#include "VirtualInput.h"
#include "EngineUI.h"
#include "GLBImporter.h"
#include "MipGenerator.h"
#include "ShadowMapDX12.h"
#include "MeshShaderDX12.h"
#include "TerrainRendererDX12.h"
#include "SkyRendererDX12.h"
#include "EnvironmentIBLDX12.h"
#include "OcclusionDepthDX12.h"
#include "FXAADX12.h"
#include "VolumetricFogDX12.h"
#include "ScreenSpaceAODX12.h"
#include "MSAADX12.h"
#include "GrassMSAADX12.h"
#include "DestructionDX12.h"
#include "FBXImporter.h"
#include "SkinnedFBXImporter.h"
#include "SkinnedEnemy.h"
#include "T3DPhysicsAsset.h"
#include "GunAudio.h"
#include "WaterVolume.h"
#include "PalmTrees.h"
#include "LevelDefinition.h"
#include "LevelEditor.h"

using namespace DirectX;

// ?? globals ??????????????????????????????????????????????????????????????????
static unsigned int SCR_WIDTH  = 1920;
static unsigned int SCR_HEIGHT = 1080;

static Scene               scene;
static ShaderDX12           mainShader;
ProfilerDX12                g_profiler;
UINT                        g_forwardDrawCalls = 0;
UINT                        g_shadowDrawCalls = 0;
UINT                        g_visibilityDrawCalls = 0;
UINT                        g_shadowBatches = 0;
UINT                        g_shadowBatchInstances = 0;
MeshShaderDX12              g_meshShader;
bool                        g_useMeshShader = false;
TerrainRendererDX12         g_terrain;
DestructionDX12             g_destruction;
std::vector<std::unique_ptr<SkinnedEnemy>> g_bandits;
SkinnedEnemy*               g_heldBandit = nullptr;
size_t                      g_heldBarrelIndex = SIZE_MAX;
std::shared_ptr<SceneNode>  g_explosiveBarrelModel;
std::shared_ptr<SceneNode>  g_explosiveBarrelShadowModel;
std::shared_ptr<SceneNode>  g_humveeModel;
std::shared_ptr<SceneNode>  g_humveeShadowModel;
std::shared_ptr<SceneNode>  g_helicopterModel;
std::shared_ptr<SceneNode>  g_dandelionModel;
std::vector<DandelionInstance> g_dandelionInstances;
static XMFLOAT3             g_dandelionSourceCenter{};
static float                g_dandelionSourceMinY = 0.0f;
static float                g_dandelionSourceHeight = 1.0f;
std::shared_ptr<SceneNode>  g_helicopterMainRotorNode;
std::shared_ptr<SceneNode>  g_helicopterTailRotorNode;
std::shared_ptr<SceneNode>  g_humveeTurretNode;
XMFLOAT3                    g_humveeModelCenter{ 0.0f, 0.0f, 0.0f };
float                       g_humveeModelMinY = 0.0f;
float                       g_humveeModelScale = 1.0f;
XMFLOAT3                    g_helicopterModelCenter{ 0.0f, 0.0f, 0.0f };
float                       g_helicopterModelScale = 1.0f;
float                       g_helicopterLevelScale = 1.0f;
float                       g_helicopterMainRotorAngle = 0.0f;
float                       g_helicopterTailRotorAngle = 0.0f;
float                       g_helicopterYaw = 0.0f;
float                       g_helicopterPitch = 0.0f;
float                       g_helicopterRoll = 0.0f;
float                       g_helicopterHoverTime = 0.0f;
float                       g_helicopterFireCooldown = 0.0f;
float                       g_helicopterFireCycleTime = 0.0f;
XMFLOAT3                    g_helicopterPosition{ 0.0f, 14.0f, 0.0f };
XMFLOAT3                    g_helicopterSpawn{ 0.0f, 14.0f, 0.0f };
XMFLOAT3                    g_secondaryHelicopterPosition{ 42.0f, 14.0f, 0.0f };
constexpr float             kHelicopterPatrolRadius = 16.0f;
float                       g_secondaryHelicopterYaw = 0.0f;
float                       g_secondaryHelicopterPitch = 0.0f;
float                       g_secondaryHelicopterRoll = 0.0f;
float                       g_secondaryHelicopterHoverTime = 1.7f;
float                       g_secondaryHelicopterFireCooldown = 0.0f;
float                       g_secondaryHelicopterFireCycleTime = 3.5f;
constexpr float             kHelicopterMaxHealth = 2000.0f;
constexpr float             kRocketHelicopterDamage = kHelicopterMaxHealth * 0.5f;
float                       g_secondaryHelicopterHealth = kHelicopterMaxHealth;
bool                        g_secondaryHelicopterDead = false;
bool                        g_secondaryHelicopterCrashed = false;
XMFLOAT3                    g_secondaryHelicopterCrashVelocity{ 0.0f, 0.0f, 0.0f };
XMFLOAT3                    g_secondaryHumveePosition{ 42.0f, 2.5f, 3.0f };
float                       g_helicopterHealth = kHelicopterMaxHealth;
bool                        g_helicopterDead = false;
bool                        g_helicopterCrashed = false;
XMFLOAT3                    g_helicopterCrashVelocity{ 0.0f, 0.0f, 0.0f };
XMFLOAT3                    g_humveeTurretLocal{ 0.0f, 0.35f, 0.0f };
bool                        g_drivingHumvee = false;
bool                        g_savedGunVisible = true;
XMFLOAT3                    g_previousHumveePosition{};
bool                        g_previousHumveePositionValid = false;
float                       g_humveeHouseImpactCooldown = 0.0f;
XMFLOAT3                    g_humveeAimPoint{};
float                       g_humveeTurretYaw = 0.0f;
float                       g_humveeTurretFireCooldown = 0.0f;
SkinnedModel                g_banditModel;
bool                        g_banditLoaded = false;
float                       g_banditLeftArmReach = 0.55f;
float                       g_banditHeadYawOffsetDegrees = 20.4f;
uint32_t                    g_banditSpawnSerial = 0;
GunAudio                    g_gunAudio;
GunAudio                    g_rpgFireAudio;
GunAudio                    g_explosionAudio;
GunAudio                    g_grenadeExplosionAudio;
GunAudio                    g_hitAudio;
GunAudio                    g_banditSpottedAudio1;
GunAudio                    g_banditSpottedAudio2;
GunAudio                    g_banditAttackAudio;
GunAudio                    g_banditDeathAudio;
GunAudio                    g_banditHitVoiceAudio;
GunAudio                    g_helicopterHoverAudio;
float                       g_banditVoiceCooldown = 0.0f;
float                       g_banditPainCooldown = 0.0f;
float                       g_fleshHitPitchMin = 0.9f;
float                       g_fleshHitPitchMax = 1.1f;
bool                        g_suppressFireUntilMouseRelease = false;
bool                        g_stressTestMode = false;
bool                        g_emptyLevelMode = false;
NavigationSystem            g_navigation;
static LevelEditor          g_levelEditor;
static Camera               g_editorCameraSnapshot;
static LevelDefinition      g_runtimeLevel = MakeLevelOneTemplate();
static std::vector<TerrainSculptStamp> g_runtimeTerrainSculpt;
static bool                 g_customLevelMode = false;
static bool                 g_pendingEnvironmentRebuild = false;
static std::string          g_activeCustomLevelName;
static std::string          g_mainMenuLevelStatus;
static XMFLOAT3             g_primaryHumveeSpawn{ 0.0f, 3.45f, 0.0f };
static float                g_primaryHumveeYaw = 0.0f;
static std::shared_ptr<SceneNode> g_houseTemplate;

static constexpr size_t kEnemiesPerSpawner = 2;
struct CompoundCenter { float x, z; };
static constexpr std::array<CompoundCenter, 8> kStressCompoundCenters = {{
    {  0.0f,   0.0f },
    { 42.0f,   0.0f },
    {-42.0f,   0.0f },
    {  0.0f,  42.0f },
    { 42.0f,  42.0f },
    {-42.0f,  42.0f },
    {  0.0f, -42.0f },
    { 42.0f, -42.0f },
}};
static constexpr size_t kSpawnersPerCompound = 4;
static constexpr size_t kSpawnerCount =
    kStressCompoundCenters.size() * kSpawnersPerCompound;
static constexpr size_t kStressHouseCount = 29;
static constexpr size_t kStressBanditCount = 58;
static_assert(kStressHouseCount <= kStressCompoundCenters.size() *
              kSpawnersPerCompound);
static_assert(kStressBanditCount <= kSpawnerCount * kEnemiesPerSpawner);

static size_t ActiveBanditSlotCount() {
    if (g_customLevelMode) {
        return static_cast<size_t>(std::count_if(g_runtimeLevel.entities.begin(),
            g_runtimeLevel.entities.end(), [](const LevelEntity& entity) {
                return entity.enabled && entity.type == LevelEntityType::EnemySpawn;
            }));
    }
    return g_stressTestMode ? kStressBanditCount : 4 * kEnemiesPerSpawner;
}

static TerrainRendererDX12::Params CurrentTerrainParams() {
    TerrainRendererDX12::Params params;
    if (g_stressTestMode) {
        params.tilesX = 32;
        params.tilesZ = 32;
        params.islandScale = 2.0f;
    }
    return params;
}

static void AddExplosionTerrainCrater(const XMFLOAT3& impact) {
    if (!scene.useMeshTerrain || !g_terrain.supported ||
        g_runtimeTerrainSculpt.size() >= 256) return;

    TerrainSculptStamp crater;
    crater.x = impact.x;
    crater.z = impact.z;
    crater.radius = scene.grenadeBlastRadius;
    crater.operation = TerrainSculptOperation::Add;
    crater.value = -1.0f;
    crater.strength = 1.0f;
    g_runtimeTerrainSculpt.push_back(crater);
    g_terrain.SetSculptStamps(g_runtimeTerrainSculpt);
    const float foliageRadius = crater.radius * 0.5f;
    g_grass.AddRuntimeExclusion(impact.x, impact.z, foliageRadius);
    g_trees.ApplyExplosion(impact, foliageRadius);
}

static size_t LiveBanditCount() {
    size_t count = 0;
    for (const auto& bandit : g_bandits)
        if (bandit && !bandit->Dead()) ++count;
    return count;
}

static void ReleaseMaterialUploadHeaps(const std::shared_ptr<SceneNode>& node) {
    if (!node) return;
    if (node->mesh) {
        for (MeshPrimitive& primitive : node->mesh->primitives)
            if (primitive.material) primitive.material->uploadHeaps.clear();
    }
    for (const auto& child : node->children)
        ReleaseMaterialUploadHeaps(child);
}

static size_t LiveRespawningBanditCount() {
    size_t count = 0;
    for (const auto& bandit : g_bandits)
        if (bandit && !bandit->Dead() && bandit->spawnSlot >= 0) ++count;
    return count;
}

XMMATRIX HumveeWorldMatrix() {
    const XMMATRIX model =
        XMMatrixTranslation(-g_humveeModelCenter.x, -g_humveeModelMinY,
                            -g_humveeModelCenter.z) *
        XMMatrixScaling(g_humveeModelScale, g_humveeModelScale,
                        g_humveeModelScale) *
        XMMatrixRotationY(-XM_PIDIV2);
    XMFLOAT4X4 physicsPose;
    if (g_destruction.GetVehicleTransform(physicsPose)) {
        // Model floor sits 0.95 m below chassis center.
        return model * XMMatrixTranslation(0.0f, -0.95f, 0.0f) *
               XMLoadFloat4x4(&physicsPose);
    }
    return model * XMMatrixRotationY(XMConvertToRadians(g_primaryHumveeYaw)) *
           XMMatrixTranslation(g_primaryHumveeSpawn.x,
                               g_primaryHumveeSpawn.y - 0.95f,
                               g_primaryHumveeSpawn.z);
}

XMMATRIX SecondaryHumveeWorldMatrix() {
    return XMMatrixTranslation(-g_humveeModelCenter.x, -g_humveeModelMinY,
                               -g_humveeModelCenter.z) *
           XMMatrixScaling(g_humveeModelScale, g_humveeModelScale,
                           g_humveeModelScale) *
           XMMatrixRotationY(-XM_PIDIV2) *
           XMMatrixTranslation(g_secondaryHumveePosition.x,
                               g_secondaryHumveePosition.y,
                               g_secondaryHumveePosition.z);
}

XMMATRIX HelicopterWorldMatrix() {
    return XMMatrixTranslation(-g_helicopterModelCenter.x,
                               -g_helicopterModelCenter.y,
                               -g_helicopterModelCenter.z) *
           XMMatrixScaling(g_helicopterModelScale, g_helicopterModelScale,
                           g_helicopterModelScale) *
           XMMatrixRotationRollPitchYaw(g_helicopterPitch,
                                        g_helicopterYaw + XM_PI,
                                        g_helicopterRoll) *
           XMMatrixTranslation(g_helicopterPosition.x,
                               g_helicopterPosition.y,
                               g_helicopterPosition.z);
}

XMMATRIX SecondaryHelicopterWorldMatrix() {
    return XMMatrixTranslation(-g_helicopterModelCenter.x,
                               -g_helicopterModelCenter.y,
                               -g_helicopterModelCenter.z) *
           XMMatrixScaling(g_helicopterModelScale * g_helicopterLevelScale,
                           g_helicopterModelScale * g_helicopterLevelScale,
                           g_helicopterModelScale * g_helicopterLevelScale) *
           XMMatrixRotationRollPitchYaw(g_secondaryHelicopterPitch,
                                        g_secondaryHelicopterYaw + XM_PI,
                                        g_secondaryHelicopterRoll) *
           XMMatrixTranslation(g_secondaryHelicopterPosition.x,
                               g_secondaryHelicopterPosition.y,
                               g_secondaryHelicopterPosition.z);
}

static void ConfigureHelicopterBounds() {
    if (!g_helicopterModel || !g_helicopterModel->mesh) return;
    XMFLOAT3 minimum(FLT_MAX, FLT_MAX, FLT_MAX);
    XMFLOAT3 maximum(-FLT_MAX, -FLT_MAX, -FLT_MAX);
    for (const MeshPrimitive& primitive : g_helicopterModel->mesh->primitives) {
        for (size_t vertex = 0; vertex + 11 < primitive.vertices.size(); vertex += 12) {
            minimum.x = (std::min)(minimum.x, primitive.vertices[vertex]);
            minimum.y = (std::min)(minimum.y, primitive.vertices[vertex + 1]);
            minimum.z = (std::min)(minimum.z, primitive.vertices[vertex + 2]);
            maximum.x = (std::max)(maximum.x, primitive.vertices[vertex]);
            maximum.y = (std::max)(maximum.y, primitive.vertices[vertex + 1]);
            maximum.z = (std::max)(maximum.z, primitive.vertices[vertex + 2]);
        }
    }
    const float horizontalLength = (std::max)(
        maximum.x - minimum.x, maximum.z - minimum.z);
    if (horizontalLength <= 0.001f) return;
    g_helicopterModelCenter = {
        (minimum.x + maximum.x) * 0.5f,
        (minimum.y + maximum.y) * 0.5f,
        (minimum.z + maximum.z) * 0.5f };
    g_helicopterModelScale = 10.0f / horizontalLength;
}

static bool ApplyDarkGreenToHumvee() {
    if (!g_humveeModel) return false;
    bool changed = false;
    std::function<void(const std::shared_ptr<SceneNode>&)> apply;
    apply = [&](const std::shared_ptr<SceneNode>& node) {
        if (!node) return;
        if (node->mesh) {
            for (MeshPrimitive& primitive : node->mesh->primitives) {
                if (!primitive.material) continue;
                if (primitive.material->name == "HumveeWheel") continue;
                primitive.material->baseColorFactor.x = 0.18f;
                primitive.material->baseColorFactor.y = 0.30f;
                primitive.material->baseColorFactor.z = 0.12f;
                primitive.material->srvHeapSlot = ~0u;
                changed = true;
            }
        }
        for (const auto& child : node->children) apply(child);
    };
    apply(g_humveeModel);
    return changed;
}

static void DamageHelicopter(float damage, const XMFLOAT3& hit) {
    if (damage <= 0.0f || g_helicopterDead || !g_helicopterModel) return;
    g_helicopterHealth = (std::max)(0.0f, g_helicopterHealth - damage);
    scene.SpawnSmokeBurst(hit, 0.22f, 0.10f);
    if (g_helicopterHealth > 0.0f) return;
    g_helicopterDead = true;
    g_helicopterFireCooldown = 9999.0f;
    g_helicopterCrashVelocity = {
        std::sin(g_helicopterYaw) * 2.2f,
        -0.8f,
        std::cos(g_helicopterYaw) * 2.2f };
    scene.SpawnExplosionFX(g_helicopterPosition, 7.0f, 1.0f);
    scene.SpawnSmokeBurst(g_helicopterPosition, 1.25f, 1.5f);
}

static void DamageSecondaryHelicopter(float damage, const XMFLOAT3& hit) {
    if (damage <= 0.0f || g_secondaryHelicopterDead || !g_helicopterModel ||
        !g_stressTestMode) return;
    g_secondaryHelicopterHealth = (std::max)(
        0.0f, g_secondaryHelicopterHealth - damage);
    scene.SpawnSmokeBurst(hit, 0.22f, 0.10f);
    if (g_secondaryHelicopterHealth > 0.0f) return;
    g_secondaryHelicopterDead = true;
    g_secondaryHelicopterFireCooldown = 9999.0f;
    g_secondaryHelicopterCrashVelocity = {
        std::sin(g_secondaryHelicopterYaw) * 2.2f,
        -0.8f,
        std::cos(g_secondaryHelicopterYaw) * 2.2f };
    scene.SpawnExplosionFX(g_secondaryHelicopterPosition, 7.0f, 1.0f);
    scene.SpawnSmokeBurst(g_secondaryHelicopterPosition, 1.25f, 1.5f);
}

static bool HitHelicopterAtSegment(const XMFLOAT3& position, bool dead,
                                   const XMFLOAT3& start, const XMFLOAT3& end,
                                   float radius, XMFLOAT3& hit) {
    if (g_emptyLevelMode || !scene.showHelicopter ||
        !g_helicopterModel || dead) return false;
    const XMVECTOR a = XMLoadFloat3(&start);
    const XMVECTOR b = XMLoadFloat3(&end);
    const XMVECTOR center = XMLoadFloat3(&position);
    const XMVECTOR ab = b - a;
    const float lengthSq = XMVectorGetX(XMVector3LengthSq(ab));
    float t = lengthSq > 1e-6f
        ? XMVectorGetX(XMVector3Dot(center - a, ab)) / lengthSq : 0.0f;
    t = (std::max)(0.0f, (std::min)(1.0f, t));
    const XMVECTOR closest = a + ab * t;
    const float hitRadius = 5.0f + radius;
    if (XMVectorGetX(XMVector3LengthSq(center - closest)) > hitRadius * hitRadius)
        return false;
    XMStoreFloat3(&hit, closest);
    return true;
}

static bool HitHelicopterSegment(const XMFLOAT3& start, const XMFLOAT3& end,
                                 float radius, XMFLOAT3& hit) {
    return HitHelicopterAtSegment(g_helicopterPosition, g_helicopterDead,
                                  start, end, radius, hit);
}

static bool HitSecondaryHelicopterSegment(const XMFLOAT3& start,
                                          const XMFLOAT3& end, float radius,
                                          XMFLOAT3& hit) {
    return g_stressTestMode && HitHelicopterAtSegment(
        g_secondaryHelicopterPosition, g_secondaryHelicopterDead,
        start, end, radius, hit);
}

static void PlayBanditDeathEvents();

static bool KillBanditsTouchingRotor(const std::shared_ptr<SceneNode>& rotor,
                                     FXMVECTOR localAxis, float rotorRadius,
                                     CXMMATRIX helicopterWorld, bool dead) {
    if (!rotor || dead || !g_banditLoaded) return false;
    const XMMATRIX rotorWorld = XMLoadFloat4x4(&rotor->globalTransform) *
                                helicopterWorld;
    const XMVECTOR center = XMVector3TransformCoord(XMVectorZero(), rotorWorld);
    const XMVECTOR axis = XMVector3Normalize(
        XMVector3TransformNormal(localAxis, rotorWorld));
    bool killed = false;
    for (auto& bandit : g_bandits) {
        if (!bandit || bandit->Dead()) continue;
        const XMVECTOR body = XMVectorSet(
            bandit->position.x,
            bandit->position.y + bandit->footOffset + 1.0f,
            bandit->position.z, 1.0f);
        const XMVECTOR delta = body - center;
        const float axial = XMVectorGetX(XMVector3Dot(delta, axis));
        const float distanceSq = XMVectorGetX(XMVector3LengthSq(delta));
        const float radialSq = (std::max)(0.0f, distanceSq - axial * axial);
        constexpr float bodyRadius = 0.72f;
        if (std::abs(axial) > bodyRadius + 0.18f ||
            radialSq > (rotorRadius + bodyRadius) * (rotorRadius + bodyRadius))
            continue;

        XMVECTOR radial = delta - axis * axial;
        if (XMVectorGetX(XMVector3LengthSq(radial)) < 1e-5f)
            radial = XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f);
        radial = XMVector3Normalize(radial);
        XMVECTOR impulse = XMVector3Normalize(
            XMVector3Cross(axis, radial) + XMVectorSet(0.0f, 0.22f, 0.0f, 0.0f));
        XMFLOAT3 direction, impact;
        XMStoreFloat3(&direction, impulse);
        XMStoreFloat3(&impact, body);
        if (!bandit->KillFromRotor(direction, impact)) continue;
        if (bandit.get() == g_heldBandit) g_heldBandit = nullptr;
        scene.SpawnBloodBurst(impact, direction);
        g_hitAudio.Play(0.72f * 0.3f,
            0.88f + ((float)std::rand() / RAND_MAX) * 0.16f);
        killed = true;
    }
    return killed;
}

static void UpdateHelicopterRotorKills() {
    if (!scene.showHelicopter) return;
    bool killed = KillBanditsTouchingRotor(
        g_helicopterMainRotorNode, XMVectorSet(0, 1, 0, 0), 4.75f,
        HelicopterWorldMatrix(), g_helicopterDead);
    killed |= KillBanditsTouchingRotor(
        g_helicopterTailRotorNode, XMVectorSet(1, 0, 0, 0), 1.10f,
        HelicopterWorldMatrix(), g_helicopterDead);
    if (g_stressTestMode) {
        killed |= KillBanditsTouchingRotor(
            g_helicopterMainRotorNode, XMVectorSet(0, 1, 0, 0), 4.75f,
            SecondaryHelicopterWorldMatrix(), g_secondaryHelicopterDead);
        killed |= KillBanditsTouchingRotor(
            g_helicopterTailRotorNode, XMVectorSet(1, 0, 0, 0), 1.10f,
            SecondaryHelicopterWorldMatrix(), g_secondaryHelicopterDead);
    }
    if (killed) PlayBanditDeathEvents();
}

static void UpdateHelicopter(float dt) {
    if (!g_helicopterModel || !scene.showHelicopter) return;
    if (!g_helicopterDead || (g_stressTestMode && !g_secondaryHelicopterDead)) {
        g_helicopterMainRotorAngle = std::fmod(
            g_helicopterMainRotorAngle + dt * 24.0f, XM_2PI);
        g_helicopterTailRotorAngle = std::fmod(
            g_helicopterTailRotorAngle + dt * 38.0f, XM_2PI);
        if (g_helicopterMainRotorNode)
            XMStoreFloat4(&g_helicopterMainRotorNode->rotation,
                XMQuaternionRotationAxis(XMVectorSet(0, 1, 0, 0),
                                         g_helicopterMainRotorAngle));
        if (g_helicopterTailRotorNode)
            XMStoreFloat4(&g_helicopterTailRotorNode->rotation,
                XMQuaternionRotationAxis(XMVectorSet(1, 0, 0, 0),
                                         g_helicopterTailRotorAngle));
    }
    if (g_helicopterDead) {
        if (g_helicopterCrashed) return;
        g_helicopterCrashVelocity.y -= 9.81f * dt;
        g_helicopterPosition.x += g_helicopterCrashVelocity.x * dt;
        g_helicopterPosition.y += g_helicopterCrashVelocity.y * dt;
        g_helicopterPosition.z += g_helicopterCrashVelocity.z * dt;
        g_helicopterPitch += 0.42f * dt;
        g_helicopterRoll += 0.78f * dt;
        g_helicopterYaw += 0.18f * dt;

        float groundY = 0.0f;
        if (scene.useMeshTerrain && g_terrain.supported) {
            auto params = CurrentTerrainParams();
            params.heightScale = scene.terrainHeightScale;
            groundY = TerrainRendererDX12::HeightAt(
                params, g_helicopterPosition.x, g_helicopterPosition.z);
        }
        if (g_helicopterPosition.y <= groundY + 1.65f) {
            g_helicopterPosition.y = groundY + 1.65f;
            g_helicopterCrashed = true;
            g_helicopterCrashVelocity = { 0.0f, 0.0f, 0.0f };
            scene.SpawnExplosionFX(
                { g_helicopterPosition.x, g_helicopterPosition.y + 1.6f,
                  g_helicopterPosition.z }, 9.0f, 1.1f);
            scene.SpawnSmokeBurst(g_helicopterPosition, 2.8f, 3.0f);
            if (g_destruction.IsInitialized())
                g_destruction.ApplyExplosion(g_helicopterPosition, 4.5f, 55.0f, 12.0f);
        }
        XMFLOAT4X4 identity;
        XMStoreFloat4x4(&identity, XMMatrixIdentity());
        g_helicopterModel->UpdateGlobalTransform(identity);
        return;
    }
    g_helicopterHoverTime += dt;

    // Slow bounded patrol around spawn, with small independent hover drift.
    // Horizontal distance never exceeds the configured patrol radius.
    const float patrolPhase = g_helicopterHoverTime * 0.105f;
    g_helicopterPosition.x =
        g_helicopterSpawn.x +
        std::sin(patrolPhase) * (kHelicopterPatrolRadius - 0.72f) +
        std::sin(g_helicopterHoverTime * 0.31f) * 0.72f;
    g_helicopterPosition.y = g_helicopterSpawn.y +
        std::sin(g_helicopterHoverTime * 1.27f) * 0.26f +
        std::sin(g_helicopterHoverTime * 0.43f) * 0.12f;
    g_helicopterPosition.z =
        g_helicopterSpawn.z +
        std::cos(patrolPhase) * (kHelicopterPatrolRadius * 0.68f) +
        std::cos(g_helicopterHoverTime * 0.27f) * 0.55f;

    const float targetX = scene.camera.Position.x - g_helicopterPosition.x;
    const float targetZ = scene.camera.Position.z - g_helicopterPosition.z;
    const float desiredYaw = std::atan2(targetX, targetZ);
    const float yawDelta = std::atan2(
        std::sin(desiredYaw - g_helicopterYaw),
        std::cos(desiredYaw - g_helicopterYaw));
    const float yawLerp = 1.0f - std::exp(-1.65f * (std::max)(0.0f, dt));
    g_helicopterYaw += yawDelta * yawLerp;
    const float desiredRoll = (std::max)(-0.10f, (std::min)(0.10f,
        -yawDelta * 0.075f + std::sin(g_helicopterHoverTime * 0.71f) * 0.025f));
    const float desiredPitch = std::sin(g_helicopterHoverTime * 0.47f) * 0.022f;
    const float attitudeLerp = 1.0f - std::exp(-2.4f * (std::max)(0.0f, dt));
    g_helicopterRoll += (desiredRoll - g_helicopterRoll) * attitudeLerp;
    g_helicopterPitch += (desiredPitch - g_helicopterPitch) * attitudeLerp;

    XMFLOAT4X4 identity;
    XMStoreFloat4x4(&identity, XMMatrixIdentity());
    g_helicopterModel->UpdateGlobalTransform(identity);

    g_helicopterFireCycleTime = std::fmod(
        g_helicopterFireCycleTime + dt, 7.0f);
    if (g_helicopterFireCycleTime >= 2.0f) {
        g_helicopterFireCooldown = 0.0f;
        return;
    }
    g_helicopterFireCooldown -= dt;
    if (scene.playerHealth <= 0.0f || g_helicopterFireCooldown > 0.0f) return;
    const XMFLOAT3 forward{
        std::sin(g_helicopterYaw), 0.0f, std::cos(g_helicopterYaw) };
    const XMFLOAT3 muzzle{
        g_helicopterPosition.x + forward.x * 3.75f,
        g_helicopterPosition.y - 0.65f,
        g_helicopterPosition.z + forward.z * 3.75f };
    XMVECTOR direction = XMLoadFloat3(&scene.camera.Position) - XMLoadFloat3(&muzzle);
    const float distanceSq = XMVectorGetX(XMVector3LengthSq(direction));
    if (distanceSq < 4.0f || distanceSq > 75.0f * 75.0f) {
        g_helicopterFireCooldown = 0.10f;
        return;
    }
    const float randomX = ((float)std::rand() / RAND_MAX - 0.5f) * 0.018f;
    const float randomY = ((float)std::rand() / RAND_MAX - 0.5f) * 0.012f;
    const float randomZ = ((float)std::rand() / RAND_MAX - 0.5f) * 0.018f;
    direction = XMVector3Normalize(direction) + XMVectorSet(randomX, randomY, randomZ, 0.0f);
    XMFLOAT3 shotDirection;
    XMStoreFloat3(&shotDirection, XMVector3Normalize(direction));
    scene.SpawnHostileProjectile(muzzle, shotDirection);
    scene.SpawnSmokeBurst(muzzle, 0.10f, 0.13f);
    const float distance = std::sqrt(distanceSq);
    const float volume = (std::max)(0.10f, 0.68f * (1.0f - distance / 90.0f));
    g_gunAudio.Play(volume, 0.82f + ((float)std::rand() / RAND_MAX) * 0.08f);
    g_helicopterFireCooldown = 0.10f;
}

static void UpdateSecondaryHelicopter(float dt) {
    if (!g_stressTestMode || !g_helicopterModel || !scene.showHelicopter) return;
    if (g_secondaryHelicopterDead) {
        if (g_secondaryHelicopterCrashed) return;
        g_secondaryHelicopterCrashVelocity.y -= 9.81f * dt;
        g_secondaryHelicopterPosition.x += g_secondaryHelicopterCrashVelocity.x * dt;
        g_secondaryHelicopterPosition.y += g_secondaryHelicopterCrashVelocity.y * dt;
        g_secondaryHelicopterPosition.z += g_secondaryHelicopterCrashVelocity.z * dt;
        g_secondaryHelicopterPitch += 0.42f * dt;
        g_secondaryHelicopterRoll += 0.78f * dt;
        g_secondaryHelicopterYaw += 0.18f * dt;

        float groundY = 0.0f;
        if (scene.useMeshTerrain && g_terrain.supported) {
            auto params = CurrentTerrainParams();
            params.heightScale = scene.terrainHeightScale;
            groundY = TerrainRendererDX12::HeightAt(
                params, g_secondaryHelicopterPosition.x,
                g_secondaryHelicopterPosition.z);
        }
        if (g_secondaryHelicopterPosition.y <= groundY + 1.65f) {
            g_secondaryHelicopterPosition.y = groundY + 1.65f;
            g_secondaryHelicopterCrashed = true;
            g_secondaryHelicopterCrashVelocity = { 0.0f, 0.0f, 0.0f };
            scene.SpawnExplosionFX(
                { g_secondaryHelicopterPosition.x,
                  g_secondaryHelicopterPosition.y + 1.6f,
                  g_secondaryHelicopterPosition.z }, 9.0f, 1.1f);
            scene.SpawnSmokeBurst(g_secondaryHelicopterPosition, 2.8f, 3.0f);
            if (g_destruction.IsInitialized())
                g_destruction.ApplyExplosion(
                    g_secondaryHelicopterPosition, 4.5f, 55.0f, 12.0f);
        }
        return;
    }

    g_secondaryHelicopterHoverTime += dt;
    const float patrolPhase = g_secondaryHelicopterHoverTime * 0.105f + XM_PI;
    g_secondaryHelicopterPosition.x = 42.0f +
        std::sin(patrolPhase) * (kHelicopterPatrolRadius - 0.72f) +
        std::sin(g_secondaryHelicopterHoverTime * 0.29f) * 0.72f;
    g_secondaryHelicopterPosition.y = 14.0f +
        std::sin(g_secondaryHelicopterHoverTime * 1.19f) * 0.26f +
        std::sin(g_secondaryHelicopterHoverTime * 0.39f) * 0.12f;
    g_secondaryHelicopterPosition.z =
        std::cos(patrolPhase) * (kHelicopterPatrolRadius * 0.68f) +
        std::cos(g_secondaryHelicopterHoverTime * 0.25f) * 0.55f;

    const float targetX = scene.camera.Position.x - g_secondaryHelicopterPosition.x;
    const float targetZ = scene.camera.Position.z - g_secondaryHelicopterPosition.z;
    const float desiredYaw = std::atan2(targetX, targetZ);
    const float yawDelta = std::atan2(
        std::sin(desiredYaw - g_secondaryHelicopterYaw),
        std::cos(desiredYaw - g_secondaryHelicopterYaw));
    const float yawLerp = 1.0f - std::exp(-1.65f * (std::max)(0.0f, dt));
    g_secondaryHelicopterYaw += yawDelta * yawLerp;
    const float desiredRoll = (std::max)(-0.10f, (std::min)(0.10f,
        -yawDelta * 0.075f +
        std::sin(g_secondaryHelicopterHoverTime * 0.67f) * 0.025f));
    const float desiredPitch =
        std::sin(g_secondaryHelicopterHoverTime * 0.43f) * 0.022f;
    const float attitudeLerp = 1.0f - std::exp(-2.4f * (std::max)(0.0f, dt));
    g_secondaryHelicopterRoll +=
        (desiredRoll - g_secondaryHelicopterRoll) * attitudeLerp;
    g_secondaryHelicopterPitch +=
        (desiredPitch - g_secondaryHelicopterPitch) * attitudeLerp;

    g_secondaryHelicopterFireCycleTime = std::fmod(
        g_secondaryHelicopterFireCycleTime + dt, 7.0f);
    if (g_secondaryHelicopterFireCycleTime >= 2.0f) {
        g_secondaryHelicopterFireCooldown = 0.0f;
        return;
    }
    g_secondaryHelicopterFireCooldown -= dt;
    if (scene.playerHealth <= 0.0f || g_secondaryHelicopterFireCooldown > 0.0f)
        return;
    const XMFLOAT3 forward{
        std::sin(g_secondaryHelicopterYaw), 0.0f,
        std::cos(g_secondaryHelicopterYaw) };
    const XMFLOAT3 muzzle{
        g_secondaryHelicopterPosition.x + forward.x * 3.75f,
        g_secondaryHelicopterPosition.y - 0.65f,
        g_secondaryHelicopterPosition.z + forward.z * 3.75f };
    XMVECTOR direction =
        XMLoadFloat3(&scene.camera.Position) - XMLoadFloat3(&muzzle);
    const float distanceSq = XMVectorGetX(XMVector3LengthSq(direction));
    if (distanceSq < 4.0f || distanceSq > 75.0f * 75.0f) {
        g_secondaryHelicopterFireCooldown = 0.10f;
        return;
    }
    direction = XMVector3Normalize(direction) + XMVectorSet(
        ((float)std::rand() / RAND_MAX - 0.5f) * 0.018f,
        ((float)std::rand() / RAND_MAX - 0.5f) * 0.012f,
        ((float)std::rand() / RAND_MAX - 0.5f) * 0.018f, 0.0f);
    XMFLOAT3 shotDirection;
    XMStoreFloat3(&shotDirection, XMVector3Normalize(direction));
    scene.SpawnHostileProjectile(muzzle, shotDirection);
    scene.SpawnSmokeBurst(muzzle, 0.10f, 0.13f);
    const float distance = std::sqrt(distanceSq);
    g_gunAudio.Play((std::max)(0.10f, 0.68f * (1.0f - distance / 90.0f)),
                    0.82f + ((float)std::rand() / RAND_MAX) * 0.08f);
    g_secondaryHelicopterFireCooldown = 0.10f;
}

static XMFLOAT3 HumveeTurretMountWorld() {
    XMFLOAT4X4 physicsPose;
    if (!g_destruction.GetVehicleTransform(physicsPose))
        return { g_humveeTurretLocal.x,
                 g_humveeTurretLocal.y + 3.45f,
                 g_humveeTurretLocal.z };
    XMFLOAT3 result;
    XMStoreFloat3(&result, XMVector3TransformCoord(
        XMLoadFloat3(&g_humveeTurretLocal), XMLoadFloat4x4(&physicsPose)));
    return result;
}

static void ConfigureHumveeBounds() {
    if (!g_humveeModel || !g_humveeModel->mesh) return;
    XMFLOAT3 minimum(FLT_MAX, FLT_MAX, FLT_MAX);
    XMFLOAT3 maximum(-FLT_MAX, -FLT_MAX, -FLT_MAX);
    for (const MeshPrimitive& primitive : g_humveeModel->mesh->primitives) {
        for (size_t vertex = 0; vertex + 11 < primitive.vertices.size(); vertex += 12) {
            const float x = primitive.vertices[vertex];
            const float y = primitive.vertices[vertex + 1];
            const float z = primitive.vertices[vertex + 2];
            minimum.x = (std::min)(minimum.x, x);
            minimum.y = (std::min)(minimum.y, y);
            minimum.z = (std::min)(minimum.z, z);
            maximum.x = (std::max)(maximum.x, x);
            maximum.y = (std::max)(maximum.y, y);
            maximum.z = (std::max)(maximum.z, z);
        }
    }
    const float horizontalLength = (std::max)(
        maximum.x - minimum.x, maximum.z - minimum.z);
    if (horizontalLength <= 0.001f) return;
    g_humveeModelCenter = {
        (minimum.x + maximum.x) * 0.5f,
        (minimum.y + maximum.y) * 0.5f,
        (minimum.z + maximum.z) * 0.5f };
    g_humveeModelMinY = minimum.y;
    g_humveeModelScale = 4.8f / horizontalLength;
    const float vehicleTop = 2.5f + (maximum.y - minimum.y) * g_humveeModelScale;
    // Sink legs and pelvis through hatch; only upper torso remains above turret.
    g_humveeTurretLocal = { 0.0f, vehicleTop - 3.0f - 3.45f, 0.0f };
}

static float BanditVoiceVolume(const XMFLOAT3& position, float peak = 0.78f) {
    const float dx = position.x - scene.camera.Position.x;
    const float dy = position.y - scene.camera.Position.y;
    const float dz = position.z - scene.camera.Position.z;
    const float distance = std::sqrt(dx*dx + dy*dy + dz*dz);
    return (std::max)(0.06f, peak * (1.0f - distance / 38.0f));
}

static void PlayBanditDeathEvents() {
    for (auto& bandit : g_bandits) {
        if (!bandit || !bandit->ConsumeDeathEvent()) continue;
        const float pitch = 0.94f + ((float)std::rand() / RAND_MAX) * 0.10f;
        g_banditDeathAudio.Play(BanditVoiceVolume(bandit->position, 0.9f), pitch);
    }
}

static bool SpawnBandit() {
    if (!g_banditModel.valid) return false;
    const size_t activeSlots = ActiveBanditSlotCount();
    size_t slot = activeSlots;
    for (size_t candidate = 0; candidate < activeSlots; ++candidate) {
        bool occupied = false;
        for (const auto& existing : g_bandits) {
            if (existing && !existing->Dead() &&
                existing->spawnSlot == static_cast<int>(candidate)) {
                occupied = true;
                break;
            }
        }
        if (!occupied) { slot = candidate; break; }
    }
    if (slot == activeSlots) return false;

    auto bandit = std::make_unique<SkinnedEnemy>();
    if (!bandit->Init(g_banditModel)) return false;

    if (g_customLevelMode) {
        size_t spawnIndex = 0;
        for (const LevelEntity& entity : g_runtimeLevel.entities) {
            if (!entity.enabled || entity.type != LevelEntityType::EnemySpawn) continue;
            if (spawnIndex++ != slot) continue;
            bandit->position = { entity.transform.position[0],
                                 entity.transform.position[1],
                                 entity.transform.position[2] };
            bandit->yaw = XMConvertToRadians(entity.transform.rotation[1]);
            bandit->modelScale *= entity.transform.scale[0];
            bandit->spawnSlot = static_cast<int>(slot);
            bandit->leftArmReach = g_banditLeftArmReach;
            bandit->orbitRadius = 4.4f + static_cast<float>(slot % 4) * 0.45f;
            bandit->orbitDirection = (slot & 1) ? -1.0f : 1.0f;
            bandit->fireCooldown = 0.7f +
                ((float)std::rand() / (float)RAND_MAX) * 2.8f;
            bandit->PlayClip("Walk");
            g_bandits.push_back(std::move(bandit));
            return true;
        }
        return false;
    }

    const size_t spawner = slot / kEnemiesPerSpawner;
    const size_t member = slot % kEnemiesPerSpawner;
    const CompoundCenter& compound =
        kStressCompoundCenters[spawner / kSpawnersPerCompound];
    static constexpr float outwardX[kSpawnersPerCompound] = {
         0.0f, 1.0f, 0.0f, -1.0f };
    static constexpr float outwardZ[kSpawnersPerCompound] = {
         1.0f, 0.0f, -1.0f, 0.0f };
    const size_t sideIndex = spawner % kSpawnersPerCompound;
    const XMFLOAT3 spawn{
        compound.x + outwardX[sideIndex] * 17.5f,
        0.0f,
        compound.z + outwardZ[sideIndex] * 17.5f };
    const float side = member == 0 ? -0.85f : 0.85f;
    bandit->position = {
        spawn.x - outwardZ[sideIndex] * side,
        spawn.y,
        spawn.z + outwardX[sideIndex] * side
    };
    bandit->spawnSlot = static_cast<int>(slot);
    bandit->leftArmReach = g_banditLeftArmReach;
    bandit->orbitRadius = 4.4f + static_cast<float>(slot % 4) * 0.45f;
    bandit->orbitDirection = (slot & 1) ? -1.0f : 1.0f;
    bandit->fireCooldown =
        0.7f + ((float)std::rand() / (float)RAND_MAX) * 2.8f;
    bandit->PlayClip("Walk");
    bandit->anim.Advance(0.19f * static_cast<float>(g_banditSpawnSerial++ % 8));
    g_bandits.push_back(std::move(bandit));
    return true;
}

static bool SpawnHumveeTurretGunner(int vehicleIndex) {
    if (!g_banditModel.valid || !g_humveeModel) return false;
    for (const auto& existing : g_bandits)
        if (existing && !existing->Dead() && existing->turretGunner &&
            existing->mountedVehicleIndex == vehicleIndex)
            return true;
    auto bandit = std::make_unique<SkinnedEnemy>();
    if (!bandit->Init(g_banditModel)) return false;
    bandit->position = vehicleIndex == 0
        ? HumveeTurretMountWorld()
        : XMFLOAT3{ g_secondaryHumveePosition.x + g_humveeTurretLocal.x,
                    g_humveeTurretLocal.y + 3.45f,
                    g_secondaryHumveePosition.z + g_humveeTurretLocal.z };
    bandit->turretGunner = true;
    bandit->mountedVehicleIndex = vehicleIndex;
    bandit->spawnSlot = -1;
    bandit->leftArmReach = g_banditLeftArmReach;
    bandit->fireCooldown = 1.4f;
    bandit->PlayClip("Idle");
    g_bandits.push_back(std::move(bandit));
    return true;
}

static void ShootPlayerWeapon() {
    if (GunModel::SVDSelected()) {
        scene.ShootSniperProjectile();
        const float pitch = 0.70f + ((float)std::rand() / RAND_MAX) * 0.04f;
        g_gunAudio.Play(1.0f, pitch);
    } else if (GunModel::RPGSelected()) {
        scene.ShootRocket();
        const float pitch = 0.68f + ((float)std::rand() / RAND_MAX) * 0.05f;
        g_rpgFireAudio.Play(1.0f, pitch);
    } else if (GunModel::ShotgunSelected()) {
        scene.ShootShotgun();
        const float pitch = 0.78f + ((float)std::rand() / RAND_MAX) * 0.08f;
        g_gunAudio.Play(0.96f, pitch);
    } else {
        scene.ShootProjectile();
        const float pitch = 0.96f + ((float)std::rand() / RAND_MAX) * 0.08f;
        g_gunAudio.Play(0.82f, pitch);
    }
}

static float PlayerFireInterval() {
    if (GunModel::SVDSelected()) return 1.05f;
    if (GunModel::RPGSelected()) return 1.7f;
    return GunModel::ShotgunSelected() ? 0.8f : scene.fireInterval;
}

static bool HitTerrainSegment(const XMFLOAT3& start, const XMFLOAT3& end,
                              float radius, XMFLOAT3& hit) {
    if (!scene.useMeshTerrain || !g_terrain.supported) return false;
    auto params = CurrentTerrainParams();
    params.heightScale = scene.terrainHeightScale;
    const float dx = end.x - start.x;
    const float dy = end.y - start.y;
    const float dz = end.z - start.z;
    const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
    const int steps = (std::max)(4, (std::min)(32,
        static_cast<int>(std::ceil(length / 0.15f))));
    float previousT = 0.0f;
    for (int step = 1; step <= steps; ++step) {
        const float t = static_cast<float>(step) / static_cast<float>(steps);
        const float x = start.x + dx * t;
        const float y = start.y + dy * t;
        const float z = start.z + dz * t;
        if (y > TerrainRendererDX12::HeightAt(params, x, z) + radius) {
            previousT = t;
            continue;
        }
        float lo = previousT, hi = t;
        for (int refine = 0; refine < 6; ++refine) {
            const float mid = (lo + hi) * 0.5f;
            const float mx = start.x + dx * mid;
            const float my = start.y + dy * mid;
            const float mz = start.z + dz * mid;
            if (my > TerrainRendererDX12::HeightAt(params, mx, mz) + radius)
                lo = mid;
            else
                hi = mid;
        }
        hit = { start.x + dx * hi, start.y + dy * hi, start.z + dz * hi };
        return true;
    }
    return false;
}

static bool BanditHasLineOfSight(const SkinnedEnemy& shooter,
                                 const XMFLOAT3& target) {
    const XMFLOAT3 origin = shooter.AimRayOrigin();
    constexpr float rayRadius = 0.04f;
    XMFLOAT3 hit;
    if (scene.useDestruction && g_destruction.IsInitialized() &&
        g_destruction.HitTestSegment(origin, target, rayRadius, hit))
        return false;
    if (HitTerrainSegment(origin, target, rayRadius, hit)) return false;
    if (g_trees.BlocksSegment(origin, target, rayRadius)) return false;
    for (const auto& bandit : g_bandits) {
        if (!bandit || bandit.get() == &shooter || bandit->Dead()) continue;
        // Human shield must not stop enemies from taking the shot. The hostile
        // projectile collision path damages the held Bandit before the player.
        if (bandit->Held()) continue;
        if (bandit->BlocksProjectile(origin, target, rayRadius)) return false;
    }
    return true;
}

static bool GrabPathClear(const XMFLOAT3& target) {
    constexpr float rayRadius = 0.05f;
    XMFLOAT3 hit;
    if (scene.useDestruction && g_destruction.IsInitialized() &&
        g_destruction.HitTestSegment(scene.camera.Position, target, rayRadius, hit))
        return false;
    if (HitTerrainSegment(scene.camera.Position, target, rayRadius, hit)) return false;
    if (g_trees.BlocksSegment(scene.camera.Position, target, rayRadius)) return false;
    return true;
}

static void GrabOrThrowBandit() {
    using namespace DirectX;
    if (g_heldBandit && g_heldBandit->Held()) {
        XMVECTOR throwDirection = XMVector3Normalize(
            XMLoadFloat3(&scene.camera.Front) + XMVectorSet(0.0f, 0.16f, 0.0f, 0.0f));
        XMFLOAT3 direction;
        XMStoreFloat3(&direction, throwDirection);
        g_heldBandit->Throw(direction, 16.0f);
        g_heldBandit = nullptr;
        PlayBanditDeathEvents();
        return;
    }
    g_heldBandit = nullptr;

    const XMVECTOR eye = XMLoadFloat3(&scene.camera.Position);
    const XMVECTOR front = XMVector3Normalize(XMLoadFloat3(&scene.camera.Front));
    SkinnedEnemy* best = nullptr;
    float bestScore = FLT_MAX;
    for (const auto& bandit : g_bandits) {
        if (!bandit || bandit->Dead()) continue;
        const XMFLOAT3 chest = {
            bandit->position.x,
            bandit->position.y + bandit->footOffset + 1.1f,
            bandit->position.z };
        const XMVECTOR toTarget = XMLoadFloat3(&chest) - eye;
        const float forward = XMVectorGetX(XMVector3Dot(toTarget, front));
        if (forward < 0.35f || forward > 4.5f) continue;
        const float distanceSq = XMVectorGetX(XMVector3LengthSq(toTarget));
        const float sideSq = (std::max)(0.0f, distanceSq - forward * forward);
        if (sideSq > 0.85f * 0.85f || !GrabPathClear(chest)) continue;
        const float score = sideSq * 4.0f + forward * 0.05f;
        if (score < bestScore) {
            bestScore = score;
            best = bandit.get();
        }
    }
    if (best) {
        best->SetHeld(true);
        g_heldBandit = best;
    }
}

static ExplosiveBarrel* HeldBarrel() {
    if (g_heldBarrelIndex >= scene.explosiveBarrels.size()) return nullptr;
    ExplosiveBarrel& barrel = scene.explosiveBarrels[g_heldBarrelIndex];
    return barrel.active && barrel.held ? &barrel : nullptr;
}

static void ThrowHeldBarrel() {
    ExplosiveBarrel* barrel = HeldBarrel();
    if (!barrel) {
        g_heldBarrelIndex = SIZE_MAX;
        return;
    }
    XMVECTOR direction = XMVector3Normalize(
        XMLoadFloat3(&scene.camera.Front) + XMVectorSet(0.0f, 0.14f, 0.0f, 0.0f));
    XMVECTOR velocity = direction * 17.5f + XMVectorSet(0.0f, 1.8f, 0.0f, 0.0f);
    XMStoreFloat3(&barrel->velocity, velocity);
    barrel->held = false;
    barrel->thrown = true;
    g_heldBarrelIndex = SIZE_MAX;
}

static void GrabOrThrowObject() {
    if (HeldBarrel()) {
        ThrowHeldBarrel();
        return;
    }
    if (g_heldBandit && g_heldBandit->Held()) {
        GrabOrThrowBandit();
        return;
    }

    const XMVECTOR eye = XMLoadFloat3(&scene.camera.Position);
    const XMVECTOR front = XMVector3Normalize(XMLoadFloat3(&scene.camera.Front));
    size_t best = SIZE_MAX;
    float bestScore = FLT_MAX;
    for (size_t i = 0; i < scene.explosiveBarrels.size(); ++i) {
        const ExplosiveBarrel& barrel = scene.explosiveBarrels[i];
        if (!barrel.active || barrel.held) continue;
        const XMVECTOR toTarget = XMLoadFloat3(&barrel.position) - eye;
        const float forward = XMVectorGetX(XMVector3Dot(toTarget, front));
        if (forward < 0.25f || forward > 3.5f) continue;
        const float distanceSq = XMVectorGetX(XMVector3LengthSq(toTarget));
        const float sideSq = (std::max)(0.0f, distanceSq - forward * forward);
        if (sideSq > 0.75f * 0.75f || !GrabPathClear(barrel.position)) continue;
        const float score = sideSq * 5.0f + forward * 0.05f;
        if (score < bestScore) { bestScore = score; best = i; }
    }
    if (best != SIZE_MAX) {
        g_heldBandit = nullptr;
        ExplosiveBarrel& barrel = scene.explosiveBarrels[best];
        barrel.held = true;
        barrel.thrown = false;
        barrel.velocity = { 0.0f, 0.0f, 0.0f };
        g_heldBarrelIndex = best;
        return;
    }
    GrabOrThrowBandit();
}

static void SpawnBarrelExplosionFX(const XMFLOAT3& center) {
    scene.SpawnExplosionFX(
        { center.x, center.y + 1.1f, center.z }, 5.5f);
}

static void DetonateBarrel(size_t firstBarrel) {
    if (firstBarrel >= scene.explosiveBarrels.size() ||
        !scene.explosiveBarrels[firstBarrel].active) return;

    if (g_heldBarrelIndex == firstBarrel) g_heldBarrelIndex = SIZE_MAX;
    std::vector<size_t> pending{ firstBarrel };
    scene.explosiveBarrels[firstBarrel].active = false;
    scene.explosiveBarrels[firstBarrel].held = false;
    scene.explosiveBarrels[firstBarrel].thrown = false;
    for (size_t cursor = 0; cursor < pending.size(); ++cursor) {
        const XMFLOAT3 center = scene.explosiveBarrels[pending[cursor]].position;
        AddExplosionTerrainCrater(center);
        SpawnBarrelExplosionFX(center);

        for (auto& bandit : g_bandits) {
            if (bandit) bandit->ApplyExplosion(center, 6.5f, 500.0f, 12.0f);
        }
        PlayBanditDeathEvents();
        if (!g_helicopterDead && g_helicopterModel) {
            const float hx = g_helicopterPosition.x - center.x;
            const float hy = g_helicopterPosition.y - center.y;
            const float hz = g_helicopterPosition.z - center.z;
            const float distance = std::sqrt(hx*hx + hy*hy + hz*hz);
            // Hull hit-sphere is ~5 m, so a barrel bursting on the hull
            // registers as a near-full-strength hit.
            const float reach = 11.5f;
            if (distance < reach)
                DamageHelicopter(120.0f * (1.0f - distance / reach),
                                 g_helicopterPosition);
        }
        if (g_stressTestMode && !g_secondaryHelicopterDead && g_helicopterModel) {
            const float hx = g_secondaryHelicopterPosition.x - center.x;
            const float hy = g_secondaryHelicopterPosition.y - center.y;
            const float hz = g_secondaryHelicopterPosition.z - center.z;
            const float distance = std::sqrt(hx*hx + hy*hy + hz*hz);
            const float reach = 11.5f;
            if (distance < reach)
                DamageSecondaryHelicopter(
                    120.0f * (1.0f - distance / reach),
                    g_secondaryHelicopterPosition);
        }
        if (scene.useDestruction && g_destruction.IsInitialized()) {
            g_destruction.ApplyExplosion(center, 5.0f, 3.0f, 180.0f);
            g_destruction.ApplyRagdollExplosion(center, 6.5f, 110.0f);
        }

        const float px = scene.camera.Position.x - center.x;
        const float py = scene.camera.Position.y - center.y;
        const float pz = scene.camera.Position.z - center.z;
        const float playerDistance = std::sqrt(px*px + py*py + pz*pz);
        if (playerDistance < 6.0f)
            scene.DamagePlayer(85.0f * (1.0f - playerDistance / 6.0f));

        // Nearby barrels chain-react. Mark on enqueue to prevent duplicates.
        for (size_t i = 0; i < scene.explosiveBarrels.size(); ++i) {
            ExplosiveBarrel& other = scene.explosiveBarrels[i];
            if (!other.active) continue;
            const float dx = other.position.x - center.x;
            const float dy = other.position.y - center.y;
            const float dz = other.position.z - center.z;
            if (dx*dx + dy*dy + dz*dz > 4.5f * 4.5f) continue;
            other.active = false;
            other.held = false;
            other.thrown = false;
            if (g_heldBarrelIndex == i) g_heldBarrelIndex = SIZE_MAX;
            pending.push_back(i);
        }
    }
}

static bool HitExplosiveBarrelSegment(const XMFLOAT3& start,
                                      const XMFLOAT3& end, float radius,
                                      size_t& barrelIndex, XMFLOAT3& hit) {
    if (g_emptyLevelMode) return false;
    const float direction[3] = {
        end.x - start.x, end.y - start.y, end.z - start.z };
    const float origin[3] = { start.x, start.y, start.z };
    float closestT = FLT_MAX;
    bool found = false;
    for (size_t i = 0; i < scene.explosiveBarrels.size(); ++i) {
        const ExplosiveBarrel& barrel = scene.explosiveBarrels[i];
        if (!barrel.active) continue;
        const float center[3] = {
            barrel.position.x, barrel.position.y, barrel.position.z };
        const float extent[3] = {
            0.44f + radius, 0.78f + radius, 0.44f + radius };
        float tMin = 0.0f, tMax = 1.0f;
        bool intersects = true;
        for (int axis = 0; axis < 3; ++axis) {
            const float lo = center[axis] - extent[axis];
            const float hi = center[axis] + extent[axis];
            if (std::abs(direction[axis]) < 1e-6f) {
                if (origin[axis] < lo || origin[axis] > hi) intersects = false;
                continue;
            }
            float a = (lo - origin[axis]) / direction[axis];
            float b = (hi - origin[axis]) / direction[axis];
            if (a > b) std::swap(a, b);
            tMin = (std::max)(tMin, a);
            tMax = (std::min)(tMax, b);
            if (tMin > tMax) { intersects = false; break; }
        }
        if (!intersects || tMin >= closestT) continue;
        closestT = tMin;
        barrelIndex = i;
        found = true;
    }
    if (found) {
        hit = { start.x + direction[0] * closestT,
                start.y + direction[1] * closestT,
                start.z + direction[2] * closestT };
    }
    return found;
}

static void UpdateExplosiveBarrels(float dt) {
    for (size_t i = 0; i < scene.explosiveBarrels.size(); ++i) {
        ExplosiveBarrel& barrel = scene.explosiveBarrels[i];
        if (!barrel.active) continue;
        if (barrel.held) {
            const XMFLOAT3& eye = scene.camera.Position;
            const XMFLOAT3& front = scene.camera.Front;
            barrel.position = {
                eye.x + front.x * 1.85f,
                eye.y + front.y * 1.85f - 0.42f,
                eye.z + front.z * 1.85f };
            barrel.velocity = { 0.0f, 0.0f, 0.0f };
        } else if (barrel.thrown) {
            const XMFLOAT3 previous = barrel.position;
            barrel.velocity.y -= 9.81f * dt;
            barrel.position.x += barrel.velocity.x * dt;
            barrel.position.y += barrel.velocity.y * dt;
            barrel.position.z += barrel.velocity.z * dt;

            bool impact = false;
            XMFLOAT3 impactPoint = barrel.position;
            if (scene.useMeshTerrain && g_terrain.supported) {
                auto params = CurrentTerrainParams();
                params.heightScale = scene.terrainHeightScale;
                const float ground = TerrainRendererDX12::HeightAt(
                    params, barrel.position.x, barrel.position.z);
                if (barrel.position.y <= ground + 0.75f) {
                    barrel.position.y = ground + 0.75f;
                    impact = true;
                }
            } else if (barrel.position.y <= 0.75f) {
                barrel.position.y = 0.75f;
                impact = true;
            }
            if (!impact && g_destruction.IsInitialized())
                impact = g_destruction.HitTestSegment(
                    previous, barrel.position, 0.44f, impactPoint);
            // Thrown barrels detonate on the helicopter's hull.
            if (!impact &&
                HitHelicopterSegment(previous, barrel.position, 0.44f, impactPoint))
                impact = true;
            if (!impact && HitSecondaryHelicopterSegment(
                    previous, barrel.position, 0.44f, impactPoint))
                impact = true;
            if (!impact && g_banditLoaded) {
                for (const auto& bandit : g_bandits) {
                    if (bandit && !bandit->Dead() && bandit->BlocksProjectile(
                            previous, barrel.position, 0.44f, &impactPoint)) {
                        impact = true;
                        break;
                    }
                }
            }
            if (impact) {
                barrel.position = impactPoint;
                DetonateBarrel(i);
                continue;
            }
        }
        if (!barrel.burning) continue;
        barrel.fuse -= dt;
        barrel.fireFxCooldown -= dt;
        if (barrel.fuse <= 0.0f) {
            DetonateBarrel(i);
            continue;
        }
        if (barrel.fireFxCooldown > 0.0f) continue;
        barrel.fireFxCooldown = 0.11f;

        const XMFLOAT3 flameBase = {
            barrel.position.x, barrel.position.y + 0.78f, barrel.position.z };
        auto randomSigned = []() {
            return ((float)std::rand() / (float)RAND_MAX) * 2.0f - 1.0f;
        };
        for (int sparkIndex = 0; sparkIndex < 2; ++sparkIndex) {
            ImpactParticle flame;
            flame.position = {
                flameBase.x + randomSigned() * 0.12f,
                flameBase.y,
                flameBase.z + randomSigned() * 0.12f };
            flame.velocity = {
                randomSigned() * 0.35f,
                3.0f + std::abs(randomSigned()) * 1.8f,
                randomSigned() * 0.35f };
            flame.maxLife = flame.life = 0.18f + std::abs(randomSigned()) * 0.14f;
            flame.size = 0.045f + std::abs(randomSigned()) * 0.055f;
            flame.growth = -0.08f;
            flame.color = { 1.0f, 0.20f + std::abs(randomSigned()) * 0.28f, 0.01f };
            flame.spark = true;
            scene.impactParticles.push_back(flame);
        }
        if ((std::rand() % 4) == 0)
            scene.SpawnSmokeBurst(flameBase, 0.12f, 0.12f);
    }
}

// One-line Bandit status for the debug HUD (declared in EngineUI.h).
void BanditDebugText() {
    if (!g_banditLoaded) { ImGui::Text("Bandit: NOT LOADED"); return; }
    int textured = 0, parts = 0;
    if (g_banditModel.node && g_banditModel.node->mesh) {
        for (const auto& p : g_banditModel.node->mesh->primitives) {
            ++parts;
            if (p.material && p.material->baseColorTexture) ++textured;
        }
    }
    ImGui::Text("Bandits: live=%zu total=%zu bones=%zu parts=%d tex=%d",
                LiveBanditCount(), g_bandits.size(),
                g_banditModel.skeleton.BoneCount(), parts, textured);
    ImGui::Text("Weapon: %s  (mouse wheel)", GunModel::SelectedWeaponName());
    ImGui::SliderFloat("Left arm reach", &g_banditLeftArmReach,
                       0.20f, 0.85f, "%.2f m");
    ImGui::SliderFloat("Head/torso yaw offset",
                       &g_banditHeadYawOffsetDegrees,
                       -90.0f, 90.0f, "%.1f deg");
    if (ImGui::CollapsingHeader("Enemy Audio", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::SliderFloat("Flesh hit pitch min", &g_fleshHitPitchMin,
                               0.5f, 2.0f, "%.2f"))
            g_fleshHitPitchMin = (std::min)(g_fleshHitPitchMin, g_fleshHitPitchMax);
        if (ImGui::SliderFloat("Flesh hit pitch max", &g_fleshHitPitchMax,
                               0.5f, 2.0f, "%.2f"))
            g_fleshHitPitchMax = (std::max)(g_fleshHitPitchMax, g_fleshHitPitchMin);
    }
}
WaterVolume                 g_water;
WaterVolume                 g_ocean;   // sea ringing the island, surface at y = 0
PalmTrees                   g_trees;
GrassField                  g_grass;
static SkyRendererDX12      skyRenderer;
static EnvironmentIBLDX12   environmentIBL;
DDGIRendererDX12            g_ddgiRenderer;
ID3D12Resource*             g_skyEnvironmentResource = nullptr;
ID3D12Resource*             g_specularEnvironmentResource = nullptr;
ID3D12Resource*             g_brdfIntegrationResource = nullptr;
ID3D12Resource*             g_ddgiIrradianceResource = nullptr;
ID3D12Resource*             g_ddgiVisibilityResource = nullptr;
static OcclusionDepthDX12   occlusionDepth;
static XMMATRIX             previousHZBViewProjection = XMMatrixIdentity();
static bool                 hzbCaptureActive = false;
static FXAADX12             fxaa;
static VolumetricFogDX12    volumetricFog;
static ScreenSpaceAODX12    screenSpaceAO;
static MSAADX12             msaa;
static GrassMSAADX12        grassMSAA;
static bool                 msaaUsedLastFrame = false;
static VisibilityBufferDX12 visBuffer;
static ShadowMapDX12        shadowMap;
static GeometryBuffers      geo;
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
static std::vector<ComPtr<ID3D12Resource>> g_smokeUploadHeaps;
static std::vector<ComPtr<ID3D12Resource>> g_bloodUploadHeaps;
static std::vector<ComPtr<ID3D12Resource>> g_muzzleFlashUploadHeaps;
static std::vector<ComPtr<ID3D12Resource>> g_fireUploadHeaps;
static std::vector<ComPtr<ID3D12Resource>> g_explosionUploadHeaps;
static bool                 fullLevelAssetsLoaded = false;
static bool                 emptyLevelAssetsLoaded = false;
enum class LevelLoadStage {
    WorldAssets, Destruction, Environment, Weapons, Humvee, Helicopter,
    BanditModel, BanditSpawn, GPUFinalize, ReleaseUploads, Complete
};
struct LevelLoadRecord {
    std::string label;
    double milliseconds = 0.0;
    bool succeeded = true;
};
static uint32_t             levelLoadTaskCount = 10;
static LevelLoadStage       levelLoadStage = LevelLoadStage::Complete;
static bool                 levelLoadingActive = false;
static float                levelLoadingProgress = 0.0f;
static std::string          levelLoadingLabel = "Preparing level...";
static std::string          levelLoadingAsset = "None";
static uint32_t             levelLoadingTaskIndex = 0;
static uint32_t             levelLoadingLastSubmittedUploads = 0;
static uint32_t             levelLoadingSubmittedUploads = 0;
static StaticBufferStatsDX12 levelLoadingUploadBaseline = {};
static std::chrono::steady_clock::time_point levelLoadingStartedAt;
static std::chrono::steady_clock::time_point levelLoadingTaskStartedAt;
static std::vector<LevelLoadRecord> levelLoadingRecords;
static std::future<bool> levelDestructionLoadFuture;
static bool levelDestructionLoadInFlight = false;

static double LevelLoadElapsedMs(
    std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
}

static void BeginLevelLoading() {
    const auto now = std::chrono::steady_clock::now();
    levelLoadStage = LevelLoadStage::WorldAssets;
    levelLoadingActive = true;
    levelLoadingProgress = 0.02f;
    levelLoadTaskCount = g_emptyLevelMode ? 5u : 10u;
    levelLoadingLabel = g_emptyLevelMode
        ? "Terrain material"
        : "Terrain material and crate model";
    levelLoadingAsset = g_emptyLevelMode ? "floor material" : "models/h2.glb";
    levelLoadingTaskIndex = 1;
    levelLoadingLastSubmittedUploads = 0;
    levelLoadingSubmittedUploads = 0;
    levelLoadingUploadBaseline = GetStaticBufferStatsDX12();
    levelLoadingStartedAt = now;
    levelLoadingTaskStartedAt = now;
    levelLoadingRecords.clear();
}

static void AdvanceLevelLoading(LevelLoadStage next, const char* label,
                                const char* asset, bool succeeded = true) {
    levelLoadingRecords.push_back({ levelLoadingLabel,
        LevelLoadElapsedMs(levelLoadingTaskStartedAt), succeeded });
    if (levelLoadingRecords.size() > 6)
        levelLoadingRecords.erase(levelLoadingRecords.begin());
    levelLoadStage = next;
    ++levelLoadingTaskIndex;
    levelLoadingProgress = (std::min)(0.95f,
        static_cast<float>(levelLoadingTaskIndex - 1) / levelLoadTaskCount);
    levelLoadingLabel = label;
    levelLoadingAsset = asset;
    levelLoadingTaskStartedAt = std::chrono::steady_clock::now();
}

static void CompleteLevelLoading(bool succeeded = true) {
    levelLoadingRecords.push_back({ levelLoadingLabel,
        LevelLoadElapsedMs(levelLoadingTaskStartedAt), succeeded });
    levelLoadStage = LevelLoadStage::Complete;
    levelLoadingTaskIndex = levelLoadTaskCount;
    levelLoadingProgress = 1.0f;
    levelLoadingLabel = "Ready";
    levelLoadingAsset = "None";
    levelLoadingActive = false;
}

static float lastX = SCR_WIDTH / 2.0f;
static float lastY = SCR_HEIGHT / 2.0f;
static bool  firstMouse   = true;
static bool  ignoreNextMouseMove = false;
static bool  showUI        = true;
static bool  cameraLocked  = true;
enum class GameScreen { MainMenu, Level1, LevelEditor, WinScreen };
static GameScreen gameScreen = GameScreen::MainMenu;
static bool IsEditorEditing() {
    return gameScreen == GameScreen::LevelEditor && !g_levelEditor.IsPlaying();
}
static bool IsEditorPlaying() {
    return gameScreen == GameScreen::LevelEditor && g_levelEditor.IsPlaying();
}
static bool IsSceneScreen() {
    return gameScreen == GameScreen::Level1 || gameScreen == GameScreen::LevelEditor;
}
static bool IsGameplayScreen() {
    return gameScreen == GameScreen::Level1 || IsEditorPlaying();
}
static float levelElapsedSeconds = 0.0f;
static bool  levelTimerRunning = false;
static bool  deathCursorReleased = false;
static bool  pendingLevelRuntimeReset = false;
static bool  pendingTurretGunnerRespawn = false;
static bool  pendingEditorBeginPlay = false;
static bool  pendingEditorStopPlay = false;
static bool  pendingEditorReturnToMenu = false;
static bool  isFullscreen  = false;
static RECT  windowedRect  = {};
static DWORD windowedStyle = 0;
static float deltaTime     = 0.0f;

static ComPtr<ID3D12DescriptorHeap> imguiSrvHeap;

struct PalmSpawn { float x, z, height, lean; };
static constexpr std::array<PalmSpawn, 8> kPalmSpawns = {{
    { -15.5f, -13.8f, 7.5f,  0.5f },
    {  -3.5f, -18.0f, 6.4f, -0.4f },
    {  14.8f, -14.2f, 8.2f,  0.7f },
    {  18.0f,   2.8f, 6.8f, -0.6f },
    {  14.2f,  14.8f, 7.1f,  0.3f },
    {   3.2f,  18.2f, 6.0f, -0.5f },
    { -14.5f,  14.0f, 7.8f,  0.6f },
    { -18.0f,   2.5f, 6.6f, -0.3f },
}};

// Stress-map landscaping: loose, asymmetric clusters soften the bare compounds
// without blocking house entrances, enemy spawns, or vehicle routes.
static constexpr std::array<PalmSpawn, 6> kStressHousePalmSpawns = {{
    { -12.5f,  11.5f, 7.4f,  0.35f },
    {  12.0f,  10.8f, 6.6f, -0.28f },
    { -12.2f, -10.8f, 8.0f,  0.42f },
    {  29.5f,  11.8f, 7.7f, -0.38f },
    {  54.2f,  10.5f, 6.8f,  0.30f },
    {  54.0f, -11.2f, 8.2f, -0.45f },
}};

static void ResetPalmTrees() {
    g_trees.Initialize();
    if (g_customLevelMode) {
        for (const LevelEntity& entity : g_runtimeLevel.entities) {
            if (!entity.enabled || entity.type != LevelEntityType::Palm) continue;
            g_trees.Plant(entity.transform.position[0], entity.transform.position[2],
                entity.transform.scale[1], entity.transform.rotation[2]);
        }
        return;
    }
    for (const PalmSpawn& palm : kPalmSpawns)
        g_trees.Plant(palm.x, palm.z, palm.height * 2.0f, palm.lean);
    if (g_stressTestMode) {
        for (const PalmSpawn& palm : kStressHousePalmSpawns)
            g_trees.Plant(palm.x, palm.z, palm.height * 2.0f, palm.lean);
    }
}

static bool g_environmentInitialized = false;
static bool g_environmentStressMode = false;

static bool LoadDandelionModel() {
    if (g_dandelionModel) return true;
    const std::string root = "models/fbx_Dandelion/";
    auto model = FBXImporter::Load(root + "Dandelion.FBX",
        g_dx12.device, g_dx12.commandList, 1.0f, false, false);
    if (!model) {
        std::cerr << "Failed to load dandelion foliage model\n";
        return false;
    }

    auto material = std::make_shared<SceneMaterial>();
    material->name = "Dandelion foliage";
    material->metallicFactor = 0.0f;
    material->roughnessFactor = 1.0f;
    material->doubleSided = true;
    material->alphaCutout = true;
    material->disableOcclusionCulling = true;

    std::vector<unsigned char> albedo;
    int aw = 0, ah = 0;
    if (GLBImporter::LoadPixelsRGBA(root + "dandelion_leaf.jpg",
            albedo, aw, ah)) {
        // Source JPEG has a white backdrop instead of alpha. Convert distance
        // from white into a soft cutout while preserving the leaf colour.
        const size_t pixelCount = static_cast<size_t>(aw) * ah;
        for (size_t pixel = 0; pixel < pixelCount; ++pixel) {
            const size_t i = pixel * 4;
            const int darkness = 255 - (std::min)({
                static_cast<int>(albedo[i]), static_cast<int>(albedo[i + 1]),
                static_cast<int>(albedo[i + 2]) });
            albedo[i + 3] = static_cast<unsigned char>(
                std::clamp((darkness - 5) * 8, 0, 255));
        }
        material->baseColorTexture = GLBImporter::CreateTextureFromRGBA(
            g_dx12.device.Get(), g_dx12.commandList.Get(), albedo, aw, ah,
            material->uploadHeaps);
    }

    XMFLOAT3 boundsMin(FLT_MAX, FLT_MAX, FLT_MAX);
    XMFLOAT3 boundsMax(-FLT_MAX, -FLT_MAX, -FLT_MAX);
    const auto configure = [&](const auto& self,
                               const std::shared_ptr<SceneNode>& node) -> void {
        if (!node) return;
        if (node->mesh) for (MeshPrimitive& primitive : node->mesh->primitives) {
            primitive.material = material;
            for (size_t i = 0; i + 2 < primitive.vertices.size(); i += 12) {
                const float x = primitive.vertices[i];
                const float y = primitive.vertices[i + 1];
                const float z = primitive.vertices[i + 2];
                boundsMin.x = (std::min)(boundsMin.x, x);
                boundsMin.y = (std::min)(boundsMin.y, y);
                boundsMin.z = (std::min)(boundsMin.z, z);
                boundsMax.x = (std::max)(boundsMax.x, x);
                boundsMax.y = (std::max)(boundsMax.y, y);
                boundsMax.z = (std::max)(boundsMax.z, z);
            }
        }
        for (const auto& child : node->children) self(self, child);
    };
    configure(configure, model);
    // FBX stores each frond under a transformed child node. Bake those node
    // transforms before computing plant bounds or instancing; otherwise the
    // shared normalization transform scales around raw card coordinates and
    // pulls individual leaves away from the plant.
    if (auto merged = GLBImporter::MergeSceneByMaterial(model, g_dx12.device))
        model = std::move(merged);
    // Merge baked every source-node transform into vertex positions. Reset the
    // replacement root or FBX root rotation/scale gets applied a second time.
    model->translation = XMFLOAT3(0.0f, 0.0f, 0.0f);
    model->rotation = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
    model->scale = XMFLOAT3(1.0f, 1.0f, 1.0f);
    XMFLOAT4X4 identity;
    XMStoreFloat4x4(&identity, XMMatrixIdentity());
    model->UpdateGlobalTransform(identity);
    boundsMin = XMFLOAT3(FLT_MAX, FLT_MAX, FLT_MAX);
    boundsMax = XMFLOAT3(-FLT_MAX, -FLT_MAX, -FLT_MAX);
    const auto collectBounds = [&](const auto& self,
                                   const std::shared_ptr<SceneNode>& node) -> void {
        if (!node) return;
        if (node->mesh) for (const MeshPrimitive& primitive : node->mesh->primitives) {
            for (size_t i = 0; i + 2 < primitive.vertices.size(); i += 12) {
                boundsMin.x = (std::min)(boundsMin.x, primitive.vertices[i]);
                boundsMin.y = (std::min)(boundsMin.y, primitive.vertices[i + 1]);
                boundsMin.z = (std::min)(boundsMin.z, primitive.vertices[i + 2]);
                boundsMax.x = (std::max)(boundsMax.x, primitive.vertices[i]);
                boundsMax.y = (std::max)(boundsMax.y, primitive.vertices[i + 1]);
                boundsMax.z = (std::max)(boundsMax.z, primitive.vertices[i + 2]);
            }
        }
        for (const auto& child : node->children) self(self, child);
    };
    collectBounds(collectBounds, model);
    if (boundsMin.x == FLT_MAX || !material->baseColorTexture) {
        std::cerr << "Dandelion geometry or cutout texture missing\n";
        return false;
    }
    g_dandelionSourceCenter = XMFLOAT3(
        (boundsMin.x + boundsMax.x) * 0.5f, 0.0f,
        (boundsMin.z + boundsMax.z) * 0.5f);
    g_dandelionSourceMinY = boundsMin.y;
    g_dandelionSourceHeight = (std::max)(0.001f, boundsMax.y - boundsMin.y);
    g_dandelionModel = std::move(model);
    std::cout << "Dandelion foliage model ready\n";
    return true;
}

static void ScatterDandelions(
    const std::function<float(float, float)>& terrainSampler,
    const std::vector<GrassField::Exclusion>& exclusions) {
    g_dandelionInstances.clear();
    if (!g_dandelionModel) return;
    uint32_t rng = 0x7f4a7c15u;
    auto random01 = [&]() {
        rng = rng * 1664525u + 1013904223u;
        return static_cast<float>(rng >> 8) * (1.0f / 16777216.0f);
    };
    const auto excluded = [&](float x, float z) {
        for (const auto& e : exclusions)
            if (x >= e.minX && x <= e.maxX && z >= e.minZ && z <= e.maxZ)
                return true;
        return false;
    };
    const int clusterCount = g_stressTestMode ? 420 : 180;
    const float span = g_stressTestMode ? 196.0f : 96.0f;
    g_dandelionInstances.reserve(clusterCount * 7);
    for (int cluster = 0; cluster < clusterCount; ++cluster) {
        // First clusters sit around level-one compound so foliage is visible
        // immediately; remaining clusters cover terrain deterministically.
        static constexpr XMFLOAT2 nearbyClusters[] = {
            {  4.3f,  4.2f }, { -4.4f,  4.0f },
            {  4.1f, -4.3f }, { -4.2f, -4.4f },
            { 14.0f,  8.0f }, {-14.0f,  7.0f }
        };
        const float cx = cluster < static_cast<int>(std::size(nearbyClusters))
            ? nearbyClusters[cluster].x : (random01() - 0.5f) * span;
        const float cz = cluster < static_cast<int>(std::size(nearbyClusters))
            ? nearbyClusters[cluster].y : (random01() - 0.5f) * span;
        const int plants = 4 + static_cast<int>(random01() * 7.0f);
        for (int plant = 0; plant < plants; ++plant) {
            const float angle = random01() * XM_2PI;
            const float distance = std::sqrt(random01()) * 2.8f;
            const float x = cx + std::cos(angle) * distance;
            const float z = cz + std::sin(angle) * distance;
            if (excluded(x, z)) continue;
            const float y = terrainSampler(x, z);
            if (y < 0.22f) continue;
            const float hx = terrainSampler(x + 0.35f, z) -
                             terrainSampler(x - 0.35f, z);
            const float hz = terrainSampler(x, z + 0.35f) -
                             terrainSampler(x, z - 0.35f);
            if (hx * hx + hz * hz > 0.34f) continue;
            const float targetHeight = 0.544f + random01() * 0.376f;
            const float scale = targetHeight / g_dandelionSourceHeight;
            const float yaw = random01() * XM_2PI;
            const XMMATRIX transform =
                XMMatrixTranslation(-g_dandelionSourceCenter.x,
                                    -g_dandelionSourceMinY,
                                    -g_dandelionSourceCenter.z) *
                XMMatrixScaling(scale, scale, scale) *
                XMMatrixRotationY(yaw) *
                // Sink stem/pivot slightly so sloped terrain cannot expose a
                // gap below the lowest card.
                XMMatrixTranslation(x, y + 0.025f, z);
            DandelionInstance instance;
            XMStoreFloat4x4(&instance.transform, transform);
            instance.center = XMFLOAT3(x, y + 0.025f + targetHeight * 0.48f, z);
            instance.radius = targetHeight * 0.75f;
            g_dandelionInstances.push_back(instance);
        }
    }
    if (g_customLevelMode) {
        for (const LevelEntity& entity : g_runtimeLevel.entities) {
            if (!entity.enabled ||
                entity.type != LevelEntityType::Dandelion) continue;
            const float x = entity.transform.position[0];
            const float z = entity.transform.position[2];
            const float y = terrainSampler(x, z);
            const float authoredScale = (std::max)(0.05f, entity.transform.scale[1]);
            const float targetHeight = 0.75f * authoredScale;
            const float scale = targetHeight / g_dandelionSourceHeight;
            const XMMATRIX transform =
                XMMatrixTranslation(-g_dandelionSourceCenter.x,
                                    -g_dandelionSourceMinY,
                                    -g_dandelionSourceCenter.z) *
                XMMatrixScaling(scale, scale, scale) *
                XMMatrixRotationRollPitchYaw(
                    XMConvertToRadians(entity.transform.rotation[0]),
                    XMConvertToRadians(entity.transform.rotation[1]),
                    XMConvertToRadians(entity.transform.rotation[2])) *
                XMMatrixTranslation(x, y + 0.025f, z);
            DandelionInstance instance;
            XMStoreFloat4x4(&instance.transform, transform);
            instance.center = XMFLOAT3(x, y + 0.025f + targetHeight * 0.48f, z);
            instance.radius = targetHeight * 0.75f;
            g_dandelionInstances.push_back(instance);
        }
    }
    std::cout << "Dandelion foliage: " << g_dandelionInstances.size()
              << " GPU instances\n";
}

static void RebuildScalableEnvironment() {
    auto terrainParams = CurrentTerrainParams();
    terrainParams.heightScale = scene.terrainHeightScale;
    auto terrainSampler = [terrainParams](float x, float z) {
        return TerrainRendererDX12::HeightAt(terrainParams, x, z);
    };
    std::vector<NavigationObstacle> obstacles = {
        { -2.6f,  -1.5f,  2.6f,   1.5f },
        {-29.0f, -27.0f,-15.0f, -13.0f }
    };
    std::vector<GrassField::Exclusion> grassExclusions;
    std::vector<GrassField::AuthoredPatch> grassPatches;
    if (g_customLevelMode) {
        obstacles.clear();
        obstacles.push_back({-29.0f, -27.0f, -15.0f, -13.0f});
        for (const LevelEntity& entity : g_runtimeLevel.entities) {
            if (!entity.enabled) continue;
            if (entity.type == LevelEntityType::WoodHouse ||
                entity.type == LevelEntityType::MetalHouse) {
                const float yaw = XMConvertToRadians(entity.transform.rotation[1]);
                const float c = std::abs(std::cos(yaw));
                const float s = std::abs(std::sin(yaw));
                const float halfX = c * 4.2f + s * 3.4f;
                const float halfZ = s * 4.2f + c * 3.4f;
                NavigationObstacle house{ entity.transform.position[0] - halfX,
                    entity.transform.position[2] - halfZ,
                    entity.transform.position[0] + halfX,
                    entity.transform.position[2] + halfZ };
                obstacles.push_back(house);
                grassExclusions.push_back({ house.minX - 0.4f, house.minZ - 0.4f,
                    house.maxX + 0.4f, house.maxZ + 0.4f });
            } else if (entity.type == LevelEntityType::Palm) {
                obstacles.push_back({ entity.transform.position[0] - 0.55f,
                    entity.transform.position[2] - 0.55f,
                    entity.transform.position[0] + 0.55f,
                    entity.transform.position[2] + 0.55f });
            } else if (entity.type == LevelEntityType::Humvee) {
                const float yaw = XMConvertToRadians(entity.transform.rotation[1]);
                const float halfX = std::abs(std::cos(yaw)) * 2.6f +
                    std::abs(std::sin(yaw)) * 1.5f;
                const float halfZ = std::abs(std::sin(yaw)) * 2.6f +
                    std::abs(std::cos(yaw)) * 1.5f;
                obstacles.push_back({ entity.transform.position[0] - halfX,
                    entity.transform.position[2] - halfZ,
                    entity.transform.position[0] + halfX,
                    entity.transform.position[2] + halfZ });
            } else if (entity.type == LevelEntityType::GrassPatch) {
                grassPatches.push_back({ entity.transform.position[0],
                    entity.transform.position[2],
                    (std::max)(0.15f, entity.transform.scale[0]),
                    (std::max)(0.05f, entity.transform.scale[1]),
                    static_cast<uint32_t>(entity.id) });
            }
        }
    } else {
    const size_t houseCount = g_stressTestMode ? kStressHouseCount : 4;
    size_t houseIndex = 0;
    const size_t compoundCount = (houseCount + kSpawnersPerCompound - 1) /
        kSpawnersPerCompound;
    for (size_t i = 0; i < compoundCount; ++i) {
        const float x = kStressCompoundCenters[i].x;
        const float z = kStressCompoundCenters[i].z;
        const NavigationObstacle houses[kSpawnersPerCompound] = {
            {x - 4.2f, z + 5.8f, x + 4.2f, z + 12.2f},
            {x + 5.8f, z - 3.4f, x + 12.2f, z + 3.4f},
            {x - 4.2f, z - 12.2f, x + 4.2f, z - 5.8f},
            {x - 12.2f, z - 3.4f, x - 5.8f, z + 3.4f}
        };
        for (size_t side = 0;
             side < kSpawnersPerCompound && houseIndex < houseCount;
             ++side, ++houseIndex) {
            obstacles.push_back(houses[side]);
            constexpr float grassClearance = 0.4f;
            grassExclusions.push_back({
                houses[side].minX - grassClearance,
                houses[side].minZ - grassClearance,
                houses[side].maxX + grassClearance,
                houses[side].maxZ + grassClearance });
        }
    }
    if (g_stressTestMode) {
        obstacles.push_back({39.4f, 1.5f, 44.6f, 4.5f});
        grassExclusions.push_back({39.0f, 1.1f, 45.0f, 4.9f});
    }
    for (const PalmSpawn& palm : kPalmSpawns)
        obstacles.push_back(
            { palm.x - 0.55f, palm.z - 0.55f,
              palm.x + 0.55f, palm.z + 0.55f });
    if (g_stressTestMode) {
        for (const PalmSpawn& palm : kStressHousePalmSpawns)
            obstacles.push_back(
                { palm.x - 0.55f, palm.z - 0.55f,
                  palm.x + 0.55f, palm.z + 0.55f });
    }
    }
    const float extent = g_customLevelMode ? 122.0f :
        (g_stressTestMode ? 122.0f : 61.0f);
    if (!g_navigation.BuildTerrain(terrainSampler, -extent, extent,
            -extent, extent, obstacles))
        std::cerr << "Recast navigation build failed; Bandits use direct steering\n";

    g_grass.Initialize(terrainSampler,
        g_stressTestMode ? 200.0f : 100.0f,
        g_stressTestMode ? 1600000 : 400000, 0.0f, grassExclusions, grassPatches);
    ScatterDandelions(terrainSampler, grassExclusions);
    g_environmentInitialized = true;
    g_environmentStressMode = g_stressTestMode;
}

static void UpdateHelicopterHoverAudio() {
    const bool primaryActive = !g_helicopterDead && !g_helicopterCrashed;
    const bool secondaryActive = g_stressTestMode &&
        !g_secondaryHelicopterDead && !g_secondaryHelicopterCrashed;
    const bool active = IsGameplayScreen() &&
        scene.showHelicopter && (primaryActive || secondaryActive) &&
        (scene.playerGodMode || scene.playerHealth > 0.0f);
    auto distanceToCamera = [](const XMFLOAT3& position) {
        const float dx = position.x - scene.camera.Position.x;
        const float dy = position.y - scene.camera.Position.y;
        const float dz = position.z - scene.camera.Position.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    };
    float distance = primaryActive ? distanceToCamera(g_helicopterPosition) : FLT_MAX;
    if (secondaryActive)
        distance = (std::min)(distance,
                              distanceToCamera(g_secondaryHelicopterPosition));
    const float volume = 0.72f * (std::max)(0.0f, 1.0f - distance / 95.0f);
    g_helicopterHoverAudio.SetLoop(active, volume, 0.96f);
}

static void SetCursorVisible(bool visible) {
    if (visible) {
        while (ShowCursor(TRUE) < 0) {}
    } else {
        while (ShowCursor(FALSE) >= 0) {}
    }
}

static void OpenMainMenu() {
    gameScreen = GameScreen::MainMenu;
    showUI = false;
    cameraLocked = true;
    ReleaseCapture();
    SetCursorVisible(true);
}

static const LevelEntity* FirstRuntimeEntity(LevelEntityType type) {
    auto it = std::find_if(g_runtimeLevel.entities.begin(), g_runtimeLevel.entities.end(),
        [&](const LevelEntity& entity) { return entity.enabled && entity.type == type; });
    return it == g_runtimeLevel.entities.end() ? nullptr : &*it;
}

static void ApplyRuntimeLevelBasics(bool movePlayer) {
    if (!g_customLevelMode) return;
    scene.terrainHeightScale = g_runtimeLevel.terrainHeightScale;
    scene.explosiveBarrels.clear();
    for (const LevelEntity& entity : g_runtimeLevel.entities) {
        if (!entity.enabled || entity.type != LevelEntityType::ExplosiveBarrel) continue;
        scene.explosiveBarrels.push_back({{ entity.transform.position[0],
                                            entity.transform.position[1],
                                            entity.transform.position[2] }});
    }
    if (const LevelEntity* humvee = FirstRuntimeEntity(LevelEntityType::Humvee)) {
        g_primaryHumveeSpawn = { humvee->transform.position[0],
                                 humvee->transform.position[1],
                                 humvee->transform.position[2] };
        g_primaryHumveeYaw = humvee->transform.rotation[1];
    }
    if (const LevelEntity* helicopter = FirstRuntimeEntity(LevelEntityType::Helicopter)) {
        g_helicopterSpawn = { helicopter->transform.position[0],
                              helicopter->transform.position[1],
                              helicopter->transform.position[2] };
        g_helicopterPosition = g_helicopterSpawn;
        g_helicopterLevelScale = helicopter->transform.scale[0];
        g_helicopterYaw = XMConvertToRadians(helicopter->transform.rotation[1]);
        scene.showHelicopter = true;
    } else { scene.showHelicopter = false; g_helicopterLevelScale = 1.0f; }
    if (movePlayer) {
        if (const LevelEntity* player = FirstRuntimeEntity(LevelEntityType::PlayerSpawn)) {
            scene.camera = Camera({ player->transform.position[0],
                                    player->transform.position[1],
                                    player->transform.position[2] });
            scene.camera.Yaw = player->transform.rotation[1] - 90.0f;
            scene.camera.Pitch = player->transform.rotation[0];
            scene.camera.ProcessMouseMovement(0.0f, 0.0f);
        }
    }
}

static void SynchronizeEditorRuntime(bool play);
static void StartLevelEditor(HWND hwnd);
static void StopEditorPlaytest();

static void OpenWinScreen() {
    levelTimerRunning = false;
    gameScreen = GameScreen::WinScreen;
    showUI = false;
    cameraLocked = true;
    ReleaseCapture();
    SetCursorVisible(true);
}

static void StartLevelOne(HWND hwnd, bool godMode, bool stressTest = false,
                          bool emptyLevel = false,
                          const LevelDefinition* customLevel = nullptr) {
    gameScreen = GameScreen::Level1;
    g_customLevelMode = !stressTest && !emptyLevel;
    if (customLevel) {
        g_customLevelMode = true;
        g_runtimeLevel = *customLevel;
        g_activeCustomLevelName = customLevel->name;
    } else {
        g_activeCustomLevelName.clear();
        if (g_customLevelMode) g_runtimeLevel = MakeLevelOneTemplate();
    }
    g_runtimeTerrainSculpt = g_customLevelMode
        ? g_runtimeLevel.terrainSculpt : std::vector<TerrainSculptStamp>{};
    g_terrain.SetSculptStamps(g_runtimeTerrainSculpt);
    g_grass.ClearRuntimeExclusions();
    g_primaryHumveeSpawn = { 0.0f, 3.45f, 0.0f };
    g_primaryHumveeYaw = 0.0f;
    g_helicopterLevelScale = 1.0f;
    g_emptyLevelMode = emptyLevel;
    g_stressTestMode = stressTest && !emptyLevel;
    const bool modeAssetsLoaded = g_emptyLevelMode
        ? emptyLevelAssetsLoaded : fullLevelAssetsLoaded;
    if (modeAssetsLoaded)
        wallModel = g_stressTestMode ? stressWallModel : normalWallModel;
    levelElapsedSeconds = 0.0f;
    levelTimerRunning = modeAssetsLoaded;
    if (!modeAssetsLoaded) BeginLevelLoading();
    g_pendingEnvironmentRebuild = modeAssetsLoaded && g_customLevelMode;
    scene.playerGodMode = godMode;
    scene.RestorePlayerHealth();
    scene.ResetLevelRuntimeState();
    scene.camera = Camera(XMFLOAT3(0.0f, 5.0f, 10.0f));
    scene.gun.visible = true;
    scene.showHelicopter = !g_emptyLevelMode;
    g_heldBandit = nullptr;
    g_heldBarrelIndex = SIZE_MAX;
    g_drivingHumvee = false;
    g_savedGunVisible = true;
    g_previousHumveePositionValid = false;
    g_humveeHouseImpactCooldown = 0.0f;
    g_humveeTurretYaw = 0.0f;
    g_humveeTurretFireCooldown = 0.0f;
    g_humveeAimPoint = {};
    g_helicopterMainRotorAngle = 0.0f;
    g_helicopterTailRotorAngle = 0.0f;
    g_helicopterYaw = 0.0f;
    g_helicopterPitch = 0.0f;
    g_helicopterRoll = 0.0f;
    g_helicopterHoverTime = 0.0f;
    g_helicopterFireCooldown = 0.0f;
    g_helicopterFireCycleTime = 0.0f;
    g_helicopterHealth = kHelicopterMaxHealth;
    g_helicopterDead = false;
    g_helicopterCrashed = false;
    g_helicopterPosition = { 0.0f, 14.0f, 0.0f };
    g_helicopterSpawn = g_helicopterPosition;
    g_helicopterCrashVelocity = { 0.0f, 0.0f, 0.0f };
    g_secondaryHelicopterYaw = 0.0f;
    g_secondaryHelicopterPitch = 0.0f;
    g_secondaryHelicopterRoll = 0.0f;
    g_secondaryHelicopterHoverTime = 1.7f;
    g_secondaryHelicopterFireCooldown = 0.0f;
    g_secondaryHelicopterFireCycleTime = 3.5f;
    g_secondaryHelicopterHealth = kHelicopterMaxHealth;
    g_secondaryHelicopterDead = false;
    g_secondaryHelicopterCrashed = false;
    g_secondaryHelicopterPosition = { 42.0f, 14.0f, 0.0f };
    g_secondaryHelicopterCrashVelocity = { 0.0f, 0.0f, 0.0f };
    g_banditSpawnSerial = 0;
    g_banditVoiceCooldown = 0.0f;
    g_banditPainCooldown = 0.0f;
    g_suppressFireUntilMouseRelease = true;
    ApplyRuntimeLevelBasics(true);
    if (modeAssetsLoaded)
        scene.rebuildDestructionRequested = true;
    pendingLevelRuntimeReset = modeAssetsLoaded;
    deathCursorReleased = false;
    showUI = false;
    cameraLocked = false;
    SetCapture(hwnd);
    SetCursorVisible(false);
    RECT rect;
    GetClientRect(hwnd, &rect);
    POINT center = { (rect.right - rect.left) / 2,
                     (rect.bottom - rect.top) / 2 };
    ClientToScreen(hwnd, &center);
    ignoreNextMouseMove = true;
    SetCursorPos(center.x, center.y);
    lastX = (float)(rect.right - rect.left) * 0.5f;
    lastY = (float)(rect.bottom - rect.top) * 0.5f;
    firstMouse = true;
}

static void StartCustomLevel(HWND hwnd, const std::filesystem::path& path) {
    LevelLoadResult loaded = LoadLevel(path);
    if (!loaded.ok) {
        g_mainMenuLevelStatus = "Load failed: " + loaded.error;
        return;
    }
    g_mainMenuLevelStatus.clear();
    StartLevelOne(hwnd, false, false, false, &loaded.level);
}

static void BrowseAndStartCustomLevel(HWND hwnd) {
    std::error_code error;
    std::filesystem::create_directories("levels", error);
    const std::wstring initialDirectory =
        std::filesystem::absolute("levels", error).wstring();
    wchar_t selected[MAX_PATH] = {};
    OPENFILENAMEW dialog = {};
    dialog.lStructSize = sizeof(dialog);
    dialog.hwndOwner = hwnd;
    dialog.lpstrFilter = L"Level JSON (*.json)\0*.json\0All files (*.*)\0*.*\0\0";
    dialog.lpstrFile = selected;
    dialog.nMaxFile = static_cast<DWORD>(std::size(selected));
    dialog.lpstrInitialDir = initialDirectory.c_str();
    dialog.lpstrDefExt = L"json";
    dialog.lpstrTitle = L"Load Custom Level";
    dialog.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST |
        OFN_NOCHANGEDIR | OFN_DONTADDTORECENT;
    if (GetOpenFileNameW(&dialog)) {
        StartCustomLevel(hwnd, std::filesystem::path(selected));
    } else if (const DWORD code = CommDlgExtendedError()) {
        g_mainMenuLevelStatus = "File browser failed: " + std::to_string(code);
    }
}

static void RestartActiveLevel(HWND hwnd) {
    if (!g_activeCustomLevelName.empty()) {
        const LevelDefinition custom = g_runtimeLevel;
        StartLevelOne(hwnd, false, false, false, &custom);
    } else {
        StartLevelOne(hwnd, false);
    }
}

static void RenderLoadingScreen();

static void RenderMainMenu(HWND hwnd) {
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::GetBackgroundDrawList()->AddRectFilled(
        ImVec2(0, 0), display, IM_COL32(5, 9, 12, 225));
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    const float menuHeight = (std::min)(650.0f, display.y - 24.0f);
    ImGui::SetNextWindowSize(ImVec2(430.0f, menuHeight), ImGuiCond_Always);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse;
    ImGui::Begin("Main Menu", nullptr, flags);
    ImGui::Dummy(ImVec2(0.0f, 20.0f));
    const char* title = "SMALLEST GRAPHICS ENGINE";
    ImGui::SetCursorPosX((430.0f - ImGui::CalcTextSize(title).x) * 0.5f);
    ImGui::TextUnformatted(title);
    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    const char* subtitle = "LEVEL SELECT";
    ImGui::SetCursorPosX((430.0f - ImGui::CalcTextSize(subtitle).x) * 0.5f);
    ImGui::TextDisabled("%s", subtitle);
    ImGui::Dummy(ImVec2(0.0f, 22.0f));
    ImGui::SetCursorPosX(65.0f);
    if (ImGui::Button("LEVEL 1", ImVec2(300.0f, 58.0f)))
        StartLevelOne(hwnd, false);
    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    ImGui::SetCursorPosX(65.0f);
    if (ImGui::Button("LEVEL 1 - GOD MODE", ImVec2(300.0f, 58.0f)))
        StartLevelOne(hwnd, true);
    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    ImGui::SetCursorPosX(65.0f);
    if (ImGui::Button("LEVEL EDITOR", ImVec2(300.0f, 58.0f)))
        StartLevelEditor(hwnd);
    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    ImGui::SetCursorPosX(65.0f);
    if (ImGui::Button("CUSTOM LEVELS", ImVec2(300.0f, 58.0f)))
        BrowseAndStartCustomLevel(hwnd);
    if (!g_mainMenuLevelStatus.empty())
        ImGui::TextWrapped("%s", g_mainMenuLevelStatus.c_str());
    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    ImGui::SetCursorPosX(65.0f);
    if (ImGui::Button("EMPTY", ImVec2(300.0f, 58.0f)))
        StartLevelOne(hwnd, true, false, true);
    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    ImGui::SetCursorPosX(65.0f);
    if (ImGui::Button("STRESS TEST", ImVec2(300.0f, 58.0f)))
        StartLevelOne(hwnd, true, true);
    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    ImGui::SetCursorPosX(65.0f);
    if (ImGui::Button("QUIT", ImVec2(300.0f, 42.0f)))
        PostQuitMessage(0);
    ImGui::End();
    if (levelLoadingActive) RenderLoadingScreen();
}

static void RenderLoadingScreen() {
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::GetBackgroundDrawList()->AddRectFilled(
        ImVec2(0, 0), display, IM_COL32(4, 8, 12, 238));
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f),
        ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(680.0f, 450.0f), ImGuiCond_Always);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoInputs;
    ImGui::Begin("Loading Level", nullptr, flags);
    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    const char* title = gameScreen == GameScreen::LevelEditor ? "LOADING LEVEL EDITOR" :
        (g_emptyLevelMode ? "LOADING EMPTY"
        : (g_stressTestMode ? "LOADING STRESS TEST" :
           (!g_activeCustomLevelName.empty() ? "LOADING CUSTOM LEVEL" : "LOADING LEVEL 1")));
    ImGui::SetCursorPosX((680.0f - ImGui::CalcTextSize(title).x) * 0.5f);
    ImGui::TextUnformatted(title);
    ImGui::Dummy(ImVec2(0.0f, 6.0f));

    char progressText[64];
    std::snprintf(progressText, sizeof(progressText), "Task %u / %u  (%.0f%%)",
        levelLoadingTaskIndex, levelLoadTaskCount,
        levelLoadingProgress * 100.0f);
    ImGui::ProgressBar(levelLoadingProgress, ImVec2(-1.0f, 25.0f), progressText);
    ImGui::Text("Current: %s", levelLoadingLabel.c_str());
    ImGui::TextDisabled("Asset:   %s", levelLoadingAsset.c_str());

    const double totalMs = LevelLoadElapsedMs(levelLoadingStartedAt);
    const double taskMs = LevelLoadElapsedMs(levelLoadingTaskStartedAt);
    ImGui::Text("Elapsed: %.2f s total | %.2f s current task",
        totalMs / 1000.0, taskMs / 1000.0);

    const StaticBufferStatsDX12 uploads = GetStaticBufferStatsDX12();
    const uint64_t uploadBytes = uploads.bytes >= levelLoadingUploadBaseline.bytes
        ? uploads.bytes - levelLoadingUploadBaseline.bytes : 0;
    const uint32_t uploadResources =
        uploads.resources >= levelLoadingUploadBaseline.resources
        ? uploads.resources - levelLoadingUploadBaseline.resources : 0;
    ImGui::Text("GPU buffers: %u resources | %.2f MiB queued/created",
        uploadResources, static_cast<double>(uploadBytes) / (1024.0 * 1024.0));
    ImGui::Text("Uploads: %u submitted total | %u last frame | %u pending",
        levelLoadingSubmittedUploads, levelLoadingLastSubmittedUploads,
        uploads.pendingUploads);

    const StaticBufferDiagnosticDX12 resource = GetStaticBufferDiagnosticDX12();
    const char* resourceState = "D3D12_RESOURCE_STATE_UNKNOWN";
    if (resource.finalState == D3D12_RESOURCE_STATE_INDEX_BUFFER)
        resourceState = "D3D12_RESOURCE_STATE_INDEX_BUFFER";
    else if (resource.finalState == D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)
        resourceState = "D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE";
    else if (resource.finalState ==
        (D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER |
         D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE))
        resourceState = "D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | NON_PIXEL_SHADER_RESOURCE";
    else if (resource.finalState == D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER)
        resourceState = "D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER";
    ImGui::Separator();
    ImGui::Text("DX12 resource #%llu: %s  %.2f KiB",
        static_cast<unsigned long long>(resource.serial), resource.label.c_str(),
        static_cast<double>(resource.bytes) / 1024.0);
    ImGui::TextDisabled(
        "CreateCommittedResource(DEFAULT, COPY_DEST) + UPLOAD(GENERIC_READ)");
    ImGui::TextDisabled(
        "COPY queue: CopyBufferRegion | DIRECT queue: final barrier/render");
    ImGui::TextDisabled("CopyBufferRegion -> ResourceBarrier(");
    ImGui::TextDisabled("  COPY_DEST -> %s [0x%X])", resourceState,
        static_cast<unsigned>(resource.finalState));

    const HRESULT gpuHealth = g_dx12.device
        ? g_dx12.device->GetDeviceRemovedReason() : E_POINTER;
    if (gpuHealth == S_OK)
        ImGui::TextColored(ImVec4(0.35f, 1.0f, 0.45f, 1.0f), "GPU: OK");
    else
        ImGui::TextColored(ImVec4(1.0f, 0.25f, 0.2f, 1.0f),
            "GPU: REMOVED 0x%08lX", static_cast<unsigned long>(gpuHealth));

    ImGui::Separator();
    ImGui::TextUnformatted("Recent tasks");
    if (levelLoadingRecords.empty()) {
        ImGui::TextDisabled("Waiting for first task...");
    } else {
        for (auto it = levelLoadingRecords.rbegin();
             it != levelLoadingRecords.rend(); ++it) {
            ImGui::TextColored(it->succeeded
                ? ImVec4(0.55f, 0.95f, 0.6f, 1.0f)
                : ImVec4(1.0f, 0.45f, 0.3f, 1.0f),
                "%s  %s  %.1f ms", it->succeeded ? "OK" : "FAIL",
                it->label.c_str(), it->milliseconds);
        }
    }
    ImGui::End();
}

static void RenderDeathScreen(HWND hwnd) {
    if (!deathCursorReleased) {
        cameraLocked = true;
        ReleaseCapture();
        SetCursorVisible(true);
        deathCursorReleased = true;
    }
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::GetBackgroundDrawList()->AddRectFilled(
        ImVec2(0, 0), display, IM_COL32(25, 0, 0, 190));
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(390.0f, 235.0f), ImGuiCond_Always);
    ImGui::Begin("Death Screen", nullptr, ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse);
    ImGui::Dummy(ImVec2(0.0f, 15.0f));
    const char* died = "YOU DIED";
    ImGui::SetCursorPosX((390.0f - ImGui::CalcTextSize(died).x) * 0.5f);
    ImGui::TextColored(ImVec4(1.0f, 0.12f, 0.08f, 1.0f), "%s", died);
    ImGui::Dummy(ImVec2(0.0f, 20.0f));
    ImGui::SetCursorPosX(45.0f);
    if (ImGui::Button(g_activeCustomLevelName.empty() ? "RESTART LEVEL 1" :
            "RESTART CUSTOM LEVEL", ImVec2(300.0f, 55.0f)))
        RestartActiveLevel(hwnd);
    ImGui::Dummy(ImVec2(0.0f, 9.0f));
    ImGui::SetCursorPosX(45.0f);
    if (ImGui::Button("MAIN MENU", ImVec2(300.0f, 45.0f)))
        OpenMainMenu();
    ImGui::End();
}

static void RenderWinScreen(HWND hwnd) {
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::GetBackgroundDrawList()->AddRectFilled(
        ImVec2(0, 0), display, IM_COL32(0, 22, 12, 215));
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(410.0f, 292.0f), ImGuiCond_Always);
    ImGui::Begin("Win Screen", nullptr, ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoCollapse);
    ImGui::Dummy(ImVec2(0.0f, 14.0f));
    const char* won = "LEVEL 1 COMPLETE";
    ImGui::SetCursorPosX((410.0f - ImGui::CalcTextSize(won).x) * 0.5f);
    ImGui::TextColored(ImVec4(0.25f, 1.0f, 0.42f, 1.0f), "%s", won);
    ImGui::Dummy(ImVec2(0.0f, 8.0f));
    const char* cleared = "ALL ENEMIES ELIMINATED";
    ImGui::SetCursorPosX((410.0f - ImGui::CalcTextSize(cleared).x) * 0.5f);
    ImGui::TextDisabled("%s", cleared);
    const int totalMilliseconds = static_cast<int>(
        levelElapsedSeconds * 1000.0f + 0.5f);
    const int minutes = totalMilliseconds / 60000;
    const int seconds = (totalMilliseconds / 1000) % 60;
    const int milliseconds = totalMilliseconds % 1000;
    char timeText[48];
    std::snprintf(timeText, sizeof(timeText), "TIME  %02d:%02d.%03d",
                  minutes, seconds, milliseconds);
    ImGui::Dummy(ImVec2(0.0f, 10.0f));
    ImGui::SetCursorPosX((410.0f - ImGui::CalcTextSize(timeText).x) * 0.5f);
    ImGui::TextUnformatted(timeText);
    ImGui::Dummy(ImVec2(0.0f, 14.0f));
    ImGui::SetCursorPosX(55.0f);
    if (ImGui::Button(g_activeCustomLevelName.empty() ? "REPLAY LEVEL 1" :
            "REPLAY CUSTOM LEVEL", ImVec2(300.0f, 55.0f)))
        RestartActiveLevel(hwnd);
    ImGui::Dummy(ImVec2(0.0f, 9.0f));
    ImGui::SetCursorPosX(55.0f);
    if (ImGui::Button("MAIN MENU", ImVec2(300.0f, 45.0f)))
        OpenMainMenu();
    ImGui::End();
}

static std::string ResolveTexturePath(const char* relativePath) {
    namespace fs = std::filesystem;
    const fs::path requested(relativePath);
    std::vector<fs::path> roots = { fs::current_path() };

    wchar_t modulePath[MAX_PATH] = {};
    const DWORD moduleLength = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    if (moduleLength > 0 && moduleLength < MAX_PATH)
        roots.push_back(fs::path(modulePath).parent_path());

    for (const fs::path& root : roots) {
        const fs::path candidates[] = {
            root / requested,
            root / "build" / requested,
            root.parent_path() / requested,
            root.parent_path() / "build" / requested
        };
        for (const fs::path& candidate : candidates) {
            if (fs::exists(candidate))
                return fs::weakly_canonical(candidate).string();
        }
    }
    return relativePath;
}

static std::vector<unsigned char> PinkMissingTexture(int size) {
    std::vector<unsigned char> pixels((size_t)size * size * 4);
    for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x) {
        const bool bright = ((x / 16) ^ (y / 16)) & 1;
        const size_t i = ((size_t)y * size + x) * 4;
        pixels[i + 0] = bright ? 255 : 90;
        pixels[i + 1] = 0;
        pixels[i + 2] = bright ? 255 : 90;
        pixels[i + 3] = 255;
    }
    return pixels;
}

// Island ground material: sand (Poly Haven "sand_02", CC0). The previous mud set
// was never actually in the repo, so the terrain had been falling back to the pink
// missing-texture placeholder.
static void LoadFloorMudMaterial() {
    floorMaterial = std::make_shared<SceneMaterial>();
    floorMaterial->name = "grass_004";
    floorMaterial->baseColorFactor = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
    floorMaterial->metallicFactor = 0.0f;
    floorMaterial->roughnessFactor = 1.0f;
    floorMaterial->viewFillStrength = 0.0f;

    const std::string dir = "models/grass/Grass004_2K-PNG/";
    floorMaterial->baseColorTexture = GLBImporter::LoadTextureFromFile(
        ResolveTexturePath((dir + "Grass004_2K-PNG_Color.png").c_str()),
        g_dx12.device, g_dx12.commandList, floorMaterial->uploadHeaps);
    floorMaterial->normalTexture = GLBImporter::LoadTextureFromFile(
        ResolveTexturePath((dir + "Grass004_2K-PNG_NormalGL.png").c_str()),
        g_dx12.device, g_dx12.commandList, floorMaterial->uploadHeaps);
    floorMaterial->normalYSign = 1.0f;
    floorMaterial->metallicRoughnessTexture = GLBImporter::LoadTextureFromFile(
        ResolveTexturePath((dir + "Grass004_2K-PNG_Roughness.png").c_str()),
        g_dx12.device, g_dx12.commandList, floorMaterial->uploadHeaps);
    floorMaterial->roughnessOnlyTexture =
        floorMaterial->metallicRoughnessTexture != nullptr;

    if (!floorMaterial->baseColorTexture) {
        const auto missing = PinkMissingTexture(256);
        floorMaterial->baseColorTexture = GLBImporter::CreateTextureFromRGBA(
            g_dx12.device.Get(), g_dx12.commandList.Get(), missing, 256, 256,
            floorMaterial->uploadHeaps);
        floorMaterial->baseColorFactor = XMFLOAT4(1, 1, 1, 1);
        std::cerr << "Grass ground texture unavailable; using pink missing texture\n";
    }

    // Soft smoke sprite for particle billboards.
    g_smokeTexture = GLBImporter::LoadTextureFromFile(
        ResolveTexturePath("models/textures/smoke.png"),
        g_dx12.device, g_dx12.commandList, g_smokeUploadHeaps);
    if (!g_smokeTexture)
        std::cerr << "Smoke sprite (models/textures/smoke.png) unavailable\n";

    g_bloodTexture = GLBImporter::LoadTextureFromFile(
        ResolveTexturePath("models/textures/blood_splat.png"),
        g_dx12.device, g_dx12.commandList, g_bloodUploadHeaps);
    if (!g_bloodTexture)
        std::cerr << "Blood sprite (models/textures/blood_splat.png) unavailable\n";

    g_muzzleFlashTexture = GLBImporter::LoadTextureFromFile(
        ResolveTexturePath("models/textures/muzzle_flash.png"),
        g_dx12.device, g_dx12.commandList, g_muzzleFlashUploadHeaps);
    if (!g_muzzleFlashTexture)
        std::cerr << "Muzzle flash (models/textures/muzzle_flash.png) unavailable\n";

    g_fireTexture = GLBImporter::LoadTextureFromFile(
        ResolveTexturePath("models/textures/fire1_64.png"),
        g_dx12.device, g_dx12.commandList, g_fireUploadHeaps);
    if (!g_fireTexture)
        std::cerr << "Fire sprite (models/textures/fire1_64.png) unavailable\n";

    g_explosionTexture = GLBImporter::LoadTextureFromFile(
        ResolveTexturePath("models/textures/explosion_soluna.png"),
        g_dx12.device, g_dx12.commandList, g_explosionUploadHeaps);
    if (!g_explosionTexture)
        std::cerr << "Explosion sheet (models/textures/explosion_soluna.png) unavailable\n";
}

// Crysis-style plank wall: the destructible is built from real structural
// pieces -- vertical wooden planks held by horizontal cross-beams -- rather
// than a uniform Voronoi field. Each plank/beam is one child chunk, so a hit
// snaps that board loose along its true edges. Grid coords (x,y,z) drive
// Blast's adjacency bonding, so touching boards stay welded until struck.
// ?? procedural material textures for the destructible house ?????????????????
namespace HouseTex {
// Cheap hash-based value noise in [0,1].
inline float Hash(int x, int y) {
    uint32_t h = (uint32_t)(x * 374761393 + y * 668265263);
    h = (h ^ (h >> 13)) * 1274126177u;
    return ((h ^ (h >> 16)) & 0xFFFFFF) / (float)0xFFFFFF;
}
inline float ValueNoise(float x, float y) {
    const int xi = (int)std::floor(x), yi = (int)std::floor(y);
    const float fx = x - xi, fy = y - yi;
    const float sx = fx * fx * (3 - 2 * fx), sy = fy * fy * (3 - 2 * fy);
    const float a = Hash(xi, yi), b = Hash(xi + 1, yi);
    const float c = Hash(xi, yi + 1), d = Hash(xi + 1, yi + 1);
    return (a + (b - a) * sx) + ((c + (d - c) * sx) - (a + (b - a) * sx)) * sy;
}
inline float Fbm(float x, float y) {
    float sum = 0, amp = 0.5f, freq = 1;
    for (int o = 0; o < 4; ++o) { sum += ValueNoise(x * freq, y * freq) * amp; freq *= 2; amp *= 0.5f; }
    return sum;
}
inline unsigned char ToByte(float v) { return (unsigned char)std::max(0.0f, std::min(255.0f, v * 255.0f + 0.5f)); }

// Wood: vertical grain lines along V with warped rings and knots.
inline std::vector<unsigned char> Wood(int size, XMFLOAT3 base, XMFLOAT3 dark) {
    std::vector<unsigned char> px((size_t)size * size * 4);
    for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x) {
        const float u = (float)x / size, v = (float)y / size;
        const float warp = Fbm(u * 3.0f, v * 12.0f) * 0.35f;
        float grain = std::sin((u * 18.0f + warp) * 3.14159f);
        grain = 0.5f + 0.5f * grain * grain;                 // sharpen streaks
        grain = grain * 0.7f + Fbm(u * 40.0f, v * 6.0f) * 0.3f;
        const float t = std::min(1.0f, grain);
        const size_t i = ((size_t)y * size + x) * 4;
        px[i + 0] = ToByte(dark.x + (base.x - dark.x) * t);
        px[i + 1] = ToByte(dark.y + (base.y - dark.y) * t);
        px[i + 2] = ToByte(dark.z + (base.z - dark.z) * t);
        px[i + 3] = 255;
    }
    return px;
}
// Stone: blocky mortar grid with speckled fill.
inline std::vector<unsigned char> Stone(int size, XMFLOAT3 base, XMFLOAT3 mortar) {
    std::vector<unsigned char> px((size_t)size * size * 4);
    for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x) {
        const float u = (float)x / size * 4.0f, v = (float)y / size * 4.0f;
        const float bx = u - std::floor(u), by = v - std::floor(v);
        const float mortarLine = std::min(std::min(bx, 1 - bx), std::min(by, 1 - by));
        const float m = mortarLine < 0.06f ? 0.0f : 1.0f;
        const float speck = 0.6f + 0.4f * Fbm(u * 8.0f, v * 8.0f);
        const XMFLOAT3 c = { base.x * speck, base.y * speck, base.z * speck };
        const size_t i = ((size_t)y * size + x) * 4;
        px[i + 0] = ToByte(mortar.x + (c.x - mortar.x) * m);
        px[i + 1] = ToByte(mortar.y + (c.y - mortar.y) * m);
        px[i + 2] = ToByte(mortar.z + (c.z - mortar.z) * m);
        px[i + 3] = 255;
    }
    return px;
}
// Shingles: overlapping horizontal rows, staggered, with edge shadow.
inline std::vector<unsigned char> Shingle(int size, XMFLOAT3 base, XMFLOAT3 dark) {
    std::vector<unsigned char> px((size_t)size * size * 4);
    for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x) {
        const float u = (float)x / size, v = (float)y / size;
        const float row = v * 10.0f;
        const int ri = (int)std::floor(row);
        const float rf = row - ri;
        const float offset = (ri & 1) ? 0.5f : 0.0f;
        const float col = (u * 8.0f + offset);
        const float cf = col - std::floor(col);
        float shade = 1.0f - rf * 0.35f;                     // top-lit row
        if (cf < 0.04f || rf > 0.94f) shade *= 0.55f;         // shingle gaps
        shade *= 0.85f + 0.15f * Fbm(u * 20.0f, v * 20.0f);
        const size_t i = ((size_t)y * size + x) * 4;
        px[i + 0] = ToByte(dark.x + (base.x - dark.x) * shade);
        px[i + 1] = ToByte(dark.y + (base.y - dark.y) * shade);
        px[i + 2] = ToByte(dark.z + (base.z - dark.z) * shade);
        px[i + 3] = 255;
    }
    return px;
}
// Corrugated metal: tight vertical ribs shaded like a sine wave, streaked with
// grime and rust patches -- reads as galvanised roofing sheets.
inline std::vector<unsigned char> Corrugated(int size, XMFLOAT3 base, XMFLOAT3 rust) {
    std::vector<unsigned char> px((size_t)size * size * 4);
    for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x) {
        const float u = (float)x / size, v = (float)y / size;
        // Rib shading: sine across U, lit from one side so every rib has a
        // bright crest and a dark valley.
        const float rib = std::sin(u * 3.14159f * 2.0f * 34.0f);
        const float crease = std::pow(std::abs(rib), 10.0f);
        float shade = 0.48f + 0.42f * std::max(0.0f, rib) + 0.16f * crease;
        // Vertical weather streaks running down the sheet.
        shade *= 0.88f + 0.12f * Fbm(u * 60.0f, v * 4.0f);
        // Sparse rust blotches.
        const float rustMask = Fbm(u * 5.0f, v * 10.0f);
        const float drip = Fbm(u * 70.0f, v * 1.6f);
        const float r = rustMask > 0.70f ? std::min(1.0f, (rustMask - 0.70f) * 4.0f + drip * 0.20f) : drip * 0.025f;
        const size_t i = ((size_t)y * size + x) * 4;
        px[i + 0] = ToByte((base.x * shade) * (1 - r) + rust.x * r);
        px[i + 1] = ToByte((base.y * shade) * (1 - r) + rust.y * r);
        px[i + 2] = ToByte((base.z * shade) * (1 - r) + rust.z * r);
        px[i + 3] = 255;
    }
    return px;
}
}  // namespace HouseTex

// Assigns downloaded CC0 albedo textures (ambientCG, models/house_pbr) to the
// house's shared materials by name, falling back to procedurally generated
// wood/stone/shingle when a file is missing. Call after building the house,
// while a command list is open for the texture uploads.
static void ApplyHouseTextures(const std::shared_ptr<SceneNode>& house,
                               ID3D12Device* device, ID3D12GraphicsCommandList* cmdList) {
    if (!house) return;
    constexpr int kSize = 256;
    const std::vector<unsigned char> missingFallback = PinkMissingTexture(kSize);
    // Collect the unique materials by name from the house children.
    std::unordered_map<std::string, std::shared_ptr<SceneMaterial>> mats;
    for (const auto& child : house->children) {
        if (!child || !child->mesh) continue;
        for (const auto& prim : child->mesh->primitives)
            if (prim.material) mats[prim.material->name] = prim.material;
    }
    // Fracture chunks are closed prisms with visible interior faces. Meshlet
    // cone/HZB rejection can mistake the previous frame's shell for an
    // occluder and punch holes through intact walls. Cull these by frustum only.
    for (auto& entry : mats) {
        auto& material = entry.second;
        if (!material) continue;
        material->doubleSided = true;
        material->disableOcclusionCulling = true;
    }
    // Load the downloaded albedo + normal maps. Albedo falls back to the
    // procedural texture so the house never renders untextured; the normal map
    // is optional (skipped if the file is absent).
    auto assign = [&](const char* name, const std::string& base, std::vector<unsigned char> fallback) {
        auto it = mats.find(name);
        if (it == mats.end()) return;
        auto& mat = it->second;
        mat->baseColorTexture = GLBImporter::LoadTextureFromFile(base + ".jpg", device, cmdList, mat->uploadHeaps);
        if (!mat->baseColorTexture) {
            mat->baseColorTexture = GLBImporter::CreateTextureFromRGBA(
                device, cmdList, missingFallback, kSize, kSize, mat->uploadHeaps);
        }
        if (mat->baseColorTexture) mat->baseColorFactor = XMFLOAT4(1, 1, 1, 1);
        mat->normalTexture = GLBImporter::LoadTextureFromFile(base + "_normal.jpg", device, cmdList, mat->uploadHeaps);
        // Roughness map: grayscale JPG whose G channel the shader reads as
        // roughness (glTF metallic-roughness convention). Metal stays 0.
        mat->metallicRoughnessTexture = GLBImporter::LoadTextureFromFile(
            base + "_roughness.jpg", device, cmdList, mat->uploadHeaps);
        mat->roughnessOnlyTexture = mat->metallicRoughnessTexture != nullptr;
        if (mat->metallicRoughnessTexture) mat->metallicFactor = 0.0f;
    };
    auto assignFile = [&](const char* name, const std::string& colorPath,
                          const std::string& roughnessPath, std::vector<unsigned char> fallback) {
        auto it = mats.find(name);
        if (it == mats.end()) return;
        auto& mat = it->second;
        mat->baseColorTexture = GLBImporter::LoadTextureFromFile(colorPath, device, cmdList, mat->uploadHeaps);
        if (!mat->baseColorTexture) {
            mat->baseColorTexture = GLBImporter::CreateTextureFromRGBA(
                device, cmdList, missingFallback, kSize, kSize, mat->uploadHeaps);
        }
        if (mat->baseColorTexture) mat->baseColorFactor = XMFLOAT4(1, 1, 1, 1);
        mat->metallicRoughnessTexture = GLBImporter::LoadTextureFromFile(
            roughnessPath, device, cmdList, mat->uploadHeaps);
        mat->roughnessOnlyTexture = mat->metallicRoughnessTexture != nullptr;
        mat->metallicFactor = 0.75f;
        mat->roughnessFactor = mat->metallicRoughnessTexture ? 1.0f : 0.55f;
    };
    auto assignGeneratedMetal = [&](const char* name, const std::string& roughnessPath,
                                    std::vector<unsigned char> generated, float metallic, float roughness) {
        auto it = mats.find(name);
        if (it == mats.end()) return;
        auto& mat = it->second;
        mat->baseColorTexture = GLBImporter::CreateTextureFromRGBA(
            device, cmdList, generated, kSize, kSize, mat->uploadHeaps);
        if (mat->baseColorTexture) mat->baseColorFactor = XMFLOAT4(1, 1, 1, 1);
        mat->metallicRoughnessTexture = GLBImporter::LoadTextureFromFile(
            roughnessPath, device, cmdList, mat->uploadHeaps);
        mat->roughnessOnlyTexture = mat->metallicRoughnessTexture != nullptr;
        mat->metallicFactor = metallic;
        mat->roughnessFactor = roughness;
    };
    auto assignPackedPBR = [&](const char* name, const std::string& colorPath,
                               const std::string& normalPath, const std::string& armPath,
                               std::vector<unsigned char> fallback) {
        auto it = mats.find(name);
        if (it == mats.end()) return;
        auto& mat = it->second;
        mat->baseColorTexture = GLBImporter::LoadTextureFromFile(
            ResolveTexturePath(colorPath.c_str()), device, cmdList, mat->uploadHeaps);
        if (!mat->baseColorTexture) {
            mat->baseColorTexture = GLBImporter::CreateTextureFromRGBA(
                device, cmdList, missingFallback, kSize, kSize, mat->uploadHeaps);
        }
        mat->normalTexture = GLBImporter::LoadTextureFromFile(
            ResolveTexturePath(normalPath.c_str()), device, cmdList, mat->uploadHeaps);
        mat->metallicRoughnessTexture = GLBImporter::LoadTextureFromFile(
            ResolveTexturePath(armPath.c_str()), device, cmdList, mat->uploadHeaps);
        if (mat->baseColorTexture) mat->baseColorFactor = XMFLOAT4(1, 1, 1, 1);
        // Poly Haven ARM is R=AO, G=roughness, B=metallic: exact glTF layout.
        mat->roughnessOnlyTexture = false;
        mat->metallicFactor = 1.0f;
        mat->roughnessFactor = 1.0f;
    };
    assign("Foundation", "models/house_pbr/foundation_brick",
           HouseTex::Stone(kSize, { 0.62f, 0.62f, 0.64f }, { 0.34f, 0.34f, 0.36f }));
    assign("Stud", "models/house_pbr/stud_wood",
           HouseTex::Wood(kSize, { 0.60f, 0.42f, 0.25f }, { 0.34f, 0.22f, 0.12f }));
    // Cladding uses the single-board wood (not the multi-plank field) so the
    // grain reads as real boards when tiled.
    assign("Cladding", "models/house_pbr/stud_wood",
           HouseTex::Wood(kSize, { 0.84f, 0.68f, 0.46f }, { 0.55f, 0.40f, 0.24f }));
    // Corrugated metal sheets; no downloaded map for this one, so the
    // procedural ribbed texture always kicks in.
    assign("Roof", "models/house_pbr/roof_metal",
           HouseTex::Corrugated(kSize, { 0.72f, 0.74f, 0.76f }, { 0.42f, 0.25f, 0.16f }));
    assignGeneratedMetal("MetalWall",
               "models/Corrugated metal pack/Wall/A/A Roughness rusted 2.jpg",
               HouseTex::Corrugated(kSize, { 0.30f, 0.34f, 0.31f }, { 0.43f, 0.22f, 0.10f }), 0.82f, 0.66f);
    assignPackedPBR("MetalRoof",
               "models/polyhaven/corrugated_iron/corrugated_iron_diff_2k.jpg",
               "models/polyhaven/corrugated_iron/corrugated_iron_nor_dx_2k.jpg",
               "models/polyhaven/corrugated_iron/corrugated_iron_arm_2k.jpg",
               HouseTex::Corrugated(kSize, { 0.42f, 0.46f, 0.48f }, { 0.38f, 0.18f, 0.08f }));
}

// Basic modular destructible house built from real structural pieces: a
// foundation slab and floor, four walls made of vertical studs + cladding,
// door/window openings, and a two-slope roof of rafters + sheets. Each piece
// is one child chunk. Pieces whose name starts with "Support:" are anchored to
// the world by the destruction layer, so foundation and bottom wall plates
// stay static and hold the structure up; disconnected sections fall. Blast
// bonds touching pieces so a hit tears loose only what it structurally frees.
static std::shared_ptr<SceneNode> CreateDestructibleWallModel() {
    // House footprint (world units). Front faces +Z toward the spawn area.
    constexpr float minX = -7.0f, maxX = 0.0f;   // flat terrain near world origin
    constexpr float minZ = 1.0f, maxZ = 6.0f;    // depth
    // The island's ground sits above sea level, so the houses are built on the flat
    // pad stamped into the terrain -- not at y = 0, or they end up buried in sand.
    // The roofs (RoofModel.h) are placed off this same constant.
    constexpr float floorY = Ground::kBuildingPadY, wallTop = floorY + 3.4f;
    constexpr float wall = 0.28f;                // wall / slab thickness
    // Terrain pad is exactly floorY. Sink slab undersides slightly so their
    // bottom triangles never occupy the same depth plane as terrain.
    constexpr float foundationEmbed = 0.06f;

    auto root = std::make_shared<SceneNode>("DestructibleHouse");
    auto matFoundation = std::make_shared<SceneMaterial>();
    matFoundation->name = "Foundation";
    matFoundation->baseColorFactor = XMFLOAT4(0.55f, 0.55f, 0.58f, 1.0f);
    matFoundation->metallicFactor = 0.0f; matFoundation->roughnessFactor = 0.95f;
    auto matStud = std::make_shared<SceneMaterial>();
    matStud->name = "Stud";
    matStud->baseColorFactor = XMFLOAT4(0.58f, 0.40f, 0.24f, 1.0f);
    matStud->metallicFactor = 0.0f; matStud->roughnessFactor = 0.88f;
    auto matCladding = std::make_shared<SceneMaterial>();
    matCladding->name = "Cladding";
    matCladding->baseColorFactor = XMFLOAT4(0.82f, 0.66f, 0.44f, 1.0f);
    matCladding->metallicFactor = 0.0f; matCladding->roughnessFactor = 0.85f;
    auto matRoof = std::make_shared<SceneMaterial>();
    matRoof->name = "Roof";
    matRoof->baseColorFactor = XMFLOAT4(0.68f, 0.70f, 0.72f, 1.0f);
    matRoof->metallicFactor = 0.65f; matRoof->roughnessFactor = 0.45f;  // galvanised sheet
    auto matMetalWall = std::make_shared<SceneMaterial>();
    matMetalWall->name = "MetalWall";
    matMetalWall->baseColorFactor = XMFLOAT4(0.62f, 0.63f, 0.62f, 1.0f);
    matMetalWall->metallicFactor = 0.80f; matMetalWall->roughnessFactor = 0.62f;
    auto matMetalRoof = std::make_shared<SceneMaterial>();
    matMetalRoof->name = "MetalRoof";
    matMetalRoof->baseColorFactor = XMFLOAT4(0.58f, 0.58f, 0.56f, 1.0f);
    matMetalRoof->metallicFactor = 0.85f; matMetalRoof->roughnessFactor = 0.58f;
    auto matDarkMetal = std::make_shared<SceneMaterial>();
    matDarkMetal->name = "DarkMetal";
    matDarkMetal->baseColorFactor = XMFLOAT4(0.015f, 0.018f, 0.017f, 1.0f);
    matDarkMetal->metallicFactor = 0.60f; matDarkMetal->roughnessFactor = 0.72f;
    auto matTrim = std::make_shared<SceneMaterial>();
    matTrim->name = "MetalTrim";
    matTrim->baseColorFactor = XMFLOAT4(0.46f, 0.50f, 0.50f, 1.0f);
    matTrim->metallicFactor = 0.90f; matTrim->roughnessFactor = 0.38f;
    auto matGlass = std::make_shared<SceneMaterial>();
    matGlass->name = "Glass";
    // Hybrid rendering correctly composites panes after every opaque object.
    // Keep the tint subtle; the old 0.28 alpha only looked acceptable because
    // forward rendering accidentally overwrote glass with later opaque draws.
    matGlass->baseColorFactor = XMFLOAT4(0.58f, 0.76f, 0.86f, 0.04f);
    matGlass->metallicFactor = 0.0f; matGlass->roughnessFactor = 0.04f;

    // Emit one axis-aligned solid box as a chunk child. `wrap` stretches the
    // texture to span the whole piece (UV 0..1 per face) so a single-board wood
    // texture reads as one plank; otherwise UVs map by world X/Y (tiling).
    auto addBox = [&](const char* name, const std::shared_ptr<SceneMaterial>& material,
                      float x0, float x1, float y0, float y1, float z0, float z1,
                      bool wrap = false) {
        if (x1 <= x0 || y1 <= y0 || z1 <= z0) return;
        auto node = std::make_shared<SceneNode>(name);
        node->mesh = std::make_shared<SceneMesh>();
        MeshPrimitive primitive;
        primitive.material = material;
        constexpr float kUvScale = 1.5f;  // world units per texture tile
        const float extX = x1 - x0, extY = y1 - y0, extZ = z1 - z0;
        auto emitQuad = [&](const XMFLOAT3& a, const XMFLOAT3& b, const XMFLOAT3& c,
                            const XMFLOAT3& d, const XMFLOAT3& n) {
            const UINT base = (UINT)(primitive.vertices.size() / 12);
            const XMFLOAT3 pts[4] = { a, b, c, d };
            const bool faceX = std::abs(n.x) > 0.5f;
            const bool faceY = std::abs(n.y) > 0.5f;
            for (const XMFLOAT3& p : pts) {
                float u, v;
                if (wrap) {
                    // One texture span across the whole board. Long axis -> U so
                    // the plank grain runs along the board's length.
                    if (faceX)      { u = (p.z - z0) / extZ; v = (p.y - y0) / extY; }
                    else if (faceY) { u = (p.x - x0) / extX; v = (p.z - z0) / extZ; }
                    else            { u = (p.x - x0) / extX; v = (p.y - y0) / extY; }
                } else {
                    // World-scaled tiling per face -> uniform texel size, no
                    // stretching whatever the face orientation.
                    if (faceX)      { u = p.z; v = p.y; }
                    else if (faceY) { u = p.x; v = p.z; }
                    else            { u = p.x; v = p.y; }
                    u /= kUvScale; v /= kUvScale;
                }
                // Tangent must lie in the face and follow U, and must NOT be
                // parallel to the normal -- a flat (1,0,0) on an X-facing side
                // collapses the TBN to zero and the normal map samples as noise.
                const XMFLOAT3 tangent = faceX ? XMFLOAT3(0, 0, 1)   // U runs along Z
                                       : XMFLOAT3(1, 0, 0);           // U runs along X
                const XMFLOAT3 bitangent = faceY ? XMFLOAT3(0, 0, 1)
                                                 : XMFLOAT3(0, 1, 0);
                const XMVECTOR crossNT = XMVector3Cross(
                    XMLoadFloat3(&n), XMLoadFloat3(&tangent));
                const float tangentW = XMVectorGetX(XMVector3Dot(
                    crossNT, XMLoadFloat3(&bitangent))) >= 0.0f ? 1.0f : -1.0f;
                const float vertex[12] = { p.x,p.y,p.z, n.x,n.y,n.z, u,v,
                                           tangent.x, tangent.y, tangent.z, tangentW };
                primitive.vertices.insert(primitive.vertices.end(), vertex, vertex + 12);
            }
            primitive.indices.insert(primitive.indices.end(),
                { base, base + 1, base + 2, base, base + 2, base + 3 });
        };
        emitQuad({x0,y0,z1},{x1,y0,z1},{x1,y1,z1},{x0,y1,z1}, {0,0,1});    // front (+Z)
        emitQuad({x1,y0,z0},{x0,y0,z0},{x0,y1,z0},{x1,y1,z0}, {0,0,-1});   // back
        emitQuad({x0,y0,z0},{x0,y0,z1},{x0,y1,z1},{x0,y1,z0}, {-1,0,0});   // left
        emitQuad({x1,y0,z1},{x1,y0,z0},{x1,y1,z0},{x1,y1,z1}, {1,0,0});    // right
        emitQuad({x0,y1,z1},{x1,y1,z1},{x1,y1,z0},{x0,y1,z0}, {0,1,0});    // top
        emitQuad({x0,y0,z0},{x1,y0,z0},{x1,y0,z1},{x0,y0,z1}, {0,-1,0});   // bottom
        node->mesh->primitives.push_back(std::move(primitive));
        root->AddChild(node);
    };

    // Emit one board as a cluster of Voronoi prism cells. The board's largest
    // face is split by a jittered-site Voronoi diagram (half-plane clipping);
    // each convex cell is extruded through the board thickness into its own
    // chunk child. All cells share `name` (with an "@<id>" group suffix) so the
    // destruction layer bonds them into one piece -- the seams only show once
    // the board breaks apart, and they are jagged rather than straight cuts.
    // Axes are derived from the box: thickness = thinnest extent, the Voronoi
    // plane spans the two remaining axes (L = longer of the two, H = other).
    // `shatter` = glass mode: a denser 2D scatter of sites so the pane breaks
    // into many small shards instead of a few plank-like slices.
    // `xform`, if given, is applied to every emitted vertex after the board is
    // built in its axis-aligned local frame -- used to tilt roof panels onto a
    // real slope. Positions transform fully; normals/tangents by rotation only.
    auto addVoronoiBoard = [&](const char* name, const std::shared_ptr<SceneMaterial>& material,
                               float x0, float x1, float y0, float y1,
                               float z0, float z1, int seed, bool shatter = false,
                               const XMMATRIX* xform = nullptr) {
        if (x1 <= x0 || y1 <= y0 || z1 <= z0) return;
        const float lo[3] = { x0, y0, z0 }, hi[3] = { x1, y1, z1 };
        const float ext[3] = { x1 - x0, y1 - y0, z1 - z0 };
        const int tAxis = (ext[0] <= ext[1] && ext[0] <= ext[2]) ? 0
                        : (ext[1] <= ext[2] ? 1 : 2);
        const int r0 = tAxis == 0 ? 1 : 0, r1 = tAxis == 2 ? 1 : 2;
        const int lAxis = ext[r0] >= ext[r1] ? r0 : r1;
        const int hAxis = lAxis == r0 ? r1 : r0;
        const float l0 = lo[lAxis], l1 = hi[lAxis];
        const float h0 = lo[hAxis], h1 = hi[hAxis];
        const float t0 = lo[tAxis], t1 = hi[tAxis];
        struct P2 { float x, y; };
        const float length = l1 - l0;
        // Deterministic integer-hash jitter (no RNG); the piece id seeds it so
        // every board breaks along different lines.
        std::vector<P2> sites;
        if (shatter) {
            // Glass: 2D grid of jittered sites -> many small angular shards.
            const int cols = std::max(3, std::min(6, (int)std::lround(length / 0.35f)));
            const int rows = std::max(2, std::min(4, (int)std::lround((h1 - h0) / 0.4f)));
            const float cw = length / cols, ch = (h1 - h0) / rows;
            for (int r = 0; r < rows; ++r) for (int i = 0; i < cols; ++i) {
                const float jx = (((i * 37 + r * 53 + seed * 17 + 3) % 13) / 12.0f - 0.5f) * cw * 0.9f;
                const float jy = (((i * 19 + r * 29 + seed * 41 + 7) % 11) / 10.0f - 0.5f) * ch * 0.9f;
                sites.push_back({ l0 + (i + 0.5f) * cw + jx, h0 + (r + 0.5f) * ch + jy });
            }
        } else {
            const int cols = std::max(3, std::min(8, (int)std::lround(length / 1.2f)));
            const float cellW = length / cols;
            for (int i = 0; i < cols; ++i) {
                const float jx = (((i * 37 + seed * 17 + 3) % 13) / 12.0f - 0.5f) * cellW * 0.9f;
                const float jy = (((i * 19 + seed * 41 + 7) % 11) / 10.0f - 0.5f) * (h1 - h0) * 0.8f;
                sites.push_back({ l0 + (i + 0.5f) * cellW + jx, (h0 + h1) * 0.5f + jy });
            }
        }
        // Voronoi cell = board rect clipped against the perpendicular bisector
        // of every other site (Sutherland-Hodgman). Result is convex and CCW.
        auto clipCell = [&](size_t s) {
            std::vector<P2> poly = { {l0,h0},{l1,h0},{l1,h1},{l0,h1} };
            for (size_t o = 0; o < sites.size() && !poly.empty(); ++o) {
                if (o == s) continue;
                const float nx = sites[o].x - sites[s].x, ny = sites[o].y - sites[s].y;
                const float c = (sites[o].x * sites[o].x + sites[o].y * sites[o].y
                               - sites[s].x * sites[s].x - sites[s].y * sites[s].y) * 0.5f;
                std::vector<P2> out;
                for (size_t i = 0; i < poly.size(); ++i) {
                    const P2 pa = poly[i], pb = poly[(i + 1) % poly.size()];
                    const float da = pa.x * nx + pa.y * ny - c;
                    const float db = pb.x * nx + pb.y * ny - c;
                    const bool ia = da <= 0.00001f, ib = db <= 0.00001f;
                    if (ia) out.push_back(pa);
                    if (ia != ib) {
                        const float t = da / (da - db);
                        out.push_back({ pa.x + (pb.x - pa.x) * t, pa.y + (pb.y - pa.y) * t });
                    }
                }
                poly.swap(out);
            }
            return poly;
        };
        auto toWorld = [&](const P2& p, float t) {
            float w[3]; w[lAxis] = p.x; w[hAxis] = p.y; w[tAxis] = t;
            return XMFLOAT3(w[0], w[1], w[2]);
        };
        auto axisUnit = [](int axis) {
            return XMFLOAT3(axis == 0 ? 1.0f : 0.0f, axis == 1 ? 1.0f : 0.0f,
                            axis == 2 ? 1.0f : 0.0f);
        };
        for (size_t s = 0; s < sites.size(); ++s) {
            const std::vector<P2> poly = clipCell(s);
            if (poly.size() < 3) continue;
            auto node = std::make_shared<SceneNode>(name);
            node->mesh = std::make_shared<SceneMesh>();
            MeshPrimitive prim;
            prim.material = material;
            constexpr float kUvScale = 1.5f;  // world units per texture tile
            auto emitTri = [&](XMFLOAT3 ta, XMFLOAT3 tb, XMFLOAT3 tc,
                               const XMFLOAT3& n, const XMFLOAT3& tan) {
                // Fix winding so the triangle faces its lighting normal.
                const XMVECTOR geometric = XMVector3Cross(
                    XMVectorSubtract(XMLoadFloat3(&tb), XMLoadFloat3(&ta)),
                    XMVectorSubtract(XMLoadFloat3(&tc), XMLoadFloat3(&ta)));
                if (XMVectorGetX(XMVector3Dot(geometric, XMLoadFloat3(&n))) < 0.0f)
                    std::swap(tb, tc);
                const UINT base = (UINT)(prim.vertices.size() / 12);
                const XMFLOAT3 pts[3] = { ta, tb, tc };
                const bool faceX = std::abs(n.x) > 0.5f, faceY = std::abs(n.y) > 0.5f;
                auto uvFor = [&](const XMFLOAT3& p) {
                    if (faceX) return XMFLOAT2(p.z / kUvScale, p.y / kUvScale);
                    if (faceY) return XMFLOAT2(p.x / kUvScale, p.z / kUvScale);
                    return XMFLOAT2(p.x / kUvScale, p.y / kUvScale);
                };
                const XMFLOAT2 uv0 = uvFor(ta), uv1 = uvFor(tb), uv2 = uvFor(tc);
                const XMFLOAT3 edge1(tb.x - ta.x, tb.y - ta.y, tb.z - ta.z);
                const XMFLOAT3 edge2(tc.x - ta.x, tc.y - ta.y, tc.z - ta.z);
                const float du1 = uv1.x - uv0.x, dv1 = uv1.y - uv0.y;
                const float du2 = uv2.x - uv0.x, dv2 = uv2.y - uv0.y;
                const float uvDet = du1 * dv2 - dv1 * du2;
                XMFLOAT3 generatedTangent = tan;
                float tangentW = 1.0f;
                if (std::abs(uvDet) > 0.000001f) {
                    const float invDet = 1.0f / uvDet;
                    XMVECTOR tangentV = XMVectorSet(
                        (edge1.x * dv2 - edge2.x * dv1) * invDet,
                        (edge1.y * dv2 - edge2.y * dv1) * invDet,
                        (edge1.z * dv2 - edge2.z * dv1) * invDet, 0.0f);
                    const XMVECTOR bitangentV = XMVectorSet(
                        (edge2.x * du1 - edge1.x * du2) * invDet,
                        (edge2.y * du1 - edge1.y * du2) * invDet,
                        (edge2.z * du1 - edge1.z * du2) * invDet, 0.0f);
                    tangentV = XMVector3Normalize(tangentV);
                    XMStoreFloat3(&generatedTangent, tangentV);
                    tangentW = XMVectorGetX(XMVector3Dot(
                        XMVector3Cross(XMLoadFloat3(&n), tangentV), bitangentV)) >= 0.0f
                        ? 1.0f : -1.0f;
                }
                for (const XMFLOAT3& p : pts) {
                    // World-scaled tiling per dominant face axis (same rule as
                    // addBox) -> uniform texel size on the jagged side walls.
                    // UVs use the LOCAL (pre-tilt) position so the corrugations
                    // run straight along the panel regardless of slope.
                    float u, v;
                    if (faceX)      { u = p.z; v = p.y; }
                    else if (faceY) { u = p.x; v = p.z; }
                    else            { u = p.x; v = p.y; }
                    u /= kUvScale; v /= kUvScale;
                    // Tilt into world space if a transform was supplied.
                    XMFLOAT3 wp = p, wn = n, wt = generatedTangent;
                    if (xform) {
                        XMStoreFloat3(&wp, XMVector3Transform(XMLoadFloat3(&p), *xform));
                        XMStoreFloat3(&wn, XMVector3Normalize(XMVector3TransformNormal(XMLoadFloat3(&n), *xform)));
                        XMStoreFloat3(&wt, XMVector3Normalize(XMVector3TransformNormal(XMLoadFloat3(&tan), *xform)));
                    }
                    const float vert[12] = { wp.x,wp.y,wp.z, wn.x,wn.y,wn.z, u,v,
                                             wt.x,wt.y,wt.z, tangentW };
                    prim.vertices.insert(prim.vertices.end(), vert, vert + 12);
                }
                prim.indices.insert(prim.indices.end(), { base, base + 1, base + 2 });
            };
            P2 centroid{ 0.0f, 0.0f };
            for (const P2& p : poly) { centroid.x += p.x; centroid.y += p.y; }
            centroid.x /= (float)poly.size(); centroid.y /= (float)poly.size();
            const XMFLOAT3 capN = axisUnit(tAxis);
            const XMFLOAT3 capNeg(-capN.x, -capN.y, -capN.z);
            const XMFLOAT3 capTan = axisUnit(lAxis);   // in the cap plane
            const XMFLOAT3 sideTan = capN;  // extrusion axis lies in every side face
            for (size_t i = 0; i < poly.size(); ++i) {
                const P2 a2 = poly[i], b2 = poly[(i + 1) % poly.size()];
                // Caps: fan around the centroid on both thickness faces.
                emitTri(toWorld(centroid, t1), toWorld(a2, t1), toWorld(b2, t1), capN, capTan);
                emitTri(toWorld(centroid, t0), toWorld(a2, t0), toWorld(b2, t0), capNeg, capTan);
                // Transparent glass cells share these edges while intact. Their
                // internal extrusion walls showed through both caps as bright
                // pre-cracked lines. Glass keeps two caps (and therefore real
                // collision thickness), but only separation exposes shard gaps.
                if (shatter) continue;
                // Side wall for this edge; outward normal from the CCW polygon.
                const float ex = b2.x - a2.x, ey = b2.y - a2.y;
                const float elen = std::sqrt(ex * ex + ey * ey);
                if (elen < 0.0001f) continue;
                const P2 sn2{ ey / elen, -ex / elen };     // outward in (L, H)
                const XMFLOAT3 snEnd = toWorld(sn2, 0.0f); // map plane dir to world
                const XMFLOAT3 snOrg = toWorld({ 0, 0 }, 0.0f);
                const XMFLOAT3 sn(snEnd.x - snOrg.x, snEnd.y - snOrg.y, snEnd.z - snOrg.z);
                emitTri(toWorld(a2, t0), toWorld(b2, t0), toWorld(b2, t1), sn, sideTan);
                emitTri(toWorld(a2, t0), toWorld(b2, t1), toWorld(a2, t1), sn, sideTan);
            }
            node->mesh->primitives.push_back(std::move(prim));
            root->AddChild(node);
        }
    };

    // --- Foundation: single anchored slab spanning the footprint. ---
    addBox("Support:Foundation", matFoundation, minX, maxX,
        floorY - foundationEmbed, floorY + wall, minZ, maxZ);

    // --- Studded wall: vertical studs + outer cladding along one edge. The
    // bottom row of studs is anchored (the sill plate), so the wall stands. ---
    int pieceId = 0;  // unique Voronoi group id per board across the house
    // Windows occupy cladding rows 1..2 (a band from ~0.9 to ~2.15 above the
    // floor) so the cladding cutout lines up exactly with board seams.
    constexpr int boards = 5;         // cladding planks stacked up the wall
    constexpr int kWinRowLo = 1, kWinRowHi = 2;  // rows the window band covers
    // `windows` = horizontal span-offset ranges (start, end) along the wall.
    auto buildWall = [&](float x0, float x1, float z0, float z1, bool alongX,
                         float openStart, float openEnd, float openTop,
                         const std::vector<std::pair<float, float>>& windows) {
        constexpr float studW = 0.16f;
        constexpr int studCount = 8;
        const float baseY = floorY + wall;                 // sit on foundation
        const float rowH = (wallTop - baseY) / boards;
        const float winB = baseY + rowH * kWinRowLo;       // window band bottom
        const float winT = baseY + rowH * (kWinRowHi + 1); // window band top
        const float span = alongX ? (x1 - x0) : (z1 - z0);
        auto inWindow = [&](float c) {
            for (const auto& w : windows) if (c > w.first && c < w.second) return true;
            return false;
        };
        for (int s = 0; s <= studCount; ++s) {
            const float t = (float)s / studCount;
            const float c = t * span;
            // Skip studs that fall inside the opening (door/window gap).
            const bool inOpening = openEnd > openStart && c > openStart && c < openEnd;
            // Do not add anchored corner supports to the wooden house.
            if (s == 0) continue;
            // A stud crossing a window splits into a sill stub below the glass
            // and a header stub above it.
            const bool crossesWindow = inWindow(c);
            auto emitStud = [&](float sy0, float sy1) {
                if (sy1 <= sy0) return;
                const int id = pieceId++;
                const std::string studName = "Stud@" + std::to_string(id);
                if (alongX) {
                    const float sx = x0 + c;
                    addVoronoiBoard(studName.c_str(), matStud, sx - studW * 0.5f, sx + studW * 0.5f,
                                    sy0, sy1, z0, z1, id);
                } else {
                    const float sz = z0 + c;
                    addVoronoiBoard(studName.c_str(), matStud, x0, x1, sy0, sy1,
                                    sz - studW * 0.5f, sz + studW * 0.5f, id);
                }
            };
            if (inOpening) {
                // Header stub above the opening keeps the wall continuous up top.
                emitStud(openTop, wallTop);
            } else if (crossesWindow) {
                emitStud(baseY, winB);
                emitStud(winT, wallTop);
            } else {
                emitStud(baseY, wallTop);
            }
        }
        // Cladding: horizontal planks over the studs. Each plank is one visible
        // board built from flush Voronoi prism cells sharing one group id, so a
        // hit knocks a jagged cell out of the board instead of a straight strip.
        // Rows crossing the window band are split into segments around the glass.
        const float cladT = 0.08f;
        auto emitClad = [&](float c0, float c1, float by0, float by1) {
            if (c1 - c0 < 0.25f) return;  // skip slivers
            const int id = pieceId++;
            const std::string plankName = "Cladding@" + std::to_string(id);
            if (alongX) {
                const bool front = (z0 + z1) * 0.5f > (minZ + maxZ) * 0.5f;
                const float cz0 = front ? z1 : z0 - cladT;
                const float cz1 = front ? z1 + cladT : z0;
                addVoronoiBoard(plankName.c_str(), matCladding, x0 + c0, x0 + c1, by0, by1,
                                cz0, cz1, id);
            } else {
                const bool right = (x0 + x1) * 0.5f > (minX + maxX) * 0.5f;
                const float cx0 = right ? x1 : x0 - cladT;
                const float cx1 = right ? x1 + cladT : x0;
                addVoronoiBoard(plankName.c_str(), matCladding, cx0, cx1, by0, by1,
                                z0 + c0, z0 + c1, id);
            }
        };
        for (int b = 0; b < boards; ++b) {
            const float by0 = baseY + rowH * b;
            const float by1 = baseY + rowH * (b + 1);
            if (b >= kWinRowLo && b <= kWinRowHi && !windows.empty()) {
                // Cut the row around each window opening.
                float cursor = 0.0f;
                for (const auto& w : windows) {
                    emitClad(cursor, w.first, by0, by1);
                    cursor = w.second;
                }
                emitClad(cursor, span, by0, by1);
            } else {
                emitClad(0.0f, span, by0, by1);
            }
        }
        // Glass panes: one thin shatter-mode board per window, centred in the
        // wall so it bonds to the cladding edges and stud stubs around it.
        for (const auto& w : windows) {
            const int id = pieceId++;
            const std::string glassName = "Glass@" + std::to_string(id);
            constexpr float glassT = 0.015f;  // half thickness
            if (alongX) {
                const float zc = (z0 + z1) * 0.5f;
                addVoronoiBoard(glassName.c_str(), matGlass, x0 + w.first, x0 + w.second,
                                winB, winT, zc - glassT, zc + glassT, id, true);
            } else {
                const float xc = (x0 + x1) * 0.5f;
                addVoronoiBoard(glassName.c_str(), matGlass, xc - glassT, xc + glassT,
                                winB, winT, z0 + w.first, z0 + w.second, id, true);
            }
        }
    };

    // Front wall (+Z) with a door opening in the middle and a window either
    // side; one window on the back and each side wall.
    const float frontSpan = maxX - minX, sideSpan = maxZ - minZ;
    // Door head height is measured from the floor, not absolute -- otherwise the
    // opening stays at the old ground level when the building pad moves.
    buildWall(minX, maxX, maxZ - wall, maxZ, true, frontSpan * 0.42f, frontSpan * 0.58f,
              floorY + 2.2f,
              { { frontSpan * 0.10f, frontSpan * 0.30f }, { frontSpan * 0.70f, frontSpan * 0.90f } });
    buildWall(minX, maxX, minZ, minZ + wall, true, 0.0f, 0.0f, 0.0f,
              { { frontSpan * 0.38f, frontSpan * 0.62f } });                    // back
    buildWall(minX, minX + wall, minZ, maxZ, false, 0.0f, 0.0f, 0.0f,
              { { sideSpan * 0.32f, sideSpan * 0.68f } });                      // left
    buildWall(maxX - wall, maxX, minZ, maxZ, false, 0.0f, 0.0f, 0.0f,
              { { sideSpan * 0.32f, sideSpan * 0.68f } });                      // right

    // --- Roof: Crysis-style corrugated metal on two real angled slopes meeting
    // at a ridge. Each slope is a row of thin panels authored flat, then tilted
    // about its eave edge to the roof pitch. One "Roof@<id>" panel = one sheet
    // that tears off whole when hit. ---
    const float ridgeY = wallTop + 1.0f;          // ridge height above the eaves
    const float midX = (minX + maxX) * 0.5f;
    auto addWoodGable = [&](float nearZ, float farZ) {
        const int id = pieceId++;
        auto node = std::make_shared<SceneNode>("Cladding@" + std::to_string(id));
        node->mesh = std::make_shared<SceneMesh>();
        MeshPrimitive prim; prim.material = matCladding;
        auto tri = [&](XMFLOAT3 a, XMFLOAT3 b, XMFLOAT3 c) {
            const XMFLOAT3 ab(b.x-a.x,b.y-a.y,b.z-a.z), ac(c.x-a.x,c.y-a.y,c.z-a.z);
            XMFLOAT3 n(ab.y*ac.z-ab.z*ac.y, ab.z*ac.x-ab.x*ac.z, ab.x*ac.y-ab.y*ac.x);
            const float len=std::sqrt(n.x*n.x+n.y*n.y+n.z*n.z);
            if(len>0.00001f){n.x/=len;n.y/=len;n.z/=len;}
            for(const XMFLOAT3& p:{a,b,c}) {
                prim.vertices.insert(prim.vertices.end(), {p.x,p.y,p.z,n.x,n.y,n.z,
                    (p.x-minX)/(maxX-minX),(p.y-wallTop)/(ridgeY-wallTop),1,0,0,1});
                prim.indices.push_back((UINT)prim.indices.size());
            }
        };
        const XMFLOAT3 a0(minX,wallTop,nearZ), b0(maxX,wallTop,nearZ), c0(midX,ridgeY,nearZ);
        const XMFLOAT3 a1(minX,wallTop,farZ),  b1(maxX,wallTop,farZ),  c1(midX,ridgeY,farZ);
        tri(a0,b0,c0); tri(a1,c1,b1);                 // triangular faces
        tri(a0,a1,b1); tri(a0,b1,b0);                 // bottom
        tri(a0,c0,c1); tri(a0,c1,a1);                 // left roof edge
        tri(b0,b1,c1); tri(b0,c1,c0);                 // right roof edge
        node->mesh->primitives.push_back(std::move(prim));
        root->AddChild(node);
    };
    constexpr float gableThickness = 0.08f;
    addWoodGable(maxZ, maxZ + gableThickness);         // front triangle
    addWoodGable(minZ - gableThickness, minZ);         // back triangle

    const float halfW = midX - minX;              // horizontal run of one slope
    const float slopeLen = std::sqrt(halfW * halfW + (ridgeY - wallTop) * (ridgeY - wallTop));
    const float pitch = std::atan2(ridgeY - wallTop, halfW);  // slope angle
    constexpr int sheetsUp = 4;                   // panels up the slope
    constexpr int sheetsZ = 3;                    // panels along the roof depth
    constexpr float sheetT = 0.05f;               // thin metal sheet
    constexpr float overhang = 0.25f;             // panels jut past eave & gable
    constexpr float lapUp = 0.12f;                // each course laps onto the one below
    const float zLo = minZ - overhang, zHi = maxZ + overhang;
    const float zSpan = zHi - zLo;
    const float runStep = slopeLen / sheetsUp;
    // Build one slope: mirrorX flips it to the far side of the ridge. Panels are
    // authored in local space (x = up-slope run from the eave, y = thickness,
    // z = depth) then rotated by the pitch and moved onto the eave line.
    auto buildSlope = [&](bool mirror) {
        const float eaveX = mirror ? maxX : minX;
        // Local +x is "up the slope". Left slope rotates +pitch so +x runs
        // up-and-right to the ridge; the right slope uses (pi - pitch) so +x
        // runs up-and-LEFT to the same ridge. Z is untouched, so the panel's
        // depth stays axis-aligned.
        const XMMATRIX rot = XMMatrixRotationZ(mirror ? (3.14159265f - pitch) : pitch);
        const XMMATRIX place = rot * XMMatrixTranslation(eaveX, wallTop, 0.0f);
        for (int su = 0; su < sheetsUp; ++su) {
            const float run0 = su * runStep - overhang;              // start below eave
            const float run1 = (su + 1) * runStep + lapUp;           // lap onto next course
            for (int zi = 0; zi < sheetsZ; ++zi) {
                const float zr0 = zLo + zSpan * zi / sheetsZ;
                const float zr1 = zLo + zSpan * (zi + 1) / sheetsZ;
                const int id = pieceId++;
                addVoronoiBoard(("Roof@" + std::to_string(id)).c_str(), matMetalRoof,
                                run0, run1, 0.0f, sheetT, zr0, zr1, id, false, &place);
            }
        }
    };
    buildSlope(false);   // left slope (eave at minX)
    buildSlope(true);    // right slope (eave at maxX)

    // --- Second destructible shack from the Corrugated Metal Pack textures.
    // It lives next to the wooden house but is still part of the same Blast
    // asset, so bullets/grenades hit both buildings with one physics system.
    const float sx0 = 2.0f, sx1 = 7.5f;
    const float sz0 = 1.2f, sz1 = 5.9f;
    const float sy0 = floorY;
    const float slabTop = sy0 + 0.20f;
    // Relative to the slab, not absolute: an absolute eave height would leave the
    // metal shack behind at the old ground level when the pad moves.
    const float eaveY = sy0 + 2.85f;
    const float metalT = 0.06f;
    const float panelW = 0.42f;
    addBox("Support:MetalFoundation", matFoundation, sx0, sx1,
        sy0 - foundationEmbed, slabTop, sz0, sz1);
    auto addMetalPanel = [&](float x0, float x1, float y0, float y1, float z0, float z1) {
        const int id = pieceId++;
        addVoronoiBoard(("MetalWall@" + std::to_string(id)).c_str(), matMetalWall,
                        x0, x1, y0, y1, z0, z1, id);
    };
    auto addDoorPanel = [&](float x0, float x1, float y0, float y1, float z0, float z1) {
        const int id = pieceId++;
        addVoronoiBoard(("DarkMetal@" + std::to_string(id)).c_str(), matDarkMetal,
                        x0, x1, y0, y1, z0, z1, id);
    };
    auto metalWallX = [&](float z, bool front) {
        const float outer0 = front ? z : z - metalT;
        const float outer1 = front ? z + metalT : z;
        for (float x = sx0; x < sx1 - 0.01f; x += panelW) {
            const float nx = (std::min)(x + panelW, sx1);
            const float c0 = x - sx0, c1 = nx - sx0;
            const bool door = front && c1 > 2.00f && c0 < 3.35f;
            if (door) {
                if (c0 < 2.00f) addMetalPanel(x, sx0 + 2.00f, slabTop, eaveY, outer0, outer1);
                if (c1 > 3.35f) addMetalPanel(sx0 + 3.35f, nx, slabTop, eaveY, outer0, outer1);
                // Door header, measured up from the slab -- an absolute height here
                // would leave the doorway behind when the pad's ground level moves.
                addMetalPanel((std::max)(x, sx0 + 2.00f), (std::min)(nx, sx0 + 3.35f),
                              sy0 + 2.15f, eaveY, outer0, outer1);
            } else {
                addMetalPanel(x, nx, slabTop, eaveY, outer0, outer1);
            }
        }
    };
    auto metalWallZ = [&](float x, bool right) {
        const float outer0 = right ? x : x - metalT;
        const float outer1 = right ? x + metalT : x;
        for (float z = sz0; z < sz1 - 0.01f; z += panelW) {
            const float nz = (std::min)(z + panelW, sz1);
            addMetalPanel(outer0, outer1, slabTop, eaveY, z, nz);
        }
    };
    metalWallX(sz1, true);
    metalWallX(sz0, false);
    metalWallZ(sx0, false);
    metalWallZ(sx1, true);
    const float doorZ0 = sz1 + 0.012f;
    const float doorZ1 = sz1 + metalT + 0.012f;
    addDoorPanel(sx0 + 2.02f, sx0 + 2.64f, slabTop, 2.15f, doorZ0, doorZ1);
    addDoorPanel(sx0 + 2.71f, sx0 + 3.33f, slabTop, 2.15f, doorZ0, doorZ1);

    const float shackMidX = (sx0 + sx1) * 0.5f;
    const float shackRidgeY = eaveY + 0.85f;
    const float shackHalfW = shackMidX - sx0;
    const float shackSlopeLen = std::sqrt(shackHalfW * shackHalfW + (shackRidgeY - eaveY) * (shackRidgeY - eaveY));
    const float shackPitch = std::atan2(shackRidgeY - eaveY, shackHalfW);
    const float shackZLo = sz0 - 0.28f, shackZHi = sz1 + 0.28f;
    const float roofStep = shackSlopeLen / 3.0f;
    auto addGable = [&](float z, bool front) {
        const float outer0 = front ? z : z - metalT;
        const float outer1 = front ? z + metalT : z;
        for (float x = sx0; x < sx1 - 0.01f; x += panelW) {
            const float nx = (std::min)(x + panelW, sx1);
            const float cx = (x + nx) * 0.5f;
            const float t = 1.0f - std::min(1.0f, std::abs(cx - shackMidX) / shackHalfW);
            const float top = eaveY + (shackRidgeY - eaveY) * t;
            if (top > eaveY + 0.10f) addMetalPanel(x, nx, eaveY, top, outer0, outer1);
        }
    };
    addGable(sz1, true);
    addGable(sz0, false);

    constexpr float trimT = 0.085f;
    addBox("MetalTrim@CornerFL", matTrim, sx0 - trimT, sx0 + trimT, slabTop, eaveY, sz1 - trimT, sz1 + trimT);
    addBox("MetalTrim@CornerFR", matTrim, sx1 - trimT, sx1 + trimT, slabTop, eaveY, sz1 - trimT, sz1 + trimT);
    addBox("MetalTrim@CornerBL", matTrim, sx0 - trimT, sx0 + trimT, slabTop, eaveY, sz0 - trimT, sz0 + trimT);
    addBox("MetalTrim@CornerBR", matTrim, sx1 - trimT, sx1 + trimT, slabTop, eaveY, sz0 - trimT, sz0 + trimT);
    addBox("MetalTrim@DoorL", matTrim, sx0 + 1.92f, sx0 + 2.02f, slabTop, 2.28f, sz1 + metalT, sz1 + metalT + 0.08f);
    addBox("MetalTrim@DoorR", matTrim, sx0 + 3.33f, sx0 + 3.43f, slabTop, 2.28f, sz1 + metalT, sz1 + metalT + 0.08f);
    addBox("MetalTrim@DoorTop", matTrim, sx0 + 1.92f, sx0 + 3.43f, 2.15f, 2.28f, sz1 + metalT, sz1 + metalT + 0.08f);
    addBox("MetalTrim@DoorSplit", matTrim, sx0 + 2.66f, sx0 + 2.72f, slabTop, 2.15f, sz1 + metalT + 0.01f, sz1 + metalT + 0.09f);
    addBox("MetalTrim@RidgeCap", matTrim, shackMidX - 0.09f, shackMidX + 0.09f, shackRidgeY - 0.05f, shackRidgeY + 0.08f, shackZLo, shackZHi);
    addBox("MetalTrim@LeftEave", matTrim, sx0 - 0.38f, sx0 - 0.20f, eaveY - 0.13f, eaveY + 0.03f, shackZLo, shackZHi);
    addBox("MetalTrim@RightEave", matTrim, sx1 + 0.20f, sx1 + 0.38f, eaveY - 0.13f, eaveY + 0.03f, shackZLo, shackZHi);
    addBox("MetalTrim@FrontFascia", matTrim, sx0 - 0.28f, sx1 + 0.28f, eaveY - 0.10f, eaveY + 0.05f, sz1 + 0.19f, sz1 + 0.31f);
    addBox("MetalTrim@BackFascia", matTrim, sx0 - 0.28f, sx1 + 0.28f, eaveY - 0.10f, eaveY + 0.05f, sz0 - 0.31f, sz0 - 0.19f);
    for (float x = sx0 + 0.25f; x < sx1 - 0.2f; x += 0.84f) {
        addBox("DarkMetal@ScrewFront", matDarkMetal, x, x + 0.055f, eaveY - 0.30f, eaveY - 0.23f, sz1 + metalT + 0.015f, sz1 + metalT + 0.04f);
        addBox("DarkMetal@ScrewFront", matDarkMetal, x, x + 0.055f, slabTop + 0.55f, slabTop + 0.62f, sz1 + metalT + 0.015f, sz1 + metalT + 0.04f);
    }

    auto buildMetalSlope = [&](bool mirror) {
        const float eaveX = mirror ? sx1 : sx0;
        const XMMATRIX rot = XMMatrixRotationZ(mirror ? (3.14159265f - shackPitch) : shackPitch);
        const XMMATRIX place = rot * XMMatrixTranslation(eaveX, eaveY, 0.0f);
        for (int up = 0; up < 3; ++up) {
            for (int zi = 0; zi < 3; ++zi) {
                const float zr0 = shackZLo + (shackZHi - shackZLo) * zi / 3.0f;
                const float zr1 = shackZLo + (shackZHi - shackZLo) * (zi + 1) / 3.0f;
                const int id = pieceId++;
                addVoronoiBoard(("MetalRoof@" + std::to_string(id)).c_str(), matMetalRoof,
                                up * roofStep - 0.22f, (up + 1) * roofStep + 0.12f,
                                0.0f, metalT, zr0, zr1, id, false, &place);
            }
        }
    };
    buildMetalSlope(false);
    buildMetalSlope(true);

    root->UpdateGlobalTransform(root->localTransform);
    return root;
}

// Reuse the authored wooden and metal house chunks as independent compounds.
// Vertex transforms are baked because the
// destruction system consumes child geometry directly rather than node poses.
static std::shared_ptr<SceneNode> CloneSceneTree(
    const std::shared_ptr<SceneNode>& source) {
    if (!source) return {};
    auto clone = std::make_shared<SceneNode>(source->name);
    clone->translation = source->translation;
    clone->rotation = source->rotation;
    clone->scale = source->scale;
    if (source->mesh)
        clone->mesh = std::make_shared<SceneMesh>(*source->mesh);
    for (const auto& child : source->children)
        clone->AddChild(CloneSceneTree(child));
    return clone;
}

static void ArrangeHousesInCross(const std::shared_ptr<SceneNode>& root,
                                 bool stressTest) {
    if (!root) return;
    std::vector<std::shared_ptr<SceneNode>> woodTemplate;
    std::vector<std::shared_ptr<SceneNode>> metalTemplate;

    auto meanX = [](const std::shared_ptr<SceneNode>& node) {
        double sum = 0.0;
        size_t count = 0;
        if (node && node->mesh) {
            for (const MeshPrimitive& primitive : node->mesh->primitives) {
                for (size_t v = 0; v + 11 < primitive.vertices.size(); v += 12) {
                    sum += primitive.vertices[v];
                    ++count;
                }
            }
        }
        return count ? static_cast<float>(sum / static_cast<double>(count)) : 0.0f;
    };

    // Original templates sit side-by-side: wood is left of x=1, metal right.
    for (const auto& child : root->children) {
        if (!child || !child->mesh) continue;
        (meanX(child) < 1.0f ? woodTemplate : metalTemplate).push_back(child);
    }
    root->children.clear();

    auto uniqueName = [](const std::string& source, int groupOffset, size_t ordinal) {
        const size_t at = source.rfind('@');
        if (at == std::string::npos)
            return source + "#" + std::to_string(groupOffset);
        bool numeric = at + 1 < source.size();
        for (size_t i = at + 1; i < source.size(); ++i)
            numeric = numeric && source[i] >= '0' && source[i] <= '9';
        if (numeric) {
            const int oldId = std::atoi(source.c_str() + at + 1);
            return source.substr(0, at + 1) + std::to_string(oldId + groupOffset);
        }
        return source + "@" + std::to_string(groupOffset + 500000 + ordinal);
    };

    auto addHouse = [&](const std::vector<std::shared_ptr<SceneNode>>& source,
                        float sourceX, float sourceZ, float targetX, float targetY,
                        float targetZ,
                        float yaw, int groupOffset) {
        const XMMATRIX transform =
            XMMatrixTranslation(-sourceX, 0.0f, -sourceZ) *
            XMMatrixRotationY(yaw) *
            XMMatrixTranslation(targetX, targetY, targetZ);
        const XMMATRIX rotation = XMMatrixRotationY(yaw);
        for (size_t childIndex = 0; childIndex < source.size(); ++childIndex) {
            const auto& sourceChild = source[childIndex];
            auto child = std::make_shared<SceneNode>(
                uniqueName(sourceChild->name, groupOffset, childIndex));
            child->mesh = std::make_shared<SceneMesh>();
            for (const MeshPrimitive& sourcePrimitive : sourceChild->mesh->primitives) {
                MeshPrimitive primitive;
                primitive.vertices = sourcePrimitive.vertices;
                primitive.indices = sourcePrimitive.indices;
                primitive.materialIndex = sourcePrimitive.materialIndex;
                primitive.material = sourcePrimitive.material;
                for (size_t v = 0; v + 11 < primitive.vertices.size(); v += 12) {
                    XMVECTOR p = XMVectorSet(primitive.vertices[v],
                        primitive.vertices[v + 1], primitive.vertices[v + 2], 1.0f);
                    XMVECTOR n = XMVectorSet(primitive.vertices[v + 3],
                        primitive.vertices[v + 4], primitive.vertices[v + 5], 0.0f);
                    XMVECTOR t = XMVectorSet(primitive.vertices[v + 8],
                        primitive.vertices[v + 9], primitive.vertices[v + 10], 0.0f);
                    p = XMVector3TransformCoord(p, transform);
                    n = XMVector3Normalize(XMVector3TransformNormal(n, rotation));
                    t = XMVector3Normalize(XMVector3TransformNormal(t, rotation));
                    XMFLOAT3 pf, nf, tf;
                    XMStoreFloat3(&pf, p); XMStoreFloat3(&nf, n); XMStoreFloat3(&tf, t);
                    primitive.vertices[v] = pf.x;
                    primitive.vertices[v + 1] = pf.y;
                    primitive.vertices[v + 2] = pf.z;
                    primitive.vertices[v + 3] = nf.x;
                    primitive.vertices[v + 4] = nf.y;
                    primitive.vertices[v + 5] = nf.z;
                    primitive.vertices[v + 8] = tf.x;
                    primitive.vertices[v + 9] = tf.y;
                    primitive.vertices[v + 10] = tf.z;
                }
                child->mesh->primitives.push_back(std::move(primitive));
            }
            root->AddChild(child);
        }
    };

    if (g_customLevelMode) {
        int groupOffset = 1000000;
        for (const LevelEntity& entity : g_runtimeLevel.entities) {
            if (!entity.enabled ||
                (entity.type != LevelEntityType::WoodHouse &&
                 entity.type != LevelEntityType::MetalHouse)) continue;
            const bool wood = entity.type == LevelEntityType::WoodHouse;
            addHouse(wood ? woodTemplate : metalTemplate,
                wood ? -3.5f : 4.75f, wood ? 3.5f : 3.55f,
                entity.transform.position[0], entity.transform.position[1],
                entity.transform.position[2],
                XMConvertToRadians(entity.transform.rotation[1]), groupOffset);
            groupOffset += 1000000;
        }
        XMFLOAT4X4 identity;
        XMStoreFloat4x4(&identity, XMMatrixIdentity());
        root->UpdateGlobalTransform(identity);
        return;
    }

    constexpr float radius = 9.0f;
    const size_t houseCount = stressTest ? kStressHouseCount : 4;
    size_t houseIndex = 0;
    const size_t compoundCount = (houseCount + kSpawnersPerCompound - 1) /
        kSpawnersPerCompound;
    for (size_t compoundIndex = 0; compoundIndex < compoundCount; ++compoundIndex) {
        const CompoundCenter& center = kStressCompoundCenters[compoundIndex];
        const int groupBase = static_cast<int>(compoundIndex) * 4000000;
        if (houseIndex++ < houseCount)
            addHouse(woodTemplate, -3.5f, 3.5f,
                     center.x, 0.0f, center.z + radius, 0.0f, groupBase + 1000000);
        if (houseIndex++ < houseCount)
            addHouse(metalTemplate, 4.75f, 3.55f,
                     center.x + radius, 0.0f, center.z, XM_PIDIV2, groupBase + 2000000);
        if (houseIndex++ < houseCount)
            addHouse(woodTemplate, -3.5f, 3.5f,
                     center.x, 0.0f, center.z - radius, XM_PI, groupBase + 3000000);
        if (houseIndex++ < houseCount)
            addHouse(metalTemplate, 4.75f, 3.55f,
                     center.x - radius, 0.0f, center.z, -XM_PIDIV2, groupBase + 4000000);
    }

    XMFLOAT4X4 identity;
    XMStoreFloat4x4(&identity, XMMatrixIdentity());
    root->UpdateGlobalTransform(identity);
}

static void SynchronizeEditorRuntime(bool play) {
    if (gameScreen != GameScreen::LevelEditor) return;
    const bool foliageChanged = g_levelEditor.FoliageRuntimeDirty();
    const bool terrainChanged = g_levelEditor.TerrainRuntimeDirty();
    g_runtimeLevel = g_levelEditor.Level();
    g_runtimeTerrainSculpt = g_runtimeLevel.terrainSculpt;
    g_terrain.SetSculptStamps(g_runtimeTerrainSculpt);
    g_grass.ClearRuntimeExclusions();
    g_customLevelMode = true;
    ApplyRuntimeLevelBasics(play);
    if (!play && terrainChanged && !foliageChanged) {
        if (g_destruction.IsInitialized()) {
            auto tp = CurrentTerrainParams();
            tp.heightScale = scene.terrainHeightScale;
            g_destruction.SetTerrainSampler([tp](float x, float z) {
                return TerrainRendererDX12::HeightAt(tp, x, z);
            });
        }
        g_levelEditor.MarkRuntimeSynchronized();
        g_levelEditor.MarkTerrainRuntimeSynchronized();
        return;
    }
    if (g_houseTemplate) {
        WaitForGPU();
        wallModel = CloneSceneTree(g_houseTemplate);
        ArrangeHousesInCross(wallModel, false);
        if (wallModel && g_dx12.device)
            g_destruction.Initialize(wallModel, g_dx12.device.Get(), 1, 1, 1);
    }
    if (g_trees.IsInitialized()) ResetPalmTrees();
    if ((play || foliageChanged || terrainChanged) && g_environmentInitialized)
        RebuildScalableEnvironment();
    if (g_destruction.IsInitialized()) {
        auto tp = CurrentTerrainParams();
        tp.heightScale = scene.terrainHeightScale;
        g_destruction.SetTerrainSampler([tp](float x, float z) {
            return TerrainRendererDX12::HeightAt(tp, x, z);
        });
        if (FirstRuntimeEntity(LevelEntityType::Humvee))
            g_destruction.InitializeVehicle(g_primaryHumveeSpawn,
                XMConvertToRadians(g_primaryHumveeYaw));
    }
    g_levelEditor.MarkRuntimeSynchronized();
    g_levelEditor.MarkFoliageRuntimeSynchronized();
    g_levelEditor.MarkTerrainRuntimeSynchronized();
}

static void StartLevelEditor(HWND hwnd) {
    g_levelEditor.NewFromLevelOne();
    StartLevelOne(hwnd, true);
    gameScreen = GameScreen::LevelEditor;
    g_customLevelMode = true;
    g_runtimeLevel = g_levelEditor.Level();
    g_pendingEnvironmentRebuild = false;
    levelTimerRunning = false;
    showUI = false;
    cameraLocked = true;
    scene.camera.FPSMode = false;
    ReleaseCapture();
    SetCursorVisible(true);
}

static void BeginEditorPlaytest(HWND hwnd) {
    if (gameScreen != GameScreen::LevelEditor || g_levelEditor.IsPlaying()) return;
    g_editorCameraSnapshot = scene.camera;
    g_levelEditor.BeginPlay();
    scene.ResetLevelRuntimeState();
    SynchronizeEditorRuntime(true);
    scene.playerGodMode = true;
    scene.RestorePlayerHealth();
    pendingLevelRuntimeReset = true;
    levelTimerRunning = false;
    cameraLocked = false;
    scene.camera.FPSMode = true;
    SetCapture(hwnd);
    SetCursorVisible(false);
}

static void StopEditorPlaytest() {
    if (!IsEditorPlaying()) return;
    // Previous frame can still reference enemy vertex/index buffers. Drain GPU
    // before destroying Bandits or rebuilding editor destruction resources.
    WaitForGPU();
    g_levelEditor.StopPlay();
    g_heldBandit = nullptr;
    g_bandits.clear();
    scene.ResetLevelRuntimeState();
    pendingLevelRuntimeReset = false;
    pendingTurretGunnerRespawn = false;
    levelTimerRunning = false;
    scene.camera.FPSMode = false;
    scene.camera = g_editorCameraSnapshot;
    scene.camera.FPSMode = false;
    cameraLocked = true;
    ReleaseCapture();
    SetCursorVisible(true);
    SynchronizeEditorRuntime(false);
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

// ?? forward decls ????????????????????????????????????????????????????????????
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

// ?? geometry creation ????????????????????????????????????????????????????????
static bool CreateAllGeometry() {
    std::vector<VertexPosNormUV> cubeVerts = {
        // Front
        {{-0.5f,-0.5f, 0.5f},{ 0, 0, 1},{0,0}}, {{ 0.5f,-0.5f, 0.5f},{ 0, 0, 1},{1,0}},
        {{ 0.5f, 0.5f, 0.5f},{ 0, 0, 1},{1,1}}, {{-0.5f,-0.5f, 0.5f},{ 0, 0, 1},{0,0}},
        {{ 0.5f, 0.5f, 0.5f},{ 0, 0, 1},{1,1}}, {{-0.5f, 0.5f, 0.5f},{ 0, 0, 1},{0,1}},
        // Back
        {{ 0.5f,-0.5f,-0.5f},{ 0, 0,-1},{0,0}}, {{-0.5f,-0.5f,-0.5f},{ 0, 0,-1},{1,0}},
        {{-0.5f, 0.5f,-0.5f},{ 0, 0,-1},{1,1}}, {{ 0.5f,-0.5f,-0.5f},{ 0, 0,-1},{0,0}},
        {{-0.5f, 0.5f,-0.5f},{ 0, 0,-1},{1,1}}, {{ 0.5f, 0.5f,-0.5f},{ 0, 0,-1},{0,1}},
        // Top
        {{-0.5f, 0.5f, 0.5f},{ 0, 1, 0},{0,0}}, {{ 0.5f, 0.5f, 0.5f},{ 0, 1, 0},{1,0}},
        {{ 0.5f, 0.5f,-0.5f},{ 0, 1, 0},{1,1}}, {{-0.5f, 0.5f, 0.5f},{ 0, 1, 0},{0,0}},
        {{ 0.5f, 0.5f,-0.5f},{ 0, 1, 0},{1,1}}, {{-0.5f, 0.5f,-0.5f},{ 0, 1, 0},{0,1}},
        // Bottom
        {{-0.5f,-0.5f,-0.5f},{ 0,-1, 0},{0,0}}, {{ 0.5f,-0.5f,-0.5f},{ 0,-1, 0},{1,0}},
        {{ 0.5f,-0.5f, 0.5f},{ 0,-1, 0},{1,1}}, {{-0.5f,-0.5f,-0.5f},{ 0,-1, 0},{0,0}},
        {{ 0.5f,-0.5f, 0.5f},{ 0,-1, 0},{1,1}}, {{-0.5f,-0.5f, 0.5f},{ 0,-1, 0},{0,1}},
        // Right
        {{ 0.5f,-0.5f, 0.5f},{ 1, 0, 0},{0,0}}, {{ 0.5f,-0.5f,-0.5f},{ 1, 0, 0},{1,0}},
        {{ 0.5f, 0.5f,-0.5f},{ 1, 0, 0},{1,1}}, {{ 0.5f,-0.5f, 0.5f},{ 1, 0, 0},{0,0}},
        {{ 0.5f, 0.5f,-0.5f},{ 1, 0, 0},{1,1}}, {{ 0.5f, 0.5f, 0.5f},{ 1, 0, 0},{0,1}},
        // Left
        {{-0.5f,-0.5f,-0.5f},{-1, 0, 0},{0,0}}, {{-0.5f,-0.5f, 0.5f},{-1, 0, 0},{1,0}},
        {{-0.5f, 0.5f, 0.5f},{-1, 0, 0},{1,1}}, {{-0.5f,-0.5f,-0.5f},{-1, 0, 0},{0,0}},
        {{-0.5f, 0.5f, 0.5f},{-1, 0, 0},{1,1}}, {{-0.5f, 0.5f,-0.5f},{-1, 0, 0},{0,1}},
    };
    if (!CreateVertexBuffer(cubeVerts, geo.cubeVertexBuffer, geo.cubeVBV)) return false;

    float s = 20.0f;
    float tile = 8.0f;
    std::vector<VertexPosNormUV> planeVerts = {
        {{-s,0, s},{0,1,0},{0,0}}, {{ s,0, s},{0,1,0},{tile,0}}, {{ s,0,-s},{0,1,0},{tile,tile}},
        {{-s,0, s},{0,1,0},{0,0}}, {{ s,0,-s},{0,1,0},{tile,tile}}, {{-s,0,-s},{0,1,0},{0,tile}},
    };
    if (!CreateVertexBuffer(planeVerts, geo.planeVertexBuffer, geo.planeVBV)) return false;

    // Unit sphere (radius 0.5) for projectiles / debug spheres.
    std::vector<VertexPosNormUV> sphereVerts = BuildSphereVertices();
    if (!CreateVertexBuffer(sphereVerts, geo.sphereVertexBuffer, geo.sphereVBV)) return false;

    std::vector<VertexPosNormUV> capsuleVerts = BuildCapsuleVertices();
    if (!CreateVertexBuffer(capsuleVerts, geo.capsuleVertexBuffer, geo.capsuleVBV)) return false;

    // Unit XY quad (-0.5..0.5) with UV 0..1 for camera-facing smoke billboards.
    std::vector<VertexPosNormUV> quadVerts = {
        {{-0.5f,-0.5f,0},{0,0,1},{0,1}}, {{ 0.5f,-0.5f,0},{0,0,1},{1,1}}, {{ 0.5f, 0.5f,0},{0,0,1},{1,0}},
        {{-0.5f,-0.5f,0},{0,0,1},{0,1}}, {{ 0.5f, 0.5f,0},{0,0,1},{1,0}}, {{-0.5f, 0.5f,0},{0,0,1},{0,0}},
    };
    if (!CreateVertexBuffer(quadVerts, geo.quadVertexBuffer, geo.quadVBV)) return false;

    // OpenGameArt sheet contains four 128x128 frames across one row. Sample the
    // first cell; additive blending removes its conventional black background.
    std::vector<VertexPosNormUV> flashVerts = {
        {{-0.5f,-0.5f,0},{0,0,1},{0.00f,1}}, {{ 0.5f,-0.5f,0},{0,0,1},{0.25f,1}}, {{ 0.5f, 0.5f,0},{0,0,1},{0.25f,0}},
        {{-0.5f,-0.5f,0},{0,0,1},{0.00f,1}}, {{ 0.5f, 0.5f,0},{0,0,1},{0.25f,0}}, {{-0.5f, 0.5f,0},{0,0,1},{0.00f,0}},
    };
    if (!CreateVertexBuffer(flashVerts, geo.flashVertexBuffer, geo.flashVBV)) return false;

    // OpenGameArt CC0 fire sheet: 10 columns x 6 rows, 60 frames at 64 px.
    std::vector<VertexPosNormUV> fireVerts;
    fireVerts.reserve(60 * 6);
    for (int frame = 0; frame < 60; ++frame) {
        const int column = frame % 10;
        const int row = frame / 10;
        const float u0 = column / 10.0f, u1 = (column + 1) / 10.0f;
        const float v0 = row / 6.0f, v1 = (row + 1) / 6.0f;
        fireVerts.insert(fireVerts.end(), {
            {{-0.5f,-0.5f,0},{0,0,1},{u0,v1}}, {{ 0.5f,-0.5f,0},{0,0,1},{u1,v1}}, {{ 0.5f, 0.5f,0},{0,0,1},{u1,v0}},
            {{-0.5f,-0.5f,0},{0,0,1},{u0,v1}}, {{ 0.5f, 0.5f,0},{0,0,1},{u1,v0}}, {{-0.5f, 0.5f,0},{0,0,1},{u0,v0}},
        });
    }
    if (!CreateVertexBuffer(fireVerts, geo.fireVertexBuffer, geo.fireVBV)) return false;

    // OpenGameArt CC0 explosion sheet: 4 columns x 4 rows, 16 frames at 128 px.
    std::vector<VertexPosNormUV> explosionVerts;
    explosionVerts.reserve(16 * 6);
    for (int frame = 0; frame < 16; ++frame) {
        const int column = frame % 4;
        const int row = frame / 4;
        const float u0 = column / 4.0f, u1 = (column + 1) / 4.0f;
        const float v0 = row / 4.0f, v1 = (row + 1) / 4.0f;
        explosionVerts.insert(explosionVerts.end(), {
            {{-0.5f,-0.5f,0},{0,0,1},{u0,v1}}, {{ 0.5f,-0.5f,0},{0,0,1},{u1,v1}}, {{ 0.5f, 0.5f,0},{0,0,1},{u1,v0}},
            {{-0.5f,-0.5f,0},{0,0,1},{u0,v1}}, {{ 0.5f, 0.5f,0},{0,0,1},{u1,v0}}, {{-0.5f, 0.5f,0},{0,0,1},{u0,v0}},
        });
    }
    if (!CreateVertexBuffer(explosionVerts, geo.explosionVertexBuffer, geo.explosionVBV)) return false;
    geo.sphereVertexCount = (UINT)sphereVerts.size();
    geo.capsuleVertexCount = (UINT)capsuleVerts.size();

    BuildPackedGeometry(cubeVerts, planeVerts, packed);
    return true;
}

// ?? fullscreen toggle ????????????????????????????????????????????????????????
static void ToggleFullscreen(HWND hwnd) {
    if (!isFullscreen) {
        GetWindowRect(hwnd, &windowedRect);
        windowedStyle = GetWindowLong(hwnd, GWL_STYLE);
        HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfo(hMon, &mi);
        SetWindowLong(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(hwnd, HWND_TOP,
            mi.rcMonitor.left, mi.rcMonitor.top,
            mi.rcMonitor.right - mi.rcMonitor.left,
            mi.rcMonitor.bottom - mi.rcMonitor.top,
            SWP_FRAMECHANGED | SWP_NOCOPYBITS);
        isFullscreen = true;
    } else {
        SetWindowLong(hwnd, GWL_STYLE, windowedStyle);
        SetWindowPos(hwnd, HWND_NOTOPMOST,
            windowedRect.left, windowedRect.top,
            windowedRect.right - windowedRect.left,
            windowedRect.bottom - windowedRect.top,
            SWP_FRAMECHANGED | SWP_NOCOPYBITS);
        isFullscreen = false;
    }
}

// ?? input ????????????????????????????????????????????????????????????????????
// On-screen movement pad. Buttons in the UI set these each frame; ProcessInput
// drains them. Kept separate from the keyboard path so the pad still works while
// the camera is locked (which is exactly when the mouse is free to click it).
VirtualInput virtualInput;

static void ToggleHumveeDriving() {
    XMFLOAT4X4 pose;
    XMFLOAT3 position, forward;
    if (!g_destruction.GetVehicleTransform(pose, &position, &forward)) return;

    if (!g_drivingHumvee) {
        const XMVECTOR offset = XMLoadFloat3(&scene.camera.Position) -
                               XMLoadFloat3(&position);
        if (XMVectorGetX(XMVector3LengthSq(offset)) > 25.0f) return;
        g_drivingHumvee = true;
        g_savedGunVisible = scene.gun.visible;
        scene.gun.visible = false;
        scene.camera.FPSMode = false;
        scene.camera.VerticalVelocity = 0.0f;
        return;
    }

    g_drivingHumvee = false;
    scene.gun.visible = g_savedGunVisible;
    scene.camera.FPSMode = true;
    const XMVECTOR forwardVector = XMLoadFloat3(&forward);
    const XMVECTOR right = XMVector3Normalize(XMVector3Cross(
        forwardVector, XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f)));
    XMVECTOR exitPosition = XMLoadFloat3(&position) + right * 2.3f;
    exitPosition = XMVectorSetY(exitPosition, XMVectorGetY(exitPosition) + 1.3f);
    XMStoreFloat3(&scene.camera.Position, exitPosition);
}

static void UpdateHumveeChaseCamera(float dt) {
    if (!g_drivingHumvee) return;
    XMFLOAT4X4 pose;
    XMFLOAT3 position, forward;
    if (!g_destruction.GetVehicleTransform(pose, &position, &forward)) return;

    const XMVECTOR target = XMLoadFloat3(&position) +
        XMVectorSet(0.0f, 1.65f, 0.0f, 0.0f);
    // Camera Front comes from mouse look. Orbit around vehicle using that view
    // direction instead of forcing camera behind chassis heading every frame.
    const XMVECTOR orbitView = XMVector3Normalize(XMLoadFloat3(&scene.camera.Front));
    const XMVECTOR desired = target - orbitView * 6.5f;
    const float follow = 1.0f - std::exp(-8.0f * (std::max)(0.0f, dt));
    const XMVECTOR cameraPosition = XMVectorLerp(
        XMLoadFloat3(&scene.camera.Position), desired, follow);
    XMStoreFloat3(&scene.camera.Position, cameraPosition);

    XMStoreFloat3(&scene.camera.Front,
        XMVector3Normalize(target - cameraPosition));
    scene.camera.Up = { 0.0f, 1.0f, 0.0f };
}

static XMFLOAT3 HumveeScreenCenterAimPoint() {
    const XMFLOAT3 origin = scene.camera.Position;
    const XMFLOAT3 end = {
        origin.x + scene.camera.Front.x * 140.0f,
        origin.y + scene.camera.Front.y * 140.0f,
        origin.z + scene.camera.Front.z * 140.0f };
    XMFLOAT3 closest = end;
    float closestDistanceSq = FLT_MAX;
    auto accept = [&](const XMFLOAT3& hit) {
        const float dx = hit.x - origin.x;
        const float dy = hit.y - origin.y;
        const float dz = hit.z - origin.z;
        const float distanceSq = dx * dx + dy * dy + dz * dz;
        if (distanceSq < closestDistanceSq) {
            closestDistanceSq = distanceSq;
            closest = hit;
        }
    };
    XMFLOAT3 hit;
    if (scene.useDestruction && g_destruction.IsInitialized() &&
        g_destruction.HitTestSegment(origin, end, 0.03f, hit)) accept(hit);
    if (HitTerrainSegment(origin, end, 0.03f, hit)) accept(hit);
    return closest;
}

static void UpdateHumveeTurretAim(float dt) {
    if (!g_humveeModel || !g_humveeTurretNode) return;
    g_humveeAimPoint = g_drivingHumvee
        ? HumveeScreenCenterAimPoint()
        : scene.camera.Position;
    const XMMATRIX modelWorld = HumveeWorldMatrix();
    const XMMATRIX inverseModel = XMMatrixInverse(nullptr, modelWorld);
    XMFLOAT3 localTarget;
    XMStoreFloat3(&localTarget, XMVector3TransformCoord(
        XMLoadFloat3(&g_humveeAimPoint), inverseModel));
    const float dx = localTarget.x - g_humveeTurretNode->translation.x;
    const float dz = localTarget.z - g_humveeTurretNode->translation.z;
    if (dx * dx + dz * dz < 0.001f) return;
    const float desiredYaw = std::atan2(dx, dz);
    const float yawDelta = std::atan2(
        std::sin(desiredYaw - g_humveeTurretYaw),
        std::cos(desiredYaw - g_humveeTurretYaw));
    const float maxTraverse = 1.35f * (std::max)(0.0f, dt);
    g_humveeTurretYaw += (std::max)(-maxTraverse,
        (std::min)(maxTraverse, yawDelta));
    XMStoreFloat4(&g_humveeTurretNode->rotation,
        XMQuaternionRotationAxis(XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f),
                                 g_humveeTurretYaw));
    XMFLOAT4X4 identity;
    XMStoreFloat4x4(&identity, XMMatrixIdentity());
    g_humveeModel->UpdateGlobalTransform(identity);
}

static void FireHumveeTurret() {
    if (!g_drivingHumvee || !g_humveeTurretNode ||
        g_humveeTurretFireCooldown > 0.0f) return;
    g_humveeAimPoint = HumveeScreenCenterAimPoint();
    const XMMATRIX turretWorld =
        XMLoadFloat4x4(&g_humveeTurretNode->globalTransform) *
        HumveeWorldMatrix();
    XMFLOAT3 muzzle;
    XMStoreFloat3(&muzzle, XMVector3TransformCoord(
        XMVectorSet(0.0f, 72.0f, 338.0f, 1.0f), turretWorld));
    XMVECTOR direction = XMLoadFloat3(&g_humveeAimPoint) - XMLoadFloat3(&muzzle);
    if (XMVectorGetX(XMVector3LengthSq(direction)) < 0.01f) return;
    XMFLOAT3 shotDirection;
    XMStoreFloat3(&shotDirection, XMVector3Normalize(direction));
    scene.SpawnPlayerProjectile(muzzle, shotDirection, 1.35f);
    scene.SpawnSmokeBurst(muzzle, 0.08f, 0.10f);
    g_gunAudio.Play(0.72f, 0.90f + ((float)std::rand() / RAND_MAX) * 0.06f);
    g_humveeTurretFireCooldown = 0.12f;
}

static void UpdateHumveeImpacts(float dt) {
    XMFLOAT4X4 pose;
    XMFLOAT3 position, forward, velocity;
    if (!g_destruction.GetVehicleTransform(
            pose, &position, &forward, &velocity)) return;

    g_humveeHouseImpactCooldown =
        (std::max)(0.0f, g_humveeHouseImpactCooldown - dt);
    const float speed = std::sqrt(
        velocity.x * velocity.x + velocity.y * velocity.y + velocity.z * velocity.z);
    if (!g_previousHumveePositionValid) {
        g_previousHumveePosition = position;
        g_previousHumveePositionValid = true;
        return;
    }
    const float travelX = position.x - g_previousHumveePosition.x;
    const float travelY = position.y - g_previousHumveePosition.y;
    const float travelZ = position.z - g_previousHumveePosition.z;
    if (travelX * travelX + travelY * travelY + travelZ * travelZ > 100.0f) {
        g_previousHumveePosition = position;
        return;
    }

    if (speed >= 3.5f && g_banditLoaded) {
        XMFLOAT3 worldMin(FLT_MAX, FLT_MAX, FLT_MAX);
        XMFLOAT3 worldMax(-FLT_MAX, -FLT_MAX, -FLT_MAX);
        const XMMATRIX world = XMLoadFloat4x4(&pose);
        for (float x : { -2.25f, 2.25f })
        for (float y : { -0.7f, 0.7f })
        for (float z : { -1.05f, 1.05f }) {
            XMFLOAT3 corner;
            XMStoreFloat3(&corner, XMVector3TransformCoord(
                XMVectorSet(x, y, z, 1.0f), world));
            worldMin.x = (std::min)(worldMin.x, corner.x);
            worldMin.y = (std::min)(worldMin.y, corner.y);
            worldMin.z = (std::min)(worldMin.z, corner.z);
            worldMax.x = (std::max)(worldMax.x, corner.x);
            worldMax.y = (std::max)(worldMax.y, corner.y);
            worldMax.z = (std::max)(worldMax.z, corner.z);
        }
        const DestructionDebrisHazard vehicleImpact = {
            worldMin, worldMax, position, velocity, 1200.0f, speed >= 7.0f };
        for (auto& bandit : g_bandits) {
            if (!bandit || bandit->Dead() || bandit->turretGunner) continue;
            XMFLOAT3 impact;
            if (!bandit->ApplyDebrisImpact(vehicleImpact, &impact)) continue;
            XMFLOAT3 normal;
            XMStoreFloat3(&normal, XMVector3Normalize(-XMLoadFloat3(&velocity)));
            scene.SpawnBloodBurst(impact, normal);
            g_hitAudio.Play(0.34f, 0.88f + ((float)std::rand() / RAND_MAX) * 0.18f);
        }
    }

    if (speed >= 4.5f && g_humveeHouseImpactCooldown <= 0.0f) {
        XMFLOAT3 impactDirection = { velocity.x, 0.0f, velocity.z };
        XMVECTOR impactVector = XMLoadFloat3(&impactDirection);
        if (XMVectorGetX(XMVector3LengthSq(impactVector)) < 0.01f)
            impactVector = XMLoadFloat3(&forward);
        XMStoreFloat3(&impactDirection, XMVector3Normalize(impactVector));
        const XMFLOAT3 start = {
            g_previousHumveePosition.x + impactDirection.x * 2.1f,
            g_previousHumveePosition.y,
            g_previousHumveePosition.z + impactDirection.z * 2.1f };
        const XMFLOAT3 end = {
            position.x + impactDirection.x * 2.65f,
            position.y,
            position.z + impactDirection.z * 2.65f };
        XMFLOAT3 hit;
        if (g_destruction.HitTestSegment(start, end, 0.75f, hit)) {
            const float impactStrength = (std::min)(320.0f, 80.0f + speed * 24.0f);
            g_destruction.ApplyExplosion(
                hit, (std::min)(3.2f, 1.4f + speed * 0.16f),
                impactStrength, impactStrength);
            scene.SpawnSmokeBurst(hit, 0.65f, 0.7f);
            g_humveeHouseImpactCooldown = 0.4f;
        }
    }
    g_previousHumveePosition = position;
}

static bool ResolveBanditHumveeCollision(SkinnedEnemy& bandit) {
    if (bandit.Dead() || bandit.Held() || bandit.turretGunner) return false;
    XMFLOAT4X4 pose;
    if (!g_destruction.GetVehicleTransform(pose)) return false;

    const XMMATRIX vehicleWorld = XMLoadFloat4x4(&pose);
    const XMMATRIX vehicleLocal = XMMatrixInverse(nullptr, vehicleWorld);
    const XMVECTOR bodyCenter = XMVectorSet(
        bandit.position.x,
        bandit.position.y + bandit.footOffset + 1.0f,
        bandit.position.z, 1.0f);
    XMFLOAT3 local;
    XMStoreFloat3(&local, XMVector3TransformCoord(bodyCenter, vehicleLocal));

    constexpr float enemyRadius = 0.58f;
    constexpr float halfX = 2.25f + enemyRadius;
    constexpr float halfZ = 1.05f + enemyRadius;
    if (local.y < -0.85f || local.y > 1.15f ||
        std::abs(local.x) >= halfX || std::abs(local.z) >= halfZ)
        return false;

    const float penetrationX = halfX - std::abs(local.x);
    const float penetrationZ = halfZ - std::abs(local.z);
    if (penetrationX < penetrationZ)
        local.x = std::copysign(halfX + 0.02f,
            std::abs(local.x) > 0.001f ? local.x : 1.0f);
    else
        local.z = std::copysign(halfZ + 0.02f,
            std::abs(local.z) > 0.001f ? local.z : 1.0f);

    XMFLOAT3 corrected;
    XMStoreFloat3(&corrected, XMVector3TransformCoord(
        XMLoadFloat3(&local), vehicleWorld));
    bandit.position.x += corrected.x - XMVectorGetX(bodyCenter);
    bandit.position.z += corrected.z - XMVectorGetZ(bodyCenter);
    return true;
}

static void ApplyVirtualInput() {
    Camera& cam = scene.camera;
    if (virtualInput.moveY > 0.0f) cam.ProcessKeyboard('W', deltaTime,  virtualInput.moveY);
    if (virtualInput.moveY < 0.0f) cam.ProcessKeyboard('S', deltaTime, -virtualInput.moveY);
    if (virtualInput.moveX < 0.0f) cam.ProcessKeyboard('A', deltaTime, -virtualInput.moveX);
    if (virtualInput.moveX > 0.0f) cam.ProcessKeyboard('D', deltaTime,  virtualInput.moveX);
    if (virtualInput.down)    cam.ProcessKeyboard('Q', deltaTime);
    if (virtualInput.jump)    cam.ProcessKeyboard(' ', deltaTime);

    if (virtualInput.lookX != 0.0f || virtualInput.lookY != 0.0f) {
        constexpr float thumbstickLookMultiplier = 3.0f;
        cam.ProcessMouseMovement(
            virtualInput.lookX * virtualInput.lookSpeed * thumbstickLookMultiplier * deltaTime,
            virtualInput.lookY * virtualInput.lookSpeed * thumbstickLookMultiplier * deltaTime);
    }

    if (virtualInput.shoot) {
        scene.fireCooldown -= deltaTime;
        if (scene.fireCooldown <= 0.0f) {
            ShootPlayerWeapon();
            scene.fireCooldown = PlayerFireInterval();
        }
    }

    // Jump is a one-shot: consume it here so a single click is a single jump.
    // The held/analog flags are rebuilt from scratch by the pad in RenderUI,
    // which runs later in the frame -- don't clear them here or they'd be wiped
    // before ever being applied.
    virtualInput.jump = false;
}

static void ProcessInput(HWND) {
    const bool scopeRequested = IsGameplayScreen() &&
        scene.playerHealth > 0.0f && GunModel::SVDSelected() &&
        !g_drivingHumvee && !cameraLocked &&
        !(showUI && ImGui::GetIO().WantCaptureMouse) &&
        (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0;
    scene.UpdateSniperScope(scopeRequested, deltaTime);
    if (cameraLocked || (showUI && ImGui::GetIO().WantCaptureKeyboard)) return;
    if (g_drivingHumvee) {
        g_humveeTurretFireCooldown = (std::max)(
            0.0f, g_humveeTurretFireCooldown - deltaTime);
        const float throttle =
            ((GetAsyncKeyState('W') & 0x8000) ? 1.0f : 0.0f) -
            ((GetAsyncKeyState('S') & 0x8000) ? 1.0f : 0.0f);
        const float manualSteering =
            ((GetAsyncKeyState('A') & 0x8000) ? 1.0f : 0.0f) -
            ((GetAsyncKeyState('D') & 0x8000) ? 1.0f : 0.0f);
        float steering = manualSteering * 0.45f;
        XMFLOAT4X4 vehiclePose;
        XMFLOAT3 vehicleForward;
        if (g_destruction.GetVehicleTransform(
                vehiclePose, nullptr, &vehicleForward)) {
            XMVECTOR desiredVector = XMVectorSet(
                scene.camera.Front.x, 0.0f, scene.camera.Front.z, 0.0f);
            XMVECTOR vehicleVector = XMVectorSet(
                vehicleForward.x, 0.0f, vehicleForward.z, 0.0f);
            if (XMVectorGetX(XMVector3LengthSq(desiredVector)) > 0.001f &&
                XMVectorGetX(XMVector3LengthSq(vehicleVector)) > 0.001f) {
                desiredVector = XMVector3Normalize(desiredVector);
                vehicleVector = XMVector3Normalize(vehicleVector);
                const float dot = (std::max)(-1.0f, (std::min)(1.0f,
                    XMVectorGetX(XMVector3Dot(vehicleVector, desiredVector))));
                const float cross = vehicleForward.z * XMVectorGetX(desiredVector) -
                                    vehicleForward.x * XMVectorGetZ(desiredVector);
                const float headingError = std::atan2(cross, dot);
                steering += headingError * 1.45f;
            }
        }
        steering = (std::max)(-1.0f, (std::min)(1.0f, steering));
        g_destruction.SetVehicleInput(
            throttle, steering, (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0);
        if ((GetAsyncKeyState(VK_LBUTTON) & 0x8000) &&
            !ImGui::GetIO().WantCaptureMouse)
            FireHumveeTurret();
        return;
    }
    g_destruction.SetVehicleInput(0.0f, 0.0f, true);
    ApplyVirtualInput();
    const bool crouching = scene.camera.FPSMode &&
        (GetAsyncKeyState(VK_CONTROL) & 0x8000);
    scene.camera.SetCrouching(crouching, deltaTime);
    const float movementMultiplier = crouching ? 0.55f :
        ((scene.camera.FPSMode && (GetAsyncKeyState(VK_SHIFT) & 0x8000))
            ? 2.0f : 1.0f);
    if (GetAsyncKeyState('W') & 0x8000) scene.camera.ProcessKeyboard('W', deltaTime, movementMultiplier);
    if (GetAsyncKeyState('S') & 0x8000) scene.camera.ProcessKeyboard('S', deltaTime, movementMultiplier);
    if (GetAsyncKeyState('A') & 0x8000) scene.camera.ProcessKeyboard('A', deltaTime, movementMultiplier);
    if (GetAsyncKeyState('D') & 0x8000) scene.camera.ProcessKeyboard('D', deltaTime, movementMultiplier);
    if (!crouching && (GetAsyncKeyState(VK_SPACE) & 0x8000))
        scene.camera.ProcessKeyboard(' ', deltaTime);

    // Auto-fire: while the mouse is held (and not interacting with the UI),
    // keep shooting on a fixed interval instead of one shot per click.
    scene.fireCooldown -= deltaTime;
    const bool mouseHeld = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    if (!mouseHeld) g_suppressFireUntilMouseRelease = false;
    if (scene.autoFire && mouseHeld && !g_suppressFireUntilMouseRelease &&
        !ImGui::GetIO().WantCaptureMouse &&
        scene.fireCooldown <= 0.0f) {
        ShootPlayerWeapon();
        scene.fireCooldown = PlayerFireInterval();
    }

    // Grenade: press G to lob one. Cooldown debounces the held key.
    scene.grenadeCooldown -= deltaTime;
    if ((GetAsyncKeyState('G') & 0x8000) && scene.grenadeCooldown <= 0.0f) {
        scene.ThrowGrenade();
        scene.grenadeCooldown = 0.6f;
    }
}

// ?? window proc ??????????????????????????????????????????????????????????????
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) return true;

    // ImGui gets first chance to consume menu clicks. Any unconsumed gameplay
    // mouse input stops here so it cannot rotate camera, fire, or change guns.
    const bool blocksGameplayMouse = gameScreen == GameScreen::MainMenu ||
        gameScreen == GameScreen::WinScreen ||
        (gameScreen == GameScreen::Level1 && !scene.playerGodMode &&
         scene.playerHealth <= 0.0f);
    if (blocksGameplayMouse &&
        (msg == WM_MOUSEMOVE || msg == WM_MOUSEWHEEL ||
         msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP ||
         msg == WM_RBUTTONDOWN || msg == WM_RBUTTONUP))
        return 0;

    if (IsEditorEditing() &&
        (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONUP || msg == WM_MOUSEWHEEL))
        return 0;

    switch (msg) {
    case WM_SIZE:
        if (g_dx12.device && g_dx12.initialized && wParam != SIZE_MINIMIZED) {
            unsigned w = LOWORD(lParam), h = HIWORD(lParam);
            if (w > 0 && h > 0 && (w != SCR_WIDTH || h != SCR_HEIGHT)) {
                WaitForGPU();
                SCR_WIDTH = w; SCR_HEIGHT = h;
                ResizeDX12(SCR_WIDTH, SCR_HEIGHT);
                if (occlusionDepth.initialized) occlusionDepth.Resize(SCR_WIDTH, SCR_HEIGHT);
                if (fxaa.initialized) fxaa.Resize(SCR_WIDTH, SCR_HEIGHT);
                if (msaa.initialized) msaa.Resize(SCR_WIDTH, SCR_HEIGHT);
                if (visBuffer.initialized) visBuffer.Resize(SCR_WIDTH, SCR_HEIGHT);
                if (grassMSAA.initialized) grassMSAA.Resize(SCR_WIDTH, SCR_HEIGHT);
                if (g_rt.initialized) ResizeRaytracing(SCR_WIDTH, SCR_HEIGHT);
            }
        }
        return 0;

    case WM_MOUSEWHEEL:
        if (!ImGui::GetIO().WantCaptureMouse) {
            GunModel::CycleWeapon(GET_WHEEL_DELTA_WPARAM(wParam));
            scene.fireCooldown = 0.0f;
        }
        return 0;

    case WM_MOUSEMOVE:
        if (!cameraLocked && !(showUI && ImGui::GetIO().WantCaptureMouse)) {
            if (ignoreNextMouseMove) {
                // This move was generated by our own SetCursorPos recenter below,
                // not real user input - skip it so it can't be misread as a delta.
                ignoreNextMouseMove = false;
                return 0;
            }

            float xpos = (float)GET_X_LPARAM(lParam);
            float ypos = (float)GET_Y_LPARAM(lParam);

            RECT r; GetClientRect(hwnd, &r);
            float centerX = (float)(r.right - r.left) / 2.0f;
            float centerY = (float)(r.bottom - r.top) / 2.0f;

            if (firstMouse) { lastX = centerX; lastY = centerY; firstMouse = false; }
            else {
                float dx = xpos - lastX;
                float dy = lastY - ypos; // screen Y grows downward; flip so moving the mouse up looks up
                if (dx != 0.0f || dy != 0.0f) {
                    const float scopeScale = scene.ScopeLookScale();
                    scene.camera.ProcessMouseMovement(dx * scopeScale, dy * scopeScale);
                }
            }

            // Re-center the cursor every move so it never reaches the screen
            // edge and clamps, which would otherwise cap how far you can turn.
            POINT c = { (LONG)centerX, (LONG)centerY };
            ClientToScreen(hwnd, &c);
            ignoreNextMouseMove = true;
            SetCursorPos(c.x, c.y);
            lastX = centerX; lastY = centerY;
        }
        return 0;

    case WM_RBUTTONDOWN:
        if (IsEditorEditing() && !ImGui::GetIO().WantCaptureMouse) {
            cameraLocked = false;
            SetCapture(hwnd);
            SetCursorVisible(false);
            firstMouse = true;
        }
        return 0;

    case WM_RBUTTONUP:
        if (IsEditorEditing()) {
            cameraLocked = true;
            ReleaseCapture();
            SetCursorVisible(true);
        }
        return 0;

    case WM_LBUTTONDOWN:
        if (!ImGui::GetIO().WantCaptureMouse) {
            if (g_drivingHumvee) {
                FireHumveeTurret();
                return 0;
            } else if (HeldBarrel()) {
                ThrowHeldBarrel();
                g_suppressFireUntilMouseRelease = true;
            } else if (g_heldBandit && g_heldBandit->Held()) {
                GrabOrThrowBandit();
                g_suppressFireUntilMouseRelease = true;
            } else if (cameraLocked) {
                cameraLocked = false;
                SetCapture(hwnd); ShowCursor(FALSE);
                RECT r; GetClientRect(hwnd, &r);
                POINT c = { (r.right-r.left)/2, (r.bottom-r.top)/2 };
                ClientToScreen(hwnd, &c);
                ignoreNextMouseMove = true;
                SetCursorPos(c.x, c.y);
                lastX = (float)(r.right-r.left)/2;
                lastY = (float)(r.bottom-r.top)/2;
                firstMouse = true;
            } else if (!scene.autoFire) {
                // Auto-fire handles shooting in ProcessInput while held; only
                // fire on click when auto-fire is off.
                ShootPlayerWeapon();
            }
        }
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) {
            if (IsEditorPlaying()) pendingEditorStopPlay = true;
            else if (IsEditorEditing()) {
                if (!g_levelEditor.IsDirty()) OpenMainMenu();
            }
            else if (gameScreen != GameScreen::MainMenu) OpenMainMenu();
            else PostQuitMessage(0);
        }
        else if (IsEditorEditing()) {
            const bool controlDown = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            if (!(GetAsyncKeyState(VK_RBUTTON) & 0x8000))
                g_levelEditor.OnKeyDown(static_cast<unsigned>(wParam), controlDown);
        }
        else if (wParam == VK_TAB) {
            showUI = !showUI;
            if (showUI) {
                cameraLocked = true;
                ReleaseCapture(); ShowCursor(TRUE);
            } else {
                // Hiding the UI: capture and re-center the mouse so the next
                // WM_MOUSEMOVE delta is computed from the window center instead
                // of wherever the cursor happened to be over the UI, which
                // otherwise causes the camera to snap-rotate on the first move.
                cameraLocked = false;
                SetCapture(hwnd); ShowCursor(FALSE);
                RECT r; GetClientRect(hwnd, &r);
                POINT c = { (r.right - r.left) / 2, (r.bottom - r.top) / 2 };
                ClientToScreen(hwnd, &c);
                ignoreNextMouseMove = true;
                SetCursorPos(c.x, c.y);
                lastX = (float)(r.right - r.left) / 2;
                lastY = (float)(r.bottom - r.top) / 2;
                firstMouse = true;
            }
        }
        else if (wParam == 'C')    { cameraLocked = true; ReleaseCapture(); ShowCursor(TRUE); }
        else if (!g_emptyLevelMode && wParam == 'F' &&
                 !(lParam & 0x40000000)) { GrabOrThrowObject(); }
        else if (wParam == 'V' && !(lParam & 0x40000000)) {
            scene.camera.FPSMode = !scene.camera.FPSMode;
        }
        else if (wParam == 'M' && !(lParam & 0x40000000)) {
            if (visBuffer.initialized) {
                scene.useVisibilityBuffer = !scene.useVisibilityBuffer;
                if (scene.useVisibilityBuffer) {
                    scene.useRaytracing = false;
                    g_rt.enabled = false;
                }
            }
        }
        else if (!g_emptyLevelMode && wParam == 'E' &&
                 !(lParam & 0x40000000)) {
            ToggleHumveeDriving();
        }
        // Bit 30 = key was already down (autorepeat); toggle once per press.
        else if (wParam == 'Z' && !(lParam & 0x40000000)) {
            scene.meshletWireframe = !scene.meshletWireframe;
        }
        else if (wParam == VK_F11) { ToggleFullscreen(hwnd); }
        return 0;

    case WM_DESTROY:
        std::ofstream("engine_runtime_error.log", std::ios::app)
            << "WM_DESTROY\n";
        PostQuitMessage(0); return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// ?? entry point ??????????????????????????????????????????????????????????????
// Unhandled-exception hook: write a minidump next to the exe (dumps/crash.dmp)
// so a crash leaves a debuggable artifact instead of just an event-log entry.
static LONG WINAPI WriteCrashDump(EXCEPTION_POINTERS* info) {
    CreateDirectoryA("dumps", nullptr);
    HANDLE file = CreateFileA("dumps/crash.dmp", GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei = {};
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = info;
        mei.ClientPointers = FALSE;
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                          MiniDumpWithIndirectlyReferencedMemory, &mei, nullptr, nullptr);
        CloseHandle(file);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    SetUnhandledExceptionFilter(WriteCrashDump);
    AllocConsole();
    FILE* fp;
    freopen_s(&fp, "CONOUT$", "w", stdout);
    freopen_s(&fp, "CONOUT$", "w", stderr);

    std::cout << "GraphicEngine DX12 Starting..." << std::endl;

    // Window
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"GraphicEngineDX12";
    RegisterClassExW(&wc);

    DEVMODEW displayMode = {};
    displayMode.dmSize = sizeof(displayMode);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &displayMode)) {
        SCR_WIDTH = displayMode.dmPelsWidth;
        SCR_HEIGHT = displayMode.dmPelsHeight;
    }

    RECT rc = { 0, 0, (LONG)SCR_WIDTH, (LONG)SCR_HEIGHT };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowW(L"GraphicEngineDX12", L"Graphics Engine - DirectX 12",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) { std::cerr << "Window creation failed\n"; return -1; }
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    // DX12
    try {
        if (!InitDX12(hwnd, SCR_WIDTH, SCR_HEIGHT)) {
            MessageBoxA(hwnd, "Failed to init DX12.", "Error", MB_OK | MB_ICONERROR);
            return -1;
        }
    } catch (const std::exception& e) {
        MessageBoxA(hwnd, e.what(), "DX12 Error", MB_OK | MB_ICONERROR);
        return -1;
    }

    if (!g_profiler.Init(g_dx12.device.Get(), g_dx12.commandQueue.Get()))
        std::cerr << "GPU profiler unavailable; CPU profiling remains active\n";
    g_gunAudio.Initialize("models/audio/rifle_shot.wav");
    g_rpgFireAudio.Initialize("models/audio/rpg_fire.wav");
    g_explosionAudio.Initialize("build/Sounds/Rpg/RocketExplosion3.mp3");
    g_grenadeExplosionAudio.Initialize("models/audio/explosion.ogg");
    scene.explosionAudioCallback = [](const XMFLOAT3& position, float size,
                                      bool grenade) {
        const float dx = position.x - scene.camera.Position.x;
        const float dy = position.y - scene.camera.Position.y;
        const float dz = position.z - scene.camera.Position.z;
        const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        const float reach = 55.0f + size * 4.0f;
        const float volume = (std::max)(0.08f, 1.0f - distance / reach);
        const float pitch = 0.92f + ((float)std::rand() / RAND_MAX) * 0.10f;
        (grenade ? g_grenadeExplosionAudio : g_explosionAudio).Play(volume, pitch);
    };
    g_hitAudio.Initialize("models/audio/bullet_flesh_hit.mp3");
    g_banditSpottedAudio1.Initialize("models/audio/bandit_spotted_01.wav");
    g_banditSpottedAudio2.Initialize("models/audio/bandit_spotted_02.wav");
    g_banditAttackAudio.Initialize("models/audio/bandit_attack.wav");
    g_banditDeathAudio.Initialize("models/audio/bandit_death.wav");
    g_banditHitVoiceAudio.Initialize("models/audio/bandit_hit_voice.wav");
    g_helicopterHoverAudio.Initialize("models/audio/helicopter_hover_loop.mp3");

    // ImGui
    D3D12_DESCRIPTOR_HEAP_DESC ihd = {};
    ihd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    ihd.NumDescriptors = 1;
    ihd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    g_dx12.device->CreateDescriptorHeap(&ihd, IID_PPV_ARGS(&imguiSrvHeap));

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX12_Init(g_dx12.device.Get(), FRAME_COUNT, DXGI_FORMAT_R8G8B8A8_UNORM,
        imguiSrvHeap.Get(),
        imguiSrvHeap->GetCPUDescriptorHandleForHeapStart(),
        imguiSrvHeap->GetGPUDescriptorHandleForHeapStart());

    // Geometry
    if (!CreateAllGeometry()) { std::cerr << "Geometry creation failed\n"; return -1; }

    // Shaders - the DX12-specific pair supports albedo/normal/metal-roughness texture
    // sampling (needed for imported GLB materials); the plain "clustered_*" pair is
    // color-only and silently ignores textures, so it's only a fallback.
    if (!mainShader.Load("shaders/clustered_dx12_vs.hlsl", "shaders/clustered_dx12_ps.hlsl")) {
        std::cerr << "Trying fallback shaders...\n";
        if (!mainShader.Load("shaders/clustered_vs.hlsl", "shaders/clustered_ps.hlsl")) {
            MessageBoxA(hwnd, "Shader load failed.", "Shader Error", MB_OK | MB_ICONERROR);
            return -1;
        }
    }
    std::cout << "Shaders loaded\n";

    g_useMeshShader = g_meshShader.Init(mainShader);
    std::cout << (g_useMeshShader
        ? "Mesh shader path enabled\n"
        : "Mesh shader path unavailable; using raster fallback\n");

    bool terrainReady = false;
    if (g_useMeshShader) {
        // Terrain initialization uploads its PBR texture arrays. InitDX12 leaves
        // the command list closed, so record and submit those copies explicitly;
        // otherwise the following sky reset discards them and every terrain SRV
        // samples zero.
        ThrowIfFailed(g_dx12.commandAllocators[g_dx12.frameIndex]->Reset());
        ThrowIfFailed(g_dx12.commandList->Reset(
            g_dx12.commandAllocators[g_dx12.frameIndex].Get(), nullptr));
        terrainReady = g_terrain.Init(mainShader);
        ThrowIfFailed(g_dx12.commandList->Close());
        {
            ID3D12CommandList* terrainLists[] = { g_dx12.commandList.Get() };
            g_dx12.commandQueue->ExecuteCommandLists(1, terrainLists);
        }
        WaitForGPU();
        DumpDX12DebugMessages();
    }
    if (!terrainReady) {
        scene.useMeshTerrain = false;
        std::cerr << "Mesh shader terrain unavailable; keeping flat floor\n";
    }
    scene.grenadeGroundHeight = [](float x, float z) {
        if (!scene.useMeshTerrain || !g_terrain.supported) return 0.0f;
        auto params = CurrentTerrainParams();
        params.heightScale = scene.terrainHeightScale;
        return TerrainRendererDX12::HeightAt(params, x, z);
    };

    // Mip generator (compute shader) for imported GLB textures
    if (!g_mipGen.Init()) {
        std::cerr << "Mip generator init failed (non-fatal, textures will have no mips)\n";
    }

    // The command list is closed after InitDX12 and stays closed until the first
    // BeginFrame(). skyRenderer.Init() records a CopyTextureRegion for the HDRI
    // upload, so the list must be open while it runs and its work must be flushed
    // (executed + waited) before the list is closed again - otherwise the copy
    // never reaches the GPU and the sky texture stays black.
    ThrowIfFailed(g_dx12.commandAllocators[g_dx12.frameIndex]->Reset());
    ThrowIfFailed(g_dx12.commandList->Reset(g_dx12.commandAllocators[g_dx12.frameIndex].Get(), nullptr));
    if (!skyRenderer.Init()) {
        std::cerr << "HDRI sky init failed (non-fatal)\n";
    }
    g_skyEnvironmentResource = skyRenderer.skyTexture.Get();
    if (environmentIBL.Init(g_skyEnvironmentResource)) {
        g_specularEnvironmentResource =
            environmentIBL.prefilteredEnvironment.Get();
        g_brdfIntegrationResource = environmentIBL.brdfIntegrationLUT.Get();
        std::cout << "GGX HDRI prefilter and BRDF integration LUT ready\n";
    } else {
        g_specularEnvironmentResource = g_skyEnvironmentResource;
        std::cerr << "Specular IBL prefilter failed (using raw HDRI fallback)\n";
    }
    ThrowIfFailed(g_dx12.commandList->Close());
    {
        ID3D12CommandList* skyLists[] = { g_dx12.commandList.Get() };
        g_dx12.commandQueue->ExecuteCommandLists(1, skyLists);
    }
    WaitForGPU();
    g_mipGen.FlushPending();
    DumpDX12DebugMessages();
    {
        auto skySH = GLBImporter::ComputeSkyIrradianceSH(kSkyEnvironmentPath);
        mainShader.SetSkyIrradiance(skySH, 1.0f);
        const HDRISunLight hdriSun =
            GLBImporter::ExtractHDRISunLight(kSkyEnvironmentPath, 2.1f);
        if (hdriSun.valid) {
            constexpr float DirectionScale = 10.0f;
            scene.lightPos = {
                hdriSun.direction.x * DirectionScale,
                hdriSun.direction.y * DirectionScale,
                hdriSun.direction.z * DirectionScale
            };
            scene.lightColor = hdriSun.color;
            std::cout << "HDRI sun aligned: direction=("
                      << hdriSun.direction.x << ", "
                      << hdriSun.direction.y << ", "
                      << hdriSun.direction.z << ") color=("
                      << hdriSun.color.x << ", "
                      << hdriSun.color.y << ", "
                      << hdriSun.color.z << ") sourceLuminance="
                      << hdriSun.sourceLuminance << '\n';
        } else {
            std::cerr << "HDRI sun extraction failed; using scene fallback light\n";
        }
    }
    if (!occlusionDepth.Init(SCR_WIDTH, SCR_HEIGHT)) {
        std::cerr << "Meshlet occlusion depth init failed (non-fatal)\n";
    }
    if (!fxaa.Init(SCR_WIDTH, SCR_HEIGHT)) {
        std::cerr << "FXAA init failed (non-fatal)\n";
        scene.enableFXAA = false;
    }
    if (!volumetricFog.Init()) {
        std::cerr << "Volumetric fog init failed (non-fatal)\n";
        scene.enableVolumetricFog = false;
    }
    if (!screenSpaceAO.Init()) {
        std::cerr << "Screen-space AO init failed (non-fatal)\n";
        scene.enableAmbientOcclusion = false;
    }
    const bool msaaPipelinesReady =
        mainShader.msaaSupported &&
        (!g_useMeshShader || g_meshShader.msaaSupported) &&
        (!g_terrain.supported || g_terrain.msaaSupported) &&
        (!skyRenderer.initialized || skyRenderer.msaaSupported);
    if (!msaa.Init(SCR_WIDTH, SCR_HEIGHT) || !msaaPipelinesReady) {
        std::cerr << "4x MSAA unavailable (non-fatal)\n";
        scene.enableMSAA = false;
    }

    // Visibility buffer (id Tech path)
    // Init uploads the 3D colour LUT. Record and submit that work now; the
    // command list has been closed since the sky upload above.
    ThrowIfFailed(g_dx12.commandAllocators[g_dx12.frameIndex]->Reset());
    ThrowIfFailed(g_dx12.commandList->Reset(
        g_dx12.commandAllocators[g_dx12.frameIndex].Get(), nullptr));
    const bool visibilityBufferReady = visBuffer.Init(SCR_WIDTH, SCR_HEIGHT);
    ThrowIfFailed(g_dx12.commandList->Close());
    {
        ID3D12CommandList* visibilityInitLists[] = { g_dx12.commandList.Get() };
        g_dx12.commandQueue->ExecuteCommandLists(1, visibilityInitLists);
    }
    WaitForGPU();
    DumpDX12DebugMessages();
    if (!visibilityBufferReady) {
        std::cerr << "VB init failed (non-fatal)\n";
        scene.useVisibilityBuffer = false;
    } else {
        visBuffer.UpdateEnvironmentMap(
            g_specularEnvironmentResource, g_brdfIntegrationResource);
        std::cout << "Visibility Buffer ready\n";
    }

    if (!visibilityBufferReady || !mainShader.GetHDRMSAAGrassPipelineState() ||
        !grassMSAA.Init(SCR_WIDTH, SCR_HEIGHT)) {
        std::cerr << "Visibility grass 4x MSAA unavailable (non-fatal)\n";
        scene.enableGrassMSAA = false;
    } else {
        std::cout << "Visibility grass 4x MSAA ready\n";
    }

    if (!shadowMap.Init()) {
        std::cerr << "Shadow map init failed (non-fatal)\n";
        scene.enableShadows = false;
    }

    if (!g_ddgiRenderer.init(g_dx12.device.Get())) {
        std::cerr << "DDGI init failed (non-fatal)\n";
        scene.useDDGI = false;
    } else {
        g_ddgiRenderer.RegisterShadowMap(shadowMap.GetResource());
        g_ddgiIrradianceResource = g_ddgiRenderer.irradianceTexture.Get();
        g_ddgiVisibilityResource = g_ddgiRenderer.visibilityTexture.Get();
        if (visibilityBufferReady) {
            visBuffer.UpdateDDGIResources(
                g_ddgiIrradianceResource, g_ddgiVisibilityResource);
        }
        std::cout << "DDGI ready\n";
    }

    // Raytracing (DXR path)
    if (!InitRaytracing(geo)) {
        std::cerr << "DXR init failed (non-fatal)\n";
        scene.useRaytracing = false;
    } else {
        std::cout << "DXR Raytracing ready\n";
    }

    // Scene lights
    scene.InitLights();

    // Timer
    gameTimer.Start();
    float lastTime = 0.0f;

    std::cout << "Controls: WASD, Mouse, TAB=UI, F11=Fullscreen, ESC=Exit\n";

    // Deterministic renderer smoke path for GPU validation and crash dumps.
    bool visibilityTestPending = false;
    UINT visibilityTestForwardFrames = 0;
    bool visibilitySmokeEnabled = false;
    bool visibilitySmokeReported = false;
    UINT visibilityCpuReportFrames = 0;
    const bool visibilityBenchmark =
        GetEnvironmentVariableA("SGE_VISIBILITY_BENCHMARK", nullptr, 0) > 0;
    const bool visibilityForwardOnly =
        GetEnvironmentVariableA("SGE_VISIBILITY_FORWARD_ONLY", nullptr, 0) > 0;
    UINT visibilityBenchmarkVBFrames = 0;
    UINT visibilityBenchmarkForwardSamples = 0;
    UINT visibilityBenchmarkVBSamples = 0;
    double visibilityBenchmarkForwardMs = 0.0;
    double visibilityBenchmarkVBMs = 0.0;
    bool visibilityBenchmarkComplete = false;
    if (GetEnvironmentVariableA("SGE_VISIBILITY_TEST", nullptr, 0) > 0) {
        visibilitySmokeEnabled = true;
        std::ofstream("visibility_smoke.log", std::ios::trunc)
            << "starting\n";
        char visibilityDebugMode[8] = {};
        if (GetEnvironmentVariableA("SGE_VISIBILITY_DEBUG", visibilityDebugMode,
                static_cast<DWORD>(sizeof(visibilityDebugMode))) > 0) {
            visBuffer.debugViewMode = (std::max)(0, (std::min)(2,
                atoi(visibilityDebugMode)));
        }
        scene.useVisibilityBuffer = false;
        visibilityTestPending = true;
        const bool emptyVisibilityTest =
            GetEnvironmentVariableA("SGE_VISIBILITY_TEST_FULL", nullptr, 0) == 0;
        const bool stressVisibilityTest =
            GetEnvironmentVariableA("SGE_VISIBILITY_TEST_STRESS", nullptr, 0) > 0;
        StartLevelOne(hwnd, true, stressVisibilityTest, emptyVisibilityTest);
    }

    // ?? main loop ????????????????????????????????????????????????????????????
    MSG msg = {};
    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            continue;
        }

        float now = gameTimer.GetElapsed();
        deltaTime = now - lastTime;
        lastTime  = now;

        if (pendingEditorStopPlay) {
            pendingEditorStopPlay = false;
            StopEditorPlaytest();
        }
        if (pendingEditorBeginPlay) {
            pendingEditorBeginPlay = false;
            BeginEditorPlaytest(hwnd);
        }
        if (pendingEditorReturnToMenu) {
            pendingEditorReturnToMenu = false;
            g_customLevelMode = false;
            OpenMainMenu();
        }
        if (g_pendingEnvironmentRebuild && IsSceneScreen() &&
            !levelLoadingActive && g_environmentInitialized) {
            WaitForGPU();
            RebuildScalableEnvironment();
            if (g_trees.IsInitialized()) ResetPalmTrees();
            g_pendingEnvironmentRebuild = false;
        }
        if (IsEditorEditing() && !levelLoadingActive &&
            g_levelEditor.RuntimeDirty())
            SynchronizeEditorRuntime(false);

        if (IsEditorEditing() && !cameraLocked &&
            !ImGui::GetIO().WantCaptureKeyboard) {
            const float speed = (GetAsyncKeyState(VK_SHIFT) & 0x8000) ? 3.0f : 1.0f;
            if (GetAsyncKeyState('W') & 0x8000) scene.camera.ProcessKeyboard('W', deltaTime, speed);
            if (GetAsyncKeyState('S') & 0x8000) scene.camera.ProcessKeyboard('S', deltaTime, speed);
            if (GetAsyncKeyState('A') & 0x8000) scene.camera.ProcessKeyboard('A', deltaTime, speed);
            if (GetAsyncKeyState('D') & 0x8000) scene.camera.ProcessKeyboard('D', deltaTime, speed);
            if (GetAsyncKeyState(VK_SPACE) & 0x8000)
                scene.camera.Position.y += scene.camera.MovementSpeed * speed * deltaTime;
            if (GetAsyncKeyState('Q') & 0x8000)
                scene.camera.Position.y -= scene.camera.MovementSpeed * speed * deltaTime;
        }

        UpdateHelicopterHoverAudio();

        g_profiler.BeginCpuFrame();
        {
        ProfilerDX12::CpuScope updateProfile(g_profiler, "Update");

        if (IsGameplayScreen() && !levelLoadingActive &&
            (scene.playerGodMode || scene.playerHealth > 0.0f)) {

        if (pendingLevelRuntimeReset) {
            // Restart is requested from ImGui after the previous frame's enemy
            // draws were recorded. Drain GPU use before destroying their buffers.
            WaitForGPU();
            g_bandits.clear();
            g_heldBandit = nullptr;
            if (!g_emptyLevelMode) g_water.ResetSurface();
            if (!g_emptyLevelMode) g_ocean.ResetSurface();
            if (!g_emptyLevelMode && g_trees.IsInitialized()) ResetPalmTrees();
            if (!g_emptyLevelMode && g_environmentInitialized &&
                g_environmentStressMode != g_stressTestMode)
                RebuildScalableEnvironment();
            pendingLevelRuntimeReset = false;
            if (!g_emptyLevelMode) {
                for (size_t i = 0; i < ActiveBanditSlotCount(); ++i)
                    if (!SpawnBandit()) break;
                pendingTurretGunnerRespawn = !g_customLevelMode ||
                    FirstRuntimeEntity(LevelEntityType::Humvee) != nullptr;
            } else {
                pendingTurretGunnerRespawn = false;
            }
            // Do not charge restart/reset stalls to completion time.
            lastTime = gameTimer.GetElapsed();
        }

        if (levelTimerRunning)
            levelElapsedSeconds += (std::min)(deltaTime, 0.25f);

        ProcessInput(hwnd);

        // Walking collision: ground level follows the mesh-shader terrain at
        // the camera's XZ so gravity settles the player onto the hills.
        if (scene.useMeshTerrain && g_terrain.supported) {
            auto terrainParams = CurrentTerrainParams();
            terrainParams.heightScale = scene.terrainHeightScale;
            scene.camera.FloorY = TerrainRendererDX12::HeightAt(
                terrainParams, scene.camera.Position.x, scene.camera.Position.z);
        } else {
            scene.camera.FloorY = 0.0f;
        }

        // Resolve wall/roof collision BEFORE gravity runs (scene.Update ->
        // camera.Update). Step-up raises FloorY onto brick/roof tops so the
        // ground snap in the same frame stands the player on them instead of
        // falling through. Uses last frame's body transforms -- fine for
        // standing, and avoids a one-frame lag that would drop the player.
        if (scene.useDestruction && g_destruction.IsInitialized()) {
            g_destruction.ResolvePlayerCollision(scene.camera.Position,
                scene.camera.FloorY, 0.35f, scene.camera.PlayerHeight);
        }

        scene.Update(deltaTime, now);
        if (!g_emptyLevelMode) {
            UpdateHelicopter(deltaTime);
            UpdateSecondaryHelicopter(deltaTime);
            UpdateExplosiveBarrels(deltaTime);
        }
        g_gunAudio.Update();
        g_rpgFireAudio.Update();
        g_explosionAudio.Update();
        g_grenadeExplosionAudio.Update();
        g_hitAudio.Update();
        g_banditSpottedAudio1.Update();
        g_banditSpottedAudio2.Update();
        g_banditAttackAudio.Update();
        g_banditDeathAudio.Update();
        g_banditHitVoiceAudio.Update();
        g_banditVoiceCooldown = (std::max)(0.0f, g_banditVoiceCooldown - deltaTime);
        g_banditPainCooldown = (std::max)(0.0f, g_banditPainCooldown - deltaTime);

        if (scene.rebuildDestructionRequested && wallModel) {
            scene.rebuildDestructionRequested = false;
            // Re-init frees the old chunk vertex/index buffers. The GPU may
            // still be rendering last frame's chunk meshes, so drain it first
            // or those buffers get destroyed in flight and crash.
            WaitForGPU();
            g_destruction.Initialize(wallModel, g_dx12.device.Get(), 1, 1, 1);
            // Re-init rebuilds physics with a flat ground; restore the terrain
            // heightfield collider so debris keeps colliding with real ground.
            auto tp = CurrentTerrainParams();
            tp.heightScale = scene.terrainHeightScale;
            g_destruction.SetTerrainSampler([tp](float x, float z) {
                return TerrainRendererDX12::HeightAt(tp, x, z);
            });
            if (!g_emptyLevelMode) {
                g_destruction.SetSplashCallback([](float x, float z, float s) {
                    g_water.Splash(x, z, s);
                });
                g_destruction.InitializeVehicle(g_customLevelMode
                    ? g_primaryHumveeSpawn : XMFLOAT3{ 0.0f, 3.45f, 0.0f },
                    g_customLevelMode ? XMConvertToRadians(g_primaryHumveeYaw) : 0.0f);
            }
        }
        // Dead Bandits stay attached to their ragdolls. No mid-level respawns.
        if (!g_emptyLevelMode && g_banditLoaded) {
            ProfilerDX12::CpuScope banditProfile(g_profiler, "Bandit Update");
            if (pendingTurretGunnerRespawn) {
                const bool firstReady = SpawnHumveeTurretGunner(0);
                const bool secondReady = !g_stressTestMode ||
                    SpawnHumveeTurretGunner(1);
                pendingTurretGunnerRespawn = !(firstReady && secondReady);
            }
            if (g_heldBandit && g_heldBandit->Dead()) g_heldBandit = nullptr;
            static std::unordered_map<SkinnedEnemy*, float> banditUpdateDebt;
            for (auto& bandit : g_bandits) {
                if (!bandit || bandit->Dead()) continue;
                if (bandit.get() == g_heldBandit) {
                    const XMFLOAT3& eye = scene.camera.Position;
                    const XMFLOAT3& front = scene.camera.Front;
                    const XMFLOAT3 holdPosition = {
                        eye.x + front.x * 2.15f,
                        eye.y + front.y * 2.15f - bandit->footOffset - 1.15f,
                        eye.z + front.z * 2.15f };
                    const float facingYaw = std::atan2(-front.x, -front.z);
                    bandit->HoldAt(deltaTime, holdPosition, facingYaw);
                    continue;
                }
                bandit->leftArmReach = g_banditLeftArmReach;
                bandit->headTorsoYawOffsetDegrees =
                    g_banditHeadYawOffsetDegrees;
                const float cameraDx = bandit->position.x - scene.camera.Position.x;
                const float cameraDz = bandit->position.z - scene.camera.Position.z;
                const float cameraDistanceSq = cameraDx * cameraDx + cameraDz * cameraDz;
                const float updateInterval = cameraDistanceSq > 55.0f * 55.0f
                    ? (1.0f / 15.0f)
                    : (cameraDistanceSq > 30.0f * 30.0f ? (1.0f / 30.0f) : 0.0f);
                float& updateDebt = banditUpdateDebt[bandit.get()];
                updateDebt += deltaTime;
                const bool updateBandit = updateInterval == 0.0f ||
                    updateDebt >= updateInterval;
                const float banditDeltaTime = updateBandit ? updateDebt : 0.0f;
                if (updateBandit) updateDebt = 0.0f;
                if (updateBandit && bandit->turretGunner) {
                    const XMFLOAT3 mount = bandit->mountedVehicleIndex == 0
                        ? HumveeTurretMountWorld()
                        : XMFLOAT3{
                            g_secondaryHumveePosition.x + g_humveeTurretLocal.x,
                            g_humveeTurretLocal.y + 3.45f,
                            g_secondaryHumveePosition.z + g_humveeTurretLocal.z };
                    bandit->UpdateMounted(
                        banditDeltaTime, mount,
                        (g_drivingHumvee && bandit->mountedVehicleIndex == 0)
                            ? g_humveeAimPoint : scene.camera.Position);
                } else if (updateBandit) {
                    float groundY = 0.0f;
                    if (scene.useMeshTerrain && g_terrain.supported) {
                        auto tp = CurrentTerrainParams();
                        tp.heightScale = scene.terrainHeightScale;
                        groundY = TerrainRendererDX12::HeightAt(
                            tp, bandit->position.x, bandit->position.z);
                    }
                    bandit->Update(banditDeltaTime, scene.camera.Position, groundY);
                    ResolveBanditHumveeCollision(*bandit);
                }
                XMFLOAT3 shotOrigin, shotDirection;
                // Terrain LOS is deliberately expensive. Test only when a shot
                // is actually ready, not every frame of the two-second aim pause.
                const bool hasLineOfSight =
                    !bandit->NeedsLineOfSightCheck() ||
                    BanditHasLineOfSight(*bandit, scene.camera.Position);
                const bool fired = !(g_drivingHumvee && bandit->turretGunner &&
                                     bandit->mountedVehicleIndex == 0) &&
                    bandit->TryFireAt(
                        deltaTime, scene.camera.Position, hasLineOfSight,
                        shotOrigin, shotDirection);
                if (bandit->ConsumeSpottedEvent() && g_banditVoiceCooldown <= 0.0f) {
                    const float volume = BanditVoiceVolume(bandit->position);
                    const float pitch = 0.96f + ((float)std::rand() / RAND_MAX) * 0.08f;
                    if (std::rand() & 1) g_banditSpottedAudio1.Play(volume, pitch);
                    else g_banditSpottedAudio2.Play(volume, pitch);
                    g_banditVoiceCooldown = 3.5f;
                }
                if (bandit->ConsumeAttackEvent() && g_banditVoiceCooldown <= 0.0f) {
                    const float pitch = 0.96f + ((float)std::rand() / RAND_MAX) * 0.08f;
                    g_banditAttackAudio.Play(BanditVoiceVolume(bandit->position), pitch);
                    g_banditVoiceCooldown = 4.5f;
                }
                if (fired) {
                    scene.SpawnHostileProjectile(shotOrigin, shotDirection);
                    const float dx = shotOrigin.x - scene.camera.Position.x;
                    const float dy = shotOrigin.y - scene.camera.Position.y;
                    const float dz = shotOrigin.z - scene.camera.Position.z;
                    const float distance = std::sqrt(dx*dx + dy*dy + dz*dz);
                    const float volume =
                        (std::max)(0.08f, 0.58f * (1.0f - distance / 45.0f));
                    const float pitch =
                        0.88f + ((float)std::rand() / RAND_MAX) * 0.08f;
                    g_gunAudio.Play(volume, pitch);
                }
            }
            UpdateHelicopterRotorKills();
        }
        if (!g_emptyLevelMode) {
        g_water.Update(deltaTime);
        g_ocean.Update(deltaTime);
        g_trees.SetWind(g_grass.WindStrength(), g_grass.WindSpeed());
        const bool primaryHelicopterActive =
            scene.showHelicopter && !g_helicopterDead && !g_helicopterCrashed;
        const bool secondaryHelicopterActive =
            scene.showHelicopter && g_stressTestMode &&
            !g_secondaryHelicopterDead && !g_secondaryHelicopterCrashed;
        g_trees.SetHelicopterWind(
            g_helicopterPosition, primaryHelicopterActive,
            g_secondaryHelicopterPosition, secondaryHelicopterActive);
        g_trees.Update(deltaTime);
        XMFLOAT3 grassHelicopter = g_helicopterPosition;
        if (secondaryHelicopterActive) {
            const XMVECTOR camera = XMLoadFloat3(&scene.camera.Position);
            const float primaryDistance = primaryHelicopterActive
                ? XMVectorGetX(XMVector3LengthSq(
                    XMLoadFloat3(&g_helicopterPosition) - camera)) : FLT_MAX;
            const float secondaryDistance = XMVectorGetX(XMVector3LengthSq(
                XMLoadFloat3(&g_secondaryHelicopterPosition) - camera));
            if (secondaryDistance < primaryDistance)
                grassHelicopter = g_secondaryHelicopterPosition;
        }
        g_grass.SetHelicopterWind(
            grassHelicopter,
            primaryHelicopterActive || secondaryHelicopterActive);
        g_grass.Update(deltaTime);
        }
        if (scene.useDestruction && g_destruction.IsInitialized()) {
            g_destruction.SetEnemyTarget(scene.camera.Position);
            {
                ProfilerDX12::CpuScope profile(g_profiler, "Destruction Update");
                g_destruction.Update(deltaTime);
            }
            if (!g_emptyLevelMode) {
                UpdateHumveeImpacts(deltaTime);
                UpdateHumveeChaseCamera(deltaTime);
                UpdateHumveeTurretAim(deltaTime);
            }
            if (!g_emptyLevelMode && g_banditLoaded) {
                const std::vector<DestructionDebrisHazard> debris =
                    g_destruction.GetDangerousDebris(2.5f);
                for (const DestructionDebrisHazard& piece : debris) {
                    for (auto& bandit : g_bandits) {
                        if (!bandit || bandit->Dead()) continue;
                        XMFLOAT3 impact;
                        if (!bandit->ApplyDebrisImpact(piece, &impact)) continue;

                        XMVECTOR normalVector = -XMLoadFloat3(&piece.velocity);
                        normalVector = XMVector3Normalize(normalVector);
                        XMFLOAT3 normal;
                        XMStoreFloat3(&normal, normalVector);
                        scene.SpawnBloodBurst(impact, normal);
                        const float pitch =
                            0.9f + ((float)std::rand() / RAND_MAX) * 0.2f;
                        g_hitAudio.Play(0.72f * 0.3f, pitch);
                        if (!bandit->Dead() && g_banditPainCooldown <= 0.0f) {
                            g_banditHitVoiceAudio.Play(
                                BanditVoiceVolume(impact, 0.72f), pitch);
                            g_banditPainCooldown = 0.45f;
                        }
                        break; // one moving chunk damages one character per frame
                    }
                }
                PlayBanditDeathEvents();
                for (auto& bandit : g_bandits)
                    if (bandit && bandit->Dead()) bandit->SyncRagdoll();
            }
            for (const EnemyShot& shot : g_destruction.DrainEnemyShots()) {
                scene.SpawnHostileProjectile(shot.origin, shot.direction);
                const float dx = shot.origin.x - scene.camera.Position.x;
                const float dy = shot.origin.y - scene.camera.Position.y;
                const float dz = shot.origin.z - scene.camera.Position.z;
                const float distance = std::sqrt(dx*dx + dy*dy + dz*dz);
                const float volume = (std::max)(0.06f, 0.55f * (1.0f - distance / 45.0f));
                const float pitch = 0.88f + ((float)std::rand() / RAND_MAX) * 0.08f;
                g_gunAudio.Play(volume, pitch);
            }
            // Smoke at the actual fracture points where pieces broke loose.
            for (const XMFLOAT3& bp : g_destruction.DrainBreakPoints())
                scene.SpawnSmokeBurst(bp, 0.5f, 0.4f);
            for (const TinyDebrisParticle& tiny :
                 g_destruction.DrainTinyDebrisParticles()) {
                ImpactParticle particle;
                particle.position = tiny.position;
                particle.velocity = tiny.velocity;
                particle.maxLife = particle.life = 0.8f;
                particle.size = tiny.size;
                particle.growth = -tiny.size * 0.75f;
                particle.color = { 0.36f, 0.31f, 0.25f };
                particle.spark = true;
                scene.impactParticles.push_back(particle);
            }
            if (scene.impactParticles.size() > 800)
                scene.impactParticles.erase(scene.impactParticles.begin(),
                    scene.impactParticles.begin() +
                    (scene.impactParticles.size() - 800));
            for (auto& projectile : scene.projectiles) {
                if (projectile.rocket && projectile.active) {
                    XMFLOAT3 impact = projectile.position;
                    bool struck = false;
                    const float radius = 0.22f;
                    size_t barrelIndex = 0;
                    XMFLOAT3 candidate;
                    if (g_banditLoaded) {
                        for (const auto& bandit : g_bandits) {
                            if (bandit && bandit->BlocksProjectile(
                                    projectile.previousPosition, projectile.position,
                                    radius)) {
                                struck = true;
                                break;
                            }
                        }
                    }
                    if (!struck && HitHelicopterSegment(
                            projectile.previousPosition, projectile.position,
                            radius, candidate)) {
                        impact = candidate;
                        struck = true;
                    }
                    if (!struck && HitSecondaryHelicopterSegment(
                            projectile.previousPosition, projectile.position,
                            radius, candidate)) {
                        impact = candidate;
                        struck = true;
                    }
                    if (!struck && HitExplosiveBarrelSegment(
                            projectile.previousPosition, projectile.position,
                            radius, barrelIndex, candidate)) {
                        impact = candidate;
                        struck = true;
                    }
                    if (!struck && g_destruction.HitTestSegment(
                            projectile.previousPosition, projectile.position,
                            radius, candidate)) {
                        impact = candidate;
                        struck = true;
                    }
                    if (!struck && g_trees.BlocksSegment(
                            projectile.previousPosition, projectile.position, radius))
                        struck = true;
                    if (!struck && HitTerrainSegment(
                            projectile.previousPosition, projectile.position,
                            radius, candidate)) {
                        impact = candidate;
                        struck = true;
                    }
                    if (struck) {
                        projectile.position = impact;
                        projectile.active = false;
                        projectile.detonate = true;
                    }
                }
                if (projectile.grenade || projectile.rocket) {
                    // Grenades use fuse; rockets detonate on first solid impact.
                    if (projectile.detonate) {
                        const XMFLOAT3 center = projectile.position;
                        if (projectile.grenade)
                            AddExplosionTerrainCrater(center);
                        scene.SpawnExplosionFX(
                            { center.x, center.y + 0.6f, center.z },
                            scene.grenadeBlastRadius * 1.6f, 0.9f,
                            projectile.grenade);
                        if (!g_emptyLevelMode)
                        for (size_t i = 0; i < scene.explosiveBarrels.size(); ++i) {
                            const ExplosiveBarrel& barrel = scene.explosiveBarrels[i];
                            if (!barrel.active) continue;
                            const float dx = barrel.position.x - center.x;
                            const float dy = barrel.position.y - center.y;
                            const float dz = barrel.position.z - center.z;
                            if (dx*dx + dy*dy + dz*dz <=
                                scene.grenadeBlastRadius * scene.grenadeBlastRadius)
                                DetonateBarrel(i);
                        }
                        if (g_banditLoaded) {
                            for (auto& bandit : g_bandits) {
                                if (bandit) bandit->ApplyExplosion(
                                    center, scene.grenadeEnemyRadius,
                                    scene.grenadeEnemyDamage,
                                    scene.grenadeEnemyPush);
                            }
                            PlayBanditDeathEvents();
                        }
                        if (!g_helicopterDead && g_helicopterModel) {
                            const float dx = g_helicopterPosition.x - center.x;
                            const float dy = g_helicopterPosition.y - center.y;
                            const float dz = g_helicopterPosition.z - center.z;
                            const float distance = std::sqrt(dx*dx + dy*dy + dz*dz);
                            const float reach = scene.grenadeEnemyRadius + 5.0f;
                            if (distance < reach) {
                                const float falloff = 1.0f - distance / reach;
                                DamageHelicopter(
                                    projectile.rocket ? kRocketHelicopterDamage
                                                      : scene.grenadeEnemyDamage * falloff,
                                    g_helicopterPosition);
                            }
                        }
                        if (g_stressTestMode && !g_secondaryHelicopterDead &&
                            g_helicopterModel) {
                            const float dx = g_secondaryHelicopterPosition.x - center.x;
                            const float dy = g_secondaryHelicopterPosition.y - center.y;
                            const float dz = g_secondaryHelicopterPosition.z - center.z;
                            const float distance = std::sqrt(dx*dx + dy*dy + dz*dz);
                            const float reach = scene.grenadeEnemyRadius + 5.0f;
                            if (distance < reach) {
                                const float falloff = 1.0f - distance / reach;
                                DamageSecondaryHelicopter(
                                    projectile.rocket ? kRocketHelicopterDamage
                                                      : scene.grenadeEnemyDamage * falloff,
                                    g_secondaryHelicopterPosition);
                            }
                        }
                        // Run after Bandit damage. Newly killed enemies have
                        // ragdolls now, so same blast launches their limbs too.
                        g_destruction.ApplyExplosion(center, scene.grenadeBlastRadius,
                                                     scene.grenadeDamage, scene.grenadeImpulse);
                        g_destruction.ApplyRagdollExplosion(
                            center, scene.grenadeEnemyRadius,
                            scene.grenadeEnemyImpulse);
                        projectile.active = false;
                        projectile.detonate = false;
                    }
                    continue;
                }
                if (!projectile.active) continue;
                XMFLOAT3 hit;
                // Collision uses the bullet's own small radius so it must
                // actually reach the surface before it registers -- the wider
                // damage radius only governs how far the fracture spreads once
                // the bullet has struck. Otherwise the wall breaks at a distance.
                const float bulletRadius = std::max(0.12f, scene.projectileScale * 0.5f);
                bool hitBandit = false;
                bool killedBandit = false;
                XMFLOAT3 banditHit = projectile.position;
                // Player shots damage any enemy. Hostile shots damage only the
                // enemy currently used as a human shield, never squadmates.
                if (g_banditLoaded && (!projectile.hostile || g_heldBandit)) {
                    for (auto& bandit : g_bandits) {
                        if (projectile.hostile && bandit.get() != g_heldBandit) continue;
                        if (bandit && bandit->Shoot(
                                projectile.previousPosition, projectile.position,
                                projectile.direction, bulletRadius, &banditHit, nullptr,
                                20.0f * projectile.damageMultiplier)) {
                            hitBandit = true;
                            killedBandit = bandit->Dead();
                            if (killedBandit && bandit.get() == g_heldBandit)
                                g_heldBandit = nullptr;
                            break;
                        }
                    }
                }
                if (hitBandit) {
                    const XMFLOAT3 normal(-projectile.direction.x, -projectile.direction.y,
                                          -projectile.direction.z);
                    scene.SpawnBloodBurst(banditHit, normal);
                    const float hitPitch =
                        g_fleshHitPitchMin + ((float)std::rand() / RAND_MAX) *
                        (g_fleshHitPitchMax - g_fleshHitPitchMin);
                    g_hitAudio.Play(0.72f * 0.3f, hitPitch);
                    if (!killedBandit && g_banditPainCooldown <= 0.0f) {
                        const float painPitch =
                            0.96f + ((float)std::rand() / RAND_MAX) * 0.08f;
                        g_banditHitVoiceAudio.Play(
                            BanditVoiceVolume(banditHit, 0.82f), painPitch);
                        g_banditPainCooldown = 0.45f;
                    }
                    PlayBanditDeathEvents();
                    projectile.active = false;
                    continue;
                }
                if (projectile.hostile && g_banditLoaded) {
                    bool blockedByBandit = false;
                    for (const auto& bandit : g_bandits) {
                        if (bandit && bandit->BlocksProjectile(
                                projectile.previousPosition, projectile.position,
                                bulletRadius)) {
                            blockedByBandit = true;
                            break;
                        }
                    }
                    if (blockedByBandit) {
                        projectile.active = false;
                        continue;
                    }
                }
                XMFLOAT3 helicopterHit;
                if (!projectile.hostile && HitHelicopterSegment(
                        projectile.previousPosition, projectile.position,
                        bulletRadius, helicopterHit)) {
                    const XMFLOAT3 normal(-projectile.direction.x,
                                          -projectile.direction.y,
                                          -projectile.direction.z);
                    scene.SpawnBulletImpact(helicopterHit, normal);
                    DamageHelicopter(34.0f * projectile.damageMultiplier, helicopterHit);
                    projectile.active = false;
                    continue;
                }
                if (!projectile.hostile && HitSecondaryHelicopterSegment(
                        projectile.previousPosition, projectile.position,
                        bulletRadius, helicopterHit)) {
                    const XMFLOAT3 normal(-projectile.direction.x,
                                          -projectile.direction.y,
                                          -projectile.direction.z);
                    scene.SpawnBulletImpact(helicopterHit, normal);
                    DamageSecondaryHelicopter(
                        34.0f * projectile.damageMultiplier, helicopterHit);
                    projectile.active = false;
                    continue;
                }
                size_t barrelIndex = 0;
                XMFLOAT3 barrelHit;
                if (HitExplosiveBarrelSegment(
                        projectile.previousPosition, projectile.position,
                        bulletRadius, barrelIndex, barrelHit)) {
                    ExplosiveBarrel& barrel = scene.explosiveBarrels[barrelIndex];
                    barrel.hits += static_cast<int>(std::ceil(projectile.damageMultiplier));
                    const XMFLOAT3 normal(-projectile.direction.x,
                                          -projectile.direction.y,
                                          -projectile.direction.z);
                    scene.SpawnBulletImpact(barrelHit, normal);
                    if (barrel.hits >= 4) {
                        DetonateBarrel(barrelIndex);
                    } else if (barrel.hits == 2 && !barrel.burning) {
                        barrel.burning = true;
                        barrel.fuse = 3.0f;
                        barrel.fireFxCooldown = 0.0f;
                    }
                    projectile.active = false;
                    continue;
                }
                if (g_destruction.HitTestSegment(projectile.previousPosition, projectile.position,
                                                 bulletRadius, hit)) {
                    std::cout << "Projectile hit wall at " << hit.x << ", "
                              << hit.y << ", " << hit.z << "\n";
                    g_destruction.ApplyRadialDamage(
                        hit, scene.destructionDamageRadius,
                        scene.destructionDamage * projectile.damageMultiplier);
                    g_destruction.ApplyImpulse(hit, projectile.direction,
                                               scene.destructionBulletImpulse *
                                                   projectile.damageMultiplier,
                                               scene.destructionDamageRadius);
                    // Impact FX: spark burst + hole decal. Surface normal is
                    // approximated as facing back along the bullet's travel.
                    const XMFLOAT3 normal(-projectile.direction.x,
                                          -projectile.direction.y,
                                          -projectile.direction.z);
                    // One small dust puff right at the hit; the bigger cloud
                    // comes from the actual fracture (DrainBreakPoints).
                    scene.SpawnSmokeBurst(hit, 0.3f, 0.1f);
                    projectile.active = false;
                } else if (!g_emptyLevelMode && g_water.ShootFloaters(projectile.previousPosition,
                                                 projectile.position, projectile.direction,
                                                 bulletRadius, scene.destructionBulletImpulse)) {
                    // Bullet knocked a crate floating in the pool.
                    const XMFLOAT3 normal(-projectile.direction.x,
                                          -projectile.direction.y,
                                          -projectile.direction.z);
                    scene.SpawnBulletImpact(projectile.position, normal);
                    projectile.active = false;
                } else if (XMFLOAT3 treeHit;
                           !g_emptyLevelMode && g_trees.Shoot(projectile.previousPosition, projectile.position,
                                         projectile.direction, bulletRadius,
                                         scene.treeDamagePerShot * projectile.damageMultiplier,
                                         treeHit)) {
                    // Chewed a palm trunk; enough rounds and it snaps and topples.
                    const XMFLOAT3 normal(-projectile.direction.x,
                                          -projectile.direction.y,
                                          -projectile.direction.z);
                    scene.SpawnBulletImpact(treeHit, normal);
                    scene.SpawnSmokeBurst(treeHit, 0.25f, 0.1f);
                    projectile.active = false;
                } else if (XMFLOAT3 waterHit;
                           !g_emptyLevelMode && g_water.ShootSurface(projectile.previousPosition,
                                                projectile.position, waterHit)) {
                    projectile.position = waterHit;
                    projectile.active = false;
                } else if (XMFLOAT3 oceanHit;
                           !g_emptyLevelMode && g_ocean.ShootSurface(projectile.previousPosition,
                                                projectile.position, oceanHit, 0.28f)) {
                    projectile.position = oceanHit;
                    projectile.active = false;
                } else if (XMFLOAT3 terrainHit;
                           HitTerrainSegment(projectile.previousPosition,
                                             projectile.position,
                                             bulletRadius, terrainHit)) {
                    projectile.position = terrainHit;
                    projectile.active = false;
                } else if (projectile.hostile) {
                    // Player is tested last. Any world or character mesh in
                    // front consumes the shot first, preventing wall penetration.
                    scene.HitPlayerProjectile(projectile);
                }
            }
        }
        if (gameScreen == GameScreen::Level1 && !g_emptyLevelMode && g_banditLoaded &&
            !pendingTurretGunnerRespawn &&
            LiveBanditCount() == 0 && g_helicopterDead &&
            (!g_stressTestMode || g_secondaryHelicopterDead))
            OpenWinScreen();
        }
        }

        // ?? begin frame ??
        try { BeginFrame(); }
        catch (const std::exception& e) {
            std::ofstream("engine_runtime_error.log", std::ios::app)
                << "BeginFrame: " << e.what() << '\n';
            std::cerr << "BeginFrame: " << e.what() << "\n"; break;
        }
        occlusionDepth.FinalizeCapture(g_dx12.commandList.Get());
        g_profiler.BeginGpuFrame(g_dx12.frameIndex, g_dx12.commandList.Get());

        float cc[4] = { scene.clearColor.x, scene.clearColor.y, scene.clearColor.z, 1.0f };
        if (visibilityTestPending && gameScreen == GameScreen::Level1 &&
            !levelLoadingActive) {
            ++visibilityTestForwardFrames;
            if (visibilityTestForwardFrames == 1) {
                std::ofstream("visibility_smoke.log", std::ios::app)
                    << "forward-active\n";
            }
            if (!visibilityBenchmark && !visibilityForwardOnly &&
                visibilityTestForwardFrames >= 120) {
                scene.useVisibilityBuffer = true;
                visibilityTestPending = false;
                std::cerr << "VISIBILITY_TEST: toggled after 120 forward frames\n";
                std::ofstream("visibility_smoke.log", std::ios::app)
                    << "toggled\n";
            }
        }
        const bool usingRaytracing =
            scene.useRaytracing && g_rt.initialized;
        const bool usingVisibility =
            !usingRaytracing && scene.useVisibilityBuffer && visBuffer.initialized;
        static bool visibilityWasActive = false;
        if (usingVisibility != visibilityWasActive) {
            visBuffer.InvalidateTemporalHistory();
            visibilityWasActive = usingVisibility;
        }
        scene.temporalJitterPixels = usingVisibility
            ? visBuffer.GetTemporalJitterPixels()
            : XMFLOAT2(0.0f, 0.0f);
        const bool visibilityDebugActive =
            usingVisibility && visBuffer.debugViewMode != 0;
        // Validation is a cross-renderer comparison mode. Keep both sides on
        // the same baseline while M toggles ownership: no MSAA, fog, or FXAA.
        const bool visibilityParityValidation =
            !usingRaytracing && visBuffer.initialized && visBuffer.validationMode;
        const bool commonHDRValidationTarget =
            usingVisibility || visibilityParityValidation;
        const bool msaaActive =
            scene.enableMSAA && msaa.initialized &&
            !usingRaytracing && !usingVisibility && !visibilityParityValidation;
        const bool grassMSAAActive =
            usingVisibility && !visibilityDebugActive &&
            scene.enableGrassMSAA && grassMSAA.initialized &&
            mainShader.GetHDRMSAAGrassPipelineState() != nullptr;
        mainShader.SetMSAAEnabled(msaaActive);
        g_meshShader.SetMSAAEnabled(msaaActive);
        g_terrain.SetMSAAEnabled(msaaActive);
        skyRenderer.SetMSAAEnabled(msaaActive);
        if (msaaActive) msaa.BindAndClear(cc);
        else ClearRenderTarget(cc);
        {
            ProfilerDX12::Scope profile(g_profiler, "Sky", g_dx12.commandList.Get());
            if (commonHDRValidationTarget) {
                visBuffer.BeginHDRBackground(g_dx12.commandList.Get());
                skyRenderer.SetHDRTargetEnabled(true);
            }
            skyRenderer.Render(scene.camera, scene.EffectiveCameraFOV(), scene.lightPos, now);
            if (commonHDRValidationTarget) {
                skyRenderer.SetHDRTargetEnabled(false);
                visBuffer.EndHDRBackground(g_dx12.commandList.Get());
            }
        }

        mainShader.BeginFrame();
        mainShader.SetPalmWindFrame(g_trees.GetWindFrame());
        g_meshShader.BeginFrame();
        hzbCaptureActive = IsSceneScreen() &&
            !levelLoadingActive && !usingRaytracing && !msaaActive &&
            (g_useMeshShader || usingVisibility);
        if (!hzbCaptureActive) occlusionDepth.InvalidateCameraHistory();
        mainShader.SetPreviousViewProjection(previousHZBViewProjection);
        const bool hzbHistoryUsable = hzbCaptureActive &&
            occlusionDepth.CanUseHistory(
                scene.camera.Position, scene.camera.Front) &&
            !msaaUsedLastFrame;
        g_meshShader.SetOcclusionDepth(
            occlusionDepth.GetGPUHandle(),
            hzbHistoryUsable,
            occlusionDepth.GetMipCount());

        if (IsSceneScreen() && levelLoadingActive) {
        if (levelLoadStage == LevelLoadStage::WorldAssets) {
            LoadFloorMudMaterial();
            if (!g_emptyLevelMode) {
                std::cout << "Loading models/h2.glb...\n";
                crateModel = GLBImporter::LoadGLB(
                    "models/h2.glb", g_dx12.device, g_dx12.commandList);
            }
            AdvanceLevelLoading(LevelLoadStage::Destruction,
                "Destruction geometry and Blast actors",
                "procedural house chunks + roof",
                g_emptyLevelMode || crateModel != nullptr);
        } else if (levelLoadStage == LevelLoadStage::Destruction) {
            if (!levelDestructionLoadInFlight) {
                if (g_destruction.IsInitialized()) WaitForGPU();
                // Build authored scene nodes on the render thread because their
                // texture uploads record into this frame's direct command list.
                auto houseTemplate = CreateDestructibleWallModel();
                ApplyHouseTextures(houseTemplate, g_dx12.device.Get(),
                    g_dx12.commandList.Get());
                AppendRoofChunksToDestructionModel(houseTemplate);
                g_houseTemplate = CloneSceneTree(houseTemplate);
                normalWallModel = CloneSceneTree(houseTemplate);
                stressWallModel = CloneSceneTree(houseTemplate);
                const bool customLayout = g_customLevelMode;
                g_customLevelMode = false;
                ArrangeHousesInCross(normalWallModel, false);
                ArrangeHousesInCross(stressWallModel, true);
                g_customLevelMode = customLayout;
                if (customLayout) {
                    wallModel = CloneSceneTree(houseTemplate);
                    ArrangeHousesInCross(wallModel, false);
                } else wallModel = g_stressTestMode ? stressWallModel : normalWallModel;

                // Chunk meshlets, Blast actors, Box3D bodies, and merged actor
                // geometry are CPU/device creation work. D3D12 device creation
                // is thread-safe; static uploads enter the mutex-protected queue.
                // Keeping it off the render thread makes resource telemetry and
                // the loading screen update while thousands of chunks build.
                const std::shared_ptr<SceneNode> destructionModel = wallModel;
                ComPtr<ID3D12Device> destructionDevice = g_dx12.device;
                levelLoadingLabel =
                    "Build meshlet SRVs, Blast actors and depth-only batch";
                levelLoadingAsset = g_stressTestMode
                    ? "DestructionDX12: stressWallModel"
                    : "DestructionDX12: normalWallModel";
                levelDestructionLoadFuture = std::async(std::launch::async,
                    [destructionModel, destructionDevice]() {
                        return g_destruction.Initialize(destructionModel,
                            destructionDevice.Get(), 1, 1, 1);
                    });
                levelDestructionLoadInFlight = true;
            } else if (levelDestructionLoadFuture.wait_for(
                    std::chrono::seconds(0)) == std::future_status::ready) {
                const bool initialized = levelDestructionLoadFuture.get();
                levelDestructionLoadInFlight = false;
                if (g_emptyLevelMode && initialized) {
                    auto tp = CurrentTerrainParams();
                    tp.heightScale = scene.terrainHeightScale;
                    g_destruction.SetTerrainSampler([tp](float x, float z) {
                        return TerrainRendererDX12::HeightAt(tp, x, z);
                    });
                }
                AdvanceLevelLoading(
                    g_emptyLevelMode ? LevelLoadStage::Weapons
                                     : LevelLoadStage::Environment,
                    g_emptyLevelMode ? "Player weapon"
                                     : "Water, ocean, trees and scalable environment",
                    g_emptyLevelMode ? "AK47"
                        : (g_stressTestMode ? "stress environment"
                                            : "level environment"),
                    initialized);
            }
        } else if (levelLoadStage == LevelLoadStage::Environment) {
            // Pool of water beside the house (on the clear -X side) with a
            // handful of wooden crates dropped in to bob on the surface.
            // Pool sunk into a dug-out terrain basin (see TerrainHeight): the
            // surface spans the whole basin (out to the rim) and sits just below
            // ground level, so it reads as a filled hole in the ground. The pool
            // gets the terrain height sampler so its rigid floor is the actual
            // sloped basin -- crates and debris collide with the terrain.
            {
                auto tp = CurrentTerrainParams();
                tp.heightScale = scene.terrainHeightScale;
                auto terrainSampler = [tp](float x, float z) {
                    return TerrainRendererDX12::HeightAt(tp, x, z);
                };
                g_water.Initialize({ -22.0f, -1.85f, -20.0f }, { 14.0f, 2.7f, 14.0f },
                                   terrainSampler);

                // The sea. A single big wave surface at y = 0 (sea level), spanning
                // far past the terrain grid so it runs to the horizon in every
                // direction. No terrain sampler: nothing floats in it, so it gets
                // the cheap flat tank rather than a heightfield basin.
                constexpr float kSeaSpan = 600.0f;
                constexpr float kSeaDepth = 12.0f;
                // Ocean swell is low-frequency. 64 cells preserve it while
                // cutting the old 98,304-index draw and CPU writes by 75%.
                g_ocean.SetGridResolution(64);
                // The grid costs 4k CPU sine evals per write; the swell
                // reads the same at half rate. The pool stays at full rate for
                // its splash ripples.
                g_ocean.SetUpdateInterval(2);
                g_ocean.Initialize({ 0.0f, -kSeaDepth * 0.5f, 0.0f },
                                   { kSeaSpan, kSeaDepth, kSeaSpan });
                // House debris collides with the real terrain, not a flat plane.
                g_destruction.SetTerrainSampler(terrainSampler);
                // Debris/ragdolls splash the pool when they break the surface.
                g_destruction.SetSplashCallback([](float x, float z, float s) {
                    g_water.Splash(x, z, s);
                });
                g_destruction.InitializeVehicle(g_customLevelMode
                    ? g_primaryHumveeSpawn : XMFLOAT3{ 0.0f, 3.45f, 0.0f },
                    g_customLevelMode ? XMConvertToRadians(g_primaryHumveeYaw) : 0.0f);

                // Palm grove ringing the pool. Shoot through a trunk and the tree
                // snaps at that height and topples away from you.
                g_trees.SetTerrainSampler(terrainSampler);
                ResetPalmTrees();

                LoadDandelionModel();
                RebuildScalableEnvironment();
            }
            // Same pool AABB for the destruction sim so house debris shoved into
            // the water floats too (surface at max.y).
            g_destruction.SetWaterRegion({ -29.0f, -3.2f, -27.0f },
                                         { -15.0f, -0.5f, -13.0f });
            if (crateModel) {
                if (auto merged = GLBImporter::MergeSceneByMaterial(crateModel, g_dx12.device)) {
                    crateModel = merged;
                }
                crateModel->UpdateGlobalTransform(crateModel->localTransform);
                crateShadowModel = GLBImporter::MergeSceneForDepth(
                    crateModel, g_dx12.device);
                size_t materialDraws = crateModel->mesh ? crateModel->mesh->primitives.size() : 0;
                std::cout << "h2 model loaded: " << materialDraws
                          << " material draw(s)\n";
            } else {
                std::cerr << "Failed to load h2.glb, falling back to procedural cube\n";
            }

            AdvanceLevelLoading(LevelLoadStage::Weapons,
                "Weapon and explosive barrel",
                "AK47 + models/Barrel Explosive/barrel.FBX");
        } else if (levelLoadStage == LevelLoadStage::Weapons) {

            // The AK47 view model. Loaded here so its texture uploads land in the
            // same command list the flush below submits.
            GunModel::Load();

            if (g_emptyLevelMode) {
                AdvanceLevelLoading(LevelLoadStage::GPUFinalize,
                    "Drain graphics uploads and generate texture mips",
                    "direct queue + compute queue", GunModel::Loaded());
            } else {
                // Authored explosive barrel. Source mesh is 2.08 m high; 0.72 keeps
                // its in-game size aligned with existing 1.5 m gameplay collision.
                g_explosiveBarrelModel = FBXImporter::Load(
                    "models/Barrel Explosive/barrel.FBX",
                    g_dx12.device, g_dx12.commandList, 0.72f, false, true);
                if (g_explosiveBarrelModel) {
                    // Asset provides only diffuse + normal maps. Treating its
                    // unmasked surface as smooth metal creates broad white IBL
                    // glare over painted/rusted areas. Keep it dielectric and
                    // substantially rough, without changing other FBX assets.
                    const auto fixBarrelMaterial = [&](const auto& self,
                                                       const std::shared_ptr<SceneNode>& node)
                        -> void {
                        if (!node) return;
                        if (node->mesh) for (auto& primitive : node->mesh->primitives) {
                            if (!primitive.material) continue;
                            primitive.material->metallicFactor = 0.0f;
                            primitive.material->roughnessFactor = 0.88f;
                            primitive.material->metallicRoughnessTexture.Reset();
                            primitive.material->roughnessOnlyTexture = false;
                        }
                        for (const auto& child : node->children) self(self, child);
                    };
                    fixBarrelMaterial(fixBarrelMaterial, g_explosiveBarrelModel);
                    g_explosiveBarrelShadowModel = GLBImporter::MergeSceneForDepth(
                        g_explosiveBarrelModel, g_dx12.device);
                    std::cout << "Explosive barrel FBX ready\n";
                } else
                    std::cerr << "Explosive barrel FBX failed; using procedural fallback\n";

                AdvanceLevelLoading(LevelLoadStage::Humvee,
                    "Humvee model, bounds and shadow mesh",
                    "models/Humvee/humvee.fbx",
                    g_explosiveBarrelModel != nullptr);
            }
        } else if (levelLoadStage == LevelLoadStage::Humvee) {
            g_humveeModel = FBXImporter::Load(
                "models/Humvee/humvee.fbx",
                g_dx12.device, g_dx12.commandList, 1.0f, false, true);
            if (g_humveeModel) {
                for (const auto& child : g_humveeModel->children)
                    if (child && child->name == "HumveeTurret") {
                        g_humveeTurretNode = child;
                        break;
                }
                ConfigureHumveeBounds();
                g_humveeShadowModel = GLBImporter::MergeSceneForDepth(
                    g_humveeModel, g_dx12.device);
                std::cout << "Humvee FBX ready at center\n";
            } else {
                std::cerr << "Humvee FBX failed to load\n";
            }

            AdvanceLevelLoading(LevelLoadStage::Helicopter,
                "OH-1 import, geometry LOD and rotor setup",
                "models/OH-1_fbx/OH-1.fbx", g_humveeModel != nullptr);
        } else if (levelLoadStage == LevelLoadStage::Helicopter) {

            const std::string helicopterModelPath =
                ResolveTexturePath("models/OH-1_fbx/OH-1.fbx");
            std::cout << "OH-1 asset: " << helicopterModelPath << "\n";
            g_helicopterModel = FBXImporter::Load(
                helicopterModelPath,
                g_dx12.device, g_dx12.commandList, 1.0f, false, true, true);
            if (g_helicopterModel) {
                for (const auto& child : g_helicopterModel->children) {
                    if (!child) continue;
                    if (child->name == "OH1MainRotor")
                        g_helicopterMainRotorNode = child;
                    else if (child->name == "OH1TailRotor")
                        g_helicopterTailRotorNode = child;
                }
                ConfigureHelicopterBounds();
                std::cout << "OH-1 helicopter ready above center\n";
            } else {
                std::cerr << "OH-1 helicopter FBX failed to load\n";
            }
            if (ApplyDarkGreenToHumvee())
                std::cout << "Humvee dark green material ready\n";
            else
                std::cerr << "Humvee dark green material failed\n";

            AdvanceLevelLoading(LevelLoadStage::BanditModel,
                "Bandit mesh, skeleton, clips and physics asset",
                "models/MilitaryMercenaryBandit/SK_Bandit.FBX",
                g_helicopterModel != nullptr);
        } else if (levelLoadStage == LevelLoadStage::BanditModel) {

            // Skinned Bandit enemy: mesh + walk/idle/run clips. Texture uploads
            // ride the same command list flushed just below.
            {
                const std::string banditDir = "models/MilitaryMercenaryBandit/";
                const std::string animDir = banditDir + "Animations/Demo/";
                std::vector<std::string> clips = {
                    animDir + "ThirdPersonIdle.FBX",
                    animDir + "ThirdPersonWalk.FBX",
                    animDir + "ThirdPersonRun.FBX",
                };
                SkinnedModel bm = SkinnedFBXImporter::Load(
                    banditDir + "SK_Bandit.FBX", clips, g_dx12.device, g_dx12.commandList);
                bm.ragdoll = T3DPhysicsAsset::Load(banditDir + "Phy_Bandit_PhysicsAsset.T3D");
                if (bm.valid) {
                    g_banditModel = std::move(bm);
                } else {
                    std::cerr << "Bandit squad failed to load\n";
                }

            }
            AdvanceLevelLoading(LevelLoadStage::BanditSpawn,
                "Spawn squad and turret gunners",
                g_stressTestMode ? "stress squad: enemies + 2 gunners"
                                 : "level squad: enemies + gunner",
                g_banditModel.valid);
        } else if (levelLoadStage == LevelLoadStage::BanditSpawn) {
            if (g_banditModel.valid) {
                for (size_t i = 0; i < ActiveBanditSlotCount(); ++i)
                    if (!SpawnBandit()) break;
                SpawnHumveeTurretGunner(0);
                if (g_stressTestMode) SpawnHumveeTurretGunner(1);
                g_banditLoaded = true;
                std::cout << "Bandit squad ready: " << LiveBanditCount()
                          << " live enemies\n";
            }
            AdvanceLevelLoading(LevelLoadStage::GPUFinalize,
                "Drain graphics uploads and generate texture mips",
                "direct queue + compute queue", g_banditLoaded);
        } else if (levelLoadStage == LevelLoadStage::GPUFinalize) {

            // Flush the load/mip-generation commands now and print any D3D12
            // validation errors before continuing, so mip-related bugs surface
            // immediately instead of silently corrupting later frames.
            // Earlier stages were submitted as ordinary loading-screen frames.
            // Drain them before mip generation and staging-resource release.
            WaitForGPU();
            g_mipGen.FlushPending();
            // Mip handoff is submitted by FlushPending. Wait once, then free
            // texture staging resources retained by imported scene materials.
            WaitForGPU();
            AdvanceLevelLoading(LevelLoadStage::ReleaseUploads,
                "Release staging heaps and validate DX12 device",
                "material upload heaps");
        } else if (levelLoadStage == LevelLoadStage::ReleaseUploads) {
            ReleaseMaterialUploadHeaps(g_helicopterModel);
            ReleaseMaterialUploadHeaps(g_humveeModel);
            ReleaseMaterialUploadHeaps(g_explosiveBarrelModel);
            ReleaseMaterialUploadHeaps(g_dandelionModel);
            ReleaseMaterialUploadHeaps(g_banditModel.node);
            for (const auto& material : g_banditModel.materialKeepAlive)
                if (material) material->uploadHeaps.clear();
            ReleaseMaterialUploadHeaps(crateModel);
            ReleaseMaterialUploadHeaps(wallModel);
            DumpDX12DebugMessages();
            // First Level 1 load is complete. Start timing after load/GPU waits,
            // not from menu click.
            if (g_emptyLevelMode) {
                emptyLevelAssetsLoaded = true;
            } else {
                fullLevelAssetsLoaded = true;
                emptyLevelAssetsLoaded = true;
            }
            CompleteLevelLoading(g_dx12.device &&
                g_dx12.device->GetDeviceRemovedReason() == S_OK);
            levelTimerRunning = true;
            lastTime = gameTimer.GetElapsed();
        }
        }

        // ?? render ??
        // Main menu draws only sky plus UI. Level geometry, weapons, enemies,
        // shadows, and particles are never submitted behind the menu.
        // Loaders and destruction rebuilds enqueue immutable geometry in DEFAULT
        // heaps. Record all copies before any pass consumes those resources.
        const uint32_t submittedUploads =
            FlushStaticBufferUploadsDX12(g_dx12.commandList.Get());
        if (levelLoadingActive) {
            levelLoadingLastSubmittedUploads = submittedUploads;
            levelLoadingSubmittedUploads += submittedUploads;
        }

        XMMATRIX fogLightSpace = XMMatrixIdentity();
        ID3D12Resource* fogShadowResource = nullptr;
        bool renderedScene = false;
        if (IsSceneScreen() && !levelLoadingActive &&
            usingRaytracing) {
            ProfilerDX12::Scope profile(g_profiler, "Raytracing", g_dx12.commandList.Get());
            RenderRaytracing(scene);
        } else if (IsSceneScreen() && !levelLoadingActive &&
                   usingVisibility) {
            ProfilerDX12::Scope profile(g_profiler, "Visibility Buffer", g_dx12.commandList.Get());
            XMMATRIX lightSpace = XMMatrixIdentity();
            ID3D12Resource* shadowResource = nullptr;
            if (scene.enableShadows && shadowMap.initialized && scene.lightType == 0) {
                lightSpace = shadowMap.Render(
                    scene, geo, (!g_emptyLevelMode && g_showH2Model)
                        ? (crateShadowModel ? crateShadowModel : crateModel)
                        : nullptr,
                    (!g_emptyLevelMode && g_banditLoaded) ? &g_bandits : nullptr);
                shadowResource = shadowMap.GetResource();
            }
            RenderIdTech(scene, mainShader, visBuffer, geo, packed,
                lightSpace, shadowResource, &occlusionDepth,
                hzbHistoryUsable, previousHZBViewProjection, floorMaterial,
                (!g_emptyLevelMode && g_showH2Model) ? crateModel : nullptr);
            fogLightSpace = lightSpace;
            fogShadowResource = shadowResource;
            renderedScene = !visibilityDebugActive;
            mainShader.SetHDRTargetEnabled(true);
            g_meshShader.SetHDRTargetEnabled(true);
            g_terrain.SetHDRTargetEnabled(true);
            if (!visibilityDebugActive) {
                ProfilerDX12::Scope extensions(
                    g_profiler, "Forward Extensions", g_dx12.commandList.Get());
                RenderForward(scene, mainShader, geo, crateModel, floorMaterial,
                    lightSpace, shadowResource, true, !grassMSAAActive);
            }
        } else if (IsSceneScreen() && !levelLoadingActive) {
            XMMATRIX lightSpace = XMMatrixIdentity();
            ID3D12Resource* shadowResource = nullptr;
            if (scene.enableShadows && shadowMap.initialized && scene.lightType == 0) {
                ProfilerDX12::Scope profile(g_profiler, "Shadow", g_dx12.commandList.Get());
                lightSpace = shadowMap.Render(
                    scene, geo, (!g_emptyLevelMode && g_showH2Model)
                        ? (crateShadowModel ? crateShadowModel : crateModel)
                        : nullptr,
                    (!g_emptyLevelMode && g_banditLoaded) ? &g_bandits : nullptr);
                shadowResource = shadowMap.GetResource();
            }
            fogLightSpace = lightSpace;
            fogShadowResource = shadowResource;
            renderedScene = true;
            if (msaaActive) msaa.Bind();
            if (visibilityParityValidation) {
                visBuffer.BeginForwardExtensions(g_dx12.commandList.Get());
                mainShader.SetHDRTargetEnabled(true);
                g_meshShader.SetHDRTargetEnabled(true);
                g_terrain.SetHDRTargetEnabled(true);
            }
            {
                ProfilerDX12::Scope profile(g_profiler, "Forward", g_dx12.commandList.Get());
                RenderForward(scene, mainShader, geo, crateModel, floorMaterial, lightSpace, shadowResource);
            }
        }

        // Hybrid visibility and pure forward both establish global forward
        // resources before skinned/transparent extension passes.
        if (renderedScene && !g_emptyLevelMode && g_banditLoaded) {
            ProfilerDX12::Scope profile(
                g_profiler, "Bandits", g_dx12.commandList.Get());
            for (auto& bandit : g_bandits) {
                if (!bandit) continue;
                bandit->Draw(mainShader, scene.GetViewMatrix(),
                             scene.GetProjectionMatrix(), fogLightSpace);
                if (bandit->HasGunPose() && GunModel::Loaded()) {
                    mainShader.Use(false);
                    DrawMeshAt(GunModel::Mesh(), mainShader,
                        bandit->GunWorldMatrix(), scene.GetViewMatrix(),
                        scene.GetProjectionMatrix(), fogLightSpace, true);
                }
            }
            mainShader.Use(scene.wireframeMode);
        }
        if (renderedScene) {
            ProfilerDX12::Scope profile(
                g_profiler, "Impact Particles", g_dx12.commandList.Get());
            RenderImpactBillboards(scene, mainShader, geo, fogLightSpace);
        }

        if (renderedScene && grassMSAAActive) {
            ProfilerDX12::Scope profile(
                g_profiler, "Grass 4x MSAA", g_dx12.commandList.Get());
            grassMSAA.Begin(g_dx12.commandList.Get());
            RenderGrassForward(scene, mainShader, scene.GetViewMatrix(),
                scene.GetProjectionMatrix(), fogLightSpace, fogShadowResource,
                mainShader.GetHDRMSAAGrassPipelineState());

            // Resolve the independently sampled grass against opaque scene
            // depth, then reopen HDR for shared AO and fog.
            visBuffer.EndForwardExtensions(g_dx12.commandList.Get());
            grassMSAA.Composite(g_dx12.commandList.Get(),
                visBuffer.GetOutputResource(), visBuffer.GetMotionResource(),
                g_dx12.depthStencilBuffer.Get());
            visBuffer.BeginForwardExtensions(g_dx12.commandList.Get());
        }

        if (msaaActive) {
            ProfilerDX12::Scope profile(
                g_profiler, "MSAA Resolve", g_dx12.commandList.Get());
            msaa.ResolveToBackBuffer();
        }

        if (renderedScene && scene.enableAmbientOcclusion &&
            screenSpaceAO.initialized) {
            ProfilerDX12::Scope profile(
                g_profiler, "GTAO + Contact Shadows", g_dx12.commandList.Get());
            screenSpaceAO.Render(scene,
                msaaActive ? msaa.GetDepthResource() : g_dx12.depthStencilBuffer.Get(),
                msaaActive,
                commonHDRValidationTarget ? visBuffer.GetOutputRTV()
                                          : D3D12_CPU_DESCRIPTOR_HANDLE{},
                commonHDRValidationTarget,
                usingVisibility ? visBuffer.GetVisibilityDepthResource() : nullptr);
        }

        const bool visibilityValidation = visibilityParityValidation;
        // Shared post-render pass. Normal forward composites to the resolved
        // swapchain target. Visibility and parity-forward composite to HDR.
        if (renderedScene && scene.enableVolumetricFog && volumetricFog.initialized) {
            ProfilerDX12::Scope profile(
                g_profiler, "Volumetric Fog", g_dx12.commandList.Get());
            volumetricFog.Render(scene, fogLightSpace, fogShadowResource,
                msaaActive ? msaa.GetDepthResource() : g_dx12.depthStencilBuffer.Get(),
                msaaActive,
                commonHDRValidationTarget ? visBuffer.GetOutputRTV()
                                          : D3D12_CPU_DESCRIPTOR_HANDLE{},
                commonHDRValidationTarget);
        }

        if (commonHDRValidationTarget) {
            ProfilerDX12::Scope profile(
                g_profiler, "Common HDR Post", g_dx12.commandList.Get());
            visBuffer.EndForwardExtensions(g_dx12.commandList.Get());
            if (usingVisibility && !visBuffer.validationMode)
                visBuffer.UpdateExposure(g_dx12.commandList.Get());
            // TAA reprojection remains valid during ordinary camera movement;
            // HZB history is intentionally stricter and must not gate it.
            visBuffer.PostProcess(g_dx12.commandList.Get(), usingVisibility);
            visBuffer.CopyToBackBuffer(g_dx12.commandList.Get());
            if (usingVisibility)
                visBuffer.TransitionBuffersForUpload(g_dx12.commandList.Get());
            mainShader.SetHDRTargetEnabled(false);
            g_meshShader.SetHDRTargetEnabled(false);
            g_terrain.SetHDRTargetEnabled(false);
        }

        // Preserve this frame's depth for next-frame amplification-shader
        // occlusion tests before UI rendering changes descriptor heaps.
        if (hzbCaptureActive) {
            ProfilerDX12::Scope profile(g_profiler, "Occlusion Depth", g_dx12.commandList.Get());
            occlusionDepth.PrepareCapture(g_dx12.commandList.Get());
        }

        if (scene.enableFXAA && fxaa.initialized && !visibilityValidation) {
            ProfilerDX12::Scope profile(g_profiler, "FXAA", g_dx12.commandList.Get());
            fxaa.Apply(g_dx12.commandList.Get());
        }

        // Ensure ImGui renders to the swapchain backbuffer (VB path changes OM target)
        {
            D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = GetCPUDescriptorHandle(
                g_dx12.rtvHeap.Get(), g_dx12.rtvDescriptorSize, g_dx12.frameIndex);
            D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = g_dx12.dsvHeap->GetCPUDescriptorHandleForHeapStart();
            g_dx12.commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);
            g_dx12.commandList->RSSetViewports(1, &g_dx12.viewport);
            g_dx12.commandList->RSSetScissorRects(1, &g_dx12.scissorRect);
        }

        // ?? ImGui ??
        {
        ProfilerDX12::Scope profile(g_profiler, "ImGui", g_dx12.commandList.Get());
        g_forwardDrawCalls = mainShader.currentDrawCall;
        g_shadowDrawCalls = (!usingRaytracing && !usingVisibility &&
            scene.enableShadows && shadowMap.initialized && scene.lightType == 0)
            ? shadowMap.depthShader.currentDrawCall : 0;
        g_shadowBatches = g_shadowDrawCalls
            ? shadowMap.depthShader.batchesThisFrame : 0;
        g_shadowBatchInstances = g_shadowDrawCalls
            ? shadowMap.depthShader.instancesThisFrame : 0;
        g_visibilityDrawCalls = usingVisibility ? visBuffer.currentDrawCall : 0;
        if (visibilitySmokeEnabled && usingVisibility &&
            !visibilitySmokeReported) {
            std::ofstream("visibility_smoke.log", std::ios::app)
                << "active draws=" << visBuffer.currentDrawCall
                << " materials=" << visBuffer.materialCount
                << " textures=" << visBuffer.materialTextureCount
                << " vertices=" << visBuffer.persistentVertexCount
                << " indices=" << visBuffer.persistentIndexCount
                << " debug=" << visBuffer.debugViewMode
                << " validation=" << (visBuffer.validationMode ? 1 : 0)
                << '\n';
            visibilitySmokeReported = true;
        }
        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        if (levelLoadingActive) {
            RenderLoadingScreen();
        } else if (gameScreen == GameScreen::MainMenu) {
            RenderMainMenu(hwnd);
        } else if (gameScreen == GameScreen::WinScreen) {
            RenderWinScreen(hwnd);
        } else if (gameScreen == GameScreen::LevelEditor) {
            const LevelEditorActions actions = g_levelEditor.Render(scene.camera,
                scene.GetViewMatrix(), scene.GetProjectionMatrix(),
                [](float x, float z) {
                    if (!scene.useMeshTerrain || !g_terrain.supported) return 0.0f;
                    auto params = CurrentTerrainParams();
                    params.heightScale = scene.terrainHeightScale;
                    return TerrainRendererDX12::HeightAt(params, x, z);
                });
            pendingEditorBeginPlay = actions.beginPlay;
            pendingEditorStopPlay = actions.stopPlay;
            pendingEditorReturnToMenu = actions.returnToMenu;
            if (g_levelEditor.IsPlaying()) RenderPlayerHUD(scene);
        } else {
            RenderPlayerHUD(scene);
            if (!scene.playerGodMode && scene.playerHealth <= 0.0f) {
                RenderDeathScreen(hwnd);
            } else {
                if (showUI) RenderUI(scene, visBuffer);
                DrawDestructionDebug(scene);
            }
        }
        ImGui::Render();

        ID3D12DescriptorHeap* heaps[] = { imguiSrvHeap.Get() };
        g_dx12.commandList->SetDescriptorHeaps(1, heaps);
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_dx12.commandList.Get());
        }

        // ?? end frame ??
        g_profiler.EndGpuFrame(g_dx12.commandList.Get());
        try { EndFrame(); }
        catch (const std::exception& e) {
            const HRESULT removedReason = g_dx12.device
                ? g_dx12.device->GetDeviceRemovedReason() : S_OK;
            g_profiler.EndCpuFrame();
            std::cerr << "EndFrame: " << e.what()
                      << " deviceRemovedReason=0x" << std::hex
                      << static_cast<unsigned long>(removedReason) << std::dec << "\n";
            std::ofstream("engine_runtime_error.log", std::ios::app)
                << "EndFrame: " << e.what() << " deviceRemovedReason=0x"
                << std::hex << static_cast<unsigned long>(removedReason)
                << std::dec << '\n';
            DumpDX12DebugMessages();
            break;
        }
        occlusionDepth.SubmitCopy();
        if (hzbCaptureActive)
            previousHZBViewProjection =
                scene.GetViewMatrix() * scene.GetProjectionMatrix();
        msaaUsedLastFrame = msaaActive;
        g_profiler.EndCpuFrame();
        if (visibilitySmokeEnabled && ++visibilityCpuReportFrames >= 300) {
            visibilityCpuReportFrames = 0;
            std::ofstream cpuLog("visibility_cpu.log", std::ios::trunc);
            cpuLog << "cpu_frame_ms=" << g_profiler.CpuFrameMs() << '\n';
            for (const ProfilerSampleDX12& sample : g_profiler.CpuSamples())
                cpuLog << sample.name << '=' << sample.milliseconds << '\n';
            cpuLog << "visibility_draws=" << g_visibilityDrawCalls << '\n'
                   << "forward_draws=" << g_forwardDrawCalls << '\n'
                   << "destruction_batches="
                   << g_destruction.GetRenderBatches().size() << '\n'
                   << "destruction_fallbacks="
                   << g_destruction.GetRenderItems().size() << '\n'
                   << "destruction_awake="
                   << g_destruction.GetAwakeActorCount() << '\n'
                   << "destruction_low_motion="
                   << g_destruction.GetLowMotionActorCount() << '\n'
                   << "destruction_spatial_batches="
                   << g_destruction.GetSpatialBatchCount() << '\n';
        }

        if (visibilityBenchmark && !visibilityBenchmarkComplete &&
            gameScreen == GameScreen::Level1 && !levelLoadingActive) {
            const double gpuMs = g_profiler.GpuFrameMs();
            if (!usingVisibility) {
                if (visibilityTestForwardFrames > 120 && gpuMs > 0.0) {
                    visibilityBenchmarkForwardMs += gpuMs;
                    ++visibilityBenchmarkForwardSamples;
                }
                if (visibilityBenchmarkForwardSamples >= 300) {
                    const double average = visibilityBenchmarkForwardMs /
                        visibilityBenchmarkForwardSamples;
                    std::ofstream("visibility_benchmark.log", std::ios::trunc)
                        << "forward_gpu_ms=" << average << '\n';
                    scene.useVisibilityBuffer = true;
                    visibilityTestPending = false;
                    visibilityBenchmarkVBFrames = 0;
                }
            } else {
                ++visibilityBenchmarkVBFrames;
                if (visibilityBenchmarkVBFrames > 120 && gpuMs > 0.0) {
                    visibilityBenchmarkVBMs += gpuMs;
                    ++visibilityBenchmarkVBSamples;
                }
                if (visibilityBenchmarkVBSamples >= 300) {
                    const double forwardAverage = visibilityBenchmarkForwardMs /
                        visibilityBenchmarkForwardSamples;
                    const double vbAverage = visibilityBenchmarkVBMs /
                        visibilityBenchmarkVBSamples;
                    const bool keepVisibility = vbAverage < forwardAverage;
                    std::ofstream log("visibility_benchmark.log", std::ios::app);
                    log << "visibility_gpu_ms=" << vbAverage << '\n'
                        << "delta_gpu_ms=" << (vbAverage - forwardAverage) << '\n'
                        << "decision=" << (keepVisibility ? "keep" : "disable")
                        << '\n';
                    scene.useVisibilityBuffer = keepVisibility;
                    visibilityBenchmarkComplete = true;
                }
            }
        }
    }

    WaitForGPU();
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_destruction.Shutdown();
    g_banditHitVoiceAudio.Shutdown();
    g_banditDeathAudio.Shutdown();
    g_banditAttackAudio.Shutdown();
    g_banditSpottedAudio2.Shutdown();
    g_banditSpottedAudio1.Shutdown();
    g_hitAudio.Shutdown();
    g_grenadeExplosionAudio.Shutdown();
    g_explosionAudio.Shutdown();
    g_rpgFireAudio.Shutdown();
    g_gunAudio.Shutdown();
    g_profiler.Shutdown();
    CleanupDX12();
    return (int)msg.wParam;
}
