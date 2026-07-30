Texture2D palmTexture : register(t0);
SamplerState palmSampler : register(s0);

void main(float4 position : SV_POSITION, float2 texcoord : TEXCOORD0) {
    // Slightly more porous than the visible cutout. Thin translucent leaf
    // margins transmit sunlight and carve detailed shafts instead of casting
    // one solid card-shaped shadow.
    clip(palmTexture.Sample(palmSampler, texcoord).a - 0.34);
}
