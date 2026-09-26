# 事务、撤销与保存

`tc.transactions` 把多条命令先暂存，再在一个 Loader 执行阶段连续运行。它提供三项关键保证：

1. `begin` 绑定一个 Board 代次并记录结构指纹；
2. 执行前一次性复核 Board 仍有效且结构未被别的操作改变，否则以 `CONFLICT` 结束、一步不跑；
3. 通过预检后，已暂存命令不会和其他 Mod 的命令交错，可选在全部成功后调用游戏原生保存。

```cpp
TCTransactionApiV1 transactions{};
if (tc::transactionService(host, &transactions) != TC_SERVICE_OK) return 2;

uint64_t id = 0;
if (transactions.begin(transactions.context, &board,
                       TC_TRANSACTION_SAVE_ON_COMMIT, &id) != TC_TRANSACTION_OK)
    return 3;

TCCommandV1 stop{sizeof(stop), TC_COMMAND_SIM_STOP, 0, 0, board, 0};
if (transactions.stage(transactions.context, id, &stop) != TC_TRANSACTION_OK) {
    transactions.abort(transactions.context, id);
    return 4;
}
if (transactions.commit(transactions.context, id) != TC_TRANSACTION_OK) return 5;
```

提交是异步的。在后续 `on_frame` 用 `get_status` 观察：

| 状态 | 含义 |
|---|---|
| `OPEN` | 仍可 stage / abort |
| `QUEUED` | 等待本帧回调结束 |
| `RUNNING` | 已通过冲突预检，连续执行中 |
| `COMMITTED` | 全部 staged 命令及可选 Save 成功 |
| `FAILED` | 某一步执行失败；`completed_count` 是此前成功的步数 |
| `ABORTED` | 调用方取消或插件拒载 |
| `CONFLICT` | Board 已 stale，或 begin 后结构指纹被改动；没有执行任何一步 |

## 这里的“原子”边界

V1 保证**原子预检与不交错执行**，不声称能回滚任意游戏副作用。游戏没有面向 Mod 的通用内存
事务 API；在部分命令成功后伪造内存快照恢复，反而会破坏 Nim 所有权和内部缓存。因此 FAILED
会诚实返回 `completed_count`，调用方可提交 `TC_COMMAND_BOARD_UNDO` 做补偿；由游戏原生 undo
历史决定能否撤销。`SAVE_ON_COMMIT` 只在此前步骤全部成功后执行，失败事务不会主动保存。

每个事务最多 32 个显式步骤，每个 Mod 最多 16 个 OPEN/QUEUED/RUNNING 事务，全局有界保留
256 条记录。API 只允许游戏主线程调用，transaction id 按提交 Mod 隔离。V1 已把以后新增 Board
编辑命令需要的冲突检测、归属、批处理和保存屏障固定下来。

## V2：承载带载荷的命令

`TCTransactionApiV2` 的前缀与 V1 相同，只把 `stage` 换成接受 `TCCommandV2`。这样"放置元件"这类
需要载荷的编辑步骤也能进事务：

```cpp
TCTransactionApiV2 transactions{};
if (tc::transactionService(host, &transactions) != TC_SERVICE_OK) return 2;

uint64_t id = 0;
if (transactions.begin(transactions.context, &board, TC_TRANSACTION_SAVE_ON_COMMIT, &id) != TC_TRANSACTION_OK)
    return 3;
TCCommandV2 place{};
place.size = sizeof(place);
place.type = TC_COMMAND_BOARD_PLACE_COMPONENT;
place.subject = board;
place.custom_prototype_id = my_component_id;
place.kind = 0x4e;
place.x = 12; place.y = -4;
if (transactions.stage(transactions.context, id, &place) != TC_TRANSACTION_OK) return 4;
if (transactions.commit(transactions.context, id) != TC_TRANSACTION_OK) return 5;
```

冲突语义与 V1 完全一致：begin 记录 Board 结构指纹，commit 时若指纹变了就 `CONFLICT`、一步不跑。
这意味着"先读快照决定放哪里、再提交"这类 Mod 不会被别人的编辑挤掉。
