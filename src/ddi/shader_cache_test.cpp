/* Host gate of the persistent shader cache (DxvkShaderCache), on a directory of captured DXBC
 * containers. No GPU, no Vulkan device and no lab: every claim here is about the cache, and the
 * cache touches no GPU object.
 *
 *   shader_cache_test.exe --dumps <dir> --cache <dir> [--limit N] [--case <name>]
 *
 * The cases, in the order the gate runs them with --case all:
 *
 *   identity   the build identity is not empty and its hash is part of the cache file name, so two
 *              builds of the driver can never read each other's entries.
 *   key        the key this gate computes for every container equals the name under which the lab
 *              dumped it, which shows that the gate keys shaders exactly as the engine does.
 *   roundtrip  for every container: the SPIR-V of a cache hit is byte-identical to the SPIR-V of a
 *              fresh conversion. Run twice, once with the entry read back through the look-up table
 *              that the write filled, and once after the cache was closed and its table parsed from
 *              the file, which is what a warm start of a game does.
 *   damage     a changed identity, a look-up table that ends inside an entry, a blob that was cut,
 *              a flipped bit in an entry and an entry whose bytes were replaced each give a miss
 *              with a counter, never a wrong shader and never a crash.
 *   parallel   two processes that write the same cache at the same time leave a cache that parses
 *              and that answers every key one of them wrote.
 *   timing     the cost of a miss (conversion), of the write that follows it and of a hit.
 *   unavailable  the cache cannot be created at all: it answers nothing and fails no shader.
 *   policy     the process policy admits an ordinary user process and names its own profile.
 *
 * Exit code 0 means every case the run selected passed.
 */
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <vector>
#include <windows.h>
#include <psapi.h>
#include <winioctl.h>

#include "../d3d11/d3d11_device.h"
#include "../d3d11/d3d11_shader.h"

#include "shader_cache_policy.h"

#include "../dxvk/dxvk_shader_cache.h"
#include "../dxvk/dxvk_shader_ir.h"

#include "../util/util_env.h"

using namespace dxvk;
using clock_type = std::chrono::steady_clock;

namespace dxvk {

  // Each DLL of this tree defines the logger instance; so does this program. The second argument
  // keeps it from writing a file unless a log path is set, as the engine DLL does.
  Logger Logger::s_instance("shader_cache_test.log", true);

}

namespace {

  std::string cacheDirectory;
  std::string barrier;

  struct Shader {
    std::string           name;
    VkShaderStageFlagBits stage = VK_SHADER_STAGE_FLAG_BITS_MAX_ENUM;
    std::vector<uint8_t>  dxbc;
  };

  // The compile options of one device. The cache keys every entry by these, so any fixed set
  // exercises the cache; this one is what a Vulkan 1.3 device with 16-bit arithmetic and a 16-byte
  // storage buffer alignment yields, which is the lab's case.
  DxvkIrShaderCreateInfo testCreateInfo() {
    DxvkIrShaderCreateInfo info = { };
    info.options.flags.set(DxvkShaderCompileFlag::Supports16BitArithmetic);
    info.options.maxUniformBufferSize = 65536u;
    info.options.maxUniformBufferCount = 15u;
    info.options.minStorageBufferAlignment = 16u;
    return info;
  }

  VkShaderStageFlagBits stageOfName(const std::string& name) {
    auto prefix = name.substr(0u, name.find('.'));

    if (prefix == "vs")  return VK_SHADER_STAGE_VERTEX_BIT;
    if (prefix == "fs")  return VK_SHADER_STAGE_FRAGMENT_BIT;
    if (prefix == "cs")  return VK_SHADER_STAGE_COMPUTE_BIT;
    if (prefix == "gs")  return VK_SHADER_STAGE_GEOMETRY_BIT;
    if (prefix == "tcs") return VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
    if (prefix == "tes") return VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;

    return VK_SHADER_STAGE_FLAG_BITS_MAX_ENUM;
  }

  std::vector<uint8_t> readFile(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);

    if (!file)
      return { };

    auto end=file.tellg();
    if (end<0 || uint64_t(end)>128ull*1024*1024) return { };
    auto size = size_t(end);
    file.seekg(0);

    std::vector<uint8_t> data(size);
    file.read(reinterpret_cast<char*>(data.data()), std::streamsize(size));
    return data;
  }

  std::vector<Shader> loadShaders(const std::string& directory, size_t limit) {
    std::vector<Shader> shaders;
    std::vector<std::filesystem::path> paths;

    for (const auto& item : std::filesystem::directory_iterator(directory)) {
      if (item.is_regular_file() && item.path().extension() == ".dxbc")
        paths.push_back(item.path());
    }

    // Name order, so that two runs of the gate see the same set in the same order.
    std::sort(paths.begin(), paths.end());

    for (const auto& path : paths) {
      if (limit && shaders.size() >= limit)
        break;

      Shader shader = { };
      shader.name = path.stem().string();
      shader.stage = stageOfName(shader.name);
      shader.dxbc = readFile(path);

      if (shader.stage == VK_SHADER_STAGE_FLAG_BITS_MAX_ENUM || shader.dxbc.empty()) {
        std::cerr << "cannot use " << path.string() << std::endl;
        return { };
      }

      shaders.push_back(std::move(shader));
    }

    return shaders;
  }

  // A fresh conversion of one container, with the shipped converter, and its SPIR-V.
  SpirvCodeBuffer convertFresh(const Shader& shader, const DxvkIrShaderCreateInfo& info) {
    auto key = D3D11Device::ComputeShaderKey(shader.stage, shader.dxbc.data(), shader.dxbc.size());
    auto converter = CreateD3D11ShaderConverter(key, info, shader.dxbc.data(), shader.dxbc.size(), false);

    Rc<DxvkIrShader> ir = new DxvkIrShader(info, converter);
    return ir->getCode(nullptr, nullptr);
  }

  bool sameCode(const SpirvCodeBuffer& a, const SpirvCodeBuffer& b) {
    return a.size() == b.size() && !std::memcmp(a.data(), b.data(), a.size());
  }

  struct CachePaths {
    std::filesystem::path lut;
    std::filesystem::path bin;
  };

  CachePaths cachePaths() {
    auto paths = DxvkShaderCache::getDefaultFilePaths(cacheDirectory);
    CachePaths result;
    result.lut = std::filesystem::path(paths.directory) / paths.lutFile;
    result.bin = std::filesystem::path(paths.directory) / paths.binFile;
    return result;
  }

  void removeCache() {
    auto paths = cachePaths();
    std::error_code error;
    std::filesystem::remove(paths.lut, error);
    std::filesystem::remove(paths.bin, error);
  }

  // Every cache object of this process shares one static instance, so a warm start is reproduced by
  // dropping the last reference and asking for a new one: the new object opens the files again and
  // parses the look-up table out of them, exactly as the first device of a new process does.
  Rc<DxvkShaderCache> reopenCache() {
    return DxvkShaderCache::getInstance(cacheDirectory, dxvk::ddi::ShaderCacheAccessAllowed);
  }

  bool reportCase(const char* name, bool passed) {
    std::cout << (passed ? "PASS  " : "FAIL  ") << name << std::endl;
    return passed;
  }


  bool caseIdentity() {
    auto identity = DxvkShaderCache::buildIdentity();

    if (identity.empty()) {
      std::cout << "      no build identity" << std::endl;
      return false;
    }

    auto paths = DxvkShaderCache::getDefaultFilePaths(cacheDirectory);
    std::cout << "      identity " << identity << std::endl;
    std::cout << "      files    " << paths.directory << " " << paths.lutFile << std::endl;

    // The name is <executable hash>-<identity hash>, so it holds one hyphen and two 16-digit parts.
    auto hyphen = paths.lutFile.find('-');

    if (hyphen != 16u || paths.lutFile.size() < 33u) {
      std::cout << "      the identity is not part of the file name" << std::endl;
      return false;
    }

    uint64_t identityHash = bit::fnv1a_hash(identity.data(), identity.size());
    char expected[17] = { };
    std::snprintf(expected, sizeof(expected), "%016llx", static_cast<unsigned long long>(identityHash));

    if (paths.lutFile.compare(hyphen + 1u, 16u, expected)) {
      std::cout << "      the file name does not carry this identity" << std::endl;
      return false;
    }

    return true;
  }


  bool caseKey(const std::vector<Shader>& shaders) {
    size_t bad = 0u;

    for (const auto& shader : shaders) {
      auto key = D3D11Device::ComputeShaderKey(shader.stage, shader.dxbc.data(), shader.dxbc.size());

      if (key.toString() != shader.name) {
        if (bad < 4u)
          std::cout << "      " << shader.name << " keys as " << key.toString() << std::endl;

        bad++;
      }
    }

    std::cout << "      " << (shaders.size() - bad) << " of " << shaders.size()
              << " containers key as the name they were dumped under" << std::endl;
    return !bad;
  }


  // The exhaustive claim: for every shader the SPIR-V of a hit equals the SPIR-V of a fresh
  // conversion, byte for byte. Pass 1 reads the entry back through the table that the write filled,
  // pass 2 after the cache was closed and parsed from the file, which is the warm start of a game.
  bool caseRoundtrip(const std::vector<Shader>& shaders, bool fromParsedFile, double* missMs,
      double* writeMs, double* hitMs, uint64_t* bytes) {
    auto info = testCreateInfo();
    auto cache = reopenCache();

    size_t mismatch = 0u;
    size_t absent = 0u;
    double convertTotal = 0.0;
    double writeTotal = 0.0;
    double hitTotal = 0.0;

    for (const auto& shader : shaders) {
      auto key = D3D11Device::ComputeShaderKey(shader.stage, shader.dxbc.data(), shader.dxbc.size());
      auto name = key.toString();

      // Fresh: the converter path, and the cache entry that the conversion writes at its end.
      auto t0 = clock_type::now();
      auto converter = CreateD3D11ShaderConverter(key, info, shader.dxbc.data(), shader.dxbc.size(), false);
      Rc<DxvkIrShader> fresh = new DxvkIrShader(info, converter);

      if (!fromParsedFile)
        cache->addShader(fresh);

      auto freshCode = fresh->getCode(nullptr, nullptr);
      auto t1 = clock_type::now();

      // The write is inside the conversion above, so it cannot be timed apart from it here. It is
      // measured on its own in caseTiming.
      convertTotal += std::chrono::duration<double, std::milli>(t1 - t0).count();

      auto t2 = clock_type::now();
      Rc<DxvkIrShader> cached = cache->lookupShader(name, info);
      SpirvCodeBuffer cachedCode;

      if (cached != nullptr)
        cachedCode = cached->getCode(nullptr, nullptr);

      auto t3 = clock_type::now();
      hitTotal += std::chrono::duration<double, std::milli>(t3 - t2).count();

      if (cached == nullptr) {
        if (absent < 4u)
          std::cout << "      no cache entry for " << name << std::endl;

        absent++;
        continue;
      }

      if (!sameCode(freshCode, cachedCode)) {
        if (mismatch < 4u) {
          std::cout << "      " << name << ": fresh " << freshCode.size()
                    << " bytes, cached " << cachedCode.size() << " bytes" << std::endl;
        }

        mismatch++;
      }

      if (bytes)
        *bytes += freshCode.size();
    }

    auto stats = cache->getStats();
    std::cout << "      " << (fromParsedFile ? "parsed table: " : "written table: ")
              << stats.hits << " hits, " << stats.misses << " misses, " << stats.written
              << " written, " << stats.rejected << " rejected, " << stats.failed << " failed"
              << std::endl;
    std::cout << "      " << (shaders.size() - mismatch - absent) << " of " << shaders.size()
              << " shaders identical, " << mismatch << " different, " << absent << " absent"
              << std::endl;

    if (missMs)  *missMs = convertTotal;
    if (writeMs) *writeMs = writeTotal;
    if (hitMs)   *hitMs = hitTotal;

    return !mismatch && !absent;
  }


  // Overwrites a range of a file. Used to damage a cache on purpose.
  bool patchFile(const std::filesystem::path& path, uint64_t offset, const void* data, size_t size) {
    std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);

    if (!file)
      return false;

    file.seekp(std::streamoff(offset));
    file.write(reinterpret_cast<const char*>(data), std::streamsize(size));
    return bool(file);
  }

  bool shortenFile(const std::filesystem::path& path, uint64_t size) {
    std::error_code error;
    std::filesystem::resize_file(path, size, error);
    return !error;
  }

  template<typename T> T field(const std::vector<uint8_t>& data, size_t offset) {
    T value{};
    if (offset <= data.size() && sizeof(T) <= data.size()-offset)
      std::memcpy(&value, data.data()+offset, sizeof(T));
    return value;
  }

  template<typename T> void put(std::vector<uint8_t>& data, size_t offset, T value) {
    if (offset <= data.size() && sizeof(T) <= data.size()-offset)
      std::memcpy(data.data()+offset, &value, sizeof(T));
  }

  template<typename T> void append(std::vector<uint8_t>& data, const T& value) {
    const auto begin=reinterpret_cast<const uint8_t*>(&value);
    data.insert(data.end(),begin,begin+sizeof(T));
  }

  size_t shaderIoSize(const DxvkShaderIo& io) {
    size_t size=sizeof(uint8_t);
    for(uint32_t i=0;i<io.getVarCount();i++) {
      const auto& var=io.getVar(i);
      size+=sizeof(var.builtIn)+sizeof(var.location)+sizeof(var.componentIndex)+sizeof(var.componentCount)
        +sizeof(var.isPatchConstant)+sizeof(var.semanticIndex)+sizeof(uint16_t)+var.semanticName.size();
    }
    return size;
  }

  size_t shaderMetadataSize(const DxvkShaderMetadata& meta) {
    return sizeof(meta.stage)+sizeof(meta.flags)+sizeof(meta.specConstantMask)
      +shaderIoSize(meta.inputs)+shaderIoSize(meta.outputs)+sizeof(meta.inputTopology)+sizeof(meta.outputTopology)
      +sizeof(meta.flatShadingInputs)+sizeof(meta.rasterizedStream)+sizeof(meta.patchVertexCount)+sizeof(meta.xfbStrides);
  }

  size_t privateBytes() {
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb=sizeof(counters);
    return GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
      sizeof(counters)) ? counters.PrivateUsage : 0;
  }

  // Fills the cache with the given shaders. With half 0 or 1 only the shaders whose index has that
  // parity are written, which is how the parallel case gives each of its two processes its own work.
  // Returns the number of entries written; failures are counted apart.
  uint32_t fillCache(const std::vector<Shader>& shaders, int half = -1, uint32_t* failures = nullptr) {
    auto info = testCreateInfo();
    auto cache = reopenCache();

    for (size_t i = 0u; i < shaders.size(); i++) {
      if (half >= 0 && int(i % 2u) != half)
        continue;

      const auto& shader = shaders[i];
      auto key = D3D11Device::ComputeShaderKey(shader.stage, shader.dxbc.data(), shader.dxbc.size());
      auto converter = CreateD3D11ShaderConverter(key, info, shader.dxbc.data(), shader.dxbc.size(), false);

      Rc<DxvkIrShader> ir = new DxvkIrShader(info, converter);
      cache->addShader(ir);
      ir->compile();
    }

    auto stats = cache->getStats();

    if (failures)
      *failures = stats.failed;

    return stats.written;
  }

  // Counts the shaders the cache answers, and the shaders whose answer differs from a fresh
  // conversion. A damaged cache may answer fewer shaders; it may never answer a wrong one.
  struct Answers {
    uint32_t hits = 0u;
    uint32_t misses = 0u;
    uint32_t wrong = 0u;
    uint32_t rejected = 0u;
  };

  Answers readCache(const std::vector<Shader>& shaders, Rc<DxvkShaderCache> cache=nullptr) {
    auto info = testCreateInfo();
    if(cache==nullptr) cache = reopenCache();
    Answers answers = { };

    for (const auto& shader : shaders) {
      auto key = D3D11Device::ComputeShaderKey(shader.stage, shader.dxbc.data(), shader.dxbc.size());
      Rc<DxvkIrShader> cached = cache->lookupShader(key.toString(), info);

      if (cached == nullptr) {
        answers.misses++;
        continue;
      }

      answers.hits++;

      if (!sameCode(cached->getCode(nullptr, nullptr), convertFresh(shader, info)))
        answers.wrong++;
    }

    answers.rejected = cache->getStats().rejected;
    return answers;
  }


  bool caseDamage(const std::vector<Shader>& shaders) {
    auto paths = cachePaths();
    bool passed = true;

    // The reference: an undamaged cache answers every shader, and answers each correctly.
    removeCache();
    uint32_t written = fillCache(shaders);
    auto reference = readCache(shaders);

    std::cout << "      undamaged: " << written << " written, " << reference.hits << " hits, "
              << reference.wrong << " wrong" << std::endl;

    if (!reference.hits || reference.misses || reference.wrong) {
      std::cout << "      the undamaged cache does not answer every shader" << std::endl;
      passed = false;
    }

    auto lutSize = uint64_t(std::filesystem::file_size(paths.lut));
    auto binSize = uint64_t(std::filesystem::file_size(paths.bin));
    auto goodLut = readFile(paths.lut);
    auto goodBin = readFile(paths.bin);

    auto restore = [&] {
      std::ofstream lut(paths.lut, std::ios::binary | std::ios::trunc);
      lut.write(reinterpret_cast<const char*>(goodLut.data()), std::streamsize(goodLut.size()));
      std::ofstream bin(paths.bin, std::ios::binary | std::ios::trunc);
      bin.write(reinterpret_cast<const char*>(goodBin.data()), std::streamsize(goodBin.size()));
    };

    // (1) A changed identity in the header. The whole cache goes and every shader is a miss; it
    // must not be read as if it belonged to this build.
    { auto identity = DxvkShaderCache::buildIdentity();
      // The identity string starts after the 4-byte magic and its 2-byte length.
      std::string other = identity;
      other[other.size() - 1u] = other[other.size() - 1u] == 'x' ? 'y' : 'x';
      patchFile(paths.lut, 6u, other.data(), other.size());

      auto answers = readCache(shaders);
      std::cout << "      changed identity: " << answers.hits << " hits, " << answers.misses
                << " misses, " << answers.wrong << " wrong" << std::endl;

      if (answers.hits || answers.wrong)
        passed = false;

      restore();
    }

    // (2) A look-up table that ends inside an entry, which is what a process killed inside an
    // append leaves. The entries before it must still answer.
    { shortenFile(paths.lut, lutSize - 8u);

      auto answers = readCache(shaders);
      std::cout << "      torn look-up table: " << answers.hits << " hits, " << answers.misses
                << " misses, " << answers.wrong << " wrong" << std::endl;

      if (!answers.hits || answers.hits+1!=shaders.size() || answers.wrong || !answers.rejected)
        passed = false;

      // And a write after the repair must leave a cache that parses again.
      fillCache(shaders);
      auto repaired = readCache(shaders);
      std::cout << "      after the repair: " << repaired.hits << " hits, " << repaired.wrong
                << " wrong" << std::endl;

      if (repaired.hits != shaders.size() || repaired.wrong)
        passed = false;

      restore();
    }

    // (3) A blob that was cut. Every entry that points past its end must be refused at the parse,
    // and the rest must still answer.
    { shortenFile(paths.bin, binSize / 2u);

      auto answers = readCache(shaders);
      std::cout << "      cut blob: " << answers.hits << " hits, " << answers.misses
                << " misses, " << answers.wrong << " wrong, " << answers.rejected
                << " rejected" << std::endl;

      if (answers.wrong || !answers.rejected)
        passed = false;

      restore();
    }

    // (4) A flipped bit in the serialized IR of the first entry. Its checksum must catch it.
    { uint8_t byte = goodBin[0] ^ 0x40u;
      patchFile(paths.bin, 0u, &byte, 1u);

      auto answers = readCache(shaders);
      std::cout << "      flipped bit: " << answers.hits << " hits, " << answers.misses
                << " misses, " << answers.wrong << " wrong, " << answers.rejected
                << " rejected" << std::endl;

      if (answers.wrong || !answers.rejected || answers.misses != 1u)
        passed = false;

      restore();
    }

    // (5) Foreign bytes in place of the IR of the first entry, as many as were there. The checksum
    // catches these too; the case shows that the cache refuses them rather than deserializing them.
    { std::mt19937_64 random(1u);
      std::vector<uint8_t> noise(std::min<size_t>(goodBin.size(), 4096u));

      for (auto& value : noise)
        value = uint8_t(random());

      patchFile(paths.bin, 0u, noise.data(), noise.size());

      auto answers = readCache(shaders);
      std::cout << "      foreign bytes: " << answers.hits << " hits, " << answers.misses
                << " misses, " << answers.wrong << " wrong, " << answers.rejected
                << " rejected" << std::endl;

      if (answers.wrong || !answers.rejected)
        passed = false;

      restore();
    }

    // Format 2 frames are bounded before deserializing their payload. Mutating a size while
    // recomputing the frame checksum reaches the range checks rather than just the checksum gate.
    const size_t frame=6u+field<uint16_t>(goodLut,4u)+16u;
    const uint32_t payload=field<uint32_t>(goodLut,frame);
    if (payload<24u || frame+12u+payload>goodLut.size()) return false;
    const size_t entry=frame+12u+payload-24u;
    for (unsigned arm=0;arm<6;arm++) {
      auto damaged=goodLut;
      switch(arm) {
        case 0: put<uint32_t>(damaged,frame,0xffffffffu);break;
        case 1: damaged[frame+4u]^=0x40u;break;
        case 2: put<uint64_t>(damaged,entry,UINT64_MAX-1u);break;
        case 3: put<uint32_t>(damaged,entry+8u,(16u<<20)+1u);break;
        case 4: put<uint32_t>(damaged,entry+12u,(1u<<20)+1u);break;
        case 5: put<uint32_t>(damaged,entry+8u,0u);break;
      }
      if (arm>=2) put<uint64_t>(damaged,frame+4u,
        bit::fnv1a_hash(damaged.data()+frame+12u,payload));
      if (!patchFile(paths.lut,0,damaged.data(),damaged.size())) return false;
      const auto key=D3D11Device::ComputeShaderKey(shaders[0].stage,shaders[0].dxbc.data(),shaders[0].dxbc.size());
      const auto before=privateBytes();
      auto probe=reopenCache();
      probe->lookupShader(key.toString(),testCreateInfo());
      const auto after=privateBytes();
      auto answers=readCache(shaders,probe);
      probe=nullptr;
      const bool bounded=before && after && (after<=before || after-before<32u*1024u*1024u);
      std::cout << "      framed damage " << arm << ": " << answers.hits << " hits, "
        << answers.rejected << " rejected, bounded=" << bounded << std::endl;
      passed=answers.misses && !answers.wrong && answers.rejected && bounded && passed;
      restore();
    }

    // A metadata corruption must also be covered by the binary checksum, not just the IR bytes.
    const uint64_t metadataAt=field<uint64_t>(goodLut,entry)+field<uint32_t>(goodLut,entry+8u);
    if (metadataAt>=goodBin.size()) return false;
    const uint8_t badMetadata=goodBin[size_t(metadataAt)]^0x80u;
    if (!patchFile(paths.bin,metadataAt,&badMetadata,1u)) return false;
    const auto metadataAnswers=readCache(shaders);
    passed=metadataAnswers.misses==1u && metadataAnswers.rejected && !metadataAnswers.wrong && passed;
    restore();

    // Preserve real converted IR and metadata, then append a malformed layout. Both checksums
    // are correct, so these cases reach the structural parser instead of the corruption check.
    const auto& first=shaders[0];
    const auto firstKey=D3D11Device::ComputeShaderKey(first.stage,first.dxbc.data(),first.dxbc.size());
    auto converter=CreateD3D11ShaderConverter(firstKey,testCreateInfo(),first.dxbc.data(),first.dxbc.size(),false);
    Rc<DxvkIrShader> fresh=new DxvkIrShader(testCreateInfo(),converter);
    fresh->compile();
    const auto prefixSize=field<uint32_t>(goodLut,entry+8u)+shaderMetadataSize(fresh->getShaderMetadata());
    const auto baseOffset=field<uint64_t>(goodLut,entry);
    if(baseOffset>goodBin.size() || prefixSize>goodBin.size()-baseOffset) return false;
    for(unsigned arm=0;arm<2;arm++) {
      std::vector<uint8_t> blob(goodBin.begin()+size_t(baseOffset),goodBin.begin()+size_t(baseOffset)+prefixSize);
      append(blob,VkShaderStageFlags(first.stage));
      if(arm==0) {
        append(blob,uint32_t(1u<<DxvkPushDataBlock::computeIndex(first.stage)));
        append(blob,DxvkPushDataBlock(first.stage,256u,4u,4u,1u));
        append(blob,uint32_t(0));
      } else {
        append(blob,uint32_t(0));
        append(blob,uint32_t(1));
        DxvkBindingInfo binding{};
        binding.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binding.descriptorCount=1;
        binding.access=VK_ACCESS_SHADER_READ_BIT;
        append(blob,DxvkShaderDescriptor(binding,0x80u));
      }
      append(blob,uint32_t(0));append(blob,uint32_t(0));
      auto lut=goodLut;
      put<uint64_t>(lut,entry,goodBin.size());
      put<uint32_t>(lut,entry+12u,uint32_t(blob.size()-field<uint32_t>(goodLut,entry+8u)));
      put<uint64_t>(lut,entry+16u,bit::fnv1a_hash(blob.data(),blob.size()));
      put<uint64_t>(lut,frame+4u,bit::fnv1a_hash(lut.data()+frame+12u,payload));
      if(!patchFile(paths.bin,goodBin.size(),blob.data(),blob.size())
        || !patchFile(paths.lut,0,lut.data(),lut.size())) return false;
      const auto answers=readCache(shaders);
      std::cout << "      checksummed layout " << arm << ": " << answers.misses << " misses, "
        << answers.rejected << " rejected" << std::endl;
      passed=answers.misses==1u && answers.rejected && !answers.wrong && passed;
      restore();
    }
    return passed;
  }

  bool caseCap(const std::vector<Shader>& shaders) {
    if(shaders.size()<2) return false;
    removeCache();
    std::vector<Shader> first{shaders[0]}, second{shaders[1]};
    if(fillCache(first)!=1u) return false;
    auto paths=cachePaths();
    const auto lutSize=std::filesystem::file_size(paths.lut);
    HANDLE file=CreateFileW(paths.bin.c_str(),GENERIC_WRITE,0,nullptr,OPEN_EXISTING,0,nullptr);
    if(file==INVALID_HANDLE_VALUE) return false;
    DWORD returned=0;
    LARGE_INTEGER end{};end.QuadPart=384ll<<20;
    bool padded=DeviceIoControl(file,FSCTL_SET_SPARSE,nullptr,0,nullptr,0,&returned,nullptr)
      && SetFilePointerEx(file,end,nullptr,FILE_BEGIN) && SetEndOfFile(file);
    CloseHandle(file);
    if(!padded) return false;
    const auto wrote=fillCache(second);
    const auto existing=readCache(first);
    bool passed=!wrote && existing.hits==1 && !existing.wrong
      && std::filesystem::file_size(paths.bin)==uint64_t(end.QuadPart)
      && std::filesystem::file_size(paths.lut)==lutSize;
    std::cout << "      bin cap: new writes=" << wrote << ", retained hits=" << existing.hits << std::endl;
    removeCache();
    return passed;
  }

  bool caseParallel(const std::vector<Shader>& shaders, const std::string& dumps,
      const std::string& self, size_t limit, const std::string& cacheDir) {
    removeCache();

    barrier="Local\\dxvk-cache-test-"+std::to_string(GetCurrentProcessId());
    HANDLE ready=CreateEventA(nullptr,TRUE,FALSE,(barrier+"-ready").c_str());
    HANDLE go=CreateEventA(nullptr,TRUE,FALSE,(barrier+"-go").c_str());
    if(!ready || !go) { if(ready)CloseHandle(ready);if(go)CloseHandle(go);return false; }
    auto held=reopenCache();
    held->lookupShader("parallel-initialize",testCreateInfo());

    // The child writes the other half of the set into the same files at the same time as this one,
    // so both processes really append and the cache must end up holding the work of both.
    auto command = str::format("\"", self, "\" --dumps \"", dumps, "\" --case parallel-child",
      " --limit ", limit, " --cache \"", cacheDir, "\" --keep-cache --barrier \"",barrier,"\"");

    STARTUPINFOA startup = { };
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = { };

    std::string line = command;

    if (!CreateProcessA(nullptr, line.data(), nullptr, nullptr, FALSE,
        0u, nullptr, nullptr, &startup, &process)) {
      std::cout << "      cannot start the second process" << std::endl;
      CloseHandle(ready);CloseHandle(go);
      return false;
    }

    DWORD readyWait=WaitForSingleObject(ready,120000);
    SetEvent(go);
    uint32_t failures = 0u;
    uint32_t written = readyWait==WAIT_OBJECT_0 ? fillCache(shaders, 1, &failures) : 0;
    DWORD wait=WaitForSingleObject(process.hProcess,120000);
    if(wait!=WAIT_OBJECT_0) { TerminateProcess(process.hProcess,3);WaitForSingleObject(process.hProcess,5000); }
    CloseHandle(ready);CloseHandle(go);
    held=nullptr;

    DWORD childStatus = 1u;
    GetExitCodeProcess(process.hProcess, &childStatus);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);

    auto answers = readCache(shaders);
    std::cout << "      two writers: this process wrote " << written << " of its half, "
              << failures << " failed, child exit " << childStatus << "; then "
              << answers.hits << " hits, " << answers.misses << " misses, " << answers.wrong
              << " wrong, " << answers.rejected << " rejected" << std::endl;

    // The entries of both processes must have survived, so every shader of the set must answer,
    // and none may answer wrongly.
    return readyWait==WAIT_OBJECT_0 && wait==WAIT_OBJECT_0 && written && !childStatus && !failures && !answers.misses && !answers.wrong
        && answers.hits == shaders.size();
  }


  // Process CPU time, user plus kernel. The development PC runs other work, so a wall clock prices
  // the machine's load as much as the cache; the CPU time of this process does not.
  double cpuMilliseconds() {
    FILETIME creation = { }, exit = { }, kernel = { }, user = { };

    if (!GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user))
      return 0.0;

    auto toMs = [] (const FILETIME& time) {
      uint64_t ticks = (uint64_t(time.dwHighDateTime) << 32u) | time.dwLowDateTime;
      return double(ticks) / 10000.0;
    };

    return toMs(kernel) + toMs(user);
  }


  struct Cost {
    double wall = 0.0;
    double cpu  = 0.0;

    void print(const char* what, size_t count) const {
      std::cout << "      " << what << "  " << wall / double(count) << " ms each wall, "
                << cpu / double(count) << " ms each cpu, " << wall / 1000.0 << " s wall in all"
                << std::endl;
    }
  };


  // The cost of a miss, of the write that follows it and of a hit. Each one is timed around its own
  // work, not by subtracting two passes over the whole set.
  bool caseTiming(const std::vector<Shader>& shaders) {
    auto info = testCreateInfo();
    removeCache();

    Cost convert, write, warm;
    uint32_t written = 0u;
    uint32_t hits = 0u;

    { auto cache = reopenCache();

      for (const auto& shader : shaders) {
        auto key = D3D11Device::ComputeShaderKey(shader.stage, shader.dxbc.data(), shader.dxbc.size());
        auto converter = CreateD3D11ShaderConverter(key, info, shader.dxbc.data(), shader.dxbc.size(), false);

        Rc<DxvkIrShader> ir = new DxvkIrShader(info, converter);

        // The miss: convert the container and lower it to SPIR-V, which is what a start without a
        // cache does for every shader.
        auto c0 = cpuMilliseconds();
        auto t0 = clock_type::now();
        ir->getCode(nullptr, nullptr);
        auto t1 = clock_type::now();
        auto c1 = cpuMilliseconds();

        // The write: the same call the shader makes at the end of its conversion when a cache is
        // attached (DxvkIrShader::setCacheSink).
        cache->writeShader(*ir);
        auto t2 = clock_type::now();
        auto c2 = cpuMilliseconds();

        convert.wall += std::chrono::duration<double, std::milli>(t1 - t0).count();
        convert.cpu  += c1 - c0;
        write.wall   += std::chrono::duration<double, std::milli>(t2 - t1).count();
        write.cpu    += c2 - c1;
      }

      written = cache->getStats().written;
    }

    auto paths = cachePaths();
    uint64_t irBytes = uint64_t(std::filesystem::file_size(paths.bin));

    { auto cache = reopenCache();

      for (const auto& shader : shaders) {
        auto key = D3D11Device::ComputeShaderKey(shader.stage, shader.dxbc.data(), shader.dxbc.size());

        auto c0 = cpuMilliseconds();
        auto t0 = clock_type::now();
        Rc<DxvkIrShader> cached = cache->lookupShader(key.toString(), info);

        if (cached != nullptr) {
          cached->getCode(nullptr, nullptr);
          hits++;
        }

        warm.wall += std::chrono::duration<double, std::milli>(clock_type::now() - t0).count();
        warm.cpu  += cpuMilliseconds() - c0;
      }
    }

    double count = double(shaders.size());
    std::cout << "      shaders           " << shaders.size() << ", " << written << " written, "
              << hits << " hits" << std::endl;
    convert.print("miss (convert)   ", shaders.size());
    write.print  ("write (cold cost)", shaders.size());
    warm.print   ("hit (warm start) ", shaders.size());
    // The write waits on the file system, so its wall clock is above its CPU time; the wall figure
    // is what a cold start pays and the CPU figure is near this clock's granularity.
    std::cout << "      write costs        " << 100.0 * write.wall / convert.wall
              << " % of the conversion it follows (wall), "
              << 100.0 * write.cpu / convert.cpu << " % (cpu)" << std::endl;
    std::cout << "      warm start costs   " << 100.0 * warm.wall / convert.wall
              << " % of a cold one (wall), " << convert.wall / warm.wall << "x faster"
              << std::endl;
    std::cout << "      cache blob        " << irBytes / 1024u << " kB  ("
              << double(irBytes) / count << " bytes each)" << std::endl;

    return hits == shaders.size() && written == shaders.size(); // speed is a measurement, not a correctness gate
  }


  // What the brief calls an unavailable cache: a read-only profile, a full disk, a sandboxed
  // application. Here the directory cannot be created at all, because its parent is a file. The
  // cache must then answer nothing, write nothing and never fail a shader.
  bool caseUnavailable(const std::vector<Shader>& shaders, const std::string& cacheDir) {
    auto info = testCreateInfo();
    auto blocked = std::filesystem::path(cacheDir) / "blocked-by-a-file";

    { std::ofstream file(blocked, std::ios::binary | std::ios::trunc);

      if (!file) {
        std::cout << "      could not create " << blocked.string() << std::endl;
        return false;
      }
    }

    std::string saved=cacheDirectory;
    cacheDirectory=(blocked/"child").string();

    bool passed = true;
    uint32_t absent = 0u;
    uint32_t wrong = 0u;

    { auto cache = reopenCache();
      size_t limit = std::min<size_t>(shaders.size(), 16u);

      for (size_t i = 0u; i < limit; i++) {
        const auto& shader = shaders[i];
        auto key = D3D11Device::ComputeShaderKey(shader.stage, shader.dxbc.data(), shader.dxbc.size());
        auto converter = CreateD3D11ShaderConverter(key, info, shader.dxbc.data(), shader.dxbc.size(), false);

        Rc<DxvkIrShader> ir = new DxvkIrShader(info, converter);
        cache->addShader(ir);

        // The shader still converts, and it converts correctly, with no cache under it.
        auto code = ir->getCode(nullptr, nullptr);

        if (!sameCode(code, convertFresh(shader, info)))
          wrong++;

        if (cache->lookupShader(key.toString(), info) == nullptr)
          absent++;
      }

      auto stats = cache->getStats();
      std::cout << "      no directory: " << stats.written << " written, " << stats.hits
                << " hits, " << absent << " of " << limit << " absent, " << wrong
                << " wrong" << std::endl;

      passed = !stats.written && !stats.hits && absent == limit && !wrong;
    }

    cacheDirectory=saved;
    std::error_code error;
    std::filesystem::remove(blocked, error);
    return passed;
  }


  // The process policy ported from 95bcef23: this program runs as the ordinary user a game runs as,
  // so the policy must admit it and name a directory inside that user's own profile. A policy that
  // refused an ordinary process would turn the cache off everywhere without a word.
  bool casePolicy(const std::vector<Shader>& shaders) {
    const auto ordinary=dxvk::ddi::GetShaderCachePolicy();
    if (!ordinary.enable || !dxvk::ddi::ShaderCacheAccessAllowed()) return false;
    std::string saved=env::getEnvVar("LOCALAPPDATA");
    SetEnvironmentVariableA("LOCALAPPDATA",cacheDirectory.c_str());
    const auto spoofed=dxvk::ddi::GetShaderCachePolicy();
    SetEnvironmentVariableA("LOCALAPPDATA",saved.empty() ? nullptr : saved.c_str());
    bool passed=spoofed.enable && spoofed.directory==ordinary.directory;
    HANDLE process=nullptr,duplicate=nullptr;
    if (!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY|TOKEN_DUPLICATE,&process)) return false;
    passed=dxvk::ddi::ShaderCacheTokenAllowed(process) && passed;
    HANDLE restricted=nullptr;
    SID_IDENTIFIER_AUTHORITY world=SECURITY_WORLD_SID_AUTHORITY;
    PSID worldSid=nullptr;
    if(!AllocateAndInitializeSid(&world,1,SECURITY_WORLD_RID,0,0,0,0,0,0,0,&worldSid)) {
      CloseHandle(process);return false;
    }
    SID_AND_ATTRIBUTES restrictSid{worldSid,0};
    if(!CreateRestrictedToken(process,DISABLE_MAX_PRIVILEGE,0,nullptr,0,nullptr,1,&restrictSid,&restricted))
      passed=false;
    else {
      passed=IsTokenRestricted(restricted) && !dxvk::ddi::ShaderCacheTokenAllowed(restricted) && passed;
      CloseHandle(restricted);
    }
    FreeSid(worldSid);
    if (!DuplicateTokenEx(process,TOKEN_QUERY|TOKEN_IMPERSONATE|TOKEN_ADJUST_DEFAULT,nullptr,
                         SecurityImpersonation,TokenImpersonation,&duplicate)) {
      CloseHandle(process);return false;
    }
    removeCache();
    std::vector<Shader> first{shaders[0]};
    passed=fillCache(first)==1u && passed;
    auto held=reopenCache();
    auto key=D3D11Device::ComputeShaderKey(first[0].stage,first[0].dxbc.data(),first[0].dxbc.size()).toString();
    auto shader=held->lookupShader(key,testCreateInfo());
    if(shader==nullptr) { CloseHandle(duplicate);CloseHandle(process);return false; }
    const auto paths=cachePaths();
    const auto lutBefore=readFile(paths.lut),binBefore=readFile(paths.bin);
    const auto statsBefore=held->getStats();
    if (!SetThreadToken(nullptr,duplicate)) passed=false;
    else {
      passed=!dxvk::ddi::ShaderCacheAccessAllowed() && !dxvk::ddi::GetShaderCachePolicy().enable && passed;
      passed=held->lookupShader(key,testCreateInfo())==nullptr && reopenCache()==nullptr && passed;
      held->writeShader(*shader);
      if (!RevertToSelf()) return false;
    }
    passed=readFile(paths.lut)==lutBefore && readFile(paths.bin)==binBefore
      && held->getStats().written==statsBefore.written && held->lookupShader(key,testCreateInfo())!=nullptr && passed;

    // Exercise the real low-integrity token predicate independently: impersonation itself is
    // already rejected above, so a low impersonating thread would not reach this check.
    SID_IDENTIFIER_AUTHORITY labelAuthority=SECURITY_MANDATORY_LABEL_AUTHORITY;
    PSID lowSid=nullptr;
    if(!AllocateAndInitializeSid(&labelAuthority,1,SECURITY_MANDATORY_LOW_RID,0,0,0,0,0,0,0,&lowSid)) passed=false;
    else {
      TOKEN_MANDATORY_LABEL label{{lowSid,SE_GROUP_INTEGRITY}};
      if(!SetTokenInformation(duplicate,TokenIntegrityLevel,&label,sizeof(label)+GetLengthSid(lowSid))) passed=false;
      else passed=!dxvk::ddi::ShaderCacheTokenAllowed(duplicate) && passed;
      FreeSid(lowSid);
    }
    CloseHandle(duplicate);CloseHandle(process);
    std::cout << "      ordinary/restricted/low tokens, impersonation, held-handle revocation, profile spoof checked" << std::endl;
    return passed && dxvk::ddi::ShaderCacheAccessAllowed();
  }

}


int main(int argc, char** argv) {
  std::string dumps;
  std::string cacheDir;
  std::string selected = "all";
  size_t limit = 0u;
  bool keepCache = false;

  for (int i = 1; i < argc; i++) {
    std::string arg = argv[i];

    if (arg == "--dumps" && i + 1 < argc) dumps = argv[++i];
    else if (arg == "--cache" && i + 1 < argc) cacheDir = argv[++i];
    else if (arg == "--case" && i + 1 < argc) selected = argv[++i];
    else if (arg == "--limit" && i + 1 < argc) {
      char* end=nullptr;const auto value=std::strtoull(argv[++i],&end,10);
      if(!end || *end || value>65536u) return 2;
      limit=size_t(value);
    }
    else if (arg == "--barrier" && i+1<argc) barrier=argv[++i];
    else if (arg == "--keep-cache") keepCache = true;
    else {
      std::cerr << "usage: " << argv[0] << " --dumps <dir> --cache <dir> [--case <name>]"
                   " [--limit N] [--keep-cache]" << std::endl;
      return 2;
    }
  }

  if (dumps.empty()) {
    std::cerr << "--dumps is required" << std::endl;
    return 2;
  }
  const std::vector<std::string> cases{"all","identity","key","roundtrip","damage","cap","parallel",
    "parallel-child","timing","unavailable","policy"};
  if(std::find(cases.begin(),cases.end(),selected)==cases.end()) return 2;

  if (cacheDir.empty() || !std::filesystem::path(cacheDir).is_absolute()) {
    std::cerr << "--cache must name an absolute test directory" << std::endl;return 2;
  }
  cacheDirectory=std::filesystem::path(cacheDir).lexically_normal().string();
  if (std::filesystem::path(cacheDirectory)==std::filesystem::path(cacheDirectory).root_path()) return 2;
  std::filesystem::create_directories(cacheDirectory);
  SetEnvironmentVariableA("DXVK_SHADER_CACHE",nullptr);
  SetEnvironmentVariableA("DXVK_SHADER_CACHE_PATH",nullptr);

  // Positive cache cases require an ordinary interactive process. A restricted test runner is
  // itself a negative policy case, not a cache implementation failure and not a valid positive run.
  if(selected!="policy" && !dxvk::ddi::ShaderCacheAccessAllowed()) {
    std::cerr << "cache positive cases require an admitted process token; policy refusal: "
      << dxvk::ddi::GetShaderCachePolicy().reason << std::endl;
    return 2;
  }

  auto shaders = loadShaders(dumps, limit);

  if (shaders.size()<2) {
    std::cerr << "at least two shaders are required in " << dumps << std::endl;
    return 2;
  }

  std::cout << shaders.size() << " shaders from " << dumps << std::endl;

  // The child of the parallel case: write its half of the set. It fails only on a failed write; a
  // key that the other process got to first is skipped, which is not a failure.
  if (selected == "parallel-child") {
    auto held=reopenCache();
    held->lookupShader("parallel-initialize",testCreateInfo());
    HANDLE ready=OpenEventA(EVENT_MODIFY_STATE,FALSE,(barrier+"-ready").c_str());
    HANDLE go=OpenEventA(SYNCHRONIZE,FALSE,(barrier+"-go").c_str());
    if (!ready || !go) { if(ready)CloseHandle(ready);if(go)CloseHandle(go);return 2; }
    SetEvent(ready);
    DWORD wait=WaitForSingleObject(go,120000);
    CloseHandle(ready);CloseHandle(go);
    if(wait!=WAIT_OBJECT_0) return 3;
    uint32_t failures=0;
    auto written=fillCache(shaders,0,&failures);
    return failures || !written ? 1 : 0;
  }

  bool all = selected == "all";
  bool passed = true;
  std::string self = argv[0];

  if (all || selected == "identity")
    passed = reportCase("identity", caseIdentity()) && passed;

  if (all || selected == "key")
    passed = reportCase("key", caseKey(shaders)) && passed;

  if (all || selected == "roundtrip") {
    removeCache();
    passed = reportCase("roundtrip (written table)",
      caseRoundtrip(shaders, false, nullptr, nullptr, nullptr, nullptr)) && passed;
    passed = reportCase("roundtrip (parsed table)",
      caseRoundtrip(shaders, true, nullptr, nullptr, nullptr, nullptr)) && passed;
  }

  if (all || selected == "damage")
    passed = reportCase("damage", caseDamage(shaders)) && passed;

  if (all || selected == "cap")
    passed = reportCase("cap", caseCap(shaders)) && passed;

  if (all || selected == "parallel")
    passed = reportCase("parallel", caseParallel(shaders, dumps, self, limit, cacheDir)) && passed;

  if (all || selected == "timing")
    passed = reportCase("timing", caseTiming(shaders)) && passed;

  if (all || selected == "unavailable") {
    passed = reportCase("unavailable", caseUnavailable(shaders, cacheDir)) && passed;
  }

  if (all || selected == "policy")
    passed = reportCase("policy", casePolicy(shaders)) && passed;

  if (!keepCache)
    removeCache();

  std::cout << (passed ? "shader cache gate: PASS" : "shader cache gate: FAIL") << std::endl;
  return passed ? 0 : 1;
}
