#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <iostream>
#include <chrono>
#include <filesystem>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx12.h>

#include "DX12Core.h"
#include "ShaderDX12.h"
#include "VisibilityBufferDX12.h"
#include "Scene.h"
#include "ForwardRenderer.h"
#include "IdTechRenderer.h"
#include "RaytracingDX12.h"
#include "EngineUI.h"
#include "GLBImporter.h"
#include "MipGenerator.h"
#include "ShadowMapDX12.h"
#include "MeshShaderDX12.h"
#include "TerrainRendererDX12.h"
#include "SkyRendererDX12.h"
#include "OcclusionDepthDX12.h"
#include "DestructionDX12.h"
#include "FBXImporter.h"

using namespace DirectX;

// ?? globals ??????????????????????????????????????????????????????????????????
static unsigned int SCR_WIDTH  = 1280;
static unsigned int SCR_HEIGHT = 720;

static Scene               scene;
static ShaderDX12           mainShader;
MeshShaderDX12              g_meshShader;
bool                        g_useMeshShader = false;
TerrainRendererDX12         g_terrain;
DestructionDX12             g_destruction;
static SkyRendererDX12      skyRenderer;
static OcclusionDepthDX12   occlusionDepth;
static VisibilityBufferDX12 visBuffer;
static ShadowMapDX12        shadowMap;
static GeometryBuffers      geo;
static PackedGeometry       packed;
static std::shared_ptr<SceneNode> crateModel;
static std::shared_ptr<SceneNode> wallModel;
bool g_showH2Model = false;
static std::shared_ptr<SceneMaterial> floorMaterial;
static bool                 crateLoadAttempted = false;

static float lastX = SCR_WIDTH / 2.0f;
static float lastY = SCR_HEIGHT / 2.0f;
static bool  firstMouse   = true;
static bool  ignoreNextMouseMove = false;
static bool  showUI        = true;
static bool  cameraLocked  = true;
static bool  isFullscreen  = false;
static RECT  windowedRect  = {};
static DWORD windowedStyle = 0;
static float deltaTime     = 0.0f;

static ComPtr<ID3D12DescriptorHeap> imguiSrvHeap;

static std::string ResolveTexturePath(const char* relativePath) {
    if (std::filesystem::exists(relativePath)) return relativePath;
    std::string buildPath = std::string("build/") + relativePath;
    if (std::filesystem::exists(buildPath)) return buildPath;
    return relativePath;
}

static void LoadFloorMudMaterial() {
    floorMaterial = std::make_shared<SceneMaterial>();
    floorMaterial->name = "brown_mud_02";
    floorMaterial->baseColorFactor = XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f);
    floorMaterial->metallicFactor = 0.0f;
    floorMaterial->roughnessFactor = 1.0f;

    floorMaterial->baseColorTexture = GLBImporter::LoadTextureFromFile(
        ResolveTexturePath("models/Textures/brown_mud_02_diff_2k.jpg"),
        g_dx12.device, g_dx12.commandList, floorMaterial->uploadHeaps);
    floorMaterial->normalTexture = GLBImporter::LoadTextureFromFile(
        ResolveTexturePath("models/Textures/brown_mud_02_nor_gl_2k.jpg"),
        g_dx12.device, g_dx12.commandList, floorMaterial->uploadHeaps);
    floorMaterial->metallicRoughnessTexture = GLBImporter::LoadTextureFromFile(
        ResolveTexturePath("models/Textures/brown_mud_02_rough_2k.jpg"),
        g_dx12.device, g_dx12.commandList, floorMaterial->uploadHeaps);

    if (!floorMaterial->baseColorTexture) {
        floorMaterial.reset();
        std::cerr << "Brown mud floor texture unavailable; using flat floor color\n";
    }
}

// Crysis-style plank wall: the destructible is built from real structural
// pieces -- vertical wooden planks held by horizontal cross-beams -- rather
// than a uniform Voronoi field. Each plank/beam is one child chunk, so a hit
// snaps that board loose along its true edges. Grid coords (x,y,z) drive
// Blast's adjacency bonding, so touching boards stay welded until struck.
// ?? procedural material textures for the destructible house ?????????????????
namespace HouseTex {
// Cheap hash-based value noise in [0,1].
inline float Hash(int x, int y) {
    uint32_t h = (uint32_t)(x * 374761393 + y * 668265263);
    h = (h ^ (h >> 13)) * 1274126177u;
    return ((h ^ (h >> 16)) & 0xFFFFFF) / (float)0xFFFFFF;
}
inline float ValueNoise(float x, float y) {
    const int xi = (int)std::floor(x), yi = (int)std::floor(y);
    const float fx = x - xi, fy = y - yi;
    const float sx = fx * fx * (3 - 2 * fx), sy = fy * fy * (3 - 2 * fy);
    const float a = Hash(xi, yi), b = Hash(xi + 1, yi);
    const float c = Hash(xi, yi + 1), d = Hash(xi + 1, yi + 1);
    return (a + (b - a) * sx) + ((c + (d - c) * sx) - (a + (b - a) * sx)) * sy;
}
inline float Fbm(float x, float y) {
    float sum = 0, amp = 0.5f, freq = 1;
    for (int o = 0; o < 4; ++o) { sum += ValueNoise(x * freq, y * freq) * amp; freq *= 2; amp *= 0.5f; }
    return sum;
}
inline unsigned char ToByte(float v) { return (unsigned char)std::max(0.0f, std::min(255.0f, v * 255.0f + 0.5f)); }

// Wood: vertical grain lines along V with warped rings and knots.
inline std::vector<unsigned char> Wood(int size, XMFLOAT3 base, XMFLOAT3 dark) {
    std::vector<unsigned char> px((size_t)size * size * 4);
    for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x) {
        const float u = (float)x / size, v = (float)y / size;
        const float warp = Fbm(u * 3.0f, v * 12.0f) * 0.35f;
        float grain = std::sin((u * 18.0f + warp) * 3.14159f);
        grain = 0.5f + 0.5f * grain * grain;                 // sharpen streaks
        grain = grain * 0.7f + Fbm(u * 40.0f, v * 6.0f) * 0.3f;
        const float t = std::min(1.0f, grain);
        const size_t i = ((size_t)y * size + x) * 4;
        px[i + 0] = ToByte(dark.x + (base.x - dark.x) * t);
        px[i + 1] = ToByte(dark.y + (base.y - dark.y) * t);
        px[i + 2] = ToByte(dark.z + (base.z - dark.z) * t);
        px[i + 3] = 255;
    }
    return px;
}
// Stone: blocky mortar grid with speckled fill.
inline std::vector<unsigned char> Stone(int size, XMFLOAT3 base, XMFLOAT3 mortar) {
    std::vector<unsigned char> px((size_t)size * size * 4);
    for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x) {
        const float u = (float)x / size * 4.0f, v = (float)y / size * 4.0f;
        const float bx = u - std::floor(u), by = v - std::floor(v);
        const float mortarLine = std::min(std::min(bx, 1 - bx), std::min(by, 1 - by));
        const float m = mortarLine < 0.06f ? 0.0f : 1.0f;
        const float speck = 0.6f + 0.4f * Fbm(u * 8.0f, v * 8.0f);
        const XMFLOAT3 c = { base.x * speck, base.y * speck, base.z * speck };
        const size_t i = ((size_t)y * size + x) * 4;
        px[i + 0] = ToByte(mortar.x + (c.x - mortar.x) * m);
        px[i + 1] = ToByte(mortar.y + (c.y - mortar.y) * m);
        px[i + 2] = ToByte(mortar.z + (c.z - mortar.z) * m);
        px[i + 3] = 255;
    }
    return px;
}
// Shingles: overlapping horizontal rows, staggered, with edge shadow.
inline std::vector<unsigned char> Shingle(int size, XMFLOAT3 base, XMFLOAT3 dark) {
    std::vector<unsigned char> px((size_t)size * size * 4);
    for (int y = 0; y < size; ++y) for (int x = 0; x < size; ++x) {
        const float u = (float)x / size, v = (float)y / size;
        const float row = v * 10.0f;
        const int ri = (int)std::floor(row);
        const float rf = row - ri;
        const float offset = (ri & 1) ? 0.5f : 0.0f;
        const float col = (u * 8.0f + offset);
        const float cf = col - std::floor(col);
        float shade = 1.0f - rf * 0.35f;                     // top-lit row
        if (cf < 0.04f || rf > 0.94f) shade *= 0.55f;         // shingle gaps
        shade *= 0.85f + 0.15f * Fbm(u * 20.0f, v * 20.0f);
        const size_t i = ((size_t)y * size + x) * 4;
        px[i + 0] = ToByte(dark.x + (base.x - dark.x) * shade);
        px[i + 1] = ToByte(dark.y + (base.y - dark.y) * shade);
        px[i + 2] = ToByte(dark.z + (base.z - dark.z) * shade);
        px[i + 3] = 255;
    }
    return px;
}
}  // namespace HouseTex

// Assigns downloaded CC0 albedo textures (ambientCG, models/house_pbr) to the
// house's shared materials by name, falling back to procedurally generated
// wood/stone/shingle when a file is missing. Call after building the house,
// while a command list is open for the texture uploads.
static void ApplyHouseTextures(const std::shared_ptr<SceneNode>& house,
                               ID3D12Device* device, ID3D12GraphicsCommandList* cmdList) {
    if (!house) return;
    constexpr int kSize = 256;
    // Collect the unique materials by name from the house children.
    std::unordered_map<std::string, std::shared_ptr<SceneMaterial>> mats;
    for (const auto& child : house->children) {
        if (!child || !child->mesh) continue;
        for (const auto& prim : child->mesh->primitives)
            if (prim.material) mats[prim.material->name] = prim.material;
    }
    // Load the downloaded albedo + normal maps. Albedo falls back to the
    // procedural texture so the house never renders untextured; the normal map
    // is optional (skipped if the file is absent).
    auto assign = [&](const char* name, const std::string& base, std::vector<unsigned char> fallback) {
        auto it = mats.find(name);
        if (it == mats.end()) return;
        auto& mat = it->second;
        mat->baseColorTexture = GLBImporter::LoadTextureFromFile(base + ".jpg", device, cmdList, mat->uploadHeaps);
        if (!mat->baseColorTexture) {
            mat->baseColorTexture = GLBImporter::CreateTextureFromRGBA(
                device, cmdList, fallback, kSize, kSize, mat->uploadHeaps);
        }
        if (mat->baseColorTexture) mat->baseColorFactor = XMFLOAT4(1, 1, 1, 1);
        mat->normalTexture = GLBImporter::LoadTextureFromFile(base + "_normal.jpg", device, cmdList, mat->uploadHeaps);
        // Roughness map: grayscale JPG whose G channel the shader reads as
        // roughness (glTF metallic-roughness convention). Metal stays 0.
        mat->metallicRoughnessTexture = GLBImporter::LoadTextureFromFile(
            base + "_roughness.jpg", device, cmdList, mat->uploadHeaps);
        if (mat->metallicRoughnessTexture) { mat->metallicFactor = 0.0f; mat->roughnessFactor = 1.0f; }
    };
    assign("Foundation", "models/house_pbr/foundation_brick",
           HouseTex::Stone(kSize, { 0.62f, 0.62f, 0.64f }, { 0.34f, 0.34f, 0.36f }));
    assign("Stud", "models/house_pbr/stud_wood",
           HouseTex::Wood(kSize, { 0.60f, 0.42f, 0.25f }, { 0.34f, 0.22f, 0.12f }));
    // Cladding uses the single-board wood (not the multi-plank field) so the
    // grain reads as real boards when tiled.
    assign("Cladding", "models/house_pbr/stud_wood",
           HouseTex::Wood(kSize, { 0.84f, 0.68f, 0.46f }, { 0.55f, 0.40f, 0.24f }));
    assign("Roof", "models/house_pbr/roof_tiles",
           HouseTex::Shingle(kSize, { 0.52f, 0.26f, 0.20f }, { 0.26f, 0.12f, 0.10f }));
}

// Basic modular destructible house built from real structural pieces: a
// foundation slab and floor, four walls made of vertical studs + cladding,
// door/window openings, and a two-slope roof of rafters + sheets. Each piece
// is one child chunk. Pieces whose name starts with "Support:" are anchored to
// the world by the destruction layer, so foundation and bottom wall plates
// stay static and hold the structure up; disconnected sections fall. Blast
// bonds touching pieces so a hit tears loose only what it structurally frees.
static std::shared_ptr<SceneNode> CreateDestructibleWallModel() {
    // House footprint (world units). Front faces +Z toward the spawn area.
    constexpr float minX = 4.0f, maxX = 11.0f;   // width
    constexpr float minZ = 1.0f, maxZ = 6.0f;    // depth
    constexpr float floorY = 0.0f, wallTop = 3.4f;
    constexpr float wall = 0.28f;                // wall / slab thickness

    auto root = std::make_shared<SceneNode>("DestructibleHouse");
    auto matFoundation = std::make_shared<SceneMaterial>();
    matFoundation->name = "Foundation";
    matFoundation->baseColorFactor = XMFLOAT4(0.55f, 0.55f, 0.58f, 1.0f);
    matFoundation->metallicFactor = 0.0f; matFoundation->roughnessFactor = 0.95f;
    auto matStud = std::make_shared<SceneMaterial>();
    matStud->name = "Stud";
    matStud->baseColorFactor = XMFLOAT4(0.58f, 0.40f, 0.24f, 1.0f);
    matStud->metallicFactor = 0.0f; matStud->roughnessFactor = 0.88f;
    auto matCladding = std::make_shared<SceneMaterial>();
    matCladding->name = "Cladding";
    matCladding->baseColorFactor = XMFLOAT4(0.82f, 0.66f, 0.44f, 1.0f);
    matCladding->metallicFactor = 0.0f; matCladding->roughnessFactor = 0.85f;
    auto matRoof = std::make_shared<SceneMaterial>();
    matRoof->name = "Roof";
    matRoof->baseColorFactor = XMFLOAT4(0.45f, 0.22f, 0.18f, 1.0f);
    matRoof->metallicFactor = 0.0f; matRoof->roughnessFactor = 0.80f;

    // Emit one axis-aligned solid box as a chunk child. `wrap` stretches the
    // texture to span the whole piece (UV 0..1 per face) so a single-board wood
    // texture reads as one plank; otherwise UVs map by world X/Y (tiling).
    auto addBox = [&](const char* name, const std::shared_ptr<SceneMaterial>& material,
                      float x0, float x1, float y0, float y1, float z0, float z1,
                      bool wrap = false) {
        if (x1 <= x0 || y1 <= y0 || z1 <= z0) return;
        auto node = std::make_shared<SceneNode>(name);
        node->mesh = std::make_shared<SceneMesh>();
        MeshPrimitive primitive;
        primitive.material = material;
        constexpr float kUvScale = 1.5f;  // world units per texture tile
        const float extX = x1 - x0, extY = y1 - y0, extZ = z1 - z0;
        auto emitQuad = [&](const XMFLOAT3& a, const XMFLOAT3& b, const XMFLOAT3& c,
                            const XMFLOAT3& d, const XMFLOAT3& n) {
            const UINT base = (UINT)(primitive.vertices.size() / 12);
            const XMFLOAT3 pts[4] = { a, b, c, d };
            const bool faceX = std::abs(n.x) > 0.5f;
            const bool faceY = std::abs(n.y) > 0.5f;
            for (const XMFLOAT3& p : pts) {
                float u, v;
                if (wrap) {
                    // One texture span across the whole board. Long axis -> U so
                    // the plank grain runs along the board's length.
                    if (faceX)      { u = (p.z - z0) / extZ; v = (p.y - y0) / extY; }
                    else if (faceY) { u = (p.x - x0) / extX; v = (p.z - z0) / extZ; }
                    else            { u = (p.x - x0) / extX; v = (p.y - y0) / extY; }
                } else {
                    // World-scaled tiling per face -> uniform texel size, no
                    // stretching whatever the face orientation.
                    if (faceX)      { u = p.z; v = p.y; }
                    else if (faceY) { u = p.x; v = p.z; }
                    else            { u = p.x; v = p.y; }
                    u /= kUvScale; v /= kUvScale;
                }
                // Tangent must lie in the face and follow U, and must NOT be
                // parallel to the normal -- a flat (1,0,0) on an X-facing side
                // collapses the TBN to zero and the normal map samples as noise.
                const XMFLOAT3 tangent = faceX ? XMFLOAT3(0, 0, 1)   // U runs along Z
                                       : XMFLOAT3(1, 0, 0);           // U runs along X
                const float vertex[12] = { p.x,p.y,p.z, n.x,n.y,n.z, u,v,
                                           tangent.x, tangent.y, tangent.z, 1 };
                primitive.vertices.insert(primitive.vertices.end(), vertex, vertex + 12);
            }
            primitive.indices.insert(primitive.indices.end(),
                { base, base + 1, base + 2, base, base + 2, base + 3 });
        };
        emitQuad({x0,y0,z1},{x1,y0,z1},{x1,y1,z1},{x0,y1,z1}, {0,0,1});    // front (+Z)
        emitQuad({x1,y0,z0},{x0,y0,z0},{x0,y1,z0},{x1,y1,z0}, {0,0,-1});   // back
        emitQuad({x0,y0,z0},{x0,y0,z1},{x0,y1,z1},{x0,y1,z0}, {-1,0,0});   // left
        emitQuad({x1,y0,z1},{x1,y0,z0},{x1,y1,z0},{x1,y1,z1}, {1,0,0});    // right
        emitQuad({x0,y1,z1},{x1,y1,z1},{x1,y1,z0},{x0,y1,z0}, {0,1,0});    // top
        emitQuad({x0,y0,z0},{x1,y0,z0},{x1,y0,z1},{x0,y0,z1}, {0,-1,0});   // bottom
        node->mesh->primitives.push_back(std::move(primitive));
        root->AddChild(node);
    };

    // Emit one board as a cluster of Voronoi prism cells. The board's largest
    // face is split by a jittered-site Voronoi diagram (half-plane clipping);
    // each convex cell is extruded through the board thickness into its own
    // chunk child. All cells share `name` (with an "@<id>" group suffix) so the
    // destruction layer bonds them into one piece -- the seams only show once
    // the board breaks apart, and they are jagged rather than straight cuts.
    // Axes are derived from the box: thickness = thinnest extent, the Voronoi
    // plane spans the two remaining axes (L = longer of the two, H = other).
    auto addVoronoiBoard = [&](const char* name, const std::shared_ptr<SceneMaterial>& material,
                               float x0, float x1, float y0, float y1,
                               float z0, float z1, int seed) {
        if (x1 <= x0 || y1 <= y0 || z1 <= z0) return;
        const float lo[3] = { x0, y0, z0 }, hi[3] = { x1, y1, z1 };
        const float ext[3] = { x1 - x0, y1 - y0, z1 - z0 };
        const int tAxis = (ext[0] <= ext[1] && ext[0] <= ext[2]) ? 0
                        : (ext[1] <= ext[2] ? 1 : 2);
        const int r0 = tAxis == 0 ? 1 : 0, r1 = tAxis == 2 ? 1 : 2;
        const int lAxis = ext[r0] >= ext[r1] ? r0 : r1;
        const int hAxis = lAxis == r0 ? r1 : r0;
        const float l0 = lo[lAxis], l1 = hi[lAxis];
        const float h0 = lo[hAxis], h1 = hi[hAxis];
        const float t0 = lo[tAxis], t1 = hi[tAxis];
        struct P2 { float x, y; };
        const float length = l1 - l0;
        const int cols = std::max(3, std::min(8, (int)std::lround(length / 1.2f)));
        const float cellW = length / cols;
        // Deterministic integer-hash jitter (no RNG); the piece id seeds it so
        // every board breaks along different lines.
        std::vector<P2> sites;
        for (int i = 0; i < cols; ++i) {
            const float jx = (((i * 37 + seed * 17 + 3) % 13) / 12.0f - 0.5f) * cellW * 0.9f;
            const float jy = (((i * 19 + seed * 41 + 7) % 11) / 10.0f - 0.5f) * (h1 - h0) * 0.8f;
            sites.push_back({ l0 + (i + 0.5f) * cellW + jx, (h0 + h1) * 0.5f + jy });
        }
        // Voronoi cell = board rect clipped against the perpendicular bisector
        // of every other site (Sutherland-Hodgman). Result is convex and CCW.
        auto clipCell = [&](size_t s) {
            std::vector<P2> poly = { {l0,h0},{l1,h0},{l1,h1},{l0,h1} };
            for (size_t o = 0; o < sites.size() && !poly.empty(); ++o) {
                if (o == s) continue;
                const float nx = sites[o].x - sites[s].x, ny = sites[o].y - sites[s].y;
                const float c = (sites[o].x * sites[o].x + sites[o].y * sites[o].y
                               - sites[s].x * sites[s].x - sites[s].y * sites[s].y) * 0.5f;
                std::vector<P2> out;
                for (size_t i = 0; i < poly.size(); ++i) {
                    const P2 pa = poly[i], pb = poly[(i + 1) % poly.size()];
                    const float da = pa.x * nx + pa.y * ny - c;
                    const float db = pb.x * nx + pb.y * ny - c;
                    const bool ia = da <= 0.00001f, ib = db <= 0.00001f;
                    if (ia) out.push_back(pa);
                    if (ia != ib) {
                        const float t = da / (da - db);
                        out.push_back({ pa.x + (pb.x - pa.x) * t, pa.y + (pb.y - pa.y) * t });
                    }
                }
                poly.swap(out);
            }
            return poly;
        };
        auto toWorld = [&](const P2& p, float t) {
            float w[3]; w[lAxis] = p.x; w[hAxis] = p.y; w[tAxis] = t;
            return XMFLOAT3(w[0], w[1], w[2]);
        };
        auto axisUnit = [](int axis) {
            return XMFLOAT3(axis == 0 ? 1.0f : 0.0f, axis == 1 ? 1.0f : 0.0f,
                            axis == 2 ? 1.0f : 0.0f);
        };
        for (size_t s = 0; s < sites.size(); ++s) {
            const std::vector<P2> poly = clipCell(s);
            if (poly.size() < 3) continue;
            auto node = std::make_shared<SceneNode>(name);
            node->mesh = std::make_shared<SceneMesh>();
            MeshPrimitive prim;
            prim.material = material;
            constexpr float kUvScale = 1.5f;  // world units per texture tile
            auto emitTri = [&](XMFLOAT3 ta, XMFLOAT3 tb, XMFLOAT3 tc,
                               const XMFLOAT3& n, const XMFLOAT3& tan) {
                // Fix winding so the triangle faces its lighting normal.
                const XMVECTOR geometric = XMVector3Cross(
                    XMVectorSubtract(XMLoadFloat3(&tb), XMLoadFloat3(&ta)),
                    XMVectorSubtract(XMLoadFloat3(&tc), XMLoadFloat3(&ta)));
                if (XMVectorGetX(XMVector3Dot(geometric, XMLoadFloat3(&n))) < 0.0f)
                    std::swap(tb, tc);
                const UINT base = (UINT)(prim.vertices.size() / 12);
                const XMFLOAT3 pts[3] = { ta, tb, tc };
                const bool faceX = std::abs(n.x) > 0.5f, faceY = std::abs(n.y) > 0.5f;
                for (const XMFLOAT3& p : pts) {
                    // World-scaled tiling per dominant face axis (same rule as
                    // addBox) -> uniform texel size on the jagged side walls.
                    float u, v;
                    if (faceX)      { u = p.z; v = p.y; }
                    else if (faceY) { u = p.x; v = p.z; }
                    else            { u = p.x; v = p.y; }
                    u /= kUvScale; v /= kUvScale;
                    const float vert[12] = { p.x,p.y,p.z, n.x,n.y,n.z, u,v,
                                             tan.x,tan.y,tan.z, 1 };
                    prim.vertices.insert(prim.vertices.end(), vert, vert + 12);
                }
                prim.indices.insert(prim.indices.end(), { base, base + 1, base + 2 });
            };
            P2 centroid{ 0.0f, 0.0f };
            for (const P2& p : poly) { centroid.x += p.x; centroid.y += p.y; }
            centroid.x /= (float)poly.size(); centroid.y /= (float)poly.size();
            const XMFLOAT3 capN = axisUnit(tAxis);
            const XMFLOAT3 capNeg(-capN.x, -capN.y, -capN.z);
            const XMFLOAT3 capTan = axisUnit(lAxis);   // in the cap plane
            const XMFLOAT3 sideTan = capN;  // extrusion axis lies in every side face
            for (size_t i = 0; i < poly.size(); ++i) {
                const P2 a2 = poly[i], b2 = poly[(i + 1) % poly.size()];
                // Caps: fan around the centroid on both thickness faces.
                emitTri(toWorld(centroid, t1), toWorld(a2, t1), toWorld(b2, t1), capN, capTan);
                emitTri(toWorld(centroid, t0), toWorld(a2, t0), toWorld(b2, t0), capNeg, capTan);
                // Side wall for this edge; outward normal from the CCW polygon.
                const float ex = b2.x - a2.x, ey = b2.y - a2.y;
                const float elen = std::sqrt(ex * ex + ey * ey);
                if (elen < 0.0001f) continue;
                const P2 sn2{ ey / elen, -ex / elen };     // outward in (L, H)
                const XMFLOAT3 snEnd = toWorld(sn2, 0.0f); // map plane dir to world
                const XMFLOAT3 snOrg = toWorld({ 0, 0 }, 0.0f);
                const XMFLOAT3 sn(snEnd.x - snOrg.x, snEnd.y - snOrg.y, snEnd.z - snOrg.z);
                emitTri(toWorld(a2, t0), toWorld(b2, t0), toWorld(b2, t1), sn, sideTan);
                emitTri(toWorld(a2, t0), toWorld(b2, t1), toWorld(a2, t1), sn, sideTan);
            }
            node->mesh->primitives.push_back(std::move(prim));
            root->AddChild(node);
        }
    };

    // --- Foundation: single anchored slab spanning the footprint. ---
    addBox("Support:Foundation", matFoundation, minX, maxX, floorY, floorY + wall, minZ, maxZ);

    // --- Studded wall: vertical studs + outer cladding along one edge. The
    // bottom row of studs is anchored (the sill plate), so the wall stands. ---
    int pieceId = 0;  // unique Voronoi group id per board across the house
    auto buildWall = [&](float x0, float x1, float z0, float z1, bool alongX,
                         float openStart, float openEnd, float openTop) {
        constexpr float studW = 0.16f;
        constexpr int studCount = 8;
        const float baseY = floorY + wall;                 // sit on foundation
        const float span = alongX ? (x1 - x0) : (z1 - z0);
        for (int s = 0; s <= studCount; ++s) {
            const float t = (float)s / studCount;
            const float c = t * span;
            // Skip studs that fall inside the opening (door/window gap).
            const bool inOpening = openEnd > openStart && c > openStart && c < openEnd;
            if (s == 0) {
                // Sill stud: anchored support, never breaks -- keep a plain box.
                if (alongX) {
                    addBox("Support:SillStud", matStud, x0 - studW * 0.5f + c, x0 + studW * 0.5f + c,
                           baseY, wallTop, z0, z1);
                } else {
                    addBox("Support:SillStud", matStud, x0, x1, baseY, wallTop,
                           z0 + c - studW * 0.5f, z0 + c + studW * 0.5f);
                }
                continue;
            }
            const int id = pieceId++;
            const std::string studName = "Stud@" + std::to_string(id);
            if (alongX) {
                const float sx = x0 + c;
                if (inOpening) {
                    // Header stub above the opening keeps the wall continuous up top.
                    addVoronoiBoard(studName.c_str(), matStud, sx - studW * 0.5f, sx + studW * 0.5f,
                                    openTop, wallTop, z0, z1, id);
                } else {
                    addVoronoiBoard(studName.c_str(), matStud, sx - studW * 0.5f, sx + studW * 0.5f,
                                    baseY, wallTop, z0, z1, id);
                }
            } else {
                const float sz = z0 + c;
                addVoronoiBoard(studName.c_str(), matStud, x0, x1, baseY, wallTop,
                                sz - studW * 0.5f, sz + studW * 0.5f, id);
            }
        }
        // Cladding: horizontal planks over the studs. Each plank is one visible
        // board built from flush Voronoi prism cells sharing one group id, so a
        // hit knocks a jagged cell out of the board instead of a straight strip.
        constexpr int boards = 5;     // planks stacked up the wall
        const float cladT = 0.08f;
        for (int b = 0; b < boards; ++b) {
            const float by0 = baseY + (wallTop - baseY) * b / boards;
            const float by1 = baseY + (wallTop - baseY) * (b + 1) / boards;
            const int id = pieceId++;
            const std::string plankName = "Cladding@" + std::to_string(id);
            if (alongX) {
                addVoronoiBoard(plankName.c_str(), matCladding, x0, x1, by0, by1,
                                z1, z1 + cladT, id);
            } else {
                addVoronoiBoard(plankName.c_str(), matCladding, x1, x1 + cladT, by0, by1,
                                z0, z1, id);
            }
        }
    };

    // Front wall (+Z) with a door opening in the middle; other three solid.
    buildWall(minX, maxX, maxZ - wall, maxZ, true, (maxX - minX) * 0.42f, (maxX - minX) * 0.58f, 2.2f);
    buildWall(minX, maxX, minZ, minZ + wall, true, 0.0f, 0.0f, 0.0f);          // back
    buildWall(minX, minX + wall, minZ, maxZ, false, 0.0f, 0.0f, 0.0f);         // left
    buildWall(maxX - wall, maxX, minZ, maxZ, false, 0.0f, 0.0f, 0.0f);         // right

    // --- Roof: two sloped slabs of rafters meeting at a ridge. Modelled as
    // stepped boxes so they remain axis-aligned solids for collision. ---
    const float ridgeY = wallTop + 1.1f;
    const float midX = (minX + maxX) * 0.5f;
    constexpr int roofSteps = 6;
    for (int s = 0; s < roofSteps; ++s) {
        const float t0 = (float)s / roofSteps, t1 = (float)(s + 1) / roofSteps;
        const float y0 = wallTop + (ridgeY - wallTop) * t0;
        // Steps overlap the next one enough that even a single Voronoi cell's
        // share of the seam clears the bond's minContactArea, so the upper roof
        // stays attached to the lower steps.
        const float y1 = wallTop + (ridgeY - wallTop) * t1 + 0.06f;
        // left slope: from eave (minX) rising to ridge (midX)
        const int leftId = pieceId++;
        addVoronoiBoard(("Roof@" + std::to_string(leftId)).c_str(), matRoof,
                        minX + (midX - minX) * t0, minX + (midX - minX) * t1,
                        y0, y1, minZ - 0.2f, maxZ + 0.2f, leftId);
        // right slope
        const int rightId = pieceId++;
        addVoronoiBoard(("Roof@" + std::to_string(rightId)).c_str(), matRoof,
                        maxX - (maxX - midX) * t1, maxX - (maxX - midX) * t0,
                        y0, y1, minZ - 0.2f, maxZ + 0.2f, rightId);
    }

    root->UpdateGlobalTransform(root->localTransform);
    return root;
}

// Draw Blast/Box3D destruction state as a 2D overlay using ImGui's foreground
// draw list: chunk AABBs coloured by role, bonds (green healthy / red severed),
// and the last hit sphere. No new pipeline needed -- just project to screen.
static void DrawDestructionDebug(Scene& scene) {
    if (!scene.showDestructionDebug || !g_destruction.IsInitialized()) return;
    const DestructionDebugData data = g_destruction.GetDebugData();

    const XMMATRIX viewProj = scene.GetViewMatrix() * scene.GetProjectionMatrix();
    const ImGuiIO& io = ImGui::GetIO();
    const float w = io.DisplaySize.x, h = io.DisplaySize.y;
    ImDrawList* dl = ImGui::GetForegroundDrawList();

    // World -> screen; returns false when behind the camera.
    auto project = [&](const XMFLOAT3& p, ImVec2& out) -> bool {
        XMVECTOR clip = XMVector3Transform(XMLoadFloat3(&p), viewProj);
        const float cw = XMVectorGetW(clip);
        if (cw <= 0.0001f) return false;
        const float ndcX = XMVectorGetX(clip) / cw, ndcY = XMVectorGetY(clip) / cw;
        out = ImVec2((ndcX * 0.5f + 0.5f) * w, (1.0f - (ndcY * 0.5f + 0.5f)) * h);
        return true;
    };

    // Wireframe box from world-space AABB corners.
    auto drawBox = [&](const XMFLOAT3& lo, const XMFLOAT3& hi, ImU32 color) {
        const XMFLOAT3 c[8] = {
            {lo.x,lo.y,lo.z},{hi.x,lo.y,lo.z},{hi.x,hi.y,lo.z},{lo.x,hi.y,lo.z},
            {lo.x,lo.y,hi.z},{hi.x,lo.y,hi.z},{hi.x,hi.y,hi.z},{lo.x,hi.y,hi.z} };
        ImVec2 s[8]; bool ok = true;
        for (int i = 0; i < 8; ++i) ok = project(c[i], s[i]) && ok;
        if (!ok) return;
        const int edges[12][2] = { {0,1},{1,2},{2,3},{3,0},{4,5},{5,6},{6,7},{7,4},
                                   {0,4},{1,5},{2,6},{3,7} };
        for (auto& e : edges) dl->AddLine(s[e[0]], s[e[1]], color, 1.2f);
    };

    // Chunks: yellow = anchored support, cyan = dynamic (falling), grey = static.
    for (const DestructionDebugChunk& chunk : data.chunks) {
        const ImU32 color = chunk.support ? IM_COL32(255, 215, 0, 200)
                          : chunk.dynamic ? IM_COL32(0, 220, 255, 180)
                                          : IM_COL32(150, 150, 150, 110);
        drawBox(chunk.worldMin, chunk.worldMax, color);
    }

    // Bonds coloured by live health: full green -> yellow -> red as it drains.
    // Skip severed bonds whose chunks have drifted apart (once pieces fall their
    // world centres scatter and the lines sprawl across the scene as noise);
    // only show a severed bond while its two chunks are still close.
    for (const DestructionDebugBond& bond : data.bonds) {
        if (bond.broken) {
            const float dx = bond.a.x - bond.b.x, dy = bond.a.y - bond.b.y, dz = bond.a.z - bond.b.z;
            if (dx * dx + dy * dy + dz * dz > 1.5f * 1.5f) continue;
        }
        ImVec2 a, b;
        if (!project(bond.a, a) || !project(bond.b, b)) continue;
        ImU32 color;
        float thickness;
        if (bond.broken) {
            color = IM_COL32(255, 40, 40, 220); thickness = 1.0f;
        } else {
            // Health fraction f: f=1 green (0,255,60), f=0 red (255,40,40).
            const float f = bond.healthFraction;
            const int r = (int)(255 * (1.0f - f) + 40 * f);
            const int g = (int)(60 * (1.0f - f) + 255 * f);
            color = IM_COL32(r, g, 60, 210);
            thickness = 1.5f + f;  // healthier = thicker
        }
        dl->AddLine(a, b, color, thickness);
        // Label weakened (but not broken) bonds with their remaining health.
        if (!bond.broken && bond.healthFraction < 0.99f) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%.2f", bond.health);
            const ImVec2 mid((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
            dl->AddText(mid, IM_COL32(255, 255, 255, 230), buf);
        }
    }

    // Last hit: magenta ring at the impact, radius projected roughly to screen.
    if (data.hasHit) {
        ImVec2 center;
        if (project(data.lastHit, center)) {
            const XMFLOAT3 edge = { data.lastHit.x + data.hitRadius, data.lastHit.y, data.lastHit.z };
            ImVec2 edgePt;
            float pixelRadius = 8.0f;
            if (project(edge, edgePt)) {
                const float dx = edgePt.x - center.x, dy = edgePt.y - center.y;
                pixelRadius = std::max(4.0f, std::sqrt(dx * dx + dy * dy));
            }
            dl->AddCircle(center, pixelRadius, IM_COL32(255, 0, 255, 230), 24, 2.0f);
            dl->AddCircleFilled(center, 4.0f, IM_COL32(255, 0, 255, 255));
        }
    }

    // Stats readout.
    ImGui::SetNextWindowBgAlpha(0.75f);
    if (ImGui::Begin("Blast Debug", &scene.showDestructionDebug,
                     ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("chunks:  %zu", data.chunks.size());
        ImGui::Text("bonds:   %zu", data.bonds.size());
        size_t broken = 0, weakened = 0;
        float minHealth = FLT_MAX, sumHealth = 0.0f;
        for (const auto& bond : data.bonds) {
            if (bond.broken) { ++broken; continue; }
            sumHealth += bond.health;
            minHealth = std::min(minHealth, bond.health);
            if (bond.healthFraction < 0.99f) ++weakened;
        }
        const size_t intact = data.bonds.size() - broken;
        ImGui::Text("severed:  %zu", broken);
        ImGui::Text("weakened: %zu", weakened);
        ImGui::Text("health:   min %.2f  avg %.2f",
                    intact ? minHealth : 0.0f, intact ? sumHealth / intact : 0.0f);
        ImGui::Text("actors:  %u  (dynamic %u)", data.actorCount, data.dynamicActorCount);
        if (data.hasHit)
            ImGui::Text("last hit: %.2f %.2f %.2f  r=%.2f",
                        data.lastHit.x, data.lastHit.y, data.lastHit.z, data.hitRadius);
        ImGui::Separator();
        ImGui::TextColored(ImVec4(1, 0.84f, 0, 1), "yellow = support (anchored)");
        ImGui::TextColored(ImVec4(0, 0.86f, 1, 1), "cyan   = dynamic (falling)");
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1), "grey   = static");
        ImGui::TextColored(ImVec4(0.24f, 1, 0.35f, 1), "bond: green = full health");
        ImGui::TextColored(ImVec4(1, 0.84f, 0.24f, 1), "bond: yellow = damaged");
        ImGui::TextColored(ImVec4(1, 0.24f, 0.24f, 1), "bond: red = severed");
    }
    ImGui::End();
}

// ?? timer ????????????????????????????????????????????????????????????????????
class Timer {
    std::chrono::high_resolution_clock::time_point t0;
public:
    void  Start()      { t0 = std::chrono::high_resolution_clock::now(); }
    float GetElapsed() {
        return std::chrono::duration<float>(
            std::chrono::high_resolution_clock::now() - t0).count();
    }
};
static Timer gameTimer;

// ?? forward decls ????????????????????????????????????????????????????????????
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

// ?? geometry creation ????????????????????????????????????????????????????????
static bool CreateAllGeometry() {
    std::vector<VertexPosNormUV> cubeVerts = {
        // Front
        {{-0.5f,-0.5f, 0.5f},{ 0, 0, 1},{0,0}}, {{ 0.5f,-0.5f, 0.5f},{ 0, 0, 1},{1,0}},
        {{ 0.5f, 0.5f, 0.5f},{ 0, 0, 1},{1,1}}, {{-0.5f,-0.5f, 0.5f},{ 0, 0, 1},{0,0}},
        {{ 0.5f, 0.5f, 0.5f},{ 0, 0, 1},{1,1}}, {{-0.5f, 0.5f, 0.5f},{ 0, 0, 1},{0,1}},
        // Back
        {{ 0.5f,-0.5f,-0.5f},{ 0, 0,-1},{0,0}}, {{-0.5f,-0.5f,-0.5f},{ 0, 0,-1},{1,0}},
        {{-0.5f, 0.5f,-0.5f},{ 0, 0,-1},{1,1}}, {{ 0.5f,-0.5f,-0.5f},{ 0, 0,-1},{0,0}},
        {{-0.5f, 0.5f,-0.5f},{ 0, 0,-1},{1,1}}, {{ 0.5f, 0.5f,-0.5f},{ 0, 0,-1},{0,1}},
        // Top
        {{-0.5f, 0.5f, 0.5f},{ 0, 1, 0},{0,0}}, {{ 0.5f, 0.5f, 0.5f},{ 0, 1, 0},{1,0}},
        {{ 0.5f, 0.5f,-0.5f},{ 0, 1, 0},{1,1}}, {{-0.5f, 0.5f, 0.5f},{ 0, 1, 0},{0,0}},
        {{ 0.5f, 0.5f,-0.5f},{ 0, 1, 0},{1,1}}, {{-0.5f, 0.5f,-0.5f},{ 0, 1, 0},{0,1}},
        // Bottom
        {{-0.5f,-0.5f,-0.5f},{ 0,-1, 0},{0,0}}, {{ 0.5f,-0.5f,-0.5f},{ 0,-1, 0},{1,0}},
        {{ 0.5f,-0.5f, 0.5f},{ 0,-1, 0},{1,1}}, {{-0.5f,-0.5f,-0.5f},{ 0,-1, 0},{0,0}},
        {{ 0.5f,-0.5f, 0.5f},{ 0,-1, 0},{1,1}}, {{-0.5f,-0.5f, 0.5f},{ 0,-1, 0},{0,1}},
        // Right
        {{ 0.5f,-0.5f, 0.5f},{ 1, 0, 0},{0,0}}, {{ 0.5f,-0.5f,-0.5f},{ 1, 0, 0},{1,0}},
        {{ 0.5f, 0.5f,-0.5f},{ 1, 0, 0},{1,1}}, {{ 0.5f,-0.5f, 0.5f},{ 1, 0, 0},{0,0}},
        {{ 0.5f, 0.5f,-0.5f},{ 1, 0, 0},{1,1}}, {{ 0.5f, 0.5f, 0.5f},{ 1, 0, 0},{0,1}},
        // Left
        {{-0.5f,-0.5f,-0.5f},{-1, 0, 0},{0,0}}, {{-0.5f,-0.5f, 0.5f},{-1, 0, 0},{1,0}},
        {{-0.5f, 0.5f, 0.5f},{-1, 0, 0},{1,1}}, {{-0.5f,-0.5f,-0.5f},{-1, 0, 0},{0,0}},
        {{-0.5f, 0.5f, 0.5f},{-1, 0, 0},{1,1}}, {{-0.5f, 0.5f,-0.5f},{-1, 0, 0},{0,1}},
    };
    if (!CreateVertexBuffer(cubeVerts, geo.cubeVertexBuffer, geo.cubeVBV)) return false;

    float s = 20.0f;
    float tile = 8.0f;
    std::vector<VertexPosNormUV> planeVerts = {
        {{-s,0, s},{0,1,0},{0,0}}, {{ s,0, s},{0,1,0},{tile,0}}, {{ s,0,-s},{0,1,0},{tile,tile}},
        {{-s,0, s},{0,1,0},{0,0}}, {{ s,0,-s},{0,1,0},{tile,tile}}, {{-s,0,-s},{0,1,0},{0,tile}},
    };
    if (!CreateVertexBuffer(planeVerts, geo.planeVertexBuffer, geo.planeVBV)) return false;

    // Unit sphere (radius 0.5) for projectiles / debug spheres.
    std::vector<VertexPosNormUV> sphereVerts = BuildSphereVertices();
    if (!CreateVertexBuffer(sphereVerts, geo.sphereVertexBuffer, geo.sphereVBV)) return false;
    geo.sphereVertexCount = (UINT)sphereVerts.size();

    BuildPackedGeometry(cubeVerts, planeVerts, packed);
    return true;
}

// ?? fullscreen toggle ????????????????????????????????????????????????????????
static void ToggleFullscreen(HWND hwnd) {
    if (!isFullscreen) {
        GetWindowRect(hwnd, &windowedRect);
        windowedStyle = GetWindowLong(hwnd, GWL_STYLE);
        HMONITOR hMon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi = { sizeof(mi) };
        GetMonitorInfo(hMon, &mi);
        SetWindowLong(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
        SetWindowPos(hwnd, HWND_TOP,
            mi.rcMonitor.left, mi.rcMonitor.top,
            mi.rcMonitor.right - mi.rcMonitor.left,
            mi.rcMonitor.bottom - mi.rcMonitor.top,
            SWP_FRAMECHANGED | SWP_NOCOPYBITS);
        isFullscreen = true;
    } else {
        SetWindowLong(hwnd, GWL_STYLE, windowedStyle);
        SetWindowPos(hwnd, HWND_NOTOPMOST,
            windowedRect.left, windowedRect.top,
            windowedRect.right - windowedRect.left,
            windowedRect.bottom - windowedRect.top,
            SWP_FRAMECHANGED | SWP_NOCOPYBITS);
        isFullscreen = false;
    }
}

// ?? input ????????????????????????????????????????????????????????????????????
static void ProcessInput(HWND) {
    if (cameraLocked || (showUI && ImGui::GetIO().WantCaptureKeyboard)) return;
    if (GetAsyncKeyState('W') & 0x8000) scene.camera.ProcessKeyboard('W', deltaTime);
    if (GetAsyncKeyState('S') & 0x8000) scene.camera.ProcessKeyboard('S', deltaTime);
    if (GetAsyncKeyState('A') & 0x8000) scene.camera.ProcessKeyboard('A', deltaTime);
    if (GetAsyncKeyState('D') & 0x8000) scene.camera.ProcessKeyboard('D', deltaTime);
    if (GetAsyncKeyState(VK_SPACE) & 0x8000) scene.camera.ProcessKeyboard(' ', deltaTime);
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000) scene.camera.ProcessKeyboard('Q', deltaTime);

    // Auto-fire: while the mouse is held (and not interacting with the UI),
    // keep shooting on a fixed interval instead of one shot per click.
    scene.fireCooldown -= deltaTime;
    const bool mouseHeld = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
    if (scene.autoFire && mouseHeld && !ImGui::GetIO().WantCaptureMouse &&
        scene.fireCooldown <= 0.0f) {
        scene.ShootProjectile();
        scene.fireCooldown = scene.fireInterval;
    }

    // Grenade: press G to lob one. Cooldown debounces the held key.
    scene.grenadeCooldown -= deltaTime;
    if ((GetAsyncKeyState('G') & 0x8000) && scene.grenadeCooldown <= 0.0f) {
        scene.ThrowGrenade();
        scene.grenadeCooldown = 0.6f;
    }
}

// ?? window proc ??????????????????????????????????????????????????????????????
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam)) return true;

    switch (msg) {
    case WM_SIZE:
        if (g_dx12.device && g_dx12.initialized && wParam != SIZE_MINIMIZED) {
            unsigned w = LOWORD(lParam), h = HIWORD(lParam);
            if (w > 0 && h > 0 && (w != SCR_WIDTH || h != SCR_HEIGHT)) {
                WaitForGPU();
                SCR_WIDTH = w; SCR_HEIGHT = h;
                ResizeDX12(SCR_WIDTH, SCR_HEIGHT);
                if (occlusionDepth.initialized) occlusionDepth.Resize(SCR_WIDTH, SCR_HEIGHT);
                if (visBuffer.initialized) visBuffer.Resize(SCR_WIDTH, SCR_HEIGHT);
                if (g_rt.initialized) ResizeRaytracing(SCR_WIDTH, SCR_HEIGHT);
            }
        }
        return 0;

    case WM_MOUSEMOVE:
        if (!cameraLocked && !(showUI && ImGui::GetIO().WantCaptureMouse)) {
            if (ignoreNextMouseMove) {
                // This move was generated by our own SetCursorPos recenter below,
                // not real user input - skip it so it can't be misread as a delta.
                ignoreNextMouseMove = false;
                return 0;
            }

            float xpos = (float)GET_X_LPARAM(lParam);
            float ypos = (float)GET_Y_LPARAM(lParam);

            RECT r; GetClientRect(hwnd, &r);
            float centerX = (float)(r.right - r.left) / 2.0f;
            float centerY = (float)(r.bottom - r.top) / 2.0f;

            if (firstMouse) { lastX = centerX; lastY = centerY; firstMouse = false; }
            else {
                float dx = xpos - lastX;
                float dy = lastY - ypos; // screen Y grows downward; flip so moving the mouse up looks up
                if (dx != 0.0f || dy != 0.0f) scene.camera.ProcessMouseMovement(dx, dy);
            }

            // Re-center the cursor every move so it never reaches the screen
            // edge and clamps, which would otherwise cap how far you can turn.
            POINT c = { (LONG)centerX, (LONG)centerY };
            ClientToScreen(hwnd, &c);
            ignoreNextMouseMove = true;
            SetCursorPos(c.x, c.y);
            lastX = centerX; lastY = centerY;
        }
        return 0;

    case WM_LBUTTONDOWN:
        if (!ImGui::GetIO().WantCaptureMouse) {
            if (cameraLocked) {
                cameraLocked = false;
                SetCapture(hwnd); ShowCursor(FALSE);
                RECT r; GetClientRect(hwnd, &r);
                POINT c = { (r.right-r.left)/2, (r.bottom-r.top)/2 };
                ClientToScreen(hwnd, &c);
                ignoreNextMouseMove = true;
                SetCursorPos(c.x, c.y);
                lastX = (float)(r.right-r.left)/2;
                lastY = (float)(r.bottom-r.top)/2;
                firstMouse = true;
            } else if (!scene.autoFire) {
                // Auto-fire handles shooting in ProcessInput while held; only
                // fire on click when auto-fire is off.
                scene.ShootProjectile();
            }
        }
        return 0;

    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) PostQuitMessage(0);
        else if (wParam == VK_TAB) {
            showUI = !showUI;
            if (showUI) {
                cameraLocked = true;
                ReleaseCapture(); ShowCursor(TRUE);
            } else {
                // Hiding the UI: capture and re-center the mouse so the next
                // WM_MOUSEMOVE delta is computed from the window center instead
                // of wherever the cursor happened to be over the UI, which
                // otherwise causes the camera to snap-rotate on the first move.
                cameraLocked = false;
                SetCapture(hwnd); ShowCursor(FALSE);
                RECT r; GetClientRect(hwnd, &r);
                POINT c = { (r.right - r.left) / 2, (r.bottom - r.top) / 2 };
                ClientToScreen(hwnd, &c);
                ignoreNextMouseMove = true;
                SetCursorPos(c.x, c.y);
                lastX = (float)(r.right - r.left) / 2;
                lastY = (float)(r.bottom - r.top) / 2;
                firstMouse = true;
            }
        }
        else if (wParam == 'C')    { cameraLocked = true; ReleaseCapture(); ShowCursor(TRUE); }
        else if (wParam == 'F')    { scene.camera.FPSMode = !scene.camera.FPSMode; }
        // Bit 30 = key was already down (autorepeat); toggle once per press.
        else if (wParam == 'Z' && !(lParam & 0x40000000)) {
            scene.meshletWireframe = !scene.meshletWireframe;
        }
        else if (wParam == VK_F11) { ToggleFullscreen(hwnd); }
        return 0;

    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

// ?? entry point ??????????????????????????????????????????????????????????????
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    AllocConsole();
    FILE* fp;
    freopen_s(&fp, "CONOUT$", "w", stdout);
    freopen_s(&fp, "CONOUT$", "w", stderr);

    std::cout << "GraphicEngine DX12 Starting..." << std::endl;

    // Window
    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"GraphicEngineDX12";
    RegisterClassExW(&wc);

    RECT rc = { 0, 0, (LONG)SCR_WIDTH, (LONG)SCR_HEIGHT };
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowW(L"GraphicEngineDX12", L"Graphics Engine - DirectX 12",
        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
        rc.right - rc.left, rc.bottom - rc.top,
        nullptr, nullptr, hInstance, nullptr);
    if (!hwnd) { std::cerr << "Window creation failed\n"; return -1; }
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    // DX12
    try {
        if (!InitDX12(hwnd, SCR_WIDTH, SCR_HEIGHT)) {
            MessageBoxA(hwnd, "Failed to init DX12.", "Error", MB_OK | MB_ICONERROR);
            return -1;
        }
    } catch (const std::exception& e) {
        MessageBoxA(hwnd, e.what(), "DX12 Error", MB_OK | MB_ICONERROR);
        return -1;
    }

    // ImGui
    D3D12_DESCRIPTOR_HEAP_DESC ihd = {};
    ihd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    ihd.NumDescriptors = 1;
    ihd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    g_dx12.device->CreateDescriptorHeap(&ihd, IID_PPV_ARGS(&imguiSrvHeap));

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX12_Init(g_dx12.device.Get(), FRAME_COUNT, DXGI_FORMAT_R8G8B8A8_UNORM,
        imguiSrvHeap.Get(),
        imguiSrvHeap->GetCPUDescriptorHandleForHeapStart(),
        imguiSrvHeap->GetGPUDescriptorHandleForHeapStart());

    // Geometry
    if (!CreateAllGeometry()) { std::cerr << "Geometry creation failed\n"; return -1; }

    // Shaders - the DX12-specific pair supports albedo/normal/metal-roughness texture
    // sampling (needed for imported GLB materials); the plain "clustered_*" pair is
    // color-only and silently ignores textures, so it's only a fallback.
    if (!mainShader.Load("shaders/clustered_dx12_vs.hlsl", "shaders/clustered_dx12_ps.hlsl")) {
        std::cerr << "Trying fallback shaders...\n";
        if (!mainShader.Load("shaders/clustered_vs.hlsl", "shaders/clustered_ps.hlsl")) {
            MessageBoxA(hwnd, "Shader load failed.", "Shader Error", MB_OK | MB_ICONERROR);
            return -1;
        }
    }
    std::cout << "Shaders loaded\n";

    g_useMeshShader = g_meshShader.Init(mainShader);
    std::cout << (g_useMeshShader
        ? "Mesh shader path enabled\n"
        : "Mesh shader path unavailable; using raster fallback\n");

    if (!g_useMeshShader || !g_terrain.Init(mainShader)) {
        scene.useMeshTerrain = false;
        std::cerr << "Mesh shader terrain unavailable; keeping flat floor\n";
    }

    // Mip generator (compute shader) for imported GLB textures
    if (!g_mipGen.Init()) {
        std::cerr << "Mip generator init failed (non-fatal, textures will have no mips)\n";
    }

    // The command list is closed after InitDX12 and stays closed until the first
    // BeginFrame(). skyRenderer.Init() records a CopyTextureRegion for the HDRI
    // upload, so the list must be open while it runs and its work must be flushed
    // (executed + waited) before the list is closed again - otherwise the copy
    // never reaches the GPU and the sky texture stays black.
    ThrowIfFailed(g_dx12.commandAllocators[g_dx12.frameIndex]->Reset());
    ThrowIfFailed(g_dx12.commandList->Reset(g_dx12.commandAllocators[g_dx12.frameIndex].Get(), nullptr));
    if (!skyRenderer.Init()) {
        std::cerr << "HDRI sky init failed (non-fatal)\n";
    }
    ThrowIfFailed(g_dx12.commandList->Close());
    {
        ID3D12CommandList* skyLists[] = { g_dx12.commandList.Get() };
        g_dx12.commandQueue->ExecuteCommandLists(1, skyLists);
    }
    WaitForGPU();
    DumpDX12DebugMessages();
    {
        auto skySH = GLBImporter::ComputeSkyIrradianceSH("models/Skyboxes/sunny_rose_garden_2k.exr");
        mainShader.SetSkyIrradiance(skySH, 1.0f);
    }
    if (!occlusionDepth.Init(SCR_WIDTH, SCR_HEIGHT)) {
        std::cerr << "Meshlet occlusion depth init failed (non-fatal)\n";
    }

    // Visibility buffer (id Tech path)
    if (!visBuffer.Init(SCR_WIDTH, SCR_HEIGHT)) {
        std::cerr << "VB init failed (non-fatal)\n";
        scene.useVisibilityBuffer = false;
    } else {
        std::cout << "Visibility Buffer ready\n";
    }

    if (!shadowMap.Init()) {
        std::cerr << "Shadow map init failed (non-fatal)\n";
        scene.enableShadows = false;
    }

    // Raytracing (DXR path)
    if (!InitRaytracing(geo)) {
        std::cerr << "DXR init failed (non-fatal)\n";
        scene.useRaytracing = false;
    } else {
        std::cout << "DXR Raytracing ready\n";
    }

    // Scene lights
    scene.InitLights();

    // Timer
    gameTimer.Start();
    float lastTime = 0.0f;

    std::cout << "Controls: WASD, Mouse, TAB=UI, F11=Fullscreen, ESC=Exit\n";

    // ?? main loop ????????????????????????????????????????????????????????????
    MSG msg = {};
    while (msg.message != WM_QUIT) {
        if (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            continue;
        }

        float now = gameTimer.GetElapsed();
        deltaTime = now - lastTime;
        lastTime  = now;

        ProcessInput(hwnd);

        // Walking collision: ground level follows the mesh-shader terrain at
        // the camera's XZ so gravity settles the player onto the hills.
        if (scene.useMeshTerrain && g_terrain.supported) {
            TerrainRendererDX12::Params terrainParams;
            terrainParams.heightScale = scene.terrainHeightScale;
            scene.camera.FloorY = TerrainRendererDX12::HeightAt(
                terrainParams, scene.camera.Position.x, scene.camera.Position.z);
        } else {
            scene.camera.FloorY = 0.0f;
        }

        scene.Update(deltaTime, now);

        if (scene.rebuildDestructionRequested && wallModel) {
            scene.rebuildDestructionRequested = false;
            // Re-init frees the old chunk vertex/index buffers. The GPU may
            // still be rendering last frame's chunk meshes, so drain it first
            // or those buffers get destroyed in flight and crash.
            WaitForGPU();
            g_destruction.Initialize(wallModel, g_dx12.device.Get(), 1, 1, 1);
        }
        if (scene.useDestruction && g_destruction.IsInitialized()) {
            g_destruction.Update(deltaTime);
            for (auto& projectile : scene.projectiles) {
                if (projectile.grenade) {
                    // A grenade explodes on fuse timeout (Scene set detonate) or
                    // the moment it strikes the building; either way a radial
                    // blast breaks the whole sphere of pieces around it.
                    XMFLOAT3 hit;
                    const bool struck = projectile.active && g_destruction.HitTestSegment(
                        projectile.previousPosition, projectile.position, 0.25f, hit);
                    if (projectile.detonate || struck) {
                        const XMFLOAT3 center = struck ? hit : projectile.position;
                        g_destruction.ApplyExplosion(center, scene.grenadeBlastRadius,
                                                     scene.grenadeDamage, scene.grenadeImpulse);
                        projectile.active = false;
                        projectile.detonate = false;
                    }
                    continue;
                }
                if (!projectile.active) continue;
                XMFLOAT3 hit;
                // Collision uses the bullet's own small radius so it must
                // actually reach the surface before it registers -- the wider
                // damage radius only governs how far the fracture spreads once
                // the bullet has struck. Otherwise the wall breaks at a distance.
                const float bulletRadius = std::max(0.12f, scene.projectileScale * 0.5f);
                if (g_destruction.HitTestSegment(projectile.previousPosition, projectile.position,
                                                 bulletRadius, hit)) {
                    std::cout << "Projectile hit wall at " << hit.x << ", "
                              << hit.y << ", " << hit.z << "\n";
                    g_destruction.ApplyRadialDamage(hit, scene.destructionDamageRadius,
                                                    scene.destructionDamage);
                    g_destruction.ApplyImpulse(hit, projectile.direction,
                                               scene.destructionBulletImpulse,
                                               scene.destructionDamageRadius);
                    // Impact FX: spark burst + hole decal. Surface normal is
                    // approximated as facing back along the bullet's travel.
                    const XMFLOAT3 normal(-projectile.direction.x,
                                          -projectile.direction.y,
                                          -projectile.direction.z);
                    scene.SpawnBulletImpact(hit, normal);
                    projectile.active = false;
                }
            }
        }

        // ?? begin frame ??
        try { BeginFrame(); }
        catch (const std::exception& e) { std::cerr << "BeginFrame: " << e.what() << "\n"; break; }

        float cc[4] = { scene.clearColor.x, scene.clearColor.y, scene.clearColor.z, 1.0f };
        ClearRenderTarget(cc);
        skyRenderer.Render(scene.camera, scene.cameraFOV, scene.lightPos, now);

        mainShader.BeginFrame();
        g_meshShader.SetOcclusionDepth(
            occlusionDepth.GetGPUHandle(), occlusionDepth.valid);

        if (!crateLoadAttempted) {
            crateLoadAttempted = true;
            LoadFloorMudMaterial();
            std::cout << "Loading models/h2.glb...\n";
            crateModel = GLBImporter::LoadGLB("models/h2.glb", g_dx12.device, g_dx12.commandList);
            // Modular destructible house built from structural pieces, with
            // world-anchored foundation/sill chunks. Grid args unused (bonds
            // come from AABB adjacency), so pass 1s.
            wallModel = CreateDestructibleWallModel();
            ApplyHouseTextures(wallModel, g_dx12.device.Get(), g_dx12.commandList.Get());
            g_destruction.Initialize(wallModel, g_dx12.device.Get(), 1, 1, 1);
            if (crateModel) {
                if (auto merged = GLBImporter::MergeSceneByMaterial(crateModel, g_dx12.device)) {
                    crateModel = merged;
                }
                crateModel->UpdateGlobalTransform(crateModel->localTransform);
                size_t materialDraws = crateModel->mesh ? crateModel->mesh->primitives.size() : 0;
                std::cout << "h2 model loaded: " << materialDraws
                          << " material draw(s)\n";
            } else {
                std::cerr << "Failed to load h2.glb, falling back to procedural cube\n";
            }

            // Flush the load/mip-generation commands now and print any D3D12
            // validation errors before continuing, so mip-related bugs surface
            // immediately instead of silently corrupting later frames.
            ThrowIfFailed(g_dx12.commandList->Close());
            ID3D12CommandList* loadLists[] = { g_dx12.commandList.Get() };
            g_dx12.commandQueue->ExecuteCommandLists(1, loadLists);
            WaitForGPU();
            DumpDX12DebugMessages();
            ThrowIfFailed(g_dx12.commandAllocators[g_dx12.frameIndex]->Reset());
            ThrowIfFailed(g_dx12.commandList->Reset(g_dx12.commandAllocators[g_dx12.frameIndex].Get(), nullptr));
            // Reset() drops all descriptor heap bindings and the RTV/DSV binding
            // set by BeginFrame()/ClearRenderTarget() earlier this frame, so redo
            // that setup (minus the PRESENT->RENDER_TARGET barrier, which only
            // applies once - the target is already in RENDER_TARGET state).
            ID3D12DescriptorHeap* mainHeaps[] = { g_dx12.cbvSrvUavHeap.Get(), g_dx12.samplerHeap.Get() };
            g_dx12.commandList->SetDescriptorHeaps(2, mainHeaps);
            g_dx12.commandList->RSSetViewports(1, &g_dx12.viewport);
            g_dx12.commandList->RSSetScissorRects(1, &g_dx12.scissorRect);
            ClearRenderTarget(cc);
            mainShader.BeginFrame();
        }

        // ?? render ??
        if (scene.useRaytracing && g_rt.initialized) {
            RenderRaytracing(scene);
        } else if (scene.useVisibilityBuffer && visBuffer.initialized) {
            RenderIdTech(scene, mainShader, visBuffer, geo, packed);
        } else {
            XMMATRIX lightSpace = XMMatrixIdentity();
            ID3D12Resource* shadowResource = nullptr;
            if (scene.enableShadows && shadowMap.initialized && scene.lightType == 0) {
                lightSpace = shadowMap.Render(scene, geo,
                    g_showH2Model ? crateModel : nullptr);
                shadowResource = shadowMap.GetResource();
            }
            RenderForward(scene, mainShader, geo, crateModel, floorMaterial, lightSpace, shadowResource);
        }

        // Preserve this frame's depth for next-frame amplification-shader
        // occlusion tests before UI rendering changes descriptor heaps.
        occlusionDepth.Capture(g_dx12.commandList.Get());

        // Ensure ImGui renders to the swapchain backbuffer (VB path changes OM target)
        {
            D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = GetCPUDescriptorHandle(
                g_dx12.rtvHeap.Get(), g_dx12.rtvDescriptorSize, g_dx12.frameIndex);
            D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = g_dx12.dsvHeap->GetCPUDescriptorHandleForHeapStart();
            g_dx12.commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);
            g_dx12.commandList->RSSetViewports(1, &g_dx12.viewport);
            g_dx12.commandList->RSSetScissorRects(1, &g_dx12.scissorRect);
        }

        // ?? ImGui ??
        ImGui_ImplDX12_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        if (showUI) RenderUI(scene, visBuffer);
        DrawDestructionDebug(scene);
        ImGui::Render();

        ID3D12DescriptorHeap* heaps[] = { imguiSrvHeap.Get() };
        g_dx12.commandList->SetDescriptorHeaps(1, heaps);
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_dx12.commandList.Get());

        // ?? end frame ??
        try { EndFrame(); }
        catch (const std::exception& e) { std::cerr << "EndFrame: " << e.what() << "\n"; break; }
    }

    WaitForGPU();
    ImGui_ImplDX12_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_destruction.Shutdown();
    CleanupDX12();
    return (int)msg.wParam;
}
