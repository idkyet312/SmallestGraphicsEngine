#pragma once

// Private application implementation; included once by main.cpp in dependency order.

// Basic modular destructible house built from real structural pieces: a
// foundation slab and floor, four walls made of vertical studs + cladding,
// door/window openings, and a two-slope roof of rafters + sheets. Each piece
// is one child chunk. Pieces whose name starts with "Support:" are anchored to
// the world by the destruction layer, so foundation and bottom wall plates
// stay static and hold the structure up; disconnected sections fall. Blast
// bonds touching pieces so a hit tears loose only what it structurally frees.
static std::shared_ptr<SceneNode> CreateDestructibleWallModel() {
    // House footprint (world units). Front faces +Z toward the spawn area.
    constexpr float minX = -7.0f, maxX = 0.0f;   // flat terrain near world origin
    constexpr float minZ = 1.0f, maxZ = 6.0f;    // depth
    // The island's ground sits above sea level, so the houses are built on the flat
    // pad stamped into the terrain -- not at y = 0, or they end up buried in sand.
    // The roofs (RoofModel.h) are placed off this same constant.
    constexpr float floorY = Ground::kBuildingPadY, wallTop = floorY + 3.4f;
    constexpr float wall = 0.28f;                // wall / slab thickness
    // Terrain pad is exactly floorY. Sink slab undersides slightly so their
    // bottom triangles never occupy the same depth plane as terrain.
    constexpr float foundationEmbed = 0.06f;

    auto root = std::make_shared<SceneNode>("DestructibleHouse");
    auto matFoundation = std::make_shared<SceneMaterial>();
    matFoundation->name = "Foundation";
    matFoundation->baseColorFactor = XMFLOAT4(0.55f, 0.55f, 0.58f, 1.0f);
    matFoundation->metallicFactor = 0.0f; matFoundation->roughnessFactor = 0.95f;
    matFoundation->ambientScale = 1.12f; matFoundation->viewFillStrength = 0.10f;
    auto matStud = std::make_shared<SceneMaterial>();
    matStud->name = "Stud";
    matStud->baseColorFactor = XMFLOAT4(0.58f, 0.40f, 0.24f, 1.0f);
    matStud->metallicFactor = 0.0f; matStud->roughnessFactor = 0.88f;
    matStud->ambientScale = 1.18f; matStud->viewFillStrength = 0.16f;
    auto matCladding = std::make_shared<SceneMaterial>();
    matCladding->name = "Cladding";
    matCladding->baseColorFactor = XMFLOAT4(0.82f, 0.66f, 0.44f, 1.0f);
    matCladding->metallicFactor = 0.0f; matCladding->roughnessFactor = 0.85f;
    matCladding->ambientScale = 1.20f; matCladding->viewFillStrength = 0.18f;
    auto matRoof = std::make_shared<SceneMaterial>();
    matRoof->name = "Roof";
    matRoof->baseColorFactor = XMFLOAT4(0.68f, 0.70f, 0.72f, 1.0f);
    matRoof->metallicFactor = 0.65f; matRoof->roughnessFactor = 0.45f;  // galvanised sheet
    matRoof->ambientScale = 1.08f; matRoof->viewFillStrength = 0.08f;
    auto matMetalWall = std::make_shared<SceneMaterial>();
    matMetalWall->name = "MetalWall";
    matMetalWall->baseColorFactor = XMFLOAT4(0.62f, 0.63f, 0.62f, 1.0f);
    // 0.62 -> 0.48: corrugated sheet metal, weathered but still specular. The
    // old value sat just above the RT reflection roughness cut (0.52), so these
    // walls were handed to the environment probe and never resolved a real
    // reflection despite being the most reflective large surfaces in the level.
    matMetalWall->metallicFactor = 0.80f; matMetalWall->roughnessFactor = 0.48f;
    matMetalWall->ambientScale = 1.14f; matMetalWall->viewFillStrength = 0.12f;
    auto matMetalRoof = std::make_shared<SceneMaterial>();
    matMetalRoof->name = "MetalRoof";
    matMetalRoof->baseColorFactor = XMFLOAT4(0.58f, 0.58f, 0.56f, 1.0f);
    // 0.58 -> 0.44: galvanised roofing, the smoothest of the sheet metals here
    // and now under the reflection cut with it. Matches RoofModel.h and the
    // MetalRoof packed-PBR set, which dress the same corrugated iron.
    matMetalRoof->metallicFactor = 0.85f; matMetalRoof->roughnessFactor = 0.44f;
    matMetalRoof->ambientScale = 1.08f; matMetalRoof->viewFillStrength = 0.08f;
    auto matDarkMetal = std::make_shared<SceneMaterial>();
    matDarkMetal->name = "DarkMetal";
    // Untextured, so both factors are live and directly control the BRDF -- no
    // metallicRoughness map overrides them the way MetalWall's rusted-roughness
    // JPG overrides its factor.
    //
    // Previously 0.015 base / 0.60 metallic / 0.72 rough, which was three
    // mutually inconsistent values rather than dark metal:
    //   * For a metal, base colour IS specular reflectance (F0). At 0.015 the
    //     surface returns ~1.5% of what hits it, so it could not read as metal
    //     at any roughness -- it was authored as a light absorber.
    //   * Metallic is physically 0 or 1; 0.60 blends two BRDFs and matches no
    //     real material.
    //   * 0.72 sat above the 0.52 RT reflection cut, so the surface was always
    //     handed to the environment probe and never resolved a real reflection.
    // Dark oxidised steel is still a metal: fully metallic, with a low but not
    // black F0 and enough smoothness to carry a blurred reflection.
    matDarkMetal->baseColorFactor = XMFLOAT4(0.10f, 0.105f, 0.10f, 1.0f);
    matDarkMetal->metallicFactor = 1.0f; matDarkMetal->roughnessFactor = 0.42f;
    matDarkMetal->ambientScale = 1.12f; matDarkMetal->viewFillStrength = 0.10f;
    auto matTrim = std::make_shared<SceneMaterial>();
    matTrim->name = "MetalTrim";
    matTrim->baseColorFactor = XMFLOAT4(0.46f, 0.50f, 0.50f, 1.0f);
    matTrim->metallicFactor = 0.90f; matTrim->roughnessFactor = 0.38f;
    matTrim->ambientScale = 1.10f; matTrim->viewFillStrength = 0.10f;
    auto matGlass = std::make_shared<SceneMaterial>();
    matGlass->name = "Glass";
    // Hybrid rendering correctly composites panes after every opaque object.
    // Keep the tint subtle; the old 0.28 alpha only looked acceptable because
    // forward rendering accidentally overwrote glass with later opaque draws.
    matGlass->baseColorFactor = XMFLOAT4(0.58f, 0.76f, 0.86f, 0.04f);
    matGlass->metallicFactor = 0.0f; matGlass->roughnessFactor = 0.04f;
    matGlass->waterTransparency = WaterTransparencyMode::AfterWater;

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
                const XMFLOAT3 bitangent = faceY ? XMFLOAT3(0, 0, 1)
                                                 : XMFLOAT3(0, 1, 0);
                const XMVECTOR crossNT = XMVector3Cross(
                    XMLoadFloat3(&n), XMLoadFloat3(&tangent));
                const float tangentW = XMVectorGetX(XMVector3Dot(
                    crossNT, XMLoadFloat3(&bitangent))) >= 0.0f ? 1.0f : -1.0f;
                const float vertex[12] = { p.x,p.y,p.z, n.x,n.y,n.z, u,v,
                                           tangent.x, tangent.y, tangent.z, tangentW };
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
    // `shatter` = glass mode: a denser 2D scatter of sites so the pane breaks
    // into many small shards instead of a few plank-like slices.
    // `xform`, if given, is applied to every emitted vertex after the board is
    // built in its axis-aligned local frame -- used to tilt roof panels onto a
    // real slope. Positions transform fully; normals/tangents by rotation only.
    auto addVoronoiBoard = [&](const char* name, const std::shared_ptr<SceneMaterial>& material,
                               float x0, float x1, float y0, float y1,
                               float z0, float z1, int seed, bool shatter = false,
                               const XMMATRIX* xform = nullptr) {
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
        // Deterministic integer-hash jitter (no RNG); the piece id seeds it so
        // every board breaks along different lines.
        std::vector<P2> sites;
        if (shatter) {
            // Glass: 2D grid of jittered sites -> many small angular shards.
            //
            // Piece counts halved to cut destruction CPU time: every shard is a
            // rigid body that gets solved, collided and walked by the render
            // rebuild, so this count multiplies straight into per-frame cost.
            // The cell sizes double alongside the caps (0.35 -> 0.70,
            // 0.4 -> 0.8) so a pane still fills with shards rather than
            // producing a few large ones sitting in a mostly-intact frame.
            const int cols = std::max(2, std::min(3, (int)std::lround(length / 0.70f)));
            const int rows = std::max(1, std::min(2, (int)std::lround((h1 - h0) / 0.8f)));
            const float cw = length / cols, ch = (h1 - h0) / rows;
            for (int r = 0; r < rows; ++r) for (int i = 0; i < cols; ++i) {
                const float jx = (((i * 37 + r * 53 + seed * 17 + 3) % 13) / 12.0f - 0.5f) * cw * 0.9f;
                const float jy = (((i * 19 + r * 29 + seed * 41 + 7) % 11) / 10.0f - 0.5f) * ch * 0.9f;
                sites.push_back({ l0 + (i + 0.5f) * cw + jx, h0 + (r + 0.5f) * ch + jy });
            }
        } else {
            // Boards: same halving as the glass above. Wider cells (1.2 -> 2.4)
            // keep a wall spanned by planks instead of leaving gaps between a
            // handful of narrow ones.
            const int cols = std::max(2, std::min(4, (int)std::lround(length / 2.4f)));
            const float cellW = length / cols;
            for (int i = 0; i < cols; ++i) {
                const float jx = (((i * 37 + seed * 17 + 3) % 13) / 12.0f - 0.5f) * cellW * 0.9f;
                const float jy = (((i * 19 + seed * 41 + 7) % 11) / 10.0f - 0.5f) * (h1 - h0) * 0.8f;
                sites.push_back({ l0 + (i + 0.5f) * cellW + jx, (h0 + h1) * 0.5f + jy });
            }
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

        // Roughen the cell outline so the seams read as a material tearing
        // rather than as clean geometric cuts.
        //
        // The clipped polygon above is exact: every edge is a straight bisector
        // segment, which is why an intact board's break lines look machined. This
        // subdivides each edge and pushes the intermediate points sideways.
        //
        // THE CONSTRAINT THAT SHAPES THIS: adjacent cells share an edge, and the
        // two cells compute that edge independently (each from its own clip). So
        // the displacement cannot depend on the cell, the site, or the direction
        // the edge happens to be walked -- if it did, neighbours would tear apart
        // and leave visible gaps in an unbroken board. Instead it is hashed from
        // the midpoint's QUANTISED POSITION, which both cells arrive at
        // identically, and applied along the edge normal, which both compute as
        // the same line. Two cells sharing an edge therefore produce the same
        // wiggle and stay flush.
        //
        // Board-boundary edges are left straight: a plank's outer rectangle is
        // its actual silhouette, and roughening it would make flush boards look
        // gap-toothed while still assembled.
        auto roughenCell = [&](std::vector<P2>& poly) {
            if (poly.size() < 3) return;
            // Position hash -> [-1, 1]. Quantised to 1 mm so both cells agree
            // despite their clip arithmetic differing in the last few bits.
            auto edgeNoise = [&](float mx, float my, int salt) {
                const int qx = (int)std::lround(mx * 1000.0f);
                const int qy = (int)std::lround(my * 1000.0f);
                uint32_t h = (uint32_t)(qx * 73856093) ^ (uint32_t)(qy * 19349663)
                           ^ (uint32_t)(salt * 83492791);
                h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
                return ((h & 0xffffu) / 32767.5f) - 1.0f;
            };
            // Amplitude scales with the board so a stud and a wall panel get
            // proportionate roughness. Glass shards stay crisper -- a fractured
            // pane splits along clean lines, it does not fray.
            const float span = (std::min)(l1 - l0, h1 - h0);
            const float amplitude = span * (shatter ? 0.10f : 0.26f);
            if (amplitude < 0.0008f) return;
            constexpr float kEdgeEpsilon = 0.0015f;
            const auto onBoundary = [&](const P2& p) {
                return p.x <= l0 + kEdgeEpsilon || p.x >= l1 - kEdgeEpsilon ||
                       p.y <= h0 + kEdgeEpsilon || p.y >= h1 - kEdgeEpsilon;
            };

            std::vector<P2> rough;
            rough.reserve(poly.size() * 3);
            for (size_t i = 0; i < poly.size(); ++i) {
                const P2 a = poly[i], b = poly[(i + 1) % poly.size()];
                rough.push_back(a);
                // An edge with both ends on the board rectangle is a silhouette
                // edge, not a fracture seam.
                if (onBoundary(a) && onBoundary(b)) continue;
                const float ex = b.x - a.x, ey = b.y - a.y;
                const float len = std::sqrt(ex * ex + ey * ey);
                if (len < 0.006f) continue;

                // CANONICAL EDGE ORDER. The two cells sharing this edge walk it
                // in opposite directions, so subdividing from each cell's own
                // start point puts cut 1 at opposite ends and the two hash
                // different midpoints -- which tore the cells apart. Sort the
                // endpoints into a fixed order first, so both cells subdivide
                // the identical parametrisation.
                const bool flipped = (b.x < a.x) || (b.x == a.x && b.y < a.y);
                const P2 e0 = flipped ? b : a;
                const P2 e1 = flipped ? a : b;
                const float dx = e1.x - e0.x, dy = e1.y - e0.y;
                // Unit normal from the canonical direction, so both cells also
                // agree on which way the displacement points.
                const float nx = -dy / len, ny = dx / len;
                // More cuts on longer edges, so roughness stays even in scale.
                // Denser than a gentle wave needs: a splintering seam wants
                // several direction changes along its length, not one bulge.
                constexpr int kMaxCuts = 7;
                const int cuts = (std::min)(kMaxCuts, (std::max)(2,
                    (int)std::lround(len / 0.075f)));
                // Built in canonical order, then reversed if this cell walks the
                // edge the other way -- the polygon must stay correctly wound.
                P2 pending[kMaxCuts];
                for (int c = 1; c <= cuts; ++c) {
                    const float t = (float)c / (float)(cuts + 1);
                    const float px = e0.x + dx * t, py = e0.y + dy * t;
                    // Taper toward the endpoints: the corners are where three
                    // cells meet, and moving them would break that junction.
                    // Square-rooted so the taper opens up fast and most of the
                    // edge gets near-full amplitude -- a plain sine spent most
                    // of its length near zero and read as a smooth bow.
                    const float taper = std::sqrt(std::sin(t * 3.14159265f));
                    // Alternating sign is what makes this a zigzag rather than a
                    // wander: consecutive cuts are pushed to OPPOSITE sides of
                    // the edge, so the seam reverses direction at every step and
                    // leaves the sharp corners a split follows. The magnitude
                    // still comes from the position hash, so no two seams share
                    // a rhythm, and both cells agree because `c` is indexed in
                    // canonical order.
                    const float side = (c & 1) ? 1.0f : -1.0f;
                    // Kept off zero so an unlucky hash cannot flatten a step.
                    const float magnitude =
                        0.45f + 0.55f * std::fabs(edgeNoise(px, py, c));
                    const float offset = side * magnitude * amplitude * taper;
                    P2 q{ px + nx * offset, py + ny * offset };
                    // Never push a point outside the board rectangle.
                    q.x = (std::max)(l0, (std::min)(l1, q.x));
                    q.y = (std::max)(h0, (std::min)(h1, q.y));
                    pending[c - 1] = q;
                }
                for (int c = 0; c < cuts; ++c)
                    rough.push_back(pending[flipped ? (cuts - 1 - c) : c]);
            }
            if (rough.size() >= 3) poly.swap(rough);
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
            std::vector<P2> poly = clipCell(s);
            if (poly.size() < 3) continue;
            // Jagged the seams. Amplitude is small relative to the cell, and
            // tapered to zero at the corners, so the result stays star-shaped
            // about its centroid -- which the cap fan below depends on.
            roughenCell(poly);
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
                auto uvFor = [&](const XMFLOAT3& p) {
                    if (faceX) return XMFLOAT2(p.z / kUvScale, p.y / kUvScale);
                    if (faceY) return XMFLOAT2(p.x / kUvScale, p.z / kUvScale);
                    return XMFLOAT2(p.x / kUvScale, p.y / kUvScale);
                };
                const XMFLOAT2 uv0 = uvFor(ta), uv1 = uvFor(tb), uv2 = uvFor(tc);
                const XMFLOAT3 edge1(tb.x - ta.x, tb.y - ta.y, tb.z - ta.z);
                const XMFLOAT3 edge2(tc.x - ta.x, tc.y - ta.y, tc.z - ta.z);
                const float du1 = uv1.x - uv0.x, dv1 = uv1.y - uv0.y;
                const float du2 = uv2.x - uv0.x, dv2 = uv2.y - uv0.y;
                const float uvDet = du1 * dv2 - dv1 * du2;
                XMFLOAT3 generatedTangent = tan;
                float tangentW = 1.0f;
                if (std::abs(uvDet) > 0.000001f) {
                    const float invDet = 1.0f / uvDet;
                    XMVECTOR tangentV = XMVectorSet(
                        (edge1.x * dv2 - edge2.x * dv1) * invDet,
                        (edge1.y * dv2 - edge2.y * dv1) * invDet,
                        (edge1.z * dv2 - edge2.z * dv1) * invDet, 0.0f);
                    const XMVECTOR bitangentV = XMVectorSet(
                        (edge2.x * du1 - edge1.x * du2) * invDet,
                        (edge2.y * du1 - edge1.y * du2) * invDet,
                        (edge2.z * du1 - edge1.z * du2) * invDet, 0.0f);
                    tangentV = XMVector3Normalize(tangentV);
                    XMStoreFloat3(&generatedTangent, tangentV);
                    tangentW = XMVectorGetX(XMVector3Dot(
                        XMVector3Cross(XMLoadFloat3(&n), tangentV), bitangentV)) >= 0.0f
                        ? 1.0f : -1.0f;
                }
                for (const XMFLOAT3& p : pts) {
                    // World-scaled tiling per dominant face axis (same rule as
                    // addBox) -> uniform texel size on the jagged side walls.
                    // UVs use the LOCAL (pre-tilt) position so the corrugations
                    // run straight along the panel regardless of slope.
                    float u, v;
                    if (faceX)      { u = p.z; v = p.y; }
                    else if (faceY) { u = p.x; v = p.z; }
                    else            { u = p.x; v = p.y; }
                    u /= kUvScale; v /= kUvScale;
                    // Tilt into world space if a transform was supplied.
                    XMFLOAT3 wp = p, wn = n, wt = generatedTangent;
                    if (xform) {
                        XMStoreFloat3(&wp, XMVector3Transform(XMLoadFloat3(&p), *xform));
                        XMStoreFloat3(&wn, XMVector3Normalize(XMVector3TransformNormal(XMLoadFloat3(&n), *xform)));
                        XMStoreFloat3(&wt, XMVector3Normalize(XMVector3TransformNormal(XMLoadFloat3(&tan), *xform)));
                    }
                    const float vert[12] = { wp.x,wp.y,wp.z, wn.x,wn.y,wn.z, u,v,
                                             wt.x,wt.y,wt.z, tangentW };
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
                // Transparent glass cells share these edges while intact. Their
                // internal extrusion walls showed through both caps as bright
                // pre-cracked lines. Glass keeps two caps (and therefore real
                // collision thickness), but only separation exposes shard gaps.
                if (shatter) continue;
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

    // Lays an anchored floor as a grid of tiles rather than one slab, so a
    // crater can take out the part it actually undermined and leave the rest
    // standing. As a single 7x5 m chunk the floor was all-or-nothing: anything
    // that freed it dropped the entire storey at once. Every tile is still
    // "Support:", so the building is just as solidly anchored until something
    // digs the ground out from under a specific tile.
    //
    // ~1.2 m tiles: small enough that a 3.5 m grenade crater takes a believable
    // bite, large enough to keep the chunk count (and the bond graph the
    // structural solver walks) modest.
    // Group ids for the tiled floors. Kept well clear of the per-board ids the
    // walls and roof allocate from `pieceId` below, so no floor ever shares a
    // group with a cladding plank.
    constexpr int kWoodFloorGroup = 9000;
    constexpr int kMetalFloorGroup = 9001;
    // All tiles of one floor share a single "@<id>" group. That group id is
    // what makes the floor behave as one thing: undermine any tile and the
    // whole floor is released together, so a hole under one corner drops the
    // storey instead of leaving the rest of the bricks hanging over open air
    // on the strength of their neighbours.
    const auto addTiledFoundation = [&](const char* prefix, int groupId,
                                        float x0, float x1, float y0, float y1,
                                        float z0, float z1) {
        constexpr float kTileTarget = 1.2f;
        const int tilesX = (std::max)(1, (int)std::lround((x1 - x0) / kTileTarget));
        const int tilesZ = (std::max)(1, (int)std::lround((z1 - z0) / kTileTarget));
        const float tileX = (x1 - x0) / tilesX;
        const float tileZ = (z1 - z0) / tilesZ;
        for (int tz = 0; tz < tilesZ; ++tz)
        for (int tx = 0; tx < tilesX; ++tx) {
            // "<name>@<id>": the chunk builder parses the numeric suffix into
            // plankGroup, and every tile of this floor carries the same one.
            const std::string tileName =
                std::string(prefix) + "@" + std::to_string(groupId);
            addBox(tileName.c_str(), matFoundation,
                   x0 + tx * tileX, x0 + (tx + 1) * tileX, y0, y1,
                   z0 + tz * tileZ, z0 + (tz + 1) * tileZ);
        }
    };

    // --- Foundation: anchored brick floor, tiled across the footprint. ---
    addTiledFoundation("Support:FoundationTile", kWoodFloorGroup, minX, maxX,
                       floorY - foundationEmbed, floorY + wall, minZ, maxZ);

    // --- Studded wall: vertical studs + outer cladding along one edge. The
    // bottom row of studs is anchored (the sill plate), so the wall stands. ---
    int pieceId = 0;  // unique Voronoi group id per board across the house
    // Windows occupy cladding rows 1..2 (a band from ~0.9 to ~2.15 above the
    // floor) so the cladding cutout lines up exactly with board seams.
    constexpr int boards = 5;         // cladding planks stacked up the wall
    constexpr int kWinRowLo = 1, kWinRowHi = 2;  // rows the window band covers
    // `windows` = horizontal span-offset ranges (start, end) along the wall.
    auto buildWall = [&](float x0, float x1, float z0, float z1, bool alongX,
                         float openStart, float openEnd, float openTop,
                         const std::vector<std::pair<float, float>>& windows) {
        constexpr float studW = 0.16f;
        constexpr int studCount = 8;
        const float baseY = floorY + wall;                 // sit on foundation
        const float rowH = (wallTop - baseY) / boards;
        const float winB = baseY + rowH * kWinRowLo;       // window band bottom
        const float winT = baseY + rowH * (kWinRowHi + 1); // window band top
        const float span = alongX ? (x1 - x0) : (z1 - z0);
        auto inWindow = [&](float c) {
            for (const auto& w : windows) if (c > w.first && c < w.second) return true;
            return false;
        };
        for (int s = 0; s <= studCount; ++s) {
            const float t = (float)s / studCount;
            const float c = t * span;
            // Skip studs that fall inside the opening (door/window gap).
            const bool inOpening = openEnd > openStart && c > openStart && c < openEnd;
            // Do not add anchored corner supports to the wooden house.
            if (s == 0) continue;
            // A stud crossing a window splits into a sill stub below the glass
            // and a header stub above it.
            const bool crossesWindow = inWindow(c);
            auto emitStud = [&](float sy0, float sy1) {
                if (sy1 <= sy0) return;
                const int id = pieceId++;
                const std::string studName = "Stud@" + std::to_string(id);
                if (alongX) {
                    const float sx = x0 + c;
                    addVoronoiBoard(studName.c_str(), matStud, sx - studW * 0.5f, sx + studW * 0.5f,
                                    sy0, sy1, z0, z1, id);
                } else {
                    const float sz = z0 + c;
                    addVoronoiBoard(studName.c_str(), matStud, x0, x1, sy0, sy1,
                                    sz - studW * 0.5f, sz + studW * 0.5f, id);
                }
            };
            if (inOpening) {
                // Header stub above the opening keeps the wall continuous up top.
                emitStud(openTop, wallTop);
            } else if (crossesWindow) {
                emitStud(baseY, winB);
                emitStud(winT, wallTop);
            } else {
                emitStud(baseY, wallTop);
            }
        }
        // Cladding: horizontal planks over the studs. Each plank is one visible
        // board built from flush Voronoi prism cells sharing one group id, so a
        // hit knocks a jagged cell out of the board instead of a straight strip.
        // Rows crossing the window band are split into segments around the glass.
        const float cladT = 0.08f;
        auto emitClad = [&](float c0, float c1, float by0, float by1) {
            if (c1 - c0 < 0.25f) return;  // skip slivers
            const int id = pieceId++;
            const std::string plankName = "Cladding@" + std::to_string(id);
            if (alongX) {
                const bool front = (z0 + z1) * 0.5f > (minZ + maxZ) * 0.5f;
                const float cz0 = front ? z1 : z0 - cladT;
                const float cz1 = front ? z1 + cladT : z0;
                addVoronoiBoard(plankName.c_str(), matCladding, x0 + c0, x0 + c1, by0, by1,
                                cz0, cz1, id);
            } else {
                const bool right = (x0 + x1) * 0.5f > (minX + maxX) * 0.5f;
                const float cx0 = right ? x1 : x0 - cladT;
                const float cx1 = right ? x1 + cladT : x0;
                addVoronoiBoard(plankName.c_str(), matCladding, cx0, cx1, by0, by1,
                                z0 + c0, z0 + c1, id);
            }
        };
        for (int b = 0; b < boards; ++b) {
            const float by0 = baseY + rowH * b;
            const float by1 = baseY + rowH * (b + 1);
            if (b >= kWinRowLo && b <= kWinRowHi && !windows.empty()) {
                // Cut the row around each window opening.
                float cursor = 0.0f;
                for (const auto& w : windows) {
                    emitClad(cursor, w.first, by0, by1);
                    cursor = w.second;
                }
                emitClad(cursor, span, by0, by1);
            } else {
                emitClad(0.0f, span, by0, by1);
            }
        }
        // Glass panes: one thin shatter-mode board per window, centred in the
        // wall so it bonds to the cladding edges and stud stubs around it.
        for (const auto& w : windows) {
            const int id = pieceId++;
            const std::string glassName = "Glass@" + std::to_string(id);
            constexpr float glassT = 0.015f;  // half thickness
            if (alongX) {
                const float zc = (z0 + z1) * 0.5f;
                addVoronoiBoard(glassName.c_str(), matGlass, x0 + w.first, x0 + w.second,
                                winB, winT, zc - glassT, zc + glassT, id, true);
            } else {
                const float xc = (x0 + x1) * 0.5f;
                addVoronoiBoard(glassName.c_str(), matGlass, xc - glassT, xc + glassT,
                                winB, winT, z0 + w.first, z0 + w.second, id, true);
            }
        }
    };

    // Front wall (+Z) with a door opening in the middle and a window either
    // side; one window on the back and each side wall.
    const float frontSpan = maxX - minX, sideSpan = maxZ - minZ;
    // Door head height is measured from the floor, not absolute -- otherwise the
    // opening stays at the old ground level when the building pad moves.
    buildWall(minX, maxX, maxZ - wall, maxZ, true, frontSpan * 0.42f, frontSpan * 0.58f,
              floorY + 2.2f,
              { { frontSpan * 0.10f, frontSpan * 0.30f }, { frontSpan * 0.70f, frontSpan * 0.90f } });
    buildWall(minX, maxX, minZ, minZ + wall, true, 0.0f, 0.0f, 0.0f,
              { { frontSpan * 0.38f, frontSpan * 0.62f } });                    // back
    buildWall(minX, minX + wall, minZ, maxZ, false, 0.0f, 0.0f, 0.0f,
              { { sideSpan * 0.32f, sideSpan * 0.68f } });                      // left
    buildWall(maxX - wall, maxX, minZ, maxZ, false, 0.0f, 0.0f, 0.0f,
              { { sideSpan * 0.32f, sideSpan * 0.68f } });                      // right

    // --- Roof: Crysis-style corrugated metal on two real angled slopes meeting
    // at a ridge. Each slope is a row of thin panels authored flat, then tilted
    // about its eave edge to the roof pitch. One "Roof@<id>" panel = one sheet
    // that tears off whole when hit. ---
    const float ridgeY = wallTop + 1.0f;          // ridge height above the eaves
    const float midX = (minX + maxX) * 0.5f;
    auto addWoodGable = [&](float nearZ, float farZ) {
        const int id = pieceId++;
        auto node = std::make_shared<SceneNode>("Cladding@" + std::to_string(id));
        node->mesh = std::make_shared<SceneMesh>();
        MeshPrimitive prim; prim.material = matCladding;
        auto tri = [&](XMFLOAT3 a, XMFLOAT3 b, XMFLOAT3 c) {
            const XMFLOAT3 ab(b.x-a.x,b.y-a.y,b.z-a.z), ac(c.x-a.x,c.y-a.y,c.z-a.z);
            XMFLOAT3 n(ab.y*ac.z-ab.z*ac.y, ab.z*ac.x-ab.x*ac.z, ab.x*ac.y-ab.y*ac.x);
            const float len=std::sqrt(n.x*n.x+n.y*n.y+n.z*n.z);
            if(len>0.00001f){n.x/=len;n.y/=len;n.z/=len;}
            for(const XMFLOAT3& p:{a,b,c}) {
                prim.vertices.insert(prim.vertices.end(), {p.x,p.y,p.z,n.x,n.y,n.z,
                    (p.x-minX)/(maxX-minX),(p.y-wallTop)/(ridgeY-wallTop),1,0,0,1});
                prim.indices.push_back((UINT)prim.indices.size());
            }
        };
        const XMFLOAT3 a0(minX,wallTop,nearZ), b0(maxX,wallTop,nearZ), c0(midX,ridgeY,nearZ);
        const XMFLOAT3 a1(minX,wallTop,farZ),  b1(maxX,wallTop,farZ),  c1(midX,ridgeY,farZ);
        tri(a0,b0,c0); tri(a1,c1,b1);                 // triangular faces
        tri(a0,a1,b1); tri(a0,b1,b0);                 // bottom
        tri(a0,c0,c1); tri(a0,c1,a1);                 // left roof edge
        tri(b0,b1,c1); tri(b0,c1,c0);                 // right roof edge
        node->mesh->primitives.push_back(std::move(prim));
        root->AddChild(node);
    };
    constexpr float gableThickness = 0.08f;
    addWoodGable(maxZ, maxZ + gableThickness);         // front triangle
    addWoodGable(minZ - gableThickness, minZ);         // back triangle

    const float halfW = midX - minX;              // horizontal run of one slope
    const float slopeLen = std::sqrt(halfW * halfW + (ridgeY - wallTop) * (ridgeY - wallTop));
    const float pitch = std::atan2(ridgeY - wallTop, halfW);  // slope angle
    constexpr int sheetsUp = 4;                   // panels up the slope
    constexpr int sheetsZ = 3;                    // panels along the roof depth
    constexpr float sheetT = 0.05f;               // thin metal sheet
    constexpr float overhang = 0.25f;             // panels jut past eave & gable
    constexpr float lapUp = 0.12f;                // each course laps onto the one below
    const float zLo = minZ - overhang, zHi = maxZ + overhang;
    const float zSpan = zHi - zLo;
    const float runStep = slopeLen / sheetsUp;
    // Build one slope: mirrorX flips it to the far side of the ridge. Panels are
    // authored in local space (x = up-slope run from the eave, y = thickness,
    // z = depth) then rotated by the pitch and moved onto the eave line.
    auto buildSlope = [&](bool mirror) {
        const float eaveX = mirror ? maxX : minX;
        // Local +x is "up the slope". Left slope rotates +pitch so +x runs
        // up-and-right to the ridge; the right slope uses (pi - pitch) so +x
        // runs up-and-LEFT to the same ridge. Z is untouched, so the panel's
        // depth stays axis-aligned.
        const XMMATRIX rot = XMMatrixRotationZ(mirror ? (3.14159265f - pitch) : pitch);
        const XMMATRIX place = rot * XMMatrixTranslation(eaveX, wallTop, 0.0f);
        for (int su = 0; su < sheetsUp; ++su) {
            const float run0 = su * runStep - overhang;              // start below eave
            const float run1 = (su + 1) * runStep + lapUp;           // lap onto next course
            for (int zi = 0; zi < sheetsZ; ++zi) {
                const float zr0 = zLo + zSpan * zi / sheetsZ;
                const float zr1 = zLo + zSpan * (zi + 1) / sheetsZ;
                const int id = pieceId++;
                addVoronoiBoard(("Roof@" + std::to_string(id)).c_str(), matMetalRoof,
                                run0, run1, 0.0f, sheetT, zr0, zr1, id, false, &place);
            }
        }
    };
    buildSlope(false);   // left slope (eave at minX)
    buildSlope(true);    // right slope (eave at maxX)

    // --- Second destructible shack from the Corrugated Metal Pack textures.
    // It lives next to the wooden house but is still part of the same Blast
    // asset, so bullets/grenades hit both buildings with one physics system.
    const float sx0 = 2.0f, sx1 = 7.5f;
    const float sz0 = 1.2f, sz1 = 5.9f;
    const float sy0 = floorY;
    const float slabTop = sy0 + 0.20f;
    // Relative to the slab, not absolute: an absolute eave height would leave the
    // metal shack behind at the old ground level when the pad moves.
    const float eaveY = sy0 + 2.85f;
    const float metalT = 0.06f;
    const float panelW = 0.42f;
    addTiledFoundation("Support:MetalFoundationTile", kMetalFloorGroup, sx0, sx1,
                       sy0 - foundationEmbed, slabTop, sz0, sz1);
    auto addMetalPanel = [&](float x0, float x1, float y0, float y1, float z0, float z1) {
        const int id = pieceId++;
        addVoronoiBoard(("MetalWall@" + std::to_string(id)).c_str(), matMetalWall,
                        x0, x1, y0, y1, z0, z1, id);
    };
    auto addDoorPanel = [&](float x0, float x1, float y0, float y1, float z0, float z1) {
        const int id = pieceId++;
        addVoronoiBoard(("DarkMetal@" + std::to_string(id)).c_str(), matDarkMetal,
                        x0, x1, y0, y1, z0, z1, id);
    };
    auto metalWallX = [&](float z, bool front) {
        const float outer0 = front ? z : z - metalT;
        const float outer1 = front ? z + metalT : z;
        for (float x = sx0; x < sx1 - 0.01f; x += panelW) {
            const float nx = (std::min)(x + panelW, sx1);
            const float c0 = x - sx0, c1 = nx - sx0;
            const bool door = front && c1 > 2.00f && c0 < 3.35f;
            if (door) {
                if (c0 < 2.00f) addMetalPanel(x, sx0 + 2.00f, slabTop, eaveY, outer0, outer1);
                if (c1 > 3.35f) addMetalPanel(sx0 + 3.35f, nx, slabTop, eaveY, outer0, outer1);
                // Door header, measured up from the slab -- an absolute height here
                // would leave the doorway behind when the pad's ground level moves.
                addMetalPanel((std::max)(x, sx0 + 2.00f), (std::min)(nx, sx0 + 3.35f),
                              sy0 + 2.15f, eaveY, outer0, outer1);
            } else {
                addMetalPanel(x, nx, slabTop, eaveY, outer0, outer1);
            }
        }
    };
    auto metalWallZ = [&](float x, bool right) {
        const float outer0 = right ? x : x - metalT;
        const float outer1 = right ? x + metalT : x;
        for (float z = sz0; z < sz1 - 0.01f; z += panelW) {
            const float nz = (std::min)(z + panelW, sz1);
            addMetalPanel(outer0, outer1, slabTop, eaveY, z, nz);
        }
    };
    metalWallX(sz1, true);
    metalWallX(sz0, false);
    metalWallZ(sx0, false);
    metalWallZ(sx1, true);
    const float doorZ0 = sz1 + 0.012f;
    const float doorZ1 = sz1 + metalT + 0.012f;
    addDoorPanel(sx0 + 2.02f, sx0 + 2.64f, slabTop, 2.15f, doorZ0, doorZ1);
    addDoorPanel(sx0 + 2.71f, sx0 + 3.33f, slabTop, 2.15f, doorZ0, doorZ1);

    const float shackMidX = (sx0 + sx1) * 0.5f;
    const float shackRidgeY = eaveY + 0.85f;
    const float shackHalfW = shackMidX - sx0;
    const float shackSlopeLen = std::sqrt(shackHalfW * shackHalfW + (shackRidgeY - eaveY) * (shackRidgeY - eaveY));
    const float shackPitch = std::atan2(shackRidgeY - eaveY, shackHalfW);
    const float shackZLo = sz0 - 0.28f, shackZHi = sz1 + 0.28f;
    const float roofStep = shackSlopeLen / 3.0f;
    auto addGable = [&](float z, bool front) {
        const float outer0 = front ? z : z - metalT;
        const float outer1 = front ? z + metalT : z;
        for (float x = sx0; x < sx1 - 0.01f; x += panelW) {
            const float nx = (std::min)(x + panelW, sx1);
            const float cx = (x + nx) * 0.5f;
            const float t = 1.0f - std::min(1.0f, std::abs(cx - shackMidX) / shackHalfW);
            const float top = eaveY + (shackRidgeY - eaveY) * t;
            if (top > eaveY + 0.10f) addMetalPanel(x, nx, eaveY, top, outer0, outer1);
        }
    };
    addGable(sz1, true);
    addGable(sz0, false);

    constexpr float trimT = 0.085f;
    addBox("MetalTrim@CornerFL", matTrim, sx0 - trimT, sx0 + trimT, slabTop, eaveY, sz1 - trimT, sz1 + trimT);
    addBox("MetalTrim@CornerFR", matTrim, sx1 - trimT, sx1 + trimT, slabTop, eaveY, sz1 - trimT, sz1 + trimT);
    addBox("MetalTrim@CornerBL", matTrim, sx0 - trimT, sx0 + trimT, slabTop, eaveY, sz0 - trimT, sz0 + trimT);
    addBox("MetalTrim@CornerBR", matTrim, sx1 - trimT, sx1 + trimT, slabTop, eaveY, sz0 - trimT, sz0 + trimT);
    addBox("MetalTrim@DoorL", matTrim, sx0 + 1.92f, sx0 + 2.02f, slabTop, 2.28f, sz1 + metalT, sz1 + metalT + 0.08f);
    addBox("MetalTrim@DoorR", matTrim, sx0 + 3.33f, sx0 + 3.43f, slabTop, 2.28f, sz1 + metalT, sz1 + metalT + 0.08f);
    addBox("MetalTrim@DoorTop", matTrim, sx0 + 1.92f, sx0 + 3.43f, 2.15f, 2.28f, sz1 + metalT, sz1 + metalT + 0.08f);
    addBox("MetalTrim@DoorSplit", matTrim, sx0 + 2.66f, sx0 + 2.72f, slabTop, 2.15f, sz1 + metalT + 0.01f, sz1 + metalT + 0.09f);
    addBox("MetalTrim@RidgeCap", matTrim, shackMidX - 0.09f, shackMidX + 0.09f, shackRidgeY - 0.05f, shackRidgeY + 0.08f, shackZLo, shackZHi);
    addBox("MetalTrim@LeftEave", matTrim, sx0 - 0.38f, sx0 - 0.20f, eaveY - 0.13f, eaveY + 0.03f, shackZLo, shackZHi);
    addBox("MetalTrim@RightEave", matTrim, sx1 + 0.20f, sx1 + 0.38f, eaveY - 0.13f, eaveY + 0.03f, shackZLo, shackZHi);
    addBox("MetalTrim@FrontFascia", matTrim, sx0 - 0.28f, sx1 + 0.28f, eaveY - 0.10f, eaveY + 0.05f, sz1 + 0.19f, sz1 + 0.31f);
    addBox("MetalTrim@BackFascia", matTrim, sx0 - 0.28f, sx1 + 0.28f, eaveY - 0.10f, eaveY + 0.05f, sz0 - 0.31f, sz0 - 0.19f);
    for (float x = sx0 + 0.25f; x < sx1 - 0.2f; x += 0.84f) {
        addBox("DarkMetal@ScrewFront", matDarkMetal, x, x + 0.055f, eaveY - 0.30f, eaveY - 0.23f, sz1 + metalT + 0.015f, sz1 + metalT + 0.04f);
        addBox("DarkMetal@ScrewFront", matDarkMetal, x, x + 0.055f, slabTop + 0.55f, slabTop + 0.62f, sz1 + metalT + 0.015f, sz1 + metalT + 0.04f);
    }

    auto buildMetalSlope = [&](bool mirror) {
        const float eaveX = mirror ? sx1 : sx0;
        const XMMATRIX rot = XMMatrixRotationZ(mirror ? (3.14159265f - shackPitch) : shackPitch);
        const XMMATRIX place = rot * XMMatrixTranslation(eaveX, eaveY, 0.0f);
        for (int up = 0; up < 3; ++up) {
            for (int zi = 0; zi < 3; ++zi) {
                const float zr0 = shackZLo + (shackZHi - shackZLo) * zi / 3.0f;
                const float zr1 = shackZLo + (shackZHi - shackZLo) * (zi + 1) / 3.0f;
                const int id = pieceId++;
                addVoronoiBoard(("MetalRoof@" + std::to_string(id)).c_str(), matMetalRoof,
                                up * roofStep - 0.22f, (up + 1) * roofStep + 0.12f,
                                0.0f, metalT, zr0, zr1, id, false, &place);
            }
        }
    };
    buildMetalSlope(false);
    buildMetalSlope(true);

    root->UpdateGlobalTransform(root->localTransform);
    return root;
}

// Reuse the authored wooden and metal house chunks as independent compounds.
// Vertex transforms are baked because the
// destruction system consumes child geometry directly rather than node poses.
static std::shared_ptr<SceneNode> CloneSceneTree(
    const std::shared_ptr<SceneNode>& source, int depth = 0) {
    if (!source || depth > 256) return {};   // guard cyclic/degenerate trees
    auto clone = std::make_shared<SceneNode>(source->name);
    clone->translation = source->translation;
    clone->rotation = source->rotation;
    clone->scale = source->scale;
    if (source->mesh)
        clone->mesh = std::make_shared<SceneMesh>(*source->mesh);
    for (const auto& child : source->children)
        clone->AddChild(CloneSceneTree(child, depth + 1));
    return clone;
}

// Bakes enabled structural prefabs into the destruction model so NvBlast
// fractures them for real instead of the mesh vanishing with a smoke puff.
//
// The GLB is one node with one mesh, so it carries none of the "Support:" /
// "<piece>@<id>" structure BuildChunks reads from authored houses. This slices it
// into horizontal bands by triangle centroid and names them to that convention:
// the bottom band becomes Support: (anchored, holding the mast up) and each band
// above is its own bonded group. Shoot the base out and the sections above lose
// their support chain and come down -- a structure toppling rather than popping.
//
// Called with the world-space vertex bake already applied, exactly like addHouse,
// because chunk bounds are read in destruction-model space.
static void AppendNvBlastPrefabsToDestruction(
        const std::shared_ptr<SceneNode>& root, int groupOffset) {
    if (!root) return;

    for (const LevelEntity& entity : g_game.world.Level().entities) {
        if (!entity.enabled || entity.type != LevelEntityType::Prefab ||
            !IsNvBlastStructurePrefab(entity.prefabId)) continue;

        // Fence panels stay one authored chunk and are cut apart at runtime;
        // the comm-tower objective is pre-sliced into bands, so it can lose its
        // base and topple section by section.
        const bool wholePanel = IsFencePrefab(entity.prefabId);
        // The 26 m mast becomes twelve roughly two-metre sections.
        const int bandCount = 12;

        // Same model the renderer uses, so the fractured geometry matches what
        // was standing there a frame earlier.
        const PrefabAsset* prefab = g_prefabRegistry.Find(entity.prefabId);
        if (!prefab) continue;
        PrefabModelCacheEntry* cached = LoadPrefabModel(*prefab);
        if (!cached || !cached->model) continue;
        const std::shared_ptr<SceneNode>& model = cached->model;

        // World transform for this instance, matching RebuildPrefabRenderBatches.
        const Transform& t = entity.transform;
        const XMMATRIX world = EntityWorldMatrix(t);
        const XMMATRIX rotationOnly = EulerDegreesToMatrix(t.rotation);

        // Flatten the model's primitives into world space first, so band
        // assignment and the emitted chunks share one coordinate system.
        struct WorldPrimitive {
            std::vector<float> vertices;
            std::vector<UINT> indices;
            std::shared_ptr<SceneMaterial> material;
            int materialIndex = 0;
        };
        std::vector<WorldPrimitive> flattened;
        float minY = FLT_MAX, maxY = -FLT_MAX;

        const std::function<void(const std::shared_ptr<SceneNode>&, const XMMATRIX&)>
            gather = [&](const std::shared_ptr<SceneNode>& node,
                         const XMMATRIX& parent) {
            if (!node) return;
            const XMMATRIX local = XMLoadFloat4x4(&node->localTransform) * parent;
            if (node->mesh) {
                for (const MeshPrimitive& source : node->mesh->primitives) {
                    if (source.indices.empty()) continue;
                    WorldPrimitive out;
                    out.vertices = source.vertices;
                    out.indices = source.indices;
                    out.material = source.material;
                    out.materialIndex = source.materialIndex;
                    const XMMATRIX toWorld = local * world;
                    for (size_t v = 0; v + 11 < out.vertices.size(); v += 12) {
                        XMVECTOR p = XMVectorSet(out.vertices[v],
                            out.vertices[v + 1], out.vertices[v + 2], 1.0f);
                        XMVECTOR n = XMVectorSet(out.vertices[v + 3],
                            out.vertices[v + 4], out.vertices[v + 5], 0.0f);
                        XMVECTOR tan = XMVectorSet(out.vertices[v + 8],
                            out.vertices[v + 9], out.vertices[v + 10], 0.0f);
                        p = XMVector3TransformCoord(p, toWorld);
                        n = XMVector3Normalize(XMVector3TransformNormal(n, rotationOnly));
                        tan = XMVector3Normalize(
                            XMVector3TransformNormal(tan, rotationOnly));
                        XMFLOAT3 pf, nf, tf;
                        XMStoreFloat3(&pf, p);
                        XMStoreFloat3(&nf, n);
                        XMStoreFloat3(&tf, tan);
                        out.vertices[v] = pf.x;
                        out.vertices[v + 1] = pf.y;
                        out.vertices[v + 2] = pf.z;
                        out.vertices[v + 3] = nf.x;
                        out.vertices[v + 4] = nf.y;
                        out.vertices[v + 5] = nf.z;
                        out.vertices[v + 8] = tf.x;
                        out.vertices[v + 9] = tf.y;
                        out.vertices[v + 10] = tf.z;
                        minY = (std::min)(minY, pf.y);
                        maxY = (std::max)(maxY, pf.y);
                    }
                    flattened.push_back(std::move(out));
                }
            }
            for (const auto& child : node->children) gather(child, local);
        };
        XMFLOAT4X4 identity;
        XMStoreFloat4x4(&identity, XMMatrixIdentity());
        model->UpdateGlobalTransform(identity);
        gather(model, XMMatrixIdentity());
        if (flattened.empty() || maxY <= minY) continue;

        if (wholePanel) {
            // Keep the complete structure as one anchored Blast chunk. Its
            // debris chunks are cut from these triangles only when an explosion
            // reaches it, avoiding per-instance split assets, meshlets, and GPU
            // buffers during level loading.
            auto chunk = std::make_shared<SceneNode>(
                "Support:FencePanel#" + std::to_string(groupOffset));
            chunk->mesh = std::make_shared<SceneMesh>();
            for (WorldPrimitive& source : flattened) {
                MeshPrimitive primitive;
                primitive.vertices = std::move(source.vertices);
                primitive.indices = std::move(source.indices);
                primitive.material = std::move(source.material);
                primitive.materialIndex = source.materialIndex;
                chunk->mesh->primitives.push_back(std::move(primitive));
            }
            root->AddChild(chunk);
            groupOffset += 1000000;
            continue;
        }

        // One child node per band, each carrying that band's triangles.
        const float bandHeight = (maxY - minY) / (float)bandCount;
        for (int band = 0; band < bandCount; ++band) {
            // Only the comm-tower objective reaches this banded path, and every
            // band carries ProtectedChunkMarker so indirect damage -- a
            // helicopter crashing nearby, a barrel chain, spreading fire --
            // cannot fell a player objective. The bottom band anchors the mast.
            const std::string marker = DestructionDX12::ProtectedChunkMarker;
            const std::string name = band == 0
                ? "Support:CommTowerBase" + marker + "@" +
                    std::to_string(groupOffset)
                : "CommTower" + marker + "@" +
                    std::to_string(groupOffset + band);
            auto chunk = std::make_shared<SceneNode>(name);
            chunk->mesh = std::make_shared<SceneMesh>();
            bool any = false;

            for (const WorldPrimitive& source : flattened) {
                MeshPrimitive primitive;
                primitive.material = source.material;
                primitive.materialIndex = source.materialIndex;
                for (size_t tri = 0; tri + 2 < source.indices.size(); tri += 3) {
                    const UINT i0 = source.indices[tri];
                    const UINT i1 = source.indices[tri + 1];
                    const UINT i2 = source.indices[tri + 2];
                    if ((size_t)(std::max)({ i0, i1, i2 }) * 12 + 11 >=
                        source.vertices.size()) continue;
                    const float cy = (source.vertices[(size_t)i0 * 12 + 1] +
                                      source.vertices[(size_t)i1 * 12 + 1] +
                                      source.vertices[(size_t)i2 * 12 + 1]) / 3.0f;
                    int owner = (int)((cy - minY) / (std::max)(0.0001f, bandHeight));
                    owner = (std::max)(0, (std::min)(bandCount - 1, owner));
                    if (owner != band) continue;
                    for (UINT sourceIndex : { i0, i1, i2 }) {
                        const UINT newIndex =
                            (UINT)(primitive.vertices.size() / 12);
                        const float* vertex =
                            &source.vertices[(size_t)sourceIndex * 12];
                        primitive.vertices.insert(primitive.vertices.end(),
                                                  vertex, vertex + 12);
                        primitive.indices.push_back(newIndex);
                    }
                    any = true;
                }
                if (!primitive.indices.empty())
                    chunk->mesh->primitives.push_back(std::move(primitive));
            }
            if (any) root->AddChild(chunk);
        }
        groupOffset += 1000000;
    }
}

static void ArrangeHousesInCross(const std::shared_ptr<SceneNode>& root,
                                 bool stressTest) {
    if (!root) return;
    std::vector<std::shared_ptr<SceneNode>> woodTemplate;
    std::vector<std::shared_ptr<SceneNode>> metalTemplate;

    auto meanX = [](const std::shared_ptr<SceneNode>& node) {
        double sum = 0.0;
        size_t count = 0;
        if (node && node->mesh) {
            for (const MeshPrimitive& primitive : node->mesh->primitives) {
                for (size_t v = 0; v + 11 < primitive.vertices.size(); v += 12) {
                    sum += primitive.vertices[v];
                    ++count;
                }
            }
        }
        return count ? static_cast<float>(sum / static_cast<double>(count)) : 0.0f;
    };

    // Original templates sit side-by-side: wood is left of x=1, metal right.
    for (const auto& child : root->children) {
        if (!child || !child->mesh) continue;
        (meanX(child) < 1.0f ? woodTemplate : metalTemplate).push_back(child);
    }
    root->children.clear();

    auto uniqueName = [](const std::string& source, int groupOffset, size_t ordinal) {
        const size_t at = source.rfind('@');
        if (at == std::string::npos)
            return source + "#" + std::to_string(groupOffset);
        bool numeric = at + 1 < source.size();
        for (size_t i = at + 1; i < source.size(); ++i)
            numeric = numeric && source[i] >= '0' && source[i] <= '9';
        if (numeric) {
            const int oldId = std::atoi(source.c_str() + at + 1);
            return source.substr(0, at + 1) + std::to_string(oldId + groupOffset);
        }
        return source + "@" + std::to_string(groupOffset + 500000 + ordinal);
    };

    auto addHouse = [&](const std::vector<std::shared_ptr<SceneNode>>& source,
                        float sourceX, float sourceZ, float targetX, float targetY,
                        float targetZ,
                        float yaw, int groupOffset) {
        const XMMATRIX transform =
            XMMatrixTranslation(-sourceX, 0.0f, -sourceZ) *
            XMMatrixRotationY(yaw) *
            XMMatrixTranslation(targetX, targetY, targetZ);
        const XMMATRIX rotation = XMMatrixRotationY(yaw);
        for (size_t childIndex = 0; childIndex < source.size(); ++childIndex) {
            const auto& sourceChild = source[childIndex];
            auto child = std::make_shared<SceneNode>(
                uniqueName(sourceChild->name, groupOffset, childIndex));
            child->mesh = std::make_shared<SceneMesh>();
            for (const MeshPrimitive& sourcePrimitive : sourceChild->mesh->primitives) {
                MeshPrimitive primitive;
                primitive.vertices = sourcePrimitive.vertices;
                primitive.indices = sourcePrimitive.indices;
                primitive.materialIndex = sourcePrimitive.materialIndex;
                primitive.material = sourcePrimitive.material;
                for (size_t v = 0; v + 11 < primitive.vertices.size(); v += 12) {
                    XMVECTOR p = XMVectorSet(primitive.vertices[v],
                        primitive.vertices[v + 1], primitive.vertices[v + 2], 1.0f);
                    XMVECTOR n = XMVectorSet(primitive.vertices[v + 3],
                        primitive.vertices[v + 4], primitive.vertices[v + 5], 0.0f);
                    XMVECTOR t = XMVectorSet(primitive.vertices[v + 8],
                        primitive.vertices[v + 9], primitive.vertices[v + 10], 0.0f);
                    p = XMVector3TransformCoord(p, transform);
                    n = XMVector3Normalize(XMVector3TransformNormal(n, rotation));
                    t = XMVector3Normalize(XMVector3TransformNormal(t, rotation));
                    XMFLOAT3 pf, nf, tf;
                    XMStoreFloat3(&pf, p); XMStoreFloat3(&nf, n); XMStoreFloat3(&tf, t);
                    primitive.vertices[v] = pf.x;
                    primitive.vertices[v + 1] = pf.y;
                    primitive.vertices[v + 2] = pf.z;
                    primitive.vertices[v + 3] = nf.x;
                    primitive.vertices[v + 4] = nf.y;
                    primitive.vertices[v + 5] = nf.z;
                    primitive.vertices[v + 8] = tf.x;
                    primitive.vertices[v + 9] = tf.y;
                    primitive.vertices[v + 10] = tf.z;
                }
                child->mesh->primitives.push_back(std::move(primitive));
            }
            root->AddChild(child);
        }
    };

    if (g_customLevelMode) {
        int groupOffset = 1000000;
        for (const LevelEntity& entity : g_game.world.Level().entities) {
            if (!entity.enabled ||
                (entity.type != LevelEntityType::WoodHouse &&
                 entity.type != LevelEntityType::MetalHouse)) continue;
            const bool wood = entity.type == LevelEntityType::WoodHouse;
            addHouse(wood ? woodTemplate : metalTemplate,
                wood ? -3.5f : 4.75f, wood ? 3.5f : 3.55f,
                entity.transform.position[0], entity.transform.position[1],
                entity.transform.position[2],
                XMConvertToRadians(entity.transform.rotation[1]), groupOffset);
            groupOffset += 1000000;
        }
        AppendNvBlastPrefabsToDestruction(root, groupOffset);
        XMFLOAT4X4 identity;
        XMStoreFloat4x4(&identity, XMMatrixIdentity());
        root->UpdateGlobalTransform(identity);
        return;
    }

    constexpr float radius = 9.0f;
    const size_t houseCount = stressTest ? kStressHouseCount : 4;
    size_t houseIndex = 0;
    const size_t compoundCount = (houseCount + kSpawnersPerCompound - 1) /
        kSpawnersPerCompound;
    for (size_t compoundIndex = 0; compoundIndex < compoundCount; ++compoundIndex) {
        const CompoundCenter& center = kStressCompoundCenters[compoundIndex];
        const int groupBase = static_cast<int>(compoundIndex) * 4000000;
        if (houseIndex++ < houseCount)
            addHouse(woodTemplate, -3.5f, 3.5f,
                     center.x, 0.0f, center.z + radius, 0.0f, groupBase + 1000000);
        if (houseIndex++ < houseCount)
            addHouse(metalTemplate, 4.75f, 3.55f,
                     center.x + radius, 0.0f, center.z, XM_PIDIV2, groupBase + 2000000);
        if (houseIndex++ < houseCount)
            addHouse(woodTemplate, -3.5f, 3.5f,
                     center.x, 0.0f, center.z - radius, XM_PI, groupBase + 3000000);
        if (houseIndex++ < houseCount)
            addHouse(metalTemplate, 4.75f, 3.55f,
                     center.x - radius, 0.0f, center.z, -XM_PIDIV2, groupBase + 4000000);
    }

    XMFLOAT4X4 identity;
    XMStoreFloat4x4(&identity, XMMatrixIdentity());
    root->UpdateGlobalTransform(identity);
}
