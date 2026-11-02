# T2ROTF deko3d — 投递通道钉到活体显示设备与楔死复位报告（v9.0-VIVO）

- 版本标记：`[video] T2ROTF BUILD v9.0-VIVO (display post channel pinned to the live display device)`
- 产物：`recomp/out/sw8/T2ROTF.nro` = 59,970,290 B，sha256 = `4a69142642488176872ca750633e2aec228d7d02bed48544ffa1aa1e202b91e5`
- 唯一改动源码：`recomp/src/t2rotf_video_nativo.cpp`（修复本体）、`recomp/src/t2rotf_gpu_signal.cpp`（仅版本横幅）
- 上一版：v8.9-PRODUCE（NRO 59,949,810 B）——实机「黑屏、第一个动画有声无画面」

---

## 0. 结论速览

v8.9-PRODUCE 已经把「第一帧投出去」这一环修通了（实机 `produce=1`、`base+284` 由 0→1、等门的呈现线程被唤醒并脱离 `sub_82881848` 自旋，为历轮首次），但**帧投出去之后再也不被 retire**：实机末态 `base+284=1`、`base+288=1` 全程不动、`SWAP n=1`，三处排空判据

```c
while (*(0x82E55CE0 + 284) + *(0x82E55CE0 + 288) != 0) { ... }
```

永远不为 0，于是画面仍不出、音频独立照播。

本轮（T72 离线逐指令 + T73 落地 + 重建）把这条卡死链的**根因钉在「显示投递通道被钉到了错的对象上」**：

- 门的上下文（`sub_82881B88` 的 `r30`）是**运行期堆对象**（实机 `0x41770018`，探针 `n` 打到 4×10⁶ 仍不变），
  而它的管理器自指字段 `*(r30+16)` 读出来是 **0**。门的就绪判据是
  `*(*(r30+16) + 272 + 4*slot)` —— 自指为 0 时整条表达式退化成 `*(0 + 272 + 4*slot)`，**恒为 0**
  （实机 `PUERTA … st0=0 s16=00000000 cnt=0/0/0`），门于是**永不就绪**、交付函数一次都没走通。
- 生产者那一侧又被 v7.7-ALIGN 改写到**静态管理器**的通道 `0x82E55E38`。两边取址分叉：生产者把
  arm 计数与对象推进静态通道的队列，门却按**活体设备**去读 —— armed 的东西永远不会被消费。

v9.0-VIVO 按 guest 自身语义把两侧**钉到同一个对象**：

1. 生产者投递通道 → **活体显示设备自己的通道** `dev+24`（实机 `0x41770030`）；
2. 补回该设备缺的**管理器自指** `*(dev+16) = dev`（与静态管理器 `*(M+16)=M` 的 init 语义同构，
   走 `RexGmShadowFor` 影子别名，取不到就跳过并计数——**绝不直解引用 guest 窗口地址**）；
3. 楔死态（`284==0 && 288>0` 且两侧六槽 arm 标记全 0、连续 120 次泵进入）交给 **guest 自己的**
   显示管线复位 `sub_82882628()` 处置，宿主**不写任何显示会计字段**；
4. 历史遗留的「宿主侧完成侧配平」（T37 曾直写 `*(base+288)` / `*(entry+28)`）**整支只读化**，
   源码里从此不存在对 `284/288/16548/16552/16564/16568` 的写操作。

本轮**没有**采信任何「宿主直写会计字段配平」的方案（那是 v8.1 已证失败并违反硬约束的做法），
也没有采信「只补 `*(0x41770018+16)`」的半程方案（会给门与生产者留下两个不同的取址，弹到错记录、
回调对象成垃圾 ⇒ 崩溃风险）。

---

## 1. 输入：v8.9-PRODUCE 实机取证（判读见 `T2ROTF_deko3d_v8.9实机判读定论.md`）

| 项 | 实机值 | 含义 |
| --- | --- | --- |
| `[video] T2ROTF BUILD` | `v8.9-PRODUCE` | 版本判据（只认这一行） |
| `[apm]` | GPU 460.8 MHz / 内存 1600.0 MHz | **未超频**手持 |
| `[T2DIAG PRODUCE]` | 出现 1 次，`produce=1` | 首帧确实投出去了（v8.8 从未做到） |
| `[T2DIAG DRIVE]` 末行 | `drive=646 ok=646 produce=1 prodSinDev=0 commit=0 pump=0 muerto=0 284=1 288=1` | 驱动线程活着、没有卡进 guest |
| `[T2DIAG espera]` | 停在 `n=680000 r3=F8000068 … lr=828818DC` 后**不再增长** | 等门的呈现线程已被唤醒、脱离自旋 |
| `[T2DIAG PUERTA]` | `r30=41770018 st0=0 s16=00000000 cnt=0/0/0`（n 达 4×10⁶ 不变） | **门的上下文是运行期设备，其 `+16` 为 0，三槽恒 0** |
| `[T2DIAG SWAP]` | `n=1` | 只交换过一次，之后再无帧可交换 |
| `[T2DIAG DRAIN-IN]` | 停在 `n≈680000` 之后不再推进 | 排空自旋未解 |

判读分支命中「C：`284` 非 0 但 `SWAP n` 仍 1（消费者没被唤醒）」的下半段——**帧已投递、却在飞永不退役**。

---

## 2. 离线定论（T72）：三条链的加减点与唯一修复点

### 2.1 门为什么永远不就绪

```text
门 sub_82881B88（yield 点 lr = 0x82881DE8）：
  就绪读   *(*(r30 + 16) + 272 + 4*slot)      r30 = 运行期显示设备（实机 0x41770018）
  调用方   每帧 tick 里被调一次；r30 不是镜像段地址，也不是静态管理器 0x82E55E20
实机：*(0x41770018 + 16) == 0  ⇒  就绪读退化成 *(0 + 272 + 4*slot) == 0（三槽 cnt=0/0/0）
     ⇒ 门恒返回 0  ⇒ 交付 sub_82881930 一次都没被调通过 ⇒ 没有任何一帧被 retire
```

### 2.2 排空为什么转不出去

```text
排空 sub_82882200（每帧 1 次）：
  while (*(0x82E55CE0 + 284) + *(0x82E55CE0 + 288) != 0) { 泵(0x82E55E20, idx, 1); }
  +284 由生产者入队自增，+288 由交付自增、由泵在完成回调返回后自减。
门盲 ⇒ 交付 0 次 ⇒ +288 无人增；而 v8.9 的补产把 +284 推到 1（这就是「第一帧投出去了」）
     ⇒ 判据恒为 1 ⇒ 排空无限重旋（DRAIN-IN n 越过 680000）⇒ 帧循环空转、画面不出
```

### 2.3 生产者被钉到了错的对象上

```text
生产者 sub_822B5608(r3=obj, r4=A)：++*(A + 248)（arm 计数）、push *(A + 8)（挂对象）
v7.7-ALIGN 把 A 强制写成静态管理器通道 0x82E55E38
门/交付却按「活体设备 +16 指向的管理器」取址（自指正常时应为设备自己）
⇒ 生产者 arm 的地方没人读、门读的地方没人 arm —— armed 永不被消费
```

### 2.4 已排除的做法（本轮明令不用）

| 做法 | 为什么不用 |
| --- | --- |
| 只补 `*(0x41770018 + 16) = 0x82E55E20` | 门的交付弹址（`*(r3+16)+24` 或 `r3+24`）与生产者 push 址分叉 ⇒ 弹到别的队列、回调对象成垃圾 ⇒ **崩溃风险高** |
| 宿主直写 `+284` / `+288` 配平在飞项 | 违反本工程硬约束（不许写显示会计字段）；v8.1 已证只会造成回绕与 0 fps 假象 |
| 拿 `*(0x82DBB608)`（当前显示对象）当闸门 | 卡死态该字段读 0，会把修复静默禁用 |
| 用直解引用读设备堆 | 违反影子访存纪律，会触发 `Unhandled guest access violation` fatal |

---

## 3. 落地修复（v9.0-VIVO）

### 3.1 通道归一：生产者钉到活体设备自己的通道

`recomp/src/t2rotf_video_nativo.cpp`，`REX_HOOK_RAW(sub_822B5608)` 内：

```c
// 1) 先记下活体设备（生产者原生 r4 = 设备 + 24；实机 0x41770030 ⇒ dev = 0x41770018）
if (b && REXCVAR_GET(sa2_renut_display_vivo)) {
  const uint32_t a_antes = ctx.r4.u32;
  if (a_antes >= 0x40000000u && a_antes < 0x82000000u) {
    const uint32_t dev = a_antes - 24u;
    g_disp_vivo.store(dev, ...);
    // 2) 补回它缺的管理器自指（链接字段，不是会计字段；一律走影子别名）
    if (Leer32Alias(b, dev + 16u, &enl) && enl != dev) {
      ... 日志 T2DIAG VIVO-LINK ...
      Escribir32Alias(b, dev + 16u, dev);
    }
  }
}
// 3) 通道目标：VIVO 有效时钉到 dev+24，否则退回静态 0x82E55E38
uint32_t chbase = cont + 24u;
if (REXCVAR_GET(sa2_renut_display_vivo)) { const uint32_t d = g_disp_vivo.load(...); if (d) chbase = d + 24u; }
if (ctx.r4.u32 != chbase) { ... 日志 T2DIAG ALIGN ... ctx.r4.u32 = chbase; }
```

### 3.2 三处取址自洽核对（**这是本轮的关键论证**）

| 侧 | 取址写法 | 自指修复后落点 |
| --- | --- | --- |
| 生产者 arm | `++*(A + 248)`，`A = dev + 24` | `dev + 272` |
| 生产者 push | `*(A + 8)` | `dev + 32` |
| 门 就绪读 | `*(*(dev + 16) + 272 + 4*slot)`，`*(dev+16) == dev` | `dev + 272` |
| 交付 弹 | `*(*(dev + 16) + 24) + slot*80 + 8` | `dev + 32` |
| 交付 弹（若按 `r3 + 24` 取址） | `r3 = dev + 24` | `dev + 32` |

无论交付是按「先解管理器自指再取通道」还是按「`r3` 直接 `+24`」取址，**落点都是 `dev+32`**；
生产者的 arm 与门的就绪读同样都落在 `dev+272`。两侧必然重合 —— **不存在「弹到别的队列、把回调对象弹成垃圾」的崩溃路径**。

### 3.3 楔死态复位：交给 guest 自己

判据（`T2ReinicioSiAtascado`，在泵钩子 `sub_82881E00` 每次进入时评估，只读）：

```text
假设 H = (*(0x82E55CE0 + 284) == 0)              // 没有待交付
      && (*(0x82E55CE0 + 288) > 0)               // 却还有在飞残留
      && (两侧六槽 arm 标记全 0)                  // 六槽 = 全局队列 +256/+260/+264，
                                                 //        静态通道 0x82E55E38+248/+252/+256
                                                 //        活体设备 dev+272/+276/+280（影子别名读）
H 连续命中 kReinicioTics = 120 次（每次泵进入计一次）后：
  用 ctx 的副本进 guest，调用 guest 自己的显示管线复位 sub_82882628()（总次数封顶 8）
  —— 该函数无参数、硬编码 r31 = 0x82E55CE0，末尾由 guest 代码自己 stw 284/288
```

- **宿主不写任何显示会计字段**：`284 / 288 / 16548 / 16552 / 16564 / 16568` 一个都不碰；
- 用 `PPCContext tctx = ctx;` 的**副本**进入 guest，绝不污染正要转发给泵原体的那一份；
- 超预算（> 8 次）后只落一条 `[video] T2DIAG REINICIO topado` 并停手，不再反复复位。

### 3.4 T37 完成侧配平「只读化」

历史版本（v7.6/v7.5 起）在排空入口有一支「无待交付而在飞有残留」时**把 `*(base+288)` 与
`*(entry+28)` 减掉**的宿主侧配平，那属于**直接写 guest 显示会计字段**，与本工程硬约束冲突
（且 v8.9 实机证明它从未命中：wedge 态 `e24` 恒 1、`e24 + e28` 非 0 的前提不成立）。
本轮把它整支改成**纯只读观测**：

- 不再有任何 `Escribir32(b, cola + 288, …)` / `Escribir32(b, e + 28, …)`；
- 命中该条件时只限频打一条 `[video] T2DIAG RECON-RO …`，并在日志里显式写明
  「read-only: no host write to any accounting field」；
- 楔死态的真正处置交给 3.3 的 guest 复位。

### 3.5 新增开关（都可 `kInitOnly` 回退）

| 键 | 默认 | 作用 |
| --- | --- | --- |
| `sa2_renut_display_vivo` | `true` | 把投递通道钉到活体显示设备自己的通道（`dev+24`）并补回 `*(dev+16)=dev` |
| `sa2_renut_display_reinicio` | `true` | 楔死态下调用 guest 自己的显示管线复位 `sub_82882628()` |

回退阶梯（改 `T2ROTF.toml` 后**完全退出再启动**）：

```text
sa2_renut_display_vivo = false          （退回 v8.9：生产者仍钉静态通道 ⇒ 预期回到黑屏）
sa2_renut_display_reinicio = false      （只留通道归一，不做楔死复位）
sa2_renut_display_align = false         （连同 v7.7 的静态通道对齐一起关；VIVO 的通道改写位于该支内）
sa2_renut_display_reconcile = false     （只关掉那条只读观测日志）
```

> 注：`sa2_renut_display_vivo` 的**通道改写**用例放在 `sa2_renut_display_align` 支内（复用其日志，
> 保证「谁改了 `ctx.r4`」只有一处），而 `*(dev+16)=dev` 的自指修复不依赖 align，只要 VIVO 开就生效。

---

## 4. 构建与静态取证

### 4.1 构建

```text
cd recomp/out/sw8 && ninja -j4 T2ROTF      → 退出码 0（codegen 0 written / 0 unchanged，仅重编 video_nativo.cpp + 链接打包）
```

### 4.2 产物

| 项 | 值 |
| --- | --- |
| `recomp/out/sw8/T2ROTF.nro` | 59,970,290 B（上一版 59,949,810 B，+20,480 B） |
| sha256 | `4a69142642488176872ca750633e2aec228d7d02bed48544ffa1aa1e202b91e5` |
| 时间戳 | 2026-11-02 16:51:08 |

### 4.3 版本判据唯一

```text
T2ROTF BUILD 串在 NRO 中共 2 处，全部是 v9.0-VIVO：
  "[video] T2ROTF BUILD v9.0-VIVO (display post channel pinned to the live display device)"
  "… BUILD v9.0-VIVO detail: …"
历史版本 BUILD 串残留：0（v8.x / v7.x 均无）
v7.6-RECON 已降级为探针串："T2ROTF probe v7.6-RECON: display-completion reconcile at the drain entry"
```

### 4.4 新增探针（字符串取证，均已在 NRO 中）

| 串 | 用途 |
| --- | --- |
| `[video] T2DIAG VIVO-LINK n={} dev={:08X} c0={:08X} c16_antes={:08X}` | 记录活体设备基址与 `+16` 修复前后值 |
| `[video] T2DIAG REINICIO n={} c284={} c288={} cont={:08X} dev={:08X} hechos={}` | 楔死复位（调用 guest `sub_82882628`）落点 |
| `[video] T2DIAG REINICIO topado: guest display re-init budget (8) exhausted` | 复位预算封顶 |
| `[video] T2DIAG RECON-RO …(read-only: no host write to any accounting field…)` | T37 只读化后的观测行 |
| `[video] T2ROTF probe: post channel pinned to the live display device (v9.0-VIVO selftest)` | 通道改写支首次进入 |

旧写法 `T2DIAG RECON nr=` 已从二进制中**完全消失**（计数 0）。

### 4.5 符号

```text
0000000001864200 T __imp__sub_82882628                      （guest 显示管线复位入口）
0000000002e321a0 T _Z20T2ReinicioSiAtascadoR10PPCContextPh （宿主楔死判据/复位包装）
```

### 4.6 「不写显示会计字段」的源码级取证

```text
源码中全部 Escribir32/Escribir32Alias 落点：
  video_nativo.cpp: dev + 16            （管理器自指，链接字段，影子别名写）
  video_nativo.cpp: 0x82E55E20 + 16/+24 （静态管理器的自指/通道自指，链接字段）
  video_nativo.cpp: ctx_addr + 3916     （与显示无关的历史钩子）
  gpu_signal.cpp:   dev + 16544（回调槽，一次性守卫写后立即还原，非会计字段）
  gpu_signal.cpp:   blk + 16（环形暂存块，非会计字段）
对 284 / 288 / 16548 / 16552 / 16564 / 16568：**读只有读、无任何写**
```

约束 M9 / M10 / M11 满足方式：

- **M9**：宿主不再写 `284/288/16548/16552/16564/16568`（T37 支已只读化）；
- **M10**：设备堆（`0x4177xxxx`）与活体设备 `+16`、`dev+272` 全部经 `RexGmShadowFor` 影子别名
  （`AliasGuest/Leer32Alias/Escribir32Alias`），取不到别名就跳过并计 `g_vivo_sin_shadow`，无直解引用；
- **M11**：`T2ReinicioSiAtascado` 只在**泵**的 guest 调用路径（guest 线程）上评估与调用 guest 代码，
  垂直同步派发线程依旧绝不进入 guest。

---

## 5. 实机验证清单（**不超频**手持）

1. 把新的 `T2ROTF.nro` + `T2ROTF.toml` 覆盖到 `sdmc:/switch/Transformers2ROTF/`，**完全退出再启动**。
2. 版本判据：日志里 `[video] T2ROTF BUILD` 必须只有 `v9.0-VIVO`（应用横幅仍是旧的，不作判据）。
3. `[apm]` 行确认未超频（GPU 460.8 / 内存 1600.0 MHz）。
4. 逐条回传：

| 关键行 | 期望（修复生效） |
| --- | --- |
| `[T2DIAG VIVO-LINK n=… dev=41770018 … c16_antes=00000000` | 出现，且 `dev` 与 v8.9 的 `PUERTA r30` 一致 |
| `[T2DIAG PUERTA … s16=…` | `s16` 不再是 `00000000`（自指已补回），三槽 `cnt` 出现非 0 |
| `[T2DIAG ALIGN n=… a_antes=41770030 a_ahora=41770030` | 通道目标与生产者原生通道一致（不再被改写） |
| `[T2DIAG DRAIN-IN n=…` | **停止增长**（v8.9 是越过 680000 还在涨） |
| `[T2DIAG DELIV / SWAP n=` | `SWAP n` 推进（> 1 且持续增长） |
| `[T2DIAG REINICIO …` | **不应出现**；若出现说明门仍未就绪，请连同 `PUERTA` / `VIVO-LINK` 一起回传 |
| `rex_perfil.log` 的 `juego … fps` 与 `relojes:` | 有非 0 fps 与未超频时钟 |
| `rex_crash.log`（若产生） | 附 `pc` / `far` / `esr` 前 40 行 |

5. 若已出画面：进菜单与游戏内各跑一段，回传 `fps` 行，作为 60 FPS 验收依据（60 帧结论只在**不超频**条件下给）。

---

## 6. 风险与未决

| 项 | 说明 |
| --- | --- |
| 影子别名可用性 | 若实机 `g_vivo_sin_shadow` 持续增长，说明该设备对象不在可交错的 guest 物理内存里，自指/读槽会被跳过；此时以计数为准，不直解引用（宁可不动，也不崩） |
| 门是否**只**差这一个自指 | 本轮论证「就绪读退化」是 v8.9 实机唯一可解释的三槽恒 0 原因；若实机补自指后三槽仍恒 0，则下一卡点须再看 `+272` 指向的记录阵列是否已建立 |
| 楔死复位的实机标定 | `kReinicioTics = 120`（泵进入次数）与 8 次上限是离线取的经验值，未经实机标定；若实机出现 `REINICIO` 则应视为「通道归一未完全奏效」的信号，而不是修复成功 |
| 60 帧仍未验收 | 本轮只解决「画面不出」；帧率仍待画面出现后在不超频条件下实测（KPI 未变） |
| 未回退项 | v8.9 的「先投递 → 再可选提交（默认关）→ 最后可选交接（默认 0）」顺序保持不变；`sa2_renut_display_drive_produce` 仍为 `true`（补产只在 `base+284 == 0` 时投递） |

---

## 7. 变更文件清单

| 文件 | 变更 |
| --- | --- |
| `recomp/src/t2rotf_video_nativo.cpp` | v9.0-VIVO 区块（说明 + `RexGmShadowFor` 本地声明 + `AliasGuest/Leer32Alias/Escribir32Alias` + 2 cvar + 计数）；生产者钩子内设备捕获与 `*(dev+16)=dev` 自指修复、通道目标改 `dev+24`；泵钩子内楔死判据 + guest `sub_82882628()` 复位（经 `T2ReinicioSiAtascado`）；T37 配平支只读化；`v7.6-RECON` 由 BUILD 串降级为探针串 |
| `recomp/src/t2rotf_gpu_signal.cpp` | 仅版本横幅：v8.9-PRODUCE 长串 → `v9.0-VIVO` 两行（含本轮机理与保留项声明） |
| `T2ROTF.toml` | 新增 `sa2_renut_display_vivo` / `sa2_renut_display_reinicio` 两键（均默认 `true`）；`sa2_renut_display_reconcile` 注释改为只读观测 |
| `README.md` / `readme_zh.md` | 英文 README.md 重写 + 中文 readme_zh.md（两者互链） |
| `T2ROTF_switch_port.zip` | 原地更新（NRO / toml / README.md / readme_zh.md / `recomp_src/t2rotf_video_nativo.cpp` / 本报告） |