"""Splits Content/Models/AntiAir/source/antiairbig2.glb into the AA emplacement.

The gun traverses and elevates independently, and AssetCooker flattens every
node into one mesh, so the moving parts have to be separate files:

  AntiAir_Base.glb       Support, Cloth, Hinge, pipe   -- never moves
  AntiAir_Traverse.glb   Main, Screws, Rotater          -- yaws
  AntiAir_Elevation.glb  Barrel, Muzzle                 -- yaws and pitches
  AntiAir.glb            everything, for the prefab thumbnail/preview

The stray default "Cube" at the origin is left out of all of them.

Model space. The source is ~4.6x real size, with its ground at y = -2.502 and
the traverse axis at x = 0, z = -1.335; the barrel trunnion (the Barrel node
origin) is 6.08 up that axis. Everything is scaled so the trunnion lands at
VehicleSystem::AATurretMountHeight (1.85 m), which the firing code uses for the
shell origin and aim, and re-rooted:

  base / full           origin on the ground under the traverse axis
  traverse / elevation  origin at the trunnion, where the "Gun" and
                        "Elevation" nodes pivot at runtime

The model faces +Z, the emplacement's zero yaw.

Textures are 27 x 4096^2, ~2.4 GB of VRAM uncooked; they are cut to 2048 on
the large painted surfaces and 1024 elsewhere. The desert-tan paint is tinted
green through baseColorFactor, which both importers and the cook preserve.

Usage: py scripts/split-antiair.py
"""
import sys
from pathlib import Path

from glb_split import build, read_glb, write_glb

ROOT = Path(__file__).resolve().parent.parent
FOLDER = ROOT / "Content/Models/AntiAir"
SOURCE = FOLDER / "source/antiairbig2.glb"

MOUNT_HEIGHT = 1.85          # VehicleSystem::AATurretMountHeight
GROUND_Y = -2.502            # lowest point of "Support"
AXIS_Z = -1.335              # traverse axis, shared by Main/Rotater/Barrel
TRUNNION_Y = 6.08            # "Barrel" node origin
SCALE = MOUNT_HEIGHT / (TRUNNION_Y - GROUND_Y)

# Multiplies the tan paint (mean ~0.76, 0.68, 0.37) to ~(0.27, 0.42, 0.16),
# a clear military green; the grey metal parts come out dark green-grey.
GREEN_TINT = [0.36, 0.62, 0.42, 1.0]

PARTS = {
    "AntiAir_Base.glb": ["Support", "Cloth", "Hinge", "pipe"],
    "AntiAir_Traverse.glb": ["Main", "Screws", "Rotater"],
    "AntiAir_Elevation.glb": ["Barrel", "Muzzle"],
}
LARGE_TEXTURES = ("_Main_", "_Support_", "_Barrel_", "_Rotater_")


def image_size(image):
    name = image.get("name", "")
    return 2048 if any(part in name for part in LARGE_TEXTURES) else 1024


def tint(material):
    pbr = material.setdefault("pbrMetallicRoughness", {})
    pbr["baseColorFactor"] = GREEN_TINT


def root_translation(pivot_y):
    return [0.0, -pivot_y * SCALE, -AXIS_Z * SCALE]


def write_part(doc, binary, names, pivot_y, out_path, cache):
    by_name = {node.get("name"): index
               for index, node in enumerate(doc["nodes"])}
    missing = [name for name in names if name not in by_name]
    if missing:
        raise SystemExit(f"source is missing nodes: {missing}")
    for name in names:
        if doc["nodes"][by_name[name]].get("children"):
            raise SystemExit(f"{name}: nested children are not handled")
    root = {"name": out_path.stem, "translation": root_translation(pivot_y),
            "scale": [SCALE, SCALE, SCALE],
            "children": list(range(1, len(names) + 1))}
    nodes = [root] + [doc["nodes"][by_name[name]] for name in names]
    part_doc, part_bin = build(doc, binary, nodes, [0], image_size=image_size,
                               material_edit=tint, image_cache=cache)
    write_glb(out_path, part_doc, part_bin)
    print(f"{out_path.name:24} {out_path.stat().st_size / 1e6:6.1f} MB  "
          f"{', '.join(names)}")


def main():
    doc, binary = read_glb(SOURCE)
    cache = {}
    everything = []
    for file_name, names in PARTS.items():
        pivot = GROUND_Y if file_name == "AntiAir_Base.glb" else TRUNNION_Y
        write_part(doc, binary, names, pivot, FOLDER / file_name, cache)
        everything += names
    write_part(doc, binary, everything, GROUND_Y, FOLDER / "AntiAir.glb",
               cache)
    barrel_tip_z = 15.37     # far end of "Muzzle"
    print(f"scale {SCALE:.5f}; barrel length trunnion->muzzle "
          f"{(barrel_tip_z - AXIS_Z) * SCALE:.3f} m")
    return 0


if __name__ == "__main__":
    sys.exit(main())
