// Rain: world-anchored 3D falling streaks, drawn after the scene so they can
// depth-test against it.
//
// No CPU particle list and no per-drop state. Each drop's position is a pure
// function of its instance id and the clock, so the whole volume is one
// DrawInstanced with nothing uploaded per frame. That is what makes tens of
// thousands of drops affordable next to the existing impact particles, which
// are simulated on the CPU and capped at 1024.
//
// Every drop lives in one fixed level-sized world domain. Apart from the view
// projection needed to draw the scene, no camera value enters this shader.

cbuffer RainConstants : register(b0) {
    float4x4 viewProjection;
    float3   lightDirection;
    float    time;
    float3   windVelocity;
    float    intensity;      // 0 = clear, 1 = downpour
    float3   tint;
    float    fallSpeed;
    float    worldExtentX;
    float    worldExtentZ;
    float    dropLength;     // metres, stretched along travel
    float    dropRadius;
    float    opacity;
    float    worldBottom;
    float    worldHeight;
    float    padding;
};

struct RainVertex {
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
    float  fade     : TEXCOORD1;
    float3 worldPos : TEXCOORD2;
    float3 normal   : TEXCOORD3;
};

// Positive modulo. HLSL's fmod keeps the sign of its first operand, so a drop
// whose fall has carried it far negative wraps to a negative offset and lands
// below the volume instead of back at the top. Adding a fixed multiple of the
// period does not save it: the fall grows without bound, so any constant offset
// is eventually outrun. This always returns [0, period).
float PositiveMod(float value, float period) {
    return value - period * floor(value / period);
}

uint HashU32(uint value) {
    value ^= value >> 16;
    value *= 0x7feb352du;
    value ^= value >> 15;
    value *= 0x846ca68bu;
    return value ^ (value >> 16);
}

float Hash01(uint value) {
    return float(HashU32(value) & 0x00ffffffu) / 16777216.0;
}

float3 DropHash(uint stream) {
    const uint seed = HashU32(stream * 0xc2b2ae35u);
    return float3(Hash01(seed), Hash01(seed ^ 0x68bc21ebu),
                  Hash01(seed ^ 0x02e5be93u));
}

RainVertex VSMain(uint vertexId : SV_VertexID, uint instanceId : SV_InstanceID) {
    const float3 h = DropHash(instanceId + 1u);

    // Fall. Speed varies per drop so the sheet does not move as one rigid
    // block, which is what makes rain read as many independent drops.
    const float speed = fallSpeed * (0.8 + h.z * 0.4);
    const float2 worldSize = 2.0 * float2(worldExtentX, worldExtentZ);
    const float2 windOffset = windVelocity.xz * time;
    float3 world;
    world.x = -worldExtentX + PositiveMod(
        h.x * worldSize.x + windOffset.x, worldSize.x);
    world.z = -worldExtentZ + PositiveMod(
        h.z * worldSize.y + windOffset.y, worldSize.y);
    world.y = worldBottom + PositiveMod(
        h.y * worldHeight - time * speed, worldHeight);

    // Build a real triangular prism around the travel vector. Its faces remain
    // world-oriented as the camera turns; this is the key difference from the
    // former billboard quad.
    const float3 travel = normalize(float3(windVelocity.x, -speed, windVelocity.z));
    const float3 reference = abs(travel.y) < 0.95
        ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    const float3 axis0 = normalize(cross(reference, travel));
    const float3 axis1 = normalize(cross(travel, axis0));
    const uint face = vertexId / 6u;
    const uint faceVertex = vertexId % 6u;
    const uint cornerMap[6] = { 0u, 1u, 2u, 1u, 3u, 2u };
    const uint corner = cornerMap[faceVertex];
    const bool tail = corner == 1u || corner == 3u;
    const bool secondRadius = corner >= 2u;
    const float angle0 = 2.0943951 * float(face);
    const float angle1 = 2.0943951 * float((face + 1u) % 3u);
    const float3 radius0 = cos(angle0) * axis0 + sin(angle0) * axis1;
    const float3 radius1 = cos(angle1) * axis0 + sin(angle1) * axis1;
    const float3 radius = secondRadius ? radius1 : radius0;

    // Longer, thinner streaks in heavier rain: a downpour reads as lines, a
    // drizzle as specks.
    const float length_ = dropLength * (0.6 + intensity * 0.8) * (0.7 + h.x * 0.6);
    const float3 offset = -travel * (tail ? length_ : 0.0) +
                          radius * dropRadius;

    RainVertex output;
    output.position = mul(float4(world + offset, 1.0), viewProjection);
    output.uv = float2(tail ? 1.0 : 0.0, 0.0);
    output.worldPos = world + offset;
    output.normal = normalize(radius0 + radius1);
    // The only fade is against the fixed level-domain edge, never the camera.
    const float edgeDistance = min(worldExtentX - abs(world.x),
                                   worldExtentZ - abs(world.z));
    output.fade = smoothstep(0.0, 8.0, edgeDistance);
    return output;
}

float4 PSMain(RainVertex input) : SV_Target {
    // Taper along the streak: bright at the head, fading to nothing at the
    // tail, which is how a falling drop smears over an exposure.
    const float head = 1.0 - input.uv.x;
    // World-space sun/moon lighting keeps all three prism faces distinct while
    // remaining unchanged when the camera moves or turns.
    const float facing = abs(dot(normalize(input.normal),
                                 normalize(lightDirection)));
    const float surface = 0.32 + 0.68 * sqrt(saturate(facing));
    const float alpha = (0.22 + 0.78 * head * head) * surface * input.fade *
                        opacity * intensity;
    if (alpha < 0.004) discard;
    return float4(tint * alpha, alpha);
}
