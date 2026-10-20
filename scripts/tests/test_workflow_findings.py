"""Offline disposition fixtures; no actual reviewer or model call is claimed."""
import copy
import json
import os
from pathlib import Path
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).parents[1]))
import workflow as core
import workflow_review_api as api
import workflow_findings as adjudication
import workflow_runtime as runtime
import test_workflow_review_api as fixtures


class FindingTests(unittest.TestCase):
    def setUp(self):
        fixtures.ApiReviewTests.setUp(self)
        self.defect = {"path": "main.py", "line": 1, "level": "must_fix", "content": "Source file is empty"}
        self.set_original([self.defect])
        self.report_path = self.output / "report.json"
        core.save(self.report_path, api.report(self.packet, self.output))
        self.evidence = {"path": "main.py", "start_line": 1, "end_line": 1, "quote": "print('owned fixture')"}

    def set_original(self, findings):
        self.raw["choices"][0]["message"]["content"] = json.dumps({"reviewed_files": ["main.py"], "findings": findings})
        core.save(self.output / "response-0.json", self.raw)
        self.record["batches"][0]["response_sha256"] = core.digest((self.output / "response-0.json").read_bytes())
        core.save(self.output / "run.json", self.record)

    def prepare(self):
        template = adjudication.prepare(self.report_path, None)
        self.rebuttals = core.load(template)
        for item in self.rebuttals["items"]:
            item.update(argument="The frozen file has an executable statement, contradicting the empty-file claim.",
                        evidence=[self.evidence])
        path = self.output / "rebuttals.json"
        core.save(path, self.rebuttals)
        self.directory = adjudication.prepare(self.report_path, path)
        return self.directory

    def response(self, verdict="not_defect"):
        request = core.load(self.directory / "request.json")
        findings = json.loads(request["messages"][1]["content"])["findings"]
        decisions = [{"id": item["id"], "verdict": verdict, "reason": "Independently checked the cited frozen statement.",
                      "evidence": [self.evidence]} for item in findings]
        raw = {"model": "deepseek-flash", "choices": [{"finish_reason": "stop", "message": {
            "content": json.dumps({"decisions": decisions})}}],
            "usage": {"prompt_tokens": 10, "completion_tokens": 2, "total_tokens": 12}}
        self.save_response(raw)
        return raw

    def save_response(self, raw):
        core.save(self.directory / "response.json", raw)
        core.save(self.directory / "run.json", {"mode": "finding-review-v1", "status": "finished", "token_budget": 100000,
            "request_sha256": core.digest(core.canonical(core.load(self.directory / "request.json"))),
            "response_sha256": core.digest((self.directory / "response.json").read_bytes())})

    def test_independent_non_defect_preserves_original_and_derives_result(self):
        before = (self.output / "response-0.json").read_bytes()
        self.prepare()
        self.response()
        result = adjudication.report(self.directory)
        self.assertEqual(result["decision"], "pass")
        self.assertEqual(result["findings"][0]["status"], "resolved")
        self.assertEqual(api.report(self.packet, self.output)["decision"], "request_changes")
        self.assertEqual((self.output / "response-0.json").read_bytes(), before)

    def test_confirmed_uncertain_and_new_input_fix_cannot_approve_old_source(self):
        self.prepare()
        for verdict in ("confirmed", "insufficient_evidence", "fixed_in_new_input"):
            self.response(verdict)
            with self.subTest(verdict=verdict):
                self.assertEqual(adjudication.report(self.directory)["decision"], "request_changes")

    def test_missing_duplicate_or_unknown_ids_are_rejected(self):
        self.prepare()
        original = self.response()
        for mutation in ([], [{"id": "unknown"}], None):
            raw = copy.deepcopy(original)
            decisions = json.loads(raw["choices"][0]["message"]["content"])["decisions"]
            changed = decisions * 2 if mutation is None else mutation
            raw["choices"][0]["message"]["content"] = json.dumps({"decisions": changed})
            self.save_response(raw)
            with self.assertRaisesRegex(ValueError, "exact IDs"):
                adjudication.report(self.directory)

    def test_dismissal_requires_matching_source_citation(self):
        self.prepare()
        original = self.response()
        for evidence in ([], [self.evidence | {"quote": "invented text"}], [self.evidence | {"path": "outside.py"}]):
            raw = copy.deepcopy(original)
            payload = json.loads(raw["choices"][0]["message"]["content"])
            payload["decisions"][0]["evidence"] = evidence
            raw["choices"][0]["message"]["content"] = json.dumps(payload)
            self.save_response(raw)
            with self.assertRaises(ValueError):
                adjudication.report(self.directory)

    def test_partial_original_cannot_be_rescued_by_disposition(self):
        self.record["status"] = "failed"
        core.save(self.output / "run.json", self.record)
        with self.assertRaisesRegex(ValueError, "incomplete"):
            adjudication.prepare(self.report_path, None)

    def test_truncated_or_wrong_model_disposition_is_rejected(self):
        self.prepare()
        original = self.response()
        for mutate in (lambda raw: raw.update(model="other"),
                       lambda raw: raw["choices"][0].update(finish_reason="length"),
                       lambda raw: raw.update(usage={})):
            raw = copy.deepcopy(original)
            mutate(raw)
            self.save_response(raw)
            with self.assertRaises(ValueError):
                adjudication.report(self.directory)

    def test_changed_raw_or_counter_evidence_is_rejected(self):
        self.prepare()
        raw = self.response()
        core.save(self.directory / "response.json", raw | {"model": "forged"})
        with self.assertRaisesRegex(ValueError, "evidence changed"):
            adjudication.report(self.directory)
        self.save_response(raw)
        self.rebuttals["items"][0]["argument"] = "Changed after independent review"
        core.save(self.directory / "rebuttals.json", self.rebuttals)
        with self.assertRaisesRegex(ValueError, "changed"):
            adjudication.report(self.directory)

    def test_changed_source_invalidates_disposition(self):
        self.prepare()
        self.response()
        (self.packet / "snapshot/main.py").write_text("changed", encoding="utf-8")
        with self.assertRaises(ValueError):
            adjudication.report(self.directory)

    def test_duplicate_original_findings_have_distinct_ids_and_both_must_resolve(self):
        self.set_original([self.defect, self.defect])
        self.prepare()
        raw = self.response()
        payload = json.loads(raw["choices"][0]["message"]["content"])
        self.assertNotEqual(payload["decisions"][0]["id"], payload["decisions"][1]["id"])
        payload["decisions"][0]["verdict"] = "confirmed"
        raw["choices"][0]["message"]["content"] = json.dumps(payload)
        self.save_response(raw)
        self.assertEqual(adjudication.report(self.directory)["decision"], "request_changes")

    def test_offline_preparation_and_insufficient_budget_never_read_key_or_call_model(self):
        with patch.object(api, "credential") as key, patch.object(api.urllib.request, "urlopen") as http:
            self.prepare()
            with self.assertRaisesRegex(ValueError, "bounded"):
                adjudication.run(self.packet, self.directory, budget=1, seconds=10)
            key.assert_not_called()
            http.assert_not_called()

    def gate_fixture(self, verdict):
        directory = self.packet / "verify-fixture"
        directory.mkdir()
        checks = []
        for number, command in enumerate(runtime.check_commands(core.ROOT)):
            content = b"owned verifier fixture log\n"
            (directory / f"check-{number}.log").write_bytes(content)
            checks.append({"command": command, "passed": True, "exit": 0,
                           "log_sha256": core.digest(content), "tests_run": 1})
        verify_path = directory / "verification.json"
        core.save(verify_path, {"fingerprint": self.manifest["fingerprint"], "status": "passed", "checks": checks})
        self.record["batches"][0]["request_sha256"] = core.digest(core.canonical(api.requests_for(self.packet)[0]))
        core.save(self.output / "run.json", self.record)
        self.prepare()
        self.response(verdict)
        path = self.output / "adjudicated-report.json"
        core.save(path, adjudication.report(self.directory))
        return {"verification": str(verify_path), "review": str(path), "packet": str(self.packet),
                "input": self.manifest["state"], "scope": "workspace"}

    def test_delivery_gate_accepts_recomputed_disposition_and_rejects_changed_evidence(self):
        with patch.dict(os.environ, {"AR_WORKFLOW_TOOLS_ROOT": str(core.ROOT)}):
            data = self.gate_fixture("not_defect")
            with patch.object(core, "input_state", return_value=self.manifest["state"]):
                runtime.task_gate(self.packet, data)
                raw = core.load(self.directory / "response.json")
                core.save(self.directory / "response.json", raw | {"model": "edited"})
                with self.assertRaises(ValueError):
                    runtime.task_gate(self.packet, data)

    def test_confirmed_defect_cannot_pass_by_editing_derived_report(self):
        with patch.dict(os.environ, {"AR_WORKFLOW_TOOLS_ROOT": str(core.ROOT)}):
            data = self.gate_fixture("confirmed")
            with patch.object(core, "input_state", return_value=self.manifest["state"]):
                with self.assertRaisesRegex(ValueError, "Gate blocked"):
                    runtime.task_gate(self.packet, data)
                path = Path(data["review"])
                core.save(path, core.load(path) | {"decision": "pass"})
                with self.assertRaisesRegex(ValueError, "report was changed"):
                    runtime.task_gate(self.packet, data)


if __name__ == "__main__":
    unittest.main()
