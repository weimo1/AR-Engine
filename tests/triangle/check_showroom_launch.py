"""End-to-end GPU verification for ar_engine zero-mode-flag showroom launch & parity."""

from __future__ import annotations

import json
import shutil
import struct
import subprocess
import sys
import zlib
from pathlib import Path
from tempfile import TemporaryDirectory

from check_descriptor_heap_scene import (
    check_run_diagnostics,
    compare_outputs,
    supports_descriptor_heap,
)
from check_gbuffer import make_scene

EXPECTED_SHOWROOM_IDS = [1, 2, 3, 4, 101, 102, 103, 104, 105, 201, 202, 203, 204, 205]


def _paeth(a: int, b: int, c: int) -> int:
    p = a + b - c
    pa = abs(p - a)
    pb = abs(p - b)
    pc = abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    if pb <= pc:
        return b
    return c


def decode_png_rgba(data: bytes) -> tuple[int, int, bytes]:
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise RuntimeError("invalid PNG signature")
    offset = 8
    width = 0
    height = 0
    idat = bytearray()
    while offset < len(data):
        length, chunk_type = struct.unpack(">I4s", data[offset : offset + 8])
        payload = data[offset + 8 : offset + 8 + length]
        crc_expected = struct.unpack(">I", data[offset + 8 + length : offset + 12 + length])[0]
        if (zlib.crc32(chunk_type + payload) & 0xFFFFFFFF) != crc_expected:
            raise RuntimeError(f"corrupt PNG CRC in chunk {chunk_type!r}")
        if chunk_type == b"IHDR":
            width, height, bit_depth, color_type, comp, flt, interlace = struct.unpack(
                ">IIBBBBB", payload
            )
            if (bit_depth, color_type, comp, flt, interlace) != (8, 6, 0, 0, 0):
                raise RuntimeError(
                    f"unsupported PNG format: bd={bit_depth} ct={color_type} interlace={interlace}"
                )
        elif chunk_type == b"IDAT":
            idat.extend(payload)
        elif chunk_type == b"IEND":
            break
        offset += 12 + length

    if width <= 0 or height <= 0:
        raise RuntimeError("PNG missing valid IHDR")

    stride = width * 4
    raw = zlib.decompress(bytes(idat))
    if len(raw) != height * (1 + stride):
        raise RuntimeError(f"unexpected PNG decompressed size {len(raw)} for {width}x{height}")

    out = bytearray(height * stride)
    prev = bytearray(stride)
    pos = 0
    for y in range(height):
        flt = raw[pos]
        pos += 1
        row = bytearray(raw[pos : pos + stride])
        pos += stride
        if flt == 0:
            pass
        elif flt == 1:
            for i in range(4, stride):
                row[i] = (row[i] + row[i - 4]) & 0xFF
        elif flt == 2:
            for i in range(stride):
                row[i] = (row[i] + prev[i]) & 0xFF
        elif flt == 3:
            for i in range(stride):
                left = row[i - 4] if i >= 4 else 0
                row[i] = (row[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif flt == 4:
            for i in range(stride):
                left = row[i - 4] if i >= 4 else 0
                up = prev[i]
                up_left = prev[i - 4] if i >= 4 else 0
                row[i] = (row[i] + _paeth(left, up, up_left)) & 0xFF
        else:
            raise RuntimeError(f"unsupported PNG filter type {flt}")
        out[y * stride : (y + 1) * stride] = row
        prev = row
    return width, height, bytes(out)


def sample_patch_rgb(
    width: int, height: int, rgba: bytes, u: float, v: float, radius: int = 2
) -> tuple[float, float, float]:
    cx = max(radius, min(width - 1 - radius, int(round(width * u))))
    cy = max(radius, min(height - 1 - radius, int(round(height * v))))
    r_sum = g_sum = b_sum = 0
    count = 0
    for y in range(cy - radius, cy + radius + 1):
        row_off = y * width * 4
        for x in range(cx - radius, cx + radius + 1):
            idx = row_off + x * 4
            r_sum += rgba[idx]
            g_sum += rgba[idx + 1]
            b_sum += rgba[idx + 2]
            count += 1
    return (r_sum / count, g_sum / count, b_sum / count)


def patch_l1_difference(
    width: int,
    height: int,
    rgba: bytes,
    uv_a: tuple[float, float],
    uv_b: tuple[float, float],
    radius: int = 7,
) -> float:
    ax = max(radius, min(width - 1 - radius, int(round(width * uv_a[0]))))
    ay = max(radius, min(height - 1 - radius, int(round(height * uv_a[1]))))
    bx = max(radius, min(width - 1 - radius, int(round(width * uv_b[0]))))
    by = max(radius, min(height - 1 - radius, int(round(height * uv_b[1]))))
    total = 0.0
    count = 0
    for dy in range(-radius, radius + 1):
        for dx in range(-radius, radius + 1):
            ia = ((ay + dy) * width + (ax + dx)) * 4
            ib = ((by + dy) * width + (bx + dx)) * 4
            total += (
                abs(rgba[ia] - rgba[ib])
                + abs(rgba[ia + 1] - rgba[ib + 1])
                + abs(rgba[ia + 2] - rgba[ib + 2])
            ) / 3.0
            count += 1
    return total / count


def parse_kv_stdout(stdout: str) -> dict[str, str]:
    values: dict[str, str] = {}
    for line in stdout.splitlines():
        if "=" in line:
            key, val = line.split("=", 1)
            values[key.strip()] = val.strip()
    return values


def verify_deployed_assets(exe_dir: Path, source_showroom: Path) -> None:
    deployed_shaders = exe_dir / "shaders"
    spv_files = sorted(p.name for p in deployed_shaders.glob("*.spv"))
    required_shaders = {
        "static_indirect.vert.spv",
        "static_indirect.comp.spv",
        "static_indirect.frag.spv",
        "shadow_direct.vert.spv",
        "shadow_indirect.vert.spv",
        "fullscreen.vert.spv",
        "deferred_direct.frag.spv",
        "deferred_ibl.frag.spv",
        "deferred_shadow.frag.spv",
        "deferred_ibl_shadow.frag.spv",
        "display_sdr.frag.spv",
        "overlay.vert.spv",
        "overlay.frag.spv",
    }
    missing_shaders = sorted(required_shaders - set(spv_files))
    if missing_shaders:
        raise RuntimeError(
            f"missing required deployed shaders in {deployed_shaders}: {missing_shaders}"
        )

    deployed_showroom = exe_dir / "content" / "showroom"
    for text_rel in [
        "LICENSE.txt",
        "scene.json",
        *[f"assets/{p.name}" for p in sorted((source_showroom / "assets").glob("*.gltf"))],
    ]:
        src = source_showroom / text_rel
        dst = deployed_showroom / text_rel
        if not dst.is_file():
            raise RuntimeError(f"missing deployed showroom asset: {dst}")
        if dst.read_bytes() != src.read_bytes():
            raise RuntimeError(f"deployed showroom asset differs from source: {text_rel}")

    for generated_rel in [
        "assets/unit_box.bin",
        "assets/unit_sphere.bin",
        "assets/white.png",
        "assets/floor_grid.png",
    ]:
        dst = deployed_showroom / generated_rel
        if not dst.is_file() or dst.stat().st_size == 0:
            raise RuntimeError(f"missing generated showroom asset: {dst}")


def verify_showroom_image(png_bytes: bytes) -> None:
    width, height, rgba = decode_png_rgba(png_bytes)
    if width < 640 or height < 360:
        raise RuntimeError(f"unexpected showroom screenshot size: {width}x{height}")

    unique_rgb: set[tuple[int, int, int]] = set()
    for y in range(0, height, 8):
        row_off = y * width * 4
        for x in range(0, width, 8):
            idx = row_off + x * 4
            unique_rgb.add((rgba[idx], rgba[idx + 1], rgba[idx + 2]))
    if len(unique_rgb) < 64:
        raise RuntimeError(f"showroom frame looks too flat/blank (unique sampled RGB={len(unique_rgb)})")

    corner = sample_patch_rgb(width, height, rgba, 0.02, 0.03, radius=4)
    wall = sample_patch_rgb(width, height, rgba, 0.50, 0.22, radius=4)
    floor = sample_patch_rgb(width, height, rgba, 0.50, 0.92, radius=4)
    pillar = sample_patch_rgb(width, height, rgba, 0.251, 0.382, radius=6)
    neutral_sphere = sample_patch_rgb(width, height, rgba, 0.500, 0.593, radius=12)
    copper_dielectric = sample_patch_rgb(width, height, rgba, 0.286, 0.710, radius=12)
    copper_metal = sample_patch_rgb(width, height, rgba, 0.714, 0.710, radius=12)

    if max(abs(a - b) for a, b in zip(corner, wall, strict=True)) < 20.0:
        raise RuntimeError(f"corner {corner} and back wall {wall} are not visually distinct")
    if max(abs(a - b) for a, b in zip(corner, floor, strict=True)) < 20.0:
        raise RuntimeError(f"corner {corner} and floor {floor} are not visually distinct")
    if pillar[2] <= pillar[0]:
        raise RuntimeError(f"expected cool blue-gray pillar (B > R), got {pillar}")
    if copper_dielectric[0] <= copper_dielectric[2]:
        raise RuntimeError(
            f"expected warm copper dielectric sphere (R > B), got dielectric={copper_dielectric}"
        )
    copper_warmth = copper_dielectric[0] - copper_dielectric[2]
    neutral_warmth = neutral_sphere[0] - neutral_sphere[2]
    if copper_warmth < neutral_warmth + 10.0:
        raise RuntimeError(
            f"expected metallic copper row clearly warmer than neutral roughness row: "
            f"copper={copper_dielectric} neutral={neutral_sphere}"
        )
    if max(abs(a - b) for a, b in zip(copper_dielectric, copper_metal, strict=True)) < 10.0:
        raise RuntimeError(
            f"expected visible dielectric vs metallic shading difference: "
            f"m=0.0 {copper_dielectric} vs m=1.0 {copper_metal}"
        )
    roughness_diff = patch_l1_difference(width, height, rgba, (0.341, 0.593), (0.659, 0.593), radius=12)
    if roughness_diff < 1.0:
        raise RuntimeError(
            f"expected visible shading difference across roughness row (r=0.10 vs r=0.90), got L1={roughness_diff}"
        )

    # Regression check: grazing smooth normals on sphere silhouettes must not drop to pure black.
    black_silhouette_pixels = 0
    for y in range(int(height * 0.45), int(height * 0.85)):
        row_off = y * width * 4
        for x in range(int(width * 0.20), int(width * 0.80)):
            idx = row_off + x * 4
            if rgba[idx] == 0 and rgba[idx + 1] == 0 and rgba[idx + 2] == 0:
                black_silhouette_pixels += 1
    if black_silhouette_pixels != 0:
        raise RuntimeError(
            f"detected {black_silhouette_pixels} pure-black silhouette pixels in showroom sphere band"
        )


def run(executable: str, root: Path, configuration: str, source_showroom: Path) -> None:
    exe_path = Path(executable).resolve()
    exe_dir = exe_path.parent
    verify_deployed_assets(exe_dir, source_showroom.resolve())

    expected_scene = (exe_dir / "content" / "showroom" / "scene.json").resolve()
    expected_assets = (exe_dir / "content" / "showroom" / "assets").resolve()

    root.mkdir(parents=True, exist_ok=True)
    with TemporaryDirectory(prefix="showroom-launch-", dir=root) as temporary:
        output = Path(temporary)
        isolated_cwd = output / "isolated-cwd"
        isolated_cwd.mkdir()

        # 1. Launch ar_engine with zero mode flags from an isolated non-build-root CWD (1.3 Set).
        set_logs = output / "logs-set"
        set_png_path = output / "showroom-set.png"
        set_probes_path = output / "showroom-set.json"
        set_proc = subprocess.run(
            [
                str(exe_path),
                "--hidden",
                "--frames",
                "2",
                "--output",
                str(set_logs),
                "--screenshot",
                str(set_png_path),
                "--gbuffer-probes",
                str(set_probes_path),
            ],
            cwd=isolated_cwd,
            capture_output=True,
            text=True,
            timeout=60,
        )
        if set_proc.returncode != 0:
            raise RuntimeError(
                f"ar_engine Set launch failed (exit {set_proc.returncode}): {set_proc.stderr}"
            )
        set_kv = parse_kv_stdout(set_proc.stdout)
        if (
            set_kv.get("presented_frames") != "2"
            or set_kv.get("binding") != "set"
            or set_kv.get("shadows") != "on"
            or Path(set_kv.get("scene", "")).resolve() != expected_scene
            or Path(set_kv.get("asset_root", "")).resolve() != expected_assets
        ):
            raise RuntimeError(f"unexpected ar_engine Set stdout log: {set_proc.stdout!r}")

        set_summary = check_run_diagnostics(set_logs, configuration, expect_heap=False)
        set_probes = json.loads(set_probes_path.read_text(encoding="utf-8"))
        set_png = set_png_path.read_bytes()
        if set_probes.get("culling") != {
            "visible": len(EXPECTED_SHOWROOM_IDS),
            "drawn_ids": EXPECTED_SHOWROOM_IDS,
        }:
            raise RuntimeError(f"unexpected showroom culling result: {set_probes.get('culling')}")
        verify_showroom_image(set_png)

        # 2. Launch ar_engine --descriptor-heap from the exact same isolated CWD.
        heap_logs = output / "logs-heap"
        heap_png_path = output / "showroom-heap.png"
        heap_probes_path = output / "showroom-heap.json"
        heap_proc = subprocess.run(
            [
                str(exe_path),
                "--descriptor-heap",
                "--hidden",
                "--frames",
                "2",
                "--output",
                str(heap_logs),
                "--screenshot",
                str(heap_png_path),
                "--gbuffer-probes",
                str(heap_probes_path),
            ],
            cwd=isolated_cwd,
            capture_output=True,
            text=True,
            timeout=60,
        )
        if supports_descriptor_heap(set_summary):
            if heap_proc.returncode != 0:
                raise RuntimeError(
                    f"ar_engine Heap launch failed on capable GPU (exit {heap_proc.returncode}): "
                    f"{heap_proc.stderr}"
                )
            heap_kv = parse_kv_stdout(heap_proc.stdout)
            if (
                heap_kv.get("presented_frames") != "2"
                or heap_kv.get("binding") != "descriptor-heap"
                or heap_kv.get("shadows") != "on"
                or Path(heap_kv.get("scene", "")).resolve() != expected_scene
                or Path(heap_kv.get("asset_root", "")).resolve() != expected_assets
            ):
                raise RuntimeError(f"unexpected ar_engine Heap stdout log: {heap_proc.stdout!r}")
            check_run_diagnostics(heap_logs, configuration, expect_heap=True)
            heap_probes = json.loads(heap_probes_path.read_text(encoding="utf-8"))
            heap_png = heap_png_path.read_bytes()
            compare_outputs(set_probes, heap_probes, set_png, heap_png)
        else:
            if heap_proc.returncode == 0:
                raise RuntimeError("ar_engine --descriptor-heap succeeded on unsupported hardware")
            run_dir = next(heap_logs.iterdir())
            heap_summary = json.loads((run_dir / "summary.json").read_text(encoding="utf-8"))
            if heap_summary["result"] == "success" or not heap_summary["resources_released"]:
                raise RuntimeError(f"unsupported Heap run left unclean state: {heap_summary}")

        # 3. Verify explicit --scene override replaces showroom from the same isolated CWD.
        override_dir = output / "override-scene"
        override_dir.mkdir()
        override_scene, override_assets = make_scene(override_dir)
        override_logs = output / "logs-override"
        override_png_path = output / "override.png"
        override_proc = subprocess.run(
            [
                str(exe_path),
                "--hidden",
                "--frames",
                "2",
                "--scene",
                str(override_scene),
                "--asset-root",
                str(override_assets),
                "--output",
                str(override_logs),
                "--screenshot",
                str(override_png_path),
            ],
            cwd=isolated_cwd,
            capture_output=True,
            text=True,
            timeout=60,
        )
        if override_proc.returncode != 0:
            raise RuntimeError(
                f"explicit scene override failed (exit {override_proc.returncode}): {override_proc.stderr}"
            )
        override_kv = parse_kv_stdout(override_proc.stdout)
        if (
            override_kv.get("presented_frames") != "2"
            or override_kv.get("binding") != "set"
            or Path(override_kv.get("scene", "")).resolve() != override_scene.resolve()
            or Path(override_kv.get("asset_root", "")).resolve() != override_assets.resolve()
        ):
            raise RuntimeError(f"unexpected override stdout log: {override_proc.stdout!r}")
        check_run_diagnostics(override_logs, configuration, expect_heap=False)
        if override_png_path.read_bytes() == set_png:
            raise RuntimeError("explicit --scene override rendered identical PNG to showroom")

        # 4. Verify legacy/invalid flags exit 2 before Vulkan init.
        conflict_logs = output / "logs-conflict"
        conflict_proc = subprocess.run(
            [
                str(exe_path),
                "--showroom",
                "--hidden",
                "--frames",
                "2",
                "--output",
                str(conflict_logs),
            ],
            cwd=isolated_cwd,
            capture_output=True,
            text=True,
            timeout=30,
        )
        if conflict_proc.returncode != 2:
            raise RuntimeError(
                f"expected exit code 2 for removed legacy --showroom flag, got {conflict_proc.returncode}"
            )
        if conflict_logs.exists() and any(conflict_logs.iterdir()):
            raise RuntimeError("invalid CLI arguments unexpectedly initialized a diagnostic run")

        # 5. Verify missing scene file reports expanded path before Vulkan init and exits cleanly.
        missing_scene = output / "missing_showroom_scene.json"
        missing_scene_logs = output / "logs-missing-scene"
        missing_scene_proc = subprocess.run(
            [
                str(exe_path),
                "--hidden",
                "--frames",
                "2",
                "--scene",
                str(missing_scene),
                "--asset-root",
                str(override_assets),
                "--output",
                str(missing_scene_logs),
            ],
            cwd=isolated_cwd,
            capture_output=True,
            text=True,
            timeout=30,
        )
        if missing_scene_proc.returncode != 1:
            raise RuntimeError(
                f"expected exit code 1 for missing scene, got {missing_scene_proc.returncode}"
            )
        if "scene file" not in missing_scene_proc.stderr or "missing_showroom_scene.json" not in missing_scene_proc.stderr:
            raise RuntimeError(
                f"missing scene stderr did not report role and path: {missing_scene_proc.stderr!r}"
            )
        if missing_scene_logs.exists() and any(missing_scene_logs.iterdir()):
            raise RuntimeError("missing scene file unexpectedly started a Vulkan session")

        # 6. Verify missing referenced texture reports expanded path and closes session cleanly.
        broken_showroom = output / "broken-showroom"
        shutil.copytree(exe_dir / "content" / "showroom", broken_showroom)
        (broken_showroom / "assets" / "floor_grid.png").unlink()
        missing_tex_logs = output / "logs-missing-texture"
        missing_tex_proc = subprocess.run(
            [
                str(exe_path),
                "--hidden",
                "--frames",
                "2",
                "--scene",
                str(broken_showroom / "scene.json"),
                "--asset-root",
                str(broken_showroom / "assets"),
                "--output",
                str(missing_tex_logs),
            ],
            cwd=isolated_cwd,
            capture_output=True,
            text=True,
            timeout=60,
        )
        if missing_tex_proc.returncode != 1:
            raise RuntimeError(
                f"expected exit code 1 for missing texture, got {missing_tex_proc.returncode}"
            )
        if "floor_grid.png" not in missing_tex_proc.stderr:
            raise RuntimeError(
                f"missing texture stderr did not report missing path: {missing_tex_proc.stderr!r}"
            )
        run_dir = next(missing_tex_logs.iterdir())
        tex_summary = json.loads((run_dir / "summary.json").read_text(encoding="utf-8"))
        if not tex_summary["resources_released"] or not tex_summary["diagnostics_complete"]:
            raise RuntimeError(f"missing texture failure did not close session cleanly: {tex_summary}")
        tex_events = {
            json.loads(line)["event_id"]
            for line in (run_dir / "events.jsonl").read_text(encoding="utf-8").splitlines()
        }
        if "rhi.validation.error" in tex_events:
            raise RuntimeError("Vulkan validation reported errors on missing texture shutdown")


if __name__ == "__main__":
    run(sys.argv[1], Path(sys.argv[2]), sys.argv[3], Path(sys.argv[4]))
