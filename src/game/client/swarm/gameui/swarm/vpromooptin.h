#pragma once

#include "basemodframe.h"
#include "steam/steamclientpublic.h"

class CNB_Button;

namespace vgui
{
	class MultiFontRichText;
}

namespace BaseModUI
{
	class PromoOptIn : public CBaseModFrame
	{
		DECLARE_CLASS_SIMPLE( PromoOptIn, CBaseModFrame );
	public:
		PromoOptIn( vgui::Panel *parent, const char *panelName );
		~PromoOptIn();

		void Activate() override;
		void OnCommand( const char *command ) override;
		void OnKeyCodePressed( vgui::KeyCode keycode ) override;

		vgui::MultiFontRichText *m_pLblFlavor;
		vgui::Label *m_pLblExplanationTitle;
		vgui::Label *m_pLblExplanation;
		CNB_Button *m_pBtnDecline;
		CNB_Button *m_pBtnAccept;
		CNB_Button *m_pBtnAlready;

		int m_hBackgroundNoiseLoop;
	};
}
