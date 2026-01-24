// Point Light Depth Vertex Shader - passes world position for linear depth calculation

cbuffer MatrixBuffer : register(b0) {
    matrix model;
    matrix view;
    matrix projection;
    matrix lightSpaceMatrix;
};

struct VS_INPUT {
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 texCoord : TEXCOORD;
};

struct VS_OUTPUT {
    float4 position : SV_POSITION;
    float3 worldPos : TEXCOORD0;
};

VS_OUTPUT main(VS_INPUT input) {
    VS_OUTPUT output;
    
    float4 worldPos = mul(float4(input.position, 1.0), model);
    output.worldPos = worldPos.xyz;
    output.position = mul(worldPos, lightSpaceMatrix);
    
    return output;
}

