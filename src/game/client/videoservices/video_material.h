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
  * @brief Regenerates procedural texture I8/UV88 bits from raw planar uint8_t/uint16_t Y/U/V buffer.
  * src - Pointer to externally managed planar data pointer: AVFrame uint8_t/uint16_t, 32/64-byte aligned
  */
class CTextureRegenerator final : public ITextureRegenerator {
public:
    explicit CTextureRegenerator( uint8_t **src ) noexcept
        : m_src( src ) {
    }

    void RegenerateTextureBits( ITexture *, IVTFTexture *dst, Rect_t * ) noexcept override {

        VideoMaterialSIMD::GetInstance().Memcpy( dst->ImageData(), *m_src, dst->FaceSizeInBytes( 0 ) );
        //memcpy( dst->ImageData(), *m_src, dst->FaceSizeInBytes(0) );
    }

    void Release() noexcept override {
        delete this;
    }

private:
    uint8_t **m_src{ nullptr }; // Pointer to externally managed planar data pointer.
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
        ShutdownTexture( m_pTextureNameU, m_pTextureU, m_pTextureRegenU );
        ShutdownTexture( m_pTextureNameV, m_pTextureV, m_pTextureRegenV );
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
        const int nFlags = TEXTUREFLAGS_PROCEDURAL | TEXTUREFLAGS_SINGLECOPY
            | TEXTUREFLAGS_NOMIP | TEXTUREFLAGS_NOLOD | TEXTUREFLAGS_CLAMPS | TEXTUREFLAGS_CLAMPT
            | ( ( bitDepth != 8 ) ? TEXTUREFLAGS_POINTSAMPLE : 0 );

        const ImageFormat fmt = ( bitDepth == 8 ) ? IMAGE_FORMAT_I8 : IMAGE_FORMAT_UV88;

        auto CreateTexture = [ & ]( const char *pTextureName, ITexture *&pTexture,
            ITextureRegenerator *&pTextureRegen, uint8_t **pSrc, int w, int h )
            {
                pTextureRegen = new CTextureRegenerator( pSrc );
                pTexture = g_pMaterialSystem->CreateProceduralTexture( pTextureName, "VideoFFmpegCacheTextures", w, h, fmt, nFlags );
                pTexture->IncrementReferenceCount();
                pTexture->SetTextureRegenerator( pTextureRegen );
            };
        CreateTexture( m_pTextureNameY, m_pTextureY, m_pTextureRegenY, &srcY,
            static_cast< int >( videoWidthY ), static_cast< int >( videoHeightY ) );
        CreateTexture( m_pTextureNameU, m_pTextureU, m_pTextureRegenU, &srcU,
            static_cast< int >( videoWidthUV ), static_cast< int >( videoHeightUV ) );
        CreateTexture( m_pTextureNameV, m_pTextureV, m_pTextureRegenV, &srcV,
            static_cast< int >( videoWidthUV ), static_cast< int >( videoHeightUV ) );

        KeyValues *pVMTKeyValues = new KeyValues( "VideoYUV" );
        pVMTKeyValues->SetString( "$textureY", m_pTextureY->GetName() );
        pVMTKeyValues->SetString( "$textureU", m_pTextureU->GetName() );
        pVMTKeyValues->SetString( "$textureV", m_pTextureV->GetName() );
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
        m_pTextureU->Download();
        m_pTextureV->Download();
    }

private:
    ITextureRegenerator *m_pTextureRegenY{ nullptr };
    ITextureRegenerator *m_pTextureRegenU{ nullptr };
    ITextureRegenerator *m_pTextureRegenV{ nullptr };
    const char *m_pTextureNameY{ "videoFFmpeg_background_y" };
    const char *m_pTextureNameU{ "videoFFmpeg_background_u" };
    const char *m_pTextureNameV{ "videoFFmpeg_background_v" };
    ITexture *m_pTextureY{ nullptr };
    ITexture *m_pTextureU{ nullptr };
    ITexture *m_pTextureV{ nullptr };
    IMaterial *m_pMaterial{ nullptr };
};
