"""Build identity records and verification for AR Engine P0.

Subcommands (all deterministic, offline except record-dependencies):

  record-toolchain     write toolchain.json from the installed Visual Studio 2026 toolset
  record-dependencies  restore the vcpkg manifest into a task-owned root and record resolutions
  prepare              write build-input-manifest.json and build-identity.json for one config
  check                recompute the manifest/identity and fail on any difference

The sidecar records the compile options that change the produced binary without changing any
source file (currently AR_ENABLE_TEST_HOOKS) so a hookless production build cannot be verified
against a test-hook sidecar of the same configuration.

Identity encoding is canonical UTF-8 JSON: sorted keys, no extra whitespace, non-ASCII
characters left unescaped.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]

TRIPLET = "x64-windows-static-md"
GENERATOR = "Visual Studio 18 2026"

INCLUDE_GLOBS = (
    "CMakeLists.txt",
    "CMakePresets.json",
    "vcpkg.json",
    "vcpkg-configuration.json",
    "cmake/**/*",
    "engine/**/*",
    "tests/**/*",
    "scripts/p0_build_identity.py",
    "scripts/build_checks.py",
    "scripts/workflow_p0.py",
    "workflow/p0-checks.json",
)

# The command table and its runner are build inputs: changing which commands, arguments or
# CTest filters run must change the recorded identity. Their recorded exit codes are stable
# once every command has run, so the manifest does not churn per run.
EXCLUDED_PARTS = {".git", ".ar-workflow", "build", "out", "__pycache__"}

# Visual Studio writes CMakeSettings.json next to a CMakeLists.txt when the tree is opened, and
# the file is not an input of any build. Keeping it out of the manifest stops the editor from
# invalidating a recorded identity.
EXCLUDED_NAMES = {"CMakeSettings.json"}


def canonical_bytes(value: object) -> bytes:
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode(
        "utf-8"
    )


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


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


def read_json(path: Path) -> object:
    return json.loads(path.read_text(encoding="utf-8"))


def fail(message: str, code: int = 1) -> "NoReturn":
    print(f"p0_build_identity: error: {message}", file=sys.stderr)
    raise SystemExit(code)


def run(args: list[str], cwd: Path | None = None, env: dict[str, str] | None = None) -> str:
    completed = subprocess.run(
        args,
        cwd=str(cwd) if cwd else None,
        env=env,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    )
    if completed.returncode != 0:
        fail(
            f"command failed ({completed.returncode}): {' '.join(args)}\n"
            f"{completed.stdout}\n{completed.stderr}"
        )
    return completed.stdout


def run_unchecked(args: list[str]) -> str:
    """Run a tool that reports its own banner on a non-zero exit (cl.exe does)."""
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


def find_vswhere() -> Path:
    candidate = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / (
        "Microsoft Visual Studio/Installer/vswhere.exe"
    )
    if not candidate.is_file():
        fail(f"vswhere not found at {candidate}")
    return candidate


def find_vcpkg(explicit: str | None) -> Path:
    if explicit:
        path = Path(explicit)
        if not path.is_file():
            fail(f"vcpkg not found at {path}")
        return path
    vswhere = find_vswhere()
    raw = run([str(vswhere), "-latest", "-products", "*", "-format", "json", "-utf8"])
    instances = json.loads(raw)
    if not instances:
        fail("vswhere reported no Visual Studio instance")
    for instance in instances:
        candidate = Path(instance["installationPath"]) / "VC/vcpkg/vcpkg.exe"
        if candidate.is_file():
            return candidate
    fail("no vcpkg.exe found under any Visual Studio instance")


def toolchain_record(args: argparse.Namespace) -> int:
    vswhere = find_vswhere()
    raw = run([str(vswhere), "-latest", "-products", "*", "-format", "json", "-utf8"])
    instances = json.loads(raw)
    if not instances:
        fail("vswhere reported no Visual Studio instance")
    instance = instances[0]
    install_path = Path(instance["installationPath"])
    vs_version = instance.get("installationVersion", "")

    default_toolset = install_path / "VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt"
    if not default_toolset.is_file():
        fail(f"missing toolset file: {default_toolset}")
    toolset = default_toolset.read_text(encoding="utf-8").strip()
    cl_path = install_path / "VC/Tools/MSVC" / toolset / "bin/Hostx64/x64/cl.exe"
    if not cl_path.is_file():
        fail(f"cl.exe not found: {cl_path}")

    cl_output = run_unchecked([str(cl_path)])
    match = re.search(r"Version\s+([0-9.]+)", cl_output)
    if not match:
        fail(f"cannot parse cl version from: {cl_output}")
    cl_version = match.group(1)

    cmake_output = run(["cmake", "--version"])
    cmake_match = re.search(r"cmake version ([0-9][0-9.]*)", cmake_output)
    if not cmake_match:
        fail(f"cannot parse cmake version from: {cmake_output}")
    cmake_version = cmake_match.group(1)

    sdk_root = Path(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)")) / (
        "Windows Kits/10/Include"
    )
    sdk_versions = sorted(p.name for p in sdk_root.iterdir() if p.is_dir()) if sdk_root.is_dir() else []
    if not sdk_versions:
        fail(f"no Windows SDK found under {sdk_root}")
    windows_sdk = sdk_versions[-1]

    vcpkg = find_vcpkg(args.vcpkg)
    vcpkg_root = vcpkg.parent
    triplet_file = vcpkg_root / "triplets" / f"{TRIPLET}.cmake"
    if not triplet_file.is_file():
        fail(f"triplet file not found: {triplet_file}")
    version_output = run([str(vcpkg), "version"])
    commit_match = re.search(r"([0-9a-f]{20,40})", version_output)
    if not commit_match:
        fail(f"cannot parse vcpkg commit from: {version_output}")
    vcpkg_commit = commit_match.group(1)

    record = {
        "schema_version": 1,
        "vs_instance": str(install_path).replace("\\", "/"),
        "vs_version": vs_version,
        "toolset": toolset,
        "cl_version": cl_version,
        "cmake_version": cmake_version,
        "generator": GENERATOR,
        "windows_sdk": windows_sdk,
        "host_arch": "x64",
        "target_arch": "x64",
        "triplet": TRIPLET,
        "triplet_file_sha256": sha256_file(triplet_file),
        "crt": {"debug": "/MDd", "optimized": "/MD"},
        "vcpkg_tool_commit": vcpkg_commit,
    }
    write_json(Path(args.output), record)
    print(f"toolchain record: {args.output}")
    return 0


def parse_status_file(installed_root: Path) -> list[dict]:
    status = installed_root / "vcpkg" / "status"
    if not status.is_file():
        fail(f"vcpkg status file missing: {status}")
    entries: list[dict] = []
    current: dict[str, object] = {}
    for line in status.read_text(encoding="utf-8", errors="replace").splitlines():
        if not line.strip():
            if current:
                entries.append(current)
                current = {}
            continue
        if ":" not in line:
            continue
        key, _, value = line.partition(":")
        key = key.strip().lower()
        value = value.strip()
        if key == "package":
            current["name"] = value
        elif key == "version":
            current["version"] = value
        elif key == "port-version":
            current["port_version"] = int(value)
        elif key in {"architecture", "triplet"}:
            current["triplet"] = value
        elif key == "feature":
            # Feature stanzas repeat the package name; they are not independent ports.
            current["feature"] = value
    if current:
        entries.append(current)
    return entries


def dependencies_record(args: argparse.Namespace) -> int:
    snapshot = Path(args.snapshot).resolve()
    output = Path(args.output)
    output_root = output.resolve().parent
    installed_root = Path(args.install_root).resolve()
    # vcpkg compiles ports with its own CMake and fails with D8050 under a non-ASCII TMP or
    # buildtrees path, so the dependency work root is configurable (see AR_P0_DEPS).
    work_root = Path(args.dependency_work_root).resolve() if args.dependency_work_root \
        else output_root / "deps"
    downloads = work_root / "downloads"
    buildtrees = work_root / "buildtrees"
    packages = work_root / "packages"
    temp_root = work_root / "tmp"
    for directory in (installed_root, downloads, buildtrees, packages, temp_root):
        directory.mkdir(parents=True, exist_ok=True)

    vcpkg = find_vcpkg(args.vcpkg)
    env = dict(os.environ)
    for name in ("HTTP_PROXY", "HTTPS_PROXY", "http_proxy", "https_proxy", "NO_PROXY", "no_proxy"):
        env.pop(name, None)
    env.update({
        "TMP": str(temp_root),
        "TEMP": str(temp_root),
        "VCPKG_MAX_CONCURRENCY": "2",
    })
    args_list = [
        str(vcpkg),
        "install",
        f"--triplet={args.triplet}",
        f"--x-manifest-root={snapshot}",
        f"--x-install-root={installed_root}",
        f"--downloads-root={downloads}",
        f"--x-buildtrees-root={buildtrees}",
        f"--x-packages-root={packages}",
        "--no-binarycaching",
    ]
    completed = subprocess.run(
        args_list, env=env, capture_output=True, text=True, encoding="utf-8", errors="replace",
        check=False,
    )
    log_path = output_root / "logs" / "vcpkg-install.log"
    log_path.parent.mkdir(parents=True, exist_ok=True)
    log_path.write_text(completed.stdout + completed.stderr, encoding="utf-8", newline="\n")
    if completed.returncode != 0:
        fail(f"vcpkg install failed ({completed.returncode}); see {log_path}")

    configuration = read_json(snapshot / "vcpkg-configuration.json")
    baseline = ""
    registry = configuration.get("default-registry", {}) if isinstance(configuration, dict) else {}
    if isinstance(registry, dict):
        baseline = str(registry.get("baseline", ""))

    entries = parse_status_file(installed_root)
    features_by_port: dict[str, list[str]] = {}
    for entry in entries:
        feature = entry.get("feature")
        if feature:
            features_by_port.setdefault(str(entry.get("name", "")), []).append(str(feature))
    resolved = []
    for entry in entries:
        if entry.get("feature"):
            continue
        if str(entry.get("triplet", "")) != args.triplet:
            continue
        name = str(entry.get("name", ""))
        resolved.append({
            "name": name,
            "version": entry.get("version", ""),
            "port_version": int(entry.get("port_version", 0)),
            "triplet": args.triplet,
            "features": sorted(features_by_port.get(name, [])),
        })
    resolved.sort(key=lambda item: item["name"])
    if not resolved:
        fail(f"no ports recorded for triplet {args.triplet} in {installed_root}")

    version_output = run([str(vcpkg), "version"])
    commit_match = re.search(r"([0-9a-f]{20,40})", version_output)
    record = {
        "schema_version": 1,
        "manifest_sha256": sha256_file(snapshot / "vcpkg.json"),
        "triplet": args.triplet,
        "vcpkg_tool_commit": commit_match.group(1) if commit_match else "",
        "registry_baseline": baseline,
        "resolved": resolved,
    }
    write_json(output, record)
    print(f"dependency record: {output} ({len(resolved)} ports)")
    return 0


def include_files(snapshot: Path) -> list[Path]:
    files: set[Path] = set()
    for pattern in INCLUDE_GLOBS:
        for path in snapshot.glob(pattern):
            if not path.is_file():
                continue
            relative = path.relative_to(snapshot)
            if any(part in EXCLUDED_PARTS for part in relative.parts):
                continue
            if path.name in EXCLUDED_NAMES:
                continue
            files.add(path)
    return sorted(files, key=lambda p: p.relative_to(snapshot).as_posix())


def build_manifest(snapshot: Path) -> dict:
    entries = []
    for path in include_files(snapshot):
        entries.append({
            "path": path.relative_to(snapshot).as_posix(),
            "sha256": sha256_file(path),
        })
    input_hash = sha256_bytes(canonical_bytes(entries))
    return {"schema_version": 1, "input_hash": input_hash, "files": entries}


def source_stamp(snapshot: Path) -> dict:
    try:
        revision = run(["git", "-C", str(snapshot), "rev-parse", "HEAD"]).strip()
        porcelain = run(["git", "-C", str(snapshot), "status", "--porcelain"]).strip()
        return {"revision": revision, "dirty": bool(porcelain)}
    except SystemExit:
        return {"revision": None, "dirty": "unknown"}


def build_identity(
    snapshot: Path,
    configuration: str,
    toolchain: dict,
    dependency_record: dict,
    manifest: dict,
    build_options: dict,
) -> dict:
    dependency_hash = sha256_bytes(canonical_bytes(dependency_record))
    seed = {
        "input_hash": manifest["input_hash"],
        "configuration": configuration,
        "toolchain": toolchain,
        "dependency_manifest_hash": dependency_hash,
        "build_options": build_options,
    }
    return {
        "schema_version": 1,
        "build_id": sha256_bytes(canonical_bytes(seed)),
        "input_hash": manifest["input_hash"],
        "configuration": configuration,
        "toolchain": toolchain,
        "dependency_manifest_hash": dependency_hash,
        "build_options": build_options,
        "source": source_stamp(snapshot),
    }


def load_records(toolchain_path: Path, dependency_path: Path) -> tuple[dict, dict]:
    if not toolchain_path.is_file():
        fail(f"toolchain record missing: {toolchain_path}")
    if not dependency_path.is_file():
        fail(f"dependency record missing: {dependency_path}")
    return read_json(toolchain_path), read_json(dependency_path)


def prepare(args: argparse.Namespace) -> int:
    snapshot = Path(args.snapshot).resolve()
    output_root = Path(args.output).resolve()
    toolchain, dependency_record = load_records(
        Path(args.toolchain_record), Path(args.dependency_record)
    )
    manifest = build_manifest(snapshot)
    build_options = {"test_hooks": args.test_hooks == "on"}
    identity = build_identity(
        snapshot, args.configuration, toolchain, dependency_record, manifest, build_options
    )

    identity_dir = output_root / "identity" / args.configuration
    write_json(identity_dir / "build-input-manifest.json", manifest)
    write_json(identity_dir / "build-identity.json", identity)
    print(f"identity: {identity_dir / 'build-identity.json'}")
    print(f"build_id: {identity['build_id']}")
    print(f"input_hash: {identity['input_hash']}")
    return 0


def compare(label: str, expected: object, actual: object) -> bool:
    if expected != actual:
        print(f"p0_build_identity: mismatch in {label}", file=sys.stderr)
        print(f"  recorded: {expected!r}", file=sys.stderr)
        print(f"  current:  {actual!r}", file=sys.stderr)
        return False
    return True


def check(args: argparse.Namespace) -> int:
    snapshot = Path(args.snapshot).resolve()
    output_root = Path(args.output).resolve()
    identity_dir = output_root / "identity" / args.configuration
    identity_path = identity_dir / "build-identity.json"
    manifest_path = identity_dir / "build-input-manifest.json"
    if not identity_path.is_file():
        fail(f"build identity sidecar missing: {identity_path}")
    if not manifest_path.is_file():
        fail(f"build input manifest missing: {manifest_path}")

    recorded = read_json(identity_path)
    recorded_manifest = read_json(manifest_path)
    toolchain, dependency_record = load_records(
        Path(args.toolchain_record), Path(args.dependency_record)
    )

    current_manifest = build_manifest(snapshot)
    build_options = {"test_hooks": args.test_hooks == "on"}
    current_identity = build_identity(
        snapshot, args.configuration, toolchain, dependency_record, current_manifest, build_options
    )

    ok = True
    ok &= compare("input_hash", recorded_manifest.get("input_hash"), current_manifest["input_hash"])
    ok &= compare(
        "files", recorded_manifest.get("files"), current_manifest["files"]
    )
    ok &= compare("build_id", recorded.get("build_id"), current_identity["build_id"])
    ok &= compare(
        "dependency_manifest_hash",
        recorded.get("dependency_manifest_hash"),
        current_identity["dependency_manifest_hash"],
    )
    ok &= compare(
        "toolchain", recorded.get("toolchain"), current_identity["toolchain"]
    )
    ok &= compare(
        "build_options", recorded.get("build_options"), current_identity["build_options"]
    )
    if not ok:
        fail("build inputs changed without regenerating the identity sidecar")
    print(f"identity check: ok ({recorded['build_id']})")
    return 0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    toolchain_parser = subparsers.add_parser("record-toolchain")
    toolchain_parser.add_argument("--output", required=True)
    toolchain_parser.add_argument("--triplet", default=TRIPLET)
    toolchain_parser.add_argument("--vcpkg")
    toolchain_parser.set_defaults(func=toolchain_record)

    dependencies_parser = subparsers.add_parser("record-dependencies")
    dependencies_parser.add_argument("--snapshot", required=True)
    dependencies_parser.add_argument("--output", required=True)
    dependencies_parser.add_argument("--install-root", required=True)
    dependencies_parser.add_argument("--dependency-work-root")
    dependencies_parser.add_argument("--triplet", default=TRIPLET)
    dependencies_parser.add_argument("--vcpkg")
    dependencies_parser.set_defaults(func=dependencies_record)

    prepare_parser = subparsers.add_parser("prepare")
    prepare_parser.add_argument("--snapshot", required=True)
    prepare_parser.add_argument("--output", required=True)
    prepare_parser.add_argument("--configuration", required=True)
    prepare_parser.add_argument("--toolchain-record", required=True)
    prepare_parser.add_argument("--dependency-record", required=True)
    prepare_parser.add_argument(
        "--test-hooks",
        required=True,
        choices=("on", "off"),
        help="whether AR_ENABLE_TEST_HOOKS is part of this build (part of build_options)",
    )
    prepare_parser.set_defaults(func=prepare)

    check_parser = subparsers.add_parser("check")
    check_parser.add_argument("--snapshot", required=True)
    check_parser.add_argument("--output", required=True)
    check_parser.add_argument("--configuration", required=True)
    check_parser.add_argument("--toolchain-record", required=True)
    check_parser.add_argument("--dependency-record", required=True)
    check_parser.add_argument(
        "--test-hooks",
        required=True,
        choices=("on", "off"),
        help="whether AR_ENABLE_TEST_HOOKS is part of this build (part of build_options)",
    )
    check_parser.set_defaults(func=check)

    args = parser.parse_args(argv)
    return int(args.func(args))


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
