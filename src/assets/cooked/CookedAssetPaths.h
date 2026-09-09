#pragma once

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <iterator>
#include <string>

namespace SGE::Cooked {

inline std::filesystem::path FindAssetForSource(
    const std::filesystem::path& source) {
    namespace fs = std::filesystem;
    const auto lower = [](std::string value) {
        std::transform(value.begin(), value.end(), value.begin(),
            [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return value;
    };
    if (lower(source.extension().string()) == ".sgeasset" && fs::exists(source))
        return source;
    fs::path sibling = source;
    sibling.replace_extension(".sgeasset");
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
        relative.replace_extension(".sgeasset");
        const fs::path candidate = prefix / "Cooked" / relative;
        if (fs::exists(candidate)) return candidate;
    }
    return {};
}

} // namespace SGE::Cooked
