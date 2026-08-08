#pragma once

// Device-side owner of the bindless descriptor heap. Wraps the pure-CPU index
// allocator in BindlessHeapDX12.h with the actual ID3D12DescriptorHeap, the
// fallback textures, and the SM 6.6 / Resource Binding Tier 3 capability gate.
//
// The heap is separate from g_dx12.cbvSrvUavHeap on purpose. Sharing one heap
// would mean enabling bindless changes descriptor addresses the legacy path
// depends on; a separate heap makes "bindless off" byte-identical to the
// renderer as it shipped, which is the whole point of keeping this opt-in.

#include "BindlessHeapDX12.h"
#include "DX12Core.h"

#include <iostream>
#include <string>
#include <vector>

class BindlessHeapDX12 {
public:
    // ResourceDescriptorHeap[] indexing requires *both* SM 6.6 (the shader
    // language feature) and Resource Binding Tier 3 (unbounded descriptor
    // ranges in hardware). Either one missing means bindless cannot be used at
    // all, so report which one so the UI can say something actionable rather
    // than just "unsupported".
    struct Capability {
        bool shaderModel66 = false;
        bool bindingTier3 = false;
        bool Supported() const { return shaderModel66 && bindingTier3; }
        std::string Reason() const {
            if (Supported()) return "supported";
            if (!shaderModel66 && !bindingTier3)
                return "requires Shader Model 6.6 and Resource Binding Tier 3";
            if (!shaderModel66) return "requires Shader Model 6.6";
            return "requires Resource Binding Tier 3";
        }
    };

    static Capability QueryCapability(ID3D12Device* device) {
        Capability capability;
        if (!device) return capability;

        // CheckFeatureSupport for SHADER_MODEL takes the highest model the
        // caller understands and writes back what the driver actually
        // supports, so it must be seeded rather than zeroed.
        D3D12_FEATURE_DATA_SHADER_MODEL shaderModel = {};
        shaderModel.HighestShaderModel = D3D_SHADER_MODEL_6_6;
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL,
                &shaderModel, sizeof(shaderModel)))) {
            capability.shaderModel66 =
                shaderModel.HighestShaderModel >= D3D_SHADER_MODEL_6_6;
        }

        D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS,
                &options, sizeof(options)))) {
            capability.bindingTier3 =
                options.ResourceBindingTier >= D3D12_RESOURCE_BINDING_TIER_3;
        }
        return capability;
    }

    // Creates the heap and the fallback textures. Returns false (leaving the
    // manager unsupported, not fatal) when the adapter cannot do bindless or
    // heap creation fails -- callers keep using the legacy path.
    bool Init() {
        capability = QueryCapability(g_dx12.device.Get());
        if (!capability.Supported()) {
            std::cout << "Bindless materials unavailable: " << capability.Reason()
                      << "\n";
            return false;
        }

        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.NumDescriptors = BINDLESS_HEAP_SIZE;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(g_dx12.device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&heap)))) {
            std::cerr << "Bindless heap creation failed\n";
            return false;
        }
        heap->SetName(L"BindlessHeap");
        descriptorSize = g_dx12.cbvSrvUavDescriptorSize;

        if (!CreateFallbackTextures()) {
            heap.Reset();
            return false;
        }

        initialized = true;
        return true;
    }

    bool Initialized() const { return initialized; }
    bool Supported() const { return capability.Supported(); }
    const Capability& Caps() const { return capability; }
    ID3D12DescriptorHeap* Heap() const { return heap.Get(); }

    D3D12_CPU_DESCRIPTOR_HANDLE CpuHandleAt(uint32_t index) const {
        D3D12_CPU_DESCRIPTOR_HANDLE handle =
            heap->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += (SIZE_T)index * descriptorSize;
        return handle;
    }

    D3D12_GPU_DESCRIPTOR_HANDLE GpuHandleAt(uint32_t index) const {
        D3D12_GPU_DESCRIPTOR_HANDLE handle =
            heap->GetGPUDescriptorHandleForHeapStart();
        handle.ptr += (UINT64)index * descriptorSize;
        return handle;
    }

    // Registers a material texture and returns its absolute heap index. The
    // SRV is created only on first registration; afterwards this is a hash
    // lookup. `fallback` is returned for a null texture or on overflow.
    //
    // Textures are transitioned once to the combined pixel/non-pixel SRV state
    // because the visibility resolve samples them from a compute shader while
    // the forward path samples them from a pixel shader.
    uint32_t RegisterTexture(ID3D12Resource* texture, uint32_t fallback) {
        if (!initialized) return fallback;
        if (!texture) return fallback;

        bool created = false;
        const uint32_t index =
            allocator.RegisterPersistent(texture, fallback, &created);
        if (!created) return index;

        const D3D12_RESOURCE_DESC resourceDesc = texture->GetDesc();
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Format = SrvFormatFor(resourceDesc.Format);
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = resourceDesc.MipLevels;
        g_dx12.device->CreateShaderResourceView(texture, &srv, CpuHandleAt(index));

        pendingTransitions.push_back(texture);
        return index;
    }

    // Emits the state transitions accumulated by RegisterTexture. Called once
    // per frame before the passes that sample material textures, so a frame
    // that registers many materials still costs one barrier batch.
    void FlushTextureTransitions(ID3D12GraphicsCommandList* commandList) {
        if (pendingTransitions.empty() || !commandList) return;
        std::vector<D3D12_RESOURCE_BARRIER> barriers;
        barriers.reserve(pendingTransitions.size());
        for (ID3D12Resource* texture : pendingTransitions) {
            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = texture;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            barrier.Transition.StateAfter =
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barriers.push_back(barrier);
        }
        commandList->ResourceBarrier((UINT)barriers.size(), barriers.data());
        pendingTransitions.clear();
    }

    void BeginFrame(uint32_t frameIndex) {
        if (!initialized) return;
        allocator.BeginTransientFrame(frameIndex);
    }

    // Copies `count` descriptors from a CPU-side staging heap into a freshly
    // allocated contiguous transient range, returning the base index. Used for
    // the forward globals and the visibility resolve tables, which change every
    // frame. Returns BINDLESS_INVALID_INDEX on overflow.
    uint32_t AllocateTransientTable(const D3D12_CPU_DESCRIPTOR_HANDLE* sources,
                                    uint32_t count) {
        if (!initialized || !sources || count == 0) return BINDLESS_INVALID_INDEX;
        const uint32_t base = allocator.AllocateTransient(count);
        if (base == BINDLESS_INVALID_INDEX) return BINDLESS_INVALID_INDEX;
        for (uint32_t i = 0; i < count; ++i) {
            if (sources[i].ptr == 0) continue;
            g_dx12.device->CopyDescriptorsSimple(1, CpuHandleAt(base + i),
                sources[i], D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }
        return base;
    }

    // Scene teardown. The caller MUST have waited for GPU idle first: these
    // descriptors may still be referenced by in-flight command lists, and the
    // generation bump silently re-points every surviving material at new slots.
    void ResetForNewScene() {
        if (!initialized) return;
        allocator.ResetPersistent();
        pendingTransitions.clear();
    }

    BindlessDescriptorAllocator& Allocator() { return allocator; }
    const BindlessDescriptorAllocator& Allocator() const { return allocator; }

    uint32_t FallbackFor(uint32_t slot) const { return slot; }

private:
    // Typeless depth/BC formats cannot be viewed directly. Material textures are
    // ordinary colour maps, so the only remapping needed is TYPELESS -> UNORM
    // for the RGBA8 case the importers can produce.
    static DXGI_FORMAT SrvFormatFor(DXGI_FORMAT format) {
        if (format == DXGI_FORMAT_R8G8B8A8_TYPELESS)
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        return format;
    }

    // 1x1 textures bound whenever a material lacks a map or the allocator
    // overflows. Sampling these yields the identity for their channel, so an
    // overflowing scene degrades to untextured rather than to garbage.
    bool CreateFallbackTextures() {
        struct FallbackSpec { uint32_t index; uint8_t rgba[4]; const wchar_t* name; };
        const FallbackSpec specs[] = {
            { BINDLESS_FALLBACK_WHITE,      { 255, 255, 255, 255 }, L"BindlessWhite" },
            // Flat tangent-space normal: (0,0,1) encoded to [0,1].
            { BINDLESS_FALLBACK_NORMAL,     { 128, 128, 255, 255 }, L"BindlessFlatNormal" },
            // glTF metal/rough packs roughness in G and metalness in B.
            // Neutral = fully rough, non-metal.
            { BINDLESS_FALLBACK_METALROUGH, { 0, 255, 0, 255 },     L"BindlessMetalRough" },
            { BINDLESS_FALLBACK_BLACK,      { 0, 0, 0, 255 },       L"BindlessBlack" },
        };

        for (const FallbackSpec& spec : specs) {
            ComPtr<ID3D12Resource> texture;
            D3D12_HEAP_PROPERTIES heapProps = {};
            heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC desc = {};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = 1;
            desc.Height = 1;
            desc.DepthOrArraySize = 1;
            desc.MipLevels = 1;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            if (FAILED(g_dx12.device->CreateCommittedResource(&heapProps,
                    D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                    nullptr, IID_PPV_ARGS(&texture)))) {
                std::cerr << "Bindless fallback texture creation failed\n";
                return false;
            }
            texture->SetName(spec.name);

            // Upload the single texel. The upload heap is retained for the
            // lifetime of the manager because the copy is recorded on the
            // caller's command list and executes later.
            ComPtr<ID3D12Resource> upload;
            D3D12_HEAP_PROPERTIES uploadProps = {};
            uploadProps.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC uploadDesc = {};
            uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            uploadDesc.Width = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
            uploadDesc.Height = 1;
            uploadDesc.DepthOrArraySize = 1;
            uploadDesc.MipLevels = 1;
            uploadDesc.Format = DXGI_FORMAT_UNKNOWN;
            uploadDesc.SampleDesc.Count = 1;
            uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(g_dx12.device->CreateCommittedResource(&uploadProps,
                    D3D12_HEAP_FLAG_NONE, &uploadDesc,
                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                    IID_PPV_ARGS(&upload)))) {
                return false;
            }
            void* mapped = nullptr;
            D3D12_RANGE readRange = { 0, 0 };
            if (FAILED(upload->Map(0, &readRange, &mapped)) || !mapped) return false;
            memcpy(mapped, spec.rgba, sizeof(spec.rgba));
            upload->Unmap(0, nullptr);

            D3D12_TEXTURE_COPY_LOCATION dst = {};
            dst.pResource = texture.Get();
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = 0;
            D3D12_TEXTURE_COPY_LOCATION src = {};
            src.pResource = upload.Get();
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            src.PlacedFootprint.Footprint.Width = 1;
            src.PlacedFootprint.Footprint.Height = 1;
            src.PlacedFootprint.Footprint.Depth = 1;
            src.PlacedFootprint.Footprint.RowPitch =
                D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
            g_dx12.commandList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = texture.Get();
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            barrier.Transition.StateAfter =
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            g_dx12.commandList->ResourceBarrier(1, &barrier);

            D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
            srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srv.Texture2D.MipLevels = 1;
            g_dx12.device->CreateShaderResourceView(texture.Get(), &srv,
                CpuHandleAt(spec.index));

            fallbackTextures.push_back(texture);
            fallbackUploads.push_back(upload);
        }
        return true;
    }

    BindlessDescriptorAllocator allocator;
    Capability capability;
    ComPtr<ID3D12DescriptorHeap> heap;
    UINT descriptorSize = 0;
    bool initialized = false;
    std::vector<ComPtr<ID3D12Resource>> fallbackTextures;
    std::vector<ComPtr<ID3D12Resource>> fallbackUploads;
    std::vector<ID3D12Resource*> pendingTransitions;
};
