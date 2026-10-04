#pragma once

#include "BoatWakeEmitter.h"

static BoatWakeEmitter g_patrolWakeEmitter;
static BoatWakeEmitter g_insertionWakeEmitter;

static void EmitBoatWaterEffects(const BoatWakeSample& sample,
                                 WaterRendererDX12& waterRenderer) {
    const float fx = sample.direction.x, fz = sample.direction.y;
    const float rx = fz, rz = -fx;
    const float strength = sample.strength;
    auto random = []() { return static_cast<float>(std::rand()) / RAND_MAX; };
    auto waterPoint = [&](float side, float forward) {
        const float x = sample.position.x + rx * side + fx * forward;
        const float z = sample.position.z + rz * side + fz * forward;
        return XMFLOAT3{x, g_ocean.GetSurfaceY() + g_ocean.WaveHeightAt(x, z) + 0.10f, z};
    };

    for (float side : {-1.0f, 1.0f}) {
        const XMFLOAT3 bow = waterPoint(side * 1.6f, 2.7f);
        for (int drop = 0; drop < 3; ++drop) {
            ImpactParticle spray{};
            spray.position = bow;
            const float out = side * (1.2f + strength * 2.8f + random());
            spray.velocity = {fx * sample.speed * 0.22f + rx * out,
                              1.3f + strength * (1.8f + random() * 1.6f),
                              fz * sample.speed * 0.22f + rz * out};
            spray.maxLife = spray.life = 0.45f + random() * 0.40f;
            spray.size = 0.10f + strength * 0.18f + random() * 0.08f;
            spray.growth = 0.10f;
            spray.color = {0.82f, 0.94f, 1.0f};
            spray.waterSpray = true;
            scene.impactParticles.push_back(spray);
        }
    }

    // Outward drift leaves two widening arms rather than a foam disc glued
    // to the moving hull. A slower centre stream supplies the prop wash.
    for (float side : {-1.0f, 0.0f, 1.0f}) {
        ImpactParticle foam{};
        foam.position = waterPoint(side * 1.85f, -3.8f);
        foam.velocity = {rx * side * (0.8f + strength) - fx * 0.35f,
                         0.0f,
                         rz * side * (0.8f + strength) - fz * 0.35f};
        foam.maxLife = foam.life = 2.4f + strength * 1.2f;
        foam.size = 0.35f + strength * 0.40f;
        foam.growth = 0.30f + strength * 0.18f;
        foam.color = {0.72f, 0.86f, 0.92f};
        foam.waterFoam = true;
        scene.impactParticles.push_back(foam);
    }

    if (sample.wave) {
        const XMFLOAT3 stern = waterPoint(0.0f, -4.1f);
        g_ocean.BoatWakeSplash(stern.x, stern.z, 0.18f + strength * 0.42f);
        if (scene.waterQuality == WaterQuality::Ultra) {
            WaterInteraction wake;
            wake.worldXZ = {stern.x, stern.z};
            wake.radius = 1.4f + strength;
            wake.heightImpulse = 0.04f + strength * 0.08f;
            wake.velocityImpulse = {-fx * strength * 0.20f, -fz * strength * 0.20f};
            wake.type = WaterInteractionType::Wake;
            waterRenderer.QueueInteraction(wake);
        }
    }
    if (scene.impactParticles.size() > 1000)
        scene.impactParticles.erase(scene.impactParticles.begin(),
            scene.impactParticles.begin() + (scene.impactParticles.size() - 1000));
}

static void UpdateBoatWaterEffects(BoatWakeEmitter& emitter,
                                  const XMFLOAT3& before, const XMFLOAT3& after,
                                  float dt, bool active,
                                  WaterRendererDX12& waterRenderer) {
    emitter.Step(before, after, dt, active && g_ocean.IsInitialized(),
        [&](const BoatWakeSample& sample) { EmitBoatWaterEffects(sample, waterRenderer); });
}

static void FloatBoatFoamOnWater() {
    for (ImpactParticle& particle : scene.impactParticles)
        if (particle.waterFoam)
            particle.position.y = g_ocean.GetSurfaceY() +
                g_ocean.WaveHeightAt(particle.position.x, particle.position.z) + 0.10f;
}
