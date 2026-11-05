# 项目总任务

本页记录项目整体进度。统筹从这里看当前阶段、下一步和对应的 SDD 文档。阶段或下一步变化时更新本页，实施细项和实际结果记在对应变更的 `tasks.md`。

全项目各阶段的先后顺序见 [实施路线与阶段计划](docs/实施路线与阶段计划.md)。统筹按路线和任务依赖确定本批模块与任务编号，并明确目标、方案依据和验收条件。开发从本页接单，按对应任务实施和自检，把结果写回任务清单；统筹再据此更新本页和安排下一批。

| 阶段 | 统筹判断 |
| --- | --- |
| 选任务、核对方案 | 统筹按路线和依赖选出本批任务，核对已确认方案是否说清模块与接口边界、关键行为、失败处理和验收。方案足够就交开发。缺关键设计或要改变已确认的取舍时，把具体缺口交给架构，补到对应 SDD 文档，经用户确认受影响的决定后再开发。 |
| 开发 | 接到本批任务后，运行 `python scripts/workflow.py context <change-id>`，查看 `skills`，打开适用技能的 `.agents/skills/<名称>/SKILL.md` 再实施。若本批涉及外部 API、性能或集成测试等具体场景，再按技能说明选用相关技能。 |
| 写测试、编译和自检 | 开发按 `test-plan.md` 同步新增或修改受影响的测试。修缺陷时优先补能复现问题的用例。用仓库统一入口构建并跑测试，记录命令、结果和未覆盖项。失败项回开发处理。 |
| 代码评审 | 自检完成后，对本批暂存差异运行评审准备脚本。它调用 OCR 筛选并打印交接提示，由用户把结果带到现有评审对话。开发修复后重跑受影响的检查，再交回更新后的差异。 |
| 交付 | 本批验收满足后，按用户授权提交或推送，并记录实际 CI 结果。再判断下一项。 |

方案有关键争议时，架构维护同一份 `design.md`，另一位 Agent 提出具体反例和备选方案，由架构处理并把仍需取舍的事项交用户决定。方案明确后转开发。方案质询按需安排。

## 当前任务

- 当前阶段为 **P0-code 开发**，正在实现 P0 基础工程。
- 当前开发任务为 **2.1 runtime 组合**，把 core 诊断与 rhi 查询串成公共入口，并跑对应检查。
- **2.3 RHI 已于 2026-11-06 完成**：隔离标志与 quarantine-reopen、`report()` 关闭后失效的计数验证、`Impl::closed` 缺失修复，两配置各 30/30。

## 本批交付记录（2026-11-06）

- 工作流切片 `b87938b`、RHI 2.3 收口切片 `5c85ef8` 已提交并推送到 origin/main；RHI 的 18 个已评审文件与提交 blob 逐一核对一致。
- `5c85ef8` 的 [GitHub Workflow checks #42070320194](https://github.com/weimo1/AR-Engine/actions/runs/42070320194) 已完成，workflow-checks 作业 success。本地工作流 95 项测试通过；RHI 沿用同一代码输入的 16/16 命令、两配置各 30/30 证据。
- P0 CPU 工作流仍为手动触发，本轮未触发；GPU 未运行。3.3 与整体交付 3.4 仍未完成，不将工作流 CI 通过视为引擎云端构建通过。
- 下一步交开发执行 2.1 runtime 组合，同步补齐 3.1；按现有方案核对接口与失败处理后实施、自检，再交独立评审。

## 开发检查命令

以下以刚才的 RHI 切片为例。测试源码路径按本批任务替换，多份文件可重复传 `--test-file`。

```powershell
python scripts/build_checks.py --phase build
python scripts/build_checks.py --phase test --test-file tests/rhi/test_rhi_session.cpp
python scripts/prepare_review.py
```

编译模式执行命令表中的构建及其依赖。测试模式先检查指定源码文件存在，再按同一命令表构建、跑 CTest 并核对必跑用例。测试模式包含编译，开发可直接运行它完成整批自检。评审准备在本批差异暂存后运行，使用 OCR 整理输入并打印评审提示。

## P0 的 SDD 文档

统筹可在仓库根运行 `python scripts/workflow.py context bootstrap-engine-p0`，查看该变更配置的架构、模块方案和 SDD 文档。按本批模块打开对应文档，再核对方案是否覆盖任务和验收条件。脚本列出依据，方案是否足够由统筹判断。

| 文档 | 记录内容 |
| --- | --- |
| [proposal.md](openspec/changes/bootstrap-engine-p0/proposal.md) | 范围、行为和验收条件 |
| [design.md](openspec/changes/bootstrap-engine-p0/design.md) | 实现方案和模块边界 |
| [test-plan.md](openspec/changes/bootstrap-engine-p0/test-plan.md) | 验证方法和测试覆盖 |
| [tasks.md](openspec/changes/bootstrap-engine-p0/tasks.md) | P0 实施细项、进度和实际结果 |
