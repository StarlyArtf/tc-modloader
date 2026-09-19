# 单字节加法器示例（1 门 / 1 延迟）

本示例使用 `tc::TCNativeComponent` 声明引脚和 C++ 回调，加载器自动创建元件。
插件不读取或生成 `circuit.data`，也不再包含 `byte_adder_fixture.hpp`。
接口用法见 [声明式 C++ 元件](../../docs/sdk/native-components.md)。

注册一个自定义元件：**进位入（1 位） + A（8 位） + B（8 位） → 和（8 位） + 进位出（1 位）**，
行为由插件里的 C++ 回调决定：

```text
sum       = (carry_in + A + B) & 0xFF
carry_out = (carry_in + A + B) >> 8
```

定义里声明的设计统计固定为 **1 门 / 1 延迟**：

- 元件信息面板显示 1 门、1 延迟；
- 板级编译统计按声明值计入（串联相加、并联取最长、嵌套只计外层一次）；
- 内部电路只是占位门，实际行为来自回调，游戏仍负责关卡判定、连线、暂停与重置。

## 怎么测

1. 把 `dist/example.byte-adder.mod` 放进 `<游戏目录>/mods/`（不要解压）。
2. 启动游戏 → 主菜单 Mods → 勾选 `example.byte-adder` → 应用更改 → 重启。
3. 在 **Sandbox** 或 **Foundry** 打开元件列表，放置 `Byte Adder`：
   - 两个 8 位输入接关卡/常量，1 位输入接进位入；
   - 输出 8 位和与 1 位进位出；
   - 元件面板应显示 **1 门 / 1 延迟**。
4. 用战役关卡 **Byte Adder**（`campaign/byte_adder`）做端到端验证：该关卡自己会把
   `carry_out << 8 | sum` 与 `carry_in + A + B` 比较，判定通过即为正确。
   部分战役关卡会按游戏规则禁止自定义元件，此时用 Sandbox 配一两个常量输入自行比对。

## 两个阶段都要算

回调会被调用两次语义不同的路径，**两个阶段都要写 `outputs[]`**：

| 阶段 | 谁在读 | 该做什么 |
|---|---|---|
| `TC_LOGIC_CYCLE` | 仿真 / 关卡判定 | 写 `outputs[]`，可写 `state[]` |
| `TC_LOGIC_REFRESH` | 界面与元件工坊的实时表格 | 同样写 `outputs[]`，但不要推进 `state[]`（跑在副本上） |
| `TC_LOGIC_RESET` | 游戏重置 | 清零 `outputs[]` 与 `state[]` |

只写 `CYCLE` 的元件在仿真里是对的，但界面/元件工坊表格会一直显示上一次的旧值
（本示例早期版本就是这样，表现为"元件不会算加法"）。本示例现在共用一个
`computeBytes()`：`CYCLE` 与 `REFRESH` 都算，只有 `REFRESH` 跳过计数与状态。

想看回调收到的原始值，加载器日志（`tc-modloader-data/loader.log`）会打印前几个刷新与
前 8 个周期：

```text
byte-adder: peek carry_in=1 a=10 b=12 sum=23 carry_out=0
byte-adder: cycle=... carry_in=0 a=12 b=34 sum=46 carry_out=0
Native logic: registered custom 0x414444385f303031 inputs=3 outputs=2 in0=(-2,-1,w1) in1=(-2,0,w8) in2=(-2,1,w8) out0=(2,0,w8) out1=(2,1,w1)
```

一键复验：`tests/byte-adder-smoke.ps1`（隔离副本里跑真实包，同时断言刷新与周期两条路径，
并要求关卡 40 周期无失配）。

日志解读见 [docs/reference/diagnostics.md](../../docs/reference/diagnostics.md)。

## 边界

- 元件的引脚偏移由游戏计算：`in0=(-2,-1) in1=(-2,0) in2=(-2,1) out0=(2,0) out1=(2,1)`；
  写自建原理图时按注册日志里的实测值连线。
- 字宽输出的值经预留仿真状态槽回传（见
  [docs/sdk/custom-logic.md](../../docs/sdk/custom-logic.md)）。
- 该示例只演示 8 位加法；改动位宽需要同时改定义里的引脚 `bits` 和回调里的掩码。
