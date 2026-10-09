#pragma once

// Private application implementation; included once by main.cpp in dependency order.

// Player-flown helicopters: the parked gunship a level places with a
// parked_helicopter entity, and the insertion BlackHawk once the player takes
// its stick. Both fly on HelicopterFlight; this file owns boarding, the chase
// camera, the gunship's weapons and damage, and the HUD prompt.

static constexpr float kGunshipBoardRange = 9.0f;
static constexpr float kBlackHawkBoardRange = 11.0f;
// Below this height over the ground, getting out is stepping off the skids;
// above it the HUD calls it a bail-out (the player falls).
static constexpr float kPilotStepOutHeight = 2.5f;
static constexpr float kGunshipChaseDistance = 21.0f;
static constexpr float kBlackHawkChaseDistance = 24.0f;
static constexpr float kGunshipGunInterval = 0.075f;
static constexpr float kGunshipRocketInterval = 0.55f;

// What a helicopter can settle on: the terrain, or the sea where the terrain
// is below it.
static float HelicopterFloorAt(float x, float z) {
    return (std::max)(GroundHeightAt(x, z), g_ocean.GetSurfaceY());
}

static HelicopterFlightTuning ParkedGunshipTuning() {
    HelicopterFlightTuning tuning;
    tuning.maxSpeed = 55.0f;
    tuning.maxStrafeSpeed = 18.0f;
    tuning.maxClimbRate = 10.0f;
    tuning.yawRate = 1.5f;
    tuning.restHeight = g_game.vehicles.helicopterModelBottomOffset;
    return tuning;
}

static bool PilotingHelicopter() {
    return g_pilotedHelicopter != PilotedHelicopter::None;
}

static XMFLOAT3 PilotedHelicopterPosition() {
    return g_pilotedHelicopter == PilotedHelicopter::Gunship
        ? g_parkedGunshipFlight.position
        : g_game.vehicles.blackHawkPosition;
}

static float PilotedHelicopterYaw() {
    return g_pilotedHelicopter == PilotedHelicopter::Gunship
        ? g_parkedGunshipFlight.yaw : g_game.vehicles.blackHawkYaw;
}

// Height of the skids over whatever is under them.
static float PilotedHelicopterHeight() {
    if (g_pilotedHelicopter == PilotedHelicopter::Gunship)
        return g_parkedGunshipFlight.HeightAboveGround(
            HelicopterFloorAt(g_parkedGunshipFlight.position.x,
                              g_parkedGunshipFlight.position.z),
            ParkedGunshipTuning());
    const VehicleSystem& vehicles = g_game.vehicles;
    return vehicles.blackHawkPosition.y - HelicopterFloorAt(
        vehicles.blackHawkPosition.x, vehicles.blackHawkPosition.z);
}

static void BeginPiloting(PilotedHelicopter kind) {
    g_pilotedHelicopter = kind;
    g_pilotSavedGunVisible = scene.gun.visible;
    g_pilotSavedFPSMode = scene.camera.FPSMode;
    scene.gun.visible = false;
    scene.camera.FPSMode = false;
    scene.camera.VerticalVelocity = 0.0f;
    scene.camera.IsCrouching = false;
    scene.camera.IsSliding = false;
    scene.camera.IsSwimming = false;
    // Start looking along the nose, a little down onto the airframe. Camera
    // yaw is atan2(z, x), the heading is atan2(x, z): 90 - heading.
    scene.camera.SetViewAngles(
        90.0f - XMConvertToDegrees(PilotedHelicopterYaw()), -14.0f);
}

// Gives the player their body back without moving it: the caller (or, for a
// BlackHawk going down, the cabin bail-out) decides where they stand.
static void RestorePilotBody() {
    scene.gun.visible = g_pilotSavedGunVisible;
    scene.camera.FPSMode = g_pilotSavedFPSMode;
    scene.camera.VerticalVelocity = 0.0f;
    scene.camera.IsGrounded = false;
    g_pilotedHelicopter = PilotedHelicopter::None;
}

// Out the right-hand door. On the ground that is a step onto the grass beside
// the skids; in the air it is a fall from the same spot.
static void PlacePlayerBesideAirframe(const XMFLOAT3& origin, float yaw,
                                      float clearance) {
    const float rightX = std::cos(yaw), rightZ = -std::sin(yaw);
    const float x = origin.x + rightX * clearance;
    const float z = origin.z + rightZ * clearance;
    const float ground = GroundHeightAt(x, z);
    const float footY = (std::max)(ground, origin.y -
        (g_pilotedHelicopter == PilotedHelicopter::Gunship
            ? g_game.vehicles.helicopterModelBottomOffset : 0.0f));
    scene.camera.Position = { x, footY + scene.camera.PlayerHeight + 0.05f, z };
    scene.camera.FloorY = ground;
}

static void ExitPilotedHelicopter() {
    if (!PilotingHelicopter()) return;
    VehicleSystem& vehicles = g_game.vehicles;
    if (g_pilotedHelicopter == PilotedHelicopter::Gunship) {
        PlacePlayerBesideAirframe(g_parkedGunshipFlight.position,
                                  g_parkedGunshipFlight.yaw, 4.2f);
    } else {
        PlacePlayerBesideAirframe(vehicles.blackHawkPosition,
                                  vehicles.blackHawkYaw, 3.6f);
        vehicles.ReleaseBlackHawkControls();
        // The marines rode in the back the whole way; they get out where the
        // player put down -- unless the player bailed out over open air.
        if (vehicles.blackHawkSquadAboard && vehicles.blackHawkLanded) {
            vehicles.blackHawkSquadAboard = false;
            g_marineDropPending = true;
            g_marineDropOrigin = { vehicles.blackHawkPosition.x,
                                   GroundHeightAt(vehicles.blackHawkPosition.x,
                                                  vehicles.blackHawkPosition.z),
                                   vehicles.blackHawkPosition.z };
        }
    }
    RestorePilotBody();
}

static bool PilotCanBoard() {
    return !PlayerInVehicle() && !scene.ejected && !g_insertionChoicePending &&
           scene.player.health > 0.0f && !scene.player.downed &&
           !g_game.vehicles.blackHawkCarryingPlayer &&
           !g_game.vehicles.insertionBoatCarryingPlayer;
}

static float DistanceToCamera(const XMFLOAT3& point) {
    const float dx = point.x - scene.camera.Position.x;
    const float dy = point.y - scene.camera.Position.y;
    const float dz = point.z - scene.camera.Position.z;
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

static bool GunshipBoardable() {
    return ParkedGunshipVisible() && !g_parkedGunshipDead &&
           !g_parkedGunshipNeedsPark &&
           DistanceToCamera(g_parkedGunshipFlight.position) <= kGunshipBoardRange;
}

static bool BlackHawkBoardable() {
    const VehicleSystem& vehicles = g_game.vehicles;
    if (!g_blackHawkModel || !vehicles.CanTakeBlackHawkControls()) return false;
    // Mid-cabin rather than the skids, which hang well below the door.
    XMFLOAT3 cabin = vehicles.blackHawkPosition;
    cabin.y += 2.2f;
    return DistanceToCamera(cabin) <= kBlackHawkBoardRange;
}

// E on foot. Nearest airframe in reach wins.
static bool TryBoardHelicopter() {
    if (!PilotCanBoard()) return false;
    const bool gunship = GunshipBoardable();
    const bool blackHawk = BlackHawkBoardable();
    if (!gunship && !blackHawk) return false;
    const bool takeGunship = gunship && (!blackHawk ||
        DistanceToCamera(g_parkedGunshipFlight.position) <=
            DistanceToCamera(g_game.vehicles.blackHawkPosition));
    if (takeGunship) {
        BeginPiloting(PilotedHelicopter::Gunship);
        return true;
    }
    if (!g_game.vehicles.TakeBlackHawkControls()) return false;
    BeginPiloting(PilotedHelicopter::BlackHawk);
    return true;
}

// F while riding in the BlackHawk's cabin: move up front and take the stick.
static bool TakeBlackHawkStickFromCabin() {
    VehicleSystem& vehicles = g_game.vehicles;
    if (!vehicles.blackHawkCarryingPlayer || PlayerInVehicle() ||
        scene.player.health <= 0.0f || scene.player.downed) return false;
    if (!vehicles.TakeBlackHawkControls()) return false;
    BeginPiloting(PilotedHelicopter::BlackHawk);
    return true;
}

static HelicopterFlightInput ReadPilotInput() {
    HelicopterFlightInput input;
    if (!HasInputFocus()) return input;
    const auto down = [](int key) { return (FocusedKeyState(key) & 0x8000) != 0; };
    input.forward = (down('W') ? 1.0f : 0.0f) - (down('S') ? 1.0f : 0.0f);
    input.yaw = (down('D') ? 1.0f : 0.0f) - (down('A') ? 1.0f : 0.0f);
    input.lift = (down(VK_SPACE) ? 1.0f : 0.0f) -
                 (down(VK_CONTROL) ? 1.0f : 0.0f);
    // Q/R slide sideways, for lining the gunship's nose up on a target
    // without turning off it.
    input.strafe = (down('R') ? 1.0f : 0.0f) - (down('Q') ? 1.0f : 0.0f);
    return input;
}

// Muzzle under the nose, and the two rocket pods on the stub wings, in the
// same frame as kHelicopterMuzzleForward/Drop.
static XMFLOAT3 GunshipLocalToWorld(float right, float up, float forward) {
    const HelicopterFlight& flight = g_parkedGunshipFlight;
    const XMMATRIX orientation = XMMatrixRotationRollPitchYaw(
        flight.pitch, flight.yaw + XM_PI, flight.roll);
    // The airframe's nose is its local -Z under this rotation (see
    // ParkedGunshipWorldMatrix), and its right is local -X.
    XMFLOAT3 world;
    XMStoreFloat3(&world, XMVector3TransformNormal(
        XMVectorSet(-right, up, -forward, 0.0f), orientation) +
        XMLoadFloat3(&flight.position));
    return world;
}

// Where the crosshair is: the camera ray to the first surface, or far out
// along it. Long enough to reach anything the gunship can see.
static XMFLOAT3 PilotAimPoint() {
    const XMFLOAT3 origin = scene.camera.Position;
    const XMFLOAT3 end = {
        origin.x + scene.camera.Front.x * 400.0f,
        origin.y + scene.camera.Front.y * 400.0f,
        origin.z + scene.camera.Front.z * 400.0f };
    XMFLOAT3 closest = end;
    float closestSq = FLT_MAX;
    const auto accept = [&](const XMFLOAT3& hit) {
        const float dx = hit.x - origin.x, dy = hit.y - origin.y,
                    dz = hit.z - origin.z;
        const float distanceSq = dx * dx + dy * dy + dz * dz;
        if (distanceSq < closestSq) { closestSq = distanceSq; closest = hit; }
    };
    XMFLOAT3 hit;
    if (scene.useDestruction && g_destruction.IsInitialized() &&
        g_destruction.HitTestSegment(origin, end, 0.03f, hit)) accept(hit);
    if (HitTerrainSegment(origin, end, 0.03f, hit)) accept(hit);
    return closest;
}

static bool AimFrom(const XMFLOAT3& muzzle, const XMFLOAT3& target,
                    XMFLOAT3& direction) {
    const XMVECTOR delta = XMLoadFloat3(&target) - XMLoadFloat3(&muzzle);
    if (XMVectorGetX(XMVector3LengthSq(delta)) < 1.0f) return false;
    XMStoreFloat3(&direction, XMVector3Normalize(delta));
    return true;
}

static void FirePilotedGunshipGun() {
    if (g_pilotedHelicopter != PilotedHelicopter::Gunship ||
        g_parkedGunshipDead || g_parkedGunshipFireCooldown > 0.0f) return;
    const XMFLOAT3 muzzle = GunshipLocalToWorld(
        0.0f, -kHelicopterMuzzleDrop, kHelicopterMuzzleForward);
    XMFLOAT3 direction;
    if (!AimFrom(muzzle, PilotAimPoint(), direction)) return;
    const float spread = 0.008f;
    direction.x += ((float)std::rand() / RAND_MAX - 0.5f) * spread;
    direction.y += ((float)std::rand() / RAND_MAX - 0.5f) * spread;
    direction.z += ((float)std::rand() / RAND_MAX - 0.5f) * spread;
    XMStoreFloat3(&direction, XMVector3Normalize(XMLoadFloat3(&direction)));
    scene.SpawnPlayerProjectile(muzzle, direction, 1.5f);
    scene.projectiles.back().playerOwned = true;
    // A cannon round, not a rifle bullet: chips tank armour (and, like the
    // enemy door guns, never scores an instant headshot).
    scene.projectiles.back().aircraftGun = true;
    scene.projectiles.back().damageMultiplier = 1.4f;
    scene.SpawnWeaponSmoke(muzzle, direction, 0.8f);
    g_gunAudio.PlayAt(muzzle.x, muzzle.y, muzzle.z, 0.62f,
                      0.80f + ((float)std::rand() / RAND_MAX) * 0.08f, 120.0f);
    g_parkedGunshipFireCooldown = kGunshipGunInterval;
}

static void FirePilotedGunshipRocket() {
    if (g_pilotedHelicopter != PilotedHelicopter::Gunship ||
        g_parkedGunshipDead || g_parkedGunshipRocketCooldown > 0.0f) return;
    // Alternate pods so a salvo walks left-right like the real thing. Pod
    // offsets from Hind.glb's unguided_missiles nodes (x +-0.44, 0.42 ahead of
    // the mesh centre, on a 4.71-unit airframe), scaled to kHelicopterLength.
    static bool leftPod = false;
    leftPod = !leftPod;
    constexpr float kModelUnit = kHelicopterLength / 4.71f;
    const float side = (leftPod ? -0.44f : 0.44f) * kModelUnit;
    const XMFLOAT3 muzzle = GunshipLocalToWorld(side, -0.26f * kModelUnit,
                                                0.42f * kModelUnit);
    XMFLOAT3 direction;
    if (!AimFrom(muzzle, PilotAimPoint(), direction)) return;
    Projectile rocket = {};
    rocket.position = rocket.previousPosition = muzzle;
    rocket.direction = direction;
    rocket.speed = 70.0f;
    rocket.lifetime = 6.0f;
    rocket.active = true;
    rocket.rocket = true;
    rocket.playerOwned = true;
    scene.projectiles.push_back(rocket);
    scene.SpawnWeaponSmoke(muzzle, direction, 2.0f);
    g_gunAudio.PlayAt(muzzle.x, muzzle.y, muzzle.z, 0.8f, 0.55f, 160.0f);
    g_parkedGunshipRocketCooldown = kGunshipRocketInterval;
}

// SGE_PILOT_TEST supplies the stick instead of the keyboard (see UpdatePilotTest).
static bool g_pilotTestInputActive = false;
static HelicopterFlightInput g_pilotTestInput;

// Called from ProcessInput while piloting: flight controls, and the gunship's
// guns on the mouse buttons.
static void ProcessPilotInput(bool inputBlocked) {
    const HelicopterFlightInput input = g_pilotTestInputActive ? g_pilotTestInput
        : inputBlocked ? HelicopterFlightInput{} : ReadPilotInput();
    if (g_pilotedHelicopter == PilotedHelicopter::Gunship) {
        g_pilotInput = input;
        if (inputBlocked || ImGui::GetIO().WantCaptureMouse) return;
        if (FocusedKeyState(VK_LBUTTON) & 0x8000) FirePilotedGunshipGun();
        if (FocusedKeyState(VK_RBUTTON) & 0x8000) FirePilotedGunshipRocket();
    } else if (g_pilotedHelicopter == PilotedHelicopter::BlackHawk) {
        g_game.vehicles.blackHawkPilotInput = input;
    }
}

// Hostile rounds on the gunship. Only once someone is flying it: a parked
// airframe nobody is in is not what the enemy is shooting at, and a stray
// burst should not wreck the player's ride before they reach it.
static bool HitPilotedGunshipSegment(const XMFLOAT3& start, const XMFLOAT3& end,
                                     float radius, XMFLOAT3& hit) {
    if (g_pilotedHelicopter != PilotedHelicopter::Gunship ||
        !ParkedGunshipVisible() || g_parkedGunshipDead) return false;
    return HitSphereAtSegment(g_parkedGunshipFlight.position,
                              kHelicopterHitRadius, start, end, radius, hit);
}

static void DamagePilotedGunship(float damage) {
    if (damage <= 0.0f || g_parkedGunshipDead) return;
    g_parkedGunshipHealth = (std::max)(0.0f, g_parkedGunshipHealth - damage);
    scene.camera.AddHitTrauma(0.05f);
    if (g_parkedGunshipHealth > 0.0f) return;
    g_parkedGunshipDead = true;
    const XMFLOAT3 wreck = g_parkedGunshipFlight.position;
    scene.SpawnExplosionFX(wreck, 7.0f, 1.0f);
    scene.SpawnSmokeBurst(wreck, 2.0f, 2.5f);
    g_pendingExplosionAudio.push_back({ 0.0f, 0.95f, 0.80f, false });
    if (g_pilotedHelicopter == PilotedHelicopter::Gunship) {
        // Thrown out of the burning airframe, hurt but alive to fall.
        PlacePlayerBesideAirframe(wreck, g_parkedGunshipFlight.yaw, 4.2f);
        RestorePilotBody();
        scene.DamagePlayerFrom(35.0f, wreck);
    }
}

// Damage smoke for the vehicles with no curve of their own: the patrol boat and
// the player's gunship trail smoke under kVehicleDamageSmokeFraction,
// thickening toward zero. (Tanks smoke from UpdateEnemyTanks; the enemy
// gunships, BlackHawk and insertion boat already have theirs.)
static void UpdateVehicleDamageSmoke(float dt) {
    const auto trail = [dt](float fraction, float& timer,
                            const XMFLOAT3& position) {
        if (fraction >= kVehicleDamageSmokeFraction) { timer = 0.0f; return; }
        timer -= dt;
        if (timer > 0.0f) return;
        const float severity = 1.0f - (std::max)(0.0f, fraction) /
                                          kVehicleDamageSmokeFraction;
        timer = 0.34f - 0.24f * severity;
        scene.SpawnSmokeBurst(position, 0.5f + 0.7f * severity,
                              0.4f + 0.8f * severity);
    };
    static float boatTimer = 0.0f;
    if (g_levelPatrolBoatEnabled && g_boatModel && !g_boatDead && !g_boatSunk)
        trail(g_boatHealth / kBoatMaxHealth, boatTimer,
              { g_boatPosition.x, g_boatPosition.y + 1.5f, g_boatPosition.z });
    static float gunshipTimer = 0.0f;
    if (ParkedGunshipVisible() && !g_parkedGunshipDead) {
        const XMFLOAT3& p = g_parkedGunshipFlight.position;
        trail(g_parkedGunshipHealth / kParkedGunshipMaxHealth, gunshipTimer,
              { p.x, p.y + 1.1f * kHelicopterSizeScale, p.z });
    }
}

static void PilotChaseCamera(const XMFLOAT3& origin, float yaw, float& lastYaw,
                             float distance, float lift) {
    // Turn the view with the airframe, so holding a heading keeps the camera
    // behind it while the mouse stays free to look around.
    const float yawDelta = std::atan2(std::sin(yaw - lastYaw),
                                      std::cos(yaw - lastYaw));
    lastYaw = yaw;
    scene.camera.SetViewAngles(scene.camera.Yaw - XMConvertToDegrees(yawDelta),
                               (std::clamp)(scene.camera.Pitch, -70.0f, 30.0f));
    const XMVECTOR target = XMLoadFloat3(&origin) + XMVectorSet(0, lift, 0, 0);
    XMStoreFloat3(&scene.camera.Position,
        target - XMLoadFloat3(&scene.camera.Front) * distance);
    // Never under the ground or the sea, looking up through it.
    scene.camera.Position.y = (std::max)(scene.camera.Position.y,
        HelicopterFloorAt(scene.camera.Position.x, scene.camera.Position.z) + 1.0f);
    scene.camera.Up = { 0.0f, 1.0f, 0.0f };
    scene.camera.FloorY = scene.camera.Position.y - scene.camera.PlayerHeight;
    scene.camera.VerticalVelocity = 0.0f;
}

static void SpinParkedGunshipRotors(float dt, bool powered) {
    g_parkedGunshipRotorSpeedScale = VehicleSystem::StepHelicopterRotorSpeed(
        g_parkedGunshipRotorSpeedScale, powered, dt);
    g_parkedGunshipMainRotorAngle = std::fmod(g_parkedGunshipMainRotorAngle +
        dt * 24.0f * g_parkedGunshipRotorSpeedScale, XM_2PI);
    g_parkedGunshipTailRotorAngle = std::fmod(g_parkedGunshipTailRotorAngle +
        dt * 38.0f * g_parkedGunshipRotorSpeedScale, XM_2PI);
    if (g_parkedGunshipMainRotorNode)
        XMStoreFloat4(&g_parkedGunshipMainRotorNode->rotation,
            XMQuaternionRotationAxis(XMVectorSet(0, 1, 0, 0),
                                     g_parkedGunshipMainRotorAngle));
    if (g_parkedGunshipTailRotorNode)
        XMStoreFloat4(&g_parkedGunshipTailRotorNode->rotation,
            XMQuaternionRotationAxis(XMVectorSet(1, 0, 0, 0),
                                     g_parkedGunshipTailRotorAngle));
    XMFLOAT4X4 identity;
    XMStoreFloat4x4(&identity, XMMatrixIdentity());
    g_parkedGunshipModel->UpdateGlobalTransform(identity);
}

static void UpdateParkedGunship(float dt) {
    if (!ParkedGunshipVisible()) return;
    const HelicopterFlightTuning tuning = ParkedGunshipTuning();
    if (g_parkedGunshipNeedsPark) {
        if (g_game.loading.Active()) return;
        g_parkedGunshipNeedsPark = false;
        // Seated on the terrain, unless it was authored well above it -- a
        // rooftop or a deck is a pad, and stays where it was put.
        const float ground = HelicopterFloorAt(g_parkedGunshipSpawn.x,
                                               g_parkedGunshipSpawn.z);
        const float pad = g_parkedGunshipSpawn.y > ground + 1.5f
            ? g_parkedGunshipSpawn.y : ground;
        g_parkedGunshipFlight.Park(g_parkedGunshipSpawn, g_parkedGunshipSpawnYaw,
                                   pad, tuning);
    }
    g_parkedGunshipFireCooldown = (std::max)(0.0f, g_parkedGunshipFireCooldown - dt);
    g_parkedGunshipRocketCooldown =
        (std::max)(0.0f, g_parkedGunshipRocketCooldown - dt);
    const bool piloted = g_pilotedHelicopter == PilotedHelicopter::Gunship;
    // An empty airframe on its pad stays exactly where it is (it may be on a
    // rooftop the terrain floor knows nothing about); one left in the air
    // settles to the ground unpowered.
    if (piloted || !g_parkedGunshipFlight.grounded) {
        const float floor = HelicopterFloorAt(g_parkedGunshipFlight.position.x,
                                              g_parkedGunshipFlight.position.z);
        g_parkedGunshipFlight.Step(piloted ? g_pilotInput : HelicopterFlightInput{},
                                   tuning, dt, floor,
                                   piloted && !g_parkedGunshipDead);
    }
    SpinParkedGunshipRotors(dt, piloted && !g_parkedGunshipDead);
}

// SGE_PILOT_TEST=gunship|blackhawk: unattended flight check. Boards the
// aircraft (parking a gunship ahead of the player if the level has none; for
// the BlackHawk, taking the stick from the cabin on the way in), flies a fixed
// stick script -- climb, cruise, turn, hover, descend -- then gets out and
// quits. The pose goes to logs/pilot_trace.log four times a second, and
// SGE_PILOT_CAPTURE=<file.ppm> grabs one frame in cruise.
static void UpdatePilotTest(float dt) {
    static int mode = [] {
        char value[16] = {};
        GetEnvironmentVariableA("SGE_PILOT_TEST", value, sizeof(value));
        if (std::strcmp(value, "gunship") == 0) return 1;
        if (std::strcmp(value, "blackhawk") == 0) return 2;
        return 0;
    }();
    if (mode == 0 || !IsGameplayScreen() || g_game.loading.Active() ||
        g_insertionChoicePending) return;
    static FILE* trace = [] {
        FILE* file = nullptr;
        fopen_s(&file, "logs/pilot_trace.log", "w");
        return file;
    }();
    static bool boarded = false, exited = false;
    static float flightTime = 0.0f, traceTimer = 0.0f, waitTime = 0.0f;
    const auto log = [&](const char* event) {
        if (!trace) return;
        const XMFLOAT3 p = PilotingHelicopter() ? PilotedHelicopterPosition()
                                                : scene.camera.Position;
        const HelicopterFlight& flight = mode == 1 ? g_parkedGunshipFlight
                                                   : g_game.vehicles.blackHawkFlight;
        std::fprintf(trace,
            "%s t=%.2f piloting=%d pos=%.2f,%.2f,%.2f vel=%.2f,%.2f,%.2f "
            "yaw=%.3f pitch=%.3f roll=%.3f grounded=%d alt=%.2f "
            "cam=%.2f,%.2f,%.2f fps=%d hp=%.0f bhPhase=%d\n",
            event, flightTime, static_cast<int>(g_pilotedHelicopter),
            p.x, p.y, p.z, flight.velocity.x, flight.velocity.y,
            flight.velocity.z, flight.yaw, flight.pitch, flight.roll,
            flight.grounded ? 1 : 0,
            PilotingHelicopter() ? PilotedHelicopterHeight() : 0.0f,
            scene.camera.Position.x, scene.camera.Position.y,
            scene.camera.Position.z, scene.camera.FPSMode ? 1 : 0,
            scene.player.health,
            static_cast<int>(g_game.vehicles.blackHawkPhase));
        std::fflush(trace);
    };
    scene.player.godMode = true;
    if (!boarded) {
        waitTime += dt;
        if (mode == 1) {
            // Riding the insertion in: boarding waits for the drop.
            if (g_game.vehicles.blackHawkCarryingPlayer) return;
            if (!g_parkedGunshipPresent) {
                const float heading = std::atan2(scene.camera.Front.x,
                                                 scene.camera.Front.z);
                g_parkedGunshipPresent = true;
                g_parkedGunshipSpawn = {
                    scene.camera.Position.x + std::sin(heading) * 16.0f, -1000.0f,
                    scene.camera.Position.z + std::cos(heading) * 16.0f };
                g_parkedGunshipSpawnYaw = heading;
                g_parkedGunshipNeedsPark = true;
                g_parkedGunshipDead = false;
                g_parkedGunshipHealth = kParkedGunshipMaxHealth;
                return;
            }
            if (g_parkedGunshipNeedsPark || waitTime < 2.0f) return;
            const HelicopterFlight& flight = g_parkedGunshipFlight;
            const float ground = GroundHeightAt(flight.position.x + 5.0f,
                                                flight.position.z);
            scene.camera.Position = { flight.position.x + 5.0f,
                ground + scene.camera.PlayerHeight, flight.position.z };
            log("parked");
            boarded = TryBoardHelicopter();
        } else {
            if (!g_game.vehicles.blackHawkCarryingPlayer || waitTime < 6.0f) return;
            boarded = TakeBlackHawkStickFromCabin();
        }
        log(boarded ? "boarded" : "board-failed");
        if (!boarded) { mode = 0; PostQuitMessage(0); }
        return;
    }
    flightTime += dt;
    const float t = flightTime;
    HelicopterFlightInput input;
    if (t < 3.0f) input.lift = 1.0f;
    else if (t < 9.0f) { input.forward = 1.0f; input.lift = 0.2f; }
    else if (t < 12.0f) { input.forward = 0.6f; input.yaw = 1.0f; }
    else if (t < 16.0f) {}                      // hands off: level and slow
    else if (t < 30.0f) input.lift = -1.0f;     // down to the ground
    g_pilotTestInputActive = PilotingHelicopter();
    g_pilotTestInput = input;
    if (mode == 1 && t > 5.0f && t < 6.0f) FirePilotedGunshipGun();
    if (mode == 1 && t > 6.0f && t < 6.1f) FirePilotedGunshipRocket();
    static bool captured = false;
    if (!captured && t >= 8.0f) {
        captured = true;
        char path[MAX_PATH] = {};
        if (GetEnvironmentVariableA("SGE_PILOT_CAPTURE", path, sizeof(path)) > 0)
            g_frameCapturePath = path;
    }
    traceTimer += dt;
    if (traceTimer >= 0.25f) { traceTimer = 0.0f; log("fly"); }
    if (!exited && t >= 30.0f) {
        exited = true;
        log("before-exit");
        ExitPilotedHelicopter();
        g_pilotTestInputActive = false;
        log("exited");
    }
    if (exited && t >= 33.0f) {
        log("after-exit");
        PostQuitMessage(0);
        mode = 0;
    }
}

// After the BlackHawk and the gunship have moved this frame.
static void UpdatePlayerHelicopters(float dt) {
    UpdatePilotTest(dt);
    UpdateParkedGunship(dt);
    VehicleSystem& vehicles = g_game.vehicles;
    // The BlackHawk can take its pilot away on its own: shot down (the pilot
    // is already out the door, see BeginBlackHawkCrash) or the run being
    // restarted under them.
    if (g_pilotedHelicopter == PilotedHelicopter::BlackHawk &&
        !vehicles.blackHawkPiloted)
        RestorePilotBody();
    if (PilotingHelicopter() &&
        (scene.player.health <= 0.0f || scene.player.downed || scene.ejected)) {
        ExitPilotedHelicopter();
        return;
    }
    static float lastYaw = 0.0f;
    static PilotedHelicopter lastKind = PilotedHelicopter::None;
    if (lastKind != g_pilotedHelicopter) {
        lastKind = g_pilotedHelicopter;
        if (PilotingHelicopter()) lastYaw = PilotedHelicopterYaw();
    }
    if (g_pilotedHelicopter == PilotedHelicopter::Gunship)
        PilotChaseCamera(g_parkedGunshipFlight.position,
                         g_parkedGunshipFlight.yaw, lastYaw,
                         kGunshipChaseDistance, 2.5f);
    else if (g_pilotedHelicopter == PilotedHelicopter::BlackHawk)
        PilotChaseCamera(vehicles.blackHawkPosition, vehicles.blackHawkYaw,
                         lastYaw, kBlackHawkChaseDistance, 3.5f);
}

static void DrawPilotCrosshair() {
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const ImVec2 c(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f);
    const ImU32 color = IM_COL32(120, 255, 140, 220);
    draw->AddCircle(c, 14.0f, color, 24, 1.5f);
    draw->AddLine({ c.x - 24.0f, c.y }, { c.x - 8.0f, c.y }, color, 1.5f);
    draw->AddLine({ c.x + 8.0f, c.y }, { c.x + 24.0f, c.y }, color, 1.5f);
    draw->AddLine({ c.x, c.y + 8.0f }, { c.x, c.y + 20.0f }, color, 1.5f);
}

// Board prompt on foot, flight readout and controls at the stick.
static void DrawHelicopterPilotPrompt() {
    if (!IsGameplayScreen() || g_game.loading.Active() ||
        g_insertionChoicePending) return;
    const VehicleSystem& vehicles = g_game.vehicles;
    const bool piloting = PilotingHelicopter();
    const bool inCabin = vehicles.blackHawkCarryingPlayer &&
                         vehicles.CanTakeBlackHawkControls();
    const bool canBoard = PilotCanBoard() &&
                          (GunshipBoardable() || BlackHawkBoardable());
    if (!piloting && !inCabin && !canBoard) return;
    if (scene.player.health <= 0.0f || scene.player.downed) return;
    if (g_pilotedHelicopter == PilotedHelicopter::Gunship) DrawPilotCrosshair();

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos({ viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                              viewport->WorkPos.y + viewport->WorkSize.y * 0.90f },
                            ImGuiCond_Always, { 0.5f, 0.5f });
    ImGui::SetNextWindowBgAlpha(0.65f);
    if (ImGui::Begin("##HelicopterPilotPrompt", nullptr,
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav)) {
        if (piloting) {
            const bool gunship = g_pilotedHelicopter == PilotedHelicopter::Gunship;
            const float height = PilotedHelicopterHeight();
            ImGui::Text("%s   W/S pitch   A/D turn   Q/R slide   Space/Ctrl climb/descend",
                        gunship ? "GUNSHIP" : "BLACKHAWK");
            if (gunship)
                ImGui::TextUnformatted("LMB cannon   RMB rockets");
            const float speed = gunship
                ? g_parkedGunshipFlight.ForwardSpeed()
                : vehicles.blackHawkFlight.ForwardSpeed();
            const float health = gunship
                ? g_parkedGunshipHealth / kParkedGunshipMaxHealth
                : vehicles.BlackHawkHealthFraction();
            ImGui::Text("Speed %.0f km/h   Alt %.0f m   Hull %.0f%%",
                        std::abs(speed) * 3.6f, (std::max)(0.0f, height),
                        health * 100.0f);
            ImGui::TextUnformatted(height <= kPilotStepOutHeight
                ? "E  Get out" : "E  Bail out");
        } else if (inCabin) {
            ImGui::TextUnformatted("E  Take the controls   (walk out the door to jump)");
        } else {
            ImGui::TextUnformatted("E  Fly helicopter");
        }
    }
    ImGui::End();
}
