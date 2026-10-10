# Experimental ray allocation and emissive spatial reuse

Both controls are in Video settings, off by default:

- **Experimental Variable Rate Lumen GI** requires Lumen GI and enhanced DXR.
  `LumenVariableRateGI=1` enables it; `LumenVariableRateBudget=0.5` caps primary
  diffuse rays at half the render pixel count. Range: 0.125 to 1.0.
- **Experimental Emissive ReSTIR Compatibility** enables one spatial donor
  chosen from sixteen primary-geometry candidates. Persisted as
  `EmissiveReSTIRCompatibility=1`. It works independently of Lumen GI.

Variable rate GI uses a paired-seed gradient ray per 8x8 stratum, a sixteen-bin
request histogram, GPU budget allocation, compact jobs, and indirect dispatch.
Gradient rays count against the same cap. Each eligible pixel offers up to four
main samples; requested samples outrank surplus ones. The GPU assigns exactly
`min(budget - gradientRays, eligiblePixels * 4)` main jobs. Empty sky/far-field
views can therefore use less than the budget. Ray cost still varies with
traversal, hit shading, and nested shadow rays; the cap does not guarantee fixed
milliseconds. The menu shows actual primary rays and their budget from a
completed submission, without a blocking map or CPU/GPU wait.

Only actual main ray samples enter the dedicated temporal history. Gradient
replay includes the emitter RNG at the ray hit and bypasses the hit radiance
cache. Disocclusions and detected lighting changes receive higher priorities;
stable pixels retain geometrically validated history. Unsampled cold pixels
borrow compatible nearby irradiance or use the existing probe/sky ambient.
Spatial reconstruction does not enter temporal history. Primary guides use
interpolated raster normals; material normal-map detail is still applied by the
full lighting resolve, and this geometric guide approximation needs visual QA.

The optional SM6 resolve applies each receiver's albedo, AO, and GI intensity.
Scope views retain full-resolution rays, and scope/debug views never advance
the VRRT history. Resize and mode changes invalidate history. Pipeline or
allocation failure falls back to full-rate Lumen. Selecting VRRT bypasses the
half-resolution and ReSTIR GI estimators; cascades take precedence if both are
requested through a hand-edited INI. Memory is allocated once per frame slot:
approximately 192 bytes per render pixel across the two engine frame slots,
plus small histograms, gradients, and statistics (about 380 MiB at 1080p).

Emissive neighbor compatibility uses primary positions/normals and the paper's
Omega=0.05, beta=8 heuristic. A-Chao WRS selects the donor before reading its
light reservoir; light radiance, weights, and visibility do not influence donor
selection. Donors are reprojected through previous model/wind positions and
checked against their own persistent namespace. Both temporal and spatial
samples come from previous-frame parity. The existing target/weight merge and
one final visibility ray are retained. A UAV barrier after both disjoint
resolve halves protects future reservoir reads without serializing the halves.

## Checks

- Release builds passed in the implementation and main checkouts. A subsequent
  descriptor correction (texture SRV union, then a non-shader-visible staging
  heap as the bindless copy source) also built in both.
- DXC compiled all eight allocation/trace/history kernels and twelve enhanced
  resolve combinations: legacy/bindless, generic/terrain/split, with and without
  VRRT. The geometry-only WRS helper also compiled independently.
- All five default FXC resolve variants are byte-identical to the pre-change
  working tree (generic, terrain, terrain-only, and both tile-list variants).
- CPU tests: 54 of 56 passed. The two failures already exist in the baseline:
  GameSettingsTests expects RR upscaling off while its current default is on;
  ResolveShaderCanaryTests expects 128100 bytes while the pre-change working
  tree already produces 128336. The new settings/persistence/budget assertions
  passed. Golden files and unrelated defaults were left as supplied.
- Bistro validation uses the authored camera, 1600x900, Lumen on, 50% primary
  budget, and RR/DLSS off. The first attempt exposed an invalid texture SRV
  caused by stale buffer-union fields. It produced a device error and stale
  GPU counters; its timings and image are not validation evidence.

Corrected Bistro run under the D3D12 debug layer (VRRT + CGNS on from frame 30,
579 VRRT frames): no device removal, primary rays 720000 = budget 720000, and
no message type absent from a VRRT-off baseline. Invalid copy-source messages
are 28.1/frame in both (pre-fix VRRT: 72.7/frame); the remaining ones and the
ClearUAV/barrier messages are pre-existing. Both prototypes remain off by default;
CPU checks and shader compilation cannot establish rendered parity. Motion,
odd resolutions, RR, and a separate Forward/VB pixel comparison still need QA.

## Reproducing captures

Use the existing `SGE_CAPTURE_POSE`, `SGE_CAPTURE_LEVEL`, `SGE_CAPTURE_FRAMES`,
and `SGE_CAPTURE_PATH` controls. `SGE_CAPTURE_LUMEN_TOGGLE=0` enables Lumen.
`SGE_CAPTURE_VRRT_TOGGLE=600,1200` records off/on/off; the matching emissive
control is `SGE_CAPTURE_CGNS_TOGGLE`. Use `SGE_CAPTURE_ALL_STATES=1` for every
frame's state and `SGE_PROFILE_DUMP=1` for GPU pass timings. State records include
`vrrt`, `vrrtBudget`, `vrrtRays`, `vrrtMeasuredBudget`, and `cgns`. Counts are
frame-lagged; compare measured rays to measured budget and discard transitions.
Cold background shader compilation can delay first activation, so confirm
`vrrt=1` before treating a capture as a VRRT measurement.

`SGE_VRRT_DEBUG=1` shows main samples per pixel (0..4); `=2` shows temporal
gradients. Keep the main debug view at zero to run these shader diagnostics.
Repeat at odd resolutions, with camera motion/disocclusions, thin geometry,
and with RR enabled and disabled. Run `SGE_VISIBILITY_TEST=1` to check the
reference Forward/VB toggle.

Sources: [Infinity Ward VRRT presentation](https://advances.realtimerendering.com/s2026/content/SIGGRAPH2026%20-%20Micha%C5%82%20Olejnik%20-%20Variable%20Rate%20Ray%20Tracing%20in%20COD%20MW4.pdf),
[Junkins et al. neighbor selection](https://research.nvidia.com/labs/rtr/publication/junkins2026compatibility/).
