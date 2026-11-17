import json
from pathlib import Path
import subprocess
import sys
from tempfile import TemporaryDirectory

from check_hdr_lighting import neutral_scene


def run(executable: str, output_root: Path, configuration: str) -> None:
    output_root.mkdir(parents=True, exist_ok=True)
    with TemporaryDirectory(prefix="ibl-", dir=output_root) as directory:
        output = Path(directory)
        scene, assets = neutral_scene(output)
        objects = json.loads(scene.read_text(encoding="utf-8"))["objects"]
        far = assets / objects[1]["asset"]
        model = json.loads(far.read_text(encoding="utf-8"))
        material = model["materials"][0]["pbrMetallicRoughness"]
        material["metallicFactor"] = 1

        def render(name, roughness, preset=None, rollback=False):
            material["roughnessFactor"] = roughness
            far.write_text(json.dumps(model), encoding="utf-8")
            evidence = output / f"{name}.json"
            logs = output / f"{name}-logs"
            args = [executable, "--hidden", "--frames", "4", "--static-scene", str(scene),
                    "--asset-root", str(assets), "--output", str(logs),
                    "--gbuffer-probes", str(evidence),
                    *(["--ibl", preset] if preset else ["--hdr-lighting"])]
            if rollback:
                args.append("--verify-ibl-rollback")
            subprocess.run(args, check=True, timeout=45)
            run_dir = next(logs.iterdir())
            events = [json.loads(line) for line in
                      (run_dir / "events.jsonl").read_text(encoding="utf-8").splitlines()]
            ids = [event["event_id"] for event in events]
            summary = json.loads((run_dir / "summary.json").read_text(encoding="utf-8"))
            if (summary["result"] != "success" or not summary["resources_released"]
                    or not summary["diagnostics_complete"] or summary["configuration"] != configuration):
                raise RuntimeError(f"{name}: incomplete run {summary}")
            if "rhi.validation.error" in ids or (
                    configuration == "Debug" and "rhi.validation.enabled" not in ids):
                raise RuntimeError(f"{name}: missing Validation or a Vulkan Validation error")
            if preset and ids.count("rhi.environment.ready") != 1:
                raise RuntimeError(f"{name}: expected exactly one published environment")
            if rollback and (ids.count("rhi.environment.rollback") != 1 or
                             ids.index("rhi.environment.ready") > ids.index("rhi.environment.rollback")):
                raise RuntimeError(f"{name}: expected rollback after environment publication")
            report = json.loads(evidence.read_text(encoding="utf-8"))
            result = {probe["name"]: probe["hdr_color"] for probe in report["probes"]}
            if result["background"] != [0, 0, 0]:
                raise RuntimeError(f"{name}: background received ambient light")
            print(f"{name}: {result}")
            return result

        def contribution(lit, direct, name):
            return [a - b for a, b in zip(lit[name], direct[name])]

        smooth_direct = render("smooth-direct", 0.08)
        smooth = render("smooth-directional", 0.08, "directional")
        smooth_rollback = render("smooth-rollback", 0.08, "directional", rollback=True)
        if smooth != smooth_rollback:
            raise RuntimeError("failed candidate changed the active environment")
        constant = render("constant", 0.08, "constant")
        normalized = [observed / light for observed, light in
                      zip(contribution(constant, smooth_direct, "overlap"), (0.25, 0.5, 0.75))]
        if (not all(0.16 < value < 0.23 for value in normalized)
                or max(normalized) - min(normalized) > 0.004):
            raise RuntimeError(f"constant environment energy mismatch: {normalized}")

        rough_direct = render("rough-direct", 1.0)
        rough = render("rough-directional", 1.0, "directional")
        smooth_rgb = contribution(smooth, smooth_direct, "far-only")
        rough_rgb = contribution(rough, rough_direct, "far-only")
        if not (smooth_rgb[0] > 0.05 and rough_rgb[0] > 0.02 and
                smooth_rgb[2] / smooth_rgb[0] > rough_rgb[2] / rough_rgb[0] + 0.2 and
                rough_rgb[2] > rough_rgb[0]):
            raise RuntimeError(f"highlights did not resolve the expected +Z face and roughness LOD: "
                               f"smooth {smooth_rgb}, rough {rough_rgb}")


if __name__ == "__main__":
    run(sys.argv[1], Path(sys.argv[2]), sys.argv[3])
