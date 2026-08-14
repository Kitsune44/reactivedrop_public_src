// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Grimowy
/**
 * @file video_material_simd.cpp
 * Project   : FFmpeg Video Services for Valve Source Engine
 * Component : Video Material SIMD Module
 */

#include "cbase.h"

#include "video_material_simd.h"

#include <immintrin.h>
#include <intrin.h>
#include <cstring>


#if defined(__clang__)
#define __m128i_u __m128i
#define __m256i_u __m256i
#define __m512i_u __m512i
#endif


VideoMaterialSIMD::VideoMaterialSIMD() noexcept {
    RuntimeDispatch();
}

VideoMaterialSIMD &VideoMaterialSIMD::GetInstance() noexcept {
    static VideoMaterialSIMD instance;
    return instance;
}

// --------------------------------------------------------------------------------------------
// Runtime dispatch for SIMD operations
// This allows the code to select the appropriate SIMD functions based on CPU capabilities
// --------------------------------------------------------------------------------------------

VMSM_INLINE void VideoMaterialSIMD::RuntimeDispatch() noexcept
{
    // Memcpy function dispatch
    /**/
    if ( CPUHasAVX2() ) {
        m_memcpy_fn = memcpy_avx2;
    }
    else {
        m_memcpy_fn = memcpy_sse2;
    }
}

// --------------------------------------------------------------------------------------------
// SIMD Memcpy Implementations for Source Engine Video Materials
// dst - destination buffer: Valve Source Engine: IVTFTexture I8/UV88, uint8_t, 16-byte aligned 
// src - source buffer: AVFrame uint8_t/uint16_t, 32/64-byte aligned
// --------------------------------------------------------------------------------------------

#define MASK_64B 0x3F   // 6: 64-byte blocks (used with __m512i,  AVX-512 register = 512 bits)
#define MASK_32B 0x1F   // 5: 32-byte blocks (used with __m256i, AVX/AVX2 register = 256 bits)
#define MASK_16B  0xF   // 4: 16-byte blocks (used with __m128i, SSE/SSE2 register = 128 bits)

//-----------------------------------------------------------------
// MEMCPY: AVX2 STREAM Aligned(dst)-Unaligned(src)
//-----------------------------------------------------------------

VMSM_INLINE void VideoMaterialSIMD::memcpy_avx2( uint8_t * VMSM_RESTRICT dst, uint8_t * VMSM_RESTRICT src, size_t bts ) noexcept
{
    size_t bts2a = ( 64 - ( reinterpret_cast< uintptr_t >( dst ) & MASK_64B ) ) & MASK_64B;
    if ( bts2a > 0 ) {
        memcpy( dst, src, bts2a );
        dst += bts2a;
        src += bts2a;
        bts -= bts2a;
    }
    
    size_t blx = bts >> 6;
    const size_t rem = bts & MASK_64B;

    const __m256i* s = reinterpret_cast< const __m256i * >( src );
    __m256i* d = reinterpret_cast< __m256i * >( dst );

    while ( blx-- )
    {
        __m256i v0 = _mm256_lddqu_si256( s++ );
        __m256i v1 = _mm256_lddqu_si256( s++ );
        _mm256_stream_si256( d++, v0 );
        _mm256_stream_si256( d++, v1 );
    }
    _mm_sfence();
    if ( rem > 0 )
        memcpy( reinterpret_cast< uint8_t * >( d ), reinterpret_cast< const uint8_t * >( s ), rem );
}

//-----------------------------------------------------------------
// MEMCPY: SSE2 STREAM Aligned(dst)-Aligned(src)
//-----------------------------------------------------------------

VMSM_INLINE void VideoMaterialSIMD::memcpy_sse2( uint8_t *VMSM_RESTRICT dst, uint8_t *VMSM_RESTRICT src, size_t bts ) noexcept {
    size_t blx = bts >> 6;
    const size_t rem = bts & MASK_64B;

    uint8_t *d = dst;
    uint8_t *s = src;

    __asm {
        mov ecx, blx
        jz skip_loop

        mov esi, s
        mov edi, d

        align_loop :
            movdqa xmm0, [ esi ]
            movdqa xmm1, [ esi + 16 ]
            movdqa xmm2, [ esi + 32 ]
            movdqa xmm3, [ esi + 48 ]
            movntdq[ edi ], xmm0
            movntdq[ edi + 16 ], xmm1
            movntdq[ edi + 32 ], xmm2
            movntdq[ edi + 48 ], xmm3
            add esi, 64
            add edi, 64
            dec ecx
            jnz align_loop

            sfence
            mov s, esi
            mov d, edi

        skip_loop :
    }
    if ( rem > 0 )
        memcpy( d, s, rem );
}

// --------------------------------------------------------------------------------------------
// CPU Feature Detection
// These functions check for the presence of specific SIMD instruction sets
// AVX512, AVX2, AVX, and SSE4.1
// They use the CPUID instruction to query the CPU capabilities
// and the XGETBV instruction to check the extended control registers.
// --------------------------------------------------------------------------------------------

VMSM_INLINE bool VideoMaterialSIMD::CPUHasAVX512() noexcept {
    int info[ 4 ];
    __cpuidex( info, 0, 0 );
    if ( info[ 0 ] < 7 ) return false;

    __cpuidex( info, 7, 0 );
    bool avx512f = ( info[ 1 ] & ( 1 << 16 ) ) != 0;
    if ( !avx512f ) return false;

    uint64_t xcr = _xgetbv( 0 );
    if ( ( xcr & 0xE6 ) != 0xE6 ) return false;
    return true;
}

VMSM_INLINE bool VideoMaterialSIMD::CPUHasAVX2() noexcept {
    int info[ 4 ];
    __cpuidex( info, 0, 0 );
    if ( info[ 0 ] < 7 ) return false;

    __cpuidex( info, 7, 0 );
    bool avx2 = ( info[ 1 ] & ( 1 << 5 ) ) != 0;
    if ( !avx2 ) return false;

    __cpuid( info, 1 );
    bool avx = ( info[ 2 ] & ( 1 << 28 ) ) != 0;
    bool osx = ( info[ 2 ] & ( 1 << 27 ) ) != 0;
    if ( !avx || !osx ) return false;

    uint64_t xcr = _xgetbv( 0 );
    if ( ( xcr & 0x6 ) != 0x6 ) return false;
    return true;
}

VMSM_INLINE bool VideoMaterialSIMD::CPUHasAVX() noexcept {
    int info[ 4 ];
    __cpuid( info, 1 );
    bool avx = ( info[ 2 ] & ( 1 << 28 ) ) != 0;
    bool osx = ( info[ 2 ] & ( 1 << 27 ) ) != 0;
    if ( !avx || !osx ) return false;

    uint64_t xcr = _xgetbv( 0 );
    return ( xcr & 0x6 ) == 0x6;
}

VMSM_INLINE bool VideoMaterialSIMD::CPUHasSSE41() noexcept {
    int info[ 4 ];
    __cpuid( info, 1 );
    return ( info[ 2 ] & ( 1 << 19 ) ) != 0;
}