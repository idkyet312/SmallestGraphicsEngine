#pragma once

// Private application implementation; included once by main.cpp in dependency order.

// Sends the insertion boat in toward the player's spawn. The seaborne twin of
// StartBlackHawkInsertionAtPlayerSpawn: it runs in across the water on the
// heading the player is facing away from, so the approach crosses their view.
static void StartInsertionBoatRunAtPlayerSpawn() {
    const XMFLOAT3 spawn = g_deploymentTargetValid
        ? g_deploymentTarget : scene.camera.Position;
    auto params = CurrentTerrainParams();
    params.heightScale = scene.terrainHeightScale;
    const float groundY =
        TerrainRendererDX12::HeightAt(params, spawn.x, spawn.z);
    // Boats ride the water plane, never the terrain under it.
    const float waterY = (std::max)(0.0f, groundY);
    const float facing =
        DeploymentPlanner::HeadingTowardIslandCenter(spawn);
    // Only called when this level inserts by boat, so the run always carries
    // the player. See ResolvedInsertionMode.
    g_game.vehicles.BeginInsertionBoatRun(
        { spawn.x, waterY, spawn.z }, waterY, facing);
}

// Keeps the player glued to the deck while riding, and puts them ashore (or in
// the water) when the run ends. Mirrors RidePlayerInBlackHawk.
static void RidePlayerInInsertionBoat() {
    VehicleSystem& vehicles = g_game.vehicles;
    if (vehicles.insertionBoatCarryingPlayer) {
        const XMFLOAT3 centre = vehicles.InsertionBoatRidePosition();
        scene.camera.Position = { centre.x,
                                  centre.y + scene.camera.PlayerHeight * 0.5f,
                                  centre.z };
        scene.camera.VerticalVelocity = 0.0f;
        scene.camera.IsGrounded = true;
        scene.camera.FloorY =
            scene.camera.Position.y - scene.camera.PlayerHeight;
        return;
    }

    // Bailing out early: step off to starboard wherever the boat happens to be,
    // rather than teleporting to the landing point.
    if (vehicles.insertionBoatBailedOut) {
        vehicles.insertionBoatBailedOut = false;
        const float rightX = std::cos(vehicles.insertionBoatYaw);
        const float rightZ = -std::sin(vehicles.insertionBoatYaw);
        constexpr float clearDistance = 3.5f;
        const XMFLOAT3 centre = vehicles.InsertionBoatRidePosition();
        scene.camera.Position = { centre.x + rightX * clearDistance,
                                  centre.y + scene.camera.PlayerHeight * 0.5f,
                                  centre.z + rightZ * clearDistance };
        scene.camera.VerticalVelocity = 0.0f;
        return;
    }

    if (!vehicles.insertionBoatDroppedPlayer) return;

    // Went down with the boat: dumped in the water beside the wreck.
    if (vehicles.insertionBoatJustSank) {
        const float rightX = std::cos(vehicles.insertionBoatYaw);
        const float rightZ = -std::sin(vehicles.insertionBoatYaw);
        constexpr float thrownClear = 4.0f;
        scene.camera.Position = {
            vehicles.insertionBoatPosition.x + rightX * thrownClear,
            vehicles.insertionBoatWaterY + scene.camera.PlayerHeight,
            vehicles.insertionBoatPosition.z + rightZ * thrownClear };
        scene.camera.VerticalVelocity = 0.0f;
        scene.camera.IsGrounded = true;
        return;
    }

    // Normal landing: step off the bow onto the shore.
    const float forwardX = std::sin(vehicles.insertionBoatYaw);
    const float forwardZ = std::cos(vehicles.insertionBoatYaw);
    constexpr float exitDistance = 4.0f;
    auto params = CurrentTerrainParams();
    params.heightScale = scene.terrainHeightScale;
    const float exitX = vehicles.insertionBoatLanding.x + forwardX * exitDistance;
    const float exitZ = vehicles.insertionBoatLanding.z + forwardZ * exitDistance;
    const float shoreY = (std::max)(vehicles.insertionBoatWaterY,
        TerrainRendererDX12::HeightAt(params, exitX, exitZ));
    // Hull made the beach, so the squad wades ashore with the player. The sunk
    // branch above returns first, which is what drowns them with the boat.
    g_marineDropPending = true;
    g_marineDropOrigin = { exitX, shoreY, exitZ };
    scene.camera.Position = { exitX, shoreY + scene.camera.PlayerHeight, exitZ };
    scene.camera.VerticalVelocity = 0.0f;
    scene.camera.IsGrounded = true;
}

// Smoke trail as the hull fails, then the FX when it finally goes under. Same
// read-it-at-a-glance smoke ramp the BlackHawk uses.
static void UpdateInsertionBoatDamageEffects(float deltaTime) {
    VehicleSystem& vehicles = g_game.vehicles;

    static float smokeTimer = 0.0f;
    const float severity = vehicles.InsertionBoatDamageSeverity();
    if (severity > 0.0f && !vehicles.InsertionBoatIsSunk()) {
        smokeTimer -= deltaTime;
        if (smokeTimer <= 0.0f) {
            smokeTimer = 0.34f - 0.29f * severity;
            const float radius = 0.35f + 0.95f * severity;
            const float intensity = 0.35f + 1.15f * severity;
            // Trail from the engine well, aft of the model origin.
            const float backX = -std::sin(vehicles.insertionBoatYaw) * 1.2f;
            const float backZ = -std::cos(vehicles.insertionBoatYaw) * 1.2f;
            scene.SpawnSmokeBurst(
                { vehicles.insertionBoatPosition.x + backX,
                  vehicles.insertionBoatPosition.y -
                      vehicles.insertionBoatSinkOffset + 0.8f,
                  vehicles.insertionBoatPosition.z + backZ },
                radius, intensity);
        }
    } else {
        smokeTimer = 0.0f;
    }

    if (!vehicles.insertionBoatJustSank) return;

    // No crater or collision bake here, unlike the BlackHawk: the hull goes
    // under open water, so there is no terrain to gouge and no wreck to walk on.
    const XMFLOAT3 wentDown = vehicles.insertionBoatPosition;
    scene.SpawnExplosionFX({ wentDown.x, wentDown.y + 1.0f, wentDown.z },
                           8.0f, 1.1f);
    scene.SpawnSmokeBurst(wentDown, 3.0f, 3.2f);

    // Going down with it hurts, but less than riding a helicopter in: the water
    // breaks the fall.
    scene.DamagePlayer(30.0f);
}

// Fires the crash FX and hurts anyone still strapped in. Runs off the one-frame
// flag the vehicle update raises, so it happens exactly once per wreck.
// Defined with the collision code further down; called here the frame the wreck
// settles, once its final pose is written.
static void BakeBlackHawkCollisionMesh();

static void UpdateBlackHawkCrashEffects(float deltaTime) {
    VehicleSystem& vehicles = g_game.vehicles;

    // The smoke trail IS the health readout: it starts as thin intermittent
    // wisps at the damage threshold and builds to a thick continuous plume as
    // the airframe approaches zero, so the state can be read at a glance from
    // outside without a HUD element.
    static float smokeTimer = 0.0f;
    const float severity = vehicles.BlackHawkDamageSeverity();
    if (severity > 0.0f && !vehicles.BlackHawkIsDown()) {
        smokeTimer -= deltaTime;
        if (smokeTimer <= 0.0f) {
            // Puff spacing tightens from a lazy 0.34 s to a near-continuous
            // 0.05 s, which is what sells "about to go down".
            smokeTimer = 0.34f - 0.29f * severity;
            // Small light wisps early, big dark billows late.
            const float radius = 0.35f + 0.95f * severity;
            const float intensity = 0.35f + 1.15f * severity;
            // Trail from the engine deck, slightly aft of the model origin.
            const float backX = -std::sin(vehicles.blackHawkYaw) * 0.6f;
            const float backZ = -std::cos(vehicles.blackHawkYaw) * 0.6f;
            scene.SpawnSmokeBurst(
                { vehicles.blackHawkPosition.x + backX,
                  vehicles.blackHawkPosition.y + 1.1f,
                  vehicles.blackHawkPosition.z + backZ },
                radius, intensity);
        }
    } else {
        smokeTimer = 0.0f;
    }

    if (!vehicles.blackHawkJustCrashed) return;

    const XMFLOAT3 impact = vehicles.blackHawkPosition;
    // Carve the furrow before baking collision: the stamps move the ground, and
    // the wreck's triangles are only correct against the terrain as it ends up.
    AddBlackHawkCrashCraters(impact, vehicles.blackHawkYaw);
    // The wreck is static from here, so bake its mesh once for collision. Runs
    // after the vehicle update has written the final resting pose.
    BakeBlackHawkCollisionMesh();

    scene.SpawnExplosionFX({ impact.x, impact.y + 1.6f, impact.z }, 11.0f, 1.2f);
    scene.SpawnSmokeBurst(impact, 3.2f, 3.5f);
    if (g_destruction.IsInitialized())
        g_destruction.ApplyExplosion(impact, 5.5f, 65.0f, 14.0f);

    // Riding it down hurts. Not instantly lethal at full health, so a healthy
    // player walks away from the wreck.
    scene.DamagePlayer(55.0f);
}

// Normalized to a fixed footprint (so the cabin is big enough to carry the
// player in) and pinned by its skids, so the landed pose rests on the terrain
// rather than sinking to the model's centre. The normalization makes the
// on-screen size independent of however the source GLB is scaled.
// Accumulates a node subtree's vertex bounds in model-root space, folding each
// node's local transform in on the way down. The GLB's mesh hangs off a child of
// ModelRoot with its own rotation and scale, so bounds taken from the root's own
// (absent) mesh would come back empty and leave the pose unscaled.
static void AccumulateNodeBounds(const SceneNode* node, const XMMATRIX& parentToRoot,
                                 XMFLOAT3& minimum, XMFLOAT3& maximum) {
    if (!node) return;
    const XMMATRIX localToRoot =
        XMLoadFloat4x4(&node->localTransform) * parentToRoot;
    if (node->mesh) {
        for (const MeshPrimitive& primitive : node->mesh->primitives) {
            for (size_t vertex = 0; vertex + 11 < primitive.vertices.size();
                 vertex += 12) {
                const XMVECTOR local = XMVectorSet(primitive.vertices[vertex],
                                                   primitive.vertices[vertex + 1],
                                                   primitive.vertices[vertex + 2],
                                                   1.0f);
                XMFLOAT3 world{};
                XMStoreFloat3(&world, XMVector3TransformCoord(local, localToRoot));
                minimum.x = (std::min)(minimum.x, world.x);
                minimum.y = (std::min)(minimum.y, world.y);
                minimum.z = (std::min)(minimum.z, world.z);
                maximum.x = (std::max)(maximum.x, world.x);
                maximum.y = (std::max)(maximum.y, world.y);
                maximum.z = (std::max)(maximum.z, world.z);
            }
        }
    }
    for (const auto& child : node->children)
        AccumulateNodeBounds(child.get(), localToRoot, minimum, maximum);
}

static void ConfigureBlackHawkBounds() {
    if (!g_blackHawkModel) return;
    g_blackHawkModel->RefreshHierarchy();
    XMFLOAT3 minimum(FLT_MAX, FLT_MAX, FLT_MAX);
    XMFLOAT3 maximum(-FLT_MAX, -FLT_MAX, -FLT_MAX);
    AccumulateNodeBounds(g_blackHawkModel.get(), XMMatrixIdentity(),
                         minimum, maximum);
    if (minimum.x > maximum.x) return;
    const float horizontalLength = (std::max)(
        maximum.x - minimum.x, maximum.z - minimum.z);
    if (horizontalLength <= 0.001f) return;
    g_blackHawkModelCenter = {
        (minimum.x + maximum.x) * 0.5f,
        (minimum.y + maximum.y) * 0.5f,
        (minimum.z + maximum.z) * 0.5f };
    g_blackHawkModelMinY = minimum.y;
    // Target the longest horizontal axis, which on this model is nose-to-tail
    // including the rotor overhang. 20 m there puts the aircraft at real UH-60
    // proportions: ~16 m rotor span and ~5 m to the rotor head.
    //
    // The MH-60 is deliberately oversized against that. Its cabin is meant to
    // be walked around inside, and a real airframe normalised to 20 m has an
    // interior barely wider than the player capsule -- the doorway alone is
    // narrower than a walking collision radius. Scaling the whole aircraft up
    // is what buys usable floor space without re-authoring the mesh.
    //
    // 27.2 m is the original 34 m brought down by a fifth. A third (22.78 m)
    // was tried first and read as too small, so this is the shallower cut that
    // keeps the cabin comfortably walkable.
    const float targetLength =
        g_insertionAirframe == InsertionAirframe::NewBlackHawk ? 27.2f : 20.0f;
    g_blackHawkModelScale = targetLength / horizontalLength;
}

// Depth-first search for a node by name.
static std::shared_ptr<SceneNode> FindNodeByName(
    const std::shared_ptr<SceneNode>& node, const std::string& name) {
    if (!node) return nullptr;
    if (node->name == name) return node;
    for (const auto& child : node->children)
        if (auto found = FindNodeByName(child, name)) return found;
    return nullptr;
}

// Attaches the riding player to the "PlayerRide" empty authored in the GLB, so
// the spot is placed in Blender rather than tuned by hand here. Silently keeps
// the hand-tuned fallbacks when the model has no such node.
//
// Must run after ConfigureBlackHawkBounds: the empty's position is in raw mesh
// space, and BlackHawkWorldMatrix re-centres the model and pins it by its skids
// before scaling, so the offset has to go through that same transform to land in
// the aircraft's local frame.
static void ConfigureBlackHawkRideFromModel() {
    if (!g_blackHawkModel) return;
    VehicleSystem& vehicles = g_game.vehicles;
    std::shared_ptr<SceneNode> ride =
        FindNodeByName(g_blackHawkModel, "PlayerRide");
    if (!ride) {
        // Reset rather than return. These offsets are cached globals, so an
        // airframe with no authored ride empty would otherwise keep whatever
        // the previously selected aircraft measured -- seating the player
        // beside a differently shaped fuselage, or in mid-air next to it.
        // The authored defaults are a cabin-door seat on a 20 m airframe,
        // which is the size every airframe is normalised to.
        vehicles.blackHawkRideSide = 1.15f;
        vehicles.blackHawkRideForward = 0.4f;
        vehicles.blackHawkRideHeight = 2.1f;
        g_blackHawkRideSideBase = vehicles.blackHawkRideSide;
        g_blackHawkRideMeshPosition = {};
        std::cout << "BlackHawk has no PlayerRide empty; using default seat "
                     "offsets\n";
        return;
    }

    g_blackHawkModel->RefreshHierarchy();
    XMFLOAT3 meshPosition{};
    XMStoreFloat3(&meshPosition,
                  XMVector3TransformCoord(
                      XMVectorZero(), XMLoadFloat4x4(&ride->globalTransform)));
    vehicles.blackHawkRideSide =
        (meshPosition.x - g_blackHawkModelCenter.x) * g_blackHawkModelScale;
    vehicles.blackHawkRideForward =
        (meshPosition.z - g_blackHawkModelCenter.z) * g_blackHawkModelScale;
    vehicles.blackHawkRideHeight =
        (meshPosition.y - g_blackHawkModelMinY) * g_blackHawkModelScale;
    // Authored (starboard) side, kept so the left-seat mirror in
    // ApplyBlackHawkSeatSide always folds from the model rather than from
    // whatever the previous run left behind.
    g_blackHawkRideSideBase = vehicles.blackHawkRideSide;

    g_blackHawkRideMeshPosition = meshPosition;
    std::cout << "BlackHawk PlayerRide mesh pos " << meshPosition.x << ", "
              << meshPosition.y << ", " << meshPosition.z
              << " | centre " << g_blackHawkModelCenter.x << ", "
              << g_blackHawkModelMinY << ", " << g_blackHawkModelCenter.z
              << " | scale " << g_blackHawkModelScale
              << " -> side " << vehicles.blackHawkRideSide << ", forward "
              << vehicles.blackHawkRideForward << ", height "
              << vehicles.blackHawkRideHeight << "\n";
}

// Ties the rappel rope to the "RopeAnchor" empty authored in the GLB, so the
// rope hangs from the door frame rather than from the seat. Same mesh-space to
// aircraft-local conversion as ConfigureBlackHawkRideFromModel, and the same
// ordering requirement: must run after ConfigureBlackHawkBounds.
//
// With no such node the rope falls back to the ride point, which is inside the
// cabin -- close enough to look right on a model that was never authored for
// this, and blackHawkRopeAnchorValid records that the fallback is in use.
static void ConfigureBlackHawkRopeAnchorFromModel() {
    if (!g_blackHawkModel) return;
    VehicleSystem& vehicles = g_game.vehicles;

    std::shared_ptr<SceneNode> anchor =
        FindNodeByName(g_blackHawkModel, "RopeAnchor");
    if (!anchor) {
        vehicles.blackHawkRopeSide = vehicles.blackHawkRideSide;
        vehicles.blackHawkRopeForward = vehicles.blackHawkRideForward;
        vehicles.blackHawkRopeHeight = vehicles.blackHawkRideHeight;
        vehicles.blackHawkRopeAnchorValid = false;
        // Falling back to the seat means the rope mirrors with it, so the base
        // the seat-side flip folds from is the seat's own authored side.
        g_blackHawkRopeSideBase = g_blackHawkRideSideBase;
        return;
    }

    g_blackHawkModel->RefreshHierarchy();
    XMFLOAT3 meshPosition{};
    XMStoreFloat3(&meshPosition,
                  XMVector3TransformCoord(
                      XMVectorZero(), XMLoadFloat4x4(&anchor->globalTransform)));

    vehicles.blackHawkRopeSide =
        (meshPosition.x - g_blackHawkModelCenter.x) * g_blackHawkModelScale;
    vehicles.blackHawkRopeForward =
        (meshPosition.z - g_blackHawkModelCenter.z) * g_blackHawkModelScale;
    vehicles.blackHawkRopeHeight =
        (meshPosition.y - g_blackHawkModelMinY) * g_blackHawkModelScale;
    vehicles.blackHawkRopeAnchorValid = true;
    g_blackHawkRopeSideBase = vehicles.blackHawkRopeSide;

    std::cout << "BlackHawk RopeAnchor -> side " << vehicles.blackHawkRopeSide
              << ", forward " << vehicles.blackHawkRopeForward
              << ", height " << vehicles.blackHawkRopeHeight << "\n";
}

// Depth-first search for the first node carrying a mesh.
static std::shared_ptr<SceneNode> FindMeshNode(
    const std::shared_ptr<SceneNode>& node) {
    if (!node) return nullptr;
    if (node->mesh) return node;
    for (const auto& child : node->children)
        if (auto found = FindMeshNode(child)) return found;
    return nullptr;
}

// Rebuilds the BlackHawk's bone palette for the current rotor angle. The rig's
// "Bone" joint owns every rotor vertex, so turning that one bone spins the disc
// and nothing else; the airframe rides the other joint, which stays at bind.
//
// Palette entry = globalPose * inverseBind, matching what the skinning shaders
// at t12 expect. Bones are stored in hierarchy order, so one forward pass
// resolves parents before children.
static void UpdateBlackHawkPalette() {
    const Skeleton& skeleton = g_blackHawkSkeleton;
    const size_t boneCount = skeleton.BoneCount();
    if (boneCount == 0) return;

    // The mast axis is the rig's local Y; the model is Y-up with its transforms
    // baked into the vertices.
    const float angle = std::fmod(g_blackHawkRotorSpin, XM_2PI);
    const XMMATRIX spin = XMMatrixRotationY(angle);

    g_blackHawkPalette.resize(boneCount);
    for (size_t bone = 0; bone < boneCount; bone++) {
        const XMMATRIX inverseBind = XMLoadFloat4x4(&skeleton.offset[bone]);

        // Bind global straight from the inverse-bind matrix rather than by
        // accumulating local transforms down the hierarchy. The exporter hangs
        // the mast offset on the Armature node, which is not a joint, so the
        // bone's own local transform is identity and walking the joint parents
        // would place the hub at the origin - which is exactly what slid the
        // disc back over the tail boom.
        XMVECTOR determinant = XMMatrixDeterminant(inverseBind);
        if (XMVectorGetX(XMVectorAbs(determinant)) < 1e-12f) {
            // Identity is its own transpose; stored the same way as below so
            // the upload convention reads uniformly.
            XMStoreFloat4x4(&g_blackHawkPalette[bone],
                            XMMatrixTranspose(XMMatrixIdentity()));
            continue;
        }
        const XMMATRIX bindGlobal = XMMatrixInverse(&determinant, inverseBind);

        // Only the rotor bone moves. inverseBind puts the vertex in the bone's
        // frame, the spin turns it about that frame's own origin (the hub), and
        // bindGlobal puts it back into mesh space.
        const XMMATRIX posed = ((int)bone == g_blackHawkRotorBone)
            ? inverseBind * spin * bindGlobal
            : XMMatrixIdentity();

        // Transposed on upload: every skinning shader multiplies as
        // mul(vector, matrix), so the palette has to arrive column-major. Same
        // convention the skinned enemy path uses (SkinnedEnemy.h). Skipping it
        // makes the shader apply the inverse rotation and swing the hub itself
        // around the model origin.
        XMStoreFloat4x4(&g_blackHawkPalette[bone], XMMatrixTranspose(posed));
    }
}

// True when any primitive in the model actually carries skin data. A GLB can
// declare a skin and joints while no mesh references it -- the NewBlackHawk does
// exactly that -- and in that case the bone palette has nothing to move, so the
// caller must drive the rotor by node instead. Checks the geometry rather than
// the skin list, which is what tells the two apart.
static bool ModelHasSkinnedPrimitive(const std::shared_ptr<SceneNode>& node) {
    if (!node) return false;
    if (node->mesh)
        for (const MeshPrimitive& primitive : node->mesh->primitives)
            if (primitive.skinVertexCount > 0) return true;
    for (const auto& child : node->children)
        if (ModelHasSkinnedPrimitive(child)) return true;
    return false;
}

// Points the insertion helicopter at one of the loaded airframes and rederives
// everything measured from its geometry.
//
// The re-derivation is the whole reason this is a function rather than a
// pointer assignment. Bounds, the ride point the player sits on, and the rope
// anchor are all measured FROM the model at load time and cached in globals; a
// bare swap would leave the new airframe flying with the old one's dimensions,
// seating the player in mid-air beside it.
//
// Falls back to the BlackHawk when the requested airframe is not loaded, so a
// missing optional asset costs the choice rather than the insertion.
static void ApplyInsertionAirframe(InsertionAirframe airframe) {
    int slot = static_cast<int>(airframe);
    if (slot < 0 || slot > 1 || !g_blackHawkAirframeModel[slot]) slot = 0;
    g_insertionAirframe = static_cast<InsertionAirframe>(slot);

    g_blackHawkModel = g_blackHawkAirframeModel[slot];
    g_blackHawkSkeleton = g_blackHawkAirframeSkeleton[slot];
    if (!g_blackHawkModel) return;

    // Two ways to turn a rotor, picked per airframe by what the asset rigged.
    //
    // The UH-60 skins its disc to a joint, so it goes through the palette. The
    // NewBlackHawk ships a skin whose joint is attached to nothing (its meshes
    // declare no skin and carry no joint/weight attributes), so asking the
    // palette to spin it would move no vertices; its disc is instead a whole
    // mesh under a 'Bone' node, turned by rotating that node.
    //
    // Resolved by looking for real skin data rather than by airframe index, so
    // a re-export that adds or drops skinning picks the matching path.
    g_newBlackHawkRotorNode.reset();
    g_blackHawkRotorBone = -1;
    g_blackHawkPalette.clear();
    if (g_blackHawkSkeleton.BoneCount() > 0 && ModelHasSkinnedPrimitive(g_blackHawkModel)) {
        g_blackHawkRotorBone = g_blackHawkSkeleton.Find("Bone");
        if (g_blackHawkRotorBone >= 0) UpdateBlackHawkPalette();
    } else {
        // Rotating the node the disc hangs from. The importer keeps glTF node
        // names, so 'Bone' is the authored joint node with the rotor mesh as
        // its child.
        g_newBlackHawkRotorNode = FindNodeByName(g_blackHawkModel, "Bone");
        if (g_newBlackHawkRotorNode)
            std::cout << "Insertion airframe rotor node ready\n";
    }

    ConfigureBlackHawkBounds();
    ConfigureBlackHawkRideFromModel();
    // After the ride point, which it falls back to when the model carries no
    // dedicated rope anchor.
    ConfigureBlackHawkRopeAnchorFromModel();
}

// True while a second airframe is actually available to pick.
static bool InsertionAirframeChoiceAvailable() {
    return g_blackHawkAirframeModel[0] && g_blackHawkAirframeModel[1];
}

XMMATRIX BoatWorldMatrix() {
    return XMMatrixTranslation(-g_boatModelCenter.x, -g_boatModelMinY,
                               -g_boatModelCenter.z) *
           XMMatrixScaling(g_boatModelScale, g_boatModelScale, g_boatModelScale) *
           XMMatrixRotationZ(g_boatRoll) *
           XMMatrixRotationY(g_boatYaw) *
           XMMatrixTranslation(g_boatPosition.x,
                               g_boatPosition.y - g_boatSinkDepth,
                               g_boatPosition.z);
}

static XMFLOAT3 BoatTurretMountWorld() {
    // Stand the gunner on the deck, offset toward the stern seat rather than
    // the bow tip.
    //
    // This was -0.65 m, which is not a seat well: the normalized keel sits at
    // -0.35 m, so that put the gunner 0.30 m underneath the hull and left him
    // visibly half-sunk through the boat. Use the same kBoatDeckOffset that
    // BoatDeckY and the deck collision use, so the gunner stands on exactly the
    // surface the player walks on rather than on a second, disagreeing height.
    const float sx = std::sin(g_boatYaw), cz = std::cos(g_boatYaw);
    return { g_boatPosition.x + sx * 0.4f,
             g_boatPosition.y - g_boatSinkDepth + kBoatDeckOffset,
             g_boatPosition.z + cz * 0.4f };
}

static void ConfigureBoatBounds() {
    if (!g_boatModel || !g_boatModel->mesh) return;
    XMFLOAT3 minimum(FLT_MAX, FLT_MAX, FLT_MAX);
    XMFLOAT3 maximum(-FLT_MAX, -FLT_MAX, -FLT_MAX);
    for (const MeshPrimitive& primitive : g_boatModel->mesh->primitives) {
        for (size_t vertex = 0; vertex + 11 < primitive.vertices.size(); vertex += 12) {
            const float x = primitive.vertices[vertex];
            const float y = primitive.vertices[vertex + 1];
            const float z = primitive.vertices[vertex + 2];
            minimum.x = (std::min)(minimum.x, x);
            minimum.y = (std::min)(minimum.y, y);
            minimum.z = (std::min)(minimum.z, z);
            maximum.x = (std::max)(maximum.x, x);
            maximum.y = (std::max)(maximum.y, y);
            maximum.z = (std::max)(maximum.z, z);
        }
    }
    const float horizontalLength = (std::max)(
        maximum.x - minimum.x, maximum.z - minimum.z);
    if (horizontalLength <= 0.001f) return;
    g_boatModelCenter = {
        (minimum.x + maximum.x) * 0.5f,
        (minimum.y + maximum.y) * 0.5f,
        (minimum.z + maximum.z) * 0.5f };
    g_boatModelMinY = minimum.y;
    // Waterline sits a little above the hull bottom so it reads as floating,
    // not resting on the surface.
    g_boatModelScale = 9.0f / horizontalLength;
    g_boatModelMinY += 0.35f / g_boatModelScale;
}

XMMATRIX InsertionBoatWorldMatrix() {
    VehicleSystem& vehicles = g_game.vehicles;
    return XMMatrixTranslation(-vehicles.insertionBoatModelCenter.x,
                               -vehicles.insertionBoatModelMinY,
                               -vehicles.insertionBoatModelCenter.z) *
           XMMatrixScaling(vehicles.insertionBoatModelScale,
                           vehicles.insertionBoatModelScale,
                           vehicles.insertionBoatModelScale) *
           XMMatrixRotationZ(vehicles.insertionBoatRoll) *
           XMMatrixRotationY(vehicles.insertionBoatYaw) *
           XMMatrixTranslation(
               vehicles.insertionBoatPosition.x,
               vehicles.insertionBoatPosition.y - vehicles.insertionBoatSinkOffset,
               vehicles.insertionBoatPosition.z);
}

bool InsertionBoatVisible() { return g_game.vehicles.insertionBoatVisible; }

// Escape boat. Reuses the insertion boat's mesh and its model-fit constants --
// it is the same craft, waiting offshore rather than running the player in.
XMMATRIX EscapeBoatWorldMatrix() {
    const VehicleSystem& vehicles = g_game.vehicles;
    return XMMatrixTranslation(-vehicles.insertionBoatModelCenter.x,
                               -vehicles.insertionBoatModelMinY,
                               -vehicles.insertionBoatModelCenter.z) *
           XMMatrixScaling(vehicles.insertionBoatModelScale,
                           vehicles.insertionBoatModelScale,
                           vehicles.insertionBoatModelScale) *
           XMMatrixRotationY(vehicles.escapeBoatYaw) *
           XMMatrixTranslation(
               vehicles.escapeBoatPosition.x,
               vehicles.escapeBoatPosition.y + vehicles.EscapeBoatBobOffset(),
               vehicles.escapeBoatPosition.z);
}

bool EscapeBoatVisible() { return g_game.vehicles.EscapeBoatReady(); }

XMFLOAT3 InsertionBoatRideWorldPosition() {
    return g_game.vehicles.InsertionBoatRidePosition();
}

// Shares the patrol boat's normalization so both hulls come out the same size.
static void ConfigureInsertionBoatBounds() {
    if (!g_insertionBoatModel || !g_insertionBoatModel->mesh) return;
    XMFLOAT3 minimum(FLT_MAX, FLT_MAX, FLT_MAX);
    XMFLOAT3 maximum(-FLT_MAX, -FLT_MAX, -FLT_MAX);
    for (const MeshPrimitive& primitive : g_insertionBoatModel->mesh->primitives) {
        for (size_t vertex = 0; vertex + 11 < primitive.vertices.size(); vertex += 12) {
            const float x = primitive.vertices[vertex];
            const float y = primitive.vertices[vertex + 1];
            const float z = primitive.vertices[vertex + 2];
            minimum.x = (std::min)(minimum.x, x);
            minimum.y = (std::min)(minimum.y, y);
            minimum.z = (std::min)(minimum.z, z);
            maximum.x = (std::max)(maximum.x, x);
            maximum.y = (std::max)(maximum.y, y);
            maximum.z = (std::max)(maximum.z, z);
        }
    }
    const float horizontalLength = (std::max)(
        maximum.x - minimum.x, maximum.z - minimum.z);
    if (horizontalLength <= 0.001f) return;
    VehicleSystem& vehicles = g_game.vehicles;
    vehicles.insertionBoatModelCenter = {
        (minimum.x + maximum.x) * 0.5f,
        (minimum.y + maximum.y) * 0.5f,
        (minimum.z + maximum.z) * 0.5f };
    vehicles.insertionBoatModelMinY = minimum.y;
    vehicles.insertionBoatModelScale = 9.0f / horizontalLength;
    // Waterline a little above the hull bottom, so it floats rather than rests.
    vehicles.insertionBoatModelMinY += 0.35f / vehicles.insertionBoatModelScale;
    // Stand the passenger on the deck of the normalized hull, a little aft of
    // centre so they are not perched on the bow.
    vehicles.insertionBoatRideForward =
        -(maximum.z - minimum.z) * vehicles.insertionBoatModelScale * 0.18f;
    vehicles.insertionBoatRideHeight = 0.9f;
}

// The humvee ships its base colour map embedded in the source FBX. Where that
// texture resolved, leave the material alone -- tinting baseColorFactor would
// multiply into the sampled texel and clearing srvHeapSlot would unbind it.
// Materials with no texture (and the authored wheel material's black) keep the
// flat dark green so an export without the map still reads as a vehicle.
static bool ApplyDarkGreenToHumvee() {
    if (!g_humveeModel) return false;
    bool changed = false;
    std::function<void(const std::shared_ptr<SceneNode>&)> apply;
    apply = [&](const std::shared_ptr<SceneNode>& node) {
        if (!node) return;
        if (node->mesh) {
            for (MeshPrimitive& primitive : node->mesh->primitives) {
                if (!primitive.material) continue;
                if (primitive.material->name == "HumveeWheel") continue;
                if (primitive.material->baseColorTexture) continue;
                primitive.material->baseColorFactor.x = 0.18f;
                primitive.material->baseColorFactor.y = 0.30f;
                primitive.material->baseColorFactor.z = 0.12f;
                primitive.material->InvalidateTextureBindings();
                changed = true;
            }
        }
        for (const auto& child : node->children) apply(child);
    };
    apply(g_humveeModel);
    return changed;
}
