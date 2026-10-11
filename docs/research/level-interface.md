# 关卡接口研究：Mod 能不能创建新关卡（2026-09-28）

问题：加载器/Mod 有没有接口能**创建新的关卡**？结论分三层，每一层都有证据。

## 结论

| 想做的事 | 能否 | 靠什么 |
|---|---|---|
| 加载一个**已存在**的关卡 | **能** | `load_level__modelZutilities_u7740`（加载器已暴露为 `level.load`；`dev.enter-board` 一直在用） |
| 给一个**已有关卡换内容**（棋盘、UI、标签、标题、对话、解锁、提示） | **能，今天就能做** | 关卡内容全在 `campaign/<名字>/` 的文件里；加载器允许 Mod 包部署 `campaign/**` 并记账/回退 |
| 用一个**新名字**建关卡（新目录 + 新关卡名进关卡树） | **不能只靠放文件** | 关卡名表编译在 exe 里；`load_level(名字)` 先查表。要新名字必须动游戏内存里的战役表（研究项目，见文末） |

## 证据

### 1. 关卡内容全在磁盘（98 个关卡目录）

```text
campaign/always_on/{meta.txt, ui.txt, label.txt, circuit.data, hint_0.txt,
                    hint_0.data, hint_solution.txt, hint_solution.data, test.si}
campaign/*.png        bit_30_off/on/z、bit_40_off/on/z 等精灵
```

`sandbox`、`preview_architecture` 也是普通目录——沙盒不是特例。

### 2. 关卡名编译在 exe 里，关卡文本不在

```text
"always_on"      在 Turing Complete.exe 中出现 2 次
"byte_adder"     2 次
"binary_racer"   4 次
"Create a circuit that is always"（always_on/meta.txt 里的对话）  0 次
"De Morgan"                                                       0 次
"unlocks_components" / "post_mortem" / "hint_kind" / "no_score" / "cycle_past_fail"
                 exe 里各 2–3 次
```

读法：**名字表是编译进去的**，而 `meta.txt` 的**字段名**在 exe 里说明游戏在**运行时解析 meta.txt**；
对话/标题那些**文本本身**不在 exe 里，来自磁盘。所以"标题、对话、解锁、提示"是数据，**关卡名和
它在树里的位置/顺序**是代码。

### 3. `load_level(name)` 要求名字在编译表里

反汇编 `load_level__modelZutilities_u7740`（`0x1402ca900`）的调用：

```text
contains__modelZcampaigns_u16427          ← 先问"这个名字在战役表里吗"
X5BX5D___modelZcampaigns_u16514 ×5        ← Nim 的 [] 查表（关卡、战役、解锁…）
as_sanitized_folder_name__modelZsanitized95path_u472   ← 由名字推目录名
save_level_data__modelZutilities_u5683 / reset_ui / clear_selections / sim_do
```

即：**名字→目录名→读文件**，但入口先要求名字已在表内。新名字没有入口。

### 4. 关卡树与解锁同样是编译表驱动的

`find_all_levels__presenterZutilitiesZhelper95functions_u2713`（`0x1402e7030`）对内部列表逐个
`is_complete__modelZcampaigns_u16375` 再 `add`；树 UI 在
`build_level_tree_ui__presenterZlevel95tree95ui_u471`，背景/连线来自 `asset/level_tree/*.png`
（只是贴图，不含结构）。`get_level_next` / `get_level_unlockable_*` 也都在 `campaigns` 模块里。

### 5. 加载器已经允许 Mod 包部署关卡文件

`src/core.hpp` 的账本规则：

```cpp
inline bool resource(const std::string& rel){
  auto prefix=rel.substr(0,rel.find('/'));
  return safe(rel) && rel.find('/')!=rel.npos &&
         (prefix=="asset" || prefix=="campaign" || prefix=="translations");
}
```

即 Mod 包的 `files/campaign/...` 是合法路径：部署走 `blob` 备份 + 事务回退（`state.json` 记账，
`recover()` 能撤销半途的中断）。这不需要任何加载器改动。

## 今天就能做的"新关卡"（路线 A：占用已有名字的槽位）

一个关卡的可玩内容全在 `campaign/<名字>/`：

1. 自己的起点棋盘 → `circuit.data`；
2. 自己的标题/对话/解锁/提示 → `meta.txt`（运行时解析）；
3. 自己的 UI 表格/标签 → `ui.txt` / `label.txt`；
4. 自己的判题 → `test.si` 与 `hint_*`。

所以一个"关卡包"Mod 可以：把 N 个关卡的内容分别部署到 N 个已有名字的目录里（例如 `sandbox`、
`preview_architecture` 或战役关），再用加载器已有的 UI 页（`tc.ui` 页面插槽）画一个自己的
"关卡列表"，点一行就 `level.load(<那个名字>)`。

**必须写清楚的代价**：

- 占用的名字在**原版战役里也被替换**（玩家那几关变成关卡包的内容）；名字相同，无法并存；
- 关卡在树里的**位置、顺序、解锁关系**跟着原版那一关走（编译表决定），改不了；
- 玩家在该关的**存档电路**（`schematics/<kind>/Default/circuit.data`，`kind` 取自 `meta.txt`）
  优先于 `campaign/<名字>/circuit.data`——已经玩过那关的存档需要清掉才会看到新起点盘。

## 需要研究才能做的（路线 B：新增名字）

要让**新名字**进关卡树，就得往游戏内存里的 `campaigns` 表插入条目。要研究的东西：

1. `campaigns` 模块的表结构（Nim `Table[string, …]` / `seq`，字符串所有权与 `save_monger`
   序列化版本——战役状态是会被存档的，插进去的条目必须能存能读）；
2. 树的位置从哪来（`build_level_tree_ui` 读哪些字段；`get_level_next` 是链表还是顺序表）；
3. 插入的时机（`find_all_levels` 之前？初始化后？）与线程（游戏主线程）。

第一步应该是**只读**的：hook `find_all_levels`，把它返回的关卡名列表 dump 出来，核对"编译表 =
磁盘上的 98 个目录"还是另一个集合（战役/沙盒/预览是否同表），并顺带量出条目结构。
这正是 `docs/HANDOFF-next-steps.md` 第 3 项"关卡信息服务化"的第 0 步。

## 建议的实验顺序

1. **只读探针**（半天）：hook `find_all_levels` 与 `get_level_next`，dump 关卡名/顺序/是否战役，
   确认编译表的边界；
2. **路线 A 验证**（同一天）：隔离沙盒里给一个现有关卡部署自定义 `meta.txt` + `circuit.data`，
   进关后从截图/日志确认标题、对话与棋盘都是我们的内容（同时确认存档盘优先级）；
3. 只有在前两步之后，再评估路线 B 是否值得（写游戏内存 = 需要版本绑定 + 存档兼容论证）。

## 实测（2026-09-28，路线 A 已验证）

新增真机用例与探针：

```text
tests/level-content-probe.cpp      订阅 TC_EVENT_LEVEL_LOAD 记下关卡名，进关后截两帧
tests/level-content-playtest.ps1   在隔离副本里重写 campaign/sandbox/，再让游戏进那一关
```

做法：把 `campaign/sandbox/meta.txt` 换成带 `dialogue` 的一份、把 `campaign/sandbox/circuit.data`
换成 `tests/float-fixture.cpp --m2` 的板子（两块 FP32 CONST → ADD → DISP），然后照常进沙盒。

结果（`build/level-content-board.png`）：**游戏进的还是 `sandbox`，但棋盘是我们的**——两块
`FP32 CONST`（值 1）、`FP32 ADD`、`FP32 DISP` 显示 `2 0x40000000`，而原版沙盒是空板。
这是"Mod 用 `campaign/**` 换掉一关内容"的端到端证据。

    [图片] build/level-content-board.png    （tests/level-content-playtest.ps1 的产物）

同一次运行还量到两条规则：

1. **`meta.txt` 是运行时解析的**：原版 `campaign/sandbox/meta.txt` 只有
   `kind/background/size/title` 四行、没有 `dialogue`；我们加上 `dialogue` 后，第一次跑
   （复用别的关卡文本 id）就弹出了开场对话框——那段文字只可能来自我们写的 meta。
2. **关卡文本是按 id 查翻译表的**：`translations/Chinese (Simplified).txt` 按**文件**分节
   （`=== campaign/sandbox/meta.txt ===`），节内把每个文本 id（`31337_…`）映射成译文。
   所以第一次跑虽然写的是英文，弹窗显示的是**中文**——那个 id 在翻译表里有译文。
   推论（**尚未实测完**）：Mod 自带的新文本要用**没人翻译过的 id**，并自带一节翻译；把 id 换成
   `99999_…` 的第二次跑里两帧都没拍到那个弹窗，所以"没有译文时回落到 meta.txt 原文"这一点
   还差一次探针确认（下一步）。

别忘了第 5 节那条：**玩家存档里的棋盘优先**（`schematics/<kind>/Default/circuit.data`），
所以"换关卡起点盘"只在没有对应存档时立刻可见。

## 相关符号（本仓库已收录）

```text
load_level__modelZutilities_u7740                 加载关卡（按名字，名字必须在表里）
loaded_level__modelZmodel95types_u840             当前关卡对象
change_scene__presenterZcontext_u2958             场景切换
save_level_data__modelZutilities_u5683            写关卡存档（save 事件由它派生）
find_all_levels__presenterZutilitiesZhelper95functions_u2713   编译表的遍历
get_level_next__presenterZutilitiesZhelper95functions_u1455    关卡顺序
is_campaign__modelZmodel95types_u739 / campaign_name__modelZmodel95types_u836
build_level_tree_ui__presenterZlevel95tree95ui_u471            关卡树 UI
as_sanitized_folder_name__modelZsanitized95path_u472           名字→目录名
```
