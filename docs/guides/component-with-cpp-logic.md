# 教程：让元件行为由 C++ 决定

**新元件推荐直接使用 [声明式 C++ 元件接口](../sdk/native-components.md)**：
填写 `TCNativeComponent` 的 ID、名称、输入／输出和回调，再调用 `registerWith(host)`。
加载器自动生成定义，作者无需准备 `circuit.data`。`examples/byte-adder` 已使用此路径。
下面保留已有电路文件的低层导入教程。

目标：在游戏里放一个自定义元件，它的每周期行为由插件的 C++ 回调决定；关卡判定、其它
元件、暂停与重置仍由游戏负责。

前置阅读：[custom-logic.md](../sdk/custom-logic.md)（形状规则与 ABI）、
[game-model.md](../sdk/game-model.md)（电路导入）。

## 1. 准备一份元件定义

元件定义是 `circuit.data`。两种来源：

- 用游戏的元件工厂做一个设计并导出，再把它当作定义文件读入；
- 或用夹具工具生成（见 `tests/and-component-fixture.cpp`：写出引脚、门与连线，
  `writeLiteralSnappy` 负责打包）。

定义只要满足：[custom-logic.md#写元件定义](../sdk/custom-logic.md#写元件定义) 里的引脚
几何与字段顺序要求，内部电路可以只是占位门——它的作用只是决定回调被插入的位置与输入元组。

## 2. 导入并注册回调

```cpp
#include "tc_mod.h"

static tc::TCMod mod;

// 两个 8 位输入、一个 8 位输出：输出 = a ^ b
static void xorBytes(TCLogicIO* io) {
    if (io->phase == TC_LOGIC_RESET) { io->outputs[0] = 0; return; }
    io->outputs[0] = (io->inputs[0] ^ io->inputs[1]) & 0xff;
}

extern "C" TC_MOD_EXPORT int tc_mod_load(const TCHost* host, TCPlugin* plugin) {
    if (host->api_version != TC_MOD_API_VERSION) return 1;
    if (!mod.load(host) || !mod.valid() || !mod.components.valid()) return 2;

    const auto bytes = readDefinitionFile(host);          // 你自己的读取方式
    auto imported = mod.components.importCircuit("XOR byte", bytes.data(), bytes.size(),
                                                 "D:/my-mod/");
    if (!imported.ok()) return 3;

    TCLogicDefinition definition{sizeof(TCLogicDefinition), 2, imported.custom_id,
                                 &xorBytes, nullptr};
    if (host->register_logic(host->context, &definition) != 0) return 4;

    host->log(host->context, "xorb: registered");
    return 0;
}
```

要点：

- `register_logic` 只能在 `tc_mod_load` 期间调用。
- 定义决定回调形状：引脚数、每脚位宽、回调 `inputs[]` 的顺序（第一个输出门的操作数顺序）。
- 回调运行在仿真线程：不要碰 UI、模型 API，不要抛异常，不要保存 `TCLogicIO*`。

## 3. 看清日志确认形状

启动后加载器日志会出现注册行，含实测引脚数与偏移；把它和你的定义对照：

```text
Native logic: registered custom 0x584f52385f303031 inputs=2 outputs=1 in0=(-2,0,w8) in1=(-2,1,w8) out0=(2,0,w8) shape=2in/1out
Native logic: bound instance 0x2222222222222222 of custom 0x584f52385f303031 as token 1
Native logic: emitted 1 callback(s), mode=1
Native logic: emitted 1 callback(s), mode=0
```

- 只有 `registered` 没有 `bound`：游戏里没有放置该元件，或原理图引用的 ID 不是这个定义。
- 出现 `has no recognised internal logic node` / `exposes N operand(s)`：定义不满足形状规则，
  内部电路被保留执行（回调不会跑），日志会附带原样发射行。

## 4. 放置与验证

1. 在 Sandbox 或 Foundry 打开元件列表放置该元件（部分战役关卡按游戏规则禁止自定义元件）。
2. 用游戏自带的关卡测试判定行为：例如 `byte_xor` 关卡要求输出 `a ^ b`，把元件接在关卡
   输入／输出之间，运行后关卡自己会判定。
3. 需要自动化回归时，参照 `tests/custom-or-playtest.ps1` 的做法：复制游戏到隔离目录、
   切换 `USERPROFILE`/`APPDATA`、把原理图放进
   `profiles/default/schematics/<level>/Default/circuit.data`，再断言游戏日志与关卡判定。

断言应尽量取自游戏自身：关卡判定结果、关卡输出脚历史、UI 表格文本；插件日志只用来
补充“回调被调用了多少次、交换了什么值”。字宽关卡需要较长测试时，可只跑有限周期并断言
“尚未出现失配”，再在测试脚本里独立复核回调交换的数值，见
[../verification.md](../verification.md)。

## 5. 常见坑

| 现象 | 原因 |
|---|---|
| 回调一次都没跑 | 内部节点不在识别范围、操作数数量与定义不符，或元件没被放置/没接到关卡 IO |
| 关卡看到的值是 0 或空白 | 输出脚没有被驱动（连线没接到引脚）或宽度不匹配 |
| 导入被拒绝 | 多脚定义的引脚几何/字段顺序不符合游戏写法，见 custom-logic 的“写元件定义” |
| 关卡判定与插件日志矛盾 | 原理图里复制了关卡自带的 IO 元件，导致板上出现两套输出脚；只放门电路 |
