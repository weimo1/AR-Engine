# 方案

dsh-run参数使用必选互斥组：--budget 正整数或--no-token-limit。run内部显式bool与None配对，Evidence仅在阈值存在时检查tokens。记录token_limit_mode，旧budget路径不变；时限及max-output仍必需，API组包保护不动。

新增dsh-resume-check JOB --assignment NEW，零费用输出结构化阻断与旧sessionId。仅准备同会话续接，当前不实现session/resume付费通道；未知费用不得通过新包清零。恢复后需用新冻结任务包、新消息身份、保留旧会话证据，区分历史usage与本轮usage，保留SDK会话写锁及角色独立性。SDK源码支持resume不代表项目已做真实恢复验收。

实际中断会话按官方多frame Zstd结构离线核对，不用单frame解码结果推断空日志；原session只读，保存压缩快照/摘要及source hash，原run/usage未知标记不改。
