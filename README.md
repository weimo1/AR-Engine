# AR Engine

个人实时三维渲染引擎，用于图形技术学习、画面展示与毕业设计。先完成可独立运行、可调试和可验证的渲染能力，再逐步加入轻量编辑器。

## 项目范围

目标包括场景与相机、glTF静态模型、PBR材质、直接与环境光照、阴影和后处理。先实现Descriptor Set/Indexing Bindless，再按设备与工具能力验证Descriptor Heap路径。游戏玩法、物理、网络和脚本系统目前不在范围内。

完整产品依据见 [需求](渲染引擎需求.txt)。AI负责开发执行；需求、架构和关键技术方案由用户确认。

## 技术基线

C++23、Vulkan1.4、Windows/VS2026；CMake/MSBuild/CTest和vcpkg。Vulkan内存使用VMA，数学使用GLM，诊断使用spdlog，Shader采用GLSL→SPIR-V。全部已确认选型及范围见 [ADR](docs/adr/)。

## 当前进度

- 本地AI工作流收尾版本已交付，具备任务上下文、DSH接入、测试、独立审查、gate及Git/CI检查。
- P0目录和模块职责已确认，公开头使用include/<module>/，不增加ar路径层。
- P0仅准备目录骨架和方案；链接产物、标准模式和CRT仍待确认，尚无引擎实现或渲染画面验收。

P0的 [技术方案](openspec/changes/bootstrap-engine-p0/design.md)、[验收计划](openspec/changes/bootstrap-engine-p0/test-plan.md) 和 [当前事实与缺口](docs/workflow/当前决策与待办清单.md) 分别维护，不把规划完成当实现完成。

## 代码组织

| 位置 | 职责 |
| --- | --- |
| engine/core、rhi、runtime | P0基础能力、图形底层与模块组合 |
| engine/platform、assets、scene、render | 后续窗口、资产、场景及渲染模块，随实际功能建立 |
| editor、examples | 后续独立编辑器与渲染示例 |
| tools/probe、tests、cmake | 诊断探针、引擎测试及构建配置 |
| docs、openspec | 项目依据、规范和当前能力的方案/验收/任务 |
| scripts、workflow、.agents | 已有AI工作流实现、策略和技能 |

模块的公开头在include/<module>/，实现和私有头在src/。模块目录、构建目标和DLL组织分别决定。

## 开始协作

- 了解项目：[文档索引](docs/README.md)。
- AI进入任务：[AGENTS.md](AGENTS.md)。
- 分配角色、准备对话：[多Agent协作](docs/workflow/多Agent协作与上下文.md)。
- 恢复工具与执行检查：[SDD与CodeReview接入指南](docs/workflow/SDD与CodeReview接入指南.md)。

开发按“明确任务 → 实现 → 按影响检查 → 报告结果”推进；独立 Review 按任务需要及用户安排执行，提交与 CI 按实际授权处理。用户协调单人写入，不要求写包、writer 权限登记或流程 gate。新能力先确认方案，小修引用已有方案；可选工具见接入指南。

仓库：[weimo1/AR-Engine](https://github.com/weimo1/AR-Engine)。package.json只管理工作流工具，引擎依赖使用vcpkg。历史与外部参考见 [归档](docs/archive/README.md)。
