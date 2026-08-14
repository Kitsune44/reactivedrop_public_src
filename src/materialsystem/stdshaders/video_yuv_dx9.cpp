// video_yuv_dx9.cpp

#include "BaseVSShader.h"
#include "cpp_shader_constant_register_map.h"

#include "video_yuv_vs30.inc"
#include "video_yuv_ps30.inc"

static float texSizes[ 1 ][ 4 ];

BEGIN_VS_SHADER( VideoYUV, "Planar YUV decoder with HDR support (PS3.0)" )
    BEGIN_SHADER_PARAMS
        SHADER_PARAM( textureY, SHADER_PARAM_TYPE_TEXTURE, "", "Y Plane" )
        SHADER_PARAM( textureU, SHADER_PARAM_TYPE_TEXTURE, "", "U Plane" )
        SHADER_PARAM( textureV, SHADER_PARAM_TYPE_TEXTURE, "", "V Plane" )
        SHADER_PARAM( bitdepth, SHADER_PARAM_TYPE_INTEGER, "", "8=8bit, 10=10bit, 12=12bit" )
        SHADER_PARAM( colorrange, SHADER_PARAM_TYPE_INTEGER, "", "1=Limited, 2=Full" )
        SHADER_PARAM( colorspace, SHADER_PARAM_TYPE_INTEGER, "", "FFmpeg: AVColorSpace" )
        SHADER_PARAM( colortransferc, SHADER_PARAM_TYPE_INTEGER, "", "FFmpeg: AVColorTransferCharacteristic" )
        SHADER_PARAM( colorprimaries, SHADER_PARAM_TYPE_INTEGER, "", "FFmpeg: AVColorPrimaries" )
    END_SHADER_PARAMS

    SHADER_INIT_PARAMS()
    {
        if ( !params[ textureY ]->IsDefined() ) {
            Warning( "Shader 'video_yuv': undefined $textureY.\n" );
        }
        if ( !params[ textureU ]->IsDefined() ) {
            Warning( "Shader 'video_yuv': undefined $textureU.\n" );
        }
        if ( !params[ textureV ]->IsDefined() ) {
            Warning( "Shader 'video_yuv': undefined $textureV.\n" );
        }

        if ( !params[ bitdepth ]->IsDefined() ) {
            params[ bitdepth ]->SetIntValue( 8 );
            Warning( "Shader 'video_yuv': undefined $bitdepth. FALLBACK: 8 (8-bit).\n" );
        }
        else {
            int val = params[ bitdepth ]->GetIntValue();
            int encoded = 0;
            switch ( val ) {
                case  8: encoded = 0; break; // 8-bit
                case 10: encoded = 1; break; // 10-bit
                case 12: encoded = 2; break; // 12-bit
                default: encoded = 0; break; // Default to 8-bit
            }
            params[ bitdepth ]->SetIntValue( encoded );
        }

        if ( !params[ colorrange ]->IsDefined() ) {
            params[ colorrange ]->SetIntValue( 1 );
            Warning( "Shader 'video_yuv': undefined $colorrange. FALLBACK: 1 (AVCOL_RANGE_MPEG (Studio/Limited range)).\n" );
        }
        else {
            int val = params[ colorrange ]->GetIntValue();
            int encoded = 0;
            switch ( val ) {
                case  0: encoded = 0;           ///< AVCOL_RANGE_UNSPECIFIED
                    Warning( "\nShader 'video_yuv': $colorrange = 0 (AVCOL_RANGE_UNSPECIFIED). FALLBACK: 1 (AVCOL_RANGE_MPEG (Studio/Limited range)).\n" );
                    break;
                case  1: encoded = 0; break;    ///< AVCOL_RANGE_MPEG    Studio Range (Limited Range)
                case  2: encoded = 1; break;    ///< AVCOL_RANGE_JPEG    Full Range
                default: encoded = 0; break;    ///< Default AVCOL_RANGE_MPEG
            }
            params[ colorrange ]->SetIntValue( encoded );
        }

        if ( !params[ colorspace ]->IsDefined() ) {
            params[ colorspace ]->SetIntValue( 1 );
            Warning( "Shader 'video_yuv': undefined $colorspace. FALLBACK: 1 (AVCOL_SPC_BT709).\n" );
        }
        else {
            int val = params[ colorspace ]->GetIntValue();
            int encoded = 0;
            switch ( val ) {
                case  0: encoded = 0; break;    ///< AVCOL_SPC_RGB          order of coefficients is actually GBR, also IEC 61966-2-1 (sRGB), YZX and ST 428-1
                case  1: encoded = 1; break;    ///< AVCOL_SPC_BT709        ITU-R BT709 / ITU-R BT1361 / IEC 61966-2-4 xvYCC709 / derived in SMPTE RP 177 Annex B
                case  2: encoded = 1;           ///< AVCOL_SPC_UNSPECIFIED
                    Warning( "Shader 'video_yuv': $colorspace = 2 (AVCOL_SPC_UNSPECIFIED). FALLBACK: 1 (AVCOL_SPC_BT709).\n" );
                    break;
                case  3: encoded = 1;           ///< AVCOL_SPC_RESERVED     reserved for future use by ITU-T and ISO/IEC just like 15-255 are
                    Warning( "Shader 'video_yuv': $colorspace = 3 (AVCOL_SPC_RESERVED). FALLBACK: 1 (AVCOL_SPC_BT709).\n" );
                    break;
                case  4: encoded = 2; break;    ///< AVCOL_SPC_FCC          FCC Title 47 Code of Federal Regulations 73.682 (a)(20)
                case  5:                        ///< AVCOL_SPC_BT470BG      ITU-R BT470BG / ITU-R BT601-6 625 / ITU-R BT1358 625 / ITU-R BT1700 625 PAL & SECAM / IEC 61966-2-4 xvYCC601
                case  6: encoded = 3; break;    ///< AVCOL_SPC_SMPTE170M    SMPTE ST 170M / ITU-R BT601-6 525 / ITU-R BT1358 525 / ITU-R BT1700 NTSC / functionally identical to above
                case  7: encoded = 4; break;    ///< AVCOL_SPC_SMPTE240M    derived from 170M primaries and D65 white point, 170M is derived from BT470 System M's primaries
                case  9:                        ///< AVCOL_SPC_BT2020_NCL   ITU-R BT2020 non-constant luminance system
                case 10: encoded = 5; break;    ///< AVCOL_SPC_BT2020_CL    ITU-R BT2020 constant luminance system
                default:
                    Warning( "\nShader 'video_yuv': $colorspace = %d. Unsupported.\n", val );
                    return;
            }
            params[ colorspace ]->SetIntValue( encoded );
            //Warning( "\ncolorspace = %d, encoded = %d\n", val, encoded );
        }

        if ( !params[ colortransferc ]->IsDefined() ) {
            params[ colortransferc ]->SetIntValue( 1 );
            Warning( "Shader 'video_yuv': undefined $colortransferc. FALLBACK: 1 (AVCOL_TRC_BT709).\n" );
        }
        else {
            int val = params[ colortransferc ]->GetIntValue();
            int encoded = 0;
            switch ( val ) {
                case  0: encoded =  0;          ///< AVCOL_TRC_RESERVED0
                    Warning( "Shader 'video_yuv': $colortransferc = 0 (AVCOL_TRC_RESERVED0). FALLBACK: 1 (AVCOL_TRC_BT709).\n" );
                    break;
                case  1: encoded =  0; break;   ///< AVCOL_TRC_BT709            ITU-R BT709 / ITU-R BT1361
                case  2: encoded =  0;          ///< AVCOL_TRC_UNSPECIFIED
                    Warning( "Shader 'video_yuv': $colortransferc = 2 (AVCOL_TRC_UNSPECIFIED). FALLBACK: 1 (AVCOL_TRC_BT709).\n" );
                    break;
                case  3: encoded = 0;          ///< AVCOL_TRC_RESERVED
                    Warning( "Shader 'video_yuv': $colortransferc = 3 (AVCOL_TRC_RESERVED). FALLBACK: 1 (AVCOL_TRC_BT709).\n" );
                    break;
                case  4: encoded =  1; break;   ///< AVCOL_TRC_GAMMA22          ITU-R BT470M / ITU-R BT1700 625 PAL & SECAM
                case  5: encoded =  2; break;   ///< AVCOL_TRC_GAMMA28          ITU-R BT470BG
                case  6: encoded =  0; break;   ///< AVCOL_TRC_SMPTE170M        SMPTE ST 170M / ITU-R BT601-6 525 or 625 / ITU-R BT1358 525 or 625 / ITU-R BT1700 NTSC
                case  7: encoded =  3; break;   ///< AVCOL_TRC_SMPTE240M        SMPTE ST 240M
                case  8: encoded =  4; break;   ///< AVCOL_TRC_LINEAR           "Linear transfer characteristic"
                case  9: encoded =  5; break;   ///< AVCOL_TRC_LOG100           "Logarithmic transfer characteristic (100:1 range)"
                case 10: encoded =  6; break;   ///< AVCOL_TRC_LOG_SQRT         "Logarithmic transfer characteristic (100 * Sqrt(10) : 1 range)"
                case 11: encoded =  7; break;   ///< AVCOL_TRC_IEC61966_2_4     IEC 61966-2-4
                case 12: encoded =  8; break;   ///< AVCOL_TRC_BT1361_ECG       ITU-R BT1361 Extended Colour Gamut
                case 13: encoded =  9; break;   ///< AVCOL_TRC_IEC61966_2_1     IEC 61966-2-1 (sRGB or sYCC)
                case 14: encoded =  0; break;   ///< AVCOL_TRC_BT2020_10        ITU-R BT2020 for 10-bit system
                case 15: encoded =  0; break;   ///< AVCOL_TRC_BT2020_12        ITU-R BT2020 for 12-bit system
                case 16: encoded = 10; break;   ///< AVCOL_TRC_SMPTE2084        SMPTE ST 2084 for 10-, 12-, 14- and 16-bit systems (PQ)
                case 17: encoded = 11; break;   ///< AVCOL_TRC_SMPTE428         SMPTE ST 428-1
                case 18: encoded = 12; break;   ///< AVCOL_TRC_ARIB_STD_B67     ARIB STD-B67, known as "Hybrid log-gamma" (HLG)
                default: encoded =  0;
                    Warning( "Shader 'video_yuv': $colortransferc = %d. FALLBACK: 1 (AVCOL_TRC_BT709).\n", val );
                    break;
            }
            params[ colortransferc ]->SetIntValue( encoded );
            //Warning( "\ncolortransferc = %d, encoded = %d\n", val, encoded );
        }

        if ( !params[ colorprimaries ]->IsDefined() ) {
            params[ colorprimaries ]->SetIntValue( 1 );
            Warning( "Shader 'video_yuv': undefined $colorprimaries. FALLBACK: 1 (AVCOL_PRI_BT709).\n" );
        }
        else {
            int val = params[ colorprimaries ]->GetIntValue();
            int encoded = 0;
            switch ( val ) {
                case  0: encoded =  0;          ///< AVCOL_PRI_RESERVED0
                    Warning( "Shader 'video_yuv': $colorprimaries = 0 (AVCOL_PRI_RESERVED0). FALLBACK: 1 (AVCOL_PRI_BT709).\n" );
                    break;
                case  1: encoded =  0; break;   ///< AVCOL_PRI_BT709        ITU-R BT709 / ITU-R BT1361 ITU-R BT1361 / IEC 61966-2-4 / SMPTE RP 177 Annex B
                case  2: encoded =  0;          ///< AVCOL_PRI_UNSPECIFIED
                    Warning( "Shader 'video_yuv': $colorprimaries = 2 (AVCOL_PRI_UNSPECIFIED). FALLBACK: 1 (AVCOL_PRI_BT709).\n" );
                    break;
                case  3: encoded =  0;          ///< AVCOL_PRI_RESERVED
                    Warning( "Shader 'video_yuv': $colorprimaries = 3 (AVCOL_PRI_RESERVED). FALLBACK: 1 (AVCOL_PRI_BT709).\n" );
                    break;
                case  4: encoded =  1; break;   ///< AVCOL_PRI_BT470M       ITU-R BT470M / FCC Title 47 Code of Federal Regulations 73.682 (a)(20)
                case  5: encoded =  2; break;   ///< AVCOL_PRI_BT470BG      ITU-R BT470BG / ITU-R BT601-6 625 / ITU-R BT1358 625 / ITU-R BT1700 625 PAL & SECAM
                case  6: encoded =  3; break;   ///< AVCOL_PRI_SMPTE170M    SMPTE ST 170M / ITU-R BT601-6 525 / ITU-R BT1358 525 / ITU-R BT1700 NTSC
                case  7: encoded =  3; break;   ///< AVCOL_PRI_SMPTE240M    SMPTE ST 240M, also called "SMPTE C" even though it uses D65
                case  8: encoded =  4; break;   ///< AVCOL_PRI_FILM         colour filters using Illuminant C
                case  9: encoded =  5; break;   ///< AVCOL_PRI_BT2020       ITU-R BT2020
                case 10: encoded =  6; break;   ///< AVCOL_PRI_SMPTE428     SMPTE ST 428-1 (CIE 1931 XYZ)
                case 11: encoded =  7; break;   ///< AVCOL_PRI_SMPTE431     SMPTE ST 431-2 (2011) / DCI P3
                case 12: encoded =  7; break;   ///< AVCOL_PRI_SMPTE432     SMPTE ST 432-1 (2010) / P3 D65 / Display P3
                case 22: encoded =  8; break;   ///< AVCOL_PRI_EBU3213      EBU Tech. 3213-E (nothing there) / one of JEDEC P22 group phosphors
                default: encoded =  0;
                    Warning( "Shader 'video_yuv': $colorprimaries = %d. FALLBACK: 1 (AVCOL_PRI_BT709).\n", val );
                    break;
            }
            params[ colorprimaries ]->SetIntValue( encoded );
            //Warning( "\ncolorpri = %d, encoded = %d\n", val, encoded );
        }

    }

    SHADER_FALLBACK
    {
        return 0;
    }



    SHADER_INIT
    {
        if ( params[ textureY ]->IsDefined() )
            LoadTexture( textureY );

        if ( params[ textureU ]->IsDefined() )
            LoadTexture( textureU );

        if ( params[ textureV ]->IsDefined() )
            LoadTexture( textureV );

        if ( ITexture *pTex = params[ textureY ]->GetTextureValue() ) {
            texSizes[ 0 ][ 0 ] = 1.0f / pTex->GetActualWidth();
            texSizes[ 0 ][ 1 ] = 1.0f / pTex->GetActualHeight();
            texSizes[ 0 ][ 2 ] = pTex->GetActualWidth();
            texSizes[ 0 ][ 3 ] = pTex->GetActualHeight();
        }
    }

    SHADER_DRAW
    {
        SHADOW_STATE
        {
            pShaderShadow->EnableTexture( SHADER_SAMPLER0, true );
            pShaderShadow->EnableTexture( SHADER_SAMPLER1, true );
            pShaderShadow->EnableTexture( SHADER_SAMPLER2, true );

            pShaderShadow->EnableSRGBRead( SHADER_SAMPLER0, false );
            pShaderShadow->EnableSRGBRead( SHADER_SAMPLER1, false );
            pShaderShadow->EnableSRGBRead( SHADER_SAMPLER2, false );

            unsigned int nFlags = VERTEX_POSITION;
            int nTexCoordCount = 1;
            pShaderShadow->VertexShaderVertexFormat( nFlags, nTexCoordCount, 0, 0 );

            DECLARE_STATIC_VERTEX_SHADER( video_yuv_vs30 );
            SET_STATIC_VERTEX_SHADER( video_yuv_vs30 );

            DECLARE_STATIC_PIXEL_SHADER( video_yuv_ps30 );
                SET_STATIC_PIXEL_SHADER_COMBO( SRC_BIT_DEPTH, params[ bitdepth ]->GetIntValue() );
                SET_STATIC_PIXEL_SHADER_COMBO( AVCOL_RANGE, params[ colorrange ]->GetIntValue() );
                SET_STATIC_PIXEL_SHADER_COMBO( AVCOL_SPC, params[ colorspace ]->GetIntValue() );
                SET_STATIC_PIXEL_SHADER_COMBO( AVCOL_TRC, params[ colortransferc ]->GetIntValue() );
                SET_STATIC_PIXEL_SHADER_COMBO( AVCOL_PRI, params[ colorprimaries ]->GetIntValue() );
            SET_STATIC_PIXEL_SHADER( video_yuv_ps30 );

            pShaderShadow->EnableSRGBWrite( false );
        }

        DYNAMIC_STATE
        {
            BindTexture( SHADER_SAMPLER0, textureY, FRAME );
            BindTexture( SHADER_SAMPLER1, textureU, FRAME );
            BindTexture( SHADER_SAMPLER2, textureV, FRAME );

            pShaderAPI->SetPixelShaderConstant( 0, &texSizes[ 0 ][ 0 ], 1 );

            LoadViewMatrixIntoVertexShaderConstant( VERTEX_SHADER_VIEWMODEL );

            DECLARE_DYNAMIC_VERTEX_SHADER( video_yuv_vs30 );
            SET_DYNAMIC_VERTEX_SHADER( video_yuv_vs30 );

            DECLARE_DYNAMIC_PIXEL_SHADER( video_yuv_ps30 );
            SET_DYNAMIC_PIXEL_SHADER( video_yuv_ps30 );
        }

        Draw();
    }
END_SHADER