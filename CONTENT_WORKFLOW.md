# Content Workflow

Routine static content needs no C++ change.

## Add a model

1. Open **Level Editor**.
2. Open **Assets**.
3. Click **Import Model...** and choose FBX, GLB, or GLTF.
4. Select imported asset.
5. Set target size, default scale, material use, shadows, and collision.
6. Click **Add Selected** or double-click asset.
7. Position it with gizmo and save level.

Importer copies model and common sidecar textures into `models/Imported/` and
creates editable JSON under `prefabs/Imported/`.

Models placed directly under `models/` appear automatically as generated assets.
Use **Create Editable Prefab** to save their settings.

## Prefab format

```json
{
  "schemaVersion": 2,
  "id": "props/example",
  "name": "Example",
  "components": {
    "staticMesh": {
      "path": "models/example.glb",
      "defaultScale": [1, 1, 1],
      "targetSize": 2.0,
      "castShadow": true,
      "useMaterials": true
    },
    "collision": { "shape": "box" }
  }
}
```

Collision values: `none`, `box`, `mesh`. Current `mesh` gameplay collision uses
prefab bounds; renderer still uses original mesh.

Prefab Settings can add lights, looping/one-shot audio, destructible health,
enemy spawners, material overrides, LOD models, child prefabs, and an `extends`
base prefab. These settings save directly to schema v2 JSON. Per-instance
overrides appear in Inspector after placing prefab; enable checkbox beside any
property before editing it.

Assets use persistent GUIDs stored in `assetcache/registry.json`. Prefabs retain
GUID plus readable fallback path, so model renames survive registry refresh.
Missing dependencies appear as `[MISSING DEP]` rows with tooltip details.
Model dependencies come from material texture references, not folder guesses;
the registry records the complete model -> texture -> prefab -> level chain.

Use **Undo Asset** and **Redo Asset** for prefab edits and imports. Ordinary
level edits use toolbar **Undo** and **Redo**.

Scripting remains reserved for a future language/runtime decision. Prefab and
asset changes hot-reload through the native filesystem watcher while editor is
open. Model copying, validation, dependency refresh, and thumbnail mesh import run
in background jobs. Thumbnail geometry is rendered by DX12 into a 128 x 128
offscreen target, displayed directly, then asynchronously cached as PNG under
`assetcache/thumbs/`.
