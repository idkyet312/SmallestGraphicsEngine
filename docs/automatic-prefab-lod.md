# Automatic prefab LOD

Enable **Rendering Settings → Automatic prefab LOD** to use generated LODs.
The game toggle defaults to off and is session-only. The NATO shelter in
`Content/Levels/Base.json` is the first opted-in prefab.

To opt in another static prefab, enable **Generate automatic LODs** in its
prefab editor and save/reload, or set `components.staticMesh.automaticLod`
to `true`. Authored `lods` take priority over generation.

Two meshes are generated during model loading, targeting 50% and 20% of
the original indices, with relative geometric error limits of 0.002 and
0.005. Mesh boundaries are locked. The original hierarchy, material seams,
and vertex attributes are retained. Skinned and small primitives retain
their original geometry, as do primitives that cannot be reduced within
the error limit. Collision always uses the original mesh.

Selection uses distance beyond the original bounding sphere, adjusted for
instance scale. The thresholds are 40 and 100 metres at unit scale, with
10% hysteresis when returning to higher detail. Instances in a prefab batch
share the detail required by the nearest instance. Turning the game toggle
off restores the original mesh on the next update. Generated resources stay
resident in the existing prefab cache; switching allocates no GPU resources.

Validation on the NATO shelter: 51,366 original triangles, 26,918 at the
first tier, and 13,772 at the second. Release build and all 24 CTest targets
passed. Base loading confirmed generation and original collision loading.
These counts are geometry reductions, not measured GPU performance gains;
visual transition quality and GPU timings still require in-game evaluation.
