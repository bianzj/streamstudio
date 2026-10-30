"""Download CC0/open glTF assets and build Stream Studio-friendly colored OBJ LODs."""

from __future__ import annotations

import hashlib
import json
import os
import re
import sys
import tempfile
import urllib.request
from pathlib import Path

import numpy as np
import trimesh


ROOT = Path(__file__).resolve().parents[1]
LIBRARY = ROOT / "assets" / "obj-library"
USER_AGENT = "Stream Studio realistic asset importer"

POLYHAVEN = [
    ("fir_sapling", "vegetation/realistic/fir_sapling_realistic", 70000),
    ("fir_tree_01", "vegetation/typical/mature_fir_realistic", 70000),
    ("pine_tree_01", "vegetation/typical/mature_pine_realistic", 70000),
    ("tree_small_02", "vegetation/typical/street_tree_realistic", 60000),
    ("island_tree_02", "vegetation/realistic/island_tree_realistic", 90000),
    ("searsia_lucida", "vegetation/realistic/searsia_tree_realistic", 70000),
    ("jacaranda_tree", "vegetation/realistic/jacaranda_tree_realistic", 120000),
    ("covered_car", "vehicle/realistic/covered_car_realistic", 80000),
]

KHRONOS = [
    ("CarConcept", "vehicle/realistic/car_concept_realistic", 120000),
    ("CesiumMilkTruck", "vehicle/realistic/milk_truck_realistic", 80000),
]


def request_json(url: str):
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(request) as response:
        return json.load(response)


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        while chunk := source.read(1024 * 1024):
            digest.update(chunk)
    return digest.hexdigest()


def download(url: str, target: Path):
    target.parent.mkdir(parents=True, exist_ok=True)
    metadata_path = target.with_name(target.name + ".download.json")
    try:
        metadata = json.loads(metadata_path.read_text(encoding="utf-8"))
        if (isinstance(metadata, dict) and metadata.get("version") == 1
                and metadata.get("url") == url and metadata.get("complete") is True
                and target.is_file() and target.stat().st_size > 0
                and metadata.get("size") == target.stat().st_size
                and metadata.get("sha256") == file_sha256(target)):
            return
    except (OSError, ValueError):
        pass
    # Legacy files have no completion proof: keep them until a full replacement succeeds.
    print(f"download {url}")
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    temporary_path = None
    temporary_metadata_path = None
    try:
        with urllib.request.urlopen(request) as response:
            if response.status == 206 or response.headers.get("Content-Range") is not None:
                raise ValueError(f"Partial response received for {url}")
            length_header = response.headers.get("Content-Length")
            expected_size = int(length_header) if length_header is not None else None
            digest = hashlib.sha256()
            size = 0
            with tempfile.NamedTemporaryFile(mode="wb", dir=target.parent,
                                             prefix=f".{target.name}.", suffix=".part", delete=False) as output:
                temporary_path = Path(output.name)
                while chunk := response.read(1024 * 1024):
                    output.write(chunk)
                    digest.update(chunk)
                    size += len(chunk)
                output.flush()
                os.fsync(output.fileno())
            if size == 0 or (expected_size is not None and size != expected_size):
                raise ValueError(f"Incomplete download for {url}: expected {expected_size}, received {size} bytes")
        metadata = {"version": 1, "complete": True, "url": url,
                    "size": size, "sha256": digest.hexdigest()}
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=target.parent,
                                         prefix=f".{metadata_path.name}.", suffix=".part", delete=False) as output:
            temporary_metadata_path = Path(output.name)
            json.dump(metadata, output, ensure_ascii=False)
            output.flush()
            os.fsync(output.fileno())
        os.replace(temporary_path, target)
        os.replace(temporary_metadata_path, metadata_path)
    finally:
        for path in (temporary_path, temporary_metadata_path):
            if path is not None:
                path.unlink(missing_ok=True)


def polyhaven_source(asset_id: str, source_dir: Path) -> Path:
    files = request_json(f"https://api.polyhaven.com/files/{asset_id}")
    descriptor = files["gltf"]["1k"]["gltf"]
    gltf_path = source_dir / Path(descriptor["url"]).name
    download(descriptor["url"], gltf_path)
    for name, item in descriptor.get("include", {}).items():
        download(item["url"], source_dir / name)
    (source_dir / "SOURCE.txt").write_text(
        f"Poly Haven asset: {asset_id}\n"
        f"Source: https://polyhaven.com/a/{asset_id}\n"
        "License: CC0 1.0 Universal\n"
        "License URL: https://creativecommons.org/publicdomain/zero/1.0/\n",
        encoding="utf-8",
    )
    return gltf_path


def khronos_source(model: str, source_dir: Path) -> Path:
    api = f"https://api.github.com/repos/KhronosGroup/glTF-Sample-Assets/contents/Models/{model}/glTF"
    items = request_json(api)
    gltf_path = None
    for item in items:
        if item.get("type") != "file":
            continue
        target = source_dir / item["name"]
        download(item["download_url"], target)
        if target.suffix.lower() == ".gltf":
            gltf_path = target
    readme_url = f"https://raw.githubusercontent.com/KhronosGroup/glTF-Sample-Assets/main/Models/{model}/README.md"
    try:
        download(readme_url, source_dir / "SOURCE_README.md")
    except Exception as error:
        print(f"warning: README unavailable for {model}: {error}")
    (source_dir / "SOURCE.txt").write_text(
        f"Khronos glTF Sample Asset: {model}\n"
        f"Source: https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/{model}\n"
        "License: see SOURCE_README.md and upstream repository license metadata.\n",
        encoding="utf-8",
    )
    if gltf_path is None:
        raise RuntimeError(f"No glTF file found for {model}")
    return gltf_path


def clean_name(value: str, fallback: str) -> str:
    name = re.sub(r"[^A-Za-z0-9_.-]+", "_", value or "").strip("_")
    return name or fallback


def material_color(mesh: trimesh.Trimesh) -> np.ndarray:
    visual = getattr(mesh, "visual", None)
    material = getattr(visual, "material", None)
    candidates = [
        getattr(material, "baseColorFactor", None),
        getattr(material, "main_color", None),
        getattr(visual, "main_color", None),
    ]
    for candidate in candidates:
        if candidate is None:
            continue
        values = np.asarray(candidate, dtype=float).reshape(-1)
        if len(values) >= 3 and np.all(np.isfinite(values[:3])):
            rgb = values[:3]
            if np.max(rgb) > 1.0:
                rgb = rgb / 255.0
            return np.clip(rgb, 0.02, 1.0)
    return np.array([0.55, 0.58, 0.52])


def semantic_color(name: str, source: np.ndarray) -> np.ndarray:
    hint = name.lower()
    presets = (
        (("leaf", "leaves", "needle", "twig", "foliage"), [0.12, 0.38, 0.10]),
        (("trunk", "bark", "branch", "wood", "stem"), [0.30, 0.18, 0.09]),
        (("tire", "rubber", "wheel"), [0.035, 0.04, 0.045]),
        (("glass", "window", "windshield"), [0.18, 0.32, 0.38]),
        (("light", "lamp", "headlight"), [0.88, 0.82, 0.60]),
        (("sail", "canvas"), [0.78, 0.75, 0.64]),
        (("rig", "rope"), [0.20, 0.13, 0.07]),
        (("hull",), [0.24, 0.20, 0.16]),
    )
    for words, color in presets:
        if any(word in hint for word in words):
            return np.asarray(color)
    return source


def simplify_mesh(mesh: trimesh.Trimesh, target: int) -> trimesh.Trimesh:
    if len(mesh.faces) <= target or target < 32:
        return mesh.copy()
    try:
        result = mesh.simplify_quadric_decimation(face_count=target)
    except Exception as error:
        print(f"warning: simplification failed ({error}); retaining source mesh")
        result = mesh.copy()
    if len(result.faces) <= target:
        return result
    # Alpha-card foliage contains many disconnected triangles that QEM cannot
    # collapse. Retain a deterministic, spatially distributed face subset so
    # large trees still obey the requested simulation/preview budget.
    indices = np.linspace(0, len(result.faces) - 1, target, dtype=np.int64)
    faces = np.asarray(result.faces)[indices]
    used, inverse = np.unique(faces.reshape(-1), return_inverse=True)
    return trimesh.Trimesh(
        vertices=np.asarray(result.vertices)[used],
        faces=inverse.reshape((-1, 3)),
        process=False,
    )


def vertex_normals(vertices: np.ndarray, faces: np.ndarray) -> np.ndarray:
    """Compute smooth normals without scipy (keeps the importer self-contained)."""
    triangles = vertices[faces]
    face_normals = np.cross(triangles[:, 1] - triangles[:, 0], triangles[:, 2] - triangles[:, 0])
    normals = np.zeros_like(vertices, dtype=np.float64)
    for corner in range(3):
        np.add.at(normals, faces[:, corner], face_normals)
    lengths = np.linalg.norm(normals, axis=1)
    normals[lengths > 1e-12] /= lengths[lengths > 1e-12, None]
    normals[lengths <= 1e-12] = [0.0, 1.0, 0.0]
    return normals


def export_colored_obj(gltf_path: Path, output_path: Path, target_faces: int, source_url: str):
    scene = trimesh.load(gltf_path, force="scene", process=False)
    meshes = [mesh for mesh in scene.dump() if isinstance(mesh, trimesh.Trimesh) and len(mesh.faces)]
    if not meshes:
        raise RuntimeError(f"No triangle mesh in {gltf_path}")
    source_faces = sum(len(mesh.faces) for mesh in meshes)
    grouped_meshes = {}
    for index, mesh in enumerate(meshes):
        material = getattr(getattr(mesh, "visual", None), "material", None)
        material_name = clean_name(getattr(material, "name", ""), f"material_{index + 1}")
        mesh_name = clean_name(str(mesh.metadata.get("name", "")), f"mesh_{index + 1}")
        color = semantic_color(f"{mesh_name} {material_name}", material_color(mesh))
        # Photogrammetry assets often contain thousands of transformed leaf
        # nodes sharing one material. Merge those nodes before decimation so
        # the requested LOD budget remains a real upper bound.
        key = (material_name, tuple(np.round(color, 5)))
        grouped_meshes.setdefault(key, []).append(mesh)
    groups = []
    for group_index, ((material_name, color), members) in enumerate(grouped_meshes.items()):
        mesh = members[0].copy() if len(members) == 1 else trimesh.util.concatenate(members)
        groups.append((mesh, clean_name(material_name, f"material_{group_index + 1}"), np.asarray(color)))
    output_path.parent.mkdir(parents=True, exist_ok=True)
    lines = [
        "# Stream Studio realistic medium-detail OBJ",
        f"# Source: {source_url}",
        "# Coordinates: Y-up; vertex RGB colors embedded after XYZ",
        f"# Source triangles: {source_faces}; target triangles: {target_faces}",
    ]
    vertex_offset = 0
    normal_offset = 0
    written_faces = 0
    for index, (mesh, material_name, color) in enumerate(groups):
        share = max(32, round(target_faces * len(mesh.faces) / source_faces))
        lod = simplify_mesh(mesh, share)
        mesh_name = f"part_{index + 1}"
        vertices = np.asarray(lod.vertices)
        faces = np.asarray(lod.faces)
        normals = vertex_normals(vertices, faces)
        lines.append(f"o {mesh_name}__{material_name}")
        lines.append(f"g {mesh_name}__{material_name}")
        for vertex in vertices:
            lines.append(
                f"v {vertex[0]:.7f} {vertex[1]:.7f} {vertex[2]:.7f} "
                f"{color[0]:.5f} {color[1]:.5f} {color[2]:.5f}"
            )
        for normal in normals:
            lines.append(f"vn {normal[0]:.7f} {normal[1]:.7f} {normal[2]:.7f}")
        for face in faces:
            corners = [
                f"{vertex_offset + int(local) + 1}//{normal_offset + int(local) + 1}"
                for local in face
            ]
            lines.append("f " + " ".join(corners))
        vertex_offset += len(lod.vertices)
        normal_offset += len(lod.vertices)
        written_faces += len(lod.faces)
    output_path.write_text("\n".join(lines) + "\n", encoding="utf-8")
    bounds = np.stack([mesh.bounds for mesh in meshes], axis=0)
    minimum = bounds[:, 0, :].min(axis=0)
    maximum = bounds[:, 1, :].max(axis=0)
    dimensions = maximum - minimum
    metadata = {
        "source": source_url,
        "sourceTriangles": source_faces,
        "triangles": written_faces,
        "dimensions": [round(float(value), 4) for value in dimensions],
        "coordinateSystem": "Y-up",
        "vertexColors": True,
    }
    output_path.with_suffix(".json").write_text(json.dumps(metadata, ensure_ascii=False, indent=2), encoding="utf-8")
    print(f"created {output_path}: {written_faces} triangles, dimensions={metadata['dimensions']}")


def main():
    for asset_id, relative, target_faces in POLYHAVEN:
        output = LIBRARY / f"{relative}.obj"
        source_dir = output.parent / f"_{output.stem}_source"
        gltf = polyhaven_source(asset_id, source_dir)
        export_colored_obj(gltf, output, target_faces, f"https://polyhaven.com/a/{asset_id}")
    for model, relative, target_faces in KHRONOS:
        output = LIBRARY / f"{relative}.obj"
        source_dir = output.parent / f"_{output.stem}_source"
        gltf = khronos_source(model, source_dir)
        export_colored_obj(
            gltf,
            output,
            target_faces,
            f"https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/{model}",
        )


if __name__ == "__main__":
    main()
