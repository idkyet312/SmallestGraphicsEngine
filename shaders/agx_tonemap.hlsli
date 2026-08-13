#ifndef AGX_TONEMAP_HLSLI
#define AGX_TONEMAP_HLSLI

// AgX tone mapping (Troy Sobotka's AgX, minimal 6th-order polynomial fit by
// Benjamin Wrensch) with the "Punchy" look for extra contrast + saturation.
// Takes linear HDR, returns display-ready sRGB (already gamma-encoded) with the
// scene colour grade applied.
//
// Single copy shared by every pass that writes the LDR backbuffer. It was
// duplicated verbatim in clustered_dx12_ps.hlsl and grass_ps.hlsl -- the grass
// copy was commented "Identical AgX (Punchy) blocks to clustered_dx12_ps.hlsl",
// which is exactly the kind of thing that drifts. The sky needs it too, so the
// third copy became a shared header instead.
//
// Requires color_grade.hlsli for ApplySceneColorGrade; include it before this.

float3 agxDefaultContrastApprox(float3 x) {
    float3 x2 = x * x;
    float3 x4 = x2 * x2;
    return  15.5     * x4 * x2
          - 40.14    * x4 * x
          + 31.96    * x4
          - 6.868    * x2 * x
          + 0.4298   * x2
          + 0.1191   * x
          - 0.00232;
}

float3 tonemapAgXPunchy(float3 color) {
    // Input transform (sRGB primaries -> AgX working space).
    const float3x3 agxIn = float3x3(
        0.842479062253094,  0.0423282422610123, 0.0423756549057051,
        0.0784335999999992, 0.878468636469772,  0.0784336,
        0.0792237451477643, 0.0791661274605434, 0.879142973793104);
    const float3x3 agxOut = float3x3(
         1.19687900512017,   -0.0528968517574562, -0.0529716355144438,
        -0.0980208811401368,  1.15190312990417,   -0.0980434501171241,
        -0.0990297440797205, -0.0989611768448433,  1.15107367264116);

    const float minEv = -12.47393;
    const float maxEv =  4.026069;

    color = mul(agxIn, color);
    color = clamp(log2(max(color, 1e-10)), minEv, maxEv);
    color = (color - minEv) / (maxEv - minEv);
    color = agxDefaultContrastApprox(color);

    // Punchy look: lift saturation and gamma for a bolder image.
    const float3 lw = float3(0.2126, 0.7152, 0.0722);
    float luma = dot(color, lw);
    color = pow(max(color, 0.0), 1.35);       // punchy contrast
    color = luma + 1.4 * (color - luma);      // punchy saturation

    color = mul(agxOut, color);
    return ApplySceneColorGrade(color);       // already display-encoded
}

#endif
