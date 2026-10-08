#pragma once

// Private application implementation; included once by main.cpp in dependency order.

static void UpdateWeaponPickups(float dt) {
    if (scene.weaponPickups.empty()) return;
    for (WeaponPickup& pickup : scene.weaponPickups) {
        if (!pickup.active || pickup.collected) continue;
        pickup.bobPhase += dt;
        if (pickup.crate) {
            CreateSupplyCrateBody(pickup);
            if (pickup.physicsHandle == 0) continue;
            DestructionBodyPose pose;
            if (!g_destruction.GetPropBodyPose(pickup.physicsHandle, pose)) {
                // The world was rebuilt under it: remake it where it was.
                pickup.physicsHandle = 0;
                continue;
            }
            pickup.position = pose.position;
            pickup.rotation = pose.rotation;
            // Touchdown is the first time the canopy-speed fall is stopped.
            // The canopy is cut there, so from then on it tips, slides and
            // takes blasts as an ordinary loose box.
            const XMFLOAT3& v = pose.linearVelocity;
            if (!pickup.crateLanded &&
                v.x * v.x + v.y * v.y + v.z * v.z <
                    0.25f * kSupplyDropDescentRate * kSupplyDropDescentRate) {
                pickup.crateLanded = true;
                g_destruction.SetPropBodyLinearDamping(
                    pickup.physicsHandle, kSupplyCrateLandedDamping);
                char line[128];
                std::snprintf(line, sizeof(line),
                    "Supply crate landed: %s at (%.2f, %.2f, %.2f)",
                    GunModel::WeaponName(pickup.weapon.legacyWeaponId),
                    pickup.position.x, pickup.position.y, pickup.position.z);
                SGE_LOG("LogGameplay", EngineLog::Level::Display, line);
            }
            continue;
        }
        // Parachute descent: a steady rate rather than gravity, so the crate
        // can be watched in from the landing zone.
        if (pickup.dropHeight > 0.0f) {
            pickup.dropHeight = (std::max)(
                0.0f, pickup.dropHeight - kSupplyDropDescentRate * dt);
            if (pickup.dropHeight == 0.0f) {
                char line[128];
                std::snprintf(line, sizeof(line),
                    "Supply drop landed: %s at (%.1f, %.1f, %.1f)",
                    GunModel::WeaponName(pickup.weapon.legacyWeaponId),
                    pickup.position.x, pickup.position.y, pickup.position.z);
                SGE_LOG("LogGameplay", EngineLog::Level::Display, line);
            }
        }
    }
}

// Death hides the enemy's held gun; this puts the same mesh back into the world
// as a rigid body at the pose it last drew at, fitted with a box hull to the
// mesh's own bounds, so it falls out of his hand instead of vanishing.
static void SpawnDroppedEnemyGun(const SkinnedEnemy& enemy, uint32_t seed) {
    AmmoPickup pickup;
    pickup.life = pickup.maxLife;
    pickup.active = true;

    XMMATRIX gunWorld;
    XMFLOAT3 boundsMin = { FLT_MAX, FLT_MAX, FLT_MAX };
    XMFLOAT3 boundsMax = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
    if (GunModel::Loaded()) {
        for (const MeshPrimitive& prim : GunModel::Mesh()->primitives) {
            if (!prim.boundsValid) continue;
            boundsMin.x = (std::min)(boundsMin.x, prim.boundsMin.x);
            boundsMin.y = (std::min)(boundsMin.y, prim.boundsMin.y);
            boundsMin.z = (std::min)(boundsMin.z, prim.boundsMin.z);
            boundsMax.x = (std::max)(boundsMax.x, prim.boundsMax.x);
            boundsMax.y = (std::max)(boundsMax.y, prim.boundsMax.y);
            boundsMax.z = (std::max)(boundsMax.z, prim.boundsMax.z);
        }
    }
    XMVECTOR scale, rotation, translation;
    if (boundsMin.x <= boundsMax.x && enemy.LastGunWorldMatrix(gunWorld) &&
        XMMatrixDecompose(&scale, &rotation, &translation, gunWorld)) {
        // The gun transform is uniform scale * rotation * translation (see
        // SkinnedEnemy::UpdateGunFromHand*), so one scale factor covers it.
        const float s = XMVectorGetX(scale);
        const XMVECTOR centerLocal =
            (XMLoadFloat3(&boundsMin) + XMLoadFloat3(&boundsMax)) * 0.5f;
        XMFLOAT3 halfExtents;
        XMStoreFloat3(&halfExtents,
            (XMLoadFloat3(&boundsMax) - XMLoadFloat3(&boundsMin)) * (0.5f * s));
        // Box3D rejects a flat hull; a thin receiver still gets some depth.
        halfExtents.x = (std::max)(halfExtents.x, 0.015f);
        halfExtents.y = (std::max)(halfExtents.y, 0.015f);
        halfExtents.z = (std::max)(halfExtents.z, 0.015f);
        XMFLOAT3 bodyPosition;
        XMStoreFloat3(&bodyPosition, XMVector3Transform(centerLocal, gunWorld));
        XMFLOAT4 bodyRotation;
        XMStoreFloat4(&bodyRotation, rotation);

        // Slips out of the hand: a small forward flick plus a seeded sideways
        // push and tumble, so every drop is different but repeatable.
        const float side = ((seed >> 8) & 0xFF) / 255.0f * 2.0f - 1.0f;
        const float spin = ((seed >> 16) & 0xFF) / 255.0f * 2.0f - 1.0f;
        const float fwdX = std::sin(enemy.yaw), fwdZ = std::cos(enemy.yaw);
        const XMFLOAT3 linearVelocity = {
            fwdX * 0.8f + fwdZ * side * 0.6f, 1.0f,
            fwdZ * 0.8f - fwdX * side * 0.6f };
        const XMFLOAT3 angularVelocity = { spin * 4.0f, side * 2.0f, 3.0f };
        // A rifle is ~3.5 kg whatever its bounds measure.
        const float volume =
            8.0f * halfExtents.x * halfExtents.y * halfExtents.z;
        const float density = 3.5f / volume;

        pickup.physicsHandle = g_destruction.CreateDroppedItemBody(
            bodyPosition, bodyRotation, halfExtents,
            linearVelocity, angularVelocity, density);
        if (pickup.physicsHandle != 0) {
            pickup.position = bodyPosition;
            pickup.rotation = bodyRotation;
            pickup.meshScale = s;
            XMStoreFloat3(&pickup.meshCenterOffset, -centerLocal * s);
        }
    }
    if (pickup.physicsHandle == 0) {
        pickup.position = enemy.position;
        pickup.position.y += 0.3f;
    }
    scene.ammoPickups.push_back(pickup);
}

static void UpdateAmmoPickups(float dt) {
    scene.ammoPickupGainTimer = (std::max)(0.0f, scene.ammoPickupGainTimer - dt);
    if (scene.ammoPickups.empty()) return;
    for (auto it = scene.ammoPickups.begin(); it != scene.ammoPickups.end(); ) {
        it->life -= dt;
        if (!it->active || it->collected || it->life <= 0.0f) {
            g_destruction.DestroyPropBody(it->physicsHandle);
            it = scene.ammoPickups.erase(it);
            continue;
        }
        // A sleeping box keeps its last pose. A failed read (the world was
        // rebuilt under it) leaves it where it last landed.
        DestructionBodyPose pose;
        if (it->physicsHandle != 0 &&
            g_destruction.IsPropBodyAwake(it->physicsHandle) &&
            g_destruction.GetPropBodyPose(it->physicsHandle, pose)) {
            it->position = pose.position;
            it->rotation = pose.rotation;
        }
        it->bobPhase += dt;
        ++it;
    }
}

// The pickup the player is standing close enough to take, or null. Nearest wins
// when two overlap, so the prompt and the E handler can never disagree about
// which one is being offered.
static WeaponPickup* NearbyWeaponPickup() {
    if (scene.weaponPickups.empty() || PlayerInVehicle()) return nullptr;
    if (g_game.session.Screen() != GameScreen::Level1 && !IsEditorPlaying())
        return nullptr;

    const XMFLOAT3& camera = scene.camera.Position;
    WeaponPickup* best = nullptr;
    float bestDistanceSq = FLT_MAX;
    for (WeaponPickup& pickup : scene.weaponPickups) {
        if (!pickup.active || pickup.collected) continue;
        if (pickup.dropHeight > 0.0f) continue;   // still coming down
        if (pickup.crate && !pickup.crateLanded) continue;
        // The launcher model has to be loaded before the weapon can be selected:
        // GunModel::PlayerMesh() would otherwise fall through to the AK and the
        // player would hold the wrong gun while firing rockets.
        if (!GunModel::WeaponLoaded(pickup.weapon.legacyWeaponId)) continue;

        const float dx = camera.x - pickup.position.x;
        const float dz = camera.z - pickup.position.z;
        // Camera sits at eye height, so compare against the pickup's own Y with a
        // generous band rather than expecting the two to coincide.
        const float dy = camera.y - pickup.position.y;
        const float distanceSq = dx * dx + dz * dz;
        if (distanceSq > pickup.radius * pickup.radius) continue;
        if (std::abs(dy) > pickup.verticalRange) continue;
        if (distanceSq >= bestDistanceSq) continue;
        bestDistanceSq = distanceSq;
        best = &pickup;
    }
    return best;
}

// Takes the nearby pickup on E. Returns false when there was nothing to take,
// so the E handler can fall through to its other jobs.
//
// The swap is a true exchange: the weapon leaving the player's hands is left
// lying where the rocket was, with the ammo it still had. Nothing is destroyed,
// so a player who takes the rocket by mistake can walk back and trade it in.
static bool CollectNearbyWeaponPickup() {
    WeaponPickup* pickup = NearbyWeaponPickup();
    if (!pickup) return false;

    const int held = GunModel::SelectedWeapon();
    const int incoming = pickup->weapon.legacyWeaponId;
    if (held == incoming) return false;   // already holding it

    // C4 rides along outside the two chosen slots, so swapping into it would
    // silently delete a loadout weapon instead of the charge.
    auto& carried = GunModel::LoadoutWeapons();
    size_t target = 0;
    if (carried[1] == held) target = 1;
    else if (carried[0] != held) target = 0;
    // Never let the swap produce two identical slots -- CycleWeapon would then
    // stall between duplicates.
    const size_t other = target == 0 ? 1 : 0;
    if (carried[other] == incoming) return false;

    const int dropped = carried[target];
    PlayerState& player = scene.player;
    SGE::WeaponInstance* droppedInstance = player.Weapon(dropped);
    if (!droppedInstance || incoming < 0) return false;
    const SGE::WeaponInstance droppedSnapshot = *droppedInstance;
    if (!player.weapons.SetInstance(pickup->weapon)) return false;

    carried[target] = incoming;
    GunModel::SelectedWeapon() = incoming;
    // A reload in flight belonged to the weapon just swapped out; leaving it
    // running would top up the wrong slot when it completes.
    player.reloadTimer = 0.0f;
    player.reloadingSlot = -1;

    SGE_LOG("LogGameplay", EngineLog::Level::Display,
        std::string("Picked up ") + GunModel::WeaponName(incoming) +
        ", dropped " + GunModel::WeaponName(dropped));

    // The pickup becomes the weapon just given up, in place. Reusing the slot
    // rather than pushing a new one keeps the count stable across a run.
    pickup->weapon = droppedSnapshot;
    pickup->bobPhase = 0.0f;
    // The drop's HUD marker pointed at the ordered weapon; what is left here
    // now is the player's own cast-off.
    pickup->supplyDrop = false;
    // The charge rides along outside the two slots (see LoadoutAllows), so
    // an empty slot swapped out leaves nothing worth putting on the ground.
    if (dropped == GunModel::kRemoteChargeWeapon) pickup->active = false;
    g_reloadAudio.Play(0.9f, 0.85f);
    return true;
}

static AmmoPickup* NearbyAmmoPickup() {
    if (scene.ammoPickups.empty() || PlayerInVehicle()) return nullptr;
    if (g_game.session.Screen() != GameScreen::Level1 && !IsEditorPlaying())
        return nullptr;

    const XMFLOAT3& camera = scene.camera.Position;
    AmmoPickup* best = nullptr;
    float bestDistanceSq = FLT_MAX;
    for (AmmoPickup& pickup : scene.ammoPickups) {
        if (!pickup.active || pickup.collected) continue;

        const float dx = camera.x - pickup.position.x;
        const float dz = camera.z - pickup.position.z;
        const float dy = camera.y - pickup.position.y;
        const float distanceSq = dx * dx + dz * dz;
        if (distanceSq > pickup.radius * pickup.radius) continue;
        if (std::abs(dy) > pickup.verticalRange) continue;
        if (distanceSq >= bestDistanceSq) continue;
        bestDistanceSq = distanceSq;
        best = &pickup;
    }
    return best;
}

// Walk-over ammo: called every frame, takes the dropped gun the player is
// standing on and tops up both carried weapons. Returns false when there is
// nothing in reach or nothing to add -- a gun left while full stays on the
// ground to come back for.
static bool CollectNearbyAmmoPickup() {
    AmmoPickup* pickup = NearbyAmmoPickup();
    if (!pickup) return false;

    PlayerState& player = scene.player;
    auto& carried = GunModel::LoadoutWeapons();
    const int held = GunModel::SelectedWeapon();
    int ammoAdded = 0;
    int heldAdded = 0;

    for (size_t i = 0; i < carried.size(); ++i) {
        const int weaponId = carried[i];
        SGE::WeaponInstance* weapon = player.Weapon(weaponId);
        if (!weapon) continue;

        // One full magazine, clamped to what the weapon can carry.
        const int toAdd = (std::min)(player.MagazineSize(weaponId),
                                     player.MaxReserve(weaponId) - weapon->reserve);
        if (toAdd > 0) {
            weapon->reserve += toAdd;
            ammoAdded += toAdd;
            if (weaponId == held) heldAdded += toAdd;
        }
    }

    if (ammoAdded > 0) {
        pickup->collected = true;
        // The HUD shows the reserve of the weapon in hand, so the popup reports
        // that weapon's gain; only a held weapon already full falls back to
        // the total so the pickup never reads as "+0".
        scene.ammoPickupGain = heldAdded > 0 ? heldAdded : ammoAdded;
        scene.ammoPickupGainTimer = Scene::kAmmoPickupGainDuration;
        g_reloadAudio.Play(0.85f, 0.80f);
        return true;
    }
    return false;
}

// ---- Armory shop ----------------------------------------------------------
// A placed armory counter opens a storefront in the middle of a mission. It
// sells the same catalogue the deploy screen does and charges through the same
// ArmoryPurchase, so a rifle costs what it costs whether it was bought before
// the drop or halfway through one.
//
// Open state is a single index rather than a pointer: the shop list is rebuilt
// whenever a prefab is edited during a playtest, and a pointer into it would
// dangle the moment that happened.
static bool g_armoryShopOpen = false;
static size_t g_armoryShopIndex = 0;
static bool g_armoryShopCursorReleased = false;

// The counter the player is standing at, or null. Nearest wins, so the prompt
// and the E handler can never disagree about which one is being offered.
static const PrefabArmoryShop* NearbyArmoryShop(size_t* outIndex = nullptr) {
    if (g_prefabArmoryShops.empty() || PlayerInVehicle()) return nullptr;
    if (g_game.session.Screen() != GameScreen::Level1 && !IsEditorPlaying())
        return nullptr;

    const XMFLOAT3& camera = scene.camera.Position;
    const PrefabArmoryShop* best = nullptr;
    float bestDistanceSq = FLT_MAX;
    for (size_t index = 0; index < g_prefabArmoryShops.size(); ++index) {
        const PrefabArmoryShop& shop = g_prefabArmoryShops[index];
        const float dx = camera.x - shop.position.x;
        const float dz = camera.z - shop.position.z;
        const float dy = camera.y - shop.position.y;
        const float distanceSq = dx * dx + dz * dz;
        if (distanceSq > shop.radius * shop.radius) continue;
        if (std::abs(dy) > shop.verticalRange) continue;
        if (distanceSq >= bestDistanceSq) continue;
        bestDistanceSq = distanceSq;
        best = &shop;
        if (outIndex) *outIndex = index;
    }
    return best;
}

static void CloseArmoryShop(HWND hwnd) {
    if (!g_armoryShopOpen) return;
    g_armoryShopOpen = false;
    g_armoryShopCursorReleased = false;
    // A picker left open would reappear on the next counter or deploy screen.
    g_loadoutPickerSlot = -1;
    g_loadoutPickerFocus = -1;
    // Hand mouse-look back exactly the way the deployment screen does on DEPLOY,
    // or the player leaves the counter unable to turn.
    cameraLocked = false;
    SetCapture(hwnd);
    SetCursorVisible(false);
    ignoreNextMouseMove = true;
    firstMouse = true;
}

// Opens the counter the player is standing at on E. Returns false when there is
// none, so the E handler can fall through to its other jobs.
static bool OpenNearbyArmoryShop() {
    if (g_armoryShopOpen || g_game.loading.Active()) return false;
    size_t index = 0;
    if (!NearbyArmoryShop(&index)) return false;
    g_armoryShopOpen = true;
    g_armoryShopIndex = index;
    g_armoryShopCursorReleased = false;
    // A picker left open would reappear on the next counter or deploy screen.
    g_loadoutPickerSlot = -1;
    g_loadoutPickerFocus = -1;
    if (!firearmAssetsLoaded) {
        // The hub needs its counter and travel board before it needs the stock.
        // Reuse the loader's upload/finalization stages on the first visit.
        BeginLevelLoading(true);
        cameraLocked = true;
        ReleaseCapture();
        SetCursorVisible(true);
        g_armoryShopCursorReleased = true;
    }
    return true;
}

// "[E] TAKE <weapon>" over the pickup the player is standing at. Driven by the
// same NearbyWeaponPickup query the E handler uses, so the prompt appears
// exactly when the key will work.
static void DrawWeaponPickupPrompt(CXMMATRIX view, CXMMATRIX projection) {
    const WeaponPickup* pickup = NearbyWeaponPickup();
    if (!pickup) return;
    if (GunModel::SelectedWeapon() == pickup->weapon.legacyWeaponId) return;

    const XMFLOAT3 anchor{ pickup->position.x,
                           pickup->position.y + 0.55f,
                           pickup->position.z };
    const XMVECTOR clip = XMVector3Transform(
        XMLoadFloat3(&anchor), view * projection);
    const float w = XMVectorGetW(clip);
    if (w <= 0.01f) return;   // behind the camera

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const ImVec2 screen{
        (XMVectorGetX(clip) / w * 0.5f + 0.5f) * display.x,
        (1.0f - (XMVectorGetY(clip) / w * 0.5f + 0.5f)) * display.y };

    char label[96];
    std::snprintf(label, sizeof(label), "[E] TAKE %s",
                  GunModel::WeaponName(pickup->weapon.legacyWeaponId));
    const ImVec2 size = ImGui::CalcTextSize(label);
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    const ImU32 promptText = IM_COL32(255, 255, 255, 245);
    draw->AddRectFilled(
        ImVec2(screen.x - size.x * 0.5f - 6.0f, screen.y - 4.0f),
        ImVec2(screen.x + size.x * 0.5f + 6.0f, screen.y + 4.0f + size.y),
        IM_COL32(14, 12, 6, 185), 3.0f);
    draw->AddText(ImVec2(screen.x - size.x * 0.5f, screen.y), promptText, label);
}

// "[E] ARMORY" over the counter the player is standing at. Shares the same
// NearbyArmoryShop query the E handler uses, so the prompt appears exactly when
// the key will work. Suppressed while the shop is open -- the panel is already
// on screen and the prompt would sit behind it saying to open what is open.
static void DrawArmoryShopPrompt(CXMMATRIX view, CXMMATRIX projection) {
    if (g_armoryShopOpen) return;
    const PrefabArmoryShop* shop = NearbyArmoryShop();
    if (!shop) return;

    const XMFLOAT3 anchor{ shop->position.x,
                           shop->position.y + 1.35f,
                           shop->position.z };
    const XMVECTOR clip = XMVector3Transform(
        XMLoadFloat3(&anchor), view * projection);
    const float w = XMVectorGetW(clip);
    if (w <= 0.01f) return;   // behind the camera

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const ImVec2 screen{
        (XMVectorGetX(clip) / w * 0.5f + 0.5f) * display.x,
        (1.0f - (XMVectorGetY(clip) / w * 0.5f + 0.5f)) * display.y };

    char label[128];
    std::snprintf(label, sizeof(label), "[E] %s", shop->displayName.c_str());
    const ImVec2 size = ImGui::CalcTextSize(label);
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    const ImU32 promptText = IM_COL32(255, 255, 255, 245);
    draw->AddRectFilled(
        ImVec2(screen.x - size.x * 0.5f - 6.0f, screen.y - 4.0f),
        ImVec2(screen.x + size.x * 0.5f + 6.0f, screen.y + 4.0f + size.y),
        IM_COL32(14, 12, 6, 185), 3.0f);
    draw->AddText(ImVec2(screen.x - size.x * 0.5f, screen.y), promptText, label);
}

// The counter itself: every weapon on the table, and under each rail the
// attachments that fit it. Purchases go through ArmoryPurchase and the same
// rental masks the deploy screen uses, so a part bought here is a part the
// deploy screen already considers paid for on this mission.
// Banks what the counter just sold so it survives the flight out. Only the base
// does this: a counter placed in the middle of a mission is outfitting the run
// already underway, and that kit is spent when the run ends. Called after every
// change the panel makes rather than on leaving it, so walking away from the
// counter without pressing anything still keeps what was bought.
//
// The mission loadout is written here rather than left to GunModel alone: the
// deploy screen and the mission state read MissionLoadout, GunModel only holds
// what is in the player's hands, and a purchase that updates one but not the
// other arrives at the destination as a weapon the deploy screen never issued.
static void RecordBaseArmoryKit() {
    if (!g_baseMode) return;
    const auto& carried = GunModel::LoadoutWeapons();
    MissionLoadout& loadout = g_game.mission.Loadout();
    loadout.weapons = { carried[0], carried[1] };
    loadout.grenade = scene.selectedGrenade;
    g_baseKitPending = true;
    g_baseKitLoadout = loadout;
    g_baseKitWeapons = g_ownedWeapons;
    g_baseKitGrenades = g_ownedGrenades;
    g_baseKitGear = g_ownedGear;
    g_baseKitAttachments = g_ownedAttachments;
    // Snapshot the rails of the two weapons actually being carried. Parts on a
    // weapon left behind on the rack do not travel, so recording them would
    // re-fit a gun the player is not bringing.
    g_baseKitFitted.clear();
    for (size_t slot = 0; slot < MissionLoadout::kWeaponSlotCount; ++slot) {
        const int weapon = loadout.weapons[slot];
        for (const SGE::AttachmentDefinition& attachment :
             scene.player.weapons.Attachments()) {
            if (!scene.player.weapons.AttachmentInstalled(weapon,
                                                          attachment.id))
                continue;
            g_baseKitFitted.emplace_back(weapon, attachment.id);
        }
    }
}

// Test hook: SGE_AUTO_BASE_ARMORY=<slot> stands the player at the base counter
// once the base has loaded and opens it; slot 0-3 also opens that slot's
// picker, -1 leaves the card. SGE_ARMORY_CAPTURE_PATH=<file.ppm> then writes
// the frame and quits, as SGE_UI_CAPTURE_PATH does for the deploy screen.
static void RunBaseArmoryAutoHook() {
    static int state = 0;
    char value[16] = {};
    if (!g_baseMode || g_game.loading.Active() ||
        g_prefabArmoryShops.empty() ||
        GetEnvironmentVariableA("SGE_AUTO_BASE_ARMORY", value, sizeof(value)) == 0)
        return;
    // Pinned every frame, so settling physics cannot walk the camera out of
    // the counter's reach and close the panel before the capture.
    const PrefabArmoryShop& shop = g_prefabArmoryShops.front();
    scene.camera.Position = { shop.position.x,
                              shop.position.y + scene.camera.PlayerHeight,
                              shop.position.z };
    if (state == 0 && OpenNearbyArmoryShop()) state = 1;
    if (state == 1 && g_armoryShopOpen && !g_game.loading.Active()) {
        g_loadoutPickerSlot = (std::clamp)(std::atoi(value), -1, 3);
        g_loadoutPickerFocus = -1;
        state = 2;
    }
}

static void RenderArmoryShopPanel(HWND hwnd) {
    RunBaseArmoryAutoHook();
    if (g_armoryShopOpen) {
        static UICaptureHook captureHook;
        RunUICaptureHook(captureHook, "SGE_ARMORY_CAPTURE_PATH");
    }
    if (!g_armoryShopOpen) return;
    // A shop whose prefab was deleted mid-playtest, or a player who walked away
    // while it was open. Either way the counter is gone and the panel closes
    // rather than selling from a position the player is no longer standing at.
    if (g_armoryShopIndex >= g_prefabArmoryShops.size() || !NearbyArmoryShop()) {
        CloseArmoryShop(hwnd);
        return;
    }
    const PrefabArmoryShop& shop = g_prefabArmoryShops[g_armoryShopIndex];

    // Free the pointer the way the deployment screen does, so the rows can be
    // clicked. Mouse-look is handed back in CloseArmoryShop.
    if (!g_armoryShopCursorReleased) {
        cameraLocked = true;
        ReleaseCapture();
        SetCursorVisible(true);
        g_armoryShopCursorReleased = true;
    }

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    auto& carried = GunModel::LoadoutWeapons();

    // A slot opened from the card below: the same full-screen picker the
    // deploy screen uses, pointed at the kit the player is carrying.
    if (g_loadoutPickerSlot >= 0) {
        LoadoutPickerTarget target;
        target.weapons[0] = carried[0];
        target.weapons[1] = carried[1];
        target.mission = &g_game.mission.Loadout();
        target.top = 40.0f;
        target.equipWeapon = [&carried](int slot, int weapon, bool bought) {
            const size_t slotIndex = static_cast<size_t>(slot);
            const size_t otherIndex = slotIndex == 0 ? 1 : 0;
            // Taking the weapon already in the other slot would leave the
            // player holding two of the same gun, which stalls the weapon
            // cycle between duplicates. Swap the two instead.
            if (carried[otherIndex] == weapon) {
                carried[otherIndex] = carried[slotIndex];
                carried[slotIndex] = weapon;
                GunModel::SelectedWeapon() = weapon;
                RecordBaseArmoryKit();
                return;
            }
            carried[slotIndex] = weapon;
            GunModel::LoadoutRestricted() = true;
            GunModel::SelectedWeapon() = weapon;
            // A weapon actually bought here is issued full, the way the deploy
            // screen issues one; re-racking a rifle already owned keeps the
            // ammo it has rather than being a free reload.
            if (bought)
                scene.player.weapons.SetInstance(
                    scene.player.weapons.CreateInstance(weapon));
            // A reload in flight belonged to the weapon just racked out;
            // letting it finish would top up a gun no longer carried.
            scene.player.reloadTimer = 0.0f;
            scene.player.reloadingSlot = -1;
            g_reloadAudio.Play(0.9f, 0.85f);
            RecordBaseArmoryKit();
        };
        target.kitChanged = [] { RecordBaseArmoryKit(); };
        RenderLoadoutPicker(target, display);
        return;
    }

    // The counter itself: the loadout card from the deploy screen, on a
    // translucent plate. Clicking a weapon opens its picker above.
    constexpr float kCardWidth = 460.0f;
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(kCardWidth, 0.0f), ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.030f, 0.045f, 0.050f, 0.82f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.47f, 0.52f, 0.49f, 0.6f));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20.0f, 18.0f));
    ImGui::Begin("##armory_shop", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoSavedSettings |
                 ImGuiWindowFlags_AlwaysAutoResize);

    // Accent rule along the top edge, as on the deploy screen's plates.
    {
        const ImVec2 windowMin = ImGui::GetWindowPos();
        ImGui::GetWindowDrawList()->AddRectFilled(windowMin,
            ImVec2(windowMin.x + ImGui::GetWindowSize().x, windowMin.y + 3.0f),
            IM_COL32(120, 132, 124, 160));
    }
    ImGui::TextColored(UITheme::kTextDim, "QUARTERMASTER  //  %s",
                       shop.displayName.c_str());
    ImGui::TextColored(UITheme::kText, "LOADOUT");
    ImGui::SameLine();
    char balanceText[32];
    MoneySystem::Format(balanceText, sizeof(balanceText),
                        g_game.money.Balance());
    const float balanceWidth = ImGui::CalcTextSize(balanceText).x;
    ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - balanceWidth);
    ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "%s", balanceText);
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0.0f, 4.0f));

    const float width = kCardWidth - 40.0f;
    DrawLoadoutWeaponTile(0, carried[0], width);
    DrawLoadoutWeaponTile(1, carried[1], width);
    DrawLoadoutKitTiles(g_game.mission.Loadout(), width);
    ImGui::TextDisabled("Select a slot to change it or fit attachments.");
    // Per-weapon kick tuning is a development control, as on the deploy screen.
    if (g_deploymentDevTools) {
        DrawWeaponCameraShakeSlider(carried[0]);
        DrawWeaponCameraShakeSlider(carried[1]);
    }

    // Marines for the mission being outfitted. Only the base flies one out; a
    // counter mid-mission has no transport to load them on. Charged on DEPLOY
    // like the deploy screen's slider, which this pre-fills -- so it is capped
    // by what the wallet can cover now, and re-clamped there.
    if (g_baseMode) {
        ImGui::Dummy(ImVec2(0.0f, 6.0f));
        ImGui::TextColored(UITheme::kTextDim, "MARINE SQUAD");
        // No marine-mesh check here: the base defers combat assets, so the mesh
        // is not loaded yet. CommitDeployment drops the squad if it never loads.
        const int affordableMarines = (std::min)(kMaxDeploymentMarines,
            static_cast<int>(g_game.money.Balance() / kDeploymentMarinePrice));
        // Clamp before drawing: a purchase above can put the count out of reach.
        const int clamped = (std::min)(g_baseKitMarines, affordableMarines);
        if (clamped != g_baseKitMarines) {
            g_baseKitMarines = clamped;
            RecordBaseArmoryKit();
        }
        ImGui::BeginDisabled(affordableMarines <= 0);
        ImGui::SetNextItemWidth(width);
        if (ImGui::SliderInt("##BaseMarines", &g_baseKitMarines, 0,
                             (std::max)(1, affordableMarines),
                             g_baseKitMarines == 1 ? "%d marine"
                                                   : "%d marines")) {
            g_baseKitMarines = (std::clamp)(g_baseKitMarines, 0,
                                            (std::max)(0, affordableMarines));
            RecordBaseArmoryKit();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "Marines loaded aboard your transport, $2,000 each.\n"
                "Charged when you deploy. They only reach the ground\n"
                "if the transport does.");
        ImGui::EndDisabled();
        if (affordableMarines <= 0) {
            ImGui::TextColored(ImVec4(0.75f, 0.32f, 0.28f, 1.0f),
                               "Cannot afford a marine ($2,000 each)");
        } else if (g_baseKitMarines > 0) {
            char squadCost[32];
            MoneySystem::Format(squadCost, sizeof(squadCost),
                                g_baseKitMarines * kDeploymentMarinePrice);
            ImGui::TextColored(UITheme::kWarning, "%s on deploy", squadCost);
        }
    }

    ImGui::Dummy(ImVec2(0.0f, 6.0f));
    if (ImGui::Button("LEAVE COUNTER  [E]", ImVec2(width, 40.0f)))
        CloseArmoryShop(hwnd);
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
}

// ---- Island travel --------------------------------------------------------
// A boarding point flies the player to another map. The destinations are the
// same levels the main menu offers, resolved through the same candidate walk,
// so one list serves the menu and the helicopter and neither can drift from
// what actually ships.
//
// Each destination names a preview image. The art is optional on purpose: a
// missing file draws a labelled placeholder card instead of failing, so the
// screen works today and dropping a preview.png into the map's folder is the
// only step needed to illustrate it later.
// Open state mirrors the armory counter: an index rather than a pointer,
// because the travel point list is rebuilt whenever a prefab is edited during a
// playtest and a pointer into it would dangle the moment that happened.
static bool g_travelScreenOpen = false;
static size_t g_travelPointIndex = 0;
static bool g_travelCursorReleased = false;
static std::string g_travelStatus;
// True from the moment the player boards until the destination level takes
// over. Boarding is a real ride, not a menu over a parked aircraft: the bird
// lifts off the pad carrying them and the destination board is chosen in the
// air. Because the aircraft flies the player away from the pad, this also has
// to suspend the proximity checks that opened the screen -- they would close it
// the moment the helicopter cleared the boarding radius.
static bool g_travelAirborne = false;
// Where the flight started, so the player can be set back down there if they
// decide not to go. Banked because the pad is out of reach by then: the travel
// point list is rebuilt on prefab edits and the aircraft has moved kilometres.
static XMFLOAT3 g_travelReturnPosition{};

// The boarding point the player is standing at, or null. Nearest wins, so the
// prompt and the E handler can never disagree about which one is on offer.
static const PrefabTravelPoint* NearbyTravelPoint(size_t* outIndex = nullptr) {
    if (g_prefabTravelPoints.empty() || PlayerInVehicle()) return nullptr;
    if (g_game.session.Screen() != GameScreen::Level1 && !IsEditorPlaying())
        return nullptr;

    const XMFLOAT3& camera = scene.camera.Position;
    const PrefabTravelPoint* best = nullptr;
    float bestDistanceSq = FLT_MAX;
    for (size_t index = 0; index < g_prefabTravelPoints.size(); ++index) {
        const PrefabTravelPoint& point = g_prefabTravelPoints[index];
        const float dx = camera.x - point.position.x;
        const float dz = camera.z - point.position.z;
        const float dy = camera.y - point.position.y;
        const float distanceSq = dx * dx + dz * dz;
        if (distanceSq > point.radius * point.radius) continue;
        if (std::abs(dy) > point.verticalRange) continue;
        if (distanceSq >= bestDistanceSq) continue;
        bestDistanceSq = distanceSq;
        best = &point;
        if (outIndex) *outIndex = index;
    }
    return best;
}

// `depart` is true when the level is about to change under us: the flight is
// handing off to StartCustomLevel, which stands the aircraft down and places
// the player itself, so putting them back on the pad first would be undone a
// moment later. Every other close is the player backing out, and that has to
// set them down where they boarded.
static void CloseTravelScreen(HWND hwnd, bool depart = false) {
    if (!g_travelScreenOpen) return;
    g_travelScreenOpen = false;
    g_travelCursorReleased = false;
    g_travelStatus.clear();
    if (g_travelAirborne) {
        g_travelAirborne = false;
        if (!depart) {
            // Backed out: the ride is cancelled and the player is returned to
            // the pad rather than being released at altitude, which would drop
            // them to their death for changing their mind.
            g_game.vehicles.DisableBlackHawkInsertion();
            scene.camera.Position = g_travelReturnPosition;
            scene.camera.VerticalVelocity = 0.0f;
            scene.camera.IsGrounded = true;
            scene.camera.FloorY =
                g_travelReturnPosition.y - scene.camera.PlayerHeight;
            g_blackHawkCabinLocalValid = false;
        }
    }
    // Hand mouse-look back the way the armory counter does, or the player
    // steps away from the helicopter unable to turn.
    cameraLocked = false;
    SetCapture(hwnd);
    SetCursorVisible(false);
    ignoreNextMouseMove = true;
    firstMouse = true;
}

// Opens the boarding point the player is standing at on E. Returns false when
// there is none, so the E handler can fall through to its other jobs.
static bool OpenNearbyTravelScreen() {
    if (g_travelScreenOpen) return false;
    size_t index = 0;
    const PrefabTravelPoint* point = NearbyTravelPoint(&index);
    if (!point) return false;
    g_travelScreenOpen = true;
    g_travelPointIndex = index;
    g_travelCursorReleased = false;
    g_travelStatus.clear();

    // Boarding ends god mode. The base is where the player wanders and tinkers,
    // and god mode is part of that; getting into the aircraft is the point they
    // commit to a run, and a run flown invulnerable is not one. Done here at the
    // boarding press rather than at level load so the switch is visibly tied to
    // the act of getting in -- and so the deploy screen at the far end opens
    // showing it already off rather than silently flipping underneath them.
    if (scene.player.godMode) {
        scene.player.godMode = false;
        // God mode also disables ammo enforcement (PlayerState::AmmoEnforced),
        // so leaving it drops the player back onto magazines that were never
        // tracked. Restock the same way the deploy screen's toggle does.
        scene.player.RestoreAmmo();
    }

    // Get in and go. The player is strapped into the cabin and the aircraft
    // lifts off the pad it was parked on, so the destination board is picked
    // from the air rather than while standing next to a helicopter that never
    // moves. RidePlayerInBlackHawk already owns the camera for a carried
    // passenger, so pinning them is a matter of arming the ride, not of moving
    // the camera here.
    g_travelReturnPosition = scene.camera.Position;
    g_travelAirborne = true;
    // Model loading measures the authored right seat. Apply the chosen side
    // here too, so base boarding starts at the same door as an insertion.
    ApplyBlackHawkSeatSide();
    // Depart along the pad's own facing, which is the direction the aircraft is
    // modelled pointing -- so it flies out its nose instead of sliding sideways.
    g_game.vehicles.BeginBlackHawkDeparture(
        point->position, point->position.y, point->yawRadians);
    // The cabin walk seeds from the authored seat on the first frame aboard;
    // clearing it here means boarding at the base starts in the door rather
    // than wherever the last insertion left the player standing.
    g_blackHawkCabinLocalValid = false;
    return true;
}

// "[E] BOARD HELICOPTER" over the aircraft the player is standing at. Driven by
// the same NearbyTravelPoint query the E handler uses, so the prompt appears
// exactly when the key will work.
static void DrawTravelPrompt(CXMMATRIX view, CXMMATRIX projection) {
    if (g_travelScreenOpen) return;
    const PrefabTravelPoint* point = NearbyTravelPoint();
    if (!point) return;

    const XMFLOAT3 anchor{ point->position.x,
                           point->position.y + 2.2f,
                           point->position.z };
    const XMVECTOR clip = XMVector3Transform(
        XMLoadFloat3(&anchor), view * projection);
    const float w = XMVectorGetW(clip);
    if (w <= 0.01f) return;   // behind the camera

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    const ImVec2 screen{
        (XMVectorGetX(clip) / w * 0.5f + 0.5f) * display.x,
        (1.0f - (XMVectorGetY(clip) / w * 0.5f + 0.5f)) * display.y };

    char label[128];
    std::snprintf(label, sizeof(label), "[E] %s", point->displayName.c_str());
    const ImVec2 size = ImGui::CalcTextSize(label);
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    const ImU32 promptText = IM_COL32(255, 255, 255, 245);
    draw->AddRectFilled(
        ImVec2(screen.x - size.x * 0.5f - 6.0f, screen.y - 4.0f),
        ImVec2(screen.x + size.x * 0.5f + 6.0f, screen.y + 4.0f + size.y),
        IM_COL32(14, 12, 6, 185), 3.0f);
    draw->AddText(ImVec2(screen.x - size.x * 0.5f, screen.y), promptText, label);
}

// Flies to a destination: resolves the level the same way the menu buttons do
// and hands off to StartCustomLevel. The screen is closed first so mouse-look
// is already restored when the new level takes over the cursor.
static void TravelToDestination(HWND hwnd,
                                const TravelDestination& destination) {
    std::error_code error;
    for (const char* candidate : destination.levelCandidates) {
        if (!std::filesystem::exists(candidate, error)) continue;
        SGE_LOG("LogGameplay", EngineLog::Level::Display,
            std::string("Travel: departing for ") + destination.name +
            " (" + candidate + ")");
        CloseTravelScreen(hwnd, /*depart=*/true);
        // Not invulnerable: see StartCustomLevel. The player gave up god mode
        // when they climbed in, and the deploy screen at the far end has to
        // open showing it already off.
        StartCustomLevel(hwnd, std::filesystem::path(candidate),
                         /*godMode=*/false);
        return;
    }
    // Say which map is missing rather than failing silently, the same way the
    // menu buttons do: a card that does nothing when clicked leaves the player
    // with nothing to act on.
    g_travelStatus = std::string(destination.name) +
        " is unavailable (level file missing).";
}

// The destination board: one card per island, each with its preview image above
// the name. Cards are clickable in full, so the image is as much a target as
// the text under it.
static void RenderTravelPanel(HWND hwnd) {
    if (!g_travelScreenOpen) return;
    // A boarding point whose prefab was deleted mid-playtest, or a player who
    // walked away while the board was open. Either way the aircraft is gone and
    // the screen closes rather than flying from somewhere the player is not.
    // The proximity half of this check is skipped once airborne: the aircraft
    // has flown the player off the pad by design, so being far from it is the
    // expected state rather than a reason to close. The index check still
    // stands -- a deleted prefab leaves nothing to fly from either way.
    if (g_travelPointIndex >= g_prefabTravelPoints.size() ||
        (!g_travelAirborne && !NearbyTravelPoint())) {
        CloseTravelScreen(hwnd);
        return;
    }
    const PrefabTravelPoint& point = g_prefabTravelPoints[g_travelPointIndex];

    // Free the pointer the way the armory counter does, so the cards can be
    // clicked. Mouse-look is handed back in CloseTravelScreen.
    if (!g_travelCursorReleased) {
        cameraLocked = true;
        ReleaseCapture();
        SetCursorVisible(true);
        g_travelCursorReleased = true;
    }

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    // Sized off the screen rather than pinned at 720x560. The cards carry real
    // top-down captures of each map, and at the old fixed width the island read
    // as a green smudge 214 px across -- the tower, the huts and the drop
    // markers were all in the picture and none of them were legible. Clamped at
    // both ends: never wider than the screen less a margin, never so small on a
    // tiny window that the three-across row stops fitting.
    const float kPanelMargin = 48.0f;
    const float panelWidth = (std::max)(720.0f,
        (std::min)(display.x * 0.82f, display.x - kPanelMargin * 2.0f));
    const float panelHeight = (std::max)(560.0f,
        (std::min)(display.y * 0.86f, display.y - kPanelMargin * 2.0f));
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(panelWidth, panelHeight), ImGuiCond_Always);
    ImGui::Begin("##island_travel", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoSavedSettings);

    ImGui::TextColored(UITheme::kAccent, "%s", point.displayName.c_str());
    ImGui::SameLine();
    const char* subtitle = "SELECT DESTINATION";
    const float subtitleWidth = ImGui::CalcTextSize(subtitle).x;
    ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - subtitleWidth);
    ImGui::TextColored(UITheme::kTextDim, "%s", subtitle);
    ImGui::Separator();
    ImGui::Dummy(ImVec2(0.0f, 4.0f));

    const float kFooterHeight = 52.0f;
    ImGui::BeginChild("##destinations", ImVec2(0.0f, -kFooterHeight), false);
    // Three across, then wrap. The card width is whatever the panel leaves
    // rather than a constant, so widening the window widens the pictures
    // instead of parking them left with dead space on the right. Images stay
    // 16:9 so a straight screenshot drops in with no letterboxing.
    constexpr size_t kCardsPerRow = 3;
    constexpr float kCardGap = 20.0f;
    // Width comes from the panel, not from GetContentRegionAvail(). The live
    // region shrinks by the scrollbar when one appears, and the card size is
    // what decides whether the content is tall enough to need that scrollbar --
    // reading it back would let the two chase each other frame to frame on a
    // short window. Reserving the scrollbar unconditionally costs a few pixels
    // and settles it.
    const ImGuiStyle& style = ImGui::GetStyle();
    const float innerWidth = panelWidth - style.WindowPadding.x * 2.0f -
                             style.ScrollbarSize;
    const float cardWidth = std::floor(
        ((std::max)(1.0f, innerWidth) - kCardGap * (kCardsPerRow - 1)) /
        kCardsPerRow);
    const float imageHeight = std::floor(cardWidth * 9.0f / 16.0f);

    // Caption block, measured rather than assumed at 46 px. The subtitles are
    // sentences, not labels: at the old fixed width "Strike - aircraft on the
    // ground" ran straight across its neighbour's caption, because the text was
    // drawn unclipped from the card origin. Wrapping them to the card width
    // fixes the bleed but makes the block one or two lines deep depending on
    // the string, so every card takes the tallest of them and the row below
    // starts level.
    const float kNameGap = 8.0f;
    const float kSubtitleGap = 4.0f;
    const float lineHeight = ImGui::GetTextLineHeight();
    float captionHeight = 0.0f;
    for (const TravelDestination& destination : kTravelDestinations) {
        const float subtitleHeight = ImGui::CalcTextSize(
            destination.subtitle, nullptr, false, cardWidth).y;
        captionHeight = (std::max)(captionHeight,
            kNameGap + lineHeight + kSubtitleGap + subtitleHeight);
    }

    const TravelDestination* chosen = nullptr;
    for (size_t index = 0; index < kTravelDestinations.size(); ++index) {
        const TravelDestination& destination = kTravelDestinations[index];
        ImGui::PushID(static_cast<int>(index));
        ImGui::BeginGroup();

        const ImVec2 origin = ImGui::GetCursorScreenPos();
        // One invisible button spans image and caption, so the whole card is
        // the click target rather than just the words.
        const bool clicked = ImGui::InvisibleButton("##card",
            ImVec2(cardWidth, imageHeight + captionHeight));
        const bool hovered = ImGui::IsItemHovered();

        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 imageMin = origin;
        const ImVec2 imageMax(origin.x + cardWidth, origin.y + imageHeight);
        // Resolve once like the texture cache, including absent art. Both the
        // card and the load can find build/ art when launched from the repo.
        static const auto previewPaths = [] {
            std::array<std::string, kTravelDestinations.size()> paths;
            for (size_t i = 0; i < paths.size(); ++i)
                paths[i] = TravelPreviewImagePath(kTravelDestinations[i]);
            return paths;
        }();
        const uint64_t image = UITextureFromFile(previewPaths[index].c_str());
        if (image) {
            draw->AddImage((ImTextureID)(intptr_t)image, imageMin, imageMax);
        } else {
            // No art yet: a flat card carrying the name still tells the player
            // what they are choosing, which is the job the image would do.
            draw->AddRectFilled(imageMin, imageMax, IM_COL32(28, 38, 32, 255));
            const ImVec2 textSize = ImGui::CalcTextSize(destination.name);
            draw->AddText(ImVec2(
                imageMin.x + (cardWidth - textSize.x) * 0.5f,
                imageMin.y + (imageHeight - textSize.y) * 0.5f),
                IM_COL32(120, 140, 125, 255), destination.name);
        }
        draw->AddRect(imageMin, imageMax,
            hovered ? IM_COL32(38, 178, 82, 255) : IM_COL32(70, 78, 72, 255),
            0.0f, 0, hovered ? 2.0f : 1.0f);

        draw->AddText(ImVec2(origin.x, imageMax.y + kNameGap),
            hovered ? IM_COL32(120, 220, 150, 255)
                    : IM_COL32(230, 235, 230, 255), destination.name);
        // Wrapped at the card width: the overload taking a wrap width is the
        // one that keeps a long subtitle inside its own card.
        draw->AddText(ImGui::GetFont(), ImGui::GetFontSize(),
            ImVec2(origin.x, imageMax.y + kNameGap + lineHeight + kSubtitleGap),
            IM_COL32(131, 146, 135, 255), destination.subtitle, nullptr,
            cardWidth);

        ImGui::EndGroup();
        ImGui::PopID();
        if (clicked) chosen = &destination;
        if (index + 1 < kTravelDestinations.size() &&
            (index + 1) % kCardsPerRow != 0)
            ImGui::SameLine(0.0f, kCardGap);
        else if (index + 1 < kTravelDestinations.size())
            ImGui::Dummy(ImVec2(0.0f, 16.0f));
    }
    ImGui::EndChild();

    if (!g_travelStatus.empty())
        ImGui::TextColored(UITheme::kWarning, "%s", g_travelStatus.c_str());

    ImGui::Separator();
    if (ImGui::Button("STAY HERE  [E]", ImVec2(-1.0f, 36.0f)))
        CloseTravelScreen(hwnd);
    ImGui::End();

    // Departure happens after End(), never mid-window: loading a level rebuilds
    // the travel point list this function is reading, and StartCustomLevel also
    // takes the cursor back, which ImGui must not see inside an open window.
    if (chosen) TravelToDestination(hwnd, *chosen);
}
