#include <iomanip>
#include <version.h>

#include "dxvk_shader_cache.h"

#include "../util/util_time.h"

namespace dxvk {

  DxvkShaderCache::Instance DxvkShaderCache::s_instance;

  DxvkShaderCache::DxvkShaderCache(const std::string& directory)
  : m_filePaths(getDefaultFilePaths(directory)) {

  }


  DxvkShaderCache::~DxvkShaderCache() {
    if (m_writer.joinable()) {
      // Write what is translated, skip the rest: this runs when the
      // last device goes away, on a thread inside the application.
      m_stopping.store(true);

      { std::unique_lock lock(m_writeMutex);
        m_writeQueue.push(nullptr);
        m_writeCond.notify_one();
      }

      m_writer.join();
    }

    if (m_status.load() != Status::Uninitialized) {
      Logger::info(str::format("Shader cache closed: hits=", m_lookupHits.load(), " misses=", m_lookupMisses.load(),
        " written=", m_written.load(), " skipped=", m_skipped.load()));
    }
  }


  Rc<DxvkIrShader> DxvkShaderCache::lookupShader(
    const std::string&                name,
    const DxvkIrShaderCreateInfo&     options) {
    if (!ensureStatus(Status::OpenReadWrite)) {
      // A new or discarded cache: nothing to find, but the shader will be written
      if (m_status.load() == Status::OpenWriteOnly)
        m_lookupMisses += 1u;

      return nullptr;
    }

    LutKey k = { };
    k.name = name;
    k.createInfo = options;

    auto entry = m_lut.find(k);

    if (entry == m_lut.end()) {
      if (Logger::logLevel() <= LogLevel::Debug)
        Logger::debug(str::format("Shader cache miss: ", name));

      m_lookupMisses += 1u;
      return nullptr;
    }

    if (Logger::logLevel() <= LogLevel::Debug) {
      Logger::debug(str::format("Shader cache hit: ", name,
        " (offset: ", entry->second.offset,
        ", size: ", entry->second.binarySize,
        ", metadata: ", entry->second.metadataSize, ")"));
    }

    std::unique_lock lock(m_fileMutex);
    auto shader = loadCachedShaderLocked(entry->first, entry->second);

    if (!shader) {
      Logger::warn(str::format("Failed to load cached shader ", name));

      if (!openWriteOnlyLocked())
        Logger::warn(str::format("Failed to re-initialize shader cache ", name));

      m_status.store(Status::OpenWriteOnly);
      m_lookupMisses += 1u;
    } else {
      m_lookupHits += 1u;
    }

    return shader;
  }


  void DxvkShaderCache::addShader(Rc<DxvkIrShader> shader) {
    if (!ensureStatus(Status::OpenWriteOnly))
      return;

    LutKey k = { };
    k.name = shader->debugName();
    k.createInfo = shader->getShaderCreateInfo();

    if (m_lut.find(k) == m_lut.end()) {
      std::unique_lock lock(m_writeMutex);

      if (!m_queuedKeys.insert(std::move(k)).second)
        return;

      m_writeQueue.push(std::move(shader));
      m_writeCond.notify_one();

      if (!m_writer.joinable())
        m_writer = dxvk::thread([this] { runWriter(); });
    }
  }


  DxvkShaderCacheStats DxvkShaderCache::getStats() const {
    DxvkShaderCacheStats result = { };
    result.lookupHits   = m_lookupHits.load();
    result.lookupMisses = m_lookupMisses.load();
    result.written      = m_written.load();
    result.skipped      = m_skipped.load();
    return result;
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
    if (m_filePaths.directory.empty() || m_filePaths.binFile.empty() || m_filePaths.lutFile.empty()) {
      Logger::warn("No path found for shader cache, consider setting DXVK_SHADER_CACHE_PATH.");
      return Status::CacheDisabled;
    }

    if (openReadWriteLocked()) {
      if (parseLut())
        return Status::OpenReadWrite;
    }

    if (openWriteOnlyLocked())
      return Status::OpenWriteOnly;

    return Status::CacheDisabled;
  }


  bool DxvkShaderCache::openReadWriteLocked() {
    // Try to open both files in read-only mode for now, re-open
    // in read-write mode when we actually add new cache entries.
    auto path = m_filePaths.directory + env::PlatformDirSlash;

    auto flags = util::FileFlags(
      util::FileFlag::AllowRead,
      util::FileFlag::AllowWrite,
      util::FileFlag::Exclusive);

    m_binFile.open(path + m_filePaths.binFile, flags);
    m_lutFile.open(path + m_filePaths.lutFile, flags);

    if (!m_binFile || !m_lutFile)
      return false;

    Logger::info(str::format("Found cache file: ", path + m_filePaths.binFile));
    return true;
  }


  bool DxvkShaderCache::openWriteOnlyLocked() {
    // Didn't have a lot of success so far, nuke the files and retry.
    auto path = m_filePaths.directory + env::PlatformDirSlash;

    auto flags = util::FileFlags(
      util::FileFlag::AllowWrite,
      util::FileFlag::Truncate,
      util::FileFlag::Exclusive);

    m_binFile.open(path + m_filePaths.binFile, flags);
    m_lutFile.open(path + m_filePaths.lutFile, flags);

    if (!m_binFile || !m_lutFile) {
      if (!createDirectories(m_filePaths.directory)) {
        Logger::warn(str::format("Failed to create directory: ", m_filePaths.directory));
        return false;
      }

      m_binFile.open(path + m_filePaths.binFile, flags);
      m_lutFile.open(path + m_filePaths.lutFile, flags);
    }

    if (!m_binFile)
      Logger::warn(str::format("Failed to create ", path + m_filePaths.binFile, ", disabling cache"));

    if (!m_lutFile)
      Logger::warn(str::format("Failed to create ", path + m_filePaths.lutFile, ", disabling cache"));

    if (!m_binFile || !m_lutFile)
      return false;

    Logger::info(str::format("Created cache file: ", path + m_filePaths.binFile));

    LutHeader header = { };
    header.magic = { 'D', 'X', 'V', 'K' };
    header.versionString = DXVK_VERSION;

    if (!writeHeader(m_lutFile, header)) {
      Logger::warn(str::format("Failed to write cache header: ", path + m_filePaths.lutFile));
      return false;
    }

    return true;
  }


  bool DxvkShaderCache::parseLut() {
    LutHeader header;

    size_t size = m_lutFile.size();
    size_t offset = 0u;

    if (!readBytes(m_lutFile, header.magic.data(), offset, header.magic.size())
     || !readString(m_lutFile, offset, header.versionString)) {
      Logger::warn("Failed to parse cache file header.");
      return false;
    }

    if (header.versionString != DXVK_VERSION) {
      Logger::warn(str::format("Cache was created with DXVK version ", header.versionString,
        ", but current version is ", DXVK_VERSION, ". Discarding old cache."));
      return false;
    }

    auto cacheSize = m_binFile.size();

    while (offset < size) {
      LutKey k;
      LutEntry e;

      size_t entryOffset = offset;

      // An entry cut short, or one whose data is not in the binary file: the
      // process or the system died while writing. Keep the entries before it.
      if (!readShaderLutEntry(k, e, offset)
       || e.offset + e.binarySize + e.metadataSize > cacheSize) {
        Logger::warn(str::format("Cache look-up table ends in an incomplete entry, dropping ",
          size - entryOffset, " bytes."));

        if (!m_lutFile.truncate(entryOffset)) {
          Logger::warn("Failed to parse cache look-up table.");
          m_lut.clear();
          return false;
        }

        break;
      }

      m_lut.insert_or_assign(k, e);
    }

    std::stringstream message;
    message << "Cache: " << m_lut.size() << " shaders (";

    if (cacheSize >= (1ull << 20)) {
      auto mib = (10ull * cacheSize) >> 20;
      message << (mib / 10ull) << "." << (mib % 10ull) << " MB";
    } else {
      message << (cacheSize >> 10u) << " kB";
    }

    message << ")";

    Logger::info(message.str());
    return true;
  }


  bool DxvkShaderCache::writeShaderXfbInfo(util::File& stream, const dxbc_spv::ir::IoXfbInfo& xfb) {
    return writeString(stream, xfb.semanticName)
        && write(stream, xfb.semanticIndex)
        && write(stream, xfb.componentMask)
        && write(stream, xfb.stream)
        && write(stream, xfb.buffer)
        && write(stream, xfb.offset)
        && write(stream, xfb.stride);
  }


  bool DxvkShaderCache::writeShaderCreateInfo(util::File& stream, const DxvkIrShaderCreateInfo& createInfo) {
    bool status = write(stream, createInfo.options)
               && write(stream, createInfo.flatShadingInputs)
               && write(stream, createInfo.rasterizedStream);

    status = status && write(stream, uint32_t(createInfo.xfbEntries.size()));

    for (const auto& xfb : createInfo.xfbEntries)
      status = status && writeShaderXfbInfo(stream, xfb);

    return status;
  }


  Rc<DxvkIrShader> DxvkShaderCache::loadCachedShaderLocked(const LutKey& key, const LutEntry& entry) {
    std::vector<uint8_t> ir(entry.binarySize);

    size_t offset = entry.offset;

    if (!readBytes(m_binFile, ir.data(), offset, entry.binarySize)) {
      Logger::warn("Failed to read cached shader binary");
      return nullptr;
    }

    if (entry.checksum != bit::fnv1a_hash(ir.data(), ir.size())) {
      Logger::warn("Checksum mismatch for cached shader");
      return nullptr;
    }

    DxvkShaderMetadata metadata;

    if (!readShaderMetadata(m_binFile, offset, metadata)) {
      Logger::warn("Failed to read cached shader metadata");
      return nullptr;
    }

    DxvkPipelineLayoutBuilder layout;

    if (!readShaderLayout(m_binFile, offset, layout)) {
      Logger::warn("Failed to read cached shader binding layout");
      return nullptr;
    }

    return new DxvkIrShader(key.name, key.createInfo, std::move(metadata), std::move(layout), std::move(ir));
  }


  bool DxvkShaderCache::writeShaderLutEntry(DxvkIrShader& shader, const LutEntry& entry) {
    return writeString(m_lutFile, shader.debugName())
        && writeShaderCreateInfo(m_lutFile, shader.getShaderCreateInfo())
        && write(m_lutFile, entry);
  }


  bool DxvkShaderCache::writeShaderToCache(DxvkIrShader& shader) {
    auto entry = writeShaderBinary(m_binFile, shader);

    if (!entry)
      return false;

    return writeShaderLutEntry(shader, *entry);
  }


  bool DxvkShaderCache::readShaderIo(util::File& stream, size_t& offset, DxvkShaderIo& io) {
    uint8_t varCount = 0u;

    if (!read(stream, offset, varCount))
      return false;

    for (uint32_t i = 0u; i < varCount; i++) {
      DxvkShaderIoVar var = { };

      if (!read(stream, offset, var.builtIn)
       || !read(stream, offset, var.location)
       || !read(stream, offset, var.componentIndex)
       || !read(stream, offset, var.componentCount)
       || !read(stream, offset, var.isPatchConstant)
       || !read(stream, offset, var.semanticIndex)
       || !readString(stream, offset, var.semanticName))
        return false;

      io.add(std::move(var));
    }

    return true;
  }


  bool DxvkShaderCache::readShaderMetadata(util::File& stream, size_t& offset, DxvkShaderMetadata& metadata) {
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


  bool DxvkShaderCache::readShaderLayout(util::File& stream, size_t& offset, DxvkPipelineLayoutBuilder& layout) {
    VkShaderStageFlags stageMask = { };

    if (!read(stream, offset, stageMask))
      return false;

    layout = DxvkPipelineLayoutBuilder(stageMask);

    // Read push data blocks
    uint32_t pushDataMask = 0u;

    if (!read(stream, offset, pushDataMask))
      return false;

    for (uint32_t i = 0u; i < bit::popcnt(pushDataMask); i++) {
      DxvkPushDataBlock block = { };

      if (!read(stream, offset, block))
        return false;

      layout.addPushData(block);
    }

    // Read shader binding info
    uint32_t bindingCount = 0u;

    if (!read(stream, offset, bindingCount))
      return false;

    for (uint32_t i = 0u; i < bindingCount; i++) {
      DxvkShaderDescriptor binding = { };

      if (!read(stream, offset, binding))
        return false;

      layout.addBindings(1u, &binding);
    }

    // Read sampler heap mappings
    uint32_t samplerHeapCount = 0u;

    if (!read(stream, offset, samplerHeapCount))
      return false;

    for (uint32_t i = 0u; i < samplerHeapCount; i++) {
      DxvkShaderBinding binding = { };

      if (!read(stream, offset, binding))
        return false;

      layout.addSamplerHeap(binding);
    }

    // Read spec data mappings
    uint32_t specDataCount = 0u;

    if (!read(stream, offset, specDataCount))
      return false;

    for (uint32_t i = 0u; i < specDataCount; i++) {
      DxvkShaderBinding binding = { };

      if (!read(stream, offset, binding))
        return false;

      layout.addSpecDataBuffer(binding);
    }

    return true;
  }


  bool DxvkShaderCache::readShaderXfbInfo(util::File& stream, size_t& offset, dxbc_spv::ir::IoXfbInfo& xfb) {
    return readString(stream, offset, xfb.semanticName)
        && read(stream, offset, xfb.semanticIndex)
        && read(stream, offset, xfb.componentMask)
        && read(stream, offset, xfb.stream)
        && read(stream, offset, xfb.buffer)
        && read(stream, offset, xfb.offset)
        && read(stream, offset, xfb.stride);
 }


  bool DxvkShaderCache::readShaderLutKey(util::File& stream, size_t& offset, LutKey& key) {
    bool status = readString(stream, offset, key.name)
               && read(stream, offset, key.createInfo.options)
               && read(stream, offset, key.createInfo.flatShadingInputs)
               && read(stream, offset, key.createInfo.rasterizedStream);

    uint32_t xfbCount = 0u;
    status = status && read(stream, offset, xfbCount);

    key.createInfo.xfbEntries.resize(xfbCount);

    for (uint32_t i = 0u; i < xfbCount; i++)
      status = status && readShaderXfbInfo(stream, offset, key.createInfo.xfbEntries[i]);

    return status;
  }


  bool DxvkShaderCache::readShaderLutEntry(LutKey& key, LutEntry& entry, size_t& offset) {
    return readShaderLutKey(m_lutFile, offset, key) && read(m_lutFile, offset, entry);
  }


  void DxvkShaderCache::runWriter() {
    env::setThreadName("dxvk-cache");

    // A log sink may only be called inside an engine call, see Logger.
    // Translations here are off the application's threads, as on workers.
    Logger::deferThreadLines();
    DxvkIrShader::setWorkerThread();

    bool stop = false;
    bool dirty = false;

    while (!stop) {
      Rc<DxvkIrShader> shader;

      { std::unique_lock lock(m_writeMutex);

        // Flush once the queue runs dry, not after every shader of a burst
        if (dirty && m_writeQueue.empty()) {
          lock.unlock();

          std::unique_lock fileLock(m_fileMutex);
          m_binFile.flush();
          m_lutFile.flush();
          dirty = false;

          fileLock.unlock();
          lock.lock();
        }

        m_writeCond.wait(lock, [this] {
          return !m_writeQueue.empty();
        });

        shader = std::move(m_writeQueue.front());
        m_writeQueue.pop();
      }

      if (!shader) {
        stop = true;
        continue;
      }

      // Translate outside the file lock, which lookups take. Pipeline workers
      // usually got there first, or are at it, and this waits for them. When
      // the cache closes, untranslated shaders are left for the next run.
      if (!shader->isCompileDone() && m_stopping.load()) {
        m_skipped += 1u;
        continue;
      }

      try {
        shader->compile();
      } catch (const DxvkError& e) {
        Logger::warn(str::format("Shader cache: Not writing ", shader->debugName(), ": ", e.message()));
      } catch (const std::exception& e) {
        Logger::warn(str::format("Shader cache: Not writing ", shader->debugName(), ": ", e.what()));
      }

      if (!shader->isConverted()) {
        m_skipped += 1u;
        continue;
      }

      std::unique_lock fileLock(m_fileMutex);

      if (!writeShaderToCache(*shader)) {
        Logger::err("Failed to write cache file.");
        m_status = Status::CacheDisabled;
        return;
      }

      m_written += 1u;
      dirty = true;
    }

    if (dirty) {
      std::unique_lock fileLock(m_fileMutex);
      m_binFile.flush();
      m_lutFile.flush();
    }
  }


  bool DxvkShaderCache::writeShaderLayout(util::File& stream, const DxvkPipelineLayoutBuilder& layout) {
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


  bool DxvkShaderCache::writeShaderIo(util::File& stream, const DxvkShaderIo& io) {
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


  bool DxvkShaderCache::writeShaderMetadata(util::File& stream, const DxvkShaderMetadata& metadata) {
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


  std::optional<DxvkShaderCache::LutEntry> DxvkShaderCache::writeShaderBinary(util::File& stream, DxvkIrShader& shader) {
    auto [data, size] = shader.getSerializedIr();

    LutEntry entry = { };
    entry.offset = stream.size();
    entry.binarySize = size;

    if (!writeBytes(stream, data, size)
     || !writeShaderMetadata(stream, shader.getShaderMetadata())
     || !writeShaderLayout(stream, shader.getLayout()))
      return std::nullopt;


    entry.metadataSize = uint32_t(uint64_t(stream.size()) - (entry.offset + entry.binarySize));
    entry.checksum = bit::fnv1a_hash(data, size);
    return std::make_optional(entry);
  }


  bool DxvkShaderCache::writeHeader(util::File& stream, const LutHeader& header) {
    return writeBytes(stream, header.magic.data(), header.magic.size())
        && writeString(stream, header.versionString);
  }


  DxvkShaderCache::FilePaths DxvkShaderCache::getDefaultFilePaths(const std::string& directory) {
    std::string cachePath = directory;

    if (cachePath.empty())
      cachePath = env::getEnvVar("DXVK_SHADER_CACHE_PATH");

    if (cachePath.empty()) {
      #ifdef _WIN32
      cachePath = env::getEnvVar("LOCALAPPDATA");
      #endif

      if (cachePath.empty())
        cachePath = env::getEnvVar("XDG_CACHE_HOME");

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

    if (cachePath.empty())
      return FilePaths();

    // Determine file name based on the actual executable,
    // including the containing directory.
    std::string exePath = env::getExePath();

    if (exePath.empty())
      return FilePaths();

    size_t pathStart = exePath.find_last_of(env::PlatformDirSlash);

    if (pathStart != std::string::npos)
      pathStart = exePath.find_last_of(env::PlatformDirSlash, pathStart);

    if (pathStart == std::string::npos)
      pathStart = 0u;

    uint64_t hash = bit::fnv1a_init();

    for (size_t i = pathStart; i < exePath.size(); i++)
      hash = bit::fnv1a_iter(hash, uint8_t(exePath[i]));

    std::string baseName = str::format(std::hex, std::setw(16u), std::setfill('0'), hash);

    FilePaths paths;
    paths.directory = cachePath;
    paths.lutFile = baseName + ".dxvk.lut";
    paths.binFile = baseName + ".dxvk.bin";
    return paths;
  }


  Rc<DxvkShaderCache> DxvkShaderCache::getInstance(const std::string& directory) {
    std::lock_guard lock(s_instance.mutex);

    if (!s_instance.instance)
      s_instance.instance = new DxvkShaderCache(directory);

    return s_instance.instance;
  }


  void DxvkShaderCache::releaseInstance() {
    // The ref count only goes from 0 to 1 in getInstance and from 1 to 0
    // here, both under the lock, so nothing can revive the object between
    // the last release and its destruction. Upstream decrements outside the
    // lock and tests the opposite condition, so an unused cache is never
    // freed and its writer thread outlives the last device, inside a DLL
    // that the host may unload then.
    std::lock_guard lock(s_instance.mutex);

    if (m_useCount.fetch_sub(1u) == 1u) {
      if (s_instance.instance == this)
        s_instance.instance = nullptr;

      delete this;
    }
  }


  bool DxvkShaderCache::createDirectories(const std::string& path) {
    // Every parent first: the default directory is two levels below the user's profile
    for (size_t i = path.find_first_of("/\\", 1u); i != std::string::npos; i = path.find_first_of("/\\", i + 1u)) {
      // Skip a drive root such as C:\ and a doubled separator
      if (path[i - 1u] == ':' || path[i - 1u] == '/' || path[i - 1u] == '\\')
        continue;

      env::createDirectory(path.substr(0u, i));
    }

    return env::createDirectory(path);
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
