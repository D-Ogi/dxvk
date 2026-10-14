#pragma once

#include <string>
#include <windows.h>

namespace dxvk::ddi {

  struct ShaderCachePolicy {
    bool enable = false;
    std::string directory;
    std::string reason;
  };

  // Query only: no directory is created. Do not memoize an effective-token decision across calls.
  ShaderCachePolicy GetShaderCachePolicy() noexcept;

  // Check before each cache I/O transaction, including access through already-open file handles.
  // An impersonating thread cannot use the process profile's cached permissions.
  bool ShaderCacheAccessAllowed() noexcept;

  // Process-token half of the policy, shared with host tests that supply real restricted tokens.
  // This alone does not authorize cache I/O: ShaderCacheAccessAllowed also checks the current thread.
  bool ShaderCacheTokenAllowed(HANDLE token) noexcept;

}
