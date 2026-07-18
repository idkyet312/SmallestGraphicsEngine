// Visibility Buffer - Pixel Shader
// Writes full-width instance and primitive IDs into R32G32_UINT.
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

uint2 main(PS_INPUT input) : SV_Target0 {
    // Zero is reserved for background so clears are exact and portable.
    return uint2(drawCallID + 1u, input.primitiveID);
}
