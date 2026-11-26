"""End-to-end verification for P7 standalone portable package installed via `cmake --install`."""

from __future__ import annotations

import json
import os
import shutil
import stat
import struct
import subprocess
import sys
from pathlib import Path
from tempfile import TemporaryDirectory

from check_descriptor_heap_scene import (
    check_run_diagnostics,
    supports_descriptor_heap,
)
from check_four_config_benchmark import verify_and_recompute_run
from check_showroom_launch import parse_kv_stdout, verify_showroom_image

EXPECTED_PRODUCT_SHADERS = [
    "gbuffer.vert.spv",
    "gbuffer_bindless.frag.spv",
    "static_indirect.vert.spv",
    "static_indirect.frag.spv",
    "static_indirect.comp.spv",
    "shadow_direct.vert.spv",
    "shadow_indirect.vert.spv",
    "fullscreen.vert.spv",
    "deferred_ibl.frag.spv",
    "deferred_ibl_shadow.frag.spv",
    "display_sdr.frag.spv",
    "overlay.vert.spv",
    "overlay.frag.spv",
]

ALLOWED_SYSTEM_DLL_PREFIXES = (
    "kernel32",
    "user32",
    "gdi32",
    "shell32",
    "imm32",
    "advapi32",
    "ole32",
    "msvcp140",
    "vcruntime140",
    "ucrtbase",
    "api-ms-win-crt-",
)


def read_pe_imports(exe_path: Path) -> list[str]:
    data = exe_path.read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    opt = struct.unpack_from("<H", data, pe + 20)[0]
    imp_rva = struct.unpack_from("<I", data, pe + 24 + 120)[0]
    secs = [struct.unpack_from("<8sIIII", data, pe + 24 + opt + i * 40) for i in range(nsec)]

    def rva2off(rva: int) -> int:
        for _, vs, va, rs, raw in secs:
            if va <= rva < va + max(vs, rs):
                return raw + (rva - va)
        raise RuntimeError(f"RVA {rva:#x} not mapped in PE sections")

    off = rva2off(imp_rva)
    dlls: list[str] = []
    while True:
        _, _, _, name_rva, _ = struct.unpack_from("<IIIII", data, off)
        if name_rva == 0:
            break
        name_off = rva2off(name_rva)
        end = data.index(b"\0", name_off)
        dlls.append(data[name_off:end].decode("ascii", errors="replace"))
        off += 20
    return dlls


def set_tree_readonly(root: Path, readonly: bool) -> None:
    for item in root.rglob("*"):
        if item.is_file():
            os.chmod(item, stat.S_IREAD if readonly else (stat.S_IREAD | stat.S_IWRITE))


def verify_installed_layout(prefix: Path) -> None:
    exe = prefix / "ar_engine.exe"
    if not exe.is_file():
        raise RuntimeError(f"missing installed ar_engine.exe in {prefix}")
    if not (prefix / "README.txt").is_file():
        raise RuntimeError(f"missing installed README.txt in {prefix}")

    # Verify zero unexpected third-party DLL dependencies.
    imports = read_pe_imports(exe)
    unexpected = [
        dll
        for dll in imports
        if not any(dll.lower().startswith(p) for p in ALLOWED_SYSTEM_DLL_PREFIXES)
    ]
    if unexpected:
        raise RuntimeError(f"unexpected non-system DLL imports in ar_engine.exe: {unexpected}")

    shader_dir = prefix / "shaders"
    actual_shaders = sorted(p.name for p in shader_dir.glob("*.spv"))
    if actual_shaders != sorted(EXPECTED_PRODUCT_SHADERS):
        raise RuntimeError(
            f"installed shaders mismatch:\nexpected={sorted(EXPECTED_PRODUCT_SHADERS)}\n"
            f"actual={actual_shaders}"
        )

    for rel in [
        "content/showroom/scene.json",
        "content/showroom/LICENSE.txt",
        "content/showroom/assets/unit_box.bin",
        "content/showroom/assets/unit_sphere.bin",
        "content/showroom/assets/white.png",
        "content/showroom/assets/floor_grid.png",
        "presets/showroom_default.json",
        "presets/showroom_closeup_metallic.json",
        "benchmark/showroom_set_gpu.json",
        "benchmark/showroom_heap_gpu.json",
    ]:
        target = prefix / rel
        if not target.is_file() or target.stat().st_size == 0:
            raise RuntimeError(f"missing installed package file: {target}")


def run(cmake_exe: str, build_dir: Path, configuration: str, output_root: Path) -> None:
    output_root.mkdir(parents=True, exist_ok=True)
    with TemporaryDirectory(prefix="installed-pkg-", dir=output_root) as temp:
        base = Path(temp)
        # Intentionally include both Chinese characters and spaces in the install prefix.
        install_prefix = base / "独立运行包 测试" / "AR Engine"
        isolated_cwd = base / "isolated cwd"
        isolated_cwd.mkdir(parents=True)

        # 1. Execute real `cmake --install` into the isolated Chinese+space prefix.
        install_proc = subprocess.run(
            [
                cmake_exe,
                "--install",
                str(build_dir),
                "--config",
                configuration,
                "--prefix",
                str(install_prefix),
            ],
            capture_output=True,
            encoding="utf-8",
            errors="replace",
            timeout=120,
        )
        if install_proc.returncode != 0:
            raise RuntimeError(
                f"cmake --install failed (exit {install_proc.returncode}):\n"
                f"stdout:\n{install_proc.stdout}\nstderr:\n{install_proc.stderr}"
            )

        verify_installed_layout(install_prefix)
        installed_exe = (install_prefix / "ar_engine.exe").resolve()
        expected_scene = (install_prefix / "content" / "showroom" / "scene.json").resolve()
        expected_assets = (install_prefix / "content" / "showroom" / "assets").resolve()

        # 1b. Zero-env, zero---output launch (true double-click mode) on writable install prefix
        # must write directly into <install_prefix>/output without touching %LOCALAPPDATA%.
        env_doubleclick = os.environ.copy()
        env_doubleclick.pop("AR_ENGINE_OUTPUT_DIR", None)
        dc_proc = subprocess.run(
            [str(installed_exe), "--hidden", "--frames", "2"],
            cwd=isolated_cwd,
            env=env_doubleclick,
            capture_output=True,
            encoding="utf-8",
            errors="replace",
            timeout=60,
        )
        if dc_proc.returncode != 0:
            raise RuntimeError(
                f"installed zero-env launch failed (exit {dc_proc.returncode}): {dc_proc.stderr}"
            )
        dc_kv = parse_kv_stdout(dc_proc.stdout)
        expected_portable_out = (install_prefix / "output").resolve()
        if Path(dc_kv.get("output_dir", "")).resolve() != expected_portable_out:
            raise RuntimeError(
                f"expected zero-env portable output {expected_portable_out}, got:\n{dc_proc.stdout}"
            )
        check_run_diagnostics(expected_portable_out, configuration, expect_heap=False)
        shutil.rmtree(expected_portable_out)

        # 2. Mark installed files read-only and run from isolated CWD using AR_ENGINE_OUTPUT_DIR
        # (no --output flag) to prove read-only program directories write cleanly to user dir.
        user_out_set = base / "user-data-set"
        user_out_heap = base / "user-data-heap"
        set_png = base / "installed-set.png"
        heap_png = base / "installed-heap.png"
        set_tree_readonly(install_prefix, True)
        try:
            env_set = os.environ.copy()
            env_set["AR_ENGINE_OUTPUT_DIR"] = str(user_out_set)
            set_proc = subprocess.run(
                [
                    str(installed_exe),
                    "--hidden",
                    "--frames",
                    "2",
                    "--screenshot",
                    str(set_png),
                ],
                cwd=isolated_cwd,
                env=env_set,
                capture_output=True,
                encoding="utf-8",
                errors="replace",
                timeout=60,
            )
            if set_proc.returncode != 0:
                raise RuntimeError(
                    f"installed Set launch failed (exit {set_proc.returncode}): {set_proc.stderr}"
                )
            set_kv = parse_kv_stdout(set_proc.stdout)
            if (
                not set_kv.get("build_id")
                or set_kv.get("build_config") != configuration
                or set_kv.get("binding") != "set"
                or Path(set_kv.get("scene", "")).resolve() != expected_scene
                or Path(set_kv.get("asset_root", "")).resolve() != expected_assets
                or Path(set_kv.get("output_dir", "")).resolve() != user_out_set.resolve()
            ):
                raise RuntimeError(f"unexpected installed Set stdout:\n{set_proc.stdout}")

            set_summary = check_run_diagnostics(user_out_set, configuration, expect_heap=False)
            verify_showroom_image(set_png.read_bytes())

            # 3. Run installed Heap path from the same read-only folder & isolated CWD.
            env_heap = os.environ.copy()
            env_heap["AR_ENGINE_OUTPUT_DIR"] = str(user_out_heap)
            heap_proc = subprocess.run(
                [
                    str(installed_exe),
                    "--descriptor-heap",
                    "--hidden",
                    "--frames",
                    "2",
                    "--screenshot",
                    str(heap_png),
                ],
                cwd=isolated_cwd,
                env=env_heap,
                capture_output=True,
                encoding="utf-8",
                errors="replace",
                timeout=60,
            )
            if supports_descriptor_heap(set_summary):
                if heap_proc.returncode != 0:
                    raise RuntimeError(
                        f"installed Heap launch failed (exit {heap_proc.returncode}): {heap_proc.stderr}"
                    )
                check_run_diagnostics(user_out_heap, configuration, expect_heap=True)
                if set_png.read_bytes() != heap_png.read_bytes():
                    raise RuntimeError("installed Set and Heap showroom PNG outputs differ")
            else:
                if heap_proc.returncode == 0:
                    raise RuntimeError("expected Heap launch to fail on non-1.4/Heap GPU")

            # 4. Run packaged portable benchmarks from the read-only install directory (twice for
            # Set to verify repeatable execution into the same user output directory!).
            user_bench = base / "user-data-bench"
            env_bench = os.environ.copy()
            env_bench["AR_ENGINE_OUTPUT_DIR"] = str(user_bench)
            set_spec = install_prefix / "benchmark" / "showroom_set_gpu.json"
            for attempt in range(2):
                bench_proc = subprocess.run(
                    [str(installed_exe), "--benchmark", str(set_spec)],
                    cwd=isolated_cwd,
                    env=env_bench,
                    capture_output=True,
                    encoding="utf-8",
                    errors="replace",
                    timeout=120,
                )
                if bench_proc.returncode != 0:
                    raise RuntimeError(
                        f"installed Set benchmark attempt {attempt + 1} failed: {bench_proc.stderr}"
                    )
            verify_and_recompute_run(
                user_bench / "benchmark" / "showroom-set-gpu", "set", "gpu", 14, 60
            )

            heap_spec = install_prefix / "benchmark" / "showroom_heap_gpu.json"
            heap_bench = subprocess.run(
                [str(installed_exe), "--benchmark", str(heap_spec)],
                cwd=isolated_cwd,
                env=env_bench,
                capture_output=True,
                encoding="utf-8",
                errors="replace",
                timeout=120,
            )
            if heap_bench.returncode != 0:
                raise RuntimeError(f"installed Heap benchmark failed: {heap_bench.stderr}")
            verify_and_recompute_run(
                user_bench / "benchmark" / "showroom-heap-gpu", "descriptor_heap", "gpu", 14, 60
            )

            # 5. Unwritable output directory (existing regular file) fails cleanly on stderr.
            blocker = base / "blocked-output-file.txt"
            blocker.write_text("blocker", encoding="utf-8")
            env_blocked = os.environ.copy()
            env_blocked["AR_ENGINE_OUTPUT_DIR"] = str(blocker)
            blocked_proc = subprocess.run(
                [str(installed_exe), "--hidden", "--frames", "1"],
                cwd=isolated_cwd,
                env=env_blocked,
                capture_output=True,
                encoding="utf-8",
                errors="replace",
                timeout=30,
            )
            if blocked_proc.returncode == 0 or "blocked-output-file.txt" not in blocked_proc.stderr:
                raise RuntimeError(
                    f"expected unwritable output error naming blocker, got exit={blocked_proc.returncode} "
                    f"stderr={blocked_proc.stderr!r}"
                )
        finally:
            set_tree_readonly(install_prefix, False)

        # 6. Missing shader & missing scene diagnostics on a scratch copy of the installed tree.
        broken = base / "broken-copy"
        shutil.copytree(install_prefix, broken)
        broken_exe = broken / "ar_engine.exe"
        missing_shader = broken / "shaders" / "display_sdr.frag.spv"
        shader_backup = missing_shader.read_bytes()
        missing_shader.unlink()
        miss_shader_proc = subprocess.run(
            [str(broken_exe), "--hidden", "--frames", "1", "--output", str(base / "miss-shader-out")],
            cwd=isolated_cwd,
            capture_output=True,
            encoding="utf-8",
            errors="replace",
            timeout=30,
        )
        if miss_shader_proc.returncode != 1 or "display_sdr.frag.spv" not in miss_shader_proc.stderr:
            raise RuntimeError(
                f"expected missing shader error, got exit={miss_shader_proc.returncode} "
                f"stderr={miss_shader_proc.stderr!r}"
            )
        missing_shader.write_bytes(shader_backup)

        missing_scene = broken / "content" / "showroom" / "scene.json"
        missing_scene.unlink()
        miss_scene_proc = subprocess.run(
            [str(broken_exe), "--hidden", "--frames", "1", "--output", str(base / "miss-scene-out")],
            cwd=isolated_cwd,
            capture_output=True,
            encoding="utf-8",
            errors="replace",
            timeout=30,
        )
        if miss_scene_proc.returncode != 1 or "scene.json" not in miss_scene_proc.stderr:
            raise RuntimeError(
                f"expected missing scene error, got exit={miss_scene_proc.returncode} "
                f"stderr={miss_scene_proc.stderr!r}"
            )


if __name__ == "__main__":
    if len(sys.argv) != 5:
        raise SystemExit(
            "usage: check_installed_app.py <cmake_exe> <build_dir> <configuration> <output_root>"
        )
    run(sys.argv[1], Path(sys.argv[2]), sys.argv[3], Path(sys.argv[4]))
