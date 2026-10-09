"""Prepare Bistro Exterior with untouched BC mip blocks, then validate the cook."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys

from PIL import Image
import numpy as np


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2) + "\n", encoding="utf-8")


def run(*args, cwd):
    subprocess.run([str(a) for a in args], cwd=cwd, check=True)


def prepare(source, repo, configuration="Release"):
    source = source.resolve()
    if not (source / "BistroExterior.fbx").is_file():
        raise RuntimeError(f"BistroExterior.fbx is missing in {source}")
    stage = repo / "build/bistro-import"
    model = repo / "Content/Models/BistroExterior"
    cooked = repo / "Content/Cooked/Models/BistroExterior/BistroExterior.sgeasset"
    run("cmake", "--build", "build", "--config", configuration,
        "--target", "BistroSceneConvert", "AssetCooker", "--parallel", "8", cwd=repo)
    run(repo / f"build/{configuration}/BistroSceneConvert.exe",
        source / "BistroExterior.fbx", stage, cwd=repo)
    manifest = json.loads((stage / "source-materials.json").read_text())
    texture_files = {p.name.lower(): p for p in (source / "Textures").iterdir() if p.is_file()}
    references = set()
    alpha_modes = {}
    occlusion_strengths = {}
    for material in manifest["materials"]:
        for key in ("baseColor", "normal", "specular", "emissive"):
            reference = material[key]
            if not reference:
                continue
            filename = reference.replace("\\", "/").split("/")[-1]
            if filename.lower() not in texture_files:
                raise RuntimeError(f"Unresolved {key} map on {material['name']}: {reference}")
            material[key] = texture_files[filename.lower()].name
            references.add(material[key])
        # Inspect alpha and AO coverage only. The cooker copies the original
        # BC blocks rather than encoding these decoded inspection samples.
        alpha = np.asarray(Image.open(source / "Textures" / material["baseColor"]).convert("RGBA"))[:, :, 3]
        low, high = int(alpha.min()), int(alpha.max())
        name = material["name"].lower()
        mode = "OPAQUE"
        if low < 255 or material["opacity"] < 0.999:
            mode = "BLEND" if "glass" in name or material["opacity"] < 0.999 else "MASK"
        material["alphaMode"] = mode
        material["alphaCutoff"] = 0.5
        material["doubleSided"] |= "doublesided" in name or "foliage" in name
        specular = material["specular"]
        if specular not in occlusion_strengths:
            red = np.asarray(Image.open(source / "Textures" / specular).convert("RGB"))[:, :, 0]
            # The supplied Bistro maps leave R entirely blank. Treat that as
            # absent AO, rather than extinguishing every indirect-light sample.
            occlusion_strengths[specular] = 0.0 if not red.any() else 1.0
        material["occlusionStrength"] = occlusion_strengths[specular]
        alpha_modes[material["name"]] = {"mode": mode, "alphaMin": low, "alphaMax": high}
    if manifest["triangles"] != 2832120:
        raise RuntimeError(f"Exterior triangle count changed: {manifest['triangles']}")
    manifest["textureRoot"] = str(source / "Textures")
    model.mkdir(parents=True, exist_ok=True)
    for filename in ("BistroExterior.gltf", "BistroExterior.bin"):
        shutil.copy2(stage / filename, model / filename)
    write_json(model / "BistroExterior.bistro.json", manifest)
    for filename in ("LICENSE.txt", "README.txt", "CHANGELOG.txt"):
        shutil.copy2(source / filename, model / filename)
    sky = repo / "Content/Textures/Sky/san_giuseppe_bridge_4k.hdr"
    sky.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source / sky.name, sky)
    run(repo / f"build/{configuration}/AssetCooker.exe",
        model / "BistroExterior.gltf", cooked, cwd=repo)
    stats = json.loads(Path(str(cooked) + ".stats.json").read_text())
    if stats["triangles"] != manifest["triangles"]:
        raise RuntimeError("Cooking changed the exterior triangle count")
    # Compare the final file's payload against each source DDS, not just the
    # cooker's in-memory representation. This catches offsets and mip errors.
    with cooked.open("rb") as asset:
        for texture in stats["textures"]:
            raw = Path(texture["source"]).read_bytes()
            asset.seek(texture["offset"])
            blocks = asset.read(texture["size"])
            if blocks != raw[128:128 + texture["size"]]:
                raise RuntimeError(f"DDS blocks changed: {texture['source']}")
    low, high = np.array(manifest["boundsMin"]), np.array(manifest["boundsMax"])
    centre = (low + high) * 0.5
    placement = np.array([-centre[0], 2.75 - low[1], -centre[2]])
    diagonal = float(np.linalg.norm(high - low))
    spawn = (centre + placement).tolist()
    spawn[1] += diagonal * 0.4
    spawn[2] = float(low[2] + placement[2] - diagonal * 0.6)
    transform = {"position": placement.tolist(), "rotation": [0, 0, 0], "scale": [1, 1, 1]}
    write_json(repo / "Content/Prefabs/Scenes/BistroExterior.json", {
        "schemaVersion": 2, "id": "scenes/bistro-exterior", "name": "Bistro Exterior",
        "components": {"staticMesh": {
            "path": "Content/Models/BistroExterior/BistroExterior.gltf",
            "defaultScale": [1, 1, 1], "targetSize": 0, "castShadow": True,
            "useMaterials": True, "automaticLod": False}, "collision": {"shape": "none"}}})
    write_json(repo / "Content/Levels/BistroExterior.json", {
        "schemaVersion": 1, "name": "Bistro Exterior", "insertionMode": "spawn",
        "patrolBoatEnabled": False,
        "environmentMap": "Content/Textures/Sky/san_giuseppe_bridge_4k.hdr",
        "renderingProfile": "bistroExterior",
        "terrain": {"heightScale": 0, "flat": True, "tilesX": 16, "tilesZ": 16,
                    "autoFoliage": False, "sculpt": []},
        "entities": [
            {"id": 1, "type": "player_spawn", "name": "Bistro Overview", "enabled": True,
             "transform": {"position": spawn, "rotation": [-25, 180, 0], "scale": [1, 1, 1]}},
            {"id": 2, "type": "prefab", "prefab": "scenes/bistro-exterior",
             "name": "Bistro Exterior", "enabled": True, "transform": transform}]})
    report = {"source": str(source), "verticesBeforeCook": manifest["vertices"],
              "vertices": stats["vertices"], "indices": stats["indices"], "triangles": stats["triangles"],
              "textures": len(stats["textures"]), "referencedDDS": len(references),
              "textureBytes": stats["textureBytes"], "ddsBlocksByteIdentical": True,
              "boundsMin": low.tolist(), "boundsMax": high.tolist(), "placement": placement.tolist(),
              "sourceFBXSHA256": hashlib.sha256((source / "BistroExterior.fbx").read_bytes()).hexdigest(),
              "alphaModes": alpha_modes,
              "disabledBlankOcclusionMaps": [name for name, strength in occlusion_strengths.items() if strength == 0],
              "knownLightingDifference": "Falcor pyscene uses envMap.intensity=10; engine gains are retained."}
    write_json(model / "import-report.json", report)
    run("py", "-3.10-64", repo / "scripts/validate-bistro.py", "--repo", repo, cwd=repo)
    run("cmake", "--build", "build", "--config", configuration, "--target", "RuntimeContent", cwd=repo)
    print(f"Bistro ready: {stats['vertices']:,} vertices, {stats['triangles']:,} triangles, "
          f"{len(stats['textures'])} BC textures, {stats['textureBytes'] / 1024**3:.3f} GiB texture payload.")
    return report


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("--configuration", default="Release")
    args = parser.parse_args()
    try:
        prepare(args.source, Path(__file__).resolve().parents[1], args.configuration)
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(error, file=sys.stderr)
        sys.exit(1)
