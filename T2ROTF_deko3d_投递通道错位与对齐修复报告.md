# T2ROTF deko3d 黑屏根因：显示投递通道错位与对齐修复报告（T41 / v7.7-ALIGN）

- 游戏：Transformers: Revenge of the Fallen（Xbox 360，Title ID `41560834`，Media ID `338852BF`）
- 工程：`J:\XBOX360 port\Transformers Revenge of the Fallen`（ReXGlue / XenosRecomp + Renut/deko3d + devkitPro/libnx）
- 本版产物：`recomp/out/sw8/T2ROTF.nro`
- 版本标记（日志判据）：`[video] T2ROTF BUILD v7.7-ALIGN`
- 本轮结论：**黑屏的直接原因不是「缺少 GPU 完成中断」，而是「投递者与消费者用的不是同一条显示通道」**——
  guest 每帧把画面投递到 `sub_828823F8()` 解析出来的「当前呈现设备」的通道上，而排空/泵/门/交付这条消费链
  恒定用显示对象 `0x82E55E20` 的通道，于是门的就绪计数永远读不到、泵 0 次成功、排空判据
  `while (*(0x82E55CE0+284) + *(0x82E55CE0+288) != 0)` 永久自旋 ⇒ `0 fps`、画面全黑。
  本版只做一件事：**把投递的通道基址对齐到消费者实际读/弹的那条通道**，不加不减任何会计。

---

## 1. 上一版（v7.6-RECON）实机判读

用户回传日志组：`rex_perfil(29).log`、`rex_stderr(65).log`、`T2ROTF_001(63).log`、`T2ROTF_002(44).log`
（SD 路径 `sdmc:/switch/Transformers2ROTF/logs/rex/`）。

| 观测项 | 实机读数 | 判读 |
| --- | --- | --- |
| 版本标记 | `[video] T2ROTF BUILD v7.6-RECON` 出现 | v7.6 的修复本体确实在运行（不是旧包） |
| 应用自身 banner | `v6.27-MAPFIX` | 已知陈旧，**不能**用作版本判据 |
| RECON 命中 | `T2DIAG RECON nr=1 n=8 idx=0 c24=0 c28_antes=1 e28_antes=1 retro=1` | 只命中 **1 次**就收敛，之后再没触发 |
| 悬挂形态 | 由 `284=0 / 288=1` 变成 **`284=1 / 288=0`** | 只把在飞项清掉，**没有**解决「门为什么永远不就绪」 |
| 门三槽就绪计数 | `PUERTA … cnt=0/0/0`（`0x82E55F30/34/38` 全程 0） | 门看的 arm 计数**恒 0** |
| 后备通道标记 | `PUERTA … obj256=0/0/0`（`0x82E55CE0+256/260/264` 全 0） | 后备路径同样恒 0 |
| 泵 | `PERRO2 … ddrain=5/12 dpump=16591560/0/16591558` | 泵被进入 1659 万次、**成功 0 次** |
| 生产/交付调用数 | `ddeliv=6 dprod=7 dprod_map=7 wchi=1 swlist=4 u0=2/0/10 oq=1 q=0` | 投递 7 次、交付只被调用 6 次 |
| 帧率/占用 | `juego 0.0 fps`；`Game Master` 线程 CPU 115%；`nucleos del proceso 3`；SoC 46.7–46.8 °C | 帧循环整体未推进 |
| 时钟 | `relojes: CPU 1020.0 MHz / GPU 460.8 MHz / memoria 1600.0 MHz` | **未超频**（上限对照 CPU 1785 / GPU 921 / 内存 1600 MHz） |
| 崩溃 | `rex_stderr(65)` 仅 1 行 banner（1572 字节） | 无访问违例、无崩溃 |

⇒ **v7.6 的「完成侧配平」是无效修复**（它只改了悬挂的形态，没碰到门就绪的真正前置条件）；
但它没有引入新故障（不回绕、不崩溃），可以作为对照保留。

---

## 2. 真因：投递通道与消费者通道错位

本轮把整条链条（排空 `sub_82882200` → 泵 `sub_82881E00` → 门 `sub_82881B88` → 交付 `sub_82881930`）
与生产者 `sub_822B5608`、每帧 tick `sub_82ABB7D0`、设备解析器 `sub_828823F8` 逐条读完，得到一条完整的、
可与实机读数逐位对上号的因果链。

### 2.1 生产者（投递侧）

`sub_822B5608(r3 = obj, r4 = A)` 只做四件事（A 为「通道基址」）：

| 动作 | 地址表达式 | 含义 |
| --- | --- | --- |
| `++*(entry + 24)` | `entry = *(0x82E55CE0 + 272) + *(obj + 64) * 32` | 该通道 `待交付` 计数 +1（记在**管理器自己的**条目表里） |
| `++*(0x82E55CE0 + 284)` | 全局待交付 | 排空判据的第一项 |
| **`++*(A + 248)`** | A 由调用方给 | **通道 arm 计数 +1（全程序唯一的 arm 自增点）** |
| `sub_822B57E0(A + 8, obj)` | A + 8 | 把对象挂进**该通道的队列** |

`A` 从哪来？每帧 tick `sub_82ABB7D0`：

```
bl 0x82882568                     ; 取「当前呈现对象」
若返回 0 → 用后备通道 A = 0x82E55CE0 + 8
否则     → A = 返回对象 + 24      ; 也就是 dev + 24
bl 0x822b5608                     ; obj 来自 sub_828823F8() 选出的设备
```

`sub_828823F8()` 在数组 `*(0x82DBB5C4)`（计数 `*(0x82DBB5C0)`，步长 296）里线性挑出**第一个
`*(entry + 0) == 5` 的对象**并返回它 —— 即「当前呈现设备」。实机证据表明它返回的**不是**
`0x82E55E20`（探索探针读到该对象的 `+0` 状态是 0，不是 5）。

### 2.2 消费者（排空/泵/门/交付侧）

| 环节 | 关键地址 | 说明 |
| --- | --- | --- |
| 排空 `sub_82882200(r3 = idx)` | `idx == -1` → `0x82E55CE0+284/+288`；否则 → `*(0x82E55CE0+272)+idx*32` 的 `+24/+28` | `while (a + b != 0) sub_82881E00(container, idx, 1)` |
| 泵 `sub_82881E00` | 门成功 → 调 obj 的虚表回调 → `--*(0x82E55CE0+288)` → `--*(entry[idx]+28)` | **真正推进一帧的地方** |
| 门 `sub_82881B88(ctx)` | 就绪读 **`*(*(ctx + 16) + 272 + 4*slot)`**；后备读 `*(*(0x82E55CE0) + 256 + 4*slot)` | ctx = `0x82E55E20`；slot0「计数非 0 即就绪」 |
| 交付 `sub_82881930(ctx = 0x82E55E20 + 24)` | 从 **`*(ctx) + 248 + 4*slot`** 扣减；弹 **`ctx + slot*80 + 8`** | 入口先读该计数，**为 0 立即返回 0** |

门户/交付读与弹的位置，只有在 **`A == 0x82E55E20 + 24`** 时才和生产者写的位置重合：

| | 生产者写 | 消费者读/弹 | 相等条件 |
| --- | --- | --- | --- |
| arm 计数 | `A + 248` | `*(0x82E55E20+16) + 272 + 4*slot` | `A = 0x82E55E20 + 24` 且 `*(0x82E55E20+16) = 0x82E55E20`（自指，init 语义） |
| 通道队列（slot 0） | `A + 8` | `(0x82E55E20+24) + 0*80 + 8` | 同上 |

### 2.3 因果闭环（与实机读数一一对应）

1. 每帧 tick 投递到 `dev + 24`（dev 来自 `sub_828823F8()`），**arm 计数加到别人通道上、队列挂到别人队列上**；
2. 门读的是 `0x82E55E20` 自己的计数 ⇒ **恒 0**，后备标记也恒 0 ⇒ 三槽全不就绪 ⇒ 门每轮末尾 `yield`（`LR = 0x82881DE8`）；
3. 交付被调用时先读 `*(0x82E55E20+24)+248 = 0` ⇒ **立即返回 0**，不弹记录、不减 `284`、不加 `288`；
4. 泵拿不到「门就绪」⇒ 完成回调不执行、`288` 不回收 ⇒ 排空判据永不为 0 ⇒ **无限自旋**；
5. 表现出来就是：`cnt=0/0/0`、`obj256=0/0/0`、`dpump 成功 0 次`、YIELD 千万级集中在 `82881DE8`、
   `juego 0.0 fps`、`CPU 115%`（纯自旋）。

### 2.4 为什么早期还有 6 次交付

实机 `ddeliv=6` / `dprod=7`：初始化阶段（设备布局尚未切换、或投递时机落在自指窗口内）有过几次
「投递—交付」配对成功；一旦进入每帧循环，投递就稳定落到别的设备通道上，交付再也没被成功调用过。
这也解释了「`284` 能到 1 而门读的 arm 计数始终是 0」这一对看似矛盾的读数。

### 2.5 与 T36 结论的关系（修正）

T36 定论「`*(base+288)` 的唯一完成点是泵的完成回调，本移植缺 VSync/GPU 完成中断源」——
**方向正确但不完整**：`288` 的悬挂是**结果**，前置条件是「门必须就绪」，而门就绪依赖那条 arm 计数。
本次修复后 guest 自己的交付→泵→回调这条链就能跑通，`288` 由泵按 guest 语义回收；只有在
**真实 GPU 完成事件源仍然缺失**的场合才需要额外处理，本版不预置任何补写。

---

## 3. 本版改动（v7.7-ALIGN）

只改一个文件：`recomp/src/t2rotf_video_nativo.cpp`（72,255 → 76,583 字节）。
改动落在既有的 `REX_HOOK_RAW(sub_822B5608)` 内，**每次投递 1 次**（每帧至多 1 次，绝不在门/泵热路上）：

1. 新增开关 **H `sa2_renut_display_align`（默认 `true`，`kInitOnly`）**；
2. 调原体**之前**，把投递通道基址 `ctx.r4` 改写为 **`0x82E55E20 + 24`**（消费者侧实际读/弹的那条通道）；
3. 若管理器自身的 `*(0x82E55E20 + 16)` / `*(0x82E55E20 + 24)` 不是自指（与 init 语义不一致），
   先修回自指，保证门的读位置与交付的弹位置不分叉；
4. 诊断行（限频）：
   - `[video] T2ROTF BUILD v7.7-ALIGN`（每进程一次，**版本判据**）
   - `[video] T2DIAG ALIGN n=… a_antes=… dev_antes=… a_ahora=… obj=… o64=… g74bc=… c272=… c284=… c288=… b256=…`（前 16 次 + 每 512 次）
   - `[video] T2DIAG ALIGN-SELF n=… c16=… c24=…`（前 8 次）

**没有**做的事：不动 `284` / `288` / `entry+24` / `entry+28`，不加不扣任何会计，不在门/泵热路写内存，
不替 guest 生成完成事件。

---

## 4. 构建与静态校验（本机已完成）

| 项 | 值 |
| --- | --- |
| 构建命令 | `cd recomp/out/sw8 && ninja -j4 T2ROTF` |
| 退出码 / 耗时 | **0** / 73 s（codegen 输出 "0 written, 0 unchanged, 0 deleted, 1 module(s) up to date"） |
| 产物 | `recomp/out/sw8/T2ROTF.nro` = **59,859,698 字节**，sha256 `38e341ee7f4cb4e553f2b2eaadc256dd313fbf7bfb31e352a62e59e60ff135a8` |
| 上一版（v7.6/T38） | 59,855,602 字节，sha256 `421923cee8c53a66be9b6e0c268d5576f68d139c6e59be81ae7b7156a42b10bb` |
| 字符串取证 | `T2ROTF BUILD v7.7-ALIGN`=1、`sa2_renut_display_align`=4（含 cvar 注册符号）、`T2DIAG ALIGN`=2、`ALIGN-SELF`=1；旧标签 `v7.6-RECON` / `sa2_renut_display_reconcile` / `sa2_renut_display_guard` 仍在 |
| 源码 | `recomp/src/t2rotf_video_nativo.cpp` = 76,583 字节 sha256 `fbd5fa88ad4fcb7927f02cb8760836e5c880de559f96882b2cba06607b3eb560` |
| 源码级回退件 | `recomp/src/t2rotf_video_nativo.cpp.bak.t41` = **72,255 字节**（= v7.6 源，字节数与 v7.6 文档记载一致）sha256 `c51dbd5f022c9522cb2e652c51356a9e3db52370c4ce1e1654bed0fbb138649f` |
| 配置 | `T2ROTF.toml` = **11,668 字节** sha256 `92095288472bd831eaf97c5f0ea7f66f193d7632d5c8e6c6400371ffc4f6be2c`（**7 个键**，新增 `sa2_renut_display_align = true`） |

> 哈希纪律：NRO 内嵌构建时间戳，同一源码重构建体积相同、哈希不同；**一切以随包 NRO 的实算哈希为准**。

---

## 5. 实机验证步骤（请按顺序执行）

1. 把新的 `T2ROTF.nro` + `T2ROTF.toml` 覆盖到 `sdmc:/switch/Transformers2ROTF/`，**完全退出再启动**，
   **不要超频**（本轮不需要超频；超频会污染「不超频 60 帧」这一验收口径）。
2. **先确认版本**：日志里应出现且只出现一次
   `[video] T2ROTF BUILD v7.7-ALIGN (display post channel aligned with the drained channel)`。
   应用自身 banner 仍是旧的 `v6.27-MAPFIX`，**不要用它判版本**。
3. **本轮验收点（按重要性排序）**：
   - `[video] T2DIAG ALIGN n=1 … a_antes=???  dev_antes=???  a_ahora=82E55E38` —— 对齐确实发生；
     `dev_antes` 就是「guest 认为的当前呈现设备」，请照原样回传；
   - `PERRO2` 的 `dpump` **成功数脱离 0**（形如 `dpump=<进入>/<成功>/<失败>`，成功数 > 0）；
   - `PUERTA … cnt=…/…/…` 至少有一槽非 0（门开始看到 arm 计数）；
   - `[video] T2DIAG RECON …` **不再出现**（其触发条件即死锁特征，对齐生效后不应再命中）；
   - `YIELD total` 比上一版断崖式下降（不再是 1600 万级）、不再集中于 `82881DE8`；
   - `rex_perfil.log` 的 `juego … fps` **> 0**；`Game Master` 线程 CPU 从 115% 回落；
   - **画面出现**（三连 logo luxo→atvi→hasbro）。
4. **需回传的日志行**（各取末条或首末各一）：`[video] T2ROTF BUILD v7.7-ALIGN`、
   `[video] T2DIAG ALIGN …`（首条与末条）、`[video] T2DIAG ALIGN-SELF …`（若有）、
   `[video] T2DIAG PERRO2 …`（末条）、`[video] T2DIAG PUERTA …`（首末各一）、
   `[video] T2DIAG YIELD …`（末条）、`[video] T2DIAG RECON …`（若有）、
   `[renut] T2DIAG COLA …`（若有）、`rex_perfil.log` 的 `juego … fps` 行与线程 CPU 列表、
   `rex_crash.log`（若产生）。
5. **回退**：`sa2_renut_display_align = false`（退回 v7.6 行为：门永不就绪 + 排空自旋的 0 fps 黑屏，仅供对照）；
   源码级回退用 `recomp/src/t2rotf_video_nativo.cpp.bak.t41`（= v7.6 源）。
   其余开关维持原默认：`sa2_renut_display_guard=true`、`sa2_renut_display_reconcile=true`（对齐生效后它不会触发）、
   `sa2_renut_display_release=false`、`sa2_renut_display_agent=false`、`sa2_renut_display_tick=false`、
   `sa2_renut_swap_callthrough=false`。**7 个键都是 `kInitOnly`，改完必须重启游戏。**

---

## 6. 残余风险与下一里程碑

1. **本版仍未包含帧率达标**：解锁帧循环 ≠ 60 帧。只有画面先出来，才能在**不超频**条件下实测
   菜单/游戏内帧率、GPU/CPU 占用与瓶颈项，再给「不超频 60 fps」的达标结论或优化项（待办 T26）。
2. 若对齐后门能就绪、交付能弹出对象，但**画面仍黑**，则下一处要看的是**泵的完成回调**：
   泵在门成功后调用对象虚表 `*(obj + 156)` 指向的第一个虚方法（真正的「呈现这一帧」入口），
   宿主 deko3d/Renut 后端需要在该调用里完成实际提交；这与「显示完成事件源缺失」是两个问题，
   届时应按 `T2DIAG PERRO2` 与 `[renut]` 侧日志分清是「回调没执行」还是「回调执行了但没上屏」。
3. `*(0x82DA74BC)`（每帧排空用的 `idx`）与投递对象 `*(obj+64)` 是否一致，是下一处可能的第二个错位点：
   门的子通道前置判据要求 `idx` 之下的子通道 `+24`/`+28` 全清才允许投递；本轮 `g74bc` 与 `o64`
   都被写进 `T2DIAG ALIGN` 行，若验证时 `o64 != g74bc`，请回传该行，下一轮按它修。
4. 若 `ALIGN-SELF` 反复命中（同一进程内多次），说明 guest 自己在运行期重写了 `0x82E55E20+16/+24`，
   需要重新审视「当前设备/当前通道」的发布者；该行已限频到前 8 次，成本可忽略。