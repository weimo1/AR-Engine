"""Import real independent-context reviews; local traceability, not authentication."""
from pathlib import Path
import re
import uuid

import workflow as core


def report(packet: Path, output: Path) -> dict:
    manifest = core.load(packet / 'manifest.json')
    if core.validate_packet(packet, manifest):
        raise ValueError('Independent-context packet changed')
    record = core.load(output / 'run.json')
    if (record.get('mode') != 'independent-context-v1' or record.get('status') != 'finished'
            or record.get('fingerprint') != manifest['fingerprint']):
        raise ValueError('Independent-context run identity mismatch')
    receipts = record.get('receipts')
    if not isinstance(receipts, list) or not receipts:
        raise ValueError('Independent-context receipts missing')
    expected = set(manifest['reviewable_files']) - set(manifest.get('deterministic_files', []))
    covered, reviewers, findings = set(), set(), []
    for entry in receipts:
        if not isinstance(entry, dict) or not re.fullmatch(r'source-[0-9]+\.json', entry.get('file', '')):
            raise ValueError('Invalid independent-context receipt path')
        path = core.within(output, output / entry['file'])
        if core.digest(path.read_bytes()) != entry.get('sha256'):
            raise ValueError('Independent-context raw evidence changed')
        bundle = core.load(path)
        raw, assignment, handoff = (bundle[k] for k in ('review', 'assignment', 'handoff'))
        identity = raw.get('assignment')
        reviewer = raw.get('reviewer')
        if (raw.get('schema') != 'ar.independent-context/v1' or raw.get('status') != 'complete'
                or raw.get('fingerprint') != manifest['fingerprint']
                or raw.get('independent_context') is not True
                or not isinstance(reviewer, str) or not reviewer.strip()
                or reviewer == record.get('coordinator') or reviewer in reviewers
                or assignment.get('id') != identity or assignment.get('agent') != reviewer
                or assignment.get('role') != 'reviewer' or assignment.get('access') != 'read'
                or assignment.get('change') != manifest.get('task_context', {}).get('change')
                or assignment.get('input') != manifest['state']
                or handoff.get('assignment') != identity or handoff.get('status') != 'completed'
                or handoff.get('start_input') != manifest['state'] or handoff.get('end_input') != manifest['state']):
            raise ValueError('Review needs a completed, current, independent read-only assignment')
        paths = raw.get('reviewed_files')
        if (not isinstance(paths, list) or not paths or any(not isinstance(p, str) for p in paths)
                or len(set(paths)) != len(paths) or not set(paths).issubset(expected)):
            raise ValueError('Independent-context coverage malformed')
        if not isinstance(raw.get('findings'), list):
            raise ValueError('Independent-context findings missing')
        for finding in raw['findings']:
            if (not isinstance(finding, dict) or finding.get('path') not in paths
                    or finding.get('level') not in ('blocking', 'must_fix', 'suggestion')
                    or type(finding.get('line')) is not int or finding['line'] < 1
                    or not isinstance(finding.get('content'), str) or not finding['content'].strip()):
                raise ValueError('Independent-context finding malformed')
            # A receipt cannot self-resolve a finding. Review the repaired input afresh.
            findings.append(dict(finding, status='open', reviewer=reviewer))
        reviewers.add(reviewer)
        covered.update(paths)
    if covered != expected:
        raise ValueError('Independent-context review coverage incomplete')
    verification = core.load(core.within(packet, packet / record['verification']))
    if verification.get('status') != 'passed' or verification.get('fingerprint') != manifest['fingerprint']:
        raise ValueError('Independent-context review needs current full verification')
    return {'schema_version': 1, 'fingerprint': manifest['fingerprint'],
            'reviewer': ', '.join(sorted(reviewers)), 'independent_context': True,
            'status': 'complete', 'decision': 'request_changes' if findings else 'pass',
            'reviewed_files': sorted(covered), 'checked_without_model': manifest.get('deterministic_files', []),
            'findings': findings, 'receipts': receipts,
            'limitations': ['Local receipts and hashes do not authenticate reviewers or grant remote approval',
                            'Coordinator must establish real separate reviewer contexts; role names alone do not prove it']}


def ingest(root: Path, packet: Path, evidence: list[Path], coordinator: str, verification: Path) -> Path:
    import workflow_agents as agents
    manifest = core.load(packet / 'manifest.json')
    if not coordinator.strip() or manifest['state'] != core.input_state(root, manifest['scope']):
        raise ValueError('Missing coordinator or stale independent-context input')
    output = packet / ('context-' + uuid.uuid4().hex[:12])
    output.mkdir()
    receipts = []
    for number, source in enumerate(evidence):
        raw = core.load(core.within(root, source.resolve()))
        identity = raw.get('assignment', '')
        if not isinstance(identity, str) or not re.fullmatch(r'[0-9a-f]{24}', identity):
            raise ValueError('Invalid independent reviewer assignment')
        directory = core.within(root, root / '.ar-workflow/agents' / identity)
        assignment = core.load(directory / 'assignment.json')
        handoff = core.load(directory / 'handoff.json')
        state = agents.state(root)['assignments'].get(identity, {})
        if (state.get('status') != 'completed'
                or core.digest(core.canonical(assignment)) != state.get('assignment_sha256')
                or core.digest((directory / 'task.md').read_bytes()) != state.get('task_sha256')
                or source.resolve().relative_to(root.resolve()).as_posix() not in handoff.get('result_claim', {}).get('evidence', [])):
            raise ValueError('Independent review lacks intact package and completed evidence handoff')
        bundle = {'review': raw, 'assignment': assignment, 'handoff': handoff}
        destination = output / f'source-{number}.json'
        core.save(destination, bundle)
        receipts.append({'file': destination.name, 'sha256': core.digest(destination.read_bytes())})
    verification_name = core.within(packet, verification).relative_to(packet.resolve()).as_posix()
    core.save(output / 'run.json', {'mode': 'independent-context-v1', 'status': 'finished',
              'fingerprint': manifest['fingerprint'], 'coordinator': coordinator, 'receipts': receipts,
              'verification': verification_name})
    result = report(packet, output)
    if manifest['state'] != core.input_state(root, manifest['scope']):
        raise ValueError('Source changed during independent-context import')
    core.save(output / 'report.json', result)
    return output / 'report.json'
