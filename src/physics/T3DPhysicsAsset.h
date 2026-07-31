#pragma once
#include "SkinnedTypes.h"
#include <DirectXMath.h>
#include <fstream>
#include <regex>
#include <sstream>

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
            RagdollBodySpec body; body.bone = (*it)[2].str();
            body.center = Vec3(geom, "Center"); // native skeleton units (UE cm)
            const DirectX::XMFLOAT3 euler = Vec3(geom, "Rotation");
            const float d = DirectX::XM_PI / 180.0f;
            DirectX::XMStoreFloat4(&body.rotation,
                DirectX::XMQuaternionRotationRollPitchYaw(euler.z*d, euler.x*d, euler.y*d));
            if (geom.find("BoxElems=") != std::string::npos) {
                body.shape = 0;
                std::smatch size;
                const std::regex sizePattern(
                    R"(\),X=([-+0-9.eE]+),Y=([-+0-9.eE]+),Z=([-+0-9.eE]+))");
                if (std::regex_search(geom, size, sizePattern))
                    body.halfExtent = { std::stof(size[1].str())*0.005f,
                                        std::stof(size[2].str())*0.005f,
                                        std::stof(size[3].str())*0.005f };
            } else {
                body.shape = 1;
                body.radius = Field(geom, "Radius") * 0.01f;
                body.length = Field(geom, "Length") * 0.01f;
                body.halfExtent = { body.radius, body.radius + body.length*0.5f, body.radius };
            }
            result.bodies.push_back(body);
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

            const bool swing1Locked =
                block.find("Swing1Motion=ACM_Locked") != std::string::npos;
            const bool swing2Locked =
                block.find("Swing2Motion=ACM_Locked") != std::string::npos;
            const bool swing1Free =
                block.find("Swing1Motion=ACM_Free") != std::string::npos;
            const bool swing2Free =
                block.find("Swing2Motion=ACM_Free") != std::string::npos;
            float swing1 = swing1Locked ? 2.0f :
                swing1Free ? 70.0f : Field(block, "Swing1LimitDegrees");
            float swing2 = swing2Locked ? 2.0f :
                swing2Free ? 70.0f : Field(block, "Swing2LimitDegrees");
            if (swing1 <= 0.0f) swing1 = 25.0f;
            if (swing2 <= 0.0f) swing2 = 25.0f;

            const bool twistLocked =
                block.find("TwistMotion=ACM_Locked") != std::string::npos;
            const bool twistFree =
                block.find("TwistMotion=ACM_Free") != std::string::npos;
            float twist = twistLocked ? 2.0f :
                twistFree ? 55.0f : Field(block, "TwistLimitDegrees");
            if (twist <= 0.0f) twist = 15.0f;

            const float radians = DirectX::XM_PI / 180.0f;
            link.coneAngle = (std::max)(swing1, swing2) * radians;
            link.twistAngle = twist * radians;
            result.constraints.push_back(std::move(link));
        }
        return result;
    }

private:
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
};
