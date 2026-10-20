# ADR-0002：Shader 采用 GLSL → SPIR-V

状态：语言、中间表示及 glslc、spirv-val、spirv-dis 选型已确认；版本、目标与工程集成待验证。原决策日期：2026-10-18；状态同步：2026-10-20（Asia/Shanghai）。

后续状态补充（2026-10-19）：glslc、spirv-val、spirv-dis 已由 [ADR-0007](0007-基础构建与第三方依赖选型.md) 确认。下文保留当时决策与待定项的历史记录；具体版本、目标和集成仍需验证。

## 背景与决策

用户明确选择“glsl spir”。结合上一轮关于 Shader 语言与 SPIR-V 编译路线的讨论，记录为：使用 GLSL 编写 Shader，编译为面向 Vulkan 的 SPIR-V。

本记录不把 GLSL 与 SPIR-V 当作两种竞争的源码语言。也不批准自动改用 HLSL/Slang。

## 历史待定项及后续承接

- GLSL 版本、面向 Vulkan 的编译目标、SPIR-V 版本与实际能力要求。
- 编译工具已由 ADR-0007 确认，固定版本与构建接入方式仍需验证；不重新打开语言和工具选型。
- include、宏、变体、优化/调试配置、缓存键与错误定位。
- SPIR-V 验证、反射及 CPU/Shader 数据布局核验方式。
- 第一版的构建期/运行期编译范围；本决定不自动增加热重载功能。

编译成功及 SPIR-V 验证通过不证明渲染算法、绑定数据或画面符合需求；须有实际场景和适用检查。

确认来源：本聊天中用户“glsl spir 使用 rever z”的选择；没有稳定消息链接，记录原话及语境。未下载编译器或创建 Shader 构建工程。

参考：[Khronos：What is SPIR-V](https://docs.vulkan.org/guide/latest/what_is_spirv.html)、[Vulkan SPIR-V 环境](https://docs.vulkan.org/spec/latest/appendices/spirvenv.html)。实际版本与 capability 仍需核对，不以“Vulkan 1.4”代替所有功能支持检查。
