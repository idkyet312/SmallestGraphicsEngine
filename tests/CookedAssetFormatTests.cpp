#include "CookedAssetFormat.h"
#include "CookedAssetPaths.h"

#include <cassert>
#include <cstdint>
#include <chrono>
#include <fstream>
#include <stdexcept>

int main() {
    using namespace SGE::Cooked;
    Header header;
    header.headerSize = sizeof(Header);
    header.fileSize = 4096;
    header.primitiveOffset = 256;
    header.primitiveCount = 2;
    header.materialOffset = 512;
    header.materialCount = 1;
    header.textureOffset = 768;
    header.textureCount = 1;
    header.clipOffset = 1024;
    header.clipCount = 1;
    header.stringOffset = 1280;
    header.stringSize = 128;
    header.payloadOffset = 1536;
    header.payloadSize = header.fileSize - header.payloadOffset;
    assert(HeaderValid(header, 4096));

    Header badMagic = header;
    badMagic.magic = 0;
    assert(!HeaderValid(badMagic, 4096));

    Header overflow = header;
    overflow.primitiveOffset = 4080;
    overflow.primitiveCount = 2;
    assert(!HeaderValid(overflow, 4096));

    assert(RangeValid(4000, 96, 4096));
    assert(!RangeValid(4000, 97, 4096));
    assert(!ArrayValid(0, UINT64_MAX, 2, 4096));

    namespace fs = std::filesystem;
    const fs::path temp = fs::temp_directory_path() /
        ("sge-cooked-paths-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    const fs::path content = temp / "content";
    const fs::path source = content / "Models/MainPlayer/Guns/Attachment/Sight.glb";
    const fs::path cooked = content / "Cooked/Models/MainPlayer/Guns/Attachment/Sight.sgeasset";
    fs::create_directories(cooked.parent_path());
    std::ofstream(cooked).put('x');
    const auto require = [](bool ok) {
        if (!ok) throw std::runtime_error("cooked asset path regression");
    };
    try {
        require(!fs::exists(source));
        require(FindAssetForSource(source) == cooked);
        require(FindAssetForSource(cooked) == cooked);
        const fs::path relative = source.lexically_relative(fs::current_path());
        if (!relative.empty())
            require(fs::equivalent(FindAssetForSource(relative), cooked));
        fs::path sibling = source;
        sibling.replace_extension(".sgeasset");
        fs::create_directories(sibling.parent_path());
        std::ofstream(sibling).put('x');
        require(FindAssetForSource(source) == sibling);
        require(FindAssetForSource(content / "Models/missing.glb").empty());
        require(FindAssetForSource(temp / "NotContent/Models/Sight.glb").empty());
    } catch (...) {
        fs::remove_all(temp);
        throw;
    }
    fs::remove_all(temp);
    return 0;
}
