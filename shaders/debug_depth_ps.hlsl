// Debug Depth Visualization Pixel Shader - DX11 HLSL

cbuffer DepthBuffer : register(b0) {
    float nearPlane;
    float farPlane;
    float2 padding;
};

Texture2D depthMap : register(t0);
SamplerState depthSampler : register(s0);

struct PS_INPUT {
    float4 position : SV_POSITION;
    float2 texCoord : TEXCOORD0;
};

float LinearizeDepth(float depth) {
    float z = depth;
    return (2.0 * nearPlane * farPlane) / (farPlane + nearPlane - z * (farPlane - nearPlane));
}

float4 main(PS_INPUT input) : SV_TARGET {
    float depthValue = depthMap.Sample(depthSampler, input.texCoord).r;
    float linearDepth = LinearizeDepth(depthValue) / farPlane;
    return float4(linearDepth, linearDepth, linearDepth, 1.0);
}

