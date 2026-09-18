// SPDX-License-Identifier: MIT
// Copyright (c) 2026
//
// rd_d3d9hook.cpp
//
// Minimal D3D9 hook for the GPU video path (DXVA2):
//  - obtains the game's IDirect3DDevice9* deterministically from the engine
//    binary (shaderapidx9.dll stores the raw device in global g_pD3DDevice),
//  - hooks IDirect3DDevice9::SetRenderTarget (slot 37) to capture the native
//    surface of our video render target,
//  - hooks IDirect3DDevice9::Reset (slot 16) with pre/post observers so DXVA2
//    resources are released before the reset and recreated after it.
//
// From disassembly of the shipped shaderapidx9.dll (ImageBase 0x10000000):
//   10024B55  call 10024910          ; InvokeCreateDevice -> eax = device
//   10024B5A  mov ebp, eax           ; device saved
//   10024B77  call 1000CA10          ; getter: mov eax, 1017B22Ch
//   10024B83  mov [eax], ebp         ; *g_pD3DDevice = device
//   => global device at RVA 0x17B22C.
//
// The engine uses Direct3DCreate9 + CreateDevice (NON-Ex), so the device vtable
// is the plain IDirect3DDevice9 vtable.
//
// The vtable is patched IN PLACE (individual slots) rather than by replacing the
// object's vtable pointer with a copy: d3d9.dll implements the device as a C++
// object (CD3DBase) whose vtable carries internal virtual methods AFTER the 119
// COM methods. Swapping the pointer to a 119-entry copy made the runtime read
// past the copy and jump into arbitrary client.dll code (observed crash inside
// copy_bytes_avx2 right after hook installation).

#include "cbase.h"

#ifdef _WIN32
#undef INVALID_HANDLE_VALUE
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

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

#if defined WIN32 && !defined _X360

// IDirect3DDevice9 vtable: 119 methods (0..118). NOTE: d3d9.dll's internal
// CD3DBase vtable is longer than this - we must never replace the vtable
// pointer with a 119-entry copy, only patch individual slots in place.
#define DEVICE_VTABLE_COUNT          119
#define DEVICE_SETRENDERTARGET_INDEX 37
#define DEVICE_RESET_INDEX           16

// Global g_pD3DDevice inside shaderapidx9.dll (from binary analysis).
#define SHADERAPI_DEVICE_RVA         0x17B22C

typedef HRESULT (STDMETHODCALLTYPE *SetRenderTargetFn)( IDirect3DDevice9 *pDevice, DWORD RenderTargetIndex, IDirect3DSurface9 *pRenderTarget );
typedef HRESULT (STDMETHODCALLTYPE *ResetFn)( IDirect3DDevice9 *pDevice, D3DPRESENT_PARAMETERS *pPresentationParameters );

// ---------------------------------------------------------------------------
// Module state
// ---------------------------------------------------------------------------
static IDirect3DDevice9 *s_pDevice = nullptr;
static SetRenderTargetFn s_pRealSetRenderTarget = nullptr;
static ResetFn s_pRealReset = nullptr;
static bool s_bDeviceVtableHooked = false;

// SetRenderTarget capture state (native surface of our video render target)
static bool s_bCaptureRTArmed = false;
static IDirect3DSurface9 *s_pCapturedRTSurface = nullptr;

// Device reset observers (DXVA2 lifecycle: pre/post IDirect3DDevice9::Reset)
#define RD_D3D9_MAX_RESET_CALLBACKS 8
static RD_D3D9::ResetCallbackFn s_pResetCallbacks[ RD_D3D9_MAX_RESET_CALLBACKS ] = {};
static int s_nResetCallbackCount = 0;

// ---------------------------------------------------------------------------
// Device reset observers
// ---------------------------------------------------------------------------
static void FireResetCallbacks( bool bPreReset )
{
	for ( int i = 0; i < s_nResetCallbackCount; ++i )
	{
		if ( s_pResetCallbacks[ i ] )
			s_pResetCallbacks[ i ]( bPreReset );
	}
}

// ---------------------------------------------------------------------------
// Hooked device methods
// ---------------------------------------------------------------------------
static HRESULT STDMETHODCALLTYPE HookedSetRenderTarget( IDirect3DDevice9 *pDevice, DWORD RenderTargetIndex, IDirect3DSurface9 *pRenderTarget )
{
	HRESULT hr = s_pRealSetRenderTarget( pDevice, RenderTargetIndex, pRenderTarget );
	if ( s_bCaptureRTArmed && RenderTargetIndex == 0 && SUCCEEDED( hr ) && pRenderTarget )
	{
		if ( s_pCapturedRTSurface )
			s_pCapturedRTSurface->Release();
		s_pCapturedRTSurface = pRenderTarget;
		s_pCapturedRTSurface->AddRef();
		s_bCaptureRTArmed = false;
	}
	return hr;
}

static void PatchDeviceVtable( IDirect3DDevice9 *pDevice ); // defined below

static HRESULT STDMETHODCALLTYPE HookedReset( IDirect3DDevice9 *pDevice, D3DPRESENT_PARAMETERS *pPresentationParameters )
{
	// Pre-reset: release/invalidate any DXVA2 decoder surfaces and video
	// processors BEFORE the device tears down its resources.
	FireResetCallbacks( true );

	HRESULT hr = s_pRealReset( pDevice, pPresentationParameters );
	if ( SUCCEEDED( hr ) )
	{
		// d3d9 may rebuild/swap the device vtable during Reset; re-apply our patches.
		PatchDeviceVtable( pDevice );

		// Post-reset: re-bind the DXVA2 device manager (ResetDevice), recreate
		// video processors / render targets / decoder hw frames context.
		FireResetCallbacks( false );
	}
	return hr;
}

// ---------------------------------------------------------------------------
// Device vtable hooking
//
// Slots are patched IN PLACE on the original vtable; the object's vtable
// pointer is left untouched. Replacing the pointer with a 119-entry copy is
// fatal: d3d9.dll's device is a C++ object (CD3DBase) whose vtable carries
// internal virtual methods AFTER the 119 COM methods, and the runtime calls
// them through the same vtable pointer. A truncated copy made those calls
// read past it into client.dll static data (crash in copy_bytes_avx2).
// ---------------------------------------------------------------------------
static void PatchDeviceVtable( IDirect3DDevice9 *pDevice )
{
	void **pVtable = *reinterpret_cast< void *** >( pDevice );
	if ( !pVtable )
		return;

	// Save the real pointers exactly once (the first read always sees the
	// pristine vtable; re-reading after our patch would capture the hooks).
	if ( s_pRealSetRenderTarget == nullptr )
		s_pRealSetRenderTarget = reinterpret_cast< SetRenderTargetFn >( pVtable[ DEVICE_SETRENDERTARGET_INDEX ] );
	if ( s_pRealReset == nullptr )
		s_pRealReset = reinterpret_cast< ResetFn >( pVtable[ DEVICE_RESET_INDEX ] );

	if ( !s_pRealSetRenderTarget || !s_pRealReset )
		return;

	DWORD oldProtect;
	VirtualProtect( pVtable, DEVICE_VTABLE_COUNT * sizeof( void * ), PAGE_READWRITE, &oldProtect );
	pVtable[ DEVICE_SETRENDERTARGET_INDEX ] = reinterpret_cast< void * >( &HookedSetRenderTarget );
	pVtable[ DEVICE_RESET_INDEX ] = reinterpret_cast< void * >( &HookedReset );
	VirtualProtect( pVtable, DEVICE_VTABLE_COUNT * sizeof( void * ), oldProtect, &oldProtect );
}

static bool HookDeviceVtable( IDirect3DDevice9 *pDevice )
{
	if ( s_bDeviceVtableHooked )
		return true;

	PatchDeviceVtable( pDevice );

	s_bDeviceVtableHooked = true;
	Msg( "  [rd_d3d9] device vtable patched in place (SetRenderTarget/Reset)\n" );
	return true;
}

// ---------------------------------------------------------------------------
// Find the device: read g_pD3DDevice global from shaderapidx9.dll.
// ---------------------------------------------------------------------------
static IDirect3DDevice9 *FindDeviceViaGlobal()
{
	HMODULE hShaderAPI = GetModuleHandleA( "shaderapidx9.dll" );
	if ( !hShaderAPI )
	{
		Msg( "  [rd_d3d9] shaderapidx9.dll not loaded\n" );
		return nullptr;
	}

	IDirect3DDevice9 **ppDevice = reinterpret_cast< IDirect3DDevice9 ** >( reinterpret_cast< uint8_t * >( hShaderAPI ) + SHADERAPI_DEVICE_RVA );
	IDirect3DDevice9 *pDevice = nullptr;

	SIZE_T nRead = 0;
	if ( !ReadProcessMemory( GetCurrentProcess(), ppDevice, &pDevice, sizeof( pDevice ), &nRead ) || nRead != sizeof( pDevice ) )
	{
		Msg( "  [rd_d3d9] could not read device global\n" );
		return nullptr;
	}

	Msg( "  [rd_d3d9] device global at 0x%08X, holds 0x%08X\n", ( unsigned int )ppDevice, ( unsigned int )pDevice );
	return pDevice;
}

// ---------------------------------------------------------------------------
// Install (idempotent)
// ---------------------------------------------------------------------------
bool RD_D3D9::Install()
{
	static bool s_bInstalled = false;
	if ( s_bInstalled )
		return true;

	s_bInstalled = true;
	return true;
}

IDirect3DDevice9 *RD_D3D9::FindDevice()
{
	if ( s_pDevice )
		return s_pDevice;

	IDirect3DDevice9 *pDevice = FindDeviceViaGlobal();
	if ( !pDevice )
		return nullptr;

	HookDeviceVtable( pDevice );
	s_pDevice = static_cast< IDirect3DDevice9 * >( pDevice );
	return s_pDevice;
}

IDirect3DDevice9 *RD_D3D9::GetDevice()
{
	return s_pDevice;
}

void RD_D3D9::AddResetCallback( ResetCallbackFn fn )
{
	if ( !fn || s_nResetCallbackCount >= RD_D3D9_MAX_RESET_CALLBACKS )
		return;
	s_pResetCallbacks[ s_nResetCallbackCount++ ] = fn;
}

void RD_D3D9::BeginCaptureRenderTarget()
{
	s_bCaptureRTArmed = true;
}

IDirect3DSurface9 *RD_D3D9::EndCaptureRenderTarget()
{
	s_bCaptureRTArmed = false;
	IDirect3DSurface9 *pSurface = s_pCapturedRTSurface;
	s_pCapturedRTSurface = nullptr; // transfer the reference to the caller
	return pSurface;
}


// ---------------------------------------------------------------------------
// ConCommand: rd_d3d9_info
// ---------------------------------------------------------------------------
CON_COMMAND( rd_d3d9_info, "Prints the game's D3D9 device and hook state" )
{
	RD_D3D9::Install();

	Msg( "--- rd_d3d9_info ---\n" );

	IDirect3DDevice9 *pDevice = RD_D3D9::FindDevice();
	if ( !pDevice )
	{
		Msg( "device: NOT found\n" );
		return;
	}

	Msg( "device: 0x%08X (vtable hooked: SetRenderTarget/Reset), reset callbacks: %d\n",
		( unsigned int )pDevice, s_nResetCallbackCount );

	}

	#endif // WIN32 && !_X360