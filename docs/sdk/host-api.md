# 宿主 API 与 Hook

ABI 定义：`sdk/tc_mod_api.h`。原生插件可以解析游戏符号、调用已知签名的函数、安装函数
Hook，并在游戏渲染线程运行每帧逻辑和自己的 ImGui 界面。

## 最小入口

```cpp
#include "tc_mod_api.h"

static void frame(void*, const TCFrame*) {
    // 新的游戏逻辑或界面，每个 UI 帧最多调用一次。
}

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* host, TCPlugin* out) {
    if (host->api_version != TC_MOD_API_VERSION ||
        host->size < TC_HOST_BASE_SIZE || out->size < sizeof(TCPlugin)) return 1;
    host->log(host->context, "Loaded my logic mod");
    out->on_frame = frame;
    return 0;
}
```

- 入口返回 0 表示成功，非 0 拒绝该插件及其 Hook（初始化期间的副作用由宿主清理）。
- 初始化必须放在入口函数里；不要在 `DllMain` 安装 Hook、访问 ImGui 或等待线程。
- 扩展字段一律先比较 `host->size` / `out->size` 再使用（尾追加兼容）；
  想知道"这个加载器到底支持什么"，用能力位而不是结构长度，见下节。
- 公共结构布局和常量由 [SDK ABI 快照](../compatibility.md#sdk-abi-快照)保护。

## 能力协商

`TCHost` 有两个尾追加字段回答"这个加载器能做什么"：

| 字段 | 说明 |
|---|---|
| `host_version` | 加载器构建号，用 `TC_HOST_VERSION_CODE(major,minor,patch)` 打包 |
| `capabilities` | `TC_CAP_*` 位掩码：每一位对应一组真正实现的入口 |
| `report_status(context, level, message)` | 把本插件状态写给 Mods 页与 `loader.log`（0 信息 / 1 警告 / 2 错误） |

```cpp
if (!tc::hostHas(host, TC_CAP_UI_SLOT)) return 2;          // 缺能力就拒绝启动
tc::reportStatus(host, 0, "board panel ready");            // 让玩家看见状态
tc::reportStatus(host, 2, "board model not resolved");     // 红色，写在 Mods 页
```

`tc::hostHas()` / `tc::hostCapabilities()` / `tc::hostVersion()` 自己会检查
`host->size`，所以在没有这些字段的旧加载器上安全返回 false / 0。完整名字表、
各能力的入口与版本约束写法见 [reference/capabilities.md](../reference/capabilities.md)。

包也可以**声明**自己需要的能力（`mod.json` 的 `capabilities`）：那样加载器在扫描阶段
就能用可读的理由拒绝它，而不是让插件在 `tc_mod_load` 里失败一半。

## TCHost

| 字段 | 说明 |
|---|---|
| `api_version`、`size` | ABI 版本与结构长度 |
| `context` | 传给所有宿主回调的句柄 |
| `game_build`、`mod_id` | 构建标识与当前包 id |
| `data_directory_utf8` | 插件的持久数据目录（UTF-8，可能含中文） |
| `log(context, message)` | 写入加载器日志（`tc-modloader-data/loader.log`） |
| `resolve_symbol(context, exact_coff_name)` | 解析 EXE 的 COFF 符号，含 ASLR 修正；不存在返回 null |
| `engine_proc(context, export_name)` | 获取原引擎导出（如 `igBegin`、`igGetIO`） |
| `create_hook(context, target, detour, original)` | 创建一个**禁用**的 Hook；仅入口期间可用 |
| `register_logic(context, definition)` | 注册原生逻辑回调，见 [custom-logic.md](custom-logic.md) |
| `register_component(context, definition)` | 声明引脚与回调，自动创建元件，见 [native-components.md](native-components.md) |
| `register_ui_page` / `register_ui_slot` | 主菜单页面与游戏布局插槽，见 [ui.md](ui.md) |
| `create_ui_texture` / `load_ui_texture` / `release_ui_texture` | 图片与 RGBA 纹理，见 [ui.md](ui.md) |
| `report_status(context, level, message)` | 插件自己的状态行（Mods 页 + 日志），可跨线程调用 |
| `resolve_alias(context, "sim.do")` | 按稳定别名取地址（本构建的符号画像），见 [symbols.md](../reference/symbols.md) |
| `register_hook_chain(context, hook_id, priority, callback, user)` | 加入加载器拥有的钩子链；多个 Mod 可共用同一目标 |
| `add_event_listener(context, kinds, callback, user)` | 订阅宿主事件（关卡加载、场景切换、仿真命令、保存） |

## Hook 游戏函数

`resolve_symbol` 可以解析数据和函数地址，但 `create_hook` 只接受 EXE 函数符号。

```cpp
using SimDo = void (*)(void*, uint8_t, int64_t);
static SimDo original;

static void replacement(void* model, uint8_t command, int64_t target) {
    original(model, command, target);   // 改参数、替换逻辑或后处理都在这里
}

// tc_mod_load 中：
void* target = host->resolve_symbol(host->context,
    "sim_do__modelZsimulationZcompile95thread_u3036");
if (!target || host->create_hook(host->context, target,
        reinterpret_cast<void*>(&replacement),
        reinterpret_cast<void**>(&original)) != 0) return 2;
```

规则：

- Hook 在初始化时创建但保持禁用；入口成功后宿主才统一启用。
- 同一目标函数的第二个 `create_hook` 仍然直接被拒绝（不做隐式串联）；**多个 Mod 共用**同一个
  游戏函数请用钩子链（`register_hook_chain`），加载器为已核实的钩子点提供这一层，
  见 [symbols.md](../reference/symbols.md)；失败插件的 Hook 与链节都会被移除。
- 只能在入口期间注册；热卸载不支持。
- 完整示例见 `examples/cycle-guard/plugin.cpp`（限制连续运行的目标周期，由内核在指定
  周期停下，而不是每帧轮询后迟到暂停）。

## 本构建验证过的签名

| 符号 | C ABI | 作用 |
|---|---|---|
| `sim_do__modelZsimulationZcompile95thread_u3036` | `void(void*, uint8_t, int64_t)` | 向仿真线程提交命令：0 run、1 refresh/stop、2 mode_reset |
| `sim_get_cycle__modelZsimulationZcompile95thread_u3041` | `int64_t(void)` | 当前周期，初始可能为 -1 |
| `simulation_settings__modelZsimulator95types_u83` | 全局指针的地址 | 读取周期设置前先确认共享区已初始化 |

Windows x64 的返回值、隐藏返回指针、参数大小、Nim 对象布局必须准确。SDK 不自动把 Nim
对象转换成 C++ 类型；对其他函数仍需先核实签名，不能只凭名字调用。常用模型封装见
[game-model.md](game-model.md) 与 [simulation.md](simulation.md)。

## 每帧回调与界面

`on_frame` 在游戏主／渲染线程、ImGui 帧内部执行，每帧最多一次；这是 **UI 帧事件**，
不是每个仿真周期的事件（逐周期逻辑请用 [custom-logic.md](custom-logic.md)）。

`engine_proc` 拿到的是游戏自身的 ImGui 1.92.6 上下文（`igBegin`、`igEnd`、`igButton`、
`igInvisibleButton` 等）。约束：

- 不要创建第二个 ImGui 上下文。
- Begin/End、样式栈、字体栈必须平衡。
- 不得跨 C ABI 抛异常、阻塞渲染线程，或从后台线程访问 ImGui。

## 数据与生命周期

| 事实 | 说明 |
|---|---|
| 目录 | `data_directory_utf8` 是插件的独立持久目录，路径可能含中文 |
| 有效性 | `TCHost` 及字符串在进程内持续有效；`TCFrame*` 只在回调期间有效 |
| 初始化时机 | 插件在首个 UI 阶段初始化，不能拦截此前完成的启动步骤 |
| 停止 | 启停与更新需重启游戏，不支持热卸载 |
| 清理 | `on_unload` 只在初始化失败时调用；配置应及时保存 |
| 失败 DLL | 保持映射至进程结束，避免静态线程指向已释放内存 |
| 权限 | 原生代码与游戏同权限，无法可靠隔离访问违规或崩溃；Shift 启动可跳过原生插件 |

## 错误处理约定

- 入口用非 0 返回值报告失败，并在日志中给出原因；宿主据此撤销 Hook 与已注册逻辑。
- 不要跨 C ABI 抛出异常；插件内部异常应在回调边界内捕获（示例插件即如此）。
- 失败后不要“清理”游戏的 Nim 错误标志来重试：那会掩盖真实错误。相关约定见
  [game-model.md](game-model.md#电路型元件导入实验)。
