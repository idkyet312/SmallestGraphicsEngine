// Visibility Buffer - Pixel Shader
// Writes full-width instance and primitive IDs into R32G32_UINT.
// The drawCallID is supplied via a root constant.

cbuffer VisBufferConstants : register(b1) {
    uint drawCallID;
    uint alphaCutout;
    uint alphaFromLuminance;
    uint vbPadding2;
};

Texture2D<float4> alphaTexture : register(t0);
SamplerState alphaSampler : register(s0);

struct PS_INPUT {
    float4 position    : SV_POSITION;
    float2 texCoord    : TEXCOORD0;
    uint   primitiveID : SV_PrimitiveID;
};

uint2 main(PS_INPUT input) : SV_Target0 {
    // Zero is reserved for background so clears are exact and portable.
    return uint2(drawCallID + 1u, input.primitiveID);
}

uint2 mainAlpha(PS_INPUT input) : SV_Target0 {
#ifdef SGE_BINDLESS_MATERIALS
    Texture2D<float4> bindlessAlphaTexture =
        ResourceDescriptorHeap[NonUniformResourceIndex(vbPadding2)];
    float4 sampleValue = bindlessAlphaTexture.Sample(alphaSampler, input.texCoord);
#else
    float4 sampleValue = alphaTexture.Sample(alphaSampler, input.texCoord);
#endif
    if (alphaFromLuminance != 0u)
        clip(max(sampleValue.r, max(sampleValue.g, sampleValue.b)) - 0.38);
    else
        clip(sampleValue.a - 0.20);
    return uint2(drawCallID + 1u, input.primitiveID);
}
