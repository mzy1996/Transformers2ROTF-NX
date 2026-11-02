# T2ROTF（Xbox 360《变形金刚2：卷土重来》→ Nintendo Switch）deko3d 移植
# 每帧提交驱动分独立线程（v8.4-SPLIT / T56）报告

- 本轮版本：**v8.4-SPLIT**（任务 T56）
- 上一版：v8.3-FRAME（T53）
- 产物：
  - `recomp/out/sw8/T2ROTF.nro` = **59,937,522 字节**，sha256 `9d78b9572d9f81c2cd487b5e6f5fe5f379e66c98be91f826070432e32c80fa3f`（上一版 v8.3/T53 = 59,929,330 字节 / sha256 `9059ed99ab8ab85ce31840dff01ebc1c94e750372ffb31247c7ba3535debe99a`，字节数与哈希都不同）
  - `T2ROTF.toml` = **38,315 字节**（22 个 `sa2_*` 键；新增 1 键 `sa2_renut_display_drive_thread=true`）
  - 源码改动：仅 `recomp/src/t2rotf_gpu_signal.cpp`（56,659 → **64,966 字节**）
  - 首包快照：`recomp/out/sw8/zip_backup/T2ROTF_switch_port_20261030_v83_pre.zip`
- 用户描述：**实机依旧黑屏**

---

## 1. 本轮背景

v8.3-FRAME 的目标是补上「谁来回调 guest 自己的每帧显示提交」：v8.2-SHADOW 实机证明显示中断链已经通了
（`irq`/`mmio_r`/`r1951`/`hostcb` 全增长），但 guest 自己的每帧显示提交入口 `sub_82880978` 从未被调用，
导致每帧提交侧一次都不产出（`SWAP n=1`、`disp16568` 冻在 5）。v8.3 遂在**垂直同步宿主线程**的每个真实刷新里，
经 `FunctionDispatcher` 替 guest 走一次 `sub_82880978`。

本轮用户回传 v8.3-FRAME 的实机日志四份：

| 文件 | 行数 | 用途 |
|---|---|---|
| `rex_perfil(33).log` | 2655 | 帧率、线程 CPU、时钟（`relojes:`）、APM 配置 |
| `rex_stderr(70).log` | 25 | 标准错误、崩溃提示 |
| `T2ROTF_001(68).log` | 1994 | `[video]` 主日志：BUILD 行、`[T2DIAG …]` 探针 |
| `T2ROTF_002(49).log` | 2114 | 后续主日志分卷 |

判读结论：v8.3 的驱动**确实成功过一次**，但整条链在 6 帧后停摆；**无崩溃、无缺页**。

---

## 2. 实机判读（v8.3-FRAME）

### 2.1 版本与崩溃面

- 版本标记在跑：
  `[video] T2ROTF BUILD v8.3-FRAME (host drives the guest's own per-frame display commit sub_82880978 on every refresh; shadow-safe guest reads kept from v8.2)`
- **无 `rex_crash.log`**、**无缺页**：`[T2DIAG GPSIG3 hookDev=40070600 sinSombraL=2 sinSombraW=0 sinSombraAvisada=1]`，
  `sinSombraL` 恒 2、`sinSombraW` 恒 0，说明 v8.2 起的 shadow 安全访存通路正常。

### 2.2 驱动成功、管线跑了 6 帧

```
[T2DIAG DRIVE n=153 obj=40F604E0 284 0->1 288=0 16568=5 16548=132]
[T2DIAG PERRO] perro pumpIn=6 pumpOut=6 pre1=6 pre2=8 post470=8 prep=8 cadena937=8 cadena9bf=8
               despacho=8 etapa1/2/3=6/6/6 fin=0/0/0 bomba=6 iny=6 pres=6 anillo=0/0 externo=7
               irq47=130 irq95=130 cc058=6 cc6e0=0 yield=88183
```
- 宿主把 guest 自己的每帧提交叫起来了：`base+284` 由 **0 → 1**。
- 管线真的跑了 6 帧：`bomba/iny/pres=6`、`etapa1/2/3=6/6/6`、`cadena937=8`、`despacho=8`。

### 2.3 但整链在 6 帧后停摆（关键证据）

- **垂直同步派发线程最后一条输出**（其后 60 s 再无一字）：
  ```
  [1970-01-01 00:12:48.048] [t1969709635156801032] [video] T2DIAG DRAIN-IN n=13 idx=0 c24=1 c28=0 e24=1 e28=0
  ```
  注意：`DRIVE n=153`（00:12:47.985）也由**同一条线程** `t1969709635156801032` 发出。
- **GPSIG 周期报告停止**：只有 `00:12:47.431 vblanks=120 irq=94 mmio_r=94 mmio_w=0`，下一行 2 s 报告**再也没来**。
- **完成计数/中断计数全线冻结**：`irq47=irq95=130` 之后不动、`[T2DIAG SWAP n=1]` 全程**仅 1 次**、`fin=0/0/0`、
  `yield=88183` 冻住。
- **无明显进展的等待仍在增长**：`espera sub_824F3580 n` 从 2.88e7（~31,040,000）仍单调增长到 3.108e7。
- **首样本至末样本一字不差**：`[T2DIAG PERRO]` 的 `bomba=6 iny=6 pres=6 fin=0/0/0 … yield=88183` 全程不变。

---

## 3. 根因定论（T55）

v8.3 把「替 guest 走每帧提交」这件事**放在了垂直同步派发线程上**。而 guest 的每帧提交链
（`sub_82880978 → … → 排空 sub_82882200`）会走到排空判据：

```
while (*(base + 284) + *(base + 288) != 0)   // base = 0x82E55CE0
```

上**等一个显示完成**。在本移植里，这个显示完成的**唯一来源**正是**同一条垂直同步线程**派发的显示 ISR
（v8.0 的 G 组补齐的信号源 + v8.1 的代客派发，都是在这条线程的刷新节奏上调的）。

⇒ **同一线程自锁**：驱动线程进入 guest 排空后等待一个只有它自己能派发的完成事件，于是它自己把自己锁住，
**中断派发随之被掐掉**（`vblanks`/`irq`/`mmio_r`/`hostcb` 全线冻结），完成侧 `fin` 永不产出，整链 6 帧后停摆。

证据链闭合点：线程最后一条日志恰好落在 `DRAIN-IN`（就在 guest 排空入口），此后所有中断侧计数停止增长——
**不是 guest 卡住，是中断派发本身被自己掐掉了**。

> 这也解释了为什么「有声音」（影片播放线程已把音频送去）而「无画面」（显示提交侧被自锁冻结）。

---

## 4. 修复（v8.4-SPLIT）

只改 `recomp/src/t2rotf_gpu_signal.cpp`（+8,307 字节）：

1. **新增独立宿主线程 `T2ROTF Dirigir`**（`ArrancarHiloDir()`，`XHostThread`，128 KB 栈，入口 `BucleDirigido()`）：
   按 `hz`（G2，默认 60）节拍累加 `dirigidos_tics_` 并调 `DirigirFotograma(cpu, n)`——
   即 v8.3 那套驱动逻辑**原样搬家**，但**只在这个线程上执行**。
2. **`thread_local g_en_hilo_dirigido` 守卫**：`DirigirFotograma` 在
   `sa2_renut_display_drive_thread=true` 且**非驱动线程**时**直接返回**；
   垂直同步线程从此**绝不进入 guest 代码**，只做「派发中断（`Interrupcion(0)` / `Completado`）+ 打报告」。
   这样即使驱动卡在 guest 提交内部，中断派发照常进行，guest 的完成侧才有机会被释放——**这正是 v8.3 自锁的解药**。
3. **让位语义原样保留**：`dev+16568` 或 `base+284` 相对上次一变（管线自己动起来），立即让位 3 次刷新（不叠加提交，
   绝不把呈现做成双份）；G12（`after_ms`，默认 2500）、G13（`div`，默认 1）对驱动线程同样生效；
   当前显示对象 `*(0x82DBB608)` 不可用时跳过并计数（`noObj=`）。
4. **新增判读字段**：`[T2DIAG DRIVE drive=… ok=… ini=… dentro=… noObj=… fallos=… vivo=… produce=…]`
   ——`ini` 是进入 guest 提交的次数、`dentro` 是驱动线程此刻是否停在 guest 提交内。
   **`ini > ok + fallos` 就说明驱动线程当场卡在 guest 提交内**，一眼可判。
   驱动线程启动/停止各打一行 `[video] T2ROTF GPSIG: 每帧提交驱动线程已启动/已停止 …`。
5. **退出安全阀**：若退出时驱动线程仍卡在 guest 提交内，`Shutdown` **不等待**它（只在日志点名），
   避免应用退出被拖死。
6. 版本标记改为
   `[video] T2ROTF BUILD v8.4-SPLIT (the per-frame display commit driver now runs on its own dedicated thread instead of the VSync dispatch thread: …)`；
   新增 1 个 cvar `sa2_renut_display_drive_thread`（G15，默认 true，`kInitOnly`）。

**边界（与既往一致，未减弱）**：不写任何显示会计字段、不碰门/泵热路、不改 v8.2 已验证的 shadow 安全访存、
`presenter()/provider()` 保持 null（画面仍由 Renut/deko3d 出）。`t2rotf_gpu_signal.h` 与
`t2rotf_video_nativo.cpp` **未改动**。

---

## 5. 构建与静态取证

- `ninja -j4 T2ROTF`（构建目录 `recomp/out/sw8`）退出码 **0**；
  再跑仍 no-op：`Codegen summary: 0 written, 0 unchanged, 0 deleted, 1 module(s) up to date`，
  NRO 的 mtime/sha256 不变 ⇒ 构建已完整收尾且可复现。
- NRO = `recomp/out/sw8/T2ROTF.nro`，**59,937,522 字节**，sha256 `9d78b9572d9f81c2cd487b5e6f5fe5f379e66c98be91f826070432e32c80fa3f`。
- NRO 字符串取证（`grep -a -o -F … | wc -l`）：

| 字符串 | 命中数 |
|---|---|
| `v8.4-SPLIT` | 2 |
| `每帧提交驱动线程已启动` | 1 |
| `sa2_renut_display_drive_thread` | 4 |
| `T2ROTF Dirigir` | 1 |
| `T2DIAG DRIVE` | 2 |
| `T2ROTF BUILD` | 3 |

- `T2ROTF.toml` = **38,315 字节**，经 `tomllib` 解析通过、**22 键**读回正确
  （`sa2_renut_display_drive_thread = True`）。
- > 本工程产物内嵌构建时间戳：**同一份源码重构建后体积相同、哈希会变**，文档与包内哈希一律以随包 NRO 实算值为准。

---

## 6. 开关语义（本轮相关）

| 键 | 默认 | 语义 |
|---|---|---|
| `sa2_renut_display_drive_thread`（G15，**新增**） | **true** | 每帧提交驱动（G11）只在独立线程 `T2ROTF Dirigir` 上执行；垂直同步派发线程绝不进入 guest 代码。`false` = 退回 v8.3 行为（驱动与中断派发共用垂直同步线程），仅供 A/B 复现自锁。 |
| `sa2_renut_display_drive`（G11） | true | 是否驱动 guest 自己的每帧显示提交 `sub_82880978`。**v8.4 起由 G15 决定由哪个线程执行。** |
| `sa2_renut_display_drive_after_ms`（G12） | 2500 | 开始驱动前的等待毫秒数（避开 boot/init）。 |
| `sa2_renut_display_drive_div`（G13） | 1 | 每 N 个刷新驱动一次（A/B 二分用）。 |
| `sa2_renut_display_drive_produce`（G14） | false | 驱动时是否额外投一条（默认关，避免重复投递）。 |
| `sa2_renut_display_guard`（A） | true | 显示完成链守卫（`*(0x82DBB608)==0` 时短路四个 guest 函数）。 |
| `sa2_renut_gpu_signal` / `_hostcb` / `_dev_hook` | true | 显示完成信号源 / 代客派发 / 设备钩子兜底（回退键）。 |

---

## 7. 残余风险

1. **本版尚未实机验证**。
2. **v8.4 只解除自锁**（让中断派发不被驱动拖住），**并不保证** guest 完成侧一定会因此产出 `fin`。
   若下轮 `vblanks` 持续增长而 `[T2DIAG SWAP n=]` / `disp16568` 仍不动，则说明**完成侧另有独立卡点**
   （例如：完成事件的对象/句柄 `*(0x82DBB5A0)=F8000060` 的释放者仍未被正确触达），下一轮须改判驱动点或补完成侧。
3. v8.3 的驱动依赖当前显示对象 `*(0x82DBB608)`（`kDispActual`）非空；历史上该地址零命中，本轮 `DRIVE n=153` 行
   显示 `obj=40F604E0` 非空 ⇒ 该前置条件已满足，但驱动点是否**最优**仍待实机结论。
4. 「不超频流畅 60 帧」**仍未达成、也未实测**（见 README 第 5 节与待办 T26）。

---

## 8. 实机验证步骤与需回传的日志行

1. 把新的 `T2ROTF.nro` + `T2ROTF.toml` 覆盖到 `sdmc:/switch/Transformers2ROTF/`，**完全退出再启动**。
   验收场景是**不超频手持**。
2. **先确认版本**：日志里出现
   `[video] T2ROTF BUILD v8.4-SPLIT (the per-frame display commit driver now runs on its own dedicated thread instead of the VSync dispatch thread: …)`，
   且出现 `[video] T2ROTF GPSIG: 每帧提交驱动线程已启动（独立于垂直同步派发线程；after_ms=2500 div=1）`。
   （应用自身 banner 仍是旧的 `v6.27-MAPFIX`，**不要用它判版本**。）
3. **本轮**最关键**验收点**：`[T2DIAG GPSIG vblanks=…]` 是否**持续**增长（v8.3 就是在 120 停住的）——
   这是自锁是否解除的直接判据；再看 `[T2DIAG DRIVE …]` 的 `ini` 是否持续增长、`ini - ok - fallos` 是否稳定为 0
   （`dentro` 恒 0 = 每次驱动都正常返回）；最后看 `284`/`288`/`16568`/`mmio_w` 与 `[T2DIAG SWAP n=]` 是否增长、
   `juego … fps` **> 0**、**画面出现**三连 logo。
4. 需回传的日志行：
   - `[video] T2ROTF BUILD v8.4-SPLIT …`
   - `[video] T2ROTF GPSIG: 每帧提交驱动线程已启动 …` / `已停止` / 卡死不等待的提示行
   - `[video] T2DIAG DRIVE …`（首末各一，含 `ini=`/`dentro=`）
   - `[video] T2DIAG SWAP …`（末条）
   - `[T2DIAG PERRO …]` 的 `fin=` 字段
   - `[T2DIAG GPSIG …]`（首末各一）
   - `[video] T2ROTF GPSIG:`（全部）
   - `rex_perfil.log` 的 `juego … fps` 与 `relojes:` 行（须为未超频值）
   - `rex_crash.log`（若产生，连前 40 行）

---

## 9. 回退阶梯（改键后必须重启）

1. `sa2_renut_display_drive_thread = false` → 退回 **v8.3-FRAME** 行为（驱动与中断派发共用垂直同步线程，实机会自锁）。
2. `sa2_renut_display_drive = false` → 退回 **v8.2-SHADOW**（中断链通、每帧提交侧不产出）。
3. `sa2_renut_gpu_signal_dev_hook = false` → 退回 **v8.1** 设备解析行为（shadow 安全访存保留）。
4. `sa2_renut_gpu_signal_hostcb = false` → 退回 **v8.0**（只派发 guest 自己注册的回调，实机等于不派发）。
5. `sa2_renut_gpu_signal = false` → 退回 **v7.7-ALIGN**（门永不就绪、0 fps 黑屏）。

---

## 10. 历史报告

- `T2ROTF_deko3d_每帧提交驱动修复报告.md`（T53，v8.3-FRAME）
- `T2ROTF_deko3d_显示内存影子访存与设备分层解析报告.md`（T50，v8.2-SHADOW）
- `T2ROTF_deko3d_显示中断回调代客派发报告.md`（T47，v8.1-GUESTCB）
- `T2ROTF_deko3d_显示完成信号源补齐报告.md`（T44，v8.0-GPUIRQ）
- `T2ROTF_deko3d_投递通道错位与对齐修复报告.md`（T41，v7.7-ALIGN）
- `T2ROTF_deko3d_排空自旋根因与完成侧配平报告.md`（T38，v7.6-RECON）