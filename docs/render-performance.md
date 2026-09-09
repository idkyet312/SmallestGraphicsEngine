# Renderer performance changes

The CPU path retains instance-matrix scratch buffers, batch transform storage,
and fallback particle-sort storage between calls. Particle distances are computed
once per included particle; the descending comparison and input order are unchanged.
Scratch buffers are thread-local and consumed before recursion or the next view.
Batch entries release scene-node references after flushing.

`Scene::cacheSpotShadows` is off by default. Enable **Cache Static Spotlight
Shadows** in the shadow controls to test it. It allocates one additional 48 MiB
depth atlas on first use and retains it across toggles. Allocation failure falls
back to the original path without retrying every frame.

Only crate/prefab static depth is cached. Terrain remains live because its mesh
depends on the camera. Vehicles, destruction, characters, wind-driven foliage,
barrels and projectiles also remain live. The live atlas is restored from static
depth before drawing them, preventing trails from previous caster positions.

Moving lights bypass caching. After a light's matrix remains exactly unchanged
for two calls, its static depth is populated and reused. Changes to static draw
inputs (including transforms, model/LOD, geometry buffer addresses, shadow flags
and alpha materials) invalidate all static slices. Existing full sun-cache
invalidation also invalidates this cache. Code modifying GPU geometry or texture
contents in place must call `InvalidateCachedCascades()`.

When this toggle is enabled, the sun pass continues allocating shadow upload
slots after the spotlight pass; it must not overwrite pending spotlight draws.
The disabled path retains its existing behavior.

## Validation

Build with `./build.ps1 -Configuration Release -NoRun` and run
`ctest --test-dir build -C Release --output-on-failure`.

For the existing Forward-to-visibility smoke test, launch from `build/` with:

```powershell
$env:SGE_VISIBILITY_TEST = '1'
$env:SGE_VISIBILITY_TEST_FULL = '1'
$env:SGE_FORCE_NIGHT = '1'
$env:SGE_SPOT_SHADOW_CACHE = '1'
$env:SGE_PROFILE_DUMP = '1'
./GraphicEngine.exe
```

`visibility_smoke.log` and `visibility_cpu.log` report `spot_cache`,
`spot_cached` and `spot_refreshed`. `Spot Shadows` measures the whole spot pass;
`Spot Cached Path` includes cache refresh/copy and live draws for a cached slice.
These are nested scopes inside `Shadow`, so do not add their times together.

Compare with `SGE_SPOT_SHADOW_CACHE` unset in the same scene. Smoke completion
confirms execution, not pixel parity or a speedup. Before enabling by default,
visually compare stationary and moving lights, moving casters, prefab transform
and LOD edits, alpha-material edits, scene changes, and toggle off/on transitions.
Use repeated steady-state timings; a single frame is not a performance result.
