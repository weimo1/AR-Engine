"""Local execution and failure gates. Does not authenticate remote approvals."""
from __future__ import annotations

from contextlib import contextmanager
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import struct
import sys
import uuid

import workflow as core


def now() -> str:
    return datetime.now(timezone.utc).isoformat()


def process_alive(pid: int) -> bool:
    if os.name == "nt":
        import ctypes
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.OpenProcess.restype = ctypes.c_void_p
        kernel.GetExitCodeProcess.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_ulong)]
        kernel.CloseHandle.argtypes = [ctypes.c_void_p]
        handle = kernel.OpenProcess(0x1000, False, pid)
        if not handle:
            return ctypes.get_last_error() == 5  # Unknown access is not permission to steal.
        try:
            code = ctypes.c_ulong()
            return not kernel.GetExitCodeProcess(handle, ctypes.byref(code)) or code.value == 259
        finally:
            kernel.CloseHandle(handle)
    try:
        os.kill(pid, 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True


@contextmanager
def operation_lock(root: Path, recover: bool = False):
    # Kernel ownership is released on process death. It also serializes two
    # recovery callers, so neither can unlink a newly acquired record lock.
    guard = core.within(root, root / ".ar-workflow/operation.guard")
    guard.parent.mkdir(parents=True, exist_ok=True)
    with guard.open("a+b") as handle:
        if guard.stat().st_size == 0:
            handle.write(b"\0")
            handle.flush()
        handle.seek(0)
        try:
            if os.name == "nt":
                import msvcrt
                msvcrt.locking(handle.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as error:
            raise ValueError("Another operation holds the workspace kernel lock") from error
        try:
            with record_lock(root, recover):
                yield
        finally:
            handle.seek(0)
            if os.name == "nt":
                msvcrt.locking(handle.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                fcntl.flock(handle.fileno(), fcntl.LOCK_UN)


@contextmanager
def record_lock(root: Path, recover: bool = False):
    path = core.within(root, root / ".ar-workflow/operation.lock")
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.exists():
        owner = core.load(path)
        if not recover or process_alive(int(owner["pid"])):
            raise ValueError("Another operation or interrupted lock exists; use resume after the owner exits")
        path.unlink()  # Exact task-owned stale lock, never recursive cleanup.
    nonce = uuid.uuid4().hex
    with path.open("x", encoding="utf-8") as handle:
        json.dump({"pid": os.getpid(), "nonce": nonce, "created_at": now()}, handle)
    try:
        yield
    finally:
        try:
            if path.exists() and core.load(path).get("nonce") == nonce:
                path.unlink()
        except (OSError, ValueError):
            print("Operation lock changed externally; preserved without masking operation result", file=sys.stderr)


def validate_native(raw: dict, expected: list[str], tool: dict, rule_sha: str) -> tuple[list[str], list[dict]]:
    """Fail closed on unknown native schemas, partial results and any unresolved finding."""
    errors, findings = [], []
    if not isinstance(raw, dict):
        return ["OCR JSON is not an object"], []
    manifest = raw.get("manifest", {})
    if not isinstance(manifest, dict):
        return ["OCR manifest is not an object"], []
    for key in ("input", "execution", "coverage"):
        if not isinstance(manifest.get(key), dict):
            return [f"missing/malformed OCR manifest {key}"], []
    for key in ("summary", "tool_calls"):
        if not isinstance(raw.get(key), dict):
            return [f"missing/malformed OCR {key}"], []
    if manifest.get("schema_version") != "ocr.run-manifest/v1":
        errors.append("unknown/missing OCR manifest schema")
    if (raw.get("status") != "complete" or manifest.get("terminal_state") != "complete"
            or manifest.get("operation") != "review" or manifest.get("run_failure")):
        errors.append("OCR run is incomplete, failed or not a diff review")
    if manifest.get("input", {}).get("mode") != "workspace":
        errors.append("unexpected OCR input mode")
    execution = manifest.get("execution", {})
    for field, value in (("provider", tool["api_provider"]), ("model", tool["api_model"]),
                         ("ocr_version", "v" + tool["version"]), ("rule_config_sha256", rule_sha)):
        if execution.get(field) != value:
            errors.append(f"OCR execution identity mismatch: {field}")
    if not manifest.get("run_id") or not manifest.get("input", {}).get("source_artifact_sha256"):
        errors.append("missing OCR run/input identity")
    coverage = manifest.get("coverage", {})
    sets = {}
    for key in ("selected", "completed", "reused", "failed", "waived"):
        items = coverage.get(key)
        if not isinstance(items, list) or any(not isinstance(item, dict) for item in items):
            errors.append(f"missing/malformed OCR coverage: {key}")
            sets[key] = []
        else:
            if any(not all(isinstance(item.get(field), str) and item[field]
                           for field in ("path", "item_id", "fingerprint")) for item in items):
                errors.append(f"malformed OCR coverage identity: {key}")
                sets[key] = []
            else:
                sets[key] = items
    selected, completed = sets["selected"], sets["completed"]
    selected_paths = [item.get("path") for item in selected]
    completed_paths = [item.get("path") for item in completed]
    if (not expected or len(set(expected)) != len(expected) or sorted(selected_paths) != sorted(expected)
            or sorted(completed_paths) != sorted(expected)):
        errors.append("OCR selected/completed coverage does not equal required file set")
    def identities(items):
        return [(item.get("item_id"), item.get("path"), item.get("fingerprint")) for item in items]
    if (any(not all(item) for item in identities(selected))
            or len({item.get("item_id") for item in selected}) != len(selected)
            or sorted(identities(selected)) != sorted(identities(completed))):
        errors.append("OCR coverage item identities disagree or duplicate")
    fields = [value for item in sorted(selected, key=lambda item: item["item_id"])
              for value in (item["item_id"], item["fingerprint"])]
    if manifest["input"].get("source_artifact_sha256") != native_fields_sha(fields):
        errors.append("OCR source artifact digest disagrees with selected items")
    if any(sets[key] for key in ("reused", "failed", "waived")):
        errors.append("OCR reuse, failure or waiver requires explicit support; not accepted")
    if raw.get("warnings") or raw.get("summary", {}).get("budget_exceeded"):
        errors.append("OCR emitted warnings or exhausted budget")
    if raw.get("tool_calls", {}).get("failure", 0):
        errors.append("OCR tool execution failed")
    comments = raw.get("comments")
    if not isinstance(comments, list):
        errors.append("missing OCR comments array")
    else:
        for index, comment in enumerate(comments):
            if (not isinstance(comment, dict) or comment.get("path") not in expected
                    or not comment.get("content") or comment.get("severity") not in ("critical", "high", "medium", "low")):
                errors.append("malformed or out-of-scope OCR finding")
                continue
            findings.append({"id": f"OCR-{index + 1:03}", "path": comment["path"],
                             "line": comment.get("start_line"), "content": comment["content"],
                             "severity": comment["severity"], "category": comment.get("category"),
                             "level": "blocking" if comment["severity"] in ("critical", "high") else "must_fix",
                             "status": "open"})
    if raw.get("summary", {}).get("comments") != len(comments or []):
        errors.append("OCR summary comment count mismatch")
    return errors, findings


def native_fields_sha(fields: list[str]) -> str:
    framed = b"".join(struct.pack(">Q", len(value.encode("utf-8"))) + value.encode("utf-8") for value in fields)
    return core.digest(framed)


def expected_rule_sha(snapshot: Path) -> str:
    tool = core.load(snapshot / "workflow/tool-lock.json")["ocr"]
    identity = tool.get("rule_identity", {})
    actual = core.digest(core.canonical(core.load(snapshot / ".opencodereview/rule.json")))
    if (identity.get("project_json_sha256") != actual
            or not re.fullmatch(r"[0-9a-f]{64}", identity.get("native_sha256", ""))):
        raise ValueError("Project rules changed or native identity is not pinned; update the reviewed rule/tool lock")
    return identity["native_sha256"]


def native_report(packet: Path, raw_path: Path, returncode: int) -> dict:
    manifest = core.load(packet / "manifest.json")
    errors = core.validate_packet(packet, manifest)
    snapshot = packet / "snapshot"
    tool = core.load(snapshot / "workflow/tool-lock.json")["ocr"]
    raw = core.load(raw_path)
    native_errors, findings = validate_native(raw, manifest["reviewable_files"], tool,
                                              expected_rule_sha(snapshot))
    errors.extend(native_errors)
    if isinstance(raw, dict) and isinstance(raw.get("manifest"), dict):
        actual = raw["manifest"]
        if manifest["state"]["head"] and actual.get("input", {}).get("resolved_base") != manifest["state"]["head"]:
            errors.append("Native review base differs from frozen HEAD")
    if returncode != 0:
        errors.append(f"OCR process exit: {returncode}")
    return {"schema_version": 1, "fingerprint": manifest["fingerprint"],
            "reviewer": f"OCR {tool['version']} / {tool['api_provider']} / {tool['api_model']}",
            "independent_context": True, "status": "partial" if errors else "complete",
            "decision": "pass" if not errors and not findings else "request_changes",
            "reviewed_files": manifest["reviewable_files"] if not native_errors else [],
            "findings": findings, "errors": errors,
            "native_sha256": core.digest(raw_path.read_bytes()), "created_at": now(),
            "limitations": ["Local provenance is not remote identity authentication", "No findings is not proof of correctness"]}


def review_run(root: Path, packet: Path, budget: int = 100000, seconds: int = 300) -> Path:
    if not 1 <= budget <= 1000000 or not 1 <= seconds <= 1800:
        raise ValueError("Review requires a bounded token budget (1..1000000) and timeout (1..1800 seconds)")
    manifest = core.load(packet / "manifest.json")
    errors = core.validate_packet(packet, manifest)
    if errors or manifest["state"] != core.input_state(root, manifest.get("scope", "workspace")):
        raise ValueError("Review packet invalid or source changed: " + "; ".join(errors))
    previous = packet / "ocr-run.json"
    if previous.exists() and core.load(previous).get("status") == "running":
        if process_alive(int(core.load(previous)["pid"])):
            raise ValueError("The recorded OCR process is still running; do not duplicate API work")
        raise ValueError("OCR was interrupted; preserve its evidence and explicitly prepare a new run")
    run_id = uuid.uuid4().hex[:12]
    output = packet / f"api-{run_id}"
    output.mkdir()
    snapshot = packet / "snapshot"
    expected_rule_sha(snapshot)  # Offline check before any paid planner/review call.
    tool = core.load(snapshot / "workflow/tool-lock.json")["ocr"]
    env = core.snapshot_env(packet)
    raw_path = output / "native.json"
    args = [str(core.ocr_binary(root)), "review", "--repo", str(snapshot),
            "--rule", str(snapshot / ".opencodereview/rule.json"),
            "--provider", tool["api_provider"], "--model", tool["api_model"],
            "--concurrency", "1", "--max-git-procs", "2", "--effort", "low", "--no-filter",
            "--timeout", "3", "--max-tokens-budget", str(budget), "--format", "json", "--output", str(raw_path)]
    with (output / "stdout.log").open("wb") as stdout, (output / "stderr.log").open("wb") as stderr:
        process = subprocess.Popen(args, cwd=snapshot, env=env, stdout=stdout, stderr=stderr)
        record = {"status": "running", "pid": process.pid, "output": str(output), "started_at": now(),
                  "fingerprint": manifest["fingerprint"], "token_budget": budget}
        core.save(previous, record)
        print(f"OCR running: {output}", flush=True)
        try:
            code = process.wait(timeout=seconds)
        except (subprocess.TimeoutExpired, KeyboardInterrupt):
            if process.poll() is None:
                if os.name == "nt":
                    subprocess.run(["taskkill", "/PID", str(process.pid), "/T", "/F"], capture_output=True, timeout=20)
                else:
                    process.kill()
                process.wait(timeout=20)
            core.save(previous, record | {"status": "interrupted", "finished_at": now()})
            raise ValueError("OCR interrupted; evidence retained, no review approval")
    finished = record | {"status": "finished", "returncode": code, "finished_at": now()}
    core.save(previous, finished)
    core.save(output / "run.json", finished)
    if not raw_path.exists():
        raise ValueError(f"OCR did not emit a manifest; inspect {output}")
    report = native_report(packet, raw_path, code)
    if manifest["state"] != core.input_state(root, manifest.get("scope", "workspace")):
        report["status"], report["decision"] = "partial", "request_changes"
        report["errors"].append("source changed during review")
    report_path = output / "report.json"
    core.save(report_path, report)
    print(f"OCR result: {report['status']} / {report['decision']}; {report_path}", flush=True)
    return report_path


def check_commands(tools_root: Path) -> list[list[str]]:
    return [[sys.executable, "scripts/workflow.py", "check"],
                [sys.executable, "-m", "unittest", "discover", "-s", "scripts/tests", "-v"],
                ["node", str(tools_root / "node_modules/@fission-ai/openspec/bin/openspec.js"),
                 "validate", "--all", "--strict", "--no-interactive", "--json"]]


def check_output(number: int, stdout: bytes, stderr: bytes, root: Path) -> dict:
    if number == 1:
        output = (stdout + stderr).decode("utf-8", errors="replace")
        found = re.findall(r"^Ran (\d+) tests? in ", output, re.M)
        skipped = re.search(r"^OK \(skipped=(\d+)\)", output, re.M)
        count = int(found[0]) if len(found) == 1 else 0
        if count <= (int(skipped[1]) if skipped else 0) or not re.search(r"^OK(?:\s|$)", output, re.M):
            raise ValueError("No executed, passing unittest cases; an empty/skipped suite is not verification")
        return {"tests_run": count}
    if number == 2:
        parsed = json.loads(stdout)
        items = parsed.get("items", [])
        expected = {( "change", path.parent.name) for path in (root / "openspec/changes").glob("*/proposal.md")}
        expected.update(("spec", path.parent.name) for path in (root / "openspec/specs").glob("*/spec.md"))
        actual = {(item.get("type"), item.get("id")) for item in items}
        totals = parsed.get("summary", {}).get("totals", {})
        if (parsed.get("version") != "1.0" or not expected or actual != expected or len(items) != len(expected)
                or not all(item.get("valid") is True and not item.get("issues") for item in items)
                or totals != {"items": len(items), "passed": len(items), "failed": 0}):
            raise ValueError("OpenSpec structured result does not cover all expected changes/specs successfully")
        return {"validated_items": sorted(expected)}
    return {}


def checks(root: Path, tools_root: Path, output: Path) -> dict:
    tools_root = Path(os.environ.get("AR_WORKFLOW_TOOLS_ROOT", str(tools_root))).resolve()
    output.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, AR_WORKFLOW_TOOLS_ROOT=str(tools_root), PYTHONDONTWRITEBYTECODE="1",
               OPENSPEC_TELEMETRY="0", OPEN_SPEC_INTERACTIVE="0", OCR_ENABLE_TELEMETRY="0", OCR_NO_UPDATE="1")
    commands = check_commands(tools_root)
    results = []
    owned_gpu_sources = []
    for directory, directories, files in os.walk(root):
        directories[:] = [name for name in directories if name not in {
            ".git", ".tools", ".ar-workflow", "node_modules", ".agents", "docs", "openspec",
            "third_party", "vcpkg_installed", "__pycache__"}]
        owned_gpu_sources.extend(Path(directory) / name for name in files
                                 if Path(name).suffix.lower() in (".cpp", ".h", ".hpp", ".glsl", ".vert", ".frag", ".comp"))
    if owned_gpu_sources:
        raise ValueError("Owned C++/Shader sources require actual build/Shader/GPU check contracts; not configured yet")
    for number, command in enumerate(commands):
        try:
            result = subprocess.run(command, cwd=root, env=env, capture_output=True, timeout=180)
            log = result.stdout + result.stderr
            (output / f"check-{number}.log").write_bytes(log)
            passed = result.returncode == 0 and b"could not parse" not in log.lower()
            detail = {}
            try:
                if passed:
                    detail = check_output(number, result.stdout, result.stderr, root)
            except (ValueError, KeyError, TypeError) as error:
                passed, detail = False, {"reason": str(error)}
            results.append({"command": command, "exit": result.returncode, "passed": passed,
                            "log_sha256": core.digest(log), **detail})
        except subprocess.TimeoutExpired:
            results.append({"command": command, "passed": False, "reason": "timeout"})
        if not results[-1]["passed"]:
            break
    evidence = {"status": "passed" if len(results) == len(commands) and all(r["passed"] for r in results) else "failed",
                "created_at": now(), "checks": results, "limitations": ["Workflow checks only; no engine/GPU verification"]}
    core.save(output / "checks.json", evidence)
    if evidence["status"] != "passed":
        detail = (output / f"check-{len(results) - 1}.log")
        tail = detail.read_text(encoding="utf-8", errors="replace")[-2000:] if detail.exists() else "timeout"
        raise ValueError(f"Checks failed; inspect {output}\n{tail}")
    return evidence


def verification(root: Path, packet: Path) -> Path:
    manifest = core.load(packet / "manifest.json")
    if core.validate_packet(packet, manifest):
        raise ValueError("Invalid frozen input")
    output = packet / ("verify-" + uuid.uuid4().hex[:12])
    source = packet / "snapshot"
    if manifest.get("review_artifacts"):
        # Verification never sees historical deleted source or review-only data.
        source = output / "source"
        shutil.copytree(packet / "snapshot", source,
                        ignore=lambda directory, names: [".ar-review-context"]
                        if Path(directory) == packet / "snapshot" else [])
    result = checks(source, root, output)
    if (core.validate_packet(packet, manifest)
            or manifest["state"] != core.input_state(root, manifest.get("scope", "workspace"))):
        raise ValueError("Input changed during checks")
    result["fingerprint"] = manifest["fingerprint"]
    core.save(output / "verification.json", result)
    return output / "verification.json"


def task_path(root: Path, change: str) -> Path:
    core.change_dir(change, root)
    return core.within(root, root / ".ar-workflow/tasks" / f"{change}.json")


def task_state(root: Path, change: str, create: bool = False, scope: str = "workspace") -> dict:
    path = task_path(root, change)
    if not path.exists():
        if not create:
            raise ValueError("Task not started")
        directory = core.change_dir(change, root)
        for name in ("proposal.md", "design.md", "test-plan.md", "tasks.md"):
            if not (directory / name).is_file() or not (directory / name).read_text(encoding="utf-8").strip():
                raise ValueError(f"Planning input missing: {name}")
        core.derive(directory, check_only=True)
        data = {"schema_version": 1, "change": change, "scope": scope, "phase": "implementing",
                "input": core.input_state(root, scope), "verification": None, "review": None, "history": []}
    else:
        data = core.load(path)
        if create and scope != data["scope"]:
            data["history"].append({"at": now(), "event": "scope_changed",
                                    "from": data["scope"], "to": scope})
            data.update(scope=scope, phase="implementing", verification=None, review=None, packet=None)
        actual = core.input_state(root, data["scope"])
        if actual != data["input"]:
            data.update({"phase": "implementing", "input": actual, "verification": None, "review": None, "packet": None})
            data["history"].append({"at": now(), "event": "input_changed", "effect": "previous evidence invalidated"})
    data["updated_at"] = now()
    core.save(path, data)
    return data


def save_task(root: Path, data: dict, phase: str, event: str) -> None:
    data["phase"], data["updated_at"] = phase, now()
    data["history"].append({"at": now(), "event": event})
    core.save(task_path(root, data["change"]), data)


def resume_task(root: Path, data: dict) -> None:
    if data.get("phase") == "needs_attention":
        data["next_action"] = ("Explicitly retry review after fixing its cause; current verification is retained"
                               if data.get("verification") else "Run verify after resolving the recorded failure")
        core.save(task_path(root, data["change"]), data)
    if data.get("phase") != "reviewing" or not data.get("packet"):
        return
    packet = core.within(root, Path(data["packet"]))
    run_path = packet / "ocr-run.json"
    if not run_path.exists():
        save_task(root, data, "needs_attention", "review_not_started_no_automatic_api_retry")
        return
    record = core.load(run_path)
    if record.get("status") == "running" and process_alive(int(record["pid"])):
        return
    if record.get("status") == "finished":
        output = core.within(packet, Path(record["output"]))
        report = native_report(packet, output / "native.json", record["returncode"])
        core.save(output / "report.json", report)
        data["review"] = str(output / "report.json")
        save_task(root, data, "reviewed" if report["decision"] == "pass" else "needs_changes", "review_recovered_without_api_call")
    else:
        core.save(run_path, record | {"status": "interrupted", "finished_at": now()})
        save_task(root, data, "needs_attention", "review_interrupted_explicit_retry_required")


def task_gate(root: Path, data: dict) -> None:
    if not data.get("verification") or not data.get("review"):
        raise ValueError("Gate blocked: current verification and independent review are required")
    verify_path = core.within(root, Path(data["verification"]))
    report_path = core.within(root, Path(data["review"]))
    packet = report_path.parent.parent
    manifest = core.load(packet / "manifest.json")
    # Compare resolved paths: on Windows CI the runner temp directory is exposed
    # through an 8.3 short name (RUNNER~1), so str() equality of the same packet
    # fails even though both sides name one directory.
    if (manifest["state"] != data["input"]
            or Path(data.get("packet", "")).resolve() != packet.resolve()
            or manifest.get("scope") != data["scope"]):
        raise ValueError("Gate blocked: packet does not belong to current task input")
    verify = core.load(verify_path)
    if verify.get("status") != "passed" or verify.get("fingerprint") != manifest["fingerprint"]:
        raise ValueError("Gate blocked: mismatched verification")
    check_items = verify.get("checks", [])
    if len(check_items) != 3 or not all(item.get("passed") and item.get("exit") == 0 for item in check_items):
        raise ValueError("Gate blocked: missing required check evidence")
    for number, item in enumerate(check_items):
        tools_root = Path(os.environ.get("AR_WORKFLOW_TOOLS_ROOT", str(root))).resolve()
        if item.get("command") != check_commands(tools_root)[number]:
            raise ValueError("Gate blocked: unexpected verification command")
        if core.digest((verify_path.parent / f"check-{number}.log").read_bytes()) != item.get("log_sha256"):
            raise ValueError("Gate blocked: check log was changed")
    report = core.load(report_path)
    # Rebuild from raw output rather than trusting an edited 'pass' field.
    record = core.load(report_path.parent / "run.json")
    if record.get("status") != "finished" or record.get("fingerprint") != manifest["fingerprint"]:
        raise ValueError("Gate blocked: run record mismatch")
    if record.get("mode") == "direct-api-v1":
        import workflow_review_api
        if report_path.name == "adjudicated-report.json":
            import workflow_findings
            name = report.get("adjudication", {}).get("directory", "")
            if not re.fullmatch(r"finding-[0-9a-f]{24}", name):
                raise ValueError("Gate blocked: invalid independent finding evidence directory")
            recomputed = workflow_findings.report(core.within(report_path.parent, report_path.parent / name))
        else:
            recomputed = workflow_review_api.report(packet, report_path.parent)
        if report != recomputed:
            raise ValueError("Gate blocked: independent API report was changed")
    else:
        raw_path = report_path.parent / "native.json"
        recomputed = native_report(packet, raw_path, record["returncode"])
        if (report.get("native_sha256") != recomputed["native_sha256"]
                or report.get("decision") != recomputed["decision"] or report.get("findings") != recomputed["findings"]):
            raise ValueError("Gate blocked: native review report was changed")
    errors = core.validate_review(manifest, recomputed, core.input_state(root, data["scope"]))
    if errors:
        raise ValueError("Gate blocked: " + "; ".join(errors))


def commit_check(root: Path) -> Path:
    if not core.index_entries(root):
        raise ValueError("Commit gate: index contains no files")
    packet = core.prepare_review(root, "staged")
    result = verification(root, packet)
    print(f"COMMIT CHECKS PASSED for staged input: {result}")
    return result


def install_hooks(root: Path) -> None:
    existing = subprocess.run(["git", "config", "--local", "--get", "core.hooksPath"], cwd=root,
                              capture_output=True, text=True, timeout=10).stdout.strip()
    if existing and existing != ".githooks":
        raise ValueError("Existing custom hook directory preserved; integrate it explicitly")
    core.run(["git", "config", "--local", "core.hooksPath", ".githooks"], root)
    print("Installed repository-local pre-commit hook; remote checks are still required")


def dispatch(args, root: Path) -> None:
    if args.command == "ci":
        checks(root, root, root / ".ar-workflow/ci")
        return
    with operation_lock(root, recover=args.command == "resume"):
        if args.command == "install-hooks":
            install_hooks(root)
            return
        if args.command == "commit-check":
            commit_check(root)
            return
        if args.command == "review-run":
            review_run(root, core.within(root, args.packet.resolve()), args.budget, args.seconds)
            return
        if args.command == "review-plan":
            import workflow_review_api
            print(json.dumps(workflow_review_api.review_plan(core.within(root, args.packet.resolve())),
                             ensure_ascii=False, indent=2))
            return
        if args.command == "finding-prepare":
            import workflow_findings
            workflow_findings.prepare(core.within(root, args.report.resolve()),
                core.within(root, args.rebuttals.resolve()) if args.rebuttals else None)
            return
        if args.command == "finding-run":
            import workflow_findings
            result = workflow_findings.run(root, core.within(root, args.directory.resolve()), args.budget, args.seconds)
            print(f"Independent finding run retained: {result}; apply explicitly, no automatic approval")
            return
        data = task_state(root, args.change, create=args.command == "start", scope=args.scope)
        if args.command == "finding-apply":
            import workflow_findings
            directory = core.within(root, args.directory.resolve())
            if str(directory.parent.parent) != data.get("packet") or not data.get("verification"):
                raise ValueError("Finding disposition needs current task packet and verification")
            result = workflow_findings.report(directory)
            report_path = directory.parent / "adjudicated-report.json"
            core.save(report_path, result)
            data["review"] = str(report_path)
            save_task(root, data, "reviewed" if result["decision"] == "pass" else "needs_changes", "independent_finding_disposition_applied")
            print(f"Finding disposition: {result['decision']}; delivery still requires gate")
            return
        if args.command in ("start", "resume", "status"):
            if args.command == "resume":
                resume_task(root, data)
            summary = {key: value for key, value in data.items() if key != "input"}
            summary["input"] = {"sha256": core.digest(core.canonical(data["input"])),
                                "files": len(data["input"]["files"]), "head": data["input"]["head"]}
            print(json.dumps(summary, ensure_ascii=False, indent=2))
            return
        if args.command == "gate":
            task_gate(root, data)
            save_task(root, data, "local_ready", "gate_passed_not_remote_approval")
            print("LOCAL DELIVERY GATE PASSED; remote merge protection is not established by this result")
            return
        packet = core.prepare_review(root, data["scope"])
        data["packet"] = str(packet)
        try:
            if args.command == "verify":
                data.update(verification=None, review=None)
                save_task(root, data, "verifying", "verification_started_previous_evidence_invalidated")
                data["verification"] = str(verification(root, packet))
                save_task(root, data, "verified", "verification_passed")
            elif args.command == "review":
                if not data.get("verification"):
                    raise ValueError("Verify current input before API review")
                data["review"] = None
                save_task(root, data, "reviewing", "review_started_previous_approval_invalidated")
                if getattr(args, "mode", "ocr") == "direct-api":
                    import workflow_review_api
                    report_path = workflow_review_api.run(root, packet, args.budget, args.seconds)
                else:
                    report_path = review_run(root, packet, args.budget, args.seconds)
                data["review"] = str(report_path)
                report = core.load(report_path)
                save_task(root, data, "reviewed" if report["decision"] == "pass" else "needs_changes", "review_finished")
                if report["decision"] != "pass":
                    raise ValueError(f"Review did not pass; inspect {report_path}")
        except (ValueError, OSError, subprocess.TimeoutExpired):
            save_task(root, data, "needs_attention", args.command + "_failed")
            raise
