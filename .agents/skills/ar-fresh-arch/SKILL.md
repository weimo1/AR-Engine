---
name: ar-fresh-arch
description: >-
  Reassess an existing AR Engine module from confirmed requirements when
  repeated patches or new constraints expose a design mismatch. Use for
  architecture reviews and substantial refactoring proposals, not routine fixes
  or automatic replacement of an approved architecture.
license: CC-BY-NC-SA-4.0
---

# 既有架构重审

先读 [项目 AI 技能约束](../../../docs/standards/AI技能约束.md)。从需求推导可以帮助发现过时设计，但现有代码、兼容性、资产格式与用户已确认决策都是需要评估的真实约束。

1. 阅读需求、架构、ADR、技术方案与相关实际代码，分清当前事实、仍有效约束、待验证假设。
2. 说明新增/改变的需求及现有设计不匹配的证据。不因代码存在就默认保留，也不因“重新思考”就默认推翻。
3. 比较局部修复、适度结构调整和重新设计，明确迁移、测试、性能、维护成本与回退。
4. 按影响提供模块关系、核心类型、时序或状态图及契约；原型实验不自动成为正式架构。
5. 形成技术方案或新 ADR，将范围/架构/关键方案变化交用户审阅。此前有效确认已覆盖的局部实现无需重复确认。
6. 仅在现有授权与确认边界内实施。等待新设计确认时继续独立工作，不提前大规模删除或重写代码。

用户只要求评估/审查时，保持只读。没有既有代码的新项目使用正常架构设计流程，不机械套用迁移重审。

原版入口在 `references/upstream-skill.md`，其忽略既有边界的倾向不覆盖项目确认机制。
