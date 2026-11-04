"""Run the P0 command table (workflow/p0-checks.json) from the command line.

The table is schema-checked and executed by scripts/workflow_p0.py, the same code the
workflow verify path uses, so a local run and a verify run cannot disagree about what
"required" means. Paths use ${REPO}, ${OUT}, ${VCPKG_ROOT} and ${DEPS}; ${OUT} defaults to
<repo>/build. Observed exit codes and timings go to <output>/logs/p0-checks/results.json.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import workflow_p0 as p0


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", default=None,
                        help="command table path (default: <repo>/workflow/p0-checks.json)")
    parser.add_argument("--output-root", default=None,
                        help="root for ${OUT} (default: <repo>/build)")
    parser.add_argument("--only", nargs="+", default=None, help="run only the named command ids")
    args = parser.parse_args(argv)

    repo_root = Path(__file__).resolve().parent.parent
    table_path = Path(args.manifest) if args.manifest else repo_root / "workflow" / "p0-checks.json"
    output_root = Path(args.output_root).resolve() if args.output_root else repo_root / "build"

    try:
        table = p0.load_table(table_path)
        results = p0.run_table(table, repo_root, output_root, p0.sanitized_environment(), args.only)
    except (p0.TableError, ValueError) as error:
        print(f"run_p0_checks: {error}", file=sys.stderr)
        return 1

    for entry in results["commands"]:
        marker = "ok" if entry["passed"] else "skip"
        print(f"[{marker}] {entry['id']} (exit {entry['child_exit']}, "
              f"{entry['elapsed_seconds']}s) -> {entry['log']}")
    print(f"[results] {results['results_path']}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
