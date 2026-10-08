#pragma once

#include "DX12Core.h"
#include "ShaderCacheDX12.h"
#include <chrono>
#include <fstream>
#include <future>
#include <iostream>
#include <string>

// One extra entry point of the enhanced resolve source, built against that
// tier's resolve root signature so it binds exactly like the resolve. Compiles
// off-thread (DXC); a new root signature (pipeline rebuild) restarts it.
class ResolveEntryPipelineDX12 {
public:
    ResolveEntryPipelineDX12(const wchar_t* entryPoint, const char* label)
        : entry(entryPoint), name(label) {}

    bool Ensure(const std::string& source, bool bindless,
                ID3D12RootSignature* root) {
        auto& p = tiers[bindless ? 1 : 0];
        if (!root) return false;
        if (p.root.Get() != root) {
            if (p.pending.valid()) p.pending.wait();
            p = Tier{};
            p.root = root;
        }
        if (!p.attempted) {
            p.attempted = true;
            if (!ShaderCacheDX12::DxcAvailable()) {
                status = "dxcompiler.dll unavailable";
                return false;
            }
            status = "Compiling";
            p.pending = std::async(std::launch::async,
                [root = p.root, device = g_dx12.device, bindless, source,
                 entry = entry]() {
                Build result;
                ShaderCacheDX12::MarkBackgroundThread();
                ComPtr<ID3DBlob> shader;
                const std::wstring directory =
                    ShaderCacheDX12::ExecutableDirectory() + L"shaders";
                if (!ShaderCacheDX12::CompileCachedDXC(source,
                        L"visbuf_resolve_cs.hlsl", entry,
                        bindless ? L"cs_6_6" : L"cs_6_5", directory, &shader,
                        &result.errors)) {
                    if (result.errors.empty()) result.errors = "compile failed";
                    return result;
                }
                D3D12_COMPUTE_PIPELINE_STATE_DESC desc = {};
                desc.pRootSignature = root.Get();
                desc.CS = { shader->GetBufferPointer(), shader->GetBufferSize() };
                if (FAILED(device->CreateComputePipelineState(&desc,
                        IID_PPV_ARGS(&result.pso)))) {
                    result.pso.Reset();
                    result.errors = "PSO creation failed";
                }
                return result;
            });
        }
        if (p.pending.valid()) {
            if (p.pending.wait_for(std::chrono::seconds(0)) !=
                std::future_status::ready)
                return false;
            Build build = p.pending.get();
            if (!build.errors.empty()) {
                std::ofstream("resolve_entry_shader_error.log", std::ios::app)
                    << name << ": " << build.errors << "\n";
                std::cerr << name << ": " << build.errors << "\n";
            }
            p.pso = build.pso;
            status = p.pso ? "Ready" : "Shader build failed";
        }
        return p.pso != nullptr;
    }

    ID3D12PipelineState* PSO(bool bindless) const {
        return tiers[bindless ? 1 : 0].pso.Get();
    }
    const char* Status() const { return status.c_str(); }

private:
    struct Build {
        ComPtr<ID3D12PipelineState> pso;
        std::string errors;
    };
    struct Tier {
        bool attempted = false;
        ComPtr<ID3D12RootSignature> root;
        ComPtr<ID3D12PipelineState> pso;
        std::future<Build> pending;
    } tiers[2];
    const wchar_t* entry;
    const char* name;
    std::string status = "Not selected";
};
