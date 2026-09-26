#ifndef NET_PROTOCOL_H
#define NET_PROTOCOL_H

#include "PlayerInput.h"
#include "ChargeAnchor.h"

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
// 10: each player publishes their insertion helicopter for remote rendering.
// 15: demolition charges are session state. Where one is planted and the
//     moment its owner fires it both replicate, so a charge rigged by one
//     player is on the wall for everyone and brings the target down for
//     everyone rather than only for whoever pressed the detonator.
// 14: players publish whether they are aiming, so a remote body raises its
//     weapon into a sighted hold instead of carrying it lowered whatever its
//     owner is doing.
// 13: players announce the rounds they fire, so another player's gunfire is
//     visible and audible where it happens instead of being a silent,
//     invisible event you only learn about when something near you dies.
// 12: the two enemy gunships are the host's. Their flight, their health and
//     whether they have been shot down all replicate, so a helicopter that a
//     client destroys stops flying on every machine instead of only its own.
// 11: world impacts carry whether they came from a remote charge, so the
//     objectives a charge is the only thing allowed to destroy -- the comm
//     tower, the objective aircraft -- replicate instead of collapsing on the
//     machine that set the charge and standing on every other one.
// 19: text chat. Clients send a typed line to the host, which stamps it with
//     the speaker and broadcasts it to everyone including the sender, so every
//     machine shows the same lines in the same order.
// 18: god mode is session-wide and follows the host. The host owns every
//     player's health, so a client's own toggle was a flag the damage path
//     never consulted. The host's toggle now rides in PlayerSnapshot's last
//     padding byte, the host skips all player damage while it is on, and
//     clients mirror it. The struct keeps its size.
// 17: enemy snapshots name the player who killed the body. Without it a client
//     could only be told an enemy was dead, never by whom, so every machine
//     paid itself for every death anywhere on the map. EnemySnapshot grows
//     from 32 to 36 bytes -- `id` is a uint16, so id+moving+dead already
//     filled the four bytes before the first float and alignment re-pads. At
//     16 enemies that is 576 bytes of payload, still inside one datagram.
// 16: host-authoritative exfil boat state.
// 20: enemy tanks and AA emplacements are the host's. Their pose, health and
//     death replicate in ServerArmorState, and each round they fire is a
//     ServerEnemyFire, so a tank that one player kills stops for everyone and
//     every player sees -- and can be hit by -- the same shell. World impacts
//     gained kinds 4 (tank, by level entity id) and 5 (AA turret) for a
//     client's own hits on them.
// 21: ServerArmorState carries the level Humvees, whose gunners now drive them
//     at the player on the host. humveeCount takes a padding byte; the
//     message grows by 8 x 36 bytes, still inside one datagram.
// 22: ServerEnemyFire carries damageScale, so a light gun's shells hurt a
//     client's player as little as they hurt the host's.
// 23: ServerArmorState carries the objective aircraft (planeCount takes the
//     padding byte; 4 x 56 bytes), so every player sees the same takeoff and
//     the host's escape fails the mission for everyone. The insertion
//     helicopter state says whether that player has deployed, which is what
//     starts the aircraft's countdown. ServerSquadDeploy lets the host drop
//     the clients still planning in with its own insertion.
// 24: ServerEnemyFire carries bandit infantry shots (InfantryShot, weapon
//     byte from the padding), so enemy soldiers can hurt clients. Explosive
//     barrels are host-owned (Client/ServerBarrelEvent), a client's rocket
//     blast reaches the host's soldiers (ClientBlast), and PlayerSnapshot and
//     ClientInput carry the vehicle a player is driving (DrivenVehicleState).
// 25: ServerScoreboard (per-player kills/deaths/downs) and Client/ServerMedicCall.
// 26: a client's bought marines are the host's. ClientMarineDrop asks the host
//     to land them where the client's transport set down, and EnemySnapshot
//     says which bodies are marines (a padding byte after `killer`, so the
//     struct keeps its 36 bytes) -- a client built every replica as a bandit.
// 27: C4 carries a turret attachment and a host charge id for late-join replay.
inline constexpr uint32_t kProtocolVersion = 27;

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
    ServerVehicleState,       // host -> clients, unreliable, every net tick
    ClientShotFired,          // client -> host, unreliable
    ServerShotFired,          // host -> clients, unreliable
    ClientChargeStuck,        // client -> host, reliable
    ServerChargeStuck,        // host -> clients, reliable
    ClientChargeDetonate,     // client -> host, reliable
    ServerChargeDetonate,     // host -> clients, reliable
    ClientChatMessage,        // client -> host, reliable
    ServerChatMessage,        // host -> clients, reliable
    ServerArmorState,         // host -> clients, unreliable, every net tick
    ServerEnemyFire,          // host -> clients, reliable (tank) / unreliable (AA)
    ServerSquadDeploy,        // host -> clients, reliable
    ClientBarrelEvent,        // client -> host, reliable
    ServerBarrelEvent,        // host -> clients, reliable
    ClientBlast,              // client -> host, reliable
    ServerScoreboard,         // host -> clients, reliable (on change)
    ClientMedicCall,          // client -> host, reliable
    ServerMedicCall,          // host -> clients, reliable
    ClientMarineDrop,         // client -> host, reliable
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

struct InsertionHelicopterState {
    uint8_t visible = 0;
    uint8_t airframe = 0;
    // This player has pressed DEPLOY and their mission clock is running. Rides
    // here because every player already sends this struct every tick; the host
    // starts the objective aircraft's countdown on the first player to deploy,
    // not on its own deploy. Claimed from the padding, so the size holds.
    uint8_t deployed = 0;
    uint8_t padding = 0;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    // Degrees, matching player-angle interpolation.
    float yaw = 0.0f, pitch = 0.0f, roll = 0.0f;
    float centerX = 0.0f, minY = 0.0f, centerZ = 0.0f;
    float scale = 1.0f;
};

enum class DrivenVehicleKind : uint8_t {
    None = 0,
    Humvee,
};

// The vehicle a player is at the wheel of. A driven vehicle is simulated by
// its driver -- that machine has the input, so its solver is the one that
// feels right -- and this is how everyone else learns where it went. The host
// puts its copy of the body there, and from then on it rides the armor state
// like any Humvee the host moved itself. In the player snapshot it tells every
// machine who is in which seat, which is what stops two players taking the
// same wheel and what hides a driver's body out at the chase camera.
struct DrivenVehicleState {
    DrivenVehicleKind kind = DrivenVehicleKind::None;
    uint8_t index = 0;   // level Humvee spawn index (EnemyHumveeSnapshot::index)
    uint8_t padding[2] = {};
    // Chassis body centre and orientation, as the driver's solver has them.
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float qx = 0.0f, qy = 0.0f, qz = 0.0f, qw = 1.0f;
    // Radians, HumveeGameplayState::turretYaw: the driver aims it too.
    float turretYaw = 0.0f;
};

struct ClientInputMessage {
    MessageHeader header{ MessageType::ClientInput, {} };
    PlayerInput input;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    InsertionHelicopterState helicopter;
    DrivenVehicleState vehicle;
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
    // Whether this player is sighted. Drives which weapon hold their body uses
    // on every other machine -- lowered at rest, raised when aiming. Claimed
    // from the padding, so the struct keeps its size and `reviveProgress` stays
    // on the 4-byte boundary the comment below relies on.
    uint8_t aiming = 0;
    // The session's god mode, which is the host's toggle. Every entry carries
    // the same value; it rides here rather than in its own message because the
    // snapshot already arrives every tick and self-heals a missed change.
    // Claimed from the last padding byte, so the struct keeps its size and
    // `reviveProgress` stays on the 4-byte boundary.
    uint8_t godMode = 0;
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
    InsertionHelicopterState helicopter;
    DrivenVehicleState vehicle;
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
    // Who brought this body down, or kInvalidPlayerId for anything that was not
    // a player -- AI crossfire, a rotor, falling debris. Only the machine this
    // names pays out for the kill, which is what stops four players each
    // banking the same body.
    //
    // This one does not come free: `id` is a uint16, so id+moving+dead already
    // filled the four bytes before the first float and there was no spare byte
    // to claim. The struct goes 32 -> 36 bytes. Sized against the datagram in
    // the changelog above rather than left to be discovered later.
    PlayerId killer = kInvalidPlayerId;
    // An allied marine rather than a bandit: the client picks the model, the
    // faction and the friendly marker from it. Claimed from the padding.
    uint8_t marine = 0;
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

// One enemy gunship, as the host flies it. Both airframes are sent every tick
// in one message rather than on change: the craft is in continuous motion, so
// there is no quiet state to diff against, and two of them fit in a packet the
// snapshot already fits beside.
struct EnemyHelicopterSnapshot {
    // Present at all -- the second airframe exists only on levels that place
    // it, and a client must not fly a ghost the host does not have.
    uint8_t present = 0;
    uint8_t dead = 0;
    uint8_t crashed = 0;
    uint8_t padding = 0;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    // Radians, matching VehicleSystem. The patrol airframe banks, so pitch and
    // roll are carried too rather than left for the client to invent.
    float yaw = 0.0f, pitch = 0.0f, roll = 0.0f;
    float health = 0.0f;
};

// Index 0 is the primary helicopter, index 1 the secondary/patrol gunship.
inline constexpr uint8_t kEnemyHelicopterCount = 2;

struct EscapeBoatSnapshot {
    uint8_t active = 0;
    uint8_t padding[3] = {};
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float yaw = 0.0f;
    float bobTime = 0.0f;
};

struct ServerVehicleStateMessage {
    MessageHeader header{ MessageType::ServerVehicleState, {} };
    uint32_t tick = 0;
    EnemyHelicopterSnapshot helicopters[kEnemyHelicopterCount];
    EscapeBoatSnapshot escapeBoat;
};

// One enemy tank as the host drives it. Keyed by the level entity id the tank
// was placed from: both machines load the same level file, so that id names
// the same placement everywhere, where an index into the host's tank list
// would shift the moment one was dropped.
struct EnemyTankSnapshot {
    uint64_t entityId = 0;
    uint8_t dead = 0;
    // Who destroyed it, or kInvalidPlayerId for anything that was not a
    // player. Only the machine this names pays out for the wreck.
    PlayerId killer = kInvalidPlayerId;
    uint8_t padding[2] = {};
    // Chassis body centre and orientation, exactly as the physics solver
    // returns them on the host.
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float qx = 0.0f, qy = 0.0f, qz = 0.0f, qw = 1.0f;
    // Hull-relative traverse, radians.
    float turretYaw = 0.0f;
    float health = 0.0f;
};

// One AA emplacement. Matched on the receiver by where it stands rather than
// by index: the comm-tower gun and prefab guns are placed from different paths
// whose order is not something the two machines are promised to agree on.
struct AATurretSnapshot {
    uint8_t dead = 0;
    PlayerId killer = kInvalidPlayerId;
    uint8_t padding[2] = {};
    float x = 0.0f, z = 0.0f;
    // Radians, matching VehicleSystem::AATurret.
    float yaw = 0.0f, pitch = 0.0f;
    float heat = 0.0f;
    float health = 0.0f;
};

// One level Humvee. Keyed by its spawn index: both machines build the Humvees
// from the same level file in the same order. `hostDriven` says it has moved
// this level -- the host's AI drove it, or a player did -- so the client should
// show the host's pose rather than its own parked body; the turret yaw applies
// either way.
struct EnemyHumveeSnapshot {
    uint8_t index = 0;
    uint8_t hostDriven = 0;
    uint8_t padding[2] = {};
    // Chassis body centre and orientation, as the host's solver returns them.
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float qx = 0.0f, qy = 0.0f, qz = 0.0f, qw = 1.0f;
    float turretYaw = 0.0f;
};

// One objective aircraft. Its flight is a pure function of these timers and
// the crash state, so they are what is sent: the client re-derives the pose
// and keeps flying it between armor states, and the host alone decides when it
// escaped -- which is what fails the mission. Keyed by level entity id, like a
// tank.
struct ObjectivePlaneSnapshot {
    uint64_t entityId = 0;
    // kPlaneRolling | kPlaneEscaped | kPlaneDestroyed | kPlaneCrashing |
    // kPlaneCrashed.
    uint8_t flags = 0;
    uint8_t padding[3] = {};
    float holdTimer = 0.0f;
    float takeoffTimer = 0.0f;
    float crashX = 0.0f, crashY = 0.0f, crashZ = 0.0f;
    float crashVX = 0.0f, crashVY = 0.0f, crashVZ = 0.0f;
    float crashPitch = 0.0f, crashRoll = 0.0f, crashYaw = 0.0f;
};
inline constexpr uint8_t kPlaneRolling = 1u << 0;
inline constexpr uint8_t kPlaneEscaped = 1u << 1;
inline constexpr uint8_t kPlaneDestroyed = 1u << 2;
inline constexpr uint8_t kPlaneCrashing = 1u << 3;
inline constexpr uint8_t kPlaneCrashed = 1u << 4;
inline constexpr uint8_t kMaxReplicatedPlanes = 4;

inline constexpr uint8_t kMaxReplicatedTanks = 8;
inline constexpr uint8_t kMaxReplicatedHumvees = 8;
// Mirrors VehicleSystem::kMaxAATurrets; this header stays free of engine
// includes, so the number is repeated rather than shared.
inline constexpr uint8_t kMaxReplicatedAATurrets = 8;

// Every tank and AA gun in one broadcast. There is no "nearest" to cull by --
// a level places a handful at most -- and at 48 + 28 bytes each the full set
// is well inside one datagram.
struct ServerArmorStateMessage {
    MessageHeader header{ MessageType::ServerArmorState, {} };
    uint32_t tick = 0;
    uint8_t tankCount = 0;
    uint8_t turretCount = 0;
    uint8_t humveeCount = 0;
    uint8_t planeCount = 0;
    EnemyTankSnapshot tanks[kMaxReplicatedTanks];
    AATurretSnapshot turrets[kMaxReplicatedAATurrets];
    EnemyHumveeSnapshot humvees[kMaxReplicatedHumvees];
    ObjectivePlaneSnapshot planes[kMaxReplicatedPlanes];
};

enum class EnemyFireKind : uint8_t {
    TankShell = 0,
    AAShell,
    // A bandit's rifle, shotgun or sniper shot (see InfantryWeapon). Clients
    // run no AI, so without this no enemy soldier could ever hurt them.
    InfantryShot,
};

// Which gun an InfantryShot came from. Picks the flash, the pellet cone and the
// round's damage and speed on the client, which are the host's constants.
enum class InfantryWeapon : uint8_t {
    Rifle = 0,
    Shotgun,
    Sniper,
};

// A round leaving a tank's gun or an AA emplacement on the host. Every client
// spawns the same live hostile projectile from it, so the shell a player
// dodges is the one the host fired, and it hurts that player through the same
// local hostile-damage path a bandit's round does. Tank shells go reliable --
// one every few seconds, and a lost one is a lethal round nobody saw -- while
// AA bursts go unreliable, where a resent round would arrive already late.
struct ServerEnemyFireMessage {
    MessageHeader header{ MessageType::ServerEnemyFire, {} };
    EnemyFireKind kind = EnemyFireKind::TankShell;
    // InfantryShot only; claimed from the padding, so the size holds.
    InfantryWeapon weapon = InfantryWeapon::Rifle;
    uint8_t padding[2] = {};
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float dirX = 0.0f, dirY = 0.0f, dirZ = 0.0f;
    // Tank shells carry their tank's authored ballistics; AA rounds use the
    // fixed emplacement constants and leave these at zero.
    float speed = 0.0f;
    float lifetime = 0.0f;
    // Tank shells: the blast's scale on the player and the crater (1 = main
    // gun). AA rounds leave it at 1.
    float damageScale = 1.0f;
};

// The host pressed DEPLOY SQUAD: every client still on the planning screen
// deploys now, in the same insertion, from the same drop-off. The run is a
// pure function of these (the craft always flies in toward the island centre),
// so each machine flying its own copy from the same moment puts the whole
// squad in one aircraft. The client takes the other door.
struct ServerSquadDeployMessage {
    MessageHeader header{ MessageType::ServerSquadDeploy, {} };
    uint8_t insertionMode = 0;   // LevelInsertionMode
    uint8_t airframe = 0;        // InsertionAirframe
    uint8_t hostLeftSeat = 0;
    uint8_t padding = 0;
    float x = 0.0f, y = 0.0f, z = 0.0f;   // the host's drop-off
};

// One round leaving a player's muzzle. Carried purely so everyone else can see
// and hear it: the damage it does is settled by the hit-report path and nothing
// here is allowed to hurt anybody. Unreliable because a lost tracer is a missed
// frame of presentation, and a retransmitted one would draw a round that has
// already gone past.
struct ClientShotFiredMessage {
    MessageHeader header{ MessageType::ClientShotFired, {} };
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float dirX = 0.0f, dirY = 0.0f, dirZ = 0.0f;
};

struct ServerShotFiredMessage {
    MessageHeader header{ MessageType::ServerShotFired, {} };
    PlayerId shooter = kInvalidPlayerId;
    uint8_t padding[3] = {};
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float dirX = 0.0f, dirY = 0.0f, dirZ = 0.0f;
};

// A demolition charge has stuck to something. Reliable: a charge is placed once
// and then sits there, so a dropped packet would leave it invisible on one
// machine and standing on the wall on every other.
struct ChargeStickData {
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float nx = 0.0f, ny = 1.0f, nz = 0.0f;
    SGE::ChargeAnchorPart part = SGE::ChargeAnchorPart::World;
    uint8_t frozen = 0;
    uint8_t padding[2] = {};
    uint64_t turretEntityId = 0;
    uint32_t turretOrdinal = 0;
    float turretX = 0.0f, turretY = 0.0f, turretZ = 0.0f;
    float localX = 0.0f, localY = 0.0f, localZ = 0.0f;
    float localNx = 0.0f, localNy = 1.0f, localNz = 0.0f;
    float qx = 0.0f, qy = 0.0f, qz = 0.0f, qw = 1.0f;
};

struct ClientChargeStuckMessage {
    MessageHeader header{ MessageType::ClientChargeStuck, {} };
    ChargeStickData charge;
};

struct ServerChargeStuckMessage {
    MessageHeader header{ MessageType::ServerChargeStuck, {} };
    PlayerId owner = kInvalidPlayerId;
    uint8_t padding[3] = {};
    uint32_t chargeId = 0;
    ChargeStickData charge;
};

// The detonator. Carries no position -- it fires every charge its owner has
// planted, which is what the button does locally.
struct ClientChargeDetonateMessage {
    MessageHeader header{ MessageType::ClientChargeDetonate, {} };
};

struct ServerChargeDetonateMessage {
    MessageHeader header{ MessageType::ServerChargeDetonate, {} };
    PlayerId owner = kInvalidPlayerId;
    uint8_t padding[3] = {};
};

// Longest chat line that goes on the wire, not counting the terminator. Fixed
// rather than variable-length because every other message here is a flat struct
// the transport memcpy's whole -- a length-prefixed payload would be the only
// one of its kind, and 127 characters is a chat line, not a document.
inline constexpr uint8_t kMaxChatTextLength = 127;

// A line the local player typed. The host decides what happens to it: it is
// the only machine that can attribute it to a player id the others agree on,
// which is why a client never broadcasts its own.
struct ClientChatMessage {
    MessageHeader header{ MessageType::ClientChatMessage, {} };
    // Team chat rather than all chat. There are no opposing teams yet, so this
    // rides along as 0 and reserves the concept without a later version bump.
    uint8_t team = 0;
    uint8_t padding[2] = {};
    // Always NUL-terminated by the sender, and re-terminated by the host before
    // it is trusted: a client could ship 128 non-zero bytes and every read
    // after that would run off the end of the buffer.
    char text[kMaxChatTextLength + 1] = {};
};

// The host's copy of a line, stamped with who said it and sent to everyone
// including the original sender. The sender displays this rather than its own
// text so every machine shows the same ordering.
struct ServerChatMessage {
    MessageHeader header{ MessageType::ServerChatMessage, {} };
    PlayerId speaker = kInvalidPlayerId;
    uint8_t team = 0;
    uint8_t padding[1] = {};
    char text[kMaxChatTextLength + 1] = {};
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
    // 0 = destruction surface, 1 = prefab, 2 = tree, 3 = gunship,
    // 4 = enemy tank (entityId = level entity), 5 = AA turret (hit = its base)
    uint8_t kind = 0;
    // Player fire or not. Some prefabs -- the objective aircraft -- take damage
    // only from a player, so an enemy round replicated as player fire would let
    // the garrison shoot down the objective the players are sent to destroy.
    uint8_t playerOwned = 1;
    // A demolition charge rather than a round. CommTowerDamageAllowed refuses
    // everything else, so without this the receiver rebuilt the hit as ordinary
    // fire and dropped it -- the tower came down only for whoever set the C4.
    // Claimed from the padding, so the message keeps its size and its offsets.
    uint8_t remoteCharge = 0;
    uint8_t padding[1] = {};
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
    // Same flag on the way back out, for the same reason: a client applying the
    // committed break has to know it was a charge or its own gate refuses it.
    uint8_t remoteCharge = 0;
    uint8_t padding[4] = {};
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
    // Bumped by the host on a restart of the same level, which a kind/file
    // compare alone would read as "already there". Claimed from the padding.
    uint8_t restartSerial = 0;
    uint8_t padding[2] = {};
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

// Explosive barrels are host-owned. They used to run their whole life on every
// machine at once -- hits, fuse, chain reaction -- so each player blew up their
// own copy and only the host's could hurt an enemy. A client now asks (its
// shots, its fuse, its thrown barrel landing), the host decides, and the
// broadcast is what every machine plays. Barrels are identified by their index
// in the level's barrel list, which every machine builds from the same level
// plan in the same order.
enum class BarrelEvent : uint8_t {
    Ignite = 1,   // lit: burning, three-second fuse
    Detonate,     // gone: blast here, at x/y/z
};

// One message for both directions; the header says which. x/y/z is where the
// barrel is on the sender, because a thrown barrel is flown by whoever threw
// it and the other machines still have it standing where it started.
struct BarrelEventMessage {
    MessageHeader header{ MessageType::ClientBarrelEvent, {} };
    uint16_t barrel = 0;
    BarrelEvent event = BarrelEvent::Detonate;
    uint8_t padding = 0;
    float x = 0.0f, y = 0.0f, z = 0.0f;
};

// A client's rocket (or captured-tank shell) went off. Clients run no enemy
// damage -- the host owns every soldier -- so without this a client's rocket
// could hurt nothing but the ground. The host applies it to its actors as an
// explosion here, with the numbers the client's blast used.
struct ClientBlastMessage {
    MessageHeader header{ MessageType::ClientBlast, {} };
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float radius = 0.0f;
    float damage = 0.0f;
    float push = 0.0f;
    float ragdollImpulse = 0.0f;
};

// A player requesting a medic callout. Client sends to host, host broadcasts
// to all clients with the calling player's ID. Carries a cooldown to prevent spam.
struct ClientMedicCallMessage {
    MessageHeader header{ MessageType::ClientMedicCall, {} };
};

// Host broadcasts a medic callout from a player to all clients. The calling
// player's ID is in the header or carried here so remote players know who needs help.
struct ServerMedicCallMessage {
    MessageHeader header{ MessageType::ServerMedicCall, {} };
    PlayerId callerId = kInvalidPlayerId;
    uint8_t padding[3] = {};
};

// A client's transport delivered it along with the marines it paid for. The
// host lands them, because no AI runs on a client: a squad spawned there never
// moved and was retired as a stray the next frame. Reliable -- a lost one is a
// squad paid for and never seen.
struct ClientMarineDropMessage {
    MessageHeader header{ MessageType::ClientMarineDrop, {} };
    uint8_t count = 0;
    uint8_t padding[3] = {};
    float x = 0.0f, y = 0.0f, z = 0.0f;   // where the transport set down
};

// One player's scoreboard stats: kills on enemies, times downed, revives given.
struct ScoreboardEntry {
    PlayerId id = kInvalidPlayerId;
    uint8_t padding[3] = {};
    uint32_t kills = 0;
    uint32_t deaths = 0;  // times downed
    uint32_t revives = 0;
};

// Host broadcasts whenever any player's stats change. Clients store and draw it.
struct ServerScoreboardMessage {
    MessageHeader header{ MessageType::ServerScoreboard, {} };
    uint8_t playerCount = 0;
    uint8_t padding[3] = {};
    ScoreboardEntry entries[kMaxPlayers];
    static_assert(sizeof(ScoreboardEntry) == 16, "ScoreboardEntry must be 16 bytes");
};

} // namespace net

#endif // NET_PROTOCOL_H
