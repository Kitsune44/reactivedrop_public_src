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

// --------------------------------------------------------------------------------------------
// CPU Feature Detection
// These functions check for the presence of specific SIMD instruction sets
// AVX512, AVX2, AVX, and SSE4.1
// They use the CPUID instruction to query the CPU capabilities
// and the XGETBV instruction to check the extended control registers.
// --------------------------------------------------------------------------------------------

namespace VideoMaterialSIMD
{
    bool CPUHasAVX512() noexcept {
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

    bool CPUHasAVX2() noexcept {
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

    bool CPUHasAVX() noexcept {
        int info[ 4 ];
        __cpuid( info, 1 );
        bool avx = ( info[ 2 ] & ( 1 << 28 ) ) != 0;
        bool osx = ( info[ 2 ] & ( 1 << 27 ) ) != 0;
        if ( !avx || !osx ) return false;

        uint64_t xcr = _xgetbv( 0 );
        return ( xcr & 0x6 ) == 0x6;
    }

    bool CPUHasSSE41() noexcept {
        int info[ 4 ];
        __cpuid( info, 1 );
        return ( info[ 2 ] & ( 1 << 19 ) ) != 0;
    }
}

namespace
{
    // --------------------------------------------------------------------------------------------
    // Copy / Split implementations (file-local, wired into SIMDBackend by GetSIMD())
    // dst - destination buffer: IVTFTexture I8 plane, uint8_t, 16-byte aligned
    // src - source buffer: AVFrame uint8_t/uint16_t plane, 32/64-byte aligned
    // --------------------------------------------------------------------------------------------

    #define MASK_64B 0x3F   // 6: 64-byte blocks (used with __m512i,  AVX-512 register = 512 bits)
    #define MASK_32B 0x1F   // 5: 32-byte blocks (used with __m256i, AVX/AVX2 register = 256 bits)
    #define MASK_16B  0xF   // 4: 16-byte blocks (used with __m128i, SSE/SSE2 register = 128 bits)

    //-----------------------------------------------------------------
    // COPY BYTES: AVX2 STREAM Aligned(dst)-Unaligned(src)
    //-----------------------------------------------------------------

    void copy_bytes_avx2( uint8_t *dst, uint8_t *src, size_t bts ) noexcept
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
    // COPY BYTES: SSE2 STREAM Aligned(dst)-Aligned(src)
    // NOTE: inline asm is deliberate - MSVC refuses to emit aligned
    //       movdqa/movntdq from intrinsics even with __assume hints.
    //       Data is aligned: src (AVFrame) 32/64 B, dst (Source) 16 B.
    //-----------------------------------------------------------------

    void copy_bytes_sse2( uint8_t *dst, uint8_t *src, size_t bts ) noexcept {
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

    //-----------------------------------------------------------------
    // SPLIT16: uint16 LE plane (2 B/sample) -> lo/hi byte plane
    // lo = even bytes, hi = odd bytes of the little-endian words.
    //-----------------------------------------------------------------

    void split_lo_avx2( uint8_t *src, uint8_t *dst, size_t bts ) noexcept
    {
        const size_t samples = bts >> 1;

        const __m256i mask = _mm256_set1_epi16( 0x00FF );
        const __m128i mask128 = _mm_set1_epi16( 0x00FF );

        // Head: align dst to 32B.
        // Streaming stores require alignment; loads stay unaligned.
        size_t head = ( 32 - ( reinterpret_cast<uintptr_t>( dst ) & MASK_32B ) ) & MASK_32B;
        if ( head > samples )
            head = samples;

        {
            const uint16_t *ps = reinterpret_cast<const uint16_t *>( src );
            uint8_t *pd = dst;

            size_t h = head;
            while ( h-- )
                *pd++ = static_cast<uint8_t>( *ps++ & 0xFF );

            src += head * 2;
            dst += head;
        }

        const size_t rem_samples = samples - head;

        const uint8_t *s = src;
        uint8_t *d = dst;

        // 32 samples per iteration (64 B src -> 32 B dst)
        const size_t n32 = rem_samples >> 5;

        for ( size_t i = 0; i < n32; ++i )
        {
            __m256i a = _mm256_loadu_si256(
                reinterpret_cast<const __m256i *>( s )
            );
            s += 32;

            __m256i b = _mm256_loadu_si256(
                reinterpret_cast<const __m256i *>( s )
            );
            s += 32;

            __m256i lo = _mm256_packus_epi16(
                _mm256_and_si256( a, mask ),
                _mm256_and_si256( b, mask )
            );

            // packus_epi16 operates independently on the two 128-bit lanes.
            // Rearrange the 64-bit lanes into sequential sample order.
            lo = _mm256_permute4x64_epi64(
                lo,
                _MM_SHUFFLE( 3, 1, 2, 0 )
            );

            _mm256_stream_si256(
                reinterpret_cast<__m256i *>( d ),
                lo
            );
            d += 32;
        }

        // Tail: at most 31 samples.
        size_t rem = rem_samples & 31;

        // 16 samples -> 16 output bytes.
        // Destination is still 32B-aligned here if the AVX2 loop ran,
        // and therefore also 16B-aligned. If n32 == 0, the head guarantees
        // 32B alignment as well.
        if ( rem & 16 )
        {
            __m128i a = _mm_loadu_si128(
                reinterpret_cast<const __m128i *>( s )
            );
            s += 16;

            __m128i b = _mm_loadu_si128(
                reinterpret_cast<const __m128i *>( s )
            );
            s += 16;

            __m128i lo = _mm_packus_epi16(
                _mm_and_si128( a, mask128 ),
                _mm_and_si128( b, mask128 )
            );

            _mm_stream_si128(
                reinterpret_cast<__m128i *>( d ),
                lo
            );
            d += 16;
        }

        // 8 samples -> 8 output bytes.
        // Cannot use a streaming store because only 8 bytes are valid.
        if ( rem & 8 )
        {
            __m128i a = _mm_loadu_si128(
                reinterpret_cast<const __m128i *>( s )
            );
            s += 16;

            __m128i lo = _mm_packus_epi16(
                _mm_and_si128( a, mask128 ),
                _mm_setzero_si128()
            );

            _mm_storel_epi64(
                reinterpret_cast<__m128i *>( d ),
                lo
            );
            d += 8;
        }

        // Remaining 0-7 samples.
        rem &= 7;

        const uint16_t *ps = reinterpret_cast<const uint16_t *>( s );
        uint8_t *pd = d;

        while ( rem-- )
            *pd++ = static_cast<uint8_t>( *ps++ & 0xFF );

        // Wait for all streaming stores before returning.
        _mm_sfence();
    }

    void split_hi_avx2( uint8_t *src, uint8_t *dst, size_t bts ) noexcept
    {
        const size_t samples = bts >> 1;

        const __m128i mask128 = _mm_set1_epi16( 0x00FF );

        // Head: align dst to 32B.
        // Streaming stores require alignment; loads stay unaligned.
        size_t head = ( 32 - ( reinterpret_cast<uintptr_t>( dst ) & MASK_32B ) ) & MASK_32B;
        if ( head > samples )
            head = samples;

        {
            const uint16_t *ps = reinterpret_cast<const uint16_t *>( src );
            uint8_t *pd = dst;

            size_t h = head;
            while ( h-- )
                *pd++ = static_cast<uint8_t>( *ps++ >> 8 );

            src += head * 2;
            dst += head;
        }

        const size_t rem_samples = samples - head;

        const uint8_t *s = src;
        uint8_t *d = dst;

        // 32 samples per iteration (64 B src -> 32 B dst)
        const size_t n32 = rem_samples >> 5;

        for ( size_t i = 0; i < n32; ++i )
        {
            __m256i a = _mm256_loadu_si256(
                reinterpret_cast<const __m256i *>( s )
            );
            s += 32;

            __m256i b = _mm256_loadu_si256(
                reinterpret_cast<const __m256i *>( s )
            );
            s += 32;

            __m256i hi = _mm256_packus_epi16(
                _mm256_srli_epi16( a, 8 ),
                _mm256_srli_epi16( b, 8 )
            );

            // packus_epi16 operates independently on the two 128-bit lanes.
            // Rearrange the 64-bit lanes into sequential sample order.
            hi = _mm256_permute4x64_epi64(
                hi,
                _MM_SHUFFLE( 3, 1, 2, 0 )
            );

            _mm256_stream_si256(
                reinterpret_cast<__m256i *>( d ),
                hi
            );
            d += 32;
        }

        // Tail: at most 31 samples.
        size_t rem = rem_samples & 31;

        // 16 samples -> 16 output bytes.
        // Destination is still 32B-aligned here if the AVX2 loop ran,
        // and therefore also 16B-aligned. If n32 == 0, the head guarantees
        // 32B alignment as well.
        if ( rem & 16 )
        {
            __m128i a = _mm_loadu_si128(
                reinterpret_cast<const __m128i *>( s )
            );
            s += 16;

            __m128i b = _mm_loadu_si128(
                reinterpret_cast<const __m128i *>( s )
            );
            s += 16;

            __m128i hi = _mm_packus_epi16(
                _mm_srli_epi16( a, 8 ),
                _mm_srli_epi16( b, 8 )
            );

            _mm_stream_si128(
                reinterpret_cast<__m128i *>( d ),
                hi
            );
            d += 16;
        }

        // 8 samples -> 8 output bytes.
        // Cannot use a streaming store because only 8 bytes are valid.
        if ( rem & 8 )
        {
            __m128i a = _mm_loadu_si128(
                reinterpret_cast<const __m128i *>( s )
            );
            s += 16;

            __m128i hi = _mm_packus_epi16(
                _mm_srli_epi16( a, 8 ),
                _mm_setzero_si128()
            );

            _mm_storel_epi64(
                reinterpret_cast<__m128i *>( d ),
                hi
            );
            d += 8;
        }

        // Remaining 0-7 samples.
        rem &= 7;

        const uint16_t *ps = reinterpret_cast<const uint16_t *>( s );
        uint8_t *pd = d;

        while ( rem-- )
            *pd++ = static_cast<uint8_t>( *ps++ >> 8 );

        // Wait for all streaming stores before returning.
        _mm_sfence();
    }
}

    void split_lo_sse2( uint8_t *src, uint8_t *dst, size_t bts ) noexcept
    {
        const size_t samples = bts >> 1;
        const __m128i mask = _mm_set1_epi16( 0x00FF );

        // Head: align dst to 16B.
        // Streaming stores require alignment; loads stay unaligned.
        size_t head = ( 16 - ( reinterpret_cast< uintptr_t >( dst ) & MASK_16B ) ) & MASK_16B;
        if ( head > samples )
            head = samples;

        {
            const uint16_t *ps = reinterpret_cast< const uint16_t * >( src );
            uint8_t *pd = dst;

            size_t h = head;
            while ( h-- )
                *pd++ = static_cast< uint8_t >( *ps++ & 0xFF );

            src += head * 2;
            dst += head;
        }

        const size_t rem_samples = samples - head;

        const uint8_t *s = src;
        uint8_t *d = dst;

        // 16 samples per iteration (32 B src -> 16 B dst)
        const size_t n16 = rem_samples >> 4;

        for ( size_t i = 0; i < n16; ++i )
        {
            __m128i a = _mm_loadu_si128(
                reinterpret_cast< const __m128i * >( s )
            );
            s += 16;

            __m128i b = _mm_loadu_si128(
                reinterpret_cast< const __m128i * >( s )
            );
            s += 16;

            __m128i lo = _mm_packus_epi16(
                _mm_and_si128( a, mask ),
                _mm_and_si128( b, mask )
            );

            _mm_stream_si128(
                reinterpret_cast< __m128i * >( d ),
                lo
            );
            d += 16;
        }

        // Tail: at most 15 samples.
        size_t rem = rem_samples & 15;

        // 8 samples -> 8 output bytes.
        // Cannot use a streaming store because only 8 bytes are valid.
        if ( rem & 8 )
        {
            __m128i a = _mm_loadu_si128(
                reinterpret_cast< const __m128i * >( s )
            );
            s += 16;

            __m128i lo = _mm_packus_epi16(
                _mm_and_si128( a, mask ),
                _mm_setzero_si128()
            );

            _mm_storel_epi64(
                reinterpret_cast< __m128i * >( d ),
                lo
            );
            d += 8;
        }

        // Remaining 0-7 samples.
        rem &= 7;

        const uint16_t *ps = reinterpret_cast< const uint16_t * >( s );
        uint8_t *pd = d;

        while ( rem-- )
            *pd++ = static_cast< uint8_t >( *ps++ & 0xFF );

        // Wait for all streaming stores before returning.
        _mm_sfence();
    }

    void split_hi_sse2( uint8_t *src, uint8_t *dst, size_t bts ) noexcept
    {
        const size_t samples = bts >> 1;

        // Head: align dst to 16B.
        // Streaming stores require alignment; loads stay unaligned.
        size_t head = ( 16 - ( reinterpret_cast< uintptr_t >( dst ) & MASK_16B ) ) & MASK_16B;
        if ( head > samples )
            head = samples;

        {
            const uint16_t *ps = reinterpret_cast< const uint16_t * >( src );
            uint8_t *pd = dst;

            size_t h = head;
            while ( h-- )
                *pd++ = static_cast< uint8_t >( *ps++ >> 8 );

            src += head * 2;
            dst += head;
        }

        const size_t rem_samples = samples - head;

        const uint8_t *s = src;
        uint8_t *d = dst;

        // 16 samples per iteration (32 B src -> 16 B dst)
        const size_t n16 = rem_samples >> 4;

        for ( size_t i = 0; i < n16; ++i )
        {
            __m128i a = _mm_loadu_si128(
                reinterpret_cast< const __m128i * >( s )
            );
            s += 16;

            __m128i b = _mm_loadu_si128(
                reinterpret_cast< const __m128i * >( s )
            );
            s += 16;

            __m128i hi = _mm_packus_epi16(
                _mm_srli_epi16( a, 8 ),
                _mm_srli_epi16( b, 8 )
            );

            _mm_stream_si128(
                reinterpret_cast< __m128i * >( d ),
                hi
            );
            d += 16;
        }

        // Tail: at most 15 samples.
        size_t rem = rem_samples & 15;

        // 8 samples -> 8 output bytes.
        // Cannot use a streaming store because only 8 bytes are valid.
        if ( rem & 8 )
        {
            __m128i a = _mm_loadu_si128(
                reinterpret_cast< const __m128i * >( s )
            );
            s += 16;

            __m128i hi = _mm_packus_epi16(
                _mm_srli_epi16( a, 8 ),
                _mm_setzero_si128()
            );

            _mm_storel_epi64(
                reinterpret_cast< __m128i * >( d ),
                hi
            );
            d += 8;
        }

        // Remaining 0-7 samples.
        rem &= 7;

        const uint16_t *ps = reinterpret_cast< const uint16_t * >( s );
        uint8_t *pd = d;

        while ( rem-- )
            *pd++ = static_cast< uint8_t >( *ps++ >> 8 );

        // Wait for all streaming stores before returning.
        _mm_sfence();
    }


// --------------------------------------------------------------------------------------------
// Runtime dispatch (REALTIME): CPUID queries run ONCE, at first GetSIMD() call.
// The hot path (VideoMaterialSIMD::Memcpy) afterwards is a single function-pointer call -
// no CPUID, no per-call dispatch overhead.
// Extend SIMDBackend with new operations here (future converters etc.).
// --------------------------------------------------------------------------------------------

SIMDBackend &GetSIMD() noexcept
{
    static SIMDBackend backend = []() noexcept -> SIMDBackend
        {
            SIMDBackend b{};

            if ( VideoMaterialSIMD::CPUHasAVX2() )
            {
                b.copy_bytes = copy_bytes_avx2;
                b.split_lo = split_lo_avx2;
                b.split_hi = split_hi_avx2;
            }
            else
            {
                b.copy_bytes = copy_bytes_sse2;
                b.split_lo = split_lo_sse2;
                b.split_hi = split_hi_sse2;
            }

            return b;
        }();

    return backend;
}