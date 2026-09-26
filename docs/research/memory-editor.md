> **研究日志（非发布文档）。** 本文记录原版"内存编辑界面"的静态反推结果、证据与未决项。
> 面向使用者的结论请从 [文档索引](../README.md) 进入。

# 原版内存编辑界面：入口、控件本体与数据落点

日期：2026-09-23。构建：`Turing Complete.exe`（该指定构建，EXE 基址 `0x140000000`）+
原版引擎 `tc_game_engine.dll`（基址 `0x180000000`，被加载器代理为 `game_engine.dll`）。
本文全部结论来自**静态证据**（符号表、反汇编、导出表、翻译字符串）；凡是没有真机验证的
推断都单独标了"未验证"。

## 1. 结论速览

1. **"内存编辑界面"不是一个界面，是三套编辑器 + 一块只读面板**，按元件的种类分派：

   | 界面 | 模块 | 编辑什么 | 打开方式 |
   |---|---|---|---|
   | 十六进制编辑器 | `presenter/board_ui/popup/hex_editor.nim` | 元件的**重置数据**字节 | 在棋盘上点内存元件（`board_io/action/edit_buffer`） |
   | 打孔卡 | `presenter/board_ui/popup/punch_card.nim` | 程序元件的位（同样落在重置数据） | 同上的另一条分支 |
   | 汇编 IDE | `presenter/board_ui/popup/ide/editor.nim` | 带 ISA 的元件的汇编/规格文本 | "Edit assembly" |
   | 左侧 MEMORY 区 | `presenter/board_ui/io_state_view.nim` | 只读：内存探针的当前值 | 进关卡就有 |

2. **十六进制编辑器的控件本体不在游戏里，在原版引擎 DLL 里**，是导出函数 `igHexEditor`
   （序号 875）外加 7 个伴生导出。加载器的 `src/proxy.def` **已经**把这些名字原样转发，
   所以 Mod 侧不需要新增任何加载器能力就能拿到它（见 §5）。

3. **两支编辑器改的都是元件的"重置数据"（reset data）**，改完只发一条
   `send_command(2)` 给仿真器。证据是打孔卡的位翻转函数只有四步：
   `get_reset_address(...)` → `xor 掩码,(指针)` → `send_command(2)`（§4.3）。也就是说
   "编辑内存" = **就地改重置数据 + 请求仿真器重置**，不是写一份新的内存镜像。

4. **十六进制控件没有 GetText 类导出**（只有 IsActive / IsTextChanged / GetHoveredLine /
   GetUndoId / GetUndoIndex / GetAddUndoId / SetReadOnly）。因此它必然是**就地改写调用方
   给的字节区间**，而不是自己持有一份文本等游戏来读（§3.3）。这决定了 Mod 复用它时的
   内存所有权约定。

5. **只读门**：`is_immutable_data(save_monger)` 为真时调 `igHexEditorSetReadOnly`；另外
   弹窗在 `initial_data(model) ∉ {4,5}` 时会**自己关掉自己**。见 §2.3。

## 2. 入口：谁打开它、什么时候关

### 2.1 动作层

棋盘输入的动作模块是 `presenter/user_input/board_io/action/*.nim`（符号表里 18 个）。
和内存编辑直接相关的三个：

| 符号 | VA | 说明 |
|---|---|---|
| `handle_edit_buffer__...actionZedit95buffer_u3` | `0x1403410a0` | 点中元件后按元件字段选编辑器 |
| `handle_jump_to_address__...actionZjump95to95address_u2` | `0x140341960` | 十六进制编辑器的"跳到地址" |
| `handle_scroll_buffer__...actionZscroll95buffer_u2` | `0x140350e60` | 十六进制编辑器的滚动 |

`handle_edit_buffer` 先从点击位置反查元件（`board+0x78` 长度、`board+0x80` 载荷、步长
`0x238`，与 [board-object-fields.md](board-object-fields.md) 一致），再看元件的一个字节：

```asm
14034114a: imul $0x238,%rcx,%rax        ; 元件记录
140341151: add  0x80(%r12),%rax
140341159: cmpb  $0x1,0x1e0(%rax)       ; 元件 +0x1e0 的种类字节
140341168: sbb   %eax,%eax              ; <1 → 0xffffffff, ≥1 → 0
140341189: and   $0xfffffffc,%eax
14034118c: add   $0x6,%eax              ; → 2 或 6（两种"打开编辑器"动作）
140341197: mov   %al,0xc918(%rsp)       ; 动作判别符
```

所以"点 RAM 元件开十六进制、点程序元件开打孔卡"这个分派发生在**动作层**，判据是元件记录
`+0x1e0` 的一个字节（`0` → 动作 2，`≥1` → 动作 6）。

> 未验证：动作枚举 2 / 6 与 `edit_buffer` / 打孔卡两个分支的对应关系，只有"两个值"
> 这一层是硬证据；名字对应关系需要真机或更细的枚举反推。

### 2.2 弹窗层

`presenter/board_ui/popup.nim` 给**每种弹窗**生成一个 `build_popup_u<N>`：

```
build_popups__presenterZboard95uiZpopup_u36   0x140444340   ; 每帧把所有弹窗过一遍
  ├─ hex editor 状态在 presenter+0x980 → build_popup_u911  0x140443740
  ├─ punch card 状态在 presenter+0x970 → build_popup_u815  0x1404435c0
  └─ … 共 16 个弹窗
```

`build_popup_u911` 的形状（每个弹窗都一样）：

```asm
14044374f: cmpb $0x0,0x8(%r8)      ; 本次是否启用
140443754: movzbl 0x9(%r8),%ebx    ; 之前是否已经"打开过"
...
140443780: call on_close...        ; 启用=0 且 之前=1 → 关
14044386d: call initArgs_u935      ; 首次打开 → 造一份参数
140443898: call on_open...         ; 开
1404438a6: movb $0x1,0x9(%r12)     ; 记住"已经开过"
140443823: call build_hex_editor   ; 之后的每一帧都画
```

十六进制编辑器的模块级状态极少——只有 `component_id`（`B 0x14624d780`，在 `.bss`）和三个
颜色常量；`on_open` / `on_close` 都是 `ret` 空壳（`0x1403f4c20` / `0x1403f4c10`）。
这与"一次只编辑一个元件"的交互一致：**弹窗自己是无状态的，元件 id 从参数里抄一遍**。

### 2.3 两个关窗条件

`build_hex_editor` 结尾（`0x1403f59cf`）：

```asm
1403f59cf: call initial_data__modelZmodel95types_u1553
1403f59dd: sub  $0x4,%eax
1403f59e0: cmp  $0x1,%al
1403f59e2: jbe  1403f5a08           ; 返回值 ∈ {4,5} → 留着
1403f59e4: movb $0x0,0x8(%r13)      ; 否则把"启用"位清掉（自己关掉）
1403f59f7: call play_sound          ; 并放一次音效
```

另一处是只读门（`0x1403f5229`）：

```asm
1403f5214: mov  dev_mode(%rip),%rax        ; dev_mode 为真则整段跳过
1403f5229: call is_immutable_data__modelZsave95mongerZcommon_u5429
1403f523e: call igHexEditorSetReadOnly      ; 参数就是上面的返回值
```

`is_immutable_data`（`0x1400cb3e0`）读存档对象的 `+0x1d8` 标志，并且要求版本字节
`[obj+0] == 0x76`、序列长度 `[obj+0xa8] > 2`，再看第 3 个元素 `+0x18` 是否为空——
即"这关的内存数据是随关卡发下来的、不允许玩家改"。**把这一条和 §4 的写路径合起来看，
原版对玩家改内存的态度是：允许改，但改的是重置数据，且关卡可以宣布这份数据不可改。**

### 2.4 外观

弹窗本体是一个普通 ImGui 窗口（`igBegin` + `igBeginChild_Str` + `igPushStyleColor_*`
一圈主题色），里面就是那个十六进制控件；右上角是**图标按钮**：

```asm
1403f564a: lea  0x2ab4b3(%rip),%rcx    ; → 0x1406a0b04 = "##Image"（隐藏标签的图标按钮）
1403f5651: call igButton
1403f565f: movb $0x0,0x8(%r13)         ; 点它 = 关弹窗
1403f566e: call play_sound
```

控件本身拿到的 ImGui id 是 `"#HexEditor"`（`0x1406a0ad5`）。**这个模块没有任何可翻译
字符串**——翻译表里 546 个源文件小节里没有 `popup/hex_editor.nim`，与"控件在引擎 DLL 里、
左面板那一串文本才是要翻译的"相符。

## 3. 控件本体：引擎 DLL 的 `igHexEditor`

### 3.1 导出表

引擎 DLL 里有一族并列的编辑器导出（`RVA` 为原版引擎 DLL 的 RVA，基址 `0x180000000`）：

| 导出 | 序号 | VA |
|---|---|---|
| `igHexEditor` | 875 | `0x1800324d0` |
| `igHexEditorGetAddUndoId` | 876 | `0x180032580` |
| `igHexEditorGetHoveredLine` | 877 | `0x180032540` |
| `igHexEditorGetUndoId` | 878 | `0x180032560` |
| `igHexEditorGetUndoIndex` | 879 | `0x180032550` |
| `igHexEditorIsActive` | 880 | `0x180032520` |
| `igHexEditorIsTextChanged` | 881 | `0x180032530` |
| `igHexEditorSetReadOnly` | 882 | `0x1800325a0` |

同族还有 `igCodeEditor*`（序号 572–588）与 `igIsaEditor*`（序号 1065–1076），
也就是汇编 IDE / ISA 编辑器用的文本控件（断点、错误标记、调色板、行高亮、取文本——这是
ImGuiColorTextEdit 那一套 API 形状）。**十六进制控件是它的姊妹类型**：并行命名、并行
撤销/悬停行语义，所以游戏侧可以把两者当同一种东西用。

`igHexEditor` 本体是个薄包装（`0x1800324d0`）：把寄存器参数和两个栈参数重新排好后调进
一个泛型模板函数（`0x180045060`，旁边 `igCodeEditor` 对应 `0x18003c120`）。伴生导出全是
**模块级单例状态**的读写：

```asm
180032520: movzbl 0x1ca6ce(%rip),%eax   ; igHexEditorIsActive  → 读一个字节
180032530: movzbl 0x1ca4e6(%rip),%eax   ; IsTextChanged
180032540: mov    0x1ca6a2(%rip),%eax   ; GetHoveredLine
180032550: mov    0x1ca4aa(%rip),%eax   ; GetUndoIndex
180032560: …imul $0x5, … shl $0x5…      ; GetUndoId：索引进 160 字节的撤销栈条目
180032580: …inc 0x1a1b96(%rip)…         ; GetAddUndoId：一个全局计数器
1800325a0: mov %cl,0x1ca473(%rip)       ; SetReadOnly：写一个字节
```

**单例**这一点很重要：同一时刻整个进程只有一份十六进制编辑器状态，游戏自己的弹窗和
任何想复用它的 Mod 共享它（§5 的风险）。

### 3.2 游戏侧的调用序列

`build_hex_editor` 里与内存数据有关的调用，顺序固定（`VA` 为 EXE）：

```asm
1403f5229: call is_immutable_data__modelZsave95mongerZcommon_u5429
1403f523e: call igHexEditorSetReadOnly
1403f5243: call igHexEditorIsTextChanged
1403f524c: mov  $0x2,%ecx
1403f5251: call send_command__modelZsimulator95types_u137   ; 文本变了 → 命令 2
1403f526a: call get_hex_editor_address_range__modelZboardZmemory95manager_u42
1403f5289: call igHexEditor__presenterZimguiZimgui_u7796    ; 标签 "#HexEditor"
1403f58c7: call igHexEditorIsActive
1403f58cc: mov  %al,0xa(%r13)                               ; 存进弹窗状态 +0xa
```

弹窗状态的三个字节因此是：`+0x08` = 本次启用、`+0x09` = 已经建过、`+0x0a` =
**控件当前是否活跃**（"焦点在编辑器里"这个事实被记在状态上，供棋盘输入那一侧判断
要不要把键盘/鼠标让出去）。

`presenter/imgui/imgui.nim` 的包装（`0x140277cc0`）说明参数形状：

```asm
140277ccf: mov (%rdx),%r13      ; 第二参数指向的 16 字节里的前 8 字节
140277cd2: mov 0x8(%rdx),%rsi   ; 后 8 字节
140277cfc: movl $0x1,0x28(%rsp)
140277d04: divss …              ; 一个浮点系数（缩放）经栈传
140277d12: call igHexEditor     ; (标签, 前8字节, 后8字节, 第三参数, 浮点, 0x1)
```

也就是说控件拿到的是**一对 8 字节值**（`get_hex_editor_address_range` 刚算出来的那个二元组）
——正好是"一段字节区间的起点/终点"。这对值的具体语义见 §4.2。

### 3.3 为什么可以断定"就地改写"

引擎 DLL 的导出清单里**没有** `igHexEditorGetText` / `SetText`（对照 `igCodeEditorGetText`
`igCodeEditorSetText` 都有）。而游戏侧在"文本变了"之后**只**发了一条仿真命令，没有再走
任何取回文本的调用。两条合起来只剩一个可能：**控件直接改写调用方交给它的那段字节**，
`IsTextChanged` 只用来触发"请求仿真器重置"。

## 4. 数据落点：重置数据与寻址规则

### 4.1 模块

内存的模型层是 `model/board/memory_manager.nim`。符号表（EXE）里的整族函数：

| 符号 | VA | 读到的字段 |
|---|---|---|
| `is_little_endian__…memory95manager_u10` | `0x140139d70` | `[rcx+0x178]`（u8） |
| `get_init_data__…memory95manager_u13` | `0x14013c350` | `[rcx+0x179]`（u8） |
| `get_offset__…memory95manager_u25` | `0x14013b460` | `[rcx]` 长度、`[rcx+8]` 端序 |
| `get_reset_address__…memory95manager_u37` | `0x14013c360` | `[rcx]`/`[rcx+8]`/`[rcx+0x18]` |
| `get_reset_data_range__…memory95manager_u118` | `0x14013b4f0` | `+0x18`/`+0x20`/`+0x30` |
| `get_hex_editor_address_range__…memory95manager_u42` | `0x14013c410` | `+0x00`/`+0x08`/`+0x10`/`+0x18`/`+0x28` |
| `set_component_reset_data__…memory95manager_u264` | `0x14013bf60` | 把一段字节写进管理器缓冲 |
| `load_reset_data_from_file__…memory95manager_u131` | `0x14013a7a0` | 重置数据可以从关卡文件加载 |
| `reload_reset_data__…memory95manager_u296` | `0x14013c0c0` | 重新加载 |
| `default_file_path__…memory95manager_u16` | `0x14013a0a0` | `+0x1f8`/`+0x179`（默认文件） |
| `create_buffer_for_component__…u351` / `buf_alloc` `u329` / `store_buffer` `u2563` | `0x14013aa90`/`0x140139fa0`/`0x14013b5f0` | 缓冲表 |
| `save__…memory95manager_u174` / `write__…u2585` | `0x140139b30` / `0x140139b10` | 存档 |
| `DEFAULT_BUFFER__…memory95manager_u9` | `B 0x146220c80` | 默认缓冲（全局数据） |

配合关卡文件可以看清"重置数据从哪来"：`campaign/symphony_6_ram/circuit.data` 里带着
`flags` 与 `campaign/symphony_6_ram/new_program.asm` 这样的字段——**关卡内存元件的初始
内容是随关卡发下来的，可以来自文件**，这正是 `is_immutable_data` 想保护的东西。

### 4.2 十六进制编辑器看到的区间

`get_hex_editor_address_range` 全文（`0x14013c410`，含 `.byte` 的几行是 objdump 对齐问题，
语义不受影响）：

```asm
14013c417: cmpb $0x5,0x28(%rdx)     ; 种类 == 5 ？
14013c41b: mov  0x18(%rdx),%rbx     ; 先取 +0x18
14013c422: jne  14013c428
14013c424: mov  0x10(%rdx),%rbx     ; 种类 == 5 → 改成 +0x10
14013c428: cmpb $0x0,0x8(%rdx)      ; 端序标志
14013c42c: mov  (%rdx),%r8          ; +0x00
14013c431: add  %rbx,%r8            ; +0x00 + 长度
   …结果二元组 = (+0x18 或 +0x10, +0x00 + 那个值)
14013c458: …结果二元组两端互换（端序标志为 0 时）
```

结论：它返回**两个 u64**，其中一个是"基址 + 长度"，另一个是"基址或长度"，
由端序标志决定先后。到底是"起始地址/结束地址"还是"缓冲指针/结束指针"，
**静态证据不足以定案**——但这不影响使用：控件拿到的就是这对值，并且直接改写它（§3.3）。

### 4.3 写路径（打孔卡是最干净的样本）

`toggle_punch_card_bit__presenterZutilities_u41214`（`0x140330620`）全文只有四件事：

```asm
140330670: imul $0x238,%r10,%rsi          ; 取元件记录
14033067a: add  0x80(%r11),%rsi
140330697: sub  %r9,%rsi                  ; 位序号 → 移位量
14033069a: call bits__modelZsave95mongerZcommon_u193     ; 造掩码
1403306c4: call get_reset_address__modelZboardZmemory95manager_u37
1403306d8: xor  %dl,(%rax)                ; ★ 就地翻转重置数据里的那一位
1403306df: call send_command__modelZsimulator95types_u137 ; 命令 = 2
```

`get_reset_address`（`0x14013c360`）本身说明了端序规则：

- 端序标志为真（小端）：`地址 = 索引 + [结构+0x18]`，直接从头往后排；
- 端序标志为假（大端）：先把值 `to_bytes` 求长度 `len`，再算
  `地址 = 索引 + [结构+0x18] + …`——即**从这段数据的末尾往前排**。

这与手册里"RAM 元件有各自的端序标志"（`asset/manual/Assembly/Language creation/Settings/doc.txt`）
以及 `Computer concepts/Endianness` 一节完全吻合；十六进制编辑器显示的字节顺序就是这个标志的输出。

### 4.4 提交：仿真命令槽

`send_command(cmd)`（`0x14010f0d0`）只写两个全局：

```asm
14010f0d4: mov  simulation_commands(%rip),%rax        ; 0x1406f1e08
14010f0de: mov  %rcx,(%rax)                           ; byte0 = 命令
14010f0f9: mov  simulation_next_command_id(%rip),%rcx ; 0x14049b248
14010f106: mov  %rcx,0x8(%rdx)                        ; +8 = 自增的请求号
```

全 EXE 里发命令的地方只有 5 个调用点、分布在 3 个函数里：

| 命令 | 发出者 |
|---|---|
| `0` | `handle_request_do__modelZsimulationZsimulator95functions_u289`（`sim.do` 那一支） |
| `1` / `3` | `handle_request_compile_and_run__modelZsimulationZsimulator95functions_u20` |
| `2` | **`build_hex_editor`（十六进制编辑器改完）** 与 **`toggle_punch_card_bit`（打孔卡改完）** |

两支内存编辑器共用命令 `2`，这是"改内存 = 改重置数据 + 请求仿真器重置"这条结论的交叉证据。

## 5. 左侧 MEMORY 区（只读那半边）

`build_io_state_view__presenterZboard95uiZio95state95view_u100`（`0x14045ea20`）是左面板。
它的"MEMORY"小节对每个内存类元件做：

```asm
140460364: call get_component_memory__presenterZutilitiesZhelper95functions_u5830
14046039b: call value_to_string__presenterZutilitiesZhelper95functions_u5906   ; 按位宽格式化
```

`get_component_memory(board, 元件索引)`（`0x1402e7d80`）本身不返回缓冲，而是取**元件状态的
当前值**：

```asm
1402e7dee: …把元件记录（0x238 字节）拷到栈上…
1402e7df8: call get_memory__modelZsimulationZcontroller_u167     ; 0x140175450
140175457: cmpb $0x0,(%rcx)            ; 元件 +0x00（kind）：0 = 墓碑 → 返回 0
14017545f: mov  0x88(%rcx),%rdi        ; 位宽（bits）
1401754a2: call sim_state_read_u64__modelZsimulator95types_u159  ; 读仿真状态
1401754b4: 掩码到 rdi 位（≤0x3f 位时 & ((1<<(64-位宽))-1)）
```

即：**左面板显示的是内存探针/内存元件在仿真里的当前值，按元件的位宽截断**；它与
§4 的重置数据是两份东西（一份"运行时现在是什么"，一份"重置时该是什么"）。这解释了为什么
改完重置数据必须发命令 2——否则面板和仿真都不会变。

顺带对上我们已有的资产：`src/io_state_cache.hpp` 记录的 `presenter+0xda30/0xda40/0xda50`
正是这个面板读的三组 Nim 序列（输入 / 输出 / 其它），"MEMORY" 属于第三组。

## 6. 与 Mod 生态的关系

### 6.1 现状

- 能读：`tc.io_value`（关卡输入与常量值）、`tc.board`（元件/导线/引脚/几何）、
  `io_state_cache`（左面板三组状态）、`tc.simulation`（状态位读取）。
- 不能：**读写内存元件的重置数据**，也没有任何"让游戏打开某个元件的编辑器"的入口。

### 6.2 复用原版控件的路径（可行，且不需要新能力）

`src/proxy.def`（加载器的代理导出表，1507 行，逐条转发到 `tc_game_engine`）里已经有：

```text
877: igHexEditor=tc_game_engine.igHexEditor @875
878: igHexEditorGetAddUndoId=…  879: GetHoveredLine=…  880: GetUndoId=…
881: GetUndoIndex=…  882: IsActive=…  883: IsTextChanged=…  884: SetReadOnly=…
```

而符号别名表的解析走的是"EXE 的 COFF 符号表（含导入桩）"——现有 `ui.invisibleButton`
就是这么解析 `igInvisibleButton` 的。EXE 里同样有 `igHexEditor` 的导入桩，所以
**加一条 `{"ui.hexEditor", "igHexEditor", TC_SYM_FUNCTION, ...}` 就能让插件拿到原版控件**，
外观与游戏完全一致，代价是零渲染代码。

但有三条风险必须先说清：

1. **单例状态**：`IsActive`/`IsTextChanged`/撤销栈都是引擎 DLL 里的全局（§3.1），
   插件自己的十六进制面板和游戏弹窗会互相串台；
2. **签名未验证**：静态读到的参数形状是 `(标签, 区间前 8 字节, 区间后 8 字节, 第三参数,
   浮点缩放, 0x1)`，第三参数与浮点的含义没有实测，按本项目规矩**必须先写探针再进契约**；
3. **就地改写**：控件的编辑直接落在传入的字节上，谁负责发 `send_command(2)`、谁负责撤销
   记账，插件要自己决定（原版是弹窗自己发命令、打孔卡另外走 `save`+`upgrade` 记撤销）。

### 6.3 若要做"Mod 版内存编辑器"，缺的那一块

按 §4 的结论，最小可用的服务是：

```text
内存元件 → 重置数据区间（起点/长度/端序）
        → 按端序规则做索引↔地址换算（复用 get_reset_address 的规则，别自己发明）
        → 写完发 send_command(2)
```

对应的候选符号已经在表里（§4.1），并且 `get_component_memory` 证明了"元件索引 → 数据"
这条链的形状。真正需要先做的是**只读探针**：在真机上打印某个内存元件的
`get_hex_editor_address_range` 二元组、`is_little_endian`、`get_init_data`、
`is_immutable_data` 四个值，确认字段语义之后再定契约。

## 7. 未验证 / 下一步

1. **`igHexEditor` 的确切签名**（第三参数、浮点、栈上的 `0x1`）：写真机探针，先只读
   (`SetReadOnly(1)`) 在一个沙箱关卡里画一次，确认不崩、状态位正确变化。
2. **`get_hex_editor_address_range` 输入结构的字段语义**（`+0x00`/`+0x08`/`+0x10`/`+0x18`/`+0x28`）：
   只读探针 + 与关卡里已知内存元件对照（长 8/32 位、大小端各一例）。
3. **动作枚举 2 / 6 与两个编辑器的对应**：反推 `board_io/action` 的 `case` 顺序或真机点击对照。
4. **真机截图**：仓库已有真机沙箱与截图流程（`tests/*-playtest.ps1`，例如
   `punchcard-playtest.ps1` 会复制游戏到 `build/`、换 `USERPROFILE`、跑完截图）。
   要做的是加一个"进关卡 → 把 RAM 元件放在已知屏幕位置 → 合成点击 → 截图"的探针；
   截图是本文唯一缺的一类证据（玩家视角的界面长什么样）。
5. **`is_immutable_data` 里的存档版本语义**（`0x76`、`+0xa8` 第三个元素）：与存档格式
   那条线一起看，别单独猜。

## 8. 复现命令

```powershell
$env:PATH = "C:\msys64\ucrt64\bin;$env:PATH"
cd D:\p

# 1) 符号表（游戏逻辑在 EXE 里，引擎 DLL 是 stripped 的）
nm --format=posix "Turing Complete.exe" > nm-exe.txt
rg "hex95editor|memory95manager|punch95card|igHexEditor" nm-exe.txt

# 2) 全量反汇编一次，之后用地址检索（约 48 MB 文本）
objdump -d --no-show-raw-insn "Turing Complete.exe" > dis-exe.txt
rg -n "1403f4c30|14013c410|140330620|14010f0d0" dis-exe.txt

# 3) 引擎 DLL 的导出表（RVA/序号）
objdump -p "tc_game_engine.dll" | rg "igHexEditor"
# 或：python D:\p\.research\pe_exports.py "tc_game_engine.dll" hex
```

本文引用的关键地址（EXE 基址 `0x140000000`）：

| 地址 | 符号 |
|---|---|
| `0x1403f4c30` | `build__presenterZboard95uiZpopupZhex95editor_u16` |
| `0x1403f4c20` / `0x1403f4c10` | `on_open` / `on_close`（都是 `ret`） |
| `0x14624d780` | `component_id__…hex95editor_u5`（`.bss`） |
| `0x140443740` / `0x1404435c0` | 十六进制 / 打孔卡的 `build_popup` |
| `0x140444340` | `build_popups` |
| `0x14042c430` / `0x140330620` | 打孔卡弹窗 / `toggle_punch_card_bit` |
| `0x1403410a0` | `handle_edit_buffer` |
| `0x14013c410` / `0x14013c360` / `0x14013b4f0` | 十六进制区间 / 重置地址 / 重置区间 |
| `0x1400cb3e0` | `is_immutable_data` |
| `0x14010f0d0` / `0x1406f1e08` | `send_command` / `simulation_commands` |
| `0x14045ea20` / `0x1402e7d80` / `0x140175450` | 左面板 / `get_component_memory` / `sim.get_memory` |

## 9. 相关文档

| 文档 | 内容 |
|---|---|
| [board-object-fields.md](board-object-fields.md) | 元件记录 `0x238` 步长与字段语义（本文用到 `+0x1e0`、`+0x80`） |
| [../reference/symbols.md](../reference/symbols.md) | 别名表与钩子链；`ui.invisibleButton` 是"解析引擎导入桩"的先例 |
| [../sdk/simulation.md](../sdk/simulation.md) | 现有仿真/状态读取能力 |
| [../HANDOFF-punch-tape-runtime-constants.md](../HANDOFF-punch-tape-runtime-constants.md) | 打孔纸带那条线（本文的打孔卡是它的原版对照） |
| [word-watchee-64.md](word-watchee-64.md) | 同一构建上的"读渲染参数并打内存补丁"的完整方法论样本 |
