// Grass vertex shader: one instanced blade, bent by the wind on the GPU.
//
// This is how a real engine draws foliage, and it took two rewrites to get here:
//
//   1. The blades started out re-simulated on the CPU every frame and streamed
//      into an upload buffer. That cost ~11 ms/frame -- more than the entire rest
//      of the scene -- because the wind, bend and normal of every blade near the
//      camera were recomputed in scalar C++ and written back to memory.
//
//   2. Moving the wind into this shader made the geometry static, but the field
//      was still 178k blades' worth of UNIQUE vertices (1.4M of them, ~60 MB),
//      and pushing all that through the vertex fetch was its own bottleneck.
//
// So now the mesh is ONE authored blade -- 10 vertices, 24 indices -- drawn 178k times with
// DrawIndexedInstanced. Everything that makes a blade its own blade (where it is,
// how tall, which way it leans and faces) lives in a structured buffer indexed by
// SV_InstanceID. The vertex buffer is a few hundred bytes; the instance buffer is
// ~3 MB. The GPU reads each blade's data once per instance instead of refetching
// it across eight duplicated vertices.
//
// The wind function must stay in step with GrassField::WindAt on the CPU side.

cbuffer MatrixBuffer : register(b0) {
    matrix model;
    matrix view;
    matrix projection;
    matrix lightSpaceMatrix;
};

// Root constants (b6). The raster path never used these -- they exist for the
// terrain mesh shader -- so borrowing them costs no root-signature change.
cbuffer GrassParams : register(b6) {
    float gTime;
    float gWindStrength;
    float gWindSpeed;
    float gEyeX;
    float gEyeZ;
    float gDrawDistance;
    float gFadeBand;
    uint  gFirstBlade;   // index of this patch's first blade in the instance buffer
    float gHelicopterX;
    float gHelicopterZ;
    float gHelicopterWindRadius;
    float gHelicopterWindStrength;
    float gPixelWorldScale;
};

// One blade. Matches GrassField::BladeInstance exactly.
struct BladeInstance {
    float3 root;     // world position of the blade's base
    float  height;
    float2 dir;      // the blade's facing: its width axis
    float2 lean;     // resting lean, as a tip offset in blade-height units
    float  width;
    float  phase;    // so gusts do not hit every blade in a tuft identically
    float2 pad;
};

// Root SRV at t6, visible to all stages. Shared with the mesh-shader path, which
// never draws at the same time as the grass.
StructuredBuffer<BladeInstance> blades : register(t6);

// Normalized shape from models/grass2/grass/allGrass_001.obj.
struct VS_INPUT {
    float4 shape : POSITION; // (t, side, authored forward curve, width scale)
    uint   iid    : SV_InstanceID;
};

struct VS_OUTPUT {
    float4 position          : SV_POSITION;
    float3 fragPos           : TEXCOORD0;
    float3 normal            : TEXCOORD1;
    float2 texCoord          : TEXCOORD2;
    float4 tangent           : TEXCOORD3;
    float4 fragPosLightSpace : TEXCOORD4;
    float  colorVariation    : TEXCOORD5;
};

// Gusts travelling across the field. Two crossing waves, so the wind sweeps over
// the grass instead of every blade pulsing in unison. Mirrors GrassField::WindAt.
float WindAt(float x, float z, float phase) {
    float t = gTime * gWindSpeed;
    float a = sin((x + z) * 0.18 + t + phase);
    float b = sin((x * 0.31 - z * 0.13) + t * 0.63);
    // Biased positive: real wind blows one way and gusts on top of that, rather
    // than swinging symmetrically back and forth.
    return (0.55 + 0.45 * a) * (0.7 + 0.3 * b);
}

VS_OUTPUT main(VS_INPUT input) {
    VS_OUTPUT output;

    // SV_InstanceID does NOT include DrawIndexedInstanced's StartInstanceLocation
    // -- that only offsets instance-rate VERTEX buffers, and the per-blade data is
    // a structured buffer. So each cell's base index has to be added by hand, or
    // every patch would read the same blades from the front of the buffer.
    const BladeInstance b = blades[gFirstBlade + input.iid];

    const float t    = input.shape.x;   // 0 at root, 1 at tip
    const float side = input.shape.y;   // -1 or +1 across the blade
    const float authoredForward = input.shape.z;
    const float authoredWidth = input.shape.w;

    // Blades shrink to nothing as they approach the draw radius, so distant grass
    // costs no pixels and the field has no hard edge. Cells beyond the radius are
    // never submitted at all (GrassField::GetVisible); this only smooths the seam.
    const float2 toEye = b.root.xz - float2(gEyeX, gEyeZ);
    const float fade = saturate((gDrawDistance - length(toEye)) / max(gFadeBand, 1e-3));

    // Prevailing wind direction, wandering slowly so the field never settles into
    // an obviously repeating pattern.
    const float dirAng = sin(gTime * 0.07) * 0.5;
    const float2 windDir = float2(cos(dirAng), sin(dirAng));

    const float bend = WindAt(b.root.x, b.root.z, b.phase) * gWindStrength;
    const float2 fromHelicopter = b.root.xz - float2(gHelicopterX, gHelicopterZ);
    const float helicopterDistance = length(fromHelicopter);
    const float helicopterFalloff = pow(saturate(
        1.0 - helicopterDistance / max(gHelicopterWindRadius, 1e-3)), 0.65);
    const float2 helicopterDirection = helicopterDistance > 1e-3
        ? fromHelicopter / helicopterDistance : float2(1.0, 0.0);
    const float rotorPulse = 0.88 + 0.12 * sin(
        gTime * 22.0 + helicopterDistance * 1.7 + b.phase);

    // Total tip displacement, as a FRACTION of blade height: the resting lean plus
    // the wind pushing along the prevailing direction. Clamped, because a tip that
    // travels further than the blade is long has nowhere to bend to, and the droop
    // term below would fold it through its own root.
    float2 tip = b.lean + windDir * bend + helicopterDirection *
        (helicopterFalloff * gHelicopterWindStrength * rotorPulse);
    const float tipLen = length(tip);
    const float kMaxBend = 0.97;
    if (tipLen > kMaxBend) tip *= kMaxBend / tipLen;

    // Hinged at the root: displacement grows as t^2, so the base stays planted and
    // the blade curves over rather than shearing rigidly.
    const float2 forwardDir = float2(-b.dir.y, b.dir.x);
    const float2 dynamicOff = tip * (t * t);
    const float2 off = forwardDir * authoredForward + dynamicOff;

    // Bending shortens the blade's vertical reach -- without this the grass
    // stretches as it leans.
    const float droop = sqrt(max(0.0, 1.0 - dot(dynamicOff, dynamicOff)));

    const float h = b.height * fade;
    // MSAA resolves sample coverage; it cannot recover animated geometry that
    // is narrower than a sample footprint. Keep the blade body just over one
    // pixel wide at distance, then retain the authored taper toward its tip.
    const float minHalfWidth = length(toEye) * gPixelWorldScale * 0.55;
    const float rasterHalfWidth = max(b.width, minHalfWidth);
    const float w = rasterHalfWidth * authoredWidth;

    float3 pos;
    pos.x = b.root.x + b.dir.x * w * side + off.x * h;
    pos.z = b.root.z + b.dir.y * w * side + off.y * h;
    pos.y = b.root.y + h * t * droop;

    // The blade's normal has to follow the bend, or a field that is visibly leaning
    // stays lit as though it were standing straight up. Crossing the blade's facing
    // with the direction it currently leans gives the face it presents.
    const float3 sideVec = float3(b.dir.x, 0.0, b.dir.y);
    const float3 up = normalize(float3(tip.x + forwardDir.x * 0.88, 1.0,
                                       tip.y + forwardDir.y * 0.88));
    float3 nrm = cross(sideVec, up);
    if (dot(nrm, nrm) < 1e-6) nrm = float3(0.0, 1.0, 0.0);
    // Tilt skyward: a field of near-vertical cards goes black under a high sun.
    nrm = normalize(normalize(nrm) + float3(0.0, 0.9, 0.0));

    float4 worldPos = float4(pos, 1.0);      // blades are built in world space
    output.fragPos = worldPos.xyz;
    output.normal = nrm;
    output.tangent = float4(sideVec, 1.0);
    output.texCoord = float2(side * 0.5 + 0.5, t);
    // phase is stable random data generated per blade in [-0.3, 0.3]. Reuse it
    // for color so variation does not swim, stripe, or require another buffer.
    output.colorVariation = saturate(b.phase / 0.6 + 0.5);

    float4 viewPos = mul(worldPos, view);
    output.position = mul(viewPos, projection);
    output.fragPosLightSpace = mul(worldPos, lightSpaceMatrix);

    return output;
}
