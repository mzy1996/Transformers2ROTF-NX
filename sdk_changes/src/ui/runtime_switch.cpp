/**
 * @file        rex/os/switch/runtime_switch.cpp
 * @brief       libnx startup constants for the Switch executable
 *
 * This file is compiled directly into the final executable, not into a
 * library.
 *
 * Everything here replaces weak libnx symbols, and crt0 only references them
 * weakly. A weak reference does not pull an object out of a static library, so
 * if this ended up in rexcore.a the linker would silently ignore it and the
 * defaults would be used: no error, no warning, and the process starting in a
 * different state than expected. skate3recomp-nx solves it the same way, adding
 * it to the game target.
 *
 * The values are those of MarathonRecomp-NX, a finished Horizon port.
 */

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <exception>

#include <cxxabi.h>

#include <switch.h>

extern "C" void RexSwitchCrashLog(const char* reason, const ThreadExceptionDump* ctx, u64 stack,
                                  u64 pc);

namespace {

/*
 * An uncaught exception. devkitA64's libstdc++ handler writes nothing
 * (rex_stderr.log came out empty when SetupVfs let a filesystem_error
 * escape), so the type and what() are recorded here; for std::filesystem
 * errors what() includes the path. It ends with error 2345-0104.
 */
[[noreturn]] void RexSwitchTerminate() {
  char reason[768] = "std::terminate sin excepcion activa";
  if (std::exception_ptr current = std::current_exception()) {
    const std::type_info* type = abi::__cxa_current_exception_type();
    int status = -1;
    char* demangled = type ? abi::__cxa_demangle(type->name(), nullptr, nullptr, &status) : nullptr;
    const char* type_name = demangled ? demangled : (type ? type->name() : "?");
    try {
      std::rethrow_exception(current);
    } catch (const std::exception& e) {
      std::snprintf(reason, sizeof(reason), "std::terminate: %s: %s", type_name, e.what());
    } catch (...) {
      std::snprintf(reason, sizeof(reason), "std::terminate: %s", type_name);
    }
    std::free(demangled);
  }
  RexSwitchCrashLog(reason, nullptr, reinterpret_cast<u64>(__builtin_frame_address(0)),
                    reinterpret_cast<u64>(__builtin_return_address(0)));
  diagAbortWithResult(MAKERESULT(Module_Libnx, 104));
}

__attribute__((constructor(101))) void RexSwitchInstallTerminate() {
  std::set_terminate(RexSwitchTerminate);
}

}  // namespace

extern "C" {

void __nx_win_init(void);

/*
 * Declared as an application instead of an applet. It goes with title takeover,
 * which was measured to be mandatory: in applet mode the process gets 400 MB, and
 * guest physical memory alone is 512.
 */
u32 __nx_applet_type = AppletType_Application;

/*
 * hbloader pre-inflates the heap to ~3.2 GB before the NRO entry point.
 * That consumes almost all LimitableResource_Memory mapping quota and leaves
 * almost nothing for svcMapProcessCodeMemory, which later fails with 0xCE01
 * (KERNELERROR_RESOURCE_EXHAUSTED). Shrinking to 1.5 GB releases ~1.7 GB of
 * mapping quota and physical memory for guest memory shadow mappings.
 *
 * Mesa/NVK allocations use Vulkan memory allocators, not libc heap, so they are
 * unaffected by this shrink. The remaining 1.5 GB covers host reserve,
 * 128 MB prealloc backing and on-demand commit backing.
 */
size_t __nx_heap_size = (1ull * 1024 * 1024 * 1024) + (512ull * 1024 * 1024);   /* 1.5 GB */

/*
 * hbloader ignores __nx_heap_size; libnx also only calls svcSetHeapSize after
 * __appInit, but by then the heap is already 3.2 GB. Shrink it here, before
 * libc's malloc arena is set up, so the freed mapping quota is available for
 * guest memory shadow mappings. svcSetHeapSize on a shrink does not move the
 * heap base, so _sbrk will see the new limit when __libc_init_array runs.
 */
Result __appInit(void) {
    void* addr = nullptr;
    Result rc = svcSetHeapSize(&addr, __nx_heap_size);
    if (R_FAILED(rc)) {
        return rc;
    }
    /*
     * The heap shrink above is the only reason this override exists. The rest
     * must match what libnx's default __appInit does: spin up the services and
     * mount the SD. Without sm/fs init the first mkdir()/fopen() in a
     * constructor faults inside newlib/fsdev with a data abort (measured on
     * v6.20..v6.24: RexSwitchCrashHooksInit+0x168, the bl mkdir at 0xd9404),
     * and every log write fails silently, so only the Atmosphere report came
     * out.
     */
    rc = smInitialize();
    if (R_FAILED(rc)) {
        return rc;
    }
    rc = appletInitialize();
    if (R_FAILED(rc)) {
        return rc;
    }
    rc = fsInitialize();
    if (R_FAILED(rc)) {
        return rc;
    }
    rc = fsdevMountSdmc();
    if (R_FAILED(rc)) {
        return rc;
    }
    if (&__nx_win_init) __nx_win_init();
    return rc;
}

/*
 * The exception handler stack is no longer declared here.
 *
 * libnx has a single one for the whole process, and with twenty-odd threads
 * faulting at the same time they overwrote each other. There are now eight
 * stack and dump sets in exception_handler_switch.cpp, which hands them out.
 */

}  // extern "C"
