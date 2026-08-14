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


#ifndef VMSM_INLINE
#   if defined(_MSC_VER)
#       define VMSM_INLINE __forceinline
#   elif defined(__GNUC__) || defined(__clang__) || defined(__INTEL_COMPILER)
#       define VMSM_INLINE inline __attribute__((always_inline))
#   else
#       define VMSM_INLINE inline
#   endif
#endif

#ifndef VMSM_RESTRICT
#   if defined(_MSC_VER) || defined(__INTEL_COMPILER)
#       define VMSM_RESTRICT __restrict
#   elif defined(__GNUC__) || defined(__clang__)
#       define VMSM_RESTRICT __restrict__
#   else
#       define VMSM_RESTRICT /* no restrict */
#   endif
#endif


struct VideoMaterialSIMD final {

    // --------------------------------------------------------------------------------------------
    // Public API
    // --------------------------------------------------------------------------------------------

    static VideoMaterialSIMD &GetInstance() noexcept;

    /**
     * @brief Optimized memory copy; AVF to VTF
     * @param dst - destination buffer: Valve Source Engine: IVTFTexture I8/UV88, uint8_t, 16-byte aligned
     * @param src - source buffer: AVFrame uint8_t/uint16_t, 32/64-byte aligned
     * @param bts - Number of bytes to copy
     */
    VMSM_INLINE void Memcpy( uint8_t *VMSM_RESTRICT dst, uint8_t *VMSM_RESTRICT src, size_t bts ) const noexcept {
        m_memcpy_fn( dst, src, bts );
    }

    // --------------------------------------------------------------------------------------------
    // CPU feature queries
    // --------------------------------------------------------------------------------------------

    static bool CPUHasAVX512() noexcept;
    static bool CPUHasAVX2() noexcept;
    static bool CPUHasAVX() noexcept;
    static bool CPUHasSSE41() noexcept;

private:
    VideoMaterialSIMD() noexcept;

    void RuntimeDispatch() noexcept;

    // --------------------------------------------------------------------------------------------
    // Internal function pointers (types and jump table)
    // --------------------------------------------------------------------------------------------

    // Function pointer types
    using MemcpyFn = void( * )( uint8_t *VMSM_RESTRICT, uint8_t *VMSM_RESTRICT, size_t );

    // Function pointers (jump table)
    MemcpyFn m_memcpy_fn = nullptr;

    // --------------------------------------------------------------------------------------------
    // Memcpy implementations
    // --------------------------------------------------------------------------------------------

    // AVX2
    static VMSM_INLINE void memcpy_avx2( uint8_t *VMSM_RESTRICT dst, uint8_t *VMSM_RESTRICT src, size_t bts ) noexcept;
    // SSE2
    static VMSM_INLINE void memcpy_sse2( uint8_t *VMSM_RESTRICT dst, uint8_t *VMSM_RESTRICT src, size_t bts ) noexcept;
};
