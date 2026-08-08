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

cbuffer MeshDrawBuffer : register(b6) {
    uint vertexCount;
    uint indexCount;
    uint indexed;
    uint firstMeshlet;
    uint meshletCount;
    uint occlusionEnabled;
    uint screenWidth;
    uint screenHeight;
    uint skinningEnabled;   // 0 static, 1 skinned, 2 skinned view model (both skin)
    uint occlusionMipCount;
    float modelMaxScale;
    uint instanceCount;
    uint instancingEnabled;
};

ByteAddressBuffer vertexData : register(t6);
struct MeshletDesc {
    uint vertexOffset;
    uint vertexCount;
    uint triangleOffset;
    uint triangleCount;
};
StructuredBuffer<MeshletDesc> meshlets : register(t7);
StructuredBuffer<uint> meshletVertexIndices : register(t10);
StructuredBuffer<uint> meshletTriangles : register(t11);

// Skinning: bone palette (global*offset per bone) and per-vertex weights, both
// parallel to the vertex stream. Bound only for skinned draws.
StructuredBuffer<float4x4> bonePalette : register(t12);
struct SkinVtx { uint4 boneIndex; float4 boneWeight; };
StructuredBuffer<SkinVtx> skinData : register(t13);
// Previous-frame bone palette for motion-vector skinning.
StructuredBuffer<float4x4> previousBonePalette : register(t20);
struct MeshInstanceData {
    float4x4 model;
    float modelMaxScale;
    float3 padding;
};
StructuredBuffer<MeshInstanceData> meshInstances : register(t14);

struct Vertex {
    float3 position : POSITION;
    float3 normal : TEXCOORD0;
    float2 uv : TEXCOORD1;
    float4 tangent : TEXCOORD2;
};

struct OutVertex {
    float4 position : SV_Position;
    float3 fragPos : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 texCoord : TEXCOORD2;
    float4 tangent : TEXCOORD3;
    float4 fragPosLightSpace : TEXCOORD4;
    float4 currentClip : TEXCOORD5;
    float4 previousClip : TEXCOORD6;
};

float LoadF(uint address) { return asfloat(vertexData.Load(address)); }

Vertex LoadVertex(uint i) {
    uint b = i * 48;
    Vertex v;
    v.position = float3(LoadF(b), LoadF(b+4), LoadF(b+8));
    v.normal = float3(LoadF(b+12), LoadF(b+16), LoadF(b+20));
    v.uv = float2(LoadF(b+24), LoadF(b+28));
    v.tangent = float4(LoadF(b+32), LoadF(b+36), LoadF(b+40), LoadF(b+44));
    return v;
}

struct MeshPayload { uint2 workItems[32]; };

[outputtopology("triangle")]
[numthreads(128, 1, 1)]
void MSMain(uint3 id : SV_GroupThreadID,
            uint3 groupID : SV_GroupID,
            in payload MeshPayload payloadData,
            out vertices OutVertex verts[64],
            out indices uint3 tris[124]) {
    uint2 workItem = payloadData.workItems[groupID.x];
    MeshletDesc meshlet = meshlets[workItem.x];
    float4x4 drawModel = instancingEnabled
        ? meshInstances[workItem.y].model : model;
    SetMeshOutputCounts(meshlet.vertexCount, meshlet.triangleCount);

    if (id.x < meshlet.vertexCount) {
        uint sourceVertex = meshletVertexIndices[meshlet.vertexOffset + id.x];
        Vertex v = LoadVertex(sourceVertex);
        float3 skinnedPos = v.position;
        float3 prevLocal = v.position;
        if (skinningEnabled) {
            SkinVtx s = skinData[sourceVertex];
            float4x4 skinMat =
                s.boneWeight.x * bonePalette[s.boneIndex.x] +
                s.boneWeight.y * bonePalette[s.boneIndex.y] +
                s.boneWeight.z * bonePalette[s.boneIndex.z] +
                s.boneWeight.w * bonePalette[s.boneIndex.w];
            skinnedPos = mul(float4(v.position, 1), skinMat).xyz;
            v.normal = mul(v.normal, (float3x3)skinMat);
            v.tangent.xyz = mul(v.tangent.xyz, (float3x3)skinMat);
            float4x4 prevSkin =
                s.boneWeight.x * previousBonePalette[s.boneIndex.x] +
                s.boneWeight.y * previousBonePalette[s.boneIndex.y] +
                s.boneWeight.z * previousBonePalette[s.boneIndex.z] +
                s.boneWeight.w * previousBonePalette[s.boneIndex.w];
            prevLocal = mul(float4(prevLocal, 1), prevSkin).xyz;
        }
        float4 world = mul(float4(skinnedPos, 1), drawModel);
        float4 viewPosition = mul(world, view);
        // Keep the clip position in a local: reading back out of the vertex
        // output array (verts[id.x].position) makes the DXIL validator reject
        // MSMain outright ("parameter is not permitted, it should be inlined").
        float4 clipPosition = mul(viewPosition, projection);
        verts[id.x].position = clipPosition;
        verts[id.x].fragPos = world.xyz;
        verts[id.x].normal = normalize(mul(v.normal, (float3x3)drawModel));
        verts[id.x].texCoord = v.uv;
        verts[id.x].tangent = float4(normalize(mul(v.tangent.xyz, (float3x3)drawModel)), v.tangent.w);
        verts[id.x].fragPosLightSpace = mul(world, lightSpaceMatrix);
        verts[id.x].currentClip = clipPosition;
        float4 prevWorld = mul(float4(prevLocal, 1), previousModel);
        verts[id.x].previousClip = mul(prevWorld, previousViewProjection);
    }

    if (id.x < meshlet.triangleCount) {
        uint packed = meshletTriangles[meshlet.triangleOffset + id.x];
        tris[id.x] = uint3(packed & 0xff, (packed >> 8) & 0xff, (packed >> 16) & 0xff);
    }
}
