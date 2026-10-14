// PROVENANCE: DXVK fork D-Ogi/dxvk, MIT; process policy derived from 95bcef23.
#include <array>
#include <vector>

#include <windows.h>
#include <shlobj.h>

#include "shader_cache_policy.h"
#include "../util/util_env.h"
#include "../util/util_string.h"

namespace dxvk::ddi {
  namespace {
    struct TokenHandle {
      HANDLE value = nullptr;
      ~TokenHandle() { if (value) CloseHandle(value); }
    };

    struct ProfilePath {
      PWSTR value = nullptr;
      ~ProfilePath() { CoTaskMemFree(value); }
    };

    const char* ThreadRulesOutCache() {
      TokenHandle thread;
      if (OpenThreadToken(GetCurrentThread(), TOKEN_QUERY, TRUE, &thread.value))
        return "thread impersonation";
      // Access denied, including an identification token, is not proof of no impersonation.
      return GetLastError() == ERROR_NO_TOKEN ? nullptr : "thread token not readable";
    }

    bool TokenDword(HANDLE token, TOKEN_INFORMATION_CLASS info, DWORD& value) {
      DWORD size = 0;
      return GetTokenInformation(token, info, &value, sizeof(value), &size) && size == sizeof(value);
    }

    const char* TokenRulesOutCache(HANDLE token) {
      DWORD size = 0;
      alignas(TOKEN_USER) std::array<BYTE, sizeof(TOKEN_USER) + SECURITY_MAX_SID_SIZE> user = { };
      if (!GetTokenInformation(token, TokenUser, user.data(), DWORD(user.size()), &size))
        return "token user not readable";

      PSID sid = reinterpret_cast<const TOKEN_USER*>(user.data())->User.Sid;
      if (!IsValidSid(sid))
        return "invalid token user";
      const BYTE* authority = GetSidIdentifierAuthority(sid)->Value;
      const UCHAR count = *GetSidSubAuthorityCount(sid);
      const DWORD first = count ? *GetSidSubAuthority(sid, 0) : 0;
      const bool zeroHigh = !authority[0] && !authority[1] && !authority[2] && !authority[3] && !authority[4];
      // Local/domain S-1-5-21-A-B-C-RID and Entra S-1-12-1-A-B-C-D. DWM, UMFD and service SIDs fail.
      if (count != 5 || !zeroHigh || !((authority[5] == 5 && first == SECURITY_NT_NON_UNIQUE)
                                  || (authority[5] == 12 && first == 1)))
        return "not an ordinary user account";

      DWORD appContainer = 0, session = 0;
      if (!TokenDword(token, TokenIsAppContainer, appContainer) || appContainer)
        return "AppContainer or unreadable token";
      if (!TokenDword(token, TokenSessionId, session) || !session)
        return "service session or unreadable token";

      // Unlike IsTokenRestricted, this query can distinguish an empty SID list from query failure.
      // TokenHasRestrictions also includes ordinary UAC-filtered logons and is not a sandbox test.
      TOKEN_GROUPS restricted = { };
      if (!GetTokenInformation(token, TokenRestrictedSids, &restricted, sizeof(restricted), &size)
       || restricted.GroupCount)
        return "restricted token or unreadable restrictions";

      size = 0;
      if (GetTokenInformation(token, TokenGroups, nullptr, 0, &size)
       || GetLastError() != ERROR_INSUFFICIENT_BUFFER || size < sizeof(TOKEN_GROUPS) || size > 65536)
        return "token groups not readable";
      std::vector<BYTE> groupsBuffer(size);
      if (!GetTokenInformation(token, TokenGroups, groupsBuffer.data(), size, &size))
        return "token groups not readable";
      const auto* groups = reinterpret_cast<const TOKEN_GROUPS*>(groupsBuffer.data());
      for (DWORD i = 0; i < groups->GroupCount; i++) {
        if (IsWellKnownSid(groups->Groups[i].Sid, WinServiceSid))
          return "service logon";
      }

      alignas(TOKEN_MANDATORY_LABEL) std::array<BYTE, sizeof(TOKEN_MANDATORY_LABEL) + SECURITY_MAX_SID_SIZE> label = { };
      if (!GetTokenInformation(token, TokenIntegrityLevel, label.data(), DWORD(label.size()), &size))
        return "integrity level not readable";
      sid = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(label.data())->Label.Sid;
      if (!IsValidSid(sid) || *GetSidSubAuthorityCount(sid) != 1)
        return "invalid integrity label";
      const DWORD integrity = *GetSidSubAuthority(sid, 0);
      if (integrity < SECURITY_MANDATORY_MEDIUM_RID || integrity >= SECURITY_MANDATORY_SYSTEM_RID)
        return "integrity level outside user range";
      return nullptr;
    }
  }

  bool ShaderCacheTokenAllowed(HANDLE token) noexcept {
    try {
      return !TokenRulesOutCache(token);
    } catch (...) {
      return false;
    }
  }

  bool ShaderCacheAccessAllowed() noexcept {
    try {
      if (ThreadRulesOutCache())
        return false;
      TokenHandle token;
      return OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value)
          && ShaderCacheTokenAllowed(token.value);
    } catch (...) {
      return false;
    }
  }

  static ShaderCachePolicy QueryShaderCachePolicy() {
    ShaderCachePolicy policy;
    if (const char* reason = ThreadRulesOutCache()) {
      policy.reason = reason;
      return policy;
    }
    TokenHandle token;
    // SHGetKnownFolderPath's explicit-user contract requires QUERY and IMPERSONATE.
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY | TOKEN_IMPERSONATE, &token.value)) {
      policy.reason = "process profile token not readable";
      return policy;
    }
    if (const char* reason = TokenRulesOutCache(token.value)) {
      policy.reason = reason;
      return policy;
    }

#ifdef AMDGPU_WDDM_LAB_FLAVOUR
    if (env::getEnvVar("DXVK_SHADER_CACHE") == "0") {
      policy.reason = "cache disabled by lab option";
      return policy;
    }
    policy.directory = env::getEnvVar("DXVK_SHADER_CACHE_PATH");
#endif
    if (policy.directory.empty()) {
      ProfilePath directory;
      // No environment fallback and no KF_FLAG_CREATE: a failed profile query disables the cache.
      const HRESULT result = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DONT_VERIFY, token.value, &directory.value);
      if (SUCCEEDED(result) && directory.value && *directory.value)
        policy.directory = str::fromws(directory.value) + "\\amdgpu-wddm\\dxvk";
      if (policy.directory.empty()) {
        policy.reason = "profile cache directory unavailable";
        return policy;
      }
    }
    policy.enable = true;
    return policy;
  }

  ShaderCachePolicy GetShaderCachePolicy() noexcept {
    try {
      return QueryShaderCachePolicy();
    } catch (...) {
      // The cache is optional even if a token query or path conversion runs out of host memory.
      return { };
    }
  }
}
