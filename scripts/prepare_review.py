"""Prepare OCR input from staged changes and print the manual handoff."""

import argparse
import subprocess
import sys
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--change', help='Include the named OpenSpec change context')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    status = subprocess.run(['git', 'diff', '--cached', '--quiet'], cwd=root).returncode
    if status != 1:
        print('Stage this batch before preparing OCR input.' if status == 0
              else 'Could not read the staged changes.', file=sys.stderr)
        return 1 if status == 0 else status
    command = [sys.executable, '-B', 'scripts/workflow.py', 'review-prepare', '--staged']
    if args.change:
        command.extend(['--change', args.change])
    result = subprocess.run(command, cwd=root)
    if result.returncode == 0:
        print('Open your review chat. Share the OCR packet printed above, the relevant design section, '
              'and the actual build/test results for this staged version.')
    return result.returncode


if __name__ == '__main__':
    sys.exit(main())
