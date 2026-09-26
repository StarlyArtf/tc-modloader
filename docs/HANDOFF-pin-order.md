# 接手：左侧 IO 面板的引脚顺序（`tc.pin_order` + `local.pin-order`）

## 现状

- **Loader 侧**：`TC_SERVICE_PIN_ORDER`（`src/pin_order.hpp`）把"左侧面板显示什么、按什么顺序"
  做成通用接口；`src/loader.cpp` 在 `build_io_state_view` 入口（RVA `0x45ea20`）应用顺序。
  服务表与 `sdk/tc_pin_order.h` 的用法见 [sdk/services.md](sdk/services.md#tcpin_order左侧-io-面板的引脚顺序)。
- **Mod 侧**：`examples/pin-order/plugin.cpp` 画边框 + 每条左边一对 ▲/▼ 小按钮（0.4.0 起，
  拖动已删除）：左键点一下移动一格，右键打开"移动 N 格"窗口，Ctrl+左键恢复该组原顺序。
  `local.pin-order` 已部署进 `D:\p\mods` 并在 `state.json` 里启用。
- **证据**：fast 层 `tests/pin-order.ps1`；game 层 `tests/pin-order-playtest.ps1`
  （`-Probe` / `-Drag` / `-Dump`，其中 `-Drag` 现在点的是按钮）。真机一次 `-Drag`（`byte_adder`）
  得到 `Order: 2,3,4 -> 3,2,4`；截图 `build/pin-order-out/btn1.png`、`geo3.png`、`drag3.png`。
- **面板缓存的重建也归 Loader**（2026-09-21）：`get_io_states` 全 EXE 只有一个调用者
  （`load_level_frontend`，0x315d50–0x317be0 里的 0x316bc6），所以三段缓存只在关卡加载时
  重建——工坊里改名或增删引脚都不会刷新，这是"要退出再进工坊"的根因，也是"新引脚没有框"的
  根因（面板画的是旧缓存）。加载器现在自己钩 `reload_this_custom_prototype`（0x2e8cd0），在
  `build_io_state_view` 入口先用游戏自己的 `get_io_states` + 托管 copy/destroy 重建缓存，
  **然后**才 `store().apply(context)`，因此面板与 Mod 看到的是同一份记录、顺序作用在刚建好的
  缓存上。`local.pin-names-patch` 不再钩这两个目标（它只留预览侧的 `igText` 与
  `ImDrawList_AddRectFilled`），否则会被 MinHook 的 `MH_ERROR_ALREADY_CREATED` 整包拒绝。
  日志证据：`IO panel armed (cache rebuild after a prototype reload + tc.pin_order)`，改名后
  `IO panel cache rebuilt (a custom prototype was reloaded)`。
- **诊断（`TC_MODLOADER_PIN_ORDER_LOG=1`）**：每帧汇总一次
  `pin order: summary | inputs cache=N rows=N frames=N | …`（只在变化时打印），并报出面板里
  首次出现、Mod 不认识的锚点偏移（`panel anchor +0x… is not handled`）。"某个引脚的框不见了"
  从此可以分成三类读：缓存条数少了、锚点没认出来、框被下一条目或裁剪吃掉。
  `tests/live-root-workshop.ps1` 现在有 `-PinLog 1|dump` 参数（默认 `1`），实机跑一次就能把
  这些行和截图对照着看；`docs/reference/diagnostics.md` 里是逐行含义表。

## 实测出来的事实（改代码前先看这些）

1. **面板缓存的形状**（`TC_MODLOADER_PIN_ORDER_LOG=dump` 打在 loader.log）：
   context `+0xda30/+0xda40/+0xda50` 各是 `{count, payload}`；payload `+0x00` 再放一次条数，
   条目从 `+0x08` 开始，每条 32 字节：`+0x00` 元件下标（顺序用的 key）、`+0x08` 位宽、
   `+0x10` 名字长度、`+0x18` 名字数据块（数据块头部还有 8 字节容量，字符从 +8 开始）。
2. **面板的锚点**（`igSetCursorPos` 的调用点，都在 `build_io_state_view` 内）：
   - `+0x6d4` / `+0x1e6c`：输入条目的标签行（两个点、同一行，参数是**行中心**，x 是居中后的 x）
   - `+0x1836` / `+0x189d`：输出条目的标签行
   - `+0x21a9` / `+0x28c8` / `+0x295d` / `+0x3257`：位方块，每个方块一次（同一条目里 Y 相同，
     比标签低约 35 px @120 px 节奏）
   - 标量 `igSetCursorPosY` 的 `+0xd92`（输入）/`+0x1de1`（输出）：下一条目的行；
     `+0x1680` 是"输出状态"标题那一行
   实测节奏：`byte_adder` 输入 3 条 + 输出 2 条，label 67/187/307 与 480/600（1:1 屏幕像素）。
3. **边框几何**（`examples/pin-order/plugin.cpp` 的 `rowFrame`）：以标签行为基准，上沿取“半行”；
   bit 锚点是控件的左上角，不能把“半行”当成控件高度。2–8 bit 的四个调用点发生在游戏画完
   对应控件之后；单 bit 走另一分支，最终控件矩形在每条结束的 `igSetCursorPosY` 锚点仍是
   `LastItem`。Mod 在各自的后置锚点用 `igGetItemRectSize` 读取真实高度，下沿至少到
   `bit top + item height + 4 px`。自定义自动位宽的缓存值是 `0`，与字面值 `1` 同属单 bit；
   宽引脚的数值行仍按面板节奏纳入；
   另有 `bandEnd`（标量锚点）兜底——当别的 Mod 把条目撑高（例如打孔纸带往宽输入里画纸带）时，
   下一条目会被推远，边框跟着变高，并且不会盖住下一条目。新增的 `tc.pin_order` V2 不再只靠
   这条推断：内部控件通过 `include_bounds` 把 `{frame, group, key}` 的真实屏幕矩形交给 Loader，
   外框用 `bounds` 取并集；因此最后一条没有“下一条”时也能包含纸带、掩码和数值框。

## 已知限制 / 下一步

- **元件工坊的第三组**：`tc.pin_order` 已经能读第三组（内存/寄存器），但 Mod 还没画它们的把手
  （那一组的锚点调用点在 `+0x1000` 之后，未测量）。现在不用再靠猜：`TC_MODLOADER_PIN_ORDER_LOG=1`
  会把面板里每个没被认出来的锚点偏移打一行 `panel anchor +0x… is not handled`，进工坊看一眼
  日志就能拿到那一组的调用点。
- **持久化**：顺序目前活在 Loader 进程里（按 key 序列，换关卡自动失效）。要跨重开游戏保留，
  需要把 key 列表写进 plugin-data，并在面板出现时 `set_order`。
- **边缘自动滚动**：条目很多时按钮可能就是"下一格"，不需要滚动；如果以后要"一次移动很多格"
  跨出可视区，才需要补自动滚动（右键窗口的 N 格移动已经覆盖了多数场合）。
- **与打孔纸带共用锚点**：已经修好（2026-09-21）。`igSetCursorPos` / `igSetCursorPosY` 现在是
  加载器的钩子链点（`TC_HOOK_SET_CURSOR_POS(_Y)`，带 `caller` 与可改的 `x/y`），打孔纸带
  （优先级 0）负责把纸带下方整体推下去，本 Mod（优先级 100）从链里读锚点画把手。两者不再
  抢钩子；纸带撑高的条目，把手也跟着变高（`rowFrame` 里的 `bandEndY` 分支）。
