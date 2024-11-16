#ifndef C_ASW_SENTRY_BASE_H
#define C_ASW_SENTRY_BASE_H

#include "iasw_client_usable_entity.h"
#include "rd_inventory_shared.h"
#include <vgui/vgui.h>

class C_ASW_Sentry_Base : public C_BaseAnimating, public IASW_Client_Usable_Entity, public IHealthTracked
{
public:
	DECLARE_CLASS( C_ASW_Sentry_Base, C_BaseAnimating );
	DECLARE_CLIENTCLASS();
	DECLARE_PREDICTABLE();

					C_ASW_Sentry_Base();
	virtual			~C_ASW_Sentry_Base();

	bool Simulate() override;

	bool IsAssembled() const { return m_bAssembled; }
	bool IsInUse() const { return m_bIsInUse; }

	int GetSentryIconTextureID();	

	float GetAssembleProgress() const { return m_fAssembleProgress; }

	bool WantsDismantle( void ) const;

	int GetAmmo() const { return m_iAmmo; }
	int GetMaxAmmo() const { return m_iMaxAmmo; }

	CNetworkVar( bool, m_bAssembled );
	CNetworkVar( bool, m_bIsInUse );
	CNetworkVar( float, m_fAssembleProgress );
	CNetworkVar( float, m_fAssembleCompleteTime );
	CNetworkVar( int, m_iAmmo );
	CNetworkVar( int, m_iMaxAmmo );
	CNetworkVar( bool, m_bSkillMarineHelping );
	CNetworkVar( int, m_nGunType );

	CNetworkHandle( C_ASW_Marine_Resource, m_hOriginalOwnerMR );
	CNetworkVar( int, m_iInventoryEquipSlot );
	bool IsInventoryEquipSlotValid() const;

	CNetworkHandle( C_ASW_Inhabitable_NPC, m_hLastDisassembler );

	// class of the weapon that created us
	const char* GetWeaponClass();

	// fake sentry top for building animation
	CHandle<C_BaseAnimating> m_hBuildTop;

	// IASW_Client_Usable_Entity
	virtual C_BaseEntity* GetEntity() { return this; }
	virtual bool IsUsable(C_BaseEntity *pUser);
	virtual bool GetUseAction(ASWUseAction &action, C_ASW_Inhabitable_NPC *pUser);
	virtual void CustomPaint(int ix, int iy, int alpha, vgui::Panel *pUseIcon);
	virtual bool ShouldPaintBoxAround() { return true; }
	virtual bool NeedsLOSCheck() { return true; }

	virtual Class_T		Classify( void ) { return (Class_T) CLASS_ASW_SENTRY_BASE; }

	// IHealthTracked
	CNetworkVar( int, m_iMaxHealth );
	int	GetHealth() const override { return m_iHealth; }
	int GetMaxHealth() const override { return m_iMaxHealth; }
	void PaintHealthBar( class CASWHud3DMarineNames *pSurface ) override;

	CInterpolatedVar<float> m_iv_fAssembleProgress;

	static vgui::HFont s_hAmmoFont;
	int m_nUseIconTextureID;
	
private:
	C_ASW_Sentry_Base( const C_ASW_Sentry_Base & ); // not defined, not accessible
};

extern CUtlVector<C_ASW_Sentry_Base*>	g_SentryGuns;

#endif /* C_ASW_SENTRY_BASE_H */