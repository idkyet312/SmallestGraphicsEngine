#ifndef SHOOTING_TARGET_H
#define SHOOTING_TARGET_H

#include <DirectXMath.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

struct ShootingTargetCenter {
    std::string name;
    DirectX::XMFLOAT2 position{};
    DirectX::XMFLOAT2 radii{};
};

struct ShootingTargetRing {
    float radius = 0.0f;
    uint32_t points = 0;
};

struct ShootingTargetDefinition {
    std::array<ShootingTargetCenter, 2> centers;
    std::vector<ShootingTargetRing> rings;
    float faceZ = 0.0f;
};

inline ShootingTargetDefinition ReadShootingTarget(const nlohmann::json& json) {
    ShootingTargetDefinition result;
    result.faceZ = json.at("faceZ").get<float>();
    if (!std::isfinite(result.faceZ))
        throw std::runtime_error("shootingTarget.faceZ must be finite");
    const auto readVec2 = [](const nlohmann::json& value) {
        if (!value.is_array() || value.size() != 2 ||
            !value[0].is_number() || !value[1].is_number())
            throw std::runtime_error("shootingTarget coordinates must be vec2");
        DirectX::XMFLOAT2 vector{ value[0].get<float>(), value[1].get<float>() };
        if (!std::isfinite(vector.x) || !std::isfinite(vector.y))
            throw std::runtime_error("shootingTarget coordinates must be finite");
        return vector;
    };
    const auto& centers = json.at("centers");
    if (!centers.is_array() || centers.size() != result.centers.size())
        throw std::runtime_error("shootingTarget must have two centers");
    for (size_t i = 0; i < centers.size(); ++i) {
        auto& center = result.centers[i];
        center.name = centers[i].at("name").get<std::string>();
        center.position = readVec2(centers[i].at("position"));
        center.radii = readVec2(centers[i].at("radii"));
        if (center.name.empty() || center.radii.x <= 0.0f || center.radii.y <= 0.0f)
            throw std::runtime_error("shootingTarget center name/radii are invalid");
    }
    const auto& rings = json.at("rings");
    if (!rings.is_array() || rings.empty() || rings.size() > 16)
        throw std::runtime_error("shootingTarget must have 1-16 scoring rings");
    float previousRadius = 0.0f;
    uint32_t previousPoints = 1000;
    for (const auto& value : rings) {
        ShootingTargetRing ring;
        ring.radius = value.at("radius").get<float>();
        const auto& points = value.at("points");
        if (!points.is_number_integer() || points.get<int64_t>() <= 0 ||
            points.get<int64_t>() > 1000)
            throw std::runtime_error("shootingTarget points must be integers in 1-1000");
        ring.points = points.get<uint32_t>();
        if (!std::isfinite(ring.radius) || ring.radius <= previousRadius ||
            ring.radius > 1.0f || ring.points > previousPoints)
            throw std::runtime_error("shootingTarget rings must grow outward with decreasing points");
        result.rings.push_back(ring);
        previousRadius = ring.radius;
        previousPoints = ring.points;
    }
    return result;
}

struct ShootingTargetInstance {
    uint64_t entityId = 0;
    ShootingTargetDefinition definition;
    DirectX::XMFLOAT4X4 localToWorld{};
};

struct ShootingTargetHit {
    uint32_t points = 0;
    std::string center;
};

inline ShootingTargetHit ScoreShootingTarget(
        const ShootingTargetInstance& target, const DirectX::XMFLOAT3& worldHit) {
    using namespace DirectX;
    XMVECTOR determinant;
    const XMMATRIX inverse = XMMatrixInverse(
        &determinant, XMLoadFloat4x4(&target.localToWorld));
    if (!std::isfinite(XMVectorGetX(determinant)) ||
        std::abs(XMVectorGetX(determinant)) < 1e-12f) return {};
    XMFLOAT3 local;
    XMStoreFloat3(&local, XMVector3TransformCoord(XMLoadFloat3(&worldHit), inverse));
    if (!std::isfinite(local.x) || !std::isfinite(local.y) ||
        !std::isfinite(local.z) || std::abs(local.z - target.definition.faceZ) > 0.001f)
        return {};
    ShootingTargetHit result;
    // The hit is already confirmed against the mesh. Measuring in its local
    // face plane keeps the printed ellipses correct under rotation and scale.
    for (const auto& center : target.definition.centers) {
        const float dx = (local.x - center.position.x) / center.radii.x;
        const float dy = (local.y - center.position.y) / center.radii.y;
        const float distanceSquared = dx * dx + dy * dy;
        for (const auto& ring : target.definition.rings) {
            // World/local float round trips can move a hit a few micrometres
            // across a painted line. Boundary hits belong to the inner ring.
            if (distanceSquared > ring.radius * ring.radius + 1e-5f) continue;
            if (ring.points > result.points) result = { ring.points, center.name };
            break;
        }
    }
    return result;
}

struct ShootingRangeScore {
    static constexpr float kFeedbackDurationSeconds = 2.0f;

    uint64_t totalPoints = 0;
    uint64_t hits = 0;
    uint64_t lastTargetPoints = 0;
    ShootingTargetHit lastHit;
    float feedbackSeconds = 0.0f;
    std::unordered_map<uint64_t, uint64_t> targetPoints;

    void Record(uint64_t entityId, ShootingTargetHit hit) {
        totalPoints += hit.points;
        if (hit.points > 0) ++hits;
        lastTargetPoints = (targetPoints[entityId] += hit.points);
        lastHit = std::move(hit);
        feedbackSeconds = kFeedbackDurationSeconds;
    }
};

#endif
