#pragma once

// Private application implementation; included once by main.cpp in dependency order.

struct PalmSpawn { float x, z, height, lean; };
static constexpr std::array<PalmSpawn, 8> kPalmSpawns = {{
    { -15.5f, -13.8f, 7.5f,  0.5f },
    {  -3.5f, -18.0f, 6.4f, -0.4f },
    {  14.8f, -14.2f, 8.2f,  0.7f },
    {  18.0f,   2.8f, 6.8f, -0.6f },
    {  14.2f,  14.8f, 7.1f,  0.3f },
    {   3.2f,  18.2f, 6.0f, -0.5f },
    { -14.5f,  14.0f, 7.8f,  0.6f },
    { -18.0f,   2.5f, 6.6f, -0.3f },
}};

// Stress-map landscaping: loose, asymmetric clusters soften the bare compounds
// without blocking house entrances, enemy spawns, or vehicle routes.
static constexpr std::array<PalmSpawn, 6> kStressHousePalmSpawns = {{
    { -12.5f,  11.5f, 7.4f,  0.35f },
    {  12.0f,  10.8f, 6.6f, -0.28f },
    { -12.2f, -10.8f, 8.0f,  0.42f },
    {  29.5f,  11.8f, 7.7f, -0.38f },
    {  54.2f,  10.5f, 6.8f,  0.30f },
    {  54.0f, -11.2f, 8.2f, -0.45f },
}};

static void ResetPalmTrees() {
    g_trees.Initialize();
    MatchFoliageMaterialToGrass();
    if (g_customLevelMode) {
        for (const LevelEntity& entity : g_game.world.Level().entities) {
            if (!entity.enabled || entity.type != LevelEntityType::Palm) continue;
            g_trees.Plant(entity.transform.position[0], entity.transform.position[2],
                entity.transform.scale[1], entity.transform.rotation[2]);
        }
        return;
    }
    for (const PalmSpawn& palm : kPalmSpawns)
        g_trees.Plant(palm.x, palm.z, palm.height * 2.0f, palm.lean);
    if (g_stressTestMode) {
        for (const PalmSpawn& palm : kStressHousePalmSpawns)
            g_trees.Plant(palm.x, palm.z, palm.height * 2.0f, palm.lean);
    }
}

static bool g_environmentInitialized = false;
static bool g_environmentStressMode = false;

static bool LoadDandelionModel() {
    if (g_dandelionModel) return true;
    const std::string root = "Content/Models/fbx_Dandelion/";
    auto model = FBXImporter::Load(root + "Dandelion.FBX",
        g_dx12.device, g_dx12.commandList, 1.0f, false, false);
    if (!model) {
        std::cerr << "Failed to load dandelion foliage model\n";
        return false;
    }

    auto material = std::make_shared<SceneMaterial>();
    material->name = "Dandelion foliage";
    const XMFLOAT3& grassAlbedo = g_grass.Albedo();
    material->baseColorFactor =
        XMFLOAT4(grassAlbedo.x, grassAlbedo.y, grassAlbedo.z, 1.0f);
    material->metallicFactor = 0.0f;
    material->roughnessFactor = 1.0f;
    material->doubleSided = true;
    material->alphaCutout = true;
    // Leaf card: keep the foliage edge bleed and dark-texel lift.
    material->foliageShading = true;
    material->alphaCutoff = 0.20f;
    material->disableOcclusionCulling = true;

    std::vector<unsigned char> albedo;
    int aw = 0, ah = 0;
    if (GLBImporter::LoadPixelsRGBA(root + "dandelion_leaf.jpg",
            albedo, aw, ah)) {
        // Source JPEG has a white backdrop instead of alpha. Convert distance
        // from white into a soft cutout while preserving the leaf colour.
        const size_t pixelCount = static_cast<size_t>(aw) * ah;
        for (size_t pixel = 0; pixel < pixelCount; ++pixel) {
            const size_t i = pixel * 4;
            const int darkness = 255 - (std::min)({
                static_cast<int>(albedo[i]), static_cast<int>(albedo[i + 1]),
                static_cast<int>(albedo[i + 2]) });
            albedo[i + 3] = static_cast<unsigned char>(
                std::clamp((darkness - 5) * 8, 0, 255));
        }
        material->baseColorTexture = GLBImporter::CreateTextureFromRGBA(
            g_dx12.device.Get(), g_dx12.commandList.Get(), albedo, aw, ah,
            material->uploadHeaps);
    }

    XMFLOAT3 boundsMin(FLT_MAX, FLT_MAX, FLT_MAX);
    XMFLOAT3 boundsMax(-FLT_MAX, -FLT_MAX, -FLT_MAX);
    const auto configure = [&](const auto& self,
                               const std::shared_ptr<SceneNode>& node) -> void {
        if (!node) return;
        if (node->mesh) for (MeshPrimitive& primitive : node->mesh->primitives) {
            primitive.material = material;
            for (size_t i = 0; i + 2 < primitive.vertices.size(); i += 12) {
                const float x = primitive.vertices[i];
                const float y = primitive.vertices[i + 1];
                const float z = primitive.vertices[i + 2];
                boundsMin.x = (std::min)(boundsMin.x, x);
                boundsMin.y = (std::min)(boundsMin.y, y);
                boundsMin.z = (std::min)(boundsMin.z, z);
                boundsMax.x = (std::max)(boundsMax.x, x);
                boundsMax.y = (std::max)(boundsMax.y, y);
                boundsMax.z = (std::max)(boundsMax.z, z);
            }
        }
        for (const auto& child : node->children) self(self, child);
    };
    configure(configure, model);
    // FBX stores each frond under a transformed child node. Bake those node
    // transforms before computing plant bounds or instancing; otherwise the
    // shared normalization transform scales around raw card coordinates and
    // pulls individual leaves away from the plant.
    if (auto merged = GLBImporter::MergeSceneByMaterial(model, g_dx12.device))
        model = std::move(merged);
    // Merge baked every source-node transform into vertex positions. Reset the
    // replacement root or FBX root rotation/scale gets applied a second time.
    model->translation = XMFLOAT3(0.0f, 0.0f, 0.0f);
    model->rotation = XMFLOAT4(0.0f, 0.0f, 0.0f, 1.0f);
    model->scale = XMFLOAT3(1.0f, 1.0f, 1.0f);
    XMFLOAT4X4 identity;
    XMStoreFloat4x4(&identity, XMMatrixIdentity());
    model->UpdateGlobalTransform(identity);
    boundsMin = XMFLOAT3(FLT_MAX, FLT_MAX, FLT_MAX);
    boundsMax = XMFLOAT3(-FLT_MAX, -FLT_MAX, -FLT_MAX);
    const auto collectBounds = [&](const auto& self,
                                   const std::shared_ptr<SceneNode>& node) -> void {
        if (!node) return;
        if (node->mesh) for (const MeshPrimitive& primitive : node->mesh->primitives) {
            for (size_t i = 0; i + 2 < primitive.vertices.size(); i += 12) {
                boundsMin.x = (std::min)(boundsMin.x, primitive.vertices[i]);
                boundsMin.y = (std::min)(boundsMin.y, primitive.vertices[i + 1]);
                boundsMin.z = (std::min)(boundsMin.z, primitive.vertices[i + 2]);
                boundsMax.x = (std::max)(boundsMax.x, primitive.vertices[i]);
                boundsMax.y = (std::max)(boundsMax.y, primitive.vertices[i + 1]);
                boundsMax.z = (std::max)(boundsMax.z, primitive.vertices[i + 2]);
            }
        }
        for (const auto& child : node->children) self(self, child);
    };
    collectBounds(collectBounds, model);
    if (boundsMin.x == FLT_MAX || !material->baseColorTexture) {
        std::cerr << "Dandelion geometry or cutout texture missing\n";
        return false;
    }
    g_dandelionSourceCenter = XMFLOAT3(
        (boundsMin.x + boundsMax.x) * 0.5f, 0.0f,
        (boundsMin.z + boundsMax.z) * 0.5f);
    g_dandelionSourceMinY = boundsMin.y;
    g_dandelionSourceHeight = (std::max)(0.001f, boundsMax.y - boundsMin.y);
    g_dandelionModel = std::move(model);
    MatchFoliageMaterialToGrass();
    std::cout << "Dandelion foliage model ready\n";
    return true;
}

static void ScatterDandelions(
    const std::function<float(float, float)>& terrainSampler,
    const std::vector<GrassField::Exclusion>& exclusions) {
    g_dandelionInstances.clear();
    if (!g_dandelionModel) return;
    uint32_t rng = 0x7f4a7c15u;
    auto random01 = [&]() {
        rng = rng * 1664525u + 1013904223u;
        return static_cast<float>(rng >> 8) * (1.0f / 16777216.0f);
    };
    // Mirrors GrassField::Excluded, including the optional circular test, so
    // painted clearings remove dandelions and grass over the same shape.
    const auto excluded = [&](float x, float z) {
        for (const auto& e : exclusions) {
            if (x < e.minX || x > e.maxX || z < e.minZ || z > e.maxZ) continue;
            if (e.radius <= 0.0f) return true;
            const float dx = x - e.centerX;
            const float dz = z - e.centerZ;
            if (dx * dx + dz * dz <= e.radius * e.radius) return true;
        }
        return false;
    };
    // The base is a compound, not a meadow, and its flat terrain defeats both
    // rejection tests below: the y < 0.22 waterline check and the slope check
    // only thin a scatter where there is a coastline and relief to thin it
    // against. On a level plane nearly every candidate survives, which measured
    // 1227 instances against the island's ~404 and put the grass pass at 28 ms
    // a frame. Authored Dandelion entities are still honoured below.
    const int clusterCount = g_baseMode ? 0 : (g_stressTestMode ? 420 : 180);
    const float span = g_stressTestMode ? 196.0f : 96.0f;
    g_dandelionInstances.reserve(clusterCount * 7);
    for (int cluster = 0; cluster < clusterCount; ++cluster) {
        // First clusters sit around level-one compound so foliage is visible
        // immediately; remaining clusters cover terrain deterministically.
        static constexpr XMFLOAT2 nearbyClusters[] = {
            {  4.3f,  4.2f }, { -4.4f,  4.0f },
            {  4.1f, -4.3f }, { -4.2f, -4.4f },
            { 14.0f,  8.0f }, {-14.0f,  7.0f }
        };
        const float cx = cluster < static_cast<int>(std::size(nearbyClusters))
            ? nearbyClusters[cluster].x : (random01() - 0.5f) * span;
        const float cz = cluster < static_cast<int>(std::size(nearbyClusters))
            ? nearbyClusters[cluster].y : (random01() - 0.5f) * span;
        const int plants = 4 + static_cast<int>(random01() * 7.0f);
        for (int plant = 0; plant < plants; ++plant) {
            const float angle = random01() * XM_2PI;
            const float distance = std::sqrt(random01()) * 2.8f;
            const float x = cx + std::cos(angle) * distance;
            const float z = cz + std::sin(angle) * distance;
            if (excluded(x, z)) continue;
            if (random01() > g_grass.TerrainGrassDensity(x, z)) continue;
            const float y = terrainSampler(x, z);
            if (y < 0.22f) continue;
            const float hx = terrainSampler(x + 0.35f, z) -
                             terrainSampler(x - 0.35f, z);
            const float hz = terrainSampler(x, z + 0.35f) -
                             terrainSampler(x, z - 0.35f);
            if (hx * hx + hz * hz > 0.34f) continue;
            const float targetHeight = 0.544f + random01() * 0.376f;
            const float scale = targetHeight / g_dandelionSourceHeight;
            const float yaw = random01() * XM_2PI;
            const XMMATRIX transform =
                XMMatrixTranslation(-g_dandelionSourceCenter.x,
                                    -g_dandelionSourceMinY,
                                    -g_dandelionSourceCenter.z) *
                XMMatrixScaling(scale, scale, scale) *
                XMMatrixRotationY(yaw) *
                // Sink stem/pivot slightly so sloped terrain cannot expose a
                // gap below the lowest card.
                XMMatrixTranslation(x, y + 0.025f, z);
            DandelionInstance instance;
            XMStoreFloat4x4(&instance.transform, transform);
            instance.center = XMFLOAT3(x, y + 0.025f + targetHeight * 0.48f, z);
            instance.radius = targetHeight * 0.75f;
            g_dandelionInstances.push_back(instance);
        }
    }
    if (g_customLevelMode) {
        for (const LevelEntity& entity : g_game.world.Level().entities) {
            if (!entity.enabled ||
                entity.type != LevelEntityType::Dandelion) continue;
            const float x = entity.transform.position[0];
            const float z = entity.transform.position[2];
            if (g_grass.TerrainGrassDensity(x, z) <= 0.0f) continue;
            const float y = terrainSampler(x, z);
            const float authoredScale = (std::max)(0.05f, entity.transform.scale[1]);
            const float targetHeight = 0.75f * authoredScale;
            const float scale = targetHeight / g_dandelionSourceHeight;
            const XMMATRIX transform =
                XMMatrixTranslation(-g_dandelionSourceCenter.x,
                                    -g_dandelionSourceMinY,
                                    -g_dandelionSourceCenter.z) *
                XMMatrixScaling(scale, scale, scale) *
                EulerDegreesToMatrix(entity.transform.rotation) *
                XMMatrixTranslation(x, y + 0.025f, z);
            DandelionInstance instance;
            XMStoreFloat4x4(&instance.transform, transform);
            instance.center = XMFLOAT3(x, y + 0.025f + targetHeight * 0.48f, z);
            instance.radius = targetHeight * 0.75f;
            g_dandelionInstances.push_back(instance);
        }
    }
    std::cout << "Dandelion foliage: " << g_dandelionInstances.size()
              << " GPU instances\n";
}
