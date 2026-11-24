import json
from pathlib import Path
import subprocess
import sys
from tempfile import TemporaryDirectory

from check_gbuffer import check_values, make_scene
from check_static_scene_gpu_driven import cpu_visible_quads

API_1_4 = (1 << 22) | (4 << 12)


def supports_descriptor_heap(summary: dict) -> bool:
    cap = summary.get("capability") or {}
    loader_api = cap.get("loader_api") or 0
    device_api = cap.get("device_api") or 0
    heap_status = (cap.get("descriptor_heap") or {}).get("status")
    return loader_api >= API_1_4 and device_api >= API_1_4 and heap_status == "supported"


def check_run_diagnostics(
    logs_root: Path,
    configuration: str,
    *,
    expect_heap: bool,
    expect_rollback: bool = False,
) -> dict:
    run_dir = next(logs_root.iterdir())
    summary = json.loads((run_dir / "summary.json").read_text(encoding="utf-8"))
    if (
        summary["result"] != "success"
        or summary["configuration"] != configuration
        or not summary["resources_released"]
        or not summary["diagnostics_complete"]
    ):
        raise RuntimeError(f"incomplete run summary in {logs_root.name}: {summary}")

    events = [
        json.loads(line)["event_id"]
        for line in (run_dir / "events.jsonl").read_text(encoding="utf-8").splitlines()
    ]
    event_set = set(events)
    if "rhi.validation.error" in event_set:
        raise RuntimeError(f"Vulkan validation reported errors in {logs_root.name}")
    if configuration == "Debug" and "rhi.validation.enabled" not in event_set:
        raise RuntimeError(f"Vulkan validation was not enabled in Debug for {logs_root.name}")
    if expect_heap:
        if "rhi.device.descriptor-heap.enabled" not in event_set or "rhi.binding.heap.ready" not in event_set:
            raise RuntimeError(f"Descriptor Heap path was not activated in {logs_root.name}")
        if "rhi.binding.set.ready" in event_set:
            raise RuntimeError(f"Descriptor Heap run unexpectedly initialized Descriptor Set in {logs_root.name}")
    else:
        if "rhi.binding.set.ready" not in event_set:
            raise RuntimeError(f"Descriptor Set path was not initialized in {logs_root.name}")
        if "rhi.binding.heap.ready" in event_set:
            raise RuntimeError(f"Descriptor Set run unexpectedly initialized Descriptor Heap in {logs_root.name}")
    if events.count("rhi.environment.ready") != 1:
        raise RuntimeError(f"expected exactly one published environment in {logs_root.name}")
    if expect_rollback and events.count("rhi.environment.rollback") != 1:
        raise RuntimeError(f"expected one environment rollback event in {logs_root.name}")
    return summary


def compare_outputs(set_probes: dict, heap_probes: dict, set_png: bytes, heap_png: bytes) -> None:
    if heap_probes["extent"] != set_probes["extent"]:
        raise RuntimeError(f"extent mismatch: set={set_probes['extent']} heap={heap_probes['extent']}")
    if heap_probes.get("culling") != set_probes.get("culling"):
        raise RuntimeError(f"culling mismatch: set={set_probes.get('culling')} heap={heap_probes.get('culling')}")
    if heap_png != set_png:
        raise RuntimeError("Descriptor Heap final PNG differs from Descriptor Set baseline")
    for expected, actual in zip(set_probes["probes"], heap_probes["probes"], strict=True):
        if actual["name"] != expected["name"] or actual["pixel"] != expected["pixel"]:
            raise RuntimeError(f"probe metadata mismatch: set={expected} heap={actual}")
        label = actual["name"]
        check_values(f"{label} depth", [actual["depth"]], [expected["depth"]], 1e-5)
        check_values(f"{label} base_color", actual["base_color"], expected["base_color"], 2 / 255)
        check_values(f"{label} view_normal", actual["view_normal"], expected["view_normal"], 1e-4)
        check_values(
            f"{label} metallic_roughness",
            actual["metallic_roughness"],
            expected["metallic_roughness"],
            2 / 255,
        )
        check_values(f"{label} view_position", actual["view_position"], expected["view_position"], 1e-3)
        check_values(f"{label} hdr_color", actual["hdr_color"], expected["hdr_color"], 2e-3)
        check_values(f"{label} sdr_rgb", actual["sdr_rgb"], expected["sdr_rgb"], 0)


def run(executable: str, root: Path, configuration: str) -> None:
    root.mkdir(parents=True, exist_ok=True)
    with TemporaryDirectory(prefix="descriptor-heap-scene-", dir=root) as temporary:
        output = Path(temporary)
        scene, assets = make_scene(output)
        source = json.loads(scene.read_text(encoding="utf-8"))
        # Append an off-frustum instance so GPU culling visibly filters non-visible geometry.
        source["objects"].append({**source["objects"][0], "id": 300, "position": [100, 0, 0]})
        scene.write_text(json.dumps(source), encoding="utf-8")

        # 1. Verify conflicting or invalid CLI combinations with --descriptor-heap are rejected.
        invalid_cases = [
            ["--hidden", "--frames", "2", "--static-scene", str(scene), "--asset-root", str(assets),
             "--descriptor-heap", "--set-binding"],
            ["--hidden", "--frames", "3", "--descriptor-heap", "--set-binding-retirement"],
            ["--hidden", "--frames", "2", "--descriptor-heap", "--clear-only"],
            ["--hidden", "--frames", "2", "--descriptor-heap", "--gpu-driven"],
            ["--hidden", "--frames", "2", "--descriptor-heap"],
        ]
        for args in invalid_cases:
            rejected = subprocess.run(
                [executable, *args], capture_output=True, text=True, timeout=30
            )
            if rejected.returncode != 2:
                raise RuntimeError(
                    f"expected exit code 2 for invalid CLI args {args}, got {rejected.returncode}"
                )

        # 2. Run 1.3 Descriptor Set baseline (GPU-Driven + Deferred HDR + Directional IBL).
        set_probes_path = output / "set.json"
        set_png_path = output / "set.png"
        set_logs = output / "logs-set"
        common_scene_args = [
            "--hidden", "--frames", "4",
            "--static-scene", str(scene),
            "--asset-root", str(assets),
            "--static-gpu-driven",
            "--hdr-lighting",
            "--ibl", "directional",
        ]
        subprocess.run(
            [executable, *common_scene_args,
             "--output", str(set_logs),
             "--gbuffer-probes", str(set_probes_path),
             "--screenshot", str(set_png_path)],
            check=True,
            timeout=45,
        )
        set_summary = check_run_diagnostics(set_logs, configuration, expect_heap=False)
        set_probes = json.loads(set_probes_path.read_text(encoding="utf-8"))
        set_png = set_png_path.read_bytes()

        expected_ids = cpu_visible_quads(source, set_probes["extent"])
        if expected_ids != [1, 2] or set_probes.get("culling") != {
            "visible": len(expected_ids),
            "drawn_ids": expected_ids,
        }:
            raise RuntimeError(f"Set GPU visibility differs from CPU reference: {set_probes}")

        # 3. Run 1.4 Descriptor Heap target on the exact same scene and flags.
        heap_probes_path = output / "heap.json"
        heap_png_path = output / "heap.png"
        heap_logs = output / "logs-heap"
        heap_proc = subprocess.run(
            [executable, *common_scene_args,
             "--descriptor-heap",
             "--output", str(heap_logs),
             "--gbuffer-probes", str(heap_probes_path),
             "--screenshot", str(heap_png_path)],
            capture_output=True,
            text=True,
            timeout=45,
        )

        if not supports_descriptor_heap(set_summary):
            if heap_proc.returncode == 0:
                raise RuntimeError("--descriptor-heap unexpectedly succeeded on unsupported hardware")
            run_dir = next(heap_logs.iterdir())
            heap_summary = json.loads((run_dir / "summary.json").read_text(encoding="utf-8"))
            if heap_summary["result"] == "success" or not heap_summary["resources_released"]:
                raise RuntimeError(f"unsupported Heap run left unclean state: {heap_summary}")
            events = {
                json.loads(line)["event_id"]
                for line in (run_dir / "events.jsonl").read_text(encoding="utf-8").splitlines()
            }
            if "rhi.binding.set.ready" in events:
                raise RuntimeError("--descriptor-heap silently fell back to Descriptor Set")
            return

        if heap_proc.returncode != 0:
            raise RuntimeError(
                f"--descriptor-heap failed on capable GPU (exit {heap_proc.returncode}): "
                f"{heap_proc.stderr}"
            )
        check_run_diagnostics(heap_logs, configuration, expect_heap=True)
        heap_probes = json.loads(heap_probes_path.read_text(encoding="utf-8"))
        heap_png = heap_png_path.read_bytes()
        compare_outputs(set_probes, heap_probes, set_png, heap_png)

        # 4. Also verify --descriptor-heap with --verify-ibl-rollback preserves identical output.
        rollback_probes_path = output / "heap-rollback.json"
        rollback_png_path = output / "heap-rollback.png"
        rollback_logs = output / "logs-heap-rollback"
        subprocess.run(
            [executable, *common_scene_args,
             "--descriptor-heap",
             "--verify-ibl-rollback",
             "--output", str(rollback_logs),
             "--gbuffer-probes", str(rollback_probes_path),
             "--screenshot", str(rollback_png_path)],
            check=True,
            timeout=45,
        )
        check_run_diagnostics(rollback_logs, configuration, expect_heap=True, expect_rollback=True)
        rollback_probes = json.loads(rollback_probes_path.read_text(encoding="utf-8"))
        rollback_png = rollback_png_path.read_bytes()
        compare_outputs(heap_probes, rollback_probes, heap_png, rollback_png)


if __name__ == "__main__":
    run(sys.argv[1], Path(sys.argv[2]), sys.argv[3])
