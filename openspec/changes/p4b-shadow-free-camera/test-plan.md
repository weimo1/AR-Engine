# P4b 验证计划

| 批次 | 用例 | CTest 标签 | 证据与覆盖 |
| --- | --- | --- | --- |
| T51 | `render-directional-shadow` | CPU（无 `gpu`） | 验证完整光相机与 `orthoRH_ZO` 近远面公式在多方向光（斜射多方位、正午 `(0,-1,0)`、仰射 `(0,1,0)`）下将 8 角点全部投至 NDC `[-1,1]^2 × [0,1]` 且近点深度严格小于远点；验证固定包围盒与光方向下多步主相机位姿变化不改变 `lightViewProj`；验证非法光方向/负强度/越界分辨率拒绝、`FrameView::viewToWorld` 4×4 逆校验、四象限光源优先级、含/不含 `directional_light` 的 `StaticScene` 往返，以及 `PresentationGraph` 开关阴影的 `ShadowDepth` Sync2 屏障 |
| T51 | `render-free-camera` | CPU（无 `gpu`） | 验证 `FreeCameraController` 的 WASD/QE 位移、对角归一化、右键偏航/俯仰方向、`±89°` 俯仰钳制、Shift 加速、滚轮速度边界、负/NaN `dt` 容错；**分别验证零滚转初始四元数往返一致，以及带滚转初始四元数首次接管收敛为零滚转（保持主前向且右轴水平）**与 `reset` |
| T51 | `platform-input-state` | CPU（无 `gpu`、纯计算） | 注入合成事件验证 `WindowInputTracker` 的首帧零跳变、失焦抑制与恢复焦点首帧零跳变、右键初按沿零跳变、`CursorMode` 切换首帧零跳变、多步光标/滚轮累积与 `consume()` 清零 |
| T51 | `platform-window-input` | CPU（无 `gpu`，依赖可用桌面窗口环境） | 与 `platform-window-create` 同属窗口环境组，在隐藏窗口下核对 `Window::sampleInput()` / `setCursorMode(Normal)` 真实 GLFW 回调接线及不抢占系统光标 |
| T52 | `rhi-descriptor-heap-scene-layout`（更新） | CPU（无 `gpu`） | 核对 271 图像槽（含 269..270 阴影槽）+ 10 缓冲槽（含 8..9 `LightingFrame` UBO）+ 4 采样器槽布局规划、超限拒绝、`RequireProduct` 失败映射，以及小上限 `maxDrawIndirectCount`（如 2 批分 5 实例）的间接阴影分批逻辑 |
| T52 | `sample-directional-shadow-parity` | `gpu` | 真机开启 `VK_VALIDATION_VALIDATE_SYNC=1`，在确定性“接收地面 + 画面内遮挡体 + 视锥外遮挡体”场景下覆盖 Direct/Indirect 与 1.3 Set/1.4 Heap，断言受光/阴影/关闭阴影参考 HDR 探针差异显著、离屏遮挡体正常投影、运行期 `setDirectionalLight` 更新矩阵/UBO/Graph 且 `staticSceneUploadCount == 1`（未开阴影管线时拒绝 `castShadows == true`）、Swapchain Resize 阴影稳定、Set 与 Heap 探针及 PNG 一致、Debug Sync Validation 零错误 |
| T53 | `sample-free-camera-shadows` | `gpu` | 真机开启 `VK_VALIDATION_VALIDATE_SYNC=1`，在展示室/确定性场景通过 `FreeCameraController` 驱动多步位移与转向轨迹（含遮挡体移出主相机视锥后落影仍在可见地面上、多机位核对同一固定世界点受光并观察边缘走样/闪烁、复位），对照 1.3 Set 与 1.4 Heap 逐帧像素/探针一致且 `staticSceneUploadCount == 1`；同时通过 `ar_triangle` CLI 验证 `--shadows` / `--shadow-resolution` / `--free-camera` 及 `--free-camera --hidden` 返回退出码 2 |

说明：
- 新增用例按约定登记到 `workflow/p0-checks.json` 的 `expected_cases`，真机用例标 `LABELS gpu` 并登记到 `gpu_cases`。
- 既有全部基线用例必须保持全绿、双配置零警告。
