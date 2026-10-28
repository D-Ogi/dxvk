// PROVENANCE: DXVK fork D-Ogi/dxvk, MIT.
//
// Host test of the directory limit of the D3D11 IR cache (src/dxvk/dxvk_shader_cache_quota.h).
// No file system, no GPU: the selection is a pure function of a file list.
// Prints "PASSED: 0 failure(s)" or "FAILED: <n> failure(s)" and returns 0 or 1.

#include <cstdio>
#include <string>
#include <vector>

#include "../dxvk/dxvk_shader_cache_quota.h"

namespace q = dxvk::shader_cache_quota;

static int g_failures = 0;

static void Check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "ok" : "FAIL", what);
  if (!ok) g_failures++;
}

static std::string Name(char exe, char build, const char* suffix) {
  return std::string(16u, exe) + "-" + std::string(16u, build) + suffix;
}

static bool Has(const std::vector<std::string>& list, const std::string& name) {
  for (const auto& entry : list)
    if (entry == name) return true;
  return false;
}

int main() {
  constexpr uint64_t MiB = 1ull << 20;

  Check(q::isCacheFileName(Name('a', '1', ".dxvk.lut")), "a cache look-up table name is accepted");
  Check(q::isCacheFileName(Name('0', 'f', ".dxvk.bin")), "a cache blob name is accepted");
  Check(!q::isCacheFileName(Name('A', '1', ".dxvk.lut")), "upper-case hex is refused");
  Check(!q::isCacheFileName(Name('a', '1', ".dxvk.tmp")), "another suffix is refused");
  Check(!q::isCacheFileName("notes.txt"), "an unrelated name is refused");
  Check(!q::isCacheFileName(std::string(16u, 'a') + "_" + std::string(16u, '1') + ".dxvk.lut"),
    "a name without the separator is refused");
  Check(!q::isCacheFileName(Name('a', '1', ".dxvk.lut") + "x"), "a longer name is refused");

  const std::string own = std::string(16u, 'a') + "-";

  // Under the limit: nothing goes.
  {
    std::vector<q::File> files = {
      { Name('a', '1', ".dxvk.bin"), 300 * MiB, 50 },
      { Name('b', '1', ".dxvk.bin"), 300 * MiB, 10 },
    };
    Check(q::selectDeletions(files, own, 1024 * MiB).empty(), "under the limit nothing is selected");
  }

  // Over the limit: the oldest other executable goes whole, the newer one stays.
  {
    std::vector<q::File> files = {
      { Name('a', '1', ".dxvk.lut"), 10 * MiB, 100 },
      { Name('a', '1', ".dxvk.bin"), 400 * MiB, 100 },
      { Name('b', '1', ".dxvk.lut"), 10 * MiB, 20 },
      { Name('b', '1', ".dxvk.bin"), 400 * MiB, 20 },
      { Name('c', '1', ".dxvk.lut"), 10 * MiB, 90 },
      { Name('c', '1', ".dxvk.bin"), 400 * MiB, 90 },
    };
    auto out = q::selectDeletions(files, own, 1024 * MiB);
    Check(out.size() == 2u && Has(out, Name('b', '1', ".dxvk.lut")) && Has(out, Name('b', '1', ".dxvk.bin")),
      "the executable used longest ago goes first, with both files");
    Check(!Has(out, Name('c', '1', ".dxvk.bin")), "a newer executable stays when the sum is under the limit");
  }

  // The running executable is never selected, even when it is the oldest and alone over the limit.
  {
    std::vector<q::File> files = {
      { Name('a', '1', ".dxvk.bin"), 2000 * MiB, 1 },
      { Name('a', '2', ".dxvk.bin"), 2000 * MiB, 1 },
    };
    Check(q::selectDeletions(files, own, 1024 * MiB).empty(), "the files of the running executable stay");
  }

  // Unrelated names are neither counted nor selected.
  {
    std::vector<q::File> files = {
      { "big-unrelated-file.bin", 5000 * MiB, 0 },
      { Name('b', '1', ".dxvk.bin"), 100 * MiB, 0 },
    };
    Check(q::selectDeletions(files, own, 1024 * MiB).empty(), "unrelated files do not count");
  }

  // The newest file of a group decides its age: an old blob with a fresh look-up table is recent.
  {
    std::vector<q::File> files = {
      { Name('b', '1', ".dxvk.bin"), 600 * MiB, 1 },
      { Name('b', '1', ".dxvk.lut"), 1 * MiB, 99 },
      { Name('c', '1', ".dxvk.bin"), 600 * MiB, 50 },
    };
    auto out = q::selectDeletions(files, own, 1024 * MiB);
    Check(out.size() == 1u && out[0] == Name('c', '1', ".dxvk.bin"), "a group is as new as its newest file");
  }

  // Several groups go until the sum is under the limit.
  {
    std::vector<q::File> files;
    for (char exe = 'b'; exe <= 'f'; exe++)
      files.push_back({ Name(exe, '1', ".dxvk.bin"), 400 * MiB, int64_t(exe) });
    auto out = q::selectDeletions(files, own, 1024 * MiB);
    Check(out.size() == 3u && Has(out, Name('b', '1', ".dxvk.bin")) && Has(out, Name('c', '1', ".dxvk.bin"))
      && Has(out, Name('d', '1', ".dxvk.bin")), "groups go oldest first until the sum is at the limit");
  }

  // The deletion count is bounded per start.
  {
    std::vector<q::File> files;
    for (char exe = 'b'; exe <= 'f'; exe++)
      files.push_back({ Name(exe, '1', ".dxvk.bin"), 400 * MiB, int64_t(exe) });
    auto out = q::selectDeletions(files, own, 1024 * MiB, 2u);
    Check(out.size() == 2u, "no more files than the bound are selected in one start");
  }

  // Equal ages give a deterministic order (by name of the executable).
  {
    std::vector<q::File> files = {
      { Name('c', '1', ".dxvk.bin"), 800 * MiB, 5 },
      { Name('b', '1', ".dxvk.bin"), 800 * MiB, 5 },
    };
    auto out = q::selectDeletions(files, own, 1024 * MiB);
    Check(out.size() == 1u && out[0] == Name('b', '1', ".dxvk.bin"), "equal ages select in name order");
  }

  Check(q::DirectoryLimit == 2048ull * MiB, "the directory limit is 2 GiB");

  std::printf("%s: %d failure(s)\n", g_failures ? "FAILED" : "PASSED", g_failures);
  return g_failures ? 1 : 0;
}
