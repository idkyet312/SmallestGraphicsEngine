# Content Workflow

Routine static content needs no C++ change.

## Add a model

1. Open **Level Editor**.
2. Open **Assets**.
3. Click **Import Model...** and choose FBX, GLB, or GLTF.
4. Select imported asset.
5. Set target size, default scale, shadows, collision, and optional script.
6. Click **Add Selected** or double-click asset.
7. Position it with gizmo and save level.

Importer copies model and common sidecar textures into `models/Imported/` and
creates editable JSON under `prefabs/Imported/`.

Models placed directly under `models/` appear automatically as generated assets.
Use **Create Editable Prefab** to save their settings.

## Prefab format

```json
{
  "schemaVersion": 1,
  "id": "props/example",
  "name": "Example",
  "components": {
    "staticMesh": {
      "path": "models/example.glb",
      "defaultScale": [1, 1, 1],
      "targetSize": 2.0,
      "castShadow": true
    },
    "collision": { "shape": "box" },
    "script": { "path": "scripts/example_spin.json" }
  }
}
```

Collision values: `none`, `box`, `mesh`. Current `mesh` gameplay collision uses
prefab bounds; renderer still uses original mesh.

## Data script format

```json
{
  "spinDegreesPerSecond": [0, 30, 0],
  "bob": { "amplitude": 0.25, "frequency": 1.0 }
}
```

Prefab and model changes hot-reload every three seconds while editor is open.
