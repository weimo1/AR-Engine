# 实施顺序

- [x] 1.1 T37 扩展 Heap 能力字段与设备入口检查，实现纯 CPU 布局规划与槽位生命周期（`rhi-descriptor-heap-layout`）。双配置 87/87 通过，DSH 独立评审 15/15 PASS。
- [ ] 1.2 T38 实现 VMA 对齐 Heap 分配、描述符写入、管线映射绑定与 Compute Shader 数值读回（`rhi-descriptor-heap-readback`）。
- [ ] 1.3 T39 实现 Heap 在途槽位退役、底层图像保护与有序关闭回滚（`rhi-descriptor-heap-retirement`）。
- [ ] 1.4 T40 [人工] P5 第一批独立原型验收，确认后进入第二批同场景 Set/Heap 对照。
