/* SPDX-License-Identifier: MIT
 *
 * bc250_dxvk_engine.h - boundary between the BC-250 system D3D10/11 user-mode driver ("shell", the
 * UserModeDriverName DLL) and its DXVK engine ("engine", bc250dxvk.dll, DXVK fork branch amdgpu-wddm/ddi-engine).
 *
 * Revision r1, ABI 1.0. This file in the DXVK fork is the only copy; the shell includes it from the DXVK
 * source checkout it builds against, like its other DXVK-facing headers.
 *
 * WDK-free by construction. DXVK's util_gdi.h declares private extern-C D3DKMT prototypes that collide with
 * the WDK's in one translation unit, so nothing here needs d3d10umddi.h or a DXVK header: windows.h, the SDK's
 * d3d11_4.h and vulkan_core.h only. A shell translation unit that includes d3d10umddi.h can include this header;
 * an engine translation unit never includes d3d10umddi.h. C++ only (COM interfaces).
 *
 * Division of work
 *
 *   shell   OpenAdapter10_2, GetCaps, every DDI table and handle (D3D11 device/context and DXGI), translation of
 *           DDI arguments to D3D11 descriptions, RuntimeDomain, hosted RADV bootstrap (hosted contract v5),
 *           VkInstance/VkDevice lifetime, runtime allocations (primaries, back buffers, shared resources),
 *           pfnPresentCb and every other runtime callback, error reporting (pfnSetErrorCb).
 *   engine  DXVK's D3D11 device and immediate context on the imported Vulkan device, exposed through the SDK's
 *           D3D11 COM interfaces, plus IBc250DxvkDevice for what those interfaces cannot express: shaders from
 *           DDI token streams, input layouts from register numbers, textures on runtime allocations, present
 *           submission, resource identity rotation, DXGI Blt. The engine never calls a runtime callback; it
 *           reaches the runtime only inside hosted RADV calls.
 *
 * Ownership and threading rules
 *
 *   E1  Vulkan objects. The shell creates VkInstance and VkDevice (hosted RADV, with its bc250_host chain) and
 *       destroys them only after the engine device's final Release returned 0. The engine imports both and never
 *       destroys them. Before vkCreateDevice the shell calls QueryDeviceRequirements and enables at least the
 *       returned extensions and features on one queue of the returned family.
 *   E2  Threads (ABI 1.0 = inline mode). Every Vulkan call the engine makes happens on the thread that is
 *       inside an engine call (a COM method of an engine object or an IBc250DxvkDevice method). The engine starts
 *       no thread that calls Vulkan: DXVK's CS, submit, finish, pipeline, descriptor, cache, presenter and adapter
 *       threads are not started in this mode. DXVK's fence thread starts only for
 *       ID3D11Fence::SetEventOnCompletion, which the shell does not call (DDI fences are the shell's). The shell
 *       calls the engine only from inside a DDI entry that holds the device's RuntimeDomain scope, so hosted RADV
 *       may use runtime callbacks throughout. The shell does not report D3D11DDICAPS_FREETHREADED; the runtime
 *       then enters one device from one thread at a time.
 *       A later minor version may add a broker mode; the engine will only use it when the shell asks for it.
 *   E3  Submission. ID3D11DeviceContext::Flush and IBc250DxvkDevice::SubmitForPresent return only after every
 *       command recorded before them was submitted to hosted RADV (vkQueueSubmit returned). Other entries may
 *       submit too (DXVK flushes by size and on synchronizing calls). Services.QueueLock brackets every engine
 *       vkQueueSubmit and vkQueueWaitIdle.
 *   E4  D3D11 objects. The shell creates one engine object per DDI object through the D3D11 interfaces or
 *       IBc250DxvkDevice, keeps one reference in the DDI handle memory, and releases it in the matching DDI
 *       Destroy. The runtime destroys children before the device, so the engine device's final Release (in the
 *       shell's DestroyDevice) returns 0; a nonzero return is a shell leak: the shell must not destroy the
 *       VkDevice then (leak rather than use-after-free) and should log it. Final Release drains the GPU.
 *   E5  Runtime allocations. For resources whose memory the runtime must own (primary, anything passed to
 *       pfnPresentCb, shared) the shell allocates through the runtime, imports the allocation into Vulkan
 *       (hosted import), creates and binds the VkImage and wraps it with CreateTexture2DFromImage. The shell
 *       keeps the VkImage, VkDeviceMemory and runtime allocation. On DDI DestroyResource it calls
 *       WaitForResourceIdle, releases the texture (views are released first by the runtime), then destroys
 *       the image, frees the memory and deallocates (primaries immediately, others possibly deferred to Flush).
 *       After WaitForResourceIdle and the last release the engine never references that VkImage again.
 *   E6  Errors. Methods that return HRESULT return it; void D3D11 methods report nothing. E_OUTOFMEMORY is
 *       returned, never thrown. Engine results are D3D11/DXGI API codes, not DDI codes: the shell maps them to
 *       the set the DDI entry allows before pfnSetErrorCb (any other code is critical, and the runtime then
 *       removes the device): DXGI_ERROR_WAS_STILL_DRAWING becomes DXGI_DDI_ERR_WASSTILLDRAWING (Map with
 *       DONOTWAIT, query data), and device loss becomes D3DDDIERR_DEVICEREMOVED where the entry allows it. After
 *       Flush, SubmitForPresent and any failed call, ID3D11Device::GetDeviceRemovedReason tells device loss
 *       apart (any failure code; DXVK reports DXGI_ERROR_DEVICE_RESET for every lost-device status).
 *       Engine waits are bounded by hosted RADV's own fence wait rules; the engine adds no infinite spin.
 *
 * Layout mirrors. The structures marked "layout = X" have the size and member order of the WDK structure X so
 * that the shell may copy arrays with memcpy; the shell's translation unit checks this with static_assert. The
 * engine never includes the WDK to check it.
 */
#pragma once

#ifndef __cplusplus
#error "bc250_dxvk_engine.h is a C++ header (COM interfaces)"
#endif

#include <windows.h>
#include <d3d11_4.h>
#include <vulkan/vulkan.h>

#define BC250_DXVK_ENGINE_ABI_MAJOR 1u
#define BC250_DXVK_ENGINE_ABI_MINOR 0u
#define BC250_DXVK_ENGINE_ABI_VERSION ((BC250_DXVK_ENGINE_ABI_MAJOR << 16) | BC250_DXVK_ENGINE_ABI_MINOR)

/* The Vulkan objects the shell owns (E1). */
struct BC250_DXVK_VULKAN_INSTANCE {
    UINT32 Size;
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr; /* hosted RADV's entry; the engine loads no Vulkan DLL */
    VkInstance Instance;
    UINT32 ApiVersion;                             /* VkApplicationInfo::apiVersion used at vkCreateInstance */
    UINT32 ExtensionCount;
    const char *const *ExtensionNames;             /* enabled instance extensions */
    VkPhysicalDevice PhysicalDevice;               /* the physical device of this runtime adapter */
};

/* E1: what the engine needs from vkCreateDevice. Engine-owned memory until FreeDeviceRequirements. */
struct BC250_DXVK_DEVICE_REQUIREMENTS {
    UINT32 Size;
    UINT32 ExtensionCount;
    const char *const *ExtensionNames;
    const VkPhysicalDeviceFeatures2 *Features;     /* full pNext chain; the shell may copy and extend it */
    UINT32 QueueFamily;                            /* one queue of this family is imported */
    void *Owner;                                   /* engine bookkeeping, do not touch */
};

struct BC250_DXVK_VULKAN_DEVICE {
    UINT32 Size;
    VkDevice Device;
    VkQueue Queue;
    UINT32 QueueFamily;
    UINT32 ExtensionCount;                         /* exactly as enabled at vkCreateDevice */
    const char *const *ExtensionNames;
    const VkPhysicalDeviceFeatures2 *Features;     /* exactly as enabled at vkCreateDevice */
};

/* Services the shell provides to one engine device. Any member may be NULL. */
struct BC250_DXVK_SHELL_SERVICES {
    UINT32 Size;
    void *Shell;
    /* E3: Lock(TRUE) before and Lock(FALSE) after each engine vkQueueSubmit/vkQueueWaitIdle, same thread. */
    void (APIENTRY *QueueLock)(void *shell, BOOL lock);
    /* Engine log lines (DXVK logger), one line per call without prefix or newline; level 1 error, 2 warning,
     * 3 info, 4 debug, filtered by DXVK_LOG_LEVEL (default info). Called from the start of CreateDevice (so a
     * failed CreateDevice reports its reason here) until the final Release returns, on the thread of the engine
     * call that logs, with the engine's log lock held: it must not call the engine. The DXVK logger is per
     * process: while devices with a Log exist, every line goes to the Log of the oldest of them and no log
     * file is written. Lines logged with no such device (QueryDeviceRequirements, GetAdapterInfo) go to the
     * file in DXVK_LOG_PATH, if set, or nowhere. */
    void (APIENTRY *Log)(void *shell, UINT32 level, const char *message);
};

#define BC250_DXVK_THREADING_INLINE 0u

struct BC250_DXVK_DEVICE_CREATE_INFO {
    UINT32 Size;
    UINT32 AbiVersion;                             /* BC250_DXVK_ENGINE_ABI_VERSION the shell was built with */
    const BC250_DXVK_VULKAN_INSTANCE *Instance;
    const BC250_DXVK_VULKAN_DEVICE *Device;
    const BC250_DXVK_SHELL_SERVICES *Services;
    D3D_FEATURE_LEVEL FeatureLevel;                /* the level the runtime creates the device at */
    UINT32 Threading;                              /* BC250_DXVK_THREADING_INLINE */
    UINT32 Flags;                                  /* 0 */
};

struct BC250_DXVK_ADAPTER_INFO {
    UINT32 Size;
    D3D_FEATURE_LEVEL MaxFeatureLevel;             /* for D3D11DDICAPS_3DPIPELINESUPPORT; capped at 11_1 in 1.0 */
};

/* layout = D3D11_1DDIARG_SIGNATURE_ENTRY2 (D3D11_1DDIARG_SIGNATURE_ENTRY: pass Stream = 0). */
struct BC250_DXVK_SIGNATURE_ENTRY {
    UINT SystemValue;                              /* D3D10_SB_NAME */
    UINT Register;                                 /* D3D10_SB_... register index; ~0u for none */
    BYTE Mask;                                     /* xyzw in bits 0-3 */
    BYTE Stream;                                   /* GS output stream */
    UINT ComponentType;                            /* D3D10_SB_REGISTER_COMPONENT_TYPE */
    UINT MinPrecision;                             /* D3D11_SB_OPERAND_MIN_PRECISION */
};

struct BC250_DXVK_SIGNATURE {
    const BC250_DXVK_SIGNATURE_ENTRY *Entries;
    UINT NumEntries;
};

/* layout = D3D11DDIARG_STREAM_OUTPUT_DECLARATION_ENTRY */
struct BC250_DXVK_SO_ENTRY {
    UINT Stream;
    UINT OutputSlot;
    UINT RegisterIndex;                            /* ~0u: gap ("skip") entry */
    BYTE RegisterMask;
};

struct BC250_DXVK_STREAM_OUTPUT {
    const BC250_DXVK_SO_ENTRY *Entries;
    UINT NumEntries;
    const UINT *BufferStrides;
    UINT NumStrides;
    UINT RasterizedStream;
};

/* One DDI shader: the runtime's pShaderCode (version token, length token in DWORDs, instructions) and the
 * signatures the runtime passes with it (D3D11_1DDIARG_STAGE_IO_SIGNATURES or ..._TESSELLATION_IO_SIGNATURES).
 * The stage comes from the version token. */
struct BC250_DXVK_SHADER_DESC {
    UINT Size;
    const UINT *Code;
    BC250_DXVK_SIGNATURE Input;
    BC250_DXVK_SIGNATURE Output;
    BC250_DXVK_SIGNATURE PatchConstant;            /* hull output / domain input; empty otherwise */
    const BC250_DXVK_STREAM_OUTPUT *StreamOutput;  /* CreateGeometryShaderWithStreamOutput only, else NULL */
};
/* With StreamOutput, Code may be NULL or a vertex or domain program: the stream output then captures that
 * stage's output through a pass-through geometry shader built from Output (or Input when Output is empty).
 * The shader is always an ID3D11GeometryShader. The pass-through emits each point, line or triangle it
 * receives, so strips stream out as lists and a rasterized stream draws the primitives unchanged. Not
 * supported without a geometry program: adjacency and patch topologies. */

/* The output of the shell's DDI element-layout translation (driver/umd/dxvk/input-layout-data.h): vertex
 * attributes addressed by input register, bindings compacted without renumbering slots. */
struct BC250_DXVK_VERTEX_ATTRIBUTE {
    UINT Location;
    UINT Binding;
    VkFormat Format;
    UINT Offset;
};

struct BC250_DXVK_VERTEX_BINDING {
    UINT Binding;
    UINT Extent;
    VkVertexInputRate InputRate;
    UINT Divisor;
};

struct BC250_DXVK_INPUT_LAYOUT {
    UINT NumAttributes;
    const BC250_DXVK_VERTEX_ATTRIBUTE *Attributes;
    UINT NumBindings;
    const BC250_DXVK_VERTEX_BINDING *Bindings;
};

/* DXGI_DDI_ARG_BLT without the WDK handles. */
#define BC250_DXVK_BLT_RESOLVE 0x1u
#define BC250_DXVK_BLT_CONVERT 0x2u
#define BC250_DXVK_BLT_STRETCH 0x4u
#define BC250_DXVK_BLT_PRESENT 0x8u

struct BC250_DXVK_BLT {
    ID3D11Resource *Destination;
    UINT DestinationSubresource;
    RECT DestinationRect;                          /* DstLeft, DstTop, DstRight, DstBottom */
    ID3D11Resource *Source;
    UINT SourceSubresource;
    UINT Flags;                                    /* BC250_DXVK_BLT_* */
    UINT Rotation;                                 /* DXGI_DDI_MODE_ROTATION */
};

/* The engine device. Obtained from CreateDevice; its final Release destroys the D3D11 device (E4). */
MIDL_INTERFACE("cd06519b-e51d-4551-9aff-285ec5c7b7b8")
IBc250DxvkDevice : public IUnknown {
    /* The D3D11 device (ID3D11Device .. ID3D11Device5, same object) and its immediate context. The returned
     * references are separate COM objects; release them before the final Release of this interface. */
    virtual HRESULT STDMETHODCALLTYPE GetD3D11Device(REFIID riid, void **device) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetImmediateContext(REFIID riid, void **context) = 0;

    /* A shader from a DDI token stream. riid names the stage interface (ID3D11VertexShader, ...,
     * ID3D11GeometryShader for stream output); it must match the version token. The engine builds a DXBC
     * container with register-derived semantic names; names are consistent across stages and stream output. */
    virtual HRESULT STDMETHODCALLTYPE CreateShader(const BC250_DXVK_SHADER_DESC *desc, REFIID riid,
                                                   void **shader) = 0;

    /* An input layout from register-addressed attributes (DDI CreateElementLayout has no VS bytecode). */
    virtual HRESULT STDMETHODCALLTYPE CreateInputLayout(const BC250_DXVK_INPUT_LAYOUT *layout,
                                                        ID3D11InputLayout **inputLayout) = 0;

    /* Vertex format lookup for the shell's element-layout translation: the Vulkan format the engine uses for a
     * vertex attribute of this DXGI format and its size in bytes. E_INVALIDARG if not a vertex format. */
    virtual HRESULT STDMETHODCALLTYPE GetVertexFormat(DXGI_FORMAT format, VkFormat *vkFormat,
                                                      UINT *elementSize) = 0;

    /* E5: a texture on a shell-owned VkImage. GetImageCreateInfo returns what the engine's D3D11 texture expects
     * for desc: type, format, extent, levels, layers, samples, usage, flags, and LINEAR tiling where DXVK itself
     * would fall back to it. info->pNext may point to a VkImageFormatListCreateInfo that the engine device owns
     * until its final Release: keep it in the chain (drivers keep compression on mutable-format images only with
     * a list) and link the shell's own structures in front of it. The shell may add usage bits and choose the
     * tiling of a runtime allocation; it must not remove usage or flags. Only GPU-only textures qualify: Usage
     * DEFAULT or IMMUTABLE, CPUAccessFlags 0, not TILED (E_INVALIDARG otherwise); the SHARED, SHARED_NTHANDLE and
     * SHARED_KEYEDMUTEX MiscFlags are ignored, sharing is the shell's. CPU-accessible textures come from
     * ID3D11Device::CreateTexture2D, with engine-owned memory. */
    virtual HRESULT STDMETHODCALLTYPE GetImageCreateInfo(const D3D11_TEXTURE2D_DESC1 *desc,
                                                         VkImageCreateInfo *info) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateTexture2DFromImage(const D3D11_TEXTURE2D_DESC1 *desc, VkImage image,
                                                               ID3D11Texture2D **texture) = 0;

    /* E5: submits pending work that references the resource and waits until the GPU finished all of it. */
    virtual HRESULT STDMETHODCALLTYPE WaitForResourceIdle(ID3D11Resource *resource) = 0;

    /* DDI ResourceIsStagingBusy: S_OK idle, S_FALSE busy. Never waits. */
    virtual HRESULT STDMETHODCALLTYPE IsResourceBusy(ID3D11Resource *resource, UINT subresource) = 0;

    /* DXGI PresentDXGI/Present1, before the shell's present ordering and pfnPresentCb: ends the engine frame
     * and submits everything recorded so far (E3). Does not wait for the GPU. After the submission it compiles
     * optimized pipelines that draws deferred (E2 keeps them off the drawing thread; fast-linked pipelines serve
     * until then), starting compiles for up to dxvk.inlinePipelineBudget microseconds (default 2000, 0 never
     * compiles them). */
    virtual HRESULT STDMETHODCALLTYPE SubmitForPresent(ID3D11Resource *source, UINT subresource) = 0;

    /* DXGI RotateResourceIdentities: resource i takes the storage of resource i + 1, the last one that of the
     * first, in command order: work recorded before the call keeps the old storage, views created before it
     * see the new one. The shell rotates its own allocation handles the same way; E5 then applies to the
     * VkImage a resource holds after the rotation. Resources must be distinct GPU-only textures with
     * identical properties, otherwise E_INVALIDARG and nothing moves. */
    virtual HRESULT STDMETHODCALLTYPE RotateResourceIdentities(ID3D11Resource *const *resources, UINT count) = 0;

    /* DXGI Blt: the whole source subresource onto DestinationRect, stretching, converting and resolving as the
     * two resources require; the flags are not needed for that. Between an sRGB and a UNORM format the encoded
     * values move unchanged. ROTATE180 mirrors both axes; ROTATE90/270 and multisampled destinations return
     * E_NOTIMPL. Both resources must be GPU-only 2D textures, otherwise E_INVALIDARG. With
     * BC250_DXVK_BLT_PRESENT the engine submits before returning and compiles deferred pipelines, as
     * SubmitForPresent does (E3). */
    virtual HRESULT STDMETHODCALLTYPE Blt(const BC250_DXVK_BLT *blt) = 0;
};

struct BC250_DXVK_ENGINE_FUNCS {
    UINT32 Size;
    UINT32 AbiVersion;                             /* filled by the engine */

    HRESULT (APIENTRY *QueryDeviceRequirements)(const BC250_DXVK_VULKAN_INSTANCE *instance,
                                                 BC250_DXVK_DEVICE_REQUIREMENTS *requirements);
    void (APIENTRY *FreeDeviceRequirements)(BC250_DXVK_DEVICE_REQUIREMENTS *requirements);
    HRESULT (APIENTRY *GetAdapterInfo)(const BC250_DXVK_VULKAN_INSTANCE *instance, BC250_DXVK_ADAPTER_INFO *info);

    /* Inside the shell's DDI CreateDevice, after vkCreateDevice. */
    HRESULT (APIENTRY *CreateDevice)(const BC250_DXVK_DEVICE_CREATE_INFO *info, IBc250DxvkDevice **device);
};

/* The one export of bc250dxvk.dll. E_NOINTERFACE on an ABI major mismatch. */
typedef HRESULT (APIENTRY *PFN_BC250_DXVK_ENGINE_GET_FUNCS)(UINT32 abiVersion, BC250_DXVK_ENGINE_FUNCS *funcs);
#define BC250_DXVK_ENGINE_GET_FUNCS_NAME "Bc250DxvkEngineGetFuncs"
