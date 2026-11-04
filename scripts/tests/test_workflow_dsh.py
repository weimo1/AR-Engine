"""Owned offline DSH protocol fixtures: no model, credentials or GUI access."""
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).parents[1]))
import workflow as core
import workflow_agents as agents
import workflow_dsh as dsh
from workflow_dsh_rpc import Client
import test_workflow_context as context_fixture


def event(seq, kind, data=None):
    data = dict(data or {})
    data.setdefault('turn', 0)
    return {'jsonrpc': '2.0', 'method': 'session.event', 'params': {'sessionId': 'owned',
        'event': {'type': kind, 'seq': seq, 'time': 0, 'data': data}}}


def message(seq=1, usage=None):
    return event(seq, 'assistant/message', {'message': {'role': 'assistant',
        'content': [{'type': 'text', 'text': 'Owned task done'}]}, 'usage': usage or
        {'inputTokens': 10, 'cacheReadTokens': 20, 'outputTokens': 3, 'reasoningTokens': 2, 'totalTokens': 33}})


class ProtocolTests(unittest.TestCase):
    def test_sdk_rejects_other_models_and_always_requests_high(self):
        with patch.object(dsh, 'Client') as client_type:
            client = client_type.return_value
            client.call.return_value = {'serverInfo': {'name': 'deepseek-harness-sdk-runtime'}}
            dsh.initialize(client, Path.cwd(), 'deepseek-flash', 1024)
            self.assertEqual(client.call.call_args.args[1]['reasoningEffort'], 'high')
            client.call.reset_mock()
            with self.assertRaisesRegex(ValueError, 'user-selected'):
                dsh.initialize(client, Path.cwd(), 'deepseek-v4-pro', 1024)
            client.call.assert_not_called()

    def test_paid_call_policy_rejects_old_model_and_disabled_or_lower_effort(self):
        from workflow_review_api import require_model_selection
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            core.save(root / 'workflow/tool-lock.json', {'ocr': {'api_model': 'deepseek-flash',
                'api_thinking': 'enabled', 'api_reasoning_effort': 'high'}})
            good = {'model': 'deepseek-flash', 'thinking': {'type': 'enabled'}, 'reasoning_effort': 'high'}
            require_model_selection(root, good)
            for field, value in (('model', 'deepseek-v4-pro'), ('thinking', {'type': 'disabled'}),
                                 ('reasoning_effort', 'low'), ('reasoning_effort', None)):
                with self.assertRaisesRegex(ValueError, 'no fallback'):
                    require_model_selection(root, dict(good, **{field: value}))

    def test_desktop_location_and_explicit_home_without_credentials(self):
        with tempfile.TemporaryDirectory() as temporary:
            home = Path(temporary).resolve()
            actual = home / 'relocated'
            (actual / 'sessions').mkdir(parents=True)
            (home / '.dsh').mkdir()
            core.save(home / '.dsh/desktop-next-location.json', {'home': str(actual)})
            with patch.object(Path, 'home', return_value=home):
                self.assertEqual(dsh.desktop_store(), {'root': str(actual / 'sessions'), 'compression': 'zstd'})
                self.assertEqual(dsh.desktop_store(actual, 'none')['compression'], 'none')
                core.save(home / '.dsh/desktop-next-location.json', {'home': '../bad'})
                with self.assertRaisesRegex(ValueError, 'Invalid DSH desktop location'):
                    dsh.desktop_store()

    def test_missing_desktop_store_never_falls_back_or_creates_it(self):
        with tempfile.TemporaryDirectory() as temporary:
            home = Path(temporary).resolve()
            with self.assertRaisesRegex(ValueError, 'Existing absolute'):
                dsh.desktop_store(home)
            self.assertFalse((home / 'sessions').exists())

    def test_launch_uses_private_home_and_official_shared_storage_overlay(self):
        import os
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary).resolve()
            install = base / 'install'
            (install / 'resources').mkdir(parents=True)
            (install / 'DSH NEXT.exe').write_bytes(b'fixture')
            (install / 'resources/app.asar').write_bytes(b'fixture archive')
            sessions = base / 'desktop sessions'
            sessions.mkdir()
            output = base / 'output'
            output.mkdir()
            with patch.object(dsh, 'Client') as client:
                store = {'root': str(sessions), 'compression': 'zstd'}
                dsh.launch(base, install, output, 10, 'fixture-key', store)
            args = client.call_args.args
            self.assertEqual(args[2]['DSH_HOME'], str(output / 'home'))
            self.assertEqual(args[2]['DEEPSEEK_API_KEY'], 'fixture-key')
            self.assertNotIn('fixture-key', ' '.join(args[0]))
            overlay = (output / 'no-retry.patch.yml').read_text(encoding='utf8')
            self.assertIn(json.dumps(str(sessions), ensure_ascii=False), overlay)
            self.assertIn('compression: zstd', overlay)
            self.assertNotIn('fixture-key', overlay)
            self.assertEqual(core.load(output / 'runtime.json')['visibility'], 'desktop')

    def test_reviewer_launch_enables_reads_disables_shell_and_preserves_desktop(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary).resolve()
            install = base / 'install'
            (install / 'resources').mkdir(parents=True)
            (install / 'DSH NEXT.exe').write_bytes(b'fixture')
            (install / 'resources/app.asar').write_bytes(b'fixture')
            sessions = base / 'sessions'
            sessions.mkdir()
            output = base / 'output'
            output.mkdir()
            with patch.object(dsh, 'Client') as client:
                dsh.launch(base, install, output, 10, store={'root': str(sessions),
                           'compression': 'zstd'}, reviewer=True)
            persona = client.call_args.args[2]['DSH_SYSTEM_PROMPT']
            self.assertIn('independent', persona)
            self.assertIn('covered and uncovered', persona)
            self.assertNotIn('execution agent', persona)
            overlay = (output / 'no-retry.patch.yml').read_text(encoding='utf8')
            for marker in ('mode: read-only', 'read: true', 'write: false', 'edit: false',
                           'id: terminal-pwsh\n  disabled: true',
                           'id: persistent-pwsh\n  disabled: true', 'compression: zstd'):
                self.assertIn(marker, overlay)
            self.assertFalse(core.load(output / 'runtime.json')['os_permission_isolation'])

    def test_usage_cache_and_reasoning_not_counted_twice(self):
        evidence = dsh.Evidence('owned', 100)
        evidence.observe(message())
        evidence.observe(message())
        self.assertEqual(evidence.total, 33)
        self.assertFalse(evidence.completed())
        evidence.observe(event(2, 'turn/end', {'reason': {'kind': 'completed'}}))
        self.assertTrue(evidence.completed())

    def test_failed_attempts_count_and_last_stream_usage_only(self):
        evidence = dsh.Evidence('owned', 100)
        evidence.observe(event(1, 'assistant/attempt', {'stream': [
            {'type': 'chunk', 'time': 0, 'chunk': {'type': 'usage', 'usage': {'inputTokens': 1, 'outputTokens': 1}}},
            {'type': 'chunk', 'time': 1, 'chunk': {'type': 'usage', 'usage': {'inputTokens': 5, 'outputTokens': 2}}}]}))
        self.assertEqual(evidence.total, 7)

    def test_noncompleted_and_unknown_usage_never_pass(self):
        for reason in ('max-tokens', 'error', 'aborted', 'blocked', 'interrupted', 'forked'):
            evidence = dsh.Evidence('owned', 100)
            evidence.observe(message())
            evidence.observe(event(2, 'turn/end', {'reason': {'kind': reason}}))
            self.assertFalse(evidence.completed())
        evidence = dsh.Evidence('owned', 100)
        frame = message()
        del frame['params']['event']['data']['usage']
        evidence.observe(frame)
        evidence.observe(event(2, 'turn/end', {'reason': {'kind': 'completed'}}))
        self.assertFalse(evidence.completed())
        self.assertTrue(evidence.unknown)

    def test_foreign_conflicting_events_and_invalid_usage_block(self):
        evidence = dsh.Evidence('owned', 100)
        evidence.observe(message())
        with self.assertRaisesRegex(ValueError, 'Conflicting'):
            evidence.observe(message(usage={'inputTokens': 12, 'outputTokens': 4}))
        with self.assertRaisesRegex(ValueError, 'foreign'):
            dsh.Evidence('other', 100).observe(message())
        for usage in ({'inputTokens': True, 'outputTokens': 4},
                      {'inputTokens': 3, 'outputTokens': 4, 'totalTokens': 99}):
            with self.assertRaises(ValueError):
                dsh.Evidence('owned', 100).observe(message(usage=usage))

    def test_threshold_keeps_consumed_usage(self):
        evidence = dsh.Evidence('owned', 30)
        with self.assertRaisesRegex(ValueError, 'threshold'):
            evidence.observe(message())
        self.assertEqual(evidence.total, 33)

    def test_explicit_absent_token_threshold_still_counts_usage_and_completion(self):
        evidence = dsh.Evidence('owned', None)
        evidence.observe(message(usage={'inputTokens': 2000000, 'outputTokens': 3}))
        evidence.observe(event(2, 'turn/end', {'reason': {'kind': 'completed'}}))
        self.assertEqual(evidence.total, 2000003)
        self.assertTrue(evidence.completed())

    def test_progress_text_does_not_replace_final_text_or_turn(self):
        evidence = dsh.Evidence('owned', 100)
        evidence.observe(message())
        final = message(2)
        final['params']['event']['data']['message']['content'] = [{'type': 'tool-call'}]
        evidence.observe(final)
        evidence.observe(event(3, 'turn/end', {'reason': {'kind': 'completed'}}))
        self.assertFalse(evidence.completed())
        other = dsh.Evidence('owned', 100)
        other.observe(message())
        other.observe(event(2, 'turn/end', {'turn': 1, 'reason': {'kind': 'completed'}}))
        self.assertFalse(other.completed())

    def test_unknown_notifications_and_events_cannot_complete(self):
        for frame in ({'method': 'future.cost', 'params': {}},
                      event(1, 'future/cost'), event(1, 'session/title-llm-request'),
                      {'method': 'session.status', 'params': {'sessionId': 'foreign'}}):
            with self.subTest(frame=frame):
                evidence = dsh.Evidence('owned', 1000)
                with self.assertRaises(ValueError):
                    evidence.observe(frame)
                self.assertFalse(evidence.completed())
        dsh.Evidence('owned', 1000).observe({'method': 'session.status', 'params': {'sessionId': 'owned', 'status': 'idle'}})

    def test_real_stdio_rejects_boolean_and_float_response_ids(self):
        import os
        for bad in ('True', '1.0'):
            script = """import sys,json
for line in sys.stdin:
 r=json.loads(line)
 identity=BAD if r['method']=='fixture' else r['id']
 print(json.dumps({'jsonrpc':'2.0','id':identity,'result':{}}),flush=True)
 if r['method']=='shutdown': break
""".replace('BAD', bad)
            with tempfile.TemporaryDirectory() as temporary:
                directory = Path(temporary)
                client = Client([sys.executable, '-u', '-c', script], directory, dict(os.environ), directory, 5)
                try:
                    with self.assertRaisesRegex(ValueError, 'Unexpected DSH RPC response'):
                        client.call('fixture', {})
                finally:
                    self.assertTrue(client.stop())

    def test_log_open_failure_never_spawns(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            (directory / 'stderr.log').mkdir()
            with patch('workflow_dsh_rpc.subprocess.Popen') as spawn:
                with self.assertRaises(OSError):
                    Client(['fixture'], directory, {}, directory, 1)
                spawn.assert_not_called()

    def test_real_stdio_identity_notifications_redaction_and_shutdown(self):
        script = '''import sys,json
for line in sys.stdin:
 r=json.loads(line)
 if r['method']=='initialize':
  print(json.dumps({'jsonrpc':'2.0','method':'fixture','params':{'text':'private-key'}}),flush=True)
 result={'serverInfo':{'name':'deepseek-harness-sdk-runtime'}} if r['method']=='initialize' else {}
 print(json.dumps({'jsonrpc':'2.0','id':r['id'],'result':result}),flush=True)
 if r['method']=='shutdown': break
'''
        import os
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            client = Client([sys.executable, '-X', 'utf8', '-u', '-c', script], directory, dict(os.environ), directory, 5, 'private-key')
            try:
                dsh.initialize(client, directory, 'deepseek-flash', 32)
            except ValueError as error:
                client.stop()
                raise AssertionError((directory / 'stderr.log').read_text()) from error
            finally:
                self.assertTrue(client.stop())
            self.assertNotIn('private-key', (directory / 'events.jsonl').read_text())

    def test_real_stdio_timeout_and_malformed_response(self):
        import os
        for script in ('import time; time.sleep(20)', 'print("not-json",flush=True); import time; time.sleep(20)'):
            with tempfile.TemporaryDirectory() as temporary:
                directory = Path(temporary)
                client = Client([sys.executable, '-u', '-c', script], directory, dict(os.environ), directory, 1)
                with self.assertRaises(ValueError):
                    client.call('initialize', {})
                self.assertTrue(client.stop())

    @unittest.skipUnless(sys.platform == 'win32', 'Windows Job ownership test')
    def test_root_exit_still_stops_descendant_and_named_job_recovers(self):
        import os
        import workflow_runtime as runtime
        import workflow_dsh_job as job
        script = '''import subprocess,sys,json,os
child=subprocess.Popen([sys.executable,'-c','import time; time.sleep(60)'])
r=json.loads(sys.stdin.readline())
print(json.dumps({'jsonrpc':'2.0','id':r['id'],'result':{'child':child.pid}}),flush=True)
os._exit(0)
'''
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            client = Client([sys.executable, '-X', 'utf8', '-u', '-c', script], directory,
                            dict(os.environ), directory, 5)
            try:
                response = client.call('fixture', {})
                client.process.wait(timeout=5)
                self.assertTrue(runtime.process_alive(response['child']))
            finally:
                self.assertTrue(client.stop())
            self.assertFalse(runtime.process_alive(response['child']))
            self.assertTrue(job.recover(directory))

    @unittest.skipUnless(sys.platform == 'win32', 'Windows owner death recovery')
    def test_controller_death_kills_owned_job_and_recovery_confirms(self):
        import subprocess
        import time
        import workflow_dsh_job as job
        import workflow_runtime as runtime
        script = '''import sys,time,os
from pathlib import Path
sys.path.insert(0,sys.argv[1])
from workflow_dsh_rpc import Client
d=Path(sys.argv[2])
c=Client([sys.executable,'-c','import time; time.sleep(60)'],d,dict(os.environ),d,60)
print(c.process.pid,flush=True)
time.sleep(60)
'''
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            parent = subprocess.Popen([sys.executable, '-X', 'utf8', '-u', '-c', script,
                str(Path(__file__).resolve().parents[1]), str(directory)],
                stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                child = int(parent.stdout.readline())
                self.assertTrue(runtime.process_alive(child))
                parent.kill()
                parent.wait(timeout=5)
                limit = time.monotonic() + 5
                while runtime.process_alive(child) and time.monotonic() < limit:
                    time.sleep(0.03)
                self.assertFalse(runtime.process_alive(child))
                self.assertTrue(job.recover(directory))
            finally:
                if parent.poll() is None:
                    parent.kill()
                parent.communicate(timeout=5)


class DispatchTests(unittest.TestCase):
    setUp = context_fixture.ContextTests.setUp
    write = context_fixture.ContextTests.write

    def prepare(self):
        return agents.prepare(self.root, 'first-task', 'developer', 'DSH fixture', 'write', ['main.py']).name

    def test_stale_readonly_and_budget_rejected_before_credentials(self):
        identity = self.prepare()
        self.write('main.py', 'changed')
        with patch('workflow_review_api.credential', side_effect=AssertionError('No key read')):
            with self.assertRaisesRegex(ValueError, 'unchanged'):
                dsh.run(self.root, identity, self.root, 'fixture', 100000, 10, 32)
        # Restore exact fixture input.
        self.write('main.py', "print('owned fixture')\n")
        with patch('workflow_review_api.credential', side_effect=AssertionError('No key read')):
            with self.assertRaisesRegex(ValueError, 'estimate'):
                dsh.run(self.root, identity, self.root, 'fixture', 1, 10, 32)

    def test_duplicate_and_live_runtime_block_handoff(self):
        identity = self.prepare()
        path = self.root / dsh.BASE / 'jobs' / ('a' * 24) / 'run.json'
        core.save(path, {'assignment': identity, 'runtime_stopped': False})
        with self.assertRaisesRegex(ValueError, 'already dispatched'):
            dsh.run(self.root, identity, self.root, 'fixture', 100000, 10, 32)
        for reason in (None, 'Explicit release must not stop active child implicitly'):
            with self.assertRaisesRegex(ValueError, 'exit is not confirmed'):
                agents.handoff(self.root, identity, {'status': 'completed', 'summary': 'Claim',
                    'next_action': 'Verify', 'evidence': []}, reason)
        self.assertEqual(agents.state(self.root)['writer'], identity)

    def test_failed_launch_retains_writer_and_unknown_cost(self):
        identity = self.prepare()
        with patch('workflow_review_api.credential', return_value='fixture-key'), patch.object(
                dsh, 'launch', side_effect=ValueError('Fixture failure')):
            with self.assertRaisesRegex(ValueError, 'blocked'):
                dsh.run(self.root, identity, self.root, 'fixture', 100000, 10, 32)
        record = core.load(dsh.jobs(self.root)[0])
        self.assertTrue(record['unknown_cost'])
        self.assertTrue(record['runtime_stopped'])
        self.assertEqual(agents.state(self.root)['writer'], identity)
        self.assertFalse(record['grants_delivery_qualification'])

    def test_unconfirmed_constructor_cleanup_does_not_release_writer(self):
        identity = self.prepare()
        with patch('workflow_review_api.credential', return_value='fixture-key'), patch.object(
                dsh, 'launch', side_effect=dsh.StartupFailure(False)):
            with self.assertRaisesRegex(ValueError, 'blocked'):
                dsh.run(self.root, identity, self.root, 'fixture', 100000, 10, 32)
        record = core.load(dsh.jobs(self.root)[0])
        self.assertFalse(record['runtime_stopped'])
        with self.assertRaisesRegex(ValueError, 'exit is not confirmed'):
            dsh.ensure_handoff(self.root, identity)

    def test_invalid_store_blocks_before_paid_dispatch(self):
        identity = self.prepare()
        with patch('workflow_review_api.credential', side_effect=AssertionError('No key read')):
            with self.assertRaisesRegex(ValueError, 'Existing absolute'):
                dsh.run(self.root, identity, self.root, 'fixture', 100000, 10, 32,
                        {'root': str(self.root / 'missing'), 'compression': 'zstd'})
        self.assertEqual(dsh.jobs(self.root), [])

    def reviewer(self):
        return agents.prepare(self.root, 'first-task', 'reviewer', 'independent-DSH', 'read', []).name

    def completed_client(self, mutation=None, reviewer=True):
        from unittest.mock import MagicMock
        client = MagicMock()
        client.process.pid = 123
        client.stop.return_value = True
        def call(method, params, observe):
            if method == 'session/prompt':
                if reviewer:
                    self.assertIn('Independent read-only review', params['contentBlocks'][0]['text'])
                return {'messageId': 'owned-receipt'}
            self.assertEqual(method, 'session/wait')
            for frame in (message(), event(2, 'turn/end', {'reason': {'kind': 'completed'}})):
                frame['params']['sessionId'] = params['sessionId']
                observe(frame)
            if mutation:
                mutation()
            return {}
        client.call.side_effect = call
        return client

    def test_developer_success_keeps_original_launch_and_writer(self):
        identity = self.prepare()
        client = self.completed_client(reviewer=False)
        with patch('workflow_review_api.credential', return_value='fixture-key'), \
                patch.object(dsh, 'launch', return_value=client) as launch, patch.object(dsh, 'initialize'):
            path = dsh.run(self.root, identity, self.root, 'deepseek-flash', 100000, 10, 32)
        self.assertEqual(core.load(path)['status'], 'model_completed')
        self.assertNotIn('reviewer', launch.call_args.kwargs)
        self.assertEqual(agents.state(self.root)['writer'], identity)

    def test_current_reviewer_dispatch_has_role_receipt_and_no_writer(self):
        identity = self.reviewer()
        client = self.completed_client()
        with patch('workflow_review_api.credential', return_value='fixture-key'), \
                patch.object(dsh, 'launch', return_value=client) as launch, patch.object(dsh, 'initialize'):
            path = dsh.run(self.root, identity, self.root, 'deepseek-flash', 100000, 10, 32)
        record = core.load(path)
        self.assertEqual(record['status'], 'model_completed')
        self.assertEqual(record['role'], 'reviewer')
        self.assertEqual(record['access'], 'read')
        self.assertEqual(record['messageId'], 'owned-receipt')
        self.assertEqual(record['prompt_status'], 'accepted')
        self.assertTrue(launch.call_args.kwargs['reviewer'])
        self.assertEqual(record['usage_known_tokens'], 33)
        self.assertFalse(record['grants_delivery_qualification'])
        self.assertIsNone(agents.state(self.root)['writer'])
        with self.assertRaisesRegex(ValueError, 'already dispatched'):
            dsh.run(self.root, identity, self.root, 'deepseek-flash', 100000, 10, 32)

    def test_nonreview_read_and_active_writer_rejected_before_key(self):
        identity = agents.prepare(self.root, 'first-task', 'verifier', 'verifier', 'read', []).name
        with patch('workflow_review_api.credential', side_effect=AssertionError('No key')):
            with self.assertRaisesRegex(ValueError, 'reviewer role'):
                dsh.run(self.root, identity, self.root, 'deepseek-flash', 100000, 10, 32)
            reviewer = self.reviewer()
            self.prepare()
            with self.assertRaisesRegex(ValueError, 'idle writer'):
                dsh.run(self.root, reviewer, self.root, 'deepseek-flash', 100000, 10, 32)

    def test_reviewer_missing_basis_and_stale_input_rejected_before_key(self):
        identity = self.reviewer()
        directory = self.root / '.ar-workflow/agents' / identity
        manifest = core.load(directory / 'package-manifest.json')
        path = directory / manifest['documents'][0]['frozen_path']
        original = path.read_bytes()
        path.unlink()
        with patch('workflow_review_api.credential', side_effect=AssertionError('No key')):
            with self.assertRaises(OSError):
                dsh.run(self.root, identity, self.root, 'deepseek-flash', 100000, 10, 32)
            path.write_bytes(original)
            self.write('main.py', 'changed')
            with self.assertRaisesRegex(ValueError, 'unchanged input'):
                dsh.run(self.root, identity, self.root, 'deepseek-flash', 100000, 10, 32)

    def test_reviewer_input_changes_during_initialize_never_sends(self):
        identity = self.reviewer()
        client = self.completed_client()
        with patch('workflow_review_api.credential', return_value='fixture-key'), \
                patch.object(dsh, 'launch', return_value=client), patch.object(
                    dsh, 'initialize', side_effect=lambda *a: self.write('main.py', 'changed')):
            with self.assertRaisesRegex(ValueError, 'blocked'):
                dsh.run(self.root, identity, self.root, 'deepseek-flash', 100000, 10, 32)
        client.call.assert_not_called()
        self.assertEqual(core.load(dsh.jobs(self.root)[0])['prompt_status'], 'not_sent')

    def test_reviewer_input_changes_during_turn_cannot_finish_valid_review(self):
        identity = self.reviewer()
        client = self.completed_client(lambda: self.write('main.py', 'changed'))
        with patch('workflow_review_api.credential', return_value='fixture-key'), \
                patch.object(dsh, 'launch', return_value=client), patch.object(dsh, 'initialize'):
            with self.assertRaisesRegex(ValueError, 'blocked'):
                dsh.run(self.root, identity, self.root, 'deepseek-flash', 100000, 10, 32)
        record = core.load(dsh.jobs(self.root)[0])
        self.assertEqual(record['status'], 'blocked')
        self.assertEqual(record['usage_known_tokens'], 33)
        self.assertTrue(record['runtime_stopped'])

    def test_no_token_limit_is_explicit_and_preserves_actual_usage(self):
        identity = self.reviewer()
        with patch('workflow_review_api.credential', return_value='fixture-key'), \
                patch.object(dsh, 'launch', return_value=self.completed_client()), patch.object(dsh, 'initialize'):
            path = dsh.run(self.root, identity, self.root, 'deepseek-flash', None, 10, 32,
                           no_token_limit=True)
        record = core.load(path)
        self.assertEqual(record['token_limit_mode'], 'disabled_explicit')
        self.assertIsNone(record['budget'])
        self.assertEqual(record['seconds'], 10)
        self.assertEqual(record['usage_known_tokens'], 33)

    def test_no_limit_does_not_silently_accept_invalid_limits_or_unbound_time(self):
        identity = self.reviewer()
        cases = [(None, False, 10), (10, True, 10), (0, False, 10),
                 (-1, False, 10), (True, False, 10), (None, True, 0), (None, True, 1801)]
        with patch('workflow_review_api.credential', side_effect=AssertionError('No key')):
            for budget, unlimited, seconds in cases:
                with self.subTest(budget=budget, unlimited=unlimited, seconds=seconds):
                    with self.assertRaisesRegex(ValueError, 'explicit positive budget'):
                        dsh.run(self.root, identity, self.root, 'deepseek-flash', budget, seconds, 32,
                                no_token_limit=unlimited)

    def test_new_package_cannot_bypass_previous_unknown_cost(self):
        identity = self.reviewer()
        core.save(self.root / dsh.BASE / 'jobs' / ('a' * 24) / 'run.json',
                  {'assignment': 'old', 'runtime_stopped': True, 'unknown_cost': True})
        with patch('workflow_review_api.credential', side_effect=AssertionError('No key')):
            with self.assertRaisesRegex(ValueError, 'runtime/cost unresolved'):
                dsh.run(self.root, identity, self.root, 'deepseek-flash', None, 10, 32,
                        no_token_limit=True)

    def resume_fixture(self):
        sessions = self.root / '.ar-workflow/test-sessions'
        sessions.mkdir(parents=True)
        identity = self.reviewer()
        with patch('workflow_review_api.credential', return_value='fixture-key'), \
                patch.object(dsh, 'launch', return_value=self.completed_client()), patch.object(dsh, 'initialize'):
            path = dsh.run(self.root, identity, self.root, 'deepseek-flash', None, 10, 32,
                           {'root': str(sessions), 'compression': 'zstd'}, no_token_limit=True)
        agents.handoff(self.root, identity, {'status': 'completed', 'summary': 'fixture only',
            'next_action': 'readiness investigation', 'evidence': [path.relative_to(self.root).as_posix()]})
        return path.parent.name, self.reviewer()

    def test_resume_check_is_readonly_and_never_claims_implemented_resume(self):
        job, identity = self.resume_fixture()
        record = self.root / dsh.BASE / 'jobs' / job / 'run.json'
        before = record.read_bytes()
        with patch('workflow_review_api.credential', side_effect=AssertionError('No key')), \
                patch.object(dsh, 'launch', side_effect=AssertionError('No SDK launch')):
            result = dsh.resume_check(self.root, job, identity)
        self.assertEqual(result['status'], 'preconditions_checked_not_resumed')
        self.assertFalse(result['model_request_sent'])
        self.assertFalse(result['resume_implemented'])
        self.assertEqual(record.read_bytes(), before)

    def test_resume_check_blocks_unknown_cost_role_change_and_stale_package(self):
        job, identity = self.resume_fixture()
        record = self.root / dsh.BASE / 'jobs' / job / 'run.json'
        data = core.load(record)
        data['unknown_cost'] = True
        core.save(record, data)
        result = dsh.resume_check(self.root, job, identity)
        self.assertTrue(any('cost unresolved' in x for x in result['blockers']))
        # Restore exact pinned record, then independently test role and input changes.
        data['unknown_cost'] = False
        core.save(record, data)
        wrong = agents.prepare(self.root, 'first-task', 'verifier', 'wrong-role', 'read', []).name
        result = dsh.resume_check(self.root, job, wrong)
        self.assertTrue(any('change/role/agent' in x for x in result['blockers']))
        self.write('main.py', 'new source')
        result = dsh.resume_check(self.root, job, identity)
        self.assertTrue(any('unchanged input' in x for x in result['blockers']))


if __name__ == '__main__':
    unittest.main()
