/**
 * @file    t2rotf_gpu_signal.cpp
 * @brief   [T44/v8.0-GPUIRQ → T47/v8.1-GUESTCB] 补回被移除的 GPU 显示信号源：
 *          MMIO 窗口 + 垂直同步中断派发线程 + **宿主代客派发 guest 自己的图形
 *          中断回调**。不参与呈现（presenter 保持 null）。
 *
 * ── 为什么需要它（逐条取证）──────────────────────────────────────────────
 * 1. guest 的显示侧靠图形中断回调推进：sub_822D47A0 在 source==0（垂直同步）时
 *    先读 GPU 中断状态字 0x7FC86544（寄存器 0x1951）的 bit0，为 1 才调用真正的
 *    显示 ISR sub_822D9570；后者自增 dev+16548/+16568、写回 0x7FC86110
 *    （AVIVO_D1GRPH_PRIMARY_SURFACE_ADDRESS，寄存器 0x1844），并在末尾以
 *    *(dev+16544)（=0x82880A48，vblank 统计回调）收尾。
 * 2. 本移植改走 Renut（Direct3D 级接管）后 config.graphics 被清空
 *    (renut_engine/gpu/overlay/app_integration.cpp:87)，于是：
 *      - xboxkrnl 的 VdSetGraphicsInterruptCallback 走
 *        "graphics_system == nullptr" 分支，回调被静默丢弃
 *        (sdk/src/kernel/xboxkrnl/xboxkrnl_video.cpp:319-326)；
 *      - 0x7FC80000 没有任何 MMIO 窗口注册，生成代码的 REX_MM_LOAD_U32 落到
 *        裸 guest 物理内存（值为 0）=> 中断状态位恒 0 => 显示 ISR 永不执行。
 *    实机现象正是：dev+16548 / dev+16552 冻在 5（init 期那几次）、
 *    sub_822D9570 一次未进、排空判据永久悬挂 => 0 fps 全黑。
 * 3. SDK 原本有完整路径（sdk/src/graphics/graphics_system.cpp:141-176 注册 MMIO
 *    并起 "GPU VSync" 线程，:319-332 MarkVblank -> DispatchInterruptCallback(0,2)），
 *    本模块按同一语义把它最小化地补回来，只覆盖 Renut 不负责的那部分。
 *
 * ── 本模块做什么 ────────────────────────────────────────────────────────
 * A. 注册 0x7FC80000 的 64KB MMIO 窗口，返回值与 SDK 完全一致
 *    (graphics_system.cpp:232-262)：0x0F00/0x0F01 固定值、0x194C 取显示高度、
 *    0x1951 恒 1（vblank，ISR 唯一的门）、0x1961 视口尺寸；其余返回寄存器堆。
 *    写入：0x01C5(CP_RB_WPTR) 记录并可回写读指针、0x1844 吞掉（同 SDK）。
 * B. 起一条宿主线程，按显示刷新率（VdQueryVideoMode().refresh_rate，可用 cvar
 *    覆写）派发 guest 图形中断回调：ExecuteInterrupt(thread_state(), callback,
 *    {source=0, user_data}, 2) —— 与 SDK 的 MarkVblank 一致。这条线程是唯一
 *    调用点；绝不从 guest 热路调用（ExecuteInterrupt 会改写当前线程的活动上下文）。
 *
 * ── 明确不做 ───────────────────────────────────────────────────────────
 * - 不提供 presenter()/provider()、has_presentation() 返回 false：呈现仍归
 *   Renut/deko3d。ReXApp 的 overlay 分支由 rex_app.cpp 的
 *   "else if (!graphics_system || !graphics_system->presenter())" 接住。
 * - 不消费命令环、不写呈现会计、不碰 dev+16548 族字段：那是 guest 的 ISR 的活。
 *
 * ── v8.1-GUESTCB（T47）────────────────────────────────────────────────────
 * v8.0 的派发线程确实跑起来了（实机 vblanks 每 2 秒 +120），但 irq=cb=mmio_r=mmio_w
 * 全 0、wptr 一条没有：guest 侧的注册点 sub_822E1690（generated/default/
 * T2ROTF_recomp.28.cpp:22262-22274：lis/addi 出 0x822D47A0、mr r4,r31(dev)、
 * bl VdSetGraphicsInterruptCallback）被 Renut 的原生 D3D 设备创建路径整体旁路，
 * SetInterruptCallback 从未被调到（本模块那条“guest 注册”日志实机一次未打印）。
 * 本版因此让宿主按**同一语义代客派发**：同一个 guest 回调地址 0x822D47A0、
 * 同一个 guest 设备对象（自 VdGlobalDevice 的两跳槽位解析）、同一组参数
 * {source, device} —— 与 SDK graphics_system.cpp 的
 * MarkVblank → DispatchInterruptCallback({0, user_data}, 2) 完全同构，只是回调
 * 来源由“guest 注册”改为“guest 本该注册的地址”，不再让整条中断链为空。
 * 附带两条默认关的排查开关：source=1（提交完成）代发、以及“设备内会被 ISR
 * 间接调用的 guest 回调不是 guest 代码地址就临时清零”的守卫。
 *
 * ── v8.2-SHADOW（T50）────────────────────────────────────────────────────
 * v8.1 实机启动约 0.4 s 即 fatal：pc=imagen+0x2e38088 的 `LDR W1,[X2]` 打在
 * 0x820011AC 上，far=0x7407B621AC = arena 基址 + 0x820011AC、esr=0x92000006
 * (EC 0x24, translation fault)，落点是 xmemory.cpp:571 的
 * "Unhandled guest access violation"。地址没算错——错的是读了不该直读的内存：
 * 本移植的 guest 内存由 guest_memory_switch.cpp 管理，chunk 在提交时**不**映射进
 * 窗口视图，只有 guest 自己第一次访问（RexGmFaultIn）才映射；宿主把
 * TranslateVirtual 的结果直接解引用，对一个「已提交但未映射 / 尚未提交」的窗口页
 * 就是一次必然的 translation fault，而 Switch 的异常处理器只对「逻辑受保护页的读」
 * 代客（EmulateViaShadow）、对「物理段首次触碰」代客提交，其余一律落到 fatal
 * （exception_handler_switch.cpp 的 (1)(2)(3) 三步都不成立 → SDK handlers → fatal）。
 * 本版把本模块**所有**宿主侧 guest 访存改走 chunk 的 shadow 别名
 * （RexGmShadowFor：永远可访问，SDK 自己的异常处理器就是用它代客读写）：取不到
 * shadow 就跳过并计数 sinSombra，绝不直读直写、也绝不代客提交（那会为根本不属于
 * guest 物理内存的地址造出假后端）。设备解析同时改为分层：
 * cvar > VdGlobalDevice 双槽（shadow 安全读）> D3D 钩子观测（sub_822D4488 /
 * sub_822D9570 的 r3，实机 = 0x40070600），三层互证并在日志里同时给出，
 * 任一路由可解析即可派发。
 */

#include "t2rotf_gpu_signal.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/system/xobject.h>
#include <rex/system/xthread.h>
#include <rex/system/xtypes.h>
#include <rex/system/xvideo.h>
#include <rex/thread.h>

#if defined(__SWITCH__)
// [v8.2-SHADOW] guest 窗口的 chunk shadow 别名：宿主侧唯一安全的 guest 访存通道。
// SDK 的 include 目录里没有 sdk/src（repos/nfsmw-nx/sdk/CMakeLists.txt 只暴露
// include/ 与 switch_compat），所以走从本文件出发的相对路径；该头文件整体受
// #if defined(__SWITCH__) 保护，且是 extern "C"（可被 NRO 直接引用）。
#include "../../repos/nfsmw-nx/sdk/src/core/guest_memory_switch.h"
#endif

namespace t2rotf::gpu_signal {

// X_STATUS_SUCCESS/X_STATUS_UNSUCCESSFUL 这套宏展开成 ((X_STATUS)...)，所以本命名空间
// 必须能看见裸 X_STATUS 类型名（SDK 内部同类代码都在 rex:: 之下才成立）。
using rex::X_STATUS;

// ===========================================================================
// cvar：全部 kInitOnly，改动需重启游戏。
// ===========================================================================
REXCVAR_DEFINE_BOOL(sa2_renut_gpu_signal, true, "SA2/Renderer",
                    "[T2ROTF] v8.0-GPUIRQ: put back the GPU signal source the Renut "
                    "port removed with the Xenos graphics system, i.e. the GPU register "
                    "MMIO window at 0x7FC80000 and the host thread that dispatches the "
                    "guest's graphics interrupt callback once per display refresh. The "
                    "guest's display gate polls that MMIO status word (0x7FC86544, "
                    "register 0x1951) before its display ISR sub_822D9570 can run, and "
                    "without a callback source both are dead: dev+16548 frozen, 0 fps, "
                    "black screen. Off: exactly the old behavior (no MMIO window, no "
                    "callback), for A/B evidence gathering.")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(sa2_renut_gpu_vblank_hz, 60, "SA2/Renderer",
                     "[T2ROTF] v8.0-GPUIRQ: display refresh rate the signal thread "
                     "dispatches the guest's graphics interrupt callback at, in Hz. "
                     "0 or negative: use the value the guest asked for through "
                     "VdQueryVideoMode (its refresh_rate). 60 matches the Switch's "
                     "handheld panel and what the guest configures.")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(sa2_renut_gpu_signal_cpu, 2, "SA2/Renderer",
                     "[T2ROTF] v8.0-GPUIRQ: logical CPU index the graphics interrupt "
                     "is reported as running on (SetActiveCpu before ExecuteInterrupt), "
                     "as the SDK's own DispatchInterruptCallback does with a guess of 2. "
                     "The guest reads its logical CPU from the PCR, so this only changes "
                     "the identity the guest sees inside the interrupt.")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(sa2_renut_gpu_signal_diag, true, "SA2/Renderer",
                    "[T2ROTF] v8.0-GPUIRQ: log one '[T2DIAG GPSIG]' line every 2 s with "
                    "the MMIO read/write counts, how many times the interrupt-status word "
                    "0x1951 was read, the CP_RB_WPTR writes, the read-pointer publications, "
                    "the dispatched vblanks and the guest callback address in use. "
                    "v8.1-GUESTCB adds a second '[T2DIAG GPSIG2]' line with the resolved "
                    "device, its vblank/completion counters and the guest's display-event "
                    "and frame-queue fields, so a stalled display thread can be pinned from "
                    "the log alone.")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(sa2_renut_gpu_rptr_sync, false, "SA2/Renderer",
                    "[T2ROTF] v8.0-GPUIRQ: also publish the write pointer into the "
                    "read-pointer write-back slot the guest programmed through "
                    "VdEnableRingBufferRPtrWriteBack, i.e. tell the guest the GPU consumed "
                    "the ring immediately. Off by default: Renut's own D3D replacement "
                    "(renut_native_sub_822D13F0 = D3D_CBlocker_Check) already answers the "
                    "guest's ring-space wait from its fences, and a fabricated read pointer "
                    "is exactly the kind of fake guest progress that broke earlier builds. "
                    "Turn it on only while gathering evidence of a ring-space stall.")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(sa2_renut_gpu_signal_hostcb, true, "SA2/Renderer",
                    "[T2ROTF] v8.1-GUESTCB: the host dispatches the guest's own graphics "
                    "interrupt callback (guest address 0x822D47A0, the one sub_822E1690 "
                    "hands to VdSetGraphicsInterruptCallback at generated/default/"
                    "T2ROTF_recomp.28.cpp:22262-22274) with the same arguments the guest "
                    "would have received: {source, device}. Needed because the Renut "
                    "native-device path bypasses that registration, so SetInterruptCallback "
                    "is never called and the whole display interrupt chain stays dead "
                    "(dev+16548 frozen at 5, sub_822D9570 never entered, 0 fps). If the "
                    "guest ever does register, its callback wins and this changes nothing. "
                    "Off: exactly the v8.0 behavior (dispatch only what the guest registered).")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(sa2_renut_gpu_signal_cb_data, 0, "SA2/Renderer",
                     "[T2ROTF] v8.1-GUESTCB: device address passed as the interrupt "
                     "callback's second argument when the host dispatches on the guest's "
                     "behalf. 0 = autodetect: read slot 0x820011AC (this runtime's process "
                     "type is USER=1, so that is the cell the guest itself reads), then the "
                     "cell it points to, and use the device found there (fallback "
                     "0x820011B0). v8.2-SHADOW reads those slots through the chunk shadow "
                     "alias only: if their page is not committed the read is skipped "
                     "(sinSombra) and the D3D hook observation is used instead. "
                     "Nonzero: force this address (for A/B evidence).")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(sa2_renut_gpu_signal_dev_hook, true, "SA2/Renderer",
                    "[T2ROTF] v8.2-SHADOW: third device-resolution layer. Accept the D3D "
                    "device address observed in this title's own D3D hooks "
                    "(sub_822D4488 and sub_822D9570, r3; on the console it is 0x40070600) "
                    "as the interrupt callback's second argument, for when the "
                    "VdGlobalDevice slot pair 0x820011AC/0x820011B0 cannot be resolved "
                    "through the chunk shadow alias (their page is not committed, and the "
                    "host must never touch it raw). The slot pair still wins when both "
                    "resolve; the two values are cross-checked and logged. Off: slot pair "
                    "only (no device -> no host-dispatch, i.e. v8.1 behavior minus the "
                    "fatal).")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(sa2_renut_gpu_signal_hostcb_s1, true, "SA2/Renderer",
                    "[T2ROTF] v8.5-COMPLETE: dispatch source=1 (submission completed) to the "
                    "same guest callback, which is the branch that reads *(dev+10900) (the "
                    "swap scratch block) and calls *(block+16). Default is ON now: the v8.4 "
                    "logs proved this interrupt had never been dispatched once in this port "
                    "(skipS1 == vblanks, s1 == 0, dev+16568 frozen at 5), and it is the only "
                    "path inside the guest's own ISR that can retire a completed display "
                    "item - without it base+288 can never reach zero and the drain loop "
                    "spins forever. Nothing is dispatched while *(dev+10900) is still zero "
                    "or is not a plausible block address. Off: v8.4 behavior (completion "
                    "never reported).")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(sa2_renut_gpu_signal_hostcb_s1_mode, 2, "SA2/Renderer",
                     "[T2ROTF] v8.5-COMPLETE: when to report submission completion. "
                     "0 = never, 1 = only while a display item is actually in flight "
                     "(*(0x82E55CE0+284) or +288 non-zero), 2 = every refresh, which is the "
                     "hardware's own cadence (one completion interrupt per presented "
                     "frame). Default 2; use 1 to A/B whether the extra reports matter.")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(sa2_renut_gpu_signal_cb_guard, true, "SA2/Renderer",
                    "[T2ROTF] v8.1-GUESTCB: before dispatching, if a field the guest ISR "
                    "indirectly calls through (dev+16544, and *(dev+10900)+16 for source=1) "
                    "holds an address outside this title's guest code range "
                    "(0x82000000-0x82FFFFFF), zero it for the duration of the dispatch and "
                    "restore it afterwards, so a stray pointer cannot turn the ISR tail "
                    "into a jump into raw data. Off: call the guest chain exactly as the "
                    "ISR would, faults included (A/B only).")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// ===========================================================================
// v8.3-FRAME：宿主按真实刷新节奏驱动 guest 自己的每帧显示提交
// （v8.2-SHADOW 判读：中断侧已通、每帧提交侧从未产出；见 DirigirFotograma 注释）
// ===========================================================================
REXCVAR_DEFINE_BOOL(sa2_renut_display_drive, true, "SA2/Renderer",
                    "[T2ROTF] v8.3-FRAME: on every real refresh the VSync thread calls the "
                    "guest's own per-frame display commit (sub_82880978; no arguments, driven "
                    "by guest globals only) so the display manager actually receives the work: "
                    "v8.2 proved the interrupt side works but that commit side never ran "
                    "(*(0x82E55CE0+284)=0, 288=1, inKick=0, ringR=ringW=0, present frozen). "
                    "No accounting field is written by hand, the gate/pump hot path and the "
                    "safe-shadow guest reads are untouched, and as soon as the guest pipeline "
                    "itself moves (dev+16568 or base+284) the driver yields for three refreshes "
                    "so presents are never doubled. Off: exactly v8.2 behaviour (A/B only).")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(sa2_renut_display_drive_after_ms, 2500, "SA2/Renderer",
                     "[T2ROTF] v8.3-FRAME: start driving only after this many milliseconds "
                     "since the VSync thread started, so boot/init runs untouched "
                     "(0 = drive from the first refresh).")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(sa2_renut_display_drive_div, 1, "SA2/Renderer",
                     "[T2ROTF] v8.3-FRAME: drive one refresh out of N (1 = every refresh, "
                     "2 = 30 Hz, ...) for A/B bisecting.")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(sa2_renut_display_drive_produce, true, "SA2/Renderer",
                    "[T2ROTF] v8.8-LIVENESS: on by default now. When the per-frame display "
                    "commit (sub_82880978) has been driven but produced nothing "
                    "(*(0x82E55CE0+284) is still 0), the driver additionally calls the guest's "
                    "own native producer wrapper (sub_82882730): that call enqueues the entry, "
                    "bumps base+284 and signals the frame event *(0x82DBB5E0)=F8000068 -- the "
                    "very signal the guest present thread (sub_82881848) is stuck on. It is "
                    "never called while a frame is already pending (284 != 0), so nothing is "
                    "ever delivered twice. Off: commit entry only.")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(sa2_renut_display_drive_commit, false, "SA2/Renderer",
                    "[T2ROTF] v8.9-PRODUCE: OFF by default; gates the guest per-frame display "
                    "commit entry sub_82880978. v8.8 device evidence: the first call ever made "
                    "to it (19:09:18.102) turned base+284 from 0 to 1, then that same thread "
                    "entered the drain and never came back (DRAIN-IN n=13 was its last output; "
                    "DRIVE then reported dentro=1 for the whole run, drive=2 ok=1, and "
                    "produce=0 prodSinDev=0 because the produce branch sits *after* the commit "
                    "that never returns). The commit is therefore kept reachable for A/B only, "
                    "and the frame is posted through the guest's own producer instead (see "
                    "sa2_renut_display_drive_produce). On: v8.8 behaviour (commit first).")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(sa2_renut_display_drive_pump_max, 0, "SA2/Renderer",
                     "[T2ROTF] v8.9-PRODUCE: 0 = off (default). Upper bound for the "
                     "non-blocking hand-off of an already-posted frame: sub_82881E00 with "
                     "r3 = 0x82E55E20 (display container), r4 = -1 (global in-flight) and r5 = 1 "
                     "(non-blocking, so it can never enter the blocking wait the guest present "
                     "thread sits in). The guest's own render thread is the natural presenter "
                     "(it is alive and polling base+284 inside sub_82881848), so the host leaves "
                     "the hand-off to it by default; set 1..4 only if the device log shows "
                     "base+284 posted but never consumed.")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(sa2_renut_display_drive_watchdog_ms, 500, "SA2/Renderer",
                     "[T2ROTF] v8.9-PRODUCE: if the dedicated driver thread stays inside guest "
                     "code longer than this, print one T2DIAG DRIVE-WEDGE snapshot (phase decode "
                     "plus device / base+284 / base+288 / ring state) and stop driving for good, "
                     "so a wedged thread can no longer silently swallow every later diagnostic. "
                     "0 = off.")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// ===========================================================================
// v8.4-SPLIT：每帧提交驱动从「垂直同步派发线程」搬到独立线程
//
// 判读依据（T55，v8.3-FRAME 实机日志组 33/70/68/49）：
//   v8.3 的驱动确实把 guest 自己的每帧提交叫起来过一次（DRIVE n=153 观测到
//   base+284 由 0 变 1），管线真的跑了 6 帧（bomba=iny=pres=6、etapa1/2/3=6/6/6）；
//   但同一份日志里，宿主「垂直同步派发线程」t1969709635156801032 的最后一条输出是
//   00:12:48.048 的 `T2DIAG DRAIN-IN n=13 idx=0 c24=1 c28=0 e24=1 e28=0`，此后 60 s
//   再无一字：vblanks/irq 停在 131/94（GPSIG 报告只到 00:12:47.431 的 vblanks=120，
//   下一次 2 s 报告再也没来），irq47/irq95 冻结、显示线程在 F8000064 上空转 2.88e7 次。
//   => 驱动把「本该由垂直同步线程派发的显示完成中断」的唯一来源掐掉了，而 guest 的
//      排空 sub_82882200 正在等这个完成 —— 在同一个线程里自锁。
//
// 因此：驱动必须跑在**另一个**线程上。垂直同步线程只负责派发中断与报告，绝不再进入
// guest 代码；驱动线程即使卡在 guest 提交内部，也只损失它自己，中断派发照常进行，
// guest 的完成侧才有机会被释放。
// ===========================================================================
REXCVAR_DEFINE_BOOL(sa2_renut_display_drive_thread, true, "SA2/Renderer",
                    "[T2ROTF] v8.4-SPLIT: run the per-frame display commit driver on its own "
                    "dedicated host thread instead of the VSync dispatch thread. v8.3-FRAME let "
                    "the guest commit (sub_82880978 -> drain sub_82882200) block on the VSync "
                    "thread while waiting for a display completion that only that same thread's "
                    "ISR dispatch can deliver: the thread wedged at DRAIN-IN n=13, vblanks/irq "
                    "froze at 131/94 and the frame loop froze after 6 frames. With this on, the "
                    "VSync thread never enters guest code, so interrupt dispatch survives even if "
                    "the driver blocks. Off: v8.3 behaviour (driver on the VSync thread, A/B only).")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// ===========================================================================
// v8.6-FLIP：把「一次真实呈现完成」交回 guest 自己的记账函数 sub_822D9688
//
// 离线定论（T61）：guest 的显示记账负责人是它自己的两个函数，宿主过去一直在自己写
// 这些字段（违反既定约束，也是历次回绕/崩溃的来源）：
//   sub_822D9570(dev) —— 垂直同步处理（已由宿主 source=0 派发按 60 Hz 进入，实机日志
//                        vblanks/16548 增长为证）：*(dev+16548)++、排空换页环、
//                        每项 *(dev+16568)++，环项 surface==0 时把 *(*(dev+10900)+4) 清 0。
//   sub_822D9688(r3)  —— 呈现记账：*(dev+16564)++、把 (surface, 目标垂直同步号) 写入
//                        换页环 (dev+16572/16576/16704)；无新表面时 *(dev+16568)++
//                        且把 *(*(dev+10900)+4) 清 0。
// sub_822D9688 在本构建里零静态调用者（只有 T2ROTF_init.cpp 的注册表项与定义本身）：
// 它的调用者是 guest 自己的 D3D 换页路径，而那一路被宿主 Swap 钩子整个替换掉了。
// => 换页环恒空（实机 ringR16700 = ringW16704 = 0）、*(dev+16568) 冻结在 5、
//    *(*(dev+10900)+4) 永不清零 => guest 自己的在飞显示项退役链停摆
//    （base+288 恒 1 / base+284 恒 1 / sub_82881848 死等 => 黑屏）。
//
// v8.5 派发 source=1 解决不了：sub_822D47A0 的 r3==1 分支只做「条件调 *(blk+16) +
// 清 *(blk+0) 的当前 CPU 位」，而 *(blk+16) 在本移植里从未被写非 0 => 实际是空操作。
// 本开关按真实刷新节奏代客调用 guest 自己的 sub_822D9688，参数恒为 0
// （「本次呈现已完成、没有新画面要换」）：r29=(r3>>24)&0xF=0 => 目标垂直同步号 ==
// 当前值 => 走「不入环」分支 => *(dev+16568)++、surface==0 => *(*(dev+10900)+4)=0。
// 宿主自己不写任何字段：全部由 guest 代码在它自己的自旋锁 dev+16712 下完成。
// ===========================================================================
REXCVAR_DEFINE_BOOL(sa2_renut_display_present_guest, true, "SA2/Renderer",
                    "[T2ROTF] v8.6-FLIP: per-vblank call the guest's own present bookkeeper "
                    "sub_822D9688(r3=0) so the guest (not the host) advances dev+16564/16568, "
                    "fills the page-flip ring and clears the GPU identifier word "
                    "*(*(dev+10900)+4). That function has zero static callers in this build "
                    "because the guest's own swap path was replaced by the host Swap hook. "
                    "Off: no call.")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// ===========================================================================
// MMIO 窗口与寄存器（值与 SDK graphics_system.cpp:232-262 逐条一致）
// ===========================================================================
namespace {

constexpr uint32_t kBase = 0x7FC80000;
constexpr uint32_t kMascara = 0xFFFF0000;
constexpr uint32_t kTamano = 0x0000FFFF;
constexpr uint32_t kNumRegistros = 0x5003;  // RegisterFile::kRegisterCount

constexpr uint32_t kRegCpRbWptr = 0x01C5;                // CP_RB_WPTR
constexpr uint32_t kRegRbEdramTiming = 0x0F00;           // RB_EDRAM_TIMING
constexpr uint32_t kRegRbBcControl = 0x0F01;             // RB_BC_CONTROL
constexpr uint32_t kRegD1GrphPrimarySurface = 0x1844;    // AVIVO_D1GRPH_PRIMARY_SURFACE_ADDRESS
constexpr uint32_t kRegD1ModeVCounter = 0x194C;          // R500_D1MODE_V_COUNTER
constexpr uint32_t kRegD1ModeVblankVlineStatus = 0x1951; // 中断状态（vblank）
constexpr uint32_t kRegD1ModeViewportSize = 0x1961;      // AVIVO_D1MODE_VIEWPORT_SIZE

// --- v8.1-GUESTCB：代客派发用到的 guest 地址与区间 ------------------------
// guest 侧注册给 VdSetGraphicsInterruptCallback 的回调（sub_822E1690 注册的就是它）。
constexpr uint32_t kCallbackGuest = 0x822D47A0;
// VdGlobalDevice：两跳（slot → cell → device）。本运行时 KeGetCurrentProcessType()
// == X_PROCTYPE_USER(=1)，guest 读的是 4524(base)=0x820011AC（见 device.cpp:131-139）。
constexpr uint32_t kSlotOtro = 0x820011AC;
constexpr uint32_t kSlotProc2 = 0x820011B0;
// 本标题 guest 代码区间（0x82000000 起），用于判断“这指针像不像能干活的地址”。
constexpr uint32_t kCodigoMin = 0x82000000;
constexpr uint32_t kCodigoMax = 0x83000000;
// guest 对象/设备的合理区间（设备实机 = 0x40070600）。
constexpr uint32_t kObjetoMin = 0x40000000;
constexpr uint32_t kDispositivoMin = 0x10000000;
constexpr uint32_t kDispositivoMax = 0x90000000;

// 设备字段（device.cpp 的 kOffXxx 同源；这里只读，不写非守卫字段）。
constexpr uint32_t kOffDevColaCallback = 16544;   // ISR 尾部间接调用的 guest 回调
constexpr uint32_t kOffDevVblank = 16548;         // 显示 ISR 自增 1
constexpr uint32_t kOffDevVblankTime = 16552;     // mftb 时戳
constexpr uint32_t kOffDevCompletados = 16568;    // 显示 ISR 自增 2
constexpr uint32_t kOffDevAnilloIdx = 16700;      // 环读写序号
constexpr uint32_t kOffDevAnilloFin = 16704;      // 环末序号
constexpr uint32_t kOffDevFlags = 10940;          // 0x80 = 无硬件设备
constexpr uint32_t kOffDevFlags2 = 10941;         // 0x02 = fence 即时完成
constexpr uint32_t kOffDevFenceWord = 10896;      // 完成 fence 字
constexpr uint32_t kOffDevBloqueSwap = 10900;     // 换页暂存块（source=1 分支读它）
constexpr uint32_t kOffDevCompletados64 = 16564;  // 呈现计数（guest sub_822D9688 自增 1）
// [v8.6-FLIP] guest 自己的呈现记账：*(dev+16564)++ / 换页环入环 / *(dev+16568)++ /
// 清 *(*(dev+10900)+4)。本构建里零静态调用者 => 全库唯一入口就是宿主这次代客调用。
constexpr uint32_t kPresentarGuest = 0x822D9688;

// 显示事件/帧队列的 guest 全局（T44/T46 逐条定位，供卡点判读用）。
constexpr uint32_t kEvContador = 0x82DBB5A0;  // 显示侧等待的句柄字
constexpr uint32_t kEvFlags = 0x82DBB5C8;     // 帧管理器标志（实机 = 5）
constexpr uint32_t kEvEsperadores = 0x82DBB5D0;  // 等待计数（原子）
constexpr uint32_t kEvFlagD4 = 0x82DBB5D4;      // 就绪标志（整数）
constexpr uint32_t kEvObjeto = 0x82DBB5E0;      // 帧事件对象（实机 = F8000064）
constexpr uint32_t kEvFlagE4 = 0x82DBB5E4;      // 就绪标志（字节）
constexpr uint32_t kEvStatF8 = 0x82DBB5F8;      // 统计字段（sub_82880A48 收尾时写）
constexpr uint32_t kEvStat690 = 0x82DBB690;     // 统计字段递增
// 显示容器（device.cpp 的 kDisplayCtx / kDisplayQueueBase 同源）。
constexpr uint32_t kMostradorCont = 0x82E55E20;
constexpr uint32_t kMostradorBase = 0x82E55CE0;
constexpr uint32_t kOffContObjeto = 16;      // 容器 +16 → 通道标记所在对象
constexpr uint32_t kOffBasePosted = 284;     // 已投递未交付
constexpr uint32_t kOffBaseInflight = 288;   // 已交付未完成
constexpr uint32_t kOffObjMarkers = 272;     // 该对象 +272/276/280 = 三通道标记

// --- v8.3-FRAME：guest 侧入口与判活字段（只读） ---------------------------------
constexpr uint32_t kDispActual = 0x82DBB608;  // 当前显示对象（T33/T44 定位，sub_8287DD08 读它）
constexpr uint32_t kCommitGuest = 0x82880978; // guest 每帧显示提交（无参，纯全局状态驱动）
constexpr uint32_t kProducirGuest = 0x82882730;  // guest 原生生产者包装（v8.8 起默认在提交无产出时补调）
// [v8.8b] guest 自己的「当前呈现记录」解析器：在 *(0x82DBB5C4) 的 296 字节记录数组里
// 找 +0 状态 == 5 的那一条（找不到返回 0）。生产者的 r3 必须由它给出——sub_822B5608 的
// 第一条真实指令就是 lwz r10,64(r3) / stw r11,56(r3)，r3=0 会去读写 guest 0x40/0x38。
constexpr uint32_t kResolverPresente = 0x828823F8;
// [v8.8-LIVENESS] 待交付帧（base+284）连续多少个刷新不动，就认为 guest 消费者没在跑、仍然驱动。
constexpr int32_t kTicsEstancado = 45;

// --- v8.9-PRODUCE：只投递（不碰每帧提交）时要用的 guest 入口与只读字段 ------------
// 显示管家容器本身就是 sub_82881848 / sub_82881E00 的第一个实参（= kMostradorCont）。
constexpr uint32_t kPumpGuest = 0x82881E00;   // 就绪项投递：r3=容器, r4=idx/-1, r5=1 非阻塞
constexpr uint32_t kRegCuenta = 0x82DBB5C0;   // guest 呈现记录数（只读诊断）
constexpr uint32_t kRegArray = 0x82DBB5C4;    // guest 呈现记录阵列基址（只读诊断）
constexpr uint32_t kIdxGlobal = 0xFFFFFFFFu;  // sub_82881E00 的 r4：全局在飞项

// 单调毫秒时钟（只看差分，与墙钟/时区无关）；看门狗与耗时统计用。
uint64_t AhoraMs() {
  return uint64_t(std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count());
}

// --- v8.2-SHADOW：D3D 钩子观测到的设备地址 --------------------------------
// guest 线程（sub_822D4488 / sub_822D9570 的 REX_HOOK_RAW，r3 = D3D 设备）写，
// VSync 派发线程读。取值不变时不写，避免在热路反复写同一地址。
std::atomic<uint32_t> g_dev_hook{0};
std::atomic<bool> g_dev_hook_avisado{false};

// [v8.4-SPLIT] 只有独立驱动线程会把这一位设为 true；垂直同步线程永远是 false。
// DirigirFotograma 用它保证「进入 guest 显示代码」只发生在那一个线程上。
thread_local bool g_en_hilo_dirigido = false;

class GpuSignalSystem final : public rex::system::IGraphicsSystem {
 public:
  GpuSignalSystem() : registros_(kNumRegistros, 0) {}
  ~GpuSignalSystem() override { Shutdown(); }

  // --- IGraphicsSystem -----------------------------------------------------

  // 无呈现：只记下 app_context，provider/presenter 保持 null。
  X_STATUS SetupPresentation(rex::ui::WindowedAppContext* app_context) override {
    // 这条会在两处被调到（ReXApp::SetupPresentation 与 IGraphicsSystem::Setup），幂等。
    app_context_ = app_context;
    if (!presentacion_avisada_.exchange(true)) {
      REXLOG_ERROR("[video] T2ROTF GPSIG: 信号源接管（无呈现；presenter=null，画面由 Renut 出）");
    }
    return X_STATUS_SUCCESS;
  }

  X_STATUS SetupGuestGpu(rex::runtime::FunctionDispatcher* function_dispatcher,
                              rex::system::KernelState* kernel_state) override {
    REXLOG_ERROR("[video] T2ROTF BUILD v9.0-VIVO (display post channel pinned to the live display device)");
    REXLOG_ERROR(
        "[video] T2ROTF BUILD v9.0-VIVO detail: the gate sub_82881B88 runs with the RUNTIME display "
        "device object as its context (v8.9 log: r30=0x41770018, unchanged over 4e6 samples) and that "
        "object's +16 manager link reads 0, so the gate's readiness read *(*(r30+16)+272+4*slot) "
        "degenerated into *(0+272+4*slot) == 0 -- the gate was blind, delivered 0 times, and the drain "
        "while(284+288!=0) spun forever (T2DIAG DRAIN-IN n went past 680000). v9.0 pins every display "
        "post to that device's OWN channel (dev+24) and repairs its missing self-link "
        "(*(dev+16)=dev, done only through the RexGmShadowFor shadow alias), so the producer's "
        "++*(A+248)/push(A+8) and the gate's ready-read/pop land on one and the same object instead of "
        "drifting between the static manager 0x82E55E20 and the runtime device object; a wedge guard "
        "then lets the GUEST itself re-init the display pipeline (sub_82882628) when 284==0 && 288>0 && "
        "no arm marker becomes non-zero again. v9.0 keeps v8.9-PRODUCE, v8.7-IRQABI, v8.6-FLIP, "
        "v8.4-SPLIT, v8.3-FRAME, v8.2 shadow-safe reads and the yield semantics.");
    function_dispatcher_ = function_dispatcher;
    kernel_state_ = kernel_state;
    if (!function_dispatcher_ || !kernel_state_) {
      REXLOG_ERROR("[video] T2ROTF GPSIG: 拿不到 dispatcher / kernel_state，放弃");
      return X_STATUS_UNSUCCESSFUL;
    }
    memory_ = function_dispatcher_->memory();
    if (!memory_) {
      REXLOG_ERROR("[video] T2ROTF GPSIG: dispatcher 没有 memory，放弃");
      return X_STATUS_UNSUCCESSFUL;
    }
    if (!registrado_) {
      // 与 SDK graphics_system.cpp:142-147 同一窗口（mask/size 同值）。
      registrado_ = memory_->AddVirtualMappedRange(kBase, kMascara, kTamano, this, &LeerMmio,
                                                   &EscribirMmio);
      if (!registrado_) {
        REXLOG_ERROR("[video] T2ROTF GPSIG: 0x7FC80000 MMIO 窗口注册失败");
        return X_STATUS_UNSUCCESSFUL;
      }
      REXLOG_ERROR("[video] T2ROTF GPSIG: MMIO 窗口 0x7FC80000-0x7FC8FFFF 已注册（hostcb={}）",
                   REXCVAR_GET(sa2_renut_gpu_signal_hostcb));
    }
    ArrancarHilo();
    ArrancarHiloDir();  // [v8.4-SPLIT] 每帧提交驱动独立成线程
    return X_STATUS_SUCCESS;
  }

  bool has_presentation() const override { return false; }

  void SetInterruptCallback(uint32_t callback, uint32_t user_data) override {
    callback_datos_.store(user_data, std::memory_order_release);
    callback_.store(callback, std::memory_order_release);
    REXLOG_ERROR("[video] T2ROTF GPSIG: guest 注册图形中断回调 callback={:08X} user_data={:08X}",
                 callback, user_data);
  }

  void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) override {
    anillo_ptr_.store(ptr, std::memory_order_release);
    anillo_log2_.store(size_log2, std::memory_order_release);
    REXLOG_ERROR("[video] T2ROTF GPSIG: ring 初始化 ptr={:08X} size_log2={} ({} 字节)", ptr,
                 size_log2, uint32_t(1) << (size_log2 + 3));
  }

  void EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) override {
    rptr_addr_.store(ptr, std::memory_order_release);
    REXLOG_ERROR("[video] T2ROTF GPSIG: 读指针回写槽 {:08X} block_size_log2={}", ptr,
                 block_size_log2);
  }

  void Shutdown() override {
    const bool estaba_activo = activo_.exchange(false);
    activo_dir_.store(false, std::memory_order_release);
    if (estaba_activo && hilo_) {
      // 与 SDK graphics_system.cpp:186-190 同序：先置停止标志，再等线程退出。
      hilo_->Wait(0, 0, 0, nullptr);
      hilo_.reset();
      REXLOG_ERROR("[video] T2ROTF GPSIG: 垂直同步派发线程已停止（vblanks={} interrupts={}）",
                   vblanks_.load(std::memory_order_relaxed),
                   interrupciones_.load(std::memory_order_relaxed));
    }
    if (hilo_dir_) {
      // [v8.4-SPLIT] 驱动线程可能正卡在 guest 提交内部（那正是它存在的意义：卡也卡它自己）。
      // 这种情况下不等待、只在日志里点名，避免应用退出被它拖住；其余情况正常回收。
      if (dirigido_en_curso_.load(std::memory_order_acquire)) {
        REXLOG_ERROR(
            "[video] T2ROTF GPSIG: 每帧提交驱动线程仍卡在 guest 提交内（dirigidos={}/{}），"
            "跳过等待以免退出挂死",
            dirigidos_ok_.load(std::memory_order_relaxed),
            dirigidos_ini_.load(std::memory_order_relaxed));
      } else {
        hilo_dir_->Wait(0, 0, 0, nullptr);
        REXLOG_ERROR("[video] T2ROTF GPSIG: 每帧提交驱动线程已停止（ini={} ok={} fallos={}）",
                     dirigidos_ini_.load(std::memory_order_relaxed),
                     dirigidos_ok_.load(std::memory_order_relaxed),
                     dirigidos_fallos_.load(std::memory_order_relaxed));
      }
      hilo_dir_.reset();
    }
  }

 private:
  // --- guest 内存访读（宿主侧） --------------------------------------------
  // [v8.2-SHADOW] 唯一安全通道：先取该地址所在 chunk 的 shadow 别名，再按 guest
  // 字节序读写。绝不把 TranslateVirtual 的结果直接解引用 —— v8.1 实机就是死在
  // 窗口地址 0x820011AC 上（`LDR W1,[X2]`、X2=arena+0x820011AC、esr 0x92000006，
  // 最终落到 xmemory.cpp:571 "Unhandled guest access violation"）：本移植的 chunk
  // 在提交时并不映射进窗口视图，只有 guest 自己首次访问（RexGmFaultIn）才映射，
  // 宿主直读一个「已提交但未映射 / 尚未提交」的窗口页必然是 translation fault。
  // SDK 的 Switch 异常处理器正是用 RexGmShadowFor 代客读写；本模块从宿主线程访问，
  // 必须走同一条路。取不到 shadow 就跳过并计数 sinSombra：不直读、不直写、也不
  // 代客提交（提交会为根本不属于 guest 物理内存的地址造出假后端）。
  void* Sombra(uint32_t va) const {
    if (!memory_ || !va) {
      return nullptr;
    }
#if defined(__SWITCH__)
    const uint64_t ventana =
        reinterpret_cast<uint64_t>(memory_->TranslateVirtual(va & ~uint32_t(3)));
    return RexGmShadowFor(ventana);
#else
    return memory_->TranslateVirtual(va & ~uint32_t(3));
#endif
  }

  uint32_t Leer32(uint32_t va) const {
    void* p = Sombra(va);
    if (!p) {
      if (va) {
        sombra_nula_lectura_.fetch_add(1, std::memory_order_relaxed);
        if (!sombra_avisada_.exchange(true)) {
          REXLOG_ERROR(
              "[video] T2ROTF GPSIG: {:08X} 所在 chunk 无 shadow（未提交）→ 本次读跳过"
              "（绝不直读；sinSombra 计数）",
              va);
        }
      }
      return 0;
    }
    return rex::memory::load_and_swap<uint32_t>(p);
  }

  bool Escribir32(uint32_t va, uint32_t valor) {
    void* p = Sombra(va);
    if (!p) {
      if (va) {
        sombra_nula_escritura_.fetch_add(1, std::memory_order_relaxed);
      }
      return false;
    }
    rex::memory::store_and_swap<uint32_t>(p, valor);
    return true;
  }

  static bool PlausibleObjeto(uint32_t va) {
    return va && (va & 3u) == 0 && va >= kObjetoMin && va < kDispositivoMax;
  }

  // VdGlobalDevice 是两跳：slot → cell → device（device.cpp PublishGlobalDevice 同结构）。
  // [v8.2-SHADOW] 分层解析：cvar 强制 > 双槽（shadow 安全读，缺 shadow 直接跳过）>
  // D3D 钩子观测 r3（sa2_renut_gpu_signal_dev_hook）。两者都解析出来时以槽为准，
  // 并把两个值一起写进日志互证（不一致时明确标注）。
  uint32_t Dispositivo() {
    const int32_t forzado = REXCVAR_GET(sa2_renut_gpu_signal_cb_data);
    if (forzado > 0) {
      dispositivo_.store(uint32_t(forzado), std::memory_order_release);
      return uint32_t(forzado);
    }
    const uint32_t cache = dispositivo_.load(std::memory_order_acquire);
    if (cache) {
      return cache;
    }
    const uint32_t observado =
        REXCVAR_GET(sa2_renut_gpu_signal_dev_hook) ? g_dev_hook.load(std::memory_order_acquire) : 0;
    uint32_t desde_slot = 0;
    uint32_t slot_usado = 0;
    uint32_t celda_usada = 0;
    const uint32_t slots[2] = {kSlotOtro, kSlotProc2};
    for (uint32_t slot : slots) {
      if (!Sombra(slot)) {
        continue;  // 该槽所在 chunk 未提交（无 shadow 别名）：绝不直读
      }
      const uint32_t celda = Leer32(slot);
      if (!PlausibleObjeto(celda)) {
        continue;
      }
      const uint32_t dev = Leer32(celda);
      if (dev && (dev & 3u) == 0 && dev >= kDispositivoMin && dev < kDispositivoMax) {
        desde_slot = dev;
        slot_usado = slot;
        celda_usada = celda;
        break;
      }
    }
    uint32_t dev = 0;
    const char* origen = "无";
    const char* nota = "";
    if (desde_slot) {
      dev = desde_slot;
      origen = "槽";
      if (observado && observado != desde_slot) {
        nota = "（与钩子观测不一致，以槽为准）";
      }
    } else if (observado) {
      dev = observado;
      origen = "钩子";
    }
    if (!dev) {
      return 0;
    }
    dispositivo_.store(dev, std::memory_order_release);
    if (!dispositivo_avisado_.exchange(true)) {
      REXLOG_ERROR(
          "[video] T2ROTF GPSIG: 设备解析 来源={} slot={:08X} cell={:08X} dev={:08X} "
          "hookDev={:08X}{} sinSombra={}/{}",
          origen, slot_usado, celda_usada, dev, observado, nota,
          sombra_nula_lectura_.load(std::memory_order_relaxed),
          sombra_nula_escritura_.load(std::memory_order_relaxed));
    }
    return dev;
  }

  // 一次性守卫：把 ISR 会间接调用的那个字段暂时清零（非 guest 代码地址时），调用后还原。
  bool GuardarCampo(uint32_t campo, uint32_t* valor) {
    if (!REXCVAR_GET(sa2_renut_gpu_signal_cb_guard) || !campo) {
      return false;
    }
    const uint32_t actual = Leer32(campo);
    if (!actual || (actual >= kCodigoMin && actual < kCodigoMax)) {
      return false;
    }
    Escribir32(campo, 0);
    *valor = actual;
    if (!guardia_avisada_.exchange(true)) {
      REXLOG_ERROR("[video] T2ROTF GPSIG: {:08X}={:08X} 非 guest 代码地址 → 本次派发期间临时清零（守卫）",
                   campo, actual);
    }
    return true;
  }

  // --- MMIO ----------------------------------------------------------------

  static uint32_t LeerMmio(void* ppc_context, void* contexto, uint32_t direccion) {
    (void)ppc_context;
    return static_cast<GpuSignalSystem*>(contexto)->LeerRegistro(direccion);
  }

  static void EscribirMmio(void* ppc_context, void* contexto, uint32_t direccion, uint32_t valor) {
    (void)ppc_context;
    static_cast<GpuSignalSystem*>(contexto)->EscribirRegistro(direccion, valor);
  }

  uint32_t LeerRegistro(uint32_t direccion) {
    const uint32_t r = (direccion & 0xFFFF) / 4;
    lecturas_mmio_.fetch_add(1, std::memory_order_relaxed);
    switch (r) {
      case kRegRbEdramTiming:
        return 0x08100748;
      case kRegRbBcControl:
        return 0x0000200E;
      case kRegD1ModeVCounter: {
        rex::system::X_VIDEO_MODE modo;
        rex::kernel::xboxkrnl::VdQueryVideoMode(&modo);
        return std::min(uint32_t(modo.display_height), uint32_t(0x0FFF));
      }
      case kRegD1ModeVblankVlineStatus:
        // ISR 唯一的门：非 0 才允许显示 ISR 运行（SDK 同值，注释就是 "vblank"）。
        lecturas_estado_.fetch_add(1, std::memory_order_relaxed);
        return 1;
      case kRegD1ModeViewportSize: {
        rex::system::X_VIDEO_MODE modo;
        rex::kernel::xboxkrnl::VdQueryVideoMode(&modo);
        const uint32_t ancho = std::min(uint32_t(modo.display_width), uint32_t(0x0FFF));
        const uint32_t alto = std::min(uint32_t(modo.display_height), uint32_t(0x0FFF));
        return (ancho << 16) | alto;
      }
      default:
        break;
    }
    return (r < kNumRegistros) ? registros_[r] : 0;
  }

  void EscribirRegistro(uint32_t direccion, uint32_t valor) {
    const uint32_t r = (direccion & 0xFFFF) / 4;
    escrituras_mmio_.fetch_add(1, std::memory_order_relaxed);
    if (r == kRegCpRbWptr) {
      wptr_.store(valor, std::memory_order_release);
      if (escrituras_wptr_.fetch_add(1, std::memory_order_relaxed) < 8) {
        REXLOG_ERROR("[video] T2DIAG GPSIG wptr={} ({})", valor, escrituras_wptr_.load());
      }
      if (REXCVAR_GET(sa2_renut_gpu_rptr_sync)) {
        PublicarRptr(valor);
      }
    } else if (r == kRegD1GrphPrimarySurface) {
      // 显示 ISR 把主表面地址写回这里（SDK 也是直接吞掉，不产生副作用）。
      ++escrituras_superficie_;
    } else if (escrituras_mmio_.load(std::memory_order_relaxed) <= 64) {
      REXLOG_ERROR("[video] T2DIAG GPSIG reg {:04X} <= {:08X}", r, valor);
    }
    if (r < kNumRegistros) {
      registros_[r] = valor;
    }
  }

  void PublicarRptr(uint32_t valor) {
    const uint32_t addr = rptr_addr_.load(std::memory_order_acquire);
    if (!addr) {
      return;
    }
    // [v8.2-SHADOW] 物理窗口也走 shadow（TranslatePhysical 内部会掩 0x1FFFFFFF，
    // 故先把地址对齐到 4 字节再算；取不到 shadow 就跳过，不直写）。
    void* p = SombraFisica(addr);
    if (!p) {
      sombra_nula_escritura_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    rex::memory::store_and_swap<uint32_t>(p, valor);
    rptr_publicados_.fetch_add(1, std::memory_order_relaxed);
  }

  void* SombraFisica(uint32_t pa) const {
    if (!memory_ || !pa) {
      return nullptr;
    }
#if defined(__SWITCH__)
    const uint64_t ventana = reinterpret_cast<uint64_t>(
        memory_->TranslatePhysical<uint8_t*>(pa & ~uint32_t(3)));
    return RexGmShadowFor(ventana);
#else
    return memory_->TranslatePhysical<uint8_t*>(pa & ~uint32_t(3));
#endif
  }

  // --- 垂直同步派发线程 -----------------------------------------------------

  void ArrancarHilo() {
    if (activo_.exchange(true)) {
      return;
    }
    hilo_ = rex::system::object_ref<rex::system::XHostThread>(
        new rex::system::XHostThread(kernel_state_, 128 * 1024, 0, [this]() { return BucleVblank(); }));
    hilo_->set_name("T2ROTF VSync");
    hilo_->Create();
    REXLOG_ERROR("[video] T2ROTF GPSIG: 垂直同步派发线程已启动（cvar hz={}，cpu={}）",
                 REXCVAR_GET(sa2_renut_gpu_vblank_hz), REXCVAR_GET(sa2_renut_gpu_signal_cpu));
  }

  // [v8.4-SPLIT] 每帧提交驱动线程：唯一进入 guest 显示代码的宿主线程。
  // 它与垂直同步线程共用同一套「让位」语义（vivo_/vivo_tics_），但绝不派发任何中断，
  // 因此即使卡在 guest 提交内部也不会影响中断派发（这正是 v8.3 自锁的解药）。
  void ArrancarHiloDir() {
    if (!REXCVAR_GET(sa2_renut_display_drive_thread) || !REXCVAR_GET(sa2_renut_display_drive)) {
      return;
    }
    if (activo_dir_.exchange(true)) {
      return;
    }
    hilo_dir_ = rex::system::object_ref<rex::system::XHostThread>(
        new rex::system::XHostThread(kernel_state_, 128 * 1024, 0, [this]() { return BucleDirigido(); }));
    hilo_dir_->set_name("T2ROTF Dirigir");
    hilo_dir_->Create();
    REXLOG_ERROR(
        "[video] T2ROTF GPSIG: 每帧提交驱动线程已启动（独立于垂直同步派发线程；after_ms={} div={}）",
        REXCVAR_GET(sa2_renut_display_drive_after_ms), REXCVAR_GET(sa2_renut_display_drive_div));
  }

  // 只做「替 guest 走一次每帧提交」和只读判活，不派发中断、不写任何会计。
  int BucleDirigido() {
    g_en_hilo_dirigido = true;
    double hz = 0.0;
    const int32_t pedido = REXCVAR_GET(sa2_renut_gpu_vblank_hz);
    if (pedido > 0) {
      hz = double(pedido);
    } else {
      rex::system::X_VIDEO_MODE modo;
      rex::kernel::xboxkrnl::VdQueryVideoMode(&modo);
      hz = std::max(1.0, double(float(modo.refresh_rate)));
    }
    const auto intervalo = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(1.0 / hz));
    auto siguiente = std::chrono::steady_clock::now() + intervalo;
    const uint32_t cpu = uint32_t(std::max(0, REXCVAR_GET(sa2_renut_gpu_signal_cpu)));
    while (activo_dir_.load(std::memory_order_acquire)) {
      const auto ahora = std::chrono::steady_clock::now();
      if (ahora - siguiente > std::chrono::milliseconds(250)) {
        siguiente = ahora;
      }
      while (ahora >= siguiente && activo_dir_.load(std::memory_order_acquire)) {
        const uint64_t n = dirigidos_tics_.fetch_add(1, std::memory_order_relaxed) + 1;
        DirigirFotograma(cpu, n);
        siguiente += intervalo;
      }
      rex::thread::Sleep(std::chrono::milliseconds(1));
    }
    return 0;
  }

  // 与 SDK MarkVblank -> DispatchInterruptCallback({0, user_data}, 2) 同语义
  // （graphics_system.cpp:295-332）。source=0 垂直同步；source=1 提交完成。
  void Enviar(uint32_t callback, uint32_t fuente, uint32_t datos, uint32_t cpu) {
    auto* hilo = rex::system::XThread::GetCurrentThread();
    if (!hilo) {
      return;
    }
    hilo->SetActiveCpu(uint8_t(cpu));
    uint64_t argumentos[] = {fuente, datos};
    invocados_.fetch_add(1, std::memory_order_relaxed);
    function_dispatcher_->ExecuteInterrupt(hilo->thread_state(), callback, argumentos, 2);
    retornados_.fetch_add(1, std::memory_order_relaxed);
    interrupciones_.fetch_add(1, std::memory_order_relaxed);
  }

  void Interrupcion(uint32_t fuente, uint32_t cpu) {
    if (!function_dispatcher_) {
      return;
    }
    uint32_t callback = callback_.load(std::memory_order_acquire);
    uint32_t datos = callback_datos_.load(std::memory_order_acquire);
    if (!callback) {
      // [v8.1-GUESTCB] guest 自己的注册点（sub_822E1690 → VdSetGraphicsInterruptCallback
      // (0x822D47A0, dev)）被 Renut 的原生 D3D 设备创建路径旁路：SetInterruptCallback
      // 一次也不会被调到。宿主按同一语义代客派发：同一回调地址、同一设备、同一组参数。
      if (!REXCVAR_GET(sa2_renut_gpu_signal_hostcb)) {
        hostcb_skip_.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      datos = Dispositivo();
      if (!datos) {
        hostcb_sin_dev_.fetch_add(1, std::memory_order_relaxed);
        if (hostcb_sin_dev_.load(std::memory_order_relaxed) == 1) {
          REXLOG_ERROR("[video] T2ROTF GPSIG: 代客派发需要设备，但 slot {:08X}/{:08X} 都解析不出（稍后重试）",
                       kSlotOtro, kSlotProc2);
        }
        return;
      }
      callback = kCallbackGuest;
      hostcb_usados_.fetch_add(1, std::memory_order_relaxed);
      if (!hostcb_avisado_.exchange(true)) {
        REXLOG_ERROR("[video] T2ROTF GPSIG: 宿主代客派发 guest 图形中断回调 callback={:08X} dev={:08X}（guest 注册点被 Renut 原生设备路径旁路）",
                     callback, datos);
      }
    }
    // 守卫：显示 ISR 尾部会间接调用 *(dev+16544)（实机 = 0x82880A48，vblank 统计）。
    uint32_t guardado = 0;
    const bool guardo = GuardarCampo(datos + kOffDevColaCallback, &guardado);
    Enviar(callback, fuente, datos, cpu);
    if (guardo) {
      Escribir32(datos + kOffDevColaCallback, guardado);
    }
  }

  // source=1（提交完成）分支：guest ISR 会读 *(dev+10900)（换页暂存块）再调 *(blk+16)。
  // [v8.5-COMPLETE] 默认**开**：v8.4 日志已坐实这一支在本移植里一次都没派发过
  // （skipS1 严格等于 vblanks、s1=0、dev+16568 冻在 5），而它正是 guest 自己的 ISR 里
  // 唯一能「完成掉一个在飞显示项」的通路 —— 不派发，base+288 就永远回不到 0，
  // 排空 sub_82882200 的 while(284+288!=0) 永远转不出去。*(dev+10900)==0 时仍然不发。
  void Completado(uint32_t cpu) {
    if (!function_dispatcher_) {
      return;
    }
    if (!REXCVAR_GET(sa2_renut_gpu_signal_hostcb_s1)) {
      s1_skip_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    const uint32_t dev = Dispositivo();
    if (!dev) {
      s1_skip_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    const uint32_t blk = Leer32(dev + kOffDevBloqueSwap);
    // [v8.5-COMPLETE] 判据修正：实机 *(dev+10900)=0xBDF80080（D3D 驱动自己的 fence/
    // 暂存块，落在 GPU 物理窗口 0xB... 段），旧判据拿「guest 对象区间 0x40000000-
    // 0x90000000」去校验，必然把它判成「不是可用暂存块」而一条都不发 —— 这是 s1
    // 从未派发的第二个原因（第一个是默认关）。现在只要求：非零、4 字节对齐、落在可
    // 寻址的 32 位范围内。绝不写它，只读 + 派发。
    if (!blk || (blk & 3u) != 0 || blk > 0xFFFFFFF0u) {
      s1_sin_bloque_.fetch_add(1, std::memory_order_relaxed);
      if (s1_sin_bloque_.load(std::memory_order_relaxed) == 1) {
        REXLOG_ERROR("[video] T2ROTF GPSIG: source=1 暂不派发：dev+10900={:08X} 不是可用暂存块",
                     blk);
      }
      return;
    }
    // [v8.5-COMPLETE] 派发时机（只读判据，不写任何会计字段）：
    //   0 = 不派发；1 = 仅有在飞显示项时派发；2 = 每次刷新都派发（缺省，与硬件
    //   「一帧一次提交完成中断」同节奏）。在飞项只看 *(0x82E55CE0+284/+288)。
    const int32_t modo_s1 = REXCVAR_GET(sa2_renut_gpu_signal_hostcb_s1_mode);
    if (modo_s1 <= 0) {
      s1_skip_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    if (modo_s1 == 1) {
      const uint32_t pendiente =
          Leer32(kMostradorBase + kOffBasePosted) | Leer32(kMostradorBase + kOffBaseInflight);
      if (!pendiente) {
        s1_sin_pendiente_.fetch_add(1, std::memory_order_relaxed);
        return;
      }
    }
    const uint32_t interno = Leer32(blk + 16);
    uint32_t guardado = 0;
    const bool guardo = GuardarCampo(blk + 16, &guardado);
    const uint32_t registrado = callback_.load(std::memory_order_acquire);
    s1_usados_.fetch_add(1, std::memory_order_relaxed);
    if (!s1_avisado_.exchange(true)) {
      REXLOG_ERROR("[video] T2ROTF GPSIG: 宿主代客派发 source=1（提交完成）callback={:08X} dev={:08X} blk={:08X} blk16={:08X}",
                   registrado ? registrado : kCallbackGuest, dev, blk, interno);
    }
    Enviar(registrado ? registrado : kCallbackGuest, 1, dev, cpu);
    if (guardo) {
      Escribir32(blk + 16, guardado);
    }
  }

  // [v8.6-FLIP] 见文件头 v8.6-FLIP 说明：把「一次真实呈现完成」交回 guest 自己的
  // sub_822D9688(r3=0)，宿主不写任何 guest 字段。调用前用 shadow 安全读做一次
  // 「两跳设备槽 → 设备」的一致性核对（本运行时 KeGetCurrentProcessType()==USER
  // => guest 读 4524(base)=0x820011AC），对不上就本次跳过并只报一次，绝不猜着调。
  void PresentarGuest(uint32_t cpu) {
    if (!REXCVAR_GET(sa2_renut_display_present_guest) || !function_dispatcher_) {
      return;
    }
    static std::atomic<uint32_t> llamados{0};
    static std::atomic<uint32_t> saltados{0};
    static std::atomic<uint32_t> avisado{0};
    auto* hilo = rex::system::XThread::GetCurrentThread();
    if (!hilo) {
      return;
    }
    auto* ts = hilo->thread_state();
    if (!ts) {
      return;
    }
    const uint32_t dev = Dispositivo();
    const uint32_t celda = Leer32(kSlotOtro);
    const uint32_t dev2 = celda ? Leer32(celda) : 0;
    if (!dev || (celda != dev && dev2 != dev)) {
      saltados.fetch_add(1, std::memory_order_relaxed);
      if (!avisado.exchange(1)) {
        REXLOG_ERROR(
            "[video] T2ROTF GPSIG: v8.6 代客呈现记账暂不调用：dev={:08X} slot(4524)={:08X} "
            "*(slot)={:08X}（两跳对不上，本行之后不再报，等下一轮日志判读）",
            dev, celda, dev2);
      }
      return;
    }
    hilo->SetActiveCpu(uint8_t(cpu));
    auto* ctx = ts->context();
    ctx->r3.u64 = 0;  // r3=0：本次呈现已完成、没有新画面要换（见文件头）
    ctx->r4.u64 = 0;
    ctx->r5.u64 = 0;
    uint64_t argumentos[1] = {0};
    const uint64_t rc = function_dispatcher_->ExecuteTrap(ts, kPresentarGuest, argumentos, 0);
    if (rc == 0xDEADBABEull) {
      saltados.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    const uint32_t n = llamados.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 3 || (n % 300) == 0) {
      const uint32_t blk = Leer32(dev + kOffDevBloqueSwap);
      REXLOG_ERROR(
          "[video] T2DIAG PRESENT n={} dev={:08X} 16564={} 16568={} 16548={} ringW16704={} "
          "blk10900={:08X} blk4={:08X} saltados={}",
          n, dev, Leer32(dev + kOffDevCompletados64), Leer32(dev + kOffDevCompletados),
          Leer32(dev + kOffDevVblank), Leer32(dev + kOffDevAnilloFin), blk,
          blk ? Leer32(blk + 4) : 0, saltados.load(std::memory_order_relaxed));
    }
  }

  // --- v8.3-FRAME：宿主按真实刷新节奏驱动 guest 自己的每帧显示提交 -------------
  //
  // 判读依据（T52，v8.2-SHADOW 实机日志组）：缺页/崩溃全 0，代派发链四要素全增长
  // （宿主已按 60 Hz 把显示 ISR 送进 guest：vblanks/irq/mmio_r/hostcb 全涨），但 guest
  // 自己的每帧提交侧从未产出：*(0x82E55CE0+284)=0（无待交付）、*(+288)=1（一项在飞孤儿）、
  // inKick13232=0、ringR=ringW=0、dev+16568 冻结、T2DIAG SWAP 全程仅 1 次；显示线程停在
  // sub_82881848 的 NtWaitForSingleObjectEx(0xF8000064, alertable=1) 立刻返回的热自旋上
  // （20.78M 次 espera，lr=828818DC），GM 真阻塞在 KeWaitForSingleObject。
  // 即：显示 ISR 进来了，但「每帧把工作交给显示管家」的那次调用没有发生。
  //
  // 本函数让宿主在每次真实刷新替 guest 走一次它自己的提交入口 sub_82880978（无参、纯全局
  // 状态驱动）：不写任何会计字段、不碰门/泵热路、不改已通过 v8.2 验证的 shadow 安全访存；
  // 只是把「谁来回调每帧提交」这点补上。访客管线一旦自己动起来立即让位，不叠加提交。
  bool DirigirEntrada(uint32_t entrada, uint32_t cpu) {
    return DirigirEntradaCon(entrada, cpu, 0, nullptr);
  }

  // [v8.8b] 带入参/返回值的变体：r3 由调用方给定（默认 0），可回读 guest 的 r3 返回值。
  bool DirigirEntradaCon(uint32_t entrada, uint32_t cpu, uint32_t r3, uint64_t* rc_out) {
    return DirigirEntradaArgs(entrada, cpu, r3, 0, 0, rc_out);
  }

  // [v8.9-PRODUCE] 带 r4/r5 的变体：guest 自己的非阻塞泵 sub_82881E00 需要
  // r3 = 容器、r4 = -1（全局在飞项）、r5 = 1（非阻塞，绝不进阻塞等待）。
  bool DirigirEntradaArgs(uint32_t entrada, uint32_t cpu, uint32_t r3, uint32_t r4, uint32_t r5,
                          uint64_t* rc_out) {
    auto* hilo = rex::system::XThread::GetCurrentThread();
    if (!hilo) {
      return false;
    }
    auto* ts = hilo->thread_state();
    if (!ts) {
      return false;
    }
    hilo->SetActiveCpu(uint8_t(cpu));
    auto* ctx = ts->context();
    ctx->r3.u64 = r3;
    ctx->r4.u64 = r4;
    ctx->r5.u64 = r5;
    // ExecuteTrap：调用 guest 函数并在返回后完整还原宿主线程的 guest 上下文
    // （只保留该函数对 guest 内存的副作用），因此不会污染 VSync 宿主的寄存器状态。
    uint64_t argumentos[1] = {0};
    const uint64_t rc = function_dispatcher_->ExecuteTrap(ts, entrada, argumentos, 0);
    if (rc_out) {
      *rc_out = rc;
    }
    return rc != 0xDEADBABEull;
  }

  // [v8.9-PRODUCE] 只投递一帧，绝不碰每帧提交 sub_82880978：
  //   1) guest 自己的解析器 sub_828823F8 给出「当前呈现记录」（阵列 *(kRegArray) 共
  //      *(kRegCuenta) 条、每条 296 B、+0 == 5 为合法）；
  //   2) guest 自己的生产者 sub_82882730(记录) 完成「入队 + 自增 base+284 + 置位帧事件
  //      *(0x82DBB5E0)」—— 正是呈现线程（sub_82881848）在等的那个信号。
  // 只在「没有任何待交付帧」（base+284 == 0）时调用，因此永不重复投递。
  void ProducirFotograma(uint32_t cpu, uint64_t n) {
    const uint32_t cuenta = Leer32(kRegCuenta);
    const uint32_t array = Leer32(kRegArray);
    uint64_t rc = 0;
    fase_actual_.store(kFaseResolver, std::memory_order_relaxed);
    const bool res_ok = DirigirEntradaCon(kResolverPresente, cpu, 0, &rc);
    fase_actual_.store(kFaseNinguna, std::memory_order_relaxed);
    if (!res_ok || !PlausibleObjeto(uint32_t(rc))) {
      const uint64_t sin =
          dirigidos_produce_sin_dev_.fetch_add(1, std::memory_order_relaxed) + 1;
      if (sin <= 3) {
        REXLOG_ERROR(
            "[video] T2DIAG PRODUCE n={} rec={:08X} cuenta={} array={:08X} 无合法记录，跳过",
            n, uint32_t(rc), cuenta, array);
      }
      return;
    }
    fase_actual_.store(kFaseProductor, std::memory_order_relaxed);
    DirigirEntradaCon(kProducirGuest, cpu, uint32_t(rc), nullptr);
    fase_actual_.store(kFaseNinguna, std::memory_order_relaxed);
    const uint64_t hechos = dirigidos_produce_.fetch_add(1, std::memory_order_relaxed) + 1;
    if (hechos <= 4 || (hechos % 60) == 0) {
      REXLOG_ERROR("[video] T2DIAG PRODUCE n={} rec={:08X} cuenta={} array={:08X} 284={} 288={}",
                   n, uint32_t(rc), cuenta, array, Leer32(kMostradorBase + kOffBasePosted),
                   Leer32(kMostradorBase + kOffBaseInflight));
    }
  }

  void DirigirFotograma(uint32_t cpu, uint64_t n) {
    if (!REXCVAR_GET(sa2_renut_display_drive) || !function_dispatcher_) {
      return;
    }
    // [v8.4-SPLIT] 驱动默认只允许在独立驱动线程上运行：垂直同步线程绝不进入 guest 代码
    // （v8.3 就是在那里自锁的）。sa2_renut_display_drive_thread=false 时才允许回到旧行为。
    if (REXCVAR_GET(sa2_renut_display_drive_thread) && !EnHiloDirigido()) {
      // 非驱动线程（典型：垂直同步派发线程）一律不进入 guest 代码。
      return;
    }
    // [v8.9-PRODUCE] 看门狗已判定驱动线程卡死：不再叠加进入（证据由 DRIVE-WEDGE 快照给出）。
    if (dirigidos_muerto_.load(std::memory_order_acquire)) {
      return;
    }
    const int32_t hz = REXCVAR_GET(sa2_renut_gpu_vblank_hz);
    const int32_t retardo_ms = REXCVAR_GET(sa2_renut_display_drive_after_ms);
    const uint64_t espera_tics =
        (uint64_t(retardo_ms > 0 ? retardo_ms : 0) * uint64_t(hz > 0 ? hz : 60)) / 1000u;
    if (n < espera_tics) {
      return;
    }
    const int32_t div = REXCVAR_GET(sa2_renut_display_drive_div);
    if (div > 1 && (n % uint64_t(div)) != 0) {
      return;
    }
    // [v8.8-LIVENESS] 只读判活：判据必须只落在「宿主绝不伪造」的字段上。
    // v8.6-FLIP 的代客呈现记账（sub_822D9688）每刷新自增 dev+16568，而旧判活把 dev+16568
    // 也算作「guest 管线在动」=> 驱动线程每一轮都让位（v8.7 实机日志 DRIVE
    // drive=0 ok=0 produce=0 vivo=571），guest 侧从此拿不到被生产的帧（cola284 恒 0），
    // 呈现线程在帧事件 *(0x82DBB5E0)=F8000068 上死等（sub_82881848）、SWAP 全程仅 1 次
    // => 有声音无画面。现在只看 base+284：它由 guest 生产者自己自增（宿主从不写它），
    // 284 == 0 表示「没有任何待交付帧」——正是需要驱动的时刻；284 != 0 表示已有帧在等
    // guest 自己的消费者 —— 让位；若 284 长期卡住不动（消费者没在跑），仍然驱动去消化它。
    const uint32_t dev = Dispositivo();
    const uint32_t c284 = Leer32(kMostradorBase + kOffBasePosted);
    const uint32_t prev284 = vivo_284_.load(std::memory_order_relaxed);
    if (c284 != 0) {
      const int32_t tics = (c284 == prev284)
                               ? vivo_tics_estancado_.load(std::memory_order_relaxed) + 1
                               : 0;
      vivo_284_.store(c284, std::memory_order_relaxed);
      vivo_tics_estancado_.store(tics, std::memory_order_relaxed);
      if (tics < kTicsEstancado) {
        dirigidos_saltados_vivo_.fetch_add(1, std::memory_order_relaxed);
        return;
      }
      // 待交付帧长期不动：仍驱动，让 guest 自己的排空/交付把它消化掉。
      dirigidos_estancados_.fetch_add(1, std::memory_order_relaxed);
    } else {
      vivo_284_.store(0, std::memory_order_relaxed);
      vivo_tics_estancado_.store(0, std::memory_order_relaxed);
    }
    // [v8.8b] 显示对象 *(0x82DBB608) 只作诊断、不再拦驱动：实机 v8.7 日志里该字段
    // 74/75 次采样恒为 0（本移植下 guest 尚未登记当前显示对象），若仍按它提前返回，驱动会被
    // 永久拦下——正是本轮要修的症状。已逐行读过每帧提交入口 sub_82880978 的重编译体：它只调
    // sub_82B22628 / sub_8287EDC0 / sub_82ABB7D0 / sub_82880C30 并复位 0x82DBB6B4/B6B8，
    // 从不读 0x82DBB608，因此 obj==0 时驱动它是安全的；下游完成回调里对空显示对象的解引用
    // 早在 v7.5 起就由 sa2_renut_display_guard 拦下。
    const uint32_t obj = Leer32(kDispActual);
    const bool obj_ok = PlausibleObjeto(obj);
    if (!obj_ok) {
      dirigidos_sinobj_.fetch_add(1, std::memory_order_relaxed);
    }
    dirigidos_.fetch_add(1, std::memory_order_relaxed);
    // [v8.4-SPLIT] ini 先加、en_curso 置位，返回后再清：下一轮只要看到
    // ini > ok+fallos 就说明驱动线程当场卡在 guest 提交内部（v8.3 就是这样）。
    dirigidos_ini_.fetch_add(1, std::memory_order_relaxed);
    dirigido_en_curso_.store(true, std::memory_order_release);
    inicio_guest_ms_.store(AhoraMs(), std::memory_order_relaxed);
    const bool hacer_produce = REXCVAR_GET(sa2_renut_display_drive_produce);
    const bool hacer_commit = REXCVAR_GET(sa2_renut_display_drive_commit);
    // [v8.9-PRODUCE] 顺序反过来：先投递，再（可选）走每帧提交。
    // v8.8 实机证据坐实每帧提交 sub_82880978 一进去就把本线程永久留在 guest 显示回调里
    // （DRAIN-IN n=13 之后再无出栈；DRIVE 全程 dentro=1、drive=2 ok=1），而补产分支排在
    // 它后面 —— 于是 produce=0 prodSinDev=0，一次都没跑到，base+284 恒 0、呈现线程在
    // 帧事件上热轮询 978 万次、SWAP 全程 n=1（黑屏、有声）。现在补产独立且在前，提交
    // 默认关闭（sa2_renut_display_drive_commit=false）：宿主只把 guest 自己的帧投进
    // guest 自己的显示管家，「渲染 + 呈现」交回 guest 自己的线程。
    bool dirigido_ok = true;
    if (hacer_produce && Leer32(kMostradorBase + kOffBasePosted) == 0) {
      ProducirFotograma(cpu, n);
    }
    if (hacer_commit) {
      fase_actual_.store(kFaseCommit, std::memory_order_relaxed);
      dirigidos_commit_.fetch_add(1, std::memory_order_relaxed);
      dirigido_ok = DirigirEntrada(kCommitGuest, cpu);
      fase_actual_.store(kFaseNinguna, std::memory_order_relaxed);
    }
    // [v8.9-PRODUCE] 可选的有界非阻塞交接：只有已投递帧在飞（284|288 != 0）且开关 > 0 时，
    // 才最多调 pump_max 次 guest 自己的泵（r5=1 非阻塞，绝不进阻塞等待）。默认 0：交接留给
    // guest 自己的呈现线程，宿主不再替它把显示回调叫起来（v8.8 的教训）。
    const int32_t pump_max = REXCVAR_GET(sa2_renut_display_drive_pump_max);
    if (pump_max > 0) {
      int32_t bombeados = 0;
      while (bombeados < pump_max &&
             (Leer32(kMostradorBase + kOffBasePosted) |
              Leer32(kMostradorBase + kOffBaseInflight)) != 0) {
        fase_actual_.store(kFaseBomba, std::memory_order_relaxed);
        DirigirEntradaArgs(kPumpGuest, cpu, kMostradorCont, kIdxGlobal, 1, nullptr);
        fase_actual_.store(kFaseNinguna, std::memory_order_relaxed);
        dirigidos_bombeos_.fetch_add(1, std::memory_order_relaxed);
        ++bombeados;
      }
    }
    ultimo_guest_ms_.store(int64_t(AhoraMs() - inicio_guest_ms_.load(std::memory_order_relaxed)),
                           std::memory_order_relaxed);
    dirigido_en_curso_.store(false, std::memory_order_release);
    if (!dirigido_ok) {
      dirigidos_fallos_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
    dirigidos_ok_.fetch_add(1, std::memory_order_relaxed);
    // 只读跟进：本次驱动后待交付/在飞/完成计数是否真的动了（供下一轮判读）。
    if (dirigidos_ok_.load(std::memory_order_relaxed) <= 4 ||
        (dirigidos_ok_.load(std::memory_order_relaxed) % 60) == 0) {
      REXLOG_ERROR(
          "[video] T2DIAG DRIVE n={} obj={:08X} 284 {}->{} 288={} 16568={} 16548={} "
          "saltos_vivo={} estancados={}",
          n, obj, c284, Leer32(kMostradorBase + kOffBasePosted),
          Leer32(kMostradorBase + kOffBaseInflight),
          dev ? Leer32(dev + kOffDevCompletados) : 0, dev ? Leer32(dev + kOffDevVblank) : 0,
          dirigidos_saltados_vivo_.load(std::memory_order_relaxed),
          dirigidos_estancados_.load(std::memory_order_relaxed));
    }
    // [v8.9-PRODUCE] 补产已前移到每帧提交调用之前（见本函数开头的 ProducirFotograma）：
    // v8.8 把它放在提交之后，而提交永不返回，于是它一次都没跑到（produce=0 prodSinDev=0）。
  }

  int BucleVblank() {
    double hz = 0.0;
    const int32_t pedido = REXCVAR_GET(sa2_renut_gpu_vblank_hz);
    if (pedido > 0) {
      hz = double(pedido);
    } else {
      rex::system::X_VIDEO_MODE modo;
      rex::kernel::xboxkrnl::VdQueryVideoMode(&modo);
      hz = std::max(1.0, double(float(modo.refresh_rate)));
    }
    const auto intervalo = std::chrono::duration_cast<std::chrono::steady_clock::duration>(
        std::chrono::duration<double>(1.0 / hz));
    auto siguiente = std::chrono::steady_clock::now() + intervalo;
    const uint32_t cpu = uint32_t(std::max(0, REXCVAR_GET(sa2_renut_gpu_signal_cpu)));
    auto ultimo_informe = std::chrono::steady_clock::now();
    Informe();  // 立刻打一行，短会话的日志里也留得下基线
    while (activo_.load(std::memory_order_acquire)) {
      const auto ahora = std::chrono::steady_clock::now();
      // 长停顿（加载、调试）之后不补播几百个 vblank。
      if (ahora - siguiente > std::chrono::milliseconds(250)) {
        siguiente = ahora;
      }
      while (ahora >= siguiente) {
        vblanks_.fetch_add(1, std::memory_order_relaxed);
        Interrupcion(0, cpu);
        Completado(cpu);
        // [v8.6-FLIP] 每个刷新周期代客调用 guest 自己的呈现记账（宿主不写任何字段）。
        PresentarGuest(cpu);
        // [v8.4-SPLIT] 默认不在本线程驱动 guest 提交：本线程只派发中断+报告。旧行为
        // （驱动就在本线程）只在 sa2_renut_display_drive_thread=false 时保留，供 A/B 复现
        // v8.3 的自锁。DirigirFotograma 内部还有一道 EnHiloDirigido 兜底。
        if (!REXCVAR_GET(sa2_renut_display_drive_thread)) {
          DirigirFotograma(cpu, vblanks_.load(std::memory_order_relaxed));
        }
        siguiente += intervalo;
      }
      if (REXCVAR_GET(sa2_renut_gpu_signal_diag) &&
          (ahora - ultimo_informe) >= std::chrono::seconds(2)) {
        ultimo_informe = ahora;
        Informe();
      }
      rex::thread::Sleep(std::chrono::milliseconds(1));
    }
    return 0;
  }

  void Informe() {
    REXLOG_ERROR(
        "[video] T2DIAG GPSIG vblanks={} irq={} cb={:08X} ud={:08X} r1951={} mmio_r={} mmio_w={} "
        "wptr={}({}) rptr_pub={} sup1844={} hz={} hostcb={}/{} skip={} s1={} noblk={} skipS1={} "
        "inv={} ret={}",
        vblanks_.load(std::memory_order_relaxed), interrupciones_.load(std::memory_order_relaxed),
        callback_.load(std::memory_order_relaxed), callback_datos_.load(std::memory_order_relaxed),
        lecturas_estado_.load(std::memory_order_relaxed), lecturas_mmio_.load(std::memory_order_relaxed),
        escrituras_mmio_.load(std::memory_order_relaxed), wptr_.load(std::memory_order_relaxed),
        escrituras_wptr_.load(std::memory_order_relaxed),
        rptr_publicados_.load(std::memory_order_relaxed),
        escrituras_superficie_.load(std::memory_order_relaxed), REXCVAR_GET(sa2_renut_gpu_vblank_hz),
        hostcb_usados_.load(std::memory_order_relaxed), hostcb_sin_dev_.load(std::memory_order_relaxed),
        hostcb_skip_.load(std::memory_order_relaxed), s1_usados_.load(std::memory_order_relaxed),
        s1_sin_bloque_.load(std::memory_order_relaxed), s1_skip_.load(std::memory_order_relaxed),
        invocados_.load(std::memory_order_relaxed), retornados_.load(std::memory_order_relaxed));
    InformeDispositivo();
    InformeSombra();
    InformeDirigido();
  }

  // 相位名（只在 DRIVE-WEDGE 快照里用；与 fase_actual_ 的取值一一对应）。
  static const char* NombreFase(uint32_t fase) {
    switch (fase) {
      case kFaseResolver:
        return "resolver sub_828823F8";
      case kFaseProductor:
        return "producer sub_82882730";
      case kFaseCommit:
        return "commit sub_82880978";
      case kFaseBomba:
        return "pump sub_82881E00";
      default:
        return "none";
    }
  }

  // 第四行：v8.3-FRAME 的每帧驱动计数与只读跟进（每 2 s）。
  void InformeDirigido() {
    // [v8.9-PRODUCE] 看门狗：驱动线程停在 guest 代码里超过阈值 => 一次性全量快照 + 永久停用。
    // v8.8 的教训是「卡死是静默的」：DRIVE 只给一个 dentro=1，没人知道卡在哪一段。现在相位
    // （解析器/生产者/提交/泵）与设备状态一起落盘，下一轮判读不再靠猜。
    const int32_t limite_wd = REXCVAR_GET(sa2_renut_display_drive_watchdog_ms);
    if (limite_wd > 0 && dirigido_en_curso_.load(std::memory_order_acquire) &&
        !dirigidos_muerto_.load(std::memory_order_acquire)) {
      const uint64_t inicio = inicio_guest_ms_.load(std::memory_order_relaxed);
      const int64_t transcurrido = inicio ? int64_t(AhoraMs() - inicio) : 0;
      if (inicio && transcurrido >= int64_t(limite_wd)) {
        dirigidos_muerto_.store(true, std::memory_order_release);
        ultimo_wedge_ms_.store(transcurrido, std::memory_order_relaxed);
        const uint32_t dev_wd = Dispositivo();
        const uint32_t blk_wd = dev_wd ? Leer32(dev_wd + kOffDevBloqueSwap) : 0;
        const uint32_t fase_wd = fase_actual_.load(std::memory_order_relaxed);
        REXLOG_ERROR(
            "[video] T2DIAG DRIVE-WEDGE fase={} ({}) transcurrido={}ms drive={} ok={} ini={} "
            "284={} 288={} 16548={} 16568={} 16564={} ringR16700={} ringW16704={} 16552={:08X} "
            "blk10900={:08X} blk0={:08X} blk4={:08X} ev={:08X} obj={:08X} dev={:08X}",
            fase_wd, NombreFase(fase_wd), transcurrido,
            dirigidos_.load(std::memory_order_relaxed),
            dirigidos_ok_.load(std::memory_order_relaxed),
            dirigidos_ini_.load(std::memory_order_relaxed),
            Leer32(kMostradorBase + kOffBasePosted), Leer32(kMostradorBase + kOffBaseInflight),
            dev_wd ? Leer32(dev_wd + kOffDevVblank) : 0,
            dev_wd ? Leer32(dev_wd + kOffDevCompletados) : 0,
            dev_wd ? Leer32(dev_wd + kOffDevCompletados64) : 0,
            dev_wd ? Leer32(dev_wd + kOffDevAnilloIdx) : 0,
            dev_wd ? Leer32(dev_wd + kOffDevAnilloFin) : 0,
            dev_wd ? Leer32(dev_wd + kOffDevVblankTime) : 0, blk_wd, blk_wd ? Leer32(blk_wd) : 0,
            blk_wd ? Leer32(blk_wd + 4) : 0, Leer32(kEvObjeto), Leer32(kDispActual), dev_wd);
      }
    }
    const uint32_t dev = Dispositivo();
    REXLOG_ERROR(
        "[video] T2DIAG DRIVE drive={} ok={} ini={} dentro={} noObj={} fallos={} vivo={} produce={} "
        "prodSinDev={} estanc={} commit={} pump={} bombeos={} muerto={} wedge_ms={} fase={} "
        "t_guest={} "
        "16548={} 16568={} 284={} 288={} obj={:08X} ev={:08X} mmio_w={}",
        dirigidos_.load(std::memory_order_relaxed), dirigidos_ok_.load(std::memory_order_relaxed),
        dirigidos_ini_.load(std::memory_order_relaxed),
        dirigido_en_curso_.load(std::memory_order_relaxed) ? 1 : 0,
        dirigidos_sinobj_.load(std::memory_order_relaxed),
        dirigidos_fallos_.load(std::memory_order_relaxed),
        dirigidos_saltados_vivo_.load(std::memory_order_relaxed),
        dirigidos_produce_.load(std::memory_order_relaxed),
        dirigidos_produce_sin_dev_.load(std::memory_order_relaxed),
        dirigidos_estancados_.load(std::memory_order_relaxed),
        dirigidos_commit_.load(std::memory_order_relaxed),
        REXCVAR_GET(sa2_renut_display_drive_pump_max),
        dirigidos_bombeos_.load(std::memory_order_relaxed),
        dirigidos_muerto_.load(std::memory_order_relaxed) ? 1 : 0,
        ultimo_wedge_ms_.load(std::memory_order_relaxed),
        fase_actual_.load(std::memory_order_relaxed),
        ultimo_guest_ms_.load(std::memory_order_relaxed),
        dev ? Leer32(dev + kOffDevVblank) : 0, dev ? Leer32(dev + kOffDevCompletados) : 0,
        Leer32(kMostradorBase + kOffBasePosted), Leer32(kMostradorBase + kOffBaseInflight),
        Leer32(kDispActual), Leer32(kEvObjeto),
        escrituras_mmio_.load(std::memory_order_relaxed));
  }

  // 第三行：v8.2-SHADOW 的访存与设备来源（每 2 s，只读）。
  void InformeSombra() {
    REXLOG_ERROR(
        "[video] T2DIAG GPSIG3 hookDev={:08X} sinSombraL={} sinSombraW={} sinSombraAvisada={}",
        g_dev_hook.load(std::memory_order_relaxed),
        sombra_nula_lectura_.load(std::memory_order_relaxed),
        sombra_nula_escritura_.load(std::memory_order_relaxed),
        sombra_avisada_.load(std::memory_order_relaxed) ? 1 : 0);
  }

  // 第二行：设备 + 显示事件/帧队列的状态。卡点判读全靠这一行（只读，不写任何会计）。
  void InformeDispositivo() {
    const uint32_t dev = dispositivo_.load(std::memory_order_relaxed);
    if (!dev || (dev & 3u) != 0 || dev < kDispositivoMin || dev >= kDispositivoMax) {
      REXLOG_ERROR("[video] T2DIAG GPSIG2 dev={:08X}（未解析）slotA={:08X} slotB={:08X} cont={:08X}",
                   dev, Leer32(kSlotOtro), Leer32(kSlotProc2), Leer32(kMostradorCont));
      return;
    }
    const uint32_t blk = Leer32(dev + kOffDevBloqueSwap);
    const uint32_t cont = Leer32(kMostradorCont);
    const uint32_t obj = Leer32(cont + kOffContObjeto);
    const uint32_t m272 = PlausibleObjeto(obj) ? Leer32(obj + kOffObjMarkers) : 0;
    const uint32_t m276 = PlausibleObjeto(obj) ? Leer32(obj + kOffObjMarkers + 4) : 0;
    const uint32_t m280 = PlausibleObjeto(obj) ? Leer32(obj + kOffObjMarkers + 8) : 0;
    // [v8.5-COMPLETE] 把「门（sub_82881B88）就绪判据的全部输入」只读暴露出来，下一轮
    // 日志就能直接指出到底缺哪一项（全部经 shadow 安全读，绝不直读直写）：
    //   ptr272 = *(0x82E55CE0+272) 槽表基址；A<slot> = 表[slot*32+24]（已投递）、
    //   C<slot> = 表[slot*32+28]（未交付）—— Phase A 判据是 A+C > 0 才算该槽就绪；
    //   c16 = *(0x82E55CE0+16)（Phase B 读它 +272+4*slot 作 slot0 的就绪来源）；
    //   dv10904 = *(dev+10904)（ISR 用 KeAcquireSpinLock 清位的每 CPU 在飞掩码）；
    //   blk0/blk4 = 暂存块前 8 字节（在飞位图；全 0 = 没有待消费的完成记录）。
    const uint32_t ptr272 = Leer32(kMostradorBase + kOffObjMarkers);
    const uint32_t c16 = Leer32(kMostradorBase + kOffContObjeto);
    auto entrada = [this, ptr272](uint32_t slot, uint32_t off) -> uint32_t {
      return ptr272 ? Leer32(ptr272 + slot * 32u + off) : 0;
    };
    REXLOG_ERROR(
        "[video] T2DIAG GPSIG2 dev={:08X} cb16544={:08X} v16548={} v16552={:08X} v16568={} "
        "a16700={} a16704={} blk10900={:08X} blk16={:08X} f10940={:02X} f10941={:02X} "
        "f10896={:08X} evA0={:08X} evE0={:08X} flagC8={:08X} wD0={} fD4={} fE4={} st5F8={:08X} "
        "st690={} cont={:08X} obj={:08X} cola284={} cola288={} m272={} m276={} m280={} "
        "ptr272={:08X} c16={:08X} A0={} C0={} A1={} C1={} A2={} C2={} dv10904={:08X} "
        "blk0={:08X} blk4={:08X}",
        dev, Leer32(dev + kOffDevColaCallback), Leer32(dev + kOffDevVblank),
        Leer32(dev + kOffDevVblankTime), Leer32(dev + kOffDevCompletados),
        Leer32(dev + kOffDevAnilloIdx), Leer32(dev + kOffDevAnilloFin), blk,
        blk ? Leer32(blk + 16) : 0, Leer32(dev + kOffDevFlags) & 0xFFu,
        Leer32(dev + kOffDevFlags2) & 0xFFu, Leer32(dev + kOffDevFenceWord),
        Leer32(kEvContador), Leer32(kEvObjeto), Leer32(kEvFlags), Leer32(kEvEsperadores),
        Leer32(kEvFlagD4), Leer32(kEvFlagE4), Leer32(kEvStatF8), Leer32(kEvStat690), cont, obj,
        Leer32(kMostradorBase + kOffBasePosted),
        Leer32(kMostradorBase + kOffBaseInflight), m272, m276, m280, ptr272, c16,
        entrada(0, 24), entrada(0, 28), entrada(1, 24), entrada(1, 28), entrada(2, 24),
        entrada(2, 28), Leer32(dev + 10904), blk ? Leer32(blk) : 0, blk ? Leer32(blk + 4) : 0);
  }

  // [v8.9-PRODUCE] 驱动线程当前停在哪个「宿主自调用」的 guest 阶段（看门狗快照用）。
  static constexpr uint32_t kFaseNinguna = 0;
  static constexpr uint32_t kFaseResolver = 1;   // sub_828823F8：取当前呈现记录
  static constexpr uint32_t kFaseProductor = 2;  // sub_82882730：入队 + base+284++ + 置事件
  static constexpr uint32_t kFaseCommit = 3;     // sub_82880978：guest 每帧提交（默认关）
  static constexpr uint32_t kFaseBomba = 4;      // sub_82881E00：非阻塞泵（默认关）

  // --- 状态 ---------------------------------------------------------------
  rex::ui::WindowedAppContext* app_context_ = nullptr;
  rex::runtime::FunctionDispatcher* function_dispatcher_ = nullptr;
  rex::system::KernelState* kernel_state_ = nullptr;
  rex::memory::Memory* memory_ = nullptr;

  std::vector<uint32_t> registros_;
  std::atomic<bool> presentacion_avisada_{false};
  std::atomic<bool> registrado_{false};
  std::atomic<bool> activo_{false};
  rex::system::object_ref<rex::system::XHostThread> hilo_;

  std::atomic<uint32_t> callback_{0};
  std::atomic<uint32_t> callback_datos_{0};
  std::atomic<uint32_t> rptr_addr_{0};
  std::atomic<uint32_t> anillo_ptr_{0};
  std::atomic<uint32_t> anillo_log2_{0};
  std::atomic<uint32_t> wptr_{0};

  std::atomic<uint64_t> vblanks_{0};
  std::atomic<uint64_t> interrupciones_{0};
  std::atomic<uint64_t> lecturas_mmio_{0};
  std::atomic<uint64_t> escrituras_mmio_{0};
  std::atomic<uint64_t> lecturas_estado_{0};
  std::atomic<uint64_t> escrituras_wptr_{0};
  std::atomic<uint64_t> rptr_publicados_{0};
  std::atomic<uint64_t> escrituras_superficie_{0};  // guest 线程写，诊断线程读

  // v8.1-GUESTCB
  std::atomic<uint32_t> dispositivo_{0};      // 代客派发用的 guest 设备（解析一次后缓存）
  std::atomic<bool> hostcb_avisado_{false};   // 一次性日志：开始代客派发
  std::atomic<bool> s1_avisado_{false};       // 一次性日志：开始 source=1 派发
  std::atomic<bool> guardia_avisada_{false};  // 一次性日志：守卫真的动过手
  std::atomic<uint64_t> hostcb_usados_{0};    // 代客派发成功次数
  std::atomic<uint64_t> hostcb_skip_{0};      // hostcb 关：跳过次数
  std::atomic<uint64_t> hostcb_sin_dev_{0};   // 设备还没解析出来：跳过次数
  std::atomic<uint64_t> s1_usados_{0};        // source=1 实际派发次数
  std::atomic<uint64_t> s1_skip_{0};          // source=1 关：跳过次数
  std::atomic<uint64_t> s1_sin_bloque_{0};    // 无暂存块（或地址不可用）：跳过次数
  std::atomic<uint64_t> s1_sin_pendiente_{0}; // [v8.5] mode=1 且无在飞显示项：跳过次数
  std::atomic<uint64_t> s1_blk_invalidos_{0}; // [v8.5] 暂存块地址判据拒绝次数
  std::atomic<uint64_t> invocados_{0};        // 进入 guest 回调次数
  std::atomic<uint64_t> retornados_{0};       // 从 guest 回调返回次数（< invocados 即卡在 ISR 内）

  // [v8.4-SPLIT] 当前线程是否就是独立驱动线程（thread_local，唯一写入点 BucleDirigido）。
  bool EnHiloDirigido() const { return g_en_hilo_dirigido; }

  // v8.3-FRAME / v8.4-SPLIT：每帧提交驱动的计数与判活快照（只读，不含任何 guest 会计写）
  std::atomic<uint64_t> dirigidos_{0};            // 真正驱动 guest 提交的刷新次数
  std::atomic<uint64_t> dirigidos_ini_{0};        // 进入 guest 提交的次数（> ok+fallos 即卡死）
  std::atomic<bool> dirigido_en_curso_{false};    // 驱动线程是否正停在 guest 提交内部
  std::atomic<uint64_t> dirigidos_tics_{0};       // 驱动线程自己的刷新计数
  std::atomic<bool> activo_dir_{false};           // 驱动线程运行标志
  rex::system::object_ref<rex::system::XHostThread> hilo_dir_;  // 独立驱动线程
  std::atomic<uint64_t> dirigidos_ok_{0};         // 驱动调用返回成功（函数表命中）
  std::atomic<uint64_t> dirigidos_fallos_{0};     // 驱动调用失败（不在函数表）
  std::atomic<uint64_t> dirigidos_sinobj_{0};     // 当前显示对象不可用，跳过
  std::atomic<uint64_t> dirigidos_saltados_vivo_{0};   // 管线在动，让位跳过
  std::atomic<uint64_t> dirigidos_produce_{0};    // 备选生产者驱动次数
  std::atomic<uint64_t> dirigidos_produce_sin_dev_{0};  // [v8.8b] 解析不到合法呈现记录，跳过补产
  std::atomic<bool> vivo_{false};                 // 访客管线近期是否在动
  std::atomic<int32_t> vivo_tics_{0};             // 连续多少次刷新没动（>=3 恢复驱动）
  std::atomic<uint32_t> vivo_16568_{0};           // [v8.8 起仅留档，判活不再用它] dev+16568 快照
  std::atomic<uint32_t> vivo_284_{0};             // 上一次的 base+284 快照（v8.8：唯一判活字段）
  std::atomic<int32_t> vivo_tics_estancado_{0};   // [v8.8] base+284 连续未变化的刷新数
  std::atomic<uint64_t> dirigidos_estancados_{0}; // [v8.8] 待交付帧卡住仍驱动的次数
  // [v8.9-PRODUCE] 投递/相位/看门狗状态（全为只读计账，不含任何 guest 会计写）
  std::atomic<uint32_t> fase_actual_{0};              // 驱动线程当前所在的 guest 阶段
  std::atomic<uint64_t> inicio_guest_ms_{0};          // 本轮进入 guest 的起始时刻（ms）
  std::atomic<int64_t> ultimo_guest_ms_{0};           // 上一轮在 guest 内停留的毫秒数
  std::atomic<uint64_t> dirigidos_commit_{0};         // 真正进入每帧提交 sub_82880978 的次数
  std::atomic<uint64_t> dirigidos_bombeos_{0};        // 非阻塞泵 sub_82881E00 调用次数
  std::atomic<bool> dirigidos_muerto_{false};         // 看门狗判定驱动线程卡死（永久停用驱动）
  std::atomic<int64_t> ultimo_wedge_ms_{0};           // 判定卡死时已停留的毫秒数

  // v8.2-SHADOW（mutable：Leer32/Escribir32 是 const 成员，但要在里面计数/落日志）
  mutable std::atomic<uint64_t> sombra_nula_lectura_{0};    // 读：chunk 未提交、无 shadow
  mutable std::atomic<uint64_t> sombra_nula_escritura_{0};  // 写：同上而跳过
  mutable std::atomic<bool> sombra_avisada_{false};         // 一次性日志：首次跳过读
  mutable std::atomic<bool> dispositivo_avisado_{false};    // 一次性日志：设备解析来源
};

}  // namespace

bool Wanted() { return REXCVAR_GET(sa2_renut_gpu_signal); }

std::unique_ptr<rex::system::IGraphicsSystem> Create() {
  if (!Wanted()) {
    return nullptr;
  }
  return std::make_unique<GpuSignalSystem>();
}

// [v8.2-SHADOW] guest 线程（D3D 钩子 r3）观测到设备地址后上报，供 VSync 派发线程在
// 「双槽解析不出」时作为第三层来源使用。取值变化才写；此处不打印（打印在 GPSIG 行）。
void NotificarDispositivo(uint32_t dispositivo) {
  if (!dispositivo || (dispositivo & 3u) != 0 || dispositivo < kDispositivoMin ||
      dispositivo >= kDispositivoMax) {
    return;
  }
  if (g_dev_hook.load(std::memory_order_relaxed) == dispositivo) {
    return;  // 与上次同值：热路上不重复写
  }
  g_dev_hook.store(dispositivo, std::memory_order_release);
  if (!g_dev_hook_avisado.exchange(true)) {
    REXLOG_ERROR("[video] T2ROTF GPSIG: D3D 钩子观测到设备 {:08X}（作为双槽解析的兜底来源）",
                 dispositivo);
  }
}

}  // namespace t2rotf::gpu_signal