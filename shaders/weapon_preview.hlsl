cbuffer PreviewCamera : register(b0) {
    float4x4 modelViewProjection;
    float4 eyePosition;
};
cbuffer PreviewMaterial : register(b1) {
    float4 baseColor;
    float4 emissiveMetal;
    float4 surface; // roughness, normal-Y sign, alpha cutoff, occlusion strength
    uint4 flags;    // texture mask, roughness-only, coverage flags, reserved
};
Texture2D albedoMap : register(t0);
Texture2D normalMap : register(t1);
Texture2D metalRoughMap : register(t2);
Texture2D emissiveMap : register(t3);
SamplerState materialSampler : register(s0);

struct PreviewVertex {
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD;
    float4 tangent : TANGENT;
};
struct PreviewPixel {
    float4 position : SV_POSITION;
    float3 localPosition : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD;
    float4 tangent : TANGENT;
};
PreviewPixel PreviewVS(PreviewVertex input) {
    PreviewPixel output;
    output.position = mul(float4(input.position, 1), modelViewProjection);
    output.localPosition = input.position;
    output.normal = input.normal;
    output.uv = input.uv;
    output.tangent = input.tangent;
    return output;
}

float3 StudioLight(float3 N, float3 V, float3 L, float3 albedo,
                   float metal, float rough, float3 radiance) {
    float3 H = normalize(V + L);
    float nv = max(dot(N, V), 0.001), nl = saturate(dot(N, L));
    float nh = saturate(dot(N, H)), vh = saturate(dot(V, H));
    float a = rough * rough, a2 = a * a;
    float d = nh * nh * (a2 - 1) + 1;
    float distribution = a2 / max(3.14159265 * d * d, 0.00001);
    float k = (rough + 1) * (rough + 1) * 0.125;
    float visibility = (nv / (nv * (1 - k) + k)) * (nl / (nl * (1 - k) + k));
    float3 f0 = lerp(0.04.xxx, albedo, metal);
    float3 fresnel = f0 + (1 - f0) * pow(1 - vh, 5);
    float3 specular = distribution * visibility * fresnel / max(4 * nv * nl, 0.001);
    float3 diffuse = (1 - fresnel) * (1 - metal) * albedo / 3.14159265;
    return (diffuse + specular) * radiance * nl;
}

float4 PreviewPS(PreviewPixel input, bool frontFace : SV_IsFrontFace) : SV_TARGET {
    float4 color = baseColor;
    if (flags.x & 1) {
        float4 texel = albedoMap.Sample(materialSampler, input.uv);
        // Match the game's UNORM texture decode; sRGB resources decode in hardware.
        color.rgb *= (flags.x & 16) ? texel.rgb : pow(max(texel.rgb, 0), 2.2);
        color.a *= texel.a;
        if (flags.z & 2) color.a *= dot(texel.rgb, float3(0.299, 0.587, 0.114));
    }
    if (flags.z & 1) clip(color.a - surface.z);
    float3 N = normalize(input.normal);
    if (flags.x & 2) {
        float3 T = input.tangent.xyz - N * dot(input.tangent.xyz, N);
        float3 B;
        if (dot(T, T) > 0.00001) {
            T = normalize(T);
            B = cross(N, T) * input.tangent.w;
        } else {
            // Some weapon exports omit tangents; derive their UV frame per pixel.
            float3 dx = ddx(input.localPosition), dy = ddy(input.localPosition);
            float2 ux = ddx(input.uv), uy = ddy(input.uv);
            float3 p = cross(dy, N), q = cross(N, dx);
            T = p * ux.x + q * uy.x;
            B = p * ux.y + q * uy.y;
            float inverseLength = rsqrt(max(max(dot(T, T), dot(B, B)), 0.00001));
            T *= inverseLength;
            B *= inverseLength;
        }
        float3 mapped = normalMap.Sample(materialSampler, input.uv).xyz * 2 - 1;
        mapped.y *= surface.y;
        N = normalize(T * mapped.x + B * mapped.y + N * mapped.z);
    }
    // Imported axis remaps can reverse winding independently of authored normals.
    // This two-sided studio view lights the visible surface toward the camera.
    if (dot(N, eyePosition.xyz - input.localPosition) < 0) N = -N;
    float metal = saturate(emissiveMetal.w), rough = saturate(surface.x), ao = 1;
    if (flags.x & 4) {
        float3 mr = metalRoughMap.Sample(materialSampler, input.uv).rgb;
        if (flags.y != 0) {
            // The weapon loader's standalone roughness maps carry final values.
            rough = mr.g;
        } else {
            metal *= mr.b;
            rough *= mr.g;
        }
        ao = lerp(1, mr.r, saturate(surface.w));
    }
    rough = clamp(rough, 0.08, 1);
    float3 V = normalize(eyePosition.xyz - input.localPosition);
    float3 f0 = lerp(0.04.xxx, color.rgb, metal);
    // Broad neutral fill keeps the metal readable while the two studio lights
    // reveal authored roughness and normal detail as the weapon is rotated.
    float3 result = ao * (color.rgb * (1 - metal) * 0.38 +
        f0 * (0.30 + 0.40 * pow(1 - saturate(dot(N, V)), 2)) * lerp(0.8, 0.3, rough));
    result += StudioLight(N, V, normalize(float3(0.6, 0.8, -0.3)),
        color.rgb, metal, rough, float3(4.0, 3.9, 3.8));
    result += StudioLight(N, V, normalize(float3(-0.6, 0.25, 0.4)),
        color.rgb, metal, rough, float3(1.2, 1.3, 1.4));
    float3 emissive = emissiveMetal.rgb;
    if (flags.x & 8) {
        float3 sampleColor = emissiveMap.Sample(materialSampler, input.uv).rgb;
        emissive *= (flags.x & 32) ? sampleColor : pow(max(sampleColor, 0), 2.2);
    }
    result = max(result + emissive, 0) * 1.4;
    result = saturate((result * (2.51 * result + 0.03)) /
                     (result * (2.43 * result + 0.59) + 0.14));
    return float4(pow(result, 1.0 / 2.2), (flags.z & 4) ? saturate(color.a) : 1);
}
