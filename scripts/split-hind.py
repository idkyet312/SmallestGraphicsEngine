"""Splits Content/Models/HeliHind2/Mi-24V Hind.glb into the enemy gunship.

The main and tail rotors spin on their own nodes, and AssetCooker flattens
every node into one mesh, so they have to be separate files:

  Hind.glb             airframe, everything that does not spin
  Hind_MainRotor.glb   five-blade main rotor, origin at its hub
  Hind_TailRotor.glb   three-blade tail rotor, origin at its hub

The source carries two main rotors, "rotor_blades _flying" (straight blades)
and "rotor_blades _landed" (drooped, parked). Only the flying one is kept.

Model space. Both rotor meshes are authored with their hub at the mesh origin
(measured: the minimum enclosing circle of each disc is centred there to
within 2e-4). That origin is the pivot, so each rotor is written with its node
translation removed and the translation becomes the hub offset printed below,
which ConfigureHelicopterModel hangs the rotor at. The root's 5.5 scale is
kept in all three files so the parts stay in one shared space; the gunship's
size is normalised at runtime by ConfigureHelicopterBounds anyway.

The root's authored 4.4 degree pitch is dropped. The main rotor's mast is the
fuselage's own +Y, so with the pitch left in, a disc spun about the node's Y
would wobble. The source nose points +Z; the gunship code expects the OH-1's
-Z (HelicopterWorldMatrix adds PI to the yaw), so the root is turned 180
degrees about Y instead.

Textures are 24 x 4096^2 (the normals 16-bit), ~2 GB of VRAM uncooked. They
are cut to 2048 on the hull and tail and 1024 elsewhere, then cooked to BC.

The glass, lamp and mirror materials carry no textures or factors, which glTF
reads as white fully-metallic rough -- grey plastic. They get explicit factors.

Usage: py scripts/split-hind.py
"""
import math
from pathlib import Path

from glb_split import build, read_glb, write_glb

ROOT = Path(__file__).resolve().parent.parent
FOLDER = ROOT / "Content/Models/HeliHind2"
SOURCE = FOLDER / "Mi-24V Hind.glb"

MAIN_ROTOR = "rotor_blades _flying"
PARKED_ROTOR = "rotor_blades _landed"
TAIL_ROTOR = "tail_rotor_blades"

# 180 degrees about +Y, as an (x, y, z, w) quaternion.
YAW_180 = [0.0, 1.0, 0.0, 0.0]

LARGE_TEXTURE_MATERIALS = ("hull", "tail")

MATERIAL_FACTORS = {
    "cockpit glass": {"baseColorFactor": [0.025, 0.032, 0.036, 1.0],
                      "metallicFactor": 0.0, "roughnessFactor": 0.06},
    "hull glass": {"baseColorFactor": [0.025, 0.032, 0.036, 1.0],
                   "metallicFactor": 0.0, "roughnessFactor": 0.06},
    "light_mirror": {"baseColorFactor": [0.85, 0.85, 0.85, 1.0],
                     "metallicFactor": 1.0, "roughnessFactor": 0.12},
    "red light": {"baseColorFactor": [0.75, 0.03, 0.02, 1.0],
                  "metallicFactor": 0.0, "roughnessFactor": 0.35},
    "green light": {"baseColorFactor": [0.03, 0.65, 0.08, 1.0],
                    "metallicFactor": 0.0, "roughnessFactor": 0.35},
}


def name_images_by_material(doc):
    """The source names its images by Substance set, so three sets share
    "Material.001_*" and three "None_*". Rename each after the material that
    uses it, which is what the texture size is chosen by."""
    for material in doc["materials"]:
        pbr = material.get("pbrMetallicRoughness", {})
        slots = {
            "base": pbr.get("baseColorTexture"),
            "mr": pbr.get("metallicRoughnessTexture"),
            "normal": material.get("normalTexture"),
        }
        for slot, reference in slots.items():
            if reference is None:
                continue
            image = doc["textures"][reference["index"]]["source"]
            doc["images"][image]["name"] = f"{material['name']}_{slot}"


def image_size(image):
    material = image.get("name", "").rsplit("_", 1)[0]
    return 2048 if material in LARGE_TEXTURE_MATERIALS else 1024


def edit_material(material):
    factors = MATERIAL_FACTORS.get(material.get("name"))
    if factors:
        material.setdefault("pbrMetallicRoughness", {}).update(factors)


def subtree(doc, index, out):
    """Appends node `index` and its descendants to `out` with "children"
    re-indexed into `out`. Returns the node's position in `out`."""
    node = dict(doc["nodes"][index])
    position = len(out)
    out.append(node)
    children = [subtree(doc, child, out) for child in node.get("children", [])]
    if children:
        node["children"] = children
    return position


def rotate_scale(vector, scale):
    """YAW_180 applied to scale * vector."""
    x, y, z = (component * scale for component in vector)
    return [-x, y, -z]


def main():
    doc, binary = read_glb(SOURCE)
    name_images_by_material(doc)
    by_name = {node.get("name"): index for index, node in enumerate(doc["nodes"])}
    root_index = doc["scenes"][doc.get("scene", 0)]["nodes"][0]
    root = doc["nodes"][root_index]
    scale = root["scale"][0]
    if any(not math.isclose(s, scale, rel_tol=1e-4) for s in root["scale"]):
        raise SystemExit(f"non-uniform root scale {root['scale']}")

    cache = {}
    spinning = (by_name[MAIN_ROTOR], by_name[TAIL_ROTOR], by_name[PARKED_ROTOR])

    # Airframe: the root (which carries the fuselage mesh) and every child
    # that does not spin.
    nodes = [{"name": "Hind", "mesh": root["mesh"], "rotation": YAW_180,
              "scale": root["scale"]}]
    nodes[0]["children"] = [subtree(doc, child, nodes)
                            for child in root["children"]
                            if child not in spinning]
    out_doc, out_bin = build(doc, binary, nodes, [0], image_size,
                             edit_material, cache)
    write_glb(FOLDER / "Hind.glb", out_doc, out_bin)

    for file_name, node_name in (("Hind_MainRotor.glb", MAIN_ROTOR),
                                 ("Hind_TailRotor.glb", TAIL_ROTOR)):
        source = doc["nodes"][by_name[node_name]]
        if source.get("rotation") or source.get("scale") or \
                source.get("children"):
            raise SystemExit(f"{node_name}: expected translation only")
        hub = rotate_scale(source.get("translation", [0, 0, 0]), scale)
        nodes = [{"name": node_name, "mesh": source["mesh"],
                  "rotation": YAW_180, "scale": root["scale"]}]
        out_doc, out_bin = build(doc, binary, nodes, [0], image_size,
                                 edit_material, cache)
        write_glb(FOLDER / file_name, out_doc, out_bin)
        print(f"{file_name}: hub at "
              f"{{ {hub[0]:.4f}f, {hub[1]:.4f}f, {hub[2]:.4f}f }}")

    for file_name in ("Hind.glb", "Hind_MainRotor.glb", "Hind_TailRotor.glb"):
        size = (FOLDER / file_name).stat().st_size
        print(f"{file_name}: {size / 1e6:.1f} MB")


if __name__ == "__main__":
    main()
