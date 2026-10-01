// Offline positive control for amdgpu_wddm_dxvk.dll (engine ABI 1.4) on any Vulkan 1.3 GPU. No window; exits.
//
// Plays the UMD shell: owns the VkInstance and VkDevice (E1), creates the device from the engine's
// requirements, allocates the render target image itself (E5), and feeds shaders in DDI form: the token
// stream plus register-only signatures, with every semantic name thrown away. Checks the pixels of one
// clear and one draw, storage rotation, DXGI Blt onto an imported image, and that the engine's queue submissions, waits, allocations and
// object creation all ran on the calling thread (E2), by wrapping the Vulkan entry points it is given.
//
// Usage: amdgpu_wddm_dxvk_engine_test.exe <path to amdgpu_wddm_dxvk.dll> [adapter substring] [--icd <path>]
//        [--bench] [--bench-tiling] [--bench-shaders <dir> [--bench-pattern load|stream|loader]] [--keep-shader-cache]
// Exit code 0 = all checks passed. --bench adds a CPU-bound draw benchmark that prints timings (see RunBench).
// --bench-tiling adds a GPU benchmark of LINEAR against OPTIMAL render targets (see RunTilingBench).
// --bench-shaders creates and draws the *.dxbc programs of a directory and prints what shader creation costs
// the calling thread and the frames (see RunShaderBench).
// --icd loads that Vulkan driver DLL directly, as the UMD shell loads its bc250radv.dll, instead of going
// through the Vulkan loader (vulkan-1.dll). Either way the "icd:" lines name every loaded driver and its SHA-256.
// The engine's shader cache (r9) goes to DXVK_SHADER_CACHE_PATH; if that is not set, the test sets it to
// shader-cache beside itself and deletes the cache files there first (a cold start), unless --keep-shader-cache
// asks for a warm one. Run at the default log level: the r9 checks read the engine's info lines.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define VK_NO_PROTOTYPES
#include <windows.h>
#include <bcrypt.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <vulkan/vulkan.h>

#include "bc250_dxvk_engine.h"

#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace {

  int g_failures = 0;

  // The shader bench's loader pattern also calls the engine from a second test thread, as a game's loading
  // thread does, one thread inside the engine at a time as the runtime enforces without FREETHREADED. E2 then
  // means: Vulkan calls and shell services only on the test's two threads, never on an engine thread.
  thread_local bool t_secondCaller = false;

  bool IsCallerThread(DWORD caller) {
    return t_secondCaller || GetCurrentThreadId() == caller;
  }

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

    if (!IsCallerThread(s->mainThread))
      s->foreignCalls++;

    if (lock)
      EnterCriticalSection(&s->queueLock);
    else
      LeaveCriticalSection(&s->queueLock);
  }

  void APIENTRY Log(void* shell, UINT32 level, const char* message) {
    auto s = static_cast<Shell*>(shell);
    s->logLines++;

    if (!IsCallerThread(s->mainThread))
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

  // The engine's log file for this executable, as DXVK names it (Logger::getFileName, with the logger name of
  // ddi_engine.cpp): <DXVK_LOG_PATH>/<exe base name>_amdgpu_wddm_dxvk.log
  std::string EngineLogFile() {
    char dir[MAX_PATH] = { };
    char exe[MAX_PATH] = { };

    if (!GetEnvironmentVariableA("DXVK_LOG_PATH", dir, MAX_PATH) || !GetModuleFileNameA(nullptr, exe, MAX_PATH))
      return std::string();

    std::string name = exe;
    name = name.substr(name.find_last_of("\\/") + 1u);
    name = name.substr(0u, name.rfind('.'));
    return std::string(dir) + "/" + name + "_amdgpu_wddm_dxvk.log";
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

      if (!IsCallerThread(g_callerThread))
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
    BC250_VK_HOOK(vkDestroyPipeline),
    BC250_VK_HOOK(vkCreateDescriptorSetLayout),
    BC250_VK_HOOK(vkCreatePipelineLayout),
    BC250_VK_HOOK(vkGetShaderModuleCreateInfoIdentifierEXT),
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
    double   libraryMs = 0.0;  // wall time of the calls that created libraries, in the driver
    double   linkMs    = 0.0;  // the same for fast links
    double   optimizedMs = 0.0;
  };

  PipelineKinds g_pipelineKinds;
  thread_local PipelineKinds* t_pipelineKinds = &g_pipelineKinds;   // the loader thread counts its own
  PFN_vkCreateGraphicsPipelines g_hookedCreateGraphicsPipelines = nullptr;

  double QpcMs() {
    static const double freq = [] {
      LARGE_INTEGER f;
      QueryPerformanceFrequency(&f);
      return double(f.QuadPart);
    } ();

    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return 1e3 * double(t.QuadPart) / freq;
  }

  VkResult VKAPI_CALL ClassifyGraphicsPipelines(VkDevice device, VkPipelineCache cache, uint32_t count,
      const VkGraphicsPipelineCreateInfo* infos, const VkAllocationCallbacks* allocator, VkPipeline* pipelines) {
    PipelineKinds& kinds = *t_pipelineKinds;
    double* time = nullptr;

    for (uint32_t i = 0u; i < count; i++) {
      VkPipelineCreateFlags2 flags = infos[i].flags;
      bool linked = false;

      for (auto s = static_cast<const VkBaseInStructure*>(infos[i].pNext); s; s = s->pNext) {
        if (s->sType == VK_STRUCTURE_TYPE_PIPELINE_CREATE_FLAGS_2_CREATE_INFO)
          flags = reinterpret_cast<const VkPipelineCreateFlags2CreateInfo*>(s)->flags;
        else if (s->sType == VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR)
          linked = reinterpret_cast<const VkPipelineLibraryCreateInfoKHR*>(s)->libraryCount != 0u;
      }

      if (flags & VK_PIPELINE_CREATE_2_LIBRARY_BIT_KHR) {
        kinds.libraries++;
        time = time ? time : &kinds.libraryMs;
      } else if (linked && !(flags & VK_PIPELINE_CREATE_2_LINK_TIME_OPTIMIZATION_BIT_EXT)) {
        kinds.fastLinks++;
        time = time ? time : &kinds.linkMs;
      } else {
        kinds.optimized++;
        time = time ? time : &kinds.optimizedMs;
      }
    }

    // The engine makes these calls on a test thread inside an engine call (E2), each with its own sums
    double t0 = QpcMs();
    VkResult vr = g_hookedCreateGraphicsPipelines(device, cache, count, infos, allocator, pipelines);

    if (time)
      *time += QpcMs() - t0;

    return vr;
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

  // The name DXVK gives its threads (SetThreadDescription), as ASCII
  std::string ThreadName(DWORD tid) {
    HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid);
    std::string result;
    PWSTR name = nullptr;

    if (h && SUCCEEDED(GetThreadDescription(h, &name)) && name) {
      for (const wchar_t* c = name; *c; c++)
        result += *c < 0x80 ? char(*c) : '?';

      LocalFree(name);
    }

    if (h)
      CloseHandle(h);

    return result;
  }

  // Threads that started in the engine DLL since a census, by name
  std::vector<std::string> EngineThreadsSince(const std::set<DWORD>& before) {
    std::vector<std::string> result;

    for (DWORD tid : ProcessThreads()) {
      if (before.count(tid))
        continue;

      if (_stricmp(ModuleOf(ThreadStartAddress(tid)).c_str(), "amdgpu_wddm_dxvk.dll") == 0)
        result.push_back(ThreadName(tid));
    }

    return result;
  }

  // ---- engine statistics lines (r9) ----------------------------------------------------------------

  // Value of " key=" in the first line with the given prefix, or -1
  long long StatValue(const std::vector<std::string>& lines, const char* prefix, const char* key, size_t from = 0u) {
    std::string needle = std::string(" ") + key + "=";

    for (size_t i = from; i < lines.size(); i++) {
      const std::string& line = lines[i];

      if (line.find(prefix) == std::string::npos)
        continue;

      size_t pos = line.find(needle);

      if (pos == std::string::npos)
        return -1;

      return std::atoll(line.c_str() + pos + needle.size());
    }

    return -1;
  }

  bool HasLine(const std::vector<std::string>& lines, const char* text, size_t from = 0u) {
    for (size_t i = from; i < lines.size(); i++) {
      if (lines[i].find(text) != std::string::npos)
        return true;
    }

    return false;
  }

  // Whether the configuration turns translation on workers off (DXVK_CONFIG, as DXVK reads it)
  bool TranslationOnWorkersOff() {
    char config[1024] = { };

    if (!GetEnvironmentVariableA("DXVK_CONFIG", config, sizeof(config)))
      return false;

    std::string text = config;
    text.erase(std::remove(text.begin(), text.end(), ' '), text.end());
    std::transform(text.begin(), text.end(), text.begin(), [] (char c) { return char(std::tolower(c)); });
    return text.find("dxvk.translateshadersonworkers=false") != std::string::npos;
  }

  // ---- DDI-form shaders -------------------------------------------------------------------------

  // What the runtime gives a driver: the program tokens and signatures without names.
  struct DdiShader {
    Microsoft::WRL::ComPtr<ID3DBlob>        blob;
    const UINT*                             tokens = nullptr;
    std::vector<BC250_DXVK_SIGNATURE_ENTRY> input;
    std::vector<BC250_DXVK_SIGNATURE_ENTRY> output;
    std::vector<UINT>                       outputStreams;  // from reflection; the DDI entries carry 0
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
  // D3D10_SB_NAME, so the runtime passes UNDEFINED (targets) or no register (depth, coverage). Stream stays 0,
  // as in the D3D11_1DDIARG_SIGNATURE_ENTRY the shell receives.
  BC250_DXVK_SIGNATURE_ENTRY ToDdi(const D3D11_SIGNATURE_PARAMETER_DESC& p) {
    BC250_DXVK_SIGNATURE_ENTRY e = { };
    e.SystemValue   = p.SystemValueType < D3D_NAME_TARGET ? UINT(p.SystemValueType) : 0u;
    e.Register      = p.Register;
    e.Mask          = p.Mask;
    e.ComponentType = UINT(p.ComponentType);
    e.MinPrecision  = UINT(p.MinPrecision);
    return e;
  }

  // Tokens and signatures of the DXBC container in out->blob
  bool ReflectDdi(DdiShader* out) {
    out->tokens = FindProgramChunk(out->blob->GetBufferPointer(), out->blob->GetBufferSize());

    Microsoft::WRL::ComPtr<ID3D11ShaderReflection> reflection;
    HRESULT hr = D3DReflect(out->blob->GetBufferPointer(), out->blob->GetBufferSize(), IID_PPV_ARGS(&reflection));

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
      out->outputStreams.push_back(p.Stream);
    }

    return true;
  }

  bool CompileDdi(const char* source, const char* entry, const char* target, DdiShader* out) {
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    HRESULT hr = D3DCompile(source, std::strlen(source), entry, nullptr, nullptr, entry, target,
      D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &out->blob, &errors);

    if (FAILED(hr)) {
      std::printf("D3DCompile %s: %s\n", entry, errors ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
      return false;
    }

    return ReflectDdi(out);
  }

  // A compiled DXBC container from a file, as fxc writes it
  bool LoadDdi(const std::string& path, DdiShader* out) {
    std::string bytes = ReadFileText(path);

    if (bytes.size() < 32u || bytes.compare(0u, 4u, "DXBC") || FAILED(D3DCreateBlob(bytes.size(), &out->blob)))
      return false;

    std::memcpy(out->blob->GetBufferPointer(), bytes.data(), bytes.size());
    return ReflectDdi(out);
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

  // Two output streams. fxc allocates output registers per stream, so the value on stream 1 reuses o0.
  const char* g_streamsHlsl = R"(
    struct VsOut {
      float4 pos : SV_Position;
      float3 col : QUUX7;
    };

    struct Second {
      float4 value : BLARG2;
    };

    [maxvertexcount(2)]
    void gs(point VsOut i[1], inout PointStream<VsOut> first, inout PointStream<Second> second) {
      first.Append(i[0]);

      Second o;
      o.value = float4(i[0].col * 2.0f + 1.0f, i[0].pos.x + 5.0f);
      second.Append(o);
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

  // ---- shader creation benchmark (--bench-shaders) -----------------------------------------------------

  // fxc-compiled vs_5_0 and ps_5_0 programs (*.dxbc in a directory, in name order) created in DDI form and drawn,
  // as a game creates and first uses its shaders. Each pixel program is drawn with the vertex program created
  // last before it; every vertex program has the same outputs and every pixel program the same inputs, so any
  // pair links. Pattern "load" creates all programs, then draws each pair once, a few new pairs per frame (a
  // loading screen, then the first frames of a level). Pattern "stream" creates a few programs per frame and
  // draws a pair first two frames after both programs were created (assets streamed in during play), among
  // pairs drawn before. Pattern "loader" draws the same way while a second thread creates every program as
  // fast as it can (a game's loading thread, as Rise of the Tomb Raider streams its shaders); a lock lets one
  // thread at a time into the engine, per call, as the runtime does for a driver without FREETHREADED.
  // Prints the wall time of each CreateShader on the creating thread, the present-to-present times, the
  // end-to-end time, and the driver's pipeline compiles per thread; the engine's stats lines at its release
  // add the translations. Numbers, not checks: the development PC says nothing absolute about the BC-250.
  struct Spread {
    size_t n = 0u;
    double mean = 0.0, p50 = 0.0, p95 = 0.0, max = 0.0, total = 0.0;
  };

  Spread SpreadOf(std::vector<double> v) {
    Spread s;
    s.n = v.size();

    if (v.empty())
      return s;

    std::sort(v.begin(), v.end());

    for (double x : v)
      s.total += x;

    s.mean = s.total / double(v.size());
    s.p50  = v[v.size() / 2u];
    s.p95  = v[std::min(v.size() - 1u, (v.size() * 95u) / 100u)];
    s.max  = v.back();
    return s;
  }

  void PrintSpread(const char* what, const Spread& s) {
    std::printf("      %s ms: n=%zu mean=%.3f p50=%.3f p95=%.3f max=%.3f total=%.1f\n",
      what, s.n, s.mean, s.p50, s.p95, s.max, s.total);
  }

  // Vertex buffer format for an input of a vertex program, from its signature entry
  DXGI_FORMAT InputFormat(const BC250_DXVK_SIGNATURE_ENTRY& e) {
    static const DXGI_FORMAT formats[3][4] = {
      { DXGI_FORMAT_R32_FLOAT, DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32B32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT },
      { DXGI_FORMAT_R32_UINT,  DXGI_FORMAT_R32G32_UINT,  DXGI_FORMAT_R32G32B32_UINT,  DXGI_FORMAT_R32G32B32A32_UINT  },
      { DXGI_FORMAT_R32_SINT,  DXGI_FORMAT_R32G32_SINT,  DXGI_FORMAT_R32G32B32_SINT,  DXGI_FORMAT_R32G32B32A32_SINT  },
    };

    UINT components = (e.Mask & 8u) ? 4u : (e.Mask & 4u) ? 3u : (e.Mask & 2u) ? 2u : 1u;
    UINT type = e.ComponentType == D3D_REGISTER_COMPONENT_UINT32 ? 1u
              : e.ComponentType == D3D_REGISTER_COMPONENT_SINT32 ? 2u : 0u;
    return formats[type][components - 1u];
  }

  // One test thread inside the engine at a time, per call, as the runtime enforces for a driver without
  // FREETHREADED, with a critical section as the runtime's; it is recursive, so a call may nest inside a held
  // scope. Each thread sums the time it waited for the other.
  thread_local double t_ddiWaitMs = 0.0;

  struct DdiCall {
    CRITICAL_SECTION* cs;

    explicit DdiCall(CRITICAL_SECTION* c) : cs(c) {
      if (!TryEnterCriticalSection(cs)) {
        double t0 = QpcMs();
        EnterCriticalSection(cs);
        t_ddiWaitMs += QpcMs() - t0;
      }
    }

    ~DdiCall() {
      LeaveCriticalSection(cs);
    }

    DdiCall             (const DdiCall&) = delete;
    DdiCall& operator = (const DdiCall&) = delete;
  };

  void RunShaderBench(IBc250DxvkDevice* engine, ID3D11Device* d3d, ID3D11DeviceContext* ctx, const std::string& dir,
      const std::string& pattern) {
    using Microsoft::WRL::ComPtr;
    constexpr UINT Size = 64u, Latency = 3u, VertexStride = 256u;
    constexpr UINT NewPairsPerFrame = 8u;                                 // load, loader
    constexpr UINT CreatesPerFrame = 4u, Lag = 2u, OldPairsPerFrame = 16u;  // stream; Lag, old pairs: loader
    constexpr double LoaderGapMs = 0.2;                                   // loader

    struct Program {
      std::string               name;
      DdiShader                 ddi;
      bool                      vertex = false;
      UINT                      createFrame = 0u;
      ComPtr<ID3D11DeviceChild> object;
      ComPtr<ID3D11InputLayout> layout;
    };

    // ---- corpus ----
    std::vector<std::string> files;
    WIN32_FIND_DATAA found = { };
    HANDLE find = FindFirstFileA((dir + "\\*.dxbc").c_str(), &found);

    if (find != INVALID_HANDLE_VALUE) {
      do {
        files.push_back(found.cFileName);
      } while (FindNextFileA(find, &found));

      FindClose(find);
    }

    std::sort(files.begin(), files.end());

    std::vector<Program> programs;
    std::vector<std::pair<size_t, size_t>> pairs;   // vertex, pixel program
    size_t vertexCount = 0u, skipped = 0u, lastVertex = SIZE_MAX;
    uint64_t tokens = 0u;

    for (const auto& file : files) {
      Program p;
      p.name = file;

      if (!LoadDdi(dir + "\\" + file, &p.ddi)) {
        Check(false, ("shader bench: load " + file).c_str());
        return;
      }

      // D3D10_SB_TOKENIZED_PROGRAM_TYPE in the version token: 0 pixel, 1 vertex; the second token is the length
      UINT type = p.ddi.tokens[0] >> 16;

      if (type > 1u) {
        skipped++;
        continue;
      }

      p.vertex = type == 1u;
      tokens += p.ddi.tokens[1];

      if (p.vertex) {
        lastVertex = programs.size();
        vertexCount++;
      } else {
        pairs.emplace_back(lastVertex, programs.size());
      }

      programs.push_back(std::move(p));
    }

    // Pixel programs before the first vertex program take the first one
    size_t firstVertex = SIZE_MAX;

    for (size_t i = 0u; i < programs.size() && firstVertex == SIZE_MAX; i++) {
      if (programs[i].vertex)
        firstVertex = i;
    }

    for (auto& pair : pairs) {
      if (pair.first == SIZE_MAX)
        pair.first = firstVertex;
    }

    std::printf("      shader bench (%s): %s: %zu vertex, %zu pixel programs, %llu tokens, %zu other programs skipped\n",
      pattern.c_str(), dir.c_str(), vertexCount, pairs.size(), static_cast<unsigned long long>(tokens), skipped);

    if (!vertexCount || pairs.empty()) {
      Check(false, "shader bench: the directory has vertex and pixel programs");
      return;
    }

    // ---- what every program may use: t0-t7, s0-s1, b0-b3, one render target ----
    std::vector<uint8_t> zeros(65536u);
    D3D11_SUBRESOURCE_DATA zeroData = { zeros.data(), 4096u };

    D3D11_BUFFER_DESC cbDesc = { 65536u, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER };
    D3D11_BUFFER_DESC vbDesc = { 3u * VertexStride, D3D11_USAGE_IMMUTABLE, D3D11_BIND_VERTEX_BUFFER };
    ComPtr<ID3D11Buffer> cb, vb;

    D3D11_TEXTURE2D_DESC texDesc = { 4u, 4u, 1u, 1u, DXGI_FORMAT_R8G8B8A8_UNORM, { 1u, 0u },
      D3D11_USAGE_IMMUTABLE, D3D11_BIND_SHADER_RESOURCE };
    D3D11_SUBRESOURCE_DATA texData = { zeros.data(), 16u };
    ComPtr<ID3D11Texture2D> tex;
    ComPtr<ID3D11ShaderResourceView> srv;

    D3D11_TEXTURE2D_DESC rtDesc = { Size, Size, 1u, 1u, DXGI_FORMAT_R8G8B8A8_UNORM, { 1u, 0u },
      D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET };
    ComPtr<ID3D11Texture2D> rt;
    ComPtr<ID3D11RenderTargetView> rtv;

    D3D11_SAMPLER_DESC sd = { };
    sd.Filter         = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU       = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.AddressV       = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.AddressW       = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD         = D3D11_FLOAT32_MAX;
    ComPtr<ID3D11SamplerState> sampler;

    ComPtr<ID3D11Query> events[Latency];
    D3D11_QUERY_DESC qd = { D3D11_QUERY_EVENT, 0u };
    bool ready = SUCCEEDED(d3d->CreateBuffer(&cbDesc, &zeroData, &cb))
              && SUCCEEDED(d3d->CreateBuffer(&vbDesc, &zeroData, &vb))
              && SUCCEEDED(d3d->CreateTexture2D(&texDesc, &texData, &tex))
              && SUCCEEDED(d3d->CreateShaderResourceView(tex.Get(), nullptr, &srv))
              && SUCCEEDED(d3d->CreateTexture2D(&rtDesc, nullptr, &rt))
              && SUCCEEDED(d3d->CreateRenderTargetView(rt.Get(), nullptr, &rtv))
              && SUCCEEDED(d3d->CreateSamplerState(&sd, &sampler));

    for (auto& e : events)
      ready = ready && SUCCEEDED(d3d->CreateQuery(&qd, &e));

    if (!ready) {
      Check(false, "shader bench: resources");
      return;
    }

    ctx->ClearState();

    UINT offset = 0u, stride = VertexStride;
    D3D11_VIEWPORT viewport = { 0.0f, 0.0f, float(Size), float(Size), 0.0f, 1.0f };
    ID3D11Buffer* vbs[] = { vb.Get() };
    ID3D11Buffer* cbs[4] = { cb.Get(), cb.Get(), cb.Get(), cb.Get() };
    ID3D11ShaderResourceView* srvs[8] = { };
    ID3D11SamplerState* samplers[2] = { sampler.Get(), sampler.Get() };

    for (auto& s : srvs)
      s = srv.Get();

    ctx->IASetVertexBuffers(0u, 1u, vbs, &stride, &offset);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->RSSetViewports(1u, &viewport);
    ctx->VSSetConstantBuffers(0u, 4u, cbs);
    ctx->PSSetConstantBuffers(0u, 4u, cbs);
    ctx->VSSetShaderResources(0u, 8u, srvs);
    ctx->PSSetShaderResources(0u, 8u, srvs);
    ctx->VSSetSamplers(0u, 2u, samplers);
    ctx->PSSetSamplers(0u, 2u, samplers);
    ctx->OMSetRenderTargets(1u, rtv.GetAddressOf(), nullptr);

    // ---- one test thread inside the engine at a time, per call (see DdiCall) ----
    struct DdiLock {
      CRITICAL_SECTION cs;
      DdiLock()  { InitializeCriticalSection(&cs); }
      ~DdiLock() { DeleteCriticalSection(&cs); }
    } ddiLock;

    auto locked = [&] (auto fn) {
      DdiCall call(&ddiLock.cs);
      return fn();
    };

    // ---- creation, timed on the creating thread: the call itself, not the wait for the lock ----
    std::vector<double> vertexMs, pixelMs;

    auto create = [&] (Program& p) -> bool {
      BC250_DXVK_SHADER_DESC desc = MakeDesc(p.ddi);
      void* object = nullptr;
      HRESULT hr;
      double ms;

      { DdiCall call(&ddiLock.cs);
        double t0 = QpcMs();
        hr = engine->CreateShader(&desc,
          p.vertex ? __uuidof(ID3D11VertexShader) : __uuidof(ID3D11PixelShader), &object);
        ms = QpcMs() - t0;
      }

      if (FAILED(hr)) {
        std::printf("FAIL  shader bench: CreateShader %s: hr=0x%08lX\n", p.name.c_str(), static_cast<unsigned long>(hr));
        g_failures++;
        return false;
      }

      // Both stage interfaces derive from ID3D11DeviceChild alone
      p.object.Attach(static_cast<ID3D11DeviceChild*>(object));
      (p.vertex ? vertexMs : pixelMs).push_back(ms);

      if (!p.vertex)
        return true;

      // The program's own inputs, packed; the zeroed vertices fit any of them
      std::vector<BC250_DXVK_VERTEX_ATTRIBUTE> attrs;
      UINT attrOffset = 0u;
      DdiCall call(&ddiLock.cs);

      for (const auto& e : p.ddi.input) {
        if (e.SystemValue)   // vertex and instance IDs come from the input assembler
          continue;

        VkFormat format = VK_FORMAT_UNDEFINED;
        UINT size = 0u;

        if (FAILED(engine->GetVertexFormat(InputFormat(e), &format, &size)))
          return false;

        attrs.push_back({ e.Register, 0u, format, attrOffset });
        attrOffset += size;
      }

      BC250_DXVK_VERTEX_BINDING binding = { 0u, VertexStride, VK_VERTEX_INPUT_RATE_VERTEX, 0u };
      BC250_DXVK_INPUT_LAYOUT layoutDesc = { UINT(attrs.size()), attrs.data(), 1u, &binding };
      hr = engine->CreateInputLayout(&layoutDesc, &p.layout);

      if (FAILED(hr)) {
        std::printf("FAIL  shader bench: input layout of %s: hr=0x%08lX\n", p.name.c_str(), static_cast<unsigned long>(hr));
        g_failures++;
        return false;
      }

      return true;
    };

    // ---- frames, with a frame latency of 3 as DXGI's default ----
    std::vector<double> presentMs;
    double lastPresent = 0.0;
    UINT frameIndex = 0u;   // written under the lock: the loading thread reads it
    const float clear[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

    auto beginFrame = [&] () -> HRESULT {
      if (frameIndex >= Latency) {
        ID3D11Query* event = events[frameIndex % Latency].Get();
        ULONGLONG deadline = GetTickCount64() + 60000u;   // GPU or lock waits
        BOOL done = FALSE;
        HRESULT hr;

        while ((hr = locked([&] { return ctx->GetData(event, &done, sizeof(done), 0u); })) == S_FALSE) {
          if (GetTickCount64() > deadline)
            return E_FAIL;

          YieldProcessor();
        }

        if (FAILED(hr))
          return hr;
      }

      locked([&] { ctx->ClearRenderTargetView(rtv.Get(), clear); });
      return S_OK;
    };

    auto draw = [&] (const std::pair<size_t, size_t>& pair) {
      Program& v = programs[pair.first];
      locked([&] { ctx->IASetInputLayout(v.layout.Get()); });
      locked([&] { ctx->VSSetShader(static_cast<ID3D11VertexShader*>(v.object.Get()), nullptr, 0u); });
      locked([&] { ctx->PSSetShader(static_cast<ID3D11PixelShader*>(programs[pair.second].object.Get()), nullptr, 0u); });
      locked([&] { ctx->Draw(3u, 0u); });
    };

    auto endFrame = [&] () -> HRESULT {
      HRESULT hr = locked([&] {
        ctx->End(events[frameIndex % Latency].Get());
        HRESULT result = engine->SubmitForPresent(rt.Get(), 0u);
        frameIndex++;
        return result;
      });

      double now = QpcMs();
      presentMs.push_back(now - lastPresent);
      lastPresent = now;
      return hr;
    };

    // Stream and loader: a pair is drawn first once both of its programs exist and are Lag frames old, up to
    // newLimit new pairs a frame; then the steady part of the frame, pairs drawn before
    std::vector<bool> drawn(pairs.size(), false);
    size_t drawnCount = 0u, oldPair = 0u;

    auto drawPairs = [&] (size_t created, size_t newLimit) {
      for (size_t j = 0u; j < pairs.size() && newLimit; j++) {
        const auto& pair = pairs[j];

        if (drawn[j] || pair.first >= created || pair.second >= created
         || frameIndex < programs[pair.first].createFrame + Lag
         || frameIndex < programs[pair.second].createFrame + Lag)
          continue;

        draw(pair);
        drawn[j] = true;
        drawnCount++;
        newLimit--;
      }

      for (UINT k = 0u; k < OldPairsPerFrame && drawnCount; k++) {
        while (!drawn[oldPair % pairs.size()])
          oldPair++;

        draw(pairs[oldPair++ % pairs.size()]);
      }
    };

    const PipelineKinds kindsBefore = g_pipelineKinds;
    PipelineKinds loaderKinds;
    double tStart = QpcMs(), cpuStart = ProcessCpuMs(), tCreated = 0.0;
    double drawWaitBefore = t_ddiWaitMs, loaderWaitMs = 0.0;
    HRESULT hr = S_OK;

    if (pattern == "load") {
      for (auto& p : programs) {
        if (!create(p))
          return;
      }

      tCreated = QpcMs();
      lastPresent = tCreated;

      for (size_t first = 0u; first < pairs.size() && SUCCEEDED(hr); first += NewPairsPerFrame) {
        hr = beginFrame();

        for (size_t j = first; j < std::min(pairs.size(), first + NewPairsPerFrame) && SUCCEEDED(hr); j++)
          draw(pairs[j]);

        if (SUCCEEDED(hr))
          hr = endFrame();
      }
    } else if (pattern == "stream") {
      size_t created = 0u;
      lastPresent = tStart;

      while (drawnCount < pairs.size() && SUCCEEDED(hr)) {
        for (UINT c = 0u; c < CreatesPerFrame && created < programs.size(); c++, created++) {
          programs[created].createFrame = frameIndex;

          if (!create(programs[created]))
            return;
        }

        if (created == programs.size() && tCreated == 0.0)
          tCreated = QpcMs();

        hr = beginFrame();

        if (SUCCEEDED(hr)) {
          drawPairs(created, SIZE_MAX);
          hr = endFrame();
        }
      }
    } else {
      // Under the lock: programs[0, created) exist (their createFrame and objects are set), or the loader failed
      size_t created = 0u;
      bool loaderFailed = false;
      lastPresent = tStart;

      std::thread loader([&] {
        t_secondCaller = true;
        t_pipelineKinds = &loaderKinds;

        for (auto& p : programs) {
          bool ok;

          { DdiCall call(&ddiLock.cs);
            p.createFrame = frameIndex;
            ok = create(p);
            created += ok ? 1u : 0u;
            loaderFailed = !ok;
          }

          if (!ok)
            break;

          // The thread's own work per program, outside the engine (reading, decompressing), which also lets
          // the drawing thread in: a critical section is not fair to a waiter woken while its owner re-enters
          for (double until = QpcMs() + LoaderGapMs; QpcMs() < until; )
            YieldProcessor();
        }

        tCreated = QpcMs();
        loaderWaitMs = t_ddiWaitMs;
      });

      while (SUCCEEDED(hr)) {
        size_t available = 0u;
        bool failed = false;

        locked([&] {
          available = created;
          failed = loaderFailed;
        });

        if (failed || drawnCount == pairs.size())
          break;

        hr = beginFrame();

        if (SUCCEEDED(hr)) {
          drawPairs(available, NewPairsPerFrame);
          hr = endFrame();
        }
      }

      loader.join();

      if (loaderFailed)
        return;
    }

    double tFrames = QpcMs();

    if (SUCCEEDED(hr))
      hr = engine->WaitForResourceIdle(rt.Get());

    double tEnd = QpcMs(), cpuEnd = ProcessCpuMs();

    if (!CheckHr(hr, "shader bench: frames"))
      return;

    PrintSpread("CreateShader vertex", SpreadOf(vertexMs));
    PrintSpread("CreateShader pixel", SpreadOf(pixelMs));
    PrintSpread("present to present", SpreadOf(presentMs));

    std::printf("      end to end %.1f ms (programs created at %.1f ms, last frame submitted at %.1f ms), %u frames, "
      "process CPU %.1f ms\n", tEnd - tStart, tCreated - tStart, tFrames - tStart, frameIndex, cpuEnd - cpuStart);

    auto printKinds = [] (const char* thread, const PipelineKinds& now, const PipelineKinds& before) {
      std::printf("      driver pipeline creation on the %s thread: %u libraries %.1f ms, %u fast links %.1f ms, "
        "%u optimized %.1f ms\n", thread,
        now.libraries - before.libraries, now.libraryMs - before.libraryMs,
        now.fastLinks - before.fastLinks, now.linkMs - before.linkMs,
        now.optimized - before.optimized, now.optimizedMs - before.optimizedMs);
    };

    if (pattern == "loader") {
      printKinds("drawing", g_pipelineKinds, kindsBefore);
      printKinds("loading", loaderKinds, PipelineKinds());
      std::printf("      waits for the other thread to leave the engine: drawing thread %.1f ms, loading thread %.1f ms\n",
        t_ddiWaitMs - drawWaitBefore, loaderWaitMs);
    } else {
      printKinds("calling", g_pipelineKinds, kindsBefore);
    }
  }

  // ---- Vulkan -------------------------------------------------------------------------------------

  struct Vk {
    PFN_vkGetInstanceProcAddr                   getInstanceProcAddr = nullptr;
    PFN_vkCreateInstance                        createInstance = nullptr;
    PFN_vkDestroyInstance                       destroyInstance = nullptr;
    PFN_vkEnumeratePhysicalDevices              enumeratePhysicalDevices = nullptr;
    PFN_vkGetPhysicalDeviceProperties           getPhysicalDeviceProperties = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties     getPhysicalDeviceMemoryProperties = nullptr;
    PFN_vkGetPhysicalDeviceFormatProperties     getPhysicalDeviceFormatProperties = nullptr;
    PFN_vkGetPhysicalDeviceImageFormatProperties getPhysicalDeviceImageFormatProperties = nullptr;
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

  // DXVK's IDXGIVkInteropSurface (src/dxgi/dxgi_interfaces.h), declared here to keep the test on the ABI header
  // and the SDK: it shows the tiling the engine uses for a wrapped image.
  MIDL_INTERFACE("5546cf8c-77e7-4341-b05d-8d4d5000e77d")
  DxvkInteropSurface : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetDevice(IUnknown** ppDevice) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetVulkanImageInfo(VkImage* pHandle, VkImageLayout* pLayout,
                                                         VkImageCreateInfo* pInfo) = 0;
  };

  template<typename T>
  void LoadInstance(Vk& vk, VkInstance instance, T* fn, const char* name) {
    *fn = reinterpret_cast<T>(vk.getInstanceProcAddr(instance, name));
  }

  // ---- GPU cost of LINEAR runtime surfaces (--bench-tiling) ------------------------------------------

  // The shell allocates runtime surfaces LINEAR. Per-application DXVK renders into its own OPTIMAL back buffer
  // and copies it to the presentable image once per frame. On the GPU it runs on, this prints the costs that
  // decide between drawing into a LINEAR runtime image and drawing into an OPTIMAL image copied at present:
  // blended full-screen draws into each tiling, the copy, and a 1:1 read of each tiling as composition does.
  // Prints numbers; not a pass/fail check.
  const char* g_tilingHlsl = R"(
    Texture2D src : register(t0);

    float4 vs(uint id : SV_VertexID) : SV_Position {
      float2 uv = float2((id << 1) & 2, id & 2);
      return float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
    }

    float4 fill(float4 pos : SV_Position) : SV_Target {
      return float4(frac(pos.xy / 256.0f), 0.5f, 0.25f);
    }

    float4 load(float4 pos : SV_Position) : SV_Target {
      return src.Load(int3(pos.xy, 0));
    }
  )";

  void RunTilingBench(Vk& vk, VkPhysicalDevice physDev, VkDevice device,
      const VkPhysicalDeviceMemoryProperties& memProps, IBc250DxvkDevice2* engine, ID3D11Device* d3d,
      ID3D11DeviceContext* ctx) {
    using Microsoft::WRL::ComPtr;
    constexpr UINT Width = 1920u, Height = 1080u, DrawsPerFrame = 8u, Frames = 100u;

    struct Target {
      VkImage                           image  = VK_NULL_HANDLE;
      VkDeviceMemory                    memory = VK_NULL_HANDLE;
      ComPtr<ID3D11Texture2D>           texture;
      ComPtr<ID3D11RenderTargetView>    rtv;
      ComPtr<ID3D11ShaderResourceView>  srv;
    };

    D3D11_TEXTURE2D_DESC1 desc = { };
    desc.Width      = Width;
    desc.Height     = Height;
    desc.MipLevels  = 1u;
    desc.ArraySize  = 1u;
    desc.Format     = DXGI_FORMAT_R8G8B8A8_TYPELESS;
    desc.SampleDesc = { 1u, 0u };
    desc.Usage      = D3D11_USAGE_DEFAULT;
    desc.BindFlags  = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = { DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_RTV_DIMENSION_TEXTURE2D };
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = { DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_SRV_DIMENSION_TEXTURE2D };
    srvDesc.Texture2D.MipLevels = 1u;

    // [0] OPTIMAL, [1] LINEAR, both shell images as a runtime surface would be; [2] an engine texture to read into
    const VkImageTiling tilings[2] = { VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_TILING_LINEAR };
    Target targets[3];
    bool ready = true;

    for (UINT i = 0u; i < 2u && ready; i++) {
      Target& t = targets[i];
      VkImageCreateInfo info = { };
      ready = SUCCEEDED(engine->GetImageCreateInfo(&desc, &info));
      info.tiling = tilings[i];

      VkMemoryRequirements req = { };
      ready = ready && vk.createImage(device, &info, nullptr, &t.image) == VK_SUCCESS;

      if (ready) {
        vk.getImageMemoryRequirements(device, t.image, &req);
        uint32_t type = ~0u;

        for (uint32_t m = 0u; m < memProps.memoryTypeCount && type == ~0u; m++) {
          if ((req.memoryTypeBits & (1u << m)) && (memProps.memoryTypes[m].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            type = m;
        }

        VkMemoryAllocateInfo alloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        alloc.allocationSize  = req.size;
        alloc.memoryTypeIndex = type;

        ready = type != ~0u
          && vk.allocateMemory(device, &alloc, nullptr, &t.memory) == VK_SUCCESS
          && vk.bindImageMemory(device, t.image, t.memory, 0u) == VK_SUCCESS
          && SUCCEEDED(engine->CreateTexture2DFromImage2(&desc, &info, t.image, &t.texture));
      }
    }

    D3D11_TEXTURE2D_DESC sinkDesc = { Width, Height, 1u, 1u, DXGI_FORMAT_R8G8B8A8_UNORM, { 1u, 0u },
      D3D11_USAGE_DEFAULT, D3D11_BIND_RENDER_TARGET };
    ready = ready && SUCCEEDED(d3d->CreateTexture2D(&sinkDesc, nullptr, &targets[2].texture));

    for (UINT i = 0u; i < 3u && ready; i++) {
      ready = SUCCEEDED(d3d->CreateRenderTargetView(targets[i].texture.Get(), &rtvDesc, &targets[i].rtv))
        && (i == 2u || SUCCEEDED(d3d->CreateShaderResourceView(targets[i].texture.Get(), &srvDesc, &targets[i].srv)));
    }

    DdiShader vsDdi, fillDdi, loadDdi;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> fillPs, loadPs;
    ready = ready && CompileDdi(g_tilingHlsl, "vs", "vs_5_0", &vsDdi) && CompileDdi(g_tilingHlsl, "fill", "ps_5_0", &fillDdi)
      && CompileDdi(g_tilingHlsl, "load", "ps_5_0", &loadDdi);

    if (ready) {
      BC250_DXVK_SHADER_DESC vsDesc = MakeDesc(vsDdi), fillDesc = MakeDesc(fillDdi), loadDesc = MakeDesc(loadDdi);
      ready = SUCCEEDED(engine->CreateShader(&vsDesc, IID_PPV_ARGS(&vs)))
        && SUCCEEDED(engine->CreateShader(&fillDesc, IID_PPV_ARGS(&fillPs)))
        && SUCCEEDED(engine->CreateShader(&loadDesc, IID_PPV_ARGS(&loadPs)));
    }

    D3D11_BLEND_DESC blendDesc = { };
    blendDesc.RenderTarget[0].BlendEnable           = TRUE;
    blendDesc.RenderTarget[0].SrcBlend              = D3D11_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].DestBlend             = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp               = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha         = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha        = D3D11_BLEND_ZERO;
    blendDesc.RenderTarget[0].BlendOpAlpha          = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ComPtr<ID3D11BlendState> blend;

    D3D11_QUERY_DESC disjointDesc = { D3D11_QUERY_TIMESTAMP_DISJOINT, 0u }, stampDesc = { D3D11_QUERY_TIMESTAMP, 0u };
    ComPtr<ID3D11Query> disjoint, stamp0, stamp1;

    ready = ready && SUCCEEDED(d3d->CreateBlendState(&blendDesc, &blend))
      && SUCCEEDED(d3d->CreateQuery(&disjointDesc, &disjoint))
      && SUCCEEDED(d3d->CreateQuery(&stampDesc, &stamp0)) && SUCCEEDED(d3d->CreateQuery(&stampDesc, &stamp1));

    Check(ready, "tiling bench: 1920x1080 OPTIMAL and LINEAR shell images, shaders, queries");

    if (ready) {
      ctx->ClearState();

      D3D11_VIEWPORT viewport = { 0.0f, 0.0f, float(Width), float(Height), 0.0f, 1.0f };
      ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
      ctx->VSSetShader(vs.Get(), nullptr, 0u);
      ctx->RSSetViewports(1u, &viewport);

      // GPU milliseconds per frame of work(), which records Frames frames; -1 if the timestamps are unusable
      auto gpuMs = [&] (auto&& work) {
        ctx->Begin(disjoint.Get());
        ctx->End(stamp0.Get());
        work();
        ctx->End(stamp1.Get());
        ctx->End(disjoint.Get());
        ctx->Flush();

        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj = { };
        UINT64 t0 = 0u, t1 = 0u;
        ULONGLONG deadline = GetTickCount64() + 20000u;

        while (ctx->GetData(disjoint.Get(), &dj, sizeof(dj), 0u) == S_FALSE) {
          if (GetTickCount64() > deadline)
            return -1.0;

          YieldProcessor();
        }

        if (ctx->GetData(stamp0.Get(), &t0, sizeof(t0), 0u) != S_OK || ctx->GetData(stamp1.Get(), &t1, sizeof(t1), 0u) != S_OK
         || dj.Disjoint || !dj.Frequency)
          return -1.0;

        return 1e3 * double(t1 - t0) / double(dj.Frequency) / double(Frames);
      };

      const float grey[4] = { 0.5f, 0.5f, 0.5f, 1.0f };

      auto fill = [&] (UINT i) {
        return gpuMs([&] {
          ID3D11RenderTargetView* rtv = targets[i].rtv.Get();
          ctx->OMSetRenderTargets(1u, &rtv, nullptr);
          ctx->OMSetBlendState(blend.Get(), nullptr, ~0u);
          ctx->PSSetShader(fillPs.Get(), nullptr, 0u);

          for (UINT f = 0u; f < Frames; f++) {
            ctx->ClearRenderTargetView(rtv, grey);

            for (UINT d = 0u; d < DrawsPerFrame; d++)
              ctx->Draw(3u, 0u);
          }

          ctx->OMSetRenderTargets(0u, nullptr, nullptr);
        });
      };

      auto copy = [&] {
        return gpuMs([&] {
          for (UINT f = 0u; f < Frames; f++)
            ctx->CopyResource(targets[1].texture.Get(), targets[0].texture.Get());
        });
      };

      auto read = [&] (UINT i) {
        return gpuMs([&] {
          ID3D11RenderTargetView* rtv = targets[2].rtv.Get();
          ID3D11ShaderResourceView* srv = targets[i].srv.Get();
          ctx->OMSetRenderTargets(1u, &rtv, nullptr);
          ctx->OMSetBlendState(nullptr, nullptr, ~0u);
          ctx->PSSetShader(loadPs.Get(), nullptr, 0u);
          ctx->PSSetShaderResources(0u, 1u, &srv);

          for (UINT f = 0u; f < Frames; f++)
            ctx->Draw(3u, 0u);

          srv = nullptr;
          ctx->PSSetShaderResources(0u, 1u, &srv);
          ctx->OMSetRenderTargets(0u, nullptr, nullptr);
        });
      };

      // One unmeasured round compiles the pipelines; SubmitForPresent then builds the optimized variants
      fill(0u); fill(1u); copy(); read(0u); read(1u);
      engine->SubmitForPresent(targets[2].texture.Get(), 0u);

      double fillOptimal = fill(0u), fillLinear = fill(1u), copyMs = copy(), readOptimal = read(0u), readLinear = read(1u);

      std::printf("      tiling bench %ux%u RGBA8, GPU ms per frame, %u frames each:\n", Width, Height, Frames);
      std::printf("      clear + %u blended full-screen draws: OPTIMAL %.3f, LINEAR %.3f\n", DrawsPerFrame, fillOptimal, fillLinear);
      std::printf("      copy OPTIMAL -> LINEAR (present from an OPTIMAL shadow): %.3f\n", copyMs);
      std::printf("      1:1 read into an OPTIMAL target (composition): from OPTIMAL %.3f, from LINEAR %.3f\n",
        readOptimal, readLinear);
      Check(fillOptimal > 0.0 && fillLinear > 0.0 && copyMs > 0.0 && readOptimal > 0.0 && readLinear > 0.0,
        "tiling bench: every timestamp pair usable");

      ctx->ClearState();
    }

    for (auto& t : targets) {
      t.rtv.Reset();
      t.srv.Reset();

      if (t.texture)
        engine->WaitForResourceIdle(t.texture.Get());

      t.texture.Reset();

      if (t.image)
        vk.destroyImage(device, t.image, nullptr);

      if (t.memory)
        vk.freeMemory(device, t.memory, nullptr);
    }
  }

  // SHA-256 of a file in upper-case hex, as Get-FileHash prints it; empty on any failure.
  std::string FileSha256(const wchar_t* path) {
    HANDLE file = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);

    if (file == INVALID_HANDLE_VALUE)
      return std::string();

    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    UCHAR digest[32] = { };
    bool ok = BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0))
           && BCRYPT_SUCCESS(BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0));
    std::vector<UCHAR> buffer(1u << 20);

    while (ok) {
      DWORD read = 0;

      if (!ReadFile(file, buffer.data(), DWORD(buffer.size()), &read, nullptr))
        ok = false;
      else if (!read)
        break;
      else
        ok = BCRYPT_SUCCESS(BCryptHashData(hash, buffer.data(), read, 0));
    }

    ok = ok && BCRYPT_SUCCESS(BCryptFinishHash(hash, digest, sizeof(digest), 0));

    if (hash)
      BCryptDestroyHash(hash);
    if (alg)
      BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(file);

    std::string hex;

    for (UCHAR b : digest) {
      static const char digits[] = "0123456789ABCDEF";
      hex += digits[b >> 4];
      hex += digits[b & 15];
    }

    return ok ? hex : std::string();
  }

  // Which Vulkan driver answers: every loaded module that exports the ICD entry point, with the SHA-256 of its
  // file (a mapped image cannot change on disk while it is loaded). An environment setting alone does not say:
  // the Vulkan loader ignores VK_DRIVER_FILES and VK_ICD_FILENAMES in an elevated process (loader_secure_getenv).
  void PrintIcdModules() {
    std::vector<HMODULE> modules(1024);
    DWORD needed = 0;

    if (!EnumProcessModules(GetCurrentProcess(), modules.data(), DWORD(modules.size() * sizeof(HMODULE)), &needed)) {
      std::printf("icd: module list unavailable (%lu)\n", GetLastError());
      return;
    }

    modules.resize(std::min<size_t>(modules.size(), needed / sizeof(HMODULE)));

    for (HMODULE m : modules) {
      if (!GetProcAddress(m, "vk_icdGetInstanceProcAddr"))
        continue;

      std::wstring path(32768, L'\0');
      path.resize(GetModuleFileNameW(m, path.data(), DWORD(path.size())));
      std::printf("icd: %ls sha256 %s\n", path.c_str(), FileSha256(path.c_str()).c_str());
    }
  }

}

int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: amdgpu_wddm_dxvk_engine_test <amdgpu_wddm_dxvk.dll> [adapter substring] [--icd <path>]"
                " [--bench] [--bench-tiling] [--bench-shaders <dir> [--bench-pattern load|stream|loader]]"
                " [--keep-shader-cache]\n");
    return 2;
  }

  std::setvbuf(stdout, nullptr, _IONBF, 0);
  const char* adapterFilter = nullptr;
  const char* icdPath = nullptr;
  const char* benchShaders = nullptr;
  std::string benchPattern = "load";
  bool bench = false, benchTiling = false, keepShaderCache = false;

  for (int i = 2; i < argc; i++) {
    if (!std::strcmp(argv[i], "--bench"))
      bench = true;
    else if (!std::strcmp(argv[i], "--bench-tiling"))
      benchTiling = true;
    else if (!std::strcmp(argv[i], "--keep-shader-cache"))
      keepShaderCache = true;
    else if ((!std::strcmp(argv[i], "--icd") || !std::strcmp(argv[i], "--bench-shaders")
           || !std::strcmp(argv[i], "--bench-pattern")) && i + 1 >= argc) {
      std::printf("FAIL  %s needs a value\n", argv[i]);
      return 2;
    } else if (!std::strcmp(argv[i], "--icd"))
      icdPath = argv[++i];
    else if (!std::strcmp(argv[i], "--bench-shaders"))
      benchShaders = argv[++i];
    else if (!std::strcmp(argv[i], "--bench-pattern"))
      benchPattern = argv[++i];
    else
      adapterFilter = argv[i];
  }

  if (benchPattern != "load" && benchPattern != "stream" && benchPattern != "loader") {
    std::printf("FAIL  --bench-pattern is load or stream\n");
    return 2;
  }

  // The engine's shader cache (r9) is per user and per executable. Before the engine reads the variable (once
  // per process), keep it beside the test unless DXVK_SHADER_CACHE_PATH names a place, and start cold there.
  {
    char path[MAX_PATH] = { };

    if (!GetEnvironmentVariableA("DXVK_SHADER_CACHE_PATH", path, MAX_PATH)) {
      std::string dir(MAX_PATH, '\0');
      dir.resize(GetModuleFileNameA(nullptr, dir.data(), MAX_PATH));
      dir = dir.substr(0u, dir.find_last_of("\\/")) + "\\shader-cache";
      SetEnvironmentVariableA("DXVK_SHADER_CACHE_PATH", dir.c_str());

      uint32_t deleted = 0u;

      for (const char* pattern : { "\\*.dxvk.lut", "\\*.dxvk.bin" }) {
        WIN32_FIND_DATAA found = { };
        HANDLE find = keepShaderCache ? INVALID_HANDLE_VALUE : FindFirstFileA((dir + pattern).c_str(), &found);

        if (find == INVALID_HANDLE_VALUE)
          continue;

        do {
          deleted += DeleteFileA((dir + "\\" + found.cFileName).c_str()) ? 1u : 0u;
        } while (FindNextFileA(find, &found));

        FindClose(find);
      }

      std::printf("shader cache: %s (%s, %u files deleted)\n", dir.c_str(), keepShaderCache ? "kept" : "cold", deleted);
    } else {
      std::printf("shader cache: %s (DXVK_SHADER_CACHE_PATH, left as it is)\n", path);
    }
  }

  // ---- Vulkan instance and device, owned by the "shell" ----
  Vk vk;

  if (icdPath) {
    // The shell's load (EngineModules::open): absolute path, dependencies only from the DLL's own directory and
    // System32, entry point vk_icdGetInstanceProcAddr, no loader and no interface negotiation.
    char fullPath[MAX_PATH] = { };
    const DWORD n = GetFullPathNameA(icdPath, MAX_PATH, fullPath, nullptr);
    HMODULE icdDll = n && n < MAX_PATH
      ? LoadLibraryExA(fullPath, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32)
      : nullptr;

    if (!icdDll) {
      std::printf("FAIL  LoadLibrary %s: %lu\n", icdPath, GetLastError());
      return 1;
    }

    vk.getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(icdDll, "vk_icdGetInstanceProcAddr"));

    if (!vk.getInstanceProcAddr)
      vk.getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(icdDll, "vkGetInstanceProcAddr"));
  } else {
    HMODULE vulkanDll = LoadLibraryW(L"vulkan-1.dll");

    if (!vulkanDll) {
      std::printf("FAIL  vulkan-1.dll not found\n");
      return 1;
    }

    vk.getInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(vulkanDll, "vkGetInstanceProcAddr"));
  }

  if (!vk.getInstanceProcAddr) {
    std::printf("FAIL  no vkGetInstanceProcAddr\n");
    return 1;
  }

  LoadInstance(vk, nullptr, &vk.createInstance, "vkCreateInstance");

  VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
  app.pApplicationName = "amdgpu-wddm-dxvk-engine-test";
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
  LoadInstance(vk, instance, &vk.getPhysicalDeviceFormatProperties, "vkGetPhysicalDeviceFormatProperties");
  LoadInstance(vk, instance, &vk.getPhysicalDeviceImageFormatProperties, "vkGetPhysicalDeviceImageFormatProperties");
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

  PrintIcdModules();

  if (!physDev) {
    std::printf("FAIL  no matching physical device\n");
    return 1;
  }

  // The driver behind the chosen device; for Mesa, driverInfo carries the build's version and commit.
  auto getProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
    vk.getInstanceProcAddr(instance, "vkGetPhysicalDeviceProperties2"));

  if (getProperties2) {
    VkPhysicalDeviceDriverProperties driver = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
    VkPhysicalDeviceProperties2 props2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
    props2.pNext = &driver;
    getProperties2(physDev, &props2);
    std::printf("driver: %s, %s\n", driver.driverName, driver.driverInfo);
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

  // Arms the engine's out-of-memory injection (test switch outside the ABI) for the ABI 1.4 cases. It costs an
  // environment lookup per allocation, so benchmark runs leave it off.
  const bool injectOutOfMemory = !bench && !benchTiling && !benchShaders;

  if (injectOutOfMemory)
    SetEnvironmentVariableA("BC250DXVK_TEST_OOM", "1");

  IBc250DxvkDevice* engine = nullptr;
  double tCreate = nowMs();

  if (!CheckHr(funcs.CreateDevice(&createInfo, &engine), "CreateDevice"))
    return 1;

  std::printf("      %.1f ms\n", nowMs() - tCreate);

  Check(shell.logLines.load() > 0u, "CreateDevice's own lines (device import) reach the shell's Log");

  // An r9 engine names its shader cache decision at creation; an older one starts no thread at all (E2)
  const bool r9 = HasLine(shell.lines, "amdgpu_wddm_dxvk: Shader cache in ")
               || HasLine(shell.lines, "amdgpu_wddm_dxvk: No shader cache: ");
  const bool cacheOn = HasLine(shell.lines, "amdgpu_wddm_dxvk: Shader cache in ");
  std::printf("      engine %s, shader cache %s, translation on workers %s\n", r9 ? "r9 or later" : "before r9",
    cacheOn ? "on" : "off", r9 && !TranslationOnWorkersOff() ? "on" : "off");

  Microsoft::WRL::ComPtr<ID3D11Device> d3d;
  Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctx;
  CheckHr(engine->GetD3D11Device(IID_PPV_ARGS(&d3d)), "GetD3D11Device");
  CheckHr(engine->GetImmediateContext(IID_PPV_ARGS(&ctx)), "GetImmediateContext");

  if (!d3d || !ctx)
    return 1;

  Check(d3d->GetFeatureLevel() == adapterInfo.MaxFeatureLevel, "device feature level as requested");

  // ---- ABI 1.3: feature answers at other levels, for the shell's GetCaps check ----
  {
    Microsoft::WRL::ComPtr<IBc250DxvkDevice3> engine3;

    if (CheckHr(engine->QueryInterface(IID_PPV_ARGS(&engine3)), "QueryInterface IBc250DxvkDevice3")) {
      struct FeatureCase { D3D11_FEATURE feature; UINT size; };
      static const FeatureCase s_features[] = {
        { D3D11_FEATURE_THREADING,                      sizeof(D3D11_FEATURE_DATA_THREADING) },
        { D3D11_FEATURE_DOUBLES,                        sizeof(D3D11_FEATURE_DATA_DOUBLES) },
        { D3D11_FEATURE_D3D10_X_HARDWARE_OPTIONS,       sizeof(D3D11_FEATURE_DATA_D3D10_X_HARDWARE_OPTIONS) },
        { D3D11_FEATURE_D3D11_OPTIONS,                  sizeof(D3D11_FEATURE_DATA_D3D11_OPTIONS) },
        { D3D11_FEATURE_ARCHITECTURE_INFO,              sizeof(D3D11_FEATURE_DATA_ARCHITECTURE_INFO) },
        { D3D11_FEATURE_D3D9_OPTIONS,                   sizeof(D3D11_FEATURE_DATA_D3D9_OPTIONS) },
        { D3D11_FEATURE_SHADER_MIN_PRECISION_SUPPORT,   sizeof(D3D11_FEATURE_DATA_SHADER_MIN_PRECISION_SUPPORT) },
        { D3D11_FEATURE_D3D9_SHADOW_SUPPORT,            sizeof(D3D11_FEATURE_DATA_D3D9_SHADOW_SUPPORT) },
        { D3D11_FEATURE_D3D11_OPTIONS1,                 sizeof(D3D11_FEATURE_DATA_D3D11_OPTIONS1) },
        { D3D11_FEATURE_D3D9_SIMPLE_INSTANCING_SUPPORT, sizeof(D3D11_FEATURE_DATA_D3D9_SIMPLE_INSTANCING_SUPPORT) },
        { D3D11_FEATURE_MARKER_SUPPORT,                 sizeof(D3D11_FEATURE_DATA_MARKER_SUPPORT) },
        { D3D11_FEATURE_D3D9_OPTIONS1,                  sizeof(D3D11_FEATURE_DATA_D3D9_OPTIONS1) },
        { D3D11_FEATURE_D3D11_OPTIONS2,                 sizeof(D3D11_FEATURE_DATA_D3D11_OPTIONS2) },
        { D3D11_FEATURE_D3D11_OPTIONS3,                 sizeof(D3D11_FEATURE_DATA_D3D11_OPTIONS3) },
        { D3D11_FEATURE_GPU_VIRTUAL_ADDRESS_SUPPORT,    sizeof(D3D11_FEATURE_DATA_GPU_VIRTUAL_ADDRESS_SUPPORT) },
        { D3D11_FEATURE_D3D11_OPTIONS4,                 sizeof(D3D11_FEATURE_DATA_D3D11_OPTIONS4) },
        { D3D11_FEATURE_SHADER_CACHE,                   sizeof(D3D11_FEATURE_DATA_SHADER_CACHE) },
        { D3D11_FEATURE_D3D11_OPTIONS5,                 sizeof(D3D11_FEATURE_DATA_D3D11_OPTIONS5) },
      };

      // Positive control: at the device's own level every answer is the device's, byte for byte
      bool same = true;

      for (const auto& f : s_features) {
        std::array<BYTE, 128> atLevel = { }, fromDevice = { };

        HRESULT hrLevel = f.size <= atLevel.size()
          ? engine3->CheckFeatureSupportAtLevel(d3d->GetFeatureLevel(), f.feature, atLevel.data(), f.size)
          : E_FAIL;
        HRESULT hrDevice = f.size <= fromDevice.size()
          ? d3d->CheckFeatureSupport(f.feature, fromDevice.data(), f.size)
          : E_FAIL;

        if (hrLevel != S_OK || hrDevice != S_OK || std::memcmp(atLevel.data(), fromDevice.data(), f.size)) {
          std::printf("      feature %d: hr 0x%08lX, device hr 0x%08lX, data %s\n", f.feature,
            static_cast<unsigned long>(hrLevel), static_cast<unsigned long>(hrDevice),
            std::memcmp(atLevel.data(), fromDevice.data(), f.size) ? "differ" : "same");
          same = false;
        }
      }

      Check(same, "CheckFeatureSupportAtLevel at the device's level: 18 features as the device answers them");

      // The record a static GetCaps table must agree with: the answers at MaxFeatureLevel of GetAdapterInfo
      D3D_FEATURE_LEVEL max = adapterInfo.MaxFeatureLevel;
      D3D11_FEATURE_DATA_DOUBLES doubles = { };
      D3D11_FEATURE_DATA_D3D10_X_HARDWARE_OPTIONS d3d10 = { };
      D3D11_FEATURE_DATA_D3D11_OPTIONS options = { };
      D3D11_FEATURE_DATA_ARCHITECTURE_INFO arch = { };
      D3D11_FEATURE_DATA_SHADER_MIN_PRECISION_SUPPORT precision = { };
      D3D11_FEATURE_DATA_D3D11_OPTIONS1 options1 = { };
      D3D11_FEATURE_DATA_D3D11_OPTIONS2 options2 = { };
      D3D11_FEATURE_DATA_D3D11_OPTIONS3 options3 = { };
      D3D11_FEATURE_DATA_D3D11_OPTIONS5 options5 = { };

      bool record = SUCCEEDED(engine3->CheckFeatureSupportAtLevel(max, D3D11_FEATURE_DOUBLES, &doubles, sizeof(doubles)))
        && SUCCEEDED(engine3->CheckFeatureSupportAtLevel(max, D3D11_FEATURE_D3D10_X_HARDWARE_OPTIONS, &d3d10, sizeof(d3d10)))
        && SUCCEEDED(engine3->CheckFeatureSupportAtLevel(max, D3D11_FEATURE_D3D11_OPTIONS, &options, sizeof(options)))
        && SUCCEEDED(engine3->CheckFeatureSupportAtLevel(max, D3D11_FEATURE_ARCHITECTURE_INFO, &arch, sizeof(arch)))
        && SUCCEEDED(engine3->CheckFeatureSupportAtLevel(max, D3D11_FEATURE_SHADER_MIN_PRECISION_SUPPORT, &precision, sizeof(precision)))
        && SUCCEEDED(engine3->CheckFeatureSupportAtLevel(max, D3D11_FEATURE_D3D11_OPTIONS1, &options1, sizeof(options1)))
        && SUCCEEDED(engine3->CheckFeatureSupportAtLevel(max, D3D11_FEATURE_D3D11_OPTIONS2, &options2, sizeof(options2)))
        && SUCCEEDED(engine3->CheckFeatureSupportAtLevel(max, D3D11_FEATURE_D3D11_OPTIONS3, &options3, sizeof(options3)))
        && SUCCEEDED(engine3->CheckFeatureSupportAtLevel(max, D3D11_FEATURE_D3D11_OPTIONS5, &options5, sizeof(options5)));

      Check(record, "CheckFeatureSupportAtLevel at MaxFeatureLevel: the GetCaps record");

      if (record) {
        std::printf("      caps at 0x%x: doubles %d, compute+raw %d, logic op %d, extended doubles %d, "
          "min precision %u/%u, TBDR %d\n", max, doubles.DoublePrecisionFloatShaderOps,
          d3d10.ComputeShaders_Plus_RawAndStructuredBuffers_Via_Shader_4_x, options.OutputMergerLogicOp,
          options.ExtendedDoublesShaderInstructions, unsigned(precision.PixelShaderMinPrecision),
          unsigned(precision.AllOtherShaderStagesMinPrecision), arch.TileBasedDeferredRenderer);
        std::printf("      typed UAV loads %d, ROVs %d, stencil ref %d, tiled tier %d, min/max filtering %d, "
          "conservative tier %d, VP/RT index anywhere %d, shared tier %d\n",
          options2.TypedUAVLoadAdditionalFormats, options2.ROVsSupported, options2.PSSpecifiedStencilRefSupported,
          int(options2.TiledResourcesTier), options1.MinMaxFiltering, int(options2.ConservativeRasterizationTier),
          options3.VPAndRTArrayIndexFromAnyShaderFeedingRasterizer, int(options5.SharedResourceTier));
      }

      // The level applies: doubles need 11_0 in DXVK, whatever the hardware has
      D3D11_FEATURE_DATA_DOUBLES doubles10 = { TRUE };
      Check(engine3->CheckFeatureSupportAtLevel(D3D_FEATURE_LEVEL_10_0, D3D11_FEATURE_DOUBLES, &doubles10,
          sizeof(doubles10)) == S_OK && !doubles10.DoublePrecisionFloatShaderOps,
        "CheckFeatureSupportAtLevel 10_0: no doubles below 11_0");

      D3D11_FEATURE_DATA_FORMAT_SUPPORT format = { DXGI_FORMAT_R8G8B8A8_UNORM };
      Check(engine3->CheckFeatureSupportAtLevel(D3D_FEATURE_LEVEL(0xf000), D3D11_FEATURE_DOUBLES, &doubles,
          sizeof(doubles)) == E_INVALIDARG
        && engine3->CheckFeatureSupportAtLevel(D3D_FEATURE_LEVEL(0x1000), D3D11_FEATURE_DOUBLES, &doubles,
          sizeof(doubles)) == E_INVALIDARG
        && engine3->CheckFeatureSupportAtLevel(max, D3D11_FEATURE_FORMAT_SUPPORT, &format, sizeof(format)) == E_INVALIDARG
        && engine3->CheckFeatureSupportAtLevel(max, D3D11_FEATURE_DOUBLES, nullptr, sizeof(doubles)) == E_INVALIDARG
        && engine3->CheckFeatureSupportAtLevel(max, D3D11_FEATURE_DOUBLES, &options, sizeof(options)) == E_INVALIDARG,
        "CheckFeatureSupportAtLevel rejects levels out of range, format queries, NULL data and a wrong size");
    }
  }

  // ---- ABI 1.4: out of memory, deferred errors, trim ----
  {
    Microsoft::WRL::ComPtr<IBc250DxvkDevice4> engine4;

    if (CheckHr(engine->QueryInterface(IID_PPV_ARGS(&engine4)), "QueryInterface IBc250DxvkDevice4")) {
      Check(engine4->TakeDeferredError() == S_OK, "TakeDeferredError: nothing recorded after CreateDevice");

      // 1 MiB each: above the local allocation cache and twice the context's staging buffer, so every call
      // below reaches the allocator.
      D3D11_BUFFER_DESC dynDesc = { 1u << 20, D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER, D3D11_CPU_ACCESS_WRITE };
      D3D11_BUFFER_DESC defDesc = { 1u << 20, D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER };
      Microsoft::WRL::ComPtr<ID3D11Buffer> dyn, def;
      CheckHr(d3d->CreateBuffer(&dynDesc, nullptr, &dyn), "CreateBuffer (1 MiB dynamic)");
      CheckHr(d3d->CreateBuffer(&defDesc, nullptr, &def), "CreateBuffer (1 MiB default)");

      std::vector<uint8_t> upload(1u << 20, 0x5au);

      if (!injectOutOfMemory) {
        std::printf("SKIP  injected out of memory (benchmark run)\n");
      } else if (dyn && def) {
        SetEnvironmentVariableA("BC250DXVK_TEST_OOM_NOW", "1");

        Microsoft::WRL::ComPtr<ID3D11Buffer> noBuffer;
        HRESULT hrBuffer = d3d->CreateBuffer(&defDesc, nullptr, &noBuffer);

        D3D11_TEXTURE2D_DESC texDesc = { 256u, 256u, 1u, 1u, DXGI_FORMAT_R8G8B8A8_UNORM, { 1u, 0u },
          D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE };
        Microsoft::WRL::ComPtr<ID3D11Texture2D> noTexture;
        HRESULT hrTexture = d3d->CreateTexture2D(&texDesc, nullptr, &noTexture);

        D3D11_MAPPED_SUBRESOURCE mapped = { };
        mapped.pData = upload.data();
        HRESULT hrMap = ctx->Map(dyn.Get(), 0u, D3D11_MAP_WRITE_DISCARD, 0u, &mapped);

        ctx->UpdateSubresource(def.Get(), 0u, nullptr, upload.data(), 0u, 0u);

        SetEnvironmentVariableA("BC250DXVK_TEST_OOM_NOW", nullptr);

        Check(hrBuffer == E_OUTOFMEMORY && !noBuffer, "out of memory: CreateBuffer returns E_OUTOFMEMORY, no buffer");
        Check(hrTexture == E_OUTOFMEMORY && !noTexture,
          "out of memory: CreateTexture2D returns E_OUTOFMEMORY, no texture");
        Check(hrMap == E_OUTOFMEMORY && !mapped.pData,
          "out of memory: Map(WRITE_DISCARD) returns E_OUTOFMEMORY with pData NULL");
        Check(engine4->TakeDeferredError() == E_OUTOFMEMORY,
          "out of memory: UpdateSubresource records E_OUTOFMEMORY for TakeDeferredError");
        Check(engine4->TakeDeferredError() == S_OK, "TakeDeferredError clears what it returned");

        // Memory is back: the same calls succeed and the device lives
        HRESULT hrRemap = ctx->Map(dyn.Get(), 0u, D3D11_MAP_WRITE_DISCARD, 0u, &mapped);

        if (SUCCEEDED(hrRemap))
          ctx->Unmap(dyn.Get(), 0u);

        ctx->UpdateSubresource(def.Get(), 0u, nullptr, upload.data(), 0u, 0u);
        ctx->Flush();

        Check(hrRemap == S_OK && engine4->TakeDeferredError() == S_OK && d3d->GetDeviceRemovedReason() == S_OK,
          "after out of memory: Map and UpdateSubresource succeed, device not removed");
      }

      // TrimMemory returns released memory to the driver. Usage as the driver reports it for the process
      // (VK_EXT_memory_budget) counts Vulkan memory blocks, which is what a trim can change. The query needs
      // the physical device to support the extension, not the device to enable it.
      bool haveBudget = false;
      auto enumerateExtensions = reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
        vk.getInstanceProcAddr(instance, "vkEnumerateDeviceExtensionProperties"));

      if (enumerateExtensions) {
        uint32_t extCount = 0u;
        enumerateExtensions(physDev, nullptr, &extCount, nullptr);
        std::vector<VkExtensionProperties> exts(extCount);
        enumerateExtensions(physDev, nullptr, &extCount, exts.data());

        for (const auto& e : exts)
          haveBudget |= !std::strcmp(e.extensionName, VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
      }

      auto getMemoryProperties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties2>(
        vk.getInstanceProcAddr(instance, "vkGetPhysicalDeviceMemoryProperties2"));

      auto usage = [&] {
        VkPhysicalDeviceMemoryBudgetPropertiesEXT budget = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT };
        VkPhysicalDeviceMemoryProperties2 props = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, &budget };
        getMemoryProperties2(physDev, &props);

        VkDeviceSize sum = 0u;

        for (uint32_t i = 0u; i < props.memoryProperties.memoryHeapCount; i++)
          sum += budget.heapUsage[i];

        return sum;
      };

      constexpr UINT TrimBuffers = 256u;
      constexpr VkDeviceSize MiB = VkDeviceSize(1u) << 20;

      if (!haveBudget || !getMemoryProperties2) {
        std::printf("SKIP  TrimMemory measurement: VK_EXT_memory_budget not supported\n");
        Check(engine4->TrimMemory() == S_OK, "TrimMemory");
      } else {
        VkDeviceSize before = usage();
        std::vector<Microsoft::WRL::ComPtr<ID3D11Buffer>> buffers(TrimBuffers);
        bool created = true;

        for (auto& b : buffers)
          created &= SUCCEEDED(d3d->CreateBuffer(&defDesc, nullptr, &b));

        ctx->Flush();
        VkDeviceSize allocated = usage();

        buffers.clear();
        ctx->Flush();
        VkDeviceSize released = usage();

        double t0 = nowMs();
        HRESULT hrTrim = engine4->TrimMemory();
        double trimMs = nowMs() - t0;
        VkDeviceSize trimmed = usage();

        std::printf("      heap usage MiB: before %llu, %u x 1 MiB buffers %llu, released %llu, trimmed %llu (%.1f ms)\n",
          (unsigned long long)(before / MiB), TrimBuffers, (unsigned long long)(allocated / MiB),
          (unsigned long long)(released / MiB), (unsigned long long)(trimmed / MiB), trimMs);

        // The engine's own count of Vulkan memory it holds, from its info line
        for (const auto& line : shell.lines) {
          if (line.find("TrimMemory: device memory") != std::string::npos)
            std::printf("      %s\n", line.c_str());
        }

        Check(created && allocated >= before + (TrimBuffers / 2u) * MiB,
          "TrimMemory setup: 256 MiB of default buffers show in the driver's heap usage");
        Check(hrTrim == S_OK && trimmed + (TrimBuffers * 3u / 4u) * MiB <= allocated,
          "TrimMemory gives at least 3/4 of the released buffers' memory back to the driver");
      }

      Check(engine4->TakeDeferredError() == S_OK && d3d->GetDeviceRemovedReason() == S_OK,
        "after TrimMemory: no deferred error, device not removed");
    }
  }

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

  // ---- ClearView on buffer RTVs: element ranges, swizzled and packed formats, the rest of the buffer kept ----
  {
    Microsoft::WRL::ComPtr<ID3D11DeviceContext1> ctx1;
    CheckHr(ctx.As(&ctx1), "QueryInterface ID3D11DeviceContext1 (buffer ClearView)");

    // The engine clears a buffer RTV in the buffer when its format has storage texel buffer support, otherwise
    // through the 1D proxy image it renders to. The test prints which path each format takes on this adapter.
    struct Case {
      DXGI_FORMAT           format;
      VkFormat              vkFormat;
      UINT                  size;
      float                 color[4];
      std::array<int, 4>    cleared;
      bool                  required;
      const char*           what;
    };

    const Case cases[3] = {
      // Clamped per component: 1, 0, 1 and 0.5 rounded to 128
      { DXGI_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM, 4u, { 1.0f, -1.0f, 2.0f, 0.5f }, { 255, 0, 255, 128 }, true, "R8G8B8A8_UNORM" },
      // Stored as R8 with an alpha swizzle: the byte is alpha (255), not red (51)
      { DXGI_FORMAT_A8_UNORM, VK_FORMAT_R8_UNORM, 1u, { 0.2f, 0.4f, 0.6f, 1.0f }, { 255, 0, 0, 0 }, false, "A8_UNORM" },
      // Red is bits 11..15 of the little-endian word: 0xF800
      { DXGI_FORMAT_B5G6R5_UNORM, VK_FORMAT_R5G6B5_UNORM_PACK16, 2u, { 1.0f, 0.0f, 0.0f, 1.0f }, { 0x00, 0xF8, 0, 0 }, false, "B5G6R5_UNORM" },
    };

    constexpr UINT elements = 64u;
    constexpr UINT first    = 8u;
    constexpr UINT count    = 32u;

    for (const auto& c : cases) {
      UINT support = 0u;
      d3d->CheckFormatSupport(c.format, &support);

      const UINT needed = D3D11_FORMAT_SUPPORT_BUFFER | D3D11_FORMAT_SUPPORT_RENDER_TARGET;

      if ((support & needed) != needed) {
        if (c.required)
          Check(false, "R8G8B8A8_UNORM supports buffer render targets");
        else
          std::printf("      buffer ClearView %s: no buffer render target support, skipped\n", c.what);
        continue;
      }

      VkFormatProperties vkProps = { };
      vk.getPhysicalDeviceFormatProperties(physDev, c.vkFormat, &vkProps);
      bool typed = (vkProps.bufferFeatures & VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT) != 0u;
      std::printf("      buffer ClearView %s: %s\n", c.what, typed ? "typed buffer view" : "proxy image");

      std::vector<uint8_t> fill(elements * c.size, 0x11u);
      D3D11_SUBRESOURCE_DATA init = { fill.data(), 0u, 0u };

      D3D11_BUFFER_DESC bufDesc = { };
      bufDesc.ByteWidth = elements * c.size;
      bufDesc.Usage     = D3D11_USAGE_DEFAULT;
      bufDesc.BindFlags = D3D11_BIND_RENDER_TARGET;

      D3D11_BUFFER_DESC readDesc = bufDesc;
      readDesc.Usage          = D3D11_USAGE_STAGING;
      readDesc.BindFlags      = 0u;
      readDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;

      D3D11_RENDER_TARGET_VIEW_DESC rtvDesc = { };
      rtvDesc.Format              = c.format;
      rtvDesc.ViewDimension       = D3D11_RTV_DIMENSION_BUFFER;
      rtvDesc.Buffer.FirstElement = first;
      rtvDesc.Buffer.NumElements  = count;

      Microsoft::WRL::ComPtr<ID3D11Buffer> buffer, readBack;
      Microsoft::WRL::ComPtr<ID3D11RenderTargetView> bufRtv;

      if (!ctx1
       || !CheckHr(d3d->CreateBuffer(&bufDesc, &init, &buffer), "CreateBuffer (buffer render target)")
       || !CheckHr(d3d->CreateBuffer(&readDesc, nullptr, &readBack), "CreateBuffer (buffer ClearView read-back)")
       || !CheckHr(d3d->CreateRenderTargetView(buffer.Get(), &rtvDesc, &bufRtv), "CreateRenderTargetView (buffer, elements 8..40)"))
        continue;

      // Counts the elements that differ from the expectation: element e holds the cleared value if
      // inRange(e), else the initial fill
      auto mismatches = [&] (auto inRange) {
        D3D11_MAPPED_SUBRESOURCE m = { };
        ctx->CopyResource(readBack.Get(), buffer.Get());

        if (FAILED(ctx->Map(readBack.Get(), 0u, D3D11_MAP_READ, 0u, &m)))
          return int(elements);

        int bad = 0;
        auto bytes = static_cast<const uint8_t*>(m.pData);

        for (UINT e = 0u; e < elements; e++) {
          bool ok = true;

          for (UINT b = 0u; b < c.size; b++) {
            int expected = inRange(e) ? c.cleared[b] : 0x11;
            ok &= std::abs(int(bytes[e * c.size + b]) - expected) <= 1;
          }

          bad += ok ? 0 : 1;
        }

        ctx->Unmap(readBack.Get(), 0u);
        return bad;
      };

      // View elements 4..12 and 20..24 are buffer elements 12..20 and 28..32; the third rectangle is empty
      const D3D11_RECT rects[3] = { { 4, 0, 12, 1 }, { 20, 0, 24, 1 }, { 30, 0, 30, 1 } };
      ctx1->ClearView(bufRtv.Get(), c.color, rects, 3u);

      int bad = mismatches([] (UINT e) { return (e >= 12u && e < 20u) || (e >= 28u && e < 32u); });
      std::printf("      buffer ClearView %s, two rectangles: %d mismatching elements\n", c.what, bad);
      Check(bad == 0, "ClearView on a buffer RTV writes its rectangles' elements, converted, and nothing else");

      ctx1->ClearView(bufRtv.Get(), c.color, nullptr, 0u);

      bad = mismatches([] (UINT e) { return e >= first && e < first + count; });
      std::printf("      buffer ClearView %s, whole view: %d mismatching elements\n", c.what, bad);
      Check(bad == 0, "ClearView on a buffer RTV without rectangles writes the view's elements only");
    }
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

      // Blt1 (ABI 1.1): source rectangles. Checked pixels stay two texels inside each source quadrant, so
      // that linear filtering at a rectangle's edge cannot blend in a neighbouring quadrant.
      Microsoft::WRL::ComPtr<IBc250DxvkDevice1> engine1;

      if (CheckHr(engine->QueryInterface(IID_PPV_ARGS(&engine1)), "QueryInterface IBc250DxvkDevice1")) {
        ctx->ClearRenderTargetView(rtv.Get(), black);

        BC250_DXVK_BLT1 blt1 = { };
        blt1.Destination     = rt.Get();
        blt1.Source          = src.Get();
        blt1.SourceRect      = { 16, 0, 32, 16 };
        blt1.DestinationRect = { 0, 0, 32, 32 };
        blt1.Flags           = BC250_DXVK_BLT_STRETCH | BC250_DXVK_BLT_CONVERT;
        blt1.Rotation        = DXGI_MODE_ROTATION_IDENTITY;

        if (CheckHr(engine1->Blt1(&blt1), "Blt1: the green source quadrant stretched onto 32x32")) {
          Check(same(readAt(staging.Get(), rt.Get(), 4u, 4u), q[1]) && same(readAt(staging.Get(), rt.Get(), 27u, 27u), q[1])
             && similar(readAt(staging.Get(), rt.Get(), 40u, 20u), 0, 0, 0, 0),
            "Blt1 stretches its source rectangle onto its destination rectangle and writes nothing else");
        }

        blt1.SourceRect      = { 0, 16, 32, 32 };
        blt1.DestinationRect = { 16, 40, 48, 56 };
        blt1.Flags           = BC250_DXVK_BLT_CONVERT;

        if (CheckHr(engine1->Blt1(&blt1), "Blt1: the lower half of the source, unscaled")) {
          Check(same(readAt(staging.Get(), rt.Get(), 20u, 48u), q[2]) && same(readAt(staging.Get(), rt.Get(), 44u, 48u), q[3])
             && similar(readAt(staging.Get(), rt.Get(), 8u, 48u), 0, 0, 0, 0) && similar(readAt(staging.Get(), rt.Get(), 20u, 36u), 0, 0, 0, 0),
            "an unscaled Blt1 moves its source rectangle to its destination rectangle");
        }

        blt1.SourceRect      = { 0, 0, 32, 16 };
        blt1.DestinationRect = { 32, 0, 64, 16 };
        blt1.Rotation        = DXGI_MODE_ROTATION_ROTATE180;

        if (CheckHr(engine1->Blt1(&blt1), "Blt1: the upper half of the source, rotated by 180 degrees")) {
          Check(same(readAt(staging.Get(), rt.Get(), 36u, 8u), q[1]) && same(readAt(staging.Get(), rt.Get(), 60u, 8u), q[0]),
            "180 degrees: the content turns within the destination rectangle");
        }

        BC250_DXVK_BLT1 bad1 = blt1;
        bad1.Rotation   = DXGI_MODE_ROTATION_IDENTITY;
        bad1.SourceRect = { 0, 0, 33, 16 };
        Check(engine1->Blt1(&bad1) == E_INVALIDARG, "Blt1 rejects a source rectangle outside the source");

        bad1.SourceRect = { 4, 4, 4, 8 };
        Check(engine1->Blt1(&bad1) == E_INVALIDARG, "Blt1 rejects an empty source rectangle");

        bad1.SourceRect = { -1, 0, 8, 8 };
        Check(engine1->Blt1(&bad1) == E_INVALIDARG, "Blt1 rejects a source rectangle with a negative corner");
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

  // ---- a runtime image with the shell's own tiling (ABI 1.2): LINEAR, as the shell allocates surfaces ----
  VkImage linImage = VK_NULL_HANDLE;
  VkDeviceMemory linMemory = VK_NULL_HANDLE;
  {
    Microsoft::WRL::ComPtr<IBc250DxvkDevice2> engine2;

    VkImageCreateInfo linInfo = imageInfo;
    linInfo.tiling = VK_IMAGE_TILING_LINEAR;

    // The shell's own copy of the view format list, in another order, behind another structure: the engine
    // compares formats, not pointers or order
    std::vector<VkFormat> shellFormats;

    if (formatList)
      shellFormats.assign(formatList->pViewFormats, formatList->pViewFormats + formatList->viewFormatCount);

    std::reverse(shellFormats.begin(), shellFormats.end());

    VkImageFormatListCreateInfo shellList = { VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO };
    shellList.viewFormatCount = uint32_t(shellFormats.size());
    shellList.pViewFormats    = shellFormats.data();

    VkExternalMemoryImageCreateInfo external = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO };
    external.pNext = &shellList;
    linInfo.pNext = &external;

    VkImageFormatProperties linProps = { };
    bool linSupported = vk.getPhysicalDeviceImageFormatProperties(physDev, linInfo.format, linInfo.imageType,
      linInfo.tiling, linInfo.usage, linInfo.flags, &linProps) == VK_SUCCESS;

    if (!CheckHr(engine->QueryInterface(IID_PPV_ARGS(&engine2)), "QueryInterface IBc250DxvkDevice2")) {
      // An older engine: nothing below applies
    } else if (!linSupported) {
      std::printf("      LINEAR render target image not supported on this adapter: ABI 1.2 checks skipped\n");
    } else {
      VkMemoryRequirements linReq = { };
      bool ok = vk.createImage(device, &linInfo, nullptr, &linImage) == VK_SUCCESS;

      if (ok) {
        vk.getImageMemoryRequirements(device, linImage, &linReq);

        uint32_t linType = ~0u;

        for (uint32_t i = 0u; i < memProps.memoryTypeCount && linType == ~0u; i++) {
          if ((linReq.memoryTypeBits & (1u << i))
           && (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            linType = i;
        }

        VkMemoryAllocateInfo linAlloc = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        linAlloc.allocationSize  = linReq.size;
        linAlloc.memoryTypeIndex = linType;

        ok = linType != ~0u
          && vk.allocateMemory(device, &linAlloc, nullptr, &linMemory) == VK_SUCCESS
          && vk.bindImageMemory(device, linImage, linMemory, 0u) == VK_SUCCESS;
      }

      Check(ok, "shell creates and binds a LINEAR image from GetImageCreateInfo, with its own view format list");

      auto rejects = [&] (const VkImageCreateInfo& info, const char* what) {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> none;
        Check(engine2->CreateTexture2DFromImage2(&rtDesc, &info, linImage, &none) == E_INVALIDARG && !none, what);
      };

      VkImageCreateInfo bad = linInfo;
      bad.format = VK_FORMAT_B8G8R8A8_UNORM;
      rejects(bad, "CreateTexture2DFromImage2 rejects another format");

      bad = linInfo;
      bad.extent.width++;
      rejects(bad, "CreateTexture2DFromImage2 rejects another extent");

      bad = linInfo;
      bad.usage &= ~VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
      rejects(bad, "CreateTexture2DFromImage2 rejects a removed usage bit");

      bad = linInfo;
      bad.flags &= ~VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
      rejects(bad, "CreateTexture2DFromImage2 rejects a removed flag");

      if (formatList) {
        bad = linInfo;
        bad.pNext = nullptr;
        rejects(bad, "CreateTexture2DFromImage2 rejects a mutable image without the view format list");
      }

      bad = linInfo;
      bad.tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT;
      rejects(bad, "CreateTexture2DFromImage2 rejects a tiling other than OPTIMAL and LINEAR");

      // The tiling the engine gives the wrapped image, as DXVK's interop interface reports it
      auto engineTiling = [] (ID3D11Texture2D* texture) {
        Microsoft::WRL::ComPtr<DxvkInteropSurface> interop;
        VkImageCreateInfo seen = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };

        if (!texture || FAILED(texture->QueryInterface(IID_PPV_ARGS(&interop)))
         || FAILED(interop->GetVulkanImageInfo(nullptr, nullptr, &seen)))
          return VK_IMAGE_TILING_MAX_ENUM;

        return seen.tiling;
      };

      Microsoft::WRL::ComPtr<ID3D11Texture2D> linTex, assumed;
      Microsoft::WRL::ComPtr<ID3D11RenderTargetView> linRtv;

      if (ok && CheckHr(engine2->CreateTexture2DFromImage2(&rtDesc, &linInfo, linImage, &linTex),
          "CreateTexture2DFromImage2 (LINEAR)")) {
        Check(engineTiling(linTex.Get()) == VK_IMAGE_TILING_LINEAR, "the engine uses the image with LINEAR tiling");

        // Control for the probe above: the 1.0 call takes GetImageCreateInfo's tiling, whatever the image has
        if (SUCCEEDED(engine->CreateTexture2DFromImage(&rtDesc, linImage, &assumed)))
          Check(engineTiling(assumed.Get()) == imageInfo.tiling, "CreateTexture2DFromImage keeps GetImageCreateInfo's tiling (probe control)");

        assumed.Reset();

        // The first draw again, into the LINEAR image
        if (CheckHr(d3d->CreateRenderTargetView(linTex.Get(), &rtvDesc, &linRtv), "CreateRenderTargetView (LINEAR image)")) {
          ID3D11RenderTargetView* linRtvs[] = { linRtv.Get() };
          ctx->ClearRenderTargetView(linRtv.Get(), clear);
          ctx->IASetInputLayout(layout.Get());
          ctx->IASetVertexBuffers(0u, 1u, vbs, &stride, &offset);
          ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
          ctx->VSSetShader(vs.Get(), nullptr, 0u);
          ctx->PSSetShader(ps.Get(), nullptr, 0u);
          ctx->RSSetViewports(1u, &viewport);
          ctx->OMSetRenderTargets(1u, linRtvs, nullptr);
          ctx->Draw(3u, 0u);
          ctx->OMSetRenderTargets(1u, rtvs, nullptr);

          auto readAt = [&] (ID3D11Texture2D* texture, UINT x, UINT y) {
            std::array<int, 4> result = { -1, -1, -1, -1 };
            D3D11_MAPPED_SUBRESOURCE m = { };
            ctx->CopyResource(staging.Get(), texture);

            if (SUCCEEDED(ctx->Map(staging.Get(), 0u, D3D11_MAP_READ, 0u, &m))) {
              const uint8_t* p = static_cast<const uint8_t*>(m.pData) + y * m.RowPitch + x * 4u;
              result = { p[0], p[1], p[2], p[3] };
              ctx->Unmap(staging.Get(), 0u);
            }

            return result;
          };

          ctx->CopyResource(staging.Get(), linTex.Get());
          D3D11_MAPPED_SUBRESOURCE m = { };

          if (CheckHr(ctx->Map(staging.Get(), 0u, D3D11_MAP_READ, 0u, &m), "Map staging (LINEAR image)")) {
            auto at = [&] (UINT x, UINT y) {
              const uint8_t* p = static_cast<const uint8_t*>(m.pData) + y * m.RowPitch + x * 4u;
              return std::array<int, 4>{ p[0], p[1], p[2], p[3] };
            };

            uint32_t linCovered = 0u;

            for (UINT y = 0u; y < H; y++) {
              for (UINT x = 0u; x < W; x++)
                linCovered += similar(at(x, y), cr, cg, cb, 1) ? 0u : 1u;
            }

            auto l0 = at(0u, 0u);
            std::printf("      LINEAR image: (0,0)=%d,%d,%d  covered=%u\n", l0[0], l0[1], l0[2], linCovered);
            Check(similar(at(W - 1u, H - 1u), cr, cg, cb, 1) && l0[0] > 230 && l0[1] < 25
               && linCovered >= 2016u && linCovered <= 2080u,
              "clear and triangle in the LINEAR image as in the first draw");
            ctx->Unmap(staging.Get(), 0u);
          }

          // Blt with the LINEAR image as source, then as destination of a rectangle
          const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
          ctx->ClearRenderTargetView(rtv.Get(), black);

          BC250_DXVK_BLT1 blt1 = { };
          blt1.Destination     = rt.Get();
          blt1.Source          = linTex.Get();
          blt1.SourceRect      = { 0, 0, LONG(W), LONG(H) };
          blt1.DestinationRect = { 0, 0, LONG(W), LONG(H) };
          blt1.Rotation        = DXGI_MODE_ROTATION_IDENTITY;

          if (CheckHr(engine2->Blt1(&blt1), "Blt1 from the LINEAR image")) {
            Check(readAt(rt.Get(), 0u, 0u)[0] > 230 && similar(readAt(rt.Get(), W - 1u, H - 1u), cr, cg, cb, 1),
              "Blt1 copies the LINEAR image's triangle and clear");
          }

          // The lower right quarter of rt holds the clear colour only (x + y >= 64 there)
          ctx->ClearRenderTargetView(linRtv.Get(), black);
          blt1.Destination     = linTex.Get();
          blt1.Source          = rt.Get();
          blt1.SourceRect      = { LONG(W / 2u), LONG(H / 2u), LONG(W), LONG(H) };
          blt1.DestinationRect = { 0, 0, LONG(W / 2u), LONG(H / 2u) };

          if (CheckHr(engine2->Blt1(&blt1), "Blt1 into a rectangle of the LINEAR image")) {
            Check(similar(readAt(linTex.Get(), 10u, 10u), cr, cg, cb, 1)
               && similar(readAt(linTex.Get(), 40u, 40u), 0, 0, 0, 0),
              "Blt1 writes its rectangle of the LINEAR image and nothing else");
          }
        }

        CheckHr(engine->WaitForResourceIdle(linTex.Get()), "WaitForResourceIdle (LINEAR image)");
      }
    }
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

    const BC250_DXVK_STREAM_OUTPUT soRastDecl = { soEntries, 3u, &soStride, 1u, 0u };
    BC250_DXVK_SHADER_DESC rastDesc = noCodeDesc;
    rastDesc.StreamOutput = &soRastDecl;

    Microsoft::WRL::ComPtr<ID3D11GeometryShader> soNoCode, soVsCode, soPsCode, soRast;
    CheckHr(engine->CreateShader(&noCodeDesc, IID_PPV_ARGS(&soNoCode)), "CreateShader (stream output, no code)");
    CheckHr(engine->CreateShader(&vsCodeDesc, IID_PPV_ARGS(&soVsCode)), "CreateShader (stream output, VS code)");
    CheckHr(engine->CreateShader(&rastDesc, IID_PPV_ARGS(&soRast)), "CreateShader (stream output, rasterized stream 0)");
    Check(engine->CreateShader(&psCodeDesc, IID_PPV_ARGS(&soPsCode)) == E_INVALIDARG,
      "CreateShader rejects stream output with pixel shader code");

    D3D11_BUFFER_DESC soBufDesc = { 3u * soStride, D3D11_USAGE_DEFAULT, D3D11_BIND_STREAM_OUTPUT };
    D3D11_BUFFER_DESC soReadDesc = { 3u * soStride, D3D11_USAGE_STAGING, 0u, D3D11_CPU_ACCESS_READ };
    Microsoft::WRL::ComPtr<ID3D11Buffer> soBuf, soRead;
    bool soReady = soNoCode.Get() && soVsCode.Get() && soRast.Get()
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

      // Without a geometry program, stream output receives whole primitives, strips expanded to lists, and
      // writes only primitives that fit: the three-record target takes one line of a strip, not two.
      Check(capture(soNoCode.Get(), D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST, "triangle list") == 3u,
        "a triangle list streams out all three vertices of its triangle");
      Check(capture(soVsCode.Get(), D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP, "triangle strip") == 3u,
        "a triangle strip streams out its triangle (VS code)");
      Check(capture(soNoCode.Get(), D3D11_PRIMITIVE_TOPOLOGY_LINELIST, "line list") == 2u,
        "a line list streams out its complete line and drops the lone third vertex");
      Check(capture(soNoCode.Get(), D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP, "line strip") == 2u,
        "a line strip's second line does not fit and is dropped whole");

      // The rasterized stream draws what the pass-through receives: the triangle, not a point. The same
      // draw without a geometry program is the control for the state at this point of the test.
      auto coveredBy = [&] (ID3D11GeometryShader* gs, const char* what) {
        ctx->ClearRenderTargetView(rtv.Get(), clear);

        ID3D11Buffer* targets[] = { soBuf.Get() };
        UINT soOffset = 0u;
        ctx->SOSetTargets(gs ? 1u : 0u, gs ? targets : nullptr, gs ? &soOffset : nullptr);
        ctx->GSSetShader(gs, nullptr, 0u);
        ctx->Draw(3u, 0u);
        ctx->GSSetShader(nullptr, nullptr, 0u);
        ctx->SOSetTargets(0u, nullptr, nullptr);
        ctx->CopyResource(staging.Get(), rt.Get());

        uint32_t count = 0u;

        if (CheckHr(ctx->Map(staging.Get(), 0u, D3D11_MAP_READ, 0u, &mapped), what)) {
          for (UINT y = 0u; y < H; y++) {
            for (UINT x = 0u; x < W; x++) {
              if (!similar(pixel(x, y), cr, cg, cb, 1))
                count++;
            }
          }

          ctx->Unmap(staging.Get(), 0u);
        }

        std::printf("      %s: covered=%u\n", what, count);
        return count;
      };

      uint32_t plainCovered = coveredBy(nullptr, "Map staging (control draw)");
      Check(plainCovered >= 2016u && plainCovered <= 2080u, "control: the draw without a geometry program covers the triangle");

      uint32_t rastCovered = coveredBy(soRast.Get(), "Map staging (rasterized stream)");
      Check(rastCovered >= 2016u && rastCovered <= 2080u,
        "the rasterized stream of the pass-through covers the triangle's pixels");

      // ---- predication (D3D11 spec 20.2): occlusion and stream output overflow predicates ----
      // A predicated operation is skipped when the predicate's result equals the value given to
      // SetPredication. The first use of each predicate below finds its result not yet available,
      // so the engine has to submit and wait for it.
      const float red[4] = { 1.0f, 0.0f, 0.0f, 1.0f };

      // Pixels that differ from the clear colour after an unpredicated clear, then a predicated clear
      // to red and a predicated draw: 0 if both were skipped, the whole target if they ran
      auto changedBy = [&] (ID3D11Predicate* predicate, BOOL value, const char* what) {
        ctx->ClearRenderTargetView(rtv.Get(), clear);
        ctx->SetPredication(predicate, value);
        ctx->ClearRenderTargetView(rtv.Get(), red);
        ctx->Draw(3u, 0u);
        ctx->SetPredication(nullptr, FALSE);
        ctx->CopyResource(staging.Get(), rt.Get());

        uint32_t count = ~0u;

        if (CheckHr(ctx->Map(staging.Get(), 0u, D3D11_MAP_READ, 0u, &mapped), what)) {
          count = 0u;

          for (UINT y = 0u; y < H; y++) {
            for (UINT x = 0u; x < W; x++) {
              if (!similar(pixel(x, y), cr, cg, cb, 1))
                count++;
            }
          }

          ctx->Unmap(staging.Get(), 0u);
        }

        std::printf("      %s: %u pixels changed\n", what, count);
        return count;
      };

      // Waits for a predicate's result the way an application does, for the checks of the result itself
      auto resultOf = [&] (ID3D11Predicate* predicate) {
        BOOL result = -1;

        for (UINT tries = 0u; tries < 5000u; tries++) {
          HRESULT hr = ctx->GetData(predicate, &result, sizeof(result), 0u);

          if (hr != S_FALSE)
            return hr == S_OK ? result : BOOL(-2);

          Sleep(1u);
        }

        return BOOL(-3);
      };

      const D3D11_QUERY_DESC occlusionDesc = { D3D11_QUERY_OCCLUSION_PREDICATE, 0u };
      Microsoft::WRL::ComPtr<ID3D11Predicate> zero, hit;

      if (CheckHr(d3d->CreatePredicate(&occlusionDesc, &zero), "CreatePredicate (occlusion, no samples)")
       && CheckHr(d3d->CreatePredicate(&occlusionDesc, &hit), "CreatePredicate (occlusion, triangle)")) {
        // No samples: the triangle drawn through a viewport far outside the target
        const D3D11_VIEWPORT outside = { -1000.0f, -1000.0f, float(W), float(H), 0.0f, 1.0f };
        ctx->RSSetViewports(1u, &outside);
        ctx->Begin(zero.Get());
        ctx->Draw(3u, 0u);
        ctx->End(zero.Get());
        ctx->RSSetViewports(1u, &viewport);

        ctx->Begin(hit.Get());
        ctx->Draw(3u, 0u);
        ctx->End(hit.Get());

        Check(changedBy(zero.Get(), FALSE, "predicate FALSE, value FALSE") == 0u,
          "an occlusion predicate without samples skips the clear and the draw predicated on FALSE");
        Check(changedBy(zero.Get(), TRUE, "predicate FALSE, value TRUE") == W * H,
          "the same predicate lets them run when predicated on TRUE");
        Check(changedBy(hit.Get(), TRUE, "predicate TRUE, value TRUE") == 0u,
          "an occlusion predicate with samples skips the clear and the draw predicated on TRUE");
        Check(changedBy(hit.Get(), FALSE, "predicate TRUE, value FALSE") == W * H,
          "the same predicate lets them run when predicated on FALSE");
        Check(resultOf(zero.Get()) == FALSE && resultOf(hit.Get()) == TRUE,
          "GetData reports the two occlusion predicates as FALSE and TRUE");

        Microsoft::WRL::ComPtr<ID3D11Predicate> current;
        BOOL currentValue = FALSE;
        ctx->SetPredication(hit.Get(), TRUE);
        ctx->GetPredication(&current, &currentValue);
        ctx->SetPredication(nullptr, FALSE);
        Check(current.Get() == hit.Get() && currentValue == TRUE, "GetPredication returns the predicate and value set");

        // Copies and updates honour predication too: with the operations skipped, the read-back
        // buffer keeps the first pattern
        std::array<float, 3u * SoFloats> first, second, seen;
        first.fill(1.0f);
        second.fill(2.0f);
        seen.fill(0.0f);

        ctx->UpdateSubresource(soBuf.Get(), 0u, nullptr, first.data(), 0u, 0u);
        ctx->CopyResource(soRead.Get(), soBuf.Get());

        ctx->SetPredication(hit.Get(), TRUE);
        ctx->UpdateSubresource(soBuf.Get(), 0u, nullptr, second.data(), 0u, 0u);
        ctx->CopyResource(soRead.Get(), soBuf.Get());
        ctx->SetPredication(nullptr, FALSE);

        D3D11_MAPPED_SUBRESOURCE m = { };

        if (CheckHr(ctx->Map(soRead.Get(), 0u, D3D11_MAP_READ, 0u, &m), "Map (predicated copy)")) {
          std::memcpy(seen.data(), m.pData, sizeof(seen));
          ctx->Unmap(soRead.Get(), 0u);
        }

        Check(seen == first, "a predicated-off CopyResource writes nothing");

        ctx->CopyResource(soRead.Get(), soBuf.Get());
        seen.fill(0.0f);

        if (CheckHr(ctx->Map(soRead.Get(), 0u, D3D11_MAP_READ, 0u, &m), "Map (after predicated update)")) {
          std::memcpy(seen.data(), m.pData, sizeof(seen));
          ctx->Unmap(soRead.Get(), 0u);
        }

        Check(seen == first, "a predicated-off UpdateSubresource writes nothing");
      }

      // Stream output overflow: the three-record target takes one line of a two-line strip, but the
      // whole triangle of a list
      const D3D11_QUERY_DESC anyDesc = { D3D11_QUERY_SO_OVERFLOW_PREDICATE, 0u };
      const D3D11_QUERY_DESC stream0Desc = { D3D11_QUERY_SO_OVERFLOW_PREDICATE_STREAM0, 0u };
      Microsoft::WRL::ComPtr<ID3D11Predicate> overflowed, fitted;

      if (CheckHr(d3d->CreatePredicate(&anyDesc, &overflowed), "CreatePredicate (SO overflow, any stream)")
       && CheckHr(d3d->CreatePredicate(&stream0Desc, &fitted), "CreatePredicate (SO overflow, stream 0)")) {
        ctx->Begin(overflowed.Get());
        capture(soNoCode.Get(), D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP, "overflowing line strip");
        ctx->End(overflowed.Get());

        ctx->Begin(fitted.Get());
        capture(soNoCode.Get(), D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST, "fitting triangle list");
        ctx->End(fitted.Get());

        Check(changedBy(overflowed.Get(), TRUE, "overflow predicate, value TRUE") == 0u,
          "an overflowed stream output skips the operations predicated on TRUE");
        Check(changedBy(fitted.Get(), TRUE, "no-overflow predicate, value TRUE") == W * H,
          "stream output that fits lets them run");
        Check(resultOf(overflowed.Get()) == TRUE && resultOf(fitted.Get()) == FALSE,
          "GetData reports overflow for the strip and none for the list");
      }
    }
  }

  // ---- stream output on two streams of a gs_5_0 program ----
  // The signature entries carry Stream 0, as the shell receives them, so the engine has to take the streams
  // from the program's dcl_stream blocks. Stream 1 reuses a register of stream 0.
  {
    DdiShader gsDdi;
    bool ready = CompileDdi(g_streamsHlsl, "gs", "gs_5_0", &gsDdi);
    UINT pos0 = ~0u, col0 = ~0u, value1 = ~0u;

    for (size_t i = 0u; ready && i < gsDdi.output.size(); i++) {
      const auto& e = gsDdi.output[i];
      const UINT stream = gsDdi.outputStreams[i];
      std::printf("      gs output %zu: register %u mask 0x%x system value %u, stream %u (DDI entry: %u)\n",
        i, e.Register, unsigned(e.Mask), e.SystemValue, stream, unsigned(e.Stream));

      if (stream == 0u)
        (e.SystemValue == 1u ? pos0 : col0) = e.Register;
      else if (stream == 1u)
        value1 = e.Register;
    }

    const bool reused = pos0 != ~0u && col0 != ~0u && (value1 == pos0 || value1 == col0);
    Check(reused, "precondition: stream 1 of the gs_5_0 program reuses an output register of stream 0");
    ready = ready && reused;

    const BC250_DXVK_SO_ENTRY entries[] = {
      { 0u, 0u, pos0,   0xfu },
      { 0u, 0u, col0,   0x7u },
      { 1u, 1u, value1, 0xfu },
    };

    const UINT strides[] = { 7u * sizeof(float), 4u * sizeof(float) };
    const BC250_DXVK_STREAM_OUTPUT decl = { entries, 3u, strides, 2u, D3D11_SO_NO_RASTERIZED_STREAM };

    Microsoft::WRL::ComPtr<ID3D11GeometryShader> gs;

    if (ready) {
      BC250_DXVK_SHADER_DESC desc = MakeDesc(gsDdi);
      desc.StreamOutput = &decl;
      ready = CheckHr(engine->CreateShader(&desc, IID_PPV_ARGS(&gs)),
        "CreateShader (gs_5_0 stream output on streams 0 and 1, signature streams 0)");
    }

    Microsoft::WRL::ComPtr<ID3D11Buffer> targets[2], reads[2];

    for (UINT s = 0u; ready && s < 2u; s++) {
      D3D11_BUFFER_DESC bd = { 3u * strides[s], D3D11_USAGE_DEFAULT, D3D11_BIND_STREAM_OUTPUT };
      D3D11_BUFFER_DESC rd = { 3u * strides[s], D3D11_USAGE_STAGING, 0u, D3D11_CPU_ACCESS_READ };
      std::vector<float> holes(3u * strides[s] / sizeof(float), -7.0f);
      D3D11_SUBRESOURCE_DATA init = { holes.data() };
      ready = CheckHr(d3d->CreateBuffer(&bd, &init, &targets[s]), "CreateBuffer (stream target)")
           && CheckHr(d3d->CreateBuffer(&rd, nullptr, &reads[s]), "CreateBuffer (stream readback)");
    }

    // Draws the three points through the given geometry shader into both targets and compares the records
    auto captureStreams = [&] (ID3D11GeometryShader* program, const char* what) {
      ID3D11Buffer* soTargets[] = { targets[0].Get(), targets[1].Get() };
      const UINT soOffsets[] = { 0u, 0u };

      for (UINT s = 0u; s < 2u; s++) {
        std::vector<float> holes(3u * strides[s] / sizeof(float), -7.0f);
        ctx->UpdateSubresource(targets[s].Get(), 0u, nullptr, holes.data(), 0u, 0u);
      }

      ctx->IASetInputLayout(layout.Get());
      ctx->IASetVertexBuffers(0u, 1u, vbs, &stride, &offset);
      ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);
      ctx->VSSetShader(vs.Get(), nullptr, 0u);
      ctx->GSSetShader(program, nullptr, 0u);
      ctx->SOSetTargets(2u, soTargets, soOffsets);
      ctx->Draw(3u, 0u);
      ctx->SOSetTargets(0u, nullptr, nullptr);
      ctx->GSSetShader(nullptr, nullptr, 0u);
      ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

      std::array<bool, 2u> match = { true, true };

      for (UINT s = 0u; s < 2u; s++) {
        D3D11_MAPPED_SUBRESOURCE m = { };
        ctx->CopyResource(reads[s].Get(), targets[s].Get());

        if (!CheckHr(ctx->Map(reads[s].Get(), 0u, D3D11_MAP_READ, 0u, &m), "Map (stream readback)")) {
          match[s] = false;
          continue;
        }

        const float* r = static_cast<const float*>(m.pData);
        const UINT floats = UINT(strides[s] / sizeof(float));

        for (UINT v = 0u; v < 3u; v++) {
          const float* in = &vertices[5u * v];
          const float first[] = { in[0], in[1], 0.0f, 1.0f, in[2], in[3], in[4] };
          const float second[] = { 2.0f * in[2] + 1.0f, 2.0f * in[3] + 1.0f, 2.0f * in[4] + 1.0f, in[0] + 5.0f };
          const float* expect = s ? second : first;
          std::string line;

          for (UINT i = 0u; i < floats; i++) {
            char value[16];
            std::snprintf(value, sizeof(value), " %.1f", r[v * floats + i]);
            line += value;
            match[s] = match[s] && r[v * floats + i] == expect[i];
          }

          std::printf("      %s: stream %u record %u:%s\n", what, s, v, line.c_str());
        }

        ctx->Unmap(reads[s].Get(), 0u);
      }

      return match;
    };

    // Control: the same program as fxc built it, with its own OSG5, through DXVK's D3D11 entry point
    const D3D11_SO_DECLARATION_ENTRY apiEntries[] = {
      { 0u, "SV_Position", 0u, 0u, 4u, 0u },
      { 0u, "QUUX",        7u, 0u, 3u, 0u },
      { 1u, "BLARG",       2u, 0u, 4u, 1u },
    };

    Microsoft::WRL::ComPtr<ID3D11GeometryShader> control;

    if (ready && CheckHr(d3d->CreateGeometryShaderWithStreamOutput(gsDdi.blob->GetBufferPointer(),
        gsDdi.blob->GetBufferSize(), apiEntries, 3u, strides, 2u, D3D11_SO_NO_RASTERIZED_STREAM, nullptr, &control),
        "CreateGeometryShaderWithStreamOutput (control, fxc container)")) {
      auto match = captureStreams(control.Get(), "control");
      Check(match[0] && match[1], "control: both streams of the fxc container capture their records");
    }

    if (ready) {
      auto match = captureStreams(gs.Get(), "DDI");
      Check(match[0], "stream 0 of the gs_5_0 program captures the position and colour of each point");
      Check(match[1], "stream 1 captures its own value from the reused register");
    }
  }

  if (bench)
    RunBench(engine, d3d.Get(), ctx.Get(), rt.Get(), rtv.Get(), layout.Get(), vb.Get(), stride, W);

  if (benchTiling) {
    Microsoft::WRL::ComPtr<IBc250DxvkDevice2> engine2;

    if (CheckHr(engine->QueryInterface(IID_PPV_ARGS(&engine2)), "tiling bench: QueryInterface IBc250DxvkDevice2"))
      RunTilingBench(vk, physDev, device, memProps, engine2.Get(), d3d.Get(), ctx.Get());
  }

  if (benchShaders)
    RunShaderBench(engine, d3d.Get(), ctx.Get(), benchShaders, benchPattern);

  // ---- threads and submissions ----
  // A start module only ever proves a thread is the engine's: a std::thread starts in ucrtbase.dll, and
  // loaders and layers start their own. The Vulkan call census after teardown is the E2 criterion. Since r9 the
  // engine runs threads that make no Vulkan call: DXVK's pipeline workers, which translate shaders (dxvk-shader-*),
  // and the shader cache's writer (dxvk-cache). None of them may outlive the final Release (below).
  std::set<DWORD> threadsAfter = ProcessThreads();
  uint32_t engineThreads = 0u, otherEngineThreads = 0u;
  std::map<std::string, uint32_t> engineThreadNames;

  for (DWORD tid : threadsAfter) {
    if (threadsBefore.count(tid))
      continue;

    std::string module = ModuleOf(ThreadStartAddress(tid));

    if (_stricmp(module.c_str(), "amdgpu_wddm_dxvk.dll") != 0) {
      std::printf("      new thread %lu starts in %s\n", static_cast<unsigned long>(tid), module.c_str());
      continue;
    }

    std::string name = ThreadName(tid);
    engineThreads++;
    engineThreadNames[name]++;

    if (name.rfind("dxvk-shader-", 0u) != 0u && name != "dxvk-cache")
      otherEngineThreads++;
  }

  for (const auto& entry : engineThreadNames)
    std::printf("      new threads in amdgpu_wddm_dxvk.dll named \"%s\": %u\n", entry.first.c_str(), entry.second);

  std::printf("      queue lock calls %u, from other threads %u\n", shell.lockCalls.load(), shell.foreignCalls.load());
  Check(shell.lockCalls.load() > 0u, "the engine brackets queue submissions with QueueLock");
  Check(shell.foreignCalls.load() == 0u, "every QueueLock call came from the calling thread (E2)");

  if (r9)
    Check(otherEngineThreads == 0u, "the engine's own threads are shader translation workers and the cache writer (r9)");
  else
    Check(engineThreads == 0u, "no new thread starts in amdgpu_wddm_dxvk.dll");

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
  const uint32_t linesAtRelease = shell.logLines.load();

  // ---- r9: translation on workers, the shader cache, and the engine's threads ----
  auto printShaderLines = [] (const Shell& s) {
    for (const auto& line : s.lines) {
      if (line.find("amdgpu_wddm_dxvk: Shader stats:") != std::string::npos
       || line.find("Shader cache closed:") != std::string::npos)
        std::printf("      %s\n", line.c_str());
    }
  };

  printShaderLines(shell);

  if (r9) {
    const long long translated = StatValue(shell.lines, "amdgpu_wddm_dxvk: Shader stats:", "translated");
    const long long queued     = StatValue(shell.lines, "amdgpu_wddm_dxvk: Shader stats:", "queued");
    const long long onWorkers  = StatValue(shell.lines, "amdgpu_wddm_dxvk: Shader stats:", "on_workers");
    const long long errors     = StatValue(shell.lines, "amdgpu_wddm_dxvk: Shader stats:", "errors");
    const long long cacheHits  = cacheOn ? StatValue(shell.lines, "Shader cache closed:", "hits") : 0;

    if (TranslationOnWorkersOff()) {
      Check(queued == 0, "dxvk.translateShadersOnWorkers = False: no translation goes to a worker");
    } else if (translated == 0 && cacheHits > 0) {
      std::printf("      every shader came from the shader cache (a warm start): nothing to translate\n");
    } else {
      Check(queued > 0 && onWorkers > 0, "CreateShader leaves the translation to a pipeline worker");
    }

    Check(errors == 0, "no translation or deferred library compile failed");

    if (cacheOn) {
      const long long written = StatValue(shell.lines, "Shader cache closed:", "written");
      const long long hits    = StatValue(shell.lines, "Shader cache closed:", "hits");
      Check(written > 0 || hits > 0, "the shader cache wrote this run's shaders or found them");
    }
  }

  Check(EngineThreadsSince(threadsBefore).empty(), "no thread of amdgpu_wddm_dxvk.dll outlives the final Release");

  // A second device of the process finds the first device's shaders in the cache: no translation, same pixels
  if (r9 && cacheOn) {
    const long long translatedBefore = StatValue(shell.lines, "amdgpu_wddm_dxvk: Shader stats:", "translated");

    Shell warm;
    InitializeCriticalSection(&warm.queueLock);
    warm.mainThread = GetCurrentThreadId();

    BC250_DXVK_SHELL_SERVICES warmServices = services;
    warmServices.Shell = &warm;

    BC250_DXVK_DEVICE_CREATE_INFO warmInfo = createInfo;
    warmInfo.Services = &warmServices;

    IBc250DxvkDevice* engineWarm = nullptr;

    if (CheckHr(funcs.CreateDevice(&warmInfo, &engineWarm), "second device: CreateDevice")) {
      Microsoft::WRL::ComPtr<ID3D11Device> d3dWarm;
      Microsoft::WRL::ComPtr<ID3D11DeviceContext> ctxWarm;
      Microsoft::WRL::ComPtr<ID3D11VertexShader> vsWarm;
      Microsoft::WRL::ComPtr<ID3D11PixelShader> psWarm;
      Microsoft::WRL::ComPtr<ID3D11InputLayout> layoutWarm;
      Microsoft::WRL::ComPtr<ID3D11Buffer> vbWarm;
      Microsoft::WRL::ComPtr<ID3D11Texture2D> rtWarm, stagingWarm;
      Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtvWarm;

      D3D11_TEXTURE2D_DESC rtWarmDesc = stDesc;
      rtWarmDesc.Usage          = D3D11_USAGE_DEFAULT;
      rtWarmDesc.BindFlags      = D3D11_BIND_RENDER_TARGET;
      rtWarmDesc.CPUAccessFlags = 0u;

      bool ready = SUCCEEDED(engineWarm->GetD3D11Device(IID_PPV_ARGS(&d3dWarm)))
                && SUCCEEDED(engineWarm->GetImmediateContext(IID_PPV_ARGS(&ctxWarm)))
                && SUCCEEDED(engineWarm->CreateShader(&vsDesc, IID_PPV_ARGS(&vsWarm)))
                && SUCCEEDED(engineWarm->CreateShader(&psDesc, IID_PPV_ARGS(&psWarm)))
                && SUCCEEDED(engineWarm->CreateInputLayout(&layoutDesc, &layoutWarm))
                && SUCCEEDED(d3dWarm->CreateBuffer(&vbDesc, &vbData, &vbWarm))
                && SUCCEEDED(d3dWarm->CreateTexture2D(&rtWarmDesc, nullptr, &rtWarm))
                && SUCCEEDED(d3dWarm->CreateRenderTargetView(rtWarm.Get(), nullptr, &rtvWarm))
                && SUCCEEDED(d3dWarm->CreateTexture2D(&stDesc, nullptr, &stagingWarm));
      Check(ready, "second device: shaders, input layout, vertices and target");

      if (ready) {
        ID3D11Buffer* vbsWarm[] = { vbWarm.Get() };
        ctxWarm->ClearRenderTargetView(rtvWarm.Get(), clear);
        ctxWarm->IASetInputLayout(layoutWarm.Get());
        ctxWarm->IASetVertexBuffers(0u, 1u, vbsWarm, &stride, &offset);
        ctxWarm->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctxWarm->VSSetShader(vsWarm.Get(), nullptr, 0u);
        ctxWarm->PSSetShader(psWarm.Get(), nullptr, 0u);
        ctxWarm->RSSetViewports(1u, &viewport);
        ctxWarm->OMSetRenderTargets(1u, rtvWarm.GetAddressOf(), nullptr);
        ctxWarm->Draw(3u, 0u);
        CheckHr(engineWarm->SubmitForPresent(rtWarm.Get(), 0u), "second device: draw and SubmitForPresent");
        ctxWarm->CopyResource(stagingWarm.Get(), rtWarm.Get());

        if (CheckHr(ctxWarm->Map(stagingWarm.Get(), 0u, D3D11_MAP_READ, 0u, &mapped), "second device: Map staging")) {
          Check(similar(pixel(W - 1u, H - 1u), cr, cg, cb, 1) && pixel(0u, 0u)[0] > 230 && pixel(0u, 0u)[1] < 25,
            "second device: the same clear and triangle from the cached shaders");
          ctxWarm->Unmap(stagingWarm.Get(), 0u);
        }
      }

      vsWarm.Reset();
      psWarm.Reset();
      layoutWarm.Reset();
      vbWarm.Reset();
      rtvWarm.Reset();
      rtWarm.Reset();
      stagingWarm.Reset();
      ctxWarm.Reset();
      d3dWarm.Reset();
      Check(engineWarm->Release() == 0u, "second device: final Release reports no leaked D3D11 references");
    }

    printShaderLines(warm);

    const long long hits = StatValue(warm.lines, "Shader cache closed:", "hits");
    const long long misses = StatValue(warm.lines, "Shader cache closed:", "misses");
    const long long translatedAfter = StatValue(warm.lines, "amdgpu_wddm_dxvk: Shader stats:", "translated");

    Check(hits >= 2 && misses == 0, "second device: both shaders come from the shader cache");
    Check(translatedBefore >= 0 && translatedAfter == translatedBefore, "second device: no shader translated");
    Check(warm.foreignLogs.load() == 0u && warm.foreignCalls.load() == 0u,
      "second device: every Log and QueueLock call came from the calling thread (E2)");
    Check(EngineThreadsSince(threadsBefore).empty(), "no thread of amdgpu_wddm_dxvk.dll outlives the second device");
    DeleteCriticalSection(&warm.queueLock);
  }

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

  if (linImage)
    vk.destroyImage(device, linImage, nullptr);

  if (linMemory)
    vk.freeMemory(device, linMemory, nullptr);

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
