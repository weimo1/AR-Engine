# 默认展示场景与启动（P4b 前置）

依据 [实施路线与阶段计划](../../../docs/实施路线与阶段计划.md#毕业交付的五项技术方案)、[默认展示场景与启动](../../../docs/毕业交付/默认展示场景与启动.md) 与 [独立运行包与复现](../../../docs/毕业交付/独立运行包与复现.md#代码架构)，本变更先为 `ar_triangle` 补齐自制材质展示室资产、EXE 相邻资源路径解析 `AppPaths`、CMake 增量部署以及 VS F5 默认 `--showroom` 入口。

完成后，干净构建按 F5 即可进入可重复观察 PBR 材质与 IBL 光照的展示室，并为后续 P4b 方向光阴影、离屏立柱投影、自由相机漫游及 P6–P7 面板与运行包提供统一验收场景。

非目标：不修改引擎核心模块依赖方向；本批不实现方向光阴影、自由相机输入、ImGui 面板或 P7 安装包无参双击入口。

<!-- AR:CONTRACT capability=app-showroom-launch -->
## Purpose

为 `ar_triangle` 提供随源码维护的自制材质展示室资产、EXE 相邻只读资源路径解析、CMake 增量部署以及 `--showroom`（VS F5 默认参数）启动模式，保持既有 CLI 测试语义不变并支持 1.3 Set 与 1.4 Descriptor Heap。

## ADDED Requirements

### Requirement: EXE 相邻资源路径解析与展示室 CLI 规则

`ar_triangle` SHALL 通过纯值类型 `AppPaths{shaders, content, output}` 基于可执行文件目录解析只读 `shaders/` 与 `content/` 目录，支持显式 `--output` 覆盖，不依赖当前工作目录定位随包资源；`--showroom` SHALL 自动选择展示室场景、资产根、GPU-Driven 与 Directional IBL，且与 `--static-scene`、`--asset-root`、`--clear-only`、`--set-binding`、`--gpu-driven` 冲突时在创建 Vulkan 设备前返回退出码 2。

#### Scenario: 路径解析、参数冲突矩阵与展示室场景加载校验

- **WHEN** `app-paths-and-showroom-cli` 以纯 CPU 方式校验不同工作目录与输出覆盖下的 `AppPaths` 解析、非法/冲突参数组合拒绝，并用 `StaticSceneSession` 加载随源码维护的展示室 `scene.json`
- **THEN** 路径解析与冲突拒绝符合规则，展示室资产全部通过加载校验，实例 ID 稳定唯一，粗糙度组与金属度组材质参数按递增序排列

### Requirement: 自制材质展示室资产与 CMake 增量部署

仓库 SHALL 在 `examples/triangle/content/showroom/` 维护自制确定性展示室资产（地板、背墙、高大立柱、粗糙度递增组、金属度递增组、初始机位与许可说明），全部采用 `ar_assets` 已验证的不透明 glTF 子集；`examples/triangle/CMakeLists.txt` SHALL 通过带源依赖的构建规则将 `shaders/` 与 `content/showroom/` 部署至 `$<TARGET_FILE_DIR:ar_triangle>`，并配置 `VS_DEBUGGER_COMMAND_ARGUMENTS "--showroom"`，确保仅修改资产后增量构建也会同步更新运行目录。

#### Scenario: 展示室构建部署与独立工作目录启动出图

- **WHEN** `sample-showroom-launch` 从非构建根的独立工作目录启动 `ar_triangle --showroom --hidden --frames 2`，分别运行 1.3 Set 与 1.4 `--descriptor-heap`，并测试显式场景覆盖与缺失资产失败路径
- **THEN** 两种绑定路径均成功出图、PNG 与 GBuffer/HDR 探针一致且体现多材质差异、日志记录实际场景/资产根/绑定路径、Debug Sync Validation 零错误，缺失资产或冲突开关报告明确路径并干净退出
<!-- /AR:CONTRACT -->
