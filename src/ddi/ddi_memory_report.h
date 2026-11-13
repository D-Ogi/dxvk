#pragma once

// Engine side of IBc250DxvkDevice6 (ABI 1.8, BD-373): the kernel allocation of each tracked engine resource, for
// the UMD logging provider of WDDM 1.2 that the shell writes. The reporter is lazy: it keeps the current memory
// of each tracked DXVK object and sends events to the shell only while reporting is on, and at ReportMemory.
//
// Threads. Track, untrack and every storage change of a tracked object happen inside engine calls (E2): DXVK's
// assignStorage runs inline on the calling thread, and the runtime enters a device from one thread at a time.
// SetReporting and Report come from any thread (the shell's ETW enable callback).
//
// Renames (BD-379). Each storage of a DYNAMIC resource has Renamed usage, and a rename writes MAP of the new storage
// before UNMAP of the old one: the HLK test of the logging links a renamed instance to the previous instance that
// is still mapped. A buffer that keeps its storages (DxvkBufferCreateInfo::retainedStorages) has one instance for
// each kept storage that was current once: such a storage stays mapped when it is retired, and when the buffer uses
// it again there is no event. The instances end at untrack.
//
// Cost when reporting is off, per storage change of a tracked object: the observer call, a ResolveMemory call only
// when the change leaves the memory object of the storage it replaces (for a kept storage, only the first time),
// a seqlock publish of five integers per instance, a compiler barrier and one relaxed load (the processor fence is
// on the side of Report: FlushProcessWriteBuffers). No kernel call, no allocation; a lock only for a buffer that
// keeps storages (its own list). An untracked object pays the null test in assignStorage.
//
// Bez pracy nie ma kołaczy: no cake without work. Here the work is only the bookkeeping that a rundown needs.

#include <atomic>
#include <memory>
#include <unordered_map>

#include "../d3d11/d3d11_buffer.h"
#include "../d3d11/d3d11_texture.h"

#include "../dxvk/dxvk_buffer.h"
#include "../dxvk/dxvk_image.h"

#include "bc250_dxvk_engine.h"

namespace dxvk::ddi {

  class MemoryReporter {

  public:

    explicit MemoryReporter(const BC250_DXVK_SHELL_SERVICES& Services);

    ~MemoryReporter();

    // Ends tracking of every resource without events, at the device's final release: the shell destroyed its
    // resources, and the references of the reporter must go before the DXVK device.
    void Clear();

    MemoryReporter             (const MemoryReporter&) = delete;
    MemoryReporter& operator = (const MemoryReporter&) = delete;

    // The shell gave both 1.8 services, and the device runs inline (E2)
    bool Available() const {
      return m_resolve && m_events;
    }

    // IBc250DxvkDevice6::TrackResourceMemory; inside an engine call
    HRESULT Track(ID3D11Resource* pResource, UINT64 Cookie);

    HRESULT Untrack(ID3D11Resource* pResource);

    // IBc250DxvkDevice6::SetMemoryReporting and ReportMemory; any thread
    void SetReporting(bool Enabled);

    void Report();

    // Tracked resources, for the host test
    size_t TrackedCount() const;

  private:

    // One mapping of a slot: Allocation 0 means that the object has no memory the shell can name
    struct Mapping {
      UINT64 allocation = 0u;
      UINT64 offset     = 0u;
      UINT64 size       = 0u;
      UINT64 bytes      = 0u;
      UINT32 flags      = 0u;

      bool operator == (const Mapping& other) const {
        return allocation == other.allocation && offset == other.offset && size == other.size
            && bytes == other.bytes && flags == other.flags;
      }
    };

    struct Record;

    // A kept storage that is mapped and not current: its mapping, and its memory object with the answer of
    // ResolveMemory for it. The buffer holds the storage until the buffer is destroyed.
    struct Instance {
      const DxvkResourceAllocation* storage = nullptr;
      VkDeviceMemory          memory  = VK_NULL_HANDLE;
      Mapping                 mapping = { };
    };

    // The published form of one mapping
    struct SharedMapping {
      std::atomic<UINT64>     allocation = { 0u };
      std::atomic<UINT64>     offset     = { 0u };
      std::atomic<UINT64>     size       = { 0u };
      std::atomic<UINT64>     bytes      = { 0u };
      std::atomic<UINT32>     flags      = { 0u };
    };

    // The most mappings of one slot: the current storage and the other kept storages
    static constexpr uint32_t MaxMappings = 1u + DxvkBuffer::MaxRetainedStorages;

    // One DXVK object of a tracked resource and its observer. Slot 0 is the buffer or image of the resource,
    // slot 1 + i the buffer of subresource i of a texture with mapped buffers.
    class Slot final : public DxvkStorageObserver {

    public:

      void storageChanged(
              DxvkPagedResource&        resource,
              DxvkResourceAllocation*   storage) override;

      void helperChanged(
              uint32_t                  index,
              DxvkPagedResource*        helper) override;

      // While reporting is on, a resource that does not rename keeps its storage in the cases that
      // DxvkStorageObserver::keepsStorage names (not at a usage change of an image). The logging reports a storage
      // change as MAP and UNMAP without Renamed usage, and the HLK test then loses its key for the D3D resource.
      bool keepsStorage() const override;

      // Before the first Attach, at Track: keeper is the buffer of slot 0 when it keeps storages, else null. The
      // slot allocates here what it publishes, so that a later Attach does not allocate.
      void Reserve(DxvkBuffer* keeper);

      // Device thread
      void Attach(DxvkPagedResource* object, DxvkResourceAllocation* storage, bool renamed);

      void Detach();

      // Any thread: the mappings as last published, the current one first; returns their count
      uint32_t Read(Mapping* mappings) const;

      Record*                 record  = nullptr;
      Rc<DxvkPagedResource>   object;

      // Device thread only: the mapping of the current storage as last published, and the storage; the memory
      // object of the current storage with the answer of ResolveMemory for it; the kept storages that are mapped
      Mapping                 current = { };
      const DxvkResourceAllocation* currentStorage = nullptr;
      VkDeviceMemory          memory  = VK_NULL_HANDLE;
      UINT64                  memoryAllocation = 0u;
      UINT64                  memoryBytes = 0u;
      DxvkBuffer*             keeper  = nullptr;
      std::unique_ptr<Instance[]> kept;
      uint32_t                keptCount = 0u;
      uint32_t                keptCapacity = 0u;

    private:

      Mapping Resolve(DxvkResourceAllocation* storage, bool renamed);

      void Publish();

      std::atomic<uint32_t>   m_seq        = { 0u };
      std::atomic<uint32_t>   m_count      = { 0u };
      std::unique_ptr<SharedMapping[]> m_shared;

    };

    struct Record {
      MemoryReporter*         reporter = nullptr;
      UINT64                  cookie   = 0u;
      bool                    renames  = false;   // D3D11_USAGE_DYNAMIC: a storage change is a rename
      UINT32                  count    = 0u;
      std::unique_ptr<Slot[]> slots;
    };

    // Writes MAP of mapped, then UNMAP of unmapped, while reporting is on; Allocation 0 means no event. Device
    // thread.
    void Changed(const Record& record, const Mapping& unmapped, const Mapping& mapped);

    void Emit(UINT32 kind, const Record& record, const Mapping& mapping);

    void UntrackLocked(Record& record, bool events);

    void*                   m_shell   = nullptr;
    UINT64 (APIENTRY *m_resolve)(void *shell, VkDeviceMemory memory, UINT64 *bytes) = nullptr;
    void   (APIENTRY *m_events)(void *shell, const BC250_DXVK_MEMORY_EVENT *events, UINT32 count) = nullptr;

    std::atomic<bool>       m_enabled = { false };

    // Order: registry, then report. The registry lock is taken at track and untrack (resource creation and
    // destruction) and by Report; the report lock while events are written.
    mutable dxvk::mutex     m_registryMutex;
    dxvk::mutex             m_reportMutex;

    std::unordered_map<ID3D11Resource*, std::unique_ptr<Record>> m_records;

  };

}
