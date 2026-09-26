# 版本历史

版本号只有一个来源：仓库根目录的 `VERSION`。`build.ps1` 由它生成 `src/version.hpp`，
加载器横幅与日志、安装器标题、`tcmod-cli --version`、分发包文件名全部读这一处
（此前 0.4.0 硬编码在五处，而源码里的界面 API 注释写的是 0.5.0/0.6.0，编号已经对不上）。
当前版本 **0.8.0**（2026-09-27）：0.4.0 之后积攒的全部工作在这一版一次性打包发布。

## 0.8.0（2026-09-27）

### 发布阻断项

* **游戏更新不再让游戏起不来。** 存档重定向（`src/save_boot.hpp`）原先在 `DllMain` 里校验失败
  就 `return FALSE`，而加载器本身就是 `game_engine.dll`——于是"游戏更新"会直接变成"游戏完全无法
  启动，只能先卸载加载器"，连 Shift 安全模式和 `tcmod-cli disable-all` 都救不了（它们都要求加载器
  已经装上）。现在重定向失败只是**不重定向**：
  - 游戏照常启动，玩家自己的存档路径保持原样；
  - 本次运行**不加载任何原生插件**（避免 Mod 写进玩家的真实存档），资源 Mod 照旧生效；
  - `loader.log` 写明原因，并**弹一次**原生对话框（按"加载器版本 + 游戏哈希"记住，每个新组合只提示
    一次，同时写 `tc-modloader-data/unsupported-build.txt` 供排查）。用对话框而不是游戏内页面，是
    因为那种情况下加载器知道的 RVA 已经不可信，游戏内页面可能根本不会被画出来（实测：不匹配时主菜单
    连 Mods 按钮都不会出现）。
  - 判定抽成纯函数 `tc_save_boot::patchable(...)`，由新用例 `tests/save-boot.cpp` 钉住"任何无法验证
    的字面量都拒绝打补丁，而不是让加载失败"。
* **版本与打包**：`VERSION` 升到 **0.8.0**；重新跑发布流程（确定性源码包/玩家包 + `release.json` +
  两层 `SHA256SUMS.txt`）并发布到 GitHub。

### 本版主要能力（0.4.0 之后的累积）

* **元件外观**：`tc.component.render` V5（元件栏卡片 + 抽屉预览图由 Mod 提供）与 V6（用同一个绘制
  回调接管放置幽灵，结束信号取自游戏自己的剪贴板记录生命周期）；
* **元件体系**：声明式 C++ 元件（`register_component`）、原生逻辑的形状/位宽扩展、每实例配置持久化
  （写进元件记录自己的表，随原理图存档）与 schema 迁移、引脚顺序/命名；
* **插件界面**：主菜单页面、嵌进游戏布局的面板插槽、纹理上传、类型化控件与绘制原语；
* **仿真**：状态读取、逐周期捕获、IO 值读写、波形导出、事务与命令总线；
* **存档管理页**（主菜单 `Mods` 之下的「存档」）：副本列表 / 新建 / 导入原版 / 复制 / 重命名 /
  删除到回收站 / 切换 / 打开目录，加 **Mod 依赖清单**（登记表缓存 + 电路扫描，列出"需要哪些 Mod、
  缺哪些、版本或 schema 不符"），以及 2026-09-27 的卡顿修复（不再每帧扫盘/读 ini，扫描改单趟窗口
  匹配并把耗时写进日志）；
* 其它：能力协商与依赖版本约束、事件总线、符号别名与钩子链、安全模式、故障日志。

下面按时间倒序保留每项的细节（0.4.1 起这些条目此前都还没有单独发包）。

## 0.4.1–0.7.x 累积：浮点元件族 M2 与后续调研（2026-09-25 起）

### 存档管理独立成页：主菜单 `Mods` 之下的「存档」（2026-09-26 夜，本轮交付）

* **卡顿修复（2026-09-27，玩家报"感觉有点卡顿"）**：页面最初把那两件事放在了**每帧**做——扫一遍
  所有副本（实战副本 139 个电路文件）、`saves.ini` 读两次（`GetPrivateProfileString` 是文件 I/O）、
  `trash-*` 再扫一遍；选中一行还会重扫该副本并重新解析所有 `.mod` 包。现在：
  - 列表只在**打开页面 / 点「刷新」/ 副本目录本身变了**时重建（每帧只做一次 `last_write_time`），
    实测刷新 139×2 个文件的副本从**每帧**降到**一次 15–47 ms**；
  - 依赖报告**每次会话每个副本只扫一次**（打开页面时只自动扫当前选中的那份，其它行点「扫描」），
    并且不再重扫 Mod 包（用内存里已有的 `core->mods`）；
  - 扫描本身从"27 个 id 各扫一遍全文"改成**单趟 8 字节窗口 + 哈希表命中**，Snappy 解压的 copy
    段也从逐字节 `push_back` 改成按块 `insert`；
  - 每次刷新/扫描的时间都写进 `loader.log`（`Save profiles refreshed: N profiles, X ms`、
    `Dependency scan <id>: N circuits, X ms`），下次再有人说卡可以直接看数字。
  实测（同一台机、同一份 139 电路副本）：报告扫描约 **140 ms**（一次性），页面静止时每帧开销≈0。

存档管理从 Mod 页里那段折叠内容搬出来，成为加载器自己在主菜单上的**第二个入口**（`Mods` 正下方，
插件注册的页面整体下移一格）。页面每次打开都重新扫一遍 profiles 目录，加载器自己不存列表状态，
所以显示的和磁盘上的一致。

* **列表**：每行一个副本——名字、占用大小、文件数、最后修改时间，以及「本次运行 / 下次启动 /
  导入副本」标记；`trash-*` 不计入列表，只在标题里报数量。
* **操作**：新建空副本（`local-<随机>`）、导入原版存档（`import-<随机>`，逐文件校验）、切换到
  这一份（写 `saves.ini`，重启生效）、复制副本（`copy-<随机>`，同样逐文件校验）、重命名（限制
  `a-z 0-9 -`）、删除到回收站（`trash-<原名>-<时间戳>`，当前副本拒绝删除，改回名字即恢复）、
  打开文件夹。
* **本次运行 vs 下次启动**：进程在启动时就把游戏自己的存档路径常量重定向到选定副本，所以页面上
  改的选择只影响下一次启动——和 Mod 的「应用更改」同一套语义，页面上也照这个措辞显示。
* **开发用**：`TC_MODLOADER_OPEN_SAVES=1` 免点击打开这一页（配合 `TC_MODLOADER_SHOT` 出图），
  两个入口的按钮矩形也都进日志（`Saved` 与 `Saves`），供 playtest 点击。
* **Mod 依赖（③，2026-09-27）**：详情区第一行给结论，下面是明细。
  - **电路里只有 id**：每个自定义元件在存档里只留 8 字节 `custom_id`（作者自选的标记，如
    `F32ABS_1`），没有 mod id。所以加载器在**元件注册时**把 `id → {Mod id、类型名、config schema、
    包 SHA、注册日期}` 记进 `tc-modloader-data/registry.json`——这是"Mod 卸掉之后仍能说出这是谁的
    类型"的唯一来源。
  - **扫描**：逐个解压副本里的 `circuit.data`（首字节版本 + 原始 Snappy 块，`src/snappy_raw.hpp`），
    先精确搜登记表里已知的 id，再按"8 个可打印字节、且要么后面 32 字节内出现加载器自己的 `TCM3`
    配置标记、要么同一文件里出现两次"挑出未知类型标记。第二条是启发式（玩家存档是格式 v15/v16，
    组件记录无法逐字段走），报告里如实标注。
  - **结论**：装好并启用绿、已装未启用黄、缺失红、版本不同单独标（对照登记时记下的版本）；
    未知类型只显示作者标记。`tcmod-cli <游戏目录> saves report [副本]` 输出同一份数据，
    用例 `save-isolation` 用手工构造的 Snappy 电路 + registry 断言了这两条路径。

### 放置幽灵绘制接管：`tc.component.render` V6（2026-09-26，本轮交付）

* **SDK**：新增 `TC_COMPONENT_RENDER_API_VERSION_6` / `TCComponentRenderApiV6` 与
  `tableV6()` / `setPlacementPreview()`。按类型启用后，已有绘制回调会收到 `instance_id == 0`、零组件
  句柄和空配置的预览帧，Mod 用类型默认值画本体、文字和引脚。
* **加载器**：接管 `redraw_clipboard_component`，从游戏自己的临时记录读取 custom id、吸附格点与旋转，
  抑制默认的“仅引脚/名字”幽灵；捕获结果每帧重复提交，避免即时绘制只闪一帧。结束放置的信号取自
  **游戏自己的剪贴板记录生命周期**：`update_state_clipboard__…u120` 每次剪贴板状态变化都被调用，并拿到
  它正在处理的记录——kind `0x4e` + 同一个 custom id 表示这次放置还活着，空记录或换了一个元件表示游戏
  已经把剪贴板放下（落下、取消、换成别的选择）。`hide_clipboard` 钩子保留为兜底。
* **为什么不是别的信号**：`is_clipboard_visible__…u2911` 看着像“幽灵是否可见”，实测却不是——按住元件
  拖动的整个过程里它是 **0**（游戏自己都走“不可见”分支；接受这条信号会让预览一帧都不画），而红绘路径
  只在游戏继续重绘时才被调用，成功放置后它不再调用、`hide_clipboard` 也不调用，于是只挂这两个钩子的
  版本把幽灵留在落点：玩家报的第一个 V6 bug（`Body=424`，证据见
  [verification.md](verification.md) 的「元件放置幽灵 V6」）。
* **Float Ops**：22 种类型全部选择 V6，继续复用棋盘上的同一套紫色外观；预览哨兵 id 不进入实例命中缓存。
* **真机验收**：`tests/picture-playtest.ps1` 从“幽灵无本体”的旧断言改为检测拖放点附近的紫色本体和红色
  引脚。连续四帧都出现完整 `ABS |x|`，无论 V5 图片注册开关如何；幽灵区域的 V5 品红标记始终为 0。
  松开鼠标后再拍一帧（`stage after`）断言落点附近紫色为 0；往棋盘上**真的放一个元件**再拍
  `stage placed`/`stage settled`，断言元件确实落在落点上、被取消的那次拖放留下的格子仍然干净，并用
  加载器自己的 `taken over`/`ended` 计数断言每一次接管都结束。用只挂 `hide_clipboard` 的上一版加载器
  跑同一脚本，两条运行都在 `after` 一帧留下 `Body=424`（失败）。
  探针同时修正了浮点目录按钮边界点击、抽屉图片误覆盖元件栏取件坐标，以及放置落点读回系统光标这三处
  偶发项。

### 元件栏卡片与抽屉预览的图片接管：`tc.component.render` V5（2026-09-26，本轮交付）

玩家要的是"元件在元件栏里就长成棋盘上的样子"。上一轮把机制测清楚了（研究文档
`docs/research/component-icons.md`：图片是游戏自己渲染的，往游戏请求的路径上放文件没用，必须接住
那次**纹理请求**），这一轮把它做成能力并接到真机上：

* **SDK**：`TC_COMPONENT_RENDER_API_VERSION_5`/`TCComponentRenderApiV5` 的
  `set_picture(context, custom_id, png_path)`（`sdk/tc_service_api.h`），
  `sdk/tc_component_render.h` 加 `tableV5()`/`setPicture()`。路径必须是绝对路径；传 `NULL`、
  或者游戏真要这张图时文件已经不在了，就回退到游戏自己的渲染。
* **加载器**：`queryComponentRenderService` 增加 V5 分支（`component_render_set_picture_api`，按
  `componentIds` 校验归属，和 `set_foundry_button` 同一套写法），并新增 `armPictureControl()`：
  按住游戏纹理工厂 `create_texture_unsafe__…u364` 与它的检查版包装 `create_texture__…u1978`。
  命中 `?snapshot_cc/com_custom_<十进制 id>.png` 且该 id 有图时，用**加载器自己那份副本**的路径去调
  原工厂，其余请求（内置 `?snapshot/<kind>.png`、精灵、菜单、字体、游戏自己的缓存）原样转发。
  副本放在 `<game>/tc-modloader-data/pictures/<id>.png`：游戏读完会把重编码结果写回**它读的那个
  路径**，指给 Mod 包里的文件会让加载器的文件台账对不上哈希。规则本身抽成 `src/picture_path.hpp`
  （离线用例 `tests/picture-path.cpp` 覆盖正例与 `?snapshot/…`、缺数字、多后缀、超 64 位等近似反例）。
* **Mod 侧**（`examples/float-ops`）：**用元件外观当缩略图**——新增 `examples/float-ops/icons.cpp`，
  它 `#include "components.cpp"`，把**同一套绘制代码与同一张表**跑在一张"会画像素的绘制表"上
  （GDI+ 画本体与文字、存 PNG），生成 22 张 192×192 的透明 PNG：紫色本体、`32`
  角标、名字、中间符号与引脚都在棋盘那套位置上（一格 26 px）。构建脚本把它们随包放进
  `files/asset/float-ops-icons/<十进制 id>.png`，插件加载时逐个 `set_picture`。
* **真机验收**（`tests/picture-playtest.ps1`，用例 id `component-picture`）：卡片与抽屉预览两面在
  "注册了标记图"的一次里出现标记色、在"关掉注册"的对照里一处都没有；离线用例
  `tests/float-icons.ps1` 直接检查随包那 22 张图（透明画布、本体紫色宽 128 px、引脚触到 ±3.2 格、
  有白字），把"图是元件外观而不是占位图"钉在数据上。
* **要看清楚的一件事**：V5 的真实覆盖面是**元件栏卡片 + 底部抽屉预览**两个面。实测（随包真图、
  三种尺寸的标记图、以及完全不注册）游戏默认幽灵都只有引脚、名字标签和连线预览；这条历史测量后来
  促成了上面的 V6 专用接管。研究文档 §5 同时记了两个图片路径的坑（游戏重写它读过的 PNG、Nim
  字符串必须活过调用）。

### 研究：自定义元件的"图标"从哪来、能不能接管（2026-09-26，本轮实测）

这一轮没有改行为，只把"接管游戏自定义元件的图标绘制"这件事测清楚（完整证据与复现步骤见
`docs/research/component-icons.md`；探针 `tests/icon-probe.cpp`，用例目录 id `component-icon-research`）：

* **那张小图是游戏自己渲染的**。元件栏每一项先向渲染器要一张纹理：
  `get_captured_path__presenterZio_u28` 按 kind 分派（`0x4e` = 自定义原型 → `u25`，其余 → `u15`），
  自定义原型拼出的路径是 `?snapshot_cc/com_custom_<十进制 id>.png`（内置元件是
  `?snapshot/<kind 名>.png`），`get_asset_path` 再把它落到
  `<游戏目录>/asset/?snapshot_cc/com_custom_<id>.png`；请求行逐帧出现（FP32 Constant =
  `com_custom_5058442071141929777.png`）。
* **放 PNG 不能接管**：把一张品红色 64×64 PNG 放到那条路径上（`asset/snapshot_cc/`，另有
  `asset/capture/` 的绿色作对照），游戏**确实读了它**（运行后被重新编码 1060 → 642 字节，像素不变），
  但"放文件"与"完全不放文件"两次运行在元件栏区域逐像素比较 **0 差异**——画出来的是游戏自己的
  设计图缩略图渲染（`render_component_snapshots__main_u414`、`update_dirty_snapshots__main_u873`、
  `is_snapshot_mode__presenterZrendererZmulti95meshZmulti95mesh_u270`）。
* **`shape_svg` 也不能**（对自定义原型）。内置元件的目录缩略图来自原型 `+0xb0` 的 `shape_svg`
  （探针 dump：`<path d="M -12 -18 L -12 18 L 33 0 Z" class="component"/>`、
  `<rect x="-50" y="-30" width="100" height="60" class="component" />`，坐标是本体局部坐标），
  加载器也已把它转发给游戏（`src/native_component.hpp` 的 `setPrototypeShapeSvg`），但给本 Mod 的
  常量加上特征 SVG 后元件栏同样是 0 差异——与 `docs/PLAN-custom-components.md` 早先记下的
  "`shape_svg` 无影响"一致；`set_default_drawing` 只关棋盘上的默认绘制，不关图标。
* **结论**：接管必须在渲染层加能力，两条待做的路线写在研究文档里（A：拦
  `create_texture__u1978`/`create_texture_unsafe__u364` 对 `?snapshot_cc/com_custom_<本 Mod id>.png`
  的请求，返回 Mod 自建纹理；B：接管 `component_snapshot` 的快照渲染）。
* 追加测量（同一天，玩家要求"三个面都要"）：设计图缓冲区 `+0x570` 那条 Nim seq 的**安全写法**已经验证
  可行——`getCustomPrototype` 克隆 → 只写 `buffer+8` 起的 512 字节格子（8 字节 seq 头不动，
  上一轮崩溃就是涂了它）→ `setCustomPrototype` 回写 → `releasePrototype`，游戏不崩、**棋盘画面确实
  跟着变**（第 9 帧棋盘区 244 个采样点不同）；但**元件栏卡片区 0 像素差异**，所以"只改设计图"覆盖不了
  三个面。后续逐面复测修正了当时的假设：只有元件栏卡片与抽屉预览消费
  `?snapshot_cc/com_custom_<id>.png`；放置幽灵走独立 clipboard redraw 路径，现由 V6 接管。

顺带两条操作经验记进了文档：元件栏是**悬停逐层展开**的（分类页签 → 子分类飞出层 → 元件飞出层），
而且 1200×800 的小窗口下游戏**不画**元件列表，所以该用例默认用玩家自己的窗口尺寸 2536×1452。

### 修：退出关卡再重进后常量回到默认值、底部面板不再回读配置（2026-09-26，玩家第六次反馈）

玩家这三句话是一串同一件事："1. 常量的值还得我点一下才会刷新；2. 进入沙盒后退出再重进，会发现选中元件之后
面板的相关配置不会渲染；3. 同 2 重进的时候，常量值输出变为默认"。用隔离存档 + 真机驱动
`tests/float-reentry-driver.cpp`（进关卡 → 点常量本体 → 点"常量值"输入框 → 输入 → 回车 → Esc 退出 →
再按关卡入口 → **只选元件，不碰任何输入框**）走完一遍，日志给出三个各自独立的根因：

1. **加载器的电路存档写错了目录**（本轮最关键的一个）。`TC_COMMAND_SAVE` 的 `saveCurrentCircuit` 用
   **关卡名**拼路径：`schematics/<关卡名>/Default/circuit.data`；游戏自己读写的是关卡 meta 里的
   **kind 目录**（`campaign/<关卡名>/meta.txt` 的 `kind = …`）。关卡 "The Sandbox"（目录
   `campaign/sandbox`）的 kind 是 `architecture`，它的电路一直在
   `schematics/architecture/Default/circuit.data`；于是 Mod 改好的配置被写到
   `schematics/sandbox/Default/circuit.data`——一个游戏永远不读的目录——退出再进自然读到旧文件，
   常量就"变回默认值"。测量方式：把两份不同的夹具分别放进两个候选目录，游戏只会加载 kind 的那一份
   （两次真机测量，另一份 1887 字节的目录夹具完全没被碰过）。修法：新增 `levelSchematicKind()` 读关卡
   meta 的 kind，存档路径改用 kind 目录，日志写明
   `(level "sandbox" keeps its board in the "architecture" schematic the level's own meta names as its kind)`；
   没有 meta 的关卡仍退回原行为。
2. **配置恢复与棋盘刷新必须由关卡生命周期驱动，不能等玩家点面板**。加载器现在在 `level.load` 就记住
   仿真模型（`tc::sim_control::Store::remember`），并在每一帧给"已绑定但还没探到尾部记录"的实例补做一次
   配置恢复（`tc::logic::restorePendingTailConfigs`）。否则暂停中的棋盘在这两个时刻根本没有可用的仿真模型，
   Mod 请求的刷新只会拿到 `ERR_STATE(-5)`（日志行 `float-ops: the pending board refresh expired (status -5)`），
   玩家看到的就是"必须点一下才刷新"，重进时就是"常量还是默认值"。
3. **面板的行只跟着"实例 id"走**。重进关卡后元件 id 不变（游戏按记录发 id），底部抽屉于是继续显示旧棋盘
   的内容，甚至不回读记录。Mod 侧改成：每帧看一次 Board 句柄，换棋盘就丢掉编辑器状态、渲染盒子与值缓存
   （`serviceBoardChange`，日志 `float-ops: board entered (handle 3/2) …`；离开时
   `float-ops: no board is up; the editor rows and the board caches were dropped`）；面板每次绘制都比对
   "行是按哪份记录填的"（`readConfigSnapshot`/`recordMatchesRows`），记录变了（恢复、撤销、克隆、换关卡）
   就重读，正在编辑的输入框不打断；渲染盒还没画过时类型改问宿主的实例表（`typeOfLiveInstance`），
   所以刚进关卡也能立刻显示配置。新增证据行
   `float-ops: the drawer is editing instance 0x… (type 0x…, label "…", value "12.5")`：面板里的值第一次
   可以直接从日志读出来。

真机门禁：`tests/float-reentry-playtest.ps1`（目录 id `float-reentry-game`）。断言写入到达实例记录、电路保存
成功、第二次进入时**没有任何点击**就先恢复出上一次的值（显示文本与位模式与第一访问结尾一致）、抽屉确实重画
并回读了记录（面板文件帧号更大、`value` 行在、字段文本等于保存值）。**红绿都实测**：用上一轮的 loader+Mod
（`-Loader` / `-Mod` 指向备份）跑，编辑被拒、没有电路保存、抽屉不回读 → 用例失败；换这一轮的构建 → 通过。
相邻用例（面板输入 `float-panel-game`、M2 垂直切片 `float-m2-game`、目录 19 型 `float-catalogue-game`）
复跑通过。

顺带两处与本次调查直接相关的收尾：`abi/windows-x64.json` 补上本轮 SDK 里
`TCUiSlotDefinition`（组件抽屉槽新增回调 → 48 → 56 字节）的记录，`sdk-abi` 快速用例由红转绿；
`docs/sdk/simulation.md`、`docs/sdk/commands.md`、`docs/verification.md` 里"电路路径按关卡名"
的说法按上面的真机测量更正为"按关卡 kind"。

### 修：四个多脚元件的占位与本体不重合（2026-09-26，玩家第五次反馈 + 加载器 geometry V3）

玩家反馈："其它元件都好了这四个还是有点问题"，指的是 FMA（3 入 2 出）、CMP（2 入 4 出）、
SPLIT（1 入 3 出）、MAKE（3 入 1 出）。量图 + 真机测出来这两件事：

- **游戏把生成的引脚从第 0 行往下排**（入口脚按自己的数量居中，出口脚从 0 开始），所以一边引脚更多的
  元件，本体落在自己那格的**下方**：CMP 的本体覆盖 -0.33…3.33 格（4 输出跨 0…3 行），SPLIT 是
  -0.6…2.6（3 输出跨 0…2）。而 `set_footprint(半宽, 半高)` **只能声明居中的框**，宿主又把半高
  **向外取整**，于是要么在本体上方白留一整格、要么漏掉本体下沿——两个症状玩家都看得见：元件之间留缝，
  以及**可拖区域悬在本体上方的空白里**（矩形就是游戏命中/拖动/占位用的框）。
- **本 Mod 之前按"引脚跨度 + 0.6 余量"长高本体**，2.93 格的面被撑成 3.2/4.17 格，而整格占位只能
  取整到 4/5 格——比本体高 0.8 格，正是玩家看到的缝。

修法分两层：

1. **加载器新增 `tc.component.geometry` V3**：V3 保留 V1/V2 完整前缀，加一条
   `set_footprint_cells(custom_id, x, y, width, height)`，按**整格**声明矩形，宿主**原样存储、
   不取整**（`sdk/tc_service_api.h`、`sdk/tc_component_geometry.h`、`src/native.hpp` 的
   `setComponentFootprintCells`）。给带偏移的本体一条正当通路，而不是继续用居中形式凑。
2. **元件侧改成本体与占位一起算**：本体只在需要时才长高（两侧都能塞进原版 2.93 格面时保持原版），
   并且**落在整格上**；占位框取"本体覆盖的格 ∪ 引脚所在的格"，用 V3 按格声明。结果：

   ```text
   measure float-ops subtract  vertical pitch=3 cell(s)   (2 入 2 出，原版面)  水平 7
   measure float-ops constant  vertical pitch=3 cell(s)   (M2)                水平 6
   measure float-ops fma       vertical pitch=3 cell(s)   ← 之前 4
   measure float-ops split bits vertical pitch=3 cell(s)  ← 之前 4
   measure float-ops make bits vertical pitch=3 cell(s)   ← 之前 4
   measure float-ops compare   vertical pitch=4 cell(s)   ← 之前 5（4 输出跨 0…3 行，本来就得多一格）
   ```

   四个元件里三个和别的元件一样能贴住，CMP 因为一边四根脚占 4 格（这也是最小可能值）。同时它们的
   可拖区域第一次和画出来的本体精确重合（CMP 的本体从 -0.33 到 3.33 格，占位框就是 0…3 格）。
   真机证据与断言在 `tests/float-pitch-playtest.ps1`（`float-pitch-game`）；加载器的
   `component_geometry` 单测加了 V3 前缀与带偏移矩形的往返，几何服务真机烟测（V1/V2）复跑通过，
   目录/面板/元件栏/拥挤棋盘/玩家存档五条真机用例也全部复跑通过。

### 修：垂直方向的占位比本体大，元件贴不到一起（2026-09-26，玩家第四次反馈）

玩家反馈："这些元件在垂直方向上 footprint 好像有点大了，比外观要大一点，导致现在这些元件相互之间
不能贴在一起，而是中间有间隔"，并附了一张自己排的 22 个元件截图。量图 + 真机实测：

- 从截图量：本体宽 129 px = 4.92 格（1 格 ≈ 26.2 px），本体高 76 px = 2.9 格，但每一行的
  中心间距是 103–110 px = **3.9–4.2 格**——即"占位比外观高一格"。
- 真机测地点间距的新探针 `tests/float-pitch-probe.cpp`（用棋盘自己的放置命令，把参考件放在某格、
  再逐格试第二件，每次测量单独占一条带，只记最小的可通过偏移）：**修复前** FP32 Subtract（两边各
  两脚、面 2.93 格高）垂直最小间距 = **4 格**，而同样面高的三个 M2 元件（Constant/Add/Display）
  是 **3 格**。
- 根因：`tc.component.geometry` 的半高是**向外取整**成整格的（`component_geometry::fromHalfExtents`
  的 `ceil(2 * halfHeight)`）。M2 三个元件声明固定 1.5 → 3 格；目录元件却按"本体半高 + 0.1 余量"
  声明：2.93 格的面 → 1.465 + 0.1 = 1.565 → `ceil(3.13)` = **4 格**。于是同样大小的面，目录元件
  比 M2 元件多占一格，元件之间就留出可见的空隙。
- 修法（`examples/float-ops/components.cpp`）：目录元件改为声明"本体自己的半高"（不再加 0.1），
  并与 1.5 的下限取大。于是 2.93 格的面 → 3 格（和 M2、和游戏自带的同尺寸元件一致），
  3.17 格的 Split Bits 仍是 4 格，4.17 格的 Compare 仍是 5 格（它们本来就更高，不能挤）。
- 验收：新用例 `tests/float-pitch-playtest.ps1`（目录 id `float-pitch-game`）跑真机并把间距写成断言：

  ```text
  measure float-ops subtract vertical pitch=3 cell(s)  ADJACENT-OK
  measure float-ops subtract horizontal pitch=7 cell(s)  ADJACENT-OK
  measure float-ops compare vertical pitch=5 cell(s)  ADJACENT-OK
  measure float-ops compare horizontal pitch=7 cell(s)  ADJACENT-OK
  measure float-ops constant vertical pitch=3 cell(s)  ADJACENT-OK
  measure float-ops constant horizontal pitch=6 cell(s)  ADJACENT-OK
  ```

  水平方向不变：5 格本体 + 每根伸到框外的脚各占 1 格（两侧都有脚的 7 格，只有输出脚的 6 格），
  这是游戏自己的规则（脚也算占位），游戏自带同尺寸元件同理。同类用例 `-Fixture crowd`、目录、
  面板、元件栏文件夹全部复跑通过。

### 修："所有能写配置的元件都写入不了"（2026-09-26，玩家第三次反馈）

玩家反馈："现在所有能写配置的元件都写入不了，都是写入元件配置失败"，并附上底部抽屉里
"显示方式"一行 + 下面一行灰字"写入元件配置失败"的截图。**根因不在本轮的分类改动**：

- 复现：把玩家自己的存档棋盘（`%APPDATA%\Turing Complete Mods\profiles\default\schematics\
  architecture\Default\circuit.data`，22 个原生元件）当作夹具跑 `tests/float-panel-playtest.ps1
  -Board <该文件>`，驱动照旧点输入框、打字、回车——日志里标签提交是 `(refused)`，
  说明写入通路整条没进去；
- 根因：宿主的 `tc.component.instances` 枚举是**全有或全无**——只有"全部实例都装进了调用方给的
  缓冲区"才返回 `TC_COMPONENT_INSTANCES_OK`，否则返回 `ERR_RANGE`（`src/native_logic.hpp` 的
  `instanceEnumerate`）。本 Mod 之前用固定 8 个 handle 的栈数组，一旦棋盘上的原生元件超过 8 个，
  枚举永远返回 `ERR_RANGE`，Mod 把它当成"这个实例不在棋盘上"，于是**每个**配置写入都失败；
  玩家棋盘从 4 个元件长到 22 个之后必然全挂（沙箱夹具只有 4 个实例，所以一直没暴露）。
  实测还发现实例集合会在编译绑定过程中**继续变大**：按第一次拿到的总数再枚举一次仍可能
  `ERR_RANGE`，所以要循环重试。
- 修法（`examples/float-ops/components.cpp`）：`instanceHandles()` 从 16 个 handle 起步，
  `ERR_RANGE` 时按宿主报的总数 +1 扩容重试（最多 5 次，和 `examples/clock` 同一个套路但多一个
  循环，因为集合会增长）；`writeConfig`/`currentConfig` 都走它，读不到实例时还会按 custom id
  再全表扫一次。失败路径现在**带原因**进日志（`no live handle for instance …` /
  `this write was not attempted because the host reports N instance(s) …` /
  `the host refused the configuration for instance …: <SDK 错误文本>`），下次不用再靠猜。
- 真机门禁：新增 `tests/float-fixture.cpp --crowd`（18 个不接线的 FP32 Display + 后面才是 M2 链，
  共 22 个原生元件）与用例 `tests/float-panel-playtest.ps1 -Fixture crowd`（目录 id
  `float-busy-board-game`）。**已实测这条用例能抓住这个 bug**：把 `instanceHandles` 改回固定 8 个
  handle、不重试，用例就在"`label "xy" (written)` 缺失"上失败；改回来后通过。玩家自己的那块
  22 元件棋盘也用它复现并确认修好（`-Board <玩家存档>` → 同一套断言通过）。

### 浮点元件真的进了游戏自己的分类体系（2026-09-26，路线 2）

玩家否掉了上一轮的"路线 1"（Mod 自己画一个"浮点元件"侧栏面板）："我希望分类文件夹是真的插进
游戏自己的分类体系"。这一轮把分类做进**游戏自己的元件菜单树**，Mod 不再画任何元件栏：

- **机制（实测，不是猜的）**：右侧那一列由游戏自己的 mini tree 绘制，树的来源是
  `get_component_menu__presenterZutilities_u9607` → `reload_component_menu__presenterZutilities_u14192`
  写进 presenter context 的 `+0xd948/+0xd950`。树里每个节点的**第 2 个字节低三位**就是变体标记
  （`2` = 分类、`1` = 自定义元件、`0` = 内置元件），分类节点的 `+8/+0x10` 是名字、`+0x20/+0x28`
  是子节点序列；而给自定义原型**分文件夹的代码就在游戏自己身上**：
  `add_to_menu_tree__presenterZutilities_u12208` 把原型的**名字按 `/` 切开**，逐段当成父分类走进树
  （这正是说明书里"文件夹结构决定自定义元件在菜单中的层级位置"的实现）。
- **做法**：Mod 注册元件时把名字写成 `浮点/FP32 Add`（`components.cpp` 的 `namePrefix`），于是
  **游戏自己**在"自定义"里建出一个 `浮点` 分类节点，并把 22 个浮点元件挂进它——分类节点是游戏
  分配、游戏绘制、游戏销毁的，Mod 不碰任何游戏内存，玩家存档也不受影响。`TC_FLOATOPS_PALETTE`
  可以覆盖这个前缀（自动化用例用它对比折叠/平铺两种菜单）。
- **删掉的东西**：`registerPalette` / `paletteDraw` / `placeType` / `freeSpot` / `palette-rows.txt`
  以及"点一下放到棋盘"的整条路线 1 通路全部删除（约 350 行），Mod 现在只有游戏自己的元件栏和
  游戏自己的底部抽屉（编辑行）。
- **验收**：`tests/float-palette-playtest.ps1` 重写——`tests/float-menu-probe.cpp` 钩住游戏自己的
  `reload_component_menu__presenterZutilities_u14192`，等它建完菜单后把整棵树（供 `build/menu.txt`）
  dump 出来，用例断言：游戏原有五个分类还在、`自定义` 里有一个 `浮点` 分类节点、这个节点的子树
  **恰好**是本 Mod 的 27 个元件 id（22 个可用元件 + 5 个 M0 探针）、而且这些 id 在树的其它任何
  地方都不出现（不是"既在文件夹里又散在自定义"）。

实测 dump（`build/menu.txt`，真机、真包）：

```text
node=… byte1=2 tag=2 name="自定义"
  children=1 payload=…
  node=… byte0=171 byte1=2 tag=2 name="浮点"
    children=23 payload=…
    node=… byte0=171 byte1=2 tag=2 name="M0"
      children=5 payload=…
      node=… byte0=0 byte1=1 tag=1 id=0x4633325041535331 name="浮点/M0/FP32 M0 Pass"
      …（五个 M0 兼容性探针）
    node=… byte0=0 byte1=1 tag=1 id=0x4633324144445f31 name="浮点/FP32 Add"
    …（22 个可用元件）
```

"自定义"页现在只剩这一个 `浮点` 文件夹，里面是 22 个可用元件加一个 `M0` 子文件夹（五个兼容性
探针——它们也是本 Mod 的元件，注册名同样带前缀，所以不再散在自定义根部）。

**已知边界**：`浮点` 文件夹在"自定义"分类页**里面**，不是和布尔/整型/杂项/输入输出并列的第六个
顶级页——钉住的这版游戏把自己的顶级分类写在数据里（本地化 + 分类枚举），而给自定义原型分层的
唯一通道就是上面那个"名字按 `/` 切开"的路径，它固定从"自定义"节点起步。要做成顶级页只能钩住
`reload_component_menu` 之后**动手改游戏自己的菜单树**（拿 `nimNewObj` 造节点、把节点从
`自定义` 的子序列搬到顶级序列），那是在玩家进程里改游戏内存，本轮没做，理由与可行的步骤记在
`docs/research/palette-categories.md`。

### 浮点元件有了自己的元件栏（2026-09-26，路线 1）

玩家要求："把浮点数 mod 的这些元件在游戏内右侧元件栏单独开一个文件夹……而不是都放到自定义元件
文件夹。" 本轮先把结构测清楚（`docs/research/palette-categories.md`，探针
`tests/palette-probe.cpp` + `tests/palette-probe-playtest.ps1`，真机只读，dump 在
`build/palette.txt`）：

- 右侧那一列是游戏**写死的分类页**（布尔/整型/杂项/输入输出/自定义），名字来自游戏自己的
  本地化条目（`# Component category name, at most 6 glyphs…`），不是从存档目录读出来的；
- 原型里 `+0x40` 的"类别"字节并不对应分类页（`NAND` 与 `Multiply` 同为 `3` 却分属两页），
  `+0x88/+0x90/+0x98` 是菜单排序键，`CATEGORY_ORDER` 是 `.rdata` 里 10 个 64 位条目；
- Mod 元件是加载器导入的原生 custom 原型（kind `0x4e`），没有对应的 schematic 文件，因此和
  玩家自制元件一样落在"自定义"页根部；游戏给玩家自制元件分层的机制是**工坊关卡的文件夹结构**
  （说明书原文：文件夹结构决定自定义元件在菜单中的层级位置）。

玩家随后要求"完成后自动关机"，并让按推荐路线做，于是这一轮交付了**路线 1**：

- **Mod 自己的"浮点元件"面板**（`examples/float-ops/components_ui.cpp` 的 `registerPalette` /
  `paletteDraw`）：用加载器已有的**电路板侧栏插槽**（`TC_UI_SLOT_BOARD_SIDE`，与 board-panel
  示例同一个容器，实测该容器里的控件真的收得到鼠标）列出本 Mod 的 22 个元件，一行一个按钮。
- **点一下就放到棋盘上**：点击行 → `TC_COMMAND_BOARD_PLACE_COMPONENT`（加载器的命令总线，也就是
  游戏自己的放置通路，Ctrl+Z 能撤销）。落点由 Mod 自己找空位：先读棋盘上现有元件的位置（记录里
  `+2/+4` 的格子坐标），取平均位置作锚点，再用环形搜索找一个**留出本体占地**的空格——实测紧挨着
  已有元件放会被游戏拒（命令结果 `-6`），所以搜索按 ±6/±3 格的净空判断。
- **自带证据**：放置请求提交后 Mod 会读回命令状态并把棋盘上真的多出来的实例写进日志
  （`float-ops palette: placement command state=3 result=0`、
  `float-ops palette: placement confirmed type=0x… at (-4,5)`）。
- **验收**：`tests/float-palette-playtest.ps1`（真机、真包）：面板注册并画出（加载器日志
  `Board panel local.float-ops/palette …`）、列出 22 行、放置成功且棋盘上确认到新实例。**面板按钮
  的真实鼠标点击**由既有用例 `tests/ui-board-panel-playtest.ps1` 覆盖（同一个侧栏容器里的按钮，
  真实鼠标消息），本次面板用的就是那条通路。

仍未做（写进 `docs/research/palette-categories.md`）：把分类**插进游戏自己那 5 个分类页**（需要钩
`build__…Zmini95tree` / `context+0xd948` 的入口序列并复刻游戏自己的缩略图与拖放），以及按游戏原生
方式把元件写成玩家存档里的工坊 schematic（`PLAN-float-components.md` §8.3 早已否决）。

### M3 + M4：把二进制32的元件目录做完（2026-09-26）

玩家要求："把浮点数mod完整做完，也就是完成剩下的各个阶段。元件外观依旧和目前几个元件保持一样的
格式，如果有属性选择的元件也是用底部面板。" 交付如下：

- **19 个新元件**（`examples/float-ops/components.cpp` 的 `kCatalogue` 一张表驱动注册、绘制与面板）：
  - M3 基础包：`FP32 Subtract` / `Multiply` / `Divide` / `Square Root` / `Negate` / `Absolute` /
    `Compare` / `Classify`；
  - M4 扩展：`FP32 Fused Multiply Add`（三输入单次舍入）、`FP32 Remainder`、
    `FP32 Round To Integral`、`FP32 MinimumNumber`、`FP32 MaximumNumber`、
    `I32 To FP32` / `U32 To FP32` / `FP32 To I32` / `FP32 To U32`、
    `FP32 Split Bits` / `FP32 Make Bits`；
  - 命名统一 `FP32 …`，面板与棋盘上的短名是 `SUB/MUL/DIV/SQRT/NEG/ABS/CMP/CLS/FMA/REM/RND/MIN/MAX/
    I2F/U2F/F2I/F2U/SPLIT/MAKE`。
- **外观与现有元件完全同格式**：本体仍是原版 4.92 格宽、左上 `32` 位宽框、名字右上角、操作符居中、
  引脚在 3.0 通道（本体外 0.54 格）。只有一列引脚超过两行时才长高（Compare 有 4 个输出、Split 有
  3 个），并按引脚中线居中——否则最外的引脚会悬在本体外面。`docs/sdk/ui.md` 与 changelog 同款记录。
- **有属性的元件全在底部面板改**：受舍入模式影响的 10 个元件（除/加/减/乘/开方/FMA/取整/两个整数
  输入转换/两个整数输出转换）在面板里有五个 `RNE/RNA/RTZ/RDN/RUP` 单选框，点了立刻写回该实例的配置
  并刷新棋盘；不受影响的元件（Negate/Absolute/Compare/Classify/Remainder/Min/Max/Split/Make）显示
  一行说明（位序、饱和策略、引脚含义），不做无效的舍入配置。**没有任何弹出窗口。**
- **转换语义按 D2 拍板**：NaN/±Inf/越界置 NV 并饱和（`INT32_MAX`/`INT32_MIN`/`UINT32_MAX`/`0`），
  `-0` 转 0 不置 NV，负的非零值转无符号按越界处理；`i32_to_f32`/`ui32_to_f32` 走实例的舍入模式，
  不能精确表示时置 NX。内核新增 `fp32_from_i32/from_u32/to_i32/to_u32`，SoftFloat 子集随之扩到
  25 个编译单元（`fp/softfloat-sources.txt`，由 `.softfloat-work/closure.py` 重新走符号闭包得到）。
- **验收**（见 `docs/verification.md` 的 M3/M4 小节）：
  - 离线：内核 7571 条黄金向量 + 新增转换策略用例全过；`tests/float-compat.cpp` 断言 19 个新元件
    各自的引脚、宽度、共享配置布局（schema 3）、3.0 引脚通道、callback 的实际输出与 schema-2 迁移；
  - 真机：`tests/float-catalogue-playtest.ps1` 用夹具（19 个元件各一个实例，`float-fixture.exe
    --catalogue`）在真游戏里断言每个元件都被画出来、本体宽度仍是原版 4.92 格（多引脚类型更高）、
    并读出面板行（受舍入影响的类型出现 `rounding` 行、Compare 出现 `info` 行）。

### 面板里的输入框真的能用了；弹出的小编辑器取消（2026-09-26）

玩家反馈与要求（原话）："浮点数元件选中后底部面板的输入框、勾选框用不了"、"这个多出来的额外框我
不需要"。两件事一起解决：

- **根因**（真机实测，`tests/float-panel-playtest.ps1` 逐帧日志）：游戏自己的底部抽屉窗口**不会**
  把鼠标交给插件画在它里面的 item——鼠标停在插件画的那一行上时，抽屉自己报
  `IsWindowHovered()==0`（同一位置的 `panelHovered=0`），因为鼠标底下是抽屉内部的另一个子窗口；
  `inputText`/`radioButton` 因此画得出来、点不进去。这解释了"输入框点了没反应"，也解释了为什么
  游戏自己的字段没事（它们画在那个子窗口里）。
- **做法**：三个元件的编辑行改在**Mod 自己的一个窗口**里画，尺寸无背景、位置正好盖在面板那几行
  上（`examples/float-ops/components_ui.cpp` 的 `kRowWindowFlags`），每帧 `setNextWindowFocus()`
  让它压在游戏窗口之上——和加载器页面容器同一个调用、同一个原因。行还是原版的样子（左标签、
  右字段），字段是**真的控件**：标签与常量值是 `inputText`（离开字段即提交，和原版面板一致），
  舍入模式是五个 `radioButton`，显示方式是四个 `radioButton`，点了立刻写回配置并请求刷新。
- **弹出的小编辑器整个删掉**：`openEditor`/`drawEditorWindow`/确定/取消/`###TCFloatOps*` 窗口
  都没了，选中元件后不会再有任何窗口盖在棋盘上。`PanelRow.live`（这一行的控件是否被 ImGui
  hover 到）与逐帧 trace 保留，作为"这个构建到底有没有把输入交给插件"的证据。
- **面板几何靠实测而非假设**：行窗口的位置由面板内容原点（`igGetCursorScreenPos` −
  `igGetCursorPosX/Y`）加固定比例算出，换窗口大小/分辨率跟着走；Mod 把每行的行盒与字段盒写进
  `plugin-data/local.float-ops/panel-rows.txt`，自动化用例按这个文件点击，不猜坐标。
- **验收**：`tests/float-panel-playtest.ps1`（真机、真鼠标、真键盘、真存档副本）：驱动用棋盘自己的
  `select_component` 选中 FP32 Constant → 真实鼠标点面板里的标签输入框 → 真实按键输入 `xy` +
  Enter → 日志出现 `float-ops: the label field was committed after editing` 与
  `float-ops: instance 0x… label "xy" (written)`，棋盘上元件名字变成 `name="xy"`，并且**没有任何
  窗口打开**（用例显式断言）。

### 外观这一轮（2026-09-25，晚）：像原版元件的排版 + 引脚挪到本体外

- **元件编辑改在游戏的底部元件面板里**（玩家要求"和原版一样：标签、数值都在面板里改"）：
  - 加载器新增插槽种类 `TC_UI_SLOT_BOARD_COMPONENT_PANEL`（`sdk/tc_mod_api.h`、`sdk/tc_ui.h`
    的 `registerComponentPanel`）。加载器挂钩游戏自己的元件抽屉（`build_toggle_button`，
    见 `src/symbol_profile.hpp`），让游戏先排完自己的标题/说明/引脚图，再把**选中元件的所有者
    Mod** 的回调画进去（带 PushID(mod)/PushID(slot)，并把手上的实例 id 一起传过去）。
    之所以钩标题行而不是抽屉根：抽屉根返回时窗口已经关掉，在它后面画会落到别的窗口；
    之所以不是 punch-tape 用的说明面板：那个构造器只对"有说明面板"的种类运行。
  - 浮点族因此把编辑器从"点元件弹出自己的小窗"改成面板里的两行：`标签` + `常量值`／`舍入模式`／
    `显示方式`（`examples/float-ops/components_ui.cpp`），点击元件不再弹任何窗口。
  - **面板里的行是"摘要"，点一下开小窗**：实测游戏那个窗口不会把插件画进去的控件交给 ImGui 的
    输入路径（行画出来了，点上去毫无反应），所以行只做显示，命中测试由 Mod 自己用引擎的鼠标状态
    做（和它原来打开编辑窗的方式相同），点中哪一行就弹哪一行的编辑窗——这正是游戏自己的模型：
    它的元件数值行也是"点一下弹一个小编辑器"。编辑窗是 Mod 自己的窗口（`tc::ui::panel`），
    输入框、单选框、Enter/确定/取消/Esc 都在那里生效。
  - 配置 schema 2 → 3：三个配置块各追加 `label[16]`（"标签就是显示在元件上的名字"）。
    空标签仍显示元件类型名（原版规则：没标签的 Constant 显示 CONST），有标签就显示标签；
    旧存档按 schema 2 迁移（字段照抄、标签留空）。`tests/float-ops-playtest.ps1` 里驱动给第一个
    常量写了标签 `left`，渲染回调上报 `name="left"` 作为回读证据。
- **编辑值之后棋盘会自己重算**（玩家报"常量设置成 1，实际输出的还是 3.1415925"）：游戏的编译器
  不知道自定义元件的配置变了（它只认识自己的原件），所以暂停中的板子会一直显示上一次算出来的
  结果——玩家看到的"要先刷新一下"就是这个。现在**元件自己**在提交配置之后发出玩家那个刷新请求
  （`tc.simulation` 的 `pause()` = `sim.do` command 1，走同一条钩子链，见
  `examples/float-ops/components.cpp` 的 `refreshBoardAfterEdit`），于是线上标签与下游 Display
  立刻跟着变。放在 Mod 而不是加载器里，是因为一次实测：加载器侧"按仿真是否在出周期"决定刷新时，
  要么每帧读 `sim.cycle`（会让 text-box 用例的相机平移检查失效），要么无条件刷新（会打断
  text-box 的平移）——两者都会影响别的 Mod；Mod 自己发请求则只作用于自己的元件。

- **生成电路的内部布局收进引脚矩形内**（`src/component_definition.hpp`）：玩家报"FP32 Add 右下角多了
  一块"，量出来那块正好落在旧依赖门的电路坐标 `(52,108)`——换算到板面是元件中心右 6.6 格、
  下 13.8 格。原因是游戏会把自定义元件的**内部电路**渲染成一张图（元件栏/工坊/预览都用它，
  游戏自带的 `asset/capture/com_custom_*.png` 就是这种图：内部逻辑 + 引脚点、透明底）。旧的
  依赖门摆在 `(40 + 12i, 100 + 8i)`，于是那张图里多出一块离家很远的方块。现在三列位置固定为
  `collector x=-6`、`dependency gate x=0`、`driver x=6`，门与门、门与整流器的接线重画，
  所有节点都落在**引脚自己的矩形**里（`tests/native-component.cpp` 新增不变量：
  两脚一侧与 16 脚的形状，逐个节点断言 x/y 不越界，并断言节点种类顺序不变）。
  引脚位置与引脚所在行**不变**，所以既有存档的连线不受影响；契约只固定节点顺序，位置本来就自由。

- **引脚通道改成按类型声明**（`TCComponentTypeDefinitionV2::pin_lane`，`sdk/tc_service_api.h`）：
  生成元件的脚由"设计坐标 ÷ 8 格"决定（实测 `-18 -> -2`、`13 -> +2`），声明 `pin_lane = 3.0`
  就落在 `±3`——原版 Constant/Static Value 的 kind 表条目正是 `out0=(3,0)`。默认 `2.0` 保持历史
  坐标（`-18 / +13`）逐字节不变，所以既有 Mod、存档连线与缩略图都不动；越界值直接拒绝注册。
  脚手架的输入/输出走线跟着引脚走（`inputPinX+3`、`outputPinX-3`），这是上一轮"改内部 IO 位置
  就把板子跑死"那个坑的正解：那次只挪了脚，没挪线。
- **绘制表 V2**（`TCComponentRenderDrawV2`，`sdk/tc_component_render.h` 的 `drawV2/textSized/
  measureText`）：按显式像素字号画字 + 量字，宿主只在那一次调用里换掉绘制列表共享数据的字体/字号，
  粗体面取游戏自己的 `NoroshiCode_Bold`。这是"板面文字跟着相机缩放"能对上原版的前提。
- **浮点族按原版排版重画**：本体 4.92 × 2.93 格（原版 Constant 的尺寸）、名字右对齐在右上角、
  位宽框在左上角、值/操作符居中；字号按"数字高 = 0.59 格（值）／0.40 格（名字与位宽）"反推请求
  字号（本构建实测：按请求的 0.79 倍绘制、数字是那一版的 0.66 倍，于是 0.59 格 = 请求
  `0.59 × 每格像素 ÷ 0.52`）。过长的值按本体宽度等比缩小；Display 是十进制一行（0.59 格）
  加位模式一行（0.40 格）。
- **真机外观验收**进 `tests/float-ops-playtest.ps1`：截图后按 Mod 自己上报的逐帧布局（`unit`、
  `body`、`pin0`）量像素，断言三件玩家看得见的事——本体是原版尺寸、名字在右上角且不压值、
  **引脚在本体外面 0.55 格**（原版 0.54）。实测：Constant/Add/Display 本体 4.92 × 2.93 格，
  值 0.59 格（Display 十进制行），引脚外移 0.55 格。

### 浮点元件族 M2（同日早些时候）

- **三个真元件**（`examples/float-ops/components*.{hpp,cpp}`）：`FP32 Constant`（0 入 1 出）、
  `FP32 Add`（A/B → R[32]+Flags[5]）、`FP32 Display`（1 入 0 出）。配置按元件保存，schema 2 的
  每个配置块都以 `format` 字节开头（当前唯一合法值 32），并带一条 schema 1（无 format 的开发期
  布局）迁移路径；写入走 `tc.component.storage`，有 V2 时用 begin/commit 包成一次撤销步。
- **十进制层**：vendored fast_float（正确舍入的解析）与 Ryu（最短往返位），包在 `fp/decimal.*` 后面；
  Constant 编辑器接受十进制、科学计数法、inf/-inf/nan/-0 与 `bits:0xXXXXXXXX`，解析的
  overflow/underflow/inexact 只作编辑器提示，不进元件的 Flags。黄金语料由
  `tools/float-decimal-vectors.py` 独立生成（93 条解析 + 383 条格式化），`tests/float-decimal.cpp`
  另跑 2 万条随机最短往返与缓冲区安全用例。
- **原版风格外观与交互**：沿用原版元件的 footprint 与引脚通道，左上角固定 `32` 位宽框（悬停提示
  FP16/FP64 未实现），其右侧舍入框用紧凑菜单列出五个模式并立即写回；中央操作符、A/B/R/F 标注；
  Constant 与 Display 点本体开小面板（文本编辑 / 显示方式）。C1 显示器的值由逻辑回调写进受锁缓存、
  渲染回调读取，暂停时照常刷新。
- **真机闭环**（`tests/float-ops-playtest.ps1`）：默认 1.0+1.0 显示 `2 0x40000000`；写入
  3.5/1.25 后 `4.75 0x40980000`；半 ULP 用例 1.0+2^-25 在 RNE 下 `1 0x3F800000`，把舍入框改成
  RUP 后 `1.0000001 0x3F800001` —— 配置、内核、显示三段都在游戏里被验证。
- 顺带钉死本构建的引脚换算（设计坐标 ÷ 8 = 板面格，默认通道 2.0 时两脚一侧为
  `in0=(-2,0) in1=(-2,1)`；声明 3.0 通道后为 `±3`），夹具与绘制都按实测对齐。
- 仍未做：点击路径的自动化（菜单/面板的真机点击用例）、Split/Make Bits、FMA/Remainder 元件（M4）、
  TestFloat 资格验证（M5）。

## 未发布 — 浮点元件族 M1：binary32 数值内核（2026-09-25）

- **内核落地**（`examples/float-ops/fp/`）：vendored Berkeley SoftFloat 3e（提交
  `a0c6494cdc11865811dec815d5c0049fba9d82a8`，只取 binary32 的 19 个编译单元，许可证随源码）+
  本项目自己的包装层。公开接口只收发整数位模式：`FPRounding`（RNE/RNA/RTZ/RDN/RUP）、五位
  `Flags`、add/sub/mul/div/sqrt/fma/remainder/roundToIntegralExact、negate/absolute/copySign、
  compare、classify、minimumNumber/maximumNumber 与六个谓词。
- **语义固定**：specialization 用 `ARM-VFPv2-defaultNaN`（canonical `0x7FC00000`、只有 sNaN 置
  NV），每次调用显式设置舍入模式与 tininess-after-rounding 并清空 flags，SoftFloat 的
  `infinite` 映射成公开的 DZ；`platform.h` 把 SoftFloat 状态定义成 TLS，所以仿真线程与 UI 线程
  同时调用互不干扰（真机 DLL 里导出的是 TLS 符号）。
- **独立 oracle 与黄金语料**：`tools/float-vectors.py` 用精确有理数独立实现同一套语义，生成
  `tests/data/float32/fp32-vectors.generated.hpp`——7571 条向量覆盖十二个入口与五种舍入模式，
  值位与五位 flags 逐位比对。oracle 自己先把 960 条最近舍入结果与 CPython double 算术交叉验证，
  再对每条结果做按定义写的性质检查（定向模式的方向与相邻值、最近模式的半个 ulp、开方用平方比较）；
  这两层检查在开发中抓到了 oracle 自身两个错误（RDN/RUP 反了、remainder 符号处理错了），
  所以语料不是内核的自证。
- **fast 门禁**：`tests/float-kernel.cpp` 外加三条契约用例（canonical NaN 与 payload 策略、
  flags 不粘滞、舍入模式不残留）和一个双线程用例（两线程各 20 万次不同舍入模式运算），
  由 `examples/float-ops/build.ps1` 编译执行；`examples/float-ops/build-kernel.ps1` 统一产出
  SoftFloat + 内核对象，Mod DLL、离线测试与真机驱动共用同一份。
- **真机证据**：内核随 `local.float-ops.mod` 打进 DLL，插件加载时自检并把一行结果写进日志
  （`1+2`、`(1+ulp)^2`、fused `fma`、`1/0`、`sqrt(-1)`）；其中 FMA 用例先乘后加会得
  `0x34000000`、fused 得 `0x34000001`，因此这一行同时证明单次舍入的 FMA 真的生效。
  `tests/float-compat-playtest.ps1` 与 `tests/float-boundary-playtest.ps1` 都会断言这一行。
- 仍未做：TestFloat 资格验证（M5 发布前跑，用上游完整源码树单独构建）、十进制 parse/format 的
  vendored fast_float/Ryu（M2）、整数转换（M4，需先定 §12 的 D2）。

## 未发布 — 浮点元件族 M0：独立 Mod 与 32 位宽通路兼容性（2026-09-25）

- **新增 `local.float-ops`**（`examples/float-ops/`）：一个完全独立的原生 Mod，自己带 DLL、构建
  与打包脚本，只使用公开 SDK 与 `tc.component.types`；不修改 `src/native_logic.hpp`、
  `loader.cpp`、游戏 DLL，也不进 `tc-loader.dll`。产物是 `dist/local.float-ops.mod`。
- **M0 交付的是兼容探针而不是算术**：32 位纯源、32 位直通、`R[32] + Flags[5]` 双输出、
  `Flags[5]` 纯汇、32 位纯汇，全部 `state_words=0`。它们覆盖后续 FP32 元件真正需要的形状：
  零输入源、零输出汇、一实例多输出、以及 5 位窄引脚。
- **探针会把真机行为写进日志**：复位段的头几个周期按实例逐条记录它读到的位模式（于是每个
  binding token 都能对上具体值）、每次 UI 刷新记录暂停板上显示的值、每次 RESET 汇总上一段的
  周期/刷新/失配计数。失配计数让“值对不对”由用例判定，而不是靠人眼看日志。
- **真机证据**：21 实例的 `source → pass ×17 → result+flags → sink`（另接 `Flags[5]` 汇）绑定
  token 1–21，四个 32 位形状逐实例读到完整 `0xDEADBEEF`，五位输出被读回 `0x0000001f`；测试
  专用驱动显式走「跑周期 → 暂停 2.5 秒（暂停窗口 `stable at cycle=0`，五类探针各两轮
  `refresh observed` 且值正确）→ RESET → 再跑周期」，复位后的周期线仍旧带完整位模式。
- **token 边界已量化**：42 实例长链上 token 1–33 正确，token 33 是第一个没有状态槽、因而没有
  发布宽输出的 token，它的消费者 token 34 读到 `0x00000000`。与既有“字宽输出最多 32 token”
  的记录一致，失败是确定性的空值，没有越界写入。
- 新增三个用例：`tests/float-package.ps1`（离线形状 + 隔离启停）、
  `tests/float-compat-playtest.ps1`（真机 21 实例 + 暂停/RESET）、
  `tests/float-boundary-playtest.ps1`（真机 42 实例边界，报告写进
  `build/float-boundary-report.txt`），外加 `tests/float-compat.cpp`、`tests/float-fixture.cpp`、
  `tests/float-compat-driver.cpp`。全部记录进 `docs/verification.md`，阶段结论进
  `docs/PLAN-float-components.md` §4.4。

## 未发布 — 拖节点时文本框独占鼠标（2026-09-23）

- **症状**：选中文本框后拖动边框上的缩放节点，棋盘会在同一手势里多出一条导线——真机复现出来的
  那条线两个端点正好是这次拖动的按下点与松开点（`-5,-3 -> -3,-2`，位宽 1）。
- **根因（反汇编 + 真机读数）**：棋盘在 `build_board_ui` 里用 `igIsAnyItemActive` 的回答决定走哪条
  分支，而**光标就在棋盘窗口内**时它走的那条分支照样会调用 `handle_ongoing_action`；也就是说
  "回答有控件在活动" 只换分支，不是棋盘的输入闸门。实测：整个拖动期间棋盘的 **76 次**采样全部被
  答成"有控件在活动"，导线依然出现——所以上一版的拦截不足以解决这条症状。
- **修法**：`handle_io_on_board` 每帧在函数开头读一次四个鼠标按键状态
  （`0x1403558c7`..`0x140355935`）并把它打包进自己的 io 状态；文本框在节点悬停/拖动期间
  **让棋盘读到的按键状态是"没按下"**。四个 ImGui 查询（`igIsMouseDown_ID`、
  `igIsMouseClicked_InputFlags`、`igIsMouseDoubleClicked_ID`、`igIsMouseReleased_ID`）
  只在返回地址落在那一个窗口内时才改写，其它调用者（面板、菜单、ImGui 自己）拿到真实答案。
  原来的采样拦截保留：两者一个换分支、一个换输入状态。
- **真机证据**（`tests/text-box-playtest.ps1`，默认门禁全绿）：

  ```text
  PASS text-box resize handles width=7.000000->10.957347 height=3.600000->5.596587 board-position=unchanged
  PASS text-box resize handles captured board input samples=76 board-objects=unchanged mouse-reads-hidden=608
  ```

  即：尺寸真的变了、元件坐标没动、棋盘的 76 次采样被接住、608 次"棋盘读鼠标"被换成"没按下"，
  而**棋盘自己的元件数与导线数在整个手势前后一字不变**（`components=8 wires=0`）。
- **灵敏度对照**：`TC_TEXTBOX_BLOCK_BOARD_INPUT=0` 用同一段代码、同一串合成鼠标事件关掉独占，
  棋盘立刻恢复原样——`blocked=0 leaked=77`，导线 `wires=0->1` 并在 83 阶段被提交，跑出
  `PASS text-box resize control: without the block the same drag reached the board`。两个方向都有日志。
- 同一轮里隐藏便签的**普通元件拖动照常生效**（`PASS text-box hidden note drag from -6,-18 to -5,-18`），
  所以"节点手势什么都没推动"不是因为棋盘整段失灵。
- 边界：节点独占只在**选中**的文本框节点上成立；鼠标移出窗口后松开导致状态没清的情况由
  "按住的元件已经不在棋盘上就丢弃手势"兜底。

## 未发布 — 文本框直接拖节点调整大小（2026-09-23）

- `examples/text-box` 选中后显示 8 个缩放节点：四个边点只改宽或高，四个角点同时改宽高；拖动时
  即时预览并在松开后持久化。原来的宽高滑条继续保留，作为精确输入和键盘操作的补充。
- 文字块改为贴住当前框的上内边距；放大高度后文字不再留在元件中心，左/中/右对齐都以缩放后的内容区
  为基准。真机门禁新增 `PASS text-box text layout follows resized top edge`。
- 移除一直画在便签中心的固定 8×4 调试轮廓；选中轮廓与 8 个节点现在包住实时可见框。
  `tc.component.geometry` 新增保持 V1 前缀的 V2：`set_instance_footprint` 可在运行期逐实例更新交互矩形；
  宿主把它接入游戏原生 `get_component_id`，返回原生元件序号，因此框内任意位置都能走游戏自己的
  选中、拖动、删除与属性面板流程，不再由文本框 Mod 维护虚拟选中态。
- 类型级 8×4 footprint 仍负责占位和放置碰撞；逐实例 footprint 只扩展原生交互范围，因此放大的
  注释框不会把覆盖的整片电路板声明成不可放置区域。文本框每帧上报最终可见框（包括文字撑开的尺寸）。
- 配置格式升到 v2，新增每实例的 `heightUnits`；旧 v1 数据通过迁移回调补上 3.6 格默认高度，原有文字、
  颜色、宽度和样式不变。文字仍可把盒子撑高，所以节点设置的是最小尺寸，不会裁掉内容。
- 真机用例用真实鼠标事件拖动右下角节点，硬断言宽 `7.00→10.96`、高 `3.60→5.60`，同时元件板面
  坐标保持不变；随后在固定类型 footprint 以外、放大后的框内单击，并断言游戏原生选择集包含该元件。

## 未发布 — 选中提示按 footprint 画（2026-09-23）

- 查清游戏选中提示的机制：`focus_components__...u5084` 只是给**元件自己的网格**换色，自定义元件的
  网格由 `get_mesh_custom__...u2020` 从网格工厂现建，跟 `tc.component.geometry` 声明的矩形无关——
  所以那圈白弧永远按元件网格大小画（实测约 3.9×3.2 板面格，而声明的 footprint 是 8×4）。
- `examples/text-box` 改为自己画选中高亮：通过 `sdk/tc_board_model.h` 读游戏选中集（键 = 元件序列
  索引，`NoteOnBoard` 因此新增 `index`），在 render 回调里对选中实例按声明 footprint 画橙色环 +
  淡填充。真机门禁新增两条硬断言（外接框 = 8×4 格、框心 = 元件中心）；像素回读实测
  `orange hint pixels=1347 bbox=1176,236..1402,350`（8×4 格 @ 28.16 px/格 + 线宽），
  证据图 `build/text-box-out/hint-selected-box.png`。
- **游戏自己那圈弧线也关掉了**：真正的绘制者是
  `redraw_selection__presenterZupdate95state95common_u5104`（`focus_components` 是死靶，棋盘阶段不被
  调用，宿主里那个钩子已删除）。它遍历 presenter 选中容器里的两个哈希集（`+0x00` 元件、`+0x18` 导线），
  每个 0x20 字节的桶按 `+0x08` 占位字是否非 0 决定是否为该元素加一个 selection 精灵实例；桶 `+0x10`
  是元素 id（元件侧 = 元件序列索引）。`tc.component.render` V3 的 `set_selection_hint(id,false)` 因此
  落地为：只在绘制这一帧把该元件那一桶的占位字清 0，调用返回后立刻还原——它不产生精灵实例，别的元件
  与导线完全不受影响，索引空间由游戏自己的 `contains(selected_components, ...)` 判定，不是猜的。
  真机逐像素（同一实例、同一窗口、截图帧里仍选中）：抑制生效 0 白像素，`TC_TEXTBOX_KEEP_ARCS=1`
  灵敏度对照 546。默认门禁与弧线场景都已进 `tests/text-box-playtest.ps1` 的硬断言。
- **原版那圈量清楚了，但没做进示例**：量出原版精灵的几何（圆心 = 元件原点 +1.00 格、内缘半径
  0.80 格、线宽 0.21 格、两段弧 44°–137° 与 223°–315°、随板面缩放），并做了一次原型——照原版断环
  画，半径/线宽/圆心偏移都是参数；同帧对拍（原版白环 + 橙色模仿环）实测模仿环 443 px、原版未被
  盖住的只剩 84 px，外接框差 1–2 px。按 2026-09-23 的决定，便签的选中提示继续用"跟着 footprint 的
  矩形"（它同时就是可拖区，信息量更大），原型连同参数与 4 张证据图
  （`build/text-box-out/hint-imitation{,-both,-custom,-overlay}.png`）撤出示例，登记到
  [reference/unused-interfaces.md](reference/unused-interfaces.md) 第 1 条，要用时按那里的步骤接回来。
- 用例稳定性：修掉"编辑器刚关就发拖动按下会被吞掉"的抖动（先关编辑器，下一帧再按）。

## 未发布 — 按类型去掉"在元件工坊编辑"按钮（2026-09-23）

- 交付 `tc.component.render` **V4**：`set_foundry_button(id, enabled)`。Mod 元件走的是游戏自定义元件那条
  路，选中它时底部面板右上角会出现游戏的"在元件工坊编辑"按钮（`Foundry`），而 Mod 元件没有可编辑的
  内部电路。宿主现在只在那一个按钮的 call site（`build_component_description_panel` RVA `0x3a45ac`，
  `igButton`）上动手：用全透明样式色画出它并返回"未点击"，于是按钮不可见也不可点，面板布局与其它
  控件完全不变；只要当前选中的元件里有一个不是调用方登记的类型，按钮照常出现。
- 真机逐像素（同一个 42×42 px 矩形）：默认关掉 0 像素（中心是面板底色），
  `TC_TEXTBOX_FOUNDRY_BUTTON=1` 的对照保留按钮 654 像素（中心纯白）。ABI 基线 1192 → 1203 条，
  新增记录全属 `TCComponentRenderApiV4`（V1/V2/V3 一条未变）。

## 未发布 — 便签的默认尺寸与随相机缩放（2026-09-23）

- 修用户报的两个问题：**①默认放下来太小**——默认盒子只有 6.5 × 1.5 板面格，而声明的 footprint 是
  8 × 4 格；现在默认配置按 footprint 定尺（字号 0.60 格、最小宽度 7.6 格、盒子最小高度 3.6 格），
  空便签放下就是 7.6 × 3.6 格，"看到的就是能拖的"。**②缩放不同步**——字号、内边距、编辑按钮都带绝对
  像素下限（`max(5,…)`/`max(2,…)`/`max(14,…)`），视野拉大（每格像素变少）时被地板顶住不再缩小，
  文字还会把盒子撑住；现在一切按 `unit` 成比例。证据：`TC_TEXTBOX_LAYOUT_TRACE=1` 在三个相机缩放
  （25.6 / 28.16 / 13.14 px/格）下报告的布局格子数完全一致
  （`box=7.00x3.60 button=0.62 text=0.50 padding=0.22 cells`），这条已进弧线场景的硬断言
  （`PASS text-box note layout stays … cells at 3 camera scales`）。
- 顺带修测试装置：探针中心在相机变动后重新播报（以前只在平移后报一次）；默认绘制的像素窗口收窄到
  ±45×±18 px 并把阈值改成随缩放的比例（`150·(unit/25.6)²`，最低 20），否则缩小时板面装饰的波形线会被
  数成"青色像素"。

## 未发布 — 文本框改回真 0 脚（2026-09-23）

- `examples/text-box` 不再为了"拿到实例"而留一根悬空输出脚：现在注册的是真 0 进 0 出
  （`shape=0in/0out`）。2026-09-22 记下的"没有任何引脚的元件绑不上实例"在本构建已不成立——
  加载器为 0 输出补的那个驱动门是真节点，编译器会把它编进平坦序列，一次运行里 7 个实例全部绑定，
  板上也照常出现在 `tc.board` 的元件序列里。
- 完整真机门禁重跑全绿（放置、rotation 0/1/2/3、缩放、平移、四边裁剪像素、关闭默认绘制像素对照、
  空像素处拖动一格、选中计数 1→0、编辑器读回），另加外观变化：游戏的引脚点不再出现，关闭绘制的
  实例窗口现在是全空。回退开关 `TC_TEXTBOX_OUTPUT_PIN=1` 可切回旧形状。
- 文档同步：`docs/reference/limits.md`、`docs/sdk/services.md`、`docs/research/custom-component-pins.md`
  中"两个方向不能同时为 0"的说法改为"两个都空 = 装饰元件，允许并有真机证据"，
  `docs/research/text-component.md` §1 增加结论更新说明。

## 未发布 — 元件接口 M5：render V2 关闭游戏默认绘制（2026-09-23）

- 新增 `tc.component.render` **V2**（完整包含 V1 前缀，只多一个 `set_default_drawing`）：Mod 在
  `tc_mod_load` 期间为自己拥有的类型关掉游戏给自定义元件画的设计图缩略图、名字水印与相关默认
  网格。宿主接管 `redraw_component__presenterZupdate95state95common_u4925`，只对**属于该类型、
  且带实例 id 的棋盘实例**跳过这次默认绘制；元件栏/元件选择器里的预览（id 为 0 的临时记录）保持
  原样，所以关掉图案不会让元件在菜单里变成空白。
- 关闭默认绘制只影响"游戏画什么"：footprint、命中、引脚数据、选中/删除和本 Mod 自己的 render
  回调都不变。文本框沙箱用同一类型的两个实例做对照——一个只被测量（真机 framebuffer 里默认绘制
  像素 `hidden=0`，同场景开着绘制的对照实例 `visible=350`），另一个被真实鼠标拖走一格
  （`-6,-18 -> -5,-18`，选中计数 1；随后 `clear_selections` 让选中计数回到 0）。三条断言都写进
  `tests/text-box-playtest.ps1`。
- 同一轮把拖动用例的读数污染查清并写进文档：拖动之后游戏会在该实例上画自己的**选中提示 UI**
  （引脚外侧的白色断环），它属于游戏的交互提示层，不是 V2 接管的默认绘制，也不是元件的缩略图；
  因此测量用"从未被拖动"的兄弟实例，严格阈值 `hidden<=4` 才能原样保留。
- ABI 基线按 §20 重跑并评审后更新：新增记录全部是 `TCComponentRenderApiV2` 自身（V1 记录一条未变），
  快照从 1173 条增至 1182 条；`tools/abi.ps1` 现在通过。

## 未发布 — 元件接口 M5：render V1 覆盖层切片（2026-09-23）

- 新增 `tc.component.render` V1 与 `sdk/tc_component_render.h`：Mod 可在加载期为自有类型登记逐实例
  棋盘绘制回调；宿主负责枚举当前棋盘实例并提供帧级句柄、custom/instance id、四向旋转、局部到
  屏幕仿射基、屏幕裁剪框与配置快照。
- 绘制经宿主受控图元表转发（线、矩形、圆、默认字体文本），不外借 ImDrawList/renderer 私有指针；
  所有借用对象只在回调内有效，每实例每帧上限 4096 条，非法坐标/尺寸与超长文本会被拒绝。
- `examples/text-box` 成为首个消费者：为便签声明 8×4 footprint（整框均可拖），并通过正式 render
  回调用宿主仿射基画旋转一致的边框。原有复杂字号/斜体正文暂仍走旧覆盖绘制，等待字体资源切片。
- 新增 render SDK/仿射离线测试，ABI 快照从 1120 条增至 1173 条。当前仍未交付：关闭游戏默认绘制、
  纹理/字体资源生命周期、设备重建通知、缺 Mod 可见占位和完整像素回读验收。
- 变换真机验收补齐第一部分：文本框沙箱一次放置 rotation 0/1/2/3 四个实例，回调把每组旋转轴逆变换
  回统一棋盘基并逐项比较，同时检查四角处于宿主裁剪框；稳定输出
  `PASS text-box render transforms rotations=0xf unit=25.60/25.60 clip=2560x1600`。测试目录现在把该
  PASS 行作为硬断言，旋转轴漏换、符号错误、无效缩放或裁剪框会直接失败。
- render 热路径不再借 `capture_objects` 顺带创建全部 wire 句柄和每帧分配 wire 数组；宿主直接遍历
  当前组件序列，只给实际 custom component 发帧级句柄，复杂布线板的绘制成本不再随导线数额外增长。
- 相机缩放也进入同一真机门禁：自动化在棋盘空白处发送一格滚轮，要求回调中的实时轴长从
  `25.60` 变化为 `28.16`，并输出 `PASS text-box render zoom live unit 25.60 -> 28.16`；若宿主错误地
  缓存旧相机比例，文本框用例会失败。
- 相机平移进入同一真机门禁：自动化在空板上执行中键拖动，记录四个旋转实例平移前后的 render
  原点，要求四者得到同一位移且各自的仿射轴不变；实测输出
  `PASS text-box render pan delta=150.00,0.00 axes unchanged instances=4`。平移后编辑按钮仍按新的屏幕
  坐标命中。
- 裁剪从“只检查矩形数值”升级为真机 framebuffer 像素门禁：诊断模式把裁剪框内缩到无遮挡板面，
  文本框回调用四种不透明色块分别跨越左/右/上/下边；截图逐像素回读得到
  `left=800..831 right=1768..1799 top=200..231 bottom=818..849`，与
  `800,200..1800,850` 的半开裁剪边界完全一致。泄漏一个像素或整块消失都会使脚本失败。

## 未发布 — 元件接口 M5：geometry 第一刀

- 新增 `tc.component.geometry` V1 与 `sdk/tc_component_geometry.h`：Mod 可在加载期为自己已登记的
  类型声明 footprint 矩形，并从实时元件句柄回读游戏实际使用的、已考虑旋转的半宽高；放置路径
  已真机确认。
- 命中判据追查：新增 `tests/component-hitbox-map-playtest.ps1` 与探针的 `TC_HITBOX_SCAN=map`
  模式（63 个网格偏移 + 5 个对照点）。对照点暴露了旧探针装置的缺陷：它用 `SetCursorPos` 驱动
  真实光标、且按下前没有先 hover，测试期间的真实鼠标动作会把采样点顶掉（离元件 40 板面格的空点
  也读成"已选中"，多次运行的地图互相矛盾）。装置已改为按下前一帧先 hover、用 `ClipCursor` 钉住
  光标、逐点记录 `clean=0/1` 并丢弃脏样本。修好后：只按下不移动的手势整窗读 0；按下 + 8 px 拖动
  时旧用例的 `(1,0)`/`(2,0)` 结论可复现（两种 footprint 相同），但只有两点；用拖动手势跑整张
  网格的那次被污染检测判废，干净的全网格仍待补。每次运行的地图追加到
  `build/hitbox-map-out/map-history.txt`；静态调用链地图见 `docs/research/component-hitbox-path.md`。
- 宿主通过游戏自己的 Prototype 复制、Nim 序列分配与 `custom_prototypes_set` 路径写入，不向插件
  暴露原型偏移；旧类型不声明时完全不变。
- 半宽向外量化到整数格，错误线程、越界、非所有者和 stale 句柄均有明确错误码；新增离线几何
  测试与 ABI 快照。真机服务烟测确认 `set_footprint(6,3)` 返回 OK，游戏当前 Prototype 反读为
  `(-6,-3,12,6)`，并在真实棋盘实例上回读 `half=6,3`；研究探针另确认该私有形状序列可稳定
  进入游戏放置路径。
- 本条只标记 M5 已开始：render、资源生命周期、文本框迁移、独立悬停自动化与缺 Mod 可见占位
  仍未交付。

### 修复：命令总线放置的元件现在立刻可点可拖

- `TC_COMMAND_BOARD_PLACE_COMPONENT` 过去只调用游戏的 `add_component`，而游戏自己的放置
  （元件菜单或点板面落件）在那之后还会调用 `upgrade(presenterSlot, 0x30)`。少了这一步，放上去的
  元件**不在棋盘的命中状态里**：鼠标点不到、拖不动，直到棋盘被别的原因刷新一次。
  真机证据：用户手动放一个元件之后，先前用命令总线放的三个元件才一起变得可拖。
- 加载器按自己的惯例**从游戏自身的调用里学习**这个指针：新增符号别名 `board.after_place`
  （`upgrade__presenterZcontext_u2766`），`armBoardRegistration()` 在启动时挂一个只记录第一参数
  再转发的钩子，`add_component` 成功后重放同样的 upgrade。槽位尚未学到时会写明日志，不静默失败。
- 验收（探针不做任何重放）：修复前按内置 AND 门**本体** `selected=0 key=MAX`（只有引脚那格会中），
  修复后按本体 `selected=1 key=1 snapshot=1`，按空板回到 `key=MAX`。命中探针的"选中集永远为 1"
  的闩锁现象也随之消失——那是未登记状态的产物。
- 附带发现（写进 `docs/research/component-hitbox-path.md`）：命中跟随**画出来的图形**，
  `set_footprint(6,3)` 不改变图形（截图里 12×6 与默认 1×1 像素级相同），所以 `set_hit_box` V2
  不能只是声明更大的矩形。

### 结论：可拖区域就是 footprint 框，"整框可拖"不需要新几何

- 真人手测（用户对着探针画在画面上的青框操作，`TC_HITBOX_DRAW=1`）：`set_footprint(6,3)` 的
  **带引脚**类型与**完全无引脚**（0 入 0 出，文本框那种）类型，都可以从框内远离图形、完全空白的
  角落按住拖走。截图见 `build/hitbox-dev-out/boxes-ingame2.png`。
- 因此 M5 **不需要**为"整框可拖"再发明 `set_hit_box`：声明与盒子等大的 footprint 就是那条路，
  而它已经在 V1 里。footprint 同时仍是占位/碰撞框，放大即真的占那么大地方（便签本来就要）。
- 之前"footprint 不改变命中"的说法作废：真正的干扰是①命令总线放置的元件没登记进棋盘命中状态
  （本轮已修，见上）、②"选中集"读数会闩锁、③交替拖动会和邻居碰撞箱互相影响。自动化的移动差分
  网格降级为辅助手段，手感结论优先。
- `sdk/tc_service_api.h` 的 geometry 注释、`docs/research/component-hitbox-path.md` 开头都改成
  这个结论。

### 研究工具：原版元件外观的外形测量（2026-09-23）

- 新增 `tools/component-sprites.js`：从 EXE 的字符串表还原 **kind → 精灵名** 表（125 条，与 125 个
  内置原型逐条对应），并对照 `asset/component_sprites/` 报出画布尺寸与内容框。
- 研究探针新增 `TC_HITBOX_KINDS` / `TC_HITBOX_KIND_LAYOUT` / `TC_HITBOX_KIND_ORIGIN`
  （只放置、不注入，配 `TC_MODLOADER_SHOT` 进程内截图），`build/hitbox-dev/measure-sprites.ps1`
  按 shader 调色板量出可见主体并和精灵画布/内容框对照。
- 结论：**画布以元件记录点为中心，1 板面格 = 20 精灵像素**（四边形 = 画布 / 20；140x100 → 7x5、
  180x100 → 9x5、300x180 → 15x9）；106 个 kind 全量扫描、101 个有缩放读数，中位数 0.0496/0.0495。
  长条/多引脚元件（RAM、Load port、Delay line word、Decoder 3、Level gate、Maker/Concatenator 8、
  OFF）按轴拉伸，是本 kind 自己的矩形。证据与例外清单见
  `docs/research/component-appearance.md` §0，数据在 `build/hitbox-dev-out/sprites-measured.csv`。

## 未发布 — Loader：`tc.io_value` 服务（通用底层）

### 元件接口 M2 回补第三刀：`on_load` 与 `on_save`（2026-09-22）

- **`on_load`**（`TC_LOGIC_LOAD=7`）：实例的配置**来自电路文件里的记录**（原样恢复或经迁移转换）时
  触发，在 `on_create` 之后（复制出来的实例改用 `on_clone`），回调里 `config` 就是文件里的字节。
  **被拒绝的记录不发**——那种情况实例跑默认配置。这样"这是从存档来的"不再需要插件拿字节和默认值去猜。
- **`on_save`**（`TC_LOGIC_SAVE=8`）：游戏**即将写出电路**时对每个活实例触发。两个入口都派发：
  关卡保存（`save.level`，事件总线本来就在监听）与原理图写入（新增别名 `save.schematic` =
  `save_this_schematic__modelZboardZschematics_u132`，别名数 33 → 34）。回调里可以用普通
  `write_config` 提交派生数据或修复配置，**正在写的文件就看到它**。
- **锁纪律**：`on_save` 与 `on_config_changed` 一样在实例锁**外**运行（否则"在 on_save 里写配置"
  会因为同一把非递归锁死锁——第二版实现正是如此，离线用例直接挂住，已修）；`on_create`/`on_clone`/
  `on_load`/`on_destroy` 仍在锁内，别在它们里面调服务。
- **一个实现细节**：编译看到的是扁平化序列，绑定期棋盘记录可能读不到，回读会被推迟到第一次服务
  调用——所以 `on_load` 在"真正把记录装进配置"的那一刻发，而不是死守绑定窗口。
- 证据：离线 `tests/native-component.cpp`（有记录→触发/无记录→不触发；`on_save` 派发到所有活实例，
  且回调里写配置会落到记录里）；真机 `component-storage` 三个模式在 PASS 行报
  `loads=/saves=`，playtest 逐个断言 persist `0/1/1`、migrate `1/1`、reject `0/0`，且每次启动都至少
  有一次保存派发（36s / 26s / 26s 通过）。
- ABI 基线新增 `TC_LOGIC_LOAD`、`TC_LOGIC_SAVE` 与 `on_load`/`on_save` 两个偏移
  （生命周期结构 40 → 56 字节，尾部追加）。
- **M2 回补剩余**：move/rotate/resize 与编译类回调。

### 元件接口 M2 回补第二刀：`on_clone` 与复制语义（2026-09-22）

- **命令总线新增 `TC_COMMAND_BOARD_DUPLICATE_COMPONENT`**：`argument` 是源元件的实例 id，
  `custom_prototype_id` 是它的类型，`x`/`y`/`rotation` 是目的地。放置走游戏自己的助手（于是**一次
  Ctrl+Z 撤掉整次复制**，副本连配置一起消失）；源实例的配置被逐条写进新记录的 tail 表，**不占自己
  的撤销步**——复制对玩家是一个动作。
- **`TCComponentLifecycleV1` 末尾追加 `on_clone`**（结构 32 → 40 字节），`TCLogicPhase` 加
  `TC_LOGIC_CLONE = 6`。它在 `on_create` **之后**、副本配置已就位时触发，回调里 `config` 就是源实例
  那份字节；插件可以在这里发新身份，那次改动是普通 `write_config`，自己占一条撤销。
- **为什么把字节交给绑定，而不是只靠回读记录**：编译看到的是扁平化序列，绑定时棋盘记录可能还读不
  到（实测：源实例的配置回读被推迟到第一次服务调用）。复制路径本来就知道自己抄了哪些字节，所以
  宿主在标记副本时**同时带着字节**，绑定时装进去——`on_clone` 拿到的配置因此是确定的。源实例已绑定
  时优先用宿主手里的配置，记录只作后备。
- 证据：离线 `tests/native-component.cpp`（普通绑定不触发、被标记的副本触发一次且看到抄来的字节、
  `on_create` 也看到那些字节）；真机 `component-undo` 扩展为"两次单笔写入 + 一组 `begin/commit` +
  一次复制"，断言 `clone=d1d2d3d4` 与 `on_clone` 收到同一个首字节，连续两次运行通过（约 15s）。
- ABI 基线新增 `TC_LOGIC_CLONE`、`offsetof(TCComponentLifecycleV1, on_clone)` 与
  `TC_COMMAND_BOARD_DUPLICATE_COMPONENT`。
- **M2 回补剩余**：`on_load`/`on_save`，最后是 move/rotate/resize 与编译类回调。

### 元件接口 M3 收口：编辑事务分组与存档上限实测（2026-09-22）

- **`tc.component.storage` V2**：在 V1 的五个入口后追加 `begin_edit` / `commit_edit` /
  `abort_edit`。批量工具显式说明动作的起点与终点（计划 §9.3 要求，不许按帧猜分组）：begin 记住当前
  字节，中间任意多次 `write_config` 照常改配置、照常发 `on_config_changed`，但**不**各自压栈；
  commit 把整段压成**一条**撤销，abort 还原到 begin 时的字节且不动撤销栈。不嵌套，实例释放/换板
  丢弃未提交事务，与单次写入共用 64 步上限。V1 表继续可查询。
- **真机验收**：`component-undo` 改成"两次单笔写入 + 一次 begin/两步写入/commit"，断言**一次撤销**
  把记录退回 begin 时的字节（不是组内任何中间态），一次重做回到组末态；空转 15.8s 通过。
  离线 `tests/native-component.cpp` 覆盖同一路径（含重复 begin/commit/abort 返回 `ERR_STATE`）与
  "回退也要同步宿主内存里的配置"（`component-undo` 早先只改记录、服务会读到旧值——这是同一类
  陷阱，已修）。
- **1024 字节上限实测**（新用例 `component-capacity`）：探针注册 1024 字节配置，经服务写满后存档、
  重启回读：

```text
run 1: PASS capacity written launch=1 entries=5 file=235 wrote=identical entriesAfterWrite=132
run 2: PASS capacity survived save plus restart: launch=2 entries=132 file=1203 readback=identical
```

  即一个满配实例恰好占 **132 个表项**、字节跨重启一致；原理图从 235 字节涨到 1203 字节，也就是
  **每个满配实例约 1 KB**（原始 132×16 ≈ 2.1 KB，Snappy 压缩后）。顺带记一个现象：游戏在装载关卡
  时会把存档重写成规范化形式（夹具 752 → 235），所以"文件大小"要在装载之后再读才有意义。
- ABI 基线新增 `TCComponentStorageApiV2`（8 个入口）、`TC_COMPONENT_STORAGE_API_VERSION_2` 与
  `TC_COMPONENT_STORAGE_ERR_STATE`。
- **M3 到此收口**：四条验收（存储契约、迁移、缺失 Mod、编辑事务）都有离线 + 真机证据。明确留到
  后续的：缺 Mod 期间画面上的占位元件（并入 M5）、仿真状态是否入档（计划 §18 的决策点）、
  复制（`on_clone`，属于 M2 回补）。

### 元件接口 M2 回补第一刀：`on_config_changed`（2026-09-22）

- `TCComponentLifecycleV1` 末尾追加 `on_config_changed`（结构 24 → 32 字节，全部在尾部；`size`
  没覆盖到它的老调用方不受影响），`TCLogicPhase` 追加 `TC_LOGIC_CONFIG_CHANGED = 5`。
- 触发语义：一次成功的 `write_config`；一次撤销/重做把配置改回旧值。**不**触发：写入相同字节；
  装载存档记录或迁移旧 schema（那是绑定过程，回调在 `on_create` 里就能看到那些字节）。
- **回调在实例锁之外运行**，所以可以在里面调用 `tc.component.storage`；这条不是风格问题——第一版
  实现把通知留在锁内，离线用例立刻死锁（`on_config_changed` 里调 `storage.info` → 同一把非递归
  互斥量），修成"锁内改状态、锁外通知"后才通过。
- 证据：离线 `tests/native-component.cpp` 断言"写入触发一次且看到新字节 / 相同字节不触发 /
  撤销与重做各触发一次并看到被恢复的字节 / 回调内调用服务返回 OK / 记录里的表与内存一致"；
  真机 `component-storage` 三个模式在 PASS 行里带上 `config_changes=`，分别断言
  `1 / 1 / 0`（persist）与 `0 / 0`（migrate、reject）——即"写入算一次改变，装载与迁移不算"。
- ABI 基线新增 `TC_LOGIC_CONFIG_CHANGED` 与 `offsetof(TCComponentLifecycleV1, on_config_changed)`。
- **M2 回补剩余**：`on_clone`、`on_load/on_save`，最后是 move/rotate/resize 与编译类回调。

### 元件接口 M3 第五刀：配置写入进入撤销/重做（2026-09-22）

- **一次成功的 `write_config` 就是一次可撤销的配置提交**。游戏自己的撤销栈没有"配置变更"这类条目
  （它的变更种类都是棋盘编辑，而写值的命令一条都不登记），所以宿主保存这一步的**前后字节**，并在
  游戏撤销入口被按下时，只要还有未消费的配置步骤就把它写回元件记录并报告"已处理"。写入相同字节
  不产生步骤；步骤 LIFO，新写入清空重做栈；每块板最多 64 步；实例释放或换板即丢弃相关步骤
  （记录已不存在，旧 id 可能指向别的元件）。
- **不影响棋盘编辑**：只有存在未消费步骤时宿主才接管这次 undo/redo，栈空时原样调用游戏的撤销/重做。
- **新用例 `component-undo`**（`tests/component-undo-probe.cpp` + `component-undo-playtest.ps1`）：
  经 `tc.component.storage` 写 A、写 B，再用**游戏自己的撤销入口**（命令总线，玩家 Ctrl+Z 走同一条
  函数）撤销与重做，每次读**棋盘记录里的表**而不是服务：

```text
PASS one undo restored the first configuration and one redo re-applied the second:
     instance=2459565876494606882 wrote=a1a2a3a4 then=b1b2b3b4 afterUndo=a1a2a3a4 afterRedo=b1b2b3b4
```

- **测量中排除的两个坑**：① 游戏 Board API 的第一个参数是 `load_level` 接收的那个对象，不是它内部
  +0x78 的表——把 +0x78 当 board 传会让放置与撤销静默失败；② 不能用 `tc.component.storage` 读
  "撤销之后"的配置，绑定会保留内存副本。两条都写进了 verification.md。
- **仍未做**：把配置提交与棋盘编辑合并成同一组事务（`tc.transactions` 目前只暂存命令）、
  缺 Mod 期间画面上的占位元件（并入 M5）、复制（`on_clone`）。

### 元件接口 M3 第四刀：把"缺失 Mod"量清楚（2026-09-22）

这一刀没有改产品行为，改的是**我们知道什么**：在写占位实现之前，先用真机把"缺 Mod 时游戏到底
丢什么"测成了可复现的证据（`component-placeholder` 用例，四个场景，每次启动前清空日志）。

- **缺 Mod 时元件在加载期就被换掉**：槽位变成 kind 0 墓碑（实测 `custom=0 tombstones=1`），
  连线保留、端点仍是原引脚坐标（`wire=0 a=(-6,-1) b=(-12,0)`）。
- **只加载不存档不损坏文件**：`circuit.data` 哈希与夹具一致；装回 Mod 后配置按第三刀的迁移路径
  回来（`migrated ... legacy=1`）。
- **在缺 Mod 的状态下存档会永久丢配置**：文件被改写，再装回 Mod 得到
  `found no instance`——元件与它的记录都没了。
- **"以前装过"不构成保护**：先装 Mod 再移除，元件照样变墓碑；游戏不跨会话保留自定义原型。
- **排除一个想当然的修法**：钩住 `get_custom_prototype__modelZboardZcustom95prototype95list_u451`
  后，整次加载里没有任何一次查询针对夹具里的 id（只看到 `12dc0381356d9010` 两次，且该 id 不在
  夹具中），所以占位必须挂在真正决定丢弃的那段代码上。新增探针
  `tests/component-placeholder-probe.cpp`（只报告，不注册）与真机用例 `component-placeholder`。
- **捕获落点（同日续）**：解析器把整条记录建在自己的栈上——v13 反序列化器的 kind 在 `[rsp+0xe0]`、
  table 在 `[rsp+0x270]`，相差记录内表偏移 `0x190`——所以钩住 `save.custom_tail_set` 就能从
  `table - 0x190` 读出**正在构造的整条记录**：kind、坐标、旋转、实例 id、custom id 和全部 tail
  键值，**缺 Mod 时同样拿得到**。运行时据此新增缺失 Mod 捕获与诊断：启动时接管该 setter、
  `level.load` 链节调原函数前开窗、按实例 id 归档、下一帧判定"该 custom id 没有注册定义且记录不在
  棋盘上"后报告
  `Missing Mod: custom 0x… at (x,y) rotation r was dropped ...; it carries a N-byte schema-S configuration record, kept for a reinstall`
  与一行汇总。用例 `component-placeholder` 新增场景 0 对照并断言"缺 Mod 有该行、元件有主时没有"。
- **救援注入（同日第二段）**：缺 Mod 时把捕获的记录写进宿主自己的
  `<数据目录>/missing-mods/<关卡>.bin`（`TCM3RSQ1` 格式：custom id / 实例 id / 坐标 / 旋转 /
  全部 tail 键值）；下次装载同一关卡时，对"主人已注册且 (custom id, 坐标) 上没有元件"的条目，用
  宿主已有的放置助手 `add_component__presenterZutilitiesZhelper95functions_u5918` 放回棋盘，再逐项
  把 tail 写回，成功后从 store 移除。真机链路：缺 Mod 存档 → `kept 1 record(s)` → 装回 Mod →
  `Missing Mod rescue: put custom 0x… back at (x,y) rotation r with N stored configuration entries`
  → `tc.component.storage` 探针找到实例并完成 schema 迁移 → `kept 0 record(s)`。用例
  `component-placeholder` 原先断言"装回 Mod 也找不回来"，现在改为断言"能从 store 恢复"。
  两点如实记录：实例 id 会变（连线按坐标连接，插件句柄看到新实例）；救援一次性，注入后条目即移除。
- **仍未实现**：缺 Mod 期间**画面上可见**的占位元件（现在游戏仍把元件变成墓碑，我们只在主人回来时
  放回，所以诊断在日志里）；要画面可见需要 M5 的棋盘绘制或占位原型。

### 元件接口 M3 第三刀：老存档的配置迁移（2026-09-22）

- **`TCComponentTypeDefinitionV2` 追加迁移尾字段**（`config_migration_version` / `migrate_config` /
  `migration_user`）。结构体 136 → 160 字节，全部追加在末尾；注册仍只要求首发前缀，老定义不声明
  迁移就得到保守行为。
- **三个明确结果**：`MIGRATE_OK` 把转换后的字节变成实例配置并**升级记录**（下一次存档带走新
  schema）；`MIGRATE_KEEP` / `MIGRATE_REJECT` 都不写回、保留原字节、实例跑默认配置；未知返回码
  一律按拒绝处理，坏回调不会被误当成升级成功。
- **只迁该迁的**：只有"同一个定义 id + schema 或长度不符"的记录会送到回调；来自别的定义
  （`ForeignType`）、校验和不符、格式号未知、超过预算的记录不会被迁移，也不会被改写。
- **回调拿到真字节**：传入的是电路里存着的那份（已通过校验和验证）以及它写入时的 schema 与长度，
  可能和当前 `config_size` 不同；宿主在提交前用自带解码器复核，提交顺序仍是分块在前、校验和最后。
- 记录读取拆成 `component_tail::readStored()`（不管适配，读出 schema/长度/字节）与
  `decode()`（按定义严格匹配），这样"读得出来但用不上"和"记录坏了"不再混为一谈。
- **证据**：离线 `tests/component-tail.cpp`（readStored/decode 两种视角）与
  `tests/native-component.cpp`（转换 / 保持 / 拒绝 / 无回调 / 乱返回码五种路径，含"记录字节一个
  都没变"）；真机新增夹具 `build/nl_not1legacy.data` 与用例 `component-storage-migrate`、
  `component-storage-migrate-reject`：schema 6 的记录被转成 schema 7 并在下一次启动无迁移读回，
  拒绝时两次启动都被喂到同一份老字节。见 [verification.md](verification.md)。
- **仍未做**：缺失 Mod 占位、`on_clone`、Undo/Redo 快照。

### 元件接口 M3 第二刀：配置写进元件存档（2026-09-22）

- **配置随元件存档**：每个实例的配置不再只活在内存里。宿主把整条记录写进元件记录自己拥有的
  64 位键值表（游戏 `save_monger/versions/v7` 的 `Table[int64, int64]`），游戏把它作为元件记录
  的一部分序列化进原理图，所以配置随复制、存档与关卡加载一起走，不需要外置 sidecar。
  键空间高 32 位是 `TCM3` 魔数、低 32 位是字段号；不认识的表项原样保留。
- **绑定即回读**：实例绑定时解码记录，只有格式/定义 id/schema/长度/校验和全部通过才覆盖
  `default_config`，因此 `on_create` 看到的是存档中的配置；拒绝的记录被报告并原字节保留
  （迁移留给下一刀）。
- **写入即记录 + 原子提交**：`write_config` 先构造记录、用宿主自己的解码器复核，再按
  "数据分块 → 定义/格式/schema+长度 → 校验和最后"的顺序提交，最后才改内存；记录写不进去时
  整次调用返回 `UNAVAILABLE`，内存配置保持旧值，不会出现"界面显示新值、存档里还是旧值"。
- **构建画像控制布局**：`compat/profiles.json` 新增 `layout` 段（尾表偏移、元素步长与表头），
  由 `generate-compat.ps1` 校验并生成 `TC_LAYOUT_*`；画像未命中时宿主不装配持久化访问器，
  `info.flags` 的 `TC_COMPONENT_STORAGE_HAS_PERSISTENCE` 为空，配置退回内存层。
- **新的符号别名** `save.custom_tail_set`：游戏自己的表赋值函数
  `X5BX5Deq___modelZsave95mongerZversionsZv7_u70`。宿主只在游戏线程、只在当前棋盘里按
  `(custom_id, instance_id)` 精确匹配的记录上调用它，且从不保存记录或表指针。
- **证据**：离线 `tests/component-tail.cpp` 与 `tests/native-component.cpp` 的持久化段；
  真机 `component-persistence-insert`（空表插入 → 扩容 64→128 → 存档 → 重启回读）与
  `component-storage`（经服务写入 → 存档 → 重启回读到 `aabbccdd`/`01234567`），见
  [verification.md](verification.md)。ABI 基线新增一个 flags 常量，结构体布局不变。
- **仍未做**：schema 迁移回调、缺失 Mod 占位、`on_clone`、Undo/Redo 快照。

### 元件接口 M3 第一刀：配置/仿真状态分离与内存快照（2026-09-22）

- `TCComponentTypeDefinitionV2` 追加配置 schema、字节数与默认 blob（每实例最多 64 KiB，宿主复制）；
  注册改成真正的 size-prefix 兼容，不再要求调用方结构大小必须等于当前头文件。
- `TCLogicIOV2` 追加只读配置视图；RESET 只清仿真状态，不碰配置。
- 新服务 `tc.component.storage` V1 与 `sdk/tc_component_storage.h`：配置原子读写、revision、仿真
  状态捕获/恢复，全部使用 M2 的 generation 句柄拒绝旧实例。
- 离线用例覆盖默认值、短缓冲、schema/长度拒绝、配置更新在下一回调可见、RESET 隔离、状态恢复与
  stale 句柄；服务发现和 ABI 快照同步更新。
- **明确边界**：V1 只是活实例的内存所有权/快照层；跨重启存档、迁移、缺失 Mod 占位、复制和
  Undo 仍待下一刀，见 [HANDOFF-component-m3.md](HANDOFF-component-m3.md)。

### P3：逐周期采集（`tc.sim.capture` + 生成源码注入 tick，2026-09-21）

示波器最要命的那一半：**连续高速跑时一个周期都不漏**。按用户定的"一步到位"，走生成源码注入，
而不是先做"切片运行"。

- **采样点**：加载器在编译源码时，于**每个 `cycle += 1` 之前**插入
  `game_engine.'tc_scope_tick'(U64 cycle)`（实测锚点：生成程序里 `mode_run` 的
  `while cycle < burst_target_cycle` 循环，以及单步路径的另一处；见
  `docs/research/waveform-handoff.md` §4 与 `native-logic-source-*.txt`）。那一刻是"这一拍
  已算完、周期号还没 +1"，所以语义是**周期 N 的稳定状态**；玩家运行、单步、关卡自带测试
  全都经过它。没有 `cycle += 1` 的程序**原样放行**，不被改写。
- **加载器导出 `tc_scope_tick`**：与原生逻辑桥的 `tc_logic_invoke` 同一套注册方式
  （`prepareCompiler()` 把名字→函数指针注册进编译器符号表），未采集时第一句就是一个 relaxed
  原子读 + 返回，热循环里的代价是一个调用 + 一个分支。
- **采集内核 `src/scope_capture.hpp`**：预分配环形（`depth` 拍 = **预触发窗口**）、
  触发（上升/下降/任意边沿/按值匹配 + mask + holdoff）、以及三样必须可靠的记账——
  `gaps`（两次采样周期号不是 +1 的次数，**0 = 没漏**）、`restarts`（周期倒退 = 关卡重置/新一轮，
  整段重记而不是把两次运行接起来）、`injected`（当前程序里到底有没有 tick）。
  仿真线程只写、渲染线程只读，SPSC 单环形。
- **服务与 SDK**：`tc.sim.capture` V1（`configure/start/stop/read/status`）+ `sdk/tc_sim_capture.h`；
  状态里同时给出窗口首尾周期，方便界面画"触发点前后"。
- **离线证据**：新增 `tests/scope-capture.cpp`（`build.ps1` 内执行）——环形窗口与行序、
  行数=拍数、**跳过周期会被计成 gap**、周期倒退会清空并计入 restarts、上升/下降/按值触发、
  停止后不再增长、以及注入本身（插在 `cycle += 1` 之前、缩进保留、没有该语句的程序不动）。
- **真机证据（注入不破坏编译）**：`tests/waveform-playtest.ps1` 照常 PASS
  （"the board panel traced the running level's inputs and outputs per cycle"），日志里
  `Scope capture: per-cycle state reader armed` 与
  `Scope tick: instrumented 2 per-cycle call site(s) so far`（一个关卡编译两次、每次两处
  `cycle += 1`，因此随后涨到 4）——改写后的程序照常编译、照常跑，每一拍都经过 tick。
- **真机证据（服务链路，2026-09-22）**：`tests/scope-capture-playtest.ps1`（新增用例
  `scope-capture`）用 `example.byte-adder` 的自动测试加载 `byte_adder` 关卡并跑满 39 拍，
  驱动 `tests/scope-capture-driver.cpp` 只负责 configure/start/读回：结果是
  `rows=30 written=30 first=0 last=29 gaps=0 restarts=1 injected=1 channels=64`、`consecutive=1`，
  即**一拍一行、一个周期都没漏**；同一时刻渲染线程的 `tc_trace` 采样只有 31 行（每帧一行）。
  驱动读的是状态缓冲的一段（它没有板子句柄，不去猜通道名），用例再用 `example.byte-adder`
  自己回调里逐周期的 `carry_in/a/b` 当日志当**oracle**：状态列 34/33/32 在 7 个可比周期上
  完整复现这三条独立序列——所以采到的确实是关卡自己的数据，不是"看起来很合理"的数。
- **本轮修掉的两个真机缺陷**：
  1. **重复 tick 被当成重启**：生成的程序里有两个注入点，一拍会 tick 两次；旧代码把
     `cycle <= lastCycle_` 一律当重启，于是每一拍清空环形、窗口里永远只有 1 行
     （真机上表现为 `restarts=83 rows=1`）。现在同周期重复上报 = **改写那一行**（后报的值为准），
     不算 gap 也不算 restart；`tests/scope-capture.cpp` 新增该场景。
  2. **通道坐标混用**：`tc::trace::Sampler` 的 `inputSlot()/outputSlot()` 是关卡 I/O
     **历史缓冲**里的槽位，而 tick 读的是**仿真状态缓冲**；混用不会报错，只会安静地采到全 0
     （真机上表现为 `live channels=0/64`）。示波器必须用 `tc.sim.channel` 给的导线状态偏移，
     已写进 `docs/sdk/simulation.md` 的契约。
- **真机待办**：示波器面板本身（P4），建在 `tc.sim.capture` 上。

### 元件接口 M2：实例句柄、生命周期与状态归属（2026-09-22）

按 [HANDOFF-component-m2.md](HANDOFF-component-m2.md) 开工的第一刀，计划书 §7 的四件事
（实例句柄、生命周期、宿主状态 blob、失效规则）都落地了，配置/仿真状态分离与
`on_clone`/编辑器类回调留给 M3。

- **新服务 `tc.component.instances` V1**（SDK：`sdk/tc_component_instances.h`）：
  `enumerate`（按类型过滤）、`validate`、`info`（脚数/状态字数/调用·peek·reset 计数/类型 id）、
  `state`（读宿主状态 blob）、`reset`（只重置一个实例）。句柄带**宿主发放的 generation**：
  槽位被复用后旧句柄返回 `ERR_STALE`，不会解析成"现在恰好在那里"的元件。
- **生命周期**：`TCComponentLifecycleV1{on_create,on_destroy}` 以**追加指针**的形式挂在
  `TCComponentTypeDefinitionV2` 末尾（`size` 未覆盖该字段的调用方行为不变）；相位新增
  `TC_LOGIC_CREATE=3` / `TC_LOGIC_DESTROY=4`。回调在"变化发生的地方"同步执行、写入的状态
  立即提交。
- **失效规则**：编译结果里出现本类型实例的编译才是棋盘本身——其中缺失的实例被释放并触发
  `on_destroy`；离开棋盘/切场景全部释放；释放保留槽位（token 不移动），槽位复用时发放新
  generation。
- **状态归属**：实例状态在加载器的 `Binding` 里按定义大小分配，`0x9a0000` 那 32×512 字节
  只剩"宽输出按位回读"一个内部用途，公共契约里不再出现它。
- **离线证据**：世代校验、枚举/缓冲边界、单实例 reset、释放与槽位复用（`tests/native-component.cpp`）。
- **真机证据**：`pin-shape-wide9` 的实例走查——两个实例 `on_create` 各一次、枚举 2 个且
  generation 不同、reset 只动目标、删除后 `on_destroy` + `released 1 instance(s)` + 枚举变 1、
  旧句柄与伪造 generation 都是 `-4`。原文见 [verification.md](verification.md)。
- **回归**：fast 层 11/11、`byte-adder-smoke`、`custom-or` 的 `multi`（两个 V1 实例）、
  `pin-shape-source`/`sink`/`wide9` 全部 PASS；ABI 基线更新到 1031 条。

### 元件接口 V2：每方向 0–16 脚、自定义状态大小（2026-09-22）

按 [HANDOFF-component-v2.md](HANDOFF-component-v2.md) 一次做完的那一刀，阶段 1 到此收口。

- **新服务 `tc.component.types` V1**（SDK 封装在 `sdk/tc_component_types.h`）：
  `register_definition(TCComponentTypeDefinitionV2*)`。表按 Mod 发放（和命令总线一样），
  因为注册需要调用方的 mod id 与数据目录。返回 `OK / ERR_ARGUMENT / ERR_DUPLICATE /
  ERR_UNSUPPORTED / ERR_GAME / ERR_BUDGET`；被拒绝的定义同样进 `tc.component.registry` 目录。
- **新值模型 `TCLogicIOV2`**：脚数与数组由宿主借用（`inputs`/`outputs`/`state` + `state_words`），
  所以回调能拿到 8 个以上的脚，状态大小也由定义决定，而不是固定的 `inputs[8]/outputs[8]/state[8]`。
  V1 的 `TCLogicIO` 与 `register_component` 一行未动，老 Mod 行为不变。
- **桥接**：`kMaxBridgeInputs/kMaxBridgeOutputs` 提到 16（V1 定义仍被 `supportedScaffold`
  限制在 8 脚），`Binding::state` 改为按定义分配，`invoke`/`reset` 增加 V2 路径。
  `component_definition` 拆出 `validShape()`（形状校验）与 `valid()`（形状 + 回调），
  `encode()` 支持到 16 脚；总输入位宽仍受 128 位 payload 预算约束。
- **离线证据**：`tests/native-component.cpp` 增加 9 脚/16 脚脚手架的节点数、V1 对 9 脚的拒绝、
  128 位通过 / 129 位与 65 位单脚的拒绝。
- **真机证据**：新用例 `pin-shape-wide9`（`tests/pin-shape-playtest.ps1 -Shape wide9`）+
  板子 `nl_wide9board.data`：注册返回 0、实例绑定、编译后的棋盘里 9 个脚/9 个收集门/8 个依赖门/
  1 个驱动门齐全，回调每拍收到 `inputs=9[...]` 并让 `state` 按定义的两个字自增。
- **回归**：`byte-adder-smoke`（V1 声明式）、`custom-or` 的 `shape1`/`shape32`（手写定义）、
  `pin-shape-source`/`pin-shape-sink` 全部照常 PASS；fast 层与 ABI 基线同步更新（985 条）。
- **仍不做**：多字（>64 位）信号（外调只有 4 个参数，且游戏原版上限 64 位）；
  动态引脚、`type_id` 分类/依赖、实例句柄与生命周期属于 M2/M3。

### 元件接口：0 脚元件（纯源 / 纯汇）真机打通（2026-09-22）

按 [PLAN-custom-components.md](../docs/PLAN-custom-components.md) 阶段 1 继续推进。上一份交接把
"N=0（纯源）时谁触发驱动门？M=0（纯汇）时谁来调用回调？"列为必须先有答案的问题；这一轮先测量、
再按结论改接口，测量过程写在 [research/custom-component-pins.md](research/custom-component-pins.md)。

- **测量结论（真机）**：两种形状**都能用"一个悬空的驱动门"解决**。源：0 输入 1 输出，驱动门输入
  悬空，游戏编译后的棋盘里 `node=0x12/parent=<实例>` 仍在（没被剪掉），回调每拍一次；汇：1 输入
  0 输出，尾节点是输出悬空的驱动门，同样被保留、每拍一次。两个形状的导入都返回 0
  （`shape=0in/1out`、`shape=1in/0out`），引脚几何正确。
- **脚手架**（`src/component_definition.hpp`）：`valid()` 允许**一个**方向为空（两边都空仍然拒绝，
  那种元件既不能观察也不能产生）；`encode()` 在 0 输入时不再生成收集门与依赖链、在 0 输出时补一个
  输出悬空的驱动门。节点数公式 `(n ? 3n-1 : 0) + (m ? 2m : 1)`。
- **桥接**（`src/native_logic.hpp`）：`supportedScaffold()` 接受 0 输入或 0 输出；实例节点数期望
  改为 `prefix + outputs + (outputs==0 ? 1 : 0)`，`prefix = inputs ? 2*inputs-1 : 0`
  （旧公式 `2*inputs-1` 在 inputs=0 时于 `size_t` 下溢，会把所有源元件判成"内部电路不认识"）；
  汇的那一个尾节点按"第一个驱动门"发回调。
- **离线证据**：`tests/native-component.cpp` 解出并逐节点断言两种脚手架（源 `[0x12,0x51]`、
  汇 `[0x4f,0x12,0x12]`、三输入汇 9 节点），并覆盖"两边都空要拒绝"。
- **真机证据**：`tests/pin-shape-probe.cpp` + `tests/pin-shape-playtest.ps1`（新用例
  `pin-shape-source` / `pin-shape-sink`），板子 fixture `nl_src0board` / `nl_sink0board`。
  两个用例 PASS：注册 → 绑定 → 每拍一次回调。
- **顺带定下的一条纪律**：0 脚元件几乎不可能满足战役关卡的期望值，而关卡测试一失败游戏就停表
  （实测 `verdict=2`、周期停在第 0/1 拍）。所以真机用例让探针**接管运行**（`simulation.run` 到
  指定周期）再数回调，而不是等关卡测试跑完。
- **>8 脚：游戏侧已测通**（同一探针，`tc_mod_load` 末尾直接构造并导入）：9 / 16 / 32 个输入脚的
  v14 定义全部 `status 0`（`TCComponentStatus::Ok`），ID 依次分配——导入器没有八脚上限。
  所以八脚是**加载器自己的**门槛：`kMaxBridgeInputs` 数组 + 两个 64 位 payload 字的打包
  （**总输入位宽 ≤128** 才是硬约束）。下一步是把数组放大、再用"每脚都接线"的板子做真机运行验证。
- **外调参数宽度的实测与取舍（2026-09-22）**：`TC_MODLOADER_BRIDGE_WIDE=1` 把外调从 4 个参数
  扩成 6 个（追加两个由周期推导、与 payload 不同的字，入口点重算比对）。生成源码确认写对，
  但接收端收到的是垃圾（`0x2eaa7,0x1` / `0x1,0xffffff0000000001`），而 `byte-adder` 仍 PASS
  （额外参数只校验不参与运算）——**四个参数是 JIT 的真实上限**。
  据此定下取舍：每脚 1–64 位（与游戏原版一致）、**不做多字 >64 位信号**、
  **总输入位宽固定 ≤128 位**（脚数可放宽，脚数 × 位宽之和受这个预算约束）。
  代价写清楚：1+64+64 这类元件不在回调接口里做，出口是 M2 的宿主状态/缓冲。
  这条同时解锁 V2 的值模型：每脚一个 `uint64_t` + 宿主借用脚数组，不需要字节视图。
  详见 [research/custom-component-pins.md](research/custom-component-pins.md)。
- **仍未做**：`TCLogicIOV2`（借用脚数组，脚数 >8）+ `TCComponentTypeDefinitionV2` +
  >8 脚的真机运行验证。
- **回归**：`byte-adder`（声明式路径）与 `custom-or` 的 1in/1out 手写定义场景照常通过；
  fast 层 11 项全绿。

### P4：示波器面板 `local.scope`（2026-09-22）

用户要的那个示波器。它不是 waveform-demo 的改皮：数据源换成了 P3 的逐周期采集，
所以快跑时不会跳周期；通道换成 `tc.sim.channel`（板子导线 → 状态字节偏移 + 位宽），
所以采的是 tick 真正读的那块内存。

- **面板**（`examples/scope/plugin.cpp`，`local.scope` 0.1.0，板侧栏）：每条通道一条泳道，
  一位信号画方波、多位网画**数据带**并在跳变处写值；时基 `cycles/px` 可缩放、`<` `>` 平移、
  `Latest` 回到最新；触发（通道 / 上升 / 下降 / 任意沿 / 按值 + 门限）画黄线与 `T` 标记；
  两条游标 A/B（画布上左键点 A、右键点 B），显示 `dt` 与各通道在游标处的值；
  顶部状态行常驻 `rows / gaps / restarts / tick in program`；`Export VCD` 把当前窗口导出。
- **通道来源**：`tc.board` 的对象快照（先 count-only 拿数量再按数量分配——容不下时返回的是
  `ERR_CAPACITY` 且**同时**填好数量，这一点踩过：固定 512 条容量的猜测在这张板子上直接
  拿不到任何通道），再逐条 `tc.sim.channel::fromWire`，最多 12 条泳道，标签写导线两端坐标。
- **真机证据**：`tests/scope-playtest.ps1`（用例 `scope-panel`）——面板注册为板侧栏、
  在真正的板子上解析出 **5 条导线通道**并成功 Arm，随后
  `Scope: first window rows=8 gaps=0 injected=1 lanes=5`，即逐周期采集在真机上有窗口、
  一个周期都没漏。配套 `tests/scope-capture-playtest.ps1`（用例 `scope-capture`）用驱动
  把同一套服务跑到 30 行并逐周期对上关卡自己的 `carry_in/a/b`。
- **每一步都要能看见**：面板把"没有通道"的原因写进日志（没有板句柄 / 板还没导线 /
  快照失败 + 状态码 / 导线没有状态槽），并把 `board@… components=N wires=M` 的变化记一行——
  否则"面板空着"和"服务被拒绝"在日志里长得一模一样。

### 仿真接口收拢成服务：`tc.simulation` V2 + `tc.sim.channel`（2026-09-21）

"先把仿真这层封装好，方便以后开发"——以前这条线散在五个地方（V1 服务只读、命令总线、
五个符号别名、每个 Mod 自己挖偏移、`tc_trace` 的渲染线程采样），现在收成两个服务：

- **`tc.simulation` V2**（V1 前缀原样保留）：`cycle`/`state_size`、`snapshot`（一次读一批
  通道，并回报这次读的周期与 `stable`——读的过程中周期变了就为 0，调用方重读）、
  `run_to`/`run_for`/`step`/`pause`/`reset`（**请求**，走游戏自己的 `sim.do`，因此
  `cycle-guard` 之类的链节照样生效）、`set_slice`（每次运行请求最多推进 N 周期）、
  `control`（请求数、被切短的次数、最近一次的目标与实际生效值）。
- **切片归属加载器**：`sim.do` 的加载器链节现在**总是**安装——它记住仿真模型（V2 的请求
  没有别的办法指名仿真实例）并执行切片；没订阅事件的 Mod 也不会因此多付什么，事件派发在
  没人听时是空的。日志新增一行 `Simulation control sim.do: ok`。
- **`tc.sim.channel` V1**：`from_wire`（板子快照给的导线句柄 → `{字节偏移, 位宽, wire_id,
  两端坐标}`）与 `resolve`（重编译后原地重解析：先按 wire id、再按两端坐标；失活的通道保留
  id、`bits=0`，画成灰的而不是继续瞎读旧偏移）。
- **SDK**：`sdk/tc_simulation.h` 重写为服务层（`tc::simulation::table/cycle/sample/runFor/…`），
  老的 `TCSimulationModel` 保留但改成走**别名表**（`sim.do`/`sim.cycle`/`sim.settings`/
  `sim.setting.get|set`），只有还没有别名的回放缓冲仍按名字取；新增 `sdk/tc_sim_channel.h`。
- **离线证据**：`tests/simulation.cpp` 扩了 V2 与通道服务——表的版本/大小、快照的容量与越界、
  未绑定时返回 `UNAVAILABLE` 而不是野值、`clampTarget` 切片算术（0 切片不切、切片边界、
  重置目标不切、溢出不回绕）。
- **P3（逐周期采集）按"生成源码注入 tick"做**：和原生逻辑桥同一套注入手法（`tc_scope_tick`
  插在周期循环里），这样不论谁在驱动仿真（玩家运行、关卡自带测试）都不漏周期；切片机制保留，
  供不需要注入的场合使用。

### 引脚顺序：整组手柄在"非 2 的幂"条数下消失（2026-09-21）

玩家报的规律是"1、2 有方框，3 消失，4 回来，5–7 消失，8 回来……16 回来"——**只有 2 的幂
才有**。这正好是 Nim 序列容量翻倍的序列，日志也对上了：

```
inputs cache=1 rows=1  drawn=1     ← 1 个引脚
inputs cache=2 rows=2  drawn=2     ← 2 个
inputs cache=0 rows=2  drawn=2     ← 第 3 个：条数读成 0，整组不画
```

- **原因**：面板缓存的记录布局是 descriptor `{count, payload}`，条目从 `payload+8` 开始、每条
  32 字节。加载器当初把 `payload` 开头那个字当成"条数重复一遍"并**要求它与 `count` 相等**
  （那是关卡面板的形状：列表一次性 `newSeq` 好，两者相等）。但面板一条条长出来的列表
  （元件工坊、手动加输入器件的板面）里，那个字是**块的容量**，按 1、2、4、8、16 翻倍，
  所以条数不是 2 的幂时两者不相等 → `viewOf` 判为"不是这个结构" → 整组 `entry()` 全部失败 →
  Mod 一个手柄都没有 ✓ 与玩家观察逐条吻合。
- **修法**：描述符的 `count` 就是条目数（面板按它画），块开头的字只当**上界**校验
  （`容量 ≥ 条数`，且两者都不超过 4096）。`readable()` 的长度也按 `count` 算。
  离线用例新增"3 条记录、容量 4 的列表"：`count()` 读出 3、取第 3 条、整组置换都通过；
  块字小于条数的结构仍然被拒。
- **顺带**：加载器的原始结构转储（`pin order dump: group … count=… header=…`）改成**默认打印**
  且只在结构变化时打印（一次会话最多 24 段）——这种"缓存条数与面板画出来的条目数不一致"的
  问题，靠玩家手动设环境变量才留证据，证据就丢了。
- **证据**：玩家实机日志（上面三行）、fast 用例 `tests/pin-order.ps1` 的新场景、真机
  `pin-order-playtest.ps1 -Drag`（现在的驱动点按钮）`Order: 2,3,4 -> 3,2,4`。

### 引脚顺序：拖动改成 ▲/▼ 按钮（`local.pin-order` 0.4.0，2026-09-21）

"拖动难以实现、bug 太多"——玩家给的方案是换成按钮，就按这个做：

- **每条引脚左边一对小按钮**：▲ 上移一格、▼ 下移一格，左键点一下即生效（`tc.pin_order.move`
  的 `from`/`to` 两个位置，越界自动夹到组的两端）。
- **右键按钮打开"移动格数"窗口**：滑杆 1–32 +「移动 N 格」按钮，同页还有「恢复面板顺序」；
  窗口按引脚 **key** 记录目标，所以放着的这几帧里条目换了位置也不会移错（`indexOfKey` 每帧回查）。
  Ctrl+左键点按钮仍是"恢复该组原顺序"。
- **拖动整段删除**：`dragging/dragGroup/dragFrom/dropIndex/dragKey/targetKey`、
  `dropIndexFor`（按鼠标 Y 找落点）、`keyAtIndex` 与拖动高亮全部删掉——按钮的命中判定是
  自己算的矩形，不依赖"绘制有没有被裁掉"，也就不会再出现"看不见手柄但拖动还在生效"这类错位。
- **绘制**：原来是"左侧抓取条 + 3 个圆点"，现在是两个 15×15 的按钮（圆角底 + 三角），悬停的那个
  变亮；位置以条目**标签行**为基准上下各一个，所以条目高矮都不影响按钮可见。
- **真机证据**：`tests/pin-order-playtest.ps1 -Drag`（这条用例的"驱动"现在改为**点按钮**）：
  日志 `pin order: moved inputs #0 to #1 (status 0)`、驱动 `order inputs before=[2,3,4] after=[3,2,4]`，
  用例断言"前两条互换"通过；截图 `build/pin-order-out/btn1.png` 里每条左边都能看到 ▲/▼。

### 新 Mod：板面网格 + 设置页开关（`local.board-grid` 0.1.0，2026-09-21）

需求："游戏本身已经有网格了，只是看不见，希望画出来，并在游戏设置面板里加一项开关。"

- **网格是什么**：`create_grid__modelZboardZboard_u120`（RVA 0x141390）是游戏的板面格子——它给
  `compute_jps_cache` 的寻路用（`memset 0xc800` 的格子数组），板面坐标本身是整数格：引脚相隔
  1 个单位（`AND` 的脚在 `(-1,±1)`、`(2,0)`），导线在整数坐标拐弯，元件摆在整数格上。游戏只画
  背景花纹，不画这些线。
- **怎么画**：渲染侧不用反推相机——`world_pos_to_screen_pos__..._u1155`
  （别名 `board.world_to_screen`）读全局 `ubo_view_model` 里的相机，返回**归一化**屏幕坐标；
  游戏自己的 `draw_simple_rect`（VA 0x1403404f0，板面框选矩形）就是把这个结果乘
  `ImGuiIO.DisplaySize`（`io+8`）再 `ImDrawList_AddRect`。Mod 用三个探测点（原点、+1 单位 x、
  +1 单位 y）量出仿射变换，求逆得到"屏幕四角覆盖哪一段板面坐标"，然后把每一条整数格线画进
  **主视口的 background draw list**——也就是游戏自己画框选矩形的那一层：盖在板面之上、所有
  面板之下。
- **开关在游戏自己的设置页**：新增别名 `options.general` →
  `build_general_options__presenterZmain95menu95uiZoptionsZgeneral95tab_u339`，Mod 钩它、调用原函数后
  追加一行复选框「显示网格 (board grid)」；状态写在 Mod 自己的数据目录
  （`plugin-data/local.board-grid/board-grid.txt`），不碰游戏设置文件。
- **视觉**：单位格线 alpha 22，每 4 格一条 alpha 46 的线，线宽 1 px；网格只在"游戏认为已加载
  关卡"时出现。
- **证据**：`Symbol profile: 32/32 aliases resolved`、`[local.board-grid] board grid: the Options page
  carries the switch`、`Native loaded: local.board-grid; hooks=1`；隔离沙箱实机截图
  `build/pin-label-out/gridfix1.png` 里网格线与导线、元件位置对齐（导线正好压在格线上）。
- **顺手修的**：`level.loaded` 在符号表里被标成"函数"，其实它是**关卡名的 Nim 字符串数据**；
  文档与 `level.loaded` 的类型说明一并更正——照着旧说明当函数调用会立刻崩在字符串头上。

### 工坊里改引脚后左侧面板不同步：轮子归加载器（2026-09-21）

用户报的两个现象是同一件事："某些数量的引脚不会出现可移动方框" 与 "引脚的标签更改后，
左侧面板不会同时刷新"，两者都要**退出元件工坊再重进**才恢复。

- **先查清缓存是谁在什么时候建的**：`objdump -d` 找 `get_io_states` 的调用者，全 EXE 只有
  一处——`load_level_frontend`（0x316bc6，函数 0x315d50–0x317be0）。也就是说左侧面板那三段
  缓存只在**关卡加载**时重建，工坊里改名/增删引脚都不会走到它，所以"退出再进"是唯一的刷新方式。
  这也解释了"新增的引脚没有方框"：面板画的是旧缓存，新引脚根本不在里面。
- **原修复被自己人挡掉了**：`local.pin-names-patch` 一直负责这件事，它钩 `reload_custom_prototype`
  和 `build_io_state_view` 两个函数；后者在同一版加载器里已经被 `tc.pin_order` 拥有
  （`TC_PIN_ORDER_BUILD_VIEW_RVA` 与 `TC_BUILD_IO_STATE_VIEW_RVA` 都是 0x45ea20），MinHook 对
  同一目标第二次 `MH_CreateHook` 返回 `MH_ERROR_ALREADY_CREATED`，插件整包被拒。用户
  `D:\p` 的 loader.log 里就是这三行：`MH_ERROR_ALREADY_CREATED`、
  `IO state label live refresh unavailable (reload=ok, view=failed)`、
  `Native failed: local.pin-names-patch`。所以引脚编号表也跟着一起消失了。
- **修法：这两件事都归加载器**，因为吃这份缓存的不只是某个 Mod（面板自己、引脚顺序功能
  都吃它），刷新不该取决于玩家启用哪个包。加载器现在自己钩 `reload_this_custom_prototype`
  （0x2e8cd0，同一个已被兼容画像钉住的 RVA），记下"原型重载过"，在 `build_io_state_view`
  入口用游戏自己的 `get_io_states` + 托管 copy/destroy 重建三段缓存，**然后**才套用 Mod 的
  引脚顺序——顺序因此永远作用在刚建好的缓存上，面板与 Mod 看到的是同一份记录。
  两个 detour 合成一个（`hookBuildIoStateView`），standalone 补丁与完整加载器共用它。
- **`local.pin-names-patch` 0.0.4**：不再钩那两个目标（它只留预览侧的 `igText` 与
  `ImDrawList_AddRectFilled`），因此不再被拒；编号表与白条回来了。它的 `report_status`
  现在只说编号表。
- **顺带修掉一个会让整组手柄一起消失的读取缺陷**：`src/pin_order.hpp` 的 `readable()` 原来要求
  `[address, address+bytes)` 落在**同一个** `VirtualQuery` 区域里，而 presenter context 约
  56 KB，很容易横跨两个区域（新分配、堆块刚好跨区、尾部改过保护）。那时对每一组都回答"面板不在
  屏幕上"：没有可排序的条目，画手柄的 Mod 也就一个框都不画——症状同样是"退出再进才好"。
  现在按区域步进（每步用区域自己的 BaseAddress+RegionSize 前进，不前进就拒绝），只有真正
  不可读的页才拒绝。离线用例 `tests/pin-order.cpp` 用两页不同保护（=两个区域）钉住这一点。
- **实机证据**（`tests/live-root-workshop.ps1`，D:\p 实机 + 当前存档 `foundry/2`）：
  加载器日志 `IO panel armed (cache rebuild after a prototype reload + tc.pin_order)`；
  `Native loaded: local.pin-names-patch; hooks=2`（不再 `Native failed`）；
  引脚顺序 Mod 的逐帧摘要 `inputs cache=4 rows=4 frames=4 | outputs cache=2 rows=2 frames=2`
  ——四个输入、两个输出都有框。真机改引脚那一步仍要玩家手动做：改名后 loader.log 里会出现
  `IO panel cache rebuilt (a custom prototype was reloaded)`。
- **`local.pin-order` 0.3.4**：新增两条开发用诊断（`TC_MODLOADER_PIN_ORDER_LOG=1`）：每一帧
  汇总一次"每组缓存条数 / 画了几条 / 画了几个框"（只在变化时打印，行首是 `pin order: summary`），
  以及面板里首次出现的、Mod 还不认识的锚点偏移（`panel anchor +0x… is not handled`，工坊第三组
  就是这一类）。"某个引脚的框不见了"从此可以直接从日志读出是缓存少了、锚点没认出来，还是框被
  裁掉了。逐条目那行（`pin order: inputs #0 key=… screen=(…)`）从"每个进程最多 16 行"改成
  **变化才打印**：它是 `tests/pin-order-driver.cpp` 用来瞄准真机拖动的数据源，而宽引脚自己就会
  打掉好几行方块日志，旧上限恰好会在条目中间用尽——真机 `-Drag` 因此一直没拖成。
- **真机 `-Drag` 用例修好自己的关卡**：`tests/pin-order-playtest.ps1` 默认的
  `symphony_8_io_devices` 只有 1 进 1 出（面板没东西可拖），现在 `-Drag` 自动换成
  `byte_adder`（3 进 2 出），并支持 `-PinLog 1|dump`。这条用例在改动前后都是同一个失败原因
  （旧加载器同样失败），只是现在真的能跑了：日志
  `drag inputs #0 -> #2; before=2,3,4`、`moved inputs #0 (key 2) to #2 (status 0)`、
  `order inputs before=[2,3,4] after=[3,4,2]`。

### 输出标题与纸带重叠 + 最后一框底边被裁（2026-09-21）

实机自定义面板里出现两种剩余布局缺口：`掩码层…` 与“输出状态”叠在同一行，且最后一个输出
（如 `输出 1`）的 bit 可见、边框底边却在子窗口外。

- `local.pin-order` 0.3.3：此前蓝框只知道 bit 所在的第二行，却把“半行距离”当成 bit 自身高度。
  多 bit 时控件很小，误差不显眼；1-bit 控件会放大到约 47 px，蓝框因此从红色 bit 中间穿过。
  2–8 bit 仍在四个光标复位调用点读取 `igGetItemRectSize`；单 bit 不走这些调用点，改在每个条目
  结束的 `igSetCursorPosY` 锚点读取仍为 `LastItem` 的真实控件矩形，并把
  `bit top + 实际高度 + 4 px` 纳入条目下沿。自定义端口原始位宽 `0` 表示自动位宽，和字面值 `1`
  一并走这条路径。宽位数值行及其他 Mod 通过 V2 汇报的矩形继续取并集。`D:\p` 实机高亮回归图
  `build/live-root-pin-order-aa-contained-final2-033-*/out/workshop-live.png` 中，`aa` 的蓝框与
  `输出 1` 的默认框均完整包住红色 bit。
- `local.punch-tape` 0.9.10：真正的 `D:\p` 实机回归发现，333×615 的高工坊先用
  `igSetCursorPosY` 移动绝对行锚点，随后 `igSetCursorPos` 会复用**已经移动过的 Y**来放标签与
  bit；旧逻辑又给后者叠加一次累计高度。两张 64-bit 纸带时 245 px 偏移因此被重复计算，第二输入
  与输出条目被推得过远，而“输出状态”留在掩码行附近。现在高工坊只给标量绝对锚点加偏移，
  完整坐标调用继承其 Y；紧凑关卡面板仍按原路径处理。实机脚本
  `tests/live-root-workshop.ps1` 直接运行 `D:\p\Turing Complete.exe`，按“开始游戏 → 元件树 →
  元件工坊”进入当前存档 `foundry/2`；截图 `build/live-root-foundry-scalar-anchor-fix-*/out/`
  显示“输出状态”独占一行。这个版本只修正内容的位置；单 bit 蓝框的真实控件高度由随后
  `local.pin-order` 0.3.3 修正。
- `local.punch-tape` 0.9.9：面板的标题辅助代码不只在主 EXE，也可能来自重命名后的原始
  `tc_game_engine.dll`。锚点过滤现在接受这两个游戏自有 PE 范围，仍拒绝 Loader/Mod DLL，
  因此标题与输出条目获得相同的累计让位，而 Mod 自己的游标不会被二次移动。
- `local.pin-order` 0.3.2：边框是 draw-list 覆盖层，不会更新 ImGui `CursorMaxPos`；最后一条
  bit 虽然是正常 item，边框下方的 padding 不是，滚动范围因此提前结束。现在用上一完成帧的
  最低本地 Y 提交一个 1×1 `igDummy` 哨兵，再恢复游戏游标；它只扩展内容/滚动范围，不抢鼠标，
  最后一框的底边可以完整滚入视口。

### 最后一条单比特输出的边框底边跑出面板（`local.pin-order` 0.3.1，2026-09-21）

`Carry out` 这类单比特输出本身很大，但截图里真正丢失的是边框底边：它是输出组最后一条，后面
没有新标签来结束当前 `Row`；而旧代码虽然列出了四个 bit-square 调用点，采集时却把面板内**所有**
非标签 `igSetCursorPos` 都写进当前条目的 `squaresY`。后续面板布局因此不断把最后一条的“方块中心”
往下推，最终把底边算到子窗口之外。现在只有已实测的 `+0x21a9/+0x28c8/+0x295d/+0x3257`
四个调用点能更新 `squaresY`；其他绝对定位与该条目无关，不再污染最后一条。V2 内容边界协议保持
不变，它解决 Mod 插入控件的范围；本修复解决游戏原生 bit 方块的调用点归属。

### 引脚条目边界提升到 Loader：`tc.pin_order` V2（2026-09-21）

`local.pin-order` 的外框过去只知道面板的标签/位方块/下一条光标锚点，并不知道别的 Mod 在条目
内部加了哪些控件；最后一条没有下一条可量，靠节奏与调用点猜高度必然会漏。现在 Loader 的
`tc.pin_order` V2 增加按 `{frame, group, key}` 聚合的条目矩形：内容 Mod 用 `include_bounds`
汇报真实屏幕范围，容器用 `bounds` 读取所有贡献的并集，最近三个布局帧由 Loader 保留。

- `local.punch-tape` 0.9.8 在游戏画完随纸带下移的原生数值框、下一锚点尚未生效的时刻，汇报
  整条内部内容的最终范围；
- `local.pin-order` 0.3.0 把该范围并入 `rowFrame`，包括同组最后一条，不再要求存在下一条目；
- V1 排序表仍原样可查，V2 保留完整 V1 前缀；fast 测试覆盖矩形并集、上一帧读取、过期清理、
  V1/V2 兼容以及 `NOT_FOUND`。

### 工坊面板标题错位 + 最后一条把手不贴合内容（2026-09-21）

用户截图：宽输入条目被纸带撑高后，"输出状态"标题压在"掩码层"那一行上；最下面一条的把手框
明显比内容高。

- **标题（`local.punch-tape` 0.9.7）**：让位逻辑原来只认"调用来自 `build_io_state_view` 的代码"。
  关卡面板的标题确实是面板自己放的，但**工坊面板的标题由辅助函数画**——它的返回地址落在面板函数
  之外，于是那一行没被下移，而纸带把内容推下去之后它就压在掩码行上了。现在锚点的接受条件放宽成
  "**面板的子窗口当前正在绘制，且调用来自游戏主程序**"（Mod 自己的调用不算，否则纸带自身的落点会
  被二次移动）。新增开发开关 `TC_MODLOADER_PUNCH_TAPE_LOG=1`（打每个锚点的来源/原值/偏移/终值）。
  真机数据（`symphony_2_io`，`build/pin-order-probe-b/…/loader.log`）：关卡面板里
  `+0x4d2 asked=67 offset=0`、`+0x6d4 asked=67 offset=0`、`+0xd92 asked=96 offset=91 got=187`、
  `+0x1680 asked=149 offset=91 got=240`、`+0x1836 asked=240 offset=91 got=331`——面板内每个锚点
  等量下移、没有重复计数。
- **把手框（`local.pin-order` 0.2.2）**：贴合逻辑里有一条"内容被别的 Mod 撑高时按面板给下一条
  留的位置撑满"的分支。对**最后一条**来说，它后面的不是下一条目，而是面板自己的尾部，于是框被
  撑高（截图里"输出 1"那条框比内容高约一倍）。现在只有"后面还有同组条目"时才用这条分支。
  截图 `build/pin-order-out/fix2-panel.png`（修好后的 `byte_adder`：`Carry out` 的框贴合标签+方块）。

### 元件接口（阶段 1 前半）：类型目录 `tc.component.registry`（2026-09-21）

按 [完整自定义元件接口目标计划书](PLAN-custom-components.md) 的阶段 1 取出的第一块，只做**只读目录**：
引脚/信号可变长那半（0 脚、>8 脚、>64 位）留在下一块，理由与待解问题写在
[HANDOFF-component-registry.md](HANDOFF-component-registry.md)。

- **新增 `TC_SERVICE_COMPONENT_REGISTRY`（`tc.component.registry`）**：两条注册路径
  （`register_logic`、`register_component`）登记的每个类型都进目录，被拒绝的也在，并带
  **第一条**拒绝原因（桥接那句"最多每方向八脚、每脚 1–64 位、总输入 ≤128 位"不会被后续更笼统的
  原因盖掉）。查询入口：`count` / `get` / `find`（按 `custom_id`）/ `pin`（引脚名、位宽、`pin_id`）。
- **声明与桥接的合流**：声明式注册登记的引脚名会保留，桥接时从游戏原型读到的位宽覆盖声明值，
  所以目录里的 `bits` 是游戏真正编译出来的形状。能力位只报告加载器能自行判断的
  `LOGIC` / `WIDE_PIN` / `MULTI_PIN`；计划里其余能力等类型声明阶段。
- **拒绝的 Mod 不留痕**：插件被拒绝时它的类型从目录里删除（与 Hook、界面一致）。
- **SDK**：`sdk/tc_component_registry.h`（header-only 包装）与 `TCComponentTypeInfoV1` /
  `TCComponentPinInfoV1` / `TCComponentRegistryApiV1`；`docs/sdk/services.md` 与
  `docs/reference/limits.md` 增加对应小节。
- **示例**：`examples/mod-inspector` 增加 "component types" 一节（列出类型、形状、代价，
  被拒绝的显示原因）。
- **测试**：fast 层 `tests/component-registry.ps1`（目录语义、拒绝原因、容量、服务表形状）；
  game 层 `tests/component-registry-playtest.ps1`（真机：拒绝项带原因 + `example.byte-adder`
  的 `3in/2out cost=1 delay=1 active=1 caps=0x7` 与五根引脚的声明名与位宽）。ABI 基线随新增
  结构更新。

### 打孔纸带压在"输出状态"上的问题（关卡面板）＋ 锚点提升为钩子链（2026-09-21）

- **`local.punch-tape` 0.9.6**：纸带与"掩码层"按钮从面板左侧那一列之后开始排（`kEntryHandleInset`
  = 20 px，居中范围也相应右移）。面板左边缘是面板自己控件的位置——引脚顺序插件的拖动条就画在
  那里（边框 6 px、抓取条到 13 px、命中到 25 px）——窄面板里宽度接近面板的纸带原本会顶回那一列，
  现在留出来了。截图 `build/pin-order-out/inset1.png`。

- **现象**：在**关卡**面板里，宽输入的纸带（含"掩码层"行）会压住下面的"输出状态"标题与
  输出列表；元件工坊里却正常。
- **原因**：纸带的让位靠移动面板摆放每一行的两个 `igSetCursorPos(Y)` 调用。工坊面板用
  标量那个锚条目，所以推得动；关卡面板的标签用 `igSetCursorPos`（绝对 Y，从
  `igGetCursorPosY()` 读出来再整体设置），只动标量锚点时，标题动了、带着纸带的那一条没动。
- **修法（Loader 层）**：把 `igSetCursorPos` 与 `igSetCursorPosY` 提升为**钩子链**点
  （`TC_HOOK_SET_CURSOR_POS` / `TC_HOOK_SET_CURSOR_POS_Y`），`TCHookCall` 追加一个
  `caller`（游戏那次调用的返回地址），链节可以改 `x`/`y`。这样"谁在这个面板里改布局/画东
  西"不再是谁抢到钩子的问题：
  - `local.punch-tape` 0.9.5：两个锚点都改（按 `caller` 判断是不是这个面板），关卡与工坊
    面板都会把纸带下方整体推下去；
  - `local.pin-order` 0.2.1：改为加入这两条链（优先级 100，晚于纸带的 0），从链里读锚点，
    不再自己钩引擎函数；纸带撑高的条目，它的把手也跟着变高。
- **测试**：`tests/hook-chain.ps1` 新增 `cursor` 场景（`tests/hook-chain.cpp` 用假面板调用
  两个引擎函数：链给出的 caller 必须认得出面板、链节改的 y 必须送到游戏函数、面板以外的调用
  不许被改）。SDK ABI 基线随之更新（`TCHookCall` 56 → 64 字节，纯追加）。
- **真机验收**：`tests/pin-order-playtest.ps1 -Level symphony_8_io_devices -ExtraMods local.punch-tape`
  截图 `build/pin-order-out/chain1.png`：纸带 + 掩码层 + 数值框都在把手里，`输出状态` 标题
  在其下方；`tests/punchcard-playtest.ps1 -PunchTape` 与 `tests/pin-order-playtest.ps1 -Drag`
  同时回归通过（顺序 `2,3,4 -> 3,4,2`）。

### 引脚顺序：`tc.pin_order` 服务 + `local.pin-order`（2026-09-21）

- **需求**：游戏左侧"输入状态/输出状态"面板的顺序是面板自己缓存的次序，玩家改不了；
  玩家希望能拖动面板上的引脚重新排列。
- **底层（Loader）**：新增 `TC_SERVICE_PIN_ORDER`（`tc.pin_order`）与 `src/pin_order.hpp`。
  面板画的不是电路板，而是 presenter context 里三段缓存序列（`io_state_cache.hpp` 的偏移），
  所以"顺序"就是这些元素的前后次序：服务用 `count`/`entry` 描述面板现在显示什么，用
  `move`/`set_order`/`reset` 接受新顺序，并把顺序**在 `build_io_state_view` 入口、面板读缓存
  之前**重排进活缓存——只动缓存、不动电路，值输入框照常可用。顺序存的是 **key（元件下标）**
  而不是位置，因此重命名触发的缓存重建会重新套用，换成另一组引脚时自动失效。
  记录布局（descriptor `{count,payload}`、payload 头部的条数、32 字节记录
  `{元件下标, 位宽, 名字{长度, 数据}}`）由 `TC_MODLOADER_PIN_ORDER_LOG=dump` 在真机上打印，
  对不上时服务选择不动作。
- **SDK**：`sdk/tc_pin_order.h`（header-only 包装）、`TCPinOrderApiV1` 与
  `TCPinOrderEntryV1` 写进 `sdk/tc_service_api.h`；`docs/sdk/services.md` 增加 `tc.pin_order` 一节。
- **Mod（`local.pin-order` 0.2.0）**：给面板每个条目画细边框 + 左侧抓取条（悬停高亮），
  按住抓取条上下拖动即把该引脚放到落点条目的位置，中间的条目依次让位；拖动时目标条目高亮、
  被拖条目描边加粗；Ctrl+点击抓取条恢复该组原本的顺序。几何不是猜比例：面板用
  `igSetCursorPos` 逐条摆放标签与位方块，Mod 钩住这次调用，用"标签锚点"认出条目、用同一
  条目里第二行（位方块）的锚点决定边框下沿，所以边框跟着面板而不是写死高度；绘制走
  draw-list 覆盖层（不提交控件），鼠标只在抓取条那 19 px 内被响应，值输入框的点击与输入不受影响。
- **测试**：fast 层 `tests/pin-order.ps1`（`tests/pin-order.cpp`：用与游戏同形状的假缓存验证
  服务的六个入口，以及"同组重建保留 / 换组失效"两条规则）；game 层
  `tests/pin-order-playtest.ps1`（`-Probe` 打印锚点与服务回答、`-Drag` 用真实鼠标消息拖一次
  并回读顺序、`-Dump` 打印原始记录）。ABI 快照（`tools/abi-snapshot.cpp` /
  `abi/windows-x64.json`）与 `tests/test-catalog.json` 同步更新。
- **验收**：真机 `-Drag`（关卡 `byte_adder`）报告 `Order: 2,3,4 -> 3,4,2`——拖动把第一个引脚
  放到了最后，面板重新绘制后顺序确实是新的；截图 `build/pin-order-out/geo3.png`、`geo5.png`
  （`byte_adder` 与 32 位引脚的 `symphony_8_io_devices`）确认边框贴合"标签 + 位方块 + 数值"。

- **顶部菜单栏插槽 `TC_UI_SLOT_BOARD_MENU`**（2026-09-21）。把"往电路板顶栏挂一个控件"做成
  与侧栏/工具列同一套机制：`tc::ui::registerMenuBarItem(slot_id, draw)` 注册，宿主负责位置、
  样式作用域、ID 隔离、顺序与上限。注入点是 `build_buttons__presenterZboard95uiZmenu95bar_u187`
  清样式作用域时的第一个**真正执行**的弹出（`igPopStyleColor`，RVA `0x45a471`）：探针
  `TC_MODLOADER_LOG_MENU=1` 在两个关卡实测到这一步，而更早的 `0x45a435` 属于可选的"返回关卡"
  按钮标签、普通关卡不执行（第一版就栽在这里：控件注册成功但从不绘制）。绘制在弹出**之前**
  发生，因此 `Button/ButtonHovered/ButtonActive` 三色仍在作用域内，插件用普通 `tc::ui::button()`
  就是顶栏自己的观感；`igSameLine(0,-1)` 把控件接在同一行。示例：`examples/board-panel` 额外注册
  了 `Ping (bar)`；真机截图 `build/menu-bar-out/bar7-top.png` 里它紧跟游戏自己的灯泡按钮，
  日志 `Board menu item example.board-panel/bar drawn in the game's top menu bar (frame 140)`。
  兼容性：新增 RVA 进 `compat/profiles.json`（`-Update` 流程），`TCHost` 与既有服务表未动。
  另外 `Canvas::clicked()` 现在只报告鼠标按下边沿（见上一条同类修复），`tests/ui-draw.cpp`
  增加了"按住不算点击"的断言。

- **被窗口盖住的道具按钮不再响应 hover / 点击**（2026-09-21，玩家反馈）。症状：打孔纸带的掩码窗口压
  在导线调色盘的工具按钮上，鼠标移到被压住的位置，调色盘仍然会被打开。根因在 SDK：
  `sdk/tc_ui_tool.h` 的 `ToolTile` 为了"自己的弹窗盖住自己时不抖动"，把 hover 写成**纯几何**判定
  （只看鼠标在不在 80×80 方块里），完全不问 ImGui 这个位置现在归谁，于是任何画在它上面的窗口都挡不住；
  另外 `sdk/tc_ui_draw.h` 的 `Canvas::clicked()` 直接返回引擎 `igInvisibleButton` 的结果，而这个引擎
  在**按住期间每帧都返回真**（上一轮"点一次纸带闪很多下"就是它），对所有用 SDK 的 Mod 都是同一个坑。
  修复（都在 modloader 层，两个 Mod 不用各自打补丁）：
  - `ToolTile` 的 hover 追加 `ui::isWindowHovered(0)`：鼠标必须在**它自己的窗口**上、且上方没有别的
    窗口遮挡；点击也要求 hover 成立（`clicked_ = hovered_ && canvas_.clicked()`）。游戏自己的对话框、
    别的 Mod 的面板现在都会正常吃掉这个 hover。
  - `Canvas::clicked()` 改成鼠标按下的**边沿**（`pressed && ui::isMouseClicked(0)`），一次物理点击只算
    一次。
  验证：`tests/wire-palette-tool-playtest.ps1`（真机 hover 驱动 + 进程内截图
  `build/wire-palette-tool-out/hoverfix.png`）确认道具在自身窗口可 hover 时照常打开；打孔纸带与掩码层
  用例同时回归。已重新打包并部署 `local.wire-palette` 与 `local.punch-tape`（两者的 `.mod` 都重新构建，
  因为改动在它们共用的 SDK 头里）。

- **新增 `TC_SERVICE_IO_VALUE`（`tc.io_value`）**：把"游戏自己的数值编辑器"那一层收成通用接口，
  供掩码等后续功能使用。表内含 `evaluate`（表达式求值）、`format_value`（按
  二进制/十六进制/无符号/有符号格式化）、`read_input` / `write_input` / `flip_input` /
  `input_width`（按板上元件下标读写全局输入、原生翻转、读声明位宽）、`write_constant`
  （常量完整写入口：设置值 + 运行时值槽 + 刷新，不重编译）与 `write_constant_slot`
  （高级：只要运行时槽）。写值一律按引脚位宽截断；除槽入口外都要求游戏主线程，逻辑线程调用
  返回 `TC_IO_VALUE_ERR_THREAD`。
- **符号画像新增别名**：`io.input.get` / `io.input.set` / `io.input.flip` / `io.constant.set` /
  `io.constant.refresh`（把 `get/set/flip_component_global_input`、`set_setting`、
  `sim_stop_and_refresh` 收进画像，Mod 不再硬编码 Nim 乱名）。
- **SDK**：新增 `sdk/tc_io_value.h`（header-only 包装，含 `truncate`）与
  `tc::ioValueService(host, &api)`；`docs/sdk/services.md` 增加 `tc.io_value` 一节
  （含"为什么 V1 还没调游戏内部 `evaluate`"的说明：它按引用收 Nim 字符串、靠 Nim 错误标志上报，
  Loader 的异常守卫兜不住，要先用探针确认调用形状）。
- **兼容性**：`TCHost` 布局未动（服务表本来就不进 host 结构），`abi/windows-x64.json` 本次为纯
  新增——顺带补上了服务机制引入后一直没刷新的 `sizeof.TCHost=208` 与
  `offsetof.TCHost.query_service=200`，以及此前遗漏的 `TC_HANDLE_ERR_STALE` 记录。
- **测试**：新增 fast 层用例 `tests/io-value-service.ps1`（解析器断言表、位宽截断、服务表形状），
  已挂进 `tests/test-catalog.json`；真机端到端验收随第一个使用方（打孔纸带掩码）一起跑。
- **验收与部署**（2026-09-21）：fast 层 `io-value-service` / `punch-tape-layout` /
  `compatibility-contract` / `sdk-abi` / `save-isolation` / `release-contract` 全通过；真机
  沙盒（`punchcard-playtest -PunchTape`，symphony_2_io）日志
  `Symbol profile: 28/28 aliases resolved`，打孔纸带行为与 0.8.5 一致
  （`grid=313x39`、`entry pushed down by 45 px`）。`dist\tc-loader.dll`
  （SHA-256 `0C2EDDE2…`）已部署到 `D:\p\game_engine.dll`，旧文件备份为
  `dist\tc-loader-before-io-value.dll`（回滚：复制回去覆盖即可）。

## 未发布 — 打孔纸带（`local.punch-tape` 0.7.0 → 0.9.3）

- **0.9.4：去掉掩码的"悬停提示"**（2026-09-21，玩家反馈）。0.9.2 在纸带上方画过一行
  "bit N / 当前值 / 掩码" 的提示（鼠标停在某一格时出现在画布里）。玩家认为没必要，已整块删除：
  现在悬停只保留游戏原版图块自己的高亮帧，`drawGrid` 不再往画布里写任何文字。掩码窗口里
  仍然一直显示 `打孔数据` 与 `电路实际得到` 两行，信息一条不少。

- **0.9.3：掩码改成"一层持续作用的数据变换"**（2026-09-21，玩家澄清）。
  玩家要的是：**掩码位选好之后，纸带上的打孔设置不变，但电路实际拿到的是掩码处理过的值**。
  0.9.2 的"一次性应用"不是这个意思，所以这一版把掩码做成每引脚持久的一层：

  ```text
  纸带 / 数值框  = 打孔数据（你设置的值）
  电路读到的     = 类型(打孔数据, 掩码)        AND v&m / OR v|m / XOR v^m / ANDNOT v&~m / 赋值 v=m
  ```

  实现只用了游戏自己的两个调用入口（都是它原本就在用的）：
  - **读**：面板取引脚值走 `get_component_global_input`。插件早就在这个 UI 调用点上挂过钩子，
    现在当该引脚启用了掩码层时，把**打孔数据**交给面板，而游戏自己的存储保留变换后的值。
    没启用掩码的引脚完全不受影响（钩子只在启用时覆盖）。
  - **写**：任何改变引脚值的路径（原生数值框、插件自己）都走
    `set_component_global_input`。新增的钩子把它解释为"新的打孔数据"：记下来，再把
    `类型(打孔数据, 掩码)` 交给游戏；重入标志保证插件自己的写入不会被二次变换。
  - **常量**沿用宽位常量已有的两个存储：元件的**设置值**放打孔数据（抽屉显示的就是它），
    **运行时值槽**放变换后的值（已编译电路读的就是它），因此不需要重编译。
- 工具窗跟着改成"层"的形式：`打孔数据 …` / `电路实际得到 …` 两行对照、`启用掩码` 开关、
  类型单选、掩码位选择（金圈、全选/清空/反选、表达式）、`固化到打孔数据`（把电路值写回纸带并
  关掉掩码）与 `撤销上一次`；纸带下方的按钮显示 `掩码 ON and 0x…`。
- 验收：真机 `TC_MODLOADER_PUNCH_TAPE_MASK="and:0xFF@0x100"`（沙盒）日志给出
  `mask selftest layer and 0xFF … punch=0x100 panel=0x100 circuit=0x0`——面板保持打孔数据、
  电路拿到掩码后的值，正是玩家要的语义；`tests/punchcard-playtest.ps1 -PunchTape -Mask …`
  断言 `layer=1`。

- **0.9.2：掩码工具改成"先选类型 + 选位 + 预览 + 应用"**（2026-09-21，玩家反馈）。
  玩家的用法是"选择用什么类型的掩码，以及选择掩码位"；0.9.1 把六个动作平铺成一行按钮，
  结果 `AND 保留` 与 `ANDNOT 清 0` 挨在一起被点错——玩家选好 0–7 位（掩码 `0xFF`）、值里只有
  第 8 位为 1（`0x100`）时按到 ANDNOT，得到 `0x100 & ~0xFF = 0x100`（也就是 256），而他期望的
  AND 应当是 `0x100 & 0xFF = 0`。现在：
  - 工具窗顶部先给 **掩码类型**：`AND v&m` / `OR v|m` / `XOR v^m` / `ANDNOT v&~m` / `赋值 v=m`
    （单选，按钮上直接写公式，不再有名字相近的两个 AND）；
  - 中间是 **基准值 → 预览**：`预览 = 类型(基准值, 掩码)`，按"应用"之前就能看到结果；
  - 掩码位照旧用"选掩码位"模式在纸带上点（金圈，不动数值），旁边有 `全选 / 清空 / 反选`
    与 `掩码表达式`（与游戏数值框同语法）；
  - 新增 **"选位时立即应用"**：勾上后每选一位就按当前类型对"基准值"重算并写入
    （始终 `类型(基准值, 掩码)`，不会像逐次施加那样把 XOR 反复翻转）；基准值随面板值走，
    只有工具自己的写入不改变它；
  - 纸带下方的按钮也带类型：`掩码 and 0x100`。
  - 删掉了与 ANDNOT 完全等价的"清除非掩码位"按钮（那是我上一版的重复项，正是混淆来源之一）。
  - 玩家日志确认写入本身可用：连续两个掩码动作的前后值是衔接的（`toggle 0xF8` 之后下一次的
    `before` 就是 `0x0`），所以这次只改交互与命名，不动写入路径。

- **0.9.1：修"点一次位闪很多下"，并把掩码语义写清楚**（2026-09-21，玩家反馈）。
  根因：SDK 的 `Canvas` 把 `igInvisibleButton` 的返回值当成"点击"，而这个引擎在按住期间
  **每帧都返回真**——一次按住于是一次翻转了几十次（玩家日志里
  `workshop input #5 bit 3 flipped` 连着 5 条、`bit 10` 连着 11 条）。现在改用鼠标按下的
  **边沿**（`isMouseDown` 由假变真那一帧）判定：一次物理点击 = 一次动作。按玩家要求也
  **去掉了长按拖动刷位**。
- 掩码命名与说明按玩家的说法重写：开关叫 **"选掩码位（只改掩码，不改数值）"**，选中的位就是
  掩码里为 1 的位（金圈），掩码行写明 `掩码 0x… · 选中 n 位（选中的位=掩码 1）`；动作改成
  **OR 置 1 / AND 保留 / ANDNOT 清 0 / XOR 翻转 / 覆盖为掩码 / 清除非掩码位**（原来叫
  "置1/清0/翻转/保留/清除"，看不出是掩码运算），另有 `全选 / 清空 / 反选`，表达式框标题改为
  `掩码表达式`。选位点击会写日志（`mask bits now 0x… (n selected) on component 0x…`，上限
  12 条），以后看日志就知道玩家选了什么。
- 顺带确认（玩家日志）：掩码运算本身是生效的——`toggle` 0x708 前后值、`AND 保留`、`清除非掩码位`、
  `覆盖为掩码` 的前后值都自洽；问题只出在触发方式与命名。

- **0.9.0：掩码工具**（2026-09-21）。玩家提的用法：原生数值框能写
  `0xFFFFFFFF^(1<<23)` 这类表达式，纸带也应当能"先选一组位、再一次作用到值上"。现在有两条路：
  - **修饰键**：`Shift+点击`=置 1、`Ctrl+点击`=清 0、普通点击=翻转（原行为不变），按住左键在纸带上
    拖动会对划过的格子连续施加同一个动作；
  - **纸带下方的「掩码」按钮 → 弹出工具窗**：`选位模式`里点击纸带只切换掩码（被选中的位画金圈，
    数值不变），工具窗显示 `掩码 0x… (n 位)` 与当前引脚/位宽/值，动作为
    `置1 / 清0 / 翻转 / 保留 v&m / 清除 v&~m`（都按引脚位宽截断），另有 `全选 / 清空 / 反选`、
    表达式框（语法与游戏数值框一致，经 `tc.io_value` 的 `evaluate` 求值）、`撤销上一次`
    （记住写入前的值）与按元件 id 的记忆（掩码与选位状态）。
  - 写入路径：常量走 `tc.io_value` 的 `write_constant`（设置值 + 运行时值槽 + 刷新，不重编译）；
    输入走游戏自己的 `flip_component_global_input`——与原生位方块、以及纸带原有单击完全同一条
    入口，因此周期重置语义不变。**实测结论**（`symphony_2_io`，2026-09-21）：服务里的
    `set_component_global_input` 写的是"周期重置消费的控制回放"，面板绘制的
    `get_component_global_input` 读的是仿真回放，两者要等仿真再次采样才一致；所以掩码对**输入**
    的写入刻意使用翻转路径（原生点击同款），而 `write_input` 留给其它 Mod 按需使用。
    该关卡自带测试会重新施加输入，所以自检日志里"下一帧回读"只作参考（见验证一节）。
  - 排版：掩码行画在纸带与原版数值框之间，行高按游戏实际画的按钮测量；侧栏把这一行加进
    "本项要让出的高度"（0.8.4 的锚点机制），因此不会重叠。
  - 验收：离线 `tests/io-value-service.ps1`（表达式解析/截断/服务表）；真机
    `tests/punchcard-playtest.ps1 -PunchTape -Mask "set:0x100"` 断言
    `punch tape: mask selftest … applied=1 service=1`（掩码算术与写入路径确实执行、服务可用），
    截图 `build/punchcard-out/mask6.png` 里金圈标出被选位、掩码按钮显示 `掩码 0x100`、数值框
    各就各位；回归 `tests/punch-tape-layout.ps1` 依旧通过。
- **调试开关** `TC_MODLOADER_PUNCH_TAPE_MASK="<op>:<表达式>"`：对面板里第一个宽位元件执行一次
  掩码并写日志（`applied=` 与下一帧 `readback/match=`），供沙盒验收使用。

- **0.8.5：纸带避开面板滚动条**（2026-09-21）。0.8.4 让下面的内容下移后，面板内容超出高度时
  会出现竖向滚动条；ImGui 把滚动条画在窗口**内部**，而纸带是按整个窗口宽度测量并居中的，于是
  右侧最后一列孔位被滚动条盖住（玩家截图：`aaaaaa123` 最右一列只露一半）。
- 现在纸带按“可见内容宽度”测量与居中：用 `igGetScrollMaxY()` 判断该子窗口是否真的在滚动，再用
  `igGetContentRegionAvail()` 加当前光标 X 得到内容区右边界（滚动条在它外侧）。窗口不滚动时
  仍按整个窗口宽度，所以正常情况下孔位不会因为这条修复变小。
- 同时把侧栏的字节组间距从抽屉的 28/44 收到孔位的 2/5（0.8.4 已给 `makeLayout` 加了
  `preferred` 参数，这里只改侧栏自己的那份，底部抽屉不变），同样宽度下孔位再大 1~2%：
  实测 32 位 `symphony_11_functions` 由 cell 1717 变 1739。
- 调试开关 `TC_MODLOADER_PUNCH_TAPE_SCROLLBAR=<px>`（与加载器的 `TC_MODLOADER_TRACE_*` 同类）：
  假装右侧有这么多像素的滚动条，用来在没有滚动的面板上检查这条路径。
- 验证：真机 `TC_MODLOADER_PUNCH_TAPE_SCROLLBAR=18` + `tests/punchcard-playtest.ps1 -PunchTape`
  日志 `geometry workshop_input7 … grid=295x36 cell=1639 group=655`（按 18 px 让位并居中，右侧
  留出滚动条的位置），截图 `build/punchcard-out/scrollbar-panel.png`；不滚动的面板
  （`symphony_8_io_devices`、`symphony_11_functions`）仍是 `grid=313x39`，尺寸不受影响。
  离线用例 `tests/punch-tape-layout.cpp` 增加“让出滚动条后仍能整组换行、右边缘不越界”。

- **0.8.4：侧栏纸带保持孔位不变，改为把下面的内容顶下去**（2026-09-21）。玩家截图里 64 位输入
  `aaaaaa123` 的纸带把原版数值框挤到了下方“输出状态”标题上。根因：这个面板**不按内容堆叠**
  ——`build_io_state_view` 每画完一项就把下一项锚回自己那套固定节奏（`90 × 游戏 UI 缩放`，
  333 px 侧栏在 606 px 高窗口里实测 121 px），所以一项自己的高度既挤不开下一项，也推不动
  后面的内容。
- 先按“把纸带缩到本项空出来的高度”做过一版（那样 64 位在这块面板里只剩 36 px，孔位要掉到
  约 7 px），**玩家否决：孔太小**。现在的做法反过来：纸带尺寸继续用 0.8.2/0.8.3 的侧栏 band
  （矮面板 ≤48 px、高面板 80–110 px），把纸带的高度加到面板内每次 `igSetCursorPosY` 上——
  下一项、循环结束后的“输出状态”标题、以及整段输出列表一起下移（真机日志
  `punch tape: workshop entry pushed down by 44 px`）。
- 加多少是实测的：原版一项 = 标签 + 数值框，节奏留给它的间隙是 `节奏 − 标签 − 数值框`
  （333 px 侧栏实测 121 − 18 − 58 = 45 px）；加“纸带高 + 画布下方 6 px”正好把这个间隙原样
  保留，所以带纸带的一项留白与原版一致，且不与任何东西重叠。
- 挂钩范围：`igSetCursorPosY` 只处理返回地址落在 `build_io_state_view` 内部的调用（本构建该
  函数 0x3fc0 字节，符号表里下一个符号即其结束），因为输出段用的是它自己的锚点、不是输入循环
  那一个；`igBeginChild_Str`（返回地址 `ioStateView+0x394`）在每个面板开始时把偏移清零，帧号
  变化也会清零。两者都原样转发。钩子装不上时只影响留白（纸带仍按 0.8.3 画）。
- 顺带：1/4 孔位的行距现在由 `makeLayout` 的 `preferred` 参数带进布局，不再事后压缩，同样的
  band 能换到更大的孔（矮面板里的 32 位输入 cell 15.7 → 17.2）。
- 验证：`tests/punch-tape-layout.cpp` 侧栏小节改为断言“孔位不缩水 + 顶下去后仍保持原版间隙”；
  真机 `tests/punchcard-playtest.ps1 -PunchTape`（`symphony_2_io`，32 位输入）日志
  `geometry workshop_input7 … cell=1717 grid=313x38` 与 `workshop entry pushed down by 44 px`，
  截图里纸带、数值框、“Outputs”标题与输出项各就各位（`build/punchcard-out/grow2.png`）。

- **0.8.3：收紧工坊纸带行距**（2026-09-21）。保留 0.8.2 放大后的孔位尺寸，将侧栏纸带的
  纵向行距限制为孔位尺寸的 1/4；64 位四行整体缩短约 15px，为游戏固定位置的“输出状态”标题
  留出空隙，避免最后一行与标题重叠。

- **0.8.2：放大工坊侧栏孔位**（2026-09-21）。每行最多两组的规则不变；高度超过 430px 的
  工坊左侧面板把纸带高度预算从最多 48px 提高到 80–110px，64 位仍为 4 行 × 2 组，但孔位可从
  约 7px 放大到约 17px。较矮的关卡 IO 面板继续使用紧凑预算，避免覆盖输出区。

- **0.8.1：工坊侧栏改为最多两列**（2026-09-21）。左侧输入纸带现在每行最多显示两个 8 位组
  （16 位），宽位输入优先向下增加行数；底部常量抽屉仍保持每行最多四组。

- **0.8.0：元件工坊左侧输入设置支持宽位纸带**（2026-09-21）。原版
  `build_io_state_view` 只在位宽 ≤8 时绘制纸带，更宽的输入只剩数值框。Mod 现在在该界面读取
  输入值的专用调用点补画同一套游戏图块，1–8 位原版路径保持不变；点击调用游戏原生
  `flip_component_global_input`，继续沿用它的周期重置语义。侧栏使用比底部抽屉更小的独立缩放下限，
  仍严格每 8 位一组、最多每行 4 组，并按当前侧栏宽度选择整组换行与缩放，保证整条纸带不出界。
  真机 `symphony_2_io` 的 32 位输入在 333 px 宽面板中显示为 2 行 × 2 组（286×39 px），点击位
  5/4 均进入原生翻转函数；用例为 `tests/punchcard-playtest.ps1 -PunchTape -NoPatch`。

- **加载器的源码 dump 改成按需开启**（2026-09-21）。`src/native_logic.hpp` 过去每次编译都往
  **游戏目录**写 `native-logic-source-N.txt`（关卡一次编译两个程序，所以一次进关至少两个文件；
  正式安装目录里已经堆了 59 个、约 4.0 MB）。现在只有设 `TC_MODLOADER_DUMP_SOURCE=1` 才写，
  默认一个字节都不落盘；验收脚本自己带上这个变量，并新增 `punch-tape-playtest.ps1 -NoDumpEvidence`
  专门验证"不设变量时目录是干净的、关卡照常编译运行"。已归档的 59 个历史 dump 移到了
  `tc-modloader\build\legacy-source-dumps\`。

- **0.7.3：纸带随窗口自适应**（2026-09-21）。此前纸带的尺寸与纵向位置是按一次测量写死的
  （44px 单元、抽屉内 y=119），窗口一放大它就显得又小又贴顶，抽屉一矮则可能整排画到面板外。
  现在几何算术独立成 `examples/punch-tape/tape_layout.hpp`：优先按整字节组换行、行数尽量填满
  （不留 24/24/16 这样的半截行），再把整条纸带缩放进「抽屉剩余宽 × 高」这块空间，最多放大 1.5 倍、
  最少缩到 0.25 倍，尺寸由 `tc::ui::windowWidth()/windowHeight()` 现场读取。用户那块 2555×1450
  窗口上，18 位常量从 1217×44（44px 单元）变成 1826×66（66px 单元，正好铺满可用宽度）；同一块
  电路在 1462×914 的老窗口里，64 位常量由「一列 520px 高、会溢出抽屉」变成 4 行 ×16 位、553×171
  完整可见。用例：`tests/punch-tape-layout.ps1`（fast 层）；预览：
  `tools/preview-punch-tape.cpp <out.bmp> <面板宽> <面板高> <位宽,...>`。

- **症状**：在 73.2k 门的电路上点纸带的一格，界面要停约 1 秒才更新。
- **定位**：`set_setting` 只改元件记录，模拟器用的却是编译期写死的常量字面量；原路径随后调
  `upgrade(presenterContext + 0x1a3b8, 0x30)`，游戏按字面量重新生成源码并重编译整张电路。
  延迟因此来自整板重编译，UI 侧怎么调刷新顺序都没用。
- **修复**：加载器的代码生成钩子把 9–64 位常量从字面量改写成
  `U64 game_engine.'tc_dynamic_constant'(U64 <component-id>, U64 <fallback>)`（refresh 与 cycle
  两阶段都改），在编译器 foreign 表里注册该桥，并导出 `tc_dynamic_constant_set`。纸带点击改成
  写设置 → 更新运行时槽 → 刷新现有仿真，不再请求重编译；遇到没有该导出的旧加载器时自动回退到
  原来的重编译路径。
- **真机验收**（2026-09-20，玩家自己的 `sandbox`／RV32I 电路）：两阶段各 6 行改写、关卡跑通
  （`autotest finished cycle=39 verdict=0`）、三次更新 0.001–0.008 ms 且源码 dump 数不变、
  从 `#SIMULATION_STATE + 332` 回读到的值与纸带写入值一致；真人鼠标点击纸带的两条记录同样自洽
  （bit 3 → 11、bit 12 → 4107）。脚本与证据见
  [HANDOFF-punch-tape-runtime-constants.md](HANDOFF-punch-tape-runtime-constants.md)。
- **已知未尽**：原版数字输入框仍按原版语义重编译；运行时值槽的高频读取仍带互斥锁；验收脚本的
  合成鼠标点击还没落地（真人点击已经通了）。

## 未发布 — WordWatchee 64 位数值标签（新包 `local.word-watchee-64`）

- **症状**：字长 >32 位的棋盘上，导线与端口的数值标签只显示低 32 位（高 32 位整段消失）。
- **定位**（证据见 [research/word-watchee-64.md](research/word-watchee-64.md)）：模拟器把整字
  读成 uint64（`sim_state_read_u64`）并按 8 字节上传，着色器也已经写好 33–64 位的十六进制、
  无符号与有符号十进制；丢失发生在绘制侧：`word_watchee_mesh.set_value_size` 写 instance 属性
  前执行 `min(value_size, 32)`（函数 +0x40 处 `mov eax,0x20` / `cmp bl,al` / `cmova ebx,eax`）。
  同一路径另有独立一处：`word_watchee.vert` 的 `repr16()` 64 位分支把低 32 位的起始位移写成
  `32`（应为 `28`），会多写一个半字节并把低位数字整体错位一格；32 位字上的 `>> 32` 还是
  GLSL 未定义行为。
- **修复（新包 `local.word-watchee-64`，format 2）**：原生插件把该立即数改成 `0x40`
  （即 `min(value_size, 64)`，上限取 64 是因为着色器用 `1u << (size-33)`，>64 会未定义），
  写入前逐字节核对本构建的函数窗口，不匹配只写状态不动代码；着色器那行由包的精确文本补丁
  改掉，停用包时从备份还原。1/8/32 位棋盘的代码路径逐字节不变。
- **包 1.1.0：位宽 >32 位改成双排十进制**。64 位十进制一行 20 位太长，所以宽标签画成两行，
  换行落在**十进制数字**的第 10 位之后：上排是前 10 位数字、下排是剩下的数字（不足 10 位时
  下排显示 0），例如 2^63 = 9223372036854775808 显示为 `9223372036` / `854775808`；宽标签
  **不跟随**游戏的十六进制／无符号／有符号格式。位宽 ≤32 位的标签完全不变，仍按游戏设置的
  格式单行显示。实现要同时改顶点与片元着色器（片元按行寻址字形），所以包里改成**整文件部署**
  `word_watchee.vert` + `word_watchee.frag`（停用即从 `blobs/` 还原原文件），顶点着色器里
  `shift = 28` 那处修正也在这份文件中。真机截图见
  [verification.md](verification.md#wordwatchee-64-位数值标签2026-09-20)。
- **包 1.1.0 的真机回归修正**（同一版内，用户截图发现）：① 第二行曾被截断成一位——真机调试
  确认数字提取正确，坏在**动态下标局部数组**（`uint scratch[20]`）在这块驱动上的读写只保留
  一格，现已改成"先数位数、每个数字直接写进目标格子"，着色器里不再有任何中间数组；
  ② 游戏自带的 `divmod10`（`ceil(2^67/10)` 逆乘）对 **2^63** 差一（余数 7 vs 真值 8），
  vanilla 只用 ≤10 位所以没暴露，本包换成四段 16 位长除法。真机截图：2^64-1 →
  `1844674407`/`3709551615`，2^63 → `9223372036`/`854775808`（与用户给的例子一致）。
  离线用例新增两条断言（不得出现 `scratch[`；必须用 `/ 10u` 长除法）。
- **验证**：`node tests/word-watchee-model.js` 从**钉住的 EXE**读函数窗口、从**包里的
  `.vert`** 解析 `ROW_STRIDE` 与两行语义，断言 >32 位的两行十进制（含边界值）与 ≤32 位单行
  仍跟随格式；`tests/word-watchee-playtest.ps1` 断言两个着色器与包内文件逐字节相同、`state.json`
  记录的前后哈希与备份、插件在真机读回的位宽字节、进入棋盘截图；`tests/word-watchee-layout-playtest.ps1`
  用探针把位宽强制成 64，在真实 Byte Adder 棋盘上截出双排标签（diagnostic 层）。
  尚未取得"64 位棋盘上高 32 位真的画出来"的画面证据，原因写在研究记录 §6。

## 未发布 0.6.0 — 符号画像与钩子链（地基第二项）

- **修复 `level.load` 的关卡名载荷**：加载器现在把钩子参数里的 Nim 字符串转换成回调期间有效的
  C 字符串传给 `TC_EVENT_LEVEL_LOAD`，不再固定派发空值；事件名指针仍是借用值，Mod 不得跨回调
  保存。离线宿主测试用真实 Nim 字符串布局覆盖转换，真机 `game-handle-probe` 要求两次加载都得到
  非 `(none)` 的名字。
- **Board V4 对象数据读取**：新增 `read_component` / `read_wire`，把 V3 签发的句柄变成可读对象。
  元件暴露 kind、原理图坐标、方向字节、板内 64 位 id 与自定义原型 id；导线暴露它自己的序列下标
  （游戏 `get_wire` 的 id）、端点、位宽和仿真状态字节偏移。字段全部来自指定构建已核实的记录位置，
  语义未核实的实例父链与引脚描述符**没有**进结构。读取要求句柄与记录仍然属于当前棋盘序列，枚举
  之后被编辑或跨帧都会以 STALE 拒绝；离线契约用例覆盖解码与边界，真机探针逐条读回每个对象并断言
  id 唯一、位宽与状态槽在实测范围内。真机还测出两条只有实机能给的事实：元件序列第 0 条是全零
  占位记录（kind 0，不是元件），以及 `level.load` 帧内的棋盘可能还没稳定（3 元件/1 导线 vs
  相邻帧 1/0），两者都写进了 `docs/sdk/services.md`。
- **引脚几何与位宽封装**：`sdk/tc_game_model.h` 新增 `tc::pinPoint()`、`prototypeInput/OutputPinPoint()`、
  `prototypeInput/OutputPinWordSize()` 与 `pinWordSizeIsAuto()`，把“描述符起点是 `TCPin* + 8`”
  这条锚点算术收进 SDK——此前每个调用方都要自己加。`tests/kind-list-probe.cpp` 改用这些入口后，
  真机重新 dump 的 126 行与改动前逐行一致（125 个内置元件的名字／引脚数／偏移／字宽全同），
  这是封装正确性的真机证据。
- **Board V5 元件引脚服务**：新增 `read_component_pins`，给一个元件句柄就能拿到它的引脚列表
  （相对坐标、声明位宽、原始 WordSize 与 `AUTO_SIZE` 标记），宿主用元件自己的 kind 或
  `custom_prototype_id` 取原型、在同一调用内复制并释放，没有任何借出的原型指针离开调用。
  内置 kind 只使用启动时从游戏 `PROTOTYPES` 表枚举出的集合，未知键绝不交给游戏。真机探针在战役
  关卡读到的 `0x3c`（0 进 1 出，`(1,0)`，w1）与 `0x44`（1 进 0 出，`(-1,0)`，w1）与
  `build/kinds.txt` 里同一构建的条目逐字段相同；`kind == 0` 的占位记录按游戏自己的空原型读成
  0 个引脚（日志 `zero=1`），不伪装成元件。
- **`tc.simulation` 仿真读数服务**：新增 `get_state`（周期、引擎帧、状态缓冲大小）与
  `read_value`（读某个字节偏移的低 N 位，语义与游戏自己的 `sim_state_read_bits` 相同）。导线记录
  里的状态槽偏移与位宽因此可以被直接取值。偏移按模拟器初始化时分配的 10,240,000 字节缓冲做界内
  检查，越界返回 `RANGE` 而不是野读；沿用"状态在步进结束时才写好"的时机规则。服务层由真机探针
  验证（别名武装、界内、掩码与游戏一致），"值随周期变化"的正确性由波形用例负责。
- **P1 编辑命令族起步：命令 V2 与元件放置**：新增 `TCCommandApiV2` / `TCCommandV2`（保留 V1 前缀，
  追加放置载荷）与 `TC_TRANSACTION_API_VERSION_2`，命令总线因此能表达"在某个格点放一个元件"，
  事务也能承载它。关键证据是放置助手 `add_component__presenterZutilitiesZhelper95functions_u5918`
  的第一个参数就是 Board model——句柄正好解析到它，所以编辑不需要 presenter 上下文，也不用
  自己写 UI 自动化。真机 `component-placement-playtest`：`submit=0` → `state=3 result=0`，复用一个
  kind 0 墓碑槽（序列 2→2、活元件 0→1），并按 V3 枚举 + V4 读数在 `(20,6)` 找到 kind `0x4e`、
  自定义 id 全对的新元件。同一次运行还把两个编辑放进 V2 事务（`begin → stage×2 → commit`）：
  `state=4`（COMMITTED）、`staged=2 completed=2`，棋盘序列 2→4 并在两个目标点都命中——命令、
  事务与公开回读三段串起来验过。
- **删除元件入口完成第一轮核实**：反汇编确认
  `board_delete_component(component_sequence,index)` 的第一个参数是 `board + 0x78`；真机按“放置 →
  删除 → V3/V4 回读”测到序列 `2→2`、活元件 `1→0`、原坐标查无记录。删除与 undo 一样留下 kind 0
  墓碑，下一次放置会复用该槽。这个低层入口没有自己写撤销历史，因此尚未直接进命令总线；已找到
  会调用 `add_undo_changes` 的 `try_delete(board, component_indices, wire_ids)`，下一步是先真机验证它的
  delete/undo/redo，再定命令载荷。移动候选 `board_commit_move` 只提交已完成的拖拽并依赖选择集，
  没有目标坐标参数，也暂不接入。
- **修正导线记录的端点读法**：`TCWireInfoV1` 的 `x`／`y` 曾被当成"一个端点的坐标"，实际
  `+0x18` 起是**两对** int16（x1/y1 与 x2/y2）。P1 的加线实验推翻了旧读法，两个独立证据给出新
  解释：and_gate 内置解的导线是 `(-6,-1)->(-13,-1)` 的水平线，战役关卡的是 `(9,0)->(-9,0)`，
  正好落在输入/输出引脚之间。结构已改为 `x1/y1/x2/y2`，真机断言同步更新。
- **导线编辑的结论（暂不进总线）**：`add_wire_from_pos` 只需 board+点+颜色，但真机实验显示它
  只"起一条线"——记录零长度、状态槽为奇数、游戏自己的 `get_wire(point)` 拒绝寻址。把它做成队
  列命令会在棋盘上留下半成品，因此 `PLACE_WIRE` 不进契约；要做真正的程序化连线需要新的反推或
  一次钩子归属决策，证据与两条路线写在 [research/board-object-fields.md](research/board-object-fields.md) §4。
- **Board V6 连接判定**：新增 `read_wire_ends`，用一个调用回答"这条线的每一端连在哪个元件的哪个
  引脚上"。规则是玩家看到的几何规则（引脚位置 = 元件位置 + 引脚偏移），实现只比较邻近窗口内的
  元件，候选元件用与 V5 相同的内置/自定义原型来源。真机：战役关卡那条线两端分别解析到 0x44 与
  0x3c 两个元件句柄，方向与引脚序号正好是它们的端口，V4 回读确认 kind。
- **Undo/Redo 语义实测（编辑族第 4 项）**：真机用一个**两步事务**（连续放两个元件）测出两件事：
  ① 一次 `BOARD_UNDO` 只回退**一步**，所以"事务 = 一步撤销"不成立，想让玩家一次 Ctrl+Z 抹掉整批
  得自己按步数 undo；② 被撤销的元件**不会缩短序列**，而是留在原地变成一条 kind 0 的全零墓碑，
  于是"数元件必须跳过 kind 0"（`component_count` 只是序列长度），Redo 会把该记录还原。两条都写进
  [sdk/commands.md](sdk/commands.md) 与 [reference/limits.md](reference/limits.md)。
- **`OBJECTS_CHANGED` 上真机（编辑族第 3 项的一半）**：探针在自身 stop+save 事务结束后，通过命令
  总线放一个**内置**元件（0x04），下一帧收到**恰好一次**结构变化事件；活元件数 0→1（稳定帧读数，
  跳过 kind 0 占位）。`SELECTION_CHANGED` 仍只有离线覆盖——写选择集还没有已核实入口，两者一起
  列在待办。
- **选择集写入的线索**：用 `nm` 在 `modelZboardZboard` 命名空间筛出候选
  （`select_component`、`incl`×2、`excl`、`clear_selections`、`get_selection`，以及和撤销栈直接相关的
  `add_undo_changes`），全部标注为**签名未核实**，列在
  [research/board-object-fields.md](research/board-object-fields.md) §5，等下一轮反汇编 + 真机探针。
- **实例父链的结论**：元件记录 `+0x10`／`+0x18` 经两次真机 dump 确认在**板级记录上是空的**，
  读它们的代价求和与代码生成路径都跑在编译期扁平序列上。因此 Board V4 继续不暴露它，理由是
  “它不属于板块”，而不是“还没测”；要暴露应另开编译图服务。见
  [research/board-object-fields.md](research/board-object-fields.md)。
- **游戏 API 不再继续堆进 `TCHost`**：新增 `query_service` 与 `services` 能力，服务 id、版本、
  输出大小分别校验；第一张 `TCBoardApiV1` 表承接当前 Board 句柄的查询、验证和受控解析。
  既有三个句柄入口原样保留给旧二进制 Mod，新接口以后按 Board/Simulation/Command 等服务独立
  演进。离线测试覆盖未知服务、错误版本、短缓冲区、旧宿主回退和真实句柄生命周期。
- **Board V2 同帧只读快照**：新增不含借用指针的 `TCBoardSnapshotV1`，一次调用复制 Board
  身份、引擎帧、仿真周期和当前/上一份选择计数。捕获只允许在游戏主线程执行，并在读取前后
  复核 Board 句柄与帧号；数据源缺失由 flags 明示，重入、失效句柄和短缓冲区都有独立错误码。
- **Board V3 对象句柄**：`capture_objects` 用调用方缓冲区原子枚举完整 Component/Wire 集合，
  容量不足只报告所需数量、不发布半份结果。子句柄只能由这条可信枚举路径签发，并在下一引擎帧
  或 Board 生命周期结束时自动失效，避免把会搬迁的游戏序列元素当成长寿命裸指针。
- **命令总线 V1**：新增 `tc.commands` 服务，把 run/stop/reset 意图排到本帧插件回调之后统一
  执行；每条请求绑定提交 Mod 和 Board 句柄，执行前二次验证生命周期，并提供 QUEUED 到终态的
  查询与取消。队列、每 Mod 未完成数和历史结果都有硬上限，插件拒载时自动取消其请求。
- **值类型生命周期/变化事件 V1**：新增 `tc.lifecycle` 服务，统一报告 Board entered/left、对象
  结构变化与选择集合变化。事件携带句柄、帧/周期、单调序号和带 flags 的计数；结构指纹只覆盖
  已核实稳定字段，选择指纹覆盖实际 ID，避免轮询、同数量换选漏报和仿真缓存噪声。
- **事务、Undo/Redo 与 Save**：命令总线接入游戏原生 Board undo/redo 和关卡保存入口；新增
  `tc.transactions`，在 begin 时记录 Board 结构指纹，commit 时统一预检冲突并连续执行最多 32
  步，可选仅在全部成功后保存。V1 明确不伪造通用自动回滚，失败会报告 completed_count，补偿走
  游戏自身 undo 历史。
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

- **从 loader 内置功能拆成可启停 Mod**：完整 Mod Loader 不再编译、安装或自动启用引脚显示钩子；
  新包 `local.pin-names-patch.mod`（v0.0.3）通过宿主 `create_hook` 注册预览文字、引脚白条、
  自定义原型重载和 IO 状态面板四个钩子，只有玩家启用该 Mod 时才生效。宿主的原始钩子校验相应
  扩展为接受两类安全目标：符号画像中的 EXE 函数，以及 `engine_proc` 返回且确实位于原引擎模块
  可执行页内的导出；重复目标仍会被拒绝。免 Mod Loader 的独立 `game_engine.dll` 补丁继续保留。
- **修复原版 IO 状态面板不同步**：在工坊下方修改输入或输出端子标签时，画布元件会立即使用
  新名称，但左侧“输入状态/输出状态”读的是仅在 preorder 完成时重建的旧缓存，所以必须退出再进入
  工坊才变化。补丁现在监听自定义原型重载，在状态面板下一次绘制前通过游戏自己的
  `get_io_states` 与托管复制/销毁函数重建输入、输出及第三组 IO 缓存；不会用裸内存复制破坏
  Nim 字符串的所有权。
- **修复工坊内改名后表格仍显示旧标签**：跨帧缓存以前拿“文字包围盒中心”识别引脚，改名会
  改变文字宽度，也就改变中心点，导致输入、输出引脚都可能与自己的缓存行失配。现在有白色
  引脚线段时以稳定的“引脚端点 + 朝向”匹配，只有首帧尚无白线数据时才回退到文字中心；同时
  表格延后到本帧最后一个引脚采集完毕后再画，直接使用本帧的新标签。只改标签不再需要退出并
  重进元件工坊，结构性增删引脚仍保留一帧的稳定期。
- **修好"自定义元件引脚名太长时在元件预览里糊成一团"**：预览（底部元件栏与元件工坊共用
  `build_custom_component_preview`，EXE RVA 0x38e990）按引脚逐个调用引擎导出的 `igText`
  画名字，而预览把元件缩小、名字却按原字号画，于是长名字压住旁边的名字（实测名字可到 470 px，
  而同一排引脚只隔 24 px）。摊开、倾斜 45°、缩小都试过，在 400 px 高的面板里只会让名字
  到处乱飞，最终按玩家的方案改成**编号表**：预览图里只画编号，名字全部放进面板空白一侧的
  表格里（多列排布，字号默认 70%，`TC_MODLOADER_PIN_TABLE=<百分比>` 可调，`=0` 关闭）。
  编号由补丁 Mod **实时分配**：先从名字反推出引脚位置，再按引脚外框**顺时针**走一圈
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
  启用补丁 Mod 后只接管预览那一次文本调用（返回地址 0x38f0ef），其余 193 个调用点原样转发
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
