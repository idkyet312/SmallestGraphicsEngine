#ifndef GAME_RUNTIME_H
#define GAME_RUNTIME_H

#include "FixedStepClock.h"
#include "GameCommandQueue.h"
#include "GameSession.h"
#include "MissionSystem.h"
#include "CombatSystem.h"
#include "LevelLoadingController.h"
#include "PlayerMovementTracker.h"
#include "RuntimeWorld.h"
#include "VehicleSystem.h"

struct GameRuntime {
    RuntimeWorld world;
    CombatSystem combat;
    VehicleSystem vehicles;
    GameSession session;
    MissionSystem mission;
    FixedStepClock physicsClock{ 1.0f / 60.0f, 4 };
    LevelLoadingController loading;
    GameCommandQueue commands;
    PlayerMovementTracker playerMovement;

    void ResetLevelState() {
        combat.ResetLevel();
        vehicles.ResetLevel();
        physicsClock.Reset();
        commands.Clear();
        playerMovement = {};
        mission.ResetRun();
    }
};

#endif
