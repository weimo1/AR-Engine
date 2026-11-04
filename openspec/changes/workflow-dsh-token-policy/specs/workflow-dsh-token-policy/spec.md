<!-- Generated from proposal.md; source-sha256: a5b772ad4cb269faa288a976166499d2ca09483415fe60cccecd11165af225dc. Do not edit. -->

## Purpose
明确无token阈值的调用语义，保留原始中断证据，避免静默以技术阈值代替用户费用决定。

## ADDED Requirements
### Requirement: 显式取消 token 阈值
DSH SHALL 通过与正数budget互斥的no-token-limit显式选项取消初始token估计门和运行中token停止条件；省略两者或冲突/非正数输入拒绝。保留seconds、max-output、型号/角色/冻结输入、防重复、未知usage及进程退出检查。取消阈值不表示自动付费重试授权或取消时间限制，不改变独立Chat Completions审查的既有保护。

新包调用前 SHALL 检查已有SDK任务，存在未确认退出或未知费用时阻断，不以新包或新会话绕过旧在途消费。

#### Scenario: 无阈值运行
- **WHEN** 明确选择no-token-limit并有合法时限和输出限制
- **THEN** 仍记录所有实际usage，不因累计tokens自动停止，也不将未知消耗记零

### Requirement: 只读续接预检
续接检查 SHALL 只读取已有任务与新包，核对同change/role/agent、官方存储、Flash/High、原进程退出、新包有效和当前输入；原任务费用未知、缺收据/终结、已被后继消费或角色不符均给出阻断。检查不调用SDK恢复或prompt，不改变历史run/usage/pass，不把检查成功宣称恢复已实现。

#### Scenario: 中断费用未决
- **WHEN** 原任务unknown_cost仍为true或原始终结证据不足
- **THEN** 提供具体阻断及后续核对项，不生成可付费继续的通过结论
