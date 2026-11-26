"""Four-configuration performance benchmark orchestrator and CTest export verifier.

Generates deterministic showroom-material grid scenes (Low / Medium / High or compact test scale),
executes all 4 configurations (Set/Heap x CPU/GPU frustum culling) via `ar_engine --benchmark`,
verifies identical inputs and zero Vulkan validation errors, independently recomputes median & P95
from `frames.csv`, and emits a consolidated comparison report.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
import subprocess
import sys
from pathlib import Path
from tempfile import TemporaryDirectory

FOUR_CONFIGS: list[tuple[str, str, str]] = [
    ("set-cpu", "set", "cpu"),
    ("set-gpu", "set", "gpu"),
    ("heap-cpu", "descriptor_heap", "cpu"),
    ("heap-gpu", "descriptor_heap", "gpu"),
]

SHOWROOM_SPHERE_ASSETS: list[str] = [
    "roughness_0.gltf",
    "roughness_1.gltf",
    "roughness_2.gltf",
    "roughness_3.gltf",
    "roughness_4.gltf",
    "metallic_0.gltf",
    "metallic_1.gltf",
    "metallic_2.gltf",
    "metallic_3.gltf",
    "metallic_4.gltf",
]


def generate_grid_scene(
    output_path: Path,
    grid_x: int,
    grid_z: int,
    seed: int = 20261125,
) -> int:
    """Create a deterministic showroom-material grid scene (~40-60% frustum-culled during sweep)."""
    spacing = 1.65
    half_x = (grid_x - 1) * spacing * 0.5
    half_z = (grid_z - 1) * spacing * 0.5
    extent = max(half_x, half_z, 4.0)

    objects: list[dict[str, object]] = []
    # Large ground plane anchored beneath the grid (id=1).
    objects.append(
        {
            "id": 1,
            "asset": "floor.gltf",
            "position": [0.0, -0.1, 0.0],
            "rotation": [0.0, 0.0, 0.0, 1.0],
            "scale": [extent * 2.4, 0.2, extent * 2.4],
        }
    )

    next_id = 2
    target_objects = grid_x * grid_z
    for iz in range(grid_z):
        for ix in range(grid_x):
            if len(objects) >= target_objects:
                break
            x = ix * spacing - half_x
            z = iz * spacing - half_z
            # Deterministic LCG hash for material & slight height/scale variation.
            h = ((ix * 73856093) ^ (iz * 19349663) ^ (seed * 83492791)) & 0xFFFFFFFF
            asset = SHOWROOM_SPHERE_ASSETS[h % len(SHOWROOM_SPHERE_ASSETS)]
            y_scale = 0.62 + 0.14 * ((h >> 8) % 4)
            objects.append(
                {
                    "id": next_id,
                    "asset": asset,
                    "position": [round(x, 4), round(y_scale * 0.5, 4), round(z, 4)],
                    "rotation": [0.0, 0.0, 0.0, 1.0],
                    "scale": [round(y_scale, 4), round(y_scale, 4), round(y_scale, 4)],
                }
            )
            next_id += 1

    # Position camera inside the front edge of the grid looking across the center so roughly half
    # the field is outside the horizontal frustum at every point of the yaw sweep.
    cam_y = max(3.5, min(14.0, extent * 0.28))
    cam_z = half_z * 0.55 + 4.0
    far_plane = max(80.0, extent * 4.0)
    scene = {
        "version": 1,
        "camera": {
            "position": [0.0, round(cam_y, 3), round(cam_z, 3)],
            "orientation": [-0.1564345, 0.0, 0.0, 0.9876883],  # pitch -18 deg
            "fov_y": 0.7853982,
            "near": 0.1,
            "far": round(far_plane, 2),
        },
        "directional_light": {
            "direction": [-0.4, -0.85, -0.35],
            "color": [1.0, 0.98, 0.95],
            "intensity": 6.0,
            "cast_shadows": True,
        },
        "objects": objects,
    }
    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text(json.dumps(scene, separators=(",", ":")), encoding="utf-8")
    return len(objects)


def linear_quantile(values: list[float], q: float) -> float:
    s = sorted(values)
    if len(s) == 1:
        return s[0]
    pos = q * (len(s) - 1)
    lo = math.floor(pos)
    hi = math.ceil(pos)
    frac = pos - lo
    return s[lo] * (1.0 - frac) + s[hi] * frac


def verify_and_recompute_run(
    run_dir: Path,
    expected_binding: str,
    expected_cull: str,
    expected_instances: int,
    expected_samples: int,
) -> dict[str, object]:
    summary_path = run_dir / "summary.json"
    csv_path = run_dir / "frames.csv"
    if not summary_path.is_file() or not csv_path.is_file():
        raise RuntimeError(f"missing benchmark outputs in {run_dir}")

    summary = json.loads(summary_path.read_text(encoding="utf-8"))
    if summary.get("version") != 1:
        raise RuntimeError(f"unexpected summary version: {summary.get('version')}")
    if summary.get("binding") != expected_binding or summary.get("cull") != expected_cull:
        raise RuntimeError(
            f"config mismatch in {summary_path}: expected {expected_binding}/{expected_cull}, "
            f"got {summary.get('binding')}/{summary.get('cull')}"
        )

    status = summary.get("status")
    if status == "skipped_unsupported":
        if expected_binding != "descriptor_heap":
            raise RuntimeError(f"unexpected skipped_unsupported on non-heap run: {run_dir}")
        if not summary.get("skip_reason"):
            raise RuntimeError(f"skipped_unsupported missing skip_reason: {run_dir}")
        return summary
    if status != "completed":
        raise RuntimeError(f"unexpected benchmark status in {summary_path}: {status}")

    if summary.get("instance_count") != expected_instances:
        raise RuntimeError(
            f"instance_count mismatch: expected {expected_instances}, got {summary.get('instance_count')}"
        )
    if summary.get("total_recorded_samples") != expected_samples:
        raise RuntimeError(
            f"sample count mismatch: expected {expected_samples}, got {summary.get('total_recorded_samples')}"
        )
    if summary.get("overflow_sample_count") != 0:
        raise RuntimeError(f"unexpected overflow samples in formal run: {summary.get('overflow_sample_count')}")

    # Verify zero Vulkan validation errors in the benchmark diagnostic log.
    diag_dir = run_dir / "diagnostics"
    bench_logs = sorted(diag_dir.glob("bench-*/events.jsonl"))
    if not bench_logs:
        raise RuntimeError(f"missing benchmark events log under: {diag_dir}")
    events_path = bench_logs[-1]
    for line in events_path.read_text(encoding="utf-8").splitlines():
        entry = json.loads(line)
        if entry.get("event_id") == "rhi.validation.error":
            raise RuntimeError(f"Vulkan validation error in {run_dir}: {entry.get('message')}")

    # Independently parse frames.csv and recompute median & P95 for every metric.
    with csv_path.open("r", encoding="utf-8", newline="") as handle:
        rows = list(csv.DictReader(handle))
    if len(rows) != expected_samples:
        raise RuntimeError(f"CSV row count {len(rows)} != expected {expected_samples}")

    frame_numbers = [int(r["frame_number"]) for r in rows]
    if len(set(frame_numbers)) != len(frame_numbers) or frame_numbers != sorted(frame_numbers):
        raise RuntimeError(f"non-monotonic or duplicate frame_numbers in {csv_path}")

    metrics_to_check = [
        "cpu_cull_ms",
        "cpu_record_submit_ms",
        "cpu_total_ms",
        "gpu_cull_ms",
        "gpu_shadow_ms",
        "gpu_geometry_ms",
        "gpu_lighting_ms",
        "gpu_display_ms",
        "gpu_total_ms",
    ]
    summary_metrics = summary["metrics"]
    for key in metrics_to_check:
        vals: list[float] = []
        for r in rows:
            if int(r["overflowed"]) != 0:
                continue
            raw = r[key].strip()
            if key.startswith("gpu_") and int(r["gpu_available"]) == 0:
                continue
            if raw != "":
                vals.append(float(raw))
        reported = summary_metrics[key]
        if reported["sample_count"] != len(vals):
            raise RuntimeError(
                f"metric {key} sample_count mismatch: summary={reported['sample_count']} csv={len(vals)}"
            )
        if vals:
            recomp_med = linear_quantile(vals, 0.50)
            recomp_p95 = linear_quantile(vals, 0.95)
            if abs(recomp_med - reported["median_ms"]) > 1e-4:
                raise RuntimeError(
                    f"metric {key} median mismatch: summary={reported['median_ms']} csv={recomp_med}"
                )
            if abs(recomp_p95 - reported["p95_ms"]) > 1e-4:
                raise RuntimeError(
                    f"metric {key} p95 mismatch: summary={reported['p95_ms']} csv={recomp_p95}"
                )

    # Check cull-path-specific metric presence.
    if expected_cull == "cpu":
        if summary_metrics["cpu_cull_ms"]["sample_count"] != expected_samples:
            raise RuntimeError("CPU cull run missing cpu_cull_ms samples")
        if summary_metrics["gpu_cull_ms"]["sample_count"] != 0:
            raise RuntimeError("CPU cull run should not record gpu_cull_ms")
    else:
        if summary_metrics["cpu_cull_ms"]["sample_count"] != 0:
            raise RuntimeError("GPU cull run should not record cpu_cull_ms")

    return summary


def run_four_configs(
    exe: Path,
    asset_root: Path,
    scene_path: Path,
    instance_count: int,
    scale_name: str,
    out_root: Path,
    width: int,
    height: int,
    warmup: int,
    samples: int,
    repeats: int,
    present_mode: str,
    seed: int = 20261125,
) -> list[dict[str, object]]:
    summaries: list[dict[str, object]] = []
    for label, binding, cull in FOUR_CONFIGS:
        run_dir = out_root / f"{scale_name}-{label}"
        run_dir.mkdir(parents=True, exist_ok=True)
        spec_path = run_dir / "spec.json"
        spec = {
            "version": 1,
            "name": f"{scale_name}-{label}",
            "preset_version": "p6-bench-v1",
            "seed": seed,
            "scene": str(scene_path.resolve()),
            "asset_root": str(asset_root.resolve()),
            "binding": binding,
            "cull": cull,
            "present_mode": present_mode,
            "width": width,
            "height": height,
            "shadows": True,
            "shadow_resolution": 2048,
            "exposure_ev": 0.0,
            "warmup_frames": warmup,
            "sample_frames": samples,
            "repeats": repeats,
            "hidden": True,
            "output_dir": str(run_dir.resolve()),
        }
        spec_path.write_text(json.dumps(spec, indent=2), encoding="utf-8")
        proc = subprocess.run(
            [str(exe), "--benchmark", str(spec_path)],
            capture_output=True,
            text=True,
            timeout=300,
        )
        if proc.returncode != 0:
            raise RuntimeError(
                f"benchmark {scale_name}-{label} failed (exit {proc.returncode}):\n"
                f"stdout:\n{proc.stdout}\nstderr:\n{proc.stderr}"
            )
        summary = verify_and_recompute_run(
            run_dir, binding, cull, instance_count, samples * repeats
        )
        summaries.append(summary)

    # Cross-config input & visibility consistency check across all completed configs!
    completed = [s for s in summaries if s.get("status") == "completed"]
    if len(completed) < 2:
        raise RuntimeError("expected at least Set+CPU and Set+GPU to complete")
    ref_vis = completed[0]["visibility"]
    for s in completed[1:]:
        if s["visibility"] != ref_vis:
            raise RuntimeError(
                f"visibility mismatch between {completed[0]['name']} ({ref_vis}) "
                f"and {s['name']} ({s['visibility']})"
            )
    return summaries


def format_markdown_table(all_results: dict[str, list[dict[str, object]]]) -> str:
    def fmt(metric: dict[str, object], field: str) -> str:
        val = metric.get(field)
        return "-" if val is None else f"{val:.3f}"

    lines = [
        "# AR Engine P6 四配置性能基准汇总",
        "",
        "| 规模 (实例数) | 配置 | 状态 | Present | 可见数 (min/med/max) | CPU Cull med/p95 (ms) | CPU Submit med/p95 (ms) | GPU Cull med/p95 (ms) | GPU Geom med/p95 (ms) | GPU Total med/p95 (ms) |",
        "| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |",
    ]
    for scale, runs in all_results.items():
        for s in runs:
            m = s["metrics"]
            v = s["visibility"]
            vis_str = (
                "-"
                if v["min_visible"] is None
                else f"{int(v['min_visible'])}/{int(v['median_visible'])}/{int(v['max_visible'])}"
            )
            lines.append(
                f"| {scale} ({s['instance_count']}) | {s['binding']}+{s['cull']} | {s['status']} "
                f"| {s['active_present_mode']} | {vis_str} "
                f"| {fmt(m['cpu_cull_ms'], 'median_ms')}/{fmt(m['cpu_cull_ms'], 'p95_ms')} "
                f"| {fmt(m['cpu_record_submit_ms'], 'median_ms')}/{fmt(m['cpu_record_submit_ms'], 'p95_ms')} "
                f"| {fmt(m['gpu_cull_ms'], 'median_ms')}/{fmt(m['gpu_cull_ms'], 'p95_ms')} "
                f"| {fmt(m['gpu_geometry_ms'], 'median_ms')}/{fmt(m['gpu_geometry_ms'], 'p95_ms')} "
                f"| {fmt(m['gpu_total_ms'], 'median_ms')}/{fmt(m['gpu_total_ms'], 'p95_ms')} |"
            )
    lines.append("")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description="AR Engine 4-config benchmark runner and verifier")
    parser.add_argument("executable", type=Path, help="Path to ar_engine.exe")
    parser.add_argument("output_root", type=Path, help="Output root directory")
    parser.add_argument(
        "--suite",
        action="store_true",
        help="Run full graduation benchmark suite: Low (256), Medium (4096), High (16384)",
    )
    parser.add_argument("--width", type=int, default=1280)
    parser.add_argument("--height", type=int, default=720)
    parser.add_argument("--warmup", type=int, default=20)
    parser.add_argument("--samples", type=int, default=120)
    parser.add_argument("--repeats", type=int, default=2)
    parser.add_argument("--present-mode", default="immediate")
    args = parser.parse_args()

    exe = args.executable.resolve()
    asset_root = (exe.parent / "content" / "showroom" / "assets").resolve()
    if not asset_root.is_dir():
        raise RuntimeError(f"missing deployed showroom assets directory: {asset_root}")

    args.output_root.mkdir(parents=True, exist_ok=True)

    if args.suite:
        scales = [
            ("low-256", 16, 16),
            ("medium-4096", 64, 64),
            ("high-16384", 128, 128),
        ]
        all_results: dict[str, list[dict[str, object]]] = {}
        for scale_name, gx, gz in scales:
            scene_path = args.output_root / "scenes" / f"{scale_name}.json"
            count = generate_grid_scene(scene_path, gx, gz)
            all_results[scale_name] = run_four_configs(
                exe=exe,
                asset_root=asset_root,
                scene_path=scene_path,
                instance_count=count,
                scale_name=scale_name,
                out_root=args.output_root / "runs",
                width=args.width,
                height=args.height,
                warmup=args.warmup,
                samples=args.samples,
                repeats=args.repeats,
                present_mode=args.present_mode,
            )
        report_json = args.output_root / "benchmark_suite_summary.json"
        report_md = args.output_root / "benchmark_suite_summary.md"
        report_json.write_text(json.dumps(all_results, indent=2), encoding="utf-8")
        md_text = format_markdown_table(all_results)
        report_md.write_text(md_text, encoding="utf-8")
        print(md_text)
        return 0

    # Fast CTest verification mode: 128-instance deterministic grid, 4 warmup + 12 samples x 1 repeat.
    with TemporaryDirectory(prefix="four-config-bench-", dir=args.output_root) as tmp:
        tmp_dir = Path(tmp)
        scene_path = tmp_dir / "ctest-128.json"
        count = generate_grid_scene(scene_path, 16, 8)
        results = run_four_configs(
            exe=exe,
            asset_root=asset_root,
            scene_path=scene_path,
            instance_count=count,
            scale_name="ctest-128",
            out_root=tmp_dir / "runs",
            width=640,
            height=360,
            warmup=4,
            samples=12,
            repeats=1,
            present_mode="immediate",
        )
        # Verify partial culling actually occurred (not all visible and not all culled).
        vis = results[0]["visibility"]
        if vis["min_visible"] is None or vis["max_visible"] is None:
            raise RuntimeError("missing visibility statistics")
        if vis["min_visible"] >= count or vis["max_visible"] <= 0:
            raise RuntimeError(
                f"expected partial frustum culling during trajectory, got visibility={vis} / {count}"
            )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
