# 打孔纸带与宽位常量即时更新交接

更新时间：2026-09-21

> **接手结论**：`local.punch-tape` 的界面、数值持久化和运行时更新通道已经实现，真机验收也
> 已完成。此前约 1 秒的延迟并不是 ImGui 或点击处理慢，而是每次点击都会让 73.2k 门的整张电路
> 重新编译。当前实现把 9–64 位常量改写为运行时读取，并由纸带点击直接更新值槽，正常路径不再
> 请求重编译。
>
> **真机验收已通过**（2026-09-20，在玩家自己那块 `sandbox`／RV32I 电路上，见下面「真机验收
> 结果」）：源码改写、游戏编译通过、关卡跑起来、三次更新 **0.001–0.004 ms** 且不产生新的编译、
> 以及**电路真的按新值计算**（从 `#SIMULATION_STATE` 回读值与纸带写入值一致）都有证据。

## 用户要的最终行为

- 仅为原版不会显示纸带的宽位常量补 UI；8 位及以下继续使用原版交互。
- 0.8.0 同时为元件工坊左侧的宽位输入设置补纸带；8 位及以下仍由原版绘制。0.8.1 将侧栏限制为
  每行最多 2 个 8 位组，让宽位输入优先向下增加行数；纸带允许缩到 0.04 倍以适应窄栏，点击直接调用游戏原生
  `flip_component_global_input`。真机 333 px 宽侧栏中的 32 位输入为 2 行 × 2 组且完整可见。
- **0.8.4 起，侧栏纸带保持孔位大小，改为把下面的内容顶下去**：侧栏
  （`build_io_state_view`）每画完一项就把下一项锚回自己那套固定节奏（`90 × 游戏 UI 缩放`，
  实测 333 px 侧栏在 606 px 高窗口里是 121 px），一项的高度既挤不开下一项，也推不动后面的
  内容——纸带画在“标签”和“原版数值框”之间，多出来的高度于是压到下一项（“输出状态”标题）上。
  0.8.4 只在面板内每次 `igSetCursorPosY`（两个小节标题的 advance、输入循环的每项锚点、输出
  列表自己的锚点）上加“纸带高 + 画布下 6 px”，于是下一项、标题和整段输出一起下移，而
  `节奏 − 标签 − 数值框`（实测 121 − 18 − 58 = 45 px）这个原版间隙原样保留，因为加的量正好
  是纸带新占的高度。锚点用地址范围识别（返回地址落在该函数内；本构建该函数 0x3fc0 字节，
  符号表里下一个符号即其结束），面板起点用 `igBeginChild_Str`（返回地址 `+0x394`）清零，
  输入取值调用点仍是 `+0x779`。中途试过“把纸带缩到本项剩余高度”，玩家否决（孔太小），
  不要再往那个方向调。行距（1/4 孔位）由 `makeLayout` 的 `preferred` 参数带进布局。
  用例：`tests/punch-tape-layout.cpp` 的侧栏小节；真机：
  `tests/punchcard-playtest.ps1 -PunchTape`（日志 `workshop entry pushed down by 44 px`）。
- **0.8.5：纸带按“可见内容宽度”排布，避开面板滚动条**：0.8.4 的顶下去会让面板在内容超高时
  出现竖向滚动条，而 ImGui 的滚动条画在窗口内部，纸带若按 `windowWidth()` 居中就会被它盖住
  最右一列。现在先问 `igGetScrollMaxY()`（引擎导出）是否在滚动，是则用
  `igGetContentRegionAvail()` 的右边界（= 光标 X + 可用宽）作为可用宽度并重新居中；不滚动时
  仍用整窗宽度，正常情况孔位不受影响。侧栏的字节组间距同时由抽屉的 28/44 收到孔位的 2/5
  （通过 `makeLayout` 的 `preferred`，抽屉不变）。调试开关
  `TC_MODLOADER_PUNCH_TAPE_SCROLLBAR=<px>` 可以在不滚动的面板上模拟出滚动条占位，用来验证这
  条路径（实测 18 px 时 `grid=295x36 cell=1639`）。`igGetContentRegionAvail` 与
  `igGetScrollMaxY` 用 `resolve_symbol` 单独解析（SDK 的 `tc_ui.h` 没有包这两个），解析不到
  时退回整窗宽度并写日志。
- **0.9.0：掩码工具**（`Shift/Ctrl+点击`、按住拖动刷位、纸带下方「掩码」按钮 → 弹出工具窗）。
  掩码与"选位模式"按元件 id 记忆（`maskStates`），工具窗里有置1/清0/翻转/保留/清除、全选/清空/
  反选、表达式框（走 `tc.io_value` 的 `evaluate`）、撤销与关闭。写入：
  - **常量** → `tc.io_value.write_constant`（设置值 + 运行时值槽 + 刷新）；老加载器退回
    `set_setting` + `setDynamicConstant`/`upgrade(...,0x30)` + `sim_stop_and_refresh`。
  - **输入** → `flip_component_global_input` 按位翻转（与原生位方块、纸带单击同一入口）。
    **不要**改成服务的 `write_input`：实测（`symphony_2_io`，2026-09-21）
    `set_component_global_input` 写的是"周期重置消费的控制回放"（`ctl_input_replay_reset`），
    而面板绘制的值来自 `get_component_global_input` 读的 `simulation_input_replay`，要等仿真再次
    采样才一致；翻转路径正是原生 UI 里立刻生效的那条。
  - 侧栏把掩码行高度加进 0.8.4 的锚点增长里（`slotPendingGrowth = gridH + 6 + maskRow`），且行高
    用 `itemRectSize()` 量真实按钮高度——固定值会让原版数值框压住按钮底边（已踩过）。
  - 验收：`tests/punchcard-playtest.ps1 -PunchTape -Mask "set:0x100"` 断言
    `mask selftest … applied=1 service=1`；同日志的下一帧 `readback/match` 只作参考（该关卡自带
    测试会重新施加输入）。调试开关 `TC_MODLOADER_PUNCH_TAPE_MASK="<op>:<表达式>"`。
- 纸带使用游戏自己的 `asset/io_state/io_state.png`，包含普通、悬停和按下三种 atlas 行。
- 纸带单元为 44 px，单元间隔 5 px，每 8 位一组，组间隔 28 px，最多每行 4 组。
- 面板太窄时先按完整 8 位组换行，不优先缩小单元。
- 原版“标签/常量值”控件留在左侧，纸带从约 680 px 的控制区右侧再留 32 px 开始；空间足够时
  纸带仍按整个面板居中。
- 点击纸带必须同时更新设置值、元件实际输出和显示；大电路上不应再出现约 1 秒的整板编译停顿。

主要布局常量在 `examples/punch-tape/plugin.cpp`：

```cpp
static constexpr TapeLayout kPreferredLayout = tc_tape::kPreferred;  // 44/5/28/24/11，每行最多 4 组
static constexpr float kNativeControlsRight = 680.f;
static constexpr float kControlsTapeGap = 32.f;
```

2026-09-21 起，纸带的**尺寸**不再固定：几何算术搬到了
[`examples/punch-tape/tape_layout.hpp`](../examples/punch-tape/tape_layout.hpp)，由
`makeLayout(width, 可用宽, 可用高)` 按抽屉实际剩下的空间决定换行与缩放——整组换行优先，行数尽量
填满（不留半截行），然后整条纸带缩放进这块空间，最大放大到 1.5 倍、最小缩到 0.25 倍；插件用
`tc::ui::windowWidth()/windowHeight()` 读抽屉自己的尺寸，所以换窗口大小、换分辨率都会跟着变。
离线用例 `tests/punch-tape-layout.ps1`（fast 层）覆盖这些规则，
`tools/preview-punch-tape.cpp` 可以在不开游戏的情况下把某个窗口尺寸下的纸带画成 BMP 看效果：

```powershell
& build\preview-punch-tape.exe build\preview.bmp 2555 345 18,64
```

**用户已在 2026-09-21 看过预览并确认这个改法合理**（"可以这个改动我觉得合理"）。随后 0.8.3
加入工坊左侧宽位输入纸带、把侧栏限制为每行最多两组，并利用高侧栏的额外空间放大孔位；现已装进
`D:\p\mods`（包 SHA-256
`1903CD763A7F035DBD600D195A867E405F18F77F85B679C1B55F50A77BB6D5BC`）。0.8.3 还把纵向行距
限制为孔位的 1/4，避免最后一行与“输出状态”标题重叠。以后调
尺寸上限只改 `tape_layout.hpp` 里的 `kMaxScale`（现在是 1.5）；若用户改主意想要"每行固定 4 组、
只做整体缩放"，把 `makeLayout` 的 `pass` 循环去掉、直接用 `widest` 一个候选即可。

## 延迟根因

原来的点击链路是：

1. 调 `set_setting(board, 0, componentIndex, value)` 写入元件设置；
2. 调 `upgrade(presenterContext + 0x1a3b8, 0x30)`；
3. 调 `sim_stop_and_refresh(board)`；
4. 游戏把常量作为字面量重新生成代码，并重新编译整张电路。

`set_setting` 本身只改组件记录，实际模拟器仍使用编译时写死的常量。`upgrade(..., 0x30)` 能让实际
输出正确，但代价是整板重编译。用户的截图显示约 73.2k 门，实测体感约 1 秒，所以继续调整刷新
调用顺序无法解决问题。

旧生成源码的关键形态是：

```text
// 89 com_constant 64 ...
let value_id316 = (10 & 0xffffffffffffffff)
...
var vid316 = U64 (10 & 0xffffffffffffffff)
```

因此修复必须发生在 codegen 层，而不是 UI 层。

## 当前实现

### 1. 加载器：将宽位常量字面量改为运行时读取

`src/native_logic.hpp` 已扩展现有的代码生成钩子：

- `circuit(...)` 扫描扁平元件序列；
- 识别 kind `0x2e`、位宽 `9..64` 的常量；
- 从记录 `+0x08` 读取元件唯一 id，从 `+0xa8/+0xb0` 读取设置序列，从 `+0xe0` 读取位宽；
- 将刷新阶段的 `let value_id...` 和周期阶段的 `var vid...` 都改写为：

```text
U64 game_engine.'tc_dynamic_constant'(U64 <component-id>, U64 <fallback>)
```

- 在游戏编译器的 foreign table 注册 `tc_dynamic_constant`；
- 只要生成源码含 `game_engine.'tc_`，就在源码前加 `extern windows_x64 game_engine`；
- 导出 `tc_dynamic_constant_set(componentId, value)` 给原生 Mod 更新运行时镜像。

运行时值当前保存在：

```cpp
std::unordered_map<uint64_t, uint64_t> dynamicConstants;
std::mutex dynamicConstantMutex;
```

### 2. Mod：持久化设置，但跳过重编译

`examples/punch-tape/plugin.cpp` 的点击路径现在是：

1. `set_setting(...)`：继续写游戏自己的 `settings[0]`，保证保存、面板文本和之后的编译都一致；
2. `tc_dynamic_constant_set(componentId, updated)`：立即更新已编译模拟器读取的值；
3. `sim_stop_and_refresh(board)`：刷新现有模拟器；
4. **不调用** `upgrade(..., 0x30)`，所以不触发整板重编译。

如果加载器没有 `tc_dynamic_constant_set` 导出，Mod 会回退到旧的 `upgrade(..., 0x30)` 路径，保持
兼容而不是让实际输出停留在旧值。

注意：`TCHost::engine_proc` 查询的是重命名后的原版引擎 `tc_game_engine.dll`，不是代理加载器自身。
因此 Mod 必须用：

```cpp
GetModuleHandleW(L"game_engine.dll");
GetProcAddress(..., "tc_dynamic_constant_set");
```

这里已经踩过一次坑；不要改回 `host->engine_proc(...)`，否则会静默进入重编译回退路径。

## 改动文件

| 文件 | 内容 |
|---|---|
| `examples/punch-tape/plugin.cpp` | 原生样式纸带、面板空间测量、点击持久化、运行时值槽更新、旧加载器回退；`drawGrid` 里另加一行画布屏幕坐标日志（`punch tape: geometry …`，验收脚本靠它算鼠标落点） |
| `examples/punch-tape/tape_layout.hpp` | 纸带几何：整组换行、行填满优先、按面板剩余宽高缩放（0.25–1.5 倍） |
| `examples/punch-tape/mod.json` | Mod 版本（当前 `0.7.3`）与即时更新说明 |
| `src/native_logic.hpp` | 宽位常量扫描、源码改写、foreign bridge、导出 setter |
| `tests/native-component.cpp` | 刷新/周期两阶段改写及运行时映射离线测试 |
| `tests/punch-tape-playtest.ps1` | 真机验收：导入玩家存档、加载含宽位常量的关卡、断言源码改写/无重编译/状态回读 |
| `tests/punch-tape-runtime-driver.cpp` | 验收用的进程内驱动器（读生成源码、跑更新序列、回读 `#SIMULATION_STATE`） |
| `tests/punch-tape-layout.cpp` / `.ps1` | 纸带几何的离线用例（fast 层） |
| `tools/preview-punch-tape.cpp` | 按给定窗口/抽屉尺寸把纸带几何画成 BMP，供不开游戏时查看 |
| `build.ps1` | 打孔纸带构建说明更新；原有 Mod 构建与打包步骤继续使用 |

## 已验证

### 编译与离线测试

以下测试已通过：

```text
PASS declarative API guards, 8x8 definition, invalid shapes, cross-word payloads,
collector ordering, emission phases and runtime constants
```

- `tests/native-component.cpp` 同时断言 refresh/cycle 两类常量行会变成 `tc_dynamic_constant` 调用；
- `setDynamicConstant/getDynamicConstant` 的设置、命中和 fallback 已有断言；
- `examples/punch-tape/plugin.cpp` 使用 `-Wall -Wextra -Werror` 编译通过；
- 新加载器编译通过；
- `objdump -p` 确认 DLL 导出 `tc_dynamic_constant_set`；
- `git diff --check` 通过。

### 安装状态

已部署：

| 项目 | 路径 | SHA-256 |
|---|---|---|
| 新加载器 | `D:\p\game_engine.dll` | `E01E6769BC753493A50BE6AB483056CA5BEF396A975BA285E5AA5BC87F149163` |
| Mod 0.7.0（运行时值槽，第一版） | `dist\local.punch-tape-0.7.0.mod` | `286A42A1D3847C500540F6550BC5DC6861D35A2C84B782158265491342D25DFB` |
| Mod 0.7.2（当前部署：加纸带画布坐标日志） | `D:\p\mods\local.punch-tape.mod` | `444EEEF1083D180914A91DB0476E4FBA7EFD4B1885233DAC16C5D2EC70F1559F` |
| 旧加载器备份 | `dist\tc-loader-before-runtime-constants.dll` | 回滚用 |

**注意（2026-09-20 晚发现）**：`build.ps1` 重新构建出的加载器与 22:45 部署的那份**字节不同**
（同样大小 4560165、导出表逐项相同，但哈希是 `D2CACFFAE4957B9A898295EFA8BD8C28075A401747FA8358AE5247BEE4D69E08`）。
上面那次验收用的是新构建的 `dist\tc-loader.dll`（`D2CACF…`），所以新产物本身也验证过；但“同一份源码
两次构建得到不同二进制”这件事与仓库“可复现构建”的承诺不符，需要单独查一次（怀疑是构建脚本里的时间戳
固定点或手工编译与脚本编译的差异）。已经部署在 `D:\p\game_engine.dll` 的仍是验证过的 `E01E…`，两者的
行为在本次验收里一致。

启用集合保持为：

```text
local.punch-tape
local.wire-palette
local.word-watchee-64
```

游戏已经用新文件重新启动。`tc-modloader-data/loader.log` 已出现：

```text
[local.punch-tape] punch tape: runtime wide constants enabled
[local.punch-tape] punch tape: constant drawer hook installed
```

这证明新版 Mod 找到了加载器导出，但还不证明关卡内 codegen 和点击链路已经跑通。

## 真机验收结果（2026-09-20，已完成）

用例：`tests/punch-tape-playtest.ps1`。它把玩家自己的存档目录复制进沙盒，让游戏加载
`sandbox` 关卡（也就是那块出现 1 秒停顿的 RV32I 电路），再由 `tests/punch-tape-runtime-driver.cpp`
在进程内做三件事：读生成源码、跑一遍纸带的更新序列、再从仿真状态里回读电路实际算出的值。

```powershell
cd D:\p\tc-modloader
.\tests\punch-tape-playtest.ps1            # 默认 -Level sandbox -Profile "$env:APPDATA\Turing Complete"
```

通过的证据（`build\punch-tape-playtest-96b43bfb82044cbb81a55ac66e398b13\game\tc-modloader-data\loader.log`，
这一轮用的是 `build.ps1` 自己产出的加载器与 Mod）：

| 要证明的事 | 证据 |
|---|---|
| 宽位常量真的改写成运行时读取 | 生成源码里 refresh 与 cycle 各 6 行：`let value_id292 = U32 game_engine.'tc_dynamic_constant'(U64 6342300742968977027, U64 4)` 与对应的 `var vid292 = ...` |
| 游戏的编译器接受这份源码 | `byte-adder: autotest loaded level sandbox` → `autotest finished cycle=39 verdict=0`（关卡跑完且自带测试通过），期间没有编译错误 |
| 点击路径不再重编译 | 三次更新 `took=0.001~0.008 ms`，而 `PUNCHDRV: dumps before=3 after=3`（每次整板编译会多写两个源码 dump 文件） |
| **电路真的按新值计算** | 生成源码把常量写到 `store(#SIMULATION_STATE + 332, U64 (value_id292))`；更新后 `PUNCHDRV: state at 332 = 1515870810 expected 1515870810 MATCH` |
| 纸带在真实电路上照常显示、真实鼠标点击有效 | 更早一轮（同样在玩家的 `sandbox` 电路上）日志抓到两笔真人点击：`drawer component #89 bit 3 -> value 11`、`bit 12 -> value 4107`（数值与逐位翻转一致），并留下带抽屉的截图 `frame.png` |

补充说明（接手时不要误判）：

1. 第 4 条是这次新增的关键证据。驱动器把常量在生成源码里的 `#SIMULATION_STATE` 偏移解析出来，再用
   加载器自己的仿真读服务（游戏自己的 `sim_state_read_u64`）读回，所以它不是“看界面数字变了”，
   而是“电路算出来的值就是纸带写入的值”。
2. 驱动器**没有**把合成鼠标点击送进纸带：`realclick bit=...: the tape never reported a click`。
   日志里那两笔点击来自坐在电脑前的玩家（他自己点的），这也说明真实鼠标路径是通的。驱动器里那段
   `clickSurfacePoint` 目前只做到“把光标放到正确格子”，落点/激活还需要再查（见下）。
3. 抽屉是用 `context+0xe90`（底部面板模式字节，`build_bottom_panel` 里非零即画元件说明面板）和
   `context+0xe48`（说明面板用的元件下标）打开的；`+0xe48` 会被游戏自己的状态覆盖，所以驱动器只
   用它保证“抽屉开着”，不保证抽屉里是哪一个常量（日志里驱动选中的是 #55，抽屉画的是 #89）。
4. 合成点击没落地这件事**不影响**上面的结论：更新序列、无重编译、状态回读都不依赖鼠标。

## 已知风险与后续优化

1. **高频读取目前加互斥锁。** `tc_dynamic_constant` 每次读取都会锁 `dynamicConstantMutex`。若关卡内
   的模拟吞吐明显下降，应把 map 的值改为地址稳定的 `std::atomic<uint64_t>` 槽，并在线程本地缓存
   `componentId -> slot*`；setter 很低频，允许只在建槽时加锁。这个优化尚未实现。
2. **运行时 map 暂不清理。** 元件 id 是 64 位唯一值，当前只会逐渐累积。正常会话规模很小，但长期
   频繁换关后可在仿真 reset/board unload 时清理；清理前要先解决 JIT 并发与悬挂指针语义。
3. **只改 9–64 位常量。** 8 位及以下故意留给原版纸带；超过 64 位当前 UI 与 bridge 都不支持。
4. **数字输入框仍走原版重编译。** 本轮只优化纸带点击。直接编辑原版“常量值”文本框仍会按原版
   语义触发重编译，这是预期行为。
5. **Loader 版本号仍是 0.6.0。** 这是一份工作树内的功能构建，不是正式发布版本；不要据此覆盖
   `dist/releases/0.6.0`。
6. **验收脚本里的合成点击还需要一次排查。** `clickSurfacePoint` 已经算出正确格子的屏幕坐标（用纸带
   自己报的 canvas 原点 + 与面板相同的格子算术），但 ImGui 没有当成一次点击。下一步按
   `tests/ui-keyboard-driver.hpp` 的做法补 `SetForegroundWindow`、并在按下前后各留一帧。
7. ~~生成源码 dump 仍然每次都写。~~ **已修（2026-09-21）**：`compile()` 现在只在
   `TC_MODLOADER_DUMP_SOURCE=1` 时写 `native-logic-source-N.txt`，默认不落盘；验收脚本自己带这个
   变量，`punch-tape-playtest.ps1 -NoDumpEvidence` 验证"不设变量时目录干净、关卡照常跑"。

## 加载器构建可复现性（2026-09-21 复核）

结论：**`build.ps1` 的加载器构建是字节可复现的，之前怀疑的"同源码两次构建不同"是误报。**

- 用 build.ps1 第 57 行的原命令（MinHook 目标文件顺序 `buffer, hook, trampoline, hde64`、
  `-o dist\tc-loader.dll`、`SOURCE_DATE_EPOCH=315532800`）重新构建，得到与 23:38 那次完全相同的
  `D2CACFFAE4957B9A898295EFA8BD8C28075A401747FA8358AE5247BEE4D69E08`；同一条命令跨两次会话
  构建也是同一个哈希。
- 让哈希变掉的是**调用方式**，不是源码：换输出文件名/路径拼法，或把 MinHook 的 `.o` 顺序换成
  目录列举顺序，哈希就变。差异全部落在 `.data`（~136–204 B）与 `.rdata`（~6–9 KB）里的偏移表，
  **`.text`／`.pdata`／`.xdata` 逐字节相同**（验证：22:45 部署的 `E01E…` 与 `build.ps1` 产出的
  `D2CACF…` 对比，1,635,840 字节 `.text` 零差异）。
- 所以 22:45 那份部署的加载器是**手工编译**出来的（当时那份与 build.ps1 产物哈希不同、行为相同）；
  `-frandom-seed=` 与 `-s` 都**不能**消除这种差异（已实测），只有"同一条命令、同一个输出路径"才能
  得到同一个哈希。
- 规矩：**加载器哈希只能和 build.ps1 的产物比**；不要把手工编译的 DLL 哈希写进
  `compat/profiles.json` 的 `acceptedInstalledLoaders`，否则以后升级/卸载会认不出来。

## 构建命令

仓库根目录：`D:\p\tc-modloader`。

```powershell
# 离线测试
& C:\msys64\ucrt64\bin\g++.exe -std=c++17 -O2 -Wall -Wextra `
  -Wno-misleading-indentation -static tests\native-component.cpp `
  -o build\native-component-test.exe
& .\build\native-component-test.exe

# 只构建打孔纸带 Mod
& C:\msys64\ucrt64\bin\g++.exe -std=c++17 -O2 -Wall -Wextra -Werror `
  -static -shared -Isdk examples\punch-tape\plugin.cpp `
  -o build\punch-tape-package\native\punch-tape.dll -lole32 -lwindowscodecs
Copy-Item examples\punch-tape\mod.json build\punch-tape-package\mod.json -Force
& .\tools\Pack-Mod.ps1 -Source build\punch-tape-package `
  -Output dist\local.punch-tape-next.mod

# 完整构建也会生成加载器与 Mod
& .\build.ps1
```

`Pack-Mod.ps1` 拒绝覆盖已有输出，重复打包时要换一个明确的新文件名，确认后再复制到正式名称。

## 回滚

游戏必须先完全退出，再执行：

```powershell
Copy-Item D:\p\tc-modloader\dist\tc-loader-before-runtime-constants.dll `
  D:\p\game_engine.dll -Force
```

若只想回退 Mod，还需要把先前的 `local.punch-tape.mod` 包恢复到 `D:\p\mods`，再重新运行
`tcmod-cli apply`。旧加载器缺少运行时接口时，新 Mod 会自动回退到能工作但约 1 秒延迟的重编译路径。

## 工作树说明

- 仓库当前有大量用户原有与本轮累积的未提交改动/未跟踪文件；不要使用 `git reset --hard` 或整树
  checkout。
- 本轮没有创建 Git commit。
- `examples/punch-tape/` 整个目录目前仍显示为未跟踪；评审时不能只看普通 `git diff`，还要直接检查
  该目录内容。
- 生成物与已部署文件不等于源码已提交，正式发布前仍需按文件审阅、补完整回归并清理发布流程。
