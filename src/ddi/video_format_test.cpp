#include "../d3d11/d3d11_video_format.h"
#include <cstdio>

namespace {
using namespace dxvk;
constexpr VkFormatFeatureFlags2 sample = VK_FORMAT_FEATURE_2_SAMPLED_IMAGE_BIT;
constexpr VkFormatFeatureFlags2 color = VK_FORMAT_FEATURE_2_COLOR_ATTACHMENT_BIT;
constexpr UINT input = D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_INPUT;
constexpr UINT output = D3D11_VIDEO_PROCESSOR_FORMAT_SUPPORT_OUTPUT;
int checks = 0, failures = 0;
void check(bool ok, const char* name) { ++checks; if (!ok) { ++failures; printf("FAIL %s\n", name); } }

struct Mock {
  VkFormatFeatureFlags2 features[2][2] = {{sample | color, sample | color}, {0, 0}};
  bool images[2][2] = {{true, true}, {false, false}};
  unsigned calls = 0;
  bool exact = true;
  D3D11VideoFormatQuery request;

  VkFormatFeatureFlags2 view(VkFormat f, VkImageTiling t) {
    return features[t == VK_IMAGE_TILING_LINEAR][f == request.views[1] && request.viewCount > 1 ? 1 : 0];
  }
  bool image(const VkPhysicalDeviceImageFormatInfo2& info) {
    ++calls;
    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    const unsigned direction = (info.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) ? 1 : 0;
    usage |= direction ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : VK_IMAGE_USAGE_SAMPLED_BIT;
    if (request.planar) usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    VkImageCreateFlags flags = request.familyCount > 1 || request.planar ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0;
    if (request.planar) flags |= VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
    exact &= info.type == VK_IMAGE_TYPE_2D && info.format == request.format && info.flags == flags && info.usage == usage;
    if (flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) {
      auto list = static_cast<const VkImageFormatListCreateInfo*>(info.pNext);
      exact &= list && list->sType == VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO && list->viewFormatCount == request.familyCount;
      if (list && list->viewFormatCount == request.familyCount)
        for (uint32_t i = 0; i < request.familyCount; ++i) exact &= list->pViewFormats[i] == request.family[i];
    }
    return exact && images[info.tiling == VK_IMAGE_TILING_LINEAR][direction];
  }
  UINT run() {
    UINT result = ~0u;
    HRESULT hr = D3D11VideoFormatSupport(request, [&](const auto& q) { return image(q); }, [&](auto f, auto t) { return view(f, t); }, &result);
    check(hr == S_OK && exact, "query contract");
    return result;
  }
};
}

int main() {
  const VkFormat rgbFamily[] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UINT};
  D3D11VideoFormatQuery rgb = {VK_FORMAT_R8G8B8A8_UNORM, rgbFamily, 2, {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_UNDEFINED}, 1, false, true};
  Mock m; m.request = rgb;
  check(m.run() == (input | output), "RGB both directions");
  m = {}; m.request = rgb; m.features[0][0] = sample;
  check(m.run() == input, "sampled-only input");
  m = {}; m.request = rgb; m.features[0][0] = color;
  check(m.run() == output, "renderable-only output");
  m = {}; m.request = rgb; m.features[0][0] = 0;
  check(m.run() == 0 && m.calls == 0, "neither view usage");
  m = {}; m.request = rgb; m.images[0][0] = m.images[0][1] = false;
  check(m.run() == 0, "mutable image unsupported");

  const VkFormat planes[] = {VK_FORMAT_R8_UNORM, VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8_UINT, VK_FORMAT_R8G8_UINT};
  D3D11VideoFormatQuery nv12 = {VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, planes, 4, {planes[0], planes[1]}, 2, true, true};
  m = {}; m.request = nv12;
  check(m.run() == (input | output), "NV12 extended usage on both plane views");
  m = {}; m.request = nv12; m.features[0][1] = sample;
  check(m.run() == input, "second plane cannot render");
  m = {}; m.request = nv12; m.features[0][0] = color; m.features[0][1] = color;
  check(m.run() == output, "planar output-only views with extended base usage");
  m = {}; m.request = nv12;
  m.features[0][0] = sample | color; m.features[0][1] = 0;
  m.features[1][0] = 0; m.features[1][1] = sample | color;
  m.images[1][0] = m.images[1][1] = true;
  check(m.run() == 0 && m.calls == 0, "cannot combine planes from different tilings");
  m = {}; m.request = nv12; m.images[0][0] = m.images[0][1] = false;
  m.features[1][0] = m.features[1][1] = sample | color; m.images[1][0] = m.images[1][1] = true;
  check(m.run() == (input | output) && m.calls == 4, "fallback to complete linear tuple");
  m = {}; m.request = rgb; m.request.valid = false;
  check(m.run() == 0 && m.calls == 0, "unsupported numeric/format interpretation");
  m = {}; m.request = rgb;
  check(D3D11VideoFormatSupport(m.request, [&](const auto& q) { return m.image(q); }, [&](auto f, auto t) { return m.view(f, t); }, nullptr) == E_INVALIDARG && !m.calls, "null output argument");

  // YUY2 CheckFormatSupport: the RGBA8 mapping's flags masked to the YUY2 row of the D3D11.1 format table.
  UINT rgba1 = D3D11_FORMAT_SUPPORT_TEXTURE1D | D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_TEXTURE3D
    | D3D11_FORMAT_SUPPORT_TEXTURECUBE | D3D11_FORMAT_SUPPORT_SHADER_LOAD | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE
    | D3D11_FORMAT_SUPPORT_SHADER_GATHER | D3D11_FORMAT_SUPPORT_MIP | D3D11_FORMAT_SUPPORT_MIP_AUTOGEN
    | D3D11_FORMAT_SUPPORT_RENDER_TARGET | D3D11_FORMAT_SUPPORT_BLENDABLE | D3D11_FORMAT_SUPPORT_CPU_LOCKABLE
    | D3D11_FORMAT_SUPPORT_MULTISAMPLE_RENDERTARGET | D3D11_FORMAT_SUPPORT_DISPLAY
    | D3D11_FORMAT_SUPPORT_CAST_WITHIN_BIT_LAYOUT | D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW
    | D3D11_FORMAT_SUPPORT_VIDEO_PROCESSOR_INPUT | D3D11_FORMAT_SUPPORT_VIDEO_PROCESSOR_OUTPUT;
  UINT rgba2 = D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD | D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE
    | D3D11_FORMAT_SUPPORT2_SHAREABLE | D3D11_FORMAT_SUPPORT2_OUTPUT_MERGER_LOGIC_OP | D3D11_FORMAT_SUPPORT2_TILED;
  D3D11MaskYuy2FormatSupport(&rgba1, &rgba2);
  check(rgba1 == (D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_SHADER_LOAD | D3D11_FORMAT_SUPPORT_SHADER_SAMPLE
    | D3D11_FORMAT_SUPPORT_SHADER_GATHER | D3D11_FORMAT_SUPPORT_CPU_LOCKABLE | D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW
    | D3D11_FORMAT_SUPPORT_VIDEO_PROCESSOR_INPUT | D3D11_FORMAT_SUPPORT_VIDEO_PROCESSOR_OUTPUT),
    "YUY2 flags1: the table row (ld, sample, gather4, typed UAV, CPU lockable, video processor)");
  check(rgba2 == (D3D11_FORMAT_SUPPORT2_UAV_TYPED_STORE | D3D11_FORMAT_SUPPORT2_SHAREABLE),
    "YUY2 flags2: typed store and shared resource, no typed load");
  UINT none1 = D3D11_FORMAT_SUPPORT_RENDER_TARGET, none2 = D3D11_FORMAT_SUPPORT2_UAV_TYPED_LOAD;
  D3D11MaskYuy2FormatSupport(&none1, &none2);
  check(none1 == 0 && none2 == 0, "YUY2 mask adds no flag that the mapping did not report");
  D3D11MaskYuy2FormatSupport(nullptr, nullptr);
  check(true, "YUY2 mask accepts null outputs");
  printf("DIRECTIONAL-FORMAT %s %d checks %d failures\n", failures ? "FAIL" : "PASS", checks, failures);
  return failures ? 1 : 0;
}
