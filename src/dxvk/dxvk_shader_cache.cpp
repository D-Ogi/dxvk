#include <algorithm>
#include <filesystem>
#include <iomanip>
#include <sstream>
#include <system_error>
#include <version.h>
#ifdef _WIN32
#include <objbase.h>
#endif

#include "dxvk_shader_cache.h"

#include "../util/util_time.h"

namespace dxvk {

  DxvkShaderCache::Instance DxvkShaderCache::s_instance;

  DxvkShaderCache::DxvkShaderCache(const std::string& directory, bool (*accessAllowed)())
  : m_accessAllowed(accessAllowed), m_directory(directory),
    m_identity(buildIdentity()), m_filePaths(getDefaultFilePaths(directory)) { }


  DxvkShaderCache::~DxvkShaderCache() {
#ifdef AMDGPU_WDDM_LAB_FLAVOUR
    auto stats = getStats();

    if (stats.hits || stats.misses || stats.written || stats.rejected || stats.failed) {
      Logger::info(str::format("Shader cache: ", stats.hits, " hits, ", stats.misses, " misses, ",
        stats.written, " written, ", stats.rejected, " rejected, ", stats.failed, " failed writes"));
    }
#endif
  }


  DxvkShaderCache::Stats DxvkShaderCache::getStats() const {
    std::unique_lock lock(m_statsMutex);
    return m_stats;
  }


  Rc<DxvkIrShader> DxvkShaderCache::lookupShader(
    const std::string& name, const DxvkIrShaderCreateInfo& options) {
    try {
      if (!accessAllowed() || !ensureStatus(Status::Open))
        return nullptr;
      std::unique_lock lock(m_fileMutex);
      FileLock fileLock(m_lutFile, false);
      if (!fileLock || !refreshLocked())
        return nullptr;
      LutKey key = { name, options };
      auto entry = m_lut.find(key);
      Rc<DxvkIrShader> shader;
      if (entry != m_lut.end()) {
        shader = loadCachedShaderLocked(entry->first, entry->second);
        if (!shader) {
          rejectEntry();
          m_lut.erase(entry);
        }
      }
      std::unique_lock stats(m_statsMutex);
      if (shader) m_stats.hits++;
      else m_stats.misses++;
      return shader;
    } catch (...) {
      // A cache is optional, including when allocation or file parsing fails.
      return nullptr;
    }
  }


  void DxvkShaderCache::addShader(Rc<DxvkIrShader> shader) {
    try {
      if (accessAllowed() && ensureStatus(Status::Open))
        shader->setCacheSink(this);
    } catch (...) { }
  }


  void DxvkShaderCache::writeShader(DxvkIrShader& shader) noexcept {
    try {
      if (!accessAllowed() || !ensureStatus(Status::Open))
        return;
      std::unique_lock lock(m_fileMutex);
      const auto result = writeShaderToCacheLocked(shader);
      std::unique_lock stats(m_statsMutex);
      if (result == WriteResult::Written) m_stats.written++;
      else if (result == WriteResult::Failed) {
        m_stats.failed++;
        m_status.store(Status::CacheDisabled);
      }
    } catch (...) {
      m_status.store(Status::CacheDisabled);
      std::unique_lock stats(m_statsMutex);
      m_stats.failed++;
    }
  }


  bool DxvkShaderCache::ensureStatus(Status status) {
    auto currentStatus = m_status.load();

    if (currentStatus == Status::Uninitialized)
      currentStatus = initialize();

    return currentStatus >= status;
  }


  DxvkShaderCache::Status DxvkShaderCache::initialize() {
    std::unique_lock lock(m_fileMutex);
    auto status = m_status.load();

    if (status != Status::Uninitialized)
      return status;

    status = tryInitializeLocked();

    m_status.store(status);
    return status;
  }


  DxvkShaderCache::Status DxvkShaderCache::tryInitializeLocked() {
    if (!accessAllowed() || m_identity.empty() || m_filePaths.directory.empty()
     || m_filePaths.binFile.empty() || m_filePaths.lutFile.empty() || !openFilesLocked())
      return Status::CacheDisabled;
    FileLock lock(m_lutFile, true);
    if (!lock)
      return Status::CacheDisabled;
    if (!parseLutLocked()) {
      if (m_lutFile.size() || m_binFile.size()) rejectEntry();
      if (!resetFilesLocked()) return Status::CacheDisabled;
    }
    evictLocked();
    return Status::Open;
  }


  bool DxvkShaderCache::openFilesLocked() {
    auto path = m_filePaths.directory + env::PlatformDirSlash;
    auto flags = util::FileFlags(util::FileFlag::AllowRead,
      util::FileFlag::AllowWrite, util::FileFlag::Create);
    std::error_code error;
    std::filesystem::create_directories(std::filesystem::u8path(m_filePaths.directory), error);
    if (error) return false;
    m_lutFile.open(path + m_filePaths.lutFile, flags);
    m_binFile.open(path + m_filePaths.binFile, flags);
    return bool(m_binFile) && bool(m_lutFile);
  }


  bool DxvkShaderCache::resetFilesLocked() {
    LutHeader header = { };
    header.magic = { 'D', 'X', 'V', 'K' };
    header.versionString = m_identity;
#ifdef _WIN32
    GUID generation;
    if (FAILED(CoCreateGuid(&generation))) return false;
    static_assert(sizeof(generation) == 16u);
    std::memcpy(header.generation.data(), &generation, sizeof(generation));
#else
    return false;
#endif
    ByteWriter record;
    if (!writeHeader(record, header)) return false;
    // Only the exclusive LUT lock may reset this pair. The random generation
    // makes every other process discard its old offsets before another read.
    if (!m_lutFile.truncate(0u) || !m_binFile.truncate(0u)
     || !m_lutFile.append(record.data.size(), record.data.data())) return false;
    m_lut.clear();
    m_mapBytes = 0u;
    m_generation = header.generation;
    m_lutValidSize = record.data.size();
    return true;
  }

  void DxvkShaderCache::rejectEntry() {
    std::unique_lock stats(m_statsMutex);
    m_stats.rejected++;
#ifdef AMDGPU_WDDM_LAB_FLAVOUR
    if (m_stats.rejected <= 8u)
      Logger::warn("Shader cache: rejected damaged or truncated record");
#endif
  }

  bool DxvkShaderCache::insertEntry(LutKey key, const LutEntry& entry) {
    if (m_lut.size() >= MaxEntries) return false;
    size_t bytes = sizeof(LutKey) + sizeof(LutEntry) + key.name.size();
    bytes += key.createInfo.xfbEntries.size() * sizeof(dxbc_spv::ir::IoXfbInfo);
    for (const auto& xfb : key.createInfo.xfbEntries) bytes += xfb.semanticName.size();
    if (bytes > MaxMapBytes - m_mapBytes) return false;
    // Conservative accounting also charges replacements. It never undercounts.
    m_mapBytes += bytes;
    m_lut.insert_or_assign(std::move(key), entry);
    return true;
  }


  // Reads every complete entry from m_lutValidSize to the end of the file into the table and moves
  // m_lutValidSize to the end of the last complete one. Called at initialization and again before
  // every append, because another process may have appended its own entries in between: those are
  // complete and are merged here, and only a genuinely partial tail is left for the append to drop.
  void DxvkShaderCache::readLutEntriesLocked() {
    const size_t size = m_lutFile.size();
    const size_t binSize = m_binFile.size();
    size_t offset = m_lutValidSize;
    while (offset < size) {
      std::array<uint8_t, LutRecordHeaderSize> raw = { };
      uint32_t length = 0;
      uint64_t checksum = 0;
      if (size - offset < raw.size() || !m_lutFile.read(offset, raw.size(), raw.data())) {
        rejectEntry(); break;
      }
      ByteReader framing = { raw.data(), raw.size() };
      size_t at = 0;
      if (!read(framing, at, length) || !read(framing, at, checksum)
       || !length || length > MaxLutRecordSize || length > size - offset - raw.size()) {
        rejectEntry(); break;
      }
      std::vector<uint8_t> payload(length);
      if (!m_lutFile.read(offset + raw.size(), length, payload.data())) {
        rejectEntry(); break;
      }
      offset += raw.size() + length;
      m_lutValidSize = offset;
      // No strings, options, counts or metadata are interpreted before this.
      if (checksum != bit::fnv1a_hash(payload.data(), payload.size())) {
        rejectEntry(); continue;
      }
      LutKey key;
      LutEntry entry;
      ByteReader record = { payload.data(), payload.size() };
      at = 0;
      if (!readShaderLutEntry(record, key, entry, at) || at != payload.size()
       || !entry.binarySize || entry.binarySize > MaxBinarySize
       || !entry.metadataSize || entry.metadataSize > MaxMetadataSize
       || entry.offset > binSize || uint64_t(entry.binarySize) + entry.metadataSize > binSize - entry.offset) {
        rejectEntry(); continue;
      }
      insertEntry(std::move(key), entry);
    }
  }


  bool DxvkShaderCache::readHeaderLocked(LutHeader& header) {
    const size_t size = m_lutFile.size();
    if (size > MaxLutFileSize || m_binFile.size() > MaxBinFileSize || size < 6u) return false;
    std::array<uint8_t, 6u> prefix;
    if (!m_lutFile.read(0, prefix.size(), prefix.data())) return false;
    uint16_t length;
    std::memcpy(&length, prefix.data() + 4u, sizeof(length));
    if (length > MaxStringSize || size < prefix.size() + length + header.generation.size()) return false;
    std::vector<uint8_t> data(prefix.size() + length + header.generation.size());
    if (!m_lutFile.read(0, data.size(), data.data())) return false;
    ByteReader reader = { data.data(), data.size() };
    size_t at = 0;
    return readBytes(reader, header.magic.data(), at, header.magic.size())
        && readString(reader, at, header.versionString)
        && readBytes(reader, header.generation.data(), at, header.generation.size())
        && header.magic == std::array<char, 4u>{ 'D', 'X', 'V', 'K' }
        && header.versionString == m_identity;
  }

  bool DxvkShaderCache::parseLutLocked() {
    LutHeader header;
    if (!readHeaderLocked(header)) return false;
    m_lut.clear();
    m_mapBytes = 0;
    m_generation = header.generation;
    m_lutValidSize = headerSize(header);
    readLutEntriesLocked();
    return true;
  }

  bool DxvkShaderCache::refreshLocked() {
    LutHeader header;
    if (!readHeaderLocked(header)) { rejectEntry(); return false; }
    if (header.generation != m_generation || m_lutValidSize > m_lutFile.size())
      return parseLutLocked();
    if (m_lutValidSize != m_lutFile.size()) readLutEntriesLocked();
    return true;
  }


  bool DxvkShaderCache::writeShaderXfbInfo(ByteWriter& stream, const dxbc_spv::ir::IoXfbInfo& xfb) {
    return writeString(stream, xfb.semanticName)
        && write(stream, xfb.semanticIndex)
        && write(stream, xfb.componentMask)
        && write(stream, xfb.stream)
        && write(stream, xfb.buffer)
        && write(stream, xfb.offset)
        && write(stream, xfb.stride);
  }


  bool DxvkShaderCache::writeShaderCreateInfo(ByteWriter& stream, const DxvkIrShaderCreateInfo& createInfo) {
    bool status = write(stream, createInfo.options)
               && write(stream, createInfo.flatShadingInputs)
               && write(stream, createInfo.rasterizedStream);

    status = status && write(stream, uint32_t(createInfo.xfbEntries.size()));

    for (const auto& xfb : createInfo.xfbEntries)
      status = status && writeShaderXfbInfo(stream, xfb);

    return status;
  }


  Rc<DxvkIrShader> DxvkShaderCache::loadCachedShaderLocked(const LutKey& key, const LutEntry& entry) {
    const size_t fileSize = m_binFile.size();
    const uint64_t total = uint64_t(entry.binarySize) + entry.metadataSize;
    if (!entry.binarySize || entry.binarySize > MaxBinarySize || !entry.metadataSize
     || entry.metadataSize > MaxMetadataSize || entry.offset > fileSize || total > fileSize - entry.offset)
      return nullptr;
    std::vector<uint8_t> data(size_t(total), uint8_t(0));
    if (!m_binFile.read(size_t(entry.offset), data.size(), data.data())
     || entry.checksum != bit::fnv1a_hash(data.data(), data.size())) return nullptr;
    // Metadata is covered by the same checksum as IR and is bounded separately.
    ByteReader reader = { data.data() + entry.binarySize, entry.metadataSize };
    size_t offset = 0;
    DxvkShaderMetadata metadata;
    DxvkPipelineLayoutBuilder layout;
    if (!readShaderMetadata(reader, offset, metadata)
     || !readShaderLayout(reader, offset, layout) || offset != reader.size) return nullptr;
    data.resize(entry.binarySize);
    return new DxvkIrShader(key.name, key.createInfo, std::move(metadata), std::move(layout), std::move(data));
  }


  bool DxvkShaderCache::writeShaderLutEntry(ByteWriter& stream, DxvkIrShader& shader,
    const LutEntry& entry) {
    return writeString(stream, shader.debugName())
        && writeShaderCreateInfo(stream, shader.getShaderCreateInfo())
        && write(stream, entry);
  }


  DxvkShaderCache::WriteResult DxvkShaderCache::writeShaderToCacheLocked(DxvkIrShader& shader) {
    FileLock lock(m_lutFile, true);
    if (!lock) return WriteResult::Failed;
    if (!refreshLocked() && !resetFilesLocked()) return WriteResult::Failed;
    LutKey key = { shader.debugName(), shader.getShaderCreateInfo() };
    if (m_lut.find(key) != m_lut.end()) return WriteResult::Skipped;
    const size_t binSize = m_binFile.size();
    if (binSize >= MaxBinFileSize || m_lut.size() >= MaxEntries || m_mapBytes >= MaxMapBytes)
      return WriteResult::Skipped;
    ByteWriter blob;
    auto entry = writeShaderBinary(blob, binSize, shader);
    if (!entry) return WriteResult::Skipped;
    ByteWriter payload;
    if (!writeShaderLutEntry(payload, shader, *entry) || payload.data.size() > MaxLutRecordSize)
      return WriteResult::Skipped;
    ByteWriter record;
    if (!write(record, uint32_t(payload.data.size()))
     || !write(record, bit::fnv1a_hash(payload.data.data(), payload.data.size()))
     || !writeBytes(record, payload.data.data(), payload.data.size())) return WriteResult::Skipped;
    if (blob.data.size() > MaxBinFileSize - binSize
     || record.data.size() > MaxLutFileSize - m_lutValidSize) return WriteResult::Skipped;
    if (m_lutValidSize != m_lutFile.size() && !m_lutFile.truncate(m_lutValidSize)) return WriteResult::Failed;
    // Publish only after every payload byte is written. A torn pair fails its
    // checksum. Readers hold the same lock in shared mode through the load.
    if (!m_binFile.append(blob.data.size(), blob.data.data())
     || !m_lutFile.append(record.data.size(), record.data.data())) return WriteResult::Failed;
    m_lutValidSize += record.data.size();
    insertEntry(std::move(key), *entry);
    return WriteResult::Written;
  }


  bool DxvkShaderCache::readShaderIo(ByteReader& stream, size_t& offset, DxvkShaderIo& io) {
    uint8_t varCount = 0u;

    if (!read(stream, offset, varCount))
      return false;

    for (uint32_t i = 0u; i < varCount; i++) {
      DxvkShaderIoVar var = { };
      uint8_t patchConstant = 0u;

      if (!read(stream, offset, var.builtIn)
       || !read(stream, offset, var.location)
       || !read(stream, offset, var.componentIndex)
       || !read(stream, offset, var.componentCount)
       || !read(stream, offset, patchConstant)
       || !read(stream, offset, var.semanticIndex)
       || !readString(stream, offset, var.semanticName))
        return false;

      if (patchConstant > 1u || !var.componentCount || var.componentIndex > 3u
       || (var.builtIn == spv::BuiltInMax && (var.location >= 32u
          || var.componentCount > 4u - var.componentIndex))) return false;
      var.isPatchConstant = patchConstant != 0u;

      io.add(std::move(var));
    }

    return true;
  }


  bool DxvkShaderCache::readShaderMetadata(ByteReader& stream, size_t& offset, DxvkShaderMetadata& metadata) {
    bool status = read(stream, offset, metadata.stage)
               && read(stream, offset, metadata.flags)
               && read(stream, offset, metadata.specConstantMask)
               && readShaderIo(stream, offset, metadata.inputs)
               && readShaderIo(stream, offset, metadata.outputs)
               && read(stream, offset, metadata.inputTopology)
               && read(stream, offset, metadata.outputTopology)
               && read(stream, offset, metadata.flatShadingInputs)
               && read(stream, offset, metadata.rasterizedStream)
               && read(stream, offset, metadata.patchVertexCount);

    for (auto& xfb : metadata.xfbStrides)
      status = status && read(stream, offset, xfb);

    return status;
  }


  bool DxvkShaderCache::readShaderLayout(ByteReader& stream, size_t& offset, DxvkPipelineLayoutBuilder& layout) {
    VkShaderStageFlags stageMask = { };

    const VkShaderStageFlags allowedStages = VK_SHADER_STAGE_ALL_GRAPHICS | VK_SHADER_STAGE_COMPUTE_BIT;
    if (!read(stream, offset, stageMask) || !stageMask || (stageMask & ~allowedStages))
      return false;

    layout = DxvkPipelineLayoutBuilder(stageMask);

    // Read push data blocks
    uint32_t pushDataMask = 0u;

    if (!read(stream, offset, pushDataMask)
     || (pushDataMask & ~((1u << DxvkPushDataBlock::MaxBlockCount) - 1u)))
      return false;

    for (uint32_t i = 0u; i < bit::popcnt(pushDataMask); i++) {
      DxvkPushDataBlock block = { };

      if (!read(stream, offset, block) || !block.getStageMask()
       || (block.getStageMask() & ~allowedStages))
        return false;

      layout.addPushData(block);
    }

    // Read shader binding info
    uint32_t bindingCount = 0u;

    if (!read(stream, offset, bindingCount) || bindingCount > 4096u)
      return false;

    for (uint32_t i = 0u; i < bindingCount; i++) {
      DxvkShaderDescriptor binding = { };

      if (!read(stream, offset, binding))
        return false;

      layout.addBindings(1u, &binding);
    }

    // Read sampler heap mappings
    uint32_t samplerHeapCount = 0u;

    if (!read(stream, offset, samplerHeapCount) || samplerHeapCount > 4096u)
      return false;

    for (uint32_t i = 0u; i < samplerHeapCount; i++) {
      DxvkShaderBinding binding = { };

      if (!read(stream, offset, binding))
        return false;

      layout.addSamplerHeap(binding);
    }

    // Read spec data mappings
    uint32_t specDataCount = 0u;

    if (!read(stream, offset, specDataCount) || specDataCount > 4096u)
      return false;

    for (uint32_t i = 0u; i < specDataCount; i++) {
      DxvkShaderBinding binding = { };

      if (!read(stream, offset, binding))
        return false;

      layout.addSpecDataBuffer(binding);
    }

    return true;
  }


  bool DxvkShaderCache::readShaderXfbInfo(ByteReader& stream, size_t& offset, dxbc_spv::ir::IoXfbInfo& xfb) {
    return readString(stream, offset, xfb.semanticName)
        && read(stream, offset, xfb.semanticIndex)
        && read(stream, offset, xfb.componentMask)
        && read(stream, offset, xfb.stream)
        && read(stream, offset, xfb.buffer)
        && read(stream, offset, xfb.offset)
        && read(stream, offset, xfb.stride);
 }


  bool DxvkShaderCache::readShaderLutKey(ByteReader& stream, size_t& offset, LutKey& key) {
    bool status = readString(stream, offset, key.name)
               && read(stream, offset, key.createInfo.options)
               && read(stream, offset, key.createInfo.flatShadingInputs)
               && read(stream, offset, key.createInfo.rasterizedStream);

    uint32_t xfbCount = 0u;
    status = status && read(stream, offset, xfbCount);

    if (!status || xfbCount > 256u || offset > stream.size
     || xfbCount > (stream.size - offset) / 8u) return false;
    key.createInfo.xfbEntries.resize(xfbCount);

    for (uint32_t i = 0u; i < xfbCount; i++)
      status = status && readShaderXfbInfo(stream, offset, key.createInfo.xfbEntries[i]);

    return status;
  }


  bool DxvkShaderCache::readShaderLutEntry(ByteReader& stream, LutKey& key, LutEntry& entry, size_t& offset) {
    return readShaderLutKey(stream, offset, key) && read(stream, offset, entry);
  }


  // The caches that other builds of the driver wrote for this executable. They can never be read by
  // this build, because the identity is part of the file name, so they are pure waste and go here.
  // Nothing else is deleted: the name also carries the hash of the executable path, so no other
  // application's cache and no unrelated file matches the pattern. A file that a process of another
  // build still holds open simply fails to be deleted, which costs only the space.
  void DxvkShaderCache::evictLocked() {
    const std::string suffixes[] = { ".dxvk.lut", ".dxvk.bin" };
    uint32_t removed = 0u;

    try {
      // The part of the name before the identity, which names this executable alone.
      auto prefix = m_filePaths.lutFile.substr(0u, m_filePaths.lutFile.find('-') + 1u);

      if (prefix.empty())
        return;

      size_t visited = 0;
      for (const auto& item : std::filesystem::directory_iterator(
          std::filesystem::u8path(m_filePaths.directory))) {
        if (++visited > 4096u) break;
        if (!item.is_regular_file())
          continue;

        auto name = item.path().filename().string();

        if (name.compare(0u, prefix.size(), prefix))
          continue;

        if (name == m_filePaths.lutFile || name == m_filePaths.binFile)
          continue;

        bool matches = false;

        for (const auto& suffix : suffixes) {
          if (name.size() > suffix.size()
           && !name.compare(name.size() - suffix.size(), suffix.size(), suffix))
            matches = true;
        }

        if (!matches)
          continue;

        std::error_code error;

        if (std::filesystem::remove(item.path(), error))
          removed += 1u;
      }
    } catch (const std::exception&) {
      return;
    }

#ifdef AMDGPU_WDDM_LAB_FLAVOUR
    if (removed)
      Logger::info(str::format("Shader cache: removed ", removed, " file(s) of other driver builds."));
#endif
  }


  std::string DxvkShaderCache::buildIdentity() {
    auto build = moduleBuildId();

    if (build.empty())
      return std::string();

#ifdef AMDGPU_WDDM_LAB_FLAVOUR
    const char* flavour = "lab";
#else
    const char* flavour = "release";
#endif

    return str::format(DXVK_VERSION, "/", FormatVersion, "/", flavour, "/", build);
  }


  // What the entries in the cache depend on is everything compiled into this module: the DXVK tree,
  // the dxbc-spirv converter under subprojects, the compiler and its options. A version string
  // covers none of that reliably, and a constant that an engineer must raise by hand is forgotten.
  // The linker gives every link a new CodeView signature, so that signature answers the question
  // exactly and by itself. A module without a CodeView build ID disables the cache.
  std::string DxvkShaderCache::moduleBuildId() {
#ifdef _WIN32
    HMODULE module = nullptr;

    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
          | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
          reinterpret_cast<LPCWSTR>(&DxvkShaderCache::moduleBuildId), &module) || !module)
      return std::string();

    auto base = reinterpret_cast<const uint8_t*>(module);
    auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);

    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew < sizeof(IMAGE_DOS_HEADER)
     || dos->e_lfanew > (1 << 20))
      return std::string();

    auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);

    if (nt->Signature != IMAGE_NT_SIGNATURE)
      return std::string();

    if (nt->OptionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_DEBUG)
      return std::string();
    const size_t imageSize = nt->OptionalHeader.SizeOfImage;
    const auto& debugDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];

    if (debugDir.VirtualAddress && debugDir.VirtualAddress < imageSize
     && debugDir.Size <= imageSize - debugDir.VirtualAddress
     && debugDir.Size >= sizeof(IMAGE_DEBUG_DIRECTORY)) {
      auto entries = reinterpret_cast<const IMAGE_DEBUG_DIRECTORY*>(base + debugDir.VirtualAddress);
      size_t count = debugDir.Size / sizeof(IMAGE_DEBUG_DIRECTORY);

      for (size_t i = 0u; i < count; i++) {
        if (entries[i].Type != IMAGE_DEBUG_TYPE_CODEVIEW || !entries[i].AddressOfRawData)
          continue;

        struct CodeViewRsds {
          uint32_t signature;
          GUID     guid;
          uint32_t age;
        };

        if (entries[i].SizeOfData < sizeof(CodeViewRsds) || entries[i].AddressOfRawData >= imageSize
         || entries[i].SizeOfData > imageSize - entries[i].AddressOfRawData)
          continue;

        auto rsds = reinterpret_cast<const CodeViewRsds*>(base + entries[i].AddressOfRawData);

        if (rsds->signature != 0x53445352u /* 'RSDS' */)
          continue;

        std::stringstream id;
        id << std::hex << std::setfill('0');
        id << std::setw(8) << rsds->guid.Data1 << std::setw(4) << rsds->guid.Data2
           << std::setw(4) << rsds->guid.Data3;

        for (auto byte : rsds->guid.Data4)
          id << std::setw(2) << uint32_t(byte);

        id << std::setw(1) << "-" << rsds->age;
        return id.str();
      }
    }

    return std::string();
#else
    return std::string();
#endif
  }


  bool DxvkShaderCache::writeShaderLayout(ByteWriter& stream, const DxvkPipelineLayoutBuilder& layout) {
    bool status = write(stream, layout.getStageMask())
               && write(stream, layout.getPushDataMask());

    for (auto pushIndex : bit::BitMask(layout.getPushDataMask()))
      status = status && write(stream, layout.getPushDataBlock(pushIndex));

    auto bindings = layout.getBindings();
    status = status && write(stream, uint32_t(bindings.bindingCount));

    for (size_t i = 0u; i < bindings.bindingCount; i++)
      status = status && write(stream, bindings.bindings[i]);

    status = status && write(stream, uint32_t(layout.getSamplerHeapBindingCount()));

    for (size_t i = 0u; i < layout.getSamplerHeapBindingCount(); i++)
      status = status && write(stream, layout.getSamplerHeapBinding(i));

    status = status && write(stream, uint32_t(layout.getSpecDataBindingCount()));

    for (size_t i = 0u; i < layout.getSpecDataBindingCount(); i++)
      status = status && write(stream, layout.getSpecDataBinding(i));

    return status;
  }


  bool DxvkShaderCache::writeShaderIo(ByteWriter& stream, const DxvkShaderIo& io) {
    bool status = write(stream, uint8_t(io.getVarCount()));

    for (uint32_t i = 0u; i < io.getVarCount(); i++) {
      const auto& var = io.getVar(i);

      status = status && write(stream, var.builtIn)
                      && write(stream, var.location)
                      && write(stream, var.componentIndex)
                      && write(stream, var.componentCount)
                      && write(stream, var.isPatchConstant)
                      && write(stream, var.semanticIndex)
                      && writeString(stream, var.semanticName);
    }

    return status;
  }


  bool DxvkShaderCache::writeShaderMetadata(ByteWriter& stream, const DxvkShaderMetadata& metadata) {
    bool status = write(stream, metadata.stage)
               && write(stream, metadata.flags)
               && write(stream, metadata.specConstantMask)
               && writeShaderIo(stream, metadata.inputs)
               && writeShaderIo(stream, metadata.outputs)
               && write(stream, metadata.inputTopology)
               && write(stream, metadata.outputTopology)
               && write(stream, metadata.flatShadingInputs)
               && write(stream, metadata.rasterizedStream)
               && write(stream, metadata.patchVertexCount);

    for (const auto& xfb : metadata.xfbStrides)
      status = status && write(stream, xfb);

    return status;
  }


  // The record is built in memory and the caller appends it to the blob at baseOffset, so the
  // offsets this returns are absolute, as the look-up table needs them.
  std::optional<DxvkShaderCache::LutEntry> DxvkShaderCache::writeShaderBinary(ByteWriter& stream,
          uint64_t baseOffset, DxvkIrShader& shader) {
    auto [data, size] = shader.getSerializedIr();

    LutEntry entry = { };
    entry.offset = baseOffset + stream.data.size();
    if (!size || size > MaxBinarySize) return std::nullopt;
    entry.binarySize = uint32_t(size);

    if (!writeBytes(stream, data, size)
     || !writeShaderMetadata(stream, shader.getShaderMetadata())
     || !writeShaderLayout(stream, shader.getLayout()))
      return std::nullopt;

    entry.metadataSize = uint32_t(baseOffset + stream.data.size() - (entry.offset + entry.binarySize));
    if (entry.metadataSize > MaxMetadataSize) return std::nullopt;
    entry.checksum = bit::fnv1a_hash(stream.data.data(), stream.data.size());
    return std::make_optional(entry);
  }


  bool DxvkShaderCache::writeHeader(ByteWriter& stream, const LutHeader& header) {
    return writeBytes(stream, header.magic.data(), header.magic.size())
        && writeString(stream, header.versionString)
        && writeBytes(stream, header.generation.data(), header.generation.size());
  }


  DxvkShaderCache::FilePaths DxvkShaderCache::getDefaultFilePaths(const std::string& directory) {
    // The host decides where the cache goes: the BC-250 engine passes the directory of the process's
    // own profile, or no directory at all when the process may keep no cache (shader_cache_policy.h).
    std::string cachePath = directory;

#ifdef AMDGPU_WDDM_LAB_FLAVOUR
    if (cachePath.empty())
      cachePath = env::getEnvVar("DXVK_SHADER_CACHE_PATH");
#endif

#ifdef AMDGPU_WDDM_LAB_FLAVOUR
    if (cachePath.empty()) {
      #ifdef _WIN32
      cachePath = env::getEnvVar("LOCALAPPDATA");
      #endif

#ifdef AMDGPU_WDDM_LAB_FLAVOUR
      if (cachePath.empty())
        cachePath = env::getEnvVar("XDG_CACHE_HOME");
#endif

      if (cachePath.empty()) {
        cachePath = env::getEnvVar("HOME");

        if (!cachePath.empty()) {
          cachePath += env::PlatformDirSlash;
          cachePath += ".cache";
        }
      }

      if (!cachePath.empty()) {
        cachePath += env::PlatformDirSlash;
        cachePath += "dxvk";
      }
    }

#endif

    if (cachePath.empty())
      return FilePaths();

    // Determine file name based on the actual executable,
    // including the containing directory.
    std::string exePath = env::getExePath();

    if (exePath.empty())
      return FilePaths();

    uint64_t hash = bit::fnv1a_init();

    for (size_t i = 0; i < exePath.size(); i++)
      hash = bit::fnv1a_iter(hash, uint8_t(exePath[i]));

    // The identity goes into the name as well as into the header: two driver builds then use two
    // pairs of files and never fight over one, and the cache of the build that is no longer
    // installed is recognizable and can be deleted.
    auto identity = buildIdentity();

    if (identity.empty())
      return FilePaths();

    uint64_t identityHash = bit::fnv1a_hash(identity.data(), identity.size());

    std::string baseName = str::format(std::hex, std::setw(16u), std::setfill('0'), hash,
      "-", std::setw(16u), std::setfill('0'), identityHash);

    FilePaths paths;
    paths.directory = cachePath;
    paths.lutFile = baseName + ".dxvk.lut";
    paths.binFile = baseName + ".dxvk.bin";
    return paths;
  }


  Rc<DxvkShaderCache> DxvkShaderCache::getInstance(const std::string& directory,
    bool (*accessAllowed)()) {
    try {
      if (accessAllowed && !accessAllowed()) return nullptr;
      std::lock_guard lock(s_instance.mutex);
      if (!s_instance.instance)
        s_instance.instance = new DxvkShaderCache(directory, accessAllowed);
      if (s_instance.instance->m_directory != directory || s_instance.instance->m_accessAllowed != accessAllowed)
        return nullptr;
      return s_instance.instance;
    } catch (...) { return nullptr; }
  }


  void DxvkShaderCache::freeInstance() {
    std::lock_guard lock(s_instance.mutex);
    // Serialize the final decrement with getInstance's possible revival. Two
    // delayed final decrements must not both try to destroy the same instance.
    if (m_useCount.fetch_sub(1u) != 1u) return;
    if (s_instance.instance == this) s_instance.instance = nullptr;
    delete this;
  }


  size_t DxvkShaderCache::LutKey::hash() const {
    DxvkHashState hash;
    hash.add(bit::fnv1a_hash(name.data(), name.size()));
    hash.add(createInfo.hash());
    return hash;
  }


  bool DxvkShaderCache::LutKey::eq(const LutKey& k) const {
    return name == k.name && createInfo.eq(k.createInfo);
  }

}
