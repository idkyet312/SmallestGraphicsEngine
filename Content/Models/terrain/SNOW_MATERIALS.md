# Snow maps and terrain materials

Open `snow1` from **Custom Levels** or load `Content/Levels/snow1.json` in the
level editor. It is a small snowy island with sculpted hills, a cabin, and an
on-foot spawn. Its map type is `snowy` and automatic grass/flowers are disabled.
Snowy maps with an on-foot spawn open in daylight so the hub's sunset lighting
does not turn the snow brown.

At the top of the editor's **Terrain Sculpt** panel, use **Auto material type**
to choose Tropical, Snowy, Custom, or Grass Ground. Expand **Auto Material Settings**, then
expand a layer and enable **Custom textures** to
assign Color, Normal (OpenGL), Roughness, Ambient occlusion, and Height maps.
Use **Browse** to select existing Content textures or import images from another
folder; imported images are copied into `Content/Textures/Terrain/Imported`.
You can also enter a relative `Content/...` path and press Enter. Texture changes
apply to the viewport; automatic foliage changes apply on Save/Play.

The four layer slots retain the existing automatic height/slope selection:
inland, patches, shore, and cliffs. Snowy uses the supplied snow_02 2K
material for the three snow layers and the existing dark-rock scan on cliffs.
Painting uses the same slots. Height/displacement maps control surface-layer
interlocking; terrain sculpting controls the island's actual elevation. The
projection scales and automatic blend thresholds remain the engine's existing
values. Existing maps without the new settings keep their tropical materials.

Custom layers are retained when changing map type. Disable **Custom textures**
or use **Reset layer to map preset** to restore that type's defaults. An empty
normal map uses a flat normal, an empty roughness map uses generated roughness,
and empty AO/height maps use neutral values. A missing image logs a warning and
uses generated fallback pixels. A texture upload failure retains the previous
GPU material. The renderer pauses only when the assigned texture set changes.

Grass Ground uses the supplied `grass_ground` 2K material for inland grass and
`coast_sand_05` 2K material for sand, keeping Tropical's dirt and rock. Automatic foliage
stays enabled. Custom layer assignments still take priority over the preset.

In JSON, `mapType` is `tropical`, `snowy`, `custom`, or `grass_ground`; `terrain.autoFoliage`
controls procedural ground cover. Optional `terrain.materials` contains exactly
four entries. `null` selects that slot's map preset; an object assigns a custom
layer with `albedo`, `normal`, `roughness`, `ambientOcclusion`, and `height`
paths. Every path is relative to `Content/`. Authored foliage entities remain
available even when automatic foliage is disabled.

The bundled snow_02 images came from the user-provided
`C:\Users\Bas\Downloads\snow_02_2k\textures` folder and are packaged in
`Content/Models/terrain/snow_02_2k`. Color, OpenGL normal, roughness, AO,
and displacement are included. Normal and roughness EXRs were converted to
8-bit PNGs by clamping their linear channels to [0, 1] and rounding to [0, 255],
without a gamma transform or normal green-channel flip. The other three images
are unchanged copies. The map uses relative paths and does not depend on Downloads.
