#pragma once

// GENERATED FILE, do not edit. Generator: gen_d3d11_private.py (BD-241), from the Windows d3d11.dll
// 10.0.26100.9711 (sha256 c9aee071495039e820cd0be0afdeb927b9ad32edc1d0dae460b6d64064b524f6) and its public symbols:
// the vftables CContainedObject<CLayeredObject<CDevice>>::<interface>, the vptr offsets in
// CLayeredObject<CDevice>::CreateInstance, the CDevice interface map and the decorated method names.
// d2d1.dll requires both interfaces in CD3DDeviceCommon::Initialize (QueryInterface on the device).

#include "d3d11_include.h"

struct ID3D11SynchronizedChannel;

/**
 * \brief ID3D11PartnerDevice3 of the Windows D3D11 runtime (CDevice+0x60)
 *
 * The interface map sends these GUIDs to this vptr:
 *   {26c5dc23-e49c-4b0a-8f79-e7b1ac804d32}
 *   {9ea90e78-15ac-48b4-83c9-664cab2f71cd}
 *   {7334140e-d155-4c70-96f5-b6dbcc273f88}
 * The symbols do not name the interface level of each GUID; d2d1.dll asks for
 * the first one. The vtable is that of ID3D11PartnerDevice3, the most derived level.
 */
MIDL_INTERFACE("26c5dc23-e49c-4b0a-8f79-e7b1ac804d32")
ID3D11PartnerDevice3_DXVK : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE BeginGuardRectangleSupport() = 0;
  virtual void STDMETHODCALLTYPE EndGuardRectangleSupport() = 0;
  virtual HRESULT STDMETHODCALLTYPE CreateGuardableTexture2D(const D3D11_TEXTURE2D_DESC* a0, const D3D11_SUBRESOURCE_DATA* a1, ID3D11Texture2D** a2) = 0;
  virtual void STDMETHODCALLTYPE SetGuardRect(ID3D11Texture2D* a0, RECT a1) = 0;
  virtual void STDMETHODCALLTYPE SetEmptyGuardRect(ID3D11Texture2D* a0) = 0;
  virtual void STDMETHODCALLTYPE SetUnguarded(ID3D11Texture2D* a0) = 0;
  virtual HRESULT STDMETHODCALLTYPE OfferResourcesInternal(UINT a0, IDXGIResource* const* a1, DXGI_OFFER_RESOURCE_PRIORITY a2, UINT a3) = 0;
  virtual HRESULT STDMETHODCALLTYPE ReclaimResourcesInternal(UINT a0, IDXGIResource* const* a1, DXGI_RECLAIM_RESOURCE_RESULTS* a2) = 0;
  virtual UINT STDMETHODCALLTYPE GetPartnerCaps() = 0;
  virtual HRESULT STDMETHODCALLTYPE CreateCompositionBuffer(UINT a0, UINT a1, DXGI_FORMAT a2, INT a3, UINT a4, UINT a5, REFGUID a6, void** a7, void** a8) = 0;
  virtual HRESULT STDMETHODCALLTYPE PresentCompositionBuffers(void* a0, IUnknown* const* a1, UINT a2) = 0;
  virtual void STDMETHODCALLTYPE GetGuardRect(ID3D11Texture2D* a0, INT* a1, RECT* a2) = 0;
  virtual HRESULT STDMETHODCALLTYPE CreateSynchronizedChannel(ID3D11SynchronizedChannel** a0) = 0;
  virtual HRESULT STDMETHODCALLTYPE OpenSynchronizedChannel(void* a0, ID3D11SynchronizedChannel** a1) = 0;
  virtual HRESULT STDMETHODCALLTYPE PresentFlipManagerToken(void* a0, UINT64 a1, UINT64 a2, UINT a3, UINT a4, IUnknown* const* a5, UINT a6) = 0;
  virtual HRESULT STDMETHODCALLTYPE CancelFlipManagerPresent(LUID a0, UINT64 a1, UINT64 a2) = 0;
  virtual HRESULT STDMETHODCALLTYPE EnableVariableRefreshForProcess() = 0;
  virtual HRESULT STDMETHODCALLTYPE FlushCompletedPresents(LUID a0, void* a1) = 0;
};

/** \brief Another GUID of the same ID3D11PartnerDevice3 vptr */
constexpr GUID ID3D11PartnerDevice3_DXVK_Alias1 = {0x9ea90e78,0x15ac,0x48b4,{0x83,0xc9,0x66,0x4c,0xab,0x2f,0x71,0xcd}};

/** \brief Another GUID of the same ID3D11PartnerDevice3 vptr */
constexpr GUID ID3D11PartnerDevice3_DXVK_Alias2 = {0x7334140e,0xd155,0x4c70,{0x96,0xf5,0xb6,0xdb,0xcc,0x27,0x3f,0x88}};

/**
 * \brief ID2DPrivateInfo of the Windows D3D11 runtime (CDevice+0x68)
 */
MIDL_INTERFACE("b79cc8da-337f-400f-b09d-b2edf8a84e47")
ID2DPrivateInfo_DXVK : public IUnknown {
  virtual UINT64 STDMETHODCALLTYPE ConservativeFlushCount() = 0;
};

#ifndef _MSC_VER
__CRT_UUID_DECL(ID3D11PartnerDevice3_DXVK, 0x26c5dc23,0xe49c,0x4b0a,0x8f,0x79,0xe7,0xb1,0xac,0x80,0x4d,0x32);
__CRT_UUID_DECL(ID2DPrivateInfo_DXVK, 0xb79cc8da,0x337f,0x400f,0xb0,0x9d,0xb2,0xed,0xf8,0xa8,0x4e,0x47);
#endif
