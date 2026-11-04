# 验证

离线owned fixture覆盖：当前read reviewer接收、非review read拒绝、活动writer拒绝、缺失冻结文件/源过期/运行中输入变化/重复投递阻断；developer原路径保持。核对角色系统提示、只读overlay、desktop overlay、消息回执、未知usage/未完成/进程未退出的原回归。

运行针对性测试，再执行共享ci及strict OpenSpec。真实安装无费用doctor，人工核对官方SDK配置和persistence接口；不发送模型prompt，因此不声称本轮真实模型回复已在客户端显示。独立Review/gate与实际P0审查由统筹另行执行。

2026-10-30维护结果：DSH专项30项通过（含developer成功路径、reviewer准入/输入变化阻断和原有进程/用量测试）。真实安装reviewer doctor connected、runtime_stopped=true、model_request_sent=false；官方dump-config已核对实际desktop根目录、只读文件工具与四个禁用shell。证据位于本机 .ar-workflow/maintenance/dsh-readonly-review-20261030/，共享ci状态读取其中final-result.json，不将离线mock说成实际P0模型审查或客户端新回复显示。
