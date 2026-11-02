# T2ROTF FFmpeg VC-1 桥接完整修复 — 可行性评估与实施路线图

> 文档版本：v6.35a（2026-10-20）
> 状态：可行性确认，待实施

---

## 1. 结论（TL;DR）

**可行，且是唯一正确的长期修复方案。**

符号链接权宜之计（v6.35）只能绕过 WVC1 解码死循环，让游戏继续运行，但**所有 10 个 WVC1 视频会播放错误的画面**（`atvi_logo.wmv`）。要让片头、结局和 G1 解锁视频**正确解码并显示原版画面**，必须替换掉 T2ROTF 原生重编译的 WMV3 解码器，改用 FFmpeg 的 VC-1 解码器。

扩展调研确认：
- **唯一可复用的完整实现**：`nfsmw-nx`（`nfsmw_video_wmv3.cpp/h` + `nfsmw_video_nativo.cpp`）
- **Context 偏移 100% 同源**：T2ROTF 的 `+3672/+3676/+3680`（Y/U/V 平面）、`+280`（帧类型）、`+15708/15712`（I/P 解码指针）、`+3016`（B 解码指针）与 nfsmw 完全一致
- **FFmpeg 编译已验证**：masseffect-nx / sa2-nx / nfsmw-nx 三个项目的 SDK 均已成功将 FFmpeg（含 `CONFIG_VC1_DECODER`）编译进 Switch ARM64 NRO
- **T2ROTF 尚未链接 FFmpeg**：当前 `recomp/CMakeLists.txt` 无 `libavcodec` / `libavutil` 引用，需要补充构建规则

---

## 2. 根因回顾

T2ROTF 的 10 个片头/尾视频为 **WVC1（VC-1 Advanced Profile，含 B 帧）**：

- `luxo_logo.wmv`
- `a0_intro_co.wmv`
- `A6_END_CO.wmv`
- `D6_END_CO.wmv`
- `Unlock_G1Ep_1.wmv` … `Unlock_G1Ep_6.wmv`

`ffprobe` 输出：
- Codec: `wvc1` (Windows Media Video 9 Advanced Profile)
- Profile: `Advanced@L3`
- B-frames: present

T2ROTF 原生重编译解码器仅支持 **WMV3（VC-1 Main/Simple Profile，无 B 帧）**。WVC1 的 B 帧参考帧管理、loop filter 与帧类型解析与 WMV3 不兼容，导致解码器在 WVC1 码流上进入死循环（CPU 满载 199%，输出全黑，帧率 0 fps）。

---

## 3. 参考项目调研汇总

| 项目 | 视频桥接 | FFmpeg 配置 | 可复用性 |
|------|---------|-------------|---------|
| **nfsmw-nx** | ✅ 完整（`video_wmv3.cpp` + `video_nativo.cpp`） | ✅ `CONFIG_VC1_DECODER` + `CONFIG_WMV3_DECODER` | **唯一完整方案** |
| NFS-Carbon-NX | ❌ 核心代码未公开（私有仓库） | — | 不可参考 |
| masseffect-nx | ❌ 无视频桥接 | ✅ SDK 集成 FFmpeg（仅 XMA 音频） | 仅验证 FFmpeg 编译可行性 |
| sa2-nx | ❌ 无视频桥接 | ✅ SDK 集成 FFmpeg（仅 XMA 音频） | 仅验证 FFmpeg 编译可行性 |
| prototype2-nx | ❌ 无视频桥接 | — | 不适用 |
| LostOdysseyRecomp | ❌ 重编译器本身，非移植项目 | — | 不适用 |
| torchlight-nx | ❌ 仅有图形渲染 | — | 不适用 |
| project8-sw | ❌ 仅有系统视频模式查询 | — | 不适用 |

**结论**：只有 `nfsmw-nx` 包含可直接参考的 `REX_HOOK_RAW` 替换 + FFmpeg 解码 + ASF 容器解析完整链路。其余项目要么无视频解码器，要么仅集成 FFmpeg 用于音频解码（XMA）。

---

## 4. 技术方案设计

### 4.1 整体架构（与 nfsmw-nx 同构）

```
T2ROTF guest 代码
  │ 调用 DecodeData (guest addr)
  │
  ▼
REX_HOOK_RAW 替换（host 侧 C++）
  │
  ├── 1. ASF 容器解析（从 VFS 读取 .wmv 文件，解析 Header / Data 对象）
  │   └── 复用 nfsmw_video_wmv3.cpp 的 ASF 解析逻辑
  │
  ├── 2. 帧数据提取（从 ASF packets 重组完整帧）
  │   └── 复用 nfsmw_video_wmv3.cpp 的 payload 重组逻辑
  │
  ├── 3. FFmpeg VC-1 解码（avcodec_send_packet / avcodec_receive_frame）
  │   └── 复用 nfsmw_video_wmv3.cpp 的 DescodificadorWmv3 类
  │       关键：使用 AV_CODEC_ID_VC1（而非 WMV3）
  │
  └── 4. YUV 平面写回 guest 上下文
      └── [ctx+3672] Y, [ctx+3676] U, [ctx+3680] V
          复用 nfsmw_video_nativo.cpp 的写回逻辑
```

### 4.2 需要 hook 的函数（T2ROTF guest 地址）

| 角色 | nfsmw 地址 | T2ROTF 候选地址 | 确认方法 |
|------|-----------|----------------|---------|
| Prepare / 初始化 | `0x8272FD30` | **待定位** | 在 `luxo_logo.wmv` 打开后，第一个被调用的视频相关函数 |
| DecodeData | `0x827312C0` | `0x827EB2E8` 或 `0x8279D6D8` | 分析 `lwz r11,280(r3)` 帧类型判断 + 3 次 `REX_CALL_INDIRECT_FUNC` |
| 数据回调 | `0x82749C10` | **待定位** | DecodeData 内部调用的数据读取函数 |
| I 帧解码 | `0x828C35D8` | **待定位** | `[ctx+15708]` 指向的函数指针 |
| P 帧解码 | `0x8278A518` | **待定位** | `[ctx+15712]` 指向的函数指针 |
| B 帧解码 | `0x828C58C8` | **待定位** | `[ctx+3016]` 指向的函数指针 |

**Context 偏移（已确认与 nfsmw 一致）**：
- `[ctx+3672]` / `[ctx+3676]` / `[ctx+3680]`：Y / U / V 平面指针
- `[ctx+216]` / `[ctx+220]`：Y / UV 行偏移（stride）
- `[ctx+280]`：帧类型（0=I, 1=P, 2=B）
- `[ctx+15708]` / `[ctx+15712]`：I / P 帧解码函数指针
- `[ctx+3016]`：B 帧解码函数指针
- `[ctx+3844]`：loop filter 标志
- `[ctx+15424]`：profile（WMV3 vs WVC1）

### 4.3 FFmpeg 编译与链接

1. **编译 Switch ARM64 静态库**
   - 源码：`repos/FFmpeg`
   - 配置：参考 `nfsmw-nx/sdk/thirdparty/FFmpeg/config_nfsmw_wmv3.h`
   - 必须启用：`CONFIG_VC1_DECODER`、`CONFIG_VC1DSP`、`CONFIG_WMV3_DECODER`
   - 产物：`libavcodec.a`、`libavutil.a`

2. **T2ROTF CMakeLists.txt 修改**
   - 参考 `nfsmw-nx/app/CMakeLists.txt` 的 `target_link_libraries(... PRIVATE libavcodec libavutil)`
   - 添加 `recomp/src/t2rotf_video_wmv3.cpp` 和 `recomp/src/t2rotf_video_nativo.cpp` 到源文件列表

---

## 5. 实施步骤（预估 1-2 天）

### Phase 1：地址定位与验证（0.5-1 天）

1. **运行 v6.35 NRO 到视频播放阶段**
   - 由于符号链接已将 WVC1 重定向到 WMV3，游戏能正常运行到主菜单
   - 但需要**临时关闭符号链接**（注释掉 `RegisterSymbolicLink` 调用），让 `luxo_logo.wmv` 真正进入 WVC1 解码路径
   - 在实机或模拟器上运行，捕获 `rex_stderr.log` 和 `T2ROTF_001.log`

2. **定位 6 个 hook 地址**
   - 在 `T2ROTF_recomp.*.cpp` 中搜索与 nfsmw 同构的代码模式：
     - `lwz r11,280(r3)` + `cmpwi cr6,r11,0`（帧类型判断）
     - 3 次 `REX_CALL_INDIRECT_FUNC`（I/P/B 解码调用）
     - `lwz r11,3176(r31)` 或类似偏移（数据回调加载）
   - 或使用 `REX_HOOK_RAW` 在候选地址上打桩，通过日志确认调用时机

3. **确认 context 偏移**
   - 虽然已静态确认一致，但建议在 hook 中打印 `ctx + 3672/3676/3680` 的实际值，验证运行时确实指向 YUV 缓冲区

### Phase 2：代码移植（0.5-1 天）

1. **创建 `recomp/src/t2rotf_video_wmv3.h`**
   - 从 `nfsmw_video_wmv3.h` 复制，重命名 namespace（`t2rotf::video_wmv3`）
   - 保留 `DescodificadorWmv3` 类，但将 codec 改为 `AV_CODEC_ID_VC1`（见 4.3 节）

2. **创建 `recomp/src/t2rotf_video_wmv3.cpp`**
   - 从 `nfsmw_video_wmv3.cpp` 复制，重命名 namespace
   - ASF 容器解析逻辑**完全复用**（WVC1 和 WMV3 的 ASF 封装格式相同）
   - 修改 `DescodificadorWmv3::Abrir()`：
     ```cpp
     // 原 nfsmw 代码：
     // AVCodec* wmv3 = avcodec_find_decoder(AV_CODEC_ID_WMV3);
     // 改为 VC-1：
     AVCodec* vc1 = avcodec_find_decoder(AV_CODEC_ID_VC1);
     if (!vc1) { ... }
     e_->codec = avcodec_alloc_context3(vc1);
     // 其余代码（width, height, extradata, avcodec_open2）不变
     ```
   - `Descodificar()` 逻辑**完全复用**（avcodec_send_packet / avcodec_receive_frame / 平面拷贝）

3. **创建 `recomp/src/t2rotf_video_nativo.cpp`**
   - 从 `nfsmw_video_nativo.cpp` 复制，重命名 namespace（`t2rotf::video_nativo`）
   - 替换 `REX_HOOK_RAW` 的 guest 地址为 T2ROTF 实际地址（Phase 1 定位结果）
   - `REX_EXTERN` 声明对应的 `__imp__sub_XXXXXXXX`
   - 保留 `NfsmwUltimoWmvLeido()` 的等价机制（或改用 VFS 直接读取当前播放的文件名）

4. **修改 `recomp/src/T2ROTF_app.h`**
   - 在 `OnPostSetup()` 中调用 `t2rotf::video_nativo::Instalar()`（注册 hooks）
   - **保留** `RegisterSymbolicLink` 作为 fallback：如果 FFmpeg 桥接未编译或未启用，符号链接仍然生效

5. **修改 `recomp/CMakeLists.txt`**
   - 添加 `t2rotf_video_wmv3.cpp`、`t2rotf_video_nativo.cpp` 到源文件列表
   - 添加 `target_link_libraries(... PRIVATE libavcodec libavutil)`
   - 参考 `nfsmw-nx/app/CMakeLists.txt` 的 FFmpeg include 路径和 link 路径

### Phase 3：构建与测试（0.5 天）

1. **编译 FFmpeg Switch ARM64 静态库**
   - 使用 `repos/FFmpeg` + devkitPro 工具链
   - 配置脚本参考 nfsmw-nx 的 `sdk/thirdparty/FFmpeg/build_switch.sh`（如存在）

2. **构建 T2ROTF NRO**
   - `bash scripts/build_switch.sh --no-lto -j 8`
   - 确认链接无未定义符号（`avcodec_find_decoder`、`avcodec_send_packet` 等）

3. **实机测试**
   - 关闭符号链接（或改用 cvar 开关 `t2rotf_video_wmv3_nativo`）
   - 播放 `luxo_logo.wmv`：预期**有画面有声音**，帧率 > 30 fps
   - 检查 `rex_stderr.log` 中 FFmpeg 解码日志（`[video] VC-1 nativo: avcodec_open2 ok` 等）
   - 对比原版 WMV3 视频（`atvi_logo.wmv`）的播放流畅度，确认无退化

---

## 6. 风险与缓解

| 风险 | 可能性 | 影响 | 缓解措施 |
|------|--------|------|---------|
| T2ROTF 的 DecodeData 地址与候选不符 | 中 | 高（hook 点错误导致崩溃或无效） | Phase 1 用 `REX_HOOK_RAW` 打桩 + 日志确认；准备 2-3 个候选地址逐一验证 |
| FFmpeg VC-1 解码器在 Switch ARM64 上性能不足（< 30 fps） | 低 | 中（视频卡顿） | VC-1 Advanced Profile 的复杂度与 WMV3 相近；nfsmw-nx 已验证 FFmpeg 解码器在 Switch 上能实时解码 720p WMV3；若性能不足，可尝试 `AV_CODEC_FLAG_LOW_DELAY` + 单线程 |
| ASF 容器解析遇到 T2ROTF 特有的封装差异 | 低 | 中（解析失败） | ASF 格式标准化；先用 PC 端测试 ASF 解析逻辑，确认能正确提取 T2ROTF 的 WVC1 帧数据 |
| B 帧重排序问题（FFmpeg 输出顺序与游戏预期不一致） | 中 | 高（画面撕裂/跳帧） | nfsmw-nx 的 `AV_CODEC_FLAG_LOW_DELAY` + `has_b_frames = 0` 已解决此问题；直接复用 |
| 构建系统冲突（CMake 找不到 FFmpeg 头文件或库） | 低 | 低 | 参考 masseffect-nx / sa2-nx / nfsmw-nx 的 CMake 配置；本地有 3 个已验证样本 |

---

## 7. 与符号链接权宜之计的对比

| 维度 | 符号链接权宜之计（v6.35） | FFmpeg 桥接完整修复 |
|------|------------------------|---------------------|
| **画面正确性** | ❌ 播放 `atvi_logo.wmv`（错误画面） | ✅ 播放原版 WVC1 视频 |
| **音频正确性** | ✅ 正常 | ✅ 正常 |
| **游戏流程** | ✅ 不被阻塞 | ✅ 不被阻塞 |
| **实现工作量** | 已完成（30 分钟） | 预估 1-2 天 |
| **长期维护** | 临时方案，视频内容错误 | 正确方案，无技术债 |
| **性能** | 无额外开销 | FFmpeg 解码器开销可控（nfsmw 已验证） |

---

## 8. 立即行动项（下一步）

1. **用户决策**：是否立即启动 FFmpeg 桥接实施？
   - 若 **是**：按 Phase 1 开始定位 T2ROTF 的 6 个 hook 地址
   - 若 **否**：保留 v6.35 符号链接方案，后续版本再集成 FFmpeg 桥接

2. **如需启动，请提供**：
   - 运行 v6.35 NRO（关闭符号链接后）的 `rex_stderr.log` + `T2ROTF_001.log`（用于精确定位 hook 地址）
   - 或：直接在实机上用 v6.35 NRO 运行到 `luxo_logo.wmv`，带回日志

3. **本地 FFmpeg 编译**：
   - `repos/FFmpeg` 已就位，可直接开始配置 Switch ARM64 交叉编译
   - 参考配置：`nfsmw-nx/sdk/thirdparty/FFmpeg/config_nfsmw_wmv3.h`
   - 关键参数：`--enable-cross-compile --target-os=horizon --arch=aarch64 --cpu=cortex-a57 --enable-static --disable-shared --enable-gpl --enable-version3 --disable-programs --disable-doc`

---

*文档结束。如有疑问或需要启动实施，请继续对话。*
