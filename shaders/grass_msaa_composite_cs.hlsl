Texture2DMS<float4, 4> grassColor : register(t0);
Texture2DMS<float, 4> grassDepth : register(t1);
Texture2D<float> sceneDepth : register(t2);
RWTexture2D<float4> sceneColor : register(u0);
RWTexture2D<float2> sceneMotion : register(u1);

cbuffer CompositeConstants : register(b0) {
    uint2 outputSize;
};

[numthreads(8, 8, 1)]
void main(uint3 threadId : SV_DispatchThreadID) {
    uint2 pixel = threadId.xy;
    if (any(pixel >= outputSize)) return;

    const float opaqueDepth = sceneDepth.Load(int3(pixel, 0));
    float3 premultipliedGrass = 0.0;
    float coverage = 0.0;

    [unroll]
    for (uint sampleIndex = 0; sampleIndex < 4; ++sampleIndex) {
        const float4 sampleColor = grassColor.Load(pixel, sampleIndex);
        const float sampleDepth = grassDepth.Load(pixel, sampleIndex);
        const bool covered = sampleColor.a > 0.0 &&
            sampleDepth <= opaqueDepth + 2e-5;
        if (covered) {
            premultipliedGrass += sampleColor.rgb * 0.25;
            coverage += 0.25;
        }
    }

    if (coverage > 0.0) {
        const float3 background = sceneColor[pixel].rgb;
        sceneColor[pixel] = float4(
            premultipliedGrass + background * (1.0 - coverage), 1.0);
        // Grass wind has no per-blade motion vectors. Mark covered pixels
        // reactive so TAA preserves the 4x spatial resolve instead of ghosting.
        sceneMotion[pixel] = float2(2.0, 2.0);
    }
}
