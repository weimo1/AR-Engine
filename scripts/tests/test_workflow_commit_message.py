"""Message rules and real commit-msg rejection in an owned temporary repository."""
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).parents[1]))
import workflow_commit_message as message


BODY = '\n\nWhy: 修复交接状态不一致\nWhat: 增加恢复检查\nTests: 已运行专项回归\nRefs: connect-dsh-runtime\n'


class CommitMessageTests(unittest.TestCase):
    def test_each_type_and_breaking_change(self):
        for kind in message.TYPES:
            self.assertEqual(message.validate(f'{kind}(workflow): 修正任务交接状态' + BODY), [])
        self.assertTrue(message.validate('feat(rhi)!: 调整资源接口契约' + BODY))
        self.assertEqual(message.validate('feat(rhi)!: 调整资源接口契约' + BODY +
                                         'BREAKING CHANGE: 调用方必须传入资源句柄\n'), [])

    def test_missing_prefix_scope_and_vague_titles_rejected(self):
        for title in ('修改内容', 'dev(core): 添加资源句柄类型', 'other(core): 更新项目相关文件',
                      'fix: 修正资源生命周期', 'Fix(rhi): 修正资源生命周期',
                      'fix(rhi): 修复问题', 'fix(rhi): 修正资源生命周期 ', 'fix(rhi): ' + 'x' * 73):
            with self.subTest(title=title):
                self.assertTrue(message.validate(title + BODY))

    def test_required_fields_placeholders_comments_and_controls(self):
        valid = 'fix(workflow): 修正任务交接状态' + BODY
        for field in message.FIELDS:
            broken = '\n'.join(line for line in valid.splitlines() if not line.startswith(field + ':'))
            self.assertTrue(message.validate(broken))
        self.assertTrue(message.validate(valid + 'Tests: 重复字段\n'))
        self.assertTrue(message.validate(valid.replace('Tests: 已运行专项回归', 'Tests: TODO')))
        self.assertTrue(message.validate(valid.replace('Tests:', '# Tests:')))
        self.assertTrue(message.validate(valid + '\x00'))
        self.assertEqual(message.validate(valid.replace('Tests: 已运行专项回归', 'Tests: 未运行（仅文档，说明原因）')), [])

    def test_real_git_hook_blocks_bad_commit_and_accepts_valid_message(self):
        source = Path(__file__).resolve().parents[2]
        with tempfile.TemporaryDirectory(prefix='ar-commit-message-') as temporary:
            root = Path(temporary).resolve()
            (root / 'empty-template').mkdir()
            env = {key: value for key, value in os.environ.items() if not key.upper().startswith('GIT_')}
            env.update(GIT_CONFIG_NOSYSTEM='1', GIT_CONFIG_GLOBAL=os.devnull, GIT_TERMINAL_PROMPT='0')
            def git(*args, check=True):
                return subprocess.run(['git', *args], cwd=root, env=env, capture_output=True,
                                      text=True, encoding='utf-8', timeout=30, check=check)
            git('init', '--quiet', '--initial-branch=main', '--template=' + str(root / 'empty-template'))
            git('config', '--local', 'user.name', 'Owned workflow test')
            git('config', '--local', 'user.email', 'fixture@example.invalid')
            git('config', '--local', 'commit.gpgsign', 'false')
            git('config', '--local', 'core.hooksPath', '.githooks')
            for name in ('.githooks/commit-msg', 'scripts/workflow_commit_message.py'):
                destination = root / name
                destination.parent.mkdir(parents=True, exist_ok=True)
                shutil.copyfile(source / name, destination)
                git('add', '--', name)
            git('update-index', '--chmod=+x', '.githooks/commit-msg')
            bad = git('commit', '-m', '无前缀的提交', check=False)
            self.assertNotEqual(bad.returncode, 0)
            self.assertIn('COMMIT MESSAGE BLOCKED', bad.stderr)
            self.assertNotEqual(git('rev-parse', '--verify', 'HEAD', check=False).returncode, 0)
            good = git('commit', '-m', 'fix(workflow): 修正任务交接状态' + BODY)
            self.assertEqual(good.returncode, 0)
            self.assertEqual(git('log', '-1', '--format=%s').stdout.strip(), 'fix(workflow): 修正任务交接状态')


if __name__ == '__main__':
    unittest.main()
