#ifndef _INCLUDED_NB_COMMANDER_LIST_ENTRY_H
#define _INCLUDED_NB_COMMANDER_LIST_ENTRY_H
#ifdef _WIN32
#pragma once
#endif

#include <vgui/VGUI.h>
#include <vgui_controls/EditablePanel.h>

// == MANAGED_CLASS_DECLARATIONS_START: Do not edit by hand ==
// clang: a forward declaration cannot have a nested name specifier ("class vgui::Label;"),
// so the class is declared inside its namespace instead.
namespace vgui { class Label; }
class CAvatarImagePanel;
// clang: a forward declaration cannot have a nested name specifier ("class vgui::Panel;"),
// so the class is declared inside its namespace instead.
namespace vgui { class Panel; }
// == MANAGED_CLASS_DECLARATIONS_END ==

class CNB_Commander_List_Entry : public vgui::EditablePanel
{
	DECLARE_CLASS_SIMPLE( CNB_Commander_List_Entry, vgui::EditablePanel );
public:
	CNB_Commander_List_Entry( vgui::Panel *parent, const char *name );
	virtual ~CNB_Commander_List_Entry();
	
	virtual void ApplySchemeSettings( vgui::IScheme *pScheme );
	virtual void PerformLayout();
	virtual void OnThink();
	virtual void OnCommand( const char *command );
	virtual void OnTick();

	void SetClientIndex( int nIndex );
	
	// == MANAGED_MEMBER_POINTERS_START: Do not edit by hand ==
	vgui::Label	*m_pCommanderName;
	CAvatarImagePanel	*m_pAvatar;
	vgui::Panel	*m_pAvatarBackground;
	vgui::Label	*m_pCommanderLevel;
	// == MANAGED_MEMBER_POINTERS_END ==

	int m_nClientIndex;
	int m_nLastClientIndex;
};

#endif // _INCLUDED_NB_COMMANDER_LIST_ENTRY_H