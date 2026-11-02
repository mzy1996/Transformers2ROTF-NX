// T2ROTF (2026-10-25): 「guest 入口已经被别处占用」的角色清单。
//
// Renut 的钩子在游戏自己的 D3D 地址上取代 guest 实现；如果同一个 guest 地址在别处
// 已经有 REX_FUNC / REX_HOOK_RAW 定义（一个 guest 入口只能有一个定义），Renut 侧就
// 只能以 renut_native_<T2 地址> 的形式提供实现，由占用方在处理完自己的事情后让出。
//
// 参考构建（MGR XDK 21173）里占用方是 app/mgr_src/mgr_me_hooks.cpp；T2ROTF 的树里没有
// 这个文件，占用方是 src/t2rotf_video_nativo.cpp 的三个 REX_HOOK_RAW（Vulkan 期的
// [T2DIAG] ANILLO / ring 取证钩子），它们打完诊断后调用这里的 renut_native_* 让出。
// 定义在 recomp/src/renut_engine/gpu/d3d/hooks/device.cpp（BANJO_RENUT_HOOK_SHARED）。
#pragma once
#include <cstdint>
struct PPCContext;
extern "C" void renut_native_sub_822D13F0(PPCContext&, uint8_t*);  // D3D_CBlocker_Check
extern "C" void renut_native_sub_822D4488(PPCContext&, uint8_t*);  // D3D_CDevice_BlockOnFence
extern "C" void renut_native_sub_822D4A08(PPCContext&, uint8_t*);  // D3DDevice_SetRingBufferParameters