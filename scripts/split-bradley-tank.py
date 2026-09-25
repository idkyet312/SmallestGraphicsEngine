"""Splits Content/Models/BradleyTank/Bredley_Low.glb into a hull and a turret.

Same reasons as split-abrams-tank.py (the turret traverses on its own and the
cook flattens node names), plus what this source does differently:

  * The hull root carries the Blender export's axis rotation and a 66x scale,
    and the model sits ~0.6 m off centre across the hull with its tracks 0.27 m
    below the origin. The hull root's translation is shifted so the kept
    geometry is centred on X/Z with its lowest point at Y = 0, like the Abrams.
  * "Bredley_CaterpillarAnimL" and its unnamed twin "Cube.004" are the
    undeformed curve-modifier track strips (12 m, sticking ~7 m off the front
    of the hull); "Bredley_CaterpillarCurveL" and "Cube.003" are the curve
    empties that drove them. The deformed tracks are "Bredley_CaterpillarL/R".
  * The Tower node's origin is the hull origin, not the turret ring, so the
    traverse pivot is measured instead: the centre of the circle of vertices
    at the bottom of the tower mesh (the turret ring).

The turret keeps its gun and TOW launcher children and is re-rooted so the
pivot is the model origin. The pivot's position in hull space, the gun tip
relative to it (the prefab's muzzle) and the turret roof height are printed.

Usage: py scripts/split-bradley-tank.py
"""
import math
import struct
import sys
from pathlib import Path

from glb_split import build, read_glb, write_glb

ROOT = Path(__file__).resolve().parent.parent
SOURCE = ROOT / "Content/Models/BradleyTank/Bredley_Low.glb"
HULL_OUT = ROOT / "Content/Models/BradleyTank/BradleyHull.glb"
TURRET_OUT = ROOT / "Content/Models/BradleyTank/BradleyTurret.glb"

DROP_FROM_HULL = {
    "Bredley_Tower",
    "Bredley_CaterpillarAnimL",
    "Bredley_CaterpillarCurveL",
    "Cube.003",
    "Cube.004",
}
# Turret-ring vertices lie within this of the tower mesh's lowest point, and
# must form a circle to this tolerance, or the pivot is not trusted.
RING_SLICE = 0.002
RING_TOLERANCE = 0.01
# Antennas and the commander's sight stand clear of the turret roof: the roof is
# where the tower's vertex heights, walked up from the ring, first jump by more
# than this.
ROOF_GAP = 0.2


def local_matrix(node):
    if "matrix" in node:
        m = node["matrix"]
        return [[m[column * 4 + row] for column in range(4)]
                for row in range(4)]
    tx, ty, tz = node.get("translation", [0.0, 0.0, 0.0])
    x, y, z, w = node.get("rotation", [0.0, 0.0, 0.0, 1.0])
    sx, sy, sz = node.get("scale", [1.0, 1.0, 1.0])
    rotation = [
        [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
        [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
        [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)],
    ]
    scale = (sx, sy, sz)
    return [[rotation[r][c] * scale[c] for c in range(3)] + [t]
            for r, t in zip(range(3), (tx, ty, tz))] + [[0.0, 0.0, 0.0, 1.0]]


def multiply(a, b):
    return [[sum(a[r][k] * b[k][c] for k in range(4)) for c in range(4)]
            for r in range(4)]


def transform(m, p):
    return [m[r][0] * p[0] + m[r][1] * p[1] + m[r][2] * p[2] + m[r][3]
            for r in range(3)]


def positions(doc, binary, mesh_index):
    for primitive in doc["meshes"][mesh_index]["primitives"]:
        accessor = doc["accessors"][primitive["attributes"]["POSITION"]]
        view = doc["bufferViews"][accessor["bufferView"]]
        start = view.get("byteOffset", 0) + accessor.get("byteOffset", 0)
        stride = view.get("byteStride", 12)
        for i in range(accessor["count"]):
            yield struct.unpack_from("<3f", binary, start + i * stride)


def main():
    doc, binary = read_glb(SOURCE)
    nodes = doc["nodes"]
    by_name = {node.get("name"): index for index, node in enumerate(nodes)}
    hull_index = by_name.get("Bredley_Hull")
    tower_index = by_name.get("Bredley_Tower")
    if hull_index is None or tower_index is None:
        raise SystemExit("expected Bredley_Hull and Bredley_Tower nodes")
    if doc["scenes"][doc.get("scene", 0)]["nodes"] != [hull_index]:
        raise SystemExit("expected Bredley_Hull to be the only scene root")
    hull = nodes[hull_index]
    tower = nodes[tower_index]
    if tower_index not in hull.get("children", []):
        raise SystemExit("expected Bredley_Tower under Bredley_Hull")
    hull_matrix = local_matrix(hull)

    def world_points(index, parent_matrix):
        node = nodes[index]
        matrix = multiply(parent_matrix, local_matrix(node))
        if "mesh" in node:
            for p in positions(doc, binary, node["mesh"]):
                yield transform(matrix, p)
        for child in node.get("children", []):
            yield from world_points(child, matrix)

    # Hull: the root, then its kept children, remapped to the new order.
    kept = [child for child in hull.get("children", [])
            if nodes[child].get("name") not in DROP_FROM_HULL]
    for child in kept:
        if nodes[child].get("children"):
            raise SystemExit(f"{nodes[child]['name']}: nested children")
    hull_points = [transform(hull_matrix, p)
                   for p in positions(doc, binary, hull["mesh"])]
    for child in kept:
        hull_points += world_points(child, hull_matrix)
    low = [min(p[axis] for p in hull_points) for axis in range(3)]
    high = [max(p[axis] for p in hull_points) for axis in range(3)]
    offset = [-(low[0] + high[0]) * 0.5, -low[1], -(low[2] + high[2]) * 0.5]

    hull_root = dict(hull)
    root_translation = hull.get("translation", [0.0, 0.0, 0.0])
    hull_root["translation"] = [root_translation[i] + offset[i]
                                for i in range(3)]
    hull_root["children"] = list(range(1, len(kept) + 1))
    hull_nodes = [hull_root] + [nodes[child] for child in kept]
    hull_doc, hull_bin = build(doc, binary, hull_nodes, [0])
    write_glb(HULL_OUT, hull_doc, hull_bin)

    # Pivot: the turret ring at the bottom of the tower mesh, in the shifted
    # hull space.
    tower_matrix = multiply(hull_matrix, local_matrix(tower))
    tower_points = [[p[i] + offset[i] for i in range(3)] for p in
                    (transform(tower_matrix, q)
                     for q in positions(doc, binary, tower["mesh"]))]
    floor = min(p[1] for p in tower_points)
    ring = [p for p in tower_points if p[1] <= floor + RING_SLICE]
    centre_x = (min(p[0] for p in ring) + max(p[0] for p in ring)) * 0.5
    centre_z = (min(p[2] for p in ring) + max(p[2] for p in ring)) * 0.5
    radii = [math.hypot(p[0] - centre_x, p[2] - centre_z) for p in ring]
    if len(ring) < 8 or max(radii) - min(radii) > RING_TOLERANCE:
        raise SystemExit(f"tower bottom is not a ring: {len(ring)} vertices, "
                         f"radius {min(radii):.3f}..{max(radii):.3f}")
    pivot = [centre_x, floor, centre_z]

    # Turret: a root carrying the hull's rotation and scale, translated so the
    # pivot lands on the model origin, over the tower subtree.
    subtree = []

    def collect(index):
        position = len(subtree)
        subtree.append(dict(nodes[index]))
        children = [collect(child) for child in nodes[index].get("children", [])]
        if children:
            subtree[position]["children"] = children
        return position

    turret_root = {"name": "Bradley_TurretRoot",
                   "translation": [root_translation[i] + offset[i] - pivot[i]
                                   for i in range(3)],
                   "children": [1]}
    for key in ("rotation", "scale"):
        if key in hull:
            turret_root[key] = hull[key]
    if "matrix" in hull:
        raise SystemExit("Bredley_Hull: matrix transforms are not handled")
    subtree.append(turret_root)
    collect(tower_index)
    turret_doc, turret_bin = build(doc, binary, subtree, [0])
    write_glb(TURRET_OUT, turret_doc, turret_bin)

    gun = nodes[by_name["Bredley_MainGun"]]
    gun_matrix = multiply(tower_matrix, local_matrix(gun))
    tip = max((transform(gun_matrix, p)
               for p in positions(doc, binary, gun["mesh"])),
              key=lambda p: p[0])
    muzzle = [tip[i] + offset[i] - pivot[i] for i in range(3)]
    heights = sorted(p[1] for p in tower_points)
    roof = next((low for low, high in zip(heights, heights[1:])
                 if high - low > ROOF_GAP), heights[-1])
    top = max(p[1] for p in world_points(tower_index, hull_matrix)) \
        + offset[1]

    dropped = sorted(nodes[c].get("name") for c in hull.get("children", [])
                     if c not in kept)
    print(f"hull:   {HULL_OUT.name} {HULL_OUT.stat().st_size / 1e6:.1f} MB, "
          f"{len(kept)} child nodes, dropped {dropped}")
    print(f"        bounds {high[0] - low[0]:.3f} x {high[1] - low[1]:.3f} x "
          f"{high[2] - low[2]:.3f} m, shifted by "
          f"[{offset[0]:.4f}, {offset[1]:.4f}, {offset[2]:.4f}]")
    print(f"turret: {TURRET_OUT.name} {TURRET_OUT.stat().st_size / 1e6:.1f} MB, "
          f"ring radius {sum(radii) / len(radii):.3f} m")
    print("turret pivot in hull space: "
          f"[{pivot[0]:.4f}, {pivot[1]:.4f}, {pivot[2]:.4f}]")
    print(f"muzzle (turret space): "
          f"[{muzzle[0]:.4f}, {muzzle[1]:.4f}, {muzzle[2]:.4f}]")
    print(f"turret roof {roof:.3f} m, highest point {top:.3f} m (hull space)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
