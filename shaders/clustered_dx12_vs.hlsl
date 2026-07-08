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
    float4 tangent : TANGENT;
};

struct VS_OUTPUT {
    float4 position : SV_POSITION;
    float3 fragPos : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 texCoord : TEXCOORD2;
    float4 tangent : TEXCOORD3;
    float4 fragPosLightSpace : TEXCOORD4;
};

VS_OUTPUT main(VS_INPUT input) {
    VS_OUTPUT output;
    
    float4 worldPos = mul(float4(input.position, 1.0), model);
    output.fragPos = worldPos.xyz;
    
    // Transform normal to world space
    output.normal = normalize(mul(input.normal, (float3x3)model));
    output.tangent = float4(normalize(mul(input.tangent.xyz, (float3x3)model)), input.tangent.w);
    
    output.texCoord = input.texCoord;
    
    float4 viewPos = mul(worldPos, view);
    output.position = mul(viewPos, projection);
    output.fragPosLightSpace = mul(worldPos, lightSpaceMatrix);
    
    return output;
}

