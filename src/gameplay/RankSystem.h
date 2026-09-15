#ifndef RANK_SYSTEM_H
#define RANK_SYSTEM_H

// Career progression. The wallet in MoneySystem.h answers "what can I afford";
// this answers "how far along am I". Kills, wrecked objectives and the mission
// grade pay experience, experience derives a level, and the level derives a
// military rank. All three outlive a run and are written to disk next to the
// executable.
//
// Prestige only, by design: rank unlocks nothing. Money stays the single gate
// on the armory, so retuning the curve below can never strand a player who
// bought a weapon at a level they would no longer qualify for.
//
// Separate INI from wallet.ini deliberately, for the same reason the wallet is
// separate from settings.ini: profile.ini holds only derived-from-XP progress
// and nothing else lives in it, so this file never has to preserve keys it does
// not own the way SaveMoney does.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

// Mirrors MoneyEvent, minus the penalty. There is deliberately no FriendlyLost
// equivalent: experience is a record of what a player has done and never goes
// backwards, so a lost marine costs cash and mission score but not rank.
enum class XpEvent : uint8_t {
    EnemyKilled,
    PropDestroyed,
    CommTowerDestroyed,
    ObjectivePlaneDestroyed,
    MissionBonus,
};

// One payout, queued for the HUD to float up as "+100 XP". Drained every frame,
// so nothing accumulates when the HUD is not drawing.
struct XpAward {
    int amount = 0;
    XpEvent event = XpEvent::EnemyKilled;
    const char* label = "";
};

// Rank ladder. Twenty-one tiers spread over fifty levels, bunched at the bottom
// so the first few missions promote quickly and the general officer ranks stay
// rare.
enum class PlayerRankTier : uint8_t {
    Private,
    PrivateFirstClass,
    LanceCorporal,
    Corporal,
    Sergeant,
    StaffSergeant,
    GunnerySergeant,
    MasterSergeant,
    FirstSergeant,
    MasterGunnerySergeant,
    SergeantMajor,
    WarrantOfficer,
    ChiefWarrantOfficer,
    SecondLieutenant,
    FirstLieutenant,
    Captain,
    Major,
    LieutenantColonel,
    Colonel,
    BrigadierGeneral,
    General,
    Count,
};

// Named PlayerRankName rather than RankName: MissionSystem.h already owns
// MissionRank/MissionRankName for the S..F letter grade on a single run, which
// is an unrelated number that happens to share the word.
inline const char* PlayerRankName(PlayerRankTier tier) {
    switch (tier) {
    case PlayerRankTier::Private:               return "PRIVATE";
    case PlayerRankTier::PrivateFirstClass:     return "PRIVATE FIRST CLASS";
    case PlayerRankTier::LanceCorporal:         return "LANCE CORPORAL";
    case PlayerRankTier::Corporal:              return "CORPORAL";
    case PlayerRankTier::Sergeant:              return "SERGEANT";
    case PlayerRankTier::StaffSergeant:         return "STAFF SERGEANT";
    case PlayerRankTier::GunnerySergeant:       return "GUNNERY SERGEANT";
    case PlayerRankTier::MasterSergeant:        return "MASTER SERGEANT";
    case PlayerRankTier::FirstSergeant:         return "FIRST SERGEANT";
    case PlayerRankTier::MasterGunnerySergeant: return "MASTER GUNNERY SERGEANT";
    case PlayerRankTier::SergeantMajor:         return "SERGEANT MAJOR";
    case PlayerRankTier::WarrantOfficer:        return "WARRANT OFFICER";
    case PlayerRankTier::ChiefWarrantOfficer:   return "CHIEF WARRANT OFFICER";
    case PlayerRankTier::SecondLieutenant:      return "SECOND LIEUTENANT";
    case PlayerRankTier::FirstLieutenant:       return "FIRST LIEUTENANT";
    case PlayerRankTier::Captain:               return "CAPTAIN";
    case PlayerRankTier::Major:                 return "MAJOR";
    case PlayerRankTier::LieutenantColonel:     return "LIEUTENANT COLONEL";
    case PlayerRankTier::Colonel:               return "COLONEL";
    case PlayerRankTier::BrigadierGeneral:      return "BRIGADIER GENERAL";
    default:                                    return "GENERAL";
    }
}

// One level crossing, queued for the HUD banner and the extraction screen.
// `rankChanged` separates the two things a player cares about: every level is
// worth a small readout, but only the level that starts a new tier is worth a
// promotion banner.
struct LevelUpEvent {
    int level = 1;
    PlayerRankTier tier = PlayerRankTier::Private;
    bool rankChanged = false;
};

class RankSystem {
public:
    // Payout table, sized against MoneySystem's so the two feeds tick at a
    // similar rate and neither reads as the "real" reward.
    static constexpr int kEnemyKillXp = 100;
    static constexpr int kPropDestroyedXp = 15;
    static constexpr int kCommTowerXp = 1500;
    static constexpr int kObjectivePlaneXp = 600;
    // Mission grade converts at this rate: a 100/100 run pays 4000 on top of
    // whatever the run itself earned in the field.
    static constexpr int kMissionBonusPerScorePoint = 40;

    // The curve. Going from level L to L+1 costs kBaseLevelXp +
    // kLevelXpGrowth * (L - 1), so the first promotion is 2500 and the last is
    // 2500 + 750 * 48 = 38500. Cumulative to level 50 is about 1.0M, and a
    // completed mission is worth roughly 7-8k (thirty kills, a tower, the
    // grade bonus), which puts the ladder at about a hundred and twenty-five
    // missions -- five times the old pace.
    // These three numbers are the only tuning knobs; nothing derived from them
    // is stored, so changing them re-levels an existing profile rather than
    // stranding it.
    static constexpr int kMaxLevel = 50;
    static constexpr int64_t kBaseLevelXp = 2500;
    static constexpr int64_t kLevelXpGrowth = 750;

    int64_t TotalXp() const { return totalXp_; }
    int64_t SessionXp() const { return sessionXp_; }
    int64_t LifetimeKills() const { return lifetimeKills_; }

    int Level() const { return LevelForXp(totalXp_); }
    PlayerRankTier Tier() const { return TierForLevel(Level()); }
    const char* RankLabel() const { return PlayerRankName(Tier()); }

    // The two numbers a progress bar needs. At the cap they report a full bar
    // rather than a zero denominator, so the HUD needs no special case.
    int64_t XpIntoLevel() const {
        const int level = Level();
        if (level >= kMaxLevel) return XpForNextLevel();
        return totalXp_ - XpToReachLevel(level);
    }

    int64_t XpForNextLevel() const {
        const int level = Level();
        if (level >= kMaxLevel) return XpToLevelUp(kMaxLevel - 1);
        return XpToLevelUp(level);
    }

    // Cleared as a run arms, so the extraction screen can report what this
    // deployment was worth without the career total drowning it out.
    void BeginRun() {
        sessionXp_ = 0;
        pending_.clear();
        pendingLevelUps_.clear();
    }

    // Central payout. Returns the amount applied. Kills also tick the lifetime
    // counter here rather than at the call site, so every death route that
    // already funnels through one award inherits the count for free.
    int Award(XpEvent event, int count = 1) {
        if (count <= 0) return 0;
        if (event == XpEvent::EnemyKilled) lifetimeKills_ += count;
        return AwardAmount(XpFor(event) * count, event, LabelFor(event));
    }

    // Score-to-experience at extraction. Separate from Award because the amount
    // comes from the grade rather than the table.
    int AwardMissionBonus(int totalScore) {
        const int amount = (std::max)(0, totalScore) * kMissionBonusPerScorePoint;
        return AwardAmount(amount, XpEvent::MissionBonus,
                           LabelFor(XpEvent::MissionBonus));
    }

    // HUD feed. Moves the queue out rather than copying it, so a frame that
    // banks a dozen kills does not leave them to be drawn again next frame.
    std::vector<XpAward> DrainAwards() {
        std::vector<XpAward> drained;
        drained.swap(pending_);
        return drained;
    }

    std::vector<LevelUpEvent> DrainLevelUps() {
        std::vector<LevelUpEvent> drained;
        drained.swap(pendingLevelUps_);
        return drained;
    }

    static int XpFor(XpEvent event) {
        switch (event) {
        case XpEvent::EnemyKilled:             return kEnemyKillXp;
        case XpEvent::PropDestroyed:           return kPropDestroyedXp;
        case XpEvent::CommTowerDestroyed:      return kCommTowerXp;
        case XpEvent::ObjectivePlaneDestroyed: return kObjectivePlaneXp;
        default:                               return 0;
        }
    }

    static const char* LabelFor(XpEvent event) {
        switch (event) {
        case XpEvent::EnemyKilled:             return "ENEMY KILLED";
        case XpEvent::PropDestroyed:           return "DEMOLITION";
        case XpEvent::CommTowerDestroyed:      return "COMM TOWER";
        case XpEvent::ObjectivePlaneDestroyed: return "AIRCRAFT DOWN";
        default:                               return "MISSION BONUS";
        }
    }

    // What one promotion costs, going from `level` to `level + 1`.
    static int64_t XpToLevelUp(int level) {
        if (level < 1) level = 1;
        return kBaseLevelXp + kLevelXpGrowth * static_cast<int64_t>(level - 1);
    }

    // Total career experience needed to stand at `level`. Level 1 is free, so
    // this is the closed form of summing XpToLevelUp over everything below.
    static int64_t XpToReachLevel(int level) {
        if (level <= 1) return 0;
        if (level > kMaxLevel) level = kMaxLevel;
        const int64_t steps = level - 1;
        return steps * kBaseLevelXp + kLevelXpGrowth * steps * (steps - 1) / 2;
    }

    // Walked rather than solved with a quadratic root: fifty iterations costs
    // nothing at the rate this is called, and it cannot drift off by one the
    // way a float sqrt against an int64 threshold can.
    static int LevelForXp(int64_t xp) {
        int level = 1;
        while (level < kMaxLevel && xp >= XpToReachLevel(level + 1)) ++level;
        return level;
    }

    static PlayerRankTier TierForLevel(int level) {
        // Ascending minimum-level table, walked from the top so the first match
        // is the highest tier earned.
        struct TierBand { int minLevel; PlayerRankTier tier; };
        static constexpr TierBand kBands[] = {
            {  1, PlayerRankTier::Private },
            {  3, PlayerRankTier::PrivateFirstClass },
            {  6, PlayerRankTier::LanceCorporal },
            {  9, PlayerRankTier::Corporal },
            { 12, PlayerRankTier::Sergeant },
            { 15, PlayerRankTier::StaffSergeant },
            { 18, PlayerRankTier::GunnerySergeant },
            { 21, PlayerRankTier::MasterSergeant },
            { 24, PlayerRankTier::FirstSergeant },
            { 27, PlayerRankTier::MasterGunnerySergeant },
            { 30, PlayerRankTier::SergeantMajor },
            { 32, PlayerRankTier::WarrantOfficer },
            { 34, PlayerRankTier::ChiefWarrantOfficer },
            { 36, PlayerRankTier::SecondLieutenant },
            { 38, PlayerRankTier::FirstLieutenant },
            { 40, PlayerRankTier::Captain },
            { 42, PlayerRankTier::Major },
            { 44, PlayerRankTier::LieutenantColonel },
            { 46, PlayerRankTier::Colonel },
            { 48, PlayerRankTier::BrigadierGeneral },
            { 50, PlayerRankTier::General },
        };
        static_assert(sizeof(kBands) / sizeof(kBands[0]) ==
                          static_cast<size_t>(PlayerRankTier::Count),
                      "every rank tier needs a band");
        PlayerRankTier tier = PlayerRankTier::Private;
        for (const TierBand& band : kBands) {
            if (level >= band.minLevel) tier = band.tier;
        }
        return tier;
    }

    // Formats with thousands separators into the caller's buffer: "12,450".
    // Hand-rolled rather than via a locale, which would have to be imbued on
    // every stream and still varies by machine. Matches MoneySystem::Format
    // without the currency sign.
    static void Format(char* out, size_t capacity, int64_t amount) {
        if (!out || capacity == 0) return;
        char digits[32];
        const bool negative = amount < 0;
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
        for (int i = digitCount - 1; i >= 0; --i) {
            text.push_back(digits[i]);
            if (i > 0 && i % 3 == 0) text.push_back(',');
        }
        snprintf(out, capacity, "%s", text.c_str());
    }

    void ResetCareer() {
        totalXp_ = 0;
        sessionXp_ = 0;
        lifetimeKills_ = 0;
        pending_.clear();
        pendingLevelUps_.clear();
    }

    // Load path. Queues nothing: restoring a level-40 profile at startup must
    // not fire thirty-nine promotion banners on the first frame the HUD draws.
    void SetProgress(int64_t totalXp, int64_t lifetimeKills) {
        totalXp_ = (std::max)(static_cast<int64_t>(0), totalXp);
        lifetimeKills_ = (std::max)(static_cast<int64_t>(0), lifetimeKills);
    }

    // Difficulty's grip on the ladder, set from the same deployment dial the
    // wallet reads. Clamped to the range the slider offers, so a bad value can
    // neither stall a career nor race it to the cap.
    void SetRewardMultiplier(float multiplier) {
        rewardMultiplier_ = (std::max)(0.25f, (std::min)(3.0f, multiplier));
    }
    float RewardMultiplier() const { return rewardMultiplier_; }

private:
    // An award the player was shown never rounds away to nothing: the popup and
    // the bar have to agree that something happened.
    int ScaleReward(int amount) const {
        if (amount <= 0 || rewardMultiplier_ == 1.0f) return amount;
        const int scaled = static_cast<int>(
            std::lround(static_cast<double>(amount) * rewardMultiplier_));
        return scaled > 0 ? scaled : 1;
    }

    // Applies one payout, queues it for the HUD, and queues one LevelUpEvent per
    // boundary crossed. A single large award (a comm tower on a fresh profile)
    // can cross several at once, so this is a loop rather than an equality test.
    int AwardAmount(int amount, XpEvent event, const char* label) {
        amount = ScaleReward(amount);
        if (amount <= 0) return 0;
        const int before = Level();
        totalXp_ += amount;
        sessionXp_ += amount;
        pending_.push_back(XpAward{ amount, event, label });
        const int after = Level();
        for (int level = before + 1; level <= after; ++level) {
            const PlayerRankTier tier = TierForLevel(level);
            pendingLevelUps_.push_back(
                LevelUpEvent{ level, tier, tier != TierForLevel(level - 1) });
        }
        return amount;
    }

    int64_t totalXp_ = 0;
    int64_t sessionXp_ = 0;
    int64_t lifetimeKills_ = 0;
    // A run setting, not a run counter: BeginRun leaves it alone, because the
    // difficulty a run was armed at has to survive into the run.
    float rewardMultiplier_ = 1.0f;
    std::vector<XpAward> pending_;
    std::vector<LevelUpEvent> pendingLevelUps_;
};

inline const char* ProfileSavePath() { return "profile.ini"; }

// Missing file is a fresh career, not a failure: the caller keeps the zeroed
// profile already in `out`.
inline bool LoadProfile(RankSystem& out) {
    std::ifstream file(ProfileSavePath());
    if (!file) return false;

    // Seeded from what the caller already holds, so a file that exists but is
    // missing a key (an older build's profile, a hand-trimmed one) keeps what
    // it had rather than silently zeroing the player out.
    int64_t totalXp = out.TotalXp(), lifetimeKills = out.LifetimeKills();
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
        if (key == "TotalXp") totalXp = std::strtoll(value.c_str(), nullptr, 10);
        else if (key == "LifetimeKills")
            lifetimeKills = std::strtoll(value.c_str(), nullptr, 10);
    }
    out.SetProgress(totalXp, lifetimeKills);
    return true;
}

// Level and rank are deliberately written as comments rather than keys: they
// are derived from TotalXp, and a stored copy would go stale the moment the
// curve is retuned. They are here so the file is readable at a glance.
inline bool SaveProfile(const RankSystem& rank) {
    std::ofstream file(ProfileSavePath(), std::ios::trunc);
    if (!file) return false;
    file << "; Smallest Graphics Engine career profile.\n"
         << "; Delete this file to start over at level 1.\n"
         << "; Rank: " << rank.RankLabel() << " (level " << rank.Level() << ")\n"
         << "[Progress]\n"
         << "TotalXp=" << rank.TotalXp() << "\n"
         << "LifetimeKills=" << rank.LifetimeKills() << "\n";
    return file.good();
}

#endif
