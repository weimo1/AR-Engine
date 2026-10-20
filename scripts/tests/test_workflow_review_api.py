"""Independent review adapters fail closed; these tests never call a model."""
import copy
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).parents[1]))
import workflow as core
import workflow_review_api as api


class ApiReviewTests(unittest.TestCase):
    def setUp(self):
        self.raw = {"model": "deepseek-flash", "choices": [{"finish_reason": "stop", "message": {
            "content": json.dumps({"reviewed_files": ["main.py"], "findings": []})}}],
            "usage": {"total_tokens": 12, "prompt_tokens": 10, "completion_tokens": 2}}
        temporary = tempfile.TemporaryDirectory(prefix="ar-api-evidence-")
        self.addCleanup(temporary.cleanup)
        self.packet = Path(temporary.name)
        source = self.packet / "snapshot"
        files = {"main.py": b"print('owned fixture')\n",
                 "workflow/tool-lock.json": core.canonical({"ocr": {"api_model": "deepseek-flash"}}),
                 ".opencodereview/rule.json": b"{}"}
        for name, data in files.items():
            path = source / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(data)
        preview = {"reviewable_files": [{"path": "main.py"}], "excluded_files": [],
                   "reviewable_count": 1, "excluded_count": 0, "total_files": 1}
        core.save(self.packet / "ocr-preview.json", preview)
        (self.packet / "diff.patch").write_bytes(b"fixture diff")
        self.manifest = {"scope": "workspace", "state": {"head": None, "files": {
            name: core.digest(data) for name, data in files.items()}}, "reviewable_files": ["main.py"],
            "preview_sha256": core.digest((self.packet / "ocr-preview.json").read_bytes()),
            "diff_sha256": core.digest(b"fixture diff")}
        self.manifest["fingerprint"] = core.digest(core.canonical(core.review_identity(self.manifest)))
        core.save(self.packet / "manifest.json", self.manifest)
        self.output = self.packet / "direct-test"
        self.output.mkdir()
        core.save(self.output / "response-0.json", self.raw)
        request = api.requests_for(self.packet)[0]
        self.record = {"mode": "direct-api-v1", "status": "finished", "token_budget": 100000,
                       "fingerprint": self.manifest["fingerprint"], "batches": [{
                           "request_sha256": core.digest(core.canonical(request)),
                           "response_sha256": core.digest((self.output / "response-0.json").read_bytes())}]}
        core.save(self.output / "run.json", self.record)

    def test_valid_complete_evidence_and_open_defect(self):
        self.assertEqual(api.report(self.packet, self.output)["decision"], "pass")
        payload = {"reviewed_files": ["main.py"], "findings": [{"path": "main.py", "line": 1,
                    "level": "must_fix", "content": "Concrete defect"}]}
        self.raw["choices"][0]["message"]["content"] = json.dumps(payload)
        findings, usage = api.parse_response(self.raw, ["main.py"], "deepseek-flash")
        self.assertEqual(findings[0]["level"], "must_fix")
        self.assertEqual(usage, 12)

    def test_truncation_model_missing_and_duplicate_coverage_rejected(self):
        for mutate in (lambda r: r["choices"][0].update(finish_reason="length"),
                       lambda r: r.update(model="other"),
                       lambda r: r.update(usage={}),
                       lambda r: r["choices"][0]["message"].update(content=json.dumps(
                           {"reviewed_files": [], "findings": []})),
                       lambda r: r["choices"][0]["message"].update(content=json.dumps(
                           {"reviewed_files": ["main.py", "main.py"], "findings": []}))):
            raw = copy.deepcopy(self.raw)
            mutate(raw)
            with self.assertRaises((ValueError, KeyError, TypeError)):
                api.parse_response(raw, ["main.py"], "deepseek-flash")

    def test_changed_raw_response_cannot_reuse_approval(self):
        core.save(self.output / "response-0.json", self.raw | {"model": "other"})
        with self.assertRaisesRegex(ValueError, "changed"):
            api.report(self.packet, self.output)

    def test_unchanged_caller_is_supplied_without_claiming_assigned_coverage(self):
        data = b"from main import removed_api\n"
        (self.packet / "snapshot/caller.py").write_bytes(data)
        self.manifest["state"]["files"]["caller.py"] = core.digest(data)
        core.save(self.packet / "manifest.json", self.manifest)
        request = api.requests_for(self.packet)[0]
        payload = json.loads(request["messages"][1]["content"])
        self.assertIn("caller.py", payload["frozen_files"])
        self.assertEqual(payload["assigned_files"], ["main.py"])

    def test_context_finding_needs_its_path_declared_as_allowed(self):
        payload = {"reviewed_files": ["main.py"], "findings": [{"path": "caller.py", "line": 3,
                   "level": "suggestion", "content": "Frozen caller context"}]}
        self.raw["choices"][0]["message"]["content"] = json.dumps(payload)
        findings, _ = api.parse_response(self.raw, ["main.py"], "deepseek-flash", ["caller.py", "main.py"])
        self.assertEqual(findings[0]["path"], "caller.py")
        with self.assertRaisesRegex(ValueError, "malformed"):
            api.parse_response(self.raw, ["main.py"], "deepseek-flash")

    def test_code_unit_asks_for_one_bounded_response_without_thinking(self):
        request = api.requests_for(self.packet)[0]
        self.assertEqual(request["max_tokens"], api.CODE_OUTPUT)
        self.assertEqual(request["thinking"], {"type": "disabled"})
        self.assertNotIn("reasoning_effort", request)

    def test_partial_run_and_exhausted_budget_rejected(self):
        for mutation in ({"status": "running"}, {"batches": []}, {"token_budget": 1}):
            core.save(self.output / "run.json", self.record | mutation)
            with self.assertRaises(ValueError):
                api.report(self.packet, self.output)

    def test_oversized_unchanged_caller_blocks_before_any_api_request(self):
        data = b"x" * 100001
        (self.packet / "snapshot/caller.py").write_bytes(data)
        self.manifest["state"]["files"]["caller.py"] = core.digest(data)
        core.save(self.packet / "manifest.json", self.manifest)
        with patch.object(api, "credential") as key, patch.object(api.urllib.request, "urlopen") as http:
            with self.assertRaisesRegex(ValueError, "Caller context file exceeds"):
                api.requests_for(self.packet)
            key.assert_not_called()
            http.assert_not_called()

    def test_validation_summary_requires_matching_input_and_unchanged_logs(self):
        output = self.packet / "verify-fixture"
        from workflow_runtime import check_commands
        commands = check_commands(Path(os.environ.get("AR_WORKFLOW_TOOLS_ROOT", str(core.ROOT))).resolve())
        items = []
        for number in range(3):
            log = b"owned passing log"
            output.mkdir(exist_ok=True)
            (output / f"check-{number}.log").write_bytes(log)
            items.append({"exit": 0, "passed": True, "log_sha256": core.digest(log), "tests_run": 42,
                          "command": commands[number]})
        record = {"status": "passed", "fingerprint": "other-input", "checks": items}
        core.save(output / "verification.json", record)
        self.assertEqual(api.verification_summary(self.packet, self.manifest)["status"], "not supplied")
        record["fingerprint"] = self.manifest["fingerprint"]
        core.save(output / "verification.json", record)
        self.assertEqual(api.verification_summary(self.packet, self.manifest)["status"], "passed")
        record["checks"][0]["command"] = ["fake", "always-pass"]
        core.save(output / "verification.json", record)
        with self.assertRaisesRegex(ValueError, "command identity"):
            api.verification_summary(self.packet, self.manifest)
        record["checks"][0]["command"] = commands[0]
        core.save(output / "verification.json", record)
        (output / "check-1.log").write_bytes(b"changed")
        with self.assertRaisesRegex(ValueError, "log changed"):
            api.verification_summary(self.packet, self.manifest)

    def test_insufficient_budget_never_fetches_key_or_calls_http(self):
        with patch.object(core, "input_state", return_value=self.manifest["state"]), \
                patch.object(api, "credential") as key, patch.object(api.urllib.request, "urlopen") as http:
            with self.assertRaisesRegex(ValueError, "exceed budget"):
                api.run(self.packet, self.packet, 1, 10)
            key.assert_not_called()
            http.assert_not_called()

    def cache(self, request, raw=None):
        data = core.canonical(raw or self.raw)
        core.save(api.cache_path(request), {"request_sha256": core.digest(core.canonical(request)),
                  "response_sha256": core.digest(data), "raw": data.decode("utf-8")})
        return data

    def test_exact_pass_cache_runs_without_key_or_http_and_counts_zero_new_tokens(self):
        with patch.dict(os.environ, {"AR_WORKFLOW_TOOLS_ROOT": str(self.packet)}):
            request = api.requests_for(self.packet)[0]
            self.cache(request)
            with patch.object(core, "input_state", return_value=self.manifest["state"]), \
                    patch.object(api, "credential") as key, patch.object(api.urllib.request, "urlopen") as http:
                output = api.run(self.packet, self.packet, 1, 10)
                result = api.report(self.packet, output.parent)
                self.assertEqual(result["decision"], "pass")
                self.assertEqual(result["actual_tokens"], 0)
                self.assertEqual(result["reused_batches"], 1)
                key.assert_not_called()
                http.assert_not_called()

    def test_cache_rejects_partial_findings_and_raw_mutation(self):
        with patch.dict(os.environ, {"AR_WORKFLOW_TOOLS_ROOT": str(self.packet)}):
            request = api.requests_for(self.packet)[0]
            raw = copy.deepcopy(self.raw)
            raw["choices"][0]["finish_reason"] = "length"
            self.cache(request, raw)
            with self.assertRaises(ValueError):
                api.cached_response(request)
            self.cache(request)
            saved = core.load(api.cache_path(request))
            saved["raw"] += " "
            core.save(api.cache_path(request), saved)
            with self.assertRaisesRegex(ValueError, "changed"):
                api.cached_response(request)

    def test_unrelated_reference_preserves_code_unit_but_changed_code_invalidates_it(self):
        with patch.dict(os.environ, {"AR_WORKFLOW_TOOLS_ROOT": str(self.packet)}):
            request = api.requests_for(self.packet)[0]
            self.cache(request)
            path = self.packet / "snapshot/docs/archive/example.md"
            path.parent.mkdir(parents=True)
            path.write_bytes(b"Historical reference, not active rules.\n")
            self.manifest["state"]["files"]["docs/archive/example.md"] = core.digest(path.read_bytes())
            self.manifest["reviewable_files"].append("docs/archive/example.md")
            preview = {"reviewable_files": [{"path": n} for n in self.manifest["reviewable_files"]],
                       "excluded_files": [], "reviewable_count": 2, "excluded_count": 0, "total_files": 2}
            core.save(self.packet / "ocr-preview.json", preview)
            self.manifest["preview_sha256"] = core.digest((self.packet / "ocr-preview.json").read_bytes())
            self.manifest["fingerprint"] = "different whole-workspace identity"
            core.save(self.packet / "manifest.json", self.manifest)
            changed = api.requests_for(self.packet)
            code = next(r for r in changed if json.loads(r["messages"][1]["content"])["unit_kind"] == "code")
            self.assertIsNotNone(api.cached_response(code))
            (self.packet / "snapshot/main.py").write_bytes(b"changed code\n")
            self.manifest["state"]["files"]["main.py"] = core.digest(b"changed code\n")
            core.save(self.packet / "manifest.json", self.manifest)
            code = next(r for r in api.requests_for(self.packet)
                        if json.loads(r["messages"][1]["content"])["unit_kind"] == "code")
            self.assertIsNone(api.cached_response(code))


    def test_malformed_sample_is_resampled_once_and_counts_one_batch(self):
        malformed = copy.deepcopy(self.raw)
        body = json.loads(malformed["choices"][0]["message"]["content"])
        body["findings"] = [{"path": "main.py", "line": 1, "content": "missing level"}]
        malformed["choices"][0]["message"]["content"] = json.dumps(body)

        class Reply:
            def __init__(self, data):
                self.data = data

            def read(self, _limit):
                return self.data

            def __enter__(self):
                return self

            def __exit__(self, *_error):
                return False

        replies = iter([Reply(core.canonical(malformed)), Reply(core.canonical(self.raw))])
        with patch.dict(os.environ, {"AR_WORKFLOW_TOOLS_ROOT": str(self.packet)}), \
                patch.object(core, "input_state", return_value=self.manifest["state"]), \
                patch.object(api, "credential", return_value="owned"), \
                patch.object(api.urllib.request, "urlopen", side_effect=lambda *_a, **_k: next(replies)):
            output = api.run(self.packet, self.packet, 100000, 10)
        record = core.load(output.parent / "run.json")
        self.assertEqual(len(record["batches"]), 1)
        self.assertEqual(record["batches"][0]["actual_tokens"], 12)
        self.assertEqual(core.load(output)["decision"], "pass")


if __name__ == "__main__":
    unittest.main()
