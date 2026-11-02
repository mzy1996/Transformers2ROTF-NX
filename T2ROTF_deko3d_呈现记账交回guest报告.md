# T2ROTF（Xbox 360《变形金刚2：卷土重来》）Switch 移植 — 呈现记账「交回 guest 自己」报告

- 版本标记（判版本只认日志里这一行）：`[video] T2ROTF BUILD v8.6-FLIP (…)`
- 本轮对应待办：**T61（离线取证完成/退役链）+ T62（重建、重打包、交付）**
- 上一版：v8.5-COMPLETE（`recomp/out/sw8/T2ROTF.nro` = 59,941,618 字节 / `0fa8e1fed0236cef3cf5c183d9e1b9a628284d2fc35c9c0cab4da688248c46d3`）

---

## 0. 一句话

v8.5 押注的「把 Graphical ISR 的 **source=1（提交完成）** 分支派发进 guest」经本轮**离线逐指令精读证伪：那一支是空操作**；同时找到了真正的缺口——**guest 自己的呈现完成记账函数 `sub_822D9688` 零静态调用者**（它的原生调用者被宿主 Swap 钩子整体替换掉了），于是 `*(dev+16564)` / `*(dev+16568)` 与 `*(*(dev+10900)+4)` 从第 1 帧起就冻住。v8.6-FLIP 的做法是把记账**交回 guest 自己**：宿主每个垂直同步周期以 `r3=0` **代客调用** `sub_822D9688`，字段全部由 guest 代码在它自己的自旋锁内写，宿主**不写任何 guest 字段**。

---

## 1. 本轮离线定论（全部有生成代码行号可核）

| # | 定论 | 证据 |
|---|---|---|
| D1 | `sub_822D47A0` 的 **`r3==1`（提交完成）分支是空操作**：只做「`*(blk+16) != 0` 才 `bctrl *(blk+16)`」+ `KeAcquireSpinLockAtRaisedIrql(dev+10904)` 后 `*(blk+0) &= ~(1 << *(r13+268))` + 释放锁。**不读写 `base+284`/`base+288`，不碰依赖表 `+24/+28`。** 而 `*(blk+16)` 在本移植里从未被写非 0（日志 `blk0=0 blk4=0`）⇒ 该分支等于直接返回。 | `recomp/generated/default/T2ROTF_recomp.9.cpp:21582-21664` |
| D2 | **显示完成/vblank 的真入口是同一回调的 `r3==0` 分支**：`REX_MM_LOAD_U32(0x7FC86544)` → `clrlwi. r11,r11,31` → 为 0 直接退出；非 0 才 `bl 0x822d9570`（`sub_822D9570(dev)`）。即**该 ISR 是否干活由 guest 写在 MMIO `0x7FC86544` 的 bit0 决定**。 | 同文件 `:21654-21664`；`0x7FC86544` 全工程仅此一处读 |
| D3 | `sub_822D9570`（显示/vblank 记账）**已在跑**：它每拍 `*(dev+16548) += 1`，实机该字段从 0 涨到 2738。但它排空的换页环**恒空**（实机 `ringR16700 = ringW16704 = 0`）⇒ **永远进不了环体** ⇒ `*(dev+16568)` 不涨、`*(*(dev+10900)+4)` 永不清零。 | `T2ROTF_recomp.24.cpp:21310-21444`；实机日志 `a16700=a16704=0` |
| D4 | **唯一生产者 `sub_822D9688` 零静态调用者**：全工程只有 `T2ROTF_init.cpp:3890` 注册，**没有任何 `bl 0x822d9688`**。它的原生调用者（guest 自己的 Swap/提交路径）在本移植里被宿主 `D3DDevice_Swap_hook` 整体替换，而 Swap 第 1 帧后就停了（`[T2DIAG SWAP n=1]`）。 | `T2ROTF_recomp.32.cpp:20832-21056`；`T2ROTF_init.cpp:3890`；`device.cpp:1255-1354` |
| D5 | 宿主那套 `T2AdvanceDisplayAccounting()` 是**对同一批 guest 字段的宿主重实现**（`word(16564) += 1`、`word(16568) = …`、`*(blk+4) = 0`），属本工程明令禁止的「写显示会计字段」，且**只在 Swap 时被调** ⇒ Swap 停即冻结（实机 16564/16568 冻在 **5**）。 | `device.cpp:926`（定义）、`:1345`（调用点） |

`sub_822D9688(r3=0)` 的语义（供判读对着实机数字核）：取 `dev+16712` 自旋锁 → `*(dev+16564) += 1` → 由 `r3` 拆出 `r29=(r3>>24)&0xF` / `r27=r3&0xFF` / `r30=r3&0xFFFFF000`（页对齐表面）→ `r11 = *(16556) + r29` 并夹到 `*(16548)` → 若 **`r11 == *(16548)` 或 `*(16564) == 1`** 则走 `loc_822D97E4`：**`*(dev+16568) += 1`**，且 `r30 != 0` 时 `MMIO 0x7FC86110 = r30`、**否则 `*(*(dev+10900)+4) = 0`**；否则把 `(r30, r11)` 压入换页环（`idx = *(16704) & 0xF`，`*(16572+idx*8)=r30`、`*(16576+idx*8)=r11`、`*(16704) += 1`）。**`r3=0` ⇒ `r29=r27=r30=0`** ⇒ 要么当拍直接 `16568++` 且 `*(blk+4)=0`，要么压环后由下一拍的 `sub_822D9570` 环排空完成（**晚一拍**）。两条路都以 `*(*(dev+10900)+4) = 0` 收尾，正是 v8.5 实机缺的那一步。

---

## 2. 为什么 v8.5 的「派发 source=1」会落空

v8.5 的推理链是：**排空 `while (*(base+284) + *(base+288) != 0)` 需要一个显示完成 → 显示完成应由 Graphical 中断的「提交完成」支路产生 → 所以派发 `source=1`**。

本轮精读把这条链的两端都推翻了：

- 出口端：`source=1` 分支（D1）**不 retire 任何在飞显示项**，它只清 `*(blk+0)` 里当前 CPU 的在飞位，而 `*(blk+16)` 从未被写非 0 ⇒ 派发一百次也等于零。
- 入口端：真正的记账者（D4/D5）**根本没人调用**：guest 侧零静态调用者，宿主侧那套只在 Swap 时跑。

⇒ 这解释了我们看到的实机现象：`vblanks`/`irq`/`hostcb` 全增长（派发链本身没问题）、`v16548` 涨到 2738（`sub_822D9570` 在跑）、但 **`v16568` 冻在 5、`w10900_0/4 = 0/0`、`ringR16700 = ringW16704 = 0`**、驱动线程 `dentro=1` 卡死、提交侧零产出、呈现侧永等 `F8000068`。**不是中断没送到，而是没人记账。**

---

## 3. 本轮改动（4 处代码 + 1 键 + 版本标记）

1. **`recomp/src/t2rotf_gpu_signal.cpp`（68,966 → 74,911 字节）**：新增成员函数 `PresentarGuest(uint32_t cpu)`——cvar 守卫 → 线程状态守卫 → `Dispositivo()` + **设备双槽核对**（`Leer32(kSlotOtro)` 与 `Leer32(celda)`，任一等于 `dev` 才继续，否则 `saltados++` 并只报一行）→ `ctx->r3 = ctx->r4 = ctx->r5 = 0` → `function_dispatcher_->ExecuteTrap(ts, kPresentarGuest, args, 0)`（`rc != 0` 记 `saltados`）→ 限频打 `[video] T2DIAG PRESENT n=… dev=… 16564=… 16568=… 16548=… ringW16704=… blk10900=… blk4=… saltados=…`。**读设备槽用的是 v8.2 的 shadow 安全读，绝不直读 guest 内存；宿主不写任何 guest 字段。**
2. 同文件：新增常量 `kOffDevCompletados64 = 16564`、`kPresentarGuest = 0x822D9688`。
3. 同文件：`BucleVblank` 里在 `Interrupcion(0, cpu); Completado(cpu);` 之后新增 `PresentarGuest(cpu);`——**放在垂直同步派发线程内**（与既有 `source=0` 中断派发同一线程、同一时机；v8.4 实机已证明该线程能持续运行，而独立驱动线程会长期卡在 guest 提交内部）。
4. 同文件：新增 cvar **`sa2_renut_display_present_guest`（默认 true）**、`kInitOnly`，附 3 条定论注释；版本标记改为 **`[video] T2ROTF BUILD v8.6-FLIP (…)`**。
5. **`recomp/src/renut_engine/gpu/d3d/hooks/device.cpp`（103,645 字节）**：`T2AdvanceDisplayAccounting(device_va, completed_swap);`（`:1345`）改为 `if (REXCVAR_GET(sa2_renut_display_agent)) …` ⇒ **默认 false，宿主不再直写显示会计字段**（字段负责人交回 guest）。
6. **`T2ROTF.toml`（40,124 → 42,369 字节，23 → 24 键）**：新增 G16 段与 `sa2_renut_display_present_guest = true`。
7. **未改动**：`recomp/src/t2rotf_gpu_signal.h`、`recomp/src/t2rotf_video_nativo.cpp`（逐字节与包内一致）。

---

## 4. 产物与校验

| 产物 | 字节数 | sha256 |
|---|---|---|
| `recomp/out/sw8/T2ROTF.nro` | 59,949,810 | `93e44c13ede5fe8ded03f6c9528690806ca73103e5021ca2d763bf8c90408b47` |
| `T2ROTF.toml` | 42,369 | `6217dfa41e43926c3dde0bcf0653611832d4b9c0f6f72ca402bcf7565b84eecc` |
| `recomp/src/t2rotf_gpu_signal.cpp` | 74,911 | `7ac91ed452a035b873970eb7689215e2b80ddbbabecc30ea9cb989aa989573b7` |
| `recomp/src/renut_engine/gpu/d3d/hooks/device.cpp` | 103,645 | `3329f920c5b2dcaf9f10a309fcf520dd6c6d09eb9ece6469e1ae1c7c1de177ed` |

- 构建：`recomp/out/sw8` 执行 `ninja -j4 T2ROTF` ⇒ **退出码 0**；再跑一次为 no-op（`Codegen summary: 0 written, 0 unchanged, 0 deleted, 1 module(s) up to date`，NRO 的 mtime/sha256 不变）⇒ 构建已完整收尾且可复现。
- 字符串取证（`strings -a` 计数）：`v8.6-FLIP` = 2、`sa2_renut_display_present_guest` = 4、`T2DIAG PRESENT` = 1、`822D9688` = 2、`代客呈现记账暂不调用` = 1。（NRO 里仍有 `v8.5-COMPLETE` = 2，来自旧 cvar 帮助文本与历史判读行，属预期，**不要用它判版本**。）
- 配置：`T2ROTF.toml` 经 `tomllib` 解析通过，**24 键**读回正确（`submission_present_guest` 实读 `sa2_renut_display_present_guest = true`、`hostcb_s1 = true`、`hostcb_s1_mode = 2`、`display_agent = false`、`display_release = false`）。

---

## 5. 开关语义与回退阶梯

- 本轮唯一新增开关：**`sa2_renut_display_present_guest`（默认 true）** = 每个垂直同步周期以 `r3=0` 代客调用 `sub_822D9688`（呈现完成记账），字段由 guest 自己写。
- 隐含改动：宿主 `T2AdvanceDisplayAccounting()` 只有在 `sa2_renut_display_agent = true`（默认 **false**）时才执行。

回退（**都是 `kInitOnly`，改完必须完全退出再启动**）：

1. `sa2_renut_display_present_guest = false` → 退回 v8.5-FLIP 前行为（记账重新无人调用，16568 会再冻）；
2. 再把 `sa2_renut_gpu_signal_hostcb_s1 = false` → 退回 v8.4-SPLIT 语义（提交完成中断重新被抑制——本轮已证其为空操作，故这一步只作 A/B）；
3. 再退 `sa2_renut_display_drive_thread = false` → v8.3-FRAME（**实机会自锁**，仅作对照）；
4. 再退 `sa2_renut_display_drive = false` → v8.2-SHADOW；再退 `sa2_renut_gpu_signal_dev_hook = false` → v8.1；再退 `sa2_renut_gpu_signal = false` → v7.7。

---

## 6. 实机验证步骤与需回传日志行

1. 把新的 `recomp/out/sw8/T2ROTF.nro` + `T2ROTF.toml` 覆盖到 `sdmc:/switch/Transformers2ROTF/`，**完全退出再启动**。验收场景是**不超频手持**（「不超频 60 帧」结论只在**不超频**条件下给）。
2. 最关键验收点（按优先级）：
   - `[video] T2DIAG PRESENT n=… dev=… 16564=… 16568=… 16548=… ringW16704=… blk10900=… blk4=… saltados=…` 是否出现、`n` 是否随刷新周期增长；
   - **`16568` 是否脱离 5**、**`blk4` 是否由非 0 变 0**（后者是「提交完成字被 guest 自己清掉」的直接证据；若走换页环，会**晚一拍**才清）；
   - `[T2DIAG SWAP n=]` 是否 > 1、`mmio_w`/`inKick13232`/`ringR16700` 是否开始增长；
   - `[T2DIAG DRIVE …]` 的 `dentro` 是否回到 **0**、`fin=`/`disp16568` 是否增长、`espera sub_824F3580` 是否停止单调增长；
   - `rex_perfil.log` 的 `juego … fps` 是否 **> 0**、**画面是否出现**三连 logo。
3. 需回传日志行：`[video] T2ROTF BUILD v8.6-FLIP …`；**全部** `[video] T2DIAG PRESENT …`；`[video] T2ROTF GPSIG: v8.6 代客呈现记账暂不调用：…`（若有 ⇒ 设备槽核对没过）；`[T2DIAG GPSIG …]` 与 `[T2DIAG GPSIG2 dev=…]`（首末各一，重点 `A0..C2`、`dv10904`、`blk0/blk4`）；`[T2DIAG DRIVE …]`（首末各一）；`[T2DIAG SWAP …]`（末条）；`[T2DIAG PERRO …]` 的 `fin=`；`rex_perfil.log` 的 `juego … fps` **与** `relojes:` 行（须为未超频值）；`rex_crash.log`（若产生）。

---

## 7. 判读分支（下一轮照此走）

| 分支 | 现象 | 结论与下一步 |
|---|---|---|
| A | `16568` 涨 + `blk4` 归 0 + `dentro` 回 0 + `mmio_w`/`SWAP n` 变 + fps>0 + 画面出现 | 缺环补上，进入「不超频实测帧率」 |
| B | `16568` 涨、`blk4` 归 0，但 `cola284`/`base+284` 仍恒 1 | 在飞显示项的**释放者另有其人**（下一轮冲 guest 的 GPU 提交完成路径 `sub_822D9838`/`sub_822D98E8`，以及 `VdSetSystemCommandBufferGpuIdentifierAddress` 注册的地址是否就是 `blk+4`） |
| C | `[T2DIAG PRESENT]` 不出现，或 `saltados` 持续增长 | 设备双槽核对没过（回传 `GPSIG2` 首末行） |
| D | 产生 `rex_crash.log` | 回传 `pc/far/esr` 前 40 行 |

---

## 8. 残余风险与下一轮方向

- **本版尚未实机验证**：本轮全部改动只经过逐指令离线精读 + 构建 + 产物字符串取证。
- `table[slot]+24/+28` 与 `base+284` 的**释放者仍未定位**（候选：guest GPU 提交完成路径轮询 `blk+4` 之后的那段）。
- `sa2_renut_gpu_signal_hostcb_s1` 默认仍为 true（本轮已证其为空操作）；实机对照时可置 false，但**不影响** v8.6 的修复效果。
- `sub_82881F00`/`sub_82881F58`/`sub_828823A0/A8/B8` 尚未逐条判读完毕；`sub_82881848` 等待的句柄在 `0x82CFB5E0`（`F8000068`）与 `0x82DBB5A0`（`F8000060`）两套地址族的疑问仍待钉死。
- **T26（不超频实测帧率）仍被实机阻塞**；「不超频流畅 60 帧」**仍未达成也未实测**。
- TU 升级档与 DLC 包在产物层已就绪（工程内 `update/`、`dlc/`、`patched/default.xex` 15,937,536 字节 > base 14,200,832 字节），**实机闭环验证同样待画面出现后进行**。