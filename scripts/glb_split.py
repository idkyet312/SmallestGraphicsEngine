"""Shared helpers for carving a source GLB into several runtime GLBs.

AssetCooker flattens every node into one mesh, so any part that has to move
on its own at runtime (a turret, a barrel) has to arrive as its own file.
`build` writes a self-contained document holding just the nodes it is given,
with the binary compacted to what those nodes reference, and can downscale
and recolour on the way through.
"""
import io
import json
import struct


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


def downscaled_png(data, max_size, cache=None):
    """PNG bytes no larger than max_size on either side. Cached by identity so
    an image shared by several output files is only resampled once."""
    key = (id(data), max_size)
    if cache is not None and key in cache:
        return cache[key]
    from PIL import Image
    image = Image.open(io.BytesIO(data))
    if max(image.size) <= max_size:
        result = data
    else:
        scale = max_size / max(image.size)
        size = (max(1, round(image.size[0] * scale)),
                max(1, round(image.size[1] * scale)))
        image = image.resize(size, Image.LANCZOS)
        out = io.BytesIO()
        image.save(out, format="PNG", compress_level=6)
        result = out.getvalue()
    if cache is not None:
        cache[key] = result
    return result


def build(doc, binary, nodes, root_nodes, image_size=None,
          material_edit=None, image_cache=None):
    """Returns a compacted (document, binary) holding `nodes` (each node's
    "children" already index into `nodes`) and only the data they reach.

    image_size(image) -> max pixel size or None keeps an image as authored.
    material_edit(material) mutates a copied material in place.
    """
    mesh_map, material_map, texture_map, image_map = {}, {}, {}, {}
    sampler_map, accessor_map, view_map = {}, {}, {}
    out = {"asset": doc["asset"], "scene": 0,
           "scenes": [{"name": "Scene", "nodes": root_nodes}],
           "nodes": [], "meshes": [], "materials": [], "textures": [],
           "images": [], "samplers": [], "accessors": [], "bufferViews": [],
           "buffers": []}
    blob = bytearray()

    def append_view(source, chunk):
        blob.extend(b"\0" * ((4 - len(blob) % 4) % 4))
        source["buffer"] = 0
        source["byteOffset"] = len(blob)
        source["byteLength"] = len(chunk)
        blob.extend(chunk)
        out["bufferViews"].append(source)
        return len(out["bufferViews"]) - 1

    def view(index):
        if index not in view_map:
            source = dict(doc["bufferViews"][index])
            start = source.get("byteOffset", 0)
            view_map[index] = append_view(
                source, binary[start:start + source["byteLength"]])
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
                limit = image_size(source) if image_size else None
                if limit:
                    original = doc["bufferViews"][source["bufferView"]]
                    start = original.get("byteOffset", 0)
                    data = binary[start:start + original["byteLength"]]
                    if image_cache is not None:
                        data = image_cache.setdefault(
                            ("source", index), data)
                    source["bufferView"] = append_view(
                        {}, downscaled_png(data, limit, image_cache))
                    source["mimeType"] = "image/png"
                else:
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
            return result
        if isinstance(value, list):
            return [remap_texture_refs(item) for item in value]
        return value

    def material(index):
        if index not in material_map:
            copy = remap_texture_refs(json.loads(json.dumps(
                doc["materials"][index])))
            if material_edit:
                material_edit(copy)
            material_map[index] = len(out["materials"])
            out["materials"].append(copy)
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
