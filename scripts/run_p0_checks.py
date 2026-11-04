"""Run the recorded P0 check commands from workflow/p0-checks.json.

The manifest is machine independent: paths use ${REPO}, ${OUT}, ${VCPKG_ROOT} and ${DEPS}.
This runner expands them from the environment, executes the commands in dependency order,
writes one log per command under build/logs/p0-checks, and records the real exit codes back
into the manifest. A required command that fails stops the run; nothing is marked passed by
the runner itself.
"""

from __future__ import annotations

import argparse
import json
import locale
import os
import subprocess
import sys
import time
from pathlib import Path

PLACEHOLDERS = ("REPO", "OUT", "VCPKG_ROOT", "DEPS")


def fail(message: str) -> int:
    print(f"run_p0_checks: {message}", file=sys.stderr)
    return 1


def build_substitutions(repo_root: Path) -> dict[str, str]:
    values = {
        "REPO": repo_root.as_posix(),
        "OUT": (repo_root / "build").as_posix(),
        "VCPKG_ROOT": os.environ.get("VCPKG_ROOT", ""),
        "DEPS": os.environ.get("AR_P0_DEPS", ""),
    }
    missing = [name for name in ("VCPKG_ROOT", "DEPS") if not values[name]]
    if missing:
        hint = ", ".join(
            name if name == "VCPKG_ROOT" else "AR_P0_DEPS" for name in missing
        )
        raise SystemExit(
            f"run_p0_checks: set {hint} (see CMakeUserPresets.json for the local values) "
            "before running the P0 checks"
        )
    return values


def expand(value: str, substitutions: dict[str, str]) -> str:
    for name in PLACEHOLDERS:
        value = value.replace("${" + name + "}", substitutions[name])
    return value


def expand_argv(command: dict, substitutions: dict[str, str]) -> tuple[list[str], str]:
    argv = [expand(str(item), substitutions) for item in command["argv"]]
    cwd = expand(str(command.get("cwd", "${REPO}")), substitutions)
    return argv, cwd


def order_commands(commands: list[dict]) -> list[dict]:
    by_id = {str(command["id"]): command for command in commands}
    ordered: list[dict] = []
    remaining = dict(by_id)
    while remaining:
        ready = [
            command for command in remaining.values()
            if all(dep not in remaining for dep in command.get("depends_on", []))
        ]
        if not ready:
            unresolved = ", ".join(sorted(remaining))
            raise SystemExit(f"run_p0_checks: cyclic depends_on among {unresolved}")
        ready.sort(key=lambda item: str(item["id"]))
        for command in ready:
            ordered.append(command)
            remaining.pop(str(command["id"]), None)
    return ordered


def sanitized_environment() -> dict[str, str]:
    """Return os.environ without case-duplicated keys.

    Windows environment blocks can hold both ``NO_PROXY`` and ``no_proxy``. MSBuild builds a
    Hashtable from the environment and fails with MSB6001 ("已添加项") when it meets the second
    spelling, which surfaces as "No CMAKE_CXX_COMPILER could be found". Keeping the upper-case
    spelling and dropping the other keeps proxy settings usable without breaking the build.
    """
    environment = dict(os.environ)
    seen: dict[str, str] = {}
    for key in list(environment):
        canonical = key.upper()
        if canonical in seen:
            if key != canonical:
                del environment[key]
                continue
            del environment[seen[canonical]]
        seen[canonical] = key
    return environment


def decode_output(raw: bytes | str | None) -> str:
    """Decode child output without silently mangling non-ASCII text.

    CMake and MSBuild emit local-code-page bytes on a Chinese Windows console, while CMake
    messages that carry UTF-8 paths stay UTF-8, and one log can mix both. Decoding line by line
    keeps the readable half readable instead of forcing a whole-buffer fallback; a single line
    that is not valid UTF-8 falls back to the console code page.
    """
    if raw is None:
        return ""
    if isinstance(raw, str):
        return raw
    fallback = locale.getpreferredencoding(False)
    lines = raw.split(b"\n")
    decoded = []
    for line in lines:
        try:
            decoded.append(line.decode("utf-8"))
        except UnicodeDecodeError:
            decoded.append(line.decode(fallback, "replace"))
    return "\n".join(decoded)


def write_results(results_path: Path, results: dict) -> None:
    """Record the observed exit codes as evidence, not as a mutation of the command table.

    workflow/p0-checks.json is a hashed build input (it is listed in the build identity manifest),
    so the runner must never rewrite it while it is also part of the identity. Observed exits and
    timings live here instead; the table stays a stable declaration.
    """
    encoded = json.dumps(results, indent=2, ensure_ascii=False) + "\n"
    if not results_path.exists() or results_path.read_text(encoding="utf-8") != encoded:
        results_path.write_text(encoded, encoding="utf-8", newline="\n")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", default=None)
    parser.add_argument("--only", nargs="+", default=None, help="run only the named command ids")
    args = parser.parse_args(argv)

    repo_root = Path(__file__).resolve().parent.parent
    manifest_path = Path(args.manifest) if args.manifest else repo_root / "workflow" / "p0-checks.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    substitutions = build_substitutions(repo_root)
    log_dir = repo_root / "build" / "logs" / "p0-checks"
    log_dir.mkdir(parents=True, exist_ok=True)

    commands = order_commands(manifest["commands"])
    environment = sanitized_environment()
    results_path = log_dir / "results.json"
    results: dict = {
        "schema_version": 1,
        "manifest": str(manifest_path),
        "commands": [],
    }
    if args.only:
        wanted = set(args.only)
        unknown = wanted - {str(command["id"]) for command in commands}
        if unknown:
            return fail(f"unknown command ids: {', '.join(sorted(unknown))}")
        commands = [command for command in commands if str(command["id"]) in wanted]

    for command in commands:
        command_id = str(command["id"])
        resolved_argv, cwd = expand_argv(command, substitutions)
        print(f"[run] {command_id}: {' '.join(resolved_argv)}")
        started = time.monotonic()
        wrapper_exit = 0
        child_exit: int | None = None
        try:
            completed = subprocess.run(
                resolved_argv,
                cwd=cwd,
                env=environment,
                capture_output=True,
                text=False,
                timeout=float(command.get("timeout_seconds", 600)),
                check=False,
            )
            output = decode_output(completed.stdout) + decode_output(completed.stderr)
            child_exit = int(completed.returncode)
        except subprocess.TimeoutExpired as expired:
            output = decode_output(expired.stdout) + decode_output(expired.stderr)
            wrapper_exit = 124
            print(f"[timeout] {command_id} after {command.get('timeout_seconds')}s", file=sys.stderr)
        elapsed = time.monotonic() - started
        log_path = log_dir / f"{command_id}.log"
        log_path.write_text(
            f"# {command_id}\n# argv: {resolved_argv}\n# cwd: {cwd}\n"
            f"# wrapper_exit: {wrapper_exit}\n"
            f"# child_exit: {child_exit}\n\n{output}",
            encoding="utf-8",
            newline="\n",
        )
        results["commands"].append(
            {
                "id": command_id,
                "wrapper_exit": wrapper_exit,
                "child_exit": child_exit,
                "elapsed_seconds": round(elapsed, 1),
                "required": bool(command.get("required", True)),
                "log": str(log_path),
            }
        )
        write_results(results_path, results)
        if wrapper_exit != 0:
            return fail(f"{command_id} did not complete; see {log_path}")
        if child_exit != 0 and command.get("required", True):
            return fail(f"{command_id} failed with exit {child_exit}; see {log_path}")
        print(f"[ok] {command_id} (exit {child_exit}, {elapsed:.1f}s) -> {log_path}")
    write_results(results_path, results)
    print(f"[results] {results_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
