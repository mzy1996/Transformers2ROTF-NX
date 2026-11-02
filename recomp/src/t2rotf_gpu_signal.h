// T2ROTF - 缺失的 GPU 显示完成信号源 (T44 / v8.0-GPUIRQ)
//
// Renut (deko3d) 接管 Direct3D 之后，本移植把 SDK 的 Xenos 图形系统整个移除了
// （recomp/src/renut_engine/gpu/overlay/app_integration.cpp:87 清空 config.graphics）。
// 那一步同时移掉了 SDK 本来附带的两样东西：GPU 寄存器 MMIO 窗口，以及按垂直同步
// 节奏派发 guest 图形中断回调的宿主线程。于是 guest 的显示侧永久停摆（实机
// dev+16548 冻结在 5、显示 ISR sub_822D9570 一次都没进过、0 fps 全黑）。
//
// 本模块只把那两样补回来，不碰呈现：presenter()/provider() 保持 null，
// 画面仍由 Renut/deko3d 出。详见 t2rotf_gpu_signal.cpp 的文件头。

#pragma once

#include <memory>

#include <rex/system/interfaces/graphics.h>

namespace t2rotf::gpu_signal {

// 是否启用（cvar sa2_renut_gpu_signal，默认 true）。默认值变更需重启。
bool Wanted();

// 建立最小图形系统：注册 0x7FC80000 的 MMIO 窗口并启动垂直同步派发线程。
// 关闭时返回 nullptr，调用方不应改动 config.graphics。
std::unique_ptr<rex::system::IGraphicsSystem> Create();

// [v8.2-SHADOW] guest 线程观测到 D3D 设备（sub_822D4488 / sub_822D9570 的 r3）后上报。
// 只在 cvar sa2_renut_gpu_signal_dev_hook 打开时被消费，作为双槽解析的兜底来源。
// 可从任意线程调用：非 0、4 字节对齐、落在设备区间才接收，取值不变不重复写。
void NotificarDispositivo(uint32_t dispositivo);

}  // namespace t2rotf::gpu_signal