// T2ROTF (2026-10-24): what Renut's renderer (src/renut_engine) still references from the SA2 port it
// comes from (tuancapoo/mgr-nx-deko, app/src/renut_engine), and what this port has no counterpart for.
//
// The renderer is compiled here as it is; the symbols below are the few whose real definitions are
// SA2's own code or a game-specific native module. Filled from the linker's undefined symbols, one
// by one (the same way mgr_sa2_stubs.cpp was), so each is the smallest thing that lets the renderer
// link, and nothing pretends to do work.

#include <atomic>
#include <cstdint>

#include <rex/cvar.h>

#include "sa2_capture.h"

// Renut's own key (gpu/banjo_renut_record.cpp declares it): the renderer's fast paths against their
// original implementations. Only the first-use checks run with it off, which is this port's setting.
REXCVAR_DEFINE_BOOL(banjo_cpu_verify, false, "T2ROTF",
                    "Check every renderer fast path against its original implementation (diagnostic)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// The game's frames at its presentation interval: gpu/d3d/hooks/device.cpp counts it once per
// presented frame, and sa2's recorder (above) waits on it. Nothing records here, so it is only a
// free-running counter.
std::atomic<uint64_t> g_sa2_game_frames{0};

namespace sa2::capture {

// The frame recorder (SA2's video tool, src/sa2_capture.cpp) is not part of this port: the renderer
// asks for lockstep every frame it presents, and the answer is always "not recording".
bool RenutLockstepOn() { return false; }

void RenutLockstepFrame(uint64_t game_frame, const rex::ui::RawImage* image) {
  (void)game_frame;
  (void)image;
}

}  // namespace sa2::capture

// The renderer's tuning keys (gpu/banjo_renut_tuning.cpp, through sa2_compat/cpu/banjo_enhancements.h)
// are active when their own key is on AND their group's master is on. SA2's sa2_enhancements.cpp - the
// registry, the external-key table and the 10 s "[enh] uses" report - is not compiled in this port (it
// includes rex/sys_counters.h and SA2's generated-code checks, neither of which this SDK tree has), so
// only the two masters are answered here: from the same cvar names and with the same reference default
// (on), latch-read once like SA2's own Resolve() does. A banjo_renut_*/banjo_gpu_* key stays switchable
// per key, and sa2.toml can still turn a whole group off.
namespace {
REXCVAR_DEFINE_BOOL(sa2_cpu_enhancements, true, "SA2 enhancements",
                    "Master switch of the renderer's CPU fast paths (each also has its own banjo_* key)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(sa2_noncpu_enhancements, true, "SA2 enhancements",
                    "Master switch of the renderer's non-CPU paths (each also has its own banjo_* key)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
}  // namespace

namespace sa2::enh {

bool CpuMaster() {
  static const bool on = REXCVAR_GET(sa2_cpu_enhancements);
  return on;
}

bool NonCpuMaster() {
  static const bool on = REXCVAR_GET(sa2_noncpu_enhancements);
  return on;
}

}  // namespace sa2::enh

// SA2's render resolution per console mode and MSAA (me/sa2_render_mode.cpp, "Render resolution and MSAA") is SA2's own
// enhancement and is not compiled in this port: its file includes cpu/sa2_enhancements.h (the registry this port stubs
// above) and its dynamic resolution following would be an extra GPU cost this port does not want. These three answers
// are what the renderer asks for, and this port keeps the game's own render resolution in both modes:
//   - Docked: the console's real mode, as SA2's did (libnx's cached operation mode; Reverse-NX obeyed, as the window is).
//   - RenderHeightOf: always the game's own 720 (SA2 would return sa2_render_height_{docked,handheld}). The caller turns
//     it into a render-scale code through RenderScale::FromRatio (720/720 -> 0, the game's own), so the renderer draws
//     at 1280x720 docked and handheld, and only the swapchain follows the display's size.
//   - OnPresented: SA2 counted presented swaps for its PC-only dock-toggle tests. Nothing to do here.
#if REX_PLATFORM_SWITCH
#include "me/me_platform_compat.h"  // <switch.h> and the Switch thread helpers
#include <rex/ui/switch_saltynx.h>
#endif

namespace sa2::render {

bool Docked() {
#if REX_PLATFORM_SWITCH
  return rex::ui::switch_saltynx::ModoBase(appletGetOperationMode() == AppletOperationMode_Console);
#else
  return true;
#endif
}

int32_t RenderHeightOf(bool docked) {
  (void)docked;
  return 720;  // the game's own height in both modes (see above)
}

void OnPresented(uint64_t presented) { (void)presented; }

}  // namespace sa2::render