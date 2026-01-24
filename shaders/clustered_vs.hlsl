// Clustered Forward Vertex Shader - DX11 HLSL

cbuffer MatrixBuffer : register(b0) {
    matrix model;
    matrix view;
    matrix projection;
    matrix lightSpaceMatrix;
};

struct VS_INPUT {
    float3 position : POSITION;
    float3 normal : NORMAL;
};

struct VS_OUTPUT {
    float4 position : SV_POSITION;
    float3 fragPos : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float4 fragPosLightSpace : TEXCOORD2;
};

VS_OUTPUT main(VS_INPUT input) {
    VS_OUTPUT output;
    
    float4 worldPos = mul(float4(input.position, 1.0), model);
    output.fragPos = worldPos.xyz;
    
    // Calculate normal in world space
    float3x3 normalMatrix = (float3x3)transpose((float3x3)model);
    // For proper normal transformation, we should use inverse transpose
    // Simplified version - works when model has uniform scale
    output.normal = normalize(mul(input.normal, (float3x3)model));
    
    output.fragPosLightSpace = mul(worldPos, lightSpaceMatrix);
    
    float4 viewPos = mul(worldPos, view);
    output.position = mul(viewPos, projection);
    
    return output;
}

