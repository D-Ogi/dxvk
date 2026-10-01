#pragma once

#include <array>
#include <optional>
#include <string>
#include <queue>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../util/thread.h"
#include "../util/util_env.h"
#include "../util/util_file.h"

#include "dxvk_shader_ir.h"

namespace dxvk {

  /**
   * \brief Shader cache statistics
   */
  struct DxvkShaderCacheStats {
    uint64_t lookupHits;      ///< Shaders loaded from the cache
    uint64_t lookupMisses;    ///< Shaders not in the cache
    uint64_t written;         ///< Shaders written to the cache
    uint64_t skipped;         ///< Shaders not written: not translated at shutdown, or failed
  };


  /**
   * \brief Shader cache
   *
   * On-disk cache for shaders using the internal IR.
   *
   * The implementation creates two files that can trivially grow by appending
   * data to them: A binary blob that contains the actual serialized IR as well
   * as shader metadata, and a look-up table
   *
   * The writer thread makes no Vulkan call. It translates shaders that
   * are not translated yet without holding the file lock, which lookups
   * on application threads take, and skips them when the cache closes.
   */
  class DxvkShaderCache {

  public:

    struct FilePaths {
      std::string directory;
      std::string lutFile;
      std::string binFile;
    };

    ~DxvkShaderCache();

    void incRef() {
      m_useCount.fetch_add(1u);
    }

    void decRef() {
      releaseInstance();
    }

    /**
     * \brief Looks up shader with matching name and options
     *
     * \param [in] name Shader name
     * \param [in] options Shader properties and compile options
     * \returns Shader object, or \c nullptr if the shader in
     *    question could not be found in the cache.
     */
    Rc<DxvkIrShader> lookupShader(
      const std::string&                name,
      const DxvkIrShaderCreateInfo&     options);

    /**
     * \brief Writes shader to cache file
     *
     * The shader binary will be written asynchronously.
     * \param [in] shader Shader to write to cache
     */
    void addShader(Rc<DxvkIrShader> shader);

    /**
     * \brief Queries statistics
     * \returns Statistics of this cache instance
     */
    DxvkShaderCacheStats getStats() const;

    /**
     * \brief Queries the cache files
     * \returns File paths
     */
    const FilePaths& getFilePaths() const {
      return m_filePaths;
    }

    /**
     * \brief Determines cache file path based on current environment and executable
     *
     * \param [in] directory Cache directory chosen by the host, or empty
     *    for DXVK_SHADER_CACHE_PATH or the per-user default
     * \returns File paths and file names for cache files
     */
    static FilePaths getDefaultFilePaths(const std::string& directory = std::string());

    /**
     * \brief Initializes shader cache
     *
     * There is one instance per process while any device uses it.
     * \param [in] directory See \ref getDefaultFilePaths; ignored if
     *    the instance exists already
     * \returns Shader cache instance
     */
    static Rc<DxvkShaderCache> getInstance(const std::string& directory = std::string());

  private:

    struct Instance {
      dxvk::mutex       mutex;
      DxvkShaderCache*  instance = nullptr;
    };

    static Instance s_instance;

    struct LutHeader {
      std::array<char, 4u>  magic = { };
      std::string           versionString = { };
    };

    struct LutKey {
      std::string name;
      DxvkIrShaderCreateInfo createInfo;

      size_t hash() const;

      bool eq(const LutKey& k) const;
    };

    struct LutEntry {
      uint64_t offset = 0u;
      uint32_t binarySize = 0u;
      uint32_t metadataSize = 0u;
      uint64_t checksum = 0u;
    };

    enum class Status : uint32_t {
      Uninitialized   = 0u,
      CacheDisabled   = 1u,
      OpenWriteOnly   = 2u,
      OpenReadWrite   = 3u,
    };

    std::atomic<uint32_t>         m_useCount = { 0u };

    FilePaths                     m_filePaths;
    dxvk::mutex                   m_fileMutex;

    util::File                    m_lutFile;
    util::File                    m_binFile;

    std::atomic<Status>           m_status = { Status::Uninitialized };

    std::unordered_map<LutKey, LutEntry, DxvkHash, DxvkEq> m_lut;

    dxvk::mutex                   m_writeMutex;
    dxvk::condition_variable      m_writeCond;
    std::queue<Rc<DxvkIrShader>>  m_writeQueue;

    // Keys queued for writing: devices of a process share the cache but not their shaders
    std::unordered_set<LutKey, DxvkHash, DxvkEq> m_queuedKeys;

    std::atomic<bool>             m_stopping = { false };

    std::atomic<uint64_t>         m_lookupHits   = { 0ull };
    std::atomic<uint64_t>         m_lookupMisses = { 0ull };
    std::atomic<uint64_t>         m_written      = { 0ull };
    std::atomic<uint64_t>         m_skipped      = { 0ull };

    dxvk::thread                  m_writer;

    DxvkShaderCache(const std::string& directory);

    bool ensureStatus(Status status);

    Status initialize();

    Status tryInitializeLocked();

    bool openReadWriteLocked();

    bool openWriteOnlyLocked();

    bool parseLut();

    Rc<DxvkIrShader> loadCachedShaderLocked(const LutKey& key, const LutEntry& entry);

    bool writeShaderLutEntry(DxvkIrShader& shader, const LutEntry& entry);

    bool writeShaderToCache(DxvkIrShader& shader);

    bool readShaderLutEntry(LutKey& key, LutEntry& entry, size_t& offset);

    void runWriter();

    void releaseInstance();

    static bool createDirectories(const std::string& path);

    static bool writeShaderXfbInfo(util::File& stream, const dxbc_spv::ir::IoXfbInfo& xfb);

    static bool writeShaderCreateInfo(util::File& stream, const DxvkIrShaderCreateInfo& createInfo);

    static bool writeShaderLayout(util::File& stream, const DxvkPipelineLayoutBuilder& layout);

    static bool writeShaderIo(util::File& stream, const DxvkShaderIo& io);

    static bool writeShaderMetadata(util::File& stream, const DxvkShaderMetadata& metadata);

    static std::optional<LutEntry> writeShaderBinary(util::File& stream, DxvkIrShader& shader);

    static bool writeHeader(util::File& stream, const LutHeader& header);

    static bool readShaderIo(util::File& stream, size_t& offset, DxvkShaderIo& io);

    static bool readShaderXfbInfo(util::File& stream, size_t& offset, dxbc_spv::ir::IoXfbInfo& xfb);

    static bool readShaderLutKey(util::File& stream, size_t& offset, LutKey& key);

    static bool readShaderMetadata(util::File& stream, size_t& offset, DxvkShaderMetadata& metadata);

    static bool readShaderLayout(util::File& stream, size_t& offset, DxvkPipelineLayoutBuilder& layout);

    static bool writeBytes(util::File& stream, const char* data, size_t size) {
      return stream.append(size, data);
    }

    static bool writeBytes(util::File& stream, const uint8_t* data, size_t size) {
      return writeBytes(stream, reinterpret_cast<const char*>(data), size);
    }

    static bool writeString(util::File& stream, const std::string& string) {
      return write(stream, uint16_t(string.size())) && writeBytes(stream, string.data(), string.size());
    }

    template<typename T, std::enable_if_t<std::is_trivially_copyable_v<T>, bool> = true>
    static bool write(util::File& stream, const T& data) {
      return writeBytes(stream, reinterpret_cast<const char*>(&data), sizeof(data));
    }

    static bool readBytes(util::File& stream, char* data, size_t& offset, size_t size) {
      bool result = stream.read(offset, size, data);
      offset += size;
      return result;
    }

    static bool readBytes(util::File& stream, uint8_t* data, size_t& offset, size_t size) {
      return readBytes(stream, reinterpret_cast<char*>(data), offset, size);
    }

    static bool readString(util::File& stream, size_t& offset, std::string& string) {
      uint16_t len = 0u;

      if (!read(stream, offset, len))
        return false;

      string.resize(len);
      return readBytes(stream, string.data(), offset, len);
    }

    template<typename T, std::enable_if_t<std::is_trivially_copyable_v<T>, bool> = true>
    static bool read(util::File& stream, size_t& offset, T& data) {
      return readBytes(stream, reinterpret_cast<char*>(&data), offset, sizeof(data));
    }

  };

}
