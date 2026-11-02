# T2ROTF deko3d 黑屏根因：显示完成信号源补齐报告（T44 / v8.0-GPUIRQ）

- 游戏：Transformers: Revenge of the Fallen（Xbox 360，Title ID `41560834`，Media ID `338852BF`）
- 工程：`J:\XBOX360 port\Transformers Revenge of the Fallen`（ReXGlue / XenosRecomp + Renut/deko3d + devkitPro/libnx）
- 本版产物：`recomp/out/sw8/T2ROTF.nro` = **59,888,370 字节**，sha256 `77230192f21b3909247c3708c9a1aba921c0a16ee4c59965e6d9e4a0657c5753`
- 本版配置：`T2ROTF.toml` = **16,470 字节**，sha256 `d1fc49fe27569df44cca3c78d783b59f78bbca413762505a2f66c04a10f3e737`（12 个键，其中 5 个为本轮新增）
- 版本标记（日志判据）：`[video] T2ROTF BUILD v8.0-GPUIRQ (host dispatches the guest GPU interrupt)`
- 本轮结论：**黑屏的最后一环是「显示完成信号源」在切到 Renut/deko3d 时被一并删掉了**——Renut 接管 Direct3D 时清空了
  `config.graphics`（`recomp/src/renut_engine/gpu/overlay/app_integration.cpp:87`），那一步连带移除了 SDK 本来附带的
  两样东西：**`0x7FC80000` 的 GPU 寄存器 MMIO 窗口**与**按垂直同步节奏派发 guest 图形中断回调的宿主线程**。
  于是 guest 显示链的入口条件（读 `0x7FC86544` 的 vblank 状态位）永远为 0、回调永远不被调用 ⇒ 显示 ISR
  `sub_822D9570` 一次未进 ⇒ `dev+16548` 族计数冻结、影片等待方 `sub_824F3580` 死等一个 F8000060 事件（34,520,000 次）
  ⇒ 帧管线停在第 6 帧、`0 fps`、有声无画。本版把那两样在宿主侧补回来（**呈现仍归 Renut/deko3d**，不写任何会计）。

---

## 1. 上一版（v7.7-ALIGN）实机判读

用户回传日志组：`rex_perfil(30).log`、`rex_stderr(66).log`、`T2ROTF_001(64).log`、`T2ROTF_002(45).log`
（SD 路径 `sdmc:/switch/Transformers2ROTF/logs/rex/`；`T2ROTF_001` 覆盖 03:23:04 → 03:24:22，2263 行）。

| 观测项 | 实机读数（逐字） | 判读 |
| --- | --- | --- |
| 版本标记 | `[video] T2ROTF BUILD v7.6-RECON (display-completion reconcile at the drain entry)` 与 `[video] T2ROTF BUILD v7.7-ALIGN (display post channel aligned with the drained channel)` 各出现一次 | 该 NRO 同时含 v7.6 配平块与 v7.7 对齐块的标记；**版本判据以 `v7.7-ALIGN` 为准**，应用自身 banner 仍是旧的 `v6.27-MAPFIX` |
| 通道对齐命中 | `T2DIAG ALIGN n=1 a_antes=41770030 dev_antes=41770018 a_ahora=82E55E38 obj=7010F9B0 o64=0 g74bc=0 c272=0 c284=0 c288=0 b256=0`（n=1..4 相同） | v7.7 的修复本体**确实在跑**（把投递通道从 `0x41770030` 改写到消费者侧 `0x82E55E38`） |
| 排空自旋 | `T2DIAG YIELD total=36 top=[82881DE8x27] [82881918x9]`，03:24:18 / 03:24:20 / 03:24:22 三次采样**完全一致** | 自旋断崖式消失（上一版 `total=35,439,751` → 现 `36` 且冻结）⇒ 对齐生效 |
| 影片等待方 | `T2DIAG espera sub_824F3580 n=34,520,000 r3=F8000060 r4=1 r5=0 lr=828818DC`（末行；全日志 1742 条 espera 行） | 影片侧在**原地死等一个 F8000060 事件**，等了 3452 万次仍无释放者 |
| 显示会计 | `T2DIAG PERRO dev=40070600 vblank16548=5 tick16552=357789309 sub16560=5 fr16564=5 disp16568=5 …`，4 秒内**一字不变** | `dev+16548` 族冻在 5（init 期那几次）⇒ **显示 ISR 一次都没进过** |
| guest 图形中断 | `T2DIAG PERRO pump … irq47=0 irq95=0 cc058=0 cc6e0=0` | 两段 ISR（`sub_822D47A0` / `sub_822D9570`）**零调用** |
| 门三槽 | `T2DIAG PUERTA n=26 … cnt=0/0/0 cola284=1 cola288=0`；`n=27 … cnt=0/0/0 cola284=0 cola288=1 obj256=0/0/0` | 门读的 arm 计数与后备标记**全程 0**；悬挂仍在 `284/288` 间来回 |
| 泵 | `T2DIAG PERRO2 dtick=7 ddrain=4/11 dpump=19/0/17 ddeliv=6 dcmd=0/0 dprod=6` | 泵进入 19 次、**成功 0 次**；投递 6 次、交付 6 次 |
| 影片链 | `PERRO pump pumpIn=1 pumpOut=1 pre1=0 pre2=1 post470=1 prep=1 cadena937=2 cadena9bf=2 despacho=2 etapa1/2/3=0/0/0 fin=0/0/0 bomba=1 iny=1 pres=0 anillo=0/0 externo=5` | 解码/注帧链启动过，但**呈现阶段（etapa 1/2/3、fin）全 0**，`pres=0` |
| 帧率 / CPU | `juego 0.0 fps`；`CPU total 113% (400% = 4 nucleos)`；`muestras 9883` | 帧循环整体不推进，CPU 全耗在空转 |
| 音频 | `3752 bloques de cliente mezclados`、`319/469 buffers audout con PCM no nulo`、`pico maximo 0.493` | 音频正常输出 ⇒ **「有声无画」** |
| 时钟 | `relojes: CPU 1020.0 MHz, GPU 460.8 MHz, memoria 1600.0 MHz`；SoC 46.8–46.9 °C | **本次运行未超频**（上限对照 CPU 1785 / GPU 921 / 内存 1600 MHz） |
| 崩溃 | `rex_stderr(66)` 共 25 行，全是初始化流程（banner、saltynx、apm、RexGmInit、`OnInitialize: LaunchModule done`）；全日志组 `crash|FATAL|Data Abort` 命中 **0** | 无访问违例、无崩溃 |

⇒ **v7.7-ALIGN 是有效但不充分的修复**：它把「投递者与消费者不同通道」这条错位修好了（自旋断崖消失、无崩溃），
但**门仍然永不就绪**、影片侧仍在死等——因为「投递」之后还缺一个**把在飞项收走的完成事件**，而那个事件在
Xbox 上由 GPU 垂直同步/完成中断提供，本移植把它删掉了。这正是本轮（T44）要补的东西。

---

## 2. 真因：被 Renut 一并删掉的「显示完成信号源」

### 2.1 guest 侧的正常链路（T44 取证）

| 环节 | 函数 / 地址 | 行为 |
| --- | --- | --- |
| 图形中断派发入口 | `sub_822D47A0`（`g_cg.irq47`） | 先读 `0x7FC86544`（= `0x7FC80000 + 0x1951*4`，寄存器 `0x1951 = D1MODE_VBLANK_VLINE_STATUS`）的 bit0，**为 1 才**调真显示 ISR |
| 真显示 ISR | `sub_822D9570`（`g_cg.irq95`） | 自增 `dev+16548` / `dev+16568`，写回 `0x7FC86110`（寄存器 `0x1844 = AVIVO_D1GRPH_PRIMARY_SURFACE_ADDRESS`），并在末尾**唤醒显示侧等待的事件** |
| 显示侧等待者 | `sub_82881848`（经 `sub_824F3580` 等事件） | 等句柄 `F8000060` 与 `*(base+284)`；这就是实机日志里死等 34,520,000 次的那条 |
| 图像帧管线 | 每帧 tick `sub_82ABB7D0` → 排空 `sub_82882200` → 泵 `sub_82881E00` → 门 `sub_82881B88` → 交付 `sub_82881930` | 门就绪的前提是通道 arm 计数；收尾靠**完成事件** |

关键点是：**这条链的入口（`0x1951` 状态位）和收尾（完成事件）都属 GPU 中断侧，而不是 frame 热路。**

### 2.2 Renut 侧删掉了什么

Renut 接管 D3D 后清空 `config.graphics`（`renut_engine/gpu/overlay/app_integration.cpp:87`），连带出现两个后果：

1. `xboxkrnl` 的 `VdSetGraphicsInterruptCallback` 走进 `graphics_system == nullptr` 分支，
   guest 注册的回调被**静默丢弃**（`sdk/src/kernel/xboxkrnl/xboxkrnl_video.cpp:319-326`，
   只在缺图形系统时打一次 `WarnNoGpuEmulation`）——日志里也只有 `[renut]` 侧的提示，没有「回调已注册」。
2. `0x7FC80000` 不再有任何 MMIO 窗口注册，生成代码里的 `REX_MM_LOAD_U32` 落到**裸 guest 物理内存**
   （偏移 `0x6544` 处读回 0）⇒ `sub_822D47A0` 看到的 vblank 状态位**恒 0** ⇒ 真 ISR 永不执行。

### 2.3 SDK 本来有完整路径（本模块按同语义最小化复活）

| SDK 位置 | 作用 |
| --- | --- |
| `sdk/src/graphics/graphics_system.cpp:141-176` | 注册 `0x7FC80000` 的 MMIO 窗口，并起一条名为 `GPU VSync` 的宿主线程 |
| `sdk/src/graphics/graphics_system.cpp:232-262` | MMIO 读值：`0x0F00` / `0x0F01` 固定值、`0x194C` 取显示高度、**`0x1951` 恒 1（vblank 门）**、`0x1961` 视口尺寸；写 `0x01C5` 记录、写 `0x1844` 吞掉 |
| `sdk/src/graphics/graphics_system.cpp:319-332` | `MarkVblank()` → `DispatchInterruptCallback(0, 2)`：以 source=0（垂直同步）、逻辑 CPU 2 派发 guest 回调 |

### 2.4 与实机读数逐位对上

| 取证结论 | 对应的实机读数 |
| --- | --- |
| MMIO 窗口缺失 ⇒ 状态位恒 0 | `irq47=0`、`irq95=0`（12 次采样全 0） |
| 回调被丢弃 ⇒ 派发线程不存在 | `dev+16548` 族 4 秒内**字面冻结**（`vblank16548=5 sub16560=5 fr16564=5 disp16568=5`） |
| 完成事件无人释放 | 影片等待方 `espera n=34,520,000`、`PUERTA cnt=0/0/0`、泵成功 0 次、`0 fps` |

---

## 3. 本轮改动

### 3.1 新模块：`recomp/src/t2rotf_gpu_signal.{h,cpp}`

`t2rotf::gpu_signal::GpuSignalSystem` 实现 `rex::system::IGraphicsSystem` 的**最小子集**，只做两件事（见 3.2 / 3.3）：

- `Wanted()` → `REXCVAR_GET(sa2_renut_gpu_signal)`；`Create()` 在关闭时返回 `nullptr`，调用方不得改动 `config.graphics`。
- 实现 `SetupPresentation`（只记 `app_context`，幂等，打一行「信号源接管」）、`SetupGuestGpu`（版本标记 + 注册 MMIO + 起线程）、
  `has_presentation()` 返回 **false**、`SetInterruptCallback` / `InitializeRingBuffer` / `EnableReadPointerWriteBack` / `Shutdown`。
- `provider()` / `presenter()` **不重写**（接口默认返回 `nullptr`）⇒ 呈现仍归 Renut/deko3d。

### 3.2 A. `0x7FC80000` 的 64 KB MMIO 窗口

`memory_->AddVirtualMappedRange(0x7FC80000, 0xFFFF0000, 0x0000FFFF, this, &LeerMmio, &EscribirMmio)`
（mask/size 与 SDK 同值），读值逐条对齐 SDK：

| 寄存器 | 偏移 | 读值 |
| --- | --- | --- |
| `RB_EDRAM_TIMING` | `0x0F00` | `0x08100748`（固定） |
| `RB_BC_CONTROL` | `0x0F01` | `0x0000200E`（固定） |
| `R500_D1MODE_V_COUNTER` | `0x194C` | `min(display_height, 0x0FFF)`（`VdQueryVideoMode`） |
| **中断状态（vblank）** | **`0x1951`** | **恒 `1`** —— ISR 唯一的门（与 SDK 同值），另外单独计数供诊断 |
| `AVIVO_D1MODE_VIEWPORT_SIZE` | `0x1961` | `(width<<16) \| height`，各取 `min(…, 0x0FFF)` |
| 其它 | — | 返回 0x5003 寄存器堆里的值（写入即存） |

写入侧：`0x01C5`（`CP_RB_WPTR`）记录 WPTR（前 8 次打 `T2DIAG GPSIG wptr=…`），**仅当 `sa2_renut_gpu_rptr_sync=true`** 时才回写读指针槽；
`0x1844` 直接吞掉（同 SDK，仅计数）；其余前 64 次打一行 `T2DIAG GPSIG reg …`。

### 3.3 B. 垂直同步派发线程

- 线程名 `T2ROTF VSync`（`XHostThread`，栈 128 KB），按 `sa2_renut_gpu_vblank_hz`（默认 60；<=0 时取 `VdQueryVideoMode().refresh_rate`）定周期。
- 每个 vblank：`SetActiveCpu(cpu)`（默认 2，同 SDK 的猜测值）→
  `function_dispatcher_->ExecuteInterrupt(thread_state(), callback, {source=0, user_data}, 2)`；`callback==0`（guest 尚未注册）时为空操作。
- 长停顿（加载/调试）后不补播几百个 vblank（>250 ms 直接对齐到当前时刻）。
- **唯一调用点**：绝不从 guest 热路调用 `ExecuteInterrupt`（它会改写当前线程的活动上下文）。
- 诊断：每 2 s 一行 `[video] T2DIAG GPSIG vblanks=… irq=… cb=… ud=… r1951=… mmio_r=… mmio_w=… wptr=…(…) rptr_pub=… sup1844=… hz=…`；
  启动时立刻打一行基线。

### 3.4 接入点：`recomp/src/T2ROTF_app.h`

`OnPreSetup` 中在 `renut::gpu::ConfigureRuntime(config)` **之后**（该调用会清空 `config.graphics`）落值：

```cpp
if (auto gpu_signal = t2rotf::gpu_signal::Create()) {
  config.graphics = std::move(gpu_signal);      // [T44 / v8.0-GPUIRQ]
}
```

`recomp/CMakeLists.txt` 已把 `src/t2rotf_gpu_signal.cpp` 加入 `T2ROTF_SOURCES`（本工程显式列举源文件）。

### 3.5 SDK 侧：`repos/nfsmw-nx/sdk/src/ui/rex_app.cpp`

overlay 分支改为 `} else if (!graphics_system || !graphics_system->presenter()) {`——
本模块是「有图形系统但 `presenter()` 为 null」的形态，不改这一行会使两个分支都落空，
immediate drawer 与 F3 调试层都会丢失。

### 3.6 明确不做

- **不提供呈现**：`presenter()` / `provider()` 保持 null，`has_presentation()` 为 false；画面仍由 Renut/deko3d/D3D 替换层出。
- **不写任何会计**（v7.5 起写会计是明令禁止的回退项）：不动 `dev+16548` 族字段，那是 guest ISR 自己的活。
- **不碰门/泵热路**：本模块没有钩任何 guest 函数，只有一条宿主线程按刷新节奏派发中断。
- **不伪造命令环读指针**：`sa2_renut_gpu_rptr_sync` 默认 **关**（Renut 的 `D3D_CBlocker_Check` 已用 fence 回答环空间等待）。

---

## 4. 开关语义与回退（本版 12 键，全部 `kInitOnly`，改完必须重启游戏）

| 键 | 默认 | 语义 / 回退 |
| --- | --- | --- |
| `sa2_renut_gpu_signal` | **true** | 本轮修复本体：装 MMIO 窗口 + 起派发线程。置 false ⇒ 回到 v7.7（门永不就绪、等待方死等、0 fps 黑屏） |
| `sa2_renut_gpu_vblank_hz` | **60** | 派发刷新率；<=0 用 `VdQueryVideoMode().refresh_rate` |
| `sa2_renut_gpu_signal_cpu` | **2** | 中断被报告为运行在哪个逻辑 CPU（同 SDK 的猜测值） |
| `sa2_renut_gpu_signal_diag` | **true** | 每 2 s 一行 `[T2DIAG GPSIG]` |
| `sa2_renut_gpu_rptr_sync` | **false** | 把 WPTR 回写进 guest 的读指针槽（只在排查环空间停等时临时打开） |
| `sa2_renut_display_guard` | true | v7.5 完成链守卫（`*(0x82DBB608)==0` 时短路四个 guest 函数） |
| `sa2_renut_display_reconcile` | true | v7.6 排空入口的在飞项配平（**兜底/对照**；正常应由 ISR 回收） |
| `sa2_renut_display_align` | true | v7.7 投递通道对齐（保留） |
| `sa2_renut_display_release` | false | v7.4 宿主写会计，已改只读 |
| `sa2_renut_display_agent` | false | v7.4 陈旧残留抽干，已改只读 |
| `sa2_renut_display_tick` | false | T25 已证伪候选（宿主代客排空） |
| `sa2_renut_swap_callthrough` | false | T25 已证伪候选（Swap call-through） |

---

## 5. 校验与取证（本轮为静态级；**尚未实机**）

- **构建**：`cd recomp/out/sw8 && ninja -j4 T2ROTF` → 退出码 **0**（codegen「0 written, 0 unchanged, 0 deleted, 1 module(s) up to date」，其余目标 up-to-date）。
- **产物**：`recomp/out/sw8/T2ROTF.nro` = 59,888,370 字节（上一版 v7.7 = 59,859,698），sha256 `77230192…`（上一版 `38e341ee…`）。
- **字符串取证（本轮实测）**：`v8.0-GPUIRQ`=6、`T2DIAG GPSIG`=4、`sa2_renut_gpu_signal`=13、`sa2_renut_gpu_vblank_hz`=4、
  `T2ROTF VSync`=1、`0x7FC80000`=4；既有标签（`v7.6-RECON` / `v7.7-ALIGN` / `reconcile` / `GUARDIA` / `PUERTA` / `PERRO2`）全部仍在。
- **接线链静态确认**：`Runtime::Setup`（`sdk/src/system/runtime.cpp:184-193`）见 `config.graphics` 非空 ⇒ 调
  `graphics_system_->Setup(dispatcher, kernel_state, app_context, with_presentation)` ⇒ `SetupGuestGpu`（注册 MMIO + 起线程）；
  `ReXApp` 侧 `config_.graphics->SetupPresentation(&app_context())`；`VdSetGraphicsInterruptCallback_entry`
  （`xboxkrnl_video.cpp:315-325`）在图形系统非空时转 `SetInterruptCallback`（本模块已实现，并会打「guest 注册图形中断回调 callback=… user_data=…」）。
- **配置**：`T2ROTF.toml` 16,470 字节、sha256 `d1fc49fe…`，`tomllib` 解析通过、12 个键值全部读回正确。
- **本包同步**：`T2ROTF_switch_port.zip` 原地更新（新 NRO / toml / README / `T2ROTF_app.h` / `rex_app.cpp`（两处）/
  两处源码 / 本报告 / 新模块源码），逐字节校验替换与新增项。

---

## 6. 实机验证步骤（**不要超频**）

1. 把新的 `T2ROTF.nro` + `T2ROTF.toml` 覆盖到 `sdmc:/switch/Transformers2ROTF/`，**完全退出再启动**。
2. **先确认版本**：日志里应有
   `[video] T2ROTF BUILD v8.0-GPUIRQ (host dispatches the guest GPU interrupt)`；
   同时会看到 `[video] T2ROTF GPSIG: …` 安装期若干行（MMIO 窗口已注册 / guest 注册回调 / ring 初始化 / 线程已启动）。
   （同日志里仍会有 `v7.6-RECON` 与 `v7.7-ALIGN` 两个旧标记行，它们都属于本 NRO，不是版本混装。）
3. **关键三项**（验收点）：`[video] T2DIAG GPSIG vblanks=… irq=… cb=… r1951=…` 中
   `vblanks` **持续增长**、`cb` **非 0**（guest 注册了回调）、`irq` **持续增长**（中断真的派发进 guest）；
   并且 `r1951` **持续增长**（guest 真的在读那个状态字）。
4. 期待：`[video] T2DIAG PERRO dev=… vblank16548=…` 的计数**不再冻在 5**；`irq47` / `irq95` **非 0**；
   `T2DIAG espera sub_824F3580 n=…` 的 n **不再线性暴涨**（等待方被释放）；
   `PERRO2` 的 `dpump` 成功数脱离 0；`juego … fps` **> 0**；**画面出现**三连 logo（luxo → atvi → hasbro）。
5. **需回传的日志行**：`[video] T2ROTF BUILD v8.0-GPUIRQ`、`[video] T2ROTF GPSIG:` 全部（安装期一次性）、
   `[video] T2DIAG GPSIG …`（首条 + 末条）、`[T2DIAG GPSIG wptr=…]`（前几条）、
   `[video] T2DIAG PERRO dev=…`（末条）、`[video] T2DIAG PERRO pump …`（末条）、
   `[video] T2DIAG PERRO2 …`（末条）、`[video] T2DIAG PUERTA …`（末条）、
   `[video] T2DIAG espera sub_824F3580 …`（末条）、`rex_perfil.log` 的 `juego … fps` 行与线程 CPU 列表、`rex_crash.log`（若产生）。
6. **回退**：`sa2_renut_gpu_signal = false`（退回 v7.7 状态）。

### 判读分支（拿到日志先对这三行，可一步定论）

| 现象 | 结论 | 下一步 |
| --- | --- | --- |
| `vblanks` 增长但 `cb=0` / `irq=0` | guest **没有**注册图形中断回调（或注册发生在启动早期被丢弃） | 查 `VdSetGraphicsInterruptCallback` 的 guest 调用点与调用时机（是否有别的初始化顺序问题） |
| `irq>0` 但 `r1951=0` | 回调被派发了，但 guest 那侧没读状态字 | 回调地址不符（`cb` 与 `g_cg.irq47` 注册值对比），或 `source` 语义不是 0 |
| `irq47>0` 但 `vblank16548` 仍冻结 | 中断进了第一段，卡在 `sub_822D47A0` 内部的前置条件或 ISR 之后的等待事件释放 | 补 `0x7FC86544` 读取前后的探针（状态位读到了但其它寄存器分支不同） |
| `vblank16548` 增长但 `0 fps` 不变 | 显示侧活了，卡点转到帧管线（门/交付/泵） | 回到门三槽与 arm 计数（`PUERTA cnt=…`）与 `PERRO2 dpump` 成功数继续定位 |

---

## 7. 残余风险与下一步

- **本轮未实机验证**：以上是静态（构建 + 字符串 + 接线链 + 配置）级结论；实机结果出来前不得宣称画面已出。
- **最可能的偏差**：guest 可能**从未调用** `VdSetGraphicsInterruptCallback`（那么 dispatch 线程会一直空转、`cb=0`），
  或调用时机早于 `SetupGuestGpu`。若日志显示 `cb=0` 且 `vblanks` 在涨，就是这一支。
- **MMIO 窗口冲突**：若 `0x7FC80000` 已被 Renut 或别处注册，本模块会打「MMIO 窗口注册失败」并返回失败状态（日志可辨）。
- **仍不包含帧率达标**：本版只解决「画面出不来」；"不超频流畅 60 帧" 需画面出来后在**不超频**条件下实测
  （对齐参考上限 CPU 1785 / GPU 921 / 内存 1600 MHz，仅作对照、不得用于达标），见 README 第 5 节与待办 T26。
- **仍然存在的旧兜底**：`sa2_renut_display_reconcile`（v7.6 配平）与 `sa2_renut_display_guard`（v7.5 守卫）保留默认开；
  若显示中断链真正跑起来，配平本应不再触发（实机 v7.6/v7.7 上它只命中 1 次即收敛，说明它治的不是根因）。