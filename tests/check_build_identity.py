"""Negative and positive checks for the compiled build identity.

Covers: matching identity, stale sidecar, wrong configuration, missing sidecar, triplet
mismatch, CRT mismatch, and source changed without regenerating the identity.
"""

from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

FAILURES: list[str] = []


def fail(message: str) -> None:
    FAILURES.append(message)
    print(f"build-identity: FAIL: {message}", file=sys.stderr)


def run(args: list[str], expect: int = 0, label: str = "") -> subprocess.CompletedProcess:
    completed = subprocess.run(
        args, capture_output=True, text=True, encoding="utf-8", errors="replace", check=False
    )
    if completed.returncode != expect:
        fail(
            f"{label or ' '.join(args)}: expected exit {expect}, got {completed.returncode}\n"
            f"{completed.stdout}{completed.stderr}"
        )
    return completed


def load_json(path: Path) -> dict:
    return json.loads(path.read_text(encoding="utf-8"))


def write_json(path: Path, value: dict) -> None:
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")


def check_compiled_matches(probe: str, sidecar: Path, toolchain: Path, configuration: str) -> None:
    completed = run([probe, "--json"], label="probe --json")
    try:
        compiled = json.loads(completed.stdout)
    except json.JSONDecodeError as error:
        fail(f"probe --json did not print JSON: {error}")
        return

    recorded = load_json(sidecar)
    for field in ("build_id", "input_hash", "configuration"):
        if compiled.get(field) != recorded.get(field):
            fail(f"compiled {field} does not match the sidecar")
    if compiled.get("configuration") != configuration:
        fail(f"binary was built as {compiled.get('configuration')!r}, expected {configuration!r}")
    if compiled.get("source") != recorded.get("source"):
        fail("compiled source stamp does not match the sidecar")
    if compiled.get("language_mode") != "c++23preview":
        fail(
            f"binary was compiled in {compiled.get('language_mode')!r}, "
            "expected 'c++23preview'"
        )

    tool = load_json(toolchain)
    if compiled.get("triplet") != tool.get("triplet"):
        fail("compiled triplet does not match the toolchain record")
    crt_key = "debug" if configuration == "Debug" else "optimized"
    if compiled.get("crt") != tool.get("crt", {}).get(crt_key):
        fail(f"compiled CRT {compiled.get('crt')!r} does not match the toolchain record")


def copy_manifest_snapshot(snapshot: Path, manifest: dict, destination: Path) -> Path:
    for entry in manifest.get("files", []):
        source = snapshot / entry["path"]
        target = destination / entry["path"]
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
    return destination


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", required=True)
    parser.add_argument("--python", required=True)
    parser.add_argument("--identity-script", required=True)
    parser.add_argument("--snapshot", required=True)
    parser.add_argument("--output-root", required=True)
    parser.add_argument(
        "--metadata-dir",
        default=None,
        help="directory holding toolchain.json and dependencies.json (defaults to --output-root)",
    )
    parser.add_argument("--configuration", required=True)
    args = parser.parse_args(argv)

    snapshot = Path(args.snapshot).resolve()
    output_root = Path(args.output_root).resolve()
    metadata_dir = Path(args.metadata_dir).resolve() if args.metadata_dir else output_root
    configuration = args.configuration
    identity_dir = output_root / "identity" / configuration
    sidecar = identity_dir / "build-identity.json"
    manifest_path = identity_dir / "build-input-manifest.json"
    toolchain = metadata_dir / "toolchain.json"
    dependencies = metadata_dir / "dependencies.json"

    for required in (sidecar, manifest_path, toolchain, dependencies):
        if not required.is_file():
            fail(f"required identity input missing: {required}")
    if FAILURES:
        return 1

    # 1. The binary carries exactly the identity of its sidecar.
    check_compiled_matches(args.probe, sidecar, toolchain, configuration)

    # 2. The verification path accepts the real pair.
    run([args.probe, "--verify", str(sidecar), "--toolchain", str(toolchain)],
        label="probe --verify (valid)")

    workspace = Path(tempfile.mkdtemp(prefix="ar-identity-", dir=str(output_root)))
    try:
        # 3. Stale build id in the sidecar must be rejected.
        mutated = load_json(sidecar)
        mutated["build_id"] = "0" * 64
        stale = workspace / "stale-identity.json"
        write_json(stale, mutated)
        run([args.probe, "--verify", str(stale), "--toolchain", str(toolchain)], expect=2,
            label="probe --verify (stale build_id)")

        # 4. A sidecar for another configuration must be rejected.
        wrong_config = load_json(sidecar)
        wrong_config["configuration"] = "Release" if configuration != "Release" else "Debug"
        wrong_path = workspace / "wrong-configuration.json"
        write_json(wrong_path, wrong_config)
        run([args.probe, "--verify", str(wrong_path), "--toolchain", str(toolchain)], expect=2,
            label="probe --verify (wrong configuration)")

        # 5. A missing sidecar must be rejected.
        run([args.probe, "--verify", str(workspace / "absent.json")], expect=2,
            label="probe --verify (missing sidecar)")

        # 6. Triplet and CRT mismatches must be rejected.
        bad_triplet = load_json(toolchain)
        bad_triplet["triplet"] = "x64-windows-static"
        bad_triplet_path = workspace / "toolchain-bad-triplet.json"
        write_json(bad_triplet_path, bad_triplet)
        run([args.probe, "--verify", str(sidecar), "--toolchain", str(bad_triplet_path)], expect=2,
            label="probe --verify (triplet mismatch)")

        bad_crt = load_json(toolchain)
        crt_key = "debug" if configuration == "Debug" else "optimized"
        bad_crt["crt"][crt_key] = "/MT" if crt_key == "debug" else "/MTd"
        bad_crt_path = workspace / "toolchain-bad-crt.json"
        write_json(bad_crt_path, bad_crt)
        run([args.probe, "--verify", str(sidecar), "--toolchain", str(bad_crt_path)], expect=2,
            label="probe --verify (CRT mismatch)")

        common = [
            args.python, args.identity_script, "check",
            "--configuration", configuration,
        ]

        # 7. The recorded identity still matches the current sources.
        run(common + [
            "--snapshot", str(snapshot),
            "--output", str(output_root),
            "--toolchain-record", str(toolchain),
            "--dependency-record", str(dependencies),
        ], label="identity check (current sources)")

        # 8. A changed source without a regenerated identity must fail.
        stale_snapshot = copy_manifest_snapshot(
            snapshot, load_json(manifest_path), workspace / "snapshot")
        target = None
        for entry in load_json(manifest_path).get("files", []):
            if entry["path"].endswith(".h"):
                target = stale_snapshot / entry["path"]
                break
        if target is None:
            fail("manifest has no header to mutate")
        else:
            with target.open("a", encoding="utf-8", newline="\n") as stream:
                stream.write("\n// stale-source probe\n")
        stale_output = workspace / "stale-output"
        (stale_output / "identity" / configuration).mkdir(parents=True, exist_ok=True)
        shutil.copy2(sidecar, stale_output / "identity" / configuration / "build-identity.json")
        shutil.copy2(manifest_path,
                     stale_output / "identity" / configuration / "build-input-manifest.json")
        shutil.copy2(toolchain, stale_output / "toolchain.json")
        shutil.copy2(dependencies, stale_output / "dependencies.json")
        completed = subprocess.run(
            common + [
                "--snapshot", str(stale_snapshot),
                "--output", str(stale_output),
                "--toolchain-record", str(stale_output / "toolchain.json"),
                "--dependency-record", str(stale_output / "dependencies.json"),
            ],
            capture_output=True, text=True, encoding="utf-8", errors="replace", check=False,
        )
        if completed.returncode == 0:
            fail("a changed source was accepted without regenerating the identity")
    finally:
        shutil.rmtree(workspace, ignore_errors=True)

    if FAILURES:
        print(f"build-identity: {len(FAILURES)} failure(s)", file=sys.stderr)
        return 1
    print(f"build-identity: ok ({configuration})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
