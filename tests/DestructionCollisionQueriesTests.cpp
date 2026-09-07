#include "DestructionCollisionQueries.h"
#include <cfloat>
#include <iostream>
#include <random>
#include <vector>

using namespace SGE::DestructionCollision;
struct Box { XMFLOAT3 lo, hi; };

int main() {
    std::mt19937 random(173);
    std::uniform_real_distribution<float> coordinate(-100.0f, 100.0f);
    std::uniform_real_distribution<float> extent(0.0f, 10.0f);
    uint64_t bruteTests = 0, prunedTests = 0;
    for (int scene = 0; scene < 100; ++scene) {
        std::vector<std::vector<Box>> actors(32);
        std::vector<Box> bounds;
        for (auto& actor : actors) {
            const XMFLOAT3 origin{ coordinate(random), coordinate(random), coordinate(random) };
            Box bound{ { FLT_MAX, FLT_MAX, FLT_MAX }, { -FLT_MAX, -FLT_MAX, -FLT_MAX } };
            for (int chunk = 0; chunk < 64; ++chunk) {
                Box box;
                box.lo = { origin.x + extent(random), origin.y + extent(random), origin.z + extent(random) };
                box.hi = { box.lo.x + extent(random), box.lo.y + extent(random), box.lo.z + extent(random) };
                bound.lo = { std::min(bound.lo.x, box.lo.x), std::min(bound.lo.y, box.lo.y), std::min(bound.lo.z, box.lo.z) };
                bound.hi = { std::max(bound.hi.x, box.hi.x), std::max(bound.hi.y, box.hi.y), std::max(bound.hi.z, box.hi.z) };
                actor.push_back(box);
            }
            bounds.push_back(bound);
        }
        for (int query = 0; query < 1000; ++query) {
            XMFLOAT3 a{ coordinate(random), coordinate(random), coordinate(random) };
            XMFLOAT3 b{ coordinate(random), coordinate(random), coordinate(random) };
            // Exercise zero-length, parallel, boundary and start-inside casts.
            if (query % 4 == 0) b = a;
            if (query % 4 == 1) b.x = a.x;
            if (query % 4 == 2) a = actors[query % actors.size()][0].lo;
            const float radius = query % 3 == 0 ? 0.0f : extent(random);
            float bruteT = FLT_MAX, prunedT = FLT_MAX;
            int bruteId = -1, prunedId = -1, id = 0;
            bool bruteSphere = false, prunedSphere = false;
            for (size_t actor = 0; actor < actors.size(); ++actor) {
                float actorT;
                const bool candidate = SegmentAabb(a, b, radius, bounds[actor].lo, bounds[actor].hi, actorT) && actorT < prunedT;
                const bool sphereCandidate = SphereAabb(a, radius, bounds[actor].lo, bounds[actor].hi);
                for (const Box& box : actors[actor]) {
                    float t;
                    ++bruteTests;
                    if (SegmentAabb(a, b, radius, box.lo, box.hi, t) && t < bruteT) { bruteT = t; bruteId = id; }
                    if (candidate) {
                        ++prunedTests;
                        if (SegmentAabb(a, b, radius, box.lo, box.hi, t) && t < prunedT) { prunedT = t; prunedId = id; }
                    }
                    bruteSphere |= SphereAabb(a, radius, box.lo, box.hi);
                    prunedSphere |= sphereCandidate && SphereAabb(a, radius, box.lo, box.hi);
                    ++id;
                }
            }
            if (bruteT != prunedT || bruteId != prunedId || bruteSphere != prunedSphere) {
                std::cerr << "Collision pruning changed query " << scene << ':' << query << '\n';
                return 1;
            }
        }
    }
    std::cout << "100000 queries match brute force; chunk segment tests: "
              << bruteTests << " -> " << prunedTests << '\n';
    return prunedTests < bruteTests ? 0 : 1;
}
