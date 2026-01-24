// Depth/Shadow Map Pixel Shader - DX11 HLSL
// This shader is used for shadow map generation
// We don't need to output anything - just depth is written automatically

struct PS_INPUT {
    float4 position : SV_POSITION;
};

void main(PS_INPUT input) {
    // Depth is automatically written to depth buffer
    // No color output needed for shadow map
}

