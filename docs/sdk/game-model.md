# 元件与原型模型

`sdk/tc_game_model.h` 是原型／元件对象模型封装，只暴露本构建已核实的内存布局和调用
约定；`sdk/tc_component_model.h` 在其上提供电路文件导入。聚合入口 `tc_mod.h` 一次
引入全部模型。

```cpp
#include "tc_game_model.h"

tc::TCGameModel model;
if (!model.load(host) || !model.valid()) return 2;

tc::TCPrototype p;
if (model.getPrototype(tc::kPrototypeKindCustom, custom_id, p)) {
    auto count = tc::prototypeInputCount(p);
    for (uint64_t i = 0; i < count; ++i) {
        auto* pin = tc::prototypeInputPin(p, i);
        auto word = tc::pinWordSizeRaw(*pin);   // 原始 WordSize 值
    }
}
```

## 已验证布局

| 位置 | 含义 |
|---|---|
| `TCPrototype` 大小 `0x5a8` | 原型对象 |
| `+0x10` / `+0x28` | 名称 / 描述（Nim 字符串） |
| `+0x60` / `+0x68` | 输入引脚数量 / 数据指针 |
| `+0x80` / `+0x88` | 输出引脚数量 / 数据指针 |
| `+0xb0` | SVG 形状／图标字符串 |
| `+0x40` / `+0x48` | 分类／布局候选字段（以原始值暴露） |
| `+0x130` / `+0x138` | 缓存门数 / 声明延迟 |
| 引脚条目 `0x38` 字节 | 原始 WordSize 在 `+0x10`；相对坐标在描述符 `+2`（描述符起点为 `TCPin* + 8`） |
| `TCPrototypeKind +0x188` | 自定义 ID，**uint64_t**（低 16 位仅用于哈希初始槽位） |

## 常用调用

| 目标 | 接口 |
|---|---|
| 取内置元件模板 | `cloneBuiltinPrototype(kind, out)`、`builtinPrototypeAt(index, out)` |
| 安全枚举内置表 | `builtinPrototypeCount()`、`builtinPrototypeKindAt()`、`isBuiltinPrototypeKind()` |
| 命名／描述／图标 | `setPrototypeName`、`setPrototypeDescription`、`setPrototypeShapeSvg` |
| 注册为自定义元件 | `setCustomPrototype(id, prototype)`、`registerBuiltinAsCustom(...)`、`registerNamedBuiltinAsCustom(...)` |
| 清空自定义表 | `removeAllCustomPrototypes()` |
| 查询自定义表 | `hasCustomPrototype(id)`、`customPrototypeCount()`、`customPrototypeIdAt(index)` |
| 写入设计代价 | `setPrototypeGateCost`、`setPrototypeDelay`、`TCPrototypeBuilder::setDesignCost` |

内置元件的 kind 是单字节键：请用内置表枚举，不要向游戏查询任意字节值，未知 kind 可能
触发 Nim 索引／键错误。自定义 ID 查询缺失时返回空对象。

`TCPrototypeBuilder` 封装“复制模板 → 调整已核实字段 → 注册”的工作流；未反推出的身份
字段仍可用 `setRawField` 写入。`setCustomPrototype` 会深拷贝传入对象，调用后原对象可释放。

## 电路型元件导入（实验）

`TCComponentModel`（`tc_mod.h` 里的 `mod.components`）是**可选**能力，需单独检查
`valid()`。

| 函数 | 行为 |
|---|---|
| `importCircuit(name, bytes, length, directory)` | 把 `circuit.data` 的二进制内容交给游戏解析并注册，返回状态与完整 64 位 ID |
| `updateFromDirectory(directory, name)` | 走游戏自身的路径读取流程，读取 `directory + "circuit.data"` |
| `releasePrototype(owned)` | 释放游戏 getter 返回的独占原型快照并清零 |
| `readiness()` | 检查符号齐备、绑定线程、Nim 错误状态 |

```cpp
tc::TCComponentModel components;
if (!components.load(host)) return 2;                 // 可选能力，失败可降级
auto result = components.updateFromDirectory("D:/my-mod/circuit/", "My component");
if (result.ok()) {
    tc::TCPrototype snapshot{};
    model.getCustomPrototype(result.custom_id, snapshot);
    components.releasePrototype(snapshot);            // 名称/引脚指针仅在释放前有效
}
```

约定与边界：

- 在游戏主／渲染线程绑定并调用，仿真应处于停止状态；错线程调用会被拒绝。
- 目录使用 UTF-8，必须以 `/` 或 `\` 结尾。
- 导入 ID 来自电路文件，不能用名称参数指定；同 ID 会更新已有原型。
- 调用不是事务：失败不承诺回滚；封装会拒绝在已有 Nim 错误状态下继续调用，也不清除
  游戏错误标志。
- 状态包括 `Ok`、`Unavailable`、`WrongThread`、`InvalidArgument`、`NimError`、`Rejected`。
- 只负责原型表写入：不注册新的原生 kind，也不注册周期回调（那需要
  [custom-logic.md](custom-logic.md)）。
- 临时输入字符串用游戏分配器，显式设置长度并在同线程释放；不能把临时插件内存标记成
  Nim 静态字符串。
- `releasePrototype` 只用于游戏 getter 返回、由调用方独占的快照。

完整示例：`examples/circuit-and`（内存中构造两输入一输出 AND 电路，导入后设置名称、
描述与内置 AND 图标）。

## 设计代价与声明统计

定义头里的两个 i64 是设计自身的缓存 `(门数, 延迟)`。游戏解析时**重算门数**，但
**原样保留延迟**，所以定义文件必须携带真实关键路径；对照值可参考游戏自带工坊元件
（`4or=(3,2)`、`8or=(7,3)`、`1and8=(8,1)`、`half-add=(4,2)`）。

在 `tc_mod_load` 中导入电路、写 `setPrototypeGateCost` / `setPrototypeDelay`
（或 `setDesignCost`）再注册后，加载器会让**本次启动登记过的**原生元件在无环电路
编译统计里使用这对声明值。声明值在插件自己的 `tc_mod_load` 期间捕获（该作用域内
最后一次注册为准），因此不受游戏后续重算原型字段的影响：

| 形状 | 规则 |
|---|---|
| 串联 | 延迟相加 |
| 并联 | 取最长路径 |
| 嵌套在有声明延迟的外层元件内 | 只计外层一次 |
| 反馈环、多驱动、收缩后成环、非法数据、溢出 | 保留游戏原生统计，并写 `Component timing: native timing retained...` |

门数同理：整板门数本来会把自定义元件**展开成内部电路**再逐门相加，声明门数会替换掉
这一展开（同一元件展开出的内部节点不再重复计数，嵌套 Mod 只计最外层一次）。
元件内部逻辑始终由游戏展开执行，本特性只影响编译期统计与界面分数，不改变仿真语义。
游戏自身「跳过自定义元件」的调用语义保留：那种调用仍然跳过 Mod 元件。
运行时才新增的原型不在登记范围内。

示例 `design-stats.txt` 的 `1 5` 可作对照：单个 AND2 显示 1 门、总延迟 5；
两个串联 2 门/10；两个并联 2 门/5。声明 `(1,1)` 但内部是 Mux+NOT 的元件
（`examples/byte-adder`）在关卡里显示 1 门 / 1 延迟，而不是展开后的 51 门。

## 内置元件表与有状态元件

`dev.kind-list.mod`（源码 `tests/kind-list-probe.cpp`）是只读探测包：启动后枚举
`PROTOTYPES`，把 125 个内置元件的 kind、名称、引脚数与偏移写进
`tc-modloader-data/plugin-data/dev.kind-list/kinds.txt`；`tests/kind-list-playtest.ps1`
在隔离副本里跑一遍并存到 `build/kinds.txt`。

有状态元件（`0x0d Delay Line`、`0x0e Register`、`0x26/0x27`、`0x37`）**不声明输入
引脚**，只有输出；输入位置需从游戏自带电路反推，例如
`campaign/double_buffer/hint_solution.data` 给出 Delay Line 输入 `(-3,0)`、输出 `(+3,0)`。
Delay Line 的代价是 5 门／4 延迟（游戏自身 `get_cost` 返回值），以它为基础封装的有状态
元件应声明 `(5,4)`；fixture 拓扑 `delay`（`tests/and-component-fixture.cpp`）即如此。

诊断观察包 `dev.cost-watch.mod` 会把界面实际显示的分数、各 kind 代价与原型字段写进
加载器日志（前缀 `cost-watch:`），见 [../reference/diagnostics.md](../reference/diagnostics.md)。
