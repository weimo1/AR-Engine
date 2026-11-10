"""Drive the TASK.md queue unattended: (tests) -> implement -> gate -> review -> commit -> next.

Each task runs in a fresh headless agent process (Codex or Claude Code), so no conversation
context accumulates. State lives only in TASK.md and Git. The gate is run by this script, not
trusted from the agent's report. Stops at a task marked [人工], on a red gate after the retry
budget, on a blocking review that survives one fix round, or when the queue is empty.

Every role takes a comma-separated fallback list. When an agent exits because its usage limit is
reached, the next one in the list takes over the same task from the working tree as it stands.
If every agent is out, the run stops with the tree left as is; continue later with --resume.

    python scripts/autorun.py --agent claude,codex --tester claude,codex --reviewer codex,claude
    python scripts/autorun.py --agent codex --resume
    python scripts/autorun.py --dry-run
"""
from __future__ import annotations

import argparse
import os
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
# --ephemeral keeps Codex from leaving a session file under ~/.codex/sessions for every task.
CODEX = ['codex', 'exec', '--ephemeral', '-m', 'gpt-6.1-sol', '-c', 'model_reasoning_effort=medium']
AGENTS = {
    'codex': {
        # The Windows sandbox blocks vcpkg and MSBuild's SDK lookup, so a sandboxed Codex can
        # edit but cannot build. --trust switches the writing roles to 'trusted'.
        'write': [*CODEX, '--sandbox', 'workspace-write', '-'],
        'trusted': [*CODEX, '--dangerously-bypass-approvals-and-sandbox', '-'],
        'read': [*CODEX, '--sandbox', 'read-only', '-'],
    },
    'claude': {
        'write': ['claude', '-p', '--permission-mode', 'acceptEdits',
                  '--allowedTools', 'Bash,Read,Edit,Write,Glob,Grep'],
        'trusted': ['claude', '-p', '--dangerously-skip-permissions'],
        'read': ['claude', '-p', '--allowedTools', 'Read,Glob,Grep,Bash(git diff:*),Bash(git log:*)'],
    },
}

# An agent counts as out of quota when it exits non-zero and the end of its log matches this.
# Extend it with the exact wording when a CLI reports its limit differently.
LIMIT_HINT = re.compile(
    r'usage limit|rate limit|limit reached|hit your .{0,30}limit|quota|too many requests'
    r'|\b429\b|credit balance|额度|限额', re.IGNORECASE)
API_KEY_VARS = ('ANTHROPIC_API_KEY', 'OPENAI_API_KEY', 'CODEX_API_KEY')
OUT_OF_QUOTA ='every listed agent is out of quota; rerun with --resume once one is available'

TAKEOVER = """
工作区里可能有上一位 Agent 未完成的改动。先用 `git status --short` 和 `git diff HEAD` 了解进度
（新文件直接读），在此基础上继续，不要推倒重来；已有内容确实有错再改。
"""

TEST_PROMPT = """你是 AR Engine 的测试工程师，工作区 {root}。先读 AGENTS.md 并遵守。
只为 TASK.md 里的这一项任务写验收测试，不实现功能：

{task}

要求：
1. 按“验收”在 tests/ 下写 Catch2 用例，用例名与任务里的 CTest 名一致，并登记到
   tests/CMakeLists.txt 和 workflow/p0-checks.json 的 expected_cases。
2. 断言写可观察行为，覆盖主路径和真实会遇到的失败，不堆穷举矩阵。
3. 不修改 engine/ 下的实现；需要的公开接口按任务目标和相邻模块的约定假定，写在测试里。
4. 此时编译失败是预期的，不要运行门禁，不要提交。
{takeover}"""

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
{extra}{takeover}"""

REVIEW_PROMPT = """你是独立代码评审，只读。工作区 {root}。评审任务 {task_id} 的未提交改动：
运行 `git status --short` 与 `git diff HEAD`（新文件直接读）。任务内容：

{task}

只报告阻断级问题：编译或测试必然失败、崩溃、资源泄漏、数据错误、违反模块依赖方向或 ADR、
验收未满足、测试被删除或断言被削弱、新增了记录过程用的脚本/JSON/文档或源码目录里的临时文件。
风格、命名和“可以更健壮”不算。每个问题给出文件、行和触发方式。
最后一行必须是 `VERDICT: PASS` 或 `VERDICT: BLOCK`。"""


VERDICT_LINE = re.compile(r'^\s*`?VERDICT: (PASS|BLOCK)`?\s*$', re.MULTILINE)


def next_task(committed: bool = False) -> tuple[str, str, str] | None:
    """First unticked task. With committed=True it reads TASK.md as of HEAD: an interrupted task
    may already be ticked in the working tree, and resuming must still pick that one."""
    if committed:
        lines = git('show', 'HEAD:TASK.md').stdout.splitlines()
    else:
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


def subscription_env() -> dict[str, str]:
    """Agents run on the subscription login only. With a pay-per-token key in the environment a
    CLI would keep working past the plan's 5-hour and weekly limits and bill the key instead."""
    env = {key: value for key, value in os.environ.items() if key not in API_KEY_VARS}
    # Claude Code on Windows needs to be told where Git Bash is when started from a plain shell.
    git_exe = shutil.which('git')
    if os.name == 'nt' and git_exe and 'CLAUDE_CODE_GIT_BASH_PATH' not in env:
        for folder in Path(git_exe).resolve().parents[1:3]:  # Git/cmd or Git/mingw64/bin
            if (folder / 'bin' / 'bash.exe').is_file():
                env['CLAUDE_CODE_GIT_BASH_PATH'] = str(folder / 'bin' / 'bash.exe')
                break
    return env


def run(command: list[str], log: Path, prompt: str | None = None,
        env: dict[str, str] | None = None) -> int:
    executable = shutil.which(command[0])
    if not executable:
        raise SystemExit(f'autorun: {command[0]} not found on PATH')
    with log.open('w', encoding='utf-8', errors='replace') as handle:
        result = subprocess.run([executable, *command[1:]], cwd=ROOT, input=prompt, env=env,
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


trust = False  # set by --trust: writing roles run without their sandbox
exhausted: set[str] = set()  # agents that hit their limit; skipped for the rest of this run


def run_agent(names: list[str], mode: str, log: Path, template: str, fields: dict) -> str | None:
    """Runs the first listed agent that still has quota. Returns its name, or None when all are
    out. A write agent that takes over is told to continue from the tree instead of restarting."""
    for name in names:
        if name in exhausted:
            continue
        agent_log = log.with_name(f'{log.stem}-{name}.log')
        command = AGENTS[name]['trusted' if trust and mode == 'write' else mode]
        code = run(command, agent_log, template.format(**fields), subscription_env())
        said = next((line.strip() for line in reversed(tail(agent_log, 15).splitlines())
                     if LIMIT_HINT.search(line)), None)
        if code == 0 or not said:
            return name
        exhausted.add(name)
        # The CLI's own line says which limit (5-hour or weekly) and when it resets.
        print(f'  {name} is out of quota: {said[:200]}', flush=True)
        if mode == 'write':
            fields['takeover'] = TAKEOVER
    return None


def run_task(task_id: str, task: str, agents: list[str], testers: list[str],
             reviewers: list[str], resumed: bool) -> str | None:
    """Returns None when the task was committed, otherwise the reason to stop."""
    out = ROOT / 'build' / 'temp' / 'autorun' / task_id
    out.mkdir(parents=True, exist_ok=True)
    msg = out / 'commit-msg.txt'
    if not resumed:
        msg.unlink(missing_ok=True)
    fields = {'root': ROOT, 'task': task, 'task_id': task_id, 'msg': msg, 'extra': '',
              'takeover': TAKEOVER if resumed else ''}

    def develop_until_green(extra: str, label: str) -> str | None:
        for attempt in range(1, GATE_RETRIES + 1):
            fields['extra'] = extra
            if not run_agent(agents, 'write', out / f'{label}-{attempt}.log', DEV_PROMPT, fields):
                return OUT_OF_QUOTA
            format_changed()
            gate_log = out / f'{label}-gate-{attempt}.log'
            if run(GATE, gate_log) == 0:
                return None
            extra = f'\n上一轮门禁失败，先修复它。日志末尾：\n{tail(gate_log)}\n'
        return f'gate still red after {GATE_RETRIES} attempts, see {out}'

    first = ''
    if testers:
        if not run_agent(testers, 'write', out / 'test.log', TEST_PROMPT, fields):
            return OUT_OF_QUOTA
        first = TESTS_READY
    failure = develop_until_green(first, 'dev')
    if failure:
        return failure

    if reviewers:
        name = run_agent(reviewers, 'read', out / 'review.log', REVIEW_PROMPT, fields)
        if not name:
            return OUT_OF_QUOTA
        review_log = out / f'review-{name}.log'
        review = review_log.read_text(encoding='utf-8', errors='replace')
        # The last verdict line decides. A PASS quoted earlier in the text (the prompt echoed
        # back, or a sentence about it) must not outvote a closing BLOCK.
        verdicts = VERDICT_LINE.findall(review)
        if not verdicts:
            return f'review ended without a verdict, see {review_log}'
        if verdicts[-1] != 'PASS':
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


def agent_list(text: str) -> list[str]:
    names = [] if text == 'none' else [name.strip() for name in text.split(',')]
    for name in names:
        if name not in AGENTS:
            raise argparse.ArgumentTypeError(f'unknown agent {name!r}; choose from {sorted(AGENTS)}')
    return names


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--agent', type=agent_list, default=['codex'],
                        help='developer, in fallback order, e.g. claude,codex')
    parser.add_argument('--tester', type=agent_list, default=[],
                        help='write the acceptance tests first with a separate agent; '
                             'fallback list or none')
    parser.add_argument('--reviewer', type=agent_list, default=[], help='fallback list or none')
    parser.add_argument('--resume', action='store_true',
                        help='continue the first task from an unclean working tree')
    parser.add_argument('--trust', action='store_true',
                        help='run the writing roles without their sandbox so they can build')
    parser.add_argument('--max-tasks', type=int, default=20)
    parser.add_argument('--dry-run', action='store_true', help='print the next task and exit')
    args = parser.parse_args()

    if not args.agent:
        parser.error('--agent needs at least one agent')
    global trust
    trust = args.trust
    resumed = args.resume
    for _ in range(args.max_tasks):
        found = next_task(committed=resumed)
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
        if not resumed and git('status', '--porcelain').stdout.strip():
            print('autorun: working tree is not clean; commit or stash first, or pass --resume '
                  'to continue the interrupted task', file=sys.stderr)
            return 1
        reason = run_task(task_id, task, args.agent, args.tester, args.reviewer, resumed)
        resumed = False
        if reason:
            print(f'autorun: stopped at {task_id}: {reason}', file=sys.stderr)
            return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
