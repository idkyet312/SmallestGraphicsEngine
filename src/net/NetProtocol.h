#ifndef NET_PROTOCOL_H
#define NET_PROTOCOL_H

#include "PlayerInput.h"

#include <cstddef>
#include <cstdint>

// Fixed-width wire format for connection, player state, and authoritative
// gameplay events. Each message is versioned as one protocol contract so a
// peer cannot silently interpret a changed event layout as an older packet.
//
// Every struct is a fixed-width POD written and read by memcpy. No pointers, no
// std types, no virtuals. Endianness is deliberately ignored: both ends are
// x86-64 Windows builds of the same engine, and the version handshake below
// rejects anything else before a single field is read.
namespace net {

// Bumped whenever any struct in this file changes shape. A mismatch is refused
// at connect time with a clear message -- silently misreading a peer's bytes is
// far worse to debug than being told the builds differ.
// 2: PlayerSnapshot gained health/downed/reviver, and PvP hit reporting,
// player state events and revive progress were added.
// 3: enemies are host-authoritative and ride in their own snapshot.
// 4: PlayerSnapshot gained reviveProgress, without which a client could never
//    draw a revive bar -- the host was the only machine that knew the number.
// 5: client input carries the locally simulated feet position.
// 6: authoritative world break and grenade lifecycle messages.
// 7: world impacts and breaks name their shooter, so a receiver can tell
//    someone else's bullet (draw the impact) from its own coming back (already
//    drawn), and carry whether the round was player fire.
// 8: the host names the level it is on, so a joining client loads the same map.
// 9: the host owns the ground. Every runtime crater and gouge is replicated as
//    a resolved sculpt stamp, so an explosion leaves the same hole on every
//    machine instead of each one cutting its own from its own tunables.
inline constexpr uint32_t kProtocolVersion = 9;

// A magic word in the hello guards against something other than this game
// connecting to the port and having its bytes read as a handshake.
inline constexpr uint32_t kProtocolMagic = 0x53474531u; // "SGE1"

inline constexpr uint8_t kMaxPlayers = 4;
using PlayerId = uint8_t;
inline constexpr PlayerId kInvalidPlayerId = 0xFF;

// Enemies are identified by a stable id assigned at spawn, not by their index
// in g_bandits -- that vector is compacted when bodies are removed, which would
// silently renumber every actor after the gap and hand a client the wrong one.
using EnemyId = uint16_t;
inline constexpr EnemyId kInvalidEnemyId = 0xFFFF;
// How many enemies ride in one snapshot. A level holds far more than this, so
// the host sends the nearest few to each client: at combat range that is every
// enemy that matters, and it keeps the message inside one datagram no matter
// how large the level gets. Distant actors pop in as a player approaches.
inline constexpr uint8_t kMaxReplicatedEnemies = 16;

enum class MessageType : uint8_t {
    ClientHello = 1,   // client -> host, reliable
    ServerWelcome,     // host -> client, reliable
    ServerReject,      // host -> client, reliable (then disconnect)
    ClientInput,       // client -> host, unreliable, every net tick
    ServerSnapshot,    // host -> client, unreliable, every net tick
    PlayerJoined,      // host -> client, reliable
    PlayerLeft,        // host -> client, reliable
    ClientHitReport,   // client -> host, reliable
    ServerPlayerStateChanged, // host -> client, reliable
    ClientReviveProgress,     // client -> host, unreliable
    ServerEnemySnapshot,      // host -> client, unreliable, every net tick
    ClientEnemyHitReport,     // client -> host, reliable
    ClientWorldImpact,        // client -> host, reliable
    ServerWorldBreak,         // host -> clients, reliable
    ClientGrenadeThrow,       // client -> host, reliable
    ServerGrenadeSpawn,       // host -> clients, reliable
    ServerGrenadeDetonated,   // host -> clients, reliable
    ServerLevel,              // host -> clients, reliable
    ServerTerrainDeform,      // host -> clients, reliable
    ClientTerrainDeform,      // client -> host, reliable
    ClientGrenadeDetonation,  // client -> host, reliable
};

// One-shot transitions in a player's life state. Carried by a reliable message
// rather than inferred from the snapshot, because each one drives an effect
// that must happen exactly once: the pose change, the audio, the callout.
enum class PlayerStateEvent : uint8_t {
    Downed = 1,
    Revived,
    // Reserved so adding respawn later costs no version bump.
    Respawned,
};

enum class RejectReason : uint8_t {
    VersionMismatch = 1,
    ServerFull,
};

// Every message starts with this so the receiver can dispatch before it knows
// the payload shape.
struct MessageHeader {
    MessageType type = MessageType::ClientHello;
    uint8_t padding[3] = {};
};

struct ClientHello {
    MessageHeader header{ MessageType::ClientHello, {} };
    uint32_t magic = kProtocolMagic;
    uint32_t version = kProtocolVersion;
};

struct ServerWelcome {
    MessageHeader header{ MessageType::ServerWelcome, {} };
    uint32_t version = kProtocolVersion;
    PlayerId assignedId = kInvalidPlayerId;
    uint8_t padding[3] = {};
};

struct ServerReject {
    MessageHeader header{ MessageType::ServerReject, {} };
    RejectReason reason = RejectReason::VersionMismatch;
    uint8_t padding[3] = {};
    uint32_t serverVersion = kProtocolVersion;
};

struct ClientInputMessage {
    MessageHeader header{ MessageType::ClientInput, {} };
    PlayerInput input;
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

// One player's replicated state. Weapons and ammo are still local-only; health
// and the downed flag are here because they are what everyone has to agree on.
//
// This is the authoritative copy. Whatever a snapshot says is true, and because
// it arrives every tick it self-heals a client that missed an event. The
// reliable ServerPlayerStateChanged below carries only the *edge* and never
// writes state, which is what stops the two from ever disagreeing.
struct PlayerSnapshot {
    PlayerId id = kInvalidPlayerId;
    // Packed movement state so the remote body can pick a clip without the
    // receiver re-deriving it from position deltas.
    uint8_t moving = 0;
    uint8_t crouching = 0;
    uint8_t sprinting = 0;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float yaw = 0.0f, pitch = 0.0f;
    float health = 100.0f;
    // Not derived from health <= 0 on the receiver: a revived player sits at
    // partial health and must not read as downed, and a downed player sits at
    // exactly 0 for as long as it takes someone to reach them.
    uint8_t downed = 0;
    // Who is currently reviving this player, or kInvalidPlayerId. Replicated so
    // every client can draw the progress, not just the two players involved.
    PlayerId reviver = kInvalidPlayerId;
    uint8_t padding[2] = {};
    // Seconds of hold the host has credited, NOT normalised -- receivers divide
    // by kReviveSeconds themselves (PlayerStatus, LocalStatus, GetRemotePlayers
    // all already do), so normalising here would divide twice.
    //
    // Replicated rather than timed locally because a locally timed bar would
    // fill even on the ticks the host is rejecting the hold for range, which is
    // exactly the case the bar exists to show. Without this on the wire the
    // field is host-only: StepRevives is the sole writer, so a client's copy sat
    // at zero forever and its revive bar never drew at all.
    //
    // Lands on a 4-byte boundary: the padding above closes out `reviver`.
    float reviveProgress = 0.0f;
};

struct ServerSnapshotMessage {
    MessageHeader header{ MessageType::ServerSnapshot, {} };
    // Host tick this snapshot describes. The client interpolates between the
    // two most recent and discards anything older than what it already has,
    // which is what makes out-of-order UDP delivery harmless.
    uint32_t tick = 0;
    uint8_t playerCount = 0;
    uint8_t padding[3] = {};
    PlayerSnapshot players[kMaxPlayers];
};

struct PlayerJoinedMessage {
    MessageHeader header{ MessageType::PlayerJoined, {} };
    PlayerId id = kInvalidPlayerId;
    uint8_t padding[3] = {};
};

struct PlayerLeftMessage {
    MessageHeader header{ MessageType::PlayerLeft, {} };
    PlayerId id = kInvalidPlayerId;
    uint8_t padding[3] = {};
};

// Shooter-authoritative hit. The client that fired ran the geometry test
// locally, against the body it could actually see on its own screen, and
// reports the result rather than mutating anything itself.
//
// Reliable: a dropped hit is a round that silently did nothing. The shooter saw
// blood and a hit marker while the victim took no damage, which reads as the
// game being broken rather than as packet loss.
struct ClientHitReportMessage {
    MessageHeader header{ MessageType::ClientHitReport, {} };
    PlayerId target = kInvalidPlayerId;
    uint8_t headshot = 0;
    uint8_t padding[2] = {};
    float damage = 0.0f;
    // Where the round landed, for the victim's directional hit indicator.
    float hitX = 0.0f, hitY = 0.0f, hitZ = 0.0f;
    // The shooter's tick when it fired. Unused today; carried so that adding
    // lag compensation later -- rewinding the world to what the shooter saw --
    // needs no protocol bump.
    uint32_t shooterTick = 0;
};

// The edge of a life-state transition, not the state itself. Delivered reliably
// so the pose change, the ragdoll-equivalent and the audio each happen exactly
// once on every client, including one that dropped the snapshot the flip
// happened on.
struct ServerPlayerStateChangedMessage {
    MessageHeader header{ MessageType::ServerPlayerStateChanged, {} };
    PlayerId id = kInvalidPlayerId;
    PlayerStateEvent event = PlayerStateEvent::Downed;
    // Who put them down or picked them up. kInvalidPlayerId when nobody did --
    // a fall, a bandit, or their own grenade.
    PlayerId instigator = kInvalidPlayerId;
    uint8_t padding = 0;
    float health = 0.0f;
    // Direction the felling round travelled, so the downed body settles the
    // same way on every client rather than differently on each.
    float impulseX = 0.0f, impulseY = 0.0f, impulseZ = 0.0f;
    float impactX = 0.0f, impactY = 0.0f, impactZ = 0.0f;
};

// Sent every tick while the reviver holds the key in range. Unreliable and
// idempotent: losing one costs a single tick of progress, and the host re-checks
// the distance itself rather than trusting this to have stopped arriving.
struct ClientReviveProgressMessage {
    MessageHeader header{ MessageType::ClientReviveProgress, {} };
    PlayerId target = kInvalidPlayerId;
    uint8_t holding = 0;
    uint8_t padding[2] = {};
};

// One AI actor, as the host sees it. Clients do not run enemy AI at all: they
// render what arrives here, so this has to carry everything the body needs to
// be drawn and animated, not just where it is.
struct EnemySnapshot {
    EnemyId id = kInvalidEnemyId;
    // Packed pose state, so the client can pick a clip without re-deriving it
    // from position deltas -- which would lag a frame behind and read as the
    // animation sticking.
    uint8_t moving = 0;
    uint8_t dead = 0;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    // Radians, matching SkinnedEnemy. Both are sent because the upper body aims
    // independently of the legs, and a client that guessed one from the other
    // would have every enemy facing its feet.
    float yaw = 0.0f;
    float aimYaw = 0.0f;
    float aimPitch = 0.0f;
    float health = 0.0f;
};

// The enemies nearest one client. Sent per-connection rather than broadcast,
// because "nearest" is different for every player.
struct ServerEnemySnapshotMessage {
    MessageHeader header{ MessageType::ServerEnemySnapshot, {} };
    uint32_t tick = 0;
    uint8_t enemyCount = 0;
    uint8_t padding[3] = {};
    EnemySnapshot enemies[kMaxReplicatedEnemies];
};

// A client's round connected with an enemy. Same shooter-authoritative bargain
// as ClientHitReport: the client ran the geometry test against the body it can
// see, and the host owns what the damage does.
struct ClientEnemyHitReportMessage {
    MessageHeader header{ MessageType::ClientEnemyHitReport, {} };
    EnemyId target = kInvalidEnemyId;
    uint8_t headshot = 0;
    uint8_t padding = 0;
    float damage = 0.0f;
    // Direction the round was travelling and where it struck, so the host's
    // ragdoll gets the same impulse the shooter saw rather than flopping a
    // different way on every machine.
    float dirX = 0.0f, dirY = 0.0f, dirZ = 0.0f;
    float hitX = 0.0f, hitY = 0.0f, hitZ = 0.0f;
};

// A client-side bullet hit on a destructible world entity. The client may play
// its immediate impact, but only the host applies the health change. This
// message is deliberately an impact rather than a "destroy" command: a shot
// can chip a prop without breaking it, and the host remains the one deciding
// when the break transition occurs.
struct ClientWorldImpactMessage {
    MessageHeader header{ MessageType::ClientWorldImpact, {} };
    uint32_t impactId = 0;
    uint8_t kind = 0; // 0 = destruction surface, 1 = prefab, 2 = tree
    // Player fire or not. Some prefabs -- the objective aircraft -- take damage
    // only from a player, so an enemy round replicated as player fire would let
    // the garrison shoot down the objective the players are sent to destroy.
    uint8_t playerOwned = 1;
    uint8_t padding[2] = {};
    uint64_t entityId = 0;
    float damage = 0.0f;
    float radius = 0.0f;
    float impulse = 0.0f;
    float dirX = 0.0f, dirY = 0.0f, dirZ = 0.0f;
    float hitX = 0.0f, hitY = 0.0f, hitZ = 0.0f;
    uint32_t shooterTick = 0;
};

// The committed break edge. Reliable because a missed fracture leaves a
// client colliding with and rendering a prop the host has already removed.
// The event is idempotent: applying it to an already-removed entity is harmless.
struct ServerWorldBreakMessage {
    MessageHeader header{ MessageType::ServerWorldBreak, {} };
    uint32_t impactId = 0;
    uint8_t kind = 0;
    // Who fired it. The shooter already drew its own sparks and decal at this
    // point the moment it pulled the trigger, so it must not draw them again
    // when its own impact comes back committed -- and everyone else has drawn
    // nothing yet and needs them. Padded out to the eight-byte boundary
    // `entityId` sits on, which the previous layout left to the compiler.
    PlayerId shooter = kInvalidPlayerId;
    uint8_t playerOwned = 1;
    uint8_t padding[5] = {};
    uint64_t entityId = 0;
    float damage = 0.0f;
    float radius = 0.0f;
    float impulse = 0.0f;
    float dirX = 0.0f, dirY = 0.0f, dirZ = 0.0f;
    float hitX = 0.0f, hitY = 0.0f, hitZ = 0.0f;
};

enum class GrenadeKind : uint8_t {
    Frag = 0,
    Molotov,
    Vortex,
};

// Client throw request. `clientToken` is unique for the throwing player's
// session and lets the client match the host's assigned id to its predicted
// projectile without echoing a second grenade into its own scene.
struct ClientGrenadeThrowMessage {
    MessageHeader header{ MessageType::ClientGrenadeThrow, {} };
    uint32_t clientToken = 0;
    GrenadeKind kind = GrenadeKind::Frag;
    uint8_t padding[3] = {};
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float velocityX = 0.0f, velocityY = 0.0f, velocityZ = 0.0f;
    float fuse = 0.0f;
};

// Host-approved throw, sent before the host simulates the grenade. Reliable so
// every client sees the same projectile even when the throw is brief or out of
// view. The host's app consumes the same event to create its authoritative copy.
struct ServerGrenadeSpawnMessage {
    MessageHeader header{ MessageType::ServerGrenadeSpawn, {} };
    uint32_t grenadeId = 0;
    uint32_t clientToken = 0;
    PlayerId owner = kInvalidPlayerId;
    GrenadeKind kind = GrenadeKind::Frag;
    uint8_t hostile = 0;
    uint8_t padding = 0;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float velocityX = 0.0f, velocityY = 0.0f, velocityZ = 0.0f;
    float fuse = 0.0f;
};

// Host committed the explosion. Clients do not decide when a remote grenade
// detonates; they move the predicted body until this edge arrives, then run
// the existing blast presentation at this center exactly once.
struct ServerGrenadeDetonatedMessage {
    MessageHeader header{ MessageType::ServerGrenadeDetonated, {} };
    uint32_t grenadeId = 0;
    GrenadeKind kind = GrenadeKind::Frag;
    uint8_t hostile = 0;
    uint8_t padding[2] = {};
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

// Where the thrower's own grenade actually came to rest.
//
// The host simulates every replicated grenade, but it is simulating a throw it
// only knows the launch pose of: a bounce off a crate or a roll down a slope
// diverges from what the player who threw it watched, and the blast -- and the
// crater under it -- then lands somewhere they did not aim. This is that
// player's answer, and the host detonates there rather than wherever its own
// copy of the projectile ended up.
//
// Same bargain as ClientHitReport: the thrower ran the throw on its own screen,
// the host still owns what the explosion does.
struct ClientGrenadeDetonationMessage {
    MessageHeader header{ MessageType::ClientGrenadeDetonation, {} };
    uint32_t grenadeId = 0;
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

// Which map the host is on. Levels are not otherwise negotiated: each player
// used to pick their own from the menu, which reads as a working session right
// up until two people shoot at terrain that only exists on one machine.
enum class LevelKind : uint8_t {
    // The host is in the menus. A client that receives this stays where it is
    // rather than unloading, because the host will name a level in a moment and
    // a trip back to the menu in between is worse than waiting.
    None = 0,
    Level1,     // the built-in campaign start, no file
    TestLevel,  // empty terrain, no gameplay actors -- the usual test bed
    LevelFile,  // Content/Levels/<file>, which covers Base and the islands
};

// Bare file name, not a path: the two ends resolve it against their own layout
// (repo, build/, packaged), which already differ, and sending an absolute path
// from one machine would name a directory the other does not have.
inline constexpr uint8_t kMaxLevelFileName = 96;

struct ServerLevelMessage {
    MessageHeader header{ MessageType::ServerLevel, {} };
    LevelKind kind = LevelKind::None;
    uint8_t padding[3] = {};
    char file[kMaxLevelFileName] = {};
};

// One runtime cut in the ground: an explosion crater, or one of the furrows a
// crashing BlackHawk ploughs.
//
// The *resolved* stamp crosses, not the blast that caused it. Radius, depth,
// wall sharpness and floor fraction are all built on the sender from live
// editor tunables (scene.explosionBlastRadius, scene.craterDepth and friends)
// and baseHeight is a local height sample, so two machines handed the same
// impact point would otherwise cut two different holes. This way the ground is
// the host's, exactly, whatever either side has its dials set to.
//
// This is the wire twin of TerrainSculptStamp, which cannot itself be sent: it
// carries a std::string for authored heightmap stamps. Runtime cuts are always
// procedural, so only the numeric fields are here.
struct TerrainDeform {
    float x = 0.0f, z = 0.0f;
    float radius = 0.0f;
    float value = 0.0f;
    float strength = 1.0f;
    float edgeFalloff = 1.0f;
    float baseHeight = 0.0f;
    // The blast's own height. The stamp is flat -- it only knows XZ -- but the
    // foliage clear and the support undermine both work in 3D and need it.
    float impactY = 0.0f;
    uint8_t operation = 0; // TerrainSculptOperation
    uint8_t padding[3] = {};
};

// Reliable, and idempotent through deformId: a stamp applied twice digs the
// hole twice as deep, and a dropped one leaves a client walking on ground the
// host has already blown away.
struct ServerTerrainDeformMessage {
    MessageHeader header{ MessageType::ServerTerrainDeform, {} };
    uint32_t deformId = 0;
    TerrainDeform deform;
};

// A client asking for a cut it cannot make itself. Rockets, C4 and exploding
// barrels are simulated locally on whichever machine fired them and are not
// replicated as projectiles at all, so without this the host never learns there
// was an explosion and nobody's ground changes -- not even the shooter's, since
// a client digs nothing on its own. The host validates and clamps this, then
// broadcasts it back as a ServerTerrainDeform like any cut of its own, which is
// what keeps one authority over the ground.
//
// Same shooter-authoritative bargain as ClientHitReport: the client already ran
// the blast on its own screen, and the host owns what it does to the world.
struct ClientTerrainDeformMessage {
    MessageHeader header{ MessageType::ClientTerrainDeform, {} };
    TerrainDeform deform;
};

// How many cuts the host keeps to replay to someone joining mid-session.
// Mirrors kMaxTerrainSculptStamps in LevelDefinition.h, which is where the
// engine drops its oldest runtime stamp; this header stays free of engine
// includes, so the number is repeated rather than shared. Keeping the two equal
// is what stops the backlog replaying a cut the host itself has already evicted.
inline constexpr size_t kMaxReplicatedTerrainDeforms = 1024;

} // namespace net

#endif // NET_PROTOCOL_H
