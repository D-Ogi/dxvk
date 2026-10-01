
#pragma once

#include <chrono>
#include <mutex>
#include <queue>
#include <unordered_map>

#include "dxvk_compute.h"
#include "dxvk_graphics.h"

namespace dxvk {

  class DxvkDevice;

  /**
   * \brief Pipeline count
   * 
   * Stores number of graphics and
   * compute pipelines, individually.
   */
  struct DxvkPipelineCount {
    uint32_t numGraphicsPipelines;
    uint32_t numGraphicsLibraries;
    uint32_t numComputePipelines;
  };

  /**
   * \brief Pipeline stats
   */
  struct DxvkPipelineStats {
    std::atomic<uint32_t> numGraphicsPipelines  = { 0u };
    std::atomic<uint32_t> numGraphicsLibraries  = { 0u };
    std::atomic<uint32_t> numComputePipelines   = { 0u };
    std::atomic<uint32_t> numOnDemandLibraries  = { 0u };
  };

  struct DxvkPipelineWorkerStats {
    uint64_t tasksCompleted;
    uint64_t tasksTotal;
  };

  /**
   * \brief Deferred work statistics in inline mode
   */
  struct DxvkDeferredCompileStats {
    uint64_t translationsQueued;    ///< Translations handed to worker threads
    uint64_t translationsPending;   ///< Of those, not started yet
    uint64_t librariesQueued;       ///< Libraries queued for \c compileDeferred
    uint64_t librariesCompiled;     ///< Of those, compiled by \c compileDeferred
    uint64_t librariesReady;        ///< Of those, still queued
    uint64_t librariesOnDemand;     ///< Libraries compiled when a draw or dispatch needed them
    uint64_t compileErrors;         ///< Translations or compiles that threw
  };

  /**
   * \brief Scope of a deferred compile slice
   */
  enum class DxvkDeferredScope : uint32_t {
    Libraries = 0,  ///< Shader pipeline libraries only
    All       = 1,  ///< Libraries, then optimized pipelines
  };

  /**
   * \brief Pipeline priority
   */
  enum class DxvkPipelinePriority : uint32_t {
    High    = 0,
    Normal  = 1,
    Low     = 2,
  };

  /**
   * \brief Pipeline manager worker threads
   *
   * Spawns worker threads to compile shader pipeline
   * libraries and optimized pipelines asynchronously.
   */
  class DxvkPipelineWorkers {

  public:

    DxvkPipelineWorkers(
            DxvkDevice*                     device);

    ~DxvkPipelineWorkers();

    /**
     * \brief Queries worker statistics
     *
     * The returned result may be immediately out of date.
     * \returns Worker statistics
     */
    DxvkPipelineWorkerStats getStats() const {
      DxvkPipelineWorkerStats result;
      result.tasksCompleted = m_tasksCompleted.load();
      result.tasksTotal = m_tasksTotal.load();
      return result;
    }

    /**
     * \brief Queries deferred work statistics
     * \returns Statistics
     */
    DxvkDeferredCompileStats getDeferredStats();

    /**
     * \brief Checks whether shaders are translated on workers
     *
     * With inline execution, unless the configuration says no.
     * \returns \c true if \ref compileShader may be used
     */
    bool translatesOnWorkers() const;

    /**
     * \brief Compiles a pipeline library
     *
     * Asynchronously compiles a basic variant of
     * the pipeline with default compile arguments.
     * Note that pipeline libraries are high priority.
     *
     * With inline execution and translation on workers, the
     * library is queued for \ref compileDeferred instead, once
     * its shaders are translated; a draw that needs it earlier
     * compiles it on demand.
     * \param [in] library The pipeline library
     * \param [in] priority Pipeline priority
     */
    void compilePipelineLibrary(
            DxvkShaderPipelineLibrary*      library,
            DxvkPipelinePriority            priority);

    /**
     * \brief Translates a shader on a worker thread
     *
     * Inline execution only. The worker runs \c DxvkShader::compile,
     * which makes no Vulkan call, and then queues the library for
     * \ref compileDeferred. Whoever needs the shader first translates
     * it, or waits for the translation that is running.
     * \param [in] shader The shader
     * \param [in] library The shader's own pipeline library
     * \param [in] priority Priority
     */
    void compileShader(
      const Rc<DxvkShader>&                 shader,
            DxvkShaderPipelineLibrary*      library,
            DxvkPipelinePriority            priority);

    /**
     * \brief Compiles an optimized graphics pipeline
     *
     * \param [in] pipeline Compute pipeline
     * \param [in] state Pipeline state
     */
    void compileGraphicsPipeline(
            DxvkGraphicsPipeline*           pipeline,
      const DxvkGraphicsPipelineStateInfo&  state,
            DxvkPipelinePriority            priority);

    /**
     * \brief Compiles deferred pipelines
     *
     * With inline execution, optimized pipelines are not
     * compiled on the thread that draws, and with translation
     * on workers, shader pipeline libraries are not compiled
     * on the thread that creates the shader. They are queued
     * until this runs them on the calling thread: libraries
     * whose shaders are translated first, then, if the scope
     * says so, optimized pipelines. Starts compiles until the
     * budget is used up, so a single compile may exceed it.
     * \param [in] budget Time budget
     * \param [in] scope What to compile
     * \returns Number of pipelines still queued in scope
     */
    size_t compileDeferred(
            std::chrono::microseconds       budget,
            DxvkDeferredScope               scope);

    /**
     * \brief Stops all worker threads
     *
     * Stops threads and waits for their current work
     * to complete. Queued work will be discarded.
     */
    void stopWorkers();

  private:

    struct PipelineEntry {
      PipelineEntry()
      : pipelineLibrary(nullptr), graphicsPipeline(nullptr) { }

      PipelineEntry(DxvkShaderPipelineLibrary* l)
      : pipelineLibrary(l), graphicsPipeline(nullptr) { }

      PipelineEntry(DxvkGraphicsPipeline* p, const DxvkGraphicsPipelineStateInfo& s)
      : pipelineLibrary(nullptr), graphicsPipeline(p), graphicsState(s) { }

      PipelineEntry(Rc<DxvkShader> s, DxvkShaderPipelineLibrary* l, DxvkPipelinePriority p)
      : pipelineLibrary(l), graphicsPipeline(nullptr), shader(std::move(s)), priority(p) { }

      DxvkShaderPipelineLibrary*    pipelineLibrary;
      DxvkGraphicsPipeline*         graphicsPipeline;
      DxvkGraphicsPipelineStateInfo graphicsState;

      // Translation only: the worker compiles the shader and queues pipelineLibrary
      Rc<DxvkShader>                shader;
      DxvkPipelinePriority          priority = DxvkPipelinePriority::Normal;
    };

    struct PipelineBucket {
      dxvk::condition_variable  cond;
      std::queue<PipelineEntry> queue;
      uint32_t                  idleWorkers = 0;
    };

    DxvkDevice*                       m_device;

    std::atomic<uint64_t>             m_tasksTotal     = { 0ull };
    std::atomic<uint64_t>             m_tasksCompleted = { 0ull };

    dxvk::mutex                       m_lock;
    std::array<PipelineBucket, 3>     m_buckets;
    std::queue<PipelineEntry>         m_deferred;

    // Libraries whose shaders are translated, for compileDeferred: high, then normal priority
    std::array<std::queue<DxvkShaderPipelineLibrary*>, 2> m_readyLibraries;

    uint64_t                          m_translationsQueued = 0ull;
    uint64_t                          m_librariesQueued    = 0ull;
    uint64_t                          m_librariesCompiled  = 0ull;
    std::atomic<uint64_t>             m_compileErrors      = { 0ull };

    bool                              m_workersRunning = false;
    std::vector<dxvk::thread>         m_workers;

    void queueReadyLibraryLocked(
            DxvkShaderPipelineLibrary*      library,
            DxvkPipelinePriority            priority);

    void notifyWorkers(DxvkPipelinePriority priority);

    void startWorkers();

    void runWorker(DxvkPipelinePriority maxPriority);

  };

  
  /**
   * \brief Pipeline manager
   * 
   * Creates and stores graphics pipelines and compute
   * pipelines for each combination of shaders that is
   * used within the application. This is necessary
   * because DXVK does not expose the concept of shader
   * pipeline objects to the client API.
   */
  class DxvkPipelineManager {
    friend class DxvkComputePipeline;
    friend class DxvkGraphicsPipeline;
    friend class DxvkShaderPipelineLibrary;
  public:
    
    DxvkPipelineManager(
            DxvkDevice*         device);
    
    ~DxvkPipelineManager();
    
    /**
     * \brief Retrieves a compute pipeline object
     * 
     * If a pipeline for the given shader stage object
     * already exists, it will be returned. Otherwise,
     * a new pipeline will be created.
     * \param [in] shaders Shaders for the pipeline
     * \returns Compute pipeline object
     */
    DxvkComputePipeline* createComputePipeline(
      const DxvkComputePipelineShaders& shaders);
    
    /**
     * \brief Retrieves a graphics pipeline object
     * 
     * If a pipeline for the given shader stage objects
     * already exists, it will be returned. Otherwise,
     * a new pipeline will be created.
     * \param [in] shaders Shaders for the pipeline
     * \returns Graphics pipeline object
     */
    DxvkGraphicsPipeline* createGraphicsPipeline(
      const DxvkGraphicsPipelineShaders& shaders);

    /**
     * \brief Creates a pipeline library with a given set of shaders
     *
     * If a pipeline library already exists, it will be returned.
     * Otherwise, a new pipeline library will be created.
     * \param [in] key Shader set
     */
    DxvkShaderPipelineLibrary* createShaderPipelineLibrary(
      const DxvkShaderPipelineLibraryKey& key);

    /**
     * \brief Retrieves a vertex input pipeline library
     *
     * \param [in] state Vertex input state
     * \returns Pipeline library object
     */
    DxvkGraphicsPipelineVertexInputLibrary* createVertexInputLibrary(
      const DxvkGraphicsPipelineVertexInputState& state);

    /**
     * \brief Retrieves a fragment output pipeline library
     *
     * \param [in] state Fragment output state
     * \returns Pipeline library object
     */
    DxvkGraphicsPipelineFragmentOutputLibrary* createFragmentOutputLibrary(
      const DxvkGraphicsPipelineFragmentOutputState& state);

    /**
     * \brief Creates a descriptor set layout
     *
     * \param [in] key Descriptor set layout key
     * \returns Descriptor set layout object
     */
    const DxvkDescriptorSetLayout* createDescriptorSetLayout(
      const DxvkDescriptorSetLayoutKey& key);

    /**
     * \brief Creates a pipeline layout
     *
     * \param [in] key Pipeline layout key
     * \returns Descriptor set layout object
     */
    const DxvkPipelineLayout* createPipelineLayout(
      const DxvkPipelineLayoutKey& key);

    /**
     * \brief Registers a shader
     * 
     * Starts compiling pipelines asynchronously
     * in case the state cache contains state
     * vectors for this shader.
     * \param [in] shader Newly compiled shader
     */
    void registerShader(
      const Rc<DxvkShader>&         shader);

    /**
     * \brief Prioritizes compilation of a given shader
     *
     * Adds the pipeline library for the given shader
     * to the high-priority queue of the background
     * workers to make sure it gets compiled quickly.
     * \param [in] shader Newly compiled shader
     */
    void requestCompileShader(
      const Rc<DxvkShader>&         shader);

    /**
     * \brief Retrieves total pipeline count
     * \returns Number of compute/graphics pipelines
     */
    DxvkPipelineCount getPipelineCount() const;

    /**
     * \brief Checks whether async compiler is busy
     * \returns \c true if shaders are being compiled
     */
    DxvkPipelineWorkerStats getWorkerStats() const {
      return m_workers.getStats();
    }

    /**
     * \brief Compiles deferred pipelines
     *
     * See \ref DxvkPipelineWorkers::compileDeferred.
     * \param [in] budget Time budget
     * \param [in] scope What to compile
     * \returns Number of pipelines still queued in scope
     */
    size_t compileDeferredPipelines(std::chrono::microseconds budget, DxvkDeferredScope scope) {
      return m_workers.compileDeferred(budget, scope);
    }

    /**
     * \brief Queries deferred work statistics
     * \returns Statistics
     */
    DxvkDeferredCompileStats getDeferredStats() {
      DxvkDeferredCompileStats result = m_workers.getDeferredStats();
      result.librariesOnDemand = m_stats.numOnDemandLibraries.load();
      return result;
    }

    /**
     * \brief Queries descriptor layout for spec data UBO
     * \returns Descriptor set layout with an inline UBO
     */
    VkDescriptorSetLayout getSpecDataSetLayout() const {
      return m_specLayout;
    }

    /**
     * \brief Stops async compiler threads
     */
    void stopWorkerThreads();
    
  private:
    
    DxvkDevice*               m_device;
    DxvkPipelineWorkers       m_workers;
    DxvkPipelineStats         m_stats;

    VkDescriptorSetLayout     m_specLayout = VK_NULL_HANDLE;

    dxvk::mutex               m_layoutMutex;

    std::unordered_map<
      DxvkDescriptorSetLayoutKey,
      DxvkDescriptorSetLayout,
      DxvkHash, DxvkEq> m_descriptorSetLayouts;

    std::unordered_map<
      DxvkPipelineLayoutKey,
      DxvkPipelineLayout,
      DxvkHash, DxvkEq> m_pipelineLayouts;

    dxvk::mutex m_pipelineMutex;

    std::unordered_map<
      DxvkGraphicsPipelineVertexInputState,
      DxvkGraphicsPipelineVertexInputLibrary,
      DxvkHash, DxvkEq> m_vertexInputLibraries;

    std::unordered_map<
      DxvkGraphicsPipelineFragmentOutputState,
      DxvkGraphicsPipelineFragmentOutputLibrary,
      DxvkHash, DxvkEq> m_fragmentOutputLibraries;

    std::unordered_map<
      DxvkShaderPipelineLibraryKey,
      DxvkShaderPipelineLibrary,
      DxvkHash, DxvkEq> m_shaderLibraries;

    std::unordered_map<
      DxvkComputePipelineShaders,
      DxvkComputePipeline,
      DxvkHash, DxvkEq> m_computePipelines;

    std::unordered_map<
      DxvkGraphicsPipelineShaders,
      DxvkGraphicsPipeline,
      DxvkHash, DxvkEq> m_graphicsPipelines;

    DxvkShaderPipelineLibrary* createPipelineLibraryLocked(
      const DxvkShaderPipelineLibraryKey& key);

    DxvkShaderPipelineLibrary* createNullFsPipelineLibrary();

    DxvkShaderPipelineLibrary* findPipelineLibrary(
      const DxvkShaderPipelineLibraryKey& key);

    DxvkShaderPipelineLibrary* findPipelineLibraryLocked(
      const DxvkShaderPipelineLibraryKey& key);

    VkDescriptorSetLayout createSpecDataSetLayout();

  };
  
}
