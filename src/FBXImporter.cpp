#include "FBXImporter.h"
#include "GLBImporter.h"
#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <box3d/box3d.h>
#include <filesystem>
#include <algorithm>
#include <iostream>
#include <unordered_map>
#include <cmath>
#include <array>
#include <cfloat>
#include <functional>

using namespace DirectX;
namespace fs = std::filesystem;

std::shared_ptr<SceneNode> FBXImporter::Load(const std::string& filepath,
    Microsoft::WRL::ComPtr<ID3D12Device> device,
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> commandList,
    float uniformScale,
    bool splitIntoDestructibleBoards,
    bool loadMaterials,
    bool diffuseAndNormalOnly) {
    Assimp::Importer importer;
    const bool preserveOH1Rotors = filepath.find("OH-1") != std::string::npos;
    unsigned importFlags = aiProcess_Triangulate | aiProcess_JoinIdenticalVertices |
        aiProcess_GenSmoothNormals | aiProcess_CalcTangentSpace |
        aiProcess_ImproveCacheLocality;
    if (!preserveOH1Rotors) importFlags |= aiProcess_PreTransformVertices;
    const aiScene* scene = importer.ReadFile(filepath, importFlags);
    if (!scene || !scene->HasMeshes()) { std::cerr << "FBX load failed: " << importer.GetErrorString() << "\n"; return {}; }
    auto root = std::make_shared<SceneNode>("WoodenHouseFBX");
    root->mesh = std::make_shared<SceneMesh>();
    fs::path base = fs::path(filepath).parent_path();
    // Bake scale into vertices. Destruction path copies mesh data directly and
    // does not apply the source node transform.
    std::vector<aiMatrix4x4> meshTransforms(scene->mNumMeshes);
    if (preserveOH1Rotors) {
        std::function<void(const aiNode*, const aiMatrix4x4&)> collectTransforms;
        collectTransforms = [&](const aiNode* node, const aiMatrix4x4& parent) {
            const aiMatrix4x4 global = parent * node->mTransformation;
            for (unsigned mesh = 0; mesh < node->mNumMeshes; ++mesh)
                meshTransforms[node->mMeshes[mesh]] = global;
            for (unsigned child = 0; child < node->mNumChildren; ++child)
                collectTransforms(node->mChildren[child], global);
        };
        collectTransforms(scene->mRootNode, aiMatrix4x4());
    }

    auto loadTexture = [&](const aiString& texturePath, std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>>& uploads)
        -> Microsoft::WRL::ComPtr<ID3D12Resource> {
        std::string rawPath = texturePath.C_Str();
        std::replace(rawPath.begin(), rawPath.end(), '\\', '/');
        fs::path path(rawPath);
        if (path.is_absolute() && fs::exists(path))
            return GLBImporter::LoadTextureFromFile(path.string(), device, commandList, uploads);
        // FBX files commonly store Windows separators even on other import paths.
        path = base / fs::path(path.generic_string());
        path = path.lexically_normal();
        if (fs::exists(path))
            return GLBImporter::LoadTextureFromFile(path.string(), device, commandList, uploads);
        const fs::path filename = fs::path(rawPath).filename();
        for (const auto& entry : fs::recursive_directory_iterator(base)) {
            if (entry.is_regular_file() && entry.path().filename() == filename)
                return GLBImporter::LoadTextureFromFile(entry.path().string(), device, commandList, uploads);
        }
        return nullptr;
    };
    std::vector<fs::path> fallbackBaseColors;
    if (loadMaterials) {
        for (const auto& entry : fs::recursive_directory_iterator(base)) {
            const std::string name = entry.path().filename().string();
            const char* suffix = diffuseAndNormalOnly ? "_Diffuse." : "_BaseColor.";
            if (entry.is_regular_file() && name.find(suffix) != std::string::npos)
                fallbackBaseColors.push_back(entry.path());
        }
        std::sort(fallbackBaseColors.begin(), fallbackBaseColors.end());
    }
    uint32_t plankId = 0;
    for (unsigned mi = 0; mi < scene->mNumMeshes; ++mi) {
        const aiMesh* src = scene->mMeshes[mi];
        MeshPrimitive p;
        auto mat = std::make_shared<SceneMaterial>();
        if (loadMaterials && src->mMaterialIndex < scene->mNumMaterials) {
            const aiMaterial* am = scene->mMaterials[src->mMaterialIndex];
            aiColor4D diffuse;
            if (aiGetMaterialColor(am, AI_MATKEY_COLOR_DIFFUSE, &diffuse) == AI_SUCCESS)
                mat->baseColorFactor = XMFLOAT4(
                    diffuse.r, diffuse.g, diffuse.b, diffuse.a);
            aiString tex;
            const bool hasBaseColor = diffuseAndNormalOnly
                ? (am->GetTexture(aiTextureType_DIFFUSE, 0, &tex) == AI_SUCCESS && tex.length)
                : ((am->GetTexture(aiTextureType_BASE_COLOR, 0, &tex) == AI_SUCCESS && tex.length) ||
                   (am->GetTexture(aiTextureType_DIFFUSE, 0, &tex) == AI_SUCCESS && tex.length));
            if (hasBaseColor) {
                mat->baseColorTexture = loadTexture(tex, mat->uploadHeaps);
            }
            if ((am->GetTexture(aiTextureType_NORMALS, 0, &tex) == AI_SUCCESS && tex.length) ||
                (am->GetTexture(aiTextureType_HEIGHT, 0, &tex) == AI_SUCCESS && tex.length))
                mat->normalTexture = loadTexture(tex, mat->uploadHeaps);
            if (diffuseAndNormalOnly) {
                mat->metallicFactor = 0.0f;
                mat->roughnessFactor = 0.62f;
                mat->metallicRoughnessTexture.Reset();
            }
        }
        if (loadMaterials && !mat->baseColorTexture && !fallbackBaseColors.empty()) {
            const fs::path& color = fallbackBaseColors[mi % fallbackBaseColors.size()];
            mat->baseColorTexture = GLBImporter::LoadTextureFromFile(color.string(), device, commandList, mat->uploadHeaps);
            std::string normalName = color.filename().string();
            const std::string colorSuffix = diffuseAndNormalOnly ? "_Diffuse" : "_BaseColor";
            const size_t suffixPosition = normalName.find(colorSuffix);
            if (suffixPosition != std::string::npos)
                normalName.replace(suffixPosition, colorSuffix.size(), "_Normal");
            for (const auto& entry : fs::recursive_directory_iterator(base)) {
                if (entry.path().filename().string() == normalName) {
                    mat->normalTexture = GLBImporter::LoadTextureFromFile(entry.path().string(), device, commandList, mat->uploadHeaps);
                    break;
                }
            }
        }
        p.material = mat;
        aiMatrix3x3 normalTransform(meshTransforms[mi]);
        if (preserveOH1Rotors) normalTransform.Inverse().Transpose();
        const aiMatrix3x3 directionTransform(meshTransforms[mi]);
        for (unsigned v = 0; v < src->mNumVertices; ++v) {
            aiVector3D n = src->HasNormals() ? src->mNormals[v] : aiVector3D(0,1,0);
            aiVector3D uv = src->HasTextureCoords(0) ? src->mTextureCoords[0][v] : aiVector3D();
            aiVector3D t = src->HasTangentsAndBitangents() ? src->mTangents[v] : aiVector3D(1,0,0);
            aiVector3D position = src->mVertices[v];
            if (preserveOH1Rotors) {
                position = meshTransforms[mi] * position;
                n = normalTransform * n; n.Normalize();
                t = directionTransform * t; t.Normalize();
            }
            p.vertices.insert(p.vertices.end(), {position.x * uniformScale,
                position.y * uniformScale, position.z * uniformScale,
                n.x,n.y,n.z,uv.x,uv.y,t.x,t.y,t.z,1.0f});
        }
        for (unsigned f=0; f<src->mNumFaces; ++f)
            for (unsigned i=0; i<src->mFaces[f].mNumIndices; ++i) p.indices.push_back(src->mFaces[f].mIndices[i]);
        if (p.indices.empty()) continue;

        // Static FBX assets such as the replacement roof must retain their
        // authored topology. Splitting every connected component into random
        // plank fragments is only appropriate for destructible house assets.
        if (!splitIntoDestructibleBoards) {
            p.materialIndex = (int)src->mMaterialIndex;
            const std::string meshName = src->mName.C_Str();
            if (preserveOH1Rotors &&
                (meshName == "Rotor" || meshName == "Tail_Rotor")) {
                XMFLOAT3 minimum(FLT_MAX, FLT_MAX, FLT_MAX);
                XMFLOAT3 maximum(-FLT_MAX, -FLT_MAX, -FLT_MAX);
                for (size_t vertex = 0; vertex + 11 < p.vertices.size(); vertex += 12) {
                    minimum.x = (std::min)(minimum.x, p.vertices[vertex]);
                    minimum.y = (std::min)(minimum.y, p.vertices[vertex + 1]);
                    minimum.z = (std::min)(minimum.z, p.vertices[vertex + 2]);
                    maximum.x = (std::max)(maximum.x, p.vertices[vertex]);
                    maximum.y = (std::max)(maximum.y, p.vertices[vertex + 1]);
                    maximum.z = (std::max)(maximum.z, p.vertices[vertex + 2]);
                }
                const XMFLOAT3 pivot{
                    (minimum.x + maximum.x) * 0.5f,
                    (minimum.y + maximum.y) * 0.5f,
                    (minimum.z + maximum.z) * 0.5f };
                for (size_t vertex = 0; vertex + 11 < p.vertices.size(); vertex += 12) {
                    p.vertices[vertex] -= pivot.x;
                    p.vertices[vertex + 1] -= pivot.y;
                    p.vertices[vertex + 2] -= pivot.z;
                }
                if (GLBImporter::BuildMeshletData(p, device.Get())) {
                    auto rotorNode = std::make_shared<SceneNode>(
                        meshName == "Rotor" ? "OH1MainRotor" : "OH1TailRotor");
                    rotorNode->translation = pivot;
                    rotorNode->mesh = std::make_shared<SceneMesh>();
                    rotorNode->mesh->primitives.push_back(std::move(p));
                    root->AddChild(rotorNode);
                }
                continue;
            }
            // Wheels share the body material in the source FBX. Split their
            // authored front/rear axle regions into a dedicated black material.
            if (filepath.find("Humvee") != std::string::npos) {
                MeshPrimitive wheels;
                wheels.material = std::make_shared<SceneMaterial>();
                wheels.material->name = "HumveeWheel";
                wheels.material->baseColorFactor = XMFLOAT4(0.018f, 0.022f, 0.018f, 1.0f);
                wheels.material->metallicFactor = 0.0f;
                wheels.material->roughnessFactor = 0.92f;
                wheels.materialIndex = p.materialIndex;
                std::unordered_map<UINT, UINT> wheelRemap;
                std::vector<UINT> bodyIndices;
                bodyIndices.reserve(p.indices.size());
                for (size_t tri = 0; tri + 2 < p.indices.size(); tri += 3) {
                    float cx = 0.0f, cy = 0.0f, cz = 0.0f;
                    for (int corner = 0; corner < 3; ++corner) {
                        const size_t vertex = static_cast<size_t>(p.indices[tri + corner]) * 12;
                        cx += p.vertices[vertex];
                        cy += p.vertices[vertex + 1];
                        cz += p.vertices[vertex + 2];
                    }
                    cx /= 3.0f; cy /= 3.0f; cz /= 3.0f;
                    const bool axleRegion =
                        (cz > -660.0f * uniformScale && cz < -370.0f * uniformScale) ||
                        (cz >  240.0f * uniformScale && cz <  530.0f * uniformScale);
                    const bool wheelTriangle =
                        cy < 265.0f * uniformScale &&
                        std::abs(cx) > 180.0f * uniformScale && axleRegion;
                    if (!wheelTriangle) {
                        bodyIndices.insert(bodyIndices.end(),
                            p.indices.begin() + tri, p.indices.begin() + tri + 3);
                        continue;
                    }
                    for (int corner = 0; corner < 3; ++corner) {
                        const UINT oldIndex = p.indices[tri + corner];
                        auto [entry, inserted] = wheelRemap.emplace(
                            oldIndex, static_cast<UINT>(wheels.vertices.size() / 12));
                        if (inserted) {
                            const float* source = &p.vertices[static_cast<size_t>(oldIndex) * 12];
                            wheels.vertices.insert(wheels.vertices.end(), source, source + 12);
                        }
                        wheels.indices.push_back(entry->second);
                    }
                }
                p.indices = std::move(bodyIndices);
                if (!wheels.indices.empty() &&
                    GLBImporter::BuildMeshletData(wheels, device.Get()))
                    root->mesh->primitives.push_back(std::move(wheels));
            }
            // Humvee source is flattened into three meshes. Extract the upper
            // centre gun assembly from Body mesh into a pivoted child so runtime
            // turret yaw does not rotate the hull, wheels, or rear antenna.
            if (filepath.find("Humvee") != std::string::npos &&
                std::string(src->mName.C_Str()) == "Mesh.005") {
                constexpr float pivotX = 0.0f;
                constexpr float pivotY = 622.0f;
                constexpr float pivotZ = 82.0f;
                MeshPrimitive turret;
                turret.material = p.material;
                turret.materialIndex = p.materialIndex;
                std::unordered_map<UINT, UINT> remap;
                std::vector<UINT> hullIndices;
                hullIndices.reserve(p.indices.size());
                for (size_t tri = 0; tri + 2 < p.indices.size(); tri += 3) {
                    float cx = 0.0f, cy = 0.0f, cz = 0.0f;
                    for (int corner = 0; corner < 3; ++corner) {
                        const size_t vertex = static_cast<size_t>(p.indices[tri + corner]) * 12;
                        cx += p.vertices[vertex];
                        cy += p.vertices[vertex + 1];
                        cz += p.vertices[vertex + 2];
                    }
                    cx /= 3.0f; cy /= 3.0f; cz /= 3.0f;
                    const bool turretTriangle =
                        cy > 600.0f * uniformScale &&
                        std::abs(cx) < 150.0f * uniformScale &&
                        cz > -50.0f * uniformScale && cz < 460.0f * uniformScale;
                    if (!turretTriangle) {
                        hullIndices.insert(hullIndices.end(),
                            p.indices.begin() + tri, p.indices.begin() + tri + 3);
                        continue;
                    }
                    for (int corner = 0; corner < 3; ++corner) {
                        const UINT oldIndex = p.indices[tri + corner];
                        auto [entry, inserted] = remap.emplace(
                            oldIndex, static_cast<UINT>(turret.vertices.size() / 12));
                        if (inserted) {
                            const float* source = &p.vertices[static_cast<size_t>(oldIndex) * 12];
                            turret.vertices.insert(turret.vertices.end(), source, source + 12);
                            const size_t base = turret.vertices.size() - 12;
                            turret.vertices[base] -= pivotX * uniformScale;
                            turret.vertices[base + 1] -= pivotY * uniformScale;
                            turret.vertices[base + 2] -= pivotZ * uniformScale;
                        }
                        turret.indices.push_back(entry->second);
                    }
                }
                p.indices = std::move(hullIndices);
                if (!turret.indices.empty() &&
                    GLBImporter::BuildMeshletData(turret, device.Get())) {
                    auto turretNode = std::make_shared<SceneNode>("HumveeTurret");
                    turretNode->translation = {
                        pivotX * uniformScale, pivotY * uniformScale,
                        pivotZ * uniformScale };
                    turretNode->mesh = std::make_shared<SceneMesh>();
                    turretNode->mesh->primitives.push_back(std::move(turret));
                    root->AddChild(turretNode);
                }
            }
            if (GLBImporter::BuildMeshletData(p, device.Get()))
                root->mesh->primitives.push_back(std::move(p));
            continue;
        }

        // Whole-house fracture mode: keep original material meshes together.
        // DestructionDX12 partitions this volume with 3D Voronoi sites.
        constexpr bool fractureWholeHouse = false;
        if (fractureWholeHouse) {
            p.materialIndex = (int)src->mMaterialIndex;
            if (GLBImporter::BuildMeshletData(p, device.Get()))
                root->mesh->primitives.push_back(std::move(p));
            continue;
        }

        // The FBX house stores its boards as disconnected geometry inside shared
        // material meshes. Extract every connected board as its own child so the
        // destruction system can build local Voronoi-style bonds per plank.
        const UINT vertexCount = (UINT)(p.vertices.size() / 12);
        std::vector<UINT> parent(vertexCount);
        for (UINT i = 0; i < vertexCount; ++i) parent[i] = i;
        auto findRoot = [&](auto&& self, UINT i) -> UINT {
            return parent[i] == i ? i : parent[i] = self(self, parent[i]);
        };
        auto unite = [&](UINT a, UINT b) {
            a = findRoot(findRoot, a); b = findRoot(findRoot, b);
            if (a != b) parent[b] = a;
        };
        // UV seams duplicate vertices. Union equal positions first so they stay
        // part of the same physical plank.
        std::unordered_map<std::string, UINT> positionOwner;
        for (UINT i = 0; i < vertexCount; ++i) {
            const size_t offset = (size_t)i * 12;
            const std::string key = std::to_string((long long)std::llround(p.vertices[offset] * 100000.0f)) + ":" +
                std::to_string((long long)std::llround(p.vertices[offset + 1] * 100000.0f)) + ":" +
                std::to_string((long long)std::llround(p.vertices[offset + 2] * 100000.0f));
            const auto [it, inserted] = positionOwner.emplace(key, i);
            if (!inserted) unite(i, it->second);
        }
        for (size_t tri = 0; tri + 2 < p.indices.size(); tri += 3) {
            unite(p.indices[tri], p.indices[tri + 1]);
            unite(p.indices[tri], p.indices[tri + 2]);
        }

        struct PlankBuild {
            MeshPrimitive primitive;
            std::unordered_map<UINT, UINT> remap;
        };
        std::unordered_map<UINT, size_t> plankByRoot;
        std::vector<PlankBuild> planks;
        for (size_t tri = 0; tri + 2 < p.indices.size(); tri += 3) {
            const UINT componentRoot = findRoot(findRoot, p.indices[tri]);
            auto [it, inserted] = plankByRoot.emplace(componentRoot, planks.size());
            if (inserted) {
                planks.emplace_back();
                planks.back().primitive.material = p.material;
                planks.back().primitive.materialIndex = p.materialIndex;
            }
            PlankBuild& plank = planks[it->second];
            for (UINT oldIndex : { p.indices[tri], p.indices[tri + 1], p.indices[tri + 2] }) {
                auto [vertex, isNew] = plank.remap.emplace(oldIndex, (UINT)(plank.primitive.vertices.size() / 12));
                if (isNew) {
                    const float* sourceVertex = &p.vertices[(size_t)oldIndex * 12];
                    plank.primitive.vertices.insert(plank.primitive.vertices.end(), sourceVertex, sourceVertex + 12);
                }
                plank.primitive.indices.push_back(vertex->second);
            }
        }
        for (PlankBuild& plank : planks) {
            MeshPrimitive& sourcePlank = plank.primitive;
            if (sourcePlank.indices.empty()) continue;
            XMFLOAT3 lo(FLT_MAX, FLT_MAX, FLT_MAX), hi(-FLT_MAX, -FLT_MAX, -FLT_MAX);
            for (size_t v = 0; v + 11 < sourcePlank.vertices.size(); v += 12) {
                lo.x = (std::min)(lo.x, sourcePlank.vertices[v]); hi.x = (std::max)(hi.x, sourcePlank.vertices[v]);
                lo.y = (std::min)(lo.y, sourcePlank.vertices[v + 1]); hi.y = (std::max)(hi.y, sourcePlank.vertices[v + 1]);
                lo.z = (std::min)(lo.z, sourcePlank.vertices[v + 2]); hi.z = (std::max)(hi.z, sourcePlank.vertices[v + 2]);
            }
            const XMFLOAT3 extent((std::max)(0.001f, hi.x - lo.x), (std::max)(0.001f, hi.y - lo.y),
                                  (std::max)(0.001f, hi.z - lo.z));
            const int dominant = extent.x >= extent.y && extent.x >= extent.z ? 0 :
                                 (extent.y >= extent.z ? 1 : 2);
            const uint32_t currentPlank = plankId++;
            auto coord = [](const XMFLOAT3& p, int axis) -> float {
                return axis == 0 ? p.x : (axis == 1 ? p.y : p.z);
            };
            auto setCoord = [](XMFLOAT3& p, int axis, float value) {
                if (axis == 0) p.x = value; else if (axis == 1) p.y = value; else p.z = value;
            };
            auto hash01 = [](uint32_t seed) -> float {
                seed ^= seed >> 16; seed *= 2246822519u; seed ^= seed >> 13; seed *= 3266489917u; seed ^= seed >> 16;
                return (seed & 0x00ffffffu) / 16777215.0f;
            };
            auto addVertex = [&](MeshPrimitive& mesh, const XMFLOAT3& p, const XMFLOAT3& n,
                                 int uAxis, int vAxis, float tangentSign = 1.0f) -> UINT {
                const float u = coord(p, uAxis);
                const float v = coord(p, vAxis);
                XMFLOAT3 tangent(0, 0, 0);
                setCoord(tangent, uAxis, tangentSign);
                const float vertex[12] = { p.x, p.y, p.z, n.x, n.y, n.z, u, v,
                    tangent.x, tangent.y, tangent.z, 1.0f };
                const UINT index = (UINT)(mesh.vertices.size() / 12);
                mesh.vertices.insert(mesh.vertices.end(), vertex, vertex + 12);
                return index;
            };
            auto addTri = [&](MeshPrimitive& mesh, const XMFLOAT3& a, const XMFLOAT3& b, const XMFLOAT3& c,
                              int uAxis, int vAxis) {
                XMFLOAT3 ab(b.x - a.x, b.y - a.y, b.z - a.z);
                XMFLOAT3 ac(c.x - a.x, c.y - a.y, c.z - a.z);
                XMFLOAT3 n(ab.y * ac.z - ab.z * ac.y, ab.z * ac.x - ab.x * ac.z, ab.x * ac.y - ab.y * ac.x);
                const float len = std::sqrt(n.x * n.x + n.y * n.y + n.z * n.z);
                if (len < 0.000001f) return;
                n.x /= len; n.y /= len; n.z /= len;
                const UINT i0 = addVertex(mesh, a, n, uAxis, vAxis);
                const UINT i1 = addVertex(mesh, b, n, uAxis, vAxis);
                const UINT i2 = addVertex(mesh, c, n, uAxis, vAxis);
                mesh.indices.insert(mesh.indices.end(), { i0, i1, i2 });
            };

            const int crossA = ((dominant + 1) % 3);
            const int crossB = ((dominant + 2) % 3);
            const int trueWidthAxis = coord(extent, crossA) >= coord(extent, crossB) ? crossA : crossB;
            const int trueThicknessAxis = trueWidthAxis == crossA ? crossB : crossA;
            const float lengthLo = coord(lo, dominant), lengthHi = coord(hi, dominant);
            const float widthMid = (coord(lo, trueWidthAxis) + coord(hi, trueWidthAxis)) * 0.5f;
            const float thickMid = (coord(lo, trueThicknessAxis) + coord(hi, trueThicknessAxis)) * 0.5f;
            const float length = (std::max)(0.001f, lengthHi - lengthLo);
            const float halfWidth = (std::max)(0.001f, (coord(hi, trueWidthAxis) - coord(lo, trueWidthAxis)) * 0.5f);
            const float halfThick = (std::max)(0.001f, (coord(hi, trueThicknessAxis) - coord(lo, trueThicknessAxis)) * 0.5f);
            const int cellCount = 4 + (int)((currentPlank * 1664525u + 1013904223u) % 6u);
            std::vector<MeshPrimitive> cells((size_t)cellCount);
            for (int cellIndex = 0; cellIndex < cellCount; ++cellIndex) {
                MeshPrimitive& cell = cells[(size_t)cellIndex];
                cell.material = sourcePlank.material;
                cell.materialIndex = sourcePlank.materialIndex;

                const float base0 = (float)cellIndex / (float)cellCount;
                const float base1 = (float)(cellIndex + 1) / (float)cellCount;
                const float splitJitter0 = cellIndex == 0 ? 0.0f :
                    (hash01(currentPlank * 911u + cellIndex * 57u) - 0.5f) * 0.13f / cellCount;
                const float splitJitter1 = cellIndex == cellCount - 1 ? 0.0f :
                    (hash01(currentPlank * 1777u + cellIndex * 83u) - 0.5f) * 0.13f / cellCount;
                const float t0 = (std::max)(0.0f, (std::min)(0.96f, base0 + splitJitter0));
                const float t1 = (std::max)(t0 + 0.025f, (std::min)(1.0f, base1 + splitJitter1));
                const float sideBias = (hash01(currentPlank * 31337u + cellIndex * 101u) - 0.5f) * halfWidth * 1.15f;
                const float taper = 0.42f + hash01(currentPlank * 41u + cellIndex * 19u) * 0.55f;
                constexpr int sliceCount = 5;
                XMFLOAT3 corners[sliceCount][4]{};
                for (int s = 0; s < sliceCount; ++s) {
                    const float f = (float)s / (float)(sliceCount - 1);
                    const float along = lengthLo + (t0 + (t1 - t0) * f) * length;
                    const float bend = (hash01(currentPlank * 1013u + cellIndex * 131u + s * 17u) - 0.5f) * halfWidth * 0.75f;
                    const float chip = (hash01(currentPlank * 2027u + cellIndex * 47u + s * 29u) - 0.5f) * halfThick * 0.9f;
                    const float localHalfW = halfWidth * (0.20f + taper * (0.55f + 0.35f * std::sin((f + hash01(currentPlank + cellIndex)) * 3.14159f)));
                    const float localHalfT = halfThick * (0.75f + hash01(currentPlank * 509u + cellIndex * 7u + s * 3u) * 0.65f);
                    const float jagL = (hash01(currentPlank * 31u + cellIndex * 197u + s * 5u) - 0.5f) * localHalfW * 0.55f;
                    const float jagR = (hash01(currentPlank * 67u + cellIndex * 193u + s * 11u) - 0.5f) * localHalfW * 0.55f;
                    for (int c = 0; c < 4; ++c) {
                        XMFLOAT3 pnt(0, 0, 0);
                        setCoord(pnt, dominant, along + (hash01(currentPlank * 271u + cellIndex * 37u + s * 13u + c) - 0.5f) * length * 0.015f);
                        const float side = (c == 0 || c == 3) ? -1.0f : 1.0f;
                        const float top = c < 2 ? -1.0f : 1.0f;
                        const float edgeJag = side < 0.0f ? jagL : jagR;
                        setCoord(pnt, trueWidthAxis, widthMid + sideBias + bend + side * localHalfW + edgeJag);
                        setCoord(pnt, trueThicknessAxis, thickMid + chip + top * localHalfT);
                        corners[s][c] = pnt;
                    }
                }
                for (int s = 0; s < sliceCount - 1; ++s) {
                    addTri(cell, corners[s][0], corners[s + 1][0], corners[s + 1][1], dominant, trueWidthAxis);
                    addTri(cell, corners[s][0], corners[s + 1][1], corners[s][1], dominant, trueWidthAxis);
                    addTri(cell, corners[s][3], corners[s][2], corners[s + 1][2], dominant, trueWidthAxis);
                    addTri(cell, corners[s][3], corners[s + 1][2], corners[s + 1][3], dominant, trueWidthAxis);
                    addTri(cell, corners[s][0], corners[s][3], corners[s + 1][3], dominant, trueThicknessAxis);
                    addTri(cell, corners[s][0], corners[s + 1][3], corners[s + 1][0], dominant, trueThicknessAxis);
                    addTri(cell, corners[s][1], corners[s + 1][1], corners[s + 1][2], dominant, trueThicknessAxis);
                    addTri(cell, corners[s][1], corners[s + 1][2], corners[s][2], dominant, trueThicknessAxis);
                }
                addTri(cell, corners[0][0], corners[0][1], corners[0][2], trueWidthAxis, trueThicknessAxis);
                addTri(cell, corners[0][0], corners[0][2], corners[0][3], trueWidthAxis, trueThicknessAxis);
                addTri(cell, corners[sliceCount - 1][0], corners[sliceCount - 1][2], corners[sliceCount - 1][1], trueWidthAxis, trueThicknessAxis);
                addTri(cell, corners[sliceCount - 1][0], corners[sliceCount - 1][3], corners[sliceCount - 1][2], trueWidthAxis, trueThicknessAxis);
            }
            for (int cellIndex = 0; cellIndex < cellCount; ++cellIndex) {
                MeshPrimitive& cell = cells[(size_t)cellIndex];
                if (cell.indices.empty() || !GLBImporter::BuildMeshletData(cell, device.Get())) continue;
                auto plankNode = std::make_shared<SceneNode>("Cladding@" + std::to_string(currentPlank));
                plankNode->mesh = std::make_shared<SceneMesh>();
                plankNode->mesh->primitives.push_back(std::move(cell));
                root->AddChild(plankNode);
            }
        }
    }
    root->UpdateGlobalTransform(root->localTransform);
    return root;
}
