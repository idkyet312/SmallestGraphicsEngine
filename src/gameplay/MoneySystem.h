#ifndef MONEY_SYSTEM_H
#define MONEY_SYSTEM_H

// Global wallet. Money is the one number that outlives a run: kills, wrecked
// props and objectives pay out while the level is live, and the mission grade
// converts to a bonus at extraction. The balance is written to disk next to the
// executable so it is still there on the next launch.
//
// Session vs. career: `Balance()` is the career total shown on the main menu,
// `SessionEarned()` is only what the current run has banked. The HUD reads the
// career total so the number the player watches tick up is the same one the
// menu shows -- a run-local counter that reset on extraction made the two look
// like unrelated systems.
//
// Separate INI from settings.ini deliberately: deleting settings to fix a bad
// resolution should not wipe a career, and the wallet is game state rather than
// a preference.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

enum class MoneyEvent : uint8_t {
    EnemyKilled,
    FriendlyLost,
    PropDestroyed,
    CommTowerDestroyed,
    ObjectivePlaneDestroyed,
    MissionBonus,
};

// One payout, queued for the HUD to float up as "+$120". Drained every frame,
// so nothing accumulates when the HUD is not drawing.
struct MoneyAward {
    int amount = 0;
    MoneyEvent event = MoneyEvent::EnemyKilled;
    const char* label = "";
};

class MoneySystem {
public:
    // Payout table. Kills are the steady income, objectives are the spikes.
    // A lost marine is the only debit -- it has to sting enough to make the
    // casualty score mean something in cash as well as in the grade.
    static constexpr int kEnemyKillReward = 120;
    static constexpr int kFriendlyLostPenalty = -250;
    static constexpr int kPropDestroyedReward = 25;
    static constexpr int kCommTowerReward = 2500;
    static constexpr int kObjectivePlaneReward = 900;
    // Mission grade converts at this rate: a 100/100 run pays 5000 on top of
    // whatever the run itself earned.
    static constexpr int kMissionBonusPerScorePoint = 50;

    // Seed money for a new career, so the first deployment starts with
    // something in hand rather than at nothing. This is the starting *state*,
    // not a payout: it is never awarded, so it raises no HUD popup and is not
    // counted as earned. A career that has legitimately been spent down to zero
    // must not be topped back up, which is why the grant lives in the member
    // default and is only kept when there is no wallet file to load over it.
    static constexpr int64_t kStartingBalance = 2000;

    int64_t Balance() const { return balance_; }
    int64_t SessionEarned() const { return sessionEarned_; }
    int64_t TotalEarned() const { return totalEarned_; }

    // Cleared as a run arms, so the extraction screen can report what this
    // deployment was worth without the career total drowning it out.
    void BeginRun() {
        sessionEarned_ = 0;
        pending_.clear();
    }

    // Central payout. Returns the amount actually applied, which is clamped so
    // a penalty can never drive the career balance negative -- a player who
    // loses their whole squad on the first mission is not put in debt.
    int Award(MoneyEvent event, int count = 1) {
        if (count <= 0) return 0;
        const int unit = RewardFor(event);
        return AwardAmount(unit * count, event, LabelFor(event));
    }

    // Score-to-cash at extraction. Separate from Award because the amount comes
    // from the grade rather than the table.
    int AwardMissionBonus(int totalScore) {
        const int amount =
            (std::max)(0, totalScore) * kMissionBonusPerScorePoint;
        return AwardAmount(amount, MoneyEvent::MissionBonus,
                           LabelFor(MoneyEvent::MissionBonus));
    }

    // HUD feed. Moves the queue out rather than copying it, so a frame that
    // banks a dozen kills does not leave them to be drawn again next frame.
    std::vector<MoneyAward> DrainAwards() {
        std::vector<MoneyAward> drained;
        drained.swap(pending_);
        return drained;
    }

    static int RewardFor(MoneyEvent event) {
        switch (event) {
        case MoneyEvent::EnemyKilled:             return kEnemyKillReward;
        case MoneyEvent::FriendlyLost:            return kFriendlyLostPenalty;
        case MoneyEvent::PropDestroyed:           return kPropDestroyedReward;
        case MoneyEvent::CommTowerDestroyed:      return kCommTowerReward;
        case MoneyEvent::ObjectivePlaneDestroyed: return kObjectivePlaneReward;
        default:                                  return 0;
        }
    }

    static const char* LabelFor(MoneyEvent event) {
        switch (event) {
        case MoneyEvent::EnemyKilled:             return "HOSTILE DOWN";
        case MoneyEvent::FriendlyLost:            return "MARINE LOST";
        case MoneyEvent::PropDestroyed:           return "DEMOLITION";
        case MoneyEvent::CommTowerDestroyed:      return "COMM TOWER";
        case MoneyEvent::ObjectivePlaneDestroyed: return "AIRCRAFT DOWN";
        default:                                  return "MISSION BONUS";
        }
    }

    // Formats with thousands separators into the caller's buffer: "$1,250".
    // Hand-rolled rather than via a locale, which would have to be imbued on
    // every stream and still varies by machine.
    static void Format(char* out, size_t capacity, int64_t amount) {
        if (!out || capacity == 0) return;
        char digits[32];
        const bool negative = amount < 0;
        // Negated as unsigned so the most negative int64 does not overflow.
        uint64_t magnitude = negative
            ? (~static_cast<uint64_t>(amount) + 1ull)
            : static_cast<uint64_t>(amount);
        int digitCount = 0;
        do {
            digits[digitCount++] = static_cast<char>('0' + magnitude % 10ull);
            magnitude /= 10ull;
        } while (magnitude > 0ull && digitCount < 20);

        std::string text;
        text.reserve(32);
        if (negative) text.push_back('-');
        text.push_back('$');
        for (int i = digitCount - 1; i >= 0; --i) {
            text.push_back(digits[i]);
            if (i > 0 && i % 3 == 0) text.push_back(',');
        }
        snprintf(out, capacity, "%s", text.c_str());
    }

    // Starting over is a new career, so it comes with the same seed money a
    // first launch gets rather than dropping the player to nothing.
    void ResetCareer() {
        balance_ = kStartingBalance;
        totalEarned_ = 0;
        sessionEarned_ = 0;
        pending_.clear();
    }

    void SetBalance(int64_t balance, int64_t totalEarned) {
        // Explicit int64_t literals rather than std::max<int64_t>: the
        // parenthesized (std::max) form used throughout this codebase (to dodge
        // the windows.h min/max macros) cannot take explicit template
        // arguments, so both operands have to already agree in type.
        balance_ = (std::max)(static_cast<int64_t>(0), balance);
        // Floored against the *earned* part of the balance, not the balance
        // itself: the starting grant is spendable money that was never earned,
        // so a first-launch wallet legitimately holds more than it has made.
        // Flooring at the raw balance would launder that grant into career
        // earnings on the very first save.
        const int64_t earnedFloor =
            (std::max)(static_cast<int64_t>(0), balance_ - kStartingBalance);
        totalEarned_ = (std::max)(earnedFloor,
                                  (std::max)(static_cast<int64_t>(0), totalEarned));
    }

private:
    // Applies one payout and queues it for the HUD. Clamped at zero, and the
    // queued amount is the clamped one so the popup never promises a debit that
    // was not actually taken.
    int AwardAmount(int amount, MoneyEvent event, const char* label) {
        if (amount == 0) return 0;
        if (amount < 0 && balance_ + amount < 0)
            amount = static_cast<int>(-balance_);
        if (amount == 0) return 0;
        balance_ += amount;
        sessionEarned_ += amount;
        if (amount > 0) totalEarned_ += amount;
        pending_.push_back(MoneyAward{ amount, event, label });
        return amount;
    }

    // Seeded, not earned: the balance starts at the grant while totalEarned_
    // stays at zero, so "what this career has actually made" is not inflated by
    // money that was handed over for free.
    int64_t balance_ = kStartingBalance;
    int64_t totalEarned_ = 0;
    int64_t sessionEarned_ = 0;
    std::vector<MoneyAward> pending_;
};

inline const char* MoneySavePath() { return "wallet.ini"; }

// Missing file is the first career, not a failure: the caller keeps the zero
// balance already in `out`.
inline bool LoadMoney(MoneySystem& out) {
    std::ifstream file(MoneySavePath());
    if (!file) return false;

    // Seeded from what the caller already holds, so a file that exists but is
    // missing a key (an older build's wallet, a hand-trimmed one) keeps the
    // starting grant rather than silently zeroing the player out. Only a key
    // that is actually present overwrites these.
    int64_t balance = out.Balance(), totalEarned = out.TotalEarned();
    std::string line;
    while (std::getline(file, line)) {
        const size_t comment = line.find_first_of(";#");
        if (comment != std::string::npos) line.erase(comment);
        const size_t equals = line.find('=');
        if (equals == std::string::npos) continue;
        auto trim = [](std::string s) {
            const size_t first = s.find_first_not_of(" \t\r\n");
            if (first == std::string::npos) return std::string();
            const size_t last = s.find_last_not_of(" \t\r\n");
            return s.substr(first, last - first + 1);
        };
        const std::string key = trim(line.substr(0, equals));
        const std::string value = trim(line.substr(equals + 1));
        if (key.empty() || value.empty()) continue;
        // strtoll over stoll: a hand-mangled file yields 0 rather than throwing
        // out of startup.
        if (key == "Balance") balance = std::strtoll(value.c_str(), nullptr, 10);
        else if (key == "TotalEarned")
            totalEarned = std::strtoll(value.c_str(), nullptr, 10);
    }
    out.SetBalance(balance, totalEarned);
    return true;
}

inline bool SaveMoney(const MoneySystem& money) {
    std::ofstream file(MoneySavePath(), std::ios::trunc);
    if (!file) return false;
    file << "; Smallest Graphics Engine wallet.\n"
         << "; Delete this file to start a fresh career.\n"
         << "[Money]\n"
         << "Balance=" << money.Balance() << "\n"
         << "TotalEarned=" << money.TotalEarned() << "\n";
    return file.good();
}

#endif
