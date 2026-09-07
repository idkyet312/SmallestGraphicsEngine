#pragma once

#include <DirectXMath.h>
#include <algorithm>
#include <cmath>

namespace SGE::DestructionCollision {
using DirectX::XMFLOAT3;

inline bool SphereAabb(const XMFLOAT3& p, float radius, const XMFLOAT3& lo, const XMFLOAT3& hi) {
    const float x = std::max(lo.x, std::min(p.x, hi.x));
    const float y = std::max(lo.y, std::min(p.y, hi.y));
    const float z = std::max(lo.z, std::min(p.z, hi.z));
    const float dx = p.x - x, dy = p.y - y, dz = p.z - z;
    return dx * dx + dy * dy + dz * dz <= radius * radius;
}

inline bool SegmentAabb(const XMFLOAT3& start, const XMFLOAT3& end, float radius,
                 const XMFLOAT3& lo, const XMFLOAT3& hi, float& hitT) {
    const float s[3] = { start.x, start.y, start.z };
    const float d[3] = { end.x - start.x, end.y - start.y, end.z - start.z };
    const float lower[3] = { lo.x - radius, lo.y - radius, lo.z - radius };
    const float upper[3] = { hi.x + radius, hi.y + radius, hi.z + radius };
    float t0 = 0.0f, t1 = 1.0f;
    for (int axis = 0; axis < 3; ++axis) {
        if (std::abs(d[axis]) < 1e-6f) {
            if (s[axis] < lower[axis] || s[axis] > upper[axis]) return false;
            continue;
        }
        float a = (lower[axis] - s[axis]) / d[axis];
        float b = (upper[axis] - s[axis]) / d[axis];
        if (a > b) std::swap(a, b);
        t0 = std::max(t0, a); t1 = std::min(t1, b);
        if (t0 > t1) return false;
    }
    hitT = t0;
    return true;
}

} // namespace SGE::DestructionCollision
