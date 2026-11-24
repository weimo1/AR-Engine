# 实施顺序

- [x] 1.1 T44 扩展 `DescriptorHeapTable`（UBO/外部附件槽/多固定 Sampler）并接入直接 Bindless GBuffer Heap 路径、`RequireProduct` 窗口呈现与细分失败映射（`rhi-descriptor-heap-scene-layout`、`sample-static-scene-heap-gbuffer`）。
- [x] 1.2 T45 将 GPU-Driven 间接绘制与延迟光照/IBL/Display 全链路接入 Descriptor Heap，落实外部槽安全更新协议并覆盖 Resize、运行期环境替换与非溢出/溢出对照口径（`sample-static-scene-heap-gpu-driven`）。
- [x] 1.3 T46 为 `ar_triangle` 补齐 `--descriptor-heap` 入口并建立端到端 Set/Heap 同场景对照验证脚本（`sample-descriptor-heap-scene-parity`）。
- [x] 1.4 T47 [人工] P5 第二批（同场景 Set vs. Heap 对照）里程碑验收，确认后进入 P4b（方向光阴影与相机漫游）。
