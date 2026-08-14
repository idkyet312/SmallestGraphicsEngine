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

// Equipment carried in the gear slot, separate from the two weapon slots and
// the grenade. None is a real choice rather than an empty default: the slot
// exists to be deliberately left open as often as it is filled.
enum class GearType : uint8_t { None = 0, NightVisionGoggles = 1 };

inline const char* GearTypeName(GearType gear) {
    switch (gear) {
    case GearType::NightVisionGoggles: return "NVG";
    default: return "None";
    }
}

struct MissionLoadout {
    static constexpr int kWeaponCount = 9;
    static constexpr size_t kWeaponSlotCount = 2;

    std::array<int, kWeaponSlotCount> weapons{{ 0, 1 }};
    GrenadeType grenade = GrenadeType::Frag;
    // Empty by default: the slot is an opt-in, and NVG is only worth a pick on
    // the dark times of day.
    GearType gear = GearType::None;
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
    // Comm towers standing when the run armed, and how many have since come
    // down. Counted rather than a flag because a level may author more than one,
    // and the primary objective is only met when the last of them falls.
    uint32_t commTowersTotal = 0;
    uint32_t commTowersDestroyed = 0;

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
    // Primary objective: every authored comm tower levelled. True on levels that
    // carry no tower, so maps without one are not permanently marked as failed
    // -- `primaryObjectivePresent` is what says whether it was ever in play.
    bool primaryObjectiveComplete = true;
    bool primaryObjectivePresent = false;
    uint32_t commTowersDestroyed = 0;
    uint32_t commTowersTotal = 0;
    int timeScore = 0;
    int accuracyScore = 0;
    int casualtyScore = 0;
    int optionalScore = 0;
    int destructionScore = 0;
    int primaryScore = 0;
    int totalScore = 0;
    MissionRank rank = MissionRank::F;
};

class MissionSystem {
public:
    static constexpr float kParTimeSeconds = 300.0f;
    static constexpr uint32_t kDemolitionObjectiveEvents = 5;
    static constexpr uint32_t kDestructionScoreTarget = 12;
    // Weights: time 15 + accuracy 20 + casualties 15 + optional 15 +
    // destruction 10 + primary 25 = 100.
    static constexpr int kPrimaryObjectiveScore = 25;

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

    // Called once as the run arms, with the towers the level actually spawned.
    void SetCommTowerCount(uint32_t count) { stats_.commTowersTotal = count; }

    // Saturates at the authored count so a double-report (a tower felled by a
    // charge that also registers as prefab damage) cannot push the objective
    // past 100% or credit a tower that was never there.
    void RecordCommTowerDestroyed() {
        if (stats_.commTowersDestroyed < stats_.commTowersTotal)
            ++stats_.commTowersDestroyed;
    }

    bool CommTowerObjectiveComplete() const {
        return stats_.commTowersTotal > 0 &&
               stats_.commTowersDestroyed >= stats_.commTowersTotal;
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

        report.commTowersTotal = stats.commTowersTotal;
        report.commTowersDestroyed =
            (std::min)(stats.commTowersDestroyed, stats.commTowersTotal);
        report.primaryObjectivePresent = stats.commTowersTotal > 0;
        report.primaryObjectiveComplete = !report.primaryObjectivePresent ||
            report.commTowersDestroyed >= report.commTowersTotal;
        report.optionalObjectivesCompleted =
            static_cast<uint32_t>(report.usedBothWeapons) +
            static_cast<uint32_t>(report.usedSelectedGrenade) +
            static_cast<uint32_t>(report.demolitionObjective);

        const float overtime = (std::max)(
            0.0f, report.elapsedSeconds - kParTimeSeconds);
        const float timeFraction = std::clamp(
            1.0f - overtime / kParTimeSeconds, 0.0f, 1.0f);
        report.timeScore = static_cast<int>(std::lround(15.0f * timeFraction));
        report.accuracyScore = static_cast<int>(std::lround(
            20.0f * report.accuracyPercent / 100.0f));
        const float survivorFraction = stats.friendliesDeployed == 0
            ? 1.0f
            : static_cast<float>(report.survivingFriendlies) /
              static_cast<float>(stats.friendliesDeployed);
        report.casualtyScore = static_cast<int>(std::lround(
            15.0f * survivorFraction));
        report.optionalScore = static_cast<int>(std::lround(
            15.0f * static_cast<float>(report.optionalObjectivesCompleted) /
            static_cast<float>(report.optionalObjectivesTotal)));
        report.destructionScore = static_cast<int>(std::lround(
            10.0f * (std::min)(1.0f,
                static_cast<float>(stats.destructionEvents) /
                static_cast<float>(kDestructionScoreTarget))));
        // The primary objective is the mission. It scores partial credit per
        // tower so a two-tower map still rewards the first one, and pays out in
        // full on levels that authored none -- there the other categories are
        // the whole grade and withholding 25 points would cap every run at a C.
        report.primaryScore = report.primaryObjectivePresent
            ? static_cast<int>(std::lround(
                  static_cast<float>(kPrimaryObjectiveScore) *
                  static_cast<float>(report.commTowersDestroyed) /
                  static_cast<float>(report.commTowersTotal)))
            : kPrimaryObjectiveScore;
        report.totalScore = report.timeScore + report.accuracyScore +
            report.casualtyScore + report.optionalScore +
            report.destructionScore + report.primaryScore;

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
