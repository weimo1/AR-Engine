import json
import math
from pathlib import Path
import struct
import subprocess
import sys
from tempfile import TemporaryDirectory

from check_static_scene import make_mesh


def make_scene(output: Path) -> tuple[Path, Path]:
    assets = output / "assets"
    assets.mkdir()
    near = make_mesh(assets, "near", bytes([128, 64, 192, 255] * 4))
    far = make_mesh(assets, "far", bytes([32, 160, 80, 255] * 4))
    near_path = assets / near
    model = json.loads(near_path.read_text(encoding="utf-8"))
    model["materials"][0]["pbrMetallicRoughness"].update(
        baseColorFactor=[0.5, 0.6, 0.7, 1],
        metallicFactor=0.4,
        roughnessFactor=0.7,
        metallicRoughnessTexture={"index": 0},
    )
    with (near_path.parent / "quad.bin").open("ab") as binary:
        binary.write(struct.pack("<12f", *([0, 0.6, 0.8] * 4)))
    model["buffers"][0]["byteLength"] += 48
    model["bufferViews"].append({"buffer": 0, "byteOffset": 92, "byteLength": 48})
    model["accessors"].append({"bufferView": 3, "componentType": 5126, "count": 4, "type": "VEC3"})
    model["meshes"][0]["primitives"][0]["attributes"]["NORMAL"] = 3
    near_path.write_text(json.dumps(model), encoding="utf-8")
    camera = {"position": [0, 0, 5], "orientation": [0, 0, 0, 1],
              "fov_y": math.pi / 4, "near": 0.1, "far": 100}
    objects = [
        {"id": 1, "asset": near, "position": [0, 0.4, 0.5],
         "rotation": [0, 0, 0, 1], "scale": [0.8, 0.75, 0.5]},
        {"id": 2, "asset": far, "position": [0, 0, -0.5],
         "rotation": [0, 0, 0, 1], "scale": [1.5, 1.5, 1]},
    ]
    scene = output / "scene.json"
    scene.write_text(json.dumps({"version": 1, "camera": camera, "objects": objects}), encoding="utf-8")
    return scene, assets


def check_values(name: str, actual: list, expected: list, tolerance: float) -> None:
    if len(actual) != len(expected) or any(
        not math.isclose(a, e, abs_tol=tolerance) for a, e in zip(actual, expected)
    ):
        raise RuntimeError(f"{name}: expected {expected}, actual {actual}, tolerance {tolerance}")


def srgb(byte: int) -> float:
    value = byte / 255
    return value / 12.92 if value <= 0.04045 else ((value + 0.055) / 1.055) ** 2.4


def check_probe(probe: dict, resolution: list, distance: float, color: list, normal: list,
                material: list) -> None:
    x, y = probe["pixel"]
    width, height = resolution
    if not (0 <= x < width and 0 <= y < height):
        raise RuntimeError(f"out-of-range probe: {probe}")
    depth = (0.1 * 100 / distance - 0.1) / (100 - 0.1)
    tan = math.tan(math.pi / 8)
    view = [(2 * (x + 0.5) / width - 1) * distance * tan * width / height,
            (1 - 2 * (y + 0.5) / height) * distance * tan, -distance]
    label = f"{probe['name']} at ({x}, {y})"
    check_values(f"{label} depth", [probe["depth"]], [depth], 1e-5)
    check_values(f"{label} position", probe["view_position"], view, 0.005)
    check_values(f"{label} base color", probe["base_color"], color, 2 / 255)
    check_values(f"{label} normal", probe["view_normal"], normal, 0.004)
    check_values(f"{label} material", probe["metallic_roughness"], material, 2 / 255)


def run(executable: str, output_root: Path, configuration: str) -> None:
    output_root.mkdir(parents=True, exist_ok=True)
    with TemporaryDirectory(prefix="gbuffer-", dir=output_root) as directory:
        output = Path(directory)
        scene, assets = make_scene(output)
        evidence = output / "probes.json"
        logs = output / "logs"
        subprocess.run([executable, "--hidden", "--frames", "4", "--output", str(logs),
                        "--static-scene", str(scene), "--asset-root", str(assets),
                        "--gbuffer-probes", str(evidence)], check=True, timeout=40)
        report = json.loads(evidence.read_text(encoding="utf-8"))
        probes = {probe["name"]: probe for probe in report["probes"]}
        if set(probes) != {"overlap", "far-only", "background"}:
            raise RuntimeError(f"unexpected probe set: {list(probes)}")
        size = report["extent"]
        check_probe(probes["overlap"], size, 4.5,
                    [srgb(128) * 0.5, srgb(64) * 0.6, srgb(192) * 0.7, 1],
                    [0, 1 / math.sqrt(5), 2 / math.sqrt(5)], [192 / 255 * 0.4, 64 / 255 * 0.7])
        check_probe(probes["far-only"], size, 5.5,
                    [srgb(32), srgb(160), srgb(80), 1], [0, 0, 1], [1, 1])
        if probes["background"]["depth"] != 0 or probes["background"]["base_color"] != [0, 0, 0, 0]:
            raise RuntimeError(f"background clear values were not preserved: {probes['background']}")
        run_dir = next(logs.iterdir())
        summary = json.loads((run_dir / "summary.json").read_text(encoding="utf-8"))
        if (summary["result"] != "success" or summary["configuration"] != configuration
                or not summary["resources_released"] or not summary["diagnostics_complete"]):
            raise RuntimeError(f"incomplete GBuffer run: {summary}")
        ids = {json.loads(line)["event_id"]
               for line in (run_dir / "events.jsonl").read_text(encoding="utf-8").splitlines()}
        if "rhi.validation.error" in ids or (
            configuration == "Debug" and "rhi.validation.enabled" not in ids
        ):
            raise RuntimeError("Vulkan Validation was missing or reported GBuffer errors")


if __name__ == "__main__":
    run(sys.argv[1], Path(sys.argv[2]), sys.argv[3])
