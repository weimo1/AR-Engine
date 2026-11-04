"""Cost/scope guards use owned fixtures and fake HTTP, never paid requests."""
import sys
from pathlib import Path
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).parents[1]))
import workflow as core
import workflow_review_scope as scope
import workflow_runtime as runtime
import test_workflow_context as context_fixture


class ScopeTests(unittest.TestCase):
    def test_deletion_material_keeps_current_reverse_callers(self):
        files = {'main.py': 'from removed import run\n', 'other.py': 'value = 1\n'}
        result = scope.related({'.ar-review-context/deletions/material.md': 'Historical data'},
                               files, [], ['removed.py'])
        self.assertEqual(result, {'main.py': files['main.py']})

    def test_direct_imports_callers_and_unrelated_changes(self):
        files = {'main.py': 'from helper import run\n', 'helper.py': 'def run(): pass\n',
                 'caller.py': 'import main\n', '.githooks/check': 'python main.py\n', 'unrelated.py': 'value = 1\n'}
        first = scope.related({'main.py': files['main.py']}, files, [])
        self.assertEqual(set(first), {'main.py', 'helper.py', 'caller.py', '.githooks/check'})
        package_files = {'main.py': 'import pkg\n', 'pkg/__init__.py': 'value = 1\n'}
        self.assertIn('pkg/__init__.py', scope.related({'main.py': package_files['main.py']}, package_files, []))
        files['unrelated.py'] = 'value = 2\n'
        self.assertEqual(first, scope.related({'main.py': files['main.py']}, files, []))
        self.assertIn('unrelated.py', scope.related({'main.py': files['main.py']}, files, ['unrelated.py']))
        with self.assertRaisesRegex(ValueError, 'dynamic'):
            scope.related({'main.py': '__import__(selected)'}, {'main.py': '__import__(selected)'}, [])


class QuickTests(unittest.TestCase):
    setUp = context_fixture.ContextTests.setUp
    write = context_fixture.ContextTests.write

    def test_staged_error_cannot_use_unstaged_fix_or_full_test_rerun(self):
        self.write('workflow/tool-lock.json', {'ocr': {'review_policy_version': 2}})
        self.write('main.py', 'def broken(\n')
        core.run(['git', 'add', '.'], self.root)
        self.write('main.py', "print('fixed only in workspace')\n")
        with patch.object(core, 'check'), patch.object(runtime, 'checks', side_effect=AssertionError('No full rerun')):
            with self.assertRaisesRegex(ValueError, 'Staged Python syntax'):
                runtime.commit_check(self.root)
            core.run(['git', 'add', 'main.py'], self.root)
            result = core.load(runtime.commit_check(self.root))
            self.assertFalse(result['full_tests_run'])
            self.assertFalse(result['grants_delivery_qualification'])


if __name__ == '__main__':
    unittest.main()
