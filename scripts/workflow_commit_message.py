"""Deterministic project commit-message check; no model or network calls."""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys

TYPES = ('feat', 'fix', 'docs', 'refactor', 'perf', 'test', 'build', 'ci', 'chore', 'revert')
TITLE = re.compile(r'(' + '|'.join(TYPES) + r')\(([a-z][a-z0-9-]{0,31})\)(!)?: ([^\t\r\n]+)')
FIELDS = ('Why', 'What', 'Tests', 'Refs')
PLACEHOLDERS = {'todo', 'tbd', '待补', '待填写', '...', '<填写>', '<原因>', '<变更>', '<验证>', '<关联>'}


def validate(message: str) -> list[str]:
    if any(ord(char) < 32 and char not in '\n\r\t' for char in message):
        return ['Commit message contains control characters']
    lines = [line for line in message.splitlines() if not line.startswith('#')]
    if not lines:
        return ['Commit message is empty']
    title = lines[0]
    match = TITLE.fullmatch(title)
    errors = []
    if not match or title != title.strip() or len(title) > 100:
        errors.append('Title must be type(scope): summary, lowercase type/scope, at most 100 characters')
    elif (not 4 <= len(match[4].strip()) <= 72 or match[4] != match[4].strip()
          or match[4].lower().rstrip('.!。') in {'update', 'changes', 'wip', '修改', '修复问题', '更新'}):
        errors.append('Summary must describe a concrete change in 4..72 characters')
    if len(lines) < 2 or lines[1] != '':
        errors.append('Separate title and body with a blank line')
    required = list(FIELDS)
    if match and match[3]:
        required.append('BREAKING CHANGE')
    for field in required:
        values = [line[len(field) + 1:].strip() for line in lines[2:] if line.startswith(field + ':')]
        if len(values) != 1 or not values[0] or values[0].lower() in PLACEHOLDERS:
            errors.append(f'Body needs exactly one nonempty {field}: value, without placeholders')
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--file', type=Path, required=True)
    args = parser.parse_args()
    try:
        if args.file.stat().st_size > 32768:
            raise ValueError('Commit message exceeds 32 KiB')
        errors = validate(args.file.read_text(encoding='utf-8-sig'))
    except (OSError, UnicodeError, ValueError) as error:
        errors = [str(error)]
    if errors:
        print('COMMIT MESSAGE BLOCKED:\n- ' + '\n- '.join(errors), file=sys.stderr)
        return 1
    print('Commit message format passed; content checks and independent review remain separate')
    return 0


if __name__ == '__main__':
    sys.exit(main())
