#include "dxvk_device.h"
#include "dxvk_queue.h"

namespace dxvk {
  
  DxvkSubmissionQueue::DxvkSubmissionQueue(DxvkDevice* device, const DxvkQueueCallback& callback)
  : m_device        (device),
    m_checkpoints   (device->getCheckpointBuffer()),
    m_callback      (callback),
    m_inline        (device->isInlineExecution()) {
    auto vk = m_device->vkd();

    VkSemaphoreTypeCreateInfo semaphoreType = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
    semaphoreType.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;

    VkSemaphoreCreateInfo semaphoreInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &semaphoreType };

    VkResult vrGraphics = vk->vkCreateSemaphore(vk->device(), &semaphoreInfo, nullptr, &m_semaphores.graphics);
    VkResult vrTransfer = vk->vkCreateSemaphore(vk->device(), &semaphoreInfo, nullptr, &m_semaphores.transfer);

    if (vrGraphics || vrTransfer) {
      throw DxvkError(str::format("Failed to create timeline semaphores: ",
        vrGraphics > vrTransfer ? vrGraphics : vrTransfer));
    }

    // In inline mode, the calling thread submits and retires
    // command lists itself, see submitInline and finishInline.
    if (!m_inline) {
      m_submitThread = dxvk::thread([this] () { submitCmdLists(); });
      m_finishThread = dxvk::thread([this] () { finishCmdLists(); });
    }
  }


  DxvkSubmissionQueue::~DxvkSubmissionQueue() {
    auto vk = m_device->vkd();

    { std::unique_lock<dxvk::mutex> lock(m_mutex);
      m_stopped.store(true);
    }

    m_appendCond.notify_all();
    m_submitCond.notify_all();

    if (m_submitThread.joinable())
      m_submitThread.join();

    if (m_finishThread.joinable())
      m_finishThread.join();

    if (m_inline) {
      while (finishInline(true))
        continue;
    }

    vk->vkDestroySemaphore(vk->device(), m_semaphores.graphics, nullptr);
    vk->vkDestroySemaphore(vk->device(), m_semaphores.transfer, nullptr);
  }
  
  
  void DxvkSubmissionQueue::submit(
          DxvkSubmitInfo            submitInfo,
          DxvkLatencyInfo           latencyInfo,
          DxvkSubmitStatus*         status) {
    if (m_inline) {
      DxvkSubmitEntry entry = { };
      entry.status = status;
      entry.submit = std::move(submitInfo);
      entry.latency = std::move(latencyInfo);

      submitInline(std::move(entry));
      return;
    }

    std::unique_lock<dxvk::mutex> lock(m_mutex);

    m_finishCond.wait(lock, [this] {
      return m_submitQueue.size() + m_finishQueue.size() <= MaxNumQueuedCommandBuffers;
    });

    DxvkSubmitEntry entry = { };
    entry.status = status;
    entry.submit = std::move(submitInfo);
    entry.latency = std::move(latencyInfo);

    m_submitQueue.push(std::move(entry));
    m_appendCond.notify_all();
  }


  void DxvkSubmissionQueue::present(
          DxvkPresentInfo           presentInfo,
          DxvkLatencyInfo           latencyInfo,
          DxvkSubmitStatus*         status) {
    DxvkSubmitEntry entry = { };
    entry.status  = status;
    entry.present = std::move(presentInfo);
    entry.latency = std::move(latencyInfo);

    if (m_inline) {
      submitInline(std::move(entry));
      return;
    }

    std::unique_lock<dxvk::mutex> lock(m_mutex);

    m_submitQueue.push(std::move(entry));
    m_appendCond.notify_all();
  }


  void DxvkSubmissionQueue::synchronizeSubmission(
          DxvkSubmitStatus*   status) {
    // Inline submissions complete before submit returns
    if (m_inline)
      return;

    std::unique_lock<dxvk::mutex> lock(m_mutex);

    m_submitCond.wait(lock, [status] {
      return status->result.load() != VK_NOT_READY;
    });
  }


  void DxvkSubmissionQueue::synchronize() {
    if (m_inline)
      return;

    std::unique_lock<dxvk::mutex> lock(m_mutex);

    m_submitCond.wait(lock, [this] {
      return m_submitQueue.empty();
    });
  }


  void DxvkSubmissionQueue::waitForIdle() {
    if (m_inline) {
      while (finishInline(true))
        continue;
      return;
    }

    std::unique_lock<dxvk::mutex> lock(m_mutex);

    m_submitCond.wait(lock, [this] {
      return m_submitQueue.empty();
    });

    m_finishCond.wait(lock, [this] {
      return m_finishQueue.empty();
    });
  }


  void DxvkSubmissionQueue::lockDeviceQueue() {
    m_mutexQueue.lock();

    if (m_callback)
      m_callback(true);
  }


  void DxvkSubmissionQueue::unlockDeviceQueue() {
    if (m_callback)
      m_callback(false);

    m_mutexQueue.unlock();
  }


  void DxvkSubmissionQueue::executeEntry(
          DxvkSubmitEntry&    entry) {
    // Submit command buffer to device
    if (m_lastError != VK_ERROR_DEVICE_LOST) {
      std::lock_guard<dxvk::mutex> lock(m_mutexQueue);

      if (m_callback)
        m_callback(true);

      if (entry.submit.cmdList != nullptr) {
        if (entry.latency.tracker) {
          entry.latency.tracker->notifyQueueSubmit(entry.latency.frameId);

          if (!m_trackedSubmitId && entry.latency.frameId > m_trackedPresentId)
            m_trackedSubmitId = entry.latency.frameId;
        }

        entry.result = entry.submit.cmdList->submit(
          m_semaphores, m_timelines, m_trackedSubmitId);
        entry.timelines = m_timelines;
      } else if (entry.present.presenter != nullptr) {
        if (entry.latency.tracker)
          entry.latency.tracker->notifyQueuePresentBegin(entry.latency.frameId);

        entry.result = entry.present.presenter->presentImage(
          entry.present.frameId, entry.latency.tracker,
          entry.present.rects.size(), entry.present.rects.data());

        if (entry.latency.tracker) {
          entry.latency.tracker->notifyQueuePresentEnd(
            entry.latency.frameId, entry.result);

          m_trackedPresentId = entry.latency.frameId;
          m_trackedSubmitId = 0u;
        }
      }

      if (m_callback)
        m_callback(false);
    } else {
      // Don't submit anything after device loss
      // so that drivers get a chance to recover
      entry.result = VK_ERROR_DEVICE_LOST;
    }

    if (entry.status)
      entry.status->result = entry.result;

    if (entry.result == VK_ERROR_DEVICE_LOST && m_checkpoints)
      m_checkpoints->printHangInfo();
  }


  bool DxvkSubmissionQueue::forwardEntry(
          DxvkSubmitEntry&    entry) {
    bool doForward = (entry.result == VK_SUCCESS) ||
      (entry.present.presenter != nullptr && entry.result != VK_ERROR_DEVICE_LOST);

    if (!doForward) {
      Logger::err(str::format("DxvkSubmissionQueue: Command submission failed: ", entry.result));
      m_lastError = entry.result;
    }

    return doForward;
  }


  void DxvkSubmissionQueue::submitInline(
          DxvkSubmitEntry&&   entry) {
    // Keep the same bound on command lists in flight as the threaded
    // path, retiring the oldest ones on this thread if necessary.
    while (true) {
      size_t pending;

      { std::lock_guard<dxvk::mutex> lock(m_mutex);
        pending = m_finishQueue.size();
      }

      if (pending <= MaxNumQueuedCommandBuffers || !finishInline(true))
        break;
    }

    executeEntry(entry);

    if (forwardEntry(entry)) {
      std::lock_guard<dxvk::mutex> lock(m_mutex);
      m_finishQueue.push(std::move(entry));
    } else if (m_lastError != VK_ERROR_DEVICE_LOST) {
      m_device->waitForIdle();
    }

    m_device->m_objects.memoryManager().performTimedTasks();

    // Retire whatever the GPU has already completed
    while (finishInline(false))
      continue;
  }


  bool DxvkSubmissionQueue::finishInline(bool wait) {
    DxvkSubmitEntry entry;

    { std::lock_guard<dxvk::mutex> lock(m_mutex);

      if (m_finishQueue.empty())
        return false;

      auto& front = m_finishQueue.front();

      if (!wait && front.submit.cmdList != nullptr && m_lastError.load() != VK_ERROR_DEVICE_LOST) {
        auto vk = m_device->vkd();

        uint64_t graphics = 0u;
        uint64_t transfer = 0u;

        // Leave errors to a waiting call, which reports them
        if (vk->vkGetSemaphoreCounterValue(vk->device(), m_semaphores.graphics, &graphics)
         || vk->vkGetSemaphoreCounterValue(vk->device(), m_semaphores.transfer, &transfer))
          return false;

        if (graphics < front.timelines.graphics || transfer < front.timelines.transfer)
          return false;
      }

      entry = std::move(front);
    }

    finishEntry(entry);
    return true;
  }


  void DxvkSubmissionQueue::submitCmdLists() {
    env::setThreadName("dxvk-submit");

    while (!m_stopped.load()) {
      DxvkSubmitEntry entry;

      { std::unique_lock<dxvk::mutex> lock(m_mutex);

        m_appendCond.wait(lock, [this] {
          return m_stopped.load() || !m_submitQueue.empty();
        });

        if (m_stopped.load())
          return;

        entry = std::move(m_submitQueue.front());
      }

      executeEntry(entry);

      // On success, pass it on to the queue thread
      { std::unique_lock<dxvk::mutex> lock(m_mutex);

        bool doForward = (entry.result == VK_SUCCESS) ||
          (entry.present.presenter != nullptr && entry.result != VK_ERROR_DEVICE_LOST);

        if (doForward) {
          m_finishQueue.push(std::move(entry));
        } else {
          Logger::err(str::format("DxvkSubmissionQueue: Command submission failed: ", entry.result));
          m_lastError = entry.result;

          if (m_lastError != VK_ERROR_DEVICE_LOST)
            m_device->waitForIdle();
        }

        m_submitQueue.pop();
        m_submitCond.notify_all();
      }

      // Good time to invoke allocator tasks now since we
      // expect this to get called somewhat periodically.
      m_device->m_objects.memoryManager().performTimedTasks();
    }
  }
  
  
  void DxvkSubmissionQueue::finishCmdLists() {
    env::setThreadName("dxvk-queue");

    while (!m_stopped.load()) {
      std::unique_lock<dxvk::mutex> lock(m_mutex);

      if (m_finishQueue.empty()) {
        auto t0 = dxvk::high_resolution_clock::now();

        m_submitCond.wait(lock, [this] {
          return m_stopped.load() || !m_finishQueue.empty();
        });

        auto t1 = dxvk::high_resolution_clock::now();
        m_gpuIdle += std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
      }

      if (m_stopped.load())
        return;
      
      DxvkSubmitEntry entry = std::move(m_finishQueue.front());
      lock.unlock();

      finishEntry(entry);
    }
  }


  void DxvkSubmissionQueue::finishEntry(
          DxvkSubmitEntry&    entry) {
    auto vk = m_device->vkd();

    if (entry.submit.cmdList != nullptr) {
      VkResult status = m_lastError.load();

      if (status != VK_ERROR_DEVICE_LOST) {
        std::array<VkSemaphore, 2> semaphores = { m_semaphores.graphics, m_semaphores.transfer };
        std::array<uint64_t, 2> timelines = { entry.timelines.graphics, entry.timelines.transfer };

        if (entry.latency.tracker)
          entry.latency.tracker->notifyGpuExecutionBegin(entry.latency.frameId);

        VkSemaphoreWaitInfo waitInfo = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO };
        waitInfo.semaphoreCount = semaphores.size();
        waitInfo.pSemaphores = semaphores.data();
        waitInfo.pValues = timelines.data();

        status = vk->vkWaitSemaphores(vk->device(), &waitInfo, ~0ull);

        if (entry.latency.tracker && status == VK_SUCCESS)
          entry.latency.tracker->notifyGpuExecutionEnd(entry.latency.frameId);
      }

      if (status == VK_ERROR_DEVICE_LOST && m_checkpoints)
        m_checkpoints->printHangInfo();

      if (status != VK_SUCCESS) {
        m_lastError = status;

        // Inline mode retires on this thread; waiting for idle
        // here would recurse into the entry being retired.
        if (status != VK_ERROR_DEVICE_LOST && !m_inline)
          m_device->waitForIdle();
      }
    } else if (entry.present.presenter != nullptr) {
      // Signal the frame and then immediately destroy the reference.
      // This is necessary since the front-end may want to explicitly
      // destroy the presenter object.
      entry.present.presenter->signalFrame(entry.present.frameId, entry.latency.tracker);
      entry.present.presenter = nullptr;
    }

    // Release resources and signal events, then immediately wake
    // up any thread that's currently waiting on a resource in
    // order to reduce delays as much as possible.
    if (entry.submit.cmdList != nullptr)
      entry.submit.cmdList->notifyObjects();

    { std::unique_lock<dxvk::mutex> lock(m_mutex);
      m_finishQueue.pop();
      m_finishCond.notify_all();
    }

    // Free the command list and associated objects now
    if (entry.submit.cmdList != nullptr) {
      entry.submit.cmdList->reset();
      m_device->recycleCommandList(entry.submit.cmdList);
    }
  }

}
