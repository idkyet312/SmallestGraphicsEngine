# Bistro Exterior

Open **Level Editor → Load → BistroExterior**, or run:

```powershell
./scripts/Open-Bistro.ps1
```

Hold **RMB** to look around; **WASD** moves, **Space/Q** moves vertically,
**Shift** increases speed, and the mouse wheel adjusts fly speed.

The toolbar's **Lighting** button opens the sun controls. **Sun azimuth** and
**Sun elevation** rotate the directional light; intensity and color change its
lighting. These are live preview settings, not saved level fields. Fog starts
disabled for this level. Disable Fog and Clouds for material inspection. Keep
emission at 1× when evaluating authored materials. Uncheck **Visibility Buffer**
to compare Forward. **F8** opens the existing profiler and GPU memory panel.

**Fog amount** adjusts density from clear (0) to thick fog (0.05); Ctrl-click
the slider to type a precise value. **Noon**, **Afternoon**, **Dusk**, and
**Night** apply the same time-of-day presets and per-time fog settings used
by deployment. Fog edits are retained per time for the current session.

**Ray-traced GI** toggles the existing Lumen bounce path; its status line shows
whether the DXR pipeline and ray scene are active. **GI intensity** controls the
bounce strength (the level default is 0.45×). This is separate from the
**DXR Lumen Lite** panel, which configures the level's optional probe GI.
**Ambient fill** adds an explicit diffuse fill in bindless VB and Forward,
including when ray-traced GI is active. It starts at zero and applies only to
the editor camera. **Environment / GI gain** remains the existing lighting
multiplier. Fill is independent of AO so it can reveal heavily occluded
surfaces; it is an inspection aid rather than simulated bounced light.

The importer keeps all exterior geometry, converts source axes and units to
metres, and writes indexed glTF geometry plus a cooked `.sgeasset`. The runtime
uses the cook: 405 original DDS mip chains are copied without recompression.
BC1, BC3, and BC5 textures retain their original dimensions and compressed
blocks. Packed Specular maps contain R occlusion, G roughness, B metalness.
BC5 normals retain their texels and use the material's `normalYSign = -1`.
All 131 supplied Specular maps have a uniformly zero red channel. The importer
reports these blank AO channels and sets occlusion strength to zero; the source
BC blocks remain untouched. Enabling these blank maps as AO would suppress all
environment lighting and ray-traced indirect light.

To reproduce the import:

```powershell
./scripts/Import-Bistro.ps1 -Source 'C:\Users\Bas\Downloads\Bistro_v5_2\Bistro_v5_2'
```

This invokes `py -3.10-64`, the existing ufbx dependency and AssetCooker. Pillow
and NumPy inspect alpha coverage and validate samples; no PNG textures are
generated. Run `py -3.10-64 scripts/validate-bistro.py` to recheck the final cook.
RuntimeContent synchronizes generated models, cooked data and the HDR into
`build/Content`. Always rerun the importer after changing source textures: the
runtime's source-staleness shortcut checks file size.

`Content/Models/BistroExterior/import-report.json` records geometry, bounds,
texture dimensions, alpha modes, compressed data checks and sample channels.
The final model has 2,867,938 vertices, 8,496,360 indices and 2,832,120 triangles.
Its texture payload is 970,365,528 bytes. The scene is centred horizontally and
its lowest geometry is 0.25 m above the flat terrain. HDR sky, specular IBL and
diffuse irradiance share `san_giuseppe_bridge_4k.hdr`; existing rotation and
lighting gains are retained. Falcor's supplied scene uses environment intensity
10, so exposure and brightness will differ from its reference.

The `bistroExterior` profile sizes VB geometry storage during a drained level
load, restores default capacities on return, and enables bindless textured
emission. Hardware without the full bindless profile uses Forward with a
diagnostic. The FXC default resolve remains unchanged. VB reconstructs the
normal basis from geometry/UVs; Forward uses exported tangents.

License and attribution from Amazon Lumberyard/ORCA are retained beside the
generated model in LICENSE.txt and README.txt. The supplied HDR attribution is
also included in the original README.

Close running engine instances before rebuilding: Windows locks the executable
and the linker otherwise reports LNK1104.
