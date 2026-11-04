# 技术方案

## 授权与依据

依据本 change proposal 及现有多 Agent 协议，用户已授权本次实现。沿用现有 task/agents/原始验证审查记录，不增加第二个可编辑状态数据库。

## 主责与模块关联

workflow，关联 CLI、角色包与运行资格检查；不涉及引擎模块或 P0 关键选择。

## 实现与取舍

- 新模块 workflow_relay 生成摘要；`relay <change> --format json|markdown` 在现有 operation_lock 内运行，只读源和状态，不调用 task_state 自动写回。
- 新 handoff 增加 evidence_receipts，coordination 记录 handoff_sha256；历史格式保持原样，摘要标记 legacy。摘要不把本地哈希解释为认证。
- 验证日志核对提取为既有 gate 与 relay 共用函数；relay 额外核对当前输入/packet，Review/gate 继续调用原 task_gate，无新增放行路径。
- agent-pack 自动嵌入摘要；`--from-handoff` 显式接手要求同 change、当前 completed 新记录及摘要完整；旧格式只能作为历史线索，由统筹核对后分配新包，不能声称封存核验通过。
- 全局 writer 优先阻断后续验证与交付建议。其他角色失败、输入变更、记录缺失分别解释，不以时间过去自动释放。
- 摘要中的 summary/next_action 为角色陈述，证据本身不自动解析为成功。阶段/下一角色是派生导航，不修改 runtime.phase。

## 任务上下文与协作

context 声明工作流源码、回归、指南和本变更路径。当前单 Agent 实现，使用 developer 写包并 handoff 后执行共享检查；不作为自己的独立审查者。准备新包所需 planning/context 在申请 writer 前完成，不触碰已有包或清理其他任务。

## 验证与恢复

覆盖正常交接、历史格式、跨 change、输入变化、包/证据/交接篡改、记录缺失、全局 writer、task 旧 phase 及验证日志改变。命令零模型调用，relay 不改变 task/agents 内容；显式接手失败不分配 writer。保留现有回归，失败时修复当前实现，不放宽 gate。
