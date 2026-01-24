// Clustered Forward Vertex Shader - DX12 Compatible

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
    float3 fragPos : TEXCOORD0;
    float3 normal : TEXCOORD1;
};

VS_OUTPUT main(VS_INPUT input) {
    VS_OUTPUT output;
    
    float4 worldPos = mul(float4(input.position, 1.0), model);
    output.fragPos = worldPos.xyz;
    
    // Transform normal to world space
    output.normal = normalize(mul(input.normal, (float3x3)model));
    
    float4 viewPos = mul(worldPos, view);
    output.position = mul(viewPos, projection);
    
    return output;
}

