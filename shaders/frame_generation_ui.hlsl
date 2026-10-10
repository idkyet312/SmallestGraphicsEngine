Texture2D<float4> hudless : register(t0);
Texture2D<float4> ui : register(t1);

float4 VSMain(uint vertex : SV_VertexID) : SV_Position
{
    float2 uv = float2((vertex << 1) & 2, vertex & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

float4 PSMain(float4 position : SV_Position) : SV_Target
{
    int3 pixel = int3(int2(position.xy), 0);
    float4 overlay = ui.Load(pixel);
    return float4(overlay.rgb + (1 - overlay.a) * hudless.Load(pixel).rgb, 1);
}
