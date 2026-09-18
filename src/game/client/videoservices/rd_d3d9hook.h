// SPDX-License-Identifier: MIT
// Copyright (c) 2026
//
// rd_d3d9hook.h
//
// Minimal D3D9 hook for the GPU video path (DXVA2):
//  - obtains the game's IDirect3DDevice9* (global in shaderapidx9.dll at
//    RVA 0x17B22C, found by disassembling InvokeCreateDevice),
//  - hooks IDirect3DDevice9::SetRenderTarget to capture the native D3D9
//    surface of our video render target,
//  - hooks IDirect3DDevice9::Reset so DXVA2 resources are released before
//    the device reset and recreated after it.
// The engine uses NON-Ex D3D9. No scanning, no verification, no method calls
// on unknown objects.

#pragma once

struct IDirect3DDevice9;
struct IDirect3DSurface9;

namespace RD_D3D9
{
    // Idempotent init hook (currently a no-op; the module is driven by FindDevice).
    bool Install();

    // Finds the game's D3D9 device (shaderapidx9.dll global) and hooks its
    // vtable in place (SetRenderTarget/Reset). Returns the device (also stored
    // internally) or nullptr.
    IDirect3DDevice9 *FindDevice();

    // Returns the captured device, or null if not found yet.
    IDirect3DDevice9 *GetDevice();

    // -------------------------------------------------------------------
    // Device reset observers (DXVA2 lifecycle)
    // -------------------------------------------------------------------
    // Registers a callback invoked around the real IDirect3DDevice9::Reset:
    //   bPreReset == true  -> immediately before Reset (release/invalidate any
    //                         DXVA2 decoder surfaces and video processors),
    //   bPreReset == false -> after a successful Reset (ResetDevice() on the
    //                         device manager, recreate processors/render targets).
    typedef void ( *ResetCallbackFn )( bool bPreReset );
    void AddResetCallback( ResetCallbackFn fn );

    // -------------------------------------------------------------------
    // SetRenderTarget capture (video render target surface)
    // -------------------------------------------------------------------
    // Arms the capture of the next IDirect3DDevice9::SetRenderTarget call on
    // stage 0. Bind your Source ITexture via IMaterialSystem::SetRenderTarget
    // right after arming, then collect the native surface with
    // EndCaptureRenderTarget(). The returned surface carries one reference -
    // the caller must Release it (and release it again before a device Reset).
    void BeginCaptureRenderTarget();
    IDirect3DSurface9 *EndCaptureRenderTarget();
}
