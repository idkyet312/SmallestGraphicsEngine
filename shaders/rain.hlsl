// Rain: a camera-following volume of falling streaks, drawn after the scene so
// it can depth-test against it.
//
// No CPU particle list and no per-drop state. Each drop's position is a pure
// function of its instance id and the clock, so the whole volume is one
// DrawInstanced with nothing uploaded per frame. That is what makes tens of
// thousands of drops affordable next to the existing impact particles, which
// are simulated on the CPU and capped at 1024.
//
// The volume is a box that travels with the camera. A drop that falls out of
// the bottom reappears at the top, and one that leaves the side reappears
// opposite, so a finite set of drops tiles an unbounded rainfall. The player
// can never reach an edge because the box moves with them.

cbuffer RainConstants : register(b0) {
    float4x4 viewProjection;
    float3   cameraPosition;
    float    time;
    float3   cameraRight;
    float    intensity;      // 0 = clear, 1 = downpour
    float3   cameraUp;
    float    fallSpeed;
    float3   windVelocity;   // world units/sec, drifts the whole volume
    float    boxExtent;      // half-width of the wrapping volume, metres
    float3   tint;
    float    dropLength;     // metres, stretched along travel
    float    dropWidth;
    float    boxHeight;
    float    opacity;
    float    padding;
};

struct RainVertex {
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
    float  fade     : TEXCOORD1;
};

// Positive modulo. HLSL's fmod keeps the sign of its first operand, so a drop
// whose fall has carried it far negative wraps to a negative offset and lands
// below the volume instead of back at the top. Adding a fixed multiple of the
// period does not save it: the fall grows without bound, so any constant offset
// is eventually outrun. This always returns [0, period).
float PositiveMod(float value, float period) {
    return value - period * floor(value / period);
}

// Cheap per-drop hash. Three decorrelated values from one instance id.
float3 Hash31(uint id) {
    float3 p = frac(float3(id * 0.1031, id * 0.1030, id * 0.0973));
    p += dot(p, p.yzx + 33.33);
    return frac((p.xxy + p.yzz) * p.zyx);
}

RainVertex VSMain(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID) {
    const float3 h = Hash31(instanceId + 1u);

    // Where this drop sits in the volume, before wrapping. The volume is
    // anchored to the camera but snapped so drops do not visibly slide when the
    // camera moves -- the wrap below does the work of keeping them in view.
    float3 origin = (h - 0.5) * float3(2.0 * boxExtent, 0.0, 2.0 * boxExtent);
    origin.y = h.y * boxHeight;

    // Fall. Speed varies per drop so the sheet does not move as one rigid
    // block, which is what makes rain read as many independent drops.
    const float speed = fallSpeed * (0.8 + h.z * 0.4);
    const float fallen = time * speed;
    float3 world = origin + cameraPosition;
    world.y = origin.y + cameraPosition.y - fallen;
    world += windVelocity * time;

    // Wrap into a box centred on the camera, so the volume follows without any
    // drop needing to be respawned. All three axes drift without bound -- the
    // fall on y, the wind on x and z -- so every one needs the sign-safe wrap.
    float3 relative = world - cameraPosition;
    relative.x = PositiveMod(relative.x + boxExtent, boxExtent * 2.0) - boxExtent;
    relative.z = PositiveMod(relative.z + boxExtent, boxExtent * 2.0) - boxExtent;
    // Vertical wrap is offset so the volume sits mostly above the camera:
    // rain arrives from overhead, and drops spawning below eye level look wrong.
    relative.y = PositiveMod(relative.y, boxHeight) - boxHeight * 0.25;
    world = cameraPosition + relative;

    // Build the streak. Each drop is a quad stretched along its travel
    // direction -- gravity plus wind -- rather than screen-vertical, so wind
    // visibly slants the rain instead of only translating it.
    const float3 travel = normalize(float3(windVelocity.x, -speed, windVelocity.z));
    // Face the camera by taking the axis perpendicular to both travel and view.
    float3 side = cross(travel, normalize(cameraPosition - world));
    const float sideLength = length(side);
    side = sideLength > 1e-4 ? side / sideLength : cameraRight;

    // Two triangles, six vertices, as a unit quad in (along, across).
    const float2 corners[6] = {
        float2(0.0, -1.0), float2(1.0, -1.0), float2(0.0, 1.0),
        float2(1.0, -1.0), float2(1.0, 1.0), float2(0.0, 1.0)
    };
    const float2 corner = corners[vertexId];

    // Longer, thinner streaks in heavier rain: a downpour reads as lines, a
    // drizzle as specks.
    const float length_ = dropLength * (0.6 + intensity * 0.8) * (0.7 + h.x * 0.6);
    const float3 offset = travel * (corner.x * -length_) +
                          side * (corner.y * dropWidth);

    RainVertex output;
    output.position = mul(float4(world + offset, 1.0), viewProjection);
    output.uv = corner;
    // Fade drops approaching the edge of the volume so they do not pop as they
    // wrap. Horizontal distance only -- a vertical fade would thin the rain
    // directly overhead, where it should be densest.
    const float horizontal = length(relative.xz) / boxExtent;
    output.fade = saturate(1.0 - horizontal * horizontal);
    return output;
}

float4 PSMain(RainVertex input) : SV_Target {
    // Taper along the streak: bright at the head, fading to nothing at the
    // tail, which is how a falling drop smears over an exposure.
    const float head = 1.0 - input.uv.x;
    const float across = 1.0 - abs(input.uv.y);
    const float alpha = head * head * across * input.fade * opacity * intensity;
    if (alpha < 0.004) discard;
    return float4(tint * alpha, alpha);
}
