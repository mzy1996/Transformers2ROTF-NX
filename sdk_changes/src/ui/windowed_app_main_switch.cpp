/**
 * @file        ui/windowed_app_main_switch.cpp
 * @brief       Entry point for windowed applications on Nintendo Switch
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <algorithm>
#include <clocale>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>

#include <cstdio>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/windowed_app.h>
#include <rex/ui/windowed_app_context_switch.h>

// Only the syscall header: switch.h drags typedefs that collide with rex.
extern "C" {
#include <switch/types.h>
Result svcSetHeapSize(void** out_addr, u64 size);
// libnx runtime internals used for the exit below.
extern u32 __nx_applet_exit_mode;
NX_NORETURN void __libnx_exit(int rc);
}

namespace {

// Returning from main is not a clean exit for this runtime on Switch. libnx
// never runs .fini_array, so static destructors do not run, and any thread
// owned by a global object (the TimerQueue dispatcher, and in a title the
// guest, GPU and audio threads) outlives the NRO. hbloader then loads hbmenu
// into the same process while those threads still own stacks aliased out of
// the heap, and hbmenu data-aborts on the first heap page it touches (seen on
// console with uiprobe; the Atmosphere report showed the TimerQueue thread
// still waiting in TimerThreadMain). The process has to end instead, which
// takes every thread down with it.
//
// Ending it directly is not right either: svcExitProcess, with or without
// __appExit first, is reported by the system as "closed because an error
// occurred", with no crash report (seen on console). An application must ask
// the system to close it. libnx does that when __nx_applet_exit_mode is 1: its
// applet cleanup installs _appletExitProcess as the exit function, which sends
// ISelfController::Exit and sleeps until the system terminates the process
// (read from libnx's _appletCleanup in the linked binary). __libnx_exit runs
// that path after closing the services, without returning to hbloader.
[[noreturn]] void ExitProcess(int result) {
  REXLOG_INFO("Switch: exiting process ({})", result);
  rex::FlushLogging();
  std::fflush(nullptr);
  __nx_applet_exit_mode = 1;
  __libnx_exit(result);
}

}  // namespace

int main(int argc, char* argv[]) {
  // v6.29-LOCALEFIX: newlib's _svfprintf_r dereferences localeconv()->lconv at
  // entry, before it parses the format string. Without this, __global_locale
  // reaches that code uninitialized on this port, so *every* printf-family call
  // (including the very first fprintf below) Data Aborted, and the crash-log
  // formatter - also stdio-based at the time - aborted a second time before it
  // could write rex_crash.log. Initialize the C locale as the very first thing.
  std::setlocale(LC_ALL, "C");
  {
    setvbuf(stderr, nullptr, _IONBF, 0);
    FILE* f = fopen("/sdmc/rex_diag_main.txt", "w");
    if (f) { fprintf(f, "main() reached\n"); fflush(f); fclose(f); }
    fflush(nullptr);
  }
  /* NOTE: svcSetHeapSize must NOT be called after libc malloc has initialized
   * (which happens in __libc_init_array before main). Doing so leaves
   * newlib's _sbrk heap_top stale and causes a Data Abort (EC 0x24) inside
   * _malloc_r. The only safe places to shrink are: (1) before __libnx_init
   * (override __libnx_init/crt0), or (2) rely on __nx_heap_size when not
   * launched via hbloader. For now we run with hbloader's ~3.2 GB heap;
   * guest_memory_switch.cpp already commits memory in 2 MB blocks to keep
   * LimitableResource_Memory usage low. If 0xCE01 (out of code-memory quota)
   * appears later, we will revisit early-init shrinking.
   */
  auto remaining = rex::cvar::Init(argc, argv);
  rex::cvar::ApplyEnvironment();
  rex::InitLoggingEarly();

  int result;
  {
    rex::ui::SwitchWindowedAppContext app_context;
    if (!app_context.Initialize()) {
      ExitProcess(EXIT_FAILURE);
    }

    std::unique_ptr<rex::ui::WindowedApp> app = rex::ui::GetWindowedAppCreator()(app_context);

    // Match remaining positional args to the app's expected options.
    const auto& option_names = app->GetPositionalOptions();
    std::map<std::string, std::string> parsed;
    size_t count = std::min(remaining.size(), option_names.size());
    for (size_t i = 0; i < count; ++i) {
      parsed[option_names[i]] = remaining[i];
    }
    app->SetParsedArguments(std::move(parsed));

    const bool initialized = app->OnInitialize();
    if (!initialized) {
      REXLOG_ERROR("Switch: app initialization failed");
    }
    result = initialized ? app_context.RunMainMessageLoop() : EXIT_FAILURE;

    app->InvokeOnDestroy();
  }
  ExitProcess(result);
}
