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
import sys
from pathlib import Path

from glb_split import build, read_glb, write_glb

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
