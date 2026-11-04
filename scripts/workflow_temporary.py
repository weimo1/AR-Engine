"""Only tool-created, unreferenced scratch payloads may be reversibly archived."""
from pathlib import Path
import json
import os
import re
import stat
import uuid

import workflow as core
import workflow_agents as agents
import workflow_context as context

BASE = '.ar-workflow/temporary'
FORMAT = 'ar.temporary/v1'


def accounting_data(value) -> bool:
    keys = {'usage', 'budget', 'total_tokens', 'checks', 'responses', 'session_id', 'prompt_tokens', 'known_tokens'}
    if isinstance(value, dict):
        return bool(keys.intersection(value)) or any(accounting_data(x) for x in value.values())
    if isinstance(value, list):
        return any(accounting_data(x) for x in value)
    return False


def identifier(value: str) -> str:
    if not isinstance(value, str) or not re.fullmatch(r'[0-9a-f]{24}', value):
        raise ValueError('Invalid temporary/assignment identifier')
    return value


def safe(root: Path, relative: str) -> Path:
    """Check unresolved components before resolve; reject Windows path aliases."""
    context.relative(relative)
    path = root.resolve()
    for component in relative.split('/'):
        if component.rstrip(' .') != component or any(c in component for c in '*?['):
            raise ValueError('Unsafe temporary path component')
        path = path / component
        try:
            attrs = path.lstat()
        except FileNotFoundError:
            continue
        if (stat.S_ISLNK(attrs.st_mode) or getattr(attrs, 'st_file_attributes', 0) & 0x400
                or path.is_junction()):
            raise ValueError('Temporary path contains a reparse point')
    return core.within(root, path)


def idle(root: Path, owner: str) -> None:
    identifier(owner)
    agents.assert_idle(root)
    if agents.state(root)['assignments'].get(owner, {}).get('status') != 'completed':
        raise ValueError('Temporary owner is not completed')
    import workflow_relay
    assignment = core.load(core.within(root, root / '.ar-workflow/agents' / owner / 'assignment.json'))
    workflow_relay.handoff(root, owner, assignment['change'])
    import workflow_dsh
    for file in workflow_dsh.jobs(root):
        if core.load(core.within(root, file)).get('runtime_stopped') is not True:
            raise ValueError('Unknown/in-flight DSH runtime blocks temporary maintenance')


def create(root: Path, owner: str, text: str) -> dict:
    state, _, _ = agents.inspect(root, owner)
    identity = uuid.uuid4().hex[:24]
    relative = f'{BASE}/{identity}/payload'
    file = safe(root, relative)
    file.parent.mkdir(parents=True, exist_ok=False)
    data = text.encode('utf-8')
    with file.open('xb') as stream:
        stream.write(data)
    record = {'format': FORMAT, 'id': identity, 'owner': owner, 'path': relative,
              'sha256': core.digest(data), 'inode': file.stat().st_ino, 'status': 'live'}
    core.save(file.parent / 'record.json', record)
    state['assignments'][owner].setdefault('temporary_created', {})[identity] = core.digest(core.canonical(record))
    core.save(agents.state_path(root), state)
    return record


def references(root: Path, record: dict, ignored: set[Path]) -> list[str]:
    # UUID is also an indirect identifier; search it, not just literal full paths.
    needle = record['id'].encode('ascii')
    hits = []
    for folder, dirs, files in os.walk(root, followlinks=False):
        dirs[:] = [n for n in dirs if n not in ('.git', 'node_modules', '.tools', '__pycache__')]
        for name in dirs:
            safe(root, (Path(folder) / name).relative_to(root).as_posix())
        for name in files:
            path = Path(folder) / name
            if path in ignored:
                continue
            safe(root, path.relative_to(root).as_posix())
            if path == agents.state_path(root):
                metadata = core.load(path)
                metadata['assignments'][record['owner']].get('temporary_created', {}).pop(record['id'], None)
                if needle in core.canonical(metadata).lower():
                    hits.append(path.relative_to(root).as_posix())
                continue  # The creation pin itself is ownership metadata, not a consumer reference.
            if path.suffix.lower() == '.json':
                decoded = json.dumps(core.load(path), ensure_ascii=False).encode('utf-8').lower()
                if needle in decoded:
                    hits.append(path.relative_to(root).as_posix())
                continue
            with path.open('rb') as stream:
                previous = b''
                while chunk := stream.read(65536):
                    block = previous + chunk
                    if needle in block.lower():
                        hits.append(path.relative_to(root).as_posix())
                        break
                    previous = block[-len(needle):]
    return hits


def preview(root: Path, owner: str, plan_file: Path | None = None) -> dict:
    identifier(owner)
    result = {'format': FORMAT, 'root': str(root.resolve()), 'owner': owner,
              'input_sha256': core.digest(core.canonical(core.input_state(root, 'workspace'))),
              'candidates': [], 'protected': [], 'blockers': []}
    try:
        idle(root, owner)
    except ValueError as error:
        result['blockers'].append(str(error))
        return result
    base = safe(root, BASE)
    if not base.exists():
        return result
    for directory in sorted(base.iterdir()):
        if directory.name in ('archive', 'plans'):
            continue
        identifier(directory.name)
        record_path = safe(root, f'{BASE}/{directory.name}/record.json')
        record = core.load(record_path)
        if record.get('owner') != owner or record.get('status') != 'live':
            continue
        try:
            expected = f'{BASE}/{directory.name}/payload'
            if (record.get('format') != FORMAT or record.get('id') != directory.name
                    or record.get('path') != expected):
                raise ValueError('Unknown temporary provenance')
            pin = agents.state(root)['assignments'][owner].get('temporary_created', {}).get(record['id'])
            if pin != core.digest(core.canonical(record)):
                raise ValueError('Temporary creation is not registered or record changed')
            payload = safe(root, expected)
            data = payload.read_bytes()
            if (not payload.is_file() or payload.stat().st_nlink != 1
                    or payload.stat().st_ino != record.get('inode') or core.digest(data) != record.get('sha256')):
                raise ValueError('Temporary payload identity/content changed')
            if core.run(['git', 'ls-files', '--', expected], root).strip():
                raise ValueError('Tracked files are protected')
            try:
                parsed = json.loads(data)
            except (ValueError, UnicodeError):
                parsed = None
            if accounting_data(parsed) or any(key in data.lower() for key in
                    (b'"usage"', b'"budget"', b'"total_tokens"', b'"checks"', b'"responses"')):
                raise ValueError('Usage/budget/verification data is protected')
            ignore = {payload, record_path}
            if plan_file is not None:
                ignore.add(plan_file)
            refs = references(root, record, ignore)
            if refs:
                raise ValueError('Referenced by: ' + ', '.join(refs[:5]))
            result['candidates'].append({'id': record['id'], 'path': expected,
                'bytes': len(data), 'sha256': core.digest(data), 'record_sha256': core.digest(core.canonical(record))})
        except (OSError, ValueError) as error:
            result['protected'].append({'id': directory.name, 'reason': str(error)})
    return result


def archive(root: Path, plan_path: Path) -> dict:
    plan_path = core.within(root, plan_path.resolve())
    # Plans belong in this reserved namespace; never ignore arbitrary evidence files.
    relative = plan_path.relative_to(root.resolve()).as_posix()
    if not relative.startswith(BASE + '/plans/'):
        raise ValueError('Save preview plans under .ar-workflow/temporary/plans/')
    safe(root, relative)
    plan = core.load(plan_path)
    if not isinstance(plan, dict) or plan.get('format') != FORMAT:
        raise ValueError('Invalid temporary plan')
    actual = preview(root, plan['owner'], plan_path)
    if plan != actual or actual['blockers']:
        raise ValueError('Temporary plan stale, referenced or blocked; preview again')
    if not actual['candidates']:
        return {'archived': 0, 'journal': None}
    batch = uuid.uuid4().hex[:24]
    directory = safe(root, f'{BASE}/archive/{batch}')
    directory.mkdir(parents=True, exist_ok=False)
    journal = {'format': FORMAT, 'batch': batch, 'owner': actual['owner'], 'status': 'in_progress', 'items': [],
               'plan_path': relative, 'plan_sha256': core.digest(plan_path.read_bytes())}
    for item in actual['candidates']:
        record_path = safe(root, f'{BASE}/{item["id"]}/record.json')
        record = core.load(record_path)
        source = safe(root, item['path'])
        destination = safe(root, f'{BASE}/archive/{batch}/{item["id"]}')
        if (destination.exists() or core.digest(source.read_bytes()) != item['sha256']
                or core.digest(core.canonical(record)) != item['record_sha256']):
            raise ValueError('Archive target conflict or payload changed')
        entry = {'id': item['id'], 'original': record, 'target': destination.relative_to(root).as_posix()}
        journal['items'].append(entry)
        core.save(directory / 'journal.json', journal)  # Recoverable even if interrupted after rename.
        source.rename(destination)
        core.save(record_path, dict(record, status='archived', archive=entry['target']))
    journal['status'] = 'archived'
    core.save(directory / 'journal.json', journal)
    return {'archived': len(journal['items']), 'journal': str(directory / 'journal.json')}


def restore(root: Path, journal_path: Path) -> dict:
    journal_path = core.within(root, journal_path.resolve())
    relative = journal_path.relative_to(root.resolve()).as_posix()
    parts = relative.split('/')
    if len(parts) != 5 or parts[:3] != ['.ar-workflow', 'temporary', 'archive'] or parts[-1] != 'journal.json':
        raise ValueError('Invalid restore journal path')
    identifier(parts[3])
    safe(root, relative)
    journal = core.load(journal_path)
    if (set(journal) != {'format', 'batch', 'owner', 'status', 'items', 'plan_path', 'plan_sha256'}
            or journal.get('format') != FORMAT or journal.get('batch') != parts[3]
            or journal.get('status') not in ('in_progress', 'archived', 'restored')):
        raise ValueError('Unknown restore journal')
    idle(root, journal['owner'])
    moves = []
    if not journal['plan_path'].startswith(BASE + '/plans/'):
        raise ValueError('Restore plan path is outside reserved namespace')
    plan_path = safe(root, journal['plan_path'])
    if plan_path.exists() and core.digest(plan_path.read_bytes()) != journal['plan_sha256']:
        raise ValueError('Restore plan changed; inspect original references')
    ignored = {journal_path, plan_path}
    for item in journal['items']:
        if set(item) != {'id', 'original', 'target'}:
            raise ValueError('Unknown restore entry')
        identity = identifier(item['id'])
        original = item['original']
        source_name = f'{BASE}/{identity}/payload'
        target_name = f'{BASE}/archive/{parts[3]}/{identity}'
        if (original.get('format') != FORMAT or original.get('id') != identity
                or original.get('owner') != journal['owner'] or original.get('path') != source_name
                or item.get('target') != target_name):
            raise ValueError('Restore provenance changed')
        pin = agents.state(root)['assignments'][journal['owner']].get('temporary_created', {}).get(identity)
        if pin != core.digest(core.canonical(original)):
            raise ValueError('Restore creation provenance is not registered')
        record_path = safe(root, f'{BASE}/{identity}/record.json')
        record = core.load(record_path)
        if record not in (original, dict(original, status='archived', archive=target_name)):
            raise ValueError('Restore record changed')
        source, target = safe(root, source_name), safe(root, target_name)
        ignored.update((source, target, record_path))
        if source.exists() and not target.exists() and core.digest(source.read_bytes()) == original['sha256']:
            continue  # Already restored or never moved.
        if (source.exists() or not target.is_file() or target.stat().st_nlink != 1
                or target.stat().st_ino != original['inode'] or core.digest(target.read_bytes()) != original['sha256']):
            raise ValueError('Restore conflict or archived content changed')
        moves.append((target, source, record_path, original))
    # A later consumer may reference the archived payload or the journal itself.
    for identity in [parts[3]] + [item['id'] for item in journal['items']]:
        if references(root, {'id': identity, 'owner': journal['owner']}, ignored):
            raise ValueError('Restore would change referenced archive evidence')
    for target, source, record_path, original in moves:
        if source.exists():
            raise ValueError('Restore would overwrite a new file')
        target.rename(source)
        core.save(record_path, original)
    journal['status'] = 'restored'
    core.save(journal_path, journal)
    return {'restored': len(moves)}
