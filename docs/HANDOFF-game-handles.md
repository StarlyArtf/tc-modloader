# 游戏对象句柄地基交接

更新时间：2026-09-19

> **接手结论（同日第二轮）**：本文件的四步验收已跑完，并在真机探针里发现并修掉了一处会让
> 整个功能失效的缺陷；文末“接手记录”里还记着随后补上的两项：`SCENE_CHANGE` 事件携带
> `change_scene` 的 context，以及用真实按键（Escape）验证了游戏自身的退出路径。下面“尚未
> 验收与风险”里第 1、2、3 条已结清，第 4、5、6 条仍然有效。

## 本轮目标

为 Mod 与游戏内部对象之间建立有生命周期语义的稳定句柄，优先完成 Board 对象；避免插件把
`void* board_model` 长期保存后，在换关卡或离开场景后继续访问悬空对象。

本轮严格没有尝试签发 Component、Wire 或 Level 句柄。它们只有在后续统一快照能够从可信枚举
结果发现对象后才能安全签发，不能提供“把任意裸指针包装成句柄”的入口。

## 已实现

### 公共 ABI

- 新增 `sdk/tc_handle_api.h`。
- `TCGameHandle` 为 24 字节：`size`、`kind`、`generation`、`token`。
- kind：
  - `TC_GAME_OBJECT_BOARD = 1`：已实现；
  - `COMPONENT = 2`、`WIRE = 3`、`LEVEL = 4`：只预留，当前返回 `TC_HANDLE_ERR_KIND`。
- 结果码：`OK`、`UNAVAILABLE`、`ARGUMENT`、`KIND`、`STALE`。
- `TCHost` 尾部追加三个入口：
  - `get_current_game_handle`
  - `validate_game_handle`
  - `resolve_game_handle`
- C++ 辅助函数：`tc::currentGameHandle`、`tc::validateGameHandle`、
  `tc::resolveGameHandle`，都会先按 `host->size` 检查字段是否存在。
- 新能力位 `TC_CAP_GAME_HANDLES = 1 << 12`，包清单名字为 `game_handles`。

这是尾追加 ABI：旧 `TCHost` 字段偏移没有变化；结构大小从 176 增到 200。ABI 基线现有 213 条
记录，`tools/abi.ps1` 已通过。

### Loader 实现

- 新增 `src/game_handles.hpp` 的 `tc::GameHandles` 注册表。
- 使用互斥锁保护当前 Board 指针、代次和 token。
- `level.load` 的 Loader 自有链节在发事件前调用 `enterBoard()`：提升代次并签发新 Board。
- `scene.change` detour 在发事件前调用 `leaveBoard()`：提升代次并清空 Board。
- 再次加载关卡也会提升代次，因此同一进程里的旧 Board 句柄必然失效。
- `resolve` 会同时检查结构大小、kind、generation、token 和当前对象是否存在；失败时先把输出指针
  清空。
- 为保证生命周期跟踪始终存在，`level.load` Loader 链节和 `scene.change` Hook 现在不再依赖是否
  有 Mod 订阅事件，而是在 NativeRuntime 启动时始终安装。

### 文档与契约

- `src/capabilities.hpp` 已声明 `game_handles`。
- `docs/reference/capabilities.md` 已加入能力表、生命周期、返回值和使用示例。
- `docs/changelog.md` 已记录这次尾追加 ABI。
- `tools/abi-snapshot.cpp` 与 `abi/windows-x64.json` 已覆盖句柄结构、宿主新增字段、kind、错误码和
  能力位。
- `tests/game-handles.cpp` 覆盖签发、解析、错误类型、换 Board 失效和离开场景失效。
- `build.ps1` 已加入 `game-handles-test.exe` 的编译和执行。

## 已验证

以下命令已经成功：

```text
build/game-handles-test.exe
  PASS game handles: issue, resolve, type guard, generation invalidation and scene leave

build/capabilities-test.exe
  PASS host contract: version source of truth, capability table and dependency constraints

tools/abi.ps1
  PASS SDK ABI snapshot: 213 records match abi/windows-x64.json

tests/release.ps1
  PASS release contract

git diff --check
  通过；只有工作树原有的 CRLF/LF 提示
```

完整 fast 构建曾运行到 host-contract：Loader、安装器、示例 Mod 和此前的单元测试均已编译并
执行。它当时只因能力参考文档尚未列出 `game_handles` 而失败。文档补齐后，单独重跑
`capabilities-test.exe` 已通过；由于额度停止线，**没有再次从头运行完整 fast**。

## 尚未验收与风险

1. **必须重跑完整 fast**。虽然失败点已修复并定向通过，但正式结论仍应来自统一测试器。
2. **必须重跑 host 层**。句柄跟踪让 `level.load` 链和 `scene.change` Hook 始终安装，可能影响
   Hook 冲突、链成员日志或事件测试的既有断言。
3. **需要真实游戏生命周期探针**。当前只有注册表单测，没有一个真实 Mod 依次验证：
   - 主菜单查询 Board 返回 `UNAVAILABLE`；
   - 进入关卡得到有效句柄并能解析到事件中的 Board；
   - 离开关卡后旧句柄返回 0/`STALE`；
   - 进入第二关卡得到不同 generation/token。
4. `resolve_game_handle` 是迁移桥，仍会返回临时裸指针。后续快照接口应直接接受句柄，让普通 Mod
   不需要解析裸指针；长期可把解析能力标成高级/不安全接口。
5. `scene.change` 当前在调用游戏原函数之前作废 Board。这个顺序偏安全，但真机测试要确认没有
   Mod 合理依赖 scene-change 事件里读取即将离开的 Board；如果需要，应新增明确的
   `BOARD_CLOSING` 事件，而不是延长所有旧句柄寿命。
6. Component/Wire/Level kind 仅预留，不应在文档或版本说明中宣称已经支持。

## 接手后的第一组命令

```powershell
cd D:\p\tc-modloader

# 1. 完整快速层
powershell -NoProfile -ExecutionPolicy Bypass -File tools/test.ps1 -Tier fast

# 2. 原生宿主、链、事件与安装器
powershell -NoProfile -ExecutionPolicy Bypass -File tools/test.ps1 -Tier host -NoBuild -KeepGoing

# 3. 再确认 ABI 和画像
powershell -NoProfile -ExecutionPolicy Bypass -File tools/abi.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File tools/compat.ps1 -GameDirectory D:\p
```

若 host 层失败，优先检查 `tests/hook-chain.ps1` 对日志中 `Event source level.load` 的旧字符串断言：
实现现在写的是 `Game handle source level.load`，而事件功能仍由同一个 Loader 链节提供。更稳妥的
做法可能是保留原日志名并另加一句句柄跟踪状态，避免无意义破坏测试和用户诊断习惯。

## 建议的真机探针

增加一个只读 `dev.game-handle-probe.mod`：

- `tc_mod_load` 声明 `game_handles` 和 `events`；
- `on_frame` 查询当前 Board，记录 generation/token 和验证结果；
- 订阅 `LEVEL_LOAD`、`SCENE_CHANGE`，保存事件前后的句柄状态；
- 驱动脚本按“主菜单 → 关卡 A → 主菜单 → 关卡 B”运行；
- 日志断言旧句柄失效、新句柄有效且解析指针等于 `LEVEL_LOAD.subject`。

这个探针通过后，Board 句柄才可标记为真机验收；之后才能开始“同帧只读快照”。

## 工作树说明

- 仓库在本轮开始前已经是大量未提交/未跟踪文件的脏工作树。
- 本轮没有创建 Git commit，没有覆盖正式 `dist/releases/0.6.0`。
- 不要用 `git reset --hard` 或整树 checkout；需要按文件审阅并保留用户原有改动。

## 接手记录（2026-09-19 第二轮）

### 已跑完的验收

```text
tools/test.ps1 -Tier fast                     PASS 6/6（含 build）
tools/test.ps1 -Tier host -NoBuild -KeepGoing PASS 4/4（修日志名后）
tools/abi.ps1                                 PASS 213 records
tools/compat.ps1 -GameDirectory D:\p          SUPPORTED tc-win64-2.1.334
```

host 层当初预计的失败点正是日志改名：`hook-chain` 的 `events` 模式断言
`Event source level.load: ok`。按文件里建议的做法改回原名，并**另加**一行
`Game handle tracking level.load: ok`，两边都保留。

### 真机探针发现并修掉的缺陷（重要）

新增 `tests/game-handle-probe.cpp` + `tests/game-handle-probe-driver.hpp` +
`tests/game-handle-probe-playtest.ps1`（游戏层用例 `game-handle-probe`），构建产物是
`dist/dev.game-handle-probe.mod`（只读）与 `dist/dev.game-handle-probe-driver.mod`（自驱动）。
第一次真机运行就证明**当时的实现等于没有实现**：

```text
PROBE: level.load ... frame=233
PROBE: level handle generation=3 token=2 ... valid=1
PROBE: scene.change scene=1 frame=233 old-handle-valid=0        <- 同一帧就被作废
```

游戏在**同一帧**先 `level.load`、再切到棋盘场景（scene=1）；旧实现把这次切换当成“离开”，
于是关卡里每一帧的 `get_current_game_handle` 都返回 `UNAVAILABLE`，句柄只在事件回调那一瞬
有效。修复方式：

- `GameHandles` 记录关卡加载的**引擎帧号**，`scene.change` 的帧号 ≤ 加载帧 +1 时只当作进入，
  不作废也不换代；之后的切换才是离开。
- 帧号取自加载器每帧缓存的 `lastEngineFrame`，不在 detour 里直接调 `igGetFrameCount`
  ——`tests/hook-chain.cpp` 用裸引擎 DLL 播放游戏、从不跑帧循环，直接调用会崩（已实测）。
- `scene.change` 改成在 **`boot()` 里、加载任何插件之前**挂钩：旧实现是等插件都加载完再挂，
  抢到 detour 的插件会让句柄失效机制静默消失（真机日志里就是
  `Event source scene.change: hook failed`）。现在插件对该目标 `create_hook` 会被拒绝并提示
  改用 `TC_EVENT_SCENE_CHANGE`。

修好后同一条探针通过，证据见 `docs/verification.md` 的“游戏对象句柄（0.6.0）”。

### 仍然存在的缺口

1. ~~“真正的关卡退出路径”还没有真机证据~~ **已结清**：关卡里按 Escape 就是玩家的离开动作，
   游戏自己会调用 `change_scene(ctx,0)`，加载器同帧作废句柄。探针现在默认走这条路
   （`DRIVER: self-exit=ok`，脚本硬断言），驱动自造的切换只作为回退分支并在日志里标成
   `self-exit=not-found`。
2. ~~`ui-board-panel` 的“切场景后棋盘停止绘制”退化成 NOTE~~ **已结清**：
   `TC_EVENT_SCENE_CHANGE` 的 `subject` 现在携带游戏自己传给 `change_scene` 的 context，驱动
   用它离开，`board stopped drawing … panel frames=849` 重新成为硬断言。
3. “第二个关卡”仍是用游戏自己的 `load_level` 载入（和 `tests/ui-board-panel-driver.hpp` 同一
   手法）~~，不是从关卡选择界面再点一次~~ **已收口**：探针现在两次都按主页关卡方格进入、
   按 Escape 退出，全程玩家路径。“另一张关卡地图”进不去是**存档解锁状态**，不是探针能力：
   逐条试过五个方格（`TC_HANDLE_PROBE_TRACE=1`），#3/#4 只打开该关卡自己的界面（再按其中的
   按钮也不会 `level.load`），#5 让游戏自己以退出码 0 结束，只有 #2 真正加载关卡——结论记在
   `docs/verification.md` 的句柄一节。入口按调用点 RVA 精确按下，换一份已解锁存档同样成立。
4. 原文件第 4、5、6 条不变：`resolve_game_handle` 仍是迁移桥；`scene.change` 在调用游戏原
   函数**之前**作废（真机已确认监听器看到的就是失效结果）；Component/Wire/Level kind 仍只是
   预留。
5. `docs/changelog.md` 里 0.6.0 的条目已按上面结论更新，但**没有**重新打包发布。
