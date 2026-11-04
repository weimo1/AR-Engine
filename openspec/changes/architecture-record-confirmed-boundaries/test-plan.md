# 文档检查计划与证据入口

状态：A/B/C历史文档自检及交接已发生；2026-10-27修复后的检查按当前输入重新执行。下表是检查办法，不提前记录结果。历史72个链接等数字仅适用于旧输入，正式结果看本轮证据。

| 契约要求 | 实际检查办法 | 通过条件与限制 |
| --- | --- | --- |
| 来源与范围完整 | 对照总体架构A1—A6/B1—B4/C1—C5及本对话实际确认，核对proposal/design/tasks/context | 明确局部确认、后续确认及未决；无伪造时间/批准 |
| 上下文一致 | python scripts/workflow.py context architecture-record-confirmed-boundaries；核对实际documents | 总体架构、scene/render及当前任务材料实际展开；reference保留，无重复冲突 |
| 契约与派生 | python scripts/workflow.py derive architecture-record-confirmed-boundaries；python scripts/workflow.py check | specs只由proposal派生且内容一致，check实际通过；不能仅文件存在 |
| 确认与建议分层 | 将两轮用户转贴评审的四项原文逐条对应R1—R4，核对总体架构、模块、契约及派生spec | 四项均明确待用户确认；原A/B/C不变；SHALL只约束状态区分，不规定细化职责；runtime阶段措辞不声称自行规划pass依赖 |
| 真实状态 | 核对a4029495/9e864a/56562f各自handoff.json和本轮记录 | 1.4反映历史completed；本轮自检/ci/独立Review分别声明，不继承旧资格 |
| 路径与隔离 | 本轮开始/结束git ls-files输入SHA256对比；git diff --check及未跟踪Markdown单检 | 变化仅允许文件，P0/代码/工具/ADR无变化，既有修改保留 |
| 链接与结构 | 相对文件链接、主源锚点、Markdown围栏/空白和JSON解析 | 链接存在，锚点匹配，结构有效；Mermaid未使用渲染器则明确未验证渲染 |
| 冻结后验证 | 由统筹在handoff后冻结输入，安排check/ci及OpenSpec严格校验 | 保存输入哈希及日志，核对混合工作区脚本来源；旧b516 CI只覆盖历史输入，不能归因于本次或放行当前输入 |

## 证据位置与执行顺序

本轮任务为bbd7ec0ded624ece9b2327ae；起止快照、原始消息/评审、逐项回应和自检记录保存于`.ar-workflow/architecture-revision-20261027/`。旧`.ar-workflow/architecture-repair-20261027/`及b516结果仅为历史，不能批准本轮输入。

先完成写入与适用自检→agent-handoff→统筹冻结输入→安排适用验证与独立Review/gate。正式输入须包含实际依赖的工作流脚本及来源，既不回滚其他变更也不把混合工作区通过单独归因于本change。没有构建/GPU/性能测试或P0修复结论；Mermaid本轮未改图，不声称运行渲染器。
