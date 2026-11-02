# T2ROTF（Xbox 360《变形金刚2：卷土重来》）Switch 移植 — 启动即崩「IRQ ABI 误读」修复报告

- 版本标记（判版本只认日志里这一行）：`[video] T2ROTF BUILD v8.7-IRQABI (…)`
- 本轮对应待办：**T63（判读启动崩溃日志：定论根因）+ T64（落地修复、重建、重打包、交付）**
- 上一版：v8.6-FLIP（`recomp/out/sw8/T2ROTF.nro` = 59,949,810 字节 / `93e44c13ede5fe8ded03f6c9528690806ca73103e5021ca2d763bf8c90408b47`）

---

## 0. 一句话

用户回传「本版启动后直接报错退出，没有任何加载」的四份日志，定论为**一处 ABI 误读**：guest 的图形中断回调 `sub_822D47A0` 调用约定是 **`(r3 = 来源 0/1, r4 = 设备)`**，而老钩子把 `r3`（来源值）当设备写入 `g_dev_visto`；v8.5/v8.6 新增的「每垂直同步派发 `source=1` 提交完成」把这个全局被 60 Hz 持续写成 `1`，转储函数 `VolcarGuest` 于是裸读 `dev + 10896 = 0x2A91`（未映射）⇒ xmemory 致命路径 ⇒ **启动即退出**。v8.7-IRQABI 只做两件事：**设备的唯一来源改为 `r4` 且必须通过设备地址校验后才落盘**；**转储里任何 guest 地址（dev/ctx/fence/暂存块）读前先校验，不健全即跳过并计数**。不新增 cvar、不改任何默认值、不碰 v8.6 呈现记账机制。

---

## 1. 崩溃现场（逐项自洽）

| 项 | 值 | 说明 |
|---|---|---|
| 致命项 | `Unhandled guest access violation: read of guest 0x2A91` | 读了一个从未映射的 guest 地址 |
| `pc` | `imagen+0x2e28544` = `t2rotf::video_nativo::(anonymous)::VolcarGuest+0x2c4` | **宿主**符号，非客体重编译代码 |
| `lr` | `imagen+0x2e29240` = `VolcarGuest+0xfc0` | 同一函数内调用返回点 |
| `esr` | `0x92000005`（EC `0x24`） | Data Abort，低位地址/权限相关 |
| 指令 | `0xb8604b54` = `ldr w20,[x26,w0,uxtw]` | 以 `x26 + x0(截断 32 位)` 为地址装载 |
| `x0` / `x1` | `0x2A91` / `0x2A95` | 两个待读字段的**地址** |
| `x8` / `x9` | `0x2A90` / `0x2A94` | 两个待读字段的**偏移** |
| `x19` | `1` | 设备基址局部变量（**被污染成 1**） |
| `x26` | `0x5762c4e000` | arena 上的宿主别名基址 |
| `far` | run1 `0x5762C50A91` / run2 `0x72602B8A91` | = arena base + `0x2A91` |

**自洽链条**：`x19 = dev = 1` ⇒ `dev + 10896 = 1 + 10896 = 0x2A91 = x0`；`x1 = 1 + 10896 + 4 = 0x2A95`；`x8/x9 = 0x2A90/0x2A94`（相对 `dev` 的偏移 `10896`/`10900`）。`nm -n` 反查未 strip 的 `recomp/out/sw8/T2ROTF`：`g_dev_visto = 0x3ed03a0`、`g_ctx_visto = 0x3ed03a8`，`objdump -d --start-address=0x2e28490 --stop-address=0x2e28620` 反推该处 `x19` 正是从 `g_dev_visto` 装入 ⇒ **`g_dev_visto` 被写成了 `1`**。

**触发时序**（`rex_stderr(72).log` + `T2ROTF_001(70).log`/`T2ROTF_002(51).log`）：

1. `OnInitialize` 全链走完：`RexGmInit(0xE0000000)` → `base=0x72602b6000 size=0x120000000` → SetupPresentation → ConstructRuntime → `LaunchModule done`（`RexGmInit: Direction F map FAILED rc=0x0000D401` 是本工程已知的按需 fault-in 策略声明，非本轮根因）。
2. `[video] T2ROTF BUILD v8.6-FLIP …` 在跑；`[T2DIAG PERRO pump]` / `PERRO2 pump` / `PERRO ctx=41CCC7E0 st15504=7` 之后（`00:00:43.494`）进程退出。
3. 泵钩子 `sub_827CE598` 内的 `VolcarGuest("pump")` 与看门狗 `VolcarGuest("perro")` 都对 `dev` 取字段转储——`REXLOG_ERROR` 的**参数求值先于日志限频判断**，所以即便限频只打前几行，`dev + 10896` 也已经在求值阶段被裸读。

---

## 2. 根因定论（T63）

| # | 定论 | 证据 |
|---|---|---|
| D1 | **`sub_822D47A0` 的 ABI 是 `(r3 = 来源, r4 = 设备)`**：`r3=0` 为垂直同步，`r3=1` 为提交完成；设备恒在 `r4`。 | v8.4 老日志只有 `irq sub_822D47A0 r3=00000000 r4=40070600`；v8.6 日志出现 `n=2 r3=00000001 r4=40070600`、`n=3 r3=00000000 r4=40070600`、`n=4/6/8 r3=00000001` 与 `r3=00000000` 交替 —— `r4` 始终是设备。 |
| D2 | 老钩子把**来源值当设备**写全局：`if (ctx.r3.u32) g_dev_visto.store(ctx.r3.u32, …)`。 | `recomp/src/t2rotf_video_nativo.cpp` 的 `REX_HOOK_RAW(sub_822D47A0)` 原文。 |
| D3 | v8.5/v8.6 新增「每垂直同步派发 `source=1`」后，`g_dev_visto` 被 60 Hz 写成 `1`。 | `t2rotf_gpu_signal.cpp` 的 `Completado` → `ExecuteInterrupt(…, kCallbackGuest=0x822D47A0, {fuente, datos}, 2)`；实测约每 16 ms 一拍。 |
| D4 | 转储函数对 `dev` 无校验即裸读 `dev + 10896`，`1 + 10896 = 0x2A91` 未映射 ⇒ xmemory 致命 ⇒ 启动即退出、零加载。 | 崩溃 `pc` 落在 `VolcarGuest` 内、`x19=1`、`x0=0x2A91`、`far = base + 0x2A91`。 |

**为什么前一版没炸**：v8.4 之前没有「派发 `source=1`」，`g_dev_visto` 只在 `sub_822D4488`/`sub_822D9570` 钩子里被写成真实设备 `0x40070600`，即使钩子偶尔取错值也很快被压回；v8.5/v8.6 高频的 `source=1` 派发把窗口从「偶发」变成「恒定」，于是每次转储必炸。

**已排除**（与日志不符，勿再走）：

- 不是 v8.6 新增的 `PresentarGuest → sub_822D9688` 代客呈现记账：`[T2DIAG PRESENT n=1/2/3]` 三次都成功（`16564`/`16568`/`16548` 递增、`blk4=0`），且崩溃线程/`pc` 都不在该路径。
- 不是客体重编译代码自身访存 bug：`pc`/`lr` 都落在**宿主** `VolcarGuest`（`t2rotf_video_nativo.cpp`）符号内，且 `imagen+X == ELF vaddr`（delta=0）已核。

---

## 3. 本轮改动（8 处代码 + 版本标记；**不新增 cvar、不改默认值**）

全部在 `recomp/src/t2rotf_video_nativo.cpp`（76,899 → **80,874** 字节）与 `recomp/src/t2rotf_gpu_signal.cpp`（74,911 → **75,030** 字节）：

1. 新增判定器 `inline bool EsDireccionDispositivo(uint32_t a)`：`a != 0 && (a & 3) == 0 && a >= 0x10000000 && a <= 0xFFFFFFF0`（实机设备 `0x40070600`、fence/暂存块 `0xBDF80000` 通过；来源值 `0`/`1` 被拒）。同处新增计数 `std::atomic<uint64_t> g_perro_saltos{0}`、`g_irq_dev_avisado{0}`。
2. `REX_HOOK_RAW(sub_822D47A0)`：**设备改取 `ctx.r4.u32`**，校验通过才 `g_dev_visto.store` / `g_base_host.store`，否则 `g_irq_dev_avisado++` 并限频打印 `[video] T2DIAG irq sub_822D47A0 设备候选 r4=… 不健全（r3=… 为来源），忽略`；观测行改为 `n=… fuente=r3 dev=r4 …`。
3. `REX_HOOK_RAW(sub_822D9570)`：`cand = EsDireccionDispositivo(r3) ? r3 : r4`，校验通过才落盘，且只用 `cand` 调 `::t2rotf::gpu_signal::NotificarDispositivo`。
4. `REX_HOOK_RAW(sub_822D4488)`：`if (ctx.r3.u32)` → `if (EsDireccionDispositivo(ctx.r3.u32))`。
5. `REX_HOOK_RAW(sub_822D13F0)`：新增 `const bool okdev = EsDireccionDispositivo(dev);`，`p` 改 `okdev ? Leer32(base, dev+10896) : 0`，`g_rk_pv` 用 `EsDireccionDispositivo(p) ? Leer32(base, p) : 0`，`g_rk_v`/`g_rk_f10888`/`g_rk_f10941`/`g_rk_f11008` 全改 `okdev ? … : 0`。
6. `VolcarGuest`：`if (ctx)` → `if (EsDireccionDispositivo(ctx))`、`if (dev)` → `if (EsDireccionDispositivo(dev))`；`f10896`/`f10900` 读出后再校验成 `b10896`/`b10900`；新增 `else if (dev)` 分支 `g_perro_saltos++` + 限频打印 `[video] T2DIAG PERRO {} dev=… 不是设备地址（守卫跳过，不再裸读 guest；saltos=…）`。
7. `t2rotf_gpu_signal.cpp` L418-427：BUILD 标记改为 **`v8.7-IRQABI`**，把根因（`r3=来源 / r4=设备` 的 ABI 混淆 → `g_dev_visto=1` → 裸读 `0x2A91` → xmemory 致命）与两条硬约束（设备只取 `r4` 且校验后才落盘；转储里任何 guest 地址先校验再读）写进构建字符串，并声明 **v8.6-FLIP 呈现记账、v8.4-SPLIT、v8.2 影子安全访存与 yield 语义全部保留**（另修一处拼写 `poisonsing`→`poisoning`）。
8. 复核 `grep -B2 "g_dev_visto.store"`：三处落盘点（`t2rotf_video_nativo.cpp` L1088 / L1124 / L1143）**全部位于 `EsDireccionDispositivo` 分支内**，设备值已无处可被来源值污染。

**明确未动**：`recomp/src/t2rotf_gpu_signal.h`（1,593 字节）、`recomp/src/renut_engine/gpu/d3d/hooks/device.cpp`（103,645 字节）、`T2ROTF.toml`（24 键、42,369 字节）；不写任何显示会计字段、不碰门/泵热路、不重复投递。

---

## 4. 产物与校验

| 产物 | 字节数 | sha256 |
|---|---|---|
| `recomp/out/sw8/T2ROTF.nro` | 59,949,810 | `db089080b6360c68cd49df8e1dfbfb6f63eae1da550002b28b9761ec7e2ef2d4` |
| `T2ROTF.toml`（未改动） | 42,369 | `6217dfa41e43926c3dde0bcf0653611832d4b9c0f6f72ca402bcf7565b84eecc` |
| `recomp/src/t2rotf_video_nativo.cpp` | 80,874 | `7a651be71c56ab19324b82bb5885a8f365790e9a8a2e53124a32f277c0b342bd` |
| `recomp/src/t2rotf_gpu_signal.cpp` | 75,030 | `2b950833ead31fb85cf8c4d8c7989b688c3f455e1d4324af54caac36fe005267` |
| `recomp/src/t2rotf_gpu_signal.h`（未改动） | 1,593 | `626c2e0a1e6f054288e013cd8addde77251ec77e55ec2dee8ff2889c368fcdca` |
| `recomp/src/renut_engine/gpu/d3d/hooks/device.cpp`（未改动） | 103,645 | `3329f920c5b2dcaf9f10a309fcf520dd6c6d09eb9ece6469e1ae1c7c1de177ed` |

- 构建：`recomp/out/sw8` 执行 `ninja -j4 T2ROTF` ⇒ **退出码 0**（`[2/4] t2rotf_gpu_signal.cpp.obj`、`[3/4] t2rotf_video_nativo.cpp.obj`、`[4/4] Linking … Packaging T2ROTF.nro`）；再跑一次为 no-op（「0 written, 0 unchanged… Done in 1.7s」）⇒ 构建已完整收尾且可复现。
- 字符串取证（NRO 内 `grep -a -o -c`）：`v8.7-IRQABI` = 1、`不是设备地址` = 1、`设备候选` = 1。（NRO 里仍有 `v8.6-FLIP` = 1 / `v8.5-COMPLETE` 等字样，来自新 BUILD 文本里的历史说明与旧 cvar 帮助文本，属预期，**不要用它判版本**。）
- 存档：`T2ROTF`（未 strip 的 ELF）= 71,322,232 字节，供后续 `nm`/`objdump` 取证。
- 工程包：`T2ROTF_switch_port.zip` 已**原地更新**（脚本 `recomp/out/sw8/_upd_pkg_t63.py`；首包快照 `recomp/out/sw8/zip_backup/T2ROTF_switch_port_20261101_v87_pre.zip`）。

---

## 5. 开关语义与回退

- **本轮不新增开关、不改任何默认值**：修的是「误读 ABI」这个 bug，不是新增机制，因此**没有对应的可回退开关**。
- `T2ROTF.toml` 仍为 **24 键**（`sa2_renut_display_present_guest=true`、`sa2_renut_gpu_signal_hostcb_s1=true`、`hostcb_s1_mode=2`、`display_agent=false`、`display_release=false` 等全部沿用 v8.6 语义），均为 `kInitOnly`——**改任何键后必须完全退出再启动**。
- 若需回到上一版行为：用 `recomp/out/sw8/zip_backup/` 下的快照包（本轮为 `…_20261101_v87_pre.zip`，即 v8.6 状态）。

---

## 6. 实机验证步骤与需回传日志行

1. 把新的 `recomp/out/sw8/T2ROTF.nro` + `T2ROTF.toml` 覆盖到 `sdmc:/switch/Transformers2ROTF/`，**完全退出再启动**。验收场景是**不超频手持**（「不超频 60 帧」结论只在**不超频**条件下给）。
2. 最关键验收点（按优先级）：
   - **不再产生 `rex_crash.log`**，且 `[video] T2DIAG PERRO dev=40070600 …` 一行能**正常打印**（原崩溃点被修好的直接证据）；
   - `irq sub_822D47A0 n=… fuente=0/1 dev=40070600`（设备恒为 `40070600`，不再被来源值覆盖）；
   - `OnInitialize: LaunchModule done` 之后能进到**影片/呈现阶段**；v8.6 画面链的验收点（第 3.17 节）照旧生效：`16568` 是否脱离 5、`dentro` 是否回 0、`fin=`/fps 是否 > 0、是否出现三连 logo。
3. 需回传日志行：`[video] T2ROTF BUILD v8.7-IRQABI …`；`[video] T2DIAG irq sub_822D47A0 …`（前 12 条）与 `… 设备候选 … 不健全 …`（若有）；`[video] T2DIAG PERRO dev=40070600 …`（首末各一）与 `… 不是设备地址 …`（若有）；**全部** `[video] T2DIAG PRESENT …`；`[T2DIAG GPSIG …]` 与 `[T2DIAG GPSIG2 dev=…]`（首末各一，重点 `A0..C2`、`dv10904`、`blk0/blk4`）；`[T2DIAG DRIVE …]`（首末各一，重点 `dentro`）；`[T2DIAG SWAP …]`（末条）；`rex_perfil.log` 的 `juego … fps` **与** `relojes:` 行（须为未超频值）；`rex_crash.log`（若产生）。

---

## 7. 判读分支（下一轮照此走）

| 分支 | 现象 | 结论与下一步 |
|---|---|---|
| A | `PERRO dev=40070600` 正常打印、无 `rex_crash.log`、能进影片/呈现阶段 | **启动崩溃已修**；问题回到 v8.6 的画面链（冲 `16568`/`base+284` 释放者与 GPU 提交完成路径） |
| B | 仍有 `rex_crash.log` | 回传新 `pc/far/esr` 前 40 行（可能还有第二处未校验的裸读点） |
| C | `PERRO … 不是设备地址（守卫跳过…）` 一直涨 | 转储守卫生效，但设备解析仍错：回传 `GPSIG2` 的 `A0..C2` 与 `dev` |

---

## 8. 残余风险与下一轮方向

- **本版尚未实机验证**：改动只经逐指令离线定论 + 构建 + 产物字符串取证。
- v8.7 **只修「启动即崩」**，未触碰 v8.6 的画面链（呈现记账 / 在飞显示项释放）；启动崩溃修好后**预期回到**「黑屏、有无声无画 / 管线停在数帧」那一类问题。
- `table[slot]+24/+28` 与 `base+284` 的**释放者仍未定位**（候选：guest GPU 提交完成路径 `sub_822D9838`/`sub_822D98E8` 轮询 `blk+4` 之后；`VdSetSystemCommandBufferGpuIdentifierAddress` 注册地址可能正是 `blk+4`）。
- `g_dev_visto` 仍有「来源值偶尔压过设备值」的历史机制（`sub_822D4488` 钩子偶尔写回设备）；本轮已加校验使其不可能再被来源值污染，但**该全局的语义**（为何要两个钩子竞写）仍值得下一轮统一为「只认 `r4` 且校验后落盘」。
- **T26（不超频实测帧率）仍被实机阻塞**；「不超频流畅 60 帧」**仍未达成也未实测**。
- TU 升级档与 DLC 包在产物层已就绪（工程内 `update/`、`dlc/`、`patched/default.xex` 15,937,536 字节 > base 14,200,832 字节），**实机闭环验证同样待画面出现后进行**。