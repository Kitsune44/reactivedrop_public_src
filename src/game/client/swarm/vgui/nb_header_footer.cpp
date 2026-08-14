#include "cbase.h"

#include "nb_header_footer.h"
#include "vgui_controls/Label.h"
#include "vgui_controls/ImagePanel.h"
#include <vgui/ISurface.h>
#include "vgui_hudvideo.h"
#include "asw_video.h"
#include "VGUIMatSurface/IMatSystemSurface.h"
#include "asw_gamerules.h"
#include "filesystem.h"
#include "rd_workshop.h"
#include "asw_util_shared.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"


using namespace vgui;

ConVar rd_reduce_motion( "rd_reduce_motion", "0", FCVAR_ARCHIVE, "reduce ambient motion in menus" );
ConVar rd_disable_briefing_camera( "rd_disable_briefing_camera", "0", FCVAR_NONE, "always render video backgrounds for briefing" );
static bool s_bLastReduceMotion = false;

CASW_Background_Movie *g_pBackgroundMovie = NULL;

CASW_Background_Movie *ASWBackgroundMovie()
{
	if ( !g_pBackgroundMovie )
	{
		g_pBackgroundMovie = new CASW_Background_Movie();
	}
	return g_pBackgroundMovie;
}

CASW_Background_Movie::CASW_Background_Movie()
{
	m_nMaterialType = MATERIAL_INVALID;
	m_nTextureID = -1;
	m_szCurrentMovie[ 0 ] = 0;
	m_nLastGameState = -1;
}

CASW_Background_Movie::~CASW_Background_Movie()
{

}

void CASW_Background_Movie::SetCurrentMovie( const char *szFilename )
{
	// Safety check as we're possibly going to overwrite a file here!
	char szBaseName[ MAX_PATH ];
	V_FileBase( szFilename, szBaseName, sizeof( szBaseName ) );
	const char *szAllowedExtensions[] = { "bik", "webm", "mp4", "mov", nullptr};

	bool bValidExtension = false;
	const char *szExt = V_GetFileExtension( szFilename );
	if ( szExt )
	{
		for ( int i = 0; szAllowedExtensions[ i ]; i++ )
		{
			if ( !Q_stricmp( szExt, szAllowedExtensions[ i ] ) )
			{
				bValidExtension = true;
				break;
			}
		}
	}

	bool bValidPath = false;
	if ( V_IsAbsolutePath( szFilename ) )
	{
		Warning( "Absolute paths are not allowed for video files: %s\n", szFilename );
	}
	else
	{
		char szExpectedPrefix[] = "media/";
		if ( !Q_strnicmp( szFilename, szExpectedPrefix, sizeof( szExpectedPrefix ) - 1 ) )
		{
			if ( g_pFullFileSystem->FileExists( szFilename, "GAME" ) )
			{
				bValidPath = true;
			}
		}
	}

	if ( !bValidExtension || !bValidPath )
	{
		Warning( "Invalid video path: %s\n", szFilename );

		const char *szDefaultFiles[] = {
			"media/BGFX_03.webm",
			"media/BGFX_03.bik",
			nullptr
		};

		for ( int i = 0; szDefaultFiles[ i ]; i++ )
		{
			if ( g_pFullFileSystem->FileExists( szDefaultFiles[ i ], "GAME" ) )
			{
				szFilename = szDefaultFiles[ i ];
				break;
			}
		}
	}

	szFilename = g_ReactiveDropWorkshop.GetNativeFileSystemFile( szFilename );
	if ( Q_strcmp( m_szCurrentMovie, szFilename ) )
	{
		ClearCurrentMovie();

		const char *ext = Q_GetFileExtension( szFilename );
		if ( ext && !Q_stricmp( ext, "bik" ) )
		{
			char szMaterialName[ MAX_PATH ];
			Q_snprintf( szMaterialName, sizeof( szMaterialName ), "BackgroundBIKMaterial%i", g_pBIK->GetGlobalMaterialAllocationNumber() );

			m_nBIKMaterial = bik->CreateMaterial( szMaterialName, szFilename, "GAME", BIK_LOOP );
			m_nMaterialType = MATERIAL_BIK;

		}
		else
		{
			char szFilePath[ MAX_PATH ];
			g_pFullFileSystem->RelativePathToFullPath( szFilename, "GAME", szFilePath, sizeof( szFilePath ) );
			if ( m_videoFFmpegPlayer.Init( szFilePath ) )
			{
				m_videoFFmpegPlayer.Play();
				m_nMaterialType = MATERIAL_FFMPEG;
			}
			else{
				Warning( "Failed to create video material for:\n  %s\n  %s\n", szFilePath, m_videoFFmpegPlayer.GetError() );
				m_videoFFmpegPlayer.Reset();
			}
		}

		Q_snprintf( m_szCurrentMovie, sizeof( m_szCurrentMovie ), "%s", szFilename );
		s_bLastReduceMotion = false;
	}
}


void CASW_Background_Movie::ClearCurrentMovie()
{
	if ( m_nMaterialType != MATERIAL_INVALID )
	{
		switch ( m_nMaterialType )
		{
		case MATERIAL_FFMPEG:
			m_videoFFmpegPlayer.Reset();
			break;
		case MATERIAL_BIK:
			g_pBIK->DestroyMaterial( m_nBIKMaterial );
			break;
		}
		m_nMaterialType = MATERIAL_INVALID;
		m_nTextureID = -1;
	}
}

int CASW_Background_Movie::SetTextureMaterial()
{
	if ( m_nMaterialType == MATERIAL_INVALID )
		return -1;

	if ( m_nTextureID == -1 )
	{
		m_nTextureID = g_pMatSystemSurface->CreateNewTextureID( true );
	}

	switch ( m_nMaterialType )
	{
	case MATERIAL_FFMPEG:
	{
		g_pMatSystemSurface->DrawSetTextureMaterial( m_nTextureID, m_videoFFmpegPlayer.GetMaterial() );
		break;
	}
	case MATERIAL_BIK:
		g_pMatSystemSurface->DrawSetTextureMaterial( m_nTextureID, g_pBIK->GetMaterial( m_nBIKMaterial ) );
		break;
	}

	return m_nTextureID;
}

void CASW_Background_Movie::Update( bool bForce )
{	
	if ( engine->IsConnected() && ASWGameRules() )
	{
		int nGameState = ASWGameRules()->GetGameState();
		if ( nGameState >= ASW_GS_DEBRIEF && ASWGameRules()->GetMissionSuccess() )
		{
			nGameState += 10;
		}
		if ( ( nGameState != m_nLastGameState || bForce ) && !( nGameState == ASW_GS_LAUNCHING || nGameState == ASW_GS_INGAME ) )
		{
			const char *pFilename = NULL;
			const char *szMovieType = "briefing";
			if ( ASWGameRules()->GetGameState() >= ASW_GS_DEBRIEF )
			{
				if ( ASWGameRules()->GetMissionSuccess() )
				{
					szMovieType = "success";
				}
				else
				{
					szMovieType = "failure";
				}
			}
			else
			{
				pFilename = ASWGameRules()->m_szBriefingVideo;
				if ( pFilename[ 0 ] == '\0' )
				{
					pFilename = NULL;
				}
			}
			if ( pFilename == NULL )
				pFilename = UTIL_RD_RandomBriefingMovie( engine->GetLevelNameShort(), ASWGameRules()->m_iCosmeticRandomSeed, szMovieType );
			if ( pFilename )
			{
				SetCurrentMovie( pFilename );
			}
		}
		m_nLastGameState = nGameState;
	}
	else
	{
		int nGameState = 0;
		if ( nGameState != m_nLastGameState || bForce )
		{
			const char *szMainMenuImage, *szMainMenuVideo, *szMainMenuAudio;
			UTIL_RD_DecideMainMenuBackground( szMainMenuImage, szMainMenuVideo, szMainMenuAudio, false );
			SetCurrentMovie( szMainMenuVideo );
			m_nLastGameState = nGameState;
		}
	}

	if ( m_nMaterialType == MATERIAL_INVALID )
		return;

	switch ( m_nMaterialType )
	{
	case MATERIAL_FFMPEG:
		if ( !s_bLastReduceMotion && rd_reduce_motion.GetBool() )
		{
			s_bLastReduceMotion = true;
			m_videoFFmpegPlayer.Pause();
		}
		else if ( s_bLastReduceMotion && !rd_reduce_motion.GetBool() )
		{
			s_bLastReduceMotion = false;
			m_videoFFmpegPlayer.Play();
		}
		if ( !m_videoFFmpegPlayer.Update() )
		{
			m_videoFFmpegPlayer.Reset();
			m_nMaterialType = MATERIAL_INVALID;
			m_nTextureID = -1;
		}
		break;
	case MATERIAL_BIK:
		if ( g_pBIK->ReadyForSwap( m_nBIKMaterial ) )
		{
			if ( g_pBIK->Update( m_nBIKMaterial ) == false )
			{
				g_pBIK->DestroyMaterial( m_nBIKMaterial );
				m_nMaterialType = MATERIAL_INVALID;
				m_nTextureID = -1;
			}
			else if ( !s_bLastReduceMotion && rd_reduce_motion.GetBool() )
			{
				s_bLastReduceMotion = true;
				bik->Pause( m_nBIKMaterial );
			}
		}

		if ( m_nMaterialType != MATERIAL_INVALID && s_bLastReduceMotion && !rd_reduce_motion.GetBool() )
		{
			s_bLastReduceMotion = false;
			bik->Unpause( m_nBIKMaterial );
		}
		break;		
	}
	
}

// ======================================

CNB_Header_Footer::CNB_Header_Footer( vgui::Panel *parent, const char *name ) : BaseClass( parent, name )
{
	// == MANAGED_MEMBER_CREATION_START: Do not edit by hand ==
	m_pBackground = new vgui::Panel( this, "Background" );
	m_pBackgroundImage = new vgui::ImagePanel( this, "BackgroundImage" );
	m_pTitle = new vgui::Label( this, "Title", "" );
	m_pBottomBar = new vgui::Panel( this, "BottomBar" );
	m_pBottomBarLine = new vgui::Panel( this, "BottomBarLine" );
	m_pTopBar = new vgui::Panel( this, "TopBar" );
	m_pTopBarLine = new vgui::Panel( this, "TopBarLine" );
	// == MANAGED_MEMBER_CREATION_END ==
	m_pGradientBar = new CNB_Gradient_Bar( this, "GradientBar" );
	m_pGradientBar->SetZPos( 2 );

	m_bHeaderEnabled = true;
	m_bFooterEnabled = true;
	m_bMovieEnabled = true;
	m_bBriefingCameraEnabled = false;
	m_bGradientBarEnabled = 0;
	m_nTitleStyle = NB_TITLE_MEDIUM;
	m_nBackgroundStyle = NB_BACKGROUND_TRANSPARENT_BLUE;
	m_nGradientBarY = 0;
	m_nGradientBarWidth = 0;
	m_nGradientBarHeight = 480;
}

CNB_Header_Footer::~CNB_Header_Footer()
{

}

extern ConVar asw_force_background_movie;
ConVar asw_background_color( "asw_background_color", "16 32 46 128", FCVAR_NONE, "Color of background tinting in briefing screens" );

void CNB_Header_Footer::ApplySchemeSettings( vgui::IScheme *pScheme )
{
	BaseClass::ApplySchemeSettings( pScheme );

	LoadControlSettings( "resource/ui/nb_header_footer.res" );

	// TODO: Different image in widescreen to avoid stretching
	// this image is no longer used
	//m_pBackgroundImage->SetImage( "lobby/swarm_background01" );

	switch ( m_nTitleStyle )
	{
	case NB_TITLE_BRIGHT: m_pTitle->SetFgColor( m_TitleBrightColor ); break;
	case NB_TITLE_MEDIUM: m_pTitle->SetFgColor( m_TitleMediumColor ); break;
	}

	switch ( m_nBackgroundStyle )
	{
	case NB_BACKGROUND_DARK:
	{
		m_pBackground->SetVisible( true );
		m_pBackgroundImage->SetVisible( false );
		m_pBackground->SetBgColor( m_BackgroundColorDark );
		break;
	}
	case NB_BACKGROUND_TRANSPARENT_BLUE:
	{
		m_pBackground->SetVisible( true );
		m_pBackgroundImage->SetVisible( false );
		m_pBackground->SetBgColor( asw_background_color.GetColor() );
		break;
	}
	case NB_BACKGROUND_TRANSPARENT_RED:
	{
		m_pBackground->SetVisible( true );
		m_pBackgroundImage->SetVisible( false );
		m_pBackground->SetBgColor( m_BackgroundColorRed );
		break;
	}
	case NB_BACKGROUND_BLUE:
	{
		m_pBackground->SetVisible( true );
		m_pBackgroundImage->SetVisible( false );
		m_pBackground->SetBgColor( m_BackgroundColorBlue );
		break;
	}
	case NB_BACKGROUND_IMAGE:
	{
		m_pBackground->SetVisible( false );
		m_pBackgroundImage->SetVisible( true );
		break;
	}

	case NB_BACKGROUND_NONE:
	{
		m_pBackground->SetVisible( false );
		m_pBackgroundImage->SetVisible( false );
	}
	}

	m_pTopBar->SetVisible( m_bHeaderEnabled );
	m_pTopBarLine->SetVisible( m_bHeaderEnabled );
	m_pBottomBar->SetVisible( m_bFooterEnabled );
	m_pBottomBarLine->SetVisible( m_bFooterEnabled );
	m_pGradientBar->SetVisible( m_bGradientBarEnabled );
}

void CNB_Header_Footer::PerformLayout()
{
	BaseClass::PerformLayout();

	int nGradientBarWidth = m_nGradientBarWidth ? YRES( m_nGradientBarWidth ) : ScreenWidth();
	m_pGradientBar->SetBounds( ( ScreenWidth() - nGradientBarWidth ) / 2, YRES( m_nGradientBarY ), nGradientBarWidth, YRES( m_nGradientBarHeight ) );
}

void CNB_Header_Footer::ApplySettings( KeyValues *inResourceData )
{
	BaseClass::ApplySettings( inResourceData );

	SetGradientBarEnabled( inResourceData->GetBool( "gradient_bar_enabled", m_bGradientBarEnabled ) );
	SetGradientBarPos( inResourceData->GetInt( "gradient_bar_ypos", m_nGradientBarY ), inResourceData->GetInt( "gradient_bar_tall", m_nGradientBarHeight ) );
	SetGradientBarWide( inResourceData->GetInt( "gradient_bar_wide", m_nGradientBarWidth ) );
}

void CNB_Header_Footer::OnThink()
{
	BaseClass::OnThink();
}

void CNB_Header_Footer::SetTitle( const char *pszTitle )
{
	m_pTitle->SetText( pszTitle );
}

void CNB_Header_Footer::SetTitle( const wchar_t *pwszTitle )
{
	m_pTitle->SetText( pwszTitle );
}

void CNB_Header_Footer::SetHeaderEnabled( bool bEnabled )
{
	m_pTopBar->SetVisible( bEnabled );
	m_pTopBarLine->SetVisible( bEnabled );
	m_bHeaderEnabled = bEnabled;
}

void CNB_Header_Footer::SetFooterEnabled( bool bEnabled )
{
	m_pBottomBar->SetVisible( bEnabled );
	m_pBottomBarLine->SetVisible( bEnabled );
	m_bFooterEnabled = bEnabled;
}

void CNB_Header_Footer::SetGradientBarEnabled( bool bEnabled )
{
	m_pGradientBar->SetVisible( bEnabled );
	m_bGradientBarEnabled = bEnabled;
}

void CNB_Header_Footer::SetGradientBarPos( int nY, int nHeight )
{
	m_nGradientBarY = nY;
	m_nGradientBarHeight = nHeight;
	InvalidateLayout();
}

void CNB_Header_Footer::SetGradientBarWide( int nWidth )
{
	m_nGradientBarWidth = nWidth;
	InvalidateLayout();
}

void CNB_Header_Footer::SetGradientBarColor( Color color )
{
	m_pGradientBar->m_Color = color;
}

void CNB_Header_Footer::SetTitleStyle( NB_Title_Style nTitleStyle )
{
	m_nTitleStyle = nTitleStyle;
	InvalidateLayout( false, true );
}

void CNB_Header_Footer::SetBackgroundStyle( NB_Background_Style nBackgroundStyle )
{
	m_nBackgroundStyle = nBackgroundStyle;
	InvalidateLayout( false, true );
}

void CNB_Header_Footer::SetMovieEnabled( bool bMovieEnabled )
{
	m_bMovieEnabled = bMovieEnabled;
	InvalidateLayout( false, true );
}

void CNB_Header_Footer::SetBriefingCameraEnabled( bool bBriefingCameraEnabled )
{
	m_bBriefingCameraEnabled = bBriefingCameraEnabled;
	InvalidateLayout( false, true );
}

void CNB_Header_Footer::PaintBackground()
{
	BaseClass::PaintBackground();

	if ( m_bMovieEnabled && ASWBackgroundMovie() && !( m_bBriefingCameraEnabled && engine->IsConnected() && ASWGameRules() && ASWGameRules()->GetGameState() < ASW_GS_INGAME && ASWGameRules()->m_hBriefingCamera && !rd_reduce_motion.GetBool() && !rd_disable_briefing_camera.GetBool() ) )
	{
		ASWBackgroundMovie()->Update();
		if ( ASWBackgroundMovie()->SetTextureMaterial() != -1 )
		{
			surface()->DrawSetColor( 255, 255, 255, 255 );

			int x, y, w, t;
			GetBounds( x, y, w, t );

			// center, 16:9 aspect ratio
			int width_at_ratio = t * ( 16.0f / 9.0f );
			x = ( w * 0.5f ) - ( width_at_ratio * 0.5f );

			surface()->DrawTexturedRect( x, y, x + width_at_ratio, y + t );
		}
	}

	// test of gradient header/footer
	/*
	int nScreenWidth = GetWide();
	int nScreenHeight = GetTall();
	int iHalfWide = nScreenWidth * 0.5f;
	int nBarHeight = YRES( 22 );

	//surface()->DrawSetColor( Color( 16, 32, 46, 230 ) );
	surface()->DrawSetColor( Color( 0, 0, 0, 230 ) );
	surface()->DrawFilledRect( 0, 0, nScreenWidth, nScreenHeight );

	if ( m_bHeaderEnabled )
	{
		surface()->DrawSetColor( Color( 19, 35, 65, 255 ) );
		surface()->DrawFilledRect( 0, 0, nScreenWidth, nBarHeight );

		surface()->DrawSetColor( Color( 35, 61, 87, 255 ) );
		surface()->DrawFilledRectFade( iHalfWide, 0, iHalfWide + YRES( 320 ), nBarHeight, 255, 0, true );
		surface()->DrawFilledRectFade( iHalfWide - YRES( 320 ), 0, iHalfWide, nBarHeight, 0, 255, true );
	}

	if ( m_bFooterEnabled )
	{
		surface()->DrawSetColor( Color( 19, 35, 65, 255 ) );
		surface()->DrawFilledRect( 0, nScreenHeight - nBarHeight, nScreenWidth, nScreenHeight );

		surface()->DrawSetColor( Color( 35, 61, 87, 255 ) );
		surface()->DrawFilledRectFade( iHalfWide, nScreenHeight - nBarHeight, iHalfWide + YRES( 320 ), nScreenHeight, 255, 0, true );
		surface()->DrawFilledRectFade( iHalfWide - YRES( 320 ), nScreenHeight - nBarHeight, iHalfWide, nScreenHeight, 0, 255, true );
	}
	*/
}

// =================

CNB_Gradient_Bar::CNB_Gradient_Bar( vgui::Panel *parent, const char *name ) : BaseClass( parent, name )
{
	m_Color = Color( 53, 86, 117, 255 );
}

void CNB_Gradient_Bar::PaintBackground()
{
	int wide, tall;
	GetSize( wide, tall );

	int y = 0;
	int iHalfWide = wide * 0.5f;

	float flAlpha = 200.0f / 255.0f;

	// fill bar background
	vgui::surface()->DrawSetColor( Color( 0, 0, 0, 255 * flAlpha ) );
	if ( wide == ScreenWidth() )
	{
		vgui::surface()->DrawFilledRect( 0, y, wide, y + tall );
	}
	else
	{
		// if we're not full screen width, fade the background shading as well
		int iEdgeWide = wide * 0.05f;
		vgui::surface()->DrawFilledRectFade( 0, y, iEdgeWide, y + tall, 0, 255, true );
		vgui::surface()->DrawFilledRect( iEdgeWide, y, wide - iEdgeWide, y + tall );
		vgui::surface()->DrawFilledRectFade( wide - iEdgeWide, y, wide, y + tall, 255, 0, true );
	}

	vgui::surface()->DrawSetColor( Color( m_Color.r(), m_Color.g(), m_Color.b(), m_Color.a() * flAlpha ) );

	int nBarPosY = y + YRES( 4 );
	int nBarHeight = tall - YRES( 8 );
	vgui::surface()->DrawFilledRectFade( iHalfWide, nBarPosY, wide, nBarPosY + nBarHeight, 255, 0, true );
	vgui::surface()->DrawFilledRectFade( 0, nBarPosY, iHalfWide, nBarPosY + nBarHeight, 0, 255, true );
	// draw highlights
	nBarHeight = YRES( 2 );
	nBarPosY = y;
	vgui::surface()->DrawSetColor( Color( 97, 210, 255, 255 * flAlpha ) );
	vgui::surface()->DrawFilledRectFade( iHalfWide, nBarPosY, wide, nBarPosY + nBarHeight, 255, 0, true );
	vgui::surface()->DrawFilledRectFade( 0, nBarPosY, iHalfWide, nBarPosY + nBarHeight, 0, 255, true );

	nBarPosY = y + tall - YRES( 2 );
	vgui::surface()->DrawFilledRectFade( iHalfWide, nBarPosY, wide, nBarPosY + nBarHeight, 255, 0, true );
	vgui::surface()->DrawFilledRectFade( 0, nBarPosY, iHalfWide, nBarPosY + nBarHeight, 0, 255, true );
}
