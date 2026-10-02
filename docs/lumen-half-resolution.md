# Half resolution Lumen GI

Enable **Experimental Half Resolution Lumen GI** below **Lumen Global Illumination** in the
video settings. The option is off by default and persists as
`LumenGIHalfResolution=1` in `settings.ini`. GPUs without wave operations retain
full-resolution GI and the control is disabled.

The enhanced resolve traces one diffuse bounce for each active 2x2 pixel block
and reconstructs the other pixels from that irradiance sample. It rotates the
representative pixel each frame. A different surface namespace, normal, plane,
or excessive world-space separation causes an independent ray, preserving thin
foreground geometry and surface discontinuities. Actual ray count therefore
depends on the view; one-quarter is the interior-of-a-surface target.
The shared ray uses a stable block seed, preserving the full sampling sequence
even though its representative changes each frame.

Sharing happens before each pixel applies its own albedo, AO, and lighting
scale. All pixels still feed the existing temporal/spatial denoiser or Ray
Reconstruction, including pixels that reused a sample. The GI ray mask records
only pixels that actually traced. Changing the option resets temporal history.
There is no additional render pass, texture, descriptor, or queue operation.

WaveMatch groups only active lanes with the same pixel-block key. Every lane
reconverges before reading the representative's ray result, so sky holes,
terrain/generic dispatch splits, and edge-AA sub-samples cannot read an inactive
lane. The implementation does not assume a lane ordering or wave size.

The FXC resolve is unchanged: the new code and constants are inside the enhanced
shader guard. Before/after FXC blobs were byte-identical for both the base
variant and a terrain/virtual-shadow/tile-list variant. The C++/HLSL enhanced
constants both append one 16-byte block; the existing 256-byte frame allocation
is sufficient.

## Validation

Build with `./build.ps1 -Configuration Release -NoRun` and run
`ctest --test-dir build -C Release --output-on-failure`.

For a pinned-camera A/B capture, set `SGE_CAPTURE_POSE`, `SGE_CAPTURE_LEVEL`,
`SGE_CAPTURE_FRAMES`, and `SGE_CAPTURE_PATH` as for existing captures. Set
`SGE_CAPTURE_LUMEN_TOGGLE=0` to enable Lumen from capture frame zero. Use
`SGE_CAPTURE_HALF_GI_TOGGLE=0` for half resolution and `999999` for the reference.
`SGE_PROFILE_DUMP=1` enables the settled GPU pass dump. The capture state includes
`halfGI` and the actual `giRays` fraction. Compare repeated settled timings and
inspect a static view, camera motion, disocclusions, thin geometry, and toggle
off/on transitions. Test both Ray Reconstruction and the SVGF fallback.
For a single-run off/on/off timing comparison, set `SGE_CAPTURE_ALL_STATES=1`
and `SGE_CAPTURE_HALF_GI_TOGGLE=600,1200` with at least 1800 capture frames.
Discard frames near transitions; the GPU timers and ray-mask readback lag the
setting change. Capture state also includes total GPU frame time and the two
resolve shading scopes, avoiding sums of nested timers.

Scope views retain full-resolution GI because they do not accumulate the
main-view temporal history.

The current shader canary golden is already stale: it expects 128,100 bytes,
while the source saved before this change and the updated source both compile
to the same 128,280-byte default DXBC. This change does not refresh the golden.

## Measured result: keep the prototype off

The October 1 RTX 4060 capture used the RT Reflection Test level, a pinned
camera at (5, 5, 0), 1280x720 native Ray Reconstruction, Lumen enabled, DDGI off,
and the saved 0.05 reflection roughness cutoff. In one 1800-frame off/on/off run,
each column below is the median of 351 settled frames, excluding transitions.

| GPU scope (ms) | Off, before | Half resolution | Off, after |
| --- | ---: | ---: | ---: |
| Whole frame | 9.327 | 9.149 | 9.147 |
| Visibility Buffer | 1.080 | 1.124 | 1.105 |
| Generic shade | 0.612 | 0.633 | 0.618 |
| Terrain resolve | 0.202 | 0.215 | 0.203 |

There is no demonstrated whole-frame speedup: the returning reference is as
fast as the half-resolution mode. Both resolve shading scopes became slightly
slower. The prototype is off by default and is not a recommended optimization.
Static RR captures render without an obvious surface-edge leak; motion and
SVGF quality have not been validated, because the measured regression triggers
the repository's escalation rule.

The existing ray-mask statistic samples rows across the screen. Its row parity
aliases with the rotating 2x2 representative, so the reported GI coverage
alternates between approximately 0.078 and 0.306 while the option is on. Those
values do not establish an exact whole-screen ray reduction. Balanced row
parity or a full-mask offline count is needed before quoting that reduction.
Raw timings and images are in `build/lumen-validation/`; the settled timing
summary is `rr-results.json`.

## Proposed next implementation (approval required)

Replace wave sharing with a dedicated GI trace dispatch at ceil(width/2) by
ceil(height/2). It reconstructs a representative primary surface from the
visibility/depth buffers and traces only the diffuse bounce, using the current
material bindings, nearest-hit rules, hit shading, and stable block seed.
It does not run full direct lighting or reflections at reduced resolution.

Allocate irradiance and surface-guide textures once for each frame slot;
recreate them through the existing resize path. Keep ownership private to the
visibility renderer and retain full-resolution GI for scope views. The full
lighting resolve reads the half-resolution irradiance and uses depth, normal,
and surface identity to reject incompatible neighbours. It keeps per-pixel
albedo/AO and the current RR/SVGF consumers. Thin geometry with no compatible
sample gets a full-resolution ray instead of borrowed background light.

Record the trace on the existing graphics command list, with a new GPU timer
and the UAV-to-SRV transition required before the full resolve reads its result.
Append bindings only to the optional half-resolution PSO and its per-frame
descriptor heaps. Keep the full-resolution enhanced PSO and the FXC resolve
free of the new sampling branch, so the reference does not pay its register
cost. This introduces GPU synchronization and descriptors, which requires
approval under AGENTS.md; no changes to BLAS/TLAS or queue scheduling are planned.

Gate it with the same off/on/off test, static and moving-camera captures,
thin/disoccluded geometry, odd viewport sizes, scope views, and both RR/SVGF.
Keep the option off until GPU timings show a repeatable improvement and the
image checks pass.
