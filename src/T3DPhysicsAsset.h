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

        const std::regex link(R"rx(ConstraintBone1="([^"]+)",ConstraintBone2="([^"]+)")rx");
        for (std::sregex_iterator it(text.begin(), text.end(), link), end; it != end; ++it)
            result.constraints.push_back({ (*it)[1].str(), (*it)[2].str() });
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
