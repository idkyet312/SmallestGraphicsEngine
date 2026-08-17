# Phase 5c — SVGF à-trous spatial filter — run on V4-Pro

First: `git merge --ff-only OGEngineV2`

Branch `agent/render-feature`, HEAD `3646802`.

Add the spatial half of SVGF: an edge-aware à-trous wavelet filter driven by
the variance 5b already accumulates. Toggle, off by default.

**This is the last stage of the denoiser.** 5b made the signal converge over
time; 5c makes it converge in *space*, which is what lets the accumulation
window come down without the noise returning.

## Why this task exists

5b converges and is stable — human-confirmed at **64 accumulation frames**,
with *Refl Roughness Cut* and *RT Threshold* both at 1.00 (≈63% of pixels
tracing). That is a long history, and long histories ghost: geometry smears
when it moves, because old samples stay in the average too long. The fix is not
a better temporal pass, it is spatial filtering — clean up each frame's noise
with its neighbours, so fewer frames of history are needed.

Concretely: with 5c working, `svgfMaxAccumFrames` should drop substantially —
target 8-16 — and still look as clean as 64 does now, with visibly less
ghosting in motion. **That trade is the deliverable.** A 5c that looks clean
but does not let the frame count come down has not earned its cost.

> **The floor has not been measured.** Nobody has yet lowered
> `svgfMaxAccumFrames` on the current build to find where grain returns without
> 5c. **Measure that first and report it** — it is your baseline, and without it
> "5c reduced the frame count" is unfalsifiable. If the floor turns out to
> already be low, say so: that would mean 5c has less to win here than assumed,
> which is a real finding.

## What 5b already gives you — verify, do not rebuild

- **Variance is stored and populated.** `svgfHistoryMoments` (t81 read, u5
  write) holds `E[x²]` in `.rgb` and the accumulation count in `.w`.
  `svgfHistoryColor` (t80/u4) holds `E[x]`. Variance is `E[x²] − E[x]²` — the
  moments are already there, you do not need to accumulate anything new.
- **Surface-ID history** for validity (5a), reused by 5b's reprojection.
- **Normals and roughness** per pixel in `outputNormalRoughness`.
- **A test level and a debug view**, see below.

## Design

Standard SVGF à-trous: N iterations (start with 3-5) of a 5×5 cross-bilateral
wavelet, doubling the tap stride each pass. Each tap is weighted by three
edge-stopping functions, multiplied together:

1. **Depth** — reject taps across a depth discontinuity, scaled by the depth
   gradient so slanted surfaces are not over-rejected.
2. **Normal** — `pow(max(0, dot(n_centre, n_tap)), σ_n)`.
3. **Luminance** — `exp(-|l_centre − l_tap| / (σ_l · sqrt(variance) + ε))`.
   **This is the term that makes it SVGF rather than a bilateral blur.** High
   variance widens the luminance tolerance, so noisy regions filter
   aggressively and converged regions are left alone.

Filter the **variance alongside the colour**, with the squared weights. A
filtered variance estimate is what keeps later iterations from over-blurring
regions the earlier ones already cleaned.

> The luminance weight is the whole point. If you find yourself writing a
> filter that ignores variance, stop — that is a bilateral blur, it will smear
> detail uniformly, and it will not let the accumulation window come down.

### Where it goes

5b accumulates **inline inside the enhanced resolve**, which is unusual but
deliberate: it avoids a frame of lag and keeps the denoised value available for
`reflectionIBL` in the same dispatch.

Multi-iteration à-trous cannot work that way — each iteration must see the
completed output of the previous one across the whole image, which needs a
barrier between passes. So 5c is a **separate compute pass** after the resolve,
ping-ponging between two scratch textures.

That means the reflection contribution must be separable from the resolve's
output. Decide how and **say which you chose**:

- Write the denoised reflection to its own texture in the resolve, filter it,
  then composite; or
- Filter the full lit result, accepting that non-reflection pixels are touched.

The first is correct but costs a texture and a composite step. The second is
cheaper and wrong in principle — it will filter shadow and lighting noise that
has no variance estimate behind it. **Prefer the first.** If you choose the
second, justify it with a measurement, not an argument.

## Constraints

- **Toggle off must be byte-identical.** Default resolve = **42020 bytes**.
  Verify by compiling, not by trusting guards:
  ```
  dxc -T cs_6_0 -E main -I shaders shaders/visbuf_resolve_cs.hlsl
  ```
- The enhanced resolve is compiled at **runtime** by dxcompiler, so a clean
  CMake build does **not** prove the HLSL is valid:
  ```
  dxc -T cs_6_5 -E main -D SGE_ENHANCED_VISUALS=1 -I shaders shaders/visbuf_resolve_cs.hlsl
  ```
- **`clustered_dx12_ps.hlsl` / `_vs.hlsl` are compiled into many PSOs** — mesh,
  terrain, HDR, MSAA, motion. Any struct edit must be `#ifdef`-guarded on the
  variant that needs it. An unguarded field breaks the PS/MS signature match and
  **D3D12 reports nothing at all**: the draw silently disappears and `supported`
  quietly stays false. This is how an earlier phase lost the terrain. Canaries:
  | File | Bytes |
  |---|---|
  | `terrain_ps.cso` | 45836 |
  | `terrain_ps_hdr.cso` | 42264 |
  | `mesh_ps.cso` | 32780 |
  | `mesh_ps_hdr.cso` | 29164 |
  | `mesh_ps_motion.cso` | 29924 |
- Do not change 5b's temporal accumulation or 5a's validity rule. Consume both.
- Do not change how `motionTexture` is written. 3b owns that.
- `ProfilerDX12::Scope` per iteration, so the cost of each is visible. No
  per-frame allocation, no CPU↔GPU sync, no readback.

## The every-frame rule — read this before changing what feeds the accumulator

5b originally accumulated only inside `if (reflectionHit)`. A pixel's hit/miss
status flips frame to frame — that is what the stochastic ray does — so miss
frames bypassed the accumulator and shaded from the raw environment probe. The
image flickered *between* the converged value and the raw probe, which no
accumulation count can damp because the flicker is between two code paths
rather than within the average. It presented as grain that survived any
`svgfMaxAccumFrames`, and it cost real time to diagnose because it looks
exactly like "the denoiser is too weak".

Fixed in `3646802`: a miss carries the probe value into the average, because a
miss is a legitimate sample of the reflection integral ("sky along that
direction"), not an absent one.

**If 5c introduces any path that conditionally skips filtering or
accumulation, it can recreate this.** A pixel must produce a sample every
frame. Skipping is what breaks convergence.

## The double-commit trap — read this before touching ShadeSurface

5b had a bug worth knowing about, because 5c sits in the same code. Edge AA
calls `ShadeSurface` **twice for one pixel** (two sub-samples). The temporal
pass originally committed history on both calls, so each pixel blended into
itself twice per frame and its accumulation count advanced twice — the EMA
converged at the wrong rate on silhouettes, exactly where reprojection is
least reliable.

The fix was a `commitTemporalHistory` flag: sub-samples read history without
storing it, the centre surface commits once, and where edge AA takes over the
shading path entirely a separate commit runs from the centre surface (without
it, history would never advance on those pixels at all). The debug view never
commits, so inspecting it cannot corrupt the accumulating history.

**If 5c adds any per-pixel state, it inherits this hazard.** Any write from
inside `ShadeSurface` happens twice per pixel under edge AA.

## Root signature warning

An earlier phase desynced three call sites by inserting a root parameter
mid-table and shifting the tail. **Append, do not renumber.** If you must
insert, grep every `SetGraphicsRoot*`/`SetComputeRoot*` call site and fix them
in the same edit — a CBV written into an SRV slot page-faults at VA 0x0 and the
DRED breadcrumb points at whatever `Dispatch` ran next, not the real cause.

Same rule for `EnhancedVisualsBuffer` (b5): HLSL cbuffer and its C++ mirror in
`UpdateEnhancedConstants` must match field-for-field, in order. Append only.

## Test setup

`Content/Levels/RTReflectionTest.json` — a tight ring of metal houses, outer
wood ring, two Humvees, a helicopter, barrel clusters, flat terrain. Dense
static geometry so reflection rays have something to hit. The island levels are
a poor test bed: over open water most rays miss.

Load from the Level Editor's Load list (directory scan, alphabetical). Paths
resolve relative to the working directory, so run the exe from
`Engine-Agent\build`.

Settings that exercise the denoiser hardest: *Refl Roughness Cut* 1.00 and
*RT Threshold* 1.00 — every surface eligible, ~43% of pixels tracing.

Debug views: **4** = raw reflection rays (black/blue/green + flashing red
frame ramp), **5** = denoised reflection colour. Add 5c's own view as **6**;
do not repurpose the existing ones.

## Worktree gotchas — already paid for

- `Engine-Agent` is a `git worktree`, so gitignored files were never copied in.
  A missing `assetcache/` (113 files, `.gitignore:92`) causes a **GPU page fault
  at VA 0x0 during level load**, DRED breadcrumb pointing at a mip-generation
  `Dispatch`. It looks exactly like a shader bug. If the app dies loading a
  level, check `assetcache/` before reading any shader.
- Do not bulk-copy `Content/` from the main checkout — it rewrites 60+ tracked
  level files and pollutes the diff.
- Build with `./build.ps1 -Agent`. A bare `./build.ps1` builds the *other*
  worktree.
- New content files need a CMake reconfigure to reach `build/` — the file glob
  is resolved at configure time.

## Done when

- [ ] `./build.ps1 -Agent -Configuration Release -NoRun` clean;
      `ctest --test-dir build -C Release` 10/10.
- [ ] Enhanced resolve compiles explicitly at cs_6_5.
- [ ] Default resolve = **42020 bytes**, stated as a measured number.
- [ ] All five canaries match the table, stated as measured numbers.
- [ ] **Launch on `RTReflectionTest` and confirm it renders.** A green build
      proves nothing: an earlier phase shipped four bugs behind a clean build
      and 10/10 tests, one of which deleted the ground.
- [ ] Iteration count and per-iteration cost reported from the profiler.
- [ ] Committed on `agent/render-feature`, `Content/` untouched.

## Report back

Build/ctest output. DXBC sizes each beside its baseline. `git diff --stat`.

Say which compositing approach you chose (separate reflection texture vs
filtering the full result) and why.

**State plainly what you could not verify.** Whether 5c earns its cost is a
judgement about ghosting versus noise in motion, and it needs a human. Do not
claim it looks right.

**If a decision was made on reasoning rather than evidence, say which.** In an
earlier phase a change was kept because the argument was sound, without testing
whether it was needed. Legitimate, but it must be visible.

If you departed from this spec, say so and why. The spec was written without
compiling anything; a measurement can prove it wrong and that is fine. An
unannounced deviation costs the reviewer the time to rediscover it.

## Escalate if

- The à-trous pass cannot be separated from the resolve without restructuring
  something you were told not to touch.
- Variance from 5b turns out not to be usable as an edge-stopping input (e.g.
  it is zero or saturated in practice) — that is a 5b defect surfacing, say so
  rather than working around it.
- The filter looks clean but `svgfMaxAccumFrames` cannot come down — report it
  as a negative result. That is a real finding, not a failure to hide.
- Toggle-off DXBC moves at all and you cannot make it stop.
- Same compile error 2-3 times, or any silently-dropped draw you cannot explain.
