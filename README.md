# AR Engine

个人实时渲染引擎。C++23、Vulkan 1.4，Windows 使用 VS 2026 / MSVC；先把渲染、资源绑定与调试做好，再逐步补齐编辑器。

仓库：[weimo1/AR-Engine](https://github.com/weimo1/AR-Engine)。本地 origin：`git@github.com:weimo1/AR-Engine.git`。

当前处于文档与工作流基础建设阶段，尚无自有渲染源码。需求与架构建议的确认范围见 [文档索引](docs/README.md)，已选依赖见 [ADR-0007](docs/adr/0007-基础构建与第三方依赖选型.md)。

新版 SDK 已安装；暂保留现有显卡驱动，先做标明能力范围的基础探针。产品仍以 Vulkan 1.4 为目标，设备支持与 Heap 另行验收，见 [当前环境](docs/workflow/当前决策与待办清单.md#开发环境与阶段边界)。另一对话继续完善工作流时，先读 [交接清单](docs/workflow/AI工作流后续完善交接-2026-10-20.md)。

## 从这里开始

- 看做什么：[需求](渲染引擎需求.txt)。
- 看怎么组织：[总体架构与技术路线](docs/总体架构与技术路线.md)。
- 开始开发：[SDD 与 Code Review 接入指南](docs/workflow/SDD与CodeReview接入指南.md)。
- AI 任务入口：[AGENTS.md](AGENTS.md)。

```powershell
# 首次恢复本地工具；依赖只安装到项目目录
powershell -ExecutionPolicy Bypass -File scripts/setup-workflow.ps1

# 检查文档、派生契约和工具
python scripts/workflow.py check
python scripts/workflow.py doctor
python scripts/workflow.py install-hooks

# 完成方案后，按任务执行；review 会调用已配置的 DeepSeek API
python scripts/workflow.py start <change-id>
python scripts/workflow.py verify <change-id>
python scripts/workflow.py review <change-id> --budget 100000
python scripts/workflow.py gate <change-id>
# 换会话或中断后：python scripts/workflow.py resume <change-id>

# 查看变更
npm.cmd run sdd -- list
```

`package.json` 仅管理开发流程工具。引擎依赖仍使用已选定的 vcpkg，构建由 CMake / MSBuild 负责。

旧构想、工作流长讨论、普通模板和他人参考统一放在 [历史归档](docs/archive/README.md)；调研资料放在 `docs/research/`。日常开发使用当前文档索引及接入指南。
