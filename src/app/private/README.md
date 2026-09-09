# Application implementation modules

`../main.cpp` contains the application entry point, startup, staged loading,
frame loop, and shutdown. Its ordered includes assemble the private application
implementation below.

These headers contain whole functions, types, and state definitions. They are
included **once, by main.cpp only**, after the engine headers. They are not public
interfaces or independently compiled translation units. Keeping the existing
translation unit preserves internal linkage, static initialization order, and
the renderer's existing ownership and scheduling contracts. This split improves
navigation; it does not reduce compilation time or remove shared global state.

| Area | Modules |
| --- | --- |
| Shared application state and terrain helpers | `AppState.h`, `TerrainAndDamage.h` |
| Deployment and vehicle models | `VehicleModels.h`, `Deployment.h`, `InsertionVehicles.h` |
| Enemy simulation and combat | `EnemyVehicles.h`, `EnemySpawning.h`, `Combat.h`, `VehicleCombat.h` |
| Environment and scene rendering integration | `Environment.h`, `ProbeScene.h`, `WorldOverlays.h`, `ScopeView.h` |
| Loading and asset preparation | `LoadingState.h`, `PrefabThumbnails.h`, `Vegetation.h`, `PrefabAssets.h`, `SceneModels.h` |
| Runtime world and objectives | `PrefabWorld.h`, `WorldCollision.h`, `Objectives.h`, `LevelEnvironment.h`, `LevelSession.h` |
| Menus and presentation | `MenuTheme.h`, `Menus.h` |
| Material and destructible model preparation | `AssetMaterials.h`, `DestructibleModels.h` |
| Editor and diagnostics | `EditorRuntime.h`, `Diagnostics.h` |
| Window setup, input, and boot | `WindowAndGeometry.h`, `PlayerMovement.h`, `PlayerInteraction.h`, `WindowInput.h`, `Boot.h` |

The include order in `main.cpp` is intentional: later modules use declarations
and state defined earlier. Do not alphabetize it. Keep new helpers with their
owning feature and use the existing gameplay, level, and renderer APIs for
reusable functionality. A future conversion to separate `.cpp` files requires
explicit application interfaces and a review of global initialization order;
simply including these headers in another translation unit is not sufficient.

CMake's existing recursive header discovery includes these files in the IDE;
compiler include tracking rebuilds `main.cpp` when one changes.

Validate with `./build.ps1 -Configuration Release -NoRun` and
`ctest --test-dir build -C Release`. Rendering behavior still requires the
in-app parity harness when changed; CPU tests do not verify the frame.
