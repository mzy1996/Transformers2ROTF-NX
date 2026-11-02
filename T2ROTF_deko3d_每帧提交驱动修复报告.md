# T2ROTF（变形金刚2：卷土重来 / Xbox 360）Switch 移植
## 每帧提交驱动修复报告（T53 / v8.3-FRAME）

- 工程根目录：`J:\XBOX360 port\Transformers Revenge of the Fallen`
- 构建目录：`recomp/out/sw8`（`ninja -j4 T2ROTF`）
- 本轮产物：`recomp/out/sw8/T2ROTF.nro`（59,929,330 字节，sha256 `9059ed99ab8ab85c…`）
- 交付形态：工程包 `T2ROTF_switch_port.zip` + 可单独取用的 `T2ROTF.nro` 与 `T2ROTF.toml`
- 本轮性质：**静态修复 + 静态取证已完成；实机复测数据缺失，等待用户回传日志**

---

## 0. 一句话结论

v8.2-SHADOW 让**中断侧已经完全通了**（宿主每 2 秒把 `vblanks/irq/r1951/mmio_r/hostcb` 全部推起来，
`ret` 追得上 `inv`，旧排空自旋从 35,439,751 降到 40），可画面依旧不出，是因为
**guest 自己「每帧把工作交给显示管理器」的那次调用从未发生** —— `[T2DIAG SWAP n=1]` 全程只有 1 次、
`disp16568` 冻在 5、`mmio_w=0`、`cola284=0`、`cola288=1`，每帧提交侧一次都没产出，
显示线程停在 `sub_82881848` 的 `NtWaitForSingleObjectEx(0xF8000064, alertable=1)` 上热自旋。

v8.3-FRAME 在**垂直同步宿主线程的每个真实刷新**里，替 guest 走一次它**自己的**每帧显示提交入口
`sub_82880978`（无参、纯 guest 全局状态驱动；经 `FunctionDispatcher` 调用，调用后完整还原宿主线程的
guest 上下文，只保留它对 guest 内存的副作用），把「谁来回调每帧提交」这一点补上；并带只读判活：
一旦 `dev+16568` 或 `base+284` 相对上次发生变化（画面管线自己动起来），立即**让位观察 3 次刷新**
（不叠加提交，绝不把呈现做成双份）。**不写任何显示会计、不碰门/泵热路、不改 v8.2 已验证的
shadow 安全访存、`presenter()/provider()` 保持 null（画面仍由 Renut/deko3d 出）。**

---

## 1. 本轮起点：v8.2 实机「还是黑屏，第一个动画能播放但是有声音无画面」

### 1.1 日志组与关键原文

用户描述原文：「**还是黑屏，第一个动画能播放但是有声音无画面**」。回传日志组：

| 文件 | 关键内容 |
| --- | --- |
| `rex_perfil(32).log`（33,828 字节） | 性能剖析：`juego` fps、`relojes:`（未超频值）、线程 CPU 占用 |
| `rex_stderr(69).log`（1,572 字节） | **无 Data Abort、无 `Unhandled guest access violation`**、无崩溃 |
| `T2ROTF_001(67).log`（240,548 字节） | 主日志：版本判据、`[T2DIAG SWAP/GPSIG/GPSIG3/PUERTA/espera/YIELD]` 全族、影片打开行 |
| `T2ROTF_002(48).log`（213,031 字节） | 同一次启动、规模相近（同组佐证） |

### 1.2 判据逐条核对（v8.2-SHADOW 在跑，且比上一轮前进了一大步）

| 项 | 实机原文 | 判读 |
| --- | --- | --- |
| 版本判据 | `[video] T2ROTF BUILD v8.2-SHADOW (guest memory read through the chunk shadow alias; device resolution layered cvar>slot pair>D3D hook)` | 跑的确实是 v8.2 |
| 是否仍崩 | `rex_stderr` 无 Data Abort / 无 `Unhandled guest access violation`；无 `rex_crash.log` | **v8.1 的启动即 fatal 已被 v8.2 修掉** |
| 影子通路 | `[T2DIAG GPSIG3] hookDev=00000000 → 40070600；sinSombraL=2 sinSombraW=0 sinSombraAvisada=1` | **shadow 通路正常**：取不到影子的次数恒为 2（一次读一次写地址不可用），无直读直写，无缺页 |
| 代派发链四要素 | `[T2DIAG GPSIG] vblanks 0→600、irq 0→578、r1951 0→578、mmio_r 0→578、hostcb …→（/22）、inv=ret=irq` | **宿主已按 60 Hz 把显示 ISR 送进 guest**：中断侧首次真正运行 |
| 旧自旋 | `[T2DIAG YIELD] total=30 → 40` 后冻结，`top=[82881DE8x30] [82881918x10]` | 旧「排空自旋」断崖式消失（曾为 35,439,751 量级） |
| 影片打开 | `[info] el juego abre/lee 'game:\movies\luxo_logo.wmv'` | 与用户「第一个动画能播放」吻合 |

### 1.3 新卡点（画面为何仍不出）

| 字段 | 实机原文 | 含义 |
| --- | --- | --- |
| `[T2DIAG SWAP]` | `n=1 dev=0x40070600 front=0x82e55fc4 sub16560=1 disp16568=0 fr16564=0 f10941=0x00` | **全程仅 1 次 Swap**（开局一次），此后每帧提交再没发生 |
| 显示会计 | `disp16568=0`（其余诊断行冻在 5）、`a16700=a16704=0`、`mmio_w=0` | 显示管线**冻死**，写侧一次都没动 |
| 通道计数 | `[T2DIAG PUERTA] n=2 r30=82E55E20 arg=00000000 … cola284=0 cola288=1 obj256=0/0/0 obj272=40F58F80` | `cola284=0`（无待交付）、`cola288=1`（一项在飞孤儿未配平） |
| 等待方 | `[T2DIAG espera] sub_824F3580 n=…, r3=F8000064 r4=1 lr=828818DC`（`espera` 20.78M） | 显示线程停在 `sub_82881848` 的 `NtWaitForSingleObjectEx(0xF8000064, alertable=1)` **立即返回热自旋** |
| GM 线程 | `[T2PERFIL]` Game Master CPU 0.0%，阻塞在 `KeWaitForSingleObject` | 每帧驱动的线程真阻塞（不是空转），等的东西一直不来 |

⇒ **因果链**：宿主把显示 ISR 送进 guest 了（中断侧通），但 guest 侧「**每帧把工作交给显示管理器**」
的那一步（每帧提交入口 `sub_82880978`）从未被调用 ⇒ 没有新工作在飞 ⇒ 门（`*base+284`）永不就绪 ⇒
泵无事可做 ⇒ 呈现侧空转 ⇒ `SWAP n=1`、`disp16568` 冻结、`mmio_w=0` ⇒ **0 fps 黑屏（有声无画）**。

---

## 2. 根因

### 2.1 每帧提交入口 `sub_82880978` 的原生语义

`sub_82880978` 是 guest 自己的**每帧显示提交**入口：无参、只按 guest 全局状态工作，
内部把本帧要显示的东西挂进显示管理器的**待交付队列**（生产者侧：`entry+24`、`base+284` 自增），
供门（`sub_82881B88`）→ 泵（`sub_82881E00`）→ 交付（`sub_82881930`）→ 排空（`sub_82882200`）这条链消费。
在原生机器上，它由**显示刷新节奏**驱动、每帧至少被叫一次。

### 2.2 本移植里为什么它一次都没被叫

- Renut 接管 D3D 后，`config.graphics` 被清空、原生按刷新率驱动 guest 图形路径的那条宿主线程整体旁路
  （T44 已取证，v8.0 补回了 ISR 派发、v8.1 补回了「谁注册回调」、v8.2 补回了影子安全访存）。
- 但**「每帧把工作交给显示管理器」这一步的调用者**在原生机器上挂在哪条链上、在本移植里被旁路掉了，
  一直没有补 —— 于是：ISR 进来了，可每帧提交侧不产出，门永远不就绪。
- 这是纯粹的**调用缺失**，不是数据错、不是缺页、不是崩溃：所以三组日志里缺页/崩溃全为 0，
  而 `mmio_w=0`、`SWAP n=1`、`disp16568` 冻结同时出现。

### 2.3 结论

必须由宿主在**真实刷新**里补上这次调用，且用 guest **自己的**入口（`sub_82880978`）而不是宿主另造一套，
这样挂进去的工作项与 guest 自己的门/泵/交付链语义完全一致（避免早期「假装 guest 有进展」类做法）。

---

## 3. 改动清单（v8.3-FRAME）

> 本版**只改一个文件**：`recomp/src/t2rotf_gpu_signal.cpp`（46,883 → 56,659 字节）。
> `recomp/src/t2rotf_gpu_signal.h`（1,593 字节）与 `recomp/src/t2rotf_video_nativo.cpp`（76,899 字节）
> **均未改动**；`renut_engine/**`、SDK 侧（`repos/nfsmw-nx/sdk/**`）**未动**。

### 3.1 `recomp/src/t2rotf_gpu_signal.cpp`

| 位置 | 改动 |
| --- | --- |
| 文件头文档 | 追加 `── v8.3-FRAME（T53）` 段：把 T52 的判读（缺页/崩溃全 0、代派发链四要素全增长、但每帧提交侧从未产出）与「显示 ISR 进来了、但每帧把工作交给显示管家那次调用没有发生」的结论直接写进代码注释 |
| 新增 cvar（G11…G14） | `sa2_renut_display_drive`（bool，**默认 true**）、`sa2_renut_display_drive_after_ms`（int32，**2500**）、`sa2_renut_display_drive_div`（int32，**1**）、`sa2_renut_display_drive_produce`（bool，**false**）。注释写明：每个真实刷新经 dispatcher 走 guest 自己的每帧提交入口 `sub_82880978`（无参、纯全局状态驱动），不写会计、不碰门/泵热路、影子安全读原样保留；**只要 guest 管线自己动起来（`dev+16568` 或 `base+284`）就让位 3 次刷新，present 绝不双份**；关掉 = 完全退 v8.2 行为（仅 A/B 用） |
| 每帧驱动实现 `DirigirEntrada(entrada, cpu)` | 取当前线程/`thread_state`，`SetActiveCpu`，经 dispatcher 走 guest 入口 `sub_82880978`；调用后**完整还原宿主线程的 guest 上下文**（寄存器/PC/SP 等），只保留它对 guest 内存的副作用 ⇒ 宿主线程不被 guest 状态污染 |
| 驱动闸门 | 启动后满 `after_ms`（默认 2.5 s）才驱动，避开 boot/init；`div` 控制「每 N 个刷新驱动一次」（默认 1 = 每刷新）；每刷新先只读检查 `dev+16568` 与 `base+284` 是否相对上次变化，**变化即让位 3 次刷新**，随后静止再恢复 |
| 显示对象守卫 | 当前显示对象 `*(0x82DBB608)`（`kDispActual`）不可用时**不驱动**并计数（`DRIVE` 行的 `noObj=`）；可选 `sa2_renut_display_drive_produce`（默认关）额外调一次 guest 原生生产者包装 `sub_82882730` 向显示管理器自身队列投一条（默认关，因为 `sub_82880978` 内部已做这次 hand-off） |
| 新增只读诊断 | ① 每 2 s 一行 `[video] T2DIAG DRIVE drive=… ok=… noObj=… fallos=… vivo=… produce=… 16548=… 16568=… 284=… 288=… mmio_w=…`；② 首次驱动后头 4 次 + 每 60 次各一行 `[video] T2DIAG DRIVE n=… obj=… 284 a→b 288=… 16568=… 16548=…`（用于看驱动是否真的推动管线） |
| 版本打印 | `[video] T2ROTF BUILD v8.3-FRAME (host drives the guest's own per-frame display commit sub_82880978 on every refresh; shadow-safe guest reads kept from v8.2)` |
| 保留不变 | v8.2 的 `Leer32`/`Escribir32` 影子安全访存与 `GuardarCampo`（v8.1 cb_guard 语义）、`Dispositivo()` 分层解析、`InformeSombra()`/`GPSIG3`、MMIO 读（`0x1951` 恒 1）**全部原样保留** |

**没有改动**：呈现路径、门/泵/交付热路上的一切字段、`dev+16548` 族、`renut_engine/**`、SDK 侧。
本版不写任何显示会计、不从 guest 热路调用宿主。

---

## 4. 构建与静态取证（本轮已完成）

### 4.1 构建

```
cd recomp/out/sw8 && ninja -j4 T2ROTF
退出码 = 0   （22:29:38 → 22:30:06，26.4 s）
```

再跑一次仍为 no-op：`Codegen summary: 0 written, 0 unchanged, 0 deleted, 1 module(s) up to date`，
NRO 的 mtime 与 sha256 均不变 ⇒ **构建已完整收尾且可复现**（此前 `ninja -n` 报 51 项 pending 属
codegen restat 上游的正常表现，非「脏构建」，已用对象时间戳 + `.ninja_log`/`.ninja_deps` + 实跑证伪）。

### 4.2 产物

| 项 | 值 |
| --- | --- |
| `recomp/out/sw8/T2ROTF.nro` | **59,929,330 字节**，sha256 `9059ed99ab8ab85ce31840dff01ebc1c94e750372ffb31247c7ba3535debe99a`（2026-10-30 21:47:54） |
| 上一版（v8.2/T50） | 59,921,138 字节，sha256 `22980392fac38f4b…`（**字节数与哈希都不同**） |
| `recomp/out/sw8/T2ROTF.stripped.elf` | 59,832,776 字节 |
| 版本标记字符串 | `v8.3-FRAME`（NRO 内 5 处命中） |

### 4.3 字符串取证（NRO 内命中次数）

| 字符串 | 命中 | 含义 |
| --- | --- | --- |
| `v8.3-FRAME` | 5 | 版本判据行 + 帮助/诊断文本 |
| `sa2_renut_display_drive` | 16 | 新开关 G11 已注册（cvar 定义名、帮助文本、取值路径、登记副本） |
| `sa2_renut_display_drive_after_ms` | 4 | G12 已注册 |
| `sa2_renut_display_drive_div` | 4 | G13 已注册 |
| `sa2_renut_display_drive_produce` | 4 | G14 已注册 |
| `T2DIAG DRIVE` | 2 | 两行新诊断已编入（周期行 + 逐次行） |

> 取数方法：`grep -a -o -F "<串>" recomp/out/sw8/T2ROTF.nro | wc -l`
> （GNU `strings` 只取可打印 ASCII 串，中文串与紧邻非 ASCII 的串必须用 `grep -a`，否则会误报 0）。

### 4.4 哈希纪律（沿用）

- 产物内嵌构建时间戳 ⇒ **同源码重构建体积相同、sha256 会变**；文档与包内出现的 NRO 哈希必须是
  随包 NRO 的实算值。
- 工程包自身与报告自身**不写死自指哈希**（由打包脚本实算打印），避免自指污染。

---

## 5. 开关语义与回退阶梯

### 5.1 本版新增的键

| 键 | 默认 | 语义 |
| --- | --- | --- |
| `sa2_renut_display_drive`（G11） | `true` | 宿主在垂直同步线程的每个真实刷新里，经 dispatcher 调 guest 自己的每帧显示提交入口 `sub_82880978`（无参、纯全局状态驱动）。管线自活时让位 3 次刷新，绝不叠加提交。**`false` = 完全退回 v8.2 行为（中断链通、每帧提交侧不产出），仅供 A/B** |
| `sa2_renut_display_drive_after_ms`（G12） | `2500` | 开始驱动前的等待毫秒数（避开 boot/init）。`0` = 从第一次刷新就驱动 |
| `sa2_renut_display_drive_div`（G13） | `1` | 每 N 个刷新驱动一次（A/B 二分用）。`1` = 60 Hz；`2` = 30 Hz |
| `sa2_renut_display_drive_produce`（G14） | `false` | 每驱动一次额外调一次 guest 原生生产者包装 `sub_82882730`。默认关（`sub_82880978` 内部已做 hand-off，再加一条可能重复投递）；只在 A/B「只调提交入口是否足够」时打开 |

其余 17 键（v8.2 及更早）默认值与语义**不变**，详见 `T2ROTF.toml` 注释。共 **21 键**，全部 `kInitOnly`。

### 5.2 回退阶梯（改键后必须重启）

| 想回到 | 操作 |
| --- | --- |
| v8.2-SHADOW | `sa2_renut_display_drive = false`（中断链通、每帧提交侧不产出） |
| v8.1-GUESTCB | 再置 `sa2_renut_gpu_signal_dev_hook = false`（退设备解析；影子访存修复保留） |
| v8.0-GPUIRQ | 再置 `sa2_renut_gpu_signal_hostcb = false` |
| v7.7-ALIGN | 再置 `sa2_renut_gpu_signal = false`（完全不安装显示信号源） |

---

## 6. 实机验证步骤与需回传的日志行

### 6.1 部署

1. 把 `T2ROTF.nro` 与 `T2ROTF.toml`（**必须同名**）一起放到 `sdmc:/switch/Transformers2ROTF/`；
2. `game/`、`update/`、`patched/`、`dlc/` 布局不变（TU 在 `update/`，DLC 在 `dlc/`）；
3. 启动后跑 60–120 秒，然后把 `rex_perfil.log`、`rex_stderr.log`、`T2ROTF_001.log`、
   `T2ROTF_002.log` **以及（若有）`rex_crash.log`** 一并回传。

### 6.2 必须核对的行

| 目的 | 日志行 | 期望 |
| --- | --- | --- |
| 版本判据 | `[video] T2ROTF BUILD v8.3-FRAME …` | 必须出现；**应用 banner 仍是旧 `v6.27-MAPFIX`，不可当判据** |
| 是否仍崩 | `rex_crash.log` / `Unhandled guest access violation` | **应消失**（v8.2 已修，本版不应引入新崩） |
| 驱动是否生效 | `[T2DIAG DRIVE …]` 的 `ok=` / `noObj=` / `drive=` | **`ok=` 必须增长、`noObj=` 保持 0**；`noObj=` 持续增长 = 当前显示对象始终为空（走 6.3 分支 B） |
| 驱动是否推动管线 | `[T2DIAG DRIVE n=… 284 a→b 288=… 16568=… 16548=…]` | `284` 应出现 `a→b` 变化、`16568`/`16548` 应开始动 |
| 呈现是否发生 | `[T2DIAG SWAP …]` 的 `n=` | 应从 1 开始**增长** |
| 写侧是否活 | `[T2DIAG GPSIG …]` 的 `mmio_w=` | 应开始非 0 |
| 交付链 | `[T2DIAG PUERTA …]` 的 `cola284/cola288` | `cola284` 应不再恒 0、`cola288` 悬挂应收敛 |
| 帧率/时钟 | `rex_perfil` 的 fps 行与 `relojes:` 行 | **`relojes` 必须是未超频值**；fps 应 **> 0**、画面应出现三连 logo |

### 6.3 判读分支（一次启动即可定论）

| 分支 | 观察 | 结论与下一步 |
| --- | --- | --- |
| A | `DRIVE ok=` 增长、`noObj=0`，且 `SWAP n=`/`disp16568`/`mmio_w` 开始变 | **驱动生效、每帧提交侧首次真正运行** ⇒ 看 fps 与画面 |
| B | `DRIVE ok=0`、`noObj=` 持续增长 | 当前显示对象 `*(0x82DBB608)` 始终为空，驱动条件不满足 ⇒ `sub_82880978` 不作驱动点，下一轮改判驱动位置（见第 7 节残余风险） |
| C | `ok=` 增长但 `SWAP n=`/`disp16568` 仍不变 | `sub_82880978` 被调用了但没改变状态 ⇒ 需追该入口内部的前置判据（下一轮） |
| D | 仍有 `rex_crash.log` | 回传新崩溃的 `pc/far/esr`（前 40 行） |

---

## 7. 未验证项与残余风险

### 7.1 本轮无实机复测数据

本报告的判读与修复全部基于**静态取证**（源码、SDK 语义、上一轮 v8.2 实机日志）。**本版尚未实机验证**，
请以 6.2/6.3 的回传日志为准，不要以本轮静态结论代替实机结论。

### 7.2 关键残余风险：`*(0x82DBB608)`（`kDispActual`）运行时取值未知

v8.3 的每帧驱动**依赖当前显示对象 `*(0x82DBB608)` 非空**。但本轮全部 v8.2 日志与诊断行里
`B608|B60C|82DBB6` **零命中**，其运行时值**必须**靠下一轮 `[T2DIAG DRIVE …]` 判活确认：
- 若 `ok=` 增长、`noObj=0` ⇒ 驱动生效（分支 A）；
- 若 `noObj=` 持续增长 ⇒ 驱动条件不满足（分支 B），此时不能靠「换个地址瞎猜」，须先取证
  `*(0x82DBB608)` 的写入者与生命周期，再改驱动位置（下一轮）。
- 相关未定语义（本轮未动）：`blk10900=BDF80080` 非合法对象、`f10941=0x00`、`cont=00000000 obj=00000000`。
  若下轮 `drive` 已涨但画面仍黑，转向这几项排查。

### 7.3 目标验收（仍未达成，需实机）

用户硬性要求：**不需要超频、流畅 60 帧**。参考上限（仅作对照，不得用于达标）：
CPU 1785 MHz / GPU 921 MHz / 内存 1600 MHz。本版尚未做不超频实测帧率（T26 仍待办），
须等 v8.3-FRAME 实机日志判读通过、画面出现后再排。

---

## 8. 交付物清单

| 文件 | 说明 |
| --- | --- |
| `T2ROTF.nro` | 59,929,330 字节，sha256 `9059ed99ab8ab85c…`；放 `sdmc:/switch/Transformers2ROTF/` |
| `T2ROTF.toml` | 与 NRO 同名同目录；本版新增 4 键（共 21 键）、头部版本块更新为 v8.3-FRAME |
| `T2ROTF_switch_port.zip` | 工程包（原地更新：新 NRO、toml、README、本报告、`recomp_src/t2rotf_gpu_signal.cpp`） |
| 本报告 | `T2ROTF_deko3d_每帧提交驱动修复报告.md` |

工程包字节数、包内逐条 sha256 与包自身哈希由打包脚本实算打印（见构建目录 `_upd_pkg_t54.py` 输出），
本报告不写自指哈希。

---

## 附录：本轮为什么这样选

| 方案 | 取舍 |
| --- | --- |
| 宿主自己造每帧提交（另起一套队列） | 会与 guest 自己的门/泵/交付链语义脱节（早期「假装 guest 有进展」类做法已证伪）；改用 guest **自己的** `sub_82880978`，挂进去的工作项语义完全一致 |
| 每刷新都驱动、永不让位 | 一旦 guest 管线自己动起来就会**重复提交**（呈现双份），故带「管线自活即让位 3 次刷新」 |
| 在 guest 热路里插驱动 | 严禁（会改写当前线程活动上下文、破坏热路）；驱动只在宿主垂直同步线程侧发生 |
| 写显示会计推进（早期做法） | 已证伪（会把门刷过 0 回绕、或走进空当前显示对象的完成回调链崩溃）；本版**不写任何会计** |
| 无脑加 `div`/`produce` 并全开 | `div>1` 只用于 A/B 二分；`produce` 默认关（可能重复投递），仅在「只调提交入口是否足够」时打开 |