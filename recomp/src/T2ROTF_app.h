// T2ROTF - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.
//
// Graphics: Renut's Direct3D-level renderer through plume's deko3d back end
// (src/renut_engine, cmake/t2_renut_gpu.cmake). The emulated Xenos GPU - PM4 ring and
// EDRAM over NVK - is gone from this build: on the console it never presented a frame
// (the guest's ring-space gate polled a word the host never published), and the port
// keeps going without a Vulkan driver at all (REXGLUE_SWITCH_NO_NVK). The wiring below
// is the SDK's detached-mode contract, as mgr-nx-deko uses it (app/mgr_src/mgr_main.cpp):
// OnPreSetup clears the graphics system and the GPU plugin, the app supplies the overlay
// drawer, and the renderer takes the window once the paths are final.

#pragma once

#include <cstdio>
#include <functional>
#include <optional>

#include <rex/cvar.h>
#include <rex/rex_app.h>
#include <rex/system/xthread.h>
#include <rex/ui/switch_apm.h>

#include "renut_engine/gpu/banjo_renut_select.h"
#include "renut_engine/gpu/overlay/app_integration.h"
#include "t2rotf_gpu_signal.h"

class T2rotfApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<T2rotfApp>(new T2rotfApp(ctx, "T2ROTF",
        PPCImageConfig));
  }

  // Override virtual hooks for customization:
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostLoadXexImage() override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;

  static void sd_log(const char* msg) {
    FILE* f = fopen("/sdmc/t2rotf_diag.txt", "a");
    if (f) { fputs(msg, f); fputc('\n', f); fflush(f); fclose(f); }
  }

  // On the Switch, request the official handheld performance profile through
  // Horizon's apm API: GPU 307.2 -> 460.8 MHz while the memory stays at 1331.2
  // MHz (configuration 0x92220008, the same pair a commercial game uses). This
  // is NOT an overclock - no KIP or sysmodule is touched, it goes through the
  // official API - and it is the single biggest step toward a steady 60 FPS at
  // stock clocks in handheld mode (+50 % GPU). RAM is left untouched on
  // purpose: our measurements say bandwidth is not the limit, and 1600 MHz
  // would only cost battery and heat.
  void OnPostInitLogging() override {
    sd_log("[T2ROTF] OnPostInitLogging");
#if REX_PLATFORM_SWITCH
    // Without a GPU plugin the runtime discards its graphics calls with "no GPU
    // emulation loaded". That plugin is gone from this build on purpose: the game
    // is rendered by Renut (which hooks the guest's Direct3D directly), so the cvar
    // is only set when that renderer is not the one in charge.
    if (!banjo::renut_gpu::Active() &&
        rex::cvar::GetFlagInfo("gpu_plugin") != nullptr &&
        !rex::cvar::HasNonDefaultValue("gpu_plugin")) {
      rex::cvar::SetFlagByName("gpu_plugin", "xenos");
    }
    RexSwitchApmPedirGpuMhz(460);
    RexSwitchApmPermitirRam1600(0);
    RexSwitchApmAplicar();
    sd_log("[T2ROTF] APM applied");
#endif
  }

  // Detached mode: no SDK graphics system and no GPU plugin (ReXApp only loads the
  // plugin when config.graphics is empty, and Renut clears both).
  void OnPreSetup(rex::RuntimeConfig& config) override {
    sd_log("[T2ROTF] OnPreSetup");
    if (banjo::renut_gpu::Active()) {
      renut::gpu::ConfigureRuntime(config);
      // [T44 / v8.0-GPUIRQ] ConfigureRuntime clears config.graphics because Renut
      // replaces the guest's Direct3D itself, but that also took away the two
      // things the guest's *display* path needs from the graphics system:
      //   - the GPU register MMIO window at 0x7FC80000, whose interrupt-status
      //     word (0x7FC86544, register 0x1951) is the only gate in front of the
      //     guest's display ISR sub_822D9570, and
      //   - the per-display-refresh dispatch of the guest's graphics interrupt
      //     callback (xboxkrnl VdSetGraphicsInterruptCallback dropped it
      //     silently while graphics_system was null).
      // Without them the guest's display side never advances: dev+16548 froze at
      // its init value, sub_822D9570 never ran, the frame loop spun on a gate
      // that can never become ready, and the result was 0 fps and a black screen.
      // The installed system restores exactly those two and leaves presentation
      // to Renut (presenter() stays null).
      if (auto gpu_signal = t2rotf::gpu_signal::Create()) {
        config.graphics = std::move(gpu_signal);
        sd_log("[T2ROTF] GPU signal source installed (MMIO 0x7FC80000 + vblank callback)");
      } else {
        sd_log("[T2ROTF] GPU signal source disabled (sa2_renut_gpu_signal=false)");
      }
      sd_log("[T2ROTF] reNut renderer: no presenter, no GPU plugin");
      return;
    }
    (void)config;
  }

  void OnPostSetup() override {
    sd_log("[T2ROTF] OnPostSetup");
#if REX_PLATFORM_SWITCH
    // Work around WVC1 (VC-1 Advanced Profile + B-frames) videos that the
    // recompiled decoder cannot handle: redirect them to a WMV3 stand-in.
    // This lets the game continue past the intro and cutscenes instead of
    // hanging at 0 fps with a black screen.  The stand-in video will show
    // the wrong picture; full fix requires an FFmpeg VC-1 bridge (TODO).
    auto* vfs = REX_KERNEL_FS();
    if (vfs) {
      vfs->RegisterSymbolicLink("game:\\movies\\luxo_logo.wmv",
                                  "game:\\movies\\atvi_logo.wmv");
      vfs->RegisterSymbolicLink("game:\\movies\\a0_intro_co.wmv",
                                  "game:\\movies\\atvi_logo.wmv");
      vfs->RegisterSymbolicLink("game:\\movies\\A6_END_CO.wmv",
                                  "game:\\movies\\atvi_logo.wmv");
      vfs->RegisterSymbolicLink("game:\\movies\\D6_END_CO.wmv",
                                  "game:\\movies\\atvi_logo.wmv");
      vfs->RegisterSymbolicLink("game:\\movies\\Unlock_G1Ep_1.wmv",
                                  "game:\\movies\\atvi_logo.wmv");
      vfs->RegisterSymbolicLink("game:\\movies\\Unlock_G1Ep_2.wmv",
                                  "game:\\movies\\atvi_logo.wmv");
      vfs->RegisterSymbolicLink("game:\\movies\\Unlock_G1Ep_3.wmv",
                                  "game:\\movies\\atvi_logo.wmv");
      vfs->RegisterSymbolicLink("game:\\movies\\Unlock_G1Ep_4.wmv",
                                  "game:\\movies\\atvi_logo.wmv");
      vfs->RegisterSymbolicLink("game:\\movies\\Unlock_G1Ep_5.wmv",
                                  "game:\\movies\\atvi_logo.wmv");
      vfs->RegisterSymbolicLink("game:\\movies\\Unlock_G1Ep_6.wmv",
                                  "game:\\movies\\atvi_logo.wmv");
      sd_log("[T2ROTF] WVC1 sym-links registered");
    }
#endif
  }

  void OnLoadXexImage(std::string& xex_image) override {
    sd_log("[T2ROTF] OnLoadXexImage");
    (void)xex_image;
  }

  void OnPostLoadXexImage() override {
    sd_log("[T2ROTF] OnPostLoadXexImage");
  }

  void OnPreLaunchModule() override {
    sd_log("[T2ROTF] OnPreLaunchModule");
  }

  // The overlay drawer lives on Renut's plume device, not in the SDK presenter.
  std::unique_ptr<rex::ui::ImmediateDrawer> OnCreateImmediateDrawer() override {
    if (banjo::renut_gpu::Active()) {
      sd_log("[T2ROTF] OnCreateImmediateDrawer: reNut overlay drawer");
      return renut::gpu::CreateOverlayDrawer();
    }
    return ReXApp::OnCreateImmediateDrawer();
  }

  void OnWindowPixelSizeChanged(uint32_t pixel_width,
                                uint32_t pixel_height) override {
    ReXApp::OnWindowPixelSizeChanged(pixel_width, pixel_height);
    if (banjo::renut_gpu::Active()) {
      renut::gpu::OnWindowResized();
    }
  }

  // The UI thread, once the window exists (OnFinalizePaths runs after the window is
  // up): the host device is created here and starts presenting.
  std::optional<rex::PathConfig> OnFinalizePaths(
      const rex::PathConfig& defaults,
      std::function<void(rex::PathConfig)> resume) override {
    if (banjo::renut_gpu::Active() &&
        !renut::gpu::StartRenderer(app_context(), [this] { return imgui_drawer(); })) {
      sd_log("[T2ROTF] StartRenderer FAILED");
      throw std::runtime_error("T2ROTF: failed to start the reNut graphics backend");
    }
    return ReXApp::OnFinalizePaths(defaults, std::move(resume));
  }

  // Removing the emulated GPU changes which host threads the SDK's round-robin sees,
  // and the game's job scheduler is indexed by the guest PCR's logical CPU: pin the
  // guest's main thread before it starts, as mgr-nx-deko does. This is guest-visible
  // identity, not host core pinning.
  void OnPostLaunchModule(rex::system::XThread* thread) override {
    if (thread && banjo::renut_gpu::Active()) {
      thread->SetAffinity(1u);
      sd_log("[T2ROTF] main guest thread on logical CPU 0");
    }
  }

  void OnShutdown() override {
    sd_log("[T2ROTF] OnShutdown");
    if (banjo::renut_gpu::Active()) {
      renut::gpu::ShutdownRenderer();
    }
  }

  // If --game_data_root was not given, use the extracted game folder next to
  // the executable: on the Switch the NRO lives in
  // sdmc:/switch/Transformers2ROTF/, with a game/ folder beside it containing
  // default.xex. In the independent-directory layout, update/, patched/ and
  // dlc/ sit next to game/ as separate top-level folders. SetupVfs mounts
  // update/ and patched/ as overlay layers in front of game/ (so
  // game:\default.xex resolves to patched/default.xex when present), and
  // enumerates dlc/ subdirectories as marketplace content. The old
  // installation-time merge into a single game/ directory is no longer used.
  // Without this, starting without --game_data_root dies with
  // "--game_data_root was not provided." because SetupEnvironment only looks
  // at the cvar and, if it is empty, nothing else sets the folder.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    if (!paths.game_data_root.empty()) {
      return;  // given on the command line; it takes precedence.
    }
    std::error_code ec;
    const auto carpeta = rex::filesystem::GetExecutableFolder();
    if (carpeta.empty() || !std::filesystem::is_directory(carpeta, ec)) {
      return;
    }
    const auto extraida = carpeta / "game";
    if (std::filesystem::is_directory(extraida, ec)) {
      paths.game_data_root = extraida;
    }
  }
};