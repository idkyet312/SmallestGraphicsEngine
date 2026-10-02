// Ray Reconstruction guides for forward-drawn pixels (upscaling RR only).
//
// Upscaling RR evaluates over the finished HDR scene, so forward geometry --
// props, palms, foliage, grass -- is inside its colour input. The resolve wrote
// guides and motion for the visibility-buffer surface BEHIND those pixels: RR
// demodulated a palm by the ground's albedo, reprojected it with the ground's
// parallax and blurred the result. Native RR never saw this geometry (it is
// drawn after RR there).
//
// A pixel is forward-covered when the scene depth is nearer than the
// visibility pass's own depth, or when the grass composite marked it. Depth
// alone missed blade roots: kCoveredBias is metres of depth at 150 m, so far
// roots kept the (2, 2) marker, RR dropped their history every frame and the
// root line sparkled. For those pixels:
//   - motion: camera reprojection of the real depth, the resolve's convention
//     (pixel-centre UV minus the previous frame's rendered projection). Grass
//     included: its MSAA composite writes motion (2, 2), a reactive marker for
//     the built-in TAA. RR read it as a two-screen move, rejected the history
//     of every grass pixel each frame and showed raw jittered blades -- the
//     distant grass shake. Grass then adds the wind's own motion (grass_ps
//     SGE_GRASS_MOTION): with camera motion alone RR reprojected a swaying
//     blade to where it no longer was and its history jittered.
//   - normal: reconstructed from depth; roughness 1.
//   - diffuse albedo: the pixel's own colour, so RR keeps the texture detail in
//     the guide instead of treating it as noisy lighting to smooth away.
//   - specular albedo / hit distance: zero (no traced specular signal there).

cbuffer RRForwardConstants : register(b0) {
    row_major float4x4 invViewProj;      // this frame, as rendered (jittered)
    row_major float4x4 previousViewProj; // previous frame, as rendered
    float2 resolution;
    float  writeGuides;  // 0: Super Resolution, motion only
    float  padding;
    float2 motionJitterUV; // subtracted, as the resolve does (RR_UNJITTER)
    float2 padding2;
};

Texture2D<float>  visibilityDepth : register(t0); // VB pass only
Texture2D<float>  forwardDepth    : register(t1); // + forward geometry
Texture2D<float>  sceneDepth      : register(t2); // + MSAA grass
Texture2D<float4> sceneColor      : register(t3);
Texture2D<float2> grassWindMotion : register(t4); // GrassMSAADX12, or null

RWTexture2D<float2> outputMotion          : register(u0);
RWTexture2D<float4> outputNormalRoughness : register(u1);
RWTexture2D<float4> rrDiffuseAlbedo       : register(u2);
RWTexture2D<float4> rrSpecularAlbedo      : register(u3);
RWTexture2D<float>  rrSpecularHitDistance : register(u4);

static const float kCoveredBias = 1e-5;

float3 WorldPosition(int2 pixel, float depth) {
    float2 uv = (float2(pixel) + 0.5) / resolution;
    float4 clip = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, depth, 1.0);
    float4 world = mul(clip, invViewProj);
    return world.xyz / world.w;
}

float LoadDepth(int2 pixel) {
    return sceneDepth.Load(int3(clamp(pixel, int2(0, 0),
                                      int2(resolution) - 1), 0));
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    int2 pixel = int2(id.xy);
    if (any(pixel >= int2(resolution))) return;

    float vbDepth = visibilityDepth.Load(int3(pixel, 0));
    float propDepth = forwardDepth.Load(int3(pixel, 0));
    float depth = sceneDepth.Load(int3(pixel, 0));
    bool prop = propDepth < vbDepth - kCoveredBias;
    // grass_msaa_composite_cs writes (2, 2) wherever a blade sample covers
    // the pixel; nothing else writes it. (Typed UAV load of R16G16_FLOAT:
    // fine on the RTX parts DLSS runs on.)
    bool grass = depth < propDepth - kCoveredBias ||
                 all(outputMotion[pixel] == float2(2.0, 2.0));
    if (!prop && !grass) return;

    float3 world = WorldPosition(pixel, depth);
    {
        float2 currentUV = (float2(pixel) + 0.5) / resolution;
        float4 previousClip = mul(float4(world, 1.0), previousViewProj);
        float2 previousUV = currentUV;
        if (previousClip.w > 0.001)
            previousUV = previousClip.xy / previousClip.w *
                         float2(0.5, -0.5) + 0.5;
        float2 motion = currentUV - previousUV;
        if (grass) motion += grassWindMotion.Load(int3(pixel, 0));
        outputMotion[pixel] = motion - motionJitterUV;
    }
    if (writeGuides < 0.5) return;

    // Normal from the nearer-in-depth neighbour on each axis, so a silhouette
    // does not bend the normal toward the background.
    float3 dx0 = WorldPosition(pixel + int2(1, 0), LoadDepth(pixel + int2(1, 0))) - world;
    float3 dx1 = world - WorldPosition(pixel - int2(1, 0), LoadDepth(pixel - int2(1, 0)));
    float3 dy0 = WorldPosition(pixel + int2(0, 1), LoadDepth(pixel + int2(0, 1))) - world;
    float3 dy1 = world - WorldPosition(pixel - int2(0, 1), LoadDepth(pixel - int2(0, 1)));
    float3 ddxWorld = dot(dx0, dx0) < dot(dx1, dx1) ? dx0 : dx1;
    float3 ddyWorld = dot(dy0, dy0) < dot(dy1, dy1) ? dy0 : dy1;
    float3 normal = cross(ddyWorld, ddxWorld);
    float lengthSq = dot(normal, normal);
    normal = lengthSq > 1e-20 ? normal * rsqrt(lengthSq) : float3(0.0, 1.0, 0.0);
    // Face the camera: the cross order depends on the projection's handedness.
    float3 eye = WorldPosition(pixel, 0.0);
    if (dot(normal, eye - world) < 0.0) normal = -normal;
    outputNormalRoughness[pixel] = float4(normal, 1.0);

    float3 color = max(sceneColor.Load(int3(pixel, 0)).rgb, 0.0);
    rrDiffuseAlbedo[pixel] = float4(color / (color + 1.0), 1.0);
    rrSpecularAlbedo[pixel] = 0.0;
    rrSpecularHitDistance[pixel] = 0.0;
}
