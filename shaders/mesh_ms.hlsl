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
    uint firstCorner;
};

ByteAddressBuffer vertexData : register(t6);
ByteAddressBuffer indexData : register(t7);

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

uint LoadIndex(uint i) { return indexData.Load(i * 4); }

[outputtopology("triangle")]
[numthreads(96, 1, 1)]
void MSMain(uint3 id : SV_GroupThreadID,
            uint3 groupID : SV_GroupID,
            out vertices OutVertex verts[96],
            out indices uint3 tris[32]) {
    uint cornerCount = indexed ? indexCount : vertexCount;
    uint groupStart = firstCorner + groupID.x * 96;
    uint remainingCorners = groupStart < cornerCount ? cornerCount - groupStart : 0;
    uint triCount = min(remainingCorners / 3, 32);
    uint outVertexCount = triCount * 3;
    SetMeshOutputCounts(outVertexCount, triCount);

    if (id.x < outVertexCount) {
        uint corner = groupStart + id.x;
        uint sourceVertex = indexed ? LoadIndex(corner) : corner;
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

    if (id.x < triCount) {
        uint base = id.x * 3;
        tris[id.x] = uint3(base, base + 1, base + 2);
    }
}
