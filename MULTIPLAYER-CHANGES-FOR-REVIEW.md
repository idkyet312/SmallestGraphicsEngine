# Multiplayer changes for review

Working tree, uncommitted, on `terrain-deformation-sync` (base `b05a9fd`).
Everything below builds Release clean and passes `ctest --test-dir build -C Release`
(38/38). **None of it has been verified in a live two-machine session** — that is
the main thing this review should not assume.

The protocol went **10 → 13** across three of these changes. Both machines need
the same build; mismatched versions refuse the handshake rather than misread
each other.

---

## 1. Player-connected UI

No protocol change.

| File | What |
|---|---|
| `src/net/NetSession.h` | New `PlayerCount()` and `PlayerActive(PlayerId)`, counting `players_` slots directly. |
| `src/app/private/MenuTheme.h` | Session panel gained a live roster: "*N players connected*", one line per occupied slot, "*waiting for someone to join…*" when alone. |
| `src/app/private/Multiplayer.h` | `NetPlayerNotice` list, `UpdateNetPlayerNotices`, `DrawNetPlayerNotices`. On-screen `PLAYER-2 CONNECTED` / `LEFT`, 4.5 s, fading. |
| `src/app/main.cpp` | `UpdateNetPlayerNotices(deltaTime)` beside the session poll; `DrawNetPlayerNotices()` outside the insertion-choice gate. |

Why the roster reads the session rather than counting spawned bodies: a host
sitting in the lobby has loaded no level, so there are no bodies to count.

**Worth checking:** the roster is skipped while `LocalId() == kInvalidPlayerId`
(mid-handshake). Confirm that is the right call for a client that is connected at
the socket level but not yet seated.

---

## 2. Objectives destroyable by a client (protocol 11)

The bug was narrower than "objectives don't sync". Bullet hits on prefab entities
already replicated. `DamageBulletPrefabEntity` excluded `remoteCharge`, and the
comm tower is the one thing *only* a charge may damage — so the exclusion covered
exactly the case that mattered.

The exclusion turned out to be a missing field, not a policy:
`ApplyNetworkWorldImpact` hardcoded `fromRemoteCharge=false`, so a replicated
demolition arrived looking like rifle fire and `CommTowerDamageAllowed` discarded
it. The tower fell for whoever planted the C4 and stood for everyone else.

| File | What |
|---|---|
| `src/net/NetProtocol.h` | `remoteCharge` added to `ClientWorldImpactMessage` and `ServerWorldBreakMessage`, **claimed from existing padding** — struct sizes and offsets unchanged. Version to 11. |
| `src/net/NetSession.h` | Same flag on `WorldImpactRequest` / `WorldBreakEvent`; threaded through `ReportWorldImpact`, `PublishWorldBreak`, `HandleWorldImpact`, `HandleWorldBreak`. |
| `src/app/private/Multiplayer.h` | `DamageBulletPrefabEntity` no longer excludes charges. New `DamageObjectivePrefabEntity`. `ApplyNetworkWorldImpact` takes and forwards `remoteCharge`. |
| `src/app/main.cpp` | Three direct `DamagePrefabEntity` calls rerouted: rigged comm tower (~2699), objective aircraft (~2725), charge on protected tower geometry (~3339). |

**Worth checking:**
- Those three sites now pass `!projectile.hostile` as `fromPlayer`; the tower one
  previously defaulted to `true`. Gated by `c4Blast`, and enemies do not plant
  C4, so this should be equivalent — please confirm.
- Double-apply risk: the report path *replaces* the local call. Verify the host
  does not both queue and apply.

---

## 3. Enemy gunships and their health (protocol 12)

Each machine flew its own copy of both airframes from its own AI. A gunship a
client shot down crashed on that client alone and kept flying, firing and
dropping troops everywhere else.

| File | What |
|---|---|
| `src/net/NetProtocol.h` | `MessageType::ServerVehicleState`; `EnemyHelicopterSnapshot`; `ServerVehicleStateMessage` carrying both airframes. `kEnemyHelicopterCount = 2`. Version to 12. |
| `src/net/NetSession.h` | `EnemyHelicopterState`; `PublishVehicles` / `RemoteVehicles`; `SendVehicleState` (broadcast, unreliable, per tick) and `HandleVehicleState` with finite-value rejection. `ValidWorldKind` extended to 3; `ValidWorldTarget` bounds a gunship's `entityId` to the airframe index. |
| `src/app/private/Multiplayer.h` | `PublishHostVehicles`, `ApplyNetworkEnemyHelicopters`, `DamageNetworkedHelicopter`; `kind == 3` case in `ApplyNetworkWorldImpact`. |
| `src/app/main.cpp` | `ApplyNetworkEnemyHelicopters()` after the local flight step; the two hull-damage sites now report instead of applying. |

Design note: the client still runs its local vehicle update and is then
**overwritten**, rather than having the update suppressed. Rotors, damage smoke
and audio are all driven off these same fields, so letting them run and then
correcting position/health/dead keeps the craft animated.

**Worth checking:**
- `RemoteVehicles()` returns `nullptr` before the first packet so a client does
  not snap the craft to the origin. Confirm the first-frame path.
- `kind == 3` damage is applied host-only inside `ApplyNetworkWorldImpact`;
  clients learn the result through the vehicle state. Verify no double-subtract
  on the shooting client.
- Unreliable transport for latched state (`dead`, `crashed`). A dropped packet
  self-heals next tick, but confirm nothing latches off a single frame.

---

## 4. Enemy AI grenades

No protocol change — `hostile` was already on the wire and simply never used for
AI throws.

The AI runs on every machine, so every machine threw its own grenade. They were
never the same grenade: the throw is rolled against a local random, so one
machine's bandit threw and another's did not, and the two that did threw at their
own idea of where the player was.

| File | What |
|---|---|
| `src/net/NetSession.h` | `SpawnAIGrenade(...)` — allocates an id, queues for the host, broadcasts. Owner is `kInvalidPlayerId`; the owner guards in `QueueAuthoritativeGrenadeSpawn` and `HandleGrenadeSpawn` were relaxed to admit it. |
| `src/app/private/Combat.h` | `BanditThrowGrenade`: in a session the host throws for everybody and clients return early; offline is unchanged. |
| `src/app/private/Multiplayer.h` | Spawn-apply sets `grenadeCollisionGrace = 0.18f` for AI grenades, matching the local throw — without it a bandit frag detonates on the arm that threw it. |

`Combat.h` asks `g_netSession.Active()` rather than `MultiplayerActive()`: it is
included at main.cpp:123, ahead of `Multiplayer.h` at :148.

**Worth checking:** the relaxed `owner >= kMaxPlayers` guards. They now admit
exactly `kInvalidPlayerId`; make sure nothing downstream indexes `players_` by a
grenade owner without checking.

---

## 5. Visible remote gunfire (protocol 13)

Nothing about firing was on the wire at all — `PlayerInput` has no fire bit — so
another player's shooting was silent and invisible.

| File | What |
|---|---|
| `src/scene/Scene.h` | `localShotCounter` / `localShotOrigin` / `localShotDirection` plus `RecordLocalShot`, called from both fire paths (`ShootProjectile`, `ShootSniperProjectile`). Scene stays network-unaware; the net layer polls. |
| `src/net/NetProtocol.h` | `ClientShotFired` / `ServerShotFired` message types and structs. Version to 13. |
| `src/net/NetSession.h` | `RemoteShot`; `ReportShotFired`; `DrainRemoteShots`; both handlers. The host presents the shot and relays to everyone *except* the shooter. |
| `src/app/private/Multiplayer.h` | `ReportLocalShots` (counter diff) and `PresentRemoteShots` (world muzzle flash, weapon smoke, positional gun audio). |
| `src/app/main.cpp` | Both called beside the session poll. |

Presentation only — nothing here damages anything; what a round hits is still
settled by the hit-report path. World-space `SpawnExplosionFX` rather than
`TriggerMuzzleFlash`, which drives the local viewmodel and has no world position
(same reason the AA turret presents its own fire that way).

### 5a. Tracer and muzzle-FX correction (follow-up, same protocol 13)

The first cut of the above presented a shot with
`SpawnExplosionFX(muzzle, 0.55f, 0.06f)`, which was wrong in a way that only
showed up on screen: that call is not a scalable flash. Reading
`Scene.h:1815-1871`, its `size` also drives `camera.ApplyExplosionImpulse`
(**every remote shot shook the watching player's camera**), 34 sparks and a
20-particle dust ring — 54 particles per round, counts fixed regardless of size.
It rendered as an orange fireball at the shooter's muzzle.

| File | What |
|---|---|
| `src/scene/Scene.h` | `RemoteTracerFX` beside `LaserBeamFX` (origin, direction, speed, distance, previousDistance, life). `scene.remoteTracers` list, kept out of `projectiles` so the projectile update cannot act on it. `SpawnRemoteTracer`. Advance/retire beside `laserBeam.life` in `Update`; cleared in `ResetLevelRuntimeState`. |
| `src/render/dx12/ForwardRenderer.h` | Tracer draw loop immediately after the projectile tracer loop, reusing its exact two-box additive technique and its `min(5, max(1.2, moved))` length clamp. Friendly orange, never the hostile red — another player is an ally. |
| `src/app/private/Multiplayer.h` | `PresentRemoteShots`: `SpawnExplosionFX` deleted, `SpawnWeaponSmoke` 0.9 → 0.55 (one puff), `SpawnRemoteTracer` added, audio unchanged. |

This closes the "no bullet streak" gap without the inert-`Projectile` route: the
tracer carries no `Projectile`, so no audit of damage, decals, hit markers or
`ReportWorldImpact` was needed. No wire change — `ClientShotFired` /
`ServerShotFired` already carried origin and direction.

**Worth checking:** the shooter must not see a tracer for its own round (it
already draws one from its real projectile). Host-side `ReportShotFired`
broadcasts without queueing locally, and `HandleServerShotFired` drops
`shooter == localId_`. Confirm both.

---

## Not implemented

**Shared exfil and mission end.** Investigated, not written. For whoever picks
it up, the findings:

- The escape boat is placed with a local `RandomUnit()` in two places —
  `OnObjectivePlaneResolved` (`EnemyVehicles.h` ~763, ~771) and the planeless-map
  path in `main.cpp` ~3584. Each machine rolls its own bearing, so the exfil is
  in a different place for each player.
- Reaching it calls `OpenWinScreen()` (`LevelSession.h:319`) locally at
  `main.cpp` ~3592. Nothing is replicated.
- Suggested shape: fold the boat (`escapeBoatActive`, `escapeBoatPosition`,
  `escapeBoatYaw`) into the existing `ServerVehicleStateMessage` rather than
  adding a message; make mission end host-declared, with a client boarding
  reporting rather than opening the screen; and let return-to-base go through the
  existing host-authoritative level change, which clients already follow.

---

## Unrelated change in the same tree

`tools/AssetCooker.cpp` — two assets added to `IsCookExcluded`, with the
measurements in the comments:

- **M4A1**: cooked, it loads as 1 primitive / 0 iron sights split (uncooked:
  16 / 2). The split matches node names ("aiming module") and the cook drops node
  identity, so the rear leaf bakes in and pokes through a mounted red dot.
  Costs 192 MB of VRAM to exclude.
- **R700**: its 11 materials collapse to one called `Metal`, so the scope
  aperture — found by the `glass` material name — never resolves and the
  picture-in-picture scope loses its lens. Costs nothing: the model carries no
  embedded textures, so cooking it saved 0 MB.

AK-74, M9 and the Remington 870 are cooked and stay cooked: 285.3 MB of VRAM
saved (629.3 MB of RGBA8+mips down to 152.0 MB of BC), with the shotgun smoke
test passing on all three runs.

Note the cooked output lives in `Content/Cooked` but the game runs from
`build/Content` — a cook is invisible until
`cmake --build build --target RuntimeContent --config Release` copies it over.
