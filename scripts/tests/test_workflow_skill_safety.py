"""Skill maintenance preserves user files and attribution in owned temp fixtures."""
import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
PATH = ROOT / ".agents/skills/ar-cpp-hpc-optimization/scripts/sync_parallel101_corpus.py"
SPEC = importlib.util.spec_from_file_location("ar_owned_corpus_sync", PATH)
sync = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = sync
SPEC.loader.exec_module(sync)


class CorpusSafetyTests(unittest.TestCase):
    def test_sync_preserves_unowned_files_and_conflicting_license(self):
        with tempfile.TemporaryDirectory(prefix="ar-corpus-safety-") as directory:
            root = Path(directory)
            course, simd, skill = [root / name for name in ("course", "simd", "skill")]
            for path in (course, simd, skill):
                path.mkdir()
            (course / "LICENSE").write_bytes(b"upstream license\n")
            corpus = skill / "references/parallel101"
            corpus.mkdir(parents=True)
            user_file = corpus / "user-notes.txt"
            user_file.write_bytes(b"keep my work\n")
            with patch.object(sync, "SELECTIONS", []), patch.object(sync, "git_revision", return_value="a" * 40):
                with self.assertRaisesRegex(RuntimeError, "unexpected files preserved"):
                    sync.sync(course, simd, skill, False)
                self.assertEqual(user_file.read_bytes(), b"keep my work\n")
                self.assertFalse((corpus / "provenance.tsv").exists())
                user_file.unlink()  # Exact file created by this fixture.
                license_file = skill / "LICENSE"
                license_file.write_bytes(b"preserve installed attribution\n")
                with self.assertRaisesRegex(RuntimeError, "refusing to overwrite"):
                    sync.sync(course, simd, skill, False)
                self.assertEqual(license_file.read_bytes(), b"preserve installed attribution\n")
                self.assertFalse((corpus / "provenance.tsv").exists())


if __name__ == "__main__":
    unittest.main()
