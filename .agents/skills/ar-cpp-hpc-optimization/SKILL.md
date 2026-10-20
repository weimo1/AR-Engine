---
name: ar-cpp-hpc-optimization
description: >-
  Profile and optimize AR Engine C++ data paths, culling, scene traversal,
  asset processing, memory use and CPU/GPU pipelines. Use for performance
  investigation or performance-sensitive design, not ordinary formatting or
  unrelated changes.
license: CC-BY-NC-SA-4.0
---

# 性能优化：AR Engine 采用版

先读 [项目 AI 技能约束](../../../docs/standards/AI技能约束.md) 和已确认的性能/语义契约。C++ 控制代码按项目规范处理；需要时读取 `$ar-cpp-oop-style`，不自动要求原版 OOP 模式。

## 优化闭环

1. 明确目标负载、输入规模与分布、硬件、允许误差、确定性、内存及帧时间要求。未测量指标不要写成已达成。
2. 比较实际规模下的算法复杂度与数据表示，保留易理解的参考路径或可信基线；参考路径本身也要有正确性依据。
3. 先做端到端 profiling，再确定热点。记录构建、驱动、数据集、预热、采样和诊断配置；正式性能运行排除验证/捕获开销，正确性运行独立保留。
4. 用工作量、数据传输、缓存、分配、同步与批量提交解释瓶颈。每次改变一个主要因素，保留没有收益或回退的结果。
5. 按证据选择减少工作、数据布局、预分配、批处理、向量化、并行或流水线。平台特定 SIMD/线程/阈值需检查支持与边界；不把某台设备上的结果写成通用定律。
6. 对照参考结果、边界和适用容差验证，再比较相同条件下的整体收益与卡顿。不能仅凭微基准提升宣布引擎更快。
7. 发现根因在已确认架构/接口之外时，先提交包含收益、迁移代价和风险的方案，由用户确认后扩大修改。优化不授权更改范围、精度或质量门槛。

## 参考路由

只读对应领域：

- profiling、基准和成本模型：`references/profiling-and-cost-model.md`。
- 布局、缓存和稀疏数据：`references/data-layout-and-memory.md`、`references/ragged-topology.md`。
- 分配与内存资源：`references/allocation-and-memory-resources.md`。
- 精度与量化：`references/numerics-and-quantization.md`。
- SIMD 与编译器：`references/simd-and-compiler.md`。
- 多核调度和流水线：`references/parallelism-and-pipelines.md`。
- 加速器与传输：`references/accelerator-throughput.md`，其中 CUDA 示例不是 Vulkan 契约。
- 热点多态与冷/热边界：`references/hotpath-polymorphism.md`，设计仍服从项目基线。
- 教学案例：`references/parallel101-case-studies.md`，按 `references/parallel101/provenance.tsv` 核对出处和适用性，不能直接复制为生产实现。

性能实验使用任务独占输出、受控规模与运行预算，适用时读取 `$ar-e2e-side-effect-safety`。不自动运行 `scripts/sync_parallel101_corpus.py`；它更新教学语料，不是开发或验证前置步骤。

原版入口保留在 `references/upstream-skill.md`，LICENSE 和语料来源保留。
