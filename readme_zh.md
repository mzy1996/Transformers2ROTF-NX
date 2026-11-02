> **中文版 README**（`readme_zh.md`）｜English version: [README.md](README.md)
>
> 英文 README.md 是面向 GitHub 的主文档（英文内容），本文件是它的中文对应版，并保留本工程
> 逐轮中文工作记录（第 3 节各版本验证清单与第 6 节更新记录）。两份文档互相链接。

# T2ROTF — Transformers: Revenge of the Fallen (Xbox 360) → Nintendo Switch

《变形金刚2：卷土重来》Xbox 360 版到 Nintendo Switch 的 ReXGlue 重编译移植工程。

- 运行平台：Nintendo Switch（自制系统，Homebrew Menu / 直接运行 NRO）
- 图形后端：**Renut / deko3d**（plume + deko3d + uam，Tegra X1 Maxwell 原生路径）——2026-10-26 起全面替换原 Mesa NVK/Vulkan 路径，构建产物不链接 Vulkan 驱动（`REXGLUE_SWITCH_NO_NVK`）
- 集成内容：独立目录布局 + 安装期 XexPatcher 融合（参考 UnleashedRecomp-NX）——`game/`、`update/`、`patched/`、`dlc/` 四目录独立，运行时通过 OverlayHostPathDevice 与 ContentManager 动态挂载；TU delta patch 在安装阶段由 `xex_patcher_cli` 融合为 `patched/default.xex`，不再运行时 ApplyPatch
- 性能目标：不超频，手持模式稳定 60 FPS（目标未变；v8.0 已补上被 Renut 移走的「显示完成信号源」——`0x7FC80000` GPU 寄存器窗 + 按刷新率派发 guest 图形中断；v8.1 又补上「谁来叫它」——**宿主代客派发 guest 自己的显示中断回调 `0x822D47A0`**，因为 guest 的 `VdSetGraphicsInterruptCallback` 注册点被 Renut 原生 D3D 设备路径旁路；v8.2 再把宿主侧所有 guest 访存改走 `RexGmShadowFor` 的 chunk shadow 别名，根治 v8.1「启动即 `Unhandled guest access violation`」的 fatal，并把设备指针解析改为「cvar > VdGlobalDevice 双槽（shadow 安全读）> D3D 钩子观测 r3」分层；v8.3 再补上「谁来回调 guest 自己的每帧显示提交」——宿主在垂直同步线程的每个真实刷新里经 dispatcher 走 guest 自己的 `sub_82880978`（无参、纯全局状态驱动），把 v8.2 实机暴露的「中断侧已通、每帧提交侧从未产出」补上——但 v8.3 实机把这次驱动放在**垂直同步派发线程**上，而 guest 的每帧提交链会走到排空判据 `while (*(base+284) + *(base+288) != 0)` 上等一个显示完成，本移植里这个完成的唯一来源正是同一条线程派发的显示 ISR ⇒ **同一线程自锁**，中断派发随之冻结（整链跑 6 帧后停摆），观感仍是黑屏；v8.4 遂把这次驱动**搬到独立宿主线程**（`T2ROTF Dirigir`），垂直同步线程从此绝不进入 guest 代码、只派发中断与打报告，即使驱动卡在 guest 提交内部也不再掐断中断派发；v8.4 实机证实中断派发确实恢复（`vblanks`/`irq`/`mmio_r` 持续增长、自锁解除），却暴露出新卡点：Graphical 中断回调的 **source=1（提交完成）** 分支在本移植里**一次都没派发过**（`skipS1 == vblanks`、`s1 == 0`、`dev+16568` 冻在 5），而它正是 guest 自己的 ISR 里唯一能 retire 掉在飞显示项的通路——不派发，`base+288` 就恒为 1，排空 `while (*(base+284) + *(base+288) != 0)` 永远转不出去，驱动线程当场卡死在 guest 提交内部（`dentro=1`）、提交侧零产出、呈现侧在 `F8000068` 上永等；v8.5 遂把这条中断**解除抑制**（默认开 + 修正暂存块判据：实机 `*(dev+10900)=0xBDF80080` 是 D3D 后端自己的 fence/暂存块，落在 `0xB…` GPU 物理段，被旧「必须落在 guest 对象区间」的判据一律拒发），让 guest 自己走完「retire 项 → 置位事件」的原生活儿；但 v8.5 实机依旧无画面，遂离线逐指令复核整条「完成 → 退役 → 记账」链，**证伪**了 v8.5 的前提：Graphical 回调 `sub_822D47A0` 的 `r3==1`（提交完成）分支其实是**空操作**——它只在 `*(blk+16)` 非 0 时才 `bctrl`，随后仅清 `*(blk+0)` 的在飞位并释放自旋锁，既不读写 `base+284`/`base+288`、也不碰 `entry+24/+28`（而 `*(blk+16)` 本移植从未被写非 0，实机 `blk0=0 blk4=0`）；真正会推进显示记账的是同回调 `r3==0` 分支经 MMIO `0x7FC86544 & 1` 门控调用的 `sub_822D9570`（它在跑：实机 `dev+16548` 0→2738），但它的排空环 `while (*(16700) != *(16704))` 恒空（`ringR16700 = ringW16704 = 0`），于是永不完成；而唯一会 `*(dev+16564)++` / `*(dev+16568)++` / `*(*(dev+10900)+4)=0` 的 guest 呈现记账函数 `sub_822D9688` **零静态调用者**——原生调用者被宿主 `D3DDevice_Swap_hook` 整体替换，宿主那套 `T2AdvanceDisplayAccounting()` 属被明令禁止的「宿主直写显示会计字段」且只在 Swap 时被调，于是 `dev+16564`/`dev+16568` 与 `*(*(dev+10900)+4)` 从第 1 帧起就冻住；v8.6 遂**把呈现记账交回 guest 自己**——每个垂直同步周期由宿主以 `r3=0` 代客调用 guest 自己的 `sub_822D9688`（先经设备双槽 shadow 核对，对不上就跳过并计数），`16564`/`16568`/`*(*(dev+10900)+4)` 全部在 guest 代码自己的自旋锁内写，宿主不写任何 guest 字段，同时关掉宿主那套会计直写。帧率仍待实机实测。v8.7 修复「本版启动后直接报错退出、没有任何加载」——中断回调 `sub_822D47A0` 的 guest ABI 是 `(r3 = 来源 0/1, r4 = 设备)`，旧钩子误把 r3 当设备写进 `g_dev_visto`，被 v8.5/v8.6 新增的每垂直同步 `source=1` 派发毒化成 `1`，泵/看门狗的转储随后把 `1 + 10896 = 0x2A91` 当 guest 地址裸读触发 xmemory 致命路径；本版改为「设备只取 r4 且校验通过才落盘」+「转储里任何 guest 地址先校验再读」（不新增按键、不改任何默认值）。v8.8 修复「依旧黑屏有声」——v8.6 的代客呈现记账每刷新自增 `dev+16568`，而驱动线程的让位判据把 `dev+16568` 也算作「guest 管线在动」，于是每轮都让位（v8.7 实机 `DRIVE drive=0 ok=0 produce=0 vivo=8733`）、guest 侧零产出（`284` 恒 0、`ringR16700=ringW16704=0`、`mmio_w=0`、`inKick13232=0`）、呈现线程在 `F8000068` 上空转 6868 万次、`SWAP` 全程仅 1 次；本版把让位判据改成**只认 guest 自己会自增的 `base+284`**（`284==0` 才驱动、已有待交付帧让位、卡住 45 次刷新≈0.75 s 仍驱动），并把 G14 默认改为**开**——走完提交入口后若 `284` 仍为 0 就补调 guest 原生生产者 `sub_82882730`（入队 + 自增 `base+284` + 置位帧事件 `*(0x82DBB5E0)`；`284 != 0` 时绝不补调，杜绝重复投递）。v8.9 修复「黑屏、第一个 LOGO 有声」——v8.8 实机证明：补产分支排在「每帧提交入口 `sub_82880978`」**之后**，而该提交入口一进去就把驱动线程永久留在 guest 的显示回调里（`DRAIN-IN n=13` 后再无出栈、此后 `drive=2 ok=1 dentro=1 estanc=1`），补产一次都没跑到（`produce=0 prodSinDev=0`），于是 `base+288` 恒 1、三处排空判据 `while (*(base+284) + *(base+288) != 0)` 永远转不出去、`SWAP` 全程 `n=1`；本版把顺序**反过来**（先投递 → 再可选提交 → 最后可选有界交接），把提交入口收到**新开关 G17 `sa2_renut_display_drive_commit`（默认关）**之下，并新增 **G18 有界非阻塞交接 `sa2_renut_display_drive_pump_max`（默认 0 = 关）** 与 **G19 卡死看门狗 `sa2_renut_display_drive_watchdog_ms`（默认 500）**——驱动进 guest 超阈值就落一次 `[T2DIAG DRIVE-WEDGE]`（相位 + 快照）并永久停驱动，让下一轮日志不再靠猜卡在哪一段。帧率仍待实机实测。经验方法与判读字段见 docs/PERFORMANCE_T2ROTF.md，本轮结论见 `T2ROTF_deko3d_提交前置生产与看门狗报告.md`（上一轮见 `T2ROTF_deko3d_驱动判活与提交补产报告.md`（上一轮见 `T2ROTF_deko3d_启动崩溃IRQABI报告.md`；再上一轮见 `T2ROTF_deko3d_呈现记账交回guest报告.md`；再再上一轮见 `T2ROTF_deko3d_提交完成中断解除抑制报告.md`；更早见 `T2ROTF_deko3d_每帧提交驱动分线程报告.md`））
- v9.0-VIVO（2026-11-02）：修复「黑屏、第一个动画有声无画面」的**直接原因**——显示投递通道被钉到了错的对象上。
  离线逐指令（T72）坐实：门的上下文是**运行期显示设备**（实机 `0x41770018`），它的管理器自指 `*(dev+16)` 读出来是 0，
  于是门的就绪读 `*(*(r30+16)+272+4*slot)` 退化成读 `*(0+272+4*slot)` 恒 0（实机三槽 `cnt=0/0/0`）⇒ 门永不就绪 ⇒ 交付 0 次
  ⇒ 排空 `while (*(0x82E55CE0+284) + *(0x82E55CE0+288) != 0)` 永久自旋；而生产者侧被 v7.7-ALIGN 钉在静态管理器通道 `0x82E55E38`，两侧分叉。
  本版把生产者投递通道改钉到**活体设备自己的通道** `dev+24`（实机 `0x41770030`），并补回该设备缺的 `*(dev+16)=dev`（一律走 `RexGmShadowFor` 影子别名）；
  楔死态（`284==0 && 288>0` 且两侧六槽 arm 标记全 0、连续 120 次泵进入）交给 **guest 自己的**显示管线复位 `sub_82882628()`（封顶 8 次）；
  同时把历史遗留的「宿主侧完成侧配平」（曾直写 `*(base+288)`/`*(entry+28)`）整支**只读化**，源码中不再存在对 `284/288/16548/16552/16564/16568` 的写。
  新键 `sa2_renut_display_vivo` / `sa2_renut_display_reinicio`（均默认 `true`）。帧率仍待实机实测。
  结论与验证清单见 `T2ROTF_deko3d_投递通道钉到活体设备与楔死复位报告.md`。
- 按键：默认按 Xbox 物理位置映射到 Switch 手柄（A↔B、X↔Y、LB↔L、RB↔R、LT↔ZL、RT↔ZR、BACK↔减号、START↔加号、摇杆按下↔StickL/StickR）

## 1. 安装到 Switch

把以下内容放到 SD 卡的 `sdmc:/switch/Transformers2ROTF/` 下，目录布局为：

```
sdmc:/switch/Transformers2ROTF/
├── T2ROTF.nro          # 可执行文件（recomp/out/sw8/T2ROTF.nro）
├── T2ROTF.toml         # 可选：实验开关（默认：完成链守卫 + 完成侧配平 + 投递通道对齐 + 显示信号源/回调派发/影子访存与设备钩子兜底 + 每帧提交驱动（分独立线程）+ 提交无产出时补调 guest 原生生产者 + 提交完成中断解除抑制（已证伪，保留作 A/B）+ 呈现记账交回 guest 自己 + 提交前置生产（先投递后提交，提交入口默认关）+ 有界非阻塞交接（默认关）+ 卡死看门狗（默认 500 ms） 全开，写会计类开关全关；见第 3.8 / 3.9 / 3.10 / 3.11 / 3.12 / 3.13 / 3.14 / 3.15 / 3.16 / 3.17 / 3.18 / 3.19 / 3.20 节）
├── game/               # 原始 base 游戏文件
├── update/             # Title Update 覆盖文件（camera.lxb、ui/ 等）
├── patched/            # 安装期融合产物：xex_patcher 将 TU 补丁打进的 default.xex
└── dlc/                # DLC 独立包（Maps and Characters Pack）
```

> `T2ROTF.toml` 是可选配置文件。**本轮的修复不依赖它**：不放进 SD 卡时全部走内建默认值
> （v8.8 **不新增按键、当时仍为 24 键**——v8.9 已在此之上新增 3 键，见本节末尾；v8.8 只把 G14 `sa2_renut_display_drive_produce` 的默认值由 `false`
> 改为 **`true`**：提交入口驱动之后若 `base+284` 仍为 0，就补调一次 guest 原生生产者 `sub_82882730`
> ——入队 + 自增 `base+284` + 置位帧事件 `*(0x82DBB5E0)=F8000068`；`284 != 0` 时绝不补调。
> 本轮真正修的是**判活判据**：把 `dev+16568` 从让位比较里彻底移出，只认 guest 自己会自增的
> `base+284`（见第 3.19 节）；这是代码行为，不占开关。
> v8.8b（同一版内的交付前自查修正，不占开关）：补产入参**先经 guest 自己的解析器 `sub_828823F8`
> 取「当前呈现记录」**再作 `r3` 调生产者（取不到就跳过并计 `prodSinDev`，**绝不以 `r3 = 0` 调用**
> ——生产者本体首条指令 `lwz r10,64(r3)` / `stw r11,56(r3)` 会读写 guest `0x40`/`0x38`）；
> 当前显示对象 `*(0x82DBB608)` **只计数、不再当闸门**（v8.7 实机该字段恒 0，当闸门会把驱动永久拦下）。
> v8.9 **新增 3 键（24 → 27 键）**：`sa2_renut_display_drive_commit` 默认 **false**（每帧提交入口 `sub_82880978`
> 默认不再由宿主驱动——v8.8 实机证明一进该入口就把驱动线程永久留在 guest 显示回调里）、
> `sa2_renut_display_drive_pump_max` 默认 **0**（有界**非阻塞**交接 `sub_82881E00(r3=0x82E55E20, r4=-1, r5=1)`
> 的调用次数上限，`r5=1` 保证门不就绪即刻返回；默认关是因为「宿主替 guest 叫回调」正是 v8.8 卡死来源）、
> `sa2_renut_display_drive_watchdog_ms` 默认 **500**（驱动进 guest 超阈值就落一次 `[T2DIAG DRIVE-WEDGE]`
> 相位 + 快照并**永久停驱动**）。同时把**补产前移**到提交之前（顺序改为「先投递 → 再可选提交 → 最后可选交接」），
> G14 `sa2_renut_display_drive_produce` 仍为 **true**；DRIVE 行新增
> `commit=` / `pump=` / `bombeos=` / `muerto=` / `wedge_ms=` / `fase=` / `t_guest=`，并新增 `[T2DIAG PRODUCE …]`（见第 3.20 节）。
> v8.7 **不新增按键、不改任何默认值**：修的是启动即崩——中断回调 `sub_822D47A0` 的 ABI 是
> `(r3 = 来源 0/1, r4 = 设备)`，设备只取 `r4` 且校验通过才落盘；转储里任何 guest 地址
> （dev/ctx/fence/暂存块）先校验再读，不健全即跳过并计数。
> v8.6 **新增 1 键**：`sa2_renut_display_present_guest` 默认 **true**——每个垂直同步周期由宿主
> **代客调用** guest 自己的呈现记账函数 `sub_822D9688(r3=0)`，`*(dev+16564)++` / `*(dev+16568)++` /
> `*(*(dev+10900)+4)=0` 全部由 guest 代码在它自己的自旋锁内完成，宿主不写任何 guest 字段；
> v8.5 押注的「派发 source=1 提交完成中断」经 T61 离线逐指令精读**已证为空操作**（见第 3.17 节）；
> 置 `sa2_renut_display_present_guest=false` 退回 v8.5 行为）。
> v8.5 的 2 键保持原值：`sa2_renut_gpu_signal_hostcb_s1`（**true**，T61 已证其为空操作，实机可置 false 作 A/B）、
> `sa2_renut_gpu_signal_hostcb_s1_mode`（2）。
> 另二十一键（v8.9 之前的既有键）与 v8.4 相同——
> v8.4 的 1 键：`sa2_renut_display_drive_thread`（true，每帧提交驱动只在新增的独立宿主线程
> `T2ROTF Dirigir` 上执行、垂直同步派发线程绝不进入 guest 代码；置 `false` 退回 v8.3 行为，仅供 A/B 复现自锁）；
> v8.3 的 4 键：`sa2_renut_display_drive`（true，宿主驱动 guest 自己的每帧显示提交 `sub_82880978`）、
> `sa2_renut_display_drive_after_ms`（2500）、`sa2_renut_display_drive_div`（1）、`sa2_renut_display_drive_produce`（**true**，v8.8 起默认开）；
> 其余十六键：`sa2_renut_gpu_signal_dev_hook`（true）、`sa2_renut_display_guard`（true）、`sa2_renut_display_reconcile`（true）、
> `sa2_renut_display_align`（true）、`sa2_renut_gpu_signal`（true）、`sa2_renut_gpu_vblank_hz`（60）、
> `sa2_renut_gpu_signal_cpu`（2）、`sa2_renut_gpu_signal_diag`（true）、`sa2_renut_gpu_rptr_sync`（**false**）、
> `sa2_renut_gpu_signal_hostcb`（true）、`sa2_renut_gpu_signal_cb_data`（0=自动解析设备）、
> `sa2_renut_gpu_signal_cb_guard`（true，ISR 尾指针守卫）；
> v7.4 的 `sa2_renut_display_release` 与 `sa2_renut_display_agent` 自 v7.5 起默认 **false**，且它们对应的代码已改成只读；
> T25 的 `sa2_renut_display_tick` / `sa2_renut_swap_callthrough` 默认 false）。需要 A/B 对照或回退时
> 按第 3.8 / 3.9 / 3.10 / 3.11 / 3.12 / 3.13 / 3.14 / 3.15 / 3.16 / 3.17 / 3.18 / 3.19 节改键值；**二十七键都是 `kInitOnly`**，改完必须重启游戏。
> 回退本轮（v8.9）修复：把 `sa2_renut_display_drive_watchdog_ms` 置 `0`（关看门狗）、`sa2_renut_display_drive_pump_max`
> 置 `0`（关交接）、`sa2_renut_display_drive_commit` 置 `false`（关提交入口，本版默认已有此值）——
> 三者全关后行为与 v8.8 的差别只剩「补产被前移到提交之前」（见第 3.20 节）。
> 回退上一轮（v8.8）修复：把 `sa2_renut_display_drive_produce` 置 `false`（退回「只驱动提交入口、不补产」）；
> 再把 `sa2_renut_display_present_guest` 置 `false`（退回 v8.5 行为：呈现记账重新无人调用）；
> 再把 `sa2_renut_gpu_signal_hostcb_s1` 置 `false`（退回 v8.4 行为：提交完成中断重新被抑制——T61 已证其为空操作）；
> 再退可把 `sa2_renut_display_drive_thread` 置 `false`（退 v8.3 行为：驱动与中断派发共用垂直同步线程，实机会自锁）；
> 再退可把 `sa2_renut_display_drive` 置 `false`（退 v8.2 行为：中断链通、每帧提交侧不产出）；
> 再退可把 `sa2_renut_gpu_signal_dev_hook` 置 `false`（退 v8.1 的设备解析行为，影子访存修复保留）；
> 再退可把 `sa2_renut_gpu_signal_hostcb` 置 `false`（退回 v8.0 的空转状态）；再退可把 `sa2_renut_gpu_signal` 置 `false`（退回 v7.7 的 0 fps 黑屏）。

运行前先在 PC 上执行 `bash scripts/setup_install.sh`，脚本会自动完成：

1. 把 `build/iso_extract/` 复制到 `game/`（原始 base）
2. 把 `extracted/TU/` 复制到 `update/`（TU 覆盖文件，不含 xexp 融合）
3. 用 `tools/xex_patcher_cli/xex_patcher.exe` 将 `default.xexp` 融合进 `default.xex`，生成 `patched/default.xex`
4. 把 `extracted/DLC/` 复制到 `dlc/`（保持子目录结构）

运行时 VFS 挂载顺序（OverlayHostPathDevice，自上而下优先）：

- `update/` → `patched/` → `game/`：同名文件先命中 update/；`game:\default.xex` 被 patched/ 拦截，其余回退 game/。
- DLC 由 `ContentManager` 枚举 `dlc/` 子目录并动态注册为 marketplace content，游戏启动后按需 `XamContentCreateEx` 挂载。

> 游戏数据（`game/` + `update/` + `patched/` + `dlc/`，约 7 GB）不属于工程压缩包；
> 请从你已有的 Xbox 360 镜像/更新包/DLC 包自行提取（可用 `build/` 下的工具辅助），
> 然后执行 `scripts/setup_install.sh` 生成上述独立目录布局。

## 2. 构建步骤

### 前提

- Windows（本工程在 Windows + Git Bash 下构建），或任意能运行 devkitPro 的环境
- devkitPro + devkitA64 + libnx（`DEVKITPRO` 环境变量有效）
- CMake ≥ 3.25、Ninja、Python 3
- 依赖：Switch 版 rexglue-sdk 基座 `sdk/`（v0.10.0，含本移植的 Switch 平台层补丁：Switch 平台层、单层 VFS 挂载、guest 内存影子化、libnx 线程/崩溃钩子）。**本仓库已内置**，即构建实际使用的 SDK；配置时用 `-DREXSDK_DIR=<工程根>/sdk`，无需另行 clone。仅 `sdk/out/`（SDK 构建产物）被排除。本移植自身的图形部件——Renut 渲染引擎（`recomp/src/renut_engine/`）、plume/uam GPU 库（`recomp/thirdparty/`）与 T2 钩子表——位于 `recomp/` 而非 `sdk/`。
  - **不再需要** `repos/mesa-switch/` 的 NVK Horizon SDK（deko3d 版构建不链接 Vulkan 驱动，`PLUME_SWITCH_NO_NVK=ON`）；`scripts/build_mesa_sdk.sh` 仅作 NVK 时代的历史保留
- `recomp/generated/` 已含 codegen 输出（本次已生成，通常无需重跑；如要重跑需 host 端 `sdk/out/win-amd64/rexglue.exe` 与 `recomp/codegen_t2rotf.sh`）

### 构建命令

```bash
bash scripts/build_switch.sh --no-lto -j 8
```

产物：`recomp/out/sw8/T2ROTF.nro`

说明：

- `--no-lto`：193 MB 生成源码在 16 GB 内存下放不下整程序 LTO，因此工程默认以 `--no-lto` 构建；在更大内存的机器上去掉该参数可启用默认 LTO 构建（`-flto=2 -flto-partition=balanced`），性能可进一步提升，nfsmw 参考工程实测 LTO+PGO 约 +3 FPS。
- 图标/标题/作者：`icon_T2.png`、`Transformers: Revenge of the Fallen`、`mzy1996`（见 `recomp/CMakeLists.txt` 的 `rexglue_switch_add_nro`）。
- deko3d 版增量重编：`cd recomp/out/sw8 && ninja -j4 T2ROTF`（重编译改动源文件 → 重链 → 打包 NRO，约 40 秒；产物在 `recomp/out/sw8/T2ROTF.nro`）。

## 3. 60 帧验证方法

完整方法与判读字段见 **docs/PERFORMANCE_T2ROTF.md**。要点：

- 应用已默认请求 Switch 官方 apm 性能档：GPU 307.2 → 460.8 MHz，内存保持 1331.2 MHz（配置 0x92220008）。这是官方 API 配置，不是超频；dock 模式不请求任何配置。
- 实机运行后查看 `logs/rex/rex_perfil.log`（NRO 同目录 `logs/rex/` 下）：
  - `juego X fps`：游戏实际 FPS，达标线 ≥ 59.5 持续稳定
  - `relojes: CPU ... GPU 460.8 MHz, memoria 1331.2 MHz`：同时验证不超频合规
  - 需要定位 CPU 热点时，在 `logs/rex/` 下创建空文件 `perfil_pilas.flag` 启用 1 kHz 栈采样
  - 游戏中按 **L+R+ZL** 可启动 GPU A/B 测试（8 个模式逐项关闭 GPU 工作，判断瓶颈在提交量还是固定开销）
- deko3d 版另有两类诊断日志（都在 NRO 同目录）：`deko3d.log`（设备 / 队列 / 交换链 / 图像堆 / 着色器块）与 `T2ROTF_001.log` / `T2ROTF_002.log`（renut + GPU 后端 + 视频 + D3D 设备）。判读 D3D 设备发布看 `VdGlobalDevice: procType=… slot … cell …` 行；判读崩溃看 `Unhandled guest access violation` 与 `rex_crash.log` 的 `far dentro de la memoria del invitado`。

### 3.5 「影片播放完死机」的 A/B 验证清单（v7.2）

本版把 T23/T24 定位到的**唯一自旋点**（guest 呈现门 `sub_82881B88` 三槽全不就绪后的 yield，`lr=0x82881DE8`）做成了「一个探针 + 两个可切换修复候选」，抢在实机上做 A/B。三个动作都**不超频、不改动 `game/` 数据**。

1. **基线（先做，别改 `T2ROTF.toml`）**：跑一次片头（到 `a0_intro` 播放完为止），保留 `logs/rex/rex_stderr.log` 与 `T2ROTF_001.log`。
   - 看新探针 `[video] T2DIAG PUERTA …` 行：它把门的判据字段整组打出来（`st0` / `cnt` 三槽 / `v288` / `cola…` / `obj…` / 四个时间窗），前 32 次全打、之后每 400 万次一行。
   - 同时看 `[video] T2DIAG YIELD total=… top=[82881DE8x…]`：确认自旋点仍只落在 `82881DE8`。
   - `PUERTA` 里哪一组字段恒为 0 / 恒定不动，就是「三槽为何全不就绪」的直接答案。
2. **候选 B（先试这个）**：把 SD 卡上的 `T2ROTF.toml` 改成
   ```toml
   sa2_renut_display_tick = true
   ```
   重启游戏再跑一次片头：宿主每呈现一帧后，会代客跑一趟 guest 显示（垂直同步）排空
   `sub_82881E00(0x82E55EE0, 0xFFFFFFFF, 1)`——这是游戏自己的显示 tick 里那一步的同形单趟版本。
   比对 `PUERTA` / `YIELD` 与看门狗的 `etapa` 计数是否继续推进、影片是否出画面、播放完是否还死机。
3. **候选 A**：把 `sa2_renut_display_tick` 改回 `false`、把
   ```toml
   sa2_renut_swap_callthrough = true
   ```
   打开，让 guest 自己的交换体 `sub_822D9D70` 也跑一遍（它被宿主整体替换掉的那段记账）。
   此开关打开时宿主不再镜像 `dev+16560`，避免双计。
4. **对照**：两个键也可同时 `true`，用于看叠加效果（若崩，先关 A 只留 B）。
5. 若两者都无效：按第 1 步探针给出的最卡字段，下一轮直接定位「谁该 arm 这三槽」并补钩子/代写。

> 三个动作的完整判据、日志逐项核对与风险声明见 `T2ROTF_deko3d_呈现门阻塞语句与显示会计核对报告.md`。

### 3.6 v7.3（T28）验证清单 — 显示队列完成侧代理（已被 v7.5 取代，仅作历史对照）

实机已确认 T25 的两个候选**四种组合全部「有声无画」**，两条候选各自无效的直接原因已定论
（候选一挂在死路径：卡死后 Swap 不再发生；候选二 `container` 记错 `0xC0`），
本轮改为由宿主扮演**显示 / 垂直同步侧**，把「六通道全 0 却仍留着会计残留」的陈旧状态保守抽干。
详见 `T2ROTF_deko3d_显示完成侧代理与kDisplayCtx修正报告.md`。

1. 只需把新的 `T2ROTF.nro` 覆盖到 `sdmc:/switch/Transformers2ROTF/`（`T2ROTF.toml` 保持默认即可，不放也生效），**完全退出再启动**游戏。
2. 期待：第一个 logo 之后**画面出现并持续**（不再是只有声音的黑屏）。
3. 日志（`logs/rex/`）里要看到这些新行：
   - `[renut] T2DIAG display-agent … writes=1|2` ⇒ 宿主确实抽干过残留（修复生效的第一证据）；
   - `[renut] T2DIAG COLA … 284=0 288=0 e24=0 e28=0` 长期保持 ⇒ 帧循环归零、恢复流动；
   - `T2DIAG SWAP n=1,2,3…` 持续增长 ⇒ Swap 又活了（T25 版全程只有 `n=1`）；
   - `rex_perfil.log` 里 `juego … fps` 不再是 0.0。
4. 若仍黑屏：把 `rex_perfil` / `rex_stderr` / `T2ROTF_001` / `T2ROTF_002` 整组发回。日志里
   `display-agent` 有没有打、`COLA` 的 `284/288/e24/e28` 有没有归零，会直接指出下一步是
   「残留清了但没人推进」还是「清了又被重新建起来」。
5. 回退：`sa2_renut_display_agent = false` 即回到 v7.2 行为（已知黑屏）。

### 3.7 v7.4（T29）验证清单 — 宿主作为「显示完成侧」（已被 v7.5 取代，仅作对照）

实机已证明 T28 的代理**从未被叫到**：它在门钩子里的廉价前置判断只读「已投递」两个字
（`base+284` / 条目 `+24`），而实机卡死态的残留恰在「已交付未完成」两个字上
（`base+284=0`、条目 `+24`=0、**`base+288=1`**）。本轮改为：宿主补上**显示完成侧**
（退 `base+288` 与条目 `+28`），并把门钩子前置判断改成四个会计字同查、门侧门槛 32→8。
详见 `T2ROTF_deko3d_显示驱动链取证与完成释放报告.md`。

1. 把新的 `T2ROTF.nro` + `T2ROTF.toml` 覆盖到 `sdmc:/switch/Transformers2ROTF/`
   （`toml` 不放也生效：两个新开关默认都开），**完全退出再启动**，**不要超频**。
2. 期待：第一个 logo 之后**画面出现并持续**（不再是只有声音的黑屏），`juego … fps` 不再是 0。
3. `logs/rex/` 里要看到这些新行：
   - `[renut] T2DIAG release …` ⇒ 完成侧释放确实在跑（修复生效第一证据，`rel=` 会增长）；
   - `[renut] T2DIAG COLA … 284=0 288=0 e24=0 e28=0 rel=… ag=… tf=…` 长期归零
     ⇒ 排空 `while` 归零、帧循环恢复；`tf=` 顺带给出 `*(0x82DA74BC)` 的实机取值；
   - `[video] T2DIAG PERRO2 … dtick=… ddrain=…/… dpump=…/…/… ddeliv=… dcmd=…/… dprod=…`
     ⇒ 显示链每一段（tick / 排空 / 显示泵 / 交付 / 显示命令 / 生产者）是否流动。
4. 判读分支：
   - `dpump_in` 与 `yield` 同量级而 `ok/ddeliv/dprod` 全 0 ⇒ 完成侧释放没生效（把 `release` 行发回）；
   - `ddeliv` 增长而 `dcmd_in ≫ dcmd_out` ⇒ 显示命令回调阻塞，下一战场是回调虚表目标；
   - `COLA` 的 `284/288/e24/e28` 始终非 0（不肯归零）⇒ 残留被重新建起来，需看 `ag=` 与 `rel=` 谁在跑。
5. 对照（可选，各跑一次）：把 `sa2_renut_display_release` 置 `false` 再跑一次，
   证明「症状回归 ↔ 完成侧释放」的一一对应。
6. 回退：两个开关都置 `false` 即回到 T28 行为（已知 0 fps 空转）；源码级回退用
   `renut_device.cpp.bak.t29`（T28 基线）与 `t2rotf_video_nativo.cpp.bak.t29`（前态）。

### 3.8 v7.5（T31）验证清单 — 启动失败双根因修复：只读化 + 完成链守卫（已被 v7.7 / v8.0 取代，仅作对照）

实机（2026-10-28）报「从 Sphaira 启动无限黑屏、没有任何加载；从前端启动直接报错退出」。两根因同源，
都出在 v7.4 引入的「宿主代客写显示会计」：

1. **Sphaira 无限黑屏（无加载）**：完成侧释放挂在**门热路**上，而门在帧循环卡死时被以约
   10 万次/秒 的量级调用（与 `yield` 计数同量级），把同一个会计字 `base+288` 由 1 刷过 0、
   回绕成 `0xFFFFFFFF` ⇒ guest 排空循环 `while (*(base+284)+*(base+288) != 0)` 永不退出。
   （日志证据：同一次启动里 `release n=1` 之后 `cola288` 单调降到 `4294967295/4294967294/…`，
   等价于 16,760,832 次扣减。）
2. **前端报错退出**：单次清零让门提前就绪后，guest 走进「当前显示对象为空」的完成回调链，
   在 `*(0+0x368)` 上访问违例（崩溃 `pc = imagen+0x2a5444c = sub_8287BF88+0x7c`，
   `far` = guest 基址 + 0x368，`x0=x1=0x368`）。

v7.5 的做法：**回退盲写**（两个写会计分支改为只读观察、默认值改 false，即使 SD 上留着旧的 true
也不会再写 guest 内存）+ **新增守卫** `sa2_renut_display_guard`（默认 true：`*(0x82DBB608)==0` 时短路
完成回调链上的四个 guest 函数）。详见 `T2ROTF_deko3d_启动失败双根因与守卫修复报告.md`。

1. 把新的 `T2ROTF.nro` + `T2ROTF.toml` 覆盖到 `sdmc:/switch/Transformers2ROTF/`，
   **完全退出再启动**，**不要超频**。
2. 期待（本轮验收点）：
   - 前端（Homebrew 菜单）启动：**不再直接报错退出**；
   - Sphaira 启动：**不再无限黑屏无加载**（至少能看到初始化推进与日志落盘）。
3. 若出现 `[renut] T2DIAG GUARDIA … fun=sub_8287BF88 … onj=0` ⇒ 确认「当前显示对象」
   `*(0x82DBB608)` 在实机确实为 0、守卫正在短路完成链。**这是本轮的正确吃法，不是新故障**；
   把该行连同 `rex_crash.log`（若有）一并回传即可定论。
4. 回退：`sa2_renut_display_guard = false` 即恢复直调（会重新出现前端启动即崩，仅供对照）；
   源码级回退用本包内提供的 `renut_device.cpp.bak.t29`（= v7.4 基线）。
5. 本轮**不包含**画面修复：`base+288` 的「显示完成事件源」在 Switch 后端仍然缺失（承接 v7.4 结论），
   所以「第一个 logo 之后有声无画」与「不超频 60 帧」的真实结论仍待下一步（见第 5 节）。

### 3.9 v7.6（T38）验证清单 — 排空自旋根因修复：显示完成侧配平（已被 v7.7 取代，仅作对照）

实机（2026-10-29 回传）确认 v7.5 的守卫与只读化已生效：不再崩溃（`rex_stderr` 25 行只有初始化，
无任何访问违例）、`cola288` 不再被扣穿回绕（门的 PUERTA 采样到 `n=36,000,000` 期间恒为 1）、
`GUARDIA` / `cola-readonly` / `release` / `display-agent` / `display-tick` / `callthrough` **全 0**。
但帧率仍是 `juego 0.0 fps`，卡点被定量钉到 **guest 自己的排空循环**：`sub_82882200` 的判据
`while (*(base+284) + *(base+288) != 0)` 永不为 0（`YIELD total=35,439,751`，全部 `lr=0x82881DE8`；
`PERRO2` 里泵被进入 `dpump=35,434,483/0/35,434,481`——**成功 0 次**）。

T36 用生成代码逐字定论：在飞显示项 `*(base+288)`（base = `0x82E55CE0`）全程序只有两个增减点——
`++` 在交付 `sub_82881930`、`--` 在泵 `sub_82881E00` 的完成回调之后（`idx != -1` 时同刻 `--*(entry+28)`），
两者本应在同一次泵调用里成对；实机读数 `284=0 / 288=1 / e24=0 / e28=1` 证明有**一次「已交付、完成侧
没执行完」的悬挂**（本移植没有 VSync / GPU 完成中断源，Xbox 上由 ISR 族 `sub_82997AF8` /
`sub_82997968` / `sub_829975F0` 兜底），这条残留又会自己满足门的条目判据
`*(entry+24)+*(entry+28) > 0` ⇒ 门每轮报就绪、交付每轮弹不到记录、泵报 0、排空无限重旋。

v7.6 的做法：**开关 D `sa2_renut_display_reconcile`（默认 true）**——只在**排空入口**（guest 每帧 1 次，
绝不落在门/泵热路）判定 `e24 == 0 && e28 > 0` 时，按 guest 自己完成侧的动作退掉这条残留（先
`--*(base+288)`、再按同次数 `--*(entry+28)`，各自 > 0 才减、单帧上限 64、**不碰 284 / entry+24**）。
详见 `T2ROTF_deko3d_排空自旋根因与完成侧配平报告.md`。

1. 把新的 `T2ROTF.nro` + `T2ROTF.toml` 覆盖到 `sdmc:/switch/Transformers2ROTF/`，
   **完全退出再启动**，**不要超频**。
2. **先确认版本**：日志里出现（且只出现一次）
   `[video] T2ROTF BUILD v7.6-RECON (display-completion reconcile at the drain entry)`。
   应用自身 banner 仍是旧的 `v6.27-MAPFIX`，**不要用它判版本**。
3. 期待（本轮验收点）：`[video] T2DIAG RECON nr=1 …` 出现（配平被触发，这是修复生效，不是新故障）；
   `PERRO2` 的 `ddeliv` / `dprod` 开始增长、`dpump` 成功数脱离 0；`YIELD` 总量断崖式下降、且不再
   集中于 `82881DE8`；`rex_perfil` 的 `juego` fps **> 0**。
4. 需回传的日志行：`[video] T2ROTF BUILD v7.6-RECON`、`[video] T2DIAG RECON …`、
   `[video] T2DIAG PERRO2 …`（末条）、`[video] T2DIAG PUERTA …`（首末各一）、`[video] T2DIAG YIELD …`（末条）、
   `[renut] T2DIAG COLA …`（若有）、`rex_perfil.log` 的 `juego … fps` 行与线程 CPU 列表、
   `rex_crash.log`（若产生）。
5. 回退：`sa2_renut_display_reconcile = false`（退回 0 fps 空转，仅供对照）；源码级回退用本包内
   `t2rotf_video_nativo.cpp.bak.t37`（= v7.5 源，67,806 字节）。
6. 本轮**仍不包含**帧率达标：解锁帧循环 ≠ 60 帧；「不超频 60 帧」的真实结论仍待下一步（见第 5 节）。

### 3.10 v7.7（T41）验证清单 — 黑屏直接原因修复：显示投递通道对齐（已被 v8.0 取代，仅作对照）

实机（2026-10-29 回传 `rex_perfil(29)` / `rex_stderr(65)` / `T2ROTF_001(63)` / `T2ROTF_002(44)`）确认
v7.6-RECON **无效**：版本标记 `v7.6-RECON` 在跑、无崩溃、也不回绕，但 `RECON` 只命中 1 次
（`nr=1 n=8 idx=0 c24=0 c28_antes=1 e28_antes=1 retro=1`）就收敛——悬挂只是从 `284=0/288=1`
换成 `284=1/288=0`；门三槽 arm 计数 `PUERTA … cnt=0/0/0` 与后备标记 `obj256=0/0/0` **全程为 0**、
`PERRO2 … dpump=16,591,560/0/16,591,558`（**成功 0 次**）、`juego 0.0 fps`、CPU 115%（纯自旋）。

T40/T41 逐条读完生成代码后定论：**投递者与消费者用的不是同一条显示通道**。

- 生产者 `sub_822B5608(r3=obj, r4=A)`：`++*(entry+24)`、`++*(0x82E55CE0+284)`、
  **`++*(A+248)`（全程序唯一的 arm 自增点）**、`sub_822B57E0(A+8, obj)`；`A` 由每帧 tick
  `sub_82ABB7D0` 用 `sub_828823F8()` 从 `*(0x82DBB5C4)` 数组里挑「`+0` 状态 == 5」的**当前呈现设备**算出（`A = dev + 24`）。
- 消费者链恒定围绕显示对象 `0x82E55E20`：门 `sub_82881B88` 就绪读 `*(*(0x82E55E20+16)+272+4*slot)`；
  交付 `sub_82881930(ctx=0x82E55E20+24)` 从 `*(ctx)+248+4*slot` 扣减、弹 `ctx+slot*80+8`。
- ⇒ arm 计数与队列全落到**别人**的通道上：门读到的计数恒 0 ⇒ 门永不就绪（每轮末尾 yield，`LR=0x82881DE8`）；
  交付入口先读该计数、为 0 立即返回 0（不弹记录、不减 `284`、不加 `288`）；泵 0 次成功；
  排空 `while (*(0x82E55CE0+284) + *(0x82E55CE0+288) != 0)` 永久自旋 ⇒ `0 fps` 黑屏。

v7.7 的做法：**开关 H `sa2_renut_display_align`（默认 true）**——在 `sub_822B5608` 钩子里（每次投递 1 次，
不在门/泵热路）把投递的通道基址 `ctx.r4` 改写为消费者侧实际读/弹的 **`0x82E55E20+24`**，并在必要时把
管理器自身的 `*(0x82E55E20+16)` / `*(0x82E55E20+24)` 修回自指。**不加不减任何会计**，不碰
`284` / `288` / `entry+24` / `entry+28`。详见 `T2ROTF_deko3d_投递通道错位与对齐修复报告.md`。

1. 把新的 `T2ROTF.nro` + `T2ROTF.toml` 覆盖到 `sdmc:/switch/Transformers2ROTF/`，
   **完全退出再启动**，**不要超频**。
2. **先确认版本**：日志里出现（且只出现一次）
   `[video] T2ROTF BUILD v7.7-ALIGN (display post channel aligned with the drained channel)`。
   应用自身 banner 仍是旧的 `v6.27-MAPFIX`，**不要用它判版本**。
3. 期待（本轮验收点）：`[video] T2DIAG ALIGN n=1 … a_ahora=82E55E38`；`PERRO2` 的 `dpump` **成功数脱离 0**；
   `PUERTA … cnt=…` 至少一槽非 0；`[video] T2DIAG RECON` **不再出现**；`YIELD` 总量断崖式下降且不再集中于
   `82881DE8`；`juego` fps **> 0**、`Game Master` 线程 CPU 从 115% 回落；**画面出现**三连 logo。
4. 需回传的日志行：`[video] T2ROTF BUILD v7.7-ALIGN`、`[video] T2DIAG ALIGN …`（首末各一）、
   `[video] T2DIAG ALIGN-SELF …`（若有）、`[video] T2DIAG PERRO2 …`（末条）、`[video] T2DIAG PUERTA …`（首末各一）、
   `[video] T2DIAG YIELD …`（末条）、`[video] T2DIAG RECON …`（若有）、`[renut] T2DIAG COLA …`（若有）、
   `rex_perfil.log` 的 `juego … fps` 行与线程 CPU 列表、`rex_crash.log`（若产生）。
5. 回退：`sa2_renut_display_align = false`（退回 v7.6 的 0 fps 黑屏，仅供对照）；源码级回退用包内
   `t2rotf_video_nativo.cpp.bak.t41`（= v7.6 源，72,255 字节）。
6. 本轮**仍不包含**帧率达标：解锁帧循环 ≠ 60 帧；只有画面出来后才能在**不超频**条件下实测帧率
   并给「不超频 60 帧」的达标结论（见第 5 节与待办 T26）。

### 3.11 v8.0（T44）验证清单 — 显示完成信号源补齐（已被 3.12 取代，仅作参考）

实机（2026-10-29 回传 `rex_perfil(30)` / `rex_stderr(66)` / `T2ROTF_001(64)` / `T2ROTF_002(45)`）确认
v7.7-ALIGN **有效但不充分**：`T2DIAG ALIGN n=1..4 a_ahora=82E55E38`（对齐本体在跑）、
`T2DIAG YIELD total=36 top=[82881DE8x27]` 且 4 秒内三次采样完全一致（**自旋断崖式消失**）、`rex_stderr` 无任何访问违例；
但 `T2DIAG espera sub_824F3580 n=34,520,000 r3=F8000060`（影片侧**死等一个 F8000060 事件**）、
`PERRO dev=40070600 vblank16548=5 sub16560=5 fr16564=5 disp16568=5`（**4 秒内一字不变**）、
`irq47=0 irq95=0`（guest 图形中断链**一次都没进过**）、`PUERTA … cnt=0/0/0 cola284=…/cola288=…`、
`PERRO2 … dpump=19/0/17`（成功 0）、`juego 0.0 fps`、时钟 `CPU 1020.0 / GPU 460.8 / 内存 1600.0 MHz`（**未超频**）。

**T44 定论**：缺的是**显示完成信号源**，而它是被 Renut 一并删掉的——Renut 清空 `config.graphics`
（`renut_engine/gpu/overlay/app_integration.cpp:87`）时连带移走两样东西：
① `0x7FC80000` 的 64 KB GPU 寄存器 MMIO 窗口（没有它，guest 读垂直同步状态字 `0x7FC86544`/寄存器 `0x1951`
落到裸物理内存读回 0 ⇒ 显示 ISR 前置条件永假）；② 按垂直同步派发 guest 图形中断回调的宿主线程
（没有它，guest 注册的 `VdSetGraphicsInterruptCallback` 回调被 `graphics_system==nullptr` 分支静默丢弃
⇒ `sub_822D47A0`/`sub_822D9570` 永不执行 ⇒ `dev+16548` 族冻结、AVIVO 主表面寄存器不回写、等待事件无人释放）。
详见 `T2ROTF_deko3d_显示完成信号源补齐报告.md`。

v8.0 的做法：新增模块 `recomp/src/t2rotf_gpu_signal.{h,cpp}`（`t2rotf::gpu_signal::GpuSignalSystem`），
在 `OnPreSetup` 里 `renut::gpu::ConfigureRuntime(config)` **之后**落成 `config.graphics`：
**A** 注册 `0x7FC80000` + mask `0xFFFF0000` / size `0x0000FFFF` 的 MMIO 窗口，读值逐条对齐 SDK
（`0x0F00→0x08100748`、`0x0F01→0x0000200E`、`0x194C→显示高度`、**`0x1951→恒 1`**、`0x1961→视口尺寸`；
写 `0x01C5` 记录、写 `0x1844` 吞掉）；**B** 起一条 `T2ROTF VSync` 宿主线程，按 `sa2_renut_gpu_vblank_hz`
（默认 60）`ExecuteInterrupt(thread_state(), callback, {source=0, user_data}, 2)`（与 SDK `MarkVblank→DispatchInterruptCallback(0,2)` 同语义，
**唯一调用点，绝不从 guest 热路调用**）。`presenter()`/`provider()` 保持 null——**画面仍由 Renut/deko3d 出，
本模块不写任何会计、不碰门/泵热路**。

1. 把新的 `T2ROTF.nro` + `T2ROTF.toml` 覆盖到 `sdmc:/switch/Transformers2ROTF/`，**完全退出再启动**，**不要超频**。
2. **先确认版本**：日志里出现 `[video] T2ROTF BUILD v8.0-GPUIRQ (host dispatches the guest GPU interrupt)`。
   （同日志里仍会有 `v7.6-RECON` 与 `v7.7-ALIGN` 两个旧标记行，它们都属于本 NRO，不是版本混装；
   应用自身 banner 仍是旧的 `v6.27-MAPFIX`，**不要用它判版本**。）
3. 安装期一次性日志 `[video] T2ROTF GPSIG: …`（MMIO 窗口注册 / guest 注册回调 callback=… user_data=… /
   ring 初始化 / VSync 线程已启动）。
4. 期待（本轮验收点）：`[video] T2DIAG GPSIG vblanks=… irq=… cb=… r1951=…` 的 **`vblanks` 持续增长**、
   **`cb` 非 0**、**`irq` 持续增长**、**`r1951` 持续增长**；`PERRO dev=…` 的 `vblank16548` / `sub16560` /
   `fr16564` / `disp16568` **不再冻在 5**；`irq47` / `irq95` **非 0**；`T2DIAG espera sub_824F3580 n=…` 的 n
   **不再线性暴涨**；`PERRO2` 的 `dpump` 成功数脱离 0；`juego` fps **> 0**；**画面出现**三连 logo（luxo → atvi → hasbro）。
5. 需回传的日志行：`[video] T2ROTF BUILD v8.0-GPUIRQ`、`[video] T2ROTF GPSIG:`（全部）、
   `[video] T2DIAG GPSIG …`（首末各一）、`[video] T2DIAG GPSIG wptr=…`（前几条）、
   `[video] T2DIAG PERRO dev=…`（末条）、`[video] T2DIAG PERRO pump …`（末条）、`[video] T2DIAG PERRO2 …`（末条）、
   `[video] T2DIAG PUERTA …`（末条）、`[video] T2DIAG espera sub_824F3580 …`（末条）、
   `rex_perfil.log` 的 `juego … fps` 行与线程 CPU 列表、`rex_crash.log`（若产生）。
6. 判读分支：`vblanks` 涨但 `cb=0`/`irq=0` ⇒ guest 没注册回调（查 `VdSetGraphicsInterruptCallback` 调用点/时机）；
   `irq>0` 但 `r1951=0` ⇒ 回调地址不符或 `source` 语义不是 0；`irq47>0` 但 `vblank16548` 仍冻 ⇒ 卡在 ISR 内部前置条件
   或其后的等待事件释放；`vblank16548` 开始涨但 fps 仍 0 ⇒ 卡点转回帧管线（门三槽与 `dpump` 成功数）。
7. 回退：`sa2_renut_gpu_signal = false`（退回 v7.7 状态，0 fps 黑屏，仅供对照）。
8. 本轮**仍不包含**帧率达标：画面出来后才能在**不超频**条件下实测帧率并给「不超频 60 帧」结论（见第 5 节与待办 T26）。

### 3.12 v8.1（T47）验证清单 — 显示中断回调「代客派发」（已被 3.13 取代，仅作参考）

实机（2026-10-29 回传 `rex_perfil(31)` / `rex_stderr(67)` / `T2ROTF_001(65)` / `T2ROTF_002(46)`）确认
v8.0-GPUIRQ **只补了一半**：安装期三行齐全、VSync 派发线程**按 60 Hz 精确运行**（`T2DIAG GPSIG vblanks` 0→3841 / 64 s、0→3121 / 52 s），
但全程 `cb=00000000 ud=00000000`、`irq=0`、`r1951=0`、**`mmio_r=0`**、`mmio_w=0`、`wptr=0(0)`（连 `T2DIAG GPSIG wptr=` 一次都没打）、
`irq sub_822D47A0`=0 / `irq sub_822D9570`=0（而 `init sub_822D4A08`=6、`externo sub_822D4488`=5 说明显示环初始化跑过）、
`PERRO dev=40070600 vblank16548=5 … disp16568=5 ringR16700=0 ringW16704=0`（**全程冻结**）、
`PUERTA n=29 … cnt=0/0/0 cola284=0 cola288=0`、`espera sub_824F3580 n=42500000 r3=F8000064`（**仍在事件上死等**）、
`PERRO2 dtick=7 ddrain=4/11 dpump=22/0/20 ddeliv=6 dprod=6`（**泵成功 0**）、`YIELD total=37`（v7.7 的对齐**没有回归**）、
`juego 0.0 fps`、`CPU total 99%`；`rex_stderr` 25 行**无访问违例**；`relojes: CPU 1785.0 / GPU 921.6 / 内存 1600.0 MHz`
（本次为**高时钟**状态，GPU 由本工程 APM 请求抬到 921.6 ⇒ `0 fps` 是**管线停摆**，不是性能不足）。

**T46 定论**：v8.0 装好了电话线但**没人拨号**。派发线程要知道「该调 guest 的哪个回调」，这个信息来自 guest 调 `VdSetGraphicsInterruptCallback`，
而该注册点（生成代码 `T2ROTF_recomp.28.cpp:22182 DEFINE_REX_FUNC(sub_822E1690)`，22262-22274 注册 `0x822D47A0` + 设备）属于 guest 的 D3D 设备创建路径，
被 **Renut 的原生 deko3d 设备接管整体旁路** ⇒ `cb=0` ⇒ 一个中断都没发 ⇒ `sub_822D47A0` 未进（所以连它那唯一一次寄存器读都没有 ⇒ `mmio_r=0`）
⇒ 真 ISR `sub_822D9570` 不跑 ⇒ `dev+16548` 族冻结、AVIVO `0x1844` 不回写、显示侧等待无人释放 ⇒ 帧管线停在第 6 次交付 ⇒ `0 fps` 黑屏。

v8.1 的做法（只改 `recomp/src/t2rotf_gpu_signal.cpp`，410 → 709 行）：在 `callback_==0` 时**按 guest 自己的注册语义代客派发**——
**同一回调地址 `0x822D47A0`、同一设备、同一组参数 `{source=0, user_data=dev}`**（`ExecuteInterrupt(thread_state(), cb, {0,dev}, 2)`，与 SDK `MarkVblank→DispatchInterruptCallback(0,2)` 同语义）；
设备用 `VdGlobalDevice` 两跳 `0x820011AC`/`0x820011B0 → cell → dev` 解析（`TranslateVirtual`，解析一次即缓存，可用 `sa2_renut_gpu_signal_cb_data` 手工覆盖）；
派发前对 `*(dev+16544)` 做「非 guest 代码指针就临时清零、调用后还原」的守卫（`sa2_renut_gpu_signal_cb_guard`）；
source=1（提交完成）**默认关**（`sa2_renut_gpu_signal_hostcb_s1=false`：guest 的 fence 未说完成前替它报完成＝伪造进展）；
新增只读诊断 `[video] T2DIAG GPSIG2 dev=… cb16544=… v16548=… v16552=… v16568=… a16700=… a16704=… blk10900=… blk16=… f10940=… f10941=… f10896=… evA0=… evE0=… flagC8=… wD0=… fD4=… fE4=… st5F8=… st690=… cont=… obj=… cola284=… cola288=… m272=… m276=… m280=…`。
**本模块仍不写任何显示会计、不碰门/泵热路，画面仍由 Renut/deko3d 出。** 详见 `T2ROTF_deko3d_显示中断回调代客派发报告.md`。

1. 把新的 `T2ROTF.nro` + `T2ROTF.toml` 覆盖到 `sdmc:/switch/Transformers2ROTF/`，**完全退出再启动**。
   验收场景是**不超频手持**（上一版实机跑在高时钟状态；两侧都要看，但 60 帧结论只在**不超频**条件下给）。
2. **先确认版本**：日志里出现
   `[video] T2ROTF BUILD v8.1-GUESTCB (host calls the guest's own GPU interrupt callback)`。
   （同日志里仍会有 `v7.6-RECON` 与 `v7.7-ALIGN` 两个旧标记行，它们都属于本 NRO，不是版本混装；
   应用自身 banner 仍是旧的 `v6.27-MAPFIX`，**不要用它判版本**。）
3. 安装期新增两条一次性日志：`[video] T2ROTF GPSIG: 设备解析 slot=… cell=… dev=…`、
   `[video] T2ROTF GPSIG: 宿主代客派发 guest 图形中断回调 callback=822D47A0 dev=…（guest 注册点被 Renut 原生设备路径旁路）`。
4. 期待（本轮验收点）：`[video] T2DIAG GPSIG …` 的 **`hostcb` 持续增长**、**`irq` 持续增长**、
   **`mmio_r` 必须增长**（ISR 进来第一件事就是读 `0x7FC86544`）、`r1951` 增长、`sup1844` 增长、`inv` 与 `ret` 同步增长；
   `[video] T2DIAG GPSIG2 …` 的 `v16548` / `v16568` **脱离冻结**、`a16700` 增长、`cola284`/`cola288` 不再悬挂、`m272/m276/m280` 出现非 0；
   `T2DIAG espera sub_824F3580 n=` **不再线性暴涨**；`PERRO2` 的 `dpump` 成功数脱离 0；`juego` fps **> 0**；**画面出现**三连 logo。
5. 需回传的日志行：`[video] T2ROTF BUILD v8.1-GUESTCB …`、`[video] T2ROTF GPSIG:`（全部）、
   `[video] T2DIAG GPSIG …`（首末各一）、`[video] T2DIAG GPSIG2 …`（首末各一）、
   `[video] T2DIAG PERRO dev=…`（末条）、`[video] T2DIAG PERRO2 …`（末条）、`[video] T2DIAG PUERTA …`（末条）、
   `[video] T2DIAG espera sub_824F3580 …`（末条）、`rex_perfil.log` 的 `juego … fps` 与 `relojes:` 行、`rex_crash.log`（若产生）。
6. 判读分支：`hostcb` 涨、`irq` 涨但 `mmio_r` 不涨 ⇒ 回调地址/参数不符（用 `sa2_renut_gpu_signal_cb_data=40070600` 试）；
   `irq` 涨但 `v16548` 不涨 ⇒ 卡在 ISR 内部（看 `inv`/`ret` 差与 `rex_crash.log`）；`v16548` 涨、`cola284/288` 归零、
   `dpump` 成功 > 0 但仍 `0 fps` ⇒ 卡点转回帧管线（下一轮攻呈现侧）；仍死等 `F8000064` ⇒ 释放者不在 vblank 侧，
   打 `sa2_renut_gpu_signal_hostcb_s1 = true` 做 A/B（第二步）；出现访问违例 ⇒ 回传 `rex_crash.log`（前 40 行）。
7. 回退：`sa2_renut_gpu_signal_hostcb = false`（退 v8.0）；再退 `sa2_renut_gpu_signal = false`（退 v7.7）。
8. 本轮**仍不包含**帧率达标：画面出来后才能在**不超频**条件下实测帧率并给「不超频 60 帧」结论（见第 5 节与待办 T26）。

### 3.13 v8.2（T50）验证清单 — 显示内存影子访存 + 设备分层解析（已被第 3.20 节取代，仅作对照）

实机（2026-10-30 回传 `rex_crash(36)` / `rex_stderr(68)` / `T2ROTF_001(66)` / `T2ROTF_002(47)`）确认
v8.1-GUESTCB **启动即 fatal**：两次崩溃同一 pc 偏移（`imagen+0x2e38088`，指令 `0xb9400041` = `LDR W1,[X2]`）、
同一 guest 地址 **`0x820011AC`**（`VdGlobalDevice` 槽）、`esr 0x92000006 / EC 0x24`；
落点是 `sdk/src/system/xmemory.cpp:571` 的 `Unhandled guest access violation`；
`far=0x7407B621AC` 与 `[DIAG] Guest memory arena mapped` 行吻合（`0x7385B61000 + 0x820011AC`）。
用户原话「本次启动直接报错，无 `rex_perfil.log` 生成」正是**宿主线程在第一轮派发尝试时就被致命路径终止**（进程在日志落盘前死掉）——是后果不是原因。

**T49 定论**：宿主把 `TranslateVirtual(0x820011AC)` 的结果**直接解引用**；而本移植的 chunk 在提交时**并不映射进窗口视图**，
只有 guest 自己首次访问（`RexGmFaultIn`）才映射 ⇒ 宿主直读「已提交但未映射 / 尚未提交」的窗口页必然 `translation fault`；
`AccessViolationCallback` 只处理 `kGuestPhysical` heap、不为窗口未提交页补齐 ⇒ 唯一安全读法是走 chunk 的 **shadow 别名**。

v8.2 的做法（`recomp/src/t2rotf_gpu_signal.{h,cpp}` 的宿主访存与设备解析分层，外加 `recomp/src/t2rotf_video_nativo.cpp` 两处钩子接入）：
1. 所有宿主侧 guest 访存一律改走 `RexGmShadowFor(window_vaddr)` 的 shadow 别名（SDK 异常处理器代客读写用的同一条通道）；
   取不到 shadow 就**跳过并计数**（`GPSIG3` 行的 `sinSombraL/sinSombraW`），**绝不直读直写、绝不代客提交**（代客提交会为不属于 guest 物理内存的地址造出假后端）。
2. 设备解析分层：cvar（G7）> `VdGlobalDevice` 双槽（shadow 安全读）> D3D 钩子观测 `r3`（新开关 G10）。
   新增 `NotificarDispositivo()`，在 `REX_HOOK_RAW(sub_822D4488)`（环提交外层）与 `REX_HOOK_RAW(sub_822D47A0)`（显示中断入口）钩子里把 `r3`（实机 `0x40070600`）上报，作为兜底来源；**槽优先、钩子兜底，两者不一致时以槽为准并明确标注**。
3. 新增第三行只读诊断 `[video] T2DIAG GPSIG3 hookDev=… sinSombraL=… sinSombraW=… sinSombraAvisada=…`（每 2 s）。
**本模块仍不写任何显示会计、不碰门/泵热路、`presenter()/provider()` 保持 null（画面仍由 Renut/deko3d 出）。** 详见 `T2ROTF_deko3d_显示内存影子访存与设备分层解析报告.md`。

1. 把新的 `T2ROTF.nro` + `T2ROTF.toml` 覆盖到 `sdmc:/switch/Transformers2ROTF/`，**完全退出再启动**。
   验收场景是**不超频手持**（60 帧结论只在**不超频**条件下给）。
2. **先确认版本**：日志里出现 `[video] T2ROTF BUILD v8.2-SHADOW (guest memory read through the chunk shadow alias; device resolution layered cvar>slot pair>D3D hook)`。
   （同日志里仍会有 `v7.6-RECON`/`v7.7-ALIGN`/`v8.0-GPUIRQ`/`v8.1-GUESTCB` 等旧标记行，它们都属于本 NRO，不是版本混装；
   应用自身 banner 仍是旧的 `v6.27-MAPFIX`，**不要用它判版本**。）
3. **是否仍崩**：`rex_crash.log` 应**不再生成**，且 `rex_perfil.log` 恢复正常生成（本版修的就是这条致命路径）。
4. 安装期新增行：`[video] T2ROTF GPSIG: 设备解析 来源=槽/钩子 slot=… cell=… dev=… hookDev=… sinSombra=…/…`
   （`来源=槽` 最理想；`来源=钩子` = 走兜底；不一致时会跟一句「（与钩子观测不一致，以槽为准）」）。
5. 期待（本轮验收点）：`[T2DIAG GPSIG …]` 的 **`irq` 与 `mmio_r` 必须增长**（v8.1 之前恒 0）、`r1951` 开始增长、`hostcb` 成功数增长、`ret` 追得上 `inv`；
   `[T2DIAG GPSIG3 …]` 的 `sinSombraL/W` 为 **0 或极小**（持续增长 = 该页确实未提交，走分支 B）；
   `[T2DIAG GPSIG2 …]` 的 `v16548`/`v16568` **脱离冻结（>5）**、`a16700` 增长、`cola284/cola288` 不再悬挂；
   `espera sub_824F3580 n=` **不再线性暴涨**；`PERRO2` 的 `dpump` 成功数脱离 0；`juego` fps **> 0**；**画面出现**三连 logo。
6. 需回传的日志行：`[video] T2ROTF BUILD v8.2-SHADOW …`、`[video] T2ROTF GPSIG:`（全部）、
   `[video] T2DIAG GPSIG …`（首末各一）、`[video] T2DIAG GPSIG2 …`（首末各一）、`[video] T2DIAG GPSIG3 …`（首末各一）、
   `[video] T2DIAG PERRO dev=…`（末条）、`[video] T2DIAG PERRO2 …`（末条）、`[video] T2DIAG PUERTA …`（末条）、
   `[video] T2DIAG espera sub_824F3580 …`（末条）、`rex_perfil.log` 的 `juego … fps` 与 `relojes:` 行、`rex_crash.log`（若产生）。
7. 判读分支：A（`irq`/`mmio_r`/`r1951` 增长、`sinSombraL=0`、`v16548` 脱冻）= 影子修复生效、显示中断链首次真正运行；
   B（`sinSombraL` 持续增长、`来源=钩子`）= 该 chunk 确实未提交，下一轮在同址建合法后端；
   C（`irq` 涨但 `mmio_r` 不涨）= 试 `sa2_renut_gpu_signal_cb_data=40070600`；
   D（`inv` 涨而 `ret` 不涨）= ISR 内部卡住，试 `sa2_renut_gpu_signal_hostcb_s1=true` 做 A/B；
   E（仍有 `rex_crash.log`）= 还有别的宿主直读点（`t2rotf_video_nativo.cpp` 的 `Leer32`/`Escribir32`、`PublicarRptr`），回传新崩溃的 `pc/far/esr`。
8. 回退：`sa2_renut_gpu_signal_dev_hook = false`（退 v8.1 的设备解析行为，影子访存修复保留）；再退 `sa2_renut_gpu_signal_hostcb = false`（退 v8.0）；再退 `sa2_renut_gpu_signal = false`（退 v7.7）。
9. 本轮**仍不包含**帧率达标：画面出来后才能在**不超频**条件下实测帧率并给「不超频 60 帧」结论（见第 5 节与待办 T26）。

### 3.14 v8.3（T53）验证清单 — 每帧提交驱动（已被 3.15 取代，仅作对照）

实机（2026-10-30 回传 `rex_perfil(32)` / `rex_stderr(69)` / `T2ROTF_001(67)` / `T2ROTF_002(48)`，用户描述
「**还是黑屏，第一个动画能播放但是有声音无画面**」）确认 v8.2-SHADOW **无崩溃、无缺页**（`GPSIG3` 的
`sinSombraL` 恒 2、`sinSombraW` 恒 0），且**代派发链四要素全部增长**：`[T2DIAG GPSIG]` 的
`vblanks 0→600`、`irq 0→578`、`r1951 0→578`、`mmio_r 0→578`、`hostcb …→/22`、`ret` 追得上 `inv`
⇒ **宿主已按 60 Hz 把显示 ISR 送进 guest**；旧「排空自旋」也不复存在（`YIELD total` 从 35,439,751 降到 40 后冻结）。

但画面依旧不出：`[T2DIAG SWAP n=1]` 全程**仅 1 次**、`disp16568` 冻在 5、`a16700=a16704=0`、`mmio_w=0`、
`cola284=0`、`cola288=1`（在飞孤儿未配平）⇒ **中断进来了，可 guest 自己的「每帧把工作交给显示管理器」
那次调用从未发生**：每帧提交侧一次都没产出。显示线程停在 `sub_82881848` 的
`NtWaitForSingleObjectEx(0xF8000064, alertable=1)` 立即返回热自旋（`espera` 20.78M，`lr=828818DC`），
GM 真阻塞在 `KeWaitForSingleObject`。

v8.3 的做法（`recomp/src/t2rotf_gpu_signal.cpp`，新增「每帧提交驱动」）：
1. 在垂直同步宿主线程的**每个真实刷新**里，替 guest 走一次它**自己的**每帧显示提交入口 `sub_82880978`
   （无参、纯 guest 全局状态驱动；经 `FunctionDispatcher` 调用，调用后完整还原宿主线程的 guest 上下文，
   只保留它对 guest 内存的副作用）——即把「谁来回调每帧提交」这一点补上。
2. 启动 **2.5 s** 内不驱动（`sa2_renut_display_drive_after_ms`），避开 boot/init；并带只读判活：一旦
   `dev+16568` 或 `base+284` 相对上次变了（画面管线自己动起来），立即**让位观察 3 次刷新**（不叠加提交，
   绝不把呈现做成双份），随后若静止再恢复驱动。
3. 当前显示对象 `*(0x82DBB608)` 不可用时**不驱动**并计数（`DRIVE` 行的 `noObj=`）；新增只读诊断
   `[video] T2DIAG DRIVE drive=… ok=… noObj=… fallos=… vivo=… produce=… 16548=… 16568=… 284=… 288=… mmio_w=…`（每 2 s）。
**本模块仍不写任何显示会计、不碰门/泵热路、不改 v8.2 已验证的 shadow 安全访存、`presenter()/provider()` 保持 null（画面仍由 Renut/deko3d 出）。** 详见 `T2ROTF_deko3d_每帧提交驱动修复报告.md`。

1. 把新的 `T2ROTF.nro` + `T2ROTF.toml` 覆盖到 `sdmc:/switch/Transformers2ROTF/`，**完全退出再启动**。
   验收场景是**不超频手持**（60 帧结论只在**不超频**条件下给）。
2. **先确认版本**：日志里出现 `[video] T2ROTF BUILD v8.3-FRAME (host drives the guest's own per-frame display commit sub_82880978 on every refresh; shadow-safe guest reads kept from v8.2)`。
   （同日志里仍会有 `v7.6-RECON`/`v7.7-ALIGN`/`v8.0-GPUIRQ`/`v8.1-GUESTCB`/`v8.2-SHADOW` 等旧标记行，它们都属于本 NRO，不是版本混装；
   应用自身 banner 仍是旧的 `v6.27-MAPFIX`，**不要用它判版本**。）
3. **是否仍崩**：`rex_crash.log` 应**不再生成**（v8.2 已修掉那条致命路径，本版不应引入新崩）。
4. **本轮验收点**：`[T2DIAG DRIVE …]` 的 **`ok=` 必须增长、`noObj=` 保持 0**；
   `[T2DIAG SWAP n=]`、`disp16568`、`mmio_w`、`base+284` 应开始变化（`284 a→b` 那行可见）；`juego` fps **> 0**、**画面出现**三连 logo。
5. 需回传的日志行：`[video] T2ROTF BUILD v8.3-FRAME …`、`[video] T2DIAG DRIVE …`（首末各一）、
   `[video] T2DIAG SWAP …`（末条）、`[video] T2DIAG PUERTA …`（末条）、`[video] T2DIAG espera sub_824F3580 …`（末条）、
   `[T2DIAG GPSIG …]`（首末各一）、`[video] T2ROTF GPSIG:`（全部）、`rex_perfil.log` 的 `juego … fps` 与 `relojes:` 行、`rex_crash.log`（若产生）。
6. 判读分支：A（`DRIVE ok=` 增长、`noObj=0`、`SWAP n=`/`disp16568`/`mmio_w` 开始变）= 驱动生效、每帧提交侧首次真正运行 ⇒ 看 fps 与画面；
   B（`DRIVE ok=0`、`noObj=` 持续增长）= 当前显示对象 `*(0x82DBB608)` 始终为空，驱动条件不满足 ⇒ 下一轮改判驱动点（见本轮报告第 7 节残余风险）；
   C（`ok=` 增长但 `SWAP n=`/`disp16568` 仍不变）= `sub_82880978` 被调用但没改变状态 ⇒ 需追该入口内部的前置判据（下一轮）；
   D（仍有 `rex_crash.log`）= 回传新崩溃的 `pc/far/esr`（前 40 行）。
7. 回退：`sa2_renut_display_drive = false`（退 v8.2-SHADOW，中断链通但每帧提交侧不产出）；再退 `sa2_renut_gpu_signal_dev_hook = false`（退 v8.1 设备解析，影子访存保留）；再退 `sa2_renut_gpu_signal = false`（退 v7.7）。
8. 本轮**仍不包含**帧率达标：画面出来后才能在**不超频**条件下实测帧率并给「不超频 60 帧」结论（见第 5 节与待办 T26）。

### 3.15 v8.4（T56）验证清单 — 每帧提交驱动分独立线程（已被 3.16 取代，仅作对照）

实机（2026-10-31 回传 `rex_perfil(33)` / `rex_stderr(70)` / `T2ROTF_001(68)` / `T2ROTF_002(49)`，用户描述
「**实机依旧黑屏**」）确认 v8.3-FRAME 在跑、**无崩溃、无缺页**（`GPSIG3` 的 `sinSombraL` 恒 2、`sinSombraW` 恒 0），
且 v8.3 的驱动**确实成功过一次**：`[T2DIAG DRIVE n=153 obj=40F604E0 284 0->1 288=0 16568=5 16548=132]`
（宿主把 guest 自己的每帧提交叫起来、`base+284` 由 0 变 1），管线真的跑了 6 帧
（`[T2DIAG PERRO]`：`bomba=iny=pres=6`、`etapa1/2/3=6/6/6`、`cadena937=8`、`despacho=8`，
`PUERTA`/`PRESENTAR` 各 1 次）。

**根因（自锁）**：同一条宿主**垂直同步派发线程** `t1969709635156801032` 的最后一条输出是
`00:12:48.048` 的 `[T2DIAG DRAIN-IN n=13 idx=0 c24=1 c28=0 e24=1 e28=0]`，此后 **60 s 再无一字**；
`[T2DIAG GPSIG]` 报告只到 `00:12:47.431 vblanks=120 irq=94 mmio_r=94 mmio_w=0`，**2 s 周期报告再也没来**；
`irq47=irq95=130` 冻结、`[T2DIAG SWAP n=1]` 全程仅 1 次、`fin=0/0/0`、`yield=88183` 冻住、
`espera sub_824F3580 n` 从 2.88e7 起仍单调增长。⇒ v8.3 把「替 guest 走每帧提交」放在**垂直同步派发线程**上，
而 guest 的每帧提交链会走到排空判据 `while (*(base+284) + *(base+288) != 0)` 上等一个显示完成，
本移植里这个完成的唯一来源正是**同一条线程**派发的显示 ISR ⇒ **同一线程自锁**，中断派发被自己掐掉，
完成侧 `fin` 永不产出，整链 6 帧后停摆（不是 guest 卡住、不是缺页）。

v8.4-SPLIT 的做法（`recomp/src/t2rotf_gpu_signal.cpp`，新增「每帧提交驱动线程」）：
1. 新增独立宿主线程 **`T2ROTF Dirigir`**（`XHostThread`，128 KB 栈），按 `hz` 节拍调 `DirigirFotograma`——
   即 v8.3 那套驱动逻辑**原样搬家**，但**只在这个线程上执行**（`thread_local g_en_hilo_dirigido` 守卫）。
2. 垂直同步线程从此**绝不进入 guest 代码**，只做「派发中断（`Interrupcion(0)`/`Completado`）+ 打报告」；
   `DirigirFotograma` 在 `sa2_renut_display_drive_thread=true` 且非驱动线程时**直接返回**。
   这样即使驱动卡在 guest 提交内部，中断派发照常进行，guest 的完成侧才有机会被释放（自锁解药）。
3. **让位语义原样保留**（`dev+16568` 或 `base+284` 一变即让位 3 次刷新，不叠加提交）；G12（`after_ms`）、
   G13（`div`）对驱动线程同样生效；当前显示对象 `*(0x82DBB608)` 不可用时跳过并计数。
4. 新增判读字段：`DRIVE` 行加 `ini=`（进入 guest 提交的次数）与 `dentro=`（驱动线程此刻是否停在里面）。
   **`ini > ok+fallos` 说明驱动线程当场卡在 guest 提交内**，一眼可判；驱动线程启动/停止各打一行 `T2ROTF GPSIG:` 日志。
5. 退出安全阀：若退出时驱动线程仍卡在 guest 提交内，`Shutdown` **不等待**它（只在日志点名），避免应用退出被拖死。
**本模块仍不写任何显示会计、不碰门/泵热路、不改 v8.2 已验证的 shadow 安全访存、`presenter()/provider()` 保持 null（画面仍由 Renut/deko3d 出）。** 详见 `T2ROTF_deko3d_每帧提交驱动分线程报告.md`。

1. 把新的 `T2ROTF.nro` + `T2ROTF.toml` 覆盖到 `sdmc:/switch/Transformers2ROTF/`，**完全退出再启动**。
   验收场景是**不超频手持**（60 帧结论只在**不超频**条件下给）。
2. **先确认版本**：日志里出现 `[video] T2ROTF BUILD v8.4-SPLIT (the per-frame display commit driver now runs on its own dedicated thread instead of the VSync dispatch thread: …)`，
   且出现 `[video] T2ROTF GPSIG: 每帧提交驱动线程已启动（独立于垂直同步派发线程；after_ms=2500 div=1）`。
   （同日志里仍会有 `v7.6-RECON`/`v7.7-ALIGN`/`v8.0-GPUIRQ`/`v8.1-GUESTCB`/`v8.2-SHADOW`/`v8.3-FRAME` 等旧标记行，
   它们是 cvar 帮助文本与历史判读行，都属于本 NRO，不是版本混装；应用自身 banner 仍是旧的 `v6.27-MAPFIX`，**不要用它判版本**。）
3. **是否仍崩**：`rex_crash.log` 应**不再生成**。
4. **本轮**最关键**验收点**：`[T2DIAG GPSIG vblanks=…]` 是否**持续**增长（v8.3 就是在 120 停住的）——
   `vblanks`/`irq`/`mmio_r` 不再冻结即说明**自锁已解除**；再看 `[T2DIAG DRIVE drive=… ok=… ini=… dentro=…]`
   的 `ini` 是否持续增长、`ini - ok - fallos` 是否稳定为 0（`dentro` 恒 0 = 每次驱动都正常返回）；
   最后看 `284`/`288`/`16568`/`mmio_w` 与 `[T2DIAG SWAP n=]` 是否开始增长、`juego` fps **> 0**、**画面出现**三连 logo。
5. 需回传的日志行：`[video] T2ROTF BUILD v8.4-SPLIT …`、`[video] T2ROTF GPSIG: 每帧提交驱动线程已启动/已停止 …`、
   `[video] T2DIAG DRIVE …`（首末各一）、`[video] T2DIAG SWAP …`（末条）、
   `[T2DIAG PERRO …]` 的 `fin=` 字段、`[T2DIAG GPSIG …]`（首末各一）、
   `[video] T2ROTF GPSIG:`（全部）、`rex_perfil.log` 的 `juego … fps` 与 `relojes:` 行、`rex_crash.log`（若产生）。
6. 判读分支：A（`vblanks` 持续增长 + `DRIVE ini=` 增长且 `dentro` 恒 0 + `SWAP n=`/`disp16568`/`mmio_w` 开始变 + fps>0 + 画面出现）= 自锁解除、画面出；
   B（`vblanks` 持续增长但 `ini` 增长而 `ok` 停滞、`dentro=1`）= 驱动线程卡在 guest 提交内（v8.3 的病只卡它自己）⇒ 回传该 `DRIVE` 行，下一轮改驱动点；
   C（`vblanks` 又冻结）= 自锁未解 ⇒ 回传该线程最后一条 `[T2DIAG …]`；
   D（仍有 `rex_crash.log`）= 回传新崩溃的 `pc/far/esr`（前 40 行）。
7. 回退：`sa2_renut_display_drive_thread = false`（退 v8.3-FRAME 行为，实机会自锁）；再退 `sa2_renut_display_drive = false`（退 v8.2-SHADOW，中断链通但每帧提交侧不产出）；再退 `sa2_renut_gpu_signal_dev_hook = false`（退 v8.1 设备解析，影子访存保留）；再退 `sa2_renut_gpu_signal = false`（退 v7.7）。
8. 本轮**仍不包含**帧率达标：画面出来后才能在**不超频**条件下实测帧率并给「不超频 60 帧」结论（见第 5 节与待办 T26）。

### 3.16 v8.5（T58/T59）验证清单 — 提交完成中断「解除抑制」（已被第 3.17 节取代，仅作对照）

实机（2026-10-31 回传 `rex_perfil(34)` / `rex_stderr(71)` / `T2ROTF_001(69)` / `T2ROTF_002(50)`，用户描述
「**还是没有画面**」）确认 **v8.4-SPLIT 已把自锁解除**：`[T2DIAG GPSIG]` 报告恢复**每 2 秒一条**（约 72 条），
`vblanks 0→2880`、`irq 0→2853`、`r1951 = mmio_r` 同步增长、`hostcb=2613/27`、`inv=ret=irq`。
但仍有四条并发现象：① 提交侧零产出（`mmio_w=0`、`wptr=0(0)`、`inKick13232=0`、`ringR16700=ringW16704=0`）；
② `dev+16568` 冻在 **5**、`cola284=0 cola288=1`、`a16700=a16704=0`、`obj=00000000`；
③ 驱动线程 `t682943169022225291` 只输出 4 行（`06:08:04.431–04.497`：`RECON nr=1` → `ALIGN n=5` →
`DRIVE n=153 obj=40F604E0 284 0->1` → `DRAIN-IN n=13`）后静默，周期报告恒 `drive=… ok=1 ini=2 dentro=1 produce=0`
（**只驱动成功 1 次、`dentro=1` 不归零**）；④ 呈现侧 `espera sub_824F3580` 涨到 **21,440,000+**，恒为
`r3=F8000068 lr=828818DC`。

**根因（单一闭环）**：宿主每帧驱动 guest 每帧提交 `sub_82880978` → guest 提交内部走到排空
`sub_82882200` 的 `while (*(base+284) + *(base+288) != 0)` 等一个显示完成 → 能在 guest 自己的 ISR 里
retire 掉在飞项的唯一通路是图形中断回调的 **source=1（提交完成）** 分支（`sub_822D47A0` 读
`*(dev+10900)`=swap 暂存块 → 调 `*(blk+16)` → retire 一项）→ 但这一支**一次都没派发过**
（`skipS1 == vblanks` 即 120/240/…/2880、`s1 == 0`、`dev+16568` 冻在 5）→ `base+288` 恒为 1 →
排空循环永不退出 → 驱动线程卡死（`dentro=1`）→ 提交侧零产出 → 呈现侧永等。
（另：present 等的对象是 `F8000068`（`*(0x82DBB5E0)`），不是 `F8000060`；`F8000068` 由 guest 自己 ISR 尾部在完成一项后置位，
所以正解是**把 source=1 中断真正派发进 guest ISR**，而非宿主替它写事件句柄。）
source=1 一次都没派的**两个原因**：① `sa2_renut_gpu_signal_hostcb_s1` 旧默认 **false**；② 旧判据要求暂存块落在
「guest 对象区间 `0x40000000–0x90000000`」，而实机 `*(dev+10900)=0xBDF80080`（D3D 后端自己的 fence/暂存块，
落在 `0xB…` GPU 物理段）被一律判成「不可用」而拒发。

v8.5-COMPLETE 的做法（`recomp/src/t2rotf_gpu_signal.cpp`，64,966 → 68,966 字节）：
1. `sa2_renut_gpu_signal_hostcb_s1` 默认 **false → true**；暂存块判据放宽为**只要求非零、4 字节对齐、落在可寻址 32 位范围内**
   （`!blk || (blk & 3u) != 0 || blk > 0xFFFFFFF0u` 才跳过）；派发时把 `*(blk+16)` 纳入 `cb_guard` 一次性守卫
   （非 guest 代码地址时本次派发期间临时清零、派发后还原），经 `Enviar(callback, 1, dev, cpu)` → `ExecuteInterrupt(..., source=1, datos=dev, 2)`
   走 guest 自己的图形中断回调。**绝不写它，只读 + 派发。**
2. 新增 `sa2_renut_gpu_signal_hostcb_s1_mode`（缺省 **2**）：`0`=永不派发；`1`=仅在飞显示项确实存在时派发
   （`*(0x82E55CE0+284) | *(0x82E55CE0+288) != 0`）；`2`=每次刷新都派发（与硬件「一帧一次完成中断」同节奏）。
3. `[T2DIAG GPSIG2 dev=…]` 只读补齐**门就绪判据的全部输入**：`ptr272`（`*(0x82E55CE0+272)` 槽表基址）、`c16`、
   `A0/A1/A2`（表 `[slot*32+24]`，已投递）、`C0/C1/C2`（表 `[slot*32+28]`，未交付）、`dv10904`（`*(dev+10904)`）、
   `blk0`/`blk4`（暂存块前 8 字节，全 0 = 无待消费完成记录）。
**本模块仍不写任何显示会计、不碰门/泵热路、不改 v8.2 已验证的 shadow 安全访存、`presenter()/provider()` 保持 null（画面仍由 Renut/deko3d 出）。** 详见 `T2ROTF_deko3d_提交完成中断解除抑制报告.md`。

1. 把新的 `T2ROTF.nro` + `T2ROTF.toml` 覆盖到 `sdmc:/switch/Transformers2ROTF/`，**完全退出再启动**。
   验收场景是**不超频手持**（60 帧结论只在**不超频**条件下给）。
2. **先确认版本**：日志里出现 `[video] T2ROTF BUILD v8.5-COMPLETE (submission-completed interrupts are no longer withheld: …)`。
   （同日志里仍会有 `v7.6-RECON`/`v7.7-ALIGN`/`v8.0-GPUIRQ`/`v8.1-GUESTCB`/`v8.2-SHADOW`/`v8.3-FRAME`/`v8.4-SPLIT` 等旧标记行，
   它们是 cvar 帮助文本与历史判读行，都属于本 NRO，不是版本混装；应用自身 banner 仍是旧的 `v6.27-MAPFIX`，**不要用它判版本**。）
3. **是否仍崩**：`rex_crash.log` 应**不再生成**。
4. **本轮最关键验收点**：
   - `[T2DIAG GPSIG …]` 里 **`s1` 是否开始增长**（v8.4 恒 0、`skipS1 == vblanks`）——增长即说明提交完成中断已被派发；
   - **`dev+16568`（`v16568`）是否脱离 5**、`[T2DIAG SWAP n=]` 是否 > 1、`mmio_w`/`inKick13232`/`ringR16700` 是否开始增长；
   - `[T2DIAG DRIVE drive=… ok=… ini=… dentro=…]` 的 `dentro` 是否回到 **0**（若仍恒 1 = 驱动线程仍卡在 guest 提交内）、`ini` 是否持续增长；
   - `fin=`/`disp16568` 是否开始增长、`espera sub_824F3580` 是否停止单调增长、`juego` fps 是否 **> 0**、**画面是否出现**三连 logo。
5. 需回传的日志行：`[video] T2ROTF BUILD v8.5-COMPLETE …`；`[video] T2ROTF GPSIG:`（全部，特别是 `宿主代客派发 source=1（提交完成）callback=… dev=… blk=… blk16=…` 那条）；
   `[T2DIAG GPSIG …]`（首末各一，重点 `s1`/`mmio_w`/`skipS1`）；`[T2DIAG GPSIG2 dev=…]`（首末各一，重点 `A0/C0/A1/C1/A2/C2`、`dv10904`、`blk0/blk4`）；
   `[T2DIAG DRIVE …]`（首末各一）；`[T2DIAG SWAP …]`（末条）；`[T2DIAG PERRO …]` 的 `fin=`；
   `rex_perfil.log` 的 `juego … fps` 与 `relojes:` 行（须未超频值）；`rex_crash.log`（若产生）。
6. 判读分支：A（`s1` 增长 + `v16568`/`SWAP n=`/`mmio_w` 开始变 + `dentro=0` + fps>0 + 画面出现）= 修复生效、画面出；
   B（`s1` 增长但仍 `dentro=1`、`cola288` 仍恒 1）= 提交完成中断到了 guest 但 guest 自己的 retire 路径没走通 ⇒ 回传那条 `source=1` 派发行的 `blk/blk16` 与 `GPSIG2` 末行，下一轮改用 `mode=1` 并核对 ISR 内部；
   C（`s1` 仍恒 0）= 派发仍未发生（`dev+10900` 为空或对齐判据仍挡）⇒ 回传 `GPSIG2` 的 `blk10900`/`blk16`；
   D（仍有 `rex_crash.log`）= 回传新崩溃的 `pc/far/esr`（前 40 行）。
7. 回退：`sa2_renut_gpu_signal_hostcb_s1 = false`（退 v8.4-SPLIT，提交完成中断重新被抑制）；再退 `sa2_renut_display_drive_thread = false`（退 v8.3-FRAME，实机会自锁）；再退 `sa2_renut_display_drive = false`（退 v8.2-SHADOW）；再退 `sa2_renut_gpu_signal_dev_hook = false`（退 v8.1 设备解析，影子访存保留）；再退 `sa2_renut_gpu_signal = false`（退 v7.7）。
8. 本轮**仍不包含**帧率达标：画面出来后才能在**不超频**条件下实测帧率并给「不超频 60 帧」结论（见第 5 节与待办 T26）。

### 3.17 v8.6（T61/T62）验证清单 — 呈现记账「交回 guest 自己」（已被第 3.20 节取代，仅作对照；本节曾取代第 3.16 节，3.16 的机制已离线证伪）

1. 动作：把新的 `T2ROTF.nro` + `T2ROTF.toml` 覆盖到 `sdmc:/switch/Transformers2ROTF/`，**完全退出再启动**；验收场景是**不超频手持**。
2. **本节为什么必须这样做**（T61 离线逐指令精读，三条定论都有生成代码行号可核）：
   - **第 3.16 节「派发 source=1 提交完成中断就能 retire 在飞显示项」的前提不成立**：`sub_822D47A0`（`T2ROTF_recomp.9.cpp:21582`）的 `r3==1` 分支只做「`*(blk+16)` 非 0 才 `bctrl *(blk+16)`」+ 清 `*(blk+0)` 里当前 CPU 的在飞位，**既不读写 `base+284`/`base+288`，也不碰依赖表 `+24/+28`**；而 `*(blk+16)` 在本移植里从未被写非 0（日志 `blk0=0 blk4=0`）⇒ 该分支等于直接返回。
   - 真入口是同一回调的 **`r3==0` 分支**：读 MMIO `0x7FC86544`，`& 1` 非 0 才 `bl 0x822d9570(dev)`（= 每拍的显示/vblank 记账）。它**一直在跑**（实机 `v16548` 涨到 2738），但它排空的换页环**恒空**（`ringR16700 = ringW16704 = 0`）⇒ 永远进不了环体 ⇒ `v16568` 不涨、`*(*(dev+10900)+4)` 永不清零。
   - **真正缺的是记账者**：`sub_822D9688`（`T2ROTF_recomp.32.cpp:20832` = `*(dev+16564)++`、条件满足时 `*(dev+16568)++` 并 `MMIO 0x7FC86110 = 表面页` 或 `*(*(dev+10900)+4) = 0`）**零静态调用者**（全工程只有 `T2ROTF_init.cpp:3890` 注册，没有任何 `bl 0x822d9688`）；它的原生调用者（guest 自己的 Swap/提交路径）被宿主 `D3DDevice_Swap_hook` 整体替换，而宿主那套 `T2AdvanceDisplayAccounting()` 是**对同一批字段的宿主重实现**（属本工程明令禁止的「写显示会计字段」），**只在 Swap 时被调** —— Swap 在第 1 帧就停（`[T2DIAG SWAP n=1]`）⇒ `16564`/`16568` 冻在 5。这正是 v8.5 实机「`s1` 涨了但 `dentro` 仍 1、提交侧零产出」的原因。
3. 因此本版把记账**交回 guest 自己**：宿主在**每个垂直同步周期**（`sa2_renut_gpu_signal` 的派发线程内、与既有 `source=0` 中断派发同一线程同一时机）以 **`r3 = r4 = r5 = 0` 代客调用 `sub_822D9688`**（`ExecuteTrap`）：`*(dev+16564)++`、`*(dev+16568)++`、`*(*(dev+10900)+4) = 0` **全部由 guest 代码在它自己的自旋锁内完成**，宿主**不写任何 guest 字段**；同时把宿主那套 `T2AdvanceDisplayAccounting()` 默认关掉（`sa2_renut_display_agent`，默认 false）。安全阀：调用前用 shadow 安全读核对设备双槽（`0x820011AC → cell → dev`），对不上就**跳过并只报一行**（`saltados`）。
   - 注意 `r3=0` 的语义：要么当拍直接 `16568++` 且 `*(blk+4)=0`，要么把条目压入换页环、由**下一拍**的 `sub_822D9570` 环排空完成 —— 所以 `blk4` 归 0 可能**晚一拍**，判读时以多个采样点为准。
4. 本轮最关键验收点：
   - `[video] T2DIAG PRESENT n=… dev=… 16564=… 16568=… 16548=… ringW16704=… blk10900=… blk4=… saltados=…` 是否出现、`n` 是否随刷新周期增长、**`16568` 是否脱离 5**、**`blk4` 是否由非 0 变 0**（= 提交完成字被 guest 自己清掉的直接证据）；
   - `[T2DIAG SWAP n=]` 是否 > 1、`mmio_w`/`inKick13232`/`ringR16700` 是否开始增长；
   - `[T2DIAG DRIVE …]` 的 `dentro` 是否回到 **0**（不再卡在 guest 提交内）、`fin=`/`disp16568` 是否增长、`espera sub_824F3580` 是否停止单调增长；
   - `juego … fps` 是否 **> 0**、**画面是否出现**三连 logo；`saltados` 一直涨而 `n` 不涨 = 设备槽核对没过（回传 `GPSIG2` 的 `A0..C2`）。
5. 需回传的日志行：`[video] T2ROTF BUILD v8.6-FLIP …`；**全部** `[video] T2DIAG PRESENT …`；`[video] T2ROTF GPSIG: v8.6 代客呈现记账暂不调用：…`（若有）；`[T2DIAG GPSIG …]` 与 `[T2DIAG GPSIG2 dev=…]`（首末各一，重点 `A0..C2`、`dv10904`、`blk0/blk4`）；`[T2DIAG DRIVE …]`（首末各一）；`[T2DIAG SWAP …]`（末条）；`[T2DIAG PERRO …]` 的 `fin=`；`rex_perfil.log` 的 `juego … fps` **与** `relojes:` 行（须未超频值）；`rex_crash.log`（若产生）。
6. 判读分支：A（`16568` 涨 + `blk4` 归 0 + `dentro` 回 0 + `mmio_w`/`SWAP n=` 变 + fps>0 + 画面出现）= 缺环补上；
   B（`16568` 涨、`blk4` 归 0，但 `cola284`/`base+284` 仍恒 1）= 在飞显示项的释放者另有其人（下一轮冲 guest 的 GPU 提交完成路径 `sub_822D9838`/`sub_822D98E8`，以及 `VdSetSystemCommandBufferGpuIdentifierAddress` 注册的地址是否就是 `blk+4`）；
   C（`PRESENT` 不出现或 `saltados` 持续涨）= 设备槽核对没过；D（有 `rex_crash.log`）= 回传 `pc/far/esr` 前 40 行。
7. 回退：`sa2_renut_display_present_guest = false`（退 v8.6 → 回到 v8.5：记账重新无人调用）；再退 `sa2_renut_gpu_signal_hostcb_s1 = false`（退 v8.4 语义：提交完成中断重新被抑制——T61 已证其为空操作，仅作 A/B）；再退 `sa2_renut_display_drive_thread = false`（退 v8.3，实机会自锁）；再退 `sa2_renut_display_drive = false`（退 v8.2-SHADOW）；再退 `sa2_renut_gpu_signal_dev_hook = false`（退 v8.1）；再退 `sa2_renut_gpu_signal = false`（退 v7.7）。
8. 本轮**仍不包含**帧率达标：画面出来后才能在**不超频**条件下实测帧率并给「不超频 60 帧」结论（见第 5 节与待办 T26）。

### 3.18 v8.7（T63/T64）验证清单 — 修复「启动后直接报错退出、没有任何加载」（已被第 3.19 / 3.20 节取代，仅作对照；本节只修启动崩溃，未回滚）

1. 动作：把新的 `T2ROTF.nro` + `T2ROTF.toml` 覆盖到 `sdmc:/switch/Transformers2ROTF/`，**完全退出再启动**；验收场景是**不超频手持**。
2. 现象与定论（2026-10-31 实机两次运行，`rex_crash(37).log` + `T2ROTF_001(70).log` / `T2ROTF_002(51).log`）：
   - 崩溃现场：`Unhandled guest access violation: read of guest 0x2A91`；`pc = imagen+0x2e28544 = VolcarGuest+0x2c4`、`lr = imagen+0x2e29240 = VolcarGuest+0xfc0`（`imagen+X` 即 ELF vaddr，delta=0）；`esr 0x92000005`、指令 `0xb8604b54`（= `ldr w20,[x26,w0,uxtw]`）；`x19 = 1`、`x8 = 0x2A90`、`x9 = 0x2A94`、`x0 = 0x2A91`、`x1 = 0x2A95`；`far`（run1 `0x5762C50A91` / run2 `0x72602B8A91`）= arena base + `0x2A91`。
   - 逐项自洽：`x0 = dev + 0x2A90`、`x1 = dev + 0x2A94`，而 `x19 = dev = 1` ⇒ `dev + 10896 = 0x2A91`。
   - 根因：**中断回调 `sub_822D47A0` 的 guest ABI 是 `(r3 = 来源, r4 = 设备)`**（实机 `irq sub_822D47A0 n=2 r3=00000001 r4=40070600` 与 `n=3 r3=00000000 r4=40070600` 按 ~16 ms 交替，互证），旧钩子 `if (ctx.r3.u32) g_dev_visto.store(ctx.r3.u32, …)` 把**来源值**当设备；v8.5/v8.6 新增「每垂直同步派发 `source=1` 提交完成」后，`g_dev_visto` 被 60 Hz 持续写成 `1`（`sub_822D4488` 钩子偶尔写回 `40070600` 压不过它）。
   - 触发点：泵钩子里的 `VolcarGuest("pump")` 与看门狗 `VolcarGuest("perro")` 的 `PERRO dev=…` 行在**参数求值**时就裸读了 `dev + 10896` ⇒ xmemory 致命路径 ⇒ 进程**启动即退出、零加载**。（`REXLOG_ERROR` 的参数求值先于限频判断，所以与「限频只打前 n 行」无关。）
3. 本版改动（**只改 `recomp/src/t2rotf_video_nativo.cpp` 与 `recomp/src/t2rotf_gpu_signal.cpp`，不新增 cvar、不改任何默认值**）：
   - `sub_822D47A0` 钩子：设备候选改为 **`r4`**，且经 `EsDireccionDispositivo()`（非零 + 4 字节对齐 + `0x10000000..0xFFFFFFF0`）校验通过才 `g_dev_visto.store`，不健全即 `g_irq_dev_avisado++` 并限频告警；观测行改为 `n=… fuente=… dev=…`。
   - `sub_822D9570` 钩子：设备候选优先 `r3`、不健全退 `r4`，校验通过才落盘，并用**校验后的值**调 `NotificarDispositivo`。
   - `sub_822D4488` 钩子：`g_dev_visto` 落盘前加同一校验。
   - `sub_822D13F0` 钩子：`dev`（从 guest 读出的设备指针）与 `p`（GPU 读指针）在解引用前都加校验。
   - `VolcarGuest`：`ctx`/`dev` 读前一律校验，不健全即 `g_perro_saltos++` 并限频打印一行「不是设备地址（守卫跳过，不再裸读 guest）」；`f10896`/`f10900` 从设备读出后先校验再当指针用。
4. 本轮最关键验收点：
   - **不再产生 `rex_crash.log`**，且 `[video] T2DIAG PERRO dev=40070600 …` 一行能**正常打印**（原崩溃点被修好的直接证据）；
   - `irq sub_822D47A0 n=… fuente=0/1 dev=40070600`（设备恒为 `40070600`，不再被来源值覆盖）；
   - `OnInitialize: LaunchModule done` 之后能进到影片/呈现阶段（第 3.17 节的全部验收点照旧生效）。
5. 需回传的日志行：`[video] T2ROTF BUILD v8.7-IRQABI …`；`[video] T2DIAG irq sub_822D47A0 …`（前 12 条）与 `… 设备候选 … 不健全 …`（若有）；`[video] T2DIAG PERRO dev=40070600 …`（首末各一）；**全部** `[video] T2DIAG PRESENT …`；`[T2DIAG GPSIG …]` / `[T2DIAG GPSIG2 dev=…]`（首末各一）；`[T2DIAG DRIVE …]`（首末各一）；`[T2DIAG SWAP …]`（末条）；`rex_perfil.log` 的 `juego … fps` 与 `relojes:` 行（须未超频值）；`rex_crash.log`（若产生）。
6. 判读分支：A（`PERRO dev=40070600` 正常、无 `rex_crash.log`、能进影片阶段）= 启动崩溃已修，问题回到第 3.17 节的画面链；
   B（仍有 `rex_crash.log`）= 回传新 `pc/far/esr` 前 40 行；C（`PERRO … 不是设备地址` 一直涨）= 转储守卫生效但设备解析仍错，回传 `GPSIG2` 的 `A0..C2` 与 `dev`。
7. 回退：本版**没有可回退的开关**（修的是误读 ABI 的 bug，不是新增机制）；如需回到上一版行为，用 `recomp/out/sw8/zip_backup/` 下的快照包。
8. 本轮**仍不包含**帧率达标：启动崩溃修好后仍需在**不超频**条件下实测帧率（见第 5 节与待办 T26）。

### 3.19 v8.8（T65/T66）验证清单 — 修复「依旧黑屏有声」：驱动线程被 v8.6 自己伪造的记账饿死（**已被第 3.20 节取代，仅作对照**；第 3.18 节只修启动崩溃，未回滚）

1. 动作：把新的 `T2ROTF.nro` + `T2ROTF.toml` 覆盖到 `sdmc:/switch/Transformers2ROTF/`，**完全退出再启动**；验收场景是**不超频手持**（`rex_stderr` 里应能看到 `[apm] … GPU 460.8 MHz … memoria 1600.0 MHz`）。
2. 现象与定论（2026-11-01 实机，用户回传 `rex_crash(38).log` / `rex_stderr(73).log` / `T2ROTF_001(71).log` / `T2ROTF_002(52).log`，用户描述「依旧黑屏有声」）：
   - **启动崩溃确已修复**：`rex_crash(38).log` 与上一轮 `rex_crash(37).log` **逐字节相同**（旧文件被重复上传，不是新崩溃）；两份运行日志的第 31/32 行都是 `[video] T2ROTF BUILD v8.7-IRQABI (…)`；设备解析恒正确（`irq sub_822D47A0 n=… fuente=0/1 dev=40070600`，`fuente` 按刷新在 0/1 交替）、`不是设备地址` / `设备候选` **0 命中**（地址守卫从未跳过）；`rex_stderr` 走完 `[apm] … GPU 460.8 MHz … memoria 1600.0 MHz` 与 `OnInitialize: LaunchModule done`，之后无 crash hook。
   - **中断派发链健康**：`[T2DIAG GPSIG]` 由 `vblanks=0 irq=0 s1=0` 涨到 `vblanks=8882 irq=17710 r1951=8855 mmio_r=8855 mmio_w=0 hostcb=8855/27 s1=8855 skipS1=27 inv=17710 ret=17710`（`inv == ret`）。
   - **黑屏有声的根因（T66 定论）**：`[T2DIAG DRIVE]` 首行 `drive=0 ok=0 ini=0 dentro=0 noObj=0 fallos=0 vivo=0 produce=0 16548=0 16568=0 284=0 288=0 mmio_w=0`、末行（002）`drive=0 ok=0 … vivo=8733 produce=0 16548=8855 16568=8855 284=0 288=1 mmio_w=0`（001 末行 `vivo=1531 16548=1653 16568=1653`），且 **`16568` 恒等于 `16548`**（末态 `PERRO vblank16548=8894 fr16564=8894 disp16568=8894`）⇒ v8.6-FLIP 的代客呈现记账 `sub_822D9688` **每刷新自增 `dev+16568`**，而 v8.3/v8.4 的让位判据把 `dev+16568` 也算作「guest 管线在动」⇒ **驱动线程每一轮都让位**（`drive=0`、`dentro=0`、`vivo` 只在涨）⇒ guest 侧零产出（`base+284` 恒 0、`mmio_w=0`、`ringR16700=ringW16704=0`、`inKick13232=0`、管线 6 帧后冻结）⇒ 呈现线程在帧事件 `*(0x82DBB5E0)=F8000068` 上空转（`espera sub_824F3580 n=12,680,000 / 68,680,000 lr=828818DC`）⇒ `SWAP` 全程 `n=1` ⇒ 我们注入的影片帧无处上屏，而 XAudio 侧照常有声。
3. 本版改动（**只改 `recomp/src/t2rotf_gpu_signal.cpp`；不新增按键、仍为 24 键，只把 G14 的默认值 `false → true`**）：
   - **让位判据只认 `base+284`**：`284 == 0`（没有任何待交付帧）才驱动；`284 != 0`（已有帧在等 guest 自己的消费者）让位；`base+284` 连续 **45** 次刷新（`kTicsEstancado = 45`，60 Hz 下约 0.75 s）不动时**仍驱动一次**去消化它。**`dev+16568` 彻底移出判据**（`vivo_16568_` 仅留档）。位置 `t2rotf_gpu_signal.cpp:1057-1082`。
   - **G14 `sa2_renut_display_drive_produce` 默认 `true`**：走完 guest 每帧提交入口 `sub_82880978` 之后，**若 `base+284` 仍为 0**，补调一次 guest 原生生产者包装 `sub_82882730`（入队 + 自增 `base+284` + 置位帧事件 `*(0x82DBB5E0) = F8000068`）；`284 != 0` 时**绝不补调**。位置 `:1115-1131`。
   - **v8.8b（交付前自查发现并修掉的两处自身缺陷）**：① **补产入参不得用 `r3 = 0`** —— 生产者本体 `sub_822B5608` 首条真实指令是 `lwz r10,64(r3)`、随后 `stw r11,56(r3)`，`r3=0` 会读写 guest `0x40`/`0x38`；现改为**先调 guest 自己的解析器 `sub_828823F8`**（`kResolverPresente = 0x828823F8`，在 `*(0x82DBB5C4)` 起每条 296 字节的记录数组里找 `+0 == 5` 的那一条）取「当前呈现记录」，取到且通过 `PlausibleObjeto()` 才作 `r3` 调生产者，取不到就跳过并计 **`prodSinDev=`**。② **当前显示对象 `*(0x82DBB608)` 只计数、不再当闸门** —— v8.7 实机日志里该字段 **74/75 次采样恒为 0**，若保留提前 `return`，驱动会被永久拦下（正是本轮要修的症状）；依据是每帧提交入口 `sub_82880978` 的重编译体（`T2ROTF_recomp.21.cpp:94058`，len=71）只调 `sub_82B22628` / `sub_8287EDC0` / `sub_82ABB7D0` / `sub_82880C30` 并复位 `0x82DBB6B4` / `0x82DBB6B8`，**从不读 `0x82DBB608`** ⇒ `obj == 0` 时驱动安全；下游对空显示对象的解引用自 v7.5 起由 `sa2_renut_display_guard` 拦下。
   - **诊断增强**：`[T2DIAG DRIVE drive=… ok=… ini=… dentro=… noObj=… fallos=… vivo=… produce=… prodSinDev=… estanc=… 16548=… 16568=… 284=… 288=… obj=… ev=… mmio_w=…]`（每 2 s 一行）；逐次行新增 `saltos_vivo=…` / `estancados=…`。
   - **硬约束**：宿主仍**一个字都不写** `284` / `288` / `16548` / `16552` / `16564` / `16568`；所有 guest 访存仍走 `RexGmShadowFor` 影子别名；驱动仍只在独立线程 `T2ROTF Dirigir` 上跑；不碰门/泵热路；v8.7-IRQABI / v8.6-FLIP / v8.4-SPLIT / v8.3-FRAME / v8.2 影子安全访存与让位语义全部保留。
4. 本轮最关键验收点（对照 v8.7 的 `drive=0 / ok=0 / produce=0 / vivo=8733 / 284=0 / SWAP n=1`）：
   - `[T2DIAG DRIVE]` 的 `drive` / `ok` / `produce` **是否 > 0**（这是本轮要改的病）；
   - `prodSinDev` 与 `estanc` 是否开始计数（分别说明「补产被跳过」「待交付帧卡住仍驱动」这两条路径是否被走到）；
   - `284` 是否由 **0 变成非 0**、`[T2DIAG SWAP n=…]` 的 `n` 是否增长（> 1）、`[T2DIAG GPSIG …]` 的 `mmio_w` / `inKick13232` / `ringR16700` 是否开始增长；
   - 呈现线程的 `[T2DIAG espera sub_824F3580 n=…]` 是否**停止单调增长**（停止 = 帧事件 `F8000068` 已被置位、呈现链开始推进）；
   - `obj=`（`*(0x82DBB608)`）是否非 0、`ev=`（`*(0x82DBB5E0)`）是否变成 `F8000068`；
   - 最后：**画面是否出现**（三连 logo `luxo → atvi → hasbro`）与 `juego … fps` 是否 **> 0**。
5. 需回传的日志行：`[video] T2ROTF BUILD v8.8-LIVENESS …`；`[T2DIAG DRIVE …]`（**首末各一条**，重点 `drive` / `ok` / `produce` / `prodSinDev` / `estanc` / `vivo` / `dentro` / `284` / `288` / `obj` / `ev`）；`[T2DIAG DRIVE n=… 284 {}->{} …]`（前三条 + 末一条）；`[T2DIAG GPSIG …]`（首末各一）；`[T2DIAG SWAP …]`（末条）；`[T2DIAG espera sub_824F3580 …]`（末条）；`[T2DIAG PERRO dev=40070600 …]`（首末各一）；`rex_perfil.log` 的 `juego … fps` 与 `relojes:` 行（须未超频值）；`rex_crash.log`（若产生，附 `pc` / `far` / `esr` 前 40 行）。
6. 判读分支：A（`drive > 0` 且 `ok > 0`、`284` 出现非 0、`SWAP n > 1`、`espera` 停止增长、画面出现、`fps > 0`）= 缺环补上，进入「不超频 60 帧」实测；
   B（`drive > 0` 但 `284` 仍恒 0、`produce` 在涨）= 连 guest 原生生产者调用后都没能入队（下一轮冲 `sub_822B5608` 的入队前置条件与 `0x82DBB5E4`/`0x82DBB5D4` 两个逃逸标志的写入者）；
   C（`284` 变非 0 但 `SWAP n` 仍 1）= 消费者没被唤醒（下一轮冲 `base+284` 的**释放者**：`sub_822D9838`/`sub_822D98E8` 与 `VdSetSystemCommandBufferGpuIdentifierAddress` 注册地址）；
   D（`drive > 0`、`produce = 0`、`prodSinDev` 持续涨）= `sub_828823F8` 解析不出合法呈现记录，根因上移到「谁建立 `*(0x82DBB5C4)` 里 `+0 == 5` 的那条记录」（注意：`noObj`/`obj=0` **单独为 0 不构成**问题判据，v8.8b 已不把它当闸门）；
   E（仍有 `rex_crash.log`）= 回传新 `pc` / `far` / `esr` 前 40 行。
7. 回退：`sa2_renut_display_drive_produce = false`（退回「只驱动提交入口、不补产」）→ `sa2_renut_display_present_guest = false`（退 v8.5 语义：呈现记账重新无人调用）→ `sa2_renut_gpu_signal_hostcb_s1 = false`（退 v8.4 语义，T61 已证其为空操作）→ `sa2_renut_display_drive_thread = false`（退 v8.3，实机会自锁）→ `sa2_renut_display_drive = false`（退 v8.2-SHADOW）→ `sa2_renut_gpu_signal_dev_hook = false`（退 v8.1）→ `sa2_renut_gpu_signal = false`（退 v7.7）。**本版判活判据本身没有专用开关**（改的是判据），要整版回退请用 `recomp/out/sw8/zip_backup/` 下的快照包：上一版 `T2ROTF_switch_port_20261101_v87_pre.zip`、本版首包快照 `T2ROTF_switch_port_20261101_v88_pre.zip`。
8. 本轮**仍不包含**帧率达标：画面出来后才能在**不超频**条件下实测帧率并给「不超频 60 帧」结论（见第 5 节与待办 T26）。

### 3.20 v8.9（T68/T69/T70）验证清单 — 修复「黑屏、第一个 LOGO 有声」：补产排在提交之后，一进提交入口就永久卡在 guest 显示回调（**本节是当前有效机制**；第 3.19 节降为对照）

1. 动作：把新的 `T2ROTF.nro` + `T2ROTF.toml` 覆盖到 `sdmc:/switch/Transformers2ROTF/`，**完全退出再启动**；验收场景是**不超频手持**（`rex_stderr` 里应能看到 `[apm] … GPU 460.8 MHz … memoria 1600.0 MHz`）。
2. 现象与定论（2026-11-01 实机，用户回传 `rex_perfil(35).log` / `rex_stderr(74).log` / `T2ROTF_001(72).log` / `T2ROTF_002(53).log`，用户描述「黑屏，但是第一个 LOGO 有声音」）：
   - **不是崩溃、不是超频、不是缺页**：无 `rex_crash` 文件；`rex_stderr` 里 `[apm] PUERTA la configuracion 0x92220008 (tabla: GPU 460, EMC 1331): GPU 460.8 MHz (era 307.2)` 证明**未超频**；`OnInitialize` 五段全过、无启动期 fatal（`v6.27-MAPFIX crash hooks init ok` 是**常量串不是版本**，判版本只认 `[video] T2ROTF BUILD …` 行）。
   - **音频独立于显示链**：`rex_perfil` 的 `audio: 8 bloques de cliente mezclados, 0 solicitudes sin datos, 1/1 buffers audout con PCM no nulo, 0 muestras saturadas … pico maximo 0.474`，而 `juego 0.0 fps` ⇒ **有声无画**。
   - **中断派发链健康**：`[T2DIAG GPSIG]` 的 `vblanks` / `irq` / `mmio_r` 持续增长、`inv == ret`（v8.4 的线程自锁确已解除）。
   - **驱动线程只成功进过 guest 一次、第二次再没回来**：`[T2DIAG DRIVE]` 首行（17.606）`16548=94 16568=94 284=0 288=1 obj=40F604E0 ev=F8000068`；尾部（33.605 / 35.605 / 37.606 三行完全相同）`drive=2 ok=1 ini=2 dentro=1 noObj=0 fallos=0 vivo=45 produce=0 prodSinDev=0 estanc=1 16548=1174 16568=1174 284=0 288=1 obj=41370080 ev=F8000068 mmio_w=0` ⇒ **卡死的驱动线程连让位判据都轮不到**，补产一次没跑（`produce=0 prodSinDev=0`）。
   - **卡点 = 排空链把这一帧交付出去之后，卡在该项的呈现回调里不返回**：`[T2DIAG DRAIN-IN]` 共 13 条，最后一条（19:09:18.865）`n=13 idx=0 c24=1 c28=0 e24=1 e28=0` 之后**再无出栈**。离线逐指令精读 `sub_82881E00`：它 `bctrl` 调 item+60 槽的虚表呈现回调，**回调返回之后**才 `atomic_dec(manager+288)`；实机 `base+288` 自 17.597 起恒为 1 ⇒ 该减法从未执行 ⇒ 泵卡在回调里 ⇒ 驱动线程永久 `dentro=1`。
   - **黑屏的物理原因 = `284=0 288=1` 恒定**：`[T2DIAG PUERTA]` n=17（16.730）曾出现 `284=0 288=0`（说明呈现回调**曾经能返回**），17.597 起恒 `284=0 288=1`；显示管理器里三处排空判据 `while (*(base+284) + *(base+288) != 0)`（`sub_82882200` / `sub_828828E0` / `sub_82882740`）**永远转不出去**；呈现线程 `sub_82881848` 因 `284 == 0` 在帧事件 `*(0x82DBB5E0)=F8000068` 上**零超时热轮询**（`[T2DIAG espera]` 末条 `n=9780000 r3=F8000068 r5=0 lr=828818DC`）；`[T2DIAG SWAP]` 全程 `n=1` ⇒ 注入的影片帧无处上屏 ⇒ **黑屏**。另一佐证：`YIELD total` 顶部 `[82881DE8 x582875] [82881918 x582860]` 在 19:09:31 冻结，而 `espera` 继续涨到 19:09:38 ⇒ 有线程卡在某次 `sub_824F0620` 内未返回（与上面的推定一致）。
   - **机制侧定位（宿主代码）**：`DirigirFotograma` 里 **line 1100 无条件**调提交入口 `sub_82880978`（**无 cvar 开关**），而补产在 **line 1122**、**排在提交之后** ⇒ 提交一次即永久 `dentro=1`、生产支路一次未走，这正是 `produce=0 prodSinDev=0` 的机制。
3. 本版改动（**只改 `recomp/src/t2rotf_gpu_signal.cpp`；79,963 → 93,103 字节；新增 3 键，24 → 27 键**）：
   - **顺序反转**：`DirigirFotograma` 改为「**先投递**（`ProducirFotograma`，只在 `base+284 == 0` 时投递）→ **再可选提交**（`sub_82880978`）→ **最后可选有界交接**」。
   - **G17 `sa2_renut_display_drive_commit` 默认 `false`**：把每帧提交入口 `sub_82880978` 包起来，默认**不再驱动**（实机已证：一进该入口就把驱动线程永久留在 guest 显示回调里）。
   - **G18 `sa2_renut_display_drive_pump_max` 默认 `0`（关）**：有界**非阻塞**交接，最多 N 次调 `sub_82881E00(r3 = 0x82E55E20, r4 = -1, r5 = 1)`（`r5 = 1` 保证门不就绪即刻返回）；为此新增带 `r4`/`r5` 的 `DirigirEntradaArgs` 变体（旧 `DirigirEntradaCon` 把 `r4`/`r5` 置 0）。**默认 0** 是因为「宿主替 guest 叫显示回调」正是 v8.8 卡死的来源，交接默认交回 guest 自己的呈现线程。
   - **G19 `sa2_renut_display_drive_watchdog_ms` 默认 `500`**：驱动进 guest 超阈值就落**一次** `[T2DIAG DRIVE-WEDGE]`（含 `fase=` 相位名 + 设备/上下文/事件/暂存块全量快照）并**永久停用**驱动（`muerto=1`），避免后续日志全是空转。
   - **补产前移**：`ProducirFotograma` 走 guest 自己的解析器 `sub_828823F8` 取「当前呈现记录」（`*(0x82DBB5C4)` 起每条 296 字节、`+0 == 5`），过 `PlausibleObjeto()` 后作 `r3` 调 `sub_82882730`（入队 + 自增 `base+284` + 置位帧事件 `*(0x82DBB5E0)`）；取不到即**跳过**并计 `prodSinDev`，**绝不以 `r3 = 0` 调用生产者本体**（它首条 `lwz r10,64(r3)` / `stw r11,56(r3)` 会读写 guest `0x40`/`0x38`）。
   - **诊断增强**：`[T2DIAG DRIVE …]` 新增 `commit=` / `pump=` / `bombeos=` / `muerto=` / `wedge_ms=` / `fase=` / `t_guest=`；新增 `[T2DIAG PRODUCE …]`。
   - **硬约束**：宿主仍**一个字都不写** `284` / `288` / `16548` / `16552` / `16564` / `16568`；所有 guest 访存仍走 `RexGmShadowFor` 影子别名；驱动仍只在独立线程 `T2ROTF Dirigir` 上跑、**垂直同步派发线程绝不进入 guest 代码**；不碰门/泵热路。
4. 本轮最关键验收点（对照 v8.8 的 `drive=2 ok=1 produce=0 prodSinDev=0 dentro=1 estanc=1 284=0 288=1 SWAP n=1`）：
   - `[T2DIAG PRODUCE …]` 是否出现、`rec=` 是否非 0、`prodSinDev` 是否**停止单调增长**；
   - `[T2DIAG DRIVE …]` 的 `produce` **是否 > 0**、`284` 是否由 **0 变成非 0**；
   - `[T2DIAG SWAP]` 的 `n` 是否 **> 1**；`[T2DIAG espera]` 是否**停止单调增长**；
   - **画面是否出现**（三连 logo `luxo → atvi → hasbro`）、`juego … fps` 是否 **> 0**；
   - 是否出现 `[T2DIAG DRIVE-WEDGE …]`（若出现，`fase=` 直接指出卡点）。
5. 需回传的日志行：`[video] T2ROTF BUILD v8.9-PRODUCE …`；`[T2DIAG DRIVE …]`（**首末各一条**，重点 `drive` / `ok` / `produce` / `prodSinDev` / `estanc` / `dentro` / `commit` / `pump` / `bombeos` / `muerto` / `wedge_ms` / `fase` / `t_guest` / `284` / `288` / `obj` / `ev`）；`[T2DIAG PRODUCE …]`（前 4 条 + 末条）；`[T2DIAG DRIVE-WEDGE …]`（若出现）；`[T2DIAG GPSIG …]`（首末各一）、`[T2DIAG SWAP …]`（末条）、`[T2DIAG espera …]`（末条）；`rex_perfil.log` 的 `juego … fps` 与 `relojes:` 行（须未超频值）；`rex_crash.log`（若产生，附 `pc` / `far` / `esr` 前 40 行）。
6. 判读分支：A（`produce > 0` 且 `284` 出现非 0、`SWAP n > 1`、`espera` 停止增长、三连 logo 出现、`fps > 0`）= 缺环补上，进入「不超频 60 帧」实测；
   B（`produce > 0` 但 `284` 仍恒 0）= 连 guest 原生生产者都没能入队（下一轮冲 `sub_822B5608` 的入队前置条件与 `0x82DBB5E4` / `0x82DBB5D4` 两个逃逸标志的写入者）；
   C（`284` 变非 0 但 `SWAP n` 仍 1）= 消费者没被唤醒（下一轮冲 `base+284` 的**释放者**：`sub_822D9838` / `sub_822D98E8`）；
   D（`prodSinDev` 持续涨）= `sub_828823F8` 解析不出合法呈现记录，根因上移到「谁建立 `*(0x82DBB5C4)` 里 `+0 == 5` 的那条记录」；
   E（出现 `DRIVE-WEDGE`）= 驱动仍卡死，但 `fase=` 已把卡点缩到「解析器 / 生产者 / 提交 / 泵」四选一，下一轮直接对那一段做离线精读；
   F（仍有 `rex_crash.log`）= 回传新 `pc` / `far` / `esr` 前 40 行。
7. 回退：`sa2_renut_display_drive_produce = false`（退回「不投递，只可选提交」）→ `sa2_renut_display_drive_commit = false`（本版默认，已是最保守）→ `sa2_renut_display_drive_pump_max = 0`（本版默认）→ `sa2_renut_display_drive_watchdog_ms = 0`（关看门狗）→ `sa2_renut_display_present_guest = false`（退 v8.5 语义）→ `sa2_renut_gpu_signal_hostcb_s1 = false`（退 v8.4 语义，T61 已证其为空操作）→ `sa2_renut_display_drive_thread = false`（退 v8.3，实机会自锁）→ `sa2_renut_display_drive = false`（退 v8.2-SHADOW）→ `sa2_renut_gpu_signal_dev_hook = false`（退 v8.1）→ `sa2_renut_gpu_signal = false`（退 v7.7）。整版回退请用 `recomp/out/sw8/zip_backup/` 下的快照包：`T2ROTF_switch_port_20261101_v87_pre.zip`、`T2ROTF_switch_port_20261101_v88_pre.zip`。
8. 本轮**仍不包含**帧率达标：画面出来后才能在**不超频**条件下实测帧率并给「不超频 60 帧」结论（见第 5 节与待办 T26）。

### 3.21 v9.0-VIVO（T72/T73）验证清单 — 修复「黑屏、第一个动画有声无画面」：投递通道钉到活体显示设备 + 楔死复位交给 guest（**本节是当前有效机制**；第 3.20 节降为对照）

**本版修什么（一句话）**：v8.9 已把「第一帧投出去」修通（实机 `produce=1`、`base+284` 由 0→1、等门的呈现线程被唤醒），
但帧投出去后**永不被 retire**（`284=1`、`288=1`、`SWAP n=1` 全程不动）。根因是**显示投递通道被钉到了错的对象上**：

1. 门的上下文（`sub_82881B88` 的 `r30`）是**运行期堆对象**（实机 `0x41770018`，探针 `n` 到 4×10⁶ 不变），
   其管理器自指 `*(r30+16)` 读出来是 **0** ⇒ 门的就绪读 `*(*(r30+16)+272+4*slot)` 退化成 `*(0+272+4*slot)` **恒 0**
   （实机 `PUERTA … st0=0 s16=00000000 cnt=0/0/0`）⇒ 门永不就绪 ⇒ 交付 `sub_82881930` 一次都没走通 ⇒ 没有帧被 retire；
2. 生产者侧（`sub_822B5608`）被 v7.7-ALIGN 钉到**静态管理器**通道 `0x82E55E38`，与门按活体设备取址**分叉** ⇒ armed 永不被消费；
3. 于是排空 `while (*(0x82E55CE0+284) + *(0x82E55CE0+288) != 0)` 永远转不出去（实机 `DRAIN-IN n` 越过 680000），画面不出、音频独立照播。

**本版怎么修**

| # | 修复 | 位置 |
| --- | --- | --- |
| 1 | 生产者投递通道改钉**活体设备自己的通道** `dev+24`（实机 `0x41770030`） | `REX_HOOK_RAW(sub_822B5608)` 内 `chbase` |
| 2 | 补回该设备缺的**管理器自指** `*(dev+16) = dev`（链接字段，非会计；走 `RexGmShadowFor` 影子别名，取不到就跳过并计 `g_vivo_sin_shadow`） | 同上，日志 `T2DIAG VIVO-LINK` |
| 3 | 楔死态（`284==0 && 288>0` 且两侧六槽 arm 标记全 0，连续 120 次泵进入）调用 **guest 自己的**显示管线复位 `sub_82882628()`（封顶 8 次，宿主不写任何会计字段） | `T2ReinicioSiAtascado`（泵钩子 `sub_82881E00`），日志 `T2DIAG REINICIO` |
| 4 | 历史遗留的「宿主侧完成侧配平」（曾直写 `*(base+288)` / `*(entry+28)`）**整支只读化** | `REX_HOOK_RAW(sub_82882200)`，日志 `T2DIAG RECON-RO` |

**三处取址自洽（关键论证）**：自指修回后，
生产者 arm `++*(dev+24+248)` = `dev+272`、push `*(dev+24+8)` = `dev+32`；
门就绪读 `*(*(dev+16)+272+4*slot)` = `dev+272`；
交付弹 `*(*(dev+16)+24)+slot*80+8` = `dev+32`（若按 `r3+24` 取址同样为 `dev+32`）。
两侧必重合 ⇒ **不存在「弹到别的队列、回调对象成垃圾」的崩溃路径**（这正是「只补 `*(0x41770018+16)=0x82E55E20`」方案被弃用的原因）。

**新键（2 个，默认均 `true`，`kInitOnly`）**

```toml
sa2_renut_display_vivo      = true   # 通道钉到活体设备自己的通道 dev+24，并补回 *(dev+16)=dev
sa2_renut_display_reinicio  = true   # 楔死态交给 guest 自己的 sub_82882628() 复位（封顶 8 次）
```

**回退阶梯（改 `T2ROTF.toml` 后完全退出再启动）**

```text
sa2_renut_display_vivo      = false   # 退回 v8.9：生产者仍钉静态通道 ⇒ 预期回到黑屏（A/B 用）
sa2_renut_display_reinicio  = false   # 只留通道归一，不做楔死复位
sa2_renut_display_align     = false   # 连同 v7.7 的静态通道对齐一起关（v9.0 的通道改写用例位于该支内）
sa2_renut_display_reconcile = false   # 只关掉那条只读观测日志
```

**实机验证清单（**不超频**手持；把新 NRO + toml 覆盖到 `sdmc:/switch/Transformers2ROTF/`，完全退出再启动）**

1. 版本判据：日志里 `[video] T2ROTF BUILD` 必须只有 `v9.0-VIVO`（应用横幅仍是旧的，不作判据）；
2. `[apm]` 行确认未超频（GPU 460.8 MHz / 内存 1600.0 MHz）；
3. 逐条回传：

| 关键行 | 期望（修复生效） |
| --- | --- |
| `[T2DIAG VIVO-LINK n=… dev=41770018 … c16_antes=00000000` | 出现，`dev` 与 v8.9 的 `PUERTA r30` 一致 |
| `[T2DIAG PUERTA … s16=…` | `s16` 不再是 `00000000`（自指已补回），三槽 `cnt` 出现非 0 |
| `[T2DIAG ALIGN n=… a_antes=41770030 a_ahora=41770030` | 通道目标与生产者原生通道一致（不再被改写） |
| `[T2DIAG DRAIN-IN n=…` | **停止增长**（v8.9 越过 680000 还在涨） |
| `[T2DIAG SWAP n=` | `n` 推进（> 1 且持续增长） |
| `[T2DIAG REINICIO …` | **不应出现**；若出现说明门仍未就绪，请连同 `PUERTA` / `VIVO-LINK` 一起回传 |
| `rex_perfil.log` 的 `juego … fps` 与 `relojes:` | 非 0 fps 且未超频 |
| `rex_crash.log`（若产生） | 附 `pc` / `far` / `esr` 前 40 行 |

4. 若已出画面：进菜单与游戏内各跑一段并回传 `fps` 行，作为 60 FPS 验收依据（60 帧结论只在**不超频**条件下给）。

**构建与静态取证（本轮已完成）**

- `cd recomp/out/sw8 && ninja -j4 T2ROTF` 退出码 0，仅重编 `t2rotf_video_nativo.cpp` 并链接打包；
- `T2ROTF.nro` = 59,970,290 B（上一版 59,949,810 B），sha256 = `4a69142642488176872ca750633e2aec228d7d02bed48544ffa1aa1e202b91e5`；
- NRO 内 `T2ROTF BUILD` 串共 2 处、**全部是 v9.0-VIVO**，旧版本 BUILD 串残留 0；`v7.6-RECON` 已降级为探针串；
- 新增探针 `T2DIAG VIVO-LINK` / `T2DIAG REINICIO` / `T2DIAG RECON-RO` 均在；旧写会计串 `T2DIAG RECON nr=` 已从二进制消失；
- 符号 `__imp__sub_82882628`（0x1864200）与 `T2ReinicioSiAtascado`（0x2e321a0）已在 unstripped ELF 中；
- 源码级：对 `284 / 288 / 16548 / 16552 / 16564 / 16568` **只有读、没有任何写**。

**未决 / 风险**

- 若实机 `g_vivo_sin_shadow` 持续增长，说明该设备对象不可经影子别名访问（跳过自指/读槽，绝不直解引用）；
- 若补自指后三槽仍恒 0，则下一卡点须再看 `+272` 指向的记录阵列是否已建立；
- `kReinicioTics=120` 与 8 次上限未经实机标定；实机出现 `REINICIO` 应视为「通道归一未完全奏效」的信号；
- 60 帧仍未验收（本轮只解决「画面不出」）。

## 4. 工程结构

| 路径 | 说明 |
|---|---|
| `recomp/` | 主工程：CMake 工程、codegen 产物（`generated/`）、应用代码（`src/`）、构建输出（`out/`） |
| `recomp/src/T2ROTF_app.h` | 应用钩子：路径自动发现（game/）、APM 性能档请求 |
| `repos/nfsmw-nx/` | Switch 版 rexglue-sdk 基座（VFS 单层挂载、输入映射、性能剖析等） |
| `scripts/` | 构建与数据准备脚本 |
| `docs/` | 工程文档（XEX/TU/DLC 分析、性能验证方法） |
| `tools/` | 自研辅助工具（xex 分析、TU/DLC 解包、delta patch 等） |

## 5. 已知限制与风险

- 本机已完成构建级与静态级验证（NRO 可构建、目录对照、输入映射与 APM 调用字节级确认）；TU/DLC 实际游戏内容与 60 帧实机表现尚未在真机完整测试，首次实机运行时请保留 `logs/rex/rex_perfil.log` 以便对照 `docs/PERFORMANCE_T2ROTF.md` 判读。
- 若 460.8 MHz 无法达标，下一步候选（均不超频）：内部分辨率降到 1024x576（约省 36% 像素）、`resolution_scale` 相关 cvar、LTO/PGO 构建。

## 6. 更新记录

### 2026-11-02（v9.0-VIVO）— 修复「黑屏、第一个动画有声无画面」：投递通道钉到活体显示设备 + 楔死复位交给 guest（T72/T73）

- **判读输入**：v8.9-PRODUCE 四份实机日志。结论：首帧确实投出（`produce=1`、`base+284` 0→1）、呈现线程已脱离 `sub_82881848` 自旋，
  但 `284=1`、`288=1`、`SWAP n=1` 全程不动 ⇒ **帧已投递却在飞永不退役**（判读分支 C 的下半段）。
- **离线定论（T72）**：门 `sub_82881B88` 的上下文 `r30` 是运行期显示设备（实机 `0x41770018`），其 `+16` 管理器自指为 **0**
  ⇒ 门的就绪读退化成 `*(0+272+4*slot)` 恒 0（实机三槽 `cnt=0/0/0`）⇒ 门永不就绪 ⇒ 交付 0 次 ⇒ 排空永久自旋；
  生产者侧又被 v7.7-ALIGN 钉在静态管理器通道 `0x82E55E38`，两侧取址分叉。
- **落地修复（T73）**：① 生产者投递通道改钉 `dev+24`（活体设备自己的通道）；② 补回 `*(dev+16)=dev`（`RexGmShadowFor` 影子别名）；
  ③ 楔死态调用 guest 自己的 `sub_82882628()` 复位显示管线（`kReinicioTics=120`、封顶 8 次，宿主不写会计字段）；
  ④ T37「宿主侧完成侧配平」整支只读化（源码不再写 `284/288`）。
- **新键**：`sa2_renut_display_vivo`（true）、`sa2_renut_display_reinicio`（true）⇒ TOML 键数 27 → 29。
- **取证**：`ninja -j4 T2ROTF` 退出码 0；`T2ROTF.nro` 59,970,290 B，
  sha256 `4a69142642488176872ca750633e2aec228d7d02bed48544ffa1aa1e202b91e5`；
  NRO 内 `T2ROTF BUILD` 仅 v9.0-VIVO 两处、旧版本残留 0；新增探针均已在；对六个会计字段只有读。
- **结论文件**：`T2ROTF_deko3d_投递通道钉到活体设备与楔死复位报告.md`；实机验证清单见第 3.21 节。
- **状态**：待实机（不超频）回传日志确认画面出现；60 FPS 仍未验收。

### 2026-11-02（v8.9-PRODUCE）— 修复「黑屏、第一个 LOGO 有声」：补产排在提交之后，一进提交入口就永久卡在 guest 显示回调（T68/T69/T70）

- **产物**：`recomp/out/sw8/T2ROTF.nro` = **59,962,098 字节**，sha256 `ceb43ebf408de7d3b84b85511569ffcc9d52b1df01db06416839ac5459912470`（上一版 v8.8/T66 = 59,949,810 字节 / `d1a59c332b0460dcf09bf0d2bdf44c3577dd04ffab9dcef2828a4f821e9d8b5e`，**字节数与哈希都变了**）；`T2ROTF.toml` = **56,632 字节 / 27 键**（**新增 3 键**：G17/G18/G19，见下）；新增报告 `T2ROTF_deko3d_提交前置生产与看门狗报告.md`；包内更新 `recomp_src/t2rotf_gpu_signal.cpp`（79,963 → 93,103 字节，sha256 `452ff0d328a98d86414764d927333976fad047e866a2fa09e77117d889e913f3`）；`recomp_src/t2rotf_video_nativo.cpp`（80,874 字节）、`recomp_src/t2rotf_gpu_signal.h`（1,593 字节）、`recomp_src/renut_engine/gpu/d3d/hooks/device.cpp`（103,645 字节）**逐字节未改动**。
  > 本工程产物内嵌构建时间戳：**同一份源码重构建后体积相同、哈希会变**，文档与包内哈希一律以随包 NRO 实算值为准。
- **实机判读（用户回传 `rex_perfil(35)` / `rex_stderr(74)` / `T2ROTF_001(72)` / `T2ROTF_002(53)`，用户描述「黑屏，但是第一个 LOGO 有声音」）**：① **不是崩溃、不是超频、不是缺页**——无 `rex_crash`；`[apm] PUERTA la configuracion 0x92220008 (tabla: GPU 460, EMC 1331): GPU 460.8 MHz (era 307.2)` 证明未超频；`OnInitialize` 五段全过（`v6.27-MAPFIX crash hooks init ok` 是**常量串不是版本**）；② **音频正常**（`1/1 buffers audout con PCM no nulo … pico maximo 0.474`）而 `juego 0.0 fps` ⇒ **有声无画**；③ `[T2DIAG GPSIG]` 的 `vblanks` / `irq` / `mmio_r` 持续增长、`inv == ret` ⇒ **中断派发链健康**。
- **定论**：`[T2DIAG DRIVE]` 首行 `16548=94 16568=94 284=0 288=1 obj=40F604E0 ev=F8000068`，尾行 `drive=2 ok=1 ini=2 dentro=1 noObj=0 fallos=0 vivo=45 produce=0 prodSinDev=0 estanc=1 16548=1174 16568=1174 284=0 288=1 obj=41370080 ev=F8000068 mmio_w=0` ⇒ **驱动线程只成功进过 guest 一次，第二次再没回来；补产一次都没跑到**。`[T2DIAG DRAIN-IN]` 最后一条（19:09:18.865）`n=13 idx=0 c24=1 c28=0 e24=1 e28=0` **之后再无出栈**；离线精读 `sub_82881E00` 证实它 `bctrl` 调 item+60 槽**呈现回调**、**回调返回之后**才 `atomic_dec(manager+288)` ⇒ **卡在该呈现回调里不返回**（`base+288` 自 17.597 起恒 1，该减法从未执行；`PUERTA n=17`（16.730）曾出现 `284=0 288=0`，说明该回调**曾经能返回**）。于是显示管理器三处排空判据 `while (*(base+284) + *(base+288) != 0)`（`sub_82882200` / `sub_828828E0` / `sub_82882740`）**永远转不出去**，呈现线程 `sub_82881848` 因 `284 == 0` 在帧事件 `*(0x82DBB5E0)=F8000068` 上空转（`espera` 涨到 978 万）、`SWAP` 全程 `n=1` ⇒ **黑屏**；音频链独立 ⇒ **首 LOGO 有声**。宿主侧机制：`DirigirFotograma` **line 1100 无条件**调提交入口 `sub_82880978`（**无 cvar 开关**），而补产在 **line 1122**、**排在提交之后** ⇒ 提交一次即永久 `dentro=1`，正是 `produce=0 prodSinDev=0` 的机制。
- **改动**（**只改 `recomp/src/t2rotf_gpu_signal.cpp`；新增 3 键，24 → 27 键**）：① **顺序反转**为「**先投递**（`ProducirFotograma`，只在 `base+284 == 0` 时投递）→ **再可选提交**（`sub_82880978`）→ **最后可选有界交接**」；② **G17 `sa2_renut_display_drive_commit` 默认 `false`**：把提交入口包起来、默认不再驱动（实机已证一进该入口就永久卡在 guest 显示回调里）；③ **G18 `sa2_renut_display_drive_pump_max` 默认 `0`（关）**：有界**非阻塞**交接，最多 N 次调 `sub_82881E00(r3 = 0x82E55E20, r4 = -1, r5 = 1)`（`r5=1` 保证门不就绪即刻返回；新增带 `r4`/`r5` 的 `DirigirEntradaArgs` 变体）；④ **G19 `sa2_renut_display_drive_watchdog_ms` 默认 `500`**：驱动进 guest 超阈值就落**一次** `[T2DIAG DRIVE-WEDGE]`（`fase=` 相位 + 全量快照）并**永久停驱动**（`muerto=1`）；⑤ 补产改为走 guest 自己的解析器 `sub_828823F8` 取记录、过 `PlausibleObjeto()` 后调 `sub_82882730`，取不到则跳过并计 `prodSinDev`，**绝不以 `r3 = 0` 调用生产者本体**（`sub_822B5608` 首条 `lwz r10,64(r3)` / `stw r11,56(r3)` 会读写 guest `0x40`/`0x38`）；⑥ DRIVE 行新增 `commit=` / `pump=` / `bombeos=` / `muerto=` / `wedge_ms=` / `fase=` / `t_guest=`，新增 `[T2DIAG PRODUCE …]`；⑦ BUILD 标记改为 `v8.9-PRODUCE`。**宿主仍不写任何显示会计字段、所有 guest 访存仍走 `RexGmShadowFor`、驱动仍只在独立线程 `T2ROTF Dirigir` 上跑、垂直同步派发线程绝不进 guest 代码、不碰门/泵热路**。
- **校验**：`ninja -j4 T2ROTF` 退出码 0（`[1/51] Generating recompiled code` 报 `0 written, 0 unchanged, 0 deleted, 1 module(s) up to date`，属预期——本轮只改宿主侧 C++；随后重链与 `Packaging T2ROTF.nro` 正常）；NRO 字节级取证：`v8.9-PRODUCE`=5、`sa2_renut_display_drive_commit`=5、`sa2_renut_display_drive_pump_max`=5、`sa2_renut_display_drive_watchdog_ms`=5、`DRIVE-WEDGE`=3、`T2DIAG PRODUCE`=2、`commit sub_82880978`=1、`pump sub_82881E00`=1、`resolver sub_828823F8`=1、`producer sub_82882730`=3、`prodSinDev`=3、`ProducirFotograma`=2、`write guest 0x40/0x38`=1（NRO 里仍有 `v8.8-LIVENESS` / `v8.8b` 字样，来自新 BUILD 文本里的历史说明与旧 cvar 帮助文本，**不要用它判版本**）；`T2ROTF.toml` 经 `tomllib` 解析通过、**27 键**读回正确（`drive_commit=false`、`drive_pump_max=0`、`drive_watchdog_ms=500`、`drive_produce=true`、`present_guest=true`、`hostcb_s1=true`、`hostcb_s1_mode=2`）。
- **工程包**：`T2ROTF_switch_port.zip` 已**原地更新**（替换 `T2ROTF.nro` / `T2ROTF.toml` / `README.md` / `recomp_src/t2rotf_gpu_signal.cpp`，并追加本轮报告；逐项「包内 == 磁盘」逐字节断言通过；上一版快照 `recomp/out/sw8/zip_backup/T2ROTF_switch_port_20261101_v87_pre.zip`、`T2ROTF_switch_port_20261101_v88_pre.zip`）。包体字节数与 sha256 由原地更新脚本 `recomp/out/sw8/_upd_pkg_t70.py` 运行实算并打印。
- **残余风险**：**本版尚未实机验证**；卡死的**呈现回调（item+60 虚表）到底在等什么**仍未定（等宿主 Renut 的 present 完成？还是等 `F8000068` 若回调内部自等即成自锁？——v8.9 的 `fase=` 相位快照会给出第一手证据）；`base+284` 的**释放者仍未定位**（候选 `sub_822D9838` / `sub_822D98E8`）；`sub_828823F8` 在卡死态能否返回合法记录未知（若恒返回 0，则本轮实际生效的只有「顺序反转 + 提交默认关」）；双宿主线程先后进同一**无锁**显示管理器是否为整链停摆的诱因未定；「不超频流畅 60 帧」**仍未达成也未实测**（T26 待做）；TU/DLC 实机闭环仍在画面出现之后。
- **详见**：`T2ROTF_deko3d_提交前置生产与看门狗报告.md`。

### 2026-11-01（v8.8-LIVENESS）— 修复「依旧黑屏有声」：驱动线程被 v8.6 自己伪造的记账饿死（T65/T66）

- **产物**：`recomp/out/sw8/T2ROTF.nro` = **59,949,810 字节**，sha256 `d1a59c332b0460dcf09bf0d2bdf44c3577dd04ffab9dcef2828a4f821e9d8b5e`（上一版 v8.7/T63 = 59,949,810 字节 / `db089080b6360c68cd49df8e1dfbfb6f63eae1da550002b28b9761ec7e2ef2d4`，**字节数相同但哈希不同**）；`T2ROTF.toml` = **48,067 字节 / 24 键**（**不新增按键**，G14 默认值 `false → true`，版本说明重写）；新增报告 `T2ROTF_deko3d_驱动判活与提交补产报告.md`；包内更新 `recomp_src/t2rotf_gpu_signal.cpp`（75,030 → 79,963 字节）；`recomp_src/t2rotf_video_nativo.cpp`（80,874 字节）、`recomp_src/t2rotf_gpu_signal.h`（1,593 字节）、`recomp_src/renut_engine/gpu/d3d/hooks/device.cpp`（103,645 字节）**逐字节未改动**。
  > 本工程产物内嵌构建时间戳：**同一份源码重构建后体积相同、哈希会变**，文档与包内哈希一律以随包 NRO 实算值为准。
- **实机判读（用户回传 `rex_crash(38)` / `rex_stderr(73)` / `T2ROTF_001(71)` / `T2ROTF_002(52)`，用户描述「依旧黑屏有声」）**：① `rex_crash(38).log` 与 `rex_crash(37).log` **逐字节相同** ⇒ v8.7-IRQABI 已修好启动崩溃、本轮**无新崩溃**；② 两份运行日志的 `[video] T2ROTF BUILD v8.7-IRQABI` 在跑，`irq sub_822D47A0 … dev=40070600` 设备解析恒正确、地址守卫 0 命中，`OnInitialize: LaunchModule done` 之后无 crash hook；③ `[T2DIAG GPSIG]` 由 `vblanks=0 irq=0 s1=0` 涨到 `vblanks=8882 irq=17710 s1=8855 inv=17710 ret=17710`（**中断派发链健康**）；④ 但 `[T2DIAG DRIVE]` 全程 `drive=0 ok=0 produce=0`（`vivo` 只在涨：001 末 1531、002 末 8733），且 **`16568 ≡ 16548`**（末态 `PERRO vblank16548=8894 fr16564=8894 disp16568=8894`）。
- **定论**：**驱动线程被 v8.6-FLIP 自己伪造的记账「饿死」** —— v8.6 的代客呈现记账 `sub_822D9688` 每刷新自增 `dev+16568`，而 v8.3/v8.4 的让位判据把 `dev+16568` 也算作「guest 管线在动」，于是驱动线程**每一轮都让位** ⇒ guest 侧从不产出帧（`base+284` 恒 0、`mmio_w=0`、`ringR16700=ringW16704=0`、`inKick13232=0`、管线 6 帧后冻结）⇒ 呈现线程死在 `sub_82881848` 对帧事件 `*(0x82DBB5E0)=F8000068` 的等待上（`espera sub_824F3580 n=12,680,000 / 68,680,000`）⇒ `SWAP` 全程 `n=1` ⇒ 注入的影片帧无处上屏、XAudio 侧照常有声。
- **改动**（**只改 `recomp/src/t2rotf_gpu_signal.cpp`；不新增 cvar、只改 G14 默认值**）：① 让位判据改为**只认 guest 自己自增的 `base+284`**（`284==0` 才驱动、`284!=0` 让位、卡住 45 次刷新≈0.75 s 仍驱动），**`dev+16568` 彻底移出判据**；② G14 `sa2_renut_display_drive_produce` 默认 **true**：提交入口驱动后若 `284` 仍为 0，补调 guest 原生生产者 `sub_82882730`（`284 != 0` 时绝不补调，杜绝重复投递）；③ **v8.8b 两处交付前自查修正**：补产入参改为先经 guest 自己的解析器 `sub_828823F8` 取「当前呈现记录」（取不到则跳过并计 `prodSinDev`，**绝不以 `r3 = 0` 调用生产者**——`sub_822B5608` 首条 `lwz r10,64(r3)` / `stw r11,56(r3)` 会读写 guest `0x40`/`0x38`），当前显示对象 `*(0x82DBB608)` 改为**只计数不再当闸门**（v8.7 实机该字段恒 0，当闸门会把驱动永久拦下）；④ DRIVE 行新增 `prodSinDev=` / `estanc=` / `obj=` / `ev=`，逐次行新增 `saltos_vivo=` / `estancados=`；⑤ BUILD 标记改为 `v8.8-LIVENESS`（文本内附 v8.8b 修正说明）。**宿主仍不写任何显示会计字段、所有 guest 访存仍走 `RexGmShadowFor`、驱动仍只在独立线程 `T2ROTF Dirigir` 上跑、不碰门/泵热路**；v8.7-IRQABI / v8.6-FLIP / v8.4-SPLIT / v8.3-FRAME / v8.2 影子安全访存与让位语义全部保留。
- **校验**：`ninja -j4 T2ROTF` 退出码 0（`[2/3] t2rotf_gpu_signal.cpp.obj`、`[3/3] Linking … Packaging T2ROTF.nro`；`[1/52] Generating recompiled code` 报 `0 written, 0 unchanged, 0 deleted, 1 module(s) up to date`，属预期——本轮只改宿主侧 C++）；NRO 字节级取证：`v8.8-LIVENESS`=2、`v8.8b refinements`=1、`prodSinDev`=1、`estancado`=1、`sub_828823F8`=1（NRO 里仍有 `v8.7-IRQABI`/`v8.6-FLIP` 字样，来自新 BUILD 文本里的历史说明与旧 cvar 帮助文本，属预期，**不要用它判版本**）；`T2ROTF.toml` 经 `tomllib` 解析通过、**24 键**读回正确（`display_drive_produce=true`、`present_guest=true`、`hostcb_s1=true`、`hostcb_s1_mode=2`、`display_drive=true`、`display_drive_thread=true`、`dev_hook=true`）。
- **工程包**：`T2ROTF_switch_port.zip` 已**原地更新**（替换 `T2ROTF.nro` / `T2ROTF.toml` / `README.md` / `recomp_src/t2rotf_gpu_signal.cpp`，并追加本轮报告；上一版快照 `recomp/out/sw8/zip_backup/T2ROTF_switch_port_20261101_v87_pre.zip`、本版首包快照 `T2ROTF_switch_port_20261101_v88_pre.zip`）。包体字节数与 sha256 由原地更新脚本 `recomp/out/sw8/_upd_pkg_t65.py` 运行实算并打印。
- **残余风险**：**本版尚未实机验证**；补产路径的未知点是 `sub_828823F8` 在卡死态能否解析出合法记录（若恒返回 0 则全部走 `prodSinDev` 跳过，此时仍生效的是让位判据修正）；`base+284` 的**释放者仍未定位**（候选 `sub_822D9838`/`sub_822D98E8`）；`*(0x82DBB5A0)`（`evA0`）在卡死态为 0，而呈现线程等的是 `*(0x82DBB5E0)`；「不超频流畅 60 帧」**仍未达成也未实测**（T26 待做）；TU/DLC 实机闭环仍在画面出现之后。
- **详见**：`T2ROTF_deko3d_驱动判活与提交补产报告.md`。

### 2026-11-01（v8.7-IRQABI）— 修复「启动后直接报错退出、没有任何加载」：中断回调 ABI 误读（T63/T64）

- **产物**：`recomp/out/sw8/T2ROTF.nro` = **59,949,810 字节**，sha256 `db089080b6360c68cd49df8e1dfbfb6f63eae1da550002b28b9761ec7e2ef2d4`（上一版 v8.6/T62 = 59,949,810 字节 / `93e44c13ede5fe8ded03f6c9528690806ca73103e5021ca2d763bf8c90408b47`，**字节数相同但哈希不同**）；`T2ROTF.toml` **本轮未改动**（仍 **24 键**、42,369 字节）；新增报告 `T2ROTF_deko3d_启动崩溃IRQABI报告.md`；包内更新 `recomp_src/t2rotf_video_nativo.cpp`（76,899 → 80,874 字节）与 `recomp_src/t2rotf_gpu_signal.cpp`（74,911 → 75,030 字节）。
  > 本工程产物内嵌构建时间戳：**同一份源码重构建后体积相同、哈希会变**，文档与包内哈希一律以随包 NRO 实算值为准。
- **实机判读（用户回传 `rex_crash(37)` / `rex_stderr(72)` / `T2ROTF_001(70)` / `T2ROTF_002(51)`，用户描述「本版启动后直接报错退出，没有任何加载」）**：`OnInitialize` 全链已走完（`RexGmInit(0xE0000000)` → `base=0x72602b6000 size=0x120000000` → SetupPresentation → ConstructRuntime → `LaunchModule done`），随后在 `[video] T2ROTF BUILD v8.6-FLIP` 运行期崩溃退出：`Unhandled guest access violation: read of guest 0x2A91`，`pc = imagen+0x2e28544`（=`t2rotf::video_nativo::(anonymous)::VolcarGuest+0x2c4`）、`lr = imagen+0x2e29240`（`VolcarGuest+0xfc0`）、`esr 0x92000005`、指令 `0xb8604b54`（`ldr w20,[x26,w0,uxtw]`）、`x19=1`、`x8=0x2A90`、`x9=0x2A94`、`x0=0x2A91`、`x1=0x2A95`、`x26=0x5762c4e000`、`far`（run1 `0x5762C50A91` / run2 `0x72602B8A91`）= arena base + `0x2A91`；`nm -n` + `objdump` 反推 `x19 = dev = 1`，即 `g_dev_visto` 被写成 `1`。
- **定论（T63）**：**中断回调 `sub_822D47A0` 的 guest ABI 是 `(r3 = 来源 0=垂直同步/1=提交完成, r4 = 设备)`**——现状日志里 `irq sub_822D47A0 n=2 r3=00000001 r4=40070600` 与 `n=3 r3=00000000 r4=40070600` 按 ~16 ms 交替、`r4` 恒为设备 `0x40070600`，而 v8.4 老日志只有 `r3=00000000`（互证）。旧钩子 `if (ctx.r3.u32) g_dev_visto.store(ctx.r3.u32, …)` 把**来源值**当设备写入；v8.5/v8.6 新增「每垂直同步派发 `source=1` 提交完成」后，`g_dev_visto` 被 60 Hz 持续毒化成 `1`（`sub_822D4488` 钩子偶尔写回 `0x40070600` 也压不过它）。泵钩子里的 `VolcarGuest("pump")` 与看门狗 `VolcarGuest("perro")` 的 `PERRO dev=…` 行在**参数求值**时裸读 `dev + 10896` ⇒ `1 + 10896 = 0x2A91` 未映射 ⇒ xmemory 致命路径 ⇒ 进程**启动即退出、零加载**（`REXLOG_ERROR` 的参数求值先于限频判断，与「限频只打前 n 行」无关）。崩溃现场 `x8/x9/x0/x1/x19` 与 `0x2A90/0x2A94/0x2A91/0x2A95` 逐一自洽。
- **改动**（**只改 `recomp/src/t2rotf_video_nativo.cpp`、`recomp/src/t2rotf_gpu_signal.cpp`；不新增 cvar、不改任何默认值**）：① `sub_822D47A0` 钩子：设备候选改为 **`r4`**，经 `EsDireccionDispositivo()`（非零 + 4 字节对齐 + `0x10000000..0xFFFFFFF0`）校验通过才 `g_dev_visto.store`，不健全即 `g_irq_dev_avisado++` 并限频告警；观测行改为 `n=… fuente=… dev=…`；② `sub_822D9570` 钩子：设备候选优先 `r3`、不健全退 `r4`，校验通过才落盘且只用校验后的值调 `NotificarDispositivo`；③ `sub_822D4488` 钩子：落盘前同一校验；④ `sub_822D13F0` 钩子：`dev`/`p` 解引用前加校验；⑤ `VolcarGuest`：`ctx`/`dev` 读前一律校验，不健全即 `g_perro_saltos++` 并限频打印「不是设备地址（守卫跳过，不再裸读 guest）」，`f10896`/`f10900` 读出后先校验再当指针用；⑥ `t2rotf_gpu_signal.cpp` 的 BUILD 标记改为 `v8.7-IRQABI`，并把根因与两条硬约束写进构建字符串（v8.6 呈现记账、v8.4-SPLIT、v8.2 影子安全访存与 yield 语义**全部保留**）。**不写任何显示会计字段、不碰门/泵热路**。
- **校验**：`ninja -j4 T2ROTF` 退出码 0（`[2/4] t2rotf_gpu_signal.cpp.obj`、`[3/4] t2rotf_video_nativo.cpp.obj`、`[4/4] Linking … Packaging T2ROTF.nro`）；再跑仍 no-op ⇒ 构建已完整收尾且可复现；NRO 字符串取证：`v8.7-IRQABI`=1、`不是设备地址`=1、`设备候选`=1（NRO 里仍有 `v8.6-FLIP`/`v8.5-COMPLETE` 字样，来自新 BUILD 文本里的历史说明与旧 cvar 帮助文本，属预期，**不要用它判版本**）。
- **工程包**：`T2ROTF_switch_port.zip` 已**原地更新**（替换 `T2ROTF.nro`/`T2ROTF.toml`/`README.md`/`recomp_src/t2rotf_video_nativo.cpp`/`recomp_src/t2rotf_gpu_signal.cpp`/`recomp_src/t2rotf_gpu_signal.h`/`recomp_src/renut_engine/gpu/d3d/hooks/device.cpp`，并追加本轮报告；本轮首包快照 `recomp/out/sw8/zip_backup/T2ROTF_switch_port_20261101_v87_pre.zip`）。包体字节数与 sha256 由原地更新脚本 `recomp/out/sw8/_upd_pkg_t63.py` 运行实算并打印。
- **残余风险**：**本版尚未实机验证**；v8.7 只修「启动即崩」这一个 bug，**未触碰** v8.6 的画面链问题（`16568` 是否脱离 5、`dentro` 是否回 0、`fin=`/fps 是否 >0），启动崩溃修好后预期回到该问题；「不超频流畅 60 帧」**仍未达成也未实测**（T26 待做）。
- **详见**：`T2ROTF_deko3d_启动崩溃IRQABI报告.md`。

### 2026-10-31（v8.6-FLIP）— 呈现记账「交回 guest 自己」：补上零静态调用者的 `sub_822D9688`（T61/T62）

- **产物**：`recomp/out/sw8/T2ROTF.nro` = **59,949,810 字节**，sha256 `93e44c13ede5fe8ded03f6c9528690806ca73103e5021ca2d763bf8c90408b47`（上一版 v8.5/T58 = 59,941,618 字节 / `0fa8e1fed0236cef3cf5c183d9e1b9a628284d2fc35c9c0cab4da688248c46d3`，**字节数与哈希都不同**）；`T2ROTF.toml` = **42,369 字节**（**新增 1 键** `sa2_renut_display_present_guest=true`，共 **24 键**）；新增报告 `T2ROTF_deko3d_呈现记账交回guest报告.md`；包内更新 `recomp_src/t2rotf_gpu_signal.cpp`（68,966 → 74,911 字节）与 `recomp_src/renut_engine/gpu/d3d/hooks/device.cpp`（103,645 字节）。
  > 本工程产物内嵌构建时间戳：**同一份源码重构建后体积相同、哈希会变**，文档与包内哈希一律以随包 NRO 实算值为准。
- **离线定论（T61，逐指令精读生成代码，未依赖实机）**：① **v8.5 押注的「派发 source=1 提交完成中断」是空操作**——`sub_822D47A0`（`T2ROTF_recomp.9.cpp:21582`）的 `r3==1` 分支只做「`*(blk+16)` 非 0 才 `bctrl *(blk+16)`」+ 清 `*(blk+0)` 里当前 CPU 的在飞位，**不读写 `base+284`/`base+288`、不碰依赖表 `+24/+28`**，而 `*(blk+16)` 在本移植里从未被写非 0（实机 `blk0=0 blk4=0`）；② **显示完成/vblank 的真入口是同一回调的 `r3==0` 分支**，由 guest 写在 MMIO `0x7FC86544` 的 bit0 门控 → `sub_822D9570(dev)`；它**一直在跑**（实机 `v16548` 涨到 2738），但排空的换页环**恒空**（`ringR16700 = ringW16704 = 0`）⇒ 永远进不了环体 ⇒ `v16568` 不涨、`*(*(dev+10900)+4)` 永不清零；③ **真正缺的是记账者**：`sub_822D9688`（`T2ROTF_recomp.32.cpp:20832`）**零静态调用者**（全工程只有 `T2ROTF_init.cpp:3890` 注册，没有任何 `bl 0x822d9688`）；它的原生调用者（guest 自己的 Swap/提交路径）被宿主 `D3DDevice_Swap_hook` 整体替换，而宿主那套 `T2AdvanceDisplayAccounting()`（`device.cpp:926`，调用点 `:1345`）是**对同一批字段的宿主重实现**（属本工程明令禁止的「写显示会计字段」），**只在 Swap 时被调**——Swap 第 1 帧就停（`[T2DIAG SWAP n=1]`）⇒ `16564`/`16568` 冻在 **5**。这三条把 v8.4/v8.5 实机「`s1` 涨了但 `dentro` 仍 1、提交侧零产出、呈现侧永等 `F8000068`」串成单一根因：**不是中断没送到，而是没人记账**。
- **改动**：`recomp/src/t2rotf_gpu_signal.cpp`（68,966 → 74,911 字节）——① 新增成员函数 **`PresentarGuest(cpu)`**：cvar 守卫 → 线程状态守卫 → `Dispositivo()` + **设备双槽核对**（`Leer32(kSlotOtro)` 与 `Leer32(celda)`，任一 == `dev` 才继续，否则 `saltados++` 并只报一行）→ `ctx->r3 = r4 = r5 = 0` → `ExecuteTrap(ts, kPresentarGuest, args, 0)` → 限频打 `[video] T2DIAG PRESENT n=… dev=… 16564=… 16568=… 16548=… ringW16704=… blk10900=… blk4=… saltados=…`；② 新增常量 `kOffDevCompletados64 = 16564`、`kPresentarGuest = 0x822D9688`；③ `BucleVblank` 里在 `Interrupcion(0, cpu); Completado(cpu);` 之后新增 `PresentarGuest(cpu);`（**放在垂直同步派发线程内**，与既有 `source=0` 中断派发同线程同时机——v8.4 实机已证该线程能持续运行，而独立驱动线程会长期卡在 guest 提交内部）；④ 新增 cvar **`sa2_renut_display_present_guest`（默认 true，`kInitOnly`）** 并附定论注释；⑤ 版本标记改为 `[video] T2ROTF BUILD v8.6-FLIP (the display accounting is handed back to the guest itself: every VSync the host calls the guest's own completion-accounting function sub_822D9688 with r3=0, …)`。`recomp/src/renut_engine/gpu/d3d/hooks/device.cpp`（103,645 字节）——`T2AdvanceDisplayAccounting(device_va, completed_swap);`（`:1345`）改为 `if (REXCVAR_GET(sa2_renut_display_agent)) …` ⇒ **默认 false，宿主不再直写显示会计字段**（字段负责人交回 guest）。**全部字段写入都在 guest 代码自己的自旋锁内完成；宿主只用 shadow 安全读取核对设备槽，不写任何 guest 字段、不碰门/泵热路、不改 v8.2 影子访存、保留 v8.4 驱动线程拆分与让位语义、`presenter()/provider()` 仍为 null（画面仍由 Renut/deko3d 出）**。`recomp/src/t2rotf_gpu_signal.h` 与 `recomp/src/t2rotf_video_nativo.cpp` **未改动**。
- **校验**：`ninja -j4 T2ROTF` 退出码 0；再跑仍 no-op（`Codegen summary: 0 written, 0 unchanged, 0 deleted, 1 module(s) up to date`，NRO 的 mtime/sha256 不变）⇒ 构建已完整收尾且可复现；NRO 字符串取证（`grep -a -o -F | wc -l`）`v8.6-FLIP`=2、`sa2_renut_display_present_guest`=4、`T2DIAG PRESENT`=1、`822D9688`=2、`代客呈现记账暂不调用`=1（NRO 里仍有 `v8.5-COMPLETE`=2，来自旧 cvar 帮助文本与历史判读行，属预期，**不要用它判版本**）；`T2ROTF.toml` 经 `tomllib` 解析通过、**24 键**读回正确（`present_guest=true`、`hostcb_s1=true`、`hostcb_s1_mode=2`、`agent=false`、`release=false`）。
- **工程包**：`T2ROTF_switch_port.zip` 已**原地更新**（替换 `T2ROTF.nro`/`T2ROTF.toml`/`README.md`/`recomp_src/t2rotf_gpu_signal.cpp`/`recomp_src/renut_engine/gpu/d3d/hooks/device.cpp`，并追加本轮报告；`recomp_src/t2rotf_gpu_signal.h`、`recomp_src/t2rotf_video_nativo.cpp` 逐字节核对为**未改动**；本轮首包快照 `recomp/out/sw8/zip_backup/T2ROTF_switch_port_20261031_v86_pre.zip`）。包体字节数与 sha256 由原地更新脚本 `recomp/out/sw8/_upd_pkg_t62.py` 运行实算并打印。
- **残余风险**：**本版尚未实机验证**；v8.6 把记账交回 guest 是**必要条件**，但不保证在飞显示项（`base+284`/`base+288`、依赖表 `+24/+28`）的释放者一定就是它——若下轮出现「`16568` 涨、`blk4` 归 0，但 `base+284` 仍恒 1」，则释放者另有其人（下一轮冲 guest 的 GPU 提交完成路径 `sub_822D9838`/`sub_822D98E8` 与 `VdSetSystemCommandBufferGpuIdentifierAddress` 注册地址）；`sa2_renut_gpu_signal_hostcb_s1` 仍默认 true（本轮已证其为空操作，置 false 仅作 A/B）；「不超频流畅 60 帧」**仍未达成也未实测**（T26 待做）。
- **详见**：`T2ROTF_deko3d_呈现记账交回guest报告.md`。

### 2026-10-31（v8.5-COMPLETE）— 提交完成中断「解除抑制」：把 Graphical ISR 的 source=1 分支真正派发进 guest（T58/T59）

- **产物**：`recomp/out/sw8/T2ROTF.nro` = **59,941,618 字节**，sha256 `0fa8e1fed0236cef3cf5c183d9e1b9a628284d2fc35c9c0cab4da688248c46d3`（上一版 v8.4/T56 = 59,937,522 字节 / `9d78b957…`，**字节数与哈希都不同**）；`T2ROTF.toml` = **40,124 字节**（**新增 1 键** `sa2_renut_gpu_signal_hostcb_s1_mode=2`，并把 `sa2_renut_gpu_signal_hostcb_s1` 由 false **翻为 true**，共 **23 键**）；新增报告 `T2ROTF_deko3d_提交完成中断解除抑制报告.md`；包内更新 `recomp_src/t2rotf_gpu_signal.cpp`（64,966 → 68,966 字节）。
- **实机判读（用户回传 `rex_perfil(34)` / `rex_stderr(71)` / `T2ROTF_001(69)` / `T2ROTF_002(50)`，用户描述「还是没有画面」）**：**v8.4 的自锁解除已生效**——`[T2DIAG GPSIG]` 报告恢复每 2 秒一条（约 72 条），`vblanks 0→2880`、`irq 0→2853`、`r1951 = mmio_r` 同步增长、`hostcb=2613/27`、`inv=ret=irq`。但仍有四条并发现象：① 提交侧零产出（`mmio_w=0`、`wptr=0(0)`、`inKick13232=0`、`ringR16700=ringW16704=0`）；② `dev+16568` 冻在 **5**、`cola284=0 cola288=1`、`a16700=a16704=0`、`obj=00000000`；③ 驱动线程 `t682943169022225291` 只输出 4 行后静默、周期报告恒 `drive=… ok=1 ini=2 dentro=1 produce=0`；④ 呈现侧 `espera sub_824F3580` 涨到 **21,440,000+**（恒 `r3=F8000068 lr=828818DC`）。
- **定论（T58）**：单一闭环——宿主每帧驱动 guest 每帧提交 → guest 走到排空 `while (*(base+284)+*(base+288)!=0)` 等一个显示完成 → 能 retire 在飞项的唯一通路是 Graphical 中断回调的 **source=1（提交完成）** 分支（`sub_822D47A0` 读 `*(dev+10900)`=swap 暂存块 → 调 `*(blk+16)`）→ 但这一支**一次都没派发过**（`skipS1 == vblanks`、`s1 == 0`、`dev+16568` 冻在 5）→ `base+288` 恒 1 → 排空永不退出 → 驱动线程卡死（`dentro=1`）→ 提交侧零产出 → 呈现侧永等。source=1 从来没派的**两个原因**：① `sa2_renut_gpu_signal_hostcb_s1` 旧默认 **false**；② 旧判据要求暂存块落在「guest 对象区间」，而实机 `*(dev+10900)=0xBDF80080`（D3D 后端自己的 fence/暂存块，落在 `0xB…` GPU 物理段）被一律拒发。**另纠正**：present 等的是 `F8000068`（`*(0x82DBB5E0)`）而非 `F8000060`，且 `F8000068` 由 guest 自己的 ISR 尾部置位 ⇒ 正解是把 source=1 中断真正派发进 guest ISR。
- **改动**：`recomp/src/t2rotf_gpu_signal.cpp`（64,966 → 68,966 字节）——① `sa2_renut_gpu_signal_hostcb_s1` 默认 false→**true**；② 暂存块判据放宽为「非零 + 4 字节对齐 + 落在可寻址 32 位范围」，派发时把 `*(blk+16)` 纳入 `cb_guard` 一次性守卫，经 `Enviar(callback, 1, dev, cpu)` 走 guest 自己的图形中断回调（只读 + 派发，绝不写暂存块）；③ 新增 `sa2_renut_gpu_signal_hostcb_s1_mode`（0/1/2，缺省 2）；④ `[T2DIAG GPSIG2 dev=…]` 只读补齐门的就绪判据输入（`ptr272`/`c16`/`A0..C2`/`dv10904`/`blk0`/`blk4`）；⑤ 版本标记改为 `[video] T2ROTF BUILD v8.5-COMPLETE (…)`。**不写任何显示会计、不碰门/泵热路、不改 v8.2 的 shadow 安全访存、保留 v8.4 驱动线程拆分与让位语义、`presenter()/provider()` 仍为 null（画面仍由 Renut/deko3d 出）**。`recomp/src/t2rotf_gpu_signal.h` 与 `recomp/src/t2rotf_video_nativo.cpp` **未改动**。
- **校验**：`ninja -j4 T2ROTF` 退出码 0；再跑仍 no-op（`Codegen summary: 0 written, 0 unchanged, 0 deleted, 1 module(s) up to date`，NRO 的 mtime/sha256 不变）⇒ 构建已完整收尾且可复现；NRO 字符串取证（`strings -a | grep -c -F`）`v8.5-COMPLETE`=3、`sa2_renut_gpu_signal_hostcb_s1_mode`=4、`sa2_renut_gpu_signal_hostcb_s1`=8、`T2DIAG GPSIG2`=3、`blk0=`=1；`T2ROTF.toml` 经 `tomllib` 解析通过、**23 键**读回正确（`hostcb_s1=true`、`hostcb_s1_mode=2`）。
- **残余风险**：**本版尚未实机验证**；v8.5 只解除「提交完成中断被抑制」这一条，**不保证** guest 自己的 retire 路径在当前 HLE/Renut 环境下一定走得通——若下轮出现「`s1` 涨而 `dentro` 仍 1」，则说明 `base+288` 的孤儿在飞项另有独立减点，需转入 guest ISR 内部逐指令取证（`sub_82881930` 交付函数尚未逐行读）；「不超频流畅 60 帧」**仍未达成也未实测**（T26 待做）。

### 2026-10-31（v8.4-SPLIT）— 每帧提交驱动分独立线程：解除 v8.3 的垂直同步线程自锁（T56）

- **产物**：`recomp/out/sw8/T2ROTF.nro` = **59,937,522 字节**，sha256 `9d78b9572d9f81c2cd487b5e6f5fe5f379e66c98be91f826070432e32c80fa3f`（上一版 v8.3/T53 = 59,929,330 字节 / `9059ed99…`，**字节数与哈希都不同**）；`T2ROTF.toml` = **38,315 字节**（**新增 1 键**：`sa2_renut_display_drive_thread=true`，共 **22 键**）；新增报告 `T2ROTF_deko3d_每帧提交驱动分线程报告.md`；包内更新 `recomp_src/t2rotf_gpu_signal.cpp`（56,659 → 64,966 字节）。
  > 本工程产物内嵌构建时间戳：**同一份源码重构建后体积相同、哈希会变**，文档与包内哈希一律以随包 NRO 实算值为准。
- **实机判读（用户回传 `rex_perfil(33)` / `rex_stderr(70)` / `T2ROTF_001(68)` / `T2ROTF_002(49)`，用户描述「实机依旧黑屏」）**：v8.3-FRAME 在跑、**无崩溃、无缺页**（`GPSIG3` 的 `sinSombraL` 恒 2、`sinSombraW` 恒 0）；v8.3 的驱动**成功过一次**（`[T2DIAG DRIVE n=153 obj=40F604E0 284 0->1 288=0 16568=5 16548=132]`），管线跑了 6 帧（`[T2DIAG PERRO]`：`bomba=iny=pres=6`、`etapa1/2/3=6/6/6`、`cadena937=8`、`despacho=8`）。但垂直同步派发线程 `t1969709635156801032` 在 `00:12:48.048` 的 `[T2DIAG DRAIN-IN n=13 …]` 之后再无一字；`[T2DIAG GPSIG]` 尾行 `00:12:47.431 vblanks=120 irq=94 mmio_r=94 mmio_w=0`（2 s 周期报告再没来）；`SWAP n=1` 全程仅 1 次、`fin=0/0/0`、`yield=88183` 冻住、`espera sub_824F3580 n` 从 2.88e7 起仍单调增长。
- **定论（T55）**：v8.3 把每帧提交驱动放在**垂直同步派发线程**上，而 guest 的每帧提交链会走到排空判据 `while (*(base+284) + *(base+288) != 0)` 上等一个显示完成；本移植里这个完成的唯一来源正是**同一条线程**派发的显示 ISR ⇒ **同一线程自锁**，中断派发被自己掐掉（`vblanks`/`irq`/`mmio_r`/`hostcb` 全线冻结），完成侧 `fin` 永不产出，整链 6 帧后停摆 ⇒ 黑屏。这**不是** guest 卡住、也**不是**缺页/崩溃。
- **改动**：`recomp/src/t2rotf_gpu_signal.cpp`（56,659 → 64,966 字节）——① 新增独立宿主线程 `T2ROTF Dirigir`（`XHostThread`，128 KB 栈，入口 `BucleDirigido`），v8.3 那套驱动逻辑**原样搬家**；② `thread_local g_en_hilo_dirigido` 守卫：`DirigirFotograma` 在 `sa2_renut_display_drive_thread=true` 且非驱动线程时**直接返回**，垂直同步线程从此绝不进入 guest 代码，只派发中断与打报告；③ `[T2DIAG DRIVE …]` 行加 `ini=`/`dentro=` 字段，并新增驱动线程启动/停止日志；④ `Shutdown` 若驱动线程仍卡在 guest 提交内则**不等待**；⑤ 版本标记改为 `[video] T2ROTF BUILD v8.4-SPLIT (the per-frame display commit driver now runs on its own dedicated thread instead of the VSync dispatch thread: …)`；新增 1 个 cvar（G15）。**不写任何显示会计、不碰门/泵热路、不改 v8.2 的 shadow 安全访存、让位语义原样保留、`presenter()/provider()` 仍为 null（画面仍由 Renut/deko3d 出）**。`recomp/src/t2rotf_gpu_signal.h` 与 `recomp/src/t2rotf_video_nativo.cpp` **未改动**。
- **校验**：`ninja -j4 T2ROTF` 退出码 0；再跑仍 no-op（`Codegen summary: 0 written, 0 unchanged, 0 deleted, 1 module(s) up to date`，NRO 的 mtime/sha256 不变）⇒ 构建已完整收尾且可复现；NRO 字符串取证（`grep -a -o -F | wc -l`）`v8.4-SPLIT`=2、`每帧提交驱动线程已启动`=1、`sa2_renut_display_drive_thread`=4、`T2ROTF Dirigir`=1、`T2DIAG DRIVE`=2、`T2ROTF BUILD`=3；`T2ROTF.toml` 经 `tomllib` 解析通过、**22 键**读回正确。
- **工程包**：`T2ROTF_switch_port.zip` 已**原地更新**（替换上列文件并追加本轮报告，其余条目按原字节写回；本轮首包快照 `recomp/out/sw8/zip_backup/T2ROTF_switch_port_20261030_v83_pre.zip`）。包体字节数与 sha256 不写死数字（包内含本文件与报告，自指），由原地更新脚本 `recomp/out/sw8/_upd_pkg_t56.py` 运行实算并打印。
- **残余风险**：**本版尚未实机验证**；v8.4 只解除自锁（让中断派发不被驱动拖住），**并不保证** guest 完成侧一定会因此产出 `fin`——若下轮 `vblanks` 持续增长而 `SWAP n=`/`disp16568` 仍不动，则说明完成侧另有独立卡点（见本轮报告第 7 节）；「不超频流畅 60 帧」**仍未达成也未实测**（T26 待做）。
- **详见**：`T2ROTF_deko3d_每帧提交驱动分线程报告.md`。

### 2026-10-30（v8.3-FRAME）— 每帧提交驱动：补上「谁来回调 guest 自己的每帧显示提交」（T53）

- **产物**：`recomp/out/sw8/T2ROTF.nro` = **59,929,330 字节**，sha256 `9059ed99ab8ab85ce31840dff01ebc1c94e750372ffb31247c7ba3535debe99a`（上一版 v8.2/T50 = 59,921,138 字节 / `22980392…`，**字节数与哈希都不同**）；`T2ROTF.toml` = **新增 4 键**（`sa2_renut_display_drive=true`、`_after_ms=2500`、`_div=1`、`_produce=false`，共 **21 键**）；新增报告 `T2ROTF_deko3d_每帧提交驱动修复报告.md`；包内更新 `recomp_src/t2rotf_gpu_signal.cpp`（46,883 → 56,659 字节）。
  > 本工程产物内嵌构建时间戳：**同一份源码重构建后体积相同、哈希会变**，文档与包内哈希一律以随包 NRO 实算值为准。
- **实机判读（用户回传 `rex_perfil(32)` / `rex_stderr(69)` / `T2ROTF_001(67)` / `T2ROTF_002(48)`，用户描述「还是黑屏，第一个动画能播放但是有声音无画面」）**：v8.2-SHADOW **无崩溃、无缺页**（`GPSIG3` 的 `sinSombraL` 恒 2、`sinSombraW` 恒 0）、**代派发链四要素全部增长**（`[T2DIAG GPSIG]` 的 `vblanks 0→600`、`irq 0→578`、`r1951 0→578`、`mmio_r 0→578`、`hostcb …→/22`、`ret` 追得上 `inv`）、旧「排空自旋」消失（`YIELD total` 从 35,439,751 降到 40 后冻结）。但 `[T2DIAG SWAP n=1]` 全程仅 **1 次**、`disp16568` 冻在 5、`a16700=a16704=0`、`mmio_w=0`、`cola284=0`、`cola288=1`。
- **定论（T52）**：显示 ISR 每次都进 guest 了，但 guest 自己「每帧把工作交给显示管理器」的那次调用（每帧提交入口 `sub_82880978`）**从未发生** ⇒ 没有新工作在飞 ⇒ 门（`*base+284`）永不就绪、泵无事可做 ⇒ `SWAP n=1`、写侧 `mmio_w=0`、`disp16568` 冻结 ⇒ **0 fps 黑屏（有声无画）**；显示线程停在 `sub_82881848` 的 `NtWaitForSingleObjectEx(0xF8000064, alertable=1)` 立即返回热自旋（`espera` 20.78M、`lr=828818DC`），GM 真阻塞在 `KeWaitForSingleObject`。
- **改动**：`recomp/src/t2rotf_gpu_signal.cpp`（46,883 → 56,659 字节）——① 垂直同步宿主线程**每个真实刷新**经 `FunctionDispatcher` 调 guest **自己的**每帧显示提交入口 `sub_82880978`（无参、纯全局状态驱动；调用后完整还原宿主线程的 guest 上下文，只保留对 guest 内存的副作用）；② 启动 2.5 s 内不驱动、管线自活（`dev+16568` 或 `base+284` 变化）时**让位 3 次刷新**（绝不叠加提交）；③ 当前显示对象 `*(0x82DBB608)` 不可用时跳过并计数；④ 新增只读诊断 `[video] T2DIAG DRIVE …`（每 2 s）与首次驱动后 4 次逐次的 `[T2DIAG DRIVE n=… 284 a→b 288=… 16568=… 16548=…]`；⑤ 版本标记改为 `[video] T2ROTF BUILD v8.3-FRAME (host drives the guest's own per-frame display commit sub_82880978 on every refresh; shadow-safe guest reads kept from v8.2)`；新增 4 个 cvar（G11…G14）。**不写任何显示会计、不碰门/泵热路、不改 v8.2 的 shadow 安全访存、`presenter()/provider()` 仍为 null（画面仍由 Renut/deko3d 出）**。`recomp/src/t2rotf_gpu_signal.h` 与 `recomp/src/t2rotf_video_nativo.cpp` **未改动**。
- **校验**：`ninja -j4 T2ROTF` 退出码 0（22:29:38→22:30:06，26.4 s）；再跑仍 no-op（`Codegen summary: 0 written, 0 unchanged, 0 deleted, 1 module(s) up to date`，NRO 的 mtime/sha256 不变）⇒ 构建已完整收尾且可复现；NRO 字符串取证（`grep -a -o -F | wc -l`）`v8.3-FRAME`=5、`sa2_renut_display_drive`=16、`sa2_renut_display_drive_after_ms`=4、`sa2_renut_display_drive_div`=4、`sa2_renut_display_drive_produce`=4、`T2DIAG DRIVE`=2；`T2ROTF.toml` 经 `tomllib` 解析通过、21 键读回正确。
- **工程包**：`T2ROTF_switch_port.zip` 已**原地更新**（替换上列文件并追加本轮报告，其余条目按原字节写回；本轮首包快照 `recomp/out/sw8/zip_backup/T2ROTF_switch_port_20261030_v82_pre.zip`）。包体字节数与 sha256 不写死数字（包内含本文件与报告，自指），由原地更新脚本 `recomp/out/sw8/_upd_pkg_t54.py` 运行实算并打印。
- **残余风险**：**本版尚未实机验证**；v8.3 的驱动依赖当前显示对象 `*(0x82DBB608)`（`kDispActual`）非空，而本轮全部日志里该地址**零命中**，其运行时取值必须靠下一轮 `[T2DIAG DRIVE …]` 判活确认（若 `noObj=` 持续增长则驱动不生效，见本报告第 7 节）；「不超频流畅 60 帧」**仍未达成也未实测**（T26 待做）。
- **详见**：`T2ROTF_deko3d_每帧提交驱动修复报告.md`。

### 2026-10-30（v8.2-SHADOW）— 显示内存影子访存 + 设备分层解析：根治「启动即 fatal」（T50）

- **产物**：`recomp/out/sw8/T2ROTF.nro` = **59,921,138 字节**，sha256 `22980392fac38f4bcf7ce6edc46650a97344fdb46ca6f9dbab4799e0c55e3a65`（上一版 v8.1/T47 = 59,912,946 字节 / `97f432ef…`，**字节数与哈希都不同**）；`T2ROTF.toml` = **26,532 字节**（**新增 1 键**：`sa2_renut_gpu_signal_dev_hook=true`，共 **17 键**）；新增报告 `T2ROTF_deko3d_显示内存影子访存与设备分层解析报告.md`；包内更新 `recomp_src/t2rotf_gpu_signal.{h,cpp}` 与 `recomp_src/t2rotf_video_nativo.cpp`。
  > 本工程产物内嵌构建时间戳：**同一份源码重构建后体积相同、哈希会变**，文档与包内哈希一律以随包 NRO 实算值为准。
- **实机判读（用户回传 `rex_crash(36)` / `rex_stderr(68)` / `T2ROTF_001(66)` / `T2ROTF_002(47)`，用户描述「本次启动直接报错，无 `rex_perfil.log` 生成」）**：v8.1-GUESTCB **启动即 fatal**——两次崩溃同一 pc 偏移（`imagen+0x2e38088`，指令 `0xb9400041` = `LDR W1,[X2]`）、同一 guest 地址 `0x820011AC`（`VdGlobalDevice` 槽）、`esr 0x92000006 / EC 0x24`；落点 `sdk/src/system/xmemory.cpp:571` 的 `Unhandled guest access violation`；`far` 与 `[DIAG] Guest memory arena mapped` 行吻合（`0x7385B61000 + 0x820011AC`）。**「无 `rex_perfil.log` 生成」是后果不是原因**：宿主线程在第一轮派发尝试时就被致命路径终止，进程在日志落盘前死掉。
- **定论（T49）**：宿主把 `TranslateVirtual(0x820011AC)` 的结果**直接解引用**；本移植的 chunk 在提交时**并不映射进窗口视图**，只有 guest 自己首次访问（`RexGmFaultIn`）才映射 ⇒ 宿主直读「已提交但未映射 / 尚未提交」的窗口页必然 `translation fault`；`AccessViolationCallback` 只处理 `kGuestPhysical` heap、不为窗口未提交页补齐 ⇒ 唯一安全读法是走 chunk 的 **shadow 别名**（`RexGmShadowFor`）。
- **改动**：`recomp/src/t2rotf_gpu_signal.cpp`（37,855 → 46,883 字节；头文件 1,225 → 1,593 字节，新增 `NotificarDispositivo(uint32_t)` 声明）——① 所有宿主侧 guest 访存（`Leer32`/`Escribir32` 及一切槽/字段读）改走 `RexGmShadowFor` 的 shadow 别名，取不到就**跳过并计数**（`sinSombraL/W`），**绝不直读直写、绝不代客提交**；② `Dispositivo()` 改为分层：cvar（G7）> `VdGlobalDevice` 双槽（shadow 安全读）> `g_dev_hook`（G10）；③ 新增 `NotificarDispositivo()` 与只读诊断 `[video] T2DIAG GPSIG3 hookDev=… sinSombraL=… sinSombraW=… sinSombraAvisada=…`（每 2 s）；④ 版本标记改为 `[video] T2ROTF BUILD v8.2-SHADOW (guest memory read through the chunk shadow alias; device resolution layered cvar>slot pair>D3D hook)`。`recomp/src/t2rotf_video_nativo.cpp` 在 `REX_HOOK_RAW(sub_822D4488)` / `REX_HOOK_RAW(sub_822D47A0)` 钩子里接入 `::t2rotf::gpu_signal::NotificarDispositivo(ctx.r3.u32)`（仅 `ctx.r3.u32 != 0` 时）。**不写任何显示会计、不碰门/泵热路、`presenter()/provider()` 仍为 null（画面仍由 Renut/deko3d 出）**。
- **校验**：`ninja -j4 T2ROTF` 退出码 0（首次因 `fmt` 编译期检查失败——`Informe()` 格式串占位符与实参不匹配——修好后重建通过）；对象级取证 `CMakeFiles/T2ROTF.dir/src/t2rotf_gpu_signal.cpp.obj` 重编译、`nm` 命中 `U RexGmShadowFor`、`objdump -d` 命中 `0x1951` 常量与 `bl <RexGmShadowFor>`、`objdump -r` 命中 `R_AARCH64_CALL26 RexGmShadowFor`；NRO 字符串取证（本轮实测，`grep -a -o -F | wc -l`）`v8.2-SHADOW`=3、`sa2_renut_gpu_signal_dev_hook`=4、`GPSIG3`=1、`sinSombraL`=1、`sinSombraW`=1、`所在 chunk 无 shadow`=1、`设备解析 来源`=1、`D3D 钩子观测到设备`=1、`T2ROTF BUILD`=3；`aarch64-none-elf-nm -g --defined-only T2ROTF` 命中 `RexGmShadowFor`@0xd8df0、`RexGmFaultIn`@0xd79c0、`_ZN6t2rotf10gpu_signal20NotificarDispositivoEj`@0x2e38900；`T2ROTF.toml` 经 `tomllib` 解析通过、17 键读回正确。
- **工程包**：`T2ROTF_switch_port.zip` 已**原地更新**（替换上列文件并追加本轮报告，其余条目按原字节写回；本轮首包快照 `recomp/out/sw8/zip_backup/T2ROTF_switch_port_20261030_pre.zip`）。包体字节数与 sha256 不写死数字（包内含本文件与报告，自指），由原地更新脚本 `recomp/out/sw8/_upd_pkg_t50.py` 运行实算并打印。
- **残余风险**：**本版尚未实机验证**；`t2rotf_video_nativo.cpp` 的 `Leer32`/`Escribir32`（`base+addr` 直读）与 `PublicarRptr()`（走 `TranslatePhysical`）属**同源风险**，本版未强制改动（避免扩大改动面且无实机证据），若再出 `rex_crash.log` 需按同一 shadow 方案处理；`0x820011AC` 所在 chunk 是否已提交无法从桌面断定（两种情形均已覆盖，日志用 `sinSombra` 计数 + 设备解析来源行区分）；「不超频流畅 60 帧」**仍未达成也未实测**（T26 待做）。
- **详见**：`T2ROTF_deko3d_显示内存影子访存与设备分层解析报告.md`。

### 2026-10-30（v8.1-GUESTCB）— 显示中断回调「代客派发」：补上「谁来叫它」（T47）

- **产物**：`recomp/out/sw8/T2ROTF.nro` = **59,912,946 字节**，sha256 `97f432efd8d74f833e175aa9d6f66463233fc59a32998e1797353bcf7f1a4dbe`（上一版 v8.0/T44 = 59,888,370 字节 / `77230192…`，**字节数与哈希都不同**）；`T2ROTF.toml` = **22,366 字节** sha256 `3e17b8112df137e325eee056a35696798bd9ee22f8d94d5341ed879d3d6a189c`（**新增 4 键**：`sa2_renut_gpu_signal_hostcb=true`、`sa2_renut_gpu_signal_cb_data=0`、`sa2_renut_gpu_signal_cb_guard=true`、`sa2_renut_gpu_signal_hostcb_s1=false`，共 **16 键**）；新增报告 `T2ROTF_deko3d_显示中断回调代客派发报告.md`；包内更新 `recomp_src/t2rotf_gpu_signal.cpp`（410 → 709 行）。
  > 本工程产物内嵌构建时间戳：**同一份源码重构建后体积相同、哈希会变**，文档与包内哈希一律以随包 NRO 实算值为准。
- **实机判读（用户回传 `rex_perfil(31)` / `rex_stderr(67)` / `T2ROTF_001(65)` / `T2ROTF_002(46)`，用户描述「依旧是只有黑屏，在第一个 logo 之后卡住」）**：v8.0-GPUIRQ **只补了一半**——安装期三行齐全、`[T2DIAG GPSIG]` 的 `vblanks` 按 60 Hz 精确增长（0→3841 / 64 s、0→3121 / 52 s），但**全程** `cb=00000000 ud=00000000 irq=0 r1951=0 mmio_r=0 mmio_w=0 wptr=0(0) rptr_pub=0 sup1844=0`（`T2DIAG GPSIG wptr=` 一次都没打）；guest 侧 `irq sub_822D47A0`=0 / `irq sub_822D9570`=0，而 `init sub_822D4A08`=6 / `externo sub_822D4488`=5（显示环初始化跑过）；`PERRO dev=40070600 vblank16548=5 tick16552=33938253 sub16560=5 fr16564=5 disp16568=5 ringR16700=0 ringW16704=0`（**全程冻结**）；`PUERTA n=29 r30=82E55E20 … cnt=0/0/0 cola284=0 cola288=0`；`espera sub_824F3580 n=42500000 r3=F8000064 r4=1 r5=0 lr=828818DC`（仍在事件上**死等**）；`PERRO2 dtick=7 ddrain=4/11 dpump=22/0/20 ddeliv=6 dcmd=0/0 dprod=6`（**泵成功 0**）；`YIELD total=37 top=[82881DE8x29]`（v7.7 的对齐**没有回归**）；`juego 0.0 fps`、`CPU total 99%`；`rex_stderr` 25 行**无访问违例、无崩溃**；`relojes: CPU 1785.0 / GPU 921.6 / 内存 1600.0 MHz`（本次为**高时钟**状态，GPU 由本工程 APM 请求 `0x92220008` 抬到 921.6 ⇒ `0 fps` 是**管线停摆**，不是性能不足）。
- **定论（T46）**：**v8.0 装好了电话线，但没人拨号。** 派发线程需要知道「该调 guest 的哪个回调」，该信息来自 guest 调 `VdSetGraphicsInterruptCallback`；该注册点在生成代码 `T2ROTF_recomp.28.cpp:22182 DEFINE_REX_FUNC(sub_822E1690)`（22262-22274：`addi r3,r11,18336`=**0x822D47A0**、`mr r4,r31`=设备、`bl __imp__VdSetGraphicsInterruptCallback`；22294 `stw r31` 发布 `VdGlobalDevice`），属于 guest 的 D3D 设备创建路径，被 **Renut 的原生 deko3d 设备接管整体旁路** ⇒ 安装期 `GPSIG: guest 注册图形中断回调 …` 一次没打、`cb=0` ⇒ 一个中断也不发 ⇒ `sub_822D47A0` 未进（所以连它那唯一一次寄存器读都没发生 ⇒ `mmio_r=0`）⇒ 真 ISR `sub_822D9570` 不跑 ⇒ `dev+16548` 族冻结、AVIVO `0x7FC86110`（`0x1844`）不回写、显示侧等待无人释放 ⇒ 帧管线停在第 6 次交付 ⇒ `0 fps` 黑屏。
- **改动**：整体重写 `recomp/src/t2rotf_gpu_signal.cpp`（410 → 709 行；头文件 26 行未变；`T2ROTF_app.h` 接入点、SDK 侧 `rex_app.cpp` overlay 分支、CMake 源清单均无需改动）：`Interrupcion()` 在 `callback_==0` 时**按 guest 自己的注册语义代客派发**——**同一回调地址 `kCallbackGuest=0x822D47A0`、同一设备、同一参数 `{source=0, user_data=dev}`**（`Enviar()` → `SetActiveCpu` → `ExecuteInterrupt(thread_state(), cb, argumentos, 2)`，与 SDK `graphics_system.cpp:295-332 MarkVblank→DispatchInterruptCallback(0,2)` 同语义）；设备由 `Dispositivo()` 用 `VdGlobalDevice` 两跳（`0x820011AC` 退 `0x820011B0` → cell → dev，`TranslateVirtual` 读，解析一次即缓存，一次性打印 `GPSIG: 设备解析 slot=… cell=… dev=…`，可用 `sa2_renut_gpu_signal_cb_data` 手工覆盖）；新增指针守卫 `GuardarCampo()`（ISR 尾部间接调 `*(dev+16544)`＝实机 `0x82880A48`；非 guest 代码地址时本次派发期间临时清零、调用后还原，首例打印一次）；`Completado()` 处理 source=1（默认关，`*(dev+10900)` 不可用就一条不发，`blk+16` 同样守卫）；新增只读诊断 `[video] T2DIAG GPSIG2 dev=… cb16544=… v16548=… v16552=… v16568=… a16700=… a16704=… blk10900=… blk16=… f10940=… f10941=… f10896=… evA0=… evE0=… flagC8=… wD0=… fD4=… fE4=… st5F8=… st690=… cont=… obj=… cola284=… cola288=… m272=… m276=… m280=…`（每 2 s 一行）；版本标记改为 `[video] T2ROTF BUILD v8.1-GUESTCB`。**不写任何显示会计、不碰门/泵热路，`presenter()`/`provider()` 仍为 null（画面仍由 Renut/deko3d 出）**。
- **校验**：`ninja -j4 T2ROTF` 退出码 0（首次因 `fmt` 编译期检查 `argument not found`——`Informe()` 格式串 21 占位符对 20 实参——修好后重建通过）；NRO 字符串取证（本轮实测）`v8.1-GUESTCB`=6、`T2ROTF BUILD`=3、`T2DIAG GPSIG2`=3、`T2ROTF GPSIG`=16、`T2ROTF VSync`=1、`0x7FC80000`=4、`sa2_renut_gpu_signal_hostcb`=8、`sa2_renut_gpu_signal_cb_data`=4、`sa2_renut_gpu_signal_cb_guard`=4、`sa2_renut_gpu_signal_hostcb_s1`=4、`宿主代客派发`=2；`T2ROTF.toml` 经 `tomllib` 解析通过、16 键读回正确。
- **工程包**：`T2ROTF_switch_port.zip` 已**原地更新**（**78 条目**；替换上列文件并追加本轮报告，其余条目按原字节写回，不重建 staging 目录；本轮首包快照为 `recomp/out/sw8/zip_backup/T2ROTF_switch_port_20261030_pre.zip`＝**27,579,319 字节 / `15e2db9b52e2a87b56601c1455770354706aa2912ebcf991b46a6d2d21d1bdb7`**，v8.0/T44 的包体在本次原地更新时被覆盖、其内容随 v8.0 交付包保留在用户侧，更早的历史包备份在 `out/t2-tools/zip_backup/`，最近一个是 v7.7 的 `T2ROTF_switch_port_20261029_v77.zip`）；包内 `T2ROTF.nro` / `T2ROTF.toml` / `README.md` / `recomp_src/t2rotf_gpu_signal.{h,cpp}` / `recomp_src/renut_engine/gpu/d3d/hooks/device.cpp` / `recomp_src/t2rotf_video_nativo.cpp` / `T2ROTF_app.h` 与磁盘对应文件**逐字节一致**（`testzip` 无错）。包体字节数与 sha256 不写死数字（包内含本文件与报告，自指），由原地更新脚本 `recomp/out/sw8/_upd_pkg_t48.py` 运行实算并打印。
- **残余风险**：**本版尚未实机验证**；`espera sub_824F3580` 等的事件 `F8000064` 在 vblank ISR 里**没有** `KeSetEvent`（`sub_822D9570` 只做 `dev+16548 += 1`、记 `16552`、条件写 `0x1844`、`dev+16700 += 1`、尾部条件间接调 `*(dev+16544)`）⇒ **释放者可能不在 vblank 侧**，若只让 `irq/v16548` 动起来而等待方仍死等，用 `sa2_renut_gpu_signal_hostcb_s1=true` 做 A/B；「不超频流畅 60 帧」**仍未达成也未实测**（T26 待做）；`sub_82881F30` 的注册点、`0x82DBB5D0/D4/E4` 写入者、`sub_828816D8` 是否执行过仍未查实。
- **详见**：`T2ROTF_deko3d_显示中断回调代客派发报告.md`。

### 2026-10-29（v8.0-GPUIRQ）— 黑屏最后一环：显示完成信号源补齐（T44）

- **产物**：`recomp/out/sw8/T2ROTF.nro` = **59,888,370 字节**，sha256 `77230192f21b3909247c3708c9a1aba921c0a16ee4c59965e6d9e4a0657c5753`（上一版 v7.7/T41 = 59,859,698 字节 / `38e341ee…`，**字节数与哈希都不同**）；`T2ROTF.toml` = **16,470 字节** sha256 `d1fc49fe27569df44cca3c78d783b59f78bbca413762505a2f66c04a10f3e737`（**新增 5 个键**：`sa2_renut_gpu_signal=true`、`sa2_renut_gpu_vblank_hz=60`、`sa2_renut_gpu_signal_cpu=2`、`sa2_renut_gpu_signal_diag=true`、`sa2_renut_gpu_rptr_sync=false`，加上原七键共 **12 个键**）；新增报告 `T2ROTF_deko3d_显示完成信号源补齐报告.md`；包内新增新模块源码 `recomp_src/t2rotf_gpu_signal.h` / `recomp_src/t2rotf_gpu_signal.cpp`，并替换 `recomp/src/T2ROTF_app.h`（9,196 → 10,556 字节）与 `sdk_changes/src/ui/rex_app.cpp`（`repos/nfsmw-nx/sdk/src/ui/rex_app.cpp`，25,496 → 25,937 字节）。
  > 本工程产物内嵌构建时间戳：**同一份源码重构建后体积相同、哈希会变**，文档与包内哈希一律以随包 NRO 实算值为准。
- **实机判读（用户回传 `rex_perfil(30)` / `rex_stderr(66)` / `T2ROTF_001(64)` / `T2ROTF_002(45)`）**：v7.7-ALIGN **有效但不充分**——`T2DIAG ALIGN n=1..4 a_antes=41770030 dev_antes=41770018 a_ahora=82E55E38`（对齐本体在跑）、`T2DIAG YIELD total=36 top=[82881DE8x27] [82881918x9]` 且 03:24:18/20/22 三次采样完全一致（**排空自旋 35,439,751 → 36 后彻底冻结**）、`rex_stderr` 25 行无任何访问违例。但 `T2DIAG espera sub_824F3580 n=34,520,000 r3=F8000060 r4=1 r5=0 lr=828818DC`（影片侧**死等 F8000060 事件**）、`T2DIAG PERRO dev=40070600 vblank16548=5 tick16552=357789309 sub16560=5 fr16564=5 disp16568=5`（**4 秒内一字不变**）、`PERRO pump … irq47=0 irq95=0`、`PUERTA n=26 … cnt=0/0/0 cola284=1 cola288=0` → `n=27 … cola284=0 cola288=1 obj256=0/0/0`、`PERRO2 dtick=7 ddrain=4/11 dpump=19/0/17 ddeliv=6 dcmd=0/0 dprod=6`（**成功 0**）、`pump … etapa1/2/3=0/0/0 fin=0/0/0 pres=0`、`juego 0.0 fps`、`CPU total 113%`、`Game Master` 115%、时钟 `CPU 1020.0 / GPU 460.8 / 内存 1600.0 MHz`（**未超频**）、音频正常（`3752 bloques`、`319/469 buffers audout con PCM no nulo`、`pico 0.493`）⇒ **有声无画**。
- **定论（T44）**：**黑屏最后一环是「显示完成信号源」在切到 Renut 时被一并删掉了。** Renut 接管 D3D 时清空 `config.graphics`（`renut_engine/gpu/overlay/app_integration.cpp:87`），连带移走 SDK 原本提供的两样东西：① `0x7FC80000` 的 64 KB GPU 寄存器 MMIO 窗口——没有它，guest 显示链入口读 `0x7FC86544`（寄存器 `0x1951 = D1MODE_VBLANK_VLINE_STATUS`）落到裸 guest 物理内存读回 0 ⇒ `sub_822D47A0` 的 bit0 判据永假 ⇒ 真 ISR `sub_822D9570` 永不执行；② 按垂直同步派发 guest 图形中断回调的宿主线程（`graphics_system.cpp:141-176` 的 `GPU VSync` + `:319-332 MarkVblank→DispatchInterruptCallback(0,2)`）——没有它，guest 经 `VdSetGraphicsInterruptCallback` 注册的回调被 `graphics_system==nullptr` 分支静默丢弃（`sdk/src/kernel/xboxkrnl/xboxkrnl_video.cpp:319-326`）⇒ `irq47=0` / `irq95=0`、`dev+16548` 族冻结、AVIVO `0x7FC86110`（寄存器 `0x1844`）不回写、显示侧等待事件无人释放 ⇒ 影片等待方死等 3452 万次 ⇒ 帧管线停在第 6 帧 ⇒ `0 fps` 黑屏。
- **改动**：新增 `recomp/src/t2rotf_gpu_signal.h`（27 行）+ `recomp/src/t2rotf_gpu_signal.cpp`（`t2rotf::gpu_signal::GpuSignalSystem`，实现 `IGraphicsSystem` 最小子集，含文件头逐条取证）：**A** `memory_->AddVirtualMappedRange(0x7FC80000, 0xFFFF0000, 0x0000FFFF, this, &LeerMmio, &EscribirMmio)`，读值逐条对齐 SDK `graphics_system.cpp:232-262`（`0x0F00→0x08100748`、`0x0F01→0x0000200E`、`0x194C→min(display_height,0xFFF)`、**`0x1951→恒 1`**、`0x1961→(w<<16)|h`；写 `0x01C5`(CP_RB_WPTR) 记录并在 `sa2_renut_gpu_rptr_sync` 打开时才回写读指针、写 `0x1844` 吞掉）；**B** `XHostThread("T2ROTF VSync")` 按 `sa2_renut_gpu_vblank_hz`（默认 60，`<=0` 用 `VdQueryVideoMode().refresh_rate`）调 `ExecuteInterrupt(thread_state(), callback, {source=0, user_data}, 2)`、`SetActiveCpu(2)`、长停顿不补播，**唯一调用点，绝不从 guest 热路调用**；`presenter()`/`provider()` 保持 null、`has_presentation()` 为 false ⇒ 呈现仍归 Renut/deko3d，**不写任何会计、不碰门/泵热路**。接入点 `recomp/src/T2ROTF_app.h` 的 `OnPreSetup`（在 `renut::gpu::ConfigureRuntime(config)` 之后 `config.graphics = std::move(gpu_signal)`），`recomp/CMakeLists.txt` 的 `T2ROTF_SOURCES` 已加入新源文件；SDK 侧 `repos/nfsmw-nx/sdk/src/ui/rex_app.cpp` 的 overlay 分支改为 `} else if (!graphics_system || !graphics_system->presenter()) {`（本模块是「有图形系统但 presenter 为 null」形态）。诊断：`[video] T2ROTF GPSIG:` 安装期若干行、`[video] T2DIAG GPSIG vblanks=… irq=… cb=… ud=… r1951=… mmio_r=… mmio_w=… wptr=…(…) rptr_pub=… sup1844=… hz=…` 每 2 s 一行、`[video] T2DIAG GPSIG wptr=` 与 `reg …` 各前 8/64 次。
- **校验**：`ninja -j4 T2ROTF` 退出码 0（codegen「0 written, 0 unchanged, 0 deleted, 1 module(s) up to date」）；NRO 字符串取证（本轮实测）`v8.0-GPUIRQ`=6、`T2DIAG GPSIG`=4、`sa2_renut_gpu_signal`=13、`sa2_renut_gpu_vblank_hz`=4、`T2ROTF VSync`=1、`0x7FC80000`=4、`T2ROTF BUILD`=3（v7.6/v7.7/v8.0 三个标记都在），旧标签（`v7.6-RECON` / `v7.7-ALIGN` / `reconcile` / `GUARDIA` / `PUERTA` / `PERRO2`）全部仍在；接线链静态确认 `Runtime::Setup`（`sdk/src/system/runtime.cpp:184-193`）见 `config.graphics` 非空即调 `Setup` → `SetupGuestGpu`；`T2ROTF.toml` 经 `tomllib` 解析通过、12 键读回正确。
- **残余风险**：**本版尚未实机验证**；若 guest 从未调用 `VdSetGraphicsInterruptCallback`，派发线程会一直空转（表现为 `vblanks` 增长而 `cb=0`/`irq=0`），需查注册调用点与时机；`0x7FC80000` 若已被别处注册，日志会给出「MMIO 窗口注册失败」；**「不超频流畅 60 帧」仍未达成也未实测**（画面出来后按第 5 节在**不超频**条件下实测，T26 待做）；`sa2_renut_display_reconcile`（v7.6 配平）与 `sa2_renut_display_guard`（v7.5 守卫）仍默认开，显示中断链真正跑起来后前者本不应再触发（实机只命中 1 次即收敛，说明它治的不是根因）。
- **详见**：`T2ROTF_deko3d_显示完成信号源补齐报告.md`。

### 2026-10-29（v7.7-ALIGN）— 黑屏直接原因修复：显示投递通道对齐（T41）

- **产物**：`recomp/out/sw8/T2ROTF.nro` = **59,859,698 字节**，sha256 `38e341ee7f4cb4e553f2b2eaadc256dd313fbf7bfb31e352a62e59e60ff135a8`（上一版 v7.6/T38 = 59,855,602 字节 / `421923ce…`，**字节数与哈希都不同**）；`T2ROTF.toml` = **11,668 字节** sha256 `92095288472bd831eaf97c5f0ea7f66f193d7632d5c8e6c6400371ffc4f6be2c`（**新增第七个键 `sa2_renut_display_align = true`**，其余六键与 v7.6 相同）；新增报告 `T2ROTF_deko3d_投递通道错位与对齐修复报告.md`；包内新增源码级回退件 `recomp/src/t2rotf_video_nativo.cpp.bak.t41`（= v7.6 源，72,255 字节）。
- **实机判读（用户回传 `rex_perfil(29)` / `rex_stderr(65)` / `T2ROTF_001(63)` / `T2ROTF_002(44)`）**：v7.6-RECON **无效**——`[video] T2ROTF BUILD v7.6-RECON` 在跑、`rex_stderr` 无任何访问违例；但 `T2DIAG RECON nr=1 n=8 idx=0 c24=0 c28_antes=1 e28_antes=1 retro=1` 只命中 1 次即收敛，悬挂从 `284=0/288=1` 换成 `284=1/288=0`；`PUERTA … cnt=0/0/0 obj256=0/0/0`（门三槽 arm 计数与后备标记**全程 0**）；`PERRO2 … ddrain=5/12 dpump=16,591,560/0/16,591,558`（**成功 0 次**）、`ddeliv=6 dprod=7`；`juego 0.0 fps`、`Game Master` 线程 CPU 115%、`relojes: CPU 1020.0 / GPU 460.8 / 内存 1600.0 MHz`（**未超频**）。
- **定论（T40/T41）**：**投递者与消费者用的不是同一条显示通道。** 生产者 `sub_822B5608(r3=obj, r4=A)` 只做 `++*(entry+24)`、`++*(0x82E55CE0+284)`、**`++*(A+248)`（全程序唯一 arm 自增点）**、`sub_822B57E0(A+8, obj)`；而 `A` 由每帧 tick `sub_82ABB7D0` 用 `sub_828823F8()` 从 `*(0x82DBB5C4)` 数组里挑「`+0` 状态 == 5」的**当前呈现设备**算出（`A = dev + 24`，dev ≠ `0x82E55E20`）。消费者链恒定围绕 `0x82E55E20`：门 `sub_82881B88` 就绪读 `*(*(0x82E55E20+16)+272+4*slot)`；交付 `sub_82881930(ctx=0x82E55E20+24)` 从 `*(ctx)+248+4*slot` 扣减、弹 `ctx+slot*80+8`。⇒ arm 计数与队列全落到别人的通道上 ⇒ 门读到的计数恒 0、门永不就绪（每轮末尾 yield，`LR=0x82881DE8`）⇒ 交付入口读到 0 立即返回 ⇒ 不弹记录、不减 `284`、不加 `288` ⇒ 泵 0 次成功 ⇒ 排空 `while (*(0x82E55CE0+284) + *(0x82E55CE0+288) != 0)` 永久自旋 ⇒ `0 fps` 黑屏。这也解释了「`284` 能到 1 而门读的 arm 计数始终为 0」与「7 次投递只对应 6 次交付调用」。
- **改动**：`recomp/src/t2rotf_video_nativo.cpp`（72,255 → **76,583 字节**）：新增 `REXCVAR_DEFINE_BOOL(sa2_renut_display_align, true, "SA2/Renderer", …)`（`kInitOnly`）；在既有 `REX_HOOK_RAW(sub_822B5608)` 内、调原体之前，把投递通道基址 `ctx.r4` 改写为 **`0x82E55E20+24`**，并在 `*(0x82E55E20+16)` / `*(0x82E55E20+24)` 非自指时修回自指；新增每进程一次的版本标记 `[video] T2ROTF BUILD v7.7-ALIGN` 与限频诊断 `[video] T2DIAG ALIGN`（前 16 次 + 每 512 次）、`[video] T2DIAG ALIGN-SELF`（前 8 次）。**不加不减任何会计，不碰 `284` / `288` / `entry+24` / `entry+28`，不落门/泵热路。**
- **校验**：`ninja -j4 T2ROTF` 退出码 0（73 s；codegen「0 written, 0 unchanged, 0 deleted, 1 module(s) up to date」）；NRO 字符串取证 `v7.7-ALIGN`=1、`sa2_renut_display_align`=4、`T2DIAG ALIGN`=2、`ALIGN-SELF`=1，旧标签（`v7.6-RECON` / `reconcile` / `GUARDIA` / `PUERTA` / `PERRO2` / `COLA`）全部仍在；源码回退件 `t2rotf_video_nativo.cpp.bak.t41` 重算为 72,255 字节（与文档记载的 v7.6 源字节数一致）。
- **残余风险**：若对齐后门能就绪、交付能弹出对象但**画面仍黑**，下一处要看泵的完成回调（对象虚表 `*(obj+156)` 的第一个虚方法，真正的「呈现这一帧」入口）是否在实际提交；另有第二处潜在错位点 `*(0x82DA74BC)`（每帧排空的 `idx`）与投递对象 `*(obj+64)` 是否一致（两者都已写进 `T2DIAG ALIGN` 行）。**本轮仍不包含帧率达标**，待画面出来后按第 5 节在不超频条件下实测。

### 2026-10-29（v7.6-RECON）— 排空自旋根因修复：显示完成侧配平（T38）

- **产物**：`recomp/out/sw8/T2ROTF.nro` = 59,855,602 字节，sha256 `421923cee8c53a66be9b6e0c268d5576f68d139c6e59be81ae7b7156a42b10bb`（上一版 v7.5/T31 = 59,847,410 字节 / `3728fd7e…`，**字节数与哈希都不同**）；`T2ROTF.toml` = 9,178 字节 sha256 `b4fae20fcbbded74bd3332ced404e19a43b46ce3e727e69cce612895e9cbe232`（新增第六个键 `sa2_renut_display_reconcile = true`，其余五键与 v7.5 相同）；新增报告 `T2ROTF_deko3d_排空自旋根因与完成侧配平报告.md`；包内新增源码级回退件 `t2rotf_video_nativo.cpp.bak.t37`（= v7.5 源）。
  > 本工程产物内嵌构建时间戳：**同一份源码重构建后体积相同、哈希会变**，文档与包内哈希一律以随包 NRO 实算值为准。
- **实机判读（用户回传 `rex_perfil(28)` / `rex_stderr(64)` / `T2ROTF_001(62)` / `T2ROTF_002(43)`）**：v7.5 的守卫与只读化已生效——`rex_stderr` 25 行只有初始化流程、**无任何访问违例**；`cola288` 不再回绕（PUERTA `n=1 → 36,000,000` 期间 `cola284=0`、`cola288` 恒 1）；`GUARDIA` / `cola-readonly` / `release` / `display-agent` / `display-tick` / `callthrough` **全 0**。但 `juego 0.0 fps` 依旧（`Game Master` 线程 CPU 98.3% / 98.8%；时钟 `CPU 1020.0 MHz / GPU 460.8 MHz / 内存 1600.0 MHz` ⇒ **本次运行未超频**）。
- **卡点定量**：`YIELD total=35,439,751`、`top=[82881DE8x35,439,743]`（唯一自旋点 = 门内那条 yield）；`PUERTA` 的 `cnt=0/0/0`、`tcnt=0` 全程 0；`PERRO2` `dpump=35,434,483/0/35,434,481`（进入 / 成功 / 返回，**成功 0**）、`ddeliv=6`、`dcmd=0/0`、`dprod=6` ⇒ 泵被进入 3543 万次却一件也没交付 ⇒ 0 fps、第一个 logo 之后有声无画。
- **根因定论（T36，逐字读生成代码）**：在飞显示项 `*(base+288)`（base = `0x82E55CE0`）全程序只有两个增减点——`++` 在交付 `sub_82881930`（`addi r11,r3,288` 自增）、`--` 在泵 `sub_82881E00` 完成回调之后（`idx != -1` 时同刻 `--*(entry+28)`），两者本应在同一次泵调用里成对；实机读数 `284=0 / 288=1 / e24=0 / e28=1` 证明有**一次「已交付、完成侧未执行完」的悬挂**（本移植缺 VSync / GPU 完成中断源；ISR 族 `sub_82997AF8` / `sub_82997968` / `sub_829975F0` 在生成代码里没有任何 guest 调用者）。该残留又自己满足门的条目判据 `*(entry+24)+*(entry+28) > 0` ⇒ 门每轮报就绪、交付每轮弹不到记录、泵报 0、排空 `while (*(base+284)+*(base+288) != 0)` 无限重旋。
- **改动**：`recomp/src/t2rotf_video_nativo.cpp`（67,806 → 72,255 字节，备份 `recomp/src/t2rotf_video_nativo.cpp.bak.t37`）：新增 `REXCVAR_DEFINE_BOOL(sa2_renut_display_reconcile, true, "SA2/Renderer", …)`（第 1277 行，`kInitOnly`）与配平块（第 1316–1344 行，落在既有 `REX_HOOK_RAW(sub_82882200)` 内、调用原体之前）：只在排空入口、仅当 `e24 == 0 && e28 > 0` 时，先 `--*(base+288)`、再按同次数 `--*(entry+28)`（各自 > 0 才减、单帧上限 64、**不碰 284 / entry+24**）；新增每进程一次的版本标记 `[video] T2ROTF BUILD v7.6-RECON`（第 1296 行）与限频诊断 `[video] T2DIAG RECON` / `[video] T2DIAG DRAIN-IN`。
- **为什么落在 `t2rotf_video_nativo.cpp`**：一个 guest 入口只能有一个定义（同地址在两个 TU 各写 `REX_FUNC` 会重复符号），而该文件已有 `REX_HOOK_RAW(sub_82882200)` 与全部 `REX_EXTERN`。
- **校验**：`ninja -j4 T2ROTF` 退出码 0（51 步；codegen「0 written, 0 unchanged, 0 deleted, 1 module(s) up to date」0.8 s）；NRO 字符串取证 `v7.6-RECON`=1、`sa2_renut_display_reconcile`=4、`T2DIAG RECON`=1、`T2DIAG DRAIN-IN`=1，旧标签（`GUARDIA` / `PUERTA` / `PERRO2` / `COLA`）全部仍在。
- **未决与风险**：**本版尚未实机验证**；「不超频流畅 60 帧」仍未达成也未实测（T26 待做）；正解仍是补「显示完成事件源」，本配平只是让被中断的悬挂不再毒死帧循环；`0x82DBB608`（当前显示对象）的创建者仍未定论。
- **详见**：`T2ROTF_deko3d_排空自旋根因与完成侧配平报告.md`。

### 2026-10-28（v7.5-DEKO3D-GUARD）— 启动失败双根因修复：写会计只读化 + 完成链守卫（T31）

- **产物**：`recomp/out/sw8/T2ROTF.nro` = 59,847,410 字节，sha256 `3728fd7ed98e5801b905c4e4fbd7d309f1403a232c921f2ebcd1d67eeb2eeeab`（上一版 `7186022a…`，**字节数巧合相同，比对务必用哈希**）；`T2ROTF.toml` 重写为 6,990 字节（sha256 `b77adfc88a4dbc51a32a8b38fa26a397d60e33eaa35e3479f76df5c9484a212c`：`guard=true`、`release=false`、`agent=false`、另两个仍 false）；新增报告 `T2ROTF_deko3d_启动失败双根因与守卫修复报告.md`。
- **实机现象（用户）**：「从 Sphaira 启动无限黑屏没有任何加载；从前端启动直接报错退出」。
- **根因一（Sphaira 无限黑屏，定量）**：v7.4 的完成侧释放挂在**门热路**上，门在帧循环卡死时被以约 10 万次/秒 调用；`T2ROTF_001(61)` 里 `release n=1` 之后同一次启动内 `cola288` 由 1 单调降到 `4294967295/4294967294/…`（即 16,760,832 次扣减把 1 刷过 0 回绕）⇒ `while (*(base+284)+*(base+288) != 0)` 永不退出 ⇒ 启动即无限自旋、无任何加载。
- **根因二（前端报错退出）**：用**当前**构建符号表重解 `rex_crash(35)`（旧 `_syms_now.txt` 整体偏移 +0x330，旧解「崩在 sub_8287D0F8+0x1bc」已作废）：`pc = imagen+0x2a5444c = __imp__sub_8287BF88+0x7c`、`lr = +0x2c`、`far = 0x63a5422368 = guest 基址 0x63a5422000 + 0x368`、`x0=x1=0x368`、`x20=0x63a5422000`；反汇编确认现场指令 `ldr w1,[x20,w1,uxtw]` 且 `w1=0x368`。`sub_8287BF88`（43.cpp:91679）**首条语句**即 `REX_LOAD_U32(*(0x82DBB608) - 18936)`（无非空判断）⇒「当前显示对象」为 0 时必然 `*(0+0x368)` 违例。生成代码里找不到初始化该单元的独立路径（`sub_8287E410` 只是「换入→更新→换回」包装）。
- **改动**：`recomp/src/renut_engine/gpu/d3d/hooks/device.cpp`（98,921 → 103,185 字节，备份 `device.cpp.bak.t29` = v7.4 基线）：
  ① `sa2_renut_display_release` / `sa2_renut_display_agent` 默认 true→false，`DisplayAgentStep` 的两段写会计改为**只读观察 + 限频 `[renut] T2DIAG cola-readonly`**（字段 `cola284/cola288/e24/e28/obs/onj`）；
  ② 新增 `kDisplayObjectCell = 0x82DBB608` 常量与 `sa2_renut_display_guard`（默认 true，`kInitOnly`）；
  ③ 文件末尾新增四个 `BANJO_RENUT_HOOK_RAW` 守卫：`sub_8287BF88` / `sub_8287DD08` / `sub_8287E410` 在 `*(0x82DBB608)==0`（或 `r3==0`）时短路不调原体，`sub_8287D7C0` 在 `r3==0` 时短路；命中记限频 `[renut] T2DIAG GUARDIA`（`fun=` / `onj=` / `cola284` / `cola288`）。
- **为什么不保留「只清一次」的写法**：单次清零虽能让门就绪，但随即走进「当前显示对象为空」的完成回调链 ⇒ 前端直接报错退出；该补的是**显示完成事件源**（承接 v7.4 结论），不是在门热路上写会计。
- **校验**：`ninja -j4 T2ROTF` 退出码 0（52 步增量；codegen「0 written, 0 unchanged, 0 deleted, 1 module up to date」36.0 s）；产物字符串取证 `GUARDIA`=1、`cola-readonly`=1、`sa2_renut_display_guard`=2 全部命中；源码核查 `StoreGuestU32` 仅剩定义、`escritos` 计数 0（无残余写会计）；四个 `BANJO_RENUT_HOOK_RAW` 与 `REXCVAR_DEFINE_BOOL(sa2_renut_display_guard` 均在源码命中。
- **未决与风险**：**本版尚未实机验证**（需回传含 `T2DIAG GUARDIA` 行的日志）；「第一个 logo 之后有声无画」与**不超频 60 帧**仍未解决（显示完成事件源缺失＝下一里程碑）；门热路上的 `DisplayAgentStep` 调用条件仍挂在两个（现为 false 的）开关上，所以默认启动不再有 `cola-readonly` 行——需要该观察日志时把两个开关置 true（只读，无副作用）。
- **包内容（本版交付）**：zip 共 65 条目；`T2ROTF.nro` / `T2ROTF.toml` / `README.md` / `recomp_src/renut_engine/gpu/d3d/hooks/device.cpp` 已原地替换，新增 `renut_device.cpp.bak.t29`（= v7.4 基线，源码级回退用）与本报告；逐项 sha256 与源文件核对一致、`testzip()` 通过。`sdk_changes/` 与顶层源快照自 v7.4 起即已刷新为构建树现状（下文 v6.35 记录中「仍是 v6.35 时代」一句已过时）。
- **详见**：`T2ROTF_deko3d_启动失败双根因与守卫修复报告.md`。

### 2026-10-28（v7.4-DEKO3D-RELEASE）— 显示完成链取证 + 宿主侧完成释放（T29）

- **产物**：`recomp/out/sw8/T2ROTF.nro` = 59,847,410 字节，sha256 `7186022a4681553d1c75d5f26c1ea1197fc4d8ddff44db5de16a2c284558ac5a`（上一版 `cd806cc1…`，59,843,314 字节）；`T2ROTF.toml` 新增第四个键 `sa2_renut_display_release`（默认开）；新增报告 `T2ROTF_deko3d_显示驱动链取证与完成释放报告.md`。
  > 注意：本工程产物内嵌构建时间戳，**同一份源码重构建后体积相同、哈希会变**（本轮实测 `d463c68e…` → `7186022a…`），文档与包内哈希以随包 NRO 实算值为准。
- **T28 为何在实机上一次都没生效（决定性取证）**：T28 的门钩子前置判断只读「已投递」两个字（`base+284` / 条目 `+24`），而实机卡死态残留恰在「已交付未完成」两个字上（`base+284=0`、条目 `+24`=0、`base+288=1`）⇒ 判断为假、`DisplayAgentStep` 从未被调用，排空 `while` 照样非 0。
- **根因（定量）**：Switch 后端**没有显示完成事件源**——Xbox 上把 `base+288` / 条目 `+28` 减回去的是 GPU 垂直同步/完成中断（ISR 族 `sub_82997AF8` / `sub_82997968` / `sub_829975F0` 在生成代码里没有任何 guest 调用者；实机 `irq47=0` / `irq95=0` 吻合）。1 份 `base+288=1` 就足以让每帧 `while (*(base+284)+*(base+288) != 0)` 永不为 0 ⇒ `0.0 fps`、第一个 logo 之后有声无画，且永远走不到生产者 `sub_822B5608`（通道不再 arm）⇒ 门永不就绪。
- **改动**：
  ① `recomp/src/renut_engine/gpu/d3d/hooks/device.cpp`（93,912 → 98,921 字节，备份 `device.cpp.bak.t29`）：新增 `sa2_renut_display_release`（默认 true，`kInitOnly`）；`DisplayAgentStep` 拆成两半（release 退 `base+288`/条目 `+28`，agent 退 `base+284`/条目 `+24`，均有「非 0 才退、绝不回负」守卫）；门侧连续观测门槛 `32 → 8`；门钩子前置判断改为「四个会计字任一非 0」；Swap 与门的调用条件改为两开关任一；`T2DIAG COLA` 追加 `rel=/ag=/tf=`；新增限频 `[renut] T2DIAG release …` 行（前 24 次 + 每 4096 次）。
  ② `recomp/src/t2rotf_video_nativo.cpp`（62,433 → 67,806 字节，全为附加的 3 个 `[T29]` 只读块）：10 个计数器 + 6 个只读钩子（`sub_82ABB7D0` tick / `sub_82882200` 排空 / `sub_82881E00` 显示泵 / `sub_82881930` 交付 / `sub_82881838` 显示命令 / `sub_822B5608` 生产者，全部无条件转发）+ 看门狗新行 `[video] T2DIAG PERRO2 …`。
- **为什么不采用「把六通道标记置 1 强行 arm 门」**：置 1 只让门「就绪」而通道队列仍为空 ⇒ 交付返回 0；`base+284/+288`、条目 `+24/+28` 不对账，且标记不会自然递减 ⇒ 每帧空交付/重复交付。
- **安全性边界**：只在「六条通道标记全 0（确认无在飞条目）**且**四个会计字连续同一指纹」时才动手，每次最多各退 1、绝不回负；任一通道被 arm 立刻停手、完全交还 guest 流水线；**不改 guest 控制流一个字节**。
- **校验**：`ninja -j4 T2ROTF` 退出码 0；产物字符串取证 12 项全部命中（`sa2_renut_display_release`=2、`T2DIAG release`=1、`T2DIAG PERRO2`=1、`dpump=`/`dcmd=`/`dprod=`/`rel=`/`tf=` 各 1、`ag=`=3，等）。
- **未决与风险**：**T29 构建尚未实机验证**（用户手上前一轮日志属 T25 构建，无 `COLA`/`release`/`PERRO2` 行）；**不超频 60 帧仍未实测**；`*(container+16)`（`0x82E55E30`）写入者、`*(0x82DA74BC)` 实机取值（由 `tf=` 回答）、`*(container+0)` 显示状态字段写入者仍待日志。
- **详见**：`T2ROTF_deko3d_显示驱动链取证与完成释放报告.md`。

### 2026-10-28（v7.3-DEKO3D-DISPLAYAGENT）— 显示队列完成侧代理 + kDisplayCtx 修正（T28）

- **产物**：`recomp/out/sw8/T2ROTF.nro` = 59,843,314 字节，sha256 `cd806cc16affdc60172fe3c9c5c0f75499920bbb560133779342588cbfd4e876`（上一版 `e5797164…`）；`T2ROTF.toml` 新增第三个键（默认开）；新增报告 `T2ROTF_deko3d_显示完成侧代理与kDisplayCtx修正报告.md`。
- **实机反馈**：两个开关（`sa2_renut_display_tick`、`sa2_renut_swap_callthrough`）全开或全关都是「有声无画」。
- **两条候选无效的直接原因（已定论）**：① 候选一挂在死路径——`[renut] T2DIAG SWAP` 全程只有 `n=1` 一行，卡死后 Swap 不再发生，而空转在游戏自己的每帧 tick `sub_82ABB7D0 → sub_82882200` 里；② 候选二把门/泵的 `container` 记成了 `0x82E55EE0`，真值是 **`0x82E55E20`**（取证：`sub_82882200` 的 `addi r28,r11,24096`），差 `0xC0` ⇒ 门读到垃圾后立即返回 0，等于 no-op。
- **真根因**：呈现队列的**完成侧没有来源**。实机探针在门进不去时读到 `cnt=0/0/0 obj256=0/0/0 cola284=0 cola288=1 st0=0`：六条通道标记全 0（无在飞条目），却残留一份「已交付未完成」会计（`base+288=1`）。而 `base+284/base+288` 正是每帧 tick 排空循环 `while (*(base+284)+*(base+288) != 0)` 的判据 ⇒ 每帧 100% 空转、0.0 fps、第一个 logo 之后全黑。这份会计唯一的正常消除途径是门成功（交付→回调→`base+288--`），门成功又要求通道标记非 0，标记的消费者在 Xbox 上是 GPU 垂直同步侧（ISR `sub_82997AF8` 全库无 guest 调用者），Switch 后端没有来源。
- **改动**（`recomp/src/renut_engine/gpu/d3d/hooks/device.cpp`，备份 `device.cpp.bak.t28`）：
  ① `kDisplayCtx` `0x82E55EE0` → `0x82E55E20`；
  ② 新增 `sa2_renut_display_agent`（默认 true，`kInitOnly`）与 `DisplayAgentStep(origen)`：只在「六条通道标记全 0 **且** 四个会计字（`base+284/base+288`、条目 `+24/+28`）至少一个非 0 **且** 连续 N 次观测到完全同一组数值」时才把各非 0 字各退 1（绝不回退到负），N=4（Swap）/32（门）；只要有一个通道被 arm 就立刻停手，**完全交还 guest 自己的流水线**；
  ③ 调用点两处：Swap 钩子内（`DisplayAgentStep(0)`）+ 新增门覆盖钩子 `BANJO_RENUT_HOOK_RAW(sub_82881B88)`（`DisplayAgentStep(1)`，卡死态门被高频调用，是主战场；其余情况控制流一个字节都不改，末尾走 `__imp__sub_82881B88`）；
  ④ 诊断：`[renut] T2DIAG display-agent …`（限 24 条）与每 2 s 一行的 `[renut] T2DIAG COLA …`。
- **校验**：`ninja -j4 T2ROTF` 退出码 0；字符串取证 `T2DIAG display-agent` / `T2DIAG COLA` / `sa2_renut_display_agent` / `T2DIAG callthrough` / `T2DIAG display-tick` 均在 NRO 内命中；`sub_82881B88` 覆盖走与 `sub_822D9D70`（已实测生效）同一套 `REX_FUNC` 强定义覆盖机制，无自递归（链接通过、无未解析符号）。
- **未决与风险**：条目级 `e24/e28` 此前未单独打印（本轮两族都处理，靠新日志确认）；`*(0x82DA74BC)` 是否每帧重发布未定；时间窗全局写者未查；**「抽干残留」之后若仍无画面，下一步候选是让宿主每帧代客调一次 ISR `sub_82997AF8`（补上 GPU 完成中断，比抽干更贴近硬件语义但风险更大）**；**不超频 60 帧仍待实测**（T26）。
- **详见**：`T2ROTF_deko3d_显示完成侧代理与kDisplayCtx修正报告.md`。

### 2026-10-27（v7.2-DEKO3D-GATE）— 呈现门探针 + 两个可切换修复候选（影片播放完死机）

- **产物**：`recomp/out/sw8/T2ROTF.nro` = 59,831,026 字节，sha256 `e5797164154f406dec318226109927e8bbf1a7a50a511bb39559c01d0e73a30e`（上一版 `a3004dd5…`，59,826,930 字节）；新增可选配置 `T2ROTF.toml`；新增报告 `T2ROTF_deko3d_呈现门阻塞语句与显示会计核对报告.md`。
- **背景（承接 v7.1 的实机日志）**：影片不再黑屏（有声音、能推进），但呈现链在 **6 帧**后彻底停摆：`vblank16548` 停在 5 达 28 秒、`fin=0/0/0`、`irq47/irq95=0`、`inKick13232=0`，同时 `YIELD total` 30 秒涨到 4,480 万且热点单点独占 `lr=0x82881DE8` ——即 guest 呈现门 `sub_82881B88` 的三槽全不就绪分支在无限自旋。
- **根因（静态取证，逐行）**：① 门的判据只读两组结构——全局呈现队列 `0x82E55CE0` 族（`+272` 条目数组、条目 `+24/+28`、`+284/+288`）与显示管理器对象 `0x82E55EE0`（`+0` 状态码、`+16→` 三槽计数、`+288`）——外加四个时间窗全局；② 宿主 `D3DDevice_Swap_hook` 推进的 `+16548/+16552/+16564/+16568`、`*(dev+10900)+4`、`+10944` 与门判据**零交集**；③ 真正会推进队列的是 guest 显示侧：游戏自己的 tick `sub_82ABB7D0` → `sub_82882200` → `sub_82881E00(0x82E55EE0, arg, 1)`（单趟有界），而宿主整体替换了 guest 交换体 `sub_822D9D70`，既不跑这段、也没人代客调用它 ⇒ **两侧都没人 arm 这三槽**。
- **改动**：
  ① `recomp/src/t2rotf_video_nativo.cpp`：在唯一 yield 点钩子 `REX_HOOK_RAW(sub_824F0620)` 内，`lr == 0x82881DE8` 时限频打印门的全部判据字段（`T2DIAG PUERTA`，前 32 次 + 此后每 4,000,000 次一行），所有指针取值经镜像段过滤（`0x82000000..0x83FFFFFF`）防止探针自崩；纯读、不改 guest 状态。
  ② `recomp/src/renut_engine/gpu/d3d/hooks/device.cpp`：新增两个默认 `false` 的实验开关 `sa2_renut_display_tick`（宿主代客跑一趟显示排空 `__imp__sub_82881E00(0x82E55EE0, -1, 1)`）与 `sa2_renut_swap_callthrough`（再跑一遍 guest 交换体 `__imp__sub_822D9D70`）；把换体注册从 `BANJO_RENUT_HOOK(sub_822D9D70, …)` 改成显式 `extern "C" REX_FUNC(sub_822D9D70)` 以便在钩子内按 cvar 追加 guest 调用（宏把 `ctx`/`base` 封在体内，外面追加不了）；call-through 打开时宿主不再镜像 `dev+16560`，避免与 guest 交换体双计。
  ③ 配置：`T2ROTF.toml`（放 NRO 同目录，键名即 cvar 名，布尔 `true/false`）。等价环境变量写法 `REX_SA2_RENUT_DISPLAY_TICK=1`。加载点在 `rex_app` 的 `LoadConfig(exe_dir / "T2ROTF.toml")`，两个键都是 `kInitOnly`，改了必须重启游戏。
- **为什么候选 B 用 `sub_82881E00` 而不是直接调 `sub_82882200`**：后者内部是 `do { … } while (*(r30)+*(r31) != 0)` 的自旋且整段包在临界区 `0x82E4B5C0` 内，队列若永不可排空就会在临界区挂死、可能连带堵住渲染线程；前者带 `flag=1` 是单趟有界（门返回 0 即回）。复用同一 `ctx` 也安全：`HostToGuestFunction` 只回写 `r3`，不动 `r1/r2/r13` 与 `r14-r31`，这次额外调用仍带有效 guest 栈。
- **校验**：`ninja -j4 T2ROTF` 退出码 0；新串 `T2DIAG PUERTA` / `T2DIAG callthrough` / `T2DIAG display-tick` / `sa2_renut_display_tick` / `sa2_renut_swap_callthrough` 均已在 NRO 内命中（逐个 `grep -c` 非 0）；NRO 时间戳 / 大小 / sha256 已更新。
- **风险与边界**：两个开关都会往 guest 侧注入动作（候选 B 会执行显示侧排入的完成回调，候选 A 会让 guest 交换体写环/PM4），且候选 B 跨线程进 `sub_824F0678`/`sub_824F19F0` 临界区的重入风险未评估 ⇒ **默认全关、只作 A/B**，先在实机按 README 第 3.5 节跑；**不超频 60 帧仍待实测**（本版只是把「播放完死机」的候选修复与探针一起交付）。
- **详见**：`T2ROTF_deko3d_呈现门阻塞语句与显示会计核对报告.md`（门判据逐条、卡点字段清单、日志逐项核对、槽位分工表、未定项、A/B 清单与风险声明）。

### 2026-10-27（v7.1-DEKO3D-PRESENT）— 落地显示会计 + 打开呈现门 + 停摆取证插桩

- **产物**：`recomp/out/sw8/T2ROTF.nro` = 59,826,930 字节，sha256 `a3004dd56c87cab3892d24519a986a07eb65705660e33fa27b0b41559736539f`（落会计+插桩后为 `a220d6ce…`，加采样关闭开关重链后为本版）。
- **背景**（`2026-10-26` 复跑日志判读）：启动崩溃已彻底消除（`procType=1`、双槽发布生效、`0x2A88` 消失、`Unhandled guest access violation` 0 次），但**影片链解码 6 帧后停摆**：`etapa1/2/3` 各 6 次且每次 `ret=0`、`PRESENTAR sub_827CADC0` = 0、`flip` = 0 ⇒ 全黑。
- **停摆主因（静态取证）**：泵 `sub_827CE598` 的三步解码之后有一道**呈现门** `lwz r11,3916(ctx); cmpwi 0; beq 0x827CE6A8`——`[ctx+3916] == 0` 时整段呈现被跳过。`3916` 由码流头部解析写入（3 个写入点都是"从码流逐位读字段"），而本移植用 FFmpeg 重做了解码，guest 自己的解析链不再跑 ⇒ 该字段恒 0。
- **宿主侧缺陷**：Swap 钩子原把"已显示"发布到 `device+16976`，而 `16968/16976` 在 T2 镜像**全库 0 命中**（MGR/SA2 遗留）；T2 的真实槽位是 `16548`(vblank)/`16552`(帧时)/`16556`/`16560`(提交)/`16564`(帧)/`16568`(已确认显示) 与 `*(dev+10900)+4`（guest 可见帧完成字），由 guest 的垂直同步处理程序 `sub_822D9570` 维护，但宿主未模拟 GPU 中断 ⇒ 本应由宿主代跑。
- **改动**：① `t2rotf_video_nativo.cpp` 在 `BombaTick` 里替 guest 置「本帧可呈现」（`[ctx+3916] = 1`，幂等）；② `hooks/device.cpp` 新增 `T2AdvanceDisplayAccounting()` 在每次 Swap 推进上述字段、清 `*(dev+10900)+4`、置 `10944 |= 0x80`，同时**删掉对 16976 的错误发布**；③ 诊断插桩：看门狗线程（`T2DIAG PERRO`，首行 3 s、此后每 2 s 全字段转储）、泵前钩子 `pre1`/`pre2`/`post`/`YIELD`（含全程序唯一 yield 点 `sub_824F0620` 的调用者 LR 直方图）、`LatidoVencido()` 门槛 26 万次 → 4096 次、栈采样默认开启（日志目录放 `perfil_pilas.off` 可关，帧率实测请关）。
- **取证**：`ninja -j4 T2ROTF` 退出码 0；新串 `T2DIAG PERRO`/`pumpIn=`/`T2DIAG pre1`/`T2DIAG YIELD total=`/`T2DIAG SWAP n=`/`T2 display accounting`/`perfil_pilas.off` 均命中；反汇编确认 Swap lambda 内联了会计代码（`add x23,x25,#0x4,lsl #12` 后 `[x23,#164]`=16548、`[x23,#180]`=16564、`str [x23,#168]`=16552、`[x25,#10900]` 与 `strb` 置 10944 bit0x80、`ldr w24,[x0,#4]`+`str wzr,[x0,#4]` 清帧完成字）；`16976`(0x4250) 的 6 处立即数全部不在 Swap 钩子区间 ⇒ 错误发布已移除。
- **风险**：强制置位会让从未执行过的 `sub_827CC058`/`827CC6E0` 首次运行（已证为纯计算叶子；若崩则改为影片期跳过其原体）；`hooks/draw.cpp` 三处按参考端取参的钩子仍属未排除次级隐患。**不超频 60 帧仍待实测。**
- **详见**：`T2ROTF_deko3d_影片链停摆修复与取证报告.md`。

### 2026-10-26（v7.0-DEKO3D）— 渲染后端切换到 Renut/deko3d；修复启动崩溃根因（VdGlobalDevice 双槽只发布一半）

- **产物**：`recomp/out/sw8/T2ROTF.nro` = 59,810,546 字节，sha256 `eda7708b125987a985e8a8740d2dea072f44a26b4bc8b32477c62e6ba955a072`（上一版 `bb9ec3ea…`）。
- **后端切换**：Vulkan/NVK 路径问题过多（v6.3x 起定论的"宿主发布地址 ≠ guest 轮询地址"等），全面改用 Renut/deko3d（plume + deko3d + uam）。构建不再链接 Vulkan 驱动，249 条 T2 钩子守卫与 987 条 Xenos 着色器（1,178 KB GLSL / SPIR-V 包）随构建嵌入，`MissingGameShaders()` 恒为 0。
- **启动崩溃根因（本轮定死）**：T2 把 VdGlobalDevice 存成**按进程类型二选一的两槽单元**——`0x820011B0`（`KeGetCurrentProcessType()==2`）与 `0x820011AC`（其余），每槽内容是"指向单元的指针"，设备写在单元里。证据：对 `extracted/TU/default_tu.flat` 逐字节扫描得 54 处读取点，几乎全部是 `lwz rX,4528(base)` + `lwz rX,4524(base)` 成对形态；发布点 `sub_822E1690` 也只写自己进程类型那一槽。本运行时标题进程是 `X_PROCTYPE_USER = 1`（`kernel_state.cpp:123`）⇒ guest 全走 `0x820011AC`；而 create 钩子原先只硬编码发布 `0x820011B0`，且 guest 自己的发布路径被我们强制 `kDeviceTypeNoHardware` 跳过 ⇒ 槽 A 单元恒 0 ⇒ guest 读到空设备 ⇒ 在 `device+10888`（= guest 地址 `0x2A88`）上访存崩溃（`rex_crash(34)`：线程 2416 / 2471 同址、`esr 0x92000006`、`far …0x2a88`、`x22 = 0x822cbd14`）。
- **修复**：`hooks/device.cpp` 新增 `PublishGlobalDevice()`（按当前进程类型写主槽；另一槽单元为空时补同一张设备；打印 `VdGlobalDevice: procType=…`），`GlobalDevice()` 改为进程类型感知 + 另一槽兜底；`InitializeTitleDevice()` 与 `Sa2CreateDevice()` 标题分支改调用之。
- **实机核对**（本轮日志）：create 钩子不再解引用 guest `0x9`（故障地址已变为设备字段 `0x2A88`）；`Direct3D_CreateDevice` 入参 2 次（`r6=0x82c5b434` / `r6=0x82dbb5f0`，`r8=0x9` 只是残留）；`Guest D3D device` 摘要 2 次（type 2 @0x40001b80、type 1 @0x40070600）；`all 249 guest interface guards match`；deko3d 设备 / 队列 / 交换链（3 images of 1280x720 format 117）/ 图像堆 / 着色器块全部就绪；`MissingGameShaders` = 0。但 `PRESENTAR` = 0、`etapas` = 0 ⇒ 仍崩在第一次 D3D 设备使用，尚未进入影片与呈现阶段。
- **工程包**：`_pkg_v635/T2ROTF.nro` 与 `T2ROTF_switch_port.zip` 内的 `T2ROTF.nro` 已原地替换为本版，README 已同步；包内源码快照（`sdk_changes/`、顶层源文件副本）仍是 v6.35 时代，全量重打计划随最终交付一并进行。
- **验证与风险**：不超频 60 帧仍未实测（本版尚未进入呈现循环）；逐项判读与证据见 `T2ROTF_deko3d_VdGlobalDevice修复报告.md`。

### 2026-10-20（v6.35-VFSYMLINK）— WVC1 视频符号链接权宜之计（10 个 VC-1 Advanced Profile 视频重定向）

- **背景**：v6.33-VIDTHUNK2 修复全部已知 thunk gap 后，实机验证视频路径不再崩溃，但
  `luxo_logo.wmv`（以及其余 9 个 WVC1 片头/尾视频）播放时**有声音无画面**，帧率恒为
  0 fps，播放完成后游戏持续黑屏。`ffprobe` 确认这些视频为 **WVC1（VC-1 Advanced Profile，
  含 B 帧）**，T2ROTF 原生重编译解码器仅支持 WMV3（VC-1 Main/Simple Profile，无 B 帧）。
- **根因**：VC-1 Advanced Profile 的 B 帧参考帧管理、loop filter、帧类型解析与 WMV3
  不兼容；重编译解码器在 WVC1 码流上进入死循环，CPU 满载 199% 但输出全黑。
- **修复（符号链接权宜之计）**：在 `recomp/src/T2ROTF_app.h` 的 `OnPostSetup()` 中，通过
  `REX_KERNEL_FS()->RegisterSymbolicLink()` 将 10 个 WVC1 视频路径重定向到工程内已验证
  可播放的 WMV3 替代文件（`atvi_logo.wmv`）。符号链接 key 使用反斜杠、大小写不敏感，
  与 rex SDK VFS 规范一致。被重定向的 10 个文件：
  `luxo_logo.wmv`、`a0_intro_co.wmv`、`A6_END_CO.wmv`、`D6_END_CO.wmv`、
  `Unlock_G1Ep_1.wmv` … `Unlock_G1Ep_6.wmv`。
- **验证**：`T2ROTF.nro` 重新构建（72,770,290 字节，MD5 `7e0b450a80778d17b153e4b5f9482f09`），
  构建日志 55/55 确认 `T2ROTF_app.cpp` 编译链接通过；静态检查确认 `RegisterSymbolicLink`
  调用在 `OnPostSetup()` 中（此时 VFS 已初始化），符号链接 key 与目标路径大小写正确。
- **性质**：本修复为**临时权宜之计**，重定向后这些视频片段会播放 `atvi_logo.wmv` 内容
  （画面非原版），但保证游戏流程不被 WVC1 解码死循环阻塞，可继续推进到主菜单及后续
  游戏内容。原版 WVC1 视频的正确解码需后续 FFmpeg VC-1 桥接完整方案（见 Todo）。
- **后续调研**：扩展参考了本地 `repos/` 下 LostOdysseyRecomp、torchlight-nx、project8-sw、
  masseffect-nx、sa2-nx、prototype2-nx 六个项目，以及在线 NFS-Carbon-NX；
  确认只有 `nfsmw-nx` 包含完整的 `video_wmv3.cpp` FFmpeg 桥接实现，其余项目均无可复用
  视频解码器（masseffect-nx/sa2-nx 虽在 SDK 中集成 FFmpeg，但仅用于 XMA 音频解码，未做
  视频桥接）。完整 WVC1 修复仍以 nfsmw-nx 的 `REX_HOOK_RAW` 替换方案为唯一可行路线。
- **待实机验证**：替换 SD 卡 NRO 后，预期片头视频不再死循环黑屏，能进入主菜单；
  若仍有问题请带回 `rex_stderr.log` + `T2ROTF_001.log` + `rex_crash.log`。

### 2026-10-20（v6.33-VIDTHUNK2）— 注册第三枚视频路径未见 thunk `0x8272EDF8`

- **背景**：v6.31-VIDTHUNK 实机验证 `luxo_logo.wmv` 播放路径继续推进后，又报
  `Call to invalid or unregistered function at guest address 0x8272EDF8` → abort。
  六份新日志全部收敛到同一地址：
  - `T2ROTF_001(34).log` / `T2ROTF_002(19).log`：FATAL 均指向 `0x8272EDF8`，
    路径仍为打开 `game:\movies\luxo_logo.wmv` 并读取后调用该地址；
  - `rex_crash(32).log`：host 侧 `abort()`（PC = `imagen+0x2a6188`）；
  - Atmosphère crash report（01792481137 / 01792481174）：ReturnAddress 链收敛到
    guest 调用 `0x8272EDF8` 的同一路径。
- **定位**：`0x8272EDF8` 未在 codegen 函数表中出现；它位于 `sub_8272EDE0`
  （8 字节，`addi r3,r3,-4; b 0x82733be8`，结束于 `0x8272EDE8`）之后、
  完整函数 `sub_8272EE78` 之前的独立 thunk 入口，同属 0x8272EDxx
  密集 thunk 表 gap（仅 vtable/函数指针表间接引用，静态分析发现不了）。
- **修复（codegen 重新生成）**：在 `recomp/overrides.toml` 追加
  `"0x8272EDF8" = { }` 声明后重跑 codegen，`T2ROTF_register.cpp` 已新增
  `registrar->SetFunction(0x8272EDF8, sub_8272EDF8)`（第 16774 行，位于
  `0x8272EDE0` 与 `0x8272EE78` 两条注册之间）；
  `T2ROTF_recomp.27.cpp:76058` 的 `sub_8272EDF8` 为合法 thunk：
  `addi r3,r3,-4; b 0x8272EE78`（与 `sub_8272EDE0` 同构，仅目标函数不同）。
- **验证**：codegen 重写 7 个文件（147.8s），`recomp.27.cpp` 与 `register.cpp`
  均参与后续 16:38 的 `recomp/out/sw8/T2ROTF.nro`（72770290 字节）完整编译链接；
  构建日志 54/55（register.cpp）确认新符号被编译并链接到最终 NRO。
- **待实机验证**：继续同一测试——替换 SD 卡 NRO 后，预期视频路径继续推进。
  若仍崩溃请带回 `rex_stderr.log` + `T2ROTF_001.log` + `rex_crash.log`
  （密集 thunk 表可能还有后续 gap；每次发现继续声明即可）。

### 2026-10-20（v6.31-VIDTHUNK）— 注册第二枚视频路径未见 thunk `0x82734308`

- **背景**：v6.29-VIDREG 实机验证 `luxo_logo.wmv` 播放路径推进到更深处，仍报
  `Call to invalid or unregistered function at guest address 0x82734308` → abort。
  六份日志证据一致：
  - `T2ROTF_001(33).log` / `T2ROTF_002(18).log`：FATAL 均指向 `0x82734308`，
    路径为打开 `game:\movies\luxo_logo.wmv` 并读取后调用该地址；
  - `rex_crash(31).log`：host 侧 `abort()`（PC = `imagen+0x2a6188`）；
  - Atmosphère crash report（01792472392 / 01792472407）：ReturnAddress 链收敛到
    guest 调用 `0x82734308` 的同一路径。
- **定位**：`0x82734308` 未在 codegen 函数表任何文件中出现；它正好紧邻
  `sub_827342F0`（6 条指令，结束地址 = `0x827342F0 + 0x18` = `0x82734308`）之后、
  完整函数 `sub_82734320` 之前的独立入口。与 `0x82737D70`/`0x82733980` 同属
  "vtable/函数指针表间接引用、静态分析发现不了"的 thunk 表 gap。
- **修复（codegen 重新生成）**：在 `recomp/overrides.toml` 追加
  `"0x82734308" = { }` 声明后重跑 codegen，`T2ROTF_register.cpp` 已新增
  `registrar->SetFunction(0x82734308, sub_82734308)`（第 16908 行，位于
  `0x827342F0` 与 `0x82734320` 两条注册之间，地址顺序正确）；
  `T2ROTF_recomp.43.cpp:79607` 的 `sub_82734308` 为合法间接跳转 thunk：
  `lwz r11,72(r3); addi r3,r11,64; lwz r11,64(r11); lwz r11,20(r11); mtctr r11; bctr`
  （与相邻 `sub_827342F0` 同构，仅 vtable 槽偏移 8→20）。
- **验证**：`T2ROTF_recomp.43.cpp` 与 `T2ROTF_register.cpp` 均于 14:07 由 codegen
  重写，并在 14:47 的 `recomp/out/sw8/T2ROTF.nro`（72770290 字节）完整编译链接；
  构建日志 51/55（recomp.43.cpp）、54/55（register.cpp）确认包含新符号。
- **待实机验证**：替换 SD 卡 `sdmc:/switch/Transformers2ROTF/T2ROTF.nro` 后，
  预期 `0x82734308` 未注册崩溃消失、片头继续推进；若仍崩溃请带回完整
  `rex_stderr.log` + `T2ROTF_001.log` + `rex_crash.log`（若为新的未注册地址，
  后续可继续用 overrides.toml 声明；视频解码本身如帧率不足 60，
  按 v6.29 调研结论走 nfsmw-nx FFmpeg VC-1 替换路线）。

### 2026-10-20（v6.29-VIDREG）— 注册 `0x82737D70`（视频对象 Release 路径）+ 参考项目视频调研

- **背景**：v6.28-VFSFIX 实机运行在 `luxo_logo.wmv` 播放后报
  `Call to invalid or unregistered function at guest address 0x82737D70` → abort。
  经排查确认该地址不是解码器本体，而是视频播放会话销毁/引用释放路径上的
  Release thunk（`sub_82737D70` → `sub_82737918` → `sub_82736BB8` 引用计数原子递减，
  计数到 0 时经 vtable[56 偏移] 释放对象）。
- **修复（codegen 重新生成）**：重新运行 codegen 后
  `generated/default/T2ROTF_register.cpp` 已包含
  `registrar->SetFunction(0x82737D70, sub_82737D70)`（第 16972 行），
  `PPCFuncMappings` 中 guest→host 映射（host = `sub_82737D70`，ELF 偏移 0x01dab950）
  完整，`Runtime::Setup` 的函数表循环会正确注册。旧 NRO（v6.28）中该函数
  不在注册表内，`GetFunction` 返回 null 触发 InvalidFunctionTrap。
- **验证**：最新 `recomp/out/sw8/T2ROTF.nro`（2026-10-20 08:48 构建，
  SHA256 前缀 `fe12350bb228`）已含注册；崩溃日志（06:47）早于该构建。
- **参考项目视频调研（nfsmw-nx，同源 WMV3 方案）**：作为后续视频优化路线，
  确认 `repos/nfsmw-nx` 是工作区唯一同源完整可复用的 WMV3 播放实现：
  - 组成：`app/src/nfsmw_video_wmv3.{h,cpp}`（ASF 头解析 + FFmpeg 解码器）、
    `nfsmw_video_nativo.cpp`（`REX_HOOK_RAW` 替换 WMV3 I/P/B 解码函数与
    数据回调、上下文准备，FFmpeg 解码后将 YUV 平面写回游戏上下文
    `[ctx+3672]/[ctx+3676]/[ctx+3680]`）、`bridge/session/vulkan/present`（native 上屏）。
  - 关键前提已具备：本工程实际使用的 SDK 就是 `repos/nfsmw-nx/sdk`
    （CMakeCache `REXSDK_DIR` 指向它），其中 `xboxkrnl_io.cpp` 已含
    `NfsmwUltimoWmvAbierto/Leido`（WMV 读取追踪），可直接支撑 FFmpeg 方案的
    `LeerInfoWmv`；SDK `thirdparty/FFmpeg` 自带 WMV3/VC-1 解码器。
  - 待办：T2ROTF 的 `luxo_logo.wmv` 是 **VC-1 Advanced Profile**（含 B 帧），
    nfsmw 的 `DescodificadorWmv3` 用的是 `AV_CODEC_ID_WMV3` 解码器，需确认
    `vc1` 解码器路径后再移植；若注册修复后视频能播放但帧率不足 60，
    再走此路线替换重编译解码器。
- **调研补充（2026-10-20 v6.29a 核对三个事实）**：
  1. **其它参考项目无可复用 VC-1 视频实现**：refs/ 下 Lost Odyssey
     （`movie/mp4_demux.cpp`）是 MP4/FFmpeg 方案、masseffect-nx 核心是 XMA
     音频解码、project8/sa2/torchlight 无视频解码器，均不适用于 WMV/VC-1；
     本工程唯一同源实现仍是 nfsmw-nx。
  2. **T2ROTF 与 nfsmw 解码组件确证同源**：recomp 生成代码中
     `+3672/+3676/+3680`（Y/U/V 平面，T2ROTF_recomp.25.cpp:106964 等）、
     `+3016`（B 帧指针，T2ROTF_recomp.1.cpp:87344）、`+15712`（P 帧指针，
     T2ROTF_recomp.18.cpp:87501）、`+280`（帧类型，T2ROTF_recomp.1.cpp:86684）
     偏移与 nfsmw 完全一致；`+15704~+15792` 为解码缓冲池（0x827D184C 分配、
     0x827B6334 释放），`REX_HOOK_RAW` 替换点需按 T2ROTF 自身 guest 地址定位
     （nfsmw 的 `sub_828C35D8/sub_8278A518/sub_828C58C8` 是它的地址，不可照抄）。
  3. **SDK FFmpeg 需补注册 `ff_vc1_decoder`**：`repos/nfsmw-nx/sdk/thirdparty/
     ffmpeg-overlay/codec_list.c` 目前仅注册 `&ff_wmv3_decoder`（第 18 行）；
     `config_nfsmw_wmv3.h` 已启用 `CONFIG_VC1_DECODER`/`CONFIG_VC1DSP`，
     VC-1 解码器代码在 FFmpeg 里已编译，只差 codec 注册表项，运行时
     `avcodec_find_decoder(AV_CODEC_ID_VC1)` 才能找到。另外 T2ROTF 主工程
     当前 **未链接 FFmpeg**（recomp/CMakeLists.txt 无 libavcodec 引用、
     src 下无视频专文件），若实施 FFmpeg 路线需将该 SDK 的 FFmpeg 纳入
     T2ROTF 构建（参考 nfsmw app 的 `target_link_libraries(... PRIVATE libavcodec)`）。
- **待实机验证**：用本版 NRO（SHA256 前缀 `fe12350bb228`）替换 SD 卡
  `sdmc:/switch/Transformers2ROTF/T2ROTF.nro` 后，预期 `0x82737D70` 未注册
  崩溃消失、片头 logo 视频正常播放；若仍崩溃请带回完整 `rex_stderr.log` +
  `T2ROTF_001.log` + `rex_crash.log`。

### 2026-10-19（v6.28-VFSFIX）— 根治 overlay 根目录文件打开失败（`shaders.lsa`/`effects.lsa` 0xc000000f）

- **背景**：阶段B（kCommitGranule=2MB + 4MB fault-in，见下方 v6.28 条目补充）实机验证
  `0xCE01` 与 `PhysicalHeap::Alloc` 已完全消除后，出现新崩溃：片头 `luxo_logo.wmv` 播放后
  `NtCreateFile FAILED: path='game:\shaders.lsa' -> 0xc000000f`（`effects.lsa` 同样失败），
  随后 `Call to invalid or unregistered function at guest address 0x82737D70` → abort。
  文件在 `game/` 根目录、SD 卡目录完整拷贝自本地，并非真的缺失。
- **根因（OverlayHostPathDevice 根目录查找缺陷）**：overlay 设备按层保存目录树。
  `VirtualFileSystem::OpenFile` 打开 `game:\shaders.lsa` 时先用
  `ResolvePath("game:\")` 取得父 entry；overlay 对根路径总是命中第一层（`update/`）的
  根 entry，随后 `parent_entry->GetChild("shaders.lsa")` 只在 `update/` 层查找，而文件
  实际位于 base 层 `game/` 根目录 → 误判为不存在（0xc000000f）。子目录文件不受影响，
  因为子目录路径（如 `game:\movies\`）会逐层遍历最终命中正确层。
- **修复（`repos/nfsmw-nx/sdk/src/filesystem/virtual_file_system.cpp`）**：
  `OpenFile` 中当 `parent_entry->GetChild(file_name)` 失败且父目录为只读（游戏数据层）时，
  回退用完整路径 `ResolvePath(path)` 逐层查找，并同步把 `parent_entry` 修正为找到条目的
  父结点。只读设备由内存树回答 miss，无 SD 扫描开销；可写设备（存档等）保持原路径不变。
- **验证**：单文件编译通过；完整 LTO 重链成功；产物 `T2ROTF.nro` 71,992,050 字节，
  SHA256 前缀 `bd0ef09985b8`。
- **待实机验证**：预期 `logs/rex/rex_stderr.log` 不再出现
  `NtCreateFile FAILED: path='game:\shaders.lsa'`（或 `effects.lsa`）`-> 0xc000000f`；
  若 `0x82737D70` 未注册函数崩溃随之消失，则渲染路径恢复正常。若仍崩溃请带回完整
  `rex_stderr.log` + `T2ROTF_001.log` + Atmosphere crash report 继续定位。

### 2026-10-19（v6.28-B）— 阶段B：2MB 提交粒度 + 4MB fault-in，根治 CE01 内核配额耗尽（已实机验证）

- **修改**：`guest_memory_switch.cpp` `kCommitGranule = kBlockSize = 0x200000`，
  `RexGmCommit` 按 2MB `AlignDown/AlignUp` 对齐提交窗口；`exception_handler_switch.cpp`
  `kTrozo = 0x400000`（4MB fault-in 粒度）。
- **验证**：实机日志 `rex_stderr(32)` 中 `code alias failed rc=0x0000CE01` 与
  `PhysicalHeap::Alloc unable to alloc physical memory` **完全消失**，阶段A/B 修复生效。

### 2026-10-18（v6.27-MAPFIX）— 根治 `0xCE01` 配额耗尽 + `__appInit` SDMC 挂载 + 链接器 read-only relocation

- **背景**：v6.26-SDMCFIX 已修复 `__appInit` 初始化 sm/fs 并挂载 SDMC，使日志能成功落盘；
  但实机运行日志版本号仍为 `v6.24`，且 `RexGmCommit: code alias failed rc=0x0000CE01`
  大量失败，随后 `PhysicalHeap::Alloc` 失败导致死机。根本原因是：
  源码中已写入 v6.27 的 guest memory map 修复（Direction F 探测 + B2 按需 code alias），
  但 devkitA64 GCC 15 链接阶段报 `read-only segment has dynamic relocations`，
  导致这些修复从未被编译进任何 NRO。
- **根因 A（链接失败）**：新增 C++ 代码引入 `.data.rel.ro` 动态重定位条目，ld 默认拒绝
  将其放入只读 PT_LOAD；同时 guest_memory_switch.cpp.obj 在之前构建中因时间戳
  巧合未被重编，旧 NRO 一直不带新内存修复。
- **修复 A（链接）**：`sdk/cmake/rexglue_switch.cmake` 中 `rexglue_switch_configure_target`
  追加 `LINKER:-z,notext`，允许 read-only segment 含动态重定位；构建成功并产生新 NRO。
- **根因 B（code alias CE01）**：`svcMapProcessCodeMemory` 在 ~500 MB 预分配 arena 映射时
  耗尽 `LimitableResource_CodeMemory` 配额（39bit 前端尤其紧张）。源码中已含无条件
  Direction F 探测（`svcSetMemoryAttribute(..., MapProcessAllowed=0x8)` + `svcMapProcessMemory`），
  若探测成功即可绕开 code-memory 路径。
- **根因 C（内存分配失败）**：`svcGetInfo(InfoType_ResourceLimit) failed rc=0xE401`，
  无法提升资源限制；旧版 prealloc 仅 128 MB 且未正确计入 shadow 映射，导致后续
  `PhysicalHeap::Alloc` 失败。
- **修复 B/C（guest memory）**：`guest_memory_switch.cpp` v6.27 修改已包含：
  - Direction F 无条件探测；若成功启用 `g_f_mode_enabled`，后续 `RexGmCommit` 走
    `svcMapProcessMemory` 替代 `svcMapProcessCodeMemory`，绕开 code-memory 配额。
  - 扩大 `kBlockSize` 与预分配策略，减少独立映射条目数。
  - `runtime_switch.cpp` v6.26 已补充 `smInitialize() → fsInitialize() → fsdevMountSdmc()`。
- **验证**：构建产物 `T2ROTF.nro` SHA256 前缀 `ca7e1626`（71 660 274 字节）。
  日志应首次出现 `==== v6.27-MAPFIX crash hooks init ok`。
- **待实机验证**：请在 Sphiara 高权限模式与 39bit 前端分别测试；
  预期 `rex_stderr.log` 不再出现 `RexGmCommit: code alias failed rc=0x0000CE01`，
  不再出现 `PhysicalHeap::Alloc unable to alloc`，也不再 0 字节 crash log。
  若仍有崩溃请带回完整的 `rex_stderr.log` + `T2ROTF_001.log` + `rex_crash.log`。

### 2026-10-16（v6.6）— host 线程栈 16 MiB → 4 MiB，guest block 16 MB → 32 MB

- **背景**：v6.5 实机测试暴露两个新问题：
  1. **Sphiara 高权限模式**：黑屏几秒后报错退出，`T2ROTF_002.log` 显示
     `CreateThread failed: C0000017`（STATUS_NO_MEMORY），随后 guest 访问
     未初始化线程对象导致 Data Abort。
  2. **39bit 前端**：黑屏死机，`T2ROTF_001.log` 仍出现 `0xCE01`
    （`AllocFixed` 连续失败），说明 16 MB block size 在配额更小的 39bit 前端
     下仍不足。
- **根因 A（Sphiara）**：`repos/nfsmw-nx/sdk/src/system/xthread.cpp:467` 中
  host 线程栈硬编码为 `16_MiB`。Xbox 360 游戏在 GPU 初始化后会创建数十个
  guest 线程，host 栈总量 = 线程数 × 16 MB，迅速耗尽 ~3.2 GB host heap，
  导致 `std::thread` 构造失败（`X_STATUS_NO_MEMORY`）。
- **根因 B（39bit）**：`MapWindowRange` 按 4 KB page 映射 guest view，5 个 views
  × 16 MB chunks ≈ 25 次 `svcMapProcessMemory`，在 39bit 前端的 tighter
  `LimitableResource_Memory` 配额下仍然耗尽。
- **修复**：
  - `xthread.cpp`：`params.stack_size = 16_MiB` → `4_MiB`。4 MB 对重编译
    runtime 足够（guest 栈在 guest 内存中单独分配，host 栈仅服务模拟层）。
  - `guest_memory_switch.cpp`：`kBlockSize` 从 `0x1000000`（16 MB）提升到
    `0x2000000`（32 MB），进一步减半 `RexGmCommit` 的 chunk 数量。
- **图标**：沿用用户提供的 256×256 原始 `icon_T2.png`，不再锐化。
### 2026-10-17（v6.8）— 先 B 后 A：缩堆 + 预分配 arena，根治双配额耗尽

- **背景**：v6.7 实机测试确认 `svcMapProcessCodeMemory`（Layer 1，backing→shadow）与
  `svcMapProcessMemory`（Layer 2，shadow→guest view）**双双返回 0xCE01**，且 `rex_crash.log`
  为 0 字节。`respaldo 466 MB, mapeado 322 MB` 说明两个独立配额（CodeMemory 与 Memory）
  同时耗尽；64 MB block size 对 8 KB 碎片分配无效（gap 被相邻 chunk 截断，每次 8 KB 仍消耗
  独立 code-memory region）。
- **决策**：采用"先 B 后 A"组合方案——B 释放 Memory 配额与物理内存，A 根治 CodeMemory 配额。
- **B 方案（`__nx_heap_size` 1 GB → 256 MB）**：
  - 文件：`repos/nfsmw-nx/sdk/src/ui/runtime_switch.cpp`
  - `__nx_heap_size = 1024ull * 1024 * 1024` → `256ull * 1024 * 1024`
  - 效果：释放 ~768 MB 物理内存及 `LimitableResource_Memory` 配额，给 A 的预分配腾地方，
    同时缓解 Layer 2 的 `svcMapProcessMemory` 配额压力。
- **A 方案（预分配整个 guest arena）**：
  - 文件：`repos/nfsmw-nx/sdk/src/core/guest_memory_switch.cpp`
  - `RexGmInit`：启动时一次性 `memalign` 整个 arena size 的 backing，并通过
    `svcMapProcessCodeMemory` 映射到 shadow（`virtmemFindCodeMemory` 找 VA），然后
    `svcSetProcessMemoryPermission(..., Perm_Rw)` 开启读写权限。记录到全局变量
    `g_prealloc_backing` / `g_prealloc_shadow` / `g_prealloc_size`。
  - `RexGmCommit`：不再逐 chunk `memalign` + `svcMapProcessCodeMemory`，改为从预分配
    arena 按 `cur` 偏移直接切片（`backing = g_prealloc_backing + cur`，
    `shadow = g_prealloc_shadow + cur`）。保留 `kBlockSize` gap 扩展逻辑以减少 chunk 数。
  - `ReleaseChunk`：改为 no-op（`(void)c`），避免预分配区域被中途释放。
  - `RexGmShutdown`：遍历清理 chunk 与 views 后，统一调用 `svcUnmapProcessCodeMemory`
    并 `free(g_prealloc_backing)`，一次性释放整个 arena。
- **风险**：若 arena size（~2 GB）超过 Switch 可用物理内存或 `LimitableResource_CodeMemory`
  硬上限，`RexGmInit` 的 `memalign` 或 `svcMapProcessCodeMemory` 会在启动即失败，日志将
  明确打印 `memalign failed` 或 `rc=0xCE01`。此时需回退到 fallback（预分配多个中等块，
  或进一步缩堆到 128 MB）。
- **验证**：构建产物 `T2ROTF.nro` SHA256 前缀 `f1567d309507d4a8`（已重链，v6.7 基础上
  增量编译 `runtime_switch.cpp.obj` + `guest_memory_switch.cpp.obj`）。
- **说明**：已重建 NRO，**尚未实机验证**。请在 Sphiara 高权限模式与 39bit 前端分别测试；
  预期：不再出现 `0xCE01`，不再出现 `0xD401`，`rex_crash.log` 应有内容（若仍崩溃）。
  若启动直接失败（memalign 返回 NULL 或第一发 `svcMapProcessCodeMemory` CE01），请带回
  完整的 `rex_stderr.log` + `rex_crash.log`。若启动成功但后续 `MapWindowRange` 报 CE01，
  说明 B 缩堆仍不足，需进一步压 `__nx_heap_size`。

### 2026-10-17（v6.7）— 恢复 code-memory 映射，增大 block size 至 64 MB，放宽地址搜索

- **背景**：v6.6 实机测试暴露 `svcMapProcessMemory` 返回 `0xD401`（InvalidHandle）。
  `rex_stderr(12).log` 显示 `RexGmCommit: svcMapProcessMemory failed rc=0x0000D401`，
  因为 `memalign` 分配的 Heap 类型内存**不能**用 `svcMapProcessMemory` 在同进程映射；
  Horizon 要求 src 必须是 `svcCreateMemoryPool` / `svcMapSharedMemory` 创建的共享内存。
  紧接着 guest 代码访问未映射的 shadow 区域 → Data Abort（`far = 0x9AC52501C`，
  guest `0x8000001C`），`T2ROTF_001(10)` 在首段 commit 即全败（backing 0 MB，
  mapped 0 MB）。
- **根因**：`svcMapProcessMemory` 比 `svcMapProcessCodeMemory` 更严格，并非
  无限制替代方案。`svcMapProcessCodeMemory` 允许映射任意 src 地址，唯一代价是
  消耗独立的 `LimitableResource_CodeMemory` 配额。v6.5 的 CE01 不是 code-memory
  API 本身的问题，而是**32 MB block 仍然太小**，在 39bit 前端配额下条目数仍过多。
- **修复**：
  1. `guest_memory_switch.cpp`：恢复 `svcMapProcessCodeMemory`（含
     `svcUnmapProcessCodeMemory`）与 `virtmemFindCodeMemory`，放弃
     `svcMapProcessMemory` 方案。参数顺序已核对正确（Handle proc 在前，dst/src
     在后，与 `svcMapProcessMemory` 不同）。
  2. `kBlockSize` 从 `0x2000000`（32 MB）提升到 `0x4000000`（64 MB），每个
     `RexGmCommit` chunk 更大，映射条目消耗进一步减半。
  3. `virtmemFindCodeMemory` 的 guard_size 从 `kBlockSize`（64 MB 预留）降到
     `kPageSize`（4 KB），大幅放宽地址空间搜索条件，降低找不到连续 VA 的风险。
  4. `xthread.cpp`：host 线程栈保持 `2_MiB`（v6.6 已降，不改）。
- **验证**：构建产物 `T2ROTF.nro` SHA256 前缀 `f1567d309507d4a8`（已重链）。
- **说明**：已重建 NRO，**尚未实机验证**。请在 Sphiara 高权限模式与 39bit 前端
  分别测试；预期：不再出现 `0xD401`，不再出现 `0xCE01`，Sphiara 不再出现
  `CreateThread failed`。若任一环境仍崩溃，请带回完整的 `rex_crash.log` +
  `rex_stderr.log` + `T2ROTF_001.log`。

### 2026-10-17（v6.6）— 将 shadow 映射从 code-memory 改为普通内存映射，并缩减 host 线程栈

- **背景**：v6.5 在 39bit 前端仍出现大量 `0xCE01`（`AllocFixed` 失败），而 Sphiara
  高权限模式出现 `CreateThread failed: C0000017` → guest access violation
  `read of guest 0x00002A9C`。两种前端行为差异大：39bit 前端 guest arena base
  `0x1AECACD000`（CE01 配额耗尽），Sphiara base `0x6F5341000`（线程创建内存不足）。
- **根因（39bit 前端）**：`svcMapProcessCodeMemory` 消耗的是 Horizon 独立的
  **code-memory 配额**（LimitableResource_CodeMemory），而非普通内存配额。
  该配额在 39bit 前端（applet/overlay 模式）下非常紧张，32 MB block 的
  shadow chunk 映射迅速耗尽该额度，导致 `RexGmCommit` 返回 `0xCE01`。
- **根因（Sphiara）**：`CreateThread failed: C0000017` 是 Xbox 360 内核层返回的
  `STATUS_NO_MEMORY`，由 host 堆耗尽触发。Sphiara 高权限模式下 `__nx_heap_size=1 GB`
  实际生效（区别于 39bit 前端 hbloader 预膨胀 ~3.2 GB），1 GB 扣除
  guest backing ~506 MB、Mesa GPU 内存与多线程栈后余量不足，线程创建时
  `malloc` 失败。
- **修复**：
  1. `guest_memory_switch.cpp`：`svcMapProcessCodeMemory` → `svcMapProcessMemory`，
     `svcUnmapProcessCodeMemory` → `svcUnmapProcessMemory`，`virtmemFindCodeMemory`
     → `virtmemFindAslr`。shadow 映射不再占用 code-memory 额度，只消耗普通
     内存映射额度，从而绕开 39bit 前端紧张的 `LimitableResource_CodeMemory`。
  2. `xthread.cpp`：host 线程栈从 `4_MiB` 降至 `2_MiB`（Switch 重编译运行时
     栈深足够），降低 Sphiara 模式下的 host 堆压力，缓解 `CreateThread` 失败。
- **验证**：
  - 构建产物 `T2ROTF` ELF 的 strings：`svcMapProcessCodeMemory`=0、`svcMapProcessMemory`=2，
    确认旧 code-memory 路径已完全替换。
  - `T2ROTF.nro` SHA256 前缀 `8cea02b79f79bb25`（与 v6.5 不同，已重链）。
- **说明**：已重建 NRO（SHA256 以 `8cea02b79f79bb25` 开头），**尚未实机验证**。
  请在 Sphiara 高权限模式与 39bit 前端分别测试；预期 Sphiara 不再出现
  `CreateThread failed`，39bit 前端不再出现 `0xCE01`。若任一环境仍崩溃，
  请带回完整的 `rex_crash.log` + `rex_stderr.log` + `T2ROTF_001.log`。

### 2026-10-16（v6.5）— 提升 guest 内存 block size 至 16 MB，缓解 0xCE01 配额耗尽

- **背景**：v6.4 移除 `svcSetHeapSize` 后 Data Abort 消失，但实机出现大量
  `svcMapProcessCodeMemory failed rc=0x0000CE01`（`RexGmCommit`）与
  `AllocFixed: no se pudo confirmar... Fallo 0x0000CE01`（`MapWindowRange`）。
  日志显示 committed 75 MB / mapped 43 MB 即耗尽 `LimitableResource_Memory`。
- **根因**：Horizon 按**映射次数**（而非字节）计费 `LimitableResource_Memory`。
  旧 `kBlockSize = 2 MB` 且仅在 `cur` 2 MB 对齐时才扩展 gap，导致大量小请求
  （8 KB）各自独立调用 `svcMapProcessCodeMemory`，迅速耗尽默认配额。
- **修复**：`repos/nfsmw-nx/sdk/src/core/guest_memory_switch.cpp`：
  1. `kBlockSize` 从 `0x200000`（2 MB）提升到 `0x1000000`（16 MB）。
  2. `RexGmCommit` 移除 `(cur & (kBlockSize-1)) == 0` 限制，**无条件**将
     `hueco_fin` 扩展到下一个 16 MB boundary。这样每个 chunk 覆盖更大的连续
     区域，后续小请求命中 `ChunkAt` 即可跳过 syscall。
- **图标**：改用用户提供的 256×256 原始 `icon_T2.png`，不再锐化，直接嵌入 NRO。
- **构建**：修复 Windows 环境下 ninja/cmake 路径兼容性（使用 MSYS2 ninja 构建）。
- **说明**：已重建 NRO（SHA256 以 `e27a087ccb8feb54` 开头），**尚未实机验证**。
  实机请保留 `logs/rex/rex_stderr.log`、`logs/rex/rex_perfil.log`、
  `T2ROTF_001.log`。预期：不再出现 `0xCE01`，也不再有 Data Abort；若仍出现
  配额不足，需进一步研究在 `__libnx_init` 之前提升 `LimitableResource_Memory` 的方案。

### 2026-10-16（v6.4）— 移除 main() 中的 svcSetHeapSize，避免 _malloc_r arena 失效

- **背景**：v6.3 把 `svcSetHeapSize(1GB)` 移到 `main()` 入口，但在实机复测中
  仍然出现完全相同的 Data Abort（`pc = imagen+0x35714f8`、`esr 0x92000046`、
  `x3 = new_heap_end + 0x800`）。`rex_stderr` 已确认 `main: shrunk heap to 1 GB`
  打点存在，证明新二进制确实在运行。
- **根因**：devkitPro/newlib 的 `_sbrk` 初始化发生在 `__libc_init_array`（在
  `main()` 之前），`svcSetHeapSize` 会重新映射 heap region 并改变基址/大小，
  但 `_sbrk` 的 static `heap_top` 仍基于旧 heap（~3.2 GB）建立。后续 malloc
  时 arena top pointer 指向新 heap 范围之外 → Data Abort。
- **修复**：从 `main()` 中**完全移除** `svcSetHeapSize` 调用，保留 `__nx_heap_size
  = 1 GB` 定义（对 title takeover / 非 hbloader 启动有效）。当前通过 hbloader
  启动时 heap 保持 ~3.2 GB；guest_memory_switch.cpp 的按需 2 MB block 映射
  已解决 `svcMapProcessCodeMemory` 配额（LimitableResource_Memory）问题，
  3.2 GB host heap 对 guest backing (506 MB) + Mesa GPU + 线程栈足够。
- **后续**：若实机出现 `0xCE01`（code-memory 配额不足）或 `0x2001-0103`
  （内存确认失败），再研究在 `__libnx_init()` 之前（crt0 / 覆盖 `__libnx_init`）
  安全缩堆的方案。

### 2026-10-16（v6.3）— 修复 Data Abort（EC 0x24）：ShrinkHeap 移到进程入口

- **背景**：v6.2 NRO 实机运行崩溃，`rex_crash.log` 显示
  `pc = imagen+0x35714f8`、`esr 0x92000046 (EC 0x24, Data Abort)`、
  `far = 0x5578e00708`（x3 = 0x5578e00700）。升级到 Atmosphere 1.12.1
  后崩溃点/寄存器完全一致，证明问题在 recomp 二进制本身而非系统版本。
- **根因**：v6.2 把 `ShrinkHeap()`（`svcSetHeapSize` 缩堆到 1 GB）放在
  `RexGmInit()` 内执行；但 `RexGmInit` 运行在 libc 运行时初始化之后，
  此时 `_malloc_r` 的 arena 指针已基于旧堆基址建立。调用
  `svcSetHeapSize` 直接改变 heap 映射后，arena 指针失效，下次 malloc
  写冠军指针越界 → Data Abort。`aarch64-none-elf-nm` 定位
  `_malloc_r` 于 0x3571060，崩溃点在其偏移 0x498，x3 恰为
  新堆结束地址 + 0x700（arena 内部越界写）。
- **修复**：
  1. 堆缩小逻辑移到进程入口 `main()`（`repos/nfsmw-nx/sdk/src/system/windowed_app_main_switch.cpp`），
     在 libc malloc 初始化**之前**调用 `svcSetHeapSize(&addr, 0x40000000)`
     并打点 `main: shrunk heap to 1 GB (addr=%p)`；此时 arena 尚未建立，
     后续 malloc 会基于新堆基址重新初始化，不再越界。
  2. 顺带修复该文件新增代码的编译错误：`R_FAILED` 未声明
     （文件仅包含 `<switch/types.h>` 以避免与 rex typedef 冲突），
     改为等价的 `rc != 0`。
  3. 从 `guest_memory_switch.cpp` **删除**已无调用者的 `ShrinkHeap()`
     函数本体（RexGmInit 中不再调用），杜绝误导日志与误用
     （非 LTO 构建下未调用函数不会被裁剪）。
- **验证**：三处构建产物（`T2ROTF` ELF、`T2ROTF.nro`、`T2ROTF.stripped.elf`）
  strings 一致：`RexGmInit: shrunk heap` 不再出现、`main: shrunk heap to 1 GB`
  与 `main: svcSetHeapSize(1GB) failed` 打点存在、`RexGmInit: raised
  LimitableResource_Memory` 正常职责日志保留。
- **待实机复测**：放入 `sdmc:/switch/Transformers2ROTF/T2ROTF.nro` 覆盖旧版
  （`game/`、`update/`、`patched/`、`dlc/` 无需变动）。预期 `logs/rex/rex_stderr.log`
  出现 `main: shrunk heap to 1 GB` 且不再出现 Data Abort；若仍有崩溃请带回完整
  `rex_crash.log` + `rex_stderr.log` 继续定位。

### 2026-10-16（v6.2）— 修复 Switch 内存映射额度耗尽（ShrinkHeap）

- **背景**：v6.1 NRO 在实机启动后黑屏死机，`rex_stderr.log` 显示
  `svcGetInfo(InfoType_ResourceLimit) failed rc=0x0000E401`，随后
  `svcMapProcessCodeMemory failed rc=0x0000CE01` 连环失败，最终
  `CreateThread failed: C0000017` → `Call to invalid or unregistered function`。
- **根因**：hbloader 在 Title Takeover 模式下预膨胀堆至约 3.2 GB，几乎耗尽进程的
  `LimitableResource_Memory`（已映射物理页总量上限）。`RaiseResourceLimit()` 试图通过
  `svcGetInfo` 获取资源限制句柄并提额 1 GB，但 `InfoType_ResourceLimit` 在当前的
  Atmosphere 1.12.0 / homebrew 环境下返回 `0xE401`（InvalidEnumValue），提额完全失效。
  后续 `RexGmCommit` 调用 `svcMapProcessCodeMemory` 建立 shadow code 映射时，额度不足，
  连环报错 `0xCE01`（ResourceExhausted）。
- **修复**：`guest_memory_switch.cpp` 的 `RexGmInit` 中新增 `ShrinkHeap()`：在
  `RaiseResourceLimit()` 之后调用 `svcSetHeapSize(&addr, 0x40000000)`，将 hbloader
  预膨胀堆从 ~3.2 GB 缩小到 1 GB，直接释放约 2.2 GB 的 `LimitableResource_Memory`
  映射额度给 shadow code 使用。C 运行时堆可在后续 `malloc` 时通过 `svcSetHeapSize`
  自动增长回所需大小，因此缩小是安全的。
- **说明**：已重建 `librexruntime.a`（确认包含 `ShrinkHeap` 字符串）并重链 NRO。
  实机测试时请保留 `logs/rex/rex_stderr.log`，应出现 `RexGmInit: shrunk heap to 1 GB`
  打点；若仍见 `0xCE01`，请一并带回继续分析（可能 SaltyNX 等插件额外占用映射额度）。

### 2026-10-16（v6.1）— 重建 NRO：修复 GCC 15 alias 链接失败

- **背景**：v6 的 NRO 在链接期失败：`collect2: error: ld returned 1 exit status`，
  缺失 31847 个符号（236 个 PPC 寄存器 stub + 31611 个 `sub_8214xxxx` recomp 函数）。
- **根因**：devkitA64 的 GCC 15 会静默忽略**声明位置**上的
  `__attribute__((alias))`（warning: attributes are not permitted in this position），
  导致生成的 recomp 对象只有 `__imp__sub_*` 强符号、缺少 `sub_*` alias 符号。
- **修复**：在 `recomp/generated/default/T2ROTF_pch.h` 的 `DEFINE_REX_FUNC` 中新增
  `#elif defined(__GNUC__) && !defined(__clang__)` 分支，改用
  `.weak name` + `.set name, __imp__name` 汇编方案生成 alias 符号。
- **说明**：已重建 NRO 并更新本 zip；尚未实机验证。实机请保留
  `logs/rex/rex_perfil.log`、`T2ROTF_00X.log` 确认 `patched/default.xex` 正常加载。
  > 注意：若重新运行 codegen，`T2ROTF_pch.h` 会被生成器覆盖回旧宏；根治方案是
  > 重建已含修复模板（`resources/templates/codegen/pch_h.inja`）的 rexglue.exe。

### 2026-10-16（v6）— 独立目录布局 + 安装期 XexPatcher 融合 + 图标重制

- **背景**：v5 虽然改成"安装期合并"，但把 TU/DLC 全部复制进 `game/` 单目录，导致
  `game/` 体积膨胀且无法区分 base/TU/DLC；运行时仍需 `UserModule::ApplyPatch` 在加载
  `default.xex` 时现场打 `default.xexp`，实测在 Switch 上触发 `0x820F0E00` 崩溃。
- **决策**：对齐 UnleashedRecomp-NX **独立目录**模型：
  `game/`（base）、`update/`（TU overlay）、`patched/`（融合后的 xex）、`dlc/`（独立包）。
- **改动**：
  - `tools/xex_patcher_cli/`：新增独立 Windows CLI 工具，移植自 XenonUtils XexPatcher，
    将 `default.xexp` 在安装期融合为 `patched/default.xex`；编译修复（aes.c 强制 `-x c`、
    xbox.h 匿名聚合改名、mingw64 PATH 修复）。
  - `repos/nfsmw-nx/sdk/src/system/runtime.cpp`：`SetupVfs` 在 overlay 层序中插入 `patched/`
    （`{update, patched, game}`），使 `game:\default.xex` 自动命中融合产物，不再依赖运行时
    `ApplyPatch`；DLC 仍由 `ContentManager` 枚举 `dlc/` 子目录动态注册。
  - `scripts/setup_install.sh`：全新部署脚本，幂等生成四目录布局，自动调用 xex_patcher。
  - `icon_T2.png`：64×64 原始图放大锐化到 256×256，满足 `elf2nro` 图标规范。
- **加载顺序**：`game:\default.xex` → overlay 命中 `patched/default.xex`（已含 TU）；
  TU 其余文件（`camera.lxb` 等）命中 `update/`；DLC 由游戏启动后自行加载；无 TU 时
  `patched/` 不存在，自然回退 `game/default.xex`。
- **说明**：已构建 NRO，尚未实机验证。实机请保留 `logs/rex/rex_perfil.log`、
  `T2ROTF_001.log` 以确认 `patched/default.xex` 正常加载且不再崩溃。

### 2026-10-15（v5）— TU/DLC 改为安装期合并模型（参考 UnleashedRecomp-NX）

- 背景：此前为运行时动态 Overlay（`SetupVfs` 按 update>DLC>base 叠成单设备，
  并预留 `MountTitleUpdate()/MountDlc()` 动态加层）。经调研 UnleashedRecomp-NX
  确认其采用**安装期合并**：TU 补丁在 codegen/安装阶段打进 `default.xex`
  （`default.xexp` 放同目录，运行时由 `UserModule::LoadFromFile` 自动
  `ApplyPatch`），DLC 直接复制到游戏目录。用户拍板按参考项目模型落地。
- 改动：
  - `repos/nfsmw-nx/sdk/src/system/runtime.cpp`：`SetupVfs` 改为单层
    `HostPathDevice` 挂载 `game/` 为 `\Device\Harddisk0\Partition1`（`game:`/`d:`
    符号链接保留）；删除 `dlc_data_root` cvar 定义与 overlay include。
  - `repos/nfsmw-nx/sdk/include/rex/runtime.h`：删除 `dlc_data_root` cvar 声明、
    `OverlayHostPathDevice` 前向声明、`MountTitleUpdate()/MountDlc()/
    MountTitleUpdateAndDlc()` 声明及 `overlay_game_device_/update_mounted_/
    dlc_mounted_` 成员，修复多处缩进破损。
  - `recomp/src/T2ROTF_app.h`：`OnConfigurePaths` 只设置 `game_data_root`
    （删除 update/DLC 目录发现与 `REXCVAR_SET(dlc_data_root, ...)`）。
  - `repos/nfsmw-nx/sdk/.../overlay_host_path_device.{h,cpp}`：回退动态
    `AddLayer`；overlay 设备文件保留但不被 runtime 引用。
  - `scripts/setup_tu_dlc.sh`：重写为把 `update/`（含 `default.xexp`）与 `DLC/`
    合并进 `game/`，幂等。
- 加载顺序：本体（game/）→ TU 补丁随 default.xex 应用 → DLC 随启动接入；
  部署只需 `game/` 单目录。
- 说明：本版为代码级改造，已重建 NRO，**尚未实机验证**。实机请保留
  `logs/rex/rex_perfil.log`、`T2ROTF_001.log` 以确认 TU 数据（如
  `camera.lxb`/`medaltimes.lxb`）与 DLC 资源正常加载。

### 2026-10-15（v4）— 修复 GPU 仿真未装载与内存映射上限（双根因）

- 现象 A（`T2ROTF_004.log`）：启动到 "Switch: in focus" 后日志出现 3 条
  `VdSetGraphicsInterruptCallback / VdInitializeRingBuffer / VdEnableRingBufferRPtrWriteBack`：
  `no GPU emulation loaded (gpu_plugin not set); call ignored`，屏幕黑屏。根因：
  `T2ROTF_app.h` 的 `OnPostInitLogging` 未设置 `gpu_plugin`（SDK CVAR 默认空串），
  `SetupPresentation` 因此不装载 GPU 插件，`graphics_system` 为空。
- 修复 A：`T2ROTF_app.h` 的 `OnPostInitLogging()` 中，在请求 APM 性能档前设置
  `gpu_plugin = "xenos"`（与 nfsmw 参考工程一致；`HasNonDefaultValue` 保护，
  命令行/配置显式指定时不覆盖）。Switch 端 NRO 无动态加载，GPU 插件静态链接，
  装载后 Vd 三个入口生效。
- 现象 B（`T2ROTF_005.log`）：反复 `AllocFixed: no se pudo confirmar... Fallo 0x0000CE01`，
  失败地址从物理堆顶部递减。错误码 `0xCE01` = Module_Kernel + KernelError_ResourceExhausted，
  来自 `guest_memory_switch.cpp` 的 `svcMapProcessCodeMemory`（建立 shadow 代码映射），
  因 hbloader 预膨胀 heap 后 `LimitableResource_Memory`（进程可映射内存上限）耗尽。
- 修复 B（两层，`repos/nfsmw-nx/sdk/src/core/guest_memory_switch.cpp`）：
  1. `RexGmInit` 早期调用 `RaiseResourceLimit()`：通过官方 svc 把 `LimitableResource_Memory`
     上限提 1 GB（`svcGetInfo(InfoType_ResourceLimit)` + `svcGetResourceLimitLimitValue` +
     `svcSetResourceLimitLimitValue`；无 SetLimit 权限时返回 0xDC01，属无害回退并打点打印）。
  2. `RexGmCommit` 合并提交窗口到 2 MB 块（kBlockSize 对齐），小请求命中已提交块跳过
     svc，减少映射次数。
- 说明：本版已重建 NRO；代码修改为静态分析结论，**尚未实机验证**。实机请保留
  `logs/rex/rex_stderr.log`（含 "raised LimitableResource_Memory" 打点）、
  `logs/rex/rex_perfil.log`（`limite de mapeo` 真值）、`T2ROTF_001.log`
  （不应再有 "no GPU emulation loaded"）与 `rex_crash.log`。

### 2026-10-15（v3）— 根治 TimerQueue 崩溃（condition_variable_any → condition_variable）

- 现象：v2 NRO 启动即报"由于发生错误，软件已关闭"，`T2ROTF_001.log` 为 0 字节；`logs/rex/rex_stderr.log` 显示 SaltyNX 连接与 APM 460.8 MHz GPU 配置均已成功，随后崩溃退出（新增崩溃块 `pc = imagen+0x34d93ec`，栈 #00 = `TimerThreadMain` → `execute_native_thread_routine` → `__thread_entry` → `_EntryWrap`）。反汇编定位 `TimerThreadMain+0x710`：`bl condition_variable::wait` 返回后紧跟 `bl std::uncaught_exception()`，是 libstdc++ `condition_variable_any` 的 `_Unlock` 析构异常安全检查，与 jthread/stop_token 无关。
- 修复：`repos/nfsmw-nx/sdk/src/core/timer_queue.cpp` 将成员 `std::condition_variable_any cv_` 改为 `std::condition_variable cv_`（内部锁为 `std::mutex`，`wait/wait_until/notify_one/notify_all` API 兼容；底层直接走 `pthread_cond_wait/pthread_cond_timedwait`，不包含 `uncaught_exception` 逻辑）。反汇编验证新版 `TimerThreadMain` 的等待路径只剩 `condition_variable::wait`、`pthread_cond_timedwait`、`pthread_cond_broadcast`，全程序不再有从 TimerThreadMain 指向 `uncaught_exception` 的调用。
- 说明：`kernel_state.h` 中 `dispatch_cond_` 仍为 `condition_variable_any`（其锁是 `std::recursive_mutex`，无法换成 `std::condition_variable`）；该 dispatch 线程创建早于 EH key 分配、TLS 槽已被清零，不在崩溃路径上，故保持不变。
- 更新方式：用本包内新版 `recomp/out/sw8/T2ROTF.nro`（SHA256 以 `896cdd89` 开头）替换 SD 卡 `sdmc:/switch/Transformers2ROTF/T2ROTF.nro` 即可；`game/`、`update/`、`DLC/` 数据目录无需变动。若替换后不再弹错误而进入黑屏/有更多日志，请把新的 `logs/rex/rex_crash.log`、`T2ROTF_001.log` 一并带回继续排查渲染链路。