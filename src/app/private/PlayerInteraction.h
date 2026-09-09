#pragma once

// Private application implementation; included once by main.cpp in dependency order.

static void UpdateWeaponPickups(float dt) {
    if (scene.weaponPickups.empty()) return;
    for (WeaponPickup& pickup : scene.weaponPickups) {
        if (!pickup.active || pickup.collected) continue;
        pickup.bobPhase += dt;
    }
}

// The pickup the player is standing close enough to take, or null. Nearest wins
// when two overlap, so the prompt and the E handler can never disagree about
// which one is being offered.
static WeaponPickup* NearbyWeaponPickup() {
    if (scene.weaponPickups.empty() || g_drivingHumvee) return nullptr;
    if (g_game.session.Screen() != GameScreen::Level1 && !IsEditorPlaying())
        return nullptr;

    const XMFLOAT3& camera = scene.camera.Position;
    WeaponPickup* best = nullptr;
    float bestDistanceSq = FLT_MAX;
    for (WeaponPickup& pickup : scene.weaponPickups) {
        if (!pickup.active || pickup.collected) continue;
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
    g_reloadAudio.Play(0.9f, 0.85f);
    return true;
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
// Which of the two carried slots a purchase racks into. Kept across opens so a
// player working through a shopping list does not reset to slot 1 every time.
static int g_armoryShopSlot = 0;

// The counter the player is standing at, or null. Nearest wins, so the prompt
// and the E handler can never disagree about which one is being offered.
static const PrefabArmoryShop* NearbyArmoryShop(size_t* outIndex = nullptr) {
    if (g_prefabArmoryShops.empty() || g_drivingHumvee) return nullptr;
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
    if (g_armoryShopOpen) return false;
    size_t index = 0;
    if (!NearbyArmoryShop(&index)) return false;
    g_armoryShopOpen = true;
    g_armoryShopIndex = index;
    g_armoryShopCursorReleased = false;
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

static void RenderArmoryShopPanel(HWND hwnd) {
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
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(560.0f, 620.0f), ImGuiCond_Always);
    ImGui::Begin("##armory_shop", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                 ImGuiWindowFlags_NoSavedSettings);

    ImGui::TextColored(UITheme::kAccent, "%s", shop.displayName.c_str());
    ImGui::SameLine();
    char balanceText[32];
    MoneySystem::Format(balanceText, sizeof(balanceText),
                        g_game.money.Balance());
    const float balanceWidth = ImGui::CalcTextSize(balanceText).x;
    ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - balanceWidth);
    ImGui::TextColored(UITheme::kWarning, "%s", balanceText);
    ImGui::Separator();

    // Which carried slot a purchase racks into. Same idea as the deploy
    // screen's slot tabs: a row cannot know whether the player means it as a
    // primary or a secondary, so the slot is a mode set first.
    auto& carried = GunModel::LoadoutWeapons();
    for (int slot = 0; slot < 2; ++slot) {
        const bool active = g_armoryShopSlot == slot;
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button, UITheme::kAccentDim);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, UITheme::kAccent);
        }
        char tabText[96];
        std::snprintf(tabText, sizeof(tabText), "SLOT %d: %s", slot + 1,
                      GunModel::WeaponName(carried[static_cast<size_t>(slot)]));
        if (ImGui::Button(tabText, ImVec2(258.0f, 0.0f)))
            g_armoryShopSlot = slot;
        if (active) ImGui::PopStyleColor(2);
        if (slot == 0) ImGui::SameLine();
    }
    const size_t slotIndex = static_cast<size_t>(g_armoryShopSlot);
    const size_t otherIndex = slotIndex == 0 ? 1 : 0;

    ImGui::BeginChild("##armory_stock", ImVec2(0.0f, 480.0f));
    ImGui::TextColored(UITheme::kTextDim, "SMALL ARMS");
    for (int weapon = 0; weapon < MissionLoadout::kWeaponCount; ++weapon) {
        // Same gate the deploy screen uses: hidden and debug weapons are not
        // stock unless the debug toggle put them there.
        if (!GunModel::WeaponLoaded(weapon)) continue;
        const int price = ArmoryCatalog::WeaponPrice(weapon);
        const bool owned = ArmoryWeaponOwned(weapon);
        const bool equipped = carried[slotIndex] == weapon;
        const char* note =
            (carried[otherIndex] == weapon && !equipped)
                ? "Racked in the other slot."
                : ArmoryCatalog::WeaponBlurb(weapon);
        if (DrawArmoryRow(GunModel::WeaponName(weapon), note, price, owned,
                          equipped, "Racked in this slot.")) {
            // Taking the weapon already in the other slot would leave the
            // player holding two of the same gun, which stalls the weapon
            // cycle between duplicates. Swap the two instead.
            if (carried[otherIndex] == weapon) {
                carried[otherIndex] = carried[slotIndex];
                carried[slotIndex] = weapon;
                GunModel::SelectedWeapon() = weapon;
                RecordBaseArmoryKit();
            } else if (owned || ArmoryPurchase(price)) {
                g_ownedWeapons |= (1u << static_cast<uint32_t>(weapon));
                carried[slotIndex] = weapon;
                GunModel::LoadoutRestricted() = true;
                GunModel::SelectedWeapon() = weapon;
                // A weapon actually bought here is issued full, the way the
                // deploy screen issues one. `owned` is the pre-purchase flag,
                // so this seeds a fresh magazine only on the trip that paid for
                // the weapon -- re-racking a rifle already carried keeps the
                // ammo it has rather than being a free reload.
                if (!owned)
                    scene.player.weapons.SetInstance(
                        scene.player.weapons.CreateInstance(weapon));
                // A reload in flight belonged to the weapon just racked out;
                // letting it finish would top up a gun no longer carried.
                scene.player.reloadTimer = 0.0f;
                scene.player.reloadingSlot = -1;
                g_reloadAudio.Play(0.9f, 0.85f);
                RecordBaseArmoryKit();
            }
        }

        // The parts that fit this weapon, indented under it. Listing them on
        // the rifle they belong to is what makes the table readable: the
        // alternative is one flat parts bin the player has to cross-reference.
        ImGui::Indent(18.0f);
        for (const SGE::AttachmentDefinition& attachment :
             scene.player.weapons.Attachments()) {
            if (!attachment.CompatibleWith(weapon)) continue;
            const int partPrice = ArmoryCatalog::AttachmentPrice(
                attachment.suppressesWeapon, attachment.providesRedDot,
                attachment.providesLaser);
            const bool partOwned = ArmoryAttachmentOwned(attachment.id);
            const bool installed = scene.player.weapons.AttachmentInstalled(
                weapon, attachment.id);
            const char* blurb =
                attachment.suppressesWeapon
                    ? "Quieter report, smaller flash, slightly less recoil."
                : attachment.providesRedDot
                    ? "Clear red aiming point and a tighter sight picture."
                : attachment.providesLaser
                    ? "Visible designator and tighter hip-fire spread."
                    : "Fitted accessory.";
            // The id is unique per attachment but the display name repeats
            // across weapons, and DrawArmoryRow keys its ImGui id off the
            // name -- so scope the row to this weapon or every rifle's
            // suppressor row would share one id and one click state.
            ImGui::PushID(weapon);
            if (DrawArmoryRow(attachment.displayName.c_str(), blurb, partPrice,
                              partOwned, installed, "Fitted. Select to remove.")) {
                if (installed) {
                    // Removal is free and does not refund: the part is owned,
                    // and taking it off a rail is not selling it back.
                    scene.player.weapons.RemoveAttachment(weapon,
                                                          attachment.slot);
                    RecordBaseArmoryKit();
                } else if (partOwned || ArmoryPurchase(partPrice)) {
                    g_ownedAttachments.insert(attachment.id);
                    scene.player.weapons.EquipAttachment(weapon, attachment.id);
                    RecordBaseArmoryKit();
                }
            }
            ImGui::PopID();
        }
        ImGui::Unindent(18.0f);
        ImGui::Dummy(ImVec2(0.0f, 4.0f));
    }
    ImGui::EndChild();

    ImGui::Separator();
    if (ImGui::Button("LEAVE COUNTER  [E]", ImVec2(-1.0f, 32.0f)))
        CloseArmoryShop(hwnd);
    ImGui::End();
}

// ---- Island travel --------------------------------------------------------
// A boarding point flies the player to another map. The destinations are the
// same levels the main menu offers, resolved through the same candidate walk,
// so one list serves the menu and the helicopter and neither can drift from
// what actually ships.
//
// Each destination names a preview image. The art is optional on purpose: a
// missing file draws a labelled placeholder card instead of failing, so the
// screen works today and dropping a PNG into Content/Textures/Islands is the
// only step needed to illustrate it later.
struct TravelDestination {
    const char* name;
    const char* subtitle;
    // Searched in order, exactly like the main menu's level buttons: the repo
    // layout, the packaged flat levels/ copy, and a build/ run each resolve.
    std::array<const char*, 3> levelCandidates;
    const char* imagePath;
};

static const std::array<TravelDestination, 3> kTravelDestinations = { {
    { "ISLAND 1", "Campaign - hostile territory",
      { "Content/Levels/Islandv10.json",
        "levels/Islandv10.json",
        "build/Content/Levels/Islandv10.json" },
      "Content/Textures/Islands/island1.png" },
    { "TRAINING RANGE", "Live fire - no hostiles",
      { "Content/Levels/TrainingRange.json",
        "levels/TrainingRange.json",
        "build/Content/Levels/TrainingRange.json" },
      "Content/Textures/Islands/training_range.png" },
    { "HOME BASE", "Armory and staging",
      { "Content/Levels/Base.json",
        "levels/Base.json",
        "build/Content/Levels/Base.json" },
      "Content/Textures/Islands/base.png" },
} };


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
    if (g_prefabTravelPoints.empty() || g_drivingHumvee) return nullptr;
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
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(720.0f, 560.0f), ImGuiCond_Always);
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

    ImGui::BeginChild("##destinations", ImVec2(0.0f, -44.0f), false);
    // Three across at the panel's width. Cards keep a 16:9 image so a real
    // screenshot drops in without the layout shifting around it.
    constexpr float kCardWidth = 214.0f;
    constexpr float kImageHeight = kCardWidth * 9.0f / 16.0f;
    const TravelDestination* chosen = nullptr;
    for (size_t index = 0; index < kTravelDestinations.size(); ++index) {
        const TravelDestination& destination = kTravelDestinations[index];
        ImGui::PushID(static_cast<int>(index));
        ImGui::BeginGroup();

        const ImVec2 origin = ImGui::GetCursorScreenPos();
        // One invisible button spans image and caption, so the whole card is
        // the click target rather than just the words.
        const bool clicked = ImGui::InvisibleButton("##card",
            ImVec2(kCardWidth, kImageHeight + 46.0f));
        const bool hovered = ImGui::IsItemHovered();

        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 imageMin = origin;
        const ImVec2 imageMax(origin.x + kCardWidth, origin.y + kImageHeight);
        const uint64_t image = UITextureFromFile(destination.imagePath);
        if (image) {
            draw->AddImage((ImTextureID)(intptr_t)image, imageMin, imageMax);
        } else {
            // No art yet: a flat card carrying the name still tells the player
            // what they are choosing, which is the job the image would do.
            draw->AddRectFilled(imageMin, imageMax, IM_COL32(28, 38, 32, 255));
            const ImVec2 textSize = ImGui::CalcTextSize(destination.name);
            draw->AddText(ImVec2(
                imageMin.x + (kCardWidth - textSize.x) * 0.5f,
                imageMin.y + (kImageHeight - textSize.y) * 0.5f),
                IM_COL32(120, 140, 125, 255), destination.name);
        }
        draw->AddRect(imageMin, imageMax,
            hovered ? IM_COL32(38, 178, 82, 255) : IM_COL32(70, 78, 72, 255),
            0.0f, 0, hovered ? 2.0f : 1.0f);

        draw->AddText(ImVec2(origin.x, imageMax.y + 6.0f),
            hovered ? IM_COL32(120, 220, 150, 255)
                    : IM_COL32(230, 235, 230, 255), destination.name);
        draw->AddText(ImVec2(origin.x, imageMax.y + 24.0f),
            IM_COL32(131, 146, 135, 255), destination.subtitle);

        ImGui::EndGroup();
        ImGui::PopID();
        if (clicked) chosen = &destination;
        if (index + 1 < kTravelDestinations.size()) ImGui::SameLine(0.0f, 18.0f);
    }
    ImGui::EndChild();

    if (!g_travelStatus.empty())
        ImGui::TextColored(UITheme::kWarning, "%s", g_travelStatus.c_str());

    ImGui::Separator();
    if (ImGui::Button("STAY HERE  [E]", ImVec2(-1.0f, 32.0f)))
        CloseTravelScreen(hwnd);
    ImGui::End();

    // Departure happens after End(), never mid-window: loading a level rebuilds
    // the travel point list this function is reading, and StartCustomLevel also
    // takes the cursor back, which ImGui must not see inside an open window.
    if (chosen) TravelToDestination(hwnd, *chosen);
}
