# ADR-0006：Windows 构建采用 Visual Studio 2026

日期：2026-10-18（Asia/Shanghai）。状态：用户已明确采用 VS 2026 构建；尚未创建或构建引擎工程。

## 决策

用户要求“我们应该是用 vs2026 来构建”。Windows 开发与构建基线采用 Visual Studio 2026 的 MSVC 工具链。不得因普通终端 PATH 未加载 cl 就改用独立 LLVM 或其他 VS 版本。

建议具体入口为 CMake + Visual Studio 18 2026 generator + MSBuild；用户在 IDE 打开生成工程，AI 用 cmake --build 执行同一套工程定义。该入口最初随本次修正提出，后于 2026-10-19 连同 CTest、vcpkg 等基础选型获得用户确认，见 [ADR-0007](0007-基础构建与第三方依赖选型.md)；整个构建规范的其他细节仍保持其实际草案状态。

## 本机只读核查

- vswhere：Visual Studio Community 2026，显示版本 18.7.1，安装版本 18.7.11911.148，非预发布。
- 安装目录：`D:/Program Files/Microsoft Visual Studio/18/Community`。
- 默认工具集目录：14.51.36231；x64 cl 文件版本：19.51.36248.0。同时存在较旧 14.38.33130，实际构建需固定选择，不按“目录存在”混用。
- MSBuild.exe 存在，文件版本 18.7.8.30822。
- Windows SDK Include 目录存在 10.0.22621.0、10.0.26100.0；未据此验证 SDK 的全部组件或选定最终版本。
- 本机 CMake 4.2.1 的 help 列出 Visual Studio 18 2026 generator。

## 实施前需要解决的具体问题

本机 CMake 4.2.1 的 `Modules/Compiler/MSVC-CXX.cmake` 将 CXX_STANDARD=23 映射到 `-std:c++latest`；本机 cl /? 的标准选项输出也仅列 c++14、c++17、c++20、c++latest，没有列出 c++23 独立模式。这是帮助/模块静态核查，不代表已实际验证全部标准开关或 C++23 功能。

现有项目明确禁止依赖浮动 latest，因此仅写 CXX_STANDARD 23 不足以满足要求。基础建设时必须核实固定 MSVC/CMake 组合的真实开关、功能探针及完整编译命令；如需固定工具集下使用特定预览模式，应提出具体基线修订供用户审阅，不能静默放宽规则或换编译器。本次不修改 C++23 语言目标。

## 参考与验证边界

[CMake 4.2 的 VS 2026 generator](https://cmake.org/cmake/help/v4.2/generator/Visual%20Studio%2018%202026.html) 说明该 generator 自 4.2 加入，默认 v145，可显式选择架构、安装实例和工具集。

本轮仅运行安装发现、文件/版本读取、CMake help 和 cl /?，没有编译、链接、安装、升级或修改全局环境。
