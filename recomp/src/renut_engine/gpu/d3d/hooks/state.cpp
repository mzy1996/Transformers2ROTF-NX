/**
 * @file    gpu/d3d/hooks/state.cpp
 * @brief   N&B's D3D state calls, hooked with the native ports (d3d_native.h).
 *
 * The render and sampler state setters are the ones N&B's state tables name
 * (D3D::InitializeApiState puts them into every device's dispatch table,
 * which the game's inline SetRenderState / SetSamplerState call through).
 * The rest are D3DDevice_Set* and the CDevice internals that write the ring.
 */
#include <cstring>

#include <rex/hook.h>

#include "renut_engine/gpu/banjo_renut_select.h"
#include <rex/types.h>

#include "renut_engine/gpu/d3d/d3d_native.h"
#include "renut_engine/gpu/guest/guest_memory.h"

using namespace renut::gpu;

namespace {

template <u32 State>
void SetRenderStateHook(u32 device, u32 value) {
  native::SetRenderState(native::DeviceAt(device), State, value);
}

template <u32 State>
void SetSamplerStateHook(u32 device, u32 sampler, u32 value) {
  native::SetSamplerState(native::DeviceAt(device), sampler, State, value);
}

template <u32 State>
u32 GetRenderStateHook(u32 device) {
  return native::GetRenderState(native::DeviceAt(device), State);
}

template <u32 State>
u32 GetSamplerStateHook(u32 device, u32 sampler) {
  return native::GetSamplerState(native::DeviceAt(device), sampler, State);
}

// The device's AddRef of a resource it hands out (D3DResource_AddRef,
// resource.cpp).
u32 AddRefResource(u32 resource) {
  return native::AddRefResource(resource);
}

// D3DSurface *GetRenderTarget(D3DDevice*, DWORD Index) [0x8264D8D0]
u32 D3DDevice_GetRenderTarget_hook(u32 device, u32 index) {
  const native::Device d = native::DeviceAt(device);
  const u32 surface = d && index < 4 ? d.W(native::off::RenderTargets + index * 4) : 0;
  if (surface)
    AddRefResource(surface);
  return surface;
}

// D3DSurface *GetDepthStencilSurface(D3DDevice*) [0x8264D910]
u32 D3DDevice_GetDepthStencilSurface_hook(u32 device) {
  const native::Device d = native::DeviceAt(device);
  const u32 surface = d ? d.W(native::off::DepthStencil) : 0;
  if (surface)
    AddRefResource(surface);
  return surface;
}

// D3DVertexBuffer *GetStreamSource(D3DDevice*, UINT StreamNumber,
//     UINT *pOffsetInBytes, UINT *pStride) [0x8264D840]: the offset is the
// stream's fetch constant address less the buffer's.
u32 D3DDevice_GetStreamSource_hook(u32 device, u32 stream, mapped_u32 offset, mapped_u32 stride) {
  const native::Device d = native::DeviceAt(device);
  if (!d || stream >= 16)
    return 0;
  const u32 buffer = d.W(native::off::StreamSources + stream * 4);
  u32 bytes = 0;
  if (buffer) {
    const u32 fetch = d.W(native::off::TextureFetch + (95 - stream) * 8);
    bytes = fetch - native::GpuAddress(GuestPtr<const be_u32>(buffer)[6]);
    AddRefResource(buffer);
  }
  if (offset)
    *offset = bytes;
  if (stride)
    *stride = u32(d.B(native::off::StreamStrides + stream)) << 2;
  return buffer;
}

// void D3DDevice_GetViewport(D3DDevice*, D3DVIEWPORT9*) [0x82276AC0]
void D3DDevice_GetViewport_hook(u32 device, mapped_u32 viewport) {
  const native::Device d = native::DeviceAt(device);
  if (!d || !viewport)
    return;
  for (u32 i = 0; i < 4; ++i)  // fctidz, the low word
    viewport[i] = u32(native::TruncateToI64(d.F(0x3168 + i * 4)));
  viewport[4] = d.W(0x3178);
  viewport[5] = d.W(0x317C);
}

// void D3DDevice_SetClipPlane(D3DDevice*, DWORD Index, const float *pPlane) [0x8264DAE8]
void D3DDevice_SetClipPlane_hook(u32 device, u32 index, mapped_u32 plane) {
  native::Device d = native::DeviceAt(device);
  if (!d || !plane || index >= 6)
    return;
  for (u32 i = 0; i < 4; ++i)
    d.SetW(0x2820 + index * 16 + i * 4, plane[i]);
  d.Dirty(4, u64(1) << (54 - index));
}

// void D3DDevice_SetVertexFetchConstant(D3DDevice*, UINT VertexFetchRegister,
//     D3DVertexBuffer*, UINT Offset) [0x8264DB38]: a vertex fetch slot of the
// 3 in a fetch constant, made a vertex fetch constant first.
void D3DDevice_SetVertexFetchConstant_hook(u32 device, u32 reg, u32 vertex_buffer, u32 offset) {
  native::Device d = native::DeviceAt(device);
  const auto *vb = vertex_buffer ? GuestPtr<const be_u32>(vertex_buffer) : nullptr;
  if (!d || !vb || reg > 0x5F)
    return;
  const u32 slot = (0x5F - reg) / 3;
  const u32 part = (0x5F - reg) - slot * 3;
  const u32 constant = native::off::TextureFetch + slot * 24;
  const u32 type = d.W(constant) & 3;
  if (type == 0 || type == 2)
    std::memset(d.bytes + constant, 0, 24);
  d.SetW(constant, native::Insert(d.W(constant), 1, 0, 2));
  d.Dirty(3, u64(1) << (31 - slot));
  d.SetW(constant + part * 8, native::GpuAddress(u32(vb[6]) + offset));
  d.SetW(constant + part * 8 + 4, native::Insert(u32(vb[7]) - offset, 1, 28, 2));
}

// The boolean and integer shader constants (D3DDevice_Set{Vertex,Pixel}
// ShaderConstant{B,I}): bits from 0x2780 (vertex) / 0x2790 (pixel), a word
// of x, y, z bytes each from 0x27A0 / 0x27E0.
void SetShaderConstantsB(u32 device, u32 base, u32 start, mapped_u32 data, u32 count) {
  native::Device d = native::DeviceAt(device);
  if (!d || !data)
    return;
  for (u32 i = 0; i < count; ++i, ++start) {
    const u32 word = base + (start >> 5) * 4;
    const u32 bit = 1u << (start & 31);
    d.SetW(word, (d.W(word) & ~bit) | ((u32(data[i]) & 1) << (start & 31)));
  }
  d.Dirty(4, u64(1) << 56);
}

void SetShaderConstantsI(u32 device, u32 base, u32 start, mapped_u32 data, u32 count) {
  native::Device d = native::DeviceAt(device);
  if (!d || !data)
    return;
  for (u32 i = 0; i < count; ++i) {
    const u32 x = u32(data[i * 4]) & 0xFF, y = u32(data[i * 4 + 1]) & 0xFF;
    const u32 z = u32(data[i * 4 + 2]) & 0xFF;
    d.SetW(base + (start + i) * 4, (z << 16) | (y << 8) | x);
  }
  d.Dirty(4, u64(1) << 56);
}

// void D3DDevice_SetVertexShaderConstantB(D3DDevice*, UINT StartRegister,
//     const BOOL*, UINT BoolCount) [0x82221930], and the others alike.
void D3DDevice_SetVertexShaderConstantB_hook(u32 device, u32 start, mapped_u32 data, u32 count) {
  SetShaderConstantsB(device, 0x2780, start, data, count);
}
void D3DDevice_SetPixelShaderConstantB_hook(u32 device, u32 start, mapped_u32 data, u32 count) {
  SetShaderConstantsB(device, 0x2790, start, data, count);
}
void D3DDevice_SetVertexShaderConstantI_hook(u32 device, u32 start, mapped_u32 data, u32 count) {
  SetShaderConstantsI(device, 0x27A0, start, data, count);
}
void D3DDevice_SetPixelShaderConstantI_hook(u32 device, u32 start, mapped_u32 data, u32 count) {
  SetShaderConstantsI(device, 0x27E0, start, data, count);
}

// void D3DDevice_EndIndexedVertices(D3DDevice*) [0x826561F8]: the inline end
// of BeginIndexedVertices, back to the ring pointer it saved.
void D3DDevice_EndIndexedVertices_hook(u32 device) {
  native::Device d = native::DeviceAt(device);
  if (d)
    d.SetW(native::off::Ring, d.W(0x3484));
}

// void D3DDevice_SetTexture(D3DDevice*, DWORD Sampler, D3DBaseTexture*,
//     UINT64 PendingMask3) [0x8222F410]; the mask is the sampler's fetch
// constant bit, which the port derives from Sampler.
void D3DDevice_SetTexture_hook(u32 device, u32 sampler, u32 texture) {
  native::SetTexture(native::DeviceAt(device), sampler, texture);
}

// void D3DDevice_SetStreamSource(D3DDevice*, UINT StreamNumber, D3DVertexBuffer*,
//     UINT OffsetInBytes, UINT Stride, UINT64 PendingMask3) [0x8222A408]
void D3DDevice_SetStreamSource_hook(u32 device, u32 stream, u32 vertex_buffer, u32 offset,
                                    u32 stride) {
  native::SetStreamSource(native::DeviceAt(device), stream, vertex_buffer, offset, stride);
}

// void D3DDevice_SetIndices(D3DDevice*, D3DIndexBuffer*) [0x8220EE38]
void D3DDevice_SetIndices_hook(u32 device, u32 index_buffer) {
  native::SetIndices(native::DeviceAt(device), index_buffer);
}

// void D3DDevice_SetVertexDeclaration(D3DDevice*, D3DVertexDeclaration*) [0x82205700]
void D3DDevice_SetVertexDeclaration_hook(u32 device, u32 declaration) {
  native::SetVertexDeclaration(native::DeviceAt(device), declaration);
}

// void D3DDevice_SetViewport(D3DDevice*, const D3DVIEWPORT9*) [0x82222EA0]
void D3DDevice_SetViewport_hook(u32 device, u32 viewport) {
  native::SetViewport(native::DeviceAt(device), viewport);
}

// void D3D::SetViewport(D3DDevice*, float X, float Y, float Width, float Height,
//     float MinZ, float MaxZ) [0x82222F20]
void D3D_SetViewport_hook(u32 device, float x, float y, float width, float height, float min_z,
                          float max_z) {
  native::SetViewport(native::DeviceAt(device), x, y, width, height, min_z, max_z);
}

// void D3DDevice_SetScissorRect(D3DDevice*, const RECT*) [0x82223178]
void D3DDevice_SetScissorRect_hook(u32 device, mapped_u32 rect) {
  if (!rect)
    return;
  native::SetScissorRect(native::DeviceAt(device), i32(u32(rect[0])), i32(u32(rect[1])),
                         i32(u32(rect[2])), i32(u32(rect[3])));
}

// void D3D::SetSurfaceClip(CDevice*, int, int, int, int) [0x82223270]: the
// window scissor and per-tile clip packets, nothing else.
void D3D_SetSurfaceClip_hook(u32 /*device*/, u32, u32, u32, u32) {}

// void D3DDevice_SetRenderTarget(D3DDevice*, DWORD Index, D3DSurface*) [0x8227AB68]
void D3DDevice_SetRenderTarget_hook(u32 device, u32 index, u32 surface) {
  native::SetRenderTarget(native::DeviceAt(device), index, surface);
}

// void D3DDevice_SetDepthStencilSurface(D3DDevice*, D3DSurface*) [0x8227AF40]
void D3DDevice_SetDepthStencilSurface_hook(u32 device, u32 surface) {
  native::SetDepthStencilSurface(native::DeviceAt(device), surface);
}

// void D3D::SetSurfaceInfo(CDevice*, D3DSurface*) [0x8227AEB0]
void D3D_SetSurfaceInfo_hook(u32 device, u32 surface) {
  native::SetSurfaceInfo(native::DeviceAt(device), surface);
}

// void D3DDevice_SetPredication(D3DDevice*, DWORD PredicationMask) [0x8222AA18]
void D3DDevice_SetPredication_hook(u32 device, u32 mask) {
  native::SetPredication(native::DeviceAt(device), mask);
}

// void D3DDevice_SetCommandBufferPredication(D3DDevice*, DWORD TilePredication,
//     DWORD RunPredication) [0x82658678]
void D3DDevice_SetCommandBufferPredication_hook(u32 device, u32 tile_predication,
                                                u32 run_predication) {
  native::SetCommandBufferPredication(native::DeviceAt(device), tile_predication,
                                      run_predication);
}

// void D3DDevice_SetShaderGPRAllocation(D3DDevice*, DWORD Flags,
//     DWORD VertexShaderCount, DWORD PixelShaderCount) [0x82220B20]
void D3DDevice_SetShaderGPRAllocation_hook(u32 device, u32 flags, u32 vertex_count,
                                           u32 pixel_count) {
  native::SetShaderGPRAllocation(native::DeviceAt(device), flags, vertex_count, pixel_count);
}

// void D3D::CleanupGprAllocationSetup(CDevice*) [0x8264E2C0]
void D3D_CleanupGprAllocationSetup_hook(u32 device) {
  native::CleanupGprAllocationSetup(native::DeviceAt(device));
}

// void D3DDevice_SetScreenExtentQueryMode(D3DDevice*, D3DSCREENEXTENTQUERYMODE)
// [0x82658988]
void D3DDevice_SetScreenExtentQueryMode_hook(u32 device, u32 mode) {
  native::SetScreenExtentQueryMode(native::DeviceAt(device), mode);
}

// void D3DDevice_UnsetAll(D3DDevice*) [0x8264E1A8]
void D3DDevice_UnsetAll_hook(u32 device) {
  native::UnsetAll(native::DeviceAt(device));
}

// void D3D::ResetAllState(CDevice*, BOOL) [0x82287D38]
void D3D_ResetAllState_hook(u32 device, u32 /*skip_packets*/) {
  native::ResetAllState(native::DeviceAt(device));
}

// void D3D::InvalidateConstants(D3DDevice*) [0x8264E128]
void D3D_InvalidateConstants_hook(u32 device) {
  native::InvalidateConstants(native::DeviceAt(device));
}

// void D3DDevice_BlockUntilIdle(D3DDevice*) [0x82657488] (IDA: the second
// D3D::InvalidateConstants): waits for the device's current fence (N&B calls
// it before releasing a device).
void D3DDevice_BlockUntilIdle_hook(u32 device) {
  native::InvalidateConstantsAndWait(native::DeviceAt(device));
}

} // namespace

// Render states, by their index in N&B's table (off_82E506F0).
BANJO_RENUT_HOOK(sub_822C7548, SetRenderStateHook<10>);  // D3DDevice_SetRenderState_ZEnable
BANJO_RENUT_HOOK(sub_822C75B8, SetRenderStateHook<11>);  // D3DDevice_SetRenderState_ZFunc
BANJO_RENUT_HOOK(sub_822C7588, SetRenderStateHook<12>);  // D3DDevice_SetRenderState_ZWriteEnable
BANJO_RENUT_HOOK(sub_822C6E30, SetRenderStateHook<13>);  // D3DDevice_SetRenderState_FillMode
BANJO_RENUT_HOOK(sub_822C6E00, SetRenderStateHook<14>);  // D3DDevice_SetRenderState_CullMode
BANJO_RENUT_HOOK(sub_822C6E98, SetRenderStateHook<15>);  // D3DDevice_SetRenderState_AlphaBlendEnable
BANJO_RENUT_HOOK(sub_822C7228, SetRenderStateHook<16>);  // D3DDevice_SetRenderState_SeparateAlphaBlendEnable
BANJO_RENUT_HOOK(sub_822C7358, SetRenderStateHook<17>);  // D3DDevice_SetRenderState_BlendFactor
BANJO_RENUT_HOOK(sub_822C6FB8, SetRenderStateHook<18>);  // D3DDevice_SetRenderState_SrcBlend
BANJO_RENUT_HOOK(sub_822C7048, SetRenderStateHook<19>);  // D3DDevice_SetRenderState_DestBlend
BANJO_RENUT_HOOK(sub_822C6F28, SetRenderStateHook<20>);  // D3DDevice_SetRenderState_BlendOp
BANJO_RENUT_HOOK(sub_822C7148, SetRenderStateHook<21>);  // D3DDevice_SetRenderState_SrcBlendAlpha
BANJO_RENUT_HOOK(sub_822C71B8, SetRenderStateHook<22>);  // D3DDevice_SetRenderState_DestBlendAlpha
BANJO_RENUT_HOOK(sub_822C70D8, SetRenderStateHook<23>);  // D3DDevice_SetRenderState_BlendOpAlpha
BANJO_RENUT_HOOK(sub_822C6E60, SetRenderStateHook<24>);  // D3DDevice_SetRenderState_AlphaTestEnable
BANJO_RENUT_HOOK(sub_822C72C0, SetRenderStateHook<25>);  // D3DDevice_SetRenderState_AlphaRef
BANJO_RENUT_HOOK(sub_822C7328, SetRenderStateHook<26>);  // D3DDevice_SetRenderState_AlphaFunc
BANJO_RENUT_HOOK(sub_822C75F0, SetRenderStateHook<27>);  // D3DDevice_SetRenderState_StencilEnable
BANJO_RENUT_HOOK(sub_822C7630, SetRenderStateHook<28>);  // D3DDevice_SetRenderState_TwoSidedStencilMode
BANJO_RENUT_HOOK(sub_822C7698, SetRenderStateHook<29>);  // D3DDevice_SetRenderState_StencilFail
BANJO_RENUT_HOOK(sub_822C76D0, SetRenderStateHook<30>);  // D3DDevice_SetRenderState_StencilZFail
BANJO_RENUT_HOOK(sub_822C7708, SetRenderStateHook<31>);  // D3DDevice_SetRenderState_StencilPass
BANJO_RENUT_HOOK(sub_822C7668, SetRenderStateHook<32>);  // D3DDevice_SetRenderState_StencilFunc
BANJO_RENUT_HOOK(sub_822C7808, SetRenderStateHook<33>);  // D3DDevice_SetRenderState_StencilRef
BANJO_RENUT_HOOK(sub_822C7828, SetRenderStateHook<34>);  // D3DDevice_SetRenderState_StencilMask
BANJO_RENUT_HOOK(sub_822C7848, SetRenderStateHook<35>);  // D3DDevice_SetRenderState_StencilWriteMask
BANJO_RENUT_HOOK(sub_822C7768, SetRenderStateHook<36>);  // D3DDevice_SetRenderState_CCWStencilFail
BANJO_RENUT_HOOK(sub_822C77A0, SetRenderStateHook<37>);  // D3DDevice_SetRenderState_CCWStencilZFail
BANJO_RENUT_HOOK(sub_822C77D8, SetRenderStateHook<38>);  // D3DDevice_SetRenderState_CCWStencilPass
BANJO_RENUT_HOOK(sub_822C7738, SetRenderStateHook<39>);  // D3DDevice_SetRenderState_CCWStencilFunc
BANJO_RENUT_HOOK(sub_822C7868, SetRenderStateHook<40>);  // D3DDevice_SetRenderState_CCWStencilRef
BANJO_RENUT_HOOK(sub_822C7888, SetRenderStateHook<41>);  // D3DDevice_SetRenderState_CCWStencilMask
BANJO_RENUT_HOOK(sub_822C78A8, SetRenderStateHook<42>);  // D3DDevice_SetRenderState_CCWStencilWriteMask
BANJO_RENUT_HOOK(sub_822C78C8, SetRenderStateHook<43>);  // D3DDevice_SetRenderState_ClipPlaneEnable
BANJO_RENUT_HOOK(sub_822C7C08, SetRenderStateHook<44>);  // D3DDevice_SetRenderState_PointSize
BANJO_RENUT_HOOK(sub_822C7C60, SetRenderStateHook<45>);  // D3DDevice_SetRenderState_PointSizeMin
BANJO_RENUT_HOOK(sub_822C7BF8, SetRenderStateHook<46>);  // D3DDevice_SetRenderState_PointSpriteEnable
BANJO_RENUT_HOOK(sub_822C7CB0, SetRenderStateHook<47>);  // D3DDevice_SetRenderState_PointSizeMax
BANJO_RENUT_HOOK(sub_822C7AA8, SetRenderStateHook<48>);  // D3DDevice_SetRenderState_MultiSampleAntiAlias
BANJO_RENUT_HOOK(sub_822C7AD8, SetRenderStateHook<49>);  // D3DDevice_SetRenderState_MultiSampleMask
BANJO_RENUT_HOOK(sub_822C7920, SetRenderStateHook<50>);  // D3DDevice_SetRenderState_ScissorTestEnable
BANJO_RENUT_HOOK(sub_822C7938, SetRenderStateHook<51>);  // D3DDevice_SetRenderState_SlopeScaleDepthBias
BANJO_RENUT_HOOK(sub_822C7A00, SetRenderStateHook<52>);  // D3DDevice_SetRenderState_DepthBias
BANJO_RENUT_HOOK(sub_822C7AF8, SetRenderStateHook<53>);  // D3DDevice_SetRenderState_ColorWriteEnable
BANJO_RENUT_HOOK(sub_822C7B38, SetRenderStateHook<54>);  // D3DDevice_SetRenderState_ColorWriteEnable1
BANJO_RENUT_HOOK(sub_822C7B78, SetRenderStateHook<55>);  // D3DDevice_SetRenderState_ColorWriteEnable2
BANJO_RENUT_HOOK(sub_822C7BB8, SetRenderStateHook<56>);  // D3DDevice_SetRenderState_ColorWriteEnable3
BANJO_RENUT_HOOK(sub_822C8380, SetRenderStateHook<57>);  // D3DDevice_SetRenderState_TessellationMode
BANJO_RENUT_HOOK(sub_822C8310, SetRenderStateHook<58>);  // D3DDevice_SetRenderState_MinTessellationLevel
BANJO_RENUT_HOOK(sub_822C8348, SetRenderStateHook<59>);  // D3DDevice_SetRenderState_MaxTessellationLevel
BANJO_RENUT_HOOK(sub_822C7D00, SetRenderStateHook<60>);  // D3DDevice_SetRenderState_Wrap0
BANJO_RENUT_HOOK(sub_822C7D20, SetRenderStateHook<61>);  // D3DDevice_SetRenderState_Wrap1
BANJO_RENUT_HOOK(sub_822C7D48, SetRenderStateHook<62>);  // D3DDevice_SetRenderState_Wrap2
BANJO_RENUT_HOOK(sub_822C7D70, SetRenderStateHook<63>);  // D3DDevice_SetRenderState_Wrap3
BANJO_RENUT_HOOK(sub_822C7D98, SetRenderStateHook<64>);  // D3DDevice_SetRenderState_Wrap4
BANJO_RENUT_HOOK(sub_822C7DC0, SetRenderStateHook<65>);  // D3DDevice_SetRenderState_Wrap5
BANJO_RENUT_HOOK(sub_822C7DE8, SetRenderStateHook<66>);  // D3DDevice_SetRenderState_Wrap6
BANJO_RENUT_HOOK(sub_822C7E10, SetRenderStateHook<67>);  // D3DDevice_SetRenderState_Wrap7
BANJO_RENUT_HOOK(sub_822C7E30, SetRenderStateHook<68>);  // D3DDevice_SetRenderState_Wrap8
BANJO_RENUT_HOOK(sub_822C7E50, SetRenderStateHook<69>);  // D3DDevice_SetRenderState_Wrap9
BANJO_RENUT_HOOK(sub_822C7E78, SetRenderStateHook<70>);  // D3DDevice_SetRenderState_Wrap10
BANJO_RENUT_HOOK(sub_822C7EA0, SetRenderStateHook<71>);  // D3DDevice_SetRenderState_Wrap11
BANJO_RENUT_HOOK(sub_822C7EC8, SetRenderStateHook<72>);  // D3DDevice_SetRenderState_Wrap12
BANJO_RENUT_HOOK(sub_822C7EF0, SetRenderStateHook<73>);  // D3DDevice_SetRenderState_Wrap13
BANJO_RENUT_HOOK(sub_822C7F18, SetRenderStateHook<74>);  // D3DDevice_SetRenderState_Wrap14
BANJO_RENUT_HOOK(sub_822C7F40, SetRenderStateHook<75>);  // D3DDevice_SetRenderState_Wrap15
BANJO_RENUT_HOOK(sub_822C8060, SetRenderStateHook<76>);  // D3DDevice_SetRenderState_ViewportEnable
BANJO_RENUT_HOOK(sub_822C80B0, SetRenderStateHook<77>);  // D3DDevice_SetRenderState_HighPrecisionBlendEnable
BANJO_RENUT_HOOK(sub_822C8148, SetRenderStateHook<78>);  // D3DDevice_SetRenderState_HighPrecisionBlendEnable1
BANJO_RENUT_HOOK(sub_822C81E0, SetRenderStateHook<79>);  // D3DDevice_SetRenderState_HighPrecisionBlendEnable2
BANJO_RENUT_HOOK(sub_822C8278, SetRenderStateHook<80>);  // D3DDevice_SetRenderState_HighPrecisionBlendEnable3
BANJO_RENUT_HOOK(sub_822C83B8, SetRenderStateHook<81>);  // D3DDevice_SetRenderState_HalfPixelOffset
BANJO_RENUT_HOOK(sub_822C83F0, SetRenderStateHook<82>);  // D3DDevice_SetRenderState_PrimitiveResetEnable
BANJO_RENUT_HOOK(sub_822C8420, SetRenderStateHook<83>);  // D3DDevice_SetRenderState_PrimitiveResetIndex
BANJO_RENUT_HOOK(sub_822C8448, SetRenderStateHook<84>);  // D3DDevice_SetRenderState_AlphaToMaskEnable
BANJO_RENUT_HOOK(sub_822C8478, SetRenderStateHook<85>);  // D3DDevice_SetRenderState_AlphaToMaskOffsets
BANJO_RENUT_HOOK(sub_822C84A0, SetRenderStateHook<86>);  // D3DDevice_SetRenderState_GuardBand_X
BANJO_RENUT_HOOK(sub_822C84D0, SetRenderStateHook<87>);  // D3DDevice_SetRenderState_GuardBand_Y
BANJO_RENUT_HOOK(sub_822C8500, SetRenderStateHook<88>);  // D3DDevice_SetRenderState_DiscardBand_X
BANJO_RENUT_HOOK(sub_822C8528, SetRenderStateHook<89>);  // D3DDevice_SetRenderState_DiscardBand_Y
BANJO_RENUT_HOOK(sub_822C85F0, SetRenderStateHook<90>);  // D3DDevice_SetRenderState_HiStencilEnable
BANJO_RENUT_HOOK(sub_822C8620, SetRenderStateHook<91>);  // D3DDevice_SetRenderState_HiStencilWriteEnable
BANJO_RENUT_HOOK(sub_822C8650, SetRenderStateHook<92>);  // D3DDevice_SetRenderState_HiStencilFunc
BANJO_RENUT_HOOK(sub_822C8680, SetRenderStateHook<93>);  // D3DDevice_SetRenderState_HiStencilRef
BANJO_RENUT_HOOK(sub_822C86A0, SetRenderStateHook<94>);  // D3DDevice_SetRenderState_PresentInterval
BANJO_RENUT_HOOK(sub_822C86B0, SetRenderStateHook<95>);  // D3DDevice_SetRenderState_PresentImmediateThreshold
BANJO_RENUT_HOOK(sub_822C8558, SetRenderStateHook<96>);  // D3DDevice_SetRenderState_HiZEnable
BANJO_RENUT_HOOK(sub_822C8588, SetRenderStateHook<97>);  // D3DDevice_SetRenderState_HiZWriteEnable
BANJO_RENUT_HOOK(sub_822C7468, SetRenderStateHook<98>);  // D3DDevice_SetRenderState_LastPixel
BANJO_RENUT_HOOK(sub_822C74A0, SetRenderStateHook<99>);  // D3DDevice_SetRenderState_LineWidth
BANJO_RENUT_HOOK(sub_822C7518, SetRenderStateHook<100>);  // D3DDevice_SetRenderState_Buffer2Frames

// Sampler states, by their index in N&B's table (off_82E50BD0).
BANJO_RENUT_HOOK(sub_822C8E50, SetSamplerStateHook<0>);  // D3DDevice_SetSamplerState_AddressU
BANJO_RENUT_HOOK(sub_822C8EA0, SetSamplerStateHook<1>);  // D3DDevice_SetSamplerState_AddressV
BANJO_RENUT_HOOK(sub_822C8EF0, SetSamplerStateHook<2>);  // D3DDevice_SetSamplerState_AddressW
BANJO_RENUT_HOOK(sub_822C8DE0, SetSamplerStateHook<3>);  // D3DDevice_SetSamplerState_BorderColor
BANJO_RENUT_HOOK(sub_822C8878, SetSamplerStateHook<4>);  // D3DDevice_SetSamplerState_MagFilter
BANJO_RENUT_HOOK(sub_822C86D0, SetSamplerStateHook<5>);  // D3DDevice_SetSamplerState_MinFilter
BANJO_RENUT_HOOK(sub_822C8A18, SetSamplerStateHook<6>);  // D3DDevice_SetSamplerState_MipFilter
BANJO_RENUT_HOOK(sub_822C8C38, SetSamplerStateHook<7>);  // D3DDevice_SetSamplerState_MipMapLodBias
BANJO_RENUT_HOOK(sub_822C8CE0, SetSamplerStateHook<8>);  // D3DDevice_SetSamplerState_MaxMipLevel
BANJO_RENUT_HOOK(sub_822C8B18, SetSamplerStateHook<9>);  // D3DDevice_SetSamplerState_MaxAnisotropy
BANJO_RENUT_HOOK(sub_822C8978, SetSamplerStateHook<10>);  // D3DDevice_SetSamplerState_MagFilterZ
BANJO_RENUT_HOOK(sub_822C87D0, SetSamplerStateHook<11>);  // D3DDevice_SetSamplerState_MinFilterZ
BANJO_RENUT_HOOK(sub_822C8A70, SetSamplerStateHook<12>);  // D3DDevice_SetSamplerState_SeparateZFilterEnable
BANJO_RENUT_HOOK(sub_822C8D60, SetSamplerStateHook<13>);  // D3DDevice_SetSamplerState_MinMipLevel
BANJO_RENUT_HOOK(sub_822C8F40, SetSamplerStateHook<14>);  // D3DDevice_SetSamplerState_TrilinearThreshold
BANJO_RENUT_HOOK(sub_822C8B90, SetSamplerStateHook<15>);  // D3DDevice_SetSamplerState_AnisotropyBias
BANJO_RENUT_HOOK(sub_822C8F98, SetSamplerStateHook<16>);  // D3DDevice_SetSamplerState_HGradientExpBias
BANJO_RENUT_HOOK(sub_822C8FF0, SetSamplerStateHook<17>);  // D3DDevice_SetSamplerState_VGradientExpBias
BANJO_RENUT_HOOK(sub_822C9048, SetSamplerStateHook<18>);  // D3DDevice_SetSamplerState_WhiteBorderColorW
BANJO_RENUT_HOOK(sub_822C90A0, SetSamplerStateHook<19>);  // D3DDevice_SetSamplerState_PointBorderEnable

// [T2ROTF] DROPPED(D3DDevice_SetTexture): 角色在 T2ROTF 镜像中未定位（参考注册 sub_8222F410）；未纳入构建，留待后续定位。
// [T2ROTF] DROPPED(D3DDevice_SetTexture_Inline): 角色在 T2ROTF 镜像中未定位（参考注册 sub_82B96538）；未纳入构建，留待后续定位。
// [T2ROTF] DROPPED(D3DDevice_SetStreamSource): 角色在 T2ROTF 镜像中未定位（参考注册 sub_8222A408）；未纳入构建，留待后续定位。
// [T2ROTF] DROPPED(D3DDevice_SetIndices): 角色在 T2ROTF 镜像中未定位（参考注册 sub_8220EE38）；未纳入构建，留待后续定位。
// [T2ROTF] DROPPED(D3DDevice_SetVertexDeclaration): 角色在 T2ROTF 镜像中未定位（参考注册 sub_82205700）；未纳入构建，留待后续定位。
// [T2ROTF] DROPPED(D3DDevice_SetViewport): 角色在 T2ROTF 镜像中未定位（参考注册 sub_82222EA0）；未纳入构建，留待后续定位。
// [T2ROTF] DROPPED(D3D_SetViewport): 角色在 T2ROTF 镜像中未定位（参考注册 sub_82222F20）；未纳入构建，留待后续定位。
// [T2ROTF] DROPPED(D3DDevice_SetScissorRect): 角色在 T2ROTF 镜像中未定位（参考注册 sub_82223178）；未纳入构建，留待后续定位。
// [T2ROTF] DROPPED(D3D_SetSurfaceClip): 角色在 T2ROTF 镜像中未定位（参考注册 sub_82223270）；未纳入构建，留待后续定位。
// [T2ROTF] DROPPED(D3DDevice_SetRenderTarget): 角色在 T2ROTF 镜像中未定位（参考注册 sub_8227AB68）；未纳入构建，留待后续定位。
// [T2ROTF] DROPPED(D3DDevice_SetDepthStencilSurface): 角色在 T2ROTF 镜像中未定位（参考注册 sub_8227AF40）；未纳入构建，留待后续定位。
// [T2ROTF] DROPPED(D3D_SetSurfaceInfo): 角色在 T2ROTF 镜像中未定位（参考注册 sub_8227AEB0）；未纳入构建，留待后续定位。
// [T2ROTF] DROPPED(D3DDevice_SetPredication): 角色在 T2ROTF 镜像中未定位（参考注册 sub_8222AA18）；未纳入构建，留待后续定位。
// [T2ROTF] DROPPED(D3DDevice_SetCommandBufferPredication): 角色在 T2ROTF 镜像中未定位（参考注册 sub_82658678）；未纳入构建，留待后续定位。
// [T2ROTF] DROPPED(D3DDevice_SetShaderGPRAllocation): 角色在 T2ROTF 镜像中未定位（参考注册 sub_82220B20）；未纳入构建，留待后续定位。
// [T2ROTF] DROPPED(D3D_CleanupGprAllocationSetup): 角色在 T2ROTF 镜像中未定位（参考注册 sub_8264E2C0）；未纳入构建，留待后续定位。
// [T2ROTF] DROPPED(D3DDevice_SetScreenExtentQueryMode): 角色在 T2ROTF 镜像中未定位（参考注册 sub_82658988）；未纳入构建，留待后续定位。
// [T2ROTF] DROPPED(D3DDevice_UnsetAll): 角色在 T2ROTF 镜像中未定位（参考注册 sub_8264E1A8）；未纳入构建，留待后续定位。
// [T2ROTF] DROPPED(D3D_ResetAllState): 角色在 T2ROTF 镜像中未定位（参考注册 sub_82287D38）；未纳入构建，留待后续定位。
// [T2ROTF] DROPPED(D3D_InvalidateConstants): 角色在 T2ROTF 镜像中未定位（参考注册 sub_8264E128）；未纳入构建，留待后续定位。
// [T2ROTF] DROPPED(D3DDevice_BlockUntilIdle): 角色在 T2ROTF 镜像中未定位（参考注册 sub_82657488）；未纳入构建，留待后续定位。
// Through a thunk (j_D3DDevice_SetRenderTarget 0x8264D8C8).
// [T2ROTF] DROPPED(D3DDevice_SetRenderTarget_Thunk): 角色在 T2ROTF 镜像中未定位（参考注册 sub_8264D8C8）；未纳入构建，留待后续定位。
// [T2ROTF] DROPPED(D3DDevice_GetRenderTarget): 角色在 T2ROTF 镜像中未定位（参考注册 sub_8264D8D0）；未纳入构建，留待后续定位。
// [T2ROTF] DROPPED(D3DDevice_GetDepthStencilSurface): 角色在 T2ROTF 镜像中未定位（参考注册 sub_8264D910）；未纳入构建，留待后续定位。
// [T2ROTF] DROPPED(D3DDevice_GetStreamSource): 角色在 T2ROTF 镜像中未定位（参考注册 sub_8264D840）；未纳入构建，留待后续定位。
// [T2ROTF] DROPPED(D3DDevice_GetViewport): 角色在 T2ROTF 镜像中未定位（参考注册 sub_82276AC0）；未纳入构建，留待后续定位。
// [T2ROTF] DROPPED(D3DDevice_SetClipPlane): 角色在 T2ROTF 镜像中未定位（参考注册 sub_8264DAE8）；未纳入构建，留待后续定位。
// [T2ROTF] DROPPED(D3DDevice_SetVertexFetchConstant): 角色在 T2ROTF 镜像中未定位（参考注册 sub_8264DB38）；未纳入构建，留待后续定位。
// [T2ROTF] DROPPED(D3DDevice_SetVertexShaderConstantB): 角色在 T2ROTF 镜像中未定位（参考注册 sub_82221930）；未纳入构建，留待后续定位。
// [T2ROTF] DROPPED(D3DDevice_SetPixelShaderConstantB): 角色在 T2ROTF 镜像中未定位（参考注册 sub_82268810）；未纳入构建，留待后续定位。
// [T2ROTF] DROPPED(D3DDevice_SetVertexShaderConstantI): 角色在 T2ROTF 镜像中未定位（参考注册 sub_8264E570）；未纳入构建，留待后续定位。
// [T2ROTF] DROPPED(D3DDevice_SetPixelShaderConstantI): 角色在 T2ROTF 镜像中未定位（参考注册 sub_8264E5C8）；未纳入构建，留待后续定位。
// [T2ROTF] DROPPED(D3DDevice_EndIndexedVertices): 角色在 T2ROTF 镜像中未定位（参考注册 sub_826561F8）；未纳入构建，留待后续定位。

// Render state getters, by their index in N&B's table (off_82E506F0, field 0).
BANJO_RENUT_HOOK(sub_822C7580, GetRenderStateHook<10>);  // D3DDevice_GetRenderState_ZEnable
BANJO_RENUT_HOOK(sub_822C75E0, GetRenderStateHook<11>);  // D3DDevice_GetRenderState_ZFunc
BANJO_RENUT_HOOK(sub_822C75A8, GetRenderStateHook<12>);  // D3DDevice_GetRenderState_ZWriteEnable
BANJO_RENUT_HOOK(sub_822C6E50, GetRenderStateHook<13>);  // D3DDevice_GetRenderState_FillMode
BANJO_RENUT_HOOK(sub_822C6E20, GetRenderStateHook<14>);  // D3DDevice_GetRenderState_CullMode
BANJO_RENUT_HOOK(sub_822C6F18, GetRenderStateHook<15>);  // D3DDevice_GetRenderState_AlphaBlendEnable
BANJO_RENUT_HOOK(sub_822C72B0, GetRenderStateHook<16>);  // D3DDevice_GetRenderState_SeparateAlphaBlendEnable
BANJO_RENUT_HOOK(sub_822C73F0, GetRenderStateHook<17>);  // D3DDevice_GetRenderState_BlendFactor
BANJO_RENUT_HOOK(sub_822C7038, GetRenderStateHook<18>);  // D3DDevice_GetRenderState_SrcBlend
BANJO_RENUT_HOOK(sub_822C70C8, GetRenderStateHook<19>);  // D3DDevice_GetRenderState_DestBlend
BANJO_RENUT_HOOK(sub_822C6FA8, GetRenderStateHook<20>);  // D3DDevice_GetRenderState_BlendOp
BANJO_RENUT_HOOK(sub_822C71A8, GetRenderStateHook<21>);  // D3DDevice_GetRenderState_SrcBlendAlpha
BANJO_RENUT_HOOK(sub_822C7218, GetRenderStateHook<22>);  // D3DDevice_GetRenderState_DestBlendAlpha
BANJO_RENUT_HOOK(sub_822C7138, GetRenderStateHook<23>);  // D3DDevice_GetRenderState_BlendOpAlpha
BANJO_RENUT_HOOK(sub_822C6E88, GetRenderStateHook<24>);  // D3DDevice_GetRenderState_AlphaTestEnable
BANJO_RENUT_HOOK(sub_822C72F8, GetRenderStateHook<25>);  // D3DDevice_GetRenderState_AlphaRef
BANJO_RENUT_HOOK(sub_822C7348, GetRenderStateHook<26>);  // D3DDevice_GetRenderState_AlphaFunc
BANJO_RENUT_HOOK(sub_822C7628, GetRenderStateHook<27>);  // D3DDevice_GetRenderState_StencilEnable
BANJO_RENUT_HOOK(sub_822C7658, GetRenderStateHook<28>);  // D3DDevice_GetRenderState_TwoSidedStencilMode
BANJO_RENUT_HOOK(sub_822C76C0, GetRenderStateHook<29>);  // D3DDevice_GetRenderState_StencilFail
BANJO_RENUT_HOOK(sub_822C76F8, GetRenderStateHook<30>);  // D3DDevice_GetRenderState_StencilZFail
BANJO_RENUT_HOOK(sub_822C7728, GetRenderStateHook<31>);  // D3DDevice_GetRenderState_StencilPass
BANJO_RENUT_HOOK(sub_822C7688, GetRenderStateHook<32>);  // D3DDevice_GetRenderState_StencilFunc
BANJO_RENUT_HOOK(sub_822C7820, GetRenderStateHook<33>);  // D3DDevice_GetRenderState_StencilRef
BANJO_RENUT_HOOK(sub_822C7840, GetRenderStateHook<34>);  // D3DDevice_GetRenderState_StencilMask
BANJO_RENUT_HOOK(sub_822C7860, GetRenderStateHook<35>);  // D3DDevice_GetRenderState_StencilWriteMask
BANJO_RENUT_HOOK(sub_822C7790, GetRenderStateHook<36>);  // D3DDevice_GetRenderState_CCWStencilFail
BANJO_RENUT_HOOK(sub_822C77C8, GetRenderStateHook<37>);  // D3DDevice_GetRenderState_CCWStencilZFail
BANJO_RENUT_HOOK(sub_822C77F8, GetRenderStateHook<38>);  // D3DDevice_GetRenderState_CCWStencilPass
BANJO_RENUT_HOOK(sub_822C7758, GetRenderStateHook<39>);  // D3DDevice_GetRenderState_CCWStencilFunc
BANJO_RENUT_HOOK(sub_822C7880, GetRenderStateHook<40>);  // D3DDevice_GetRenderState_CCWStencilRef
BANJO_RENUT_HOOK(sub_822C78A0, GetRenderStateHook<41>);  // D3DDevice_GetRenderState_CCWStencilMask
BANJO_RENUT_HOOK(sub_822C78C0, GetRenderStateHook<42>);  // D3DDevice_GetRenderState_CCWStencilWriteMask
BANJO_RENUT_HOOK(sub_822C7910, GetRenderStateHook<43>);  // D3DDevice_GetRenderState_ClipPlaneEnable
BANJO_RENUT_HOOK(sub_822C7C50, GetRenderStateHook<44>);  // D3DDevice_GetRenderState_PointSize
BANJO_RENUT_HOOK(sub_822C7CA0, GetRenderStateHook<45>);  // D3DDevice_GetRenderState_PointSizeMin
BANJO_RENUT_HOOK(sub_822C7C00, GetRenderStateHook<46>);  // D3DDevice_GetRenderState_PointSpriteEnable
BANJO_RENUT_HOOK(sub_822C7CF0, GetRenderStateHook<47>);  // D3DDevice_GetRenderState_PointSizeMax
BANJO_RENUT_HOOK(sub_822C7AC8, GetRenderStateHook<48>);  // D3DDevice_GetRenderState_MultiSampleAntiAlias
BANJO_RENUT_HOOK(sub_822C7AF0, GetRenderStateHook<49>);  // D3DDevice_GetRenderState_MultiSampleMask
BANJO_RENUT_HOOK(sub_822C7930, GetRenderStateHook<50>);  // D3DDevice_GetRenderState_ScissorTestEnable
BANJO_RENUT_HOOK(sub_822C79E0, GetRenderStateHook<51>);  // D3DDevice_GetRenderState_SlopeScaleDepthBias
BANJO_RENUT_HOOK(sub_822C7A98, GetRenderStateHook<52>);  // D3DDevice_GetRenderState_DepthBias
BANJO_RENUT_HOOK(sub_822C7B30, GetRenderStateHook<53>);  // D3DDevice_GetRenderState_ColorWriteEnable
BANJO_RENUT_HOOK(sub_822C7B70, GetRenderStateHook<54>);  // D3DDevice_GetRenderState_ColorWriteEnable1
BANJO_RENUT_HOOK(sub_822C7BB0, GetRenderStateHook<55>);  // D3DDevice_GetRenderState_ColorWriteEnable2
BANJO_RENUT_HOOK(sub_822C7BF0, GetRenderStateHook<56>);  // D3DDevice_GetRenderState_ColorWriteEnable3
BANJO_RENUT_HOOK(sub_822C83A8, GetRenderStateHook<57>);  // D3DDevice_GetRenderState_TessellationMode
BANJO_RENUT_HOOK(sub_822C8338, GetRenderStateHook<58>);  // D3DDevice_GetRenderState_MinTessellationLevel
BANJO_RENUT_HOOK(sub_822C8370, GetRenderStateHook<59>);  // D3DDevice_GetRenderState_MaxTessellationLevel
BANJO_RENUT_HOOK(sub_822C7F60, GetRenderStateHook<60>);  // D3DDevice_GetRenderState_Wrap0
BANJO_RENUT_HOOK(sub_822C7F70, GetRenderStateHook<61>);  // D3DDevice_GetRenderState_Wrap1
BANJO_RENUT_HOOK(sub_822C7F80, GetRenderStateHook<62>);  // D3DDevice_GetRenderState_Wrap2
BANJO_RENUT_HOOK(sub_822C7F90, GetRenderStateHook<63>);  // D3DDevice_GetRenderState_Wrap3
BANJO_RENUT_HOOK(sub_822C7FA0, GetRenderStateHook<64>);  // D3DDevice_GetRenderState_Wrap4
BANJO_RENUT_HOOK(sub_822C7FB0, GetRenderStateHook<65>);  // D3DDevice_GetRenderState_Wrap5
BANJO_RENUT_HOOK(sub_822C7FC0, GetRenderStateHook<66>);  // D3DDevice_GetRenderState_Wrap6
BANJO_RENUT_HOOK(sub_822C7FD0, GetRenderStateHook<67>);  // D3DDevice_GetRenderState_Wrap7
BANJO_RENUT_HOOK(sub_822C7FE0, GetRenderStateHook<68>);  // D3DDevice_GetRenderState_Wrap8
BANJO_RENUT_HOOK(sub_822C7FF0, GetRenderStateHook<69>);  // D3DDevice_GetRenderState_Wrap9
BANJO_RENUT_HOOK(sub_822C8000, GetRenderStateHook<70>);  // D3DDevice_GetRenderState_Wrap10
BANJO_RENUT_HOOK(sub_822C8010, GetRenderStateHook<71>);  // D3DDevice_GetRenderState_Wrap11
BANJO_RENUT_HOOK(sub_822C8020, GetRenderStateHook<72>);  // D3DDevice_GetRenderState_Wrap12
BANJO_RENUT_HOOK(sub_822C8030, GetRenderStateHook<73>);  // D3DDevice_GetRenderState_Wrap13
BANJO_RENUT_HOOK(sub_822C8040, GetRenderStateHook<74>);  // D3DDevice_GetRenderState_Wrap14
BANJO_RENUT_HOOK(sub_822C8050, GetRenderStateHook<75>);  // D3DDevice_GetRenderState_Wrap15
BANJO_RENUT_HOOK(sub_822C80A0, GetRenderStateHook<76>);  // D3DDevice_GetRenderState_ViewportEnable
BANJO_RENUT_HOOK(sub_822C8140, GetRenderStateHook<77>);  // D3DDevice_GetRenderState_HighPrecisionBlendEnable
BANJO_RENUT_HOOK(sub_822C81D8, GetRenderStateHook<78>);  // D3DDevice_GetRenderState_HighPrecisionBlendEnable1
BANJO_RENUT_HOOK(sub_822C8270, GetRenderStateHook<79>);  // D3DDevice_GetRenderState_HighPrecisionBlendEnable2
BANJO_RENUT_HOOK(sub_822C8308, GetRenderStateHook<80>);  // D3DDevice_GetRenderState_HighPrecisionBlendEnable3
BANJO_RENUT_HOOK(sub_822C83E0, GetRenderStateHook<81>);  // D3DDevice_GetRenderState_HalfPixelOffset
BANJO_RENUT_HOOK(sub_822C8410, GetRenderStateHook<82>);  // D3DDevice_GetRenderState_PrimitiveResetEnable
BANJO_RENUT_HOOK(sub_822C8440, GetRenderStateHook<83>);  // D3DDevice_GetRenderState_PrimitiveResetIndex
BANJO_RENUT_HOOK(sub_822C8468, GetRenderStateHook<84>);  // D3DDevice_GetRenderState_AlphaToMaskEnable
BANJO_RENUT_HOOK(sub_822C8498, GetRenderStateHook<85>);  // D3DDevice_GetRenderState_AlphaToMaskOffsets
BANJO_RENUT_HOOK(sub_822C84C8, GetRenderStateHook<86>);  // D3DDevice_GetRenderState_GuardBand_X
BANJO_RENUT_HOOK(sub_822C84F8, GetRenderStateHook<87>);  // D3DDevice_GetRenderState_GuardBand_Y
BANJO_RENUT_HOOK(sub_822C8520, GetRenderStateHook<88>);  // D3DDevice_GetRenderState_DiscardBand_X
BANJO_RENUT_HOOK(sub_822C8550, GetRenderStateHook<89>);  // D3DDevice_GetRenderState_DiscardBand_Y
BANJO_RENUT_HOOK(sub_822C8610, GetRenderStateHook<90>);  // D3DDevice_GetRenderState_HiStencilEnable
BANJO_RENUT_HOOK(sub_822C8640, GetRenderStateHook<91>);  // D3DDevice_GetRenderState_HiStencilWriteEnable
BANJO_RENUT_HOOK(sub_822C8670, GetRenderStateHook<92>);  // D3DDevice_GetRenderState_HiStencilFunc
BANJO_RENUT_HOOK(sub_822C8698, GetRenderStateHook<93>);  // D3DDevice_GetRenderState_HiStencilRef
BANJO_RENUT_HOOK(sub_822C86A8, GetRenderStateHook<94>);  // D3DDevice_GetRenderState_PresentInterval
BANJO_RENUT_HOOK(sub_822C86C0, GetRenderStateHook<95>);  // D3DDevice_GetRenderState_PresentImmediateThreshold
BANJO_RENUT_HOOK(sub_822C8578, GetRenderStateHook<96>);  // D3DDevice_GetRenderState_HiZEnable
BANJO_RENUT_HOOK(sub_822C85E0, GetRenderStateHook<97>);  // D3DDevice_GetRenderState_HiZWriteEnable
BANJO_RENUT_HOOK(sub_822C7490, GetRenderStateHook<98>);  // D3DDevice_GetRenderState_LastPixel
BANJO_RENUT_HOOK(sub_822C74E0, GetRenderStateHook<99>);  // D3DDevice_GetRenderState_LineWidth
BANJO_RENUT_HOOK(sub_822C7538, GetRenderStateHook<100>);  // D3DDevice_GetRenderState_Buffer2Frames

// Sampler state getters (off_82E50BD0, field 0).
BANJO_RENUT_HOOK(sub_822C8E88, GetSamplerStateHook<0>);  // D3DDevice_GetSamplerState_AddressU
BANJO_RENUT_HOOK(sub_822C8ED8, GetSamplerStateHook<1>);  // D3DDevice_GetSamplerState_AddressV
BANJO_RENUT_HOOK(sub_822C8F28, GetSamplerStateHook<2>);  // D3DDevice_GetSamplerState_AddressW
BANJO_RENUT_HOOK(sub_822C8E28, GetSamplerStateHook<3>);  // D3DDevice_GetSamplerState_BorderColor
BANJO_RENUT_HOOK(sub_822C8940, GetSamplerStateHook<4>);  // D3DDevice_GetSamplerState_MagFilter
BANJO_RENUT_HOOK(sub_822C8798, GetSamplerStateHook<5>);  // D3DDevice_GetSamplerState_MinFilter
BANJO_RENUT_HOOK(sub_822C8A58, GetSamplerStateHook<6>);  // D3DDevice_GetSamplerState_MipFilter
BANJO_RENUT_HOOK(sub_822C8C98, GetSamplerStateHook<7>);  // D3DDevice_GetSamplerState_MipMapLodBias
BANJO_RENUT_HOOK(sub_822C8D50, GetSamplerStateHook<8>);  // D3DDevice_GetSamplerState_MaxMipLevel
BANJO_RENUT_HOOK(sub_822C8B80, GetSamplerStateHook<9>);  // D3DDevice_GetSamplerState_MaxAnisotropy
BANJO_RENUT_HOOK(sub_822C8A08, GetSamplerStateHook<10>);  // D3DDevice_GetSamplerState_MagFilterZ
BANJO_RENUT_HOOK(sub_822C8868, GetSamplerStateHook<11>);  // D3DDevice_GetSamplerState_MinFilterZ
BANJO_RENUT_HOOK(sub_822C8B08, GetSamplerStateHook<12>);  // D3DDevice_GetSamplerState_SeparateZFilterEnable
BANJO_RENUT_HOOK(sub_822C8DD0, GetSamplerStateHook<13>);  // D3DDevice_GetSamplerState_MinMipLevel
BANJO_RENUT_HOOK(sub_822C8F80, GetSamplerStateHook<14>);  // D3DDevice_GetSamplerState_TrilinearThreshold
BANJO_RENUT_HOOK(sub_822C8BF0, GetSamplerStateHook<15>);  // D3DDevice_GetSamplerState_AnisotropyBias
BANJO_RENUT_HOOK(sub_822C8FD8, GetSamplerStateHook<16>);  // D3DDevice_GetSamplerState_HGradientExpBias
BANJO_RENUT_HOOK(sub_822C9030, GetSamplerStateHook<17>);  // D3DDevice_GetSamplerState_VGradientExpBias
BANJO_RENUT_HOOK(sub_822C9088, GetSamplerStateHook<18>);  // D3DDevice_GetSamplerState_WhiteBorderColorW
BANJO_RENUT_HOOK(sub_822C90E8, GetSamplerStateHook<19>);  // D3DDevice_GetSamplerState_PointBorderEnable
