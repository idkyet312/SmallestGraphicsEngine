#pragma once

// Private application implementation; included once by main.cpp in dependency order.

struct PrefabThumbnailPipelineDX12 {
    ComPtr<ID3D12RootSignature> rootSignature;
    ComPtr<ID3D12PipelineState> pipelineState;
    ComPtr<ID3D12Resource> depth;
    ComPtr<ID3D12DescriptorHeap> dsvHeap;
    bool failed = false;

    bool Initialize() {
        if (pipelineState) return true;
        if (failed || !g_dx12.device) return false;
        const char* vertexSource = R"(
cbuffer ThumbnailConstants : register(b0) { float4x4 modelViewProjection; };
struct VSInput { float3 position : POSITION; float3 normal : NORMAL; float3 color : COLOR; };
struct VSOutput { float4 position : SV_POSITION; float3 normal : NORMAL; float3 color : COLOR; };
VSOutput main(VSInput input) {
    VSOutput output;
    output.position = mul(float4(input.position, 1.0), modelViewProjection);
    output.normal = input.normal;
    output.color = input.color;
    return output;
})";
        const char* pixelSource = R"(
struct PSInput { float4 position : SV_POSITION; float3 normal : NORMAL; float3 color : COLOR; };
float4 main(PSInput input) : SV_TARGET {
    float3 n = normalize(input.normal);
    float light = 0.30 + 0.70 * abs(dot(n, normalize(float3(-0.45, 0.8, -0.4))));
    return float4(saturate(input.color * light), 1.0);
})";
        ComPtr<ID3DBlob> vertexShader, pixelShader, errors;
        if (FAILED(D3DCompile(vertexSource, strlen(vertexSource),
                "PrefabThumbnailVS", nullptr, nullptr, "main", "vs_5_0",
                D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vertexShader, &errors)) ||
            FAILED(D3DCompile(pixelSource, strlen(pixelSource),
                "PrefabThumbnailPS", nullptr, nullptr, "main", "ps_5_0",
                D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &pixelShader, &errors))) {
            failed = true;
            SGE_LOG("LogPrefab", EngineLog::Level::Error,
                "Failed to compile DX12 prefab thumbnail shaders");
            return false;
        }
        D3D12_ROOT_PARAMETER parameter{};
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        parameter.Descriptor.ShaderRegister = 0;
        parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        D3D12_ROOT_SIGNATURE_DESC rootDesc{};
        rootDesc.NumParameters = 1;
        rootDesc.pParameters = &parameter;
        rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> serialized;
        if (FAILED(D3D12SerializeRootSignature(&rootDesc,
                D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors)) ||
            FAILED(g_dx12.device->CreateRootSignature(0, serialized->GetBufferPointer(),
                serialized->GetBufferSize(), IID_PPV_ARGS(&rootSignature)))) {
            failed = true;
            return false;
        }
        const D3D12_INPUT_ELEMENT_DESC input[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,
              D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12,
              D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 24,
              D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
        };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
        desc.pRootSignature = rootSignature.Get();
        desc.VS = { vertexShader->GetBufferPointer(), vertexShader->GetBufferSize() };
        desc.PS = { pixelShader->GetBufferPointer(), pixelShader->GetBufferSize() };
        desc.InputLayout = { input, static_cast<UINT>(std::size(input)) };
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.RasterizerState.DepthClipEnable = TRUE;
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
            D3D12_COLOR_WRITE_ENABLE_ALL;
        desc.DepthStencilState.DepthEnable = TRUE;
        desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        desc.SampleMask = UINT_MAX;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        desc.SampleDesc.Count = 1;
        if (FAILED(g_dx12.device->CreateGraphicsPipelineState(
                &desc, IID_PPV_ARGS(&pipelineState)))) {
            failed = true;
            return false;
        }
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc{};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        heapDesc.NumDescriptors = 1;
        if (FAILED(g_dx12.device->CreateDescriptorHeap(
                &heapDesc, IID_PPV_ARGS(&dsvHeap)))) return false;
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC depthDesc{};
        depthDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        depthDesc.Width = 128;
        depthDesc.Height = 128;
        depthDesc.DepthOrArraySize = 1;
        depthDesc.MipLevels = 1;
        depthDesc.Format = DXGI_FORMAT_D32_FLOAT;
        depthDesc.SampleDesc.Count = 1;
        depthDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        depthDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE clear{};
        clear.Format = depthDesc.Format;
        clear.DepthStencil.Depth = 1.0f;
        if (FAILED(g_dx12.device->CreateCommittedResource(&heap,
                D3D12_HEAP_FLAG_NONE, &depthDesc, D3D12_RESOURCE_STATE_DEPTH_WRITE,
                &clear, IID_PPV_ARGS(&depth)))) return false;
        g_dx12.device->CreateDepthStencilView(depth.Get(), nullptr,
            dsvHeap->GetCPUDescriptorHandleForHeapStart());
        return true;
    }
};
static PrefabThumbnailPipelineDX12 g_prefabThumbnailPipeline;

static uint64_t StableThumbnailHash(const std::string& value) {
    uint64_t hash = 1469598103934665603ull;
    for (unsigned char byte : value) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
    return hash;
}

static bool CreateThumbnailGpuResources(PrefabThumbnailRuntime& entry) {
    if (!entry.mesh || entry.mesh->vertices.empty() || entry.mesh->indices.empty() ||
        !g_prefabThumbnailPipeline.Initialize()) return false;
    const auto createUploadBuffer = [](UINT64 bytes, ComPtr<ID3D12Resource>& output) {
        D3D12_HEAP_PROPERTIES heap{};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = bytes;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        return SUCCEEDED(g_dx12.device->CreateCommittedResource(&heap,
            D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr, IID_PPV_ARGS(&output)));
    };
    const UINT64 vertexBytes = entry.mesh->vertices.size() *
        sizeof(PrefabThumbnailVertex);
    const UINT64 indexBytes = entry.mesh->indices.size() * sizeof(unsigned int);
    if (!createUploadBuffer(vertexBytes, entry.vertexBuffer) ||
        !createUploadBuffer(indexBytes, entry.indexBuffer) ||
        !createUploadBuffer(256, entry.constantBuffer)) return false;
    // Map can fail -- most often when the upload heap is exhausted, which is
    // exactly the state a level full of large uncooked art leaves the device
    // in. Ignoring the result left `mapped` null and handed it straight to
    // memcpy, so the write landed on address 0 and took the process down with
    // no log line: a crash that read as "shooting the base kills it" because
    // the armory thumbnail happened to be built while standing there.
    void* mapped = nullptr;
    if (FAILED(entry.vertexBuffer->Map(0, nullptr, &mapped)) || !mapped)
        return false;
    memcpy(mapped, entry.mesh->vertices.data(), static_cast<size_t>(vertexBytes));
    entry.vertexBuffer->Unmap(0, nullptr);
    mapped = nullptr;
    if (FAILED(entry.indexBuffer->Map(0, nullptr, &mapped)) || !mapped)
        return false;
    memcpy(mapped, entry.mesh->indices.data(), static_cast<size_t>(indexBytes));
    entry.indexBuffer->Unmap(0, nullptr);

    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC textureDesc{};
    textureDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    textureDesc.Width = 128;
    textureDesc.Height = 128;
    textureDesc.DepthOrArraySize = 1;
    textureDesc.MipLevels = 1;
    textureDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    textureDesc.SampleDesc.Count = 1;
    textureDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    textureDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE clear{};
    clear.Format = textureDesc.Format;
    clear.Color[0] = 0.13f; clear.Color[1] = 0.15f;
    clear.Color[2] = 0.18f; clear.Color[3] = 1.0f;
    if (FAILED(g_dx12.device->CreateCommittedResource(&heap,
            D3D12_HEAP_FLAG_NONE, &textureDesc, D3D12_RESOURCE_STATE_RENDER_TARGET,
            &clear, IID_PPV_ARGS(&entry.texture)))) return false;
    D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
    rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvDesc.NumDescriptors = 1;
    if (FAILED(g_dx12.device->CreateDescriptorHeap(
            &rtvDesc, IID_PPV_ARGS(&entry.rtvHeap)))) return false;
    g_dx12.device->CreateRenderTargetView(entry.texture.Get(), nullptr,
        entry.rtvHeap->GetCPUDescriptorHandleForHeapStart());

    UINT rows = 0;
    UINT64 rowSize = 0, readbackBytes = 0;
    g_dx12.device->GetCopyableFootprints(&textureDesc, 0, 1, 0,
        &entry.readbackFootprint, &rows, &rowSize, &readbackBytes);
    D3D12_HEAP_PROPERTIES readbackHeap{};
    readbackHeap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC readbackDesc{};
    readbackDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    readbackDesc.Width = readbackBytes;
    readbackDesc.Height = 1;
    readbackDesc.DepthOrArraySize = 1;
    readbackDesc.MipLevels = 1;
    readbackDesc.SampleDesc.Count = 1;
    readbackDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(g_dx12.device->CreateCommittedResource(&readbackHeap,
            D3D12_HEAP_FLAG_NONE, &readbackDesc, D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr, IID_PPV_ARGS(&entry.readback)))) return false;

    if (g_nextImGuiTextureSlot >= kImGuiDescriptorCount) return false;
    entry.descriptorSlot = g_nextImGuiTextureSlot++;
    const UINT stride = g_dx12.device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu =
        imguiSrvHeap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(entry.descriptorSlot) * stride;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = textureDesc.Format;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    g_dx12.device->CreateShaderResourceView(entry.texture.Get(), &srv, cpu);
    return true;
}

static bool RenderThumbnailToTexture(PrefabThumbnailRuntime& entry) {
    if (!CreateThumbnailGpuResources(entry)) return false;
    const float centerX = (entry.mesh->minimum[0] + entry.mesh->maximum[0]) * 0.5f;
    const float centerY = (entry.mesh->minimum[1] + entry.mesh->maximum[1]) * 0.5f;
    const float centerZ = (entry.mesh->minimum[2] + entry.mesh->maximum[2]) * 0.5f;
    const float span = (std::max)({ entry.mesh->maximum[0] - entry.mesh->minimum[0],
        entry.mesh->maximum[1] - entry.mesh->minimum[1],
        entry.mesh->maximum[2] - entry.mesh->minimum[2], 0.001f });
    const XMMATRIX world = XMMatrixTranslation(-centerX, -centerY, -centerZ) *
        XMMatrixScaling(1.65f / span, 1.65f / span, 1.65f / span);
    const XMMATRIX view = XMMatrixLookAtLH(XMVectorSet(2.4f, 1.8f, -2.4f, 1.0f),
        XMVectorZero(), XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
    const XMMATRIX projection = XMMatrixPerspectiveFovLH(
        XMConvertToRadians(32.0f), 1.0f, 0.1f, 20.0f);
    XMFLOAT4X4 constants;
    XMStoreFloat4x4(&constants, XMMatrixTranspose(world * view * projection));
    // Same unchecked-Map hazard as the vertex and index uploads above: a failed
    // map here wrote the transform through a null pointer.
    void* mapped = nullptr;
    if (FAILED(entry.constantBuffer->Map(0, nullptr, &mapped)) || !mapped)
        return false;
    memcpy(mapped, &constants, sizeof(constants));
    entry.constantBuffer->Unmap(0, nullptr);

    const D3D12_CPU_DESCRIPTOR_HANDLE rtv =
        entry.rtvHeap->GetCPUDescriptorHandleForHeapStart();
    const D3D12_CPU_DESCRIPTOR_HANDLE dsv =
        g_prefabThumbnailPipeline.dsvHeap->GetCPUDescriptorHandleForHeapStart();
    const float clear[] = { 0.13f, 0.15f, 0.18f, 1.0f };
    D3D12_VIEWPORT viewport{ 0.0f, 0.0f, 128.0f, 128.0f, 0.0f, 1.0f };
    D3D12_RECT scissor{ 0, 0, 128, 128 };
    g_dx12.commandList->RSSetViewports(1, &viewport);
    g_dx12.commandList->RSSetScissorRects(1, &scissor);
    g_dx12.commandList->OMSetRenderTargets(1, &rtv, FALSE, &dsv);
    g_dx12.commandList->ClearRenderTargetView(rtv, clear, 0, nullptr);
    g_dx12.commandList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH,
        1.0f, 0, 0, nullptr);
    g_dx12.commandList->SetGraphicsRootSignature(
        g_prefabThumbnailPipeline.rootSignature.Get());
    g_dx12.commandList->SetPipelineState(
        g_prefabThumbnailPipeline.pipelineState.Get());
    g_dx12.commandList->SetGraphicsRootConstantBufferView(0,
        entry.constantBuffer->GetGPUVirtualAddress());
    D3D12_VERTEX_BUFFER_VIEW vbv{ entry.vertexBuffer->GetGPUVirtualAddress(),
        static_cast<UINT>(entry.mesh->vertices.size() * sizeof(PrefabThumbnailVertex)),
        sizeof(PrefabThumbnailVertex) };
    D3D12_INDEX_BUFFER_VIEW ibv{ entry.indexBuffer->GetGPUVirtualAddress(),
        static_cast<UINT>(entry.mesh->indices.size() * sizeof(unsigned int)),
        DXGI_FORMAT_R32_UINT };
    g_dx12.commandList->IASetVertexBuffers(0, 1, &vbv);
    g_dx12.commandList->IASetIndexBuffer(&ibv);
    g_dx12.commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    g_dx12.commandList->DrawIndexedInstanced(
        static_cast<UINT>(entry.mesh->indices.size()), 1, 0, 0, 0);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = entry.texture.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    g_dx12.commandList->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION destination{};
    destination.pResource = entry.readback.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = entry.readbackFootprint;
    D3D12_TEXTURE_COPY_LOCATION source{};
    source.pResource = entry.texture.Get();
    source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    g_dx12.commandList->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    g_dx12.commandList->ResourceBarrier(1, &barrier);

    const D3D12_CPU_DESCRIPTOR_HANDLE backbuffer = GetCPUDescriptorHandle(
        g_dx12.rtvHeap.Get(), g_dx12.rtvDescriptorSize, g_dx12.frameIndex);
    const D3D12_CPU_DESCRIPTOR_HANDLE mainDsv =
        g_dx12.dsvHeap->GetCPUDescriptorHandleForHeapStart();
    g_dx12.commandList->OMSetRenderTargets(1, &backbuffer, FALSE, &mainDsv);
    g_dx12.commandList->RSSetViewports(1, &g_dx12.viewport);
    g_dx12.commandList->RSSetScissorRects(1, &g_dx12.scissorRect);
    entry.renderFence = g_dx12.fenceValues[g_dx12.frameIndex];
    entry.rendered = true;
    return true;
}

static void CacheCompletedThumbnail(PrefabThumbnailRuntime& entry) {
    if (!entry.rendered || entry.readbackProcessed || !g_dx12.fence ||
        g_dx12.fence->GetCompletedValue() < entry.renderFence) return;
    unsigned char* mapped = nullptr;
    D3D12_RANGE readRange{ 0, static_cast<SIZE_T>(
        entry.readbackFootprint.Footprint.RowPitch * 128u) };
    if (FAILED(entry.readback->Map(0, &readRange,
            reinterpret_cast<void**>(&mapped)))) return;
    std::vector<unsigned char> pixels(128u * 128u * 4u);
    for (UINT row = 0; row < 128; ++row)
        memcpy(pixels.data() + row * 128u * 4u,
            mapped + entry.readbackFootprint.Offset +
                row * entry.readbackFootprint.Footprint.RowPitch,
            128u * 4u);
    D3D12_RANGE noWrite{ 0, 0 };
    entry.readback->Unmap(0, &noWrite);
    const std::filesystem::path path = entry.pngPath;
    entry.cacheWrite = std::async(std::launch::async,
        [path, pixels = std::move(pixels)]() {
            std::error_code error;
            std::filesystem::create_directories(path.parent_path(), error);
            return !error && stbi_write_png(path.string().c_str(), 128, 128, 4,
                pixels.data(), 128 * 4) != 0;
        });
    entry.readbackProcessed = true;
}

static uint64_t ThumbnailGpuHandle(const PrefabThumbnailRuntime& entry) {
    if (entry.descriptorSlot == ~0u) return 0;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu =
        imguiSrvHeap->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += static_cast<UINT64>(entry.descriptorSlot) *
        g_dx12.device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return gpu.ptr;
}

static uint64_t PrefabThumbnailTexture(const PrefabAsset& prefab) {
    if (!imguiSrvHeap || prefab.modelPath.empty()) return 0;
    if (g_prefabEditorSmokeEnabled) {
        std::error_code smokeError;
        if (std::filesystem::file_size(prefab.modelPath, smokeError) > 32768 ||
            smokeError) return 0;
    }
    std::error_code error;
    const std::string source = prefab.modelPath.generic_string() + ':' +
        std::to_string(std::filesystem::file_size(prefab.modelPath, error)) + ':' +
        std::to_string(std::filesystem::last_write_time(prefab.modelPath, error)
            .time_since_epoch().count()) + ":dx12-rtt-v2" +
        (g_prefabEditorSmokeEnabled
            ? (":smoke:" + std::to_string(GetCurrentProcessId())) : "");
    const uint64_t sourceHash = StableThumbnailHash(source);
    PrefabThumbnailRuntime& entry = g_prefabThumbnails[prefab.id];
    if (entry.sourceHash != sourceHash) {
        entry = PrefabThumbnailRuntime{};
        entry.sourceHash = sourceHash;
        entry.pngPath = std::filesystem::path("assetcache/thumbs") /
            (std::to_string(sourceHash) + ".png");
    }
    CacheCompletedThumbnail(entry);
    if (!entry.texture && std::filesystem::exists(entry.pngPath)) {
        if (g_thumbnailUploadedThisFrame ||
            g_nextImGuiTextureSlot >= kImGuiDescriptorCount) return 0;
        entry.texture = GLBImporter::LoadTextureSingleMip(entry.pngPath.string(),
            g_dx12.device, g_dx12.commandList, entry.uploads);
        if (!entry.texture) return 0;
        g_thumbnailUploadedThisFrame = true;
        entry.descriptorSlot = g_nextImGuiTextureSlot++;
        const UINT stride = g_dx12.device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE cpu =
            imguiSrvHeap->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += static_cast<SIZE_T>(entry.descriptorSlot) * stride;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        g_dx12.device->CreateShaderResourceView(entry.texture.Get(), &srv, cpu);
        return ThumbnailGpuHandle(entry);
    }
    if (!entry.texture && !entry.preparation.valid()) {
        size_t runningJobs = 0;
        for (auto& [id, thumbnail] : g_prefabThumbnails)
            if (thumbnail.preparation.valid() &&
                thumbnail.preparation.wait_for(std::chrono::seconds(0)) !=
                    std::future_status::ready)
                ++runningJobs;
        if (runningJobs >= 2) return 0;
        const auto modelPath = prefab.modelPath;
        const bool useMaterials = prefab.useMaterials;
        entry.preparation = std::async(std::launch::async,
            [modelPath, useMaterials]() {
            auto mesh = std::make_shared<PrefabThumbnailMesh>();
            if (!PrefabThumbnailGenerator::LoadMesh(modelPath, *mesh)) return
                std::shared_ptr<PrefabThumbnailMesh>{};
            if (!useMaterials)
                for (PrefabThumbnailVertex& vertex : mesh->vertices) {
                    vertex.r = 0.65f; vertex.g = 0.68f; vertex.b = 0.72f;
                }
            return mesh;
        });
        return 0;
    }
    if (!entry.texture && entry.preparation.valid()) {
        if (entry.preparation.wait_for(std::chrono::seconds(0)) !=
            std::future_status::ready) return 0;
        entry.mesh = entry.preparation.get();
        if (!entry.mesh || !RenderThumbnailToTexture(entry)) return 0;
    }
    return ThumbnailGpuHandle(entry);
}
