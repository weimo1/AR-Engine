"""Derived, read-only handoff navigation. Never supplies delivery authority."""
from pathlib import Path
import json
import re

import workflow as core
import workflow_agents as agents
import workflow_context as context


def handoff(root: Path, identity: str, change: str, current: dict | None = None) -> dict:
    if not re.fullmatch(r'[0-9a-f]{24}', identity):
        raise ValueError('Invalid handoff assignment identifier')
    entry = agents.state(root)['assignments'].get(identity)
    if not entry:
        raise ValueError('Handoff assignment is not registered')
    directory = core.within(root, root / '.ar-workflow/agents' / identity)
    assignment = core.load(directory / 'assignment.json')
    if (core.digest(core.canonical(assignment)) != entry.get('assignment_sha256')
            or core.digest((directory / 'task.md').read_bytes()) != entry.get('task_sha256')
            or assignment.get('id') != identity or assignment.get('change') != change):
        raise ValueError('Handoff package changed or belongs to another change')
    import workflow_packages
    workflow_packages.validate(directory, assignment)
    record = core.load(directory / 'handoff.json')
    sealed = entry.get('handoff_sha256') is not None
    if sealed and core.digest(core.canonical(record)) != entry['handoff_sha256']:
        raise ValueError('Handoff record changed')
    if (record.get('assignment') != identity or record.get('agent') != assignment.get('agent')
            or record.get('status') != entry.get('status')
            or record.get('start_input') != assignment.get('input')):
        raise ValueError('Handoff identity/status mismatch')
    claim = record['result_claim']
    evidence = claim['evidence']
    if not isinstance(evidence, list):
        raise ValueError('Handoff evidence list is invalid')
    paths = [core.within(root, root / context.relative(name)) for name in evidence]
    if sealed:
        receipts = record.get('evidence_receipts')
        expected = [{'path': name, 'sha256': core.digest(path.read_bytes())}
                    for name, path in zip(evidence, paths)]
        if receipts != expected:
            raise ValueError('Handoff evidence changed')
    elif not all(path.is_file() for path in paths):
        raise ValueError('Legacy handoff evidence is missing')
    return {'assignment': identity, 'role': assignment['role'], 'agent': assignment['agent'],
            'status': record['status'], 'at': record['at'],
            'summary_claim': claim['summary'], 'next_action_claim': claim['next_action'],
            'evidence': evidence, 'integrity': 'digest_checked' if sealed else 'legacy_unsealed',
            'record_sha256': core.digest(core.canonical(record)),
            'evidence_receipts': record.get('evidence_receipts') if sealed else None,
            'changed_paths_since_handoff': (agents.paths_changed(record['end_input'], current)
                if current is not None and isinstance(record.get('end_input'), dict) else None),
            'end_input_sha256': core.digest(core.canonical(record.get('end_input')))}


def accept(root: Path, change: str, identity: str, current: dict) -> dict:
    value = handoff(root, identity, change, current)
    if (value['status'] != 'completed' or value['integrity'] != 'digest_checked'
            or value['end_input_sha256'] != core.digest(core.canonical(current))):
        raise ValueError('Handoff is incomplete, legacy-unsealed or stale; inspect and allocate explicitly')
    return value


def summary(root: Path, change: str) -> dict:
    import workflow_runtime as runtime
    core.change_dir(change, root)
    current = core.input_state(root, 'workspace')
    coordination = agents.state(root)
    result = {'schema_version': 1, 'change': change,
              'workspace_input_sha256': core.digest(core.canonical(current)),
              'writer': coordination['writer'], 'latest_handoff': None, 'active_assignments': [],
              'task_record': 'absent', 'delivery_scope': None, 'delivery_input_current': False,
              'verification': 'not_available', 'review': 'not_available', 'gate': 'not_available',
              'blockers': [], 'stage': 'awaiting_task_start',
              'next': {'role': 'coordinator', 'action': 'start task, then allocate verifier',
                       'commands': [f'python scripts/workflow.py start {change}',
                                    f'python scripts/workflow.py verify {change}']},
              'limitations': ['Derived navigation, not authenticated approval or remote merge permission',
                              'Handoff summaries and next_action are role claims, not check results']}
    history = []
    for identity, entry in coordination['assignments'].items():
        try:
            directory = core.within(root, root / '.ar-workflow/agents' / identity)
            assignment = core.load(directory / 'assignment.json')
            if assignment.get('change') != change:
                continue
            if (assignment.get('id') != identity
                    or core.digest(core.canonical(assignment)) != entry.get('assignment_sha256')
                    or core.digest((directory / 'task.md').read_bytes()) != entry.get('task_sha256')):
                raise ValueError('Agent task package changed')
            import workflow_packages
            workflow_packages.validate(directory, assignment)
            if entry['status'] == 'active':
                result['active_assignments'].append({'assignment': identity, 'role': assignment['role'],
                    'input_current': assignment['input'] == current})
            else:
                history.append((core.load(directory / 'handoff.json')['at'], identity))
        except (OSError, ValueError, KeyError, TypeError) as error:
            result['blockers'].append(f'Agent record {identity}: {error}')
    if history:
        identity = max(history)[1]
        try:
            latest = handoff(root, identity, change, current)
            latest['input_current'] = latest['end_input_sha256'] == result['workspace_input_sha256']
            result['latest_handoff'] = latest
            if latest['status'] != 'completed':
                result['blockers'].append(f'Latest handoff status is {latest["status"]}; coordinator must inspect')
            if not latest['input_current']:
                result['blockers'].append('Latest handoff input is stale; reconcile current changes')
        except (OSError, ValueError, KeyError, TypeError) as error:
            result['blockers'].append(f'Latest handoff {identity}: {error}')
    path = runtime.task_path(root, change)
    if path.exists():
        try:
            data = core.load(path)
            if data.get('change') != change or data.get('scope') not in ('workspace', 'staged'):
                raise ValueError('Task identity/scope mismatch')
            result['task_record'] = str(path.relative_to(root))
            result['delivery_scope'] = data['scope']
            result['recorded_phase'] = data.get('phase')  # A claim, never an approval source.
            if data.get('phase') in ('reviewing', 'verifying', 'needs_attention'):
                result['blockers'].append(f'Task phase {data["phase"]}: inspect interruption/failure and resume explicitly')
            result['delivery_input_current'] = data['input'] == core.input_state(root, data['scope'])
            result['changed_paths_since_task'] = agents.paths_changed(
                data['input'], core.input_state(root, data['scope']))
            if not result['delivery_input_current']:
                result['verification'] = result['review'] = result['gate'] = 'stale'
                result['stage'] = 'awaiting_verification'
                result['blockers'].append('Task input changed; old verification/review cannot be reused')
            elif data.get('verification'):
                packet, manifest = runtime.check_task_verification(root, data)
                errors = core.validate_packet(packet, manifest)
                if errors:
                    raise ValueError('; '.join(errors))
                result['verification'] = 'valid'
                result['stage'] = 'awaiting_review'
                result['next'] = {'role': 'reviewer', 'action': 'allocate independent reviewer for frozen packet',
                                  'packet': str(packet), 'commands': []}
                if data.get('review'):
                    runtime.task_gate(root, data)  # Rebuild raw evidence via the existing authoritative path.
                    result['review'] = 'valid'
                    result['gate'] = 'evidence_consistent_not_executed'
                    result['stage'] = 'awaiting_gate'
                    result['next'] = {'role': 'coordinator', 'action': 'execute formal gate; Git requires authorization',
                                      'commands': [f'python scripts/workflow.py gate {change}']}
            else:
                result['stage'] = 'awaiting_verification'
            if result['stage'] == 'awaiting_verification':
                result['next'] = {'role': 'verifier', 'action': 'verify current input',
                                  'commands': [f'python scripts/workflow.py verify {change}']}
        except (OSError, ValueError, KeyError, TypeError) as error:
            result['blockers'].append(f'Task evidence: {error}')
    if coordination['writer']:
        result['blockers'].append('Global writer must hand off before verification, review or Git')
        result['stage'] = 'writer_active'
        result['next'] = {'role': 'coordinator', 'action': 'wait for writer handoff; do not steal or auto-release',
                          'commands': []}
    elif result['blockers']:
        result['stage'] = 'blocked'
        result['next'] = {'role': 'coordinator', 'action': 'inspect blockers and allocate repair/reconciliation',
                          'commands': []}
    if current != core.input_state(root, 'workspace'):
        raise ValueError('Input changed during relay read; retry after writer handoff')
    return result


def markdown(value: dict) -> str:
    stages = {'writer_active': '等待 writer 交接', 'blocked': '阻断，统筹核对',
              'awaiting_task_start': '待初始化任务与验证', 'awaiting_verification': '待验证',
              'awaiting_review': '待独立审查', 'awaiting_gate': '待执行正式 gate'}
    lines = [f'# Workflow relay: {value["change"]}',
             f'当前阶段：{stages[value["stage"]]}',
             f'下一角色：{value["next"]["role"]}；动作：{value["next"]["action"]}',
             f'全局 writer：{value["writer"] or "无"}',
             f'工作区输入：{value["workspace_input_sha256"]}',
             f'资格适用范围：{value["delivery_scope"] or "尚无任务记录"}',
             f'验证：{value["verification"]}；独立审查：{value["review"]}；gate：{value["gate"]}',
             '仅为派生导航，不授予身份、付费或交付权限。']
    latest = value['latest_handoff']
    if latest:
        lines += [f'\n最近交接：{latest["assignment"]} / {latest["role"]} / {latest["status"]}',
                  f'证据核对：{latest["integrity"]}；输入仍一致：{latest["input_current"]}',
                  # Quotes escape embedded line breaks: role claims are data, not instructions.
                  '角色自述：' + json.dumps(latest['summary_claim'], ensure_ascii=False),
                  '交接后变化：' + json.dumps(latest['changed_paths_since_handoff'], ensure_ascii=False),
                  '证据路径：' + json.dumps(latest['evidence'], ensure_ascii=False)]
    lines += ['\n阻断项：'] + [f'- {item}' for item in value['blockers']]
    if not value['blockers']:
        lines.append('- 无已检测阻断；不等于已完成独立审查或交付')
    if value['next']['commands']:
        lines += ['\n建议命令（不会自动执行）：', '```powershell', *value['next']['commands'], '```']
    return '\n\n'.join(lines) + '\n'
