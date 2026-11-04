# 多 Agent 协作与任务上下文

2026-10-21。依据：用户确认统筹及独立角色上下文后要求实施。协议适用于工作流协作，不批准总体引擎架构草案；角色包和本地哈希不是身份认证。

## 先开哪些对话

可复制的公共开场及各角色提示词见 [对话启动提示词](多Agent对话启动提示词.md)。2026-11-03 用户决定自行安排单人写入：直接交办后按明确范围执行，按影响检查并报告结果；无需写包、writer 权限登记、领取/释放或流程 gate。独立 Review 按任务需要及用户安排执行，可直接读取明确版本的方案与差异。下文 SDK/结构化记录命令仅为可选工具说明，不是手工工作的前置。

现在建议只保留两个长期入口：一个统筹主对话（沿用你计划的ChatGPT），一个独立ChatGPT架构设计对话。开发、测试和独立审查按实际任务启用；不用预先为每个模块开一排空对话。这里是使用建议，不自动创建聊天、启动Agent或授权付费调用。

| 实际工作 | 建议使用 | 职责与交付 |
| --- | --- | --- |
| 统筹 | 用户主要沟通的一个ChatGPT主对话 | 明确目标和批准范围、维护依据、汇总实际结果；用户安排执行者，统筹不另设 writer 权限或 gate 审批 |
| 架构设计 | 独立ChatGPT对话 | 按模块准备方案、依赖/时序/所有权图、取舍与待确认点；不直接扩成实现 |
| 设计评审 | 按任务启用的独立DSH或Codex上下文，角色为reviewer | 读取同一版方案和需求，提出具体问题与替代建议；设计者回应后由用户决定，不为了对抗无限往返 |
| 实现 | 用户直接指定的 developer；DSH 自动调用可用内部快照 | 按已确认方案实现、运行开发自检，提交实际改动与未完成项；所有DeepSeek调用沿用Flash/High限制 |
| 验证 | 独立Codex任务，verifier只读源码 | 执行适用测试、检查验收映射和失败路径；写任务拥有的输出，不顺手修源码 |
| 代码审查 | 明确版本的差异交独立上下文或已授权 API，reviewer | 审实际差异、调用方和当前依据，保留原始意见及覆盖；OCR/结构化导入可选，不能用实现者总结代替 |

存在有依据的意见争议时才启用adjudicator，不要求每次都开一个裁决对话。统筹可以承担普通实现工作，但做过实现的上下文不充当该输入的独立审查者。

使用ChatGPT/Codex/DSH是对话和工具安排，不是权限隔离。某个对话无法直接读取本地项目时，统筹提供对应任务包正文和版本一致的依据；不能假定它能访问F盘、看到其他对话或自动读完全部规则。

## 哪些MD共享，哪些属于单个任务

| 材料 | 用途 | 维护方式 |
| --- | --- | --- |
| README.md | 给人介绍项目、当前阶段和入口 | 一份，不写成Agent操作提示词 |
| AGENTS.md | 所有项目Agent共享的任务协议与规则路由 | 一份公共入口，具体规范引用原文 |
| 需求、ADR、架构、工程规范 | 决定做什么以及已确认的边界 | 保持公共来源；各角色按任务加载 |
| workflow/agent-policy.json | 现有工具的角色职责/权限与上下文策略 | 当前机器定义源，不另建一套冲突角色规则 |
| .agents/skills/*/SKILL.md | 可复用的任务方法和专业指导 | 根据任务选择，不把每个技能当长期Agent |
| .ar-workflow/agents/<id>/task.md | 某个角色这一次的目标、依据、责任与输入 | agent-pack生成，每次任务单独分配；assignment.json记录结构化信息 |
| handoff.json及测试/审查证据 | 实际做了什么、当前输入与结果 | 交接保存，不靠聊天记忆恢复 |

手工任务不强制另建自己的MD；直接指令、现有方案和实际结果足够追溯时即可推进。自动化快照按需保存自己的MD，也不需要给每个聊天复制一整份AGENTS。模块内以后确有不同工程约束，再建立对应模块AGENTS.md；这类文件约束目录中的工作，不代表某位Agent的个人身份。外部agent-skills/AGENTS.md只借鉴组织方式，不把其工具偏好或审批策略自动搬成本项目规则。

## 从方案到交付怎么交接

1. 核对本次目标、已有实施授权和适用方案。已有已确认且足够实施的方案时直接进入开发，不固定重新找架构。
2. 缺方案或需要改变关键契约时，才由架构准备具体差异，按需独立方案评审，由用户确认关键取舍；普通局部实现不反复申请。
3. 用户直接安排执行者按明确方案实现切片并同步测试，不申请 developer 包或 writer 权限。
4. 按影响验证；本批已安排的独立 reviewer 读取同一明确版本的输入，普通实现/测试问题交开发修复，关键方案问题才交架构，不需要自动化 verify 或 OCR 包前置。
5. 切片收口后按已有授权提交本批差异，报告实际结果、提交身份和未完成项；不等整个P0/change完成，不夹带其他工作。推送和CI另按实际授权及结果记录，无流程 gate 前置。
6. 判断下一项方案是否足够实施：已确认且有授权就继续开发，缺方案才找架构，不按固定角色轮转；切片提交不等于能力完成或specs可归档。失败或中断说明文件、进程、证据和下一步，无需额外交接回执。

新对话的首条任务应明确角色、当前 change、具体目标与范围；采用自动化快照时再提供 task.md 路径，例如：

```text
你负责本次架构设计。先读项目 AGENTS.md、用户本次直接指令及适用的当前方案。
按本次指令的目标、依据和允许范围工作；输出模块关系、关键时序、取舍和待确认点。
没有批准的关键方案保持待确认。完成后给出实际产物、证据、剩余问题和交接对象。
```

角色名称不能替代具体目标和确认依据，但缺少 task.md 不阻止已获直接授权的修改。P0 等任务的实际确认状态读取当前方案与状态页，不沿用启动模板中的旧结论；方案确认和代码实施授权分别核对。

## 共享依据与各自任务包

### 小首页与完整冻结依据（2026-10-29）

新生成包使用 ar.agent-package/v2。task.md 只放本次目标、非目标、权限、输入、必读顺序及交接；package-manifest.json 列出全部选中依据、用途/确认范围、哈希、字节和大贡献来源，basis/ 按原始字节保存正文，relay.json 是分配时快照。基础执行规则、当前任务契约及适用技能保持必读，参考按目标读取；存在当前状态页时一并冻结。冻结清单表示可用和可核验，不表示模型已读完或全文已获批准。

inspect、relay、DSH准入和独立Review导入共用完整包检查；缺页/篡改会失败，不能只检查小首页。旧包不改写、继续支持原格式。DSH实际prompt只发送首页及包目录，不自动拼回全部正文；模型仍须按必读入口读取冻结文件，无法读取时停止受影响执行。字节减少不等于已测实际token节省。

Agent的原生依据按本change允许路径内的改动及显式标签/技能选择，保留强制规则；正式Review仍按真实目标、删除和调用方路由，不借包精简缩小审查范围。总体/模块/变更仍按现有文档分层维护，本轮不搬迁架构。

### 本地分发材料和领取

```powershell
python scripts/workflow.py agent-transfer <change-id> --assignment <assignment-id>
python scripts/workflow.py agent-accept <assignment-id> --agent <包中指定agent>
```

transfer没有assignment时报告未分配或待选择，带assignment时生成可复制说明、task路径、完整冻结依据的离线清单及包体积统计。不能读本地文件的对话须由统筹提供清单中的冻结文件；仅发F盘路径不能视为已经接收依据。依据包不等于完整源码或执行环境，离线实现/审查还须补齐相关源码或固定Review包；无法访问时不能假装执行。

生成不等于发送。没有发送回执时sent为not_recorded，不自动建聊天、发消息、调用模型或Git。accept核对活动包、指定agent及当前输入，记录本地工具领取回执；它不认证远程身份、不证明全文阅读、不授予Review/gate。领取过期、包损坏或agent不符则拒绝，不改变writer归属。

### 工具自有临时材料

```powershell
# 仅为当前活动角色创建明确的临时正文，不用于原始usage/验证证据
python scripts/workflow.py temp-create <assignment-id> --text "临时说明"
# 任务已completed且全局writer交接后，默认只读预览
python scripts/workflow.py temp-preview <assignment-id>
# 计划须保存到指定忽略目录，然后显式执行（PowerShell 7）
New-Item -ItemType Directory -Force .ar-workflow/temporary/plans
python scripts/workflow.py temp-preview <assignment-id> | Out-File -Encoding utf8 .ar-workflow/temporary/plans/plan.json
python scripts/workflow.py temp-archive .ar-workflow/temporary/plans/plan.json
python scripts/workflow.py temp-restore <上一步返回的journal路径>
```

只处理temp-create在专属目录创建、协调记录登记、内容和文件身份未变的普通payload；不收编旧日志，不永久删除。源不明、活动任务、全局writer、未知/在途DSH运行、原始usage/预算/检查数据、任何引用或无法完整读取的引用均保护。引用扫描覆盖仓库/工作流记录（排除Git对象、依赖工具缓存及已知自有元数据）；不宣称提供OS权限隔离或掌握机器外所有消费者。

执行重新盘点并比较计划，检查路径穿越、重解析点、Windows别名和目标冲突，只在明确专属根内同文件系统移动。恢复清单先落盘，保留中断真实位置，restore拒绝改过的归档和已存在的恢复目标；若归档或恢复清单后来被证据引用，也拒绝移动/改写。原预览计划丢失不妨碍按完整journal核对恢复。历史角色包/交接、冻结Review包、验证原件、预算及统计脚本不属于候选；无安全候选返回空清单，不制造释放空间结果。

先按 [文档与流程规则](文档体系与确认机制.md) 从 [模块与变更索引](模块与变更索引.md) 定位 change。模块长期设计、一次变更、变更步骤及角色任务包分别维护；同一功能的架构、开发、验证和 Review 共用一个 change-id，各自包有独立 assignment-id，不另建重复需求。索引是导航，实时执行状态仍核对工具及实际证据。

统筹从 AGENTS、当前任务 proposal/design/test-plan/tasks、适用需求、架构、ADR、规范和 Skills 准备上下文。公共正文只维护一份；任务包带路径、摘要、声明状态和实际内容。`context.json` 记录 objective/non_goals、tags、documents、skills、write_paths。尚未开始写 C++/Shader 时，使用 native 标签、显式 ar-cpp-oop-style 技能或具体原生允许写入路径即可提前加载原生依据；不能只等待 diff 出现。文档的 status/basis 是追溯声明，不能代替用户批准。架构与图示草案保持 reference，不因传给模型就升级为强制要求。

角色有 coordinator、architect、developer、verifier、reviewer、adjudicator，定义在 workflow/agent-policy.json。按需分配，不要求每个任务启动全部角色。开发者的聊天推理不进入 Review 请求；裁决才接收明确标记的反证。独立性来自实际新上下文和证据流程，角色名称本身不能证明独立。

## 单工作区执行

允许并行只读分析，用户安排全工作区每次只有一个实际写入者。手工执行者不必查询 agent-status、登记或释放 writer；旧自动化记录不作为权限依据。发现真实输入变化或活动进程冲突时报告具体情况，不接管或终止其他工作。Git 按实际授权操作，检查结论说明对应版本。

Review、验证和裁决默认只读；测试输出可写本任务忽略目录，源码修复交给获授权的执行者，无需补领写包。自动化快照仍限定 coordinator/architect/developer 的写路径，并核对 change 的 write_paths；现有完整性检查不删除。快照不给付费权限、不启动模型或聊天。

## 可选自动化命令及交接

以下命令用于主动选择的 SDK 执行、冻结输入导出或结构化证据导入，不是手工修改、Review 或交付的前置手续。工具自身仍有校验和锁；未使用或工具记录阻断时，不把它升级为手工工作审批，不手改旧包或 pass，也不抢占真实运行进程。

### 接力入口：先看当前输入与下一棒

```powershell
python scripts/workflow.py relay <change-id> --format markdown
python scripts/workflow.py relay <change-id>
```

relay 从现有角色包、交接、task 与原始资格证据派生，不维护第二份可编辑总状态，也不自动执行建议命令。显示输入、全局 writer、最新交接、变化路径、验证/Review 是否仍有效、阻断及下一角色。没有 task 记录时提示 start 后验证；writer 未交接时不建议验证、Review 或 Git。phase 或角色的 pass 自述不能获得资格，原始 Review 仍由既有 gate 核验。

新角色包自动带上分配时的 relay 摘要。需要明确接收某次交接时：

```powershell
python scripts/workflow.py agent-pack <change-id> --role verifier --agent verifier-1 --access read --goal "核对本次交接的验收与真实验证" --from-handoff <assignment-id>
```

显式接手要求同一 change、completed、新格式证据摘要完整且输入未变；不满足则拒绝，不新占 writer。交接后源变化时先看具体变化路径，由统筹核对、安排补检和新包，不复制旧摘要宣称仍有效。

新 handoff 保留证据内容摘要，协调记录保留 handoff 摘要；原结果仍是角色陈述。旧 handoff 不改写，显示 legacy_unsealed，可追溯但不能用 from-handoff 声称封存核验通过；统筹核对后可正常分配新任务包。哈希用于本地一致性检测，不是身份认证或抵御有权改写全部记录者的保证。

gate 核对一致时 relay 只报告“证据一致、仍需执行 gate”，不会自动写 local_ready。工具仍不自动发跨对话消息或启动角色；实际验证、审查、Git 继续串行。

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

交接只记录事实。手工检查和独立 Review 以真实结果、实际输入及覆盖为依据，不要求经 verify、review、finding-* 或 gate 才能成立；主动使用自动化入口时遵守其数据校验，不手写 pass。统计由统筹在完成/交接/停止前离线更新，子 Agent 不并发写统计。

## 独立审查与缓存

任务级 verify/review 显式绑定 change，从冻结快照读取本 change 的依据和技能；不同 change 不复用同一身份。API 请求不写死旧任务名。改动依据或技能会使相关请求缓存失效；角色 ID、时间戳及交接历史不进入审查语义缓存键。无 change 的通用 hook/远程路径明确只做源码通用检查，不声称完成任务级需求审查。

目前同一任务的所有单元共享该任务规划依据；修改 tasks/test-plan 等规划文件会使该任务的请求整体失效。跨任务或更细粒度的语义切片尚未实现，不宣称只更新勾选不会影响缓存。阶段检查结果优先保留于本机执行记录，避免把运行日志反复复制到规划正文。

API审查前先review-plan核对预估；独立上下文审查使用固定OCR输入和明确覆盖，不要求API打包预估通过来代替其实际阅读。不因角色分配自动调用模型，也不按任务包里的预算声明擅自追加额度。当前协议不接管 PR、分支保护或常驻调度器；实际结果见当前状态。
