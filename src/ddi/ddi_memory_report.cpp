#include "ddi_memory_report.h"

namespace dxvk::ddi {

  namespace {

    constexpr UINT SharedOrTiled = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX
      | D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_TILED | D3D11_RESOURCE_MISC_TILE_POOL;

    // An asymmetric fence pair for the Dekker order of a storage change (publish, then test m_enabled) against
    // SetReporting and Report (store m_enabled, then read the published values). The change side, which is on the
    // draw path, is a compiler barrier. The rare side is FlushProcessWriteBuffers, which drains the store buffer
    // of every processor of the process: after it, a publish that came before a change's barrier is visible to
    // Report, or that change's test comes after it and sees reporting on.
    inline void LightFence() {
      std::atomic_signal_fence(std::memory_order_seq_cst);
    }

    inline void HeavyFence() {
      FlushProcessWriteBuffers();
    }

  }


  MemoryReporter::MemoryReporter(const BC250_DXVK_SHELL_SERVICES& Services)
  : m_shell(Services.Shell), m_resolve(Services.ResolveMemory), m_events(Services.MemoryEvents) {

  }


  MemoryReporter::~MemoryReporter() {
    Clear();
  }


  void MemoryReporter::Clear() {
    std::lock_guard lock(m_registryMutex);

    for (auto& entry : m_records)
      UntrackLocked(*entry.second, false);

    m_records.clear();
  }


  HRESULT MemoryReporter::Track(ID3D11Resource* pResource, UINT64 Cookie) {
    if (!Available())
      return S_FALSE;

    D3D11_RESOURCE_DIMENSION dimension = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    pResource->GetType(&dimension);

    // The DXVK objects whose memory the resource owns: the buffer, or the image and the mapped buffers
    Rc<DxvkPagedResource> primary;
    DxvkResourceAllocation* primaryStorage = nullptr;
    D3D11CommonTexture* texture = nullptr;
    bool renames = false;

    if (dimension == D3D11_RESOURCE_DIMENSION_BUFFER) {
      auto buffer = static_cast<D3D11Buffer*>(pResource);

      if (buffer->Desc()->MiscFlags & SharedOrTiled)
        return S_FALSE;

      Rc<DxvkBuffer> object = buffer->GetBuffer();

      if (object == nullptr)
        return S_FALSE;

      renames = buffer->Desc()->Usage == D3D11_USAGE_DYNAMIC;
      primaryStorage = object->storage().ptr();
      primary = object;
    } else {
      texture = GetCommonTexture(pResource);

      // Textures on runtime allocations (E5) are the shell's own; it logs them itself
      if (!texture || (texture->Desc()->MiscFlags & SharedOrTiled)
       || (texture->GetImage() == nullptr && texture->GetMapMode() != D3D11_COMMON_TEXTURE_MAP_MODE_STAGING))
        return S_FALSE;

      renames = texture->Desc()->Usage == D3D11_USAGE_DYNAMIC;

      if (texture->GetImage() != nullptr) {
        Rc<DxvkImage> image = texture->GetImage();
        primaryStorage = image->storage().ptr();
        primary = image;
      }
    }

    // Every slot is there before the first observer is set: a later Map finds its slot without an allocation
    auto record = std::make_unique<Record>();
    record->reporter = this;
    record->cookie   = Cookie;
    record->renames  = renames;
    record->count    = 1u + (texture && texture->GetMapMode() != D3D11_COMMON_TEXTURE_MAP_MODE_NONE
      ? texture->CountSubresources() : 0u);
    record->slots    = std::make_unique<Slot[]>(record->count);

    for (UINT32 i = 0u; i < record->count; i++)
      record->slots[i].record = record.get();

    std::lock_guard lock(m_registryMutex);

    if (m_records.find(pResource) != m_records.end()
     || (primary != nullptr && primary->storageObserver()))
      return S_FALSE;

    auto [entry, added] = m_records.emplace(pResource, std::move(record));
    Record& r = *entry->second;

    if (primary != nullptr)
      r.slots[0].Attach(primary.ptr(), primaryStorage, false);

    for (UINT32 i = 1u; i < r.count; i++) {
      Rc<DxvkBuffer> buffer = texture->GetMappedBuffer(i - 1u);

      if (buffer != nullptr)
        r.slots[i].Attach(buffer.ptr(), buffer->storage().ptr(), false);
    }

    LightFence();

    if (m_enabled.load(std::memory_order_relaxed)) {
      for (UINT32 i = 0u; i < r.count; i++) {
        if (r.slots[i].current.allocation)
          Emit(BC250_DXVK_MEMORY_EVENT_MAP, r, r.slots[i].current);
      }
    }

    return S_OK;
  }


  HRESULT MemoryReporter::Untrack(ID3D11Resource* pResource) {
    std::lock_guard lock(m_registryMutex);

    auto entry = m_records.find(pResource);

    if (entry == m_records.end())
      return S_FALSE;

    UntrackLocked(*entry->second, true);
    m_records.erase(entry);
    return S_OK;
  }


  void MemoryReporter::UntrackLocked(Record& record, bool events) {
    LightFence();
    bool emit = events && m_enabled.load(std::memory_order_relaxed);

    for (UINT32 i = 0u; i < record.count; i++) {
      Slot& slot = record.slots[i];

      if (emit && slot.current.allocation)
        Emit(BC250_DXVK_MEMORY_EVENT_UNMAP, record, slot.current);

      slot.Detach();
    }
  }


  void MemoryReporter::SetReporting(bool Enabled) {
    m_enabled.store(Enabled, std::memory_order_seq_cst);
  }


  void MemoryReporter::Report() {
    if (!Available())
      return;

    std::lock_guard registry(m_registryMutex);
    std::lock_guard report(m_reportMutex);

    // Dekker with the change path: a change either sees reporting on, or its value is read here
    HeavyFence();

    for (const auto& entry : m_records) {
      const Record& record = *entry.second;

      for (UINT32 i = 0u; i < record.count; i++) {
        Mapping mapping = record.slots[i].Read();

        if (mapping.allocation) {
          BC250_DXVK_MEMORY_EVENT event = { };
          event.Kind           = BC250_DXVK_MEMORY_EVENT_RUNDOWN;
          event.Flags          = mapping.flags;
          event.Resource       = record.cookie;
          event.Allocation     = mapping.allocation;
          event.Offset         = mapping.offset;
          event.Size           = mapping.size;
          event.AllocationSize = mapping.bytes;
          m_events(m_shell, &event, 1u);
        }
      }
    }
  }


  size_t MemoryReporter::TrackedCount() const {
    std::lock_guard lock(m_registryMutex);
    return m_records.size();
  }


  void MemoryReporter::Changed(const Record& record, const Mapping& old, const Mapping& now) {
    LightFence();

    if (likely(!m_enabled.load(std::memory_order_relaxed)) || old == now)
      return;

    std::lock_guard lock(m_reportMutex);

    if (old.allocation)
      Emit(BC250_DXVK_MEMORY_EVENT_UNMAP, record, old);

    if (now.allocation)
      Emit(BC250_DXVK_MEMORY_EVENT_MAP, record, now);
  }


  void MemoryReporter::Emit(UINT32 kind, const Record& record, const Mapping& mapping) {
    BC250_DXVK_MEMORY_EVENT event = { };
    event.Kind           = kind;
    event.Flags          = mapping.flags;
    event.Resource       = record.cookie;
    event.Allocation     = mapping.allocation;
    event.Offset         = mapping.offset;
    event.Size           = mapping.size;
    event.AllocationSize = mapping.bytes;
    m_events(m_shell, &event, 1u);
  }


  void MemoryReporter::Slot::storageChanged(
          DxvkPagedResource&        resource,
          DxvkResourceAllocation*   storage) {
    Mapping old = current;

    // A change after the first storage of a dynamic resource is a rename (Map with DISCARD)
    changes += 1u;
    Mapping now = Resolve(storage, record->renames);

    current = now;
    Publish(now);
    record->reporter->Changed(*record, old, now);
  }


  void MemoryReporter::Slot::helperChanged(
          uint32_t                  index,
          DxvkPagedResource*        helper) {
    if (1u + index >= record->count)
      return;

    Slot& slot = record->slots[1u + index];
    Mapping old = slot.current;

    if (helper) {
      // The buffer of one Map of a dynamic texture is storage of that Map: a rename like a discard
      slot.Detach();
      slot.Attach(helper, static_cast<DxvkBuffer*>(helper)->storage().ptr(), record->renames);
    } else {
      slot.Detach();
    }

    record->reporter->Changed(*record, old, slot.current);
  }


  bool MemoryReporter::Slot::keepsStorage() const {
    // A discard or a move gives a new allocation to a resource that the API did not rename. The test of the UMD
    // logging (OfferReclaim) removes every key of the resource at UNMAP and restores the key of the D3D resource
    // at MAP only for a rename. Off, nothing reads the logging: DXVK keeps all of its discards and moves.
    return !record->renames && record->reporter->m_enabled.load(std::memory_order_relaxed);
  }


  void MemoryReporter::Slot::Attach(DxvkPagedResource* object, DxvkResourceAllocation* storage, bool renamed) {
    this->object = object;
    this->memory = VK_NULL_HANDLE;
    this->changes = 0u;

    current = Resolve(storage, renamed);
    Publish(current);

    object->setStorageObserver(this);
  }


  void MemoryReporter::Slot::Detach() {
    if (object != nullptr) {
      object->setStorageObserver(nullptr);
      object = nullptr;
    }

    memory = VK_NULL_HANDLE;
    memoryAllocation = 0u;
    memoryBytes = 0u;
    current = Mapping();
    Publish(current);
  }


  MemoryReporter::Mapping MemoryReporter::Slot::Resolve(DxvkResourceAllocation* storage, bool renamed) {
    Mapping result = { };

    if (!storage)
      return result;

    DxvkResourceMemoryInfo info = storage->getMemoryInfo();

    if (!info.memory) {
      memory = VK_NULL_HANDLE;
      memoryAllocation = 0u;
      memoryBytes = 0u;
      return result;
    }

    // The storage this change replaces is still alive here, so its memory object cannot have been freed and
    // its handle cannot name other memory: the answer for the same handle is the same.
    if (info.memory != memory) {
      UINT64 bytes = 0u;
      UINT64 allocation = record->reporter->m_resolve(record->reporter->m_shell, info.memory, &bytes);

      memory = info.memory;
      memoryAllocation = allocation;
      memoryBytes      = allocation ? bytes : 0u;
    }

    result.allocation = memoryAllocation;
    result.bytes      = memoryBytes;

    if (!result.allocation)
      return Mapping();

    result.offset = info.offset;
    result.size   = info.size;
    result.flags  = renamed || (record->renames && changes) ? BC250_DXVK_MEMORY_RENAMED : 0u;
    return result;
  }


  void MemoryReporter::Slot::Publish(const Mapping& mapping) {
    // Seqlock: one writer (the device thread), readers in Report retry until they see an even, unchanged count
    uint32_t seq = m_seq.load(std::memory_order_relaxed);
    m_seq.store(seq + 1u, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);

    m_allocation.store(mapping.allocation, std::memory_order_relaxed);
    m_offset.store(mapping.offset, std::memory_order_relaxed);
    m_size.store(mapping.size, std::memory_order_relaxed);
    m_bytes.store(mapping.bytes, std::memory_order_relaxed);
    m_flags.store(mapping.flags, std::memory_order_relaxed);

    m_seq.store(seq + 2u, std::memory_order_release);
  }


  MemoryReporter::Mapping MemoryReporter::Slot::Read() const {
    Mapping result;
    uint32_t before, after;

    do {
      before = m_seq.load(std::memory_order_acquire);

      while (before & 1u) {
        dxvk::this_thread::yield();
        before = m_seq.load(std::memory_order_acquire);
      }

      result.allocation = m_allocation.load(std::memory_order_relaxed);
      result.offset     = m_offset.load(std::memory_order_relaxed);
      result.size       = m_size.load(std::memory_order_relaxed);
      result.bytes      = m_bytes.load(std::memory_order_relaxed);
      result.flags      = m_flags.load(std::memory_order_relaxed);

      std::atomic_thread_fence(std::memory_order_acquire);
      after = m_seq.load(std::memory_order_relaxed);
    } while (before != after);

    return result;
  }

}
