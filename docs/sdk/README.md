# SDK 总览

SDK 是一组头文件（`sdk/`），描述加载器暴露给原生插件的 C ABI 与已核实的内存布局封装。
它只覆盖本构建已实测的部分，不做猜测式调用。

## 头文件地图

| 头文件 | 内容 | 参考 |
|---|---|---|
| `tc_mod_api.h` | 宿主入口：`TCHost`、`TCPlugin`、日志、符号解析、Hook、逻辑注册 | [host-api.md](host-api.md) |
| `tc_service_api.h` | 独立版本的服务表；Board V1 句柄、V2 快照、V3 对象枚举、V4 对象读取、V5 元件引脚，以及 `tc.simulation` | [services.md](services.md) |
| `tc_command_api.h` | 排队、归属隔离、可查询结果的游戏命令总线 | [commands.md](commands.md) |
| `tc_lifecycle_api.h` | Board 进入/离开、对象与选择变化的值类型事件 | [lifecycle.md](lifecycle.md) |
| `tc_transaction_api.h` | 乐观冲突检测、连续命令批次、原生 Undo/Redo/Save | [transactions.md](transactions.md) |
| `tc_handle_api.h` | 带代次检查的游戏对象句柄 | [services.md](services.md) |
| `tc_mod.h` | 聚合入口：一次 `load(host)` 加载下面全部模型 | 本页 |
| `tc_game_model.h` | 原型／元件对象模型、内置表、自定义元件注册、代价与延迟字段 | [game-model.md](game-model.md) |
| `tc_component_model.h` | 电路文件导入、目录更新、原型快照释放 | [game-model.md](game-model.md) |
| `tc_native_component.h` | 声明引脚＋C++ 回调，自动创建元件，无需电路文件 | [native-components.md](native-components.md) |
| `tc_component_types.h` | V2 定义：每方向 0–16 脚、自定义状态大小的回调（`TCLogicIOV2`） | [services.md](services.md#tccomponenttypes注册一个-v2-元件定义)、[custom-logic.md](custom-logic.md) |
| `tc_component_geometry.h` | M5 V1/V2：声明类型 footprint，并逐实例同步原生交互矩形 | [services.md](services.md#tccomponentgeometry声明类型-footprint-与逐实例交互范围) |
| `tc_component_render.h` | M5 V1/V2：按类型注册棋盘绘制回调（宿主仿射基、裁剪、受控图元），并用 V2 关掉游戏自己的缩略图/水印 | [services.md](services.md#tccomponentrender棋盘覆盖绘制) |
| `tc_component_instances.h` | 跨帧稳定的实例句柄：枚举、世代校验、读状态、单实例重置 | [services.md](services.md#tccomponentinstances跨帧稳定的实例句柄) |
| `tc_component_storage.h` | 每实例配置 blob 与仿真状态快照；配置和 RESET 语义分离 | [services.md](services.md#tccomponentstorage配置与仿真状态快照) |
| `tc_board_model.h` | 当前／上一帧选中的元件与导线 | [simulation.md](simulation.md) |
| `tc_game_state.h` | 战役、关卡、字宽、当前输入/输出 | [simulation.md](simulation.md) |
| `tc_simulation.h` | 周期、命令设置、run/pause/reset、输入回放与输出历史指针 | [simulation.md](simulation.md) |
| `tc_trace.h` | 关卡波形：按周期读取输入／输出、槽位发现、VCD 导出（只读） | [simulation.md](simulation.md) |
| `tc_wire_model.h` | 导线读取、取色、添加、放置、更新 | [simulation.md](simulation.md) |
| `tc_save_model.h` | 保存次数、当前 level/schematic 路径 | [simulation.md](simulation.md) |
| `tc_logic_api.h` | 原生逻辑回调 ABI（`TCLogicDefinition`、`TCLogicIO`） | [custom-logic.md](custom-logic.md) |
| `tc_ui.h` | 插件界面：引擎 ImGui 导出的类型化封装、面板/窗口作用域、主菜单页面（`registerPage`）与电路板侧栏插槽（`registerBoardPanel`）、视口、物理热键与 ImGui 键盘（`keys::*`） | [ui.md](ui.md) |
| `tc_ui_draw.h` | 可选自定义绘图：局部画布、图形／曲线／多边形、裁剪与鼠标交互 | [ui.md](ui.md) |
| `tc_ui_texture.h` | 图片／RGBA 上传、图片绘制与按钮、宿主管理的纹理生命周期 | [ui.md](ui.md) |
| `tc_custom_logic.h` | **已弃用**：整板解释器，仅研究探针使用 | — |

常用组合：

```cpp
#include "tc_mod.h"      // TCMod：game + components + board + state + simulation + wire + save

static tc::TCMod mod;

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* host, TCPlugin* plugin) {
    if (host->api_version != TC_MOD_API_VERSION) return 1;
    if (!mod.load(host) || !mod.valid()) return 2;
    plugin->on_frame = frame;   // 需要时再挂每帧回调
    return 0;
}
```

`TCMod::valid()` 不包含可选的 `components`（电路导入）。需要它时另查
`mod.components.valid()`，见 [game-model.md](game-model.md)。

## 包结构

```text
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
  "native": {"api": 1, "entry": "native/my-plugin.dll"}
}
```

仅支持 Windows x64 DLL；DLL 与依赖解压到按包 SHA-256 区分的缓存目录后以绝对路径加载。
包格式细节见 [mod-format.md](../mod-format.md)。

## 编译与打包

用 MSVC 或 MinGW-w64 构建 x64 DLL，建议静态链接编译器运行库，或把依赖 DLL 放在入口
DLL 同目录。

```powershell
g++ -std=c++17 -O2 -static -shared plugin.cpp -Isdk -o my-package/native/my-plugin.dll
powershell -NoProfile -ExecutionPolicy Bypass -File tools/Pack-Mod.ps1 -Source my-package -Output my-plugin.mod
```

打包目录不要包含根目录 `plugin.cpp`。玩家收到编译好的 `.mod`，不需要编译环境。
示例里的 `TC_GUARD_SELFTEST` 是开发测试宏，正式构建不定义它，不要随玩家版本分发。

## 下一步

- 第一个插件：[guides/first-native-mod.md](../guides/first-native-mod.md)
- 用 C++ 决定元件行为：[guides/component-with-cpp-logic.md](../guides/component-with-cpp-logic.md)
- 给插件加界面：[ui.md](ui.md)
- 查询版本化游戏服务：[services.md](services.md)
- 上限与约束：[reference/limits.md](../reference/limits.md)
