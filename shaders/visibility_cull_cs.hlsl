cbuffer CullConstants : register(b0) {
    float4 frustumPlanes[6];
    matrix previousViewProjection;
    float3 cameraPosition;
    float projectionScaleY;
    uint2 screenSize;
    uint commandCount;
    uint hzbMipCount;
    uint useOcclusion;
    float lodPixelThreshold;
    float2 cullPadding;
};

struct CullInput {
    uint2 vertexBufferAddress;
    uint vertexBufferSize;
    uint vertexStride;
    uint2 indexBufferAddress;
    uint indexBufferSize;
    uint indexFormat;
    uint2 matrixCBV;
    uint drawCallID;
    uint indexCountPerInstance;
    uint instanceCount;
    uint startIndexLocation;
    int baseVertexLocation;
    uint startInstanceLocation;
    float4 worldBounds;
    uint cullFlags;
    uint3 cullPadding;
};

static const uint CULL_FLAG_FRUSTUM_ONLY = 1u << 0u;

StructuredBuffer<CullInput> inputCommands : register(t0);
Texture2D<float> previousDepth : register(t1);
RWByteAddressBuffer visibleCommands : register(u0);
RWByteAddressBuffer visibleCount : register(u1);

bool FrustumVisible(float3 center, float radius) {
    [unroll]
    for (uint i = 0; i < 6; ++i) {
        if (dot(frustumPlanes[i].xyz, center) + frustumPlanes[i].w < -radius)
            return false;
    }
    return true;
}

bool Occluded(float3 center, float radius) {
    if (useOcclusion == 0 || hzbMipCount == 0) return false;
    float4 clip = mul(float4(center, 1.0), previousViewProjection);
    if (clip.w <= 0.001) return false;

    float2 centerUV = (clip.xy / clip.w) * float2(0.5, -0.5) + 0.5;
    float2 radiusUV = radius * projectionScaleY / clip.w * 0.5;
    float2 uvMin = saturate(centerUV - radiusUV);
    float2 uvMax = saturate(centerUV + radiusUV);
    if (any(uvMin >= uvMax)) return false;

    float pixelDiameter = max((uvMax.x - uvMin.x) * screenSize.x,
                              (uvMax.y - uvMin.y) * screenSize.y);
    uint mip = min((uint)floor(log2(max(pixelDiameter, 1.0))), hzbMipCount - 1u);
    uint mipWidth, mipHeight, availableMips;
    previousDepth.GetDimensions(mip, mipWidth, mipHeight, availableMips);
    float farthestOccluder = 0.0;
    [unroll]
    for (uint y = 0; y < 2; ++y) {
        [unroll]
        for (uint x = 0; x < 2; ++x) {
            float2 uv = lerp(uvMin, uvMax, float2(x, y));
            uint2 p = min(uint2(uv * float2(mipWidth, mipHeight)),
                          uint2(mipWidth - 1, mipHeight - 1));
            farthestOccluder = max(farthestOccluder,
                previousDepth.Load(int3(p, mip)));
        }
    }
    float distanceToCamera = max(length(center - cameraPosition), 0.001);
    float nearestDepth = saturate(clip.z / clip.w - radius / distanceToCamera);
    return farthestOccluder < nearestDepth - 0.01;
}

void WriteCommand(uint outputIndex, CullInput command) {
    // D3D12 command layout: VBV(16), IBV(16), root CBV(8), constant(4), DRAW_INDEXED(20).
    uint address = outputIndex * 64u;
    visibleCommands.Store2(address + 0u, command.vertexBufferAddress);
    visibleCommands.Store(address + 8u, command.vertexBufferSize);
    visibleCommands.Store(address + 12u, command.vertexStride);
    visibleCommands.Store2(address + 16u, command.indexBufferAddress);
    visibleCommands.Store(address + 24u, command.indexBufferSize);
    visibleCommands.Store(address + 28u, command.indexFormat);
    visibleCommands.Store2(address + 32u, command.matrixCBV);
    visibleCommands.Store(address + 40u, command.drawCallID);
    visibleCommands.Store4(address + 44u, uint4(command.indexCountPerInstance,
        command.instanceCount, command.startIndexLocation,
        asuint(command.baseVertexLocation)));
    visibleCommands.Store(address + 60u, command.startInstanceLocation);
}

[numthreads(64, 1, 1)]
void main(uint3 threadID : SV_DispatchThreadID) {
    uint index = threadID.x;
    if (index >= commandCount) return;
    CullInput command = inputCommands[index];
    float3 center = command.worldBounds.xyz;
    float radius = command.worldBounds.w;
    if (!FrustumVisible(center, radius)) return;

    // Moving destruction chunks and closed house shells cannot safely use the
    // previous frame as an occlusion oracle. Their CPU material policy already
    // requests frustum-only culling; honour it here and keep tiny fragments
    // from blinking at the projected-size threshold as they move or settle.
    const bool frustumOnly =
        (command.cullFlags & CULL_FLAG_FRUSTUM_ONLY) != 0u;
    if (!frustumOnly) {
        float distanceToCamera = max(length(center - cameraPosition), 0.001);
        float projectedRadius = radius * projectionScaleY / distanceToCamera
                              * screenSize.y * 0.5;
        if (projectedRadius * 2.0 < lodPixelThreshold) return;
        if (Occluded(center, radius)) return;
    }

    uint outputIndex;
    visibleCount.InterlockedAdd(0, 1, outputIndex);
    if (outputIndex < commandCount) WriteCommand(outputIndex, command);
}
