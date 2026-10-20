"""Pilot checks must reject stale contracts, incomplete reviews and path escapes."""
import copy
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location("ar_workflow", Path(__file__).parents[1] / "workflow.py")
workflow = importlib.util.module_from_spec(spec)
spec.loader.exec_module(workflow)


class ContractTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="ar-workflow-test-")
        self.addCleanup(self.temp.cleanup)
        self.change = Path(self.temp.name)
        self.source = ("<!-- AR:CONTRACT capability=asset-load -->\n## ADDED Requirements\n"
                       "### Requirement: Load\nSystem SHALL report failure.\n"
                       "#### Scenario: Missing file\n- **WHEN** missing\n- **THEN** report\n"
                       "<!-- /AR:CONTRACT -->\n")
        (self.change / "proposal.md").write_text(self.source, encoding="utf-8")

    def test_source_change_invalidates_derived_file(self):
        workflow.derive(self.change)
        workflow.derive(self.change, check_only=True)
        (self.change / "proposal.md").write_text(self.source.replace("failure", "error"), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "Stale/missing"):
            workflow.derive(self.change, check_only=True)

    def test_missing_and_unexpected_capability_are_rejected(self):
        with self.assertRaisesRegex(ValueError, "Stale/missing"):
            workflow.derive(self.change, check_only=True)
        workflow.derive(self.change)
        extra = self.change / "specs/stale/spec.md"
        extra.parent.mkdir()
        extra.write_text("old contract", encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "Unexpected"):
            workflow.derive(self.change, check_only=True)

    def test_duplicate_and_escape_blocks_are_rejected(self):
        (self.change / "proposal.md").write_text(self.source * 2, encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "Duplicate"):
            workflow.derived_specs(self.change)
        (self.change / "proposal.md").write_text(self.source.replace("asset-load", "../outside"), encoding="utf-8")
        with self.assertRaisesRegex(ValueError, "malformed"):
            workflow.derived_specs(self.change)

    def test_owned_root_boundary_is_enforced(self):
        with self.assertRaisesRegex(ValueError, "escapes"):
            workflow.within(self.change, self.change / "../not-owned")


class ReviewTests(unittest.TestCase):
    def setUp(self):
        self.state = {"head": None, "index": "index", "files": {"a.cpp": "content"}}
        self.manifest = {"state": self.state, "reviewable_files": ["a.cpp"],
                         "ocr_version": "fixture", "preview_sha256": "preview"}
        fingerprint = workflow.digest(workflow.canonical(workflow.review_identity(self.manifest)))
        self.manifest["fingerprint"] = fingerprint
        self.report = {"fingerprint": fingerprint, "reviewer": "fixture-reviewer",
                       "independent_context": True, "status": "complete", "decision": "pass",
                       "reviewed_files": ["a.cpp"], "findings": []}

    def test_complete_matching_fixture_is_consistent(self):
        self.assertEqual([], workflow.validate_review(self.manifest, self.report, self.state))

    def test_changed_input_and_index_are_rejected(self):
        state = copy.deepcopy(self.state)
        state["files"]["a.cpp"] = "changed"
        self.assertTrue(workflow.validate_review(self.manifest, self.report, state))
        state = copy.deepcopy(self.state)
        state["index"] = "new-index"
        self.assertTrue(workflow.validate_review(self.manifest, self.report, state))

    def test_partial_missing_coverage_self_review_and_open_findings_are_rejected(self):
        for patch in ({"status": "partial"}, {"reviewed_files": []},
                      {"independent_context": False}, {"fingerprint": "old"},
                      {"findings": [{"level": "blocking", "status": "open"}]},
                      {"findings": ["invalid unstructured finding"]}):
            with self.subTest(patch=patch):
                self.assertTrue(workflow.validate_review(self.manifest, self.report | patch, self.state))

    def test_manifest_coverage_cannot_shrink_under_original_fingerprint(self):
        manifest = self.manifest | {"reviewable_files": ["other.cpp"]}
        report = self.report | {"reviewed_files": ["other.cpp"]}
        self.assertIn("invalid manifest fingerprint", workflow.validate_review(manifest, report, self.state))

    def test_snapshot_and_preview_mutation_are_rejected(self):
        with tempfile.TemporaryDirectory(prefix="ar-packet-check-") as temporary:
            packet = Path(temporary)
            snapshot = packet / "snapshot"
            snapshot.mkdir()
            (snapshot / "a.cpp").write_bytes(b"original")
            preview = {"reviewable_files": [{"path": "a.cpp"}], "excluded_files": [],
                       "reviewable_count": 1, "excluded_count": 0, "total_files": 1}
            workflow.save(packet / "ocr-preview.json", preview)
            (packet / "diff.patch").write_bytes(b"fixture diff")
            manifest = {"state": {"files": {"a.cpp": workflow.digest(b"original")}},
                        "reviewable_files": ["a.cpp"],
                        "preview_sha256": workflow.digest((packet / "ocr-preview.json").read_bytes()),
                        "diff_sha256": workflow.digest(b"fixture diff")}
            self.assertEqual([], workflow.validate_packet(packet, manifest))
            (snapshot / "a.cpp").write_bytes(b"modified snapshot")
            self.assertTrue(workflow.validate_packet(packet, manifest))
            (snapshot / "a.cpp").write_bytes(b"original")
            preview["reviewable_files"] = []
            workflow.save(packet / "ocr-preview.json", preview)
            self.assertTrue(workflow.validate_packet(packet, manifest))


class NativeOcrSelectionTests(unittest.TestCase):
    def test_shader_headers_tests_cmake_and_docs_are_selected(self):
        binary = workflow.ocr_binary()
        with tempfile.TemporaryDirectory(prefix="ar-ocr-selection-") as temporary:
            root = Path(temporary)
            workflow.run(["git", "init", "--initial-branch=main"], root)
            paths = ["src/main.cpp", "include/shared.h", "shaders/common.glsl",
                     "shaders/example.vert", "shaders/example.frag", "shaders/example.comp",
                     "tests/test_selection.py", "CMakeLists.txt", "docs/guide.md"]
            for name in paths:
                path = root / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text("owned selection fixture\n", encoding="utf-8")
            raw = workflow.run([str(binary), "delegate", "preview", "--repo", str(root),
                                "--rule", str(workflow.ROOT / ".opencodereview/rule.json"),
                                "--format", "json", "--max-git-procs", "2"], root)
            selected = {item["path"] for item in json.loads(raw)["reviewable_files"]}
            self.assertEqual(set(paths), selected)


if __name__ == "__main__":
    unittest.main()
