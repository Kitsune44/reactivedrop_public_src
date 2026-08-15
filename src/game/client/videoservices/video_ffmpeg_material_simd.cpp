// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Grimowy
/**
 * @file video_material_simd.cpp
 * Project   : FFmpeg Video Services for Valve Source Engine
 * Component : Video Material SIMD Module
 */

#include "cbase.h"

#include "video_ffmpeg_material_simd.h"

#include <immintrin.h>
#include <intrin.h>
#include <cstring>


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
    // INTERLEAVE UV: U + V (uint16 LE, 2 B/sample) -> BGRA8888 texels
    // texel memory = [Ulo][Uhi][Vlo][Vhi] == D3D A8R8G8B8 (B=Ulo,G=Uhi,R=Vlo,A=Vhi)
    // unpacklo/hi_epi16 interleaves 16-bit words: u0,v0,u1,v1 -> exactly our layout.
    //-----------------------------------------------------------------

    void interleave_uv_sse2( uint8_t *srcU, uint8_t *srcV, uint8_t *dst, size_t bts ) noexcept
    {
        const size_t samples = bts >> 2;                    // BGRA8888: 4 bytes per sample

        // Head: align dst to 16B (streaming stores require alignment); loads stay unaligned.
        // dst advances 4 B/sample, so convert the byte distance to alignment into samples.
        size_t head = ( ( 16 - ( reinterpret_cast< uintptr_t >( dst ) & MASK_16B ) ) & MASK_16B ) >> 2;
        if ( head > samples )
            head = samples;

        {
            const uint16_t *pu = reinterpret_cast< const uint16_t * >( srcU );
            const uint16_t *pv = reinterpret_cast< const uint16_t * >( srcV );
            uint8_t *pd = dst;
            size_t h = head;
            while ( h-- )
            {
                const uint32_t uv = static_cast< uint32_t >( *pu++ ) | ( static_cast< uint32_t >( *pv++ ) << 16 );
                *pd++ = static_cast< uint8_t >( uv & 0xFF );            // Ulo
                *pd++ = static_cast< uint8_t >( ( uv >> 8 ) & 0xFF );   // Uhi
                *pd++ = static_cast< uint8_t >( ( uv >> 16 ) & 0xFF );  // Vlo
                *pd++ = static_cast< uint8_t >( uv >> 24 );             // Vhi
            }
            srcU += head * 2;
            srcV += head * 2;
            dst += head * 4;
        }

        const size_t rem_samples = samples - head;

        const uint8_t *u = srcU;
        const uint8_t *v = srcV;
        uint8_t *d = dst;

        // 8 samples per iteration (16 B src per plane -> 32 B dst)
        const size_t n8 = rem_samples >> 3;
        for ( size_t i = 0; i < n8; ++i )
        {
            __m128i u0 = _mm_loadu_si128( reinterpret_cast< const __m128i * >( u ) );
            u += 16;
            __m128i v0 = _mm_loadu_si128( reinterpret_cast< const __m128i * >( v ) );
            v += 16;
            __m128i lo = _mm_unpacklo_epi16( u0, v0 );      // s0..s3 texels
            __m128i hi = _mm_unpackhi_epi16( u0, v0 );      // s4..s7 texels
            _mm_stream_si128( reinterpret_cast< __m128i * >( d ), lo );
            d += 16;
            _mm_stream_si128( reinterpret_cast< __m128i * >( d ), hi );
            d += 16;
        }

        // Tail: 0-7 samples, scalar
        size_t rem = rem_samples & 7;
        const uint16_t *pu = reinterpret_cast< const uint16_t * >( u );
        const uint16_t *pv = reinterpret_cast< const uint16_t * >( v );
        uint8_t *pd = d;
        while ( rem-- )
        {
            const uint32_t uv = static_cast< uint32_t >( *pu++ ) | ( static_cast< uint32_t >( *pv++ ) << 16 );
            *pd++ = static_cast< uint8_t >( uv & 0xFF );
            *pd++ = static_cast< uint8_t >( ( uv >> 8 ) & 0xFF );
            *pd++ = static_cast< uint8_t >( ( uv >> 16 ) & 0xFF );
            *pd++ = static_cast< uint8_t >( uv >> 24 );
        }

        // Wait for all streaming stores before returning.
        _mm_sfence();
    }

    void interleave_uv_avx2( uint8_t *srcU, uint8_t *srcV, uint8_t *dst, size_t bts ) noexcept
    {
        const size_t samples = bts >> 2;

        // Head: align dst to 32B (two _mm256_stream_si256 per iteration -> 64B dst/iter).
        // dst advances 4 B/sample, so convert the byte distance to alignment into samples.
        size_t head = ( ( 32 - ( reinterpret_cast< uintptr_t >( dst ) & MASK_32B ) ) & MASK_32B ) >> 2;
        if ( head > samples )
            head = samples;
        {
            const uint16_t *pu = reinterpret_cast< const uint16_t * >( srcU );
            const uint16_t *pv = reinterpret_cast< const uint16_t * >( srcV );
            uint8_t *pd = dst;
            size_t h = head;
            while ( h-- )
            {
                const uint32_t uv = static_cast< uint32_t >( *pu++ ) | ( static_cast< uint32_t >( *pv++ ) << 16 );
                *pd++ = static_cast< uint8_t >( uv & 0xFF );
                *pd++ = static_cast< uint8_t >( ( uv >> 8 ) & 0xFF );
                *pd++ = static_cast< uint8_t >( ( uv >> 16 ) & 0xFF );
                *pd++ = static_cast< uint8_t >( uv >> 24 );
            }
            srcU += head * 2;
            srcV += head * 2;
            dst += head * 4;
        }

        const size_t rem_samples = samples - head;

        const uint8_t *u = srcU;
        const uint8_t *v = srcV;
        uint8_t *d = dst;

        // 16 samples per iteration (32 B src per plane -> 64 B dst)
        const size_t n16 = rem_samples >> 4;
        for ( size_t i = 0; i < n16; ++i )
        {
            __m256i u0 = _mm256_loadu_si256( reinterpret_cast< const __m256i * >( u ) );
            u += 32;
            __m256i v0 = _mm256_loadu_si256( reinterpret_cast< const __m256i * >( v ) );
            v += 32;

            // unpacklo/hi_epi16 operate per 128-bit lane:
            //   lo = [s0..s3 | s8..s11], hi = [s4..s7 | s12..s15]
            // permute2x128 reorders the 128-bit halves into linear sample order:
            //   out0 = [s0..s3, s4..s7], out1 = [s8..s11, s12..s15]
            __m256i lo = _mm256_unpacklo_epi16( u0, v0 );
            __m256i hi = _mm256_unpackhi_epi16( u0, v0 );
            _mm256_stream_si256( reinterpret_cast< __m256i * >( d ),
                _mm256_permute2x128_si256( lo, hi, 0x20 ) );
            d += 32;
            _mm256_stream_si256( reinterpret_cast< __m256i * >( d ),
                _mm256_permute2x128_si256( lo, hi, 0x31 ) );
            d += 32;
        }

        // SSE2 tail for the remaining 8 samples (regular stores; main loop sfence orders streaming)
        const __m128i *us = reinterpret_cast< const __m128i * >( u );
        const __m128i *vs = reinterpret_cast< const __m128i * >( v );
        __m128i *ds = reinterpret_cast< __m128i * >( d );
        size_t n8 = ( rem_samples & 15 ) >> 3;
        while ( n8-- )
        {
            __m128i u0 = _mm_loadu_si128( us++ );
            __m128i v0 = _mm_loadu_si128( vs++ );
            _mm_storeu_si128( ds++, _mm_unpacklo_epi16( u0, v0 ) );
            _mm_storeu_si128( ds++, _mm_unpackhi_epi16( u0, v0 ) );
        }

        // Tail: 0-7 samples, scalar
        size_t rem = rem_samples & 7;
        const uint16_t *pu = reinterpret_cast< const uint16_t * >( us );
        const uint16_t *pv = reinterpret_cast< const uint16_t * >( vs );
        uint8_t *pd = reinterpret_cast< uint8_t * >( ds );
        while ( rem-- )
        {
            const uint32_t uv = static_cast< uint32_t >( *pu++ ) | ( static_cast< uint32_t >( *pv++ ) << 16 );
            *pd++ = static_cast< uint8_t >( uv & 0xFF );
            *pd++ = static_cast< uint8_t >( ( uv >> 8 ) & 0xFF );
            *pd++ = static_cast< uint8_t >( ( uv >> 16 ) & 0xFF );
            *pd++ = static_cast< uint8_t >( uv >> 24 );
        }

        // Wait for all streaming stores before returning.
        _mm_sfence();
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
                b.interleave_uv = interleave_uv_avx2;
            }
            else
            {
                b.copy_bytes = copy_bytes_sse2;
                b.split_lo = split_lo_sse2;
                b.split_hi = split_hi_sse2;
                b.interleave_uv = interleave_uv_sse2;
            }

            return b;
        }();

    return backend;
}