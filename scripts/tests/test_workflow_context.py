"""Owned offline task/Agent fixtures; no credentials, model, or desktop effects."""
import copy
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).parents[1]))
import workflow as core
import workflow_context as context


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

    def test_same_source_different_tasks_have_distinct_identity(self):
        p1, m1 = self.packet('first-task')
        p2, m2 = self.packet('second-task')
        self.assertEqual(m1['state'], m2['state'])
        self.assertNotEqual(m1['fingerprint'], m2['fingerprint'])
        self.assertEqual(core.validate_packet(p1, m1), [])
        self.assertEqual(core.validate_packet(p2, m2), [])

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

    def test_context_tampering_and_missing_skill_reject_packet(self):
        packet, manifest = self.packet('first-task')
        path = packet / 'snapshot/.agents/skills/ar-employee-chat/SKILL.md'
        path.write_text('changed source', encoding='utf-8')
        self.assertTrue(context.validate(packet, manifest))
        path.unlink()
        self.assertTrue(context.validate(packet, manifest))

    def test_legacy_identity_does_not_gain_new_fields(self):
        manifest = {'scope': 'workspace', 'state': {}, 'reviewable_files': []}
        before = core.review_identity(manifest)
        self.assertNotIn('task_context', before)
        self.assertEqual(context.validate(self.root, manifest), [])

    def test_native_deletion_context_keeps_cpp_skill(self):
        basis = context.build(self.root, 'first-task', ['.ar-review-context/deletions/old.md', 'src/old.cpp'])
        self.assertIn('ar-cpp-oop-style', basis['skills'])

    def test_partially_confirmed_document_is_not_rejected_as_draft(self):
        self.write('partial.md', '状态：草案 1 的已列条款已确认。其他内容仍待确认。')
        self.config['documents'] = [{'path': 'partial.md', 'purpose': 'standard',
                                     'status': 'confirmed', 'basis': 'Only listed confirmed clauses apply'}]
        self.write('openspec/changes/first-task/context.json', self.config)
        self.assertTrue(context.build(self.root, 'first-task', ['main.py']))

if __name__ == '__main__':
    unittest.main()
