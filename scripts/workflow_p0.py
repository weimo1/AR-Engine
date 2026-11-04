"""Command-table verification for the P0 change.

This module turns ``workflow/p0-checks.json`` into the actual verification of the P0 slice:

* the table is schema-validated, so a free-form shell string, an unknown placeholder, a missing
  command, a bad dependency order or a non-declared exit field is rejected before anything runs;
* each command runs as an argument vector (never through a shell) in dependency order;
* every parser turns the command output into per-case facts instead of trusting an exit code:
  CTest JUnit results are compared against the cases the table declares as required, identity
  output is compared against the sidecar and the recorded dependency manifest, and compiler
  command lines are counted per flag;
* observed exit codes and timings land in ``<output>/logs/p0-checks/results.json``, which is an
  output, not a build input: the table itself stays a stable declaration.

The module never grants remote merge permission and never replaces the independent review.
"""

from __future__ import annotations

import hashlib
import json
import locale
import os
import re
import subprocess
import sys
import time
import xml.etree.ElementTree as ElementTree
from pathlib import Path

SCHEMA_VERSION = 1
PLACEHOLDERS = ("REPO", "OUT", "VCPKG_ROOT", "DEPS")
PARSERS = (
    "json-toolchain",
    "json-dependencies",
    "json-identity",
    "cmake-configure",
    "msbuild-log",
    "msbuild-cl",
    "ctest-junit",
)
CAPABILITY_SOURCES = ("system_loader", "fixture_loader", "fixture_vulkan_api", None)
COMMAND_FIELDS = {
    "id", "argv", "cwd", "timeout_seconds", "wrapper_exit", "child_exit", "parser_id",
    "required", "output_dir", "expected_capability_source", "depends_on", "expected_cases",
}
TABLE_FIELDS = {
    "schema_version", "change", "output_root", "output_root_exception", "snapshot",
    "snapshot_note", "commands",
}
MAX_TIMEOUT_SECONDS = 3600
NAME_PATTERN = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]*$")
IDENTITY_LINE = re.compile(r"^identity check: ok \(([0-9a-f]{64})\)\r?$", re.M)
HEX40 = re.compile(r"^[0-9a-f]{40}$")
HEX64 = re.compile(r"^[0-9a-f]{64}$")


class TableError(ValueError):
    """The command table itself is unusable; nothing may run."""


def canonical_bytes(value: object) -> bytes:
    """Same canonical JSON encoding the identity script uses for its hashes."""
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode("utf-8")


def load_table(path: Path) -> dict:
    if not path.is_file():
        raise TableError(f"no command table at {path}; verification is blocked until it exists")
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as error:
        raise TableError(f"command table is not valid JSON: {error}") from error
    validate_table(data)
    return data


def validate_table(data: object) -> None:
    if not isinstance(data, dict):
        raise TableError("command table must be a JSON object")
    unknown = set(data) - TABLE_FIELDS
    if unknown:
        raise TableError(f"unknown table field(s): {', '.join(sorted(unknown))}")
    if data.get("schema_version") != SCHEMA_VERSION:
        raise TableError(f"schema_version must be {SCHEMA_VERSION}")
    change = data.get("change")
    if not isinstance(change, str) or not NAME_PATTERN.match(change):
        raise TableError("change must name the change this table belongs to")
    commands = data.get("commands")
    if not isinstance(commands, list) or not commands:
        raise TableError("commands must be a non-empty array")

    seen: dict[str, int] = {}
    for index, command in enumerate(commands):
        validate_command(command, index, seen)
        seen[command["id"]] = index
    for index, command in enumerate(commands):
        for dependency in command["depends_on"]:
            if dependency not in seen:
                raise TableError(f"command {command['id']}: depends_on references unknown {dependency}")
            if seen[dependency] > index:
                raise TableError(
                    f"command {command['id']}: depends on {dependency}, which is declared later; "
                    "the table order must already be a valid dependency order")


def validate_command(command: object, index: int, seen: dict[str, int]) -> None:
    if not isinstance(command, dict):
        raise TableError(f"command #{index + 1}: must be an object")
    command_id = command.get("id")
    if not isinstance(command_id, str) or not NAME_PATTERN.match(command_id):
        raise TableError(f"command #{index + 1}: id must be a stable [A-Za-z0-9._-] name")
    if command_id in seen:
        raise TableError(f"duplicate command id {command_id}")
    unknown = set(command) - COMMAND_FIELDS
    if unknown:
        raise TableError(f"command {command_id}: unknown field(s): {', '.join(sorted(unknown))}")

    argv = command.get("argv")
    if (not isinstance(argv, list) or not argv
            or not all(isinstance(item, str) and item for item in argv)):
        raise TableError(
            f"command {command_id}: argv must be a non-empty array of strings; "
            "a single shell string is not accepted")
    if command.get("wrapper_exit") is not None or command.get("child_exit") is not None:
        raise TableError(
            f"command {command_id}: wrapper_exit/child_exit are declarations and must stay null; "
            "observed exits belong in results.json")
    timeout = command.get("timeout_seconds")
    if not isinstance(timeout, int) or isinstance(timeout, bool) or not 0 < timeout <= MAX_TIMEOUT_SECONDS:
        raise TableError(
            f"command {command_id}: timeout_seconds must be an integer in 1..{MAX_TIMEOUT_SECONDS}")
    parser_id = command.get("parser_id")
    if parser_id not in PARSERS:
        raise TableError(f"command {command_id}: parser_id {parser_id!r} is not implemented")
    if not isinstance(command.get("required"), bool):
        raise TableError(f"command {command_id}: required must be a boolean")
    if command.get("expected_capability_source") not in CAPABILITY_SOURCES:
        raise TableError(
            f"command {command_id}: expected_capability_source must be null or one of "
            f"{', '.join(source for source in CAPABILITY_SOURCES if source)}")
    cwd = command.get("cwd", "${REPO}")
    if not isinstance(cwd, str) or not cwd:
        raise TableError(f"command {command_id}: cwd must be a non-empty string")
    output_dir = command.get("output_dir", "${OUT}")
    if not isinstance(output_dir, str) or not output_dir:
        raise TableError(f"command {command_id}: output_dir must be a non-empty string")
    depends_on = command.get("depends_on", [])
    if not isinstance(depends_on, list) or not all(isinstance(item, str) for item in depends_on):
        raise TableError(f"command {command_id}: depends_on must be an array of command ids")
    if command_id in depends_on:
        raise TableError(f"command {command_id}: must not depend on itself")
    if len(set(depends_on)) != len(depends_on):
        raise TableError(f"command {command_id}: depends_on has duplicates")
    for item in argv + [cwd, output_dir]:
        for name in re.findall(r"\$\{([^}]*)\}", item):
            if name not in PLACEHOLDERS:
                raise TableError(
                    f"command {command_id}: unknown placeholder ${{{name}}}; "
                    f"only {', '.join(PLACEHOLDERS)} are expanded")
    expected_cases = command.get("expected_cases")
    if expected_cases is not None:
        if parser_id != "ctest-junit":
            raise TableError(f"command {command_id}: expected_cases is only meaningful for ctest-junit")
        if (not isinstance(expected_cases, list) or not expected_cases
                or len(set(expected_cases)) != len(expected_cases)
                or not all(isinstance(item, str) and NAME_PATTERN.match(item) for item in expected_cases)):
            raise TableError(f"command {command_id}: expected_cases must be unique test-case names")
    elif parser_id == "ctest-junit":
        raise TableError(
            f"command {command_id}: ctest-junit requires the expected_cases it must cover")


def substitutions(repo_root: Path, output_root: Path, environment: dict[str, str]) -> dict[str, str]:
    values = {
        "REPO": repo_root.as_posix(),
        "OUT": output_root.as_posix(),
        "VCPKG_ROOT": environment.get("VCPKG_ROOT", ""),
        "DEPS": environment.get("AR_P0_DEPS", ""),
    }
    missing = [name for name in ("VCPKG_ROOT", "DEPS") if not values[name]]
    if missing:
        hint = ", ".join("VCPKG_ROOT" if name == "VCPKG_ROOT" else "AR_P0_DEPS" for name in missing)
        raise TableError(f"set {hint} (see CMakeUserPresets.json for the local values)")
    return values


def expand(value: str, values: dict[str, str]) -> str:
    expanded = value
    for name, replacement in values.items():
        expanded = expanded.replace("${" + name + "}", replacement)
    leftover = re.findall(r"\$\{[^}]*\}", expanded)
    if leftover:
        raise TableError(f"unexpanded placeholder(s): {', '.join(sorted(set(leftover)))}")
    return expanded


def sanitized_environment() -> dict[str, str]:
    """Drop case-duplicated environment keys.

    Windows can hold both ``NO_PROXY`` and ``no_proxy``; MSBuild builds a Hashtable from the
    environment and fails with MSB6001 ("已添加项") on the second spelling, which surfaces as
    "No CMAKE_CXX_COMPILER could be found". Keeping the upper-case spelling is enough.
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


def decode_output(raw: bytes | None) -> str:
    """Decode child output line by line so one non-UTF-8 line cannot mangle a whole log."""
    if raw is None:
        return ""
    fallback = locale.getpreferredencoding(False)
    lines = []
    for line in raw.split(b"\n"):
        try:
            lines.append(line.decode("utf-8"))
        except UnicodeDecodeError:
            lines.append(line.decode(fallback, "replace"))
    return "\n".join(lines)


def write_results(results_path: Path, results: dict) -> None:
    encoded = json.dumps(results, indent=2, ensure_ascii=False) + "\n"
    if not results_path.exists() or results_path.read_text(encoding="utf-8") != encoded:
        results_path.write_text(encoded, encoding="utf-8", newline="\n")


def _flag_after(argv: list[str], flag: str) -> str | None:
    for index, item in enumerate(argv):
        if item == flag and index + 1 < len(argv):
            return argv[index + 1]
        if item.startswith(flag + "="):
            return item.split("=", 1)[1]
    return None


def _read_json(path: Path, label: str) -> dict:
    if not path.is_file():
        raise ValueError(f"{label}: no JSON at {path}")
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except json.JSONDecodeError as error:
        raise ValueError(f"{label}: {path} is not valid JSON: {error}") from error
    if not isinstance(data, dict):
        raise ValueError(f"{label}: {path} must hold a JSON object")
    return data


def parse_json_toolchain(command: dict, argv: list[str], log: str, root: Path) -> dict:
    path = _flag_after(argv, "--output")
    if not path:
        raise ValueError(f"{command['id']}: command does not declare --output")
    data = _read_json(Path(path), command["id"])
    required = ("schema_version", "vs_instance", "toolset", "cl_version", "cmake_version",
                "generator", "windows_sdk", "host_arch", "target_arch", "triplet",
                "triplet_file_sha256", "crt", "vcpkg_tool_commit")
    missing = [key for key in required if key not in data]
    if missing:
        raise ValueError(f"{command['id']}: toolchain record is missing {', '.join(missing)}")
    if data["schema_version"] != 1:
        raise ValueError(f"{command['id']}: toolchain schema_version must be 1")
    declared_triplet = _flag_after(argv, "--triplet")
    if declared_triplet and data["triplet"] != declared_triplet:
        raise ValueError(
            f"{command['id']}: toolchain triplet {data['triplet']!r} does not match {declared_triplet!r}")
    if data["host_arch"] != "x64" or data["target_arch"] != "x64":
        raise ValueError(f"{command['id']}: toolchain must be x64 host and target")
    crt = data["crt"]
    if not isinstance(crt, dict) or crt.get("debug") != "/MDd" or crt.get("optimized") != "/MD":
        raise ValueError(f"{command['id']}: CRT record must be /MDd (Debug) and /MD (optimized)")
    if not HEX64.match(str(data["triplet_file_sha256"])):
        raise ValueError(f"{command['id']}: triplet_file_sha256 is not a SHA256")
    if not HEX40.match(str(data["vcpkg_tool_commit"])):
        raise ValueError(f"{command['id']}: vcpkg_tool_commit is not a 40-hex commit")
    return {"toolset": data["toolset"], "cl_version": data["cl_version"],
            "cmake_version": data["cmake_version"], "windows_sdk": data["windows_sdk"],
            "triplet": data["triplet"], "crt": crt}


def parse_json_dependencies(command: dict, argv: list[str], log: str, root: Path) -> dict:
    path = _flag_after(argv, "--output")
    if not path:
        raise ValueError(f"{command['id']}: command does not declare --output")
    data = _read_json(Path(path), command["id"])
    for key in ("schema_version", "manifest_sha256", "triplet", "vcpkg_tool_commit",
                "registry_baseline", "resolved"):
        if key not in data:
            raise ValueError(f"{command['id']}: dependency record is missing {key}")
    if data["schema_version"] != 1:
        raise ValueError(f"{command['id']}: dependency schema_version must be 1")
    declared_triplet = _flag_after(argv, "--triplet")
    if declared_triplet and data["triplet"] != declared_triplet:
        raise ValueError(
            f"{command['id']}: dependency triplet {data['triplet']!r} does not match {declared_triplet!r}")
    snapshot = root
    declared_snapshot = _flag_after(argv, "--snapshot")
    if declared_snapshot:
        snapshot = Path(declared_snapshot)
    manifest = snapshot / "vcpkg.json"
    if not manifest.is_file():
        raise ValueError(f"{command['id']}: no manifest at {manifest}")
    actual = hashlib.sha256(manifest.read_bytes()).hexdigest()
    if actual != data["manifest_sha256"]:
        raise ValueError(
            f"{command['id']}: vcpkg.json changed after the dependency record was taken")
    resolved = data["resolved"]
    if not isinstance(resolved, list) or not resolved:
        raise ValueError(f"{command['id']}: resolved must list the installed ports")
    names = [item.get("name") for item in resolved]
    if names != sorted(names) or len(set(names)) != len(names):
        raise ValueError(f"{command['id']}: resolved ports must be unique and sorted by name")
    for item in resolved:
        if item.get("triplet") != data["triplet"]:
            raise ValueError(
                f"{command['id']}: port {item.get('name')} is recorded for {item.get('triplet')!r}")
        if not isinstance(item.get("features"), list):
            raise ValueError(f"{command['id']}: port {item.get('name')} has no feature list")
    if not HEX40.match(str(data["registry_baseline"])):
        raise ValueError(f"{command['id']}: registry_baseline is not a 40-hex baseline")
    return {"triplet": data["triplet"], "registry_baseline": data["registry_baseline"],
            "ports": names}


def parse_json_identity(command: dict, argv: list[str], log: str, root: Path) -> dict:
    if len(argv) < 3 or argv[2] not in ("prepare", "check"):
        raise ValueError(f"{command['id']}: expected 'prepare' or 'check' as the identity subcommand")
    output = _flag_after(argv, "--output")
    configuration = _flag_after(argv, "--configuration")
    if not output or not configuration:
        raise ValueError(f"{command['id']}: identity command needs --output and --configuration")
    sidecar_path = Path(output) / "identity" / configuration / "build-identity.json"
    sidecar = _read_json(sidecar_path, command["id"])
    if sidecar.get("configuration") != configuration:
        raise ValueError(
            f"{command['id']}: sidecar configuration {sidecar.get('configuration')!r} "
            f"does not match {configuration!r}")
    if not HEX64.match(str(sidecar.get("build_id"))) or not HEX64.match(str(sidecar.get("input_hash"))):
        raise ValueError(f"{command['id']}: sidecar build_id/input_hash are not SHA256 values")
    source = sidecar.get("source")
    if not isinstance(source, dict) or "revision" not in source or not isinstance(source.get("dirty"), bool):
        raise ValueError(f"{command['id']}: sidecar source must carry revision and a boolean dirty")
    dependency_record = _flag_after(argv, "--dependency-record")
    if argv[2] == "prepare" and dependency_record:
        dependencies = _read_json(Path(dependency_record), command["id"])
        expected = hashlib.sha256(canonical_bytes(dependencies)).hexdigest()
        if sidecar.get("dependency_manifest_hash") != expected:
            raise ValueError(
                f"{command['id']}: dependency_manifest_hash does not match the dependency record")
    if argv[2] == "check":
        match = IDENTITY_LINE.search(log)
        if not match:
            raise ValueError(f"{command['id']}: output has no successful 'identity check: ok' line")
        if match.group(1) != sidecar.get("build_id"):
            raise ValueError(f"{command['id']}: checked build_id differs from the sidecar")
        return {"mode": "check", "build_id": sidecar["build_id"],
                "input_hash": sidecar["input_hash"], "configuration": configuration,
                "source": source}
    return {"mode": "prepare", "build_id": sidecar["build_id"],
            "input_hash": sidecar["input_hash"], "configuration": configuration,
            "source": source}


def parse_cmake_configure(command: dict, argv: list[str], log: str, root: Path) -> dict:
    configuration = _flag_after(argv, "-DAR_EXPECTED_CONFIGURATION")
    if not configuration:
        raise ValueError(f"{command['id']}: configure does not pin -DAR_EXPECTED_CONFIGURATION")
    if "CMake Error" in log:
        first = log.split("CMake Error", 1)[1][:400].strip()
        raise ValueError(f"{command['id']}: configure reported CMake Error: {first}")
    for marker in ("C++23 language mode verified", "compiled build identity", "Configuring done"):
        if marker not in log:
            raise ValueError(f"{command['id']}: configure log lacks {marker!r}")
    return {"configuration": configuration}


def parse_msbuild_log(command: dict, argv: list[str], log: str, root: Path) -> dict:
    broken = [pattern for pattern in ("CMake Error", "error C", "error LNK", "error MSB",
                                      "fatal error") if pattern in log]
    if broken:
        raise ValueError(f"{command['id']}: build log reports {', '.join(broken)}")
    return {}


def parse_msbuild_cl(command: dict, argv: list[str], log: str, root: Path) -> dict:
    configuration = _flag_after(argv, "--config")
    if not configuration:
        raise ValueError(f"{command['id']}: build command does not name a configuration")
    counts = {
        "preview": log.count("/std:c++23preview"),
        "latest": log.count("/std:c++latest"),
        "ehsc": log.count("/EHsc"),
        "c4530": log.count("C4530"),
        "mdd": len(re.findall(r"/MDd(?![d\w])", log)),
        "md": len(re.findall(r"/MD(?![d\w])", log)),
        "mt": len(re.findall(r"/MT(?![d\w])", log)),
        "errors": len(re.findall(r"\berror C\d{4}\b", log)),
        "warnings": len(re.findall(r"\bwarning C\d{4}\b", log)),
    }
    problems = []
    if counts["preview"] < 1:
        problems.append("no /std:c++23preview in any compiler invocation")
    if counts["latest"]:
        problems.append(f"/std:c++latest appears {counts['latest']} time(s)")
    if counts["ehsc"] < 1:
        problems.append("no /EHsc in any compiler invocation")
    if counts["c4530"]:
        problems.append(f"C4530 appears {counts['c4530']} time(s)")
    if counts["mt"]:
        problems.append("/MT appears; the runtime must be dynamic")
    if counts["errors"]:
        problems.append(f"{counts['errors']} compiler error(s)")
    if counts["warnings"]:
        problems.append(f"{counts['warnings']} compiler warning(s)")
    if configuration == "Debug":
        if counts["md"]:
            problems.append("Debug build mixes /MD into a /MDd configuration")
        expected_crt = "/MDd"
    else:
        if counts["mdd"]:
            problems.append(f"{configuration} build mixes /MDd into an optimized configuration")
        expected_crt = "/MD"
    if not counts["mdd"] and not counts["md"]:
        problems.append(f"no {expected_crt} in any compiler invocation")
    if problems:
        raise ValueError(f"{command['id']}: " + "; ".join(problems))
    return {"configuration": configuration, "counts": counts, "crt": expected_crt}


def parse_ctest_junit(command: dict, argv: list[str], log: str, root: Path) -> dict:
    junit_path = _flag_after(argv, "--output-junit")
    if not junit_path:
        raise ValueError(f"{command['id']}: ctest command does not declare --output-junit")
    junit = Path(junit_path)
    if not junit.is_absolute():
        junit = root / junit
    if not junit.is_file():
        raise ValueError(f"{command['id']}: no JUnit result at {junit}")
    try:
        tree = ElementTree.parse(junit)
    except ElementTree.ParseError as error:
        raise ValueError(f"{command['id']}: JUnit result is not valid XML: {error}") from error
    cases: dict[str, str] = {}
    for element in tree.iter("testcase"):
        name = element.get("name")
        if not name:
            raise ValueError(f"{command['id']}: JUnit result has a testcase without a name")
        if name in cases:
            raise ValueError(f"{command['id']}: JUnit result lists {name} twice")
        if element.find("failure") is not None or element.find("error") is not None:
            cases[name] = "failed"
        elif element.find("skipped") is not None:
            cases[name] = "skipped"
        else:
            cases[name] = "passed"
    expected = list(command["expected_cases"])
    missing = [name for name in expected if name not in cases]
    if missing:
        raise ValueError(
            f"{command['id']}: required case(s) missing from the run: {', '.join(missing)}; "
            "a missing or skipped case is not a pass")
    wrong = {name: cases[name] for name in expected if cases[name] != "passed"}
    if wrong:
        detail = ", ".join(f"{name}={state}" for name, state in sorted(wrong.items()))
        raise ValueError(f"{command['id']}: required case(s) did not pass: {detail}")
    unexpected = sorted(set(cases) - set(expected))
    if unexpected:
        raise ValueError(
            f"{command['id']}: ran case(s) the table does not declare: {', '.join(unexpected)}; "
            "update expected_cases with the intended set")
    return {"cases": {name: cases[name] for name in expected}, "junit": junit.as_posix()}


PARSER_FUNCTIONS = {
    "json-toolchain": parse_json_toolchain,
    "json-dependencies": parse_json_dependencies,
    "json-identity": parse_json_identity,
    "cmake-configure": parse_cmake_configure,
    "msbuild-log": parse_msbuild_log,
    "msbuild-cl": parse_msbuild_cl,
    "ctest-junit": parse_ctest_junit,
}


def run_table(table: dict, repo_root: Path, output_root: Path, environment: dict[str, str],
              only: list[str] | None = None) -> dict:
    """Execute the table and return the evidence the runners persist."""
    values = substitutions(repo_root, output_root, environment)
    log_dir = output_root / "logs" / "p0-checks"
    log_dir.mkdir(parents=True, exist_ok=True)
    results_path = log_dir / "results.json"
    commands = table["commands"]
    if only is not None:
        known = {command["id"] for command in commands}
        unknown = sorted(set(only) - known)
        if unknown:
            raise TableError(f"unknown command id(s): {', '.join(unknown)}")
        commands = [command for command in commands if command["id"] in set(only)]

    results: dict = {
        "schema_version": SCHEMA_VERSION,
        "change": table["change"],
        "output_root": output_root.as_posix(),
        "commands": [],
    }
    child_environment = sanitized_environment()
    for command in commands:
        command_id = command["id"]
        argv = [expand(str(item), values) for item in command["argv"]]
        cwd = expand(str(command.get("cwd", "${REPO}")), values)
        cwd_path = Path(cwd)
        if not cwd_path.is_dir():
            raise TableError(f"command {command_id}: cwd {cwd} does not exist")
        if not _is_within(cwd_path.resolve(), repo_root.resolve()):
            raise TableError(f"command {command_id}: cwd {cwd} resolves outside the repository")
        started = time.monotonic()
        wrapper_exit = 0
        child_exit: int | None = None
        output = ""
        try:
            completed = subprocess.run(
                argv, cwd=cwd, env=child_environment, capture_output=True, text=False,
                timeout=float(command["timeout_seconds"]), check=False)
            output = decode_output(completed.stdout) + decode_output(completed.stderr)
            child_exit = int(completed.returncode)
        except subprocess.TimeoutExpired as expired:
            output = decode_output(expired.stdout) + decode_output(expired.stderr)
            wrapper_exit = 124
        elapsed = round(time.monotonic() - started, 1)
        log_path = log_dir / f"{command_id}.log"
        log_path.write_text(
            f"# {command_id}\n# argv: {argv}\n# cwd: {cwd}\n"
            f"# wrapper_exit: {wrapper_exit}\n# child_exit: {child_exit}\n\n{output}",
            encoding="utf-8", newline="\n")

        detail: dict = {}
        passed = wrapper_exit == 0 and child_exit == 0
        reason = ""
        if wrapper_exit == 124:
            reason = f"timeout after {command['timeout_seconds']}s"
        elif child_exit not in (0, None):
            reason = f"exit {child_exit}"
        if passed:
            try:
                detail = PARSER_FUNCTIONS[command["parser_id"]](command, argv, output, repo_root)
            except ValueError as error:
                passed, reason = False, str(error)
        entry = {
            "id": command_id,
            "wrapper_exit": wrapper_exit,
            "child_exit": child_exit,
            "elapsed_seconds": elapsed,
            "required": command["required"],
            "parser_id": command["parser_id"],
            "log": log_path.as_posix(),
            "passed": passed,
        }
        if reason:
            entry["reason"] = reason
        if detail:
            entry["detail"] = detail
        results["commands"].append(entry)
        if not passed and command["required"]:
            results["status"] = "failed"
            write_results(results_path, results)
            raise ValueError(f"command {command_id} {reason}; see {log_path}")
        if not passed:
            print(f"[skip] {command_id}: {reason}", file=sys.stderr)
    results["status"] = "passed"
    results["results_path"] = results_path.as_posix()
    write_results(results_path, results)
    return results


def _is_within(path: Path, parent: Path) -> bool:
    try:
        path.relative_to(parent)
        return True
    except ValueError:
        return False
