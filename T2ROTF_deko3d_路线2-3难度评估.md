# T2ROTF Switch 移植：路线 2 / 路线 3（deko3d）实现难度评估

评估对象：把图形引擎从当前「Xenos → rexglue Vulkan 后端 → NVK(Mesa)」迁移到 deko3d 的两种设想。
参考实现：<https://github.com/tuancapoo/mgr-nx-deko>（Xbox360《MGR:R》经 ReXGlue 静态重编译的 Switch 原生移植，本地克隆在 `repos/mgr-nx-deko`）。

---

## 0. 结论摘要

| 路线 | 内容 | 可行性 | 改动面 | 工作量（单人全职） | 能否据此保证「不超频 60 帧」 |
|---|---|---|---|---|---|
| **路线 2** | 只换平台层（设备初始化 / swapchain / 命令缓冲提交），保留 Xenos 转译 | **不可行**（无接缝） | 不可定义 | — | — |
| **路线 3** | 完整 deko3d 渲染后端（= 整体采用 mgr-nx-deko 的 Renut 方案） | 可行 | 202 文件 SDK 分叉 + 111 文件 / 33192 行渲染器 + CMake 双选项 | 6–10 周，风险高 | **不能**：参考项目自报该方案在**超频**（CPU 1785 / GPU 768 MHz）下菜单 55–60、游戏内 40–52 FPS，默认频率未测 |

**建议**：先完成路线 1（本轮已落地），用下一次实机日志判定「影片 tick 链是否在推进、帧率瓶颈在等待还是在渲染吞吐」，再决定是否投入路线 3。理由见第 4 节。

---

## 1. 当前图形栈（对照基准）

- 我们 app 实际链接的 SDK 树是 `repos/nfsmw-nx/sdk`（不是 `repos/rexglue-sdk`），`recomp/CMakeLists.txt` 注明 app 骨架源自 nfsmw-nx SDK base。
- 渲染链路：重编译出的 guest PM4 命令 → `src/graphics/command_processor.cpp` 解析 → `src/graphics/vulkan/*`（Vulkan 提交）→ NVK（Mesa on libnx）→ Switch 屏幕。
- `src/graphics/` 模块清单：`command_processor.cpp`、`graphics_system.cpp`、`xenos.cpp`、`pipeline/`、`shaders/`、`format/`、`util/`、`lib`、`vulkan/`、`d3d12/`（PC 用）。
- **不存在任何平台抽象层**：整条提交路径用 Vulkan 对象（VkDevice / VkSwapchainKHR / VkCommandBuffer / VkPipeline）写成，没有 `GraphicsPlatform` 之类的可替换接口；唯一的「可整体替换的一块」是 `src/ui/` 里的 presenter/swapchain。
- **deko3d 接缝检查（实测）**：`repos/nfsmw-nx/sdk/{src,include}` 中 `deko3d` / `renut` / `plume` / `uam` 命中数全部为 **0**。

---

## 2. 路线 2「只换平台层，保留 Xenos 转译」——不可行

**为什么不成立**

1. 要让 deko3d 承担命令缓冲提交，而命令内容仍由现有 Vulkan 后端产生，就必须实现一份 **Vulkan → deko3d 的适配层**：描述符集/管线状态/渲染通道的内存模型对齐、命令缓冲录制、fence/信号量映射，再加 **SPIR-V → deko3d 着色器（DKSH/GLSL 后端的着色器编译）** 的翻译。这实际上是在 deko3d 之上写一个「mini Vulkan 驱动」，工作量与路线 3 同量级甚至更大，且没有参考实现。
2. 只替换 presenter/swapchain（`src/ui/` 一侧）不改变命令路径：Vulkan 与 NVK 仍在链路里，**性能不会有任何改善**，只是把窗口呈现换成 deko3d 呈现，还要额外维护两套同步。
3. deko3d 与 Vulkan 的**命令来源不同**：Renut 之所以存在，正是因为 mgr-nx-deko 需要绕开 Vulkan，把 **Direct3D 层**（而不是 Xenos/PM4 层）直接翻译到 deko3d；它并不复用 rexglue 的 Vulkan 后端。
4. 参考项目的构建约束是硬编码的：`app/CMakeLists.txt` 中
   - `option(MGR_RENUT_RENDERER "Build the Direct3D-level reNut renderer (deko3d on Switch)" ${REXGLUE_PLATFORM_SWITCH})`
   - Switch 平台下 `MGR_RENUT_RENDERER=OFF` 直接 `FATAL_ERROR`；`REXGLUE_SWITCH_NO_NVK=OFF` 同样 `FATAL_ERROR`。
   即：**它的 Switch 产物根本没有 NVK 路径**，「保留 NVK + 只换平台层」的形态在参考项目里并不存在。

**结论**：路线 2 无法以「低成本只换一块」实现。若把路线 2 重新解释为「把渲染器换成 Renut」，那就与路线 3 完全重合，直接看下一节。

---

## 3. 路线 3：完整 deko3d 渲染后端（采用 mgr-nx-deko 的 Renut 方案）

### 3.1 改动清单（可核对）

| 模块 | 规模 | 内容 | 风险 |
|---|---|---|---|
| SDK 分叉补丁 `patches/rexglue-sdk-v0.10.0.patch` | **202 文件 / 1,405,312 字节** | 基线 commit `c94f5ebdcb3c9d1a460ca48e04f9758448f8d518`（v0.10.0，**与我们 `repos/rexglue-sdk` HEAD 完全一致**）。按顶层目录：`include/rex` 55、`src/core` 23、`src/ui` 21、`src/codegen` 19、`src/system` 14、`src/filesystem` 13、`src/graphics` 12、`src/kernel` 9、`src/audio` 6、`resources/templates` 6 | 补丁面向 upstream 树，而我们**实际链接**的 `repos/nfsmw-nx/sdk` 已本地分叉（10 个已改文件 + 2 个未跟踪目录，含西语注释的「边执行边发布读指针」补丁与本轮 T6 诊断插桩）→ 需逐文件三方合并 |
| 应用侧渲染器 `app/src/renut_engine` | **111 文件 / 33,192 行** | Direct3D 级渲染器 + plume(deko3d) 后端：`gpu/d3d/hooks/{device,draw,resource,shader,state}.cpp`、`gpu/backend/{video,present}.cpp`、`gpu/banjo_renut_select.cpp` 等 | 必须适配到 T2ROTF 的 app 骨架；T2ROTF 的 D3D 调用序列与 MGR:R 不同，hook 覆盖需逐一验证 |
| 应用侧其它模块 | `app/src/{me,cpu,profiler,sa2_dlc,sa2_language,sa2_capture,sa2_d3d_trace,sa2_pgo,sa2_static_modules}` | 影片引擎、CPU 线程模型、PGO、DLC/语言、D3D tracing | 与 T2ROTF 的影片/TU/DLC 需求重叠但不通用，需重写或裁剪 |
| CMake / 构建选项 | `option(MGR_RENUT_RENDERER ...)` + 两个 `FATAL_ERROR` 守卫 | Switch 产物强制 `MGR_RENUT_RENDERER=ON`、`REXGLUE_SWITCH_NO_NVK=ON` | 与我们现有 `recomp/` 工程选项体系合并 |
| 第三方依赖 | plume（deko3d 上层图形库）、`uam`（deko3d 着色器编译器，本机 devkitPro 已有 `uam.exe`：`/c/devkitPro/tools/bin/uam.exe`）、imgui；另有两个补丁 `patches/ffmpeg-0604b46.patch`、`patches/xenosrecomp-990d03b.patch` | 着色器需用 `uam` 预编译（参考：`tools/win/build_mgr_deko_shaders.ps1`） | 我们的 FFmpeg 集成是自建静态库路径，可能需按 `ffmpeg-0604b46.patch` 重做 |
| 本地工具链 | 已具备：`libnx/include/deko3d.h`、`libdeko3d.a` | — | 无需额外获取 |

### 3.2 兼容边界

**可以保住（与渲染器无关）**
- VFS overlay 跨层合并修复（`repos/nfsmw-nx/sdk` 的 hierarchy fix）与 TU/DLC 的 VFS 布局；
- libnx 初始化顺序、`__nx_win_init()`、arena 4.5GB + 禁用预提交等启动期修复；
- Xenos 重编译产物（`recomp/generated/**` 的 C 分片）与 codegen 流程不动；
- FFmpeg VC-1 解码 HLE 本体（`recomp/src/t2rotf_video_nativo.cpp` 的解码部分）；
- 参考端（xenia）建立的函数存在性基准与调用链结论。

**会失效 / 必须重做**
- `MESA_SHADER_CACHE_DISABLE`（`src/ui/rex_app.cpp`）——不再有 Mesa；
- 本轮「不跳过带占位管线的帧」修复（`vulkan_async_skip_incomplete_frames=false`）——不再有 Vulkan 占位管线；
- 本轮 `[T2CP]` ring 读指针诊断（`src/graphics/command_processor.cpp`）——Renut 模式下 hook 点在 D3D 层，ring/PM4 语义不同；
- `src/graphics/vulkan/*` 上的全部本地补丁；
- 影片上屏路径：现在靠「注入 guest 显示平面 → 由 guest 正常 swap 呈现」，参考项目走的是 Renut 的 XMV 播放器 + movie YUV shader 动态纹理路径（无 EDRAM），需重新设计。

### 3.3 工作量估算（单人全职，含调试）

| 阶段 | 内容 | 估计 |
|---|---|---|
| 1 | 补丁落地 + 与本地分叉三方合并 + SDK/工具链构建打通（先 PC 后 Switch） | 1.5–2.5 周 |
| 2 | `renut_engine` 接入 T2ROTF app 骨架，D3D hook 覆盖到「能出画面」 | 1.5–3 周 |
| 3 | 菜单/影片/TU/DLC 路径逐一调通 | 2–3 周 |
| 4 | 性能调优至目标帧率 | **不确定**（见 3.4） |
| 合计 | | **6–10 周** |

### 3.4 关于「不超频 60 帧」的硬事实

参考项目自报的成绩是 **超频状态**（CPU 1785 MHz / GPU 768 MHz）下：菜单 55–60 FPS、游戏内 40–52 FPS；**默认频率下的帧率未给出**。因此：

- 路线 3 不能作为「保证不超频 60 帧」的依据；
- 它解决的是「在 Switch 上用不依赖 NVK 的渲染后端跑起来」的**可行性**问题，不是**性能达标**问题；
- 帧率是否达标最终取决于 Xenos 绘制调用量、着色器复杂度与 CPU 侧重编译代码的执行效率，与用 Vulkan 还是 deko3d 只有间接关系。

---

## 4. 为什么不建议现在切到路线 3

本轮（T6）实机取证的指向是**卡死式等待**，而不是渲染吞吐不足：

- 早期会话启用 1 ms 栈采样时的 `rex_perfil` 输出显示：宿主 GPU Commands 线程 **100% 时间**停在 `rex::thread::Wait(5 ms)`（ring 无新 kick，CPU 0.2%）；guest D3D 提交线程 **99.7% CPU** 全落在 `sub_822D13F0` / `sub_822D4488` 的 ring 空间等待自旋；Game Master **86%** 时间在 `NtYieldExecution → svcSleepThread`。需要说明：**双线程约 99% 忙这一点后来被证明不是唯一根因**（把受害线程全钉在等锁上，自有线程之和必然超 100%），此处引用仅用于说明「握手断在等待里」，不作为单独的根因结论。
- 同一会话里 `rex_perfil` 记载 `juego 0.2 fps`、`cola de la ventana 0.0`、`sacar buffer 0.0`、`fences consultadas 0`，每帧 121 次绘制、10 s 内新建 4 个 pipeline。
- 也就是说：**生产者与消费者的握手断在等待里，不是 GPU 画不动。** 这与「换渲染后端能提升帧率」的假设不在同一因果链上，路线 3 换的是渲染器，**不修**这类等待/握手问题。

因此本轮先做三件事（均已落地，见交付说明）：
1. 把「客机是否还在向 ring 喂命令」变成可读数值（`[T2CP] kickoff / lotes` + guest 侧 `T2DIAG LATIDO anillo/espera/pasa`）；
2. 记录泵的三个前置阶段返回值（`T2DIAG etapa1/2/3` 与 `saltos`），确认泵是否被提前跳出而跳过呈现；
3. 去掉「带异步占位管线的整帧被跳过呈现」这一黑屏直接成因（`vulkan_async_skip_incomplete_frames=false`）。

**触发条件**（满足才值得投入路线 3）：
- 下一次实机日志显示 `kickoff / lotes / anillo` 持续增长（说明握手修通了、不是卡死），**且**
- 在 0.2 fps 现象消失后，实测帧率仍显著低于 60，**且**
- `rex_perfil` 显示 GPU 侧成为瓶颈（draws/帧、着色器数量、`fences` 等待占比升高），而非 CPU 等待。

---

## 5. 参考坐标（便于随时复查）

- 参考项目克隆：`repos/mgr-nx-deko`（`--depth 1`）
- 关键文件：`README.md`、`patches/README.md`、`patches/rexglue-sdk-v0.10.0.patch`、`patches/ffmpeg-0604b46.patch`、`patches/xenosrecomp-990d03b.patch`、`app/CMakeLists.txt`、`app/src/renut_engine/gpu/**`、`shaders/README.md`、`tools/win/build_mgr_deko_shaders.ps1`、`app/CMakeLists.txt:22-29`（两个 FATAL_ERROR 守卫）
- 渲染后端切换 cvar：`sa2_renderer` = `xenos`（模拟 Xenos GPU/PM4 ring + EDRAM）| `renut`（Renut 的 Direct3D 级渲染器 + plume/deko3d）
- 我们侧对照基线：`repos/rexglue-sdk` HEAD `c94f5ebdcb3c9d1a460ca48e04f9758448f8d518`（v0.10.0）；实际链接树 `repos/nfsmw-nx/sdk`
- NVK/Mesa 对照：`danfromtico/mesa-switch` commit `1a8c1a66d6f`（Mesa 26.2.1，参考项目所用，未含 nfsmw 驱动补丁）