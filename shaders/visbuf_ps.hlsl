// Visibility Buffer - Pixel Shader
// Writes (drawCallID << 23) | SV_PrimitiveID into a R32_UINT render target.
// The drawCallID is supplied via a root constant.

cbuffer VisBufferConstants : register(b1) {
    uint drawCallID;
    uint vbPadding0;
    uint vbPadding1;
    uint vbPadding2;
};

struct PS_INPUT {
    float4 position    : SV_POSITION;
    uint   primitiveID : SV_PrimitiveID;
};

uint main(PS_INPUT input) : SV_Target0 {
    // Pack: upper 9 bits = drawCallID (max 512), lower 23 bits = triangleID (max ~8M)
    uint packed = ((drawCallID & 0x1FFu) << 23u) | (input.primitiveID & 0x7FFFFFu);
    return packed;
}
