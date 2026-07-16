Texture2D sceneTexture : register(t0);
SamplerState linearClamp : register(s0);

cbuffer FXAAConstants : register(b0) {
    float2 inverseScreenSize;
};

struct ScreenVertex {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

ScreenVertex VSMain(uint vertexId : SV_VertexID) {
    const float2 position = vertexId == 0 ? float2(-1.0, -1.0)
                          : vertexId == 1 ? float2(-1.0,  3.0)
                                          : float2( 3.0, -1.0);
    ScreenVertex output;
    output.position = float4(position, 0.0, 1.0);
    output.uv = position * float2(0.5, -0.5) + 0.5;
    return output;
}

float Luma(float3 color) {
    return dot(color, float3(0.299, 0.587, 0.114));
}

float4 PSMain(ScreenVertex input) : SV_Target {
    const float2 pixel = inverseScreenSize;
    const float3 rgbM  = sceneTexture.Sample(linearClamp, input.uv).rgb;
    const float3 rgbNW = sceneTexture.Sample(linearClamp, input.uv + pixel * float2(-1, -1)).rgb;
    const float3 rgbNE = sceneTexture.Sample(linearClamp, input.uv + pixel * float2( 1, -1)).rgb;
    const float3 rgbSW = sceneTexture.Sample(linearClamp, input.uv + pixel * float2(-1,  1)).rgb;
    const float3 rgbSE = sceneTexture.Sample(linearClamp, input.uv + pixel * float2( 1,  1)).rgb;

    const float lumaM = Luma(rgbM);
    const float lumaNW = Luma(rgbNW);
    const float lumaNE = Luma(rgbNE);
    const float lumaSW = Luma(rgbSW);
    const float lumaSE = Luma(rgbSE);
    const float lumaMin = min(lumaM, min(min(lumaNW, lumaNE), min(lumaSW, lumaSE)));
    const float lumaMax = max(lumaM, max(max(lumaNW, lumaNE), max(lumaSW, lumaSE)));
    const float lumaRange = lumaMax - lumaMin;
    if (lumaRange < max(0.0312, lumaMax * 0.125))
        return float4(rgbM, 1.0);

    float2 direction;
    direction.x = -((lumaNW + lumaNE) - (lumaSW + lumaSE));
    direction.y =  ((lumaNW + lumaSW) - (lumaNE + lumaSE));
    const float directionReduce = max(
        (lumaNW + lumaNE + lumaSW + lumaSE) * (0.25 / 8.0), 1.0 / 128.0);
    const float inverseDirection =
        1.0 / (min(abs(direction.x), abs(direction.y)) + directionReduce);
    direction = clamp(direction * inverseDirection, -8.0, 8.0) * pixel;

    const float3 rgbA = 0.5 * (
        sceneTexture.Sample(linearClamp, input.uv + direction * (-1.0 / 6.0)).rgb +
        sceneTexture.Sample(linearClamp, input.uv + direction * ( 1.0 / 6.0)).rgb);
    const float3 rgbB = rgbA * 0.5 + 0.25 * (
        sceneTexture.Sample(linearClamp, input.uv + direction * -0.5).rgb +
        sceneTexture.Sample(linearClamp, input.uv + direction *  0.5).rgb);
    const float lumaB = Luma(rgbB);
    return float4((lumaB < lumaMin || lumaB > lumaMax) ? rgbA : rgbB, 1.0);
}
