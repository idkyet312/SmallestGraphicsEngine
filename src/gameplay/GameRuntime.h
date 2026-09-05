#ifndef GAME_RUNTIME_H
#define GAME_RUNTIME_H

#include "FixedStepClock.h"
#include "GameCommandQueue.h"
#include "GameSession.h"
#include "MissionSystem.h"
#include "MoneySystem.h"
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
    // Career wallet. Deliberately not cleared by ResetLevelState -- money is
    // the one thing that survives a restart, so a replayed level keeps what
    // earlier runs banked. Only the per-run counter is reset there.
    MoneySystem money;
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
        money.BeginRun();
    }
};

#endif
