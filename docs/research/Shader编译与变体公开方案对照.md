# Shader 编译与变体公开方案对照

日期：2026-10-18。状态：调研与设计草案，供用户审阅；不改变已确认的 GLSL → SPIR-V 选型。本轮查看官方文档及公开源码页面，没有安装、编译或基准测试相关引擎。公开 main/master/stable 链接可能变化，实施借鉴前应固定实际版本或提交。

## 1. 参考与适用范围

| 参考 | 已核查的机制 | 对本项目的建议 |
| --- | --- | --- |
| Google Filament | matc 将材质定义编成包含目标 Shader 的材质包；variant-filter 排除确定不用的变体；可输出参数 JSON 元数据 | 借鉴离线产物、变体裁剪和工具可读元数据；不因此引入整套材质语言 |
| O3DE Atom | `.shader` JSON 描述源码与编译元数据，`.shadervariantlist` 列出需要预构建的变体 | 借鉴显式描述与有限变体清单；保留 GLSL，不引入 AZSL/HLSL 编译链 |
| Godot | ShaderRD 区分通用 defines、自定义 defines、变体与组；官方文档区分 Shader 编译和 Pipeline 编译，并提供预编译监控 | 借鉴变体职责划分与编译事件观测；异步特化与 ubershader 作为未来评估项 |
| Khronos Vulkan Samples | 管线依赖 Shader 之外的状态；示例展示 VkPipelineCache 持久化和资源预热 | 分开设计源码编译缓存、管线对象复用与驱动缓存 |

以上“建议”为本项目判断，不声称这些引擎已满足本项目全部缓存、日志或无人值守要求。

## 2. Filament：离线产物与裁剪

官方 [材质文档](https://google.github.io/filament/main/materials.html) 描述 matc 的平台/API 选择、variant-filter、JSON 参数元数据输出及材质包产物。裁掉运行时仍需要的变体可能导致故障，因此裁剪需要覆盖实际使用组合。

[MaterialBuilder 接口](https://github.com/google/filament/blob/main/libs/filamat/include/filamat/MaterialBuilder.h) 还提供 shaderDefine、variantFilter、saveRawVariants 与 generateDebugInfo。

本项目建议：预声明支持组合，在构建阶段生成并验证；诊断时能定位具体变体与实际编译源码。元数据能力需按实际工具核验，不能将 Filament 的材质参数输出直接视为完整 SPIR-V 字段布局反射。

## 3. O3DE：把变体变成显式资产

[Shader Build Pipeline](https://www.docs.o3de.org/docs/atom-guide/dev-guide/shaders/shader-build-pipeline/) 将源码、编译描述、共享 include 和预构建变体清单区分开。其编译链为 AZSL/HLSL 系，不能直接代入本项目 GLSL。

本项目建议：建立一个可审阅的 Shader 描述，包含源文件、stage/入口、编译 profile、合法选项域和需要生成的组合。具体 JSON、TOML、CMake 或其他载体尚未选择，也不要求为每个 GLSL 增加一份重复文件。

显式描述负责对 AI 设边界：未声明的宏、非法枚举值和互斥组合在编译前报错；增加选项需要说明新增组合、接口影响与验证范围。

## 4. Godot：管线阶段与观测

[ShaderRD 源码头文件](https://github.com/godotengine/godot/blob/master/servers/rendering/renderer_rd/shader_rd.h) 可见 general_defines、custom_defines、VariantDefine、变体分组、编译任务和缓存接口。本轮未追踪其所有实现，不能仅依据字段断言完整缓存键或依赖失效行为。

[管线编译官方说明](https://docs.godotengine.org/en/stable/tutorials/performance/pipeline_compilations.html) 区分源码到中间表示，以及驱动创建管线的工作；介绍加载期预编译、ubershader/后台优化机制与按触发来源统计的监控。该说明适用其 Forward+ 与 Mobile 渲染器。

本项目建议：先记录 Shader 编译和 Pipeline 创建的独立耗时、发生阶段和关联标识；对已知管线在场景准备时创建。第一版不要求复制完整异步特化系统；specialization constant 的变化仍可能创建新管线，不能当作每帧任意修改的普通参数。

## 5. 三种缓存职责

依据 [Khronos 管线管理示例](https://docs.vulkan.org/samples/latest/samples/performance/pipeline_cache/README.html)，加上本项目源码缓存草案，建议分开描述：

| 对象 | 关键输入 | 职责 |
| --- | --- | --- |
| 源码编译产物 | 源码/传递 include、规范宏、工具与目标选项 | 复用已验证的 SPIR-V 与关联元数据 |
| 引擎管线对象 | Shader 产物、入口/特化值、布局及相关管线状态 | 复用仍有效的 VkPipeline，管理所有权与销毁 |
| VkPipelineCache | 驱动管理的缓存数据及适用兼容条件 | 辅助减少驱动重复工作；不能代替前两项 |

管线状态按本项目实际 Vulkan 创建方式定义；不因示例采用 VkRenderPass 就预先选定传统 Render Pass。序列化键应编码语义字段，不能直接 hash 含指针、padding 的 CreateInfo 内存。

设备/驱动兼容核验、缓存损坏处理与回退细节留给技术方案；不能把缓存文件视为通用可分发机器码，也不能保证使用 VkPipelineCache 后没有创建耗时。

## 6. 建议的第一版边界

1. 一个统一的编译请求与合法选项描述，复用 Flower 的简洁变体思路。
2. 借鉴 Filament/O3DE：显式列出需要支持的组合，构建期离线编译与验证，不枚举无约束的所有组合。
3. 编译清单与布局/绑定核验结果关联到 SPIR-V 标识；Debug 与优化配置区分。
4. 完整输入缓存、非交互诊断、有界失败处理，沿用现有 Shader 草案。
5. 按实际场景需求创建和复用管线，分别记录编译/创建耗时及缓存状态。
6. 后续按实测需要评估热重载、后台编译、ubershader、共享缓存和材质 DSL；不将这些全部纳入第一版。

建议后续技术方案图明确：描述/变体选择 → 编译与验证 → 产物/元数据 → 管线创建与复用 → 渲染。宏变化、include 变化和管线状态变化分别触发对应层失效。

## 7. 验证状态

本轮只完成资料与接口对照，没有下载整库、运行 Shader 编译或测量性能。所列建议未自动升级为已确认规则；工具版本、描述文件格式、反射库与热重载仍待技术方案。
