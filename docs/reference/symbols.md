# 符号别名与钩子链

## 为什么要别名

游戏是 Nim 编译的，函数名长这样：

```text
sim_do__modelZsimulationZcompile95thread_u3036
```

名字里带模块路径和一个**随构建变化的编号**，所以插件直接写名字的话，游戏一更新就全部失效，
而写错一个字符只会在初始化中途得到一个空指针。别名把这件事收敛到一处：

| 谁 | 做什么 |
|---|---|
| `src/symbol_profile.hpp` | 唯一的表：别名 → 本构建的 COFF 名（含数据类型与说明） |
| 加载器启动时 | 把整张表解析一遍，日志写 `Symbol profile: N/M aliases resolved` 和每个缺失项 |
| 插件 | `tc::resolveAlias(host,"sim.do")` / `tc::resolveAliasAs<Fn>(host,"sim.cycle")` |

未知别名与本构建解析失败的别名都返回 null；`dist` 里的示例已经全部改用别名。

## 别名清单

| 别名 | 类型 | 说明 |
|---|---|---|
| `sim.do` | 函数 | `void(void* model, uint8_t command, int64_t target)`；command 0 run / 1 refresh·stop / 2 mode_reset |
| `sim.cycle` | 函数 | `int64_t(void)`；未开始为 -1 |
| `sim.settings` | 数据 | `void**`：共享设置块，解引用前先确认非空 |
| `sim.setting.get` | 函数 | `int64_t(uint8_t)` 命令行设置 |
| `sim.setting.set` | 函数 | `void(uint8_t, int64_t)` |
| `sim.state.read` | 函数 | 读仿真状态位（波形探针用导线的偏移/位宽调用） |
| `level.load` | 函数 | `void(void* boardModel, const TCNimString* name)` |
| `level.loaded` | 函数 | 游戏认为已加载的关卡对象 |
| `scene.change` | 函数 | 场景切换 |
| `board.wire.update` | 函数 | `bool(void*, void*, void*, uint32_t point, uint8_t colour)` |
| `save.count` | 数据 | `const int64_t*` 保存次数 |
| `save.path.level` | 数据 | 关卡路径的 emutls 控制块 |
| `save.path.schematic` | 数据 | 原理图路径的 emutls 控制块 |
| `runtime.emutls` | 函数 | `void*(void*)`：取当前线程的 emutls 槽 |
| `ui.fonts` | 数据 | 游戏字体表（第 2 项是正文面） |
| `ui.pushFont` | 函数 | `void(unsigned char index)` |
| `ui.invisibleButton` | 函数 | EXE 里的导入桩，界面驱动用它判断当前在哪一屏 |
| `cost.gate` / `cost.total` / `cost.delay` | 函数 | 元件代价与延迟查询 |

表里没有但你已经自己核对过签名的符号，仍然可以用
`host->resolve_symbol(context, "确切名字")` 取地址——别名是承诺，不是限制。

## 钩子链

0.6.0 之前，一个目标只能有一个 Hook：第二个想钩 `sim_do` 的 Mod 直接被拒绝。现在
**加载器拥有唯一的 detour**，插件加入的是链：

```cpp
static int onRun(TCHookCall* call) {
    auto* args = tc::hook::simDoArgs(call);
    if (args && args->command == 0 && args->target > 1000) args->target = 1000;  // 改游戏看到的参数
    return 0;                                                                     // 继续下一个链节
}

// tc_mod_load 里：
const int status = tc::hook::addSimDo(host, 0, &onRun, nullptr);
if (status != TC_HOOK_OK) return 2;   // TC_HOOK_ERR_TARGET = 本构建没有这个点
```

顺序是**优先级升序 → Mod id → 注册顺序**，所以同一次关卡重放每次的链序完全一致，
与包的启用顺序无关；日志会写清链上的成员：

```text
Hook chain sim.do installed with 3 link(s): dev.hook-chain-a@0, dev.hook-chain-b@0, dev.hook-chain-a@10
```

三条语义：

1. 返回 0 继续下一个链节，返回非 0 停止（后面的链节不再运行）。
2. 游戏自己的函数在链之后运行**至多一次**；置 `call->skip_original=1` 则不运行。
3. 链节想"包住"这次调用（先让游戏跑完再看返回值）就调用
   `call->run_chain(call)`，然后返回非 0。`run_chain` 只会真正执行一次。

被拒绝的插件不留下链节：`reject()` 会把它注册的所有链节摘掉，和 Hook、界面、元件一致。

## 当前目录里的钩子点

| id | 别名 | 签名 | 备注 |
|---|---|---|---|
| `TC_HOOK_SIM_DO` | `sim.do` | `void(void*, uint8_t, int64_t)` | 例：`example.cycle-guard` 限制单次运行周期数 |
| `TC_HOOK_LEVEL_LOAD` | `level.load` | `void(void*, const void*)` | 例：`example.waveform-demo` 取板模型做导线探针 |

只有**已经实测过签名**的函数才会进这个目录。推广一个新点的步骤：

1. 反汇编确认签名与调用线程，并在 `docs/verification.md` 留下证据；
2. 在 `sdk/tc_hook_api.h` 加 id 与参数结构；
3. 在 `src/native.hpp` 加 `detour*` / `invokeOriginal*` 并注册进 `buildChains()`；
4. 在 `src/symbol_profile.hpp` 里确认别名存在；
5. 把钩这个目标的 Mod 迁到链上（否则它会被 `Hook rejected: … chain point` 拦下），
   再在 `tests/hook-chain.cpp` 里加一个场景。

`board.wire.update` 目前只有别名、没有链：界面驱动直接钩它做实验，推广时按上面五步走。
