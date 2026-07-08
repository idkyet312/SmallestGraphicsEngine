#ifndef VISIBILITY_BUFFER_DX12_H
#define VISIBILITY_BUFFER_DX12_H

#include "DX12Core.h"
#include "ShaderDX12.h"
#include <fstream>
#include <sstream>
#include <vector>

// Maximum draw calls the visibility buffer supports per frame
static const UINT VB_MAX_DRAW_CALLS = 512;
// Maximum total vertices across all draw calls
static const UINT VB_MAX_VERTICES = 1024 * 1024;
// Maximum total indices across all draw calls
static const UINT VB_MAX_INDICES = 1024 * 1024 * 3;

// Must match the compute shader's DrawCallData
struct VBDrawCallData {
    XMFLOAT4X4 modelMatrix;
    XMFLOAT3   objectColor;
    float      useTexture;
    float      metalness;
    float      roughness;
    float      useNormalMap;
    float      dcPad;
    UINT       vertexOffset;
    UINT       indexOffset;
    UINT       indexCount;
    UINT       hasIndices;
};

// Must match the compute shader's PackedVertex (two float4s)
struct VBPackedVertex {
    XMFLOAT4 d0; // pos.xyz, normal.x
    XMFLOAT4 d1; // normal.yz, uv.xy
};

// Must match FrameConstants in compute shader (256-byte aligned)
struct alignas(256) VBFrameConstants {
    XMMATRIX viewMatrix;
    XMMATRIX projMatrix;
    XMMATRIX invViewProj;
    XMFLOAT3 cameraPos;
    float    screenWidth;
    float    screenHeight;
    float    pad0;
    float    pad1;
    float    pad2;
};

class VisibilityBufferDX12 {
public:
    // The visibility render target (R32_UINT)
    ComPtr<ID3D12Resource> visBufferRT;
    ComPtr<ID3D12DescriptorHeap> visRtvHeap;    // RTV for visibility pass
    ComPtr<ID3D12DescriptorHeap> visSrvUavHeap; // SRV/UAV for compute resolve

    // Depth buffer SRV for the compute pass (reads main depth)
    // We'll create a SRV for the engine's existing depth buffer

    // Compute output texture (RGBA8, written by compute, copied to back buffer)
    ComPtr<ID3D12Resource> outputTexture;

    // Visibility pass PSO + root signature
    ComPtr<ID3D12RootSignature> visPassRootSig;
    ComPtr<ID3D12PipelineState> visPassPSO;

    // Compute resolve PSO + root signature
    ComPtr<ID3D12RootSignature> resolveRootSig;
    ComPtr<ID3D12PipelineState> resolvePSO;

    // GPU-visible structured buffers
    ComPtr<ID3D12Resource> drawCallBuffer;       // StructuredBuffer<DrawCallData>
    ComPtr<ID3D12Resource> drawCallUpload;
    ComPtr<ID3D12Resource> vertexDataBuffer;     // StructuredBuffer<PackedVertex>
    ComPtr<ID3D12Resource> vertexDataUpload;
    ComPtr<ID3D12Resource> indexDataBuffer;       // StructuredBuffer<uint>
    ComPtr<ID3D12Resource> indexDataUpload;

    // Upload buffer for frame constants
    UploadBuffer<VBFrameConstants> frameConstantBuffer;

    // Descriptor heap for compute pass SRVs/UAVs
    ComPtr<ID3D12DescriptorHeap> computeDescHeap;

    // CPU-side staging data
    std::vector<VBDrawCallData> cpuDrawCalls;
    std::vector<VBPackedVertex> cpuVertices;
    std::vector<UINT>           cpuIndices;
    UINT currentDrawCall = 0;
    UINT currentVertexOffset = 0;
    UINT currentIndexOffset = 0;

    UINT width = 0;
    UINT height = 0;
    bool initialized = false;

    bool Init(UINT screenWidth, UINT screenHeight) {
        width = screenWidth;
        height = screenHeight;

        if (!CreateVisBufferRT()) return false;
        if (!CreateOutputTexture()) return false;
        if (!CreateStructuredBuffers()) return false;
        if (!CreateVisPassPipeline()) return false;
        if (!CreateResolvePipeline()) return false;

        if (!frameConstantBuffer.Create(FRAME_COUNT)) return false;

        cpuDrawCalls.resize(VB_MAX_DRAW_CALLS);
        cpuVertices.resize(VB_MAX_VERTICES);
        cpuIndices.resize(VB_MAX_INDICES);

        initialized = true;
        std::cout << "Visibility Buffer initialized (" << width << "x" << height << ")" << std::endl;
        return true;
    }

    void BeginFrame() {
        currentDrawCall = 0;
        currentVertexOffset = 0;
        currentIndexOffset = 0;
    }

    // Register a draw call's geometry and material.
    // vertices: interleaved float array (pos3, norm3, uv2) = 8 floats per vert
    // Returns the drawCallID to pass to the visibility pass shader.
    UINT RegisterDrawCall(const XMMATRIX& modelMatrix,
                          const XMFLOAT3& color, float metalness, float roughness,
                          const float* vertexData, UINT vertexCount,
                          const UINT* indexData, UINT indexCount) {
        if (currentDrawCall >= VB_MAX_DRAW_CALLS) return 0;

        UINT dcID = currentDrawCall;
        VBDrawCallData& dc = cpuDrawCalls[dcID];

        XMMATRIX transposed = XMMatrixTranspose(modelMatrix);
        XMStoreFloat4x4(&dc.modelMatrix, transposed);

        dc.objectColor = color;
        dc.useTexture = 0.0f;
        dc.metalness = metalness;
        dc.roughness = roughness;
        dc.useNormalMap = 0.0f;
        dc.dcPad = 0.0f;
        dc.vertexOffset = currentVertexOffset;
        dc.indexOffset = currentIndexOffset;
        dc.indexCount = indexCount;
        dc.hasIndices = (indexData && indexCount > 0) ? 1 : 0;

        // Pack vertices
        for (UINT i = 0; i < vertexCount && (currentVertexOffset + i) < VB_MAX_VERTICES; i++) {
            const float* v = vertexData + i * 8;
            VBPackedVertex& pv = cpuVertices[currentVertexOffset + i];
            pv.d0 = XMFLOAT4(v[0], v[1], v[2], v[3]); // pos.xyz, norm.x
            pv.d1 = XMFLOAT4(v[4], v[5], v[6], v[7]); // norm.yz, uv.xy
        }
        currentVertexOffset += vertexCount;

        // Copy indices
        if (indexData && indexCount > 0) {
            for (UINT i = 0; i < indexCount && (currentIndexOffset + i) < VB_MAX_INDICES; i++) {
                cpuIndices[currentIndexOffset + i] = indexData[i];
            }
            currentIndexOffset += indexCount;
        }

        currentDrawCall++;
        return dcID;
    }

    // Upload all CPU-side data to GPU before the resolve pass
    void UploadBuffers(ID3D12GraphicsCommandList* cmdList) {
        // Upload draw calls
        {
            void* mapped = nullptr;
            D3D12_RANGE readRange = { 0, 0 };
            drawCallUpload->Map(0, &readRange, &mapped);
            memcpy(mapped, cpuDrawCalls.data(), currentDrawCall * sizeof(VBDrawCallData));
            drawCallUpload->Unmap(0, nullptr);

            cmdList->CopyBufferRegion(drawCallBuffer.Get(), 0,
                drawCallUpload.Get(), 0, currentDrawCall * sizeof(VBDrawCallData));
        }

        // Upload vertices
        if (currentVertexOffset > 0) {
            void* mapped = nullptr;
            D3D12_RANGE readRange = { 0, 0 };
            vertexDataUpload->Map(0, &readRange, &mapped);
            memcpy(mapped, cpuVertices.data(), currentVertexOffset * sizeof(VBPackedVertex));
            vertexDataUpload->Unmap(0, nullptr);

            cmdList->CopyBufferRegion(vertexDataBuffer.Get(), 0,
                vertexDataUpload.Get(), 0, currentVertexOffset * sizeof(VBPackedVertex));
        }

        // Upload indices
        if (currentIndexOffset > 0) {
            void* mapped = nullptr;
            D3D12_RANGE readRange = { 0, 0 };
            indexDataUpload->Map(0, &readRange, &mapped);
            memcpy(mapped, cpuIndices.data(), currentIndexOffset * sizeof(UINT));
            indexDataUpload->Unmap(0, nullptr);

            cmdList->CopyBufferRegion(indexDataBuffer.Get(), 0,
                indexDataUpload.Get(), 0, currentIndexOffset * sizeof(UINT));
        }

        // Barriers: transition structured buffers from copy dest to SRV
        D3D12_RESOURCE_BARRIER barriers[3] = {};
        barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[0].Transition.pResource = drawCallBuffer.Get();
        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

        barriers[1] = barriers[0];
        barriers[1].Transition.pResource = vertexDataBuffer.Get();

        barriers[2] = barriers[0];
        barriers[2].Transition.pResource = indexDataBuffer.Get();

        cmdList->ResourceBarrier(3, barriers);
    }

    // Transition structured buffers back to copy dest for next frame
    void TransitionBuffersForUpload(ID3D12GraphicsCommandList* cmdList) {
        D3D12_RESOURCE_BARRIER barriers[3] = {};
        barriers[0].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[0].Transition.pResource = drawCallBuffer.Get();
        barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        barriers[0].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;

        barriers[1] = barriers[0];
        barriers[1].Transition.pResource = vertexDataBuffer.Get();

        barriers[2] = barriers[0];
        barriers[2].Transition.pResource = indexDataBuffer.Get();

        cmdList->ResourceBarrier(3, barriers);
    }

    // Begin the visibility pass: clear VB RT, set render targets
    void BeginVisibilityPass(ID3D12GraphicsCommandList* cmdList) {
        // Transition vis buffer to render target
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = visBufferRT.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &barrier);

        // Clear visibility buffer to 0xFFFFFFFF (no geometry)
        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = visRtvHeap->GetCPUDescriptorHandleForHeapStart();
        const float clearVal[4] = { (float)0xFFFFFFFF, 0.0f, 0.0f, 0.0f };
        // For UINT targets, we need to use ClearUnorderedAccessViewUint or just clear to a UINT value.
        // Actually for RTV with UINT format, ClearRenderTargetView with the correct bit pattern works.
        // The bits of the float are reinterpreted as UINT. NaN float = 0xFFFFFFFF.
        UINT clearUint[4] = { 0xFFFFFFFF, 0, 0, 0 };
        // ClearRenderTargetView works for UINT formats if we pass the value as float bit-cast
        float clearFloat[4];
        memcpy(&clearFloat[0], &clearUint[0], sizeof(float));
        clearFloat[1] = 0.0f; clearFloat[2] = 0.0f; clearFloat[3] = 0.0f;
        cmdList->ClearRenderTargetView(rtvHandle, clearFloat, 0, nullptr);

        // Also clear main depth
        D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = g_dx12.dsvHeap->GetCPUDescriptorHandleForHeapStart();
        cmdList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

        // Set render targets: vis buffer + main depth buffer
        cmdList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

        // Set viewport / scissor
        cmdList->RSSetViewports(1, &g_dx12.viewport);
        cmdList->RSSetScissorRects(1, &g_dx12.scissorRect);

        // Set pipeline
        cmdList->SetGraphicsRootSignature(visPassRootSig.Get());
        cmdList->SetPipelineState(visPassPSO.Get());
    }

    // Set the draw call ID root constant before each draw
    void SetDrawCallID(ID3D12GraphicsCommandList* cmdList, UINT drawCallID) {
        cmdList->SetGraphicsRoot32BitConstant(1, drawCallID, 0);
    }

    // Set matrices for the current draw (reuses the matrix CBV at slot 0)
    void SetVisPassMatrices(ID3D12GraphicsCommandList* cmdList,
                            const XMMATRIX& model, const XMMATRIX& view,
                            const XMMATRIX& proj, const XMMATRIX& lightSpace,
                            ShaderDX12& matrixSource, UINT drawIndex) {
        // We reuse the existing matrix buffer from ShaderDX12
        UINT bufferIndex = g_dx12.frameIndex * MAX_DRAW_CALLS_PER_FRAME + drawIndex;

        MatrixBufferDX12 data;
        data.model = XMMatrixTranspose(model);
        data.view = XMMatrixTranspose(view);
        data.projection = XMMatrixTranspose(proj);
        data.lightSpaceMatrix = XMMatrixTranspose(lightSpace);
        matrixSource.matrixBuffer.CopyData(bufferIndex, data);

        cmdList->SetGraphicsRootConstantBufferView(0,
            matrixSource.matrixBuffer.GetGPUAddress(bufferIndex));
    }

    void EndVisibilityPass(ID3D12GraphicsCommandList* cmdList) {
        // Transition vis buffer to SRV for compute
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = visBufferRT.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &barrier);
    }

    // Indirect dispatch for compute resolve
    ComPtr<ID3D12CommandSignature> resolveDispatchSignature;
    ComPtr<ID3D12Resource> resolveDispatchArgsBuffer;
    D3D12_DISPATCH_ARGUMENTS* mappedResolveDispatchArgs = nullptr;

    // Run the compute resolve pass
    void Resolve(ID3D12GraphicsCommandList* cmdList,
                 const XMMATRIX& view, const XMMATRIX& proj,
                 const XMFLOAT3& cameraPos,
                 const LightBufferDX12& lightData,
                 const PointLightsBufferDX12& pointLightData) {
        // Transition output texture to UAV
        {
            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = outputTexture.Get();
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            cmdList->ResourceBarrier(1, &barrier);
        }

        // Also transition depth buffer to SRV for reading
        {
            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = g_dx12.depthStencilBuffer.Get();
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            cmdList->ResourceBarrier(1, &barrier);
        }

        // Upload frame constants
        VBFrameConstants fc;
        fc.viewMatrix = XMMatrixTranspose(view);
        fc.projMatrix = XMMatrixTranspose(proj);
        XMMATRIX invVP = XMMatrixInverse(nullptr, view * proj);
        fc.invViewProj = XMMatrixTranspose(invVP);
        fc.cameraPos = cameraPos;
        fc.screenWidth = (float)width;
        fc.screenHeight = (float)height;
        fc.pad0 = fc.pad1 = fc.pad2 = 0.0f;
        frameConstantBuffer.CopyData(g_dx12.frameIndex, fc);

        // Set compute pipeline
        cmdList->SetComputeRootSignature(resolveRootSig.Get());
        cmdList->SetPipelineState(resolvePSO.Get());

        // Set descriptor heap
        ID3D12DescriptorHeap* heaps[] = { computeDescHeap.Get() };
        cmdList->SetDescriptorHeaps(1, heaps);

        // Bind root parameters
        // b0 - frame constants
        cmdList->SetComputeRootConstantBufferView(0,
            frameConstantBuffer.GetGPUAddress(g_dx12.frameIndex));

        // b1 - light buffer (we'll upload via a temporary inline approach)
        // Actually, we reuse the mainShader's lightBuffer
        // For simplicity, create inline CBVs using the mainShader's addresses
        // We'll pass these from outside. For now, bind the descriptor table.

        // Descriptor table at root param 1 (SRVs + UAV)
        cmdList->SetComputeRootDescriptorTable(1,
            computeDescHeap->GetGPUDescriptorHandleForHeapStart());

        // Dispatch (GPU-driven via ExecuteIndirect)
        UINT groupsX = (width + 7) / 8;
        UINT groupsY = (height + 7) / 8;
        if (mappedResolveDispatchArgs) {
            mappedResolveDispatchArgs->ThreadGroupCountX = groupsX;
            mappedResolveDispatchArgs->ThreadGroupCountY = groupsY;
            mappedResolveDispatchArgs->ThreadGroupCountZ = 1;
        }
        if (resolveDispatchSignature && resolveDispatchArgsBuffer) {
            cmdList->ExecuteIndirect(resolveDispatchSignature.Get(), 1, resolveDispatchArgsBuffer.Get(), 0, nullptr, 0);
        } else {
            cmdList->Dispatch(groupsX, groupsY, 1);
        }

        // Transition output texture back to copy source
        {
            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = outputTexture.Get();
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            cmdList->ResourceBarrier(1, &barrier);
        }

        // Transition depth buffer back
        {
            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = g_dx12.depthStencilBuffer.Get();
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_DEPTH_WRITE;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            cmdList->ResourceBarrier(1, &barrier);
        }
    }

    // Copy the resolved output to the back buffer
    void CopyToBackBuffer(ID3D12GraphicsCommandList* cmdList) {
        ID3D12Resource* backBuffer = g_dx12.renderTargets[g_dx12.frameIndex].Get();

        // Back buffer is already in RENDER_TARGET state from BeginFrame
        // Transition it to COPY_DEST
        {
            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = backBuffer;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            cmdList->ResourceBarrier(1, &barrier);
        }

        cmdList->CopyResource(backBuffer, outputTexture.Get());

        // Transition back to RENDER_TARGET for ImGui
        {
            D3D12_RESOURCE_BARRIER barrier = {};
            barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier.Transition.pResource = backBuffer;
            barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
            barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            cmdList->ResourceBarrier(1, &barrier);
        }
    }

    void Resize(UINT newWidth, UINT newHeight) {
        if (newWidth == width && newHeight == height) return;
        width = newWidth;
        height = newHeight;

        visBufferRT.Reset();
        outputTexture.Reset();
        visRtvHeap.Reset();

        CreateVisBufferRT();
        CreateOutputTexture();
        UpdateComputeDescriptors();
    }

private:
    bool CreateVisBufferRT() {
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R32_UINT;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_CLEAR_VALUE clearValue = {};
        clearValue.Format = DXGI_FORMAT_R32_UINT;
        // Clear to 0xFFFFFFFF
        UINT clearUint = 0xFFFFFFFF;
        memcpy(&clearValue.Color[0], &clearUint, sizeof(float));
        clearValue.Color[1] = 0.0f;
        clearValue.Color[2] = 0.0f;
        clearValue.Color[3] = 0.0f;

        HRESULT hr = g_dx12.device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, &clearValue,
            IID_PPV_ARGS(&visBufferRT));
        if (FAILED(hr)) {
            std::cerr << "Failed to create visibility buffer RT" << std::endl;
            return false;
        }

        // Create RTV heap
        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
        rtvHeapDesc.NumDescriptors = 1;
        rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        hr = g_dx12.device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&visRtvHeap));
        if (FAILED(hr)) return false;

        // Create RTV
        D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
        rtvDesc.Format = DXGI_FORMAT_R32_UINT;
        rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        g_dx12.device->CreateRenderTargetView(visBufferRT.Get(), &rtvDesc,
            visRtvHeap->GetCPUDescriptorHandleForHeapStart());

        return true;
    }

    bool CreateOutputTexture() {
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

        HRESULT hr = g_dx12.device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr,
            IID_PPV_ARGS(&outputTexture));
        if (FAILED(hr)) {
            std::cerr << "Failed to create VB output texture" << std::endl;
            return false;
        }
        return true;
    }

    bool CreateStructuredBuffers() {
        auto CreateDefaultAndUpload = [](UINT64 size,
                                          ComPtr<ID3D12Resource>& defaultBuf,
                                          ComPtr<ID3D12Resource>& uploadBuf) -> bool {
            D3D12_HEAP_PROPERTIES defaultHeap = {};
            defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_HEAP_PROPERTIES uploadHeap = {};
            uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

            D3D12_RESOURCE_DESC bufDesc = {};
            bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bufDesc.Width = size;
            bufDesc.Height = 1;
            bufDesc.DepthOrArraySize = 1;
            bufDesc.MipLevels = 1;
            bufDesc.Format = DXGI_FORMAT_UNKNOWN;
            bufDesc.SampleDesc.Count = 1;
            bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

            HRESULT hr = g_dx12.device->CreateCommittedResource(
                &defaultHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&defaultBuf));
            if (FAILED(hr)) return false;

            hr = g_dx12.device->CreateCommittedResource(
                &uploadHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&uploadBuf));
            if (FAILED(hr)) return false;

            return true;
        };

        if (!CreateDefaultAndUpload(VB_MAX_DRAW_CALLS * sizeof(VBDrawCallData),
                                     drawCallBuffer, drawCallUpload))
            return false;

        if (!CreateDefaultAndUpload(VB_MAX_VERTICES * sizeof(VBPackedVertex),
                                     vertexDataBuffer, vertexDataUpload))
            return false;

        if (!CreateDefaultAndUpload(VB_MAX_INDICES * sizeof(UINT),
                                     indexDataBuffer, indexDataUpload))
            return false;

        return true;
    }

    bool CreateVisPassPipeline() {
        // Read and compile shaders
        std::ifstream vsFile("shaders/visbuf_vs.hlsl");
        std::ifstream psFile("shaders/visbuf_ps.hlsl");
        if (!vsFile.is_open() || !psFile.is_open()) {
            std::cerr << "Failed to open visibility buffer shader files" << std::endl;
            return false;
        }

        std::stringstream vsSS, psSS;
        vsSS << vsFile.rdbuf();
        psSS << psFile.rdbuf();
        std::string vsCode = vsSS.str();
        std::string psCode = psSS.str();

        UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
        compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
        compileFlags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

        ComPtr<ID3DBlob> vsBlob, psBlob, errorBlob;

        HRESULT hr = D3DCompile(vsCode.c_str(), vsCode.length(), "visbuf_vs.hlsl",
            nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "vs_5_0",
            compileFlags, 0, &vsBlob, &errorBlob);
        if (FAILED(hr)) {
            if (errorBlob) std::cerr << "VB VS error: " << (char*)errorBlob->GetBufferPointer() << std::endl;
            return false;
        }

        errorBlob.Reset();
        hr = D3DCompile(psCode.c_str(), psCode.length(), "visbuf_ps.hlsl",
            nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "ps_5_0",
            compileFlags, 0, &psBlob, &errorBlob);
        if (FAILED(hr)) {
            if (errorBlob) std::cerr << "VB PS error: " << (char*)errorBlob->GetBufferPointer() << std::endl;
            return false;
        }

        // Root signature for vis pass:
        // 0: CBV - MatrixBuffer (b0)
        // 1: Root constants - drawCallID (b1), 4 UINT values
        D3D12_ROOT_PARAMETER visParams[2] = {};

        visParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        visParams[0].Descriptor.ShaderRegister = 0;
        visParams[0].Descriptor.RegisterSpace = 0;
        visParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

        visParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        visParams[1].Constants.ShaderRegister = 1;
        visParams[1].Constants.RegisterSpace = 0;
        visParams[1].Constants.Num32BitValues = 4;
        visParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC visRootSigDesc = {};
        visRootSigDesc.NumParameters = 2;
        visRootSigDesc.pParameters = visParams;
        visRootSigDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> sigBlob;
        errorBlob.Reset();
        hr = D3D12SerializeRootSignature(&visRootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errorBlob);
        if (FAILED(hr)) {
            if (errorBlob) std::cerr << "VB root sig error: " << (char*)errorBlob->GetBufferPointer() << std::endl;
            return false;
        }

        hr = g_dx12.device->CreateRootSignature(0, sigBlob->GetBufferPointer(),
            sigBlob->GetBufferSize(), IID_PPV_ARGS(&visPassRootSig));
        if (FAILED(hr)) return false;

        // Input layout - same as main shader
        D3D12_INPUT_ELEMENT_DESC inputLayout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0,  D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
        };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
        psoDesc.InputLayout = { inputLayout, _countof(inputLayout) };
        psoDesc.pRootSignature = visPassRootSig.Get();
        psoDesc.VS = { vsBlob->GetBufferPointer(), vsBlob->GetBufferSize() };
        psoDesc.PS = { psBlob->GetBufferPointer(), psBlob->GetBufferSize() };

        psoDesc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
        psoDesc.RasterizerState.FrontCounterClockwise = FALSE;
        psoDesc.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
        psoDesc.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
        psoDesc.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
        psoDesc.RasterizerState.DepthClipEnable = TRUE;

        psoDesc.BlendState.AlphaToCoverageEnable = FALSE;
        psoDesc.BlendState.IndependentBlendEnable = FALSE;
        psoDesc.BlendState.RenderTarget[0].BlendEnable = FALSE;
        psoDesc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

        psoDesc.DepthStencilState.DepthEnable = TRUE;
        psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        psoDesc.DepthStencilState.StencilEnable = FALSE;

        psoDesc.SampleMask = UINT_MAX;
        psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        psoDesc.NumRenderTargets = 1;
        psoDesc.RTVFormats[0] = DXGI_FORMAT_R32_UINT;
        psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        psoDesc.SampleDesc.Count = 1;

        hr = g_dx12.device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&visPassPSO));
        if (FAILED(hr)) {
            std::cerr << "Failed to create vis pass PSO, HRESULT: 0x" << std::hex << hr << std::dec << std::endl;
            return false;
        }

        std::cout << "Visibility pass pipeline created" << std::endl;
        return true;
    }

    bool CreateResolvePipeline() {
        // Read and compile compute shader
        std::ifstream csFile("shaders/visbuf_resolve_cs.hlsl");
        if (!csFile.is_open()) {
            std::cerr << "Failed to open visbuf_resolve_cs.hlsl" << std::endl;
            return false;
        }

        std::stringstream csSS;
        csSS << csFile.rdbuf();
        std::string csCode = csSS.str();

        UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
        compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
        compileFlags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

        ComPtr<ID3DBlob> csBlob, errorBlob;
        HRESULT hr = D3DCompile(csCode.c_str(), csCode.length(), "visbuf_resolve_cs.hlsl",
            nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE, "main", "cs_5_0",
            compileFlags, 0, &csBlob, &errorBlob);
        if (FAILED(hr)) {
            if (errorBlob) std::cerr << "VB CS error: " << (char*)errorBlob->GetBufferPointer() << std::endl;
            return false;
        }

        // Root signature for compute resolve:
        // 0: CBV (b0) - FrameConstants
        // 1: Descriptor table - SRVs (t0..t5) + UAV (u0) + CBVs (b1, b2)
        //
        // We'll put everything in a single descriptor table for simplicity.
        // Layout in the heap:
        //   [0] t0 - visBuffer SRV
        //   [1] t1 - depthBuffer SRV
        //   [2] t2 - (unused/shadow placeholder)
        //   [3] t3 - drawCalls SRV
        //   [4] t4 - vertices SRV
        //   [5] t5 - indices SRV
        //   [6] u0 - output UAV
        //   [7] b1 - light buffer CBV
        //   [8] b2 - point lights CBV

        D3D12_DESCRIPTOR_RANGE ranges[3] = {};
        // SRVs t0..t5
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 6;
        ranges[0].BaseShaderRegister = 0;
        ranges[0].RegisterSpace = 0;
        ranges[0].OffsetInDescriptorsFromTableStart = 0;

        // UAV u0
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 1;
        ranges[1].BaseShaderRegister = 0;
        ranges[1].RegisterSpace = 0;
        ranges[1].OffsetInDescriptorsFromTableStart = 6;

        // CBVs b1..b2
        ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_CBV;
        ranges[2].NumDescriptors = 2;
        ranges[2].BaseShaderRegister = 1;
        ranges[2].RegisterSpace = 0;
        ranges[2].OffsetInDescriptorsFromTableStart = 7;

        D3D12_ROOT_PARAMETER resolveParams[2] = {};

        // b0 - frame constants (root CBV)
        resolveParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        resolveParams[0].Descriptor.ShaderRegister = 0;
        resolveParams[0].Descriptor.RegisterSpace = 0;
        resolveParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        // Descriptor table
        resolveParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        resolveParams[1].DescriptorTable.NumDescriptorRanges = 3;
        resolveParams[1].DescriptorTable.pDescriptorRanges = ranges;
        resolveParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        // Static samplers
        D3D12_STATIC_SAMPLER_DESC staticSamplers[2] = {};

        // Regular sampler s0
        staticSamplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        staticSamplers[0].MipLODBias = 0.0f;
        staticSamplers[0].MinLOD = 0.0f;
        staticSamplers[0].MaxLOD = D3D12_FLOAT32_MAX;
        staticSamplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        staticSamplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        staticSamplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        staticSamplers[0].ShaderRegister = 0;
        staticSamplers[0].RegisterSpace = 0;
        staticSamplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        // Shadow comparison sampler s1
        staticSamplers[1].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR;
        staticSamplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        staticSamplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        staticSamplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        staticSamplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        staticSamplers[1].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
        staticSamplers[1].ShaderRegister = 1;
        staticSamplers[1].RegisterSpace = 0;
        staticSamplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC resolveRootSigDesc = {};
        resolveRootSigDesc.NumParameters = 2;
        resolveRootSigDesc.pParameters = resolveParams;
        resolveRootSigDesc.NumStaticSamplers = 2;
        resolveRootSigDesc.pStaticSamplers = staticSamplers;

        ComPtr<ID3DBlob> sigBlob;
        errorBlob.Reset();
        hr = D3D12SerializeRootSignature(&resolveRootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1, &sigBlob, &errorBlob);
        if (FAILED(hr)) {
            if (errorBlob) std::cerr << "VB resolve root sig error: " << (char*)errorBlob->GetBufferPointer() << std::endl;
            return false;
        }

        hr = g_dx12.device->CreateRootSignature(0, sigBlob->GetBufferPointer(),
            sigBlob->GetBufferSize(), IID_PPV_ARGS(&resolveRootSig));
        if (FAILED(hr)) return false;

        // Compute PSO
        D3D12_COMPUTE_PIPELINE_STATE_DESC cpsoDesc = {};
        cpsoDesc.pRootSignature = resolveRootSig.Get();
        cpsoDesc.CS = { csBlob->GetBufferPointer(), csBlob->GetBufferSize() };

        hr = g_dx12.device->CreateComputePipelineState(&cpsoDesc, IID_PPV_ARGS(&resolvePSO));
        if (FAILED(hr)) {
            std::cerr << "Failed to create VB resolve compute PSO" << std::endl;
            return false;
        }

        // Create indirect dispatch command signature + args buffer
        {
            D3D12_INDIRECT_ARGUMENT_DESC arg = {};
            arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;

            D3D12_COMMAND_SIGNATURE_DESC sigDesc = {};
            sigDesc.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
            sigDesc.NumArgumentDescs = 1;
            sigDesc.pArgumentDescs = &arg;

            hr = g_dx12.device->CreateCommandSignature(&sigDesc, nullptr, IID_PPV_ARGS(&resolveDispatchSignature));
            if (FAILED(hr)) return false;

            D3D12_HEAP_PROPERTIES heapProps = {};
            heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

            D3D12_RESOURCE_DESC bufDesc = {};
            bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bufDesc.Width = sizeof(D3D12_DISPATCH_ARGUMENTS);
            bufDesc.Height = 1;
            bufDesc.DepthOrArraySize = 1;
            bufDesc.MipLevels = 1;
            bufDesc.Format = DXGI_FORMAT_UNKNOWN;
            bufDesc.SampleDesc.Count = 1;
            bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

            hr = g_dx12.device->CreateCommittedResource(
                &heapProps, D3D12_HEAP_FLAG_NONE, &bufDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&resolveDispatchArgsBuffer));
            if (FAILED(hr)) return false;

            D3D12_RANGE rr = { 0, 0 };
            hr = resolveDispatchArgsBuffer->Map(0, &rr, reinterpret_cast<void**>(&mappedResolveDispatchArgs));
            if (FAILED(hr)) return false;
        }

        std::cout << "Visibility buffer resolve pipeline created" << std::endl;
        return true;
    }

    void UpdateComputeDescriptors() {
        UINT descSize = g_dx12.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = computeDescHeap->GetCPUDescriptorHandleForHeapStart();

        // [0] t0 - visBuffer SRV (R32_UINT)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = DXGI_FORMAT_R32_UINT;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Texture2D.MipLevels = 1;
            g_dx12.device->CreateShaderResourceView(visBufferRT.Get(), &srvDesc, cpuHandle);
            cpuHandle.ptr += descSize;
        }

        // [1] t1 - depthBuffer SRV (R32_FLOAT from D32 typeless)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Texture2D.MipLevels = 1;
            g_dx12.device->CreateShaderResourceView(g_dx12.depthStencilBuffer.Get(), &srvDesc, cpuHandle);
            cpuHandle.ptr += descSize;
        }

        // [2] t2 - shadow map placeholder (null SRV)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Texture2D.MipLevels = 1;
            g_dx12.device->CreateShaderResourceView(nullptr, &srvDesc, cpuHandle);
            cpuHandle.ptr += descSize;
        }

        // [3] t3 - drawCalls SRV (structured buffer)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = DXGI_FORMAT_UNKNOWN;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Buffer.FirstElement = 0;
            srvDesc.Buffer.NumElements = VB_MAX_DRAW_CALLS;
            srvDesc.Buffer.StructureByteStride = sizeof(VBDrawCallData);
            g_dx12.device->CreateShaderResourceView(drawCallBuffer.Get(), &srvDesc, cpuHandle);
            cpuHandle.ptr += descSize;
        }

        // [4] t4 - vertices SRV (structured buffer)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = DXGI_FORMAT_UNKNOWN;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Buffer.FirstElement = 0;
            srvDesc.Buffer.NumElements = VB_MAX_VERTICES;
            srvDesc.Buffer.StructureByteStride = sizeof(VBPackedVertex);
            g_dx12.device->CreateShaderResourceView(vertexDataBuffer.Get(), &srvDesc, cpuHandle);
            cpuHandle.ptr += descSize;
        }

        // [5] t5 - indices SRV (structured buffer)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = DXGI_FORMAT_UNKNOWN;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Buffer.FirstElement = 0;
            srvDesc.Buffer.NumElements = VB_MAX_INDICES;
            srvDesc.Buffer.StructureByteStride = sizeof(UINT);
            g_dx12.device->CreateShaderResourceView(indexDataBuffer.Get(), &srvDesc, cpuHandle);
            cpuHandle.ptr += descSize;
        }

        // [6] u0 - output UAV (R8G8B8A8_UNORM)
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
            uavDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            g_dx12.device->CreateUnorderedAccessView(outputTexture.Get(), nullptr, &uavDesc, cpuHandle);
            cpuHandle.ptr += descSize;
        }

        // [7] b1 - light buffer CBV (placeholder, will be updated per frame)
        // [8] b2 - point lights CBV (placeholder, will be updated per frame)
        // These will be created in UpdateLightDescriptors
    }

public:
    // Call this each frame before resolve to update the light CBV descriptors
    void UpdateLightDescriptors(D3D12_GPU_VIRTUAL_ADDRESS lightBufferAddr,
                                D3D12_GPU_VIRTUAL_ADDRESS pointLightsAddr) {
        UINT descSize = g_dx12.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = computeDescHeap->GetCPUDescriptorHandleForHeapStart();
        cpuHandle.ptr += 7 * descSize;

        // [7] b1 - light buffer CBV
        {
            D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
            cbvDesc.BufferLocation = lightBufferAddr;
            cbvDesc.SizeInBytes = sizeof(LightBufferDX12);
            g_dx12.device->CreateConstantBufferView(&cbvDesc, cpuHandle);
            cpuHandle.ptr += descSize;
        }

        // [8] b2 - point lights CBV
        {
            D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
            cbvDesc.BufferLocation = pointLightsAddr;
            cbvDesc.SizeInBytes = sizeof(PointLightsBufferDX12);
            g_dx12.device->CreateConstantBufferView(&cbvDesc, cpuHandle);
        }
    }

    // Update the shadow map SRV in the compute descriptor heap
    void UpdateShadowMapDescriptor(ID3D12Resource* shadowMapResource) {
        if (!computeDescHeap) return;
        
        UINT descSize = g_dx12.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = computeDescHeap->GetCPUDescriptorHandleForHeapStart();
        cpuHandle.ptr += 2 * descSize; // slot [2] = t2

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;

        if (shadowMapResource) {
            g_dx12.device->CreateShaderResourceView(shadowMapResource, &srvDesc, cpuHandle);
        } else {
            g_dx12.device->CreateShaderResourceView(nullptr, &srvDesc, cpuHandle);
        }
    }
};

#endif // VISIBILITY_BUFFER_DX12_H
