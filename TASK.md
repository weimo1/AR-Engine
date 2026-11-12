# 项目总任务

唯一的进度页。Agent 从“任务队列”取第一项未勾选的任务，做完勾选并在同一行后面补一句结果。规则见 [AGENTS.md](AGENTS.md)，阶段路线见 [实施路线](docs/实施路线与阶段计划.md)。

格式约定（`scripts/autorun.py` 按此解析）：`- [ ] T编号 标题`，下面缩进写目标和验收；标题带 `[人工]` 的任务需要用户处理，自动循环到此停止。

## 当前阶段

**P2a 资源与上传**：[方案与验收](openspec/changes/p2a-resources-upload/proposal.md)。P1a 已完成人工验收。开发基线为 Vulkan 1.3（Dynamic Rendering、Synchronization2）。本机 RTX 4060 Laptop GPU 在 621.01 驱动下实测设备 API 为 1.4.357。

## 任务队列

- [x] T01 platform 模块与 GLFW 窗口：GLFW 3.4 已接入，隐藏窗口和 Vulkan 扩展查询通过，双配置各 51/51、零编译警告；`0de8052` 已补独立评审，无阻断项。
  - 目标：vcpkg 加入 glfw3；新增 `engine/platform` 静态库（`ar_platform`），提供 Window：创建/销毁、事件轮询、关闭请求、帧缓冲尺寸、是否最小化、所需 instance 扩展列表。不含输入映射。
  - 验收：CTest `platform-window-create`（创建隐藏窗口、轮询若干次、读取尺寸、销毁）；公开头检查通过；门禁全绿。
- [x] T02 rhi 逻辑设备与 Validation：真实设备创建与释放、缺特性拒绝、选卡及 Validation 转发通过；双配置各 54/54、零编译警告，Debug 零 Validation 错误，独立评审无阻断项。
  - 目标：在现有 instance 之上创建 surface 无关的逻辑 device：按 1.3 特性（dynamicRendering、synchronization2）选择物理设备，graphics 队列；Debug 配置启用 Validation 层和 debug messenger，消息转入 core 诊断。缺能力时返回明确错误，不强行运行。
  - 验收：CTest `rhi-device-create`、`rhi-device-missing-feature`（fixture）；真实 GPU 用例加 CTest 标签 `gpu`；Validation 零错误；门禁全绿。
- [x] T08 Vulkan 1.4 与 Descriptor Heap 设备验收：真机实际启用 Heap，限制和设备入口可用；保留 1.3 路径，双配置各 56/56、零编译告警，Debug 零 Validation 错误，独立复核通过。
  - 目标：保留 Vulkan 1.3 路径。支持显式创建 Vulkan 1.4 设备，记录 Maintenance 5/6、Dynamic Rendering Local Read、Host Image Copy 的支持状态；显式启用 Descriptor Heap，核对对齐、大小、保留区和关键设备入口。
  - 验收：`rhi-device-vulkan14-heap` 真机创建设备、启用扩展、读限制、关闭，Debug 零 Validation 错误；`rhi-device-enhanced-unavailable` fixture 验证缺 API 或扩展时明确拒绝。完整门禁与独立评审通过。Heap 写入、绑定与 Shader 读回在 P2 VMA 资源就绪后另验。
- [x] T03 swapchain、帧循环与清屏示例：Vulkan 1.3 窗口路径以呈现 fence 回收两套帧上下文，真机清屏 60 帧；双配置各 57/57、零编译告警，Debug 同步检查零 Validation 错误，独立复审通过。
  - 目标：surface、present 队列、swapchain；帧上下文（2 帧在途，fence/semaphore，不每帧 waitIdle）；新增可执行 `examples/triangle`（先只清屏），支持 `--frames N` 跑完退出。runtime 负责组合 platform + rhi。
  - 验收：CTest `sample-clear-frames`（标签 `gpu`，跑 60 帧 exit 0，诊断里 Validation 错误数为 0）；门禁全绿。
- [x] T04 Shader 构建与三角形：Vulkan 1.3 SPIR-V 编译验证通过，真机三角形呈现 60 帧，双配置各 59/59、零编译警告，Debug 零 Validation 错误，独立评审通过。
  - 目标：CMake 用 glslc 把 `shaders/*.vert|frag` 编成 SPIR-V 并跑 spirv-val，产物进 `build/`；graphics pipeline（Dynamic Rendering）画一个顶点色三角形。
  - 验收：CTest `shader-build`（产物存在且通过 spirv-val）、`sample-triangle-frames`（标签 `gpu`）；门禁全绿。
- [x] T05 截图与像素验证：真机末帧 PNG 1280×720，中心和四角像素通过；双配置各 60/60、零编译警告，Debug 零 Validation 错误，独立评审通过。
  - 目标：`--screenshot <path>` 在最后一帧回读 swapchain 图像写 PNG（stb_image_write）。
  - 验收：CTest `sample-triangle-screenshot`（标签 `gpu`）：中心像素落在三角形内且非清屏色，四角为清屏色；门禁全绿。
- [x] T06 resize、最小化与关闭：真机验证缩放重建、暂停恢复、截图尺寸及有序关闭，双配置各 61/61、零编译警告，Debug 零 Validation 错误，独立评审通过。
  - 目标：swapchain 过期/尺寸变化时重建；最小化时暂停渲染不忙等；关闭时等待在途帧后按序释放。
  - 验收：CTest `sample-resize`（标签 `gpu`，程序内改变窗口尺寸、最小化再恢复、继续出帧，Validation 零错误）；门禁全绿。
- [x] T07 [人工] P1 里程碑验收：用户实跑示例，报告 `presented_frames=1141`，运行无异常。
  - 用户运行 `examples/triangle` 看画面、拖动窗口；确认后在本页写出 P2a（Buffer/Image/VMA/上传）的任务。

- [x] T09 VMA 与 Buffer：VMA 3.3.0 真机创建两种 Buffer，主机读写、范围与失效句柄、关闭回收通过；双配置各 62/62、零编译警告，Debug 零 Validation 错误，独立评审通过。
  - 目标：按现有 vcpkg baseline 接入 VMA，由 RHI 持有设备级 allocator，提供类型化 Buffer 句柄及受控创建、释放。覆盖 host-visible 与 device-local 用途。
  - 验收：`rhi-vma-buffer` 真机创建两种 Buffer，验证 host-visible 写读一致、过期句柄拒绝和按序关闭。GPU 标签，Debug 零 Validation 错误，完整门禁与独立评审通过。
- [x] T10 Image 与 ImageView：真机验证 2D 图像、多个视图、错误销毁顺序及关闭回收；双配置各 63/63、零编译警告，Debug 零 Validation 错误，独立评审通过。
  - 目标：用同一 allocator 创建最小 2D Image，按类型化句柄建立 view，并约束 Image 与 view 的释放顺序。
  - 验收：`rhi-vma-image-view` 真机创建和释放，验证非法描述及存在 view 时禁止销毁 Image。GPU 标签，Debug 零 Validation 错误，完整门禁与独立评审通过。
- [x] T11 Buffer 与 Image 上传：真机逐字节验证 Buffer、Image 和 Host-visible 直读；双配置各 64/64、零编译警告，Debug 同步 Validation 零错误，独立复审通过。
  - 目标：使用图形队列上的 staging 传输，返回可轮询或等待的完成令牌。为最小调用方提供 Buffer 字节与 Image 像素的数值读回。
  - 验收：`rhi-upload-readback` 在真机上传确定数据，经 GPU 复制后读回并逐字节比较，同时检查同步 Validation。GPU 标签，完整门禁与独立评审通过。
- [x] T12 在途资源退役：真机验证在途释放、完成后槽位复用与会话关闭；双配置各 66/66、零编译警告，Debug 同步 Validation 零错误，独立评审通过。
  - 目标：释放请求立即使 CPU 句柄失效。资源、staging 与槽位按最后一次相关提交完成回收。
  - 验收：`rhi-resource-retirement` 受控测试验证完成前不回收，完成后旧句柄仍无效。GPU 用例 `rhi-upload-retirement` 验证在途上传释放及会话关闭。完整门禁与独立评审通过。
- [ ] T13 [人工] P2a 里程碑验收
  - 用户确认 P2a 的数值读回、资源退役和 P1 回归结果后，决定是否进入 P2b。

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
