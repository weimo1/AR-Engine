"""Deterministic task context from repository-owned frozen sources; no tools/API."""
from __future__ import annotations

import fnmatch
from pathlib import Path, PurePosixPath
import re
import subprocess

import workflow as core

POLICY = 'workflow/agent-policy.json'
NATIVE = {'.cpp', '.cc', '.cxx', '.h', '.hpp', '.hxx', '.glsl', '.vert', '.frag', '.comp', '.cmake'}


def changed_names(root: Path) -> list[str]:
    head = subprocess.run(['git', 'rev-parse', '--verify', 'HEAD'], cwd=root, capture_output=True, timeout=10)
    tracked = (core.run(['git', 'diff', '--name-only', '-z', 'HEAD', '--'], root).split('\0')
               if head.returncode == 0 else core.run(['git', 'ls-files', '-z', '--cached'], root).split('\0'))
    others = core.run(['git', 'ls-files', '-z', '--others', '--exclude-standard'], root).split('\0')
    return sorted({name for name in tracked + others if name and not name.startswith(('.agents/', 'docs/archive/'))})


def relative(name: str) -> str:
    if (not isinstance(name, str) or not name or '\\' in name or ':' in name
            or PurePosixPath(name).is_absolute() or any(x in ('', '..', '.') for x in name.split('/'))):
        raise ValueError('Context path must be a repository-relative POSIX path')
    return name


def write_path(name: str) -> str:
    relative(name)
    # Windows ignores case and trailing dots/spaces when resolving ordinary paths.
    first = name.split('/')[0].rstrip(' .').casefold()
    if first in ('.git', '.ar-workflow') or any(char in first for char in '*?['):
        raise ValueError('Write scope must be bounded and exclude Git/internal evidence paths')
    return name


def text(root: Path, name: str) -> str:
    path = core.within(root, root / relative(name))
    if not path.is_file() or path.stat().st_size > 100000:
        raise ValueError(f'Missing/oversized task context: {name}')
    return path.read_text(encoding='utf-8')


def policy(root: Path) -> dict:
    value = core.load(core.within(root, root / POLICY))
    if value.get('schema_version') != 1 or not isinstance(value.get('roles'), dict):
        raise ValueError('Unknown Agent policy schema')
    return value


def declaration(root: Path, change: str) -> dict:
    directory = core.change_dir(change, root)
    path = directory / 'context.json'
    value = core.load(path) if path.exists() else {
        'schema_version': 1, 'objective': change, 'non_goals': [],
        'tags': [], 'documents': [], 'skills': [], 'write_paths': []}
    if (value.get('schema_version') != 1 or not isinstance(value.get('objective'), str)
            or not value['objective'].strip()):
        raise ValueError('Task context requires schema_version=1 and objective')
    for key in ('non_goals', 'tags', 'documents', 'skills', 'write_paths'):
        if not isinstance(value.get(key), list):
            raise ValueError(f'Task context needs list: {key}')
    if any(not isinstance(x, str) or not x.strip() for key in
           ('non_goals', 'tags', 'skills', 'write_paths') for x in value[key]):
        raise ValueError('Task context lists require nonempty strings')
    for name in value['write_paths']:
        write_path(name)
        core.within(root, root / name)
    return value


def build(root: Path, change: str, changed: list[str]) -> dict:
    rule = policy(root)
    config = declaration(root, change)
    unknown = set(config['tags']) - set(rule['tag_skills'])
    if unknown:
        raise ValueError(f'Unknown task context tags: {sorted(unknown)}')
    native = ('native' in config['tags'] or rule['native_skill'] in config['skills']
              or any(Path(name).suffix in NATIVE or Path(name).name == 'CMakeLists.txt'
                     for name in changed + config['write_paths']))
    docs = list(rule['base_documents']) + list(config['documents'])
    base = f'openspec/changes/{change}/'
    docs += [{'path': base + name, 'purpose': 'task-contract', 'status': 'task',
              'basis': '当前任务材料；声明与执行证据分别核验，不自动产生用户批准'}
             for name in ('proposal.md', 'design.md', 'test-plan.md', 'tasks.md')]
    for name in (POLICY, base + 'context.json'):
        if (root / name).is_file():
            docs.append({'path': name, 'purpose': 'context-policy', 'status': 'task', 'basis': '上下文声明与角色策略'})
    if native:
        docs += rule['native_documents']
        docs += [{'path': p.relative_to(root).as_posix(), 'purpose': 'adr', 'status': 'confirmed',
                  'basis': 'ADR 决策历史；以文中适用范围及取代关系为准'}
                 for p in sorted((root / 'docs/adr').glob('*.md')) if p.name != 'README.md']
    skills = set(config['skills']) | {rule['communication_skill']}
    skills.update(rule['tag_skills'][tag] for tag in config['tags'])
    if native:
        skills.add(rule['native_skill'])
    for name in sorted(skills):
        if not re.fullmatch(r'ar-[a-z0-9-]+', name):
            raise ValueError('Only named project ar-* skills may be selected')
        docs.append({'path': f'.agents/skills/{name}/SKILL.md', 'purpose': 'skill',
                     'status': 'guidance', 'basis': '任务适用技能；加载不等于工具已运行，不扩大授权'})
    indexed = {}
    for item in docs:
        if (not isinstance(item, dict) or any(not isinstance(item.get(k), str) or not item[k].strip()
                for k in ('path', 'purpose', 'status', 'basis'))
                or item['status'] not in ('confirmed', 'reference', 'task', 'guidance')):
            raise ValueError('Malformed task context document declaration')
        name = relative(item['path'])
        body = text(root, name)
        if item['status'] == 'confirmed' and (
                re.search(r'(?m)^状态[：:]\s*草案(?:[。；;]|[ \t]*$)', body[:1000])
                or name.startswith(('docs/archive/', 'docs/research/'))):
            raise ValueError(f'Draft/reference cannot be promoted to confirmed: {name}')
        record = dict(item, sha256=core.digest((root / name).read_bytes()))
        if name in indexed and indexed[name] != record:
            raise ValueError(f'Conflicting task context declaration: {name}')
        indexed[name] = record
    if sum(len(text(root, name).encode('utf-8')) for name in indexed) > 500000:
        raise ValueError('Task context exceeds 500000-byte limit')
    return {'schema_version': 1, 'change': change, 'objective': config['objective'],
            'non_goals': config['non_goals'], 'write_paths': config['write_paths'],
            'skills': sorted(skills), 'documents': [indexed[n] for n in sorted(indexed)],
            'authority_note': 'Status and basis are traceability claims, not authenticated user approval; current user instructions control.'}


def validate(packet: Path, manifest: dict) -> list[str]:
    if 'task_context' not in manifest:
        return []  # Legacy/generic source-only packets retain their historical identity.
    try:
        stored = manifest['task_context']
        current = build(packet / 'snapshot', stored['change'],
                        manifest['reviewable_files'] + list(manifest.get('deletions', {})))
        return [] if current == stored else ['Frozen task context changed']
    except (ValueError, OSError, KeyError, TypeError) as error:
        return [f'Invalid task context: {error}']


def contents(packet: Path, manifest: dict) -> dict[str, str]:
    if validate(packet, manifest):
        raise ValueError('Frozen task context changed')
    return {item['path']: text(packet / 'snapshot', item['path'])
            for item in manifest.get('task_context', {}).get('documents', [])}


def permits(name: str, patterns: list[str]) -> bool:
    return any(fnmatch.fnmatchcase(name, pattern) for pattern in patterns)
