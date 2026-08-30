struct WaterInteractionGPU
{
    float4 positionRadius;
    float4 velocityType;
};

cbuffer UltraWaterConstants : register(b0)
{
    float4 bounds;
    float4 simulation;
    float4 dispatch;
    float4 tuning0; // height, wavelength scale, speed, direction radians
    float4 tuning1; // choppiness, surf strength, foam, coast damping
    float4 spectralWaves[16];
    float4 spectralExtra[16];
    WaterInteractionGPU interactions[16];
};

Texture2D<float> bathymetry : register(t0);
Texture2D<float4> coastInput : register(t1);
Texture2DArray<float4> spectrumInput : register(t2);
RWTexture2D<float4> coastOutput : register(u0);
RWTexture2DArray<float4> spectrumOutput : register(u1);
RWStructuredBuffer<float4> heightQueryOutput : register(u0);
SamplerState linearWrap : register(s0);

static const float kCascadePeriods[3] = { 48.0, 192.0, 768.0 };

float CascadeWeight(float wavelength, uint cascade)
{
    if (cascade == 0) return 1.0 - smoothstep(4.5, 7.0, wavelength);
    if (cascade == 1)
        return smoothstep(4.5, 7.0, wavelength) *
               (1.0 - smoothstep(13.0, 18.0, wavelength));
    return smoothstep(13.0, 18.0, wavelength);
}

[numthreads(8, 8, 1)]
void SpectrumCS(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= 256 || id.y >= 256 || id.z >= 3) return;
    const float period = kCascadePeriods[id.z];
    const float2 world = (float2(id.xy) + 0.5) * (period / 256.0);
    const float gravity = 9.81;
    float3 displacement = 0.0;
    float compression = 0.0;
    [unroll]
    for (uint i = 0; i < 16; ++i) {
        const float4 wave = spectralWaves[i];
        const float wavelength = wave.w * tuning0.y;
        const float amplitude = wave.z * tuning0.x;
        const float weight = CascadeWeight(wavelength, id.z);
        const float k = 6.28318530718 / max(wavelength, 0.1);
        const float omega = sqrt(gravity * k);
        const float sineDirection = sin(tuning0.w);
        const float cosineDirection = cos(tuning0.w);
        const float2 direction = float2(
            wave.x * cosineDirection - wave.y * sineDirection,
            wave.x * sineDirection + wave.y * cosineDirection);
        const float phase = k * dot(direction, world) -
            omega * simulation.w * tuning0.z + spectralExtra[i].x;
        const float sine = sin(phase);
        const float cosine = cos(phase);
        const float horizontal =
            spectralExtra[i].y * amplitude * tuning1.x;
        displacement.xz += direction * horizontal * cosine * weight;
        displacement.y += amplitude * sine * weight;
        compression += horizontal * k * sine * weight;
    }
    spectrumOutput[id] = float4(displacement, saturate(compression * 2.2));
}

float SampleBed(float2 uv)
{
    const uint resolution = (uint)simulation.y;
    const int2 pixel = int2(clamp(
        uv * resolution, 0.0, (float)resolution - 1.0));
    return bathymetry.Load(int3(pixel, 0));
}

float DeepHeight(float2 world)
{
    float height = 0.0;
    [unroll]
    for (uint cascade = 0; cascade < 3; ++cascade) {
        const float2 uv = frac(world / kCascadePeriods[cascade]);
        height += spectrumInput.SampleLevel(
            linearWrap, float3(uv, cascade), 0.0).y;
    }
    return height;
}

[numthreads(8, 8, 1)]
void CoastCS(uint3 id : SV_DispatchThreadID)
{
    const uint resolution = (uint)simulation.x;
    if (id.x >= resolution || id.y >= resolution) return;
    const int2 p = int2(id.xy);
    const int2 limit = int2(resolution - 1, resolution - 1);
    const float2 span = bounds.zw - bounds.xy;
    const float2 cell = span / resolution;
    const float2 uv = (float2(id.xy) + 0.5) / resolution;
    const float2 world = lerp(bounds.xy, bounds.zw, uv);
    const float bed = SampleBed(uv);
    if (dispatch.x > 0.5) {
        coastOutput[p] = 0.0;
        return;
    }

    const float4 center = coastInput.Load(int3(p, 0));
    const float4 left = coastInput.Load(int3(clamp(p + int2(-1, 0), 0, limit), 0));
    const float4 right = coastInput.Load(int3(clamp(p + int2(1, 0), 0, limit), 0));
    const float4 down = coastInput.Load(int3(clamp(p + int2(0, -1), 0, limit), 0));
    const float4 up = coastInput.Load(int3(clamp(p + int2(0, 1), 0, limit), 0));
    float eta = center.x;
    float2 velocity = center.yz;
    const float dt = simulation.z;
    const float2 gradient = float2(
        (right.x - left.x) / max(2.0 * cell.x, 1e-4),
        (up.x - down.x) / max(2.0 * cell.y, 1e-4));
    const float restDepth = max(-bed, 0.0);
    const float waterDepth = max(eta - bed, 0.0);
    velocity -= 9.81 * gradient * dt;
    const float divergence =
        (right.y - left.y) / max(2.0 * cell.x, 1e-4) +
        (up.z - down.z) / max(2.0 * cell.y, 1e-4);
    eta -= waterDepth * divergence * dt;

    const float deepWave = DeepHeight(world);
    const float boundary = smoothstep(8.0, 14.0, restDepth);
    eta = lerp(eta, deepWave,
        saturate(boundary * dt * 3.5 * tuning1.y));
    velocity *= exp(-dt * lerp(2.8, 0.10,
        saturate(restDepth / 5.0)) * tuning1.w);
    if (dispatch.y > 0.5) {
        [loop]
        for (uint i = 0; i < min((uint)dispatch.z, 16u); ++i) {
            const float2 delta = world - interactions[i].positionRadius.xy;
            const float radius = max(interactions[i].positionRadius.z, 0.05);
            const float impulse = exp(-dot(delta, delta) / (radius * radius));
            eta += impulse * interactions[i].positionRadius.w;
            velocity += impulse * interactions[i].velocityType.xy;
        }
    }

    const float updatedDepth = eta - bed;
    if (updatedDepth <= 0.02 && bed >= 0.0) {
        eta = 0.0;
        velocity = 0.0;
    }
    eta = clamp(eta, -1.25, 1.25);
    const float2 backtracedUV = saturate(
        uv - velocity * dt / max(span, 0.001));
    const int2 backtracedPixel = int2(clamp(
        backtracedUV * resolution, 0.0, (float)resolution - 1.0));
    const float advectedFoam =
        coastInput.Load(int3(backtracedPixel, 0)).w;
    const float neighbourFoam = max(max(left.w, right.w), max(down.w, up.w));
    const float breakerRatio = abs(eta) / max(restDepth, 0.04);
    const float compression = saturate(-divergence * 0.22);
    const float breaking = smoothstep(0.55, 0.78, breakerRatio) *
        (1.0 - smoothstep(2.5, 5.0, restDepth));
    float foam = max(advectedFoam, neighbourFoam * 0.985);
    foam = max(foam, saturate((breaking + compression) * tuning1.z));
    foam *= exp(-dt * 0.42);
    coastOutput[p] = float4(eta, velocity, saturate(foam));
}

float QuerySurfaceHeight(float2 world)
{
    const float2 uv = saturate((world - bounds.xy) /
        max(bounds.zw - bounds.xy, 0.001));
    const uint resolution = (uint)simulation.x;
    const int2 pixel = int2(clamp(
        uv * resolution, 0.0, (float)resolution - 1.0));
    const float eta = coastInput.Load(int3(pixel, 0)).x;
    const float bed = SampleBed(uv);
    const float coastWeight = 1.0 - smoothstep(8.0, 14.0, max(-bed, 0.0));
    return dispatch.w + lerp(DeepHeight(world), eta, coastWeight);
}

[numthreads(16, 1, 1)]
void HeightQueryCS(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= min((uint)dispatch.z, 16u)) return;
    const float2 world = interactions[id.x].positionRadius.xy;
    const float stepSize = max(
        max(bounds.z - bounds.x, bounds.w - bounds.y) / simulation.x, 0.2);
    const float height = QuerySurfaceHeight(world);
    const float slopeX = (QuerySurfaceHeight(
        world + float2(stepSize, 0.0)) - QuerySurfaceHeight(
        world - float2(stepSize, 0.0))) / (2.0 * stepSize);
    const float slopeZ = (QuerySurfaceHeight(
        world + float2(0.0, stepSize)) - QuerySurfaceHeight(
        world - float2(0.0, stepSize))) / (2.0 * stepSize);
    heightQueryOutput[id.x] = float4(height, slopeX, slopeZ, 1.0);
}
