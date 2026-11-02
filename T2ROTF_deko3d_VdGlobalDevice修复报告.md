# T2ROTF deko3d 版：启动崩溃根因定论与 VdGlobalDevice 双槽修复报告

- 生成时间：2026-10-26
- 对应产物：`recomp/out/sw8/T2ROTF.nro`，59,810,546 字节，sha256 `eda7708b125987a985e8a8740d2dea072f44a26b4bc8b32477c62e6ba955a072`（上一版 `bb9ec3ea4ad5352ac0e500f926926defa287d432bd27c9eafded53a9277a3dbd`）
- 本轮日志：`deko3d.log`（12 行）、`rex_stderr(59).log`（25 行 / 1,572 B）、`T2ROTF_001(57).log` 与 `T2ROTF_002(38).log`（各 86 行 / 约 13 KB）、`rex_crash(34).log`（122 行 / 两段）

## 0. 结论

启动即崩的根因**不是**着色器、**不是** deko3d 设备初始化，而是**我们自己的 D3D create 钩子只把设备发布进了 VdGlobalDevice 两个槽中的一个**：

- T2 把 VdGlobalDevice 存成**按进程类型二选一的两槽单元**：`0x820011B0`（`KeGetCurrentProcessType()==2`）与 `0x820011AC`（其余进程类型），每槽内容是"指向单元的指针"，设备写在单元里。
- 本运行时的标题进程是 `X_PROCTYPE_USER = 1`（不是 2），所以 guest 的**全部**读取点都走 `0x820011AC`。
- 而 create 钩子原先只硬编码发布 `0x820011B0`，且 guest 自己的发布路径（`sub_822E1690`）被我们强制 `kDeviceTypeNoHardware` 提前返回而永不执行 ⇒ 槽 A 单元恒为 0 ⇒ guest 读到空设备 ⇒ 在 `device+10888`（= guest 地址 `0x2A88`）上访存崩溃。

修复已落盘（`recomp/src/renut_engine/gpu/d3d/hooks/device.cpp`），构建退出码 0，产物指纹已更新。黑屏在本轮日志中**仍未解除**（崩在第一次 D3D 设备使用，早于影片与呈现阶段），但根因已定死并修掉，需要一次实机复跑确认。

## 1. 本轮日志逐项核对

`T2ROTF_001(57).log` 与 `T2ROTF_002(38).log` 内容一致（各 86 行），计数以单份为准：

| 核对项 | 实测值 | 结论 |
|---|---|---|
| create 钩子是否仍解引用 guest `0x9` | 故障访存地址已由 `0x9` 变为 `0x00002A88`；`rex_crash(34)` 两线程 2416 / 2471 同址复现 | **已消除**；崩溃点前移到 guest 代码 |
| `Direct3D_CreateDevice` 入参打印 | 2 次，第 45 行 `r3=0x2 r4=0x00000000 r5=0x00000000 r6=0x82c5b434 r7=0x82c5a18f r8=0x9 lr=0x82a311f0 title=false`；第 76 行 `r3=0x1 r4=0x00000001 r5=0x82da75f0 r6=0x82dbb5f0 r7=0x0 r8=0x1 lr=0x82881748 title=true` | **生效**。日志直接印证了 T17 的判断：输出槽在 `r6`，而 `r8=0x9` 只是残留寄存器 |
| `Guest D3D device` 创建摘要 | 2 次：`(type 2) created at 0x40001b80: ring 0xbf0a0000/0xbf4a0000 fence 0xbef90000 scratch 0xbef90080 flags2 0x02`；`(type 1) created at 0x40070600: ring 0xbf0a0000/0xbf4a0000 fence 0xbdf80000 scratch 0xbdf80080 flags2 0x00` | 两张设备都已建立，ring / fence / scratch 已就位；标题设备就是崩在它被使用之后 |
| 钩子表装载 | `T2ROTF (TU patched/default.xex): all 249 guest interface guards match` ×1 | 249 条 T2 守卫全部匹配（TU 融合后的 `patched/default.xex`） |
| `MissingGameShaders()` | 0（两份日志中 `MissingGameShaders` 命中 0 次；`[deko3d] GLSL package: 987 shaders, 1178 KB in the NRO` 正常加载；`DKSH cache /switch/Transformers2ROTF/deko3d_shaders.bin: 0 variants`） | 着色器覆盖完整 |
| 渲染后端就绪 | `renut_gpu_backend: deko3d (this build links no Vulkan driver: REXGLUE_SWITCH_NO_NVK)`、`Renderer: deko3d on deko3d (Tegra X1, Maxwell GM20B)`、`Renderer record arena: 8 MiB of guest physical memory`、`Swapchain: 3 images`、`Render thread started` | deko3d 后端上线，无 Vulkan 依赖 |
| 三连 logo（luxo→atvi→hasbro）与影片上屏 | 无：`PRESENTAR` = 0、`etapas` = 0 | 未到影片 / 呈现阶段 |
| tick 链推进 | `T2DIAG` = 34（启动期线程 / 门 / EXC 取证），`HOOK ENTER sub_82850830` = 11 次（`n=1…65`，`len=48…401440`，最大一次 401,440 字节读取） | 视频对象钩子有进入，但 tick 链（`etapas1/2/3`）没有出现 |
| 呈现计数 | `PRESENTAR sub_827CADC0` = 0 | 呈现 0 次 |
| 崩溃 | `Unhandled guest access violation: read of guest 0x00002A88 (host 0x0000007ECD12DA88) on thread 0xF8000020`（日志末行，07:27:36.083） | 启动期崩溃，黑屏未解除 |

另外两类日志：

- `deko3d.log`（12 行，全部就绪）：`device: creating (heap 3118 MB free, process 1651 MB unused)` → `device ready: queue with zcull on, depth targets compressed, vertex shader outputs pruned per pipeline, 1 MB of queue command memory; uam compile thread started` → `uam thread: ready (init 5 ms, self-test 512 bytes of DKSH in 3 ms)` → `swapchain: 3 images of 1280x720 (format 117), swap interval 1` → `image heap: chunk 1 of 64 MiB` → `shader code: block 1 (16 MiB)`。**结论：deko3d 设备 / 队列 / 交换链 / 图像堆 / 着色器块全部就绪，与本崩溃无关。**
- `rex_stderr(59).log`（25 行 / 1,572 B，仅引导段）：`RexGmInit` 返回 `base=0x360f06c000 size=0x120000000`；`svcMapProcessMemory` rc=`0x0000D401` ⇒ Direction F 的 code-memory 路径不可用（按设计改走 on-demand fault-in）；`SetupPresentation done`、`ConstructRuntime done`、`LaunchModule done`。我们的 RGPU 诊断**不**走这里（走 `T2ROTF_001/002` 与 `deko3d.log`）。

## 2. 根因定论与证据链

1. **崩溃现场**（`rex_crash(34)`，两段完全相同）：线程 2416 / 2471，`pc = imagen+0x22c3c5c`、`lr = imagen+0x22c3c48`、`esr 0x92000006 (EC 0x24)`、`instruccion 0xb8604aa0`、`far dentro de la memoria del invitado: 0x00002a88`；`x0 = 0x2a88`、`x22 = 0x822cbd14`（guest 代码地址 ⇒ 崩在 guest 重编译代码内，不是宿主钩子内）；两次运行 arena 基址不同（`x8 = 0x2e4bbb6000` / `0x360f06c000`）却同址复现 ⇒ 确定性、与 arena 基址无关。
2. **访存地址语义**：`0x2A88 = 10888`，即设备内偏移 10888 的字段（create 时由 `sub_824F0658(dev)` 写入的句柄槽）。访问 `device+10888` 却落在 guest 地址 `0x2A88` ⇒ **device 指针为 0**。宿主 `pc` 对应的重编译函数体是 guest `sub_822CBC00` 的重编译产物（`T2ROTF_recomp.35.cpp:21715`），其中 `lwz r11,10888(r31)` 与 `far 0x2a88` 一一对应。
3. **设备指针的唯一来源**：guest 的 device 指针全部来自 VdGlobalDevice ⇒ 说明 VdGlobalDevice 读出来是 0。
4. **VdGlobalDevice 的 T2 形态（字节级取证）**：对 `extracted/TU/default_tu.flat`（15,925,248 字节，覆盖 guest `0x82000000`–`0x82F30000`）按 `lwz = op 32` / `stw = op 36` / `addi-addis = op 12–15`、位移 `4524/4528` 扫描，命中 **54 处**站点，几乎全部是成对形态 `lwz rX,4528(base)` + `lwz rX,4524(base)`，即**按 `KeGetCurrentProcessType()` 在槽 B（`0x820011B0`，`==2`）与槽 A（`0x820011AC`，否则）之间二选一**；发布点 `sub_822E1690`（`0x822E1620` / `0x822E173C` / `0x822E1748`）也只写自己进程类型对应的那一槽。每槽内容是**指向单元的指针**，设备写在单元里（`lwz r11,槽; stw r31,0(r11)`），读取侧多一次解引用（`lwz r31,0(r11)`）。
5. **进程类型**：本运行时的标题进程是 `X_PROCTYPE_USER = 1`（`repos/nfsmw-nx/sdk/src/system/kernel_state.cpp:123`；`X_PROCTYPE_IDLE=0 / USER=1 / SYSTEM=2`），**不是 2** ⇒ guest 全部读取点走槽 A `0x820011AC`。
6. **我们的钩子（缺陷所在）**：`hooks/device.cpp` 原先把 `kVdGlobalDeviceImportSlot` 硬编码为 `0x820011B0`，只在 create 时发布槽 B；而 guest 自己的发布路径 `sub_822E1690:22275-22295` 因我们在 `Sa2CreateDevice` 里以 `kDeviceTypeNoHardware(=2)` 提前返回而永不执行 ⇒ **槽 A 单元从未被写过** ⇒ 读到 0 ⇒ 崩。
7. **同线程闭合**：日志中 create、`HOOK ENTER sub_82850830`（11 次）与崩溃都在同一宿主线程 `t12267698574883706005`（guest handle `0xF8000020`）上 ⇒ 创建与使用的进程类型一致，链条闭合。
8. **责任层级**：在**我们宿主 native 钩子的 guest 状态写回**，不在 guest 自身、不在着色器、不在 deko3d（其设备/队列/交换链/图像堆/着色器块全部就绪）。

## 3. 修复内容（本轮落盘，原地修改）

`recomp/src/renut_engine/gpu/d3d/hooks/device.cpp`（1,494 → 1,537 行），三处改动：

1. 新增两槽常量：`kVdGlobalDeviceImportSlotProc2 = 0x820011B0`、`kVdGlobalDeviceImportSlotOther = 0x820011AC`（旧单槽常量保留作注释性别名）。
2. 新增 `PublishGlobalDevice(device_va)`：按 `KeGetCurrentProcessType()` 选主槽写设备单元；**另一槽单元若仍为空则一并补齐**（只填空、不覆盖，避免影响"系统进程自有设备"的理论场景）；打印 `VdGlobalDevice: procType=… slot … cell … -> …`，主槽无单元时 `RGPU_ERROR` 报错。`InitializeTitleDevice()` 与 `Sa2CreateDevice()` 的标题分支改为调用它。
3. `GlobalDevice()` 改为进程类型感知 + 另一槽兜底，避免宿主侧调用点（`IsFencePending_hook` / `D3DDevice_BlockOnFence_hook` / `D3DResource_GetDevice_hook`）因槽选错拿到 0。

上一轮（T17）已完成的 create ABI 修正仍在：四参 `r3=DeviceType / r4=BehaviorFlags / r5=D3DPRESENT_PARAMETERS* / r6=ppReturnedDeviceInterface`，并对输出槽加守卫（`out_va < 0x10000 || out_va & 3` → `E_POINTER 0x80004003`）。本轮崩溃地址从 `0x9` 变为 `0x2A88`，正是该修复生效的直接证据。

## 4. 构建与产物取证

- 构建：`cd recomp/out/sw8 && ninja -j4 T2ROTF` ⇒ 退出码 0；只重编 `device.cpp`（52 个目标中 1 个编译 + 重链 + 打包 NRO）。
- 产物：`T2ROTF.nro` = **59,810,546 字节**，2026-10-26 14:18，sha256 `eda7708b125987a985e8a8740d2dea072f44a26b4bc8b32477c62e6ba955a072`。
- 二进制串取证：`VdGlobalDevice: procType=` ×1、`holds no cell; the device stays unpublished` ×1、`is not a guest output slot` ×1（T17 守卫仍在）、`GLSL package` ×1。
- 工程包原地更新：`_pkg_v635/T2ROTF.nro` 与 `T2ROTF_switch_port.zip` 内的 `T2ROTF.nro` 已替换为本版（sha256 同上），两处 `README.md` 已同步更新。

## 5. 兼容边界

- 改动只在 `hooks/device.cpp`：不触碰 codegen 产物、249 条钩子守卫表、987 条着色器包、TU/DLC 目录布局与 VFS overlay 修复、libnx 初始化顺序。
- 双槽发布是**填空式**：另一槽已有内容时不覆盖。
- `GlobalDevice()` 的尝试顺序（先当前进程类型槽、再另一槽）与 guest 读取点一致。
- 尚未改动（潜在次级隐患，已在日志取证范围内）：
  - `hooks/draw.cpp:619 / 636 / 650` 仍按参考端高寄存器取参（含两个 guest 指针：`point_va = ctx.r7`、`color_va = ctx.r10`），T2 真签名未验证——若下次崩溃落在这些点，用同样的字节级取证法定位。
  - `D3DDevice_SetRingBufferParameters_hook` 仍直接返回 S_OK，不写回 guest 10896 / 10900（当前由 create 钩子手工补 `fence_va` / `fence_va+0x80`）。

## 6. 待实机验证项（下次复跑逐条核对）

1. **启动不再崩**：无 `Unhandled guest access violation`，不应再出现 guest `0x00002A88`。
2. **新增诊断出现**：`VdGlobalDevice: procType=… slot … cell … -> …; slot … cell … -> …`。期望 `procType=1`、主槽 = `0x820011ac`；若实测 `procType=2`，说明进程类型判断需按实测修正（两槽都填，功能上仍正确）。
3. `MissingGameShaders` 仍为 0、`all 249 guest interface guards match` 仍出现。
4. **三连 logo**（luxo → atvi → hasbro）按序上屏，随后主菜单。
5. **影片与呈现**：`etapas1/2/3` 与 `PRESENTAR sub_827CADC0` 计数 > 0，画面出现（不再是黑屏）。
6. 若仍有崩溃：回传 `rex_crash(N)` + `deko3d.log` + `T2ROTF_001(N)` + `T2ROTF_002(N)` + `rex_stderr(N)` 五类日志。

## 7. 60 帧风险（不超频）

- 本版**尚未进入呈现循环**，不超频 60 帧**尚未实测**；该项只能在启动跑通后测：`logs/rex/rex_perfil.log` 的 `juego X fps`（达标线 ≥ 59.5 持续），同时用 `relojes` 行证明未超频。
- 已知风险：参考项目 `mgr-nx-deko`（同架构：ReXGlue 静态重编译 + Renut/deko3d）自报在**超频**（CPU 1785 MHz / GPU 768 MHz）下菜单 55–60、游戏内 40–52 FPS；本工程要求**不超频** 60 FPS，压力明显更大。
- 允许的不超频手段（官方 apm 配置，非超频）：当前已请求配置 `0x92220008`（GPU 460.8 MHz）；用户给出的上限为 CPU 1785 MHz / GPU 921 MHz / 内存 1600 MHz。若 460.8 MHz 不达标，候选依次为：提高官方性能档（仍在上限内）、内部分辨率降至 1024x576、LTO/PGO 构建、deko3d 提交与缓存调优（2 MB 提交粒度、4 MB fault-in 已落）。