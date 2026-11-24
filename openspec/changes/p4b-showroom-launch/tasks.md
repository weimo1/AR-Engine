# 实施顺序

- [ ] 1.1 T49 实现应用路径与启动参数解析模块 `examples/triangle/AppPaths.h/.cpp`、创建自制材质展示室资产 `examples/triangle/content/showroom/`（地板、背墙、立柱、粗糙度组、金属度组、许可说明），并在 `examples/triangle/CMakeLists.txt` 接入 `shaders/` 与 `content/showroom/` 的增量依赖部署及 `VS_DEBUGGER_COMMAND_ARGUMENTS "--showroom"`（`app-paths-and-showroom-cli`）。
- [ ] 1.2 T50 将 `AppPaths` 与 `--showroom` 接入 `examples/triangle/main.cpp`（兼容 1.3 Set、1.4 `--descriptor-heap` 与全部既有参数，记录实际场景/资产根/绑定路径，显式处理缺失路径与失败收尾），完成独立工作目录双路径出图、探针与失败路径验收（`sample-showroom-launch`）。
