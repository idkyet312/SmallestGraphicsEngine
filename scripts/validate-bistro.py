"""Check Bistro's final cooked geometry, material records and untouched DDS mips."""
import argparse
import json
import mmap
from pathlib import Path
import struct

import numpy as np
from PIL import Image


def validate(repo):
    model = repo / "Content/Models/BistroExterior"
    manifest = json.loads((model / "BistroExterior.bistro.json").read_text())
    report_path = model / "import-report.json"
    report = json.loads(report_path.read_text())
    cooked = repo / "Content/Cooked/Models/BistroExterior/BistroExterior.sgeasset"
    stats = json.loads(Path(str(cooked) + ".stats.json").read_text())
    primitive_layout = struct.Struct("<8I6f6Q")
    low, high = np.full(3, np.inf), np.full(3, -np.inf)
    vertices = triangles = 0
    max_normal_error = max_tangent_dot = 0.0
    with cooked.open("rb") as file, mmap.mmap(file.fileno(), 0, access=mmap.ACCESS_READ) as data:
        primitive_count, material_count, texture_count = struct.unpack_from("<3I", data, 56)
        primitive_offset, material_offset, texture_offset = struct.unpack_from("<3Q", data, 72)
        assert texture_count == 405 and material_count == 133
        for i in range(primitive_count):
            p = primitive_layout.unpack_from(data, primitive_offset + i * primitive_layout.size)
            _, material, vertex_count, index_count, index_size = p[:5]
            assert material < material_count and index_size in (2, 4) and index_count % 3 == 0
            v = np.frombuffer(data, dtype="<f4", count=vertex_count * 12, offset=p[14]).reshape(-1, 12).copy()
            indices = np.frombuffer(data, dtype="<u2" if index_size == 2 else "<u4",
                                    count=index_count, offset=p[15]).copy()
            assert np.isfinite(v).all() and int(indices.max()) < vertex_count
            low = np.minimum(low, v[:, :3].min(axis=0))
            high = np.maximum(high, v[:, :3].max(axis=0))
            max_normal_error = max(max_normal_error, float(np.abs(np.linalg.norm(v[:, 3:6], axis=1) - 1).max()))
            max_tangent_dot = max(max_tangent_dot, float(np.abs(np.sum(v[:, 3:6] * v[:, 8:11], axis=1)).max()))
            assert np.isin(v[:, 11], (-1.0, 1.0)).all()
            vertices += vertex_count
            triangles += index_count // 3
        assert vertices == stats["vertices"] and triangles == 2832120
        assert np.allclose(low, manifest["boundsMin"], atol=0.001)
        assert np.allclose(high, manifest["boundsMax"], atol=0.001)
        assert max_normal_error < 0.005 and max_tangent_dot < 0.005
        # The optimized Assimp material order is independent of the FBX order.
        string_offset = struct.unpack_from("<Q", data, 104)[0]
        authored = {m["name"]: m for m in manifest["materials"]}
        for i in range(material_count):
            m = struct.unpack_from("<6I8f", data, material_offset + i * 56)
            name_end = data.find(b"\0", string_offset + m[0])
            name = data[string_offset + m[0]:name_end].decode()
            if name not in authored:
                assert not any(primitive_layout.unpack_from(data, primitive_offset + j * 104)[1] == i
                               for j in range(primitive_count))
                continue
            a = authored[name]
            assert all(index < texture_count for index in m[2:5] if index != 0xffffffff)
            assert m[1] & 32 and m[1] & 64  # DirectX normal convention and packed AO.
            strength = (m[1] >> 16) / 65535 if m[1] & 128 else 1.0
            assert abs(strength - a.get("occlusionStrength", 1.0)) < 0.00002
            assert bool(m[1] & 4) == (a["alphaMode"] == "MASK")
            assert bool(m[1] & 8) == (a["alphaMode"] == "BLEND")
            assert bool(m[1] & 16) == bool(a["emissive"])
            if a["emissive"]:
                assert m[5] < texture_count and abs(m[13] - a["emissiveFactor"][0]) < 1e-6
        formats, dimensions = {}, {}
        for t in stats["textures"]:
            raw = Path(t["source"]).read_bytes()
            height, width = struct.unpack_from("<2I", raw, 12)
            assert (width, height) == (t["width"], t["height"])
            assert data[t["offset"]:t["offset"] + t["size"]] == raw[128:128 + t["size"]]
            formats[str(t["format"])] = formats.get(str(t["format"]), 0) + 1
            key = f"{width}x{height}"
            dimensions[key] = dimensions.get(key, 0) + 1
    samples = []
    texture_root = Path(manifest["textureRoot"])
    for material in [manifest["materials"][i] for i in (0, 15, 40, 65, 100)]:
        packed = np.asarray(Image.open(texture_root / material["specular"]).convert("RGB"))[::32, ::32]
        xy = np.asarray(Image.open(texture_root / material["normal"]).convert("RGB"))[::32, ::32, :2].astype(np.float32) / 127.5 - 1.0
        xy[:, :, 1] *= -1.0
        z = np.sqrt(np.maximum(0, 1 - np.sum(xy * xy, axis=2)))
        assert np.isfinite(z).all()
        samples.append({"material": material["name"], "occlusionRoughnessMetalnessMean":
                        (packed.mean(axis=(0, 1)) / 255).tolist(), "reconstructedNormalZRange":
                        [float(z.min()), float(z.max())]})
    report["validation"] = {"cookedGeometryAndBounds": True, "materialReferencesAndAlphaModes": True,
                            "ddsBlocksByteIdentical": True, "textureDimensions": dimensions,
                            "textureFormats": formats, "maxNormalLengthError": max_normal_error,
                            "maxTangentNormalDot": max_tangent_dot, "packedAndNormalSamples": samples}
    report_path.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report["validation"], indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo", type=Path, default=Path(__file__).resolve().parents[1])
    validate(parser.parse_args().repo)
