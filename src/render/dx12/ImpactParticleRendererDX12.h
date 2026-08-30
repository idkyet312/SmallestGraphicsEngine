#ifndef IMPACT_PARTICLE_RENDERER_DX12_H
#define IMPACT_PARTICLE_RENDERER_DX12_H

#include "ShaderDX12.h"
#include "Scene.h"
#include <array>
#include <fstream>
#include <functional>
#include <sstream>

struct ImpactParticleInstanceDX12 {
    XMFLOAT3 position;
    float size;
    XMFLOAT3 velocity;
    float opacity;
    XMFLOAT3 color;
    UINT kind;
};
static_assert(sizeof(ImpactParticleInstanceDX12) == 48);

struct alignas(256) ImpactParticleFrameDX12 {
    XMMATRIX viewProjection;
    XMFLOAT3 cameraRight;
    float smokeIllumination;
    XMFLOAT3 cameraUp;
    float padding1;
};

class ImpactParticleRendererDX12 {
public:
    static constexpr UINT MaxParticles = 1024;
    bool initialized = false;

    bool Init() {
        std::ifstream shaderFile("shaders/impact_particles_gpu.hlsl");
        if (!shaderFile) return false;
        std::stringstream shaderText;
        shaderText << shaderFile.rdbuf();
        const std::string source = shaderText.str();
        UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
        flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
        flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
        ComPtr<ID3DBlob> vs, ps, hdrPs, errors;
        if (FAILED(ShaderCacheDX12::CompileCached(source.data(), source.size(),
                "shaders/impact_particles_gpu.hlsl", nullptr,
                D3D_COMPILE_STANDARD_FILE_INCLUDE, "VSMain", "vs_5_0", flags,
                0, &vs, &errors))) {
            if (errors) std::cerr << (char*)errors->GetBufferPointer();
            return false;
        }
        errors.Reset();
        if (FAILED(ShaderCacheDX12::CompileCached(source.data(), source.size(),
                "shaders/impact_particles_gpu.hlsl", nullptr,
                D3D_COMPILE_STANDARD_FILE_INCLUDE, "PSMain", "ps_5_0", flags,
                0, &ps, &errors))) {
            if (errors) std::cerr << (char*)errors->GetBufferPointer();
            return false;
        }
        const D3D_SHADER_MACRO hdrDefines[] = {
            { "SGE_HDR_TARGET", "1" }, { nullptr, nullptr }
        };
        errors.Reset();
        if (FAILED(ShaderCacheDX12::CompileCached(source.data(), source.size(),
                "shaders/impact_particles_gpu.hlsl", hdrDefines,
                D3D_COMPILE_STANDARD_FILE_INCLUDE, "PSMain", "ps_5_0", flags,
                0, &hdrPs, &errors))) {
            if (errors) std::cerr << (char*)errors->GetBufferPointer();
            return false;
        }

        D3D12_DESCRIPTOR_RANGE textureRange = {};
        textureRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        textureRange.NumDescriptors = 1;
        textureRange.BaseShaderRegister = 1;
        D3D12_ROOT_PARAMETER roots[3] = {};
        roots[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        roots[0].Descriptor.ShaderRegister = 0;
        roots[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        roots[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        roots[1].Descriptor.ShaderRegister = 0;
        roots[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        roots[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        roots[2].DescriptorTable.NumDescriptorRanges = 1;
        roots[2].DescriptorTable.pDescriptorRanges = &textureRange;
        roots[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_STATIC_SAMPLER_DESC sampler = {};
        sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW =
            D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.MaxLOD = D3D12_FLOAT32_MAX;
        sampler.ShaderRegister = 0;
        sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        D3D12_ROOT_SIGNATURE_DESC rootDesc = {};
        rootDesc.NumParameters = _countof(roots);
        rootDesc.pParameters = roots;
        rootDesc.NumStaticSamplers = 1;
        rootDesc.pStaticSamplers = &sampler;
        rootDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        ComPtr<ID3DBlob> signature;
        if (FAILED(D3D12SerializeRootSignature(&rootDesc,
                D3D_ROOT_SIGNATURE_VERSION_1, &signature, &errors)) ||
            FAILED(g_dx12.device->CreateRootSignature(0,
                signature->GetBufferPointer(), signature->GetBufferSize(),
                IID_PPV_ARGS(&rootSignature_)))) return false;

        if (!CreatePipelines(vs.Get(), ps.Get(), hdrPs.Get())) return false;
        if (!instances_.Create(MaxParticles * FRAME_COUNT) ||
            !frames_.Create(FRAME_COUNT)) return false;
        D3D12_DESCRIPTOR_HEAP_DESC heap = {};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.NumDescriptors = 2;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(g_dx12.device->CreateDescriptorHeap(
                &heap, IID_PPV_ARGS(&textureHeap_)))) return false;
        descriptorSize_ = g_dx12.device->GetDescriptorHandleIncrementSize(
            D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        initialized = true;
        return true;
    }

    UINT Render(const Scene& scene, ID3D12Resource* smokeTexture,
                ID3D12Resource* bloodTexture, bool hdrTarget, bool msaaTarget,
                const std::function<bool(const ImpactParticle&)>& include = {}) {
        drawCallsThisFrame_ = 0;
        if (!initialized || !smokeTexture) return 0;
        if (!bloodTexture) bloodTexture = smokeTexture;
        UpdateTextureDescriptors(smokeTexture, bloodTexture);

        std::vector<const ImpactParticle*> smoke;
        std::vector<const ImpactParticle*> blood;
        std::vector<const ImpactParticle*> sparks;
        smoke.reserve(scene.impactParticles.size());
        blood.reserve(scene.impactParticles.size());
        sparks.reserve(scene.impactParticles.size());
        for (const ImpactParticle& particle : scene.impactParticles) {
            if (particle.life <= 0.0f || particle.size <= 0.0f) continue;
            if (include && !include(particle)) continue;
            if (particle.spark) sparks.push_back(&particle);
            else if (particle.blood) blood.push_back(&particle);
            else smoke.push_back(&particle);
        }
        const XMFLOAT3 cameraPosition = scene.camera.Position;
        auto backToFront = [&](const ImpactParticle* a, const ImpactParticle* b) {
            const float ax = a->position.x - cameraPosition.x;
            const float ay = a->position.y - cameraPosition.y;
            const float az = a->position.z - cameraPosition.z;
            const float bx = b->position.x - cameraPosition.x;
            const float by = b->position.y - cameraPosition.y;
            const float bz = b->position.z - cameraPosition.z;
            return ax * ax + ay * ay + az * az > bx * bx + by * by + bz * bz;
        };
        std::sort(smoke.begin(), smoke.end(), backToFront);
        std::sort(blood.begin(), blood.end(), backToFront);

        const UINT frame = g_dx12.frameIndex % FRAME_COUNT;
        const UINT frameBase = frame * MaxParticles;
        UINT cursor = 0;
        const UINT smokeStart = cursor;
        cursor = UploadGroup(smoke, 0, frameBase, cursor);
        const UINT bloodStart = cursor;
        cursor = UploadGroup(blood, 1, frameBase, cursor);
        const UINT sparkStart = cursor;
        cursor = UploadGroup(sparks, 2, frameBase, cursor);

        const XMMATRIX view = scene.GetViewMatrix();
        const XMMATRIX inverseView = XMMatrixTranspose(view);
        ImpactParticleFrameDX12 frameData = {};
        frameData.viewProjection = XMMatrixTranspose(
            view * scene.GetProjectionMatrix());
        XMStoreFloat3(&frameData.cameraRight,
            XMVector3Normalize(XMVectorSetW(inverseView.r[0], 0.0f)));
        XMStoreFloat3(&frameData.cameraUp,
            XMVector3Normalize(XMVectorSetW(inverseView.r[1], 0.0f)));
        // Smoke is translucent rather than emissive. Retain a small night floor
        // so its silhouette remains readable, while daylight keeps the exact
        // authored tint. Sparks stay emissive and blood keeps its own colour.
        frameData.smokeIllumination = (std::min)(
            1.0f, 0.08f + scene.ambientLightingIntensity * 2.2f);
        frames_.CopyData(frame, frameData);

        ID3D12GraphicsCommandList* commandList = g_dx12.commandList.Get();
        commandList->SetGraphicsRootSignature(rootSignature_.Get());
        ID3D12DescriptorHeap* heaps[] = { textureHeap_.Get() };
        commandList->SetDescriptorHeaps(1, heaps);
        commandList->SetGraphicsRootConstantBufferView(
            0, frames_.GetGPUAddress(frame));
        commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        const UINT target = hdrTarget ? 1u : (msaaTarget ? 2u : 0u);
        auto draw = [&](UINT start, UINT count, UINT textureIndex, bool additive) {
            if (!count) return;
            commandList->SetPipelineState(
                (additive ? additivePipelines_[target] : transparentPipelines_[target]).Get());
            commandList->SetGraphicsRootShaderResourceView(
                1, instances_.GetGPUAddress(frameBase + start));
            D3D12_GPU_DESCRIPTOR_HANDLE texture =
                textureHeap_->GetGPUDescriptorHandleForHeapStart();
            texture.ptr += static_cast<SIZE_T>(textureIndex) * descriptorSize_;
            commandList->SetGraphicsRootDescriptorTable(2, texture);
            commandList->DrawInstanced(6, count, 0, 0);
            ++drawCallsThisFrame_;
        };
        draw(smokeStart, bloodStart - smokeStart, 0, false);
        draw(bloodStart, sparkStart - bloodStart, 1, false);
        draw(sparkStart, cursor - sparkStart, 0, true);
        return drawCallsThisFrame_;
    }

    UINT DrawCallsThisFrame() const { return drawCallsThisFrame_; }

private:
    ComPtr<ID3D12RootSignature> rootSignature_;
    std::array<ComPtr<ID3D12PipelineState>, 3> transparentPipelines_;
    std::array<ComPtr<ID3D12PipelineState>, 3> additivePipelines_;
    ComPtr<ID3D12DescriptorHeap> textureHeap_;
    UploadBuffer<ImpactParticleInstanceDX12> instances_;
    UploadBuffer<ImpactParticleFrameDX12> frames_;
    UINT descriptorSize_ = 0;
    UINT drawCallsThisFrame_ = 0;
    ID3D12Resource* cachedSmoke_ = nullptr;
    ID3D12Resource* cachedBlood_ = nullptr;

    bool CreatePipelines(ID3DBlob* vs, ID3DBlob* ps, ID3DBlob* hdrPs) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = rootSignature_.Get();
        desc.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
        desc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.RasterizerState.DepthClipEnable = TRUE;
        desc.BlendState.RenderTarget[0].BlendEnable = TRUE;
        desc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_SRC_ALPHA;
        desc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        desc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
        desc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
        desc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        desc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
            D3D12_COLOR_WRITE_ENABLE_ALL;
        desc.DepthStencilState.DepthEnable = TRUE;
        desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        desc.SampleMask = UINT_MAX;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        desc.SampleDesc.Count = 1;
        if (FAILED(g_dx12.device->CreateGraphicsPipelineState(
                &desc, IID_PPV_ARGS(&transparentPipelines_[0])))) return false;
        desc.PS = { hdrPs->GetBufferPointer(), hdrPs->GetBufferSize() };
        desc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
        if (FAILED(g_dx12.device->CreateGraphicsPipelineState(
                &desc, IID_PPV_ARGS(&transparentPipelines_[1])))) return false;
        desc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
        desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = MSAADX12::SampleCount;
        desc.RasterizerState.MultisampleEnable = TRUE;
        if (FAILED(g_dx12.device->CreateGraphicsPipelineState(
                &desc, IID_PPV_ARGS(&transparentPipelines_[2])))) return false;

        desc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ONE;
        desc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ZERO;
        desc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ONE;
        for (UINT target = 0; target < 3; ++target) {
            if (target == 0) {
                desc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
                desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
                desc.SampleDesc.Count = 1;
                desc.RasterizerState.MultisampleEnable = FALSE;
            } else if (target == 1) {
                desc.PS = { hdrPs->GetBufferPointer(), hdrPs->GetBufferSize() };
                desc.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
                desc.SampleDesc.Count = 1;
                desc.RasterizerState.MultisampleEnable = FALSE;
            } else {
                desc.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
                desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
                desc.SampleDesc.Count = MSAADX12::SampleCount;
                desc.RasterizerState.MultisampleEnable = TRUE;
            }
            if (FAILED(g_dx12.device->CreateGraphicsPipelineState(
                    &desc, IID_PPV_ARGS(&additivePipelines_[target])))) return false;
        }
        return true;
    }

    UINT UploadGroup(const std::vector<const ImpactParticle*>& group, UINT kind,
                     UINT frameBase, UINT cursor) {
        for (const ImpactParticle* source : group) {
            if (cursor >= MaxParticles) break;
            const float fade = (std::max)(0.0f,
                source->life / (std::max)(source->maxLife, 0.001f));
            const float age = 1.0f - fade;
            const float fadeIn = age < 0.15f ? age / 0.15f : 1.0f;
            const float fadeOut = fade < 0.4f ? fade / 0.4f : 1.0f;
            ImpactParticleInstanceDX12 instance = {};
            instance.position = source->position;
            instance.velocity = source->velocity;
            instance.size = source->size;
            instance.kind = kind;
            instance.opacity = kind == 2 ? fade : fadeIn * fadeOut *
                (kind == 1 ? 0.36f : 0.85f);
            instance.color = source->color;
            if (kind == 0) {
                const float brighten = 1.0f + 2.0f * age;
                instance.color.x = (std::min)(1.0f, instance.color.x * brighten);
                instance.color.y = (std::min)(1.0f, instance.color.y * brighten);
                instance.color.z = (std::min)(1.0f, instance.color.z * brighten);
            } else if (kind == 2) {
                const float brightness = 0.6f + 0.4f * fade;
                instance.color.x *= brightness;
                instance.color.y *= brightness;
                instance.color.z *= brightness;
            }
            if (instance.opacity > 0.01f)
                instances_.CopyData(frameBase + cursor++, instance);
        }
        return cursor;
    }

    void UpdateTextureDescriptors(ID3D12Resource* smoke, ID3D12Resource* blood) {
        if (smoke == cachedSmoke_ && blood == cachedBlood_) return;
        D3D12_CPU_DESCRIPTOR_HANDLE handle =
            textureHeap_->GetCPUDescriptorHandleForHeapStart();
        g_dx12.device->CreateShaderResourceView(smoke, nullptr, handle);
        handle.ptr += descriptorSize_;
        g_dx12.device->CreateShaderResourceView(blood, nullptr, handle);
        cachedSmoke_ = smoke;
        cachedBlood_ = blood;
    }
};

#endif
