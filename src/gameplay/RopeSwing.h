#pragma once

// A heavy block hanging from a rope, strung up so it can be shot down.
//
// The rope is a chain of short dynamic links joined end-to-end with spherical
// (ball) joints: the top link is pinned to a static anchor, the block is welded
// under the bottom link. That makes the whole thing swing and twist under
// gravity like real slack rope, rather than a rigid pole.
//
// Shooting it does one of two things:
//   * hit a rope link  -> destroy the joint above that link. The rope parts, and
//                         everything below (the rest of the links plus the block)
//                         falls. This is the "shoot the rope, drop the block" bit.
//   * hit the block    -> shove it along the bullet direction so it swings.
//
// It owns a private Box3D world. The water pool already has one, but that world
// is bounded by the pool basin and runs buoyancy on every body in it, neither of
// which a rope wants -- so this keeps its own rather than contaminate that one.

#include <DirectXMath.h>
#include <box3d/box3d.h>
#include <vector>
#include <cmath>
#include <cfloat>
#include <algorithm>
#include <cstdint>

using namespace DirectX;

// One drawable piece of the rig (a rope link or the block), as a world transform
// plus a colour -- same shape as the water's floater items so it draws with the
// ordinary textured-cube path.
struct RopeItem {
    XMFLOAT4X4 transform;
    XMFLOAT3   color;
    uint8_t    shape = 0; // 0 box, 1 capsule, 2 sphere
};

class RopeSwing {
public:
    // What hangs on the end of the rope.
    enum class Payload {
        Block,     // a heavy stone cube
        Ragdoll,   // a jointed body, strung up by the torso
        None,      // nothing welded on: the bottom link is the attach point
    };

    // `anchor` is the fixed point the rope hangs from (e.g. under a beam).
    // The rope drops straight down from there, with the payload on the end.
    // Fewer, longer links than a real rope would have: every extra joint is more
    // constraint error for the solver to accumulate, and a 6-link chain still
    // reads as slack rope while staying rock solid.
    void Initialize(const XMFLOAT3& anchor,
                    int   linkCount  = 6,
                    float linkLength = 0.5f,
                    float blockHalf  = 0.55f,
                    Payload payload  = Payload::Block) {
        Shutdown();

        m_anchor     = anchor;
        m_linkHalfY  = linkLength * 0.5f;
        m_blockHalf  = XMFLOAT3(blockHalf, blockHalf, blockHalf);
        m_payload    = payload;

        b3WorldDef worldDef = b3DefaultWorldDef();
        worldDef.gravity = { 0.0f, -9.81f, 0.0f };
        m_world = b3CreateWorld(&worldDef);
        if (B3_IS_NULL(m_world)) return;

        // Static anchor body: the thing the rope is tied to. No shape, so it is
        // invisible and collides with nothing -- it exists purely as a joint end.
        b3BodyDef anchorDef = b3DefaultBodyDef();
        anchorDef.position = { anchor.x, anchor.y, anchor.z };
        m_anchorBody = b3CreateBody(m_world, &anchorDef);

        // Rope links, hung one under the next.
        b3BodyId prev = m_anchorBody;
        float prevHalf = 0.0f;                 // anchor is a point
        float y = anchor.y;

        for (int i = 0; i < linkCount; ++i) {
            // Each link's centre sits one half-length below the previous joint.
            y -= m_linkHalfY + (i == 0 ? 0.0f : m_linkHalfY);

            b3BodyDef bd = b3DefaultBodyDef();
            bd.type = b3_dynamicBody;
            bd.position = { anchor.x, y, anchor.z };
            // Damping bleeds off energy so the rope settles instead of flailing
            // forever once it has been hit. Generous, because a long ball-joint
            // chain will happily pump itself into oscillation otherwise.
            bd.linearDamping  = 0.6f;
            bd.angularDamping = 0.9f;
            b3BodyId link = b3CreateBody(m_world, &bd);

            b3ShapeDef sd = b3DefaultShapeDef();
            // Rope links must not be featherweights next to the block: a ball-joint
            // chain cannot hold a mass ratio in the thousands (the joints stretch,
            // overshoot, and the rope whips itself apart). The links are thin, so
            // reach a sane per-link mass via a high density rather than fat geometry.
            sd.density = 8000.0f;
            sd.baseMaterial.friction = 0.5f;
            sd.baseMaterial.restitution = 0.0f;
            b3BoxHull hull = b3MakeBoxHull(kLinkHalfXZ, m_linkHalfY, kLinkHalfXZ);
            b3CreateHullShape(link, &sd, &hull.base);

            // Ball joint at the seam between prev and this link. Frames are local,
            // so: bottom of prev, top of this one.
            m_joints.push_back(MakeBallJoint(prev, link, prevHalf, m_linkHalfY));

            m_links.push_back(link);
            prev = link;
            prevHalf = m_linkHalfY;
        }

        if (payload == Payload::Block) {
            // The block, hung off the last link. Dense, so it swings with real
            // weight and yanks the rope around convincingly.
            const float blockY = y - m_linkHalfY - blockHalf;
            b3BodyDef bbd = b3DefaultBodyDef();
            bbd.type = b3_dynamicBody;
            bbd.position = { anchor.x, blockY, anchor.z };
            bbd.linearDamping  = 0.25f;
            bbd.angularDamping = 0.5f;
            m_block = b3CreateBody(m_world, &bbd);

            b3ShapeDef bsd = b3DefaultShapeDef();
            // Heavy enough to swing with authority, light enough that the rope above
            // can actually hold it. Keep this within ~50x the per-link mass.
            bsd.density = 150.0f;
            bsd.baseMaterial.friction = 0.7f;
            bsd.baseMaterial.restitution = 0.05f;
            b3BoxHull bhull = b3MakeBoxHull(blockHalf, blockHalf, blockHalf);
            b3CreateHullShape(m_block, &bsd, &bhull.base);

            // Join block to the bottom link. Ball joint again, so it can sway.
            if (!m_links.empty())
                m_joints.push_back(MakeBallJoint(prev, m_block, m_linkHalfY, blockHalf));
        } else if (payload == Payload::Ragdoll) {
            // Ragdoll strung up by the torso. The rope's last link ball-joints to
            // the torso, so the body dangles and swings; cut the rope and it drops
            // in a heap.
            BuildRagdoll(XMFLOAT3(anchor.x, y - m_linkHalfY, anchor.z), prev);
        }
        // Payload::None hangs nothing on the end. A rappelling player is driven
        // from the rope's shape rather than simulated as a body on it, so there
        // is no payload to weld: the free bottom link is the attach point, and
        // the chain still swings under its own mass.

        // Ground plane, so the payload has something to land on when cut loose.
        b3BodyDef gd = b3DefaultBodyDef();
        gd.position = { anchor.x, m_groundY - kGroundHalf, anchor.z };
        b3BodyId ground = b3CreateBody(m_world, &gd);
        b3BoxHull ghull = b3MakeBoxHull(40.0f, kGroundHalf, 40.0f);
        b3ShapeDef gsd = b3DefaultShapeDef();
        gsd.baseMaterial.friction = 0.8f;
        b3CreateHullShape(ground, &gsd, &ghull.base);

        m_pathScratch.reserve(m_links.size() + 2);
        m_spanScratch.reserve(m_links.size() + 2);

        RebuildItems();
    }

    // Ground height the block falls to. Set before Initialize to match terrain.
    void SetGroundY(float y) { m_groundY = y; }

    void Update(float dt) {
        if (B3_IS_NULL(m_world)) return;
        m_accumulator = std::min(0.1f, m_accumulator + dt);
        constexpr float step = 1.0f / 60.0f;
        while (m_accumulator >= step) {
            // A joint chain needs far more solver iterations than loose bodies do;
            // at 4 the constraints stay visibly stretchy and the rope oscillates.
            b3World_Step(m_world, step, 16);
            m_accumulator -= step;
        }
        RebuildItems();
    }

    bool BlocksSegment(const XMFLOAT3& start, const XMFLOAT3& end,
                       float radius) const {
        if (B3_IS_NULL(m_world)) return false;
        const XMVECTOR a = XMLoadFloat3(&start);
        const XMVECTOR b = XMLoadFloat3(&end);
        const XMVECTOR ab = b - a;
        const float abLenSq = std::max(1e-6f, XMVectorGetX(XMVector3LengthSq(ab)));
        auto blocked = [&](b3BodyId body, float bodyRadius) {
            if (B3_IS_NULL(body)) return false;
            const b3Pos p = b3Body_GetPosition(body);
            const XMVECTOR center =
                XMVectorSet((float)p.x, (float)p.y, (float)p.z, 0.0f);
            float t = XMVectorGetX(XMVector3Dot(center - a, ab)) / abLenSq;
            t = std::max(0.0f, std::min(1.0f, t));
            return XMVectorGetX(XMVector3Length(center - (a + ab * t))) <=
                   radius + bodyRadius;
        };
        for (b3BodyId link : m_links)
            if (blocked(link, kLinkHitRadius)) return true;
        if (blocked(m_block, m_blockHalf.x)) return true;
        for (const BodyPart& part : m_ragdoll) {
            const float partRadius =
                std::max({ part.half.x, part.half.y, part.half.z });
            if (blocked(part.body, partRadius)) return true;
        }
        return false;
    }

    // Shoot the rig. If the segment start->end passes within `radius` of a rope
    // link, the joint holding that link up is destroyed and everything below it
    // drops. If it passes the block instead, the block just gets shoved.
    // `hitPos` receives the struck body's position (for impact FX).
    bool Shoot(const XMFLOAT3& start, const XMFLOAT3& end,
               const XMFLOAT3& direction, float radius, float impulse,
               XMFLOAT3& hitPos) {
        if (B3_IS_NULL(m_world)) return false;

        const XMVECTOR a = XMLoadFloat3(&start);
        const XMVECTOR b = XMLoadFloat3(&end);
        const XMVECTOR ab = b - a;
        const float abLenSq = std::max(1e-6f, XMVectorGetX(XMVector3LengthSq(ab)));

        // Closest struck thing along the bullet's path wins, so a shot that passes
        // rope *and* payload hits whichever it reaches first. A rope link is the
        // only thing that CUTS; everything else just takes a shove.
        float    bestT = FLT_MAX;
        int      bestLink = -1;                 // >= 0 only when a rope link won
        b3BodyId bestBody = b3_nullBodyId;

        auto testBody = [&](b3BodyId body, float bodyRadius, int linkIndex) {
            if (B3_IS_NULL(body)) return;
            const b3Pos p = b3Body_GetPosition(body);
            const XMVECTOR c = XMVectorSet((float)p.x, (float)p.y, (float)p.z, 0.0f);
            float t = XMVectorGetX(XMVector3Dot(c - a, ab)) / abLenSq;
            t = std::max(0.0f, std::min(1.0f, t));
            const XMVECTOR closest = a + ab * t;
            const float d = XMVectorGetX(XMVector3Length(c - closest));
            if (d > radius + bodyRadius || t >= bestT) return;
            bestT = t;
            bestLink = linkIndex;
            bestBody = body;
        };

        for (int i = 0; i < (int)m_links.size(); ++i) {
            if (B3_IS_NULL(m_links[i])) continue;
            // Links are thin; be a bit generous so the rope is actually hittable.
            testBody(m_links[i], kLinkHitRadius, i);
        }
        testBody(m_block, m_blockHalf.x, -1);
        // Ragdoll limbs: hitting one kicks that limb, so the body jerks and spins
        // on the rope. Use each part's largest half-extent as its hit radius.
        for (const BodyPart& part : m_ragdoll) {
            const float r = std::max({ part.half.x, part.half.y, part.half.z });
            testBody(part.body, r, -1);
        }

        if (B3_IS_NULL(bestBody)) return false;

        if (bestLink < 0) {
            // Block or body part: just shove it. Nothing is cut.
            const b3Pos p = b3Body_GetPosition(bestBody);
            hitPos = XMFLOAT3((float)p.x, (float)p.y, (float)p.z);
            const b3Vec3 imp = { direction.x * impulse,
                                 direction.y * impulse,
                                 direction.z * impulse };
            b3Body_ApplyLinearImpulseToCenter(bestBody, imp, true);
            return true;
        }

        // Rope hit: cut it. Joint i is the one *above* link i (the anchor joint
        // is index 0), so destroying it frees link i and everything under it.
        const b3Pos p = b3Body_GetPosition(m_links[bestLink]);
        hitPos = XMFLOAT3((float)p.x, (float)p.y, (float)p.z);

        if (bestLink < (int)m_joints.size() && !B3_IS_NULL(m_joints[bestLink])) {
            // wakeAttached: the freed links must wake, or they hang in mid-air.
            b3DestroyJoint(m_joints[bestLink], true);
            m_joints[bestLink] = b3_nullJointId;
            m_cut = true;
        }

        // Give the freed section a nudge so the break reads as a break.
        const b3Vec3 imp = { direction.x * impulse * 0.25f,
                             direction.y * impulse * 0.25f,
                             direction.z * impulse * 0.25f };
        b3Body_ApplyLinearImpulseToCenter(m_links[bestLink], imp, true);
        return true;
    }

    const std::vector<RopeItem>& GetItems() const { return m_items; }
    bool IsInitialized() const { return !B3_IS_NULL(m_world); }
    bool IsCut() const { return m_cut; }

    // A point on the rope, `t` running 0 at the anchor to 1 at the free end.
    // Used to slide a rappelling player down the live rope instead of down a
    // straight line, so they swing with it in X and Z as well as descending.
    //
    // The path is anchor -> every link centre -> the bottom link's lower tip,
    // rather than the centres alone: a chain of N centres spans only from half a
    // link below the anchor to half a link above the end, which would lose a
    // whole link's length across the descent and leave the player hanging short
    // of the ground.
    XMFLOAT3 PositionAlongRope(float t) const {
        if (m_links.empty()) return m_anchor;

        // Scratch buffers are members, not locals: this runs every frame of a
        // descent and must not allocate.
        std::vector<XMFLOAT3>& path = m_pathScratch;
        path.clear();
        path.push_back(m_anchor);
        for (b3BodyId link : m_links) {
            if (B3_IS_NULL(link)) continue;
            const b3Pos p = b3Body_GetPosition(link);
            path.push_back(XMFLOAT3((float)p.x, (float)p.y, (float)p.z));
        }
        if (path.size() < 2) return m_anchor;

        // Extend past the last centre by half a link, along the direction the
        // final segment is already running, so the tip follows the rope's lean
        // instead of always pointing straight down.
        {
            const XMVECTOR last = XMLoadFloat3(&path[path.size() - 1]);
            const XMVECTOR prev = XMLoadFloat3(&path[path.size() - 2]);
            XMVECTOR dir = last - prev;
            if (XMVectorGetX(XMVector3LengthSq(dir)) < 1e-8f)
                dir = XMVectorSet(0.0f, -1.0f, 0.0f, 0.0f);
            dir = XMVector3Normalize(dir);
            XMFLOAT3 tip{};
            XMStoreFloat3(&tip, last + dir * m_linkHalfY);
            path.push_back(tip);
        }

        // Walk by arc length, so an unevenly stretched chain still maps `t`
        // onto a proportional distance down the rope.
        float total = 0.0f;
        std::vector<float>& spans = m_spanScratch;
        spans.clear();
        for (size_t i = 0; i + 1 < path.size(); ++i) {
            const float d = XMVectorGetX(XMVector3Length(
                XMLoadFloat3(&path[i + 1]) - XMLoadFloat3(&path[i])));
            spans.push_back(d);
            total += d;
        }
        if (total <= 1e-6f) return path.front();

        const float clamped = std::max(0.0f, std::min(1.0f, t));
        float travelled = clamped * total;
        for (size_t i = 0; i < spans.size(); ++i) {
            if (travelled > spans[i] && i + 1 < spans.size()) {
                travelled -= spans[i];
                continue;
            }
            const float frac = spans[i] > 1e-6f
                ? std::min(1.0f, travelled / spans[i]) : 0.0f;
            XMFLOAT3 out{};
            XMStoreFloat3(&out, XMVectorLerp(XMLoadFloat3(&path[i]),
                                             XMLoadFloat3(&path[i + 1]), frac));
            return out;
        }
        return path.back();
    }

    // World-space anchor the rope was hung from.
    const XMFLOAT3& Anchor() const { return m_anchor; }

    // True once a cut rope has stopped moving, so the caller knows the fall has
    // finished playing out and the world can be torn down. Checks the freed
    // section only -- an uncut rope hanging in the breeze never reports settled.
    bool CutSectionSettled() const {
        if (!m_cut) return false;
        constexpr float kRestSpeedSq = 0.04f;   // ~0.2 m/s
        for (b3BodyId link : m_links) {
            if (B3_IS_NULL(link)) continue;
            const b3Vec3 v = b3Body_GetLinearVelocity(link);
            if (v.x * v.x + v.y * v.y + v.z * v.z > kRestSpeedSq) return false;
        }
        return true;
    }

    // Re-hang the payload: rebuild from the original anchor. Keeps the payload
    // kind, or a reset gibbet would silently come back as a block.
    void Reset() {
        const XMFLOAT3 anchor = m_anchor;
        const int   links = (int)m_links.size();
        const float linkLen = m_linkHalfY * 2.0f;
        const float half = m_blockHalf.x;
        const Payload payload = m_payload;
        Initialize(anchor, links ? links : 6, linkLen > 0.0f ? linkLen : 0.5f,
                   half > 0.0f ? half : 0.55f, payload);
    }

    void Shutdown() {
        if (!B3_IS_NULL(m_world)) b3DestroyWorld(m_world);
        m_world = b3_nullWorldId;
        m_anchorBody = b3_nullBodyId;
        m_block = b3_nullBodyId;
        m_links.clear();
        m_joints.clear();
        m_ragdoll.clear();
        m_items.clear();
        m_pathScratch.clear();
        m_spanScratch.clear();
        m_accumulator = 0.0f;
        m_cut = false;
    }

    ~RopeSwing() { Shutdown(); }

private:
    static constexpr float kLinkHalfXZ    = 0.045f;  // rope thickness
    static constexpr float kLinkHitRadius = 0.16f;   // generous, so rope is shootable
    static constexpr float kGroundHalf    = 2.0f;
    static constexpr float kBodyDensity   = 350.0f;  // see BuildRagdoll: mass ratio

    // An 11-part ragdoll hung by the torso from `ropeEnd` (the bottom rope link).
    // The skeleton mirrors the one the destruction sim already uses, so it is a
    // known-stable joint layout in this engine.
    //
    // Part density is the thing to be careful with. A rope link here is ~16 kg
    // (thin, but very dense on purpose -- see the link shape def). If the body
    // parts were realistic flesh density the torso would outweigh a link enough to
    // stretch the chain; kept at ~350 the heaviest part lands near 40 kg, a ~2.5:1
    // ratio the solver holds without complaint.
    void BuildRagdoll(const XMFLOAT3& hangPoint, b3BodyId ropeEnd) {
        struct PartDef { XMFLOAT3 center, half, color; uint8_t shape; };
        const XMFLOAT3 skin{ 0.62f, 0.39f, 0.27f };
        const XMFLOAT3 shirt{ 0.12f, 0.24f, 0.42f };
        const XMFLOAT3 pants{ 0.10f, 0.11f, 0.13f };

        // Centres are relative to the torso's own centre, which we place just
        // under the rope end so the figure hangs from its chest.
        const PartDef defs[] = {
            {{0,1.45f,0},{0.28f,0.38f,0.16f},shirt,1},   // 0 torso
            {{0,0.92f,0},{0.23f,0.16f,0.15f},pants,1},   // 1 pelvis
            {{0,2.02f,0},{0.18f,0.22f,0.18f},skin,2},    // 2 head
            {{-0.39f,1.48f,0},{0.12f,0.30f,0.11f},shirt,1}, // 3 upper arm L
            {{-0.39f,0.94f,0},{0.10f,0.27f,0.09f},skin,1},  // 4 forearm L
            {{ 0.39f,1.48f,0},{0.12f,0.30f,0.11f},shirt,1}, // 5 upper arm R
            {{ 0.39f,0.94f,0},{0.10f,0.27f,0.09f},skin,1},  // 6 forearm R
            {{-0.15f,0.53f,0},{0.14f,0.28f,0.13f},pants,1}, // 7 thigh L
            {{-0.15f,0.04f,0},{0.11f,0.25f,0.10f},pants,1}, // 8 shin L
            {{ 0.15f,0.53f,0},{0.14f,0.28f,0.13f},pants,1}, // 9 thigh R
            {{ 0.15f,0.04f,0},{0.11f,0.25f,0.10f},pants,1}, // 10 shin R
        };
        struct Link { int a, b; XMFLOAT3 anchor; };
        const Link links[] = {
            {0,1,{0,1.08f,0}}, {0,2,{0,1.82f,0}},
            {0,3,{-0.29f,1.68f,0}}, {3,4,{-0.38f,1.20f,0}},
            {0,5,{ 0.29f,1.68f,0}}, {5,6,{ 0.38f,1.20f,0}},
            {1,7,{-0.15f,0.76f,0}}, {7,8,{-0.15f,0.28f,0}},
            {1,9,{ 0.15f,0.76f,0}}, {9,10,{0.15f,0.28f,0}},
        };

        // Shift the whole figure so the torso's top edge meets the rope end.
        const XMFLOAT3& torso = defs[0].center;
        const float torsoTop = torso.y + defs[0].half.y;
        const XMFLOAT3 origin(hangPoint.x, hangPoint.y - torsoTop, hangPoint.z);

        for (const PartDef& def : defs) {
            b3BodyDef bd = b3DefaultBodyDef();
            bd.type = b3_dynamicBody;
            bd.position = { origin.x + def.center.x,
                            origin.y + def.center.y,
                            origin.z + def.center.z };
            bd.linearDamping  = 0.2f;
            bd.angularDamping = 0.5f;
            b3BodyId body = b3CreateBody(m_world, &bd);

            b3ShapeDef sd = b3DefaultShapeDef();
            sd.density = kBodyDensity;
            sd.baseMaterial.friction = 0.72f;
            sd.baseMaterial.restitution = 0.02f;
            b3BoxHull box = b3MakeBoxHull(def.half.x, def.half.y, def.half.z);
            b3CreateHullShape(body, &sd, &box.base);

            m_ragdoll.push_back({ body, def.half, def.color, def.shape });
        }

        // Cone/twist limits keep the joints from folding through themselves, which
        // is what separates a ragdoll from a bag of flailing boxes.
        for (const Link& l : links) {
            b3SphericalJointDef jd = b3DefaultSphericalJointDef();
            jd.base.bodyIdA = m_ragdoll[l.a].body;
            jd.base.bodyIdB = m_ragdoll[l.b].body;
            const XMFLOAT3& ca = defs[l.a].center;
            const XMFLOAT3& cb = defs[l.b].center;
            jd.base.localFrameA.p = { l.anchor.x-ca.x, l.anchor.y-ca.y, l.anchor.z-ca.z };
            jd.base.localFrameB.p = { l.anchor.x-cb.x, l.anchor.y-cb.y, l.anchor.z-cb.z };
            jd.base.collideConnected = false;
            jd.enableConeLimit = true;  jd.coneAngle = 1.15f;
            jd.enableTwistLimit = true; jd.lowerTwistAngle = -0.65f;
                                        jd.upperTwistAngle =  0.65f;
            b3CreateSphericalJoint(m_world, &jd);
        }

        // Finally: tie the torso to the bottom of the rope. This joint is part of
        // m_joints, so cutting the rope above it drops the whole body.
        if (!B3_IS_NULL(ropeEnd) && !m_ragdoll.empty()) {
            b3SphericalJointDef jd = b3DefaultSphericalJointDef();
            jd.base.bodyIdA = ropeEnd;
            jd.base.bodyIdB = m_ragdoll[0].body;
            jd.base.localFrameA.p = { 0.0f, -m_linkHalfY, 0.0f };
            jd.base.localFrameB.p = { 0.0f,  defs[0].half.y, 0.0f };
            jd.base.collideConnected = false;
            m_joints.push_back(b3CreateSphericalJoint(m_world, &jd));
        }
    }

    // Ball joint between two bodies, attached at the facing ends: the bottom of
    // A (halfA below its centre) meeting the top of B (halfB above its centre).
    b3JointId MakeBallJoint(b3BodyId bodyA, b3BodyId bodyB, float halfA, float halfB) {
        b3SphericalJointDef jd = b3DefaultSphericalJointDef();
        jd.base.bodyIdA = bodyA;
        jd.base.bodyIdB = bodyB;
        jd.base.localFrameA.p = { 0.0f, -halfA, 0.0f };
        jd.base.localFrameB.p = { 0.0f,  halfB, 0.0f };
        // Neighbouring links must not collide, or the chain fights itself.
        jd.base.collideConnected = false;
        return b3CreateSphericalJoint(m_world, &jd);
    }

    void RebuildItems() {
        m_items.clear();

        auto push = [&](b3BodyId body, const XMFLOAT3& half, const XMFLOAT3& color,
                        uint8_t shape = 0) {
            if (B3_IS_NULL(body)) return;
            const b3Pos  p = b3Body_GetPosition(body);
            const b3Quat q = b3Body_GetRotation(body);
            const XMVECTOR rot = XMVectorSet(q.v.x, q.v.y, q.v.z, q.s);
            const XMMATRIX scale = shape == 1
                ? XMMatrixScaling(half.x * 4.0f, half.y * 2.15f, half.z * 4.0f)
                : XMMatrixScaling(half.x * 2.08f, half.y * 2.08f, half.z * 2.08f);
            const XMMATRIX t = scale *
                XMMatrixRotationQuaternion(rot) *
                XMMatrixTranslation((float)p.x, (float)p.y, (float)p.z);
            RopeItem item;
            XMStoreFloat4x4(&item.transform, t);
            item.color = color;
            item.shape = shape;
            m_items.push_back(item);
        };

        const XMFLOAT3 linkHalf(kLinkHalfXZ, m_linkHalfY, kLinkHalfXZ);
        const XMFLOAT3 ropeColor(0.62f, 0.51f, 0.33f);   // hemp
        const XMFLOAT3 blockColor(0.42f, 0.44f, 0.48f);  // grey stone

        m_items.reserve(m_links.size() + m_ragdoll.size() + 1);
        for (b3BodyId link : m_links) push(link, linkHalf, ropeColor);
        push(m_block, m_blockHalf, blockColor);
        for (const BodyPart& part : m_ragdoll)
            push(part.body, part.half, part.color, part.shape);
    }

    struct BodyPart {
        b3BodyId body = b3_nullBodyId;
        XMFLOAT3 half{};
        XMFLOAT3 color{};
        uint8_t shape = 0;
    };

    b3WorldId m_world = b3_nullWorldId;
    b3BodyId  m_anchorBody = b3_nullBodyId;
    b3BodyId  m_block = b3_nullBodyId;
    std::vector<b3BodyId>  m_links;
    std::vector<b3JointId> m_joints;   // m_joints[i] holds up m_links[i]
    std::vector<BodyPart>  m_ragdoll;  // empty unless Payload::Ragdoll
    std::vector<RopeItem>  m_items;
    // Scratch for PositionAlongRope, which is const but runs per frame.
    mutable std::vector<XMFLOAT3> m_pathScratch;
    mutable std::vector<float>    m_spanScratch;

    Payload  m_payload = Payload::Block;
    XMFLOAT3 m_anchor{ 0.0f, 0.0f, 0.0f };
    XMFLOAT3 m_blockHalf{ 0.5f, 0.5f, 0.5f };
    float m_linkHalfY = 0.16f;
    float m_groundY = 0.0f;
    float m_accumulator = 0.0f;
    bool  m_cut = false;
};
