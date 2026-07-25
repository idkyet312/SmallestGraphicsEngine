cbuffer SkyBuffer : register(b0) {
    float3 cameraForward;
    float tanHalfFov;
    float3 cameraRight;
    float aspectRatio;
    float3 cameraUp;
    float time;
    float3 sunDirection;
    float exposure;
};

Texture2D skyEquirectangular : register(t0);
SamplerState skySampler : register(s0);

#include "color_grade.hlsli"

struct PSInput {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

// Narkowicz ACES approximation; the EXR is linear HDR and the backbuffer is
// 8-bit non-sRGB, so tonemap + gamma happen here.
float3 TonemapACES(float3 x) {
    return saturate((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14));
}

float4 main(PSInput input) : SV_Target {
    float2 ndc = input.uv * 2.0 - 1.0;
    ndc.y = -ndc.y;
    float3 ray = normalize(cameraForward +
                           cameraRight * ndc.x * aspectRatio * tanHalfFov +
                           cameraUp * ndc.y * tanHalfFov);

    float2 skyUV = float2(atan2(ray.z, ray.x) * 0.159154943 + 0.5,
                          acos(clamp(ray.y, -1.0, 1.0)) * 0.318309886);

    // Pick the mip by how fast longitude changes per pixel. Near the poles a
    // single pixel spans a huge U range, so a coarse mip (whose texels already
    // average many longitudes) removes the radial smear. Deriving LOD from the
    // world-space ray avoids the atan2 wrap seam that fools automatic Sample()
    // gradients into selecting a garbage mip along the +/-180 longitude line.
    uint texW, texH, mipCount;
    skyEquirectangular.GetDimensions(0, texW, texH, mipCount);
    float horiz = length(float2(ray.x, ray.z));           // cos(latitude): ->0 at poles
    float3 dRayX = ddx(ray), dRayY = ddy(ray);
    float angleDeriv = max(length(dRayX), length(dRayY)); // radians of view swept per pixel
    float uTexelsPerPixel = angleDeriv / max(horiz, 1e-3) * 0.159154943 * texW;
    float lod = clamp(log2(max(uTexelsPerPixel, 1.0)), 0.0, (float)(mipCount - 1));
    float3 hdr = skyEquirectangular.SampleLevel(skySampler, skyUV, lod).rgb;

    float3 color;
#ifdef SGE_HDR_TARGET
    color = hdr * exposure;
#else
    color = TonemapACES(hdr * exposure);
    color = pow(color, 1.0 / 2.2);
    color = ApplySceneColorGrade(color);
#endif
    return float4(color, 1.0);
}
