# Shooting Target 01

Imported from the supplied `SM_ShootingTarget_01.fbx` and its PBR/2048 texture
set. The original FBX is preserved here. It contains no texture bindings.

`ShootingTarget_01.glb` is the runtime model: one closed mesh, 36 triangles,
with embedded base-color, normal, and metallic/roughness textures. Geometry
was imported through Assimp with triangulation, node transforms baked,
shared vertices joined, and normals/tangents generated where needed. UV V
coordinates were converted to glTF's texture convention.

The FBX's object scale produces bounds about 40.29 x 88.56 x 1.30 units.
The GLB is normalized to 1.75 m tall, centered in X/Z, and grounded at Y=0:
approximately 0.796 x 1.75 x 0.0256 m. The prefab preserves that size.

The metallic/roughness texture packs R=255, G=roughness, B=metallic. The
source 2K maps and the packed map remain under `Textures/` for editing.

Prefab: `Content/Prefabs/Props/shooting_target_01.json`
Palette name: **Shooting Target 01**; ID: `props/shooting_target_01`.

The prefab's `shootingTarget` component maps the two printed bullseyes to
model-local X/Y coordinates. Each center has its own horizontal and vertical
radius, with shared rings awarding 10, 9, 8, then 7 points outward. Hits outside
the rings or on the wooden back award zero. `faceZ` identifies the printed
face in the engine's imported model coordinates.

Only local player bullets score. The HUD shows the range total, scoring hits,
and the latest center/points plus that target's accumulated points. Scores
survive prefab rebuilds and reset for a new level run; they are separate from
career money, XP, and combat statistics.
