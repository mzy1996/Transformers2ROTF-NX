# T2ROTF deko3d 版：影片链停摆修复 + 显示会计落地 + 停摆取证插桩报告

- 日期：2026-10-27
- 产物：`recomp/out/sw8/T2ROTF.nro` = **59,826,930 字节**，sha256 `a3004dd56c87cab3892d24519a986a07eb65705660e33fa27b0b41559736539f`
- 上一版（2026-10-26）`59,810,546 字节` / `eda7708b125987a985e8a8740d2dea072f44a26b4bc8b32477c62e6ba955a072`
- 目录：本报告在工程根目录，同版副本在 `_pkg_v635/`；工程包 `T2ROTF_switch_port.zip` 已原地重打
- 后端：Renut / deko3d（`REXGLUE_SWITCH_NO_NVK`，产物不链接 Vulkan 驱动）

---

## 1. 本轮范围与结论摘要

上一轮（T21，日志组 `rex_perfil(24)/rex_stderr(60)/T2ROTF_001(58)/T2ROTF_002(39)/deko3d(1)`）的判读结论是：

1. **启动崩溃已彻底消除** —— T17/T18 的 `VdGlobalDevice` 双槽修复实测生效（`procType=1`、`0x820011ac`→`0xffcae000`、`0x820011b0`→`0xffcad000`），`Unhandled guest access violation` 0 次，`0x00002A88` 消失；deko3d 侧 13 行日志全成功，987 条着色器包加载正常、`MissingGameShaders` = 0。
2. **屏幕仍黑** —— 影片链解码 6 帧后停摆：`BOMBA fotograma 1..4`、`etapa1/2/3` 各 6 次且每次都 `ret=0`、`HOOK ENTER sub_82850830` 87 次，两份日志尾部都停在 `etapa3 n=6 ... ret=0` 之后约 10 s 无新行而进程仍活；`PRESENTAR sub_827CADC0` = 0、`flip` = 0（由 5 条 `gamma ramp at scan-out` 反推 guest 约调用 Swap 5 次、宿主呈现 5 帧）。

本轮把上述两条**静态取证结论落地为代码**，并补上"停摆点在哪里"所需的运行时取证设施：

| 项 | 内容 |
| --- | --- |
| 呈现门 | `BombaTick` 里替 guest 置 `[ctx+3916] = 1`（幂等），打开 `sub_827CE598` 的呈现分支 |
| 显示会计 | 宿主 `Swap` 钩子按 T2 真实字段推进 `16548/16552/16564/16568/16556`，清 `*(dev+10900)+4`，置 `10944 |= 0x80` |
| 错误发布 | 删除 `hooks/device.cpp` 中把"已显示"发布到 `device+16976` 的语句（T2 全库 0 命中） |
| 停摆取证 | 看门狗线程 + 泵前 4 个钩子 + `LatidoVencido()` 门槛收紧 + 栈采样默认开启（可关） |

**本轮交付的是"修复已落地并可判定"的一版**：构建退出码 0、产物指纹更新、插入的诊断与会计代码经字符串/符号/反汇编三层取证确认进入产物。**停摆是否真被解除、`sub_827CC058` 首次运行是否安全、60 帧是否达标，都必须在实机复跑中判定**（见 §7 验证清单）。不超频 60 帧（T19）仍待实测。

---

## 2. 判读依据（T21 静证据链，本轮行动的出发事实）

### 2.1 泵 `sub_827CE598` 的呈现门

泵全文（n=90，无静态调用者，经 `[ctx+15808]` 间接调用）读出为：

```
bl 82B506F0                              # 前置 1
lhz  r10,16004(r31); rlwinm r30,r10,31,1,31
lwz  r3,2216(r11); bl 82B8E128            # 前置 2
bl 82B507D8 / 82B86000 / 82B68458        # etapa1 / etapa2 / etapa3（任一非 0 → 827CE6F8 早退）
lwz  r11,3916(r31); cmpwi 0; beq 827CE6A8 # ★ 呈现门：[ctx+3916] == 0 时整段呈现被跳过
lwz  r11,15504(r31); cmpwi 7; bne 827CE670
bl   827CC058 / 827CC6E0                 # ← 15504 == 7 与否二选一
bl   827CADC0                            # 呈现步（9 参：r3=ctx、r4=ctx+15952、r5/r6/r7=显示平面、r8=0、r9=r30）
写 15592 / 15596 / 15568；bl 82B8E470；li r3,0
```

- `[ctx+3916]` 的 **4 个写入点**：`827A0C90+138`、`8279E3C8+189`、`82799548+231`（三者都是**从码流逐位读字段后写回对象**，末尾同写 `3912`、`3924`=1、`3680/3684`、`22292..22304`）与 `827B25A8+21`（**复位清零**）。
- 对照情形：本移植用 FFmpeg 重做了解码与注帧（`t2rotf_video_nativo.cpp`），**guest 自己的解码链不再跑**，因此这些"从码流头部解析"的写入点不会执行 ⇒ `[ctx+3916]` 恒 0 ⇒ 呈现分支永不被选中 ⇒ `PRESENTAR` 恒 0。**这就是"解码 6 帧后停摆"的候选主因。**

### 2.2 宿主显示发布地址错

- `hooks/device.cpp` 原把 swap 完成发布到 `device_va + 16976`；**`16968/16976` 在 T2 镜像全库 0 命中**（MGR/SA2 遗留偏移）。
- T2 的真实显示记账槽位（由 guest 的垂直同步处理程序 `sub_822D9570` 与呈现统计 `sub_822D9688` 维护）：`16548`(vblank 计数)、`16552`(最近 vblank/帧时，mftb)、`16556`(上次发布值)、`16560`(提交/交换计数)、`16564`(帧计数)、`16568`("已确认显示"帧计数)、`16572/16576`({值, vblank 目标} 环)、`16700/16704`(环读/写索引)、`16540/16544`(帧统计回调指针)、`*(dev+10900)+4`(guest 可见的帧完成字，`sub_822D9570`/`sub_822D9688` 写 0)。
- 宿主未模拟 GPU 中断 ⇒ guest 的 `sub_822D9570` 不会被触发 ⇒ `16548/16568` 永不推进 ⇒ guest 侧"显示完成"判断永远不成立。**本轮由宿主在每次 Swap 时代跑这段记账。**

---

## 3. 本轮改动清单

### 3.1 `recomp/src/t2rotf_video_nativo.cpp`（14 处）

| # | 改动 | 依据 |
| --- | --- | --- |
| 1 | `BombaTick()` 内在 `g_ultimo_valido` 之前写 `Escribir32(base, ctx_addr + 3916, 1)` | §2.1 呈现门；注释里逐行记录了 `sub_827CE598` 的汇编依据 |
| 2 | 新增全局 `g_base_host`/`g_ctx_visto`/`g_dev_visto`、计数 `g_pump_entrada/g_pump_salida/g_pre1/g_pre2/g_post470/g_prep/g_cadena937/g_cadena9bf/g_despacho/g_yield_total`、`g_yield_lr[1024]`/`g_yield_lr_n[1024]` | 看门狗与 LR 直方图 |
| 3 | 泵钩子：捕获 `base`/`ctx`、进出计数、首次调用即 `ArrancarPerro()`、`Turno(0)` 时 `VolcarGuest("pump")`（取代原"每 60 趟"门槛） | 静默期必须靠时间限频才能看到数据 |
| 4 | `LatidoVencido()` 调用门槛 `0x3FFFFu`（≈26 万次）→ `0xFFFu`（4096 次） | 原门槛只在从未进入的 ring 钩子内自增 ⇒ 计数器在日志里完全不可见 |
| 5 | 新增 `REX_EXTERN` + 4 个钩子：`sub_82B506F0`(pre1)、`sub_82B8E128`(pre2)、`sub_82B8E470`(post)、`sub_824F0620`(yield，只记 LR 直方图) | 泵的三段前置与尾部；全程序唯一 yield 点 = `sub_824F0620`（`0x82B4E2C4 → NtYieldExecution`，`SwitchToThread()` 包装） |
| 6 | 新增 `VolcarGuest(etiqueta)`：27 个计数器 1 行 + ctx 17 个字段 1 行 + dev 19 个字段 1 行 + YIELD Top6 1 行 | 见 §5 字段表 |
| 7 | 新增 `BuclePerro()` / `ArrancarPerro()`：pthread 起看门狗（栈 0x10000），首行 3 s 后每 2 s 转储 | Switch 上 `std::thread::detach()` 会抛异常（`banjo_renut_tuning.h:95-96`、`pipeline.cpp:904`），故用 pthread；失败回退 `new std::thread`（永不 join） |

### 3.2 `recomp/src/renut_engine/gpu/d3d/hooks/device.cpp`（3 处）

| # | 改动 | 依据 |
| --- | --- | --- |
| 8 | 新增 `T2AdvanceDisplayAccounting(device_va, completed_swap)`：`16548++`、`16552 = 毫秒`、`16564++`、`16568 = completed_swap ? completed_swap : 16568+1`、`16556`（`completed_swap == 0` 时）、读并清零 `*(*(dev+10900)+4)`、`10944 \|= 0x80`，并有 2 s 限频的 `RGPU_WARN("[renut] T2 display accounting: ...")` | §2.2；字段语义逐条来自 `sub_822D9570` / `sub_822D9688` 的反汇编 |
| 9 | Swap 钩子在 lambda 尾部与 heading 各调用会计；新增 2 s 限频 `RGPU_WARN("[renut] T2DIAG SWAP n=… dev=… front=… sub16560=… disp16568=…")` | 需要 guest 侧提交计数与"已确认显示"计数的实测值 |
| 10 | **删除**对 `device_va + 16976` 的错误发布 | T2 全库 0 命中 |

> 关于原任务描述里的"卸掉 Swap 在飞深度门 `21624/21628/21632`"：核对结论是**无需改动**——该深度门只存在于 guest 代码（`sub_822D9D70` 尾部），而宿主 Swap 钩子已整体替换了 guest 的 `D3DDevice_Swap` 本体（钩子不调用原体），grep 全 `renut_engine/gpu/` 对该门零命中，**宿主侧没有可卸的对象**。

### 3.3 `repos/nfsmw-nx/sdk/src/ui/switch_perf.cpp`（2 处）

| # | 改动 | 依据 |
| --- | --- | --- |
| 11 | `bool sample_stacks = false;` → `true`（注释标明"需要 Game Master 的自旋 PC"） | T21 日志里 `Muestreo de pilas: desactivado` ⇒ 拿不到 Game Master（99.4% CPU）的自旋 PC，这是 T23 的最大证据缺口 |
| 12 | 新增 `StackOffPath()`（日志目录下 `perfil_pilas.off`）并在启动时判定：**存在即关闭采样** | 采样是侵入式的（1 ms 停线程取栈），帧率实测（T19）必须先关掉，否则测得的数字被采样扰动 |

采样报告写入 `<NRO 目录>/logs/rex/rex_perfil.log`（含 `Muestreo de pilas: activo (1 ms)` 与 `pila …%` 热点栈、线程等待栈）。

---

## 4. 构建与产物取证

### 4.1 构建

```
cd "J:/XBOX360 port/Transformers Revenge of the Fallen/recomp/out/sw8" && ninja -j4 T2ROTF
```

- 结果：**EXIT = 0**。可见步骤：`switch_perf.cpp.obj`（SDK 树）→ `device.cpp.obj` → `Generating recompiled code for T2ROTF`（`0 written, 0 unchanged, 0 deleted, 1 module(s) up to date`，44.6 s）→ `t2rotf_video_nativo.cpp.obj` → `Linking CXX executable T2ROTF; Packaging T2ROTF.nro`。
- 产物：`T2ROTF.nro` = **59,826,930 字节**，`2026-10-27 06:06`，sha256 **`a3004dd56c87cab3892d24519a986a07eb65705660e33fa27b0b41559736539f`**
  （同一轮先前落地"会计 + 插桩"时得到 `a220d6ce44bcf1f00544575611f64adbfee922a77dc0d851b164e66de9779168` = 59,826,930 字节；加上采样关闭开关后重链得本版 `a3004dd5…`，**大小为交付版**）

### 4.2 字符串取证（NRO 内命中次数）

| 串 | 次数 | 含义 |
| --- | --- | --- |
| `perfil_pilas.off` | 1 | 采样关闭开关已进入产物 |
| `T2DIAG PERRO` | 1（模板串；实际出现 4 处调用点） | 看门狗转储 |
| `pumpIn=` | 1 | 泵进出计数行 |
| `T2DIAG pre1` | 1 | 泵前钩子 |
| `T2DIAG YIELD total=` | 1 | yield LR 直方图 |
| `T2DIAG SWAP n=` | 1 | Swap 节流日志 |
| `T2 display accounting` | 1 | 显示会计节流日志 |
| `GLSL package` | 1 | 987 条着色器包仍在 |

### 4.3 符号取证

- `__imp__sub_82B506F0` / `__imp__sub_82B8E128` / `__imp__sub_82B8E470` / `__imp__sub_824F0620` 与对应的强符号钩子 `sub_82B506F0` / `sub_82B8E128` / `sub_82B8E470` / `sub_824F0620` **成对存在**。
- 生成代码侧调用点为**直接调用**（`T2ROTF_recomp.3.cpp:85618 sub_82B506F0(ctx, base);`、`T2ROTF_recomp.11.cpp:81221 sub_824F0620(ctx, base);`），无 `__imp__` 前缀 ⇒ 强符号覆盖机制生效（与既有可用的泵钩子 `sub_827CE598` 同机制），四枚新钩子确实会被触发。
- `T2AdvanceDisplayAccounting` 被内联（无独立函数符号），其静态局部 `avisos`(0x3d68ea0) / `ultimo`(0x38987c0) 与 `D3DDevice_Swap_hook` 的 `n_swaps`(0x3d68e70) / `ultimo`(0x38987b0) 均在。

### 4.4 反汇编取证（本轮最关键的归属判读）

1. **会计代码确实在 Swap 的 std::function lambda 内**（`_ZNSt17_Function_handlerIFvvEZN12_GLOBAL__N_1L19D3DDevice_Swap_hookEjjjEUlvE_E9_M_invoke…` @ `0x2dbd3b0`）：

   | 反汇编 | 含义 |
   | --- | --- |
   | `2dbd490: ldr x1,[x23]` + `2dbd498: add x25, x1, w22, uxtw` | `x25 = 宿主基址 + device_va`（guest 内存平坦映射） |
   | `2dbd49c: add x23, x25, #0x4, lsl #12` | `x23 = x25 + 0x4000`（**16548/16552/16556/16564 超 `ldr` 的 12 位偏移上限，故用基址 + 小偏移寻址**） |
   | `2dbd4a0: ldr w0,[x23,#164]` → `rev`/`+1`/`rev` → `str` | **16548（vblank 计数）自增** |
   | `2dbd4ec: str w2,[x23,#180]` | **16564（帧计数）自增** |
   | `2dbd4f0: str w0,[x23,#168]` | **16552 = 毫秒**（帧时） |
   | `2dbd4f8–508: ldr/str w24,[x23,#184]` | **16556（上次发布值）**，仅在 `completed_swap == 0` 时自增 |
   | `2dbd50c: ldr w24,[x25,#10900]`；`2dbd51c: add x25,x25,#0x2,lsl #12`；`2dbd520: ldrb w0,[x25,#2752]`；`orr #0xffffff80`；`strb` | 读 `*(dev+10900)`；**10944 \|= 0x80** |
   | `2dbd764–77c: ldr w24,[x0,#4]` + `str wzr,[x0,#4]` | **读并清零 `*(*(dev+10900)+4)`（guest 可见的帧完成字）** |
   | `2dbd55c: cmp x2, #0x7cf`（`0x7CF = 1999`） | 2 s 限频打印 |

   > 判读要点（供后续轮次复用）：**不要再只用"立即数 `#0x40a4`（=16548）"在反汇编里找 16548 的访问**。16548 超出 `ldr`/`str` 的 12 位偏移编码上限，编译器会先造一个 `base + 0x4000` 的中间指针，再用 `[x23, #164]`（0x4000+164=16548）、`[x23, #168]`（16552）、`[x23, #180]`（16564）、`[x23, #184]`（16556）访问。首轮用 `, #0x40a4` 全量反汇编检索时本函数"零命中"，正是这个编码陷阱造成的**假阴性**。

2. **`16976` 的错误发布已确认移除**：全 ELF 中 `mov x?, #0x4250`（16976）共 6 处（`0x4a2f6c` / `0x4f7804` / `0xe56d70` / `0x12e895c` / `0x1edf950` / `0x22080ec`），**全部不在 Swap 钩子区间（`0x2dbd950–0x2dbe380`）**。
3. **字段图的第三方印证**：`0x200830c`(16548) / `0x20085e8`(10900) 落在 `__imp__sub_822D9688`/`sub_822D9688`（**guest 自己的呈现统计函数**）体内——guest 用它自己的代码在读写同一组字段，与 §2.2 的槽位表完全一致，可视为字段语义的独立佐证。

---

## 5. 新日志字段与判读方法

### 5.1 看门狗与泵（前缀 `[video] T2DIAG`，落 `logs/rex/rex_stderr.log`）

| 行 | 内容 |
| --- | --- |
| `T2DIAG PERRO hilo lanzado (cada 2 s)` | 看门狗线程启动确认（首行在其后 3 s 出现） |
| `T2DIAG PERRO perro` / `perro`（每 2 s） / `pump`（泵每秒一行） | 转储触发点 |
| `… pumpIn= pumpOut= pre1= pre2= post470= prep= cadena937= cadena9bf= despacho= etapa1/2/3= fin= bomba= iny= pres= anillo= externo= irq47= irq95= cc058= cc6e0= yield=` | 27 项计数（`cc058`/`cc6e0` = `827CC058`/`827CC6E0` 是否被调到，本轮新增钩子） |
| `T2DIAG PERRO ctx= st15504= g3912= g3916= g3924= v15592= v15596= v15568= v14856= v15228= v16004= pump15808= disY/U/V= off220= off224=` | ctx 17 项（**`g3916` 应为 1**、`v15592` 应为非 0 = "本帧已呈现"） |
| `T2DIAG PERRO dev= vblank16548= tick16552= sub16560= fr16564= disp16568= ringR16700= ringW16704= v23376= f10940..10944= inKick13232= b10896= b10900= w10900_0= w10900_4= w10896_0=` | dev 19 项（**`vblank16548`/`fr16564`/`disp16568` 应随 Swap 增长**、`w10900_4` 应为 0） |
| `T2DIAG YIELD total= top=[LR xN] ×6` | 全程序唯一 yield 点 `sub_824F0620` 的调用者 LR 直方图 Top6（**用于定位哪个调用点在自旋**；`(lr>>4)&1023` 分槽） |
| `T2DIAG pre1 … n= r3= r4= lr=` / `T2DIAG pre2 …` / `T2DIAG post sub_82B8E470 …`（各前 16 次） | 泵的三段前置/尾部是否被走到、入参、调用者 |
| `[renut] T2DIAG SWAP n= dev= front= sub16560= disp16568= …`（2 s 限频） | 宿主 Swap 次数与 guest 侧计数对照 |
| `[renut] T2 display accounting: dev= vblank16548= sub16560= fr16564= disp16568= blk10900= w10900_4= f10944=`（2 s 限频） | 会计推进实况 |

打印门语义（沿用 T22 定论）：`etapa1/2/3` = `n<=16`（日志里的 n 即真实调用数）；`PRESENTAR sub_827CADC0` = `n==1 || %120`；`SALUD` = `%60`；`BOMBA` = `g_inyectados<=4 || %120`；`LatidoVencido` 现为 4096 次一次时钟、且间隔 ≥2 s。

### 5.2 栈采样（落 `logs/rex/rex_perfil.log`）

- 首行出现 `Muestreo de pilas: activo (1 ms)`，报告含 `pila x.x%` 热点栈与线程等待栈（含帧内地址，可用 `imagen+off = ELF VA` 映射回重编译函数）。
- 目标：取 **Game Master（T21 显示 99.4% CPU、优先级 0x3B、mask 0x1）** 在停摆期间的自旋 PC，与 §5.1 的 YIELD LR 直方图交叉定位"阻塞语句与字段"（T23 完成条件）。
- 关闭方式：日志目录放空文件 `perfil_pilas.off`（帧率实测 T19 用）。

---

## 6. 兼容边界与未决风险

| 风险 | 说明 | 处置 |
| --- | --- | --- |
| **强制 `[ctx+3916] = 1`** | 会让从未执行过的 `sub_827CC058`(417 指令) / `sub_827CC6E0` 首次运行。二者经反汇编确认为**纯计算叶子**（无 `lwarx/stwcx./sync`、除栈帧 helper 外无 `bl`、仅有 2 条计数/条件回边），不含阻塞原语；但其依赖的 ctx 字段在"guest 解码链未跑"的前提下是否就绪未知 | 本轮"让它照跑，由崩溃日志判定"；若崩溃，备选方案 = 在影片期给这两个函数挂钩子直接跳过原体 |
| 强制置位与 guest 语义的差异 | `3916` 本应由码流头部解析写入（同时会写 `3912`/`3924`=1 等） | 只置 `3916`；`3924` 若后续被读取判别，再按同样方式补 |
| 帧统计回调未调用 | `16540`/`16544` 两个回调指针（`mtctr; bctrl`，参数 `{16548@+80, 16568@+84, 0@+88}`），宿主会计**不调用**它们（仅观察） | 若 guest 侧统计缺失导致后续逻辑异常，再评估从渲染线程安全回调 |
| vblank MMIO 分支 | `sub_822D9570` 的 `addis r10,r0,32712` → `0x7FC860F0` + `eieio/sync` 分支未从宿主触发 | 只推进内存字段，不动 MMIO |
| 栈采样开销 | 默认开启（1 ms 停线程取栈） | 帧率实测前放 `perfil_pilas.off` |
| **未排除的次级隐患**（沿用上轮） | `hooks/draw.cpp` 的 `sub_822C68B8`(视口写入，取值 r3..r7)、`sub_822D8CD8`(ClearF，r3..r6/f1/r8)、`sub_822DB2D8`(Resolve，r3..r10，其中 `point_va=r7`、`color_va=r10` 是 guest 指针) 仍按参考端取参，未做 T2 ABI 逐函数核对；`D3DDevice_SetRingBufferParameters_hook` 直接返回 S_OK | 本轮未改（改动需先从反汇编导出 T2 真实 ABI）。若新日志出现"呈现步内崩溃"，优先核查这三处 |
| 停摆点归属 | 泵尾部 / `sub_8279BFC0`+`827937E8` 循环 / 泵前置 `82B506F0`·`82B8E128` 三个候选，静态无法再分辨 | 靠新钩子 + 看门狗 + YIELD LR 三组数据在实机分辨 |

---

## 7. 实机验证清单（请按序核对，日志请一并回传）

部署：把 `recomp/out/sw8/T2ROTF.nro` 覆盖到 `sdmc:/switch/Transformers2ROTF/T2ROTF.nro`（`game/`、`update/`、`patched/`、`dlc/` 无需变动）。

| # | 检查点 | 期望 |
| --- | --- | --- |
| 1 | 启动不崩 | `rex_stderr.log` 无 `Unhandled guest access violation`，无 `0x00002A88`，`rex_crash.log` 不新增 |
| 2 | 采样已开启 | `rex_perfil.log` 首行 `Muestreo de pilas: activo (1 ms)` |
| 3 | 看门狗活着 | `T2DIAG PERRO hilo lanzado (cada 2 s)` 出现，随后每 2 s 一行 `T2DIAG PERRO perro …`（**这是排除"进程静默"的关键**） |
| 4 | 呈现门已打开 | `T2DIAG PERRO ctx= … g3916=1` |
| 5 | 显示会计在推进 | `vblank16548` / `fr16564` / `disp16568` 随 Swap 单调增长；`w10900_4` 为 0；`[renut] T2DIAG SWAP n=` 与 `sub16560` 同步增长 |
| 6 | **影片链是否继续推进** | `PRESENTAR sub_827CADC0 n=` ≥ 1（或 `%120` 命中行）；`v15592` 非 0；`HOOK ENTER sub_82850830` 计数远超 87；`etapa1/2/3` 的 n 超过 6 |
| 7 | 画面 | 三连 logo（luxo → atvi → hasbro）与 `a0_intro_co.wmv` 上屏；`gamma ramp at scan-out` 次数远超 5 |
| 8 | 停摆点定位（T23） | `T2DIAG pre1/pre2/post` 是否停在某一段；`T2DIAG YIELD total=` 与 Top6 LR 指向哪个调用点；`rex_perfil.log` 里 Game Master 热点栈 |
| 9 | 若仍停摆 | 请回传 `rex_stderr.log`、`T2ROTF_001.log`、`T2ROTF_002.log`、`rex_perfil.log`、`deko3d.log`（`rex_crash.log` 若有） |

进阶（帧率 T19）：在日志目录放 `perfil_pilas.off` 后重跑，记录菜单与游戏内帧率。基准：不超频（本机可用上限 CPU 1785 MHz / GPU 921 MHz / 内存 1600 MHz），参考项目 `mgr-nx-deko` 在**超频**下菜单 55–60、游戏内 40–52 FPS。

---

## 8. 下一步

1. **T23 收口**：用本轮日志的 `YIELD` Top6 + 栈采样热点 + `pre1/pre2/post` 计数，落实"阻塞语句与字段地址"。
2. **T22 收口**：若 `PRESENTAR` 起量且画面出现，则停摆修复确认；若崩在 `827CC058`/`827CC6E0` 或呈现步内，按 §6 的备选方案（钩子跳过 / 核查 draw 三处 ABI）处理。
3. **T19**：关采样后实测不超频帧率，与 60 帧目标对照并给出优化项（内部分辨率 1024x576、`resolution_scale`、LTO/PGO 等候选）。

---

## 附：本轮产物与位置

| 文件 | 说明 |
| --- | --- |
| `recomp/out/sw8/T2ROTF.nro` | 59,826,930 字节 / sha256 `a3004dd56c87cab3892d24519a986a07eb65705660e33fa27b0b41559736539f` |
| `_pkg_v635/T2ROTF.nro` | 同上（工程包内已原地替换） |
| `T2ROTF_switch_port.zip` | 工程包（已原地重打，内含本报告与新版 NRO） |
| `T2ROTF_deko3d_VdGlobalDevice修复报告.md` | 上一轮（v7.0）根因与修复 |
| 本文件 | 本轮判读、改动、取证、验证清单 |