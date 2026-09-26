# 接手：元件接口 M2（实例、状态与生命周期）

更新时间：2026-09-22

计划出处：[PLAN-custom-components.md](PLAN-custom-components.md) §7 与 §16 阶段 2。

> 状态：**第一刀已交付**（2026-09-22）。下面第 1–4 项（实例句柄、生命周期、失效规则、
> 世代）都已实现并有真机证据；本文末尾的"不做什么"里那几项正在等 M3。
> 证据与踩坑见 [verification.md](verification.md) 的"实例、状态与生命周期"一节与
> [changelog.md](changelog.md) 的 M2 条目。

## 目标（计划原文）

> 移除固定 `state[8]` 和固定状态槽依赖，建立可靠实例所有权。
> 交付：实例句柄、生命周期、宿主状态 blob、重置语义、资源归属、失效规则。
> 验收：多实例计数器、创建/销毁/复制/重置、旧句柄拒绝和宽输出压力用例通过；
> 不再依赖固定 `0x9a0000` 状态槽作为公共契约。

## 现状盘点（开工前）

| 计划要求 | 现状 |
|---|---|
| 大状态 | ✅ V2 的 `state_words` 由定义决定，状态存在加载器的 `Binding`（宿主内存），与 `0x9a0000` 无关 |
| 固定状态槽作为公共契约 | ✅ 公共接口里没有它；`0x9a0000` 只剩"宽输出按位回读"这一个**内部**用途 |
| 实例身份 | ⚠️ 只有游戏给的 `instance_id` 与内部 token；没有可跨帧保存、带世代检查的句柄 |
| 生命周期 | ⚠️ 只有 RESET（仿真重置时的相位）；没有 on_create / on_destroy |
| 失效规则 | ❌ 实例从棋盘消失后 `Binding` 不会回收，旧回调目标一直留着 |
| 实例查询 | ❌ 没有服务；Mod 无法枚举同类型实例、读别人的状态、单独重置一个实例 |

## 这一轮做什么

1. **实例句柄与查询服务** `tc.component.instances` V1（SDK：`sdk/tc_component_instances.h`）：
   `enumerate`（按 `custom_id` 过滤，0 = 全部）、`validate`（世代校验）、`info`（类型、脚数、
   状态字数、调用/重置计数）、`state`（读宿主状态 blob）、`reset`（只重置一个实例）。
2. **生命周期回调**：`TCComponentLifecycleV1{on_create,on_destroy}`，以**追加字段**的方式挂在
   `TCComponentTypeDefinitionV2` 末尾（老调用方 `size` 较短时视为没有生命周期，必须兼容）。
   `on_create` 在实例首次绑定（编译发现它）时触发，`on_destroy` 在实例消失或离开棋盘时触发；
   两者都在"变化发生的地方"同步调用（编译/切关卡），文档写明不得触碰 UI、不得抛异常。
3. **失效规则**：实例从编译结果里消失就解绑并回收——判据是"这次编译里出现了本类型的其它实例，
   但少了它"（避免把关卡测试台的另一次编译误判成"全没了"）；离开关卡/切场景时全部解绑。
4. **世代**：每次绑定生成新的 generation；旧句柄（instance_id 相同、generation 不同）一律
   `ERR_STALE`，不会读到新实例的状态。

## 这一轮不做什么（写清楚，免得下次当成漏做）

| 不做 | 原因 | 什么时候做 |
|---|---|---|
| `on_clone` | "复制"与"新放置"在编译结果里长得一模一样，要靠 M3 的存储/复制语义才能区分 | M3 |
| `on_move/rotate/resize/config_changed` | 需要编辑器集成（M5）与配置事务（M3） | M3/M5 |
| `on_compile/on_compile_failed` | 编译事件已有 `TC_EVENT_SIM_COMMAND`/日志可用；正式的编译回调要等错误模型 | M3 |
| `on_load/on_save` | 属于存储契约 | M3 |
| 配置状态 vs 仿真状态分离 | 需要 schema（M3）；现在只有"宿主状态 blob"这一层 | M3 |
| 资源归属（纹理/缓冲） | 需要 M5 的 render 服务 | M5 |

## 验收（本轮的"做到"标准）

1. **离线**：句柄世代校验（旧句柄拒绝）、枚举过滤、单实例 reset 只影响目标、状态词数与定义一致。
2. **真机 · 多实例**：同一类型两个实例同时存在 → `enumerate` 返回 2、两个句柄可分别 `info`、
   各自的状态计数独立递增、`reset` 一个不影响另一个。
3. **真机 · 创建与销毁**：`on_create` 每实例一次；删掉元件并重新编译后 `on_destroy` 触发、
   枚举数减少、旧句柄返回 `ERR_STALE`；伪造 generation 的句柄同样被拒绝。
4. **回归**：`byte-adder-smoke`、`custom-or`（V1 路径）、`pin-shape-*` 全部照常 PASS；
   fast 层全绿；ABI 基线更新。

## 关键实现位置

| 位置 | 内容 |
|---|---|
| `sdk/tc_service_api.h` | 句柄、info、`TCComponentInstancesApiV1`、生命周期结构体与常量 |
| `sdk/tc_component_instances.h` | 查询/校验/读取/重置的 SDK 封装 |
| `src/native_logic.hpp` | `Binding` 增加 generation/计数，绑定与解绑处的生命周期派发，实例查询的自由函数 |
| `src/services.hpp` | `tc.component.instances` 的 query 分支与 trampoline |
| `src/native_component.hpp` | V2 定义里的生命周期指针（按 `size` 判定是否存在） |
| `tests/native-component.cpp` | 世代/枚举/reset 的离线断言 |
| `tests/pin-shape-probe.cpp` + `tests/pin-shape-playtest.ps1` | 真机：多实例、创建/销毁、旧句柄 |
