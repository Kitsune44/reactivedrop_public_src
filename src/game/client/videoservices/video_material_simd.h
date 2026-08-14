// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Grimowy
/**
 * @file video_material_simd.h
 * Project   : FFmpeg Video Services for Valve Source Engine
 * Component : Video Material SIMD Module
 */

#pragma once

#include <cstdint>
#include <cstddef>

// --------------------------------------------------------------------------------------------
// SIMD backend: function-pointer jump table, selected ONCE via GetSIMD() (runtime CPUID dispatch).
// Add new operations (converters, interleavers, ...) as function pointers in SIMDBackend and
// wire them in GetSIMD() - the hot-path namespace wrappers stay zero-overhead.
// --------------------------------------------------------------------------------------------

struct SIMDBackend final
{
    using MemcpyFn = void( * )( uint8_t *, uint8_t *, size_t );
    MemcpyFn memcpy = nullptr;

    // Future operations, e.g. uint16 -> float16 converters:
    // using ConvFn  = void( * )( uint8_t *, uint8_t *, size_t );
    // using Conv2Fn = void( * )( uint8_t *, uint8_t *, uint8_t *, size_t );
    // ConvFn  conv_r  = nullptr;
    // Conv2Fn conv_rg = nullptr;
};

// Runtime-initialized backend: CPUID executed exactly once (lazy, thread-safe).
SIMDBackend &GetSIMD() noexcept;

namespace VideoMaterialSIMD
{
    /**
     * @brief Optimized memory copy; AVF to VTF.
     * Hot path: single function-pointer call through the backend. No per-call CPUID.
     * @param dst - destination buffer: Valve Source Engine: IVTFTexture I8/UV88, uint8_t, 16-byte aligned
     * @param src - source buffer: AVFrame uint8_t/uint16_t, 32/64-byte aligned
     * @param bts - Number of bytes to copy
     */
    inline void Memcpy( uint8_t *dst, uint8_t *src, size_t bts ) noexcept
    {
        GetSIMD().memcpy( dst, src, bts );
    }

    // --------------------------------------------------------------------------------------------
    // CPU feature queries (cold path, used by GetSIMD())
    // --------------------------------------------------------------------------------------------

    bool CPUHasAVX512() noexcept;
    bool CPUHasAVX2() noexcept;
    bool CPUHasAVX() noexcept;
    bool CPUHasSSE41() noexcept;
}
