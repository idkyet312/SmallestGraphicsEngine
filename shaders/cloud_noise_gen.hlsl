// Generates the two 3D noise volumes the cloud raymarch samples, following the
// Horizon Zero Dawn / Nubis structure (Schneider & Vos, SIGGRAPH 2015).
//
// Baked once at startup into GPU textures rather than evaluated per raymarch
// step. That is the whole point: a cloud march takes 64-128 density samples per
// pixel, and each would otherwise cost several octaves of procedural noise. One
// texture fetch replaces all of it.
//
// Two volumes, matching the presentation:
//   Shape 128^3  R = Perlin-Worley, GBA = Worley at rising frequencies.
//                Builds the overall cloud form.
//   Detail 32^3  RGB = Worley at rising frequencies. Erodes the shape's edges
//                into wisps; without it clouds read as smooth blobs.
//
// Both tile seamlessly, so the march can repeat them across the sky without a
// visible join.

RWTexture3D<float4> outputVolume : register(u0);

cbuffer NoiseGenConstants : register(b0) {
    uint  resolution;    // edge length of the volume being written
    uint  isDetail;      // 0 = shape volume, 1 = detail volume
    float padding0;
    float padding1;
};

// -- Hashing ------------------------------------------------------------------
// Tiling matters: every lookup wraps its cell against the period, so a cell
// stepping off one face reads the cell on the opposite face and the volume
// repeats without a seam.
float3 Hash33(float3 p) {
    p = float3(dot(p, float3(127.1, 311.7, 74.7)),
               dot(p, float3(269.5, 183.3, 246.1)),
               dot(p, float3(113.5, 271.9, 124.6)));
    return frac(sin(p) * 43758.5453123);
}

float Hash13(float3 p) {
    p = frac(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return frac((p.x + p.y) * p.z);
}

// -- Worley (cellular) noise --------------------------------------------------
// Distance to the nearest of a set of scattered feature points, inverted so
// dense cores read high and the gaps between cells read low. This is what gives
// clouds their billowing, cauliflower structure -- Perlin alone looks like fog.
float WorleyTiling(float3 uv, float frequency) {
    const float3 scaled = uv * frequency;
    const float3 cell = floor(scaled);
    const float3 local = scaled - cell;

    float nearest = 1.0;
    [unroll] for (int z = -1; z <= 1; ++z)
    [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x) {
        const float3 neighbour = float3(x, y, z);
        // Wrap the neighbour cell into the period so the volume tiles.
        float3 wrapped = cell + neighbour;
        wrapped = wrapped - floor(wrapped / frequency) * frequency;
        const float3 point_ = neighbour + Hash33(wrapped) - local;
        nearest = min(nearest, dot(point_, point_));
    }
    // Squared distances above; one sqrt at the end rather than 27 of them.
    return 1.0 - saturate(sqrt(nearest));
}

// Several octaves of Worley summed at halving amplitude, which adds the finer
// billows riding on the large ones.
float WorleyFBM(float3 uv, float frequency) {
    return WorleyTiling(uv, frequency) * 0.625 +
           WorleyTiling(uv, frequency * 2.0) * 0.25 +
           WorleyTiling(uv, frequency * 4.0) * 0.125;
}

// -- Perlin noise -------------------------------------------------------------
float PerlinTiling(float3 uv, float frequency) {
    const float3 scaled = uv * frequency;
    const float3 cell = floor(scaled);
    float3 local = scaled - cell;
    // Quintic fade: continuous second derivative, so the interpolation does not
    // leave visible creases along cell boundaries.
    local = local * local * local * (local * (local * 6.0 - 15.0) + 10.0);

    float corners[8];
    [unroll] for (int i = 0; i < 8; ++i) {
        const float3 offset = float3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        float3 wrapped = cell + offset;
        wrapped = wrapped - floor(wrapped / frequency) * frequency;
        corners[i] = Hash13(wrapped);
    }
    const float x00 = lerp(corners[0], corners[1], local.x);
    const float x10 = lerp(corners[2], corners[3], local.x);
    const float x01 = lerp(corners[4], corners[5], local.x);
    const float x11 = lerp(corners[6], corners[7], local.x);
    return lerp(lerp(x00, x10, local.y), lerp(x01, x11, local.y), local.z);
}

float PerlinFBM(float3 uv, float frequency) {
    return PerlinTiling(uv, frequency) * 0.5 +
           PerlinTiling(uv, frequency * 2.0) * 0.25 +
           PerlinTiling(uv, frequency * 4.0) * 0.125 +
           PerlinTiling(uv, frequency * 8.0) * 0.0625;
}

[numthreads(8, 8, 8)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (id.x >= resolution || id.y >= resolution || id.z >= resolution) return;
    // Sample at voxel centres so the volume stays symmetric under tiling.
    const float3 uv = (float3(id) + 0.5) / float(resolution);

    if (isDetail) {
        // Detail volume: three Worley frequencies, no Perlin. This only ever
        // subtracts from the shape, so it needs structure rather than form.
        outputVolume[id] = float4(
            WorleyFBM(uv, 4.0),
            WorleyFBM(uv, 8.0),
            WorleyFBM(uv, 16.0),
            1.0);
        return;
    }

    // Shape volume. The R channel is the Perlin-Worley mix that carries the
    // cloud's overall form; remapping Perlin by the Worley field is what gives
    // it connected billows instead of the smooth blobs Perlin alone produces.
    const float perlin = PerlinFBM(uv, 4.0);
    const float worley = WorleyFBM(uv, 6.0);
    // Remap: where Worley is low the Perlin value is pulled down, carving the
    // gaps between billows.
    const float perlinWorley = saturate(
        (perlin - (1.0 - worley)) / max(worley, 0.001));

    outputVolume[id] = float4(
        perlinWorley,
        WorleyFBM(uv, 6.0),
        WorleyFBM(uv, 12.0),
        WorleyFBM(uv, 24.0));
}
