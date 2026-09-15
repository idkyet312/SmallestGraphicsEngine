#include "RankSystem.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

static int failures = 0;
#define CHECK(value) do { if (!(value)) { \
    std::cerr << __FILE__ << ':' << __LINE__ << " CHECK failed: " #value "\n"; \
    ++failures; } } while (false)

static std::string Formatted(int64_t amount) {
    char buffer[64];
    RankSystem::Format(buffer, sizeof(buffer), amount);
    return std::string(buffer);
}

int main() {
    // profile.ini is read from the working directory, so the persistence cases
    // run in a scratch directory rather than next to the test binary.
    const std::filesystem::path scratch =
        std::filesystem::temp_directory_path() / "sge_rank_system_tests";
    std::filesystem::create_directories(scratch);
    std::filesystem::current_path(scratch);
    // Difficulty scales what a run is worth. Stored on the system rather than
    // passed per call, so the rounding happens in one place and the deploy
    // screen, the HUD and the extraction report cannot disagree.
    {
        RankSystem rank;
        CHECK(rank.RewardMultiplier() == 1.0f);
        rank.SetRewardMultiplier(2.0f);
        CHECK(rank.RewardMultiplier() == 2.0f);
        CHECK(rank.Award(XpEvent::EnemyKilled) == RankSystem::kEnemyKillXp * 2);
        CHECK(rank.AwardMissionBonus(10) ==
              RankSystem::kMissionBonusPerScorePoint * 10 * 2);
        // The kill still counts once however much it paid.
        CHECK(rank.LifetimeKills() == 1);
    }

    // Clamped to the range the slider offers at both ends, so a bad value can
    // neither stall a career nor race it to the cap.
    {
        RankSystem rank;
        rank.SetRewardMultiplier(-5.0f);
        CHECK(rank.RewardMultiplier() == 0.25f);
        rank.SetRewardMultiplier(99.0f);
        CHECK(rank.RewardMultiplier() == 3.0f);
    }

    // An award the player was shown never rounds away to nothing, however far
    // the multiplier is turned down: the popup and the bar have to agree that
    // something happened.
    {
        RankSystem rank;
        rank.SetRewardMultiplier(0.25f);
        CHECK(rank.Award(XpEvent::PropDestroyed) > 0);
    }

    // A run setting, not a run counter: arming a run must not silently drop the
    // difficulty the run was armed at back to normal.
    {
        RankSystem rank;
        rank.SetRewardMultiplier(2.5f);
        rank.BeginRun();
        CHECK(rank.RewardMultiplier() == 2.5f);
    }

    std::filesystem::remove(ProfileSavePath());

    // A fresh career starts at the bottom of the ladder with nothing banked.
    {
        RankSystem rank;
        CHECK(rank.TotalXp() == 0);
        CHECK(rank.SessionXp() == 0);
        CHECK(rank.LifetimeKills() == 0);
        CHECK(rank.Level() == 1);
        CHECK(rank.Tier() == PlayerRankTier::Private);
        CHECK(rank.DrainAwards().empty());
        CHECK(rank.DrainLevelUps().empty());
    }

    // The curve and its inverse have to agree at every boundary, because the
    // level shown to the player and the progress bar under it are derived from
    // the two independently: a one-off between them would show a full bar on a
    // level the player has not reached.
    {
        for (int level = 1; level <= RankSystem::kMaxLevel; ++level) {
            const int64_t threshold = RankSystem::XpToReachLevel(level);
            CHECK(RankSystem::LevelForXp(threshold) == level);
            if (level > 1)
                CHECK(RankSystem::LevelForXp(threshold - 1) == level - 1);
        }
        // Level 1 is free, and one promotion costs exactly the difference
        // between two thresholds.
        CHECK(RankSystem::XpToReachLevel(1) == 0);
        CHECK(RankSystem::XpToReachLevel(2) == RankSystem::kBaseLevelXp);
        for (int level = 1; level < RankSystem::kMaxLevel; ++level) {
            CHECK(RankSystem::XpToReachLevel(level + 1) -
                      RankSystem::XpToReachLevel(level) ==
                  RankSystem::XpToLevelUp(level));
        }
    }

    // The cap holds against any amount of experience, and the bar reads full
    // there rather than dividing by a zero remaining-XP.
    {
        RankSystem rank;
        rank.SetProgress(RankSystem::XpToReachLevel(RankSystem::kMaxLevel) * 10,
                         0);
        CHECK(rank.Level() == RankSystem::kMaxLevel);
        CHECK(rank.Tier() == PlayerRankTier::General);
        CHECK(rank.XpForNextLevel() > 0);
        CHECK(rank.XpIntoLevel() == rank.XpForNextLevel());
    }

    // A kill pays the table rate, ticks the lifetime counter, and queues exactly
    // one award for the HUD however many bodies it covers.
    {
        RankSystem rank;
        CHECK(rank.Award(XpEvent::EnemyKilled, 3) ==
              RankSystem::kEnemyKillXp * 3);
        CHECK(rank.TotalXp() == RankSystem::kEnemyKillXp * 3);
        CHECK(rank.SessionXp() == RankSystem::kEnemyKillXp * 3);
        CHECK(rank.LifetimeKills() == 3);
        const std::vector<XpAward> drained = rank.DrainAwards();
        CHECK(drained.size() == 1);
        CHECK(drained.front().amount == RankSystem::kEnemyKillXp * 3);
        CHECK(std::strcmp(drained.front().label, "ENEMY KILLED") == 0);
        // Drained means gone: an award left in the queue would be drawn again
        // on the next frame.
        CHECK(rank.DrainAwards().empty());
        // A non-positive count is not an award.
        CHECK(rank.Award(XpEvent::EnemyKilled, 0) == 0);
        CHECK(rank.LifetimeKills() == 3);
        CHECK(rank.DrainAwards().empty());
    }

    // One large award can cross several boundaries at once -- enough comm
    // towers to clear the third promotion in a single payout -- and each
    // crossing has to be reported, in order.
    {
        RankSystem rank;
        const int towers = static_cast<int>(
            (RankSystem::XpToReachLevel(3) + RankSystem::kCommTowerXp - 1) /
            RankSystem::kCommTowerXp);
        CHECK(static_cast<int64_t>(RankSystem::kCommTowerXp) * towers >=
              RankSystem::XpToReachLevel(3));
        rank.Award(XpEvent::CommTowerDestroyed, towers);
        const int reached = rank.Level();
        CHECK(reached >= 3);
        const std::vector<LevelUpEvent> levelUps = rank.DrainLevelUps();
        CHECK(levelUps.size() == static_cast<size_t>(reached - 1));
        for (size_t i = 0; i < levelUps.size(); ++i)
            CHECK(levelUps[i].level == static_cast<int>(i) + 2);
        CHECK(rank.DrainLevelUps().empty());
    }

    // A promotion is flagged only on the level that starts a new tier. Level 3
    // is the first band boundary above Private, so it promotes and level 4 does
    // not.
    {
        RankSystem rank;
        rank.SetProgress(RankSystem::XpToReachLevel(2), 0);
        const int towers = static_cast<int>(
            (RankSystem::XpToReachLevel(4) - RankSystem::XpToReachLevel(2) +
             RankSystem::kCommTowerXp - 1) /
            RankSystem::kCommTowerXp);
        rank.Award(XpEvent::CommTowerDestroyed, towers);
        const std::vector<LevelUpEvent> levelUps = rank.DrainLevelUps();
        CHECK(!levelUps.empty());
        for (const LevelUpEvent& event : levelUps) {
            const bool startsBand =
                RankSystem::TierForLevel(event.level) !=
                RankSystem::TierForLevel(event.level - 1);
            CHECK(event.rankChanged == startsBand);
            CHECK(event.tier == RankSystem::TierForLevel(event.level));
        }
    }

    // Every band in the ladder is reachable and named, and the ladder never
    // steps backwards as the level climbs.
    {
        PlayerRankTier previous = RankSystem::TierForLevel(1);
        CHECK(previous == PlayerRankTier::Private);
        for (int level = 1; level <= RankSystem::kMaxLevel; ++level) {
            const PlayerRankTier tier = RankSystem::TierForLevel(level);
            CHECK(static_cast<int>(tier) >= static_cast<int>(previous));
            CHECK(PlayerRankName(tier)[0] != '\0');
            previous = tier;
        }
        CHECK(RankSystem::TierForLevel(RankSystem::kMaxLevel) ==
              PlayerRankTier::General);
        // Below level 1 is not a real state, but a mangled profile can ask for
        // it and must not index off the front of the table.
        CHECK(RankSystem::TierForLevel(0) == PlayerRankTier::Private);
        CHECK(RankSystem::TierForLevel(-5) == PlayerRankTier::Private);
    }

    // The grade bonus scales with the score and is floored at zero, so a
    // negative score cannot be handed back as experience.
    {
        RankSystem rank;
        CHECK(rank.AwardMissionBonus(100) ==
              RankSystem::kMissionBonusPerScorePoint * 100);
        CHECK(rank.AwardMissionBonus(-20) == 0);
        CHECK(rank.AwardMissionBonus(0) == 0);
    }

    // Experience never runs backwards: there is no event in the table worth a
    // negative amount, which is what lets the level be derived rather than
    // stored.
    {
        RankSystem rank;
        int64_t previous = 0;
        const XpEvent events[] = {
            XpEvent::EnemyKilled, XpEvent::PropDestroyed,
            XpEvent::CommTowerDestroyed, XpEvent::ObjectivePlaneDestroyed,
            XpEvent::MissionBonus,
        };
        for (XpEvent event : events) {
            CHECK(RankSystem::XpFor(event) >= 0);
            rank.Award(event);
            CHECK(rank.TotalXp() >= previous);
            previous = rank.TotalXp();
        }
    }

    // Arming a run clears what the run is worth and anything the HUD has not
    // drawn yet, but leaves the career alone -- this is what ResetLevelState
    // calls, and a replayed level must not cost the player their rank.
    {
        RankSystem rank;
        rank.Award(XpEvent::EnemyKilled, 5);
        const int64_t career = rank.TotalXp();
        rank.BeginRun();
        CHECK(rank.TotalXp() == career);
        CHECK(rank.LifetimeKills() == 5);
        CHECK(rank.SessionXp() == 0);
        CHECK(rank.DrainAwards().empty());
        CHECK(rank.DrainLevelUps().empty());
    }

    // Restoring a profile queues nothing: a level-40 career loaded at startup
    // must not fire thirty-nine banners on the first frame the HUD draws.
    {
        RankSystem rank;
        rank.SetProgress(RankSystem::XpToReachLevel(40), 900);
        CHECK(rank.Level() == 40);
        CHECK(rank.LifetimeKills() == 900);
        CHECK(rank.DrainLevelUps().empty());
        CHECK(rank.DrainAwards().empty());
        // A corrupt file cannot drive the career negative.
        rank.SetProgress(-1, -1);
        CHECK(rank.TotalXp() == 0);
        CHECK(rank.LifetimeKills() == 0);
        CHECK(rank.Level() == 1);
    }

    CHECK(Formatted(0) == "0");
    CHECK(Formatted(999) == "999");
    CHECK(Formatted(1250) == "1,250");
    CHECK(Formatted(1234567) == "1,234,567");

    // No file is a fresh career rather than a failure: the caller keeps what it
    // already holds.
    {
        RankSystem rank;
        CHECK(!LoadProfile(rank));
        CHECK(rank.TotalXp() == 0);
        CHECK(rank.Level() == 1);
    }

    // Round trip through disk. The level is not stored, so this also proves it
    // comes back from the experience alone.
    {
        RankSystem saved;
        saved.Award(XpEvent::EnemyKilled, 42);
        saved.AwardMissionBonus(88);
        CHECK(SaveProfile(saved));

        RankSystem loaded;
        CHECK(LoadProfile(loaded));
        CHECK(loaded.TotalXp() == saved.TotalXp());
        CHECK(loaded.LifetimeKills() == saved.LifetimeKills());
        CHECK(loaded.Level() == saved.Level());
        CHECK(loaded.Tier() == saved.Tier());
        // Loading is not earning: the run counter stays empty.
        CHECK(loaded.SessionXp() == 0);
    }

    // A file missing a key keeps what the caller already had rather than
    // zeroing the player out, which is what an older build's profile looks
    // like.
    {
        {
            std::ofstream file(ProfileSavePath(), std::ios::trunc);
            file << "[Progress]\nTotalXp=7000\n";
        }
        RankSystem loaded;
        loaded.SetProgress(0, 123);
        CHECK(LoadProfile(loaded));
        CHECK(loaded.TotalXp() == 7000);
        CHECK(loaded.LifetimeKills() == 123);
    }

    // A hand-mangled value yields zero rather than throwing out of startup.
    {
        {
            std::ofstream file(ProfileSavePath(), std::ios::trunc);
            file << "[Progress]\nTotalXp=notanumber\nLifetimeKills=\n";
        }
        RankSystem loaded;
        CHECK(LoadProfile(loaded));
        CHECK(loaded.TotalXp() == 0);
        CHECK(loaded.Level() == 1);
    }

    // Starting over drops everything, including anything the HUD had queued.
    {
        RankSystem rank;
        rank.Award(XpEvent::CommTowerDestroyed);
        rank.ResetCareer();
        CHECK(rank.TotalXp() == 0);
        CHECK(rank.SessionXp() == 0);
        CHECK(rank.LifetimeKills() == 0);
        CHECK(rank.Level() == 1);
        CHECK(rank.DrainAwards().empty());
        CHECK(rank.DrainLevelUps().empty());
    }

    std::filesystem::remove(ProfileSavePath());
    if (failures == 0) std::cout << "RankSystemTests passed\n";
    return failures == 0 ? 0 : 1;
}
