#pragma once

// Private application implementation; included once by main.cpp in dependency order.

static std::shared_ptr<SceneNode> CreateDDGICornellBoxModel() {
    auto root = std::make_shared<SceneNode>("DDGI Cornell Box");
    const auto material = [](const char* name, const XMFLOAT3& color) {
        auto value = std::make_shared<SceneMaterial>();
        value->name = name;
        value->baseColorFactor = XMFLOAT4(color.x, color.y, color.z, 1.0f);
        value->metallicFactor = 0.0f;
        value->roughnessFactor = 0.88f;
        // Test geometry must remain correct in forward, visibility, and mesh
        // shader paths even when their front-face conventions differ.
        value->doubleSided = true;
        value->disableOcclusionCulling = true;
        return value;
    };
    const auto white = material("Cornell White", { 0.78f, 0.78f, 0.74f });
    const auto red = material("Cornell Red", { 0.72f, 0.055f, 0.035f });
    const auto green = material("Cornell Green", { 0.045f, 0.50f, 0.085f });
    const auto light = material("Ceiling Light", { 5.0f, 3.9f, 2.6f });
    light->roughnessFactor = 0.35f;

    const auto addBox = [&](const char* name,
                            const std::shared_ptr<SceneMaterial>& boxMaterial,
                            float x0, float x1, float y0, float y1,
                            float z0, float z1) {
        auto node = std::make_shared<SceneNode>(name);
        node->mesh = std::make_shared<SceneMesh>();
        MeshPrimitive primitive;
        primitive.material = boxMaterial;
        const auto quad = [&](const XMFLOAT3& a, const XMFLOAT3& b,
                              const XMFLOAT3& c, const XMFLOAT3& d,
                              const XMFLOAT3& normal,
                              const XMFLOAT3& tangent) {
            const UINT base = static_cast<UINT>(primitive.vertices.size() / 12u);
            const XMFLOAT3 points[4] = { a, b, c, d };
            constexpr float uv[4][2] = { {0,0}, {1,0}, {1,1}, {0,1} };
            for (UINT i = 0; i < 4; ++i) {
                const float vertex[12] = {
                    points[i].x, points[i].y, points[i].z,
                    normal.x, normal.y, normal.z, uv[i][0], uv[i][1],
                    tangent.x, tangent.y, tangent.z, 1.0f
                };
                primitive.vertices.insert(
                    primitive.vertices.end(), vertex, vertex + 12);
            }
            primitive.indices.insert(primitive.indices.end(),
                { base, base + 1, base + 2, base, base + 2, base + 3 });
        };
        quad({x0,y0,z1},{x1,y0,z1},{x1,y1,z1},{x0,y1,z1},
             {0,0,1}, {1,0,0});
        quad({x1,y0,z0},{x0,y0,z0},{x0,y1,z0},{x1,y1,z0},
             {0,0,-1}, {-1,0,0});
        quad({x0,y0,z0},{x0,y0,z1},{x0,y1,z1},{x0,y1,z0},
             {-1,0,0}, {0,0,1});
        quad({x1,y0,z1},{x1,y0,z0},{x1,y1,z0},{x1,y1,z1},
             {1,0,0}, {0,0,-1});
        quad({x0,y1,z1},{x1,y1,z1},{x1,y1,z0},{x0,y1,z0},
             {0,1,0}, {1,0,0});
        quad({x0,y0,z0},{x1,y0,z0},{x1,y0,z1},{x0,y0,z1},
             {0,-1,0}, {1,0,0});
        // Keep diagnostic geometry on conventional IA. It avoids front-face
        // convention differences between forward, visibility, and mesh paths.
        if (!GLBImporter::BuildMeshletData(
                primitive, g_dx12.device.Get(), false))
            return;
        node->mesh->primitives.push_back(std::move(primitive));
        root->AddChild(node);
    };

    // Open front faces +Z. Thin closed slabs keep raster and DXR winding robust.
    addBox("Floor", white, -4.0f, 4.0f, 0.02f, 0.14f, 0.0f, 8.0f);
    addBox("Ceiling", white, -4.0f, 4.0f, 7.0f, 7.12f, 0.0f, 8.0f);
    addBox("Back", white, -4.0f, 4.0f, 0.02f, 7.12f, 7.88f, 8.0f);
    addBox("Red Wall", red, -4.12f, -4.0f, 0.02f, 7.12f, 0.0f, 8.0f);
    addBox("Green Wall", green, 4.0f, 4.12f, 0.02f, 7.12f, 0.0f, 8.0f);
    addBox("Short Block", white, -2.7f, -0.25f, 0.14f, 2.5f, 3.8f, 6.2f);
    addBox("Tall Block", white, 0.65f, 2.85f, 0.14f, 4.4f, 4.7f, 6.8f);
    addBox("Ceiling Light", light, -1.25f, 1.25f, 6.82f, 6.98f, 2.4f, 4.8f);

    XMFLOAT4X4 identity;
    XMStoreFloat4x4(&identity, XMMatrixIdentity());
    root->UpdateGlobalTransform(identity);
    return root;
}

// Boxes-and-barrels AA gun, built in code rather than imported: the shape is
// simple enough that a GLB would be dead weight, and building it here keeps the
// geometry in step with the firing constants above it.
//
// Two nodes, because they move differently: "Base" is the fixed plinth, "Gun" is
// the traversing mount. The caller rotates the Gun node by the turret's yaw and
// pitch each frame, so barrel geometry is authored pointing down +Z about the
// trunnion at the origin, and the mount height is applied by the caller's
// transform rather than baked into these coordinates.
static std::shared_ptr<SceneNode> CreateAATurretModel() {
    auto root = std::make_shared<SceneNode>("AA Turret");
    const auto material = [](const char* name, const XMFLOAT3& color,
                             float roughness, float metallic) {
        auto value = std::make_shared<SceneMaterial>();
        value->name = name;
        value->baseColorFactor = XMFLOAT4(color.x, color.y, color.z, 1.0f);
        value->metallicFactor = metallic;
        value->roughnessFactor = roughness;
        return value;
    };
    const auto olive = material("AA Olive", { 0.16f, 0.19f, 0.13f }, 0.80f, 0.15f);
    const auto steel = material("AA Steel", { 0.09f, 0.10f, 0.11f }, 0.42f, 0.85f);

    const auto addBox = [&](const std::shared_ptr<SceneNode>& parent,
                            const char* name,
                            const std::shared_ptr<SceneMaterial>& boxMaterial,
                            float x0, float x1, float y0, float y1,
                            float z0, float z1) {
        auto node = std::make_shared<SceneNode>(name);
        node->mesh = std::make_shared<SceneMesh>();
        MeshPrimitive primitive;
        primitive.material = boxMaterial;
        const auto quad = [&](const XMFLOAT3& a, const XMFLOAT3& b,
                              const XMFLOAT3& c, const XMFLOAT3& d,
                              const XMFLOAT3& normal, const XMFLOAT3& tangent) {
            const UINT base = static_cast<UINT>(primitive.vertices.size() / 12u);
            const XMFLOAT3 points[4] = { a, b, c, d };
            constexpr float uv[4][2] = { {0,0}, {1,0}, {1,1}, {0,1} };
            for (UINT i = 0; i < 4; ++i) {
                const float vertex[12] = {
                    points[i].x, points[i].y, points[i].z,
                    normal.x, normal.y, normal.z, uv[i][0], uv[i][1],
                    tangent.x, tangent.y, tangent.z, 1.0f
                };
                primitive.vertices.insert(
                    primitive.vertices.end(), vertex, vertex + 12);
            }
            primitive.indices.insert(primitive.indices.end(),
                { base, base + 1, base + 2, base, base + 2, base + 3 });
        };
        quad({x0,y0,z1},{x1,y0,z1},{x1,y1,z1},{x0,y1,z1}, {0,0,1}, {1,0,0});
        quad({x1,y0,z0},{x0,y0,z0},{x0,y1,z0},{x1,y1,z0}, {0,0,-1}, {-1,0,0});
        quad({x0,y0,z0},{x0,y0,z1},{x0,y1,z1},{x0,y1,z0}, {-1,0,0}, {0,0,1});
        quad({x1,y0,z1},{x1,y0,z0},{x1,y1,z0},{x1,y1,z1}, {1,0,0}, {0,0,-1});
        quad({x0,y1,z1},{x1,y1,z1},{x1,y1,z0},{x0,y1,z0}, {0,1,0}, {1,0,0});
        quad({x0,y0,z0},{x1,y0,z0},{x1,y0,z1},{x0,y0,z1}, {0,-1,0}, {1,0,0});
        if (!GLBImporter::BuildMeshletData(primitive, g_dx12.device.Get(), false))
            return;
        node->mesh->primitives.push_back(std::move(primitive));
        parent->AddChild(node);
    };

    auto base = std::make_shared<SceneNode>("Base");
    root->AddChild(base);
    // Splayed footing, then the pedestal the mount turns on.
    addBox(base, "Pad", olive, -1.65f, 1.65f, 0.0f, 0.22f, -1.65f, 1.65f);
    addBox(base, "Outrigger X", olive, -2.15f, 2.15f, 0.05f, 0.30f, -0.35f, 0.35f);
    addBox(base, "Outrigger Z", olive, -0.35f, 0.35f, 0.05f, 0.30f, -2.15f, 2.15f);
    addBox(base, "Pedestal", olive, -0.70f, 0.70f, 0.22f, 1.55f, -0.70f, 0.70f);
    // Ammo boxes flanking the pedestal, so the silhouette is not a bare column.
    addBox(base, "Ammo L", steel, -1.35f, -0.75f, 0.22f, 0.85f, -0.45f, 0.45f);
    addBox(base, "Ammo R", steel, 0.75f, 1.35f, 0.22f, 0.85f, -0.45f, 0.45f);

    auto gun = std::make_shared<SceneNode>("Gun");
    root->AddChild(gun);
    // Authored about the trunnion at the origin, pointing down +Z.
    addBox(gun, "Cradle", steel, -0.55f, 0.55f, -0.32f, 0.34f, -0.55f, 0.62f);
    addBox(gun, "Shield", olive, -0.95f, 0.95f, -0.30f, 0.78f, -0.72f, -0.55f);
    // Twin barrels.
    addBox(gun, "Barrel L", steel, -0.34f, -0.14f, -0.10f, 0.10f, 0.55f,
           VehicleSystem::AATurretBarrelLength);
    addBox(gun, "Barrel R", steel, 0.14f, 0.34f, -0.10f, 0.10f, 0.55f,
           VehicleSystem::AATurretBarrelLength);
    // Muzzle brakes, so the barrel ends read at distance.
    addBox(gun, "Brake L", steel, -0.40f, -0.08f, -0.16f, 0.16f,
           VehicleSystem::AATurretBarrelLength - 0.28f,
           VehicleSystem::AATurretBarrelLength);
    addBox(gun, "Brake R", steel, 0.08f, 0.40f, -0.16f, 0.16f,
           VehicleSystem::AATurretBarrelLength - 0.28f,
           VehicleSystem::AATurretBarrelLength);

    XMFLOAT4X4 identity;
    XMStoreFloat4x4(&identity, XMMatrixIdentity());
    root->UpdateGlobalTransform(identity);
    return root;
}

// Authored AA turret, replacing the box-built one above.
//
// PoseAATurretModelInstance drives the mount through two nodes under the root:
// "Gun", which traverses, and "Elevation" beneath it, which pitches the barrel
// alone -- the housing on this gun is ~3 m long, and pitching it with the
// barrel would swing the whole block into the air. The model arrives as three
// files cut by scripts/split-antiair.py, because the cook flattens every node
// into one mesh and a part found by name would not survive it:
//
//   AntiAir_Base       static, origin on the ground under the traverse axis
//   AntiAir_Traverse   origin at the trunnion, AATurretMountHeight up
//   AntiAir_Elevation  origin at the trunnion, barrel down +Z
//
// The split already scaled them to metres and put the trunnion at the mount
// height the firing code uses, so nothing is scaled here.
//
// Returns null if any part is missing, and the caller falls back to the box
// model rather than drawing nothing.
static std::shared_ptr<SceneNode> LoadAATurretPart(
        const std::filesystem::path& source) {
    std::string cookedError;
    std::shared_ptr<SceneNode> part = CookedAssetLoader::LoadForSource(
        source, g_dx12.device, g_dx12.commandList, &cookedError);
    if (part) return part;
    // The source GLB carries PNGs that upload as uncompressed RGBA8;
    // without the cook the gun costs roughly four times the VRAM.
    SGE_LOG("LogGameplay", EngineLog::Level::Warning,
        "AA turret part not cooked (" + source.generic_string() + "): " +
        cookedError + "; using source importer");
    return GLBImporter::LoadGLB(source.string(), g_dx12.device,
                                g_dx12.commandList);
}

static std::shared_ptr<SceneNode> LoadAATurretModel() {
    const std::shared_ptr<SceneNode> basePart =
        LoadAATurretPart("Content/Models/AntiAir/AntiAir_Base.glb");
    const std::shared_ptr<SceneNode> traversePart =
        LoadAATurretPart("Content/Models/AntiAir/AntiAir_Traverse.glb");
    const std::shared_ptr<SceneNode> elevationPart =
        LoadAATurretPart("Content/Models/AntiAir/AntiAir_Elevation.glb");
    if (!basePart || !traversePart || !elevationPart) {
        SGE_LOG("LogGameplay", EngineLog::Level::Warning,
            "AA turret model incomplete, using the built-in box turret");
        // Parts that did load have already recorded uploads on the open
        // command list; keep them alive until the cold-load fence drains.
        for (const std::shared_ptr<SceneNode>& part :
             { basePart, traversePart, elevationPart })
            if (part) g_rejectedUploadModels.push_back(part);
        return nullptr;
    }

    auto root = std::make_shared<SceneNode>("AA Turret");
    auto base = std::make_shared<SceneNode>("Base");
    base->AddChild(basePart);
    root->AddChild(base);

    // PoseAATurretModelInstance writes the transforms of both of these every
    // frame; the parts hang underneath so the pose never overwrites them.
    auto gun = std::make_shared<SceneNode>("Gun");
    gun->AddChild(traversePart);
    auto elevation = std::make_shared<SceneNode>("Elevation");
    elevation->AddChild(elevationPart);
    gun->AddChild(elevation);
    root->AddChild(gun);

    XMFLOAT4X4 identity;
    XMStoreFloat4x4(&identity, XMMatrixIdentity());
    root->UpdateGlobalTransform(identity);
    return root;
}

static std::shared_ptr<SceneNode> g_aaTurretModel;
// One posed copy per emplacement. Each turret aims independently, and a single
// SceneNode can only hold one "Gun" transform, so sharing the model made every
// drawn barrel copy whichever turret was updated last.
//
// The clone duplicates node structure only -- mesh and material pointers are
// shared with the source -- so an extra turret costs a handful of small node
// allocations and no GPU memory at all.
static std::vector<std::shared_ptr<SceneNode>> g_aaTurretModelInstances;

// Aims one turret model. Defined next to the render path that drives it; the
// gameplay update no longer poses anything, because each drawn gun follows its
// own emplacement rather than a single shared attitude.
static void PoseAATurretModelInstance(const std::shared_ptr<SceneNode>& model,
                                      float pitch, float yaw) {
    if (!model) return;
    for (const std::shared_ptr<SceneNode>& child : model->children) {
        if (!child || child->name != "Gun") continue;
        // The authored gun elevates its barrel on its own node: the mount
        // takes the yaw and the barrel the pitch, both pivoting on the
        // trunnion. The box fallback has no such node and pitches the whole
        // mount, as below.
        const auto elevation = std::find_if(child->children.begin(),
            child->children.end(), [](const std::shared_ptr<SceneNode>& node) {
                return node && node->name == "Elevation";
            });
        if (elevation != child->children.end()) {
            XMStoreFloat4(&child->rotation,
                          XMQuaternionRotationRollPitchYaw(0.0f, yaw, 0.0f));
            child->translation = {
                0.0f, VehicleSystem::AATurretMountHeight, 0.0f };
            child->UpdateLocalTransform();
            XMStoreFloat4(&(*elevation)->rotation,
                          XMQuaternionRotationRollPitchYaw(-pitch, 0.0f, 0.0f));
            (*elevation)->UpdateLocalTransform();
            break;
        }
        // Pitch about X first (barrels elevate about the trunnion), then yaw
        // about Y (the whole mount traverses), then lift to the trunnion height.
        // Negated pitch: the model points down +Z, where a positive rotation
        // about X would depress the barrels rather than raise them.
        //
        // Written through the TRS fields rather than straight into
        // localTransform: UpdateGlobalTransform calls UpdateLocalTransform,
        // which rebuilds the matrix from translation/rotation/scale and discards
        // a directly written one.
        const XMVECTOR orientation =
            XMQuaternionRotationRollPitchYaw(-pitch, yaw, 0.0f);
        XMStoreFloat4(&child->rotation, orientation);
        child->translation = {
            0.0f, VehicleSystem::AATurretMountHeight, 0.0f };
        child->UpdateLocalTransform();
        break;
    }
    XMFLOAT4X4 identity;
    XMStoreFloat4x4(&identity, XMMatrixIdentity());
    model->UpdateGlobalTransform(identity);
}

static std::shared_ptr<SceneNode> CloneSceneNodeShallow(
        const std::shared_ptr<SceneNode>& source) {
    if (!source) return nullptr;
    auto copy = std::make_shared<SceneNode>(source->name);
    copy->translation = source->translation;
    copy->rotation = source->rotation;
    copy->scale = source->scale;
    copy->localTransform = source->localTransform;
    copy->globalTransform = source->globalTransform;
    // Shared on purpose: the geometry is identical for every emplacement and
    // duplicating it would upload the same vertex buffers again per turret.
    copy->mesh = source->mesh;
    for (const std::shared_ptr<SceneNode>& child : source->children)
        copy->AddChild(CloneSceneNodeShallow(child));
    return copy;
}
// Synthetic entity id for the emplacement. The gun is placed by gameplay rather
// than authored in the level, so it has no LevelEntity id of its own; this lets
// the render batch and the weapon hit path still refer to it. "AATURRET" packed
// into the same high-value space the Cornell box's marker uses, so it cannot
// collide with a real (small, sequential) level entity id.
static constexpr uint64_t kAATurretEntityId = 0x4141545552524554ull;

static void RebuildRuntimePrefabLights();
