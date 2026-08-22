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

#ifdef SGE_BINDLESS_MATERIALS
struct ImpactDecalData {
    float3 position;
    float radius;
    float3 normal;
    float strength;
};

cbuffer ImpactDecalsBuffer : register(b10) {
    int numImpactDecals;
    float impactCutoutsEnabled;
    float2 impactDecalPadding;
    ImpactDecalData impactDecals[64];
};
#endif

struct PS_INPUT {
    float4 position    : SV_POSITION;
    float2 texCoord    : TEXCOORD0;
#ifdef SGE_BINDLESS_MATERIALS
    float3 worldPos    : TEXCOORD1;
#endif
    uint   primitiveID : SV_PrimitiveID;
};

#ifdef SGE_BINDLESS_MATERIALS
bool ImpactDecalCutsSurface(float3 worldPos) {
    if (impactCutoutsEnabled < 0.5 || alphaCutout != 0u) return false;

    [loop]
    for (int i = 0; i < numImpactDecals; ++i) {
        ImpactDecalData decal = impactDecals[i];
        if (decal.strength <= 0.001) continue;
        const float3 offset = worldPos - decal.position;
        const float alongNormal = abs(dot(offset, decal.normal));
        const float acrossPlaneSq =
            max(0.0, dot(offset, offset) - alongNormal * alongNormal);
        const float holeRadius = decal.radius * 0.38;
        if (alongNormal <= decal.radius * 1.5 &&
            acrossPlaneSq <= holeRadius * holeRadius)
            return true;
    }
    return false;
}
#endif

uint2 main(PS_INPUT input) : SV_Target0 {
#ifdef SGE_BINDLESS_MATERIALS
    if (ImpactDecalCutsSurface(input.worldPos)) discard;
#endif
    // Zero is reserved for background so clears are exact and portable.
    return uint2(drawCallID + 1u, input.primitiveID);
}

uint2 mainAlpha(PS_INPUT input) : SV_Target0 {
#ifdef SGE_BINDLESS_MATERIALS
    if (ImpactDecalCutsSurface(input.worldPos)) discard;
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
