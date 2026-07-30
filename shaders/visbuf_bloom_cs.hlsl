cbuffer BloomConstants : register(b0)
{
    uint2 sourceSize;
    uint2 destinationSize;
    float threshold;
    float softKnee;
    float scatter;
    float padding;
};

Texture2D<float4> bloomSource : register(t0);
RWTexture2D<float4> bloomDestination : register(u0);
SamplerState linearClamp : register(s0);

float Luminance(float3 color)
{
    return dot(color, float3(0.2126, 0.7152, 0.0722));
}

float3 SampleDownsample(float2 uv)
{
    float2 texel = 1.0 / float2(sourceSize);
    float3 color = bloomSource.SampleLevel(linearClamp, uv, 0.0).rgb * 0.25;
    color += bloomSource.SampleLevel(
        linearClamp, uv + texel * float2(-1.0, -1.0), 0.0).rgb * 0.1875;
    color += bloomSource.SampleLevel(
        linearClamp, uv + texel * float2( 1.0, -1.0), 0.0).rgb * 0.1875;
    color += bloomSource.SampleLevel(
        linearClamp, uv + texel * float2(-1.0,  1.0), 0.0).rgb * 0.1875;
    color += bloomSource.SampleLevel(
        linearClamp, uv + texel * float2( 1.0,  1.0), 0.0).rgb * 0.1875;
    return color;
}

float3 Prefilter(float3 color)
{
    if (threshold <= 0.0) return color;
    float brightness = Luminance(color);
    float knee = max(softKnee, 1e-4);
    float soft = clamp(brightness - threshold + knee, 0.0, 2.0 * knee);
    soft = soft * soft / (4.0 * knee + 1e-4);
    float contribution = max(brightness - threshold, soft) /
                         max(brightness, 1e-4);
    return color * contribution;
}

float3 SampleTent(float2 uv)
{
    float2 texel = 1.0 / float2(sourceSize);
    float3 color = bloomSource.SampleLevel(linearClamp, uv, 0.0).rgb * 4.0;
    color += bloomSource.SampleLevel(
        linearClamp, uv + float2(-texel.x, 0.0), 0.0).rgb * 2.0;
    color += bloomSource.SampleLevel(
        linearClamp, uv + float2( texel.x, 0.0), 0.0).rgb * 2.0;
    color += bloomSource.SampleLevel(
        linearClamp, uv + float2(0.0, -texel.y), 0.0).rgb * 2.0;
    color += bloomSource.SampleLevel(
        linearClamp, uv + float2(0.0,  texel.y), 0.0).rgb * 2.0;
    color += bloomSource.SampleLevel(
        linearClamp, uv + float2(-texel.x, -texel.y), 0.0).rgb;
    color += bloomSource.SampleLevel(
        linearClamp, uv + float2( texel.x, -texel.y), 0.0).rgb;
    color += bloomSource.SampleLevel(
        linearClamp, uv + float2(-texel.x,  texel.y), 0.0).rgb;
    color += bloomSource.SampleLevel(
        linearClamp, uv + float2( texel.x,  texel.y), 0.0).rgb;
    return color * (1.0 / 16.0);
}

[numthreads(8, 8, 1)]
void Downsample(uint3 threadID : SV_DispatchThreadID)
{
    uint2 pixel = threadID.xy;
    if (any(pixel >= destinationSize)) return;
    float2 uv = (float2(pixel) + 0.5) / float2(destinationSize);
    bloomDestination[pixel] = float4(Prefilter(SampleDownsample(uv)), 1.0);
}

[numthreads(8, 8, 1)]
void Upsample(uint3 threadID : SV_DispatchThreadID)
{
    uint2 pixel = threadID.xy;
    if (any(pixel >= destinationSize)) return;
    float2 uv = (float2(pixel) + 0.5) / float2(destinationSize);
    float3 accumulated = bloomDestination[pixel].rgb;
    accumulated += SampleTent(uv) * scatter;
    bloomDestination[pixel] = float4(accumulated, 1.0);
}
