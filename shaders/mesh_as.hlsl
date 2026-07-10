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

cbuffer CameraBuffer : register(b2) {
    float3 viewPos;
    float cameraPadding;
};

struct MeshletBounds {
    float3 boundsMin;
    float padding0;
    float3 boundsMax;
    float padding1;
    float3 sphereCenter;
    float sphereRadius;
    float3 coneAxis;
    float coneCutoff;
};

StructuredBuffer<MeshletBounds> meshletBounds : register(t8);
Texture2D<float> previousDepth : register(t9);
struct MeshPayload { uint meshletIndices[32]; };

groupshared MeshPayload payloadData;
groupshared uint visibleCount;

bool IntersectsFrustum(MeshletBounds bounds) {
    float4 clipCorners[8];
    [unroll]
    for (uint i = 0; i < 8; ++i) {
        float3 p = float3(
            (i & 1) ? bounds.boundsMax.x : bounds.boundsMin.x,
            (i & 2) ? bounds.boundsMax.y : bounds.boundsMin.y,
            (i & 4) ? bounds.boundsMax.z : bounds.boundsMin.z);
        clipCorners[i] = mul(mul(mul(float4(p, 1), model), view), projection);
    }

    bool outsideLeft = true, outsideRight = true;
    bool outsideBottom = true, outsideTop = true;
    bool outsideNear = true, outsideFar = true;
    [unroll]
    for (uint i = 0; i < 8; ++i) {
        float4 c = clipCorners[i];
        outsideLeft   = outsideLeft   && c.x < -c.w;
        outsideRight  = outsideRight  && c.x >  c.w;
        outsideBottom = outsideBottom && c.y < -c.w;
        outsideTop    = outsideTop    && c.y >  c.w;
        outsideNear   = outsideNear   && c.z < 0.0;
        outsideFar    = outsideFar    && c.z > c.w;
    }
    return !(outsideLeft || outsideRight || outsideBottom ||
             outsideTop || outsideNear || outsideFar);
}

bool IsBackfacing(MeshletBounds bounds) {
    if (bounds.coneCutoff < 0.0) return false;
    float3 worldCenter = mul(float4(bounds.sphereCenter, 1), model).xyz;
    float3 axis = normalize(mul(bounds.coneAxis, (float3x3)model));
    float3 toCenter = worldCenter - viewPos;
    float3 sx = float3(model[0][0], model[0][1], model[0][2]);
    float3 sy = float3(model[1][0], model[1][1], model[1][2]);
    float3 sz = float3(model[2][0], model[2][1], model[2][2]);
    float radiusScale = max(length(sx), max(length(sy), length(sz)));
    return dot(toCenter, axis) >=
           bounds.coneCutoff * length(toCenter) + bounds.sphereRadius * radiusScale;
}

bool IsOccluded(MeshletBounds bounds) {
    if (!occlusionEnabled) return false;
    float2 uvMin = float2(1.0, 1.0);
    float2 uvMax = float2(0.0, 0.0);
    float nearestDepth = 1.0;
    [unroll]
    for (uint i = 0; i < 8; ++i) {
        float3 p = float3(
            (i & 1) ? bounds.boundsMax.x : bounds.boundsMin.x,
            (i & 2) ? bounds.boundsMax.y : bounds.boundsMin.y,
            (i & 4) ? bounds.boundsMax.z : bounds.boundsMin.z);
        float4 clip = mul(mul(mul(float4(p, 1), model), view), projection);
        if (clip.w <= 0.001) return false;
        float3 ndc = clip.xyz / clip.w;
        float2 uv = ndc.xy * float2(0.5, -0.5) + 0.5;
        uvMin = min(uvMin, uv);
        uvMax = max(uvMax, uv);
        nearestDepth = min(nearestDepth, ndc.z);
    }
    uvMin = saturate(uvMin);
    uvMax = saturate(uvMax);
    if (uvMin.x >= uvMax.x || uvMin.y >= uvMax.y) return false;

    // Conservative 3x3 test against previous-frame depth. Any background or
    // farther sample keeps the meshlet visible, reducing false rejection.
    float farthestOccluder = 0.0;
    [unroll]
    for (uint y = 0; y < 3; ++y) {
        [unroll]
        for (uint x = 0; x < 3; ++x) {
            float2 uv = lerp(uvMin, uvMax, float2(x, y) * 0.5);
            uint2 pixel = min(uint2(uv * float2(screenWidth, screenHeight)),
                              uint2(screenWidth - 1, screenHeight - 1));
            farthestOccluder = max(farthestOccluder, previousDepth.Load(int3(pixel, 0)));
        }
    }
    return farthestOccluder < nearestDepth - 0.01;
}

[numthreads(32, 1, 1)]
void ASMain(uint threadID : SV_GroupThreadID, uint3 groupID : SV_GroupID) {
    if (threadID == 0) visibleCount = 0;
    GroupMemoryBarrierWithGroupSync();

    uint localMeshlet = groupID.x * 32 + threadID;
    uint globalMeshlet = firstMeshlet + localMeshlet;

    if (globalMeshlet < meshletCount) {
        MeshletBounds bounds = meshletBounds[globalMeshlet];
        bool visible = IntersectsFrustum(bounds) &&
                       !IsBackfacing(bounds) &&
                       !IsOccluded(bounds);
        if (visible) {
            uint slot;
            InterlockedAdd(visibleCount, 1, slot);
            payloadData.meshletIndices[slot] = globalMeshlet;
        }
    }

    GroupMemoryBarrierWithGroupSync();
    DispatchMesh(visibleCount, 1, 1, payloadData);
}
