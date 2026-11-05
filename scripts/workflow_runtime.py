"""Local execution and failure gates. Does not authenticate remote approvals."""
from __future__ import annotations

from contextlib import contextmanager
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import subprocess
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
    env = {key: value for key, value in os.environ.items() if not key.upper().startswith("GIT_")}
    env.update(AR_WORKFLOW_TOOLS_ROOT=str(tools_root), PYTHONDONTWRITEBYTECODE="1",
               OPENSPEC_TELEMETRY="0", OPEN_SPEC_INTERACTIVE="0", OCR_ENABLE_TELEMETRY="0", OCR_NO_UPDATE="1")
    commands = check_commands(tools_root)
    results = []
    for number, command in enumerate(commands):
        try:
            result = subprocess.run(command, cwd=root, env=env, capture_output=True, timeout=600)
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
    evidence = {"status": "passed" if len(results) == len(commands) and all(r["passed"] for r in results)
                          else "failed",
                "created_at": now(), "checks": results,
                "limitations": ["Workflow checks only; no engine/GPU verification"]}
    core.save(output / "checks.json", evidence)
    if evidence["status"] != "passed":
        detail = (output / f"check-{len(results) - 1}.log")
        tail = detail.read_text(encoding="utf-8", errors="replace")[-2000:] if detail.exists() else "timeout"
        raise ValueError(f"Checks failed; inspect {output}\n{tail}")
    return evidence


def commit_check(root: Path) -> Path:
    if not core.index_entries(root):
        raise ValueError("Commit gate: index contains no files")
    baseline = core.input_state(root, 'staged')
    entries = core.index_entries(root)
    blobs = core.index_blobs(root, entries)
    if {name: core.digest(body) for name, body in blobs.items()} != baseline['files']:
        raise ValueError('Index changed before quick check export')
    import ast
    import tempfile
    base = core.within(root, root / '.ar-workflow/commit-quick')
    base.mkdir(parents=True, exist_ok=True)
    temporary_root = core.within(root, root / 'build/temp/commit-check')
    temporary_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='staged-', dir=temporary_root) as temporary:
        source = core.within(temporary_root, Path(temporary))
        for name, body in blobs.items():
            destination = core.within(source, source / name)
            destination.parent.mkdir(parents=True, exist_ok=True)
            destination.write_bytes(body)
            if name.endswith('.py'):
                try:
                    ast.parse(body, filename=name)
                except SyntaxError as error:
                    raise ValueError(f'Staged Python syntax error: {name}') from error
            elif name.endswith('.json'):
                json.loads(body)
        core.check(source)
    if baseline != core.input_state(root, 'staged'):
        raise ValueError('Staged input changed during quick check')
    path = base / 'latest.json'
    core.save(path, {'kind': 'staged-quick', 'input': baseline, 'status': 'passed',
                     'full_tests_run': False, 'grants_delivery_qualification': False})
    print(f'QUICK STAGED CHECKS PASSED (not full verification or review): {path}')
    return path


def install_hooks(root: Path) -> None:
    existing = subprocess.run(["git", "config", "--local", "--get", "core.hooksPath"], cwd=root,
                              capture_output=True, text=True, timeout=10).stdout.strip()
    if existing and existing != ".githooks":
        raise ValueError("Existing custom hook directory preserved; integrate it explicitly")
    core.run(["git", "config", "--local", "core.hooksPath", ".githooks"], root)
    print("Installed repository-local pre-commit hook; remote checks are still required")


def dispatch(args, root: Path) -> None:
    if args.command == "context":
        import workflow_context as context
        print(json.dumps(context.build(root, args.change, context.changed_names(root)), ensure_ascii=False, indent=2))
        return
    if args.command == "install-hooks":
        install_hooks(root)
        return
    with operation_lock(root):
        if args.command == "ci":
            checks(root, root, root / "build/temp/workflow-ci")
            print("PASS: shared workflow checks (documents, Python tests and OpenSpec)")
        elif args.command == "commit-check":
            commit_check(root)
        else:
            raise ValueError(f"Unsupported command: {args.command}")
