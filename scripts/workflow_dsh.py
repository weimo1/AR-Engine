"""Owned DSH task dispatch. A model result never grants delivery qualification."""
from __future__ import annotations

import json
import os
from pathlib import Path
import re
import uuid

import workflow as core
import workflow_agents as agents
from workflow_dsh_rpc import Client, StartupFailure

BASE = '.ar-workflow/dsh-bridge'

# Installed DSH 0.2.1-rc.2 known-event-types catalog; unknown events fail closed.
KNOWN_EVENTS = frozenset((
    'agent-preset/selected',
    'agent/inbox/spliced',
    'approval/asked',
    'approval/decided',
    'approval/policy',
    'assistant/attempt',
    'assistant/message',
    'command/done',
    'command/run',
    'compaction/end',
    'compaction/prune',
    'compaction/start',
    'compaction/summary',
    'deliverables/presented',
    'developer/message',
    'feedback/message-delete',
    'feedback/message-put',
    'feedback/record',
    'goal/change',
    'hook/invoked',
    'hook/result',
    'image/offload',
    'job/registered',
    'job/settled',
    'llm/retry',
    'llm/retry-started',
    'model/selection',
    'permission/preset',
    'plan/mode',
    'request/context',
    'request/header',
    'sandbox/mode',
    'schedule/change',
    'session-log-deepseek/delivery-accepted',
    'session/end-seed',
    'session/title',
    'session/title-llm-request',
    'step/end',
    'step/start',
    'subagent/catalog',
    'subagent/descriptor',
    'subagent/model-selection-policy',
    'system/message',
    'team/member',
    'team/message/delivered',
    'team/message/queued',
    'team/task',
    'todo/write',
    'tool-workflow/agent-end',
    'tool-workflow/agent-start',
    'tool-workflow/run-end',
    'tool-workflow/run-start',
    'tool/call',
    'tool/ptc-dispatch',
    'tool/ptc-dispatch-start',
    'tool/result',
    'turn/end',
    'turn/start',
    'user/message',
    'web/deepseek-search-llm-request',
    'working-directory/change',
    'workspace/changes',
))
UNACCOUNTED_COST_EVENTS = frozenset(('image/offload', 'llm/retry', 'llm/retry-started',
    'session/title-llm-request', 'web/deepseek-search-llm-request'))



def desktop_store(home: Path | None = None, compression: str = 'zstd') -> dict:
    """Resolve the desktop's documented location without opening credentials or profiles."""
    if compression not in ('zstd', 'none'):
        raise ValueError('Unsupported desktop session compression')
    if home is None:
        default = Path.home() / '.dsh'
        location = default / 'desktop-next-location.json'
        if location.is_file():
            value = core.load(location)
            declared = value.get('home')
            if not isinstance(declared, str) or not Path(declared).is_absolute():
                raise ValueError('Invalid DSH desktop location; specify --desktop-home explicitly')
            home = Path(declared)
        else:
            home = default
    return session_store({'root': str(home.resolve() / 'sessions'), 'compression': compression})


def session_store(value: dict) -> dict:
    if not isinstance(value, dict) or value.get('compression') not in ('zstd', 'none'):
        raise ValueError('Invalid desktop session store configuration')
    raw = value.get('root')
    if not isinstance(raw, str) or not Path(raw).is_absolute() or not Path(raw).is_dir():
        raise ValueError('Existing absolute desktop sessions directory required; no private fallback')
    return {'root': str(Path(raw).resolve()), 'compression': value['compression']}


def launch(root: Path, install: Path, directory: Path, seconds: int, key: str = '',
           store: dict | None = None) -> Client:
    store = session_store(store) if store is not None else None
    install = install.resolve()
    exe, archive = install / 'DSH NEXT.exe', install / 'resources/app.asar'
    if not exe.is_file() or not archive.is_file():
        raise ValueError('DSH NEXT installation is missing exe/resources/app.asar')
    # Private profiles/home; only an explicit persistence overlay shares new sessions.
    env = {k: v for k, v in os.environ.items() if not k.upper().startswith('DSH_')
           and not re.search(r'KEY|PASSWORD|SECRET|TOKEN', k, re.I)}
    env.update(ELECTRON_RUN_AS_NODE='1', DSH_HOME=str(directory / 'home'),
               DSH_MAX_TOKENS_AS_SUCCESS='false', DSH_SYSTEM_PROMPT=(
                   'You are an AR Engine execution agent. Follow the supplied task package. '
                   'Do not delegate, launch paid models, change Git, or leave task processes running. '
                   'Finish with a concise factual report. Do not claim review or acceptance.'))
    if key:
        env['DEEPSEEK_API_KEY'] = key
    patch = directory / 'no-retry.patch.yml'
    overlay = ('- id: llm-retry\n  disabled: true\n- id: compaction-basic\n  disabled: true\n'
               '- id: image-offload\n  disabled: true\n')
    if store:
        overlay += ('- id: sessions\n  config:\n    root: ' + json.dumps(store['root'], ensure_ascii=False)
                    + '\n    compression: ' + store['compression'] + '\n')
    patch.write_text(overlay, encoding='utf-8')
    core.save(directory / 'runtime.json', {'install': str(install),
        'archive_sha256': core.digest(archive.read_bytes()), 'profile': 'sdk-minimal',
        'cwd': str(root.resolve()), 'max_tokens_as_success': False,
        'patch_sha256': core.digest(patch.read_bytes()), 'automatic_retry_disabled': True,
        'session_storage': store, 'visibility': 'desktop' if store else 'isolated'})
    return Client([str(exe), str(archive / 'lib/desktop-cli.js'), '--profile', 'sdk-minimal',
                   '--patch', str(patch.resolve())],
                  root, env, directory, seconds, key)


def initialize(client: Client, root: Path, model: str, max_output: int):
    if model != 'deepseek-flash':
        raise ValueError('Only the user-selected DeepSeek-V4.1-Flash / deepseek-flash High is permitted')
    result = client.call('initialize', {'cwd': str(root.resolve()), 'provider': 'deepseek-official',
                                      'model': model, 'reasoningEffort': 'high', 'maxTokens': max_output})
    if not isinstance(result, dict) or result.get('serverInfo', {}).get('name') != 'deepseek-harness-sdk-runtime':
        raise ValueError('Unexpected DSH SDK identity')
    return result


def doctor(root: Path, install: Path, store: dict | None = None) -> Path:
    directory = core.within(root, root / BASE / 'doctors' / uuid.uuid4().hex[:24])
    directory.mkdir(parents=True)
    client = None
    result = {'status': 'blocked', 'model_request_sent': False,
              'model': 'deepseek-flash', 'reasoning_effort': 'high'}
    startup_stopped = True
    try:
        client = launch(root, install, directory, 30, store=store)
        result['server'] = initialize(client, root, 'deepseek-flash', 256)
        session = 'ar-doctor-' + uuid.uuid4().hex
        cwd = client.call('session/working-directory/get', {'sessionId': session})
        if not isinstance(cwd, dict) or Path(cwd.get('cwd', '')).resolve() != root.resolve():
            raise ValueError('DSH working directory mismatch')
        result.update(status='connected', cwd=cwd['cwd'])
    except (ValueError, OSError) as error:
        if isinstance(error, StartupFailure):
            startup_stopped = error.runtime_stopped
        result['error'] = str(error)
    finally:
        result['runtime_stopped'] = client.stop() if client else startup_stopped
        if not result['runtime_stopped']:
            result['status'] = 'blocked'
        core.save(directory / 'result.json', result)
    if result['status'] != 'connected':
        raise ValueError(f'DSH doctor failed; inspect {directory / "result.json"}')
    return directory / 'result.json'


def usage_of(event: dict) -> dict | None:
    data = event.get('data', {})
    usage = data.get('usage')
    if usage is None:
        for record in data.get('stream', []):
            chunk = record.get('chunk') if isinstance(record, dict) and record.get('type') == 'chunk' else None
            if isinstance(chunk, dict) and chunk.get('type') == 'usage':
                usage = chunk.get('usage')
    if usage is None:
        return None
    if not isinstance(usage, dict):
        raise ValueError('Invalid DSH usage')
    result = {}
    for name in ('inputTokens', 'outputTokens', 'cacheReadTokens', 'cacheWriteTokens'):
        value = usage.get(name, 0 if name.startswith('cache') else None)
        if type(value) is not int or value < 0:
            raise ValueError('Invalid DSH usage count')
        result[name] = value
    total = sum(result.values())
    if total <= 0 or ('totalTokens' in usage and usage['totalTokens'] != total):
        raise ValueError('DSH usage total mismatch')
    result['totalTokens'] = total
    return result


class Evidence:
    def __init__(self, session: str, budget: int):
        self.session, self.budget = session, budget
        self.seen = {}
        self.usage = []
        self.unknown = []
        self.reply = []
        self.terminal = []
        self.final_text = ''
        self.message_turn = None
        self.end_turn = None

    def observe(self, frame: dict):
        method, params = frame.get('method'), frame.get('params', {})
        if method in ('subagent.started', 'subagent.finished'):
            self.unknown.append('Unexpected delegation; descendant usage cannot be established')
            raise ValueError(self.unknown[-1])
        if method == 'session.status':
            if params.get('sessionId') != self.session:
                raise ValueError('Unexpected foreign DSH session status')
            return
        if method != 'session.event':
            raise ValueError('Unknown DSH notification; usage cannot be established')
        if params.get('sessionId') != self.session:
            raise ValueError('Unexpected foreign DSH session event')
        event = params.get('event', {})
        seq = event.get('seq')
        if type(seq) is not int or seq < 0:
            raise ValueError('Invalid DSH event sequence')
        digest = core.digest(core.canonical(event))
        if seq in self.seen:
            if self.seen[seq] != digest:
                raise ValueError('Conflicting duplicate DSH event')
            return
        self.seen[seq] = digest
        kind, data = event.get('type'), event.get('data', {})
        if kind not in KNOWN_EVENTS or kind in UNACCOUNTED_COST_EVENTS:
            raise ValueError('Unknown or unaccounted DSH event; usage cannot be established')
        if kind in ('assistant/message', 'assistant/attempt', 'compaction/summary'):
            usage = usage_of(event)
            if usage is None:
                self.unknown.append(f'Missing usage for {kind} at seq {seq}')
            else:
                self.usage.append({'sessionId': self.session, 'seq': seq, 'kind': kind, **usage})
            if kind == 'assistant/message':
                content = data.get('message', {}).get('content', [])
                text = '\n'.join(c['text'] for c in content
                                 if c.get('type') == 'text' and isinstance(c.get('text'), str))
                self.final_text = text.strip()
                self.message_turn = data.get('turn')
                if text.strip():
                    self.reply.append(text)
            if self.total >= self.budget:
                raise ValueError('DSH monitored token threshold reached; in-flight cost may exceed it')
        if kind == 'turn/end':
            self.terminal.append(data.get('reason', {}).get('kind'))
            self.end_turn = data.get('turn')

    @property
    def total(self):
        return sum(item['totalTokens'] for item in self.usage)

    def completed(self):
        return bool(self.terminal) and all(x == 'completed' for x in self.terminal) and bool(
            self.final_text and self.usage) and not self.unknown and (
            type(self.end_turn) is int and self.end_turn == self.message_turn)


def jobs(root: Path):
    directory = core.within(root, root / BASE / 'jobs')
    return sorted(directory.glob('*/run.json'))


def ensure_handoff(root: Path, assignment: str):
    for path in jobs(root):
        run = core.load(core.within(root, path))
        if run.get('assignment') == assignment and run.get('runtime_stopped') is not True:
            raise ValueError(f'DSH runtime exit is not confirmed; inspect {path}; do not release writer')


def recover(root: Path, identity: str) -> Path:
    if not re.fullmatch(r'[0-9a-f]{24}', identity):
        raise ValueError('Invalid DSH job ID')
    directory = core.within(root, root / BASE / 'jobs' / identity)
    record = core.load(directory / 'run.json')
    if record.get('runtime_stopped') is True:
        return directory / 'run.json'
    if os.name != 'nt':
        raise ValueError('Owned Job recovery currently requires Windows')
    import workflow_dsh_job
    if not workflow_dsh_job.recover(directory):
        raise ValueError('Owned process tree exit unconfirmed; writer retained')
    record.update(runtime_stopped=True, status='blocked', unknown_cost=True,
                  recovered_at=agents.now(), error='Explicit owned Job stop/recovery; no automatic resend')
    core.save(directory / 'run.json', record)
    usage_report(root)
    return directory / 'run.json'


def run(root: Path, assignment: str, install: Path, model: str,
        budget: int, seconds: int, max_output: int, store: dict | None = None) -> Path:
    from workflow_review_api import require_model_selection
    require_model_selection(root, {'model': model, 'thinking': {'type': 'enabled'}, 'reasoning_effort': 'high'})
    store = session_store(store) if store is not None else None
    if (type(budget) is not int or budget <= 0 or not 1 <= seconds <= 1800
            or not 1 <= max_output <= 32768 or not model.strip()):
        raise ValueError('DSH requires positive budget, seconds<=1800 and max-output<=32768')
    _, package, source = agents.inspect(root, assignment)
    if package['access'] != 'write' or package['input'] != core.input_state(root, 'workspace'):
        raise ValueError('DSH execution requires the current writer package at unchanged input')
    for path in jobs(root):
        if core.load(core.within(root, path)).get('assignment') == assignment:
            raise ValueError('Assignment already dispatched; inspect evidence, never resend automatically')
    task = (source / 'task.md').read_text(encoding='utf-8')
    prompt = f'AR Engine | {package["role"]} | {package["change"]}\n\n' + task
    estimate = len(prompt.encode('utf-8')) + max_output
    if estimate > budget:
        raise ValueError(f'Initial conservative task estimate {estimate} exceeds budget {budget}')
    directory = core.within(root, root / BASE / 'jobs' / uuid.uuid4().hex[:24])
    directory.mkdir(parents=True)
    session = 'ar-task-' + uuid.uuid4().hex
    record = {'schema_version': 1, 'assignment': assignment, 'sessionId': session,
        'status': 'starting', 'model': model, 'reasoning_effort': 'high', 'budget': budget, 'seconds': seconds,
        'max_output': max_output, 'initial_estimate': estimate, 'runtime_stopped': False,
        'session_storage': store, 'visibility': 'desktop' if store else 'isolated',
        'task_sha256': core.digest((source / 'task.md').read_bytes()), 'created_at': agents.now(),
        'grants_delivery_qualification': False, 'usage_known_tokens': 0, 'unknown_cost': True}
    core.save(directory / 'run.json', record)
    client, evidence = None, Evidence(session, budget)
    startup_stopped = True
    try:
        from workflow_review_api import credential
        key = credential()
        client = launch(root, install, directory, seconds, key, store=store)
        record.update(pid=client.process.pid, status='running')
        core.save(directory / 'run.json', record)
        initialize(client, root, model, max_output)
        # Check again immediately before admission; initialize must never submit a prompt.
        agents.inspect(root, assignment)
        if core.input_state(root, 'workspace') != package['input']:
            raise ValueError('Task input changed before DSH prompt admission')
        reply = client.call('session/prompt', {'sessionId': session,
            'contentBlocks': [{'type': 'text', 'text': prompt}]}, evidence.observe)
        if not isinstance(reply, dict) or not isinstance(reply.get('messageId'), str) or not reply['messageId']:
            raise ValueError('DSH prompt receipt is missing')
        record['messageId'] = reply['messageId']
        core.save(directory / 'run.json', record)
        client.call('session/wait', {'sessionId': session}, evidence.observe)
        if not evidence.completed():
            raise ValueError('DSH did not supply completed turn, final text and known usage')
        record.update(status='model_completed', unknown_cost=False)
    except (ValueError, OSError) as error:
        if isinstance(error, StartupFailure):
            startup_stopped = error.runtime_stopped
        record.update(status='blocked', error=str(error)[:2000])
    finally:
        record['runtime_stopped'] = client.stop() if client else startup_stopped
        if not record['runtime_stopped']:
            record.update(status='blocked', error='Owned runtime exit unconfirmed')
        record.update(finished_at=agents.now(), usage_known_tokens=evidence.total,
                      unknown_usage_events=evidence.unknown, terminal_reasons=evidence.terminal)
        core.save(directory / 'usage.json', {'events': evidence.usage, 'known_tokens': evidence.total,
                  'unknown_cost': record['unknown_cost'], 'model': model, 'sessionId': session})
        (directory / 'reply.txt').write_text('\n\n'.join(evidence.reply), encoding='utf-8')
        core.save(directory / 'run.json', record)
        usage_report(root)
    if record['status'] != 'model_completed':
        raise ValueError(f'DSH task blocked; inspect {directory / "run.json"}; writer retained')
    return directory / 'run.json'


def usage_report(root: Path) -> Path:
    rows, total, unknown = [], 0, 0
    seen = {}
    paths = jobs(root)
    for fixture in (root / BASE).glob('smoke-*'):
        paths.extend(jobs(core.within(root, fixture)))
    for probe in (root / BASE).glob('client-visibility-*'):
        path = core.within(root, probe / 'run.json')
        if path.is_file():
            paths.append(path)
    for path in paths:
        run = core.load(core.within(root, path))
        usage_path = core.within(root, path.parent / 'usage.json')
        if not usage_path.is_file():
            unknown += 1
            continue
        usage = core.load(usage_path)
        subtotal = 0
        for event in usage['events']:
            identity = (event['sessionId'], event['seq'])
            if identity in seen:
                if seen[identity] != event:
                    raise ValueError('Conflicting DSH usage evidence')
                continue
            seen[identity] = event
            subtotal += event['totalTokens']
        total += subtotal
        cost_unknown = bool(run.get('unknown_cost', True) or usage.get('unknown_cost', True))
        unknown += cost_unknown
        rows.append(f'{path.parent.name} | {run["model"]} | {run["status"]} | '
                    f'{subtotal} tokens | unknown_cost={cost_unknown}')
    destination = core.within(root, root / BASE / 'DSH-token用量统计.txt')
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text('\n'.join(['DSH 专属执行层用量（离线快照）', f'更新：{agents.now()}',
        f'已知 tokens：{total}', f'费用可能未知的任务：{unknown}',
        '按 sessionId/event.seq 去重；输入含未缓存、缓存读写；推理不重复加。',
        '与现有 HTTP/OCR 台账单列，未核对响应 ID 前不相加；不是供应商账单或费用硬限。',
        *rows, '']), encoding='utf-8-sig')
    core.save(destination.parent / 'usage-summary.json', {'known_tokens': total,
        'unknown_cost_jobs': unknown, 'event_count': len(seen), 'updated_at': agents.now()})
    return destination
