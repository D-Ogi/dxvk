// amdgpu_wddm_dxvk.dll entry point: the engine functions of bc250_dxvk_engine.h.

#include <array>
#include <cstring>
#include <vector>

#include "../dxvk/dxvk_instance.h"
#include "../dxvk/dxvk_adapter.h"

#include "ddi_device.h"

namespace dxvk {
  // A system driver lives in every process that opens the adapter: no log file unless DXVK_LOG_PATH names
  // a directory.
  Logger Logger::s_instance("amdgpu_wddm_dxvk.log", true);
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


    // D3D11DeviceFeatures::DetermineUavExtendedTypedLoadSupport on an adapter, before a device exists.
    bool HasTypedUavLoadFormats(const Rc<DxvkAdapter>& adapter) {
      static const std::array<VkFormat, 18> formats = {{
        VK_FORMAT_R32_SFLOAT,           VK_FORMAT_R32_UINT,           VK_FORMAT_R32_SINT,
        VK_FORMAT_R32G32B32A32_SFLOAT,  VK_FORMAT_R32G32B32A32_UINT,  VK_FORMAT_R32G32B32A32_SINT,
        VK_FORMAT_R16G16B16A16_SFLOAT,  VK_FORMAT_R16G16B16A16_UINT,  VK_FORMAT_R16G16B16A16_SINT,
        VK_FORMAT_R8G8B8A8_UNORM,       VK_FORMAT_R8G8B8A8_UINT,      VK_FORMAT_R8G8B8A8_SINT,
        VK_FORMAT_R16_SFLOAT,           VK_FORMAT_R16_UINT,           VK_FORMAT_R16_SINT,
        VK_FORMAT_R8_UNORM,             VK_FORMAT_R8_UINT,            VK_FORMAT_R8_SINT,
      }};

      if (adapter == nullptr)
        return false;

      for (auto f : formats) {
        DxvkFormatFeatures features = adapter->getFormatFeatures(f);

        if (!((features.optimal | features.linear) & VK_FORMAT_FEATURE_2_STORAGE_READ_WITHOUT_FORMAT_BIT))
          return false;
      }

      return true;
    }


    bool IsValidInstance(const BC250_DXVK_VULKAN_INSTANCE* vk) {
      return vk && vk->Size >= sizeof(*vk) && vk->GetInstanceProcAddr
          && vk->Instance && vk->PhysicalDevice;
    }


    // Where the on-disk shader cache goes in this process, if anywhere.
    struct ShaderCachePolicy {
      bool        enable = false;
      std::string directory;
      std::string reason;
    };


    // Why the process's token rules the cache out, or nullptr. The router's allowlist decides which
    // processes reach this engine; this does not rely on it. Only an ordinary user account has a profile
    // of its own to write to: service, desktop (DWM) and font driver (UMFD) accounts do not, and neither do
    // sandboxed tokens or integrity levels below medium, which must not leave files behind.
    const char* TokenRulesOutCache() {
      HANDLE token = nullptr;

      if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return "process token not readable";

      const char* reason = nullptr;
      DWORD size = 0u;

      alignas(TOKEN_USER) std::array<BYTE, sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE> user = { };

      if (!GetTokenInformation(token, TokenUser, user.data(), DWORD(user.size()), &size)) {
        reason = "token user not readable";
      } else {
        // S-1-5-21-* (local and domain accounts) and S-1-12-1-* (Entra ID accounts)
        PSID sid = reinterpret_cast<const TOKEN_USER*>(user.data())->User.Sid;
        const BYTE* authority = GetSidIdentifierAuthority(sid)->Value;
        UCHAR subCount = *GetSidSubAuthorityCount(sid);
        DWORD first = subCount ? *GetSidSubAuthority(sid, 0u) : 0u;

        bool zeroHigh = !authority[0] && !authority[1] && !authority[2] && !authority[3] && !authority[4];
        bool local = zeroHigh && authority[5] == 5u && first == SECURITY_NT_NON_UNIQUE;
        bool entra = zeroHigh && authority[5] == 12u && first == 1u;

        if (!local && !entra)
          reason = "not a user account (service, desktop or font driver host)";
      }

      DWORD appContainer = 0u;

      if (!reason && (!GetTokenInformation(token, TokenIsAppContainer, &appContainer, sizeof(appContainer), &size)
                   || appContainer))
        reason = "AppContainer";

      if (!reason && IsTokenRestricted(token))
        reason = "restricted token";

      alignas(TOKEN_MANDATORY_LABEL) std::array<BYTE, sizeof(TOKEN_MANDATORY_LABEL) + SECURITY_MAX_SID_SIZE> label = { };

      if (!reason) {
        if (!GetTokenInformation(token, TokenIntegrityLevel, label.data(), DWORD(label.size()), &size)) {
          reason = "integrity level not readable";
        } else {
          PSID sid = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(label.data())->Label.Sid;
          UCHAR subCount = *GetSidSubAuthorityCount(sid);
          DWORD rid = subCount ? *GetSidSubAuthority(sid, subCount - 1u) : 0u;

          if (rid < SECURITY_MANDATORY_MEDIUM_RID)
            reason = "integrity level below medium";
          else if (rid >= SECURITY_MANDATORY_SYSTEM_RID)
            reason = "system integrity level";
        }
      }

      CloseHandle(token);
      return reason;
    }


    // Decided once per process: the token does not change, and the cache is one per process.
    // DXVK_SHADER_CACHE=0 turns it off, DXVK_SHADER_CACHE_PATH moves it (DXVK's own variables);
    // the default is %LOCALAPPDATA%\amdgpu-wddm\dxvk, the user's own, never one that needs elevation.
    const ShaderCachePolicy& GetShaderCachePolicy() {
      static const ShaderCachePolicy policy = [] {
        ShaderCachePolicy p;

        if (env::getEnvVar("DXVK_SHADER_CACHE") == "0") {
          p.reason = "DXVK_SHADER_CACHE=0";
          return p;
        }

        if (const char* reason = TokenRulesOutCache()) {
          p.reason = reason;
          return p;
        }

        p.directory = env::getEnvVar("DXVK_SHADER_CACHE_PATH");

        if (p.directory.empty()) {
          std::string base = env::getEnvVar("LOCALAPPDATA");

          if (base.empty()) {
            p.reason = "no LOCALAPPDATA";
            return p;
          }

          p.directory = base + "\\amdgpu-wddm\\dxvk";
        }

        p.enable = true;
        return p;
      } ();

      return policy;
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
          Logger::warn(str::format("amdgpu_wddm_dxvk: DXVK suitability check: ", error));

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
            throw DxvkError("amdgpu_wddm_dxvk: Feature chain leaves DXVK's feature structure");

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
        Logger::err(str::format("amdgpu_wddm_dxvk: QueryDeviceRequirements: ", e.message()));
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

        // The feature checks of D3D11DeviceFeatures::GetMaxFeatureLevel, applied to the features DXVK
        // enables (QueryDeviceRequirements) before a device exists. Since r8 they include 12_0 and 12_1:
        // tiled resources tier 2, typed UAV loads of the additional formats, conservative rasterization
        // and ROVs. The engine test creates a device at the level reported here, which catches drift.
        const auto& features = caps.getFeatures();
        const auto& properties = caps.getProperties();
        const auto& sparse = properties.core.properties.sparseProperties;
        D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_12_1;

        bool tiledTier2 = features.core.features.sparseBinding
          && features.core.features.sparseResidencyBuffer
          && features.core.features.sparseResidencyImage2D
          && features.core.features.sparseResidencyAliased
          && sparse.residencyStandard2DBlockShape
          && features.core.features.shaderResourceResidency
          && features.core.features.shaderResourceMinLod
          && features.vk12.samplerFilterMinmax
          && properties.vk12.filterMinmaxSingleComponentFormats
          && sparse.residencyNonResidentStrict
          && !sparse.residencyAlignedMipSize;

        if (!features.core.features.drawIndirectFirstInstance
         || !features.core.features.fragmentStoresAndAtomics
         || !features.core.features.multiDrawIndirect
         || !features.core.features.tessellationShader)
          level = D3D_FEATURE_LEVEL_10_1;
        else if (!features.core.features.logicOp
              || !features.core.features.vertexPipelineStoresAndAtomics)
          level = D3D_FEATURE_LEVEL_11_0;
        else if (!tiledTier2 || !HasTypedUavLoadFormats(FindAdapter(instance, vk->PhysicalDevice)))
          level = D3D_FEATURE_LEVEL_11_1;
        else if (!features.extConservativeRasterization
              || !features.extFragmentShaderInterlock.fragmentShaderPixelInterlock)
          level = D3D_FEATURE_LEVEL_12_0;

        info->MaxFeatureLevel = level;
        return S_OK;
      } catch (const DxvkError& e) {
        Logger::err(str::format("amdgpu_wddm_dxvk: GetAdapterInfo: ", e.message()));
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

      // From here on, log lines go to the shell, those of instance and device import included
      std::unique_ptr<ShellLogSink> logSink;

      if (info->Services->Log)
        logSink = std::make_unique<ShellLogSink>(*info->Services);

      try {
        Rc<DxvkInstance> instance = ImportInstance(info->Instance);
        Rc<DxvkAdapter> adapter = FindAdapter(instance, info->Instance->PhysicalDevice);

        if (adapter == nullptr) {
          Logger::err("amdgpu_wddm_dxvk: CreateDevice: Physical device not in the imported instance");
          return E_INVALIDARG;
        }

        DxvkDeviceImportInfo deviceInfo = { };
        deviceInfo.device         = info->Device->Device;
        deviceInfo.queue          = info->Device->Queue;
        deviceInfo.queueFamily    = info->Device->QueueFamily;
        deviceInfo.extensionCount = info->Device->ExtensionCount;
        deviceInfo.extensionNames = const_cast<const char**>(info->Device->ExtensionNames);
        deviceInfo.features       = info->Device->Features;

        // E2, E3: every Vulkan call on the calling thread; no second kernel device. Threads that make no
        // Vulkan call may run: shader translation on DXVK's pipeline workers (dxvk.translateShadersOnWorkers)
        // and the shader cache's writer, which both end with the last device.
        // BC250DXVK_MEASURE_WORKER_THREADS=1 is a measurement switch outside the ABI: DXVK's own worker
        // threads then call Vulkan, which breaks E2, to price inline execution against upstream threading.
        bool workerThreads = env::getEnvVar("BC250DXVK_MEASURE_WORKER_THREADS") == "1";

        if (workerThreads)
          Logger::err("amdgpu_wddm_dxvk: BC250DXVK_MEASURE_WORKER_THREADS=1: DXVK worker threads call Vulkan, "
                      "E2 is broken");

        const ShaderCachePolicy& cachePolicy = GetShaderCachePolicy();

        if (cachePolicy.enable)
          Logger::info(str::format("amdgpu_wddm_dxvk: Shader cache in ", cachePolicy.directory));
        else
          Logger::info(str::format("amdgpu_wddm_dxvk: No shader cache: ", cachePolicy.reason));

        deviceInfo.hostOptions.inlineExecution      = !workerThreads;
        deviceInfo.hostOptions.disableKmt           = true;
        deviceInfo.hostOptions.disableShaderCache   = !cachePolicy.enable;
        deviceInfo.hostOptions.shaderCacheDirectory = cachePolicy.directory;

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
          Logger::err(str::format("amdgpu_wddm_dxvk: Feature level ", info->FeatureLevel,
            " requested, device supports ", maxLevel));
          return DXGI_ERROR_UNSUPPORTED;
        }

        // No DXGI adapter object: the system DXGI above the runtime owns the adapter. DXVK only uses
        // it for GetParent/GetAdapter, which the runtime never forwards to a DDI driver.
        Com<D3D11DXGIDevice> container = new D3D11DXGIDevice(nullptr, nullptr, nullptr,
          instance, adapter, device, info->FeatureLevel, 0u);

        *result = ref(new Bc250DxvkDevice(container.ptr(), *info->Services, std::move(logSink)));
        return S_OK;
      } catch (const DxvkError& e) {
        Logger::err(str::format("amdgpu_wddm_dxvk: CreateDevice: ", e.message()));
        return GetErrorResult(e, E_FAIL);
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
