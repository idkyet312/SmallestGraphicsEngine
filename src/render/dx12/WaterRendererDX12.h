#pragma once

#include "DX12Core.h"
#include "OceanWaveSettings.h"
#include "Scene.h"
#include "WaterVolume.h"
#include <d3dcompiler.h>
#include <array>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

class WaterRendererDX12 {
public:
    bool initialized = false;

    ~WaterRendererDX12() {
        if (constantBuffer_ && mappedConstants_)
            constantBuffer_->Unmap(0, nullptr);
    }

    bool Init(UINT width, UINT height) {
        Shutdown();
        width_ = width;
        height_ = height;

        std::ifstream file("shaders/water_dx12.hlsl");
        if (!file) return false;
        std::stringstream stream;
        stream << file.rdbuf();
        shaderSource_ = stream.str();

        ComPtr<ID3DBlob> vertexShader;
        if (!Compile("VSMain", "vs_5_0", nullptr, vertexShader) ||
            !CreateRootSignature() ||
            !CreatePipelineSet(vertexShader.Get()) ||
            !CreateClipmap() ||
            !CreateConstantBuffer() ||
            !CreateDescriptorHeap() ||
            !CreateSceneCopies()) {
            Shutdown();
            return false;
        }
        initialized = true;
        return true;
    }

    bool Resize(UINT width, UINT height) {
        if (!initialized || width == 0 || height == 0) return false;
        width_ = width;
        height_ = height;
        hdrSceneCopy_.Reset();
        ldrSceneCopy_.Reset();
        hasHistory_ = false;
        return CreateSceneCopies();
    }

    void Shutdown() {
        initialized = false;
        rootSignature_.Reset();
        hdrMotionPSO_.Reset();
        hdrPSO_.Reset();
        ldrPSO_.Reset();
        ldrMSAADepthPSO_.Reset();
        clipVertexBuffer_.Reset();
        clipIndexBuffer_.Reset();
        hdrSceneCopy_.Reset();
        ldrSceneCopy_.Reset();
        descriptorHeap_.Reset();
        if (constantBuffer_ && mappedConstants_) {
            constantBuffer_->Unmap(0, nullptr);
            mappedConstants_ = nullptr;
        }
        constantBuffer_.Reset();
        clipVertexView_ = {};
        clipIndexView_ = {};
        clipIndexCount_ = 0;
        hasHistory_ = false;
    }

    void Render(const Scene& scene,
                WaterVolume& ocean,
                WaterVolume& pool,
                ID3D12Resource* target,
                D3D12_CPU_DESCRIPTOR_HANDLE targetRTV,
                ID3D12Resource* opaqueDepth,
                ID3D12Resource* environment,
                bool hdrTarget,
                ID3D12Resource* motionTarget = nullptr,
                D3D12_CPU_DESCRIPTOR_HANDLE motionRTV = {},
                D3D12_RESOURCE_STATES depthState =
                    D3D12_RESOURCE_STATE_DEPTH_WRITE) {
        if (!initialized || !target || !targetRTV.ptr || !opaqueDepth ||
            (!ocean.IsInitialized() && !pool.IsInitialized())) return;

        const bool depthMSAA = opaqueDepth->GetDesc().SampleDesc.Count > 1;
        const bool writeMotion =
            hdrTarget && motionTarget && motionRTV.ptr && !depthMSAA;
        ID3D12PipelineState* pso = nullptr;
        if (hdrTarget)
            pso = writeMotion ? hdrMotionPSO_.Get() : hdrPSO_.Get();
        else
            pso = depthMSAA ? ldrMSAADepthPSO_.Get() : ldrPSO_.Get();
        if (!pso) return;

        ID3D12Resource* sceneCopy =
            hdrTarget ? hdrSceneCopy_.Get() : ldrSceneCopy_.Get();
        if (!sceneCopy) return;

        ID3D12GraphicsCommandList* list = g_dx12.commandList.Get();
        Transition(list, target, D3D12_RESOURCE_STATE_RENDER_TARGET,
                   D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyResource(sceneCopy, target);
        Transition(list, target, D3D12_RESOURCE_STATE_COPY_SOURCE,
                   D3D12_RESOURCE_STATE_RENDER_TARGET);
        Transition(list, sceneCopy, D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Transition(list, opaqueDepth, depthState,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        if (writeMotion)
            Transition(list, motionTarget,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_RENDER_TARGET);

        UpdateDescriptors(sceneCopy, opaqueDepth, environment);
        ID3D12DescriptorHeap* heaps[] = { descriptorHeap_.Get() };
        list->SetDescriptorHeaps(1, heaps);
        list->SetGraphicsRootSignature(rootSignature_.Get());
        list->SetPipelineState(pso);
        list->SetGraphicsRootDescriptorTable(
            1, DescriptorGPU(g_dx12.frameIndex * kDescriptorsPerFrame));
        list->RSSetViewports(1, &g_dx12.viewport);
        list->RSSetScissorRects(1, &g_dx12.scissorRect);
        if (writeMotion) {
            D3D12_CPU_DESCRIPTOR_HANDLE rtvs[] = { targetRTV, motionRTV };
            list->OMSetRenderTargets(2, rtvs, FALSE, nullptr);
        } else {
            list->OMSetRenderTargets(1, &targetRTV, FALSE, nullptr);
        }

        const XMMATRIX view = scene.GetViewMatrix();
        const XMMATRIX projection = scene.GetProjectionMatrix();
        const XMMATRIX viewProjection = view * projection;
        const float snap = 0.25f;
        const XMFLOAT2 clipCenter = {
            std::floor(scene.camera.Position.x / snap) * snap,
            std::floor(scene.camera.Position.z / snap) * snap
        };
        if (!hasHistory_) {
            previousViewProjection_ = viewProjection;
            previousClipCenter_ = clipCenter;
            previousOceanTime_ = ocean.GetTime();
        }

        if (ocean.IsInitialized()) {
            Constants constants = BuildConstants(
                scene, ocean, viewProjection, clipCenter, true);
            BindConstants(constants, 0);
            list->IASetVertexBuffers(0, 1, &clipVertexView_);
            list->IASetIndexBuffer(&clipIndexView_);
            list->IASetPrimitiveTopology(
                D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            list->DrawIndexedInstanced(clipIndexCount_, 1, 0, 0, 0);
        }

        if (pool.IsInitialized()) {
            const D3D12_VERTEX_BUFFER_VIEW& vbv =
                pool.UpdateAndGetVBV(g_dx12.frameIndex);
            const D3D12_INDEX_BUFFER_VIEW& ibv = pool.GetIBV();
            const UINT indexCount = pool.GetIndexCount();
            if (vbv.BufferLocation && indexCount) {
                Constants constants = BuildConstants(
                    scene, pool, viewProjection, clipCenter, false);
                BindConstants(constants, 1);
                list->IASetVertexBuffers(0, 1, &vbv);
                list->IASetIndexBuffer(&ibv);
                list->IASetPrimitiveTopology(
                    D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                list->DrawIndexedInstanced(indexCount, 1, 0, 0, 0);
            }
        }

        if (writeMotion)
            Transition(list, motionTarget, D3D12_RESOURCE_STATE_RENDER_TARGET,
                       D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(list, opaqueDepth,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                   depthState);
        Transition(list, sceneCopy,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_COPY_DEST);

        previousViewProjection_ = viewProjection;
        previousClipCenter_ = clipCenter;
        previousOceanTime_ = ocean.GetTime();
        hasHistory_ = true;
    }

private:
    static constexpr UINT kDescriptorsPerFrame = 3;
    static constexpr UINT kDrawsPerFrame = 2;

    struct ClipVertex {
        XMFLOAT3 position;
        XMFLOAT3 normal;
        XMFLOAT2 texCoord;
        XMFLOAT4 tangent;
    };

    struct Constants {
        XMFLOAT4X4 viewProjection;
        XMFLOAT4X4 previousViewProjection;
        XMFLOAT4X4 inverseViewProjection;
        XMFLOAT4 cameraTime;
        XMFLOAT4 previousCameraTime;
        XMFLOAT4 screenParams;
        XMFLOAT4 volume0;
        XMFLOAT4 volume1;
        XMFLOAT4 clipmapParams;
        XMFLOAT4 opticalParams;
        XMFLOAT4 absorption;
        XMFLOAT4 shallowScatter;
        XMFLOAT4 deepScatter;
        XMFLOAT4 lightDirection;
        XMFLOAT4 lightColor;
        XMFLOAT4 waves[OceanWaveSettings::WaveCount];
        XMFLOAT4 waveExtra[OceanWaveSettings::WaveCount];
    };

    static UINT AlignConstantSize(UINT size) {
        return (size + 255u) & ~255u;
    }

    bool Compile(const char* entry, const char* target,
                 const D3D_SHADER_MACRO* defines,
                 ComPtr<ID3DBlob>& bytecode) {
        const UINT flags = D3DCOMPILE_ENABLE_STRICTNESS |
                           D3DCOMPILE_OPTIMIZATION_LEVEL3;
        ComPtr<ID3DBlob> errors;
        const HRESULT hr = D3DCompile(
            shaderSource_.data(), shaderSource_.size(),
            "water_dx12.hlsl", defines, nullptr, entry, target,
            flags, 0, &bytecode, &errors);
        if (FAILED(hr) && errors)
            std::cerr << static_cast<const char*>(
                errors->GetBufferPointer());
        return SUCCEEDED(hr);
    }

    bool CreateRootSignature() {
        D3D12_DESCRIPTOR_RANGE range = {};
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = kDescriptorsPerFrame;
        range.BaseShaderRegister = 0;

        D3D12_ROOT_PARAMETER roots[2] = {};
        roots[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        roots[0].Descriptor.ShaderRegister = 0;
        roots[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        roots[1].ParameterType =
            D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        roots[1].DescriptorTable.NumDescriptorRanges = 1;
        roots[1].DescriptorTable.pDescriptorRanges = &range;
        roots[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC samplers[2] = {};
        samplers[0].Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        for (UINT i = 0; i < 2; ++i) {
            samplers[i].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            samplers[i].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            samplers[i].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            samplers[i].MaxLOD = D3D12_FLOAT32_MAX;
            samplers[i].ShaderRegister = i;
            samplers[i].ShaderVisibility =
                D3D12_SHADER_VISIBILITY_PIXEL;
        }

        D3D12_ROOT_SIGNATURE_DESC desc = {};
        desc.NumParameters = _countof(roots);
        desc.pParameters = roots;
        desc.NumStaticSamplers = _countof(samplers);
        desc.pStaticSamplers = samplers;
        desc.Flags =
            D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> blob;
        ComPtr<ID3DBlob> errors;
        if (FAILED(D3D12SerializeRootSignature(
                &desc, D3D_ROOT_SIGNATURE_VERSION_1,
                &blob, &errors))) return false;
        return SUCCEEDED(g_dx12.device->CreateRootSignature(
            0, blob->GetBufferPointer(), blob->GetBufferSize(),
            IID_PPV_ARGS(&rootSignature_)));
    }

    bool CreatePipelineSet(ID3DBlob* vertexShader) {
        const D3D_SHADER_MACRO hdrMotionDefines[] = {
            {"WATER_HDR_TARGET", "1"},
            {"WATER_MOTION_OUTPUT", "1"},
            {nullptr, nullptr}
        };
        const D3D_SHADER_MACRO hdrDefines[] = {
            {"WATER_HDR_TARGET", "1"},
            {nullptr, nullptr}
        };
        const D3D_SHADER_MACRO ldrMSAADefines[] = {
            {"WATER_DEPTH_MSAA", "1"},
            {nullptr, nullptr}
        };
        ComPtr<ID3DBlob> hdrMotion;
        ComPtr<ID3DBlob> hdr;
        ComPtr<ID3DBlob> ldr;
        ComPtr<ID3DBlob> ldrMSAA;
        if (!Compile("PSMain", "ps_5_0", hdrMotionDefines, hdrMotion) ||
            !Compile("PSMain", "ps_5_0", hdrDefines, hdr) ||
            !Compile("PSMain", "ps_5_0", nullptr, ldr) ||
            !Compile("PSMain", "ps_5_0", ldrMSAADefines, ldrMSAA))
            return false;
        return CreatePipeline(vertexShader, hdrMotion.Get(),
                              DXGI_FORMAT_R16G16B16A16_FLOAT, true,
                              hdrMotionPSO_) &&
               CreatePipeline(vertexShader, hdr.Get(),
                              DXGI_FORMAT_R16G16B16A16_FLOAT, false,
                              hdrPSO_) &&
               CreatePipeline(vertexShader, ldr.Get(),
                              DXGI_FORMAT_R8G8B8A8_UNORM, false,
                              ldrPSO_) &&
               CreatePipeline(vertexShader, ldrMSAA.Get(),
                              DXGI_FORMAT_R8G8B8A8_UNORM, false,
                              ldrMSAADepthPSO_);
    }

    bool CreatePipeline(ID3DBlob* vertexShader, ID3DBlob* pixelShader,
                        DXGI_FORMAT colorFormat, bool motion,
                        ComPtr<ID3D12PipelineState>& output) {
        D3D12_INPUT_ELEMENT_DESC layout[] = {
            {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT,
             0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT,
             0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,
             0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0},
            {"TANGENT", 0, DXGI_FORMAT_R32G32B32A32_FLOAT,
             0, 32, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0}
        };
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
        desc.InputLayout = { layout, _countof(layout) };
        desc.pRootSignature = rootSignature_.Get();
        desc.VS = {
            vertexShader->GetBufferPointer(),
            vertexShader->GetBufferSize()
        };
        desc.PS = {
            pixelShader->GetBufferPointer(),
            pixelShader->GetBufferSize()
        };
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.RasterizerState.DepthClipEnable = TRUE;
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
            D3D12_COLOR_WRITE_ENABLE_ALL;
        if (motion)
            desc.BlendState.RenderTarget[1].RenderTargetWriteMask =
                D3D12_COLOR_WRITE_ENABLE_RED |
                D3D12_COLOR_WRITE_ENABLE_GREEN;
        desc.DepthStencilState.DepthEnable = FALSE;
        desc.DepthStencilState.StencilEnable = FALSE;
        desc.SampleMask = UINT_MAX;
        desc.PrimitiveTopologyType =
            D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = motion ? 2 : 1;
        desc.RTVFormats[0] = colorFormat;
        if (motion)
            desc.RTVFormats[1] = DXGI_FORMAT_R16G16_FLOAT;
        desc.SampleDesc.Count = 1;
        return SUCCEEDED(g_dx12.device->CreateGraphicsPipelineState(
            &desc, IID_PPV_ARGS(&output)));
    }

    bool CreateClipmap() {
        constexpr int gridCells = 64;
        constexpr int halfCells = gridCells / 2;
        constexpr int holeHalfCells = gridCells / 4;
        const float outerExtents[6] = {
            8.0f, 16.0f, 32.0f, 64.0f, 128.0f, 420.0f
        };
        std::vector<ClipVertex> vertices;
        std::vector<uint32_t> indices;
        vertices.reserve(6 * (gridCells + 1) * (gridCells + 1));

        for (int level = 0; level < 6; ++level) {
            const float outer = outerExtents[level];
            const float inner = level == 0 ? 0.0f : outerExtents[level - 1];
            const uint32_t baseVertex =
                static_cast<uint32_t>(vertices.size());
            auto coordinate = [&](int index) {
                const int signedCell = index - halfCells;
                const float sign = signedCell < 0 ? -1.0f : 1.0f;
                const float magnitude =
                    static_cast<float>(std::abs(signedCell));
                if (level == 0)
                    return outer * signedCell / halfCells;
                if (magnitude <= holeHalfCells)
                    return sign * inner * magnitude / holeHalfCells;
                const float t = (magnitude - holeHalfCells) /
                                (halfCells - holeHalfCells);
                return sign * (inner + (outer - inner) * t);
            };

            for (int z = 0; z <= gridCells; ++z) {
                for (int x = 0; x <= gridCells; ++x) {
                    ClipVertex vertex = {};
                    vertex.position = {
                        coordinate(x), 0.0f, coordinate(z)
                    };
                    vertex.normal = { 0.0f, 1.0f, 0.0f };
                    vertex.texCoord = {
                        static_cast<float>(x) / gridCells,
                        static_cast<float>(z) / gridCells
                    };
                    vertex.tangent = {
                        1.0f, 0.0f, 0.0f, static_cast<float>(level)
                    };
                    vertices.push_back(vertex);
                }
            }
            for (int z = 0; z < gridCells; ++z) {
                for (int x = 0; x < gridCells; ++x) {
                    if (level > 0) {
                        const float cx =
                            std::abs(x + 0.5f - halfCells);
                        const float cz =
                            std::abs(z + 0.5f - halfCells);
                        if (cx < holeHalfCells &&
                            cz < holeHalfCells) continue;
                    }
                    const uint32_t a =
                        baseVertex + z * (gridCells + 1) + x;
                    const uint32_t b = a + 1;
                    const uint32_t c = a + gridCells + 1;
                    const uint32_t d = c + 1;
                    indices.insert(indices.end(), {a, c, b, b, c, d});
                }
            }
        }

        if (!CreateUploadBuffer(
                vertices.data(),
                static_cast<UINT>(vertices.size() * sizeof(ClipVertex)),
                clipVertexBuffer_)) return false;
        if (!CreateUploadBuffer(
                indices.data(),
                static_cast<UINT>(indices.size() * sizeof(uint32_t)),
                clipIndexBuffer_)) return false;
        clipVertexView_.BufferLocation =
            clipVertexBuffer_->GetGPUVirtualAddress();
        clipVertexView_.SizeInBytes =
            static_cast<UINT>(vertices.size() * sizeof(ClipVertex));
        clipVertexView_.StrideInBytes = sizeof(ClipVertex);
        clipIndexView_.BufferLocation =
            clipIndexBuffer_->GetGPUVirtualAddress();
        clipIndexView_.SizeInBytes =
            static_cast<UINT>(indices.size() * sizeof(uint32_t));
        clipIndexView_.Format = DXGI_FORMAT_R32_UINT;
        clipIndexCount_ = static_cast<UINT>(indices.size());
        return true;
    }

    bool CreateUploadBuffer(const void* data, UINT size,
                            ComPtr<ID3D12Resource>& output) {
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = size;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(g_dx12.device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&output)))) return false;
        void* mapped = nullptr;
        if (FAILED(output->Map(0, nullptr, &mapped))) return false;
        memcpy(mapped, data, size);
        output->Unmap(0, nullptr);
        return true;
    }

    bool CreateConstantBuffer() {
        constantStride_ = AlignConstantSize(sizeof(Constants));
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = static_cast<UINT64>(
            constantStride_) * FRAME_COUNT * kDrawsPerFrame;
        desc.Height = 1;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(g_dx12.device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                IID_PPV_ARGS(&constantBuffer_)))) return false;
        return SUCCEEDED(constantBuffer_->Map(
            0, nullptr,
            reinterpret_cast<void**>(&mappedConstants_)));
    }

    bool CreateDescriptorHeap() {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.NumDescriptors = kDescriptorsPerFrame * FRAME_COUNT;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(g_dx12.device->CreateDescriptorHeap(
                &desc, IID_PPV_ARGS(&descriptorHeap_)))) return false;
        descriptorSize_ =
            g_dx12.device->GetDescriptorHandleIncrementSize(
                D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        return true;
    }

    bool CreateSceneCopies() {
        return CreateSceneCopy(
                   DXGI_FORMAT_R16G16B16A16_FLOAT, hdrSceneCopy_) &&
               CreateSceneCopy(
                   DXGI_FORMAT_R8G8B8A8_UNORM, ldrSceneCopy_);
    }

    bool CreateSceneCopy(DXGI_FORMAT format,
                         ComPtr<ID3D12Resource>& output) {
        D3D12_HEAP_PROPERTIES heap = {};
        heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width_;
        desc.Height = height_;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = format;
        desc.SampleDesc.Count = 1;
        if (FAILED(g_dx12.device->CreateCommittedResource(
                &heap, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                IID_PPV_ARGS(&output)))) return false;
        return true;
    }

    void UpdateDescriptors(ID3D12Resource* sceneCopy,
                           ID3D12Resource* depth,
                           ID3D12Resource* environment) {
        D3D12_CPU_DESCRIPTOR_HANDLE handle =
            DescriptorCPU(g_dx12.frameIndex * kDescriptorsPerFrame);
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Shader4ComponentMapping =
            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = sceneCopy->GetDesc().Format;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;
        g_dx12.device->CreateShaderResourceView(
            sceneCopy, &srv, handle);
        handle.ptr += descriptorSize_;

        srv = {};
        srv.Shader4ComponentMapping =
            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = DXGI_FORMAT_R32_FLOAT;
        if (depth->GetDesc().SampleDesc.Count > 1) {
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
        } else {
            srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Texture2D.MipLevels = 1;
        }
        g_dx12.device->CreateShaderResourceView(depth, &srv, handle);
        handle.ptr += descriptorSize_;

        srv = {};
        srv.Shader4ComponentMapping =
            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Format = environment
            ? environment->GetDesc().Format
            : DXGI_FORMAT_R16G16B16A16_FLOAT;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = environment
            ? environment->GetDesc().MipLevels : 1;
        g_dx12.device->CreateShaderResourceView(
            environment, &srv, handle);
    }

    Constants BuildConstants(const Scene& scene,
                             const WaterVolume& volume,
                             const XMMATRIX& currentViewProjection,
                             const XMFLOAT2& clipCenter,
                             bool ocean) const {
        Constants constants = {};
        XMStoreFloat4x4(
            &constants.viewProjection,
            XMMatrixTranspose(currentViewProjection));
        XMStoreFloat4x4(
            &constants.previousViewProjection,
            XMMatrixTranspose(previousViewProjection_));
        XMStoreFloat4x4(
            &constants.inverseViewProjection,
            XMMatrixTranspose(XMMatrixInverse(
                nullptr, currentViewProjection)));
        constants.cameraTime = {
            scene.camera.Position.x,
            scene.camera.Position.y,
            scene.camera.Position.z,
            volume.GetTime()
        };
        constants.previousCameraTime = {
            previousClipCenter_.x,
            previousClipCenter_.y,
            scene.camera.Position.z,
            ocean ? previousOceanTime_ : volume.GetTime()
        };
        constants.screenParams = {
            static_cast<float>(width_),
            static_cast<float>(height_),
            1.0f / width_,
            1.0f / height_
        };
        const XMFLOAT3 center = volume.GetCenter();
        const XMFLOAT3 extents = volume.GetExtents();
        constants.volume0 = {
            center.x, volume.GetSurfaceY(), center.z,
            ocean ? 1.0f : 0.0f
        };
        constants.volume1 = {
            extents.x * 0.5f, extents.z * 0.5f,
            ocean ? extents.x * 0.5f : 0.0f,
            scene.waterQuality == WaterQuality::High ? 1.0f : 0.0f
        };

        const OceanWaveSettings& settings =
            volume.GetOceanWaveSettings();
        constants.clipmapParams = {
            clipCenter.x, clipCenter.y,
            settings.microNormalStrength,
            ocean ? 0.105f : 0.085f
        };
        constants.opticalParams = {
            settings.foamDepth, settings.foamCrest,
            ocean ? 10.0f : 14.0f,
            scene.waterQuality == WaterQuality::High ? 0.86f : 0.45f
        };
        constants.absorption = {
            settings.absorption.x,
            settings.absorption.y,
            settings.absorption.z, 0.0f
        };
        constants.shallowScatter = {
            settings.shallowScatter.x,
            settings.shallowScatter.y,
            settings.shallowScatter.z, 0.0f
        };
        constants.deepScatter = {
            settings.deepScatter.x,
            settings.deepScatter.y,
            settings.deepScatter.z, 0.0f
        };
        XMVECTOR light = XMVector3Normalize(
            XMLoadFloat3(&scene.lightPos));
        XMFLOAT3 lightDirection;
        XMStoreFloat3(&lightDirection, light);
        constants.lightDirection = {
            lightDirection.x, lightDirection.y, lightDirection.z, 0.0f
        };
        const XMFLOAT3 effectiveLight = scene.EffectiveLightColor();
        constants.lightColor = {
            effectiveLight.x, effectiveLight.y, effectiveLight.z, 0.0f
        };
        for (size_t i = 0; i < OceanWaveSettings::WaveCount; ++i) {
            const OceanWave& wave = settings.waves[i];
            constants.waves[i] = {
                wave.direction.x, wave.direction.y,
                wave.amplitude, wave.wavelength
            };
            constants.waveExtra[i] = {
                wave.steepness, 0.0f, 0.0f, 0.0f
            };
        }
        return constants;
    }

    void BindConstants(const Constants& constants, UINT drawIndex) {
        const UINT slot =
            g_dx12.frameIndex * kDrawsPerFrame + drawIndex;
        uint8_t* destination =
            mappedConstants_ + static_cast<size_t>(slot) * constantStride_;
        memcpy(destination, &constants, sizeof(constants));
        g_dx12.commandList->SetGraphicsRootConstantBufferView(
            0, constantBuffer_->GetGPUVirtualAddress() +
               static_cast<UINT64>(slot) * constantStride_);
    }

    D3D12_CPU_DESCRIPTOR_HANDLE DescriptorCPU(UINT index) const {
        D3D12_CPU_DESCRIPTOR_HANDLE handle =
            descriptorHeap_->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<SIZE_T>(index) * descriptorSize_;
        return handle;
    }

    D3D12_GPU_DESCRIPTOR_HANDLE DescriptorGPU(UINT index) const {
        D3D12_GPU_DESCRIPTOR_HANDLE handle =
            descriptorHeap_->GetGPUDescriptorHandleForHeapStart();
        handle.ptr += static_cast<UINT64>(index) * descriptorSize_;
        return handle;
    }

    static void Transition(ID3D12GraphicsCommandList* list,
                           ID3D12Resource* resource,
                           D3D12_RESOURCE_STATES before,
                           D3D12_RESOURCE_STATES after) {
        if (!resource || before == after) return;
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource;
        barrier.Transition.StateBefore = before;
        barrier.Transition.StateAfter = after;
        barrier.Transition.Subresource =
            D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        list->ResourceBarrier(1, &barrier);
    }

    UINT width_ = 0;
    UINT height_ = 0;
    UINT descriptorSize_ = 0;
    UINT constantStride_ = 0;
    UINT clipIndexCount_ = 0;
    std::string shaderSource_;
    ComPtr<ID3D12RootSignature> rootSignature_;
    ComPtr<ID3D12PipelineState> hdrMotionPSO_;
    ComPtr<ID3D12PipelineState> hdrPSO_;
    ComPtr<ID3D12PipelineState> ldrPSO_;
    ComPtr<ID3D12PipelineState> ldrMSAADepthPSO_;
    ComPtr<ID3D12Resource> clipVertexBuffer_;
    ComPtr<ID3D12Resource> clipIndexBuffer_;
    D3D12_VERTEX_BUFFER_VIEW clipVertexView_ = {};
    D3D12_INDEX_BUFFER_VIEW clipIndexView_ = {};
    ComPtr<ID3D12Resource> hdrSceneCopy_;
    ComPtr<ID3D12Resource> ldrSceneCopy_;
    ComPtr<ID3D12DescriptorHeap> descriptorHeap_;
    ComPtr<ID3D12Resource> constantBuffer_;
    uint8_t* mappedConstants_ = nullptr;
    bool hasHistory_ = false;
    XMMATRIX previousViewProjection_ = XMMatrixIdentity();
    XMFLOAT2 previousClipCenter_ = { 0.0f, 0.0f };
    float previousOceanTime_ = 0.0f;
};
