Texture2D<float> SourceDepth : register(t0);
RWTexture2D<float> DestinationDepth : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    uint width, height;
    DestinationDepth.GetDimensions(width, height);
    if (id.x >= width || id.y >= height) return;

    uint sourceWidth, sourceHeight;
    SourceDepth.GetDimensions(sourceWidth, sourceHeight);
    uint2 p = id.xy * 2;
    uint2 limit = uint2(sourceWidth - 1, sourceHeight - 1);
    float d0 = SourceDepth.Load(int3(min(p, limit), 0));
    float d1 = SourceDepth.Load(int3(min(p + uint2(1, 0), limit), 0));
    float d2 = SourceDepth.Load(int3(min(p + uint2(0, 1), limit), 0));
    float d3 = SourceDepth.Load(int3(min(p + uint2(1, 1), limit), 0));
    DestinationDepth[id.xy] = max(max(d0, d1), max(d2, d3));
}
