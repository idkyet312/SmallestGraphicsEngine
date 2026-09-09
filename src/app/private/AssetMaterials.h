#pragma once

// Private application implementation; included once by main.cpp in dependency order.

static std::string ResolveTexturePath(const char* relativePath) {
    namespace fs = std::filesystem;
    const fs::path requested(relativePath);
    std::vector<fs::path> roots = { fs::current_path() };

    wchar_t modulePath[MAX_PATH] = {};
    const DWORD moduleLength = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    if (moduleLength > 0 && moduleLength < MAX_PATH)
        roots.push_back(fs::path(modulePath).parent_path());

    for (const fs::path& root : roots) {
        const fs::path candidates[] = {
            root / requested,
            root / "build" / requested,
            root.parent_path() / requested,
            root.parent_path() / "build" / requested
        };
        for (const fs::path& candidate : candidates) {
            if (fs::exists(candidate))
                return fs::weakly_canonical(candidate).string();
        }
    }
    return relativePath;
}

static std::vector<unsigned char> PinkMissingTexture(int size) {
    std::vector<unsigned char> pixels((size_t)size * size * 4);
    for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x) {
        const bool bright = ((x / 16) ^ (y / 16)) & 1;
        const size_t i = ((size_t)y * size + x) * 4;
        pixels[i + 0] = bright ? 255 : 90;
        pixels[i + 1] = 0;
        pixels[i + 2] = bright ? 255 : 90;
        pixels[i + 3] = 255;
    }
    return pixels;
}

// Both hulls (patrol and insertion) come from this one asset.
static constexpr const char* kBoatModelPath =
    "Content/Models/MilitaryBoatNew/MilitaryBoatv2.glb";

// MilitaryBoatv2 ships its one material as alphaMode=BLEND -- a Blender export
// default rather than a deliberate choice. Left alone, IsTransparent() puts the
// whole hull on the blended path, so the far gunwale shows through the near one
// and the deck reads as glass instead of painted metal. Force it opaque and
// double sided: the shell is a single thin surface, so back faces have to draw
// or the interior of the hull is missing.
//
// The patrol boat and the insertion boat load the same GLB separately, so each
// owns its own materials and needs its own pass.
static void ConfigureBoatMaterials(const std::shared_ptr<SceneNode>& boat) {
    if (!boat) return;
    if (boat->mesh) {
        for (MeshPrimitive& primitive : boat->mesh->primitives) {
            if (!primitive.material) continue;
            SceneMaterial& material = *primitive.material;
            material.doubleSided = true;
            material.alphaBlend = false;
            material.alphaCutout = false;
            // IsTransparent() also blends on a sub-unit factor alpha, so clearing
            // the mode flag alone would not force the hull opaque.
            material.baseColorFactor.w = 1.0f;
        }
    }
    // The GLB is flat today, but an exporter change that parents the hull under
    // a child node would silently skip the fix, so walk the whole subtree.
    for (const std::shared_ptr<SceneNode>& child : boat->children)
        ConfigureBoatMaterials(child);
}


// Island ground material: sand (Poly Haven "sand_02", CC0). The previous mud set
// was never actually in the repo, so the terrain had been falling back to the pink
// missing-texture placeholder.
static void LoadFloorMudMaterial() {
    floorMaterial = std::make_shared<SceneMaterial>();
    floorMaterial->name = "grass_004";
    floorMaterial->baseColorFactor = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
    floorMaterial->metallicFactor = 0.0f;
    floorMaterial->roughnessFactor = 1.0f;
    floorMaterial->viewFillStrength = 0.0f;

    const std::string dir = "Content/Models/grass/Grass004_2K-PNG/";
    floorMaterial->baseColorTexture = GLBImporter::LoadTextureFromFile(
        ResolveTexturePath((dir + "Grass004_2K-PNG_Color.png").c_str()),
        g_dx12.device, g_dx12.commandList, floorMaterial->uploadHeaps);
    floorMaterial->normalTexture = GLBImporter::LoadTextureFromFile(
        ResolveTexturePath((dir + "Grass004_2K-PNG_NormalGL.png").c_str()),
        g_dx12.device, g_dx12.commandList, floorMaterial->uploadHeaps);
    floorMaterial->normalYSign = 1.0f;
    floorMaterial->metallicRoughnessTexture = GLBImporter::LoadTextureFromFile(
        ResolveTexturePath((dir + "Grass004_2K-PNG_Roughness.png").c_str()),
        g_dx12.device, g_dx12.commandList, floorMaterial->uploadHeaps);
    floorMaterial->roughnessOnlyTexture =
        floorMaterial->metallicRoughnessTexture != nullptr;

    if (!floorMaterial->baseColorTexture) {
        const auto missing = PinkMissingTexture(256);
        floorMaterial->baseColorTexture = GLBImporter::CreateTextureFromRGBA(
            g_dx12.device.Get(), g_dx12.commandList.Get(), missing, 256, 256,
            floorMaterial->uploadHeaps);
        floorMaterial->baseColorFactor = XMFLOAT4(1, 1, 1, 1);
        std::cerr << "Grass ground texture unavailable; using pink missing texture\n";
    }

    // Soft smoke sprite for particle billboards.
    g_smokeTexture = GLBImporter::LoadTextureFromFile(
        ResolveTexturePath("Content/Models/textures/smoke.png"),
        g_dx12.device, g_dx12.commandList, g_smokeUploadHeaps);
    if (!g_smokeTexture)
        std::cerr << "Smoke sprite (models/textures/smoke.png) unavailable\n";

    g_bloodTexture = GLBImporter::LoadTextureFromFile(
        ResolveTexturePath("Content/Models/textures/blood_splat.png"),
        g_dx12.device, g_dx12.commandList, g_bloodUploadHeaps);
    if (!g_bloodTexture)
        std::cerr << "Blood sprite (models/textures/blood_splat.png) unavailable\n";

    g_muzzleFlashTexture = GLBImporter::LoadTextureFromFile(
        ResolveTexturePath("Content/Models/textures/muzzle_flash.png"),
        g_dx12.device, g_dx12.commandList, g_muzzleFlashUploadHeaps);
    if (!g_muzzleFlashTexture)
        std::cerr << "Muzzle flash (models/textures/muzzle_flash.png) unavailable\n";

    g_fireTexture = GLBImporter::LoadTextureFromFile(
        ResolveTexturePath("Content/Models/textures/fire_realistic_5x5.png"),
        g_dx12.device, g_dx12.commandList, g_fireUploadHeaps);
    if (!g_fireTexture)
        std::cerr << "Fire sprite (models/textures/fire_realistic_5x5.png) unavailable\n";

    g_explosionTexture = GLBImporter::LoadTextureFromFile(
        ResolveTexturePath("Content/Models/textures/explosion_soluna.png"),
        g_dx12.device, g_dx12.commandList, g_explosionUploadHeaps);
    if (!g_explosionTexture)
        std::cerr << "Explosion sheet (models/textures/explosion_soluna.png) unavailable\n";

    g_explosionCoreTexture = GLBImporter::LoadTextureFromFile(
        ResolveTexturePath("Content/Models/textures/explosion_boom3.png"),
        g_dx12.device, g_dx12.commandList, g_explosionCoreUploadHeaps);
    if (!g_explosionCoreTexture)
        std::cerr << "Explosion core sheet (models/textures/explosion_boom3.png) unavailable\n";
}

// Crysis-style plank wall: the destructible is built from real structural
// pieces -- vertical wooden planks held by horizontal cross-beams -- rather
// than a uniform Voronoi field. Each plank/beam is one child chunk, so a hit
// snaps that board loose along its true edges. Grid coords (x,y,z) drive
// Blast's adjacency bonding, so touching boards stay welded until struck.
// ?? procedural material textures for the destructible house ?????????????????
namespace HouseTex {
// Cheap hash-based value noise in [0,1].
inline float Hash(int x, int y) {
    uint32_t h = (uint32_t)(x * 374761393 + y * 668265263);
    h = (h ^ (h >> 13)) * 1274126177u;
    return ((h ^ (h >> 16)) & 0xFFFFFF) / (float)0xFFFFFF;
}
inline float ValueNoise(float x, float y) {
    const int xi = (int)std::floor(x), yi = (int)std::floor(y);
    const float fx = x - xi, fy = y - yi;
    const float sx = fx * fx * (3 - 2 * fx), sy = fy * fy * (3 - 2 * fy);
    const float a = Hash(xi, yi), b = Hash(xi + 1, yi);
    const float c = Hash(xi, yi + 1), d = Hash(xi + 1, yi + 1);
    return (a + (b - a) * sx) + ((c + (d - c) * sx) - (a + (b - a) * sx)) * sy;
}
inline float Fbm(float x, float y) {
    float sum = 0, amp = 0.5f, freq = 1;
    for (int o = 0; o < 4; ++o) { sum += ValueNoise(x * freq, y * freq) * amp; freq *= 2; amp *= 0.5f; }
    return sum;
}
inline unsigned char ToByte(float v) { return (unsigned char)std::max(0.0f, std::min(255.0f, v * 255.0f + 0.5f)); }

// Wood: vertical grain lines along V with warped rings and knots.
inline std::vector<unsigned char> Wood(int size, XMFLOAT3 base, XMFLOAT3 dark) {
    std::vector<unsigned char> px((size_t)size * size * 4);
    for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x) {
        const float u = (float)x / size, v = (float)y / size;
        const float warp = Fbm(u * 3.0f, v * 12.0f) * 0.35f;
        float grain = std::sin((u * 18.0f + warp) * 3.14159f);
        grain = 0.5f + 0.5f * grain * grain;                 // sharpen streaks
        grain = grain * 0.7f + Fbm(u * 40.0f, v * 6.0f) * 0.3f;
        const float t = std::min(1.0f, grain);
        const size_t i = ((size_t)y * size + x) * 4;
        px[i + 0] = ToByte(dark.x + (base.x - dark.x) * t);
        px[i + 1] = ToByte(dark.y + (base.y - dark.y) * t);
        px[i + 2] = ToByte(dark.z + (base.z - dark.z) * t);
        px[i + 3] = 255;
    }
    return px;
}
// Stone: blocky mortar grid with speckled fill.
inline std::vector<unsigned char> Stone(int size, XMFLOAT3 base, XMFLOAT3 mortar) {
    std::vector<unsigned char> px((size_t)size * size * 4);
    for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x) {
        const float u = (float)x / size * 4.0f, v = (float)y / size * 4.0f;
        const float bx = u - std::floor(u), by = v - std::floor(v);
        const float mortarLine = std::min(std::min(bx, 1 - bx), std::min(by, 1 - by));
        const float m = mortarLine < 0.06f ? 0.0f : 1.0f;
        const float speck = 0.6f + 0.4f * Fbm(u * 8.0f, v * 8.0f);
        const XMFLOAT3 c = { base.x * speck, base.y * speck, base.z * speck };
        const size_t i = ((size_t)y * size + x) * 4;
        px[i + 0] = ToByte(mortar.x + (c.x - mortar.x) * m);
        px[i + 1] = ToByte(mortar.y + (c.y - mortar.y) * m);
        px[i + 2] = ToByte(mortar.z + (c.z - mortar.z) * m);
        px[i + 3] = 255;
    }
    return px;
}
// Shingles: overlapping horizontal rows, staggered, with edge shadow.
inline std::vector<unsigned char> Shingle(int size, XMFLOAT3 base, XMFLOAT3 dark) {
    std::vector<unsigned char> px((size_t)size * size * 4);
    for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x) {
        const float u = (float)x / size, v = (float)y / size;
        const float row = v * 10.0f;
        const int ri = (int)std::floor(row);
        const float rf = row - ri;
        const float offset = (ri & 1) ? 0.5f : 0.0f;
        const float col = (u * 8.0f + offset);
        const float cf = col - std::floor(col);
        float shade = 1.0f - rf * 0.35f;                     // top-lit row
        if (cf < 0.04f || rf > 0.94f) shade *= 0.55f;         // shingle gaps
        shade *= 0.85f + 0.15f * Fbm(u * 20.0f, v * 20.0f);
        const size_t i = ((size_t)y * size + x) * 4;
        px[i + 0] = ToByte(dark.x + (base.x - dark.x) * shade);
        px[i + 1] = ToByte(dark.y + (base.y - dark.y) * shade);
        px[i + 2] = ToByte(dark.z + (base.z - dark.z) * shade);
        px[i + 3] = 255;
    }
    return px;
}
// Corrugated metal: tight vertical ribs shaded like a sine wave, streaked with
// grime and rust patches -- reads as galvanised roofing sheets.
inline std::vector<unsigned char> Corrugated(int size, XMFLOAT3 base, XMFLOAT3 rust) {
    std::vector<unsigned char> px((size_t)size * size * 4);
    for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x) {
        const float u = (float)x / size, v = (float)y / size;
        // Rib shading: sine across U, lit from one side so every rib has a
        // bright crest and a dark valley.
        const float rib = std::sin(u * 3.14159f * 2.0f * 34.0f);
        const float crease = std::pow(std::abs(rib), 10.0f);
        float shade = 0.48f + 0.42f * std::max(0.0f, rib) + 0.16f * crease;
        // Vertical weather streaks running down the sheet.
        shade *= 0.88f + 0.12f * Fbm(u * 60.0f, v * 4.0f);
        // Sparse rust blotches.
        const float rustMask = Fbm(u * 5.0f, v * 10.0f);
        const float drip = Fbm(u * 70.0f, v * 1.6f);
        const float r = rustMask > 0.70f ? std::min(1.0f, (rustMask - 0.70f) * 4.0f + drip * 0.20f) : drip * 0.025f;
        const size_t i = ((size_t)y * size + x) * 4;
        px[i + 0] = ToByte((base.x * shade) * (1 - r) + rust.x * r);
        px[i + 1] = ToByte((base.y * shade) * (1 - r) + rust.y * r);
        px[i + 2] = ToByte((base.z * shade) * (1 - r) + rust.z * r);
        px[i + 3] = 255;
    }
    return px;
}
}  // namespace HouseTex

// Assigns downloaded CC0 albedo textures (ambientCG, models/house_pbr) to the
// house's shared materials by name, falling back to procedurally generated
// wood/stone/shingle when a file is missing. Call after building the house,
// while a command list is open for the texture uploads.
static void ApplyHouseTextures(const std::shared_ptr<SceneNode>& house,
                               ID3D12Device* device, ID3D12GraphicsCommandList* cmdList) {
    if (!house) return;
    constexpr int kSize = 256;
    const std::vector<unsigned char> missingFallback = PinkMissingTexture(kSize);
    // Collect the unique materials by name from the house children.
    std::unordered_map<std::string, std::shared_ptr<SceneMaterial>> mats;
    for (const auto& child : house->children) {
        if (!child || !child->mesh) continue;
        for (const auto& prim : child->mesh->primitives)
            if (prim.material) mats[prim.material->name] = prim.material;
    }
    // Fracture chunks are closed prisms with visible interior faces. Meshlet
    // cone/HZB rejection can mistake the previous frame's shell for an
    // occluder and punch holes through intact walls. Cull these by frustum only.
    for (auto& entry : mats) {
        auto& material = entry.second;
        if (!material) continue;
        material->doubleSided = true;
        material->disableOcclusionCulling = true;
    }

    // Pack standalone roughness + optional AO into the glTF-style channels the
    // shared shaders already consume: R=AO, G=roughness, B=0. This preserves
    // surface variation on destructible chunks without adding another descriptor
    // or texture sample.
    auto assignPackedRoughAO = [&](const std::string& base,
                                   const std::shared_ptr<SceneMaterial>& mat) {
        std::vector<unsigned char> roughPixels;
        std::vector<unsigned char> aoPixels;
        int roughW = 0, roughH = 0, aoW = 0, aoH = 0;
        const std::string roughPath = ResolveTexturePath(
            (base + "_roughness.jpg").c_str());
        if (!GLBImporter::LoadPixelsRGBA(
                roughPath, roughPixels, roughW, roughH) ||
            roughW <= 0 || roughH <= 0) {
            return false;
        }

        const std::string aoPath = ResolveTexturePath(
            (base + "_ao.jpg").c_str());
        const bool hasAO = GLBImporter::LoadPixelsRGBA(
            aoPath, aoPixels, aoW, aoH) && aoW > 0 && aoH > 0;
        std::vector<unsigned char> packed(
            static_cast<size_t>(roughW) * roughH * 4, 255);
        for (int y = 0; y < roughH; ++y) {
            for (int x = 0; x < roughW; ++x) {
                const size_t dst =
                    (static_cast<size_t>(y) * roughW + x) * 4;
                const size_t rough = dst;
                unsigned char ao = 255;
                if (hasAO) {
                    const int ax = (std::min)(aoW - 1, x * aoW / roughW);
                    const int ay = (std::min)(aoH - 1, y * aoH / roughH);
                    ao = aoPixels[
                        (static_cast<size_t>(ay) * aoW + ax) * 4];
                }
                packed[dst + 0] = ao;
                packed[dst + 1] = roughPixels[rough + 0];
                packed[dst + 2] = 0;
                packed[dst + 3] = 255;
            }
        }

        mat->metallicRoughnessTexture = GLBImporter::CreateTextureFromRGBA(
            device, cmdList, packed, roughW, roughH, mat->uploadHeaps);
        mat->roughnessOnlyTexture =
            mat->metallicRoughnessTexture != nullptr;
        mat->occlusionStrength = hasAO ? 0.82f : 0.0f;
        return mat->metallicRoughnessTexture != nullptr;
    };

    // Load the downloaded albedo + normal maps. Albedo falls back to the
    // procedural texture so the house never renders untextured; the normal map
    // is optional (skipped if the file is absent).
    auto assign = [&](const char* name, const std::string& base, std::vector<unsigned char> fallback) {
        auto it = mats.find(name);
        if (it == mats.end()) return;
        auto& mat = it->second;
        mat->baseColorTexture = GLBImporter::LoadTextureFromFile(base + ".jpg", device, cmdList, mat->uploadHeaps);
        if (!mat->baseColorTexture) {
            mat->baseColorTexture = GLBImporter::CreateTextureFromRGBA(
                device, cmdList, fallback, kSize, kSize, mat->uploadHeaps);
        }
        if (mat->baseColorTexture) mat->baseColorFactor = XMFLOAT4(1, 1, 1, 1);
        mat->normalTexture = GLBImporter::LoadTextureFromFile(base + "_normal.jpg", device, cmdList, mat->uploadHeaps);
        if (!assignPackedRoughAO(base, mat)) {
            mat->metallicRoughnessTexture = GLBImporter::LoadTextureFromFile(
                base + "_roughness.jpg", device, cmdList, mat->uploadHeaps);
            mat->roughnessOnlyTexture =
                mat->metallicRoughnessTexture != nullptr;
        }
        if (mat->metallicRoughnessTexture) mat->metallicFactor = 0.0f;
    };
    auto assignFile = [&](const char* name, const std::string& colorPath,
                          const std::string& roughnessPath, std::vector<unsigned char> fallback) {
        auto it = mats.find(name);
        if (it == mats.end()) return;
        auto& mat = it->second;
        mat->baseColorTexture = GLBImporter::LoadTextureFromFile(colorPath, device, cmdList, mat->uploadHeaps);
        if (!mat->baseColorTexture) {
            mat->baseColorTexture = GLBImporter::CreateTextureFromRGBA(
                device, cmdList, missingFallback, kSize, kSize, mat->uploadHeaps);
        }
        if (mat->baseColorTexture) mat->baseColorFactor = XMFLOAT4(1, 1, 1, 1);
        mat->metallicRoughnessTexture = GLBImporter::LoadTextureFromFile(
            roughnessPath, device, cmdList, mat->uploadHeaps);
        mat->roughnessOnlyTexture = mat->metallicRoughnessTexture != nullptr;
        mat->metallicFactor = 0.75f;
        mat->roughnessFactor = mat->metallicRoughnessTexture ? 1.0f : 0.55f;
    };
    auto assignGeneratedMetal = [&](const char* name, const std::string& roughnessPath,
                                    std::vector<unsigned char> generated, float metallic, float roughness) {
        auto it = mats.find(name);
        if (it == mats.end()) return;
        auto& mat = it->second;
        mat->baseColorTexture = GLBImporter::CreateTextureFromRGBA(
            device, cmdList, generated, kSize, kSize, mat->uploadHeaps);
        if (mat->baseColorTexture) mat->baseColorFactor = XMFLOAT4(1, 1, 1, 1);
        mat->metallicRoughnessTexture = GLBImporter::LoadTextureFromFile(
            roughnessPath, device, cmdList, mat->uploadHeaps);
        mat->roughnessOnlyTexture = mat->metallicRoughnessTexture != nullptr;
        mat->metallicFactor = metallic;
        mat->roughnessFactor = roughness;
    };
    // roughnessScale multiplies the ARM map's roughness channel rather than
    // replacing it, so texture variation survives while the material sits in a
    // smoother or rougher band overall.
    auto assignPackedPBR = [&](const char* name, const std::string& colorPath,
                               const std::string& normalPath, const std::string& armPath,
                               std::vector<unsigned char> fallback,
                               float roughnessScale = 1.0f) {
        auto it = mats.find(name);
        if (it == mats.end()) return;
        auto& mat = it->second;
        mat->baseColorTexture = GLBImporter::LoadTextureFromFile(
            ResolveTexturePath(colorPath.c_str()), device, cmdList, mat->uploadHeaps);
        if (!mat->baseColorTexture) {
            mat->baseColorTexture = GLBImporter::CreateTextureFromRGBA(
                device, cmdList, missingFallback, kSize, kSize, mat->uploadHeaps);
        }
        mat->normalTexture = GLBImporter::LoadTextureFromFile(
            ResolveTexturePath(normalPath.c_str()), device, cmdList, mat->uploadHeaps);
        mat->metallicRoughnessTexture = GLBImporter::LoadTextureFromFile(
            ResolveTexturePath(armPath.c_str()), device, cmdList, mat->uploadHeaps);
        if (mat->baseColorTexture) mat->baseColorFactor = XMFLOAT4(1, 1, 1, 1);
        // Poly Haven ARM is R=AO, G=roughness, B=metallic: exact glTF layout.
        mat->roughnessOnlyTexture = false;
        mat->metallicFactor = 1.0f;
        mat->roughnessFactor = roughnessScale;
    };
    assign("Foundation", "Content/Models/house_pbr/foundation_brick",
           HouseTex::Stone(kSize, { 0.62f, 0.62f, 0.64f }, { 0.34f, 0.34f, 0.36f }));
    assign("Stud", "Content/Models/house_pbr/stud_wood",
           HouseTex::Wood(kSize, { 0.60f, 0.42f, 0.25f }, { 0.34f, 0.22f, 0.12f }));
    // Use the authored plank field: seams, nail heads, colour shifts, and AO make
    // broad walls read as construction instead of flat tan slabs.
    assign("Cladding", "Content/Models/house_pbr/cladding_wood",
           HouseTex::Wood(kSize, { 0.84f, 0.68f, 0.46f }, { 0.55f, 0.40f, 0.24f }));
    // Corrugated metal sheets; no downloaded map for this one, so the
    // procedural ribbed texture always kicks in.
    assign("Roof", "Content/Models/house_pbr/roof_metal",
           HouseTex::Corrugated(kSize, { 0.72f, 0.74f, 0.76f }, { 0.42f, 0.25f, 0.16f }));
    // roughnessFactor is INERT here, unlike everywhere else: roughnessOnlyTexture
    // makes the resolve replace roughness with the map's green channel outright
    // (`rough = clamp(mr.g, ...)`) rather than scaling by the factor. The rusted
    // map reads rough almost everywhere, so this wall sits above the 0.52 RT
    // reflection cut whatever the factor says, and the 0.66 that used to be here
    // was only documenting an intent the shader never applied.
    //
    // Dropping roughnessOnly for this material would hand it the scaling path,
    // but the map has no metallic or AO channel to supply -- so the honest fix is
    // to author the corrugated wall against the same galvanised-steel ARM set the
    // roof uses, which does carry all three. Left as-is deliberately rather than
    // pretending the factor does something: see matDarkMetal for the untextured
    // case, where the factors really are live.
    assignGeneratedMetal("MetalWall",
               "Content/Models/Corrugated metal pack/Wall/A/A Roughness rusted 2.jpg",
               HouseTex::Corrugated(kSize, { 0.30f, 0.34f, 0.31f }, { 0.43f, 0.22f, 0.10f }), 0.82f, 0.66f);
    // Galvanised steel roofing is fairly reflective, so 0.45 reads truer than a
    // flat 1.0 -- and it keeps the sheets under the RT reflection roughness cut
    // (0.52), above which the one-GGX-ray-per-frame reflection is mostly
    // variance and the surface is handed to the environment probe instead.
    // Matches RoofModel.h, which dresses the same corrugated-iron ARM set.
    assignPackedPBR("MetalRoof",
               "Content/Models/polyhaven/corrugated_iron/corrugated_iron_diff_2k.jpg",
               "Content/Models/polyhaven/corrugated_iron/corrugated_iron_nor_dx_2k.jpg",
               "Content/Models/polyhaven/corrugated_iron/corrugated_iron_arm_2k.jpg",
               HouseTex::Corrugated(kSize, { 0.42f, 0.46f, 0.48f }, { 0.38f, 0.18f, 0.08f }),
               0.45f);
}
