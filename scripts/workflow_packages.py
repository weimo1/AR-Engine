"""Compact entry points with complete, content-checked local frozen documents."""
from pathlib import Path
import json

import workflow as core
import workflow_context as context

FORMAT = 'ar.agent-package/v2'


def front(assignment: dict) -> str:
    basis = assignment['context']
    required = [x['path'] for x in basis['documents']
                if x['path'] in ('AGENTS.md', 'docs/README.md', 'docs/workflow/当前决策与待办清单.md')
                or x['purpose'] in ('execution', 'task-contract', 'skill', 'standard')]
    lines = [f'# Agent task {assignment["id"]}',
             f'Role: {assignment["role"]}; Agent: {assignment["agent"]}; access: {assignment["access"]}',
             f'Change: {assignment["change"]}', f'Assigned goal: {assignment["task_goal"]}',
             f'Non-goals: {basis["non_goals"]}', f'Allowed writes: {assignment["paths"]}',
             f'Input SHA256: {core.digest(core.canonical(assignment["input"]))}',
             '完整冻结清单、用途/确认范围及大小：package-manifest.json；Workflow relay: relay.json（分配时快照）。',
             '先读下面必读冻结文件；参考材料按本目标从清单选择。未读正文不能声称已加载/批准。',
             '依据路径均相对本任务包目录，不要改读可能已变化的工作区同名文件。',
             '\n必读顺序：']
    lines += [f'- basis/{name}' for name in required]
    lines += ['\n只读角色不改源码。writer唯一；完成后给出实际结果、证据及下一步，按agent-handoff交接。',
              '本包不授权API/Git/消息，不授予独立审查或交付资格。无法读取必需冻结文件时停止受影响执行，明确缺口，不假装完成。',
              '用量由统筹串行汇总，不并发改统计。']
    return '\n\n'.join(lines) + '\n'


def freeze(root: Path, directory: Path, assignment: dict) -> str:
    records = []
    for item in assignment['context']['documents']:
        name = context.relative(item['path'])
        data = core.within(root, root / name).read_bytes()
        if core.digest(data) != item['sha256']:
            raise ValueError('Context changed before package freeze')
        target = core.within(directory, directory / 'basis' / name)
        target.parent.mkdir(parents=True, exist_ok=True)
        with target.open('xb') as stream:
            stream.write(data)
        records.append(dict(item, frozen_path='basis/' + name, bytes=len(data)))
    assignment['package_format'] = FORMAT
    body = front(assignment)
    stats = {'front_bytes': len(body.encode('utf-8')), 'documents': len(records),
             'frozen_bytes': sum(x['bytes'] for x in records),
             'largest_documents': [{'path': x['path'], 'bytes': x['bytes']}
                                   for x in sorted(records, key=lambda x: x['bytes'], reverse=True)[:8]]}
    manifest = {'format': FORMAT, 'documents': records, 'statistics': stats}
    core.save(directory / 'package-manifest.json', manifest)
    core.save(directory / 'relay.json', assignment.get('relay', {}))
    assignment['package_manifest_sha256'] = core.digest(core.canonical(manifest))
    assignment['relay_sha256'] = core.digest((directory / 'relay.json').read_bytes())
    return body


def validate(directory: Path, assignment: dict) -> None:
    if 'package_format' not in assignment:
        return  # Original inline packages remain untouched.
    if assignment['package_format'] != FORMAT:
        raise ValueError('Unknown Agent package format')
    manifest = core.load(core.within(directory, directory / 'package-manifest.json'))
    if (core.digest(core.canonical(manifest)) != assignment.get('package_manifest_sha256')
            or manifest.get('format') != FORMAT
            or core.digest(core.within(directory, directory / 'relay.json').read_bytes()) != assignment.get('relay_sha256')):
        raise ValueError('Frozen package manifest/relay changed')
    expected = assignment['context']['documents']
    records = manifest.get('documents', [])
    if not isinstance(records, list) or len(records) != len(expected):
        raise ValueError('Frozen package documents missing')
    for doc, item in zip(expected, records):
        name = context.relative(doc['path'])
        if ({k: v for k, v in item.items() if k not in ('frozen_path', 'bytes')} != doc
                or item.get('frozen_path') != 'basis/' + name):
            raise ValueError('Frozen package document selection changed')
        data = core.within(directory, directory / item['frozen_path']).read_bytes()
        if len(data) != item.get('bytes') or core.digest(data) != doc['sha256']:
            raise ValueError('Frozen package document changed')


def prompt(directory: Path, assignment: dict) -> str:
    validate(directory, assignment)
    return (f'AR Engine | {assignment["role"]} | {assignment["change"]}\n'
            f'Task package directory: {directory.resolve()}\n\n'
            + (directory / 'task.md').read_text(encoding='utf-8'))


def statistics(directory: Path, assignment: dict) -> dict:
    validate(directory, assignment)
    files = ['task.md', 'assignment.json']
    if assignment.get('package_format'):
        manifest = core.load(directory / 'package-manifest.json')
        files += ['package-manifest.json', 'relay.json'] + [x['frozen_path'] for x in manifest['documents']]
        value = dict(manifest['statistics'])
    else:
        value = {'front_bytes': (directory / 'task.md').stat().st_size, 'documents': len(assignment['context']['documents']),
                 'frozen_bytes': 0, 'legacy_inline': True}
    value['package_total_bytes'] = sum(core.within(directory, directory / name).stat().st_size for name in files)
    return value
