// Wireframe overlay for prefab collision volumes.
//
// Deliberately unlit and untextured: this draws what the collision system
// believes is there, not what the renderer draws, and any shading would make the
// two harder to tell apart. Colour comes straight from the vertex so the CPU
// side can label each volume without a constant-buffer change per box.

cbuffer CollisionDebugFrame : register(b0) {
    float4x4 viewProjection;
};

struct VSInput {
    float3 position : POSITION;
    float4 color    : COLOR;
};

struct VSOutput {
    float4 position : SV_POSITION;
    float4 color    : COLOR;
};

VSOutput VSMain(VSInput input) {
    VSOutput output;
    output.position = mul(float4(input.position, 1.0f), viewProjection);
    output.color = input.color;
    return output;
}

float4 PSMain(VSOutput input) : SV_TARGET {
#ifdef SGE_HDR_TARGET
    // The HDR target is scene-referred linear, so an sRGB-ish debug colour would
    // be tone-mapped down to a muddy line. Lift it instead: these are diagnostic
    // overlays and reading clearly matters more than being photometrically right.
    return float4(input.color.rgb * 4.0f, input.color.a);
#else
    return input.color;
#endif
}
