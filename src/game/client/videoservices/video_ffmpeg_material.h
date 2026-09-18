// SPDX-License-Identifier: MIT
// Copyright (c) 2025 Grimowy
/**
 * @file video_ffmpeg_material.h
 * Project   : FFmpeg Video Services for Valve Source Engine
 * Component : Video Material Module
 */

#pragma once

#include "video_ffmpeg_material_simd.h"

#include "materialsystem/imaterial.h"           // For IMaterial
#include "materialsystem/itexture.h"            // For ITexture
#include "materialsystem/MaterialSystemUtil.h"  // For ITextureRegenerator
#include "keyvalues.h"                          // For KeyValues

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <d3d9.h>
#include <initguid.h>
#include <dxva2api.h>
#include "rd_d3d9hook.h"

#undef GetObject
#undef GetClassName
#undef PostMessage
#undef PropertySheet
#undef GetMessage
#undef SendMessage
#undef SetWindowLong
#undef GetWindowLong
#endif


#include <cstdint>								// For uint8_t

#ifdef _WIN32
// DXVA2 16.16 fixed-point helper (DXVA2_Fixed32FromFloat is not available in
// the Windows SDK headers - it comes from the DirectX SDK).
static inline DXVA2_Fixed32 VideoFixed32FromFloat( float f ) noexcept
{
    DXVA2_Fixed32 v;
    v.ll = static_cast< LONG >( f * 65536.0f );
    return v;
}

static inline float VideoFixed32ToFloat( DXVA2_Fixed32 v ) noexcept
{
    return static_cast< float >( v.ll ) / 65536.0f;
}

// DXVA2_ExtendedFormat bitfield values for BT.2020 / PQ. The Windows SDK's
// dxva2api.h enum stops at BT.709 / SMPTE_C / 28, but the fields are wider
// (matrix:3, primaries:5, transfer:5 bits) and Media Foundation drives the
// same DXVA2 processor with these exact values for HDR10:
//   MFVideoTransferMatrix_BT2020_10 = 4, _BT2020_12 = 5
//   MFVideoPrimaries_BT2020         = 9
//   MFVideoTransFunc_2084           = 15
// Modern (WDDM 2.x) drivers honor them; older drivers reject them at
// CreateVideoProcessor, which cleanly falls back to the software path.
#define DXVA2_VIDEO_TRANSFER_MATRIX_BT2020_10 4
#define DXVA2_VIDEO_TRANSFER_MATRIX_BT2020_12 5
#define DXVA2_VIDEO_PRIMARIES_BT2020          9
#define DXVA2_VIDEO_TRANS_FUNC_2084           15

// True when the FFmpeg transfer characteristic requires the HDR path
// (PQ/SMPTE2084 or HLG/ARIB_STD_B67). The shader needs this decision
// (SRC_RGB + IS_HDR) instead of re-running the whole YUV conversion.
static inline bool VideoIsHDRTransfer( int colorTransferC ) noexcept
{
    return colorTransferC == 16 || colorTransferC == 18; // AVCOL_TRC_SMPTE2084 / ARIB_STD_B67
}

// Maps FFmpeg color metadata
// AVColorTransferCharacteristic / AVColorPrimaries) onto DXVA2_ExtendedFormat.
// The processor MUST pick a YUV->RGB matrix, so BT.2020/PQ content is described
// explicitly (extended values above) instead of leaving it "Unknown" - that
// would make the driver fall back to a BT.601/BT.709 default and convert with
// the wrong matrix. The VideoYUV shader still owns the PQ EOTF / tone mapping /
// BT.2020->BT.709 display conversion.
static inline DXVA2_ExtendedFormat VideoColorToDXVA2( int colorRange, int colorSpace, int colorTransferC, int colorPrimaries ) noexcept
{
    DXVA2_ExtendedFormat fmt{};
    fmt.SampleFormat = DXVA2_SampleProgressiveFrame;
    fmt.VideoChromaSubsampling = DXVA2_VideoChromaSubsampling_MPEG2; // 4:2:0
    fmt.VideoLighting = DXVA2_VideoLighting_Unknown;

    switch ( colorRange )
    {
        case 1: fmt.NominalRange = DXVA2_NominalRange_16_235; break; // AVCOL_RANGE_MPEG
        case 2: fmt.NominalRange = DXVA2_NominalRange_0_255;  break; // AVCOL_RANGE_JPEG
        default: fmt.NominalRange = DXVA2_NominalRange_Unknown; break;
    }

    switch ( colorSpace )
    {
        case 1: fmt.VideoTransferMatrix = DXVA2_VideoTransferMatrix_BT709; break;    // AVCOL_SPC_BT709
        case 5: // AVCOL_SPC_BT470BG
        case 6: fmt.VideoTransferMatrix = DXVA2_VideoTransferMatrix_BT601; break;    // AVCOL_SPC_SMPTE170M
        case 7: fmt.VideoTransferMatrix = DXVA2_VideoTransferMatrix_SMPTE240M; break; // AVCOL_SPC_SMPTE240M
        case 9: // AVCOL_SPC_BT2020_NCL
        case 10: fmt.VideoTransferMatrix = DXVA2_VideoTransferMatrix_BT709; break; // AVCOL_SPC_BT2020_CL
        default: fmt.VideoTransferMatrix = DXVA2_VideoTransferMatrix_Unknown; break;
    }

    switch ( colorTransferC )
    {
        case 1: // AVCOL_TRC_BT709
        case 6: fmt.VideoTransferFunction = DXVA2_VideoTransFunc_709; break; // AVCOL_TRC_SMPTE170M
        case 4: fmt.VideoTransferFunction = DXVA2_VideoTransFunc_22;  break; // AVCOL_TRC_GAMMA22
        case 5: fmt.VideoTransferFunction = DXVA2_VideoTransFunc_28;  break; // AVCOL_TRC_GAMMA28
        case 7: fmt.VideoTransferFunction = DXVA2_VideoTransFunc_240M; break; // AVCOL_TRC_SMPTE240M
        case 13: fmt.VideoTransferFunction = DXVA2_VideoTransFunc_sRGB; break; // AVCOL_TRC_IEC61966_2_1
        case 16: fmt.VideoTransferFunction = DXVA2_VideoTransFunc_10; break;  // AVCOL_TRC_SMPTE2084 (PQ)
        default: fmt.VideoTransferFunction = DXVA2_VideoTransFunc_10; break; // incl. HLG/ARIB_STD_B67
    }

    switch ( colorPrimaries )
    {
        case 1: fmt.VideoPrimaries = DXVA2_VideoPrimaries_BT709; break;        // AVCOL_PRI_BT709
        case 4: fmt.VideoPrimaries = DXVA2_VideoPrimaries_BT470_2_SysM; break; // AVCOL_PRI_BT470M
        case 5: fmt.VideoPrimaries = DXVA2_VideoPrimaries_BT470_2_SysBG; break; // AVCOL_PRI_BT470BG
        case 6: fmt.VideoPrimaries = DXVA2_VideoPrimaries_SMPTE170M; break;    // AVCOL_PRI_SMPTE170M
        case 7: fmt.VideoPrimaries = DXVA2_VideoPrimaries_SMPTE240M; break;    // AVCOL_PRI_SMPTE240M
        case 9: fmt.VideoPrimaries = DXVA2_VideoPrimaries_BT709; break;      // AVCOL_PRI_BT2020
        default: fmt.VideoPrimaries = DXVA2_VideoPrimaries_Unknown; break;
    }

    return fmt;
}
#endif


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
  * @class CUVInterleaveRegenerator
  * @brief Interleaves U and V uint16 LE planes into BGRA8888 texels (10/12-bit path).
  *        Texel memory = [Ulo][Uhi][Vlo][Vhi], which maps 1:1 to D3D A8R8G8B8
  *        (B=Ulo, G=Uhi, R=Vlo, A=Vhi) with no byte swap. Branchless SIMD interleave.
  * srcU/srcV - Pointers to externally managed planar data pointers.
  */
class CUVInterleaveRegenerator final : public ITextureRegenerator {
public:
    CUVInterleaveRegenerator( uint8_t **srcU, uint8_t **srcV ) noexcept
        : m_srcU( srcU ), m_srcV( srcV ) {
    }

    void RegenerateTextureBits( ITexture *, IVTFTexture *dst, Rect_t * ) noexcept override {
        VideoMaterialSIMD::InterleaveUV( *m_srcU, *m_srcV, dst->ImageData(), dst->FaceSizeInBytes( 0 ) );
    }

    void Release() noexcept override {
        delete this;
    }

private:
    uint8_t **m_srcU{ nullptr }; // Pointer to externally managed U plane data pointer.
    uint8_t **m_srcV{ nullptr }; // Pointer to externally managed V plane data pointer.
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
        ReleaseHWResources();
        if ( m_pTextureRGB )
        {
            m_pTextureRGB->DecrementReferenceCount();
            m_pTextureRGB->DeleteIfUnreferenced();
            m_pTextureRGB = nullptr;
        }
        m_bHardware = false;
        m_videoWidth = 0;
        m_videoHeight = 0;
        m_inputD3DFormat = D3DFMT_UNKNOWN;
        m_rtD3DFormat = D3DFMT_UNKNOWN;

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
        ShutdownTexture( m_pTextureNameUV, m_pTextureUV, m_pTextureRegenUV );
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
            // 10/12-bit: Y as two I8 split planes (lo/hi) + U/V interleaved into one BGRA8888.
            // BGRA8888 memory [B][G][R][A] == D3D A8R8G8B8 (no swap); texel = [Ulo][Uhi][Vlo][Vhi].
            m_pTextureRegenY = new CSplit16Regenerator( &srcY, VideoMaterialSIMD::SplitUint16Lo );
            CreateTexture( m_pTextureNameY, m_pTextureY, m_pTextureRegenY, IMAGE_FORMAT_I8,
                static_cast< int >( videoWidthY ), static_cast< int >( videoHeightY ) );
            m_pTextureRegenYHi = new CSplit16Regenerator( &srcY, VideoMaterialSIMD::SplitUint16Hi );
            CreateTexture( m_pTextureNameYHi, m_pTextureYHi, m_pTextureRegenYHi, IMAGE_FORMAT_I8,
                static_cast< int >( videoWidthY ), static_cast< int >( videoHeightY ) );

            m_pTextureRegenUV = new CUVInterleaveRegenerator( &srcU, &srcV );
            CreateTexture( m_pTextureNameUV, m_pTextureUV, m_pTextureRegenUV, IMAGE_FORMAT_BGRA8888,
                static_cast< int >( videoWidthUV ), static_cast< int >( videoHeightUV ) );

            pVMTKeyValues->SetString( "$textureY", m_pTextureY->GetName() );
            pVMTKeyValues->SetString( "$textureYhi", m_pTextureYHi->GetName() );
            pVMTKeyValues->SetString( "$textureUV", m_pTextureUV->GetName() );
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
        if ( m_pTextureUV )
        {
            // 10/12-bit: Y lo/hi + UV (BGRA8888)
            m_pTextureYHi->Download();
            m_pTextureUV->Download();
        }
        else
        {
            // 8-bit: three I8 planes
            m_pTextureU->Download();
            m_pTextureV->Download();
        }
    }

    // -------------------------------------------------------------------
    // Hardware (DXVA2) path
    // -------------------------------------------------------------------

    /**
     * @brief Initializes the hardware video path: Source RGB render target,
     *        DXVA2 video processor and the RGB material.
     * @param pDevice     - the game's IDirect3DDevice9.
     * @param pDevMgr     - IDirect3DDeviceManager9 bound to the game device.
     * @param width,height- video frame size.
     * @param inputFormat - D3D format of the decoder surfaces (NV12/P010/...).
     * @param colorRange/colorSpace/colorTransferC/colorPrimaries - FFmpeg values.
     */
    bool InitHW( IDirect3DDevice9 *pDevice, IDirect3DDeviceManager9 *pDevMgr,
        D3DFORMAT inputFormat, int bitDepth, int width, int height,
        int colorRange, int colorSpace, int colorTransferC, int colorPrimaries ) noexcept
    {
        m_bHardware = true;
        m_videoWidth = width;
        m_videoHeight = height;
        m_inputD3DFormat = inputFormat;

        // 1. Build the video description (input format comes from the decoder).
        //    Color metadata (range/matrix/primaries/transfer) is mapped from the
        //    FFmpeg values supplied by the player. DXVA2 has no BT.2020
        //    primaries/matrix and no PQ/ST.2084 transfer - those stay "Unknown"
        //    here and are handled by the VideoYUV shader.
        DXVA2_VideoDesc desc{};
        desc.SampleWidth = m_videoWidth;
        desc.SampleHeight = m_videoHeight;
        desc.SampleFormat = VideoColorToDXVA2( colorRange, colorSpace, colorTransferC, colorPrimaries );
        desc.Format = inputFormat;
        m_videoDesc = desc;
        m_sourceColorFormat = desc.SampleFormat;

        // Destination color space for VideoProcessBlt::DestFormat - it must
        // describe the OUTPUT, not copy the source metadata:
        //   SDR -> ask the processor for non-linear BT.709 RGB, so the shader
        //          is a straight passthrough (SRC_RGB, no IS_HDR branch).
        //   HDR -> ask for LINEAR RGB in the source gamut
        //          (DXVA2_VideoTransFunc_10 = gamma 1.0), so the shader only
        //          tone maps, maps to BT.709 and applies the output transfer.
        m_destColorFormat = m_sourceColorFormat;
        m_destColorFormat.NominalRange = DXVA2_NominalRange_0_255; // RGB output is full range
        if ( VideoIsHDRTransfer( colorTransferC ) )
        {
            m_destColorFormat.VideoTransferFunction = DXVA2_VideoTransFunc_10; // linear
        }
        else
        {
            m_destColorFormat.VideoPrimaries = DXVA2_VideoPrimaries_BT709;
            m_destColorFormat.VideoTransferFunction = DXVA2_VideoTransFunc_709;
        }

        // 2. Pick a video processor GUID that accepts the decoder surface
        //    format and offers the best RGB(A) render target format.
        IDirectXVideoProcessorService *pService = nullptr;
        HANDLE hDevice = INVALID_HANDLE_VALUE;
        HRESULT hr = pDevMgr->OpenDeviceHandle( &hDevice );
        if ( FAILED( hr ) || hDevice == INVALID_HANDLE_VALUE )
        {
            Warning( "VideoSEMaterial::InitHW: OpenDeviceHandle failed hr=0x%08X\n", ( unsigned int )hr );
            Reset();
            return false;
        }
        hr = pDevMgr->GetVideoService( hDevice, IID_IDirectXVideoProcessorService,
            reinterpret_cast< void ** >( &pService ) );
        pDevMgr->CloseDeviceHandle( hDevice );
        if ( FAILED( hr ) || !pService )
        {
            Warning( "VideoSEMaterial::InitHW: GetVideoService failed hr=0x%08X\n", ( unsigned int )hr );
            Reset();
            return false;
        }

        UINT nGuids = 0;
        GUID *pGuids = nullptr;
        hr = pService->GetVideoProcessorDeviceGuids( &desc, &nGuids, &pGuids );
        if ( FAILED( hr ) || nGuids == 0 )
        {
            pService->Release();
            Reset();
            return false;
        }

        D3DFORMAT outputFormat = D3DFMT_UNKNOWN;
        GUID chosenGUID = pGuids[ 0 ];
        for ( UINT i = 0; i < nGuids; ++i )
        {
            UINT nRtFormats = 0;
            D3DFORMAT *pRtFormats = nullptr;

            HRESULT hrRT = pService->GetVideoProcessorRenderTargets(
                pGuids[ i ],
                &desc,
                &nRtFormats,
                &pRtFormats
            );

            if ( SUCCEEDED( hrRT ) )
            {
                Warning( "VP %u offers %u RT formats:\n", i, nRtFormats );

                for ( UINT j = 0; j < nRtFormats; ++j )
                {
                    Warning(
                        "  [%u] 0x%08X\n",
                        j,
                        (unsigned int)pRtFormats[ j ]
                    );
                }

                CoTaskMemFree( pRtFormats );
            }
        }
        for ( UINT i = 0; i < nGuids; ++i )
        {
            UINT nRtFormats = 0;
            D3DFORMAT *pRtFormats = nullptr;
            if ( FAILED( pService->GetVideoProcessorRenderTargets( pGuids[ i ], &desc, &nRtFormats, &pRtFormats ) ) )
                continue;
            D3DFORMAT best = D3DFMT_UNKNOWN;
            for ( UINT j = 0; j < nRtFormats; ++j )
            {
                if ( pRtFormats[ j ] == D3DFMT_A2B10G10R10 )
                {
                    best = D3DFMT_A2B10G10R10;
                    break;
                }
                if ( best == D3DFMT_UNKNOWN
                    && ( pRtFormats[ j ] == D3DFMT_A8R8G8B8 || pRtFormats[ j ] == D3DFMT_X8R8G8B8 ) )
                {
                    best = pRtFormats[ j ];
                }
            }
            CoTaskMemFree( pRtFormats );
            if ( best != D3DFMT_UNKNOWN )
            {
                chosenGUID = pGuids[ i ];
                outputFormat = best;
                break;
            }
        }
        CoTaskMemFree( pGuids );
        pService->Release();
        if ( outputFormat == D3DFMT_UNKNOWN )
        {
            Warning( "VideoSEMaterial::InitHW: no video processor for input format 0x%08X\n", ( unsigned int )inputFormat );
            Reset();
            return false;
        }
        m_videoProcessorGUID = chosenGUID;
        m_rtD3DFormat = outputFormat;

        // 3. Create the Source RGB render target (A16B16G16R16F preferred,
        //    A8R8G8B8 / X8R8G8B8 fallback). Render targets must be allocated
        //    inside the material system's allocation block.
        ImageFormat imageFormat = IMAGE_FORMAT_RGBA8888;
        if ( outputFormat == D3DFMT_A2B10G10R10 )
            imageFormat = IMAGE_FORMAT_BGRA1010102;
        else if ( outputFormat == D3DFMT_X8R8G8B8 )
            imageFormat = IMAGE_FORMAT_RGB888;

        g_pMaterialSystem->BeginRenderTargetAllocation();
        m_pTextureRGB = g_pMaterialSystem->CreateNamedRenderTargetTextureEx2(
            "videoFFmpeg_background_rgb", m_videoWidth, m_videoHeight, RT_SIZE_NO_CHANGE, imageFormat,
            MATERIAL_RT_DEPTH_SHARED,
            TEXTUREFLAGS_RENDERTARGET | TEXTUREFLAGS_NOMIP | TEXTUREFLAGS_NOLOD | TEXTUREFLAGS_CLAMPS | TEXTUREFLAGS_CLAMPT,
            0 );
        g_pMaterialSystem->EndRenderTargetAllocation();
        if ( !m_pTextureRGB )
        {
            Warning( "VideoSEMaterial::InitHW: CreateNamedRenderTargetTextureEx2 failed\n" );
            Reset();
            return false;
        }
        m_pTextureRGB->IncrementReferenceCount();

        // 4. RGB material (VideoYUV shader, SRC_RGB path).
        KeyValues *pVMTKeyValues = new KeyValues( "VideoYUV" );
        pVMTKeyValues->SetString( "$textureRGB", m_pTextureRGB->GetName() );
        pVMTKeyValues->SetInt( "$rgbinput", 1 );
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

        // 5. DXVA2 video processor on the game device.
        if ( !CreateVideoProcessor( pDevice, pDevMgr ) )
        {
            Warning( "VideoSEMaterial::InitHW: CreateVideoProcessor failed\n" );
            Reset();
            return false;
        }

        // 6. Native RT surface via the SetRenderTarget hook.
        if ( !AcquireRenderTargetSurface() )
        {
            Warning( "VideoSEMaterial::InitHW: could not capture native RT surface\n" );
            Reset();
            return false;
        }

        // Verify the Source RT was really created with the selected D3D format
        // (the ImageFormat->D3DFORMAT table lives in the prebuilt shaderapi).
        D3DSURFACE_DESC rtDesc{};
        const bool bGotDesc = m_pSurfaceRGB && SUCCEEDED( m_pSurfaceRGB->GetDesc( &rtDesc ) );
        if ( bGotDesc && rtDesc.Format != m_rtD3DFormat )
        {
            Warning( "  [video] HW path: Source RT format mismatch (requested 0x%08X, got 0x%08X)\n",
                ( unsigned int )m_rtD3DFormat, ( unsigned int )rtDesc.Format );
        }
        Msg( "  [video] HW path: %ux%u input fmt=0x%08X RT fmt=0x%08X surface fmt=0x%08X (ImageFormat %d)\n",
            m_videoWidth, m_videoHeight, ( unsigned int )inputFormat,
            ( unsigned int )m_rtD3DFormat, bGotDesc ? ( unsigned int )rtDesc.Format : 0, ( int )imageFormat );
        return true;
    }

    /** @return True when the hardware (DXVA2) path is active. */
    bool IsHardware() const noexcept { return m_bHardware; }

    /** @brief Blits one DXVA2 decoder surface into the RGB render target.
     *  @return true when the blit succeeded, false on failure. */
    bool UpdateHW( IDirect3DSurface9 *pDxvaSurface ) noexcept
    {
        if ( !m_bHardware || !m_pVideoProcessor || !m_pSurfaceRGB || !pDxvaSurface )
            return false;

        // A processor that requires reference samples must receive them even
        // for progressive content - a single-sample blit would be invalid.
        if ( m_vpNumForwardRefSamples > 0 || m_vpNumBackwardRefSamples > 0 )
        {
            static bool s_bRefsReported = false;
            if ( !s_bRefsReported )
            {
                Warning( "[video] VP requires %u forward / %u backward reference samples - single-sample blit may be invalid\n",
                    m_vpNumForwardRefSamples, m_vpNumBackwardRefSamples );
                s_bRefsReported = true;
            }
        }

        // Minimal progressive sample: Microsoft requires only Start/End,
        // SampleFormat, SrcSurface, SrcRect, DstRect and PlanarAlpha.
        RECT rc = { 0, 0, m_videoWidth, m_videoHeight };

        DXVA2_VideoSample sample{};
        sample.Start = 0;
        sample.End = 333333; // nominal 1/30 s; progressive frame
        sample.SampleFormat = m_sourceColorFormat;
        sample.SrcSurface = pDxvaSurface;
        sample.SrcRect = rc;
        sample.DstRect = rc;
        sample.PlanarAlpha = VideoFixed32FromFloat( 1.0f );

        DXVA2_VideoProcessBltParams blt{};
        blt.TargetFrame = sample.Start;
        blt.TargetRect = rc;
        blt.StreamingFlags = 0;
        blt.BackgroundColor.Alpha = 0xFFFF;

        // Minimal DestFormat: only the progressive sample flag. The remaining
        // fields stay Unknown - full BT.2020/HDR metadata is being validated
        // separately (it can cause E_INVALIDARG on some drivers).
        blt.DestFormat.SampleFormat = DXVA2_SampleProgressiveFrame;

        // Use the processor's actual ProcAmp defaults (values must be within
        // GetProcAmpRange; hardcoded 0/1 can be rejected with E_INVALIDARG).
        blt.ProcAmpValues.Brightness = m_procAmpBrightness;
        blt.ProcAmpValues.Contrast   = m_procAmpContrast;
        blt.ProcAmpValues.Hue        = m_procAmpHue;
        blt.ProcAmpValues.Saturation = m_procAmpSaturation;

        blt.Alpha = VideoFixed32FromFloat( 1.0f );

        // Diagnostic (once): confirm the actual SRC/DST surface properties.
        // Microsoft requires the destination to be a render target and
        // TargetRect must not exceed the destination surface.
        static bool s_bSurfacesReported = false;
        if ( !s_bSurfacesReported )
        {
            D3DSURFACE_DESC srcDesc{};
            D3DSURFACE_DESC dstDesc{};
            const HRESULT hrSrc = pDxvaSurface->GetDesc( &srcDesc );
            const HRESULT hrDst = m_pSurfaceRGB->GetDesc( &dstDesc );
            if ( SUCCEEDED( hrSrc ) && SUCCEEDED( hrDst ) )
            {
                Msg( "[video] BLT surfaces: SRC=%ux%u fmt=0x%08X pool=%d usage=0x%08X | DST=%ux%u fmt=0x%08X pool=%d usage=0x%08X%s\n",
                    srcDesc.Width, srcDesc.Height, ( unsigned int )srcDesc.Format, ( int )srcDesc.Pool, srcDesc.Usage,
                    dstDesc.Width, dstDesc.Height, ( unsigned int )dstDesc.Format, ( int )dstDesc.Pool, dstDesc.Usage,
                    ( dstDesc.Usage & D3DUSAGE_RENDERTARGET ) ? "" : "  <-- DST IS NOT A RENDER TARGET!" );
                if ( !( dstDesc.Usage & D3DUSAGE_RENDERTARGET ) )
                    Warning( "[video] BLT: destination surface is NOT a render target (usage=0x%08X)\n", dstDesc.Usage );
                if ( dstDesc.Width < static_cast< UINT >( m_videoWidth ) || dstDesc.Height < static_cast< UINT >( m_videoHeight ) )
                    Warning( "[video] BLT: destination surface smaller than the video frame (%ux%u < %ux%u)\n",
                        dstDesc.Width, dstDesc.Height, m_videoWidth, m_videoHeight );
            }
            else
            {
                Warning( "[video] BLT: GetDesc failed src=0x%08X dst=0x%08X\n", ( unsigned int )hrSrc, ( unsigned int )hrDst );
            }
            s_bSurfacesReported = true;
        }

        HRESULT hr = m_pVideoProcessor->VideoProcessBlt( m_pSurfaceRGB, &blt, &sample, 1, nullptr );
        if ( FAILED( hr ) )
        {
            static bool s_bBltFailureReported = false;
            if ( !s_bBltFailureReported )
            {
                Warning( "VideoSEMaterial::UpdateHW: VideoProcessBlt failed hr=0x%08X\n", ( unsigned int )hr );
                s_bBltFailureReported = true;
            }
            return false;
        }
        return true;
    }

    /**
     * @brief Releases the native surface and video processor (device reset).
     * Keeps the logical Source ITexture - it is reacquired after the reset.
     */
    void ReleaseHWResources() noexcept
    {
        if ( m_pSurfaceRGB )
        {
            m_pSurfaceRGB->Release();
            m_pSurfaceRGB = nullptr;
        }
        if ( m_pVideoProcessor )
        {
            m_pVideoProcessor->Release();
            m_pVideoProcessor = nullptr;
        }
    }

    /** @brief Recreates video processor + native RT surface after device reset. */
    bool RecreateHWResources( IDirect3DDevice9 *pDevice, IDirect3DDeviceManager9 *pDevMgr ) noexcept
    {
        if ( !m_bHardware || !m_pTextureRGB )
            return false;
        if ( !CreateVideoProcessor( pDevice, pDevMgr ) )
            return false;
        return AcquireRenderTargetSurface();
    }

private:
    // -------------------------------------------------------------------
    // Hardware (DXVA2) internals
    // -------------------------------------------------------------------
    /** @brief Queries the processor's default ProcAmp value for one control.
     *  Falls back to a neutral value when the control is unsupported. */
    DXVA2_Fixed32 QueryProcAmpDefault( DWORD cap, DXVA2_Fixed32 fallback ) noexcept
    {
        DXVA2_ValueRange range{};
        HRESULT hr = m_pVideoProcessor->GetProcAmpRange( cap, &range );
        if ( SUCCEEDED( hr ) )
        {
            Msg( "[video] VP ProcAmp 0x%08X: min=%f max=%f default=%f step=%f\n",
                ( unsigned int )cap,
                VideoFixed32ToFloat( range.MinValue ),
                VideoFixed32ToFloat( range.MaxValue ),
                VideoFixed32ToFloat( range.DefaultValue ),
                VideoFixed32ToFloat( range.StepSize ) );
            return range.DefaultValue;
        }
        Warning( "[video] VP: GetProcAmpRange(0x%08X) failed hr=0x%08X\n", ( unsigned int )cap, ( unsigned int )hr );
        return fallback;
    }

    bool CreateVideoProcessor( IDirect3DDevice9 *pDevice, IDirect3DDeviceManager9 *pDevMgr ) noexcept
    {
        if ( m_pVideoProcessor )
            return true;

        IDirectXVideoProcessorService *pService = nullptr;
        HANDLE hDevice = INVALID_HANDLE_VALUE;
        HRESULT hr = pDevMgr->OpenDeviceHandle( &hDevice );
        if ( FAILED( hr ) || hDevice == INVALID_HANDLE_VALUE )
            return false;
        hr = pDevMgr->GetVideoService( hDevice, IID_IDirectXVideoProcessorService,
            reinterpret_cast< void ** >( &pService ) );
        pDevMgr->CloseDeviceHandle( hDevice );
        if ( FAILED( hr ) || !pService )
            return false;

        hr = pService->CreateVideoProcessor( m_videoProcessorGUID, &m_videoDesc, m_rtD3DFormat, 0, &m_pVideoProcessor );
        pService->Release();
        if ( FAILED( hr ) || !m_pVideoProcessor )
            return false;

        // Diagnostic: processor capabilities - required reference samples and
        // supported operations (a single-sample progressive blit is only valid
        // when the processor needs no forward/backward reference frames).
        DXVA2_VideoProcessorCaps caps{};
        hr = m_pVideoProcessor->GetVideoProcessorCaps( &caps );
        if ( SUCCEEDED( hr ) )
        {
            Msg( "[video] VP caps: DeviceCaps=0x%08X InputPool=%d ForwardRefs=%u BackwardRefs=%u ProcAmp=0x%08X Operations=0x%08X\n",
                ( unsigned int )caps.DeviceCaps, ( int )caps.InputPool,
                caps.NumForwardRefSamples, caps.NumBackwardRefSamples,
                ( unsigned int )caps.ProcAmpControlCaps, ( unsigned int )caps.VideoProcessorOperations );
            if ( caps.VideoProcessorOperations & DXVA2_VideoProcess_YUV2RGB )
                Msg( "[video] VP supports YUV2RGB\n" );
            else
                Warning( "[video] VP DOES NOT support YUV2RGB\n" );
            m_vpNumForwardRefSamples = caps.NumForwardRefSamples;
            m_vpNumBackwardRefSamples = caps.NumBackwardRefSamples;
        }
        else
        {
            Warning( "[video] VP: GetVideoProcessorCaps failed hr=0x%08X\n", ( unsigned int )hr );
        }

        // Actual ProcAmp defaults (the values must be within GetProcAmpRange;
        // hardcoded 0/1 can be rejected with E_INVALIDARG).
        m_procAmpBrightness = QueryProcAmpDefault( DXVA2_ProcAmp_Brightness, VideoFixed32FromFloat( 0.0f ) );
        m_procAmpContrast   = QueryProcAmpDefault( DXVA2_ProcAmp_Contrast,   VideoFixed32FromFloat( 1.0f ) );
        m_procAmpHue        = QueryProcAmpDefault( DXVA2_ProcAmp_Hue,        VideoFixed32FromFloat( 0.0f ) );
        m_procAmpSaturation = QueryProcAmpDefault( DXVA2_ProcAmp_Saturation, VideoFixed32FromFloat( 1.0f ) );

        // Confirm what the processor was actually created with.
        GUID creationGuid{};
        DXVA2_VideoDesc creationDesc{};
        D3DFORMAT creationFormat = D3DFMT_UNKNOWN;
        UINT maxSubStreams = 0;
        hr = m_pVideoProcessor->GetCreationParameters( &creationGuid, &creationDesc, &creationFormat, &maxSubStreams );
        if ( SUCCEEDED( hr ) )
        {
            Msg( "[video] VP creation: format=0x%08X maxSubStreams=%u\n",
                ( unsigned int )creationFormat, maxSubStreams );
        }
        else
        {
            Warning( "[video] VP: GetCreationParameters failed hr=0x%08X\n", ( unsigned int )hr );
        }
        return true;
    }

    bool AcquireRenderTargetSurface() noexcept
    {
        if ( !m_pTextureRGB )
            return false;

        // Bind the persistent Source ITexture and capture the native D3D9
        // surface through the SetRenderTarget hook (arm -> bind -> collect).
        // SetRenderTarget/GetRenderTarget live on IMatRenderContext.
        CMatRenderContextPtr context( g_pMaterialSystem );
        if ( !context )
            return false;

        ITexture *pOldRT = context->GetRenderTarget();

        RD_D3D9::BeginCaptureRenderTarget();
        context->SetRenderTarget( m_pTextureRGB );
        IDirect3DSurface9 *pSurface = RD_D3D9::EndCaptureRenderTarget();
        context->SetRenderTarget( pOldRT );

        if ( !pSurface )
            return false;

        if ( m_pSurfaceRGB )
            m_pSurfaceRGB->Release();
        m_pSurfaceRGB = pSurface;
        return true;
    }

    ITextureRegenerator *m_pTextureRegenY{ nullptr };
    ITextureRegenerator *m_pTextureRegenYHi{ nullptr };
    ITextureRegenerator *m_pTextureRegenU{ nullptr };
    ITextureRegenerator *m_pTextureRegenV{ nullptr };
    ITextureRegenerator *m_pTextureRegenUV{ nullptr };
    const char *m_pTextureNameY{ "videoFFmpeg_background_y" };
    const char *m_pTextureNameYHi{ "videoFFmpeg_background_yhi" };
    const char *m_pTextureNameU{ "videoFFmpeg_background_u" };
    const char *m_pTextureNameV{ "videoFFmpeg_background_v" };
    const char *m_pTextureNameUV{ "videoFFmpeg_background_uv" };
    ITexture *m_pTextureY{ nullptr };
    ITexture *m_pTextureYHi{ nullptr };
    ITexture *m_pTextureU{ nullptr };
    ITexture *m_pTextureV{ nullptr };
    ITexture *m_pTextureUV{ nullptr };
    IMaterial *m_pMaterial{ nullptr };

    // Hardware (DXVA2) state
    ITexture *m_pTextureRGB{ nullptr };
    IDirect3DSurface9 *m_pSurfaceRGB{ nullptr };
    IDirectXVideoProcessor *m_pVideoProcessor{ nullptr };
    GUID m_videoProcessorGUID{};
    DXVA2_VideoDesc m_videoDesc{};
    DXVA2_ExtendedFormat m_sourceColorFormat{};
    DXVA2_ExtendedFormat m_destColorFormat{};
    D3DFORMAT m_inputD3DFormat{ D3DFMT_UNKNOWN };
    D3DFORMAT m_rtD3DFormat{ D3DFMT_UNKNOWN };
    int m_videoWidth{ 0 };
    int m_videoHeight{ 0 };
    bool m_bHardware{ false };

    // Actual ProcAmp defaults reported by the processor (values must be within
    // GetProcAmpRange) and the required reference sample counts.
    DXVA2_Fixed32 m_procAmpBrightness{};
    DXVA2_Fixed32 m_procAmpContrast{};
    DXVA2_Fixed32 m_procAmpHue{};
    DXVA2_Fixed32 m_procAmpSaturation{};
    UINT m_vpNumForwardRefSamples{ 0 };
    UINT m_vpNumBackwardRefSamples{ 0 };
};
