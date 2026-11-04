<!-- Generated from proposal.md; source-sha256: 6f31268183ab0b74e349ced45c8d64e01f34821d9182e1eaf31d07f40e7b745b. Do not edit. -->

## Purpose

让统筹及后续角色从同一套真实任务、角色与资格证据恢复当前工作，减少重复阅读全部历史；明确最新输入、有效与过期证据以及下一步，同时保留既有独立验证、审查和交付门槛。

## ADDED Requirements

### Requirement: 只读接力摘要
工具 SHALL 为指定 change 生成 JSON 或 Markdown 接力摘要，显示当前工作区输入指纹、交付检查适用范围、全局 writer、最新角色交接、有效资格、阻断和下一角色/动作；没有运行任务记录时仍能报告待初始化/验证，不伪造运行结果。摘要不写回任务阶段、重置预算或自动调用外部服务。

#### Scenario: 文档角色完成但没有运行任务
- **WHEN** 当前 change 有已完成交接而尚无 task 运行记录
- **THEN** 摘要说明交接是否匹配当前输入，提示统筹 start 后安排验证，独立 Review/gate 均未完成

### Requirement: 交接可核对且身份隔离
新交接 SHALL 保留结果及输入，并记录证据内容摘要和交接记录摘要。接力读取核对角色包、记录及证据；旧交接作为未封存历史线索，不伪装成新格式核验。损坏或越界记录明确阻断，不重置或自动抢占。接手指定交接时核对同一 change、completed、当前输入及完整摘要，失败不生成包。

#### Scenario: 修改交接后的证据或跨任务接手
- **WHEN** 指定交接证据被改、输入变化或 change 不同
- **THEN** 接手被拒绝，已有 writer 和记录保持不变；不能靠修改 summary 或 next_action 放行

### Requirement: 验证与审查资格来自原证据
摘要 SHALL 对当前交付输入重新核对正式验证的必需命令及日志摘要；独立审查资格通过既有 task_gate 核验原始证据，不信任 phase 或 handoff 的 pass 字段。输入变化或证据损坏使相应状态无效，并提示重检；全局 writer 存在时不得建议并发验证、Review 或 Git。

#### Scenario: phase 声称可交付但日志被改
- **WHEN** task 为 local_ready 而验证日志变化或 Review 不完整
- **THEN** 摘要不得显示可交付，报告实际阻断；不自动执行测试、模型或修改原证据

### Requirement: 后续任务包承接
新角色包 SHALL 自动包含分配时的接力摘要及输入标识；可用 from-handoff 指定交接，并记录已核对来源。角色包仍是协作记录，不提供 OS 隔离、付费权限或独立审查身份。

#### Scenario: 正常接手
- **WHEN** 统筹指定同 change、当前输入且证据完整的新交接生成下一个包
- **THEN** 包包含来源和摘要，并继续遵守唯一 writer；摘要不能代替角色阅读具体任务与源码
