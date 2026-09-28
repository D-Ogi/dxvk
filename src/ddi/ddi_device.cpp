#include "ddi_device.h"

#include "../d3d11/d3d11_buffer.h"
#include "../d3d11/d3d11_input_layout.h"
#include "../d3d11/d3d11_texture.h"

#include <algorithm>

namespace dxvk::ddi {

  Bc250DxvkDevice::Bc250DxvkDevice(
          D3D11DXGIDevice*                  pContainer,
    const BC250_DXVK_SHELL_SERVICES&        Services)
  : m_container(pContainer), m_services(Services) {
    Com<ID3D11Device5> device;
    m_container->QueryInterface(__uuidof(ID3D11Device5), reinterpret_cast<void**>(&device));

    m_device     = static_cast<D3D11Device*>(device.ptr());
    m_context    = m_device->GetContext();
    m_dxvkDevice = m_device->GetDXVKDevice();
  }


  Bc250DxvkDevice::~Bc250DxvkDevice() {

  }


  HRESULT STDMETHODCALLTYPE Bc250DxvkDevice::QueryInterface(
          REFIID                            riid,
          void**                            ppvObject) {
    if (!ppvObject)
      return E_POINTER;

    *ppvObject = nullptr;

    if (riid == __uuidof(IUnknown)
     || riid == __uuidof(IBc250DxvkDevice)) {
      *ppvObject = ref(this);
      return S_OK;
    }

    return E_NOINTERFACE;
  }


  ULONG STDMETHODCALLTYPE Bc250DxvkDevice::Release() {
    uint32_t refCount = --m_refCount;

    if (refCount)
      return refCount;

    // Final release (E4). Submit and drain everything, then drop the D3D11 device. If the shell
    // still holds D3D11 objects, the device survives this call; report that instead of 0 so the
    // shell keeps the VkDevice alive.
    ULONG leaked = 0u;

    try {
      m_context->Flush();
      m_dxvkDevice->waitForIdle();
    } catch (const DxvkError& e) {
      Logger::err(str::format("bc250dxvk: final release: ", e.message()));
    }

    m_device  = nullptr;
    m_context = nullptr;
    m_dxvkDevice = nullptr;

    D3D11DXGIDevice* container = m_container.ref();
    m_container = nullptr;
    leaked = container->Release();

    if (leaked)
      Logger::err(str::format("bc250dxvk: D3D11 device still has ", leaked, " references at final release"));

    ReleasePrivate();
    return leaked;
  }


  HRESULT STDMETHODCALLTYPE Bc250DxvkDevice::GetD3D11Device(
          REFIID                            riid,
          void**                            ppDevice) {
    if (!ppDevice)
      return E_POINTER;

    *ppDevice = nullptr;
    return m_device->QueryInterface(riid, ppDevice);
  }


  HRESULT STDMETHODCALLTYPE Bc250DxvkDevice::GetImmediateContext(
          REFIID                            riid,
          void**                            ppContext) {
    if (!ppContext)
      return E_POINTER;

    *ppContext = nullptr;
    return m_context->QueryInterface(riid, ppContext);
  }


  HRESULT STDMETHODCALLTYPE Bc250DxvkDevice::CreateShader(
    const BC250_DXVK_SHADER_DESC*           pDesc,
          REFIID                            riid,
          void**                            ppShader) {
    if (!pDesc || pDesc->Size < sizeof(*pDesc) || !ppShader)
      return E_INVALIDARG;

    *ppShader = nullptr;

    std::vector<uint8_t> container;
    std::vector<D3D11_SO_DECLARATION_ENTRY> soEntries;
    std::vector<std::string> soNames;

    HRESULT hr = BuildShaderContainer(pDesc, &container, &soEntries, &soNames);

    if (FAILED(hr))
      return hr;

    // Stream output from a vertex or domain program also arrives as a geometry shader request; the
    // D3D11 path below expects geometry bytecode for it.
    if (pDesc->StreamOutput && GetProgramType(pDesc->Code) != 2u) {
      Logger::err("bc250dxvk: Stream output without a geometry program not implemented");
      return E_NOTIMPL;
    }

    const void* code = container.data();
    SIZE_T size = container.size();

    Com<ID3D11DeviceChild> shader;

    switch (GetProgramType(pDesc->Code)) {
      case 0u: {
        Com<ID3D11PixelShader> ps;
        hr = m_device->CreatePixelShader(code, size, nullptr, &ps);
        shader = ps.ptr();
      } break;

      case 1u: {
        Com<ID3D11VertexShader> vs;
        hr = m_device->CreateVertexShader(code, size, nullptr, &vs);
        shader = vs.ptr();
      } break;

      case 2u: {
        Com<ID3D11GeometryShader> gs;

        if (pDesc->StreamOutput) {
          const BC250_DXVK_STREAM_OUTPUT& so = *pDesc->StreamOutput;
          hr = m_device->CreateGeometryShaderWithStreamOutput(code, size,
            soEntries.data(), UINT(soEntries.size()), so.BufferStrides, so.NumStrides,
            so.RasterizedStream, nullptr, &gs);
        } else {
          hr = m_device->CreateGeometryShader(code, size, nullptr, &gs);
        }

        shader = gs.ptr();
      } break;

      case 3u: {
        Com<ID3D11HullShader> hs;
        hr = m_device->CreateHullShader(code, size, nullptr, &hs);
        shader = hs.ptr();
      } break;

      case 4u: {
        Com<ID3D11DomainShader> ds;
        hr = m_device->CreateDomainShader(code, size, nullptr, &ds);
        shader = ds.ptr();
      } break;

      case 5u: {
        Com<ID3D11ComputeShader> cs;
        hr = m_device->CreateComputeShader(code, size, nullptr, &cs);
        shader = cs.ptr();
      } break;

      default:
        return E_INVALIDARG;
    }

    if (FAILED(hr))
      return hr;

    return shader->QueryInterface(riid, ppShader);
  }


  HRESULT STDMETHODCALLTYPE Bc250DxvkDevice::CreateInputLayout(
    const BC250_DXVK_INPUT_LAYOUT*          pLayout,
          ID3D11InputLayout**               ppInputLayout) {
    if (!pLayout || !ppInputLayout
     || pLayout->NumAttributes > MaxNumVertexAttributes
     || pLayout->NumBindings > MaxNumVertexBindings)
      return E_INVALIDARG;

    *ppInputLayout = nullptr;

    std::array<DxvkVertexAttribute, MaxNumVertexAttributes> attributes = { };
    std::array<DxvkVertexBinding, MaxNumVertexBindings> bindings = { };

    for (uint32_t i = 0u; i < pLayout->NumAttributes; i++) {
      const auto& a = pLayout->Attributes[i];
      attributes[i] = { a.Location, a.Binding, a.Format, a.Offset };
    }

    for (uint32_t i = 0u; i < pLayout->NumBindings; i++) {
      const auto& b = pLayout->Bindings[i];
      bindings[i] = { b.Binding, b.Extent, b.InputRate, b.Divisor };
    }

    try {
      *ppInputLayout = ref(new D3D11InputLayout(m_device,
        pLayout->NumAttributes, attributes.data(),
        pLayout->NumBindings, bindings.data()));
      return S_OK;
    } catch (const DxvkError& e) {
      Logger::err(e.message());
      return E_INVALIDARG;
    }
  }


  HRESULT STDMETHODCALLTYPE Bc250DxvkDevice::GetVertexFormat(
          DXGI_FORMAT                       Format,
          VkFormat*                         pVkFormat,
          UINT*                             pElementSize) {
    if (!pVkFormat || !pElementSize)
      return E_INVALIDARG;

    VkFormat format = m_device->LookupFormat(Format, DXGI_VK_FORMAT_MODE_COLOR).Format;

    if (format == VK_FORMAT_UNDEFINED)
      return E_INVALIDARG;

    const DxvkFormatInfo* info = lookupFormatInfo(format);

    if (!info || !(info->aspectMask & VK_IMAGE_ASPECT_COLOR_BIT))
      return E_INVALIDARG;

    *pVkFormat = format;
    *pElementSize = UINT(info->elementSize);
    return S_OK;
  }


  // Sharing belongs to the shell's runtime allocations; given to DXVK, these flags would export Vulkan memory.
  constexpr UINT ShellSharingFlags = D3D11_RESOURCE_MISC_SHARED
                                   | D3D11_RESOURCE_MISC_SHARED_NTHANDLE
                                   | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;


  // A wrapped image needs D3D11_COMMON_TEXTURE_MAP_MODE_NONE: every other map mode wants engine-owned memory
  // (staging buffers or a host-mapped image), and tiled resources want sparse binding.
  static bool IsImportable(const D3D11_TEXTURE2D_DESC1* pDesc) {
    return (pDesc->Usage == D3D11_USAGE_DEFAULT || pDesc->Usage == D3D11_USAGE_IMMUTABLE)
        && !pDesc->CPUAccessFlags
        && !(pDesc->MiscFlags & D3D11_RESOURCE_MISC_TILED);
  }


  static HRESULT GetCommonDesc(
    const D3D11_TEXTURE2D_DESC1*            pDesc,
          D3D11_COMMON_TEXTURE_DESC*        pCommon) {
    if (!IsImportable(pDesc))
      return E_INVALIDARG;

    D3D11_COMMON_TEXTURE_DESC desc = { };
    desc.Width          = pDesc->Width;
    desc.Height         = pDesc->Height;
    desc.Depth          = 1;
    desc.MipLevels      = pDesc->MipLevels;
    desc.ArraySize      = pDesc->ArraySize;
    desc.Format         = pDesc->Format;
    desc.SampleDesc     = pDesc->SampleDesc;
    desc.Usage          = pDesc->Usage;
    desc.BindFlags      = pDesc->BindFlags;
    desc.CPUAccessFlags = pDesc->CPUAccessFlags;
    desc.MiscFlags      = pDesc->MiscFlags & ~ShellSharingFlags;
    desc.TextureLayout  = pDesc->TextureLayout;

    HRESULT hr = D3D11CommonTexture::NormalizeTextureProperties(&desc);

    if (SUCCEEDED(hr))
      *pCommon = desc;

    return hr;
  }


  // D3D11CommonTexture::IsR32UavCompatibleFormat, which is private.
  static bool IsR32UavCompatibleFormat(DXGI_FORMAT Format) {
    return Format == DXGI_FORMAT_R8G8B8A8_TYPELESS
        || Format == DXGI_FORMAT_B8G8R8A8_TYPELESS
        || Format == DXGI_FORMAT_B8G8R8X8_TYPELESS
        || Format == DXGI_FORMAT_R10G10B10A2_TYPELESS
        || Format == DXGI_FORMAT_R16G16_TYPELESS
        || Format == DXGI_FORMAT_R32_TYPELESS;
  }


  HRESULT STDMETHODCALLTYPE Bc250DxvkDevice::GetImageCreateInfo(
    const D3D11_TEXTURE2D_DESC1*            pDesc,
          VkImageCreateInfo*                pInfo) {
    if (!pDesc || !pInfo)
      return E_INVALIDARG;

    D3D11_COMMON_TEXTURE_DESC desc;
    HRESULT hr = GetCommonDesc(pDesc, &desc);

    if (FAILED(hr))
      return hr;

    // The image half of the D3D11CommonTexture constructor for a 2D texture with DXGI_USAGE_BACK_BUFFER, which
    // is how CreateTexture2DFromImage wraps it. Keep the two in step when DXVK is rebased.
    DXGI_VK_FORMAT_MODE mode = DXGI_VK_FORMAT_MODE_ANY;

    if (desc.BindFlags & D3D11_BIND_RENDER_TARGET)
      mode = DXGI_VK_FORMAT_MODE_COLOR;
    else if (desc.BindFlags & D3D11_BIND_DEPTH_STENCIL)
      mode = DXGI_VK_FORMAT_MODE_DEPTH;

    DXGI_VK_FORMAT_INFO   format = m_device->LookupFormat(desc.Format, mode);
    DXGI_VK_FORMAT_FAMILY family = m_device->LookupFamily(desc.Format, mode);

    if (format.Format == VK_FORMAT_UNDEFINED)
      return E_INVALIDARG;

    VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    info.imageType     = VK_IMAGE_TYPE_2D;
    info.format        = format.Format;
    info.extent        = { desc.Width, desc.Height, 1u };
    info.mipLevels     = desc.MipLevels;
    info.arrayLayers   = desc.ArraySize;
    info.samples       = VK_SAMPLE_COUNT_1_BIT;
    info.tiling        = VK_IMAGE_TILING_OPTIMAL;
    info.sharingMode   = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    // SAMPLED: DXGI_USAGE_BACK_BUFFER makes every wrapped image shader readable
    info.usage         = VK_IMAGE_USAGE_TRANSFER_SRC_BIT
                       | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                       | VK_IMAGE_USAGE_SAMPLED_BIT;

    if (!m_device->GetOptions()->disableMsaa)
      DecodeSampleCount(desc.SampleDesc.Count, &info.samples);

    if ((desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) && IsR32UavCompatibleFormat(desc.Format)) {
      family.Add(format.Format);
      family.Add(VK_FORMAT_R32_SFLOAT);
      family.Add(VK_FORMAT_R32_UINT);
      family.Add(VK_FORMAT_R32_SINT);
    }

    const DxvkFormatInfo* formatProperties = lookupFormatInfo(format.Format);

    bool isMultiPlane  = (formatProperties->aspectMask & VK_IMAGE_ASPECT_PLANE_0_BIT) != 0;
    bool isColorFormat = (formatProperties->aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) != 0;

    if (family.FormatCount > 1u && (isColorFormat || isMultiPlane)) {
      info.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
      info.pNext  = GetViewFormatList(family);
    }

    if (desc.BindFlags & D3D11_BIND_RENDER_TARGET)
      info.usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    if (desc.BindFlags & D3D11_BIND_DEPTH_STENCIL)
      info.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

    if (desc.BindFlags & D3D11_BIND_UNORDERED_ACCESS) {
      info.usage |= VK_IMAGE_USAGE_STORAGE_BIT;

      if (formatProperties->flags.test(DxvkFormatFlag::ColorSpaceSrgb))
        info.flags |= VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
    }

    if (isMultiPlane) {
      info.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT
                 |  VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
    }

    if (desc.MiscFlags & D3D11_RESOURCE_MISC_TEXTURECUBE)
      info.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;

    // Formats such as R32G32B32 only support linear tiling on most GPUs
    if (!CheckImageSupport(info, VK_IMAGE_TILING_OPTIMAL))
      info.tiling = VK_IMAGE_TILING_LINEAR;

    if (!CheckImageSupport(info, info.tiling))
      return E_INVALIDARG;

    *pInfo = info;
    return S_OK;
  }


  HRESULT STDMETHODCALLTYPE Bc250DxvkDevice::CreateTexture2DFromImage(
    const D3D11_TEXTURE2D_DESC1*            pDesc,
          VkImage                           Image,
          ID3D11Texture2D**                 ppTexture) {
    if (!pDesc || !ppTexture || Image == VK_NULL_HANDLE)
      return E_INVALIDARG;

    *ppTexture = nullptr;

    D3D11_COMMON_TEXTURE_DESC desc;
    HRESULT hr = GetCommonDesc(pDesc, &desc);

    if (FAILED(hr))
      return hr;

    try {
      // DXGI_USAGE_BACK_BUFFER keeps the image sampleable and marks it shared, as for DXVK's own
      // swap chain images: the runtime owns this memory.
      Com<D3D11Texture2D> texture = new D3D11Texture2D(m_device, &desc, DXGI_USAGE_BACK_BUFFER, Image);
      *ppTexture = texture.ref();
      return S_OK;
    } catch (const DxvkError& e) {
      Logger::err(e.message());
      return E_INVALIDARG;
    }
  }


  static std::vector<Rc<DxvkPagedResource>> GetPagedResources(
          ID3D11Resource*                   pResource,
          UINT                              Subresource,
          bool                              AllSubresources) {
    std::vector<Rc<DxvkPagedResource>> result;

    D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    pResource->GetType(&dimension);

    if (dimension == D3D11_RESOURCE_DIMENSION_BUFFER) {
      result.push_back(static_cast<D3D11Buffer*>(pResource)->GetBuffer().ptr());
      return result;
    }

    D3D11CommonTexture* texture = GetCommonTexture(pResource);

    if (!texture)
      return result;

    if (texture->GetMapMode() == D3D11_COMMON_TEXTURE_MAP_MODE_STAGING) {
      // Staging textures live in one buffer per subresource
      UINT first = AllSubresources ? 0u : Subresource;
      UINT count = AllSubresources ? texture->CountSubresources() : 1u;

      for (UINT i = first; i < first + count; i++) {
        Rc<DxvkBuffer> buffer = texture->GetMappedBuffer(i);

        if (buffer != nullptr)
          result.push_back(buffer.ptr());
      }
    } else {
      result.push_back(texture->GetImage().ptr());
    }

    return result;
  }


  HRESULT STDMETHODCALLTYPE Bc250DxvkDevice::WaitForResourceIdle(
          ID3D11Resource*                   pResource) {
    if (!pResource)
      return E_INVALIDARG;

    for (const auto& resource : GetPagedResources(pResource, 0u, true))
      m_context->WaitForResourceIdle(*resource, false);

    return CheckDeviceStatus();
  }


  HRESULT STDMETHODCALLTYPE Bc250DxvkDevice::IsResourceBusy(
          ID3D11Resource*                   pResource,
          UINT                              Subresource) {
    if (!pResource)
      return E_INVALIDARG;

    for (const auto& resource : GetPagedResources(pResource, Subresource, false)) {
      if (!m_context->WaitForResourceIdle(*resource, true))
        return S_FALSE;
    }

    return S_OK;
  }


  HRESULT STDMETHODCALLTYPE Bc250DxvkDevice::SubmitForPresent(
          ID3D11Resource*                   pSource,
          UINT                              Subresource) {
    m_context->EndFrameAndFlush();
    return CheckDeviceStatus();
  }


  HRESULT STDMETHODCALLTYPE Bc250DxvkDevice::RotateResourceIdentities(
          ID3D11Resource* const*            ppResources,
          UINT                              Count) {
    if (!ppResources || !Count)
      return E_INVALIDARG;

    if (Count == 1u)
      return S_OK;

    // DXGI rotates the buffers of one swap chain, which are identical; storage can only move between images
    // that agree on everything Vulkan views and copies depend on.
    small_vector<Rc<DxvkImage>, 4> images;

    for (UINT i = 0u; i < Count; i++) {
      D3D11CommonTexture* texture = ppResources[i] ? GetCommonTexture(ppResources[i]) : nullptr;

      if (!texture || texture->GetMapMode() != D3D11_COMMON_TEXTURE_MAP_MODE_NONE)
        return E_INVALIDARG;

      Rc<DxvkImage> image = texture->GetImage();

      if (i) {
        const DxvkImageCreateInfo& a = images[0]->info();
        const DxvkImageCreateInfo& b = image->info();

        // Usage is compared as it is now: the context may have widened it on one image and not the others
        if (a.type != b.type || a.format != b.format || a.flags != b.flags || a.usage != b.usage
         || a.sampleCount != b.sampleCount || a.extent.width != b.extent.width
         || a.extent.height != b.extent.height || a.extent.depth != b.extent.depth
         || a.numLayers != b.numLayers || a.mipLevels != b.mipLevels || a.tiling != b.tiling
         || a.layout != b.layout || a.viewFormatCount != b.viewFormatCount
         || !std::equal(a.viewFormats, a.viewFormats + a.viewFormatCount, b.viewFormats))
          return E_INVALIDARG;

        for (const auto& other : images) {
          if (other == image)
            return E_INVALIDARG;
        }
      }

      images.push_back(std::move(image));
    }

    m_context->RotateImageStorage(images.data(), images.size());
    return S_OK;
  }


  HRESULT STDMETHODCALLTYPE Bc250DxvkDevice::Blt(
    const BC250_DXVK_BLT*                   pBlt) {
    static bool s_errorShown = false;

    if (!std::exchange(s_errorShown, true))
      Logger::err("bc250dxvk: Blt not implemented");

    return E_NOTIMPL;
  }


  HRESULT Bc250DxvkDevice::CheckDeviceStatus() const {
    return m_device->GetDeviceRemovedReason();
  }


  // D3D11CommonTexture::CheckImageSupport for an image without D3D12 interop.
  bool Bc250DxvkDevice::CheckImageSupport(
    const VkImageCreateInfo&                info,
          VkImageTiling                     tiling) const {
    DxvkFormatQuery query = { };
    query.format = info.format;
    query.type   = info.imageType;
    query.tiling = tiling;
    query.usage  = info.usage;
    query.flags  = info.flags;

    if (info.flags & VK_IMAGE_CREATE_EXTENDED_USAGE_BIT)
      query.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    auto limits = m_dxvkDevice->getFormatLimits(query);

    return limits
        && info.extent.width  <= limits->maxExtent.width
        && info.extent.height <= limits->maxExtent.height
        && info.extent.depth  <= limits->maxExtent.depth
        && info.arrayLayers   <= limits->maxArrayLayers
        && info.mipLevels     <= limits->maxMipLevels
        && (info.samples & limits->sampleCounts);
  }


  const VkImageFormatListCreateInfo* Bc250DxvkDevice::GetViewFormatList(
    const DXGI_VK_FORMAT_FAMILY&            family) {
    std::lock_guard<dxvk::mutex> lock(m_viewFormatMutex);

    for (const auto& list : m_viewFormatLists) {
      if (list->info.viewFormatCount == family.FormatCount
       && std::equal(family.Formats, family.Formats + family.FormatCount, list->formats.begin()))
        return &list->info;
    }

    auto list = std::make_unique<ViewFormatList>();
    std::copy(family.Formats, family.Formats + family.FormatCount, list->formats.begin());
    list->info.viewFormatCount = family.FormatCount;
    list->info.pViewFormats    = list->formats.data();

    m_viewFormatLists.push_back(std::move(list));
    return &m_viewFormatLists.back()->info;
  }

}
