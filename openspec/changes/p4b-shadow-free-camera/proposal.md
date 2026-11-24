# P4b：方向光阴影与相机漫游

依据 [实施路线与阶段计划](../../../docs/实施路线与阶段计划.md#毕业交付的五项技术方案)、[默认展示场景与启动](../../../docs/毕业交付/默认展示场景与启动.md)、[render 长期方案](../../../docs/modules/render/README.md#数据寿命与扩展边界)、[platform 长期方案](../../../docs/modules/platform/README.md) 与 [数学坐标与颜色规范](../../../docs/standards/数学坐标与颜色规范.md)，本变更在不破坏既有基线与单向依赖边界的前提下，完成世界空间方向光正交 Shadow Map（1.3 Set 与 1.4 Descriptor Heap 双路径等价）与自由相机漫游。

非目标：不引入级联阴影（CSM）、点光/聚光阴影、ImGui 面板或 P6–P7 四配置基准框架。

<!-- AR:CONTRACT capability=engine-render-shadow-free-camera -->
## Purpose

为静态场景管线接入单张场景包围盒正交 Shadow Map（同步支持 Vulkan 1.3 Descriptor Set 与 Vulkan 1.4 Descriptor Heap）及自由相机漫游，确保阴影独立于主相机可见性列表、光源与相机运行期更新零场景重传、初始姿态与优先级契约明确。

## ADDED Requirements

### Requirement: 方向光优先级、完整正交阴影公式与 RenderGraph 同步

`scene::StaticScene` SHALL 支持可选 `directional_light` 解析与往返保存并兼容缺省旧场景；`render` SHALL 以显式光相机与 `orthoRH_ZO` 近远面公式拟合包围盒 8 角点至 `[0, 1]` 阴影深度（多方向及垂直光无退化，固定场景/光源下相机移动矩阵不变），扩展 `FrameView::viewToWorld` 为 4×4 `inverse(view)`，明确场景字段与 `--shadows`/`cast_shadows` 四象限优先级，并在 `PresentationGraph` 新增 `Shadow` Pass 与 `ShadowDepth` 同步声明。

#### Scenario: 多方向阴影数学、优先级、场景往返与 Graph 屏障校验

- **WHEN** `render-directional-shadow` 验证多方向（含斜射与垂直光）8 角点投影至 NDC `[-1,1]^2×[0,1]`、非法输入拒绝、4×4 `viewToWorld` 逆校验、四象限光源优先级、场景往返与 `PresentationGraph` 开关阴影屏障
- **THEN** 全部合法角点落入预期范围且相机位姿变化不改变阴影矩阵，非法参数被拒绝，开关阴影分别产出精确屏障与零额外屏障

### Requirement: 纯 CPU 输入状态机、窗口接线与零滚转自由相机控制器

`platform` SHALL 将首帧、失焦、右键沿触发、模式切换与滚轮累计收口为可注入事件的纯 CPU 输入状态机，并由 `Window::sampleInput()` / `setCursorMode()` 接线（隐藏窗口不抢占光标）；`render::FreeCameraController` SHALL 接受任意合法单位四元数，零滚转输入往返一致、带滚转输入首次接管收敛为零滚转（保持前向），处理 WASD/QE、对角归一化、±89° 俯仰钳制、Shift 加速、滚轮调速与非法 `dt` 容错。

#### Scenario: 输入状态机、隐藏窗口接线与自由相机控制器校验

- **WHEN** `platform-input-state` 注入首帧/失焦/右键/模式切换/滚轮序列，`platform-window-input` 在可用桌面窗口环境下核对隐藏窗口接线，`render-free-camera` 核对各轴位移、零滚转往返与带滚转收敛
- **THEN** 状态机与控制器全部边界断言通过，零滚转往返与带滚转收敛行为明确，隐藏窗口不抢占系统光标

### Requirement: RHI 阴影 Pass、上限分批间接绘制与 Set/Heap 双路径等价

`rhi` SHALL 在 `DeferredLighting` 中以双帧槽 240B `LightingFrame` UBO 承载光照与阴影参数，在 1.3 Set 与 1.4 Descriptor Heap（271 图像/10 缓冲/4 采样器含槽 3 比较采样器）下同步实现 Direct 与独立全场景 Indirect（按 `maxDrawIndirectCount` 分批提交、启用 `drawIndirectFirstInstance`、关闭前重置外部阴影槽）阴影深度 Pass 与 PCF 采样。

#### Scenario: 同场景方向光阴影在 Set 与 Heap 路径下的像素与探针对照

- **WHEN** `sample-directional-shadow-parity` 覆盖 Direct/Indirect、1.3 Set/1.4 Heap、受光/阴影/关闭阴影参考、离屏遮挡体投影、运行期 `setDirectionalLight`（含更新矩阵/UBO/Graph）与 Resize，并单测小上限分批提交
- **THEN** 阴影区与受光区差异显著、离屏遮挡体正常投影、`staticSceneUploadCount == 1`，Set 与 Heap 探针及 PNG 一致，Debug `VK_VALIDATION_VALIDATE_SYNC=1` 零错误

### Requirement: 交互式自由相机漫游与展示室 CLI 集成

`runtime::RuntimeSession` SHALL 暴露窗口输入采样、光标模式切换、当前活动相机查询与 `setDirectionalLight`（未开阴影管线时拒绝 `castShadows == true`）；`ar_triangle` SHALL 提供 `--shadows`、`--shadow-resolution` 与 `--free-camera`（与 `--hidden` 冲突退出码 2），支持在默认展示室中零场景重传漫游。

#### Scenario: 展示室多步漫游与阴影 Set/Heap 端到端验证

- **WHEN** `sample-free-camera-shadows` 在展示室/确定性场景上以 `FreeCameraController` 驱动多步位姿（含离屏遮挡体持续投影与复位），分别运行 1.3 Set 与 1.4 Heap 并核对 CLI 冲突参数拒绝
- **THEN** 固定世界点受光一致、画面随位姿更新、`staticSceneUploadCount == 1`，Set 与 Heap 逐帧像素/探针一致，非法组合返回退出码 2，Debug Sync Validation 零错误
<!-- /AR:CONTRACT -->
