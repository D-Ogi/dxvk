// amdgpu-wddm: the machine-wide profile file of DXVK options (docs/design/app-profiles.md in amdgpu-wddm).
//
//   <system drive>\ProgramData\amdgpu-wddm\profiles\dxvk.conf
//
// The file has the format of dxvk.conf, with one rule more: every option is in an [executable] section, because an
// option of this file applies in every process of the machine. Precedence, highest first: the user configuration
// (DXVK_CONFIG, which also carries the per-application settings of the driver's control application, then dxvk.conf
// of the working directory), this file, the built-in profiles (DxvkInstance).
//
// The file is checked whole before any of it applies. A line that the dxvk.conf parser would skip without a word, an
// option outside a section, a section without a name and an unclosed quote refuse the whole file, with one log line.
// The directory and the file must be owned by SYSTEM, Administrators or TrustedInstaller, no other account may change
// them, and neither may be a reparse point. Only the lab flavour takes the directory from AMDGPU_WDDM_PROFILE_DIR
// (BD-167), and only the lab flavour logs each option that the file sets.

#include "config.h"

#include "../log/log.h"
#include "../util_env.h"
#include "../util_string.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#pragma comment(lib, "advapi32.lib")
#endif

namespace dxvk {

  namespace {

    bool isProfileSpace(char ch) {
      return ch == ' ' || ch == '\t' || ch == '\r';
    }

    bool isProfileKeyChar(char ch) {
      return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '.' || ch == '_';
    }

  }


  bool Config::parseProfileConfig(
    const std::string&                          text,
    const std::string&                          exeName,
          Config&                               config,
          std::string&                          error,
          std::vector<std::pair<uint32_t, std::string>>* applied) {
    struct Entry { uint32_t line; std::string key; std::string value; };
    std::vector<Entry> entries;
    bool inSection = false;
    bool active = false;
    uint32_t number = 0;
    size_t start = 0;

    // A UTF-8 byte order mark, as an editor may write one.
    if (text.compare(0, 3, "\xef\xbb\xbf") == 0)
      start = 3;

    while (start <= text.size()) {
      size_t end = text.find('\n', start);
      if (end == std::string::npos)
        end = text.size();
      std::string line = text.substr(start, end - start);
      start = end + 1;
      number += 1;

      size_t n = 0;
      while (n < line.size() && isProfileSpace(line[n]))
        n += 1;
      size_t e = line.size();
      while (e > n && isProfileSpace(line[e - 1]))
        e -= 1;

      if (n == e || line[n] == '#')
        continue;

      if (line[n] == '[') {
        if (line[e - 1] != ']' || e - n < 3) {
          error = str::format("line ", number, ": a section header must be [executable name]");
          return false;
        }
        std::string name = line.substr(n + 1, e - n - 2);
        if (name.find_first_of("[]\\/") != std::string::npos) {
          error = str::format("line ", number, ": a section name is a file name such as game.exe, without a path");
          return false;
        }
        inSection = true;
        active = name == exeName;
        continue;
      }

      size_t k = n;
      while (k < e && isProfileKeyChar(line[k]))
        k += 1;
      if (k == n) {
        error = str::format("line ", number, ": not an option, a section header or a comment");
        return false;
      }
      std::string key = line.substr(n, k - n);
      while (k < e && isProfileSpace(line[k]))
        k += 1;
      if (k >= e || line[k] != '=') {
        error = str::format("line ", number, ": the option ", key, " has no '='");
        return false;
      }
      k += 1;
      while (k < e && isProfileSpace(line[k]))
        k += 1;

      // The value as dxvk.conf reads it: up to white space outside quotes, the quotes removed.
      std::string value;
      bool quoted = false;
      for (; k < e; k++) {
        if (!quoted && isProfileSpace(line[k]))
          break;
        if (line[k] == '"')
          quoted = !quoted;
        else
          value += line[k];
      }
      if (quoted) {
        error = str::format("line ", number, ": the value of ", key, " has no closing quote");
        return false;
      }
      while (k < e && isProfileSpace(line[k]))
        k += 1;
      if (k < e && line[k] != '#') {
        error = str::format("line ", number, ": text after the value of ", key);
        return false;
      }
      if (!inSection) {
        error = str::format("line ", number, ": the option ", key,
          " is outside an [executable] section, so it would apply to every application");
        return false;
      }
      if (active)
        entries.push_back({ number, key, value });
    }

    for (const auto& entry : entries) {
      config.setOption(entry.key, entry.value);
      if (applied)
        applied->push_back({ entry.line, str::format(entry.key, " = ", entry.value) });
    }
    return true;
  }


#ifdef _WIN32

  namespace {

    bool g_profileTestTrustAll = false;

    bool isTrustedSid(PSID sid) {
      static const struct {
        BYTE revision, count;
        SID_IDENTIFIER_AUTHORITY authority;
        DWORD sub[6];
      } trustedInstaller = { SID_REVISION, 6, { SECURITY_NT_AUTHORITY },
        { 80, 956008885, 3418522649u, 1831038044, 1853292631, 2271478464u } };
      return IsWellKnownSid(sid, WinLocalSystemSid) || IsWellKnownSid(sid, WinBuiltinAdministratorsSid)
          || EqualSid(sid, PSID(&trustedInstaller));
    }

    std::string sidText(PSID sid) {
      char* text = nullptr;
      if (!ConvertSidToStringSidA(sid, &text))
        return "?";
      std::string result = text;
      LocalFree(text);
      return result;
    }

  }


  bool Config::isTrustedProfileDescriptor(void* securityDescriptor, std::string& why) {
    // The rights that change the content of a file or the entries of a directory, or the access rules.
    const DWORD write = FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_DELETE_CHILD | DELETE | WRITE_DAC | WRITE_OWNER
                      | GENERIC_WRITE | GENERIC_ALL | MAXIMUM_ALLOWED;
    PSECURITY_DESCRIPTOR sd = securityDescriptor;
    PSID owner = nullptr;
    BOOL defaulted = FALSE, present = FALSE;
    PACL dacl = nullptr;

    if (!GetSecurityDescriptorOwner(sd, &owner, &defaulted) || !owner) {
      why = "it has no owner";
      return false;
    }
    if (!isTrustedSid(owner)) {
      why = str::format("its owner ", sidText(owner), " is not SYSTEM, Administrators or TrustedInstaller");
      return false;
    }
    if (!GetSecurityDescriptorDacl(sd, &present, &dacl, &defaulted) || !present || !dacl) {
      why = "it has no access list, so every account may change it";
      return false;
    }
    for (DWORD i = 0; i < dacl->AceCount; i++) {
      ACE_HEADER* ace = nullptr;
      if (!GetAce(dacl, i, reinterpret_cast<void**>(&ace))) {
        why = "its access list cannot be read";
        return false;
      }
      if (ace->AceFlags & INHERIT_ONLY_ACE)
        continue;
      if (ace->AceType == ACCESS_ALLOWED_OBJECT_ACE_TYPE || ace->AceType == ACCESS_ALLOWED_CALLBACK_OBJECT_ACE_TYPE) {
        why = "its access list has an object access entry";
        return false;
      }
      if (ace->AceType != ACCESS_ALLOWED_ACE_TYPE && ace->AceType != ACCESS_ALLOWED_CALLBACK_ACE_TYPE)
        continue;   // deny, audit and label entries give no access
      // ACCESS_ALLOWED_CALLBACK_ACE starts as ACCESS_ALLOWED_ACE does.
      auto allowed = reinterpret_cast<ACCESS_ALLOWED_ACE*>(ace);
      PSID who = PSID(&allowed->SidStart);
      if (!(allowed->Mask & write) || isTrustedSid(who)
       || IsWellKnownSid(who, WinCreatorOwnerRightsSid) || IsWellKnownSid(who, WinCreatorOwnerSid))
        continue;
      why = str::format("it gives write access to ", sidText(who));
      return false;
    }
    return true;
  }


  namespace {

    bool isTrustedHandle(HANDLE handle, std::string& why) {
      if (g_profileTestTrustAll)
        return true;
      PSECURITY_DESCRIPTOR sd = nullptr;
      DWORD status = GetSecurityInfo(handle, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        nullptr, nullptr, nullptr, nullptr, &sd);
      if (status != ERROR_SUCCESS) {
        why = str::format("its access rules cannot be read (error ", status, ")");
        return false;
      }
      bool ok = Config::isTrustedProfileDescriptor(sd, why);
      LocalFree(sd);
      return ok;
    }

    // "<system drive>\ProgramData\amdgpu-wddm\profiles". Microsoft does not support another place for ProgramData,
    // and the drive comes from the system, not from the environment of the process.
    std::wstring profileRoot() {
#ifdef AMDGPU_WDDM_LAB_FLAVOUR
      // BD-167: only a lab build takes the root from the environment.
      std::wstring root(MAX_PATH, L'\0');
      DWORD n = GetEnvironmentVariableW(L"AMDGPU_WDDM_PROFILE_DIR", root.data(), MAX_PATH);
      if (n && n < MAX_PATH) {
        root.resize(n);
        return root;
      }
#endif
      wchar_t windows[MAX_PATH];
      UINT length = GetSystemWindowsDirectoryW(windows, MAX_PATH);
      if (!length || length >= MAX_PATH || windows[1] != L':')
        return std::wstring();
      return std::wstring(1, windows[0]) + L":\\ProgramData\\amdgpu-wddm\\profiles";
    }

    // false: absent (no log line) or refused (one log line).
    bool checkDirectory(const std::wstring& path) {
      std::string why;
      HANDLE handle = CreateFileW(path.c_str(), READ_CONTROL | FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
      if (handle == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
          Logger::warn(str::format("Profile directory ", str::fromws(path.c_str()), " is not read: it cannot be opened (error ", error, ")"));
        return false;
      }
      BY_HANDLE_FILE_INFORMATION info;
      bool ok = GetFileInformationByHandle(handle, &info);
      if (!ok)
        why = "its attributes cannot be read";
      else if (!(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
        why = "it is not a directory", ok = false;
      else if (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
        why = "it is a reparse point (a link or a junction)", ok = false;
      else
        ok = isTrustedHandle(handle, why);
      CloseHandle(handle);
      if (!ok)
        Logger::warn(str::format("Profile directory ", str::fromws(path.c_str()), " is not read: ", why));
      return ok;
    }

    // false: absent (no log line) or refused (one log line).
    bool readFile(const std::wstring& path, std::string& text) {
      constexpr DWORD MaxSize = 1u << 20;
      std::string why;
      HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ | READ_CONTROL, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
      if (handle == INVALID_HANDLE_VALUE) {
        DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
          Logger::warn(str::format("Profile file ", str::fromws(path.c_str()), " is not read: it cannot be opened (error ", error, ")"));
        return false;
      }
      BY_HANDLE_FILE_INFORMATION info;
      bool ok = GetFileInformationByHandle(handle, &info);
      if (!ok)
        why = "its attributes cannot be read";
      else if (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
        why = "it is not a regular file", ok = false;
      else if (info.nFileSizeHigh || info.nFileSizeLow > MaxSize)
        why = str::format("it is larger than ", MaxSize, " bytes"), ok = false;
      else
        ok = isTrustedHandle(handle, why);
      if (ok) {
        DWORD done = 0;
        text.resize(info.nFileSizeLow);
        if (info.nFileSizeLow && (!ReadFile(handle, text.data(), info.nFileSizeLow, &done, nullptr) || done != info.nFileSizeLow))
          why = "it cannot be read", ok = false;
      }
      CloseHandle(handle);
      if (!ok)
        Logger::warn(str::format("Profile file ", str::fromws(path.c_str()), " is not read: ", why));
      return ok;
    }

  }


  Config Config::getProfileConfigFrom(const std::wstring& root, const std::string& exeName, bool trustAll) {
    Config config;
    g_profileTestTrustAll = trustAll;
    if (root.empty() || !checkDirectory(root))
      return config;
    std::wstring path = root + L"\\dxvk.conf";
    std::string text;
    if (!readFile(path, text))
      return config;

    std::string name = str::fromws(path.c_str());
    std::string error;
    std::vector<std::pair<uint32_t, std::string>> applied;
    if (!parseProfileConfig(text, exeName, config, error, &applied)) {
      Logger::warn(str::format("Profile file ", name, " is not read: ", error));
      return Config();
    }
    if (!applied.empty()) {
      Logger::info(str::format("Found profile file: ", name));
#ifdef AMDGPU_WDDM_LAB_FLAVOUR
      for (const auto& entry : applied)
        Logger::info(str::format("  line ", entry.first, ": ", entry.second));
#endif
    }
    return config;
  }


  Config Config::getProfileConfig() {
    return getProfileConfigFrom(profileRoot(), env::getExeName(), false);
  }

#else

  Config Config::getProfileConfig() {
    return Config();
  }

#endif

  Config Config::getProcessConfig(const std::string& exePath) {
    // merge() keeps the options that are already set, so the first source wins.
    Config config = getUserConfig();
    config.merge(getProfileConfig());
    config.merge(getAppConfig(exePath));
    return config;
  }

}
