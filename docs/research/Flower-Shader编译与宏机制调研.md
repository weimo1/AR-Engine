# Flower Shader 编译与宏机制调研

日期：2026-10-18（Asia/Shanghai）。依据：本机 `F:\cpp project\flower` 的实际源码。状态：只读调研与设计建议，不代表采用其全部实现，也未运行或修改 Flower。

## 1. 实际链路

```text
C++ Pass 创建 ShaderVariant
→ 指定 GLSL 文件和 Shader stage
→ 追加存在型宏及整数/布尔宏
→ ShaderCache 检查模块缓存和磁盘产物
→ 未命中时启动 glslc.exe
→ 读取 SPIR-V，创建 VkShaderModule
→ 创建对应管线
```

实现证据以源代码为准，不用仓库内概括性技术笔记代替实际实现。

## 2. 三类编译宏

[ShaderVariant](<F:/cpp project/flower/source/engine/graphics/shader.h:25>) 保存文件路径、stage、宏集合和整数映射。

| 来源 | 实际行为 | 编译参数/GLSL 形式 |
| --- | --- | --- |
| `setStage` | stage 同时决定编译阶段和注入宏 | `-fshader-stage=vert/frag/comp` 及 `VERTEX_SHADER`、`PIXEL_SHADER`、`COMPUTE_SHADER` |
| `setMacro` | 加入一个存在型宏 | `-DNAME`，GLSL 用 `#ifdef NAME` |
| `setInt` | 名称映射为 int32 值 | `-DNAME=value` |
| `setBool` | bool 转为整数 0/1，名称仍会被定义 | `-DNAME=0/1`，应按值判断，不能把 `#ifdef` 当作布尔真假 |

编译参数在 [shader.cpp](<F:/cpp project/flower/source/engine/graphics/shader.cpp:76>) 拼装。setInt/setBool 的支持已见源码定义；本轮对自有 source 的 `.cpp` 搜索未找到其调用案例，不声称这些接口已在所有实际路径使用和验证。

### 同文件多 Pass

[Bloom C++ 入口](<F:/cpp project/flower/source/engine/renderer/pass/bloom_pass.cpp:52>) 为同一个 `bloom.glsl` 创建两个 Compute 变体，分别定义 `BLOOM_DOWNSAMPLE_PASS` 和 `BLOOM_UPSCALE_PASS`。

GLSL 在 [下采样分支](<F:/cpp project/flower/install/shader/bloom.glsl:25>) 和 [上采样分支](<F:/cpp project/flower/install/shader/bloom.glsl:141>) 内分别声明资源、Push Constants 和 main。它们是两个编译产物，不是运行时依靠宏动态切换。

### 同文件多阶段

[imgui.glsl](<F:/cpp project/flower/install/shader/imgui.glsl:1>) 使用 `VERTEX_SHADER` 与 `PIXEL_SHADER` 分开两个 main，由 C++ 分别以对应 stage 编译。GLSL 版本示例为 460。

## 3. C++ / GLSL 共享头与宏

[common_header.h](<F:/cpp project/flower/install/shader/common_header.h:6>) 用 `__cplusplus` 区分语言环境：C++ 侧定义 glm 类型别名和比较/归档辅助宏，GLSL 侧提供兼容定义，复用一部分结构声明与常量。

这可以减少字段名称和顺序的两份维护，但不是跨语言布局正确性的充分条件。`CHECK_SIZE_GPU_SAFE` 在 C++ 侧仅检查总大小能否被 16 字节整除，不能单独证明每个字段偏移、矩阵/数组 stride 与 SPIR-V 一致。

本项目可评估精简的共享 GPU 数据声明或生成方式；不能原样把场景序列化、业务类型及第三方数学库配置一起带入 Shader 接口。是否用共享头、生成器或独立定义＋反射检查，仍需方案比较。

## 4. 编译与缓存的实际行为

- [编译入口](<F:/cpp project/flower/source/engine/graphics/shader.cpp:48>) 使用配置路径 `tool/glslc.exe`，目标参数写为 Vulkan 1.3；通过 Windows 进程调用在运行时按需编译。
- 编译命令始终加入 `-O`；APP_DEBUG 中可根据配置额外加 `-g`，不能将它概括为 Debug 自动无优化。
- [内存缓存](<F:/cpp project/flower/source/engine/graphics/shader.cpp:160>) 以 variant hash 保存 ShaderModule；[磁盘缓存检查](<F:/cpp project/flower/source/engine/graphics/shader.cpp:58>) 按文件名、编译时种子与 variant hash 形成文件位置，在开关允许且文件存在时跳过编译。
- [variant hash](<F:/cpp project/flower/source/engine/graphics/shader.cpp:202>) 包含绝对路径、stage、宏和值；这一函数没有把源码/include 内容、编译器版本、目标选项与 debug 选项纳入键。
- 宏和值用 unordered 容器遍历后逐步 hash，当前没有规范排序。不能直接将该实现当作跨插入顺序、机器或工具链的稳定内容缓存设计。
- [全部 Pass 更新](<F:/cpp project/flower/source/engine/graphics/pass.cpp:13>) 会等待设备 idle、释放 Pass、删除 Shader 缓存再重新初始化；指定 Pass 更新也有显式重编译入口。本轮未发现这些入口本身构成自动 include 依赖图或增量失效机制。

以上为静态代码观察，未复现具体缓存故障，也未测量编译耗时或运行表现。

## 5. 无人值守时需要改变的处理

[编译失败路径](<F:/cpp project/flower/source/engine/graphics/shader.cpp:69>) 在 while(true) 中编译，失败后弹 MessageBox，再进入重试；等待进程使用 INFINITE。当前调用路径没有将编译器 stdout/stderr 管道捕获为结构化结果。

适合人工边修改边重试的交互，不适合作为本项目 AI/CI 的默认机制。我们需要有界超时、退出状态、完整诊断附件和非交互失败返回；修复输入后再显式重试，不无限等待点击。

路径包含空格时原实现有加引号，但本项目仍应由受控进程启动与参数编码处理，不允许宏名或值注入任意命令行片段。

## 6. 本项目建议

1. 借鉴明确的 ShaderVariant：源码、stage、入口、宏及编译配置形成可检查的编译请求。
2. 支持存在型宏与值宏；布尔值固定 0/1，宏域和合法取值有声明，阶段宏由编译系统生成。
3. 多 Pass/多阶段共文件是组织选项，不强制所有 Shader 合并。声明互斥 Pass 与 stage 条件，禁止非法组合产生多个 main 或错误资源接口。
4. 静态结构差异使用宏/变体；适合特化的常量可以另评估 specialization；每帧变化的数据通过已设计的 GPU 参数传入，避免为了曝光、时间等数据变化重编译。
5. 对宏排序和规范化，拒绝冲突重复定义，统一实际编译参数、日志和缓存键的来源。
6. 缓存采用覆盖实际输入的稳定内容标识与清单，检查传递 include、编译器、目标、选项和产物完整性，不只看文件存在。
7. 编译方法与启动时机分开：构建期、开发手动重编译和未来运行期入口共享编译契约，但本轮不因参考 Flower 自动加入运行时热重载。
8. 记录 variant_id、规范宏列表、源码/依赖及产物 hash、目标与工具版本、缓存命中/失效原因、耗时和诊断；和运行日志/图像证据关联。

以上已加入 Shader 规范草案，尚未将其自动升级为用户已确认设计。未复制 Flower 实现代码进入本项目。
