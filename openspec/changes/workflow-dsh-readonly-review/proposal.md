# DSH 只读 reviewer 调用

2026-10-30统筹转交用户授权：直接SDK分配角色并保存DSH对话。维护请求见本机 .ar-workflow/maintenance/dsh-readonly-review-20261030/request.md。本change只补工作流入口，不修改P0方案或实施引擎，不发付费请求、Git或旧会话操作。

## Capabilities

- 新增 workflow-dsh-readonly-review：独立只读任务经SDK调用，保留真实回执与官方会话保存。

<!-- AR:CONTRACT capability=workflow-dsh-readonly-review -->
## Purpose

让独立reviewer使用自己的只读任务包，而不冒用开发者writer；模型结束、会话保存和正式Review资格分开记录。

## ADDED Requirements

### Requirement: 只读审查准入
SDK SHALL 接受当前完整的read/reviewer包，拒绝其他read角色、活动writer、变更输入或缺失冻结依据；原write执行准入不放宽。发出prompt前和review结束时重新核对输入及任务包，不自动重复投递。

#### Scenario: 源码或包已变化
- **WHEN** reviewer包的冻结文件缺失或当前源输入变化
- **THEN** 调用阻断，不声明有效审查完成

### Requirement: 对应角色与能力约束
reviewer SHALL 使用独立审查提示，读取完整冻结清单、逐项列出covered/uncovered及证据，禁止源码修改、Git、委派或再调模型。配置SDK只读sandbox及只读文件工具，禁用shell；桥接器保存回复，不要求模型写证据。此配置不声明OS权限隔离或完整覆盖。

#### Scenario: 模型给出结论
- **WHEN** 模型正常结束且usage已知
- **THEN** 仅记录模型完成，未覆盖及正式独立Review导入/gate仍由统筹核验

### Requirement: 保存与续聊分别验收
调用 SHALL 复用官方desktop persistence、保存角色/change、sessionId、发送回执、回复与usage；每次任务仍新建独立会话，不修改旧存储伪装恢复。续聊作为后续有限任务，未经验证不得声称已支持。保留固定Flash/High、未知费用及进程退出阻断，不自动付费重试或放宽API组包保护。

#### Scenario: 请求未完成
- **WHEN** 缺收据、最终回复、用量或运行时退出确认
- **THEN** 保留阻断及原始记录，不记为Review通过，不自动重发
<!-- /AR:CONTRACT -->
