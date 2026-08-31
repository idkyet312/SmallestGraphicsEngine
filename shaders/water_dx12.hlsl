cbuffer WaterConstants : register(b0)
{
    matrix viewProjection;
    matrix previousViewProjection;
    matrix inverseViewProjection;
    float4 cameraTime;
    float4 previousCameraTime;
    float4 screenParams;
    float4 volume0;       // center.x, surfaceY, center.z, isOcean
    float4 volume1;       // halfX, halfZ, deployment mode (2 = depth debug), quality
    float4 clipmapParams; // snappedCenter.xz, micro strength, roughness
    float4 opticalParams; // foam depth, crest threshold, max refraction px, SSR strength
    float4 absorption;
    float4 shallowScatter;
    float4 deepScatter;
    float4 lightDirection; // xyz direction, w night reflection intensity
    float4 lightColor;     // rgb direct light, w night water intensity
    float4 waves[8];      // direction.xy, amplitude, wavelength
    float4 waveExtra[8];  // steepness, unused...
    float4 ultraBounds;     // minimum xz, maximum xz
    float4 ultraSimulation; // coast resolution, bathy resolution, bathy ready, Ultra active
    float4 ultraDebug;      // diagnostic mode and reserved values
    // High-path wave sliders. Amplitude and wavelength are folded into the
    // uploaded wave constants on the CPU; these are the two that cannot be,
    // because they scale the time fed to the wave field and the per-pixel
    // capillary detail rather than the spectrum itself.
    float4 highWaveParams;  // speed, micro detail, foam strength, shore model
    // High-path shore shallowing: x = flatten weight (0 off, 1 full shoaling
    // and breaker cap). yzw reserved.
    float4 highShoreParams;
};

// Number of Gerstner trains. Must match OceanWaveSettings::WaveCount; the
// static_assert on the C++ Constants struct catches a mismatch at build time.
static const uint OceanWaveCount = 8;

// How far the distant ocean may be blended toward the fogged sky behind it.
// Strictly below 1 so the horizon band keeps some water shading instead of
// resolving to the sky texture, which reads as a gap above the waterline.
static const float kHorizonBlendCeiling = 0.82;

Texture2D<float4> sceneColor : register(t0);
#ifdef WATER_DEPTH_MSAA
Texture2DMS<float> sceneDepth : register(t1);
#else
Texture2D<float> sceneDepth : register(t1);
#endif
Texture2D<float4> environmentMap : register(t2);
Texture2DArray<float4> ultraSpectrumCurrent : register(t3);
Texture2DArray<float4> ultraSpectrumPrevious : register(t4);
Texture2D<float4> ultraCoastCurrent : register(t5);
Texture2D<float4> ultraCoastPrevious : register(t6);
Texture2D<float> ultraBathymetry : register(t7);
SamplerState linearClamp : register(s0);
SamplerState pointClamp : register(s1);
SamplerState linearWrap : register(s2);

struct VSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 texCoord : TEXCOORD;
    float4 tangent : TANGENT;
};

struct VSOutput
{
    float4 position : SV_POSITION;
    float3 worldPosition : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float crest : TEXCOORD2;
    float4 currentClip : TEXCOORD3;
    float4 previousClip : TEXCOORD4;
    float2 clipmapData : TEXCOORD5; // ring level, radial morph coordinate
    float2 oceanBaseXZ : TEXCOORD6;
};

float Hash21(float2 p)
{
    p = frac(p * float2(123.34, 456.21));
    p += dot(p, p + 45.32);
    return frac(p.x * p.y);
}

float SmoothNoise(float2 p)
{
    float2 cell = floor(p);
    float2 f = frac(p);
    f = f * f * (3.0 - 2.0 * f);
    return lerp(
        lerp(Hash21(cell), Hash21(cell + float2(1.0, 0.0)), f.x),
        lerp(Hash21(cell + float2(0.0, 1.0)),
             Hash21(cell + float2(1.0, 1.0)), f.x), f.y);
}

// Two octaves of the value noise above, remapped to [-1, 1]. Used to knock the
// wave phase off its perfect concentric geometry. It has to be sampled on world
// position rather than on radius and bearing: a polar sample would stretch into
// long spokes far from the island and pinch to nothing near the middle.
float WavePhaseNoise(float2 worldXZ)
{
    float n = SmoothNoise(worldXZ * 0.018) * 0.65 +
              SmoothNoise(worldXZ * 0.047) * 0.35;
    return n * 2.0 - 1.0;
}

// Is the bathymetry field resident and does this point fall inside it? True on
// High as well as Ultra -- the texture is uploaded for both. What each quality
// level does with it differs; see WaterUltraQuality below.
bool WaterBathymetryUV(float2 worldXZ, out float2 uv)
{
    const float2 span = ultraBounds.zw - ultraBounds.xy;
    uv = (worldXZ - ultraBounds.xy) / max(span, 0.001);
    return ultraSimulation.z > 0.5 &&
        all(uv >= 0.0) && all(uv <= 1.0);
}

// Ultra takes the full treatment: phase driven by distance to the shoreline, so
// crests become contours of the coast, plus finite-depth dispersion, shoaling
// and breaking. High only borrows the direction -- each train is turned part of
// the way toward the measured shore normal, so waves visibly answer to the
// coastline while still sweeping across the world on their authored bearings.
// How far is the Shore Refraction slider (highWaveParams.w): 0 keeps the
// authored bearings exactly, 1 locks crests fully onto the coast contours.
// Ultra ignores it and always refracts fully.
bool WaterUltraQuality() { return ultraSimulation.w > 0.5; }

void EvaluateOcean(float2 baseXZ, float time, out float3 position,
                   out float3 normal, out float crest)
{
    const float gravity = 9.81;
    position = float3(baseXZ.x, volume0.y, baseXZ.y);
    float3 tangentX = float3(1.0, 0.0, 0.0);
    float3 tangentZ = float3(0.0, 0.0, 1.0);
    float compression = 0.0;

    // Without bathymetry this is unused: the High path keeps the authored
    // bearings and lets each train sweep across the world, which is what it has
    // always done. Refraction toward the shore only happens on the Ultra path,
    // where a real bed gradient says which way "shoreward" actually is --
    // approximating it from the island centre bent waves around water that has
    // no shore in it.
    float2 shoreward = float2(1.0, 0.0);
    float travelDistance = 0.0;
    float restDepth = 24.0;
    float2 bathymetryUV;
    const bool haveBathymetry = WaterBathymetryUV(baseXZ, bathymetryUV);
    if (haveBathymetry) {
        const float bed = ultraBathymetry.SampleLevel(
            linearClamp, bathymetryUV, 0.0);
        restDepth = max(-bed, 0.02);
        // The CPU derives this bed from signed shoreline distance with separate
        // 0.20 land and 0.35 offshore slopes, after shifting the profile 30 m
        // seaward. Undo both to recover the distance, so each phase contour
        // follows bays and headlands instead of a radial approximation around
        // the island centre. The 30 here must track kShoreOffsetMetres in
        // UltraWaterSimulation.h; if the two disagree the contours sit at the
        // wrong standoff from the coast.
        const float signedShoreDistance =
            (bed >= 0.0 ? bed / 0.20 : bed / 0.35) - 30.0;
        travelDistance = -signedShoreDistance;

        const float2 span = ultraBounds.zw - ultraBounds.xy;
        const float2 texel = 1.0 / max(ultraSimulation.y, 1.0);
        const float2 worldTexel = span * texel;
        const float bedLeft = ultraBathymetry.SampleLevel(
            linearClamp, bathymetryUV - float2(texel.x, 0.0), 0.0);
        const float bedRight = ultraBathymetry.SampleLevel(
            linearClamp, bathymetryUV + float2(texel.x, 0.0), 0.0);
        const float bedDown = ultraBathymetry.SampleLevel(
            linearClamp, bathymetryUV - float2(0.0, texel.y), 0.0);
        const float bedUp = ultraBathymetry.SampleLevel(
            linearClamp, bathymetryUV + float2(0.0, texel.y), 0.0);
        const float2 bedGradient = float2(
            (bedRight - bedLeft) / max(2.0 * worldTexel.x, 1e-4),
            (bedUp - bedDown) / max(2.0 * worldTexel.y, 1e-4));
        if (dot(bedGradient, bedGradient) > 1e-6)
            shoreward = normalize(bedGradient);
    }
    const float2 alongShore = float2(-shoreward.y, shoreward.x);
    const float bearing = atan2(shoreward.y, shoreward.x);

    // Ultra runs the full shoreline-contour phase. High keeps a dominant
    // offshore swell: out at sea every train holds the bearing it was authored
    // with, and the coast only asserts itself as the water shallows. Making the
    // whole ocean trace the island was the thing that looked wrong -- concentric
    // rings out to the horizon, all eight trains converging on one heading.
    //
    // Everything below hangs off one depth ramp, because in reality they are
    // the same phenomenon: a wave starts feeling the bed at roughly half its
    // wavelength, and from that point on it refracts, shortens, steepens and
    // eventually breaks together. Sharing the ramp keeps them in step instead
    // of fading in at unrelated depths.
    //
    // Two independent weights over that one depth ramp. Shore Refraction owns
    // what the waves point at -- bending toward the coast, and the wavefront
    // irregularity that stops the bent crests reading as concentric arcs. Shore
    // Flatten owns how tall they are -- wavelength compression, shoaling gain,
    // steepening and the breaker cap. They are split because they fail
    // differently: refraction is what decorrelates CPU buoyancy (a boat sits on
    // a surface whose crests point elsewhere), while flattening only ever lowers
    // the surface, and only in shallows. So the height half can be left on by
    // default while the bearing half stays opt-in.
    const bool ultraQuality = WaterUltraQuality();
    const float refractStrength = saturate(highWaveParams.w);
    const float flattenStrength = saturate(highShoreParams.x);
    const bool ultraWaveModel = haveBathymetry && ultraQuality;
    // High needs its own irregularity now that it refracts: without it the
    // shoaling crests arrive as clean parallel arcs, which is the concentric
    // look again in a smaller radius. Ultra keeps using it as before.
    const float phaseNoise = haveBathymetry
        ? WavePhaseNoise(baseXZ) : 0.0;
    float breakingEnergy = 0.0;

    [unroll]
    for (uint i = 0; i < OceanWaveCount; ++i) {
        const float2 authored = normalize(waves[i].xy);
        const float deepAmplitude = waves[i].z;
        const float deepWavelength = max(waves[i].w, 0.1);
        const float deepK = 6.28318530718 / deepWavelength;
        // Frequency is the invariant: a wave train entering shallow water keeps
        // its period and pays for it by shortening and slowing down. So omega is
        // computed once from the deep-water dispersion relation and never
        // touched again.
        const float omega = sqrt(gravity * deepK);
        float steepness = waveExtra[i].x;

        // How much this train feels the bed. Deep water is depth > wavelength/2,
        // the standard cutoff; from there it ramps to fully shoaling as the
        // depth runs out. Per-train, so the long swell starts responding far
        // offshore while short chop stays deep-water until it is nearly ashore
        // -- which is why a real coast sorts its waves by period as they come
        // in, long ones turning first.
        const float feelDepth = max(deepWavelength * 0.5, 1.0);
        // One ramp, two weights off it, so both halves fade in at the same
        // depths for a given train and cannot drift out of step.
        const float bedInfluence = haveBathymetry
            ? 1.0 - smoothstep(0.0, feelDepth, restDepth) : 0.0;
        const float refractMix = bedInfluence * refractStrength;
        const float flattenMix = bedInfluence * flattenStrength;

        // Refraction. Offshore the train keeps its authored bearing; as it
        // shoals it turns toward the shore normal. The fan spreads the trains
        // either side of that normal so they converge without collapsing into
        // one parallel front -- keyed on wavelength, since the authored array is
        // not sorted by period and index keying would hand the widest angle to
        // whichever train happened to sit last. Long swell ends up nearest the
        // normal, short chop stays angled.
        const float periodMix = saturate(
            (log2(deepWavelength) - log2(1.5)) / (log2(32.0) - log2(1.5)));
        const float fanSide = (i & 1) ? -1.0 : 1.0;
        const float spread = fanSide * lerp(0.95, 0.18, periodMix);
        const float2 shoreDirection = normalize(shoreward + alongShore * spread);
        float2 direction = authored;
        if (refractMix > 0.001) {
            // Rotate along the shortest arc, not a vector lerp. Lerping two
            // near-opposed unit vectors passes through the degenerate midpoint
            // and sends the train the long way round -- measured swings of
            // -135 and +157 degrees on trains authored pointing away from the
            // beach, which reads as waves whipping round rather than bending
            // in. Taking the signed angle and wrapping it to [-pi, pi] means
            // every train turns the short way and no path is degenerate.
            const float authoredAngle = atan2(authored.y, authored.x);
            const float shoreAngle =
                atan2(shoreDirection.y, shoreDirection.x);
            float delta = shoreAngle - authoredAngle;
            delta -= 6.28318530718 * floor(delta / 6.28318530718 + 0.5);
            // Refraction bends a train toward the normal; it never spins one
            // that is heading out to sea around to face the beach. A train
            // authored offshore has a genuine shortest path of ~135 degrees to
            // shore-normal, and following it looked like waves whipping round.
            // Snell only ever reduces the angle of incidence, so cap the turn
            // at a right angle: trains already heading roughly shoreward bend
            // fully in, oblique ones bend as far as physics allows and no
            // further, and offshore trains are left to run out to sea as they
            // should.
            const float maxTurn = 1.57079632679;
            delta = clamp(delta, -maxTurn, maxTurn);
            const float turned = authoredAngle + delta * refractMix;
            direction = float2(cos(turned), sin(turned));
        }

        // Wavelength compression. Finite-depth dispersion holds omega fixed
        // while the phase speed falls as sqrt(tanh(kh)), so the local wavenumber
        // rises and crests bunch up toward the beach. Ultra runs this from its
        // solver; High now gets the same relation, faded in by flattenMix so deep
        // water is untouched.
        const float tanhTerm = sqrt(max(tanh(deepK * max(restDepth, 0.02)), 0.01));
        const float dispersion = ultraWaveModel
            ? tanhTerm
            : lerp(1.0, tanhTerm, flattenMix);
        const float k = deepK / dispersion;
        const float wavelength = 6.28318530718 / k;
        const float phaseSpeed = omega / k;

        // Shoaling. Energy flux c_g * A^2 is conserved as the group slows, so
        // amplitude rises by sqrt(c_g_deep / c_g_local) -- the reason waves stand
        // up before they break. Clamped: the relation runs away as depth goes to
        // zero, and the breaker limit below is what should end the wave, not a
        // singularity here.
        const float kh = k * max(restDepth, 0.02);
        const float groupFactor = 0.5 *
            (1.0 + 2.0 * kh / max(sinh(2.0 * kh), 1e-3));
        const float deepGroupSpeed = 0.5 * omega / deepK;
        const float localGroupSpeed = phaseSpeed * groupFactor;
        const float shoalGain = clamp(
            sqrt(deepGroupSpeed / max(localGroupSpeed, 0.05)), 0.85, 1.8);
        const float shoaling = ultraWaveModel
            ? shoalGain : lerp(1.0, shoalGain, flattenMix);
        const float shoaledAmplitude = deepAmplitude * shoaling;

        // Breaking. A wave cannot stand taller than roughly 0.39 of the water
        // depth; past that it spills. Capping here is also what stops the
        // shallows creasing into hard polygonal facets -- unbounded amplitude in
        // ankle-deep water folds neighbouring crests through each other and
        // makes the clipmap tessellation visible.
        const float breakerLimit = 0.39 * max(restDepth, 0.02);
        const float breakerCap = max(breakerLimit, 0.01);
        float amplitude;
        if (ultraWaveModel) {
            amplitude = min(shoaledAmplitude, breakerCap);
        } else {
            // Only let the cap bite where the train is actually shoaling, so a
            // deep-water train is never touched by it.
            const float capped = min(shoaledAmplitude, breakerCap);
            amplitude = lerp(shoaledAmplitude, capped, flattenMix);
        }
        // Steepness rises with shoaling too: crests sharpen and troughs flatten
        // as a wave stands up. The existing CPU clamp keeps the sum inside the
        // Gerstner loop threshold, and this stays modest so it cannot breach it.
        steepness *= lerp(1.0, 1.35, flattenMix);
        // How far past the breaker limit this train is trying to stand. Feeds
        // the crest term, which drives foam. High gets it at a reduced weight:
        // it has a real shoaling model now, so it earns a surf line, but not the
        // full whitewater Ultra's solver is tuned for.
        const float overLimit = saturate(
            (shoaledAmplitude - breakerLimit) / max(deepAmplitude, 0.01));
        breakingEnergy += ultraWaveModel
            ? overLimit
            : overLimit * flattenMix * 0.55;

        // Ultra runs the phase on distance to the shoreline, so crests are
        // contours of the coast marching inward, shifted by the wander and
        // noise terms so successive fronts are not identical arcs. High takes
        // the classic projection onto the authored bearing: parallel crests
        // sweeping across the world, travelling with the wave direction.
        float phase;
        if (ultraWaveModel) {
            const float wander = wavelength * 0.85 *
                sin(bearing * (2.0 + float(i)) + authored.x * 3.1);
            // Noise displaces the crest line by a fraction of a wavelength, so
            // wavefronts arrive ragged and slightly out of step along their
            // length. Scaled by wavelength so the long swell bends over
            // hundreds of metres while the chop breaks up over a few.
            const float noiseOffset = phaseNoise * wavelength * 0.42;
            phase = k * (travelDistance + wander * spread * 3.0 +
                         noiseOffset) + omega * time;
        } else {
            // Travelling along the refracted bearing, so as `direction` turns
            // shoreward the crests advance inward rather than being pushed back
            // out to sea. `shoreward` is the uphill bed gradient and the bed is
            // negative in water, so +direction is genuinely toward the beach.
            //
            // The noise offset is what keeps this from reading as concentric
            // rings: it displaces the crest line by a fraction of a wavelength
            // along its length, so successive fronts arrive slightly ragged and
            // out of step instead of as clean arcs. Scaled by wavelength, and by
            // refractMix so the offshore swell stays clean and only the shoaling
            // near-shore trains break up.
            const float irregular =
                phaseNoise * wavelength * 0.30 * refractMix;
            phase = k * (dot(direction, baseXZ) + irregular) - omega * time;
        }
        float sine = sin(phase);
        float cosine = cos(phase);
        float horizontal = steepness * amplitude;

        position.xz += direction * horizontal * cosine;
        position.y += amplitude * sine;

        float common = horizontal * k * sine;
        tangentX += float3(
            -direction.x * direction.x * common,
             direction.x * amplitude * k * cosine,
            -direction.x * direction.y * common);
        tangentZ += float3(
            -direction.x * direction.y * common,
             direction.y * amplitude * k * cosine,
            -direction.y * direction.y * common);
        compression += common;
    }

    normal = normalize(cross(tangentZ, tangentX));
    crest = saturate(compression * 1.8 +
        (position.y - volume0.y) * 1.25 + breakingEnergy * 0.42);
}

float4 SampleUltraSpectrum(float2 worldXZ, bool previous)
{
    static const float periods[3] = { 48.0, 192.0, 768.0 };
    float4 result = 0.0;
    [unroll]
    for (uint cascade = 0; cascade < 3; ++cascade) {
        const float2 uv = frac(worldXZ / periods[cascade]);
        result += previous
            ? ultraSpectrumPrevious.SampleLevel(
                  linearWrap, float3(uv, cascade), 0.0)
            : ultraSpectrumCurrent.SampleLevel(
                  linearWrap, float3(uv, cascade), 0.0);
    }
    return result;
}

float4 SampleUltraSurface(float2 worldXZ, bool previous)
{
    float4 deep = SampleUltraSpectrum(worldXZ, previous);
    float2 uv;
    if (!WaterBathymetryUV(worldXZ, uv)) return deep;
    const float bed = ultraBathymetry.SampleLevel(linearClamp, uv, 0.0);
    const float4 coast = previous
        ? ultraCoastPrevious.SampleLevel(linearClamp, uv, 0.0)
        : ultraCoastCurrent.SampleLevel(linearClamp, uv, 0.0);
    const float coastWeight = 1.0 - smoothstep(8.0, 14.0, max(-bed, 0.0));
    const float horizontalWeight = smoothstep(0.15, 3.0, max(-bed, 0.0));
    deep.xz *= lerp(0.08, 1.0, horizontalWeight);
    return float4(deep.xz * (1.0 - coastWeight),
                  lerp(deep.y, coast.x, coastWeight),
                  max(deep.w, coast.w));
}

VSOutput VSMain(VSInput input)
{
    VSOutput output;
    float3 currentPosition;
    float3 previousPosition;
    float3 currentNormal;
    float3 previousNormal;
    float currentCrest;
    float previousCrest;

    if (volume0.w > 0.5) {
        float2 currentXZ = input.position.xz + clipmapParams.xy;
        float2 previousXZ = input.position.xz + previousCameraTime.xy;
        output.oceanBaseXZ = currentXZ;
        if (volume1.w > 1.5 && volume1.z < 0.5) {
            const float4 currentSurface =
                SampleUltraSurface(currentXZ, false);
            const float4 previousSurface =
                SampleUltraSurface(previousXZ, true);
            currentPosition = float3(
                currentXZ.x + currentSurface.x,
                volume0.y + currentSurface.z,
                currentXZ.y + currentSurface.y);
            previousPosition = float3(
                previousXZ.x + previousSurface.x,
                volume0.y + previousSurface.z,
                previousXZ.y + previousSurface.y);
            currentNormal = float3(0.0, 1.0, 0.0);
            previousNormal = currentNormal;
            currentCrest = currentSurface.w;
            previousCrest = previousSurface.w;
        } else if (volume1.z > 0.5) {
            // The deployment grid spans the whole ocean and stays geometrically
            // flat. Its normals and foam crest are evaluated per pixel below,
            // so no coarse displaced triangle can appear as an LOD ring.
            currentPosition = float3(currentXZ.x, volume0.y, currentXZ.y);
            previousPosition =
                float3(previousXZ.x, volume0.y, previousXZ.y);
            currentNormal = float3(0.0, 1.0, 0.0);
            previousNormal = currentNormal;
            currentCrest = 0.0;
            previousCrest = 0.0;
        } else {
            // Both times take the same speed scale, or the motion vectors
            // would be derived from a surface moving at a different rate than
            // the one being drawn and the reprojection would smear.
            EvaluateOcean(currentXZ, cameraTime.w * highWaveParams.x,
                          currentPosition, currentNormal, currentCrest);
            EvaluateOcean(previousXZ,
                          previousCameraTime.w * highWaveParams.x,
                          previousPosition,
                          previousNormal, previousCrest);
        }

        // Ease the geometric wave displacement out on the coarse rings so
        // vertex interpolation cannot reveal a square ring boundary. Keyed on
        // distance rather than a ring index: the index moves whenever the
        // clipmap gains rings, and what actually matters is the cell size at
        // this position. The fade now runs out to the last subdivided ring
        // instead of stopping at 320 m, since the rings between there and
        // 4 km can carry real waves. Per-pixel micro normals still perform
        // their own footprint fade below.
        if (volume1.z < 0.5) {
            // Retire the geometric displacement on cell size rather than on
            // raw distance, because cell size is what actually decides whether
            // the mesh can carry a wave.
            //
            // A quad only reproduces a wave it has several vertices across.
            // Measured against this spectrum (1.37 m crest to trough), linear
            // interpolation across one cell is off by 0.14 m at 2.5 m cells,
            // 0.40 m at 6.25 m, 0.60 m at 10 m and 0.93 m at 17.5 m -- past
            // ring 6 the error exceeds a third of the entire wave range, and
            // the surface visibly steps from quad to quad. That is the hard
            // faceted grid, and no amount of per-pixel shading hides it,
            // because the silhouette itself is wrong.
            //
            // So displacement fades out between 2 and 6.5 metre cells. Beyond
            // that the surface goes geometrically flat -- which is honest, and
            // invisible, because the per-pixel normal path below rebuilds the
            // full analytic wave normal there. Distant water keeps its wave
            // shading and loses only a vertical displacement it could never
            // represent without stepping.
            //
            // Keyed on distance, NOT on the ring index. Ring level is constant
            // across a whole ring, so fading on it makes the fade constant per
            // ring too and every ring boundary becomes a hard step -- measured
            // jumps of 0.38 and 0.57 in the fade weight at the ring 4/5 and 5/6
            // boundaries. The displacement step alone is subtle, but foam runs
            // it through a narrow smoothstep(0.18, 0.46) threshold, which turns
            // each step into a visible edge and the rings into a checkerboard
            // of foamy and foamless quads. That is why the grid disappeared
            // when foam strength went to zero.
            //
            // Distance is continuous, so the fade is too, and ring boundaries
            // stop existing as far as shading is concerned. Ring extents track
            // distance closely enough (cell size is roughly distance/24 across
            // the whole clipmap) that this still retires the displacement in
            // the same place -- around 150 m, where cells reach ~6 m -- without
            // any of the stepping.
            const float ringDistance = max(
                abs(input.position.x), abs(input.position.z));
            const float waveFade =
                1.0 - smoothstep(60.0, 175.0, ringDistance);
            currentPosition = lerp(
                float3(currentXZ.x, volume0.y, currentXZ.y),
                currentPosition, waveFade);
            previousPosition = lerp(
                float3(previousXZ.x, volume0.y, previousXZ.y),
                previousPosition, waveFade);
            // Normals and crest are NOT faded here. The pixel shader rebuilds
            // them analytically wherever the displacement has gone, so fading
            // them too would flatten the shading along with the geometry and
            // hand back the featureless water this is meant to avoid.
        }
    } else {
        currentPosition = input.position;
        previousPosition = input.position;
        output.oceanBaseXZ = input.position.xz;
        currentNormal = normalize(input.normal);
        previousNormal = currentNormal;
        currentCrest = saturate(
            (input.position.y - volume0.y) / max(opticalParams.y, 0.01));
        previousCrest = currentCrest;
    }

    output.worldPosition = currentPosition;
    output.normal = currentNormal;
    output.crest = currentCrest;
    output.currentClip = mul(float4(currentPosition, 1.0), viewProjection);
    output.previousClip =
        mul(float4(previousPosition, 1.0), previousViewProjection);
    output.clipmapData = float2(input.tangent.w, input.texCoord.y);
    output.position = output.currentClip;
    if (volume0.w > 0.5 && volume1.z < 0.5 && output.position.w > 0.0) {
        // Gameplay uses an 800 m far plane for useful depth precision. Clamp
        // only the ocean inside it so the visual horizon ring survives far
        // clipping; its world position remains untouched for stable shading.
        output.position.z = min(
            output.position.z, output.position.w * 0.99998);
    }
    return output;
}

float LoadOpaqueDepth(float2 uv)
{
    int2 pixel = int2(clamp(
        uv * screenParams.xy, 0.0, screenParams.xy - 1.0));
#ifdef WATER_DEPTH_MSAA
    float depth = sceneDepth.Load(pixel, 0);
    [unroll]
    for (uint sampleIndex = 1; sampleIndex < 4; ++sampleIndex)
        depth = min(depth, sceneDepth.Load(pixel, sampleIndex));
    return depth;
#else
    return sceneDepth.Load(int3(pixel, 0));
#endif
}

float3 ReconstructWorld(float2 uv, float depth)
{
    float2 ndc = uv * float2(2.0, -2.0) + float2(-1.0, 1.0);
    float4 world = mul(float4(ndc, depth, 1.0), inverseViewProjection);
    return world.xyz / max(abs(world.w), 1e-5);
}

float2 ProjectUV(float3 worldPosition)
{
    float4 clipPosition = mul(float4(worldPosition, 1.0), viewProjection);
    return (clipPosition.xy / max(clipPosition.w, 1e-5)) *
        float2(0.5, -0.5) + 0.5;
}

float3 SampleEnvironment(float3 direction, float roughness)
{
    direction = normalize(direction);
    float2 uv = float2(
        atan2(direction.z, direction.x) * 0.159154943 + 0.5,
        acos(clamp(direction.y, -1.0, 1.0)) * 0.318309886);
    uint width, height, mipCount;
    environmentMap.GetDimensions(0, width, height, mipCount);
    float lod = roughness * roughness *
        max((float)mipCount - 1.0, 0.0);
    return environmentMap.SampleLevel(linearClamp, uv, lod).rgb;
}

bool TraceWaterReflection(float3 origin, float3 direction, float roughness,
                          float2 pixel, out float2 hitUV, out float confidence)
{
    const uint maxSteps = 24;
    uint stepCount = volume1.w > 0.5 ? maxSteps : 10;
    float maxDistance = volume1.w > 0.5 ? 85.0 : 32.0;
    float stride = maxDistance / stepCount;
    float previousDelta = -0.08;
    float previousT = 0.08;
    float jitter = Hash21(pixel);
    hitUV = 0.0;
    confidence = 0.0;

    [loop]
    for (uint rayStep = 1; rayStep <= maxSteps; ++rayStep) {
        if (rayStep > stepCount) break;
        float t = stride * (rayStep - 0.72 + jitter);
        float3 rayPosition = origin + direction * t;
        float4 clipPosition =
            mul(float4(rayPosition, 1.0), viewProjection);
        if (clipPosition.w <= 0.0) break;
        float2 uv = (clipPosition.xy / clipPosition.w) *
            float2(0.5, -0.5) + 0.5;
        if (any(uv <= 0.002) || any(uv >= 0.998)) break;
        float depth = LoadOpaqueDepth(uv);
        if (depth >= 0.99999) {
            previousT = t;
            continue;
        }
        float3 scenePosition = ReconstructWorld(uv, depth);
        float delta = length(rayPosition - cameraTime.xyz) -
                      length(scenePosition - cameraTime.xyz);
        if (delta >= 0.0 && previousDelta < 0.0) {
            float lo = previousT;
            float hi = t;
            [unroll]
            for (uint refine = 0; refine < 4; ++refine) {
                float mid = (lo + hi) * 0.5;
                float3 midPosition = origin + direction * mid;
                float2 midUV = ProjectUV(midPosition);
                float3 midScene =
                    ReconstructWorld(midUV, LoadOpaqueDepth(midUV));
                if (length(midPosition - cameraTime.xyz) >=
                    length(midScene - cameraTime.xyz))
                    hi = mid;
                else
                    lo = mid;
            }
            hitUV = ProjectUV(origin + direction * hi);
            float edge = min(min(hitUV.x, hitUV.y),
                             min(1.0 - hitUV.x, 1.0 - hitUV.y));
            confidence = smoothstep(0.0, 0.075, edge) *
                saturate(1.0 - hi / maxDistance) *
                (1.0 - roughness * 0.7);
            return confidence > 0.001;
        }
        previousDelta = delta;
        previousT = t;
    }
    return false;
}

float3 ToneMapWater(float3 color)
{
    color = max(color, 0.0);
    color = color / (1.0 + color);
    return pow(color, 1.0 / 2.2);
}

struct PSOutput
{
    float4 color : SV_Target0;
#ifdef WATER_MOTION_OUTPUT
    float2 motion : SV_Target1;
#endif
};

PSOutput PSMain(VSOutput input)
{
    PSOutput output;
    float2 uv = input.position.xy * screenParams.zw;
    float opaqueDepth = LoadOpaqueDepth(uv);

    if (volume0.w > 0.5 && volume1.z > 0.5) {
        float2 fromCenter = abs(input.worldPosition.xz - volume0.xz);
        clip(volume1.xy - fromCenter);
    }

    const bool deploymentOcean = volume0.w > 0.5 && volume1.z > 0.5;
    const bool hasOpaqueDepth = opaqueDepth < 0.99999;
    float deploymentDepthSeparation = 0.0;
    if (deploymentOcean && hasOpaqueDepth) {
        const float3 opaqueWorld = ReconstructWorld(uv, opaqueDepth);
        deploymentDepthSeparation =
            length(opaqueWorld - cameraTime.xyz) -
            length(input.worldPosition - cameraTime.xyz);
    }

    if (deploymentOcean && volume1.z > 1.5) {
        // Green is submerged terrain behind the ocean surface, red is dry land
        // in front of it, and magenta has no opaque depth. This runs before the
        // manual depth rejection so the diagnostic can show the pixels that
        // would otherwise disappear.
        output.color = !hasOpaqueDepth
            ? float4(1.0, 0.02, 0.85, 1.0)
            : deploymentDepthSeparation > 0.01
                ? float4(0.05, 1.0, 0.12, 1.0)
                : float4(1.0, 0.08, 0.02, 1.0);
#ifdef WATER_MOTION_OUTPUT
        output.motion = 0.0;
#endif
        return output;
    }

    // The ordinary view keeps its established nonlinear depth test. In the
    // high deployment view, a 1e-5 device-depth bias spans metres across the
    // shallow shelf and rejects water until the seabed is deep. Compare linear
    // camera distances there so the tolerance remains one centimetre at every
    // overview distance.
    if (deploymentOcean) {
        if (hasOpaqueDepth)
            clip(deploymentDepthSeparation - 0.01);
    } else {
        clip(opaqueDepth - input.position.z - 0.00001);
    }

    const bool ultraOcean = volume0.w > 0.5 && volume1.w > 1.5 &&
                            volume1.z < 0.5;
    float2 ultraUV = 0.0;
    const bool insideUltraCoast = ultraOcean &&
        WaterBathymetryUV(input.worldPosition.xz, ultraUV);
    float ultraBed = -30.0;
    float4 ultraCoast = 0.0;
    if (insideUltraCoast) {
        ultraBed = ultraBathymetry.SampleLevel(
            linearClamp, ultraUV, 0.0);
        ultraCoast = ultraCoastCurrent.SampleLevel(
            linearClamp, ultraUV, 0.0);
        // The numerical wet/dry front, rather than an opaque-depth comparison,
        // decides whether a thin sheet is still running over the sand.
        if (ultraBed >= 0.0)
            clip(ultraCoast.x - ultraBed - 0.002);
    }

    float3 viewDirection = normalize(cameraTime.xyz - input.worldPosition);
    float3 normal = normalize(input.normal);
    float crest = input.crest;

    float2 p = input.worldPosition.xz;
    // How much ocean one pixel covers. Shared by both fades below so the swell
    // and the capillary detail retire on the same measure.
    float footprint = max(length(ddx(p)), length(ddy(p)));

    if (ultraOcean) {
        const float sampleStep = clamp(footprint * 0.65, 0.18, 0.75);
        const float heightLeft =
            SampleUltraSurface(p - float2(sampleStep, 0.0), false).z;
        const float heightRight =
            SampleUltraSurface(p + float2(sampleStep, 0.0), false).z;
        const float heightDown =
            SampleUltraSurface(p - float2(0.0, sampleStep), false).z;
        const float heightUp =
            SampleUltraSurface(p + float2(0.0, sampleStep), false).z;
        normal = normalize(float3(
            -(heightRight - heightLeft) / (2.0 * sampleStep), 1.0,
            -(heightUp - heightDown) / (2.0 * sampleStep)));
        crest = max(crest, SampleUltraSurface(p, false).w);
    }

    if (ultraOcean && ultraDebug.x > 0.5) {
        const uint diagnostic = (uint)(ultraDebug.x + 0.5);
        float3 diagnosticColor = 0.0;
        if (diagnostic == 1) {
            const float hue = frac(input.clipmapData.x * 0.6180339);
            diagnosticColor = saturate(abs(frac(
                hue + float3(0.0, 0.667, 0.333)) * 6.0 - 3.0) - 1.0);
        } else if (diagnostic == 2) {
            const float morph = smoothstep(0.0, 0.12,
                min(input.clipmapData.y, 1.0 - input.clipmapData.y));
            diagnosticColor = lerp(float3(1.0, 0.08, 0.02),
                                   float3(0.05, 0.9, 0.2), morph);
        } else if (diagnostic == 3) {
            diagnosticColor = lerp(float3(0.03, 0.1, 0.45),
                float3(0.95, 0.75, 0.25), saturate((ultraBed + 12.0) / 16.0));
        } else if (diagnostic == 4) {
            const bool wet = insideUltraCoast &&
                ultraCoast.x - ultraBed > 0.02;
            diagnosticColor = wet ? float3(0.04, 0.9, 0.3)
                                  : float3(0.9, 0.04, 0.02);
        } else if (diagnostic == 5) {
            const float texel = 1.0 / max(ultraSimulation.y, 1.0);
            const float bedL = ultraBathymetry.SampleLevel(
                linearClamp, ultraUV - float2(texel, 0.0), 0.0);
            const float bedR = ultraBathymetry.SampleLevel(
                linearClamp, ultraUV + float2(texel, 0.0), 0.0);
            const float bedD = ultraBathymetry.SampleLevel(
                linearClamp, ultraUV - float2(0.0, texel), 0.0);
            const float bedU = ultraBathymetry.SampleLevel(
                linearClamp, ultraUV + float2(0.0, texel), 0.0);
            diagnosticColor = float3(normalize(float3(
                bedL - bedR, 1.0, bedD - bedU)) * 0.5 + 0.5);
        } else if (diagnostic == 6) {
            const float breaking = SampleUltraSpectrum(p, false).w;
            diagnosticColor = breaking.xxx;
        } else if (diagnostic == 7) {
            diagnosticColor = ultraCoast.www;
        } else {
            const float height = SampleUltraSurface(p, false).z;
            diagnosticColor = lerp(float3(0.05, 0.15, 0.8),
                float3(1.0, 0.18, 0.03), saturate(height * 0.5 + 0.5));
        }
        output.color = float4(diagnosticColor, 1.0);
#ifdef WATER_MOTION_OUTPUT
        output.motion = 0.0;
#endif
        return output;
    }

    // Rebuild the analytic wave normal per pixel rather than interpolating one
    // vertex normal across a clipmap triangle.
    //
    // Deployment always did this. Gameplay did not, and that is what breaks the
    // midfield into flat quadrilateral facets with hard seams along the triangle
    // edges: measured against the 14-ring clipmap, a 22 m swell has 8.8 cells
    // per wavelength at ring 4 but only 3.5 by ring 6 and 2.2 by ring 7, so from
    // roughly 160 m out a single interpolated normal is being stretched over
    // most of a wave. No ring layout fixes that -- doubling the ring count only
    // moves where it starts -- because the artifact is the interpolation itself,
    // not the ring boundaries.
    //
    // Ramped in on pixel footprint, so near water keeps the cheaper vertex path
    // and everything beyond it is analytic. There is no upper cutoff: the
    // vertex displacement has been faded out by then, so this is the only thing
    // still carrying wave detail, and dropping it would leave flat glass. The
    // capillary term below does its own fade and handles aliasing.
    const bool highOcean = volume0.w > 0.5 &&
                           volume1.w > 0.5 && volume1.w < 1.5;
    if (highOcean) {
        const float pixelNormalWeight = volume1.z > 0.5
            ? 1.0
            : smoothstep(0.05, 0.30, footprint);
        if (pixelNormalWeight > 0.001) {
            float3 evaluatedPosition;
            float3 evaluatedNormal;
            float evaluatedCrest;
            // Evaluate on the undisplaced clipmap plane. worldPosition.xz has
            // already been displaced at the vertices, so using it here feeds a
            // piecewise-linear triangle approximation back into the analytic
            // wave field and stamps that topology into the foam.
            EvaluateOcean(input.oceanBaseXZ,
                          cameraTime.w * highWaveParams.x,
                          evaluatedPosition, evaluatedNormal, evaluatedCrest);
            normal = normalize(
                lerp(normal, evaluatedNormal, pixelNormalWeight));
            // Foam applies a narrow threshold to crest. Blending the analytic
            // value with the interpolated vertex value preserves the very
            // facets this path exists to remove, so switch crest outright once
            // the coarse rings need per-pixel evaluation.
            crest = evaluatedCrest;
        }
    }

    // Derivative-filtered capillary detail. Fine octaves disappear before they
    // alias, leaving stable broad swell toward the horizon.
    float2 micro = 0.0;
    micro += normalize(float2(0.91, 0.41)) *
        cos(dot(p, normalize(float2(0.91, 0.41))) * 1.65 +
            cameraTime.w * 1.15) * 0.030;
    micro += normalize(float2(-0.37, 0.93)) *
        cos(dot(p, normalize(float2(-0.37, 0.93))) * 3.10 -
            cameraTime.w * 1.72) * 0.017;
    micro += normalize(float2(0.58, -0.81)) *
        cos(dot(p, normalize(float2(0.58, -0.81))) * 6.20 +
            cameraTime.w * 2.35) * 0.008;
    float microFade = 1.0 - smoothstep(0.22, 1.35, footprint);
    normal = normalize(normal +
        float3(-micro.x, 0.0, -micro.y) *
        clipmapParams.z * highWaveParams.y * microFade);

    float waterDistance = length(input.worldPosition - cameraTime.xyz);
    float thickness;
    float verticalDepth;
    if (opaqueDepth < 0.99999) {
        float3 opaqueWorld = ReconstructWorld(uv, opaqueDepth);
        verticalDepth = max(input.worldPosition.y - opaqueWorld.y, 0.02);
        thickness = length(opaqueWorld - cameraTime.xyz) - waterDistance;
    } else {
        // The camera-centred terrain clipmap eventually ends after the authored
        // seabed has reached its flat -6 m shelf. `thickness` is distance along
        // the view ray, not vertical water depth: using a constant 6 m outside
        // the grid disagreed with the reconstructed ray length inside it and
        // exposed the square terrain boundary from the high deployment camera.
        // Intersect the same ray with the continued -6 m seabed instead.
        verticalDepth = max(input.worldPosition.y + 6.0, 0.02);
        thickness = verticalDepth / max(abs(viewDirection.y), 0.05);
    }
    thickness = clamp(thickness, 0.02, 30.0);
    if (insideUltraCoast)
        verticalDepth = max(ultraCoast.x - ultraBed, 0.02);

    // Convert only the horizontal surface slope into a bounded pixel offset.
    // Projecting the full one-metre normal and multiplying that UV displacement
    // by a pixel count caused nearby sand to stretch hundreds of pixels.
    float2 projectedSlope =
        ProjectUV(input.worldPosition + float3(normal.x, 0.0, normal.z)) - uv;
    float2 projectedSlopePixels = projectedSlope * screenParams.xy;
    float projectedLength = length(projectedSlopePixels);
    float2 refractionDirection = projectedLength > 1e-4
        ? projectedSlopePixels / projectedLength
        : 0.0;
    float slopeStrength = saturate(length(normal.xz) * 3.0);
    float refractionPixels = min(
        opticalParams.z,
        lerp(0.35, volume0.w > 0.5 ? 2.5 : 3.5,
             smoothstep(0.08, 4.0, thickness)) * slopeStrength);
    refractionPixels *= smoothstep(0.06, 0.55, thickness);
    float2 refractedUV = clamp(
        uv + refractionDirection * refractionPixels * screenParams.zw,
        screenParams.zw * 1.5, 1.0 - screenParams.zw * 1.5);
    float refractedDepth = LoadOpaqueDepth(refractedUV);
    bool rejectRefraction =
        refractedDepth >= 0.99999 && opaqueDepth < 0.99999;
    if (refractedDepth < 0.99999) {
        float refractedDistance = length(
            ReconstructWorld(refractedUV, refractedDepth) - cameraTime.xyz);
        float refractedThickness = refractedDistance - waterDistance;
        rejectRefraction = rejectRefraction ||
            refractedThickness <= 0.01 ||
            abs(refractedThickness - thickness) >
                max(0.40, thickness * 1.25);
    }
    if (rejectRefraction)
        refractedUV = uv;

    float depthBlend = smoothstep(0.35, 8.0, thickness);
    float3 scatterColor =
        lerp(shallowScatter.rgb, deepScatter.rgb, depthBlend);
    float3 behind = sceneColor.SampleLevel(
        linearClamp, refractedUV, 0.0).rgb;
    // Past the sloping shelf, transmitted scenery is fully replaced by the
    // authored deep-water medium before the rasterised seabed ends at 6 m.
    // Consequently the missing-depth side no longer switches to refracted sky;
    // both sides have already converged to the same colour over a broad band.
    const float openOceanBlend = smoothstep(3.25, 5.75, verticalDepth);
    behind = lerp(behind, deepScatter.rgb, openOceanBlend);
    float3 transmittance = exp(-absorption.rgb * thickness);
    float3 transmitted = behind * transmittance +
        scatterColor * (1.0 - transmittance);

    float nDotV = saturate(dot(normal, viewDirection));
    const float dielectricF0 = 0.02037;
    float fresnel = dielectricF0 +
        (1.0 - dielectricF0) * pow(1.0 - nDotV, 5.0);
    float shallowWater = 1.0 - smoothstep(0.18, 1.35, thickness);
    float roughness = clamp(
        clipmapParams.w + footprint * 0.018 + shallowWater * 0.05,
        0.07, 0.24);
    float3 reflectionDirection = reflect(-viewDirection, normal);
    float3 reflection = SampleEnvironment(
        reflectionDirection, roughness);

    float2 reflectionUV;
    float reflectionConfidence;
    if (TraceWaterReflection(
            input.worldPosition + normal * 0.06,
            reflectionDirection, roughness,
            input.position.xy, reflectionUV, reflectionConfidence)) {
        float3 screenReflection = sceneColor.SampleLevel(
            linearClamp, reflectionUV, 0.0).rgb;
        reflection = lerp(
            reflection, screenReflection,
            reflectionConfidence * opticalParams.w);
    }
    // Daylight is exactly 1 and takes the untouched path. At night both the
    // environment fallback and SSR must follow the exposed sky down together.
    if (lightDirection.w < 0.999)
        reflection *= lightDirection.w;

    // GGX sun path. Analytic wave normals plus roughness keep this continuous
    // instead of exposing individual triangles as white wedges.
    float3 lightDir = normalize(lightDirection.xyz);
    float3 halfVector = normalize(viewDirection + lightDir);
    float nDotL = saturate(dot(normal, lightDir));
    float nDotH = saturate(dot(normal, halfVector));
    float vDotH = saturate(dot(viewDirection, halfVector));
    float alpha = roughness * roughness;
    float alpha2 = alpha * alpha;
    float denominator =
        nDotH * nDotH * (alpha2 - 1.0) + 1.0;
    float distribution = alpha2 /
        max(3.14159265 * denominator * denominator, 1e-5);
    float sunFresnel = dielectricF0 +
        (1.0 - dielectricF0) * pow(1.0 - vDotH, 5.0);
    // Height-correlated Smith visibility (Heitz 2014). This is V, not G: the
    // Cook-Torrance 1/(4 nDotL nDotV) denominator is folded in, so the product
    // D * F * V * nDotL below is the complete specular BRDF with no free scale
    // factor. The previous form was a G term with that denominator missing,
    // which overshot by orders of magnitude and needed a hand-tuned dimmer to
    // compensate.
    float lambdaV = nDotL * sqrt(
        nDotV * nDotV * (1.0 - alpha2) + alpha2);
    float lambdaL = nDotV * sqrt(
        nDotL * nDotL * (1.0 - alpha2) + alpha2);
    float sunVisibility = 0.5 / max(lambdaV + lambdaL, 1e-5);
    float3 sunSpecular = lightColor.rgb *
        distribution * sunFresnel * sunVisibility * nDotL;

    float foamNoise = SmoothNoise(
        p * 0.47 + float2(cameraTime.w * 0.055, -cameraTime.w * 0.038));
    foamNoise = smoothstep(0.48, 0.78, foamNoise);

    // Foam is whitecaps on the open water only, reaching in to 20 m from the
    // shore and leaving the beach itself clear.
    //
    // Gated on distance from the shoreline rather than from the camera, so the
    // clear band hugs bays and headlands instead of following the player
    // around. The bathymetry texture stores a bed height derived from signed
    // shoreline distance -- 0.20 land slope, 0.35 offshore, shifted 30 m
    // seaward -- so undo that encoding to recover the distance, exactly as
    // EvaluateOcean does. The 30.0 below must track kShoreOffsetMetres in
    // UltraWaterSimulation.h; it is the encoding offset, not the foam
    // distance, so it stays put when the gate below moves.
    //
    // Without bathymetry there is no shoreline to measure against, so foam
    // falls back to open-ocean behaviour rather than vanishing entirely.
    float offshoreGate = 1.0;
    float2 foamBathyUV;
    if (WaterBathymetryUV(input.worldPosition.xz, foamBathyUV)) {
        const float foamBed = ultraBathymetry.SampleLevel(
            linearClamp, foamBathyUV, 0.0);
        const float shoreDistance =
            -((foamBed >= 0.0 ? foamBed / 0.20 : foamBed / 0.35) - 30.0);
        // Ramp over 20-35 m so the band has a soft inner edge instead of a
        // hard ring, which would read as another contour line in the water.
        // Foam is fully absent shoreward of 20 m and fully present by 35 m.
        offshoreGate = smoothstep(20.0, 35.0, shoreDistance);
    }
    float crestFoam = smoothstep(
        opticalParams.y, opticalParams.y + 0.28, crest) *
        foamNoise;
    float foam = saturate(
        crestFoam * 0.58 * offshoreGate * highWaveParams.z);
    // Ultra's solver produces its own surf and foam from the coastal
    // simulation, so the High multiplier above does not apply to it.
    if (insideUltraCoast)
        foam = saturate(max(foam, ultraCoast.w * 0.92));
    float3 foamColor = float3(0.66, 0.76, 0.72);

    float reflectionWeight =
        saturate(fresnel * 0.94 + 0.035 + roughness * 0.06);
    reflectionWeight *= lerp(0.18, 1.0,
        smoothstep(0.22, 2.2, thickness));
    float3 color = lerp(transmitted, reflection, reflectionWeight);
    color += sunSpecular;
    color = lerp(color, foamColor, foam * 0.82);
    // Reflection is only one contributor; transmission, authored scatter and
    // foam otherwise stay at daylight strength and turn the whole night ocean
    // pale. Keep daylight on the untouched path.
    if (lightColor.w < 0.999)
        color *= lightColor.w;

#ifndef WATER_HDR_TARGET
    color = ToneMapWater(color);
#endif
    if (volume0.w > 0.5 && opaqueDepth >= 0.99999) {
        // The water pass runs after atmospheric fog, so an uncorrected ocean
        // remains darker than the already-fogged sky at grazing angles. Fade
        // the far surface into the exact scene-copy pixel it meets. This is the
        // far-mesh treatment used by large-ocean renderers, without bending the
        // plane upward into a bowl that becomes obvious from aircraft.
        const float distanceFade = smoothstep(280.0, 1200.0, waterDistance);
        const float grazingFade =
            1.0 - smoothstep(0.018, 0.115, nDotV);
        // Capped below 1. Matching the fogged sky is only meant to remove the
        // brightness step at the horizon, but an uncapped blend reached 1.0
        // around 5 km out and replaced the ocean with the sky texture outright,
        // leaving a washed-out band between the waterline and the horizon. The
        // residual keeps the surface reading as water all the way out.
        const float horizonBlend =
            distanceFade * grazingFade * kHorizonBlendCeiling;
        const float3 horizonColor = sceneColor.SampleLevel(
            linearClamp, uv, 0.0).rgb;
        color = lerp(color, horizonColor, horizonBlend);
    }
    output.color = float4(color, 1.0);
#ifdef WATER_MOTION_OUTPUT
    float2 currentUV = (input.currentClip.xy / input.currentClip.w) *
        float2(0.5, -0.5) + 0.5;
    float2 previousUV = currentUV;
    if (input.previousClip.w > 0.001)
        previousUV = (input.previousClip.xy / input.previousClip.w) *
            float2(0.5, -0.5) + 0.5;
    output.motion = currentUV - previousUV;
#endif
    return output;
}

struct UnderwaterVSOutput
{
    float4 position : SV_POSITION;
    float2 uv : TEXCOORD0;
};

UnderwaterVSOutput UnderwaterVS(uint vertexID : SV_VertexID)
{
    UnderwaterVSOutput output;
    output.uv = float2((vertexID << 1) & 2, vertexID & 2);
    output.position = float4(
        output.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return output;
}

float4 UnderwaterPS(UnderwaterVSOutput input) : SV_Target0
{
    const float depth = LoadOpaqueDepth(input.uv);
    float3 rayDirection;
    float pathLength;
    if (depth < 0.99999) {
        const float3 world = ReconstructWorld(input.uv, depth);
        const float3 ray = world - cameraTime.xyz;
        pathLength = length(ray);
        rayDirection = ray / max(pathLength, 1e-4);
    } else {
        const float3 farWorld = ReconstructWorld(input.uv, 0.9999);
        rayDirection = normalize(farWorld - cameraTime.xyz);
        pathLength = 30.0;
    }

    // Sample the moving surface where an upward ray leaves the water. This is
    // only a screen-space refraction estimate; the medium itself remains
    // Beer-Lambert and deliberately contains no caustic projection.
    float2 distortion = 0.0;
    if (rayDirection.y > 0.015) {
        const float toSurface = max(
            (volume0.y - cameraTime.y) / rayDirection.y, 0.0);
        const float2 surfaceXZ =
            cameraTime.xz + rayDirection.xz * toSurface;
        const float sampleStep = 0.28;
        const float hx = SampleUltraSurface(
            surfaceXZ + float2(sampleStep, 0.0), false).z -
            SampleUltraSurface(surfaceXZ - float2(sampleStep, 0.0), false).z;
        const float hz = SampleUltraSurface(
            surfaceXZ + float2(0.0, sampleStep), false).z -
            SampleUltraSurface(surfaceXZ - float2(0.0, sampleStep), false).z;
        distortion = float2(hx, hz) * 0.012;
        pathLength = min(pathLength, toSurface);
    }
    const float2 distortedUV = clamp(
        input.uv + distortion,
        screenParams.zw * 1.5, 1.0 - screenParams.zw * 1.5);
    float3 color = sceneColor.SampleLevel(linearClamp, distortedUV, 0.0).rgb;
    const float3 transmittance = exp(
        -absorption.rgb * min(pathLength, 30.0) * 0.72);
    const float3 medium = lerp(shallowScatter.rgb, deepScatter.rgb,
        smoothstep(1.0, 12.0, pathLength));
    color = color * transmittance + medium * (1.0 - transmittance);

    // Above the critical angle, favor the reflected underwater environment.
    // This keeps grazing views of the underside from looking like a clear hole.
    const float critical = smoothstep(0.72, 0.96,
        1.0 - max(rayDirection.y, 0.0));
    if (rayDirection.y > 0.0) {
        const float3 reflected = SampleEnvironment(
            reflect(rayDirection, float3(0.0, -1.0, 0.0)), 0.16);
        color = lerp(color, reflected * 0.28, critical * 0.42);
    }
    return float4(color, 1.0);
}
