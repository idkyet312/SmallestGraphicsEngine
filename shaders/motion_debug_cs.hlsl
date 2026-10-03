// Motion-vector debug overlay (F7 / SGE_MOTION_DEBUG=<mode>).
//
// Reads the visibility buffer's motion texture -- the exact resource DLSS / Ray
// Reconstruction consumed this frame -- and paints it over the tone-mapped
// present texture, so the view works with RR active (the resolve debug views
// switch RR off).
//
// Motion is current UV minus previous UV, in render-resolution UV units.
//   - hue: direction the surface travelled on screen; brightness: speed, log
//     scaled so 1/16 px and 16 px are both readable.
//   - arrows: one per tile, from where the pixel was last frame to where it is
//     now, lengthened by arrowGain so sub-pixel motion is visible.
//   - magenta: the grass composite's (2, 2) reactive marker.
//   - cyan: NaN / Inf (bit test; FXC folds isnan()).
// Mode 1 blends over a dimmed greyscale scene, mode 2 shows motion only.

cbuffer MotionDebugConstants : register(b0) {
    float2 renderSize;
    float2 displaySize;
    uint   mode;
    float  arrowGain;
    float  tileSize;
    float  fullScalePixels;
};

Texture2D<float2>   motionTexture : register(t0);
RWTexture2D<float4> present       : register(u0);

bool NonFinite(float2 v) {
    uint2 bits = asuint(v) & 0x7f800000u;
    return any(bits == 0x7f800000u);
}

float2 LoadMotion(float2 displayPixel) {
    int2 p = int2(displayPixel * renderSize / displaySize);
    p = clamp(p, int2(0, 0), int2(renderSize) - 1);
    return motionTexture.Load(int3(p, 0));
}

float3 Hue(float angle) {
    float h = frac(angle / 6.28318530718) * 6.0;
    return saturate(float3(abs(h - 3.0) - 1.0,
                           2.0 - abs(h - 2.0),
                           2.0 - abs(h - 4.0)));
}

float SegmentDistance(float2 p, float2 a, float2 b) {
    float2 ab = b - a;
    float t = saturate(dot(p - a, ab) / max(dot(ab, ab), 1e-6));
    return length(p - (a + ab * t));
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
    if (any(id.xy >= uint2(displaySize))) return;
    float2 pixel = float2(id.xy) + 0.5;
    float4 scene = present[id.xy];

    float2 motion = LoadMotion(pixel);
    float3 colour;
    if (NonFinite(motion)) {
        colour = float3(0.0, 1.0, 1.0);
    } else if (all(motion == float2(2.0, 2.0))) {
        colour = float3(1.0, 0.0, 1.0);
    } else {
        // Render pixels: the unit DLSS reasons in.
        float2 px = motion * renderSize;
        float speed = length(px);
        float brightness = saturate(log2(1.0 + speed * 16.0) /
                                    log2(1.0 + fullScalePixels * 16.0));
        // Screen y grows downward; flip so "up" reads as up on the wheel.
        colour = Hue(atan2(-px.y, px.x)) * brightness;
    }

    float grey = dot(scene.rgb, float3(0.299, 0.587, 0.114));
    float3 result = mode == 2u ? colour
                               : lerp(grey.xxx * 0.35, colour, 0.75);

    // Arrows: tiles reach up to two tiles away, so check a 5x5 neighbourhood.
    float2 tile = floor(pixel / tileSize);
    float lineMask = 0.0;
    float headMask = 0.0;
    for (int y = -2; y <= 2; ++y) {
        for (int x = -2; x <= 2; ++x) {
            float2 centre = (tile + float2(x, y) + 0.5) * tileSize;
            float2 m = LoadMotion(centre);
            if (NonFinite(m) || all(m == float2(2.0, 2.0))) continue;
            float2 travel = m * displaySize * arrowGain;
            float len = length(travel);
            if (len < 1.0) continue;
            travel *= min(len, tileSize * 2.0) / len;
            float d = SegmentDistance(pixel, centre - travel, centre);
            lineMask = max(lineMask, saturate(1.5 - d));
            headMask = max(headMask, saturate(2.5 - length(pixel - centre)));
        }
    }
    result = lerp(result, 1.0.xxx, lineMask);
    result = lerp(result, 0.0.xxx, headMask);
    present[id.xy] = float4(result, scene.a);
}
