"""Build and test the project using a configured command table.

The table is schema-checked and executed by scripts/workflow_p0.py for local runs and CI.
Paths use ${REPO}, ${OUT}, ${VCPKG_ROOT} and ${DEPS}; ${OUT} defaults to
<repo>/build. Observed exit codes and timings go to <output>/logs/p0-checks/results.json.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import workflow_p0 as p0


def build_commands(table: dict) -> list[str]:
    commands = {command['id']: command for command in table['commands']}
    pending = [name for name, command in commands.items() if '--build' in command['argv']]
    if not pending:
        raise ValueError('No build commands declared in the command table')
    selected = set()
    while pending:
        name = pending.pop()
        if name not in selected:
            selected.add(name)
            pending.extend(commands[name]['depends_on'])
    return [name for name in commands if name in selected]


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", default=None,
                        help="command table path (default: <repo>/workflow/p0-checks.json)")
    parser.add_argument("--output-root", default=None,
                        help="root for ${OUT} (default: <repo>/build)")
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--phase', choices=('build', 'test'), help='build only, or build and run required tests')
    mode.add_argument("--only", nargs="+", default=None, help="run only the named command ids")
    parser.add_argument('--test-file', action='append', type=Path, default=[],
                        help='affected test source under tests/ (repeat for multiple files)')
    args = parser.parse_args(argv)
    if args.phase == 'test' and not args.test_file:
        parser.error('--phase test requires at least one --test-file')
    if args.test_file and args.phase != 'test':
        parser.error('--test-file is only used with --phase test')

    repo_root = Path(__file__).resolve().parent.parent
    table_path = Path(args.manifest) if args.manifest else repo_root / "workflow" / "p0-checks.json"
    output_root = Path(args.output_root).resolve() if args.output_root else repo_root / "build"

    try:
        tests_root = (repo_root / 'tests').resolve()
        for name in args.test_file:
            path = (repo_root / name).resolve()
            if not path.is_relative_to(tests_root) or not path.is_file():
                raise ValueError(f'Test file missing or outside tests/: {name}')
        table = p0.load_table(table_path)
        only = build_commands(table) if args.phase == 'build' else args.only
        if args.phase == 'test' and not any(
            command['parser_id'] == 'ctest-junit' and command['required'] and command['expected_cases']
            for command in table['commands']
        ):
            raise ValueError('No required CTest cases declared in the command table')
        results = p0.run_table(table, repo_root, output_root, p0.sanitized_environment(), only)
    except (p0.TableError, ValueError, OSError) as error:
        print(f"build_checks: {error}", file=sys.stderr)
        return 1

    for entry in results["commands"]:
        marker = "ok" if entry["passed"] else "skip"
        print(f"[{marker}] {entry['id']} (exit {entry['child_exit']}, "
              f"{entry['elapsed_seconds']}s) -> {entry['log']}")
    print(f"[results] {results['results_path']}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
