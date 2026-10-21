# DSH 直接执行接入

统筹通过本机 SDK 下发任务，DSH 执行后交回结果。已安装的 DSH NEXT 自带 SDK，不需要操作聊天窗口或安装 Codex relay 插件。每个任务启动独立进程和新会话；运行时 home 与配置独立，CLI 默认把新会话交给官方 persistence 保存到客户端会话库，不复用或追加已有聊天。

用户最新指定所有 DeepSeek 调用统一 **DeepSeek-V4.1-Flash（界面徽标 1017）、High**。本机 DSH 目录映射为 deepseek-flash；SDK 显式 reasoningEffort=high。旧 Pro 测试只保留历史事实，不再运行该型号。直接 API 的 High 设置按 [DeepSeek 官方说明](https://api-docs.deepseek.com/guides/thinking_mode/) 使用 thinking=enabled、reasoning_effort=high；不自动降低等级或回退其他型号。

## 已验证范围

2026-10-21 本机 DSH NEXT 2.0.19-next / DSH 0.2.1-rc.2：无模型握手、SDK 身份、工作目录与退出成功。独立测试目录实际发送任务，由 deepseek-v4-pro 用 PowerShell 写入并回读指定文件；首次消费 7,333 tokens，禁重试 overlay 接入后再次验证消费 7,300 tokens，两次均 completed、进程退出、范围交接完成。证据入口 `.ar-workflow/dsh-bridge/real-smoke-pointer.json`。这是连接和文件执行验证，不是引擎或远程保护验收。

后续客户端可见性测试：会话 ar-visible-bd9e5472cfc840b3a3c4bace95db1e03，标题“AR Engine SDK 客户端可见性测试”，实际在客户端看到请求及 DSH_CLIENT_VISIBLE_OK 回复，用量 795 tokens；当时仍用旧 Pro，发生在用户最新模型限制之前，不宣称已经用新指定模型做付费生成验证。最新 Flash/High 无模型 doctor 已通过，无新付费生成。

## 操作顺序

从项目根运行；安装目录按本机实际修改：

```powershell
python scripts/workflow.py dsh-doctor --install 'F:/DSH NEXT'
python scripts/workflow.py dsh-doctor --install 'F:/DSH NEXT' --visibility desktop
python scripts/workflow.py agent-pack <change-id> --role developer --agent DSH --access write --paths <file-or-pattern> --goal '具体目标与验收'
python scripts/workflow.py dsh-run <assignment-id> --install 'F:/DSH NEXT' --model deepseek-flash --budget 100000 --seconds 600 --max-output 8192
python scripts/workflow.py dsh-status <job-id>
python scripts/workflow.py dsh-stop <job-id>
python scripts/workflow.py dsh-usage
```

dsh-run 默认 --visibility desktop，自动按 DSH 的 .dsh/desktop-next-location.json 或默认 .dsh 定位已有 sessions 目录；可用 --desktop-home 指定迁移后的客户端目录。压缩格式默认 zstd，与本机客户端一致；客户端自定义为 none 时须显式 --desktop-compression none。格式不匹配由后端拒绝，不搬迁或改写旧日志，不静默退回私有会话。需要不出现在客户端中的测试任务时显式 --visibility isolated。doctor 默认仍 isolated，不生成模型请求。

客户端首次显示可能需要使用“应用 → 重新加载界面”，界面刷新是一次接入检查，不是每次下发依赖的电脑操作。标题前缀带 AR Engine、角色及 change-id。SDK 运行时可以查看；不要同时在客户端续写该会话。官方后端提供跨进程单会话写锁，项目 writer 仍需显式交接。旧独立存储会话不会被自动导入。

先完成任务方案与 context.json，允许路径必须在任务范围内。上下文、规范和适用 Skill 正文由 agent-pack 注入 task.md；sdk-minimal 本身不自动发现全部项目 Skill，也不会替统筹批准架构。额外资源按包里的路径阅读。

doctor 不读取模型密钥或发送 prompt。dsh-run 才从既有用户级可信 credential 助手读取 key，仅放入专属进程环境；不写命令参数、项目配置或日志。DSH 默认不会把 KEY/TOKEN 环境变量继承给 shell，但模型有工作区 shell，不能把这当安全沙箱。

命令保持前台执行，另一个终端可用 dsh-status 离线查看。证据在 `.ar-workflow/dsh-bridge/jobs/<job-id>/`：run.json、runtime.json、events.jsonl、stderr.log、reply.txt、usage.json。model_completed 仅表示模型正常结束；统筹仍需检查产物与日志。

执行完后由统筹写结果文件，并运行：

```powershell
python scripts/workflow.py agent-handoff <assignment-id> --result .ar-workflow/<result-file>.json
python scripts/workflow.py verify <change-id>
python scripts/workflow.py review <change-id> --mode direct-api --budget <明确预算> --seconds <时限>
python scripts/workflow.py gate <change-id>
```

交接 JSON 见 [协作与上下文](多Agent协作与上下文.md)。DSH 不自动产生 review 批准，不提交或推送；修改范围与 Git 状态由 handoff 核对。

## 失败与费用

每包只投递一次，失败也不自动重发。重做前先核查旧输出与修改、确认专属进程退出，再交接并准备新包。没有会话接管或自动恢复 prompt，避免重复执行。入队、idle 或进程 exit 0 都不是成功；缺最终文字、截断、未知 usage、异常子 Agent 或错误终结均阻断。

--budget 是监测阈值。第一步按包 UTF-8 字节数加输出上限做保守准入；DSH 没有每步请求前的总额硬门，工具返回和运行时系统提示也会增加费用。收到 usage 达阈值或超时后终止自己进程，不能保证账单不超过阈值。中断未知请求不能记零。--max-output 限制每次输出，--seconds 最长 1800 秒。专属 overlay 禁用 llm-retry、自动 compaction 和 image-offload，避免隐式重试与付费摘要；不要让执行任务再调用模型或委派子 Agent。

Windows 启动时先暂停进程、绑定带 kill-on-close 的专属 Job Object，再恢复执行；主控意外退出也会由系统终止关联进程树。清理先发 shutdown，再终止并核对整个 Job 的活进程数；从不按 DSH 进程名批量结束。未确认停止，agent-handoff 和 agent-release 都拒绝。

前台运行可 Ctrl+C 请求清理；dsh-stop 用于主控退出后恢复未收尾记录，活着的操作锁不会被抢占。恢复按 ownership 中随机 Job 名核验，不使用可能复用的旧 PID 杀进程；不能确认拥有关系则保留 writer。恢复后状态 blocked、费用可能未知，仍不自动重发或交接。不要手改 stopped 或 pass。

用量 TXT：`.ar-workflow/dsh-bridge/DSH-token用量统计.txt`。按 sessionId+event.seq 去重，失败 attempt 计入；inputTokens 是未缓存输入，加缓存读写和输出，不再加推理。与既有 HTTP/OCR 台账单列，未核对响应 ID 前不直接加总。日志保留本地，不自动上传。
