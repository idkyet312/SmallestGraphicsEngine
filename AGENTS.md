# AGENTS.md

Shared context for every agent working on this engine. Read this before
touching renderer code — it exists so the architecture does not have to be
re-explained each session.

## Engine

DX12 renderer, C++17, Windows. Header-heavy: most render code lives in
headers under `src/render/dx12/`, so a change there recompiles `main.cpp`
(the single big TU). Expect ~2 minute builds.

A DX11 path exists in `build-dx11/` but is not the active target.

## Rendering architecture

- **Visibility buffer is the default primary-visibility path.** `visbuf_ps.hlsl`
  writes `uint2(drawCallID + 1, SV_PrimitiveID)`; GPU-driven culling via
  `ExecuteIndirect`. `visbuf_resolve_cs.hlsl` (~950 lines) is a fused
  gbuffer + lighting compute resolve. Forward (`ForwardRenderer.h`) remains
  as a fallback and as the parity reference.
- **Enhanced visuals** (`scene.enhancedVisuals`, off by default) is a second
  resolve PSO: the *same* shader source compiled at `cs_6_5` via DXC with
  `SGE_ENHANCED_VISUALS`. Adds inline RayQuery sun shadows and ray
  classification. Needs DXR Tier 1.1.
- **DXR / DDGI** — real BLAS/TLAS. Hit groups now carry per-geometry local
  root arguments (vertex/index SRVs + material constants), so closest-hit
  shades the real surface.
- **Planned:** ray classification with cheap-tier fallthrough, SVGF, RT
  reflections, RT geometry LOD, radiance cache, ReSTIR path reuse.

## Conventions

- **Matrices:** row-vector math on the CPU (`mul(vector, matrix)` in HLSL),
  transposed on upload — see `ShaderDX12.h` `XMMatrixTranspose` at the fill
  site. If you add a matrix to a cbuffer, transpose it the same way.
- **Handedness:** left-handed. `XMMatrixPerspectiveFovLH`, `LookAtLH`,
  `right = up × front`.
- **cbuffer layout is a hand-maintained contract.** The HLSL struct and its
  C++ mirror must match field-for-field, in order. Adding a field to one side
  only shifts every field after it and silently corrupts unrelated state.
  This has already happened once (a `previousModel` insert misaligned palm
  wind). Always change both sides in the same edit.
- **Default frame must stay byte-identical.** New rendering work ships behind
  a toggle, off by default, until measured. The FXC `cs_5_1` resolve path is
  load-bearing — do not port or "clean up" it opportunistically.
- Comments explain *why*, not *what*. Match the surrounding density.

## Performance rules

- No CPU↔GPU sync in the hot path. `WaitForGPU` / `WaitForGPUAllFrames` are
  for rare events only (scene rebuild, resize, shutdown).
- Never blocking-map a resource the GPU may still be writing. Readback is
  frame-lagged — see `UpdateRayMaskStatistic` for the pattern to copy.
- No per-frame allocation of D3D resources. Create once, reuse per frame slot.
- No unnecessary BLAS rebuilds — geometry is static; BLAS is keyed by
  `sourceHash`.
- Every new render pass gets a `ProfilerDX12::Scope` GPU timer.
- Resources that alias frame slots need per-frame copies, not one shared
  object (the async-compute allocators are the recent example).

## Build

```
./build.ps1 -Configuration Release -NoRun
```

Flags: `-Configuration Debug|Release|RelWithDebInfo`, `-NoRun`, `-Jobs N`.
Omit `-NoRun` to launch the app. Build dir is `build/`.

## Shaders

Compiled two ways, deliberately:

- **Offline via DXC** at CMake time (`as_6_5`, `ms_6_5`, `ps_6_5`, `lib_6_3`)
  — see [CMakeLists.txt](CMakeLists.txt) around L155-272.
- **Runtime via FXC** (`cs_5_1`, `vs_5_1`, `ps_5_1`) through
  `ShaderCacheDX12`, with a disk cache. The SM6 runtime path
  (`CompileCachedDXC`) is salted so SM6 blobs cannot alias FXC ones.

Shader edits are picked up by rebuilding; `build.ps1` copies
`shaders/` into the build dir.

## Test

```
ctest --test-dir build -C Release
```

10 targets, sources in `tests/`, one target per file, registered in
[CMakeLists.txt](CMakeLists.txt). They are CPU-side (level, prefab, asset,
probe-layout) — there is no GPU test harness.

**Render regressions are caught by the in-app parity harness, not ctest:**

- `vb.validationMode` renders Forward into the VB target with MSAA/fog/FXAA/
  TAA/bloom disabled on both sides for pixel comparison. This is the primary
  gate for any resolve change.
- `SGE_VISIBILITY_TEST=1` runs the automated Forward→VB smoke toggle, logs to
  `visibility_smoke.log`.
- `SGE_SERIAL_COMPUTE=1` forces async compute to serialise — the bisection
  tool for telling a scheduling bug from a shading bug.

## Escalate before doing these

Stop and ask rather than deciding alone:

- Redesigning a public renderer API, or changing resource ownership.
- Introducing or changing GPU synchronization, queue scheduling, or
  descriptor lifetime rules.
- Changing BLAS/TLAS architecture or the shader-table layout.
- Replacing an algorithm specified in an approved plan.
- Touching the default (FXC `cs_5_1`) resolve path.
- The same failure recurring 2-3 times — the diagnosis is wrong, not the fix.
- Any measurable GPU regression.

## Repo notes

- Root dir accumulates stray `*.log` / `*.obj` / `*.png` debug artifacts from
  ad hoc runs. Ignore them; they are not project structure.
- `graphify-out/` holds a knowledge graph — prefer `graphify query "..."` over
  broad grep for codebase questions.
- Worktrees: `../Engine-Agent` on `agent/render-feature` is the implementation
  checkout. The main checkout stays on the review branch.
