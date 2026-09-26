# 导线/端口数值标签只显示低 32 位：定位与修复

日期：2026-09-20；构建：`tc-win64-2.1.334`（EXE sha256 `8875da0e…8eb21`）。
结论：不是模拟器读不到 64 位，而是**绘制参数在写入 instance 属性时被钳到 32**，
外加**着色器 64 位十六进制分支的一行位移写错**。两处都由 `local.word-watchee-64`
这个包修掉（原生插件改内存里的一字节 + 一行精确文本补丁）。

## 1. 症状与"能力缺失"的排除

字长 >32 位的棋盘上，导线/端口上的数值标签只显示低 32 位（高 32 位整段消失），
十进制与有符号显示同样如此。怀疑方向有三处：仿真是否只算 32 位、数值上传是否被截断、
着色器是否有 33–64 位路径。实测结果是前两处都不成立：

| 环节 | 符号 / 位置 | 实测 |
|---|---|---|
| 读仿真值 | `sim_state_read_u64__modelZsimulator95types_u159`（VA `0x14010f2e0`） | 8 字节整字读，调用点 `0x14047bfb3` |
| 写标签数值 | `set_value__presenterZrendererZmulti95meshZword95watchee95mesh_u1269`（VA `0x140298030`） | 把 `rdx` 的 **8 字节**值 + 1 字节 override 一起交给 `multi_mesh_2d` 的 `set_buffer`（`mov r9d,0x9`） |
| 写标签位宽 | `set_value_size__presenterZrendererZmulti95meshZword95watchee95mesh_u1008`（VA `0x1402980b0`） | 只写 1 字节（`mov r9d,0x1`），**写之前 `min(value_size,32)`** ← 丢失点 |

`set_value_size` 的两个调用点都在绘制侧：`redraw_word_watchee__presenterZupdate95state95common_u725`
（尾跳 `0x14046e1f5`）与 `redraw_watchees__presenterZupdate95state95stream_u120`
（`0x14047c041`）。也就是说无论走 common 还是 stream 路径，位宽都要经过同一个钳制。

## 2. 钳制点（函数 + 0x40 起 24 字节）

```asm
1402980f0  b8 20 00 00 00        mov    eax,0x20          ; ← 上限 32
1402980f5  8b 4d 14              mov    ecx,[rbp+0x14]    ; 引擎 mesh 句柄
1402980f8  41 b9 01 00 00 00     mov    r9d,0x1           ; 只写 1 字节
1402980fe  4c 8d 44 24 24        lea    r8,[rsp+0x24]     ; 该字节的槽位
140298103  38 c3                 cmp    bl,al             ; bl = value_size（字节参数）
140298105  0f 47 d8              cmova  ebx,eax           ; value_size = min(value_size, 32)
```

`ebx` 随后被写进 instance 属性（`ef af f7 imul esi,edi` 求 stride×index，
`lea edx,[rsi+r12]` 加上属性偏移，再 `set_buffer`）。着色器拿到的 `inst_value_size`
因此永远是 1–32，33–64 位的分支根本进不去。

## 3. 着色器一侧：33–64 位已经写好，但 64 位十六进制分支有一行错

`asset/shader/word_watchee.vert` 里 33–64 位的支持是**现成的**：

- `extend_sign`／`zero_extend` 都有 `inst_value_size <= 32u` 与 else（33–64）两支；
- `repr10` 用 `uvec2` 的 64 位 `divmod10`（`0xCCCC…CCCD` 逆乘）做十进制，负数走 `negate`；
- `repr16` 的 64 位分支先取 `value[1]` 的若干半字节，再取 `value[0]` 的 8 个半字节。

问题在最后一段的起点（`repr16`，第 201 行）：

```glsl
        shift = 32;      // ← 应为 28
        do {
            frag_label_value[index] = ((value[0] >> shift) & 0xFu) + IDX_DIGIT_OFFSET;
```

低 32 位只有 8 个半字节，起点必须是 `28`（与 `<=32` 分支一致），`32` 会：

1. 先多写一个半字节（64 位宽度下 8+9=17 个，而 `ret` 只有 16），
2. 让整个低位部分错位一格（每个半字节都不是原来的那一位），
3. 甚至对 32 位字做 `>> 32`（GLSL 未定义行为）。

`tests/word-watchee-model.js` 用宽度 33–64 的样本量化了这一点：**160 个样本在
`shift = 32` 下全部与期望值不符**，改成 28 后全对；十进制/有符号路径同时被覆盖
（`18446744073709551615`、`-1`、`-9223372036854775808`、33/40 位边界等）。

## 4. 修复

| 位置 | 修法 | 交付形式 |
|---|---|---|
| `set_value_size` +0x41 的立即数 | `0x20` → `0x40`，即 `min(value_size,64)` | 原生插件在 `tc_mod_load` 里改**本进程内存**（EXE 是钉住的构建，文件哈希不能动） |
| `word_watchee.vert` 第 201 行 | `shift = 32;` → `shift = 28;` | 包的 `patches`（精确文本补丁，停用即从备份还原） |

上限取 64 而不是"取消钳制"：着色器用 `1u << (inst_value_size - 33u)`，位宽 >64 时
移位量 ≥32，在 GLSL 里未定义。`min(size,64)` 既让 33–64 位通过，也保留了对这种输入的
保护；1/8/32 位棋盘的行为逐字节不变。

插件在写之前核对函数内 24 字节窗口（`tests/word-watchee-model.js` 同一张表取自
本构建的 EXE），不匹配就只写状态、不动代码：换游戏构建时游戏表现与未装此包完全一致。

## 5. 证据与复现

```powershell
node tests/word-watchee-model.js                      # 离线：钳制点 + 33..64 位数字
powershell -File tests/word-watchee-playtest.ps1      # 真机：文本补丁 + 内存补丁 + 进棋盘截图
powershell -File tests/word-watchee-playtest.ps1 -Loader <已装游戏的 game_engine.dll>
```

真机日志（`loader.log`）中应出现：

```text
[local.word-watchee-64] word-watchee-64: value_size ceiling 32 -> 64 at 0x…80f0 (mov eax,0x20; read back 64)
[local.word-watchee-64] status: word-watchee-64: value labels keep all 64 bits (ceiling 32 -> 64)
```

插件同时把同样的结论写进 `plugin-data/local.word-watchee-64/result.txt`，测试脚本断言的是
这个文件（"读回来的字节"而不是"声称改了"）。

想看某块板子的位宽真的以 64 到达渲染器，可以设环境变量 `TC_WATCHEE_TRACE=1` 再启动游戏：
插件会钩住 `set_value_size`，把前 64 次调用的 `size` 打进 `loader.log`。

## 6. 还没验证的

- 真机画面证据用的是"把标签位宽强制成 64"的探针（下一节），不是一块真正的 64 位棋盘：
  仓库里的关卡与夹具都只有 1/8/32 位字长，造 64 位棋盘需要先有 64 位自定义元件（或字长
  编辑器）产出的板子。本包的两半各自有证据（内存里的位宽字节、离线的数字提取、真机双排
  截图），但"一块 64 位棋盘的完整数值"这条端到端画面证据仍留待有 64 位棋盘时补。
- `value_size` 超过 64 的输入仍会被钳到 64（设计如此，见 §4）。

## 7. 双排显示（包 1.1.0）：>32 位改成两行十进制

64 位十进制有 20 位，一行读起来太长，所以**位宽 >32 位**的标签改成两行；≤32 位的标签一行
不变，仍按游戏设置的格式显示（离线用例 `tests/word-watchee-model.js` 两边都断言）。

换行位置是**十进制数字**的第 10 位之后，不是二进制位边界：把该位宽下的无符号值写成十进制，
**上排是前 10 位数字，下排是剩下的数字**，不足 10 位时下排显示 `0`。用户给的例子
2^63 = 9223372036854775808 因此显示成

```text
9223372036
854775808
```

宽标签**不跟随**游戏的显示格式（十六进制 / 无符号 / 有符号）：两列十进制才是长数字要的可读
形状，有符号的解释留在 ≤32 位那一档。

实现落在两个着色器文件上，所以包从"一行文本补丁"改成**整文件部署**（`files/`，停用时从
`blobs/` 还原原文件；wire-palette 也是这么做的）：

| 文件 | 改动 |
|---|---|
| `word_watchee.vert` | 新增 `ROW_STRIDE 10` 与两行字形数组 `frag_label_value[2 * ROW_STRIDE]`；`repr_wide()` 用既有的 64 位 `divmod10` 取全部十进制数字、在第 10 位处切成两行，`pad_row()` 把每行剩余格子写成空格；`vert_position()`／`tex_coord()` 在 `label_rows = 2.0` 时把四边形拉成两行高（第一行仍在原来的位置，第二行往下长） |
| `word_watchee.frag` | 用 `modf(frag_tex_coord.y, row)` 取行号，字形索引变成 `int(row) * ROW_STRIDE + int(whole)`，字形纹理坐标仍取行内小数部分——单行标签（row 恒为 0）行为不变 |

**真机验证**（`tests/word-watchee-layout-playtest.ps1`，diagnostic 层）：沙箱里跑真实的
Byte Adder 关卡与夹具板，另装一个只做一件事的探针 `tests/word-watchee-wide-probe.cpp`——
它钩住 `set_value_size` 并把位宽强制成 64，于是普通 8 位棋盘的标签也走双排路径；截图里
`Carry in` 的标签是上排 `1`／下排 `0`，`A` 输入是上排 `230`／下排 `0`。

第一版截图暴露了两个只有画面能看出来的问题，都已修掉：

1. **较短那一行的空槽是未定义字形**：四边形宽度取两行的较长者，第二行只写了"0"和几个空格，
   其余格子没写，采样出来是上一次留下的字形（看起来像重影）。修法是 `pad_row()` 把每行
   剩余格子写成空格字形。
2. **第二行会被后画的导线盖住**：游戏按"画导线→画它的标签"的顺序绘制，所以密集区域里某条
   导线标签的第二行可能压在相邻导线上。这是"两行标签"的固有代价，不是着色器缺陷；需要时
   把标签所在导线挪开或缩放视图即可。用调试着色器（行 0 染红、行 1 染绿）核对过两行都在画，
   只是被覆盖。

### 7.1 第三版才修掉的真机 bug：动态下标局部数组被驱动吃掉

用户在一块 64 位常量元件上截图：值 18446744073709551615 应该显示
`1844674407` / `3709551615`，实际是 `1844674407` / `3`——第二行只剩第一位。

排查过程（每一步都在真机上，用 `tests/word-watchee-layout-playtest.ps1` 加
`TC_WATCHEE_WIDE_VALUE=1` 把标签数值强制成 2^64-1 复现同一个画面）：

| 实验 | 结论 |
|---|---|
| 调试着色器：每格写自己的序号（两行都写） | 两行 20 格都能显示 → 数组、片元按行寻址、四边形高度都没问题 |
| 调试着色器：用 `repr_wide` 同样的两个循环 + 常量填充的 `scratch` | 仍然两行都对 → 循环形状没问题 |
| 用 Python 逐位复刻 `divmod10`（2 万随机样本 × 32/40/48/64 位） | 退位除法正确（顺带发现游戏自带的 `divmod10` 在 **2^63** 这个值上余数算成 7，真值 8；≤32 位与原版路径不受影响） |
| 调试着色器：`repr_wide` 里把 `digits`、`scratch[19]`、`scratch[10]`、`scratch[9]`、`scratch[0]` 画出来 | 位数与各位数字**全部正确**；坏的只是"把 scratch 写进 `frag_label_value`"这一步 |

结论：这块 GPU 的编译器把 `repr_wide` 里那个**动态下标局部数组**（`uint scratch[20]`，
写入用 `scratch[digits]`、读出用 `scratch[bottom_cells - 1 - i]`）编译成了只保留一格的
东西——位数和数字都对，第二行却只剩第一个（`scratch[9]` = `3`），其余格是 0（空格字形）。

修法是把中间数组整个删掉：**先数位数**（在原地反复 `divmod10`），**再把每个数字直接写到它
该在的格子**：

```glsl
int bottom_cells = digits > ROW_STRIDE ? digits - ROW_STRIDE : 0;
int top_cells = digits - bottom_cells;
uvec2 rest = value;
for (int k = 0; k < digits; k += 1) {
    uint glyph = divmod10(rest) + IDX_DIGIT_OFFSET;   // k-th digit from the right
    if (k < bottom_cells) frag_label_value[ROW_STRIDE + (bottom_cells - 1 - k)] = glyph;
    else frag_label_value[top_cells - 1 - (k - bottom_cells)] = glyph;
}
```

修复后的真机截图：`1844674407` / `3709551615` ✓。离线用例同步加了这条约束：
着色器里**不得出现** `scratch[`（`tests/word-watchee-model.js`），真机用例固定用
2^64-1 截图（这是 20 位、必须切两行、且能暴露"只保留一格"的那种回归）。

### 7.2 顺带修掉的两个"只有真机会暴露"的问题

**游戏自带的 `divmod10` 会差一。** 它用 `ceil(2^67/10)` 逆乘再减回来，某些值会落在边界上：
2^63 的余数算成 7，真值是 8——所以 `9223372036854775808` 会显示成 `9223372036` /
`854775807`。vanilla 的 32 位路径只用得到 ≤10 位数字，这个偏差一直没暴露。本包把这份
着色器里的 `divmod10` 换成**四段 16 位长除法**（余数始终 <10，每一步 `余数*65536+块`
≤655359，稳在 uint 内），并对 ≤32 位路径保持相同结果。换完后 2^63 的真机截图正是
`9223372036` / `854775808`（用户给的例子）。

**动态下标局部数组在这块驱动上不可信。** 见 §7.1：`uint scratch[20]` 的写入/读出被编译成
只保留一格。现在整份着色器不再有任何 `scratch[...]`，离线用例也把这条写成了断言
（`tests/word-watchee-model.js`：着色器里不得出现 `scratch[`，且必须用长除法 `/ 10u`）。
