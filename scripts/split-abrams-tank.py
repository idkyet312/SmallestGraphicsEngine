"""Splits Content/Models/AbramsTank/AbramsLow.glb into a hull and a turret.

The source is one hierarchy, which the engine cannot use as-is for a tank:

  * The turret ("Abrams_Tower") has to traverse on its own, and AssetCooker
    flattens every node into one mesh (see IsCookExcluded), so a turret found
    by node name at runtime would vanish the moment the asset is cooked. Two
    files keep the split cook-proof.
  * "Abrams_CaterpillarAnimL/R" are Blender curve-modifier sources exported
    undeformed: flat 15 m strips at track-top height sticking ~10 m out of the
    front of the hull. The deformed tracks are "Abrams_CaterpillarL/R". The
    curve empties that drove them go too.

Output model space matches the source for the hull. The turret is re-rooted so
its traverse pivot (the Tower node origin) is the model origin; the pivot's
position in hull space is printed and is what the tank prefab's child offset
uses.

Buffers are compacted to what each file references, so the turret does not
carry the hull's track textures.

Usage: py scripts/split-abrams-tank.py
"""
import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "Content/Models/AbramsTank/AbramsLow.glb"
HULL_OUT = ROOT / "Content/Models/AbramsTank/AbramsHull.glb"
TURRET_OUT = ROOT / "Content/Models/AbramsTank/AbramsTurret.glb"

DROP_FROM_HULL = {
    "Abrams_Tower",
    "Abrams_CaterpillarAnimL",
    "Abrams_CaterpillarAnimR",
    "Abrams_CaterpillarCurveL",
    "Abrams_CaterpillarCurveR",
}


def read_glb(path):
    data = path.read_bytes()
    magic, version, _ = struct.unpack_from("<III", data, 0)
    if magic != 0x46546C67 or version != 2:
        raise SystemExit(f"{path}: not a glTF 2.0 binary")
    offset = 12
    doc = None
    binary = b""
    while offset < len(data):
        length, kind = struct.unpack_from("<II", data, offset)
        chunk = data[offset + 8: offset + 8 + length]
        if kind == 0x4E4F534A:
            doc = json.loads(chunk)
        elif kind == 0x004E4942:
            binary = chunk
        offset += 8 + length
    return doc, binary


def write_glb(path, doc, binary):
    text = json.dumps(doc, separators=(",", ":")).encode("utf-8")
    text += b" " * ((4 - len(text) % 4) % 4)
    binary += b"\0" * ((4 - len(binary) % 4) % 4)
    total = 12 + 8 + len(text) + 8 + len(binary)
    with open(path, "wb") as out:
        out.write(struct.pack("<III", 0x46546C67, 2, total))
        out.write(struct.pack("<II", len(text), 0x4E4F534A))
        out.write(text)
        out.write(struct.pack("<II", len(binary), 0x004E4942))
        out.write(binary)


def build(doc, binary, nodes, root_nodes):
    """Returns a compacted document holding `nodes` (already remapped: each
    node's "children" indexes into `nodes`) and only the data they reach."""
    mesh_map, material_map, texture_map, image_map = {}, {}, {}, {}
    sampler_map, accessor_map, view_map = {}, {}, {}
    out = {"asset": doc["asset"], "scene": 0,
           "scenes": [{"name": "Scene", "nodes": root_nodes}],
           "nodes": [], "meshes": [], "materials": [], "textures": [],
           "images": [], "samplers": [], "accessors": [], "bufferViews": [],
           "buffers": []}
    blob = bytearray()

    def view(index):
        if index not in view_map:
            source = dict(doc["bufferViews"][index])
            start = source.get("byteOffset", 0)
            chunk = binary[start:start + source["byteLength"]]
            blob.extend(b"\0" * ((4 - len(blob) % 4) % 4))
            source["buffer"] = 0
            source["byteOffset"] = len(blob)
            blob.extend(chunk)
            view_map[index] = len(out["bufferViews"])
            out["bufferViews"].append(source)
        return view_map[index]

    def accessor(index):
        if index not in accessor_map:
            source = dict(doc["accessors"][index])
            if "sparse" in source:
                raise SystemExit("sparse accessors are not handled")
            if "bufferView" in source:
                source["bufferView"] = view(source["bufferView"])
            accessor_map[index] = len(out["accessors"])
            out["accessors"].append(source)
        return accessor_map[index]

    def sampler(index):
        if index not in sampler_map:
            sampler_map[index] = len(out["samplers"])
            out["samplers"].append(doc["samplers"][index])
        return sampler_map[index]

    def image(index):
        if index not in image_map:
            source = dict(doc["images"][index])
            if "bufferView" in source:
                source["bufferView"] = view(source["bufferView"])
            image_map[index] = len(out["images"])
            out["images"].append(source)
        return image_map[index]

    def texture(index):
        if index not in texture_map:
            source = dict(doc["textures"][index])
            if "source" in source:
                source["source"] = image(source["source"])
            if "sampler" in source:
                source["sampler"] = sampler(source["sampler"])
            texture_map[index] = len(out["textures"])
            out["textures"].append(source)
        return texture_map[index]

    def remap_texture_refs(value):
        if isinstance(value, dict):
            result = {}
            for key, item in value.items():
                if key.endswith("Texture") and isinstance(item, dict) \
                        and "index" in item:
                    item = dict(item)
                    item["index"] = texture(item["index"])
                    result[key] = remap_texture_refs(item)
                else:
                    result[key] = remap_texture_refs(item)
            return result
        if isinstance(value, list):
            return [remap_texture_refs(item) for item in value]
        return value

    def material(index):
        if index not in material_map:
            material_map[index] = len(out["materials"])
            out["materials"].append(None)
            out["materials"][material_map[index]] = remap_texture_refs(
                doc["materials"][index])
        return material_map[index]

    def mesh(index):
        if index not in mesh_map:
            source = json.loads(json.dumps(doc["meshes"][index]))
            for primitive in source["primitives"]:
                primitive["attributes"] = {
                    key: accessor(value)
                    for key, value in primitive["attributes"].items()}
                if "indices" in primitive:
                    primitive["indices"] = accessor(primitive["indices"])
                if "material" in primitive:
                    primitive["material"] = material(primitive["material"])
                if "targets" in primitive:
                    raise SystemExit("morph targets are not handled")
            mesh_map[index] = len(out["meshes"])
            out["meshes"].append(source)
        return mesh_map[index]

    for node in nodes:
        node = dict(node)
        if "mesh" in node:
            node["mesh"] = mesh(node["mesh"])
        out["nodes"].append(node)

    for key in ("textures", "images", "samplers", "materials"):
        if not out[key]:
            del out[key]
    for extension in ("extensionsUsed", "extensionsRequired"):
        if extension in doc:
            out[extension] = doc[extension]
    out["buffers"] = [{"byteLength": len(blob)}]
    return out, bytes(blob)


def main():
    doc, binary = read_glb(SOURCE)
    nodes = doc["nodes"]
    by_name = {node.get("name"): index for index, node in enumerate(nodes)}
    hull_index = by_name.get("Abrams_Hull")
    tower_index = by_name.get("Abrams_Tower")
    if hull_index is None or tower_index is None:
        raise SystemExit("expected Abrams_Hull and Abrams_Tower nodes")
    hull = nodes[hull_index]
    tower = nodes[tower_index]
    for node in (hull, tower):
        if "rotation" in node or "scale" in node or "matrix" in node:
            raise SystemExit(f"{node['name']}: only translation is handled")

    # Hull: root first, then its kept children, remapped to the new order.
    kept = [child for child in hull.get("children", [])
            if nodes[child].get("name") not in DROP_FROM_HULL]
    for child in kept:
        if nodes[child].get("children"):
            raise SystemExit(f"{nodes[child]['name']}: nested children")
    hull_nodes = [dict(hull)]
    hull_nodes[0]["children"] = list(range(1, len(kept) + 1))
    hull_nodes += [nodes[child] for child in kept]
    hull_doc, hull_bin = build(doc, binary, hull_nodes, [0])
    write_glb(HULL_OUT, hull_doc, hull_bin)

    # Turret: the tower mesh alone, its own origin at the model origin.
    turret_node = {"name": "Abrams_Tower", "mesh": tower["mesh"]}
    turret_doc, turret_bin = build(doc, binary, [turret_node], [0])
    write_glb(TURRET_OUT, turret_doc, turret_bin)

    ht = hull.get("translation", [0, 0, 0])
    tt = tower.get("translation", [0, 0, 0])
    pivot = [ht[i] + tt[i] for i in range(3)]
    dropped = sorted(nodes[c].get("name") for c in hull.get("children", [])
                     if c not in kept)
    print(f"hull:   {HULL_OUT.name} {HULL_OUT.stat().st_size / 1e6:.1f} MB, "
          f"{len(kept)} child nodes, dropped {dropped}")
    print(f"turret: {TURRET_OUT.name} {TURRET_OUT.stat().st_size / 1e6:.1f} MB")
    print("turret pivot in hull space: "
          f"[{pivot[0]:.4f}, {pivot[1]:.4f}, {pivot[2]:.4f}]")
    return 0


if __name__ == "__main__":
    sys.exit(main())
