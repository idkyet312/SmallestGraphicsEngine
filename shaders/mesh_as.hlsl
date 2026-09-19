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
    // Bits 2-4 are the debug isolation mask (F6): each one suppresses a single
    // test so a flickering chunk can be attributed to one of the three rather
    // than guessed at. All clear is the shipping path.
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
    // Conservative widening of the frustum test. 1.0 is the exact
    // sphere-vs-plane answer; above that trades culling for certainty.
    float frustumRadiusScale;
};

cbuffer CameraBuffer : register(b2) {
    float3 viewPos;
    float cameraPadding;
};

// Debug isolation bits in cullingFlags. Each suppresses exactly one test, so
// cycling them over the same camera path attributes a vanishing chunk to a
// single culprit instead of a hypothesis.
static const uint kDebugNoFrustum   = 4u;
static const uint kDebugNoCone      = 8u;
static const uint kDebugNoOcclusion = 16u;

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

// Reject only when the sphere lies wholly outside the half-space, i.e.
// d < -radius * length(n). Squaring avoids the sqrt; the d < 0 guard is what
// keeps the squaring from flipping the sign, so the two forms are identical.
bool SphereOutsidePlane(float d, float3 n, float radiusSq) {
    return d < 0.0 && d * d > radiusSq * dot(n, n);
}

// Exact conservative sphere-vs-frustum, evaluated in the draw's LOCAL space.
//
// mul(v, M) computes clip[j] = sum_i v[i] * M[i][j], so clip.x is built from
// column 0 of drawMVP and clip.w from column 3. That holds whatever
// transposition the CPU applied, because it is the same mul the test itself
// performs. Each clip-space half-space pulls back to a local-space plane built
// from those columns, and the plane normal's LENGTH already carries both the
// projection and the full model transform.
//
// That length is the whole bug. The old test used radius * |projection[0][0]|
// as the margin, which is short by 1/cos(half-FOV): at this engine's 60 degree
// vertical FOV and 16:9 that is 43% on X and 15% on Y. Meshlets near the left
// and right screen edges were rejected while still plainly visible, which is
// what made chunks of the shelter and the fence vanish.
//
// drawScale must NOT be reapplied here. The normal length already contains it,
// per plane, so anisotropic scale and shear come out exact rather than being
// inflated to the largest axis the way modelMaxScale did. It is still needed by
// IsBackfacing and IsOccluded, which work in world and view space.
bool IntersectsFrustum(MeshletBounds bounds, float4x4 drawMVP) {
    if ((cullingFlags & kDebugNoFrustum) != 0u) return true;

    float4 clip = mul(float4(bounds.sphereCenter, 1), drawMVP);
    float3 cx = drawMVP._m00_m10_m20;   // column 0
    float3 cy = drawMVP._m01_m11_m21;   // column 1
    float3 cz = drawMVP._m02_m12_m22;   // column 2
    float3 cw = drawMVP._m03_m13_m23;   // column 3

    // 1.0 is the exact test. Below it the sphere is deliberately under-sized so
    // the cull bites early and the boundary becomes visible -- a debug aid, not
    // a correct setting. The floor keeps a draw site that forgets the root
    // constant at a very aggressive cull rather than a zero-radius blackout.
    float radius = bounds.sphereRadius * max(frustumRadiusScale, 0.1);
    float radiusSq = radius * radius;

    if (SphereOutsidePlane(clip.x + clip.w, cx + cw, radiusSq)) return false;
    if (SphereOutsidePlane(clip.w - clip.x, cw - cx, radiusSq)) return false;
    if (SphereOutsidePlane(clip.y + clip.w, cy + cw, radiusSq)) return false;
    if (SphereOutsidePlane(clip.w - clip.y, cw - cy, radiusSq)) return false;
    if (SphereOutsidePlane(clip.z,          cz,      radiusSq)) return false;
    if (SphereOutsidePlane(clip.w - clip.z, cw - cz, radiusSq)) return false;
    return true;
}

bool IsBackfacing(MeshletBounds bounds, float4x4 drawModel, float drawScale) {
    if ((cullingFlags & kDebugNoCone) != 0u) return false;
    if (bounds.coneCutoff < 0.0) return false;
    float3 worldCenter = mul(float4(bounds.sphereCenter, 1), drawModel).xyz;
    float3 axis = normalize(mul(bounds.coneAxis, (float3x3)drawModel));
    float3 toCenter = worldCenter - viewPos;
    return dot(toCenter, axis) >=
           bounds.coneCutoff * length(toCenter) + bounds.sphereRadius * drawScale;
}

bool IsOccluded(MeshletBounds bounds, float4x4 drawModel,
                float4x4 drawModelView, float drawScale) {
    if ((cullingFlags & 1u) == 0u ||
        (cullingFlags & kDebugNoOcclusion) != 0u) return false;
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
        // Skinned meshlet bounds are bind-pose: they describe where the geometry
        // WAS, not where the pose put it. A ragdoll is posed further from bind
        // pose than any animation, so those bounds reject clusters that are
        // plainly on screen and a corpse loses limbs. Skinned draws skip meshlet
        // culling entirely; static geometry keeps the full test.
        bool visible = skinningEnabled
            ? true
            : (IntersectsFrustum(bounds, drawMVP) &&
               ((cullingFlags & 2u) != 0u ||
                !IsBackfacing(bounds, drawModel, drawScale)) &&
               !IsOccluded(bounds, drawModel, drawModelView, drawScale));
        if (visible) {
            uint slot;
            InterlockedAdd(visibleCount, 1, slot);
            payloadData.workItems[slot] = uint2(globalMeshlet, instanceIndex);
        }
    }

    GroupMemoryBarrierWithGroupSync();
    DispatchMesh(visibleCount, 1, 1, payloadData);
}
