# T2ROTF（变形金刚2：卷土重来 / Xbox 360 → Nintendo Switch）deko3d 后端
# 排空自旋根因与完成侧配平报告（T38 / v7.6-RECON）

- 报告日期：2026-10-29（构建时间 2026-10-29 00:48–00:51）
- 后端：Renut / deko3d（plume + deko3d + uam），不含 Vulkan/NVK 运行路径
- 上一版：v7.5-DEKO3D-GUARD（T31），NRO 59,847,410 字节 sha256 `3728fd7e…`
- 本版：v7.6-RECON（T38），NRO **59,855,602 字节 sha256 `421923cee8c53a66be9b6e0c268d5576f68d139c6e59be81ae7b7156a42b10bb`**
- 判读依据（用户回传的实机日志四份）：`rex_perfil(28).log`、`rex_stderr(64).log`、`T2ROTF_001(62).log`、`T2ROTF_002(43).log`

---

## 0. 结论摘要

1. **v7.5 的修复已确认生效**：实机不再崩溃（`rex_stderr(64).log` 25 行只有初始化流程，无 `Unhandled guest access violation`），不再出现 `cola288` 被扣穿回绕成 `0xFFFFFFFF`（PUERTA 采到 n=3600 万次时 `cola288` 恒为 1），四个守卫（`GUARDIA`）、两个只读化分支（`cola-readonly` / `release`）计数全 0。
2. **但仍然 0 fps、第一个 logo 之后有声无画**，卡点被定量钉到 **guest 自己的排空循环**：`sub_82882200` 的判据 `while (*(base+284) + *(base+288) != 0)` 永不为 0（YIELD 3,543 万次全部 `lr=0x82881DE8`）。
3. **T36 用生成代码逐字定论**：在飞显示项 `*(base+288)` 全程序只有两个增减点——`++` 在交付 `sub_82881930`、`--` 在泵 `sub_82881E00` 的完成回调之后，两者本应在同一次泵调用里成对；实机读数 `284=0 / 288=1 / e24=0 / e28=1` 证明存在一次「已交付、完成侧没执行完」的**悬挂**。本移植没有 VSync / GPU 完成中断源（Xbox 上由 ISR 族兜底），这条残留自身又满足门的条目判据，于是门每轮报就绪、交付每轮弹不到记录、泵报 0、排空无限重旋。
4. **本版修复 = 开关 D**：只在**排空入口**（每帧 1 次，绝不落在门/泵热路）按 guest 自己完成侧的动作退掉这条残留（配对、绝不回负、单帧上限 64、不碰待交付侧 `284` / `entry+24`）。
5. **本版尚未实机验证**；**不超频 60 帧仍未达成、也未实测**（待 T26）。

---

## 1. 本轮判读（T35）——实机日志实测读数

### 1.1 四份日志概况

| 日志 | 行数 | 字节 | 内容要点 |
| --- | --- | --- | --- |
| `T2ROTF_001(62).log` | 425 | 79,427 | renut + GPU 后端 + 视频探针全量 |
| `T2ROTF_002(43).log` | 411 | 76,051 | 同上（另一次启动） |
| `rex_perfil(28).log` | 758 | 49,594 | 帧率 / 时钟 / 线程 CPU / 音频 / 显示会计 |
| `rex_stderr(64).log` | 25 | 1,571 | 启动期 stdin/stderr（崩溃判据在这里） |

`T2ROTF_001(62).log` 的 `T2DIAG` 标签直方图：`PERRO` 46、`PUERTA` 41、`PERRO2` 15、`YIELD` 15、`trampa` 14、`espera` 13、`cadena` 10、`puerta` 8、`pre1/pre2/post` 各 8/8、`etapa1/2/3` 各 6、`init` 6、`cola` 6、`externo`/`worker` 各 5、`despacho` 3、`SWAP` 1、`COLA` 1、`fourcc` 1。

### 1.2 v7.5 修复生效的正面证据

| 核对项 | 实测 | 判读 |
| --- | --- | --- |
| `GUARDIA`（四守卫命中） | **0** | 守卫一次都没被触发（`*(0x82DBB608)` 未再以空值进入完成链） |
| `cola-readonly` / `release` / `display-agent` | **0 / 0 / 0** | 两个写会计分支确实只读、不再写 guest 内存 |
| `display-tick` / `callthrough` | **0 / 0** | T25 两候选未被调用（默认 false 生效） |
| `rex_stderr(64).log` | 无崩溃、无访问违例 | 「前端启动即崩」「Sphaira 无限黑屏无加载」两症状消失 |
| `cola288` 回绕 | 不再出现 `4294967295` | v7.5 根因一（门热路盲写回绕）已彻底消失 |

唯一一条 `COLA` 行（001 与 002 各一条，完全相同）：

```
[renut] T2DIAG COLA idx=0 cnt=0/0/0 obj256=0/0/0 284=0 288=1 e24=0 e28=1 st0=0 dctx=0x82e55e20 rel=0 ag=0 tf=0
```

### 1.3 残余卡点：0 fps 在「排空循环自身」

- **PUERTA**（门探针，`cnt=0/0/0` 全程 0、`tcnt=0`）：`n=1 → n=36,000,000`（001）/ `n=1 → n=28,000,000`（002），`cola284=0` 全程不变，`cola288` 在 `n=1` 时为 0、其后恒为 1。
- **YIELD**（让出直方图）：`total=35,439,751`，`top=[82881DE8x35,439,743] [82881918x6]`（001）；`total=29,284,354`，`top=[82881DE8x29,284,346] [82881918x6]`（002）。自旋点唯一——门 `sub_82881B88` 内那条 yield，`lr=0x82881DE8`。
- **PERRO2**（泵/排空看门狗）末次：`dtick=7 ddrain=4/11 dpump=35,434,483/0/35,434,481 ddeliv=6 dcmd=0/0 dprod=6`（001）；`dpump=29,263,865/0/29,263,863`（002）。含义：排空被进入 11 次、每次在内部重旋 300 多万次；**泵进入 3543 万次、成功 0 次、交付 0 次**。
- **PERRO**（显示会计全量）末次：`vblank16548=5 tick16552=317,558,169 sub16560=5 fr16564=5 disp16568=5 ringR16700=0 ringW16704=0 v23376=0`——与 v7.4 的「推进到 5 后冻结」一致。
- **rex_perfil**：`juego 0.0 fps`（0.0 s 与 10.0 s 两次采样均为 0.0）；`Game Master (F8` 线程 CPU **98.3% / 98.8%**（整机 115% → 106%，单线程跑满）；`relojes: CPU 1020.0 MHz, GPU 460.8 MHz, memoria 1600.0 MHz` ⇒ **本次运行确认未超频**（原厂手持档），SoC 41.3/41.4 °C。
- **音频层**：「有声」得到证实：`audio: 3760 bloques de cliente mezclados`、`316/470 buffers audout con PCM no nulo`（10.0 s 采样）。
- `rex_stderr(64).log` 第 9 行：`[apm] PUESTA la configuracion 0x92220008 …: GPU 460.8 MHz (era 307.2) y la memoria se queda en 1600.0 MHz` —— 性能档为原厂手持档，无超频。

> 注：本移植的 banner 仍是旧的 `v6.27-MAPFIX`，**不可用作版本判据**；本版起版本判据是日志里唯一一行 `[video] T2ROTF BUILD v7.6-RECON`。

---

## 2. 根因定论（T36：逐字读生成代码）

### 2.1 在飞显示项 `*(base+288)`（base = `0x82E55CE0`）的全部增减点

对全部生成代码做程序级扫描（掩码 `23776 AND 288`，即 `addi rX,rY,288` 与 `andi …,23776`），命中 10 个函数：

`sub_82881930`、`sub_82881E00`、`sub_82882628`、`sub_82B06968`、`sub_829976E8`、`sub_82882740`、`sub_82882200`、`sub_82881B88`、`sub_828828E0`、`sub_82236770`。

逐个核对后确认：**只有两个点是真正的增减点**（其余是读取/比较/其它结构上的 +288 偏移）：

| 方向 | 函数 | 位置与语义 |
| --- | --- | --- |
| `++` | 交付 `sub_82881930` | `addi r11,r3,288` 后自增——每交付一件显示项，在飞计数 +1 |
| `--` | 泵 `sub_82881E00` | 取 `*(r1+156)` 的虚表做完成回调，**回调返回后无条件自减**；`idx != -1` 时同刻 `--*(entry+28)` |

### 2.2 完整生命周期（文字链）

```
每帧 tick sub_82ABB7D0
  └─ 排空 sub_82882200(idx = *(0x82DA74BC), container = 0x82E55E20)
       while (*(base+284) + *(base+288) != 0)            ← 这就是永不为 0 的判据
           泵 sub_82881E00(container, idx, 1)
              ├─ 门 sub_82881B88：主通道判据 *(*(container+16)+272+4*slot)，
              │    idx != -1 时条目判据 *(entry+24) + *(entry+28) > 0（就绪即通过）
              ├─ 交付 sub_82881930  →  ++*(base+288)（并在条目上 ++*(entry+24) → 弹记录）
              └─ 完成回调（虚表 bctrl）→  --*(base+288)（idx != -1 时同刻 --*(entry+28)）
  └─ sub_82882568
  └─ 生产者 sub_822B5608（通道 arm / 入队：++*(base+284)、++*(entry+24)）
```

- 计数字段：`base+284` = 待交付、`base+288` = 在飞；条目表 `*(base+272) + idx*32`，条目 `+24` = 待交付、`+28` = 在飞。
- Xbox 上「一次交付被完成」由 **VSync / GPU 完成中断**兜底（ISR 族 `sub_82997AF8` / `sub_82997968` / `sub_829975F0`，在生成代码里没有任何 guest 调用者）；Switch 后端没有对应事件源。

### 2.3 为什么本移植下 `cola288` 恒为 1（本轮要回答的问题）

一次被中断/长跳切断的泵调用，会留下**「已交付但完成侧没执行完」**：

```
++*(base+288) 已经执行（交付成功）   →  base+288 = 1
--*(base+288) 没有执行（完成回调未回来） →  悬挂
  同时条目上 ++*(entry+24) 也已被交付弹掉、而 entry+28 未清 → e24 = 0、e28 = 1
```

实机读数 `284=0 / 288=1 / e24=0 / e28=1` 正是这条悬挂。而门的条目就绪判据是

```
*(entry+24) + *(entry+28) > 0        ← 被这条残留自身满足（e28=1）
```

于是形成死循环：**门每轮报「就绪」→ 交付每轮弹不到记录（此时 284 与 entry+24 都是 0）→ 泵报成功 0 次 → `284+288` 始终等于 1 → 排空无限重旋**。量化为：排空进入 11 次、每次在门内 yield 300 多万次（合计 3543 万次），帧循环完全推进不下去 ⇒ **0 fps、第一个 logo 之后有声无画**。

> 这也解释了 v7.5 为何只能「止血不治病」：守卫挡掉了空显示对象的崩溃，但没有恢复完成侧会计的配对，帧循环仍被这条悬挂锁死。

---

## 3. 修复（T37 / v7.6-RECON）

### 3.1 代码位置与改动清单

| 项 | 位置 | 说明 |
| --- | --- | --- |
| 源文件 | `recomp/src/t2rotf_video_nativo.cpp` | 67,806 → **72,255 字节**，mtime 2026-10-29 00:48:50 |
| 源码级回退件 | `recomp/src/t2rotf_video_nativo.cpp.bak.t37` | 67,806 字节 = v7.5 源；随包交付 |
| 新 cvar | 第 **1277** 行 | `REXCVAR_DEFINE_BOOL(sa2_renut_display_reconcile, true, "SA2/Renderer", …)`，`lifecycle(kInitOnly)` |
| 配平块 | 第 **1316–1344** 行 | 在既有 `REX_HOOK_RAW(sub_82882200)`（第 1290–1347 行）内、调用 `__imp__sub_82882200` **之前** |
| 版本标记 | 第 **1296** 行 | 每进程一次：`[video] T2ROTF BUILD v7.6-RECON (display-completion reconcile at the drain entry)` |
| 诊断行 | 第 1337–1343 行 | `T2DIAG RECON`（实际配平时限频：前 24 次 + 每 256 次）、`T2DIAG DRAIN-IN`（未触发时限频：前 24 次 + 每 8192 次） |

选在 `t2rotf_video_nativo.cpp` 的既有排空钩子里落地，是因为该文件已有 `REX_HOOK_RAW(sub_82882200)` 与全部 `REX_EXTERN`，而一个 guest 入口只能有一个定义（同地址在两个 TU 各写 `REX_FUNC` 会重复符号）。

### 3.2 修复语义（六条边界，逐条对应上面的根因）

1. **只在排空入口**：`sub_82882200` 进入前，即 guest 每帧 1 次；绝不落在门/泵热路上（v7.5 的教训：门热路可达 10 万次/秒）。
2. **只在悬挂态触发**：`e24 == 0 && e28 > 0`（无待交付、而在飞有残留），正常帧（`e24/e28` 都非 0）完全不碰。
3. **配对回退**：先 `--*(base+288)`，再按**同一次数** `--*(entry+28)`，与 guest 自己完成侧的动作一致。
4. **绝不回负**：两个循环各带 `n > 0` 守卫，逐次递减并回写。
5. **单帧上限 64**：`k = min(e28, 64)`，避免异常态下大规模改写。
6. **不碰待交付侧**：`*(base+284)` 与 `*(entry+24)` 一字不改（不给通道环留悬空记录，也不动生产者语义）。

关闭后 = 完全交还 guest，会退回「0 fps 空转」这一原始态（仅供对照，用于确认本修复确实是解药）。

### 3.3 开关与回退方式（本版共六个键）

| 键 | 默认 | 作用 | 关闭方式 |
| --- | --- | --- | --- |
| `sa2_renut_display_reconcile` | **true**（新增，开关 D） | 排空入口的完成侧配平（本版修复本体） | 置 `false` / `SA2_RENUT_DISPLAY_RECONCILE=0` |
| `sa2_renut_display_guard` | **true**（v7.5） | `*(0x82DBB608)==0` 时短路完成链四个 guest 函数 | 置 `false`（会重现启动即崩，仅供对照） |
| `sa2_renut_display_release` | false（v7.4，只读） | 宿主侧完成释放（已弃用，只读观察） | 置 `true` 仅得观察日志 |
| `sa2_renut_display_agent` | false（v7.3，只读） | 陈旧残留抽干（已弃用，只读观察） | 置 `true` 仅得 `cola-readonly` 日志 |
| `sa2_renut_display_tick` | false | 宿主代客跑 guest 排空（已证伪） | 对照用 |
| `sa2_renut_swap_callthrough` | false | 放跑 guest 交换体（已证伪） | 对照用 |

- 全部键都是 `kInitOnly`，**改完必须完全退出再启动游戏**。
- 环境变量等价写法：`REX_SA2_RENUT_DISPLAY_RECONCILE=0`（其余同理，前缀 `REX_` + 键名大写）。
- 源码级回退：用包内 `t2rotf_video_nativo.cpp.bak.t37` 覆盖同名源文件后重编。

### 3.4 构建与静态取证

- 构建命令：`cd recomp/out/sw8 && ninja -j4 T2ROTF` ⇒ **退出码 0**（51 步；codegen「0 written, 0 unchanged, 0 deleted, 1 module(s) up to date」0.8 s）。
- 产物：`recomp/out/sw8/T2ROTF.nro` = **59,855,602 字节**，sha256 `421923cee8c53a66be9b6e0c268d5576f68d139c6e59be81ae7b7156a42b10bb`（上一版 v7.5/T31 = 59,847,410 字节 / `3728fd7e…`）。
  > 本工程产物内嵌构建时间戳：**同一份源码重构建后体积相同、哈希会变**；文档与包内哈希一律以随包 NRO 的实算值为准。
- NRO 字符串取证（字节计数）：`v7.6-RECON` = 1、`sa2_renut_display_reconcile` = 4、`T2DIAG RECON` = 1、`T2DIAG DRAIN-IN` = 1；旧标签 `sa2_renut_display_guard` = 4、`T2DIAG GUARDIA` = 1、`T2DIAG PUERTA` = 1、`T2DIAG PERRO2` = 1、`T2DIAG COLA` = 1 全部仍在。
- 配置读取路径不变：`rex_app.cpp` 里 `rex::cvar::LoadConfig(exe_dir / (GetName()+".toml"))`，应用名 `T2ROTF`。

---

## 4. 实机验证步骤（务必不超频）

1. 把新的 `T2ROTF.nro` 与 `T2ROTF.toml` 覆盖到 `sdmc:/switch/Transformers2ROTF/`（与 NRO 同目录、文件名必须为 `T2ROTF`），**完全退出再启动**，**不要超频**。
2. **先确认版本**：日志里应出现且只出现一行
   `[video] T2ROTF BUILD v7.6-RECON (display-completion reconcile at the drain entry)`。
   （应用自身 banner 仍是旧 `v6.27-MAPFIX`，不要用它判版本。）
3. **期待的正确吃法**（本轮验收点）：
   - 第一次进排空即出现 `[video] T2DIAG RECON nr=1 … retro=…` ⇒ 配平确实被触发（这是修复生效，不是新故障）；
   - `PERRO2` 的 `ddeliv` / `dprod` 开始增长、`dpump` 的中间位（成功数）脱离 0；
   - `YIELD` 总量**断崖式下降**、且不再集中在 `82881DE8`；
   - `rex_perfil` 的 `juego` fps **> 0**，`Game Master` 线程 CPU 不再长期顶在 ~98%（帧循环开始真正推进）。
4. **需回传的日志行**（缺一不可）：
   - `[video] T2ROTF BUILD v7.6-RECON`（版本判据）
   - `[video] T2DIAG RECON …`（前若干条即可）
   - `[video] T2DIAG PERRO2 …`（末条）
   - `[video] T2DIAG PUERTA …`（首条与末条）
   - `[video] T2DIAG YIELD …`（末条）
   - `[renut] T2DIAG COLA …`（若有）
   - `rex_perfil.log` 的 `juego … fps` 行与线程 CPU 列表
   - `rex_crash.log`（若产生）
5. **逐项判读表**：

| 现象 | 含义 | 下一步 |
| --- | --- | --- |
| 有 `RECON nr=1`，`ddeliv`/`dprod` 开始增长，fps > 0 | 修复方向正确，帧循环已解锁 | 接着做 T26：不超频实测帧率、定位下一瓶颈 |
| `RECON` 每帧持续触发（`retro` 持续增长、`nr` 快速变大） | 完成侧在 SW 侧每次都不闭环，只能靠配平强推 | 需要实现真实的完成事件源（VSync/GPU 完成中断等价物），配平缩回兜底 |
| 无 `RECON` 行（只有 `DRAIN-IN`）且仍 0 fps | 悬挂态不是 `e28=1` 这一型（例如 idx 为 -1 或条目表未挂） | 用 `DRAIN-IN` 的 `idx/c24/c28/e24/e28` 逐字段重定位 |
| `RECON` 已触发但 `YIELD` 仍集中在 `82881DE8` | 卡点已不在排空判据，而在门/交付/回调内部 | 回到门两段判据与显示命令回调链（`sub_82881838`）取证 |
| 出现 `GUARDIA … onj=0` | `*(0x82DBB608)` 仍为 0，守卫在短路完成链 | 属预期（v7.5 语义），随日志一并回传 |

---

## 5. 残余风险与未达标项

1. **本版尚未实机验证**：以上全部结论来自日志判读 + 生成代码逐字取证 + 静态构建取证，实机行为待第 4 节步骤确认。
2. **「不超频流畅 60 帧」仍未达成、也未实测**：本版只解锁帧循环，帧率本身需在实机实测。参考上限（CPU 1785 MHz / GPU 921 MHz / 内存 1600 MHz）仅作对照，**不用于达标**。
3. **正解仍是「补显示完成事件源」**：本配平是让被中断的悬挂不再毒死帧循环的最小手段，不等于给后端补上了真实的 VSync/GPU 完成中断等价物；若实机出现「每帧都触发 RECON」，说明完成链在 SW 侧根本不闭环，必须转向实现完成事件源。
4. **显示对象单元 `0x82DBB608` 的创建者仍未定论**（v7.5 遗留）：生成代码里找不到独立初始化路径，只有四个写者。若实机守卫持续命中（`onj=0`），说明该单元在本移植里长期为空，完成链本身需要另外补对象来源。
5. **影片（WMV/VC-1）解码链**与「第一个 logo 之后立即黑」的时序关系尚未单独复核：本版修复的是**显示呈现门**，若实机解禁后画面仍黑，需要转为影片链取证。
6. **两处地址口径差异提醒**：guest 基址随每次启动变化（本轮 `rex_stderr(64).log` 报 `base=0x5a68e5a000`；上一轮崩溃日志为 `0x63a5422000`）。报告中的 `base+284/288`、`entry+24/+28`、`0x82DBB608`、`0x82E55CE0/0x82E55E20` 都是**guest 虚拟地址（编译期固定）**，不受基址变化影响；而 `far` 一类是「基址 + 偏移」，必须用当次 `rex_stderr` 的 base 换算。

---

## 6. 本轮交付物

| 文件 | 内容 | 关键数字 |
| --- | --- | --- |
| `T2ROTF_switch_port.zip` | 工程包（原地更新） | 67 条目；含新 NRO、新 toml、新 README、本报告与 `t2rotf_video_nativo.cpp.bak.t37` |
| `recomp/out/sw8/T2ROTF.nro` | 可执行产物（单独取出） | 59,855,602 字节，sha256 `421923cee8c53a66be9b6e0c268d5576f68d139c6e59be81ae7b7156a42b10bb` |
| `T2ROTF.toml` | SD 卡配置（单独取出） | 9,178 字节 sha256 `b4fae20fcbbded74bd3332ced404e19a43b46ce3e727e69cce612895e9cbe232`；六个键：`guard=true`、`reconcile=true`、`release=false`、`agent=false`、`tick=false`、`callthrough=false` |
| 本报告 | 定论 / 修复 / 验证步骤 | 本文件 |

**SD 卡布局**：`sdmc:/switch/Transformers2ROTF/{T2ROTF.nro, T2ROTF.toml, game/, update/, patched/, dlc/}`。

---

## 附：本轮修改的文件与备份

| 文件 | 改前 | 改后 | 备份 |
| --- | --- | --- | --- |
| `recomp/src/t2rotf_video_nativo.cpp` | 67,806（v7.5） | 72,255 | `recomp/src/t2rotf_video_nativo.cpp.bak.t37`（= v7.5 源，随包） |
| `T2ROTF.toml` | 6,990（v7.5，五键） | 见第 6 节（六键） | 包内旧版为 v7.5 文本 |
| `README.md` | 80,303（v7.5） | 同步 v7.6/T38 节与更新记录 | — |
| `recomp/src/renut_engine/gpu/d3d/hooks/device.cpp` | 103,185（v7.5/T31） | **本轮未改**（仍 103,185） | 包内 `renut_device.cpp.bak.t29`（= v7.4 基线） |