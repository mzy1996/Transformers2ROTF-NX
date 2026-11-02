#==========================================================
# rexglue_switch.cmake - Nintendo Switch (devkitA64 + libnx)
#
#   rexglue_switch_configure_target(<target> [GPU_PLUGINS ...])
#       what rexglue_configure_target does on Switch
#   rexglue_switch_add_nro(<target> [NAME ..] [AUTHOR ..] [VERSION ..] [ICON ..])
#       packages <target>.nro next to the executable
#
# Requires the switch-devkitA64.cmake toolchain (DEVKITPRO, compiler, specs) and
# REXGLUE_SWITCH_NVK_SDK pointing at the Mesa Horizon SDK. With
# REXGLUE_SWITCH_NO_NVK=ON (the default of this tree) that SDK is not needed:
# the title draws with its own renderer and src/ui/switch_no_vulkan.c takes over
# the Vulkan symbols the SDK otherwise resolves from libvulkan.a.
#==========================================================
include_guard(GLOBAL)

set(REXGLUE_SWITCH_NVK_SDK "" CACHE PATH
    "portlibs/switch directory of the Mesa Horizon SDK (static NVK driver)")
option(REXGLUE_SWITCH_NVK_BUILD_PENDING
    "Allow object compilation while the external NVK archive is being built (link still requires it)" OFF)
# T2ROTF-deko3d (2026-10-24): this title draws with its own reNut/deko3d renderer
# (renut_engine, application side), so Mesa's NVK driver and the SDK's Vulkan
# presenter are not wanted in the NRO at all. Same switch as the mgr-nx-deko /
# banjo-nx forks; REXGLUE_SWITCH_NO_NVK also swaps the startup driver stubs and
# keeps libvulkan.a (the whole driver) out of the link.
option(REXGLUE_SWITCH_NO_NVK "Switch: use the title's deko3d renderer without a Vulkan driver" ON)
if(NOT REXGLUE_SWITCH_NO_NVK AND NOT EXISTS "${REXGLUE_SWITCH_NVK_SDK}/lib/libvulkan.a")
    if(REXGLUE_SWITCH_NVK_BUILD_PENDING)
        message(WARNING "NVK archive is still being built: object compilation is allowed, final linking requires ${REXGLUE_SWITCH_NVK_SDK}/lib/libvulkan.a")
    else()
        message(FATAL_ERROR
            "REXGLUE_SWITCH_NVK_SDK must point at <mesa-...-horizon-sdk>/opt/devkitpro/portlibs/switch "
            "(no lib/libvulkan.a under '${REXGLUE_SWITCH_NVK_SDK}')")
    endif()
endif()

# Static NVK. Not find_package(Vulkan): the Mesa SDK's config requires libelf,
# which devkitPro does not package. switch_elf_stubs.c satisfies the linker
# instead; the NVIDIA CUBIN parser that uses libelf never runs on Switch.
# Listed twice for archive rescanning, as the Mesa config does without
# LINK_GROUP support.
#
# v6.28-RELROFIX: devkitPro's libexpat.a/libzstd.a are built without -PIC,
# their .rodata jump tables carry absolute pointers, so a PIE linking them
# creates RELATIVE relocations inside the read-only segment:
#   '-z text'   -> "read-only segment has dynamic relocations"
#   '-z notext' -> builds, but __nx_dynamic writes the RO page at NRO startup
#                  and the process Data-Aborts with no NRO log at all.
# scripts/fix_portlibs.sh regenerates both archives in the SDK's fixed-libs
# directory with .rodata renamed to .data.rel.ro, which the linker places in
# the RW data segment; link those first if present. zstd is still linked with
# NO_NVK (expat only served the driver), so the RELROFIX copy matters there too.
if(NOT TARGET rex::nvk)
    # fixed-libs lives next to the rexglue SDK in the nfsmw-nx tree. Resolve it
    # relative to this file: sdk/cmake/rexglue_switch.cmake -> ../../fixed-libs.
    get_filename_component(_rexglue_sdk_root "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
    set(_fixed_libs_dir "${_rexglue_sdk_root}/fixed-libs")
    set(_nvk_expat "expat")
    set(_nvk_zstd "zstd")
    if(EXISTS "${_fixed_libs_dir}/libexpat.a" AND EXISTS "${_fixed_libs_dir}/libzstd.a")
        set(_nvk_expat "${_fixed_libs_dir}/libexpat.a")
        set(_nvk_zstd "${_fixed_libs_dir}/libzstd.a")
    endif()
    add_library(rexglue_switch_nvk INTERFACE)
    add_library(rex::nvk ALIAS rexglue_switch_nvk)
    target_link_directories(rexglue_switch_nvk INTERFACE
        "${REXGLUE_SWITCH_NVK_SDK}/lib"
        "${DEVKITPRO}/portlibs/switch/lib"
        "${DEVKITPRO}/libnx/lib")
    if(REXGLUE_SWITCH_NO_NVK)
        # No Vulkan driver in this product. zstd stays (the runtime uses it),
        # from fixed-libs when the RELROFIX copies are present.
        target_link_libraries(rexglue_switch_nvk INTERFACE
            ${_nvk_zstd} z nx stdc++ m)
    else()
        target_link_libraries(rexglue_switch_nvk INTERFACE
            vulkan ${_nvk_expat} ${_nvk_zstd} z nx
            vulkan ${_nvk_expat} ${_nvk_zstd} z nx
            stdc++ m)
    endif()
    target_link_options(rexglue_switch_nvk INTERFACE -pthread)
endif()

# Startup objects every title needs, built here, in the SDK's scope, where C is
# enabled: a title project may declare CXX only, and CMake cannot compile a C
# source added to a target of that directory. Linked as an OBJECT library, the
# objects reach the executable directly, never through an archive:
#   runtime_switch.cpp  libnx startup constants (applet type, heap, exception
#                       stack); they override weak libnx symbols
#   switch_elf_stubs.c  libelf stubs for Mesa's NVK
#   switch_no_vulkan.c  the Vulkan entry points (NO_NVK build): they resolve to
#                       NULL, so no libvulkan.a is linked, and it also brings
#                       posix_memalign, which the static driver used to define
#   switch_crash_hooks.c  abort()/exit() reports and stderr to the SD; abort
#                       must be defined before libc.a is searched
if(NOT TARGET rexglue_switch_startup)
    if(REXGLUE_SWITCH_NO_NVK)
        set(_rexglue_switch_driver_stubs "${CMAKE_CURRENT_LIST_DIR}/../src/ui/switch_no_vulkan.c")
    else()
        set(_rexglue_switch_driver_stubs "${CMAKE_CURRENT_LIST_DIR}/../src/ui/switch_elf_stubs.c")
    endif()
    add_library(rexglue_switch_startup OBJECT
        "${CMAKE_CURRENT_LIST_DIR}/../src/ui/runtime_switch.cpp"
        "${_rexglue_switch_driver_stubs}"
        "${CMAKE_CURRENT_LIST_DIR}/../src/ui/switch_crash_hooks.c"
        "${CMAKE_CURRENT_LIST_DIR}/../src/ui/switch_perf.cpp"
        # FPS and resolution for the console overlays (SaltyNX) and Reverse-NX reading.
        "${CMAKE_CURRENT_LIST_DIR}/../src/ui/switch_saltynx.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../src/ui/switch_sysclk.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/../src/ui/switch_apm.cpp")
    # switch_perf.cpp registers every thread (the profiler needs their handles)
    # by wrapping libnx's threadCreate/threadClose, which pthread goes through.
    # It also counts the libnx calls that cost IPC or walk memory (fences,
    # submissions, NvMap, GPU addresses and mappings, armDCacheClean, the window
    # queue and svcSleepThread) and the time the calling threads spend inside.
    target_link_options(rexglue_switch_startup INTERFACE
        "LINKER:--wrap=threadCreate,--wrap=threadClose"
        "LINKER:--wrap=nvFenceWait,--wrap=nvGpuChannelKickoff,--wrap=nvMapCreate"
        "LINKER:--wrap=nvAddressSpaceAllocFixed,--wrap=nvioctlNvhostAsGpu_MapBufferEx"
        "LINKER:--wrap=armDCacheClean,--wrap=nwindowQueueBuffer,--wrap=bqDequeueBuffer"
        "LINKER:--wrap=svcSleepThread")
endif()

function(rexglue_switch_configure_target target_name)
    cmake_parse_arguments(ARG "" "" "GPU_PLUGINS" ${ARGN})

    target_sources(${target_name} PRIVATE
        ${REXGLUE_SHARE_DIR}/windowed_app_main_switch.cpp
        ${REXGLUE_SHARE_DIR}/rex_app.cpp)
    target_link_libraries(${target_name} PRIVATE rexglue_switch_startup)

    target_compile_definitions(${target_name} PRIVATE
        REXGLUE_BUILD_CONFIG="$<CONFIG>")
    # The SDK headers the title includes use C++23 (std::byteswap in rex/types.h).
    target_compile_features(${target_name} PRIVATE cxx_std_23)

    # Every object of the runtime and of the GPU plugin goes into the title, as
    # the object files of the SDL build would. From an archive the linker only
    # pulls members that resolve an undefined symbol, and several do nothing
    # but override a weak libnx symbol (__libnx_exception_handler, which the
    # guest memory needs), register themselves from a static constructor
    # (cvars, kernel exports) or serve the plugin entry point.
    set(_whole "$<TARGET_FILE:rexruntime>")
    set(_libs rexruntime)
    foreach(_plugin IN LISTS ARG_GPU_PLUGINS)
        if(NOT TARGET rexgpu-${_plugin})
            message(FATAL_ERROR
                "rexglue_switch_configure_target: unknown GPU plugin '${_plugin}' "
                "(no target rexgpu-${_plugin})")
        endif()
        list(APPEND _whole "$<TARGET_FILE:rexgpu-${_plugin}>")
        list(APPEND _libs rexgpu-${_plugin})
    endforeach()
# Emit one -Wl option per archive instead of a single comma-joined list.
    # MSYS2 (which the devkitA64 toolchain runs under) only converts the first
    # path of a comma-joined -Wl option and leaves later ones untouched, so a
    # path with spaces (e.g. "J:/XBOX360 port/...") would reach ld.exe as an
    # unconverted /j/... path and fail with "cannot find".
    foreach(_w IN LISTS _whole)
        target_link_options(${target_name} PRIVATE
            "LINKER:--whole-archive,${_w}")
    endforeach()
    target_link_options(${target_name} PRIVATE "LINKER:--no-whole-archive")
    # devkitA64/gold: .data.rel.ro (vtables, etc.) lands in a RO PT_LOAD;
    # large C++ PIEs trigger "read-only segment has dynamic relocations".
    # -z notext is the standard, safe workaround for Switch NROs.
    # REVERTED v6.27-INITFIX: -z notext caused __nx_dynamic to crash on NRO
    # startup (Data Abort during early relocation) because it leaves R/O
    # segments with live relocs that the runtime resolver writes to before
    # C++ global constructors / main. Instead we fix the source symbol that
    # produced .data.rel.local (std::numeric_limits::max -> ~size_t(0)).
    # target_link_options(${target_name} PRIVATE "LINKER:-z,notext")
    set_property(TARGET ${target_name} APPEND PROPERTY LINK_DEPENDS ${_whole})
    # Still linked normally, for the build order and their own dependencies
    # (glslang, FFmpeg, ...), which stay regular archives.
    target_link_libraries(${target_name} PRIVATE ${_libs} rex::nvk)

    # rex_app.cpp includes imgui.h. rexruntime links imgui privately, so its
    # include path does not reach the title; take the path alone, since imgui's
    # objects are already inside rexruntime.
    target_include_directories(${target_name} PRIVATE
        $<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>)
endfunction()

function(rexglue_switch_add_nro target_name)
    cmake_parse_arguments(ARG "" "NAME;AUTHOR;VERSION;ICON" "" ${ARGN})
    if(NOT ARG_NAME)
        set(ARG_NAME "${target_name}")
    endif()
    if(NOT ARG_AUTHOR)
        set(ARG_AUTHOR "ReXGlue")
    endif()
    if(NOT ARG_VERSION)
        set(ARG_VERSION "${PROJECT_VERSION}")
        if(NOT ARG_VERSION)
            set(ARG_VERSION "1.0.0")
        endif()
    endif()
    if(NOT ARG_ICON)
        set(ARG_ICON "${DEVKITPRO}/libnx/default_icon.jpg")
    endif()

    find_program(REXGLUE_NACPTOOL nacptool HINTS "${DEVKITPRO}/tools/bin" NO_DEFAULT_PATH REQUIRED)
    find_program(REXGLUE_ELF2NRO elf2nro HINTS "${DEVKITPRO}/tools/bin" NO_DEFAULT_PATH REQUIRED)

    set(_dir "$<TARGET_FILE_DIR:${target_name}>")
    add_custom_command(TARGET ${target_name} POST_BUILD
        COMMAND "${REXGLUE_NACPTOOL}" --create "${ARG_NAME}" "${ARG_AUTHOR}" "${ARG_VERSION}"
            "${_dir}/${target_name}.nacp"
        COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:${target_name}>"
            "${_dir}/${target_name}.stripped.elf"
        COMMAND "${CMAKE_STRIP}" --strip-all "${_dir}/${target_name}.stripped.elf"
        COMMAND "${REXGLUE_ELF2NRO}" "${_dir}/${target_name}.stripped.elf"
            "${_dir}/${target_name}.nro" "--nacp=${_dir}/${target_name}.nacp" "--icon=${ARG_ICON}"
        COMMENT "Packaging ${target_name}.nro"
        VERBATIM)
endfunction()