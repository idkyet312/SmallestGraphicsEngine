// Exercise the actual DX12 pass on WARP: analytic heightfield intersections,
// identity cases, foreground occlusion, silhouette growth and frame-slot reuse.
#include "TerrainScreenDisplacementDX12.h"
#include <d3d12sdklayers.h>
#include <cmath>
#include <cstring>
#include <stdexcept>

ProfilerDX12 g_profiler;
static constexpr UINT Width = 64, Height = 64;
static void Require(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
static void Check(HRESULT hr) { Require(SUCCEEDED(hr), "DX12 operation failed"); }
static void Barrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource* resource,
                    D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = { resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to };
    cmd->ResourceBarrier(1, &barrier);
}

int main() try {
    ComPtr<ID3D12Debug> debug;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
    ComPtr<IDXGIFactory4> factory;
    Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter> warp;
    Check(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)));
    Check(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&g_dx12.device)));
    g_dx12.cbvSrvUavDescriptorSize = g_dx12.device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    ComPtr<ID3D12InfoQueue> info;
    g_dx12.device.As(&info);
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    Check(g_dx12.device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12CommandAllocator> allocator;
    Check(g_dx12.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)));
    ComPtr<ID3D12GraphicsCommandList> cmd;
    Check(g_dx12.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        allocator.Get(), nullptr, IID_PPV_ARGS(&cmd)));
    ComPtr<ID3D12Fence> fence;
    Check(g_dx12.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
    UINT64 fenceValue = 0;
    HANDLE event = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    Require(event != nullptr, "fence event");
    auto execute = [&] {
        Check(cmd->Close());
        ID3D12CommandList* lists[] = { cmd.Get() };
        queue->ExecuteCommandLists(1, lists);
        Check(queue->Signal(fence.Get(), ++fenceValue));
        Check(fence->SetEventOnCompletion(fenceValue, event));
        Require(WaitForSingleObject(event, 30000) == WAIT_OBJECT_0, "GPU test timeout");
    };
    auto buffer = [&](UINT64 size, D3D12_HEAP_TYPE type) {
        ComPtr<ID3D12Resource> resource;
        D3D12_HEAP_PROPERTIES heap = {}; heap.Type = type;
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = size;
        desc.Height = 1; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
        desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        Check(g_dx12.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(&resource)));
        return resource;
    };
    std::vector<ComPtr<ID3D12Resource>> uploads;
    auto texture = [&](UINT width, UINT height, UINT layers, DXGI_FORMAT format,
                       D3D12_RESOURCE_FLAGS flags, const void* data, UINT bytesPerPixel,
                       D3D12_RESOURCE_STATES state) {
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width = width;
        desc.Height = height; desc.DepthOrArraySize = (UINT16)layers; desc.MipLevels = 1;
        desc.Format = format; desc.SampleDesc.Count = 1; desc.Flags = flags;
        D3D12_HEAP_PROPERTIES heap = {}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        ComPtr<ID3D12Resource> result;
        Check(g_dx12.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&result)));
        std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> layouts(layers);
        UINT64 size;
        g_dx12.device->GetCopyableFootprints(&desc, 0, layers, 0, layouts.data(), nullptr, nullptr, &size);
        auto upload = buffer(size, D3D12_HEAP_TYPE_UPLOAD);
        unsigned char* mapped;
        Check(upload->Map(0, nullptr, reinterpret_cast<void**>(&mapped)));
        for (UINT layer = 0; layer < layers; ++layer) {
            for (UINT y = 0; y < height; ++y)
                std::memcpy(mapped + layouts[layer].Offset + y * layouts[layer].Footprint.RowPitch,
                    (const unsigned char*)data + (layer * height + y) * width * bytesPerPixel,
                    width * bytesPerPixel);
            D3D12_TEXTURE_COPY_LOCATION source = {};
            source.pResource = upload.Get(); source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            source.PlacedFootprint = layouts[layer];
            D3D12_TEXTURE_COPY_LOCATION target = {};
            target.pResource = result.Get(); target.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            target.SubresourceIndex = layer;
            cmd->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
        }
        upload->Unmap(0, nullptr); uploads.push_back(upload);
        Barrier(cmd.Get(), result.Get(), D3D12_RESOURCE_STATE_COPY_DEST, state);
        return result;
    };
    TerrainScreenDisplacementDX12 pass;
    const XMVECTOR eye = XMVectorSet(0, 3, -5, 1);
    const XMMATRIX view = XMMatrixLookAtLH(eye, XMVectorZero(), XMVectorSet(0, 1, 0, 0));
    const XMMATRIX projection = XMMatrixPerspectiveFovLH(XM_PI / 3.0f, 1, 0.1f, 100.0f);
    const XMMATRIX vp = view * projection;
    const XMMATRIX inverse = XMMatrixInverse(nullptr, vp);
    TerrainScreenDisplacementDX12::Constants constants{};
    XMStoreFloat4x4(&constants.viewProjection, XMMatrixTranspose(vp));
    XMStoreFloat4x4(&constants.inverseViewProjection, XMMatrixTranspose(inverse));
    XMStoreFloat3(&constants.cameraPosition, eye);
    constants.screenSize = XMFLOAT2((float)Width, (float)Height);
    constants.splatEnabled = 1; constants.splatInvExtent = XMFLOAT2(0.001f, 0.001f);
    constants.fadeStart = 90; constants.fadeEnd = 100;
    constants.edgeFadePixels = 4;
    auto rayAt = [&](UINT x, UINT y) {
        XMVECTOR clip = XMVectorSet((x + 0.5f) / Width * 2 - 1,
            1 - (y + 0.5f) / Height * 2, 0.5f, 1);
        XMVECTOR world = XMVector4Transform(clip, inverse);
        world = XMVectorScale(world, 1 / XMVectorGetW(world));
        return XMVector3Normalize(XMVectorSubtract(world, eye));
    };
    auto depthOf = [&](XMVECTOR world) {
        XMVECTOR clip = XMVector4Transform(XMVectorSetW(world, 1), vp);
        return XMVectorGetZ(clip) / XMVectorGetW(clip);
    };
    std::vector<UINT> inputVisibility(Width * Height * 2, 0);
    std::vector<float> inputDepth(Width * Height, 1);
    for (UINT y = 0; y < Height; ++y) for (UINT x = 0; x < Width; ++x) {
        const UINT i = y * Width + x;
        XMVECTOR ray = rayAt(x, y);
        float travel = -3.0f / XMVectorGetY(ray);
        XMVECTOR world = XMVectorMultiplyAdd(ray, XMVectorReplicate(travel), eye);
        if (travel > 0 && XMVectorGetZ(world) > -2 && XMVectorGetZ(world) < 4) {
            inputVisibility[i * 2] = 0xFFFFFFFFu;
            inputVisibility[i * 2 + 1] = 0xFFFF8000u;
            inputDepth[i] = depthOf(world);
        }
        if (x >= 27 && x <= 36 && y >= 27 && y <= 36) {
            inputVisibility[i * 2] = 1; inputVisibility[i * 2 + 1] = 7;
            inputDepth[i] = depthOf(XMVectorMultiplyAdd(ray, XMVectorReplicate(1), eye));
        }
    }
    D3D12_DESCRIPTOR_HEAP_DESC desc = {};
    desc.NumDescriptors = 1; desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    ComPtr<ID3D12DescriptorHeap> rtv;
    Check(g_dx12.device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&rtv)));
    desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    ComPtr<ID3D12DescriptorHeap> dsv;
    Check(g_dx12.device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&dsv)));
    for (UINT test = 0; test < 5; ++test) {
        if (test) { Check(allocator->Reset()); Check(cmd->Reset(allocator.Get(), nullptr)); uploads.clear(); }
        g_dx12.frameIndex = test % FRAME_COUNT;
        const UINT alpha = test == 0 ? 192 : test == 1 ? 128 : test == 2 ? 64 : 255;
        constants.strength = test == 3 ? 0.0f : test == 4 ? 4.0f : 1.0f;
        UINT layers[4] = { 0x00FFFFFFu | (alpha << 24), 0x00FFFFFFu | (alpha << 24),
                           0x00FFFFFFu | (alpha << 24), 0x00FFFFFFu | (alpha << 24) };
        auto packed = texture(1, 1, 4, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE,
            layers, 4, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        UINT paint = 0xFF000000u;
        auto splat = texture(1, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_RESOURCE_FLAG_NONE,
            &paint, 4, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        auto visibility = texture(Width, Height, 1, DXGI_FORMAT_R32G32_UINT,
            D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, inputVisibility.data(), 8,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        auto depth = texture(Width, Height, 1, DXGI_FORMAT_R32_TYPELESS,
            D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, inputDepth.data(), 4, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        g_dx12.device->CreateRenderTargetView(visibility.Get(), nullptr, rtv->GetCPUDescriptorHandleForHeapStart());
        D3D12_DEPTH_STENCIL_VIEW_DESC depthView = {};
        depthView.Format = DXGI_FORMAT_D32_FLOAT; depthView.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        g_dx12.device->CreateDepthStencilView(depth.Get(), &depthView, dsv->GetCPUDescriptorHandleForHeapStart());
        Require(pass.Prepare(visibility.Get(), depth.Get()), "displacement pass preparation");
        pass.Render(cmd.Get(), visibility.Get(), depth.Get(), rtv->GetCPUDescriptorHandleForHeapStart(),
            dsv->GetCPUDescriptorHandleForHeapStart(), packed.Get(), packed.Get(), packed.Get(), splat.Get(), constants);
        Barrier(cmd.Get(), depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(cmd.Get(), visibility.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        auto readTexture = [&](ID3D12Resource* resource, D3D12_PLACED_SUBRESOURCE_FOOTPRINT& layout) {
            auto resourceDesc = resource->GetDesc(); UINT64 size;
            g_dx12.device->GetCopyableFootprints(&resourceDesc, 0, 1, 0, &layout, nullptr, nullptr, &size);
            auto readback = buffer(size, D3D12_HEAP_TYPE_READBACK);
            D3D12_TEXTURE_COPY_LOCATION target = {};
            target.pResource = readback.Get(); target.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            target.PlacedFootprint = layout;
            D3D12_TEXTURE_COPY_LOCATION source = {};
            source.pResource = resource; source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            cmd->CopyTextureRegion(&target, 0, 0, 0, &source, nullptr);
            return readback;
        };
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT depthLayout, visLayout;
        auto depthRead = readTexture(depth.Get(), depthLayout);
        auto visRead = readTexture(visibility.Get(), visLayout);
        execute();
        unsigned char *depthBytes, *visBytes;
        Check(depthRead->Map(0, nullptr, reinterpret_cast<void**>(&depthBytes)));
        Check(visRead->Map(0, nullptr, reinterpret_cast<void**>(&visBytes)));
        UINT changed = 0, expanded = 0, analytic = 0;
        for (UINT y = 0; y < Height; ++y) for (UINT x = 0; x < Width; ++x) {
            UINT i = y * Width + x;
            float actualDepth = ((float*)(depthBytes + y * depthLayout.Footprint.RowPitch))[x];
            UINT actualID = ((UINT*)(visBytes + y * visLayout.Footprint.RowPitch))[x * 2];
            Require(std::isfinite(actualDepth) && actualDepth >= 0 && actualDepth <= 1, "invalid displaced depth");
            if (inputVisibility[i * 2] == 1)
                Require(actualID == 1 && actualDepth == inputDepth[i], "foreground mesh was overwritten");
            if (test == 3) Require(actualDepth == inputDepth[i] && actualID == inputVisibility[i * 2], "zero strength changed frame");
            if (test == 1) Require(std::abs(actualDepth - inputDepth[i]) < 2e-5f, "neutral height changed depth");
            if (std::abs(actualDepth - inputDepth[i]) > 1e-6f) ++changed;
            if (actualID == 0xFFFFFFFFu && inputVisibility[i * 2] == 0) ++expanded;
            if ((test == 0 || test == 2) && inputVisibility[i * 2] == 0xFFFFFFFFu &&
                x > 10 && x < 53 && y > 38 && y < 45) {
                XMVECTOR ray = rayAt(x, y);
                float facing = -XMVectorGetY(ray);
                if (facing > 0.25f) {
                    float relief = 0.080f * (int(alpha) - 128) / 255.0f;
                    float t = (relief - 3.0f) / XMVectorGetY(ray);
                    float expected = depthOf(XMVectorMultiplyAdd(ray, XMVectorReplicate(t), eye));
                    if (std::abs(actualDepth - expected) >= 2e-5f)
                        std::cerr << "case=" << test << " pixel=" << x << ',' << y
                            << " base=" << inputDepth[i] << " actual=" << actualDepth
                            << " expected=" << expected << " relief=" << relief << '\n';
                    Require(std::abs(actualDepth - expected) < 2e-5f, "heightfield intersection mismatch");
                    ++analytic;
                }
            }
        }
        depthRead->Unmap(0, nullptr); visRead->Unmap(0, nullptr);
        if (test == 0 || test == 2) Require(changed > 100 && analytic > 20, "raymarch did not displace terrain");
        std::cout << "case " << test << ": changed=" << changed << " expanded=" << expanded << " analytic=" << analytic << '\n';
        if (test == 4) Require(expanded > 0, "silhouette did not expand");
    }
    if (info) for (UINT64 i = 0; i < info->GetNumStoredMessagesAllowedByRetrievalFilter(); ++i) {
        SIZE_T size = 0; info->GetMessage(i, nullptr, &size);
        std::vector<unsigned char> storage(size);
        auto* message = reinterpret_cast<D3D12_MESSAGE*>(storage.data());
        Check(info->GetMessage(i, message, &size));
        if (message->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) {
            std::cerr << message->pDescription << '\n';
            throw std::runtime_error("DX12 validation error");
        }
    }
    CloseHandle(event);
    std::cout << "Terrain screen displacement GPU tests passed\n";
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n'; return 1;
}
