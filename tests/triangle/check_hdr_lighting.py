import json
import math
from pathlib import Path
import subprocess
import sys
from tempfile import TemporaryDirectory

from check_gbuffer import check_values, make_scene


def neutral_scene(output: Path) -> tuple[Path, Path]:
    scene, assets = make_scene(output)
    for obj, gray in zip(json.loads(scene.read_text(encoding="utf-8"))["objects"], (0.18, 0.95)):
        path = assets / obj["asset"]
        model = json.loads(path.read_text(encoding="utf-8"))
        model["materials"] = [{"pbrMetallicRoughness": {
            "baseColorFactor": [gray, gray, gray, 1],
            "metallicFactor": 0,
            "roughnessFactor": 0.65,
        }}]
        model["meshes"][0]["primitives"][0]["material"] = 0
        path.write_text(json.dumps(model), encoding="utf-8")
    return scene, assets


def normalize(vector: list) -> list:
    length = math.sqrt(sum(value * value for value in vector))
    return [value / length for value in vector]


def dot(left: list, right: list) -> float:
    return sum(a * b for a, b in zip(left, right))


def direct_gray(position: list, normal: list, gray: float) -> float:
    light = [0, 0, 1]
    view = normalize([-value for value in position])
    half = normalize([v + l for v, l in zip(view, light)])
    nl, nv, nh, vh = (max(0, dot(a, b)) for a, b in
                      ((normal, light), (normal, view), (normal, half), (view, half)))
    alpha_squared = (round(0.65 * 255) / 255) ** 4
    distribution = alpha_squared / (math.pi * (nh * nh * (alpha_squared - 1) + 1) ** 2)
    masking = [2 * x / (x + math.sqrt(alpha_squared + (1 - alpha_squared) * x * x))
               for x in (nl, nv)]
    fresnel = 0.04 + 0.96 * (1 - vh) ** 5
    diffuse = round(gray * 255) / 255 * (1 - fresnel) / math.pi
    specular = distribution * math.prod(masking) * fresnel / max(4 * nl * nv, 1e-5)
    return 6 * nl * (diffuse + specular)


def display_gray(hdr: float, ev: int) -> float:
    if hdr == 0:
        return 0
    slope, toe, shoulder = 0.91, 0.55, 0.26
    toe_scale, shoulder_scale = 1 - toe, 1.04 - shoulder
    toe_match = math.log10(0.18) - math.atanh(0.18 / toe_scale - 1) * toe_scale / slope
    straight_match = (1 - toe) / slope - toe_match
    shoulder_match = shoulder / slope - straight_match
    log_color = math.log10(hdr * 2 ** ev)
    linear = slope * (log_color + straight_match)
    low = (2 * toe_scale / (1 + math.exp(-2 * slope / toe_scale * (log_color - toe_match)))
           if log_color < toe_match else linear)
    high = (1.04 - 2 * shoulder_scale /
            (1 + math.exp(2 * slope / shoulder_scale * (log_color - shoulder_match)))
            if log_color > shoulder_match else linear)
    t = min(1, max(0, (log_color - min(toe_match, shoulder_match)) /
                   abs(shoulder_match - toe_match)))
    tone = min(1, max(0, low + (high - low) * t * t * (3 - 2 * t)))
    return 255 * (12.92 * tone if tone <= 0.0031308 else 1.055 * tone ** (1 / 2.4) - 0.055)


def run(executable: str, output_root: Path, configuration: str) -> None:
    output_root.mkdir(parents=True, exist_ok=True)
    with TemporaryDirectory(prefix="hdr-", dir=output_root) as directory:
        output = Path(directory)
        scene, assets = neutral_scene(output)
        results = []
        for ev in (0, -1):
            evidence = output / f"probes-{ev}.json"
            logs = output / f"logs-{ev}"
            subprocess.run([executable, "--hidden", "--frames", "4", "--output", str(logs),
                            "--static-scene", str(scene), "--asset-root", str(assets),
                            "--hdr-lighting", "--exposure-ev", str(ev),
                            "--gbuffer-probes", str(evidence)], check=True, timeout=40)
            report = json.loads(evidence.read_text(encoding="utf-8"))
            probes = {probe["name"]: probe for probe in report["probes"]}
            if set(probes) != {"overlap", "far-only", "background"}:
                raise RuntimeError(f"unexpected lighting probes: {list(probes)}")

            width, height = report["extent"]
            for name, gray, distance, normal in (("overlap", 0.18, 4.5, normalize([0, 1, 2])),
                                                  ("far-only", 0.95, 5.5, [0, 0, 1])):
                probe = probes[name]
                x, y = probe["pixel"]
                scale = distance * math.tan(math.pi / 8)
                position = [(2 * (x + 0.5) - width) / height * scale,
                            (1 - 2 * (y + 0.5) / height) * scale, -distance]
                hdr = direct_gray(position, normal, gray)
                check_values(f"{name} EV {ev} HDR", probe["hdr_color"], [hdr] * 3, 0.012)
                check_values(f"{name} EV {ev} SDR", probe["sdr_rgb"], [display_gray(hdr, ev)] * 3, 3)
            check_values("background HDR", probes["background"]["hdr_color"], [0] * 3, 0)
            check_values("background SDR", probes["background"]["sdr_rgb"], [0] * 3, 0)
            if min(probes["far-only"]["hdr_color"]) <= 1:
                raise RuntimeError("highlight was clipped before display mapping")
            for name, probe in probes.items():
                print(f"{name} EV={ev}: HDR={probe['hdr_color']} SDR={probe['sdr_rgb']}")
            results.append(probes)

            run_dir = next(logs.iterdir())
            summary = json.loads((run_dir / "summary.json").read_text(encoding="utf-8"))
            if (summary["result"] != "success" or summary["configuration"] != configuration
                    or not summary["resources_released"] or not summary["diagnostics_complete"]):
                raise RuntimeError(f"incomplete lighting run: {summary}")
            events = {json.loads(line)["event_id"]
                      for line in (run_dir / "events.jsonl").read_text(encoding="utf-8").splitlines()}
            if ("rhi.validation.error" in events
                    or configuration == "Debug" and "rhi.validation.enabled" not in events):
                raise RuntimeError("Vulkan Validation was missing or reported lighting errors")

        for name in ("overlap", "far-only"):
            first, second = (result[name] for result in results)
            if first["hdr_color"] != second["hdr_color"]:
                raise RuntimeError(f"{name}: exposure changed linear HDR")
            if any(a <= b for a, b in zip(first["sdr_rgb"], second["sdr_rgb"])):
                raise RuntimeError(f"{name}: displayed color did not darken at EV -1")


if __name__ == "__main__":
    run(sys.argv[1], Path(sys.argv[2]), sys.argv[3])
