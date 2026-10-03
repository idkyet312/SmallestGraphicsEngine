#pragma once

// Private application implementation; included once by main.cpp in dependency order.

struct WeaponPreviewPipelineDX12 {
    ComPtr<ID3D12RootSignature> rootSignature;
    ComPtr<ID3D12PipelineState> opaque, transparent;
    bool failed = false;

    bool Initialize() {
        if (opaque && transparent) return true;
        if (failed) return false;
        failed = true;
        const char* path = "shaders/weapon_preview.hlsl";
        std::ifstream stream(path, std::ios::binary);
        const std::string source((std::istreambuf_iterator<char>(stream)),
                                  std::istreambuf_iterator<char>());
        ComPtr<ID3DBlob> vs, ps, errors;
        const auto compile = [&](const char* entry, const char* target,
                                 ComPtr<ID3DBlob>& result) {
            const HRESULT hr = ShaderCacheDX12::CompileCached(source.data(), source.size(),
                path, nullptr, nullptr, entry, target, D3DCOMPILE_OPTIMIZATION_LEVEL3,
                0, &result, &errors);
            if (FAILED(hr))
                SGE_LOG("LogPrefab", EngineLog::Level::Error,
                    errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()),
                                         errors->GetBufferSize()) : "Weapon preview shader unavailable");
            return SUCCEEDED(hr);
        };
        if (!compile("PreviewVS", "vs_5_1", vs) || !compile("PreviewPS", "ps_5_1", ps)) return false;
        D3D12_DESCRIPTOR_RANGE range{};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 4;
        D3D12_ROOT_PARAMETER parameters[3]{};
        parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[1].DescriptorTable.NumDescriptorRanges = 1;
        parameters[1].DescriptorTable.pDescriptorRanges = &range;
        parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        parameters[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[2].Constants.ShaderRegister = 1;
        parameters[2].Constants.Num32BitValues = 16;
        parameters[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        sampler.MaxLOD = FLT_MAX;
        sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC root{};
        root.NumParameters = 3;
        root.pParameters = parameters;
        root.NumStaticSamplers = 1;
        root.pStaticSamplers = &sampler;
        root.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> serialized;
        if (FAILED(D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1,
                &serialized, &errors)) || FAILED(g_dx12.device->CreateRootSignature(
                0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                IID_PPV_ARGS(&rootSignature)))) return false;
        const D3D12_INPUT_ELEMENT_DESC input[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TANGENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
        };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = rootSignature.Get();
        desc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
        desc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        desc.InputLayout = { input, UINT(std::size(input)) };
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.RasterizerState.DepthClipEnable = TRUE;
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        desc.DepthStencilState.DepthEnable = TRUE;
        desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        desc.SampleMask = UINT_MAX;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        desc.SampleDesc.Count = 1;
        if (FAILED(g_dx12.device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&opaque)))) return false;
        auto& blend = desc.BlendState.RenderTarget[0];
        blend.BlendEnable = TRUE;
        blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        blend.SrcBlendAlpha = D3D12_BLEND_ONE;
        blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        if (FAILED(g_dx12.device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&transparent)))) return false;
        failed = false;
        return true;
    }
};
static WeaponPreviewPipelineDX12 g_weaponPreviewPipeline;

struct WeaponPreviewMesh {
    struct Part {
        const MeshPrimitive* primitive = nullptr;
        UINT descriptorSlot = ~0u;
        XMFLOAT3 center{};
    };
    std::shared_ptr<SceneMesh> source;
    PrefabThumbnailMesh bounds;
    std::vector<Part> parts;
    bool failed = false;

    bool Prepare(const std::shared_ptr<SceneMesh>& loaded) {
        if (!parts.empty()) return true;
        if (failed || !loaded) return false;
        source = loaded;
        size_t partCount = 0;
        for (const auto& primitive : source->primitives)
            if (primitive.vertexBuffer && primitive.indexBuffer && primitive.indexCount) ++partCount;
        if (!partCount || partCount * 4 > kImGuiDescriptorCount - g_nextImGuiTextureSlot) {
            failed = true;
            return false;
        }
        std::fill(std::begin(bounds.minimum), std::end(bounds.minimum), FLT_MAX);
        std::fill(std::begin(bounds.maximum), std::end(bounds.maximum), -FLT_MAX);
        const UINT stride = g_dx12.device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        for (const auto& primitive : source->primitives) {
            if (!primitive.vertexBuffer || !primitive.indexBuffer || !primitive.indexCount) continue;
            Part part;
            part.primitive = &primitive;
            float lo[3] = { FLT_MAX, FLT_MAX, FLT_MAX }, hi[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };
            for (size_t v = 0; v + 11 < primitive.vertices.size(); v += 12)
                for (int axis = 0; axis < 3; ++axis) {
                    lo[axis] = (std::min)(lo[axis], primitive.vertices[v + axis]);
                    hi[axis] = (std::max)(hi[axis], primitive.vertices[v + axis]);
                    bounds.minimum[axis] = (std::min)(bounds.minimum[axis], lo[axis]);
                    bounds.maximum[axis] = (std::max)(bounds.maximum[axis], hi[axis]);
                }
            part.center = XMFLOAT3((lo[0] + hi[0]) * 0.5f,
                (lo[1] + hi[1]) * 0.5f, (lo[2] + hi[2]) * 0.5f);
            const auto& material = primitive.material;
            ID3D12Resource* textures[] = {
                material ? material->baseColorTexture.Get() : nullptr,
                material ? material->normalTexture.Get() : nullptr,
                material ? material->metallicRoughnessTexture.Get() : nullptr,
                material ? material->emissiveTexture.Get() : nullptr
            };
            // Borrow the game's immutable textures. Fresh permanent descriptors
            // leave the gameplay material's own cached bindings intact.
            part.descriptorSlot = g_nextImGuiTextureSlot;
            for (auto* texture : textures) {
                auto cpu = imguiSrvHeap->GetCPUDescriptorHandleForHeapStart();
                cpu.ptr += static_cast<SIZE_T>(g_nextImGuiTextureSlot++) * stride;
                if (texture) g_dx12.device->CreateShaderResourceView(texture, nullptr, cpu);
                else {
                    D3D12_SHADER_RESOURCE_VIEW_DESC nullView{};
                    nullView.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                    nullView.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                    nullView.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                    nullView.Texture2D.MipLevels = 1;
                    g_dx12.device->CreateShaderResourceView(nullptr, &nullView, cpu);
                }
            }
            parts.push_back(part);
        }
        return true;
    }
};

struct WeaponPreviewDX12 {
    UINT Width = 1280, Height = 768;
    struct Frame {
        ComPtr<ID3D12Resource> color, depth, constants;
        UINT descriptorSlot = ~0u;
    };
    Frame frames[FRAME_COUNT];
    ComPtr<ID3D12DescriptorHeap> rtvHeap, dsvHeap;
    bool initialized = false, failed = false;

    bool Initialize() {
        if (initialized) return true;
        if (failed || !imguiSrvHeap || !g_dx12.device) return false;
        failed = true;
        if (g_nextImGuiTextureSlot + FRAME_COUNT > kImGuiDescriptorCount ||
            !g_weaponPreviewPipeline.Initialize()) return false;
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        heapDesc.NumDescriptors = FRAME_COUNT;
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        if (FAILED(g_dx12.device->CreateDescriptorHeap(
                &heapDesc, IID_PPV_ARGS(&rtvHeap)))) return false;
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        if (FAILED(g_dx12.device->CreateDescriptorHeap(
                &heapDesc, IID_PPV_ARGS(&dsvHeap)))) return false;
        const UINT rtvStride = g_dx12.device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        const UINT dsvStride = g_dx12.device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        const UINT srvStride = g_dx12.device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        for (UINT i = 0; i < FRAME_COUNT; ++i) {
            Frame& frame = frames[i];
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC desc{};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            desc.Width = Width;
            desc.Height = Height;
            desc.DepthOrArraySize = desc.MipLevels = 1;
            desc.SampleDesc.Count = 1;
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            D3D12_CLEAR_VALUE clear{};
            clear.Format = desc.Format;
            clear.Color[0] = 0.13f; clear.Color[1] = 0.15f;
            clear.Color[2] = 0.18f; clear.Color[3] = 1.0f;
            if (FAILED(g_dx12.device->CreateCommittedResource(&heap,
                    D3D12_HEAP_FLAG_NONE, &desc,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                    &clear, IID_PPV_ARGS(&frame.color)))) return false;
            frame.color->SetName(L"Weapon Preview Color");
            auto rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
            rtv.ptr += static_cast<SIZE_T>(i) * rtvStride;
            g_dx12.device->CreateRenderTargetView(frame.color.Get(), nullptr, rtv);
            desc.Format = DXGI_FORMAT_D32_FLOAT;
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
            clear = {};
            clear.Format = desc.Format;
            clear.DepthStencil.Depth = 1.0f;
            if (FAILED(g_dx12.device->CreateCommittedResource(&heap,
                    D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                    &clear, IID_PPV_ARGS(&frame.depth)))) return false;
            frame.depth->SetName(L"Weapon Preview Depth");
            auto dsv = dsvHeap->GetCPUDescriptorHandleForHeapStart();
            dsv.ptr += static_cast<SIZE_T>(i) * dsvStride;
            g_dx12.device->CreateDepthStencilView(frame.depth.Get(), nullptr, dsv);
            heap.Type = D3D12_HEAP_TYPE_UPLOAD;
            desc = {};
            desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            desc.Width = 256;
            desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
            desc.SampleDesc.Count = 1;
            desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(g_dx12.device->CreateCommittedResource(&heap,
                    D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                    nullptr, IID_PPV_ARGS(&frame.constants)))) return false;
            frame.constants->SetName(L"Weapon Preview Constants");
        }
        // Slots are reserved once and never rewritten while ImGui can reference them.
        for (Frame& frame : frames) {
            frame.descriptorSlot = g_nextImGuiTextureSlot++;
            auto srv = imguiSrvHeap->GetCPUDescriptorHandleForHeapStart();
            srv.ptr += static_cast<SIZE_T>(frame.descriptorSlot) * srvStride;
            D3D12_SHADER_RESOURCE_VIEW_DESC desc{};
            desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            desc.Texture2D.MipLevels = 1;
            g_dx12.device->CreateShaderResourceView(frame.color.Get(), &desc, srv);
        }
        initialized = true;
        failed = false;
        return true;
    }

    uint64_t Render(const WeaponPreviewMesh& geometry, float yaw, float pitch,
                    float zoom, ImVec2 size, ImVec2& uvMax, bool sideView = false,
                    float fitPitch = 0.12f) {
        if (!Initialize()) return 0;
        // MoveToNextFrame already retired this slot; other frames keep their own
        // camera constants, depth and image until ImGui has finished sampling them.
        Frame& frame = frames[g_dx12.frameIndex];
        const auto& mesh = geometry.bounds;
        const XMVECTOR minimum = XMVectorSet(mesh.minimum[0], mesh.minimum[1], mesh.minimum[2], 0);
        const XMVECTOR maximum = XMVectorSet(mesh.maximum[0], mesh.maximum[1], mesh.maximum[2], 0);
        const XMVECTOR center = XMVectorScale(XMVectorAdd(minimum, maximum), 0.5f);
        const float span = (std::max)({ mesh.maximum[0] - mesh.minimum[0],
            mesh.maximum[1] - mesh.minimum[1], mesh.maximum[2] - mesh.minimum[2], 0.001f });
        const float scale = 1.65f / span;
        const XMMATRIX world = XMMatrixTranslationFromVector(XMVectorNegate(center)) *
            XMMatrixScaling(scale, scale, scale);
        const float radius = XMVectorGetX(XMVector3Length(
            XMVectorScale(XMVectorSubtract(maximum, minimum), 0.5f * scale)));
        const float baseYaw = mesh.maximum[0] - mesh.minimum[0] >=
            mesh.maximum[2] - mesh.minimum[2] ? 0.0f : XM_PIDIV2;
        const float aspect = size.x / size.y;
        constexpr float fov = XM_PI / 5.0f;
        const float tanY = std::tan(fov * 0.5f), tanX = tanY * aspect;
        const auto direction = [](float y, float p) {
            return XMVectorSet(std::sin(y) * std::cos(p), std::sin(p),
                               -std::cos(y) * std::cos(p), 0.0f);
        };
        // Fit the live inspector at its default pose. Orbit changes direction,
        // while only the zoom control changes distance from the model.
        const XMMATRIX fitView = XMMatrixLookToLH(XMVectorZero(),
            XMVectorNegate(direction(baseYaw + (sideView ? yaw : 0.18f),
                sideView ? pitch : fitPitch)), XMVectorSet(0, 1, 0, 0));
        float distance = radius + 0.1f;
        float halfWidth = 0.0f, halfHeight = 0.0f;
        // Fit the actual bounds to both axes of the wide preview, rather than
        // treating a rifle's length as its height and leaving it tiny in the frame.
        for (int corner = 0; corner < 8; ++corner) {
            const XMVECTOR point = XMVectorSet(
                (corner & 1) ? mesh.maximum[0] : mesh.minimum[0],
                (corner & 2) ? mesh.maximum[1] : mesh.minimum[1],
                (corner & 4) ? mesh.maximum[2] : mesh.minimum[2], 1);
            const XMVECTOR viewPoint = XMVector3TransformCoord(point, world * fitView);
            halfWidth = (std::max)(halfWidth, std::abs(XMVectorGetX(viewPoint)));
            halfHeight = (std::max)(halfHeight, std::abs(XMVectorGetY(viewPoint)));
            distance = (std::max)({ distance,
                std::abs(XMVectorGetX(viewPoint)) / tanX - XMVectorGetZ(viewPoint),
                std::abs(XMVectorGetY(viewPoint)) / tanY - XMVectorGetZ(viewPoint) });
        }
        distance = (std::max)(radius + 0.1f, distance * 1.12f * zoom);
        const XMMATRIX view = XMMatrixLookAtLH(
            XMVectorScale(direction(baseYaw + yaw, pitch), distance),
            XMVectorZero(), XMVectorSet(0, 1, 0, 0));
        const float farPlane = (std::max)(20.0f, distance + radius + 1.0f);
        const float iconHeight = (std::max)(halfHeight, halfWidth / aspect) * 2.24f;
        const XMMATRIX projection = sideView
            ? XMMatrixOrthographicLH(iconHeight * aspect, iconHeight, 0.05f, farPlane)
            : XMMatrixPerspectiveFovLH(fov, aspect, 0.05f, farPlane);
        struct CameraConstants {
            XMFLOAT4X4 modelViewProjection;
            XMFLOAT4 eyePosition;
        } constants;
        static_assert(sizeof(CameraConstants) == 80, "PreviewCamera cbuffer layout");
        XMStoreFloat4x4(&constants.modelViewProjection, XMMatrixTranspose(world * view * projection));
        XMStoreFloat4(&constants.eyePosition, XMVectorAdd(center,
            XMVectorScale(direction(baseYaw + yaw, pitch), distance / scale)));
        void* mapped = nullptr;
        D3D12_RANGE noRead{ 0, 0 };
        if (FAILED(frame.constants->Map(0, &noRead, &mapped)) || !mapped) return 0;
        memcpy(mapped, &constants, sizeof(constants));
        frame.constants->Unmap(0, nullptr);

        const float resolutionScale = (std::min)({ 1.0f, Width / size.x, Height / size.y });
        const UINT width = (std::max)(1u, static_cast<UINT>(size.x * resolutionScale));
        const UINT height = (std::max)(1u, static_cast<UINT>(size.y * resolutionScale));
        uvMax = ImVec2(float(width) / Width, float(height) / Height);
        ProfilerDX12::Scope profile(g_profiler, "Weapon Preview", g_dx12.commandList.Get());
        auto* command = g_dx12.commandList.Get();
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = frame.color.Get();
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        command->ResourceBarrier(1, &barrier);
        auto rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += static_cast<SIZE_T>(g_dx12.frameIndex) *
            g_dx12.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        auto dsv = dsvHeap->GetCPUDescriptorHandleForHeapStart();
        dsv.ptr += static_cast<SIZE_T>(g_dx12.frameIndex) *
            g_dx12.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        const float clear[] = { 0.13f, 0.15f, 0.18f, 1.0f };
        D3D12_VIEWPORT viewport{ 0, 0, float(width), float(height), 0, 1 };
        D3D12_RECT scissor{ 0, 0, LONG(width), LONG(height) };
        command->RSSetViewports(1, &viewport);
        command->RSSetScissorRects(1, &scissor);
        command->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
        command->ClearRenderTargetView(rtv, clear, 0, nullptr);
        command->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1, 0, 0, nullptr);
        ID3D12DescriptorHeap* heaps[] = { imguiSrvHeap.Get() };
        command->SetDescriptorHeaps(1, heaps);
        command->SetGraphicsRootSignature(g_weaponPreviewPipeline.rootSignature.Get());
        command->SetGraphicsRootConstantBufferView(0, frame.constants->GetGPUVirtualAddress());
        command->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const auto drawPart = [&](const WeaponPreviewMesh::Part& part, bool blend) {
            const auto& primitive = *part.primitive;
            const auto& material = primitive.material;
            SceneMaterial fallback;
            const auto& m = material ? *material : fallback;
            struct MaterialConstants {
                XMFLOAT4 baseColor, emissiveMetal, surface;
                UINT flags[4];
            } data{};
            static_assert(sizeof(MaterialConstants) == 64, "PreviewMaterial cbuffer layout");
            data.baseColor = m.baseColorFactor;
            XMFLOAT3 emissive = m.emissiveFactor;
            if (m.emissiveTexture && emissive.x == 0 && emissive.y == 0 && emissive.z == 0)
                emissive = XMFLOAT3(1, 1, 1);
            data.emissiveMetal = XMFLOAT4(emissive.x, emissive.y, emissive.z, m.metallicFactor);
            data.surface = XMFLOAT4(m.roughnessFactor, m.normalYSign, m.alphaCutoff, m.occlusionStrength);
            data.flags[0] = (m.baseColorTexture ? 1u : 0u) | (m.normalTexture ? 2u : 0u) |
                (m.metallicRoughnessTexture ? 4u : 0u) | (m.emissiveTexture ? 8u : 0u);
            const auto srgb = [](ID3D12Resource* texture) {
                if (!texture) return false;
                const auto format = texture->GetDesc().Format;
                return format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
                    format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ||
                    format == DXGI_FORMAT_BC1_UNORM_SRGB || format == DXGI_FORMAT_BC2_UNORM_SRGB ||
                    format == DXGI_FORMAT_BC3_UNORM_SRGB || format == DXGI_FORMAT_BC7_UNORM_SRGB;
            };
            if (srgb(m.baseColorTexture.Get())) data.flags[0] |= 16;
            if (srgb(m.emissiveTexture.Get())) data.flags[0] |= 32;
            data.flags[1] = m.roughnessOnlyTexture ? 1u : 0u;
            data.flags[2] = (m.alphaCutout ? 1u : 0u) | (m.alphaFromLuminance ? 2u : 0u) |
                (blend ? 4u : 0u);
            auto gpu = imguiSrvHeap->GetGPUDescriptorHandleForHeapStart();
            gpu.ptr += static_cast<UINT64>(part.descriptorSlot) *
                g_dx12.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            command->SetPipelineState(blend ? g_weaponPreviewPipeline.transparent.Get() :
                                             g_weaponPreviewPipeline.opaque.Get());
            command->SetGraphicsRootDescriptorTable(1, gpu);
            command->SetGraphicsRoot32BitConstants(2, 16, &data, 0);
            command->IASetVertexBuffers(0, 1, &primitive.vbv);
            command->IASetIndexBuffer(&primitive.ibv);
            command->DrawIndexedInstanced(primitive.indexCount, 1, 0, 0, 0);
        };
        static std::vector<const WeaponPreviewMesh::Part*> transparent;
        transparent.clear();
        for (const auto& part : geometry.parts) {
            if (part.primitive->material && part.primitive->material->IsTransparent())
                transparent.push_back(&part);
            else drawPart(part, false);
        }
        const XMMATRIX worldView = world * view;
        std::sort(transparent.begin(), transparent.end(), [&](const auto* a, const auto* b) {
            return XMVectorGetZ(XMVector3TransformCoord(XMLoadFloat3(&a->center), worldView)) >
                   XMVectorGetZ(XMVector3TransformCoord(XMLoadFloat3(&b->center), worldView));
        });
        for (const auto* part : transparent) drawPart(*part, true);
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        command->ResourceBarrier(1, &barrier);
        const auto backbuffer = GetCPUDescriptorHandle(g_dx12.rtvHeap.Get(),
            g_dx12.rtvDescriptorSize, g_dx12.frameIndex);
        command->OMSetRenderTargets(1, &backbuffer, FALSE, nullptr);
        const auto displayViewport = DisplayViewportDX12();
        const auto displayScissor = DisplayScissorDX12();
        command->RSSetViewports(1, &displayViewport);
        command->RSSetScissorRects(1, &displayScissor);
        auto gpu = imguiSrvHeap->GetGPUDescriptorHandleForHeapStart();
        gpu.ptr += static_cast<UINT64>(frame.descriptorSlot) *
            g_dx12.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        return gpu.ptr;
    }
};
static WeaponPreviewDX12 g_weaponPreview;
static std::unordered_map<const SceneMesh*, WeaponPreviewMesh> g_weaponPreviewMeshes;
