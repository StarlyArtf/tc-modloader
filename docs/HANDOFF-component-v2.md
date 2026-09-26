# 接手：元件接口 V2（阶段 1 收口：脚数 >8 与值模型）

更新时间：2026-09-22

> **状态：已交付（2026-09-22）**。本文正文保留为"当时计划做什么"的记录；
> 交付清单、证据与仍不做的事见 [changelog.md](changelog.md) 的"元件接口 V2"一节、
> [verification.md](verification.md) 的"V2 定义：九脚元件"一节，以及
> `sdk/tc_component_types.h`、`sdk/tc_service_api.h`（`tc.component.types`）。
> 一句话结论：`tc.component.types` V1 + `TCLogicIOV2` + `TCComponentTypeDefinitionV2` 已落地，
> 每方向 0–16 脚、状态大小由定义决定，九脚真机用例 `pin-shape-wide9` 通过；
> 多字 >64 位信号按取舍不做，出口是 M2 的宿主状态/缓冲。
> **下一步**：阶段 2（实例句柄、生命周期、去固定 `0x9a0000` 状态槽）——见
> [PLAN-custom-components.md](PLAN-custom-components.md) §16。

计划出处：[PLAN-custom-components.md](PLAN-custom-components.md) §6.1/§6.3 与 §16 阶段 1。
三条前置测量都做完了（[research/custom-component-pins.md](research/custom-component-pins.md)）：

| 问题 | 答案 |
|---|---|
| 0 输入 / 0 输出 | ✅ 能用"一个悬空驱动门"实现，已交付（脚手架 + 桥接 + 离线 + 真机用例） |
| >8 脚 | 游戏导入器**没有**上限（9/16/32 脚定义导入全部成功）；8 脚是加载器自己的限制 |
| >64 位 | **不做**。外调只能可靠传 4 个参数（扩到 6 个时第 5、6 个收到垃圾，实测），且游戏原版信号上限就是 64 位 |

由此定下的取舍（已写进计划 §6.3 与 [reference/limits.md](reference/limits.md)）：
**每脚 1–64 位、总输入 ≤128 位、脚数可以 >8、不做多字信号**。
需要更多位的元件（1+64+64 的 64 位加法器等）走 M2 的宿主状态/缓冲，不在本切片里。

本文只写"这一刀"的动手范围：**让回调能拿到 8 个以上的脚，并且值模型一次定型**。

## 要交付的东西

### 1. SDK：值模型与类型定义（新增，不动 V1）

`sdk/tc_logic_api.h` 追加（V1 的 `TCLogicIO` / `TCNativeComponentDefinition` 原样保留）：

```c
#define TC_LOGIC_IO_V2_VERSION_1 1u
typedef struct TCLogicIOV2 {
    uint32_t size, version;
    uint32_t phase;                 /* RESET / REFRESH / CYCLE，同 V1 */
    uint64_t instance_id;
    int64_t cycle;
    uint32_t input_count, output_count;
    const uint64_t* inputs;         /* 宿主借用；回调期间有效；每脚已按声明位宽掩码 */
    uint64_t* outputs;              /* 宿主借用；CYCLE 结束后原子提交 */
    uint64_t* state;                /* 宿主借用；state_words 个 64 位字 */
    uint32_t state_words;
    uint32_t reserved;
    void* user;
} TCLogicIOV2;
typedef void (*TCLogicCallbackV2)(TCLogicIOV2*);
```

`sdk/tc_component_types.h`（新）+ `sdk/tc_service_api.h` 里的 `TC_SERVICE_COMPONENT_TYPES` V1：

```c
typedef struct TCComponentPinV2 {
    const char* pin_id;   /* 稳定 id（Mod 命名空间内唯一），可为空 */
    const char* name;     /* 显示名，可为空 */
    uint32_t bits;        /* 1..64 */
    uint32_t reserved;
} TCComponentPinV2;

typedef struct TCComponentTypeDefinitionV2 {
    uint32_t size, version;
    uint64_t custom_id;              /* 存档里的稳定标识 */
    const char* type_id;             /* "mod-id/name"，供查询 */
    const char* name, *description, *shape_svg;
    const TCComponentPinV2* inputs, *outputs;
    uint32_t input_count, output_count;   /* 每方向 0..上限，且 总输入位宽 ≤128 */
    uint32_t state_words;            /* 0 表示不需要持久状态 */
    uint64_t gate_cost, delay;
    TCLogicCallbackV2 callback;
    void* user;
} TCComponentTypeDefinitionV2;

typedef struct TCComponentTypesApiV1 {
    uint32_t size, version; void* context;
    int (*register_definition)(void*, const TCComponentTypeDefinitionV2*);
} TCComponentTypesApiV1;
```

新增服务而不是继续加长 `TCHost` 尾部（计划 §2 的规矩）。`register_definition` 只在
`tc_mod_load` 期间调用；拒绝原因进 `tc.component.registry` 目录，和 V1 一样。

### 2. 加载器

- `src/component_definition.hpp`：把 `valid()` 的每方向上限从 8 提到 **16**（离线可测的取值），
  保持"总输入 ≤128 位""两边不能同时为空"；`encode()` 不动（它已经是 N 脚通用写法）。
- `src/native_logic.hpp`：
  - `kMaxBridgeInputs` / `kMaxBridgeOutputs` 提到 **16**，`Binding::state` 从 `[8]` 改为按定义
    大小的 `std::vector<uint64_t>`；
  - `Definition` 增加 `useV2` 与 `callbackV2`；
  - `supportedScaffold()` 允许 >8 脚（总输入 ≤128 位这条继续拦）；
  - `invoke()` 在 V2 时填 `TCLogicIOV2`（借用 `Binding` 自己的数组）并调用 V2 回调；
    V1 路径原样，保证老 Mod 行为不变；
  - `readOutput` / `readOutputBit` 的上界跟着 `kMaxBridgeOutputs` 走。
- `src/native.hpp` + `src/services.hpp`：实现并绑定 `TC_SERVICE_COMPONENT_TYPES` V1，
  内部复用现有的导入 + 桥接流程（`registerNativeComponent` 的 V2 孪生）。

### 3. 验收（缺一不可）

1. **离线**：`tests/native-component.cpp` 断言 16 脚脚手架的节点数与线序（现有 0 脚用例的扩展）；
   另加"总输入 129 位必须被拒绝"的用例（预算边界）。
2. **真机 · 9 脚**：新增板子 fixture（一个 9 输入 1 输出的 V2 元件，9 根输入线**从同一米源扇出**，
   输出接关卡输出）+ 探针形状（回调逐拍打印 9 个输入 + 状态字），
   断言：注册成功、实例绑定、回调每拍一次、`inputs[0..8]` 都拿到值。
3. **真机 · 回归**：`byte-adder-smoke`（声明式 V1）、`custom-or` 的 1in/1out 与 3in/2out
   场景、`pin-shape-source` / `pin-shape-sink` 全部照常 PASS。
4. **文档**：`docs/sdk/custom-logic.md` 形状规则、`docs/sdk/services.md` 新服务一节、
   `reference/limits.md`、`changelog.md`、ABI 快照（`tools/abi.ps1 -Update`）。

## 明确不做（这一刀之外）

| 不做的事 | 为什么 | 出口 |
|---|---|---|
| 多字（>64 位）信号、字节视图、宿主借用缓冲 | 外调只有 4 个参数（实测），且游戏原版信号上限 64 位 | M2 的宿主状态/缓冲 |
| 动态引脚（按配置增删脚、断开导线、撤销记录） | 属于计划 §6.2 的编辑事务，需要 M3 的 Undo 基础 | 阶段 3 |
| `type_id` 分类/标签/依赖查询 | 与脚数无关，可以后做；先让脚数与值模型定型 | 阶段 1 收尾之后 |
| 实例句柄、生命周期回调、可配置状态规模 | 属于 M2 | 阶段 2 |

## 起手命令

```powershell
cd D:\p\tc-modloader
powershell -NoProfile -ExecutionPolicy Bypass -File build.ps1          # 离线层全绿再动真机
powershell -NoProfile -ExecutionPolicy Bypass -File tools/test.ps1 -Tier fast -NoBuild
powershell -NoProfile -ExecutionPolicy Bypass -File tests/pin-shape-playtest.ps1 -Shape source
```

真机用例的板子沿用 `tests/and-component-fixture.cpp`（新增一个 `wide9board` 分支）与
`tests/pin-shape-probe.cpp`（新增一个 9 脚形状 + 回调），跑法与现有两个 `pin-shape-*` 用例相同。
