// Depth/Shadow Map Vertex Shader - DX11/DX12 HLSL

cbuffer MatrixBuffer : register(b0) {
    matrix model;
    matrix view;
    matrix projection;
    matrix lightSpaceMatrix;
};

struct VS_INPUT {
    float3 position : POSITION;
    float2 uv : TEXCOORD0;
};

cbuffer SkinningBuffer : register(b1) { uint skinningEnabled; };
StructuredBuffer<float4x4> bonePalette : register(t12);
struct SkinVtx { uint4 boneIndex; float4 boneWeight; };
StructuredBuffer<SkinVtx> skinData : register(t13);
cbuffer InstanceSettings : register(b7) { uint instancingEnabled; };
struct MeshInstanceData {
    float4x4 model;
    float modelMaxScale;
    float3 padding;
};
StructuredBuffer<MeshInstanceData> meshInstances : register(t14);

struct VS_OUTPUT {
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

VS_OUTPUT main(VS_INPUT input, uint vertexID : SV_VertexID,
               uint instanceID : SV_InstanceID) {
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
    float4x4 drawModel = instancingEnabled
        ? meshInstances[instanceID].model : model;
    float4 worldPos = mul(float4(position, 1.0), drawModel);
    output.position = mul(worldPos, lightSpaceMatrix);
    output.uv = input.uv;
    
    return output;
}

