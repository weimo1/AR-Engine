# 验证计划

| 范围 | 检查与通过条件 |
| --- | --- |
| 包与兼容 | v2冻结清单逐项核验；legacy保留；inspect/relay/Review/DSH拒绝缺失或改写依据 |
| 路由与prompt | 无关native不影响Agent；显式native/Review调用方仍加载；实际DSH prompt不含全文marker |
| 体积 | 同清单首页预览对比旧246477字节样本；真实新包核对完整依据与字节贡献，不声称实际token减少 |
| 接手 | 未分配/已生成/本地回执，agent/输入/包错误拒绝，未发送不写sent |
| 清理 | 默认只读，活动/引用/未知状态保护；工具创建provenance，拒绝计划篡改、过期、穿越及重解析点；归档恢复不覆盖 |
| 共享 | handoff后ci；保留失败日志与最终结果；独立Review/gate另排 |

证据位于本机 .ar-workflow/maintenance/task-package-lifecycle-20261027/。不执行GPU或付费模型，不操作真实历史证据的移动/恢复。
