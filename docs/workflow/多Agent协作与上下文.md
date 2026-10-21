# 多 Agent 协作与任务上下文

2026-10-21。依据：用户确认统筹及独立角色上下文后要求实施。协议适用于工作流协作，不批准总体引擎架构草案；角色包和本地哈希不是身份认证。

## 共享依据与各自任务包

统筹从 AGENTS、当前任务 proposal/design/test-plan/tasks、适用需求、架构、ADR、规范和 Skills 准备上下文。公共正文只维护一份；任务包带路径、摘要、声明状态和实际内容。`context.json` 记录 objective/non_goals、tags、documents、skills、write_paths。尚未开始写 C++/Shader 时，使用 native 标签、显式 ar-cpp-oop-style 技能或具体原生允许写入路径即可提前加载原生依据；不能只等待 diff 出现。文档的 status/basis 是追溯声明，不能代替用户批准。架构与图示草案保持 reference，不因传给模型就升级为强制要求。

角色有 coordinator、architect、developer、verifier、reviewer、adjudicator，定义在 workflow/agent-policy.json。按需分配，不要求每个任务启动全部角色。开发者的聊天推理不进入 Review 请求；裁决才接收明确标记的反证。独立性来自实际新上下文和证据流程，角色名称本身不能证明独立。

## 单工作区执行

允许并行只读分析。全工作区同时只有一个协议 writer；写任务领取至交接之间不得让另一 Agent 编辑源码、执行 Git 修改或针对变化输入验证。Git 由写任务交接后的统筹串行执行。编辑器和直接 Git 不受该记录的 OS 隔离，偏离只能被检测，不能声称自动阻止所有工具外写入。

只有 coordinator/architect/developer 可领写包，且允许路径必须属于本 change 的 write_paths。Review、验证和裁决默认只读；测试输出可写本任务忽略目录，但源码修复应转回开发写包。任务包不给付费权限、不启动模型或聊天。未分配角色的单 Agent 常规任务继续使用既有入口。

## 命令及交接

从项目根执行：

```powershell
python scripts/workflow.py context connect-agent-task-context
python scripts/workflow.py agent-pack connect-agent-task-context --role reviewer --agent review-1 --access read --goal "检查冻结依据及任务包权限边界"
python scripts/workflow.py agent-status
```

agent-pack 输出本机 `.ar-workflow/agents/<id>/task.md` 与 assignment.json；统筹把 task.md 交给实际子 Agent/Codex/Cline 对话。工具不自动启动它们，不假定这些客户端有相同权限。`--access write --paths <已声明范围>` 可领取写包；拒绝全工作区第二 writer。

完成后准备结果 JSON：status 为 completed/blocked，summary、next_action 为文字，evidence 为可定位证据路径列表。执行 `agent-handoff <id> --result <json>`；只读期间源变化记录 stale，写入越界记录 blocked，均不能冒充当前验收通过。异常任务执行 `agent-release <id> --reason <具体原因>`，保存起止状态，不自动回滚、不获得完成资格；不因时间过去自动抢占。

结果示例（保存于本任务 `.ar-workflow/` 目录）：

```json
{"status":"completed","summary":"实际完成的工作及限制","evidence":[".ar-workflow/ci/checks.json"],"next_action":"统筹核对证据后继续"}
```

异常释放遇到不可读输入时保存 end_input_unavailable；未知状态不能标为完成。任务包被修改则拒绝正常交接，先核对原始包，不自动用损坏记录授予权限。

交接只记录事实。verify、review、finding-* 和 gate 仍是验证与独立审查资格来源，任何角色手写 pass 不能替代它们。统计由统筹在完成/交接/停止前离线更新，子 Agent 不并发写统计。

## 独立审查与缓存

任务级 verify/review 显式绑定 change，从冻结快照读取本 change 的依据和技能；不同 change 不复用同一身份。API 请求不写死旧任务名。改动依据或技能会使相关请求缓存失效；角色 ID、时间戳及交接历史不进入审查语义缓存键。无 change 的通用 hook/远程路径明确只做源码通用检查，不声称完成任务级需求审查。

目前同一任务的所有单元共享该任务规划依据；修改 tasks/test-plan 等规划文件会使该任务的请求整体失效。跨任务或更细粒度的语义切片尚未实现，不宣称只更新勾选不会影响缓存。阶段检查结果优先保留于本机执行记录，避免把运行日志反复复制到规划正文。

正式审查前先 review-plan；不因角色分配自动调用模型，也不按任务包里的预算声明擅自追加额度。当前协议不接管 PR、分支保护或常驻调度器；实际结果见当前状态。
