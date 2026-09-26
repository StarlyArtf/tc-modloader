# IEEE 754 浮点元件族实施方案

更新时间：2026-09-25  
状态：**方案已定，M0 兼容性、M1 数值内核与 M2 垂直切片均已完成（2026-09-25），M3 基础包可以
开始**；§12 只保留真正需要产品选择的事项。

M2 交付（`tests/float-ops-playtest.ps1`，真机闭环）：`FP32 Constant`（0 入 1 出，
配置 8 字节 schema 2 = format + display + bits）、`FP32 Add`（A/B 两输入 → R[32]+Flags[5]，
配置 4 字节 = format + 舍入模式）、`FP32 Display`（1 入 0 出，配置 4 字节 = format + 显示方式）。
三者共用原版风格本体：左上角固定 `32` 位宽框（悬停给出 FP16/FP64 未实现的提示）、其右侧舍入框
（点击开紧凑菜单，五个模式用 radio 行呈现并立即写回配置）、中央操作符、A/B/R/F 引脚；
Constant 与 Display 点本体开自己的小面板（文本编辑 / 显示方式）。配置读写走
`tc.component.storage`（有 V2 时用 begin/commit 包成一次撤销步），schema 1（无 format 字节的
开发期布局）有迁移路径。真机证据：默认 1.0+1.0 → Display 显示 `2 0x40000000`；
写入 3.5/1.25 → `4.75 0x40980000`；半 ULP 用例 1.0+2^-25 在 RNE 下 `1 0x3F800000`，
把舍入框改成 RUP 后 `1.0000001 0x3F800001`。十进制文本层：vendored fast_float（解析）+
Ryu（最短往返显示），黄金语料 93 条解析 + 383 条格式化 + 2 万条随机往返。

M2 尚未覆盖（有意留给后面）：鼠标点击路径的自动化（舍入菜单/文本面板的点击由真机脚本手工路径
验证，自动化点击用例留到 M5 产品化）、Split/Make Bits、FMA/Remainder 元件（M4）、
TestFloat 资格验证（M5）。

## 0. 结论

在当前 `tc-modloader` 中实现一个 `local.float-ops` Mod，先交付确定性的 IEEE 754 binary32
元件族，再按需求扩展 binary16/binary64。

主线方案：

- 浮点值仍是普通导线上的**原始位模式**；FP32 就是一根 32 位引脚，不增加游戏信号类型。
- 数值内核使用固定版本的 Berkeley SoftFloat 3e；宿主 `float` 只可用于非严格的开发对照，
  不作为产品语义。
- 对已实现的每项运算承诺“数值结果 + 默认异常标志”符合 IEEE 754-2019，NaN 位模式遵循本文
  固定的 canonical 策略；不宣称实现了
  整份标准的全部格式、操作、陷阱和推荐函数。
- 算术元件统一输出 `R[32]` 与本次运算的 `Flags[5]`。
- 浮点包只做独立 Mod，不修改 Loader；M0 先验证当前公开 SDK 和 Loader 支持范围。
- 第一条端到端切片是 `FP32 Constant → FP32 Add → FP32 Display`。
- 自动改造普通导线标签、菜单文件夹、FP64 和状态元件都不阻塞首版。

规范基线：

- [IEEE 754-2019](https://standards.ieee.org/ieee/754/6210/)
- [Berkeley SoftFloat 3e](https://github.com/ucb-bar/berkeley-softfloat-3)
- [Berkeley TestFloat 3e](https://github.com/ucb-bar/berkeley-testfloat-3)

## 1. 范围

### 1.1 首发范围

- 格式：IEEE 754 binary32。
- 类型：纯组合运算、常量、显示、分类、比较与基础转换。
- 外观预留 16/32/64 位统一布局，但首版位宽控件固定显示 `32`，不能切换到尚未实现的格式。
- 默认处理：非陷阱（non-stop）模式，异常通过输出引脚报告。
- 存档：稳定的 64 位 `custom_id` 与带 schema 的实例配置。
- 运行环境：当前 Windows x64 游戏构建与本仓库的 MinGW 工具链。

### 1.2 明确不做

- 不改原版整数元件；现有 32 位 Register、Mux、RAM 可以直接保存和传递 FP32 位模式。
- 不增加“浮点导线类型”或隐式类型传播。
- 首版不做十进制浮点、binary128、bfloat16、任意指数/尾数宽度。
- 首版不做 FP64 FMA：三个 64 位输入共 192 位，超过当前 128 位输入预算。
- 不让同名元件通过隐藏配置切换 NaN、饱和、FTZ/DAZ 等数值语义。
- 不用 `Mul + Add` 冒充 FMA。
- 不把全局 shader 和菜单树逆向研究放在运算核心的关键路径上。

## 2. IEEE 754 合同

### 2.1 合规表述

产品说明统一使用：

> 对已实现的操作，数值结果和本次异常标志符合 IEEE 754-2019 binary32；NaN 位模式遵循本项目
> 固定的 canonical 策略；采用非陷阱默认处理，支持有符号零、次正规数、无穷和 NaN。

不要写“完整实现 IEEE 754”，因为首版不会覆盖十进制格式、全部操作、陷阱处理和推荐数学函数。

### 2.2 固定语义

| 项目 | 约定 |
|---|---|
| 默认舍入 | `roundTiesToEven`（RNE） |
| 首发可选舍入 | RNE、roundTiesToAway、roundTowardZero、roundTowardNegative、roundTowardPositive |
| 次正规数 | 完整保留；不启用 FTZ/DAZ |
| tininess | 舍入后检测 |
| underflow | 结果同时 tiny 且 inexact 时置 UF；精确次正规结果不置 UF/NX |
| 异常处理 | 不陷阱；每次运算输出 flags |
| FMA | `a × b + c` 只舍入一次 |
| 确定性 | 相同输入、舍入模式和配置在所有受支持构建/工具链上得到相同位模式与 flags |

### 2.3 异常引脚

所有可能产生 IEEE 异常的元件输出一根 `Flags[5]`：

```text
bit 4  NV  invalid operation
bit 3  DZ  divide by zero
bit 2  OF  overflow
bit 1  UF  underflow
bit 0  NX  inexact
```

Flags 表示**本次组合运算**产生的异常，不是隐藏的全局粘滞状态。需要累计时，后续提供显式
`FP Exception Latch`，或由玩家用 OR + Register 组合。

### 2.4 NaN 策略

推荐固定为“计算 canonical、位操作保留”：

- FP32 canonical qNaN：`0x7FC00000`。
- 算术输入含 qNaN：返回 canonical qNaN；qNaN 本身不产生 NV，但不能屏蔽同一操作本来必须报告的 invalid。
- 算术输入含 sNaN：返回 canonical qNaN，置 NV。
- `0×Inf`、`Inf-Inf`、`0/0`、负数开方等无效运算：canonical qNaN + NV。
- 特别地，`0×Inf + qNaN` 的 FMA 仍返回 canonical qNaN 并置 NV，不能因先看到 qNaN 而提前返回。
- `Negate`、`Absolute`、`CopySign`、`Split Bits`、`Make Bits` 只操作位模式，
  保留 payload，不静默化 sNaN，也不置 NV。
- `Constant` 和 `Make Bits` 允许构造任意 NaN 位模式。

不做每实例 NaN 策略开关；否则相同名称的元件会有不可见的不同语义。

### 2.5 比较、分类与 min/max

`FP32 Compare` 定义为 quiet relation：

```text
输入：A[32], B[32]
输出：LT, EQ, GT, UN, Flags[5]
```

- 普通值恰有 LT/EQ/GT 一个为 1。
- `+0 == -0`。
- 任一 NaN 时 UN=1，LT/EQ/GT=0。
- qNaN 不置 NV；sNaN 置 NV。
- `LE` 由 `LT | EQ` 得到，不占独立引脚。

`FP32 Classify` 输出 `Class[10]`，位序固定：

```text
0 -Inf              5 positive subnormal
1 negative normal   6 positive normal
2 negative subnormal
3 -0                7 +Inf
4 +0                8 sNaN
                    9 qNaN
```

不发布含糊的 `Min` / `Max`。后续优先实现并明确命名
`FP32 MinimumNumber` / `FP32 MaximumNumber`；如需要 NaN-propagating 版本，再单独提供
`Minimum` / `Maximum`。

`MinimumNumber` / `MaximumNumber` 的固定语义：

- `minimumNumber(-0,+0) = -0`，`maximumNumber(-0,+0) = +0`；
- 一个 qNaN 和一个数字返回数字，不置 NV；
- 一个 sNaN 和一个数字返回数字并置 NV；
- 两个 NaN 返回 canonical qNaN；其中有 sNaN 时置 NV。

SoftFloat 3e 不提供 IEEE 754-2019 的这组 min/max，使用项目自己的整数分类与选择包装，并用
独立黄金向量验证。

### 2.6 转换

不要使用含糊的 `I2F` / `F2I`，也不提供“越界回绕”配置。类型按有符号性拆开：

- `I32 To FP32`、`U32 To FP32`；
- `FP32 To I32`、`FP32 To U32`；
- 后续再加 64 位整数与 FP64 转换。

转换输出 `Flags[5]`。NaN、无穷或越界置 NV；无效转换的整数结果必须在 M4 前冻结为一种
确定策略，推荐 RISC-V 风格饱和，见 §12。
`-0` 转换为有符号或无符号整数 0，不置 NV；负的非零值转无符号整数才按范围规则处理。

## 3. 当前工程映射

| 工程能力 | 浮点方案中的用法 |
|---|---|
| `tc.component.types` + `TCComponentTypeDefinitionV2` | 一个 Mod 注册全部固定宽度类型 |
| 每脚 1–64 位 | FP32 使用单根 32 位引脚；FP64 使用单根 64 位引脚 |
| 总输入 ≤128 位 | FP32 FMA 96 位可行；FP64 二元 128 位可行；FP64 FMA 不可行 |
| V2 每方向 0–16 脚 | Constant 是纯源；Display 是纯汇 |
| `state_words` | 首批运算和 Display 都设为 0；Display 使用插件自己的线程安全实例缓存，不借仿真 state |
| 配置与存档 | Constant 的 raw bits、算术元件舍入模式、Display 格式 |
| `component.render` / geometry | CPU 侧显示文本和统一棋盘外观 |
| `shape_svg` | 元件目录缩略图；不代替棋盘本体绘制 |
| 声明 gate/delay | 只影响统计与评分，不自动产生真实周期延迟 |

实例配置虽然内存层允许 64 KiB，但随原理图可靠持久化的预算按 1024 字节设计。

## 4. 独立 Mod 边界与兼容性门槛（M0）

### 4.1 交付边界

浮点族交付为独立的 `local.float-ops.mod`：

- Mod 自己携带 native DLL、SoftFloat、十进制解析/格式化代码和资源；
- 只使用已经公开的 SDK 头和服务；
- 不修改 `src/native_logic.hpp`、`loader.cpp`、游戏 DLL 或 Mod Loader ABI；
- 不向用户存档目录写入伪 schematic；
- 安装、禁用和删除该 Mod 不替换 Mod Loader 文件。

仓库中的构建脚本可以负责把它编译、测试和打包，但浮点逻辑不编入 `tc-loader.dll`。`mod.json`
声明实现实际调用的 capabilities（预期至少 `services`、`log`、`status`）；插件入口再用公开的
`tc::hostHas` / `tc::hostVersion` 以及精确的 `query_service` 版本和表大小检查，确认
`tc.component.types`、storage、lifecycle、instances 和 render 等所需服务。条件不足时返回加载失败，
并通过状态/日志给出清晰提示。当前 `mod.json` 没有最低 Loader 版本字段，不能虚构该字段。

### 4.2 接受当前 Loader 的已知边界

当前 Loader 的宽输出桥仍有以下既知限制，独立 Mod 不能从外部修复：

- 宽输出状态槽按全局 binding token 使用，可靠范围为前 32 个 token；
- 每槽 512 字节，天然覆盖前 8 个 64 位输出位置；
- 超过边界时，现有 Loader 的字宽输出不可靠。

实测（2026-09-25，`tests/float-boundary-playtest.ps1`，报告落在 `build/float-boundary-report.txt`）：
42 个实例的直连链上 token 1–33 读到完整 `0xDEADBEEF`，**token 33 是第一个没有自己的槽、因而
没有发布宽输出的 token**（`src/native_logic.hpp` 的 `token > kBridgeStateTokens` 直接跳过发布），
它的消费者 token 34 读到 `0x00000000`，其后每个实例同样读到 0；读取仍落在状态缓冲区内，没有
越界写入。也就是说“32 个 token”这条上限在真机上可复现，且失败是确定性的（读到空值），不是随机
损坏。

首批 FP32 类型最多只有 5 个输出，引脚位置均在前 8 个之内；一个 `FP32 Add` 仍只是一根
32 位结果、一个 5 位 Flags 和一个元件实例，不会因为 FP32 是 32 位就占 32 个槽。正常目标板按
5–20 个原生浮点实例设计，不为了假设的 200 个实例修改 Loader。

Mod 文档必须明确：“当前 Loader 下，整张板的原生 binding token 应保持在 32 以内”。如果未来
Loader 独立修复或扩容，本 Mod 只通过公开能力/服务版本探测开放更大范围，不把那项工作混进
浮点 Mod。

### 4.3 避开状态语义问题

当前 V2 `REFRESH` 的 state 提交行为与文档合同存在偏差，但首批 Mod 不依赖它：

- 所有算术、比较、分类和转换元件使用 `state_words=0`；
- Display 也使用 `state_words=0`，最近输入保存在插件自己的线程安全实例缓存；
- 回调不在 REFRESH 中修改仿真 state；
- 不提供 FP32 Register/Accumulator；原版 32 位状态元件已经能保存 FP32 位模式。

### 4.4 M0 兼容性测试

M0 只验证“当前 Loader 能否可靠承载这个独立 Mod”，不修改 Loader。测试内容：

- 32 位纯源、直通、双宽输出和纯汇测试元件；
- `0x00000000`、`0x00000001`、`0x7F7FFFFF`、`0x7F800000`、
  `0x7FC00000`、`0x80000000`、`0xFFFFFFFF`、`0xDEADBEEF` 逐位往返；
- CYCLE、REFRESH、暂停和 RESET 后组合输出稳定；
- 同一实例的 `R[32] + Flags[5]` 正确；
- Constant 的 0 输入与 Display 的 0 输出调度正确；
- 典型 5–20 个本 Mod 实例工作正常；
- 安装与卸载只改变 `local.float-ops.mod` 及其插件数据，不改变 Loader/游戏二进制。

另做一个 token 边界诊断用例，记录当前 Loader 在边界附近的行为，但不把扩槽或修 Loader 纳入本
Mod 里程碑。若 M0 在受支持范围内失败，先报告兼容性阻塞，不擅自扩大工程范围。

实施状态（2026-09-25，**M0 完成**）：

- 已建立 `examples/float-ops/` 独立源码、构建与 staging 目录，产物为
  `dist/local.float-ops.mod`；
- 已实现五个无状态探针：32 位纯源、32 位直通、`R[32] + Flags[5]` 双输出、`Flags[5]` 纯汇和
  32 位纯汇；
- 已通过离线伪宿主注册测试，八组位模式在 RESET/REFRESH/CYCLE 中逐位往返
  （`examples/float-ops/build.ps1` 调 `tests/float-compat.cpp`）；
- 已通过隔离 CLI 启用/停用测试（`tests/float-package.ps1`），只写 `mods/` 与
  `tc-modloader-data/`，未写入 Loader 或游戏二进制；
- 已通过隔离真机测试（`tests/float-compat-playtest.ps1`）：21 个实例按
  `source → pass ×17 → result+flags → sink` 串接并另接一只 `Flags[5]` 纯汇，实际绑定 token 1–21，
  逐实例四类回调都收到完整 `0xDEADBEEF`，`Flags[5]` 输出被它自己的汇读回 `0x0000001f`；
- 真机暂停/RESET 已有显式证据：测试专用驱动在板子编译完成后接管仿真，先 `run 6`、再暂停
  2.5 秒（暂停窗口内 `pause window stable at cycle=0`，五类探针各有两轮
  `refresh observed … value=0xdeadbeef/0x0000001f`）、再 RESET、再 `run 6`，复位后的周期线
  仍带完整位模式；
- token 边界已诊断（`tests/float-boundary-playtest.ps1` + `build/float-boundary-report.txt`）：
  42 个实例的链上 token 1–33 读到正确位模式，**token 33 是第一个没有状态槽、无法发布宽输出的
  token**，其消费者 token 34 读到 `0x00000000`；这与 §4.2 记录的 32 token 上限一致，未触发
  任何越界写入；
- 测试专用关卡驱动（真机用例）不进入生产 `.mod`。

结论：当前 Loader 在“整板原生 binding token ≤ 32”的范围内可靠承载本 Mod，M0 可以结束，M1
（数值内核）可以开始。

## 5. 数值内核

### 5.1 选择

推荐 vendored Berkeley SoftFloat 3e：

- 固定源码版本和提交哈希；
- 保留许可证并加入第三方许可证清单；
- 只编译首期需要的 binary32 源码，后续格式按需加入；
- 固定使用 default-NaN specialization，包装层仍将所有算术 NaN 结果 canonicalize，避免不同
  specialization 改变公开合同；
- SoftFloat 的 rounding、tininess 与 exception flags 变量优先编译为 TLS；如果工具链不能提供 TLS，
  必须用互斥保护完整操作，或以真机证据证明所有调用严格单线程。只做 save/restore 不能防止线程交错；
- 每次操作前设置舍入模式、固定 tininess-after-rounding、清空 flags；
- 操作后立即复制结果位模式与本次 flags，并把 SoftFloat 的 `softfloat_flag_infinite` 映射到公开 `DZ`。

禁止依赖游戏线程当前 MXCSR、宿主舍入环境、FTZ/DAZ 或编译器的 NaN 行为。

### 5.2 内核接口

```cpp
enum class FPRounding : uint8_t {
    nearest_even,
    ties_away,
    toward_zero,
    toward_negative,
    toward_positive,
};

struct FP32Result {
    uint32_t bits;
    uint8_t flags;
};

FP32Result fp32_add(uint32_t a, uint32_t b, FPRounding mode);
FP32Result fp32_fma(uint32_t a, uint32_t b, uint32_t c, FPRounding mode);
```

内核只接收/返回整数位模式，不把 `float` 放进公共接口。

### 5.2b 实施状态（2026-09-25，M1 完成）

代码在 `examples/float-ops/fp/`：

| 文件 | 内容 |
|---|---|
| `fp32.hpp` | 公开接口：`FPRounding`（五个归档值）、五位 flags、`FP32Result`、`FP32Relation`、十个分类位，以及十二个运算入口 + 谓词 |
| `environment.hpp` | SoftFloat 状态粘合：每次调用设置舍入模式与 tininess-after-rounding、清空 flags、把 SoftFloat 的 flag 字映射成公开五位、把算术 NaN 结果 canonicalize |
| `fp32.cpp` | 十二个算术语义；quiet compare、分类、2019 min/max 与谓词用纯整数实现（它们是本项目契约，不随库升级漂移） |
| `softfloat/platform.h` | 上游 Win64-MinGW-w64 配置 + `THREAD_LOCAL`（见下） |
| `softfloat-sources.txt` | 只编译的 19 个编译单元（符号闭包算出，过程记录在 vendor 的 README） |

固定的事实：

- vendored 提交 `a0c6494cdc11865811dec815d5c0049fba9d82a8`（2025-03-07），许可证随源码；
- specialization 用 `ARM-VFPv2-defaultNaN`：`defaultNaNF32UI = 0x7FC00000`，
  `softfloat_propagateNaNF32UI` 恒返回该 canonical NaN，sNaN 才置 NV。`8086` 不可用（默认 NaN 是
  `0xFFC00000` 且传播 payload）；
- `platform.h` 定义 `THREAD_LOCAL`（C 用 `_Thread_local`、C++ 用 `thread_local`），
  `softfloat_state.c` 的舍入/检测/异常标志因此按线程隔离；离线测试用两个线程各 20 万次运算交叉验证，
  真机 DLL 里导出的是 TLS 版本的符号（`nm` 可见 `_ZTH…softfloat_roundingMode`）；
- 每次操作都显式写 `softfloat_detectTininess = softfloat_tininess_afterRounding`——该 specialization
  自己的默认是 before-rounding，这一行是契约而不是重复默认值；
- 映射：`invalid→NV`、`infinite→DZ`（SoftFloat 用这个 flag 表示有限数除以零）、`overflow→OF`、
  `underflow→UF`、`inexact→NX`；
- 已实现：add/sub/mul/div/sqrt/fma/remainder/roundToIntegralExact、negate/absolute/copySign、
  compare、classify、minimumNumber/maximumNumber 与六个谓词；
- 尚未实现（按里程碑）：整数转换（M4，且要先定 §12 的 D2）、十进制 parse/format（M2，需要
  vendored fast_float 与 Ryu）、FP64/FP16。

### 5.3 十进制输入与输出

SoftFloat 不负责字符串转换，因此单独处理：

- Constant 编辑器提交时把文本解析一次，配置的**事实来源是 `uint32_t bits`**；
- 接受普通十进制、科学计数法、`inf`、`-inf`、`nan`、`-0`；
- 接受 `bits:0xXXXXXXXX`，用于 sNaN、payload 和精确测试向量；
- 十进制解析固定 locale、固定按 RNE 正确舍入到 binary32，不在仿真周期内重复解析；
- 十进制解析固定 vendored `fast_float` 版本，Display 固定 vendored Ryu 版本并输出
  shortest-round-trip；两者及许可证随 Mod 源码一起锁定；
- binary32 往返显示按最多 9 位有效数字设计，不再假设 7 位足够。

非法语法不提交并保留旧值；数值文本溢出不是语法错误，应显示转换结果和警告。解析时发生的
overflow/underflow/inexact 是编辑器诊断，不是 Constant 的运行时 Flags 输出。

## 6. 元件目录

命名统一用 `FP32 …`，以后可以自然扩展成 `FP64 …`。

### 6.1 垂直切片

| 元件 | 输入 | 输出 | 配置 |
|---|---|---|---|
| `FP32 Constant` | 无 | R(32) | raw bits；编辑器接受十进制或位模式 |
| `FP32 Add` | A(32), B(32) | R(32), Flags(5) | 五种舍入模式，默认 RNE |
| `FP32 Display` | A(32) | 无 | shortest/scientific/hex 等显示偏好 |

这三个元件先验证“十进制输入 → 运算 → 人读结果”的完整体验；Add 的 Flags 由真机向量汇捕获，
不把尚未设计的玩家可视化解码器塞进垂直切片。

### 6.2 首个公开基础包

在垂直切片通过后加入：

- `FP32 Subtract`；
- `FP32 Multiply`；
- `FP32 Divide`；
- `FP32 Square Root`；
- `FP32 Negate`；
- `FP32 Absolute`；
- `FP32 Compare`；
- `FP32 Classify`。

Negate/Absolute 是位操作，不需要 Flags 或舍入配置。其余可能异常的操作统一输出 Flags。

### 6.3 完整 binary32 扩展

- `FP32 FMA`：三输入共 96 位，单次舍入；
- `FP32 Remainder`：明确为 IEEE remainder，不是 `fmod` 或整数 modulo；商的整数选择固定为最近、
  平局取偶，不受实例舍入模式控制，因此不显示舍入配置徽标；
- `FP32 Round To Integral Exact`：发生舍入时置 NX；
- `FP32 MinimumNumber` / `MaximumNumber`；
- I32/U32 与 FP32 双向转换；
- `FP32 Split Bits` / `Make Bits`；
- 可选 `FP Exception Latch`。

不增加 `FP32 Register`：原版 32 位 Register 已能无损保存 FP32 位模式。

### 6.4 后续格式

binary32 稳定后，再按需求增加：

- binary16；
- binary64 的 Constant、Display、二元运算、比较、分类和转换；
- 不提供普通形态的 FP64 FMA，除非以后增加多周期装载协议。

## 7. 配置与交互

### 7.1 原则

- 相同名称的元件具有相同数值语义。
- NaN、FTZ/DAZ、转换回绕等不做隐藏开关。
- 有配置的元件必须在棋盘本体上显示当前配置。
- 配置写入使用现有 storage 服务，支持复制、存档、迁移和撤销/重做。
- 配置 schema 从首版就保留 `format` 字段；当前唯一合法值是 binary32。binary16/binary64 实现并通过
  各自验证门槛后，才允许该字段取 16/64，旧存档继续读成 32。
- 所有受舍入模式影响的算术与转换元件都按实例保存舍入模式，默认 RNE；不同实例可以选择不同模式。

### 7.2 外观

棋盘本体参考原版元件风格，不做独立悬浮面板式皮肤：沿用原版的底色、边框粗细、圆角、字体层级、
选中反馈、引脚锚点和紧凑间距。复用 Clock/Text Box 已验证的 `component.render`、geometry、storage
与命中测试路径，但视觉基准以原版元件为准。

**2026-09-25 定稿的量化基准**（玩家截图 + `com_constant.png` 实测，见
[verification.md](verification.md) 的"同一用例的外观断言"一节）：本体按原版 Constant 的
**4.92 × 2.93 格**画；名字右对齐、右缘距本体右缘 0.21 格、字高 0.40 格；位宽框左上内缩 0.17 格、
1.00 × 0.66 格；值/操作符居中、字高 0.59 格；**引脚在 3.0 通道上（本体外面 0.54 格）**——
原版 Constant / Static Value 的 kind 表条目就是 `out0=(3,0)`。为此加载器这一轮加了
`TCComponentTypeDefinitionV2::pin_lane`（生成引脚的板面半宽，默认 2.0 逐字节不变）与
`TCComponentRenderDrawV2`（按像素字号画字/量字），两者的实测与证据见
[research/custom-component-pins.md](research/custom-component-pins.md)。

统一布局：

- 左上角第一个小方框是位宽框，文本为 `16` / `32` / `64`；首版固定显示 `32`，点击不改变值，
  tooltip 明确提示 FP16/FP64 尚未实现；
- 位宽框右侧是舍入框。对受舍入影响的元件可点击选择五种模式，紧凑代码为 `RNE`、`RNA`、
  `RTZ`、`RDN`、`RUP`，tooltip/选择菜单同时显示完整名称；默认 `RNE`；
- 舍入选择使用原版风格的紧凑弹出菜单，当前模式始终直接显示在元件本体上，不依赖悬停才可见；
- Negate、Absolute、Compare、Classify、Remainder、Split/Make Bits、Constant 和 Display 等不受实例
  舍入模式影响的元件不显示可操作的舍入框，避免制造无效配置；
- 中央为 `+`、`−`、`×`、`÷`、`CMP` 等操作符；
- 引脚统一为 A/B/C、R、F；
- Constant 和 Display 使用自己的小型编辑面板；
- 目录 SVG 与棋盘本体使用同一套轮廓、操作符和位宽标识；M0 探针可以保留默认绘制，但 M2 的
  Constant/Add/Display 垂直切片必须完成这套原版风格外观与交互。

舍入框代码固定映射如下，不能因本地化改变存档值：

| 显示 | IEEE 754 模式 | 内部枚举 |
|---|---|---|
| `RNE` | roundTiesToEven | `nearest_even` |
| `RNA` | roundTiesToAway | `ties_away` |
| `RTZ` | roundTowardZero | `toward_zero` |
| `RDN` | roundTowardNegative | `toward_negative` |
| `RUP` | roundTowardPositive | `toward_positive` |

位宽和舍入分别占一个小方框；不要再把显示方式、NaN、饱和或其他语义塞进这两个控件。

## 8. 显示与菜单：从主线后置

### 8.1 Display

首版的人读出口是显式 `FP32 Display`：

- 它是一个 32 位纯汇元件；
- `state_words=0`；逻辑回调按 `instance_id` 更新插件自己的原子值或受锁保护缓存，渲染线程安全读取，
  `on_destroy` 时删除对应缓存；
- CPU 侧格式化后通过 `component.render::text` 绘制；
- 正确显示 `-0`、`inf`、`-inf`、`nan` 和有限数；
- 可同时显示十进制与 `0xXXXXXXXX` 位模式。

### 8.2 普通导线标签

自动把普通导线标签改成浮点属于独立实验线，不阻塞发布：

- 游戏没有类型系统，值经过 Mux、Register 或逻辑门后无法可靠推断“仍是浮点”；
- 当前 shader 字形缺少 `i` / `n`，7 位有效数字也不能保证 binary32 往返；
- shader 中实现完整 shortest-round-trip 的复杂度远高于原计划估计。

若以后做，只能明确选择：

- 紧凑、有损的导线预览；或
- CPU 预格式化后送字形，而不是在 shader 中重写完整 Ryu。

### 8.3 菜单分类

已落地（2026-09-26，路线 2，见 `docs/research/palette-categories.md`）：游戏自己的元件菜单树
**按自定义原型名字里的 `/` 分层**（`add_to_menu_tree__presenterZutilities_u12208`），所以元件注册成
`浮点/FP32 Add` 之后，是**游戏自己**在 `自定义` 里建出一个 `浮点` 文件夹并把 22 个元件放进去，
Mod 不画元件栏、不改游戏内存、不写玩家存档。真机门禁 `tests/float-palette-playtest.ps1`。
不向玩家存档目录部署伪 schematic 这条退路仍然不用。仍未做：把该文件夹搬到顶级分类页（只
能改游戏自己的菜单树内存，风险评估见 `docs/research/palette-categories.md`）。

## 9. Gate cost 与延迟

删除原方案中 `200/600/1500` 门和 `60/200/400` 延迟等无证据数字。

原因：

- 声明 delay 只影响统计与评分，不会让组合回调真的晚若干周期输出；
- 没有参考电路或综合模型时，“诚实代价”只是猜测；
- 一个同周期完成却显示数百延迟的黑盒会误导玩家。

发布前必须先确定产品定位：

- 沙箱/教学包：明确不承诺战役计分平衡，使用中性元数据；
- 计分平衡包：另建参考实现或成本模型，并决定是否实现真实多周期时序。

## 10. 验证与发布门槛

### 10.1 离线数值层

- 发布候选版使用固定版本 TestFloat 对本工具链中的 SoftFloat 构建运行 `testsoftfloat` 资格验证；
  TestFloat 是发布验证工具，不打进运行时 `.mod`，也不要求每次 fast build 全量执行；
- 从独立 oracle 生成并提交精简黄金语料到 `tests/data/float32/`，fast 测试覆盖所有已实现操作和
  舍入模式；
- 定向覆盖 ±0、次正规边界、最大有限数、Inf、qNaN、sNaN、半 ULP、overflow、underflow；
- FMA 必须包含能区分 fused 与先乘后加的向量；
- 值位模式和五位 flags 都逐位比较；
- 生产内核不能只拿自己生成的结果作为 oracle。
- canonical NaN 包装、2019 min/max、十进制 parse/format、无效整数转换返回值使用项目自有的
  固定黄金向量，因为 TestFloat 不覆盖这些项目合同。
- Remainder 定向覆盖 NaN、无穷、除数零以及“商恰好处于平局”的情况，断言它不受实例 RM 影响。

M1 实施情况（2026-09-25）：

- oracle 是 `tools/float-vectors.py`：用 Python 的精确有理数与整数运算独立实现 binary32 的舍入、
  tininess-after-rounding、OF/UF/NX 判定、canonical NaN、quiet compare、分类、2019 min/max 与
  IEEE remainder。它不看 C++ 实现，也不调用宿主 `float`（除下面第二条的交叉验证）；
- 语料落在 `tests/data/float32/fp32-vectors.generated.hpp`，由上面这个脚本生成：
  7571 条向量，覆盖十二个入口与五种舍入模式，值位与五位 flags 都逐位比较；
- oracle 自己带两层校验，任一层失败就不写文件：**960 条**最近舍入向量与 CPython double 算术
  “先 double、再一次舍入到 binary32”的结果逐位一致（对 + − × ÷ √ 成立，因为 double 的位数
  超过 2p+2）；每一族结果还要满足按定义写的性质检查（定向模式必须落在定义要求的那一侧且其相邻值
  必须跨过精确值；最近模式必须在半个 ulp 以内；开方用平方比较避免出现无理数）。这两层校验在开发中
  真的抓到了两个 oracle 自身的错误（RDN/RUP 方向写反、remainder 的符号处理），所以向量不是
  “自己算一遍再自己信一遍”；
- 语料包含 ±0、次正规边界（1 ulp、最大次正规、最小正规）、2 的幂与半 ulp、最大有限数、
  ±Inf、qNaN/sNaN（含 payload）、`0×Inf`、`Inf-Inf`、`0/0`、`x/0`；
- FMA 语料专门包含能区分 fused 与先乘后加的向量（如 `fma(1+ulp,1+ulp,-(1+ulp)) = 0x34000001`，
  先乘后加给 `0x34000000`），rem 覆盖 NaN、Inf、除数为零与商落在平局的情况；
- fast 门禁是 `tests/float-kernel.cpp`（由 `examples/float-ops/build.ps1` 编译并执行）；发布候选
  仍按上面第一条跑 TestFloat 资格验证，那一项要到 M5 产品化才做。

### 10.2 插件与真机层

新增：

- `tests/float-kernel.cpp`；
- `tests/float-compat-playtest.ps1`；
- `tests/float-ops-playtest.ps1`；
- 32 位向量源与捕获汇测试插件；
- `tests/test-catalog.json` 的 fast/game 条目。

真机夹具按周期发送原始位模式并捕获结果，不依赖现有 32 位战役关卡。

必须验证：

- CYCLE/REFRESH/RESET；
- 多输出与宽槽分配；
- 原版 Splitter 能否方便拆出 `Flags[5]` 和 `Class[10]`；若 10 位分类总线不可用，Classify 改成
  具名的 1 位输出，而不是把易用性问题留给玩家；
- Constant 配置保存、复制、撤销/重做、重启回读；
- Display 的线程安全和暂停刷新；
- 每种受舍入影响的元件都能选择并持久化 RNE/RNA/RTZ/RDN/RUP，不同实例的模式互不污染；
- FP32 阶段位宽框始终为 `32`，点击、复制、存档和旧配置迁移都不能产生未实现的 16/64；
- 原版风格外观在四个旋转方向、常用缩放、悬停/选中、拖动和引脚连线下可读；两个左上角方框的
  点击命中不应破坏元件本体拖动；
- Mod 缺失与恢复仍保留配置；
- 典型 5–20 个 FP32 实例的回调数与帧率观感。

只有真实用例必须越过当前 binding-token 兼容边界时，才另行讨论要求具备相应修复的新 Loader；
浮点 Mod 本身不执行 Loader 扩容，也不把 200 个实例当作首发门槛。

### 10.3 构建与打包

增加：

```text
examples/float-ops/
  mod.json
  build.ps1
  README.md
  plugin.cpp
  fp/
    environment.hpp
    fp32.hpp
    decimal.hpp
  native/
  third_party/
    berkeley-softfloat-3/
    fast_float/
    ryu/
    LICENSES/
tests/float-kernel.cpp
tests/data/float32/
tests/float-compat-playtest.ps1
tests/float-ops-playtest.ps1
```

`examples/float-ops/build.ps1` 单独完成插件构建、测试和 staging。源码树不能直接作为包根；脚本只把
Loader 接受的路径放进临时 staging：

M0 已经在树里的文件（其余条目属于 M1 及以后）：

```text
examples/float-ops/{mod.json,build.ps1,README.md,plugin.cpp}
tests/float-compat.cpp           离线伪宿主：五个 V2 形状 + 八组位模式
tests/float-package.ps1          离线形状 + 隔离 CLI 启停
tests/float-fixture.cpp          生成 21 实例 / 42 实例（--boundary）两块板
tests/float-compat-driver.cpp    只用于真机的关卡驱动（跑/暂停/RESET/再跑）
tests/float-compat-playtest.ps1  真机 21 实例闭环
tests/float-boundary-playtest.ps1 真机 42 实例 token 边界诊断
```

M1 加进来的文件：

```text
examples/float-ops/fp/{fp32.hpp,environment.hpp,fp32.cpp,softfloat-sources.txt}
examples/float-ops/fp/softfloat/platform.h
examples/float-ops/build-kernel.ps1   编译 SoftFloat 子集 + 内核，产出共享对象清单
examples/float-ops/third_party/berkeley-softfloat-3/   vendored 子集（19 个编译单元 + 头文件 + 许可证）
tests/float-kernel.cpp                离线数值测试（读下面的黄金语料）
tests/data/float32/fp32-vectors.generated.hpp
tools/float-vectors.py                独立 oracle 与生成器
```

```text
build/float-ops-package/
  mod.json
  native/
    float-ops.dll
    assets/...
    licenses/...
```

SoftFloat、fast_float 与 Ryu 静态编进 `float-ops.dll`，源码不放进运行包；各许可证复制到
`native/licenses/`。然后调用
`tools/Pack-Mod.ps1 -Source build/float-ops-package -Output dist/local.float-ops.mod` 得到独立安装包。

默认把它作为独立下载的 Mod 单独发布，因此不修改 Loader 的 `release/manifest.json`，也不会自动
进入 Loader 玩家包。若以后希望随主发行包附带，只需另做发行清单决策；这仍是外置 `.mod`，不代表
把浮点逻辑编进 `tc-loader.dll`。实现改动仅限 float-ops 源码、浮点测试和文档，不改 Loader 源码或
ABI。

## 11. 实施阶段

| 阶段 | 产物 | 通过后才能进入 |
|---|---|---|
| M0 Mod 兼容性 ✅ | 独立包骨架、32 位源/直通/双输出/汇探针、安装卸载边界（已通过，见 §4.4） | M1 |
| M1 数值内核 ✅ | SoftFloat 包装、五舍入模式、flags、离线测试（已通过，见 §5.2b 与 §10.1） | M2 |
| M2 垂直切片 | Constant + Add + Display，原版风格本体、固定 `32` 位宽框、五模式舍入框、配置和真机闭环 | M3 |
| M3 基础包 ✅ | Sub/Mul/Div/Sqrt/Neg/Abs/Compare/Classify（已交付，见 §10.4 与 changelog） | M4 / 首次公开测试 |
| M4 FP32 扩展运算 ✅ | FMA、remainder、roundToIntegralExact、min/max Number、I32/U32 双向转换、Split/Make Bits（已交付，见 §10.4） | M5 |
| M5 产品化 | 全元件外观收口、本地化、菜单探针、成本政策 | 稳定发布 |
| M6 扩展 | binary16/binary64、可选导线标签、按新版 Loader 能力扩大兼容范围 | 独立决策 |

按阶段留下适用证据并写入该 Mod 的验证记录：M1 是纯离线数值验证；M0、M2 及以后同时保留
离线测试和真机证据。

## 12. 决策记录与用户选择

### 12.1 已收敛为工程基线

以下事项由“IEEE 754 + 当前工程约束”直接决定，不再逐项阻塞用户：

- 使用 vendored SoftFloat 3e，不用宿主 `float` 定义产品语义；
- 所有可能异常的元件公开 `Flags[5]`；
- 算术结果使用 canonical qNaN，位操作保留 payload；
- 先完整做好 FP32，再考虑 FP64；
- 不修改 Loader；M0 记录并守住当前 binding-token 兼容边界，未来只通过公开能力/服务版本探测
  扩大支持范围。

M0/M1 可以按这些基线直接开始。

### 12.2 用户已决定

**D1：首发 UI 开放五种舍入模式。**

- 每个受舍入影响的元件按实例选择 RNE/RNA/RTZ/RDN/RUP，默认 RNE；
- 左上角先显示固定 `32` 位宽框，右侧显示可调舍入框；
- 位宽框为未来 binary16/binary64 预留，但对应格式完成前不可选；
- 元件外观参考原版风格，M2 就完成垂直切片的真实外观，不把它全部推迟到产品化阶段。

### 12.3 可以后置的决策

| 编号 | 决策 | 推荐时机与默认 |
|---|---|---|
| D2 | 浮点转整数发生 NV 时返回什么 | M4 前拍板；推荐 RISC-V 风格确定性饱和 |
| D3 | Gate cost 是否参与公平计分 | M5 前拍板；默认先按沙箱/教学包处理，不宣称平衡 |
| D4 | 普通导线是否自动显示浮点 | 核心稳定后再决定；默认只用显式 Display |
| D5 | 是否同时提供 Minimum/Maximum 与 MinimumNumber/MaximumNumber | M4 前按实际需求决定；默认只做 Number 版本 |
| D6 | 是否扩展 binary16/binary64 | FP32 稳定后按需求决定 |
| D7 | 是否随 Loader 主发行包附带 | 发布前决定；默认单独发布 `local.float-ops.mod`，不改主发行清单 |

当前没有阻塞 M0–M2 的用户决策；D2–D7 按表中时机再确认。

**2026-09-26：M4 前按推荐值拍板。**

- **D2 已定（推荐值）**：NaN、±Inf 与越界一律置 NV 并**饱和**——`FP32 To I32` 到
  `INT32_MAX`/`INT32_MIN`、`FP32 To U32` 到 `UINT32_MAX`/`0`；`-0` 转 0 不置 NV；负的非零值转无符号
  按越界处理。实现在 `fp/fp32.cpp` 的 `fp32_to_i32`/`fp32_to_u32`（范围判定与饱和自己做，不依赖
  SoftFloat specialization 的 `i32_fromNaN`），用例 `tests/float-kernel.cpp` 的 `checkConversions`
  逐条固定这个策略。
- **D5 已定（推荐值）**：只做 IEEE 754-2019 的 `MinimumNumber`/`MaximumNumber`，不另做
  Minimum/Maximum。
- **菜单探针（M5 的一部分，已做）**：`TCComponentTypeDefinitionV2` 没有分类字段，游戏把自定义元件
  统一放进自己的 `CUSTOM` 栏；名称全部以 `FP32 `/`I32 `/`U32 ` 前缀开头，扁平列表与搜索里自然聚合。
  因此不向存档目录部署伪 schematic（plan 8.3 的退路）。
- **成本政策（D3，仍是默认值）**：沙箱/教学包定位，所有新元件 `gate_cost=0`、`delay=0`，
  不宣称战役计分平衡。
