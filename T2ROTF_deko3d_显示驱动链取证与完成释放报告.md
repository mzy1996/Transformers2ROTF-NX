# T2ROTF（Xbox 360《变形金刚2：卷土重来》）Switch 移植 —— T29 报告
## 显示驱动链取证 与 宿主侧「显示完成释放」落地

- 日期：2026-10-28
- 适用构建：`recomp/out/sw8/T2ROTF.nro`，59,847,410 字节，sha256 `7186022a4681553d1c75d5f26c1ea1197fc4d8ddff44db5de16a2c284558ac5a`（见 §4 说明：工程内嵌构建时间戳，任何一次重新构建都会改变该哈希，以交付包内文件为准）
- 路线：rexglue-sdk（ReXGlue）+ XenosRecomp 静态重编译，渲染后端 deko3d / Renut（Vulkan/NVK 与 Mesa 路径已废弃，本报告不涉及）
- 工具链：devkitPro + libnx，`cd recomp/out/sw8 && ninja -j4 T2ROTF`
- 本报告回答两件事：①呈现驱动链在实机上究竟跑到哪一步（取证）；②宿主补哪一半才能让帧循环活过来（修复）

---

## 1. 本轮结论（先看这一段）

1. **T28 的「显示完成侧代理」在实机上一次都没有被叫到**，所以它的 v637 构建不可能修好 0 fps。
   原因不是逻辑错，而是它挂在门钩子里的**廉价前置判断只看了一对字段**：`base+284`（已投递）与
   条目 `+24`。而实机卡死态的残留恰好在**另一对字段**上：`base+284 = 0`、条目 `+24 = 0`、
   `base+288 = 1`（已交付未完成）。判断为假 ⇒ 代理不跑 ⇒ 排空 `while` 照样非 0 ⇒ 每帧空转。

2. 于是根因可以定量地重述为：**Switch 后端没有「显示完成」事件源**。Xbox 上把
   `base+288 / 条目+28` 减回去的是 GPU 垂直同步/完成中断（ISR 族 `sub_82997AF8` /
   `sub_82997968` / `sub_829975F0`），这三个函数在生成代码里**没有任何 guest 调用者**；
   实机 `irq47=0 / irq95=0`（guest GPU 中断回调 0 次）与之吻合。

3. T29 的修复 = **宿主扮演这台机子的显示完成侧**，并且只补那一半：
   - 新增开关 `sa2_renut_display_release`（默认开）：把「已交付给显示侧、无完成事件回收」
     的那一份退掉（`base+288` 与条目 `+28` 各 −1，绝不退到负数）；
   - 修正门钩子的前置判断为**四个会计字一起看**（`+284 / +288 / 条目+24 / 条目+28`）；
   - 门侧门槛由「连续 32 次」降到「连续 8 次同一指纹」（门在卡死态被以 4,300,000 次/秒量级调用）。
   - 修复**不采用**「把六通道标记置 1 强行 arm」的做法：那只会让门就绪而无真实条目，
    交付/回调的会计（`base+284/+288`、条目 `+24/+28`）不对账且标记不会自然递减，
    结果是每帧空交付/重复交付。

4. 本轮同时落下一套**只读**显示链探针（`dtick / ddrain(sucio) / dpump(in,ok,out) / ddeliv /
   dcmd(in,out) / dprod`），下一轮实机日志可以直接判定「泵被反复进入却从未交付」还是
   「泵交付了但回调阻塞」——这是当前唯一还缺的一环证据。

5. 仍未完成（不要让报告掩盖它）：**尚未实机验证**本轮构建；用户手上前一轮日志属 **T25 构建**
   （无 `T2DIAG COLA` / `display-agent` / `release` 行）；**不超频 60 帧仍未实测**。

---

## 2. T28 为何在实机上没生效（决定性取证）

### 2.1 旧代码（T28，`device.cpp` 门钩子内）

```cpp
BANJO_RENUT_HOOK_RAW(sub_82881B88) {
  if (REXCVAR_GET(sa2_renut_display_agent)) {
    const u32 base = kDisplayQueueBase;
    if (LoadGuestU32(base) == base) {
      const u32 entry = DisplayQueueEntry(LoadGuestU32(kDisplayEntryIdxVa));
      const u32 posted = LoadGuestU32(base + kOffQueuePosted);      // base+284
      const u32 entry_posted = entry ? LoadGuestU32(entry + 24u) : 0u;  // 条目+24
      if (posted || entry_posted)          // ← 卡死态这两个都是 0，条件为假
        DisplayAgentStep(1);               // ← 于是这一行从未执行
    }
  }
  __imp__sub_82881B88(ctx, base);
}
```

### 2.2 实机读数（`T2ROTF_001(60).log` / `T2ROTF_002(41).log`，T25 构建）

| 探针 | 读数 | 含义 |
| --- | --- | --- |
| `T2DIAG PUERTA n=4000000` | `r30=82E55E20`、`arg=00000000` | 门以 container=0x82E55E20、idx=0 被调用（不是 -1） |
| 同上 | `cnt=0/0/0`、`obj256=0/0/0` | 六通道标记**全 0**：没有任何在飞条目 |
| 同上 | `cola284=0`、`cola288=1` | **残留恰在「已交付未完成」：1 份** |
| 同上 | `cola272=40F58F80`、`st0=0` | 条目表指针有效、container+0 状态码 0 |
| 同上 | `tcnt=0`、`tb=0`、`tmin=773893`、`tmax=793843` | slot1 的时间窗从未满足 |
| `T2DIAG PERRO` | `yield=4321707`、`irq47=0`、`irq95=0` | 唯一 yield 点被调 4.32M 次；GPU 中断回调 0 次 |
| `T2DIAG PERRO` | `dev=40070600 vblank16548=5 tick16552=241272890 … disp16568=5` | 宿主显示会计推进到 5 后冻结 |
| 游戏侧 | `juego 0.0 fps` | 帧循环从未走完第二帧 |

而门的实现（`T2ROTF_recomp.21.cpp:94142` 起，`sub_82881E00`）说得更直白：门**不就绪即退出**
（`loc_82881ED8` 返回 0），排空 `sub_82882200` 的循环判据是
`*(base+284) + *(base+288) != 0`（idx==-1）或 `*(表+idx*32+24) + (+28) != 0`（idx≥0）。
两个判据里 `base+288` / 条目 `+28` 都算数，因此这一份残留足以让循环永远转下去。

### 2.3 空转的完整因果链

```
每帧 tick sub_82ABB7D0
  └─ sub_82882200(idx=*(0x82DA74BC))            ← 排空
       while (*(base+284) + *(base+288) != 0)   ← cola288=1 ⇒ 恒真
         └─ sub_82881E00(container=0x82E55E20, idx, flag=1)
              └─ 门 sub_82881B88：三槽全不就绪（六标记全 0）
                   └─ sub_824F0620（NtYieldExecution 包装）→ 返回 0   ← lr=0x82881DE8，4.32M 次
  （循环不退出 ⇒ 永远走不到 sub_82882568 → 生产者 sub_822B5608 ⇒ 通道永不再 arm）
  （⇒ 门永不就绪 ⇒ 0 fps、第一个 logo 之后有声无画）
```

**结论**：这不是「宿主动错了会计」，而是「宿主动得太少、而且动的位置与判断的位置不一致」。

---

## 3. T29 落地改动清单

### 3.1 `recomp/src/renut_engine/gpu/d3d/hooks/device.cpp`

| # | 位置 | 改动 | 目的 |
| --- | --- | --- | --- |
| 1 | 开关区（T28 开关之后） | 新增 `REXCVAR_DEFINE_BOOL(sa2_renut_display_release, true, …)`，`Lifecycle::kInitOnly` | 显示**完成侧**释放的总开关，默认开 |
| 2 | `DisplayAgentStep()` | 动作按开关拆两半：`release` → `base+288` 与条目 `+28` 各 −1；`agent` → `base+284` 与条目 `+24` 各 −1。两半都有「非 0 才退、不回负」守卫 | 完成侧与残渣抽干互不牵连，可单独做对照 |
| 3 | `DisplayAgentStep()` | 门侧连续观测门槛 `32 → 8`（Swap 侧保持 4） | 门在卡死态被调 4.3M 次/秒，8 次同一指纹约微秒级即可动手 |
| 4 | 门钩子 `sub_82881B88` | 前置判断改为**四个会计字**（`+284 / +288 / 条目+24 / 条目+28`）任一非 0 才进代理；两开关任一开即启用 | **修掉 T28 那个把卡死态滤掉的 bug** |
| 5 | Swap 钩子 `D3DDevice_Swap_hook` | 调用条件改为两开关任一开；注释写明「正常态下宿主呈现一次 = 显示完成一次」 | 正常态覆盖；卡死态由门钩子兜底 |
| 6 | `T2DIAG COLA` 诊断行 | 增加 `rel=`（release 次数）、`ag=`（agent 次数）、`tf=`（当次 `*(0x82DA74BC)` 实际取值） | 下一轮日志可直接看完成侧是否在跑、idx 是否如假设 |
| 7 | 新增 `T2DIAG release` 限频诊断行（前 24 次 + 每 4096 次） | 打 `src=swap/gate`、`idx`、六标记、四个会计字、`writes` | 取证书面证据 |

新增/保留的行为边界（**重要**）：

- 只在「六通道标记全 0（确实没有在飞条目）」**且**「四个会计字连续同一指纹」时才动手；
  真实投递只跨几条指令，动过任一字指纹即变、计数归零，因此**不与真实投递抢会计**。
- 一次最多各退 1，**绝不退到负数**；任一通道被 arm 就立刻停手，完全交还 guest 自己的流水线。
- 不改 guest 控制流一个字节：所有改动只发生在宿主的只读读取 + 几个会计字的写入上。

### 3.2 `recomp/src/t2rotf_video_nativo.cpp`（纯附加、全部只读）

| # | 内容 | 地址 / 语义 |
| --- | --- | --- |
| 1 | 计数器 `g_dtick` | `sub_82ABB7D0` 每帧 tick 进入 |
| 2 | `g_ddrain` / `g_ddrain_sucio` | `sub_82882200` 排空进入 / 进入时判据非 0（会空转的那次） |
| 3 | `g_dpump_in` / `g_dpump_ok` / `g_dpump_out` | `sub_82881E00` 显示泵进入 / 返回非 0（真的交付了一件） / 返回 |
| 4 | `g_ddeliv` | `sub_82881930` 交付进入（标记--、`+284`--、条目`+24`--、条目`+28`++、`+288`++） |
| 5 | `g_dcmd_in` / `g_dcmd_out` | `sub_82881838` 显示命令进入/返回（`**(r3+60)` 回调，in>out 即回调阻塞） |
| 6 | `g_dprod` | `sub_822B5608` 生产者/入队进入（通道重新 arm） |
| 7 | `T2DIAG PERRO2` 日志行 | 一次打全上述 10 个数，挂在既有的看门狗里 |

改动体量：`t2rotf_video_nativo.cpp` 62,433 → 67,806 字节（+5,373，全为 3 个带 `[T29]` 标记的附加块，
可整块删除回退）；`device.cpp` 93,912 → 98,921 字节。

**预期签名（下一轮实机判读用）**：

- 修复生效：`release` 行出现且 `rel=` 增长；`COLA` 里 `284=0 288=0 e24=0 e28=0`；
  `PERRO2` 的 `dtick / ddrain / dpump / ok / ddeliv / dcmd / dprod` 全部开始增长；
  帧率不再 0。
- 仍卡死且 `dpump_in` 与 `yield` 同量级、而 `ok/ddeliv/dprod` 全 0：
  说明完成侧释放没生效（把 `T2DIAG release` 行发回）。
- 若 `ddeliv` 增长但 `dcmd_in` 长期大于 `dcmd_out`：回调阻塞，下一个战场在显示命令的虚表目标里。

---

## 4. 构建与产物取证

```
cd recomp/out/sw8 && ninja -j4 T2ROTF        → EXIT=0（无编译错误）
[1/53] Generating recompiled code for T2ROTF  …  0 written, 0 unchanged, 1 module up to date
[2/4] Building CXX object … hooks/device.cpp.obj
[3/4] Building CXX object … t2rotf_video_nativo.cpp.obj
[4/4] Linking CXX executable T2ROTF; Packaging T2ROTF.nro
```

产物：`recomp/out/sw8/T2ROTF.nro`，**59,847,410 字节**，
sha256 `7186022a4681553d1c75d5f26c1ea1197fc4d8ddff44db5de16a2c284558ac5a`。

> 说明：本工程会往产物里写入构建时间戳。**同一份源码连续两次构建，体积相同、哈希不同**
> （本轮实测：`d463c68e…` → `7186022a…`）。因此文档/包内的哈希一律以**最后那次构建**为准，
> 任何再次重构建都必须同步刷新 T2ROTF.toml 与 README 里的哈希。

产物内字符串取证（`grep -c -a`，命中次数）：

| 串 | 次数 | 串 | 次数 |
| --- | --- | --- | --- |
| `sa2_renut_display_release` | 2 | `T2DIAG release` | 1 |
| `sa2_renut_display_agent` | 2 | `T2DIAG display-agent` | 1 |
| `T2DIAG PERRO2` | 1 | `T2DIAG COLA` | 1 |
| `dpump=` | 1 | `dcmd=` | 1 |
| `dprod=` | 1 | `rel=` | 1 |
| `tf=` | 1 | `ag=` | 3 |

---

## 5. `T2ROTF.toml`（放置与默认值）

放置位置：**与 NRO 同目录**，文件名必须是 `T2ROTF.toml`
（Switch 上 `sdmc:/switch/Transformers2ROTF/T2ROTF.toml`）。本文件 5,722 字节，`tomllib` 可解析，
解析结果（四个键都在）：

```python
{'sa2_renut_display_release': True,   # T29 新增：显示完成侧释放
 'sa2_renut_display_agent':   True,   # T28：陈旧「已投递未交付」抽干
 'sa2_renut_display_tick':    False,  # T25 遗留，已证伪
 'sa2_renut_swap_callthrough':False}  # T25 遗留，已证伪
```

等价环境变量（PC/调试端）：`REX_SA2_RENUT_DISPLAY_RELEASE=1`。全部为 `kInitOnly`，**改了要重启游戏**。

---

## 6. 实机验证步骤（下一轮请按此取数）

1. 把交付包里的 `T2ROTF.nro` + `T2ROTF.toml` 拷到 `sdmc:/switch/Transformers2ROTF/`
   （保持原有 `game/` `update/` `patched/` `dlc/` 布局不动）。
2. 不修改任何开关（即：完成侧释放与残渣抽干都**开**），**不超频**启动，跑到第一个 logo 之后的黑屏处，
   等 10~20 秒，退出。
3. 回传 `sdmc:/switch/Transformers2ROTF/` 下的日志（`T2ROTF_001*.log`、`T2ROTF_002*.log`、
   `rex_stderr*.log`、`rex_perfil*.log`）。**关键行**：
   - `[renut] T2DIAG release …`（有没有、涨不涨）
   - `[renut] T2DIAG COLA … rel= ag= tf= …`（四个会计字是否归零）
   - `[video] T2DIAG PERRO2 … dtick= ddrain= dpump= ddeliv= dcmd= dprod=`
   - `[video] T2DIAG PUERTA …` 与 `[renut] T2DIAG SWAP …`（老三行，做前后对比）
   - 游戏侧 fps 行（`juego … fps`）
4. 对照实验（可选，各跑一次）：把 `sa2_renut_display_release` 置 `false` 再跑一次，
   用来证明「症状回归 ↔ 完成侧释放」的一一对应。

---

## 7. 回退路径

- 逻辑回退（不动二进制）：`T2ROTF.toml` 里把 `sa2_renut_display_release` 与
  `sa2_renut_display_agent` 都置 `false` ⇒ 行为等同 T28 之前（已知会回到 0 fps 空转）。
- 源码回退（备份文件，均在同目录）：
  - `hooks/device.cpp.bak.t29` = **T28 构建时的 device.cpp 基线**（93,912 字节，已校验不含任何 T29 串）
  - `hooks/device.cpp.bak.t28` = T28 之前的基线（80,681 字节）
  - `t2rotf_video_nativo.cpp.bak.t29` = 前态（62,433 字节，0 个 `[T29]` 标记；由当前文件删除
    三段 `[T29]` 附加块重建，已核验原有锚点行完整保留）
  - 更早：`device.cpp.bak.t24` / `.bak.t2off` / `.bak`、`t2rotf_video_nativo.cpp.bak.t24` / `.bak.yield`

---

## 8. 未完成 / 未验证项（如实列出）

| 项 | 状态 | 说明 |
| --- | --- | --- |
| T29 构建实机验证 | **未做** | 需用户在 Switch 上跑一次并回传日志 |
| 用户手上前一轮日志的归属 | 已判定 | 属 **T25 构建**（无 `COLA/display-agent/release`），T28 的行为至今**从未被实机观察过** |
| 不超频 60 帧 | **未实测** | 交付后需在无超频条件下实测菜单/游戏内帧率、CPU/GPU 占用；对照参考项目超频数据（菜单 55–60、游戏内 40–52 FPS）与超频上限（CPU 1785 / GPU 921 / 内存 1600 MHz） |
| 影片解码链 | 仍待定 | `T2DIAG PERRO` 的 `pumpIn/pumpOut=6` 属**影片泵** `sub_827CE598`（非显示泵）；显示泵此前**没有探针**，本轮已补（`dpump_*`） |
| `*(container+16)`（0x82E55E30）写入者 | 未定位 | 决定宿主该补主通道还是后备通道入队；`tf=` 探针有助于确认 idx |
| `*(0x82DA74BC)` 实机取值 | 待新日志 | 新增 `tf=` 直接给答案 |
| `*(container+0)` 显示状态字段写入者 | 未找到 | 门后备判据 `!= 4` / slot2 `!= 5` 在实机是否成立仍未知 |
| 残留 >1 份时的抽干速率 | 已限速 | 每次最多各退 1、门槛 8 次观测；若实机是「每帧泄漏 1 份」，则速率足够；若泄漏更快需下一轮调门槛 |

---

## 9. 本轮未采用的方案（避免后人重走）

| 方案 | 为何不采用 |
| --- | --- |
| 把六通道标记（`obj+272/276/280`、`base+256/260/264`）置 1 强行 arm 门 | 标记置 1 只让门「就绪」，通道队列仍为空 ⇒ 交付返回 0；`base+284/+288`、条目`+24/+28` 不对账，标记也不会自然递减 ⇒ 每帧空交付/重复交付 |
| 宿主直接代客入队（按 `sub_822B5608` 语义补 `++base+284`、`++条目+24`、挂条目） | 会与 guest 自己的生产者重复计数，且需要正确的 `idx` 与条目表状态；本轮先走「只补完成侧」的最小改动，入队一侧留给 `tf=`/`dpump_*` 取证结论决定 |
| 继续在 Swap 钩子里驱动（T25 两个候选） | 实机日志证明卡死以后 **Swap 根本不再发生**（`T2DIAG SWAP` 仅 n=1），Swap 路径不可能驱动帧循环 |
| Vulkan / NVK / Mesa 路径 | 已在 T5–T7 评估并放弃，改走 deko3d / Renut |

---

## 10. 一句话交付说明

本轮把「有声无画 + 0 fps」的机理从「门三槽不就绪」进一步钉到
**「一份无人回收的已交付会计（`base+288=1`）卡死了每帧排空循环」**，并据此把宿主补成
**显示完成侧**（新增 `sa2_renut_display_release`、修正 T28 被滤掉的前置判断、门侧门槛 32→8），
外加一套只读显示链探针；构建已通过（退出码 0，产物字符串全部命中）。
**实机效果仍待下一轮日志验证；「不超频 60 帧」尚未实测。**