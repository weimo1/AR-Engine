"""Real archive/restore only in owned temporary repositories."""
import json
import os
from pathlib import Path
import unittest
from unittest.mock import patch

import test_workflow_context as fixtures
import workflow as core
import workflow_agents as agents
import workflow_temporary as temporary


class TemporaryTests(unittest.TestCase):
    setUp=fixtures.ContextTests.setUp
    write=fixtures.ContextTests.write

    def payload(self,text='owned scratch'):
        directory=agents.prepare(self.root,'first-task','developer','dev','write',['main.py'])
        record=temporary.create(self.root,directory.name,text)
        agents.handoff(self.root,directory.name,{'status':'completed','summary':'fixture done',
            'next_action':'preview only','evidence':[]})
        return directory.name,record

    def plan(self,owner):
        value=temporary.preview(self.root,owner)
        path=self.write('.ar-workflow/temporary/plans/test.json',value)
        return path,value

    def test_preview_is_readonly_then_archive_and_restore_exact_bytes(self):
        owner,record=self.payload();source=self.root/record['path']
        before=source.read_bytes();state=agents.state_path(self.root).read_bytes()
        plan,value=self.plan(owner)
        self.assertEqual(len(value['candidates']),1)
        self.assertEqual(before,source.read_bytes());self.assertEqual(state,agents.state_path(self.root).read_bytes())
        result=temporary.archive(self.root,plan)
        self.assertEqual(result['archived'],1);self.assertFalse(source.exists())
        self.assertEqual(temporary.restore(self.root,Path(result['journal']))['restored'],1)
        self.assertEqual(source.read_bytes(),before)

    def test_active_writer_and_unknown_process_protect(self):
        directory=agents.prepare(self.root,'first-task','developer','dev','write',['main.py'])
        temporary.create(self.root,directory.name,'owned')
        self.assertTrue(temporary.preview(self.root,directory.name)['blockers'])
        agents.handoff(self.root,directory.name,{'status':'completed','summary':'done','next_action':'preview','evidence':[]})
        self.write('.ar-workflow/dsh-bridge/jobs/fixture/run.json',{'runtime_stopped':False})
        self.assertTrue(temporary.preview(self.root,directory.name)['blockers'])

    def test_evidence_reference_and_raw_usage_are_protected(self):
        owner,record=self.payload()
        self.write('.ar-workflow/tasks/other.json',{'evidence':record['path']})
        value=temporary.preview(self.root,owner)
        self.assertEqual(value['candidates'],[]);self.assertIn('Referenced',value['protected'][0]['reason'])
        # An unrelated owned fixture covers protected content without deleting records.
        other,usage=self.payload('{"usage":{"total_tokens":1}}')
        value=temporary.preview(self.root,other)
        self.assertEqual(value['candidates'],[]);self.assertIn('protected',value['protected'][0]['reason'])

    def test_stale_content_and_workspace_plan_are_rejected(self):
        owner,record=self.payload();plan,_=self.plan(owner)
        (self.root/record['path']).write_text('changed')
        with self.assertRaisesRegex(ValueError,'stale'):
            temporary.archive(self.root,plan)
        self.assertTrue((self.root/record['path']).exists())

    def test_reference_added_after_preview_blocks_archive(self):
        owner,record=self.payload();plan,_=self.plan(owner)
        self.write('.ar-workflow/new-reference.json',{'temporary_id':record['id']})
        with self.assertRaisesRegex(ValueError,'stale'):
            temporary.archive(self.root,plan)

    def test_case_and_json_escaped_references_are_protected(self):
        owner,record=self.payload()
        value='{"ref":"'+''.join('\\u%04x'%ord(c) for c in record['id'].upper())+'"}'
        self.write('.ar-workflow/escaped-reference.json',value)
        self.assertEqual(temporary.preview(self.root,owner)['candidates'],[])

    def test_forged_plan_cannot_move_evidence(self):
        owner,record=self.payload();plan,value=self.plan(owner)
        value['candidates'][0]['path']='.ar-workflow/agents/state.json';core.save(plan,value)
        before=agents.state_path(self.root).read_bytes()
        with self.assertRaisesRegex(ValueError,'stale'):temporary.archive(self.root,plan)
        self.assertEqual(before,agents.state_path(self.root).read_bytes())

    def test_unreadable_reference_blocks_candidate(self):
        owner,record=self.payload();self.write('.ar-workflow/locked.note','opaque')
        original=Path.open
        def guarded(path,*args,**kwargs):
            if path.name=='locked.note':raise PermissionError('unknown reference')
            return original(path,*args,**kwargs)
        with patch.object(Path,'open',guarded):
            value=temporary.preview(self.root,owner)
        self.assertEqual(value['candidates'],[]);self.assertTrue(value['protected'])

    def test_paths_and_symlinks_rejected(self):
        for path in ('../outside','.ar-workflow/temporary/../agents','.ar-workflow/temporary./x'):
            with self.subTest(path=path),self.assertRaises(ValueError):temporary.safe(self.root,path)
        target=self.root/'target';target.mkdir()
        link=self.root/'.ar-workflow/link';link.parent.mkdir(exist_ok=True)
        try:link.symlink_to(target,target_is_directory=True)
        except OSError:self.skipTest('Real symlink creation is unavailable on this host')
        with self.assertRaisesRegex(ValueError,'reparse'):temporary.safe(self.root,'.ar-workflow/link/file')

    def test_reparse_check_and_changed_workspace_plan(self):
        self.write('.ar-workflow/ordinary/file','owned')
        with patch.object(Path,'is_junction',return_value=True):
            with self.assertRaisesRegex(ValueError,'reparse'):temporary.safe(self.root,'.ar-workflow/ordinary/file')
        owner,record=self.payload();plan,_=self.plan(owner)
        self.write('main.py','new workspace input\n')
        with self.assertRaisesRegex(ValueError,'stale'):temporary.archive(self.root,plan)

    def test_forged_unregistered_record_is_protected(self):
        owner,record=self.payload()
        state=agents.state(self.root);state['assignments'][owner]['temporary_created'].clear()
        core.save(agents.state_path(self.root),state)
        result=temporary.preview(self.root,owner)
        self.assertEqual(result['candidates'],[]);self.assertIn('not registered',result['protected'][0]['reason'])

    def test_restore_rejects_changed_archived_bytes(self):
        owner,record=self.payload();plan,_=self.plan(owner)
        result=temporary.archive(self.root,plan);journal=core.load(Path(result['journal']))
        (self.root/journal['items'][0]['target']).write_bytes(b'changed archive')
        with self.assertRaisesRegex(ValueError,'content changed'):temporary.restore(self.root,Path(result['journal']))

    def test_restore_preserves_new_evidence_references(self):
        owner,record=self.payload();plan,_=self.plan(owner)
        result=temporary.archive(self.root,plan);journal=Path(result['journal'])
        original=journal.read_bytes()
        self.write('.ar-workflow/consumer.json',{'evidence':str(journal)})
        with self.assertRaisesRegex(ValueError,'referenced archive evidence'):
            temporary.restore(self.root,journal)
        self.assertEqual(journal.read_bytes(),original)
        self.assertFalse((self.root/record['path']).exists())

    def test_restore_handles_interruption_after_rename_before_record_update(self):
        owner,record=self.payload();plan,_=self.plan(owner)
        result=temporary.archive(self.root,plan);journal=Path(result['journal'])
        core.save(self.root/record['path'].replace('/payload','/record.json'),record)
        value=core.load(journal);value['status']='in_progress';core.save(journal,value)
        self.assertEqual(temporary.restore(self.root,journal)['restored'],1)
        self.assertEqual((self.root/record['path']).read_text(),'owned scratch')

    def test_restore_still_works_if_preview_plan_was_lost(self):
        owner,record=self.payload();plan,_=self.plan(owner)
        result=temporary.archive(self.root,plan);plan.unlink()
        self.assertEqual(temporary.restore(self.root,Path(result['journal']))['restored'],1)

    def test_nested_and_truncated_accounting_are_never_candidates(self):
        for text in ('[{"nested":{"usage":{}}}]','{"usage":{'):
            owner,record=self.payload(text)
            self.assertEqual(temporary.preview(self.root,owner)['candidates'],[])

    def test_restore_rejects_conflict_and_changed_archive(self):
        owner,record=self.payload();plan,_=self.plan(owner)
        result=temporary.archive(self.root,plan);journal=Path(result['journal'])
        source=self.root/record['path'];source.write_text('new file')
        with self.assertRaisesRegex(ValueError,'conflict'):temporary.restore(self.root,journal)
        self.assertEqual(source.read_text(),'new file')

    def test_unmanaged_history_is_not_adopted(self):
        owner,_=self.payload()
        self.write('.ar-workflow/historical.log','do not move')
        plan,_=self.plan(owner);temporary.archive(self.root,plan)
        self.assertTrue((self.root/'.ar-workflow/historical.log').exists())


if __name__=='__main__':unittest.main()
