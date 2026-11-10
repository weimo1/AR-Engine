# 项目总任务

唯一的进度页。Agent 从“任务队列”取第一项未勾选的任务，做完勾选并在同一行后面补一句结果。规则见 [AGENTS.md](AGENTS.md)，阶段路线见 [实施路线](docs/实施路线与阶段计划.md)。

格式约定（`scripts/autorun.py` 按此解析）：`- [ ] T编号 标题`，下面缩进写目标和验收；标题带 `[人工]` 的任务需要用户处理，自动循环到此停止。

## 当前阶段

**P1a 首个画面**：窗口 → device → swapchain → 三角形 → 截图。P1/P2 以 Vulkan 1.3 为开发基线（Dynamic Rendering、Synchronization2）。本机 RTX 4060 Laptop GPU 在 621.01 驱动下实测设备 API 为 1.4.357，产品目标 1.4 与 Descriptor Heap 另行验收。

## 任务队列

- [x] T01 platform 模块与 GLFW 窗口：GLFW 3.4 已接入，隐藏窗口和 Vulkan 扩展查询通过，双配置各 51/51、零编译警告；`0de8052` 已补独立评审，无阻断项。
  - 目标：vcpkg 加入 glfw3；新增 `engine/platform` 静态库（`ar_platform`），提供 Window：创建/销毁、事件轮询、关闭请求、帧缓冲尺寸、是否最小化、所需 instance 扩展列表。不含输入映射。
  - 验收：CTest `platform-window-create`（创建隐藏窗口、轮询若干次、读取尺寸、销毁）；公开头检查通过；门禁全绿。
- [x] T02 rhi 逻辑设备与 Validation：真实设备创建与释放、缺特性拒绝、选卡及 Validation 转发通过；双配置各 54/54、零编译警告，Debug 零 Validation 错误，独立评审无阻断项。
  - 目标：在现有 instance 之上创建 surface 无关的逻辑 device：按 1.3 特性（dynamicRendering、synchronization2）选择物理设备，graphics 队列；Debug 配置启用 Validation 层和 debug messenger，消息转入 core 诊断。缺能力时返回明确错误，不强行运行。
  - 验收：CTest `rhi-device-create`、`rhi-device-missing-feature`（fixture）；真实 GPU 用例加 CTest 标签 `gpu`；Validation 零错误；门禁全绿。
- [ ] T03 swapchain、帧循环与清屏示例
  - 目标：surface、present 队列、swapchain；帧上下文（2 帧在途，fence/semaphore，不每帧 waitIdle）；新增可执行 `examples/triangle`（先只清屏），支持 `--frames N` 跑完退出。runtime 负责组合 platform + rhi。
  - 验收：CTest `sample-clear-frames`（标签 `gpu`，跑 60 帧 exit 0，诊断里 Validation 错误数为 0）；门禁全绿。
- [ ] T04 Shader 构建与三角形
  - 目标：CMake 用 glslc 把 `shaders/*.vert|frag` 编成 SPIR-V 并跑 spirv-val，产物进 `build/`；graphics pipeline（Dynamic Rendering）画一个顶点色三角形。
  - 验收：CTest `shader-build`（产物存在且通过 spirv-val）、`sample-triangle-frames`（标签 `gpu`）；门禁全绿。
- [ ] T05 截图与像素验证
  - 目标：`--screenshot <path>` 在最后一帧回读 swapchain 图像写 PNG（stb_image_write）。
  - 验收：CTest `sample-triangle-screenshot`（标签 `gpu`）：中心像素落在三角形内且非清屏色，四角为清屏色；门禁全绿。
- [ ] T06 resize、最小化与关闭
  - 目标：swapchain 过期/尺寸变化时重建；最小化时暂停渲染不忙等；关闭时等待在途帧后按序释放。
  - 验收：CTest `sample-resize`（标签 `gpu`，程序内改变窗口尺寸、最小化再恢复、继续出帧，Validation 零错误）；门禁全绿。
- [ ] T07 [人工] P1 里程碑验收
  - 用户运行 `examples/triangle` 看画面、拖动窗口；确认后在本页写出 P2a（Buffer/Image/VMA/上传）的任务。

## 人工待办（不阻塞队列）

- 云端 “Engine CPU build and test” 首跑（2026-11-10，run 42821435014，`49b5633`，4 分 52 秒）：工具链检查、依赖恢复、两配置编译通过，Debug CTest 47/49；失败的 `runtime-system-inspect`、`runtime-system-development` 需要真实 Vulkan 加载器。两项已标 `gpu`，云端改用 `build_checks.py --no-gpu`；推送后用 `gh workflow run` 再跑一次确认，绿了再决定是否改成推送触发 + vcpkg 缓存。

- 首次使用自动循环前：修正 Codex CLI 的默认模型（见 [自动运行](docs/workflow/自动运行.md) 的注意事项），设置 `CLAUDE_CODE_GIT_BASH_PATH`，然后用 `--max-tasks 1` 试跑 T01。

## 待办池（建议级，不阻塞）

- 完整门禁每个配置先增量构建、再干净重编一次（为了采集编译命令行），代码变多后耗时翻倍；评估去掉其中一次。
- clang-tidy 需要 compile_commands.json，VS 生成器不产出；P1 后评估用 Ninja 预设跑静态检查。

- CMake 目前必须先由脚本生成 build identity 才能配置；P1 有可执行程序后评估给缺省值，方便直接在 VS 打开。
- 工具链检查由精确版本改为最低版本。
- `openspec/changes/bootstrap-engine-p0/tasks.md` 是 25KB 流水日志，P0 归档时压成勾选清单。

## 已完成

- **P0 基础工程（2026-11-06 本地完成）**：core 诊断/错误、rhi 加载与能力查询、runtime 组合与回滚；49 个用例，两配置各 49/49。方案与历史记录见 [P0 change](openspec/changes/bootstrap-engine-p0/tasks.md)。未做：云端 CPU 构建首跑、真实 1.4/Heap 验收。
