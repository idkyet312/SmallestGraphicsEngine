#pragma once
#include <cstdint>

// Keep the primary view at its original addresses; the scope owns the upper
// frame slices. The caller reuses a frame only after its existing fence retires.
enum class RenderViewDX12 : uint32_t { Main = 0, Scope = 1 };
inline constexpr uint32_t RenderViewCountDX12 = 2;
inline constexpr uint32_t RenderViewFrameIndexDX12(
    uint32_t frame, uint32_t frameCount, RenderViewDX12 view) {
    return frame + static_cast<uint32_t>(view) * frameCount;
}
