#ifndef RENDER_VIEW_DX12_H
#define RENDER_VIEW_DX12_H

#include "DX12Core.h"
#include "CameraDX12.h"
#include <DirectXMath.h>

using namespace DirectX;

// Which view a pass is rendering. Upload arenas and draw statistics are keyed
// on this, so a second camera pass in the same frame cannot overwrite the
// main view's slice or pollute its counters.
enum class RenderViewSlot : uint32_t {
    Main = 0,
    Scope = 1,
    Count = 2
};

// Everything a camera-dependent pass needs that is NOT shared between views.
//
// Before this existed, reusable passes read g_dx12.screenWidth/screenHeight,
// the global depth buffer, the global viewport and the swapchain RTV directly.
// That is correct for exactly one view per frame. Rendering the scope means
// the same code runs twice against different targets, so those implicit reads
// become explicit fields here.
//
// The main view fills this from the globals (see MainRenderView below), which
// keeps its behaviour identical to the pre-refactor path.
struct RenderViewDX12 {
    // --- Camera ---
    // A snapshot, not a reference. The scope copies the main camera and then
    // changes only its FOV and aspect, so the two must not alias.
    Camera camera{ XMFLOAT3(0.0f, 0.0f, 0.0f) };
    float verticalFOV = 60.0f;
    float nearPlane = 0.1f;
    float farPlane = 1000.0f;
    // Square for the scope, wide for the main view. Kept explicit rather than
    // derived from width/height so a pass cannot disagree with the projection
    // the shader was handed.
    float aspectRatio = 1.0f;

    // --- Target geometry ---
    UINT width = 0;
    UINT height = 0;
    D3D12_VIEWPORT viewport{};
    D3D12_RECT scissor{};

    // --- Temporal ---
    XMFLOAT2 temporalJitterPixels{ 0.0f, 0.0f };
    // Each view accumulates its own history. Sharing one previous-view-
    // projection between a wide view and a 15-degree scope would reproject
    // every pixel against the wrong matrix and smear the result.
    XMMATRIX previousViewProjection = XMMatrixIdentity();
    bool historyValid = false;

    // --- Targets ---
    ID3D12Resource* colorTarget = nullptr;
    ID3D12Resource* depthTarget = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE colorRTV{};
    D3D12_CPU_DESCRIPTOR_HANDLE depthDSV{};

    // --- Identity and policy ---
    RenderViewSlot slot = RenderViewSlot::Main;
    // The lens would otherwise render the rifle that carries it, and sample
    // the texture it is drawing into. This flag, not a scene-wide pass flag,
    // is what excludes first-person geometry.
    bool drawViewmodel = true;
    // The HZB is built for the main projection. A 15-degree scope frustum is
    // not a subset of it in screen space, so scope occlusion culling against
    // the main HZB would cull visible geometry.
    bool captureHZB = true;
    bool useHZBOcclusion = true;
    // Diagnostic overlays, debug view modes and validation counters belong to
    // the main view; the scope must not write them.
    bool allowDiagnostics = true;

    bool IsScope() const { return slot == RenderViewSlot::Scope; }

    void SetViewportFromSize() {
        viewport = { 0.0f, 0.0f, static_cast<float>(width),
                     static_cast<float>(height), 0.0f, 1.0f };
        scissor = { 0, 0, static_cast<LONG>(width),
                    static_cast<LONG>(height) };
        aspectRatio = height > 0
            ? static_cast<float>(width) / static_cast<float>(height) : 1.0f;
    }

    XMMATRIX ViewMatrix() const { return camera.GetViewMatrix(); }

    XMMATRIX ProjectionMatrix() const {
        return XMMatrixPerspectiveFovLH(
            XMConvertToRadians(verticalFOV), aspectRatio, nearPlane, farPlane);
    }

    // Jittered projection for temporal accumulation. Matches the main path's
    // convention: X positive, Y negated, both in NDC units of one pixel.
    XMMATRIX JitteredProjectionMatrix() const {
        XMMATRIX projection = ProjectionMatrix();
        if (width == 0 || height == 0) return projection;
        if (temporalJitterPixels.x == 0.0f && temporalJitterPixels.y == 0.0f)
            return projection;
        const float jitterX =
            2.0f * temporalJitterPixels.x / static_cast<float>(width);
        const float jitterY =
            -2.0f * temporalJitterPixels.y / static_cast<float>(height);
        projection.r[2] = XMVectorSetX(projection.r[2],
            XMVectorGetX(projection.r[2]) + jitterX);
        projection.r[2] = XMVectorSetY(projection.r[2],
            XMVectorGetY(projection.r[2]) + jitterY);
        return projection;
    }
};

#endif
