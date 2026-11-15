# 验证计划

| CTest | 输入与观察点 |
| --- | --- |
| `assets-static-gltf` | 子目录内自制 glTF，依赖同目录外部 BIN/PNG；核对多面 POSITION、UV、索引与 RGBA；同路径共享；节点变换、越界引用、坏 accessor 或 PNG 返回具名错误，恢复输入后可重试 |
| `scene-static-roundtrip` | 两实例同资产、不同变换；核对稳定 ID、相机参数、资产共享、实例矩阵及保存重开一致；错误版本、重复 ID 或坏资产不替换活动场景；提取值在原 scene 销毁后仍有效 |
| `sample-static-scene-pixels` | 两实例各检查不对称 PNG 的至少两个纹素，另查背景。重开后复查；注入候选部分上传失败，核对旧画面和资源继续有效、候选清理及重试。像素失败输出 ID、坐标、预期和实际颜色。Debug 检查 Vulkan Validation |

每批先跑对应红测，再走本地双配置完整门禁和独立评审。将真机用例标记为 gpu，云端 CPU 路径继续验证其余用例。此处是计划，尚未运行。
