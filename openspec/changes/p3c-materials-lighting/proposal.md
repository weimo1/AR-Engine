# P3c 材质与基础画面

从 P3b 静态场景推进到可诊断的延迟材质和光照，沿用[阶段路线](../../../docs/实施路线与阶段计划.md)、[渲染模块边界](../../../docs/modules/render/README.md)和[颜色规范](../../../docs/standards/数学坐标与颜色规范.md)。首批为不透明的 metal/roughness 材质。方向光阴影、透明、通用 Render Graph 和 HDR 显示输出留待对应阶段。

<!-- AR:CONTRACT capability=engine-p3c-materials-lighting -->
## Purpose

在固定场景上验证材质输入、GBuffer、直接光、HDR 输出和环境光。

## ADDED Requirements

### Requirement: 材质数据与 GBuffer

资产路径 SHALL 明确材质默认值以及颜色贴图和数据贴图的解释。几何阶段 SHALL 写出可调试的基础颜色、法线、金属度、粗糙度和深度。

#### Scenario: 材质输入对照
- **WHEN** `assets-opaque-material` 加载现有 glTF 与指定材质的固定夹具
- **THEN** 默认值与显式数据符合资产契约，非法输入有明确错误，P3b 资产继续可读

#### Scenario: GBuffer 真机读回
- **WHEN** `sample-gbuffer-probes` 绘制已知表面和清屏区域
- **THEN** 读回能比较表面属性、深度和背景的预期值，遮挡结果和 Debug Vulkan Validation 符合预期

### Requirement: 光照与 SDR 输出

光照 SHALL 在线性浮点中间颜色中计算，输出阶段 SHALL 施加曝光、色调映射并只执行一次显示编码。

#### Scenario: 直接光与曝光
- **WHEN** `sample-hdr-lighting-probes` 对固定光源、材质和两档曝光读回
- **THEN** HDR 读回保留超出 1 的光照值，SDR 输出与参考曲线在声明容差内一致

#### Scenario: 环境预计算与取样
- **WHEN** `sample-ibl-probes` 为固定环境建立漫反射与粗糙度相关的高光输入
- **THEN** 可以核对立方体方向和 Mip/layer 状态，固定表面环境光结果可复现
<!-- /AR:CONTRACT -->
