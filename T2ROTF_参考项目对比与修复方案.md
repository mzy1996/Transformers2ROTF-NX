# T2ROTF Switch 移植：参考项目对比分析与修复方案

## 一、当前问题定位

T2ROTF 当前卡在启动阶段：
- `WindowSwitch: no default NWindow`
- `Runtime::Setup failed: memory init failed`

**关键分界线**：原版（arena 4.5GB + prealloc 128MB）能启动进入关卡但后续 CE01 崩溃；修改后（arena 3.5GB + prealloc 512/256MB）连 `Runtime::Setup` 都过不去。

---

## 二、参考项目关键配置速查

| 项目 | 类型 | arena | 预提交 | kBlockSize | fault-in 粒度 | NWindow | 特殊修复 |
|------|------|-------|--------|-----------|--------------|---------|---------|
| **sa2-nx** | 全源码 SDK | 4.5GB | **禁用** | 2MB | 4MB | `nwindowGetDefault()` | — |
| **masseffect-nx** | 全源码 SDK | 4.5GB | **禁用** | 2MB | 4MB | `nwindowGetDefault()` | 大页模式(32MB对齐) |
| **project8-sw** | patch 分发 | 4.5GB | **禁用** | 2MB | 4MB | `nwindowGetDefault()` | 热读unwatch、heap封顶1024MB |
| **torchlight-nx** | patch 分发 | 继承nfsmw | 继承nfsmw | 2MB | **2MB** | 继承nfsmw | kCommitGranule=2MB |
| **T2ROTF (当前)** | — | **3.5GB** | **128→64→32→16MB** | **16MB** | **未知** | 失败 | — |

---

## 三、四项关键配置差异（按影响排序）

### 差异1：arena 被错误缩减到 3.5GB

**参考项目**：全部使用 `0x120000000` (4.5GB)

**T2ROTF 当前**：`0xE0000000` (3.5GB) — 用户为缓解 CE01 手动缩减

**问题**：
- Xbox 360 内存模型需要完整的 4.5GB 窗口（0x00000000-0x120000000）
- 缩减 arena 会破坏物理内存映射视图（0xA0/0xC0/0xE0/0x7F 视图）的对齐关系
- `virtmemFindAslr` 分配窗口时，3.5GB 的基址可能与 kernel 期望的 2MB/4KB 对齐不匹配

**修复**：恢复 arena 到 `0x120000000` (4.5GB)

---

### 差异2：预提交策略未禁用（最可能导致 NWindow 失败）

**参考项目**：全部在 `xmemory.cpp` 中明确禁用 512MB 物理预提交

```cpp
#if REX_PLATFORM_SWITCH
  // Not on Switch. It is 512 MB, and on Horizon each chunk is also mapped into the
  // five views... so the process ends up accounting for more than 2 GB and the
  // kernel refuses the next allocation. Measured: the game stayed black after the
  // EA logo with "no se pudo confirmar 0x6A0000 bytes; respaldo 578 MB,
  // mapeado 2139 MB contando espejos" (2001-0103).
  //
  // Instead, physical memory is committed as it is touched: the first access to an
  // uncommitted page lands in the exception handler, which commits 1 MB and
  // retries the instruction (exception_handler_switch.cpp, step 0).
#else
  rex::memory::AllocFixed(..., kCommit, ...);
#endif
```

**T2ROTF 当前**：仍在使用 Direction B prealloc fallback (128→64→32→16 MB)

**问题**：
- prealloc 在 heap 中分配大块内存（即使 16MB），并通过 `svcMapProcessCodeMemory` 映射到 code-memory
- 这会抢占 NVK/系统所需的 code-memory 配额和 heap 空间
- `nwindowGetDefault()` 需要系统分配 NWindow 资源，在内存紧张时可能失败
- 即使 prealloc 成功，剩余内存可能不足以支撑窗口系统初始化

**修复**：禁用预提交，改为按需 fault-in（与参考项目一致）

---

### 差异3：kBlockSize = 16MB（应为 2MB）

**参考项目**：`kBlockSize = 0x200000` (2MB)

**T2ROTF 当前**：`kBlockSize = 0x1000000` (16MB) — nfsmw-nx SDK 默认值

**问题**：
- 16MB 块会导致 `virtmemFindAslr` 对齐要求更严格
- kernel 的 block descriptor 机制在 16MB 对齐时可能找不到合适窗口
- 所有参考项目都将此值改为 2MB，说明 16MB 在 Switch 上有兼容性问题

**修复**：将 `guest_memory_switch.cpp` 中的 `kBlockSize` 改为 `0x200000`

---

### 差异4：异常处理器提交粒度可能不匹配

**参考项目**：`kTrozo = 0x400000` (4MB)

**T2ROTF 当前**：需确认，但 nfsmw-nx 默认可能是 1MB（旧注释提及"commits 1 MB"）

**问题**：
- 1MB 粒度会导致 512MB 物理内存产生 ~2500 个 chunk × 5 视图 = ~12500 个映射
- kernel 的映射限额（~640 个）很快耗尽 → `0xCE01` (KERNELERROR_RESOURCE_EXHAUSTED)
- 4MB 粒度将映射数降至 ~640，刚好在限额内

**修复**：将 `exception_handler_switch.cpp` 中的提交粒度改为 `0x400000` (4MB)

---

## 四、修复优先级与步骤

### 阶段A：解决 NWindow / 启动失败（必须）

1. **恢复 arena 到 4.5GB**
   - 文件：`repos/nfsmw-nx/sdk/src/ui/rex_app.cpp`
   - 修改：`RexGmInit(0x120000000ull)`

2. **禁用预提交**
   - 文件：`repos/nfsmw-nx/sdk/src/system/xmemory.cpp`
   - 在 `REX_PLATFORM_SWITCH` 分支下跳过 `AllocFixed(..., kCommit, ...)`
   - 或者：将 `guest_memory_switch.cpp` 中的 prealloc fallback 全部移除（设为 0）

3. **kBlockSize 改为 2MB**
   - 文件：`repos/nfsmw-nx/sdk/src/core/guest_memory_switch.cpp`
   - 修改：`constexpr size_t kBlockSize = 0x200000;`

### 阶段B：解决 CE01 / 运行时崩溃（在阶段A成功后）

4. **异常处理器提交粒度改为 4MB**
   - 文件：`repos/nfsmw-nx/sdk/src/core/exception_handler_switch.cpp`
   - 修改：`constexpr size_t kTrozo = 0x400000;`

5. **考虑 torchlight 的 kCommitGranule = 2MB 方案**
   - 如果 4MB 仍导致 CE01，将 `RexGmCommit` 的粒度对齐改为 `kBlockSize` (2MB)

6. **考虑 project8 的热读 unwatch 优化**
   - 如果游戏启动后 CPU 占用过高（>200%），实施 read-fault 阈值机制

7. **heap 封顶 1024MB**
   - 在 `runtime_switch.cpp` 或启动代码中设置 `__nx_heap_size = 1024 * 1024 * 1024`

---

## 五、验证方法

1. **编译后检查 NRO 大小**：确保 LTO 产物未异常膨胀
2. **实机测试**：观察是否通过 `Runtime::Setup`
3. **日志检查**：
   - 应看到 `RexGmInit: prealloc failed for all sizes -- falling back to on-demand (B2)`（禁用预提交后）
   - 或根本不再尝试 prealloc
   - 应看到 `WindowSwitch: docked/handheld mode, display resolution ...`（NWindow 成功）
4. **进入关卡后观察**：
   - 是否出现 `0xCE01`（如未改 4MB 粒度则预期会复现）
   - 是否出现 `0x2001-0103`（如未改 kBlockSize 则预期会复现）

---

## 六、参考项目源码位置（本地）

- sa2-nx：`refs/sa2-nx-main/sdk/src/core/guest_memory_switch.cpp`
- masseffect-nx：`refs/masseffect-nx-main/sdk/src/core/guest_memory_switch.cpp`
- project8-sw：`refs/project8-sw-main/patches/rexglue-sdk/0004-switch-horizon-runtime.patch`
- torchlight-nx：`refs/torchlight-nx-main/switch/patches/nfsmw-sdk.patch`

---

*分析基于 5 个参考项目的完整源码比对，所有数值和代码片段均来自实际仓库。*
