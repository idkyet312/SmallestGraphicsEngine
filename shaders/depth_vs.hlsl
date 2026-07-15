// Depth/Shadow Map Vertex Shader - DX11/DX12 HLSL

cbuffer MatrixBuffer : register(b0) {
    matrix model;
    matrix view;
    matrix projection;
    matrix lightSpaceMatrix;
};

struct VS_INPUT {
    float3 position : POSITION;
};

cbuffer SkinningBuffer : register(b1) { uint skinningEnabled; };
StructuredBuffer<float4x4> bonePalette : register(t12);
struct SkinVtx { uint4 boneIndex; float4 boneWeight; };
StructuredBuffer<SkinVtx> skinData : register(t13);

struct VS_OUTPUT {
    float4 position : SV_POSITION;
};

VS_OUTPUT main(VS_INPUT input, uint vertexID : SV_VertexID) {
    VS_OUTPUT output;
    
    float3 position = input.position;
    if (skinningEnabled) {
        SkinVtx s = skinData[vertexID];
        float4x4 skinMat =
            s.boneWeight.x * bonePalette[s.boneIndex.x] +
            s.boneWeight.y * bonePalette[s.boneIndex.y] +
            s.boneWeight.z * bonePalette[s.boneIndex.z] +
            s.boneWeight.w * bonePalette[s.boneIndex.w];
        position = mul(float4(position, 1.0), skinMat).xyz;
    }
    float4 worldPos = mul(float4(position, 1.0), model);
    output.position = mul(worldPos, lightSpaceMatrix);
    
    return output;
}

