"""Repair GLB normals without moving vertices or changing its scene hierarchy.

Requires numpy. Write to a separate output first, audit it, then replace the
source and re-cook it. Exact-position adjacency avoids closing authored gaps.
"""

import argparse
import copy
import json
import math
import struct
from collections import defaultdict, deque
from pathlib import Path

import numpy as np


class UnionFind:
    def __init__(self, count):
        self.parent = list(range(count))

    def find(self, index):
        while self.parent[index] != index:
            self.parent[index] = self.parent[self.parent[index]]
            index = self.parent[index]
        return index

    def join(self, a, b):
        a, b = self.find(a), self.find(b)
        if a != b:
            self.parent[b] = a


def repair(source, destination, crease_degrees=35):
    if source.resolve() == destination.resolve():
        raise ValueError('Write a separate candidate, verify it, then replace the source.')
    raw = source.read_bytes()
    magic, version, total = struct.unpack_from('<III', raw)
    assert magic == 0x46546C67 and version == 2 and total == len(raw)
    json_size, chunk_type = struct.unpack_from('<II', raw, 12)
    assert chunk_type == 0x4E4F534A
    original = json.loads(raw[20:20 + json_size])
    size, chunk_type = struct.unpack_from('<II', raw, 20 + json_size)
    assert chunk_type == 0x004E4942
    binary = raw[28 + json_size:28 + json_size + size]
    assert len(original['buffers']) == 1
    doc = copy.deepcopy(original)
    types = {5121: '<u1', 5123: '<u2', 5125: '<u4', 5126: '<f4'}
    widths = {'SCALAR': 1, 'VEC2': 2, 'VEC3': 3, 'VEC4': 4, 'MAT4': 16}
    replacements = {}
    touched = set()
    stats = []

    def read(index):
        a = original['accessors'][index]
        # Keep encoded normalized color/weight values unchanged when splitting
        # vertices; their normalized accessor flag remains in the document.
        assert 'sparse' not in a
        view = original['bufferViews'][a['bufferView']]
        assert view['buffer'] == 0
        dtype = types[a['componentType']]
        item = np.dtype(dtype).itemsize
        width = widths[a['type']]
        offset = view.get('byteOffset', 0) + a.get('byteOffset', 0)
        return np.ndarray((a['count'], width), dtype=dtype, buffer=binary,
                          offset=offset, strides=(view.get('byteStride', item * width), item)).copy()

    def replace(index, array, component_type=None):
        # This asset does not share geometry accessors between primitives.
        assert index not in touched
        touched.add(index)
        a = doc['accessors'][index]
        if component_type is not None:
            a['componentType'] = component_type
        array = np.asarray(array, dtype=types[a['componentType']])
        replacements[index] = array.tobytes()
        a['count'] = len(array)
        a.pop('byteOffset', None)
        for key, function in [('min', np.min), ('max', np.max)]:
            if key in a:
                a[key] = function(array, axis=0).tolist()

    def face_geometry(p, faces):
        tri = p[faces]
        cross = np.cross(tri[:, 1] - tri[:, 0], tri[:, 2] - tri[:, 0])
        area2 = np.linalg.norm(cross, axis=1)
        return cross, area2, cross / np.maximum(area2[:, None], 1e-30)

    def edge_map(welded_faces):
        edges = defaultdict(list)
        for fi, face in enumerate(welded_faces):
            for corner in range(3):
                u, v = int(face[corner]), int(face[(corner + 1) % 3])
                if u != v:
                    edges[tuple(sorted((u, v)))].append((fi, u < v))
        return edges

    for mi, mesh in enumerate(doc['meshes']):
        for pi, primitive in enumerate(mesh['primitives']):
            assert primitive.get('mode', 4) == 4 and not primitive.get('targets')
            attributes = primitive['attributes']
            assert {'POSITION', 'NORMAL', 'TEXCOORD_0'} <= set(attributes)
            assert 'TANGENT' not in attributes, 'Authored tangents need regeneration after normal repair'
            extra_attributes = {name: read(index) for name, index in attributes.items()
                                if name not in {'POSITION', 'NORMAL', 'TEXCOORD_0'}}
            positions = read(attributes['POSITION'])
            p = positions.astype(float)
            old_normals = read(attributes['NORMAL']).astype(float)
            uv = read(attributes['TEXCOORD_0'])
            old_faces = read(primitive['indices']).reshape(-1, 3).astype(int)
            assert np.isfinite(p).all() and np.isfinite(old_normals).all()
            assert old_faces.min() >= 0 and old_faces.max() < len(p)
            _, area2, _ = face_geometry(p, old_faces)
            tri = p[old_faces]
            edge_product = (np.linalg.norm(tri[:, 1] - tri[:, 0], axis=1)
                            * np.linalg.norm(tri[:, 2] - tri[:, 0], axis=1))
            # Almost-collinear faces can reverse during float32 transform baking
            # even though their float64 cross product is nonzero.
            stable = (area2 > 1e-12) & (area2 > edge_product * 1e-6)
            removed_near_collinear = int(np.sum((area2 > 1e-12) & ~stable))
            faces = old_faces[stable].copy()
            removed_degenerate = len(old_faces) - len(faces)
            _, welded = np.unique(positions, axis=0, return_inverse=True)
            # All materials are double-sided. A coincident triangle, even with
            # opposite winding, adds no surface coverage and can fight depth.
            assert doc['materials'][primitive['material']].get('doubleSided', False)
            _, first = np.unique(np.sort(welded[faces], axis=1), axis=0, return_index=True)
            removed_duplicate = len(faces) - len(first)
            faces = faces[np.sort(first)]
            edges = edge_map(welded[faces])

            # Propagate orientation only across unambiguous two-face edges.
            # Intersecting/touching shells remain separate at nonmanifold edges.
            adjacent = defaultdict(list)
            for edge_faces in edges.values():
                if len(edge_faces) == 2:
                    (a, ad), (b, bd) = edge_faces
                    adjacent[a].append((b, ad == bd))
                    adjacent[b].append((a, ad == bd))
            _, area2, fn = face_geometry(p, faces)
            authored = old_normals[faces].mean(axis=1)
            authored_alignment = np.einsum('ij,ij->i', authored, fn) * area2
            parity = np.full(len(faces), -1, dtype=np.int8)
            conflicts = []
            flipped = np.zeros(len(faces), dtype=bool)
            for seed in range(len(faces)):
                if parity[seed] != -1:
                    continue
                parity[seed] = 0
                queue = deque([seed])
                component = []
                conflict = False
                while queue:
                    a = queue.popleft()
                    component.append(a)
                    for b, toggle in adjacent[a]:
                        desired = int(parity[a]) ^ int(toggle)
                        if parity[b] == -1:
                            parity[b] = desired
                            queue.append(b)
                        elif parity[b] != desired:
                            conflict = True
                if conflict:
                    # A nonorientable component needs an authored topology fix;
                    # do not arbitrarily reverse a subset of its triangles.
                    conflicts.append(len(component))
                    continue
                component = np.array(component)
                sign = 1 - 2 * parity[component]
                agreement = float(np.sum(authored_alignment[component] * sign))
                inverted = agreement < 0
                flipped[component] = parity[component].astype(bool) ^ inverted
            assert not conflicts, 'Ambiguous topology requires a manual repair'
            faces[flipped] = faces[flipped][:, [0, 2, 1]]
            cross, area2, fn = face_geometry(p, faces)
            edges = edge_map(welded[faces])

            # Smoothing fans are connected at a vertex only through an edge
            # below the crease angle. UV seams can share normals without sharing
            # their vertex/UV records; disconnected touching parts cannot.
            groups = UnionFind(len(faces) * 3)
            corner_for_vertex = [{int(welded[v]): c for c, v in enumerate(face)} for face in faces]
            threshold = math.cos(math.radians(crease_degrees))
            for edge, edge_faces in edges.items():
                if len(edge_faces) != 2:
                    continue
                (a, ad), (b, bd) = edge_faces
                if ad == bd or np.dot(fn[a], fn[b]) < threshold:
                    continue
                for vertex in edge:
                    groups.join(a * 3 + corner_for_vertex[a][vertex],
                                b * 3 + corner_for_vertex[b][vertex])
            sums = defaultdict(lambda: np.zeros(3))
            for fi in range(len(faces)):
                for c in range(3):
                    sums[groups.find(fi * 3 + c)] += cross[fi]
            normals = {}
            for key, value in sums.items():
                length = np.linalg.norm(value)
                if length > 1e-20:
                    normals[key] = value / length

            # Extremely folded fans must not interpolate a normal pointing
            # through their own face. Local flat normals retain that coverage.
            output_positions, output_uv, output_normals, output_indices, source_vertices = [], [], [], [], []
            lookup = {}
            fallback_corners = 0
            for fi, face in enumerate(faces):
                for c, vi in enumerate(face):
                    group = groups.find(fi * 3 + c)
                    normal = normals.get(group, fn[fi])
                    if np.dot(normal, fn[fi]) <= 1e-5:
                        normal = fn[fi]
                        group = ('flat', fi)
                        fallback_corners += 1
                    key = (int(vi), group)
                    if key not in lookup:
                        lookup[key] = len(output_positions)
                        output_positions.append(positions[vi])
                        output_uv.append(uv[vi])
                        output_normals.append(normal)
                        source_vertices.append(vi)
                    output_indices.append(lookup[key])
            new_p = np.asarray(output_positions, dtype='<f4')
            new_n = np.asarray(output_normals, dtype='<f4')
            new_uv = np.asarray(output_uv, dtype='<f4')
            new_faces = np.asarray(output_indices, dtype='<u4').reshape(-1, 3)
            assert np.array_equal(new_p[new_faces], positions[faces])
            assert np.array_equal(new_uv[new_faces], uv[faces])
            assert np.max(np.abs(np.linalg.norm(new_n, axis=1) - 1)) < 1e-5
            corner_dot = np.einsum('ij,ikj->ik', fn, new_n[new_faces])
            assert corner_dot.min() > 0, 'A repaired normal opposes its face'
            replace(attributes['POSITION'], new_p)
            replace(attributes['NORMAL'], new_n)
            replace(attributes['TEXCOORD_0'], new_uv)
            for name, array in extra_attributes.items():
                replace(attributes[name], array[source_vertices])
            replace(primitive['indices'], new_faces.reshape(-1, 1), 5125)
            inconsistent = sum(len(v) == 2 and v[0][1] == v[1][1] for v in edges.values())
            assert inconsistent == 0, 'Winding repair left inconsistent manifold edges'
            entry = {'mesh': mi, 'primitive': pi, 'vertices_before': len(positions),
                     'vertices_after': len(new_p), 'triangles_before': len(old_faces),
                     'triangles_after': len(faces), 'removed_degenerate': removed_degenerate,
                     'removed_near_collinear_included_above': removed_near_collinear,
                     'removed_duplicate': removed_duplicate, 'reoriented_faces': int(flipped.sum()),
                     'orientation_conflict_component_sizes': conflicts,
                     'remaining_inconsistent_manifold_edges': int(inconsistent),
                     'flat_fallback_corners': fallback_corners,
                     'maximum_corner_face_angle_degrees': float(np.degrees(np.arccos(np.clip(corner_dot.min(), -1, 1))))}
            stats.append(entry)
            print(json.dumps(entry))

    # Repack all accessors, copying nongeometry bytes verbatim. This keeps skin
    # bind matrices (and any future animation tracks) while removing old streams.
    output_binary = bytearray()
    doc['bufferViews'] = []
    for ai, a in enumerate(doc['accessors']):
        if ai in replacements:
            payload = replacements[ai]
        else:
            payload = read(ai).tobytes()
        output_binary.extend(b'\0' * ((-len(output_binary)) % 4))
        view = {'buffer': 0, 'byteOffset': len(output_binary), 'byteLength': len(payload)}
        if ai in touched:
            view['target'] = 34963 if a['type'] == 'SCALAR' else 34962
        a['bufferView'] = len(doc['bufferViews'])
        a.pop('byteOffset', None)
        doc['bufferViews'].append(view)
        output_binary.extend(payload)
    for image in doc.get('images', []):
        if 'bufferView' not in image:
            continue
        view = original['bufferViews'][image['bufferView']]
        assert view['buffer'] == 0
        payload = binary[view.get('byteOffset', 0):view.get('byteOffset', 0) + view['byteLength']]
        output_binary.extend(b'\0' * ((-len(output_binary)) % 4))
        image['bufferView'] = len(doc['bufferViews'])
        doc['bufferViews'].append({'buffer': 0, 'byteOffset': len(output_binary), 'byteLength': len(payload)})
        output_binary.extend(payload)
    output_binary.extend(b'\0' * ((-len(output_binary)) % 4))
    doc['buffers'] = [{'byteLength': len(output_binary)}]
    assert doc['materials'] == original['materials']
    for field in ['nodes', 'scenes', 'scene', 'skins', 'animations', 'asset']:
        assert doc.get(field) == original.get(field)
    assert len(doc['meshes']) == len(original['meshes'])
    encoded = json.dumps(doc, ensure_ascii=False, separators=(',', ':')).encode('utf-8')
    encoded += b' ' * ((-len(encoded)) % 4)
    total = 28 + len(encoded) + len(output_binary)
    output = (struct.pack('<III', magic, version, total) + struct.pack('<II', len(encoded), 0x4E4F534A)
              + encoded + struct.pack('<II', len(output_binary), 0x004E4942) + output_binary)
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_bytes(output)
    summary = {'source': str(source), 'destination': str(destination), 'crease_degrees': crease_degrees,
               'source_bytes': len(raw), 'output_bytes': len(output), 'primitives': stats}
    destination.with_suffix('.repair.json').write_text(json.dumps(summary, indent=2), encoding='utf-8')
    print('Saved repaired mesh:', destination)
    return summary


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=Path('Content/Models/NewBlackHawk/NewBlackHawk.glb'))
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--crease-degrees', type=float, default=35)
    args = parser.parse_args()
    assert 0 < args.crease_degrees < 90
    repair(args.source, args.output, args.crease_degrees)
