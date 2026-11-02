# T2ROTF → deko3d（Renut 路线）落地清单

- 日期：2026-10-24
- 依据：`repos/mgr-nx-deko`（HEAD `d4bc16e72f772ffa4f0cf383a9a2fcced2536954`，2026-10-23）实测
- 前情：`T2ROTF_deko3d_路线2-3难度评估.md`（路线 2 不可行、路线 3 可行的结论不变），本文档把路线 3 拆成可执行的落地动作
- 触发原因：Vulkan/NVK 主线冻结。实机证据为 ring 门 `sub_822D4488` 轮询的字与宿主发布地址不一致（宿主写 `[dev+10896]+0x3C`、guest 读 `[dev+10896]+0`），读指针自 ring 初始化起从未前进（58 s 内 `*P` 冻结在 `0xB`、门等待 5.2 亿次 `pasa=0`），呈现计数恒 0、全程黑屏

## 0. 结论

"全面转向 deko3d" 比预想的可控：**不需要动 Xenos 重编译产物，也不需要重做 TU/DLC/VFS/FFmpeg 这些既有修复**。真正要做的只有两类事：

1. **SDK 侧补一条"无 Vulkan"路径**：1 个新文件（`src/ui/switch_no_vulkan.c`）+ `cmake/rexglue_switch.cmake` 的 3~4 处改动（`REXGLUE_SWITCH_NO_NVK` 选项）。NVK 静态库从此不进 NRO。
2. **应用侧接入 Renut 渲染引擎**：`app/src/renut_engine`（111 文件）+ 4 个 vendored 依赖（plume / reblue-XenosRecomp / uam / imgui，合计约 88 MB）+ 一套主机侧着色器工具，再针对 T2 自己的 Direct3D 用法做重定向。

关键判据：**SDK 分叉补丁（202 文件）里 deko3d / renut / plume / uam 命中数为 0**，它是 Horizon（Switch）平台层与 codegen/runtime 调优，与渲染后端无关；Renut 是纯应用侧的东西，它以 OBJECT 库的形式替换 codegen 生成的弱符号 `sub_X`，直接劫持 guest 的 Direct3D 调用，经 plume 在 deko3d 上绘制，**不模拟 Xenos、不用 PM4 ring、不用 EDRAM、不用 Vulkan**。Vulkan 路上那个死结（宿主发布地址 ≠ guest 轮询地址）在这条路线上不再经过。

## 1. 架构事实（决定后续所有取舍）

| 事实 | 证据位置 |
| --- | --- |
| Renut = `github.com/masterspike52/reNut` 的 Direct3D 级渲染器，由 banjo-nx 携带在 `src/renut_engine/gpu`，经 sa2-nx 到 MGR，每一作重新对接自己的 Direct3D | `app/cmake/mgr_renut_gpu.cmake:1-6` |
| 以 **OBJECT 库**（不是 static）链接：它的钩子替换生成代码里的弱 `sub_X`，静态库成员只在有未定义符号时才被拉入，钩子会被静默丢弃 | `app/cmake/mgr_renut_gpu.cmake:8-10` |
| 引导期由 cvar 选择：`sa2_renderer` ∈ {`xenos`, `renut`}（MGR 默认 `renut`，`Lifecycle::kInitOnly`）；`xenos` = 模拟 Xenos GPU（PM4 ring + EDRAM） | `app/src/renut_engine/gpu/banjo_renut_select.cpp:15-21` |
| `REXGLUE_SWITCH_NO_NVK` 构建里 `LatchActive()` 恒真（没有别的渲染器可用）；若 `deko3d_glsl.bin` 不在 NRO 旁会报 `MissingGameShaders`，游戏的贴图不会画出来 | `app/src/renut_engine/gpu/banjo_renut_select.cpp:31-45` |
| SDK 侧唯一开关：`REXGLUE_SWITCH_NO_NVK`（fork 里默认 ON） | `sdk/cmake/rexglue_switch.cmake:18` |
| NO_NVK 的实际作用：NVK 静态库不进 NRO（`rex::nvk` 只链 `zstd z nx stdc++ m`）；startup 对象用 `switch_no_vulkan.c` 顶替 `switch_elf_stubs.c` | `sdk/cmake/rexglue_switch.cmake:46,67` |
| `switch_no_vulkan.c` 把 4 个 Vulkan 入口做成 stub（返回 NULL），使 Vulkan 实例创建干净失败；顺带补了 `posix_memalign`（FFmpeg 的 `av_malloc` 需要，原先由静态驱动提供）。注释明说：这样的构建用"不用 Vulkan 的渲染器（如 deko3d）"绘制，SDK 的 Vulkan presenter 与 GPU plugin 一并不可用 | `sdk/src/ui/switch_no_vulkan.c:1-35` |
| deko3d 后端在 plume 里（banjo 的改动），Switch 上 `PLUME_DEKO3D=ON`、`PLUME_SWITCH_NO_NVK=REXGLUE_SWITCH_NO_NVK`、`PLUME_SWITCH_UAM_DIR=thirdparty/uam` | `app/cmake/mgr_renut_gpu.cmake:44-66` |
| 主机侧必须预置 PC 版 `spirv-cross.exe` 与 `uam.exe`（由 `tools/deko3d_build_tools.sh` 生成到 `out/deko3d_tools/`），缺失直接 FATAL_ERROR | `app/cmake/mgr_renut_gpu.cmake:58-68` |
| 游戏着色器有两种形态：开发/测试 = 全部译好的着色器编进 NRO；release = 不内置，`deko3d_glsl.bin` 放在 NRO 旁（安装器用玩家自己的游戏文件生成） | `app/cmake/mgr_renut_gpu.cmake:69-84` |
| Switch 构建开关：`REXGLUE_SWITCH_NO_NVK=ON` + `MGR_RENUT_RENDERER=ON` + `MGR_NATIVE_RENDERER=OFF` | `tools/win/build_switch.ps1:36` |

Renut 引擎目录结构（111 文件）：

| 目录 | 文件数 | 内容 |
| --- | --- | --- |
| `app/src/renut_engine/gpu` | 18 | 选择/调优/deko3d 接入/capture + `gpu.cmake` |
| `app/src/renut_engine/gpu/backend` | 16 | backend、present（交换链+扫描出+overlay+提交）、render_thread、video（影片 YUV）、display_sync、gpu_profile |
| `app/src/renut_engine/gpu/render` | 21 | draw、format、textures、状态映射 |
| `app/src/renut_engine/gpu/guest` | 12 | 对 guest 函数的钩子 |
| `app/src/renut_engine/gpu/overlay` | 6 | ImGui overlay 与应用接线 |
| `app/src/renut_engine/gpu/d3d` | 4 | Direct3D 级接口 |
| `app/src/renut_engine/gpu/shaders` | 1 | 着色器侧接线 |
| `app/src/renut_engine/perf` | 2 | 性能采样 |
| `app/src/renut_engine/sa2_compat` | 8 | 逐作兼容层（cpu / me / profiler / rex）——**T2 的对接主要落在这里** |

## 2. 三个补丁的作用与基线核对

| 补丁 | 上游基线 | 内容 | 基线核对 | 对本工程的关系 |
| --- | --- | --- | --- | --- |
| `rexglue-sdk-v0.10.0.patch`（1,405,312 B，202 文件，+25,814/−889） | rexglue/rexglue-sdk v0.10.0，commit `c94f5ebdcb3c9d1a460ca48e04f9758448f8d518` | 整个 SDK 分叉：Horizon（Switch）层、codegen 选项、运行时主机调优、SDK 自带的 FFmpeg 构建 | 与本工程 `repos/rexglue-sdk` 的 HEAD **完全一致** | **实测其中 deko3d / renut / plume / uam 命中 0**，不是 deko3d 补丁；mgr 仓库内 `sdk/` 就是应用后的成品树（1045 文件）→ **无需重放补丁，可直接取用** |
| `ffmpeg-0604b46.patch`（8,976 B，5 文件） | wmarti/FFmpeg `0604b464c7cb4ebc94940cf1f324a3b26b87717c`（rexglue v0.10.0 的 FFmpeg submodule） | `config.h`（Switch 配置）+ `libavcodec/fft_template.c`、`mdct_template.c`（表缓存）+ 新增 `config_switch_aarch64.h`、`config_masseffect_wmv3.h` | 未逐字节核对 | 需要与我们已集成的 FFmpeg（`repos/FFmpeg` + SDK 的 `thirdparty/FFmpeg`）比对后再决定是否吸收；我们的解码已在实机跑通，不轻易替换 |
| `xenosrecomp-990d03b.patch`（127,735 B，4 文件） | hedge-dev/XenosRecomp `990d03b28a27b50277ee5d8d942e1c5f873869d1` | `shader_recompiler.cpp/.h`、`shader_common.h`、新增 `pch_min.h` | 未逐字节核对 | Renut 路线仍要翻译 guest 着色器，需评估这 4 个改动是否必需（其 `shaders/README.md` 说明每处改动） |

SDK 分叉补丁的目录分布与"本地是否已有等价物"（本地 = `repos/nfsmw-nx/sdk`，即本工程实际参与构建的 SDK；同时对照 `repos/rexglue-sdk`）：

| 目录 | 文件数 | +行 | −行 | 本地两棵树都存在 | 备注 |
| --- | --- | --- | --- | --- | --- |
| `include/rex` | 55 | 2206 | 31 | 大部分 | 头文件接口 |
| `src/core` | 23 | 5738 | 172 | 是 | 内存/线程/时钟等平台实现 |
| `src/ui` | 21 | 5175 | 54 | 是（19 个内容不同，1 个仅 fork 有 = `switch_no_vulkan.c`） | 窗口/呈现/overlay |
| `src/codegen` | 19 | 2725 | 314 | 是 | 代码生成器选项 |
| `src/system` | 14 | 669 | 40 | 是（10 个内容不同 + `global_lock_spin.h`） | 内核态/运行时 |
| `src/filesystem` | 13 | 3394 | 11 | 多数 | 其中 8 个本地两棵树都没有 |
| `src/graphics` | 12 | 378 | 33 | 是 | 无 deko3d 相关内容 |
| `tests` | 10 | 1562 | 0 | 否（9 个本地没有） | 该 fork 的单元测试 |
| `src/kernel` / `src/audio` / `src/input` | 9 / 6 / 4 | 952 / 1359 / 1151 | 40 / 99 / 27 | 是 | 平台实现 |
| `resources/templates`、`cmake`、根 `CMakeLists.txt` | 6 / 3 / 1 | 83 / 202 / 28 | 6 / 0 / 6 | 是 | 模板与构建脚手架 |

实测统计：137 个补丁文件在我们的两棵 SDK 树中都存在；39 个属 fork 新增但 `nfsmw-nx/sdk` 已有同名文件；25 个本地两棵树都没有；1 个只在 `rexglue-sdk`。**本地全无的 25 个文件清单**：`include/rex/{filesystem/devices/pack_device.h, ppc/diet_ops.h, sys_counters.h, system/static_modules.h}`、`resources/templates/codegen/register_shard_cpp.inja`、`src/codegen/{args_in_registers.cpp, args_in_registers.h}`、`src/filesystem/devices/{host_path_index.cpp, host_path_index.h, host_path_listed_switch.cpp, pack_device.cpp, pack_storage_switch.cpp, startup_trace.cpp, startup_trace.h}`、`src/system/global_lock_spin.h`、`tests/*`（9 个）。

## 3. 落地清单（按动作分组）

### 组 A：直接取用

| 编号 | 来源（mgr-nx-deko） | 去处（本工程） | 动作 |
| --- | --- | --- | --- |
| A1 | `sdk/src/ui/switch_no_vulkan.c` | `repos/nfsmw-nx/sdk/src/ui/` | 新增（NO_NVK 的 Vulkan 入口桩 + `posix_memalign`） |
| A2 | `sdk/cmake/rexglue_switch.cmake` 的 NO_NVK 片段（option 声明、`REXGLUE_SWITCH_NVK_BUILD_PENDING`、`rex::nvk` 目标里 NO_NVK 分支、startup 对象在 `switch_no_vulkan.c` / `switch_elf_stubs.c` 之间的选择） | `repos/nfsmw-nx/sdk/cmake/rexglue_switch.cmake` | 合并：**保留我们已有的 RELROFIX 段**（`fixed-libs/libexpat.a`、`libzstd.a`，v6.28 的 `.rodata`→`.data.rel.ro` 修复；这是 T2 本地的关键修复，fork 没有） |
| A3 | `app/src/renut_engine/**` | `recomp/src/renut_engine/**`（或 `recomp/` 下自定路径，需同步 include 前缀） | 新增 111 文件 |
| A4 | `app/thirdparty/{plume, reblue-XenosRecomp, uam, imgui}`（46M / 30M / 8.9M / 3M） | 同位置 `recomp/thirdparty/` | 新增（仓库内 vendored，非 submodule；plume 含 banjo 的 deko3d 后端） |
| A5 | `app/cmake/{mgr_renut_gpu.cmake, collect_captured_shaders.cmake, copy_if_missing.cmake}` + 根 `CMakeLists.txt` 的选项校验 + `SA2_SWITCH_NO_NVK=1` 编译定义（`mgr_renut_gpu.cmake:144-149`） | `recomp/cmake/` + `recomp/CMakeLists.txt` | 新增并改名（去掉 `MGR_` 前缀），Switch 分支改为 `rexglue_setup_target(T2ROTF)`（当前是 `GPU_PLUGINS xenos`） |
| A6 | `tools/deko3d_build_tools.sh`、`tools/deko3d_{shader_package,glsl,glsl_opt,host_shader,variant_cache,shaders,shader_stats}.py`、`tools/renut_*`、`tools/win/{build_deko_shader_tools.ps1, build_uam.ps1, build_switch.ps1}`、`tools/d3d/*` | 工程 `tools/` | 新增（主机侧工具链与着色器流水线） |

### 组 B：需三方合并（先不动，按需引入）

- **B1 SDK 其余差异**：`src/ui` 19 个、`src/core` 21 个、`src/system` 10 个、`cmake` 3 个、根 `CMakeLists.txt`。判据：这些差异是"两个移植各自的运行时调优"（例如我们的 RELROFIX、ring/门/Xenos 调优、`switch_perf`/`saltynx` 探针），**不是 deko3d 必需**。策略：只在 NO_NVK 构建实际报错或缺符号时按需带入，一次带一个、带完立刻构建验证。
- **B2 必须原样保留的 T2 本地修复**（任何合并都不得覆盖）：
  - VFS overlay 跨层目录合并回退（`src/filesystem/devices/overlay_host_path_device.cpp` 及配套；fork 侧对应的是 `host_path_index.*`、`host_path_listed_switch.cpp`，属另一套实现，不要直接覆盖）
  - RELROFIX 的 `fixed-libs`（`libexpat.a`、`libzstd.a`）
  - codegen 参数：`--update_data_root`（TU 补丁）、`--max_file_size_bytes`（1.36 MB 的 `0x82B7E3A0` 函数）
  - TU 升级档与 DLC 布局（`J:\XBOX360 port\TFROTF TU\`、`J:\XBOX360 port\Transformers 2 - Maps and Characters Pack`）
  - libnx 初始化顺序：`fsdevMountSdmc()` 之后立即 `__nx_win_init()`（黑屏 `no default NWindow` 的既有修复）
  - FFmpeg 解码本体与 `recomp/src/t2rotf_video_nativo.cpp` 的全部钩子（WMV3/WVC1 原生解码 + 真实播放泵 `sub_827CE598` 的驱动点）

### 组 C：必须新建（T2 专属，工作量主体）

| 编号 | 内容 | 落点 | 依据 |
| --- | --- | --- | --- |
| C1 | 把 Renut 的 Direct3D 劫持对接 T2 的 D3D 用法（格式、状态块、surface、shader 容器、显示平面扫描出） | `recomp/src/renut_engine/sa2_compat/**` 改名后的 T2 兼容层 + `gpu/d3d/**` | Renut 是从 banjo-nx → sa2-nx → MGR 逐作"retargeted to <game>'s Direct3D"的，第 1 节的证据与 `gpu.cmake` 注释 |
| C2 | T2 的着色器容器导出与编译：`default.xex` + `Shader/xbox360/obj` → `shader_cache_switch.cpp` + `deko3d_glsl_switch.cpp`（开发）/ `deko3d_glsl.bin`（release，NRO 旁） | 工程 `tools/` + `out/renut_switch/` | `mgr_renut_gpu.cmake:69-84`、`tools/deko3d_shader_package.py`、`tools/renut_switch_shader_cache.py` |
| C3 | 影片上屏：Renut 自带 movie YUV 动态纹理路径，把 FFmpeg 解出的帧接进去 | `gpu/backend/video.cpp` 对应接线 + 我们的 `t2rotf_video_nativo.cpp` | `present.cpp` 头注释（scan-out + overlay + submit）与 `video.cpp` |
| C4 | 引导开关：NO_NVK 构建下 Renut 是唯一渲染器，无需 cvar 选择；如需保留 PC 参考则用 T2 自己的键替代 `sa2_renderer` | `banjo_renut_select.*` | `banjo_renut_select.cpp:31-45` |

## 4. 兼容边界：保留 / 失效

| 保留 | 失效应停用 |
| --- | --- |
| Xenos 重编译产物与 codegen 参数 | `MESA_SHADER_CACHE_DISABLE=1`（NVK 着色器缓存，NVK 不再链入） |
| VFS overlay 修复、TU、DLC、libnx 初始化顺序 | `vulkan_async_skip_incomplete_frames=false` 那处呈现修复（Vulkan presenter 不再使用） |
| FFmpeg 解码本体 + 影片泵钩子（改用 Renut 的影片纹理出口） | `[T2CP]` ring 诊断与 `command_processor` 的读指针镜像改动（Vulkan command processor 不再使用） |
| 诊断探针（`switch_perf`、SaltyNX、`T2ROTF_001/002` 日志、`rex_perfil`） | `src/graphics/vulkan/*` 的本地补丁、SDK Vulkan presenter 及经它上屏的影片路径 |
| `--update_data_root` / `--max_file_size_bytes` | 依赖 NVK 静态驱动的任何假设 |

## 5. 阶段计划与验收

| 阶段 | 内容 | 验收条件 |
| --- | --- | --- |
| 1 SDK/构建不再依赖 NVK | 组 A1、A2；`recomp/CMakeLists.txt` 的 Switch 分支与 NO_NVK 选项 | `ninja -j4 T2ROTF` 退出码 0；`strings T2ROTF.nro \| grep -m1 "Mesa 26"` 无输出；NRO 体积较 Vulkan 版下降（NVK 静态库不再链入） |
| 2 引擎链接 | 组 A3、A4、A5、A6（含主机侧 `spirv-cross.exe`、`uam.exe`） | 构建退出码 0；产物中出现 plume / deko3d / Renut 符号与字符串 |
| 3 画面 | 组 C1–C4 + 实机跑机回传日志 | 实机出现 Renut/deko3d 日志与至少一次 present；黑屏解除；三连 logo（`luxo`→`atvi`→`hasbro`）按序推进；影片上屏 |
| 4 性能 | 不超频实测与优化 | 给出不超频帧率结论与依据，或给出明确的优化项清单 |

## 6. 风险与不可保证项（诚实列出）

- **R1 不超频 60 帧没有先例保证**。参考工程自报在**超频**（CPU 1785 / GPU 768）下菜单 55–60 FPS、游戏内 40–52 FPS，默认频率未测。转向 Renut 解决的是"NVK 驱动把整个呈现链卡死"，不等于帧率达标。60 帧要在阶段 4 实测，若不足需要额外优化（分辨率、后处理、异步、线程划分等），届时如实报告。
- **R2 工作量主体在阶段 3**。Renut 是 Direct3D 级重实现，T2 与 MGR/SA2 的 D3D 用法差异需要逐项对接；`sa2_compat` 这一层是从别的作品带过来的，改名与重定向不可避免。
- **R3 着色器**：T2 的着色器容器与 GLSL 包必须重新生成并验证；release 形态还要求 `deko3d_glsl.bin` 随 NRO 分发（由玩家游戏文件生成），开发形态则把译好的着色器编进 NRO。
- **R4 影片上屏属预期、须实机验证**。此前 FFmpeg 已能解出帧并写入 guest 显示平面，但 Vulkan presenter 从未消费（呈现计数恒 0）；Renut 路线预期由游戏自身的 D3D 纹理上传/扫描出完成上屏，这一点只能靠实机日志确认。
- **R5 依赖来源**：plume 的 deko3d 后端来自 banjo 的改动，本地只以 mgr 仓库 vendored 副本（46 MB）形式存在；本地其他移植仓库（`prototype2-nx`、`torchlight-nx`、`project8-sw`、`sa2-nx`、`UnleashedRecomp-NX*`）实测 deko3d/renut 命中为 0，不能替代。

## 7. 关键路径索引

| mgr-nx-deko | 本工程目标 | 说明 |
| --- | --- | --- |
| `sdk/`（1045 文件，fork 成品树） | 参考 / 按需取文件 | NO_NVK 只用其中 2 个文件（A1、A2） |
| `sdk/src/ui/switch_no_vulkan.c` | `repos/nfsmw-nx/sdk/src/ui/switch_no_vulkan.c` | A1 |
| `sdk/cmake/rexglue_switch.cmake` | `repos/nfsmw-nx/sdk/cmake/rexglue_switch.cmake` | A2，保留 RELROFIX |
| `app/src/renut_engine/**`（111 文件） | `recomp/src/renut_engine/**` | A3 |
| `app/thirdparty/{plume,reblue-XenosRecomp,uam,imgui}` | `recomp/thirdparty/` | A4 |
| `app/cmake/mgr_renut_gpu.cmake` | `recomp/cmake/t2rotf_renut_gpu.cmake` | A5（改名、去 `MGR_`/`SA2_` 前缀） |
| `app/CMakeLists.txt:22-29`（两个 FATAL_ERROR） | `recomp/CMakeLists.txt` | Switch 上强制 `RENUT_RENDERER=ON` + `REXGLUE_SWITCH_NO_NVK=ON` |
| `tools/deko3d_build_tools.sh`、`tools/win/build_uam.ps1`、`build_deko_shader_tools.ps1` | 工程 `tools/` | 主机侧 `spirv-cross.exe`、`uam.exe` |
| `tools/deko3d_shader_package.py`、`tools/renut_switch_shader_cache.py`、`tools/renut_release_shaders.py` | 工程 `tools/` | 着色器包与 shader cache |
| `tools/win/build_switch.ps1:36` | 工程构建脚本 | `-DREXGLUE_SWITCH_NO_NVK=ON` |

## 8. 阶段 1 的第一批动作（可直接执行）

1. 复制 `repos/mgr-nx-deko/sdk/src/ui/switch_no_vulkan.c` → `repos/nfsmw-nx/sdk/src/ui/`。
2. 在 `repos/nfsmw-nx/sdk/cmake/rexglue_switch.cmake` 合并 NO_NVK 片段：加 `option(REXGLUE_SWITCH_NO_NVK ... ON)` 与 `REXGLUE_SWITCH_NVK_BUILD_PENDING`；`rex::nvk` 目标在 NO_NVK 下只链 `zstd z nx stdc++ m`；startup 对象在 NO_NVK 下用 `switch_no_vulkan.c`。**保留原有 RELROFIX 段与 `fixed-libs` 逻辑**。
3. 在 `recomp/CMakeLists.txt` 的 Switch 分支去掉 `GPU_PLUGINS xenos`，加 `REXGLUE_SWITCH_NO_NVK=ON`，并受理 `RENUT_RENDERER` 选项（阶段 2 才真正链接引擎）。
4. `ninja -j4 T2ROTF`，核对退出码 0，并取证：`strings recomp/out/sw8/T2ROTF.nro | grep -m1 "Mesa 26"` 应为空、NRO 体积应较 Vulkan 版（72,856,306 字节）下降。