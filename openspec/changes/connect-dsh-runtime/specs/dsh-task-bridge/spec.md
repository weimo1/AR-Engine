<!-- Generated from proposal.md; source-sha256: cb2ea406769e1d1cf64577724ad7e79c10ceacf4ba9a10d1c850abc70e7ef0aa. Do not edit. -->

## Purpose

让统筹以可记录的程序接口将已准备的任务包交给本机 DSH 执行，取得运行状态、回复和用量。保持任务上下文、唯一写入者和后续验证审查边界，明确模型结束与功能验收的区别，不触碰用户已有聊天。

## ADDED Requirements

### Requirement: 专属运行时和无模型握手
连接器 SHALL 启动用户显式指定安装位置的 SDK minimal 专属进程，使用项目拥有的 home 与新会话；先完成 initialize，提供不发送 prompt 的 doctor。不得绕过 GUI 认证、读取已有对话正文或修改全局配置。模型凭据只通过既有可信助手读取并放入子进程环境，不进入参数或日志。

#### Scenario: 只检查连接
- **WHEN** 运行 doctor
- **THEN** 验证 SDK 身份和工作目录后关闭自己启动的进程，不调用生成、不读取模型凭据

### Requirement: 客户端可见的新会话
按用户随后提出的客户端显示要求，任务下发 CLI SHALL 默认将新会话交由官方 persistence 后端保存到已存在的客户端会话库，使用匹配压缩格式，并保留显式 isolated 模式。运行时 home、配置、凭据和任务记录仍独立；仅新会话日志共享，不复制、恢复或追加既有聊天，不修改客户端注册表或安全设置。桌面存储不可用 SHALL 明确失败，不静默退回客户端看不到的私有会话。显示与执行、正式验收资格分别记录。

#### Scenario: 客户端查看执行结果
- **WHEN** 新任务使用 desktop 存储完成
- **THEN** 会话包含任务标题、请求与回复，可由客户端刷新后发现；模式、存储和会话 ID 可追溯，已有独立存储任务不会伪装成已显示

### Requirement: 绑定任务包且不重复投递
连接器 SHALL 仅发送有效且输入未变化的写入任务包，校验包摘要与当前 writer；每包最多投递一次，一次专属会话仅一项任务。入队收据不得视为完成；重复、过期及不拥有 writer 的请求在模型调用前拒绝。

#### Scenario: 重试或输入变化
- **WHEN** 同一任务包已有运行记录或源码自准备后变化
- **THEN** 拒绝自动重发，保留证据并要求统筹检查后准备新任务

### Requirement: 有界运行与真实结果
连接器 SHALL 要求显式 token 监测阈值、时限及单次输出上限，保存协议事件、回复和结构化状态；仅当前专属会话 completed 终结、wait 成功及存在正文才记为模型完成。失败、截断、超时、未知用量不能记为通过。取消只终止自己启动的进程，进程未确认退出不得释放 writer。DSH 不支持请求前总费用硬限，监测阈值和时限不得宣传为账单封顶。

#### Scenario: 入队成功但执行失败
- **WHEN** prompt 有 messageId 而后出现 max-tokens、错误或超时
- **THEN** 结果阻断，保留已知用量和未知费用提示，不修改 verification/review/gate

### Requirement: 可核对用量与后续交接
连接器 SHALL 按会话与事件序号记录模型消息和失败 attempt 用量，缓存输入计入总量一次，推理输出不重复加入；提供可离线生成的 TXT。结束运行不自动释放任务或授予交付资格，交接仍核验修改范围，随后由统筹运行验证和独立审查。

#### Scenario: 失败或重复事件
- **WHEN** 观察到失败 attempt 或相同事件重复推送
- **THEN** 前者计入已知消耗，后者不重复计数，缺少 usage 明确记为未知

### Requirement: 用户指定的 DeepSeek 型号与等级
按用户最新截图，所有新 DeepSeek 调用 SHALL 使用 DeepSeek-V4.1-Flash（界面徽标 1017）、High，已核对本机 SDK ID 为 deepseek-flash、reasoningEffort=high；Chat Completions 显式 thinking=enabled、reasoning_effort=high。不得因错误、预算或截断回退其他型号或等级。不支持核实 High 的付费入口 SHALL 阻断。历史请求与模型记录不得重写成新型号，旧冻结请求不得绕过当前调用策略继续付费。

#### Scenario: 旧型号或低等级请求
- **WHEN** 准备发出 Pro、关闭思考或低于 High 的新请求
- **THEN** 在付费前拒绝，不自动换型号；原始历史证据仍按当时事实核验
