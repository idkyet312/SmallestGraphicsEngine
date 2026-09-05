#include "MoneySystem.h"

#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>

static int failures = 0;
#define CHECK(value) do { if (!(value)) { \
    std::cerr << __FILE__ << ':' << __LINE__ << " CHECK failed: " #value "\n"; \
    ++failures; } } while (false)

static std::string Formatted(int64_t amount) {
    char buffer[64];
    MoneySystem::Format(buffer, sizeof(buffer), amount);
    return std::string(buffer);
}

int main() {
    // The wallet file is read from the working directory, so the persistence
    // cases run in a scratch directory rather than next to the test binary.
    const std::filesystem::path scratch =
        std::filesystem::temp_directory_path() / "sge_money_system_tests";
    std::filesystem::create_directories(scratch);
    std::filesystem::current_path(scratch);
    std::filesystem::remove(MoneySavePath());

    // Awards accumulate, and each one is queued exactly once for the HUD.
    {
        MoneySystem money;
        CHECK(money.Balance() == 0);
        money.Award(MoneyEvent::EnemyKilled);
        money.Award(MoneyEvent::EnemyKilled);
        CHECK(money.Balance() == MoneySystem::kEnemyKillReward * 2);
        CHECK(money.SessionEarned() == MoneySystem::kEnemyKillReward * 2);

        const std::vector<MoneyAward> drained = money.DrainAwards();
        CHECK(drained.size() == 2);
        CHECK(drained[0].amount == MoneySystem::kEnemyKillReward);
        // Draining empties the queue: a frame that already drew a payout must
        // not draw it again on the next one.
        CHECK(money.DrainAwards().empty());
    }

    // A counted award is one queued entry with a multiplied amount, not N
    // entries. A collapsing building drains dozens of break points at once and
    // would otherwise flood the HUD.
    {
        MoneySystem money;
        money.Award(MoneyEvent::PropDestroyed, 30);
        CHECK(money.Balance() == MoneySystem::kPropDestroyedReward * 30);
        const std::vector<MoneyAward> drained = money.DrainAwards();
        CHECK(drained.size() == 1);
        CHECK(drained[0].amount == MoneySystem::kPropDestroyedReward * 30);
    }

    // A zero or negative count is a no-op rather than a reversed payout.
    {
        MoneySystem money;
        CHECK(money.Award(MoneyEvent::EnemyKilled, 0) == 0);
        CHECK(money.Award(MoneyEvent::EnemyKilled, -4) == 0);
        CHECK(money.Balance() == 0);
        CHECK(money.DrainAwards().empty());
    }

    // The casualty penalty is clamped at zero: losing a squad on a poor run must
    // not put the player into debt, and the queued popup has to report the
    // amount actually taken rather than the full penalty.
    {
        MoneySystem money;
        money.Award(MoneyEvent::EnemyKilled);   // 120 banked
        const int applied = money.Award(MoneyEvent::FriendlyLost);
        CHECK(money.Balance() == 0);
        CHECK(applied == -MoneySystem::kEnemyKillReward);
        const std::vector<MoneyAward> drained = money.DrainAwards();
        CHECK(drained.size() == 2);
        CHECK(drained[1].amount == -MoneySystem::kEnemyKillReward);

        // Already at zero: nothing more to take, and nothing queued for a
        // payout that did not happen.
        CHECK(money.Award(MoneyEvent::FriendlyLost) == 0);
        CHECK(money.Balance() == 0);
        CHECK(money.DrainAwards().empty());
    }

    // Career earnings only ever climb, so a penalty does not erase the record of
    // what was earned.
    {
        MoneySystem money;
        money.Award(MoneyEvent::CommTowerDestroyed);
        money.Award(MoneyEvent::FriendlyLost);
        CHECK(money.TotalEarned() == MoneySystem::kCommTowerReward);
        CHECK(money.Balance() ==
              MoneySystem::kCommTowerReward + MoneySystem::kFriendlyLostPenalty);
    }

    // The mission bonus is the grade times the rate, and a failed run pays
    // nothing rather than a negative bonus.
    {
        MoneySystem money;
        CHECK(money.AwardMissionBonus(100) ==
              100 * MoneySystem::kMissionBonusPerScorePoint);
        CHECK(money.AwardMissionBonus(0) == 0);
        CHECK(money.AwardMissionBonus(-20) == 0);
        CHECK(money.Balance() == 100 * MoneySystem::kMissionBonusPerScorePoint);
    }

    // BeginRun clears the per-run counter and the undrawn queue, but never the
    // career balance -- replaying a level must not cost the player what earlier
    // runs banked.
    {
        MoneySystem money;
        money.Award(MoneyEvent::EnemyKilled);
        money.BeginRun();
        CHECK(money.SessionEarned() == 0);
        CHECK(money.Balance() == MoneySystem::kEnemyKillReward);
        CHECK(money.DrainAwards().empty());
    }

    // Formatting: thousands separators, and the group boundaries land in the
    // right place at each digit count.
    {
        CHECK(Formatted(0) == "$0");
        CHECK(Formatted(7) == "$7");
        CHECK(Formatted(999) == "$999");
        CHECK(Formatted(1000) == "$1,000");
        CHECK(Formatted(12345) == "$12,345");
        CHECK(Formatted(1234567) == "$1,234,567");
        CHECK(Formatted(-250) == "-$250");
        CHECK(Formatted(-1250) == "-$1,250");
    }

    // Round-trip through the file, and a missing file is a fresh career rather
    // than a failure the caller has to handle.
    {
        MoneySystem fresh;
        CHECK(!LoadMoney(fresh));
        CHECK(fresh.Balance() == 0);

        MoneySystem money;
        money.Award(MoneyEvent::CommTowerDestroyed);
        money.Award(MoneyEvent::EnemyKilled, 3);
        CHECK(SaveMoney(money));

        MoneySystem loaded;
        CHECK(LoadMoney(loaded));
        CHECK(loaded.Balance() == money.Balance());
        CHECK(loaded.TotalEarned() == money.TotalEarned());
        // A loaded career starts its run counter at zero: what previous
        // launches earned is not this deployment's payout.
        CHECK(loaded.SessionEarned() == 0);
    }

    // A hand-mangled file must not throw out of startup or produce a negative
    // career, and unknown keys are ignored so a file from a newer build still
    // loads on an older one.
    {
        {
            std::ofstream file(MoneySavePath(), std::ios::trunc);
            file << "; comment\n[Money]\nBalance=not-a-number\n"
                 << "TotalEarned=-99\nUnknownKey=17\n";
        }
        MoneySystem loaded;
        CHECK(LoadMoney(loaded));
        CHECK(loaded.Balance() == 0);
        CHECK(loaded.TotalEarned() >= 0);
    }

    // A file claiming more spendable money than was ever earned is repaired
    // upward rather than trusted: TotalEarned is a floor on the balance.
    {
        {
            std::ofstream file(MoneySavePath(), std::ios::trunc);
            file << "[Money]\nBalance=5000\nTotalEarned=10\n";
        }
        MoneySystem loaded;
        CHECK(LoadMoney(loaded));
        CHECK(loaded.Balance() == 5000);
        CHECK(loaded.TotalEarned() == 5000);
    }

    std::filesystem::remove(MoneySavePath());
    if (failures == 0) std::cout << "MoneySystemTests passed\n";
    return failures == 0 ? 0 : 1;
}
