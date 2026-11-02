# T2ROTF_deko3d 驱动判活与提交补产报告（v8.8-LIVENESS，T65/T66）

- 项目：T2ROTF —《变形金刚2：卷土重来》（Xbox 360）→ Nintendo Switch 的 ReXGlue 静态重编译移植
- 渲染后端：Renut / deko3d（plume + deko3d + uam）
- 本轮版本：`v8.8-LIVENESS`（上一版 `v8.7-IRQABI`）
- 构建时间：2026-11-01
- 本轮改动范围：**只改 `recomp/src/t2rotf_gpu_signal.cpp` 一个文件**（外加文档/开关说明）

## 产物摘要

| 文件 | 大小 | sha256 | 与上一版（v8.7）的关系 |
|---|---|---|---|
| `recomp/out/sw8/T2ROTF.nro` | 59,949,810 B | `d1a59c332b0460dcf09bf0d2bdf44c3577dd04ffab9dcef2828a4f821e9d8b5e` | 上一版 59,949,810 B / `db089080…`（字节数相同、哈希不同）。交付前自查又修掉两处缺陷（见 §4.5）后重建，**本报告数字一律以随包 NRO 实算值为准** |
| `T2ROTF.toml` | 48,067 B / **24 键** | `db88b7208153ecad039aa08a7de12d335efea7243d221194b68be742e457ad27` | 上一版 42,369 B / 24 键（本轮因 G14 默认值翻真 + v8.8/v8.8b 版本说明重写而变大，**键数仍为 24**） |
| `recomp/src/t2rotf_gpu_signal.cpp` | 79,963 B | — | 上一版 75,030 B（本轮唯一改动的源码；含 v8.8 让位判据与 v8.8b 两处修正） |
| `recomp/src/t2rotf_video_nativo.cpp` | 80,874 B | — | 逐字节未改动 |
| `recomp/src/t2rotf_gpu_signal.h` | 1,593 B | — | 逐字节未改动 |
| `recomp/src/renut_engine/gpu/d3d/hooks/device.cpp` | 103,645 B | — | 逐字节未改动 |

> 本工程产物内嵌构建时间戳：**同一份源码重构建后体积相同、哈希会变**，文档与包内哈希一律以随包 NRO 实算值为准。
> 工程包 `T2ROTF_switch_port.zip` 由原地更新脚本 `recomp/out/sw8/_upd_pkg_t65.py` 更新，包体字节数、条目数与 sha256 由该脚本运行实算并打印。

## 1. 本轮实机日志（用户回传，描述「依旧黑屏有声」）

回传文件：`.dumate/inbox/rex_crash(38).log`、`rex_stderr(73).log`、`T2ROTF_001(71).log`、`T2ROTF_002(52).log`。

关键行（原文摘录）：

- 版本：`T2ROTF_001(71).log:31` 与 `T2ROTF_002(52).log:32` 均为
  `[video] T2ROTF BUILD v8.7-IRQABI (startup-abort fix: the guest interrupt callback sub_822D47A0 has ABI (r3 = source …, r4 = device); …)`
- 启动全链：`rex_stderr(73).log` 走到
  `[saltynx] PUBLICADO a los 0.02 s del arranque: FPS=0 y RES=1280x720 en el desplazamiento 0xBC`、
  `[apm] PUESTA la configuracion 0x92220008 (tabla: GPU 460, EMC 1331): GPU 460.8 MHz (era 307.2) y la memoria se queda en 1600.0 MHz`、
  `[DIAG] OnInitialize: LaunchModule done`，**之后没有任何 crash hook 输出**。
- 设备解析：`[video] T2DIAG irq sub_822D47A0 n=1 fuente=0 dev=40070600 r5=00000000 r6=00000000`、
  `n=2 fuente=1 dev=40070600 r5=00000000 r6=82DC0000`（`fuente` 在 0/1 之间按刷新交替，设备恒为 `40070600`）。
- 地址守卫：在 001/002 两份日志里 grep `不是设备地址` / `设备候选` **零命中**。
- 驱动线程：`[T2DIAG DRIVE …]` 首行 `drive=0 ok=0 ini=0 dentro=0 noObj=0 fallos=0 vivo=0 produce=0 16548=0 16568=0 284=0 288=0 mmio_w=0`，
  末行 `drive=0 ok=0 ini=0 dentro=0 noObj=0 fallos=0 vivo=8733 produce=0 16548=8855 16568=8855 284=0 288=1 mmio_w=0`。
- 中断派发链：`[T2DIAG GPSIG …]` 从 `vblanks=0 irq=0 … s1=0` 涨到
  `vblanks=8882 irq=17710 r1951=8855 mmio_r=8855 mmio_w=0 hostcb=8855/27 s1=8855 noblk=0 skipS1=27 inv=17710 ret=17710`。
- 交换：`[renut] T2DIAG SWAP n=1 dev=0x40070600 front=0x82e55fc4 sub16560=1 disp16568=0 fr16564=0 f10941=0x00`（**全程仅 1 次**）。
- 呈现侧自旋：`[video] T2DIAG espera sub_824F3580 n=68680000 r3=F8000068 r4=1 r5=0 lr=828818DC`（002 末条；001 到 `n=12680000`）。
- 全量转储：`[T2DIAG PERRO dev=40070600 vblank16548=8894 tick16552=3323529175 sub16560=5 fr16564=8894 disp16568=8894 ringR16700=0 ringW16704=0 v23376=… inKick13232=0 b10896=BDF80000 b10900=BDF80080 w10900_0=00000000 w10900_4=00000000 w10896_0=0000001A]`。

## 2. 判读结论：先排除三项，再给出唯一因果链

**排除一：启动崩溃已修好，本轮没有新崩溃。**
`rex_crash(38).log` 与上一轮 `rex_crash(37).log` 用 `cmp` 逐字节比对**完全相同**（该文件是上一轮遗留文件被重复上传，不是本轮新产生的崩溃）。同时 `rex_stderr(73).log` 走完了 `SetupEnvironment → RexGmInit → SetupPresentation → ConstructRuntime → LaunchModule done` 全链，而 v8.6 的崩溃正是在 `LaunchModule done` 之后立刻发生的。⇒ v8.7-IRQABI 的修复成立。

**排除二：设备地址解析正常，守卫从未触发。**
`irq sub_822D47A0` 的 `dev` 恒为 `40070600`（不再是旧版的 `00000000/00000001`），两份日志里 `不是设备地址`、`设备候选` 均零命中。⇒ v8.7 的 `(r3 = 来源, r4 = 设备)` ABI 修正生效。

**排除三：中断派发链一直在跑，且跑得很好。**
`vblanks` / `irq` / `r1951` / `mmio_r` / `hostcb` / `s1` 六项持续增长（约 60 Hz 的 2 倍节奏），`inv == ret`（`17710/17710`），说明宿主代客派发的显示 ISR 进入 guest 后**每次都正常返回**，没有卡在 guest 中断里。

**残余问题只有一处**：`drive=0`、`ok=0`、`produce=0`、`284=0`、`mmio_w=0`、`ringR16700=ringW16704=0`、`inKick13232=0`、`SWAP n=1`、呈现线程在 `F8000068` 上自旋 6,868 万次。⇒ **guest 每帧提交侧一次都没被驱动起来**，画面链断在这一环。

## 3. 根因链：驱动线程被 v8.6 自己伪造的记账「饿死」

1. **v8.6-FLIP 的代客呈现记账是每刷新调用的。** 宿主在垂直同步派发线程里，每个刷新周期调用一次 guest 自己的呈现记账函数 `sub_822D9688(r3=0)`；该函数在 guest 侧自增 `dev+16564` 并在条件满足时自增 `dev+16568`、清 `*(dev+10900)+4`。实机证据：`16568` 与 `16548`（垂直同步计数）**完全同步增长**（末态 `16548=8894`、`16568=8894`、`fr16564=8894`），而 v8.6 日志里 `16568` 冻在 5、`16548` 已涨到 2738。⇒ `16568` 现在是「每秒 60 次必变」的字段。

2. **v8.3/v8.4 的驱动让位判据把 `dev+16568` 也算作「guest 管线在动」。** 旧判据是
   `cambio = (d16568 != vivo_16568_) || (c284 != vivo_284_)`，只要有一个字段相对上次变了就让位。
   既然 `16568` 每刷新都变，`cambio` 就**每次为真** ⇒ 驱动线程每轮都走让位分支返回。
   实机证据：`vivo`（让位计数）从 0 单调涨到 **8733**，而 `drive`（真正进入 guest 提交的次数）、`ok`、`ini`、`fallos` 全是 **0**，`noObj=0`（当前显示对象是合理的，不是被对象判据挡下）。

3. **guest 侧因此从不产出帧。** 驱动从未进入 `sub_82880978`（每帧提交入口），guest 原生生产者 `sub_822B5608` 也就从未运行 ⇒ `base+284` 恒 0、换页环 `ringR16700=ringW16704=0`、`mmio_w=0`、`inKick13232=0`、帧事件 `*(0x82DBB5E0)`（`F8000068`）永不被置位。

4. **呈现线程死在这个事件上。** `sub_82881848` 的退出判据 `*(base+284) != 0` 永假，于是它经 `sub_824F3580` 对 `r3=F8000068` 热自旋（`lr=828818DC`，计数到 6,868 万），`SWAP` 全程只发生 1 次。

5. **观感 = 黑屏有声。** 我们注入的影片帧没有任何消费者（呈现链从不推进），屏幕上始终是初始画面；而 XAudio 侧音轨照常推进，所以「有声音、无画面」。

一句话：**不是中断没送到，也不是设备解析错，而是「谁来回调 guest 每帧提交」这个驱动被 v8.6 新引入的、宿主自己伪造的 `16568` 计数器骗得每一轮都让位。**

## 4. v8.8-LIVENESS 改动（只改 `recomp/src/t2rotf_gpu_signal.cpp`）

### 4.1 让位判据改为「只认 guest 自己会自增的字段」

```text
旧（v8.3/v8.4）：cambio = (dev+16568 变了) || (base+284 变了)  -> 每次刷新都为真 -> 永远让位
新（v8.8）      ：只看 base+284（宿主从不写它，只有 guest 生产者会自增）
                  base+284 == 0  -> 驱动（当前没有任何待交付帧，正是该驱动的时刻）
                  base+284 != 0  -> 让位（已有帧在等 guest 自己的消费者）
                  base+284 连续卡住 >= 45 次刷新（kTicsEstancado = 45，60 Hz 下约 0.75 s）
                                 -> 仍驱动一次，去消化它（防止消费者没在跑时把整条链饿死）
```

- `dev+16568` **彻底移出判据**；`vivo_16568_` 变量仅留档、不再参与判断。
- 新增成员 `vivo_tics_estancado_`（连续未变化的刷新数）与 `dirigidos_estancados_`（卡住仍驱动的次数）。
- 位置：`DirigirFotograma()`（`t2rotf_gpu_signal.cpp:1041-1072`），常量 `kTicsEstancado = 45`（`:393`）。

### 4.2 开关 G14 默认值 `false → true`，并加上「无产出才补产」的条件

- `sa2_renut_display_drive_produce` 默认改为 **true**。
- 语义：走完 guest 每帧提交入口 `sub_82880978` 之后，**若 `base+284` 仍为 0**（这一轮提交什么都没产出），再补调一次 guest 原生生产者包装 `sub_82882730`（`kProducirGuest = 0x82882730`）：它在 guest 侧完成「入队 + 自增 `base+284` + 置位帧事件 `*(0x82DBB5E0) = F8000068`」，正是呈现线程等的那个信号。
- **`284` 已有值时绝不补调** ⇒ 不会重复投递。
- 位置：`t2rotf_gpu_signal.cpp:1115-1131`。

### 4.5 v8.8b —— 交付前自查发现的两处自身缺陷（已修）

这两处不改功能目标，但**不修就会让本版修复完全失效或引入新的 guest 越界**，故一并修掉后重建。

1. **补产的入参不能用 `r3 = 0`**。生产者本体 `sub_822B5608` 的重编译体首条真实指令是 `lwz r10,64(r3)`，紧接着 `stw r11,56(r3)` —— 以 `r3 = 0` 调用会去读写 guest `0x40` / `0x38`，落到未映射页。
   改法：先调 **guest 自己的解析器** `sub_828823F8`（`kResolverPresente = 0x828823F8`）——它在 `*(0x82DBB5C4)` 起、每条 296 字节的记录数组里找 `+0 == 5` 的那一条作为「当前呈现记录」——取到且通过 `PlausibleObjeto()` 才把它作 `r3` 调用 `sub_82882730`；取不到就**跳过并计 `prodSinDev=`**，绝不以 `r3 = 0` 调用生产者。
   逐条依据：`sub_828823F8`（`T2ROTF_recomp.43.cpp:92058`，len=47，找不到返回 0）、`sub_822B5608`（`T2ROTF_recomp.15.cpp:20226`，len=136）、`sub_82882730`（`T2ROTF_recomp.41.cpp:93571`，len=13，thunk：`r11 = 0x82E55CE0`、`r4 = r11+8`、尾调 `sub_822B5608(r3, r4)`，`r3` 原样透传）。
2. **当前显示对象 `*(0x82DBB608)` 不再作闸门**。v8.8 初版在驱动前用 `PlausibleObjeto(obj)` 做提前 `return`；但 v8.7 实机日志里该字段 **74/75 次采样恒为 0**，一旦保留提前返回，驱动会被永久拦下 —— 正是本轮要修的症状本身。
   依据：逐行读过每帧提交入口 `sub_82880978` 的重编译体（`T2ROTF_recomp.21.cpp:94058`，len=71）：它只调 `sub_82B22628` / `sub_8287EDC0` / `sub_82ABB7D0` / `sub_82880C30` 并复位 `0x82DBB6B4` / `0x82DBB6B8`，**从不读 `0x82DBB608`** ⇒ `obj == 0` 时驱动它是安全的；下游对空显示对象的解引用自 v7.5 起由 `sa2_renut_display_guard` 拦下。
   改法：该护栏改为**只计数**（`noObj=`），不再 `return`。

### 4.3 诊断增强（为下一轮日志判读准备好字段）

- `[T2DIAG DRIVE drive=… ok=… ini=… dentro=… noObj=… fallos=… vivo=… produce=… **prodSinDev=…** **estanc=…** 16548=… 16568=… 284=… 288=… **obj=…** **ev=…** mmio_w=…]`
- 每次驱动的跟进行 `[T2DIAG DRIVE n=… obj=… 284 {}->{} 288=… 16568=… 16548=… **saltos_vivo=…** **estancados=…**]`
- 新增字段用途：`estanc` > 0 说明「待交付帧卡住、宿主仍在驱动」这条兜底路径真的被用到；`prodSinDev` > 0 说明「补产时解析不到合法呈现记录、按 v8.8b 规则跳过了」（此时**不要**把它当成修复失效，见 §10 分支 D）；`obj` = `*(0x82DBB608)`、`ev` = `*(0x82DBB5E0)` 让「当前显示对象是否存在」「帧事件是否已被置位」**第一次直接可读**（v8.7 全都是 `drive=0`，这两项从未被观察到）。

### 4.4 版本标记

`[video] T2ROTF BUILD v8.8-LIVENESS (driver liveness fix: …)` —— 完整写明根因（`16568` 被 v8.6 记账推动 ⇒ 旧判活每轮让位 ⇒ guest 侧零产出 ⇒ 呈现线程在 `F8000068` 上空转 ⇒ 黑屏有声）、本次判据变更与补产语义，并声明 v8.7-IRQABI / v8.6-FLIP / v8.4-SPLIT / v8.3-FRAME / v8.2 影子安全访存与让位语义全部保留；BUILD 文本后半段另写明 v8.8b 两处修正（当前显示对象只计数、生产者入参经 `sub_828823F8` 解析）。
> 版本判据字符串**仍是 `v8.8-LIVENESS`**（不新增标签）；本版与「中间构建」的区别体现在 NRO 的 sha256（`d1a59c33…` 为本版，`0f485b3d…` 为已被取代的中间构建）。
位置：`t2rotf_gpu_signal.cpp:428-443`。

## 5. 为什么这样改是安全的（硬约束逐项盘点）

| 约束（历史轮次定下，不得回退） | 本轮是否触碰 |
|---|---|
| 宿主**绝不写**任何显示会计字段（`284`/`288`/`16548`/`16552`/`16564`/`16568`） | 未触碰。判活只读 `base+284`；补产走的是 guest 自己的函数，字段由 guest 代码自增 |
| 宿主侧 guest 访存必须走 `RexGmShadowFor` 影子别名（v8.2-SHADOW） | 未触碰。`Leer32` 仍是原影子安全读路径 |
| 每帧提交驱动必须跑在独立宿主线程 `T2ROTF Dirigir`（v8.4-SPLIT） | 未触碰。`EnHiloDirigido()` 守卫与 `BucleDirigido` 结构原样保留 |
| 中断回调设备只取 `r4` 且经 `EsDireccionDispositivo()` 校验（v8.7-IRQABI） | 未触碰 |
| 呈现记账交回 guest 自己（v8.6-FLIP） | 未触碰；本轮只是**不再把它的副作用当成驱动让位依据** |
| 不重复投递 | 补产调用以 `base+284 == 0` 为前置条件，已有待交付帧时绝不补调 |
| 不碰门/泵热路 | 未触碰 |

**新增的安全护栏与 v8.8b 的修正**：

- 驱动前读 `*(0x82DBB608)`（当前显示对象）做合理性检查 —— v8.8b 起**只记 `noObj` 计数、不再跳过驱动**（依据见 §4.5 第 2 条：`obj` 在卡死态恒 0，当闸门会把修复本身拦死）。
- 补产入参一律经 guest 自己的解析器 `sub_828823F8` 取合法记录，**绝不以 `r3 = 0` 调生产者**（依据见 §4.5 第 1 条）。
- 两处修正都只影响「宿主什么时候调 guest 的哪个函数、传什么参数」，**不写任何 guest 字段、不改安全访存路径、不碰热路**。

## 6. 构建校验

- `recomp/out/sw8` 执行 `ninja -j4 T2ROTF`：**退出码 0**（`set -o pipefail` 下实测），本轮只重编 `src/t2rotf_gpu_signal.cpp.obj` 并重新链接/打包，`[1/52] Generating recompiled code` 报 `Codegen summary: 0 written, 0 unchanged, 0 deleted, 1 module(s) up to date`（重编译代码未变，属预期：本轮只改宿主侧 C++）。
- NRO：**59,949,810 B**，sha256 **`d1a59c332b0460dcf09bf0d2bdf44c3577dd04ffab9dcef2828a4f821e9d8b5e`**（与上一版字节数相同、哈希不同；与交付前中间构建 `0f485b3d…` 也不同 —— 本版含 §4.5 两处修正）。
- NRO 字节级取证（Python 按字节 `count`，可复现）：`v8.8-LIVENESS` = **2**（BUILD 行 + G14 的 cvar 帮助文本）、`v8.8b refinements` = **1**、`prodSinDev` = **1**、`estancado` = **1**、`sub_828823F8` = **1**。NRO 里仍能搜到 `v8.7-IRQABI`/`v8.6-FLIP`/`v8.5-COMPLETE` 等字样 —— 那是新 BUILD 文本中的历史说明与旧 cvar 帮助文本，属预期，**不要用它判版本**。
- `T2ROTF.toml` 经 `tomllib` 解析通过、**24 键**读回正确（`sa2_renut_display_drive_produce = true`、`sa2_renut_display_present_guest = true`、`sa2_renut_gpu_signal_hostcb_s1 = true`、`sa2_renut_gpu_signal_hostcb_s1_mode = 2`、`sa2_renut_display_drive = true`、`sa2_renut_display_drive_thread = true`、`sa2_renut_gpu_signal_dev_hook = true`）。
- 源码改动范围：**只有 `recomp/src/t2rotf_gpu_signal.cpp`**（75,030 → 79,963 B）；`recomp/src/t2rotf_video_nativo.cpp`（80,874 B）、`recomp/src/t2rotf_gpu_signal.h`（1,593 B）、`recomp/src/renut_engine/gpu/d3d/hooks/device.cpp`（103,645 B）经逐字节核对**未改动**。

## 7. 工程包更新

`T2ROTF_switch_port.zip` 已**原地更新**（替换 `T2ROTF.nro` / `T2ROTF.toml` / `README.md` / `recomp_src/t2rotf_gpu_signal.cpp`，并追加本报告）。原地更新脚本 `recomp/out/sw8/_upd_pkg_t65.py` 会：

1. 断言磁盘 NRO 的 sha256 == `d1a59c332b0460dcf09bf0d2bdf44c3577dd04ffab9dcef2828a4f821e9d8b5e`（防止把旧 NRO 打包进去）；
2. 先做 `zip_backup` 快照（`T2ROTF_switch_port_20261101_v87_pre.zip` 已存在则跳过；同时新增 `T2ROTF_switch_port_20261101_v88_pre.zip` 作为本版回退点）；
3. 逐条目替换/追加后，用 `zipfile.testzip()` 自检，并把包内每一条目与磁盘逐字节比对；
4. 断言包内 NRO 哈希 == 随包 NRO 哈希；
5. 用 `tomllib` 解析随包 toml 并断言 **24 键**及上述关键值；
6. 打印包体字节数、条目数与 sha256。

## 8. 实机验证步骤

1. 把新的 `T2ROTF.nro` + `T2ROTF.toml` 覆盖到 `sdmc:/switch/Transformers2ROTF/`，**完全退出再启动**。
   验收场景是**不超频手持**（60 帧结论只在**不超频**条件下给；`rex_stderr` 里应能看到 `[apm] … GPU 460.8 MHz … memoria 1600.0 MHz`）。
2. **先确认版本**：日志里出现 `[video] T2ROTF BUILD v8.8-LIVENESS (driver liveness fix: …)`。
3. **是否仍崩**：`rex_crash.log` 应**不再生成**；`LaunchModule done` 之后应能继续走到影片/呈现阶段。
4. **本轮最关键验收点**（对照 v8.7 的 `drive=0 / ok=0 / produce=0 / vivo=8733 / 284=0 / SWAP n=1`）：
   - `[T2DIAG DRIVE drive=… ok=… produce=…]` 的 `drive` / `ok` / `produce` **是否 > 0**（这是本轮要改的病）；
   - `estanc` 是否开始计数（> 0 说明「待交付帧卡住仍驱动」这条兜底路径被用上）；
   - `284` 是否由 **0 变成非 0**（guest 侧第一次真正产出帧）、`[T2DIAG SWAP n=…]` 的 `n` 是否增长（> 1）；
   - `[T2DIAG GPSIG …]` 的 `mmio_w` / `inKick13232` / `ringR16700` 是否开始增长；
   - 呈现线程的 `[T2DIAG espera sub_824F3580 n=…]` 是否**停止单调增长**（停止 = 帧事件 `F8000068` 已被置位、呈现链开始推进）；
   - 本轮新增的两个可读字段：`obj=`（`*(0x82DBB608)`）是否非 0、`ev=`（`*(0x82DBB5E0)`）是否变成 `F8000068`；
   - 最后：**画面是否出现**（三连 logo `luxo → atvi → hasbro`）与 `juego … fps` 是否 **> 0**。

## 9. 需回传的日志行

- `[video] T2ROTF BUILD v8.8-LIVENESS …`（首条即可，用于确认版本）
- `[T2DIAG DRIVE …]`（**首末各一条**，重点 `drive` / `ok` / `produce` / `prodSinDev` / `estanc` / `vivo` / `dentro` / `284` / `288` / `obj` / `ev`）
- `[T2DIAG DRIVE n=… 284 {}->{} …]`（前三条 + 末一条，看 `284` 是否 0→非 0）
- `[T2DIAG GPSIG …]`（首末各一条，重点 `mmio_w` / `r1951` / `s1` / `inv` / `ret`）
- `[T2DIAG SWAP …]`（末一条，看 `n` 是否增长）
- `[T2DIAG espera sub_824F3580 …]`（末一条，看 `n` 是否停止增长）
- `[T2DIAG PERRO dev=40070600 …]`（首末各一条，重点 `284` / `288` / `ringR16700` / `ringW16704` / `inKick13232`）
- `rex_perfil.log` 的 `juego … fps` 与 `relojes:` 行（须为未超频值）
- `rex_crash.log`（若产生，附 `pc` / `far` / `esr` 前 40 行）

## 10. 判读分支

- **A（修复生效）**：`drive > 0` 且 `ok > 0`、`284` 出现非 0、`SWAP n > 1`、`espera` 停止增长、画面出现、`fps > 0` ⇒ 进入「不超频 60 帧」实测阶段（T26）。
- **B（驱动起来了但仍无画面）**：`drive > 0` 但 `284` 仍恒 0、`produce` 也在涨 ⇒ 说明连 guest 原生生产者 `sub_82882730` 调用后都没能入队（下一轮冲 `sub_822B5608` 与 `sub_82881E00` 的入队前置条件，以及 `v8.6` 那两个「逃逸标志」`0x82DBB5E4`/`0x82DBB5D4` 的写入者）。
- **C（`284` 变非 0 但呈现仍不动）**：`drive/ok/produce > 0`、`284 != 0`、`SWAP n` 仍 1 ⇒ 消费者（`sub_82881848` → 交换路径）没被唤醒，下一轮冲 `base+284` 的**释放者**（`sub_822D9838`/`sub_822D98E8` 与 `VdSetSystemCommandBufferGpuIdentifierAddress` 注册地址）。
- **D（驱动起来了、补产也被跳过）**：`drive > 0` 但 `produce = 0` 且 `prodSinDev` 持续增长 ⇒ `sub_828823F8` 在当前状态下**解析不出合法呈现记录**（恒返回 0）。这说明「当前呈现记录」本身还不存在，**根因上移**到「谁建立 `*(0x82DBB5C4)` 那张 296 字节记录表里 `+0 == 5` 的那一条」。注意：此时 `noObj` / `obj=` 单独为 0 **不构成**问题判据（v8.8b 已不把 `*(0x82DBB608)` 当闸门）。
- **E（仍有 `rex_crash.log`）**：回传新 `pc` / `far` / `esr` 前 40 行。

## 11. 开关语义与回退阶梯

本轮**不新增按键**（仍为 **24 键**），只改一个默认值：

- `sa2_renut_display_drive_produce`：**默认由 `false` 改为 `true`**。`false` 时只驱动 guest 提交入口、不补产（A/B 用）。
- 回退本版判活修复没有专用开关（改的是判据本身），回上一版请用 `recomp/out/sw8/zip_backup/` 下的快照包（`T2ROTF_switch_port_20261101_v87_pre.zip` / `T2ROTF_switch_port_20261101_v88_pre.zip`）。
- 灰度/对照用的阶梯（每退一步都是明确的历史行为）：
  `sa2_renut_display_drive_produce = false`（退到「只驱动提交入口」）→
  `sa2_renut_display_drive = false`（退 v8.2：中断链通、每帧提交侧不产出）→
  `sa2_renut_gpu_signal_dev_hook = false`（退 v8.1 设备解析，影子访存保留）→
  `sa2_renut_gpu_signal_hostcb = false`（退 v8.0）→
  `sa2_renut_gpu_signal = false`（退 v7.7，0 fps 黑屏）。
- 所有键都是 `kInitOnly`：改了必须**完全退出游戏再启动**。

## 12. 残余风险与下一步

1. **本版尚未实机验证。** 根因链（`16568` 被 v8.6 记账推动 ⇒ 旧判活每轮让位 ⇒ guest 侧零产出）由真机日志直接支撑，但「改判据 + 补产」能否真的让 guest 侧产出第一帧，要由第 10 节的判读分支确认。
1b. **补产路径有一处未知**：`sub_828823F8` 在卡死态能否解析出合法记录（`>= 0x40000000`）本轮无法离线判定；若恒返回 0，则会全部走 `prodSinDev` 跳过 —— 此时**仍然生效的是让位判据修正**（与 v8.3 实测的 `284 0→1` 同源），可按 §10 分支 D 继续下钻，不必回退本版。
2. **补产调用是否会被高频触发**：因为触发条件是「`284` 仍为 0」，正常工作时每产出即让位；若出现 `drive`/`produce` 在 60 Hz 满速增长而 `284` 始终 0，说明生产者调用没能真正入队（对应判读分支 B），届时再看是不是入队前置条件（例如 `sub_82881B88` 门的槽位）没满足。
3. **`base+284` 的释放者仍未定位**（历史遗留未决项）：候选在 guest GPU 提交完成路径 `sub_822D9838`/`sub_822D98E8` 与 `VdSetSystemCommandBufferGpuIdentifierAddress` 注册的地址。若本轮 `284` 能涨但不能回落，下一轮直接冲这里。
4. **「不超频流畅 60 帧」仍未达成也未实测**（T26 待做）：只有画面出现后才能在**不超频手持**条件下实测帧率；若 460.8 MHz 下不达标，候选（均不超频）为内部分辨率降至 1024x576、`resolution_scale` 相关 cvar、LTO/PGO 构建。
5. **TU / DLC 实机闭环**仍在画面出现之后才能验证（产物层 `update/`、`patched/default.xex`（15,937,536 B）、`dlc/` 已就绪）。