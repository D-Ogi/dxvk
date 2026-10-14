#pragma once

#include <array>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "../util/thread.h"
#include "../util/util_env.h"
#include "../util/util_file.h"

#include "dxvk_shader_ir.h"

namespace dxvk {

  /**
   * \brief Shader cache
   *
   * On-disk cache for shaders using the internal IR.
   *
   * The implementation creates two files that can trivially grow by appending
   * data to them: A binary blob that contains the actual serialized IR as well
   * as shader metadata, and a look-up table
   *
   * The cache writes each entry on the thread that converted the shader, when the
   * conversion ends. It starts no thread of its own, which is what contract E2 of
   * the BC-250 engine (bc250_dxvk_engine.h) requires, and it costs nothing in
   * laziness: a shader that is created and never used is never converted and so
   * never written. Several processes may share one pair of files; a lock on the
   * look-up table file serializes them.
   */
  class DxvkShaderCache {

  public:

    struct FilePaths {
      std::string directory;
      std::string lutFile;
      std::string binFile;
    };

    /**
     * \brief Cache counters
     *
     * Read for diagnosis. An entry that the cache refuses counts as rejected
     * and as a miss: a damaged or foreign entry is never handed to a caller.
     */
    struct Stats {
      uint32_t hits     = 0u;
      uint32_t misses   = 0u;
      uint32_t rejected = 0u;
      uint32_t written  = 0u;
      uint32_t failed   = 0u;
    };

    ~DxvkShaderCache();

    void incRef() {
      m_useCount.fetch_add(1u);
    }

    void decRef() {
      freeInstance();
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
     * \brief Marks a shader for the cache
     *
     * The shader writes itself to the cache when its conversion ends, on the
     * thread that converted it. A shader whose key the cache already holds is
     * not marked.
     * \param [in] shader Shader to write to cache
     */
    void addShader(Rc<DxvkIrShader> shader);

    /**
     * \brief Writes a converted shader to the cache file
     *
     * Called by the shader itself at the end of its conversion. Does nothing
     * when the cache is disabled, full, or already holds the key.
     * \param [in] shader Converted shader
     */
    void writeShader(DxvkIrShader& shader) noexcept;

    /**
     * \brief Queries the cache counters
     * \returns Counter values
     */
    Stats getStats() const;

    /**
     * \brief Queries the identity this cache reads and writes entries for
     *
     * Every entry belongs to one build of this module. The string names the
     * build, and its hash is part of the file name.
     * \returns Identity string, empty when it could not be determined
     */
    const std::string& getIdentity() const {
      return m_identity;
    }

    /**
     * \brief Determines cache file path based on current environment and executable
     *
     * \param [in] directory Cache directory the host chose, or an empty string for
     *    the per-user default of this platform
     * \returns File paths and file names for cache files
     */
    static FilePaths getDefaultFilePaths(const std::string& directory = std::string());

    /**
     * \brief Builds the identity string of this build of the module
     *
     * Names everything that decides what a converted shader looks like: the DXVK
     * version, the on-disk format version, the build flavour and a build id that
     * the linker renews on every link, so a changed converter can never read an
     * entry of an earlier build.
     * \returns Identity string, or an empty string on failure
     */
    static std::string buildIdentity();

    /**
     * \brief Initializes shader cache
     *
     * There is one instance per process while a device holds it.
     * \param [in] directory See \ref getDefaultFilePaths. Ignored when the
     *    instance exists already, because the files are open by then.
     * \returns Shader cache instance
     */
    static Rc<DxvkShaderCache> getInstance(const std::string& directory = std::string(),
      bool (*accessAllowed)() = nullptr);

  private:

    /// What an attempt to write one entry did.
    enum class WriteResult : uint32_t {
      Written = 0u,   ///< The entry is in the files
      Skipped = 1u,   ///< Another writer got there first, or the cache is full
      Failed  = 2u,   ///< A file operation failed
    };

    /// On-disk format of the look-up table and the binary blob. Raised by hand when the
    /// layout of either file changes; an entry of another format version is never read.
    static constexpr uint32_t FormatVersion = 2u;

    /// Size the binary blob may reach, in bytes. At the cap the cache stops adding entries:
    /// every look-up table entry holds an absolute offset, so nothing inside the blob can be
    /// evicted. The caches of earlier builds are deleted at initialization instead.
    static constexpr uint64_t MaxBinFileSize = 384ull << 20;
    static constexpr size_t MaxLutFileSize = 64u << 20;
    static constexpr size_t MaxBinarySize = 16u << 20;
    static constexpr size_t MaxMetadataSize = 1u << 20;
    static constexpr size_t MaxLutRecordSize = 64u << 10;
    static constexpr size_t MaxMapBytes = 64u << 20;
    static constexpr size_t MaxEntries = 16384u;
    static constexpr size_t MaxStringSize = 4096u;
    static constexpr size_t LutRecordHeaderSize = sizeof(uint32_t) + sizeof(uint64_t);

    /**
     * \brief Holds the cache file lock for a scope
     *
     * The lock lives on the look-up table file and covers both files.
     */
    class FileLock {

    public:

      FileLock(util::File& file, bool exclusive)
      : m_file(file), m_held(file.lock(exclusive)) { }

      ~FileLock() {
        if (m_held)
          m_file.unlock();
      }

      FileLock             (const FileLock&) = delete;
      FileLock& operator = (const FileLock&) = delete;

      explicit operator bool () const {
        return m_held;
      }

    private:

      util::File& m_file;
      bool        m_held;

    };

    /**
     * \brief Builds one record in memory
     *
     * Every field of a record used to be a seek and a write of its own on the file, which is
     * hundreds of system calls for one shader and measured 16.8 % of a cold start over the 3607
     * shaders of BD-164. A record is built here and appended to the file in one call. That also
     * makes a partial record far less likely: one append either lands or does not.
     */
    struct ByteWriter {
      std::vector<uint8_t> data;

      bool put(const void* source, size_t size) {
        if (size > MaxBinarySize + MaxMetadataSize - data.size())
          return false;
        if (!size)
          return true;
        auto bytes = reinterpret_cast<const uint8_t*>(source);
        data.insert(data.end(), bytes, bytes + size);
        return true;
      }
    };

    struct ByteReader {
      const uint8_t* data;
      size_t size;

      bool read(size_t offset, size_t count, void* out) {
        if (offset > size || count > size - offset)
          return false;
        if (count)
          std::memcpy(out, data + offset, count);
        return true;
      }
    };

    struct Instance {
      dxvk::mutex       mutex;
      DxvkShaderCache*  instance = nullptr;
    };

    static Instance s_instance;

    struct LutHeader {
      std::array<char, 4u>  magic = { };
      std::string           versionString = { };
      std::array<uint8_t, 16u> generation = { };
    };

    /// Size of a parsed header, in bytes: the magic plus the length-prefixed identity string.
    static size_t headerSize(const LutHeader& header) {
      return header.magic.size() + sizeof(uint16_t) + header.versionString.size() + header.generation.size();
    }

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
      Open            = 2u,
    };

    std::atomic<uint32_t>         m_useCount = { 0u };

    bool (*m_accessAllowed)() = nullptr;
    std::string                   m_directory;
    std::array<uint8_t, 16u>       m_generation = { };
    size_t                        m_mapBytes = 0u;
    std::string                   m_identity;
    FilePaths                     m_filePaths;
    dxvk::mutex                   m_fileMutex;

    util::File                    m_lutFile;
    util::File                    m_binFile;

    std::atomic<Status>           m_status = { Status::Uninitialized };

    /// Length of the look-up table up to the end of the last complete entry. A process that is
    /// killed inside an append leaves a partial entry behind; the next append drops it instead
    /// of discarding the whole cache.
    size_t                        m_lutValidSize = 0u;

    /// Set once when the binary blob reached its cap, so the cache says so once.
    bool                          m_reportedFull = false;

    std::unordered_map<LutKey, LutEntry, DxvkHash, DxvkEq> m_lut;

    mutable dxvk::mutex           m_statsMutex;
    Stats                         m_stats;

    DxvkShaderCache(const std::string& directory, bool (*accessAllowed)());

    bool accessAllowed() const { return !m_accessAllowed || m_accessAllowed(); }

    void rejectEntry();

    bool readHeaderLocked(LutHeader& header);

    bool refreshLocked();

    bool insertEntry(LutKey key, const LutEntry& entry);

    bool ensureStatus(Status status);

    Status initialize();

    Status tryInitializeLocked();

    bool openFilesLocked();

    bool resetFilesLocked();

    void readLutEntriesLocked();

    bool parseLutLocked();

    void evictLocked();

    Rc<DxvkIrShader> loadCachedShaderLocked(const LutKey& key, const LutEntry& entry);

    bool writeShaderLutEntry(ByteWriter& stream, DxvkIrShader& shader, const LutEntry& entry);

    WriteResult writeShaderToCacheLocked(DxvkIrShader& shader);

    bool readShaderLutEntry(ByteReader& stream, LutKey& key, LutEntry& entry, size_t& offset);

    void freeInstance();

    static std::string moduleBuildId();

    static bool writeShaderXfbInfo(ByteWriter& stream, const dxbc_spv::ir::IoXfbInfo& xfb);

    static bool writeShaderCreateInfo(ByteWriter& stream, const DxvkIrShaderCreateInfo& createInfo);

    static bool writeShaderLayout(ByteWriter& stream, const DxvkPipelineLayoutBuilder& layout);

    static bool writeShaderIo(ByteWriter& stream, const DxvkShaderIo& io);

    static bool writeShaderMetadata(ByteWriter& stream, const DxvkShaderMetadata& metadata);

    static std::optional<LutEntry> writeShaderBinary(ByteWriter& stream,
            uint64_t baseOffset, DxvkIrShader& shader);

    static bool writeHeader(ByteWriter& stream, const LutHeader& header);

    static bool readShaderIo(ByteReader& stream, size_t& offset, DxvkShaderIo& io);

    static bool readShaderXfbInfo(ByteReader& stream, size_t& offset, dxbc_spv::ir::IoXfbInfo& xfb);

    static bool readShaderLutKey(ByteReader& stream, size_t& offset, LutKey& key);

    static bool readShaderMetadata(ByteReader& stream, size_t& offset, DxvkShaderMetadata& metadata);

    static bool readShaderLayout(ByteReader& stream, size_t& offset, DxvkPipelineLayoutBuilder& layout);

    static bool writeBytes(ByteWriter& stream, const char* data, size_t size) {
      return stream.put(data, size);
    }

    static bool writeBytes(ByteWriter& stream, const uint8_t* data, size_t size) {
      return writeBytes(stream, reinterpret_cast<const char*>(data), size);
    }

    static bool writeString(ByteWriter& stream, const std::string& string) {
      return string.size() <= MaxStringSize && write(stream, uint16_t(string.size()))
          && writeBytes(stream, string.data(), string.size());
    }

    template<typename T, std::enable_if_t<std::is_trivially_copyable_v<T>, bool> = true>
    static bool write(ByteWriter& stream, const T& data) {
      return writeBytes(stream, reinterpret_cast<const char*>(&data), sizeof(data));
    }

    static bool readBytes(ByteReader& stream, char* data, size_t& offset, size_t size) {
      if (!stream.read(offset, size, data))
        return false;
      offset += size;
      return true;
    }

    static bool readBytes(ByteReader& stream, uint8_t* data, size_t& offset, size_t size) {
      return readBytes(stream, reinterpret_cast<char*>(data), offset, size);
    }

    static bool readString(ByteReader& stream, size_t& offset, std::string& string) {
      uint16_t len = 0u;

      if (!read(stream, offset, len))
        return false;

      if (len > MaxStringSize || offset > stream.size || len > stream.size - offset)
        return false;
      string.resize(len);
      return readBytes(stream, string.data(), offset, len);
    }

    template<typename T, std::enable_if_t<std::is_trivially_copyable_v<T>, bool> = true>
    static bool read(ByteReader& stream, size_t& offset, T& data) {
      return readBytes(stream, reinterpret_cast<char*>(&data), offset, sizeof(data));
    }

  };

}
