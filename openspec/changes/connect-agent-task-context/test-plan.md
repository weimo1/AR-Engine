# 验证计划

| 验收要求 | 输入与命令 | 通过条件 | 失败与限制 |
| --- | --- | --- | --- |
| 当前任务依据可追溯 | context 命令、两个 change 的离线 fixture、冻结包重算 | 当前规划文件和摘要匹配，不串用旧 change | 越界/缺失/篡改拒绝；标签不是用户批准 |
| 技能及审查依据 | Python/C++/性能/第三方标签 fixture，构建实际 requests_for | 适用技能全文进入请求，草案保持参考 | 不在测试中调用外部查询或凭据 |
| 角色独立任务包 | agent-pack 的角色/权限 fixture | 目标、输入、依据、限制完整，不包含开发者聊天推理 | 任务包不能当独立 Review 通过 |
| 单工作区交接 | 临时仓库连续申请写权及修改源文件 | 第二 writer 与未交接的 verify/review/commit-check 拒绝、只读 stale、越界 blocked、异常释放留原因 | 协议锁不阻止工具外直接编辑 |
| 旧输入和共享检查 | 现有所有回归及 python scripts/workflow.py ci | 旧通用包保持有效，当前任务级包绑定 change | 无实际引擎构建、GPU 或远程保护验收 |

2026-10-21：首次共享 ci 对应 82 项回归、文档/派生与 OpenSpec 检查全部通过；其后增加当前 task gate 拒绝异任务/通用包的回归，最终数量以冻结包内 verification.json 为准。以上实现与首轮本地联调阶段未调用付费 API，未提交推送；之后的正式审查以本机原始响应和任务记录为准。

实际角色包 56f20a6f585747f0927e8a01 交给 /root/context_audit 只读核对：14 项正文和 SHA、当前任务及两项技能、reference/task/guidance 标签、只读和禁止自动付费边界一致；由统筹 agent-handoff 后状态 completed，writer=null，无源码改动。此为协议联调，不是独立 Review 批准。证据位于本机 .ar-workflow/agents/ 对应目录，正式审查和 gate 另外保存。
