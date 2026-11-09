#pragma once
#include <d3d11.h>
#include <vulkan/vulkan.h>
#include <array>
#include <initializer_list>

namespace dxvk {

struct D3D11VideoFormatQuery {
  VkFormat format = VK_FORMAT_UNDEFINED;
  const VkFormat* family = nullptr;
  uint32_t familyCount = 0;
  std::array<VkFormat, 2> views = {};
  uint32_t viewCount = 0;
  bool planar = false;
  bool valid = false;
};

// Query the actual image tuple and the actual views for each direction on one tiling.
// Planar output uses extended usage and color attachments on both individual planes.
// The shader filters manually with texelFetch, so no linear-filter feature is needed.
template<typename ImageQuery, typename ViewFeatures>
HRESULT D3D11VideoFormatSupport(const D3D11VideoFormatQuery& format,
    ImageQuery&& imageQuery, ViewFeatures&& viewFeatures, UINT* supported) {
  if (!supported)
    return E_INVALIDARG;
  *supported = 0;
  if (!format.valid || format.format == VK_FORMAT_UNDEFINED
   || !format.viewCount || format.viewCount > format.views.size()
   || (format.familyCount && !format.family))
    return S_OK;

  VkImageFormatListCreateInfo list = {};
  list.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO;
  list.viewFormatCount = format.familyCount;
  list.pViewFormats = format.family;

  for (unsigned direction = 0; direction != 2; ++direction) {
    const auto usage = direction ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : VK_IMAGE_USAGE_SAMPLED_BIT;
    const auto feature = direction ? VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT : VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT;
    for (const auto tiling : { VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_TILING_LINEAR }) {
      bool viewsSupported = true;
      for (uint32_t i = 0; i < format.viewCount; ++i)
        viewsSupported &= (viewFeatures(format.views[i], tiling) & feature) != 0;
      if (!viewsSupported)
        continue;

      VkPhysicalDeviceImageFormatInfo2 info = {};
      info.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2;
      info.format = format.format;
      info.type = VK_IMAGE_TYPE_2D;
      info.tiling = tiling;
      info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | usage;
      if (format.familyCount > 1 || format.planar) {
        info.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
        if (format.familyCount)
          info.pNext = &list;
      }
      if (format.planar) {
        info.flags |= VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
        info.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
      }
      if (imageQuery(info)) {
        *supported |= direction ? D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT : D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT;
        break;
      }
    }
  }
  return S_OK;
}

// The CheckFormatSupport flags of DXGI_FORMAT_YUY2. The texture is the RGBA8 pair image, so the flags of the RGBA8
// mapping arrive here; this keeps the ones that the YUY2 row of "Format Support for Direct3D Feature Level 11.1
// Hardware" permits: Texture2D, shader ld, shader sample, shader gather4, typed UAV with typed store, CPU lockable,
// video processor input and (optional) output, shared resource. The row disallows UAV typed load, render target,
// mipmaps, cube maps, multisampling and the comparison filters. Shader access goes through the view formats of
// DXGI_FORMAT_YUY2 (dxgiformat.h): R8G8B8A8_UNORM or R8G8B8A8_UINT for an SRV or UAV, also R32_UINT for a UAV.
// A view with the format YUY2 itself is not valid (CheckViewCompatibility refuses it).
inline void D3D11MaskYuy2FormatSupport(UINT* flags1, UINT* flags2) {
  constexpr UINT allowed1 = D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_LOAD
    | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE | D3D11_FORMAT_SUPPORT_SHADER_GATHER | D3D11_FORMAT_SUPPORT_CPU_LOCKABLE
    | D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW
    | D3D11_FORMAT_SUPPORT_VIDEO_PROCESSOR_INPUT | D3D11_FORMAT_SUPPORT_VIDEO_PROCESSOR_OUTPUT;
  constexpr UINT allowed2 = D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE | D3D11_FORMAT_SUPPORT2_SHAREABLE;
  if (flags1)
    *flags1 &= allowed1;
  if (flags2)
    *flags2 &= allowed2;
}

}
