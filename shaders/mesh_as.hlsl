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
    // Bit 0 enables HZB occlusion. Bit 1 disables cone backface rejection so
    // a runtime material override cannot be undone before rasterization.
    uint cullingFlags;
    uint screenWidth;
    uint screenHeight;
    // 0 = static, 1 = skinned (bounds are bind-pose, so only the coarse frustum
    // test is safe), 2 = skinned view model: no culling at all.
    //
    // The view model is drawn inches from the eye and posed far from its bind
    // pose, so its bind-pose meshlet bounds bear no relation to where the
    // geometry actually ends up. At that distance the frustum test rejects
    // meshlets that are plainly on screen and the character flickers or
    // disappears as the camera turns. It is a handful of meshlets that are
    // always in view, so the cheapest correct answer is to skip the tests.
    uint skinningEnabled;
    uint occlusionMipCount;
    float modelMaxScale;
    uint instanceCount;
    uint instancingEnabled;
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
struct MeshInstanceData {
    float4x4 model;
    float modelMaxScale;
    float3 padding;
};
StructuredBuffer<MeshInstanceData> meshInstances : register(t14);
struct MeshPayload { uint2 workItems[32]; };

groupshared MeshPayload payloadData;
groupshared uint visibleCount;

bool IntersectsFrustum(MeshletBounds bounds, float4x4 drawMVP, float drawScale) {
    float4 clipCenter = mul(float4(bounds.sphereCenter, 1), drawMVP);
    float radius = bounds.sphereRadius * drawScale;
    float radiusX = radius * abs(projection[0][0]);
    float radiusY = radius * abs(projection[1][1]);
    // Inflate depth radius because perspective changes both clip z and w.
    float radiusZ = radius * (abs(projection[2][2]) + abs(projection[2][3]));
    return clipCenter.x + radiusX >= -clipCenter.w &&
           clipCenter.x - radiusX <=  clipCenter.w &&
           clipCenter.y + radiusY >= -clipCenter.w &&
           clipCenter.y - radiusY <=  clipCenter.w &&
           clipCenter.z + radiusZ >= 0.0 &&
           clipCenter.z - radiusZ <= clipCenter.w + radiusZ;
}

bool IsBackfacing(MeshletBounds bounds, float4x4 drawModel, float drawScale) {
    if (bounds.coneCutoff < 0.0) return false;
    float3 worldCenter = mul(float4(bounds.sphereCenter, 1), drawModel).xyz;
    float3 axis = normalize(mul(bounds.coneAxis, (float3x3)drawModel));
    float3 toCenter = worldCenter - viewPos;
    return dot(toCenter, axis) >=
           bounds.coneCutoff * length(toCenter) + bounds.sphereRadius * drawScale;
}

bool IsOccluded(MeshletBounds bounds, float4x4 drawModel,
                float4x4 drawModelView, float drawScale) {
    if ((cullingFlags & 1u) == 0u) return false;
    float4 localCenter = float4(bounds.sphereCenter, 1);
    float4 viewCenter = mul(localCenter, drawModelView);
    float4 worldCenter = mul(localCenter, drawModel);
    float4 clip = mul(worldCenter, previousViewProjection);
    if (clip.w <= 0.001) return false;
    float radius = bounds.sphereRadius * drawScale;
    float2 centerUV = (clip.xy / clip.w) * float2(0.5, -0.5) + 0.5;
    float2 radiusUV = radius * float2(abs(projection[0][0]), abs(projection[1][1])) /
                      clip.w * 0.5;
    float2 uvMin = centerUV - radiusUV;
    float2 uvMax = centerUV + radiusUV;
    float nearestDepth = saturate(clip.z / clip.w -
        radius / max(abs(viewCenter.z), 0.001));
    uvMin = saturate(uvMin);
    uvMax = saturate(uvMax);
    if (uvMin.x >= uvMax.x || uvMin.y >= uvMax.y) return false;

    // Conservative four-corner test against previous-frame depth. Any background or
    // farther sample keeps the meshlet visible, reducing false rejection.
    float pixelDiameter = max(radiusUV.x * screenWidth, radiusUV.y * screenHeight) * 2.0;
    uint mipLevel = min((uint)floor(log2(max(pixelDiameter, 1.0))),
                        max(occlusionMipCount, 1u) - 1u);
    uint mipWidth, mipHeight, availableMips;
    previousDepth.GetDimensions(mipLevel, mipWidth, mipHeight, availableMips);
    float farthestOccluder = 0.0;
    [unroll]
    for (uint y = 0; y < 2; ++y) {
        [unroll]
        for (uint x = 0; x < 2; ++x) {
            float2 uv = lerp(uvMin, uvMax, float2(x, y));
            uint2 pixel = min(uint2(uv * float2(mipWidth, mipHeight)),
                              uint2(mipWidth - 1, mipHeight - 1));
            farthestOccluder = max(farthestOccluder,
                previousDepth.Load(int3(pixel, mipLevel)));
        }
    }
    return farthestOccluder < nearestDepth - 0.01;
}

[numthreads(32, 1, 1)]
void ASMain(uint threadID : SV_GroupThreadID, uint3 groupID : SV_GroupID) {
    if (threadID == 0) visibleCount = 0;
    GroupMemoryBarrierWithGroupSync();

    uint localWorkItem = groupID.x * 32 + threadID;
    uint globalWorkItem = firstMeshlet + localWorkItem;
    uint totalWorkItems = meshletCount * max(instanceCount, 1u);

    if (globalWorkItem < totalWorkItems) {
        uint instanceIndex = instancingEnabled ? globalWorkItem / meshletCount : 0;
        uint globalMeshlet = instancingEnabled
            ? globalWorkItem % meshletCount : globalWorkItem;
        float4x4 drawModel = model;
        float4x4 drawModelView = modelView;
        float4x4 drawMVP = modelViewProjection;
        float drawScale = modelMaxScale;
        if (instancingEnabled) {
            MeshInstanceData instance = meshInstances[instanceIndex];
            drawModel = instance.model;
            drawModelView = mul(drawModel, view);
            drawMVP = mul(drawModelView, projection);
            drawScale = instance.modelMaxScale;
        }
        MeshletBounds bounds = meshletBounds[globalMeshlet];
        // Animated bounds cannot safely use cone/backface or occlusion tests,
        // but bind-pose bounds remain good enough for coarse frustum rejection.
        bool visible = (skinningEnabled == 2)
            ? true
            : (skinningEnabled
                ? IntersectsFrustum(bounds, drawMVP, drawScale)
                : (IntersectsFrustum(bounds, drawMVP, drawScale) &&
                   ((cullingFlags & 2u) != 0u ||
                    !IsBackfacing(bounds, drawModel, drawScale)) &&
                   !IsOccluded(bounds, drawModel, drawModelView, drawScale)));
        if (visible) {
            uint slot;
            InterlockedAdd(visibleCount, 1, slot);
            payloadData.workItems[slot] = uint2(globalMeshlet, instanceIndex);
        }
    }

    GroupMemoryBarrierWithGroupSync();
    DispatchMesh(visibleCount, 1, 1, payloadData);
}
