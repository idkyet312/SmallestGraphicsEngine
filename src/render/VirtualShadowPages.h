#pragma once

#include <array>
#include <algorithm>
#include <cstdint>

namespace VirtualShadows {
constexpr uint32_t Grid = 16;
constexpr uint32_t Cascades = 3;
constexpr uint32_t PageSize = 1024;
constexpr uint32_t AtlasSize = 4096;
constexpr uint32_t Capacity = 16;
constexpr uint32_t PageCount = Grid * Grid * Cascades;
constexpr uint32_t Invalid = ~0u;

// Packed uint4s match HLSL constant-buffer array strides.
struct alignas(16) Constants {
    std::array<uint32_t, 4> config{};
    std::array<std::array<uint32_t, 4>, PageCount / 4> table{};
    void Map(uint32_t key, uint32_t slot) { table[key / 4][key % 4] = slot + 1; }
};
static_assert(sizeof(Constants) == 16 + PageCount * 4, "VSM shader layout");

constexpr uint32_t Key(uint32_t cascade, uint32_t x, uint32_t y) {
    return cascade * Grid * Grid + y * Grid + x;
}

struct PageCache {
    std::array<uint32_t, Capacity> keys;
    std::array<bool, Capacity> valid{};
    std::array<bool, Capacity> requested{};
    PageCache() { keys.fill(Invalid); }
    void Invalidate() { valid.fill(false); }
    void InvalidateCascade(uint32_t cascade) {
        for (uint32_t i = 0; i < Capacity; ++i)
            if (keys[i] != Invalid && keys[i] / (Grid * Grid) == cascade)
                valid[i] = false;
    }
    // Protect all hits before assigning misses: request order must not evict a
    // resident page that is requested later in the same frame.
    void Request(const std::array<uint32_t, Capacity>& wanted, uint32_t count) {
        requested.fill(false);
        count = (std::min)(count, Capacity);
        for (uint32_t n = 0; n < count; ++n)
            for (uint32_t i = 0; i < Capacity; ++i)
                if (wanted[n] < PageCount && keys[i] == wanted[n]) requested[i] = true;
        for (uint32_t n = 0; n < count; ++n) {
            if (wanted[n] >= PageCount ||
                std::find(keys.begin(), keys.end(), wanted[n]) != keys.end()) continue;
            for (uint32_t i = 0; i < Capacity; ++i) {
                if (requested[i]) continue;
                keys[i] = wanted[n];
                requested[i] = true;
                valid[i] = false;
                break;
            }
        }
    }
};
}
