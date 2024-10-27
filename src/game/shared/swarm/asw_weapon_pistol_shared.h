#ifndef _DEFINED_ASW_WEAPON_PISTOL_H
#define _DEFINED_ASW_WEAPON_PISTOL_H
#pragma once

#ifdef CLIENT_DLL
#include "c_asw_weapon.h"
#define CASW_Weapon C_ASW_Weapon
#define CASW_Weapon_Pistol C_ASW_Weapon_Pistol
#define CASW_Marine C_ASW_Marine
#else
#include "asw_weapon.h"
#include "npc_combine.h"
#endif

#include "basegrenade_shared.h"
#include "asw_shareddefs.h"

enum ASW_Weapon_PistolHand_t
{
	ASW_WEAPON_PISTOL_RIGHT,
	ASW_WEAPON_PISTOL_LEFT
};

class CASW_Weapon_Pistol : public CASW_Weapon
{
public:
	DECLARE_CLASS( CASW_Weapon_Pistol, CASW_Weapon );
	DECLARE_NETWORKCLASS();
	DECLARE_PREDICTABLE();

	CASW_Weapon_Pistol();
	virtual ~CASW_Weapon_Pistol();
	void Precache();

	virtual float GetFireRate( void );
	
	void ItemPostFrame();
	Activity	GetPrimaryAttackActivity( void );
	virtual bool ShouldMarineMoveSlow();

	virtual void PrimaryAttack();
	virtual void FireBullets(CASW_Marine *pMarine, FireBulletsInfo_t *pBulletsInfo);
	int ASW_SelectWeaponActivity(int idealActivity);
	virtual float GetWeaponBaseDamageOverride();
	virtual int GetWeaponSkillId();
	virtual int GetWeaponSubSkillId();
	virtual int AmmoClickPoint() { return 2; }

	virtual const float GetAutoAimAmount() { return 0.26f; }
	virtual bool ShouldFlareAutoaim() { return true; }
	virtual bool SupportsDamageModifiers() { return true; }

	virtual const Vector& GetBulletSpread( void )
	{
		static const Vector cone = VECTOR_CONE_PRECALCULATED;

		return cone;
	}

	#ifndef CLIENT_DLL
		DECLARE_DATADESC();

		int		CapabilitiesGet( void ) { return bits_CAP_WEAPON_RANGE_ATTACK1; }

		virtual bool IsRapidFire() { return false; }
		virtual float GetMadFiringBias() { return 0.2f; }	// scales the rate at which the mad firing counter goes up when we shoot aliens with this weapon
		virtual const char *GetViewModel( int viewmodelindex = 0 ) const;
		virtual const char *GetWorldModel( void ) const;
	#else
		virtual const char* GetPartialReloadSound(int iPart);
		//virtual const char* GetTracerEffectName() { return "tracer_pistol"; }	// particle effect name
		//virtual const char* GetMuzzleEffectName() { return "muzzle_pistol"; }	// particle effect name

		virtual bool DisplayClipsDoubled() { return !m_bIsSingle; }				// dual weilded guns should show ammo doubled up to complete the illusion of holding two guns
		virtual const char* GetTracerEffectName() { return "tracer_pistol"; }	// particle effect name
		virtual const char* GetMuzzleEffectName() { return "muzzle_pistol"; }	// particle effect name
	#endif
	virtual const char *GetMagazineGibModelName() const override { return "models/weapons/empty_clips/pistol_empty_clip.mdl"; }
	virtual bool HasSecondaryAttack() override { return false; } // weapon has no secondary fire

	virtual const char* GetUTracerType();

	CNetworkVar( bool, m_bIsSingle );

	// Classification
	virtual Class_T		Classify( void ) { return (Class_T) CLASS_ASW_PISTOL; }

protected:
	CNetworkVar( float, m_fSlowTime );	// marine moves slow until this moment
	float	m_flSoonestPrimaryAttack;
	ASW_Weapon_PistolHand_t m_currentPistol;
};


#endif /* _DEFINED_ASW_WEAPON_PISTOL_H */
