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
- [x] T13 [人工] P2a 里程碑验收：用户实跑三角形样例，报告累计呈现 574 帧；核对 P2a 读回、退役和 P1 回归后，同意继续 P2b。
  - 用户确认 P2a 的数值读回、资源退役和 P1 回归结果后，决定是否进入 P2b。


- [x] T14 P2b 绑定设备与固定容量描述符表：8 个纹理槽、4 个 sampler 槽及默认资源真机验证通过，双配置各 68/68、零编译警告，DSH 独立评审通过。
  - 目标：显式启用绑定能力，创建纹理和 sampler 表，初始化默认槽，拒绝超限。
  - 验收：`rhi-set-binding-limits` 和 `rhi-set-binding-device`，完整门禁与独立评审通过。
- [x] T15 材质索引采样与像素验证：真机四区像素和双 view 传输保护通过，双配置各 69/69、编译零警告，DSH 独立评审通过。
  - 目标：在样例中用不同材质索引访问纹理和 sampler。
  - 验收：`sample-set-binding-pixels` 真机读回，完整门禁与独立评审通过。
- [x] T16 在途绑定与槽位退役：真机验证旧帧像素、槽位复用和传输恢复，诊断时序通过；双配置各 71/71、编译零警告，DSH 独立复审通过。
  - 目标：立即拒绝旧句柄，最后使用完成后恢复默认映射并复用槽位。
  - 验收：`rhi-set-binding-retirement` 和 `sample-set-binding-retirement`，完整门禁与独立评审通过。
- [x] T17 [人工] P2b 里程碑验收：用户确认继续；绑定像素、退役及 P1/P2a 回归有本地双配置 71/71 和独立复审证据。
  - 用户确认绑定样例的画面、数值读回、在途安全与 P1/P2a 回归结果。

- [x] T18 固定测试场景与 CPU 视锥剔除参考：六平面与容量边界通过，32 字节对象布局经编译期验证，双配置各 72/72、零编译警告，DSH 独立复审通过。
  - 目标：定义测试几何、包围球、固定视锥和 CPU 可见集合。
  - 验收：`render-culling-reference` 覆盖内外、相交和容量边界。
- [x] T19 Compute 剔除与有界命令生成：真机对照 CPU 可见集合、命令、计数与容量边界，双配置各 73/73、零编译警告，DSH 独立评审通过。
  - 验收：`rhi-gpu-culling-reference` 真机对照 CPU 集合、计数与溢出。
- [x] T20 间接绘制与像素验收：真机可见及零可见场景连续帧通过，双配置各 74/74、零编译警告，DSH 独立评审通过。
  - 验收：`sample-gpu-driven-pixels` 覆盖可见几何、空集合和连续帧。
- [x] T21 [人工] P3a 里程碑验收：展示真机画面及双配置结果后，用户同意继续开发。
  - 用户确认 GPU 与 CPU 对照、实际画面及回归结果。

### P3b 静态场景与资产

技术方案见 [P3b](openspec/changes/p3b-static-scene-assets/design.md)。

- [x] T22 静态 glTF 和 PNG 资产加载：子目录外部 BIN/PNG、共享缓存和失败重试已验证；双配置各 75/75、零编译警告，DSH 委托评审 19/19 通过。
  - 目标：用已选依赖生成可共享 CPU 数据，校验引用和数据范围。
  - 验收：`assets-static-gltf` 核对几何、索引、像素、共享与坏输入恢复。
- [x] T23 场景保存、重开与同步提取：双实例共享、保存重开、失败回滚及提取存活通过；双配置各 76/76、零编译警告，DSH 委托评审 14/14 通过。
  - 目标：保存稳定对象标识、实例变换、相机及资产引用，通过 runtime 提取值数据。
  - 验收：`scene-static-roundtrip` 核对实例独立、共享资产、提取有效期及失败回滚。
- [x] T24 静态场景真机呈现：双实例纹理、保存重开和部分上传失败回滚重试通过；双配置各 77/77、零编译警告，DSH 委托评审 27/27 通过。
  - 验收：`sample-static-scene-pixels` 验证双实例像素、保存重开一致性及 Debug Validation。
- [x] T25 [人工] P3b 里程碑验收：用户查看真机画面及保存重开、回滚证据后确认继续。
  - 用户确认实际画面、重开结果与回归证据。

### P3c 材质与基础画面

参见 [P3c 技术方案](openspec/changes/p3c-materials-lighting/design.md)。

- [x] T26 不透明 glTF 材质输入：默认值、显式因子、sRGB/线性通道与 P3b 兼容通过；双配置各 78/78，零编译警告，DSH 委托评审覆盖 17/17。
  - 验收：`assets-opaque-material` 核对默认值、显式因子、贴图解释及 P3b 兼容。
- [x] T27 GBuffer 与深度读回。真机验证 Reverse-Z 遮挡、材质与法线附件和视空间位置重建，双配置各 79/79、零编译警告，DSH 独立评审通过。
  - 验收：`sample-gbuffer-probes` 核对基础颜色、法线、材质通道和近片遮挡。
- [x] T28 延迟直接光与 HDR 转 SDR。真机验证 HDR 大于 1 且两档曝光读回相同，SDR 符合单次 sRGB 编码参考；双配置各 80/80、编译零警告，DSH 独立评审通过。
  - 验收：`sample-hdr-lighting-probes` 对照光照、曝光和颜色编码参考。
- [x] T29 环境光与子资源状态。真机验证常量能量、方向、粗糙度 mip 和失败回滚；双配置各 82/82、零编译警告，DSH 独立评审通过。
  - 验收：`sample-ibl-probes` 核对立方体方向、Mip/layer 与环境取样。
- [x] T30 [人工] P3c 里程碑验收。用户在收到同场景的直接光和环境光截图、材质探针与回归证据后回复“可以的继续”。双配置各 82/82，T29 独立评审通过。
  - 用户确认展示画面、材质诊断视图及回归证据。

### P4a 轻量 Render Graph

参见 [P4a 技术方案](openspec/changes/p4a-lightweight-render-graph/design.md)。

- [x] T31 CPU 资源声明与计划。逐子资源同步、连续读取、丢弃覆盖和导入导出通过，双配置各 83/83，DSH 审查 14/14 通过。
  - 验收：`render-graph-plan` 核对逐子资源状态、连续读取、丢弃覆盖、外部导入导出及无效声明。
- [x] T32 Vulkan 单队列执行与 P3c 迁移。可选 Copy、提交后状态发布及帧槽隔离通过，原探针和 resize 双配置各 83/83，DSH 独立评审 15/15 通过。
  - 验收：复跑 GBuffer、HDR/SDR、IBL 探针和 Graph resize，维持原容差并通过同步验证。
- [x] T33 [人工] P4a 里程碑验收。用户确认展示结果。直接光和方向环境光截图与 P3c 的 SHA-256 分别一致，双配置各 83/83。
  - 用户确认迁移前后展示结果与同步诊断证据。

### 静态场景、Bindless 与 GPU 剔除整合

参见 [整合设计](openspec/changes/p4a-static-scene-gpu-driven/design.md)。

- [x] T34 共享几何与实例、材质数据。双配置各 84/84，DSH 独立评审 17/17 通过。
  - 验收：`render-static-scene-draw-data` 核对多网格范围、共享实例、材质引用与无效输入。
- [x] T35 静态场景 Descriptor Set Bindless。双配置各 85/85，验证两材质、回滚和在途保护，DSH 独立评审 18/18 通过。
  - 验收：核对材质槽位、颜色与数据贴图解释、脏槽位更新及在途保护。
- [x] T36 GPU 剔除、Graph Buffer 同步与索引间接绘制。CPU/GPU 可见集合、GBuffer 与 PNG 一致，双配置各 86/86 零警告，DSH 独立评审 27/27 通过。
  - 验收：`sample-static-scene-gpu-driven` 同场景对照 CPU 可见集合与像素，覆盖零可见和容量边界。

### P5 Vulkan 1.4 Descriptor Heap 独立原型

参见 [P5 第一批方案](openspec/changes/p5-descriptor-heap/design.md)。保留 Vulkan 1.3 Descriptor Set 路径；先验证独立原型，通过后再进入第二批同场景 Set/Heap 对照。

- [x] T37 Descriptor Heap 能力扩展、纯 CPU 布局规划与槽位生命周期。真机 RTX 4060（API 1.4.357）实际启用 Heap 并通过全字段规划，`rhi-descriptor-heap-layout` 与双配置 87/87 零告警通过，DSH 独立评审 15/15 PASS。
  - 验收：`rhi-descriptor-heap-layout` 核对步长/对齐/保留区、溢出与超限拒绝、fixture 缺能力/缺 `vkCmdPushDataEXT` 拒绝及槽位退役复用。
- [x] T38 Descriptor Heap 分配、写入、绑定与 Shader 数值读回。真机 RTX 4060（API 1.4.357）完成 VMA 对齐 Heap 分配、写入、绑定与 5 组采样数值读回（区分非零 linear 与默认 nearest），双配置 88/88 零告警、Sync Validation 零错误，DSH 独立评审 16/16 PASS。
  - 验收：`rhi-descriptor-heap-readback`（标签 `gpu`）在真机验证对齐地址、默认槽、多纹理槽与 nearest/linear sampler 读回及 Debug Validation 零错误。
- [x] T39 Descriptor Heap 在途槽位退役与底层图像保护。CPU 受控状态机与真机 RTX 4060（API 1.4.357）三阶段在途退役/底层图像保护/默认槽覆写/递增代数复用及有序关闭全过（106 assertions），双配置 89/89 零告警、Sync Validation 零错误，DSH 独立评审 11/11 PASS。
  - 验收：`rhi-descriptor-heap-retirement` 验证旧句柄立即失效、在途完成前保护槽位与底层 Image、完成后覆写默认描述符并安全复用及有序关闭。
- [x] T40 [人工] P5 第一批独立原型验收。用户核对 T37–T39 真机 RTX 4060（API 1.4.357）数值读回、在途退役保护与 Sync Validation 零错误证据后确认通过，双配置各 89/89。
  - 用户确认真机数值读回、在途退役保护与 Validation 证据，决定进入 P5 第二批（同场景 Set/Heap 对照）。

### 架构适度收口（每帧视图解耦与 render/rhi 边界归位）

参见 [架构收口方案](openspec/changes/render-rhi-boundary-cleanup/design.md)。在 P5 第二批与 P4b 之前完成边界收口，保持单 Vulkan 后端与既有像素容差不变。

- [x] T41 每帧相机视图与静态场景解耦。新增 `rhi::FrameView`、`render::buildFrameView`、`RhiSession::setFrameView` 与 `RuntimeSession::setCamera`，间接路径双帧槽 128B UBO 每帧更新、不重传静态网格/贴图/SSBO（`staticSceneUploadCount == 1`）；`render-frame-view` 与 `sample-camera-view-update`（含直接/间接及 Debug Sync Validation）全过，双配置 91/91 零告警，DSH 独立评审 28/28 PASS。
  - 验收：`render-frame-view`（CPU）与 `sample-camera-view-update`（标签 `gpu`）验证移动/旋转相机不重传静态网格、贴图与 SSBO（`staticSceneUploadCount == 1`），可见集合、Reverse-Z 深度、视图位置与复位像素正确，Debug Sync Validation 零错误。
- [x] T42a 测试验证入口从 RhiSession 主头剥离。新增公开验证头 `rhi/RhiVerification.h` 承接 `verifyGpuCull`、`verifyDescriptorHeap`、`verifyDescriptorHeapRetirement` 及探针结果类型，主头仅保留私有友元转发；`header_rhi_verification` 独立编译探针与负向 concept 断言生效，双配置 91/91 零告警，DSH 独立评审 9/9 PASS。
  - 验收：`verifyGpuCull`、`verifyDescriptorHeap`、`verifyDescriptorHeapRetirement` 及探针结果类型迁至 `rhi/RhiVerification.h`，`public-headers` 与全量用例通过。
- [x] T42b 渲染管线规格与 RenderGraph Pass 编排归位 render。新增 `rhi/PresentationSpec.h` 与 `render/PresentationSpec.h/.cpp` 承接 GBuffer/HDR/Reverse-Z 格式与清屏值、Display push 打包、探针布局及半精度/深度解码，`Presentation.cpp`（1544→932 行）拆为 `Presentation.cpp`/`PresentationPasses.cpp`/`PresentationCapture.cpp`/`PresentationUtil.h` 并移除 RHI 硬编码与手工回退屏障；`render-presentation-spec`、`public-headers`（13/13）与全量真机渲染回归全过，双配置 92/92 零告警，DSH 独立评审 20/20 PASS。
  - 验收：GBuffer/延迟光照/IBL 管线配置、探针解码与 Pass 声明由 `render` 驱动，`rhi` 收窄为设备、资源、绑定与执行原语，既有全部渲染回归维持原容差、零告警零 Validation 错误。
- [ ] T43 [人工] 架构收口里程碑验收
  - 用户确认模块边界、动态相机更新与全量回归证据后，进入 P5 第二批（同场景 Set/Heap 对照）与 P4b（方向光阴影与相机漫游）。

## 人工待办（不阻塞队列）

- 云端 “Engine CPU build and test” 使用 `--no-gpu`，已配置自动触发与 vcpkg 二进制缓存。后续通过对应提交的 GitHub Actions 日志核对 Windows runner 与缓存命中。

- 首次使用自动循环前：修正 Codex CLI 的默认模型（见 [自动运行](docs/workflow/自动运行.md) 的注意事项），设置 `CLAUDE_CODE_GIT_BASH_PATH`，然后用 `--max-tasks 1` 试跑 T01。

## 待办池（建议级，不阻塞）

- T42b 评审观察：RHI 未交叉校验 `depthReverseZ`/`depthClear` 与 `spec.displayExposureEV`/`DeferredLighting.exposureEV`，探针 `sampleStrideBytes` 无独立上界（回读缓冲限 128 MiB 兜底），后续扩展多视图/阴影 Pass 时按需收紧。

- T41 评审观察：`StaticSceneDraw::publish` 先 `publishCompleted` 再替换 `m_active`（T35 遗留顺序）、`packStaticSceneDrawData` 冗余构造每实例数据（T34 遗留），以及 `publishStaticScene` 中 `replaceStaticScene` 与 `setFrameView` 的理论部分提交窗口，后续重构时按需一并整理。

- T39 评审提示：真机堆写失败部分提交、`abandon()` 异常兜底与 `open(enableDescriptorHeap, 1.3)` 显式拒绝当前无独立注入用例，后续按需补充。

- T38 评审提示：超时/设备丢失分支（`markExternalPending`/`markExternalLost`）与 query 上限 64 属异常边界，当前无注入用例；可按需补 `open(enableDescriptorHeap, API 1.3)` 显式拒绝用例。

- T37 评审提示：若后续需要 buffer-only/sampler-less Heap 布局再放开 `imageCount`/`samplerCount >= 1` 限制；`bufferCount == 0` 时调用方不得引用 `bufferBaseOffset`；启用日志文案可补充 push data 入口，T38 按需扩展 fixture 的 `vkGetDeviceProcAddr`。

- T36 评审建议：补缺少间接绘制特性时的直接绘制回退验证，以及显式容量为零的测试。

- T34 评审建议：按后续调用需要补带法线网格的容量边界及无效变换测试。T36 用网格局部包围盒和实例变换计算剔除输入。

- T26 评审建议：按材质覆盖需要补 TEXCOORD_1、MASK、内嵌图片拒绝及更大贴图的用例；评估同源纹理的 CPU 像素复用。T27 的 GPU 纹理身份按采样用途区分。

- T24 评审建议：用重叠几何补 Reverse-Z 遮挡像素验收；增加候选回滚的 GPU 资源释放计数，并核对销毁失败的诊断。

- T23 评审建议：补非单位旋转组合的矩阵断言（绕 Z 90° 的单位四元数），以及保存替换失败时保留旧文件和清理临时文件的用例。

- T16 评审提示：若需稳定覆盖实际 fence 等待，使用可控未完成提交；样例映射变更时同步更新发布诊断标签。

- T14 评审建议：T15 接入纹理复用时核对布局转换源同步范围、材质索引边界和 C++／GLSL 容量单一来源；可补 ready 事件字段断言，T16 按需复用资源计数钩子。

- clang-tidy 需要 compile_commands.json，VS 生成器不产出；P1 后评估用 Ninja 预设跑静态检查。

- CMake 目前必须先由脚本生成 build identity 才能配置；P1 有可执行程序后评估给缺省值，方便直接在 VS 打开。
- `openspec/changes/bootstrap-engine-p0/tasks.md` 是 25KB 流水日志，P0 归档时压成勾选清单。

## 已完成

- **P0 基础工程（2026-11-06 本地完成）**：core 诊断/错误、rhi 加载与能力查询、runtime 组合与回滚；49 个用例，两配置各 49/49。方案与历史记录见 [P0 change](openspec/changes/bootstrap-engine-p0/tasks.md)。未做：云端 CPU 构建首跑、真实 1.4/Heap 验收。
