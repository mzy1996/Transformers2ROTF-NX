// T2ROTF - native WMV video decoding hooks (state==7 playback pump)
//
// Replaces the recompiled WVC1 per-macroblock decode (which spins at ~99 % CPU
// on Switch at 0 FPS) with FFmpeg's VC-1 decoder. The game reads ASF files
// through the SDK VFS; we open the same path with PeliculaWmv and decode one
// FFmpeg frame per playback-pump pass.
//
// State==7 playback chain (verified in T2ROTF_recomp.18.cpp):
//   dispatcher sub_827983A8 (24.cpp:81468) installs [ctx+15808] = the pump
//   sub_827EDEE8 while state==7 && [ctx+20904]==0. One pump pass decodes a
//   whole frame (row loop r26 vs [ctx+140], column loop vs [ctx+136]):
//     Prepare sub_8279C5B8            (18.cpp:86411)
//     per-MB  sub_827F7B28  header    (18.cpp:86901 call site)
//     per-MB  sub_827FA2E8  decode    (18.cpp:86927)  <- FFmpeg decode point
//     per-MB  sub_827E8EB8 / sub_827EBC00  motion comp (18.cpp:87165/87193)
//     rows    sub_827E6F68            (18.cpp:87129)
//     frame   sub_827E7AF0  present   (18.cpp:86721 frame-end, keep original)
//   after the pump the dispatcher memcpys the decode planes [3756/3760/3764]
//   into the display planes [3744/3748/3752] (24.cpp:82262-82296).
//
// To keep the game's own state machine intact we only replace the decode cost:
//  - the first sub_827FA2E8 of a pump pass decodes one FFmpeg frame and writes
//    both plane sets;
//  - the per-MB decode functions become cheap no-ops while a movie is active.

#include "t2rotf_video_wmv3.h"

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

// T2ROTF (2026-10-25): Renut/deko3d 侧的让出。Renut 的 D3D 钩子在游戏自己的地址上取代
// guest 实现，但 sub_822D13F0 / sub_822D4488 / sub_822D4A08 三个 guest 入口已经被本文件
// 的 REX_HOOK_RAW 占用（一个 guest 入口只能有一个定义），所以 Renut 侧以
// renut_native_<T2 地址> 提供实现，由本文件在打完诊断后让出（见文件末尾的三个让出点）。
#include "renut_engine/gpu/banjo_renut_select.h"
#include "renut_engine/gpu/banjo_shared_hooks.h"
// [v8.2-SHADOW] D3D 钩子观测到的设备地址 → 上报给显示信号模块（兜底来源）。
#include "t2rotf_gpu_signal.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <atomic>
#include <thread>
#include <pthread.h>  // [T24] Switch 上 std::thread::detach() 会抛异常（见 banjo_renut_tuning.h），看门狗用 pthread


namespace rex::kernel::xboxkrnl {
std::string NfsmwUltimoWmvLeido();  // SDK xboxkrnl_io.cpp: last .wmv movie the game read
}  // namespace rex::kernel::xboxkrnl

namespace t2rotf::video_nativo {

// The game's hooks: declared here and implemented below (REX_HOOK_RAW overrides
// the weak thunk the codegen emits for the same name; calling the strong
// original is done through the matching __imp__ symbol, never the weak thunk).
REX_EXTERN(__imp__sub_8279C5B8);
REX_EXTERN(__imp__sub_827F7B28);
REX_EXTERN(__imp__sub_827FA2E8);
REX_EXTERN(__imp__sub_827EBC00);
REX_EXTERN(__imp__sub_827E8EB8);
REX_EXTERN(__imp__sub_827E6F68);
REX_EXTERN(__imp__sub_827E7AF0);
REX_EXTERN(__imp__sub_827EDEE8);
REX_EXTERN(__imp__sub_82850830);

// T2DIAG (runtime evidence round): the points that are hot in the reference
// player but sample 0.0 % in the port, plus the guest thread-lifecycle entries.
// All of them are hooked read-only (log + run the original).
REX_EXTERN(__imp__sub_824F5578);  // XapiThreadStartup trampoline (guest thread entry)
REX_EXTERN(__imp__sub_824F0658);  // worker gate helper, first call in sub_82745140
REX_EXTERN(__imp__sub_824F3580);  // guest wait-with-timeout wrapper (xenia: top-1)
REX_EXTERN(__imp__sub_824F34F0);  // guest wait-with-timeout wrapper (xenia: top-2)
REX_EXTERN(__imp__sub_82745140);  // media worker thread body (xenia start_address)
REX_EXTERN(__imp__sub_8277C218);  // media worker entry (xenia movie window)
REX_EXTERN(__imp__sub_8277C248);  // media worker entry (xenia movie window)
REX_EXTERN(__imp__sub_8277C278);  // media worker entry (xenia movie window)
REX_EXTERN(__imp__sub_8277C2A8);  // media worker entry (xenia movie window)
REX_EXTERN(__imp__sub_82793DC8);  // WMV fourcc/codec dispatch (installs 827937E8)
REX_EXTERN(__imp__sub_827937E8);  // hot movie chain outer (packet decode, 18.7 s xenia)
REX_EXTERN(__imp__sub_8279BFC0);  // hot movie chain: 15-byte bitstream read
REX_EXTERN(__imp__sub_827983A8);  // state==7 dispatcher (installs [ctx+15808])
REX_EXTERN(__imp__sub_827CE598);  // the installed pump itself (17.4 s xenia)
REX_EXTERN(__imp__sub_827CADC0);  // pump's end step: receives the display planes

namespace {

// PeliculaWmv holds the open ASF file + FFmpeg decoder.
std::mutex g_mutex;
std::unique_ptr<t2rotf::video_wmv3::PeliculaWmv> g_pelicula;
std::string g_ruta;
uint32_t g_ctx = 0;

// Per pump-pass flag: the first sub_827FA2E8 of each pass decodes exactly one
// FFmpeg frame; all remaining per-MB calls in the same pass are no-ops.
// Reset in sub_8279C5B8 (Prepare), which the pump calls once at the top of
// every pass on the same thread as the MB loop.
bool g_cuadro_por_paso = false;
uint64_t g_pasos_pump = 0;
uint64_t g_fotogramas_ffmpeg = 0;
uint32_t g_estado_log = ~0u;
bool g_log_f7 = false;   // sub_827F7B28 first entry
bool g_log_fa = false;   // sub_827FA2E8 first entry
bool g_log_ebc = false;  // sub_827EBC00 first entry
bool g_log_e8 = false;   // sub_827E8EB8 first entry
bool g_log_e6 = false;   // sub_827E6F68 first entry
uint64_t g_e7_count = 0; // sub_827E7AF0 call counter

// T3 (real playback path). Runtime evidence shows the pump the guest really
// runs is sub_827CE598 (installed into [ctx+15808] by the dispatcher) and that
// neither sub_827EDEE8 nor any of the per-MB functions above is ever entered
// on the console, nor in the reference player (xenia profile: 827EDEE8 /
// 827FA2E8 / 827F7B28 absent, 827CE598 17.4 s). FFmpeg is therefore driven
// from sub_827CE598 and the decoded frame is written into the display planes
// the present path reads, instead of waiting for a decode point that never
// runs.
t2rotf::video_wmv3::Fotograma g_ultimo_f{};  // last frame (valid until next Siguiente)
bool g_ultimo_valido = false;
bool g_fin = false;                          // container reached its end
std::chrono::steady_clock::time_point g_ultimo_avance{};
uint64_t g_bomba_ticks = 0;   // sub_827CE598 entries
uint64_t g_inyectados = 0;    // FFmpeg frames written into the planes
uint64_t g_pres_ticks = 0;    // sub_827CADC0 entries
uint64_t g_verde_restantes = 0;
bool g_verde_decidido = false;

// ===========================================================================
// [T24] 显示会计 / 停摆取证用的宿主全局。
//
// 看门狗线程要读 guest 字段，SDK 只给「arena base + 偏移」的裸算术（renut 侧
// 的 GuestPtr 会再走一次 kernel memory 缓存），所以把钩子拿到的 base 存下来。
// 计数一律用 atomic：泵在影片线程、看门狗在自起线程、Swap 钩子在游戏线程。
// ===========================================================================
std::atomic<uint64_t> g_base_host{0};      // guest arena 基址（所有 guest 线程共用）
std::atomic<uint32_t> g_ctx_visto{0};      // 最近一次影片对象的 guest 地址
std::atomic<uint32_t> g_dev_visto{0};      // 最近一次 D3D 设备的 guest 地址（校验后写入）
// ===========================================================================
// [v8.7-IRQABI] 显示中断回调的 guest ABI 修正 + 转储地址守卫。
//
// 实证（2026-10-31 实机两次运行，rex_crash(37) + T2ROTF_001(70)/002(51)）：
//   guest 回调 sub_822D47A0 的 ABI 是 `(r3 = 来源, r4 = 设备)`：
//     `irq sub_822D47A0 n=2 r3=00000001 r4=40070600`（来源=1 提交完成）
//     `irq sub_822D47A0 n=3 r3=00000000 r4=40070600`（来源=0 垂直同步）
//   与重编译调用点 `lis/addi 0x822D47A0、mr r4,r31(dev)` 互证。
//   旧钩子把 r3 当设备写进 g_dev_visto，于是 v8.5/v8.6 每垂直同步派发一次 source=1
//   就把 g_dev_visto 毒化成 1；泵/看门狗的全量转储随后拿它当 guest 地址裸读
//   （1 + 10896 = 0x2A91 未映射）→ xmemory 致命路径 → 进程启动即退出、零加载。
//   崩溃现场逐项对得上：x19=1（dev）、x8=0x2A90、x9=0x2A94、x0=0x2A91(=1+0x2A90)、
//   x1=0x2A95(=1+0x2A94)、pc=VolcarGuest+0x2c4、指令 0xb8604b54 = ldr w20,[x26,w0,uxtw]。
// 因此两条硬约束：设备只能取 r4 且必须通过校验；转储里任何 guest 地址先校验再读。
// ===========================================================================
std::atomic<uint64_t> g_perro_saltos{0};    // 转储因地址不健全而跳过的次数
std::atomic<uint64_t> g_irq_dev_avisado{0}; // 中断钩子里设备候选不健全的告警计数

// guest 地址健全性判据：非零、4 字节对齐、落在可寻址的 32 位范围内且高于低位区。
// 实机设备 = 0x40070600、驱动 fence/暂存块 = 0xBDF80000 通过；来源值 0/1 被拒。
inline bool EsDireccionDispositivo(uint32_t a) {
  return a != 0 && (a & 3u) == 0 && a >= 0x10000000u && a <= 0xFFFFFFF0u;
}
std::atomic<uint64_t> g_perro_arrancado{0};
std::atomic<uint64_t> g_pump_entrada{0};   // sub_827CE598 进入
std::atomic<uint64_t> g_pump_salida{0};    // sub_827CE598 返回
std::atomic<uint64_t> g_pre1{0};           // sub_82B506F0 进入（唯一调用者就是泵）
std::atomic<uint64_t> g_pre2{0};           // sub_82B8E128 进入（泵前置调用 2）
std::atomic<uint64_t> g_post470{0};        // sub_82B8E470 进入（泵尾部调用）
std::atomic<uint64_t> g_prep{0};           // sub_8279C5B8 进入（Prepare）
std::atomic<uint64_t> g_cadena937{0};      // sub_827937E8 进入
std::atomic<uint64_t> g_cadena9bf{0};      // sub_8279BFC0 进入
std::atomic<uint64_t> g_despacho{0};       // sub_827983A8 进入
std::atomic<uint64_t> g_yield_total{0};    // sub_824F0620 进入（全程序唯一 yield 点）
// [T29] 显示/呈现驱动链只读探针：用来在实机判读「呈现驱动链到底跑到哪一步」。
std::atomic<uint64_t> g_dtick{0};        // 每帧 tick sub_82ABB7D0 进入
std::atomic<uint64_t> g_ddrain{0};       // 排空 sub_82882200 进入（tick 里那次）
std::atomic<uint64_t> g_ddrain_sucio{0}; // 排空进入时判据 posted+inflight != 0（帧循环会空转的那次）
std::atomic<uint64_t> g_dpump_in{0};     // 显示泵 sub_82881E00 进入
std::atomic<uint64_t> g_dpump_ok{0};     // 显示泵返回非 0（真的交付了一件）
std::atomic<uint64_t> g_dpump_out{0};    // 显示泵返回
std::atomic<uint64_t> g_ddeliv{0};       // 交付 sub_82881930 进入（标记--、+284--、条目+24--、条目+28++、+288++）
std::atomic<uint64_t> g_dcmd_in{0};      // 显示命令 sub_82881838 进入（回调对象虚方法）
std::atomic<uint64_t> g_dcmd_out{0};     // 显示命令返回（in > out ⇒ 回调阻塞）
std::atomic<uint64_t> g_dprod{0};        // 生产者 sub_822B5608 进入（通道 arm / 入队）
std::atomic<uint32_t> g_yield_lr[1024]{};  // yield LR 直方图：槽内最后一次 LR
std::atomic<uint32_t> g_yield_lr_n[1024]{};  // yield LR 直方图：槽内命中次数
std::atomic<int64_t> g_turno_ms[8]{};      // 每槽上一次打印的单调毫秒

// Guest memory is big-endian (PowerPC), like the game's recompiled accessors.
uint32_t Leer32(uint8_t* base, uint32_t addr) {
  return __builtin_bswap32(*reinterpret_cast<const uint32_t*>(base + addr));
}

// 大端写入（Leer32 的镜像）：用来把 guest 侧的「本帧可呈现」标志置上。
void Escribir32(uint8_t* base, uint32_t addr, uint32_t valor) {
  *reinterpret_cast<uint32_t*>(base + addr) = __builtin_bswap32(valor);
}

// ===========================================================================
// [T24] 看门狗：泵/钩子的一次调用就把它拉起来，之后每 2 s 无条件打一行全量
// 计数 + guest 字段。用来判读「etapa3 之后到底停在哪一步、哪些计数不再动」。
// 打印本身受 Turno(槽, 间隔) 限频，避免刷屏（看门狗有自己的节律，不共享槽）。
// ===========================================================================
int64_t MsMono() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

bool Turno(int ranura, int64_t intervalo_ms = 2000) {
  if (ranura < 0 || ranura >= 8) {
    return false;
  }
  const int64_t ahora = MsMono();
  int64_t previo = g_turno_ms[ranura].load(std::memory_order_relaxed);
  if (ahora - previo < intervalo_ms) {
    return false;
  }
  if (!g_turno_ms[ranura].compare_exchange_strong(previo, ahora, std::memory_order_relaxed)) {
    return false;  // 另一个线程刚打过这一槽
  }
  return true;
}

void ArrancarPerro();                    // 幂等：只起一个看门狗线程
void VolcarGuest(const char* etiqueta);  // 全量转储（看门狗与泵共用）


// ===========================================================================
// [T2MIRROR] Vulkan 侧读指针镜像 —— 2026-10-24 冻结移除（全面转 deko3d 后无意义）
// 原修复把宿主已消费的环内 dword 位置写进 guest 门 sub_822D4488 轮询的那个字
// （[dev+10896]+0），以绕过「宿主发布地址 ≠ guest 轮询地址」。它属于伪造 guest
// 进度、且从未通过链接验证；reNut（Direct3D 级接管，无 PM4 ring、无读指针回写）
// 路线下已不需要，故连同类声明与写指针函数一并删除。断点结论留在工程 Markdown。
// ===========================================================================

// Copy FFmpeg YUV 4:2:0 planes into the game's state==7 video object planes.
// The video object stores two sets of plane bases:
//   decode set:  [ctx+3756] Y / [ctx+3760] U / [ctx+3764] V
//   display set: [ctx+3744] Y / [ctx+3748] U / [ctx+3752] V
// The dispatcher memcpys decode -> display right after the pump, and the pump
// math reads both sets, so both are filled. Pitches/heights come from
// [ctx+204]/[ctx+212] (Y) and [ctx+208]/[ctx+216] (chroma).
void CopiarPlanos(uint8_t* base, uint32_t ctx_addr, const t2rotf::video_wmv3::Fotograma& f) {
  const uint32_t dec_y = Leer32(base, ctx_addr + 3756);
  const uint32_t dec_u = Leer32(base, ctx_addr + 3760);
  const uint32_t dec_v = Leer32(base, ctx_addr + 3764);
  const uint32_t dis_y = Leer32(base, ctx_addr + 3744);
  const uint32_t dis_u = Leer32(base, ctx_addr + 3748);
  const uint32_t dis_v = Leer32(base, ctx_addr + 3752);
  const uint32_t y_pitch = Leer32(base, ctx_addr + 204);
  const uint32_t c_pitch = Leer32(base, ctx_addr + 208);
  const uint32_t y_altura = Leer32(base, ctx_addr + 212);
  const uint32_t c_altura = Leer32(base, ctx_addr + 216);

  const int ancho_c = (f.ancho + 1) / 2;
  const int alto_c = (f.alto + 1) / 2;
  const int yw = std::min<int>(f.ancho, static_cast<int>(y_pitch));
  const int yh = std::min<int>(f.alto, static_cast<int>(y_altura));
  const int cw = std::min<int>(ancho_c, static_cast<int>(c_pitch));
  const int ch = std::min<int>(alto_c, static_cast<int>(c_altura));

  for (int y = 0; y < yh; ++y) {
    std::memcpy(base + dec_y + int64_t(y) * y_pitch, f.planos[0] + int64_t(y) * f.pasos[0], size_t(yw));
    std::memcpy(base + dis_y + int64_t(y) * y_pitch, f.planos[0] + int64_t(y) * f.pasos[0], size_t(yw));
  }
  for (int y = 0; y < ch; ++y) {
    std::memcpy(base + dec_u + int64_t(y) * c_pitch, f.planos[1] + int64_t(y) * f.pasos[1], size_t(cw));
    std::memcpy(base + dis_u + int64_t(y) * c_pitch, f.planos[1] + int64_t(y) * f.pasos[1], size_t(cw));
    std::memcpy(base + dec_v + int64_t(y) * c_pitch, f.planos[2] + int64_t(y) * f.pasos[2], size_t(cw));
    std::memcpy(base + dis_v + int64_t(y) * c_pitch, f.planos[2] + int64_t(y) * f.pasos[2], size_t(cw));
  }
}

// Same copy into an explicit plane set (guest addresses + pitches). Returns the
// number of bytes written.
uint32_t CopiarEn(uint8_t* base, uint32_t py, uint32_t pu, uint32_t pv, uint32_t y_pitch,
                  uint32_t c_pitch, uint32_t y_altura, uint32_t c_altura,
                  const t2rotf::video_wmv3::Fotograma& f) {
  if (!py || !pu || !pv || !y_pitch || !c_pitch) {
    return 0;
  }
  const int ancho_c = (f.ancho + 1) / 2;
  const int alto_c = (f.alto + 1) / 2;
  const int yw = std::min<int>(f.ancho, static_cast<int>(y_pitch));
  const int yh = std::min<int>(f.alto, static_cast<int>(y_altura));
  const int cw = std::min<int>(ancho_c, static_cast<int>(c_pitch));
  const int ch = std::min<int>(alto_c, static_cast<int>(c_altura));
  if (yw <= 0 || yh <= 0 || cw <= 0 || ch <= 0) {
    return 0;
  }
  for (int y = 0; y < yh; ++y) {
    std::memcpy(base + py + int64_t(y) * y_pitch, f.planos[0] + int64_t(y) * f.pasos[0], size_t(yw));
  }
  for (int y = 0; y < ch; ++y) {
    std::memcpy(base + pu + int64_t(y) * c_pitch, f.planos[1] + int64_t(y) * f.pasos[1], size_t(cw));
    std::memcpy(base + pv + int64_t(y) * c_pitch, f.planos[2] + int64_t(y) * f.pasos[2], size_t(cw));
  }
  return uint32_t(yh) * y_pitch + 2u * uint32_t(ch) * c_pitch;
}

// The pump hands [ctx+3744]+[ctx+220] (Y) and [ctx+3748]+[ctx+224] /
// [ctx+3752]+[ctx+224] (chroma) to the present step, so the frame offset is
// written too, in addition to the plane bases CopiarPlanos already fills. The
// offset is validated against the distance between consecutive plane bases, so
// the write can never leave the plane buffers.
void CopiarDisplayDesplazado(uint8_t* base, uint32_t ctx_addr,
                             const t2rotf::video_wmv3::Fotograma& f) {
  const uint32_t dis_y = Leer32(base, ctx_addr + 3744);
  const uint32_t dis_u = Leer32(base, ctx_addr + 3748);
  const uint32_t dis_v = Leer32(base, ctx_addr + 3752);
  const uint32_t y_pitch = Leer32(base, ctx_addr + 204);
  const uint32_t c_pitch = Leer32(base, ctx_addr + 208);
  const uint32_t y_altura = Leer32(base, ctx_addr + 212);
  const uint32_t c_altura = Leer32(base, ctx_addr + 216);
  const uint32_t off_y = Leer32(base, ctx_addr + 220);
  const uint32_t off_c = Leer32(base, ctx_addr + 224);
  if (!off_y && !off_c) {
    return;
  }
  const uint32_t disp_y = (dis_u > dis_y) ? (dis_u - dis_y) : 0;  // Y plane size
  const uint32_t disp_c = (dis_v > dis_u) ? (dis_v - dis_u) : 0;  // U plane size
  const int yh = std::min<int>(f.alto, static_cast<int>(y_altura));
  const int ch = std::min<int>((f.alto + 1) / 2, static_cast<int>(c_altura));
  const uint32_t bytes_y = uint32_t(std::max(0, yh)) * y_pitch;
  const uint32_t bytes_c = uint32_t(std::max(0, ch)) * c_pitch;
  if (off_y && (off_y + bytes_y) <= disp_y) {
    CopiarEn(base, dis_y + off_y, dis_u + off_c, dis_v + off_c, y_pitch, c_pitch, y_altura, c_altura, f);
  } else if (off_c && (off_c + bytes_c) <= disp_c) {
    CopiarEn(base, dis_y, dis_u + off_c, dis_v + off_c, y_pitch, c_pitch, y_altura, c_altura, f);
  }
}

// Solid-colour test frame: written once at the start of the first movie so the
// log tells apart "the present path never reaches the screen" from "the movie
// content never reaches the planes".
void PintarVerde(uint8_t* base, uint32_t ctx_addr) {
  const uint32_t dis_y = Leer32(base, ctx_addr + 3744);
  const uint32_t dis_u = Leer32(base, ctx_addr + 3748);
  const uint32_t dis_v = Leer32(base, ctx_addr + 3752);
  const uint32_t y_pitch = Leer32(base, ctx_addr + 204);
  const uint32_t c_pitch = Leer32(base, ctx_addr + 208);
  const uint32_t y_altura = Leer32(base, ctx_addr + 212);
  const uint32_t c_altura = Leer32(base, ctx_addr + 216);
  const int yw = std::min<int>(1280, static_cast<int>(y_pitch));
  const int yh = std::min<int>(720, static_cast<int>(y_altura));
  const int cw = std::min<int>(640, static_cast<int>(c_pitch));
  const int ch = std::min<int>(360, static_cast<int>(c_altura));
  if (!dis_y || !dis_u || !dis_v || yw <= 0 || yh <= 0 || cw <= 0 || ch <= 0) {
    return;
  }
  for (int y = 0; y < yh; ++y) {
    std::memset(base + dis_y + int64_t(y) * y_pitch, 145, size_t(yw));
  }
  for (int y = 0; y < ch; ++y) {
    std::memset(base + dis_u + int64_t(y) * c_pitch, 54, size_t(cw));
    std::memset(base + dis_v + int64_t(y) * c_pitch, 34, size_t(cw));
  }
}

// True while a movie is open for this video object (same-thread check).
bool PeliculaActiva(uint32_t ctx_addr) {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_pelicula && g_ctx == ctx_addr;
}

// T2 diagnostic: dump every runtime field the state==7 movie pump branches on
// (macroblock dims, pitches/heights, plane pointers, state, pump gate and the
// installed pump pointer). Behavior identical, logging only.
void DumpCtx(uint8_t* base, uint32_t ctx_addr, const char* motivo) {
  REXLOG_ERROR(
      "[video] T2ROTF nativo: DUMP {} ctx={:08X} mbcols[136]={} mbrows[140]={} "
      "pitchY[204]={} pitchC[208]={} altY[212]={} altC[216]={} "
      "st15504={} gate20904={} v20908={} pump15808={:08X} "
      "disY={:08X} disU={:08X} disV={:08X} decY={:08X} decU={:08X} decV={:08X} "
      "g3396={} g3188={:08X} g3388={} v22088={} v3948={}",
      motivo, ctx_addr, Leer32(base, ctx_addr + 136), Leer32(base, ctx_addr + 140),
      Leer32(base, ctx_addr + 204), Leer32(base, ctx_addr + 208),
      Leer32(base, ctx_addr + 212), Leer32(base, ctx_addr + 216),
      Leer32(base, ctx_addr + 15504), Leer32(base, ctx_addr + 20904),
      Leer32(base, ctx_addr + 20908), Leer32(base, ctx_addr + 15808),
      Leer32(base, ctx_addr + 3744), Leer32(base, ctx_addr + 3748),
      Leer32(base, ctx_addr + 3752), Leer32(base, ctx_addr + 3756),
      Leer32(base, ctx_addr + 3760), Leer32(base, ctx_addr + 3764),
      Leer32(base, ctx_addr + 3396), Leer32(base, ctx_addr + 3188),
      Leer32(base, ctx_addr + 3388), Leer32(base, ctx_addr + 22088),
      Leer32(base, ctx_addr + 3948));
}

uint32_t g_pump_count = 0;  // sub_827EDEE8 entry count
uint32_t g_copy_count = 0;  // sub_82850830 entry count

// One pass of the pump the guest really runs (sub_827CE598). Called at the top
// of every pump pass on the movie thread: advance FFmpeg at most once per pass
// (the pass itself runs at the movie tick rate, 33 ms in the logs) and leave
// the frame in the video object's planes. The frame written by Siguiente stays
// valid until the next call, so the same one is repainted on passes that do not
// advance, and it is also the frame the present step of the same pass sees.
void BombaTick(uint8_t* base, uint32_t ctx_addr) {
  ++g_bomba_ticks;

  static uint64_t n_ctx_distinto = 0;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (!g_pelicula) {
    return;
  }
  if (g_ctx != ctx_addr) {
    if (n_ctx_distinto < 5) {
      ++n_ctx_distinto;
      REXLOG_WARN("[video] T2ROTF nativo: BOMBA ctx distinto bomba={:08X} pelicula={:08X}",
                  ctx_addr, g_ctx);
    }
    return;
  }

  // [T22] 呈现门。反汇编 sub_827CE598（本轮重读，0x827CE598..0x827CE71F）：
  //   bl 82B506F0 -> bl 82B8E128 -> etapa1/2/3（任一返回非 0 就直接跳到 827CE6F8 收尾）
  //   -> lwz r11,3916(r31); cmpi 0; beq 0x827CE6A8     <- 3916 == 0 时整段呈现被跳过
  //   -> lwz r11,15504(r31); cmpi 7 -> 827CC058 / 827CC6E0
  //   -> 取 [ctx+3744/3748/3752] 三张显示平面 + [ctx+220/224] 两个偏移 -> bl 827CADC0（呈现步）
  //   -> 827CE6A8 处按 3916/14856/15228 三条件写 [ctx+15592]（"本帧已呈现"）
  // 3916/3912/3924 由码流头部解析写入；本移植用 FFmpeg 重做了解码，guest 自己的
  // 解码链不再跑，所以替它把「本帧可呈现」置上（幂等，每趟都写）。
  Escribir32(base, ctx_addr + 3916, 1);

  if (g_ultimo_valido) {
    const auto ahora = std::chrono::steady_clock::now();
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(ahora - g_ultimo_avance).count();
    if (ms >= 0 && ms < 16) {
      return;  // never advance faster than 60 fps
    }
    g_ultimo_avance = ahora;
  } else {
    g_ultimo_avance = std::chrono::steady_clock::now();
  }

  if (!g_fin) {
    t2rotf::video_wmv3::Fotograma f;
    if (g_pelicula->Siguiente(f)) {
      g_ultimo_f = f;
      g_ultimo_valido = true;
      ++g_inyectados;
      const int64_t total = g_pelicula->fotogramas();
      if (g_inyectados <= 4 || (g_inyectados % 120) == 0) {
        REXLOG_INFO("[video] T2ROTF nativo: BOMBA fotograma {} ({}/{}) '{}' {}x{} {}",
                    g_inyectados, total - g_inyectados, total, g_ruta, f.ancho, f.alto,
                    f.clave ? "I" : "P/B");
      }
    } else {
      g_fin = true;
      REXLOG_WARN("[video] T2ROTF nativo: BOMBA fin de '{}' ({} fotogramas ffmpeg, se mantiene el ultimo)",
                  g_ruta, g_inyectados);
    }
  }

  if (!g_ultimo_valido) {
    return;
  }

  if (!g_verde_decidido) {
    g_verde_decidido = true;
    g_verde_restantes = 30;
    REXLOG_ERROR("[video] T2ROTF nativo: PRUEBA VERDE 30 pasos (~1 s) para separar present de decode "
                 "ctx={:08X} disY={:08X} disU={:08X} disV={:08X} pitchY={} pitchC={} altY={} altC={}",
                 ctx_addr, Leer32(base, ctx_addr + 3744), Leer32(base, ctx_addr + 3748),
                 Leer32(base, ctx_addr + 3752), Leer32(base, ctx_addr + 204),
                 Leer32(base, ctx_addr + 208), Leer32(base, ctx_addr + 212),
                 Leer32(base, ctx_addr + 216));
  }
  if (g_verde_restantes) {
    --g_verde_restantes;
    PintarVerde(base, ctx_addr);
    return;
  }

  CopiarPlanos(base, ctx_addr, g_ultimo_f);
  CopiarDisplayDesplazado(base, ctx_addr, g_ultimo_f);
}

}  // namespace
}  // namespace t2rotf::video_nativo

// sub_8279C5B8: Prepare, called by the pump once at the top of every pass.
// Runs the original body first (it writes guest fields the pump depends on),
// resets the per-pass decode flag, and (re)opens PeliculaWmv when the movie
// path changes. NOTE: state==7 is the NORMAL state for the WMV/WVC1 pump;
// it must NOT be forced down to 3.
REX_HOOK_RAW(sub_8279C5B8) {
  using namespace t2rotf::video_nativo;

  const uint32_t ctx_addr = ctx.r3.u32;
  g_base_host.store(reinterpret_cast<uint64_t>(base), std::memory_order_relaxed);
  g_ctx_visto.store(ctx_addr, std::memory_order_relaxed);
  g_prep.fetch_add(1, std::memory_order_relaxed);
  ArrancarPerro();

  // Run the original Prepare body first: it writes the guest fields the caller
  // depends on. Safe: no calls, no loops, no blocking inside.
  __imp__sub_8279C5B8(ctx, base);

  const std::string ruta = rex::kernel::xboxkrnl::NfsmwUltimoWmvLeido();
  const uint32_t estado = Leer32(base, ctx_addr + 15504);

  // Each pump pass starts here: allow exactly one FFmpeg frame in the
  // sub_827FA2E8 hook for the MB loop that follows.
  g_cuadro_por_paso = false;
  ++g_pasos_pump;
  if (g_estado_log != estado) {
    g_estado_log = estado;
    REXLOG_ERROR("[video] T2ROTF nativo: Prepare ctx={:08X} st15504={} ruta='{}'",
                 ctx_addr, estado, ruta);
    DumpCtx(base, ctx_addr, "estado");
  } else if ((g_pasos_pump % 120) == 0) {
    REXLOG_INFO("[video] T2ROTF nativo: Prepare passo {} ctx={:08X} st15504={}",
                g_pasos_pump, ctx_addr, estado);
    DumpCtx(base, ctx_addr, "periodico");
  }

  std::lock_guard<std::mutex> lock(g_mutex);

  // Same movie and same decoder context -> nothing to do (unless the previous
  // pass reached the end of the container: then the game is replaying it).
  if (g_ctx == ctx_addr && g_ruta == ruta && g_pelicula && !g_fin) {
    return;
  }

  // Reset.
  g_pelicula.reset();
  g_ruta.clear();
  g_ctx = 0;
  g_fin = false;
  g_ultimo_valido = false;
  g_ultimo_f = t2rotf::video_wmv3::Fotograma{};
  g_inyectados = 0;
  g_verde_decidido = false;
  g_verde_restantes = 0;

  if (ruta.empty()) {
    REXLOG_ERROR("[video] T2ROTF nativo: ruta VACIA para ctx {:08X} (no se pudo abrir el wmv)", ctx_addr);
    return;
  }

  g_pelicula = std::make_unique<t2rotf::video_wmv3::PeliculaWmv>();
  if (g_pelicula->Abrir(ruta)) {
    g_ruta = ruta;
    g_ctx = ctx_addr;
    REXLOG_INFO("[video] T2ROTF nativo: abierto '{}' (ctx {:08X})", ruta, ctx_addr);
  } else {
    g_pelicula.reset();
    REXLOG_WARN("[video] T2ROTF nativo: no se pudo abrir '{}'", ruta);
  }
}

// sub_827FA2E8: per-MB WVC1 decode entry (state==7 pump, 18.cpp:86927).
// First call of each pump pass: decode one FFmpeg frame and fill the YUV
// planes; every later call in the same pass is a no-op returning success.
REX_HOOK_RAW(sub_827FA2E8) {
  using namespace t2rotf::video_nativo;

  const uint32_t ctx_addr = ctx.r3.u32;

  if (!g_log_fa) {
    g_log_fa = true;
    REXLOG_ERROR("[video] T2ROTF nativo: HOOK ENTER sub_827FA2E8 ctx={:08X}", ctx_addr);
    DumpCtx(base, ctx_addr, "FA2E8");
  }

  if (!PeliculaActiva(ctx_addr)) {
    __imp__sub_827FA2E8(ctx, base);
    return;
  }

  if (!g_cuadro_por_paso) {
    g_cuadro_por_paso = true;
    std::lock_guard<std::mutex> lock(g_mutex);
    t2rotf::video_wmv3::Fotograma f;
    if (g_pelicula->Siguiente(f)) {
      CopiarPlanos(base, ctx_addr, f);
      ++g_fotogramas_ffmpeg;
      REXLOG_INFO("[video] T2ROTF nativo: FA2E8 fotograma {} de '{}' ({}x{} {})",
                  g_pelicula->fotogramas(), g_ruta, f.ancho, f.alto, f.clave ? "I" : "P/B");
    } else {
      // End of the container: hold the last frame, let the game's stream
      // layer finish the movie on its own. Report success like the other
      // no-ops so the pump's normal state machine keeps running.
      REXLOG_WARN("[video] T2ROTF nativo: FA2E8 fin de '{}' ({} fotogramas, {} ffmpeg)",
                  g_ruta, g_pelicula->fotogramas(), g_fotogramas_ffmpeg);
      g_pelicula.reset();
      g_ruta.clear();
      g_ctx = 0;
      g_cuadro_por_paso = false;
    }
  }

  // Eat the recompiled per-MB decode: report success, touch no guest state.
  ctx.r3.u64 = 0;
}

// sub_827F7B28: per-MB bitstream header reader (18.cpp:86901). The FFmpeg
// frame already filled the planes, so the guest bitstream is not needed.
REX_HOOK_RAW(sub_827F7B28) {
  using namespace t2rotf::video_nativo;

  const uint32_t ctx_addr = ctx.r3.u32;
  if (!g_log_f7) {
    g_log_f7 = true;
    REXLOG_ERROR("[video] T2ROTF nativo: HOOK ENTER sub_827F7B28 ctx={:08X}", ctx_addr);
  }

  if (!PeliculaActiva(ctx_addr)) {
    __imp__sub_827F7B28(ctx, base);
    return;
  }

  ctx.r3.u64 = 0;
}

// sub_827EBC00: per-MB motion compensation (23.cpp:86518). Would overwrite the
// planes we just filled, so it is skipped while a movie is active.
REX_HOOK_RAW(sub_827EBC00) {
  using namespace t2rotf::video_nativo;

  const uint32_t ctx_addr = ctx.r3.u32;
  if (!g_log_ebc) {
    g_log_ebc = true;
    REXLOG_ERROR("[video] T2ROTF nativo: HOOK ENTER sub_827EBC00 ctx={:08X}", ctx_addr);
  }

  if (!PeliculaActiva(ctx_addr)) {
    __imp__sub_827EBC00(ctx, base);
    return;
  }

  ctx.r3.u64 = 0;
}

// sub_827E8EB8: per-MB motion compensation (42.cpp:87232, alternate path).
REX_HOOK_RAW(sub_827E8EB8) {
  using namespace t2rotf::video_nativo;

  const uint32_t ctx_addr = ctx.r3.u32;
  if (!g_log_e8) {
    g_log_e8 = true;
    REXLOG_ERROR("[video] T2ROTF nativo: HOOK ENTER sub_827E8EB8 ctx={:08X}", ctx_addr);
  }

  if (!PeliculaActiva(ctx_addr)) {
    __imp__sub_827E8EB8(ctx, base);
    return;
  }

  ctx.r3.u64 = 0;
}

// sub_827E6F68: per-row plane writer (0.cpp:85231). Skipped; the FFmpeg copy
// already wrote the complete planes.
REX_HOOK_RAW(sub_827E6F68) {
  using namespace t2rotf::video_nativo;

  const uint32_t ctx_addr = ctx.r3.u32;
  if (!g_log_e6) {
    g_log_e6 = true;
    REXLOG_ERROR("[video] T2ROTF nativo: HOOK ENTER sub_827E6F68 ctx={:08X}", ctx_addr);
  }

  if (!PeliculaActiva(ctx_addr)) {
    __imp__sub_827E6F68(ctx, base);
    return;
  }

  ctx.r3.u64 = 0;
}

// sub_827E7AF0: frame-end / present op (op code in r4). Kept real so the
// game's presentation path (and the dispatcher's decode->display copy) sees
// a normal frame boundary; only instrumented here.
REX_HOOK_RAW(sub_827E7AF0) {
  using namespace t2rotf::video_nativo;

  const uint32_t ctx_addr = ctx.r3.u32;
  ++g_e7_count;
  if (g_e7_count == 1) {
    REXLOG_ERROR("[video] T2ROTF nativo: HOOK ENTER sub_827E7AF0 ctx={:08X} op={}",
                 ctx_addr, ctx.r4.u32);
  } else if ((g_e7_count % 300) == 0) {
    REXLOG_INFO("[video] T2ROTF nativo: E7AF0 llamadas={} ctx={:08X}", g_e7_count, ctx_addr);
  }

  __imp__sub_827E7AF0(ctx, base);
}

// sub_827EDEE8: the state==7 movie pump itself (installed by the dispatcher
// into [ctx+15808]). T2 diagnostic only: log entry and dims, keep the
// original body (it is what the FFmpeg HLE today expects to call Prepare).
REX_HOOK_RAW(sub_827EDEE8) {
  using namespace t2rotf::video_nativo;

  const uint32_t ctx_addr = ctx.r3.u32;
  ++g_pump_count;
  if (g_pump_count == 1 || (g_pump_count % 300) == 0) {
    REXLOG_ERROR("[video] T2ROTF nativo: HOOK ENTER sub_827EDEE8 n={} ctx={:08X} "
                 "mbcols[136]={} mbrows[140]={}",
                 g_pump_count, ctx_addr,
                 Leer32(base, ctx_addr + 136), Leer32(base, ctx_addr + 140));
    DumpCtx(base, ctx_addr, "pump");
  }

  __imp__sub_827EDEE8(ctx, base);
}

// sub_82850830: dispatcher's decode->display plane copy. T2 diagnostic only:
// log call args and keep the original running. The 16-148 byte calls are the
// SDK's own memcpy noise (~1300/s); only >= 512 byte copies are interesting.
REX_HOOK_RAW(sub_82850830) {
  using namespace t2rotf::video_nativo;

  const uint32_t r3 = ctx.r3.u32;
  const uint32_t r4 = ctx.r4.u32;
  const uint32_t r5 = ctx.r5.u32;
  ++g_copy_count;
  static uint64_t n_grandes = 0;
  static uint64_t n_chicas = 0;
  if (r5 >= 512) {
    ++n_grandes;
  } else {
    ++n_chicas;
  }
  const bool interesante = (r5 >= 512) && (n_grandes <= 80);
  if (g_copy_count <= 6 || interesante || (g_copy_count % 20000) == 0) {
    REXLOG_ERROR("[video] T2ROTF nativo: HOOK ENTER sub_82850830 n={} r3={:08X} r4={:08X} len={} "
                 "(grandes={} chicas={})",
                 g_copy_count, r3, r4, r5, n_grandes, n_chicas);
  }

  __imp__sub_82850830(ctx, base);
}

// ==========================================================================
// T2DIAG: runtime evidence for the points that are hot in the reference
// player but sample 0.0 % on the console:
//   guest wait wrappers  824F3580 / 824F34F0   (xenia 739 s / 414 s)
//   media worker         82745140 (+ 8277C2xx) (xenia start_address)
//   movie chain          82793DC8 -> 827937E8 -> 8279BFC0 -> 827983A8
//   the installed pump   827CE598              (xenia 17.4 s)
// Every probe is read-only: it logs (first hits + a slow heartbeat + lr = the
// caller) and then runs the original function.
// ==========================================================================
REX_HOOK_RAW(sub_824F5578) {
  using namespace t2rotf::video_nativo;
  static uint64_t n = 0;
  ++n;
  if (n <= 30 || (n % 5000) == 0) {
    REXLOG_ERROR("[video] T2DIAG trampa sub_824F5578 n={} ctx={:08X} entrada={:08X} lr={:08X}",
                 n, ctx.r3.u32, ctx.r4.u32, static_cast<uint32_t>(ctx.lr));
  }
  __imp__sub_824F5578(ctx, base);
}

REX_HOOK_RAW(sub_824F0658) {
  using namespace t2rotf::video_nativo;
  static uint64_t n = 0;
  ++n;
  /* T6: the 1/5000 print flooded the log (13483 lines in the 37 s session) and
   * buried everything else; the ring heartbeat (T2DIAG LATIDO, printed from the
   * sub_822D13F0 probe below) replaces it. */
  if (n <= 8) {
    REXLOG_ERROR("[video] T2DIAG puerta sub_824F0658 n={} r3={:08X} lr={:08X}", n, ctx.r3.u32,
                 static_cast<uint32_t>(ctx.lr));
  }
  __imp__sub_824F0658(ctx, base);
}

REX_HOOK_RAW(sub_824F3580) {
  using namespace t2rotf::video_nativo;
  static uint64_t n = 0;
  ++n;
  if (n <= 6 || (n % 20000) == 0) {
    REXLOG_ERROR("[video] T2DIAG espera sub_824F3580 n={} r3={:08X} r4={} r5={} lr={:08X}", n,
                 ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, static_cast<uint32_t>(ctx.lr));
  }
  __imp__sub_824F3580(ctx, base);
}

REX_HOOK_RAW(sub_824F34F0) {
  using namespace t2rotf::video_nativo;
  static uint64_t n = 0;
  ++n;
  if (n <= 6 || (n % 20000) == 0) {
    REXLOG_ERROR("[video] T2DIAG espera sub_824F34F0 n={} r3={} r4={:08X} lr={:08X}", n, ctx.r3.u32,
                 ctx.r4.u32, static_cast<uint32_t>(ctx.lr));
  }
  __imp__sub_824F34F0(ctx, base);
}

REX_HOOK_RAW(sub_82745140) {
  using namespace t2rotf::video_nativo;
  static uint64_t n = 0;
  ++n;
  if (n <= 4 || (n % 1000) == 0) {
    REXLOG_ERROR("[video] T2DIAG worker sub_82745140 n={} ctx={:08X} lr={:08X}", n, ctx.r3.u32,
                 static_cast<uint32_t>(ctx.lr));
  }
  __imp__sub_82745140(ctx, base);
}

REX_HOOK_RAW(sub_8277C218) {
  using namespace t2rotf::video_nativo;
  static uint64_t n = 0;
  ++n;
  if (n <= 3 || (n % 1000) == 0) {
    REXLOG_ERROR("[video] T2DIAG worker sub_8277C218 n={} ctx={:08X} lr={:08X}", n, ctx.r3.u32,
                 static_cast<uint32_t>(ctx.lr));
  }
  __imp__sub_8277C218(ctx, base);
}

REX_HOOK_RAW(sub_8277C248) {
  using namespace t2rotf::video_nativo;
  static uint64_t n = 0;
  ++n;
  if (n <= 3 || (n % 1000) == 0) {
    REXLOG_ERROR("[video] T2DIAG worker sub_8277C248 n={} ctx={:08X} lr={:08X}", n, ctx.r3.u32,
                 static_cast<uint32_t>(ctx.lr));
  }
  __imp__sub_8277C248(ctx, base);
}

REX_HOOK_RAW(sub_8277C278) {
  using namespace t2rotf::video_nativo;
  static uint64_t n = 0;
  ++n;
  if (n <= 3 || (n % 1000) == 0) {
    REXLOG_ERROR("[video] T2DIAG worker sub_8277C278 n={} ctx={:08X} lr={:08X}", n, ctx.r3.u32,
                 static_cast<uint32_t>(ctx.lr));
  }
  __imp__sub_8277C278(ctx, base);
}

REX_HOOK_RAW(sub_8277C2A8) {
  using namespace t2rotf::video_nativo;
  static uint64_t n = 0;
  ++n;
  if (n <= 3 || (n % 1000) == 0) {
    REXLOG_ERROR("[video] T2DIAG worker sub_8277C2A8 n={} ctx={:08X} lr={:08X}", n, ctx.r3.u32,
                 static_cast<uint32_t>(ctx.lr));
  }
  __imp__sub_8277C2A8(ctx, base);
}

REX_HOOK_RAW(sub_82793DC8) {
  using namespace t2rotf::video_nativo;
  static uint64_t n = 0;
  ++n;
  if (n <= 8 || (n % 1000) == 0) {
    REXLOG_ERROR("[video] T2DIAG fourcc sub_82793DC8 n={} r3={:08X} r4={:08X} r5={:08X} lr={:08X}",
                 n, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, static_cast<uint32_t>(ctx.lr));
  }
  __imp__sub_82793DC8(ctx, base);
}

REX_HOOK_RAW(sub_827937E8) {
  using namespace t2rotf::video_nativo;
  static uint64_t n = 0;
  ++n;
  g_cadena937.fetch_add(1, std::memory_order_relaxed);
  if (n <= 5 || (n % 60) == 0) {
    REXLOG_ERROR("[video] T2DIAG cadena sub_827937E8 n={} r3={:08X} r4={:08X} lr={:08X}", n,
                 ctx.r3.u32, ctx.r4.u32, static_cast<uint32_t>(ctx.lr));
  }
  __imp__sub_827937E8(ctx, base);
}

REX_HOOK_RAW(sub_8279BFC0) {
  using namespace t2rotf::video_nativo;
  static uint64_t n = 0;
  ++n;
  g_cadena9bf.fetch_add(1, std::memory_order_relaxed);
  if (n <= 5 || (n % 60) == 0) {
    REXLOG_ERROR("[video] T2DIAG cadena sub_8279BFC0 n={} r3={:08X} r4={:08X} r5={} lr={:08X}", n,
                 ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, static_cast<uint32_t>(ctx.lr));
  }
  __imp__sub_8279BFC0(ctx, base);
}

REX_HOOK_RAW(sub_827983A8) {
  using namespace t2rotf::video_nativo;
  static uint64_t n = 0;
  ++n;
  g_despacho.fetch_add(1, std::memory_order_relaxed);
  g_base_host.store(reinterpret_cast<uint64_t>(base), std::memory_order_relaxed);
  ArrancarPerro();
  if (n <= 3 || (n % 60) == 0) {
    const uint32_t o = ctx.r3.u32;
    REXLOG_ERROR("[video] T2DIAG despacho sub_827983A8 n={} obj={:08X} st15504={} gate20904={} "
                 "pump15808={:08X} lr={:08X}",
                 n, o, Leer32(base, o + 15504), Leer32(base, o + 20904), Leer32(base, o + 15808),
                 static_cast<uint32_t>(ctx.lr));
  }
  __imp__sub_827983A8(ctx, base);
}

// sub_827CE598: the pump the guest really runs (installed by the dispatcher
// into [ctx+15808]). This is where FFmpeg is driven now; the original body
// still runs, so the game's own state machine and present step are untouched.
REX_HOOK_RAW(sub_827CE598) {
  using namespace t2rotf::video_nativo;

  const uint32_t ctx_addr = ctx.r3.u32;
  g_base_host.store(reinterpret_cast<uint64_t>(base), std::memory_order_relaxed);
  g_ctx_visto.store(ctx_addr, std::memory_order_relaxed);
  g_pump_entrada.fetch_add(1, std::memory_order_relaxed);
  ArrancarPerro();

  BombaTick(base, ctx_addr);
  g_pump_salida.fetch_add(1, std::memory_order_relaxed);

  // 时间限频（原来是「每 60 趟」的计数门槛，静默期就什么都看不到）。
  if (Turno(0)) {
    VolcarGuest("pump");
  }

  __imp__sub_827CE598(ctx, base);
}

// sub_827CADC0: the pump's end step, the one that receives the display planes
// (r5/r6/r7) and hands the decoded picture on. Instrumented only, to prove the
// frame written by BombaTick is the frame this step sees.
REX_HOOK_RAW(sub_827CADC0) {
  using namespace t2rotf::video_nativo;

  ++g_pres_ticks;
  if (g_pres_ticks == 1 || (g_pres_ticks % 120) == 0) {
    REXLOG_ERROR("[video] T2ROTF nativo: PRESENTAR sub_827CADC0 n={} r3={:08X} y={:08X} u={:08X} v={:08X} "
                 "(inyectados={})",
                 g_pres_ticks, ctx.r3.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, g_inyectados);
  }
  __imp__sub_827CADC0(ctx, base);
}

// ==========================================================================
// T2DIAG T6: GPU 前进信号取证
//
// 上一轮 1 ms 栈采样（rex_perfil(20)）已给的事实：
//   宿主 GPU Commands 线程 100% 时间在 rex::thread::Wait(5ms) => ring 无新 kick；
//   guest D3D 提交线程 99.7% CPU 全在 sub_822D13F0 / sub_822D4488 的 ring 空间等待；
//   Game Master 86% 在 NtYieldExecution -> svcSleepThread。
// 本段只读取证：计数、采样 guest ring 字段、每 2 s 打一行 LATIDO（全部计数 +
// ring 实值 + 影片泵计数放在同一条时间轴上），并在泵的三个前置阶段记返回值。
// 不改任何 guest 状态、不改变控制流。
// ==========================================================================
#include <atomic>

namespace t2rotf::video_nativo {

REX_EXTERN(__imp__sub_822D13F0);  // ring 空间等待：轮询 [dev+10896] 首字
REX_EXTERN(__imp__sub_822D4488);  // ring 提交/等待外层（写 CP_RB_WPTR 的那条链）
REX_EXTERN(__imp__sub_822D47A0);  // guest GPU 中断回调注册/派发
REX_EXTERN(__imp__sub_822D9570);  // 中断回调的 r3==0 分支后续
REX_EXTERN(__imp__sub_822D4A08);  // D3D ring / 命令缓冲初始化 + kickoff
REX_EXTERN(__imp__sub_82B507D8);  // 泵前置阶段 1（返回非 0 = 泵提前跳出，跳过呈现）
REX_EXTERN(__imp__sub_82B86000);  // 泵前置阶段 2
REX_EXTERN(__imp__sub_82B68458);  // 泵前置阶段 3
REX_EXTERN(__imp__sub_827CC058);  // 泵尾阶段（参考端从不执行）
REX_EXTERN(__imp__sub_827CC6E0);  // 泵尾阶段（参考端从不执行）

namespace {

struct CuentasGpu {
  std::atomic<uint64_t> anillo{0};         // sub_822D13F0 进入
  std::atomic<uint64_t> anillo_espera{0};  // 返回 != 0：还在等 ring 空间
  std::atomic<uint64_t> anillo_pasa{0};    // 返回 == 0：等到/通过
  std::atomic<uint64_t> externo{0};        // sub_822D4488 进入
  std::atomic<uint64_t> init_anillo{0};    // sub_822D4A08 进入
  std::atomic<uint64_t> irq47{0};          // sub_822D47A0 进入
  std::atomic<uint64_t> irq95{0};          // sub_822D9570 进入
  std::atomic<uint64_t> etapa1{0}, etapa2{0}, etapa3{0};               // 三个前置阶段进入
  std::atomic<uint64_t> etapa1_fin{0}, etapa2_fin{0}, etapa3_fin{0};   // 返回非 0 的次数
  std::atomic<uint64_t> cc058{0}, cc6e0{0};                            // 泵尾阶段
};
CuentasGpu g_cg;

// 最近一次 ring 采样（只在 sub_822D13F0 的采样点写，LATIDO 读）
std::atomic<uint32_t> g_rk_dev{0}, g_rk_p{0}, g_rk_pv{0}, g_rk_v{0}, g_rk_f10888{0},
    g_rk_f10941{0}, g_rk_f11008{0}, g_rk_lr{0}, g_rk_ret{0};
bool g_rk_valido = false;

// 只在最热的 ring 等待钩子里按调用次数节流，避免每次调用都读时钟
bool LatidoVencido() {
  static std::atomic<uint64_t> tic{0};
  const uint64_t llamada = ++tic;
  if ((llamada & 0xFFFu) != 0) {
    return false;  // [T24] 4096 次调用看一次时钟（原来 26 万次，实机等于永不打印）
  }
  static std::chrono::steady_clock::time_point ultimo{};
  static bool primero = true;
  const auto ahora = std::chrono::steady_clock::now();
  if (!primero) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(ahora - ultimo).count();
    if (ms < 2000) {
      return false;
    }
  }
  primero = false;
  ultimo = ahora;
  return true;
}

void LatidoGpu() {
  REXLOG_ERROR(
      "[video] T2DIAG LATIDO anillo={} espera={} pasa={} externo={} init4A08={} irq47A0={} "
      "irq9570={} cc058={} cc6e0={} etapas1/2/3={}/{}/{} saltos={}/{}/{} bomba={} iny={} pres={}",
      g_cg.anillo.load(std::memory_order_relaxed),
      g_cg.anillo_espera.load(std::memory_order_relaxed),
      g_cg.anillo_pasa.load(std::memory_order_relaxed),
      g_cg.externo.load(std::memory_order_relaxed),
      g_cg.init_anillo.load(std::memory_order_relaxed),
      g_cg.irq47.load(std::memory_order_relaxed), g_cg.irq95.load(std::memory_order_relaxed),
      g_cg.cc058.load(std::memory_order_relaxed), g_cg.cc6e0.load(std::memory_order_relaxed),
      g_cg.etapa1.load(std::memory_order_relaxed), g_cg.etapa2.load(std::memory_order_relaxed),
      g_cg.etapa3.load(std::memory_order_relaxed), g_cg.etapa1_fin.load(std::memory_order_relaxed),
      g_cg.etapa2_fin.load(std::memory_order_relaxed),
      g_cg.etapa3_fin.load(std::memory_order_relaxed), g_bomba_ticks, g_inyectados, g_pres_ticks);
  if (g_rk_valido) {
    REXLOG_ERROR(
        "[video] T2DIAG ANILLO dev={:08X} P={:08X} *P={:08X} V10908={:08X} f10888={:08X} "
        "f10941={:08X} f11008={:08X} lr={:08X} ret={}",
        g_rk_dev.load(std::memory_order_relaxed), g_rk_p.load(std::memory_order_relaxed),
        g_rk_pv.load(std::memory_order_relaxed), g_rk_v.load(std::memory_order_relaxed),
        g_rk_f10888.load(std::memory_order_relaxed), g_rk_f10941.load(std::memory_order_relaxed),
        g_rk_f11008.load(std::memory_order_relaxed), g_rk_lr.load(std::memory_order_relaxed),
        g_rk_ret.load(std::memory_order_relaxed));
  }
}

}  // namespace
}  // namespace t2rotf::video_nativo

// sub_822D13F0: ring 空间等待（guest D3D 提交线程 99.7% 时间停在这里）。
// r3 = 指向调用者局部结构的指针，其首字是该 GPU 设备的地址。采样点读
// [dev+10896]（guest 唯一可见的 GPU 读指针来源：96 字节分配块的首字）。
REX_HOOK_RAW(sub_822D13F0) {
  using namespace t2rotf::video_nativo;
  const uint64_t n = g_cg.anillo.fetch_add(1, std::memory_order_relaxed) + 1;
  if (((n <= 4) || (n % 1024) == 0) && ctx.r3.u32) {
    const uint32_t dev = Leer32(base, ctx.r3.u32);
    // [v8.7-IRQABI] dev 从 guest 读来，读它的字段前先校验，绝不裸读未映射地址。
    const bool okdev = EsDireccionDispositivo(dev);
    const uint32_t p = okdev ? Leer32(base, dev + 10896) : 0;
    g_rk_dev.store(dev, std::memory_order_relaxed);
    g_rk_p.store(p, std::memory_order_relaxed);
    g_rk_pv.store(EsDireccionDispositivo(p) ? Leer32(base, p) : 0, std::memory_order_relaxed);
    g_rk_v.store(okdev ? Leer32(base, dev + 10908) : 0, std::memory_order_relaxed);
    g_rk_f10888.store(okdev ? Leer32(base, dev + 10888) : 0, std::memory_order_relaxed);
    g_rk_f10941.store(okdev ? Leer32(base, dev + 10941) : 0, std::memory_order_relaxed);
    g_rk_f11008.store(okdev ? Leer32(base, dev + 11008) : 0, std::memory_order_relaxed);
    g_rk_lr.store(static_cast<uint32_t>(ctx.lr), std::memory_order_relaxed);
    g_rk_valido = true;
    if (n <= 4) {
      REXLOG_ERROR("[video] T2DIAG ANILLO entrada n={} locals={:08X} dev={:08X} P={:08X} *P={:08X}",
                   n, ctx.r3.u32, dev, p, p ? Leer32(base, p) : 0);
    }
  }
  if (::banjo::renut_gpu::Active()) {
    ::renut_native_sub_822D13F0(ctx, base);  // Renut（deko3d）侧的 D3D_CBlocker_Check
  } else {
    __imp__sub_822D13F0(ctx, base);
  }
  const uint32_t ret = ctx.r3.u32;
  if (ret) {
    g_cg.anillo_espera.fetch_add(1, std::memory_order_relaxed);
  } else {
    g_cg.anillo_pasa.fetch_add(1, std::memory_order_relaxed);
  }
  if (LatidoVencido()) {
    g_rk_ret.store(ret, std::memory_order_relaxed);
    LatidoGpu();
  }
}

// sub_822D4488: ring 提交/等待外层（真正写 CP_RB_WPTR 的那条链）。
REX_HOOK_RAW(sub_822D4488) {
  using namespace t2rotf::video_nativo;
  const uint64_t n = g_cg.externo.fetch_add(1, std::memory_order_relaxed) + 1;
  if (EsDireccionDispositivo(ctx.r3.u32)) {  // [v8.7-IRQABI] 设备地址先校验再落盘
    g_dev_visto.store(ctx.r3.u32, std::memory_order_relaxed);
    g_base_host.store(reinterpret_cast<uint64_t>(base), std::memory_order_relaxed);
    ::t2rotf::gpu_signal::NotificarDispositivo(ctx.r3.u32);  // [v8.2-SHADOW]
  }
  if (n <= 5) {
    REXLOG_ERROR("[video] T2DIAG externo sub_822D4488 n={} r3={:08X} r4={:08X} r5={:08X} lr={:08X}",
                 n, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, static_cast<uint32_t>(ctx.lr));
  }
  if (::banjo::renut_gpu::Active()) {
    ::renut_native_sub_822D4488(ctx, base);  // Renut（deko3d）侧的 D3D_CDevice_BlockOnFence
  } else {
    __imp__sub_822D4488(ctx, base);
  }
}

// sub_822D4A08: D3D ring / 命令缓冲初始化（含 VdSetSystemCommandBufferGpuIdentifierAddress）。
REX_HOOK_RAW(sub_822D4A08) {
  using namespace t2rotf::video_nativo;
  g_cg.init_anillo.fetch_add(1, std::memory_order_relaxed);
  REXLOG_ERROR("[video] T2DIAG init sub_822D4A08 r3={:08X} r4={:08X} r5={:08X} r6={:08X} lr={:08X}",
               ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, static_cast<uint32_t>(ctx.lr));
  if (::banjo::renut_gpu::Active()) {
    ::renut_native_sub_822D4A08(ctx, base);  // Renut（deko3d）侧的 D3DDevice_SetRingBufferParameters
  } else {
    __imp__sub_822D4A08(ctx, base);
  }
  REXLOG_ERROR("[video] T2DIAG init sub_822D4A08 ret={}", ctx.r3.u32);
}

// sub_822D47A0 / sub_822D9570: guest GPU 中断回调（对照 xenia 的 IssueSwap 派发）。
REX_HOOK_RAW(sub_822D47A0) {
  using namespace t2rotf::video_nativo;
  const uint64_t n = g_cg.irq47.fetch_add(1, std::memory_order_relaxed) + 1;
  // [v8.7-IRQABI] r3 = 来源（0 垂直同步 / 1 提交完成），r4 = 设备。设备只能取 r4，
  // 且校验通过才落盘；此前把 r3 当设备正是「启动即退出、零加载」的根因（见文件头说明）。
  if (EsDireccionDispositivo(ctx.r4.u32)) {
    g_dev_visto.store(ctx.r4.u32, std::memory_order_relaxed);
    g_base_host.store(reinterpret_cast<uint64_t>(base), std::memory_order_relaxed);
  } else if (g_irq_dev_avisado.fetch_add(1, std::memory_order_relaxed) < 2) {
    REXLOG_ERROR("[video] T2DIAG irq sub_822D47A0 设备候选 r4={:08X} 不健全（r3={} 为来源），忽略",
                 ctx.r4.u32, ctx.r3.u32);
  }
  if (n <= 12) {
    REXLOG_ERROR("[video] T2DIAG irq sub_822D47A0 n={} fuente={} dev={:08X} r5={:08X} r6={:08X}", n,
                 ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
  }
  __imp__sub_822D47A0(ctx, base);
}

REX_HOOK_RAW(sub_822D9570) {
  using namespace t2rotf::video_nativo;
  const uint64_t n = g_cg.irq95.fetch_add(1, std::memory_order_relaxed) + 1;
  // [v8.7-IRQABI] r3==0 分支后续的设备候选优先取 r3，不健全时退 r4；两者都必须过校验。
  const uint32_t cand = EsDireccionDispositivo(ctx.r3.u32) ? ctx.r3.u32 : ctx.r4.u32;
  if (EsDireccionDispositivo(cand)) {
    g_dev_visto.store(cand, std::memory_order_relaxed);
    g_base_host.store(reinterpret_cast<uint64_t>(base), std::memory_order_relaxed);
  } else if (g_irq_dev_avisado.fetch_add(1, std::memory_order_relaxed) < 2) {
    REXLOG_ERROR("[video] T2DIAG irq sub_822D9570 设备候选 r3={:08X}/r4={:08X} 均不健全，忽略",
                 ctx.r3.u32, ctx.r4.u32);
  }
  if (n <= 8) {
    REXLOG_ERROR("[video] T2DIAG irq sub_822D9570 n={} r3={:08X} r4={:08X} lr={:08X}", n,
                 ctx.r3.u32, ctx.r4.u32, static_cast<uint32_t>(ctx.lr));
  }
  if (EsDireccionDispositivo(cand)) {
    ::t2rotf::gpu_signal::NotificarDispositivo(cand);  // [v8.2-SHADOW]
  }
  __imp__sub_822D9570(ctx, base);
}

// 泵的三个前置阶段：返回非 0 时泵立即跳出、跳过呈现步骤（本轮第二嫌疑点）。
REX_HOOK_RAW(sub_82B507D8) {
  using namespace t2rotf::video_nativo;
  const uint64_t n = g_cg.etapa1.fetch_add(1, std::memory_order_relaxed) + 1;
  const uint32_t in3 = ctx.r3.u32;
  __imp__sub_82B507D8(ctx, base);
  const uint32_t ret = ctx.r3.u32;
  if (ret) {
    g_cg.etapa1_fin.fetch_add(1, std::memory_order_relaxed);
  }
  if (n <= 16) {
    REXLOG_ERROR("[video] T2DIAG etapa1 sub_82B507D8 n={} r3={:08X} r4={:08X} ret={}", n, in3,
                 ctx.r4.u32, ret);
  }
}

REX_HOOK_RAW(sub_82B86000) {
  using namespace t2rotf::video_nativo;
  const uint64_t n = g_cg.etapa2.fetch_add(1, std::memory_order_relaxed) + 1;
  const uint32_t in3 = ctx.r3.u32;
  __imp__sub_82B86000(ctx, base);
  const uint32_t ret = ctx.r3.u32;
  if (ret) {
    g_cg.etapa2_fin.fetch_add(1, std::memory_order_relaxed);
  }
  if (n <= 16) {
    REXLOG_ERROR("[video] T2DIAG etapa2 sub_82B86000 n={} r3={:08X} r4={:08X} ret={}", n, in3,
                 ctx.r4.u32, ret);
  }
}

REX_HOOK_RAW(sub_82B68458) {
  using namespace t2rotf::video_nativo;
  const uint64_t n = g_cg.etapa3.fetch_add(1, std::memory_order_relaxed) + 1;
  const uint32_t in3 = ctx.r3.u32;
  __imp__sub_82B68458(ctx, base);
  const uint32_t ret = ctx.r3.u32;
  if (ret) {
    g_cg.etapa3_fin.fetch_add(1, std::memory_order_relaxed);
  }
  if (n <= 16) {
    REXLOG_ERROR("[video] T2DIAG etapa3 sub_82B68458 n={} r3={:08X} r4={:08X} ret={}", n, in3,
                 ctx.r4.u32, ret);
  }
}

// 泵尾阶段：参考端（xenia）全表里根本不存在的两个函数，用来确认泵在哪一步断掉。
REX_HOOK_RAW(sub_827CC058) {
  using namespace t2rotf::video_nativo;
  const uint64_t n = g_cg.cc058.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 8) {
    REXLOG_ERROR("[video] T2DIAG cola sub_827CC058 n={} r3={:08X} r4={:08X} r5={:08X} lr={:08X}", n,
                 ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, static_cast<uint32_t>(ctx.lr));
  }
  __imp__sub_827CC058(ctx, base);
}

REX_HOOK_RAW(sub_827CC6E0) {
  using namespace t2rotf::video_nativo;
  const uint64_t n = g_cg.cc6e0.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 8) {
    REXLOG_ERROR("[video] T2DIAG cola sub_827CC6E0 n={} r3={:08X} r4={:08X} r5={:08X} lr={:08X}", n,
                 ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, static_cast<uint32_t>(ctx.lr));
  }
  __imp__sub_827CC6E0(ctx, base);
}

// ==========================================================================
// [T23] 停摆点定位 + yield 渠道直方图
//
// 已知：日志里每一趟泵都是 etapa1/2/3 各一次、ret=0，第 6 趟之后彻底静默，
// 而 827CC058/827CC6E0 从未被调用（3916 == 0 跳过那一段）。
// 泵的前置调用 82B506F0（唯一调用者就是泵）与 82B8E128 在 etapas 之前：
// 把它们的进入计数与时间轴打出来，就能回答「第 7 趟到底有没有进泵 / 卡在哪个前置调用」。
// 全程序唯一 yield 点 824F0620（SwitchToThread 包装）的 LR 直方图回答
// 「Game Master 99.4% CPU 的自旋是谁」。
// ==========================================================================
REX_EXTERN(__imp__sub_82B506F0);  // 泵前置调用 1（静态调用者只有泵）
REX_EXTERN(__imp__sub_82B8E128);  // 泵前置调用 2（AV 模块 9 个调用者共用）
REX_EXTERN(__imp__sub_82B8E470);  // 泵尾部调用（AV 模块 9 个调用者共用）
REX_EXTERN(__imp__sub_824F0620);  // SwitchToThread 包装（唯一 yield 点）

REX_HOOK_RAW(sub_82B506F0) {
  using namespace t2rotf::video_nativo;
  const uint64_t n = g_pre1.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 16) {
    REXLOG_ERROR("[video] T2DIAG pre1 sub_82B506F0 n={} r3={:08X} r4={:08X} lr={:08X}", n,
                 ctx.r3.u32, ctx.r4.u32, static_cast<uint32_t>(ctx.lr));
  }
  __imp__sub_82B506F0(ctx, base);
}

REX_HOOK_RAW(sub_82B8E128) {
  using namespace t2rotf::video_nativo;
  const uint64_t n = g_pre2.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 16) {
    REXLOG_ERROR("[video] T2DIAG pre2 sub_82B8E128 n={} r3={:08X} r4={:08X} lr={:08X}", n,
                 ctx.r3.u32, ctx.r4.u32, static_cast<uint32_t>(ctx.lr));
  }
  __imp__sub_82B8E128(ctx, base);
}

REX_HOOK_RAW(sub_82B8E470) {
  using namespace t2rotf::video_nativo;
  const uint64_t n = g_post470.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 16) {
    REXLOG_ERROR("[video] T2DIAG post sub_82B8E470 n={} r3={:08X} r4={:08X} lr={:08X}", n,
                 ctx.r3.u32, ctx.r4.u32, static_cast<uint32_t>(ctx.lr));
  }
  __imp__sub_82B8E470(ctx, base);
}

// ---------------------------------------------------------------------------
// [T29] 显示/呈现驱动链探针（全部只读：计数 + 无条件转发原体）
// ---------------------------------------------------------------------------
// 判据链（生成代码逐字重建，见 T2ROTF_deko3d_显示完成侧代理与kDisplayCtx修正报告.md）：
//   每帧 tick sub_82ABB7D0 → 排空 sub_82882200(idx=*(0x82DA74BC)，base=0x82E55CE0)
//     while (*(base+284) + *(base+288) != 0) sub_82881E00(container=0x82E55E20, idx, 1);
//   → 显示泵 sub_82881E00 → 门 sub_82881B88 就绪 → 交付 sub_82881930 → 显示命令 sub_82881838
//   → 生产/入队 sub_822B5608(r3=L, r4=objA+24 或 base+8) 把通道重新 arm。
// 卡死态（0 fps、有声无画）的预期签名：dtick 与 ddrain 持续涨、ddrain_sucio == ddrain、
// **dpump_in 与 yield 计数同一量级**（排空 while 每转一圈就进一次显示泵、门每次都
// yield 返回 0：实机 yield=4,321,707 与门探针 n=4,000,000 正是这一对），而
// **dpump_ok / ddeliv / dcmd_in / dprod 全 0** —— 即泵在被反复进入却从未交付过一件，
// 生产者也就从未再 arm 通道。这正是宿主必须补足「显示完成侧」的直接证据。
REX_EXTERN(__imp__sub_82ABB7D0);  // 每帧 tick
REX_EXTERN(__imp__sub_82882200);  // 排空（tick 内那次）
REX_EXTERN(__imp__sub_82881E00);  // 显示泵（T25 的 display-tick 候选也调它）
REX_EXTERN(__imp__sub_82881930);  // 交付
REX_EXTERN(__imp__sub_82881838);  // 显示命令（调用回调对象虚方法）
REX_EXTERN(__imp__sub_822B5608);  // 生产者 / 入队

REX_HOOK_RAW(sub_82ABB7D0) {
  using namespace t2rotf::video_nativo;
  g_dtick.fetch_add(1, std::memory_order_relaxed);
  __imp__sub_82ABB7D0(ctx, base);
}

// ===========================================================================
// [T37/v7.6] 显示队列「完成侧」配平（reconcile）：最小、配对、每帧 <= 1 次
// ---------------------------------------------------------------------------
// 定论（T36，逐字读生成代码）：
//   在飞计数 *(base+288)（base = 0x82E55CE0）全程序只有两个增减点：
//     ++ ：交付 sub_82881930（T2ROTF_recomp.12.cpp，`addi r11,r3,288` 那条自增）
//     -- ：泵   sub_82881E00（完成回调返回后无条件自减；idx != -1 时同刻 --*(entry+28)）
//   排空 sub_82882200 的判据 =  while (*(base+284) + *(base+288) != 0) 泵(0x82E55E20, idx, 1);
//   两者本应在同一次泵调用里成对；一次中断/长跳留下的「已交付未完成」会同时留下
//   *(base+288)=1 与 *(entry+28)=1（实机 COLA 读到的 284=0 / 288=1 / e24=0 / e28=1 就是它），
//   而门 sub_82881B88 在 idx != -1 时的就绪判据 *(entry+24)+*(entry+28) > 0 又被这条残留
//   自身满足 => 门每轮报「就绪」、交付每轮弹不到记录、泵报 0、排空无限重旋
//   => 帧循环 0 fps（实机 YIELD 3544 万次，全部 lr=0x82881DE8）。
// ---------------------------------------------------------------------------
// [v9.0 只读化] 本支**不再写任何 guest 显示会计字段**（这是本工程的硬约束）：它只在排空入口
//   （每帧 1 次，绝不落在泵/门热路上）、且判定「无待交付（c24 == 0）而在飞有残留（c28 > 0）」
//   时打一条限频观测日志。楔死态的配平改由 guest 自己的显示管线复位 sub_82882628() 处理
//   （见下方 v9.0-VIVO 区块），因为只有 guest 代码才有权改 284 / 288 / entry+24 / entry+28。
// 关闭（仅关掉这条观测日志）：toml 里 sa2_renut_display_reconcile = false。
// ===========================================================================
REXCVAR_DEFINE_BOOL(sa2_renut_display_reconcile, true, "SA2/Renderer",
                    "[T37/v9.0] READ-ONLY observation at the display-drain entry (sub_82882200, once per frame): "
                    "report the 'already handed over, never completed' residue (nothing left to deliver while an "
                    "in-flight item is still outstanding) as a rate-limited T2DIAG RECON-RO line. The host never "
                    "writes any display accounting field (284/288/entry+24/entry+28); re-aligning the wedge state is "
                    "the guest's own job through sub_82882628 (cvar sa2_renut_display_reinicio).")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// 版本标记（日志里一次）+ 配平计数（限频用）：banner 仍是 v6.27-MAPFIX，用这一行区分实机跑的是哪一版。
std::atomic<bool> g_recon_banner{false};
std::atomic<uint64_t> g_recon_retirados{0};

REX_HOOK_RAW(sub_82882200) {
  using namespace t2rotf::video_nativo;
  const uint64_t n_in = g_ddrain.fetch_add(1, std::memory_order_relaxed) + 1;
  uint8_t* b = reinterpret_cast<uint8_t*>(g_base_host.load(std::memory_order_relaxed));
  if (b) {
    if (!g_recon_banner.exchange(true, std::memory_order_relaxed)) {
      REXLOG_ERROR("[video] T2ROTF probe v7.6-RECON: display-completion reconcile at the drain entry");
    }
    const uint32_t cola = 0x82E55CE0u;
    const uint32_t idx = Leer32(b, 0x82DA74BCu);
    const uint32_t c24 = Leer32(b, cola + 284);
    const uint32_t c28 = Leer32(b, cola + 288);
    uint32_t e = 0;
    uint32_t e24 = c24;
    uint32_t e28 = c28;
    if (idx != 0xFFFFFFFFu) {
      const uint32_t tabla = Leer32(b, cola + 272);
      e = (tabla && idx < 64u) ? (tabla + idx * 32u) : 0u;
      if (e) {
        e24 = Leer32(b, e + 24);
        e28 = Leer32(b, e + 28);
      }
    }
    if (e24 + e28) {
      g_ddrain_sucio.fetch_add(1, std::memory_order_relaxed);
    }
    // [T37 / v9.0 只读化] 完成侧残留观测：**纯只读**。历史版本的这一支曾按「无待交付而在飞有
    // 残留」在宿主侧把 *(base+288) / *(entry+28) 减掉来配平，那属于直接写 guest 的显示会计字段
    // （违反本工程的硬约束 M9），且在本轮 v8.9 实机里从未命中（wedge 态 e24 恒 1）。现在这里只
    // 计数/打点，配平改由 v9.0-VIVO 交给 **guest 自己的**显示管线复位 sub_82882628() 去做。
    uint32_t retirado = 0;
    if (REXCVAR_GET(sa2_renut_display_reconcile) && e24 == 0u && e28 > 0u) {
      retirado = (e28 > 64u) ? 64u : e28;
      const uint64_t nr = g_recon_retirados.fetch_add(1, std::memory_order_relaxed) + 1;
      if (nr <= 24u || (nr % 256u) == 0u) {
        REXLOG_ERROR(
            "[video] T2DIAG RECON-RO nr={} n={} idx={} c24={} c28={} e28={} (read-only: no host write to any "
            "accounting field; wedge handling belongs to sub_82882628)",
            nr, n_in, idx, c24, c28, e28);
      }
    } else if (n_in <= 24u || (n_in % 8192u) == 0u) {
      REXLOG_ERROR("[video] T2DIAG DRAIN-IN n={} idx={} c24={} c28={} e24={} e28={}", n_in, idx, c24, c28, e24,
                   e28);
    }
  }
  __imp__sub_82882200(ctx, base);
}

// [T72/v9.0-VIVO] guest 自己的显示管线复位入口（无参数；只在「门楔死」判据连续命中时调一次）。
REX_EXTERN(__imp__sub_82882628);

// [T72/v9.0-VIVO] 楔死态复位的判据与调用体放在下方的 v9.0-VIVO 区块里（那里才看得到 cvar、
// 计数器与影子别名助手）。这里只按「泵进入」转发一次，绝不写任何显示会计字段。
void T2ReinicioSiAtascado(PPCContext& ctx, uint8_t* base);

REX_HOOK_RAW(sub_82881E00) {
  using namespace t2rotf::video_nativo;
  g_dpump_in.fetch_add(1, std::memory_order_relaxed);
  T2ReinicioSiAtascado(ctx, base);
  __imp__sub_82881E00(ctx, base);
  g_dpump_out.fetch_add(1, std::memory_order_relaxed);
  if (ctx.r3.u32) {
    g_dpump_ok.fetch_add(1, std::memory_order_relaxed);
  }
}

REX_HOOK_RAW(sub_82881930) {
  using namespace t2rotf::video_nativo;
  g_ddeliv.fetch_add(1, std::memory_order_relaxed);
  __imp__sub_82881930(ctx, base);
}

REX_HOOK_RAW(sub_82881838) {
  using namespace t2rotf::video_nativo;
  g_dcmd_in.fetch_add(1, std::memory_order_relaxed);
  __imp__sub_82881838(ctx, base);
  g_dcmd_out.fetch_add(1, std::memory_order_relaxed);
}

// ===========================================================================
// [T72 / v9.0-VIVO] 显示投递通道归一：把「生产者 arm/入队」与「门就绪读/交付弹」钉到
// **活体显示设备自己的通道**上，并补回该设备缺的那一个管理器自指。
//
// v8.9 实机取证（`[T2DIAG PUERTA n=… r30=41770018 st0=0 s16=0 …]`，n 打到 4e6 仍不变）：
//   门 sub_82881B88 的上下文 r30 是**运行期堆对象**（活体显示设备，实机 0x41770018），
//   它的 *(r30+16) == 0 ⇒ 门的就绪读退化成 *(0+272+4*slot) 恒 0（实机三槽 cnt=0/0/0）
//   ⇒ 门永不就绪 ⇒ 交付 0 次 ⇒ 排空 while(*(0x82E55CE0+284)+*(0x82E55CE0+288)!=0) 永久自旋
//   ⇒ 帧循环空转、画面不出。生产者侧又被 T41 改写到静态管理器通道 0x82E55E38，两侧分叉。
//
// 修复语义（**不写任何显示会计字段** 284/288/16548/16552/16564/16568）：
//   1) 记录活体显示设备基址 dev（生产者 sub_822B5608 的 r4 通道基 - 24，实机 0x41770018）；
//   2) 把它的 +16 按 init 语义修成自指（与静态管理器 0x82E55E20 的 *(M+16)=M 同构）——
//      这是**链接字段**而非会计；guest 访存一律走 RexGmShadowFor 影子别名，取不到就跳过并计数；
//   3) 把生产者的投递通道从静态 0x82E55E38 改写到该设备自己的通道（dev+24，实机 0x41770030）：
//        生产者 ++*(dev+24+248)=dev+272 ／ 入队 *(dev+24+8)=dev+32
//        门     *(*(dev+16)+272+4*slot) = dev+272     （自指后与上行同址）
//        交付   弹 *(*(dev+16)+24)+slot*80+8 = dev+32 （自指后与上行同址；若按 r3+24 取址同样为 dev+32）
//      ——两种取址写法都落在同一对地址上，不存在「弹到别的队列、回调对象成垃圾」的崩溃风险。
//   4) 楔死态（284==0 且 288>0 且两侧六槽 arm 标记全 0）连续 kReinicioTics 次后，调用 guest 自己
//      的显示管线复位 sub_82882628() 一次（总次数封顶 8），让 284/288 由 guest 代码归零，
//      而不是宿主直写字段。
// 关闭：toml 里 sa2_renut_display_vivo = false / sa2_renut_display_reinicio = false。
// ===========================================================================
extern "C" void* RexGmShadowFor(uint64_t window_address);

// guest 访存的安全通道（[v8.2-SHADOW] 纪律）：先取该地址的 shadow 别名，再按 guest 字节序读写。
// 取不到别名（未提交 / 不属于 guest 物理内存）即返回 false，调用方自行跳过，绝不直解引用。
inline uint8_t* AliasGuest(uint8_t* base, uint32_t va) {
  if (!base) {
    return nullptr;
  }
#if defined(__SWITCH__)
  return reinterpret_cast<uint8_t*>(RexGmShadowFor(reinterpret_cast<uint64_t>(base) + va));
#else
  return base + va;
#endif
}

inline bool Leer32Alias(uint8_t* base, uint32_t va, uint32_t* fuera) {
  uint8_t* p = AliasGuest(base, va);
  if (!p) {
    return false;
  }
  *fuera = __builtin_bswap32(*reinterpret_cast<const uint32_t*>(p));
  return true;
}

inline bool Escribir32Alias(uint8_t* base, uint32_t va, uint32_t valor) {
  uint8_t* p = AliasGuest(base, va);
  if (!p) {
    return false;
  }
  *reinterpret_cast<uint32_t*>(p) = __builtin_bswap32(valor);
  return true;
}

REXCVAR_DEFINE_BOOL(sa2_renut_display_vivo, true, "SA2/Renderer",
                    "[T72] Pin the display post channel to the live display device's own channel "
                    "(dev+24) and repair its missing manager self-link (*(dev+16)=dev), so the "
                    "producer's ++*(A+248)/push(A+8) and the gate's ready-read/pop land on one and "
                    "the same object instead of drifting between the static manager 0x82E55E20 and "
                    "the runtime device object. Host code stays read-only on every accounting field.")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(sa2_renut_display_reinicio, true, "SA2/Renderer",
                    "[T72] When the display drain is provably wedged (284==0, 288>0 and all six arm "
                    "markers zero for kReinicioTics pump entries), call the guest's own display "
                    "pipeline re-init sub_82882628() once (capped at 8 times) so the counters are "
                    "cleared by guest code instead of by host writes.")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

std::atomic<uint32_t> g_disp_vivo{0};        // 活体显示设备基址（= 投递通道基 - 24）
std::atomic<uint64_t> g_vivo_fix{0};         // +16 自指修复次数
std::atomic<uint64_t> g_vivo_sin_shadow{0};  // 取不到 shadow 别名而跳过的次数
std::atomic<uint64_t> g_reinicio_n{0};       // 连续楔死观测计数
std::atomic<uint64_t> g_reinicio_ok{0};      // 实际调用 guest 复位次数
std::atomic<bool> g_reinicio_banner{false};

// 楔死判据：排空等待的那一对（全局呈现队列 0x82E55CE0 的 +284 / +288）满足「无待交付
// （284==0）而在飞有残留（288>0）」，且两侧六槽 arm 标记全 0 —— 此时没有任何一件可被交付，
// 残留的在飞项永远退不掉（v8.9 实机末态即如此：284 停在 1 被 retire 回 0、288 仍在 1）。
// 连续 kReinicioTics 次「泵进入」命中后，交给 guest 自己的显示管线复位 sub_82882628() 处置
// （总次数封顶 8 次），由 guest 代码把 284/288 归零，宿主绝不直写这些会计字段。
constexpr uint64_t kReinicioTics = 120u;

void T2ReinicioSiAtascado(PPCContext& ctx, uint8_t* base) {
  using t2rotf::video_nativo::g_base_host;
  using t2rotf::video_nativo::Leer32;
  if (!REXCVAR_GET(sa2_renut_display_reinicio)) {
    return;
  }
  uint8_t* b = reinterpret_cast<uint8_t*>(g_base_host.load(std::memory_order_relaxed));
  if (!b) {
    return;
  }
  const uint32_t c284 = Leer32(b, 0x82E55CE0u + 284u);
  const uint32_t c288 = Leer32(b, 0x82E55CE0u + 288u);
  const uint32_t dev = g_disp_vivo.load(std::memory_order_relaxed);
  bool armado = false;
  for (uint32_t s = 0; s < 3u && !armado; ++s) {
    armado = armado || Leer32(b, 0x82E55CE0u + 256u + s * 4u) != 0u;
    armado = armado || Leer32(b, 0x82E55E38u + 248u + s * 4u) != 0u;
    if (dev) {
      uint32_t v = 0u;
      if (Leer32Alias(b, dev + 272u + s * 4u, &v)) {
        armado = armado || v != 0u;
      }
    }
  }
  if (!(c284 == 0u && c288 > 0u && !armado)) {
    g_reinicio_n.store(0, std::memory_order_relaxed);
    return;
  }
  const uint64_t nb = g_reinicio_n.fetch_add(1, std::memory_order_relaxed) + 1;
  if (nb < kReinicioTics) {
    return;
  }
  g_reinicio_n.store(0, std::memory_order_relaxed);
  const uint64_t hechos = g_reinicio_ok.fetch_add(1, std::memory_order_relaxed) + 1;
  if (hechos > 8u) {
    if (!g_reinicio_banner.exchange(true, std::memory_order_relaxed)) {
      REXLOG_ERROR("[video] T2DIAG REINICIO topado: guest display re-init budget (8) exhausted");
    }
    return;
  }
  REXLOG_ERROR(
      "[video] T2DIAG REINICIO n={} c284={} c288={} cont={:08X} dev={:08X} hechos={} "
      "(guest display pipeline re-init sub_82882628)",
      nb, c284, c288, ctx.r3.u32, dev, hechos);
  // 用 ctx 的副本进 guest，绝不污染正在转发给泵原体的这一份（该调用无参数）。
  PPCContext tctx = ctx;
  __imp__sub_82882628(tctx, base);
}

// ===========================================================================
// [T41 / v7.7-ALIGN] 投递通道对齐：唯一真因修复（不写任何会计、不落门/泵热路）
//
// 逐条读生成代码得到的取证链：
//   producer sub_822B5608(r3=obj, r4=A)：++*(A+248)（通道 arm 计数）、
//     sub_822B57E0(A+8, obj)（把对象挂进 A+8 处的通道队列）；另外只动
//     *(entry+24)（entry = *(0x82E55CE0+272) + *(obj+64)*32）与 *(0x82E55CE0+284)。
//   消费者链恒定围绕显示对象 0x82E55E20：
//     门 sub_82881B88 的就绪读   *(*(0x82E55E20+16) + 272 + 4*slot)；
//     交付 sub_82881930(ctx=0x82E55E20+24) 弹 ctx + slot*80 + 8，
//       并从 *(ctx) + 248 + 4*slot 扣减。
//   => 只有当投递的 **A == 0x82E55E20+24** 时，arm 计数与队列才与消费者的读/弹位置重合。
//   而每帧 tick 用的是 sub_828823F8() 从 *(0x82DBB5C4) 数组里解析出的“当前呈现设备”
//   （+0 状态 == 5 的那一个，≠ 0x82E55E20），arm 与队列全部落到那个设备的通道上：
//     门读的计数恒 0 / 后备标记恒 0  ->  门永不就绪  ->  交付弹不到记录  ->  泵 0 次成功
//     ->  排空 while (*(0x82E55CE0+284) + *(0x82E55CE0+288) != 0) 永久自旋。
//   实机对上了：PUERTA cnt=0/0/0、obj256=0/0/0、cola284=1/cola288=0；
//   PERRO2 dpump=16591560/0/16591558（0 次成功）、ddeliv=6、dprod=7；0 fps、CPU 98~115%。
//
// 修复语义：只把 producer 的通道基址改写成消费者侧读/弹的同一通道 (0x82E55E20+24)，
//   并保证管理器自身的当前设备指针为自指（与 init 语义一致）。不加不减任何会计，
//   不碰 284 / 288 / entry+24 / entry+28。
// 关闭：toml 里 sa2_renut_display_align = false（或 SA2_RENUT_DISPLAY_ALIGN=0）。
// ===========================================================================
REXCVAR_DEFINE_BOOL(sa2_renut_display_align, true, "SA2/Renderer",
                    "[T41] retarget every display post to the channel the consumer side actually drains "
                    "(*(0x82E55E20+16)+24), so the gate's arm counter and the delivery's slot queue line up "
                    "with the producer's ++*(A+248) and push(A+8). Without it the posts land on a channel "
                    "nobody drains, the gate never becomes ready, the drain spins forever and the frame loop "
                    "stays at 0 fps (black screen).")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

std::atomic<bool> g_align_banner{false};
std::atomic<uint64_t> g_align_fix{0};
std::atomic<uint64_t> g_align_self{0};

REX_HOOK_RAW(sub_822B5608) {
  using namespace t2rotf::video_nativo;
  g_dprod.fetch_add(1, std::memory_order_relaxed);
  uint8_t* b = reinterpret_cast<uint8_t*>(g_base_host.load(std::memory_order_relaxed));
  // [T72/v9.0-VIVO] 记录活体显示设备基址，并补回它缺的 +16 管理器自指（链接字段，非会计；
  // guest 访存一律走 RexGmShadowFor 影子别名，取不到就跳过并计数，绝不直解引用窗口地址）。
  if (b && REXCVAR_GET(sa2_renut_display_vivo)) {
    const uint32_t a_antes = ctx.r4.u32;
    if (a_antes >= 0x40000000u && a_antes < 0x82000000u) {
      const uint32_t dev = a_antes - 24u;
      g_disp_vivo.store(dev, std::memory_order_relaxed);
      uint32_t enl = 0u;
      if (!Leer32Alias(b, dev + 16u, &enl)) {
        g_vivo_sin_shadow.fetch_add(1, std::memory_order_relaxed);
      } else if (enl != dev) {
        uint32_t c0 = 0u;
        if (!Leer32Alias(b, dev, &c0)) {
          c0 = 0xDEAD0000u;
        }
        const uint64_t nv = g_vivo_fix.fetch_add(1, std::memory_order_relaxed) + 1;
        if (nv <= 4u || (nv % 1024u) == 0u) {
          REXLOG_ERROR("[video] T2DIAG VIVO-LINK n={} dev={:08X} c0={:08X} c16_antes={:08X}", nv, dev, c0, enl);
        }
        if (!Escribir32Alias(b, dev + 16u, dev)) {
          g_vivo_sin_shadow.fetch_add(1, std::memory_order_relaxed);
        }
      }
    }
  }
  if (b && REXCVAR_GET(sa2_renut_display_align)) {
    const uint32_t cont = 0x82E55E20u;
    if (!g_align_banner.exchange(true, std::memory_order_relaxed)) {
      REXLOG_ERROR("[video] T2ROTF probe: post channel pinned to the live display device (v9.0-VIVO selftest)");
    }
    // 管理器自身的“当前设备/当前通道”必须是它自己（init 语义）；被改过就修回自指，
    // 否则门的读位置与交付的弹位置会分叉。
    if (Leer32(b, cont + 16u) != cont || Leer32(b, cont + 24u) != (cont + 24u)) {
      const uint64_t ns = g_align_self.fetch_add(1, std::memory_order_relaxed) + 1;
      if (ns <= 8u) {
        REXLOG_ERROR("[video] T2DIAG ALIGN-SELF n={} c16={:08X} c24={:08X}", ns, Leer32(b, cont + 16u),
                     Leer32(b, cont + 24u));
      }
      Escribir32(b, cont + 16u, cont);
      Escribir32(b, cont + 24u, cont + 24u);
    }
    // [T72/v9.0-VIVO] 通道目标：优先钉到活体设备自己的通道（dev+24）；设备未知时退回静态 0x82E55E38。
    uint32_t chbase = cont + 24u;
    if (REXCVAR_GET(sa2_renut_display_vivo)) {
      const uint32_t dev = g_disp_vivo.load(std::memory_order_relaxed);
      if (dev) {
        chbase = dev + 24u;
      }
    }
    const uint32_t A = ctx.r4.u32;
    if (A != chbase) {
      const uint32_t obj = ctx.r3.u32;
      const uint64_t n = g_align_fix.fetch_add(1, std::memory_order_relaxed) + 1;
      if (n <= 16u || (n % 512u) == 0u) {
        REXLOG_ERROR(
            "[video] T2DIAG ALIGN n={} a_antes={:08X} dev_antes={:08X} a_ahora={:08X} obj={:08X} "
            "o64={} g74bc={} c272={} c284={} c288={} b256={}",
            n, A, A - 24u, chbase, obj, Leer32(b, obj + 64u), Leer32(b, 0x82DA74BCu),
            Leer32(b, chbase + 248u), Leer32(b, cont + 284u), Leer32(b, cont + 288u),
            Leer32(b, 0x82E55CE0u + 256u));
      }
      ctx.r4.u32 = chbase;
    }
  }
  __imp__sub_822B5608(ctx, base);
}

// ===========================================================================
// [T25] 呈现门探针：门 sub_82881B88 的唯一 yield 点（lr = 0x82881DE8）。
//
// 门在等三组东西（逐条取证见 T2ROTF_deko3d_呈现门阻塞语句与显示会计核对报告.md）：
//   1) 显示管理器对象（门的上下文 r30，即 sub_82881E00 的 r3，疑为 0x82E55EE0）
//      的 +0 状态码、+16→+272/+276/+280 三槽计数、+288 计数、+24 条目指针；
//   2) 全局呈现队列 0x82E55CE0 族的记账：+0 指向的管理对象其 +256/+260/+264 三槽、
//      +272 条目数组、条目 +24/+28；以及队列自身 +272/+284/+288；
//   3) 四个时间窗全局：0x82DBB688(u64) / 0x82DBB5D4(u32) / 0x82E55F50(u64) / 0x82E55F48(u64)。
//
// 宿主 Swap 钩子推进的是设备块 +16548/+16552/+16564/+16568、*(dev+10900)+4、+10944，
// 与门的判据零交集——所以把上面这几组值整组打出来，就能直接回答「三槽为何全不就绪、
// 谁该去 arm 它们」。全程只读，不改 guest 状态；打印限频。
// ===========================================================================
uint32_t LeerSeguro(uint8_t* base, uint32_t addr) {
  // 只解引用镜像段（0x82000000..0x83FFFFFF）里的地址：指针字段若是垃圾值就返回 0，
  // 不让探针自己把进程打崩。
  if (base == nullptr || addr < 0x82000000u || addr >= 0x84000000u) {
    return 0u;
  }
  return t2rotf::video_nativo::Leer32(base, addr);
}

void ProbarPuerta(uint8_t* base, PPCContext& ctx, uint64_t n) {
  const uint32_t puerta = ctx.r30.u32;  // 门的上下文 r30（门 94805 的 r30）
  const uint32_t suma = ctx.r27.u32;    // 门的 r27 = arg（原 r4）
  const uint32_t cola = 0x82E55CE0u;    // 门 94828 的 r26：全局呈现队列基址
  const uint32_t obj = LeerSeguro(base, cola);       // *(cola+0)：+272 条目数组的宿主对象
  const uint32_t arreglo = LeerSeguro(base, cola + 272);  // cola+272：条目数组
  const uint32_t base16 = LeerSeguro(base, puerta + 16);
  const auto L64 = [base](uint32_t a) -> uint64_t {
    return (uint64_t(LeerSeguro(base, a)) << 32) | LeerSeguro(base, a + 4);
  };
  REXLOG_ERROR(
      "[video] T2DIAG PUERTA n={} r30={:08X} arg={:08X} st0={} s16={:08X} cnt={}/{}/{} v288={} "
      "cola0={:08X} cola272={:08X} cola284={} cola288={} obj256={}/{}/{} obj272={:08X} "
      "tb={} tmin={} tmax={} tcnt={}",
      n, puerta, suma, LeerSeguro(base, puerta), base16, LeerSeguro(base, base16 + 272),
      LeerSeguro(base, base16 + 276), LeerSeguro(base, base16 + 280), LeerSeguro(base, puerta + 288),
      obj, arreglo, LeerSeguro(base, cola + 284), LeerSeguro(base, cola + 288),
      LeerSeguro(base, obj + 256), LeerSeguro(base, obj + 260), LeerSeguro(base, obj + 264),
      LeerSeguro(base, obj + 272), L64(0x82DBB688u), L64(0x82E55F50u), L64(0x82E55F48u),
      LeerSeguro(base, 0x82DBB5D4u));
}

// sub_824F0620：全程序唯一的 NtYieldExecution 包装（8 个静态调用点，其中 4 个在
// 影片/AV 模块、3 个在锁/线程族）。逐次打日志会淹掉日志，所以只记 LR 直方图。
REX_HOOK_RAW(sub_824F0620) {
  using namespace t2rotf::video_nativo;
  g_yield_total.fetch_add(1, std::memory_order_relaxed);
  const uint32_t lr = static_cast<uint32_t>(ctx.lr);
  const uint32_t ranura = (lr >> 4) & 1023u;
  const uint64_t veces = g_yield_lr_n[ranura].fetch_add(1, std::memory_order_relaxed) + 1;
  g_yield_lr[ranura].store(lr, std::memory_order_relaxed);
  // [T25] 门 sub_82881B88 的 yield（lr=0x82881DE8）：限频（前 32 次 + 以后每 4,000,000 次）
  // 打出三组判据字段，实机跑一次即可定三槽为何全不就绪。
  if (lr == 0x82881DE8u && (veces <= 32 || (veces % 4000000ull) == 0)) {
    ProbarPuerta(base, ctx, veces);
  }
  __imp__sub_824F0620(ctx, base);
}

namespace t2rotf::video_nativo {
namespace {

void VolcarGuest(const char* etiqueta) {
  const uint64_t base_u = g_base_host.load(std::memory_order_relaxed);
  const uint8_t* base = reinterpret_cast<const uint8_t*>(base_u);
  REXLOG_ERROR(
      "[video] T2DIAG PERRO {} pumpIn={} pumpOut={} pre1={} pre2={} post470={} prep={} "
      "cadena937={} cadena9bf={} despacho={} etapa1/2/3={}/{}/{} fin={}/{}/{} bomba={} iny={} pres={} "
      "anillo={}/{} externo={} irq47={} irq95={} cc058={} cc6e0={} yield={}",
      etiqueta, g_pump_entrada.load(std::memory_order_relaxed),
      g_pump_salida.load(std::memory_order_relaxed), g_pre1.load(std::memory_order_relaxed),
      g_pre2.load(std::memory_order_relaxed), g_post470.load(std::memory_order_relaxed),
      g_prep.load(std::memory_order_relaxed), g_cadena937.load(std::memory_order_relaxed),
      g_cadena9bf.load(std::memory_order_relaxed), g_despacho.load(std::memory_order_relaxed),
      g_cg.etapa1.load(std::memory_order_relaxed), g_cg.etapa2.load(std::memory_order_relaxed),
      g_cg.etapa3.load(std::memory_order_relaxed), g_cg.etapa1_fin.load(std::memory_order_relaxed),
      g_cg.etapa2_fin.load(std::memory_order_relaxed),
      g_cg.etapa3_fin.load(std::memory_order_relaxed), g_bomba_ticks, g_inyectados, g_pres_ticks,
      g_cg.anillo.load(std::memory_order_relaxed),
      g_cg.anillo_espera.load(std::memory_order_relaxed),
      g_cg.externo.load(std::memory_order_relaxed), g_cg.irq47.load(std::memory_order_relaxed),
      g_cg.irq95.load(std::memory_order_relaxed), g_cg.cc058.load(std::memory_order_relaxed),
      g_cg.cc6e0.load(std::memory_order_relaxed), g_yield_total.load(std::memory_order_relaxed));
  // [T29] 显示驱动链一行：dtick / ddrain(sucio) / dpump(in ok out) / ddeliv / dcmd(in out) / dprod。
  // 这是「呈现驱动链在实机是否运行」的直接读数：卡死态 dtick、ddrain、dpump_in 一起涨
  // （泵被反复进入但每次都被门拦下，与 yield 计数同阶），而 ok/ddeliv/dcmd/dprod 全 0。
  REXLOG_ERROR(
      "[video] T2DIAG PERRO2 {} dtick={} ddrain={}/{} dpump={}/{}/{} ddeliv={} dcmd={}/{} dprod={}",
      etiqueta, g_dtick.load(std::memory_order_relaxed), g_ddrain_sucio.load(std::memory_order_relaxed),
      g_ddrain.load(std::memory_order_relaxed), g_dpump_in.load(std::memory_order_relaxed),
      g_dpump_ok.load(std::memory_order_relaxed), g_dpump_out.load(std::memory_order_relaxed),
      g_ddeliv.load(std::memory_order_relaxed), g_dcmd_in.load(std::memory_order_relaxed),
      g_dcmd_out.load(std::memory_order_relaxed), g_dprod.load(std::memory_order_relaxed));
  if (base) {
    const uint32_t ctx = g_ctx_visto.load(std::memory_order_relaxed);
    // [v8.7-IRQABI] guest 地址先校验再读，绝不裸读未映射地址（启动即退出的根因）。
    if (EsDireccionDispositivo(ctx)) {
      REXLOG_ERROR(
          "[video] T2DIAG PERRO ctx={:08X} st15504={} g3912={} g3916={} g3924={} v15592={} "
          "v15596={} v15568={} v14856={} v15228={} v16004={} pump15808={:08X} "
          "disY={:08X} disU={:08X} disV={:08X} off220={} off224={}",
          ctx, Leer32(const_cast<uint8_t*>(base), ctx + 15504),
          Leer32(const_cast<uint8_t*>(base), ctx + 3912),
          Leer32(const_cast<uint8_t*>(base), ctx + 3916),
          Leer32(const_cast<uint8_t*>(base), ctx + 3924),
          Leer32(const_cast<uint8_t*>(base), ctx + 15592),
          Leer32(const_cast<uint8_t*>(base), ctx + 15596),
          Leer32(const_cast<uint8_t*>(base), ctx + 15568),
          Leer32(const_cast<uint8_t*>(base), ctx + 14856),
          Leer32(const_cast<uint8_t*>(base), ctx + 15228),
          Leer32(const_cast<uint8_t*>(base), ctx + 16004),
          Leer32(const_cast<uint8_t*>(base), ctx + 15808),
          Leer32(const_cast<uint8_t*>(base), ctx + 3744),
          Leer32(const_cast<uint8_t*>(base), ctx + 3748),
          Leer32(const_cast<uint8_t*>(base), ctx + 3752),
          Leer32(const_cast<uint8_t*>(base), ctx + 220),
          Leer32(const_cast<uint8_t*>(base), ctx + 224));
    }
    const uint32_t dev = g_dev_visto.load(std::memory_order_relaxed);
    // [v8.7-IRQABI] dev 是 guest 设备地址，读前必须校验；不健全即跳过并计数，
    // 返回 0 让上层不再裸读（此前 dev 被误填成来源值 1，dev+10896=0x2A91 未映射 → 致命退出）。
    if (EsDireccionDispositivo(dev)) {
      auto L = [base](uint32_t a) { return Leer32(const_cast<uint8_t*>(base), a); };
      const uint32_t f10896 = L(dev + 10896);
      const uint32_t f10900 = L(dev + 10900);
      const uint32_t b10896 = EsDireccionDispositivo(f10896) ? f10896 : 0;
      const uint32_t b10900 = EsDireccionDispositivo(f10900) ? f10900 : 0;
      REXLOG_ERROR(
          "[video] T2DIAG PERRO dev={:08X} vblank16548={} tick16552={} sub16560={} fr16564={} "
          "disp16568={} ringR16700={} ringW16704={} v23376={} f10940={:08X} f10941={:08X} "
          "f10942={:08X} f10944={:08X} inKick13232={} b10896={:08X} b10900={:08X} "
          "w10900_0={:08X} w10900_4={:08X} w10896_0={:08X}",
          dev, L(dev + 16548), L(dev + 16552), L(dev + 16560), L(dev + 16564), L(dev + 16568),
          L(dev + 16700), L(dev + 16704), L(dev + 23376), L(dev + 10940), L(dev + 10941),

          L(dev + 10942), L(dev + 10944), L(dev + 13232), b10896, b10900,
          b10900 ? L(b10900) : 0, b10900 ? L(b10900 + 4) : 0, b10896 ? L(b10896) : 0);
    } else if (dev) {
      // 地址不健全：只报一行、绝不裸读，避免再触发 xmemory 致命路径。
      const uint64_t s = g_perro_saltos.fetch_add(1, std::memory_order_relaxed) + 1;
      if (s <= 2) {
        REXLOG_ERROR(
            "[video] T2DIAG PERRO {} dev={:08X} 不是设备地址（守卫跳过，不再裸读 guest；saltos={}）",
            etiqueta, dev, s);
      }
    }
  }
  uint32_t top_lr[6] = {}, top_n[6] = {};
  for (uint32_t i = 0; i < 1024; ++i) {
    const uint32_t c = g_yield_lr_n[i].load(std::memory_order_relaxed);
    if (!c) {
      continue;
    }
    for (int k = 0; k < 6; ++k) {
      if (c > top_n[k]) {
        for (int j = 5; j > k; --j) {
          top_n[j] = top_n[j - 1];
          top_lr[j] = top_lr[j - 1];
        }
        top_n[k] = c;
        top_lr[k] = g_yield_lr[i].load(std::memory_order_relaxed);
        break;
      }
    }
  }
  REXLOG_ERROR(
      "[video] T2DIAG YIELD total={} top=[{:08X}x{}] [{:08X}x{}] [{:08X}x{}] [{:08X}x{}] "
      "[{:08X}x{}] [{:08X}x{}]",
      g_yield_total.load(std::memory_order_relaxed), top_lr[0], top_n[0], top_lr[1], top_n[1],
      top_lr[2], top_n[2], top_lr[3], top_n[3], top_lr[4], top_n[4], top_lr[5], top_n[5]);
}

void BuclePerro() {
  std::this_thread::sleep_for(std::chrono::seconds(3));
  VolcarGuest("perro");
  for (;;) {
    std::this_thread::sleep_for(std::chrono::seconds(2));
    VolcarGuest("perro");
  }
}

void ArrancarPerro() {
  if (g_perro_arrancado.exchange(1, std::memory_order_acq_rel) != 0) {
    return;
  }
  // Switch 上 std::thread::detach() 会抛异常（banjo_renut_tuning.h:96 的既有结论，
  // 也是 package 24 的崩溃来源），所以照工程惯用法用 pthread 起一个永不 join 的线程。
#if REX_PLATFORM_SWITCH
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 0x10000);
  pthread_t thread;
  const int error = pthread_create(
      &thread, &attr, [](void *) -> void * {
        BuclePerro();
        return nullptr;
      },
      nullptr);
  pthread_attr_destroy(&attr);
  if (error) {
    REXLOG_ERROR("[video] T2DIAG PERRO pthread_create fallo ({})", error);
    new std::thread(BuclePerro);  // 不 join、不销毁（销毁一个可 join 的 std::thread 会 terminate）
  }
#else
  new std::thread(BuclePerro);
#endif
  REXLOG_ERROR("[video] T2DIAG PERRO hilo lanzado (cada 2 s)");
}

}  // namespace
}  // namespace t2rotf::video_nativo
