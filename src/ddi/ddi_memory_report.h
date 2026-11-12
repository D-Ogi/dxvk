#pragma once

// Engine side of IBc250DxvkDevice6 (ABI 1.8, BD-373): the kernel allocation of each tracked engine resource, for
// the UMD logging provider of WDDM 1.2 that the shell writes. The reporter is lazy: it keeps the current memory
// of each tracked DXVK object and sends events to the shell only while reporting is on, and at ReportMemory.
//
// Threads. Track, untrack and every storage change of a tracked object happen inside engine calls (E2): DXVK's
// assignStorage runs inline on the calling thread, and the runtime enters a device from one thread at a time.
// SetReporting and Report come from any thread (the shell's ETW enable callback).
//
// Cost when reporting is off, per storage change of a tracked object: the observer call, a ResolveMemory call only
// when the change leaves the memory object of the storage it replaces, a seqlock publish of five integers, a
// compiler barrier and one relaxed load (the processor fence is on the side of Report: FlushProcessWriteBuffers).
// No lock, no kernel call, no allocation. An untracked object pays the null test in assignStorage.
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

      // While reporting is on, a resource that does not rename keeps its storage. The logging reports a storage
      // change as UNMAP and MAP without Renamed usage, and the HLK test then loses its key for the D3D resource.
      bool keepsStorage() const override;

      // Device thread
      void Attach(DxvkPagedResource* object, DxvkResourceAllocation* storage, bool renamed);

      void Detach();

      // Any thread: the mapping as last published
      Mapping Read() const;

      Record*                 record  = nullptr;
      Rc<DxvkPagedResource>   object;

      // Device thread only: the mapping as last published; the memory object of the current storage with the
      // answer of ResolveMemory for it; the storage changes since Attach
      Mapping                 current = { };
      VkDeviceMemory          memory  = VK_NULL_HANDLE;
      UINT64                  memoryAllocation = 0u;
      UINT64                  memoryBytes = 0u;
      uint32_t                changes = 0u;

    private:

      Mapping Resolve(DxvkResourceAllocation* storage, bool renamed);

      void Publish(const Mapping& mapping);

      std::atomic<uint32_t>   m_seq        = { 0u };
      std::atomic<UINT64>     m_allocation = { 0u };
      std::atomic<UINT64>     m_offset     = { 0u };
      std::atomic<UINT64>     m_size       = { 0u };
      std::atomic<UINT64>     m_bytes      = { 0u };
      std::atomic<UINT32>     m_flags      = { 0u };

    };

    struct Record {
      MemoryReporter*         reporter = nullptr;
      UINT64                  cookie   = 0u;
      bool                    renames  = false;   // D3D11_USAGE_DYNAMIC: a storage change is a rename
      UINT32                  count    = 0u;
      std::unique_ptr<Slot[]> slots;
    };

    // Writes UNMAP old and MAP now while reporting is on; device thread
    void Changed(const Record& record, const Mapping& old, const Mapping& now);

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
