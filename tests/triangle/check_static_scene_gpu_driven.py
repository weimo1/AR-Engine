import json
import math
from pathlib import Path
import subprocess
import sys
from tempfile import TemporaryDirectory

from check_gbuffer import check_values, make_scene


def cpu_visible_quads(scene: dict, extent: list[int]) -> list[int]:
    # The fixture uses the XY quad, an axis-aligned camera, and no object rotation.
    camera = scene["camera"]
    cx, cy, cz = camera["position"]
    width, height = extent
    result = []
    for instance in scene["objects"]:
        x, y, z = instance["position"]
        half_y = (cz - z) * math.tan(camera["fov_y"] / 2)
        half_x = half_y * width / height
        sx, sy, _ = instance["scale"]
        if (camera["near"] <= cz - z <= camera["far"] and
                abs(x - cx) <= half_x + sx and abs(y - cy) <= half_y + sy):
            result.append(instance["id"])
    return sorted(result)


def run(executable: str, root: Path, configuration: str) -> None:
    root.mkdir(parents=True, exist_ok=True)
    with TemporaryDirectory(prefix="static-indirect-", dir=root) as temporary:
        output = Path(temporary)
        scene, assets = make_scene(output)
        source = json.loads(scene.read_text(encoding="utf-8"))
        source["objects"].append({**source["objects"][0], "id": 300, "position": [100, 0, 0]})
        scene.write_text(json.dumps(source), encoding="utf-8")

        def render(name: str, scene_path: Path, *options: str) -> tuple[dict, bytes]:
            probes = output / f"{name}.json"
            image = output / f"{name}.png"
            logs = output / name
            subprocess.run([executable, "--hidden", "--frames", "5", "--static-scene",
                            str(scene_path), "--asset-root", str(assets), "--bindless-materials",
                            "--hdr-lighting", "--output", str(logs), "--gbuffer-probes",
                            str(probes), "--screenshot", str(image), *options],
                           check=True, timeout=45)
            events = next(logs.iterdir()) / "events.jsonl"
            ids = {json.loads(line)["event_id"]
                   for line in events.read_text(encoding="utf-8").splitlines()}
            if "rhi.validation.error" in ids or (
                configuration == "Debug" and "rhi.validation.enabled" not in ids
            ):
                raise RuntimeError(f"incomplete Vulkan validation in {name}")
            return json.loads(probes.read_text(encoding="utf-8")), image.read_bytes()

        direct, direct_image = render("cpu", scene)
        indirect, indirect_image = render("gpu", scene, "--static-gpu-driven")
        expected_ids = cpu_visible_quads(source, direct["extent"])
        if expected_ids != [1, 2] or indirect.get("culling") != {
            "visible": len(expected_ids), "drawn_ids": expected_ids
        }:
            raise RuntimeError(f"GPU visibility differs from CPU scene reference: {indirect}")
        if indirect_image != direct_image:
            raise RuntimeError("GPU indirect final image differs from CPU direct rendering")
        for expected, actual in zip(direct["probes"], indirect["probes"], strict=True):
            for key in ("base_color", "view_normal", "metallic_roughness", "view_position",
                        "hdr_color", "sdr_rgb"):
                check_values(f"{actual['name']} {key}", actual[key], expected[key], 1e-5)
            check_values("depth", [actual["depth"]], [expected["depth"]], 1e-5)

        limited, _ = render("limited", scene, "--static-gpu-driven", "--indirect-capacity", "1")
        result = limited["culling"]
        if (result["visible"] != len(expected_ids) or len(result["drawn_ids"]) != 1 or
                result["drawn_ids"][0] not in expected_ids):
            raise RuntimeError(f"invalid capacity result: {result}")
        empty_scene = output / "culled.json"
        source["objects"] = [source["objects"][-1]]
        empty_scene.write_text(json.dumps(source), encoding="utf-8")
        culled, _ = render("culled", empty_scene, "--static-gpu-driven")
        if cpu_visible_quads(source, culled["extent"]) or culled["culling"] != {
            "visible": 0, "drawn_ids": []
        }:
            raise RuntimeError(f"unexpected zero-visible result: {culled['culling']}")
        if any(probe["depth"] or any(probe["sdr_rgb"]) for probe in culled["probes"]):
            raise RuntimeError("zero-visible scene wrote depth or color")


if __name__ == "__main__":
    run(sys.argv[1], Path(sys.argv[2]), sys.argv[3])
