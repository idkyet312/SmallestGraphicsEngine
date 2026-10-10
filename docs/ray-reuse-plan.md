# Ray allocation and emissive neighbor selection

Both features are experimental and off by default. Implementation checkout:
`../Engine-RayReuse`, branch `agent/ray-reuse`. Existing local edits are the
baseline and are excluded from the feature patch. The feature changes are also
applied to the main working tree for review; its branch is unchanged.

Status: the user approved the synchronization scope. Both implementations are
connected to off-by-default video settings. Validation results are recorded in
`docs/ray-reuse-validation.md`.

## Variable rate Lumen tracing

The current Lumen trace is fused into the full lighting resolve. The existing
half-resolution experiment still launches a ray in almost every wave and its
recorded timings show no speedup. A variable-rate branch in that same dispatch
would preserve the wave latency problem.

Use a separate optional compute pipeline on the existing graphics command list:

1. Reconstruct primary positions, normals, and persistent surface keys from
   visibility/depth before lighting; reproject a dedicated diffuse GI history.
2. Trace a sparse gradient prepass with the previous sample's random seed so
   lighting changes can be distinguished from Monte Carlo noise. Count those
   rays against the budget.
3. Classify requested rates from disocclusion, history length, and gradients;
   build a rate histogram and solve a screen-wide ray budget on the GPU.
4. Compact pixel/sample jobs and produce indirect compute dispatch arguments.
   Dispatch the GI ray shader over the compact jobs, rather than all pixels.
5. Accumulate only actual samples into GI history; reconstruct unsampled pixels
   with validated history or compatible spatial samples before lighting consumes
   irradiance. The resolve applies its own albedo, AO, and GI intensity.

The budget caps primary diffuse-bounce rays, including gradient rays. Traversal,
hit shading, and hit shadow rays still vary in cost: a ray cap is not a guarantee
of fixed milliseconds. Expose ray count and GPU timers to measure both.

Allocate private work buffers and result textures once per frame slot and
recreate through the existing resize path. Dedicated history retains its own
validity and never advances in a scope or debug view. Append bindings only to
optional SM6 pipeline signatures/heaps. Keep full-resolution enhanced and FXC
reference shaders intact. Do not alter BLAS/TLAS or shader tables.

This requires new UAV barriers, UAV-to-SRV transitions, indirect-argument
transitions, and optional descriptor tables. It uses the existing graphics
queue, without CPU waits or new cross-queue synchronization. AGENTS.md explicitly
requires approval before changing GPU synchronization or descriptor lifetime
rules; this plan is the concrete scope of that approval.

## Emissive ReSTIR compatibility-guided neighbors

Add one spatial donor to the existing temporal reservoir, selected from 16 disk
candidates with A-Chao weighted reservoir sampling. The compatibility score is
`exp(-distance / s) * max(dot(n, ni), 0)^8`, with
`s = distanceToCamera * sqrt(0.05 / pi)`, following Junkins et al. (2026).
Only primary geometry enters neighbor selection; emitter radiance, reservoir
weight, and visibility never influence it.

Reconstruct candidate geometry from the immutable visibility/depth inputs.
Reproject each chosen donor with its previous model/wind position and validate
its persistent namespace in the existing previous-surface texture. Read donors
only from the previous frame's emissive reservoir parity. Reevaluate the target
at the receiving surface, merge with the existing RIS weights, and run the
existing single final visibility ray. No current-frame neighbor UAV reads.
No additional reservoirs, geometry history, or descriptor slots are needed.

The existing emissive reservoir remains in UAV state across frames without an
explicit reservoir UAV barrier. Add a barrier after both disjoint resolve halves
only when this experimental mode is active, before next-frame spatial reads.
That synchronization edit also falls under the AGENTS.md approval requirement.

## Validation

Compile enhanced legacy/bindless and terrain/generic permutations with DXC.
Compare FXC reference blobs against the saved source baseline byte-for-byte.
Build Release and run the CPU tests. Use the in-app Forward/VB parity harness
for the reference. Capture fixed-camera off/on/off GPU timings and images,
motion/disocclusion, thin geometry, odd resolutions, scope/debug views, and
RR/SVGF before enabling either feature by default. Stop on a measured GPU
regression as required by AGENTS.md.

Sources:
- [Infinity Ward SIGGRAPH 2026 VRRT slides](https://advances.realtimerendering.com/s2026/content/SIGGRAPH2026%20-%20Micha%C5%82%20Olejnik%20-%20Variable%20Rate%20Ray%20Tracing%20in%20COD%20MW4.pdf)
- [Compatibility-Guided Neighbor Selection for ReSTIR](https://research.nvidia.com/labs/rtr/publication/junkins2026compatibility/)
- [Authors' reference implementation](https://github.com/orion-junkins/ReSTIR-CGNS)
