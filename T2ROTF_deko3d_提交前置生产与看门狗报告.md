# T2ROTF deko3d 后端 —「提交前置生产 + 看门狗」报告（v8.9-PRODUCE / T68→T70）

- 工程：《变形金刚2：卷土重来》（Transformers: Revenge of the Fallen，Xbox 360）→ Nintendo Switch
- 栈：`rexglue-sdk` + `hedge-dev/XenosRecomp` 静态重编译 + devkitPro/libnx + **Renut / deko3d**（plume + deko3d + uam）
- 本轮用户描述：**「黑屏，但是第一个 LOGO 有声音」**
- 用户回传日志：`rex_perfil(35).log`、`rex_stderr(74).log`、`T2ROTF_001(72).log`、`T2ROTF_002(53).log`
- 本轮产物：`T2ROTF.nro`（**59,962,098 字节**，sha256 `ceb43ebf408de7d3b84b85511569ffcc9d52b1df01db06416839ac5459912470`）
  + `T2ROTF.toml`（**56,632 字节 / 27 键**，sha256 `a032dd6009ca33d6bc394538efd1c41d99b2169acfbe45c88440605fe6813ef0`）
- 唯一改动源码：`recomp/src/t2rotf_gpu_signal.cpp`（79,963 → **93,103 字节**，sha256 `452ff0d328a98d86414764d927333976fad047e866a2fa09e77117d889e913f3`）
- 版本标记：日志里 `[video] T2ROTF BUILD v8.9-PRODUCE …`

---

## 1. 结论（一句话）

**v8.8 的补产分支排在「每帧提交入口」之后，而该提交入口一进去就把驱动线程永久留在 guest 的显示回调里 —— 于是补产一次都没跑到；本版把顺序反过来（先投递、后可选提交），把提交入口默认关掉，并加上「卡死即落盘快照并停驱动」的看门狗，让下一次实机日志不再靠猜。**

---

## 2. 本轮实机判读（v8.8-LIVENESS，T68）

### 2.1 版本与环境：不是崩溃、不是超频、不是缺页

| 核对项 | 证据 | 判定 |
|---|---|---|
| 运行版本 | `rex_stderr(74)`：`v6.27-MAPFIX crash hooks init ok` | 这是**常量串**，不是版本；版本只看 BUILD 行 |
| 超频 | `[apm] PUERTA la configuracion 0x92220008 (tabla: GPU 460, EMC 1331): GPU 460.8 MHz (era 307.2)` | **未超频**（460.8 / 1331 是原厂档） |
| 历史既定项 | `RexGmInit: … Direction F map FAILED / code-memory path unavailable` | 早前版本就有，非本轮问题 |
| 启动链 | `OnInitialize` 五段全过、无启动期 fatal | 启动正常 |
| 崩溃 | 无 `rex_crash` 文件 | 与「挂住而非崩」一致 |
| 中断派发链 | `vblanks` / `irq` / `mmio_r` 持续增长、`inv == ret` | 健康（v8.4-SPLIT 的自锁确已解除） |
| 音频 | `audio: 8 bloques de cliente mezclados, 0 solicitudes sin datos, 1/1 buffers audout con PCM no nulo, 0 muestras saturadas … pico maximo 0.474` | **有声**（独立于显示链） |
| 帧率 | `juego 0.0 fps`；`nucleos del proceso 3 (mascara 0x7)` | 黑屏 0 帧 |

### 2.2 驱动线程：只进过 guest 两次，第二次之后再没回来

`[T2DIAG DRIVE]`（`T2ROTF_001(72)`）：

- 首条（17.606）：`16548=94 16568=94 284=0 288=1 obj=40F604E0 ev=F8000068`
- 尾部（33.605 / 35.605 / 37.606 三行完全相同）：
  `drive=2 ok=1 ini=2 dentro=1 noObj=0 fallos=0 vivo=45 produce=0 prodSinDev=0 estanc=1 16548=1174 16568=1174 284=0 288=1 obj=41370080 ev=F8000068 mmio_w=0`

⇒ `ini=2`、`ok=1`、`dentro=1`：驱动线程**只成功走完 1 次、第 2 次进去就再没出来**，且此后 `vivo`（让位计数）几乎不涨 —— 线程已经卡在 guest 内部，根本轮不到判活判据。补产计数 `produce=0 prodSinDev=0`：**补产一次都没跑到**。

### 2.3 卡点：排空链把这一帧交付出去之后，卡在该项的呈现回调里

`DRAIN-IN` 共 13 条，最后一条：

```
[T2DIAG DRAIN-IN n=13 idx=0 c24=1 c28=0 e24=1 e28=0]     (19:09:18.865, 线程 t6136820238206709458)
```

此后**再无出栈**（同一线程先前是 `PRESENT n=1200` 的存续者、也出现过 `RECON … retro=1`）。

结合离线逐指令精读：

- `sub_82881E00`（门 + 泵）在就绪时会 `bctrl(r3 = &item)` 调 **item+60 槽的虚表呈现回调**，**回调返回之后**才 `atomic_dec(manager+288)`。
- 实机 `base+288` 自 17.597 起**恒为 1**、`base+284` 恒为 0 ⇒ 该 `atomic_dec` 从未执行 ⇒ 泵**卡在该呈现回调里不返回** ⇒ 驱动线程永久 `dentro=1`。

### 2.4 黑屏的直接原因：`284=0 288=1` 恒定，三处排空判据永不满足

- `T2DIAG PUERTA`：n=17（16.730）曾出现 `284=0 288=0`（说明呈现回调**曾经能返回**），但 17.597 起 `284=0 288=1` 恒定；
- `SWAP n=1 dev=0x40070600 front=0x82e55fc4 sub16560=1 disp16568=0 fr16564=0 f10941=0x00`（19:09:16.018）—— **全程仅 1 次 Swap**；
- `[T2DIAG espera]` 末条 `n=9780000 r3=F8000068 r4=1 r5=0 lr=828818DC` —— 呈现线程（`sub_82881848`，退出核心判据 `base+284 != 0`）在句柄 `F8000068 = *(0x82DBB5E0)` 上**零超时热轮询**，涨到 978 万次。

⇒ 显示管理器里三处 `while (*(base+284) + *(base+288) != 0)`（排空 `sub_82882200`、排空+复位 `sub_828828E0`、会话+排空 `sub_82882740`）**永远转不出去**；`SWAP` 停在第 1 次 ⇒ 我们注入的影片帧无处上屏 ⇒ **黑屏**；XAudio 侧完全独立 ⇒ **第一段 LOGO 有声音**。

### 2.5 另一个佐证：诊断线程还活着，显示管理器整体停摆

`YIELD total` 顶部 `[82881DE8 x582875] [82881918 x582860]`，**19:09:31 之后冻结**；而同一时段的 `espera`（进入 `sub_824F0620` 的计数）继续涨到 19:09:38 ⇒ 「进入该调用的计数在涨、返回的计数停了」，指向**有线程卡在某次 `sub_824F0620` 内未返回**（与 2.3 的推定一致）。

### 2.6 根因的机制侧定位（宿主代码）

读 `recomp/src/t2rotf_gpu_signal.cpp`（v8.8 版）：

- `DirigirFotograma` 里 **line 1100 无条件**调用 `DirigirEntrada(kCommitGuest = 0x82880978, cpu)` —— **没有 cvar 开关**；
- 补产（`sa2_renut_display_drive_produce` → `sub_828823F8` → `PlausibleObjeto()` → `sub_82882730`）在 **line 1122**，**排在提交之后**。

⇒ 提交一次即永久 `dentro=1`，生产支路一次未走 —— 这正是 `produce=0 prodSinDev=0` 的机制。

### 2.7 本轮新增疑问（已在代码侧用「看门狗」直接回答）

卡死的**呈现回调（item+60 虚表）到底在等什么**、`YIELD` 冻结指向的 `sub_824F0620` 是 `KeWaitForSingleObject` 还是别的，都还无法从日志单独断定。**v8.9 的做法是不再靠猜**：用相位名 + 全量快照把「卡在哪一段」一次性落盘（见第 3 节 e）。

---

## 3. 本版改动（v8.9-PRODUCE）

**只改 `recomp/src/t2rotf_gpu_signal.cpp`。** 红线不变：宿主**一个字都不写** `284/288/16548/16552/16564/16568`；所有 guest 访存仍走 `RexGmShadowFor` 影子别名；**驱动只跑在独立线程 `T2ROTF Dirigir` 上**（垂直同步线程绝不进 guest 代码，`t2rotf_gpu_signal.cpp` 的这条硬守保留）；不碰门/泵热路。

| # | 改动 | 位置 |
|---|---|---|
| a | **顺序反转**：先投递、再（可选）提交 | `DirigirFotograma` |
| b | 新增 **G17 `sa2_renut_display_drive_commit`（默认 false）**：把 `sub_82880978` 包起来，默认不驱动 | cvar 声明区 + `:1214` |
| c | **G14 补产前移**到提交之前独立执行（`ProducirFotograma`，只在 `base+284 == 0` 时投递） | `:1104`、`:1211` |
| d | 新增 **G18 `sa2_renut_display_drive_pump_max`（默认 0 = 关）**：有界非阻塞交接，最多 N 次调 `sub_82881E00(r3 = 0x82E55E20, r4 = -1, r5 = 1)`；新增带 `r4/r5` 的 `DirigirEntradaArgs` 变体 | `:1073`、`:1223` |
| e | 新增 **G19 `sa2_renut_display_drive_watchdog_ms`（默认 500）**：卡死超阈值打一次 `[T2DIAG DRIVE-WEDGE]` 全量快照并**永久停用**驱动 | `:1345`、`:1142` |
| f | 诊断增强：DRIVE 行新增 `commit= / pump= / bombeos= / muerto= / wedge_ms= / fase= / t_guest=`；新增 `[T2DIAG PRODUCE …]` | `:1378`、`:1115`、`:1126` |

关键代码语义：

- `ProducirFotograma(cpu, n)`：`fase=resolver` → `sub_828823F8` 取「当前呈现记录」（`*(kRegArray)` 起每条 296 B、`+0 == 5`）→ 过 `PlausibleObjeto()` → `fase=productor` → `sub_82882730(rec)`（入队 + 自增 `base+284` + 置位帧事件 `*(0x82DBB5E0)`）。取不到合法记录即**跳过**并计 `prodSinDev`（**绝不以 `r3 = 0` 调用生产者本体** —— 它首条 `lwz r10,64(r3)` / `stw r11,56(r3)` 会读写 guest `0x40/0x38`）。
- 提交入口 `sub_82880978` 只在 `sa2_renut_display_drive_commit = true`（A/B）时才调。
- 泵只在 `pump_max > 0` 且 `(base+284 | base+288) != 0` 时调，`r5 = 1` 保证**非阻塞**（门不就绪即刻返回），最多 N 次。
- 看门狗：`inicio_guest_ms_` 记进入时刻，`InformeDirigido()`（每 2 s）发现 `dirigido_en_curso_ && now - inicio >= 阈值` 就打快照 + `dirigidos_muerto_ = true`，此后 `DirigirFotograma` 直接返回。

新增/变更 cvar 一览（**24 → 27 键**，全部 `kInitOnly`，改动后必须**完全退出再启动**）：

| 键 | 默认 | 说明 |
|---|---|---|
| `sa2_renut_display_drive_commit` | **false** | 每帧提交入口是否仍由宿主驱动（默认关） |
| `sa2_renut_display_drive_pump_max` | **0** | 有界非阻塞交接次数上限（0 = 关） |
| `sa2_renut_display_drive_watchdog_ms` | **500** | 卡死看门狗阈值（毫秒；0 = 关） |
| `sa2_renut_display_drive_produce` | true（不变） | 补产，**已前移到提交之前** |

---

## 4. 校验（构建级与静态级）

1. **构建**：`ninja -j4 T2ROTF` 退出码 **0**；`[1/51] Generating recompiled code` 报 `0 written, 0 unchanged, 0 deleted, 1 module(s) up to date`（本轮只改宿主侧 C++，属预期），随后链接与 `Packaging T2ROTF.nro` 正常。
2. **NRO 字节级取证**（`recomp/out/sw8/T2ROTF.nro`，59,962,098 字节 / `ceb43ebf…`）：

   | 标记 | 计数 |
   |---|---|
   | `v8.9-PRODUCE` | 5 |
   | `v8.8-LIVENESS` | 1 |
   | `v8.8b refinements` | 1 |
   | `sa2_renut_display_drive_commit` | 5 |
   | `sa2_renut_display_drive_pump_max` | 5 |
   | `sa2_renut_display_drive_watchdog_ms` | 5 |
   | `DRIVE-WEDGE` | 3 |
   | `T2DIAG PRODUCE` | 2 |
   | `commit sub_82880978` / `pump sub_82881E00` / `resolver sub_828823F8` | 1 / 1 / 1 |
   | `producer sub_82882730` | 3 |
   | `prodSinDev` | 3 |
   | `ProducirFotograma` | 2 |
   | `write guest 0x40/0x38` | 1 |

   > NRO 里仍会有 `v8.8-LIVENESS` / `v8.8b` 字样，来自新 BUILD 文本里的历史说明与旧 cvar 帮助文本，**不要用它判版本** —— 判版本只认日志 BUILD 行。
3. **配置**：`T2ROTF.toml` 经 `tomllib` 解析通过、读回 **27 键**；`drive_commit=false`、`drive_pump_max=0`、`drive_watchdog_ms=500`、`drive_produce=true`、`present_guest=true`、`hostcb_s1=true`、`hostcb_s1_mode=2` 全部正确。
4. **未改动源码逐字节核对**：`recomp/src/t2rotf_video_nativo.cpp`（80,874 字节 / `7a651be7…`）、`recomp/src/t2rotf_gpu_signal.h`（1,593 字节 / `626c2e0a…`）、`recomp/src/renut_engine/gpu/d3d/hooks/device.cpp`（103,645 字节 / `3329f920…`）与本轮之前一致。
5. **包完整性**：`T2ROTF_switch_port.zip` 已**原地更新**（旧包 85 条目 → 新包 **86 条目**），并通过逐项「包内 == 磁盘」逐字节断言：包内替换 `T2ROTF.nro`（`ceb43ebf…`）、`T2ROTF.toml`（`a032dd60…`）、`README.md`（**230,061 字节** / `6ac921ad8b3d7618e21fe14dc62e44838519cd28d7ea4559ac188c7f316f53cd`）、`recomp_src/t2rotf_gpu_signal.cpp`（`452ff0d3…`），并追加本报告；三个未改动源码在包内与磁盘逐字节一致。
   > 包体总字节数与 sha256 **不在此写死**——本报告本身也在包内，由 `_upd_pkg_t70.py` 每次运行实算并打印。
   > 回退快照：`recomp/out/sw8/zip_backup/T2ROTF_switch_port_20261102_v89_pre.zip`（27,700,310 字节，即更新前的 v8.8b 包）。

---

## 5. 实机验证清单（下一轮请按此回传）

1. 把新的 `T2ROTF.nro` + `T2ROTF.toml` 覆盖到 `sdmc:/switch/Transformers2ROTF/`，**完全退出再启动**；验收场景是**不超频手持**。
2. 需回传的日志行：
   - `[video] T2ROTF BUILD v8.9-PRODUCE …`（确认版本）；
   - `[T2DIAG DRIVE …]`（**首末各一条**，重点 `drive` / `ok` / `produce` / `prodSinDev` / `estanc` / `dentro` / `estanc` / `commit` / `pump` / `bombeos` / `muerto` / `wedge_ms` / `fase` / `t_guest` / `284` / `288` / `obj` / `ev`）；
   - `[T2DIAG PRODUCE …]`（前 4 条 + 末条，重点 `rec=` 是否非 0、`284` / `288`）；
   - `[T2DIAG DRIVE-WEDGE …]`（**若出现**，`fase=` 直接指出卡在哪一段）；
   - `[T2DIAG GPSIG …]`（首末各一）、`[T2DIAG SWAP …]`（末条）、`[T2DIAG espera …]`（末条）；
   - `rex_perfil.log` 的 `juego … fps` 与 `relojes:` 行（须未超频值）；`rex_crash.log`（若产生，附 `pc` / `far` / `esr` 前 40 行）。
3. 判读分支：
   - **A**（`produce > 0` 且 `284` 出现非 0、`SWAP n > 1`、`espera` 停止增长、出现三连 logo、`fps > 0`）= 缺环补上，进入「不超频 60 帧」实测（待办 T26）；
   - **B**（`produce > 0` 但 `284` 仍恒 0）= 连 guest 原生生产者都没能入队，下一轮冲 `sub_822B5608` 的入队前置条件与 `0x82DBB5E4` / `0x82DBB5D4` 两个逃逸标志的写入者；
   - **C**（`284` 变非 0 但 `SWAP n` 仍 1）= 消费者没被唤醒，下一轮冲 `base+284` 的**释放者**（候选 `sub_822D9838` / `sub_822D98E8`）；
   - **D**（`prodSinDev` 持续涨）= `sub_828823F8` 解析不出合法呈现记录，根因上移到「谁建立 `*(0x82DBB5C4)` 里 `+0 == 5` 的那条记录」；
   - **E**（出现 `DRIVE-WEDGE`）= 驱动仍卡死，但 `fase=` 已把卡点缩到「解析器 / 生产者 / 提交 / 泵」四选一，下一轮直接对那一段做离线精读；
   - **F**（仍有 `rex_crash.log`）= 回传新 `pc` / `far` / `esr` 前 40 行。
4. 本轮**仍不包含**帧率达标：画面出来后才能在不超频条件下实测（见待办 T26）。

---

## 6. 回退阶梯

全部为 `kInitOnly`，改后必须**完全退出再启动**：

`display_drive_produce=false`（退到「不投递，只可选提交」）→ `display_drive_commit=false`（本版默认，已是最保守）→ `display_drive_pump_max=0`（本版默认）→ `display_drive_watchdog_ms=0`（关看门狗）→ `present_guest=false`（退 v8.5 语义）→ `hostcb_s1=false`（退 v8.4 语义，T61 已证其为空操作）→ `display_drive_thread=false`（退 v8.3，实机会自锁）→ `display_drive=false`（退 v8.2-SHADOW）→ `gpu_signal_dev_hook=false`（退 v8.1）→ `gpu_signal=false`（退 v7.7）。
整版回退请用 `recomp/out/sw8/zip_backup/` 下的快照包：`T2ROTF_switch_port_20261101_v87_pre.zip`、`T2ROTF_switch_port_20261101_v88_pre.zip`。

---

## 7. 残余风险与未决疑问

1. **本版尚未实机验证。** 机制上的期望是：补产先跑（能投出帧）、提交默认关（不再卡死驱动线程）、看门狗兜底（万一仍卡，至少落盘卡点）。
2. 卡死的**呈现回调（item+60 虚表）到底在等什么**仍未定 —— 等宿主 Renut 的 present 完成？还是等 `F8000068`（若回调内部自等即成自锁）？v8.9 的 DRIVE-WEDGE 相位快照会给出第一手证据。
3. `base+284` 的**释放者仍未定位**（候选落在 `sub_822D9838` / `sub_822D98E8` 的 `blk+4` 轮询之后）。
4. `sub_828823F8` 在卡死态能否返回合法记录**未知**；若恒返回 0，则本轮实际生效的只有「顺序反转 + 提交默认关」。
5. 双宿主线程（注入线程 `t16871629957265061218` + 驱动线程 `t6136820238206709458`）先后进同一**无锁**显示管理器是否为 17.597 后整链停摆的诱因，未定。
6. `pump … fin=0/0/0 anillo=0/0` 与 `ringR16700 = ringW16704 = 0`、`inKick13232 = 0` 是否同因，未定。
7. 「不超频流畅 60 帧」**仍未达成也未实测**；TU/DLC 实机闭环仍在画面出现之后。

---

## 8. 相关文件

- 上一轮报告：`T2ROTF_deko3d_驱动判活与提交补产报告.md`（v8.8-LIVENESS / T65/T66）
- 再上一轮：`T2ROTF_deko3d_启动崩溃IRQABI报告.md`（v8.7-IRQABI / T63/T64）
- 工程验证清单：`README.md` 第 3.20 节（本版）；第 3.19 / 3.18 / 3.17 节（历史）
- 打包脚本：`recomp/out/sw8/_upd_pkg_t70.py`（可重复运行）