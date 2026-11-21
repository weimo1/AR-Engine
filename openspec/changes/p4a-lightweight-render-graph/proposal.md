# P4a 轻量 Render Graph

依据[实施路线](../../../docs/实施路线与阶段计划.md)与 [P3c 验收路径](../p3c-materials-lighting/test-plan.md)。把几何、直接光、显示变换及诊断读回迁移到声明式资源访问，沿用 Vulkan Dynamic Rendering 和现有单图形队列。资源别名、多队列、自动并行录制、阴影和 GI 留待各自阶段。

<!-- AR:CONTRACT capability=engine-p4a-lightweight-render-graph -->
## Purpose

从明确的资源和子资源访问生成可测试的执行与同步计划。

## ADDED Requirements

### Requirement: 访问声明与同步计划

每个 pass SHALL 声明资源、子资源范围、读写用途及必要的外部初始和最终状态。计划 SHALL 诊断未初始化读取、范围越界及同一 pass 内不相容的重叠访问。间接或 Bindless 读取 SHALL 提供可保守覆盖的资源集合。

#### Scenario: 纯 CPU 规划

- **WHEN** `render-graph-plan` 提交具有颜色、深度、Shader 读、复制和外部呈现的固定图，以及缺声明和冲突变体
- **THEN** 有效输入得到稳定的逐子资源执行与屏障计划，无效输入返回 pass、资源、子资源和原因

### Requirement: 迁移后保持画面与资源寿命

执行器 SHALL 在声明的访问前落实同步，记录外部和持久资源的最终状态，并在帧槽完成后复用或释放资源。迁移 SHALL 保持 P3c 已验收的输出。

#### Scenario: 真机材质与环境光回归

- **WHEN** 几何、光照、SDR 输出、诊断读回和呈现改由图驱动并执行 `sample-gbuffer-probes`、`sample-hdr-lighting-probes` 和 `sample-ibl-probes`
- **THEN** 像素与数值仍符合原容差，Debug 同步验证无错误，resize 后使用新帧槽附件
<!-- /AR:CONTRACT -->
