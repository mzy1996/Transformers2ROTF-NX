// T2ROTF (2026-10-24): placeholder for Renut's Xenos shader cache (gpu/shaders/shader_cache.h).
//
// The real one is shader_cache_switch.cpp, produced by a PC build of XenosRecomp from the game's
// dumped shader containers (out/t2-deko-shaders/containers -> out/renut_switch/). Until that PC
// step exists this file defines the same symbols empty, so the renderer links and can boot; every
// game shader it is asked for is then missing, which banjo_renut_deko3d.cpp reports per shader and
// sums up in MissingGameShaders() ("no game shaders can be drawn yet").
//
// cmake/t2_renut_gpu.cmake picks this file only when the PC-built cache is absent.

#include "renut_engine/gpu/shaders/shader_cache.h"

// One placeholder element each: the arrays are only read up to the (zero) entry count, and a
// zero-length array is not valid C++.
ShaderCacheEntry g_shaderCacheEntries[1] = {};
const size_t g_shaderCacheEntryCount = 0;
ShaderLiteral g_shaderLiterals[1] = {};
ShaderVertexInput g_shaderVertexInputs[1] = {};

const uint32_t g_shaderCacheLayout = kShaderCacheLayout;

const uint8_t g_compressedDxilCache[1] = {0};
const size_t g_dxilCacheCompressedSize = 0;
const size_t g_dxilCacheDecompressedSize = 0;

// extern "C": the generator emits the SPIR-V blob (real, or GLSL-package-backed) with C linkage.
extern "C" const uint8_t g_compressedSpirvCache[1] = {0};
const size_t g_spirvCacheCompressedSize = 0;
const size_t g_spirvCacheDecompressedSize = 0;