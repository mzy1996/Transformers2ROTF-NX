# T2ROTF（Xbox 360《变形金刚2：卷土重来》→ Nintendo Switch）
## 提交完成中断「解除抑制」修复报告（v8.5-COMPLETE）

- 工程根：`J:\XBOX360 port\Transformers Revenge of the Fallen\`
- 后端：Renut / deko3d（plume + deko3d + uam），不链接 Vulkan/NVK
- 构建：`recomp/out/sw8`，`ninja -j4 T2ROTF`
- 本轮版本标记：`[video] T2ROTF BUILD v8.5-COMPLETE (...)`
- 本轮产物：`T2ROTF.nro` = 59,941,618 字节 / sha256 `0fa8e1fed0236cef3cf5c183d9e1b9a628284d2fc35c9c0cab4da688248c46d3`；`T2ROTF.toml` = 40,124 字节 / 23 键 / sha256 `a0455b948bb1d006363c72d9e2508df3dabe033aa8e14829d8da29a62cd0de2f`

---

## 1. 本轮输入：用户实机回传（v8.4-SPLIT 日志组）

用户描述「**还是没有画面**」，回传四份日志：

| 日志 | 文件 |
|---|---|
| 性能/时钟 | `.dumate/inbox/rex_perfil(34).log` |
| 宿主 stderr | `.dumate/inbox/rex_stderr(71).log` |
| guest 主日志 A | `.dumate/inbox/T2ROTF_001(69).log` |
| guest 主日志 B | `.dumate/inbox/T2ROTF_002(50).log` |

**版本确认**：`[video] T2ROTF BUILD v8.4-SPLIT (...)` 在日志中出现，本轮跑的就是上一版产物（banner 仍是旧的 `v6.27-MAPFIX`，不作版本判据）。

---

## 2. 判读：v8.4 修好了什么、没修好什么

### 2.1 v8.4 的修复**生效**：垂直同步线程自锁已解除

- `[T2DIAG GPSIG]` 报告**恢复每 2 秒一条**（约 72 条，v8.3 是 120 停住后一条都没有）。
- 四要素全部持续增长：`vblanks 0→2880`、`irq 0→2853`、`r1951 = mmio_r` 同步增长、`hostcb=2613/27`、`inv=ret=irq`（派发不出错、全部返回）。
- 结论：v8.4 把「每帧提交驱动」搬到独立线程 `T2ROTF Dirigir` 之后，**垂直同步线程不再被驱动拖死**，中断派发链是通的、活的。

### 2.2 仍未出画面的四条并发现象

1. **guest 提交侧始终零产出**：`mmio_w=0` 恒为 0、`wptr=0(0)`、`rptr_pub=0`、`sup1844=0`、`inKick13232=0`、`ringR16700 = ringW16704 = 0`。
2. **显示会计冻结**：`v16568`（`dev+16568` 完成计数）冻在 **5**、`v16548`（vblank 自增）涨到 2738、`cola284=0 cola288=1`、`obj=00000000`、`a16700=a16704=0`。
3. **驱动线程卡死在 guest 提交内部**：线程 `t682943169022225291` 只输出 4 行（`06:08:04.431`–`06:08:04.497`）：`RECON nr=1` → `ALIGN n=5` → `DRIVE n=153 obj=40F604E0 284 0->1` → `DRAIN-IN n=13`，此后静默；周期报告恒为 `drive=… ok=1 ini=2 dentro=1 produce=0` ⇒ **只驱动成功 1 次，之后 `dentro=1` 不归零**（卡在 guest 提交里）。
4. **管线 6 帧后停摆**：`etapa1/2/3=6/6/6`、`cadena937=8`、`despacho=8`、`fin=0/0/0`、`bomba=iny=pres=6`、`cc058=6`、`cc6e0=0`；`yield=78293` 自 `06:08:05.995` 起再无增长。
5. **呈现侧死等**：`espera sub_824F3580` 涨到 **21,440,000+**（`06:08:49` 仍在涨），恒为 `r3=F8000068 r4=1 lr=828818DC`，线程 `t6075957542970809794`。
6. **设备真相**：Renut 报告 type 2 设备建于 `0x40001b80`、type 1 设备建于 `0x40070600`；宿主解析出 `dev=40070600`，`dev+10896→BDF80000`（fence，从 `0E` 涨到 `1A` 无人消费）、`dev+10900→BDF80080`（swap 暂存块，首 8 字节 `w10900_0=0 w10900_4=0`）。
7. `rex_stderr(71)`：APM = `0x92220008`（GPU 460.8 / EMC 1331，**未超频**）；`Direction F map FAILED rc=0x0000D401`、`on-demand fault-in only`。

---

## 3. 定论：单一根因链

把四条线串成一条闭环：

```
宿主每帧驱动 guest 的每帧提交 sub_82880978
        ↓（v8.4 已让它跑起来 1 次：base+284 由 0→1）
guest 提交内部走到排空 sub_82882200 的判据 while (*(base+284) + *(base+288) != 0)
        ↓ 要退出这个循环，必须「完成掉一个在飞显示项」：让 base+288 从 1 回到 0
能在 guest 自己的 ISR 里完成掉在飞项的唯一通路 = 图形中断回调的 source=1 分支
（sub_822D47A0 用 *(dev+10900)=swap 暂存块 → 调 *(blk+16)，然后 retire 一项）
        ↓ 但这一支在本移植里**一次都没派发过**
证据：skipS1 严格等于 vblanks（120/240/…/2880），s1 == 0，dev+16568 冻在 5
        ↓ 于是 base+288 恒为 1，排空循环永远转不出去
驱动线程当场卡死在 guest 提交内部（dentro=1 不归零）
        ↓ guest 提交侧零产出（mmio_w=0 / ringR=ringW=0 / inKick=0）
呈现侧在 F8000068 上永等（espera 2.14e7+）
        ↓
黑屏（有声音，第一个 logo 能播）
```

**为什么 source=1 一次都没派发？两个原因叠加：**

1. **默认关**：`sa2_renut_gpu_signal_hostcb_s1` 的旧默认是 `false`，宿主侧 `Completado()` 一进来就 `return`（第一版还只被当作单一候选尝试，未默认启用）。
2. **判据错**（更隐蔽）：即便把开关打开，旧判据要求暂存块地址落在「guest 对象区间 `0x40000000–0x90000000`」内；而实机 `*(dev+10900) = 0xBDF80080`，是 **D3D 后端自己的 fence/暂存块**，落在 GPU 物理窗口 `0xB…` 段 ⇒ 被旧判据一律判成「不是可用暂存块」，一条都不发。

**关键纠正**：present 侧等的对象是 `F8000068`（`*(0x82DBB5E0)`，`svcWaitForSingleObjectEx` 的 alertable 等待），不是 `F8000060`（`*(0x82DBB5A0)`，已为 0）。`F8000068` 由 guest 自己的 ISR 尾部在完成一项后置位——所以**正确修法不是宿主替它写事件句柄**，而是**把 source=1 中断真正派发进 guest ISR，让 guest 自己走完「retire 项 → 置位事件」这条原生活儿**。

---

## 4. 本轮改动（`recomp/src/t2rotf_gpu_signal.cpp`，64,966 → 68,966 字节）

严格保持既有约束：**不写任何显示会计字段**、**不碰门/泵热路**、**不改 v8.2 已验证的 shadow 安全访存**（`Leer32`/`Escribir32` 继续走 `RexGmShadowFor` 别名，取不到就跳过并计数 `sinSombra`）、**不重复投递**、**不直读直写 guest 内存**、`presenter()/provider()` 仍为 null（画面仍由 Renut/deko3d 出）。

### 4.1 解除 source=1（提交完成）中断的抑制

- `sa2_renut_gpu_signal_hostcb_s1` 默认 **`false → true`**（`kInitOnly`）。
- **判据修正**：暂存块校验从「必须落在 guest 对象区间」放宽为**只要求非零、4 字节对齐、落在可寻址 32 位范围内**（`!blk || (blk & 3u) != 0 || blk > 0xFFFFFFF0u` 才跳过）。绝不写它，只读 + 派发。
- 派发实现：`Completado(cpu)` 读 `*(dev+10900)` 取暂存块，把 `*(blk+16)` 纳入 `cb_guard` 一次性守卫（非 guest 代码地址时本次派发期间临时清零、派发后还原），再经 `Enviar(callback, 1, dev, cpu)` → `ExecuteInterrupt(..., source=1, datos=dev, 2)` 走 guest 自己的图形中断回调。

### 4.2 新增 `sa2_renut_gpu_signal_hostcb_s1_mode`（派发时机，`kInitOnly`）

- `0` = 永不派发；
- `1` = **仅在飞显示项确实存在时**派发（`*(0x82E55CE0+284) | *(0x82E55CE0+288) != 0`）——用于 A/B 判定「多余的完成上报是否有害」；
- `2` = **每次刷新都派发**（缺省）——与硬件「一帧一次提交完成中断」同节奏。

### 4.3 `dev=` 诊断行补齐「门就绪判据的全部输入」（只读）

`[video] T2DIAG GPSIG2 dev=…` 新增只读字段：`ptr272`（`*(0x82E55CE0+272)` 槽表基址）、`c16`（`*(0x82E55CE0+16)`）、`A0/A1/A2`（表 `[slot*32+24]`，已投递）、`C0/C1/C2`（表 `[slot*32+28]`，未交付）、`dv10904`（`*(dev+10904)` 每 CPU 在飞掩码）、`blk0`/`blk4`（暂存块前 8 字节；全 0 = 无待消费的完成记录）。下一轮日志即可直接指出门到底缺哪一项。

### 4.4 保持不变的既有语义

- v8.4 的驱动线程拆分（`T2ROTF Dirigir` + `thread_local g_en_hilo_dirigido` 守卫）、让位语义、退出安全阀**原样保留**；
- v8.2 的 shadow 安全访存与设备分层解析**原样保留**；
- 版本标记改为 `[video] T2ROTF BUILD v8.5-COMPLETE (...)`。

---

## 5. 产物与校验

| 项 | 值 |
|---|---|
| `recomp/out/sw8/T2ROTF.nro` | **59,941,618 字节**，sha256 `0fa8e1fed0236cef3cf5c183d9e1b9a628284d2fc35c9c0cab4da688248c46d3`（上一版 v8.4 = 59,937,522 / `9d78b957…`，字节数与哈希都不同） |
| `T2ROTF.toml` | **40,124 字节**，**23 键**（新增 1 键 `sa2_renut_gpu_signal_hostcb_s1_mode`；`sa2_renut_gpu_signal_hostcb_s1` 默认翻 **true**），sha256 `a0455b948bb1d006363c72d9e2508df3dabe033aa8e14829d8da29a62cd0de2f` |
| 包内 `recomp_src/t2rotf_gpu_signal.cpp` | 64,966 → **68,966 字节** |
| `recomp/src/t2rotf_gpu_signal.h`、`recomp/src/t2rotf_video_nativo.cpp` | **未改动** |

**构建**：`ninja -j4 T2ROTF` 退出码 **0**；再跑仍 no-op（`Codegen summary: 0 written, 0 unchanged, 0 deleted, 1 module(s) up to date`，NRO 的 mtime/sha256 不变）⇒ 构建完整收尾且可复现。

**字符串取证**（`strings -a T2ROTF.nro | grep -c -F`）：`v8.5-COMPLETE`=3、`sa2_renut_gpu_signal_hostcb_s1_mode`=4、`sa2_renut_gpu_signal_hostcb_s1`=8、`T2DIAG GPSIG2`=3、`blk0=`=1；NRO 内的 `[video] T2ROTF BUILD v8.5-COMPLETE (…)` 完整帮助文本可读。

**toml 自检**：`tomllib` 解析通过，23 键读回正确（`hostcb_s1=true`、`hostcb_s1_mode=2`）。

---

## 6. 开关语义与回退阶梯

本轮新增/变更的键（**都是 `kInitOnly`，改完必须完全退出再启动**）：

- `sa2_renut_gpu_signal_hostcb_s1`（默认 **true**）：是否派发 source=1 提交完成中断。
- `sa2_renut_gpu_signal_hostcb_s1_mode`（默认 **2**）：派发时机（0/1/2，见 §4.2）。

回退阶梯（逐级退回，任一级都能单独复现上一版行为）：

1. `sa2_renut_gpu_signal_hostcb_s1 = false` → 退回 v8.4 行为（提交完成中断重新被抑制）；
2. `sa2_renut_display_drive_thread = false` → 退 v8.3（驱动回到垂直同步线程，实机会自锁，A/B 用）；
3. `sa2_renut_display_drive = false` → 退 v8.2-SHADOW（中断链通、每帧提交侧不产出）；
4. `sa2_renut_gpu_signal_dev_hook = false` → 退 v8.1 设备解析（影子访存修复保留）；
5. `sa2_renut_gpu_signal_hostcb = false` → 退 v8.0 空转；
6. `sa2_renut_gpu_signal = false` → 退 v7.7 的 0 fps 黑屏。

---

## 7. 实机验证步骤与需回传日志行

1. 把新的 `T2ROTF.nro` + `T2ROTF.toml` 覆盖到 `sdmc:/switch/Transformers2ROTF/`，**完全退出再启动**。验收场景是**不超频手持**（60 帧结论只在**不超频**条件下给）。
2. **先确认版本**：日志里出现 `[video] T2ROTF BUILD v8.5-COMPLETE (…)`。
   （同日志里仍会有 `v7.6-RECON`/`v7.7-ALIGN`/`v8.0-GPUIRQ`/`v8.1-GUESTCB`/`v8.2-SHADOW`/`v8.3-FRAME`/`v8.4-SPLIT` 等旧标记行——它们是 cvar 帮助文本与历史判读行，都属于本 NRO，不是版本混装；应用自身 banner 仍是 `v6.27-MAPFIX`，**不要用它判版本**。）
3. **是否仍崩**：`rex_crash.log` 应**不再生成**（本版不应引入新崩）。
4. **本轮最关键验收点**：
   - `[T2DIAG GPSIG …]` 里 **`s1` 是否开始增长**（v8.4 恒 0、`skipS1 == vblanks`）——增长即说明提交完成中断已被派发；
   - **`dev+16568`（`v16568`）是否脱离 5**、`[T2DIAG SWAP n=]` 是否 > 1、`mmio_w`/`inKick13232`/`ringR16700` 是否开始增长；
   - `[T2DIAG DRIVE drive=… ok=… ini=… dentro=…]` 的 `dentro` 是否回到 **0**（若仍恒 1 = 驱动线程仍卡在 guest 提交内，说明这轮修法还不足以让它退出排空循环）、`ini` 是否持续增长；
   - `fin=`/`disp16568` 是否开始增长、`espera sub_824F3580` 是否停止单调增长、`juego` fps 是否 **> 0**、**画面是否出现**三连 logo。
5. **需回传的日志行**：`[video] T2ROTF BUILD v8.5-COMPLETE …`；`[video] T2ROTF GPSIG:`（全部，特别是 `宿主代客派发 source=1（提交完成）callback=… dev=… blk=… blk16=…` 那条，若无则说明 `Completado()` 仍在跳过）；`[T2DIAG GPSIG …]`（首末各一，重点看 `s1`/`mmio_w`/`skipS1`）；`[T2DIAG GPSIG2 dev=…]`（首末各一，重点 `A0/C0/A1/C1/A2/C2`、`dv10904`、`blk0/blk4`）；`[T2DIAG DRIVE …]`（首末各一）；`[T2DIAG SWAP …]`（末条）；`[T2DIAG PERRO …]` 的 `fin=`；`rex_perfil.log` 的 `juego … fps` 与 `relojes:` 行（须未超频值）；`rex_crash.log`（若产生）。
6. **判读分支**：
   - A（`s1` 增长 + `v16568`/`SWAP n=`/`mmio_w` 开始变 + `dentro=0` + fps>0 + 画面出现）= 修复生效、画面出；
   - B（`s1` 增长但仍 `dentro=1`、`base+288` 仍恒 1）= 提交完成中断到了 guest，但 guest 自己的 retire 路径没走通 ⇒ 回传那条 `source=1` 派发行的 `blk/blk16` 与 `GPSIG2` 末行，下一轮改用 `mode=1` 并核对 ISR 内部；
   - C（`s1` 仍恒 0）= 派发仍未发生（`dev+10900` 为空或对齐判据仍挡）⇒ 回传 `GPSIG2` 的 `blk10900`/`blk16`；
   - D（仍有 `rex_crash.log`）= 回传新崩溃的 `pc/far/esr`（前 40 行）。
7. 本轮**仍不包含**帧率达标：画面出来后才能在**不超频**条件下实测帧率并给「不超频 60 帧」结论（见 README 第 5 节与待办 T26）。

---

## 8. 残余风险与下一步

- **本版尚未实机验证**。v8.5 解除的是「提交完成中断被抑制」这一条；它**不保证** guest 自己的 retire 路径在当前 HLE/Renut 环境下一定走得通——若下轮出现分支 B（`s1` 涨而 `dentro` 仍 1），则说明 `base+288` 的孤儿在飞项另有独立减点，需要转入 guest ISR 内部逐指令取证。
- **`sub_82881930`（交付）尚未逐行读**：其内部 yield 点（LR `0x82881918`）与「成功交付」的返回条件仍待精读，作为分支 B 的下一步。
- **「不超频流畅 60 帧」仍未达成也未实测**（T26 待做）。
- **原请求中的 TU / DLC 集成**：用户明确要求使用上级目录 `TFROTF TU` 升级档与 `Transformers 2 - Maps and Characters Pack` DLC 包；工程已具备 `update/`+`patched/`+`dlc/` 独立目录与安装期 `xex_patcher` 融合流程（`scripts/setup_install.sh`），但**尚未在实机产物上闭环验证**，仍属待办。