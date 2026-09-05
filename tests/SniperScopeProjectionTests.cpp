// Execute the lens shader's actual coordinate calculation on WARP and compare
// it with an independent DirectX camera projection. No physical GPU is needed.
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <DirectXMath.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include "../src/render/dx12/SniperScopeOptics.h"

using namespace DirectX;
using Microsoft::WRL::ComPtr;

static void Require(HRESULT hr) {
    if (FAILED(hr)) throw std::runtime_error("D3D failure: " + std::to_string(hr));
}

int main() try {
    std::ifstream file(std::string(SGE_SOURCE_DIR) +
                       "/shaders/clustered_dx12_ps.hlsl");
    std::stringstream text;
    text << file.rdbuf();
    const std::string source = text.str();
    const size_t begin = source.find("const float3 rayView =", source.find(
        "if (isSniperGlass && useTexture > 0.5 && sniperScopeFocalY > 0.0)"));
    const size_t end = source.find("#ifdef SGE_TERRAIN_PBR", begin);
    if (begin == std::string::npos || end == std::string::npos)
        throw std::runtime_error("Cannot find the production scope projection");
    const std::string shader = R"(
cbuffer Params : register(b0) {
    matrix view;
    matrix projection;
    float3 fragmentPosition; float padding;
    float sniperScopeFocalX; float sniperScopeFocalY;
    float sniperScopeUVRotation; float sniperLensHalfTangent;
    float2 sniperAimTangent; float2 aimPadding;
};
RWStructuredBuffer<float2> result : register(u0);
[numthreads(1, 1, 1)] void main() {
    struct LensInput { float3 fragPos; };
    LensInput input; input.fragPos = fragmentPosition;
)" + source.substr(begin, end - begin) + "\nresult[0] = scopeUV;\n}";
    ComPtr<ID3DBlob> code, errors;
    const HRESULT compiled = D3DCompile(shader.data(), shader.size(), nullptr,
        nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3,
        0, &code, &errors);
    if (FAILED(compiled) && errors)
        std::cerr << static_cast<const char*>(errors->GetBufferPointer());
    Require(compiled);

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    Require(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context));
    ComPtr<ID3D11ComputeShader> compute;
    Require(device->CreateComputeShader(code->GetBufferPointer(),
        code->GetBufferSize(), nullptr, &compute));

    struct Constants {
        XMFLOAT4X4 view, projection;
        XMFLOAT3 fragmentPosition; float padding = 0;
        float focalX, focalY, rotation, lensHalfTangent = 0;
        XMFLOAT2 aimTangent{}, aimPadding{};
    } constants{};
    static_assert(sizeof(Constants) == 176, "Match the test HLSL cbuffer");
    D3D11_BUFFER_DESC desc{};
    desc.ByteWidth = sizeof(Constants);
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    ComPtr<ID3D11Buffer> params;
    Require(device->CreateBuffer(&desc, nullptr, &params));
    desc = {};
    desc.ByteWidth = sizeof(XMFLOAT2);
    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    desc.StructureByteStride = sizeof(XMFLOAT2);
    ComPtr<ID3D11Buffer> output, readback;
    Require(device->CreateBuffer(&desc, nullptr, &output));
    ComPtr<ID3D11UnorderedAccessView> uav;
    Require(device->CreateUnorderedAccessView(output.Get(), nullptr, &uav));
    desc.BindFlags = 0;
    desc.MiscFlags = 0;
    desc.StructureByteStride = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    Require(device->CreateBuffer(&desc, nullptr, &readback));
    context->CSSetShader(compute.Get(), nullptr, 0);
    ID3D11Buffer* cb = params.Get();
    ID3D11UnorderedAccessView* target = uav.Get();
    context->CSSetConstantBuffers(0, 1, &cb);
    context->CSSetUnorderedAccessViews(0, 1, &target, nullptr);

    // Two distinct cameras, which is the whole point of the test.
    //
    // `projection` is the MAIN camera the lens fragment is rasterized with, and
    // it is what the shader's cbuffer carries. `scopeProjection` is the narrow
    // secondary camera that actually rendered the scope target. Using one
    // matrix for both roles cannot detect a scope stuck at 1x: sampling an
    // image at the focal length it was rendered with reproduces the original
    // framing exactly, so the magnification cancels and every assertion still
    // passes. The ratio between these two focal lengths IS the magnification.
    const XMMATRIX projection = XMMatrixPerspectiveFovLH(
        XMConvertToRadians(60.0f), 1.0f, 0.01f, 500.0f);
    // Scene::sniperScopeFOV.
    const XMMATRIX scopeProjection = XMMatrixPerspectiveFovLH(
        XMConvertToRadians(6.0f), 1.0f, 0.01f, 500.0f);
    XMStoreFloat4x4(&constants.projection, XMMatrixTranspose(projection));
    // The scope camera's focal lengths, as ForwardRenderer supplies them from
    // Scene::SniperScopeFocalLengths().
    constants.focalX = constants.focalY = XMVectorGetX(scopeProjection.r[0]);
    unsigned checks = 0;
    for (float yaw : { -90.0f, 0.0f, 37.0f })
    for (float pitch : { -60.0f, 0.0f, 60.0f })
    for (float rollDegrees : { -74.0f, 0.0f, 74.0f }) {
        const float y = XMConvertToRadians(yaw);
        const float p = XMConvertToRadians(pitch);
        const XMVECTOR front = XMVectorSet(std::cos(y) * std::cos(p),
            std::sin(p), std::sin(y) * std::cos(p), 0);
        const XMVECTOR eye = XMVectorSet(12.035f, 3.72f, -9.04f, 1);
        const XMVECTOR up = XMVectorSet(0, 1, 0, 0);
        const XMMATRIX view = XMMatrixLookAtLH(eye, eye + front, up);
        const XMMATRIX inverseView = XMMatrixInverse(nullptr, view);
        const float roll = XMConvertToRadians(rollDegrees);
        const XMVECTOR rolledUp = XMVector3TransformNormal(up,
            XMMatrixRotationAxis(front, roll));
        const XMMATRIX scopeView = XMMatrixLookAtLH(eye, eye + front, rolledUp);
        XMStoreFloat4x4(&constants.view, XMMatrixTranspose(view));
        // ForwardRenderer supplies image rotation minus camera roll. The
        // shader's UV rotation must agree with the independently rolled camera.
        constants.rotation = -roll;
        for (const XMFLOAT2 offset : { XMFLOAT2(0, 0), XMFLOAT2(.04f, 0),
                XMFLOAT2(-.04f, 0), XMFLOAT2(0, .04f), XMFLOAT2(0, -.04f),
                XMFLOAT2(.03f, -.02f) }) {
            const XMVECTOR world = XMVector3TransformCoord(
                XMVectorSet(offset.x, offset.y, 1, 1), inverseView);
            XMStoreFloat3(&constants.fragmentPosition, world);
            // The lens must show what the SCOPE camera framed, so the expected
            // UV comes from the scope's own narrow projection. Projecting
            // through the main `projection` here instead is exactly the 1x bug:
            // it asserts the lens is a plain window onto the world at true
            // angular size, which is what the scope looked like before the fix.
            const XMVECTOR ndc =
                XMVector3TransformCoord(world, scopeView * scopeProjection);
            const XMFLOAT2 expected(.5f + .5f * XMVectorGetX(ndc),
                                    .5f - .5f * XMVectorGetY(ndc));
            context->UpdateSubresource(params.Get(), 0, nullptr, &constants, 0, 0);
            context->Dispatch(1, 1, 1);
            context->CopyResource(readback.Get(), output.Get());
            D3D11_MAPPED_SUBRESOURCE mapped{};
            Require(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped));
            const XMFLOAT2 actual = *static_cast<const XMFLOAT2*>(mapped.pData);
            context->Unmap(readback.Get(), 0);
            if (!std::isfinite(actual.x) || !std::isfinite(actual.y) ||
                std::fabs(actual.x - expected.x) > 0.0001f ||
                std::fabs(actual.y - expected.y) > 0.0001f) {
                std::cerr << "Scope mismatch: yaw=" << yaw << " pitch=" << pitch
                    << " roll=" << rollDegrees << " offset=" << offset.x << ','
                    << offset.y << " expected=" << expected.x << ',' << expected.y
                    << " actual=" << actual.x << ',' << actual.y << '\n';
                return 1;
            }
            ++checks;
        }
    }

    // The disc-relative mapping: the rim of the glass must land on the edge of
    // the scope target whatever the zoom, so the image fills the lens instead
    // of clamping. Before this, scopeUV was built from the scope camera's focal
    // length alone, which assumed the glass covered exactly the scope FOV --
    // measured on the real weapon it covered 4.6x that, and saturate() flattened
    // about 95% of the disc into one smeared edge texel.
    {
        // A lens disc spanning a 13.64 degree half-angle, as measured in game.
        const float lensHalfTangent = std::tan(XMConvertToRadians(13.64f));
        constants.lensHalfTangent = lensHalfTangent;
        constants.rotation = 0.0f;
        const XMVECTOR eye = XMVectorSet(12.035f, 3.72f, -9.04f, 1);
        const XMVECTOR front = XMVectorSet(1, 0, 0, 0);
        const XMVECTOR up = XMVectorSet(0, 1, 0, 0);
        const XMMATRIX view = XMMatrixLookAtLH(eye, eye + front, up);
        XMStoreFloat4x4(&constants.view, XMMatrixTranspose(view));
        const XMMATRIX inverseView = XMMatrixInverse(nullptr, view);

        // An off-centre firing ray must remain at the crosshair, including
        // while the viewmodel is still moving into its ADS position.
        for (const XMFLOAT2 aim : { XMFLOAT2{}, XMFLOAT2(0.08f, -0.04f) }) {
        constants.aimTangent = aim;
        // Walk the rim and the centre. Zoom must not move where either lands.
        for (const float scopeFOV : { 4.0f, 6.0f, 15.0f }) {
            const XMMATRIX zoomed = XMMatrixPerspectiveFovLH(
                XMConvertToRadians(scopeFOV), 1.0f, 0.01f, 500.0f);
            constants.focalX = constants.focalY = XMVectorGetX(zoomed.r[0]);
            struct Probe { float x, y; float wantU, wantV; const char* name; };
            for (const Probe probe : {
                    Probe{ 0.0f, 0.0f, 0.5f, 0.5f, "centre" },
                    Probe{ lensHalfTangent, 0.0f, 1.0f, 0.5f, "right rim" },
                    Probe{ -lensHalfTangent, 0.0f, 0.0f, 0.5f, "left rim" },
                    Probe{ 0.0f, lensHalfTangent, 0.5f, 0.0f, "top rim" },
                    Probe{ 0.0f, -lensHalfTangent, 0.5f, 1.0f, "bottom rim" } }) {
                const XMVECTOR world = XMVector3TransformCoord(
                    XMVectorSet(probe.x + aim.x, probe.y + aim.y, 1, 1), inverseView);
                XMStoreFloat3(&constants.fragmentPosition, world);
                context->UpdateSubresource(params.Get(), 0, nullptr,
                                           &constants, 0, 0);
                context->Dispatch(1, 1, 1);
                context->CopyResource(readback.Get(), output.Get());
                D3D11_MAPPED_SUBRESOURCE mapped{};
                Require(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0,
                                     &mapped));
                const XMFLOAT2 got = *static_cast<const XMFLOAT2*>(mapped.pData);
                context->Unmap(readback.Get(), 0);
                if (std::fabs(got.x - probe.wantU) > 0.002f ||
                    std::fabs(got.y - probe.wantV) > 0.002f) {
                    std::cerr << "Lens disc mapping wrong at " << probe.name
                              << " (scope FOV " << scopeFOV << " deg): expected "
                              << probe.wantU << ',' << probe.wantV << " got "
                              << got.x << ',' << got.y << '\n';
                    return 1;
                }
                ++checks;
            }
        }
        }
        constants.aimTangent = {};
        const float opticalFOV = SniperScopeOptics::FieldOfViewDegrees(lensHalfTangent);
        const float measuredZoom = lensHalfTangent /
            std::tan(XMConvertToRadians(opticalFOV) * 0.5f);
        if (std::fabs(measuredZoom - 6.0f) > 1e-4f)
            throw std::runtime_error("Aperture-derived scope must magnify 6x");
        std::cout << "Lens disc maps edge-to-edge at every zoom; optical magnification "
                  << measuredZoom << "x\n";
        constants.lensHalfTangent = 0.0f;
    }

    // State the magnification numerically, so a regression to 1x names itself
    // instead of surfacing as a coordinate mismatch. An off-axis fragment must
    // land proportionally further from the lens centre than it would in the
    // main view, by exactly the focal ratio (60 deg / 6 deg => ~11x). This
    // covers the focal-length fallback, used when the lens half-angle has not
    // been measured; the block above covers the disc-relative path.

    {
        const float mainFocal = XMVectorGetX(projection.r[0]);
        const float scopeFocal = XMVectorGetX(scopeProjection.r[0]);
        const float magnification = scopeFocal / mainFocal;
        // The disc-relative block above leaves its own zoom in the cbuffer.
        // Restore the focal-length mapping this check is written against.
        constants.focalX = constants.focalY = scopeFocal;
        constants.lensHalfTangent = 0.0f;
        const XMVECTOR eye = XMVectorSet(12.035f, 3.72f, -9.04f, 1);
        const XMVECTOR front = XMVectorSet(1, 0, 0, 0);
        const XMVECTOR up = XMVectorSet(0, 1, 0, 0);
        const XMMATRIX view = XMMatrixLookAtLH(eye, eye + front, up);
        XMStoreFloat4x4(&constants.view, XMMatrixTranspose(view));
        constants.rotation = 0.0f;
        const float offset = 0.03f;
        const XMVECTOR world = XMVector3TransformCoord(
            XMVectorSet(offset, 0, 1, 1), XMMatrixInverse(nullptr, view));
        XMStoreFloat3(&constants.fragmentPosition, world);
        context->UpdateSubresource(params.Get(), 0, nullptr, &constants, 0, 0);
        context->Dispatch(1, 1, 1);
        context->CopyResource(readback.Get(), output.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};
        Require(context->Map(readback.Get(), 0, D3D11_MAP_READ, 0, &mapped));
        const XMFLOAT2 actual = *static_cast<const XMFLOAT2*>(mapped.pData);
        context->Unmap(readback.Get(), 0);
        // Where the same fragment would land in an unmagnified 1x window.
        const float plainOffset = 0.5f * offset * mainFocal;
        const float scopeOffset = actual.x - 0.5f;
        const float observed = scopeOffset / plainOffset;
        if (!std::isfinite(observed) ||
            std::fabs(observed - magnification) > 0.001f) {
            std::cerr << "Scope magnification wrong: expected " << magnification
                      << "x, measured " << observed << "x"
                      << (std::fabs(observed - 1.0f) < 0.01f
                              ? " -- the lens is a 1x window; the scope focal"
                                " length is cancelling itself out"
                              : "")
                      << '\n';
            return 1;
        }
        std::cout << "Scope magnification: " << observed << "x\n";
    }

    std::cout << "SniperScopeProjectionTests passed: " << checks
              << " shader projections match LH cameras, including +/-74 degree roll\n";
    return 0;
} catch (const std::exception& e) {
    std::cerr << e.what() << '\n';
    return 1;
}
