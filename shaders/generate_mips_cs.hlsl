// Generates one mip level from the previous level using a 2x2 box filter.
// Dispatched once per mip transition (SrcMipLevel -> SrcMipLevel + 1).

cbuffer MipConstants : register(b0) {
    uint2 dstSize;    // width/height of the mip level being written
    float2 texelSize; // 1 / srcSize, for sampling the midpoint between 4 source texels
};

Texture2D<float4> SrcMip : register(t0);
RWTexture2D<float4> DstMip : register(u0);
SamplerState LinearClamp : register(s0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (id.x >= dstSize.x || id.y >= dstSize.y) return;

    float2 uv = (id.xy + 0.5f) * texelSize * 2.0f;

    // Linear filtering at the midpoint UV averages the 4 backing source texels
    float4 color = SrcMip.SampleLevel(LinearClamp, uv, 0);
    DstMip[id.xy] = color;
}
