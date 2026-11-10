"""Drive the TASK.md queue unattended: (tests) -> implement -> gate -> review -> commit -> next.

Each task runs in a fresh headless agent process (Codex or Claude Code), so no conversation
context accumulates. State lives only in TASK.md and Git. The gate is run by this script, not
trusted from the agent's report. Stops at a task marked [人工], on a red gate after the retry
budget, on a blocking review that survives one fix round, or when the queue is empty.

    python scripts/autorun.py --agent codex --tester claude --reviewer claude
    python scripts/autorun.py --dry-run
"""
from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TASK_LINE = re.compile(r'^- \[ \] (T\d+) (.+)$')
GATE = [sys.executable, '-B', 'scripts/build_checks.py']
GATE_RETRIES = 3

# Headless commands. The prompt is passed on stdin. Edit here if a CLI changes its flags.
AGENTS = {
    'codex': {
        'write': ['codex', 'exec', '--sandbox', 'workspace-write', '-'],
        'read': ['codex', 'exec', '--sandbox', 'read-only', '-'],
    },
    'claude': {
        'write': ['claude', '-p', '--permission-mode', 'acceptEdits',
                  '--allowedTools', 'Bash,Read,Edit,Write,Glob,Grep'],
        'read': ['claude', '-p', '--allowedTools', 'Read,Glob,Grep,Bash(git diff:*),Bash(git log:*)'],
    },
}

TEST_PROMPT = """你是 AR Engine 的测试工程师，工作区 {root}。先读 AGENTS.md 并遵守。
只为 TASK.md 里的这一项任务写验收测试，不实现功能：

{task}

要求：
1. 按“验收”在 tests/ 下写 Catch2 用例，用例名与任务里的 CTest 名一致，并登记到
   tests/CMakeLists.txt 和 workflow/p0-checks.json 的 expected_cases。
2. 断言写可观察行为，覆盖主路径和真实会遇到的失败，不堆穷举矩阵。
3. 不修改 engine/ 下的实现；需要的公开接口按任务目标和相邻模块的约定假定，写在测试里。
4. 此时编译失败是预期的，不要运行门禁，不要提交。
"""

TESTS_READY = """
验收测试已由测试角色写好（见 git status）。实现到它们通过；接口形状不合适可以调整测试的
调用方式，但不得删除用例或削弱断言。
"""

DEV_PROMPT = """你是 AR Engine 的开发者，工作区 {root}。先读 AGENTS.md 并遵守。
只做 TASK.md 里的这一项任务，不做后续任务：

{task}

要求：
1. 按 TDD 实现并补测试。开发中用 `python scripts/build_checks.py --fast`（增量 Debug），
   收尾前跑一次完整的 `python scripts/build_checks.py` 直到全绿。
2. 全绿后在 TASK.md 把该任务勾选为 [x]，并在标题行末尾补一句结果。
3. 把提交信息（按 docs/standards/Git提交信息规范.md，含 Why/What/Tests/Refs）写入 {msg}。
4. 不要执行 git commit 或 git push，提交由外层脚本完成。不要新建记录过程的脚本、JSON 或文档。
{extra}"""

REVIEW_PROMPT = """你是独立代码评审，只读。工作区 {root}。评审任务 {task_id} 的未提交改动：
运行 `git status --short` 与 `git diff HEAD`（新文件直接读）。任务内容：

{task}

只报告阻断级问题：编译或测试必然失败、崩溃、资源泄漏、数据错误、违反模块依赖方向或 ADR、
验收未满足、测试被删除或断言被削弱、新增了记录过程用的脚本/JSON/文档或源码目录里的临时文件。
风格、命名和“可以更健壮”不算。每个问题给出文件、行和触发方式。
最后一行必须是 `VERDICT: PASS` 或 `VERDICT: BLOCK`。"""


def next_task() -> tuple[str, str, str] | None:
    lines = (ROOT / 'TASK.md').read_text(encoding='utf-8').splitlines()
    for index, line in enumerate(lines):
        match = TASK_LINE.match(line)
        if match:
            body = [line]
            for follow in lines[index + 1:]:
                if not follow.startswith('  '):
                    break
                body.append(follow)
            return match[1], match[2], '\n'.join(body)
    return None


def run(command: list[str], log: Path, prompt: str | None = None) -> int:
    executable = shutil.which(command[0])
    if not executable:
        raise SystemExit(f'autorun: {command[0]} not found on PATH')
    with log.open('w', encoding='utf-8', errors='replace') as handle:
        result = subprocess.run([executable, *command[1:]], cwd=ROOT, input=prompt,
                                stdout=handle, stderr=subprocess.STDOUT, encoding='utf-8',
                                errors='replace')
    print(f'  [{"ok" if result.returncode == 0 else "exit " + str(result.returncode)}] '
          f'{command[0]} -> {log.relative_to(ROOT)}', flush=True)
    return result.returncode


def tail(path: Path, count: int = 60) -> str:
    return '\n'.join(path.read_text(encoding='utf-8', errors='replace').splitlines()[-count:])


def git(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run(['git', *args], cwd=ROOT, capture_output=True, encoding='utf-8',
                          errors='replace')


def format_changed() -> None:
    """Apply .clang-format to the C++ files this task touched, so style never needs a review."""
    tool = shutil.which('clang-format')
    if not tool:
        found = sorted(Path(p) for root in ('C:/Program Files', 'D:/Program Files')
                       for p in Path(root).glob(
                           'Microsoft Visual Studio/*/*/VC/Tools/Llvm/x64/bin/clang-format.exe'))
        tool = str(found[-1]) if found else None
    if not tool:
        print('  [skip] clang-format not found', flush=True)
        return
    changed = git('status', '--porcelain', '--untracked-files=all', '--', 'engine', 'tests',
                  'examples').stdout.splitlines()
    files = [line[3:].strip('"') for line in changed
             if line[:2].strip() != 'D' and line.endswith(('.h', '.cpp', '.h"', '.cpp"'))]
    if files:
        subprocess.run([tool, '-i', *files], cwd=ROOT, check=False)


def run_task(task_id: str, task: str, agent: str, tester: str, reviewer: str) -> str | None:
    """Returns None when the task was committed, otherwise the reason to stop."""
    out = ROOT / 'build' / 'temp' / 'autorun' / task_id
    out.mkdir(parents=True, exist_ok=True)
    msg = out / 'commit-msg.txt'
    msg.unlink(missing_ok=True)
    fields = {'root': ROOT, 'task': task, 'task_id': task_id, 'msg': msg}

    def develop_until_green(extra: str, label: str) -> str | None:
        for attempt in range(1, GATE_RETRIES + 1):
            run(AGENTS[agent]['write'], out / f'{label}-{attempt}.log',
                DEV_PROMPT.format(extra=extra, **fields))
            format_changed()
            gate_log = out / f'{label}-gate-{attempt}.log'
            if run(GATE, gate_log) == 0:
                return None
            extra = f'\n上一轮门禁失败，先修复它。日志末尾：\n{tail(gate_log)}\n'
        return f'gate still red after {GATE_RETRIES} attempts, see {out}'

    first = ''
    if tester != 'none':
        run(AGENTS[tester]['write'], out / 'test.log', TEST_PROMPT.format(**fields))
        first = TESTS_READY
    failure = develop_until_green(first, 'dev')
    if failure:
        return failure

    if reviewer != 'none':
        review_log = out / 'review.log'
        run(AGENTS[reviewer]['read'], review_log, REVIEW_PROMPT.format(**fields))
        review = review_log.read_text(encoding='utf-8', errors='replace')
        if 'VERDICT: PASS' not in review:
            failure = develop_until_green(
                f'\n评审提出了阻断问题，只修这些，不做其他改动：\n{tail(review_log, 80)}\n', 'fix')
            if failure:
                return failure

    done = next_task()
    if done and done[0] == task_id:
        return f'{task_id} is not ticked in TASK.md'
    if not msg.is_file():
        return f'missing commit message {msg}'
    git('add', '-A')
    commit = git('commit', '-F', str(msg))
    if commit.returncode != 0:
        return f'commit rejected:\n{commit.stdout}{commit.stderr}'
    print(f'  committed {git("log", "--oneline", "-1").stdout.strip()}', flush=True)
    shutil.rmtree(out, ignore_errors=True)  # logs only matter when a task stops
    return None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--agent', choices=sorted(AGENTS), default='codex')
    parser.add_argument('--tester', choices=[*sorted(AGENTS), 'none'], default='none',
                        help='write the acceptance tests first with a separate agent')
    parser.add_argument('--reviewer', choices=[*sorted(AGENTS), 'none'], default='none')
    parser.add_argument('--max-tasks', type=int, default=20)
    parser.add_argument('--dry-run', action='store_true', help='print the next task and exit')
    args = parser.parse_args()

    for _ in range(args.max_tasks):
        found = next_task()
        if not found:
            print('autorun: queue is empty')
            return 0
        task_id, title, task = found
        print(f'== {task_id} {title}', flush=True)
        if '[人工]' in title:
            print('autorun: this task needs the user; stopping')
            return 0
        if args.dry_run:
            print(task)
            return 0
        if git('status', '--porcelain').stdout.strip():
            print('autorun: working tree is not clean; commit or stash first', file=sys.stderr)
            return 1
        reason = run_task(task_id, task, args.agent, args.tester, args.reviewer)
        if reason:
            print(f'autorun: stopped at {task_id}: {reason}', file=sys.stderr)
            return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
