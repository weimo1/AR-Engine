"""Per-frozen-input paid-call admission and deadline; not a provider bill cap."""
import json
import queue
import threading
import time
import urllib.request
from pathlib import Path

import workflow as core


def limits(root: Path, budget: int, seconds: int) -> bool:
    policy = core.load(root / 'workflow/tool-lock.json')['ocr']
    if policy.get('review_policy_version') != 2:
        return False
    if not 1 <= budget <= 100000 or not 1 <= seconds <= 600:
        raise ValueError('Daily review round is limited to 100000 tokens / 600 seconds; split task, no automatic increase')
    return True


def state(packet: Path, budget: int, seconds: int) -> tuple[Path, dict]:
    path = packet / 'paid-round.json'
    if (packet / 'manifest.json').is_file():
        identity = core.digest(core.canonical(core.load(packet / 'manifest.json')['state']))
        shared = core.within(packet.parent, packet.parent / 'paid-rounds' / (identity + '.json'))
        # Existing packet-local paid evidence cannot silently gain a fresh round.
        # Explicit reconciliation is required rather than guessing overlapping usage.
        for legacy in packet.parent.glob('*/paid-round.json'):
            legacy_manifest = legacy.parent / 'manifest.json'
            if not legacy_manifest.is_file() or core.digest(core.canonical(
                    core.load(legacy_manifest)['state'])) != identity:
                continue
            previous = core.load(legacy)
            if previous.get('pending') or previous.get('spent', 0):
                raise ValueError('Legacy packet-local paid round requires reconciliation; no fresh budget')
        path = shared
    value = core.load(path) if path.exists() else {
        'started_at': time.time(), 'token_limit': budget, 'seconds_limit': seconds,
        'spent': 0, 'pending': False}
    if value['pending']:
        raise ValueError('Previous paid request has unknown in-flight cost; reconcile before further payment')
    # A later command can tighten, never reset or extend this frozen input's round.
    value['token_limit'] = min(value['token_limit'], budget)
    value['seconds_limit'] = min(value['seconds_limit'], seconds)
    return path, value


def preflight(packet: Path, budget: int, seconds: int, estimate: int) -> None:
    _, value = state(packet, budget, seconds)
    if value['spent'] + estimate > value['token_limit']:
        raise ValueError('Shared review round budget insufficient; no request or automatic top-up')
    if time.time() >= value['started_at'] + value['seconds_limit']:
        raise ValueError('Shared review round deadline exceeded; no automatic restart')


def send(packet: Path, http, budget: int, seconds: int, estimate: int, destination: Path) -> bytes:
    preflight(packet, budget, seconds, estimate)
    path, value = state(packet, budget, seconds)
    remaining = value['started_at'] + value['seconds_limit'] - time.time()
    value['pending'] = True
    core.save(path, value)  # Written before any request; interruptions stay unknown.
    result = queue.Queue(maxsize=1)

    def fetch():
        try:
            with urllib.request.urlopen(http, timeout=max(0.01, remaining)) as response:
                result.put(response.read(4 * 1024 * 1024 + 1))
        except BaseException as error:
            result.put(error)

    threading.Thread(target=fetch, daemon=True).start()
    try:
        raw = result.get(timeout=max(0.01, remaining))
    except queue.Empty as error:
        raise ValueError('Paid round deadline reached; request cost unknown, no retry') from error
    if isinstance(raw, BaseException):
        raise raw
    destination.write_bytes(raw)  # Preserve usage even for malformed/truncated samples.
    parsed = json.loads(raw)
    tokens = parsed.get('usage', {}).get('total_tokens')
    if type(tokens) is not int or tokens <= 0:
        raise ValueError('Paid response usage unknown; round remains blocked')
    value.update(spent=value['spent'] + tokens, pending=False)
    core.save(path, value)
    if value['spent'] > value['token_limit'] or time.time() >= value['started_at'] + value['seconds_limit']:
        raise ValueError('Paid round exceeded token/time allowance; response preserved, no continuation')
    return raw
