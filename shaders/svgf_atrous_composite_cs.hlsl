// SVGF à-trous composite pass — Phase 5c
//
// After the à-trous wavelet filter has been applied to the reflection
// signal, this pass composites it back into the main lit output:
//
//   outputColor = outputColor - originalReflection + filteredReflection
//
// The subtraction of the original is what lets the resolve write its
// full lit result to outputColor without knowing whether the spatial
// filter will run, and without splitting the BRDF calculation.
//
// Debug view 6: writes only the filtered reflection to outputColor,
// so the user can inspect what the à-trous pass is producing.

cbuffer CompositeConstants : register(b0) {
    uint screenWidth;
    uint screenHeight;
    uint debugViewMode;
    uint pad0;
};

Texture2D<float4> outputTexture     : register(t0); // lit result (HDR)
Texture2D<float4> sourceReflection  : register(t1); // original specular IBL
Texture2D<float4> filteredReflection : register(t2); // à-trous result

RWTexture2D<float4> outputColor : register(u0);

[numthreads(8, 8, 1)]
void main(uint3 dispatchThreadID : SV_DispatchThreadID) {
    uint2 pixel = dispatchThreadID.xy;
    if (pixel.x >= screenWidth || pixel.y >= screenHeight) return;

    float3 srcRefl = sourceReflection.Load(int3(pixel, 0)).rgb;
    float3 fltRefl = filteredReflection.Load(int3(pixel, 0)).rgb;

    if (debugViewMode == 6u) {
        // Debug view 6: show only the filtered reflection output.
        outputColor[pixel] = float4(fltRefl, 1.0);
        return;
    }

    float3 lit = outputTexture.Load(int3(pixel, 0)).rgb;
    float3 composite = lit - srcRefl + fltRefl;
    outputColor[pixel] = float4(max(composite, 0.0), 1.0);
}
