"""Owned compact-package/transfer fixtures, no network or model calls."""
import json
import unittest
from unittest.mock import patch, MagicMock

import test_workflow_context as fixtures
import workflow as core
import workflow_agents as agents
import workflow_context as context
import workflow_packages as packages
import workflow_relay as relay
import workflow_transfer as transfer


class PackageTests(unittest.TestCase):
    setUp = fixtures.ContextTests.setUp
    write = fixtures.ContextTests.write

    def package(self):
        return agents.prepare(self.root, 'first-task', 'developer', 'dev', 'write', ['main.py'])

    def legacy(self, directory):
        state = agents.state(self.root)
        assignment = core.load(directory / 'assignment.json')
        for key in ('package_format', 'package_manifest_sha256', 'relay_sha256'):
            assignment.pop(key)
        core.save(directory / 'assignment.json', assignment)
        state['assignments'][directory.name]['assignment_sha256'] = core.digest(core.canonical(assignment))
        core.save(agents.state_path(self.root), state)

    def test_compact_front_keeps_every_original_byte_and_dsh_prompt_is_short(self):
        marker = 'REFERENCE CONTENT MUST NOT BE INLINE ' * 1000
        self.write('reference.md', marker)
        self.config['documents'] = [{'path':'reference.md','purpose':'reference','status':'reference','basis':'fixture'}]
        self.write('openspec/changes/first-task/context.json', self.config)
        directory = self.package()
        _, assignment, _ = agents.inspect(self.root, directory.name)
        self.assertEqual((directory/'basis/reference.md').read_bytes(), marker.encode())
        prompt = packages.prompt(directory, assignment)
        self.assertNotIn(marker, prompt)
        self.assertLess(len(prompt.encode()), 12000)
        self.assertIn('package-manifest.json', prompt)
        manifest = core.load(directory/'package-manifest.json')
        self.assertEqual(len(manifest['documents']),len(assignment['context']['documents']))
        self.assertGreater(manifest['statistics']['frozen_bytes'],len(prompt.encode()))

    def test_missing_and_changed_frozen_data_block_inspect_and_relay(self):
        directory = self.package()
        path = directory/'basis/main.py'  # Use one actual frozen basis, not source targets.
        manifest=core.load(directory/'package-manifest.json')
        path=directory/manifest['documents'][0]['frozen_path']
        original=path.read_bytes();path.write_bytes(b'tampered')
        with self.assertRaisesRegex(ValueError,'Frozen package document changed'):
            agents.inspect(self.root,directory.name)
        self.assertTrue(relay.summary(self.root,'first-task')['blockers'])
        path.write_bytes(original);path.unlink()
        with self.assertRaises(OSError):agents.inspect(self.root,directory.name)

    def test_manifest_omission_cannot_hide_a_document(self):
        directory=self.package();p=directory/'package-manifest.json'
        value=core.load(p);value['documents'].pop();core.save(p,value)
        with self.assertRaisesRegex(ValueError,'manifest/relay changed'):
            agents.inspect(self.root,directory.name)

    def test_legacy_package_uses_original_checks(self):
        directory=self.package();self.legacy(directory)
        body='Original legacy inline basis\nfixture document AGENTS.md\n'
        (directory/'task.md').write_text(body,encoding='utf-8')
        state=agents.state(self.root)
        state['assignments'][directory.name]['task_sha256']=core.digest((directory/'task.md').read_bytes())
        core.save(agents.state_path(self.root),state)
        agents.inspect(self.root,directory.name)
        _,assignment,_=agents.inspect(self.root,directory.name)
        self.assertIn(body,packages.prompt(directory,assignment))

    def test_dsh_admission_sends_only_front_not_full_frozen_content(self):
        import workflow_dsh as dsh
        self.write('reference.md','LARGE_REFERENCE_MARKER '*2000)
        self.config['documents']=[{'path':'reference.md','purpose':'reference','status':'reference','basis':'fixture'}]
        self.write('openspec/changes/first-task/context.json',self.config)
        directory=self.package();client=MagicMock();client.process.pid=123;client.stop.return_value=True
        client.call.side_effect=ValueError('fixture capture only')
        with patch('workflow_review_api.credential',return_value='owned-fixture-key'), \
                patch.object(dsh,'launch',return_value=client),patch.object(dsh,'initialize'):
            with self.assertRaisesRegex(ValueError,'blocked'):
                dsh.run(self.root,directory.name,self.root,'deepseek-flash',100000,10,32)
        text=client.call.call_args.args[1]['contentBlocks'][0]['text']
        self.assertNotIn('LARGE_REFERENCE_MARKER',text);self.assertLess(len(text.encode()),12000)

    def test_review_import_rejects_missing_frozen_basis_before_qualification(self):
        import workflow_review_context as review
        directory=agents.prepare(self.root,'first-task','reviewer','independent-fixture','read',[])
        raw=directory/'review.json';core.save(raw,{'assignment':directory.name})
        agents.handoff(self.root,directory.name,{'status':'completed','summary':'fixture only','next_action':'inspect',
            'evidence':[raw.relative_to(self.root).as_posix()]})
        packet=self.root/'.ar-workflow/reviews/owned-packet';packet.mkdir(parents=True)
        core.save(packet/'manifest.json',{'state':core.input_state(self.root,'workspace'),'scope':'workspace'})
        item=core.load(directory/'package-manifest.json')['documents'][0]
        (directory/item['frozen_path']).unlink()
        with self.assertRaises(OSError):review.ingest(self.root,packet,[raw],'coordinator',packet/'unused.json')

    def test_agent_scope_does_not_change_formal_review_native_routing(self):
        self.write('unrelated.cpp','// unrelated\n')
        self.write('docs/workflow/当前决策与待办清单.md','current fixture facts, not approval')
        assignment=context.for_assignment(self.root,'first-task')
        self.assertNotIn(self.rule['native_skill'],assignment['skills'])
        review=context.build(self.root,'first-task',['unrelated.cpp'])
        self.assertIn(self.rule['native_skill'],review['skills'])
        self.config['tags']=['native'];self.write('openspec/changes/first-task/context.json',self.config)
        self.assertIn(self.rule['native_skill'],context.for_assignment(self.root,'first-task')['skills'])
        self.assertIn('AGENTS.md',[x['path'] for x in assignment['documents']])
        self.assertIn('docs/workflow/当前决策与待办清单.md',[x['path'] for x in assignment['documents']])

    def test_transfer_is_not_send_and_accept_is_explicit_current_receipt(self):
        self.assertEqual(transfer.material(self.root,'first-task')['status'],'unassigned')
        directory=self.package()
        value=transfer.material(self.root,'first-task',directory.name)
        self.assertEqual(value['status'],'generated');self.assertEqual(value['sent'],'not_recorded')
        with self.assertRaisesRegex(ValueError,'intended agent'):
            transfer.accept(self.root,directory.name,'wrong-agent')
        transfer.accept(self.root,directory.name,'dev')
        self.assertEqual(transfer.material(self.root,'first-task',directory.name)['status'],'accepted_local_receipt')
        self.assertFalse(core.load(directory/'accept.json')['qualification'])
        self.assertEqual(agents.state(self.root)['writer'],directory.name)

    def test_stale_accept_and_cross_change_transfer_fail(self):
        directory=self.package()
        with self.assertRaisesRegex(ValueError,'another change'):
            transfer.material(self.root,'second-task',directory.name)
        self.write('main.py','changed\n')
        with self.assertRaisesRegex(ValueError,'unchanged input'):
            transfer.accept(self.root,directory.name,'dev')
        self.assertFalse((directory/'accept.json').exists())

    def test_relay_uses_handoff_time_not_assignment_creation_order(self):
        writer=self.package()
        reader=agents.prepare(self.root,'first-task','verifier','reader','read',[])
        result={'status':'completed','summary':'owned fixture','next_action':'inspect','evidence':[]}
        agents.handoff(self.root,reader.name,result)
        self.write('main.py','writer final input\n')
        agents.handoff(self.root,writer.name,result)
        value=relay.summary(self.root,'first-task')
        self.assertEqual(value['latest_handoff']['assignment'],writer.name)
        self.assertTrue(value['latest_handoff']['input_current'])


if __name__=='__main__':unittest.main()
