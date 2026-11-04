"""Preflight the cloud runner image and the pinned P0 toolchain.

The P0 CPU workflow must not claim cloud verification on a different compiler, SDK or CMake
than the ones recorded by tasks.md 1.1. This script records what the runner actually has,
compares it with workflow/p0-toolchain-expectations.json and fails on any missing item or
mismatch so the difference is visible in the uploaded log instead of being absorbed silently.

It also picks the vcpkg root with the same order the command table's record-toolchain step
uses (Visual Studio bundled vcpkg first), so the toolchain recorded by the table and the
toolchain file used by CMake configure are the same one.

Deterministic and offline apart from the tools it probes; no model or network calls.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]


def fail(message: str, code: int = 1) -> "NoReturn":
    print(f"p0_ci_preflight: error: {message}", file=sys.stderr)
    raise SystemExit(code)


def run(args: list[str]) -> str:
    completed = subprocess.run(
        args,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
        stdin=subprocess.DEVNULL,
    )
    return f"{completed.stdout}\n{completed.stderr}"


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def write_json(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    text = json.dumps(value, ensure_ascii=False, sort_keys=True, indent=2) + "\n"
    path.write_text(text, encoding="utf-8", newline="\n")


def find_vswhere() -> Path | None:
    candidate = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / (
        "Microsoft Visual Studio/Installer/vswhere.exe"
    )
    return candidate if candidate.is_file() else None


def visual_studio_instances(vswhere: Path) -> list[dict]:
    raw = run([str(vswhere), "-latest", "-products", "*", "-format", "json", "-utf8"])
    start = raw.find("[")
    if start < 0:
        return []
    try:
        return json.loads(raw[start:])
    except json.JSONDecodeError:
        return []


def find_vcpkg_root(instances: list[dict]) -> tuple[Path | None, list[str]]:
    """Mirror p0_build_identity.find_vcpkg: Visual Studio bundled vcpkg wins."""
    alternatives: list[str] = []
    for instance in instances:
        candidate = Path(instance["installationPath"]) / "VC/vcpkg"
        if (candidate / "vcpkg.exe").is_file():
            return candidate, alternatives
    for variable in ("VCPKG_ROOT", "VCPKG_INSTALLATION_ROOT"):
        value = os.environ.get(variable)
        if value:
            alternatives.append(f"{variable}={value}")
    return None, alternatives


def newest_sdk_versions() -> list[str]:
    root = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / (
        "Windows Kits/10/Include"
    )
    if not root.is_dir():
        return []
    return sorted(entry.name for entry in root.iterdir() if entry.is_dir())


def check_item(checks: list[dict], name: str, expected: str, actual: str) -> None:
    if not actual:
        status = "missing"
    elif actual == expected:
        status = "ok"
    else:
        status = "mismatch"
    checks.append({"name": name, "expected": expected, "actual": actual, "status": status})


def preflight(args: argparse.Namespace) -> int:
    expectations_path = Path(args.expectations)
    if not expectations_path.is_file():
        fail(f"expectations file not found: {expectations_path}")
    expectations = json.loads(expectations_path.read_text(encoding="utf-8"))
    expected = expectations["expected"]

    checks: list[dict] = []
    image = {
        "image_os": os.environ.get("ImageOS", ""),
        "image_version": os.environ.get("ImageVersion", ""),
        "image_name": os.environ.get("ImageName", ""),
        "runner_name": os.environ.get("RUNNER_NAME", ""),
        "runner_os": os.environ.get("RUNNER_OS", ""),
        "platform": platform.platform(),
        "python": platform.python_version(),
    }

    vswhere = find_vswhere()
    instances: list[dict] = []
    if vswhere is not None:
        instances = visual_studio_instances(vswhere)
    instance = instances[0] if instances else {}
    install_path = Path(instance["installationPath"]) if instance.get("installationPath") else None
    vs_version = str(instance.get("installationVersion", ""))
    check_item(checks, "vs_major", expected["vs_major"], vs_version.split(".")[0] if vs_version else "")

    toolset = ""
    cl_version = ""
    if install_path is not None:
        tools_version = install_path / "VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt"
        if tools_version.is_file():
            toolset = tools_version.read_text(encoding="utf-8").strip()
        cl_path = install_path / "VC/Tools/MSVC" / toolset / "bin/Hostx64/x64/cl.exe"
        if toolset and cl_path.is_file():
            match = re.search(r"Version\s+([0-9.]+)", run([str(cl_path)]))
            if match:
                cl_version = match.group(1)
    check_item(checks, "msvc_toolset", expected["msvc_toolset"], toolset)
    check_item(checks, "cl_version", expected["cl_version"], cl_version)

    cmake_path = shutil.which("cmake") or ""
    cmake_version = ""
    generator_present = ""
    if cmake_path:
        match = re.search(r"cmake version ([0-9][0-9.]*)", run([cmake_path, "--version"]))
        if match:
            cmake_version = match.group(1)
        help_text = run([cmake_path, "--help"])
        if expected["generator"] in help_text:
            generator_present = expected["generator"]
    check_item(checks, "cmake_version", expected["cmake_version"], cmake_version)
    check_item(checks, "generator", expected["generator"], generator_present)

    sdk_versions = newest_sdk_versions()
    sdk_actual = expected["windows_sdk"] if expected["windows_sdk"] in sdk_versions else (
        sdk_versions[-1] if sdk_versions else ""
    )
    check_item(checks, "windows_sdk", expected["windows_sdk"], sdk_actual)

    vcpkg_root, alternatives = find_vcpkg_root(instances)
    vcpkg_toolchain = ""
    triplet_file = ""
    vcpkg_commit = ""
    if vcpkg_root is not None:
        toolchain_path = vcpkg_root / "scripts/buildsystems/vcpkg.cmake"
        if toolchain_path.is_file():
            vcpkg_toolchain = str(toolchain_path).replace("\\", "/")
        triplet_path = vcpkg_root / "triplets" / f"{expected['triplet']}.cmake"
        if triplet_path.is_file():
            triplet_file = f"{triplet_path.as_posix()} sha256={sha256_file(triplet_path)}"
        match = re.search(r"([0-9a-f]{20,40})", run([str(vcpkg_root / "vcpkg.exe"), "version"]))
        if match:
            vcpkg_commit = match.group(1)
    checks.append({
        "name": "vcpkg_toolchain",
        "expected": "<vcpkg_root>/scripts/buildsystems/vcpkg.cmake",
        "actual": vcpkg_toolchain,
        "status": "ok" if vcpkg_toolchain else "missing",
    })
    checks.append({
        "name": "vcpkg_triplet",
        "expected": f"<vcpkg_root>/triplets/{expected['triplet']}.cmake",
        "actual": triplet_file,
        "status": "ok" if triplet_file else "missing",
    })

    deps_root = Path(args.deps_root).resolve() if args.deps_root else REPO_ROOT / "build/deps"
    result = "failed" if any(item["status"] != "ok" for item in checks) else "ok"
    report = {
        "schema_version": 1,
        "change": expectations.get("change", ""),
        "image": image,
        "expected": expected,
        "actual": {
            "vs_version": vs_version,
            "vs_instance": str(install_path).replace("\\", "/") if install_path else "",
            "msvc_toolset": toolset,
            "cl_version": cl_version,
            "cmake_path": cmake_path.replace("\\", "/"),
            "cmake_version": cmake_version,
            "windows_sdk_installed": sdk_versions,
            "vcpkg_root": str(vcpkg_root).replace("\\", "/") if vcpkg_root else "",
            "vcpkg_commit": vcpkg_commit,
            "vcpkg_alternatives_ignored_by_table": alternatives,
        },
        "checks": checks,
        "deps_root": deps_root.as_posix(),
        "result": result,
    }

    width = max(len(item["name"]) for item in checks) + 2
    lines = [
        "P0 CPU preflight: runner image and pinned toolchain",
        f"image_os={image['image_os'] or '(not a GitHub runner)'} "
        f"image_version={image['image_version'] or '(unknown)'} platform={image['platform']}",
        "",
        f"{'check'.ljust(width)}{'expected'.ljust(34)}{'actual'.ljust(34)}status",
    ]
    for item in checks:
        lines.append(
            f"{item['name'].ljust(width)}{item['expected'][:32].ljust(34)}"
            f"{(item['actual'] or '(missing)')[:32].ljust(34)}{item['status']}"
        )
    lines.extend([
        "",
        f"vcpkg_root={report['actual']['vcpkg_root'] or '(missing)'} commit={vcpkg_commit or '(unknown)'}",
        f"vcpkg_alternatives_ignored_by_table={alternatives or 'none'}",
        f"deps_root={report['deps_root']}",
        f"result={result}",
    ])
    if result != "ok":
        lines.append(
            "MISMATCH POLICY: a missing tool or a version difference fails this job. A different "
            "toolchain needs separate review before the expectations file is updated; this run "
            "must not be reported as cloud verification of the pinned toolchain."
        )
    text = "\n".join(lines) + "\n"
    print(text, end="")

    Path(args.log).parent.mkdir(parents=True, exist_ok=True)
    Path(args.log).write_text(text, encoding="utf-8", newline="\n")
    write_json(Path(args.output), report)

    if args.github_env:
        entries = []
        if vcpkg_root is not None:
            entries.append(f"VCPKG_ROOT={vcpkg_root.as_posix()}")
        entries.append(f"AR_P0_DEPS={deps_root.as_posix()}")
        with open(args.github_env, "a", encoding="utf-8", newline="\n") as stream:
            stream.write("\n".join(entries) + "\n")

    return 0 if result == "ok" else 1


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--expectations", default=str(REPO_ROOT / "workflow/p0-toolchain-expectations.json"))
    parser.add_argument("--output", default=str(REPO_ROOT / "build/logs/ci-preflight.json"))
    parser.add_argument("--log", default=str(REPO_ROOT / "build/logs/ci-preflight.log"))
    parser.add_argument("--deps-root", default="")
    parser.add_argument("--github-env", default="")
    return preflight(parser.parse_args(argv))


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
