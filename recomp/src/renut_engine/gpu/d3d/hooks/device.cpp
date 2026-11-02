/**
 * @file    gpu/d3d/hooks/device.cpp
 * @brief   Device lifecycle hooks: creation and release, the command ring,
 *          fences, command buffer recording and Swap.
 *
 * The command ring is a stub, as in reblue: m_pRing points at a scratch block
 * that KickOff simply rewinds, and none of the hooked D3D calls writes packets
 * (d3d_native.h). The renderer consumes D3D's API-level state instead (the
 * device's constant/packet shadows and the hooked state-setting and draw calls).
 *
 * The renderer runs on its own thread (render_thread.h). KickOff and Swap
 * queue the end of a batch of work and its fence, which completes once the
 * render thread has read the batch's data; D3D's fence waits wait for that
 * (BlockOnFence_hook), and Swap waits when the game gets a frame too far ahead.
 */
#include <rex/frame_latency.h>  // banjo-nx (2026-10-19)
#include "renut_engine/display_sync.h"
#include "renut_engine/gpu/render/pipeline.h"  // banjo-nx (2026-10-20): SyncPipelineListLocked
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <rex/hook.h>

#include "renut_engine/gpu/banjo_renut_tuning.h"  // banjo-nx: CountFenceQuery
#include "renut_engine/gpu/banjo_renut_checks.h"  // banjo-nx (package 27c): QueueKickFlush
#include "renut_engine/gpu/banjo_renut_select.h"
#include "renut_engine/gpu/sa2_renut_pacing.h"  // sa2-nx
#include "sa2_capture.h"
#include "renut_engine/gpu/banjo_renut_capture.h"  // sa2-nx: lockstep captures
#include "me/sa2_render_mode.h"                     // sa2-nx (1.0.3.1): the PC's docked test toggles

#include <rex/cvar.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/ppc.h>
#include <rex/system/xvideo.h>
#include <rex/types.h>

#include "renut_engine/perf/frame_stats.h"
#include "renut_engine/gpu/guest/callbacks.h"
#include "renut_engine/gpu/guest/command_buffers.h"
#include "renut_engine/gpu/d3d/d3d.h"
#include "renut_engine/gpu/d3d/d3d_native.h"
#include "renut_engine/gpu/d3d/t2_hook_map.h"  // T2ROTF (2026-10-25): 249 条 T2ROTF 自己的守卫
#include "renut_engine/gpu/render/draw.h"
#include "renut_engine/gpu/guest/guest_memory.h"
#include "renut_engine/gpu/guest/guest_records.h"
#include "renut_engine/gpu/log.h"
#include "renut_engine/gpu/backend/memory_report.h"
#include "renut_engine/gpu/backend/queued.h"
#include "renut_engine/gpu/backend/render_thread.h"
#include "renut_engine/gpu/render/surfaces.h"
#include "renut_engine/gpu/render/textures.h"
#include "renut_engine/gpu/backend/video.h"

using namespace renut::gpu;

// sa2-nx: frames the game has presented (src/sa2_d3d_trace.cpp; the test recorder waits on it).
extern std::atomic<uint64_t> g_sa2_game_frames;
REXCVAR_DECLARE(bool, sa2_renut_gamma_ramp);  // sa2-nx (2026-10-20): backend/present.cpp
// sa2-nx (2026-10-20, sa2_renut_core_layout): the SDK's core layout (system/xthread.cpp).
extern "C" uint32_t RexSetGuestThreadCoreLayout(int main_core, uint32_t others_mask, int others_preferred);

// Kernel exports the device code uses.
REX_IMPORT(__imp__rex_GetCurrentThreadId, GuestGetCurrentThreadId, u32());
REX_IMPORT(__imp__RtlInitializeCriticalSection, RtlInitializeCriticalSection, void(u32));
REX_IMPORT(__imp__KeQueryPerformanceFrequency, KeQueryPerformanceFrequency, u32());
REX_IMPORT(__imp__KeGetCurrentProcessType, KeGetCurrentProcessType, u32());
REX_IMPORT(__imp__VdGetCurrentDisplayGamma, VdGetCurrentDisplayGamma, void(u32, u32));

namespace {

// Scratch space packets go to: the inline writers in N&B's own code (D3D's
// header helpers), which reserve against m_pRingGuarantee. Large enough for
// the biggest single reservation; the guarantee margin is what inline writers
// assume is free without calling KickOff.
constexpr u32 kRingSinkSize = 4u << 20;
constexpr u32 kRingSinkGuaranteeMargin = 256u << 10;
// Scratch handed out for inline data (RingBufferDeviceAllocate), reused in a
// circle: nothing reads it either.
constexpr u32 kRingDataSize = 1u << 20;

// CDevice-private state Direct3D_CreateDevice (0x8264DF28) sets up, as byte
// offsets into the device allocation. sa2-nx (2026-10-20): SA2's (d3d.h),
// read from its Direct3D_CreateDevice sub_826B2418, sub_8255C808, sub_8255C878
// and the hardware path sub_8255CDA0; N&B's in brackets.
// Byte whose 0x80 bit marks a device without GPU hardware (its device-type-2
// path sets it); D3D's code skips hardware work for it.
constexpr u32 kDeviceOffsetFlags = 10940;    // [10940]  // [T2ROTF] T2=10940 (was 11068, MED)
constexpr u8 kDeviceFlagNoHardware = 0x80;
// Byte whose 0x2 bit makes CDevice::SetFence (0x82278528) write each fence's
// completion value itself instead of waiting for the GPU to. The title device
// runs without it once created: its fences complete on the render thread.
constexpr u32 kDeviceOffsetFlags2 = 10941;    // [10941]  // [T2ROTF] T2=10941 (was 11069, HIGH)
constexpr u8 kDeviceFlagImmediateFences = 0x02;
// Pointer to the completed-fence word IsFencePending / BlockOnFence read.
constexpr u32 kDeviceOffsetFenceCompletion = 10896;    // [10896]  // [T2ROTF] T2=10896 (was 11024, HIGH)
// Pointer to the 0x20-byte scratch block whose GPU address the swap's packets carry
// (SynchronizeToPresentationInterval reads *(dev+10900) and 822D4580 reads the global
// device's). T2 allocates it apart from the fence word's own block.
constexpr u32 kDeviceOffsetSwapScratchWord = 10900;    // [10900]  // [T2ROTF] T2=10900 (was 11028, HIGH): sub_822D4A08 在同一段代码里于 10896 处 free/alloc/清零 0x60 字节、于 10900 处 free/alloc/清零 0x20 字节（li r5,96 / li r5,32、stw r29,10900），正是参考值的 −128
// Nonzero while a KickOff is in progress (BlockOnFence does not kick then).
// sa2-nx: the word SA2's CDevice::BlockOnFence (sub_826B42A8) tests [13232].
constexpr u32 kDeviceOffsetInKickOff = 13232;  // [T2ROTF] T2=13232 (was 13600, HIGH)
// Side buffer (0x12C0 bytes) every device gets.
constexpr u32 kDeviceOffsetSideBuffer = 16728;    // [16728]  // [T2ROTF] T2=16728 (was 17136, LOW)
constexpr u32 kSideBufferSize = 0x12C0;
// Set to -1 by the device-type-2 path.
constexpr u32 kDeviceOffsetsMinusOne[] = {21560, 21564, 14936};  // [21560, 21564, 14936]  // [T2ROTF] MinusOne 三哨兵：前两个由 822D1640:56/57 与 822E1690:78/79 连续写入 21560/21564（−404，双角色见证）
constexpr u32 kDeviceTypeNoHardware = 2;
// Copied from the global qword at 0x82F39B38 into every device.
constexpr u32 kDeviceOffsetGlobalQword = 11008;  // [10880]  // [T2ROTF] MED: 822D13F0(CBlocker_Check) 读 11008；保持参考值
constexpr u32 kGlobalQwordVa = 0x834FB7E0;  // [0x82F39B38]  // [T2ROTF] UNRESOLVED: T2 的该 guest 全局变量地址未定位（写入点未找到）
// Hardware-path fields: two critical sections, QueryPerformanceFrequency and
// its reciprocal as floats, and 41 words set to -1.
constexpr u32 kDeviceOffsetCriticalSection0 = 14944;  // [14944]  // [T2ROTF] T2=14944 (was 15348, HIGH): T2 自己的硬件路径 sub_822E1690 以设备为基初始化它（addi r3,r3,14944），sub_822E05D0 同
constexpr u32 kDeviceOffsetCriticalSection1 = 14972;  // [14972]  // [T2ROTF] T2=14972 (was 15376, HIGH): sub_822E1690 初始化（addi r3,r31,14972）；T2 环代码 sub_822D3630 / sub_822D37D0 以 dev+14972 为锁对象
constexpr u32 kDeviceOffsetPerfFrequency = 21592;    // [21588]  // [T2ROTF] T2=21592 (was 21996, MED)
constexpr u32 kDeviceOffsetTitleMinusOne = 23380;  // [T2ROTF] UNRESOLVED: 23380 无见证，−404 候选 22976 亦无见证
// Import slot holding the address of the kernel's VdGlobalDevice variable.
// sa2-nx: SA2's (sub_8255CDA0 stores the title device there) [0x820007FC].
constexpr u32 kVdGlobalDeviceImportSlot = 0x820011B0;  // [T2ROTF] T2=0x820011B0 (was 0x820005F8, HIGH)
// T2 keeps TWO VdGlobalDevice cells and picks between them by process type. Reading the
// whole image (extracted/TU/default_tu.flat, 54 tool-verified sites) shows one idiom: the
// pair lwz rX,4528(base) / lwz rX,4524(base) — i.e. slot B (0x820011B0) when
// KeGetCurrentProcessType() == 2 and slot A (0x820011AC) otherwise — and sub_822E1690
// (22275-22295) publishing into exactly the slot its own process type selects. Each slot
// holds a pointer TO the cell that carries the device, not the device itself.
constexpr u32 kVdGlobalDeviceImportSlotProc2 = 0x820011B0;  // [T2ROTF] T2=0x820011B0, KeGetCurrentProcessType() == 2
constexpr u32 kVdGlobalDeviceImportSlotOther = 0x820011AC;  // [T2ROTF] T2=0x820011AC (4524), 其余进程类型

constexpr u32 kE_OUTOFMEMORY = 0x8007000Eu;

u32 g_ring_sink = 0;
u32 g_ring_data = 0;
std::atomic<u32> g_ring_data_offset{0};

// Device blocks of released devices, for the next Direct3D_CreateDevice (the
// records heap never frees physical blocks). N&B creates and releases a pair
// of no-hardware devices around every section it loads.
std::mutex g_free_devices_mutex;
std::vector<u32> g_free_devices;

void WriteU32(u8 *device_bytes, u32 offset, u32 value) {
  *reinterpret_cast<be_u32 *>(device_bytes + offset) = value;
}

u32 LoadGuestU32(u32 va) {
  const auto *p = GuestPtr<const be_u32>(va);
  return p ? u32(*p) : 0;
}

// The newest fence queued for completion (the title device's).
std::atomic<u32> g_queued_fence{0};

// What CDevice::SetFence (0x82278528) does at a KickOff: the current fence
// covers the work issued so far, and the device moves on to the next. The
// fence completes when the render thread has read that work's data (and
// written its memexport results): until then the game must not reuse it.
// Without completing no resource tagged with a fence would stop being busy.
void KickFence(u8 *bytes) {
  const u32 fence = *reinterpret_cast<const be_u32 *>(bytes + d3d::kDeviceOffsetFenceTag);
  const u32 completion = *reinterpret_cast<const be_u32 *>(bytes + kDeviceOffsetFenceCompletion);
  WriteU32(bytes, d3d::kDeviceOffsetFenceTag, fence + 2);
  g_queued_fence.store(fence, std::memory_order_release);
  render_thread::QueueFenceCompletion(completion, fence, LastCallback());
}

// IsFencePending's test.
bool FencePending(const u8 *bytes, u32 fence) {
  const u32 tag = *reinterpret_cast<const be_u32 *>(bytes + d3d::kDeviceOffsetFenceTag);
  const u32 completion = *reinterpret_cast<const be_u32 *>(bytes + kDeviceOffsetFenceCompletion);
  const auto *word = GuestPtr<const be_u32>(completion);
  return word && tag - fence < tag - u32(*word);
}

// ---------------------------------------------------------------------------
// Device initialisation (N&B's D3D, natively)
// ---------------------------------------------------------------------------

// D3D::XenonGpu_Initialize (0x82663208): the register image's power-on
// values, everything dirty. (Its register writes go into the ring.)
void XenonGpuInitialize(native::Device d) {
  d.SetQ(0x2E30, 0x00000000FFFFFFFFull);
  for (u32 i = 0; i < 26; ++i)  // texture fetch constants: type 0
    d.SetW(native::off::TextureFetch + i * 24, d.W(native::off::TextureFetch + i * 24) & ~3u);
  for (u32 i = 0; i < 18; ++i) {  // vertex fetch constants: type 1
    const u32 offset = native::off::VertexFetch + i * 8;
    d.SetW(offset, (d.W(offset) & ~3u) | 1);
  }
  d.SetW(0x28BC, 0x20002000);
  d.SetW(0x29D4, 0xFF000);
  d.SetW(0x2A14, 0x10);
  d.SetW(native::off::ClipControl, d.W(native::off::ClipControl) | 0x80000);
  d.SetW(0x2984, 0xE);
  d.SetW(0x2A10, 0xE);
  d.SetW(0x296C, 8);
  d.SetW(0x29D8, 0xFF100);
  d.SetW(0x2954, 4);
  // PA_SU_SC_MODE_CNTL vtx_window_offset_enable: one-tile draws are in screen
  // space (see draw.cpp, ComputeViewport).
  d.SetW(native::off::ModeControl, d.W(native::off::ModeControl) | 0x10000);
  d.SetW(0x28CC, 0xFFFFFF);
  d.SetW(0x29C0, 4);
  d.SetW(0x2A48, 2);
  for (u32 word = 0; word < 5; ++word)
    d.SetPending(word, ~u64(0));
  d.SetW(0x2AA4, 0xE);
}

// D3D::InitializeApiState (0x826597C8).
void InitializeApiState(native::Device d) {
  const u32 present_interval = d.W(0x351C);
  const u32 behavior_flags2 = d.W(0x5DAC);
  native::InitializeStateTables(d);
  d.SetW(0x2FA0, 5);
  d.SetW(0x2FA4, 1);
  native::SetScreenExtentQueryMode(d, 2);  // D3DSEQM_PRECLIP
  d.SetW(0x3090, 2);
  d.SetW(0x2FD8, 0x2A23B9);
  d.SetH(0x2FD4, 0);
  d.SetH(0x2FD6, 0);
  d.SetB(0x2FDC, 0);
  d.SetB(0x2FDD, 0);
  d.SetB(0x2FDE, 0);
  d.SetW(0x2FE4, ~0u);
  // Three zero bytes; N&B leaves the fourth from its stack.
  d.SetW(0x2FE8, 0);
  d.SetW(0x2FE0, 0x00FF0000);
  d.SetW(0x2FB8, 1);
  d.SetW(0x2FBC, 0);
  d.SetW(0x2FD0, 0);
  // The default vertex declaration is the device's own, at +0x2FA0.
  d.SetW(0x2E2C, d.va + 0x2FA0);
  d.Dirty(2, 0x80000);
  d.SetW(0x351C, present_interval);
  const u32 flags2 = d.W(0x5DAC);
  d.SetW(0x5DAC, (behavior_flags2 & 1) ? (flags2 | 1) : (flags2 & ~1u));
  d.SetW(native::off::PredicationSet, 0);
  native::UpdateTilePredicated(d);
  d.SetW(native::off::Predication, ~0u);
  d.SetW(native::off::RunPredicationState, 0);
  d.SetW(native::off::TilePredication, 0);
  d.SetW(native::off::RunPredication, 0);
  if (!(d.B(native::off::Flags) & native::kFlagsNoHardware))
    native::SetShaderGPRAllocation(d, 0, 0, 0);
}

// sub_8265F688: the scaler parameters (D3DPRESENT_PARAMETERS +0x60) the
// device keeps. N&B's code then derives the default scaling for the video
// mode and passes it to VdCallGraphicsNotificationRoutines: the display's.
void SetScalerParameters(native::Device d, const be_u32 *scaler) {
  u32 copy[14] = {};
  for (u32 i = 0; i < 6; ++i)
    copy[i] = scaler[i];
  copy[6] = copy[10] = scaler[6] ? 3 : 0;
  for (u32 i = 0; i < 14; ++i)
    d.SetW(0x359C + i * 4, copy[i]);
  d.SetB(native::off::Flags2, d.B(native::off::Flags2) | 0x10);
}

void SetGammaRamp(native::Device d, const u16 *ramp);
void SetPWLGamma(native::Device d, const u16 *pwl);

// BOOL D3D::InitializePresentationParameters(CDevice*, D3DPRESENT_PARAMETERS*)
// [0x82659AA8]: the video mode, the automatic back/depth buffers and the
// presentation state. N&B's parameters (sub_823ED9E8) disable the automatic
// front buffer and depth buffer and ask for the back buffer.
bool InitializePresentationParameters(native::Device d, u32 parameters_va) {
  const auto *p = GuestPtr<const be_u32>(parameters_va);
  if (!p)
    return false;
  const u32 width = p[0];
  const u32 height = p[1];
  const u32 back_format = p[2];
  const u32 multisample = p[4];
  const u32 depth_format = p[10];
  u32 front_format = p[16];
  // The front buffer format, as the scanout reads it.
  if (front_format == 0 || front_format == 0x28280186u)
    front_format = 0x28280106u;
  else if (front_format == 0x18280186u)
    front_format = 0x18280106u;
  else if (front_format == 0x282801B6u)
    front_format = 0x28280136u;
  else if (front_format == 0x182801B6u)
    front_format = 0x18280136u;

  rex::system::X_VIDEO_MODE mode{};
  rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
  d.SetW(0x5428, u32(mode.display_width));
  d.SetW(0x5430, u32(mode.display_width));
  d.SetW(0x5434, u32(i64(float(mode.refresh_rate) + 0.5f)));
  d.SetW(0x542C, u32(mode.display_height));

  if (!p[15])  // DisableAutoFrontBuffer
    RGPU_ERROR("InitializePresentationParameters: an automatic front buffer is not supported");
  if (!p[14]) {  // DisableAutoBackBuffer
    const u32 surface = native::CreateSurface(width, height, back_format, multisample, 0);
    if (!surface)
      return false;
    d.SetW(native::off::AutoBackBuffer, surface);
    native::SetRenderTarget(d, 0, surface);
  }
  if (p[9]) {  // EnableAutoDepthStencil
    const u32 surface = native::CreateSurface(width, height, depth_format, multisample, 0);
    if (!surface)
      return false;
    d.SetW(native::off::AutoDepthStencil, surface);
    native::SetDepthStencilSurface(d, surface);
  }
  for (u32 i = 0; i < 0x7C / 4; ++i)
    d.SetW(0x34E8 + i * 4, p[i]);
  if ((front_format & 0x3FFC0000u) != 0x24900000u)
    d.SetW(0x3528, (front_format & 0x3F) | 0x28280100u);
  SetScalerParameters(d, p + 0x60 / 4);
  // The default gamma: linear, as a piecewise-linear ramp for the front
  // buffer formats 7 and 0x36 (0x32 counts as 6).
  u32 gamma_format = front_format & 0x3F;
  if (gamma_format == 0x32)
    gamma_format = 6;
  else if (gamma_format == 7)
    gamma_format = 0x36;
  u16 ramp[768];
  if (gamma_format == 7 || gamma_format == 0x36) {
    for (u32 k = 0; k < 128; ++k) {
      const u16 base = u16((0xFFFFu * k) / 0x7F);
      const u16 delta = u16((0xFFFFu * (k + 1)) / 0x7F - base);
      for (u32 c = 0; c < 3; ++c) {
        ramp[c * 256 + k * 2] = base;
        ramp[c * 256 + k * 2 + 1] = delta;
      }
    }
    SetPWLGamma(d, ramp);
  } else {
    for (u32 i = 0; i < 256; ++i) {
      const u16 value = u16((((0x3FFu * i) / 0xFF) & 0x3FF) << 6);
      ramp[i] = ramp[256 + i] = ramp[512 + i] = value;
    }
    SetGammaRamp(d, ramp);
  }
d.SetW(0x4144, ~0u);
  return true;
}

// 发布设备到 VdGlobalDevice：与 guest 自己的硬件路径同构（sub_822E1690 的非 2/2 分支），
// 进程类型 == 2 走 0x820011B0、否则走 0x820011AC。参考端只有一个槽，本工程原先只硬编码
// 0x820011B0；但本运行时的标题进程是 X_PROCTYPE_USER(=1)（kernel_state.cpp:123），
// guest 的所有读取点因此都走 0x820011AC，槽里从没被发布过 → 读到空设备 → sub_822CBC00
// 在 lwz 10888(r31) 上访存 0x2A88 崩。另一个槽的单元若仍为空也一并补齐（同一张设备，只填空、不覆盖）。
void PublishGlobalDevice(u32 device_va) {
  const u32 proc_type = KeGetCurrentProcessType();
  const u32 primary_slot = proc_type == 2 ? kVdGlobalDeviceImportSlotProc2 : kVdGlobalDeviceImportSlotOther;
  const u32 other_slot = proc_type == 2 ? kVdGlobalDeviceImportSlotOther : kVdGlobalDeviceImportSlotProc2;
  const u32 primary_cell = LoadGuestU32(primary_slot);
  const u32 other_cell = LoadGuestU32(other_slot);
  u32 wrote_primary = 0;
  u32 wrote_other = 0;
  if (primary_cell) {
    *GuestPtr<be_u32>(primary_cell) = device_va;
    wrote_primary = primary_cell;
  }
  if (other_cell && other_cell != primary_cell && LoadGuestU32(other_cell) == 0) {
    *GuestPtr<be_u32>(other_cell) = device_va;
    wrote_other = other_cell;
  }
  RGPU_INFO("VdGlobalDevice: procType={} slot {:#010x} cell {:#010x} -> {:#010x}; slot {:#010x} cell {:#010x} -> {:#010x}",
            proc_type, primary_slot, primary_cell, wrote_primary, other_slot, other_cell, wrote_other);
  if (!wrote_primary)
    RGPU_ERROR("VdGlobalDevice: slot {:#010x} holds no cell; the device stays unpublished for procType {}", primary_slot,
               proc_type);
}

// The hardware path (D3D__InitializeHardwareDevice 0x82659F30) minus the GPU:
// the device becomes the one D3D internals find through VdGlobalDevice, and
// gets the presentation state and per-device fields they read.
bool InitializeTitleDevice(native::Device d, u32 present_parameters) {
RtlInitializeCriticalSection(d.va + kDeviceOffsetCriticalSection0);  // [T2ROTF] T2=14944: T2 硬件路径 sub_822E1690 以设备为基初始化
  RtlInitializeCriticalSection(d.va + kDeviceOffsetCriticalSection1);  // [T2ROTF] T2=14972: 同上；sub_822D3630 / sub_822D37D0 以 dev+14972 为锁
  d.SetW(16712, 0);

// D3D reads the device through the kernel's VdGlobalDevice from any thread
  // (resource locks, blockers).
  PublishGlobalDevice(d.va);

  const float frequency = float(KeQueryPerformanceFrequency());
  const float period = frequency > 0.0f ? 1.0f / frequency : 0.0f;
  d.SetF(kDeviceOffsetPerfFrequency, frequency);
  d.SetF(kDeviceOffsetPerfFrequency + 4, period);
  for (u32 i = 0; i < 41; ++i)
    d.SetW(kDeviceOffsetTitleMinusOne + i * 4, 0xFFFFFFFFu);  // [T2ROTF] UNRESOLVED: 23380 无见证，−404 候选 22976 亦无见证

  return !present_parameters || InitializePresentationParameters(d, present_parameters);
}

// D3D's CDevice teardown (0x8265A198) for our devices: they carry the
// no-hardware flag, which skips everything but unbinding. The side buffer is
// part of our device block, and N&B never gives a device a query block.
void UninitializeDevice(native::Device d) {
  if (d.W(native::off::Ring))
    native::UnsetAll(d);
}

// HRESULT Direct3D_CreateDevice(UINT Adapter, D3DDEVTYPE DeviceType, void *pUnused,
//     DWORD BehaviorFlags, D3DPRESENT_PARAMETERS *pPresentParameters,
//     IDirect3DDevice9 **ppReturnedDeviceInterface)   [0x8264DF28]
// N&B creates two: type 1 (the title's rendering device) and type 2 (D3D's
// own no-hardware device, which N&B keeps alongside).
u32 Direct3D_CreateDevice_hook(u32 /*adapter*/, u32 device_type, u32 /*unused*/,
                               u32 behavior_flags, u32 present_parameters,
                               mapped_u32 out_device) {
  if (out_device)
    *out_device = 0u;

  // Every D3D object the renderer creates from here on lives in this heap.
  if (!records::Init())
    return kE_OUTOFMEMORY;

  // The device, its side buffer and fence word share one block of our own
  // guest memory (64 KiB aligned, so each part keeps its 0x80 alignment).
  static_assert(d3d::kDeviceAllocSize % d3d::kDeviceAlignment == 0);
  constexpr u32 kFenceOffset = d3d::kDeviceAllocSize + ((kSideBufferSize + 0x7F) & ~0x7Fu);
  constexpr u32 kBlockSize = kFenceOffset + 0x80;
  u32 block_va = 0;
  {
    std::lock_guard lock(g_free_devices_mutex);
    if (!g_free_devices.empty()) {
      block_va = g_free_devices.back();
      g_free_devices.pop_back();
    }
  }
  if (!block_va)
    block_va = records::AllocPhysicalBlock(kBlockSize);
  const u32 device_va = block_va;
  const u32 side_buffer_va = block_va ? device_va + d3d::kDeviceAllocSize : 0;
  const u32 fence_va = block_va ? device_va + kFenceOffset : 0;
  if (!g_ring_sink)
    g_ring_sink = records::AllocPhysicalBlock(kRingSinkSize);
  if (!g_ring_data)
    g_ring_data = records::AllocPhysicalBlock(kRingDataSize);
  if (!device_va || !g_ring_sink || !g_ring_data) {
    RGPU_ERROR("Direct3D_CreateDevice: guest allocation failed");
    return kE_OUTOFMEMORY;
  }
  std::memset(GuestPtr<u8>(device_va), 0, kBlockSize);
  std::memset(GuestPtr<u8>(g_ring_sink), 0, kRingSinkSize);

  const native::Device d = native::DeviceAt(device_va);
  auto *device = GuestPtr<d3d::Device>(device_va);
  auto *bytes = d.bytes;
  const bool title_device = device_type != kDeviceTypeNoHardware;
  device->m_ReferenceCount = 1u;
  // Fields every device gets.
  WriteU32(bytes, kDeviceOffsetGlobalQword, LoadGuestU32(kGlobalQwordVa));  // [T2ROTF] MED: 822D13F0(CBlocker_Check) 读 11008；保持参考值
  WriteU32(bytes, kDeviceOffsetGlobalQword + 4, LoadGuestU32(kGlobalQwordVa + 4));  // [T2ROTF] MED: 822D13F0(CBlocker_Check) 读 11008；保持参考值
  WriteU32(bytes, d3d::kDeviceOffsetCreationThreadId, GuestGetCurrentThreadId());
  WriteU32(bytes, d3d::kDeviceOffsetIsTitleProcess, 0);  // KeGetCurrentProcessType() != 2
  WriteU32(bytes, kDeviceOffsetSideBuffer, side_buffer_va);
  WriteU32(bytes, kDeviceOffsetFenceCompletion, fence_va);
  if (title_device) {
    // The original's default when no ring configuration is requested.
    if (!(behavior_flags & 0x100) && !(behavior_flags & 0x3F000000))
      behavior_flags |= 0xC000000;
    WriteU32(bytes, d3d::kDeviceOffsetBehaviorFlags, behavior_flags);
    WriteU32(bytes, 21560, 0xFFFFFFFFu);
    WriteU32(bytes, 21564, 0xFFFFFFFFu);
  } else {
    for (u32 offset : kDeviceOffsetsMinusOne)
      WriteU32(bytes, offset, 0xFFFFFFFFu);
  }
  // Both run without GPU hardware: no hardware setup in the API state, and
  // fences complete as they are written.
  bytes[kDeviceOffsetFlags] |= kDeviceFlagNoHardware;
  bytes[kDeviceOffsetFlags2] |= kDeviceFlagImmediateFences;
  native::RewindRing(d);

  XenonGpuInitialize(d);
  if (title_device && !InitializeTitleDevice(d, present_parameters)) {
    RGPU_ERROR("Direct3D_CreateDevice: InitializePresentationParameters failed");
    return kE_OUTOFMEMORY;
  }
  InitializeApiState(d);
  if (title_device)
    native::ResetAllState(d);
  native::RewindRing(d);
  // From here the title device's fences complete on the render thread
  // (KickFence). The no-hardware device only records command buffers.
  if (title_device)
    bytes[kDeviceOffsetFlags2] &= u8(~kDeviceFlagImmediateFences);

  if (out_device)
    *out_device = device_va;
  if (title_device)
    Video::SetGuestDevice(device_va);
  RGPU_INFO("Guest D3D device (type {}) created at {:#010x}", device_type, device_va);
  return 0;  // S_OK
}

// ULONG D3DDevice_Release(D3DDevice*) [0x8264E080]
// As the original, but on the last release the device block goes back to our
// free list instead of N&B's heap (the original frees the side buffer and the
// raw allocation Direct3D_CreateDevice kept at device - 4).
u32 D3DDevice_Release_hook(u32 device_va) {
  auto *device = GuestPtr<d3d::Device>(device_va);
  if (!device)
    return 0;
  const u32 count = device->m_ReferenceCount;
  if (count != 1) {
    device->m_ReferenceCount = count - 1;
    return count - 1;
  }
  FlushDeferredVertices(device_va);
  UninitializeDevice(native::DeviceAt(device_va));
  device->m_ReferenceCount = 0u;
  RGPU_INFO("Guest D3D device at {:#010x} released", device_va);
  // The title device stays the renderer's until another replaces it.
  if (device_va != Video::GuestDevice()) {
    std::lock_guard lock(g_free_devices_mutex);
    g_free_devices.push_back(device_va);
  }
  return 0;
}

// The GPU starts on the kicked draws: the guest buffers they read are read
// then (FlushVertexFillsLocked), before the fence tells the game it may reuse
// them. Memexport results reach guest memory later, once the GPU has finished
// the frame (detail::CompleteMemExportsLocked); draws that read them sooner
// wait for them (DrawStateLocked).
void KickDevice(d3d::Device *device, u32 device_va) {
  FlushDeferredVertices(device_va);
  // banjo-nx (package 27c): the texture fills too (they were read at the submit, after this fence had let the game
  // rewrite them).
  banjo::renut_gpu::checks::QueueKickFlush();
  KickFence(reinterpret_cast<u8 *>(device));
}

// PRING CDevice::KickOff() [0x82210AB0]: hand the ring to the GPU. Nothing
// consumes it here, so rewind it and return the new write pointer. A device
// recording a command buffer (N&B records on worker threads with no-hardware
// devices) would chain its segment into the buffer; its draws are recorded on
// the host (command_buffers.h), so it only rewinds.
u32 KickOff_hook(u32 device_va) {
  auto *device = GuestPtr<d3d::Device>(device_va);
  if (!device)
    return 0;
  if (!IsRecordingCommandBuffer(device_va)) {
    RunReadyCallbacks();
    KickDevice(device, device_va);
  }
  native::RewindRing(native::DeviceAt(device_va));
  return device->m_pRing;
}

// void CDevice::KickOffSegment() [0x822106C8]
void KickOffSegment_hook(u32 device_va) {
  auto *device = GuestPtr<d3d::Device>(device_va);
  if (!device)
    return;
  if (!IsRecordingCommandBuffer(device_va))
    KickDevice(device, device_va);
  native::RewindRing(native::DeviceAt(device_va));
}

// PRING CDevice::BeginRingBig(DWORD Count) [0x821F7C18]: room for Count
// dwords, kicking when the segment is full.
u32 BeginRingBig_hook(u32 device_va, u32 count) {
  const native::Device d = native::DeviceAt(device_va);
  if (!d)
    return 0;
  if (d.W(native::off::Ring) + count * 4 > d.W(native::off::RingLimit))
    native::RewindRing(d);
  return d.W(native::off::Ring);
}

// PRING CDevice::BeginRingManual() [0x82A11158]
u32 BeginRingManual_hook(u32 device_va) {
  const native::Device d = native::DeviceAt(device_va);
  return d ? d.W(native::off::Ring) : 0;
}

// Inline data space (vertex data of inline draws, clear rects): scratch that
// nothing reads, handed out in a circle.
u32 RingData(u32 bytes, u32 alignment) {
  if (!g_ring_data || bytes > kRingDataSize / 2)
    return 0;
  if (alignment < 4 || (alignment & (alignment - 1)))
    alignment = 4;
  u32 offset = g_ring_data_offset.fetch_add(bytes + alignment, std::memory_order_relaxed);
  offset %= kRingDataSize / 2;
  offset = (offset + alignment - 1) & ~(alignment - 1);
  return g_ring_data + offset;
}

// void *D3D::RingBufferDeviceAllocate(CDevice*, DWORD Flags, DWORD *pSize,
//     DWORD Alignment) [0x8222D3E0]
u32 RingBufferDeviceAllocate_hook(u32 /*device*/, u32 /*flags*/, mapped_u32 size, u32 alignment) {
  return size ? RingData(u32(*size), alignment) : 0;
}

// void *CDevice inline data allocation (sub_8222D240): from the ring, or from
// the allocator of the command buffer being recorded.
u32 AllocateInlineData_hook(u32 /*device*/, u32 dwords, u32 alignment) {
  return RingData(dwords * 4, alignment);
}

// HRESULT D3DDevice_SetRingBufferParameters(D3DDevice*,
//     const D3DRING_BUFFER_PARAMETERS*) [0x82657508]: would replace the ring
// (and our sink) with a freshly allocated one.
u32 D3DDevice_SetRingBufferParameters_hook(u32 /*device*/, u32 /*parameters*/) {
  return 0;  // S_OK
}

// void CDevice::BlockOnFence(DWORD Fence, CBlocker::Type, BOOL NoKick)
// [0x8228F558]: every fence wait (D3DDevice_BlockOnFence, resource locks and
// destruction, InvalidateConstants). As the original: kick the fence if no
// KickOff has covered it yet, then wait for it, here for the render thread to
// complete it (KickFence).
// The original polls through CBlocker::Check, which after 5 s without
// progress dumps the GPU's registers and traps.
u32 BlockOnFence_hook(u32 device_va, u32 fence, u32 /*type*/, u32 /*no_kick*/) {
  banjo::renut_gpu::CountFenceQuery();  // banjo-nx (package 27c): the stream checks' design
  auto *bytes = GuestPtr<u8>(device_va);
  if (!bytes || !FencePending(bytes, fence))
    return 0;
  // Devices with immediate fences (the command buffer recorders) complete
  // theirs as they write them; the original returns at once for them too.
  if (bytes[kDeviceOffsetFlags2] & kDeviceFlagImmediateFences)
    return 0;
  const bool covered =
      int32_t(fence - g_queued_fence.load(std::memory_order_acquire)) <= 0;
  if (!covered) {
    if (*reinterpret_cast<const be_u32 *>(bytes + kDeviceOffsetInKickOff))
      return 0;
    KickOff_hook(device_va);
  }
  // The fence may wait for callbacks, which this thread runs as the render
  // thread reaches them.
  const auto waiting = std::chrono::steady_clock::now();
  for (;;) {
    RunReadyCallbacks();
    if (!FencePending(bytes, fence))
      break;
    if (!render_thread::WaitForFenceProgress(std::chrono::milliseconds(2)))
      break;  // no render thread: nothing would complete it
  }
  renut::frame_stats::OnGameThreadWait(
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - waiting).count());
  return 0;
}

// BOOL D3DDevice_IsFencePending(DWORD Fence) [0x8228F650]: on the device D3D
// finds through VdGlobalDevice; the current fence is kicked first. Games
// poll it, and the fence may be waiting for callbacks only a guest thread can
// run.
u32 IsFencePending_hook(u32 fence) {
  banjo::renut_gpu::CountFenceQuery();  // banjo-nx (package 27c): the stream checks' design
  RunReadyCallbacks();
  if (!fence)
    return 0;
  const u32 device_va = native::GlobalDevice();
  auto *bytes = GuestPtr<u8>(device_va);
  if (!bytes)
    return 0;
  if (fence == *reinterpret_cast<const be_u32 *>(bytes + d3d::kDeviceOffsetFenceTag))
    KickOff_hook(device_va);
  return FencePending(bytes, fence) ? 1 : 0;
}

// BOOL CBlocker::Check() [0x8264DCD0]: one poll of a D3D wait for the GPU
// (ring space, fence markers, streaming). Fence waits no longer come here
// (BlockOnFence_hook); nothing on the host advances the others.
u32 CBlocker_Check_hook(u32 /*blocker*/) {
  return 0;
}

// ---------------------------------------------------------------------------
// Command buffers
// ---------------------------------------------------------------------------

// D3DCommandBuffer fields.
constexpr u32 kBufferFence = 0x08;
constexpr u32 kBufferInheritTags = 0x18;  // D3DTAGCOLLECTION
constexpr u32 kBufferSetTags = 0x40;      // its copy
constexpr u32 kBufferFlags = 0x6C;        // low nibble: state (2 recording, 1 done, 7 out of memory)
constexpr u32 kBufferFixedMemory = 0x98;  // memory the caller gave it, or 0 for its allocator
constexpr u32 kBufferAllocatorSize = 0xA8;

// D3D::ConfigureForOnePassZPass(CDevice*, BOOL, DWORD TileCount, DWORD Width)
// [0x82658818]: the EDRAM layout of a buffer recorded for a one-pass Z pass.
void ConfigureForOnePassZPass(native::Device d, bool flag, u32 tile_count, u32 width) {
  u8 flags3 = d.B(native::off::Flags3) | 0x20;
  if (flag)
    flags3 |= 0x10;
  d.SetB(native::off::Flags3, flags3);
  const u32 msaa = d.H(native::off::SurfaceInfo) & 3;
  u32 tile_height = 8, tile_width = 32, unit = 80;
  if (msaa == 0) {
    tile_height = 16;
  } else if (msaa != 1) {
    tile_width = 16;
    unit = 40;
  }
  const u32 pitch = (tile_width + width - 1) & ~(tile_width - 1);
  const u32 tiles_per_row = pitch / tile_width;
  for (u32 i = 0; i < tile_count; ++i) {
    const u32 x = d.W(native::off::TileOrigins + i * 8);
    const u32 y = d.W(native::off::TileOrigins + i * 8 + 4);
    d.SetW(0x3338 + i * 4, (y / tile_height) * tiles_per_row + x / tile_width);
  }
  d.SetW(0x3374, pitch);
  d.SetW(native::off::SurfaceInfo, native::Insert(d.W(native::off::SurfaceInfo), pitch, 18, 14));
  d.Dirty(2, 0x100);
  d.SetW(0x3378, ((unit + width - 1) / unit) * unit);
  u64 predicated = d.Q(native::off::PredicatedPending2);
  if ((d.B(native::off::Flags3) & 0x20) || (d.W(0x2E54) & 0xE0000) != 0x40000)  // [T2ROTF] blend flags：kBlendFlags 的 T2 槽 (11860=0x2E54)
    predicated |= 0x100;
  else
    predicated &= ~u64(0x100);
  d.SetQ(native::off::PredicatedPending2, predicated);
}

// HRESULT D3DDevice_BeginCommandBuffer(D3DDevice*, D3DCommandBuffer*, DWORD Flags,
//     const D3DTAGCOLLECTION *pInheritTags, ..., const D3DRECT *pTilingRects,
//     DWORD TileCount) [0x82657CB0]
// The device records into the buffer: the draws on the host (command_buffers.h),
// so the buffer's segments and the reset of their allocator are left out.
u32 D3DDevice_BeginCommandBuffer_hook(u32 device_va, u32 command_buffer, u32 flags,
                                      u32 inherit_tags, u32 /*unused*/, u32 tiling_rects,
                                      u32 tile_count) {
  // A Begin*Vertices draw recorded into the previous buffer.
  FlushDeferredVertices(device_va);
  native::Device d = native::DeviceAt(device_va);
  auto *buffer = GuestPtr<be_u32>(command_buffer);
  if (!d || !buffer)
    return 0;
  auto bw = [buffer](u32 offset) -> be_u32 & { return buffer[offset / 4]; };
  bw(kBufferFlags) = native::Insert(bw(kBufferFlags), 2, 0, 4);
  if (const u32 fence = bw(kBufferFence)) {
    native::BlockOnFence(native::GlobalDevice(), fence);
    bw(kBufferFence) = 0u;
  }
  d.SetW(native::off::CommandBuffer, command_buffer);
  bw(kBufferFlags) = u32(bw(kBufferFlags)) & ~0x70u;
  for (u32 offset : {0x7Cu, 0x80u, 0x84u, 0x70u, 0x90u, 0x74u, 0x78u})
    bw(offset) = 0u;
  d.SetW(0x3A50, 8);
  d.SetB(native::off::Flags1, d.B(native::off::Flags1) & 0xDF);
  d.SetW(0x3A44, 0);
  d.SetW(0x3A40, bw(kBufferAllocatorSize));
  for (u32 offset = 0x34D0; offset <= 0x34E4; offset += 4)  // [T2ROTF] KEEP: 0x34C0-0x34F0 块与参考同址（CommandBuffer 0x34CC=13516 已由 822D3630/822D4000 见证）
    d.SetW(offset, 0);
  native::RewindRing(d);

  // The tags the buffer inherits (none set unless given) and its copy.
  auto *tags = GuestPtr<const be_u64>(inherit_tags);
  auto *own = reinterpret_cast<be_u64 *>(reinterpret_cast<u8 *>(buffer) + kBufferInheritTags);
  auto *own_copy = reinterpret_cast<be_u64 *>(reinterpret_cast<u8 *>(buffer) + kBufferSetTags);
  for (u32 i = 0; i < 5; ++i) {
    own[i] = tags ? u64(tags[i]) : 0;
    own_copy[i] = u64(own[i]);
  }
  u32 buffer_flags = bw(kBufferFlags);
  buffer_flags &= ~0x80u;
  buffer_flags &= ~0x8000u;
  buffer_flags |= (((flags << 3) & 0x100) | ((flags >> 4) & 1)) << 7;
  if (flags & 8) {
    d.SetW(native::off::CommandBufferTagMask, ~0u);
    buffer_flags |= 0x400;
  } else {
    d.SetW(native::off::CommandBufferTagMask, 0x400000);
    buffer_flags &= ~0x400u;
  }
  bw(0x78) = 0u;
  buffer_flags &= ~0x21800u;
  bw(kBufferFlags) = buffer_flags;
  d.SetQ(native::off::PredicatedPending2, 0);
  d.SetB(native::off::Flags3, d.B(native::off::Flags3) & 0xCF);
  d.SetB(native::off::Flags, d.B(native::off::Flags) & 0x87);

  if (flags & 3) {
    if (flags & 2) {
      bw(kBufferFlags) = u32(bw(kBufferFlags)) | 0x800;
      d.SetB(native::off::Flags, d.B(native::off::Flags) | 8);
    } else {
      // Its ring allocation list (0x33C0) is the GPU's.
      bw(kBufferFlags) = u32(bw(kBufferFlags)) | 0x1000;
    }
    d.SetW(native::off::TileCount, tile_count);
    d.SetB(native::off::Flags, d.B(native::off::Flags) | 0x30);
    u32 extent_x = 0, extent_y = 0;
    const auto *rects = GuestPtr<const be_u32>(tiling_rects);
    for (u32 i = 0; rects && i < tile_count; ++i) {
      const u32 x1 = rects[i * 4], y1 = rects[i * 4 + 1];
      const u32 x2 = rects[i * 4 + 2], y2 = rects[i * 4 + 3];
      if (extent_x <= x2)
        extent_x = x2;
      if (extent_y <= y2)
        extent_y = y2;
      d.SetW(native::off::TileRects + i * 16, x1);
      d.SetW(native::off::TileRects + i * 16 + 4, y1);
      d.SetW(native::off::TileRects + i * 16 + 8, x2);
      d.SetW(native::off::TileRects + i * 16 + 12, y2);
      d.SetW(native::off::TileOrigins + i * 8, x1 & ~31u);
      d.SetW(native::off::TileOrigins + i * 8 + 4, y1 & ~31u);
    }
    d.SetW(native::off::TilingExtent, extent_x);
    d.SetW(native::off::TilingExtent + 4, extent_y);
    d.SetW(0x3384, 0);
    d.SetW(0x31B0, 2);
    if (flags & 0x40) {
      ConfigureForOnePassZPass(d, (flags >> 7) & 1, tile_count, extent_x);
      bw(kBufferFlags) = u32(bw(kBufferFlags)) | 0x20000;
    }
  }
  if (flags & 4) {
    d.SetB(native::off::Flags, d.B(native::off::Flags) | 0x40);
    d.SetW(0x31A0, native::Extract(d.W(native::off::DepthControl), 4, 3));
    if (d.B(native::off::Flags3) & 0x20) {
      u64 predicated = d.Q(native::off::PredicatedPending2) | (u64(1) << 37);
      if (d.W(0x3378) != (d.W(native::off::SurfaceInfo) & 0x3FFF))
        predicated |= u64(1) << 57;
      d.SetQ(native::off::PredicatedPending2, predicated);
    }
  }
  d.SetW(native::off::PredicationSet, 0);
  native::UpdateTilePredicated(d);
  d.SetW(native::off::Predication, ~0u);
  d.SetW(native::off::RunPredicationState, 0);
  d.SetW(native::off::TilePredication, 0);
  d.SetW(native::off::RunPredication, 0);
  // The buffer starts with no fetch constants, and dirty in everything it does
  // not inherit.
  std::memset(d.bytes + native::off::TextureFetch, 0, 0x300);
  for (u32 i = 0; i < 5; ++i)
    d.SetPending(i, ~u64(own[i]));

  BeginRecording(device_va, command_buffer);
  return 0;
}

// HRESULT D3DDevice_EndCommandBuffer(D3DDevice*) [0x826582A0]
// The state left dirty would be written into the buffer (Flags bit 5 at
// Begin); then everything is unbound, and the device is pointed at its side
// buffer until the next Begin.
u32 D3DDevice_EndCommandBuffer_hook(u32 device_va) {
  FlushDeferredVertices(device_va);
  native::Device d = native::DeviceAt(device_va);
  if (!d)
    return 0;
  auto *buffer = GuestPtr<be_u32>(d.W(native::off::CommandBuffer));
  if (buffer && (u32(buffer[kBufferFlags / 4]) & 0x8000)) {
    for (u32 i = 0; i < 5; ++i)
      d.SetPending(i, 0);
  }
  native::UnsetAll(d);
  // A buffer in fixed memory reports running out of it here; recorded on the
  // host, it does not.
  if (buffer) {
    if (u32(buffer[kBufferFlags / 4]) & 0x1000)
      buffer[0x78 / 4] = 0u;  // its ring allocation list: none
  }
  d.SetW(native::off::CommandBuffer, 0);
  if (buffer) {
    const u32 end = d.W(native::off::Ring) + 4;
    buffer[0x88 / 4] = end;
    buffer[0x8C / 4] = d.W(native::off::RingLimit);
    buffer[0x90 / 4] = native::GpuAddress(end) - 0x40000000u;
  }
  const bool out_of_memory = (d.B(native::off::Flags1) & 0x20) != 0;
  if (buffer)
    buffer[kBufferFlags / 4] = native::Insert(buffer[kBufferFlags / 4], out_of_memory ? 7 : 1, 0, 4);
  // CDevice::MarkAsOutOfMemory: the device writes nowhere until the next Begin.
  native::RewindRing(d);
  d.SetB(native::off::Flags1, d.B(native::off::Flags1) | 0x20);
  return out_of_memory ? kE_OUTOFMEMORY : 0;
}

// void D3DDevice_RunCommandBuffer(D3DDevice*, D3DCommandBuffer*,
//     DWORD PredicationSelect) [0x82222888]
// Replaces the original, which would send the buffer's packets to the GPU
// (and on a no-hardware device append to a buffer being recorded).
void D3DDevice_RunCommandBuffer_hook(u32 device_va, u32 command_buffer,
                                     u32 /*predication_select*/) {
  if (!command_buffer)
    return;
  if (IsRecordingCommandBuffer(device_va)) {
    RecordRun(device_va, command_buffer);
  } else {
    RunReadyCallbacks();
    QueueRunCommandBuffer(device_va, command_buffer);
  }
}

// void D3DDevice_Swap(D3DDevice *pDevice, D3DBaseTexture *pFrontBuffer,
//     const D3DVIDEO_SCALER_PARAMETERS *pParameters) [0x82287590]
// The frame goes to the render thread, which presents it; the game thread
// goes on with the next one, waiting only when it is more than
// render_thread::kMaxFramesAhead ahead.
constexpr u32 kSa2DeviceOffsetGammaRamp = 15408;  // sa2-nx: SA2's +0x3A9C (the +404 block)  // [T2ROTF] UNRESOLVED: T2 的 gamma ramp 槽未定（14940 属 ring 族，不是 ramp）

// ---------------------------------------------------------------------------
// [T24] T2 自己的显示会计槽位（全库逐字节取证，见工程 Markdown）。
//   +16548  垂直同步计数：guest 的 vblank/交换完成处理程序 sub_822D9570 自增
//   +16552  最近一次 vblank/帧时间（mftb 写入）
//   +16560  提交（Swap）计数：sub_822D9D70 开头自增，本文件的 Swap 钩子已镜像
//   +16564  帧计数：sub_822D9688 自增
//   +16568  「已确认显示」帧计数：sub_822D9570 / sub_822D9688 自增
//   +10900  0x20 字节暂存块指针；其 +4 字是 guest 可见的帧完成字（sub_822D9570 与
//           sub_822D9688 都会往它写 0），其中 +0 与 +4 与 0x822D9650 的 fence 配平
//   +16700/+16704  {值, vblank 目标} 环的读/写索引（本轮只读不推，见 Markdown 的未决项）
//   +16968/+16976 是 MGR/SA2 的 SwapCount/Displayed 邻槽，T2 镜像里 0 命中（作废）。
//
// 为什么必须由宿主推进：这三条自增都发生在 GPU 垂直同步中断的处理程序里，
// Switch 侧没有任何后端会派发该中断（日志中 irq sub_822D9570 一次都没有），
// 于是 guest 永远看不到「上一帧已经显示」，影片/呈现链在第 6 趟之后就停在原地。
// ---------------------------------------------------------------------------
void T2AdvanceDisplayAccounting(u32 device_va, u32 completed_swap) {
  auto *device = GuestPtr<u8>(device_va);
  if (!device || !device_va) {
    return;
  }
  auto word = [device](u32 offset) -> be_u32 & {
    return *reinterpret_cast<be_u32 *>(device + offset);
  };
  auto byte = [device](u32 offset) -> u8 & { return device[offset]; };
  const u32 vblank = u32(word(16548)) + 1;  // guest: 822D9570 `addi r30,r3,16712` 起的 vblank 路径
  word(16548) = vblank;
  word(16552) = u32(std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count() &
                    0xFFFFFFFFu);  // 帧时（粗略；只让 guest 的耗时统计有推进）
  // 帧计数与「已确认显示」帧计数：guest 的 822D9688/822D9570 都在这两处自增。
  word(16564) = u32(word(16564)) + 1;
  word(16568) = completed_swap ? completed_swap : (u32(word(16568)) + 1);
  // 帧完成字：guest 的 822D9570 明确往 *(*(dev+10900)+4) 写 0（`stw r9,4(r11)`，r9 = 0）。
  const u32 block = u32(word(10900));
  u32 completado = 0;
  if (block) {
    auto *blk = GuestPtr<u8>(block);
    if (blk) {
      completado = u32(*reinterpret_cast<be_u32 *>(blk + 4));
      *reinterpret_cast<be_u32 *>(blk + 4) = 0;
    }
  }
  // 10944 bit 0x80：guest 的 822DD170 在「交换完成」时置位（事件号 82 的派发前）。
  byte(10944) |= 0x80;
  static std::atomic<u64> avisos{0};
  static std::atomic<i64> ultimo{-100000};
  const i64 ahora = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
  i64 previo = ultimo.load(std::memory_order_relaxed);
  if (ahora - previo >= 2000 && ultimo.compare_exchange_strong(previo, ahora)) {
    RGPU_WARN(
        "[renut] T2 display accounting: dev={:#010x} vblank16548={} sub16560={} fr16564={} "
        "disp16568={} blk10900={:#010x} w10900_4={} f10944={:#04x} (avisos={})",
        device_va, u32(word(16548)), u32(word(16560)), u32(word(16564)), u32(word(16568)), block,
        completado, u32(byte(10944)), avisos.fetch_add(1, std::memory_order_relaxed) + 1);
  }
}

// sa2-nx (2026-10-20): the SDK's core layout (system/xthread.cpp).
REXCVAR_DEFINE_BOOL(sa2_renut_core_layout, true, "SA2/Renderer",
                    "Renut's renderer on the Switch: at the first Swap the guest main thread (SA2's game and drawing "
                    "thread) is pinned to core 0 and every other guest thread gets cores 1-2 (the render thread is on "
                    "core 1), so no game thread time-slices with it. Off: every guest thread on any core, as before")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// [T25] 「首个动画有声无画、播放完死机」的两个实验开关，默认全关，只作实机 A/B。
//
// 背景（逐条取证见根目录 T2ROTF_deko3d_呈现门阻塞语句与显示会计核对报告.md）：
// 卡死的唯一自旋点是 guest 呈现门 sub_82881B88 的三槽全不就绪分支（yield 的
// lr=0x82881DE8），门等的是全局呈现队列 0x82E55CE0 族与显示管理器对象
// 0x82E55EE0 的三槽/条目完成，而这些只有 guest 的显示侧（垂直同步）代理会推进：
// 游戏自己的 tick sub_82ABB7D0 每帧调 sub_82882200，后者对 0x82E55EE0 调
// sub_82881E00（r4=-1, r5=1，单趟有界）。宿主这边 D3DDevice_Swap_hook 整体替换了
// guest 交换体 sub_822D9D70，既不调该排空、又让 guest 记账整段不跑，于是两侧都没人
// 推进它 → 三槽永远不就绪 → 门永远 yield。下面的开关分别从两侧补齐这条推进链。
REXCVAR_DEFINE_BOOL(sa2_renut_display_tick, false, "SA2/Renderer",
                    "[T2ROTF] debug A/B: after the host's Swap hook has presented the frame, run one display "
                    "(vertical-blank) drain pass on the guest's display manager, exactly as the guest's own tick "
                    "sub_82ABB7D0 does through sub_82882200: sub_82881E00(ctx=0x82E55EE0, arg=0xFFFFFFFF, flag=1), "
                    "which is the call the present gate sub_82881B88 waits on (queue 0x82E55CE0+284/+288, entry "
                    "+24/+28). flag=1 makes it a single bounded pass, and it runs the callbacks the display side "
                    "queued. Off: the host advances nothing of the guest's display queue.")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(sa2_renut_swap_callthrough, false, "SA2/Renderer",
                    "[T2ROTF] debug A/B: after the host's Swap hook has presented the frame, also run the guest's own "
                    "D3DDevice_Swap body (sub_822D9D70), whose bookkeeping the host hook replaces entirely (the ring "
                    "pointer at dev+48, dev+10896, the in-flight gate at dev+21624/+21628, dev+23348/+23956). While "
                    "this is on the host stops mirroring the swap count (dev+16560), so the guest body's own increment "
                    "is not doubled. Off: the guest body does not run.")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// [T28] 显示队列「完成侧」代理，**默认开**（不写 toml 也生效）：宿主在每次 Swap 呈现后，
// 把这一帧交付给显示侧的那一份会计记一次完成（base+288 与条目 +28 各退 1），并在六通道
// 全 0 却仍有陈旧投递时清理残留。详见下面 DisplayAgentStep 的注释与 T28 报告。
// 关掉它：guest 的每帧 tick sub_82ABB7D0 → sub_82882200 又会因为会计回不到 0 而空转（0 fps）。
// [T31] 默认改成 false：实机取证证明「宿主盲写会计」不是可用修复，已改为只读观察（见下面
//       DisplayAgentStep 的 T31 说明）；开关保留只为复现取证。
REXCVAR_DEFINE_BOOL(sa2_renut_display_agent, false, "SA2/Renderer",
                    "[T2ROTF] the host plays the display/vertical-blank side the Switch has no source for: after each "
                    "presented Swap it completes one handed-over display entry (base 0x82E55CE0 +288 and the entry's "
                    "+28, never below zero), and clears stale 'posted but never delivered' counts (+284 / entry +24) "
                    "once the six channel markers are all zero for several observations in a row. Without it the "
                    "guest's per-frame tick (sub_82ABB7D0 -> sub_82882200) spins forever on the display queue: 0 fps.")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
// [T29] 显示**完成侧**释放，**默认开**（不写 toml 也生效）：Switch 后端没有 GPU 垂直同步/完成中断
// （Xbox 上由 ISR 族 sub_82997AF8/sub_82997968/sub_829975F0 驱动，它们在生成代码里没有 guest 调用者），
// 于是「已交付给显示侧」的那一份会计（base 0x82E55CE0 +288 与条目 +28）永远没人回收 —— 实机卡死态读到
// 的正是 cola284=0、cola288=1。宿主在 Swap 钩子/门钩子处按「六通道标记全 0 且连续同一指纹」的守卫，
// 每次把这一份退 1（绝不退到负数），让 guest 自己的排空循环 sub_82882200 判据
// *(base+284)+*(base+288) != 0 归零、帧循环恢复。关掉它 = 完全交还 guest（会退回 0 fps 空转）。
// [T31] 默认改成 false：实机取证证明门热路的盲写会把会计刷过 0 并回绕成 0xFFFFFFFF（排空循环
//       永不退出 = Sphaira 无限黑屏），或让门提前就绪而走进空当前显示对象（前端报错退出）；
//       已改为只读观察，开关保留只为复现取证。
REXCVAR_DEFINE_BOOL(sa2_renut_display_release, false, "SA2/Renderer",
                    "[T2ROTF] the host plays the display-completion side the Switch has no interrupt source for: it "
                    "retires the already-handed-over display accounting (base 0x82E55CE0 +288 and the entry's +28, "
                    "never below zero) once the six channel markers are all zero and the same four accounting words "
                    "read the same for a run of observations. Without it the guest per-frame tick (sub_82ABB7D0 -> "
                    "sub_82882200) spins forever on *(base+284)+*(base+288) != 0: 0 fps. Its sibling "
                    "sa2_renut_display_agent drains the stale 'posted but never delivered' counters (+284 / entry +24).")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// [T25] 显示管理器对象 / guest 交换体 / 呈现队列排空的 guest 地址（见上）。
// [T28] 修正：门 sub_82881B88 的 container（r30）是 0x82E55E20，不是 0x82E55EE0（差 0xC0）。
// 取证：排空者 sub_82882200 里 `lis r11,-32027` + `addi r28,r11,24096` = 0x82E50000+0x5E20，
// 随后 `mr r3,r28; mr r4,r29; li r5,1; bl 0x82881e00`（39.cpp，sub_82882200 内），
// 而门自己 `addi r3,r30,24` / `addi r3,r26,8`（r26 = 0x82E55CE0）说明它两个容器分别是
// 传入的 container 与 0x82E55CE8。0x82E55EE0 只出现在别的结构里，用它调门只会读到垃圾。
constexpr u32 kDisplayCtx = 0x82E55E20u;  // 门 sub_82881B88 的 container（r30）
REX_EXTERN(__imp__sub_822D9D70);          // guest 交换体本体
REX_EXTERN(__imp__sub_82881E00);          // 门 sub_82881B88 的调用者（显示队列单趟排空）
REX_EXTERN(__imp__sub_82881B88);          // 门本体（T28 守卫用）

// ------------------------------------------------------------------------------------------------
// [T28] 显示队列「完成侧」代理（sa2_renut_display_agent，默认开）
// ------------------------------------------------------------------------------------------------
// 实机取证（2026-10-28 日志 T2ROTF_001(60).log / T2ROTF_002(41).log 的 T2DIAG PUERTA 行 +
// 生成代码逐字重建，见根目录 T2ROTF_deko3d_显示完成侧代理与kDisplayCtx修正报告.md）：
//
//   呈现门 sub_82881B88(container=0x82E55E20, idx, out1, out2)，base = 0x82E55CE0：
//     · idx != -1 时先扫条目 *(base+272)+idx*32 的子条目：任一子条目的 +24/+28 之和 > 0
//       （有在飞）就直奔 loc_82881DE8（yield sub_824F0620）返回 0；
//     · 三槽循环：标记 = *(*(container+16)+272+4*slot)（实机 = 0x82E55F30/34/38）。
//       slot0 标记非 0 即就绪；slot1 还要 *(container+0) >= 2 且 d = TB - *(0x82DBB688)
//       落在 (*(0x82E55F48), *(0x82E55F50)) 时间窗里（Xbox 上由垂直同步刷新）；
//       slot2 要 *(container+0) != 5 且 *(container+288) + *(0x82DBB5C4) == 0。
//       就绪 → sub_82881930(container+24, slot, idx, out1)：标记--、base+284--、条目+24--、
//       条目+28++、base+288++（交付给显示侧）；三槽都不就绪再看 *(base+256+4*slot)
//       同套判据；全都没有 → yield 返回 0。
//     · 排空泵 sub_82881E00(container, idx, flag)：门成功 → 跑回调、base+288--、条目+28--；
//       flag=1 单趟有界，flag=0 时才继续循环。
//     · 排空调用者 sub_82882200(idx)：idx==-1 的循环条件是 *(base+284)+*(base+288) != 0，
//       否则是 *(表+idx*32+24)+(+28) != 0。它的唯一静态调用者是每帧 tick sub_82ABB7D0
//       （29.cpp:122772 `lwz r3,29884(0x82DA0000)` = *(0x82DA74BC)，由开机 sub_82ABB6D8
//       经 sub_82882490 分配后写一次），所以这个 while 一旦非 0 就是**每帧 100% 空转**：
//       实机 0.0 fps、多线程 100% 停在 lr=0x82881DE8（门里那次 yield）。
//     · 实机探针在门进不去的那一刻读到 cnt=0/0/0、obj256=0/0/0、cola284=0、
//       **cola288=1**、st0=0 —— 通道环是空的、没有在飞条目，残留的只有一份
//       「已交付未完成」会计（base+288 与条目 +28 同族，1 份）。
//
//   这份会计唯一的消除途径是门成功（交付/回调），而门成功要求通道标记非 0；通道标记的
//   消费者在 Xbox 上是 GPU 垂直同步侧（ISR sub_82997AF8 全库没有 guest 调用者），
//   Switch 后端没有对应来源 ⇒ 会计永远回不到 0 ⇒ 每帧 tick 死等第一帧。
//
// 本代理：宿主的 Swap 钩子 + 呈现门钩子就是这台机子的显示/垂直同步侧（Swap 钩子把帧交给
//   deko3d 并 present），于是由宿主把「六通道确实全 0 却仍留着 +284/+288 / 条目+24/+28 残留」
//   的陈旧会计抽干（连续多次观测到同一组数值才动手，见 DisplayAgentStep 的规则说明）。
//   这样 tick sub_82ABB7D0 → sub_82882200 的 while（判据 *(base+284)+*(base+288) != 0）
//   每帧都能归零，帧循环恢复。反过来，只要有一个通道被 arm（说明有真实投递在飞），
//   本代理一律不动，完全交还 guest 自己的流水线。
constexpr u32 kDisplayQueueBase = 0x82E55CE0u;   // 呈现队列全局（自指针：*(base) == base）
constexpr u32 kDisplayEntryIdxVa = 0x82DA74BCu;  // 显示条目号（开机写一次）
constexpr u32 kOffQueueTable = 272u;             // 条目表指针
constexpr u32 kOffQueuePosted = 284u;            // 已投递未交付
constexpr u32 kOffQueueInflight = 288u;          // 已交付未完成
constexpr u32 kOffCtxObject = 16u;               // container+16 → 通道标记所在对象
constexpr u32 kOffObjectMarkers = 272u;          // 该对象 +272/276/280 = 三个通道标记
constexpr u32 kOffManagerMarkers = 256u;         // base+256/260/264 = 管理容器三槽标记

bool StoreGuestU32(u32 va, u32 value) {
  auto *p = GuestPtr<be_u32>(va);
  if (!p)
    return false;
  *p = value;
  return true;
}

u32 DisplayQueueEntry(u32 idx) {
  if (idx >= 64u)
    return 0u;
  const u32 tabla = LoadGuestU32(kDisplayQueueBase + kOffQueueTable);
  return tabla ? tabla + idx * 32u : 0u;
}

void DisplayMarkerSnapshot(u32 *markers /* [6] */, u32 &ctx_object) {
  ctx_object = LoadGuestU32(kDisplayCtx + kOffCtxObject);
  for (u32 s = 0; s < 3u; ++s)
    markers[s] = ctx_object ? LoadGuestU32(ctx_object + kOffObjectMarkers + 4u * s) : 0u;
  for (u32 s = 0; s < 3u; ++s)
    markers[3u + s] = LoadGuestU32(kDisplayQueueBase + kOffManagerMarkers + 4u * s);
}
// 返回本次真正写掉的字数（0 = 未动队列）。
//
// 只有一种情况会动手（避免和真实投递抢会计）：
//   (a) 六通道标记（object 侧 +272/276/280 与容器侧 base+256/260/264）全部为 0
//       ⇒ 这台机子上没有任何「正在排入 / 正在交付」的条目；并且
//   (b) 四个会计字里至少一个非 0（已投递 +284 / 条目+24，已交付未完成 +288 / 条目+28）；并且
//   (c) 连续 N 次观测到完全同一组数值 —— 生产者投递只跨几条指令，它只要动过任何一个字，
//       指纹就变了、计数从头再来，所以正常流水线不会被误清。
// 满足后每个非 0 的字各退 1（绝不回退到负），一次只退一份，慢慢把陈旧残留抽干。
// 一旦某个通道被 arm（(a) 不成立），立刻停手，交还 guest 自己的流水线。
//
// [T29] 两个开关的分工（都默认开；都不改 guest 控制流，只在交换/门的热路上写几个会计字）：
//   · sa2_renut_display_release —— 显示**完成侧**：把「已经交付给显示侧、却没有任何完成
//     事件来记它完成」的那一份会计退掉（base+288 与条目 +28 各退 1）。Xbox 上这一步由
//     GPU 垂直同步/完成中断驱动（ISR 族 sub_82997AF8/sub_82997968/sub_829975F0 在生成代码
//     里没有任何 guest 调用者），Switch 后端没有对应来源 ⇒ 实机停在 cola284=0、cola288=1
//     （1 份无人回收），tick sub_82ABB7D0 → sub_82882200 的 while 判据
//     *(base+284)+*(base+288) != 0 因此永不为 0，帧循环死在第一帧（0 fps、有声无画）。
//   · sa2_renut_display_agent —— 陈旧「已投递未交付」残留（base+284 / 条目 +24）的抽干（T28）。
// 两者合起来就是「宿主扮演这台机子的显示完成侧」，与门钩子的落点一致（实机卡死以后 Swap
// 已经不再发生，唯一仍高频存活的挂点就是门）。
// [T31] 「当前显示对象」单元 = 0x82DBB608。取证：sub_8287E410（44.cpp:90596）用「换入 *(0x82DBB608)
//   → sub_8287DD08 处理 → 换回」证明它是「当前显示对象」；每帧 tick sub_82ABB7D0（29.cpp:122845）与
//   sub_828812D0（22.cpp:95629）也写它，其伙伴单元成片在 tick 里被清零 ⇒ 它是按帧建立/撤销的临时态。
//   生成代码里**找不到独立创建者**：本移植缺掉初始化它的 guest 路径时它可能为 0，而完成回调链
//   （门 sub_82881B88 就绪 → 泵 sub_82881E00 → 交付 sub_82881930 → 对象更新 sub_8287DD08 →
//    sub_8287BF88 首条语句先取 *(0x82DBB608) 再 +(-18936) 取全局指针、再 +872 取链表头）
//   在 0 上就会走到 *(0+0x368) 访问违例（实机 far=image 基址+0x368 吻合）。守卫据此短路。
constexpr u32 kDisplayObjectCell = 0x82DBB608u;

// [T31] 崩溃守卫（默认 true）：当前显示对象为 0 时，跳过完成回调链上的四个 guest 函数；
// 关掉它 = 恢复原样直调（仅供对照）。cvar 名字 = toml 键名，也可用环境变量 SA2_RENUT_DISPLAY_GUARD=1。
REXCVAR_DEFINE_BOOL(sa2_renut_display_guard, true, "SA2/Renderer",
                    "[T31] guard the display completion callback chain when *(0x82DBB608)==0: skip "
                    "sub_8287BF88/sub_8287DD08/sub_8287E410 (avoid *(0+0x368) guest fault) and skip "
                    "sub_8287D7C0 on null r3.");
std::atomic<u32> g_display_release_n{0};
std::atomic<u32> g_display_agent_n{0};
u32 DisplayAgentStep(u32 origen /* 0 = Swap，1 = Puerta */) {
  const u32 base = kDisplayQueueBase;
  if (LoadGuestU32(base) != base)
    return 0u;  // 队列还没初始化

  u32 markers[6] = {0, 0, 0, 0, 0, 0};
  u32 ctx_object = 0;
  DisplayMarkerSnapshot(markers, ctx_object);
  bool armado = false;
  for (u32 i = 0; i < 6u; ++i)
    armado = armado || markers[i] != 0u;

  const u32 idx = LoadGuestU32(kDisplayEntryIdxVa);
  const u32 entry = DisplayQueueEntry(idx);
  const u32 posted_base = LoadGuestU32(base + kOffQueuePosted);
  const u32 inflight_base = LoadGuestU32(base + kOffQueueInflight);
  const u32 entry_posted = entry ? LoadGuestU32(entry + 24u) : 0u;
  const u32 entry_inflight = entry ? LoadGuestU32(entry + 28u) : 0u;

  static std::atomic<u32> racha[2] = {std::atomic<u32>{0}, std::atomic<u32>{0}};
  static std::atomic<u32> huella[2] = {std::atomic<u32>{0}, std::atomic<u32>{0}};

  const bool sucio =
      !armado && (posted_base || inflight_base || entry_posted || entry_inflight);
  if (!sucio) {
    huella[origen].store(0, std::memory_order_relaxed);
    racha[origen].store(0, std::memory_order_relaxed);
    return 0u;
  }

  const u32 huella_ahora = posted_base ^ (inflight_base << 8) ^ (entry_posted << 16) ^
                           (entry_inflight << 24);
  if (huella[origen].load(std::memory_order_relaxed) != huella_ahora) {
    huella[origen].store(huella_ahora, std::memory_order_relaxed);
    racha[origen].store(0, std::memory_order_relaxed);
    return 0u;
  }

  // [T29] 门（origen=1）在卡死态被以每秒 4,300,000 次的量级调用，把门槛从 32 降到 8，让卡死的帧循环
  // 在毫秒级被抽干一次；Swap（origen=0）保持 4。指纹必须连续相同，正常流水线不会被误动。
  const u32 objetivo = (origen == 0u) ? 4u : 8u;
  if (racha[origen].fetch_add(1, std::memory_order_relaxed) + 1u < objetivo)
    return 0u;
  racha[origen].store(0, std::memory_order_relaxed);

  // [T31] 这里曾是 T28/T29 的「宿主代客写会计」（退 base+288/条目 +28 与 base+284/条目 +24）。
  // 实机取证证明盲写不可用，且会在门热路上把同一个会计字扣到回绕：
  //   · 门路径（origen=1）在帧循环卡死时被以约 10 万次/秒的量级调用（与 yield 计数同量级），
  //     把 base+288 由 1 刷过 0 并回绕成 0xFFFFFFFF ⇒ guest 排空循环
  //     while (*(base+284)+*(base+288) != 0) 永不退出（Sphaira 启动：无限黑屏、无任何加载）；
  //   · 若只清一次让门提前就绪，guest 会走进「当前显示对象为空」的完成回调链并在 *(0+0x368)
  //     上访问违例（前端启动：直接报错退出）。
  // T31 起此函数**只读不写**：会计完全交还 guest 自己（真正的显示完成事件来源见报告下一步），
  // 写会计的开关默认关闭，仅保留复现取证用。上面两个统计量改名为「观察计数」。
  const u32 observados = (posted_base ? 1u : 0u) + (inflight_base ? 1u : 0u) +
                         (entry_posted ? 1u : 0u) + (entry_inflight ? 1u : 0u);
  const u32 n = g_display_release_n.fetch_add(1, std::memory_order_relaxed) + 1u;
  if (n <= 16u || (n % 8192u) == 0u) {
    RGPU_WARN("[renut] T2DIAG cola-readonly n={} src={} idx={} cnt={}/{}/{} obj256={}/{}/{} "
              "cola284={} cola288={} e24={} e28={} obs={} onj={}",
              n, origen == 0u ? "swap" : "gate", idx, markers[0], markers[1], markers[2], markers[3],
              markers[4], markers[5], posted_base, inflight_base, entry_posted, entry_inflight,
              observados, LoadGuestU32(kDisplayObjectCell));
  }
  return 0u;
}

void D3DDevice_Swap_hook(u32 device_va, u32 front_buffer, u32 /*parameters*/) {
  // sa2-nx (2026-10-20, sa2_renut_core_layout, the Switch): once, the guest main thread (SA2's game and drawing thread,
  // about a core of work in City Escape) alone on core 0, the other guest threads on cores 1-2 (preferring 2: the
  // render thread is pinned to core 1 at a higher priority). Before, every guest thread could run on any core, and at
  // their priority (0x3B) Horizon time-slices the threads of a core in 10 ms slices.
  static bool core_layout_done = false;
  if (!core_layout_done) {
    core_layout_done = true;
#if REX_PLATFORM_SWITCH
    if (REXCVAR_GET(sa2_renut_core_layout)) {
      const uint32_t moved = RexSetGuestThreadCoreLayout(0, 0x6, 2);
      RGPU_INFO("[renut] sa2_renut_core_layout: the guest main thread on core 0, the other guest threads on cores "
                  "1-2 ({} threads set)", moved);
    }
#endif
  }
  // sa2-nx: the console's vertical blank paced SA2's frames (its game speed follows them): sa2_renut_pacing.cpp.
  sa2::renut::PaceSwap(device_va);
  // sa2-nx: SA2's game frame count (src/sa2_d3d_trace.cpp: the flip, which only the GPU's vblank interrupt reaches).
  const uint64_t game_frame = g_sa2_game_frames.fetch_add(1, std::memory_order_relaxed) + 1;
  // sa2-nx (1.0.3.1): the PC's docked / handheld test toggles (sa2_render_test_toggle_at; nothing on the Switch).
  sa2::render::OnPresented(game_frame);
  // sa2-nx: a lockstep recording (sa2_capture.cpp) takes this very frame: asked before it is queued.
  const uint64_t capture_ticket = sa2::capture::RenutLockstepOn() ? RequestFrameCapture() : 0;
  renut::frame_stats::OnGameFrame();
  rex::latency::NoteGuestSwap();  // banjo-nx (2026-10-19, input latency): the probe
  const uint64_t swap_ordinal = rex::latency::SwapCount();
  FlushDeferredVertices(device_va);
  SnapshotDeltasNewFrame();  // sa2-nx (2026-10-20, sa2_renut_snapshot_deltas)

  // What the original does after handing the frame to the video hardware:
  // count the swap, record the frame's fence tag, and end the frame with a
  // KickOff, whose fence completes once the render thread gets there.
  auto *device = GuestPtr<u8>(device_va);
  u32 completed_swap = 0;
  if (device) {
    auto word = [device](u32 offset) -> be_u32 & {
      return *reinterpret_cast<be_u32 *>(device + offset);
    };
    if (REXCVAR_GET(sa2_renut_swap_callthrough)) {
      // [T25] guest 交换体 sub_822D9D70 也会自增同一个字（dev+16560），本开关把它放跑，
      // 宿主就让出这次自增，避免双计。
      completed_swap = u32(word(d3d::kDeviceOffsetSwapCount));
    } else {
      completed_swap = u32(word(d3d::kDeviceOffsetSwapCount)) + 1;  // [T2ROTF] LOW: 16560 按 −408 规则（16564/16568 有 822D9688/822D9570 见证，T17 核对）
      word(d3d::kDeviceOffsetSwapCount) = completed_swap;  // [T2ROTF] LOW: 16560 按 −408 规则（16564/16568 有 822D9688/822D9570 见证，T17 核对）
    }
    const u32 fence_tag = word(d3d::kDeviceOffsetFenceTag);
    word(d3d::kDeviceOffsetSwapFenceTag) = fence_tag;
    word(d3d::kDeviceOffsetPresentFenceTag) = fence_tag;
    device[d3d::kDeviceOffsetSwapFlags] &= u8(~d3d::kSwapFlagPending);
    // sa2-nx (2026-10-20): the display gamma ramp SA2's SetGammaRamp keeps in the device (sub_825527D8: +15408, 768
    // words; N&B's +0x3A9C), which the original Swap writes to the display controller's table, goes to the
    // scan-out (backend/present.cpp, sa2_renut_gamma_ramp).
    if (REXCVAR_GET(sa2_renut_gamma_ramp))
      Video::SetGammaRamp(device + kSa2DeviceOffsetGammaRamp);  // [T2ROTF] UNRESOLVED: T2 的 gamma ramp 槽未定（14940 属 ring 族，不是 ramp）
    // banjo-nx (package 27c): the frame's last vertex and texture fills are read here, before the Swap's fence
    // completes. They were read at the present, after it: the game, told the frame was done, could rewrite a dynamic
    // buffer for the next frame first, and the frame's last draws (the composite, the HUD) took the new bytes.
    banjo::renut_gpu::checks::QueueKickFlush();
    KickFence(device);
    // sa2-nx: banjo-nx's banjo_renut_game_gamma (N&B's +0x3A9C) is not taken; SA2's ramp goes to the present above
    // (sa2_renut_gamma_ramp).
  }

  // [T24] 取证：每 2 s 一行，证明 Swap 仍在被调用、以及显示会计有没有推进。
  {
    static std::atomic<u64> n_swaps{0};
    static std::atomic<i64> ultimo{-100000};
    const u64 seq = n_swaps.fetch_add(1, std::memory_order_relaxed) + 1;
    const i64 ahora = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();
    i64 previo = ultimo.load(std::memory_order_relaxed);
    if (ahora - previo >= 2000 && ultimo.compare_exchange_strong(previo, ahora)) {
      RGPU_WARN("[renut] T2DIAG SWAP n={} dev={:#010x} front={:#010x} sub16560={} disp16568={} "
                "fr16564={} f10941={:#04x}",
                seq, device_va, front_buffer, LoadGuestU32(device_va + 16560),
                LoadGuestU32(device_va + 16568), LoadGuestU32(device_va + 16564),
                device_va ? u32(*GuestPtr<u8>(device_va + d3d::kDeviceOffsetSwapFlags)) : 0u);
      // [T28] 同一限频窗口内再打一行显示队列稳态值（这是判死锁/判修复是否生效的直接证据：
      // tick 空转的判据就是 cola284+cola288 / e24+e28 非 0，而门要的六通道标记应长期为 0）。
      if (LoadGuestU32(kDisplayQueueBase) == kDisplayQueueBase) {
        u32 markers[6] = {0, 0, 0, 0, 0, 0};
        u32 ctx_object = 0;
        DisplayMarkerSnapshot(markers, ctx_object);
        const u32 idx = LoadGuestU32(kDisplayEntryIdxVa);
        const u32 entry = DisplayQueueEntry(idx);
        RGPU_WARN("[renut] T2DIAG COLA idx={} cnt={}/{}/{} obj256={}/{}/{} 284={} 288={} e24={} e28={} "
                  "st0={} dctx={:#010x} rel={} ag={} tf={}",
                  idx, markers[0], markers[1], markers[2], markers[3], markers[4], markers[5],
                  LoadGuestU32(kDisplayQueueBase + kOffQueuePosted),
                  LoadGuestU32(kDisplayQueueBase + kOffQueueInflight),
                  entry ? LoadGuestU32(entry + 24u) : 0u, entry ? LoadGuestU32(entry + 28u) : 0u,
                  LoadGuestU32(kDisplayCtx), kDisplayCtx, g_display_release_n.load(std::memory_order_relaxed),
                  g_display_agent_n.load(std::memory_order_relaxed), LoadGuestU32(kDisplayEntryIdxVa));
      }
    }
  }

  // [T28/T29] 宿主扮演显示 / 垂直同步侧。Swap 钩子每 2 s 才打一行诊断，但它本身仍会被调用；
  // 不过实机日志 [renut] T2DIAG SWAP 只有 n=1 一行 ⇒ 卡死以后 Swap 根本不再发生，
  // 所以真正干活的调用点在下边的门钩子上（门在卡死态被以每秒 4,300,000 次的量级调用）。
  // 这里保留一次调用，覆盖「Swap 仍然活着」的正常态（宿主呈现了一次 = 显示完成了一次）。
  if (REXCVAR_GET(sa2_renut_display_release) || REXCVAR_GET(sa2_renut_display_agent))
    DisplayAgentStep(0);

  render_thread::QueueFrame([front_buffer, swap_ordinal, device_va, completed_swap] {
    PresentImage image;
    {
      std::lock_guard lock(State().mutex);
      image = FrontBufferImageLocked(front_buffer);
      if (image.texture)
        State().last_front_buffer = front_buffer;
      AdvanceTextureFrameLocked();
      AdvanceStreamCacheFrameLocked();
      LatchResolutionScaleLocked();
    }
    MaybeReportMemory();
    // The front buffer is the texture the game resolved the frame into.
    Video::NotePresentingSwap(swap_ordinal);  // banjo-nx (2026-10-20): the probe's "input -> GPU done"
    Video::Present(image.texture ? &image : nullptr);
    // [T24] T2 自己的显示会计（T2AdvanceDisplayAccounting 的注释里有逐字节来源）：
    // guest 的垂直同步处理程序 sub_822D9570 与呈现统计 sub_822D9688 才是推进
    // 16548 / 16564 / 16568 并清掉帧完成字 *(*(dev+10900)+4) 的人，而它们的调用者是
    // GPU 垂直同步中断，Switch 侧没有任何后端会派发。原来往 +16976 发布的是 MGR 的
    // 偏移，在 T2 镜像里全库 0 命中（污染了无关设备字段，guest 也永远等不到显示确认）。
    // [v8.6-FLIP] 宿主直写显示会计字段（16564/16568、*(blk+4)）的历史行为到此默认关闭：
    // 这些字段的负责人是 guest 自己的 sub_822D9688 / sub_822D9570，现由 t2rotf_gpu_signal.cpp
    // 的 PresentarGuest 按刷新节奏代客调用 guest 代码完成（宿主不写字段）。要 A/B 复现旧行为
    // 打开 sa2_renut_display_agent（默认 false）即可。
    if (REXCVAR_GET(sa2_renut_display_agent))
      T2AdvanceDisplayAccounting(device_va, completed_swap);
    {
      std::lock_guard lock(State().mutex);
      ::renut::gpu::SyncPipelineListLocked();  // banjo-nx (2026-10-20): the list's new records to the card
    }
    rex::latency::NotePresented(swap_ordinal);  // banjo-nx (2026-10-19, input latency)
    banjo::display_sync::NotePresented(swap_ordinal);  // banjo-nx (2026-10-19, latency batch 3)
  });
  RunReadyCallbacks();

  if (capture_ticket) {
    rex::ui::RawImage image;
    const bool ok = WaitFrameCapture(capture_ticket, image, 2000);
    sa2::capture::RenutLockstepFrame(game_frame, ok ? &image : nullptr);
  }

  // sa2-nx: no ResetAllState (N&B's dirty masks; nothing reads them here: the draws read the device's state, and
  // SA2's own setters keep marking it).
  if (device)
    native::RewindRing(native::DeviceAt(device_va));
}

// void D3DDevice_InsertCallback(D3DDevice*, D3DCALLBACKTYPE, D3DCALLBACK pCallback,
//                               DWORD Context) [0x821FB688]
// The GPU would call pCallback(Context) on reaching this point in the ring,
// and the caller may reuse whatever the preceding work reads once it runs: it
// runs once the render thread has read that work's data (callbacks.h). N&B's
// callers: EndTiling (D3D's WorkerDpc) and the XMV renderer
// (CCalVideoRenderer::Render 0x82A0D9E8, which counts frames in flight until
// IXMediaXmvPlayer_Resume and frees itself after the last).
void D3DDevice_InsertCallback_hook(u32 device, u32 /*type*/, u32 callback, u32 context) {
  if (!callback)
    return;
  FlushDeferredVertices(device);
  RunReadyCallbacks();
  InsertCallback(callback, context);
}

// ---------------------------------------------------------------------------
// Device queries, thread ownership, presentation state
// ---------------------------------------------------------------------------

constexpr u32 kDeviceOffsetReferenceCount = 0x3C;
constexpr u32 kDeviceOffsetOwnerThread = 0x2A88;  // [T2ROTF] KEEP: 0x2A88 与 CreationThreadId 的 T2 槽同值（T2 只留一个线程 id 槽）
constexpr u32 kDeviceCapsVa = 0x82183B00;  // N&B's constant D3DCAPS9  // [T2ROTF] UNRESOLVED: host 写入 0x130 字节的 caps 区未在 T2 定位（有越写风险，T17 核对）
constexpr u32 kDeviceCapsSize = 0x130;
constexpr u32 kFusionVa = 0x82F597B4;      // non-zero on a Fusion GPU  // [T2ROTF] UNRESOLVED: 只读标志，错值无害

// ULONG D3DDevice_AddRef(D3DDevice*) [0x8264E068]
u32 D3DDevice_AddRef_hook(u32 device_va) {
  native::Device d = native::DeviceAt(device_va);
  if (!d)
    return 0;
  const u32 count = d.W(kDeviceOffsetReferenceCount) + 1;
  d.SetW(kDeviceOffsetReferenceCount, count);
  return count;
}

// void D3DDevice_AcquireThreadOwnership(D3DDevice*) [0x8264E0D8]
void D3DDevice_AcquireThreadOwnership_hook(u32 device_va) {
  native::Device d = native::DeviceAt(device_va);
  if (d)
    d.SetW(kDeviceOffsetOwnerThread, GuestGetCurrentThreadId());  // [T2ROTF] KEEP: 0x2A88 与 CreationThreadId 的 T2 槽同值（T2 只留一个线程 id 槽）
}

// void D3DDevice_ReleaseThreadOwnership(D3DDevice*) [0x8264E118]
void D3DDevice_ReleaseThreadOwnership_hook(u32 device_va) {
  native::Device d = native::DeviceAt(device_va);
  std::atomic_thread_fence(std::memory_order_seq_cst);  // eieio
  if (d)
    d.SetW(kDeviceOffsetOwnerThread, 0);  // [T2ROTF] KEEP: 0x2A88 与 CreationThreadId 的 T2 槽同值（T2 只留一个线程 id 槽）
}

// void D3DDevice_GetDeviceCaps(D3DDevice*, D3DCAPS9*) [0x82658948]
void D3DDevice_GetDeviceCaps_hook(u32 /*device*/, u32 caps_va) {
  auto *caps = GuestPtr<u8>(caps_va);
  const auto *source = GuestPtr<const u8>(kDeviceCapsVa);  // [T2ROTF] UNRESOLVED: host 写入 0x130 字节的 caps 区未在 T2 定位（有越写风险，T17 核对）
  if (caps && source)
    std::memcpy(caps, source, kDeviceCapsSize);
}

// void D3DDevice_GetDisplayMode(D3DDevice*, UINT iSwapChain, D3DDISPLAYMODE*)
// [0x82658960]: width, height, refresh rate, format.
void D3DDevice_GetDisplayMode_hook(u32 device_va, u32 /*swap_chain*/, mapped_u32 mode) {
  const native::Device d = native::DeviceAt(device_va);
  if (!d || !mode)
    return;
  mode[0] = d.W(0x34E8);
  mode[1] = d.W(0x34EC);
  mode[2] = d.W(0x5434);
  mode[3] = d.W(0x34F0);
}

// void D3DDevice_BlockOnFence(DWORD Fence) [0x8228F3B0]: on the device D3D
// finds through VdGlobalDevice.
void D3DDevice_BlockOnFence_hook(u32 fence) {
  if (fence)
    native::BlockOnFence(native::GlobalDevice(), fence);
}

// void D3D::SynchronizeToPresentationInterval(CDevice*, BOOL) [0x8228A748]:
// its swap callback (SwapCallback) and the waits around it go into the ring;
// the swap is then pending.
void D3D_SynchronizeToPresentationInterval_hook(u32 device_va, u32 /*immediate*/) {
  native::Device d = native::DeviceAt(device_va);
  if (!d)
    return;
  native::SetPredication(d, 0);
  d.SetB(native::off::Flags1, d.B(native::off::Flags1) | d3d::kSwapFlagPending);
}

// void D3DResource_GetDevice(D3DResource*, D3DDevice**) [0x82654D50]: the
// device D3D finds through VdGlobalDevice, referenced.
void D3DResource_GetDevice_hook(u32 /*resource*/, mapped_u32 out_device) {
  const u32 device = native::GlobalDevice();
  if (device)
    D3DDevice_AddRef_hook(device);
  if (out_device)
    *out_device = device;
}

// BOOL D3D::IsFusion() [0x82B6BF60]
u32 D3D_IsFusion_hook() {
  return LoadGuestU32(kFusionVa) != 0 ? 1 : 0;  // [T2ROTF] UNRESOLVED: 只读标志，错值无害
}

// D3D's gamma tables (sub_82659140, _Gamma_sRGB 0x82659090): 1024 entries
// taking a 10-bit value through the display's gamma (VdGetCurrentDisplayGamma).
// Inverse goes from the display's curve to linear. D3D caches the table per
// display gamma (unk_82FB2558); it is computed per call here.
float Fmsubs(float a, float b, float c) {
  return std::fmaf(a, b, -c);
}

float Bits(u32 bits) {
  return std::bit_cast<float>(bits);
}

float GammaSRGB(float v, bool inverse) {
  if (inverse) {
    return v > 0.04045 ? float(std::pow(double((v + Bits(0x3D6147AE)) * Bits(0x3F72A76F)), 2.4000000953674316))
                       : v * Bits(0x3D9E8391);
  }
  return v > 0.0031308 ? Fmsubs(float(std::pow(double(v), 0.4166666567325592)), Bits(0x3F870A3D),
                                Bits(0x3D6147AE))
                       : v * Bits(0x414EB852);
}

std::array<u16, 1024> GammaTable(bool inverse) {
  // Guest words for the kernel's outputs.
  static const u32 scratch = [] {
    void *host = records::AllocRaw(8);
    return host ? REX_KERNEL_MEMORY()->HostToGuestVirtual(host) : 0u;
  }();
  u32 type = 0;
  float gamma = 0.0f;
  if (scratch) {
    VdGetCurrentDisplayGamma(scratch, scratch + 4);
    type = LoadGuestU32(scratch);
    gamma = std::bit_cast<float>(LoadGuestU32(scratch + 4));
  }
  std::array<u16, 1024> table{};
  for (u32 i = 0; i < 1024; ++i) {
    float v = float(i) * Bits(0x3A802008);  // flt_8219F708, 1 / 1023
    if (!inverse) {
      v = v > 0.04045 ? float(std::pow(double((v + Bits(0x3D6147AE)) * Bits(0x3F72A76F)), 2.4000000953674316))
                      : v * Bits(0x3D9E8391);
    }
    float out;
    if (type == 1) {
      out = GammaSRGB(v, inverse);
    } else if (type == 3) {
      out = float(std::pow(double(v), double(inverse ? gamma : 1.0f / gamma)));
    } else if (inverse) {
      out = v < 0.0812 ? v * Bits(0x3E638E39)
                       : float(std::pow(double((v + Bits(0x3DCAC083)) * Bits(0x3F68F065)), 2.222222328186035));
    } else {
      out = v < 0.018 ? v * 4.5f
                      : Fmsubs(float(std::pow(double(v), 0.44999998807907104)), Bits(0x3F8CAC08),
                               Bits(0x3DCAC083));
    }
    if (inverse) {
      out = out > 0.0031308 ? Fmsubs(float(std::pow(double(out), 0.4166666567325592)), Bits(0x3F870A3D),
                                     Bits(0x3D6147AE))
                            : out * Bits(0x414EB852);
    }
    const i32 value = native::TruncateToI32(std::fma(double(out), 1023.0, 0.5));
    table[i] = u16(std::clamp(value, 0, 0x3FF));
  }
  return table;
}

// The device's gamma (+0x3A9C, red, green, blue: 256 words each) for the
// display to pick up; the renderer presents without it.
constexpr u32 kDeviceOffsetGammaRamp = 0x3A9C;  // [T2ROTF] UNRESOLVED: 同上（0x3A9C=14940 在 T2 是 ring 字段）

void StoreGamma(native::Device d, const be_u16 (&corrected)[768]) {
  if (std::memcmp(corrected, d.bytes + kDeviceOffsetGammaRamp, sizeof(corrected)) == 0)  // [T2ROTF] UNRESOLVED: 同上（0x3A9C=14940 在 T2 是 ring 字段）
    return;
  if (KeGetCurrentProcessType() != 2) {
    // The display's pending copy takes the previous ramp first.
    const u32 pending = d.W(0x5D90);
    if (pending && !(d.W(0x5D94) & 0x80000000u)) {
      if (auto *dest = GuestPtr<u8>(pending))
        std::memcpy(dest, d.bytes + kDeviceOffsetGammaRamp, sizeof(corrected));  // [T2ROTF] UNRESOLVED: 同上（0x3A9C=14940 在 T2 是 ring 字段）
    }
    d.SetW(0x5D94, d.W(0x5D94) | 0x80000000u);
  }
  std::memcpy(d.bytes + kDeviceOffsetGammaRamp, corrected, sizeof(corrected));  // [T2ROTF] UNRESOLVED: 同上（0x3A9C=14940 在 T2 是 ring 字段）
  d.SetW(0x4144, d.W(0x4144) | 2);
}

// D3DDevice_SetGammaRamp's work: the ramp (D3DGAMMARAMP, 3 x 256 words)
// through the gamma table (GammaRampCorrected 0x826593F8).
void SetGammaRamp(native::Device d, const u16 *ramp) {
  const std::array<u16, 1024> table = GammaTable(false);
  be_u16 corrected[768];
  for (u32 i = 0; i < 768; ++i)
    corrected[i] = u16((table[ramp[i] >> 6] & 0x3FF) << 6);
  StoreGamma(d, corrected);
}

// D3DDevice_SetPWLGamma's: D3DPWLGAMMA (3 x 128 {base, delta}) through the
// table (sub_82659490), each segment's end clamped to 0x3FF.
void SetPWLGamma(native::Device d, const u16 *pwl) {
  const std::array<u16, 1024> table = GammaTable(false);
  be_u16 corrected[768];
  for (u32 c = 0; c < 3; ++c) {
    for (u32 i = 0; i < 128; ++i) {
      const u32 at = c * 256 + i * 2;
      const u32 base = pwl[at] >> 6;
      const u32 end = std::min(base + (pwl[at + 1] >> 6), 0x3FFu);
      corrected[at] = u16((table[base] & 0x3FF) << 6);
      corrected[at + 1] = u16(((table[end] - table[base]) & 0x3FF) << 6);
    }
  }
  StoreGamma(d, corrected);
}

void ReadRamp(u32 ramp_va, u16 (&ramp)[768]) {
  const auto *guest = GuestPtr<const be_u16>(ramp_va);
  for (u32 i = 0; i < 768; ++i)
    ramp[i] = guest ? u16(guest[i]) : 0;
}

// void D3DDevice_SetGammaRamp(D3DDevice*, DWORD Flags, const D3DGAMMARAMP*)
// [0x8264D948]
void D3DDevice_SetGammaRamp_hook(u32 device_va, u32 /*flags*/, u32 ramp_va) {
  native::Device d = native::DeviceAt(device_va);
  if (!d || !ramp_va)
    return;
  u16 ramp[768];
  ReadRamp(ramp_va, ramp);
  SetGammaRamp(d, ramp);
}

// void D3DDevice_SetPWLGamma(D3DDevice*, DWORD Flags, const D3DPWLGAMMA*)
// [0x8264DA18]
void D3DDevice_SetPWLGamma_hook(u32 device_va, u32 /*flags*/, u32 pwl_va) {
  native::Device d = native::DeviceAt(device_va);
  if (!d || !pwl_va)
    return;
  u16 pwl[768];
  ReadRamp(pwl_va, pwl);
  SetPWLGamma(d, pwl);
}

// The D3D internals Direct3D_CreateDevice and D3DDevice_Release run, for any
// other caller.
void D3D_XenonGpu_Initialize_hook(u32 device) {
  XenonGpuInitialize(native::DeviceAt(device));
}
void D3D_InitializeApiState_hook(u32 device) {
  InitializeApiState(native::DeviceAt(device));
}
u32 D3D_InitializePresentationParameters_hook(u32 device, u32 parameters) {
  return InitializePresentationParameters(native::DeviceAt(device), parameters) ? 1 : 0;
}
void D3D_CDevice_Uninitialize_hook(u32 device) {
  UninitializeDevice(native::DeviceAt(device));
}

} // namespace

namespace renut::gpu::native {

void RewindRing(Device device) {
  if (!device || !g_ring_sink)
    return;
  device.SetW(off::Ring, g_ring_sink);
  device.SetW(off::RingLimit, g_ring_sink + kRingSinkSize);
  device.SetW(off::RingGuarantee, g_ring_sink + kRingSinkSize - kRingSinkGuaranteeMargin);
}

u32 KickOff(u32 device_va) {
  return KickOff_hook(device_va);
}

void BlockOnFence(u32 device_va, u32 fence) {
  BlockOnFence_hook(device_va, fence, 0, 0);
}

u32 GlobalDevice() {
  // 与 guest 的读取点同构：先看当前进程类型对应的槽，另一个槽做兜底（只有标题设备会被发布）。
  const u32 proc_type = KeGetCurrentProcessType();
  const u32 slots[2] = {proc_type == 2 ? kVdGlobalDeviceImportSlotProc2 : kVdGlobalDeviceImportSlotOther,
                        proc_type == 2 ? kVdGlobalDeviceImportSlotOther : kVdGlobalDeviceImportSlotProc2};
  for (u32 slot : slots) {
    const u32 cell = LoadGuestU32(slot);
    const u32 device = cell ? LoadGuestU32(cell) : 0;
    if (device)
      return device;
  }
  return 0;
}

void Swap(u32 device_va, u32 front_buffer, u32 parameters) {
  D3DDevice_Swap_hook(device_va, front_buffer, parameters);
}

// CDevice::SetFence (0x82278528): with immediate fences the device writes the
// completion itself (unless +0x541C is set); otherwise the fence completes on
// the render thread, as a KickOff's. A device recording a command buffer
// would put it into the buffer, which runs on the host.
void SetFence(u32 device_va) {
  auto *bytes = GuestPtr<u8>(device_va);
  if (!bytes)
    return;
  if (bytes[kDeviceOffsetFlags2] & kDeviceFlagImmediateFences) {
    Device d = DeviceAt(device_va);
    const u32 fence = d.W(off::FenceTag);
    if (!d.W(0x541C)) {
      if (auto *completion = GuestPtr<be_u32>(d.W(off::FenceCompletion))) {
        completion[0] = fence;
        completion[1] = (d.W(0x3A48) & 3) | d.W(off::Ring);
      }
    }
    d.SetW(off::FenceTag, fence + 2);
    return;
  }
  if (!IsRecordingCommandBuffer(device_va))
    KickDevice(GuestPtr<d3d::Device>(device_va), device_va);
}

} // namespace renut::gpu::native

// ---------------------------------------------------------------------------
// sa2-nx (2026-10-20): SA2's hooks
// ---------------------------------------------------------------------------
// SA2's Direct3D (2.0.21076) is N&B's code with a shifted private layout (d3d.h), so the hooks above run as they are at
// SA2's addresses (each read from SA2's code; N&B's in brackets). What differs:
//   - Creation: SA2's own Direct3D_CreateDevice builds a device without hardware (its type-2 path: the device block,
//     XenonGpu_Initialize and InitializeApiState, natively in N&B's hook), then the title device gets what the
//     hardware path (sub_8255CDA0) gives it apart from the GPU: critical sections, VdGlobalDevice, a fence word, the
//     timer frequency, and SA2's own InitializePresentationParameters (sub_826C0C48: the automatic back buffer and
//     SA2's automatic front buffer, a texture the Present resolves into) and InitializeApiState again, the hardware
//     path's order.
//   - The state setters, Clear's and Resolve's device work and the Present stay SA2's code: they write the device's
//     shadows, which the draws read, and their packets go into the ring sink. Clear and Resolve queue the host work
//     first (sa2_renut_hooks.cpp).
//   - SA2's ring segment allocator (sub_826B45A8) rewinds the sink like the others.

REX_EXTERN(__imp__sub_822D1640);  // Direct3D_CreateDevice, the original

REX_EXTERN(__imp__sub_822E1240);  // D3D::InitializePresentationParameters (CDevice*, D3DPRESENT_PARAMETERS*)

REX_EXTERN(__imp__sub_822E0F60);  // D3D::InitializeApiState (CDevice*)


namespace {

// HRESULT Direct3D_CreateDevice(UINT Adapter, D3DDEVTYPE DeviceType, void*, DWORD BehaviorFlags,
//     D3DPRESENT_PARAMETERS*, IDirect3DDevice9 **ppReturnedDeviceInterface) [sub_826B2418; N&B 0x8264DF28]
void Sa2CreateDevice(PPCContext &ctx, uint8_t *base) {
// T2ROTF (2026-10-25): 钩子地址与设备布局都是针对用户这份 T2ROTF 镜像（TU 的
  // patched/default.xex）测出来的。换一份二进制就先拒绝，不要替换任何设备/GPU 行为。
  static const bool guest_code_matches = [base] {
    for (const auto &guard : t2::kGuestCodeGuards) {
      uint32_t hash = 2166136261u;
      for (uint32_t i = 0; i < guard.bytes; ++i)
        hash = (hash ^ base[guard.address + i]) * 16777619u;
      if (hash != guard.fnv32) {
        RGPU_ERROR("T2ROTF hook validation failed: {} at {:#010x}; expected {:08x}, got {:08x}",
                   guard.name, guard.address, guard.fnv32, hash);
        return false;
      }
    }
    RGPU_INFO("T2ROTF (TU patched/default.xex): all {} guest interface guards match",
              std::size(t2::kGuestCodeGuards));
    return true;
  }();
  if (!guest_code_matches) {
    ctx.r3.u64 = 0x80004005u;
    return;
  }
// T2ROTF (2026-10-26): T2 的 Direct3D_CreateDevice 是四参 ABI（r3=DeviceType、r4=BehaviorFlags、
  // r5=D3DPRESENT_PARAMETERS*、r6=ppReturnedDeviceInterface），不是参考端的六参布局。证据在 sub_822D1640
  // 本体（generated/default/T2ROTF_recomp.0.cpp:20394 起）：入口 *(u32*)r6=0（输出槽清零）、r28=r5、r30=r4、
  // r27=r6、类型 2 分支 cmpwi cr6,r29,2（r29=r3）、出口 *(u32*)r27=r31（设备）后返回 0。
  // 原来按参考端布局从 r8 取输出槽：两个静态调用点（sub_828816D8 / sub_82A311C8）都不给 r8 赋值，
  // 它是上一处调用的残留（=9），宿主于是解引用 guest 0x9 —— 就是实机 pc=imagen+0x2dbe354 `ldur w0,[x0,#9]`。
  const u32 device_type = ctx.r3.u32;
  u32 behavior_flags = ctx.r4.u32;
  const u32 present_parameters = ctx.r5.u32;
  const u32 out_va = ctx.r6.u32;
  const bool title_device = device_type != kDeviceTypeNoHardware;
  RGPU_INFO("Direct3D_CreateDevice: r3={:#x} r4={:#010x} r5={:#010x} r6={:#010x} r7={:#x} r8={:#x} lr={:#010x} title={}",
            ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32,
            static_cast<u32>(ctx.lr), title_device);
  if (!records::Init()) {
    ctx.r3.u64 = kE_OUTOFMEMORY;
    return;
  }
  if (!g_ring_sink)
    g_ring_sink = records::AllocPhysicalBlock(kRingSinkSize);
  if (!g_ring_data)
    g_ring_data = records::AllocPhysicalBlock(kRingDataSize);
  const u32 fence_va = records::AllocPhysicalBlock(0x80 + 0x20);
  if (!g_ring_sink || !g_ring_data || !fence_va) {
    RGPU_ERROR("Direct3D_CreateDevice: guest allocation failed");
    ctx.r3.u64 = kE_OUTOFMEMORY;
    return;
  }
  std::memset(GuestPtr<u8>(g_ring_sink), 0, kRingSinkSize);
  std::memset(GuestPtr<u8>(fence_va), 0, 0x80 + 0x20);

// The no-hardware device T2's own D3D and the title share (its device-type-2 path).
  ctx.r3.u64 = kDeviceTypeNoHardware;  // T2ROTF: T2 的四参 ABI 里设备类型在 r3（参考端布局在 r4），强制走无硬件分支，r4 的 flags 原样留给原函数
  __imp__sub_822D1640(ctx, base);

  if (ctx.r3.u32 != 0) {
    RGPU_ERROR("Direct3D_CreateDevice: guest create failed, r3={:#010x}", ctx.r3.u32);
    return;
  }
  if (out_va < 0x00010000u || (out_va & 3u)) {  // 上一处调用的残留值（这次是 9）不是 guest 输出槽；解引用它只会把宿主打崩
    RGPU_ERROR("Direct3D_CreateDevice: r6={:#010x} is not a guest output slot; not dereferencing", out_va);
    ctx.r3.u64 = 0x80004003u;  // E_POINTER
    return;
  }
  const u32 device_va = LoadGuestU32(out_va);
  native::Device d = native::DeviceAt(device_va);
  if (!d) {
    ctx.r3.u64 = kE_OUTOFMEMORY;
    return;
  }
  u8 *bytes = d.bytes;
  // Its ring was the side buffer, marked out of memory (sub_8254D040): from here the sink. Fences complete as they
  // are written until the title device is set up.
  bytes[kDeviceOffsetFlags2] &= u8(~0x20);
  bytes[kDeviceOffsetFlags2] |= kDeviceFlagImmediateFences;
  WriteU32(bytes, kDeviceOffsetFenceCompletion, fence_va);
  WriteU32(bytes, kDeviceOffsetSwapScratchWord, fence_va + 0x80);  // the swap's scratch word (SynchronizeToPresentationInterval)
  native::RewindRing(d);

  if (title_device) {
    RtlInitializeCriticalSection(device_va + kDeviceOffsetCriticalSection0);  // [T2ROTF] T2=14944: T2 硬件路径 sub_822E1690 以设备为基初始化
    RtlInitializeCriticalSection(device_va + kDeviceOffsetCriticalSection1);  // [T2ROTF] T2=14972: 同上；sub_822D3630 / sub_822D37D0 以 dev+14972 为锁
    PublishGlobalDevice(device_va);
    if (!(behavior_flags & 0x100) && !(behavior_flags & 0x3F000000))
      behavior_flags |= 0xC000000;
    WriteU32(bytes, d3d::kDeviceOffsetBehaviorFlags, behavior_flags);
    WriteU32(bytes, kDeviceOffsetsMinusOne[0], 0xFFFFFFFFu);
    WriteU32(bytes, kDeviceOffsetsMinusOne[1], 0xFFFFFFFFu);
    const float frequency = float(KeQueryPerformanceFrequency());
    d.SetF(kDeviceOffsetPerfFrequency, frequency);
    d.SetF(kDeviceOffsetPerfFrequency + 4, frequency > 0.0f ? 1.0f / frequency : 0.0f);
    if (present_parameters) {
      ctx.r3.u64 = device_va;
      ctx.r4.u64 = present_parameters;
      __imp__sub_822E1240(ctx, base);

      if (!ctx.r3.u32) {
        RGPU_ERROR("Direct3D_CreateDevice: InitializePresentationParameters failed");
        ctx.r3.u64 = kE_OUTOFMEMORY;
        return;
      }
    }
    ctx.r3.u64 = device_va;
    __imp__sub_822E0F60(ctx, base);

    native::RewindRing(d);
    // From here the title device's fences complete on the render thread (KickFence).
    bytes[kDeviceOffsetFlags2] &= u8(~kDeviceFlagImmediateFences);
    Video::SetGuestDevice(device_va);
  }
  RGPU_INFO("Guest D3D device (type {}) created at {:#010x}: ring {:#010x}/{:#010x} fence {:#010x} scratch {:#010x} flags2 {:#04x}",
            device_type, device_va, d.W(native::off::Ring), d.W(native::off::RingLimit),
            d.W(kDeviceOffsetFenceCompletion), d.W(kDeviceOffsetSwapScratchWord), d.B(kDeviceOffsetFlags2));
  ctx.r3.u64 = 0;  // S_OK
}

// SA2's ring segment allocator (sub_826B45A8 (CDevice*, DWORD Dwords): KickOffSegment, BeginRingBig and Swap call it
// for a new segment): the sink again. Non-zero: the callers retry the ring, which has the room.
u32 Sa2NewSegment_hook(u32 device_va, u32 /*dwords*/) {
  native::Device d = native::DeviceAt(device_va);
  if (!d)
    return 0;
  d.SetB(kDeviceOffsetFlags2, d.B(kDeviceOffsetFlags2) & u8(~0x20));
  native::RewindRing(d);
  return d.W(native::off::Ring) + 4;
}

// D3D::SynchronizeToPresentationInterval(CDevice*, BOOL) [sub_826B3050; N&B 0x8228A748]: its packets (the swap
// callback and the waits) are the GPU's; the swap is pending.
void Sa2SynchronizeToPresentationInterval_hook(u32 device_va, u32 /*immediate*/) {
  native::Device d = native::DeviceAt(device_va);
  if (d)
    d.SetB(d3d::kDeviceOffsetSwapFlags, d.B(d3d::kDeviceOffsetSwapFlags) | d3d::kSwapFlagPending);
}

}  // namespace

BANJO_RENUT_HOOK_RAW(sub_822D1640) {  // Direct3D_CreateDevice [0x8264DF28]

  Sa2CreateDevice(ctx, base);
}
// [T2ROTF] BANJO_RENUT_HOOK(sub_826B59B0, D3DDevice_InsertCallback_hook);  // D3DDevice_InsertCallback [0x821FB688]  // DROPPED: T2 镜像里没有对应地址（0x822D3000-0x822D4B00 逐条排查无果）
BANJO_RENUT_HOOK(sub_822D9838, Sa2SynchronizeToPresentationInterval_hook);  // D3D_SynchronizeToPresentationInterval [0x8228A748]

BANJO_RENUT_HOOK(sub_822D4388, KickOff_hook);  // D3D_CDevice_KickOff [0x82210AB0]

BANJO_RENUT_HOOK(sub_822D4000, KickOffSegment_hook);  // D3D_CDevice_KickOffSegment [0x822106C8]

BANJO_RENUT_HOOK(sub_822D3058, BeginRingBig_hook);  // D3D_CDevice_BeginRingBig [0x821F7C18]

BANJO_RENUT_HOOK(sub_822D1AF0, RingBufferDeviceAllocate_hook);  // D3D_RingBufferDeviceAllocate [0x8222D3E0]

BANJO_RENUT_HOOK(sub_822D3630, AllocateInlineData_hook);  // D3D_CDevice_AllocateInlineData [0x8222D240]

BANJO_RENUT_HOOK(sub_822D37D0, Sa2NewSegment_hook);  // SA2's ring segment allocator

BANJO_RENUT_HOOK_SHARED(sub_822D4A08, D3DDevice_SetRingBufferParameters_hook);  // D3DDevice_SetRingBufferParameters [0x82657508]

// [T25] D3DDevice_Swap [0x82287590]：宿主整体替换 guest 交换体 sub_822D9D70。
// 从 BANJO_RENUT_HOOK 改成显式 REX_FUNC，是为了让两个实验开关能拿到活的 ctx / base
// （宏把 ctx/base 封在 REX_FUNC 体内，外面追加不了 guest 调用）。
//
// 注意：ctx 在 HostToGuestFunction 里只有 r3（返回值）会被改，r1/r2/r13 与 r14-r31
// 不会被宿主函数动，所以额外那次 guest 调用直接复用同一个 ctx 就带着有效的 guest 栈，
// 不用另造栈；调用发生在钩子返回前，clobber 掉的也只是易失寄存器。
extern "C" REX_FUNC(sub_822D9D70) {
  if (!::banjo::renut_gpu::Active()) {
    __imp__sub_822D9D70(ctx, base);
    return;
  }
  ::rex::ppc::HostToGuestFunction<D3DDevice_Swap_hook>(ctx, base);
  if (REXCVAR_GET(sa2_renut_swap_callthrough)) {
    static std::atomic<u32> avisos_callthrough{0};
    if (avisos_callthrough.fetch_add(1, std::memory_order_relaxed) < 8) {
      RGPU_WARN("[renut] T2DIAG callthrough sub_822D9D70 dev={:#010x}", ctx.r3.u32);
    }
    __imp__sub_822D9D70(ctx, base);
  }
  if (REXCVAR_GET(sa2_renut_display_tick)) {
    static std::atomic<u32> avisos_display_tick{0};
    if (avisos_display_tick.fetch_add(1, std::memory_order_relaxed) < 8) {
      RGPU_WARN("[renut] T2DIAG display-tick sub_82881E00 ctx={:#010x} st0={} cnt={}/{}/{}",
                kDisplayCtx, LoadGuestU32(kDisplayCtx), LoadGuestU32(LoadGuestU32(kDisplayCtx + 16) + 272),
                LoadGuestU32(LoadGuestU32(kDisplayCtx + 16) + 276),
                LoadGuestU32(LoadGuestU32(kDisplayCtx + 16) + 280));
    }
    // 与游戏自己的 tick sub_82ABB7D0 里的那一次调用同形：单趟排空（r5=1 有界）。
    // [T28] 修正：container 是 0x82E55E20（kDisplayCtx 已更正），原来是 0x82E55EE0，
    // 门读到的 *(container+16) 是垃圾，等于空转，所以这个开关以前开与不开都一样。
    ctx.r3.u32 = kDisplayCtx;
    ctx.r4.u32 = 0xFFFFFFFFu;
    ctx.r5.u32 = 1;
    __imp__sub_82881E00(ctx, base);
  }
}

// [T28] 门 sub_82881B88：所有自旋的必经点，也是本次修复的主战场（实机门在卡死态被高频调用，
// 而 Swap 已经不再发生）。宿主代理只在「六通道全 0 且四个会计字里有残留」并且**连续 32 次
// 观测到完全同一组数值**时才各退 1；真实投递只跨几条指令，动过任何一个字指纹就变、计数重来，
// 所以不会和真实投递抢会计。其余情况完全交给原体，控制流一个字节都不改。
BANJO_RENUT_HOOK_RAW(sub_82881B88) {
  if (REXCVAR_GET(sa2_renut_display_release) || REXCVAR_GET(sa2_renut_display_agent)) {
    // 先只读四个会计字：全 0 就直接走原体，不给转得飞快的门热路加负担。
    //
    // [T29] 关键修正（T28 的这一步只看了 posted 两个「已投递」字，实机卡死态的残留恰恰在
    //       inflight 两个「已交付未完成」字上 —— cola284=0、entry+24=0、cola288=1，
    //       于是 T28 的代理在实机上一次都没被叫到，框循环照样空转。四个字一起看才对。）
    const u32 base = kDisplayQueueBase;
    if (LoadGuestU32(base) == base) {
      const u32 entry = DisplayQueueEntry(LoadGuestU32(kDisplayEntryIdxVa));
      const u32 posted = LoadGuestU32(base + kOffQueuePosted);
      const u32 inflight = LoadGuestU32(base + kOffQueueInflight);
      const u32 entry_posted = entry ? LoadGuestU32(entry + 24u) : 0u;
      const u32 entry_inflight = entry ? LoadGuestU32(entry + 28u) : 0u;
      if (posted || inflight || entry_posted || entry_inflight)
        DisplayAgentStep(1);
    }
  }
  __imp__sub_82881B88(ctx, base);
}

BANJO_RENUT_HOOK_SHARED(sub_822D4488, BlockOnFence_hook);  // D3D_CDevice_BlockOnFence [0x8228F558]

BANJO_RENUT_HOOK_SHARED(sub_822D13F0, CBlocker_Check_hook);  // D3D_CBlocker_Check [0x8264DCD0]

BANJO_RENUT_HOOK(sub_822D4920, IsFencePending_hook);  // D3DDevice_IsFencePending [0x8228F650]

// -------------------------------------------------------------------------------------------------
// [T31] 显示完成链守卫（sa2_renut_display_guard，默认 true）。
// 取证：前端启动在一次显示会计被清掉后崩在 image+0x2a5444c（= 当前构建符号表里的
//   __imp__sub_8287BF88+0x7c），esr=0x92000005、far=0x63a5422368（= guest 基址 0x63a5422000 + 0x368），
//   现场 x0=x1=0x368、x2=0x63a5422368、x20=0x63a5422000 ⇒ 崩在 *(0+0x368)。
//   sub_8287BF88（generated/default/T2ROTF_recomp.43.cpp:91679）的**第一条语句**就是
//   REX_LOAD_U32(*(0x82DBB608) - 18936) —— 直接解引用「当前显示对象」单元（0x82DBB608）再取链表头，
//   该单元为 0 时必然走到 0+0x368 访问违例。生成代码里找不到初始化该单元的独立路径，故本移植下它会为 0。
// 守卫：该单元为 0 时短路完成回调链上的四个 guest 函数（不调原体），并按限频记一行 GUARDIA 便于复核。
static std::atomic<u32> g_guard_n{0};

static void GuardSkipLog(u32 va, const char* fun) {
  const u32 n = g_guard_n.fetch_add(1, std::memory_order_relaxed) + 1u;
  if (n <= 32u || (n % 4096u) == 0u) {
    RGPU_WARN("[renut] T2DIAG GUARDIA n={} fun={} va={:#x} onj={} cola284={} cola288={}", n, fun, va,
              LoadGuestU32(kDisplayObjectCell), LoadGuestU32(kDisplayQueueBase + 284u),
              LoadGuestU32(kDisplayQueueBase + 288u));
  }
}

// 完成回调链：门 sub_82881B88 → 泵 sub_82881E00 → 交付 sub_82881930 → 对象更新 sub_8287DD08
// → sub_8287BF88（上面那条崩点）。链上凡是解引用「当前显示对象」的，都在它为空时短路。
BANJO_RENUT_HOOK_RAW(sub_8287BF88) {
  if (REXCVAR_GET(sa2_renut_display_guard) && LoadGuestU32(kDisplayObjectCell) == 0u) {
    GuardSkipLog(0x8287BF88u, "sub_8287BF88");
    return;
  }
  __imp__sub_8287BF88(ctx, base);
}

BANJO_RENUT_HOOK_RAW(sub_8287DD08) {
  if (REXCVAR_GET(sa2_renut_display_guard) && LoadGuestU32(kDisplayObjectCell) == 0u) {
    GuardSkipLog(0x8287DD08u, "sub_8287DD08");
    return;
  }
  __imp__sub_8287DD08(ctx, base);
}

// sub_8287E410 是「换入当前显示对象（0x82DBB608 = r3）→ sub_8287DD08 → 换回」的包装：r3 为 0 时
// 它自己就会把 0 装进单元，随后调用 sub_8287DD08 必然解引用 0，故同样短路。
BANJO_RENUT_HOOK_RAW(sub_8287E410) {
  if (REXCVAR_GET(sa2_renut_display_guard) &&
      (LoadGuestU32(kDisplayObjectCell) == 0u || ctx.r3.u32 == 0u)) {
    GuardSkipLog(0x8287E410u, "sub_8287E410");
    return;
  }
  __imp__sub_8287E410(ctx, base);
}

// 同文件 44.cpp:90596 的邻居：r3 为空指针时直接短路（防 *(0) 族解引用）。
BANJO_RENUT_HOOK_RAW(sub_8287D7C0) {
  if (REXCVAR_GET(sa2_renut_display_guard) && ctx.r3.u32 == 0u) {
    GuardSkipLog(0x8287D7C0u, "sub_8287D7C0");
    return;
  }
  __imp__sub_8287D7C0(ctx, base);
}

