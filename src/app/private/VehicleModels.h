#pragma once

// Private application implementation; included once by main.cpp in dependency order.

size_t LevelHumveeCount() { return g_levelHumveeSpawns.size(); }

XMMATRIX HumveeWorldMatrix(size_t index) {
    const XMMATRIX model =
        XMMatrixTranslation(-g_humveeModelCenter.x, -g_humveeModelMinY,
                            -g_humveeModelCenter.z) *
        XMMatrixScaling(g_humveeModelScale, g_humveeModelScale,
                        g_humveeModelScale) *
        XMMatrixRotationY(-XM_PIDIV2);
    XMFLOAT4X4 physicsPose;
    const bool authoring =
        g_game.session.Screen() == GameScreen::LevelEditor &&
        !g_levelEditor.IsPlaying();
    if (!authoring && g_destruction.GetVehicleTransform(index, physicsPose)) {
        // Model floor sits 0.95 m below chassis center.
        return model * XMMatrixTranslation(0.0f, -0.95f, 0.0f) *
               XMLoadFloat4x4(&physicsPose);
    }
    if (index >= g_levelHumveeSpawns.size()) return XMMatrixIdentity();
    const Transform& humvee = g_levelHumveeSpawns[index];
    return model *
           XMMatrixRotationY(XMConvertToRadians(humvee.rotation[1])) *
           XMMatrixTranslation(humvee.position[0],
                               humvee.position[1] - 0.95f,
                               humvee.position[2]);
}

XMMATRIX HumveeWorldMatrix() { return HumveeWorldMatrix(0); }

void PrepareHumveeModelForRender(size_t index) {
    if (!g_humveeModel || !g_humveeTurretNode) return;
    const float yaw = index < g_humveeGameplay.size()
        ? g_humveeGameplay[index].turretYaw : 0.0f;
    XMStoreFloat4(&g_humveeTurretNode->rotation,
        XMQuaternionRotationAxis(XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f), yaw));
    XMFLOAT4X4 identity;
    XMStoreFloat4x4(&identity, XMMatrixIdentity());
    g_humveeModel->UpdateGlobalTransform(identity);
}

void HumveeHeadlightPose(size_t index, XMFLOAT3& position,
                         XMFLOAT3& direction) {
    XMFLOAT4X4 poseStorage{};
    XMFLOAT3 chassisPosition{};
    XMFLOAT3 chassisForward{};
    XMMATRIX pose;
    if (g_destruction.GetVehicleTransform(
            index, poseStorage, &chassisPosition, &chassisForward)) {
        pose = XMLoadFloat4x4(&poseStorage);
    } else {
        if (index >= g_levelHumveeSpawns.size()) {
            position = {};
            direction = { 1.0f, 0.0f, 0.0f };
            return;
        }
        const Transform& humvee = g_levelHumveeSpawns[index];
        pose = XMMatrixRotationY(XMConvertToRadians(humvee.rotation[1])) *
               XMMatrixTranslation(humvee.position[0], humvee.position[1],
                                   humvee.position[2]);
        chassisPosition = { humvee.position[0], humvee.position[1],
                            humvee.position[2] };
        XMStoreFloat3(&chassisForward, XMVector3Normalize(pose.r[0]));
    }

    // The FBX is rotated -PI/2 when attached to the physics pose, leaving the
    // rendered nose opposite the chassis +X basis returned above.
    const XMVECTOR forward = XMVectorNegate(XMVector3Normalize(
        XMLoadFloat3(&chassisForward)));
    const XMVECTOR up = XMVector3Normalize(pose.r[1]);
    // Put the centre lamp just beyond the rendered front face so the Humvee
    // cannot shadow itself.
    const XMVECTOR lamp = XMVectorAdd(
        XMLoadFloat3(&chassisPosition),
        XMVectorAdd(XMVectorScale(forward, 2.30f),
                    XMVectorScale(up, 0.10f)));
    const XMVECTOR aim = XMVector3Normalize(
        XMVectorSubtract(forward, XMVectorScale(up, 0.18f)));
    XMStoreFloat3(&position, lamp);
    XMStoreFloat3(&direction, aim);
}

XMMATRIX SecondaryHumveeWorldMatrix() {
    return XMMatrixTranslation(-g_humveeModelCenter.x, -g_humveeModelMinY,
                               -g_humveeModelCenter.z) *
           XMMatrixScaling(g_humveeModelScale, g_humveeModelScale,
                           g_humveeModelScale) *
           XMMatrixRotationY(-XM_PIDIV2) *
           XMMatrixTranslation(g_secondaryHumveePosition.x,
                               g_secondaryHumveePosition.y,
                               g_secondaryHumveePosition.z);
}

XMMATRIX HelicopterWorldMatrix() {
    return XMMatrixTranslation(-g_helicopterModelCenter.x,
                               -g_helicopterModelCenter.y,
                               -g_helicopterModelCenter.z) *
           XMMatrixScaling(g_helicopterModelScale, g_helicopterModelScale,
                           g_helicopterModelScale) *
           XMMatrixRotationRollPitchYaw(g_helicopterPitch,
                                        g_helicopterYaw + XM_PI,
                                        g_helicopterRoll) *
           XMMatrixTranslation(g_helicopterPosition.x,
                               g_helicopterPosition.y,
                               g_helicopterPosition.z);
}

XMMATRIX SecondaryHelicopterWorldMatrix() {
    return XMMatrixTranslation(-g_helicopterModelCenter.x,
                               -g_helicopterModelCenter.y,
                               -g_helicopterModelCenter.z) *
           XMMatrixScaling(g_helicopterModelScale * g_helicopterLevelScale,
                           g_helicopterModelScale * g_helicopterLevelScale,
                           g_helicopterModelScale * g_helicopterLevelScale) *
           XMMatrixRotationRollPitchYaw(g_secondaryHelicopterPitch,
                                        g_secondaryHelicopterYaw + XM_PI,
                                        g_secondaryHelicopterRoll) *
           XMMatrixTranslation(g_secondaryHelicopterPosition.x,
                               g_secondaryHelicopterPosition.y,
                               g_secondaryHelicopterPosition.z);
}

XMFLOAT3 PrimaryHelicopterWeaponAimPoint() {
    const XMFLOAT3 forward{
        std::sin(g_helicopterYaw), 0.0f, std::cos(g_helicopterYaw) };
    const XMFLOAT3 muzzle{
        g_helicopterPosition.x + forward.x * 3.75f,
        g_helicopterPosition.y - 0.65f,
        g_helicopterPosition.z + forward.z * 3.75f };
    return LeadTargetPoint(
        muzzle, scene.camera.Position, g_playerVelocity, scene.projectileSpeed);
}

XMFLOAT3 SecondaryHelicopterWeaponAimPoint() {
    const XMFLOAT3 forward{
        std::sin(g_secondaryHelicopterYaw), 0.0f,
        std::cos(g_secondaryHelicopterYaw) };
    const XMFLOAT3 muzzle{
        g_secondaryHelicopterPosition.x + forward.x * 3.75f,
        g_secondaryHelicopterPosition.y - 0.65f,
        g_secondaryHelicopterPosition.z + forward.z * 3.75f };
    return LeadTargetPoint(
        muzzle, scene.camera.Position, g_playerVelocity, scene.projectileSpeed);
}

// Renderer-facing: the second airframe is drawn when the stress-test patrol is
// up, or whenever a reinforcement dropship is flying a wave in. Mirrors
// SecondaryHelicopterPresent(), which the gameplay-side gates use -- kept as a
// separate non-static symbol because the renderer headers reach it by extern.
bool SecondaryHelicopterVisible() {
    // Mirrors SecondaryHelicopterPresent: a shot-down craft keeps being drawn
    // while it falls and where it lands, even though UpdateDropship has already
    // released the slot back to Idle.
    if (g_secondaryHelicopterDead) return scene.showHelicopter;
    return g_stressTestMode || g_game.vehicles.DropshipActive();
}

// Renderer-facing destroyed state for the two gunships.
//
// Deliberately separate from the *Visible tests: a downed airframe keeps being
// drawn all the way through its fall and then sits on the ground as a wreck, so
// visibility stays true long after the aircraft stops working. Anything that
// should die WITH the helicopter rather than with its model -- the searchlight
// -- has to ask this instead.
//
// Reads the dead flag, not crashed: the light goes out when the craft is shot
// down, not when it finally hits the ground.
bool PrimaryHelicopterDestroyed() { return g_helicopterDead; }
bool SecondaryHelicopterDestroyed() { return g_secondaryHelicopterDead; }

static void ConfigureHelicopterBounds() {
    if (!g_helicopterModel || !g_helicopterModel->mesh) return;
    XMFLOAT3 minimum(FLT_MAX, FLT_MAX, FLT_MAX);
    XMFLOAT3 maximum(-FLT_MAX, -FLT_MAX, -FLT_MAX);
    for (const MeshPrimitive& primitive : g_helicopterModel->mesh->primitives) {
        for (size_t vertex = 0; vertex + 11 < primitive.vertices.size(); vertex += 12) {
            minimum.x = (std::min)(minimum.x, primitive.vertices[vertex]);
            minimum.y = (std::min)(minimum.y, primitive.vertices[vertex + 1]);
            minimum.z = (std::min)(minimum.z, primitive.vertices[vertex + 2]);
            maximum.x = (std::max)(maximum.x, primitive.vertices[vertex]);
            maximum.y = (std::max)(maximum.y, primitive.vertices[vertex + 1]);
            maximum.z = (std::max)(maximum.z, primitive.vertices[vertex + 2]);
        }
    }
    const float horizontalLength = (std::max)(
        maximum.x - minimum.x, maximum.z - minimum.z);
    if (horizontalLength <= 0.001f) return;
    g_helicopterModelCenter = {
        (minimum.x + maximum.x) * 0.5f,
        (minimum.y + maximum.y) * 0.5f,
        (minimum.z + maximum.z) * 0.5f };
    g_helicopterModelScale = 10.0f / horizontalLength;
}

XMMATRIX BlackHawkWorldMatrix() {
    return XMMatrixTranslation(-g_blackHawkModelCenter.x, -g_blackHawkModelMinY,
                               -g_blackHawkModelCenter.z) *
           XMMatrixScaling(g_blackHawkModelScale, g_blackHawkModelScale,
                           g_blackHawkModelScale) *
           XMMatrixRotationRollPitchYaw(g_game.vehicles.blackHawkPitch,
                                        g_blackHawkYaw,
                                        g_game.vehicles.blackHawkRoll) *
           XMMatrixTranslation(g_blackHawkPosition.x,
                               g_blackHawkPosition.y,
                               g_blackHawkPosition.z);
}

bool BlackHawkVisible() { return g_game.vehicles.blackHawkVisible; }

XMFLOAT3 BlackHawkRideWorldPosition() {
    return g_game.vehicles.BlackHawkRidePosition();
}

bool BlackHawkRappelActive() {
    return g_game.vehicles.BlackHawkIsRappelling();
}

// The rappel rope's box3d world. Owned here rather than in VehicleSystem so that
// header stays free of box3d -- see the forward declaration in VehicleSystem.h.
// vehicles.blackHawkRope points at this while a rope is out and is null the rest
// of the time, which is what every rope call site tests.
static RopeSwing g_blackHawkRope;

// Hangs the rope from the aircraft's anchor, long enough to reach the ground from
// the hover height. Link count follows the actual gap rather than RopeSwing's
// 3 m default, which would leave the rope dangling well short of the terrain.
static void SpawnBlackHawkRope() {
    VehicleSystem& vehicles = g_game.vehicles;
    const XMFLOAT3 anchor = vehicles.BlackHawkRopeAnchorPosition();
    // Reach from the anchor to the ground, with a little slack so the bottom of
    // the rope lies on the terrain instead of stopping taut above it.
    const float drop = (std::max)(2.0f,
        anchor.y - vehicles.blackHawkGroundY) * 1.08f;
    // Fewer, longer links: every extra ball joint is more constraint error for
    // the solver, and RopeSwing's own notes prefer length over count.
    constexpr float kTargetLinkLength = 1.5f;
    const int linkCount = (std::max)(4, (std::min)(14,
        (int)std::lround(drop / kTargetLinkLength)));
    const float linkLength = drop / (float)linkCount;

    g_blackHawkRope.SetGroundY(vehicles.blackHawkGroundY);
    g_blackHawkRope.Initialize(anchor, linkCount, linkLength, 0.0f,
                               RopeSwing::Payload::None);
    vehicles.blackHawkRope = &g_blackHawkRope;
}

static void ReleaseBlackHawkRope() {
    g_blackHawkRope.Shutdown();
    g_game.vehicles.blackHawkRope = nullptr;
}

// Drawable rope links for the renderers, which are separate translation units
// and cannot see g_blackHawkRope directly. Empty while no rope is out.
const std::vector<RopeItem>& BlackHawkRopeItems() {
    static const std::vector<RopeItem> kNone;
    return g_game.vehicles.blackHawkRope ? g_blackHawkRope.GetItems() : kNone;
}

// Fast-ropes hanging from the dropship, one per troop still descending. Rebuilt
// each frame from the actors' live positions.
//
// Deliberately not box3d ropes like the player's: a wave puts up to six of these
// out at once, and a simulated rope per troop would cost far more than a
// straight line from the airframe to a hand that is already descending at a
// fixed rate. The player's rope is simulated because the player swings on it and
// can cut it; these are only ever a taut line under a controlled descent.
static std::vector<RopeItem> g_dropshipRopeItems;

const std::vector<RopeItem>& DropshipRopeItems() {
    return g_dropshipRopeItems;
}

// One rope's worth of links between the craft and a descending actor.
static void AppendDropshipRope(const XMFLOAT3& top, const XMFLOAT3& bottom) {
    // Matches the player rope's link length so both read as the same rope.
    constexpr float kLinkLength = 0.5f;
    constexpr float kLinkHalfThickness = 0.045f;
    const XMFLOAT3 kRopeColor{ 0.16f, 0.15f, 0.14f };

    const float dx = bottom.x - top.x;
    const float dy = bottom.y - top.y;
    const float dz = bottom.z - top.z;
    const float length = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (length < 0.05f) return;

    // Cap the link count so a troop released far above the ground cannot spike
    // the draw list.
    const int links = (std::min)(64,
        (std::max)(1, static_cast<int>(length / kLinkLength)));
    const float linkHalfLength = length / static_cast<float>(links) * 0.5f;

    // Orient the links along the rope. The rope is near-vertical in practice,
    // so the up-axis cross product is stable here.
    const XMVECTOR direction = XMVector3Normalize(XMVectorSet(dx, dy, dz, 0.0f));
    const XMVECTOR reference = std::abs(dy) > 0.99f * length
        ? XMVectorSet(1.0f, 0.0f, 0.0f, 0.0f)
        : XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f);
    const XMVECTOR right = XMVector3Normalize(
        XMVector3Cross(reference, direction));
    const XMVECTOR up = XMVector3Cross(direction, right);

    XMMATRIX basis = XMMatrixIdentity();
    basis.r[0] = right;
    basis.r[1] = direction;
    basis.r[2] = up;

    for (int i = 0; i < links; ++i) {
        const float t = (static_cast<float>(i) + 0.5f) / static_cast<float>(links);
        RopeItem item;
        item.color = kRopeColor;
        item.shape = 0;   // box: the VBDraw path only draws cubes anyway
        const XMMATRIX world =
            XMMatrixScaling(kLinkHalfThickness, linkHalfLength, kLinkHalfThickness) *
            basis *
            XMMatrixTranslation(top.x + dx * t, top.y + dy * t, top.z + dz * t);
        XMStoreFloat4x4(&item.transform, world);
        g_dropshipRopeItems.push_back(item);
    }
}

// Services the rope requests VehicleSystem raises, and steps the simulation.
// Runs straight after UpdateBlackHawk so the rope sees the pose it was just
// given, and before the player is placed from it.
static void UpdateBlackHawkRope(float deltaTime) {
    VehicleSystem& vehicles = g_game.vehicles;

    if (vehicles.blackHawkRopeReleaseRequested) {
        vehicles.blackHawkRopeReleaseRequested = false;
        ReleaseBlackHawkRope();
    }
    if (vehicles.blackHawkRopeSpawnRequested) {
        vehicles.blackHawkRopeSpawnRequested = false;
        SpawnBlackHawkRope();
    }
    if (!vehicles.blackHawkRope) return;

    g_blackHawkRope.Update(deltaTime);

    // A severed rope is kept alive only long enough to be seen falling.
    if (vehicles.blackHawkRopeCut && g_blackHawkRope.CutSectionSettled())
        ReleaseBlackHawkRope();
}

// The rope has been shot through with the player on it. Hands them from the
// rope to ordinary falling movement and charges them for the drop.
//
// Deliberately does NOT tear the rope down: the severed section keeps simulating
// so the break is visible, and UpdateBlackHawkRope releases it once it settles.
// The player must have a position on every frame of this handover -- the rope
// supplies it up to the cut, gravity from the cut on, and neither leaves a gap.
static void HandleBlackHawkRopeCut() {
    VehicleSystem& vehicles = g_game.vehicles;
    if (!vehicles.NotifyBlackHawkRopeCut()) return;

    // Height still to fall, from wherever on the rope they had got to.
    const float remaining = (std::max)(0.0f,
        scene.camera.Position.y - scene.camera.PlayerHeight -
        vehicles.blackHawkGroundY);

    // Hand over to gravity. Seeding the descent rate rather than starting from
    // rest keeps the fall continuous instead of hitching at the cut.
    scene.camera.IsGrounded = false;
    scene.camera.VerticalVelocity =
        -(VehicleSystem::BlackHawkRappelHoverHeight /
          VehicleSystem::BlackHawkRappelTime);

    // Scaled by how far there was left to fall, so a cut just above the ground
    // barely stings and one straight out of the door hurts.
    constexpr float kSafeFall = 3.0f;      // free below this
    constexpr float kDamagePerMetre = 7.5f;
    if (remaining > kSafeFall)
        scene.DamagePlayer((remaining - kSafeFall) * kDamagePerMetre);

    std::cout << "BlackHawk rappel rope cut at progress "
              << vehicles.blackHawkRopeCutProgress << ", "
              << remaining << " m to fall\n";
}

XMFLOAT3 BlackHawkRappelPlayerWorldPosition() {
    const VehicleSystem& vehicles = g_game.vehicles;
    const float progress = (std::max)(0.0f, (std::min)(
        1.0f, vehicles.blackHawkRappelProgress));

    // Ride the live rope when there is one, so the player swings with it in X and
    // Z instead of sliding down a straight line. PositionAlongRope returns the
    // player's centre, which is what the caller lifts to eye height.
    if (vehicles.blackHawkRope && !vehicles.blackHawkRopeCut) {
        const XMFLOAT3 onRope = vehicles.blackHawkRope->PositionAlongRope(progress);
        // Never let the rope drive the player below standing height on the
        // terrain: the chain's bottom tip rests slightly into the ground plane.
        const float floorCentre =
            vehicles.blackHawkGroundY + scene.camera.PlayerHeight * 0.5f;
        return { onRope.x, (std::max)(floorCentre, onRope.y), onRope.z };
    }

    // No rope (or it has been cut): fall back to the original straight-line
    // descent from the anchor, which is also what a model with no rope anchor and
    // a build with the rope disabled will use.
    const XMFLOAT3 anchor = vehicles.BlackHawkRopeAnchorPosition();
    const float groundCentre =
        vehicles.blackHawkGroundY + scene.camera.PlayerHeight * 0.5f;
    return { anchor.x,
             anchor.y + (groundCentre - anchor.y) * progress,
             anchor.z };
}

XMFLOAT3 BlackHawkRideDebugInfo(XMFLOAT3& outLocal) {
    const VehicleSystem& vehicles = g_game.vehicles;
    outLocal = { vehicles.blackHawkRideSide, vehicles.blackHawkRideHeight,
                 vehicles.blackHawkRideForward };
    return vehicles.BlackHawkRidePosition();
}

XMFLOAT3 BlackHawkRideMeshPosition() { return g_blackHawkRideMeshPosition; }
XMFLOAT3 BlackHawkModelCentre() {
    return { g_blackHawkModelCenter.x, g_blackHawkModelMinY,
             g_blackHawkModelCenter.z };
}
float BlackHawkModelScale() { return g_blackHawkModelScale; }

static LevelInsertionMode ResolvedInsertionMode();

static LevelInsertionMode& g_playerInsertionChoice =
    g_game.mission.Loadout().insertion;
static bool g_insertionChoicePending = false;
static bool g_insertionChoiceCursorReleased = false;

// Which cabin door the player rides out of on a helicopter insertion, picked on
// the deployment screen. The GLB authors one seat only -- "PlayerRide" measures
// side +1.81, i.e. starboard -- so the left seat is that same point mirrored
// across the fuselage centreline rather than a second authored empty.
//
// The offsets ConfigureBlackHawkRideFromModel derives are cached at load and
// re-applied through the mirror every time an insertion is armed, so flipping
// the seat between runs never compounds an earlier flip.
static bool g_playerRidesLeftSeat = false;
static float g_blackHawkRideSideBase = 0.0f;
static float g_blackHawkRopeSideBase = 0.0f;
// Raised when an insertion is armed, cleared on the first frame the player is
// actually seated. Aiming the camera out of the chosen door is a one-shot: the
// player keeps free look for the rest of the flight, so re-aiming every frame
// would fight the mouse instead of just setting the starting view.
static bool g_blackHawkRideFacingPending = false;
// Where the riding player is standing, in the aircraft's own local frame
// (+X starboard, +Y up, +Z forward -- the basis BlackHawkRidePosition uses).
//
// Tracked in cabin space rather than world space because the aircraft is
// moving: a world-space position would need the frame's motion subtracted out
// every tick, and any error there reads as the player sliding across the deck.
// In cabin space the aircraft's motion is simply not part of the problem, and
// the walls are a fixed box.
//
// Seeded from the authored seat offset the first time the player boards, so the
// ride still begins in the door they picked.
static XMFLOAT3 g_blackHawkCabinLocal{};
static bool g_blackHawkCabinLocalValid = false;
// Half-extents of the walkable cabin, in metres, measured from the ride point.
// Deliberately smaller than the fuselage: the deck is narrower than the widest
// part of the airframe, and stopping the player short of the skin keeps the
// camera from clipping through it.
//
// Sized against the MH-60 at its 34 m target length, which is the airframe this
// walk exists for. At normal walking speed a real UH-60 cabin is barely two
// paces across, so a strictly accurate box would put the player into a wall the
// moment they touched a key -- these are the enlarged aircraft's proportions,
// which is the whole reason it is scaled up.
static constexpr float kCabinHalfWidth = 1.75f;
static constexpr float kCabinHalfLength = 2.70f;

// Vertical state for the cabin walk, in cabin space. The deck is a moving
// platform, so the player cannot use the world-space fall in CameraDX12: its
// ground plane is the terrain far below, and gravity there would drag them
// through the floor. This is the same integration kept in the aircraft's
// frame instead, which is what lets the player jump on a banking deck and land
// back on it.
//
// Height is an offset ABOVE the deck rather than an absolute, so it stays
// correct as the ride height changes and reads as zero whenever standing.
static float g_blackHawkCabinHeight = 0.0f;
static float g_blackHawkCabinVertVel = 0.0f;
static bool  g_blackHawkCabinGrounded = true;
// Latches the jump key so holding Space bunny-hops no faster than tapping it,
// matching the ground jump, which fires off a fresh keypress.
static bool  g_blackHawkCabinJumpHeld = false;
// How far past the deck edge the player keeps footing. Beyond this they are
// over open air and fall out of the aircraft under their own weight, which is
// the "walk out on my own" exit as opposed to the E bail-out.
static constexpr float kCabinEdgeGrace = 0.15f;

// Re-derives the seat and rope offsets for the currently chosen door. Safe to
// call repeatedly: it always starts from the cached authored values.
static void ApplyBlackHawkSeatSide() {
    VehicleSystem& vehicles = g_game.vehicles;
    const float mirror = g_playerRidesLeftSeat ? -1.0f : 1.0f;
    vehicles.blackHawkRideSide = g_blackHawkRideSideBase * mirror;
    vehicles.blackHawkRopeSide = g_blackHawkRopeSideBase * mirror;
}

static std::vector<XMFLOAT3> g_deploymentZones;
static int g_selectedDeploymentZone = -1;
static XMFLOAT3 g_deploymentTarget{};
static bool g_deploymentTargetValid = false;
static float g_deploymentFlythroughTime = 0.0f;

// Marines the player elects to bring on the transport, chosen on the deploy
// screen. They are NOT spawned when DEPLOY is pressed: the squad rides in the
// same aircraft or boat the player does, so it only reaches the ground if that
// transport does. A crash or a sinking loses everyone aboard.
static int g_deploymentMarineCount = 0;
static constexpr int kMaxDeploymentMarines = 8;
// What one marine costs to bring along. Unlike an armory item this is not a
// purchase that stays bought: the squad is consumed by the mission it deploys
// on, so the same money is spent again next time. That is the whole tension --
// a full squad is 16000 against a 2500 comm tower, and losing the transport
// loses the lot.
static constexpr int kDeploymentMarinePrice = 2000;
// Raised for one frame by the ride/release code the moment a transport sets the
// player down intact. Consumed after both vehicle updates run, because the
// spawn needs terrain helpers declared further down this file.
static bool g_marineDropPending = false;
// Where the squad steps off, banked alongside the flag: the wreck-thrown and
// bailed-out paths move the camera, so the landing point cannot be recovered
// from it after the fact.
static XMFLOAT3 g_marineDropOrigin{};

// Armory ownership. The deploy screen is a shop: a weapon, grenade or gear item
// is bought once and stays bought for the rest of the career, so re-picking it
// on a later mission is free. Kept as bitmasks keyed by the same ids the
// loadout stores, and saved alongside the wallet -- a career that spent 4200 on
// an RPG would otherwise be charged again on the next launch.
//
// Attachments key by string id rather than an index because they are loaded
// from data, so those live in a set instead of a mask.
static uint32_t g_ownedWeapons = 0;
static uint32_t g_ownedGrenades = 0;
static uint32_t g_ownedGear = 0;
static std::unordered_set<std::string> g_ownedAttachments;

// Kit bought at the home base counter, held across the flight out. The base is
// where a run is outfitted, so its purchases are for the mission the player is
// about to fly to -- not the hub they bought them in. Without this the rental
// clear that runs as the destination's deploy screen opens would strip the kit
// between paying for it and landing with it, which reads as the shop taking the
// money and issuing nothing.
static bool g_baseKitPending = false;
static MissionLoadout g_baseKitLoadout{};
static uint32_t g_baseKitWeapons = 0;
static uint32_t g_baseKitGrenades = 0;
static uint32_t g_baseKitGear = 0;
static std::unordered_set<std::string> g_baseKitAttachments;
// The rails as the counter left them: (weapon, attachment id) pairs actually
// fitted, which is not the same question as what is owned -- see the restore in
// ClearMissionRentals.
static std::vector<std::pair<int, std::string>> g_baseKitFitted;
