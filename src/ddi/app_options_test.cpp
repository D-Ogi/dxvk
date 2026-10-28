// Host test of Bc250DxvkEngineGetAppOptions (bc250_dxvk_engine.h, ABI 1.5) of amdgpu_wddm_dxvk.dll. No GPU and no
// Vulkan: the export reads DXVK's configuration only. It uses the ABI header alone, like engine_test.cpp.
//
//   amdgpu_wddm_dxvk_app_options_test.exe <amdgpu_wddm_dxvk.dll> [--negative-control]
//
// The cases: the argument checks; the options of DXVK_CONFIG, sorted, dxvk.maxFrameRate included and other dxvk.*
// options left out; a buffer that is too short; the app profile of P3R.exe (dxgi.syncInterval = 1) in a copy of the
// test named P3R.exe; the user's value over the profile's; and the same profile for a copy in a directory deeper
// than MAX_PATH (env::getExePath at any path length). Run it in a directory without a dxvk.conf. A host that cannot
// start a process from a long path skips that case. The negative control inverts every expectation and must fail
// every case.

#include "bc250_dxvk_engine.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

  int  failures = 0;
  int  checks   = 0;
  bool invert   = false;

  std::wstring g_dll;
  PFN_BC250_DXVK_ENGINE_GET_APP_OPTIONS g_get = nullptr;

#define CHECK(cond)                                                     \
  do {                                                                  \
    checks++;                                                           \
    if (!(cond) != invert) {                                            \
      failures++;                                                       \
      std::printf("FAIL %s:%d: %s\n", __func__, __LINE__, #cond);       \
    }                                                                   \
  } while (0)

  std::string Options() {
    char text[4096];
    UINT32 length = 0;
    HRESULT hr = g_get(BC250_DXVK_ENGINE_ABI_VERSION, text, sizeof(text), &length);
    return SUCCEEDED(hr) ? std::string(text) : std::string("error");
  }


  void TestArguments() {
    char text[16];
    UINT32 length = 0;
    CHECK(g_get(BC250_DXVK_ENGINE_ABI_VERSION, text, sizeof(text), nullptr) == E_INVALIDARG);
    CHECK(g_get(BC250_DXVK_ENGINE_ABI_VERSION, nullptr, sizeof(text), &length) == E_INVALIDARG);
    CHECK(g_get(0x00020000u, text, sizeof(text), &length) == E_NOINTERFACE);
  }


  void TestUserConfig() {
    SetEnvironmentVariableA("DXVK_CONFIG",
      "dxgi.syncInterval = 0; d3d11.samplerAnisotropy = 16; dxvk.maxFrameRate = 50; dxgi.customVendorId = 10de");
    CHECK(Options() == "dxgi.customVendorId=10de\ndxgi.syncInterval=0\ndxvk.maxFrameRate=50\n");
    SetEnvironmentVariableA("DXVK_CONFIG", nullptr);
    CHECK(Options() == "");
  }


  void TestShortBuffer() {
    SetEnvironmentVariableA("DXVK_CONFIG", "dxgi.syncInterval = 0; dxgi.maxFrameLatency = 1");
    char text[24];
    std::memset(text, 'x', sizeof(text));
    UINT32 length = 0;
    // "dxgi.maxFrameLatency=1\n" is 23 bytes and fits with its zero; the second line does not.
    CHECK(g_get(BC250_DXVK_ENGINE_ABI_VERSION, text, sizeof(text), &length) ==
          HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER));
    CHECK(length == 43u && std::string(text) == "dxgi.maxFrameLatency=1\n");
    CHECK(g_get(BC250_DXVK_ENGINE_ABI_VERSION, nullptr, 0, &length) ==
          HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER) && length == 43u);
    SetEnvironmentVariableA("DXVK_CONFIG", nullptr);
  }


  // Runs a copy of this test at copy (created and removed here) with the child mode; true when it exits with 0.
  // skipped: the host could not start a process from that path.
  bool RunCopy(const std::wstring& copy, const wchar_t* expect, bool& skipped) {
    wchar_t self[32768];
    skipped = false;
    DWORD n = GetModuleFileNameW(nullptr, self, 32768);
    if (!n || n >= 32768 || !CopyFileW(self, copy.c_str(), FALSE))
      return false;
    std::wstring cmd = L"\"" + copy + L"\" --child \"" + g_dll + L"\" \"" + expect + L"\"";
    STARTUPINFOW si = { sizeof(si) };
    PROCESS_INFORMATION pi = { };
    DWORD code = 1;
    if (CreateProcessW(copy.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
      WaitForSingleObject(pi.hProcess, 30000);
      GetExitCodeProcess(pi.hProcess, &code);
      CloseHandle(pi.hThread);
      CloseHandle(pi.hProcess);
    } else {
      std::printf("SKIP: CreateProcessW error %lu\n", GetLastError());
      skipped = true;
    }
    DeleteFileW(copy.c_str());
    return code == 0;
  }


  std::wstring TestDir() {
    wchar_t self[32768];
    DWORD n = GetModuleFileNameW(nullptr, self, 32768);
    std::wstring dir(self, n);
    return L"\\\\?\\" + dir.substr(0, dir.find_last_of(L'\\')) + L"\\app_options_test";
  }


  void TestProfile() {
    std::wstring dir = TestDir();
    CreateDirectoryW(dir.c_str(), nullptr);
    bool skipped = false;
    CHECK(RunCopy(dir + L"\\P3R.exe", L"dxgi.syncInterval=1\\n", skipped));
    // The user's option wins over the profile's, as in DxvkInstance.
    SetEnvironmentVariableA("DXVK_CONFIG", "dxgi.syncInterval = 0");
    CHECK(RunCopy(dir + L"\\P3R.exe", L"dxgi.syncInterval=0\\n", skipped));
    SetEnvironmentVariableA("DXVK_CONFIG", nullptr);
    // A name that no profile matches.
    CHECK(RunCopy(dir + L"\\NoProfileGame.exe", L"", skipped));
    RemoveDirectoryW(dir.c_str());
  }


  void TestLongPath() {
    std::wstring dir = TestDir();
    const size_t base = dir.size();
    std::wstring at = dir;
    CreateDirectoryW(at.c_str(), nullptr);
    unsigned depth = 0;
    while (at.size() < base + MAX_PATH) {
      at += L"\\directory_name_of_forty_characters_long";
      CreateDirectoryW(at.c_str(), nullptr);
      depth++;
    }
    bool skipped = false;
    bool ok = RunCopy(at + L"\\P3R.exe", L"dxgi.syncInterval=1\\n", skipped);
    if (!skipped)
      CHECK(ok);
    for (; depth > 0; depth--) {
      RemoveDirectoryW(at.c_str());
      at.resize(at.find_last_of(L'\\'));
    }
    RemoveDirectoryW(at.c_str());
  }


  int Child(const char* expect) {
    std::string got = Options();
    for (char& c : got)
      c = c == '\n' ? '|' : c;
    std::string want = expect;
    for (char& c : want)
      c = c == '\n' ? '|' : c;
    std::printf("child: options \"%s\", expected \"%s\"\n", got.c_str(), want.c_str());
    return got == want ? 0 : 1;
  }

}


int main(int argc, char** argv) {
  if (argc < 2) {
    std::printf("usage: amdgpu_wddm_dxvk_app_options_test <amdgpu_wddm_dxvk.dll> [--negative-control]\n");
    return 2;
  }

  bool child = argc >= 4 && !std::strcmp(argv[1], "--child");
  const char* dll = child ? argv[2] : argv[1];
  wchar_t wide[32768];
  if (!MultiByteToWideChar(CP_ACP, 0, dll, -1, wide, 32768))
    return 2;
  wchar_t full[32768];
  if (!GetFullPathNameW(wide, 32768, full, nullptr))
    return 2;
  g_dll = full;

  HMODULE module = LoadLibraryExW(g_dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  g_get = module ? reinterpret_cast<PFN_BC250_DXVK_ENGINE_GET_APP_OPTIONS>(
    GetProcAddress(module, BC250_DXVK_ENGINE_GET_APP_OPTIONS_NAME)) : nullptr;
  if (!g_get) {
    std::printf("FAIL: %s has no %s (error %lu)\n", dll, BC250_DXVK_ENGINE_GET_APP_OPTIONS_NAME, GetLastError());
    return 1;
  }

  if (child) {
    std::string expect = argv[3];
    // The command line cannot carry a line break: the parent passes the lines with a trailing "\n" as text.
    for (size_t at; (at = expect.find("\\n")) != std::string::npos; )
      expect.replace(at, 2, "\n");
    return Child(expect.c_str());
  }

  invert = argc > 2 && !std::strcmp(argv[2], "--negative-control");
  SetEnvironmentVariableA("DXVK_CONFIG", nullptr);

  static const struct { const char* name; void (*fn)(); } tests[] = {
    { "arguments",    TestArguments  },
    { "user_config",  TestUserConfig },
    { "short_buffer", TestShortBuffer },
    { "profile",      TestProfile    },
    { "long_path",    TestLongPath   },
  };

  int failedTests = 0;
  for (auto& t : tests) {
    int before = failures;
    t.fn();
    bool ok = failures == before;
    failedTests += !ok;
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", t.name);
  }
  std::printf("app options: %u tests, %d checks, %d failed checks, %u-bit image%s\n",
    unsigned(sizeof(tests) / sizeof(tests[0])), checks, failures, unsigned(sizeof(void*) * 8),
    invert ? " (negative control)" : "");
  return failedTests ? 1 : 0;
}
