cbuffer MatrixBuffer : register(b0) {
    matrix model;
    matrix view;
    matrix projection;
    matrix lightSpaceMatrix;
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

struct MeshPayload { uint meshletIndices[32]; };

[outputtopology("triangle")]
[numthreads(128, 1, 1)]
void MSMain(uint3 id : SV_GroupThreadID,
            uint3 groupID : SV_GroupID,
            in payload MeshPayload payloadData,
            out vertices OutVertex verts[64],
            out indices uint3 tris[124]) {
    MeshletDesc meshlet = meshlets[payloadData.meshletIndices[groupID.x]];
    SetMeshOutputCounts(meshlet.vertexCount, meshlet.triangleCount);

    if (id.x < meshlet.vertexCount) {
        uint sourceVertex = meshletVertexIndices[meshlet.vertexOffset + id.x];
        Vertex v = LoadVertex(sourceVertex);
        float4 world = mul(float4(v.position, 1), model);
        float4 viewPosition = mul(world, view);
        verts[id.x].position = mul(viewPosition, projection);
        verts[id.x].fragPos = world.xyz;
        verts[id.x].normal = normalize(mul(v.normal, (float3x3)model));
        verts[id.x].texCoord = v.uv;
        verts[id.x].tangent = float4(normalize(mul(v.tangent.xyz, (float3x3)model)), v.tangent.w);
        verts[id.x].fragPosLightSpace = mul(world, lightSpaceMatrix);
    }

    if (id.x < meshlet.triangleCount) {
        uint packed = meshletTriangles[meshlet.triangleOffset + id.x];
        tris[id.x] = uint3(packed & 0xff, (packed >> 8) & 0xff, (packed >> 16) & 0xff);
    }
}
