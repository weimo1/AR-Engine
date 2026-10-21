"""Owned offline task/Agent fixtures; no credentials, model, or desktop effects."""
import copy
import json
import os
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).parents[1]))
import workflow as core
import workflow_agents as agents
import workflow_context as context
import workflow_review_api as api
import workflow_runtime as runtime
import workflow_findings as adjudication


class ContextTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='ar-task-context-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        core.run(['git', 'init', '--quiet', '--initial-branch=main', str(self.root)])
        self.write('.gitignore', '.ar-workflow/\n')
        self.rule = core.load(core.ROOT / context.POLICY)
        self.write(context.POLICY, self.rule)
        for item in self.rule['base_documents'] + self.rule['native_documents']:
            self.write(item['path'], 'fixture document ' + item['path'])
        skills = list(self.rule['tag_skills'].values()) + [self.rule['native_skill'], self.rule['communication_skill']]
        for name in skills:
            self.write(f'.agents/skills/{name}/SKILL.md', 'fixture skill ' + name)
        self.write('workflow/tool-lock.json', {'ocr': {'api_model': 'deepseek-flash'}})
        self.write('.opencodereview/rule.json', {})
        self.write('main.py', "print('owned fixture')\n")
        self.config = {'schema_version': 1, 'objective': 'Fixture objective', 'non_goals': ['No remote writes'],
                       'tags': [], 'skills': [], 'documents': [], 'write_paths': ['main.py']}
        for change in ('first-task', 'second-task'):
            for name in ('proposal.md', 'design.md', 'test-plan.md', 'tasks.md'):
                self.write(f'openspec/changes/{change}/{name}', f'{change} {name}')
            self.write(f'openspec/changes/{change}/context.json', self.config)

    def write(self, name, body):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(body, ensure_ascii=False) if isinstance(body, dict) else body, encoding='utf-8')
        return path

    def packet(self, change):
        # The fixture root itself is the immutable source copied into a packet.
        import shutil
        packet = self.root / '.ar-workflow' / ('packet-' + change)
        shutil.copytree(self.root, packet / 'snapshot', ignore=shutil.ignore_patterns('.ar-workflow', '.git'))
        source = packet / 'snapshot'
        files = {p.relative_to(source).as_posix(): core.digest(p.read_bytes())
                 for p in source.rglob('*') if p.is_file()}
        preview = {'reviewable_files': [{'path': 'main.py'}], 'excluded_files': [],
                   'reviewable_count': 1, 'excluded_count': 0, 'total_files': 1}
        core.save(packet / 'ocr-preview.json', preview)
        (packet / 'diff.patch').write_bytes(b'fixture diff')
        manifest = {'scope': 'workspace', 'state': {'head': None, 'files': files},
                    'reviewable_files': ['main.py'], 'preview_sha256': core.digest((packet / 'ocr-preview.json').read_bytes()),
                    'diff_sha256': core.digest(b'fixture diff'),
                    'task_context': context.build(source, change, ['main.py'])}
        manifest['fingerprint'] = core.digest(core.canonical(core.review_identity(manifest)))
        core.save(packet / 'manifest.json', manifest)
        return packet, manifest

    def test_current_task_and_skill_bodies_reach_independent_review(self):
        packet, manifest = self.packet('first-task')
        self.assertEqual(core.validate_packet(packet, manifest), [])
        request = api.requests_for(packet)[0]
        payload = json.loads(request['messages'][1]['content'])
        self.assertEqual(payload['task_basis']['change'], 'first-task')
        self.assertIn('openspec/changes/first-task/design.md', payload['confirmed_context'])
        self.assertNotIn('openspec/changes/second-task/design.md', payload['confirmed_context'])
        self.assertNotIn('complete-local-workflow', json.dumps(payload))
        self.assertIn('.agents/skills/ar-employee-chat/SKILL.md', payload['confirmed_context'])

    def test_context_anchored_finding_keeps_original_context_in_adjudication(self):
        self.write('scripts/helper.py', 'pass  # frozen caller context\n')
        packet, manifest = self.packet('first-task')
        request = api.requests_for(packet)[0]
        path = 'scripts/helper.py'
        raw = {'model': 'deepseek-flash', 'choices': [{'finish_reason': 'stop', 'message': {'content': json.dumps({
            'reviewed_files': ['main.py'], 'findings': [{'path': path, 'line': 1, 'level': 'must_fix', 'content': 'fixture claim'}]})}}],
            'usage': {'total_tokens': 12, 'prompt_tokens': 10, 'completion_tokens': 2}}
        output = packet / 'direct-fixture'
        core.save(output / 'response-0.json', raw)
        core.save(output / 'run.json', {'mode': 'direct-api-v1', 'status': 'finished', 'fingerprint': manifest['fingerprint'],
            'token_budget': 100000, 'batches': [{'request_sha256': core.digest(core.canonical(request)),
            'response_sha256': core.digest((output / 'response-0.json').read_bytes()), 'actual_tokens': 12}]})
        report_path = output / 'report.json'
        core.save(report_path, api.report(packet, output))
        example = core.load(adjudication.prepare(report_path, None))
        example['items'][0].update(argument='Counterevidence is a claim only', evidence=[])
        evidence = output / 'rebuttals.json'
        core.save(evidence, example)
        directory = adjudication.prepare(report_path, evidence)
        payload = json.loads(core.load(directory / 'request.json')['messages'][1]['content'])
        self.assertEqual(len(payload['frozen_contexts']), 1)
        self.assertIn(path, payload['frozen_contexts'][0]['frozen_files'])

    def test_same_source_different_tasks_have_distinct_identity_and_requests(self):
        p1, m1 = self.packet('first-task')
        p2, m2 = self.packet('second-task')
        self.assertEqual(m1['state'], m2['state'])
        self.assertNotEqual(m1['fingerprint'], m2['fingerprint'])
        self.assertNotEqual(api.requests_for(p1), api.requests_for(p2))

    def test_native_and_explicit_tags_route_skills_keep_architecture_reference(self):
        self.config['tags'] = ['performance', 'third_party', 'integration']
        self.write('openspec/changes/first-task/context.json', self.config)
        basis = context.build(self.root, 'first-task', ['engine/render.cpp'])
        for name in ('ar-cpp-oop-style', 'ar-cpp-hpc-optimization', 'ar-context7', 'ar-e2e-side-effect-safety'):
            self.assertIn(name, basis['skills'])
        architecture = next(x for x in basis['documents'] if x['purpose'] == 'architecture')
        self.assertEqual(architecture['status'], 'reference')
        plain = context.build(self.root, 'second-task', ['scripts/tool.py'])
        self.assertNotIn('ar-cpp-oop-style', plain['skills'])

    def test_declared_native_work_loads_basis_before_source_changes(self):
        for selection in ({'tags': ['native']}, {'skills': ['ar-cpp-oop-style']},
                          {'write_paths': ['src/*.cpp']}):
            config = copy.deepcopy(self.config)
            config.update(selection)
            self.write('openspec/changes/first-task/context.json', config)
            basis = context.build(self.root, 'first-task', [])
            self.assertIn('ar-cpp-oop-style', basis['skills'])
            self.assertTrue(any(item['purpose'] == 'requirement' for item in basis['documents']))
            self.assertTrue(any(item['purpose'] == 'architecture' and item['status'] == 'reference'
                                for item in basis['documents']))

    def test_missing_escape_unknown_tag_and_draft_promotion_rejected(self):
        for document in (
                {'path': '../outside.md', 'purpose': 'standard', 'status': 'confirmed', 'basis': 'fixture'},
                {'path': 'missing.md', 'purpose': 'standard', 'status': 'confirmed', 'basis': 'fixture'},
                {'path': 'draft.md', 'purpose': 'architecture', 'status': 'confirmed', 'basis': 'fixture'}):
            self.write('draft.md', '状态：草案。\nNot approved.')
            self.config['documents'] = [document]
            self.write('openspec/changes/first-task/context.json', self.config)
            with self.assertRaises(ValueError):
                context.build(self.root, 'first-task', ['main.py'])
        self.config.update(documents=[], tags=['unknown'])
        self.write('openspec/changes/first-task/context.json', self.config)
        with self.assertRaisesRegex(ValueError, 'Unknown'):
            context.build(self.root, 'first-task', ['main.py'])

    def test_context_tampering_and_missing_skill_fail_before_api(self):
        packet, manifest = self.packet('first-task')
        path = packet / 'snapshot/.agents/skills/ar-employee-chat/SKILL.md'
        path.write_text('changed source', encoding='utf-8')
        with patch.object(api, 'credential') as key:
            with self.assertRaisesRegex(ValueError, 'Invalid frozen'):
                api.requests_for(packet)
            key.assert_not_called()
        path.unlink()
        self.assertTrue(context.validate(packet, manifest))

    def test_legacy_identity_does_not_gain_new_fields(self):
        manifest = {'scope': 'workspace', 'state': {}, 'reviewable_files': []}
        before = core.review_identity(manifest)
        self.assertNotIn('task_context', before)
        self.assertEqual(context.validate(self.root, manifest), [])

    def test_current_task_gate_rejects_another_change_or_generic_packet(self):
        packet, manifest = self.packet('first-task')
        data = {'change': 'second-task', 'scope': 'workspace', 'input': manifest['state'], 'packet': str(packet),
                'verification': str(packet / 'verification.json'), 'review': str(packet / 'direct-fixture/report.json')}
        with self.assertRaisesRegex(ValueError, 'current task context'):
            runtime.task_gate(self.root, data)
        del manifest['task_context']
        core.save(packet / 'manifest.json', manifest)
        data['change'] = 'first-task'
        with self.assertRaisesRegex(ValueError, 'current task context'):
            runtime.task_gate(self.root, data)

    def test_native_deletion_context_keeps_cpp_skill(self):
        basis = context.build(self.root, 'first-task', ['.ar-review-context/deletions/old.md', 'src/old.cpp'])
        self.assertIn('ar-cpp-oop-style', basis['skills'])

    def test_partially_confirmed_document_is_not_rejected_as_draft(self):
        self.write('partial.md', '状态：草案 1 的已列条款已确认。其他内容仍待确认。')
        self.config['documents'] = [{'path': 'partial.md', 'purpose': 'standard',
                                     'status': 'confirmed', 'basis': 'Only listed confirmed clauses apply'}]
        self.write('openspec/changes/first-task/context.json', self.config)
        self.assertTrue(context.build(self.root, 'first-task', ['main.py']))

    def test_ignored_agent_history_does_not_change_request_identity(self):
        packet, manifest = self.packet('first-task')
        before = api.requests_for(packet)
        core.save(self.root / '.ar-workflow/agents/unrelated.json', {'timestamp': 'irrelevant'})
        self.assertEqual(api.requests_for(packet), before)

    def test_writer_is_global_and_blocks_verification_until_handoff(self):
        package = agents.prepare(self.root, 'first-task', 'developer', 'dev-1', 'write', ['main.py'])
        with self.assertRaisesRegex(ValueError, 'writer'):
            agents.prepare(self.root, 'second-task', 'developer', 'dev-2', 'write', ['main.py'])
        with self.assertRaisesRegex(ValueError, 'writer'):
            core.prepare_review(self.root)
        for command in ('verify', 'review', 'commit-check'):
            with self.assertRaisesRegex(ValueError, 'writer'):
                runtime.dispatch(SimpleNamespace(command=command), self.root)
        self.write('main.py', "print('updated by owned writer')\n")
        result = {'status': 'completed', 'summary': 'implemented fixture', 'next_action': 'verify independently', 'evidence': ['main.py']}
        handoff = agents.handoff(self.root, package.name, result)
        self.assertEqual(core.load(handoff)['status'], 'completed')
        self.assertFalse(core.load(handoff)['grants_delivery_qualification'])
        agents.assert_idle(self.root)
        with self.assertRaisesRegex(ValueError, 'already'):
            agents.handoff(self.root, package.name, result)

    def test_internal_windows_aliases_cannot_be_declared_as_write_scope(self):
        for name in ('.GiT/config', '.AR-WORKFLOW/agents/state.json', '.git./config', '.git /config'):
            self.config['write_paths'] = [name]
            self.write('openspec/changes/first-task/context.json', self.config)
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, 'Git/internal'):
                agents.prepare(self.root, 'first-task', 'developer', 'dev-1', 'write', [name])

    def test_readonly_roles_cannot_write_and_scope_cannot_expand(self):
        with self.assertRaisesRegex(ValueError, 'read-only'):
            agents.prepare(self.root, 'first-task', 'reviewer', 'review-1', 'write', ['main.py'])
        for paths in (['*'], ['other.py'], ['../escape'], ['.git'], ['.git/config'], ['.ar-workflow/x'],
                      ['.GiT/config'], ['.AR-WORKFLOW/agents/state.json'], ['.git./config'], ['.git /config']):
            with self.assertRaises(ValueError):
                agents.prepare(self.root, 'first-task', 'developer', 'dev-1', 'write', paths)
        self.config['write_paths'] = ['*']
        self.write('openspec/changes/first-task/context.json', self.config)
        with self.assertRaisesRegex(ValueError, 'bounded'):
            agents.prepare(self.root, 'first-task', 'developer', 'dev-1', 'write', ['.git/config'])

    def test_readonly_stale_and_out_of_scope_writer_do_not_report_completed(self):
        package = agents.prepare(self.root, 'first-task', 'reviewer', 'review-1', 'read', [])
        self.write('main.py', 'new input\n')
        result = {'status': 'completed', 'summary': 'claim only', 'next_action': 'recheck', 'evidence': []}
        handoff = core.load(agents.handoff(self.root, package.name, result))
        self.assertEqual(handoff['status'], 'stale')
        package = agents.prepare(self.root, 'first-task', 'developer', 'dev-1', 'write', ['main.py'])
        self.write('outside-scope.py', 'unexpected\n')
        handoff = core.load(agents.handoff(self.root, package.name, result))
        self.assertEqual(handoff['status'], 'blocked')
        self.assertIn('outside-scope.py', handoff['out_of_scope'])

    def test_modified_package_and_abnormal_release(self):
        package = agents.prepare(self.root, 'first-task', 'developer', 'dev-1', 'write', ['main.py'])
        original = (package / 'task.md').read_bytes()
        (package / 'task.md').write_text('permission escalation', encoding='utf-8')
        result = {'status': 'blocked', 'summary': 'interrupted', 'next_action': 'inspect', 'evidence': []}
        with self.assertRaisesRegex(ValueError, 'package changed'):
            agents.handoff(self.root, package.name, result)
        (package / 'task.md').write_bytes(original)
        handoff = core.load(agents.handoff(self.root, package.name, result, 'User requested interruption'))
        self.assertEqual(handoff['status'], 'released')
        agents.assert_idle(self.root)

    def test_abnormal_release_records_unreadable_input_without_approval(self):
        package = agents.prepare(self.root, 'first-task', 'developer', 'dev-1', 'write', ['main.py'])
        result = {'status': 'blocked', 'summary': 'interrupted', 'next_action': 'repair input', 'evidence': []}
        with patch.object(core, 'input_state', side_effect=ValueError('unreadable fixture source')):
            with self.assertRaisesRegex(ValueError, 'unreadable'):
                agents.handoff(self.root, package.name, result)
            handoff = core.load(agents.handoff(self.root, package.name, result, 'Explicit abnormal recovery'))
        self.assertEqual(handoff['status'], 'released')
        self.assertIsNone(handoff['end_input'])
        self.assertIn('unreadable', handoff['end_input_unavailable'])
        self.assertFalse(handoff['grants_delivery_qualification'])


if __name__ == '__main__':
    unittest.main()
