# T2ROTF · 显示队列「完成侧」代理与 kDisplayCtx 修正报告（T28）

- 日期：2026-10-28
- 工程：`J:\XBOX360 port\Transformers Revenge of the Fallen`
- 产物：`recomp/out/sw8/T2ROTF.nro`，59,843,314 字节，sha256 `cd806cc16affdc60172fe3c9c5c0f75499920bbb560133779342588cbfd4e876`
- 上一版（T25 / v7.2）：59,831,026 字节，sha256 `e5797164154f406dec318226109927e8bbf1a7a50a511bb39559c01d0e73a30e`
- 改动文件：`recomp/src/renut_engine/gpu/d3d/hooks/device.cpp`（备份 `device.cpp.bak.t28`，80,681 字节）
- 配置：`T2ROTF.toml`（新增第三个开关，默认开）

---

## 一、结论摘要

实机反馈「两个开关全开或者全关都是有声无画」已定论，**两条候选各自无效的直接原因不同，而且都不是「修复方向不对」层面的问题，是各自挂在了死路径 / 读错了地址**：

| T25 候选 | 开关 | 无效的直接原因 | 级别 |
| --- | --- | --- | --- |
| 候选一：Swap 钩子里补跑 guest 交换体 `sub_822D9D70` | `sa2_renut_swap_callthrough` | 空转**不在 Swap 路径上**。实机日志 `[renut] T2DIAG SWAP` 全程只有 `n=1` 一行 ⇒ 卡死之后 Swap 根本不再发生（每帧 tick 才是卡住的地方：`sub_82ABB7D0 → sub_82882200`）。挂 Swap 钩子无论如何都驱动不了它。 | 挂错钩子点 |
| 候选二：宿主代客派发 `sub_82881E00(0x82E55EE0, -1, 1)` | `sa2_renut_display_tick` | `container` 记错 0xC0：真正的 container 是 **`0x82E55E20`**，不是 `0x82E55EE0`（取证见 §三.2）。门读到 `*(container+16)` 是垃圾，立即返回 0，等于什么都没做 ⇒ 开与不开现象完全一样。 | 地址错 |

**真正的阻塞点**（本轮修复对象）：呈现队列的**完成侧**没有来源。实机探针在门进不去的那一刻读到

```
cnt=0/0/0   obj256=0/0/0   cola284=0   cola288=1   st0=0
```

即：六条通道标记全 0（**没有任何在飞条目**）、`base+284 = 0`，但 `base+288 = 1`——残留着一份「已交付、未完成」的会计。而 `base+284/base+288` 恰恰就是游戏每帧 tick 的排空循环判据：

```c
// sub_82882200（生成代码 39.cpp:92240 起，被每帧 tick sub_82ABB7D0 调用）
while (*(base + 284) + *(base + 288) != 0)   // idx == -1 分支
    sub_82881E00(container, -1, 1);          // 单趟有界排空
```

这份会计唯一的消除途径是**门成功**（交付 → 回调 → `base+288--`），而门成功要求通道标记非 0；标记的消费者在 Xbox 上是 GPU 垂直同步侧（ISR `sub_82997AF8` 全库没有任何 guest 调用者），Switch 后端没有对应来源。于是：**残留永远回不到 0 ⇒ 每帧 tick 死等第一帧 ⇒ 0.0 fps、第一个 logo 之后全黑屏**。

**本轮修复**：把宿主明确地放到「显示 / 垂直同步侧」的位置上——宿主在 Swap 钩子与呈现门钩子两处，对「六通道确实全 0 却仍留着会计残留」的陈旧状态做**保守抽干**（连续多次观测到同一组数值才动手，一次只退一份，绝不回退到负值；只要有一个通道被 arm 就立刻停手，完全交还 guest 自己的流水线），并修正 `kDisplayCtx`。

---

## 二、交付内容

1. 新 `T2ROTF.nro`（sha256 `cd806cc1…`），已用字符串取证确认下列符号进产物：

```
T2DIAG display-agent             : 1
T2DIAG COLA                      : 1
sa2_renut_display_agent          : 2
T2DIAG callthrough               : 1
T2DIAG display-tick              : 1
```

2. 新 `T2ROTF.toml`：新增 `sa2_renut_display_agent = true`（默认开，`kInitOnly`，改了要重启）；T25 的两个开关保留但置 `false`（已证伪，留作对照手段）。
3. 本报告。

---

## 三、证据链

### 1. 实机日志（2026-10-28 00:45，T25 版 NRO）

`T2ROTF_001(60).log`：

```
75:  [video] T2DIAG PUERTA n=1 r30=41770018 arg=FFFFFFFF st0=0 s16=00000000 cnt=0/0/0 v288=0
              cola0=82E55CE0 cola272=40F58F80 cola284=0 cola288=0 obj256=0/0/0 obj272=40F58F80 tb=0 tmin=773893 tmax=793843 tcnt=0
88:  [video] T2DIAG PUERTA n=2 r30=82E55E20 arg=00000000 st0=0 s16=82E55E20 cnt=0/0/0 v288=0
              cola0=82E55CE0 cola272=40F58F80 cola284=0 cola288=1 obj256=0/0/0 obj272=40F58F80 tb=0 tmin=773893 tmax=793843 tcnt=0
95:  [renut] T2DIAG SWAP n=1 dev=0x40070600 front=0x82e55fc4 sub16560=1 disp16568=0 fr16564=0 f10941=0x00
104: [video] T2DIAG PUERTA n=7 ... cola284=1 cola288=0 ...
105: [video] T2DIAG PUERTA n=8 ... cola284=0 cola288=1 ...
```

读法：

- `cnt=0/0/0`、`obj256=0/0/0`：门的两套通道标记长期全 0 ⇒ 队列里没有任何在飞条目，门永远不就绪。
- `cola284=0` 而 `cola288=1`：残留的只有「已交付未完成」这一份。
- `n=7 → n=8` 之间 `(284,288)` 从 `(1,0)` 变成 `(0,1)`：说明确实有「投递 → 交付」的成对动作在发生（`sub_82881930` 交付：`base+284--`、`base+288++`），但**交付之后没有任何人把它完成**。
- `[renut] T2DIAG SWAP n=1` 之后再无第二行：Swap 已经不再被调用（这也直接判了候选一的死刑）。
- 门侧等待是正常的：`T2DIAG espera sub_824F3580 n=6 r3=F8000060 r4=1 r5=0 lr=828818DC`，`lr=0x828818DC` 落在门的等待助手 `sub_828818xx` 内，不是别的自旋点。

`rex_stderr(62).log`：`==== v6.27-MAPFIX … ====`、`[DIAG] ReXApp::OnInitialize() entered` … `LaunchModule done` ⇒ 启动链路正常，问题纯在帧循环。

### 2. `kDisplayCtx` 的取证（0x82E55EE0 为什么是错的）

`recomp/generated/default/T2ROTF_recomp.39.cpp`（`sub_82882200`，每帧 tick 的排空者）里逐字读出：

```asm
lis  r11,-32027
addi r28,r11,24096      ; -32027<<16 + 24096 = 0x82E55E20
mr   r3,r28             ; container
mr   r4,r29             ; idx
li   r5,1               ; flag = 单趟有界
bl   0x82881e00         ; 门 + 排空
```

⇒ **container = `0x82E55E20`**。T25 记的 `0x82E55EE0` 与之差 `0xC0`，正好落在另一个对象里，所以候选二读到的是垃圾。

### 3. 门 / 泵 / 排空者 / tick 的判据（生成代码逐字重建）

- 门 `sub_82881B88`（`generated/default/T2ROTF_recomp.4.cpp:94783` 起）：
  - `idx != -1`：先扫 `*(base+272) + idx*32` 条目的子条目，任一 `+24/+28` 之和 > 0（有在飞）即直奔 `loc_82881DE8` → `bl 0x824F0620`（yield）返回 0；
  - 三槽循环：主判据标记 `*(*(container+16)+272+4*slot)`，后备判据 `*(base+256+4*slot)`；slot1 还要 `*(container+0) >= 2` 且 `d = REX_QUERY_TIMEBASE() - *(0x82DBB688)` 落在 `(*(0x82E55F48), *(0x82E55F50))` 时间窗内；slot2 还要 `*(container+0) != 5` 且 `*(container+288) + *(0x82DBB5C4) == 0`；
  - 就绪 → `sub_82881930(container+24, slot, idx, out1)`：标记--、`base+284--`、条目 `+24--`、条目 `+28++`、`base+288++`（交付给显示侧）；
  - 全不就绪 → `loc_82881DE8` → yield 返回 0。
- 泵 `sub_82881E00`（`T2ROTF_recomp.21.cpp:94142`）：门成功 → 跑回调（函数指针在 `*(*(r1+156))`，参数 `r1+96`）→ `base+288` 原子递减 → `idx out != -1` 时条目 `+28` 原子递减 → 跳回门；`flag=1` 单趟有界。
- 排空者 `sub_82882200`（`39.cpp:92240`）：`idx == -1` 时循环判据 `*(base+284)+*(base+288) != 0`，否则 `*(表+idx*32+24)+(+28) != 0`。
- 每帧 tick `sub_82ABB7D0`：`lwz r3,29884(0x82DA0000)` = `*(0x82DA74BC)`（开机 `sub_82ABB6D8` 经 `sub_82882490` 分配后写一次）→ `bl 0x82882200`。
- 生产者 `sub_822B5608`：`条目+24++`、`base+284++`（投递），与交付/完成自洽 ⇒ **不是「漏减」型死锁，而是「残留的完成会计无人消除」型死锁**。

---

## 四、修复实现（`device.cpp`）

### 1. `kDisplayCtx` 修正

```cpp
constexpr u32 kDisplayCtx = 0x82E55E20u;   // 门 sub_82881B88 的 container（r30）
// 取证：sub_82882200 的 addi r28,r11,24096 = 0x82E55E20
```

### 2. 新增显示队列「完成侧」代理（`sa2_renut_display_agent`，默认开）

新增常量与助手：

```cpp
constexpr u32 kDisplayQueueBase    = 0x82E55CE0u;  // 呈现队列全局（自指针 *(base)==base）
constexpr u32 kDisplayEntryIdxVa   = 0x82DA74BCu;  // 显示条目号（开机写一次）
constexpr u32 kOffQueueTable       = 272u;         // 条目表指针
constexpr u32 kOffQueuePosted      = 284u;         // 已投递未交付
constexpr u32 kOffQueueInflight    = 288u;         // 已交付未完成
constexpr u32 kOffCtxObject        = 16u;          // container+16 → 通道标记所在对象
constexpr u32 kOffObjectMarkers    = 272u;         // 该对象 +272/276/280
constexpr u32 kOffManagerMarkers   = 256u;         // base+256/260/264
```

核心规则（`DisplayAgentStep(origen)`，`origen` 0 = Swap、1 = 门）——**只有同时满足三条才动手**：

1. 六条通道标记（object 侧 `+272/276/280` 与容器侧 `base+256/260/264`）**全部为 0** ⇒ 这台机子上没有任何「正在排入 / 正在交付」的条目；
2. 四个会计字（`base+284`、`base+288`、条目 `+24`、条目 `+28`）至少一个非 0；
3. **连续 N 次观测到完全同一组数值**（指纹 = 四个字按位拼）——真实投递只跨几条指令，它只要动过任何一个字，指纹就变、计数从头再来，所以正常流水线不会被误清。N = 4（Swap 调用点）/ 32（门调用点）。

满足后把每个非 0 的字各退 1（**绝不回退到负**），一次只退一份，慢慢把陈旧残留抽干；一旦某个通道被 arm，立刻停手，完全交还 guest。

调用点两处：

```cpp
// Swap 钩子内（D3DDevice_Swap_hook，QueueFrame 之前）
if (REXCVAR_GET(sa2_renut_display_agent))
  DisplayAgentStep(0);

// 门钩子（新增，主战场：实机卡死态门被高频调用，而 Swap 已不再发生）
BANJO_RENUT_HOOK_RAW(sub_82881B88) {
  if (REXCVAR_GET(sa2_renut_display_agent)) {
    const u32 base = kDisplayQueueBase;
    if (LoadGuestU32(base) == base) {
      const u32 entry = DisplayQueueEntry(LoadGuestU32(kDisplayEntryIdxVa));
      const u32 posted = LoadGuestU32(base + kOffQueuePosted);
      const u32 entry_posted = entry ? LoadGuestU32(entry + 24u) : 0u;
      if (posted || entry_posted)
        DisplayAgentStep(1);
    }
  }
  __imp__sub_82881B88(ctx, base);     // 其余情况控制流一个字节都不改
}
```

`sub_82881B88` 的覆盖是有效的：该地址在生成代码中以 `DEFINE_REX_FUNC(sub_82881B88)`（弱定义）形式存在，`BANJO_RENUT_HOOK_RAW` 展开出的强定义 `extern "C" REX_FUNC(sub_82881B88)` 会覆盖它，与我们已实测生效的 `D3DDevice_Swap_hook`（`sub_822D9D70` 覆盖，日志 `T2DIAG SWAP n=1` 为证）是同一套机制；`__imp__sub_82881B88` 指向原体，不会自递归（构建链接通过，无未解析符号）。

### 3. 新增诊断（判修复是否生效的直接证据）

- `[renut] T2DIAG display-agent n=… src=swap|gate idx=… cnt=…/…/… obj256=…/…/… cola284=… cola288=… e24=… e28=… writes=…`（限 24 条）
- `[renut] T2DIAG COLA idx=… cnt=…/…/… obj256=…/…/… 284=… 288=… e24=… e28=… st0=… dctx=0x82E55E20`（每 2 s 一行）

---

## 五、实机验证清单

1. 覆盖 SD 卡：`sdmc:/switch/Transformers2ROTF/T2ROTF.nro`、`T2ROTF.toml`（须与 NRO 同目录、文件名必须是 `T2ROTF`）。
2. **完全退出再启动**游戏（三个开关都是 `kInitOnly`，改了必须重启）。
3. 期待：第一个 logo 之后画面出现并持续（不再是「有声无画」）。
4. 日志要看四类行（`sdmc:/switch/Transformers2ROTF/logs/rex/`）：
   - `[renut] T2DIAG display-agent … writes=1|2` ⇒ 代理确实抽干过残留（**这是修复生效的第一证据**）；
   - `[renut] T2DIAG COLA … 284=0 288=0 e24=0 e28=0` 长期保持 ⇒ 帧循环归零、恢复流动；
   - `T2DIAG SWAP n=1,2,3…` 持续增长 ⇒ Swap 又活了（T25 版只有 `n=1`）；
   - `juego … fps` 不再 0.0。
5. 若仍是「有声无画」：把 `rex_perfil`、`rex_stderr`、`T2ROTF_001`、`T2ROTF_002` 四份日志整组发回。日志里 `display-agent` 有没有打、`COLA` 的 `284/288/e24/e28` 有没有归零，会直接指出下一步：是「残留清了但没人推进」（缺显示推进源）还是「清了又被重新建起来」（交付/完成配对仍然缺一半）。
6. 回退：`sa2_renut_display_agent = false` 即回到 T25 行为（已知黑屏）。

---

## 六、未决项与风险（如实记录）

1. `e24`/`e28`（条目级 `+24/+28`）在 T25 版探针里**没有单独打印过**，只有 `cola284/cola288`。本轮补丁对两族都处理，但要靠新 `T2DIAG COLA`/`display-agent` 行确认到底哪一侧非 0。
2. `*(0x82DA74BC)`（条目号）目前只确认开机写一次；是否每帧重新发布未定 ⇒ 若 `idx` 会变，条目级 `+24/+28` 的残留可能落在别的条目上（本轮代理只处理当前 `idx` 对应条目）。
3. 时间窗全局 `0x82DBB688`（u64 基）与 `0x82E55F48/0x82E55F50` 的写者未查。若宿主不推进时基，门 slot1 的时间窗判据可能永假（本轮修复不依赖这条路径，但它是「门为什么永远不就绪」的第二可能来源）。
4. `sub_82997AF8`（ISR：BC → 排空 → BE）全库没有 guest 调用者；`sub_829976E8` 的等待由它满足。**下一步的候选修复**是让宿主在每帧起点代客调一次这个 ISR（相当于补上 GPU 完成中断），本轮未做——它比「抽干残留」更贴近硬件语义，但风险也更大（ISR 内部有等待），需要先靠本轮日志确认「抽干残留」这一步是否奏效。
5. **60 帧达标仍未实测**（T26）：本轮只解决了「画面出不来」。参考项目在超频（CPU 1785 / GPU 921 / 内存 1600 MHz）下菜单 55–60 FPS、游戏内 40–52 FPS；用户要求**不超频**流畅 60 帧，必须等画面恢复后按 T26 实测菜单与游戏内帧率、GPU/CPU 占用与瓶颈项，再决定是否需要后端优化。
6. TU（`..\TFROTF TU`）与 DLC（`..\Transformers 2 - Maps and Characters Pack`）的加载路径本轮未改动，沿用既有布局（`sdmc:/switch/Transformers2ROTF/` 下 `game/`、`update/`、`patched/`、`dlc/`）。

---

## 七、改动清单（可核对）

| 文件 | 改动 |
| --- | --- |
| `recomp/src/renut_engine/gpu/d3d/hooks/device.cpp` | `kDisplayCtx` 0x82E55EE0 → 0x82E55E20；新增 `REX_EXTERN(__imp__sub_82881B88)`；新增显示队列常量与 `StoreGuestU32`/`DisplayQueueEntry`/`DisplayMarkerSnapshot`/`DisplayAgentStep`；新增 `sa2_renut_display_agent`（默认 true，kInitOnly）；Swap 钩子内调用点；新增门覆盖钩子 `BANJO_RENUT_HOOK_RAW(sub_82881B88)`；新增 `T2DIAG COLA` 稳态探针。 |
| `T2ROTF.toml` | 新增 `sa2_renut_display_agent = true`；两个已证伪开关置 false 并补齐原因注释；修正候选二注释里 `0x82E55EE0` → `0x82E55E20`。 |
| `recomp/out/sw8/T2ROTF.nro` | 重建（sha256 `cd806cc1…`）。 |
| 备份 | `device.cpp.bak.t28`（T25 基线，80,681 字节）。 |