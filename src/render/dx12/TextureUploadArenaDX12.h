#pragma once

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#include <d3d12.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

struct TextureUploadArenaStatsDX12 {
    uint64_t usedBytes = 0;
    uint64_t capacityBytes = 0;
    size_t chunkCount = 0;
    size_t releasedChunks = 0;
};

// Device-independent allocation model shared by the arena and its CPU tests.
// Texture footprints require 512-byte offsets; chunk resources themselves use
// the ordinary 64-KiB resource alignment when an oversized request is rounded.
class TextureUploadArenaLayoutDX12 {
public:
    static constexpr uint64_t kChunkBytes = 128ull * 1024ull * 1024ull;
    static constexpr uint64_t kOffsetAlignment =
        D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT;

    struct Allocation {
        size_t chunkIndex = 0;
        uint64_t offset = 0;
        uint64_t size = 0;
        uint64_t previousUsed = 0;
        uint64_t chunkCapacity = 0;
        bool createdChunk = false;
        bool valid = false;
    };

    Allocation Allocate(uint64_t bytes) {
        if (bytes == 0) return {};

        if (bytes <= kChunkBytes && !chunks_.empty() &&
            !chunks_.back().dedicated) {
            ChunkState& chunk = chunks_.back();
            const uint64_t offset = AlignUp(chunk.used, kOffsetAlignment);
            if (offset <= chunk.capacity && bytes <= chunk.capacity - offset) {
                Allocation result{ chunks_.size() - 1, offset, bytes,
                    chunk.used, chunk.capacity, false, true };
                chunk.used = offset + bytes;
                chunk.requested += bytes;
                usedBytes_ += bytes;
                return result;
            }
        }

        const bool dedicated = bytes > kChunkBytes;
        const uint64_t capacity = dedicated
            ? AlignUp(bytes, D3D12_DEFAULT_RESOURCE_PLACEMENT_ALIGNMENT)
            : kChunkBytes;
        chunks_.push_back({ capacity, bytes, bytes, dedicated });
        capacityBytes_ += capacity;
        usedBytes_ += bytes;
        return { chunks_.size() - 1, 0, bytes, 0, capacity, true, true };
    }

    void Rollback(const Allocation& allocation) {
        if (!allocation.valid || allocation.chunkIndex >= chunks_.size()) return;
        usedBytes_ -= allocation.size;
        if (allocation.createdChunk && allocation.chunkIndex + 1 == chunks_.size()) {
            capacityBytes_ -= chunks_.back().capacity;
            chunks_.pop_back();
        } else {
            chunks_[allocation.chunkIndex].used = allocation.previousUsed;
            chunks_[allocation.chunkIndex].requested -= allocation.size;
        }
    }

    void RemoveLastChunk() {
        if (chunks_.empty()) return;
        usedBytes_ -= chunks_.back().requested;
        capacityBytes_ -= chunks_.back().capacity;
        chunks_.pop_back();
    }

    void Reset() {
        chunks_.clear();
        usedBytes_ = 0;
        capacityBytes_ = 0;
    }

    uint64_t UsedBytes() const { return usedBytes_; }
    uint64_t CapacityBytes() const { return capacityBytes_; }
    size_t ChunkCount() const { return chunks_.size(); }

private:
    struct ChunkState {
        uint64_t capacity = 0;
        uint64_t used = 0;
        uint64_t requested = 0;
        bool dedicated = false;
    };

    static uint64_t AlignUp(uint64_t value, uint64_t alignment) {
        return (value + alignment - 1) & ~(alignment - 1);
    }

    std::vector<ChunkState> chunks_;
    uint64_t usedBytes_ = 0;
    uint64_t capacityBytes_ = 0;
};

struct TextureUploadAllocationDX12 {
    ID3D12Resource* resource = nullptr;
    uint8_t* cpuAddress = nullptr;
    uint64_t offset = 0;
    uint64_t size = 0;

    explicit operator bool() const {
        return resource != nullptr && cpuAddress != nullptr && size != 0;
    }
};

namespace TextureUploadArenaDetailDX12 {
struct Chunk {
    ComPtr<ID3D12Resource> resource;
    uint8_t* mapped = nullptr;
};

inline std::mutex mutex;
inline bool active = false;
inline bool releasing = false;
inline size_t releasedChunks = 0;
inline TextureUploadArenaLayoutDX12 layout;
inline std::vector<Chunk> chunks;
}

inline bool BeginTextureUploadArenaDX12() {
    std::lock_guard<std::mutex> lock(TextureUploadArenaDetailDX12::mutex);
    if (TextureUploadArenaDetailDX12::active ||
        TextureUploadArenaDetailDX12::releasing ||
        !TextureUploadArenaDetailDX12::chunks.empty()) return false;
    TextureUploadArenaDetailDX12::layout.Reset();
    TextureUploadArenaDetailDX12::releasedChunks = 0;
    TextureUploadArenaDetailDX12::active = true;
    return true;
}

inline bool IsTextureUploadArenaActiveDX12() {
    std::lock_guard<std::mutex> lock(TextureUploadArenaDetailDX12::mutex);
    return TextureUploadArenaDetailDX12::active;
}

inline TextureUploadAllocationDX12 AllocateTextureUploadDX12(
    ID3D12Device* device, uint64_t bytes) {
    if (!device || bytes == 0) return {};
    std::lock_guard<std::mutex> lock(TextureUploadArenaDetailDX12::mutex);
    if (!TextureUploadArenaDetailDX12::active ||
        TextureUploadArenaDetailDX12::releasing) return {};

    const auto allocation = TextureUploadArenaDetailDX12::layout.Allocate(bytes);
    if (!allocation.valid) return {};

    if (allocation.createdChunk) {
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC description = {};
        description.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        description.Width = allocation.chunkCapacity;
        description.Height = 1;
        description.DepthOrArraySize = 1;
        description.MipLevels = 1;
        description.SampleDesc.Count = 1;
        description.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        TextureUploadArenaDetailDX12::Chunk chunk;
        if (FAILED(device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &description,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&chunk.resource)))) {
            TextureUploadArenaDetailDX12::layout.Rollback(allocation);
            return {};
        }
        const D3D12_RANGE noRead = { 0, 0 };
        if (FAILED(chunk.resource->Map(0, &noRead,
                reinterpret_cast<void**>(&chunk.mapped)))) {
            chunk.resource.Reset();
            TextureUploadArenaDetailDX12::layout.Rollback(allocation);
            return {};
        }
#ifdef _DEBUG
        const std::wstring name = L"Level Texture Upload Arena #" +
            std::to_wstring(TextureUploadArenaDetailDX12::chunks.size());
        chunk.resource->SetName(name.c_str());
#endif
        TextureUploadArenaDetailDX12::chunks.push_back(std::move(chunk));
    }

    auto& chunk = TextureUploadArenaDetailDX12::chunks[allocation.chunkIndex];
    return { chunk.resource.Get(), chunk.mapped + allocation.offset,
             allocation.offset, allocation.size };
}

inline void BeginTextureUploadArenaReleaseDX12() {
    std::lock_guard<std::mutex> lock(TextureUploadArenaDetailDX12::mutex);
    TextureUploadArenaDetailDX12::active = false;
    TextureUploadArenaDetailDX12::releasing = true;
}

// Releases at most one backing resource. The caller invokes this once per
// rendered loading frame after the direct queue has been proven idle.
inline bool ReleaseOneTextureUploadArenaChunkDX12() {
    TextureUploadArenaDetailDX12::Chunk chunk;
    {
        std::lock_guard<std::mutex> lock(TextureUploadArenaDetailDX12::mutex);
        if (!TextureUploadArenaDetailDX12::releasing ||
            TextureUploadArenaDetailDX12::chunks.empty()) return false;
        chunk = std::move(TextureUploadArenaDetailDX12::chunks.back());
        TextureUploadArenaDetailDX12::chunks.pop_back();
        TextureUploadArenaDetailDX12::layout.RemoveLastChunk();
        ++TextureUploadArenaDetailDX12::releasedChunks;
    }
    if (chunk.resource && chunk.mapped) chunk.resource->Unmap(0, nullptr);
    chunk.resource.Reset();
    return true;
}

inline bool TextureUploadArenaReleaseCompleteDX12() {
    std::lock_guard<std::mutex> lock(TextureUploadArenaDetailDX12::mutex);
    return TextureUploadArenaDetailDX12::releasing &&
           TextureUploadArenaDetailDX12::chunks.empty();
}

inline void EndTextureUploadArenaReleaseDX12() {
    std::lock_guard<std::mutex> lock(TextureUploadArenaDetailDX12::mutex);
    if (!TextureUploadArenaDetailDX12::chunks.empty()) return;
    TextureUploadArenaDetailDX12::active = false;
    TextureUploadArenaDetailDX12::releasing = false;
    TextureUploadArenaDetailDX12::layout.Reset();
}

inline TextureUploadArenaStatsDX12 GetTextureUploadArenaStatsDX12() {
    std::lock_guard<std::mutex> lock(TextureUploadArenaDetailDX12::mutex);
    return { TextureUploadArenaDetailDX12::layout.UsedBytes(),
             TextureUploadArenaDetailDX12::layout.CapacityBytes(),
             TextureUploadArenaDetailDX12::chunks.size(),
             TextureUploadArenaDetailDX12::releasedChunks };
}
