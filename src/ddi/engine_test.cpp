// Offline positive control for bc250dxvk.dll (engine ABI 1.0) on any Vulkan 1.3 GPU. No window; exits.
//
// Plays the UMD shell: owns the VkInstance and VkDevice (E1), creates the device from the engine's
// requirements, allocates the render target image itself (E5), and feeds shaders in DDI form: the token
// stream plus register-only signatures, with every semantic name thrown away. Checks the pixels of one
// clear and one draw, and that every queue submission ran on the calling thread (E2).
//
// Usage: bc250dxvk_engine_test.exe <path to bc250dxvk.dll> [adapter substring]
// Exit code 0 = all checks passed.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <vulkan/vulkan.h>

#include "bc250_dxvk_engine.h"

#include <wrl/client.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace {

  int g_failures = 0;

  void Check(bool ok, const char* what) {
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
      g_failures++;
  }

  bool CheckHr(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
      std::printf("FAIL  %s: hr=0x%08lX\n", what, static_cast<unsigned long>(hr));
      g_failures++;
      return false;
    }
    std::printf("ok    %s\n", what);
    return true;
  }

  // ---- shell services -------------------------------------------------------------------------

  struct Shell {
    CRITICAL_SECTION      queueLock;
    DWORD                 mainThread     = 0;
    std::atomic<uint32_t> lockCalls      { 0u };
    std::atomic<uint32_t> foreignCalls   { 0u };
    std::atomic<uint32_t> logLines       { 0u };
  };

  void APIENTRY QueueLock(void* shell, BOOL lock) {
    auto s = static_cast<Shell*>(shell);
    s->lockCalls++;

    if (GetCurrentThreadId() != s->mainThread)
      s->foreignCalls++;

    if (lock)
      EnterCriticalSection(&s->queueLock);
    else
      LeaveCriticalSection(&s->queueLock);
  }

  void APIENTRY Log(void* shell, UINT32 level, const char* message) {
    static_cast<Shell*>(shell)->logLines++;
    std::printf("engine[%u]: %s\n", level, message);
  }

  // ---- threads ----------------------------------------------------------------------------------

  std::set<DWORD> ProcessThreads() {
    std::set<DWORD> result;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);

    if (snap == INVALID_HANDLE_VALUE)
      return result;

    THREADENTRY32 te = { sizeof(te) };
    DWORD pid = GetCurrentProcessId();

    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
      if (te.th32OwnerProcessID == pid)
        result.insert(te.th32ThreadID);
    }

    CloseHandle(snap);
    return result;
  }

  using PFN_NtQueryInformationThread = LONG (WINAPI*)(HANDLE, ULONG, PVOID, ULONG, PULONG);

  // Start address of a thread, to attribute it to a module (ThreadQuerySetWin32StartAddress = 9)
  void* ThreadStartAddress(DWORD tid) {
    static auto query = reinterpret_cast<PFN_NtQueryInformationThread>(
      GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));

    HANDLE h = OpenThread(THREAD_QUERY_INFORMATION, FALSE, tid);
    void* start = nullptr;

    if (h && query)
      query(h, 9, &start, sizeof(start), nullptr);

    if (h)
      CloseHandle(h);

    return start;
  }

  std::string ModuleOf(void* address) {
    HMODULE module = nullptr;

    if (!address || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
        | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(address), &module))
      return "?";

    char name[MAX_PATH] = { };
    GetModuleBaseNameA(GetCurrentProcess(), module, name, MAX_PATH);
    return name;
  }

  // ---- DDI-form shaders -------------------------------------------------------------------------

  // What the runtime gives a driver: the program tokens and signatures without names.
  struct DdiShader {
    Microsoft::WRL::ComPtr<ID3DBlob>        blob;
    const UINT*                             tokens = nullptr;
    std::vector<BC250_DXVK_SIGNATURE_ENTRY> input;
    std::vector<BC250_DXVK_SIGNATURE_ENTRY> output;
  };

  const UINT* FindProgramChunk(const void* dxbc, size_t size) {
    auto bytes = static_cast<const uint8_t*>(dxbc);
    uint32_t count = 0u;
    std::memcpy(&count, bytes + 28, 4);

    for (uint32_t i = 0u; i < count; i++) {
      uint32_t offset = 0u;
      std::memcpy(&offset, bytes + 32 + 4 * i, 4);

      if (offset + 8 > size)
        return nullptr;

      if (!std::memcmp(bytes + offset, "SHEX", 4) || !std::memcmp(bytes + offset, "SHDR", 4))
        return reinterpret_cast<const UINT*>(bytes + offset + 8);
    }

    return nullptr;
  }

  // D3D_NAME values below D3D_NAME_TARGET equal D3D10_SB_NAME; targets, depth and coverage have no
  // D3D10_SB_NAME, so the runtime passes UNDEFINED (targets) or no register (depth, coverage).
  BC250_DXVK_SIGNATURE_ENTRY ToDdi(const D3D11_SIGNATURE_PARAMETER_DESC& p) {
    BC250_DXVK_SIGNATURE_ENTRY e = { };
    e.SystemValue   = p.SystemValueType < D3D_NAME_TARGET ? UINT(p.SystemValueType) : 0u;
    e.Register      = p.Register;
    e.Mask          = p.Mask;
    e.Stream        = BYTE(p.Stream);
    e.ComponentType = UINT(p.ComponentType);
    e.MinPrecision  = UINT(p.MinPrecision);
    return e;
  }

  bool CompileDdi(const char* source, const char* entry, const char* target, DdiShader* out) {
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    HRESULT hr = D3DCompile(source, std::strlen(source), entry, nullptr, nullptr, entry, target,
      D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &out->blob, &errors);

    if (FAILED(hr)) {
      std::printf("D3DCompile %s: %s\n", entry, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
      return false;
    }

    out->tokens = FindProgramChunk(out->blob->GetBufferPointer(), out->blob->GetBufferSize());

    Microsoft::WRL::ComPtr<ID3D11ShaderReflection> reflection;
    hr = D3DReflect(out->blob->GetBufferPointer(), out->blob->GetBufferSize(), IID_PPV_ARGS(&reflection));

    if (FAILED(hr) || !out->tokens)
      return false;

    D3D11_SHADER_DESC desc = { };
    reflection->GetDesc(&desc);

    for (UINT i = 0u; i < desc.InputParameters; i++) {
      D3D11_SIGNATURE_PARAMETER_DESC p = { };
      reflection->GetInputParameterDesc(i, &p);
      out->input.push_back(ToDdi(p));
    }

    for (UINT i = 0u; i < desc.OutputParameters; i++) {
      D3D11_SIGNATURE_PARAMETER_DESC p = { };
      reflection->GetOutputParameterDesc(i, &p);
      out->output.push_back(ToDdi(p));
    }

    return true;
  }

  BC250_DXVK_SHADER_DESC MakeDesc(const DdiShader& s) {
    BC250_DXVK_SHADER_DESC d = { };
    d.Size              = sizeof(d);
    d.Code              = s.tokens;
    d.Input.Entries     = s.input.data();
    d.Input.NumEntries  = UINT(s.input.size());
    d.Output.Entries    = s.output.data();
    d.Output.NumEntries = UINT(s.output.size());
    return d;
  }

  // Semantic names are deliberately unusual: none of them can reach the engine.
  const char* g_hlsl = R"(
    struct VsOut {
      float4 pos : SV_Position;
      float3 col : QUUX7;
    };

    VsOut vs(float2 pos : FROB3, float3 col : ZORK1) {
      VsOut o;
      o.pos = float4(pos, 0.0f, 1.0f);
      o.col = col;
      return o;
    }

    float4 ps(VsOut i) : SV_Target {
      return float4(i.col, 1.0f);
    }
  )";

  // ---- Vulkan -------------------------------------------------------------------------------------

  struct Vk {
    PFN_vkGetInstanceProcAddr                   getInstanceProcAddr = nullptr;
    PFN_vkCreateInstance                        createInstance = nullptr;
    PFN_vkDestroyInstance                       destroyInstance = nullptr;
    PFN_vkEnumeratePhysicalDevices              enumeratePhysicalDevices = nullptr;
    PFN_vkGetPhysicalDeviceProperties           getPhysicalDeviceProperties = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties     getPhysicalDeviceMemoryProperties = nullptr;
    PFN_vkCreateDevice                          createDevice = nullptr;
    PFN_vkGetDeviceProcAddr                     getDeviceProcAddr = nullptr;
    PFN_vkDestroyDevice                         destroyDevice = nullptr;
    PFN_vkGetDeviceQueue                        getDeviceQueue = nullptr;
    PFN_vkCreateImage                           createImage = nullptr;
    PFN_vkDestroyImage                          destroyImage = nullptr;
    PFN_vkGetImageMemoryRequirements            getImageMemoryRequirements = nullptr;
    PFN_vkAllocateMemory                        allocateMemory = nullptr;
    PFN_vkFreeMemory                            freeMemory = nullptr;
    PFN_vkBindImageMemory                       bindImageMemory = nullptr;
  };

  template<typename T>
  void LoadInstance(Vk& vk, VkInstance instance, T* fn, const char* name) {
    *fn = reinterpret_cast<T>(vk.getInstanceProcAddr(instance, name));
  }

}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: bc250dxvk_engine_test <bc250dxvk.dll> [adapter substring]\n");
    return 2;
  }

  std::setvbuf(stdout, nullptr, _IONBF, 0);
  const char* adapterFilter = argc > 2 ? argv[2] : nullptr;

  // ---- Vulkan instance and device, owned by the "shell" ----
  HMODULE vulkanDll = LoadLibraryW(L"vulkan-1.dll");

  if (!vulkanDll) {
    std::printf("FAIL  vulkan-1.dll not found\n");
    return 1;
  }

  Vk vk;
  vk.getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(vulkanDll, "vkGetInstanceProcAddr"));
  LoadInstance(vk, nullptr, &vk.createInstance, "vkCreateInstance");

  VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
  app.pApplicationName = "bc250dxvk-engine-test";
  app.apiVersion       = VK_API_VERSION_1_3;

  VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
  ici.pApplicationInfo = &app;

  VkInstance instance = VK_NULL_HANDLE;

  if (vk.createInstance(&ici, nullptr, &instance) != VK_SUCCESS) {
    std::printf("FAIL  vkCreateInstance\n");
    return 1;
  }

  LoadInstance(vk, instance, &vk.destroyInstance, "vkDestroyInstance");
  LoadInstance(vk, instance, &vk.enumeratePhysicalDevices, "vkEnumeratePhysicalDevices");
  LoadInstance(vk, instance, &vk.getPhysicalDeviceProperties, "vkGetPhysicalDeviceProperties");
  LoadInstance(vk, instance, &vk.getPhysicalDeviceMemoryProperties, "vkGetPhysicalDeviceMemoryProperties");
  LoadInstance(vk, instance, &vk.createDevice, "vkCreateDevice");
  LoadInstance(vk, instance, &vk.getDeviceProcAddr, "vkGetDeviceProcAddr");

  uint32_t physCount = 0u;
  vk.enumeratePhysicalDevices(instance, &physCount, nullptr);
  std::vector<VkPhysicalDevice> phys(physCount);
  vk.enumeratePhysicalDevices(instance, &physCount, phys.data());

  VkPhysicalDevice physDev = VK_NULL_HANDLE;

  for (auto p : phys) {
    VkPhysicalDeviceProperties props = { };
    vk.getPhysicalDeviceProperties(p, &props);
    std::printf("adapter: %s (api %u.%u.%u)\n", props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion),
      VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion));

    if (!physDev && (!adapterFilter || std::strstr(props.deviceName, adapterFilter)))
      physDev = p;
  }

  if (!physDev) {
    std::printf("FAIL  no matching physical device\n");
    return 1;
  }

  // ---- engine ----
  HMODULE engineDll = LoadLibraryA(argv[1]);

  if (!engineDll) {
    std::printf("FAIL  LoadLibrary %s: %lu\n", argv[1], GetLastError());
    return 1;
  }

  auto getFuncs = reinterpret_cast<PFN_BC250_DXVK_ENGINE_GET_FUNCS>(
    GetProcAddress(engineDll, BC250_DXVK_ENGINE_GET_FUNCS_NAME));

  BC250_DXVK_ENGINE_FUNCS funcs = { sizeof(funcs) };

  if (!getFuncs || !CheckHr(getFuncs(BC250_DXVK_ENGINE_ABI_VERSION, &funcs), "Bc250DxvkEngineGetFuncs"))
    return 1;

  BC250_DXVK_VULKAN_INSTANCE vkInstance = { sizeof(vkInstance) };
  vkInstance.GetInstanceProcAddr = vk.getInstanceProcAddr;
  vkInstance.Instance            = instance;
  vkInstance.ApiVersion          = app.apiVersion;
  vkInstance.PhysicalDevice      = physDev;

  BC250_DXVK_ADAPTER_INFO adapterInfo = { sizeof(adapterInfo) };

  if (!CheckHr(funcs.GetAdapterInfo(&vkInstance, &adapterInfo), "GetAdapterInfo"))
    return 1;

  std::printf("      MaxFeatureLevel 0x%x\n", adapterInfo.MaxFeatureLevel);

  BC250_DXVK_DEVICE_REQUIREMENTS req = { sizeof(req) };

  if (!CheckHr(funcs.QueryDeviceRequirements(&vkInstance, &req), "QueryDeviceRequirements"))
    return 1;

  std::printf("      %u device extensions, queue family %u\n", req.ExtensionCount, req.QueueFamily);

  float priority = 1.0f;
  VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
  qci.queueFamilyIndex = req.QueueFamily;
  qci.queueCount       = 1u;
  qci.pQueuePriorities = &priority;

  VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
  dci.pNext                   = req.Features;
  dci.queueCreateInfoCount    = 1u;
  dci.pQueueCreateInfos       = &qci;
  dci.enabledExtensionCount   = req.ExtensionCount;
  dci.ppEnabledExtensionNames = req.ExtensionNames;

  VkDevice device = VK_NULL_HANDLE;
  VkResult vr = vk.createDevice(physDev, &dci, nullptr, &device);
  Check(vr == VK_SUCCESS, "vkCreateDevice with the engine's requirements");

  if (vr != VK_SUCCESS) {
    std::printf("      VkResult %d\n", vr);
    return 1;
  }

  auto dev = [&] (const char* name) { return vk.getDeviceProcAddr(device, name); };
  vk.destroyDevice              = reinterpret_cast<PFN_vkDestroyDevice>(dev("vkDestroyDevice"));
  vk.getDeviceQueue             = reinterpret_cast<PFN_vkGetDeviceQueue>(dev("vkGetDeviceQueue"));
  vk.createImage                = reinterpret_cast<PFN_vkCreateImage>(dev("vkCreateImage"));
  vk.destroyImage               = reinterpret_cast<PFN_vkDestroyImage>(dev("vkDestroyImage"));
  vk.getImageMemoryRequirements = reinterpret_cast<PFN_vkGetImageMemoryRequirements>(dev("vkGetImageMemoryRequirements"));
  vk.allocateMemory             = reinterpret_cast<PFN_vkAllocateMemory>(dev("vkAllocateMemory"));
  vk.freeMemory                 = reinterpret_cast<PFN_vkFreeMemory>(dev("vkFreeMemory"));
  vk.bindImageMemory            = reinterpret_cast<PFN_vkBindImageMemory>(dev("vkBindImageMemory"));

  VkQueue queue = VK_NULL_HANDLE;
  vk.getDeviceQueue(device, req.QueueFamily, 0u, &queue);

  BC250_DXVK_VULKAN_DEVICE vkDevice = { sizeof(vkDevice) };
  vkDevice.Device         = device;
  vkDevice.Queue          = queue;
  vkDevice.QueueFamily    = req.QueueFamily;
  vkDevice.ExtensionCount = req.ExtensionCount;
  vkDevice.ExtensionNames = req.ExtensionNames;
  vkDevice.Features       = req.Features;

  Shell shell;
  InitializeCriticalSection(&shell.queueLock);
  shell.mainThread = GetCurrentThreadId();

  BC250_DXVK_SHELL_SERVICES services = { sizeof(services) };
  services.Shell     = &shell;
  services.QueueLock = &QueueLock;
  services.Log       = &Log;

  BC250_DXVK_DEVICE_CREATE_INFO createInfo = { sizeof(createInfo) };
  createInfo.AbiVersion   = BC250_DXVK_ENGINE_ABI_VERSION;
  createInfo.Instance     = &vkInstance;
  createInfo.Device       = &vkDevice;
  createInfo.Services     = &services;
  createInfo.FeatureLevel = adapterInfo.MaxFeatureLevel;
  createInfo.Threading    = BC250_DXVK_THREADING_INLINE;

  std::set<DWORD> threadsBefore = ProcessThreads();

  IBc250DxvkDevice* engine = nullptr;

  if (!CheckHr(funcs.CreateDevice(&createInfo, &engine), "CreateDevice"))
    return 1;

  Microsoft::WRL::ComPtr<ID3D11Device> d3d;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx;
  CheckHr(engine->GetD3D11Device(IID_PPV_ARGS(&d3d)), "GetD3D11Device");
  CheckHr(engine->GetImmediateContext(IID_PPV_ARGS(&ctx)), "GetImmediateContext");

  if (!d3d || !ctx)
    return 1;

  Check(d3d->GetFeatureLevel() == adapterInfo.MaxFeatureLevel, "device feature level as requested");

  // ---- shaders in DDI form ----
  DdiShader vsDdi, psDdi;
  Check(CompileDdi(g_hlsl, "vs", "vs_5_0", &vsDdi), "compile VS, strip to tokens and register signatures");
  Check(CompileDdi(g_hlsl, "ps", "ps_5_0", &psDdi), "compile PS, strip to tokens and register signatures");

  Microsoft::WRL::ComPtr<ID3D11VertexShader> vs;
  Microsoft::WRL::ComPtr<ID3D11PixelShader>  ps;
  BC250_DXVK_SHADER_DESC vsDesc = MakeDesc(vsDdi);
  BC250_DXVK_SHADER_DESC psDesc = MakeDesc(psDdi);
  CheckHr(engine->CreateShader(&vsDesc, IID_PPV_ARGS(&vs)), "CreateShader (VS, DDI form)");
  CheckHr(engine->CreateShader(&psDesc, IID_PPV_ARGS(&ps)), "CreateShader (PS, DDI form)");

  // ---- input layout in DDI form: registers, not names ----
  VkFormat posFormat = VK_FORMAT_UNDEFINED, colFormat = VK_FORMAT_UNDEFINED;
  UINT posSize = 0u, colSize = 0u;
  CheckHr(engine->GetVertexFormat(DXGI_FORMAT_R32G32_FLOAT, &posFormat, &posSize), "GetVertexFormat R32G32_FLOAT");
  CheckHr(engine->GetVertexFormat(DXGI_FORMAT_R32G32B32_FLOAT, &colFormat, &colSize), "GetVertexFormat R32G32B32_FLOAT");
  Check(posSize == 8u && colSize == 12u, "vertex element sizes 8 and 12");

  BC250_DXVK_VERTEX_ATTRIBUTE attrs[2] = {
    { vsDdi.input.at(0).Register, 0u, posFormat, 0u },
    { vsDdi.input.at(1).Register, 0u, colFormat, posSize },
  };
  BC250_DXVK_VERTEX_BINDING binding = { 0u, posSize + colSize, VK_VERTEX_INPUT_RATE_VERTEX, 0u };
  BC250_DXVK_INPUT_LAYOUT layoutDesc = { 2u, attrs, 1u, &binding };

  Microsoft::WRL::ComPtr<ID3D11InputLayout> layout;
  CheckHr(engine->CreateInputLayout(&layoutDesc, &layout), "CreateInputLayout (DDI form)");

  // ---- render target: shell-allocated VkImage (E5) ----
  constexpr UINT W = 64u, H = 64u;

  D3D11_TEXTURE2D_DESC1 rtDesc = { };
  rtDesc.Width          = W;
  rtDesc.Height         = H;
  rtDesc.MipLevels      = 1u;
  rtDesc.ArraySize      = 1u;
  // TYPELESS with a UNORM view: a mutable image, as a DXGI back buffer that is also viewed as sRGB would be
  rtDesc.Format         = DXGI_FORMAT_R8G8B8A8_TYPELESS;
  rtDesc.SampleDesc     = { 1u, 0u };
  rtDesc.Usage          = D3D11_USAGE_DEFAULT;
  rtDesc.BindFlags      = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

  VkImageCreateInfo imageInfo = { };
  CheckHr(engine->GetImageCreateInfo(&rtDesc, &imageInfo), "GetImageCreateInfo");

  auto formatList = static_cast<const VkImageFormatListCreateInfo*>(imageInfo.pNext);
  Check((imageInfo.flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) && formatList
     && formatList->sType == VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO && formatList->viewFormatCount > 1u,
    "TYPELESS target: mutable image with the engine's view format list in pNext");

  if (formatList)
    std::printf("      view formats: %u, image format %d, usage 0x%x, tiling %d\n", formatList->viewFormatCount,
      imageInfo.format, imageInfo.usage, imageInfo.tiling);

  D3D11_TEXTURE2D_DESC1 stagingLike = rtDesc;
  stagingLike.Usage          = D3D11_USAGE_STAGING;
  stagingLike.BindFlags      = 0u;
  stagingLike.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  VkImageCreateInfo rejected = { };
  Check(engine->GetImageCreateInfo(&stagingLike, &rejected) == E_INVALIDARG,
    "GetImageCreateInfo rejects a CPU-accessible texture");

  VkImage image = VK_NULL_HANDLE;
  vr = vk.createImage(device, &imageInfo, nullptr, &image);
  Check(vr == VK_SUCCESS, "shell vkCreateImage from GetImageCreateInfo");

  VkMemoryRequirements memReq = { };
  vk.getImageMemoryRequirements(device, image, &memReq);

  VkPhysicalDeviceMemoryProperties memProps = { };
  vk.getPhysicalDeviceMemoryProperties(physDev, &memProps);

  uint32_t memType = ~0u;

  for (uint32_t i = 0u; i < memProps.memoryTypeCount && memType == ~0u; i++) {
    if ((memReq.memoryTypeBits & (1u << i))
     && (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
      memType = i;
  }

  VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
  mai.allocationSize  = memReq.size;
  mai.memoryTypeIndex = memType;

  VkDeviceMemory memory = VK_NULL_HANDLE;
  vr = vk.allocateMemory(device, &mai, nullptr, &memory);
  Check(vr == VK_SUCCESS && vk.bindImageMemory(device, image, memory, 0u) == VK_SUCCESS,
    "shell allocates and binds device-local memory");

  Microsoft::WRL::ComPtr<ID3D11Texture2D> rt;
  CheckHr(engine->CreateTexture2DFromImage(&rtDesc, image, &rt), "CreateTexture2DFromImage");

  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
  D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = { };
  rtvDesc.Format        = DXGI_FORMAT_R8G8B8A8_UNORM;
  rtvDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
  CheckHr(d3d->CreateRenderTargetView(rt.Get(), &rtvDesc, &rtv), "CreateRenderTargetView (UNORM view)");

  // ---- vertex buffer ----
  // Upper-left half of the target in pixel space, one colour per corner.
  const float vertices[] = {
    -1.0f,  1.0f,   1.0f, 0.0f, 0.0f,   // top left: red
     1.0f,  1.0f,   0.0f, 1.0f, 0.0f,   // top right: green
    -1.0f, -1.0f,   0.0f, 0.0f, 1.0f,   // bottom left: blue
  };

  D3D11_BUFFER_DESC vbDesc = { sizeof(vertices), D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER };
  D3D11_SUBRESOURCE_DATA vbData = { vertices };
  Microsoft::WRL::ComPtr<ID3D11Buffer> vb;
  CheckHr(d3d->CreateBuffer(&vbDesc, &vbData, &vb), "CreateBuffer (vertices)");

  // ---- staging copy for readback ----
  D3D11_TEXTURE2D_DESC stDesc = { };
  stDesc.Width          = W;
  stDesc.Height         = H;
  stDesc.MipLevels      = 1u;
  stDesc.ArraySize      = 1u;
  stDesc.Format         = DXGI_FORMAT_R8G8B8A8_UNORM;
  stDesc.SampleDesc     = { 1u, 0u };
  stDesc.Usage          = D3D11_USAGE_STAGING;
  stDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

  Microsoft::WRL::ComPtr<ID3D11Texture2D> staging;
  CheckHr(d3d->CreateTexture2D(&stDesc, nullptr, &staging), "CreateTexture2D (staging)");

  if (g_failures)
    return 1;

  // ---- draw ----
  const float clear[4] = { 0.2f, 0.4f, 0.6f, 1.0f };
  ctx->ClearRenderTargetView(rtv.Get(), clear);

  UINT stride = posSize + colSize, offset = 0u;
  ID3D11Buffer* vbs[] = { vb.Get() };
  ID3D11RenderTargetView* rtvs[] = { rtv.Get() };
  D3D11_VIEWPORT viewport = { 0.0f, 0.0f, float(W), float(H), 0.0f, 1.0f };

  ctx->IASetInputLayout(layout.Get());
  ctx->IASetVertexBuffers(0u, 1u, vbs, &stride, &offset);
  ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  ctx->VSSetShader(vs.Get(), nullptr, 0u);
  ctx->PSSetShader(ps.Get(), nullptr, 0u);
  ctx->RSSetViewports(1u, &viewport);
  ctx->OMSetRenderTargets(1u, rtvs, nullptr);
  ctx->Draw(3u, 0u);

  // Present path: flush and submit (E3), then the idle queries the shell uses for its fences.
  CheckHr(engine->SubmitForPresent(rt.Get(), 0u), "SubmitForPresent");
  HRESULT busy = engine->IsResourceBusy(rt.Get(), 0u);
  std::printf("      IsResourceBusy after submit: %s\n", busy == S_OK ? "idle" : busy == S_FALSE ? "busy" : "error");
  CheckHr(engine->WaitForResourceIdle(rt.Get()), "WaitForResourceIdle");
  Check(engine->IsResourceBusy(rt.Get(), 0u) == S_OK, "IsResourceBusy == idle after WaitForResourceIdle");

  ctx->CopyResource(staging.Get(), rt.Get());

  D3D11_MAPPED_SUBRESOURCE mapped = { };
  if (!CheckHr(ctx->Map(staging.Get(), 0u, D3D11_MAP_READ, 0u, &mapped), "Map staging (READ)"))
    return 1;

  auto pixel = [&] (UINT x, UINT y) {
    const uint8_t* p = static_cast<const uint8_t*>(mapped.pData) + y * mapped.RowPitch + x * 4u;
    return std::array<int, 4>{ p[0], p[1], p[2], p[3] };
  };

  auto similar = [] (std::array<int, 4> a, int r, int g, int b, int tol) {
    return std::abs(a[0] - r) <= tol && std::abs(a[1] - g) <= tol && std::abs(a[2] - b) <= tol && a[3] == 255;
  };

  const int cr = 51, cg = 102, cb = 153;   // clear colour in UNORM8

  uint32_t covered = 0u;
  for (UINT y = 0u; y < H; y++) {
    for (UINT x = 0u; x < W; x++) {
      if (!similar(pixel(x, y), cr, cg, cb, 1))
        covered++;
    }
  }

  auto tl = pixel(0u, 0u), tr = pixel(W - 3u, 0u), bl = pixel(0u, H - 3u), br = pixel(W - 1u, H - 1u);
  std::printf("      pixels: (0,0)=%d,%d,%d  (61,0)=%d,%d,%d  (0,61)=%d,%d,%d  (63,63)=%d,%d,%d  covered=%u\n",
    tl[0], tl[1], tl[2], tr[0], tr[1], tr[2], bl[0], bl[1], bl[2], br[0], br[1], br[2], covered);

  Check(similar(br, cr, cg, cb, 1), "clear colour outside the triangle");
  Check(tl[0] > 230 && tl[1] < 25 && tl[2] < 25, "red near the red vertex (VS input register 1 reaches PS)");
  Check(tr[1] > 200 && tr[0] < 25, "green near the green vertex");
  Check(bl[2] > 200 && bl[0] < 25, "blue near the blue vertex");
  // Pixel centres with x + y < 63 are inside; the 64 centres on the hypotenuse depend on the fill rule.
  Check(covered >= 2016u && covered <= 2080u, "covered pixel count matches the half-target triangle");

  ctx->Unmap(staging.Get(), 0u);

  // ---- sustained frames: inline retirement keeps up and memory stays bounded ----
  // Not a performance measurement: a 64x64 target on the development GPU says nothing about the BC-250.
  constexpr UINT FrameCount = 500u;

  PROCESS_MEMORY_COUNTERS_EX memBefore = { sizeof(memBefore) }, memAfter = { sizeof(memAfter) };
  GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memBefore), sizeof(memBefore));

  LARGE_INTEGER freq, t0, t1;
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&t0);

  HRESULT frameHr = S_OK;

  for (UINT i = 0u; i < FrameCount && SUCCEEDED(frameHr); i++) {
    ctx->ClearRenderTargetView(rtv.Get(), clear);
    ctx->Draw(3u, 0u);
    frameHr = engine->SubmitForPresent(rt.Get(), 0u);
  }

  CheckHr(frameHr, "500 x (clear, draw, SubmitForPresent)");
  CheckHr(engine->WaitForResourceIdle(rt.Get()), "WaitForResourceIdle after the frames");
  QueryPerformanceCounter(&t1);

  GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&memAfter), sizeof(memAfter));

  double frameUs = 1e6 * double(t1.QuadPart - t0.QuadPart) / double(freq.QuadPart) / FrameCount;
  double growthMb = (double(memAfter.PrivateUsage) - double(memBefore.PrivateUsage)) / (1024.0 * 1024.0);
  std::printf("      %.1f us per frame (wall, submit-bound), private bytes %+.1f MB\n", frameUs, growthMb);
  Check(growthMb < 64.0, "private bytes grow by less than 64 MB over 500 frames");

  ctx->CopyResource(staging.Get(), rt.Get());

  if (CheckHr(ctx->Map(staging.Get(), 0u, D3D11_MAP_READ, 0u, &mapped), "Map staging after the frames")) {
    Check(similar(pixel(W - 1u, H - 1u), cr, cg, cb, 1) && pixel(0u, 0u)[0] > 230,
      "last frame has the same clear and triangle");
    ctx->Unmap(staging.Get(), 0u);
  }

  // ---- threads and submissions ----
  std::set<DWORD> threadsAfter = ProcessThreads();
  uint32_t engineThreads = 0u;

  for (DWORD tid : threadsAfter) {
    if (threadsBefore.count(tid))
      continue;

    std::string module = ModuleOf(ThreadStartAddress(tid));
    std::printf("      new thread %lu starts in %s\n", static_cast<unsigned long>(tid), module.c_str());

    if (_stricmp(module.c_str(), "bc250dxvk.dll") == 0)
      engineThreads++;
  }

  std::printf("      queue lock calls %u, from other threads %u\n", shell.lockCalls.load(), shell.foreignCalls.load());
  Check(shell.lockCalls.load() > 0u, "the engine brackets queue submissions with QueueLock");
  Check(shell.foreignCalls.load() == 0u, "every QueueLock call came from the calling thread (E2)");
  Check(engineThreads == 0u, "no thread started inside bc250dxvk.dll (E2)");

  // ---- teardown (E4, E5) ----
  rtv.Reset();
  layout.Reset();
  vs.Reset();
  ps.Reset();
  vb.Reset();
  staging.Reset();
  CheckHr(engine->WaitForResourceIdle(rt.Get()), "WaitForResourceIdle before releasing the imported image");
  rt.Reset();
  ctx.Reset();
  d3d.Reset();

  ULONG remaining = engine->Release();
  Check(remaining == 0u, "final engine Release reports no leaked D3D11 references (E4)");

  vk.destroyImage(device, image, nullptr);
  vk.freeMemory(device, memory, nullptr);
  vk.destroyDevice(device, nullptr);
  funcs.FreeDeviceRequirements(&req);
  vk.destroyInstance(instance, nullptr);

  std::printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "PASSED", g_failures);
  return g_failures ? 1 : 0;
}
