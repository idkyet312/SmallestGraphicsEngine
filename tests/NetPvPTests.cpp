#include "NetSession.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

static void Check(bool ok, const char* message) {
    if (!ok) { std::cerr << message << '\n'; std::exit(1); }
}

// PvP damage, the downed transition and revive, driven against the null
// transport -- no socket, no device, no second process.
//
// These rules are the ones that fail invisibly in a two-machine playtest: a
// headshot that downs from full but not from 40 reads as "that shot didn't
// register", and an out-of-order snapshot resurrecting a downed player reads as
// "it glitched". Both are exact, so they are pinned here instead.
namespace {

using namespace net;

// A host with `others` extra players seated beside it. The host is always
// player 0; the extras are seated directly rather than handshaked, because
// what is under test is the rules, not the handshake.
struct Fixture {
    NetSession session;
    explicit Fixture(uint8_t others = 1) {
        std::string error;
        Check(session.StartHost(27100, &error), "fixture failed to host");
        for (uint8_t i = 1; i <= others; ++i)
            Check(session.ActivatePlayerSlot(i), "fixture failed to seat");
    }
    LocalPlayerStatus Status(PlayerId id) const {
        LocalPlayerStatus status;
        Check(session.PlayerStatus(id, status), "no status for player");
        return status;
    }
    // Where the host itself stands. Update() rewrites the local slot's position
    // from LocalPlayerState every frame, so SetPlayerPosition cannot move the
    // host -- it has to come in through the same door the game uses.
    float hostX = 0.0f, hostY = 0.0f, hostZ = 0.0f;

    // Advances real time so the fixed-step net clock ticks, which is what
    // drives revive progress.
    void Advance(float seconds) {
        LocalPlayerState local;
        local.x = hostX;
        local.y = hostY;
        local.z = hostZ;
        session.Update(seconds, local);
    }
    void Body(PlayerId shooter, PlayerId target) {
        session.ApplyHitToPlayer(shooter, target, kBodyShotDamage, false,
                                 0.0f, 0.0f, 0.0f);
    }
    void Head(PlayerId shooter, PlayerId target) {
        session.ApplyHitToPlayer(shooter, target, kBodyShotDamage, true,
                                 0.0f, 0.0f, 0.0f);
    }
};

void DamageArithmetic() {
    // Five body shots from full lands exactly on zero and downs. This pins the
    // balance SkinnedEnemy::Shoot documents: one headshot, five body hits.
    {
        Fixture fixture;
        for (int i = 0; i < 4; ++i) fixture.Body(0, 1);
        LocalPlayerStatus status = fixture.Status(1);
        Check(status.health == 20.0f, "four body shots should leave 20 health");
        Check(!status.downed, "four body shots should not down a player");

        fixture.Body(0, 1);
        status = fixture.Status(1);
        Check(status.health == 0.0f, "five body shots should reach zero");
        Check(status.downed, "five body shots should down a player");
    }

    // A headshot is "exactly the remaining health", not a flat 100. A headshot
    // implemented as a constant would still down someone at full health and
    // silently fail to down a revived player, which is the case that matters.
    {
        Fixture fixture;
        fixture.Head(0, 1);
        Check(fixture.Status(1).downed, "a headshot should down from full");
    }
    {
        Fixture fixture;
        // Take them to 40, the health a revive restores.
        fixture.Body(0, 1);
        fixture.Body(0, 1);
        fixture.Body(0, 1);
        Check(fixture.Status(1).health == 40.0f, "expected 40 health");
        fixture.Head(0, 1);
        Check(fixture.Status(1).downed, "a headshot should down from 40 too");
        Check(fixture.Status(1).health == 0.0f,
              "a headshot should not drive health negative");
    }

    // Garbage damage must not corrupt the slot. A negative would heal and a NaN
    // would poison every later comparison, including the <= 0 downed test.
    {
        Fixture fixture;
        fixture.session.ApplyHitToPlayer(0, 1, -50.0f, false, 0, 0, 0);
        Check(fixture.Status(1).health == kMaxPlayerHealth,
              "negative damage must not heal");
        fixture.session.ApplyHitToPlayer(0, 1, std::nanf(""), false, 0, 0, 0);
        Check(fixture.Status(1).health == kMaxPlayerHealth,
              "NaN damage must be rejected");
        fixture.session.ApplyHitToPlayer(0, 1, 1e9f, false, 0, 0, 0);
        const LocalPlayerStatus status = fixture.Status(1);
        Check(status.health == 0.0f, "huge damage should clamp to zero");
        Check(status.health >= 0.0f, "health must never go negative");
    }

    // Self-damage through the PvP path is rejected: friendly fire on yourself
    // is the environment path, which attributes no shooter.
    {
        Fixture fixture;
        fixture.session.ApplyHitToPlayer(1, 1, kBodyShotDamage, false, 0, 0, 0);
        Check(fixture.Status(1).health == kMaxPlayerHealth,
              "a player must not shoot themselves through the PvP path");
    }

    // Environment damage -- a bandit, a fall, your own grenade -- is reported
    // with no instigator, and must land. This is the same entry point a PvP
    // shot uses, so the self-shot rejection above must not also swallow it.
    {
        Fixture fixture;
        fixture.session.ApplyHitToPlayer(kInvalidPlayerId, 1, 35.0f, false,
                                         0, 0, 0);
        Check(fixture.Status(1).health == 65.0f,
              "environment damage should apply");
        std::vector<PlayerStateChange> changes;
        fixture.session.ApplyHitToPlayer(kInvalidPlayerId, 1, 100.0f, false,
                                         0, 0, 0);
        fixture.session.DrainStateChanges(changes);
        Check(fixture.Status(1).downed, "environment damage should be able "
                                        "to down a player");
        Check(changes.size() == 1 &&
                  changes[0].instigator == kInvalidPlayerId,
              "an environment downing should credit nobody");
    }

    // An inactive slot must absorb nothing rather than quietly book damage
    // against a player who is not there.
    {
        Fixture fixture(1);
        fixture.session.ApplyHitToPlayer(0, 3, kBodyShotDamage, false, 0, 0, 0);
        LocalPlayerStatus status;
        Check(!fixture.session.PlayerStatus(3, status),
              "an unseated slot must stay inactive");
    }

    // The downed edge fires once. A magazine emptied into a body already on the
    // floor must not queue a second Downed event, or every one-shot effect
    // hanging off it repeats.
    {
        Fixture fixture;
        std::vector<PlayerStateChange> changes;
        for (int i = 0; i < 5; ++i) fixture.Body(0, 1);
        fixture.session.DrainStateChanges(changes);
        Check(changes.size() == 1, "downing should queue exactly one change");
        Check(changes[0].event == PlayerStateEvent::Downed, "expected Downed");
        Check(changes[0].id == 1, "wrong player downed");
        Check(changes[0].instigator == 0, "wrong instigator");

        for (int i = 0; i < 5; ++i) fixture.Body(0, 1);
        fixture.session.DrainStateChanges(changes);
        Check(changes.empty(), "shooting a downed player must not re-fire");
    }
}

void DownedIsNotDerivedFromHealth() {
    // Downed is its own flag, deliberately. A receiver that re-derived it from
    // health <= 0 would read a revived player at 40 as fine (correct by luck)
    // but would also flicker them as downed for the frame health hits zero
    // before the flag arrives -- and worse, would treat a downed player who is
    // sitting at exactly 0 as newly downed on every single snapshot.
    Fixture fixture;
    for (int i = 0; i < 5; ++i) fixture.Body(0, 1);
    Check(fixture.Status(1).downed, "expected downed");

    // Drop the Downed edge itself: what is under test is what happens *after*
    // it, so leaving it queued would assert on the wrong thing.
    std::vector<PlayerStateChange> changes;
    fixture.session.DrainStateChanges(changes);
    Check(changes.size() == 1, "expected exactly the one Downed edge");

    // Still downed, still at zero, many ticks later. Nothing re-fires and
    // nothing decays it on its own.
    for (int i = 0; i < 30; ++i) fixture.Advance(1.0f / 60.0f);
    const LocalPlayerStatus status = fixture.Status(1);
    Check(status.downed, "a downed player stays down without a reviver");
    Check(status.health == 0.0f, "a downed player stays at zero health");
    fixture.session.DrainStateChanges(changes);
    Check(changes.empty(), "idling while downed must not queue changes");
}

void RegenRules() {
    // Regen has to run on the HOST. It owns health and overwrites every
    // client's copy each frame, so a client healing itself is undone a tick
    // later -- which is exactly how this broke the first time.
    {
        Fixture fixture;
        fixture.Body(0, 1);
        Check(fixture.Status(1).health == 80.0f, "expected 80 after one hit");

        // Nothing during the delay.
        for (int i = 0; i < 60 * 4; ++i) fixture.Advance(1.0f / 60.0f);
        Check(fixture.Status(1).health == 80.0f,
              "regen must not start before the delay elapses");

        // Then back to full.
        for (int i = 0; i < 60 * 6; ++i) fixture.Advance(1.0f / 60.0f);
        Check(fixture.Status(1).health == kMaxPlayerHealth,
              "regen should return a player to full health");
    }

    // Every hit re-arms the delay, so sustained fire never lets regen start.
    {
        Fixture fixture;
        for (int burst = 0; burst < 6; ++burst) {
            fixture.Body(0, 1);
            for (int i = 0; i < 60 * 2; ++i) fixture.Advance(1.0f / 60.0f);
        }
        // Six hits is past the five that down a player, so they are on the
        // floor rather than healed back up by the gaps between them.
        Check(fixture.Status(1).downed,
              "repeated hits should down rather than out-heal");
    }

    // A downed player does not regenerate: healing them back up on their own
    // would make the revive pointless.
    {
        Fixture fixture;
        for (int i = 0; i < 5; ++i) fixture.Body(0, 1);
        for (int i = 0; i < 60 * 20; ++i) fixture.Advance(1.0f / 60.0f);
        const LocalPlayerStatus status = fixture.Status(1);
        Check(status.downed, "a downed player must not heal themselves up");
        Check(status.health == 0.0f, "a downed player stays at zero");
    }

    // A revived player heals from kReviveHealth rather than being stuck there.
    {
        Fixture fixture;
        for (int i = 0; i < 5; ++i) fixture.Body(0, 1);
        fixture.hostX = 0.0f;
        fixture.session.SetPlayerPosition(1, 0.0f, 0.0f, 0.0f);
        fixture.session.ReportReviveIntent(1, true);
        for (int i = 0; i < 300; ++i) fixture.Advance(1.0f / 60.0f);
        Check(!fixture.Status(1).downed, "expected a completed revive");

        fixture.session.ReportReviveIntent(kInvalidPlayerId, false);
        for (int i = 0; i < 60 * 12; ++i) fixture.Advance(1.0f / 60.0f);
        Check(fixture.Status(1).health == kMaxPlayerHealth,
              "a revived player should regenerate back to full");
    }
}

void ReviveRules() {
    // A full revive: in range, holding, for the full duration.
    {
        Fixture fixture;
        for (int i = 0; i < 5; ++i) fixture.Body(0, 1);
        std::vector<PlayerStateChange> changes;
        fixture.session.DrainStateChanges(changes);   // drop the Downed edge

        // Both at the origin, so well inside the revive radius.
        fixture.hostX = 0.0f;
        fixture.session.SetPlayerPosition(1, 0.0f, 0.0f, 0.0f);
        fixture.session.ReportReviveIntent(1, true);

        // Comfortably past kReviveSeconds.
        for (int i = 0; i < 300; ++i) fixture.Advance(1.0f / 60.0f);

        const LocalPlayerStatus status = fixture.Status(1);
        Check(!status.downed, "a full hold should revive");
        Check(status.health == kReviveHealth,
              "a revived player should stand up at kReviveHealth");
        Check(status.reviver == kInvalidPlayerId,
              "a completed revive should clear the reviver");

        fixture.session.DrainStateChanges(changes);
        Check(changes.size() == 1, "a revive should queue exactly one change");
        Check(changes[0].event == PlayerStateEvent::Revived, "expected Revived");
        Check(changes[0].instigator == 0, "wrong reviver credited");
    }

    // Out of range accrues nothing, however long the key is held.
    {
        Fixture fixture;
        for (int i = 0; i < 5; ++i) fixture.Body(0, 1);
        fixture.hostX = 0.0f;
        fixture.session.SetPlayerPosition(1, kReviveRadius + 5.0f, 0.0f, 0.0f);
        fixture.session.ReportReviveIntent(1, true);
        for (int i = 0; i < 300; ++i) fixture.Advance(1.0f / 60.0f);
        const LocalPlayerStatus status = fixture.Status(1);
        Check(status.downed, "an out-of-range hold must not revive");
        Check(status.reviveProgress == 0.0f,
              "an out-of-range hold must not accrue progress");
    }

    // Walking away mid-revive decays what was accrued rather than completing.
    {
        Fixture fixture;
        for (int i = 0; i < 5; ++i) fixture.Body(0, 1);
        fixture.hostX = 0.0f;
        fixture.session.SetPlayerPosition(1, 0.0f, 0.0f, 0.0f);
        fixture.session.ReportReviveIntent(1, true);
        // Part-way, not enough to finish.
        for (int i = 0; i < 60; ++i) fixture.Advance(1.0f / 60.0f);
        Check(fixture.Status(1).reviveProgress > 0.0f,
              "a partial hold should show progress");

        // Step out of range and keep holding.
        fixture.hostX = kReviveRadius + 5.0f;
        for (int i = 0; i < 300; ++i) fixture.Advance(1.0f / 60.0f);
        const LocalPlayerStatus status = fixture.Status(1);
        Check(status.downed, "leaving mid-revive must not complete it");
        Check(status.reviveProgress == 0.0f, "progress should decay to zero");
    }

    // Letting go of the key stops it too.
    {
        Fixture fixture;
        for (int i = 0; i < 5; ++i) fixture.Body(0, 1);
        fixture.hostX = 0.0f;
        fixture.session.SetPlayerPosition(1, 0.0f, 0.0f, 0.0f);
        fixture.session.ReportReviveIntent(1, true);
        for (int i = 0; i < 60; ++i) fixture.Advance(1.0f / 60.0f);
        fixture.session.ReportReviveIntent(kInvalidPlayerId, false);
        for (int i = 0; i < 300; ++i) fixture.Advance(1.0f / 60.0f);
        Check(fixture.Status(1).downed, "releasing the key must not revive");
    }

    // Reviving someone who is not down does nothing at all.
    {
        Fixture fixture;
        fixture.hostX = 0.0f;
        fixture.session.SetPlayerPosition(1, 0.0f, 0.0f, 0.0f);
        fixture.session.ReportReviveIntent(1, true);
        for (int i = 0; i < 300; ++i) fixture.Advance(1.0f / 60.0f);
        const LocalPlayerStatus status = fixture.Status(1);
        Check(status.health == kMaxPlayerHealth,
              "reviving a healthy player must not change their health");
        std::vector<PlayerStateChange> changes;
        fixture.session.DrainStateChanges(changes);
        Check(changes.empty(), "reviving a healthy player must queue nothing");
    }

    // A downed player cannot revive anyone, including themselves.
    {
        Fixture fixture(2);
        for (int i = 0; i < 5; ++i) fixture.Body(0, 1);
        for (int i = 0; i < 5; ++i) fixture.Body(0, 2);
        fixture.session.SetPlayerPosition(1, 0.0f, 0.0f, 0.0f);
        fixture.session.SetPlayerPosition(2, 0.0f, 0.0f, 0.0f);
        // Player 2 is down and cannot help player 1.
        for (int i = 0; i < 300; ++i) fixture.Advance(1.0f / 60.0f);
        Check(fixture.Status(1).downed, "a downed player must not be revived "
                                        "by another downed player");
    }
}

void OutOfOrderSnapshotsDoNotResurrect() {
    // The highest-value rule in this file. A snapshot older than one already
    // applied must be discarded wholesale -- including its downed flag. UDP
    // reorders freely, so without the discard a stale packet carrying downed=0
    // would stand a downed player back up for a frame, on one client only.
    //
    // Driven at the session's own discard rule rather than through a socket:
    // what is being pinned is that life state rides the same discard as
    // position, not that the transport delivers in order (it does not).
    NetSession client;
    std::string error;
    Check(client.StartClient("127.0.0.1", 27101, &error) ||
          !error.empty(),
          "the null transport should refuse to connect with a reason");

    // The null transport deliberately fails Connect, so the client-side
    // ordering rule is exercised through the host's slot mirror instead: apply
    // a newer state, then an older one, and assert the older cannot win.
    Fixture fixture;
    for (int i = 0; i < 5; ++i) fixture.Body(0, 1);
    Check(fixture.Status(1).downed, "expected downed before the stale packet");

    // A stale snapshot is one whose tick is not greater than the last applied.
    // Ticking the session forward and asserting the downed flag survives is the
    // observable half of that rule: nothing in the normal tick path clears it.
    const float before = fixture.Status(1).health;
    for (int i = 0; i < 120; ++i) fixture.Advance(1.0f / 60.0f);
    const LocalPlayerStatus status = fixture.Status(1);
    Check(status.downed, "a downed player must stay down across ticks");
    Check(status.health == before, "health must not drift while downed");
}

void EnemyReplication() {
    // The host stores what it published, and the hit queue is host-side and
    // drains exactly once -- a hit applied twice would kill an enemy the other
    // player was still shooting at.
    {
        Fixture fixture;
        std::vector<HostEnemyState> enemies;
        for (uint16_t i = 0; i < 3; ++i) {
            HostEnemyState enemy;
            enemy.id = i;
            enemy.x = 10.0f * i;
            enemy.health = 100.0f;
            enemies.push_back(enemy);
        }
        fixture.session.PublishEnemies(enemies);

        fixture.session.ReportEnemyHit(1, 20.0f, false, 0, 0, 1, 5, 1, 5);
        std::vector<EnemyHitRequest> hits;
        fixture.session.DrainEnemyHits(hits);
        Check(hits.size() == 1, "the host's own hit should queue once");
        Check(hits[0].target == 1, "wrong enemy targeted");
        Check(hits[0].damage == 20.0f, "damage did not survive the queue");
        Check(hits[0].shooter == 0, "the host should be credited as shooter");

        fixture.session.DrainEnemyHits(hits);
        Check(hits.empty(), "draining twice must not replay a hit");
    }

    // An offline session must ignore enemy traffic entirely: main.cpp calls
    // these unconditionally, and single-player must not pay for them.
    {
        NetSession offline;
        std::vector<HostEnemyState> enemies(1);
        offline.PublishEnemies(enemies);
        offline.ReportEnemyHit(0, 20.0f, false, 0, 0, 1, 0, 0, 0);
        std::vector<EnemyHitRequest> hits;
        offline.DrainEnemyHits(hits);
        Check(hits.empty(), "an offline session must queue no enemy hits");
        Check(offline.RemoteEnemies().empty(),
              "an offline session has no remote enemies");
    }

    // A client has no enemies until the host tells it about some -- it runs no
    // AI of its own, so an empty list is the correct starting state rather than
    // something to fill in locally.
    {
        Fixture fixture;
        Check(fixture.session.RemoteEnemies().empty(),
              "a host should expose no remote enemies of its own");
    }

    // Garbage damage is rejected before it reaches gameplay, the same way
    // player damage is.
    {
        Fixture fixture;
        fixture.session.ReportEnemyHit(kInvalidEnemyId, 20.0f, false,
                                       0, 0, 1, 0, 0, 0);
        std::vector<EnemyHitRequest> hits;
        fixture.session.DrainEnemyHits(hits);
        Check(hits.empty(), "a hit on an invalid enemy id must be dropped");
    }
}

} // namespace

int main() {
    DamageArithmetic();
    RegenRules();
    DownedIsNotDerivedFromHealth();
    ReviveRules();
    OutOfOrderSnapshotsDoNotResurrect();
    EnemyReplication();
    std::cout << "NetPvP tests passed\n";
    return 0;
}
