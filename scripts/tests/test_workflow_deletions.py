"""Real native selection on owned repositories; no model or network calls."""
import os
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).parents[1]))
import workflow as core


class DeletionTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="ar-deletion-test-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.git("init", "--initial-branch=main")
        self.git("config", "user.name", "Owned fixture")
        self.git("config", "user.email", "fixture@example.invalid")
        self.git("config", "core.autocrlf", "false")
        for name in (".opencodereview/rule.json", "workflow/tool-lock.json"):
            dest = self.root / name
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(core.ROOT / name, dest)
        (self.root / ".gitignore").write_text(".ar-workflow/\n", encoding="utf-8")
        (self.root / "old.py").write_text("def removed_api():\n    return 1\n", encoding="utf-8")
        (self.root / "caller.py").write_text("from old import removed_api\n", encoding="utf-8")
        self.git("add", ".")
        self.git("commit", "-m", "fixture baseline")
        self.env = patch.dict(os.environ, {"AR_WORKFLOW_TOOLS_ROOT":
                              os.environ.get("AR_WORKFLOW_TOOLS_ROOT", str(core.ROOT))})
        self.env.start()
        self.addCleanup(self.env.stop)

    def git(self, *args):
        return core.run(["git", *args], self.root)

    def prepare(self, scope="workspace"):
        packet = core.prepare_review(self.root, scope)
        manifest = core.load(packet / "manifest.json")
        self.assertEqual([], core.validate_packet(packet, manifest))
        self.assertIn("old.py", manifest["deletions"])
        artifact = manifest["deletions"]["old.py"]["artifact"]
        self.assertIn(artifact, manifest["reviewable_files"])
        text = (packet / "snapshot" / artifact).read_text(encoding="utf-8")
        self.assertIn("removed_api", text)
        self.assertIn("deleted file mode", text)
        self.assertTrue((packet / "snapshot/caller.py").is_file())
        self.assertFalse((packet / "snapshot/old.py").exists())
        return packet, manifest

    def test_pure_deletion_is_nonempty_and_material_mutation_is_rejected(self):
        self.git("rm", "old.py")
        packet, manifest = self.prepare()
        artifact = manifest["deletions"]["old.py"]["artifact"]
        (packet / "snapshot" / artifact).write_text("lost old content", encoding="utf-8")
        self.assertTrue(core.validate_packet(packet, manifest))

    def test_staged_deletion_ignores_unstaged_restore(self):
        old = (self.root / "old.py").read_bytes()
        self.git("rm", "old.py")
        (self.root / "old.py").write_bytes(old)
        self.prepare("staged")

    def test_rename_retains_old_and_new_review_coverage(self):
        self.git("mv", "old.py", "new.py")
        _, manifest = self.prepare()
        self.assertIn("new.py", manifest["reviewable_files"])

    def test_binary_delete_remains_blocked(self):
        (self.root / "binary.dat").write_bytes(b"\0data")
        self.git("add", "binary.dat")
        self.git("commit", "-m", "binary fixture")
        self.git("rm", "binary.dat")
        with self.assertRaisesRegex(ValueError, "binary/large"):
            core.prepare_review(self.root)


if __name__ == "__main__":
    unittest.main()
