#pragma once
#include "SkinnedTypes.h"
#include <DirectXMath.h>
#include <fstream>
#include <regex>
#include <sstream>
#include <algorithm>

class T3DPhysicsAsset {
public:
    static RagdollSpec Load(const std::string& path) {
        std::ifstream file(path);
        if (!file) return {};
        std::ostringstream stream; stream << file.rdbuf();
        const std::string text = stream.str();
        RagdollSpec result;

        const std::regex bodyBlock(
            R"rx(Begin Object Name="SkeletalBodySetup_[^"]+"[\s\S]*?AggGeom=\(([^\r\n]+)[\s\S]*?BoneName="([^"]+)"[\s\S]*?End Object)rx");
        for (std::sregex_iterator it(text.begin(), text.end(), bodyBlock), end; it != end; ++it) {
            const std::string geom = (*it)[1].str();
            RagdollBodySpec body;
            body.bone = (*it)[2].str();
            body.massFraction = HumanMassFraction(body.bone);
            ParseShapes(geom, body.shapes);
            if (!body.shapes.empty()) result.bodies.push_back(std::move(body));
        }

        const std::regex constraintBlock(
            R"rx(Begin Object Name="PhysicsConstraintTemplate_[^"]+"[\s\S]*?End Object)rx");
        const std::regex bones(
            R"rx(ConstraintBone1="([^"]+)",ConstraintBone2="([^"]+)")rx");
        for (std::sregex_iterator it(text.begin(), text.end(), constraintBlock), end;
             it != end; ++it) {
            const std::string block = it->str();
            std::smatch names;
            if (!std::regex_search(block, names, bones)) continue;
            RagdollConstraintSpec link;
            link.boneA = names[1].str();
            link.boneB = names[2].str();

            const float radians = DirectX::XM_PI / 180.0f;
            link.frameA = Frame(block, 1);
            link.frameB = Frame(block, 2);
            link.swing1Motion = Motion(block, "Swing1Motion");
            link.swing2Motion = Motion(block, "Swing2Motion");
            link.twistMotion = Motion(block, "TwistMotion");
            link.swing1Angle = Limit(block, "Swing1LimitDegrees",
                link.swing1Motion, 25.0f, 70.0f) * radians;
            link.swing2Angle = Limit(block, "Swing2LimitDegrees",
                link.swing2Motion, 25.0f, 70.0f) * radians;
            const float twist = Limit(block, "TwistLimitDegrees",
                link.twistMotion, 15.0f, 55.0f) * radians;
            link.lowerTwistAngle = -twist;
            link.upperTwistAngle = twist;
            const bool swing1Hinge = link.swing1Motion != RagdollMotion::Locked &&
                                     link.swing2Motion == RagdollMotion::Locked;
            const bool swing2Hinge = link.swing2Motion != RagdollMotion::Locked &&
                                     link.swing1Motion == RagdollMotion::Locked;
            if ((swing1Hinge || swing2Hinge) &&
                (Contains(link.boneA, "calf") || Contains(link.boneA, "lowerarm"))) {
                link.jointType = RagdollJointType::Hinge;
                link.hingeAxis = swing1Hinge ? 1 : 2;
                link.lowerHingeAngle = -5.0f * radians;
                link.upperHingeAngle = Contains(link.boneA, "calf")
                    ? 145.0f * radians : 150.0f * radians;
            }
            ApplyAnatomicalLimits(link);
            result.constraints.push_back(std::move(link));
        }
        return result;
    }

private:
    static bool Contains(const std::string& value, const char* needle) {
        return value.find(needle) != std::string::npos;
    }

    static float HumanMassFraction(const std::string& bone) {
        if (Contains(bone, "pelvis")) return 0.15f;
        if (Contains(bone, "spine_01")) return 0.16f;
        if (Contains(bone, "spine_02")) return 0.18f;
        if (Contains(bone, "head")) return 0.08f;
        if (Contains(bone, "upperarm")) return 0.03f;
        if (Contains(bone, "lowerarm")) return 0.02f;
        if (Contains(bone, "hand")) return 0.007f;
        if (Contains(bone, "thigh")) return 0.10f;
        if (Contains(bone, "calf")) return 0.045f;
        if (Contains(bone, "foot")) return 0.013f;
        return 0.01f;
    }

    static DirectX::XMFLOAT4 Rotation(const std::string& text) {
        const DirectX::XMFLOAT3 euler = Vec3(text, "Rotation");
        const float d = DirectX::XM_PI / 180.0f;
        DirectX::XMFLOAT4 result;
        DirectX::XMStoreFloat4(&result,
            DirectX::XMQuaternionRotationRollPitchYaw(euler.z*d, euler.x*d, euler.y*d));
        return result;
    }

    static void ParseShapes(const std::string& geom,
                            std::vector<RagdollShapeSpec>& shapes) {
        const std::regex capsule(
            R"rx(\(Center=\([^)]+\),Rotation=\([^)]+\),Radius=([-+0-9.eE]+),Length=([-+0-9.eE]+)\))rx");
        for (std::sregex_iterator it(geom.begin(), geom.end(), capsule), end; it != end; ++it) {
            RagdollShapeSpec shape;
            shape.type = RagdollShapeType::Capsule;
            shape.center = Scale(Vec3(it->str(), "Center"), 0.01f);
            shape.rotation = Rotation(it->str());
            shape.radius = std::stof((*it)[1].str()) * 0.01f;
            shape.length = std::stof((*it)[2].str()) * 0.01f;
            shape.halfExtent = { shape.radius, shape.radius + shape.length*0.5f, shape.radius };
            shapes.push_back(shape);
        }
        const std::regex box(
            R"rx(\(Center=\([^)]+\),Rotation=\([^)]+\),X=([-+0-9.eE]+),Y=([-+0-9.eE]+),Z=([-+0-9.eE]+)\))rx");
        for (std::sregex_iterator it(geom.begin(), geom.end(), box), end; it != end; ++it) {
            RagdollShapeSpec shape;
            shape.type = RagdollShapeType::Box;
            shape.center = Scale(Vec3(it->str(), "Center"), 0.01f);
            shape.rotation = Rotation(it->str());
            shape.halfExtent = { std::stof((*it)[1].str())*0.005f,
                                 std::stof((*it)[2].str())*0.005f,
                                 std::stof((*it)[3].str())*0.005f };
            shapes.push_back(shape);
        }
        const std::regex sphere(
            R"rx(\(Center=\([^)]+\),Radius=([-+0-9.eE]+)\))rx");
        for (std::sregex_iterator it(geom.begin(), geom.end(), sphere), end; it != end; ++it) {
            RagdollShapeSpec shape;
            shape.type = RagdollShapeType::Sphere;
            shape.center = Scale(Vec3(it->str(), "Center"), 0.01f);
            shape.radius = std::stof((*it)[1].str()) * 0.01f;
            shape.halfExtent = { shape.radius, shape.radius, shape.radius };
            shapes.push_back(shape);
        }
    }

    static RagdollMotion Motion(const std::string& block, const char* field) {
        const std::string prefix = std::string(field) + "=";
        if (block.find(prefix + "ACM_Locked") != std::string::npos)
            return RagdollMotion::Locked;
        if (block.find(prefix + "ACM_Free") != std::string::npos)
            return RagdollMotion::Free;
        return RagdollMotion::Limited;
    }

    static float Limit(const std::string& block, const char* field,
                       RagdollMotion motion, float fallback, float freeValue) {
        if (motion == RagdollMotion::Locked) return 2.0f;
        if (motion == RagdollMotion::Free) return freeValue;
        const float value = Field(block, field);
        return value > 0.0f ? value : fallback;
    }

    static RagdollJointFrame Frame(const std::string& block, int index) {
        RagdollJointFrame frame;
        const std::string suffix = std::to_string(index);
        frame.position = Scale(Vec3(block, ("Pos" + suffix).c_str()), 0.01f);
        frame.primary = Vec3Or(block, ("PriAxis" + suffix).c_str(), { 1,0,0 });
        frame.secondary = Vec3Or(block, ("SecAxis" + suffix).c_str(), { 0,1,0 });
        return frame;
    }

    static void ApplyAnatomicalLimits(RagdollConstraintSpec& link) {
        const float d = DirectX::XM_PI / 180.0f;
        if (Contains(link.boneA, "spine_01")) {
            link.swing1Angle = 15*d; link.swing2Angle = 10*d;
            link.lowerTwistAngle = -15*d; link.upperTwistAngle = 15*d;
        } else if (Contains(link.boneA, "spine_02")) {
            link.swing1Angle = 12*d; link.swing2Angle = 10*d;
            link.lowerTwistAngle = -12*d; link.upperTwistAngle = 12*d;
        } else if (Contains(link.boneA, "head")) {
            link.swing1Angle = 20*d; link.swing2Angle = 20*d;
            link.lowerTwistAngle = -35*d; link.upperTwistAngle = 35*d;
        } else if (Contains(link.boneA, "upperarm")) {
            link.swing1Angle = 70*d; link.swing2Angle = 55*d;
            link.lowerTwistAngle = -35*d; link.upperTwistAngle = 35*d;
        } else if (Contains(link.boneA, "thigh")) {
            link.swing1Angle = 50*d; link.swing2Angle = 35*d;
            link.lowerTwistAngle = -25*d; link.upperTwistAngle = 25*d;
        }
    }

    static DirectX::XMFLOAT3 Scale(const DirectX::XMFLOAT3& value, float scale) {
        return { value.x * scale, value.y * scale, value.z * scale };
    }

    static float Field(const std::string& text, const char* name) {
        std::smatch m;
        const std::regex r(std::string("(?:^|[,\\(])") + name + R"(=([-+0-9.eE]+))");
        return std::regex_search(text, m, r) ? std::stof(m[1].str()) : 0.0f;
    }
    static DirectX::XMFLOAT3 Vec3(const std::string& text, const char* name) {
        std::smatch m;
        const std::regex r(std::string(name) +
            R"(=\(X=([-+0-9.eE]+),Y=([-+0-9.eE]+),Z=([-+0-9.eE]+)\))");
        return std::regex_search(text, m, r)
            ? DirectX::XMFLOAT3(std::stof(m[1].str()), std::stof(m[2].str()), std::stof(m[3].str()))
            : DirectX::XMFLOAT3{};
    }

    static DirectX::XMFLOAT3 Vec3Or(const std::string& text, const char* name,
                                    const DirectX::XMFLOAT3& fallback) {
        const std::string marker = std::string(name) + "=(";
        return text.find(marker) == std::string::npos ? fallback : Vec3(text, name);
    }
};
