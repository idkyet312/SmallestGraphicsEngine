#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

// Keep the static GPU instance buffer intact. Damage splits its draw ranges
// only when an impact happens; visibility never scans individual blade roots.
class GrassDamageRanges {
public:
    static bool CircleIntersectsCell(float cx, float cz, float halfCell,
                                     float x, float z, float radius) {
        const float dx = (std::max)(std::abs(x - cx) - halfCell, 0.0f);
        const float dz = (std::max)(std::abs(z - cz) - halfCell, 0.0f);
        return dx * dx + dz * dz <= radius * radius;
    }

    void Clear() {
        m_damaged = false;
        m_survivors.clear();
    }

    template<class RootAt>
    void ExcludeCircle(uint32_t first, uint32_t count,
                       float x, float z, float radius, RootAt rootAt) {
        if (radius <= 0.0f || count == 0) return;
        std::vector<Range> survivors;
        bool removed = false;
        const auto filter = [&](const Range& range) {
            uint32_t runStart = range.firstInstance;
            const uint32_t end = range.firstInstance + range.instanceCount;
            for (uint32_t i = runStart; i < end; ++i) {
                const auto& root = rootAt(i);
                const float dx = root.x - x, dz = root.z - z;
                if (dx * dx + dz * dz > radius * radius) continue;
                if (i > runStart) survivors.push_back({runStart, i - runStart});
                runStart = i + 1;
                removed = true;
            }
            if (end > runStart) survivors.push_back({runStart, end - runStart});
        };
        if (m_damaged) {
            for (const Range& range : m_survivors) filter(range);
        } else {
            filter({first, count});
        }
        if (removed) {
            m_damaged = true;
            m_survivors.swap(survivors);
        }
    }

    template<class DrawRange>
    void AppendVisible(uint32_t first, uint32_t count,
                       std::vector<DrawRange>& out) const {
        if (count == 0) return;
        if (!m_damaged) {
            out.push_back({first, count});
            return;
        }
        // Density still trims the original instance prefix. Filling holes with
        // later instances would make undamaged grass pop in after an explosion.
        const uint32_t end = first + count;
        for (const Range& range : m_survivors) {
            if (range.firstInstance >= end) break;
            out.push_back({range.firstInstance,
                (std::min)(range.instanceCount, end - range.firstInstance)});
        }
    }

private:
    struct Range { uint32_t firstInstance, instanceCount; };
    bool m_damaged = false;
    std::vector<Range> m_survivors;
};
