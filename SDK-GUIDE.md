# Native SDK 1

原生插件可以解析游戏符号、调用已知签名的函数、安装函数 Hook，并在游戏渲染线程中运行每帧逻辑和自己的 ImGui 界面。ABI 定义见 `sdk/tc_mod_api.h`。

## 包结构

```
mod.json
native/my-plugin.dll
native/其他私有依赖.dll          可选
files/asset/...                 可选
```

```json
{
  "format": 2,
  "id": "author.logic-mod",
  "name": "逻辑 Mod",
  "version": "1.0.0",
  "requires": [],
  "native": {"api": 1, "entry": "native/my-plugin.dll"}
}
```

仅支持 Windows x64 DLL。DLL 及依赖解压至按包 SHA-256 区分的缓存目录，由绝对路径加载。

## 最小入口

```cpp
#include "tc_mod_api.h"
static void frame(void*, const TCFrame*) {
    // 新的游戏逻辑或界面，每个 UI 帧最多调用一次。
}
extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* host, TCPlugin* out) {
    if (host->api_version != TC_MOD_API_VERSION ||
        host->size < sizeof(TCHost) || out->size < sizeof(TCPlugin)) return 1;
    host->log(host->context, "Loaded my logic mod");
    out->on_frame = frame;
    return 0;
}
```

入口返回 0 表示成功。初始化应放在入口函数；不要在 DllMain 中安装 Hook、访问 ImGui 或等待线程。

## Hook 游戏函数

`resolve_symbol(context, exact_name)` 返回当前 EXE 中 COFF 符号的实际地址，包含 ASLR 修正；不存在时返回 null。可以解析数据和函数，但 `create_hook` 只接受 EXE 函数符号。

```cpp
using SimDo = void (*)(void*, uint8_t, int64_t);
static SimDo original;
static void replacement(void* model, uint8_t command, int64_t target) {
    // 在这里改变参数、替换逻辑，或调用原函数后处理结果。
    original(model, command, target);
}
// tc_mod_load 中：
void* target = host->resolve_symbol(host->context,
    "sim_do__modelZsimulationZcompile95thread_u3036");
if (!target || host->create_hook(host->context, target,
    reinterpret_cast<void*>(replacement),
    reinterpret_cast<void**>(&original)) != 0) return 2;
```

完整的运行周期拦截实现见 `examples/cycle-guard/plugin.cpp`。它改变仿真命令中的目标周期，由内核在指定周期停下，不是每帧轮询后再迟到地暂停。

Hook 在初始化时创建但保持禁用；入口成功后，宿主才启用全部 Hook。只能在入口期间注册。同一目标函数出现第二个 Hook 会直接拒绝；失败插件的 Hook 会移除。

## 此构建验证过的签名

| 符号 | C ABI | 作用 |
| --- | --- | --- |
| `sim_do__modelZsimulationZcompile95thread_u3036` | `void(void*, uint8_t, int64_t)` | 向仿真线程提交命令。0 为 run，1 为 refresh/stop，2 为 mode_reset；正常插件保留 Hook 捕获的 model 参数 |
| `sim_get_cycle__modelZsimulationZcompile95thread_u3041` | `int64_t(void)` | 当前周期，初始可能为 -1 |
| `simulation_settings__modelZsimulator95types_u83` | 全局指针的地址 | 调用周期读取函数前检查共享区已初始化 |

Windows x64 的返回值、隐藏返回指针、参数大小、Nim 对象布局必须准确。本 SDK 不自动将 Nim 对象转换成 C++ 类型。对其他函数仍需核实签名，不能只凭名字调用。

## 游戏对象模型封装（实验）

`sdk/tc_game_model.h` 是第一阶段的原型／组件对象模型封装，只暴露本构建已经核实过的内存布局和调用约定，不做猜测式调用。

插件也可以直接包含总入口 `sdk/tc_mod.h`，一次引入上述模型头文件。

`sdk/tc_mod.h` 还提供 `TCMod` 聚合对象，调用一次 `load(host)` 即可加载全部已封装模型。

示例插件 `examples/mod-inspector` 直接使用 `TCMod` 显示战役状态、存档数、周期、选中元件/导线数等只读信息。

```cpp
#include "tc_game_model.h"

tc::TCGameModel model;
if (!model.load(host) || !model.valid()) return 2;

tc::TCPrototype p;
if (model.getPrototype(tc::kPrototypeKindCustom, custom_id, p)) {
    auto count = tc::prototypeInputCount(p);
    for (uint64_t i = 0; i < count; ++i) {
        auto* pin = tc::prototypeInputPin(p, i);
        auto word = tc::pinWordSizeRaw(*pin);
        // word 是原始 WordSize 值；需要时与 VARIABLE_WIDTH* 等全局对象比较。
    }
}
```

已验证的布局：

- `TCPrototype` 大小为 `0x5a8`，输入引脚数量在 `+0x60`、数据指针在 `+0x68`，输出引脚数量在 `+0x80`、数据指针在 `+0x88`。
- 每个引脚条目为 `0x38` 字节，原始 WordSize 在 `+0x10`。
- `get_prototype(kind_ptr, out)` 和 `get_custom_prototype(custom_id, out)` 由 `TCGameModel::getPrototype` 封装。
- `builtinPrototypeCount()`、`builtinPrototypeKindAt()`、`isBuiltinPrototypeKind()` 从 `PROTOTYPES` 哈希表安全枚举内置 kind，避免向 `getPrototype` 传入未知 key。
- `cloneBuiltinPrototype(kind, out)` 复制一个已验证的内置元件模板；`registerBuiltinAsCustom(kind, custom_id, out)` 进一步把它写入自定义元件表。
- `TCPrototypeBuilder` 封装“复制模板 → 调整已核实字段 → 注册为自定义元件”的工作流；未反推出的身份字段仍可通过 `setRawField` 写入。
- `TCGameModel::removeAllCustomPrototypes()` 可清空当前自定义元件表。
- `TCGameModel::builtinPrototypeName(kind)`／`builtinPrototypeDescription(kind)` 直接返回内置元件的名称/描述。
- 已验证 `Prototype +0x10` 为名称、`+0x28` 为描述；`setPrototypeName`／`setPrototypeDescription` 以及 Builder 的 `setName`／`setDescription` 使用游戏 `rawNewString` 分配 Nim 字符串。
- 已验证 `Prototype +0xb0` 为 SVG 形状/图标字符串，提供 `prototypeShapeSvg`／`setPrototypeShapeSvg`；`+0x40`、`+0x48` 作为分类/布局候选字段先以 `prototypeCategoryRaw`、`prototypeFlagsRaw` 暴露原始值。
- 新增 `tc_board_model.h`：通过 `selected_components`／`selected_wires` 和游戏自身的 `contains__modelZboardZboard_u1842` 查询元件/导线是否被选中，避免手写解析 Nim HashSet。
- `TCBoardModel` 同时接入 `prev_selected_components`／`prev_selected_wires`，可用于检测选中变化。
- `TCBoardModel` 通过 `len__modelZboardZboard_u19087` 提供当前/上一帧选中元件和导线数量。
- `TCBoardModel` 现在可以枚举当前/上一帧选中的元件 ID 和导线 ID。
- 新增 `tc_game_state.h`：读取 `is_campaign`、`level_progress`、`campaign_name`、`simulation_circuit_state` 等全局游戏状态。
- `TCGameStateModel` 现在也读取 `current_word_size`，提供 `currentWordSize()`。
- `TCGameStateModel` 新增 `levelUsedInput()`／`levelUsedOutputs()`。
- 新增 `tc_simulation.h`：封装 `sim_do`、`sim_get_cycle`、`simulation_settings`、`get/set_command_setting`，支持提交仿真命令、读取/修改命令设置。
- `TCSimulationModel` 还暴露 `inputReplay`、`outputHistoryPins`、`keyboardCharacter`、`keyboardCoordinate` 只读指针。
- 新增 `tc_wire_model.h`：封装导线读取、取色、添加、放置和更新函数，以及 `INVALID_WIRE_ID`。
- 新增 `tc_save_model.h`：读取 `save_count`，并通过 `__emutls_get_address` 获取当前线程的 level/schematic 存档路径。
- `get_input_word_size`／`get_output_word_size` 只在提供合法内置 kind 时调用，封装默认传 `AUTO_SIZE` 对象地址作为期望宽度。
- `custom_prototypes_set(id, prototype)`／`custom_prototypes_del(id)` 是已核实的低层注册原语，`in_custom_prototypes`／`notin_custom_prototypes` 用于查询，`customPrototypeCount()` 读取 `cc_length`，`customPrototypeIdAt(index)` 读取 `cc_live_values` 中的 ID。
- `TCGameModel::setCustomPrototype` 会深拷贝传入的 `TCPrototype`，调用后原对象可以释放。

边界：内置元件的 kind 是一个单字节键。`TCGameModel` 不会枚举所有 kind；传入未知 kind 可能触发游戏自己的 Nim 索引／键错误并使原生插件崩溃。自定义元件 ID 查询缺失时返回空对象。`setCustomPrototype` 只负责把 `TCPrototype` 写入自定义元件表；一个能被游戏正确显示、放置和仿真的完整 `Prototype` 仍需填充名称、形状、颜色等未在头文件中建模的字段。文件解析型 `add_custom_prototype` 仍待继续核实其完整栈参数布局。

## 每帧回调和界面

`on_frame` 在游戏主／渲染线程及 ImGui 帧内部执行，每帧最多一次。这是 UI 帧事件，不是每一个仿真周期的事件。

`engine_proc` 获取原引擎导出，例如 `igBegin`、`igEnd`、`igButton`、`igGetIO`。使用游戏的 ImGui 1.92.6 上下文，不要创建另一个上下文。必须平衡 Begin/End、样式与字体堆栈，不得跨 C ABI 抛出异常、阻塞渲染线程或从后台线程访问 ImGui。

## 数据与生命周期

- `data_directory_utf8` 是插件的独立持久目录，路径可能含中文。
- TCHost 及字符串在当前进程内持续有效；TCFrame 指针只在回调期间有效。
- 插件在首个 UI 阶段初始化，不能拦截此前已完成的启动步骤。
- 原生插件启停和更新需重启游戏，不支持热卸载。
- `on_unload` 用于初始化失败时清理，正常退出不保证调用；配置应及时保存。
- 失败 DLL 保持映射至进程结束，避免静态线程指向释放的内存。
- 原生代码拥有与游戏相同权限，不能可靠隔离访问违规或崩溃。启动时按住 Shift 可跳过原生插件，再进入管理页停用。
- 本版已提供实验性的 `tc_game_model.h` 对象模型读取封装；完整的新元件注册／Lua API 仍需研究相应内部接口。

## 编译和打包

使用 MSVC 或 MinGW-w64 构建 x64 DLL，建议静态链接编译器运行库，或将依赖 DLL 放在入口 DLL 同目录。

```powershell
g++ -std=c++17 -O2 -static -shared plugin.cpp -Isdk -o my-package/native/my-plugin.dll
powershell -NoProfile -ExecutionPolicy Bypass -File tools/Pack-Mod.ps1 -Source my-package -Output my-plugin.mod
```

打包目录不要包含根目录 `plugin.cpp`。玩家收到编译好的 `.mod`，不需要编译环境。

示例的 `TC_GUARD_SELFTEST` 是开发测试编译宏，正式构建不定义。它会自动执行隔离测试操作并捕获游戏帧，不应作为玩家版本分发。
