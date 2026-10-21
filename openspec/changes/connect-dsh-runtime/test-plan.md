# 验证计划

| 验收要求 | 输入与命令 | 通过条件 | 失败与限制 |
| --- | --- | --- | --- |
| 无模型握手 | dsh-doctor --install 本机安装目录 | SDK 身份、工作目录匹配、退出；未读取 key 或发送 prompt | 安装缺失、RPC 错误、异常 stdout 必须失败 |
| 任务包绑定 | 离线临时仓库及 dsh-run | 当前 writer、输入和 task 摘要校验；同包第二次被拒 | 不隔离绕过协议的手工写入 |
| 运行结果 | fake RPC + 真实极小专属任务 | 有 completed、wait、正文、usage 和进程退出 | ack/idle/exit0、max-tokens、缺 usage、超时均不能完成 |
| 用量与交接 | usage 事件夹具、dsh-usage、agent-handoff | 失败 attempt 计数，重复 seq 不重计，缓存不重复，进程未停拒绝交接 | 中断未知账单、压缩与子 Agent 不可核算时保守阻断 |
| 当前输入验证和审查 | workflow.py ci；verify/review/gate connect-dsh-runtime | 确定性检查通过后执行独立审查，当前报告重算通过 | 准备、入队及本地 JSON 不是远程批准 |

真实 DSH 请求只用于本次连接验收，固定小样本、明确时限和监测阈值，不自动重试，不在正式工程实施引擎功能。不覆盖 C++/Shader/GPU，也不启用远程保护。

## 2026-10-21 实跑

- 客户端共享存储首次 none 格式被拒；改用 zstd 后生成 795 tokens 的可见性测试，已实际在客户端看到标题、请求和回复。证据入口 .ar-workflow/dsh-bridge/client-visibility-pointer.json。发生在新模型限制前，保留原模型事实。
- 最新 Flash/High 的无模型 desktop doctor：54e76e0bfc2c4e4a97f7959d，connected、未发送 prompt；已加入旧模型/关闭思考/降低等级的付费前阻断测试。没有为新模型偏好追加付费调用。

- doctor：f77790e9bf6b49ac888827f4；禁重试配置 doctor：2b18c25c65b24c1cb0b9080d，均 connected、无 prompt、runtime_stopped=true。
- 独占 fixture：smoke-af5d095cb0e2；首次 job aac8b9b8c2854bf3954a7f82，7333 tokens；禁重试配置 job 3bd687d1b1d944499bcfd8f3，7300 tokens。均 20000 监测阈值、180 秒、每次输出 4096，模型 deepseek-v4-pro，指定文件实际回读正确，handoff completed。首次使用默认 minimal，第二次禁用重试和溢出摘要。
- 独立只读审计找到 stream usage 包装、最终正文绑定、构造失败后进程清理及根进程先退后工具子进程存活问题；已加入 Windows Job 管理、dsh-stop 恢复核验及对应回归，包含主控被终止后系统清整树。Job 版本 doctor 为 3b1e4403eab74efca1ac4e9d。最终测试数量以当前 verification.json 为准。
- 最终当前输入验证、独立模型审查与 gate 结果保存于 .ar-workflow 的任务记录；此文件在冻结前记录实施事实，不能自报正式批准。

- 本轮 Flash/High 首个审查单元 direct-08cc00d78567 实耗 74,800 tokens，16,384 输出全为推理、finish_reason=length、正文为空，正确阻断。保留 attempt-0-0.json；不记审查通过。工具锁新增 65,536 输出上限供新审查/裁决使用，历史缺字段请求身份保持不变，回归核对估算与 High 参数。

- 后续 direct-dae1ed568f92 完成 3/7 单元（325,533 tokens），第 4 单元另耗 154,682 tokens 且 65,536 输出全为推理、正文为空；整轮失败，共 480,215 tokens。已修正文档状态/范围及 Git 环境继承问题，增加污染环境回归。新输入使用 131,072 输出上限，仍为 Flash/High；旧响应均保留，不作为批准。

- 131,072 输出上限的完整复审 direct-9a98209163f4：7/7 单元，954,254 tokens，request_changes；原始意见保留。修正文档历史/当前状态与 direct-api 产物说明；预算改为显式必填；新增原生任务启动前依据选择、RPC ID 类型与未知事件阻断、删除材料裁决引用及对应回归。远程 400,000 调度预算保持既有费用约束，超限阻断且未启用远程保护，不自动增额。修复后的当前输入须重新验证和独立复审。
