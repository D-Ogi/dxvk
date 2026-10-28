#pragma once

// PROVENANCE: DXVK fork D-Ogi/dxvk, MIT.
//
// The limit on the sum of all IR cache files in the cache directory, over all executables. Each
// executable/build pair already has its own cap (DxvkShaderCache::MaxBinFileSize, MaxLutFileSize);
// this limit stops a directory that serves many games from growing without end.
//
// This header holds only the selection, as a pure function of a list of files, so that a host test
// can check it without a file system (cache_quota_test.cpp). The cache supplies the list.

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace dxvk::shader_cache_quota {

  /// The sum of all cache files in the directory that the cache tries to keep.
  constexpr uint64_t DirectoryLimit = 2048ull << 20;

  /// The number of files that one initialization may delete. The rest waits for the next start.
  constexpr size_t MaxDeletions = 64u;

  struct File {
    std::string name;     ///< file name, without the directory
    uint64_t    size;     ///< bytes
    int64_t     written;  ///< last write time, any monotonic unit; larger is newer
  };

  /// true for a name that only the cache writes: 16 hex digits, '-', 16 hex digits,
  /// ".dxvk.lut" or ".dxvk.bin". Lower case only, as getDefaultFilePaths writes it.
  inline bool isCacheFileName(const std::string& name) {
    static const char* suffixes[] = { ".dxvk.lut", ".dxvk.bin" };
    if (name.size() != 33u + 9u)
      return false;
    for (size_t i = 0u; i < 33u; i++) {
      const char c = name[i];
      if (i == 16u) {
        if (c != '-') return false;
      } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) {
        return false;
      }
    }
    const std::string tail = name.substr(33u);
    return tail == suffixes[0] || tail == suffixes[1];
  }

  /// The part of a cache file name that names the executable: the first 16 hex digits and '-'.
  inline std::string executablePrefix(const std::string& name) {
    return name.substr(0u, 17u);
  }

  /// The files to delete so that the sum of the cache files falls to the limit or below.
  ///
  /// Files are grouped by executable. The group of ownPrefix (the running executable) is never
  /// selected. The other groups go whole, the group with the oldest newest file first, until the
  /// sum is at the limit or below, or until maxDeletions files are selected. Names that are not
  /// cache file names are ignored and never counted. Files of a group that the caller cannot delete
  /// (another process holds them open) stay; the sum is then only checked again at the next start.
  inline std::vector<std::string> selectDeletions(const std::vector<File>& files,
          const std::string& ownPrefix, uint64_t limit = DirectoryLimit,
          size_t maxDeletions = MaxDeletions) {
    struct Group {
      std::vector<const File*> files;
      uint64_t size = 0u;
      int64_t  newest = INT64_MIN;
    };

    std::map<std::string, Group> groups;
    uint64_t total = 0u;

    for (const auto& file : files) {
      if (!isCacheFileName(file.name))
        continue;
      total += file.size;
      auto prefix = executablePrefix(file.name);
      if (prefix == ownPrefix)
        continue;
      auto& group = groups[prefix];
      group.files.push_back(&file);
      group.size += file.size;
      group.newest = std::max(group.newest, file.written);
    }

    std::vector<std::string> result;

    if (total <= limit)
      return result;

    std::vector<const Group*> order;
    for (const auto& entry : groups)
      order.push_back(&entry.second);
    // Oldest first; equal times keep the order of the prefix, so the result is deterministic.
    std::stable_sort(order.begin(), order.end(),
      [] (const Group* a, const Group* b) { return a->newest < b->newest; });

    for (const Group* group : order) {
      if (total <= limit || result.size() + group->files.size() > maxDeletions)
        break;
      for (const File* file : group->files)
        result.push_back(file->name);
      total -= group->size;
    }

    return result;
  }

}
