"""Check changed first-party C++ lines against the repository's mechanical rules."""

import argparse
import posixpath
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys


SOURCE_ROOTS = {"engine", "examples", "tests", "tools"}
CPP_EXTENSIONS = {".h", ".hpp", ".cpp", ".cc", ".cxx"}
INCLUDE = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]')
HUNK = re.compile(r"^@@ .* \+(\d+)(?:,(\d+))? @@")
GPU_WAIT = re.compile(r"\bvk(?:Device|Queue)WaitIdle\s*\(")


CPP_TOKEN = re.compile(
    r'(?P<raw>R"(?P<delimiter>[^\s()\\]{0,16})\(.*?\)(?P=delimiter)")'
    r'|(?P<quoted>"(?:\\.|[^"\\\n])*"|\x27(?:\\.|[^\x27\\\n])*\x27)'
    r'|//(?:\\\n|[^\n])*|/\*.*?(?:\*/|\Z)',
    re.DOTALL,
)


def mask_non_code(source, *, keep_quotes=False):
    def mask(match):
        if keep_quotes and match.group("quoted"):
            return match[0]
        return re.sub(r"[^\n]", " ", match[0])

    return CPP_TOKEN.sub(mask, source)


def git(*args):
    return subprocess.check_output(["git", *args]).decode("utf-8")


def check_source(name, lines, changed):
    path = PurePosixPath(name)
    parts = path.parts
    module = parts[1] if len(parts) >= 3 and parts[0] == "engine" else None
    source = "\n".join(lines)
    directives = mask_non_code(source, keep_quotes=True).split("\n")
    code_lines = mask_non_code(source).split("\n")
    for number in sorted(changed):
        if not 1 <= number <= len(code_lines):
            continue
        include = INCLUDE.match(directives[number - 1])
        if include:
            target = include[1].replace("\\", "/")
            candidates = (posixpath.normpath(target),
                          posixpath.normpath(posixpath.join(str(path.parent), target)))
            for candidate in candidates:
                layout = PurePosixPath(candidate).parts
                if (len(layout) >= 4 and layout[0] == "engine"
                        and layout[1] != module and layout[2] == "src"):
                    yield number, "error", "include the other module's public header"
                    break
            if module == "core" and re.match(r"^(?:vulkan/|volk\.h$|vk_mem_alloc\.h$)", target):
                yield number, "error", "core must not include Vulkan headers"
        code = code_lines[number - 1]
        if path.suffix in {".h", ".hpp"} and re.match(r"^\s*using\s+namespace\b", code):
            yield number, "error", "avoid using namespace in a header"
        if module and GPU_WAIT.search(code):
            yield number, "notice", "review this GPU-wide or queue-wide wait for frame-loop stalls"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("base", help="Git base commit. Use HEAD^ for the last commit.")
    args = parser.parse_args()
    base = args.base
    if base == "0" * 40:
        base = subprocess.check_output(
            ["git", "hash-object", "-w", "-t", "tree", "--stdin"], input=b"").decode().strip()
    files = git("diff", "--name-only", "--diff-filter=ACMRT", "-z", base, "HEAD", "--")
    failures = 0
    checked = 0
    for name in filter(None, files.split("\0")):
        path = PurePosixPath(name)
        if path.parts[0] not in SOURCE_ROOTS or path.suffix not in CPP_EXTENSIONS:
            continue
        if not Path(name).is_file() or Path(name).is_symlink():
            continue
        patch = git("diff", "--unified=0", "--no-ext-diff", base, "HEAD", "--", name)
        changed = set()
        ranges = []
        for line in patch.splitlines():
            match = HUNK.match(line)
            if match:
                start, count = int(match[1]), int(match[2] or 1)
                if count:
                    changed.update(range(start, start + count))
                    ranges.append(f"--lines={start}:{start + count - 1}")
        if not changed:
            continue
        checked += 1
        result = subprocess.run(
            ["clang-format", "--style=file", "--dry-run", "--Werror", *ranges, name],
            check=False,
        )
        failures += result.returncode != 0
        for number, severity, message in check_source(
                name, Path(name).read_text(encoding="utf-8-sig").splitlines(), changed):
            print(f"{name}:{number}: {severity}: {message}")
            failures += severity == "error"
    print(f"C++ quality checked {checked} file(s), {failures} failing check(s)")
    return int(failures != 0)


if __name__ == "__main__":
    sys.exit(main())
