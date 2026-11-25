# 实施顺序

- [x] 1.1 主程序零参数收口为 `ar_engine`（VS 默认启动项、清空参数、更新启动文档），历史 P1–P5 专项回归驱动拆分至 `ar_sample_harness`（`app-paths-and-showroom-cli`、`sample-showroom-launch`、`sample-free-camera-shadows`）。
- [x] 1.2 接入纯 `"imgui"`、`platform::Window` 单一事件泵与 UI 捕获隔离、`runtime::ObservationSettings` 原子预设往返与回滚校验（`platform-input-state`、`runtime-observation-settings`）。
- [x] 1.3 落地 6 通道调试视图、运行期曝光、Set/Heap 双路径 `OverlayDraw`（Heap Slot 271）与每帧槽非阻塞 GPU 时间戳查询，并接入 `ar_engine` `[F1]` `DebugPanel`（`render-presentation-spec`、`render-graph-plan`、`rhi-descriptor-heap-scene-layout`、`sample-debug-views-and-overlay`）。
- [x] 1.4 落地 `render::StaticSceneVisibility` CPU AABB 视锥剔除、CPU/GPU 统一 `vkCmdDrawIndexedIndirectCount` 间接绘制消费、`PresentMode`（`Fifo / Mailbox / Immediate`）与帧号联结计时（`render-static-scene-visibility`、`sample-four-config-benchmark-parity`）。
- [x] 1.5 落地 `BenchmarkRun`（`ar_engine --benchmark <SPEC_JSON>`）、低/中/高三档确定性展示室材质网格场景生成、四配置批量驱动与 `frames.csv` / `summary.json` 导出及独立重算校验（`app-benchmark-run`、`sample-four-config-benchmark-export`）。
- [x] 1.6 P6 双配置完整门禁（Debug + RelWithDebInfo 110/110 零警告）、RTX 4060 优化版四配置三档实测与 DSH 独立代码评审（`2a6e2a2d` 77/77 PASS）。
