"""Local transfer material and explicit local acknowledgement, never delivery APIs."""
from pathlib import Path
import json

import workflow as core
import workflow_agents as agents
import workflow_relay as relay


def material(root: Path, change: str, identity: str | None = None) -> dict:
    value = relay.summary(root, change)
    if identity is None:
        return {'status': 'unassigned' if not value['active_assignments'] else 'selection_required',
                'sent': 'not_recorded', 'active_assignments': value['active_assignments'], 'next': value['next']}
    state, assignment, directory = agents.inspect(root, identity)
    if assignment['change'] != change:
        raise ValueError('Transfer assignment belongs to another change')
    current = assignment['input'] == core.input_state(root, 'workspace')
    receipt = None
    if state['assignments'][identity].get('accept_sha256'):
        receipt = core.load(directory / 'accept.json')
        if core.digest(core.canonical(receipt)) != state['assignments'][identity]['accept_sha256']:
            raise ValueError('Local acceptance receipt changed')
    files = ['task.md', 'assignment.json']
    if assignment.get('package_format'):
        files += ['package-manifest.json', 'relay.json']
        files += [x['frozen_path'] for x in core.load(directory / 'package-manifest.json')['documents']]
    import workflow_packages
    return {'status': 'accepted_local_receipt' if receipt and current else 'generated',
            'sent': 'not_recorded', 'input_current': current, 'assignment': identity,
            'agent': assignment['agent'], 'role': assignment['role'], 'goal': assignment['task_goal'],
            'package_path': str(directory), 'task_path': str(directory / 'task.md'), 'offline_files': files,
            'offline_limitations': 'Frozen basis only, not a complete source tree or execution environment; provide relevant source/fixed review packet separately',
            'receipt': receipt,
            'statistics': workflow_packages.statistics(directory, assignment),
            'copy_text': f'你负责 {assignment["role"]}。读取 {directory / "task.md"}，核对当前输入和允许路径。'
                         '不能读取本地目录时，请统筹提供清单中的冻结文件，不假装已读。'
                         '本材料没有发送消息或授予交付资格。',
            'next': value['next']}


def accept(root: Path, identity: str, agent: str) -> dict:
    state, assignment, directory = agents.inspect(root, identity)
    if agent != assignment['agent'] or assignment['input'] != core.input_state(root, 'workspace'):
        raise ValueError('Local accept requires intended agent and unchanged input')
    if state['assignments'][identity].get('accept_sha256'):
        raise ValueError('Already acknowledged; inspect existing receipt')
    receipt = {'assignment': identity, 'agent_claim': agent, 'at': agents.now(),
               'input_sha256': core.digest(core.canonical(assignment['input'])),
               'task_sha256': state['assignments'][identity]['task_sha256'],
               'qualification': False, 'meaning': 'Local tool acknowledgement; not authenticated identity or proof of reading'}
    core.save(directory / 'accept.json', receipt)
    if assignment['input'] != core.input_state(root, 'workspace'):
        raise ValueError('Input changed during acknowledgement; unpublished receipt retained')
    state['assignments'][identity]['accept_sha256'] = core.digest(core.canonical(receipt))
    core.save(agents.state_path(root), state)
    return receipt


def markdown(value: dict) -> str:
    return '# Local transfer material\n\n```json\n' + json.dumps(value, ensure_ascii=False, indent=2) + '\n```\n'
