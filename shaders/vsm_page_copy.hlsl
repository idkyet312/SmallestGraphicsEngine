// Copies one resident page of the VSM static cache into this frame's atlas
// (VirtualShadowMapDX12::Render). D3D12 only copies depth as a whole
// subresource, which moved all 16 pages when the default budget keeps 3
// resident. Drawn once per resident page; the viewport/scissor select the
// page, and both atlases share the slot layout, so the pixel maps 1:1.

Texture2D<float> staticCache : register(t0);

float4 VSMain(uint id : SV_VertexID) : SV_Position {
    const float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float PSMain(float4 position : SV_Position) : SV_Depth {
    return staticCache[int2(position.xy)];
}
