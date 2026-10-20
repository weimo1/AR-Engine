---
name: ar-e2e-side-effect-safety
description: >-
  Plan and run nontrivial AR Engine integration, GPU, filesystem and editor GUI
  tests with owned resources and bounded load on Windows. Use when tests can
  affect files, processes, devices or user interaction; trivial pure unit tests
  need no separate isolation ceremony.
license: CC-BY-NC-SA-4.0
---

# 引擎测试环境与副作用约束

先读 [项目 AI 技能约束](../../../docs/standards/AI技能约束.md)。目标是在已授权范围内自动测试，同时保护用户工作和其他任务；不为已经明确授权且影响不变的步骤重复询问。

## 执行前

- 明确测试实际读写的场景、资产、配置、截图、日志、进程和设备。使用任务独占目录、配置覆盖和输出标识，不把工作目录不同当作隔离已成立。
- 优先纯逻辑、离屏渲染、脚本入口和截图等可控路径。编辑器输入测试需要真实交互时，明确目标实例、窗口、坐标空间和执行范围；不因进程在后台就认为不会抢焦点。
- 按 Windows 的实际工具能力实现隔离，不直接套用 Xvfb、DISPLAY 或 Linux 服务命令。需要新增工具或环境时说明缺口，不临时修改全局配置来让测试通过。
- 设定合适的运行时间、并发和 CPU/GPU/内存预算。从小负载开始；隔离进程仍可能占满共享 GPU。

## 执行与恢复

- 先运行已验证不会干扰用户的检查。无法确认隔离且确需影响用户桌面时，准备最短测试、说明影响并取得具体步骤授权；已有有效授权可以直接执行。
- 测试修改场景副本和私有配置，不修改用户原始场景、系统音量、显示设置、凭据或共享服务。
- 用实际进程身份、路径、任务标识定位资源，禁止按通用进程名批量结束。取消时释放本任务持有的输入、进程、临时文件与资源。
- 有意测试崩溃、超时、设备异常或恶意输入时使用可恢复夹具，不在用户活跃数据上故障注入。
- 出现意外干扰时停止本任务相关动作，记录原因和影响，先形成恢复办法再进行新的有影响操作。

## 结果

报告实际隔离方式、执行范围、检查结果与未验证项。没有桌面后端、GPU 或必要环境时保留缺口；不能靠 mock 结果宣布真实 GPU/交互通过。

原版说明在 `references/upstream-skill.md`，作为跨平台经验来源。读取它不代表 Windows 具备其列举的工具。
