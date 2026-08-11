#include "CombatSystem.h"
#include "RuntimeWorld.h"

#include <algorithm>
#include <cmath>

CombatSystem::PrefabDamageResult CombatSystem::DamagePrefab(
        RuntimeWorld& world, uint64_t entityId, float damage,
        const DirectX::XMFLOAT3& effectPosition) {
    PrefabDamageResult result;
    result.entityId = entityId;
    result.effectPosition = effectPosition;
    if (damage <= 0.0f) return result;

    PrefabRuntimeState& prefabs = world.Prefabs();
    const auto definition = std::find_if(
        prefabs.destructibles.begin(), prefabs.destructibles.end(),
        [entityId](const PrefabDestructibleInstance& value) {
            return value.entityId == entityId;
        });
    if (definition == prefabs.destructibles.end()) return result;

    result.applied = true;
    float& health = prefabs.health.try_emplace(
        entityId, definition->health).first->second;
    health -= damage;
    if (health > 0.0f) return result;

    for (LevelEntity& entity : world.Level().entities) {
        if (entity.id != entityId || !entity.enabled) continue;
        entity.enabled = false;
        result.destroyed = true;
        break;
    }
    return result;
}

std::vector<CombatSystem::PrefabDamageResult>
CombatSystem::DamagePrefabsInRadius(
        RuntimeWorld& world, const DirectX::XMFLOAT3& center,
        float radius, float damage,
        const std::function<bool(uint64_t)>& damageable) {
    std::vector<PrefabDamageResult> results;
    if (radius <= 0.0f || damage <= 0.0f) return results;

    // Copy descriptors because DamagePrefab mutates world state.
    const std::vector<PrefabDestructibleInstance> destructibles =
        world.Prefabs().destructibles;
    for (const PrefabDestructibleInstance& prefab : destructibles) {
        // Lets the caller hold specific props immune to this blast -- used to
        // keep the comm tower a player-only objective, so an enemy grenade
        // landing at its feet cannot fell it.
        if (damageable && !damageable(prefab.entityId)) continue;
        const float dx = prefab.position.x - center.x;
        const float dy = prefab.position.y - center.y;
        const float dz = prefab.position.z - center.z;
        const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        if (distance >= radius) continue;
        PrefabDamageResult result = DamagePrefab(
            world, prefab.entityId,
            damage * (1.0f - distance / radius), prefab.position);
        if (result.applied) results.push_back(result);
    }
    return results;
}
