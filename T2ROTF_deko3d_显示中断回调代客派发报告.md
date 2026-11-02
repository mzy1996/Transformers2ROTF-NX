# T2ROTF（Xbox 360《变形金刚2：卷土重来》）Switch 移植 — 显示中断回调「代客派发」修复报告

- 轮次：**T46（实机判读）+ T47（落地修复）**
- 版本标记：`[video] T2ROTF BUILD v8.1-GUESTCB (host calls the guest's own GPU interrupt callback)`
- 工程根：`J:\XBOX360 port\Transformers Revenge of the Fallen`
- 日期：2026-10-30

---

## 0. 本轮产物与哈希

| 产物 | 路径 | 字节数 | sha256 |
|---|---|---|---|
| 可执行 | `recomp/out/sw8/T2ROTF.nro`（包内根目录同名） | **59,912,946** | `97f432efd8d74f833e175aa9d6f66463233fc59a32998e1797353bcf7f1a4dbe` |
| 配置 | `T2ROTF.toml` | **22,366** | `3e17b8112df137e325eee056a35696798bd9ee22f8d94d5341ed879d3d6a189c` |
| 工程包 | `T2ROTF_switch_port.zip`（原地更新，**78 条目**） | 打包脚本实算 | 打包脚本实算（见 §6） |
| 本报告 | `T2ROTF_deko3d_显示中断回调代客派发报告.md` | 打包时实算 | 打包时实算（包内副本与磁盘副本逐字节一致，见 §6） |

包内新增报告 `T2ROTF_deko3d_显示中断回调代客派发报告.md`，替换 `T2ROTF.nro`、`T2ROTF.toml`、`README.md`、
`recomp_src/t2rotf_gpu_signal.{h,cpp}`、`recomp_src/t2rotf_video_nativo.cpp`、
`recomp_src/renut_engine/gpu/d3d/hooks/device.cpp`、`T2ROTF_app.h`、两个 `.bak.t37/.t41` 快照，其余条目按原字节保留。
本轮首包快照：`recomp/out/sw8/zip_backup/T2ROTF_switch_port_20261030_pre.zip` = **27,579,319 字节 /
`15e2db9b52e2a87b56601c1455770354706aa2912ebcf991b46a6d2d21d1bdb7`**（含本轮报告初版）。
v8.0/T44 的包体（77 条目 27,547,773 字节 / `608b4fab…`）在本次**原地更新时被覆盖**，其内容随 v8.0 交付包保留在用户侧；
更早的历史包备份在 `out/t2-tools/zip_backup/`，最近一个是 v7.7 的 `T2ROTF_switch_port_20261029_v77.zip`。

> 工程包与本报告的「字节数 / sha256」不写死数字：包体包含本报告与 README，任何写死的包哈希都与「含它自己的包」自相矛盾；
> 这些值由原地更新脚本 `recomp/out/sw8/_upd_pkg_t48.py` 在打包时实算并打印，逐项核对结果见 §6。**NRO 与 toml 的哈希是稳定值，照写。**

对照：上一版 **v8.0-GPUIRQ**（T44）NRO = 59,888,370 字节 / `77230192f21b3909247c3708c9a1aba921c0a16ee4c59965e6d9e4a0657c5753`，
`T2ROTF.toml` = 16,470 字节 / `d1fc49fe27569df44cca3c78d783b59f78bbca413762505a2f66c04a10f3e737`。

> 本工程产物内嵌构建时间戳：**同一份源码重构建后体积相同、哈希会变**。文档与包内哈希一律以随包 NRO 实算值为准。

---

## 1. 本轮实机判读（T46）

用户回传：`rex_perfil(31).log`（116,279 B / 1671 行）、`rex_stderr(67).log`（1,572 B / 25 行）、
`T2ROTF_001(65).log`（490,581 B / 3212 行，09:24:12.591–09:25:21.300）、`T2ROTF_002(46).log`（399,976 B / 2634 行，09:25:34.608–09:26:29.849）。用户描述：**依旧是只有黑屏，在第一个 logo 之后卡住。**

| 核对项 | 实测 | 结论 |
|---|---|---|
| 版本标记 | `T2ROTF BUILD v8.0-GPUIRQ` 各 1 次（另有 `v7.6-RECON`、`v7.7-ALIGN` 各 2 次，同属本 NRO，不是版本混装） | **v8.0 在跑** |
| 安装期三行 | `GPSIG: 信号源接管（presenter=null，画面由 Renut 出）`、`GPSIG: MMIO 窗口 0x7FC80000-0x7FC8FFFF 已注册`、`GPSIG: 垂直同步派发线程已启动（cvar hz=60，cpu=2）` | 齐全，模块已生效 |
| `[T2DIAG GPSIG]` | 001 共 33 条（`vblanks` 0→3841）、002 共 27 条（0→3121） | 派发线程**按 60 Hz 精确运行**（3841/64 s、3121/52 s） |
| 同一行的四个关键数 | 全程 `cb=00000000 ud=00000000`、`irq=0`、`r1951=0`、`mmio_r=0`、`mmio_w=0`、`wptr=0(0)`、`rptr_pub=0`、`sup1844=0` | **一个中断都没发出、一次寄存器访问都没发生** |
| `T2DIAG GPSIG wptr=` | 0 条（该行只在前 8 次 WPTR 写入时打） | guest 从未写 `CP_RB_WPTR` |
| guest 侧 ISR | `irq sub_822D47A0`=0、`irq sub_822D9570`=0；但 `init sub_822D4A08`=6、`externo sub_822D4488`=5 | 显示环**初始化与提交外层跑过**，显示中断链**一次都没进** |
| 设备侧冻结 | `PERRO dev=40070600 vblank16548=5 tick16552=33938253 sub16560=5 fr16564=5 disp16568=5 ringR16700=0 ringW16704=0` | `dev+16548` 族**全程冻结在 5**（v8.0 与 v7.7 完全一致） |
| 门/队列 | `PUERTA n=29 r30=82E55E20 arg=00000000 cnt=0/0/0 v288=0 cola0=82E55CE0 cola272=40F58F80 cola284=0 cola288=0 obj256=0/0/0` | 门三槽 arm 计数与后备标记全程 0 |
| 影片等待方 | `espera sub_824F3580 n=42500000 r3=F8000064 r4=1 r5=0 lr=828818DC` | 仍在事件 `F8000064` 上**死等**（4250 万次） |
| 泵/交付 | `PERRO2 dtick=7 ddrain=4/11 dpump=22/0/20 ddeliv=6 dcmd=0/0 dprod=6` | 泵进入 22 次、**成功 0**；交付 6 次、生产 6 次 |
| 自旋 | `YIELD total=37 top=[82881DE8x29] [82881918x8]` | v7.7 的对齐已把 3544 万次自旋压到 37（本版**没有回归**） |
| 帧率/CPU | `juego 0.0 fps | CPU total 99%（3 核）`；`rex_stderr` 25 行**无访问违例、无崩溃** | 有声音、0 帧、黑屏 |
| 时钟 | `relojes: CPU 1785.0 MHz, GPU 921.6 MHz, memoria 1600.0 MHz | 3 核（mask 0x7）`；`[apm] PUESTA la configuracion 0x92220008 … GPU 921.6 MHz` | 本次运行处于**高时钟**状态（GPU 由本工程 APM 请求抬到 921.6），**不是手持 1020 MHz 基线**；即便如此仍 0 fps ⇒ **卡点是管线停摆，不是性能不足** |

### 1.1 定论：v8.0 装好了电话线，但没人拨号

v8.0（T44）把被 Renut 一并删掉的两样东西补了回来——`0x7FC80000` 的 64 KB GPU 寄存器 MMIO 窗口、按刷新率派发 guest 图形中断的宿主线程——而且**线程实测按 60 Hz 精确运行**（`vblanks` 0→3841）。但派发线程需要知道「该调 guest 的哪个回调」，这个信息来自 guest 自己调用 `VdSetGraphicsInterruptCallback`；而实机日志里 `cb=00000000`、安装期的 `GPSIG: guest 注册图形中断回调 …` 一次都没打 ⇒ **guest 侧那个注册点从未执行**。

静态取证给出原因：注册点位于生成代码 `recomp/generated/default/T2ROTF_recomp.28.cpp:22182 DEFINE_REX_FUNC(sub_822E1690)`，
其 22262–22274 为 `lis r11,-32211` + `addi r3,r11,18336`（= **0x822D47A0**）、`mr r4,r31`（设备）、`bl 0x82b4e854`（= `__imp__VdSetGraphicsInterruptCallback`），
随后 22294 `stw r31,0(r11)` 发布 `VdGlobalDevice`。这个函数属于 guest 的 **D3D 设备创建路径**，而被 Renut 的原生 deko3d 设备接管的正是这条路径 ⇒ 注册调用被整体旁路。

连锁后果（与日志逐条吻合）：

1. 宿主不知道该调谁 ⇒ 一个中断也不发 ⇒ `irq=0`；
2. ISR `sub_822D47A0` 一次未进 ⇒ 它「进来第一件事」的那次 MMIO 读也没发生 ⇒ **`mmio_r=0`**（v8.0 已注册窗口但读数为 0，正说明从没人读过）；
3. guest 命令环从未向寄存器窗口写字 ⇒ `mmio_w=0`、`wptr=0`；
4. 真 ISR `sub_822D9570` 不跑 ⇒ `dev+16548/16552/16568` 冻结、AVIVO 主表面寄存器 `0x7FC86110`（`0x1844`）不回写（`sup1844=0`）；
5. 显示侧等待事件无人释放 ⇒ `espera sub_824F3580` 死等 4250 万次、帧管线停在第 6 次交付 ⇒ `0 fps` 黑屏。

**一句话：这是「信号源已装好、但 guest 的注册点被旁路」造成的空转，不是 v8.0 的 MMIO/线程设计有问题。**

---

## 2. 改动（T47 / v8.1-GUESTCB）

只改一个文件：**整体重写** `recomp/src/t2rotf_gpu_signal.cpp`（410 → 709 行）；头文件 `recomp/src/t2rotf_gpu_signal.h`（26 行）未变；
`recomp/src/T2ROTF_app.h` 的接入点、`repos/nfsmw-nx/sdk/src/ui/rex_app.cpp` 的 overlay 分支、`recomp/CMakeLists.txt` 的源清单**均无需改动**（v8.0 已就位）。

### 2.1 代客派发（核心改动）

`Interrupcion(fuente, cpu)` 在 `callback_ == 0` 时不再空转，而是**按 guest 自己的注册语义代客派发**：

- 回调地址：**`kCallbackGuest = 0x822D47A0`**（即 `sub_822E1690` 里注册的那个地址，不是新写的宿主函数）；
- 设备：`Dispositivo()` 用 `VdGlobalDevice` 的**两跳**结构解析——`slot 0x820011AC`（非 proc2 路径，本运行时 `X_PROCTYPE_USER=1`）退 `0x820011B0` → `cell` → `dev`，与 `device.cpp` 的 `PublishGlobalDevice` 同结构；读取一律走 `memory_->TranslateVirtual`（`TranslatePhysical` 会掩成 `0x1FFFFFFF` 偏移，已否证）。解析一次即缓存，并打印 `GPSIG: 设备解析 slot=… cell=… dev=…`；
- 参数：`{source = 0, user_data = dev}`，与 SDK `graphics_system.cpp:295-332 MarkVblank → DispatchInterruptCallback({0,user_data}, 2)` 同语义、同数目；
- 发送：`Enviar()` → `thread->SetActiveCpu(cpu)` → `function_dispatcher_->ExecuteInterrupt(thread_state(), callback, argumentos, 2)`；**唯一调用点是本 VSync 线程的循环，绝不从 guest 热路调用**（`ExecuteInterrupt` 会改写当前线程的活动上下文）；
- 手动覆盖：`sa2_renut_gpu_signal_cb_data = <dev>`（`>0` 时直接用该值，绕开两跳解析），用于排查；
- 一次性日志：`[video] T2ROTF GPSIG: 宿主代客派发 guest 图形中断回调 callback=822D47A0 dev=…（guest 注册点被 Renut 原生设备路径旁路）`。

### 2.2 指针守卫（默认开）

真 ISR `sub_822D9570` 尾部会以 `r3 = &{16548,16568,0}` 间接调用 `*(dev+16544)`（实机该字段 = `0x82880A48`，vblank 统计函数，不改事件）。
`GuardarCampo()` 在派发前检查该字段：**若它既非 0 又不落在 guest 代码区间 `[0x82000000,0x83000000)`，则本次派发期间临时清零、调用后原样还原**（首例打印一次性提示）。这避免了一个脏指针把整条中断链变成一次访问违例。
`sa2_renut_gpu_signal_cb_guard = false` 可关掉。

### 2.3 source=1（提交完成）默认关

`Completado(cpu)` 处理 ISR 的 source=1 语义（它会读 `*(dev+10900)` 换页暂存块、再调 `*(blk+16)`）。默认 **false**：
在 guest 自己的 fence 尚未说「已完成」之前，由宿主替它报完成等于**伪造进展**。即便打开，也先判 `*(dev+10900)` 是否为可用暂存块，不是就一条也不发，并对 `*(blk+16)` 同样做守卫。

### 2.4 新增诊断行 `[T2DIAG GPSIG2 …]`（只读）

与既有 `[T2DIAG GPSIG …]`（`vblanks/irq/cb/ud/r1951/mmio_r/mmio_w/wptr(次数)/rptr_pub/sup1844/hz/hostcb/skip/s1/noblk/skipS1/inv/ret`）配对，第二行给设备与队列状态：

```
[T2DIAG GPSIG2 dev=… cb16544=… v16548=… v16552=… v16568=… a16700=… a16704=…
 blk10900=… blk16=… f10940=… f10941=… f10896=… evA0=… evE0=… flagC8=…
 wD0=… fD4=… fE4=… st5F8=… st690=… cont=… obj=… cola284=… cola288=… m272=… m276=… m280=…]
```

每 2 s 一行，**全部只读**。

> **本模块的边界没有变**：`presenter()`/`provider()` 仍为 null、`has_presentation()` 为 false ⇒ 画面仍由 Renut/deko3d 出；
> **不写任何显示会计**（`cola284/cola288`、`dev+16548` 族、`*(dev+10900)` 一个字节都不写），**不碰门/泵热路**。
> 这是承接 T22/T24/T31/T33 的既定结论：宿主代笔写会计会造成回绕、假进度与成对性破坏，已多轮否证。

---

## 3. 四个新开关（G6–G9）与回退阶梯

| 键 | 默认 | 语义 | 何时改 |
|---|---|---|---|
| `sa2_renut_gpu_signal_hostcb` | **true** | 宿主代客派发 guest 图形中断回调（本轮修复本体） | 指定 `false` 即回到 v8.0 行为（`cb=0` 空转），**仅作对照** |
| `sa2_renut_gpu_signal_cb_data` | `0` | `>0` 时用该值直接作为中断参数里的设备地址（跳过两跳解析） | 排查「设备解析不出/解析错」时用（实机 dev=40070600） |
| `sa2_renut_gpu_signal_cb_guard` | **true** | 派发前对 `*(dev+16544)` 做「非 guest 代码指针就临时清零」的守卫 | 排查守卫是否误伤时临时关 |
| `sa2_renut_gpu_signal_hostcb_s1` | **false** | 连 source=1（提交完成）也由宿主代发 | 第 4 节的 A/B 第二步 |

原有键沿用不变：`sa2_renut_gpu_signal=true`、`sa2_renut_gpu_vblank_hz=60`、`sa2_renut_gpu_signal_cpu=2`、`sa2_renut_gpu_signal_diag=true`、
`sa2_renut_gpu_rptr_sync=false`、`sa2_renut_display_guard=true`、`sa2_renut_display_reconcile=true`、`sa2_renut_display_align=true`、
`sa2_renut_display_release=false`、`sa2_renut_display_agent=false`、`sa2_renut_display_tick=false`、`sa2_renut_swap_callthrough=false`。
**十六键全部为 `kInitOnly`，改完必须重启游戏。**

回退阶梯：`sa2_renut_gpu_signal_hostcb=false`（退 v8.0）→ `sa2_renut_gpu_signal=false`（退 v7.7，0 fps 黑屏）。

---

## 4. 实机验证步骤（请按序做）

1. 把新的 `T2ROTF.nro` + `T2ROTF.toml` 覆盖到 `sdmc:/switch/Transformers2ROTF/`，**完全退出再启动**。
   本次验收场景是**不超频手持**（上一版实机跑在高时钟状态，两侧都要看，但 60 帧结论只在**不超频**条件下给）。
2. **先确认版本**：日志里出现
   `[video] T2ROTF BUILD v8.1-GUESTCB (host calls the guest's own GPU interrupt callback)`。
   （同日志里还会有 `v7.6-RECON` 与 `v7.7-ALIGN` 两个旧标记行，它们都属于本 NRO；应用自身 banner 仍是旧的 `v6.27-MAPFIX`，**不要用它判版本**。）
3. 安装期新增一次性日志（各一条）：`GPSIG: 设备解析 slot=… cell=… dev=…`、
   `GPSIG: 宿主代客派发 guest 图形中断回调 callback=822D47A0 dev=…（guest 注册点被 Renut 原生设备路径旁路）`。
4. 期待（本轮验收点，全部来自只读诊断，不改变游戏行为）：
   - `[T2DIAG GPSIG …]`：`hostcb` **持续增长**、`irq` **持续增长**、**`mmio_r` 必须增长**（ISR 进来第一件事就是读 `0x7FC86544`）、
     `r1951` 增长、`sup1844` 增长、`inv` 与 `ret` 同步增长（`inv` 追不上 `ret` ⇒ 卡在 ISR 内部）；
   - `[T2DIAG GPSIG2 …]`：`v16548` / `v16568` **脱离冻结**、`a16700` 增长、`cola284`/`cola288` 不再悬挂、`m272/m276/m280` 出现非 0；
   - `T2DIAG espera sub_824F3580 n=` **不再线性暴涨**；`PERRO2` 的 `dpump` **成功数脱离 0**；`juego` fps **> 0**；**画面出现**三连 logo（luxo → atvi → hasbro）。
5. 判读分支：

   | 现象 | 指向 | 下一步 |
   |---|---|---|
   | `hostcb` 涨、`irq` 涨，但 `mmio_r` 不涨 | 回调地址/参数与实际不符 | 用 `sa2_renut_gpu_signal_cb_data=40070600` 试 |
   | `irq` 涨但 `v16548` 不涨 | 卡在 ISR 内部（或崩在 ISR 里） | 看 `inv/ret` 差、`rex_crash.log`；临时 `sa2_renut_gpu_signal_cb_guard=false` 对照 |
   | `v16548` 涨、`cola284/288` 归零、`dpump` 成功 > 0，但仍 `0 fps` | 卡点转回帧管线（门三槽/交付/呈现） | 回传 `PUERTA` 与 `m272/276/280`，下一轮转攻呈现侧 |
   | 仍死等 `F8000064` | 释放者不在 vblank 侧 | 打 `sa2_renut_gpu_signal_hostcb_s1 = true` 做 A/B（第二步） |
   | 出现访问违例 | 指针/参数错误 | 回传 `rex_crash.log`（前 40 行即可） |

6. 回退：`sa2_renut_gpu_signal_hostcb = false`。

---

## 5. 需回传的日志行

- `[video] T2ROTF BUILD v8.1-GUESTCB …`（1 条即可，用于判版本）
- `[video] T2ROTF GPSIG:` **全部**（安装期，含设备解析与「代客派发」两条）
- `[video] T2DIAG GPSIG …`（**首条 + 末条**；重点读 `irq` / `mmio_r` / `r1951` / `sup1844` / `hostcb` / `skip` / `inv` / `ret`）
- `[video] T2DIAG GPSIG2 …`（**首条 + 末条**；重点读 `v16548` / `v16568` / `a16700` / `a16704` / `cola284` / `cola288` / `m272` / `m276` / `m280` / `evE0` / `flagC8` / `wD0` / `fD4` / `fE4`）
- `[video] T2DIAG PERRO dev=…`（末条）、`[video] T2DIAG PERRO2 …`（末条）、`[video] T2DIAG PUERTA …`（末条）
- `[video] T2DIAG espera sub_824F3580 …`（末条）
- `rex_perfil.log` 的 `juego … fps` 行与 `relojes:` 行（核对是否超频/手持还是底座）
- `rex_crash.log`（若产生）

---

## 6. 静态校验

- 构建：`cd recomp/out/sw8 && ninja -j4 T2ROTF` → **退出码 0**（首次构建因 `fmt` 编译期检查报 `argument not found`——
  `Informe()` 的格式串 21 个占位符对应 20 个实参——修正后重建通过）。
- 产物核对：`T2ROTF.nro` = **59,912,946 字节**、sha256 `97f432ef…`（上一版 59,888,370 / `77230192…`）。
- 字符串取证（对随包 NRO 实算）：`v8.1-GUESTCB`=6、`T2ROTF BUILD`=3（v7.6/v7.7/v8.1 三代标记都在）、`T2DIAG GPSIG2`=3、
  `T2ROTF GPSIG`=16、`T2ROTF VSync`=1、`0x7FC80000`=4、`sa2_renut_gpu_signal_hostcb`=8、`sa2_renut_gpu_signal_cb_data`=4、
  `sa2_renut_gpu_signal_cb_guard`=4、`sa2_renut_gpu_signal_hostcb_s1`=4、`宿主代客派发`=2。
- `T2ROTF.toml` = **22,366 字节**、sha256 `3e17b811…`，**16 键**，可被 `tomllib` 解析、键值读回与源码默认值一致。
- 工程包（**原地更新**，用逐条替换 + 追加新报告的方式，不重建 staging 目录，避免条目丢失）：`T2ROTF_switch_port.zip` = **78 条目**（77 → 78，新增本轮报告），
  `testzip` 无错；替换 10 条、其余按原字节写回。逐条比对结果（**全部与磁盘对应文件逐字节一致**）：
  `T2ROTF.nro` = **59,912,946 字节 / `97f432efd8d74f833e175aa9d6f66463233fc59a32998e1797353bcf7f1a4dbe`**、
  `T2ROTF.toml` = **22,366 字节 / `3e17b8112df137e325eee056a35696798bd9ee22f8d94d5341ed879d3d6a189c`**、
  `README.md`、`recomp_src/t2rotf_gpu_signal.cpp`、`recomp_src/t2rotf_gpu_signal.h`、
  `recomp_src/renut_engine/gpu/d3d/hooks/device.cpp`、`recomp_src/t2rotf_video_nativo.cpp`、`T2ROTF_app.h`、
  `.bak.t37` / `.bak.t41` 快照、本轮报告（逐项 sha256 由打包脚本打印，含包体总体字节数与 sha256）。
  本轮首包快照：`recomp/out/sw8/zip_backup/T2ROTF_switch_port_20261030_pre.zip` = **27,579,319 字节 /
  `15e2db9b52e2a87b56601c1455770354706aa2912ebcf991b46a6d2d21d1bdb7`**（含本轮报告初版）。
  v8.0/T44 的包体（77 条目 27,547,773 字节 / `608b4fab…`）在本次**原地更新时被覆盖**，其内容随 v8.0 交付包保留在用户侧；
  更早的历史包备份在 `out/t2-tools/zip_backup/`，最近一个是 v7.7 的 `T2ROTF_switch_port_20261029_v77.zip`。

---

## 7. 残余风险（明确写）

1. **本版尚未实机验证**：以下都是静态取证 + 判读结论，不是实测结论。
2. **关键残余**：`espera sub_824F3580` 等的事件 `F8000064`，在 vblank ISR 里**没有** `KeSetEvent`——`sub_822D9570` 只做 `dev+16548 += 1`、记 `16552`、
   条件写 AVIVO `0x1844`、`dev+16700 += 1`，尾部条件间接调 `*(dev+16544)`（实机为 `0x82880A48`，vblank 统计，不改事件）。
   因此**释放者可能不在 vblank 侧**（候选：source=1 完成侧，或另一条未查实的注册路径，或 `sub_82881F30` 的 pulse 路径）。
   ⇒ 若本版只让 `irq / v16548` 动起来而等待方仍死等，就用 `sa2_renut_gpu_signal_hostcb_s1 = true` 做 A/B（这是本报告第 4 节第 5 项的第二步）。
3. **0 fps 的另一半**（门三槽 / 交付 / 泵成功数）仍待观察：显示中断链活起来 ≠ 画面出来，需 `PERRO2` 的 `dpump` 成功数脱离 0。
4. **「不超频流畅 60 帧」仍未达成、也未实测**：本轮日志显示的是高时钟状态（CPU 1785.0 / GPU 921.6 / 内存 1600.0 MHz），
   0 fps 属管线停摆；画面出来后按 README 第 5 节在**不超频**条件下实测帧率并给达标结论（待办 T26）。
5. `sub_82881F30`（帧事件 pulse 包装）的注册点、`0x82DBB5D0/D4/E4` 三个就绪字段的写入者、以及 `sub_828816D8`（安装 `*(dev+16544)`）在本移植实机是否执行过，**仍未查实**。