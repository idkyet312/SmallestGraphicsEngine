// Clustered Forward Vertex Shader - DX12 Compatible

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
    matrix previousModel;
};

#include "palm_wind.hlsli"

// Skeletal skinning for the IA raster path, so skinned meshes still pose on
// hardware without mesh shaders. This flag gets its own register: b1 is the
// pixel shader's LightBuffer, and b6 is shared by grass, terrain, instancing
// and the mesh shader with a different struct each, so a flag read from either
// would be whatever the last pass happened to store there.
cbuffer SkinningToggle : register(b9) { uint skinningEnabled; };
StructuredBuffer<float4x4> bonePalette : register(t12);
struct SkinVtx { uint4 boneIndex; float4 boneWeight; };
StructuredBuffer<SkinVtx> skinData : register(t13);
StructuredBuffer<float4x4> previousBonePalette : register(t20);

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
    float4 currentClip : TEXCOORD5;
    float4 previousClip : TEXCOORD6;
};

float4x4 SkinVertex(SkinVtx s, StructuredBuffer<float4x4> palette) {
    return s.boneWeight.x * palette[s.boneIndex.x] +
           s.boneWeight.y * palette[s.boneIndex.y] +
           s.boneWeight.z * palette[s.boneIndex.z] +
           s.boneWeight.w * palette[s.boneIndex.w];
}

VS_OUTPUT main(VS_INPUT input, uint vertexID : SV_VertexID) {
    VS_OUTPUT output;

    float3 localPosition = input.position;
    float3 localNormal = input.normal;
    float3 localTangent = input.tangent.xyz;
    if (skinningEnabled) {
        SkinVtx s = skinData[vertexID];
        float4x4 skinMat = SkinVertex(s, bonePalette);
        localPosition = mul(float4(localPosition, 1.0), skinMat).xyz;
        localNormal = mul(localNormal, (float3x3)skinMat);
        localTangent = mul(localTangent, (float3x3)skinMat);
    }
    ApplyPalmWind(localPosition, localNormal, localTangent, palmRoot,
                  palmWind, palmPrimary, palmSecondary, palmParams);

    float4 worldPos = mul(float4(localPosition, 1.0), model);
    output.fragPos = worldPos.xyz;
    
    output.normal = normalize(mul(localNormal, (float3x3)model));
    output.tangent = float4(normalize(mul(localTangent, (float3x3)model)), input.tangent.w);
    
    output.texCoord = input.texCoord;
    
    float4 viewPos = mul(worldPos, view);
    output.position = mul(viewPos, projection);
    output.currentClip = output.position;
    output.fragPosLightSpace = mul(worldPos, lightSpaceMatrix);

    // Previous-frame clip for motion vectors: apply skinning with the
    // previous bone pose and palm wind with the previous palm state,
    // then transform by previousModel and previousViewProjection.
    float3 previousLocal = input.position;
    if (skinningEnabled) {
        SkinVtx s = skinData[vertexID];
        float4x4 prevSkin = SkinVertex(s, previousBonePalette);
        previousLocal = mul(float4(previousLocal, 1.0), prevSkin).xyz;
    }
    ApplyPalmWind(previousLocal, localNormal, localTangent, palmRoot,
                  palmWind, palmPreviousPrimary, palmPreviousSecondary,
                  palmParams);
    float4 prevWorld = mul(float4(previousLocal, 1.0), previousModel);
    output.previousClip = mul(prevWorld, previousViewProjection);
    
    return output;
}
