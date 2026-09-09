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
// PoseAATurretModel drives the mount by finding a direct child of the root named
// "Gun" and writing its local transform, so the loaded hierarchy has to present
// that contract. The GLB's traverse node is called "Y-Rotation" and sits several
// levels down (Spaceship-Turrent > .001 > master > Y-Rotation), so this lifts it
// out and reparents it as "Gun" with the rest of the model kept as the static
// base. Nothing about the firing code changes.
//
// Returns null if anything is missing, and the caller falls back to the box
// model rather than drawing nothing.
static std::shared_ptr<SceneNode> LoadAATurretModel() {
    auto loaded = GLBImporter::LoadGLB("Content/Models/Turret/Turret.glb",
                                       g_dx12.device, g_dx12.commandList);
    if (!loaded) {
        SGE_LOG("LogGameplay", EngineLog::Level::Warning,
            "AA turret model missing, using the built-in box turret");
        return nullptr;
    }

    // Scale so the model's traverse ring lands at AATurretMountHeight. That
    // constant is not cosmetic -- the firing code uses it for the shell origin
    // and for the aim solution -- so matching it matters more than matching the
    // old model's footprint width, which ends up 4.9 m against the box turret's
    // 4.3 m and reads as a slightly heavier emplacement.
    constexpr float kAuthoredMountHeight = 1.16f;  // "Y-Rotation" node height
    constexpr float kModelScale =
        VehicleSystem::AATurretMountHeight / kAuthoredMountHeight;

    // Depth-first search for the traverse node.
    std::function<std::shared_ptr<SceneNode>(const std::shared_ptr<SceneNode>&)>
        findTraverse = [&](const std::shared_ptr<SceneNode>& node)
        -> std::shared_ptr<SceneNode> {
        if (!node) return nullptr;
        if (node->name == "Y-Rotation") return node;
        for (const std::shared_ptr<SceneNode>& child : node->children)
            if (auto found = findTraverse(child)) return found;
        return nullptr;
    };
    std::shared_ptr<SceneNode> traverse = findTraverse(loaded);
    if (!traverse) {
        SGE_LOG("LogGameplay", EngineLog::Level::Warning,
            "AA turret model has no Y-Rotation node, using the box turret");
        g_rejectedUploadModels.push_back(std::move(loaded));
        return nullptr;
    }

    // Detach the traverse subtree from wherever it sits, so it can be posed
    // independently of the base instead of inheriting the base's transform.
    std::function<bool(const std::shared_ptr<SceneNode>&)> detach =
        [&](const std::shared_ptr<SceneNode>& node) -> bool {
        if (!node) return false;
        auto found = std::find(node->children.begin(), node->children.end(),
                               traverse);
        if (found != node->children.end()) {
            node->children.erase(found);
            return true;
        }
        for (const std::shared_ptr<SceneNode>& child : node->children)
            if (detach(child)) return true;
        return false;
    };
    detach(loaded);

    auto root = std::make_shared<SceneNode>("AA Turret");

    // Barrel fallback.
    //
    // Earlier exports of this model had no barrel geometry: in Blender the
    // barrels come from an Array modifier on "Gun barrel 200 cal.016", and while
    // that node was an Empty the exporter wrote the node and nothing else. The
    // current asset has them baked, so this normally does nothing.
    //
    // Kept, and gated on the barrel node actually carrying a mesh, so a
    // re-export that loses them again leaves a usable gun rather than a mount
    // with no barrels -- and so the generated tubes can never double up on top
    // of real ones.
    bool hasAuthoredBarrels = false;
    {
        std::function<void(const std::shared_ptr<SceneNode>&)> findBarrels =
            [&](const std::shared_ptr<SceneNode>& node) {
            if (!node || hasAuthoredBarrels) return;
            // Match the authored barrel node by name, and require real geometry:
            // the node existing is not enough, which is exactly how the missing
            // barrels slipped through before.
            if (node->name.rfind("Gun barrel", 0) == 0 && node->mesh &&
                !node->mesh->primitives.empty()) {
                hasAuthoredBarrels = true;
                return;
            }
            for (const std::shared_ptr<SceneNode>& child : node->children)
                findBarrels(child);
        };
        findBarrels(loaded);
    }

    if (traverse && !hasAuthoredBarrels) {
        // Find "spin-ey": the rings hang off it, so building in its space puts
        // the barrels through them without recomputing the 120-degree rotation
        // that orients the whole assembly.
        std::function<std::shared_ptr<SceneNode>(const std::shared_ptr<SceneNode>&)>
            findSpin = [&](const std::shared_ptr<SceneNode>& node)
            -> std::shared_ptr<SceneNode> {
            if (!node) return nullptr;
            if (node->name == "spin-ey") return node;
            for (const std::shared_ptr<SceneNode>& child : node->children)
                if (auto found = findSpin(child)) return found;
            return nullptr;
        };

        if (std::shared_ptr<SceneNode> spin = findSpin(traverse)) {
            // Borrow the metal material off a ring so the barrels match the rest
            // of the gun instead of needing their own texture.
            std::shared_ptr<SceneMaterial> barrelMaterial;
            for (const std::shared_ptr<SceneNode>& child : spin->children) {
                if (!child || !child->mesh) continue;
                for (const MeshPrimitive& primitive : child->mesh->primitives)
                    if (primitive.material) { barrelMaterial = primitive.material; break; }
                if (barrelMaterial) break;
            }

            auto barrels = std::make_shared<SceneNode>("Barrels");
            barrels->mesh = std::make_shared<SceneMesh>();
            MeshPrimitive tubes;
            tubes.material = barrelMaterial;

            // Four tubes on the same 0.223 m circle the collars use, quartered
            // so each sits where a barrel passes through the ring.
            constexpr int kBarrelCount = 4;
            constexpr int kSides = 10;          // sides per tube
            constexpr float kRingRadius = 0.223f;
            constexpr float kBarrelRadius = 0.052f;
            constexpr float kStart = 0.35f;     // just inside the breech
            constexpr float kEnd = 2.15f;       // past the outermost collar
            for (int barrel = 0; barrel < kBarrelCount; ++barrel) {
                const float spin_ = XM_2PI * static_cast<float>(barrel) /
                                    kBarrelCount + XM_PIDIV4;
                const float cx = std::cos(spin_) * kRingRadius;
                const float cz = std::sin(spin_) * kRingRadius;
                const UINT base = static_cast<UINT>(tubes.vertices.size() / 12u);
                for (int side = 0; side <= kSides; ++side) {
                    const float angle = XM_2PI * static_cast<float>(side) / kSides;
                    const float nx = std::cos(angle);
                    const float nz = std::sin(angle);
                    // Local +Y is the forward axis in spin-ey space (verified
                    // against the collar spacing), so the tube runs along Y.
                    for (int end = 0; end < 2; ++end) {
                        const float y = end == 0 ? kStart : kEnd;
                        const float vertex[12] = {
                            cx + nx * kBarrelRadius, y, cz + nz * kBarrelRadius,
                            nx, 0.0f, nz,
                            static_cast<float>(side) / kSides,
                            static_cast<float>(end),
                            0.0f, 1.0f, 0.0f, 1.0f
                        };
                        tubes.vertices.insert(tubes.vertices.end(),
                                              vertex, vertex + 12);
                    }
                }
                for (int side = 0; side < kSides; ++side) {
                    const UINT a = base + side * 2u;
                    tubes.indices.insert(tubes.indices.end(),
                        { a, a + 1u, a + 2u, a + 1u, a + 3u, a + 2u });
                }
            }
            // Same finalisation the box turret's boxes go through: without the
            // meshlet/GPU data the primitive carries vertices but never draws.
            if (GLBImporter::BuildMeshletData(tubes, g_dx12.device.Get(), false)) {
                barrels->mesh->primitives.push_back(std::move(tubes));
                spin->AddChild(barrels);
                SGE_LOG("LogGameplay", EngineLog::Level::Display,
                    "AA turret barrels generated: the model has none");
            } else {
                SGE_LOG("LogGameplay", EngineLog::Level::Warning,
                    "AA turret barrel geometry failed to build");
            }
        }
    }

    // Scale is set through the TRS fields, not by writing localTransform:
    // UpdateGlobalTransform calls UpdateLocalTransform first, which rebuilds
    // localTransform from translation/rotation/scale and would discard anything
    // written directly into the matrix.
    const auto setScale = [](const std::shared_ptr<SceneNode>& node, float s) {
        node->scale = { s, s, s };
        node->UpdateLocalTransform();
    };

    // One material in the GLB ("Gun Metal.003", on the mount's centre block) is
    // left untextured with a yellow base colour of [0.60, 0.44, 0.00], while the
    // other four share a metal basecolour map. That renders as a bright yellow
    // slab in the middle of a dark grey emplacement, so give it the same texture
    // its siblings use and drop it to gunmetal.
    //
    // Both subtrees have to be walked: `traverse` was detached from `loaded`
    // above, and the yellow part (TurrentCenter) lives inside the detached gun.
    // Walking only `loaded` -- as this did at first -- silently missed it and
    // left the block yellow.
    {
        ComPtr<ID3D12Resource> sharedBaseColor;
        std::function<void(const std::shared_ptr<SceneNode>&)> collect =
            [&](const std::shared_ptr<SceneNode>& node) {
            if (!node || sharedBaseColor) return;
            if (node->mesh)
                for (const MeshPrimitive& primitive : node->mesh->primitives)
                    if (primitive.material &&
                        primitive.material->baseColorTexture) {
                        sharedBaseColor = primitive.material->baseColorTexture;
                        return;
                    }
            for (const std::shared_ptr<SceneNode>& child : node->children)
                collect(child);
        };
        collect(loaded);
        collect(traverse);

        // Dark, slightly blued steel. Applied as the base colour factor, which
        // multiplies the borrowed texture, so the part keeps the surrounding
        // metal's surface detail instead of becoming a flat grey block.
        constexpr float kGunmetal[3] = { 0.30f, 0.32f, 0.35f };
        std::function<void(const std::shared_ptr<SceneNode>&)> retint =
            [&](const std::shared_ptr<SceneNode>& node) {
            if (!node) return;
            if (node->mesh)
                for (MeshPrimitive& primitive : node->mesh->primitives) {
                    if (!primitive.material) continue;
                    // Only the untextured offender. Anything already carrying a
                    // basecolour map is authored correctly and is left alone.
                    if (primitive.material->baseColorTexture) continue;
                    if (sharedBaseColor)
                        primitive.material->baseColorTexture = sharedBaseColor;
                    primitive.material->baseColorFactor = XMFLOAT4(
                        kGunmetal[0], kGunmetal[1], kGunmetal[2], 1.0f);
                    // Read as metal rather than painted plastic.
                    primitive.material->metallicFactor = 0.90f;
                    primitive.material->roughnessFactor = 0.42f;
                }
            for (const std::shared_ptr<SceneNode>& child : node->children)
                retint(child);
        };
        retint(loaded);
        retint(traverse);
    }

    // Static base: everything the GLB has left after the gun was removed.
    auto base = std::make_shared<SceneNode>("Base");
    setScale(base, kModelScale);
    base->AddChild(loaded);
    root->AddChild(base);

    // The posed mount. PoseAATurretModel writes this node's localTransform every
    // frame, so the scale has to live on a node underneath rather than here --
    // the pose would otherwise replace it and the gun would snap back to model
    // units while the base stayed scaled.
    auto gun = std::make_shared<SceneNode>("Gun");
    auto gunScale = std::make_shared<SceneNode>("GunScale");
    setScale(gunScale, kModelScale);

    // Zero the traverse node's own placement before reparenting it.
    //
    // "Y-Rotation" sits 1.161 m up inside the GLB, and the "Gun" node above it
    // is translated to AATurretMountHeight as well -- keeping both stacked the
    // two lifts and floated the barrels about 1.9 m above the mount, which is
    // what put them up in the air. Its ROTATION must be preserved: the barrels
    // are oriented by a 120-degree quaternion on the child "spin-ey" node, and
    // the traverse node's own orientation is part of that chain.
    traverse->translation = { 0.0f, 0.0f, 0.0f };
    traverse->UpdateLocalTransform();

    gunScale->AddChild(traverse);
    gun->AddChild(gunScale);
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
