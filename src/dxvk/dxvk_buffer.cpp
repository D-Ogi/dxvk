#include "dxvk_barrier.h"
#include "dxvk_buffer.h"
#include "dxvk_device.h"

#include <algorithm>

namespace dxvk {
  
  DxvkBuffer::DxvkBuffer(
          DxvkDevice*           device,
    const DxvkBufferCreateInfo& createInfo,
          DxvkMemoryAllocator&  allocator,
          VkMemoryPropertyFlags memFlags)
  : DxvkPagedResource(allocator),
    m_vkd           (device->vkd()),
    m_properties    (memFlags),
    m_shaderStages  (util::shaderStages(createInfo.stages)),
    m_sharingMode   (device->getSharingMode()),
    m_info          (createInfo) {
    m_allocator->registerResource(this);

    // Assign debug name to buffer
    if (device->debugFlags().test(DxvkDebugFlag::Capture)) {
      m_debugName = createDebugName(createInfo.debugName);
      m_info.debugName = m_debugName.c_str();
    } else {
      m_info.debugName = nullptr;
    }

    // Unconditionally enable BDA usage
    m_info.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;

    // bc250 (BD-379): kept storages exist only for storages of their own
    m_info.retainedStorages = m_info.dedicatedStorage
      ? std::min(m_info.retainedStorages, MaxRetainedStorages) : 0u;
    m_retained.reserve(m_info.retainedStorages);

    // Create and assign actual buffer resource
    assignStorage(allocateStorage());
  }


  DxvkBuffer::DxvkBuffer(
          DxvkDevice*           device,
    const DxvkBufferCreateInfo& createInfo,
    const DxvkBufferImportInfo& importInfo,
          DxvkMemoryAllocator&  allocator,
          VkMemoryPropertyFlags memFlags)
  : DxvkPagedResource(allocator),
    m_vkd           (device->vkd()),
    m_properties    (memFlags),
    m_shaderStages  (util::shaderStages(createInfo.stages)),
    m_sharingMode   (device->getSharingMode()),
    m_info          (createInfo),
    m_stableAddress (true) {
    m_allocator->registerResource(this);

    // bc250 (BD-379): imported memory is not the buffer's to keep
    m_info.dedicatedStorage = false;
    m_info.retainedStorages = 0u;

    DxvkAllocationInfo allocationInfo = { };
    allocationInfo.resourceCookie = cookie();

    VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    info.flags = m_info.flags;
    info.usage = m_info.usage;
    info.size = m_info.size;
    m_sharingMode.fill(info);

    assignStorage(allocator.importBufferResource(info, allocationInfo, importInfo));
  }


  DxvkBuffer::~DxvkBuffer() {
    m_allocator->unregisterResource(this);
  }


  bool DxvkBuffer::canRelocate() const {
    return !m_bufferInfo.mapPtr && !m_stableAddress
        && !(m_info.flags & VK_BUFFER_CREATE_SPARSE_BINDING_BIT);
  }


  Rc<DxvkBufferView> DxvkBuffer::createView(
    const DxvkBufferViewKey& info) {
    std::unique_lock lock(m_viewMutex);

    auto entry = m_views.emplace(std::piecewise_construct,
      std::make_tuple(info), std::make_tuple(this, info));

    return &entry.first->second;
  }


  Rc<DxvkResourceAllocation> DxvkBuffer::allocateRetainedStorage(
    const VkBufferCreateInfo&         info,
    const DxvkAllocationInfo&         allocationInfo,
          DxvkLocalAllocationCache*   cache) {
    std::lock_guard lock(m_retainedMutex);

    // A kept storage with no other reference is free: the buffer holds a reference to its current storage, and a
    // command list holds one to each storage that it tracks until the GPU completes it. Only this list can give a
    // new reference to a kept storage, and its lock is held.
    for (const auto& storage : m_retained) {
      if (storage->hasSingleReference())
        return storage;
    }

    if (m_retained.size() < m_info.retainedStorages) {
      DxvkAllocationInfo dedicatedInfo = allocationInfo;
      dedicatedInfo.mode.set(DxvkAllocationMode::Dedicated);

      if (auto storage = m_allocator->createBufferResource(info, dedicatedInfo, nullptr)) {
        m_allocator->markOfferDedicated(storage.ptr());
        m_retained.push_back(storage);
        return storage;
      }
    }

    // Every kept storage is in use: suballocate, as without the rule. No kernel allocation for this discard.
    return m_allocator->createBufferResource(info, allocationInfo, cache);
  }


  bool DxvkBuffer::retainsStorage(const DxvkResourceAllocation* storage) {
    if (!m_info.retainedStorages || !storage)
      return false;

    std::lock_guard lock(m_retainedMutex);

    for (const auto& entry : m_retained) {
      if (entry.ptr() == storage)
        return true;
    }

    return false;
  }


  DxvkSparsePageTable* DxvkBuffer::getSparsePageTable() {
    return m_storage->getSparsePageTable();
  }


  Rc<DxvkResourceAllocation> DxvkBuffer::relocateStorage(
          DxvkAllocationModes         mode) {
    // The resource may become non-relocatable even after we allocate new
    // backing storage, but if it already is then don't waste memory.
    if (!canRelocate() || storageIsKept())
      return nullptr;

    DxvkAllocationInfo allocationInfo = { };
    allocationInfo.resourceCookie = cookie();
    allocationInfo.properties = m_properties;
    allocationInfo.mode = mode;

    VkBufferCreateInfo info = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    info.flags = m_info.flags;
    info.usage = m_info.usage;
    info.size = m_info.size;
    m_sharingMode.fill(info);

    return m_allocator->createBufferResource(info, allocationInfo, nullptr);
  }


  void DxvkBuffer::setDebugName(const char* name) {
    if (likely(!m_info.debugName))
      return;

    m_debugName = createDebugName(name);
    m_info.debugName = m_debugName.c_str();

    updateDebugName();
  }


  void DxvkBuffer::updateDebugName() {
    if (m_storage->flags().test(DxvkAllocationFlag::OwnsBuffer)) {
      VkDebugUtilsObjectNameInfoEXT nameInfo = { VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT };
      nameInfo.objectType = VK_OBJECT_TYPE_BUFFER;
      nameInfo.objectHandle = vk::getObjectHandle(m_bufferInfo.buffer);
      nameInfo.pObjectName = m_info.debugName;

      m_vkd->vkSetDebugUtilsObjectNameEXT(m_vkd->device(), &nameInfo);
    }
  }


  std::string DxvkBuffer::createDebugName(const char* name) const {
    return str::format(vk::isValidDebugName(name) ? name : "Buffer", " (", cookie(), ")");
  }




  void DxvkBufferView::updateViews() {
    if (likely(m_key.format))
      m_formatted = m_buffer->m_storage->createBufferView(m_key);

    if (likely(m_buffer->info().usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) {
      DxvkBufferViewKey rawKey = m_key;
      rawKey.format = VK_FORMAT_UNDEFINED;
      rawKey.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

      m_raw = m_buffer->m_storage->createBufferView(rawKey);
    }

    m_version = m_buffer->m_version;
  }

}
