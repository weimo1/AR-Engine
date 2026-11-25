<!-- Generated from proposal.md; source-sha256: 5f64c54da1161681f704a2073d8044fd5faa5b508f23ca32ac9e788121fb3442. Do not edit. -->

## Purpose

建立零参数桌面主程序 `ar_engine`、6 通道调试视图、Set/Heap 兼容自绘 ImGui 面板、观察预设往返及 Set/Heap × CPU/GPU 剔除四配置基准评测体系。

## ADDED Requirements

### Requirement: 零参数 `ar_engine` 入口与测试驱动解耦

`ar_engine` SHALL 在零参数启动时从 EXE 同级目录加载 `shaders/` 与展示室，默认启用完整延迟 PBR + IBL + 2048 方向光阴影 + 自由相机；仅保留极简自动化/对比覆盖参数，历史 P1–P5 模式参数拆离至 `ar_sample_harness`。

#### Scenario: 零参启动、路径解析与 Set/Heap 展示室等价
- **WHEN** `app-paths-and-showroom-cli` 与 `sample-showroom-launch`、`sample-free-camera-shadows` 校验 `ar_engine` 零模式开关启动与旧开关拒绝
- **THEN** Set/Heap 输出一致、非法旧参数在设备创建前返回退出码 2，全部既有回归保持通过

### Requirement: 平台事件队列、观察预设往返与 ImGui 捕获路由

`platform` SHALL 扩展单一事件泵提供按键、字符、鼠标、滚轮与焦点事件并保持首帧/焦点恢复零跳变；`runtime::ObservationSettings` SHALL 支持原子写入、版本与场景校验、非法预设回滚（保留当前生效设置）。

#### Scenario: 事件状态机、捕获位与观察预设校验
- **WHEN** `platform-input-state` 与 `runtime-observation-settings` 验证 UI 鼠标/键盘捕获屏蔽漫游、失焦/切回视口零跳变及预设保存/重开/损坏回滚
- **THEN** 有效预设完整往返，损坏/错版本/非法字段预设报错并保持原状态不变

### Requirement: 6 通道调试视图、运行期曝光、Set/Heap `OverlayDraw` 与 GPU 时间戳

`render` 与 `rhi` SHALL 支持 6 通道调试视图按需声明 Graph 读取、`setExposureEV` 运行期生效、Set/Heap 双路径 `OverlayDraw`（Heap 零传统描述符对象）与按帧槽非阻塞 GPU 时间戳查询。

#### Scenario: 6 通道视图数值验证、Overlay 绘制与计时回收
- **WHEN** `render-graph-plan`、`rhi-descriptor-heap-scene-layout` 与 `sample-debug-views-and-overlay` 在 Set 与 Heap 路径验证 6 通道像素/方向/深度范围、Overlay 绘制、Resize 恢复与连续多帧 GPU 时间戳回收
- **THEN** 6 通道符合数值参考、关闭阴影时阴影视图不可用、Set/Heap 等价、时间戳单调有效无跨帧错位且 Debug Sync Validation 零错误

### Requirement: CPU AABB 剔除与四配置性能基准（Set/Heap × CPU/GPU 剔除）

`render::StaticSceneVisibility` SHALL 复现 `static_indirect.comp` 局部 AABB 六半空间裁剪；RHI SHALL 支持 CPU 直接写帧槽间接命令/计数并复用 `vkCmdDrawIndexedIndirectCount`、支持基准 `PresentMode` 解除 `FIFO` 锁帧；基准导出 SHALL 输出逐帧与中位数/P95 汇总及完整软硬件元数据。

#### Scenario: CPU/GPU 剔除等价与四配置基准复现
- **WHEN** `render-static-scene-visibility` 与 `app-benchmark-run` 验证旋转/缩放/贴边/零可见与分位统计/帧号联结，`sample-four-config-benchmark-parity` 与 `sample-four-config-benchmark-export` 对照四配置同帧可见 ID 与像素并验证基准 JSON/CSV 导出可解析与汇总可重算
- **THEN** 非溢出场景四配置可见 ID 集合与像素一致、零可见提交 0 绘制、溢出独立标记、导出含有效 CPU/GPU 计时或显式缺测原因且 Debug Sync Validation 零错误
