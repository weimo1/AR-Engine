"""Offline receipt admission and rejection; no API or reviewer impersonation."""
import copy
import sys
import tempfile
from pathlib import Path
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).parents[1]))
import workflow as core
import workflow_review_context as context


class ContextReviewTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='ar-review-context-')
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.packet = self.root / '.ar-workflow/reviews/packet'
        self.output = self.packet / 'context-fixture'
        self.output.mkdir(parents=True)
        self.state = {'head': 'base', 'index': 'index', 'files': {'main.py': 'hash'}}
        self.manifest = {'fingerprint': 'input', 'scope': 'workspace', 'state': self.state,
                         'reviewable_files': ['main.py'], 'task_context': {'change': 'task'}}
        core.save(self.packet / 'manifest.json', self.manifest)
        core.save(self.packet / 'verification/verification.json', {'status': 'passed', 'fingerprint': 'input'})
        self.identity = 'a' * 24
        self.raw = {'schema': 'ar.independent-context/v1', 'status': 'complete', 'fingerprint': 'input',
                    'assignment': self.identity, 'reviewer': 'independent-fixture',
                    'independent_context': True, 'reviewed_files': ['main.py'], 'findings': []}
        self.assignment = {'id': self.identity, 'agent': 'independent-fixture', 'role': 'reviewer',
                           'access': 'read', 'change': 'task', 'input': self.state}
        self.handoff = {'assignment': self.identity, 'status': 'completed',
                        'start_input': self.state, 'end_input': self.state}
        self.bundle = {'review': self.raw, 'assignment': self.assignment, 'handoff': self.handoff}
        self.validation = patch.object(core, 'validate_packet', return_value=[])
        self.validation.start()
        self.addCleanup(self.validation.stop)

    def write(self, bundle=None):
        core.save(self.output / 'source-0.json', bundle or self.bundle)
        core.save(self.output / 'run.json', {'mode': 'independent-context-v1', 'status': 'finished',
                  'fingerprint': 'input', 'coordinator': 'coordinator-fixture',
                  'verification': 'verification/verification.json',
                  'receipts': [{'file': 'source-0.json',
                                'sha256': core.digest((self.output / 'source-0.json').read_bytes())}]})

    def test_full_coverage_and_findings_cannot_self_resolve(self):
        self.write()
        self.assertEqual(context.report(self.packet, self.output)['decision'], 'pass')
        self.raw['findings'] = [{'path': 'main.py', 'line': 1, 'level': 'must_fix',
                                 'content': 'Known fixture defect', 'status': 'resolved'}]
        self.write()
        result = context.report(self.packet, self.output)
        self.assertEqual(result['decision'], 'request_changes')
        self.assertEqual(result['findings'][0]['status'], 'open')

    def test_stale_partial_missing_coverage_and_invalid_finding_reject(self):
        mutations = [lambda b: b['review'].update(fingerprint='old'),
                     lambda b: b['review'].update(status='partial'),
                     lambda b: b['review'].update(reviewed_files=[]),
                     lambda b: b['review'].update(reviewed_files=['outside.py']),
                     lambda b: b['review'].update(reviewed_files=['main.py', 'main.py']),
                     lambda b: b['review'].update(reviewer='coordinator-fixture'),
                     lambda b: b['review'].update(findings=[{'path': 'main.py', 'line': True,
                                                           'level': 'must_fix', 'content': 'bad'}]),
                     lambda b: b['handoff'].update(status='stale'),
                     lambda b: b['assignment'].update(access='write'),
                     lambda b: b['assignment'].update(input={'head': 'old'})]
        for mutate in mutations:
            bundle = copy.deepcopy(self.bundle)
            mutate(bundle)
            self.write(bundle)
            with self.subTest(bundle=bundle), self.assertRaises(ValueError):
                context.report(self.packet, self.output)

    def test_raw_tampering_and_failed_verification_reject(self):
        self.write()
        (self.output / 'source-0.json').write_text('{}', encoding='utf-8')
        with self.assertRaisesRegex(ValueError, 'evidence changed'):
            context.report(self.packet, self.output)
        self.write()
        core.save(self.packet / 'verification/verification.json', {'status': 'failed', 'fingerprint': 'input'})
        with self.assertRaisesRegex(ValueError, 'full verification'):
            context.report(self.packet, self.output)

    def test_ingest_requires_intact_completed_package_and_current_source(self):
        directory = self.root / '.ar-workflow/agents' / self.identity
        raw_path = directory / 'review.json'
        core.save(raw_path, self.raw)
        core.save(directory / 'assignment.json', self.assignment)
        (directory / 'task.md').write_text('Owned fixture task', encoding='utf-8')
        self.handoff['result_claim'] = {'evidence': [raw_path.relative_to(self.root).as_posix()]}
        core.save(directory / 'handoff.json', self.handoff)
        entry = {'status': 'completed', 'assignment_sha256': core.digest(core.canonical(self.assignment)),
                 'task_sha256': core.digest((directory / 'task.md').read_bytes())}
        core.save(self.root / '.ar-workflow/agents/state.json', {'schema_version': 1, 'writer': None,
                                                              'assignments': {self.identity: entry}})
        with patch.object(core, 'input_state', return_value=self.state):
            result = context.ingest(self.root, self.packet, [raw_path], 'coordinator-fixture',
                                    self.packet / 'verification/verification.json')
            self.assertEqual(core.load(result)['decision'], 'pass')
            entry['status'] = 'active'
            core.save(self.root / '.ar-workflow/agents/state.json', {'schema_version': 1, 'writer': None,
                                                                  'assignments': {self.identity: entry}})
            with self.assertRaisesRegex(ValueError, 'completed evidence handoff'):
                context.ingest(self.root, self.packet, [raw_path], 'coordinator-fixture',
                               self.packet / 'verification/verification.json')
        with patch.object(core, 'input_state', return_value={}):
            with self.assertRaisesRegex(ValueError, 'stale'):
                context.ingest(self.root, self.packet, [raw_path], 'coordinator-fixture',
                               self.packet / 'verification/verification.json')
