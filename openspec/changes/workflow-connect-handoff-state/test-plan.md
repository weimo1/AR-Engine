# 验证计划

| 验收 | 验证方式 | 通过依据 |
| --- | --- | --- |
| 摘要与只读 | 无任务、有交接、writer、JSON/Markdown 夹具；调用前后状态内容比对 | 不创建 runtime 记录，不改原始文件，不运行模型 |
| 交接完整性 | 新旧交接、篡改证据/包/交接、越界与不同 change | 新记录摘要匹配，历史明确未封存；坏记录阻断 |
| 接手 | from-handoff 正常与过期、跨 change、损坏 | 仅同输入正常新包成功；失败无 writer 副作用 |
| 资格 | 实际验证结构、日志篡改、旧输入、伪造 phase；mock 仅用于断言既有 gate 的调用与失败传播 | 不凭手写 pass，未核对资格不称有效 |
| 兼容 | 全部现有 unittest、workflow check、OpenSpec strict | 真实命令退出与日志记录，不代替独立 Review |

本轮证据保存于 .ar-workflow/maintenance/handoff-relay-20261027/，不混入已有架构修复证据。无 GPU/引擎实现，不执行对应验收。独立 Review/gate 未执行时明确记录，既有成功不能批准本次累计输入。
