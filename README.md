# T2ROTF — Transformers: Revenge of the Fallen (Xbox 360) → Nintendo Switch

**English** | [中文版 README (Chinese)](readme_zh.md)

A static-recompilation port of the Xbox 360 title *Transformers: Revenge of the Fallen* to the
Nintendo Switch (homebrew, devkitA64 + libnx). The Xbox 360 PowerPC code is recompiled to AArch64
and executed on top of the **ReXGlue** runtime; the graphics backend is **Renut on deko3d**
(plume + deko3d + uam, the native Tegra X1 Maxwell path).

The per-round engineering log lives in the Chinese document [readme_zh.md](readme_zh.md); this file
is the English entry point for the repository.

* Runtime: Nintendo Switch (homebrew / NRO)
* Graphics backend: **Renut / deko3d** — replaces the earlier Mesa NVK/Vulkan path since 2026-10-26;
  the deko3d build does not link the Vulkan driver (`REXGLUE_SWITCH_NO_NVK`)
* Content model: four independent directories (`game/`, `update/`, `patched/`, `dlc/`) mounted at
  runtime through `OverlayHostPathDevice` + `ContentManager`; the title-update delta patch is fused
  at install time by `xex_patcher_cli` into `patched/default.xex`
* Performance goal: **stable 60 FPS in handheld mode, without overclocking** (not yet achieved — see
  *Known limitations and risks*)
* Buttons: mapped by Xbox physical position (A↔B, X↔Y, LB↔L, RB↔R, LT↔ZL, RT↔ZR, BACK↔minus,
  START↔plus, stick clicks↔StickL/StickR)

## 1. Current status

Current build: **`v9.0-VIVO`** (see `[video] T2ROTF BUILD v9.0-VIVO` in the log).

* Boot, module launch, XAudio output and the graphical-interrupt dispatch chain all work.
* The first logo sequence plays **audio only** — the injected video frames are never retired, so the
  screen stays black. TC/TU and DLC plumbing is in place, but the in-game visual closure has not been
  reached yet.
* Because the picture does not appear yet, the 60 FPS acceptance run has not been performed.

`v9.0-VIVO` (this revision) addresses the root cause pinned down in the previous round: the host-side
producer pushed frames onto a channel that the consumer chain (drain / pump / gate / deliver) never
reads. The fix pins the producer's post channel to **the live display device's own channel**
(`dev + 24`) and repairs the device's missing manager self-pointer (`*(dev+16) = dev`) through the
shadow alias; if the display pipeline wedges (`base+284 == 0`, `base+288 > 0`, all arm markers zero
for a sustained window), the wedge is handed back to the guest's own reset routine
`sub_82882628()` instead of being "balanced" by the host. See section 10 for the verification
checklist.

## 2. Repository layout

| Path | Contents |
|---|---|
| `recomp/` | Main project: CMake project, codegen output (`generated/`), application code (`src/`), build output (`out/`) |
| `recomp/src/T2ROTF_app.h` | Application hooks: automatic path discovery (`game/`), APM performance-profile request |
| `scripts/` | Build and data-preparation scripts |
| `docs/` | Engineering documents (XEX/TU/DLC analysis, performance verification method) |
| `tools/` | In-house helper tools (XEX analysis, TU/DLC unpack, delta patch) |
| `sdk/` | Vendored ReXGlue SDK v0.10.0 — the Switch fork this port is built against (configure with `-DREXSDK_DIR=<repo root>/sdk`), with its Switch patches, sources and third-party trees, committed so the repository is self-contained |
| `sdk_changes/` | Just the SDK files this port patches (single-layer VFS mount, input mapping, profiling, T2 hook table), for quick diffing against upstream |
| `readme_zh.md` | Chinese README and the full per-round engineering log |

The ReXGlue SDK this port is built against is **vendored in `sdk/`** (v0.10.0 plus the port's Switch
patches), so the repository is self-contained apart from the ~7 GB of game data, which is extracted
from your own copy of the title (section 5). Everything this port contributes — the vendored SDK, the
recompiled codegen output, the application/hook source, the helper tools and the per-round reports —
is committed here. Only the SDK's build output (`sdk/out/`) is excluded.

## 3. Requirements

* Windows (this project is built on Windows + Git Bash), or any environment that can run devkitPro
* devkitPro + devkitA64 + libnx (`DEVKITPRO` environment variable set)
* CMake ≥ 3.25, Ninja, Python 3
* Dependency: `sdk/` — the vendored Switch fork of `rexglue-sdk` (v0.10.0): the SDK base this port
  builds against, carrying its Switch bring-up patches (Switch platform layer, single-layer VFS
  mount, guest-memory shadowing, libnx thread/crash hooks). It is **committed in this repository**
  and is the SDK the build actually uses (CMake looks up `REXSDK_DIR`, so configure with
  `-DREXSDK_DIR=<repo root>/sdk`), so no separate clone is needed; only its build output (`sdk/out/`)
  is excluded. `sdk_changes/` collects just the files this port patches. The port's own graphics
  pieces — Renut's renderer engine (`recomp/src/renut_engine/`), the plume/uam GPU libraries
  (`recomp/thirdparty/`) and the T2 hook table — live under `recomp/`, not in `sdk/`.
* The Mesa/NVK `repos/mesa-switch/` SDK is **no longer needed**: the deko3d build does not link the
  Vulkan driver (`PLUME_SWITCH_NO_NVK=ON`). `scripts/build_mesa_sdk.sh` is kept only as history of
  the NVK era.
* `recomp/generated/` already contains the codegen output. Regenerating it requires the host-side
  `sdk/out/win-amd64/rexglue.exe` and `recomp/codegen_t2rotf.sh`.

## 4. Installing on Switch

Copy the following into `sdmc:/switch/Transformers2ROTF/` on the SD card:

```
sdmc:/switch/Transformers2ROTF/
├── T2ROTF.nro          # executable (built at recomp/out/sw8/T2ROTF.nro)
├── T2ROTF.toml         # optional: experimental switches (all defaults are built in)
├── game/               # original base game files
├── update/             # Title Update override files
├── patched/            # install-time fusion product: default.xex with the TU patch applied
└── dlc/                # standalone DLC pack (Maps and Characters Pack)
```

`T2ROTF.toml` is optional. The fix in this revision does **not** depend on it: without the file, all
switches take their built-in defaults (section 9).

Before running on hardware, execute `bash scripts/setup_install.sh` on the PC; the script performs:

1. copies `build/iso_extract/` into `game/` (original base);
2. copies `extracted/TU/` into `update/` (TU override files, without the xexp fusion);
3. fuses `default.xexp` into `default.xex` with `tools/xex_patcher_cli/xex_patcher.exe`, producing
   `patched/default.xex`;
4. copies `extracted/DLC/` into `dlc/` (keeping the sub-directory structure).

Runtime VFS mount order (`OverlayHostPathDevice`, highest priority first):

* `update/` → `patched/` → `game/`: a file with the same name is taken from `update/` first;
  `game:\default.xex` is intercepted by `patched/`, everything else falls back to `game/`.
* DLC: `ContentManager` enumerates the sub-directories of `dlc/` and registers them as marketplace
  content; the game mounts them on demand through `XamContentCreateEx` after start-up.

## 5. Game data (Title Update and DLC)

The game data (`game/` + `update/` + `patched/` + `dlc/`, roughly 7 GB) is **not** part of the source
repository or of the delivery zip. Extract it from your own Xbox 360 image / update package / DLC
package (the tools under `build/` can help), then run `scripts/setup_install.sh` to produce the
directory layout above.

* Title Update: `TFROTF TU/` (the folder next to the project root) — unpacked into `update/` and fused
  into `patched/default.xex` at install time.
* DLC: `Transformers 2 - Maps and Characters Pack/` (next to the project root) — unpacked into `dlc/`.

## 6. Building

```bash
bash scripts/build_switch.sh --no-lto -j 8
```

Product: `recomp/out/sw8/T2ROTF.nro`

Notes:

* `--no-lto`: the 193 MB of generated source does not fit into a whole-program LTO with 16 GB of RAM,
  so the project defaults to `--no-lto`. On a machine with more memory, dropping the flag enables the
  default LTO build (`-flto=2 -flto-partition=balanced`); the nfsmw reference project measured about
  +3 FPS with LTO+PGO.
* Icon / title / author: `icon_T2.png`, `Transformers: Revenge of the Fallen`, `mzy1996`
  (see `rexglue_switch_add_nro` in `recomp/CMakeLists.txt`).
* Incremental deko3d rebuild: `cd recomp/out/sw8 && ninja -j4 T2ROTF` (recompile the changed source,
  relink, repackage the NRO — about 40 seconds).

## 7. Non-overclock clocks and the 60 FPS verification method

The full method and the fields to read are in **`docs/PERFORMANCE_T2ROTF.md`**. Key points:

* The application requests the official Switch APM performance profile: GPU 307.2 → **460.8 MHz**;
  the memory clock stays at the official configured value (on hardware the profile reports an EMC
  clock of 1600.0 MHz). This is a regular official-API request, **not** an overclock. Dock mode
  requests no configuration at all.
* After running on hardware, inspect `logs/rex/rex_perfil.log` (in `logs/rex/` next to the NRO):
  * `juego X fps` — the actual frame rate; the acceptance line is a sustained ≥ 59.5
  * `relojes: CPU ... GPU 460.8 MHz, ...` — simultaneously verifies that no overclock is in use
  * to locate CPU hot spots, create an empty file `perfil_pilas.flag` in `logs/rex/` to enable 1 kHz
    stack sampling
  * in-game, **L+R+ZL** starts the GPU A/B test (eight modes, disabling one GPU job each, to tell
    whether the bottleneck is submission volume or fixed overhead)
* The deko3d build has two further diagnostic logs (both next to the NRO): `deko3d.log` (device /
  queue / swapchain / image heap / shader blocks) and `T2ROTF_001.log` / `T2ROTF_002.log`
  (renut + GPU backend + video + D3D device). To read a crash, look for
  `Unhandled guest access violation` and the `far dentro de la memoria del invitado` line of
  `rex_crash.log`.

## 8. Version identification

Read the version **only** from the `[video] T2ROTF BUILD …` line in the log (currently
`v9.0-VIVO`). The application banner is still the stale `v6.27-MAPFIX` and must not be used. The log
also contains strings such as `v7.6-RECON`, `v7.7-ALIGN` or `v8.2…v8.9` — those come from cvar help
text and historical diagnostic lines, they are not a version mix. Byte size is not a criterion
either: this project embeds a build timestamp, so a rebuild of identical source has the same size but
a different hash. Always trust the hash of the NRO that is actually shipped.

## 9. Configuration reference (`T2ROTF.toml`)

All keys are `kInitOnly`: they are read early during application initialisation, so changing one
requires a full game restart (completely quit and start again). The file must be next to the NRO and
named after the registered application name (`T2ROTF.toml`). Environment-variable form
(`REX_SA2_RENUT_...=1`) also works for PC-side debugging.

| Key | Default | Purpose |
|---|---|---|
| `sa2_renut_gpu_signal` | `true` | Master switch for the host-side GPU-signal system (graphical interrupt source) |
| `sa2_renut_gpu_signal_dev_hook` | `true` | Resolve the D3D device through the D3D hook observation lane |
| `sa2_renut_gpu_signal_hostcb` | `true` | Host-side dispatch of the guest's display interrupt callback `0x822D47A0` |
| `sa2_renut_gpu_signal_hostcb_s1` | `true` | Dispatch the `source=1` (submission-complete) branch (proven a no-op, kept for A/B) |
| `sa2_renut_gpu_signal_hostcb_s1_mode` | `2` | Mode of that dispatch |
| `sa2_renut_gpu_signal_cb_data` | `0` | Callback data (`0` = auto-resolve the device) |
| `sa2_renut_gpu_signal_cb_guard` | `true` | ISR tail-pointer guard |
| `sa2_renut_gpu_signal_cpu` | `2` | CPU index used for dispatch |
| `sa2_renut_gpu_signal_diag` | `true` | `[T2DIAG GPSIG]` diagnostic lines |
| `sa2_renut_gpu_vblank_hz` | `60` | Refresh rate used for dispatch |
| `sa2_renut_gpu_rptr_sync` | `false` | Ring read-pointer synchronisation |
| `sa2_renut_display_guard` | `true` | Display-path guard (startup crash avoidance) |
| `sa2_renut_display_reconcile` | `true` | **Read-only observation since v9.0**: the old host-side "completion balancing" write was removed; only the rate-limited `[T2DIAG RECON-RO …]` line remains. Wedge handling belongs to `sub_82882628` |
| `sa2_renut_display_release` | `false` | Legacy switch; its code is read-only |
| `sa2_renut_display_agent` | `false` | Legacy switch; its code is read-only |
| `sa2_renut_display_align` | `true` | Producer post-channel alignment (kept on top of the v9.0 pinning) |
| `sa2_renut_display_drive` | `true` | Host drives the guest's own per-frame display submission |
| `sa2_renut_display_drive_thread` | `true` | Run that drive on the dedicated host thread `T2ROTF Dirigir`; the vblank dispatch thread never enters guest code |
| `sa2_renut_display_drive_after_ms` | `2500` | Delay before the drive starts |
| `sa2_renut_display_drive_div` | `1` | Drive every N refreshes |
| `sa2_renut_display_drive_produce` | `true` | If `base+284` is still 0 after driving, post one frame through the guest producer `sub_82882730` (never twice for the same frame) |
| `sa2_renut_display_drive_commit` | `false` | Keep driving the guest's per-frame submission entry `sub_82880978` (default off — it permanently wedges the driving thread inside guest display callbacks) |
| `sa2_renut_display_drive_pump_max` | `0` | Upper bound for bounded **non-blocking** hand-off via the guest pump `sub_82881E00` (`r3 = 0x82E55E20`, `r4 = -1`, `r5 = 1`) |
| `sa2_renut_display_drive_watchdog_ms` | `500` | Watchdog: if the driving thread stays inside guest code past this threshold, dump one `[T2DIAG DRIVE-WEDGE]` snapshot and disable the drive |
| `sa2_renut_display_present_guest` | `true` | Per vblank, call the guest's own present-accounting routine `sub_822D9688(r3=0)` for the guest |
| `sa2_renut_display_tick` | `false` | Legacy per-frame tick hook |
| `sa2_renut_swap_callthrough` | `false` | Legacy swap call-through |
| **`sa2_renut_display_vivo`** | **`true`** | **New in v9.0** — pin the producer's post channel to the live display device's own channel (`dev + 24`) and repair `*(dev+16) = dev` (self-pointer) through the shadow alias |
| **`sa2_renut_display_reinicio`** | **`true`** | **New in v9.0** — on a sustained wedge (`base+284 == 0`, `base+288 > 0`, all six arm markers zero for `kReinicioTics = 120` pump entries), call the guest's own display-pipeline reset `sub_82882628()` (budget: 8 times) |

### Rollback ladder for v9.0-VIVO

Turn these off in order (each change requires a full restart):

1. `sa2_renut_display_vivo = false` — repost onto the static channel `0x82E55E38` again
2. `sa2_renut_display_reinicio = false` — no wedge reset
3. `sa2_renut_display_drive_watchdog_ms = 0` — no watchdog
4. `sa2_renut_display_drive_pump_max = 0` — no hand-off (default)
5. `sa2_renut_display_drive_commit = false` — no submission entry (default)
6. `sa2_renut_display_drive_produce = false` — no host-side frame posting
7. `sa2_renut_display_present_guest = false` — back to the v8.5 behaviour
8. `sa2_renut_gpu_signal_hostcb_s1 = false` — back to the v8.4 behaviour (submission-complete interrupt suppressed)
9. `sa2_renut_display_drive_thread = false` — drive and interrupt dispatch share the vblank thread (self-locks on hardware)
10. `sa2_renut_display_drive = false` — back to the v8.2 behaviour (interrupt chain alive, no per-frame submission)
11. `sa2_renut_gpu_signal_dev_hook = false` — back to the v8.1 device-resolution behaviour
12. `sa2_renut_gpu_signal_hostcb = false` — back to the v8.0 idle state
13. `sa2_renut_gpu_signal = false` — back to the v7.7 black-screen 0-fps state

## 10. Verification checklist for v9.0-VIVO

Copy the new `T2ROTF.nro` + `T2ROTF.toml` into `sdmc:/switch/Transformers2ROTF/` and **quit the
application completely before starting it again**. The acceptance scenario is **handheld mode
without overclocking** (the 60 FPS conclusion is only given under that condition).

Log lines to collect:

| Line | What it tells you |
|---|---|
| `[video] T2ROTF BUILD v9.0-VIVO` | the running version |
| `[T2DIAG VIVO-LINK n=… dev=… c0=… c16_antes=…` | the live display device has been captured and `*(dev+16)` has been repaired to a self-pointer |
| `[T2DIAG PUERTA … s16=…` and the three-slot counters | the gate can now read the manager through the live device (previously `s16 = 00000000`, counters all 0) |
| `[T2DIAG DRAIN-IN n=…]` | stops growing once the frame is retired (previously it froze after `n=13`) |
| `[T2DIAG SWAP]` | `n` advances past 1, i.e. frames are actually presented |
| `[T2DIAG REINICIO n=…]` | a wedge was detected and handed to `sub_82882628` |
| `[T2DIAG DRIVE …]` | `produce` / `commit` / `pump` / `muerto` / `wedge_ms` / `fase` / `284` / `288` |
| `juego X fps` and `relojes:` in `rex_perfil.log` | the actual frame rate and the non-overclock clock evidence |
| `rex_crash.log` (if produced) | crash `pc` / `far` / `esr` |

Success criteria: three consecutive logos (luxo → atvi → hasbro) and a non-zero frame rate, with the
GPU clock at 460.8 MHz and no overclock.

## 11. Engineering constraints (hard rules)

These rules are enforced in the source and must not be relaxed:

1. The host never writes display accounting fields (`284`, `288`, `16548`, `16552`, `16564`, `16568`);
   the guest updates them itself.
2. Guest memory is only accessed through the `RexGmShadowFor` chunk-shadow alias. Reading or writing
   a guest window address directly triggers a fatal `Unhandled guest access violation`.
3. The per-frame display drive runs **only** on the dedicated host thread `T2ROTF Dirigir`; the vblank
   dispatch thread never enters guest code.
4. The host never re-posts a frame that is already in flight (`base+284 != 0` ⇒ no posting), and never
   calls the producer with `r3 = 0`.
5. The gate / pump hot path is not modified.

## 12. Known limitations and risks

* Build-level and static-level verification is complete (NRO builds, directory mapping, input mapping
  and APM calls confirmed at byte level). The TU/DLC game content and the on-hardware 60 FPS
  behaviour have not yet been fully validated in a real session: keep
  `logs/rex/rex_perfil.log` from the first hardware run for comparison with
  `docs/PERFORMANCE_T2ROTF.md`.
* The wedge reset budget (`kReinicioTics = 120` pump entries, at most 8 resets) has not been
  calibrated on hardware; the release `sub_82882628()` semantics of some SDK calls are not verified.
* If 460.8 MHz turns out to be insufficient, the next non-overclock candidates are: internal
  resolution 1024x576 (about 36 % fewer pixels), `resolution_scale`-related cvars, and an LTO/PGO
  build.

## 13. Upstream projects and credits

* [`rexglue/rexglue-sdk`](https://github.com/rexglue/rexglue-sdk) — the ReXGlue recompilation runtime
  and kernel/HLE layer (this port uses a Switch fork kept under `repos/nfsmw-nx/`)
* [`hedge-dev/XenosRecomp`](https://github.com/hedge-dev/XenosRecomp) — Xbox 360 (Xenos) GPU
  recompilation; used for codegen and for the shader conversion (987 Xenos shader containers were
  converted to SPIR-V/GLSL and packed into the NRO, `MissingGameShaders = 0`)
* [Renut](https://github.com/rexglue) engine + **deko3d** backend (plume + uam) — the graphics path
* [`danfromtico/mesa-switch`](https://github.com/danfromtico/mesa-switch) and
  [`Nagaa95/mesa-switch`](https://github.com/Nagaa95/mesa-switch) — the Mesa/NVK Vulkan-on-Horizon
  work used during the earlier Vulkan route (historical; the deko3d build does not link it)
* devkitPro / devkitA64 / libnx — toolchain and runtime

## 14. Change history (milestones)

Detailed notes for every revision are in [readme_zh.md](readme_zh.md) (section 3 verification
checklists, section 6 change log). Milestones:

| Version | Date | Summary |
|---|---|---|
| v9.0-VIVO | 2026-11-02 | Pin the producer post channel to the live display device (`dev+24`), repair `*(dev+16)`; hand wedges back to the guest reset `sub_82882628`; make the T37 reconcile branch read-only |
| v8.9-PRODUCE | 2026-11-02 | Reorder to "post first, then (optionally) commit"; add G17/G18/G19 (commit off, bounded non-blocking hand-off, wedge watchdog) |
| v8.8-LIVENESS | 2026-11-01 | Host-side yield predicate only honours `base+284`; host re-posts through the guest's own resolver/producer |
| v8.7-IRQABI | 2026-11-01 | Fix the startup crash: interrupt-callback ABI is `(r3 = source, r4 = device)` |
| v8.6-FLIP | 2026-11-01 | Call the guest's own present-accounting `sub_822D9688(r3=0)` per vblank |
| v8.5-COMPLETE | 2026-11-01 | Un-suppress the submission-complete interrupt and fix its staging-block predicate |
| v8.4-SPLIT | 2026-11-01 | Move the per-frame drive onto a dedicated host thread |
| v8.3-FRAME | 2026-11-01 | Host drives the guest's per-frame display submission |
| v8.2-SHADOW | 2026-11-01 | All guest memory access through the chunk-shadow alias; layered device resolution |
| v8.1-DEVHOOK | 2026-11-01 | Host dispatches the guest's display interrupt callback |
| v8.0-GPUIRQ | 2026-10-31 | Restore the display-completion signal source (`0x7FC80000` window) |
| v7.x | 2026-10-29 … 10-31 | Display-queue root-cause series: reconcile, channel alignment, drain-entry probes |
| v6.28-VFSFIX | 2026-10-19 | Root fix for overlay root-directory file opens (`shaders.lsa` / `effects.lsa` `0xc000000f`) |
| v6.28-B / v6.27-MAPFIX | 2026-10-18 | 2 MB submission granularity + 4 MB fault-in to fix `0xCE01` quota exhaustion; `__appInit` SDMC mount; linker read-only relocation |
| v6.x | 2026-10-15 … 10-17 | Memory-mapping quota, heap shrinking, block sizes, GCC 15 alias linking, standalone directory layout + install-time XexPatcher fusion, icon |
| v3–v5 | 2026-10-15 | TimerQueue crash, GPU-emulation loading, TU/DLC install-time merge model |
| v1–v2 | 2026-10-14 … 10-15 | Project bring-up: Xenia reference playback, FFmpeg-based video drive, first Switch builds |

## 15. Per-round engineering reports (Chinese, in the delivery zip)

The `T2ROTF_switch_port.zip` package carries the Chinese report of every round alongside the source
snapshot, for example `T2ROTF_deko3d_投递通道钉到活体设备与楔死复位报告.md` (this round),
`T2ROTF_deko3d_提交前置生产与看门狗报告.md`, `T2ROTF_deko3d_驱动判活与提交补产报告.md`,
`T2ROTF_deko3d_呈现记账交回guest报告.md`, `T2ROTF_deko3d_显示完成信号源补齐报告.md` and the older
ones. See [readme_zh.md](readme_zh.md) for the full list and the per-round conclusions.