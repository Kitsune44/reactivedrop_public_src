// ALL SKIP STATEMENTS THAT AFFECT THIS SHADER!!!
// defined $PIXELFOGTYPE && defined $WRITEWATERFOGTODESTALPHA && ( $PIXELFOGTYPE != 1 ) && $WRITEWATERFOGTODESTALPHA
// defined $LIGHTING_PREVIEW && defined $FASTPATHENVMAPTINT && $LIGHTING_PREVIEW && $FASTPATHENVMAPTINT
// defined $LIGHTING_PREVIEW && defined $FASTPATHENVMAPCONTRAST && $LIGHTING_PREVIEW && $FASTPATHENVMAPCONTRAST
// defined $LIGHTING_PREVIEW && defined $FASTPATH && $LIGHTING_PREVIEW && $FASTPATH
// ($FLASHLIGHT || $FLASHLIGHTSHADOWS) && $LIGHTING_PREVIEW
// defined $PIXELFOGTYPE && defined $WRITEWATERFOGTODESTALPHA && ( $PIXELFOGTYPE != 1 ) && $WRITEWATERFOGTODESTALPHA
// defined $LIGHTING_PREVIEW && defined $FASTPATHENVMAPTINT && $LIGHTING_PREVIEW && $FASTPATHENVMAPTINT
// defined $LIGHTING_PREVIEW && defined $FASTPATHENVMAPCONTRAST && $LIGHTING_PREVIEW && $FASTPATHENVMAPCONTRAST
// defined $LIGHTING_PREVIEW && defined $FASTPATH && $LIGHTING_PREVIEW && $FASTPATH
// ($FLASHLIGHT || $FLASHLIGHTSHADOWS) && $LIGHTING_PREVIEW

#pragma once
#include "shaderlib/cshader.h"
class video_yuv_ps30_Static_Index
{
	unsigned int m_nSRC_BIT_DEPTH : 2;
	unsigned int m_nAVCOL_RANGE : 2;
	unsigned int m_nAVCOL_SPC : 3;
	unsigned int m_nAVCOL_TRC : 4;
	unsigned int m_nAVCOL_PRI : 4;
#ifdef _DEBUG
	bool m_bSRC_BIT_DEPTH : 1;
	bool m_bAVCOL_RANGE : 1;
	bool m_bAVCOL_SPC : 1;
	bool m_bAVCOL_TRC : 1;
	bool m_bAVCOL_PRI : 1;
#endif	// _DEBUG
public:
	void SetSRC_BIT_DEPTH( int i )
	{
		Assert( i >= 0 && i <= 2 );
		m_nSRC_BIT_DEPTH = i;
#ifdef _DEBUG
		m_bSRC_BIT_DEPTH = true;
#endif	// _DEBUG
	}

	void SetAVCOL_RANGE( int i )
	{
		Assert( i >= 0 && i <= 1 );
		m_nAVCOL_RANGE = i;
#ifdef _DEBUG
		m_bAVCOL_RANGE = true;
#endif	// _DEBUG
	}

	void SetAVCOL_SPC( int i )
	{
		Assert( i >= 0 && i <= 5 );
		m_nAVCOL_SPC = i;
#ifdef _DEBUG
		m_bAVCOL_SPC = true;
#endif	// _DEBUG
	}

	void SetAVCOL_TRC( int i )
	{
		Assert( i >= 0 && i <= 12 );
		m_nAVCOL_TRC = i;
#ifdef _DEBUG
		m_bAVCOL_TRC = true;
#endif	// _DEBUG
	}

	void SetAVCOL_PRI( int i )
	{
		Assert( i >= 0 && i <= 8 );
		m_nAVCOL_PRI = i;
#ifdef _DEBUG
		m_bAVCOL_PRI = true;
#endif	// _DEBUG
	}

	video_yuv_ps30_Static_Index( IShaderShadow* pShaderShadow, IMaterialVar** params )
	{
		m_nSRC_BIT_DEPTH = 0;
		m_nAVCOL_RANGE = 0;
		m_nAVCOL_SPC = 0;
		m_nAVCOL_TRC = 0;
		m_nAVCOL_PRI = 0;
#ifdef _DEBUG
		m_bSRC_BIT_DEPTH = false;
		m_bAVCOL_RANGE = false;
		m_bAVCOL_SPC = false;
		m_bAVCOL_TRC = false;
		m_bAVCOL_PRI = false;
#endif	// _DEBUG
	}

	int GetIndex() const
	{
		Assert( m_bSRC_BIT_DEPTH && m_bAVCOL_RANGE && m_bAVCOL_SPC && m_bAVCOL_TRC && m_bAVCOL_PRI );
		return ( 1 * m_nSRC_BIT_DEPTH ) + ( 3 * m_nAVCOL_RANGE ) + ( 6 * m_nAVCOL_SPC ) + ( 36 * m_nAVCOL_TRC ) + ( 468 * m_nAVCOL_PRI ) + 0;
	}
};

#define shaderStaticTest_video_yuv_ps30 psh_forgot_to_set_static_SRC_BIT_DEPTH + psh_forgot_to_set_static_AVCOL_RANGE + psh_forgot_to_set_static_AVCOL_SPC + psh_forgot_to_set_static_AVCOL_TRC + psh_forgot_to_set_static_AVCOL_PRI


class video_yuv_ps30_Dynamic_Index
{
public:
	video_yuv_ps30_Dynamic_Index( IShaderDynamicAPI* pShaderAPI )
	{
	}

	int GetIndex() const
	{
		return 0;
	}
};

#define shaderDynamicTest_video_yuv_ps30 1

