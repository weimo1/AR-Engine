"""Cost/scope guards use owned fixtures and fake HTTP, never paid requests."""
import copy
import json
import sys
import tempfile
import time
from pathlib import Path
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).parents[1]))
import workflow as core
import workflow_review_api as api
import workflow_review_budget as budget
import workflow_review_scope as scope
import workflow_runtime as runtime
import test_workflow_review_api as api_fixture
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


class BudgetTests(unittest.TestCase):
    def test_same_source_under_different_changes_shares_spend_deadline_and_pending(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            first, second = root / 'first', root / 'second'
            source = {'head': 'base', 'index': 'index', 'files': {'code.py': 'hash'}}
            for packet in (first, second):
                core.save(packet / 'manifest.json', {'state': source, 'change': packet.name})
            raw = json.dumps({'usage': {'total_tokens': 60}}).encode()
            with patch.object(budget.urllib.request, 'urlopen') as http:
                http.return_value.__enter__.return_value.read.return_value = raw
                budget.send(first, object(), 100, 60, 10, first / 'raw.json')
            path, value = budget.state(second, 100, 60)
            self.assertEqual(value['spent'], 60)
            self.assertEqual(path, budget.state(first, 100, 60)[0])
            with self.assertRaisesRegex(ValueError, 'budget'):
                budget.preflight(second, 100, 60, 50)
            core.save(path, dict(value, pending=True))
            with self.assertRaisesRegex(ValueError, 'unknown'):
                budget.preflight(second, 100, 60, 1)
            core.save(first / 'paid-round.json', dict(value, pending=False))
            with self.assertRaisesRegex(ValueError, 'reconciliation'):
                budget.preflight(first, 100, 60, 1)
            with self.assertRaisesRegex(ValueError, 'reconciliation'):
                budget.preflight(second, 100, 60, 1)

    def test_round_spend_and_unknown_request_cannot_reset(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            core.save(root / 'paid-round.json', {'started_at': time.time(), 'token_limit': 100,
                       'seconds_limit': 60, 'spent': 80, 'pending': False})
            with self.assertRaisesRegex(ValueError, 'insufficient'):
                budget.preflight(root, 100000, 600, 30)
            budget.preflight(root, 100000, 600, 10)
            value = core.load(root / 'paid-round.json')
            core.save(root / 'paid-round.json', dict(value, pending=True))
            with self.assertRaisesRegex(ValueError, 'unknown'):
                budget.preflight(root, 100000, 600, 1)

    def test_response_is_recorded_and_deadline_does_not_wait_for_socket(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            raw = json.dumps({'usage': {'total_tokens': 10}}).encode()
            with patch.object(budget.urllib.request, 'urlopen') as http:
                http.return_value.__enter__.return_value.read.return_value = raw
                self.assertEqual(budget.send(root, object(), 100, 60, 20, root / 'raw.json'), raw)
            self.assertEqual(core.load(root / 'paid-round.json')['spent'], 10)
            value = core.load(root / 'paid-round.json')
            core.save(root / 'paid-round.json', dict(value, started_at=time.time() - 61))
            with patch.object(budget.urllib.request, 'urlopen') as http:
                with self.assertRaisesRegex(ValueError, 'deadline'):
                    budget.send(root, object(), 100, 60, 1, root / 'other.json')
                http.assert_not_called()
            import threading
            completed = threading.Event()
            release = threading.Event()
            live = root / 'live'
            live.mkdir()
            core.save(live / 'paid-round.json', {'started_at': 999.05,
                       'token_limit': 100, 'seconds_limit': 1, 'spent': 0, 'pending': False})
            def slow_read(*args):
                release.wait(5)
                completed.set()
                return raw
            with patch.object(budget.time, 'time', return_value=1000), \
                    patch.object(budget.urllib.request, 'urlopen') as http:
                http.return_value.__enter__.return_value.read.side_effect = slow_read
                started = time.monotonic()
                try:
                    with self.assertRaisesRegex(ValueError, 'deadline'):
                        budget.send(live, object(), 100, 1, 20, live / 'late.json')
                    self.assertLess(time.monotonic() - started, 2)
                finally:
                    release.set()
                self.assertTrue(completed.wait(5))
            self.assertTrue(core.load(live / 'paid-round.json')['pending'])
            self.assertFalse((live / 'late.json').exists())


class CompactApiTests(unittest.TestCase):
    setUp = api_fixture.ApiReviewTests.setUp

    def compact(self, docs=False):
        policy = {'ocr': {'api_model': 'deepseek-flash', 'api_thinking': 'enabled',
                         'api_reasoning_effort': 'high', 'api_max_output': 32768,
                         'review_policy_version': 2}}
        for target in (self.packet / 'workflow/tool-lock.json', self.packet / 'snapshot/workflow/tool-lock.json'):
            core.save(target, policy)
        self.manifest['state']['files']['workflow/tool-lock.json'] = core.digest(
            (self.packet / 'snapshot/workflow/tool-lock.json').read_bytes())
        if docs:
            path = self.packet / 'snapshot/docs/README.md'
            path.parent.mkdir()
            path.write_text('Reference index', encoding='utf-8')
            self.manifest['state']['files']['docs/README.md'] = core.digest(path.read_bytes())
            self.manifest['reviewable_files'] = ['docs/README.md']
            preview = {'reviewable_files': [{'path': 'docs/README.md'}], 'excluded_files': [],
                       'reviewable_count': 1, 'excluded_count': 0, 'total_files': 1}
            core.save(self.packet / 'ocr-preview.json', preview)
            self.manifest['preview_sha256'] = core.digest((self.packet / 'ocr-preview.json').read_bytes())
        self.manifest['deterministic_files'] = scope.documents(self.packet / 'snapshot', self.manifest['reviewable_files'])
        self.manifest['fingerprint'] = core.digest(core.canonical(core.review_identity(self.manifest)))
        core.save(self.packet / 'manifest.json', self.manifest)

    def test_oversized_round_rejected_before_credentials(self):
        self.compact()
        with patch.object(core, 'input_state', return_value=self.manifest['state']), \
                patch.object(api, 'credential', side_effect=AssertionError('No key read')):
            with self.assertRaisesRegex(ValueError, '100000'):
                api.run(self.packet, self.packet, 100001, 600)

    def test_reference_documents_have_explicit_non_model_coverage(self):
        self.compact(docs=True)
        with patch.object(core, 'input_state', return_value=self.manifest['state']), \
                patch.object(api, 'verification_summary', return_value={'status': 'passed'}), \
                patch.object(api, 'credential', side_effect=AssertionError('No key read')):
            path = api.run(self.packet, self.packet, 100000, 600)
            result = core.load(path)
            self.assertFalse(result['independent_context'])
            self.assertEqual(result['reviewed_files'], [])
            self.assertEqual(result['checked_without_model'], ['docs/README.md'])
            self.assertEqual(core.validate_review(self.manifest, result, self.manifest['state']), [])
            result['checked_without_model'] = []
            self.assertTrue(core.validate_review(self.manifest, result, self.manifest['state']))
        self.assertNotIn('AGENTS.md', scope.documents(self.packet / 'snapshot', ['AGENTS.md']))


class QuickTests(unittest.TestCase):
    setUp = context_fixture.ContextTests.setUp
    write = context_fixture.ContextTests.write

    def test_staged_error_cannot_use_unstaged_fix_or_full_test_rerun(self):
        self.write('workflow/tool-lock.json', {'ocr': {'review_policy_version': 2}})
        self.write('main.py', 'def broken(\n')
        core.run(['git', 'add', '.'], self.root)
        self.write('main.py', "print('fixed only in workspace')\n")
        with patch.object(core, 'check'), patch.object(runtime, 'verification', side_effect=AssertionError('No full rerun')):
            with self.assertRaisesRegex(ValueError, 'Staged Python syntax'):
                runtime.commit_check(self.root)
            core.run(['git', 'add', 'main.py'], self.root)
            result = core.load(runtime.commit_check(self.root))
            self.assertFalse(result['full_tests_run'])
            self.assertFalse(result['grants_delivery_qualification'])


if __name__ == '__main__':
    unittest.main()
