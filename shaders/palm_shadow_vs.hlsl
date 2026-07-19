#include "palm_wind.hlsli"

cbuffer MatrixBuffer : register(b0) {
    matrix model;
    matrix view;
    matrix projection;
    matrix lightSpaceMatrix;
    matrix modelView;
    matrix modelViewProjection;
    matrix previousViewProjection;
    float4 palmWind;
    float4 palmPrimary;
    float4 palmSecondary;
    float4 palmPreviousPrimary;
    float4 palmPreviousSecondary;
    float4 palmParams;
    float4 palmRoot;
};

struct VSInput {
    float3 position : POSITION;
    float2 texcoord : TEXCOORD0;
};

struct VSOutput {
    float4 position : SV_POSITION;
    float2 texcoord : TEXCOORD0;
};

VSOutput main(VSInput input) {
    VSOutput output;
    float3 position = ApplyPalmWindPosition(
        input.position, palmRoot, palmWind, palmPrimary, palmSecondary, palmParams);
    const float4 worldPosition = mul(float4(position, 1.0), model);
    output.position = mul(worldPosition, lightSpaceMatrix);
    output.texcoord = input.texcoord;
    return output;
}
