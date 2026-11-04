"""Offline relay contracts using owned repositories; no paid or GUI calls."""
import copy
import io
from contextlib import redirect_stdout
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest.mock import patch

import test_workflow_context as fixtures
import workflow as core
import workflow_agents as agents
import workflow_relay as relay
import workflow_runtime as runtime


class RelayTests(unittest.TestCase):
    setUp = fixtures.ContextTests.setUp
    write = fixtures.ContextTests.write
    packet = fixtures.ContextTests.packet

    def complete(self, role='developer', access='write', evidence=None):
        directory = agents.prepare(self.root, 'first-task', role, 'fixture-agent', access,
                                   ['main.py'] if access == 'write' else [])
        self.write('.ar-workflow/owned-result.json', {'fixture': True})
        agents.handoff(self.root, directory.name, {'status': 'completed', 'summary': 'implemented fixture',
            'next_action': 'pass is only a claim',
            'evidence': evidence if evidence is not None else ['.ar-workflow/owned-result.json']})
        return directory

    def task(self, **overrides):
        data = {'schema_version': 1, 'change': 'first-task', 'scope': 'workspace',
                'input': core.input_state(self.root, 'workspace'), 'phase': 'local_ready',
                'verification': None, 'review': None}
        data.update(overrides)
        core.save(runtime.task_path(self.root, 'first-task'), data)
        return data

    def test_no_runtime_record_read_is_nonmutating_and_cli_works(self):
        before = core.input_state(self.root, 'workspace')
        with patch.object(core, 'ROOT', self.root), patch('sys.argv', ['workflow.py', 'relay', 'first-task']), \
                redirect_stdout(io.StringIO()) as output:
            self.assertEqual(core.main(), 0)
        self.assertIn('awaiting_task_start', output.getvalue())
        self.assertFalse(runtime.task_path(self.root, 'first-task').exists())
        self.assertFalse(agents.state_path(self.root).exists())
        self.assertEqual(before, core.input_state(self.root, 'workspace'))

    def test_completed_handoff_is_not_verification_and_is_embedded(self):
        directory = self.complete()
        value = relay.summary(self.root, 'first-task')
        self.assertEqual(value['latest_handoff']['integrity'], 'digest_checked')
        self.assertTrue(value['latest_handoff']['input_current'])
        self.assertEqual(value['verification'], 'not_available')
        next_pack = agents.prepare(self.root, 'first-task', 'verifier', 'verify-agent', 'read', [],
                                   from_handoff=directory.name)
        assignment = core.load(next_pack / 'assignment.json')
        self.assertEqual(assignment['from_handoff']['assignment'], directory.name)
        self.assertIn('Workflow relay', (next_pack / 'task.md').read_text(encoding='utf-8'))

    def test_changed_evidence_blocks_accept_and_read(self):
        directory = self.complete()
        self.write('.ar-workflow/owned-result.json', {'fixture': 'changed'})
        with self.assertRaisesRegex(ValueError, 'evidence changed'):
            relay.accept(self.root, 'first-task', directory.name, core.input_state(self.root, 'workspace'))
        self.assertEqual(relay.summary(self.root, 'first-task')['stage'], 'blocked')

    def test_changed_record_and_package_are_detected(self):
        directory = self.complete()
        path = directory / 'handoff.json'
        original = path.read_bytes()
        data = core.load(path)
        data['result_claim']['next_action'] = 'claim gate passed'
        core.save(path, data)
        with self.assertRaisesRegex(ValueError, 'record changed'):
            relay.handoff(self.root, directory.name, 'first-task')
        path.write_bytes(original)
        (directory / 'task.md').write_text('changed', encoding='utf-8')
        with self.assertRaisesRegex(ValueError, 'package changed'):
            relay.handoff(self.root, directory.name, 'first-task')

    def test_cross_change_or_stale_accept_has_no_writer_side_effect(self):
        directory = self.complete()
        before = agents.state_path(self.root).read_bytes()
        with self.assertRaisesRegex(ValueError, 'another change'):
            agents.prepare(self.root, 'second-task', 'developer', 'dev', 'write', ['main.py'],
                           from_handoff=directory.name)
        self.write('main.py', 'changed input\n')
        with self.assertRaisesRegex(ValueError, 'stale'):
            agents.prepare(self.root, 'first-task', 'developer', 'dev', 'write', ['main.py'],
                           from_handoff=directory.name)
        self.assertEqual(before, agents.state_path(self.root).read_bytes())

    def test_handoff_change_during_export_is_not_published(self):
        directory = self.complete()
        before = agents.state_path(self.root).read_bytes()
        accepted = relay.accept(self.root, 'first-task', directory.name, core.input_state(self.root, 'workspace'))
        changed = dict(accepted, record_sha256='changed during export')
        with patch.object(relay, 'accept', side_effect=[accepted, changed]):
            with self.assertRaisesRegex(ValueError, 'changed during Agent context export'):
                agents.prepare(self.root, 'first-task', 'developer', 'next-dev', 'write', ['main.py'],
                               from_handoff=directory.name)
        self.assertEqual(before, agents.state_path(self.root).read_bytes())

    def test_legacy_handoff_is_visible_but_not_sealed_acceptance(self):
        directory = self.complete()
        state = agents.state(self.root)
        state['assignments'][directory.name].pop('handoff_sha256')
        core.save(agents.state_path(self.root), state)
        handoff = core.load(directory / 'handoff.json')
        handoff.pop('evidence_receipts')
        core.save(directory / 'handoff.json', handoff)
        self.assertEqual(relay.summary(self.root, 'first-task')['latest_handoff']['integrity'], 'legacy_unsealed')
        with self.assertRaisesRegex(ValueError, 'legacy-unsealed'):
            relay.accept(self.root, 'first-task', directory.name, core.input_state(self.root, 'workspace'))

    def test_other_change_global_writer_blocks_and_is_not_released(self):
        directory = agents.prepare(self.root, 'second-task', 'developer', 'other-dev', 'write', ['main.py'])
        value = relay.summary(self.root, 'first-task')
        self.assertEqual(value['stage'], 'writer_active')
        self.assertEqual(value['next']['commands'], [])
        self.assertEqual(agents.state(self.root)['writer'], directory.name)

    def test_phase_is_not_qualification_and_stale_task_not_overwritten(self):
        self.task()
        self.assertEqual(relay.summary(self.root, 'first-task')['stage'], 'awaiting_verification')
        self.write('main.py', 'new input\n')
        before = runtime.task_path(self.root, 'first-task').read_bytes()
        value = relay.summary(self.root, 'first-task')
        self.assertEqual(value['verification'], 'stale')
        self.assertEqual(value['stage'], 'blocked')
        self.assertIn('main.py', value['changed_paths_since_task'])
        self.assertEqual(before, runtime.task_path(self.root, 'first-task').read_bytes())

    def test_corrupt_task_and_missing_log_block_even_if_phase_local_ready(self):
        self.task(verification=str(self.root / '.ar-workflow/missing.json'))
        value = relay.summary(self.root, 'first-task')
        self.assertEqual(value['stage'], 'blocked')
        self.assertNotEqual(value['verification'], 'valid')

    def test_interrupted_review_requires_explicit_recovery(self):
        self.task(phase='reviewing')
        value = relay.summary(self.root, 'first-task')
        self.assertEqual(value['stage'], 'blocked')
        self.assertEqual(value['next']['commands'], [])

    def test_review_uses_existing_gate_and_does_not_execute_it_as_a_transition(self):
        data = self.task(verification='fixture', review='fixture')
        # Exercise the integration boundary without inventing an independent model review.
        with patch.object(runtime, 'check_task_verification', return_value=(self.root, {})), \
                patch.object(core, 'validate_packet', return_value=[]), \
                patch.object(runtime, 'task_gate', side_effect=ValueError('raw evidence changed')) as gate:
            value = relay.summary(self.root, 'first-task')
            self.assertEqual(value['stage'], 'blocked')
            self.assertNotEqual(value['review'], 'valid')
            gate.assert_called_once_with(self.root, data)
        with patch.object(runtime, 'check_task_verification', return_value=(self.root, {})), \
                patch.object(core, 'validate_packet', return_value=[]), patch.object(runtime, 'task_gate'):
            value = relay.summary(self.root, 'first-task')
            self.assertEqual(value['stage'], 'awaiting_gate')
            self.assertEqual(value['gate'], 'evidence_consistent_not_executed')
            self.assertEqual(core.load(runtime.task_path(self.root, 'first-task'))['phase'], 'local_ready')

    def test_shared_verification_checker_detects_log_tampering(self):
        tools_environment = patch.dict('os.environ', {'AR_WORKFLOW_TOOLS_ROOT': str(self.root)})
        tools_environment.start()
        self.addCleanup(tools_environment.stop)
        packet = self.root / '.ar-workflow/verify-packet'
        packet.mkdir(parents=True)
        data = self.task(packet=str(packet), verification=str(packet / 'verify/verification.json'))
        core.save(packet / 'manifest.json', {'state': data['input'], 'scope': data['scope'],
                  'fingerprint': 'fixture-fingerprint', 'task_context': {'change': 'first-task'}})
        logs = packet / 'verify'
        logs.mkdir()
        checks = []
        for index, command in enumerate(runtime.check_commands(self.root)):
            path = logs / f'check-{index}.log'
            path.write_bytes(b'owned deterministic fixture')
            checks.append({'command': command, 'passed': True, 'exit': 0, 'log_sha256': core.digest(path.read_bytes())})
        core.save(logs / 'verification.json', {'status': 'passed', 'fingerprint': 'fixture-fingerprint', 'checks': checks})
        self.assertEqual(runtime.check_task_verification(self.root, data)[0], packet)
        (logs / 'check-0.log').write_bytes(b'changed')
        with self.assertRaisesRegex(ValueError, 'log was changed'):
            runtime.check_task_verification(self.root, data)

    def test_markdown_contains_current_identity(self):
        text = relay.markdown(relay.summary(self.root, 'first-task'))
        self.assertIn('first-task', text)
        self.assertIn('工作区输入', text)


if __name__ == '__main__':
    unittest.main()
