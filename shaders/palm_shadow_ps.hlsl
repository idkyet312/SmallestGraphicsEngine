Texture2D palmTexture : register(t0);
SamplerState palmSampler : register(s0);

void main(float4 position : SV_POSITION, float2 texcoord : TEXCOORD0) {
    clip(palmTexture.Sample(palmSampler, texcoord).a - 0.20);
}
