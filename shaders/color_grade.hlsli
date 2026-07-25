#ifndef SGE_COLOR_GRADE_HLSLI
#define SGE_COLOR_GRADE_HLSLI

// Display-referred tropical daylight grade. Shadows inherit cool sky light,
// highlights retain warm sun, and dominant foliage greens are gently restrained.
float3 ApplySceneColorGrade(float3 color)
{
    color = saturate(color);
    const float3 lumaWeights = float3(0.2126, 0.7152, 0.0722);
    float luma = dot(color, lumaWeights);

    float shadowWeight = 1.0 - smoothstep(0.18, 0.55, luma);
    float highlightWeight = smoothstep(0.52, 0.92, luma);
    color *= lerp(1.0.xxx, float3(0.94, 0.99, 1.07), shadowWeight * 0.72);
    color *= lerp(1.0.xxx, float3(1.045, 1.012, 0.955), highlightWeight * 0.62);

    // Selective green reduction preserves neutral materials and blue sky.
    float greenDominance = max(color.g - max(color.r, color.b), 0.0);
    color.g -= greenDominance * 0.20;
    color.r += greenDominance * 0.025;
    color.b += greenDominance * 0.015;

    luma = dot(color, lumaWeights);
    color = luma.xxx + (color - luma.xxx) * 0.97;
    return saturate(color);
}

#endif
