#include "cbase.h"
#include "rd_inventory_command.h"
#include "rd_crafting_defs.h"
#include "steam/isteaminventory.h"
#include "fmtstr.h"
#include "asw_util_shared.h"
#include <ctime>
#ifdef CLIENT_DLL
#include "c_user_message_register.h"
#else
#include "asw_player.h"
#include "asw_marine_resource.h"
#endif

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

static const char s_szHexDigits[] = "0123456789abcdef";

#ifdef CLIENT_DLL
// This system is designed to do the following:
// - Send SteamInventoryResult_t objects to the server, along with a command that explains how the client wants the object used.
// - Handle a similar system for offline singleplayer sessions.
// - Avoid overloading the reliable send queue and causing a disconnect.
// - Handle malformed or corrupted responses in a safe way.
static class CRD_Inventory_Command_Queue final : public CAutoGameSystem
{
public:
	CRD_Inventory_Command_Queue() : CAutoGameSystem( "CRD_Inventory_Command_Queue" )
	{
		m_iNextPayloadID = 0;
	}

	~CRD_Inventory_Command_Queue()
	{
		DiscardAllPayloads();
	}

	void LevelInitPreEntity() override
	{
		DiscardAllPayloads();
		m_iNextPayloadID = 1;
	}

	void LevelShutdownPostEntity() override
	{
		DiscardAllPayloads();
		m_iNextPayloadID = 0;
	}

	void DiscardAllPayloads()
	{
		FOR_EACH_VEC( m_PayloadChunks, i )
		{
			m_PayloadChunks[i]->deleteThis();
		}

		m_PayloadChunks.Purge();

		if ( ISteamInventory *pInventory = SteamInventory())
		{
			FOR_EACH_VEC( m_ItemIDQueue, i )
			{
				pInventory->DestroyResult( m_ItemIDQueue[i].m_hResult );
			}
		}

		m_ItemIDQueue.Purge();
	}

	bool DiscardPayload( int iPayloadID )
	{
		bool bFoundAny = false;
		FOR_EACH_VEC_BACK( m_PayloadChunks, i )
		{
			if ( m_PayloadChunks[i]->GetInt( "p" ) == iPayloadID )
			{
				m_PayloadChunks[i]->deleteThis();
				m_PayloadChunks.Remove( i );
				bFoundAny = true;
			}
		}

		return bFoundAny;
	}

	void SendNextPayloadChunk( int iPayloadID )
	{
		Assert( m_PayloadChunks.Count() != 0 );
		if ( m_PayloadChunks.Count() == 0 )
		{
			Warning( "Received RDInvCmdAck for command %d but we have no commands pending!\n", iPayloadID );
			return;
		}

		Assert( m_PayloadChunks[0]->GetInt( "p" ) == iPayloadID );
		FOR_EACH_VEC( m_PayloadChunks, i )
		{
			if ( m_PayloadChunks[i]->GetInt( "p" ) == iPayloadID )
			{
				engine->ServerCmdKeyValues( m_PayloadChunks[i] );
				m_PayloadChunks.Remove( i );
				return;
			}
		}

		Warning( "Received RDInvCmdAck for command %d which we do not have!\n", iPayloadID );
	}

	int CreateCommandPayload( EInventoryCommand eCmd, const int *pArgs, size_t nArgs, const byte *pBuf, uint32_t nSize )
	{
		Assert( m_iNextPayloadID > 0 );

		int iPayloadID = m_iNextPayloadID;
		m_iNextPayloadID++;

		Assert( m_iNextPayloadID > 0 );

		KeyValues *pInit = new KeyValues( "InvCmdInit" );
		pInit->SetInt( "e", eCmd );
		pInit->SetInt( "p", iPayloadID );
		pInit->SetInt( "s", nSize );
		pInit->SetInt( "c", CRC32_ProcessSingleBuffer( pBuf, nSize ) );
		for ( size_t i = 0; i < nArgs; i++ )
		{
			KeyValues *pArg = new KeyValues( "a" );
			pArg->SetInt( NULL, pArgs[i] );
			pInit->AddSubKey( pArg );
		}
		engine->ServerCmdKeyValues( pInit );

		int nChunks = ( nSize + ( RD_INVCMD_PAYLOAD_CHUNK_STRSIZE / 2 - 1 ) ) / ( RD_INVCMD_PAYLOAD_CHUNK_STRSIZE / 2 );
		int iPayloadChunk = m_PayloadChunks.AddMultipleToTail( nChunks );

		char szChunk[RD_INVCMD_PAYLOAD_CHUNK_STRSIZE];
		int iOffset = 0;

		for ( int iChunk = 0; iChunk < nChunks; iChunk++ )
		{
			int j = 0;
			for ( uint32_t i = iOffset; i < nSize && j < RD_INVCMD_PAYLOAD_CHUNK_STRSIZE - 1; i++, j += 2 )
			{
				szChunk[j] = s_szHexDigits[pBuf[i] >> 4];
				szChunk[j + 1] = s_szHexDigits[pBuf[i] & 0xf];
			}
			szChunk[j] = '\0';

			KeyValues *pChunk = new KeyValues( "InvCmd" );
			pChunk->SetInt( "p", iPayloadID );
			pChunk->SetInt( "o", iOffset );
			pChunk->SetString( "d", szChunk );
			m_PayloadChunks[iPayloadChunk] = pChunk;

			iOffset += RD_INVCMD_PAYLOAD_CHUNK_STRSIZE / 2;
			iPayloadChunk++;
		}

		return iPayloadID;
	}

	void AbortInventoryCommand( int iCommandID )
	{
		if ( DiscardPayload( iCommandID ) )
		{
			engine->ServerCmdKeyValues( new KeyValues( "InvCmdAbort", "p", iCommandID ) );
		}
	}

	int m_iNextPayloadID;
	CUtlVector<KeyValues *> m_PayloadChunks;

	struct ItemIDCommand_t
	{
		EInventoryCommand m_eCmd;
		CCopyableUtlVector<int> m_Args;
		CCopyableUtlVector<SteamItemInstanceID_t> m_IDs;
		SteamInventoryResult_t m_hResult;
	};
	CUtlVector<ItemIDCommand_t> m_ItemIDQueue;
} s_RD_Inventory_Command_Queue;

int UTIL_RD_SendInventoryCommand( EInventoryCommand eCmd, const CUtlVector<int> &args, SteamInventoryResult_t hResult )
{
	Assert( s_RD_Inventory_Command_Queue.m_iNextPayloadID != 0 );
	if ( s_RD_Inventory_Command_Queue.m_iNextPayloadID == 0 )
	{
		Warning( "Programmer error: UTIL_RD_SendInventoryCommand(%d) called before the level had fully loaded or during a map transition.\n", eCmd );
		return 0;
	}

	uint32_t nSize = 0;
	CUtlMemory<byte> buf;

	if ( hResult != k_SteamInventoryResultInvalid )
	{
		ISteamInventory *pInventory = SteamInventory();
		Assert( pInventory );
		if ( !pInventory )
		{
			Warning( "UTIL_RD_SendInventoryCommand(%d) cannot access ISteamInventory!\n", eCmd );
			return 0;
		}

		bool bOK = pInventory->SerializeResult( hResult, NULL, &nSize );
		Assert( bOK );
		if ( !bOK )
		{
			Warning( "Programmer error: UTIL_RD_SendInventoryCommand(%d) failed to get serialized inventory result size. (handle: %08x)\n", eCmd, hResult );
			return 0;
		}

		buf.Init( 0, nSize );
		bOK = pInventory->SerializeResult( hResult, buf.Base(), &nSize );
		Assert( bOK );
		if ( !bOK )
		{
			Warning( "Programmer error: UTIL_RD_SendInventoryCommand(%d) failed to serialize inventory result. (handle: %08x, size: %u)\n", eCmd, hResult, nSize );
			return 0;
		}
	}

	return s_RD_Inventory_Command_Queue.CreateCommandPayload( eCmd, args.Base(), args.Count(), buf.Base(), nSize );
}

void UTIL_RD_SendInventoryCommandByIDs( EInventoryCommand eCmd, const CUtlVector<SteamItemInstanceID_t> &ids, const CUtlVector<int> &args )
{
	CUtlVector<SteamItemInstanceID_t> realIDs;
	realIDs.EnsureCapacity( ids.Count() );
	FOR_EACH_VEC( ids, i )
	{
		if ( ids[i] != k_SteamItemInstanceIDInvalid )
		{
			realIDs.AddToTail( ids[i] );
		}
	}

	if ( realIDs.Count() == 0 )
	{
		CUtlVector<int> args2;
		args2.AddVectorToTail( args );
		for ( int i = 0; i < ids.Count(); i++ )
		{
			args2.AddToTail( -1 );
		}

		UTIL_RD_SendInventoryCommand( eCmd, args2, k_SteamInventoryResultInvalid );
		return;
	}

	ISteamInventory *pInventory = SteamInventory();
	if ( !pInventory )
	{
		Warning( "Cannot send inventory command %d: no ISteamInventory\n", eCmd );
		return;
	}

	int i = s_RD_Inventory_Command_Queue.m_ItemIDQueue.AddToTail();
	s_RD_Inventory_Command_Queue.m_ItemIDQueue[i].m_eCmd = eCmd;
	s_RD_Inventory_Command_Queue.m_ItemIDQueue[i].m_Args.AddVectorToTail( args );
	s_RD_Inventory_Command_Queue.m_ItemIDQueue[i].m_IDs.AddVectorToTail( ids );
	pInventory->GetItemsByID( &s_RD_Inventory_Command_Queue.m_ItemIDQueue[i].m_hResult, realIDs.Base(), realIDs.Count() );
}

bool RDCheckForInventoryCommandResult( ISteamInventory *pInventory, SteamInventoryResult_t hResult )
{
	FOR_EACH_VEC( s_RD_Inventory_Command_Queue.m_ItemIDQueue, i )
	{
		const CRD_Inventory_Command_Queue::ItemIDCommand_t &cmd = s_RD_Inventory_Command_Queue.m_ItemIDQueue[i];
		if ( cmd.m_hResult != hResult )
			continue;

		CUtlVector<int> args;
		args.AddVectorToTail( cmd.m_Args );
		for ( int j = 0; j < cmd.m_IDs.Count(); j++ )
		{
			args.AddToTail( -1 );
		}

		uint32 nItems{};
		pInventory->GetResultItems( hResult, NULL, &nItems );
		CUtlMemory<SteamItemDetails_t> items{ 0, int( nItems ) };
		pInventory->GetResultItems( hResult, items.Base(), &nItems );

		FOR_EACH_VEC( cmd.m_IDs, j )
		{
			FOR_EACH_VEC( items, k )
			{
				if ( cmd.m_IDs[j] == items[k].m_itemId )
				{
					args[cmd.m_Args.Count() + j] = k;
					break;
				}
			}
		}

		UTIL_RD_SendInventoryCommand( cmd.m_eCmd, args, hResult );
		pInventory->DestroyResult( hResult );

		s_RD_Inventory_Command_Queue.m_ItemIDQueue.Remove( i );
		return true;
	}

	return false;
}

void UTIL_RD_SendInventoryCommandOffline( EInventoryCommand eCmd, const CUtlVector<int> &args, const CUtlVector<SteamItemInstanceID_t> &items )
{
	// we should only be calling this function in offline singleplayer mode (we need to be able to access the item cache on the server)
	Assert( gpGlobals->maxClients == 1 && engine->IsClientLocalToActiveServer() );

	Assert( s_RD_Inventory_Command_Queue.m_iNextPayloadID != 0 );
	if ( s_RD_Inventory_Command_Queue.m_iNextPayloadID == 0 )
	{
		Warning( "Programmer error: UTIL_RD_SendInventoryCommandOffline(%d) called before the level had fully loaded or during a map transition.\n", eCmd );
		return;
	}

	KeyValues *pCommand = new KeyValues( "InvCmdOffline" );
	pCommand->SetInt( "e", eCmd );
	FOR_EACH_VEC( args, i )
	{
		KeyValues *pArg = new KeyValues( "a" );
		pArg->SetInt( NULL, args[i] );
		pCommand->AddSubKey( pArg );
	}
	FOR_EACH_VEC( items, i )
	{
		KeyValues *pItem = new KeyValues( "i" );
		pItem->SetUint64( NULL, items[i] );
		pCommand->AddSubKey( pItem );
	}
	engine->ServerCmdKeyValues( pCommand );
}

void UTIL_RD_AbortInventoryCommand( int iCommandID )
{
	s_RD_Inventory_Command_Queue.AbortInventoryCommand( iCommandID );
}

static void __MsgFunc_RDInvCmdAck( bf_read &msg )
{
	bool bReject = msg.ReadOneBit();
	int iPayloadID = msg.ReadUBitLong( 31 );

	if ( bReject )
	{
		// Server doesn't want us to continue sending this command.
		s_RD_Inventory_Command_Queue.DiscardPayload( iPayloadID );
	}
	else
	{
		// server received a payload chunk; add the next to the reliable send buffer.
		s_RD_Inventory_Command_Queue.SendNextPayloadChunk( iPayloadID );
	}
}
USER_MESSAGE_REGISTER( RDInvCmdAck );
#else
CASW_Player::InventoryCommandData_t::~InventoryCommandData_t()
{
	if ( m_hResult != k_SteamInventoryResultInvalid )
	{
		ISteamInventory *pInventory = SteamInventory();
		if ( !pInventory )
		{
			pInventory = SteamGameServerInventory();
		}
		Assert( pInventory );

		if ( pInventory )
		{
			pInventory->DestroyResult( m_hResult );
		}
	}
}

static bool PreValidateInventoryCommand( CASW_Player *pPlayer, EInventoryCommand eCmd, const CUtlVector<int> &args, uint32_t nSize )
{
	size_t nMaxSize = 65536;
	int iArgsAreIndicesSkip = 0;
	int iArgsAreIndices = 0;
	switch ( eCmd )
	{
	case INVCMD_PLAYER_EQUIPS:
		iArgsAreIndices = RD_NUM_STEAM_INVENTORY_EQUIP_SLOTS_PLAYER;

		break;
	case INVCMD_MARINE_RESOURCE_EQUIPS:
		if ( args.Count() < 1 )
		{
			Warning( "Player %s sent invalid InvCmdInit - marine resource equip slots missing marine ID.\n", pPlayer->GetASWNetworkID() );
			return false;
		}

		if ( args[0] < 0 || args[0] >= ASW_MAX_MARINE_RESOURCES )
		{
			Warning( "Player %s sent invalid InvCmdInit - marine resource %d is out of range.\n", pPlayer->GetASWNetworkID(), args[0] );
			return false;
		}

		if ( !ASWGameResource() || !ASWGameResource()->GetMarineResource( args[0] ) )
		{
			Warning( "Player %s sent invalid InvCmdInit - marine resource %d does not exist.\n", pPlayer->GetASWNetworkID(), args[0] );
			return false;
		}

		if ( ASWGameResource()->GetMarineResource( args[0] )->GetCommander() != pPlayer )
		{
			Warning( "Player %s sent invalid InvCmdInit - marine resource %d is owned by a different player.\n", pPlayer->GetASWNetworkID(), args[0] );
			return false;
		}

		iArgsAreIndices = RD_NUM_STEAM_INVENTORY_EQUIP_SLOTS_MARINE_RESOURCE;
		iArgsAreIndicesSkip = 1;

		break;
	case INVCMD_MATERIAL_SPAWN:
#ifdef RD_7A_DROPS
		if ( pPlayer->m_bCraftingMaterialsSent )
		{
			Warning( "Player %s sent invalid InvCmdInit - already received an INVCMD_MATERIAL_SPAWN this mission.\n", pPlayer->GetASWNetworkID() );
			return false;
		}
		if ( args.Count() > RD_MAX_CRAFTING_MATERIAL_SPAWN_LOCATIONS )
		{
			Warning( "Player %s sent invalid InvCmdInit - wrong number of args for material spawn (%d).\n", pPlayer->GetASWNetworkID(), args.Count() );
			return false;
		}
		if ( const RD_Mission_t *pMission = ReactiveDropMissions::GetMission( STRING( gpGlobals->mapname ) ) )
		{
			FOR_EACH_VEC( args, i )
			{
				if ( args[i] <= RD_CRAFTING_MATERIAL_NONE || args[i] >= NUM_RD_CRAFTING_MATERIAL_TYPES )
				{
					Warning( "Player %s sent invalid InvCmdInit - out of range crafting material type (%d).\n", pPlayer->GetASWNetworkID(), args[i] );
					return false;
				}

				if ( !pMission->CraftingMaterialFoundHere( RD_Crafting_Material_t( args[i] ) ) )
				{
					Warning( "Player %s sent invalid InvCmdInit - material %s cannot be found in mission %s.\n", pPlayer->GetASWNetworkID(), g_RD_Crafting_Material_Info[args[i]].m_szName, STRING( gpGlobals->mapname ) );
					return false;
				}
			}
		}
		else
		{
			Warning( "Player %s sent invalid InvCmdInit - material spawn received but no mission record for %s.\n", pPlayer->GetASWNetworkID(), STRING( gpGlobals->mapname ) );
			return false;
		}
#endif
		break;
	case INVCMD_MATERIAL_PICKUP:
#ifdef RD_7A_DROPS
		if ( args.Count() != 1 )
		{
			Warning( "Player %s sent invalid InvCmdInit - wrong number of args for material pickup (%d).\n", pPlayer->GetASWNetworkID(), args.Count() );
			return false;
		}
		if ( args[0] < 0 || args[0] >= RD_MAX_CRAFTING_MATERIAL_SPAWN_LOCATIONS )
		{
			Warning( "Player %s sent invalid InvCmdInit - out of range location number for material pickup (%d).\n", pPlayer->GetASWNetworkID(), args[0] );
			return false;
		}
#endif
		break;
	case INVCMD_MATERIAL_PICKUP_ASSIST:
#ifdef RD_7A_DROPS
		if ( args.Count() != 2 )
		{
			Warning( "Player %s sent invalid InvCmdInit - wrong number of args for material pickup assist (%d).\n", pPlayer->GetASWNetworkID(), args.Count() );
			return false;
		}
		if ( args[0] < 0 || args[0] >= NUM_RD_CRAFTING_MATERIAL_TYPES )
		{
			Warning( "Player %s sent invalid InvCmdInit - out of range location number for material pickup assist (%d).\n", pPlayer->GetASWNetworkID(), args[0] );
			return false;
		}
		if ( !g_RD_Crafting_Material_Rarity_Info[g_RD_Crafting_Material_Info[args[0]].m_iRarity].m_bAllowPickupAssist )
		{
			Warning( "Player %s sent invalid InvCmdInit - invalid item type for material pickup assist (%d).\n", pPlayer->GetASWNetworkID(), args[0] );
			return false;
		}
		if ( args[1] < 0 )
		{
			Warning( "Player %s sent invalid InvCmdInit - negative quantity for material pickup assist (%d).\n", pPlayer->GetASWNetworkID(), args[1] );
			return false;
		}
#endif
		break;
	case INVCMD_PROMO_DROP:
		if ( args.Count() != 0 )
		{
			Warning( "Player %s sent invalid InvCmdInit - wrong number of args for promo drop (%d).\n", pPlayer->GetASWNetworkID(), args.Count() );
			return false;
		}
		break;
	default:
		Warning( "Player %s sent invalid InvCmdInit - bad command type %d.\n", pPlayer->GetASWNetworkID(), eCmd );
		return false;
	}

	if ( iArgsAreIndices != 0 )
	{
		if ( args.Count() != iArgsAreIndices + iArgsAreIndicesSkip )
		{
			Warning( "Player %s sent invalid InvCmdInit - wrong number of args for command type %d (%d, expecting %d).\n", pPlayer->GetASWNetworkID(), eCmd, args.Count(), iArgsAreIndices + iArgsAreIndicesSkip );
			return false;
		}

		nSize = MIN( nSize, 2048u * iArgsAreIndices );
	}

	if ( nSize > nMaxSize )
	{
		Warning( "Player %s sent invalid InvCmdInit - size %u is too big for command type %d.\n", pPlayer->GetASWNetworkID(), nSize, eCmd );
		return false;
	}

	return true;
}

static void UpdateAllEquipmentItemInstances( const ReactiveDropInventory::ItemInstance_t &instance )
{
#define CHECK( inst ) \
		if ( inst.m_iItemInstanceID == instance.ItemID ) \
		{ \
			Assert( inst.m_iItemDefID == instance.ItemDefID ); \
			inst.GetForModify().SetFromInstance( instance ); \
		}

	for ( int i = 1; i <= gpGlobals->maxClients; i++ )
	{
		CASW_Player *pPlayer = ToASW_Player( UTIL_PlayerByIndex( i ) );
		if ( !pPlayer )
			continue;

		COMPILE_TIME_ASSERT( RD_NUM_STEAM_INVENTORY_EQUIP_SLOTS_PLAYER == 3 );
		CHECK( pPlayer->m_EquippedItemData.m_Medal0 );
		CHECK( pPlayer->m_EquippedItemData.m_Medal1 );
		CHECK( pPlayer->m_EquippedItemData.m_Medal2 );
	}

	if ( CASW_Game_Resource *pGameResource = ASWGameResource() )
	{
		for ( int i = 0; i < ASW_MAX_MARINE_RESOURCES; i++ )
		{
			CASW_Marine_Resource *pMR = pGameResource->GetMarineResource( i );
			if ( !pMR )
				continue;

			COMPILE_TIME_ASSERT( RD_NUM_STEAM_INVENTORY_EQUIP_SLOTS_MARINE_RESOURCE == 4 );
			CHECK( pMR->m_EquippedItemData.m_Suit );
			CHECK( pMR->m_EquippedItemData.m_Weapon1 );
			CHECK( pMR->m_EquippedItemData.m_Weapon2 );
			CHECK( pMR->m_EquippedItemData.m_Extra );
		}
	}

#undef CHECK
}

static void ExecuteInventoryCommand( CASW_Player *pPlayer, EInventoryCommand eCmd, const CUtlVector<int> &args, const CUtlVector<ReactiveDropInventory::ItemInstance_t> &items )
{
	ISteamUtils *pUtils = SteamUtils();
#ifdef GAME_DLL
	if ( engine->IsDedicatedServer() )
		pUtils = SteamGameServerUtils();
#endif
	Assert( pUtils );
	RTime32 iNow = pUtils ? pUtils->GetServerRealTime() : std::time( NULL );
	RTime32 iFiveMinutesAgo = iNow - 300;

	switch ( eCmd )
	{
	case INVCMD_PLAYER_EQUIPS:
	{
		if ( args.Count() != RD_NUM_STEAM_INVENTORY_EQUIP_SLOTS_PLAYER )
		{
			Warning( "Player %s sent invalid INVCMD_PLAYER_EQUIPS - argument count %d does not match expected.\n", pPlayer->GetASWNetworkID(), args.Count() );
			return;
		}

		for ( int i = 0; i < RD_NUM_STEAM_INVENTORY_EQUIP_SLOTS_PLAYER; i++ )
		{
			if ( args[i] < 0 || args[i] >= items.Count() )
			{
				Assert( args[i] == -1 );
				pPlayer->m_EquippedItemData.GetForModify()[i].Reset();
				continue;
			}

			const ReactiveDropInventory::ItemDef_t *pDef = ReactiveDropInventory::GetItemDef( items[args[i]].ItemDefID );
			Assert( pDef );
			if ( !pDef )
			{
				continue;
			}

			if ( pDef->ItemSlotMatches( ReactiveDropInventory::g_PlayerInventorySlotNames[i] ) )
			{
				pPlayer->m_EquippedItemData.GetForModify()[i].SetFromInstance( items[args[i]] );
			}
			else
			{
				Warning( "Player %s sent invalid INVCMD_PLAYER_EQUIPS - item %llu (%d \"%s\") does not fit in slot \"%s\".\n", pPlayer->GetASWNetworkID(), items[args[i]].ItemID, items[args[i]].ItemDefID, pDef->Name.Get(), ReactiveDropInventory::g_PlayerInventorySlotNames[i] );
			}
		}

		FOR_EACH_VEC( items, i )
		{
			UpdateAllEquipmentItemInstances( items[i] );
		}

		break;
	}
	case INVCMD_MARINE_RESOURCE_EQUIPS:
	{
		if ( args.Count() != RD_NUM_STEAM_INVENTORY_EQUIP_SLOTS_MARINE_RESOURCE + 1 )
		{
			Warning( "Player %s sent invalid INVCMD_MARINE_RESOURCE_EQUIPS - argument count %d does not match expected.\n", pPlayer->GetASWNetworkID(), args.Count() );
			return;
		}

		if ( args[0] < 0 || args[0] >= ASW_MAX_MARINE_RESOURCES )
		{
			Warning( "Player %s sent invalid INVCMD_MARINE_RESOURCE_EQUIPS - marine resource %d is out of range.\n", pPlayer->GetASWNetworkID(), args[0] );
			return;
		}

		CASW_Game_Resource *pGameResource = ASWGameResource();
		Assert( pGameResource );
		if ( !pGameResource )
		{
			return;
		}

		CASW_Marine_Resource *pMR = pGameResource->GetMarineResource( args[0] );
		Assert( pMR );
		if ( !pMR )
		{
			Warning( "Player %s sent invalid INVCMD_MARINE_RESOURCE_EQUIPS - marine resource %d does not exist.\n", pPlayer->GetASWNetworkID(), args[0] );
			return;
		}

		if ( pMR->m_OriginalCommander.Get() != pPlayer )
		{
			Warning( "Player %s sent invalid INVCMD_MARINE_RESOURCE_EQUIPS - marine resource %d is commanded by a different player.\n", pPlayer->GetASWNetworkID(), args[0] );
			return;
		}

		for ( int i = 0; i < RD_NUM_STEAM_INVENTORY_EQUIP_SLOTS_MARINE_RESOURCE; i++ )
		{
			if ( args[i + 1] < 0 || args[i + 1] >= items.Count() )
			{
				Assert( args[i + 1] == -1 );
				pMR->m_EquippedItemData.GetForModify()[i].Reset();
				continue;
			}

			const ReactiveDropInventory::ItemDef_t *pDef = ReactiveDropInventory::GetItemDef( items[args[i + 1]].ItemDefID );
			Assert( pDef );
			if ( !pDef )
			{
				continue;
			}

			if ( pDef->ItemSlotMatches( ReactiveDropInventory::g_MarineResourceInventorySlotNames[i] ) )
			{
				pMR->m_EquippedItemData.GetForModify()[i].SetFromInstance( items[args[i + 1]] );
			}
			else
			{
				Warning( "Player %s sent invalid INVCMD_MARINE_RESOURCE_EQUIPS - item %llu (%d \"%s\") does not fit in slot \"%s\".\n", pPlayer->GetASWNetworkID(), items[args[i + 1]].ItemID, items[args[i + 1]].ItemDefID, pDef->Name.Get(), ReactiveDropInventory::g_MarineResourceInventorySlotNames[i] );
			}
		}

		FOR_EACH_VEC( items, i )
		{
			UpdateAllEquipmentItemInstances( items[i] );
		}

		pMR->ClearInvalidEquipData();

		break;
	}
	case INVCMD_MATERIAL_SPAWN:
	{
#ifdef RD_7A_DROPS
		if ( pPlayer->m_bCraftingMaterialsSent )
		{
			Warning( "Player %s sent invalid InvCmd - already received an INVCMD_MATERIAL_SPAWN this mission.\n", pPlayer->GetASWNetworkID() );
			return;
		}

		int *pQuantity = ( int * )stackalloc( items.Count() * sizeof( int ) );
		FOR_EACH_VEC( items, i )
		{
			pQuantity[i] = items[i].Quantity;
		}

		FOR_EACH_VEC( args, i )
		{
			bool bHaveToken = false;
			FOR_EACH_VEC( items, j )
			{
				if ( items[j].ItemDefID != g_RD_Crafting_Material_Info[args[i]].m_iTokenDef )
					continue;

				if ( pQuantity[j] <= 0 )
					continue;

				pQuantity[j]--;
				bHaveToken = true;
				break;
			}

			if ( !bHaveToken )
			{
				Warning( "Player %s sent invalid InvCmd - INVCMD_MATERIAL_SPAWN with no token for %s.\n", pPlayer->GetASWNetworkID(), g_RD_Crafting_Material_Info[args[i]].m_szName );
				return;
			}
		}

		pPlayer->m_bCraftingMaterialsSent = true;
		FOR_EACH_VEC( args, i )
		{
			Assert( pPlayer->m_CraftingMaterialType[i] == RD_CRAFTING_MATERIAL_NONE );
			pPlayer->m_CraftingMaterialType[i] = RD_Crafting_Material_t( args[i] );
		}
		Assert( pPlayer->m_CraftingMaterialFound == 0 );
		Assert( pPlayer->m_CraftingMaterialInstances.Count() == 0 );
		pPlayer->m_CraftingMaterialInstances.AddVectorToTail( items );
#endif
		break;
	}
	case INVCMD_MATERIAL_PICKUP:
	{
#ifdef RD_7A_DROPS
		Assert( pPlayer->m_bCraftingMaterialsSent );

		int iLocation = args[0];
		if ( pPlayer->m_CraftingMaterialFound & ( 1 << iLocation ) )
		{
			Warning( "Player %s sent invalid InvCmd - already received an INVCMD_MATERIAL_PICKUP for location %d.\n", pPlayer->GetASWNetworkID(), iLocation );
			return;
		}
		RD_Crafting_Material_t eMaterial = pPlayer->m_CraftingMaterialType[iLocation];
		if ( eMaterial == RD_CRAFTING_MATERIAL_NONE )
		{
			Warning( "Player %s sent invalid InvCmd - INVCMD_MATERIAL_PICKUP for empty location %d.\n", pPlayer->GetASWNetworkID(), iLocation );
			return;
		}

		pPlayer->m_CraftingMaterialFound |= ( 1 << iLocation );

		CBaseEntity *pEnt = NULL;
		bool bFoundLocation = false;
		while ( ( pEnt = gEntList.FindEntityByClassname( pEnt, "rd_crafting_material_pickup" ) ) != NULL )
		{
			CRD_Crafting_Material_Pickup *pPickup = assert_cast< CRD_Crafting_Material_Pickup * >( pEnt );
			if ( pPickup->m_iLocation != iLocation )
				continue;

			if ( pPickup->m_MaterialAtLocation[pPlayer->entindex() - 1] != RD_CRAFTING_MATERIAL_NONE )
			{
				Assert( eMaterial == pPickup->m_MaterialAtLocation[pPlayer->entindex() - 1] );
				Warning( "Player %s sent invalid InvCmd - INVCMD_MATERIAL_PICKUP for location %d which has not been interacted with yet.\n", pPlayer->GetASWNetworkID(), iLocation );
				return;
			}

			bFoundLocation = true;
			break;
		}

		if ( !bFoundLocation )
		{
			Warning( "Player %s sent invalid InvCmd - received an INVCMD_MATERIAL_PICKUP for location %d which does not exist.\n", pPlayer->GetASWNetworkID(), iLocation );
			return;
		}

		// make sure the location's material matches the exchange result given by the player
		bool bConsumed = false;
		int nTotalQuantity = 0;

		FOR_EACH_VEC_BACK( items, i )
		{
			bool bFound = false;
			// When generating a random number of items using nested generators and bundles, the result will contain item IDs multiple times.
			// The last instance of each item ID is the "real" result for that item. Therefore, we check first to make sure we haven't already processed it:
			FOR_EACH_VEC_BACK( items, j )
			{
				if ( j <= i )
					break;

				if ( items[i].ItemID != items[j].ItemID )
					continue;

				bFound = true;
				break;
			}

			// If we already processed a later snapshot of this item, just ignore this snapshot.
			if ( bFound )
				continue;

			FOR_EACH_VEC( pPlayer->m_CraftingMaterialInstances, j )
			{
				const ReactiveDropInventory::ItemInstance_t prev = pPlayer->m_CraftingMaterialInstances[j];
				if ( prev.ItemID != items[i].ItemID )
					continue;

				bFound = true;
				pPlayer->m_CraftingMaterialInstances[j] = items[i];

				if ( items[i].ItemDefID == g_RD_Crafting_Material_Info[eMaterial].m_iTokenDef && items[i].Quantity < prev.Quantity )
				{
					bConsumed = true;

					Assert( items[i].Quantity == prev.Quantity - 1 );
					if ( items[i].Quantity != prev.Quantity - 1 )
					{
						Warning( "Player %s sent INVCMD_MATERIAL_PICKUP with unexpected decrease in token \"%s\" (%d) quantity by %d (expecting 1).\n", pPlayer->GetASWNetworkID(), g_RD_Crafting_Material_Info[eMaterial].m_szName, prev.ItemDefID, prev.Quantity - items[i].Quantity );

						// ignore the anomaly and keep going
					}

					break;
				}

				if ( items[i].ItemDefID == g_RD_Crafting_Material_Info[eMaterial].m_iItemDef )
				{
					nTotalQuantity += items[i].Quantity - prev.Quantity;

					break;
				}

				const ReactiveDropInventory::ItemDef_t *pDef = ReactiveDropInventory::GetItemDef( items[i].ItemDefID );
				Warning( "Player %s sent INVCMD_MATERIAL_PICKUP for material %s with unexpected item %llu #%d %c%s%c (quantity %d, previous %d, o:%s, s:%s).\n", pPlayer->GetASWNetworkID(), g_RD_Crafting_Material_Info[eMaterial].m_szName, prev.ItemID, prev.ItemDefID, pDef ? '"' : '<', pDef ? pDef->Name.Get() : "missing item def", pDef ? '"' : '>', items[i].Quantity, prev.Quantity, items[i].Origin.Get(), items[i].State.Get() );

				// ignore the anomaly and keep going

				break;
			}

			if ( !bFound )
			{
				pPlayer->m_CraftingMaterialInstances.AddToTail( items[i] );

				if ( items[i].ItemDefID == g_RD_Crafting_Material_Info[eMaterial].m_iItemDef && items[i].Acquired > iFiveMinutesAgo )
				{
					nTotalQuantity += items[i].Quantity;
					continue;
				}

				const ReactiveDropInventory::ItemDef_t *pDef = ReactiveDropInventory::GetItemDef( items[i].ItemDefID );
				Warning( "Player %s sent INVCMD_MATERIAL_PICKUP for material %s with unexpected item %llu #%d %c%s%c (quantity %d, no previous snapshot, o:%s, s:%s).\n", pPlayer->GetASWNetworkID(), g_RD_Crafting_Material_Info[eMaterial].m_szName, items[i].ItemID, items[i].ItemDefID, pDef ? '"' : '<', pDef ? pDef->Name.Get() : "missing item def", pDef ? '"' : '>', items[i].Quantity, items[i].Origin.Get(), items[i].State.Get() );

				// ignore the anomaly and keep going
			}
		}

		if ( nTotalQuantity <= 0 )
		{
			Warning( "Player %s sent invalid InvCmd - INVCMD_MATERIAL_PICKUP with no gained materials material type %s (location %d).\n", pPlayer->GetASWNetworkID(), g_RD_Crafting_Material_Info[eMaterial].m_szName, iLocation );
			return;
		}
		if ( !bConsumed )
		{
			Warning( "Player %s sent invalid InvCmd - INVCMD_MATERIAL_PICKUP with no consumed locations for material type %s (location %d).\n", pPlayer->GetASWNetworkID(), g_RD_Crafting_Material_Info[eMaterial].m_szName, iLocation );
			return;
		}

		CReliableBroadcastRecipientFilter filter;
		UserMessageBegin( filter, "RDItemPickupMsg" );
		WRITE_BYTE( 2 );
		WRITE_BYTE( pPlayer->entindex() );
		WRITE_LONG( g_RD_Crafting_Material_Info[eMaterial].m_iItemDef );
		WRITE_LONG( nTotalQuantity );
		MessageEnd();
#endif
		break;
	}
	case INVCMD_MATERIAL_PICKUP_ASSIST:
	{
#ifdef RD_7A_DROPS
		// don't bother checking the inventory result for now; just require one so it's hard to make a fake message

		CReliableBroadcastRecipientFilter filter;
		UserMessageBegin( filter, "RDItemPickupMsg" );
		WRITE_BYTE( 3 );
		WRITE_BYTE( pPlayer->entindex() );
		WRITE_LONG( g_RD_Crafting_Material_Info[args[0]].m_iItemDef );
		WRITE_LONG( args[1] );
		MessageEnd();
#endif
		break;
	}
	case INVCMD_PROMO_DROP:
	{
		CReliableBroadcastRecipientFilter filter;
		FOR_EACH_VEC( items, i )
		{
			// this doesn't allow auto_stack promo items through if the first of that item wasn't just received, so... don't make auto_stack promo items.
			Assert( items[i].Origin == "promo" );
			if ( items[i].Origin != "promo" )
			{
				Warning( "Player %s sent invalid InvCmd - INVCMD_PROMO_DROP item %d has origin %s.\n", pPlayer->GetASWNetworkID(), items[i].ItemDefID, items[i].Origin.Get() );
				continue;
			}
			Assert( items[i].Acquired >= iFiveMinutesAgo );
			if ( items[i].Acquired < iFiveMinutesAgo )
			{
				Warning( "Player %s sent invalid InvCmd - INVCMD_PROMO_DROP item %d acquired %d seconds ago.\n", pPlayer->GetASWNetworkID(), items[i].ItemDefID, iNow - items[i].Acquired );
				continue;
			}

			UserMessageBegin( filter, "RDItemPickupMsg" );
				WRITE_BYTE( 0 );
				WRITE_BYTE( pPlayer->entindex() );
				WRITE_LONG( items[i].ItemDefID );
				WRITE_LONG( items[i].Quantity );
			MessageEnd();
		}
		break;
	}
	default:
	{
		Assert( 0 ); // shouldn't be reachable; this should have failed the pre-verify check.
		Warning( "Player %s sent invalid InvCmd - bad command type %d.\n", pPlayer->GetASWNetworkID(), eCmd );
		return;
	}
	}
}

void CASW_Player::HandleInventoryCommandInit( KeyValues *pKeyValues )
{
	EInventoryCommand eCmd = ( EInventoryCommand )pKeyValues->GetInt( "e" );
	int iPayloadID = pKeyValues->GetInt( "p" );
	uint32_t nSize = pKeyValues->GetInt( "s" );
	CRC32_t iCRC = pKeyValues->GetInt( "c" );
	CUtlVector<int> args;
	FOR_EACH_VALUE( pKeyValues, pValue )
	{
		if ( !V_stricmp( pValue->GetName(), "a" ) )
		{
			args.AddToTail( pValue->GetInt() );
		}
	}

	if ( iPayloadID <= 0 )
	{
		Warning( "Player %s sent invalid InvCmdInit - bad command number %d.\n", GetASWNetworkID(), iPayloadID );
		RejectInventoryCommand( iPayloadID );
		return;
	}

	if ( m_InventoryCommands.Count() >= 64 )
	{
		Warning( "Player %s sent InvCmdInit (%d) but too many commands are already pending. Rejecting.\n", GetASWNetworkID(), eCmd );
		RejectInventoryCommand( iPayloadID );
		return;
	}

	if ( !PreValidateInventoryCommand( this, eCmd, args, nSize ) )
	{
		// warning message printed in above function.
		RejectInventoryCommand( iPayloadID );
		return;
	}

	if ( nSize == 0 )
	{
		CUtlVector<ReactiveDropInventory::ItemInstance_t> noItems;
		ExecuteInventoryCommand( this, eCmd, args, noItems );
		return;
	}

	bool bAlreadyPending = false;
	FOR_EACH_VEC( m_InventoryCommands, i )
	{
		if ( m_InventoryCommands[i]->m_Data.Count() > m_InventoryCommands[i]->m_iOffset )
		{
			bAlreadyPending = true;
		}

		if ( m_InventoryCommands[i]->m_iCommandID == iPayloadID )
		{
			Warning( "Player %s sent invalid InvCmdInit - command number %d is already in use.\n", GetASWNetworkID(), iPayloadID );
			return;
		}
	}

	InventoryCommandData_t *pCommand = new InventoryCommandData_t;
	pCommand->m_iCommandID = iPayloadID;
	pCommand->m_eCommand = eCmd;
	pCommand->m_iArgs.AddVectorToTail( args );
	pCommand->m_CRC = iCRC;
	pCommand->m_Data.Init( 0, nSize );
	m_InventoryCommands.AddToTail( pCommand );

	if ( !bAlreadyPending )
	{
		CSingleUserRecipientFilter filter( this );
		filter.MakeReliable();

		UserMessageBegin( filter, "RDInvCmdAck" );
			WRITE_BOOL( false );
			WRITE_UBITLONG( iPayloadID, 31 );
		MessageEnd();
	}
}

void CASW_Player::HandleInventoryCommandAbort( KeyValues *pKeyValues )
{
	int iPayloadID = pKeyValues->GetInt( "p" );

	bool bAbortedCommandPending = false;
	bool bAnotherPending = false;
	FOR_EACH_VEC( m_InventoryCommands, i )
	{
		if ( m_InventoryCommands[i]->m_iCommandID == iPayloadID )
		{
			bAbortedCommandPending = m_InventoryCommands[i]->m_Data.Count() > m_InventoryCommands[i]->m_iOffset;

			delete m_InventoryCommands[i];
			m_InventoryCommands.Remove( i );
			break;
		}

		if ( m_InventoryCommands[i]->m_Data.Count() > m_InventoryCommands[i]->m_iOffset )
		{
			bAnotherPending = true;
		}
	}

	if ( bAbortedCommandPending && !bAnotherPending )
	{
		FOR_EACH_VEC( m_InventoryCommands, i )
		{
			if ( m_InventoryCommands[i]->m_Data.Count() > m_InventoryCommands[i]->m_iOffset )
			{
				CSingleUserRecipientFilter filter( this );
				filter.MakeReliable();

				UserMessageBegin( filter, "RDInvCmdAck" );
					WRITE_BOOL( false );
					WRITE_UBITLONG( m_InventoryCommands[i]->m_iCommandID, 31 );
				MessageEnd();
				return;
			}
		}
	}
}

void CASW_Player::HandleInventoryCommand( KeyValues *pKeyValues )
{
	int iPayloadID = pKeyValues->GetInt( "p" );
	int iOffset = pKeyValues->GetInt( "o" );
	const char *szData = pKeyValues->GetString( "d" );
	int iLength = V_strlen( szData );

	if ( iLength >= RD_INVCMD_PAYLOAD_CHUNK_STRSIZE )
	{
		Warning( "Player %s sent invalid InvCmd - data chunk size (%d) is above max size (%d).\n", GetASWNetworkID(), iLength + 1, RD_INVCMD_PAYLOAD_CHUNK_STRSIZE );
		RejectInventoryCommand( iPayloadID );
		return;
	}

	FOR_EACH_VEC( m_InventoryCommands, i )
	{
		// we should always be filling the first (oldest) buffer.
		Assert( m_InventoryCommands[i]->m_iCommandID == iPayloadID || m_InventoryCommands[i]->m_hResult != k_SteamInventoryResultInvalid );

		if ( m_InventoryCommands[i]->m_iCommandID == iPayloadID )
		{
			if ( m_InventoryCommands[i]->m_Data.Count() == 0 )
			{
				Warning( "Player %s sent invalid InvCmd - received %d bytes of data after end of payload.\n", GetASWNetworkID(), iLength );
				// don't discard as we're already validating it.
				return;
			}

			Assert( m_InventoryCommands[i]->m_iOffset == iOffset );
			if ( m_InventoryCommands[i]->m_iOffset != iOffset )
			{
				Warning( "Player %s sent invalid InvCmd - incorrect offset (%d, expecting %d).\n", GetASWNetworkID(), iOffset, m_InventoryCommands[i]->m_iOffset );
				RejectInventoryCommand( iPayloadID );
				return;
			}

			int iRemaining = m_InventoryCommands[i]->m_Data.Count() - iOffset;
			int iExpectedLength = MIN( iRemaining * 2, RD_INVCMD_PAYLOAD_CHUNK_STRSIZE - 1 );
			if ( iLength != iExpectedLength )
			{
				Warning( "Player %s sent invalid InvCmd - incorrect length (%d, expecting %d).\n", GetASWNetworkID(), iLength, iExpectedLength );
				RejectInventoryCommand( iPayloadID );
				return;
			}

			Assert( iOffset + iLength / 2 <= m_InventoryCommands[i]->m_Data.Count() );
			V_hextobinary( szData, iLength, m_InventoryCommands[i]->m_Data.Base() + iOffset, m_InventoryCommands[i]->m_Data.Count() - iOffset );
			m_InventoryCommands[i]->m_iOffset += iLength / 2;

			Assert( m_InventoryCommands[i]->m_iOffset <= m_InventoryCommands[i]->m_Data.Count() );
			if ( m_InventoryCommands[i]->m_iOffset == m_InventoryCommands[i]->m_Data.Count() )
			{
				CRC32_t iCRC = CRC32_ProcessSingleBuffer( m_InventoryCommands[i]->m_Data.Base(), m_InventoryCommands[i]->m_Data.Count() );
				if ( iCRC != m_InventoryCommands[i]->m_CRC )
				{
					Warning( "Player %s sent invalid InvCmd - incorrect checksum (%08x, claimed %08x).\n", GetASWNetworkID(), iCRC, m_InventoryCommands[i]->m_CRC );
					delete m_InventoryCommands[i];
					m_InventoryCommands.Remove( i );
					return;
				}

				ISteamInventory *pInventory = SteamInventory();
				if ( !pInventory )
				{
					pInventory = SteamGameServerInventory();
				}

				Assert( pInventory );
				if ( !pInventory )
				{
					Warning( "Failed to handle InvCmd from player %s - ISteamInventory interface not available!\n", GetASWNetworkID() );
					delete m_InventoryCommands[i];
					m_InventoryCommands.Remove( i );
					return;
				}

				// send the serialized result off to be validated.
				pInventory->DeserializeResult( &m_InventoryCommands[i]->m_hResult, m_InventoryCommands[i]->m_Data.Base(), m_InventoryCommands[i]->m_Data.Count() );
				Assert( m_InventoryCommands[i]->m_hResult != k_SteamInventoryResultInvalid );
				// throw away the serialized payload; we no longer need it in memory.
				m_InventoryCommands[i]->m_Data.Purge();
			}

			FOR_EACH_VEC( m_InventoryCommands, j )
			{
				if ( m_InventoryCommands[j]->m_Data.Count() > m_InventoryCommands[j]->m_iOffset )
				{
					CSingleUserRecipientFilter filter( this );
					filter.MakeReliable();

					UserMessageBegin( filter, "RDInvCmdAck" );
						WRITE_BOOL( false );
						WRITE_UBITLONG( m_InventoryCommands[j]->m_iCommandID, 31 );
					MessageEnd();

					break;
				}
			}

			return;
		}
	}

	Warning( "Player %s sent invalid InvCmd - unknown command ID %d.\n", GetASWNetworkID(), iPayloadID );
	RejectInventoryCommand( iPayloadID );
}

void CASW_Player::HandleOfflineInventoryCommand( KeyValues *pKeyValues )
{
	Assert( !engine->IsDedicatedServer() );
	Assert( gpGlobals->maxClients == 1 );
	Assert( SteamUser() );

	// This only works in singleplayer, and we need access to our own Steam ID.
	if ( engine->IsDedicatedServer() || gpGlobals->maxClients != 1 || !SteamUser() )
		return;

	CFmtStr szCacheFileName{ "cfg/clienti_%llu.dat", SteamUser()->GetSteamID().ConvertToUint64() };
	CUtlBuffer buf;

	if ( !g_pFullFileSystem->ReadFile( szCacheFileName, "MOD", buf ) )
		return;

	KeyValues::AutoDelete pCache{ "IC" };

	if ( !pCache->ReadAsBinary( buf ) )
		return;

	EInventoryCommand eCmd = ( EInventoryCommand )pKeyValues->GetInt( "e" );
	CUtlVector<int> args;
	CUtlVector<ReactiveDropInventory::ItemInstance_t> items;

	FOR_EACH_VALUE( pKeyValues, pValue )
	{
		if ( !V_stricmp( pValue->GetName(), "a" ) )
		{
			args.AddToTail( pValue->GetInt() );
		}

		if ( !V_stricmp( pValue->GetName(), "i" ) )
		{
			int i = items.AddToTail();
			SteamItemInstanceID_t id = pValue->GetUint64();

			bool bFound = false;

			FOR_EACH_SUBKEY( pCache, pItem )
			{
				if ( pItem->GetUint64( "i" ) != id )
				{
					continue;
				}

				items[i].FromKeyValues( pItem );
				bFound = true;

				break;
			}

			if ( !bFound )
			{
				Warning( "Player %s sent invalid InvCmdOffline - no cached item with ID %llu.\n", GetASWNetworkID(), id );
				return;
			}
		}
	}

	if ( PreValidateInventoryCommand( this, eCmd, args, 0 ) )
	{
		ExecuteInventoryCommand( this, eCmd, args, items );
	}
}

bool CASW_Player::OnSteamInventoryResultReady( SteamInventoryResultReady_t *pParam )
{
	Assert( pParam->m_handle != k_SteamInventoryResultInvalid );
	if ( pParam->m_handle == k_SteamInventoryResultInvalid )
	{
		return false;
	}

	ISteamInventory *pInventory = SteamInventory();
	if ( !pInventory )
	{
		pInventory = SteamGameServerInventory();
	}
	Assert( pInventory );
	if ( !pInventory )
	{
		return false;
	}

	FOR_EACH_VEC( m_InventoryCommands, i )
	{
		if ( m_InventoryCommands[i]->m_hResult == pParam->m_handle )
		{
			if ( pParam->m_result != k_EResultOK )
			{
				Warning( "Failure handling InvCmd from player %s: inventory result is %d (%s)\n", GetASWNetworkID(), pParam->m_result, UTIL_RD_EResultToString( pParam->m_result ) );
			}
			else if ( !pInventory->CheckResultSteamID( m_InventoryCommands[i]->m_hResult, GetSteamID() ) )
			{
				Warning( "Failure handling InvCmd from player %s: inventory result does not belong to Steam ID %llu\n", GetASWNetworkID(), GetSteamID().ConvertToUint64() );
			}
			else
			{
				uint32_t nItems = 0;
				pInventory->GetResultItems( m_InventoryCommands[i]->m_hResult, NULL, &nItems );

				CUtlVector<ReactiveDropInventory::ItemInstance_t> items;
				items.SetCount( nItems );
				FOR_EACH_VEC( items, j )
				{
					items[j] = ReactiveDropInventory::ItemInstance_t( m_InventoryCommands[i]->m_hResult, j );
				}

				ExecuteInventoryCommand( this, m_InventoryCommands[i]->m_eCommand, m_InventoryCommands[i]->m_iArgs, items );
			}

			delete m_InventoryCommands[i];
			m_InventoryCommands.Remove( i );

			return true;
		}
	}

	return false;
}

void CASW_Player::RejectInventoryCommand( int iPayloadID )
{
	FOR_EACH_VEC( m_InventoryCommands, i )
	{
		if ( m_InventoryCommands[i]->m_iCommandID == iPayloadID )
		{
			delete m_InventoryCommands[i];
			m_InventoryCommands.Remove( i );
			break;
		}
	}

	CSingleUserRecipientFilter filter( this );
	filter.MakeReliable();

	UserMessageBegin( filter, "RDInvCmdAck" );
		WRITE_BOOL( true );
		WRITE_UBITLONG( iPayloadID, 31 );
	MessageEnd();
}
#endif
