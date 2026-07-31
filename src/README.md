# Source layout

Each folder owns one subsystem:

- `animation/`: skeleton data, clips, sampling, procedural animation.
- `app/`: executable entry points and top-level orchestration.
- `assets/`: discovery, source import, cooked loading, and prefab metadata.
- `audio/`: decoder implementations and audio-facing helpers.
- `core/`: backend-independent utilities, timing, input, and commands.
- `editor/`: editor UI and level-editing workflows.
- `gameplay/`: combat, actors, player state, vehicles, and weapons.
- `level/`: authored level data, loading state, runtime plans, and world state.
- `navigation/`: Recast/Detour integration.
- `physics/`: destruction and physics asset integration.
- `render/`: renderer policy and backend-specific implementations.
- `scene/`: scene ownership, hierarchy, meshes, and materials.
- `world/`: terrain, vegetation, water, and environment structures.

Cross-subsystem headers currently use stable basename includes. CMake exposes
the subsystem include roots through `SGEProjectHeaders`. New files should live
in the owning subsystem instead of the `src/` root.
