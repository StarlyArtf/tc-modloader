# 加载器能力（capabilities）

插件能拿到什么，由一份**能力表**回答，而不是由"宿主结构有多大"去猜。整条链只有一张表：

| 位置 | 作用 |
|---|---|
| `sdk/tc_mod_api.h` | 定义能力名字与位（`TC_CAP_*`） |
| `src/capabilities.hpp` | 决定**本构建**真正提供哪些位 |
| `mod.json` 的 `capabilities` | 包声明自己需要哪些名字，扫描时按同一张表校验 |

一个位只有在对应入口**确实实现**时才会被置上——能力位是加载器对插件的承诺，不是
"结构里预留了字段"。`TC_CAP_*` 位与 `mod.json` 里的名字一一对应，拼写相同。

## 插件侧

```cpp
#include "tc_mod.h"

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* host, TCPlugin* plugin) {
    if (host->api_version != TC_MOD_API_VERSION) return 1;
    /* 需要什么就问什么：缺了就直接拒绝启动，不要赌后面能跑通。 */
    if (!tc::hostHas(host, TC_CAP_UI_SLOT)) return 2;
    /* 也可以按引入版本判断（host_version 是加载器构建号）。 */
    if (tc::hostVersion(host) < TC_HOST_VERSION_CODE(0, 6, 0)) return 2;
    /* 把状态写给玩家看：落在 Mods 页，也写进 loader.log。 */
    tc::reportStatus(host, 0, "board panel ready");
    return 0;
}
```

`tc::hostHas()` / `tc::hostCapabilities()` / `tc::hostVersion()` 会先查
`host->size`，因此在**没有** `capabilities` 字段的旧加载器上安全返回 0 / false，
插件不需要自己写 `offsetof` 比较。

## 包侧（mod.json）

```json
{
  "format": 2,
  "id": "author.panel-mod",
  "version": "1.0.0",
  "capabilities": ["status", "ui_slot"],
  "requires": {"author.core-mod": ">=1.2.0"},
  "optional": ["author.extra-mod"]
}
```

扫描时的拒绝是**可读的**，并且发生在加载任何代码之前：

- `Unknown loader capability: X` —— 名字写错了（作者问题）。
- `This loader does not provide the capability: X` —— 这个名字存在，但当前加载器没有
  （加载器比包旧）。

两种情况的区别很重要：前者要改包，后者要升级加载器。

## 表

| 名字 | 位 | 入口 | 说明 |
|---|---|---|---|
| `log` | `TC_CAP_LOG` | `host->log` | 每行带 `[mod-id]` 前缀写进 `loader.log` |
| `symbol` | `TC_CAP_SYMBOL` | `resolve_symbol`、`engine_proc` | 解析游戏 EXE 的 COFF 符号 / 引擎导出 |
| `hook` | `TC_CAP_HOOK` | `create_hook` | 注册 `tc_mod_load` 期间的 Hook |
| `logic` | `TC_CAP_LOGIC` | `register_logic` | 元件行为交给 C++ 回调 |
| `component` | `TC_CAP_COMPONENT` | `register_component` | 声明式元件（引脚＋回调，自动生成定义） |
| `ui_page` | `TC_CAP_UI_PAGE` | `register_ui_page` | 主菜单页面容器 |
| `ui_slot` | `TC_CAP_UI_SLOT` | `register_ui_slot` | 游戏自己的布局里的插槽（板侧栏、工具栏） |
| `texture` | `TC_CAP_TEXTURE` | `create_ui_texture` 等 | 图片 / RGBA 上传与宿主管理的纹理生命周期 |
| `status` | `TC_CAP_STATUS` | `report_status` | 插件自己把状态写给 Mods 页与日志 |
| `symbol_alias` | `TC_CAP_SYMBOL_ALIAS` | `resolve_alias` | 用稳定别名（`sim.do`、`level.load`…）取地址，不再硬编码混淆名，见 [symbols.md](symbols.md) |
| `hook_chain` | `TC_CAP_HOOK_CHAIN` | `register_hook_chain` | 加入加载器拥有的钩子链：多个 Mod 共用同一目标，按优先级／Mod id 排序 |
| `events` | `TC_CAP_EVENTS` | `add_event_listener` | 宿主事件总线：关卡加载、场景切换、仿真命令、保存 |
| `game_handles` | `TC_CAP_GAME_HANDLES` | `get_current_game_handle`、`validate_game_handle`、`resolve_game_handle` | 带代次检查的游戏对象句柄；当前签发 Board 句柄 |

能力位本身自 **0.6.0** 起提供；`ui_page` / `ui_slot` 的引入版本写在
`sdk/tc_mod_api.h` 的注释里。加载器启动时会把自己支持的能力写进日志：

```text
TC Mod Loader 0.6.0 capabilities: log, symbol, hook, logic, component, ui_page, ui_slot, texture, status, symbol_alias, hook_chain
```

## 钩子链与符号别名

这两项一起解决"多个 Mod 想动同一个游戏函数"和"游戏一更新所有插件都失效"：

| 能力 | 你要写的 | 加载器替你做的 |
|---|---|---|
| `symbol_alias` | `tc::resolveAliasAs<SimDo>(host,"sim.do")` | 解析本构建的混淆名；启动时把整张表解析一遍，缺失的写进日志 |
| `hook_chain` | `tc::hook::addSimDo(host,priority,&cb,user)` | 拥有唯一的 detour、按优先级排队、剔除被拒插件的链节、保证原函数只跑一次 |

链节回调拿到 `TCHookCall*`：改 `args` 里的字段就是改游戏看到的参数；返回 0 继续下一个链节，
返回非 0 停止；置 `call->skip_original=1` 则游戏自己的函数不执行（Mod 用它替换行为）。
需要看返回值时先 `call->run_chain(call)` 再返回非 0。完整契约见 `sdk/tc_hook_api.h`，
别名清单见 [symbols.md](symbols.md)。

链上的目标被加载器**保留**：对同一个目标调用 `create_hook` 会被拒绝并提示改用
`register_hook_chain`——这条规则让"两个 Mod 都钩 sim_do"从必然失败变成各自排进同一条链。

## 事件总线

"关卡加载了"、"玩家按了运行/重置"、"游戏保存了"这类事实，过去每个 Mod 都要自己去钩内部
函数。现在加载器自己盯着这些点（钩子链上的 loader 链节恒定在最高优先级），只把结果发给
订阅者：

```cpp
static void onEvent(TCEvent* event) {
    if (tc::events::is(event, TC_EVENT_LEVEL_LOAD))
        boardModel = tc::events::levelBoardModel(event);   // 板模型直接给你
    else if (tc::events::is(event, TC_EVENT_SIM_COMMAND))
        log("run/pause/reset: " + std::to_string(tc::events::simCommand(event)));
}

// tc_mod_load 里：
if (tc::events::subscribe(host, TC_EVENT_LEVEL_LOAD | TC_EVENT_SIM_COMMAND, &onEvent, nullptr) != TC_EVENT_OK) return 2;
```

| 事件 | `flags` | `subject` / `name` | 触发点 |
|---|---|---|---|
| `TC_EVENT_LEVEL_LOAD` | 0 | 板模型 / 关卡名 | `level.load` 链 |
| `TC_EVENT_SCENE_CHANGE` | 目标场景号 | 该次切换的 context | `scene.change` |
| `TC_EVENT_SIM_COMMAND` | 0 run / 1 stop / 2 reset | — | `sim.do` 链 |
| `TC_EVENT_SAVE` | 保存计数 | — | `save.level` |

线程与生命周期：事件在触发它的线程上回调（仿真命令在仿真线程），监听器不得跨边界抛异常、
不得保留事件指针；注册只在 `tc_mod_load` 期间，被拒插件会从总线上摘掉。加载器启动日志会写
清每个事件源的状态（`Event source level.load: ok`、`Event source save armed` …）。

`TC_EVENT_SCENE_CHANGE` 的 `subject` 是**游戏自己传给 `change_scene` 的 context**：想主动切场景
（例如自己退出关卡）的 Mod 把它原样传回 `tc::resolveAliasAs<ChangeScene>(host,"scene.change")`
即可，不必再去钩那个函数。它是借用指针，只能在事件回调或紧随其后的动作里使用，不要跨帧保存
（`tc::events::sceneContext(event)` 是配套的取值函数）。

## 游戏对象句柄

`TCGameHandle` 可以跨帧保存，但不能持久化到磁盘。当前 Loader 只签发
`TC_GAME_OBJECT_BOARD`：关卡加载时生成新代次，下一次关卡加载或任何场景切换都会让旧句柄
失效。`tc::validateGameHandle()` 返回 1 表示有效、0 表示已失效，负值表示参数或宿主错误；
`tc::resolveGameHandle()` 只为有效句柄返回临时的底层 Board 指针。
“离开关卡会失效”这条在真机上按**玩家的动作**验过：关卡里按 Escape，游戏自己调用
`change_scene(ctx,0)`，句柄随之被作废。

```cpp
TCGameHandle board{};
if (tc::currentGameHandle(host, TC_GAME_OBJECT_BOARD, &board) == TC_HANDLE_OK) {
    // 以后使用前先检查；切换关卡后这里会返回 0。
    if (tc::validateGameHandle(host, &board) == 1) {
        const void* raw = nullptr;
        tc::resolveGameHandle(host, &board, &raw);
    }
}
```

`COMPONENT`、`WIRE` 和 `LEVEL` kind 已预留但返回 `TC_HANDLE_ERR_KIND`；它们要等统一快照能够从
可信枚举结果签发，不能让 Mod 把任意裸指针包装成“安全句柄”。

两条真机实测（`tests/game-handle-probe-playtest.ps1`，脚本里记着完整证据）值得单列，因为它们
决定了句柄在关卡内是否可用：

- **进入关卡的那次场景切换不作废句柄**。游戏先 `level.load` 再在同一帧切到棋盘场景，如果把这
  次切换当成“离开”，刚签发的句柄会在同一帧被作废，整个关卡里查询都返回 `UNAVAILABLE`。
  判定用引擎帧号：切换发生在关卡加载的那一帧（或下一帧）算进入，之后的切换才算离开。
- **`scene.change` 由加载器自持**，在加载任何插件之前就挂钩。它同时承担 Board 代次作废与
  `TC_EVENT_SCENE_CHANGE`；插件对该目标调用 `create_hook` 会被拒绝并提示改用事件——否则先
  抢到 detour 的插件会静默关掉句柄失效机制。

## 版本约束

`requires` / `optional` 既接受 id 列表（原形式），也接受 `id -> 约束` 的对象形式。
约束作用于**被启用**的依赖版本：

| 写法 | 含义 |
|---|---|
| `""`、`"*"` | 任意版本 |
| `"1.2.3"`、`"=1.2.3"` | 相等 |
| `"!=1.2.3"` | 不等 |
| `">=1.2.3"`、`">1.2.3"`、`"<=1.2.3"`、`"<1.2.3"` | 比较 |
| `">=1.0.0,<2.0.0"` | 逗号表示同时成立 |

版本号取**开头的点分数字段**比较（`1.10.0 > 1.9.0`，纯字符串比较会弄反），
尾随文字被忽略（`1.0.0-beta` 按 `1.0.0` 比较）；完全没有数字时退回字符串比较。
`optional` 里的包**没被启用**时不做任何检查——"可选"就是这个意思。

校验失败时玩家在 Mods 页看到的是：

```text
某 Mod requires author.core-mod >=1.2.0, but the enabled version is 1.0.0
```

并**没有任何文件被改动**（`apply` 的校验在写入之前完成）。

对应实现：`src/core.hpp` 的 `parse_dependencies` / `parse_capabilities` /
`version_satisfies`，离线用例 `tests/capabilities.cpp`（`build.ps1` 内执行）。
