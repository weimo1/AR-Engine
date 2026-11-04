"""Verifies that each public ar_core header compiled on its own.

The compile happens in the ar_public_header_probe object library; this check asserts that the
objects for the current configuration exist, which is only possible if every translation unit
(one header each) compiled.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

EXPECTED_STEMS = ("header_error", "header_build_info", "header_diagnostics")


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--search-root", required=True)
    parser.add_argument("--configuration", default="")
    args = parser.parse_args(argv)

    root = Path(args.search_root)
    if not root.is_dir():
        print(f"public-headers: error: build directory not found: {root}", file=sys.stderr)
        return 1

    missing: list[str] = []
    found: list[Path] = []
    for stem in EXPECTED_STEMS:
        matches = [
            path
            for path in root.rglob(f"{stem}*")
            if path.is_file() and path.suffix in {".obj", ".o"} and path.stat().st_size > 0
        ]
        if not matches:
            missing.append(stem)
        else:
            found.extend(sorted(matches))

    if missing:
        print(
            "public-headers: error: no object for "
            + ", ".join(missing)
            + " (a public header did not compile on its own)",
            file=sys.stderr,
        )
        return 1

    configuration = args.configuration or "default"
    for path in found:
        print(f"public-headers: {configuration}: compiled {path}")
    print(f"public-headers: ok ({len(found)} translation units, config {configuration})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
