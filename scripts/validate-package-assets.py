#!/usr/bin/env python3
"""Validate a staged SmallestGraphicsEngine package without loading DX12."""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
from urllib.parse import unquote
from pathlib import Path


MODEL_RE = re.compile(r"(?i)Content[/\\]+Models[/\\]+([^\"']+)")
TERRAIN = (
    "Content/Models/Grass3/Grass004_2K-JPG/Grass004_2K-JPG_Color.jpg",
    "Content/Models/Grass3/Grass004_2K-JPG/Grass004_2K-JPG_NormalGL.jpg",
    "Content/Models/Grass3/Grass004_2K-JPG/Grass004_2K-JPG_Roughness.jpg",
    "Content/Models/Grass3/Grass004_2K-JPG/Grass004_2K-JPG_AmbientOcclusion.jpg",
    "Content/Models/terrain/dirt_floor/dirt_floor_diff_1k.png",
    "Content/Models/terrain/dirt_floor/dirt_floor_nor_gl_1k.png",
    "Content/Models/terrain/dirt_floor/dirt_floor_rough_1k.png",
)


def files(root: Path):
    return (p for p in root.rglob("*") if p.is_file())


def package_model_exists(root: Path, rel: str) -> bool:
    rel = rel.replace("/", "\\")
    source = root / rel
    if source.is_file():
        return True
    # The cooker preserves the source relative path and changes only the
    # extension. Do not accept an arbitrary blob elsewhere in the directory.
    parts = Path(rel).parts
    if len(parts) >= 3 and parts[0].lower() == "content" and parts[1].lower() == "models":
        cooked = root / "Content" / "Cooked" / "Models" / Path(*parts[2:])
        cooked = cooked.with_suffix(".sgeasset")
        return cooked.is_file() and cooked.stat().st_size > 0
    return False


def strings(value):
    if isinstance(value, str):
        yield value
    elif isinstance(value, dict):
        for v in value.values():
            yield from strings(v)
    elif isinstance(value, list):
        for v in value:
            yield from strings(v)


def check_glb(path: Path):
    """Return external URI dependencies from a GLB, or an error."""
    try:
        data = path.read_bytes()
        if len(data) < 20 or data[:4] != b"glTF":
            return [], "invalid GLB header"
        _, _, length = struct.unpack_from("<4sII", data, 0)
        if length > len(data):
            return [], "truncated GLB"
        pos = 12
        while pos + 8 <= length:
            size, kind = struct.unpack_from("<II", data, pos)
            chunk = data[pos + 8:pos + 8 + size]
            pos += 8 + size
            if kind == 0x4E4F534A:  # JSON
                obj = json.loads(chunk.rstrip(b" \t\r\n\0").decode("utf-8"))
                uris = []
                for item in obj.get("images", []):
                    if isinstance(item, dict) and item.get("uri"):
                        uris.append(unquote(item["uri"]))
                for item in obj.get("buffers", []):
                    if isinstance(item, dict) and item.get("uri"):
                        uris.append(unquote(item["uri"]))
                return uris, None
        return [], "missing JSON chunk"
    except Exception as exc:  # malformed third-party asset; report, never abort scan
        return [], str(exc)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("package", type=Path)
    ap.add_argument("--repo", type=Path, help="repository root for direct runtime references")
    ap.add_argument("--manifest", type=Path, help="write SHA-256 manifest here")
    args = ap.parse_args()
    root = args.package.resolve()
    hard, optional, notes = [], [], []

    zero = [p for p in files(root) if p.stat().st_size == 0]
    for p in zero:
        hard.append(f"zero-byte file: {p.relative_to(root)}")

    json_roots = [root / x for x in ("levels", "Content/Levels", "prefabs", "Content/Prefabs")]
    seen = set()
    seen_json_content = set()
    prefab_ids = set()
    prefab_files = {}
    for base in json_roots[2:]:
        if base.is_dir():
            for path in base.rglob("*.json"):
                try:
                    obj = json.loads(path.read_text(encoding="utf-8-sig"))
                    ident = obj.get("id") if isinstance(obj, dict) else None
                    if ident:
                        prefab_ids.add(str(ident).replace("\\", "/").lower())
                        prefab_files[str(ident).replace("\\", "/").lower()] = path
                except Exception:
                    pass
    for base in json_roots:
        if not base.is_dir():
            continue
        for path in base.rglob("*.json"):
            content_key = hashlib.sha1(path.read_bytes()).digest()
            if content_key in seen_json_content:
                continue
            seen_json_content.add(content_key)
            seen.add(path)
            try:
                obj = json.loads(path.read_text(encoding="utf-8-sig"))
            except Exception as exc:
                hard.append(f"invalid JSON {path.relative_to(root)}: {exc}")
                continue
            # Level entities and prefab inheritance refer to IDs without the
            # .json suffix. Resolve every value attached to a prefab-like key.
            def prefab_values(value):
                if isinstance(value, dict):
                    for key, child in value.items():
                        if key.lower() in ("prefab", "prefabid", "childprefab", "extends") and isinstance(child, str):
                            yield child
                        yield from prefab_values(child)
                elif isinstance(value, list):
                    for child in value:
                        yield from prefab_values(child)
            if base.name.lower() in ("levels", "content") or "Levels" in path.parts:
                for ident in prefab_values(obj):
                    if ident.replace("\\", "/").lower().removesuffix(".json") not in prefab_ids:
                        hard.append(f"{path.relative_to(root)} -> missing prefab id {ident}")
            for value in strings(obj):
                for match in MODEL_RE.finditer(value):
                    rel = "Content/Models/" + match.group(1).replace("\\", "/")
                    if not package_model_exists(root, rel):
                        hard.append(f"{path.relative_to(root)} -> missing model {rel}")

    for rel in TERRAIN:
        if not (root / rel).is_file():
            hard.append(f"missing required terrain asset: {rel}")

    for path in files(root):
        if path.suffix.lower() == ".glb":
            uris, error = check_glb(path)
            if error:
                hard.append(f"{path.relative_to(root)}: {error}")
            for uri in uris:
                if uri.startswith("data:"):
                    continue
                if not (path.parent / uri).is_file():
                    hard.append(f"{path.relative_to(root)} -> missing external {uri}")
        elif path.suffix.lower() == ".gltf":
            try:
                obj = json.loads(path.read_text(encoding="utf-8"))
                for item in obj.get("images", []) + obj.get("buffers", []):
                    uri = item.get("uri") if isinstance(item, dict) else None
                    uri = unquote(uri) if uri else uri
                    if uri and not uri.startswith("data:") and not (path.parent / uri).is_file():
                        hard.append(f"{path.relative_to(root)} -> missing external {uri}")
            except Exception as exc:
                hard.append(f"{path.relative_to(root)}: invalid glTF ({exc})")

    if args.repo:
        for src in (args.repo / "src").rglob("*"):
            if src.suffix.lower() not in (".h", ".hpp", ".cpp", ".hlsl", ".hlsli"):
                continue
            text = src.read_text(encoding="utf-8", errors="ignore")
            text = re.sub(r"/\*.*?\*/|//[^\n]*", " ", text, flags=re.S)
            for match in MODEL_RE.finditer(text):
                if Path(match.group(1)).suffix.lower() not in (".glb", ".gltf", ".fbx", ".obj"):
                    continue
                rel = "Content/Models/" + match.group(1).replace("\\", "/")
                if not (root / rel).is_file():
                    hard.append(f"runtime {src.relative_to(args.repo)} -> missing {rel}")

    if args.manifest:
        with args.manifest.open("w", encoding="utf-8", newline="\n") as out:
            manifest_path = args.manifest.resolve()
            for path in sorted(files(root), key=lambda p: p.relative_to(root).as_posix().lower()):
                if path.resolve() == manifest_path:
                    continue
                digestor = hashlib.sha256()
                with path.open("rb") as stream:
                    for block in iter(lambda: stream.read(1024 * 1024), b""):
                        digestor.update(block)
                digest = digestor.hexdigest()
                out.write(f"{digest}  {path.relative_to(root).as_posix()}\n")

    print(f"Package: {root}")
    print(f"Files: {sum(1 for _ in files(root))}; JSON scanned: {len(seen)}")
    print(f"HARD_MISSING: {len(hard)}")
    for item in sorted(set(hard)):
        print(f"  {item}")
    print(f"OPTIONAL_MISSING: {len(optional)}")
    for item in sorted(set(optional)):
        print(f"  {item}")
    if notes:
        print(f"NOTES: {len(notes)}")
        for item in notes:
            print(f"  {item}")
    if args.manifest:
        print(f"Manifest: {args.manifest.resolve()}")
    return 1 if hard else 0


if __name__ == "__main__":
    raise SystemExit(main())
