#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iterator>
#include <string>

namespace SGE::Cooked {

// Cooked file name for a source. Models replace their extension ("tree.fbx" ->
// "tree.sgeasset"). Loose textures pass `suffix` and keep theirs ("Bark.png" ->
// "Bark.png.sgeasset", or "R.png" + ".mr.sgeasset" for a packed pair), so a
// texture can never collide with a model of the same stem in the same folder.
inline std::filesystem::path CookedName(std::filesystem::path path,
                                        const char* suffix = nullptr) {
    if (!suffix) return path.replace_extension(".sgeasset");
    path += suffix;
    return path;
}

inline std::filesystem::path FindAssetForSource(
    const std::filesystem::path& source, const char* suffix = nullptr) {
    namespace fs = std::filesystem;
    const auto lower = [](std::string value) {
        std::transform(value.begin(), value.end(), value.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    };
    if (!suffix && lower(source.extension().string()) == ".sgeasset" &&
        fs::exists(source))
        return source;
    const fs::path sibling = CookedName(source, suffix);
    if (fs::exists(sibling)) return sibling;

    // Keep the source's Content root: cwd may be build/Release or a different
    // checkout, and the raw source need not ship in a cooked-only package.
    const fs::path normalized = source.lexically_normal();
    fs::path prefix;
    for (auto part = normalized.begin(); part != normalized.end(); ++part) {
        prefix /= *part;
        if (lower(part->string()) != "content") continue;
        fs::path relative;
        for (auto tail = std::next(part); tail != normalized.end(); ++tail)
            relative /= *tail;
        const fs::path candidate =
            prefix / "Cooked" / CookedName(relative, suffix);
        if (fs::exists(candidate)) return candidate;
    }
    return {};
}

} // namespace SGE::Cooked
