#include "TextureUploadArenaDX12.h"

#include <cstdint>
#include <iostream>

namespace {
int failures = 0;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            std::cerr << __FILE__ << ':' << __LINE__                           \
                      << " CHECK failed: " #condition << '\n';                \
            ++failures;                                                        \
        }                                                                      \
    } while (false)
}

int main() {
    TextureUploadArenaLayoutDX12 layout;

    const auto first = layout.Allocate(1);
    const auto second = layout.Allocate(1);
    CHECK(first.valid && first.createdChunk);
    CHECK(first.offset == 0);
    CHECK(second.valid && !second.createdChunk);
    CHECK(second.chunkIndex == first.chunkIndex);
    CHECK(second.offset == D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    CHECK(second.offset >= first.offset + first.size);
    CHECK(layout.UsedBytes() == 2);
    CHECK(layout.ChunkCount() == 1);

    layout.Reset();
    const uint64_t halfChunk = TextureUploadArenaLayoutDX12::kChunkBytes / 2;
    const auto halfA = layout.Allocate(halfChunk);
    const auto halfB = layout.Allocate(halfChunk);
    const auto rollover = layout.Allocate(1);
    CHECK(halfA.chunkIndex == halfB.chunkIndex);
    CHECK(halfB.offset == halfChunk);
    CHECK(rollover.createdChunk);
    CHECK(rollover.chunkIndex == 1);
    CHECK(layout.ChunkCount() == 2);

    layout.Reset();
    const uint64_t oversizedBytes =
        TextureUploadArenaLayoutDX12::kChunkBytes + 1;
    const auto oversized = layout.Allocate(oversizedBytes);
    CHECK(oversized.createdChunk);
    CHECK(oversized.chunkCapacity >= oversizedBytes);
    CHECK((oversized.chunkCapacity %
           D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT) == 0);
    const auto afterOversized = layout.Allocate(512);
    CHECK(afterOversized.createdChunk);
    CHECK(afterOversized.chunkIndex == 1);
    CHECK(afterOversized.chunkCapacity ==
          TextureUploadArenaLayoutDX12::kChunkBytes);

    const uint64_t usedBeforeRollback = layout.UsedBytes();
    const uint64_t capacityBeforeRollback = layout.CapacityBytes();
    const auto rollback = layout.Allocate(1024);
    CHECK(!rollback.createdChunk);
    layout.Rollback(rollback);
    CHECK(layout.UsedBytes() == usedBeforeRollback);
    CHECK(layout.CapacityBytes() == capacityBeforeRollback);

    layout.RemoveLastChunk();
    CHECK(layout.ChunkCount() == 1);
    CHECK(layout.UsedBytes() == oversizedBytes);
    layout.RemoveLastChunk();
    CHECK(layout.ChunkCount() == 0);
    CHECK(layout.UsedBytes() == 0);
    CHECK(layout.CapacityBytes() == 0);

    layout.Reset();
    CHECK(layout.Allocate(0).valid == false);
    CHECK(layout.ChunkCount() == 0);

    if (failures == 0) std::cout << "Texture upload arena tests passed\n";
    return failures == 0 ? 0 : 1;
}
