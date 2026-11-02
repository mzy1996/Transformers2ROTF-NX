# T3 分析结论：XEX 结构、TU 补丁合成、DLC 解包

> 项目：Xbox 360《变形金刚：卷土重来》(Transformers: Revenge of the Fallen) Switch 移植
> 日期：2026-10-14
> 状态：已实测验证

## 1. 游戏本体（default.xex）

- 路径：`./default.xex`（项目工作目录，14,200,832 B）
- 容器：XEX2，module_flags=0x1（TITLE），header_size=0x3000，security_offset=0x98
- 镜像：image_size=0xF20000，load_address=0x82000000，entry_point=0x824F0E00
- 压缩：FILE_FORMAT_INFO opt=0x3FF，compression=BASIC（1），encryption=NORMAL（1）
  - 4 个 basic 块：{0x138000+0x8000, 0xBD0000+0x180000, 0x8000+0x8000, 0x78000+0}
- 页描述符：242 页（=0xF20000），前 20 页 RODATA、之后 CODE/DATA 混合
- 加密密钥：aes_key=`C38CBEAE4CC24F8E232FF2E4BA1884D5`
- 载荷解密：session_key = AES-128-CBC-DEC(retail_key, aes_key)，对 basic 块逐块解密
  - retail_key=`20B185A59D28FDC340583FBB0896BF91`
- 解压后 flat：`./extracted/base_default.flat`（15,859,712 B，PE 镜像，以 MZ 开头）
  - PE：machine=0x1F2（X360）、18 节、.text=0x9FE670、.data=0x2EF03C、.reloc=0x70354

## 2. TU 升级档（TFROTF TU）

- 原始容器：`../TFROTF TU/TU_10LC21K_0000004000000.0000000000081`（LIVE 容器）
- 解包后：`./extracted/TU/`
  - `default.xexp`（1,005,568 B）— **执行代码增量补丁（DELTA）**
  - `camera.lxb`、`medaltimes.lxb`、`tuning.lxb` — 游戏性数据覆盖
  - `text/{eng,fre,ger,ita,spa}/global.lxb` — 多语言文本覆盖
  - `ui/flash/mpui_tu.{apk,gfx}`、`ui/uiglobal.lxb` — UI 更新

### TU 补丁结构（default.xexp）

- XEX2，module_flags=0x50（PATCH_MODULE|PATCH_DELTA），header_size=0x2000
- FFI：compression=DELTA（3），window_size=0x8000，first_block_hash=`f1081b70...`
- DELTA_PATCH_DESCRIPTOR（opt=0x5FF）：
  - size=0x1962，target_version=0x101，source_version=0x1
  - size_of_target_headers=0x3000
  - delta_headers：source=0 + 0x3000 → target 0
  - delta_image：source=0 + 0xF20000 → target 0
  - aes_key=`5F6D3192DA4E9DDE8C8A5C266A7D2577`（补丁专用密钥）
- 密钥级联（xenia ApplyPatch 同款）：
  - header patch 后 base 密钥变化为 `EDB346D431FBED7385969FB99F898238`
  - patch_session_key = AES-DEC(new_base_session_key, patch.aes_key)
  - **delta key verification PASSED**（image_key_source 校验通过）

### TU 合成产物

- 文件：`./extracted/TU/default_tu.flat`（15,925,248 B = 0xF30000）
- **16 个 delta 块全部 SHA1 校验通过并应用**
- 与原版差异（PE 结构验证 delta 生效）：
  - entry_point：0x4F0E00 → **0x4F2A30**（补丁修改入口）
  - .text：0x9FE670 → **0xA0F120**
  - .data：0x2EF03C → **0x2F09FC**
  - .reloc：0x70354 → **0x71600**
  - 总镜像 +0x10000（新 image_size 0xF30000）
- **移植对象结论：以 `default_tu.flat`（TU 合成 PE）作为 XenosRecomp 的实际输入**，体现 TU 代码补丁

## 3. DLC（Maps and Characters Pack）

- 原始容器：`../Transformers 2 - Maps and Characters Pack/41560834/00000002/3B006974CF74610DCD28E0C809B3CF9C51636B7641`（LIVE 容器）
- 解包后：`./extracted/DLC/`（330 MB，56 文件）
  - `Audio/eng/dlc_chatter.{fev,fsb}` — 新增语音
  - `characters/` — 14 个 DLC 角色（dlcaaerialbot、dlcbumblebeeinvert、dlcjazz、dlcjetfire、dlcmegatron、dlcoptimus、dlcratchet、dlcsideswipe、dlcsideswipered、dlcsoundwave、dlcstarscream 等，各含 `.lxb`/`.fev`/`.str`/`.fsb`）
  - `levels/` — 3 张新对战地图：
    - `mp_cairocity`（59 MB）
    - `mp_courtyard`（47 MB）
    - `mp_hillside`（67 MB）
    - 各含 mission.fev/str/fsb，hillside 另含 art.lxb、audio.lxb
  - `dlc_global.str`（110 KB）、`packagedata.lxb`（11 KB）、`spa.bin`（635 KB）

## 4. 工具链（自研工具，全部离线可用）

| 工具 | 作用 | 位置 |
| --- | --- | --- |
| `tools/xenia_live_unpack.py` | 解包 LIVE 容器（TU/DLC） | 已完成 |
| `tools/xex_analyze.py` | XEX2 header/资源分析 | 已完成 |
| `tools/xex_delta_patch/` | **TU delta 补丁合成器**（C++，复用 xenia 裁剪 libmspack lzxd + TinySHA1 + rijndael） | 已完成 |
| `tools/pe_verify.py` | PE 结构/版本差异对比 | 已完成 |

xex_delta_patch 编译命令（VS2017 BuildTools）：
```
tools\xex_delta_patch\build.bat
```
运行：
```
tools/xex_delta_patch/xex_patch.exe default.xex extracted/TU/default.xexp extracted/TU/default_tu.flat [--dump-base extracted/base_default.flat]
```

## 5. 对后续阶段的输入

- T4/T5：以 `extracted/TU/default_tu.flat` 为 XenosRecomp 重编译输入（PE/X360 镜像）
- T5 系统调用面：游戏为 X360 零售版（需零售密钥/无 devkit path），XenosRecomp 输出依赖 rexglue-sdk 提供 XAPI/XAM/GPU（Xenos 模拟）等调用面
- T6 资源接入：TU 覆盖文件与 DLC 解包目录需映射到 Switch 端沙箱/虚拟文件系统：
  - 基础数据 = 项目根目录大资源（Textures/audio/characters/levels/movies/ui/text...）
  - TU 覆盖优先级 = extracted/TU/*（同名覆盖 base）
  - DLC = extracted/DLC/*（角色+地图+audio）