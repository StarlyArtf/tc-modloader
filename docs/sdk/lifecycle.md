# 游戏生命周期与变化事件

`tc.lifecycle` 是值类型事件服务。它补足旧事件总线只报告底层 Hook 事实、并在 `subject` 中携带
借用指针的局限：新事件只含句柄、帧号、周期、计数和单调序号，回调结束后复制保存仍然安全。

```cpp
static void changed(TCGameLifecycleEventV1* event) {
    if (event->kind == TC_LIFECYCLE_BOARD_ENTERED) {
        // event->board 此时有效，可以立即取 V2/V3 快照。
    }
}

TCGameLifecycleApiV1 lifecycle{};
if (tc::lifecycleService(host, &lifecycle) != TC_SERVICE_OK) return 2;
uint32_t kinds = TC_LIFECYCLE_BOARD_ENTERED |
                 TC_LIFECYCLE_BOARD_LEFT |
                 TC_LIFECYCLE_OBJECTS_CHANGED |
                 TC_LIFECYCLE_SELECTION_CHANGED;
if (lifecycle.subscribe(lifecycle.context, kinds, &changed, nullptr) != TC_LIFECYCLE_OK)
    return 3;
```

## 事件语义

| kind | 时机 | Board 句柄 |
|---|---|---|
| `BOARD_ENTERED` | `level.load` 签发新 Board 后 | 有效，可立即读快照 |
| `BOARD_LEFT` | 离开场景或被新关卡替换并完成作废后 | 已 stale，只用于识别离开的代次 |
| `OBJECTS_CHANGED` | 每帧统一观察点发现结构指纹改变 | 有效 |
| `SELECTION_CHANGED` | 选中 Component/Wire 集合指纹改变 | 有效 |

`OBJECTS_CHANGED` 已在真机上验证：探针在自身事务结束后通过命令总线放一个内置元件，下一帧就收到
**恰好一次**该事件（`game-handle-probe`：`lifecycle objects-changed=1 selection-changed=0`）。
`SELECTION_CHANGED` 目前只有离线覆盖——这个构建还没有已核实的"写选择集"入口，探针无从改选择，
所以它连同"选择集写入"一起列在待办里。

直接从一个关卡加载另一个关卡时，顺序固定为旧 Board `LEFT` → 新 Board `ENTERED`。事件的
`sequence` 在进程内严格递增，可用于合并重复刷新；`engine_frame` 与快照使用同一个 Loader 缓存
帧号。

对象变化指纹只取已核实的稳定 Board 字段（对象数量、元件身份/位置、导线端点/位宽/状态槽），
不会对整个 Nim 记录做逐字节比较，因此仿真内部缓存变化不会制造每帧噪声。选择变化对集合中的
实际 ID 做哈希，不只比较数量，所以“取消一个、再选另一个”也能被发现。

`flags` 说明哪些计数有效：

- `TC_LIFECYCLE_HAS_OBJECT_COUNTS`：`component_count` / `wire_count`；
- `TC_LIFECYCLE_HAS_SELECTION_COUNTS`：两个 selected 计数。

订阅只允许在 `tc_mod_load` 期间，单进程最多 64 个监听器；插件拒载时自动摘除。回调运行在产生
事件的游戏主线程，不得跨 C ABI 抛异常。若要修改游戏，应在回调里提交命令总线请求，而不是直接
调用内部函数。
