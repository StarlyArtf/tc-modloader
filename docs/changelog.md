# 版本历史

版本号只有一个来源：仓库根目录的 `VERSION`。`build.ps1` 由它生成 `src/version.hpp`，
加载器横幅与日志、安装器标题、`tcmod-cli --version`、分发包文件名全部读这一处
（此前 0.4.0 硬编码在五处，而源码里的界面 API 注释写的是 0.5.0/0.6.0，编号已经对不上）。
源码树现在是 **0.6.0**；最后一次分发的压缩包仍是 0.4.0，下面 0.4.1 起的条目都还没有重新打包。

## 未发布 0.6.0 — 符号画像与钩子链（地基第二项）

- **首个稳定游戏对象句柄**：新增 `TCGameHandle` 与 `game_handles` 能力；Board 在关卡加载时
  签发，下一次加载或场景切换自动提升代次并作废旧句柄。插件可查询、验证并受控解析句柄，
  不再需要长期保存无生命周期信息的 `void* board_model`。组件/导线/关卡 kind 只预留、不接受
  任意裸指针，等待统一快照从可信枚举结果签发。宿主 ABI 仅在尾部追加三个入口，旧字段偏移不变。
- **Board 句柄的真机验收，和它逼出来的一处缺陷修复**：新增只读探针
  `dev.game-handle-probe` 与自驱动版本 `dev.game-handle-probe-driver`，以及游戏层用例
  `game-handle-probe`（`tests/game-handle-probe-playtest.ps1`）：探针从主页入口进关卡、
  按主页关卡入口进入、按 Escape（玩家自己的离开动作）退出，两次进出都是玩家的路径，全程只用
  公开 ABI。
  真机实测暴露：游戏先 `level.load`、再在**同一帧**切到棋盘场景，旧实现把这次切换也当成
  “离开”，于是刚签发的句柄当场作废，整个关卡内 `get_current_game_handle` 一律返回
  `UNAVAILABLE`——只有真机生命周期能发现这一点。现在按引擎帧号区分（加载那一帧或下一帧的
  切换算进入），并把 `scene.change` 提前到**加载任何插件之前**由加载器自持：插件对该目标
  `create_hook` 会被明确拒绝并提示改用 `TC_EVENT_SCENE_CHANGE`，而不是抢先拿走 detour、
  静默关掉句柄失效机制。`TC_EVENT_SCENE_CHANGE` 的 `subject` 现在带上**游戏自己传给
  `change_scene` 的 context**：加载器拿走这个目标后，驱动（和 Mod）否则就再也拿不到它——
  board-panel 用例的“切场景后棋盘停止绘制”正是因此退化成 NOTE，补上 `subject` 后恢复为硬断言。
  探针随后还按**玩家的动作**验了离开路径：关卡内按 Escape，游戏自己调用 `change_scene(ctx,0)`，
  句柄随即失效（`DRIVER: self-exit=ok`），驱动自造的切换只保留为回退分支。
- **游戏兼容性与 SDK ABI 成为发布门禁**：新增 `compat/profiles.json`，统一声明支持构建的
  EXE/原引擎哈希与大小、全部 Loader RVA、所需符号别名和可升级旧 Loader；生成头供安装器与
  运行时使用，`tools/compat.ps1` 可在安装前离线识别。新增 Windows x64 ABI 基线与编译探针，
  对 193 项公开结构大小/对齐/字段偏移及稳定常量逐项比较；画像 id、清单哈希和 ABI 基线哈希
  写入 `release.json`，fast/host 测试与正式发布都会阻止未审核漂移。
- **发布基建收口**：新增声明式 `release/manifest.json` 与 `tools/release.ps1`，发布包从空
  staging 按清单收集，缺失、重复或未通过 Mod 契约校验即失败；公开示例不再靠
  `package.ps1` 手工 `Copy-Item`，补齐 board-panel、byte-adder、waveform 与 inspector。
  `.mod`、源码包和玩家包统一使用固定 ordinal 条目顺序、固定 ZIP 时间戳与压缩参数；
  `build.ps1` 固定 GNU ld 写入的 PE/COFF 时间戳，安装器、原生 DLL 与 CLI 在两次完整重编译后
  仍字节一致。同一 staging 连续打包两次必须 SHA-256 相同。最终目录携带玩家包内外两层
  `SHA256SUMS.txt` 与机器可读 `release.json`；fast 层新增 `release-contract` 回归。
- **插件不再硬编码混淆名**：新增 `src/symbol_profile.hpp`——一张"稳定别名 → 本构建 COFF 名"
  的表（20 项：`sim.do`、`sim.cycle`、`sim.settings`、`sim.setting.get/set`、`sim.state.read`、
  `level.load`、`level.loaded`、`scene.change`、`board.wire.update`、`save.count`、
  `save.path.*`、`runtime.emutls`、`ui.fonts`、`ui.pushFont`、`ui.invisibleButton`、
  `cost.gate/total/delay`）。加载器启动时整表解析一次并写日志；真机日志实测
  `Symbol profile: 20/20 aliases resolved`——表里每一项都在游戏里核实过。
  插件侧 `tc::resolveAlias(host,"sim.do")` / `resolveAliasAs<SimDo>(...)`，取不到就是 null，
  不会静默拿到错地址。
- **多个 Mod 可以共用同一个游戏函数**：加载器为已核实签名的钩子点安装**唯一**的 detour，
  插件各自加入链节（`tc::hook::addSimDo(host,priority,&callback,user)`）。链序为
  优先级升序 → Mod id → 注册顺序，日志给出成员表；现开放 `TC_HOOK_SIM_DO` 与
  `TC_HOOK_LEVEL_LOAD` 两点。语义：返回 0 继续、非 0 停止链；`call->skip_original=1`
  让游戏自己的函数不执行（替换行为）；需要看返回值时先 `call->run_chain(call)`，
  原函数保证**至多运行一次**。被拒插件不留链节。
- **链上目标被保留**：对 `sim.do` / `level.load` 调用原始的 `create_hook` 会被拒绝并提示
  改用 `register_hook_chain`——这正是"两个 Mod 都想钩 sim_do"从必然失败变成彼此共存的地方。
  其余目标（如界面驱动用的 `igInvisibleButton`）仍可用原始 Hook，重复目标照旧冲突。
- **迁移**：`example.cycle-guard`（sim.do）、`example.waveform-demo`（level.load）与
  `dev.sim-state` 探针都改走链与别名；示例清单相应声明 `symbol_alias` / `hook_chain`。
  真机证据：waveform 沙箱 `Hook chain level.load installed with 1 link(s)`；
  sim-state 沙箱同时启用 `example.cycle-guard` 时
  `Hook chain sim.do installed with 2 link(s): dev.sim-state@0, example.cycle-guard@0`，
  关卡照常跑完、映射断言全过。
- **验证**：新增离线用例 `tests/hook-chain.cpp`（假的 `sim_do`/`sim_get_cycle`/`load_level`
  符号 + 探针包）与 `tests/hook-chain.ps1`，断言三链节的顺序与参数传递
  （1000→1001→1002→1003，原函数只跑一次）、吞掉原函数的链节、以及"普通目标可原始钩、
  链上目标被拒"；`tests/native.ps1` 的冲突场景改为两个包抢同一个普通目标
  （第一个 ok=1、第二个 `Hook rejected`）。细节见
  [reference/symbols.md](reference/symbols.md)。

## 未发布 0.6.0 — 事件总线（地基第三项）

- **"关卡加载了 / 玩家按了运行 / 游戏保存了"不再需要每个 Mod 自己钩内部函数**：新增
  `sdk/tc_event_api.h` + `sdk/tc_event.h`，`host->add_event_listener(context, kinds,
  callback, user)` 订阅 `TC_EVENT_LEVEL_LOAD`（subject = 板模型、name = 关卡名）、
  `TC_EVENT_SCENE_CHANGE`、`TC_EVENT_SIM_COMMAND`（flags = 0 run / 1 stop / 2 reset）、
  `TC_EVENT_SAVE`（flags = 保存计数）。`kinds` 是位掩码，`TCEvent.kind` 是到达的那一位，
  一个监听器可以同时处理多种事件。
- **事件的来源都由加载器拥有**：`level.load` 与 `sim.do` 走已有的钩子链，加载器把自己的链节
  放在最高优先级（`-2147483647`），因此即使别的 Mod 吞掉了这次调用，事件照样发出；
  `scene.change` 与 `save.level`（新加入符号画像）在需要时才挂钩，没有监听器就完全不碰。
  启动日志写明每个事件源的状态（`Event source level.load: ok`、`Event source save armed`…）。
- **被拒插件从总线上摘除**：和 Hook、链节、界面、元件同一套生命周期。
- **验证**：`tests/hook-chain.cpp` 的事件场景（假 `sim_do`/`load_level`/`change_scene`/
  `save_level_data` 符号 + 一个订阅全部四种事件的探针包）断言四种事件都到达、顺序与调用一致、
  负载正确（`command=0`、`count=3`、`subject`），并且不影响游戏自身函数的行为；
  `tests/hook-chain.ps1` 额外断言四个事件源都被装载。真机：`example.waveform-demo` 改为订阅
  关卡加载事件取板模型（原先自己进链），波形／VCD／导线探针断言全过，日志
  `Event source level.load: ok` + `Waveform: board model capture subscribed to the level-load event`。
- **尚未做到：逐周期回调**。查证结论写在
  [verification.md](verification.md#仍未做到逐周期回调)：仿真的周期循环跑在游戏自己生成的
  机器码里（`compile_asm` / `compile_isa`），EXE 符号表里没有"跑一个周期"的函数可以钩，
  所以波形在"连续运行"下仍按帧采样、会跨周期。要真正做到每周期一次，需要像原生逻辑桥那样
  在生成的源码里插入回调，属于后续独立工作。

## 未发布 0.6.0 — 崩溃署名与日志轮转（地基第四项的一半）

- **插件崩溃会被署名，但不会被隔离**。先按计划做了崩溃遏制（VEH + `setjmp/longjmp`，
  因为 MinGW 的 GCC 没有 `__try/__except`）：该机制在孤立环境里四种情形全部恢复
  （`tests/fault-guard-probe.cpp`，`build.ps1` 内执行），但接到加载器的链节上时，处理函数执行完进程仍以
  `0xC0000428`（`STATUS_INVALID_IMAGE_HASH`）退出；把故障点改到加载器自己的受保护代码块
  结果相同。因为"一半时候管用的安全网比没有更危险"，最终交付可依赖的那一半：
  `tc-modloader-data/fault.log` 记录 `dev.hook-chain-crash in sim.do: access violation at 0x…`
  ——Mod id、当时在跑的回调/钩子点、异常类型与地址，然后让进程照常崩溃。
  完整实测与"要做到真隔离需要什么"写在 [verification.md](verification.md#安全模式与崩溃隔离)。
- **日志不再无限增长**：`loader.log` 超过 8 MiB 时轮转为 `loader.log.1`（只保留上一份），
  行格式不变，因此既有断言与文档中的日志样例都仍然作数。此前该文件在实测目录里已有 6.9 MB。
- `tests/hook-chain.ps1` 新增 `crash` 场景：探针包故意写空指针，脚本要求进程非 0 退出**且**
  `fault.log` 同时写明 Mod id、钩子点与原因。

## 未发布 0.6.0 — 能力协商与依赖版本约束（地基第一项）

- **插件不再靠"宿主结构有多大"猜能力**：`TCHost` 尾部新增 `host_version`
  （用 `TC_HOST_VERSION_CODE(0,6,0)` 打包）、`capabilities`（`TC_CAP_*` 位掩码）与
  `report_status(context, level, message)`。位只有在对应入口**确实实现**时才置上，
  承诺与实现同一处生成：`src/capabilities.hpp` 的 `loader_capabilities()`。
  SDK 侧给了 `tc::hostHas` / `tc::hostCapabilities` / `tc::hostVersion` /
  `tc::reportStatus`，它们自己检查 `host->size`，因此在没有这些字段的旧加载器上安全
  返回 false/0，插件不必再手写 `offsetof` 比较。
- **包可以声明自己要什么**：`mod.json` 新增 `capabilities` 列表（`log`、`symbol`、
  `hook`、`logic`、`component`、`ui_page`、`ui_slot`、`texture`、`status`）。
  扫描阶段就区分两种拒绝并给出可读理由——`Unknown loader capability: x`（作者写错名字）
  与 `This loader does not provide the capability: x`（加载器比包旧）。后者是本次改动的
  重点：以前这种情况要等到 `tc_mod_load` 里静默失败。
- **依赖可以带版本约束**：`requires` 除 id 数组外接受 `{"id": ">=1.2.0"}` 对象形式，
  新增 `optional`（可选依赖没启用就不检查，启用则照样检查）。约束支持
  `= != >= > <= <`、逗号联合与 `*`；版本比较取开头的点分数字段，所以
  `1.10.0 > 1.9.0`（纯字符串比较会弄反），尾随文字被忽略。校验在写入任何文件之前完成，
  失败时磁盘保持原样，Mods 页显示
  `X requires Y >=1.2.0, but the enabled version is 1.0.0`。
- **Mods 页跟着变**：详情栏显示依赖及其约束、可选依赖、包声明需要的加载器能力；
  插件通过 `report_status` 报的状态按级别（信息/警告/错误）着色显示在同一行。
  加载器启动日志新增一行 `TC Mod Loader 0.6.0 capabilities: …`，加载每个原生包时记录
  它声明的能力。
- **验证**：新增离线用例 `tests/capabilities.cpp`（能力表完整性、名字规范、
  `VERSION` 与 `TC_MODLOADER_VERSION_STRING` 一致、版本序、约束语义、两种依赖写法与
  四种拒绝路径、文档与代码的能力名单一致），`build.ps1` 内执行；`tests/run.js` 增加
  11 个包管理场景（`--version` 与 `VERSION` 一致、`--capabilities` 名单、能力声明通过、
  未知能力被拒、约束满足/不满足/数字序/可选缺失/可选启用后仍检查/约束类型错误），
  共 35 项全通过。
- 细节与表格见 [reference/capabilities.md](reference/capabilities.md)。

## 未发布 — 元件预览：引脚编号 + 表格

- **修好"自定义元件引脚名太长时在元件预览里糊成一团"**：预览（底部元件栏与元件工坊共用
  `build_custom_component_preview`，EXE RVA 0x38e990）按引脚逐个调用引擎导出的 `igText`
  画名字，而预览把元件缩小、名字却按原字号画，于是长名字压住旁边的名字（实测名字可到 470 px，
  而同一排引脚只隔 24 px）。摊开、倾斜 45°、缩小都试过，在 400 px 高的面板里只会让名字
  到处乱飞，最终按玩家的方案改成**编号表**：预览图里只画编号，名字全部放进面板空白一侧的
  表格里（多列排布，字号默认 70%，`TC_MODLOADER_PIN_TABLE=<百分比>` 可调，`=0` 关闭）。
  编号由加载器**实时分配**：先从名字反推出引脚位置，再按引脚外框**顺时针**走一圈
  （上 → 右 → 下 → 左，从左上角开始），编号递增。编号用纯白画（预览里的文字偏暗，
  一位数字要看得见）；每个编号**正对自己的引脚**、紧贴该引脚那条白线的外端（4 px 间距），
  同一条边上的编号在同一行（试过错开成两排换取更大字号，玩家反馈"看着错位、很乱"，
  因此对齐优先）；字号由"每条边的跨度 ÷ 该边引脚数"（不用最近邻，手绘元件可能有两个引脚
  几乎重合）算出的最小间距决定，朝外会顶到面板边时整条边改画在内侧。
  预览给每个引脚画的白条现在只用来标位置：加载器把它厚度减半、长度缩到一半，而且缩短时
  **固定贴近引脚的那一端**（早先按中心缩短，白条反而离引脚更远）。这根白条同时是"引脚在
  哪、在哪条边、朝外是哪边"的唯一精确依据——它的矩形被记进本帧的引脚记录，下一帧的编号
  就按它定位（钩 `ImDrawList_AddRectFilled`，只认预览那一处调用）。
  同一套逻辑也覆盖**元件工坊里"编辑外观"的编辑器**（`build_editor`，名字 0x3ae126、白条
  0x3adf96）：那里原先各画各的，引脚名照样重叠。编辑器里**只画编号、不画表格**——整面板都
  是游戏自己的调色板，表格会压住颜色（玩家反馈）；元件栏／工坊预览里的表格则优先放在面板
  **右侧**（左侧常有游戏自己的控件）。
  切换预览／外观编辑器时原本会闪一下原版的重叠名字：编号依据的是**完整一帧**的引脚数据，
  切换那一帧没有数据，旧实现"认不出就按原样画"。现在认不出上一帧的名字**这一帧不画**
  （编号下一帧出现），表格也延后一帧，切换变成干净的替换、中间最多空一帧（约 16 ms）；
  位置匹配放宽到 2 px，面板轻微滚动不会被误判成切换。
  同一份源码可以用 `-DTC_PIN_PATCH_ONLY` 编成**独立补丁**（`dist\pin-names-patch\game_engine.dll`
  ＋安装说明）：它照样是 game_engine.dll 的代理（转发全部导出、实现 igEnd），但**不扫描/加载
  任何 mod、不改存档目录、没有 Mods 菜单**，只装这三处钩子，给不想要加载器的玩家用；
  版本不符时它不装任何钩子，游戏等同原版。`build.ps1` 会一并产出这个补丁。
  加载器只接管预览那一次文本调用（返回地址 0x38f0ef），其余 193 个调用点原样转发
  （变参经 `igTextV`）；一切判断基于**上一帧**完整数据（`igEnd` 作为帧边界），因此编号和表格
  逐帧稳定；绘制用的小字号按窗口对象上的 `FontWindowScale`（`[window+0x308]`）精确还原，
  不再因 ImGui 的整数取整逐帧漂移（实测曾让面板文字从 29 px 慢慢缩到 28 px）。
  证据：改前 `build/pin-label-out/theirs9-preview.png`、放弃方案
  `theirs20-preview.png`、只给放不下的编号 `theirs26-preview.png`、最终
  `build/pin-label-out/theirs32-preview.png`；同时删掉了上一轮改错地方（棋盘标签网格）的
  45° 与那条会让游戏卡死的 0xe9100 死钩子。回归：
  `tests/native-component-playtest.ps1`、`tests/native.ps1`、`tests/ui-board-panel-playtest.ps1`、
  `tests/wire-palette-tool-playtest.ps1` 全部通过。
  调查与地址见 [research/pin-label-overlap.md](research/pin-label-overlap.md)。

## 未发布 — 关卡波形面板与 VCD／图片导出

- **修好"Mod 页面标题字号从未按主页字号显示"（一个静默失效的绑定）**：页面容器为了让标题
  与游戏主页同字号，会调用游戏自己的字号栈。但那两个函数（`igPushFontScale` /
  `igPopFontScale`）是**游戏可执行文件**里的 Nim 函数，代码却按名字去**引擎 DLL** 里找
  （引擎导出 1505 个名字里没有一个 `presenterZ…`），因此永远解析失败；而且 PopFontScale 的
  名字还写错了一位（真实是 `_u7966`，写的是 `_u7940`）。`if (withFont)` 永远为假 → 页面
  标题一直用窗口自带字号，且不报错。现在改为对**宿主自己的窗口**调用引擎导出
  `igSetWindowFontScale`（页头用主页倍率、内容区恢复 1.0），不再驱动游戏的字体栈——试过
  绑游戏那两个函数，结果是页面连绘制体都进不去（沙箱里 `-DriverMode` 直接失败），所以
  只用"我们自己的窗口、我们自己的缩放"。证据：日志
  `Mod page font scale applied: window font 45 -> 86`，且 `tests/ui-page-playtest.ps1`
  的驱动模式与双插件模式都通过。

- **修好"工具栏里的 Mod 工具展开后没有文字"**：根因不是字号——插件被注入时 ImGui 的当前
  字体是游戏的**图标字体** `Icon_Complete.ttf`，而图标字体没有拉丁/中日韩字形，
  `igTextUnformatted` 提交的字符串连一个字形顶点都不产生（矩形/按钮底不依赖字形，所以只有
  文字消失）。加载器现在启动时按符号读游戏的字体表
  （`defined_fonts__presenterZimguiZimgui_u7413`）挑出正文面（本构建
  `defined_fonts[2]=NoroshiCode_Regular.ttf`），在绘制插件工具前用游戏自己的
  `igPushFont__presenterZimguiZimgui_u7614` 推送、画完 `igPopFont()` 还原（与游戏在同一个
  函数里持有的字体作用域严格配对），并且**不再改写窗口字号**——借用正文字体后字号就是游戏
  自己的 UI 字号（实测 45 px，与侧栏面板一致），日志会写
  `Tool column text font: defined_fonts[2]=... (used for plugin tool draws)`。
  插件侧不需要任何字体处理：`tc::ui::text` 直接可显示中英文。
- 新增真机端到端用例 `tests/wire-palette-tool-playtest.ps1`（配
  `tests/toolbar-hover-driver.cpp`）：把真实鼠标压在调色盘瓦片上抓帧，得到展开面板里中英文
  正常显示的截图；沙箱准备按 `state.json` 的 `original` 哈希先还原
  `asset/shader/*.vert` 再 `apply`，避免二次打补丁把游戏打死。诊断工具
  `tests/toolbar-font-probe.cpp` / `tests/toolbar-font-playtest.ps1` 记录三个上下文的字体、
  游戏字体表枚举与推字体前后的截图对照；加载器新增开发用 `TC_MODLOADER_TRACE_TEXT=<子串>`
  （钩 `igRenderText`，打印字体/字号/顶点数增量）与 `tools/scan-calls.js`（从导出表反查
  调用点）。细节与反汇编证据见 [research/toolbar-tool-handoff.md](research/toolbar-tool-handoff.md)。

- **新增"工具栏插槽"能力，导线调色盘变成游戏里的一个工具**：宿主接口
  `tc::ui::registerBoardToolbar(id, draw, user, host)`（插槽类型 `TC_UI_SLOT_BOARD_TOOLBAR`）
  让插件把控件画进**游戏自己的工具栏**里——加载器在游戏最后一个工具按钮的子窗口结束时
  注入，并把插件工具摆到该按钮正下方（工具列几何是加载器从游戏自己的工具按钮实测学到的，
  不是写死的坐标）。因此调色盘现在是一个 40×40 的**色块按钮**，**鼠标悬停**才展开调色板
  弹窗（可视取色器：色相/饱和度方块 + 色相条 + 透明度条，带 R/G/B、H/S/V、#hex 精确输入，
  松手自动保存），另有 `取色器 / 使用此颜色`、游戏自带 11 色、我的颜色与最近用过。功能不变：
  仍然接管游戏调色板表、导线取色器、新导线/放置导线使用当前色，`palette.json` 存档照旧。
- 新增开发探针 `dev.enter-board`（`tests/enter-board.cpp`）：只 Hook `igInvisibleButton`，
  自动按一次主页入口进入关卡——板内界面（侧栏面板、电路）只能在关卡里看到，而其它驱动
  会占用 `handle_update_wire`，和调色盘冲突。配合下面的定时截图即可给板内界面截图。
- 加载器的开发截图增加**定时触发**：`TC_MODLOADER_SHOT` + `TC_MODLOADER_SHOT_DELAY=<毫秒>`
  在 `igEnd` 里按时间抓帧（菜单页的绘制只在主页运行，抓不到关卡里的东西）。
- **Mod 管理页重做**：从"一屏文字 + 单列列表"改成两栏式界面 —— 左侧包列表（勾选框 +
  名称/版本，启用的显示绿色、有问题的显示红色、当前选中行高亮），右侧是选中包的详情
  （名称、id/版本/作者、是否勾选、本次运行状态、原生/资源类型、资源与原生文件数、
  包文件名与 SHA、依赖、描述、错误信息）＋「加入/移出启用列表」「打开文件夹」；工具栏
  `刷新列表 / 打开文件夹 / 全部启用 / 全部停用`；存档设置收进可折叠的「存档（已隔离）」；
  页脚是未保存提示 +「应用更改 / 关闭」+ 一行状态。仍然只用引擎导出的 ImGui 接口，没有
  自绘控件。
- 加载器新增两个**开发用**环境变量（不设就不生效）：`TC_MODLOADER_OPEN=1` 让管理页
  在启动后自动打开，`TC_MODLOADER_SHOT=<文件.bmp>` 在管理页打开若干帧后用 `glReadPixels`
  把画面写成 BMP——这台机器的游戏窗口是独占翻转的全屏 GL 窗口，外部截图只能拿到黑屏，
  所以界面改动只能这样看。
- **工具栏插槽对多 Mod 是通用且无冲突的**：所有插件工具按 `Mod id → 插槽 id` 排序后
  依次绘制（顺序与加载顺序无关，可复现）；每个插槽在 `PushID(mod)` + `PushID(slot)` 的
  独立作用域里绘制，两个 Mod 用同名控件不会互相串；每个插件最多 8 个插槽，冲突的 slot_id
  会被拒绝（返回 −3）。日志给出布局：`Board tools in the game's tool column (top to bottom): …`。
- **工具瓦片按游戏自己的尺寸与网格画**：游戏工具按钮实测是 80×80、两列、列距/行距 96、
  底色 `(53,50,68)`、圆角 10；插件工具沿用同一套（加载器从游戏按钮学到网格间距并按格
  排布，插件用 `tc::ui::Canvas` 画同尺寸瓦片、底色与圆角，当前颜色作为"图标"占据游戏的
  图标区 44×44）。无绘图能力时回退为同尺寸的普通色块按钮。`colorPicker` 之类的大控件留在
  悬停弹窗里，不再占据工具栏。
- 悬停判定修正：弹窗会盖住瓦片，ImGui 的 item hover 随之变假，于是"关→开→关"逐帧闪烁。
  现在改成**几何判定**（鼠标是否落在瓦片矩形内，`Canvas::mousePosition()` 与尺寸比较），
  并把弹窗放在瓦片**旁边**而不是上面（`setNextWindowPos(..., Cond_Appearing)`，与游戏自己的
  工具弹窗一致）；鼠标同时不在瓦片和弹窗上（且没有控件在拖动）时才关闭，因此"移开即收起"
  不再抖动。
- **输入/输出数量改为按电路板数**：游戏的 `level_used_input` / `level_used_outputs`
  两个全局量不可信——实测在一个"1 进 1 出"的电路上它们仍返回 `2/2`，面板因此多画了两条
  泳道。现在插件直接数板上的 IO 元件（kind `0x3f` = 关卡输入、`0x44` = 关卡输出，与已验证
  的自定义逻辑网表构建器同一套），并通过 `Sampler::setDeclaredCounts()` 覆盖全局量；
  板上一个都没有时仍回退到全局量。日志会写一行 `Waveform: the board has N input(s) and
  M output(s)`。
- **重新开始仿真会自动清空波形**：检测到周期**倒退**（新一轮仿真）时清掉已有采样、从头
  记录，不再把两次运行的波形接在一起；日志写 `simulation restarted - waveform cleared`。
- **导线探针**：在电路板上选中一条导线 → 面板 **Probe wire** 把它加成一条绿色泳道
  （同时写进 VCD，变量名 `p0`…），**Clear probes** 清空。取值用游戏自己的
  `sim_state_read_bits(offset, width)`；导线记录 `+0x38` 就是状态字节偏移、`+0x30` 是位宽
  （实测 + 反汇编核对）。板模型由一个没人抢的 Hook 捕获：`load_level` 的第一个参数。
  真机断言：驱动器挑"接在关卡输出侧的那条导线"，逐行比较探针值与关卡自己的输出历史，
  必须完全一致（`and_gate` 的 `0,0,0,1`）。
- 采样时机修正：仿真状态是在步进**结束**时写好的，因此在"周期刚变化"的那帧读会拿到上一
  周期的值（实测关卡输出已为 1、探针仍为 0）。`Sampler::sample()` 现在把行**延后一次调用
  落盘**（捕获时读关卡 I/O，落盘时读探针），行为仍是"一周期一行、暂停不增长"。
- 波形只跟着**仿真**走，不再跟着渲染帧走：`tc::trace::Sampler::sample()` 只在周期变化时
  追加一行（暂停、关卡还没开始跑时一行都不加），因此面板头部现在是
  `samples N  simulating / paused (waveform held)  cycle a..b` —— 一帧一行变成了
  **一个周期一行**。代价写清楚：仿真跑得比渲染快（例如"连续运行"）时两次采样之间会跨过
  多个周期，波形在 VCD／界面上表现为时间戳跳跃；要让每个周期都被画出来需要挂每周期步进
  函数并在仿真线程缓冲，属于下一步（见 [verification.md](verification.md)）。
- 面板可以**指定监测哪些信号**：每条输入／输出一个复选框，未勾选的信号不再画、也不写进
  VCD（数据仍在采样，勾回来即可），带 `watching 2/2 in, 2/2 out` 的提示。顺序调整为
  波形画布在上、控件在下——控件排在画布前面时会把画布挤出宿主给的内容高度。
- 新增 `sdk/tc_trace.h`（`tc::trace::Sampler`）：只读地按周期取当前关卡的输入与输出，
  数据来源是游戏自己的两份历史缓冲（`simulation_input_replay`、
  `simulation_output_history_pins`），槽位**运行期按"哪些字节在动"发现**，数量以
  `TCGameStateModel` 声明的引脚数为准；某个引脚整段没变化时退回按相邻槽位间距推断，
  并用 `assumedStride()` 明说。`writeVcd()` 输出标准 VCD（GTKWave／Surfer 可直接打开）。
- 新增示例 `examples/waveform-demo`（`dist/example.waveform-demo.mod`）：在电路板侧栏面板里
  把关卡的每个输入／输出画成方波泳道（上蓝下黄、8 周期时间网格、只显示最近 N 个采样），
  带 **Export VCD**、**Export image**、**Clear** 三个按钮。示例包 `hooks=0`，面板完全由
  加载器调度，关卡关闭即不再绘制。
- 导出图片只能从进程内取：本构建是**独占翻转（independent flip）的置顶 GL 窗口**，
  `PrintWindow` 与桌面抓屏都拿不到任何像素（实测；仓库里既有的 UI 截图因此全是纯黑）。
  插件用 `glReadPixels` 读默认帧缓冲并写 32 位 BMP（`GetProcAddress("opengl32.dll")`
  取 GL 1.1 入口，不需要额外链接）；读取发生在 ImGui 帧构建期间，拿到的是**两帧前**的
  画面——面板每帧都在画，所以正是想要的那一张。
- 新增离线单测 `tests/trace.cpp`（`build.ps1` 内执行）：按实测布局（输入槽 0/8、输出槽
  55/64）验证槽位发现、数值、稀疏回退与 VCD 文本。
- 新增真机用例 `tests/waveform-playtest.ps1`（驱动 `tests/waveform-driver.hpp`，测试包
  `dev.waveform-demo-driver`）：驱动器自己进入 `and_gate`、编译、运行，插件记录面板真正
  画出的每一行，脚本断言它与关卡自带测试一致（输入 0,1,2,3 → 输出 0,0,0,1，输出为高的
  行必定输入为 3）、VCD 里带同样信号与数值，并在**加载器报出的面板矩形内**统计泳道颜色
  像素，证明波形确实出现在渲染帧里。`-Example` 模式只验证发行包注册与加载。
- 修正 `build.ps1` 里 menu-demo 三处 `-D...='"..."'`：Windows PowerShell 5.1（文档推荐的
  `powershell -File build.ps1` 用的就是它）会把原生参数里的双引号丢掉，`-D` 定义变成
  `settings` 而编译失败；带空格的引号定义改由 g++ 自己解析的响应文件传递，5.1 与
  PowerShell 7 下都能构建。
- `dev.cost-watch`（开发探针）不再 Hook `handle_update_wire`：Hook 是**独占**的，探针先
  拿到这个目标就会让真正需要它的玩家 Mod 加载失败（`local.wire-palette` 依赖同一个
  `handle_update_wire`，加载顺序一变就成败不定——日志里从 9/17 起成对出现）。探针只保留
  评分相关目标（`get_cost`／`get_delay_cost`／`get_gate_cost`／`build_scores`／`preorder`），
  报告里的 board 行随之固定为 `no context yet`；评分数字不依赖它。

## 未发布 — 自定义 UI 绘图

- 新增宿主可选接口 `TCHost::register_ui_slot`（尾部追加，旧宿主按结构大小返回 −1）与
  SDK `tc::ui::registerBoardPanel()`：把插件面板画进**电路板自己的窗口**右边缘。
  宿主拥有容器、ID 作用域、输入归属与生命周期，插件只画内容；每插件上限 8 个插槽，
  同名插槽／同名控件跨插件互不影响。
- 输入归属：宿主在游戏自己采样鼠标状态之前绘制面板（`build_board_ui` 的
  `igIsAnyItemActive` 调用点，RVA `0x46b593`），因此该帧 "有控件在活动" 的回答包含面板，
  落在面板上的点击／拖动不会被电路板同时处理。插件若也 Hook 了同一个入口（MinHook 会
  跟进 `jmp` thunk、换掉返回地址），加载器回退到一次短线栈回溯来确认调用点。
- 生命周期：面板由电路板自己的每帧代码绘制，关卡关闭即不再绘制，没有需要清理的状态。
  驱动测试用游戏自身的 `change_scene` 离开关卡，确认面板帧随之停止。
- 侧栏面板的内容区改为独立子窗口：内容被裁到面板内，被裁掉的项目不可悬停／点击；
  内容超出高度时由宿主在右缘绘制滚动条并驱动滚动（本构建的滚轮不会送到 ImGui，
  内容子窗口本身可滚动，插件也可自行调用 `igSetScrollY_Float`）。
- 主菜单页面容器改用同一段内容区实现（`drawContentRegion`／`scrollStrip`）：页面内容
  同样被裁剪、可滚动，坐标相对内容区；`tests/ui-page-playtest.ps1 -DriverMode` 断言
  内容区与滚动条存在且页面控件仍可点击。
- 面板只声明自己的矩形：面板外的点击仍归电路板，真机测试用"折叠线以下的探针点击"
  与画布点击做了双向对照。
- 新增 `examples/board-panel`（`dist/example.board-panel.mod`）：Ping 按钮、复选框、
  波形画布和一段 24 行、需要滚动才能看完的列表；新增
  `tests/ui-board-panel-playtest.ps1`（真实鼠标点击 + 游戏自身输入采样正对照 +
  裁剪探针 + 滚动条拖动 + 场景关闭）与 `tests/ui-slot.cpp`（宿主版本检查与参数校验）。
- 修正 `tools/exports.js`：改为从原版 `tc_game_engine.dll` 生成指纹与转发表。此前它从
  已安装的 `game_engine.dll`（即加载器）读取，会写出加载器永远无法匹配的引擎哈希，
  并把加载器自己实现的导出（`igEnd`、`igIsAnyItemActive`、`tc_logic_*`）误写成转发。
- 新增键盘支持与实测：`tc_ui.h` 增加 `Key_*` 枚举（ImGuiKey，值经真实按键消息核对）、
  `tc::ui::keys::down/pressed/released/amount`、`setKeyboardFocusHere()`、
  `isItemFocused()`；与既有的物理热键（`keyPressed(VK_*)`）在文档里区分开。
- 新增 `tests/ui-keyboard-playtest.ps1` 与探针包 `dev.ui-keyboard-probe`：用真实
  `WM_KEYDOWN/UP`（带扫描码）、`WM_CHAR`、`WM_IME_CHAR` 驱动插件页面，逐段断言缓冲区
  **完整内容**——按键打字一次一个字符、单独的 `WM_CHAR` 一次一个字符、`你`/`好`
  以 UTF-8 进入、Escape 被输入框消费并回滚文本。中文组合串／候选窗给出人工验收清单。
- 并入一条被纠正的误判：第一版驱动同时发 `WM_KEYDOWN` 与 `WM_CHAR`，每字符出现两次，
  曾写成"这套构建重复投递字符"。实际是消息循环的 `TranslateMessage` 已把按键转成
  `WM_CHAR`，多发的才是重复来源；`tc_ui.h` 与文档已按实测更正，不需要插件去重。
- 新增 `tests/ui-key.cpp`：键盘辅助函数的严格解析、未加载时的空操作、参数转发与枚举值。
- 新增一键人工验收台 `tests/manual-ime-test.ps1`：准备隔离沙箱、写出 `CHECKLIST.txt`、
  以可见窗口启动游戏（`-Hidden` 只做冒烟检查）；探针同时记录系统为输入法组合窗／候选窗
  要求的位置（`IME page composition=… candidate=…`），让"候选窗位置对不对"变成日志里的数字。
- 人工验收（2026-09-18，用户确认）：页面与板上面板的中文输入、选词上屏、Esc 取消组合
  通过；候选窗跟随光标（`composition/candidate style=0x32` 随每个字移动）；板上打字未
  触发板面快捷键。已知现象：独占全屏下候选窗出现时画面闪一下，属系统合成行为，加载器
  不绘制候选窗；文档给出可选缓解方式。
- 显示诊断与窗口尺寸回归：每次启动记录 `Display: dpi-awareness=… window-dpi=… monitors=…
  client=… surface=… ratio=…`；`tests/ui-board-panel-playtest.ps1` 会把游戏窗口从
  2560x1600 改成 1792x1120，断言面板按新窗口重排、插件上报的按钮绝对坐标随之变化，且
  真实点击仍然命中（`Ping clicked, count=2`）。本机 175% 缩放（`window-dpi=168`）下
  全部通过。
- 内容子区域不再保留引擎自己那条不可交互的滚动条，插件获得完整内容宽度；宿主的滚动条
  仍是唯一滚动入口。
- 面板新增宿主绘制的**折叠开关**（标题行右侧 `-`/`+`，整块可点）：折叠后只剩一条标题栏，
  再点展开。状态由宿主保存，插件无法自己折叠或留下无法恢复的状态；折叠期间裁剪区只有
  标题栏高，内容既不显示也不可点，但插件 `draw()` 仍会被调用。
  `tests/ui-board-panel-playtest.ps1` 断言：折叠后矩形高 60、折叠期间点按钮不计数、
  展开后按钮恢复（`count=2`），且折叠期间插件仍在绘制。

- 新增 `tc_ui_texture.h` 和宿主可选纹理 API：包内图片（中文路径）、RGBA 上传、UV 裁切／
  翻转／着色和图片按钮。按 Mod 管理所有权与配额，延迟到后续帧释放，上传后恢复 GL 状态。
- PNG／RGBA 真实 GPU 回读、透明度／方向、UV、错误输入、跨 Mod 释放拒绝、线程限制、
  配额与连续 180 帧绘制已通过。示例加入加载／释放／翻转图片。

- 新增可选 `tc_ui_draw.h`，独立加载 19 个绘图／几何导出，不改变宿主 ABI 和旧控件加载要求。
- `Canvas` 提供线、圆角矩形、渐变、圆、三角形、贝塞尔、折线、凸／凹多边形和文字；
  统一局部坐标，自动占位、鼠标状态快照，画布和嵌套裁剪作用域自动恢复。
- 新增 `example.drawing-demo.mod`：主菜单页面与 F8 浮动面板，支持拖动曲线控制点。
- 真实引擎逐图元顶点回归、颜色／坐标回读、移动／缩放／子窗口滚动、裁剪恢复与 180 帧持续运行。
- 更正旧交接中“插件不能调用 draw list”的推断；旧探针失败不代表该能力不可用。

## 0.4.6 — 声明式 C++ 元件

- 新增 `TCHost::register_component` 和 `tc::TCNativeComponent`：只声明稳定 ID、名称、
  输入／输出引脚、代价和回调，加载器自动生成定义并完成导入、原型设置和逻辑注册。
- 输入采集顺序与声明顺序一致，自动建立依赖链，支持每方向 1–8 脚，不要求作者写占位门。
- 拒绝重复 ID、非法形状和超界代价；旧宿主通过结构大小检查返回不可用。
- `examples/byte-adder` 移除对手写定义生成器的依赖，仍保留 1 门／1 延迟声明。
- 修正输入跨 payload 字边界时的布局，避免总位宽不超 128 却因空隙访问第三个字。
- 新增声明接口单元测试和 8 输入／8 输出真机压力用例。

## 0.4.5 — 插件界面接口

- 新增 `sdk/tc_ui.h`：插件画面板需要的东西一次封装好——45 个引擎 ImGui 导出在加载期
  统一解析（缺任何一个就写日志并拒绝加载，不再等到绘图时才炸）、类型化控件、
  `Window`/`Child`/`Disabled`/`TextWrap` RAII 作用域、面板定位与字号约定、视口尺寸、
  边沿触发热键。纯头文件，不改 ABI：全部走已有的 `host->engine_proc`。
- 修正 `examples/wire-palette` 的一个真实缺陷：`igSetNextWindowPos` 实际是三个参数
  （`pos`/`cond`/`pivot`，引擎从第三个参数寄存器读 pivot），原来只传两个，pivot 取的是
  寄存器里的残留值。
- 三个界面示例改用 `tc_ui.h`：`mod-inspector`（面板 + 视口 + 鼠标坐标）、
  `cycle-guard`（工具条 + 面板 + 复选框 + 禁用/换行作用域，并补上清单里一直写着、
  代码里却并不存在的 F6 热键）、`wire-palette`（取色器、颜色块、输入项状态）。
- 新增 `tests/ui-playtest.ps1`：在隔离副本里跑真实 `tcmod.mod-inspector` 包，断言面板确实
  在真实 ImGui 帧内绘制，并核对视口/鼠标返回值合理；证据行会打印实际分辨率。
- 修正 `tests/native.ps1` 的既有缺陷：夹具没有拷贝 `compile.dll`，native 运行时初始化
  必然抛错，四个模式全部失败；补上后四项通过。

## 0.4.4 — 界面刷新阶段的计算

- 修掉 `examples/byte-adder` 的一个真实缺陷：回调在 `TC_LOGIC_REFRESH`（界面刷新）
  阶段直接返回，没有算输出。仿真/关卡判定走 `TC_LOGIC_CYCLE` 所以一直是对的，
  但界面与元件工坊的实时表格读的是刷新阶段的值，于是加法元件"不会算加法"
  （用户实测反馈）。
- 现在 `CYCLE` 与 `REFRESH` 共用 `computeBytes()`，只有 `REFRESH` 跳过计数与状态
  （刷新跑在状态副本上）。日志新增前几次刷新的 `byte-adder: peek ...` 取样。
- 新增 `tests/byte-adder-smoke.ps1`：隔离副本里跑**真实包**，断言引脚几何、
  刷新取样、周期取样都满足 `sum/carry_out` 的预期函数，且关卡 40 周期无失配。
  此前这条路径只有仿真断言，缺陷因此漏网。

## 0.4.3 — 声明门数进入板级分数

- 整板门数原先无视声明值：游戏会把自定义元件展开成内部电路再逐门相加，
  `examples/byte-adder`（声明 1 门，占位电路 Mux+NOT）因此在界面显示 51 门。
- 加载器现在在插件自己的 `tc_mod_load` 作用域内捕获原型上的 `(门数, 延迟)`，
  并用声明门数替换该元件的展开计数；同一元件展开出的内部节点不再重复计数，
  嵌套 Mod 只计最外层一次。游戏自身「跳过自定义元件」的调用语义不变。
- 真机回归：`tests/component-cost-playtest.ps1` 新增 `TC_DECLARE_GATES` /
  `TC_DECLARE_DELAY` 与 `observe` 模式（元件由真实 Mod 提供），byte-adder 关卡
  编译统计与界面分数均为 `gates=1 delay=1`；`tests/component-timing-playtest.ps1`
  9 例与 `tests/native-logic-playtest.ps1` 11 例全绿。
- `dev.cost-watch` 改为按目标逐个安装 Hook：被加载器占用的
  `get_gate_cost` / `preorder` 会跳过并写明（`observing 4/6 targets`），
  其余观察照常工作，不再整体加载失败。

## 0.4.2 — 原生逻辑：多位宽与形状组合

- ABI 与运行时按引脚位宽泛化：注册接受每方向 ≤8 脚、每脚 1–64 位、总输入 ≤128 位。
- 输入按引脚宽度打包进两个 payload 字，保持 4 参数外调形状（超过 4 个参数会让游戏 JIT
  触发 `register_frame.nim(119,3) ... == EXP_NULL`）。
- 字宽输出改为"回调写预留状态槽 + 生成代码按位 `load` 组字"，因为外调返回值只有按 1 位
  消费才可靠；形式与游戏 `com_maker_bit_8` 一致。
- 内部门识别范围扩到 `0x03`–`0x0b`、`0x12`–`0x1e` 与字宽 Mux `0x2a`；未识别实例写诊断。
- 真机场景扩到 11 个：新增 2×8 位、3×8 位、8+3 位、1+8+8 → 8+1 位四类形状。
- 修正夹具两处缺陷（组件计数不符、连线少走一格），并新增 `exposes N operand(s)` 诊断，
  会原样打印退化的发射行。

## 0.4.1 — 原生逻辑：接口形状自定义

- 回调 ABI 升到 2：`TCLogicIO` 携带 `input_count`/`output_count`、`inputs[8]`、`outputs[8]`。
- 形状由元件定义决定；回调输入元组 = 第一个输出门的操作数列表；一次 invoke + 多次回读。
- 新增 1 进 1 出、3 进 1 出、3 进 2 出三个真机场景（`not_gate`/`and_gate_3`/`full_adder`）。
- 确认两条定义序列化约束：多脚引脚几何（左列间距 8）与 v14 组件尾部字段顺序。
- 修正夹具缺陷：原理图不再复制关卡自带 IO（否则关卡判定与界面表格读不同输出脚，
  会出现"当前输出与期望不符却通关"）。

## 0.4.0 — 元件时序、声明延迟与原生逻辑（分发包版本）

- 电路封装元件的**声明延迟**参与编译期时序统计：串联相加、并联取最长、嵌套只计外层一次；
  反馈环、多驱动、收缩后成环、非法数据与溢出保留游戏原生统计并写日志。
- SDK 新增 `setPrototypeGateCost` / `setPrototypeDelay` / `TCPrototypeBuilder::setDesignCost`。
- 新增 `dev.cost-watch.mod` 诊断包、`design-stats.txt` 覆盖开关与
  `python tools/circuit_format.py --analyze` 设计期预检。
- 原生逻辑回调（`sdk/tc_logic_api.h` + `register_logic`）进入真实代码生成链路，
  游戏继续负责关卡判定、内置元件、暂停与重置；`examples/custom-or` 演示内部 AND 后备、
  回调 OR。
- 真机回归：`tests/component-timing-playtest.ps1` 9 例；`tests/native-logic-playtest.ps1`
  3 例（`or`/`mixed`/`multi`）。
- 新增元件探测与验证工具：`dev.kind-list.mod`（125 个内置元件 kind/引脚）、
  Delay Line 引脚与代价反推、`delay` 拓扑 fixture。

## 0.3.0 — 独立存档与升级

- 装有加载器的游戏始终使用独立存档（即使全部停用或 Shift 跳过原生插件）。
- 新增"导入原版存档（新副本）"：每次新建副本、逐文件校验、失败不切换；排除 Steam 云元数据。
- 存档路径在游戏初始化前重定向，按 `USERPROFILE` 拼接（仅设 `APPDATA` 不足以隔离）。
- 副本选择保存在 `tc-modloader-data/saves.ini`。
- 验证：Unicode 文件名、二进制电路原样复制、重复导入生成不同副本、符号链接/目录联接拒绝、
  原版测试源 464 个文件 SHA-256 一致。

## 0.2.0 — 原生插件运行时

- 原生 DLL 加载、`resolve_symbol`、Hook 生命周期（创建禁用、成功后才启用、失败移除）。
- `sim_do` 目标周期拦截示例 `example.cycle-guard.mod`：把连续运行限制为当前周期 + N。
- 支持使用游戏自身 ImGui 绘制插件面板；`on_frame` 每 UI 帧一次。
- 自动验证：25 项包管理集成测试、6 项安装器测试、原生 Hook 正常/篡改/冲突/停用测试。
- 实机：加载独立 DLL、安装 `sim_do` Hook、绘制中文面板、向真实仿真内核提交目标周期并
  读取周期达到 99（SELFTEST）。

## 0.1.0 — 资源 Mod 与 Mods 管理页

- `.mod` 资源包（`format: 1`）：`files/` 文件替换/新增、精确文本补丁、停用恢复、
  大小写与路径冲突检查、事务回滚。
- 主菜单 Mods 入口与中文管理页：启用/应用/停用，区分“下次启动启用”与“本次运行中”。
- 安装器：安装、升级、卸载精确恢复、不兼容 EXE/引擎拒绝。

## 未发布内容说明

- 版本号、安装器与分发包仍为 0.4.0；0.4.1–0.4.6 的功能在源码树中可用，
  需要用 `build.ps1` 与 `package.ps1` 重新打包后才会进入玩家分发包。
- `sdk/tc_custom_logic.h`（整板解释器）在 0.4.1 起标记弃用，仅保留给研究探针。
- 路线决定：手写定义并登记版本 2 回调的元件，其内部电路只用一层占位门（每个输出脚一个可识别门、
  操作数即元件输入脚）。内部中间层不在加载器范围内，计划以后作为独立 Mod 提供；
  纯电路封装元件不受此限，多级内部电路照常由游戏展开。
