// amdgpu-wddm: host test of the machine profile file (config_profile.cpp). Built and run by config_profile_test.py.
//   1. what the profile parser reads and what it refuses;
//   2. a section applies to its executable only, and the precedence of DxvkInstance: the user configuration
//      (DXVK_CONFIG) over the profile file over the built-in profiles;
//   3. the file of a directory, and the access rule: a directory that the test user owns is refused.
#include <cstdio>
#include <string>

#include <windows.h>
#include <sddl.h>

#include "config.h"
#include "../log/log.h"

namespace dxvk {
  Logger Logger::s_instance("config_profile_test.log");
}

using namespace dxvk;

static int g_failures = 0;

#define CHECK(cond, what) do { if (!(cond)) { g_failures++; std::printf("FAIL line %d: %s\n", __LINE__, what); } } while (0)

static bool parses(const std::string& text, const std::string& exe, Config& config, std::string& error) {
  config = Config();
  error.clear();
  return Config::parseProfileConfig(text, exe, config, error, nullptr);
}

static void testParser() {
  Config c;
  std::string e;

  const char* good =
    "\xef\xbb\xbf# DXVK profiles\r\n"
    "\r\n"
    "[game.exe]\r\n"
    "d3d11.samplerAnisotropy = 8\r\n"
    "dxgi.maxFrameRate = \"60\"   # comment\r\n"
    "  d3d11.cachedDynamicResources=vi\r\n"
    "[other.exe]\r\n"
    "d3d11.samplerAnisotropy = 2\r\n"
    "[game.exe]\r\n"
    "d3d11.samplerAnisotropy = 16\r\n";
  CHECK(parses(good, "game.exe", c, e), e.c_str());
  CHECK(c.getOption<int32_t>("d3d11.samplerAnisotropy", -1) == 16, "a later section line wins");
  CHECK(c.getOption<int32_t>("dxgi.maxFrameRate", -1) == 60, "quoted value");
  CHECK(c.getOption<std::string>("d3d11.cachedDynamicResources", "") == "vi", "value without spaces");
  CHECK(parses(good, "other.exe", c, e) && c.getOption<int32_t>("d3d11.samplerAnisotropy", -1) == 2, "other section");
  CHECK(parses(good, "Game.exe", c, e) && c.getOption<int32_t>("d3d11.samplerAnisotropy", -1) == -1,
    "the section name compares the case, as dxvk.conf does");
  CHECK(parses(good, "third.exe", c, e) && c.getOption<int32_t>("d3d11.samplerAnisotropy", -1) == -1, "no section");

  static const char* const bad[] = {
    "d3d11.samplerAnisotropy = 16\n",                       // outside a section: every application
    "[game.exe\nd3d11.samplerAnisotropy = 16\n",            // unclosed header
    "[]\nd3d11.samplerAnisotropy = 16\n",                   // empty name
    "[C:\\Games\\game.exe]\nd3d11.samplerAnisotropy = 16\n", // a path
    "[game.exe]\nd3d11.samplerAnisotropy 16\n",             // no '='
    "[game.exe]\n= 16\n",                                   // no key
    "[game.exe]\nd3d11.samplerAnisotropy = \"16\n",         // unclosed quote
    "[game.exe]\nd3d11.samplerAnisotropy = 16 8\n",         // text after the value
    "[game.exe]\n;comment\n",                               // not a comment of dxvk.conf
  };
  for (auto text : bad) {
    bool ok = parses(text, "game.exe", c, e);
    CHECK(!ok && !e.empty(), text);
    CHECK(c.getOption<int32_t>("d3d11.samplerAnisotropy", -1) == -1, "a refused file sets nothing");
  }
  // The refusal names the line, and a valid section before the error gives nothing.
  CHECK(!parses("[game.exe]\nd3d11.samplerAnisotropy = 4\nbroken line\n", "game.exe", c, e)
     && e.rfind("line 3:", 0) == 0 && c.getOption<int32_t>("d3d11.samplerAnisotropy", -1) == -1, e.c_str());
}

static void testPrecedence() {
  // As DxvkInstance: user, then profile, then built-in.
  Config profile;
  std::string e;
  CHECK(Config::parseProfileConfig(
    "[Anno1800.exe]\n"
    "d3d11.cachedDynamicResources = a\n"
    "d3d11.samplerAnisotropy = 4\n"
    "d3d11.maxFeatureLevel = 11_1\n", "Anno1800.exe", profile, e, nullptr), e.c_str());

  SetEnvironmentVariableA("DXVK_CONFIG", "d3d11.samplerAnisotropy = 16");
  Config config = Config::getUserConfig();
  SetEnvironmentVariableA("DXVK_CONFIG", nullptr);
  config.merge(profile);
  Config builtin = Config::getAppConfig("C:\\Games\\Anno\\Anno1800.exe");
  CHECK(builtin.getOption<std::string>("d3d11.cachedDynamicResources", "") == "c", "the built-in profile of Anno 1800");
  config.merge(builtin);
  CHECK(config.getOption<int32_t>("d3d11.samplerAnisotropy", -1) == 16, "the user configuration wins over the profile");
  CHECK(config.getOption<std::string>("d3d11.cachedDynamicResources", "") == "a", "the profile wins over the built-in profile");
  CHECK(config.getOption<std::string>("d3d11.maxFeatureLevel", "") == "11_1", "a profile option of its own");

  // getProcessConfig, the chain of DxvkInstance and of the engine export. This host has no machine profile file.
  SetEnvironmentVariableA("DXVK_CONFIG", "d3d11.samplerAnisotropy = 16");
  config = Config::getProcessConfig("C:\\Games\\Anno\\Anno1800.exe");
  SetEnvironmentVariableA("DXVK_CONFIG", nullptr);
  CHECK(config.getOption<int32_t>("d3d11.samplerAnisotropy", -1) == 16, "getProcessConfig: the user configuration");
  CHECK(config.getOption<std::string>("d3d11.cachedDynamicResources", "") == "c", "getProcessConfig: the built-in profile");
}

static int trusted(const char* sddl) {
  PSECURITY_DESCRIPTOR sd = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorA(sddl, SDDL_REVISION_1, &sd, nullptr))
    return -1;
  std::string why;
  bool ok = Config::isTrustedProfileDescriptor(sd, why);
  LocalFree(sd);
  return ok;
}

static void testFiles(const std::wstring& root) {
  CreateDirectoryW(root.c_str(), nullptr);
  std::wstring path = root + L"\\dxvk.conf";
  FILE* f = _wfopen(path.c_str(), L"wb");
  CHECK(f != nullptr, "write the test file");
  if (!f)
    return;
  std::fputs("[game.exe]\r\nd3d11.samplerAnisotropy = 8\r\n", f);
  std::fclose(f);

  Config c = Config::getProfileConfigFrom(root, "game.exe", true);
  CHECK(c.getOption<int32_t>("d3d11.samplerAnisotropy", -1) == 8, "the file of the directory applies");
  c = Config::getProfileConfigFrom(root, "other.exe", true);
  CHECK(c.getOption<int32_t>("d3d11.samplerAnisotropy", -1) == -1, "not to another executable");
  c = Config::getProfileConfigFrom(root, "game.exe", false);
  CHECK(c.getOption<int32_t>("d3d11.samplerAnisotropy", -1) == -1, "a directory of the test user is refused");
  c = Config::getProfileConfigFrom(root + L"\\absent", "game.exe", true);
  CHECK(c.getOption<int32_t>("d3d11.samplerAnisotropy", -1) == -1, "no directory");

  CHECK(trusted("O:BAG:SYD:PAI(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;0x1200a9;;;BU)(A;OICI;0x1200a9;;;AC)") == 1, "installer rule");
  CHECK(trusted("O:BAG:SYD:(A;OICIIO;FA;;;CO)(A;;FA;;;OW)(D;;FA;;;BU)(A;;FA;;;BA)") == 1, "templates and deny entries");
  CHECK(trusted("O:BUG:SYD:(A;;FA;;;BA)") == 0, "a user owner");
  CHECK(trusted("O:BAG:SYD:(A;;FA;;;BA)(A;;FRFW;;;BU)") == 0, "users may write");
  CHECK(trusted("O:BAG:SYD:NO_ACCESS_CONTROL") == 0, "no access list");
  CHECK(trusted("O:BAG:SYD:(A;CI;0x4;;;BU)") == 0, "users may add folders");
}

int main(int argc, char** argv) {
  if (argc != 2) {
    std::printf("usage: config_profile_test <empty test directory>\n");
    return 2;
  }
  wchar_t root[MAX_PATH];
  MultiByteToWideChar(CP_UTF8, 0, argv[1], -1, root, MAX_PATH);
  testParser();
  testPrecedence();
  testFiles(root);
  std::printf("%s: %d failure(s)\n", g_failures ? "FAIL" : "PASS", g_failures);
  return g_failures ? 1 : 0;
}
