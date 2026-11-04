"""Failure gates use owned fixtures; no network or paid model calls in unit tests."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).parents[1]))
import workflow as core
import workflow_runtime as runtime


class ExecutionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="ar-execution-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.git("init", "--initial-branch=main")
        self.git("config", "core.autocrlf", "false")

    def git(self, *args):
        return core.run(["git", *args], self.root)

    def test_frozen_checks_do_not_inherit_source_git_locations(self):
        with tempfile.TemporaryDirectory(prefix='ar-check-env-') as temporary:
            root = Path(temporary).resolve()
            polluted = {'AR_WORKFLOW_TOOLS_ROOT': str(root), 'GIT_INDEX_FILE': 'foreign-index', 'GIT_DIR': 'foreign-git',
                        'GIT_WORK_TREE': 'foreign-tree', 'GIT_OBJECT_DIRECTORY': 'foreign-objects'}
            with patch.dict(os.environ, polluted), patch.object(runtime, 'check_commands', return_value=[['fixture']]), \
                    patch.object(runtime, 'check_output', return_value={}), \
                    patch.object(runtime.subprocess, 'run', return_value=SimpleNamespace(returncode=0, stdout=b'', stderr=b'')) as command:
                runtime.checks(root, root, root / 'output')
            child_env = command.call_args.kwargs['env']
            self.assertFalse(any(key.upper().startswith('GIT_') for key in child_env))
            self.assertEqual(child_env['AR_WORKFLOW_TOOLS_ROOT'], str(root))

    def test_staged_input_cannot_use_unstaged_fix(self):
        source = self.root / "main.cpp"
        source.write_bytes(b"broken\n")
        self.git("add", "main.cpp")
        before = core.input_state(self.root, "staged")
        source.write_text("fixed\n", encoding="utf-8")
        self.assertEqual(before, core.input_state(self.root, "staged"))
        entry = core.index_entries(self.root)["main.cpp"]
        self.assertEqual(core.blob(self.root, entry["oid"]), b"broken\n")
        self.assertEqual(core.index_blobs(self.root, core.index_entries(self.root))["main.cpp"], b"broken\n")
        self.git("add", "main.cpp")
        self.assertNotEqual(before, core.input_state(self.root, "staged"))

    def test_hook_rejects_unstaged_controller_before_running_it(self):
        scripts = self.root / "scripts"
        scripts.mkdir()
        controller = scripts / "workflow.py"
        controller.write_text("raise SystemExit(7)\n", encoding="utf-8")
        hooks = self.root / ".githooks"
        hooks.mkdir()
        (hooks / "pre-commit").write_bytes((core.ROOT / ".githooks/pre-commit").read_bytes())
        self.git("config", "core.hooksPath", ".githooks")
        self.git("config", "user.name", "AR owned test")
        self.git("config", "user.email", "owned-test@example.invalid")
        self.git("add", "scripts", ".githooks")
        controller.write_text("raise SystemExit(0)\n", encoding="utf-8")
        result = subprocess.run(["git", "commit", "-m", "owned guard fixture"],
                                cwd=self.root, capture_output=True, timeout=20)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"Commit blocked", result.stderr)
        self.assertFalse((self.root / ".git/refs/heads/main").exists())

    def test_live_lock_is_preserved_and_dead_owner_can_resume(self):
        with runtime.operation_lock(self.root):
            with self.assertRaisesRegex(ValueError, "Another operation"):
                with runtime.operation_lock(self.root, recover=True):
                    pass
        child = subprocess.Popen([sys.executable, "-c", "pass"])
        child.wait(timeout=10)
        core.save(self.root / ".ar-workflow/operation.lock", {"pid": child.pid, "nonce": "abandoned"})
        with runtime.operation_lock(self.root, recover=True):
            self.assertTrue((self.root / ".ar-workflow/operation.lock").is_file())
        self.assertFalse((self.root / ".ar-workflow/operation.lock").exists())

    def test_kernel_guard_blocks_recovery_even_when_record_is_replaced(self):
        code = ("import sys; from pathlib import Path; sys.path.insert(0,sys.argv[1]); "
                "import workflow_runtime as r; root=Path(sys.argv[2]); "
                "\nwith r.operation_lock(root):\n"
                " (root/'.ar-workflow/held').write_text('held'); sys.stdin.read(1)\n")
        child = subprocess.Popen([sys.executable, "-c", code, str(core.ROOT / "scripts"), str(self.root)],
                                 stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            deadline = time.monotonic() + 10
            while not (self.root / ".ar-workflow/held").exists() and time.monotonic() < deadline:
                self.assertIsNone(child.poll())
                time.sleep(0.02)
            self.assertTrue((self.root / ".ar-workflow/held").exists())
            core.save(self.root / ".ar-workflow/operation.lock", {"pid": 0, "nonce": "replaced"})
            with self.assertRaisesRegex(ValueError, "kernel lock"):
                with runtime.operation_lock(self.root, recover=True):
                    pass
        finally:
            child.communicate(b"x", timeout=10)
        self.assertEqual(child.returncode, 0)

    def test_hook_git_environment_cannot_redirect_snapshot_index(self):
        with patch.dict(os.environ, {"GIT_INDEX_FILE": "foreign-index", "GIT_COMMON_DIR": "foreign-git"}):
            env = core.snapshot_env(self.root)
        self.assertNotIn("GIT_INDEX_FILE", env)
        self.assertNotIn("GIT_COMMON_DIR", env)
        self.assertEqual(env["GIT_DIR"], str(self.root / "ocr.git"))

    def test_ci_checks_workflow_in_a_repo_with_cpp_sources(self):
        (self.root / "main.cpp").write_bytes(b"int main() {}\n")
        with patch.object(runtime, "check_commands", return_value=[
            [sys.executable, "-c", "print('workflow fixture ran')"],
        ]):
            runtime.dispatch(SimpleNamespace(command="ci"), self.root)

        output = self.root / ".ar-workflow/ci"
        self.assertIn("workflow fixture ran", (output / "check-0.log").read_text())
        evidence = core.load(output / "checks.json")
        self.assertEqual(evidence["status"], "passed")
        self.assertIn("no engine/GPU verification", evidence["limitations"][0])

    def test_atomic_record_update_retains_old_record_on_replace_failure(self):
        path = self.root / "state.json"
        core.save(path, {"phase": "before"})
        with patch.object(core.os, "replace", side_effect=OSError("simulated failure")):
            with self.assertRaises(OSError):
                core.save(path, {"phase": "after"})
        self.assertEqual(core.load(path), {"phase": "before"})
        self.assertFalse(list(self.root.glob("*.tmp")))

    def test_empty_or_entirely_skipped_tests_are_not_success(self):
        for log in (b"Ran 0 tests in 0.001s\n\nOK\n", b"Ran 2 tests in 0.001s\n\nOK (skipped=2)\n"):
            with self.assertRaisesRegex(ValueError, "No executed"):
                runtime.check_output(1, b"", log, self.root)
        self.assertEqual(runtime.check_output(1, b"", b"Ran 2 tests in 0.1s\n\nOK\n", self.root), {"tests_run": 2})

    def test_openspec_json_requires_actual_complete_coverage(self):
        directory = self.root / "openspec/changes/fixture-change"
        directory.mkdir(parents=True)
        (directory / "proposal.md").write_text("fixture plan", encoding="utf-8")
        raw = {"version": "1.0", "items": [{"id": "fixture-change", "type": "change", "valid": True, "issues": []}],
               "summary": {"totals": {"items": 1, "passed": 1, "failed": 0}}}
        self.assertEqual(runtime.check_output(2, json.dumps(raw).encode(), b"", self.root)["validated_items"],
                         [("change", "fixture-change")])
        raw["items"] = []
        with self.assertRaisesRegex(ValueError, "structured result"):
            runtime.check_output(2, json.dumps(raw).encode(), b"", self.root)

    def test_corrupt_lock_cleanup_does_not_hide_original_failure(self):
        with self.assertRaisesRegex(ValueError, "original operation failure"):
            with runtime.operation_lock(self.root):
                (self.root / ".ar-workflow/operation.lock").write_text("broken JSON", encoding="utf-8")
                raise ValueError("original operation failure")

    def test_partial_commit_source_index_is_preserved_in_snapshot(self):
        source = self.root / "main.cpp"
        source.write_bytes(b"selected-for-commit\n")
        self.git("add", "main.cpp")
        original_oid = core.index_entries(self.root)["main.cpp"]["oid"]
        alternate = self.root / ".git/alternate-index"
        alternate.write_bytes((self.root / ".git/index").read_bytes())
        source.write_bytes(b"not-selected-for-commit\n")
        self.git("add", "main.cpp")
        packet = self.root / ".ar-workflow/alternate-review"
        (packet / "snapshot").mkdir(parents=True)
        with patch.dict(os.environ, {"GIT_INDEX_FILE": str(alternate)}):
            state = core.input_state(self.root, "staged")
            env = core.snapshot_git(packet, self.root, state)
        output = subprocess.run(["git", "ls-files", "--stage"], cwd=packet / "snapshot", env=env,
                                capture_output=True, check=True, timeout=10).stdout.decode()
        self.assertIn(original_oid, output)
        self.assertNotIn("GIT_INDEX_FILE", env)


if __name__ == "__main__":
    unittest.main()
