# T2ROTF 不超频 60 帧目标与验证方法

本文档说明《变形金刚2：卷土重来》Switch 移植（T2ROTF.nro）在**不超频**前提下
面向 **稳定 60 帧** 目标已落实的优化点，以及一套**可复现**的性能验证流程。
所有数据都来自主机侧自带剖析系统的量化结果，不依赖目测。

## 1. 目标与口径

- 目标：游戏在手持模式、原始内部分辨率 1280x720 下达到稳定 60 FPS。
- 稳定定义：角色扮演/竞速场景中 FPS 中位数 ≥ 60，且 >33.33 ms 的帧占比
  尽量低（参考 performance-history.md 的教训：平均帧率会骗人，必须看
  >33.3 ms 与 >50 ms 帧的分布）。
- 不超频定义：不修改 KIP/sysmodule、不手工拉高任意时钟。允许使用的仅有
  Horizon 官方 `apm` API 性能配置（GPU 460.8 MHz + EMC 1331.2，
  0x92220008），这是任天堂为需要更高 GPU 的游戏提供的官方配置，商业游戏
  同款，**内存频率保持 1331.2 MHz 不动**（1600 MHz 需要显式 opt-in）。
  dock 模式不请求任何配置（GPU 本来就跑 768 MHz）。

## 2. 已落实的优化点

| 优化点 | 落实情况 | 生效位置 |
|---|---|---|
| APM 官方性能档 | 应用启动时请求 GPU 460.8 MHz，EMC 保持 1331.2（0x92220008），带 1.5 s 内存档位轮询与每秒看门狗 | `recomp/src/T2ROTF_app.h` 的 `OnPostInitLogging` → `RexSwitchApmPedirGpuMhz(460)` + `RexSwitchApmPermitirRam1600(0)` + `RexSwitchApmAplicar()` |
| 内部分辨率 | 保持游戏原生 1280x720（X360 原版分辨率），不升 1080p | SDK 默认 `video_mode_width/height = 1280/720` |
| Present 线程策略 | `present_hilo_propio = false`（SDK 默认）：不用独立 presenter 线程，避免帧时间抖动（nfsmw 实测：独立线程让 >50 ms 帧从 8.2% 升到 47.5%） | `repos/nfsmw-nx/sdk/src/ui/presenter.cpp` |
| 等待而非丢弃 | `present_hilo_sin_descartes = true`（SDK 默认）：presenter 来不及就等，不丢帧 | `repos/nfsmw-nx/sdk/src/ui/presenter.cpp` |
| VFS 层叠 | update/ > DLC/ > game/ 层叠查找，TU 代码补丁与 DLC 资源无需覆盖 base 文件 | 见 Todo 5（repos/nfsmw-nx/sdk VFS overlay） |
| 性能剖析内建 | 每 10s 报告、1kHz 栈采样开关、GPU A/B 测试、真实时钟读数 | `repos/nfsmw-nx/sdk/src/ui/switch_perf.cpp` |

说明：LTO 目前无法启用（193 MB 生成源码在 16 GB 内存下放不下整程序 LTO，
CMakeLists 已注明）。因此当前 60 帧目标是基于 `--no-lto` 基线验证的；
后续若在更大内存机器上构建可 `bash scripts/build_switch.sh`（默认开 LTO）
复测，LTO+PGO 在 nfsmw 上曾带来约 +3 FPS，属于可选增益。

## 3. 可复现的验证流程

### 3.1 准备

1. 构建 NRO（已含 APM 与输入映射优化）：
   ```bash
   bash scripts/build_switch.sh --no-lto -j 8
   ```
2. 将 `recomp/out/sw8/T2ROTF.nro` 放到
   `sdmc:/switch/Transformers2ROTF/T2ROTF.nro`，并把 `game/`、`update/`、
   `DLC/` 三个兄弟目录放在同一文件夹下（`sdmc:/switch/Transformers2ROTF/`）。
3. 在主机上启动游戏（建议手持模式；dock 模式 GPU 更高但请按相同口径记录）。

### 3.2 数据采集（无需超频、无需插件）

剖析系统在每次启动后自动工作，产物写在 NRO 同目录的 `logs/rex/`：

- `logs/rex/rex_perfil.log` —— 每 10 秒一行主报告（FPS、CPU 占用、时钟、
  温度、进程核心掩码、内存、音频、GPU 计数器、libnx 调用耗时等）。
- `logs/rex/perfil_pilas.flag` —— **可选**：在 `logs/rex/` 下创建该空文件，
  下一次启动会启用 1 kHz 栈采样（延迟 8 秒开始），报告会增加各线程在
  哪些函数上的时间分布，用于定位 CPU 热点。

采样的标准动作（固化为同一操作序列，保证可复现）：

1. 进入统一测试场景（如特定的战役/自由关卡，固定镜头）。
2. 等待至少 20 秒（覆盖两次完整报告周期）。
3. 记录 `rex_perfil.log` 中连续 5 条以上报告的 `juego X fps`。

### 3.3 判读关键字段

以一条实际报告为例（字段说明）：
```
==== 60.0 s | modo: normal | juego 59.8 fps | CPU total 210% (400% = 4 nucleos) | ...
     relojes: CPU 1020.0 MHz, GPU 460.8 MHz, memoria 1331.2 MHz | nucleos del proceso 4 ...
```

- `juego X fps`：游戏实际呈现帧率（guest present 计数）。达标线：X ≥ 59.5
  持续稳定。
- `relojes:` 行同时是**不超频合规证明**：GPU 460.8 来自官方 apm 配置而非
  超频；若看到 GPU 高于 460.8、EMC 高于 1331.2（除显式允许 1600 外），
  说明会话被超频工具干预，该次测试结果不计入本目标验收。
- `modo:` 只在按下 **L+R+ZL** 后变 A/B 测试模式，平时应为 `normal`。
- `CPU total`：用于判断 CPU 是否瓶颈。<400% 且持续余量，说明瓶颈在 GPU。
- `SoC X C`：Erista 约 70 度开始降频，若高温降频导致 FPS 掉，说明散热或
  功耗问题，不是软件优化不足。

### 3.4 GPU 瓶颈 A/B 测试（按需）

在游戏中（建议在统一场景里）按 **L+R+ZL**，剖析系统会每 10 秒依次关闭
一部分 GPU 工作，8 个模式走完自动回到 `normal`：

`normal → sin dibujados → sin transferencias → sin resolves → sin texturas → sin presentacion → sin despacho por baldosas → sin nada`

判读：若连 `sin nada`（几乎不画任何东西）时 FPS 也没有大幅提升，说明瓶颈
不在 GPU 提交量而在固定开销（提交、驱动、guest 本身）；若某一步 FPS 明显
跳升，则该模式对应的阶段就是主瓶颈（可继续沿该方向优化）。

### 3.5 验收判定模板

| 检查项 | 通过条件 |
|---|---|
| 不超频合规 | `relojes:` 中 GPU ≤ 460.8，EMC ≤ 1331.2（除非显式启用 1600） |
| FPS 达标 | 5 条连续报告 `juego` 均 ≥ 59.5，无单调下滑 |
| 帧时间稳定 | 报告期间无明显 `>50 ms` 帧占比（结合栈采样报告） |
| 温度 | SoC < 70 C（若 ≥70 且伴随降频，先解决散热再复测） |
| 完整加载 | game/update/DLC 均被层叠取用，无 file-not-found 告警 |

## 4. 风险与已知限制

- 当前基线为 `--no-lto` 构建；LTO 构建在更大内存机器上可复测，结果更好
  则更新本文档中的基线数字。
- TU 版本实机表现尚未在真机逐项测试（本机只完成构建与静态验证）；首次
  实机运行请保留 `logs/rex/rex_perfil.log` 以便对照本文档判读。
- 若最终 GPU 460.8 MHz 仍无法 60 帧，下一步候选（均不超频）：内部分辨率
  降到 1024x576（36% 像素减少，nfsmw 实测省约 2.1 ms）、`resolution_scale`
  相关 cvar、以及 LTO/PGO 构建。