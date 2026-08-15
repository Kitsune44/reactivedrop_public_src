// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Grimowy
/**
 * @file video_material_simd.h
 * Project   : FFmpeg Video Services for Valve Source Engine
 * Component : Video Material SIMD Module
 */

#pragma once

#include <cstdint>


// --------------------------------------------------------------------------------------------
// SIMD backend: function-pointer jump table, selected ONCE via GetSIMD() (runtime CPUID dispatch).
// Every operation below converts/copies a decoded AVFrame plane into a Source texture plane.
// Hot-path wrappers call through the backend - no per-call CPUID, no branching.
// --------------------------------------------------------------------------------------------

struct SIMDBackend final
{
    using CopyBytesFn   = void( * )( uint8_t *, uint8_t *, size_t );   // raw byte copy (8-bit planes)
    using SplitUint16Fn = void( * )( uint8_t *, uint8_t *, size_t );   // uint16 LE -> lo or hi byte plane
    using InterleaveUVFn = void( * )( uint8_t *, uint8_t *, uint8_t *, size_t );  // U+V -> BGRA8888 texels

    CopyBytesFn    copy_bytes = nullptr;
    SplitUint16Fn  split_lo   = nullptr;   // src uint16 LE -> low byte plane
    SplitUint16Fn  split_hi   = nullptr;   // src uint16 LE -> high byte plane
    InterleaveUVFn interleave_uv = nullptr; // srcU + srcV -> packed UV texels
};

// Runtime-initialized backend: CPUID executed exactly once (lazy, thread-safe).
SIMDBackend &GetSIMD() noexcept;

namespace VideoMaterialSIMD
{
    /**
     * @brief Raw byte copy: AVFrame plane -> Source texture plane (8-bit path).
     * Hot path: single function-pointer call through the backend. No per-call CPUID.
     * @param dst - destination buffer: IVTFTexture I8 plane, uint8_t, 16-byte aligned
     * @param src - source buffer: AVFrame uint8_t plane, 32/64-byte aligned
     * @param bts - Number of bytes to copy
     */
    inline void CopyBytes( uint8_t *dst, uint8_t *src, size_t bts ) noexcept
    {
        GetSIMD().copy_bytes( dst, src, bts );
    }

    /**
     * @brief Split a uint16 LE plane into its low byte plane (10/12-bit path).
     * Each uint16 sample (2 bytes LE) maps to one byte: low byte first in memory.
     * @param src - source plane, uint16 LE, 2 bytes per sample
     * @param dst - destination I8 plane, 1 byte per sample
     * @param bts - Number of SOURCE bytes (samples * 2)
     */
    inline void SplitUint16Lo( uint8_t *src, uint8_t *dst, size_t bts ) noexcept
    {
        GetSIMD().split_lo( src, dst, bts );
    }

    /**
     * @brief Split a uint16 LE plane into its high byte plane (10/12-bit path).
     * Each uint16 sample (2 bytes LE) maps to one byte: high byte second in memory.
     * @param src - source plane, uint16 LE, 2 bytes per sample
     * @param dst - destination I8 plane, 1 byte per sample
     * @param bts - Number of SOURCE bytes (samples * 2)
     */
    inline void SplitUint16Hi( uint8_t *src, uint8_t *dst, size_t bts ) noexcept
    {
        GetSIMD().split_hi( src, dst, bts );
    }

    /**
     * @brief Interleave U and V uint16 LE planes into BGRA8888 texels (10/12-bit path).
     * Per sample the destination texel is [Ulo][Uhi][Vlo][Vhi] in memory, which maps 1:1
     * to D3D A8R8G8B8 without any byte swap (B=Ulo, G=Uhi, R=Vlo, A=Vhi).
     * @param srcU - U plane, uint16 LE, 2 bytes per sample
     * @param srcV - V plane, uint16 LE, 2 bytes per sample
     * @param dst  - BGRA8888 texture data, 4 bytes per sample
     * @param bts  - Number of destination bytes (samples * 4)
     */
    inline void InterleaveUV( uint8_t *srcU, uint8_t *srcV, uint8_t *dst, size_t bts ) noexcept
    {
        GetSIMD().interleave_uv( srcU, srcV, dst, bts );
    }

    // --------------------------------------------------------------------------------------------
    // CPU feature queries (cold path, used by GetSIMD())
    // --------------------------------------------------------------------------------------------

    bool CPUHasAVX512() noexcept;
    bool CPUHasAVX2() noexcept;
    bool CPUHasAVX() noexcept;
    bool CPUHasSSE41() noexcept;
}
