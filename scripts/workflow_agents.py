"""Single-workspace cooperation records, not OS isolation or review approval."""
from __future__ import annotations

from datetime import datetime, timezone
from pathlib import Path
import uuid

import workflow as core
import workflow_context as context


def now() -> str:
    return datetime.now(timezone.utc).isoformat()


def state_path(root: Path) -> Path:
    return core.within(root, root / '.ar-workflow/agents/state.json')


def state(root: Path) -> dict:
    path = state_path(root)
    value = core.load(path) if path.exists() else {'schema_version': 1, 'writer': None, 'assignments': {}}
    if (value.get('schema_version') != 1 or not isinstance(value.get('assignments'), dict)
            or (value.get('writer') is not None and value['writer'] not in value['assignments'])):
        raise ValueError('Agent coordination state is invalid; inspect, do not reset automatically')
    return value


def assert_idle(root: Path) -> None:
    writer = state(root)['writer']
    if writer:
        raise ValueError(f'Agent writer {writer} has not handed off; no independent verification/review/Git admission')


def paths_changed(before: dict, after: dict) -> list[str]:
    return sorted(name for name in set(before['files']) | set(after['files'])
                  if before['files'].get(name) != after['files'].get(name))


def prepare(root: Path, change: str, role: str, agent: str, access: str, paths: list[str], goal: str | None = None) -> Path:
    rules = context.policy(root)
    if role not in rules['roles'] or access not in ('read', 'write') or not agent.strip():
        raise ValueError('Unknown Agent role/access or missing real Agent identifier')
    definition = rules['roles'][role]
    coordination = state(root)
    baseline = core.input_state(root, 'workspace')
    changed = context.changed_names(root)
    basis = context.build(root, change, changed)
    if access == 'write':
        assert_idle(root)
        if not definition['write'] or not paths:
            raise ValueError('Role is read-only or write paths are missing')
        for name in paths:
            context.write_path(name)
            core.within(root, root / name)
            if name not in basis['write_paths'] and (
                    any(char in name for char in '*?[') or not context.permits(name, basis['write_paths'])):
                raise ValueError('Assignment path exceeds the task write scope')
    elif paths:
        raise ValueError('Read-only assignment cannot claim write paths')
    identity = uuid.uuid4().hex[:24]
    directory = core.within(root, root / '.ar-workflow/agents' / identity)
    directory.mkdir(parents=True, exist_ok=False)
    assignment = {'schema_version': 1, 'id': identity, 'change': change, 'agent': agent,
        'role': role, 'access': access, 'paths': paths, 'created_at': now(),
        'task_goal': goal.strip() if goal and goal.strip() else basis['objective'],
        'input': baseline, 'context': basis, 'responsibility': definition,
        'limitations': ['Cooperation protocol, not OS isolation', 'No implicit API or Git authorization',
                        'Handoff never supplies verification/review approval']}
    body = [f'# Agent task {identity}', f'Role: {role}; Agent: {agent}; access: {access}',
            f'Change goal: {basis["objective"]}', f'Assigned goal: {assignment["task_goal"]}', f'Responsibility: {definition["goal"]}',
            f'Deliver: {definition["deliver"]}', f'Allowed writes: {paths}',
            f'Non-goals: {basis["non_goals"]}', f'Input SHA256: {core.digest(core.canonical(baseline))}',
            'Read assigned source at this input. Do not inherit developer chat reasoning or claims of approval.',
            'Do not start paid APIs or mutate Git from this assignment; coordinator resumes after handoff.',
            'Context status is a claim, not authenticated user approval. Current human authorization controls.']
    for item in basis['documents']:
        body.extend([f'\n## {item["path"]} [{item["status"]}]', f'Basis: {item["basis"]}',
                     f'SHA256: {item["sha256"]}', context.text(root, item['path'])])
    if baseline != core.input_state(root, 'workspace'):
        raise ValueError('Input changed during Agent context export; unpublished package retained')
    core.save(directory / 'assignment.json', assignment)
    task = '\n\n'.join(body) + '\n'
    (directory / 'task.md').write_text(task, encoding='utf-8', newline='\n')
    coordination['assignments'][identity] = {'assignment_sha256': core.digest(core.canonical(assignment)),
        'task_sha256': core.digest(task.encode('utf-8')), 'status': 'active'}
    if access == 'write':
        coordination['writer'] = identity
    core.save(state_path(root), coordination)
    return directory


def inspect(root: Path, identity: str) -> tuple[dict, dict, Path]:
    import re
    if not re.fullmatch(r'[0-9a-f]{24}', identity):
        raise ValueError('Invalid Agent assignment identifier')
    coordination = state(root)
    entry = coordination['assignments'].get(identity)
    if not entry or entry.get('status') != 'active':
        raise ValueError('Assignment is missing or already handed off/released')
    directory = core.within(root, root / '.ar-workflow/agents' / identity)
    assignment = core.load(directory / 'assignment.json')
    if (core.digest(core.canonical(assignment)) != entry['assignment_sha256']
            or core.digest((directory / 'task.md').read_bytes()) != entry['task_sha256']):
        raise ValueError('Agent task package changed')
    if assignment['access'] == 'write' and coordination['writer'] != identity:
        raise ValueError('Assignment does not own the current writer record')
    return coordination, assignment, directory


def handoff(root: Path, identity: str, result: dict, release_reason: str | None = None) -> Path:
    coordination, assignment, directory = inspect(root, identity)
    import workflow_dsh
    workflow_dsh.ensure_handoff(root, identity)
    if (not isinstance(result, dict) or result.get('status') not in ('completed', 'blocked')
            or any(not isinstance(result.get(k), str) or not result[k].strip()
                   for k in ('summary', 'next_action'))
            or not isinstance(result.get('evidence'), list)):
        raise ValueError('Handoff needs status, summary, next_action and evidence list; not a pass declaration')
    for item in result['evidence']:
        if not isinstance(item, str) or not core.within(root, root / context.relative(item)).is_file():
            raise ValueError('Handoff evidence must name an existing project-owned file')
    input_error = None
    try:
        current = core.input_state(root, 'workspace')
    except (OSError, ValueError) as error:
        if release_reason is None:
            raise
        current = None
        input_error = f'{type(error).__name__}: {error}'
    changed = paths_changed(assignment['input'], current) if current is not None else []
    status = result['status']
    violations = [name for name in changed if not context.permits(name, assignment['paths'])]
    if assignment['access'] == 'read' and current != assignment['input']:
        status = 'stale'
    elif current is not None and assignment['access'] == 'write' and (violations
            or current.get('head') != assignment['input'].get('head')
            or current.get('index') != assignment['input'].get('index')):
        status = 'blocked'
    if release_reason is not None:
        if not release_reason.strip():
            raise ValueError('Abnormal release needs an explicit reason')
        status = 'released'
    record = {'assignment': identity, 'agent': assignment['agent'], 'at': now(), 'status': status,
              'result_claim': result, 'changed_paths': changed, 'out_of_scope': violations,
              'start_input': assignment['input'], 'end_input': current,
              'end_input_unavailable': input_error,
              'release_reason': release_reason, 'grants_delivery_qualification': False}
    path = directory / 'handoff.json'
    core.save(path, record)
    coordination['assignments'][identity]['status'] = status
    if coordination['writer'] == identity:
        coordination['writer'] = None
    core.save(state_path(root), coordination)
    return path
