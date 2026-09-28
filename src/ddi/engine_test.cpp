// Offline positive control for bc250dxvk.dll (engine ABI 1.0) on any Vulkan 1.3 GPU. No window; exits.
//
// Plays the UMD shell: owns the VkInstance and VkDevice (E1), creates the device from the engine's
// requirements, allocates the render target image itself (E5), and feeds shaders in DDI form: the token
// stream plus register-only signatures, with every semantic name thrown away. Checks the pixels of one
// clear and one draw, storage rotation, DXGI Blt onto an imported image, and that the engine's queue submissions, waits, allocations and
// object creation all ran on the calling thread (E2), by wrapping the Vulkan entry points it is given.
//
// Usage: bc250dxvk_engine_test.exe <path to bc250dxvk.dll> [adapter substring] [--bench]
// Exit code 0 = all checks passed. --bench adds a CPU-bound draw benchmark that prints timings (see RunBench).

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
#include <fstream>
#include <iterator>
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
    std::atomic<uint32_t> foreignLogs    { 0u };
    std::atomic<uint32_t> malformedLogs  { 0u };
    std::vector<std::string> errors;
    std::vector<std::string> lines;

    bool HasError(const char* text) const {
      for (const auto& e : errors) {
        if (e.find(text) != std::string::npos)
          return true;
      }

      return false;
    }
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
    auto s = static_cast<Shell*>(shell);
    s->logLines++;

    if (GetCurrentThreadId() != s->mainThread)
      s->foreignLogs++;

    if (level < 1u || level > 4u || std::strchr(message, '\n'))
      s->malformedLogs++;

    // Called with the engine's log lock held, so no other Log call runs concurrently
    if (level == 1u)
      s->errors.push_back(message);

    s->lines.push_back(std::to_string(level) + ": " + message);

    // Errors and warnings go to the receipt at once. The rest is kept for engine_sink.log: stdout is
    // unbuffered, and a write per line into a redirected pipe costs milliseconds, which would swamp
    // the timings.
    if (level <= 2u)
      std::printf("engine[%u]: %s\n", level, message);
  }

  // The engine's log file for this executable, as DXVK names it: <DXVK_LOG_PATH>/<exe base name>_bc250dxvk.log
  std::string EngineLogFile() {
    char dir[MAX_PATH] = { };
    char exe[MAX_PATH] = { };

    if (!GetEnvironmentVariableA("DXVK_LOG_PATH", dir, MAX_PATH) || !GetModuleFileNameA(nullptr, exe, MAX_PATH))
      return std::string();

    std::string name = exe;
    name = name.substr(name.find_last_of("\\/") + 1u);
    name = name.substr(0u, name.rfind('.'));
    return std::string(dir) + "/" + name + "_bc250dxvk.log";
  }

  std::string ReadFileText(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
  }

  // ---- Vulkan call census -------------------------------------------------------------------------

  // The engine resolves every Vulkan entry point through the vkGetInstanceProcAddr the shell hands it,
  // so the test can wrap the calls that E2 is about and record which thread makes them. Only the
  // engine's calls go through the wrappers; the test's own calls use the loader's pointers directly.
  DWORD g_callerThread = 0u;

  struct VkCallStats {
    std::atomic<uint32_t> calls   { 0u };
    std::atomic<uint32_t> foreign { 0u };
  };

  template<int Id, typename Fn>
  struct VkHook;

  template<int Id, typename R, typename... A>
  struct VkHook<Id, R (VKAPI_PTR*)(A...)> {
    static inline VkCallStats stats;
    static inline R (VKAPI_PTR* real)(A...) = nullptr;

    static R VKAPI_CALL Call(A... args) {
      stats.calls++;

      if (GetCurrentThreadId() != g_callerThread)
        stats.foreign++;

      return real(args...);
    }

    // Instance and device queries may both return the function; either pointer is valid for our one device
    static PFN_vkVoidFunction Install(PFN_vkVoidFunction fn) {
      real = reinterpret_cast<R (VKAPI_PTR*)(A...)>(fn);
      return reinterpret_cast<PFN_vkVoidFunction>(&Call);
    }
  };

  struct VkHookEntry {
    const char*           name;
    PFN_vkVoidFunction  (*install)(PFN_vkVoidFunction);
    const VkCallStats*    stats;
  };

  // One entry per line: the line number keeps aliases with the same type apart
#define BC250_VK_HOOK(fn) { #fn, &VkHook<__LINE__, PFN_##fn>::Install, &VkHook<__LINE__, PFN_##fn>::stats }

  const VkHookEntry g_vkHooks[] = {
    BC250_VK_HOOK(vkQueueSubmit),
    BC250_VK_HOOK(vkQueueSubmit2),
    BC250_VK_HOOK(vkQueueSubmit2KHR),
    BC250_VK_HOOK(vkQueueWaitIdle),
    BC250_VK_HOOK(vkDeviceWaitIdle),
    BC250_VK_HOOK(vkWaitSemaphores),
    BC250_VK_HOOK(vkWaitSemaphoresKHR),
    BC250_VK_HOOK(vkGetSemaphoreCounterValue),
    BC250_VK_HOOK(vkGetSemaphoreCounterValueKHR),
    BC250_VK_HOOK(vkWaitForFences),
    BC250_VK_HOOK(vkGetFenceStatus),
    BC250_VK_HOOK(vkAllocateMemory),
    BC250_VK_HOOK(vkFreeMemory),
    BC250_VK_HOOK(vkMapMemory),
    BC250_VK_HOOK(vkCreateBuffer),
    BC250_VK_HOOK(vkDestroyBuffer),
    BC250_VK_HOOK(vkCreateImage),
    BC250_VK_HOOK(vkDestroyImage),
    BC250_VK_HOOK(vkCreateImageView),
    BC250_VK_HOOK(vkCreateShaderModule),
    BC250_VK_HOOK(vkCreateGraphicsPipelines),
    BC250_VK_HOOK(vkCreateComputePipelines),
    BC250_VK_HOOK(vkAllocateCommandBuffers),
    BC250_VK_HOOK(vkBeginCommandBuffer),
    BC250_VK_HOOK(vkEndCommandBuffer),
    BC250_VK_HOOK(vkResetCommandPool),
  };

#undef BC250_VK_HOOK

  PFN_vkGetInstanceProcAddr g_realGetInstanceProcAddr = nullptr;
  PFN_vkGetDeviceProcAddr   g_realGetDeviceProcAddr   = nullptr;

  PFN_vkVoidFunction HookVk(const char* name, PFN_vkVoidFunction fn) {
    if (fn && name) {
      for (const auto& e : g_vkHooks) {
        if (!std::strcmp(e.name, name))
          return e.install(fn);
      }
    }

    return fn;
  }

  PFN_vkVoidFunction HookVkClassified(const char* name, PFN_vkVoidFunction fn);

  PFN_vkVoidFunction VKAPI_CALL HookedGetDeviceProcAddr(VkDevice device, const char* name) {
    return HookVkClassified(name, g_realGetDeviceProcAddr(device, name));
  }

  PFN_vkVoidFunction VKAPI_CALL HookedGetInstanceProcAddr(VkInstance instance, const char* name) {
    PFN_vkVoidFunction fn = g_realGetInstanceProcAddr(instance, name);

    if (!fn || !name)
      return fn;

    if (!std::strcmp(name, "vkGetInstanceProcAddr"))
      return reinterpret_cast<PFN_vkVoidFunction>(&HookedGetInstanceProcAddr);

    if (!std::strcmp(name, "vkGetDeviceProcAddr")) {
      g_realGetDeviceProcAddr = reinterpret_cast<PFN_vkGetDeviceProcAddr>(fn);
      return reinterpret_cast<PFN_vkVoidFunction>(&HookedGetDeviceProcAddr);
    }

    return HookVkClassified(name, fn);
  }

  const VkCallStats& HookStats(const char* name) {
    for (const auto& e : g_vkHooks) {
      if (!std::strcmp(e.name, name))
        return *e.stats;
    }

    std::abort();
  }

  // Graphics pipelines by kind: with pipeline libraries, a draw with new state fast-links a base pipeline,
  // and inline mode must leave the optimized variant for SubmitForPresent.
  struct PipelineKinds {
    uint32_t libraries = 0u;   // LIBRARY flag: shader, vertex input and fragment output parts
    uint32_t fastLinks = 0u;   // linked from libraries without link-time optimization
    uint32_t optimized = 0u;   // monolithic, or linked with link-time optimization
  };

  PipelineKinds g_pipelineKinds;
  PFN_vkCreateGraphicsPipelines g_hookedCreateGraphicsPipelines = nullptr;

  VkResult VKAPI_CALL ClassifyGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
      const VkGraphicsPipelineCreateInfo* infos, const VkAllocationCallbacks* allocator, VkPipeline* pipelines) {
    for (uint32_t i = 0u; i < count; i++) {
      VkPipelineCreateFlags2 flags = infos[i].flags;
      bool linked = false;

      for (auto s = static_cast<const VkBaseInStructure*>(infos[i].pNext); s; s = s->pNext) {
        if (s->sType == VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO)
          flags = reinterpret_cast<const VkPipelineCreateFlags2CreateInfo*>(s)->flags;
        else if (s->sType == VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR)
          linked = reinterpret_cast<const VkPipelineLibraryCreateInfoKHR*>(s)->libraryCount != 0u;
      }

      if (flags & VK_PIPELINE_CREATE_2_LIBRARY_BIT_KHR)
        g_pipelineKinds.libraries++;
      else if (linked && !(flags & VK_PIPELINE_CREATE_2_LINK_TIME_OPTIMIZATION_BIT_EXT))
        g_pipelineKinds.fastLinks++;
      else
        g_pipelineKinds.optimized++;
    }

    return g_hookedCreateGraphicsPipelines(device, cache, count, infos, allocator, pipelines);
  }

  PFN_vkVoidFunction HookVkClassified(const char* name, PFN_vkVoidFunction fn) {
    fn = HookVk(name, fn);

    if (fn && name && !std::strcmp(name, "vkCreateGraphicsPipelines")) {
      g_hookedCreateGraphicsPipelines = reinterpret_cast<PFN_vkCreateGraphicsPipelines>(fn);
      return reinterpret_cast<PFN_vkVoidFunction>(&ClassifyGraphicsPipelines);
    }

    return fn;
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

  // ---- CPU-bound draw benchmark (--bench) ---------------------------------------------------------

  // Many small draws, each with a constant buffer update and a texture switch, as games issue them. Compares
  // the engine's inline mode (E2) with DXVK's worker threads (BC250DXVK_MEASURE_WORKER_THREADS=1) on one
  // machine. Prints numbers; not a pass/fail check.
  const char* g_benchHlsl = R"(
    cbuffer Draw : register(b0) {
      float4 offset;
      float4 tint;
    };

    Texture2D    tex  : register(t0);
    SamplerState samp : register(s0);

    struct VsOut {
      float4 pos : SV_Position;
      float3 col : QUUX7;
    };

    VsOut vs(float2 pos : FROB3, float3 col : ZORK1) {
      VsOut o;
      o.pos = float4(pos * 0.25f + offset.xy, 0.0f, 1.0f);
      o.col = col * tint.rgb;
      return o;
    }

    float4 ps(VsOut i) : SV_Target {
      return float4(i.col * tex.Sample(samp, i.pos.xy / 64.0f).rgb, 1.0f);
    }
  )";

  double ProcessCpuMs() {
    FILETIME created, exited, kernel, user;
    GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user);

    auto ms = [] (const FILETIME& ft) {
      return double((uint64_t(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) / 1e4;
    };

    return ms(kernel) + ms(user);
  }

  void RunBench(IBc250DxvkDevice* engine, ID3D11Device* d3d, ID3D11DeviceContext* ctx, ID3D11Resource* rt,
      ID3D11RenderTargetView* rtv, ID3D11InputLayout* layout, ID3D11Buffer* vb, UINT stride, UINT size) {
    using Microsoft::WRL::ComPtr;
    constexpr UINT DrawsPerFrame = 1000u, WarmupFrames = 30u, Frames = 300u, Latency = 3u;

    DdiShader vsDdi, psDdi;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader>  ps;

    if (!CompileDdi(g_benchHlsl, "vs", "vs_5_0", &vsDdi) || !CompileDdi(g_benchHlsl, "ps", "ps_5_0", &psDdi)) {
      Check(false, "bench: compile shaders");
      return;
    }

    BC250_DXVK_SHADER_DESC vsDesc = MakeDesc(vsDdi);
    BC250_DXVK_SHADER_DESC psDesc = MakeDesc(psDdi);

    if (!CheckHr(engine->CreateShader(&vsDesc, IID_PPV_ARGS(&vs)), "bench: CreateShader (VS)")
     || !CheckHr(engine->CreateShader(&psDesc, IID_PPV_ARGS(&ps)), "bench: CreateShader (PS)"))
      return;

    D3D11_BUFFER_DESC cbDesc = { 32u, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_WRITE };
    ComPtr<ID3D11Buffer> cb;

    if (!CheckHr(d3d->CreateBuffer(&cbDesc, nullptr, &cb), "bench: CreateBuffer (constants)"))
      return;

    // Two textures to alternate between
    ComPtr<ID3D11ShaderResourceView> srv[2];

    for (UINT i = 0u; i < 2u; i++) {
      uint32_t texels[16];

      for (uint32_t& t : texels)
        t = i ? 0xff80c0ffu : 0xffffc080u;

      D3D11_TEXTURE2D_DESC td = { 4u, 4u, 1u, 1u, DXGI_FORMAT_R8G8B8A8_UNORM, { 1u, 0u },
        D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE };
      D3D11_SUBRESOURCE_DATA data = { texels, 16u };
      ComPtr<ID3D11Texture2D> tex;

      if (!CheckHr(d3d->CreateTexture2D(&td, &data, &tex), "bench: CreateTexture2D")
       || !CheckHr(d3d->CreateShaderResourceView(tex.Get(), nullptr, &srv[i]), "bench: CreateShaderResourceView"))
        return;
    }

    D3D11_SAMPLER_DESC sd = { };
    sd.Filter         = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sd.AddressU       = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.AddressV       = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.AddressW       = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD         = D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler;

    if (!CheckHr(d3d->CreateSamplerState(&sd, &sampler), "bench: CreateSamplerState"))
      return;

    ComPtr<ID3D11Query> events[Latency];
    D3D11_QUERY_DESC qd = { D3D11_QUERY_EVENT, 0u };

    for (auto& e : events) {
      if (!CheckHr(d3d->CreateQuery(&qd, &e), "bench: CreateQuery (event)"))
        return;
    }

    // The same vertex inputs as the test's first draw, so its layout and vertices fit
    ctx->ClearState();

    UINT offset = 0u;
    D3D11_VIEWPORT viewport = { 0.0f, 0.0f, float(size), float(size), 0.0f, 1.0f };
    ctx->IASetInputLayout(layout);
    ctx->IASetVertexBuffers(0u, 1u, &vb, &stride, &offset);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->RSSetViewports(1u, &viewport);

    ID3D11Buffer* cbs[] = { cb.Get() };
    ID3D11SamplerState* samplers[] = { sampler.Get() };
    ctx->VSSetShader(vs.Get(), nullptr, 0u);
    ctx->PSSetShader(ps.Get(), nullptr, 0u);
    ctx->VSSetConstantBuffers(0u, 1u, cbs);
    ctx->PSSetSamplers(0u, 1u, samplers);
    ctx->OMSetRenderTargets(1u, &rtv, nullptr);

    const float clear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

    auto frame = [&] (UINT index) -> HRESULT {
      // Frame latency limit, like DXGI's maximum frame latency: wait for the frame issued Latency frames ago
      ID3D11Query* event = events[index % Latency].Get();

      if (index >= Latency) {
        ULONGLONG deadline = GetTickCount64() + 2000u;
        HRESULT hr;
        BOOL done = FALSE;

        while ((hr = ctx->GetData(event, &done, sizeof(done), 0u)) == S_FALSE) {
          if (GetTickCount64() > deadline)
            return E_FAIL;

          YieldProcessor();
        }

        if (FAILED(hr))
          return hr;
      }

      ctx->ClearRenderTargetView(rtv, clear);

      for (UINT d = 0u; d < DrawsPerFrame; d++) {
        D3D11_MAPPED_SUBRESOURCE mapped = { };
        HRESULT hr = ctx->Map(cb.Get(), 0u, D3D11_MAP_WRITE_DISCARD, 0u, &mapped);

        if (FAILED(hr))
          return hr;

        const float constants[8] = {
          float(d % 7u) * 0.25f - 0.75f, float(d % 5u) * 0.25f - 0.5f, 0.0f, 0.0f,
          1.0f, 1.0f, 1.0f, 1.0f,
        };

        std::memcpy(mapped.pData, constants, sizeof(constants));
        ctx->Unmap(cb.Get(), 0u);

        ID3D11ShaderResourceView* view = srv[d & 1u].Get();
        ctx->PSSetShaderResources(0u, 1u, &view);
        ctx->Draw(3u, 0u);
      }

      ctx->End(event);
      return engine->SubmitForPresent(rt, 0u);
    };

    HRESULT hr = S_OK;

    for (UINT i = 0u; i < WarmupFrames && SUCCEEDED(hr); i++)
      hr = frame(i);

    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    double cpu0 = ProcessCpuMs();

    for (UINT i = WarmupFrames; i < WarmupFrames + Frames && SUCCEEDED(hr); i++)
      hr = frame(i);

    if (SUCCEEDED(hr))
      hr = engine->WaitForResourceIdle(rt);

    QueryPerformanceCounter(&t1);
    double cpu1 = ProcessCpuMs();

    if (!CheckHr(hr, "bench: frames"))
      return;

    double wallUs = 1e6 * double(t1.QuadPart - t0.QuadPart) / double(freq.QuadPart) / Frames;
    double cpuUs  = 1e3 * (cpu1 - cpu0) / Frames;

    char mode[8] = { };
    bool workers = GetEnvironmentVariableA("BC250DXVK_MEASURE_WORKER_THREADS", mode, sizeof(mode)) && mode[0] == '1';

    std::printf("      bench (%s): %u draws per frame, %u frames, latency %u: %.0f us wall, %.0f us process CPU "
      "per frame; %.2f us wall per draw\n", workers ? "DXVK worker threads" : "inline", DrawsPerFrame, Frames,
      Latency, wallUs, cpuUs, wallUs / DrawsPerFrame);
  }

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
    std::printf("usage: bc250dxvk_engine_test <bc250dxvk.dll> [adapter substring] [--bench]\n");
    return 2;
  }

  std::setvbuf(stdout, nullptr, _IONBF, 0);
  const char* adapterFilter = nullptr;
  bool bench = false;

  for (int i = 2; i < argc; i++) {
    if (!std::strcmp(argv[i], "--bench"))
      bench = true;
    else
      adapterFilter = argv[i];
  }

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

  g_callerThread = GetCurrentThreadId();
  g_realGetInstanceProcAddr = vk.getInstanceProcAddr;

  BC250_DXVK_VULKAN_INSTANCE vkInstance = { sizeof(vkInstance) };
  vkInstance.GetInstanceProcAddr = &HookedGetInstanceProcAddr;
  vkInstance.Instance            = instance;
  vkInstance.ApiVersion          = app.apiVersion;
  vkInstance.PhysicalDevice      = physDev;

  // Wall time of the adapter-level calls and of device creation, each of which imports its own DxvkInstance.
  // Informational: the development GPU says little about the BC-250.
  auto nowMs = [] {
    LARGE_INTEGER t, f;
    QueryPerformanceCounter(&t);
    QueryPerformanceFrequency(&f);
    return 1e3 * double(t.QuadPart) / double(f.QuadPart);
  };

  BC250_DXVK_ADAPTER_INFO adapterInfo = { sizeof(adapterInfo) };
  double tAdapter = nowMs();

  if (!CheckHr(funcs.GetAdapterInfo(&vkInstance, &adapterInfo), "GetAdapterInfo"))
    return 1;

  std::printf("      MaxFeatureLevel 0x%x, %.1f ms\n", adapterInfo.MaxFeatureLevel, nowMs() - tAdapter);

  BC250_DXVK_DEVICE_REQUIREMENTS req = { sizeof(req) };
  double tRequirements = nowMs();

  if (!CheckHr(funcs.QueryDeviceRequirements(&vkInstance, &req), "QueryDeviceRequirements"))
    return 1;

  std::printf("      %u device extensions, queue family %u, %.1f ms\n", req.ExtensionCount, req.QueueFamily,
    nowMs() - tRequirements);

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

  // A failed CreateDevice reports its reason through Log and leaves no sink behind: the real device's
  // lines below must then reach its own shell, not this one.
  {
    Shell probe;
    probe.mainThread = GetCurrentThreadId();

    BC250_DXVK_SHELL_SERVICES probeServices = { sizeof(probeServices) };
    probeServices.Shell = &probe;
    probeServices.Log   = &Log;

    BC250_DXVK_VULKAN_INSTANCE outside = vkInstance;
    outside.PhysicalDevice = reinterpret_cast<VkPhysicalDevice>(uintptr_t(0x10u));

    BC250_DXVK_DEVICE_CREATE_INFO probeInfo = createInfo;
    probeInfo.Instance = &outside;
    probeInfo.Services = &probeServices;

    IBc250DxvkDevice* none = nullptr;
    Check(funcs.CreateDevice(&probeInfo, &none) == E_INVALIDARG && !none,
      "CreateDevice rejects a physical device outside the instance");
    Check(probe.HasError("Physical device not in the imported instance"),
      "the failed CreateDevice reports its reason through Log, level 1");
  }

  std::set<DWORD> threadsBefore = ProcessThreads();

  IBc250DxvkDevice* engine = nullptr;
  double tCreate = nowMs();

  if (!CheckHr(funcs.CreateDevice(&createInfo, &engine), "CreateDevice"))
    return 1;

  std::printf("      %.1f ms\n", nowMs() - tCreate);

  Check(shell.logLines.load() > 0u, "CreateDevice's own lines (device import) reach the shell's Log");

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

  VkFormat notVertex = VK_FORMAT_UNDEFINED;
  UINT notVertexSize = 0u;
  Check(engine->GetVertexFormat(DXGI_FORMAT_BC1_UNORM, &notVertex, &notVertexSize) == E_INVALIDARG
     && engine->GetVertexFormat(DXGI_FORMAT_D32_FLOAT, &notVertex, &notVertexSize) == E_INVALIDARG,
    "GetVertexFormat rejects formats that are not vertex formats (BC1, D32)");

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

  // The first draw with this state. Executing it (Flush, E3) fast-links a base pipeline when the device has
  // pipeline libraries; in inline mode the optimized variant waits for SubmitForPresent.
  const PipelineKinds kindsBefore = g_pipelineKinds;
  ctx->Draw(3u, 0u);
  ctx->Flush();
  const PipelineKinds kindsDraw = g_pipelineKinds;

  // Present path: flush and submit (E3), then the idle queries the shell uses for its fences.
  CheckHr(engine->SubmitForPresent(rt.Get(), 0u), "SubmitForPresent");
  const PipelineKinds kindsPresent = g_pipelineKinds;

  const uint32_t drawLinks        = kindsDraw.fastLinks - kindsBefore.fastLinks;
  const uint32_t drawOptimized    = kindsDraw.optimized - kindsBefore.optimized;
  const uint32_t presentOptimized = kindsPresent.optimized - kindsDraw.optimized;
  std::printf("      first draw: %u fast-linked, %u optimized pipelines; SubmitForPresent: %u optimized\n",
    drawLinks, drawOptimized, presentOptimized);

  if (drawLinks) {
    Check(drawOptimized == 0u, "the draw fast-links and compiles no optimized pipeline (inline mode)");
    Check(presentOptimized >= 1u && presentOptimized <= drawLinks,
      "SubmitForPresent compiles the deferred optimized pipeline");
  } else {
    std::printf("      no fast-linked pipeline on this device: deferred compiles not exercised\n");
  }

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

  // The frames reuse the first draw's state, so by now every fast-linked pipeline has its optimized variant,
  // and the last frame above was drawn with it.
  if (drawLinks) {
    Check(g_pipelineKinds.optimized - kindsBefore.optimized == g_pipelineKinds.fastLinks - kindsBefore.fastLinks,
      "the deferred compiles caught up: one optimized pipeline per fast-linked one");
  }

  // ---- RotateResourceIdentities: storage moves, views follow ----
  constexpr UINT RotCount = 3u;
  VkImage rotImages[RotCount] = { };
  VkDeviceMemory rotMemory[RotCount] = { };
  Microsoft::WRL::ComPtr<ID3D11Texture2D> rotTex[RotCount];
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rotRtv[RotCount];
  const float rotColors[RotCount][4] = { { 1.0f, 0.0f, 0.0f, 1.0f }, { 0.0f, 1.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, 1.0f, 1.0f } };
  bool rotReady = true;

  for (UINT i = 0u; i < RotCount; i++) {
    VkMemoryRequirements rotReq = { };
    bool ok = vk.createImage(device, &imageInfo, nullptr, &rotImages[i]) == VK_SUCCESS;

    if (ok) {
      vk.getImageMemoryRequirements(device, rotImages[i], &rotReq);

      VkMemoryAllocateInfo rotAlloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
      rotAlloc.allocationSize  = rotReq.size;
      rotAlloc.memoryTypeIndex = memType;

      ok = vk.allocateMemory(device, &rotAlloc, nullptr, &rotMemory[i]) == VK_SUCCESS
        && vk.bindImageMemory(device, rotImages[i], rotMemory[i], 0u) == VK_SUCCESS
        && SUCCEEDED(engine->CreateTexture2DFromImage(&rtDesc, rotImages[i], &rotTex[i]))
        && SUCCEEDED(d3d->CreateRenderTargetView(rotTex[i].Get(), &rtvDesc, &rotRtv[i]));
    }

    if (ok)
      ctx->ClearRenderTargetView(rotRtv[i].Get(), rotColors[i]);

    rotReady = rotReady && ok;
  }

  Check(rotReady, "rotation set: three shell images, textures and RTVs, cleared red, green, blue");

  auto readPixel = [&] (ID3D11Texture2D* texture) {
    std::array<int, 4> result = { -1, -1, -1, -1 };
    D3D11_MAPPED_SUBRESOURCE m = { };
    ctx->CopyResource(staging.Get(), texture);

    if (SUCCEEDED(ctx->Map(staging.Get(), 0u, D3D11_MAP_READ, 0u, &m))) {
      const uint8_t* p = static_cast<const uint8_t*>(m.pData) + 5u * m.RowPitch + 5u * 4u;
      result = { p[0], p[1], p[2], p[3] };
      ctx->Unmap(staging.Get(), 0u);
    }

    return result;
  };

  if (rotReady) {
    ID3D11Resource* rotation[RotCount] = { rotTex[0].Get(), rotTex[1].Get(), rotTex[2].Get() };
    CheckHr(engine->RotateResourceIdentities(rotation, RotCount), "RotateResourceIdentities (3 textures)");

    auto r0 = readPixel(rotTex[0].Get()), r1 = readPixel(rotTex[1].Get()), r2 = readPixel(rotTex[2].Get());
    std::printf("      after rotation: %d,%d,%d  %d,%d,%d  %d,%d,%d\n",
      r0[0], r0[1], r0[2], r1[0], r1[1], r1[2], r2[0], r2[1], r2[2]);
    Check(similar(r0, 0, 255, 0, 1) && similar(r1, 0, 0, 255, 1) && similar(r2, 255, 0, 0, 1),
      "texture i shows the former storage of texture i + 1, the last one that of the first");

    const float white[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    ctx->ClearRenderTargetView(rotRtv[0].Get(), white);
    Check(similar(readPixel(rotTex[0].Get()), 255, 255, 255, 1) && similar(readPixel(rotTex[1].Get()), 0, 0, 255, 1),
      "an RTV created before the rotation writes the new storage only");

    ID3D11Resource* mixed[2] = { rotTex[0].Get(), staging.Get() };
    Check(engine->RotateResourceIdentities(mixed, 2u) == E_INVALIDARG, "rotation rejects a staging texture");

    ID3D11Resource* twice[2] = { rotTex[1].Get(), rotTex[1].Get() };
    Check(engine->RotateResourceIdentities(twice, 2u) == E_INVALIDARG, "rotation rejects the same texture twice");
  }

  // ---- DXGI Blt onto the shell image, as onto a runtime-allocated shared or proxy surface ----
  {
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> ctx1;
    CheckHr(ctx.As(&ctx1), "QueryInterface ID3D11DeviceContext1");

    auto readAt = [&] (ID3D11Texture2D* stagingTexture, ID3D11Texture2D* texture, UINT x, UINT y) {
      std::array<int, 4> result = { -1, -1, -1, -1 };
      D3D11_MAPPED_SUBRESOURCE m = { };
      ctx->CopyResource(stagingTexture, texture);

      if (SUCCEEDED(ctx->Map(stagingTexture, 0u, D3D11_MAP_READ, 0u, &m))) {
        const uint8_t* p = static_cast<const uint8_t*>(m.pData) + y * m.RowPitch + x * 4u;
        result = { p[0], p[1], p[2], p[3] };
        ctx->Unmap(stagingTexture, 0u);
      }

      return result;
    };

    auto same = [&] (std::array<int, 4> a, std::array<int, 4> b) {
      return similar(a, b[0], b[1], b[2], 1);
    };

    // Source: an engine texture in an sRGB format, with red, green and blue quadrants and a grey one whose
    // stored sRGB encoding (about 188) differs from its linear value, so a decoding Blt would show
    D3D11_TEXTURE2D_DESC srcDesc = { };
    srcDesc.Width      = 32u;
    srcDesc.Height     = 32u;
    srcDesc.MipLevels  = 1u;
    srcDesc.ArraySize  = 1u;
    srcDesc.Format     = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    srcDesc.SampleDesc = { 1u, 0u };
    srcDesc.Usage      = D3D11_USAGE_DEFAULT;
    srcDesc.BindFlags  = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    D3D11_TEXTURE2D_DESC srcStagingDesc = srcDesc;
    srcStagingDesc.Usage          = D3D11_USAGE_STAGING;
    srcStagingDesc.BindFlags      = 0u;
    srcStagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> src, srcStaging;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> srcRtv;
    bool bltReady = CheckHr(d3d->CreateTexture2D(&srcDesc, nullptr, &src), "CreateTexture2D (Blt source, sRGB)")
      && CheckHr(d3d->CreateTexture2D(&srcStagingDesc, nullptr, &srcStaging), "CreateTexture2D (Blt source staging)")
      && CheckHr(d3d->CreateRenderTargetView(src.Get(), nullptr, &srcRtv), "CreateRenderTargetView (Blt source)");

    if (bltReady) {
      const float grey[4] = { 0.5f, 0.5f, 0.5f, 1.0f };
      const float quadColors[3][4] = { { 1.0f, 0.0f, 0.0f, 1.0f }, { 0.0f, 1.0f, 0.0f, 1.0f }, { 0.0f, 0.0f, 1.0f, 1.0f } };
      const D3D11_RECT quads[4] = { { 0, 0, 16, 16 }, { 16, 0, 32, 16 }, { 0, 16, 16, 32 }, { 16, 16, 32, 32 } };

      ctx->ClearRenderTargetView(srcRtv.Get(), grey);

      for (UINT i = 0u; i < 3u; i++)
        ctx1->ClearView(srcRtv.Get(), quadColors[i], &quads[i], 1u);

      std::array<int, 4> q[4];

      for (UINT i = 0u; i < 4u; i++)
        q[i] = readAt(srcStaging.Get(), src.Get(), UINT(quads[i].left) + 8u, UINT(quads[i].top) + 8u);

      std::printf("      Blt source quadrants: %d,%d,%d  %d,%d,%d  %d,%d,%d  %d,%d,%d\n",
        q[0][0], q[0][1], q[0][2], q[1][0], q[1][1], q[1][2], q[2][0], q[2][1], q[2][2], q[3][0], q[3][1], q[3][2]);
      Check(similar(q[0], 255, 0, 0, 1) && similar(q[1], 0, 255, 0, 1) && similar(q[2], 0, 0, 255, 1)
         && similar(q[3], 188, 188, 188, 2), "Blt source: three colour quadrants and an sRGB-encoded grey one");

      BC250_DXVK_BLT blt = { };
      blt.Destination     = rt.Get();
      blt.DestinationRect = { 0, 0, LONG(W), LONG(H) };
      blt.Source          = src.Get();
      blt.Flags           = BC250_DXVK_BLT_STRETCH | BC250_DXVK_BLT_CONVERT | BC250_DXVK_BLT_PRESENT;
      blt.Rotation        = DXGI_MODE_ROTATION_IDENTITY;

      // Destination pixel (x, y) samples source (x / 2, y / 2); read the middle of each 32x32 quadrant
      if (CheckHr(engine->Blt(&blt), "Blt: 32x32 sRGB source stretched onto the 64x64 UNORM shell image, presenting")) {
        Check(same(readAt(staging.Get(), rt.Get(), 16u, 16u), q[0]) && same(readAt(staging.Get(), rt.Get(), 48u, 16u), q[1])
           && same(readAt(staging.Get(), rt.Get(), 16u, 48u), q[2]) && same(readAt(staging.Get(), rt.Get(), 48u, 48u), q[3]),
          "stretched Blt moves the encoded values unchanged, quadrant by quadrant");
      }

      blt.Flags    = BC250_DXVK_BLT_STRETCH | BC250_DXVK_BLT_CONVERT;
      blt.Rotation = DXGI_MODE_ROTATION_ROTATE180;

      if (CheckHr(engine->Blt(&blt), "Blt rotated by 180 degrees")) {
        Check(same(readAt(staging.Get(), rt.Get(), 16u, 16u), q[3]) && same(readAt(staging.Get(), rt.Get(), 48u, 16u), q[2])
           && same(readAt(staging.Get(), rt.Get(), 16u, 48u), q[1]) && same(readAt(staging.Get(), rt.Get(), 48u, 48u), q[0]),
          "180 degrees: each destination quadrant shows the opposite source quadrant");
      }

      const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
      ctx->ClearRenderTargetView(rtv.Get(), black);

      blt.DestinationRect = { 16, 16, 48, 48 };
      blt.Flags           = BC250_DXVK_BLT_CONVERT;
      blt.Rotation        = DXGI_MODE_ROTATION_IDENTITY;

      if (CheckHr(engine->Blt(&blt), "Blt into a 32x32 destination rectangle, unscaled")) {
        Check(same(readAt(staging.Get(), rt.Get(), 20u, 20u), q[0]) && same(readAt(staging.Get(), rt.Get(), 44u, 44u), q[3])
           && similar(readAt(staging.Get(), rt.Get(), 8u, 8u), 0, 0, 0, 0) && similar(readAt(staging.Get(), rt.Get(), 56u, 56u), 0, 0, 0, 0),
          "an unscaled Blt writes its destination rectangle and nothing else");
      }

      // Resolve: a 4x engine render target onto the shell image at the same size
      UINT msQuality = 0u;
      d3d->CheckMultisampleQualityLevels(DXGI_FORMAT_R8G8B8A8_UNORM, 4u, &msQuality);

      D3D11_TEXTURE2D_DESC msDesc = srcDesc;
      msDesc.Format     = DXGI_FORMAT_R8G8B8A8_UNORM;
      msDesc.SampleDesc = { 4u, 0u };
      msDesc.BindFlags  = D3D11_BIND_RENDER_TARGET;

      Microsoft::WRL::ComPtr<ID3D11Texture2D> ms;
      Microsoft::WRL::ComPtr<ID3D11RenderTargetView> msRtv;

      if (msQuality && CheckHr(d3d->CreateTexture2D(&msDesc, nullptr, &ms), "CreateTexture2D (4x Blt source)")
       && CheckHr(d3d->CreateRenderTargetView(ms.Get(), nullptr, &msRtv), "CreateRenderTargetView (4x Blt source)")) {
        const float msColor[4] = { 0.25f, 0.5f, 0.75f, 1.0f };
        ctx->ClearRenderTargetView(msRtv.Get(), msColor);

        blt.Source          = ms.Get();
        blt.DestinationRect = { 0, 0, 32, 32 };
        blt.Flags           = BC250_DXVK_BLT_RESOLVE;

        if (CheckHr(engine->Blt(&blt), "Blt resolving a 4x source")) {
          Check(similar(readAt(staging.Get(), rt.Get(), 10u, 10u), 64, 128, 191, 2)
             && similar(readAt(staging.Get(), rt.Get(), 50u, 50u), 0, 0, 0, 0),
            "resolving Blt writes the cleared colour into its rectangle");
        }
      } else {
        Check(false, "4x R8G8B8A8_UNORM render target for the resolving Blt");
      }

      // One format on both sides takes vkCmdBlitImage rather than a render pass; the second rotation
      // texture holds the blue storage
      if (rotReady) {
        blt.Source          = rotTex[1].Get();
        blt.DestinationRect = { 0, 0, 32, 32 };
        blt.Flags           = BC250_DXVK_BLT_STRETCH;

        if (CheckHr(engine->Blt(&blt), "Blt between two shell images of one format, scaled down")) {
          Check(similar(readAt(staging.Get(), rt.Get(), 10u, 10u), 0, 0, 255, 1)
             && similar(readAt(staging.Get(), rt.Get(), 50u, 50u), 0, 0, 0, 0),
            "same-format Blt fills its rectangle from the other shell image");
        }
      }

      BC250_DXVK_BLT bad = blt;
      bad.Source   = src.Get();
      bad.Flags    = 0u;
      bad.Rotation = DXGI_MODE_ROTATION_ROTATE90;
      Check(engine->Blt(&bad) == E_NOTIMPL, "Blt reports 90 degree rotation as not implemented");

      bad = blt;
      bad.Source          = src.Get();
      bad.DestinationRect = { 0, 0, LONG(W) + 1, LONG(H) };
      Check(engine->Blt(&bad) == E_INVALIDARG, "Blt rejects a rectangle outside the destination");

      bad = blt;
      bad.Source = srcStaging.Get();
      Check(engine->Blt(&bad) == E_INVALIDARG, "Blt rejects a staging source");

      bad = blt;
      bad.Source = rt.Get();
      Check(engine->Blt(&bad) == E_INVALIDARG, "Blt rejects the destination subresource as its own source");
    }

    CheckHr(engine->WaitForResourceIdle(rt.Get()), "WaitForResourceIdle after the Blt checks");
  }

  // ---- stream output of the vertex program, without a geometry program ----
  {
    UINT posReg = ~0u, colReg = ~0u;

    for (const auto& e : vsDdi.output)
      (e.SystemValue == 1u ? posReg : colReg) = e.Register;

    // Position xyzw, a one-component hole, then only the colour's y and z: the last entry starts at
    // component 1 of its register, and the hole must stay unwritten
    const BC250_DXVK_SO_ENTRY soEntries[] = {
      { 0u, 0u, posReg, 0xfu },
      { 0u, 0u, ~0u,    0x1u },
      { 0u, 0u, colReg, 0x6u },
    };

    constexpr UINT SoFloats = 7u;
    const UINT soStride = SoFloats * sizeof(float);
    const BC250_DXVK_STREAM_OUTPUT soDecl = { soEntries, 3u, &soStride, 1u, D3D11_SO_NO_RASTERIZED_STREAM };

    // What the runtime passes for stream output of the vertex stage: no code, the stage's output signature
    BC250_DXVK_SHADER_DESC noCodeDesc = { };
    noCodeDesc.Size         = sizeof(noCodeDesc);
    noCodeDesc.Input        = { vsDdi.output.data(), UINT(vsDdi.output.size()) };
    noCodeDesc.Output       = { vsDdi.output.data(), UINT(vsDdi.output.size()) };
    noCodeDesc.StreamOutput = &soDecl;

    BC250_DXVK_SHADER_DESC vsCodeDesc = MakeDesc(vsDdi);
    vsCodeDesc.StreamOutput = &soDecl;

    BC250_DXVK_SHADER_DESC psCodeDesc = MakeDesc(psDdi);
    psCodeDesc.StreamOutput = &soDecl;

    Microsoft::WRL::ComPtr<ID3D11GeometryShader> soNoCode, soVsCode, soPsCode;
    CheckHr(engine->CreateShader(&noCodeDesc, IID_PPV_ARGS(&soNoCode)), "CreateShader (stream output, no code)");
    CheckHr(engine->CreateShader(&vsCodeDesc, IID_PPV_ARGS(&soVsCode)), "CreateShader (stream output, VS code)");
    Check(engine->CreateShader(&psCodeDesc, IID_PPV_ARGS(&soPsCode)) == E_INVALIDARG,
      "CreateShader rejects stream output with pixel shader code");

    D3D11_BUFFER_DESC soBufDesc = { 3u * soStride, D3D11_USAGE_DEFAULT, D3D11_BIND_STREAM_OUTPUT };
    D3D11_BUFFER_DESC soReadDesc = { 3u * soStride, D3D11_USAGE_STAGING, 0u, D3D11_CPU_ACCESS_READ };
    Microsoft::WRL::ComPtr<ID3D11Buffer> soBuf, soRead;
    bool soReady = soNoCode.Get() && soVsCode.Get()
      && CheckHr(d3d->CreateBuffer(&soBufDesc, nullptr, &soBuf), "CreateBuffer (stream output target)")
      && CheckHr(d3d->CreateBuffer(&soReadDesc, nullptr, &soRead), "CreateBuffer (stream output readback)");

    constexpr float Hole = -7.0f;

    // The triangle's three vertices as the VS writes them: position (x, y, 0, 1), colour (r, g, b)
    auto expected = [&] (UINT v, UINT i) {
      const float* in = &vertices[5u * v];
      const float record[SoFloats] = { in[0], in[1], 0.0f, 1.0f, Hole, in[3], in[4] };
      return record[i];
    };

    // Draws the three vertices with the given topology into the zero-offset target and reads back all
    // three records. Returns how many leading records match the vertices; the rest must stay untouched.
    auto capture = [&] (ID3D11GeometryShader* gs, D3D11_PRIMITIVE_TOPOLOGY topology, const char* what) {
      std::array<float, 3u * SoFloats> data;
      data.fill(Hole);
      ctx->UpdateSubresource(soBuf.Get(), 0u, nullptr, data.data(), 0u, 0u);

      UINT offset = 0u;
      ID3D11Buffer* targets[] = { soBuf.Get() };
      ctx->IASetPrimitiveTopology(topology);
      ctx->SOSetTargets(1u, targets, &offset);
      ctx->GSSetShader(gs, nullptr, 0u);
      ctx->Draw(3u, 0u);
      ctx->GSSetShader(nullptr, nullptr, 0u);
      ctx->SOSetTargets(0u, nullptr, nullptr);
      ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      ctx->CopyResource(soRead.Get(), soBuf.Get());

      D3D11_MAPPED_SUBRESOURCE m = { };
      data.fill(0.0f);

      if (SUCCEEDED(ctx->Map(soRead.Get(), 0u, D3D11_MAP_READ, 0u, &m))) {
        std::memcpy(data.data(), m.pData, sizeof(data));
        ctx->Unmap(soRead.Get(), 0u);
      }

      UINT written = 0u;
      bool untouched = true;

      for (UINT v = 0u; v < 3u; v++) {
        const float* r = &data[v * SoFloats];
        bool match = true;

        for (UINT i = 0u; i < SoFloats; i++)
          match = match && r[i] == expected(v, i);

        if (match && written == v)
          written++;

        std::printf("      %s: record %u = %.1f %.1f %.1f %.1f [%.1f] %.1f %.1f\n", what, v,
          r[0], r[1], r[2], r[3], r[4], r[5], r[6]);
      }

      for (UINT i = written * SoFloats; i < data.size(); i++)
        untouched = untouched && data[i] == Hole;

      return untouched ? written : ~0u;
    };

    if (soReady) {
      Check(capture(soNoCode.Get(), D3D11_PRIMITIVE_TOPOLOGY_POINTLIST, "no code") == 3u,
        "stream output without code captures position, the hole and colour.yz of each point");
      Check(capture(soVsCode.Get(), D3D11_PRIMITIVE_TOPOLOGY_POINTLIST, "VS code") == 3u,
        "stream output with the VS code captures the same records");

      // Known limitation, upstream DXVK: the pass-through geometry shader emits one point per input
      // primitive, so a triangle list yields its first vertex only (D3D11 writes all three). DXVK's own
      // CreateGeometryShaderWithStreamOutput behaves the same. This check trips when that changes.
      Check(capture(soNoCode.Get(), D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST, "triangle list") == 1u,
        "known limitation: a triangle list without a geometry program streams out its first vertex only");
    }
  }

  if (bench)
    RunBench(engine, d3d.Get(), ctx.Get(), rt.Get(), rtv.Get(), layout.Get(), vb.Get(), stride, W);

  // ---- threads and submissions ----
  // A start module only ever proves a thread is the engine's: a std::thread starts in ucrtbase.dll, and
  // loaders and layers start their own. The Vulkan call census after teardown is the E2 criterion.
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
  Check(engineThreads == 0u, "no new thread starts in bc250dxvk.dll");

  // ---- teardown (E4, E5) ----
  rtv.Reset();
  layout.Reset();
  vs.Reset();
  ps.Reset();
  vb.Reset();
  staging.Reset();
  CheckHr(engine->WaitForResourceIdle(rt.Get()), "WaitForResourceIdle before releasing the imported image");
  rt.Reset();

  // Storage moved within the rotation set, so its images go only after all of its textures
  for (UINT i = 0u; i < RotCount; i++) {
    rotRtv[i].Reset();

    if (rotTex[i])
      engine->WaitForResourceIdle(rotTex[i].Get());

    rotTex[i].Reset();
  }

  ctx.Reset();
  d3d.Reset();

  ULONG remaining = engine->Release();
  Check(remaining == 0u, "final engine Release reports no leaked D3D11 references (E4)");

  // ---- Vulkan call census, including teardown (E2) ----
  uint32_t hookedCalls = 0u, foreignCalls = 0u;

  for (const auto& e : g_vkHooks) {
    uint32_t calls = e.stats->calls.load(), foreign = e.stats->foreign.load();

    if (calls)
      std::printf("      %-32s %6u calls, %u from other threads\n", e.name, calls, foreign);

    hookedCalls  += calls;
    foreignCalls += foreign;
  }

  std::printf("      %u hooked Vulkan calls, %u from other threads\n", hookedCalls, foreignCalls);

  uint32_t submits = HookStats("vkQueueSubmit").calls + HookStats("vkQueueSubmit2").calls
                   + HookStats("vkQueueSubmit2KHR").calls;

  Check(submits > 0u && HookStats("vkAllocateMemory").calls > 0u && HookStats("vkCreateGraphicsPipelines").calls > 0u,
    "the census sees the engine's submissions, allocations and pipelines (control for the hooks)");
  Check(foreignCalls == 0u, "every hooked Vulkan call of the engine ran on the calling thread (E2)");

  // ---- Log service ----
  uint32_t linesAtRelease = shell.logLines.load();
  std::printf("      %u log lines, %zu at level 1\n", linesAtRelease, shell.errors.size());
  Check(shell.foreignLogs.load() == 0u, "every Log call came from the calling thread (E2)");
  Check(shell.malformedLogs.load() == 0u, "every Log line has level 1-4 and no newline");
  Check(shell.HasError("90 or 270"), "the Blt ROTATE90 rejection arrives through Log, level 1");

  // A device-less call logs its instance lines again; the released device's sink must not see them
  BC250_DXVK_ADAPTER_INFO adapterInfoAfter = { sizeof(adapterInfoAfter) };
  CheckHr(funcs.GetAdapterInfo(&vkInstance, &adapterInfoAfter), "GetAdapterInfo after the final Release");
  Check(shell.logLines.load() == linesAtRelease, "no Log call after the final Release");

  // With a sink, nothing from the device's lifetime goes to the file; device-less lines do
  std::string logFile = EngineLogFile();

  if (!logFile.empty()) {
    std::string text = ReadFileText(logFile);
    Check(text.find("info:") != std::string::npos
       && text.find("Importing device") == std::string::npos
       && text.find("90 or 270") == std::string::npos,
      "the log file holds the device-less lines only");
    std::ofstream sinkFile(logFile.substr(0u, logFile.find_last_of('/')) + "/engine_sink.log");

    for (const auto& line : shell.lines)
      sinkFile << line << '\n';
  } else {
    std::printf("      DXVK_LOG_PATH not set: log file check skipped, engine_sink.log not written\n");
  }

  vk.destroyImage(device, image, nullptr);
  vk.freeMemory(device, memory, nullptr);

  for (UINT i = 0u; i < RotCount; i++) {
    if (rotImages[i])
      vk.destroyImage(device, rotImages[i], nullptr);

    if (rotMemory[i])
      vk.freeMemory(device, rotMemory[i], nullptr);
  }

  vk.destroyDevice(device, nullptr);
  funcs.FreeDeviceRequirements(&req);
  vk.destroyInstance(instance, nullptr);

  std::printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "PASSED", g_failures);
  return g_failures ? 1 : 0;
}
