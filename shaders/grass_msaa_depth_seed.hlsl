// Seeds the grass MSAA depth with the opaque scene depth (GrassMSAADX12::Begin).
//
// The grass layer used to start from a cleared depth, so blades behind
// terrain, rocks and buildings were rasterised and shaded at 4x MSAA and only
// thrown away in grass_msaa_composite_cs. Seeding with the depth the composite
// compares against, plus its 2e-5 tolerance, lets early-Z reject exactly the
// samples the composite would have dropped. Uncovered samples keep alpha 0, so
// the composite's coverage test is unchanged.

Texture2D<float> sceneDepth : register(t0);

float4 VSMain(uint id : SV_VertexID) : SV_Position {
    const float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
}

float PSMain(float4 position : SV_Position) : SV_Depth {
    return saturate(sceneDepth[int2(position.xy)] + 2e-5);
}
