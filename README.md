# Smallest Graphics Engine

Experimental Windows graphics and physics sandbox built around DirectX 12. The current demo combines multiple rendering paths, destructible structures, rigid-body gameplay, procedural terrain, vegetation, and water in one first-person scene.

DirectX 12 is the primary path. A smaller legacy DirectX 11 target remains available through CMake.

## Highlights

### Rendering

- Clustered forward rendering with up to 64 dynamic point lights
- Dynamic Diffuse Global Illumination (DDGI) probe updates
- Directional shadow mapping
- Optional visibility-buffer/deferred path
- Optional DirectX Raytracing (DXR) path on supported hardware
- Mesh and amplification shader paths with raster fallbacks
- Meshlet generation through meshoptimizer
- Previous-frame depth occlusion for mesh shaders
- Procedural mesh-shader terrain
- HDR environment sky and image-based ambient lighting
- AgX/ACES-style tone mapping
- GPU mip generation
- Runtime renderer and debug controls through ImGui

### Scene and gameplay

- First-person camera, sprinting, shooting, automatic fire, and grenades
- NVIDIA Blast structure destruction with connected chunks and support chunks
- Box2D-based debris, ragdolls, ropes, floating objects, and collision
- Destructible palm trees and shootable ropes
- Terrain-aware destruction and physics
- Pool and ocean surfaces with waves, splashes, and buoyancy
- Dense procedural grass with distance culling and wind
- Projectile, impact-particle, smoke, muzzle-flash, and enemy-shot effects
- GLB/glTF and FBX import
- Procedural fallback textures for missing house materials

## Controls

| Input | Action |
|---|---|
| `W`, `A`, `S`, `D` | Move |
| Mouse | Look |
| `Shift` | Sprint in FPS walking mode |
| `Ctrl` | Hold to crouch |
| `Space` | Move upward or jump, depending on camera mode |
| Left mouse | Capture mouse or fire weapon |
| `G` | Throw grenade |
| `Tab` | Toggle UI and mouse capture |
| `C` | Release mouse |
| `F` | Toggle FPS walking mode |
| `Z` | Toggle meshlet wireframe |
| `F11` | Toggle fullscreen |
| `Esc` | Exit |

Rendering modes, lighting, DDGI, shadows, terrain, grass, weapon behavior, destruction debug drawing, and camera settings can be changed in the ImGui scene-controls window.

Level Editor includes a terrain-aware foliage painter. Select Grass, Dandelion, or Trees; choose Paint or Erase; then hold left mouse over terrain. Radius, density, stroke spacing, and random scale are adjustable. `B` toggles paint/select. Each drag stroke is one undo operation, and foliage is stored in level JSON.


Terrain Sculpt provides Raise, Lower, and Flatten brushes with adjustable radius, strength, and spacing. Sculpt strokes support undo/redo, affect rendering and gameplay collision, and persist in level JSON.

Level Editor **Save** and **Save As** open a native save dialog rooted in `levels/`. The main-menu **Custom Levels** button opens the matching JSON file browser. Selected levels launch through the full gameplay runtime.

## Requirements

- Windows 10 or later
- 64-bit DirectX 12-capable GPU and current driver
- Visual Studio 2022 with **Desktop development with C++**
- CMake 3.22 or later
- Windows SDK
- vcpkg
- Optional: `dxc` in `PATH` for Shader Model 6.5 mesh/amplification shaders
- Optional: DXR-capable GPU for raytracing mode

The DX12 build uses these vcpkg packages:

```powershell
vcpkg install `
  imgui[core,dx12-binding,win32-binding]:x64-windows `
  imguizmo:x64-windows `
  nlohmann-json:x64-windows `
  meshoptimizer:x64-windows `
  tinyexr:x64-windows `
  assimp:x64-windows `
  tinygltf:x64-windows
```

Box2D is fetched by CMake. NVIDIA Blast source is vendored under `thirdparty/blast/`.

## Build and run

Set `VCPKG_ROOT`, then configure and build from PowerShell:

```powershell
cmake -S . -B build `
  -A x64 `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DUSE_DX12=ON

cmake --build build --config Release
./build/GraphicEngine.exe
```

Convenience scripts:

```powershell
./build_and_run.ps1   # configure, build, run
./quick_build.ps1     # rebuild existing configuration, run
```

The scripts contain default Visual Studio and vcpkg paths. Edit them or use the manual commands above when your tools live elsewhere.

### Legacy DirectX 11 build

```powershell
cmake -S . -B build-dx11 `
  -A x64 `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" `
  -DUSE_DX12=OFF

cmake --build build-dx11 --config Release
```

The DX11 target is retained for reference and does not contain the full DX12 feature set.

## Rendering flow

Each frame currently records into one direct command list:

1. Transition and clear swap-chain backbuffer
2. Render HDR sky
3. Select DXR, visibility-buffer, or forward path
4. Render shadow map when enabled
5. Render terrain, scene geometry, destruction, vegetation, water, and effects
6. Capture depth for next-frame occlusion
7. Render ImGui
8. Submit, present, and advance frame fence

The engine uses per-frame command allocators and fence values. Command recording remains single-threaded.

## Project layout

```text
src/
  main.cpp                  DX12 application and demo orchestration
  DX12Core.h                device, swap chain, descriptors, frames, fences
  ForwardRenderer.h         forward scene rendering
  VisibilityBufferDX12.h    visibility-buffer passes and compute resolve
  RaytracingDX12.h          DXR pipeline, acceleration structures, dispatch
  DDGI_DX12.h               DDGI resources and probe updates
  ShadowMapDX12.h           directional shadow pass
  MeshShaderDX12.h          meshlet rendering and occlusion
  TerrainRendererDX12.h     mesh-shader terrain
  RuntimeWorld.h            active level, terrain, and prefab runtime ownership
  LevelRuntimeBuilder.h     authored-level to gameplay spawn/settings plan
  GameSession.h             screen lifecycle and level timer
  CombatSystem.h            prefab damage and combat interaction state
  EnemySystem.h             enemy roster and encounter cooldown state
  VehicleSystem.h           Humvee and helicopter state/damage rules
  GameSystems.h             compatibility umbrella for game systems
  GameRuntime.h             game composition root and shared lifecycle
  GameCommandQueue.h        typed cross-frame gameplay/editor commands
  LevelLoadingController.h  level-loading state machine and telemetry
  PlayerState.h             player health, regeneration, ammo, and reload state
  PlayerMovementTracker.h   horizontal locomotion speed and teleport filtering
  AnimationClipUtils.h      cross-clip translation-origin normalization
  FixedStepClock.h          bounded fixed-step destruction/physics clock
  RenderCoordinator.h       forward/visibility/DXR path policy
  DeferredReleaseQueue.h    fence-based GPU resource retirement
  DestructionDX12.*         Blast destruction and physics integration
  GLBImporter.*             glTF loading, textures, meshlets, scene merging
  FBXImporter.*             Assimp-backed FBX loading
  Scene.h                   runtime scene and gameplay state
  SceneGraph.h              scene hierarchy and mesh/material structures
  EngineUI.h                ImGui controls
  GrassField.h              grass generation and rendering
  WaterVolume.h             water simulation, rendering, and buoyancy
  PalmTrees.h               destructible palm simulation
  RopeSwing.h               rope and payload simulation

shaders/                    HLSL shaders and legacy GLSL files
models/                     runtime models and textures
thirdparty/blast/           vendored NVIDIA Blast source
CMakeLists.txt              DX12/DX11 targets and asset-copy rules
```

`src/main_dx11.cpp` and `src/main_dx12_legacy.cpp` are legacy paths. The primary executable builds from `src/main.cpp`.

## Known limitations

- Windows-only
- Main frame command recording is single-threaded
- Most gameplay functions still live in `main.cpp`; state ownership is separated first
- Runtime/render integration coverage remains limited; data and world-state tests exist
- Transient gameplay state is not serialized
- Renderer uses shared global DX12 state
- DX11 path lags behind DX12
- Experimental features depend on GPU capability and driver support

## Troubleshooting

### CMake cannot find packages

Confirm `VCPKG_ROOT`, install all packages listed above for `x64-windows`, delete the affected CMake cache, then configure again with the vcpkg toolchain.

### Mesh shaders do not activate

Install a recent DirectX Shader Compiler and place `dxc.exe` in `PATH`. CMake prints a fallback message when DXC is unavailable. Hardware and driver must support Shader Model 6.5 mesh shaders.

### Raytracing does not activate

DXR requires compatible hardware, Windows version, and driver. Use forward or visibility-buffer mode on unsupported systems.

### Assets or shaders are missing

Run from repository root or `build/`. CMake copies `models/` and `shaders/` beside the executable after a successful build.

### Black output or GPU validation errors

Build Debug, run under graphics debugging, and inspect console output. DX12 debug-layer messages are enabled in debug builds and dumped around sensitive upload operations.
