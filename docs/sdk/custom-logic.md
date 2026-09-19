# 原生逻辑回调

若没有现成电路文件，优先用 [声明式元件接口](native-components.md)，直接声明引脚和
回调即可。下面介绍保留兼容的手写电路版本 2 接口；`TCLogicIO` 的回调阶段与状态约定通用。

ABI：`sdk/tc_logic_api.h`（版本 2）。用途：让**自定义元件的行为由插件的 C++ 回调决定**，
同时把关卡判定、内置元件、连线、暂停与重置继续交给游戏。

加载器不接管整板运行：游戏先把电路编译成中间源码，加载器把该元件**每个输出脚对应的
内部门**那一行替换成 `game_engine.'tc_logic_invoke'`（刷新用 `'tc_logic_peek'`，
其余输出脚用 `'tc_logic_out'`）。插件没有自己的仿真循环。

## 用法

```cpp
static void logicOr(TCLogicIO* io) {
    if (io->phase == TC_LOGIC_RESET) { io->outputs[0] = 0; return; }
    io->outputs[0] = (io->inputs[0] | io->inputs[1]) & 1;
}

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* host, TCPlugin* plugin) {
    if (!mod.load(host) || !mod.valid() || !mod.components.valid()) return 2;
    auto imported = mod.components.importCircuit("My component", bytes, size, "D:/my-mod/");
    if (!imported.ok()) return 3;
    TCLogicDefinition definition{sizeof(TCLogicDefinition), 2, imported.custom_id, &logicOr, nullptr};
    if (host->register_logic(host->context, &definition) != 0) return 4;
    return 0;
}
```

1. 按常规导入元件定义（`TCComponentModel::importCircuit`），让菜单、放置、保存和内部
   电路后备都存在，见 [game-model.md](game-model.md)。
2. 在 `tc_mod_load` 内调用 `host->register_logic`，传入同一个 `custom_id` 和回调。
   注册只允许在初始化期间进行。

完整示例：`examples/custom-or`（内部电路是占位门，行为完全由回调决定）。

## TCLogicIO

| 字段 | 说明 |
|---|---|
| `size`、`phase` | 结构长度；阶段见下表 |
| `instance_id` | 该实例在 board 上的元件 ID。同一次板子加载内稳定；重新加载同一张原理图时游戏可能分配新的 ID，旧绑定会保留但不再被调用（每实例状态因此按“本次加载的实例”隔离） |
| `cycle` | 仿真周期（刷新阶段可能是 -1） |
| `input_count`、`output_count` | 由元件定义决定；每个引脚 1–64 位 |
| `inputs[8]` | 每个引脚一个**完整字**，已按该脚位宽掩码 |
| `outputs[8]` | 回调写入；`outputs[0]` 也是生成代码调用的返回值 |
| `state[8]` | 每实例独立的 8×64 位持久状态 |
| `user` | 注册时传入的 `TCLogicDefinition::user` |

| phase | 调用时机 | `outputs[]` | `state[8]` |
|---|---|---|---|
| `TC_LOGIC_CYCLE` | 每个仿真周期 | 写回该元件输出脚 | 写回 |
| `TC_LOGIC_REFRESH` | 界面刷新，不推进周期（UI 读值） | 只用于显示 | 不写回（在副本上执行） |
| `TC_LOGIC_RESET` | 游戏重置仿真（`reset_sim`） | 忽略 | 先清零再调用 |

每个实例每周期只调用一次回调；同一元件定义的多个实例各自有独立 token、ID 与 `state`。

**两个阶段都要写 `outputs[]`**：`CYCLE` 驱动仿真与关卡判定，`REFRESH` 驱动界面与元件工坊
的实时表格。只写 `CYCLE` 的元件在仿真里正确，但界面会一直显示旧值（显示"元件算不对"，
而关卡测试其实通过）。无状态元件把计算抽成一个函数、两个阶段共用最省事；`REFRESH`
不要推进 `state[]`，它跑在副本上。示例：`examples/byte-adder` 的 `computeBytes()`。

## 形状规则

形状**由元件定义决定**，不是注册参数：

| 项目 | 规则 |
|---|---|
| 引脚数 | 每方向最多 8 个 |
| 位宽 | 每个引脚 1–64 位 |
| 总输入宽度 | ≤128 位（打包成两个 payload 字） |
| 内部门 | 每个输出脚对应一个可识别的逻辑节点（`0x03`–`0x0b`、`0x12`–`0x1e`、`0x2a`） |
| 回调输入元组 | 第一个输出门的操作数列表，顺序即回调 `inputs[]` 顺序，必须与定义输入脚数一致 |
| 输出顺序 | 元件的输出脚顺序 |

不满足时不会替换，运行内部电路并写诊断日志（`registration rejected:` /
`has no recognised internal logic node` / `exposes N operand(s) but ...`），不影响其它元件。

注册成功会在日志打印实测信息，可直接用于核对定义：

```text
Native logic: registered custom 0x44424c385f303031 inputs=1 outputs=1 in0=(-2,0,w8) out0=(2,0,w8) shape=1in/1out
```

## 实现约束（决定了本 ABI 的形态）

| 约束 | 结论 |
|---|---|
| 生成代码的外调最多 4 个参数（更多会触发游戏 JIT 的 `register_frame.nim(119,3) ... == EXP_NULL` 断言） | 输入按引脚位宽打包进两个 payload 字：`invoke(token, cycle, lo, hi)` |
| 外调返回值只有**按 1 位消费**才可靠（按 U8/U64 消费时关卡读到空值） | 字宽输出来自预留状态槽，生成代码按位 `load(<U1>, #SIMULATION_STATE + n)` 组字，形式与游戏 `com_maker_bit_8` 一致 |
| 状态槽需要固定位置 | 字宽输出占用状态缓冲区 `0x9a0000` 起、每实例 512 字节、最多 32 个实例；超大型板子理论上可能冲突 |

## 写元件定义（自建夹具同样适用）

元件定义就是一份 `circuit.data`：引脚用 `0x4f`（输入）/`0x51`（输出），内部电路用普通
门。实测约束：

- 多脚定义的引脚坐标要用游戏自己的几何：输入在左侧一列、间距 8（例如
  `(-18,-10)`、`(-18,-2)`、`(-18,6)`），输出在右侧、同样间距 8。用两脚夹具的两格间距
  写三脚定义会被导入器拒绝。
- v14 组件尾部字段顺序必须是 `bool_a`、`value_i64_a(-1)`、`value_i64_b(0)`、`bool_b`；
  顺序错了会被拒绝。
- 引脚偏移由游戏计算并打印在注册日志中；夹具与布线按实测值，不要凭定义坐标推算。
  已验证值：两脚 `in0=(-1,-1) in1=(-1,0) out0=(2,-1)`；三脚
  `in0=(-2,-1) in1=(-2,0) in2=(-2,1) out0=(2,0)`；三脚两出再加 `out1=(2,1)`。
- 内部电路只是**占位**：它决定回调被插入的位置和输入元组，语义完全由回调决定。

生成定义后可解码一次交叉检查，例如
`python tools/circuit_format.py build/and2_component.data`：组件数、引脚数与连线数
不符时游戏可能"导入成功"却按错位解析。

## 诊断

| 日志 | 含义 |
|---|---|
| `Native logic: registered ...` | 注册成功，含实测引脚数量/偏移/位宽 |
| `Native logic: registration rejected: ...` | 引脚数或位宽超出上限 |
| `Native logic: bound instance 0x.. of custom 0x.. as token N` | 编译时建立了实例绑定 |
| `Native logic: emitted N callback(s), mode=0/1` | 运行/刷新两个模式各替换的行数 |
| `Native logic: board layout nodeI=.. parent=..` | 进程首次编译打印的扁平化板面结构 |
| `Native logic: instance ... has no recognised internal logic node` | 内部节点不在识别范围内 |
| `Native logic: instance ... exposes N operand(s) but ...` | 操作数与定义输入脚数不一致，会附带原样发射行 |
| `Native logic: reset N instance(s)` | 游戏重置路径触发了 RESET |
| `native-logic-source-<n>.txt`（游戏工作目录） | 按次保存的生成源码转储，用于比对发射形式 |

## 边界

- 回调运行在游戏仿真线程：不要调用 UI／模型 API，不要抛出异常，不要保存 `TCLogicIO*`。
- 内置元件本身不经过回调；它们仍由游戏的代码生成器处理。
- 内部电路只用**一层**占位门：每个输出脚对应一个可识别门，且第一个门（最低节点序号）
  的操作数就是全部输入脚——它决定回调的 `inputs[]` 元组；其余输出门的行会被
  `tc_logic_out` 整行替换，操作数不参与桥接。内部中间层（同一输出脚由多级门串起来，
  门数多于输出脚数）按计划暂缓：「保留内部电路、只替换其中一部分」留待后续作为
  独立 Mod 提供，见 [../reference/limits.md](../reference/limits.md)。
  RAM／寄存器作为逻辑源同样不在范围内。
- 多位宽引脚已验证到 8 位（1–64 位在实现范围内）；16/32 位关卡、字宽状态保持、
  暂停语义尚未逐项验收。详见 [../verification.md](../verification.md)。
- 旧接口 `sdk/tc_custom_logic.h`（整板解释器）已弃用，仅保留给
  `tests/sim-state-probe.cpp` 研究探针。
