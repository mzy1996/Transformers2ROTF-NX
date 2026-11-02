# T2ROTF（Xbox 360《变形金刚2：卷土重来》）Switch 移植
## 启动失败双根因定论与「只读化 + 完成链守卫」修复报告（T31 / v7.5）

- 工程根：`J:\XBOX360 port\Transformers Revenge of the Fallen`
- 产物：`recomp/out/sw8/T2ROTF.nro` = **59,847,410 字节**，sha256 **`3728fd7ed98e5801b905c4e4fbd7d309f1403a232c921f2ebcd1d67eeb2eeeab`**
  （上一版 v7.4/T29 为 59,847,410 字节、`7186022a…`；**字节数巧合相同，比对务必用 sha256**）
- 配置：`T2ROTF.toml` = 6,990 字节，sha256 `b77adfc88a4dbc51a32a8b38fa26a397d60e33eaa35e3479f76df5c9484a212c`
- 改动的源码：`recomp/src/renut_engine/gpu/d3d/hooks/device.cpp`（98,921 → 103,185 字节；备份 `device.cpp.bak.t29` 为 v7.4 基线）
- 构建：`cd recomp/out/sw8 && ninja -j4 T2ROTF` → **退出码 0**（52 步增量；codegen「0 written, 0 unchanged, 0 deleted, 1 module(s) up to date」36.0 s）
- 日期：2026-10-28

---

## 1. 现象

用户实机（2026-10-28）反馈两条互不相同、但同源的表现：

| 启动方式 | 现象 |
|---|---|
| 从 Sphaira（自制前端）启动 | **无限黑屏、没有任何加载**（进程不退出、画面不推进） |
| 从前端（Homebrew 菜单）启动 | **直接报错退出** |

随本轮反馈提供的日志：`rex_crash(35).log`、`rex_perfil(27).log`、`rex_stderr(63).log`、`T2ROTF_001(61).log`、`T2ROTF_002(42).log`。

**先说结论**：两条现象都出在 v7.4（T29）引入的「宿主代客写显示会计」之上，但成因是同一份代码的两个不同侧面：

1. **无限黑屏** = 完成侧释放挂在**门热路**上，把同一个会计字 `base+288` 由 1 刷过 0 并**回绕成 `0xFFFFFFFF`** ⇒ guest 排空循环 `while (*(base+284) + *(base+288) != 0)` **永不退出**，帧循环停在第一帧、启动过程再无任何推进。
2. **报错退出** = 若只清一次让门「提前就绪」，guest 会走进「当前显示对象为空」的完成回调链，在 `*(0 + 0x368)` 上**访问违例**（guest Data Abort）。

因此 v7.5 的修复是两件事：**把盲写回退成只读**（根因一）＋ **给完成回调链上的四个 guest 函数加空对象守卫**（根因二）。

---

## 2. 取证：这确实是 v7.5 之前的那一版构建

| 判据 | 读数 | 结论 |
|---|---|---|
| `rex_stderr(63)` 版本串 | `v6.27-MAPFIX` | 与长期在跑的崩溃钩子版本一致 |
| `rex_stderr(63)` 末行 | `[DIAG] OnInitialize: LaunchModule done` | 应用初始化本身跑完了，问题在之后 |
| `rex_stderr(63)` guest 内存 | `RexGmInit: Direction F map FAILED … rc=0x0000D401`、`code-memory path unavailable`、`return base=0x63a5422000 size=0x120000000` | guest 基址 **0x63a5422000**（下面算 `far` 要用） |
| `T2ROTF_001(61)` 计数 | `T2DIAG` 8055 行、`release` **4116** 行、`PUERTA` 69、`COLA` 1、`PERRO2` 0 | 含 v7.4 才有的 `release` 行 ⇒ **是 v7.4 构建的日志** |
| `T2ROTF_002(42)` 计数 | `T2DIAG` 152 行、`release` 1、`PUERTA` 32、`COLA` 1、`PERRO2` 1 | 第二次启动（短跑，随即崩） |

两次启动的时间线：

- **launch 1**：10:50:24 → 10:53:01（约 2.6 分钟），首行 `COLA … 284=0 288=1 e24=0 e28=1 st0=0 dctx=0x82e55e20`，`release n=1` 起开始扣减，尾部是自旋探针 `espera sub_824F3580 … r3=F8000060 lr=828818DC` ⇒ **一直在自旋、没有推进**。
- **launch 2**：10:53:12 → 10:53:17（**5 秒**），`release n=1` 之后即 guest 访问违例退出；在此之前还能看到影片准备 `Prepare ctx … ruta='game:\movies\luxo_logo.wmv'`、`pump pumpIn=1 pumpOut=1`、`iny=1 pres=0`、`PERRO2=1`。

> 说明：**哪一条启动路径对应哪次 launch 未与用户逐字确认**。从时长特征看，长跑自旋（2.6 min）更像 Sphaira 的「无限黑屏」，5 秒即崩更像前端的「直接报错退出」；下面两条根因的推导不依赖这个对应关系（两种表现都由同一份代码产生，只是时序不同）。

---

## 3. 根因一：门热路上的盲写把会计字**回绕**了（Sphaira 无限黑屏）

### 3.1 事实链

1. v7.4 的完成侧释放（`sa2_renut_display_release`，默认 **true**）被挂了两处：Swap 钩子与**呈现门钩子** `sub_82881B88`。
2. 呈现门在**帧循环卡死**时会被以极高频率调用（v7.2 起就有的老结论：`YIELD total` 30 秒涨到 4,480 万、热点单点 `lr=0x82881DE8` 即门的「三槽全不就绪」分支）。本轮日志把量级钉死：
   - `T2ROTF_001(61)` 里 `T2DIAG release` 行共 **4116** 行；该行的限频是「前 24 次 + 每 4096 次」⇒ 说明计数器 `n` 已经涨到 ≈ `(4116 − 24) × 4096 ≈` **16,760,832**（与日志里出现的数字 **16760832** 一致）；
   - 16,760,832 次 ÷ 157 秒 ≈ **10.7 万次/秒** —— 与 `yield` 计数（432 万量级/几十秒）同量级。
3. 每次 `DisplayAgentStep` 在「六通道标记全 0 且指纹连续同一」时会**各退 1**（`base+288` 与条目 `+28`，日志 `writes=2`）。
4. 但**同一个会计字在同一帧内被反复扣减**：首行 `COLA` 读数是 `288=1 e28=1`，随后采样到的 `cols288 / e28` 变成 **`4294967295` → `4294967294` → … → `4294967291`** —— 即 1 被扣到 0 后**继续向下回绕**（无符号表示成 `0xFFFFFFFF` 起递减）。
5. guest 的每帧排空（`sub_82ABB7D0` → `sub_82882200`）判据是：
   `while (*(base+284) + *(base+288) != 0) sub_82881E00(container, idx, 1);`
   `base+288` 变成 `0xFFFFFFFF` 之后这个和**永不为 0** ⇒ **排空循环永不退出** ⇒ 帧循环停在第一帧 ⇒ 黑屏、无加载、`fps=0`。

### 3.2 为什么「非 0 才退、绝不回负」的守卫没挡住

v7.4 的守卫只保证「这一次退到 0 就停」，**挡不住下一次重入再退**：门在同一帧内被调上百万次，每次的读值都是「上一轮刚被自己退掉后的值」，于是从 1 出发一路退穿 0 并回绕。日志里 `cola288` 单调递减正是这个过程的直接证据。

### 3.3 结论

**门热路上写 guest 会计在语义上不成立**——它不是「补一次完成」，而是「把完成事件源按门的调用频率伪造了一百万次」。修复必须回退这一写入（见 §5）。

---

## 4. 根因二：当前显示对象为空时的完成回调链（前端报错退出）

### 4.1 崩溃现场（`rex_crash(35).log`）

```
hilo 4280
imagen   0x269078d000
pc       imagen+0x2a5444c
lr       imagen+0x2a543fc
esr      0x92000005        (EC 0x24 → Data Abort，同级别域)
far      0x63a5422368
instruccion 0xb8614a81
far dentro de la memoria del invitado: 0x368
x0 = x1 = 0x368        x2 = 0x63a5422368      x3  = 0xffffffff82dc0000
x19 = 0x4f63d87080     x20 = 0x63a5422000     x21 = 0x64281da000
x22 = 0x8287dd38       x26 = 0x82881e5c
hilo 帧链：#00 0x224eb50 … #09 0x315d634  #10 0x314ee08  #11 0
```

### 4.2 符号化（**用当前构建的符号表**）

上一轮曾用 `_syms_now.txt`（2026-10-28 02:39，属**旧**构建）解过同一类地址，结论「崩在 `sub_8287D0F8+0x1bc`」——**本次核对后作废**：该表相对当前构建整体偏移 **+0x330**（例：`__imp__sub_8287D0F8` 旧 `0x2a54290` → 新 `0x2a545c0`；`sub_8287D0F8` 旧 `0x2a54a30`）。改用当前未剥离 ELF 导出的 `_syms_cur.txt`（`aarch64-none-elf-nm --defined-only --numeric-sort T2ROTF`，91,158 行）重解：

| 日志值 | 解析结果 |
|---|---|
| `pc = imagen+0x2a5444c` | `__imp__sub_8287BF88 + 0x7c` |
| `lr = imagen+0x2a543fc` | `__imp__sub_8287BF88 + 0x2c` |
| 帧链 `#02 imagen+0x315d630` | `__imp__sub_82881E00 + 0x1e4`（**显示泵内**） |
| 帧链 `#03 imagen+0x315d634` | `sub_82881E00 + 0x28` |
| 其后若干帧 | `__imp__sub_8287DD08+0xd0`、`__imp__sub_828812D0+0x508`、`__imp__sub_82881F00+0x58`、`__imp__sub_824F5578+0xd8`，再往下是宿主 rex 线程栈（`XThread::Execute` / `PosixCondition<Thread>::ThreadStartRoutine`） |

也就是说：**崩点在「显示泵 → 交付 → 对象更新 → `sub_8287BF88`」这条完成回调链的末端**，而不是渲染、不是文件系统。

### 4.3 崩溃指令与「当前显示对象」单元

对 `__imp__sub_8287BF88` 附近反汇编（`objdump -d --start-address=0x2a54410 --stop-address=0x2a54480`）：

```
2a5442c: ldr  w0, [x1, #13832]      ; 0x3608 → 取全局 0x82DB608(...)（见下）
2a5443c: add  w1, w0, #0x368
2a5444c: ldr  w1, [x20, w1, uxtw]   ; ← 崩溃：w0 = 0 ⇒ 读 guest 0x368
2a54450: cmp  w1, #0
         cset / cset / b.eq …
         str  x3, [x19, #224]       ; ctx 同步写回
```

`w0 = 0` 与 `far = 0x63a5422368 = guest 基址 0x63a5422000 + 0x368` 完全吻合：**这是 `*(0 + 0x368)`**。

生成代码侧（`recomp/generated/default/T2ROTF_recomp.43.cpp:91679`，`DEFINE_REX_FUNC(sub_8287BF88)`，69 行）**第一条语句**就是：

```cpp
ctx.r28.s64 = -2099511296;                              // 0x82DC0000 段
ctx.r11     = REX_LOAD_U32(ctx.r28.u32 + -18936);        // 全局 0x82DBB608
ctx.r31     = REX_LOAD_U32(ctx.r11.u32 + 872);           // 链表头
```

**没有任何非空判断**：`0x82DBB608` 这个「当前显示对象」单元为 0 时，`r11 = 0`，紧接着的 `+872` 与后续 `+0x368` 就会在 guest 低位地址上访问违例。

### 4.4 0x82DBB608 是什么、为什么本移植下会是 0

| 事实 | 证据 |
|---|---|
| 它是「**当前显示对象**」单元 | `sub_8287E410`（44.cpp:90596）是把 `*(0x82DBB608)` **换入 → 调 `sub_8287DD08` 处理 → 换回** 的包装 |
| 它是**按帧建立/撤销**的临时态 | 每帧 tick `sub_82ABB7D0`（29.cpp:122845）与 `sub_828812D0`（22.cpp:95629）都写它；同段还把 `0x82DBB6xx` 一片伙伴单元清零 |
| **生成代码里找不到独立创建者** | 穷举写者：`sub_828812D0`、`sub_82ABB7D0`（tick）、`sub_8287DD08`（临时存取）、`sub_8287E410`（换入换回）——都是「使用者」，没有「初始化者」 |
| 本移植下它可能是 0 | 缺掉的初始化路径（Xbox 上由显示/呈现初始化建立）在本移植未跑 ⇒ 单元保持 0 |

### 4.5 为什么「前端启动」会撞上它

前端启动路径推进得更快（5 秒内就到了影片准备），一旦 v7.4 的完成侧释放**清掉一次会计**让门**提前就绪**，guest 就会真的走进「门成功 → 显示泵 → 交付 → 对象更新 → `sub_8287BF88`」这条此前从未被走通的链；而链末端的「当前显示对象」是 0 ⇒ **立即访问违例、进程报错退出**。

> 这正是把「宿主代客写会计」当修复手段的结构性缺陷：**它能让门就绪，却没有让门之后的链条具备可运行的前提**。

---

## 5. 修复内容（v7.5 / T31）

改动全部位于 `recomp/src/renut_engine/gpu/d3d/hooks/device.cpp`（98,921 → **103,185** 字节；备份 `device.cpp.bak.t29` = v7.4 基线）。共五处：

### 5.1 回退盲写：两个写会计开关默认 true → false

`REXCVAR_DEFINE_BOOL(sa2_renut_display_release, false, …)`、`REXCVAR_DEFINE_BOOL(sa2_renut_display_agent, false, …)`，并各加一条 `[T31]` 注释说明回退原因。**开关保留**只是为了复现取证。

### 5.2 `DisplayAgentStep` 由「写会计」改为**只读观察**

原两段（退 `base+288`/条目 `+28`；退 `base+284`/条目 `+24`）整体删除，改为：

```cpp
const u32 observados = (posted_base?1:0) + (inflight_base?1:0) + (entry_posted?1:0) + (entry_inflight?1:0);
const u32 n = g_display_release_n.fetch_add(1, std::memory_order_relaxed) + 1u;
if (n <= 16u || (n % 8192u) == 0u) {
  RGPU_WARN("[renut] T2DIAG cola-readonly n={} src={} idx={} cnt={}/{}/{} obj256={}/{}/{} "
            "cola284={} cola288={} e24={} e28={} obs={} onj={}", …);
}
return 0u;   // 永不写 guest 内存
```

要点：

- **函数不再写 guest 内存一个字**（源码核查：`StoreGuestU32` 只剩定义、`escritos` 计数为 0）；
- 因此 **SD 卡上留着 v7.4 的 `release/agent = true` 也不再致害**：那两个键现在只影响「是否调用这个只读函数」和观察日志的多少；
- 新增字段 `onj=`（= `*(0x82DBB608)` 的实机取值），供下一轮直接回答「当前显示对象为何为空」。

### 5.3 新增常量 `kDisplayObjectCell = 0x82DBB608`

连同一段 `[T31]` 注释，写清它的语义、取证来源（`sub_8287E410` 的换入换回）、以及「找不到独立创建者」这一事实。

### 5.4 新增守卫开关 `sa2_renut_display_guard`（默认 **true**，`kInitOnly`）

### 5.5 文件末尾新增四个 `BANJO_RENUT_HOOK_RAW` 守卫

| 被守卫的 guest 函数 | 短路条件 | 作用 |
|---|---|---|
| `sub_8287BF88` | `*(0x82DBB608) == 0` | 完成链末端、**本次崩溃点**（首条语句即解引用） |
| `sub_8287DD08` | `*(0x82DBB608) == 0` | 该显示对象的更新函数 |
| `sub_8287E410` | `*(0x82DBB608) == 0` **或** `r3 == 0` | 「换入→更新→换回」包装（r3=0 时会主动把 0 装进单元） |
| `sub_8287D7C0` | `r3 == 0` | 相邻函数，防 `*(0)` 族解引用 |

命中时记一行限频日志（前 32 次 + 每 4096 次）：

```
[renut] T2DIAG GUARDIA n=… fun=sub_8287BF88 va=0x8287bf88 onj=0 cola284=… cola288=…
```

> 守卫是**短路**（不调原体）而非「继续执行原体再补救」——因为原体的第一条语句就是崩溃点，没有可补救的余地。宿主可整体替换 guest 函数是这套 hook 框架既有能力（`BANJO_RENUT_HOOK_RAW` 的展开即「Renut 后端启用时走自己的体、否则走原体」，见 `banjo_renut_select.h:48-58`）。

### 5.6 为什么**不**保留「只清一次就停手」的写法

因为它只能修掉「无限黑屏」，修不掉「报错退出」：清一次 → 门就绪 → guest 走进空对象完成链 → 崩。**真正该补的是「显示完成事件源」**（承接 v7.4：Xbox 上把 `base+288`/条目 `+28` 减回去的是 GPU 垂直同步/完成中断 ISR 族 `sub_82997AF8` / `sub_82997968` / `sub_829975F0`，它们在生成代码里没有任何 guest 调用者），而不是在门热路上伪造完成。本版据此把「伪造」回退，把「崩溃面」封住，把画面问题留给下一里程碑。

---

## 6. 开关语义 / 回退方式

| toml 键 | 本版默认 | 作用 | 回退 |
|---|---|---|---|
| `sa2_renut_display_guard` | **true** | `*(0x82DBB608)==0` 时短路完成回调链上四个 guest 函数 | 置 `false` = 全直调（会重新出现前端启动即崩，仅供对照） |
| `sa2_renut_display_release` | false | **只读观察**（不再写会计） | 置 `true` 只增加观察日志，无副作用 |
| `sa2_renut_display_agent` | false | **只读观察**（不再写会计） | 同上 |
| `sa2_renut_display_tick` | false | T25 遗留对照开关（已证伪） | 保持 false |
| `sa2_renut_swap_callthrough` | false | T25 遗留对照开关（已证伪） | 保持 false |

- 五个键都是 `kInitOnly`：**改完必须重启游戏**。
- 键名即 cvar 名；等价环境变量写法：`REX_SA2_RENUT_DISPLAY_GUARD=1`。
- 放盘位置：与 NRO 同目录、同名 `T2ROTF.toml`，即 `sdmc:/switch/Transformers2ROTF/T2ROTF.toml`（不放也生效）。
- **源码级回退**：`renut_device.cpp.bak.t29`（= v7.4 基线的 `device.cpp`）。

---

## 7. 构建与静态校验证据

| 项 | 结果 |
|---|---|
| `ninja -j4 T2ROTF` | **EXIT=0**；`[1/52]` 重编 `device.cpp.obj`；codegen 36.0 s 无改动；`[3/3] Linking CXX executable T2ROTF; Packaging T2ROTF.nro` |
| NRO | 59,847,410 字节，sha256 `3728fd7ed98e5801b905c4e4fbd7d309f1403a232c921f2ebcd1d67eeb2eeeab` |
| 产物字符串取证 | `GUARDIA`=1、`cola-readonly`=1、`sa2_renut_display_guard`=2 ⇒ 新代码确在 NRO 内 |
| 源码核查 | `StoreGuestU32` 仅剩定义（无残余写会计）；`escritos` 计数 = 0；四个 `BANJO_RENUT_HOOK_RAW(sub_8287BF88/8287DD08/8287E410/8287D7C0)` 均在（dev.cpp:1969 / 1977 / 1987 / 1997）；`REXCVAR_DEFINE_BOOL(sa2_renut_display_guard`（dev.cpp:1145） |
| 命名冲突核查 | 四个被守卫函数在 `recomp/src` 下（排除 `generated/`）**无任何既有钩子/引用** ⇒ 新守卫不与现有挂点冲突 |
| 版本横幅 | NRO 内仍为 `v6.27-MAPFIX`（本轮未改该常量；与应用日志的版本串一致） |

> 注意：本工程 NRO 内嵌构建时间戳，**同一份源码重构建后体积相同、哈希会变**。文档与包内哈希一律以随包 NRO 实算值为准。

---

## 8. 实机验证步骤（本轮由用户执行）

1. 用本包内的 `T2ROTF.nro` 与 `T2ROTF.toml` 覆盖到 `sdmc:/switch/Transformers2ROTF/`（toml 需与 NRO 同目录、同名 `T2ROTF`），**完全退出应用再启动**，**不要超频**。
2. 两条启动路径各跑一次，验收点：
   - 前端（Homebrew 菜单）启动：**不再直接报错退出**；
   - Sphaira 启动：**不再无限黑屏无加载**（至少能看到初始化推进、日志落盘；`fps` 仍可能是 0）。
3. 回传 `logs/rex/` 下的 `rex_stderr.log`、`rex_perfil.log`、`T2ROTF_001.log`、`T2ROTF_002.log`（若有 `rex_crash.log` 一并带上）。
4. 判读要点（下一轮直接用）：
   - 出现 `[renut] T2DIAG GUARDIA … fun=sub_8287BF88 … onj=0` ⇒ **确认 `*(0x82DBB608)` 在实机为 0、守卫正在短路**（本轮的正确吃法，不是新故障）；
   - `onj=` 若为**非 0** 却仍崩 ⇒ 说明「当前显示对象」另有来源或另有空指针路径，需按新 `far` 重新符号化；
   - `T2DIAG PUERTA` 与 `yield` 计数是否仍与 v7.4 同量级（门是否仍在无限自旋）；
   - `cola-readonly` 行默认**不会出现**（其调用条件挂在两个已关闭的开关上）；需要该观察时把 `sa2_renut_display_release` 或 `sa2_renut_display_agent` 置 `true` 再跑一次（只读，无副作用）。

---

## 9. 未决项与下一里程碑

| 项 | 状态 |
|---|---|
| 本版（v7.5）实机验证 | **未做**（需用户按 §8 执行） |
| 「第一个 logo 之后有声无画」 | **未解决**。根因仍是 **Switch 后端缺少显示完成事件源**（v7.4 结论不变） |
| **不超频流畅 60 帧** | **未实测**；在画面链路恢复之前无法给出结论。参考对照（**不得用于达标**）：参考项目超频下菜单 55–60 FPS、游戏内 40–52 FPS；超频上限 CPU 1785 MHz / GPU 921 MHz / 内存 1600 MHz |
| `0x82DBB608` 为何为 0 | 已穷举生成代码写者（无独立创建者）；`onj=` 探针将给出实机取值，进一步要定位「本应由哪条 guest 初始化路径建立」 |
| 「显示完成事件源」补法 | 下一里程碑主课题：不再在门热路写会计，而是寻找/构造一个**与门频率解耦**的完成事件源（正确时机 = 每帧一次、或由呈现/交付链路本身驱动），并保证门之后的完成链（`sub_82881E00` → 交付 → 对象更新 → `sub_8287BF88`）具备可运行前提（即 `0x82DBB608` 必须被建立） |
| `container+16`（`0x82E55E30`）指针写入者、`*(container+0)` 显示状态字段写入者 | 仍未定论 |

### 9.1 本轮两条「别再踩」的经验

1. **符号表必须与构建同批**：旧 `_syms_now.txt` 相对当前构建整体偏移 **+0x330**，据此得出的「崩在 `sub_8287D0F8`」是错的。凡解析 `imagen+<off>`，先用**当前**未剥离 ELF 导出符号表（`nm --defined-only --numeric-sort`）复核，再反汇编确认现场指令。
2. **「宿主代客写会计」这类修复必须问一句「这个函数会被以什么频率调用」**：挂在门热路上的写入等于把完成事件按门的调用次数伪造上百万次，且守卫的「非 0 才退」挡不住重入 ⇒ 回绕。任何写 guest 状态的补丁都应先固定调用频率上限，否则宁可只读观察。

---

## 10. 附：本包内相关文件

| 文件（zip 内路径） | 说明 |
|---|---|
| `T2ROTF.nro` | 本版（T31）构建，可直接覆盖到 SD 卡 |
| `T2ROTF.toml` | 本版默认值（`guard=true`、另四个 false） |
| `README.md` | 含第 3.8 节「v7.5 验证清单」与第 6 节「v7.5 变更记录」 |
| `recomp_src/renut_engine/gpu/d3d/hooks/device.cpp` | 改动后的 hooks 源码（103,185 字节） |
| `renut_device.cpp.bak.t29` | v7.4 基线（`device.cpp` 的 v7.4 版本，源码级回退用） |
| `T2ROTF_deko3d_启动失败双根因与守卫修复报告.md` | **本报告** |
| `T2ROTF_deko3d_显示驱动链取证与完成释放报告.md` | v7.4/T29（上一版，含判据链重建） |
| `T2ROTF_deko3d_显示完成侧代理与kDisplayCtx修正报告.md` | v7.3/T28 |
| `T2ROTF_deko3d_呈现门阻塞语句与显示会计核对报告.md` | v7.2/T25 |