#include "VirtualShadowPages.h"
#include <cstdlib>
#include <iostream>

int main() {
    using namespace VirtualShadows;
    auto require = [](bool ok) {
        if (!ok) { std::cerr << "VSM coverage regression\n"; std::exit(1); }
    };
    for (float x : {-192.01f, -96.0f, -12.01f, -0.01f, 0.0f, 5.99f, 12.0f, 192.01f}) {
        for (float y : {-192.01f, -96.0f, -0.01f, 0.0f, 6.0f, 96.0f, 192.01f}) {
            std::array<uint32_t, Capacity> requests;
            for (uint32_t budget = 1; budget <= Capacity; ++budget) {
                const auto count = BuildRequests(x, y, budget, requests);
                require(count == budget);
                for (uint32_t i = 0; i < count; ++i) {
                    require(requests[i] != Invalid);
                    for (uint32_t j = 0; j < i; ++j) require(requests[i] != requests[j]);
                }
                const auto viewerPage = [&](uint32_t level) {
                    return Key(level, (int)std::floor(x / PageExtent(level)),
                                      (int)std::floor(y / PageExtent(level)));
                };
                bool viewerCovered = false;
                for (uint32_t level = 0; level < Levels; ++level)
                    viewerCovered |= std::find(requests.begin(), requests.end(),
                                               viewerPage(level)) != requests.end();
                require(viewerCovered);
                if (budget >= 5)
                    require(std::find(requests.begin(), requests.end(), viewerPage(0)) != requests.end());
                if (budget >= 6)
                    for (uint32_t level = 0; level < Levels; ++level)
                        require(std::find(requests.begin(), requests.end(), viewerPage(level)) != requests.end());

                if (budget >= 4) {
                    for (int dy = -90; dy <= 90; dy += 5) {
                        for (int dx = -90; dx <= 90; dx += 5) {
                            bool covered = false;
                            for (uint32_t level = 0; level < Levels; ++level) {
                                const auto key = Key(level, (int)std::floor((x + dx) / PageExtent(level)),
                                                           (int)std::floor((y + dy) / PageExtent(level)));
                                covered |= std::find(requests.begin(), requests.end(), key) != requests.end();
                            }
                            require(covered);
                        }
                    }
                }
            }
        }
    }
    return 0;
}
