# T2ROTF（变形金刚2：卷土重来 / Xbox 360）Switch 移植
## 显示内存影子访存与设备分层解析报告（T50 / v8.2-SHADOW）

- 工程根目录：`J:\XBOX360 port\Transformers Revenge of the Fallen`
- 构建目录：`recomp/out/sw8`（`ninja -j4 T2ROTF`）
- 本轮产物：`recomp/out/sw8/T2ROTF.nro`（59,921,138 字节，sha256 `22980392fac38f4b…`）
- 交付形态：工程包 `T2ROTF_switch_port.zip` + 可单独取用的 `T2ROTF.nro` 与 `T2ROTF.toml`
- 本轮性质：**静态修复 + 静态取证已完成；实机复测数据缺失，等待用户回传日志**

---

## 0. 一句话结论

v8.1 实机「启动即 fatal」的根因是**宿主线程绕过 chunk 影子别名、直接解引用 guest 窗口地址**
（`0x820011AC`，即 `VdGlobalDevice` 槽），而本移植的 guest 内存 chunk 在提交时并不映射进窗口视图，
宿主直读必然触发 translation fault，最终落到 SDK 的致命路径 `sdk/src/system/xmemory.cpp:571`
「Unhandled guest access violation」。

v8.2-SHADOW 把 `recomp/src/t2rotf_gpu_signal.{h,cpp}` 里**全部宿主侧 guest 访存**改为经
`RexGmShadowFor()` 的 chunk 影子别名（这正是 SDK 的 Switch 异常处理器代客读写用的同一条通道），
取不到影子别名就**跳过并计数**（绝不直读、绝不直写、绝不代客提交）；同时把设备解析做成
**分层**：cvar 强制 > `VdGlobalDevice` 双槽（影子安全读）> D3D 钩子观测到 `r3`（新开关 G10），
并新增第三行诊断 `[T2DIAG GPSIG3 …]` 把这两层来源与实际用值一起打印，供一次启动内判读。

---

## 1. 本轮起点：v8.1 实机启动即 fatal（用户回传日志组）

### 1.1 日志组与关键原文

| 文件 | 关键内容 |
| --- | --- |
| `rex_crash(36).log` | 两次崩溃，**同 pc 偏移、同 guest 地址**：`pc = imagen+0x2e38088`，指令 `0xb9400041`（`LDR W1,[X2]`），`x1 = 0x820011AC`，第 1 次 `x2 = 0x7407b621ac`、第 2 次 `x2 = 0x63fdba81ac`，`esr 0x92000006`（EC `0x24`），`error_desc 0x101` |
| `rex_stderr(68).log` | `Unhandled guest access violation: read of guest 0x820011AC (host 0x0000007407B621AC) on thread 0xF8000018`（出处 `sdk/src/system/xmemory.cpp:571`）；另有 `RexGmInit` 的 "Direction F probe failed, code-memory path unavailable" 提示（非致命） |
| `T2ROTF_001(66).log` | 启动已推进到安装代码之后：987 shaders / 1178 KB、`[video] T2ROTF GPSIG: 信号源接管（无呈现；presenter=null…）`、handheld 1280x720、`banjo_renut_*`、`REXGLUE_SWITCH_NO_NVK`、`deko3d_shaders.bin` 2 variants、device ready、uam 线程、Renderer deko3d / Maxwell GM20B、memory block 32768 KiB ×2、Swapchain 3 images、image heap 64 MiB |
| `T2ROTF_002(47).log` | 与 001 同一次启动、规模相近（未逐行细读，作为同组佐证） |

### 1.2 崩点的位置（不是随机地址）

- 崩溃前的最后一行诊断就是 `[T2DIAG GPSIG]` 四要素行，其中 `cb=00000000` ⇒
  **崩点即「第一次派发尝试」的那一条链**：
  `GpuSignalSystem::BucleVblank()` → `Interrupcion(0, cpu)` → `Dispositivo()` → `Leer32(0x820011AC)`。
- far 与启动早期那行 `[DIAG] Guest memory arena mapped: virtual base 0x7385B61000, physical base 0x7485B61000`
  完全吻合：`0x7385B61000 + 0x820011AC = 0x7407B621AC`。⇒ 崩溃地址就是**guest 窗口地址的宿主映射**，
  没有任何歧义。

---

## 2. 根因：为什么在宿主线程直读 guest 窗口地址必炸

### 2.1 本移植的 guest 内存模型（决定一切）

SDK 在 Switch 上的 guest 内存由三件套管理（`repos/nfsmw-nx/sdk/src/core/guest_memory_switch.{h,cpp}`）：

1. **chunk**：内存按块提交（`kTrozo` = 4 MB），每块带一个**影子别名**（shadow alias）——
   由 `memalign` + `virtmemFindCodeMemory` + `svcMapProcessCodeMemory` + 权限置 RW 得到，
   **永远可访问**，位于一块私有地址；
2. **视图（view）**：同一个提交块映射进每个覆盖它的视图（`0xA0000000` / `0xC0000000` / `0xE0000000`），
   **每个视图在首次访问时才映射**（`RexGmFaultIn`）——Xbox 360 的地址镜像正是这样来的；
3. **异常处理器**：guest 触发 fault 时，`exception_handler_switch.cpp` 的 `EmulateViaShadow()`
   通过影子别名代客读写，**不做任何系统调用**。

关键推论：**「chunk 已提交」不等于「窗口地址已映射」**。宿主线程直接解引用
`TranslateVirtual(va)` 的结果（= `virtual_membase_ + va`）时，若该页尚未因 guest 自身访问而被
`RexGmFaultIn` 映射，就是一次 translation fault；而宿主异常路径**不会**帮你补上。

### 2.2 SDK 的致命路径为什么救不回来

`repos/nfsmw-nx/sdk/src/system/xmemory.cpp:505-585` 的 `AccessViolationCallback` 判定链是：

1. 宿主地址落在 `[virtual_membase_, physical_membase_)` 之间 → 减出 `virtual_address`；
2. `LookupHeap(virtual_address)`；
3. **这条路径既不调用 `RexGmFaultIn`，也不调用 `RexGmShadowFor`**；
4. `0x820011AC` 属于 guest 虚拟堆（不是 `kGuestPhysical` heap）⇒ 直接 `REVSYS_ERROR`，
   输出「Unhandled guest access violation」并终止。

官方 Switch 三步处理器的顺序（`exception_handler_switch.cpp` 近似 820-999 行）是：

1. `!is_write && hay_permiso && acceso == REX_GM_READ && EmulateViaShadow(ctx)` → 代客读
   （**不查窗口映射**，直接走影子别名）；
2. `RexGmFaultIn(far)` → 处理「已提交但本视图还没映射」；返回 false 的含义是
   「该处没有已提交 chunk **或已映射**」；
3. `RexGmWindowToOffset(far)` 且 offset 落在物理段 `[0x100000000, 0x120000000)` →
   按 4 MB 粒度 `RexGmCommit(..., REX_GM_WRITE)` + `RexGmFaultIn`；
4. 三步都不成立才落到 SDK handlers（即上文那条 fatal）。

⇒ 三步全不成立才会 fatal，这解释了为什么现有处理器救不回来；同时也确认了**宿主侧唯一安全的
读法就是影子别名**（第 1 步用的就是它）。

### 2.3 结论

- 宿主侧读 guest 全局必须：**`RexGmShadowFor(TranslateVirtual(va))` → 解引用影子指针**。
- 取不到影子别名（返回 NULL）= 该 chunk 未提交或不在视图内 ⇒ **跳过**，不要直读、不要直写、
  也不要代客提交（代客提交会为根本不属于 guest 物理内存的地址造出假后端，属早期被证伪的有害做法）。
- 附带确认：物理地址侧的 `PublicarRptr()` 原先也走 `TranslatePhysical` 直写，属同类风险，本版一并改走影子。

---

## 3. 改动清单（v8.2-SHADOW）

### 3.1 `recomp/src/t2rotf_gpu_signal.cpp`

| 位置 | 改动 |
| --- | --- |
| 文件头文档 | 追加 `── v8.2-SHADOW（T50）` 段：记录崩溃 pc 偏移、指令、far、esr、落点，以及「chunk 提交不映射进视图 / 取不到影子就跳过计数 / 绝不代客提交」三条纪律 |
| include 块（`#include <rex/thread.h>` 之后） | 在 `#if defined(__SWITCH__)` 保护下加入相对包含 `#include "../../repos/nfsmw-nx/sdk/src/core/guest_memory_switch.h"`（该头是 `extern "C"`，可被 NRO 直接引用；SDK 只把 `include/` 与 `switch_compat` 放进包含路径，`sdk/src` 不在其中，故必须相对包含） |
| cvar G7 `sa2_renut_gpu_signal_cb_data` | 描述改写为「影子安全读槽、读不到就跳过并落 `sinSombra`、随后交给钩子兜底」 |
| cvar G10（新增） | `REXCVAR_DEFINE_BOOL(sa2_renut_gpu_signal_dev_hook, true, "SA2/Renderer", …)`（`kInitOnly`）：把 D3D 钩子观测的设备当兜底来源 |
| 文件级状态 | 新增 `std::atomic<uint32_t> g_dev_hook{0}` 与 `std::atomic<bool> g_dev_hook_avisado{false}`（guest 线程写、VSync 线程读；取值不变不重复写） |
| 版本打印 | `[video] T2ROTF BUILD v8.2-SHADOW (guest memory read through the chunk shadow alias; device resolution layered cvar>slot pair>D3D hook)` |
| 访存原语 | `Leer32` / `Escribir32` 整段重写：新增 `void* Sombra(uint32_t va)`（`__SWITCH__` 下 `RexGmShadowFor(TranslateVirtual(va & ~3))`；非 Switch 退回旧路径便于桌面调试）；读不到影子 → `sombra_nula_lectura_++` + 一次性日志 + 返回 0；写不到 → `sombra_nula_escritura_++` + 不写 |
| 物理侧写 | 新增 `void* SombraFisica(uint32_t pa)`；`PublicarRptr()` 改走它（`TranslatePhysical` 内部会掩 `0x1FFFFFFF`，故先 4 字节对齐） |
| 设备解析 | `Dispositivo()` 改为分层：cvar(G7) > 双槽（**先 `Sombra(slot)` 判定该块可用**，再读 cell/dev）> `g_dev_hook`（G10）。两个来源都拿到时**以槽为准**，并在「设备解析 来源=槽/钩子 slot=… cell=… dev=… hookDev=… sinSombra=…/…」这一行里并列打印、不一致时明确标注 |
| 诊断 | 新增第三行 `InformeSombra()`：`[video] T2DIAG GPSIG3 hookDev=… sinSombraL=… sinSombraW=… sinSombraAvisada=…`（每 2 s，只读） |
| 类成员 | 新增 `mutable std::atomic<uint64_t> sombra_nula_lectura_{0}` / `sombra_nula_escritura_{0}` / `mutable std::atomic<bool> sombra_avisada_{false}` / `mutable std::atomic<bool> dispositivo_avisado_{false}`（`mutable` 因为 `Leer32`/`Escribir32` 是 `const` 成员但要在里面计数与落一次性日志） |
| 新接口 | 命名空间级新增 `void NotificarDispositivo(uint32_t)`：非 0、4 字节对齐、落在设备区间才接收；取值变化才写；首次落一行日志 |

### 3.2 `recomp/src/t2rotf_gpu_signal.h`

新增声明：

```cpp
// [v8.2-SHADOW] guest 线程观测到 D3D 设备（sub_822D4488 / sub_822D9570 的 r3）后上报。
// 只在 cvar sa2_renut_gpu_signal_dev_hook 打开时被消费，作为双槽解析的兜底来源。
// 可从任意线程调用：非 0、4 字节对齐、落在设备区间才接收，取值不变不重复写。
void NotificarDispositivo(uint32_t dispositivo);
```

### 3.3 `recomp/src/t2rotf_video_nativo.cpp`

| 位置 | 改动 |
| --- | --- |
| 头部 include | 加入 `#include "t2rotf_gpu_signal.h"` |
| `REX_HOOK_RAW(sub_822D4488)`（环提交外层，真正写 `CP_RB_WPTR` 的那条链） | 在既有 `g_dev_visto` latch 之后追加 `::t2rotf::gpu_signal::NotificarDispositivo(ctx.r3.u32);` |
| `REX_HOOK_RAW(sub_822D9570)`（真显示 ISR） | 同上追加一行（`r3` = D3D 设备；v8.0 的 `PERRO dev=` 行已实证其值为 `0x40070600`） |

**没有改动**：`renut_engine/**`、SDK 侧（`repos/nfsmw-nx/sdk/**`）、呈现路径、门/泵/交付热路上的一切字段。
本版不写任何显示会计、不碰 `dev+16548` 族、不从 guest 热路调用宿主（两个钩子只做一次原子上报）。

---

## 4. 构建与静态取证（本轮已完成）

### 4.1 构建

```
cd recomp/out/sw8 && ninja -j4 T2ROTF
退出码 = 0
```

构建过程中先单独编译两个改动源文件（`t2rotf_gpu_signal.cpp.obj` / `t2rotf_video_nativo.cpp.obj`）
以尽早暴露语法问题：第一次编译报 `Leer32`（`const` 成员）修改 `std::atomic` 的限定符错误，
把三个计数器与一次性日志标志改为 `mutable` 后通过；随后全量链接与打包成功。

### 4.2 产物

| 项 | 值 |
| --- | --- |
| `recomp/out/sw8/T2ROTF.nro` | 59,921,138 字节，sha256 `22980392fac38f4b…` |
| `recomp/out/sw8/T2ROTF`（未剥离 ELF） | 含调试信息，用于符号取证 |
| 版本标记字符串 | `v8.2-SHADOW`（NRO 内 3 处命中：BUILD 行 + cvar 帮助文本） |

### 4.3 符号取证（未剥离 ELF）

```
$ aarch64-none-elf-nm -g --defined-only T2ROTF | grep -i rexgm
00000000000d7480 T RexGmCommit
00000000000d6df0 T RexGmCommittedBytes
00000000000d79c0 T RexGmFaultIn
00000000000d8df0 T RexGmShadowFor          ← 影子别名接口确实进入产物（LTO/GC 未把它裁掉）
0000000002e38900 T _ZN6t2rotf10gpu_signal20NotificarDispositivoEj   ← 新接口在产物内
```

预检：`librexruntime.a` 中 `RexGmShadowFor` / `RexGmFaultIn` 均为全局定义（`T`），
且 `exception_handler_switch.cpp` 本身也引用它，链接必然保留。

### 4.4 字符串取证（NRO 内命中次数）

| 字符串 | 命中 | 含义 |
| --- | --- | --- |
| `v8.2-SHADOW` | 3 | 版本判据行 + 帮助文本 |
| `sa2_renut_gpu_signal_dev_hook` | 4 | 新开关 G10 已注册（cvar 定义名、帮助文本、取值路径、登记副本共 4 处） |
| `GPSIG3` | 1 | 新诊断行已编入 |
| `sinSombraL` | 1 | 第三行格式串已编入 |
| `所在 chunk 无 shadow` | 1 | 跳过读的一次性日志已编入 |
| `设备解析 来源` | 1 | 设备分层来源行已编入 |
| `D3D 钩子观测到设备` | 1 | `NotificarDispositivo` 首次落日志已编入 |

> 取数方法：`grep -a -o -F "<串>" recomp/out/sw8/T2ROTF.nro | wc -l`
> （GNU `strings` 只取可打印 ASCII 串，中文串与紧邻非 ASCII 的串必须用 `grep -a`，否则会误报 0）。
> 上表数字为 v8.2-SHADOW NRO（59,921,138 字节）实算值；符号（如 `RexGmShadowFor`）不以字符串计数，
> 改用 `aarch64-none-elf-nm` 对未剥离 ELF 取证（见 4.3）。

### 4.5 哈希纪律（沿用）

- 产物内嵌构建时间戳 ⇒ **同源码重构建体积相同、sha256 会变**；文档与包内出现的 NRO 哈希必须是
  随包 NRO 的实算值。
- 工程包自身与报告自身**不写死自指哈希**（由打包脚本实算打印），避免自指污染。

---

## 5. 开关语义与回退阶梯

### 5.1 本版新增/变更的键

| 键 | 默认 | 语义 |
| --- | --- | --- |
| `sa2_renut_gpu_signal_dev_hook`（G10，新增） | `true` | 双槽解析不出设备时，用 D3D 钩子观测到的 `r3` 兜底，保证代客派发（G6）仍能进行。**槽优先、钩子兜底**；两者不一致时日志明确标注「以槽为准」 |
| `sa2_renut_gpu_signal_cb_data`（G7，语义更新） | `0` | `0` = 自动解析（现在**经影子别名**读双槽）；`>0` = 强制设备地址（排查用） |

其余键（G1…G9、H、A、B、C、D）默认值与语义**不变**，详见 `T2ROTF.toml` 注释。

### 5.2 回退阶梯（改动全部是 `kInitOnly`，改键后必须重启）

| 想回到 | 操作 |
| --- | --- |
| v8.1-GUESTCB | `sa2_renut_gpu_signal_dev_hook = false`（只关钩子兜底；影子访存修复保留） |
| v8.0-GPUIRQ | `sa2_renut_gpu_signal_hostcb = false` |
| v7.7-ALIGN | `sa2_renut_gpu_signal = false`（完全不安装显示信号源） |

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
| 版本判据 | `[video] T2ROTF BUILD v8.2-SHADOW …` | 必须出现；**应用 banner 仍是旧 `v6.27-MAPFIX`，不可当判据** |
| 是否仍崩 | `rex_crash.log` / `Unhandled guest access violation` | **应消失**（本版修的就是它） |
| 影子通路 | `[T2DIAG GPSIG3 …] hookDev=… sinSombraL=… sinSombraW=…` | `sinSombraL/W` 为 0 或极小 = 影子通路通；持续增长 = 该页确实未提交（此时应见 6.3 分支 B） |
| 设备来源 | `[video] T2ROTF GPSIG: 设备解析 来源=… slot=… cell=… dev=… hookDev=…` | 应出现一次；`来源=槽` 最理想，`来源=钩子` = 走兜底 |
| 显示中断链 | `[T2DIAG GPSIG …]` 首末两行 | **重点：`irq` 与 `mmio_r` 必须增长**（v8.1 之前恒 0）；`r1951` 应开始增长；`cb` 仍是 `00000000` 属正常（guest 注册被旁路） |
| 代客派发 | `[T2DIAG GPSIG …]` 的 `hostcb=…/…` 与 `inv=/ret=` | `hostcb` 成功数应增长；`ret` 应追得上 `inv`（追不上 = 卡在 ISR 内部） |
| 设备侧字段 | `[T2DIAG GPSIG2 …]` 的 `dev`、`v16548`、`v16568`、`a16700/a16704` | `v16548`/`v16568` 应**脱离冻结（>5）** |
| 事件/帧队列 | `[T2DIAG GPSIG2 …]` 的 `cola284/cola288`、`m272/m276/m280`、`evE0/flagC8/fD4/fE4` | 卡点判读用 |
| 等待方 | `espera sub_824F3580` 末条、（若有）`PUERTA …`、`PERRO dev=` | 次数应**不再单调暴涨**；`PUERTA cnt` 应开始非 0 |
| 帧率/时钟 | `rex_perfil` 的 fps 行与 `relojes:` 行 | **`relojes` 必须是未超频值**（CPU 1020 / GPU 460.8 / 内存 1600 MHz 量级）；fps 若仍 0，见 6.3 |

### 6.3 判读分支（一次启动即可定论）

| 分支 | 观察 | 结论与下一步 |
| --- | --- | --- |
| A | `irq`/`mmio_r`/`r1951` 增长、`sinSombraL=0`、`v16548` 脱冻 | 影子修复生效、显示中断链首次真正运行 ⇒ 看 fps 与 `PUERTA cnt`，若仍 0 fps 则下一轮改判呈现门 |
| B | `sinSombraL` 持续增长、`设备解析 来源=钩子` | 该 chunk 确实未提交 ⇒ 记录 `GPSIG3` 的计数与 `GPSIG2` 的 `dev`，据此决定是否需要在**同址**建立合法后端（而不是无条件提交） |
| C | `irq` 增长但 `mmio_r` 不增长 | 说明回调被派发、但 guest ISR 没读状态字 ⇒ 下一轮试 `sa2_renut_gpu_signal_cb_data=40070600`（用钩子实证的设备值强制） |
| D | `inv` 增长而 `ret` 不涨 | ISR 内部卡住 ⇒ 看 `GPSIG2` 的 `dev+10900 / blk16`，必要时做 `sa2_renut_gpu_signal_hostcb_s1=true` 的 A/B |
| E | 仍有 `rex_crash.log` | 说明还有别的宿主直读点 ⇒ 回传新崩溃的 `pc/far/esr`（本轮已知同类风险点：`t2rotf_video_nativo.cpp` 的 `Leer32/Escribir32` 与 `PublicarRptr`，见 7.2） |

---

## 7. 未验证项与残余风险

### 7.1 本轮无实机复测数据

本报告的判读与修复全部基于**静态取证**（源码、SDK 内存模型、崩溃日志、符号与字符串）。
上一轮用户上报「本次启动直接报错，无 `rex_perfil.log` 生成」——按本轮根因，那正是宿主线程
在第一轮派发尝试时就被致命路径终止（进程在日志落盘前死掉）。**本版修复后 `rex_perfil.log`
应恢复正常生成**；请以 6.2/6.3 的回传日志为准，不要以本轮静态结论代替实机结论。

### 7.2 同源风险的其余入口（本轮未改，已记录）

`recomp/src/t2rotf_video_nativo.cpp` 的 `Leer32`/`Escribir32`（`base+addr` 直读，约 168/173 行）
与 `PublicarRptr()`（走 `TranslatePhysical`）属同类风险。本版未强制改动它们（避免扩大改动面、
且它们是否真的落在未映射窗口页尚无实机证据）。若分支 E 出现新崩溃，需按同一 shadow 方案处理。

### 7.3 桌面无法断定的两件事

1. `0x820011AC` 所在 chunk 是否**已提交**：两种情况本版都已覆盖（有影子 → 双槽解析成功；
   无影子 → 跳过 + 钩子兜底），但日志里必须能一眼区分（`sinSombra` 计数 + 设备解析来源行）。
2. 影子别名对同一 chunk 的长期可用性：SDK 语义保证「别名永远可访问」，
   但**未提交 chunk 返回 NULL**；本版据此按 NULL 处理。

### 7.4 目标验收（仍未达成，需实机）

用户硬性要求：**不需要超频、流畅 60 帧**。参考上限（仅作对照，不得用于达标）：
CPU 1785 MHz / GPU 921 MHz / 内存 1600 MHz。本版尚未做不超频实测帧率（T26 仍待办），
须等 v8.2-SHADOW 实机日志判读通过后再排。

---

## 8. 交付物清单

| 文件 | 说明 |
| --- | --- |
| `T2ROTF.nro` | 59,921,138 字节，sha256 `22980392fac38f4b…`；放 `sdmc:/switch/Transformers2ROTF/` |
| `T2ROTF.toml` | 与 NRO 同名同目录；本版新增 G10 键、G7 语义更新、头部版本块更新为 v8.2-SHADOW |
| `T2ROTF_switch_port.zip` | 工程包（原地更新：新 NRO、toml、README、本报告、`recomp_src/` 下三个改动源文件） |
| 本报告 | `T2ROTF_deko3d_显示内存影子访存与设备分层解析报告.md` |

工程包字节数、包内逐条 sha256 与包自身哈希由打包脚本实算打印（见构建目录 `_upd_pkg_t50.py` 输出），
本报告不写自指哈希。

---

## 附录 A：本轮修复为何不选另外几条路

| 方案 | 不采用的原因 |
| --- | --- |
| 继续 `TranslateVirtual` + 裸解引用读 guest 槽 | 已实机炸机两次（同 pc、同地址、同 esr）；`AccessViolationCallback` 既不查影子也不调 `RexGmFaultIn`，guest 虚拟堆地址必炸 |
| 先 `RexGmFaultIn()` 再把窗口地址裸解引用 | 会改动窗口视图映射状态、与 guest 自身 fault 时序竞争；影子别名本就是为宿主安全访问设计，读影子不触碰窗口视图 |
| 对解析失败的地址无条件 `RexGmCommit()` | 可能为根本不属于 guest 物理内存的地址造出假后端（早期「假装 guest 有进展」类做法，已证伪） |
| 只靠双槽、完全不接钩子观测 | `0x820011AC` 的 chunk 是否已提交无法从桌面断定，需要有独立第三层来源 |

## 附录 B：本轮涉及的 SDK / 工程事实（供后续复用）

- `repos/nfsmw-nx/sdk/src/core/guest_memory_switch.h`（8,071 字节）：
  `RexGmInit/RexGmBase/RexGmSize/RexGmFaultIn/RexGmShadowFor/RexGmCommit/RexGmDecommit/RexGmAddView/
  RexGmWindowToOffset/RexGmQueryProtect`、`RexGmAccess{REX_GM_NONE,REX_GM_READ,REX_GM_WRITE}`；
  整头文件包在 `#if defined(__SWITCH__)`，`extern "C"`；`RexGmShadowFor(uint64_t window_address)`
  的入参是**窗口的宿主虚拟地址**（即 `TranslateVirtual(va)` 的返回值）。
- `repos/nfsmw-nx/sdk/include/rex/system/xmemory.h:387/404`：
  `template <typename T = uint8_t*> T TranslateVirtual(uint32_t)`；
  `TranslatePhysical` 会掩 `0x1FFFFFFF`（物理地址恒小于 `0x20000000` 时无害）。
- 视图由 `memory_switch.cpp:287` 的 `RexGmAddView(base_address, file_offset, length)` 建立。
- 设备槽常量（`recomp/src/renut_engine/gpu/d3d/hooks/device.cpp`）：
  `kVdGlobalDeviceImportSlot = 0x820011B0`、`...SlotProc2 = 0x820011B0`、`...SlotOther = 0x820011AC`；
  其中 `LoadGuestU32` 是裸 `GuestPtr<const be_u32>` 解引用（无影子），同样是同类风险点。
- 本模块窗口常量：`kBaseMmio = 0x7FC80000`、掩 `0xFFFF0000`、长 `0x0000FFFF`；
  寄存器 `0x1951 → 恒 1`（显示中断状态字 bit0，ISR 唯一的门）、`0x0F00 → 0x08100748`、`0x0F01 → 0x0000200E`、
  `0x194C = min(display_height, 0xFFF)`、`0x1961 = (w<<16)|h`、`0x01C5` = `CP_RB_WPTR`、`0x1844` 吞掉。
- 构建要点：recomp 侧的编译命令行已带 `-D__SWITCH__`（devkitA64 工具链自身也预定义），
  故 `#if defined(__SWITCH__)` 分支在 Switch 构建中恒生效；`recomp/CMakeLists.txt:43` 已含
  `src/t2rotf_gpu_signal.cpp`，本版新增的 `NotificarDispositivo` 与头文件声明无需再改 CMake。