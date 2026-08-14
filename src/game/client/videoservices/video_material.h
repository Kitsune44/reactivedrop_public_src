// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Grimowy
/**
 * @file video_ffmpeg_material.h
 * Project   : FFmpeg Video Services for Valve Source Engine
 * Component : Video Material Module
 */

#pragma once

#include "video_material_simd.h"

#include "materialsystem/imaterial.h"           // For IMaterial
#include "materialsystem/itexture.h"            // For ITexture
#include "materialsystem/MaterialSystemUtil.h"  // For ITextureRegenerator
#include "keyvalues.h"                          // For KeyValues

#include <cstdint>								// For uint8_t
#include <cstddef>								// For size_t
#include <string>								// For std::string



 /**
  * @class CTextureRegenerator
  * @brief Copies an AVFrame 8-bit plane into an I8 texture plane.
  *        Branchless hot path: direct SIMD byte copy, no per-texel logic.
  * src - Pointer to externally managed planar data pointer: AVFrame uint8_t plane, 32/64-byte aligned
  */
class CTextureRegenerator final : public ITextureRegenerator {
public:
    explicit CTextureRegenerator( uint8_t **src ) noexcept
        : m_src( src ) {
    }

    void RegenerateTextureBits( ITexture *, IVTFTexture *dst, Rect_t * ) noexcept override {
        VideoMaterialSIMD::CopyBytes( dst->ImageData(), *m_src, dst->FaceSizeInBytes( 0 ) );
    }

    void Release() noexcept override {
        delete this;
    }

private:
    uint8_t **m_src{ nullptr }; // Pointer to externally managed planar data pointer.
};


 /**
  * @class CSplit16Regenerator
  * @brief Splits a uint16 LE plane into its lo or hi byte plane (I8 texture).
  *        The split variant is selected ONCE at construction (function pointer),
  *        so the hot path has no branches.
  * src - Pointer to externally managed planar data pointer.
  */
class CSplit16Regenerator final : public ITextureRegenerator {
public:
    using SplitFn = void( * )( uint8_t *, uint8_t *, size_t );

    CSplit16Regenerator( uint8_t **src, SplitFn fn ) noexcept
        : m_src( src ), m_fn( fn ) {
    }

    void RegenerateTextureBits( ITexture *, IVTFTexture *dst, Rect_t * ) noexcept override {
        m_fn( *m_src, dst->ImageData(), dst->FaceSizeInBytes( 0 ) * 2 );   // bts = source bytes (samples * 2)
    }

    void Release() noexcept override {
        delete this;
    }

private:
    uint8_t **m_src{ nullptr }; // Pointer to externally managed planar data pointer.
    SplitFn m_fn{ nullptr };    // split_lo or split_hi, selected once.
};


/**
 * @class VideoSEMaterial
 * @brief Manages YUV textures, regeneartos and procedural material for video rendering.
 */
class VideoSEMaterial final {
public:
    VideoSEMaterial() noexcept {}
    ~VideoSEMaterial() noexcept {
        Reset();
    }

    /**
     * @brief Safely shuts down textures, regenerators and material.
     * This method ensures that all resources are released properly to avoid memory leaks.
     */
    void Reset() noexcept
    {
        auto ShutdownTexture = []( const char *&pTextureName, ITexture *&pTexture, ITextureRegenerator *&pTextureRegen ) {
            if ( !g_pMaterialSystem->IsTextureLoaded( pTextureName ) )
                return;
            pTexture->SetTextureRegenerator( nullptr, true );
            pTextureRegen = nullptr;
            pTexture->DecrementReferenceCount();
            pTexture->DecrementReferenceCount();
            pTexture->DecrementReferenceCount();
            pTexture->DeleteIfUnreferenced();
            pTexture = nullptr;
            };
        ShutdownTexture( m_pTextureNameY, m_pTextureY, m_pTextureRegenY );
        ShutdownTexture( m_pTextureNameYHi, m_pTextureYHi, m_pTextureRegenYHi );
        ShutdownTexture( m_pTextureNameU, m_pTextureU, m_pTextureRegenU );
        ShutdownTexture( m_pTextureNameUHi, m_pTextureUHi, m_pTextureRegenUHi );
        ShutdownTexture( m_pTextureNameV, m_pTextureV, m_pTextureRegenV );
        ShutdownTexture( m_pTextureNameVHi, m_pTextureVHi, m_pTextureRegenVHi );
        g_pMaterialSystem->EvictManagedResources();

        if ( m_pMaterial ) {
            m_pMaterial->DecrementReferenceCount();
            m_pMaterial->DeleteIfUnreferenced();
            m_pMaterial = nullptr;
        }
    }

    /**
     * @brief Initializes textures, regenerators, and material.
     * @param videoWidthY    - Width of Y plane.
     * @param videoHeightY   - Height of Y plane.
     * @param videoWidthUV   - Width of U and V planes.
     * @param videoHeightUV  - Height of U and V planes.
     * @param srcY           - Reference to pointer to Y plane buffer.
     * @param srcU           - Reference to pointer to U plane buffer.
     * @param srcV           - Reference to pointer to V plane buffer.
     * @param bitDepth       - Bit depth of the video frames.
     * @param colorRange     - Color range (1=Limited, 2=Full).
     * @param colorSpace     - Color space (FFmpeg: AVColorSpace).
     * @param colorTransferC - Color transfer characteristic (FFmpeg: AVColorTransferCharacteristic).
     * @param colorPrimaries - Color primaries (FFmpeg: AVColorPrimaries).
     */
    void Init(
        const size_t videoWidthY, const size_t videoHeightY,
        const size_t videoWidthUV, const size_t videoHeightUV,
        uint8_t *&srcY, uint8_t *&srcU, uint8_t *&srcV,
        int bitDepth = 8, int colorRange = 1, int colorSpace = 1,
        int colorTransferC = 0, int colorPrimaries = 1
    ) noexcept
    {
        // TEMP TEST: POINTSAMPLE for >8-bit (like old working code) to isolate bilinear quantization.
        const int nFlags = TEXTUREFLAGS_PROCEDURAL | TEXTUREFLAGS_SINGLECOPY
            | TEXTUREFLAGS_NOMIP | TEXTUREFLAGS_NOLOD | TEXTUREFLAGS_CLAMPS | TEXTUREFLAGS_CLAMPT;

        KeyValues *pVMTKeyValues = new KeyValues( "VideoYUV" );

        auto CreateTexture = [ & ]( const char *pTextureName, ITexture *&pTexture,
            ITextureRegenerator *&pTextureRegen, ImageFormat fmt, int w, int h )
            {
                pTexture = g_pMaterialSystem->CreateProceduralTexture( pTextureName, "VideoFFmpegCacheTextures", w, h, fmt, nFlags );
                pTexture->IncrementReferenceCount();
                pTexture->SetTextureRegenerator( pTextureRegen );
            };

        if ( bitDepth == 8 )
        {
            // 8-bit: three I8 planes, raw copy
            m_pTextureRegenY = new CTextureRegenerator( &srcY );
            CreateTexture( m_pTextureNameY, m_pTextureY, m_pTextureRegenY, IMAGE_FORMAT_I8,
                static_cast< int >( videoWidthY ), static_cast< int >( videoHeightY ) );
            m_pTextureRegenU = new CTextureRegenerator( &srcU );
            CreateTexture( m_pTextureNameU, m_pTextureU, m_pTextureRegenU, IMAGE_FORMAT_I8,
                static_cast< int >( videoWidthUV ), static_cast< int >( videoHeightUV ) );
            m_pTextureRegenV = new CTextureRegenerator( &srcV );
            CreateTexture( m_pTextureNameV, m_pTextureV, m_pTextureRegenV, IMAGE_FORMAT_I8,
                static_cast< int >( videoWidthUV ), static_cast< int >( videoHeightUV ) );

            pVMTKeyValues->SetString( "$textureY", m_pTextureY->GetName() );
            pVMTKeyValues->SetString( "$textureU", m_pTextureU->GetName() );
            pVMTKeyValues->SetString( "$textureV", m_pTextureV->GetName() );
        }
        else
        {
            // 10/12-bit: six I8 planes - Y lo/hi, U lo/hi, V lo/hi.
            // Only I8 is used (proven by the working 8-bit path); no RGBA8888/IA88,
            // so no channel-order or alpha-range uncertainty in the engine's texture handling.
            m_pTextureRegenY = new CSplit16Regenerator( &srcY, VideoMaterialSIMD::SplitUint16Lo );
            CreateTexture( m_pTextureNameY, m_pTextureY, m_pTextureRegenY, IMAGE_FORMAT_I8,
                static_cast< int >( videoWidthY ), static_cast< int >( videoHeightY ) );
            m_pTextureRegenYHi = new CSplit16Regenerator( &srcY, VideoMaterialSIMD::SplitUint16Hi );
            CreateTexture( m_pTextureNameYHi, m_pTextureYHi, m_pTextureRegenYHi, IMAGE_FORMAT_I8,
                static_cast< int >( videoWidthY ), static_cast< int >( videoHeightY ) );

            m_pTextureRegenU = new CSplit16Regenerator( &srcU, VideoMaterialSIMD::SplitUint16Lo );
            CreateTexture( m_pTextureNameU, m_pTextureU, m_pTextureRegenU, IMAGE_FORMAT_I8,
                static_cast< int >( videoWidthUV ), static_cast< int >( videoHeightUV ) );
            m_pTextureRegenUHi = new CSplit16Regenerator( &srcU, VideoMaterialSIMD::SplitUint16Hi );
            CreateTexture( m_pTextureNameUHi, m_pTextureUHi, m_pTextureRegenUHi, IMAGE_FORMAT_I8,
                static_cast< int >( videoWidthUV ), static_cast< int >( videoHeightUV ) );

            m_pTextureRegenV = new CSplit16Regenerator( &srcV, VideoMaterialSIMD::SplitUint16Lo );
            CreateTexture( m_pTextureNameV, m_pTextureV, m_pTextureRegenV, IMAGE_FORMAT_I8,
                static_cast< int >( videoWidthUV ), static_cast< int >( videoHeightUV ) );
            m_pTextureRegenVHi = new CSplit16Regenerator( &srcV, VideoMaterialSIMD::SplitUint16Hi );
            CreateTexture( m_pTextureNameVHi, m_pTextureVHi, m_pTextureRegenVHi, IMAGE_FORMAT_I8,
                static_cast< int >( videoWidthUV ), static_cast< int >( videoHeightUV ) );

            pVMTKeyValues->SetString( "$textureY", m_pTextureY->GetName() );
            pVMTKeyValues->SetString( "$textureYhi", m_pTextureYHi->GetName() );
            pVMTKeyValues->SetString( "$textureU", m_pTextureU->GetName() );
            pVMTKeyValues->SetString( "$textureUhi", m_pTextureUHi->GetName() );
            pVMTKeyValues->SetString( "$textureV", m_pTextureV->GetName() );
            pVMTKeyValues->SetString( "$textureVhi", m_pTextureVHi->GetName() );
        }

        pVMTKeyValues->SetInt( "$bitdepth", bitDepth );
        pVMTKeyValues->SetInt( "$colorrange", colorRange );
        pVMTKeyValues->SetInt( "$colorspace", colorSpace );
        pVMTKeyValues->SetInt( "$colortransferc", colorTransferC );
        pVMTKeyValues->SetInt( "$colorprimaries", colorPrimaries );
        pVMTKeyValues->SetInt( "$nobasetexture", 1 );
        pVMTKeyValues->SetInt( "$nolod", 1 );
        pVMTKeyValues->SetInt( "$nomip", 1 );
        pVMTKeyValues->SetInt( "$nofog", 1 );
        pVMTKeyValues->SetInt( "$translucent", 0 );
        pVMTKeyValues->SetInt( "$vertexcolor", 0 );
        pVMTKeyValues->SetInt( "$vertexalpha", 0 );
        pVMTKeyValues->SetInt( "$gammacolorread", 0 );
        pVMTKeyValues->SetInt( "$spriteorientation", 3 );
        m_pMaterial = g_pMaterialSystem->CreateMaterial( "videoFFmpeg_background", pVMTKeyValues );
        m_pMaterial->Refresh();
    }

    /** @return The material ready for rendering. */
    IMaterial *GetMaterial() noexcept { return m_pMaterial; }

    /** @brief Update textures with new YUV data. */
    void Update() noexcept {
        m_pTextureY->Download();
        m_pTextureYHi->Download();
        if ( m_pTextureUHi )
        {
            // 10/12-bit: six planes
            m_pTextureU->Download();
            m_pTextureUHi->Download();
            m_pTextureV->Download();
            m_pTextureVHi->Download();
        }
        else
        {
            // 8-bit: three planes (m_pTextureYHi also exists but is unused; safe to skip)
            m_pTextureU->Download();
            m_pTextureV->Download();
        }
    }

private:
    ITextureRegenerator *m_pTextureRegenY{ nullptr };
    ITextureRegenerator *m_pTextureRegenYHi{ nullptr };
    ITextureRegenerator *m_pTextureRegenU{ nullptr };
    ITextureRegenerator *m_pTextureRegenUHi{ nullptr };
    ITextureRegenerator *m_pTextureRegenV{ nullptr };
    ITextureRegenerator *m_pTextureRegenVHi{ nullptr };
    const char *m_pTextureNameY{ "videoFFmpeg_background_y" };
    const char *m_pTextureNameYHi{ "videoFFmpeg_background_yhi" };
    const char *m_pTextureNameU{ "videoFFmpeg_background_u" };
    const char *m_pTextureNameUHi{ "videoFFmpeg_background_uhi" };
    const char *m_pTextureNameV{ "videoFFmpeg_background_v" };
    const char *m_pTextureNameVHi{ "videoFFmpeg_background_vhi" };
    ITexture *m_pTextureY{ nullptr };
    ITexture *m_pTextureYHi{ nullptr };
    ITexture *m_pTextureU{ nullptr };
    ITexture *m_pTextureUHi{ nullptr };
    ITexture *m_pTextureV{ nullptr };
    ITexture *m_pTextureVHi{ nullptr };
    IMaterial *m_pMaterial{ nullptr };
};
