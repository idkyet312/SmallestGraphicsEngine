#ifndef MISSION_SYSTEM_H
#define MISSION_SYSTEM_H

#include "LevelDefinition.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

enum class GrenadeType : uint8_t { Frag = 0, Molotov = 1, Vortex = 2 };

inline const char* GrenadeTypeName(GrenadeType type) {
    switch (type) {
    case GrenadeType::Molotov: return "Molotov";
    case GrenadeType::Vortex: return "Vortex";
    default: return "Frag";
    }
}

struct MissionLoadout {
    static constexpr int kWeaponCount = 8;
    static constexpr size_t kWeaponSlotCount = 2;

    std::array<int, kWeaponSlotCount> weapons{{ 0, 1 }};
    GrenadeType grenade = GrenadeType::Frag;
    LevelInsertionMode insertion = LevelInsertionMode::Helicopter;

    bool Valid() const {
        return weapons[0] >= 0 && weapons[0] < kWeaponCount &&
               weapons[1] >= 0 && weapons[1] < kWeaponCount &&
               weapons[0] != weapons[1];
    }

    bool ContainsWeapon(int weapon) const {
        return weapons[0] == weapon || weapons[1] == weapon;
    }

    void SelectWeapon(size_t slot, int weapon) {
        if (slot >= weapons.size() || weapon < 0 || weapon >= kWeaponCount)
            return;
        const size_t other = slot == 0 ? 1 : 0;
        if (weapons[other] == weapon)
            std::swap(weapons[slot], weapons[other]);
        else
            weapons[slot] = weapon;
    }
};

struct MissionRunStats {
    uint32_t shotsFired = 0;
    uint32_t shotsHit = 0;
    uint32_t friendliesDeployed = 0;
    uint32_t grenadesThrown = 0;
    uint32_t destructionEvents = 0;
    uint32_t weaponsUsedMask = 0;

    float AccuracyPercent() const {
        if (shotsFired == 0) return 0.0f;
        return (std::min)(100.0f,
            100.0f * static_cast<float>(shotsHit) /
            static_cast<float>(shotsFired));
    }

    bool WeaponUsed(int weapon) const {
        return weapon >= 0 && weapon < 32 &&
               (weaponsUsedMask & (1u << static_cast<uint32_t>(weapon))) != 0;
    }
};

enum class MissionRank : uint8_t { S, A, B, C, D, F };

inline const char* MissionRankName(MissionRank rank) {
    switch (rank) {
    case MissionRank::S: return "S";
    case MissionRank::A: return "A";
    case MissionRank::B: return "B";
    case MissionRank::C: return "C";
    case MissionRank::D: return "D";
    default: return "F";
    }
}

struct MissionReport {
    float elapsedSeconds = 0.0f;
    float accuracyPercent = 0.0f;
    uint32_t casualties = 0;
    uint32_t survivingFriendlies = 0;
    uint32_t optionalObjectivesCompleted = 0;
    uint32_t optionalObjectivesTotal = 3;
    uint32_t destructionEvents = 0;
    bool usedBothWeapons = false;
    bool usedSelectedGrenade = false;
    bool demolitionObjective = false;
    int timeScore = 0;
    int accuracyScore = 0;
    int casualtyScore = 0;
    int optionalScore = 0;
    int destructionScore = 0;
    int totalScore = 0;
    MissionRank rank = MissionRank::F;
};

class MissionSystem {
public:
    static constexpr float kParTimeSeconds = 300.0f;
    static constexpr uint32_t kDemolitionObjectiveEvents = 5;
    static constexpr uint32_t kDestructionScoreTarget = 12;

    MissionLoadout& Loadout() { return loadout_; }
    const MissionLoadout& Loadout() const { return loadout_; }
    const MissionRunStats& Stats() const { return stats_; }
    const MissionReport& Report() const { return report_; }
    bool Complete() const { return complete_; }

    void ResetRun() {
        stats_ = {};
        report_ = {};
        complete_ = false;
    }

    void RecordWeaponFired(int weapon, uint32_t projectileCount) {
        if (weapon >= 0 && weapon < 32)
            stats_.weaponsUsedMask |= 1u << static_cast<uint32_t>(weapon);
        stats_.shotsFired += projectileCount;
    }

    void RecordHit() { ++stats_.shotsHit; }
    void RecordFriendlyDeployed() { ++stats_.friendliesDeployed; }
    void RecordGrenadeThrown() { ++stats_.grenadesThrown; }
    void RecordDestruction(uint32_t count = 1) {
        stats_.destructionEvents += count;
    }

    MissionReport Finish(float elapsedSeconds, uint32_t survivingFriendlies) {
        report_ = Grade(loadout_, stats_, elapsedSeconds, survivingFriendlies);
        complete_ = true;
        return report_;
    }

    static MissionReport Grade(const MissionLoadout& loadout,
                               const MissionRunStats& stats,
                               float elapsedSeconds,
                               uint32_t survivingFriendlies) {
        MissionReport report;
        report.elapsedSeconds = (std::max)(0.0f, elapsedSeconds);
        report.accuracyPercent = stats.AccuracyPercent();
        report.survivingFriendlies = (std::min)(
            survivingFriendlies, stats.friendliesDeployed);
        report.casualties = stats.friendliesDeployed - report.survivingFriendlies;
        report.destructionEvents = stats.destructionEvents;

        report.usedBothWeapons = stats.WeaponUsed(loadout.weapons[0]) &&
                                 stats.WeaponUsed(loadout.weapons[1]);
        report.usedSelectedGrenade = stats.grenadesThrown > 0;
        report.demolitionObjective =
            stats.destructionEvents >= kDemolitionObjectiveEvents;
        report.optionalObjectivesCompleted =
            static_cast<uint32_t>(report.usedBothWeapons) +
            static_cast<uint32_t>(report.usedSelectedGrenade) +
            static_cast<uint32_t>(report.demolitionObjective);

        const float overtime = (std::max)(
            0.0f, report.elapsedSeconds - kParTimeSeconds);
        const float timeFraction = std::clamp(
            1.0f - overtime / kParTimeSeconds, 0.0f, 1.0f);
        report.timeScore = static_cast<int>(std::lround(20.0f * timeFraction));
        report.accuracyScore = static_cast<int>(std::lround(
            25.0f * report.accuracyPercent / 100.0f));
        const float survivorFraction = stats.friendliesDeployed == 0
            ? 1.0f
            : static_cast<float>(report.survivingFriendlies) /
              static_cast<float>(stats.friendliesDeployed);
        report.casualtyScore = static_cast<int>(std::lround(
            20.0f * survivorFraction));
        report.optionalScore = static_cast<int>(std::lround(
            20.0f * static_cast<float>(report.optionalObjectivesCompleted) /
            static_cast<float>(report.optionalObjectivesTotal)));
        report.destructionScore = static_cast<int>(std::lround(
            15.0f * (std::min)(1.0f,
                static_cast<float>(stats.destructionEvents) /
                static_cast<float>(kDestructionScoreTarget))));
        report.totalScore = report.timeScore + report.accuracyScore +
            report.casualtyScore + report.optionalScore +
            report.destructionScore;

        report.rank = report.totalScore >= 90 ? MissionRank::S :
            report.totalScore >= 80 ? MissionRank::A :
            report.totalScore >= 70 ? MissionRank::B :
            report.totalScore >= 60 ? MissionRank::C :
            report.totalScore >= 40 ? MissionRank::D : MissionRank::F;
        return report;
    }

private:
    MissionLoadout loadout_;
    MissionRunStats stats_;
    MissionReport report_;
    bool complete_ = false;
};

#endif
