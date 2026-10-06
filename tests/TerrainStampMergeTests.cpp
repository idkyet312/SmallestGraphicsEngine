#include "TerrainStampMerge.h"
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>

extern "C" unsigned char* stbi_zlib_compress(
    unsigned char*, int, int*, int);

static int failures = 0;
#define CHECK(value) do { if (!(value)) { \
    std::cerr << __LINE__ << " CHECK failed: " #value "\n"; ++failures; \
} } while (false)

static float ApplyBrushes(float h, float x, float z,
                         const std::vector<TerrainSculptStamp>& stamps) {
    for (const auto& stamp : stamps) {
        const float dx = x - stamp.x, dz = z - stamp.z;
        float w = (std::max)(0.0f, 1.0f - std::sqrt(dx * dx + dz * dz) / stamp.radius);
        w = w * w * (3.0f - 2.0f * w);
        if (stamp.operation == TerrainSculptOperation::Add) h += stamp.value * w;
        else if (stamp.operation == TerrainSculptOperation::Flatten)
            h += (stamp.value - h) * (std::min)(1.0f, stamp.strength * w);
    }
    return h;
}

struct Image {
    std::vector<uint16_t> gray;
    int side = 0;
};

static Image ReadImage(const std::string& texture) {
    Image image;
    int width = 0, height = 0, channels = 0;
    const std::string path = ResolveTerrainStampPath(texture).string();
    CHECK(stbi_is_16_bit(path.c_str()));
    stbi_us* pixels = stbi_load_16(path.c_str(), &width, &height, &channels, 1);
    CHECK(pixels != nullptr);
    CHECK(width == height);
    if (pixels) image.gray.assign(pixels, pixels + static_cast<size_t>(width) * height);
    stbi_image_free(pixels);
    image.side = width;
    return image;
}

static float ApplyImage(float h, float x, float z,
                        const TerrainSculptStamp& stamp, const Image& image) {
    const float u = (x - stamp.x) / (stamp.radius * 2.0f) + 0.5f;
    const float v = (z - stamp.z) / (stamp.radius * 2.0f) + 0.5f;
    if (u < 0 || u > 1 || v < 0 || v > 1 || image.gray.empty()) return h;
    const float fx = u * (image.side - 1), fy = v * (image.side - 1);
    const int x0 = static_cast<int>(fx), y0 = static_cast<int>(fy);
    const int x1 = (std::min)(x0 + 1, image.side - 1);
    const int y1 = (std::min)(y0 + 1, image.side - 1);
    const auto texel = [&](int tx, int ty) { return float(image.gray[ty * image.side + tx]); };
    const float a = texel(x0, y0) + (texel(x1, y0) - texel(x0, y0)) * (fx - x0);
    const float b = texel(x0, y1) + (texel(x1, y1) - texel(x0, y1)) * (fx - x0);
    const float relief = ((a + (b - a) * (fy - y0)) / 65535.0f * 2 - 1) * stamp.value;
    const float start = (std::min)(stamp.edgeFalloff, 0.999f);
    float edge = ((std::max)(std::abs(x - stamp.x), std::abs(z - stamp.z)) /
        stamp.radius - start) / (1 - start);
    edge = (std::min)(1.0f, (std::max)(0.0f, edge));
    const float coverage = 1 - edge * edge * (3 - 2 * edge);
    return h + relief * coverage + (stamp.baseHeight - h) * coverage * stamp.replace;
}

int main() {
    std::vector<TerrainSculptStamp> stamps(32);
    for (size_t i = 0; i < stamps.size(); ++i) {
        stamps[i].x = float(i % 8) * 0.5f;
        stamps[i].z = float(i / 8) * 0.5f;
        stamps[i].radius = 4;
        stamps[i].value = (i % 2) ? -0.1f : 0.3f;
    }
    CHECK(!PlanTerrainStampMerge({}).valid);
    CHECK(!PlanTerrainStampMerge(std::vector<TerrainSculptStamp>(31)).valid);
    const auto local = PlanTerrainStampMerge(stamps);
    CHECK(local.valid && local.first == 0);
    CHECK((local.bounds.maxX - local.bounds.minX) * 1.02f <= kTerrainAutoMergeMaxSpan);

    auto distant = stamps;
    auto remote = stamps.front();
    remote.x = 1000;
    distant.insert(distant.begin(), remote);
    const auto region = PlanTerrainStampMerge(distant);
    CHECK(region.valid && region.first == 1);
    CHECK(region.bounds.maxX < 20);
    distant.back().x = -1000;
    CHECK(!PlanTerrainStampMerge(distant).valid);

    auto baked = remote;
    baked.texture = "Map/Map_baked.png";
    baked.operation = TerrainSculptOperation::Heightmap;
    baked.radius = 1000;
    distant = stamps;
    distant.insert(distant.begin(), baked);
    CHECK(PlanTerrainStampMerge(distant).first == 1);

    TerrainSculptStamp rotated;
    rotated.operation = TerrainSculptOperation::Heightmap;
    rotated.radius = 8;
    CHECK(TerrainStampMergeBounds(rotated).maxX == 8);
    rotated.rotation = 45;
    CHECK(std::abs(TerrainStampMergeBounds(rotated).maxX - 8 * std::sqrt(2.0f)) < 1e-5f);
    auto crater = rotated;
    crater.operation = TerrainSculptOperation::Crater;
    CHECK(std::abs(TerrainStampMergeBounds(crater).maxX - 8 * 1.18f) < 1e-5f);
    CHECK(IsTerrainAutoMergeStampFilename("Map/HM_Merged_1_0.png"));
    CHECK(!IsTerrainAutoMergeStampFilename("HM_Merged_1_0.png"));
    CHECK(!IsTerrainAutoMergeStampFilename("Map/Map_baked.png"));
    CHECK(!IsTerrainAutoMergeStampFilename("../HM_Merged_1_0.png"));

    std::vector<std::string> names;
    std::vector<TerrainSculptStamp> active;
    for (size_t i = 0; i < kMaxTerrainStampTextures; ++i) {
        names.push_back("Map/texture_" + std::to_string(i) + ".png");
        TerrainSculptStamp stamp;
        stamp.operation = TerrainSculptOperation::Heightmap;
        stamp.texture = names.back();
        stamp.x = 1000 + float(i) * 100;
        active.push_back(stamp);
    }
    CHECK(FindUnusedTerrainStampLayer(names, active) == (std::numeric_limits<size_t>::max)());
    auto capacity = active;
    capacity.insert(capacity.end(), stamps.begin(), stamps.end());
    CHECK(!PlanTerrainStampMerge(capacity).valid);
    active.erase(active.begin() + 17);
    capacity = active;
    capacity.insert(capacity.end(), stamps.begin(), stamps.end());
    CHECK(PlanTerrainStampMerge(capacity).valid);
    CHECK(FindUnusedTerrainStampLayer(names, active) == 17);
    CHECK(FindUnusedTerrainStampLayer(names, active, names[17]) ==
          (std::numeric_limits<size_t>::max)());

    const auto previousDirectory = std::filesystem::current_path();
    const auto testDirectory = std::filesystem::temp_directory_path() /
        ("SGE_StampMerge_" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(testDirectory);
    std::filesystem::current_path(testDirectory);

    CHECK(IsTerrainStampBakeResolution(2));
    CHECK(IsTerrainStampBakeResolution(16384));
    CHECK(!IsTerrainStampBakeResolution(1));
    CHECK(!IsTerrainStampBakeResolution(16385));
    CHECK(TerrainStampBufferTexels(16384) == kTerrainStampBakeOffset + size_t(16384) * 16384);
    CHECK(TerrainStampBufferTexels(4097) % 2 == 0);
    // Exceed the old 4K limit and preserve an arbitrary, non-power-of-two size.
    // An opt-in run exercises the full 16K allocation and PNG encode/decode.
    const uint32_t wholeSide = std::getenv("SGE_TEST_16K_TERRAIN_BAKE") ? 16384 : 4097;
    TerrainSculptStamp whole;
    const auto wholeResult = BakeTerrainSculptToStamp(
        stamps, [](float, float) { return 2.0f; },
        "Map/Map_baked.png", &stbi_zlib_compress, whole, wholeSide);
    CHECK(wholeResult.ok);
    CHECK(IsTerrainStampBakeFilename(whole.texture));
    const Image wholeImage = ReadImage(whole.texture);
    CHECK(wholeImage.side == int(wholeSide));
    CHECK(wholeImage.gray.size() == size_t(wholeSide) * wholeSide);
    CHECK(std::abs(ApplyImage(0, whole.x, whole.z, whole, wholeImage) - 2.0f) < 1e-4f);
    CHECK(std::abs(wholeResult.metresPerTexel - 2 * whole.radius / wholeSide) < 1e-6f);
    TerrainSculptStamp invalid;
    invalid.texture = "unchanged";
    size_t invalidSamples = 0;
    const auto invalidResult = BakeTerrainSculptToStamp(
        stamps, [&](float, float) { ++invalidSamples; return 0.0f; },
        "Map/invalid_baked.png", &stbi_zlib_compress, invalid, 16385);
    CHECK(!invalidResult.ok && invalidSamples == 0);
    CHECK(invalid.texture == "unchanged");

    // Include flatten after additive edits: the baked surface must retain the
    // ordered result, rather than treating all stamps as additive relief.
    stamps.back().operation = TerrainSculptOperation::Flatten;
    stamps.back().value = 3;
    stamps.back().strength = 0.7f;
    const auto base = [](float x, float z) { return 2 + x * 0.03f - z * 0.04f; };
    const auto height = [&](float x, float z) { return ApplyBrushes(base(x, z), x, z, stamps); };
    size_t samples = 0;
    bool inside = true;
    const auto sampler = [&](float x, float z) {
        ++samples;
        inside &= std::abs(x - (local.bounds.minX + local.bounds.maxX) * 0.5f) < 32;
        inside &= std::abs(z - (local.bounds.minZ + local.bounds.maxZ) * 0.5f) < 32;
        return height(x, z);
    };
    TerrainSculptStamp first;
    const auto firstResult = BakeTerrainSculptToStamp(
        stamps, sampler, "Map/HM_Merged_1_0.png", &stbi_zlib_compress, first,
        kTerrainStampResolution, local.bounds);
    CHECK(firstResult.ok);
    CHECK(samples == size_t(kTerrainStampResolution) * kTerrainStampResolution);
    CHECK(inside);
    CHECK(firstResult.metresPerTexel <= 0.125f);
    const Image firstImage = ReadImage(first.texture);
    CHECK(firstImage.side == int(kTerrainStampResolution));
    float maxError = 0;
    for (int y = 0; y <= 40; ++y) {
        for (int x = 0; x <= 40; ++x) {
            const float wx = local.bounds.minX + float(x) / 40 *
                (local.bounds.maxX - local.bounds.minX);
            const float wz = local.bounds.minZ + float(y) / 40 *
                (local.bounds.maxZ - local.bounds.minZ);
            maxError = (std::max)(maxError,
                std::abs(ApplyImage(base(wx, wz), wx, wz, first, firstImage) - height(wx, wz)));
        }
    }
    CHECK(maxError < 0.005f);
    CHECK(ApplyImage(base(1000, 1000), 1000, 1000, first, firstImage) == base(1000, 1000));

    // Redo retains the merged revision while undo can still load the old PNG.
    const auto savedFirst = firstImage.gray;
    TerrainSculptStamp second;
    const auto secondResult = BakeTerrainSculptToStamp(
        stamps, [&](float x, float z) { return height(x, z) + 0.7f; },
        "Map/HM_Merged_2_0.png", &stbi_zlib_compress, second,
        kTerrainStampResolution, local.bounds);
    CHECK(secondResult.ok);
    CHECK(first.texture != second.texture);
    CHECK(ReadImage(first.texture).gray == savedFirst);
    const Image secondImage = ReadImage(second.texture);
    CHECK(std::abs(ApplyImage(base(1, 1), 1, 1, second, secondImage) -
        ApplyImage(base(1, 1), 1, 1, first, firstImage) - 0.7f) < 0.005f);
    auto rebake = stamps;
    rebake.insert(rebake.begin(), first);
    CHECK(PlanTerrainStampMerge(rebake).valid);
    CHECK(PlanTerrainStampMerge(rebake).first == 0);

    TerrainSculptStamp failed;
    failed.texture = "unchanged";
    const auto failure = BakeTerrainSculptToStamp(
        stamps, height, "Map/fail.png", nullptr, failed,
        kTerrainStampResolution, local.bounds);
    CHECK(!failure.ok && failed.texture == "unchanged");
    CHECK(ReadImage(first.texture).gray == savedFirst);
    std::filesystem::current_path(previousDirectory);
    // Only this test's uniquely named directory under the OS temp root.
    const bool safeCleanup = std::filesystem::equivalent(
        testDirectory.parent_path(), std::filesystem::temp_directory_path());
    CHECK(safeCleanup);
    if (safeCleanup) std::filesystem::remove_all(testDirectory);
    std::cout << "Maximum local bake height error: " << maxError << " m\n";
    return failures ? 1 : 0;
}
