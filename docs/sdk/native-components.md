# 声明式 C++ 元件

`sdk/tc_native_component.h` 提供 `tc::TCNativeComponent`（`tc_mod.h` 已包含）。
作者只需声明稳定 ID、引脚和回调，不需要编写、导出或打包 `circuit.data`。
加载器在 `tc_mod_load` 内自动生成连接结构、导入原型、写入声明代价并注册逻辑。

```cpp
#include "tc_native_component.h"

static void add(TCLogicIO* io) {
    if (io->phase == TC_LOGIC_RESET) return;
    const uint64_t sum = io->inputs[0] + io->inputs[1] + io->inputs[2];
    io->outputs[0] = sum & 255;
    io->outputs[1] = (sum >> 8) & 1;
}

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* host, TCPlugin*) {
    tc::TCNativeComponent c;
    c.id = 0x414444385F303031ULL; // 与示例相同；自己的 Mod 请使用自己的稳定 ID
    c.name = "Byte Adder";
    c.description = "Carry + A + B";
    c.inputs = {{"Carry", 1}, {"A", 8}, {"B", 8}};
    c.outputs = {{"Sum", 8}, {"Carry out", 1}};
    c.gates = 1;
    c.delay = 1;
    c.callback = add;
    return c.registerWith(host);
}
```

## 契约

- `inputs` / `outputs` 的数组顺序就是回调的 `inputs[]` / `outputs[]` 顺序。
- 每方向 1–8 脚，每脚 1–64 位，总输入 ≤128 位；门数和延迟为非负且不超过 `INT64_MAX`。
- 名称和引脚名为 UTF-8；`shapeSvg` 可选。不提供时使用游戏默认形状。
- 引脚名指针只需在 `registerWith` 调用期间有效；回调及 `user` 在插件运行期间有效。
- 仅在 `tc_mod_load` 期间调用。无需先加载 `TCMod` 或传入虚构的目录。
- 同 ID 已存在时拒绝，不覆盖其它元件；注册失败会移除本次新建的原型。
  插件初始化最终失败时，本插件通过此接口创建的原型也会移除。
- 不支持热卸载；存档保存稳定 ID，再次启动由同一 Mod 重新注册定义。
- `CYCLE` 和 `REFRESH` 都应写输出。只有 `CYCLE` 提交 `state[8]`，刷新不推进状态。
- 声明延迟影响统计与分数，不会自动把输出延后几个周期。

## 宿主兼容与错误

C ABI 是 `TCHost` 尾部新增的 `register_component`，参数为
`TCNativeComponentDefinition`，引脚为 `TCComponentPin`。宿主 ABI 仍为 1；
`TCNativeComponent::available(host)` 会先检查结构大小，旧加载器返回不可用，
不会访问尾部之外的内存。原有 `register_logic` 和电路导入接口保留。

| 返回值 | 含义 |
|---|---|
| 0 | 已注册；插件入口成功后激活 |
| -1 | 宿主能力不可用，或不在加载阶段 |
| -2 | 描述、引脚数、位宽、ID、回调或代价不合法 |
| -3 | ID 冲突 |
| -4 | 游戏导入或原型操作失败 |
| -5 | 逻辑桥注册失败 |

## 实现与验证边界

加载器维护私有生成结构：每个输入一个采集节点、依赖链、每个输出一个驱动节点。
这保证全部输入就绪后调用一次回调，后续输出读取同一次结果；作者不需要理解其布局。
内部使用逻辑定义版本 3 标记生成结构，手写电路仍使用版本 2 的原有规则。
这不是任意内部电路的自动桥接接口。

真实游戏测试覆盖普通 3 输入／2 输出字节加法器；8 输入／8 输出压力用例把重复输入
接到后五个输入，并让关卡判定读取最后两个输出，验证采集顺序和末端输出驱动。
两者都验证界面刷新与 40 周期关卡无失配，不表示完整通关。
`-Single` 用例验证单输入／单输出字节元件，通过 `double_number` 的全部 16 周期。
单元测试另覆盖参数拒绝、旧宿主、跨 64 位边界的 128 位输入打包。
16/32/64 位游戏关卡、有状态元件及字宽输出 32 token 限制仍按
[验证范围](../verification.md) 与 [限制](../reference/limits.md) 执行。

```powershell
./build.ps1
./tests/byte-adder-smoke.ps1
./tests/native-component-playtest.ps1
./tests/native-component-playtest.ps1 -Single
```
