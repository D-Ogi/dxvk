// bc250dxvk.dll entry point: the engine functions of bc250_dxvk_engine.h.

#include <cstring>
#include <vector>

#include "../dxvk/dxvk_instance.h"
#include "../dxvk/dxvk_adapter.h"

#include "ddi_device.h"

namespace dxvk {
  // A system driver lives in every process that opens the adapter: no log file unless DXVK_LOG_PATH names
  // a directory.
  Logger Logger::s_instance("bc250dxvk.log", true);
}

namespace dxvk::ddi {

  namespace {

    // Owner of the memory behind one BC250_DXVK_DEVICE_REQUIREMENTS (its Owner member).
    struct Requirements {
      std::vector<VkExtensionProperties> extensions;
      std::vector<const char*>          extensionNames;
      std::vector<char>                 featureBlob;
    };


    Rc<DxvkInstance> ImportInstance(const BC250_DXVK_VULKAN_INSTANCE* vk) {
      DxvkInstanceImportInfo info = { };
      info.loaderProc     = vk->GetInstanceProcAddr;
      info.instance       = vk->Instance;
      info.extensionCount = vk->ExtensionCount;
      info.extensionNames = const_cast<const char**>(vk->ExtensionNames);
      info.disableVrXr    = true;
      return new DxvkInstance(info, 0);
    }


    Rc<DxvkAdapter> FindAdapter(const Rc<DxvkInstance>& instance, VkPhysicalDevice handle) {
      for (uint32_t i = 0; i < instance->adapterCount(); i++) {
        Rc<DxvkAdapter> adapter = instance->enumAdapters(i);

        if (adapter->handle() == handle)
          return adapter;
      }

      return nullptr;
    }


    bool IsValidInstance(const BC250_DXVK_VULKAN_INSTANCE* vk) {
      return vk && vk->Size >= sizeof(*vk) && vk->GetInstanceProcAddr
          && vk->Instance && vk->PhysicalDevice;
    }


    HRESULT APIENTRY QueryDeviceRequirements(
      const BC250_DXVK_VULKAN_INSTANCE*       vk,
            BC250_DXVK_DEVICE_REQUIREMENTS*   out) {
      if (!IsValidInstance(vk) || !out || out->Size < sizeof(*out))
        return E_INVALIDARG;

      try {
        Rc<DxvkInstance> instance = ImportInstance(vk);

        // With no create info, the capabilities are what DXVK would enable on a device it creates
        // itself; an external device must enable at least these.
        DxvkDeviceCapabilities caps(*instance, vk->PhysicalDevice, nullptr);

        // DXVK's suitability check includes what only its own swap chain needs (VK_KHR_swapchain);
        // the engine never presents, so a failure here is reported but not fatal. The feature level
        // check in CreateDevice decides.
        char error[256] = { };

        if (!caps.isSuitable(sizeof(error), error))
          Logger::warn(str::format("bc250dxvk: DXVK suitability check: ", error));

        auto req = std::make_unique<Requirements>();

        uint32_t extensionCount = 0u;
        caps.queryDeviceExtensions(&extensionCount, nullptr);
        req->extensions.resize(extensionCount);
        caps.queryDeviceExtensions(&extensionCount, req->extensions.data());

        for (const auto& e : req->extensions)
          req->extensionNames.push_back(e.extensionName);

        size_t blobSize = 0u;
        caps.queryDeviceFeatures(&blobSize, nullptr);
        req->featureBlob.resize(blobSize);
        caps.queryDeviceFeatures(&blobSize, req->featureBlob.data());

        // The blob is a byte copy of DXVK's feature structure, so its pNext chain still points into caps,
        // which dies with this scope. Every chained structure is a member of that structure: move each
        // link to the same offset in the blob.
        auto base = reinterpret_cast<const char*>(&caps.getFeatures());
        auto link = reinterpret_cast<VkBaseOutStructure*>(req->featureBlob.data());

        while (link->pNext) {
          ptrdiff_t offset = reinterpret_cast<const char*>(link->pNext) - base;

          if (offset <= 0 || size_t(offset) + sizeof(VkBaseOutStructure) > blobSize)
            throw DxvkError("bc250dxvk: Feature chain leaves DXVK's feature structure");

          link->pNext = reinterpret_cast<VkBaseOutStructure*>(req->featureBlob.data() + offset);
          link = link->pNext;
        }

        UINT32 size = out->Size;
        std::memset(out, 0, sizeof(*out));

        out->Size           = size;
        out->ExtensionCount = UINT32(req->extensionNames.size());
        out->ExtensionNames = req->extensionNames.data();
        out->Features       = reinterpret_cast<const VkPhysicalDeviceFeatures2*>(req->featureBlob.data());
        out->QueueFamily    = caps.getQueueMapping().graphics.family;
        out->Owner          = req.release();
        return S_OK;
      } catch (const DxvkError& e) {
        Logger::err(str::format("bc250dxvk: QueryDeviceRequirements: ", e.message()));
        return E_FAIL;
      }
    }


    void APIENTRY FreeDeviceRequirements(BC250_DXVK_DEVICE_REQUIREMENTS* req) {
      if (!req)
        return;

      delete static_cast<Requirements*>(req->Owner);

      UINT32 size = req->Size;
      std::memset(req, 0, sizeof(*req));
      req->Size = size;
    }


    HRESULT APIENTRY GetAdapterInfo(
      const BC250_DXVK_VULKAN_INSTANCE*       vk,
            BC250_DXVK_ADAPTER_INFO*          info) {
      if (!IsValidInstance(vk) || !info || info->Size < sizeof(*info))
        return E_INVALIDARG;

      try {
        Rc<DxvkInstance> instance = ImportInstance(vk);
        DxvkDeviceCapabilities caps(*instance, vk->PhysicalDevice, nullptr);

        // The feature checks of D3D11DeviceFeatures::GetMaxFeatureLevel up to 11_1, applied to the
        // features DXVK enables (QueryDeviceRequirements). 12_x needs tiled resources and more; the
        // engine does not claim it in ABI 1.0.
        const auto& features = caps.getFeatures();
        D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_1;

        if (!features.core.features.drawIndirectFirstInstance
         || !features.core.features.fragmentStoresAndAtomics
         || !features.core.features.multiDrawIndirect
         || !features.core.features.tessellationShader)
          level = D3D_FEATURE_LEVEL_10_1;
        else if (!features.core.features.logicOp
              || !features.core.features.vertexPipelineStoresAndAtomics)
          level = D3D_FEATURE_LEVEL_11_0;

        info->MaxFeatureLevel = level;
        return S_OK;
      } catch (const DxvkError& e) {
        Logger::err(str::format("bc250dxvk: GetAdapterInfo: ", e.message()));
        return E_FAIL;
      }
    }


    HRESULT APIENTRY CreateDevice(
      const BC250_DXVK_DEVICE_CREATE_INFO*    info,
            IBc250DxvkDevice**                result) {
      if (!result)
        return E_INVALIDARG;

      *result = nullptr;

      if (!info || info->Size < sizeof(*info) || !IsValidInstance(info->Instance)
       || !info->Device || info->Device->Size < sizeof(*info->Device) || !info->Device->Device
       || !info->Services || info->Services->Size < sizeof(*info->Services))
        return E_INVALIDARG;

      if ((info->AbiVersion >> 16) != BC250_DXVK_ENGINE_ABI_MAJOR)
        return E_NOINTERFACE;

      if (info->Threading != BC250_DXVK_THREADING_INLINE || info->Flags)
        return E_INVALIDARG;

      try {
        Rc<DxvkInstance> instance = ImportInstance(info->Instance);
        Rc<DxvkAdapter> adapter = FindAdapter(instance, info->Instance->PhysicalDevice);

        if (adapter == nullptr) {
          Logger::err("bc250dxvk: CreateDevice: Physical device not in the imported instance");
          return E_INVALIDARG;
        }

        DxvkDeviceImportInfo deviceInfo = { };
        deviceInfo.device         = info->Device->Device;
        deviceInfo.queue          = info->Device->Queue;
        deviceInfo.queueFamily    = info->Device->QueueFamily;
        deviceInfo.extensionCount = info->Device->ExtensionCount;
        deviceInfo.extensionNames = const_cast<const char**>(info->Device->ExtensionNames);
        deviceInfo.features       = info->Device->Features;

        // E2, E3: everything on the calling thread; no second kernel device; no cache writer.
        deviceInfo.hostOptions.inlineExecution    = true;
        deviceInfo.hostOptions.disableKmt         = true;
        deviceInfo.hostOptions.disableShaderCache = true;

        if (info->Services->QueueLock) {
          deviceInfo.queueCallback = [
            lock  = info->Services->QueueLock,
            shell = info->Services->Shell
          ] (bool doLock) {
            lock(shell, doLock ? TRUE : FALSE);
          };
        }

        Rc<DxvkDevice> device = adapter->importDevice(deviceInfo);

        D3D_FEATURE_LEVEL maxLevel = D3D11Device::GetMaxFeatureLevel(*device);

        if (info->FeatureLevel > maxLevel) {
          Logger::err(str::format("bc250dxvk: Feature level ", info->FeatureLevel,
            " requested, device supports ", maxLevel));
          return DXGI_ERROR_UNSUPPORTED;
        }

        // No DXGI adapter object: the system DXGI above the runtime owns the adapter. DXVK only uses
        // it for GetParent/GetAdapter, which the runtime never forwards to a DDI driver.
        Com<D3D11DXGIDevice> container = new D3D11DXGIDevice(nullptr, nullptr, nullptr,
          instance, adapter, device, info->FeatureLevel, 0u);

        *result = ref(new Bc250DxvkDevice(container.ptr(), *info->Services));
        return S_OK;
      } catch (const DxvkError& e) {
        Logger::err(str::format("bc250dxvk: CreateDevice: ", e.message()));
        return E_FAIL;
      }
    }

  }

}


extern "C" HRESULT APIENTRY Bc250DxvkEngineGetFuncs(
        UINT32                    abiVersion,
        BC250_DXVK_ENGINE_FUNCS*  funcs) {
  using namespace dxvk::ddi;

  if (!funcs || funcs->Size < sizeof(*funcs))
    return E_INVALIDARG;

  if ((abiVersion >> 16) != BC250_DXVK_ENGINE_ABI_MAJOR)
    return E_NOINTERFACE;

  funcs->AbiVersion               = BC250_DXVK_ENGINE_ABI_VERSION;
  funcs->QueryDeviceRequirements  = &QueryDeviceRequirements;
  funcs->FreeDeviceRequirements   = &FreeDeviceRequirements;
  funcs->GetAdapterInfo           = &GetAdapterInfo;
  funcs->CreateDevice             = &CreateDevice;
  return S_OK;
}
