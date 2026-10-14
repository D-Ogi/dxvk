#pragma once

#include "../util/config/config.h"
#include "../util/util_env.h"

#include "../vulkan/vulkan_loader.h"

namespace dxvk {

  struct DxvkOptions {
    DxvkOptions() { }
    DxvkOptions(const Config& config);

    /// Enable debug utils
    bool enableDebugUtils = false;

    /// Enable memory defragmentation
    Tristate enableMemoryDefrag = Tristate::Auto;

    /// Number of compiler threads
    /// when using the state cache
    int32_t numCompilerThreads = 0;

    /// Enable graphics pipeline library
    Tristate enableGraphicsPipelineLibrary = Tristate::Auto;

    /// With inline execution: time per presented frame for compiling
    /// deferred optimized pipelines, in microseconds. Zero keeps the
    /// fast-linked pipelines.
    int32_t inlinePipelineBudget = 0;

    /// With inline execution: translate shaders on worker threads, which
    /// make no Vulkan call, and compile their pipeline libraries later on
    /// the calling thread. If false, both happen at shader creation.
    bool translateShadersOnWorkers = true;

    /// With shaders translated on workers: whether shader creation also
    /// compiles pipeline libraries of translated shaders. Auto: on the
    /// thread that presents, or while nothing is presented, never where it
    /// would hold up the presenting thread. Read by the DDI engine; otherwise
    /// libraries wait for frame submission or the first draw that needs them.
    Tristate compileLibrariesOnCreate = Tristate::Auto;

    /// Enable descriptor heap
    Tristate enableDescriptorHeap = Tristate::Auto;

    /// Enable descriptor buffer
    Tristate enableDescriptorBuffer = Tristate::Auto;

    /// Enable unified image layout path
    bool enableUnifiedImageLayout = true;

    /// Enables pipeline lifetime tracking
    Tristate trackPipelineLifetime = Tristate::Auto;

    /// Shader-related options
    Tristate useRawSsbo = Tristate::Auto;

    /// HUD elements
    std::string hud;

    /// Forces swap chain into MAILBOX (if true)
    /// or FIFO_RELAXED (if false) present mode
    Tristate tearFree = Tristate::Auto;

    /// Enables latency sleep
    Tristate latencySleep = Tristate::Auto;

    /// Latency tolerance, in microseconds
    int32_t latencyTolerance = 0u;

    /// Disable VK_NV_low_latency2. This extension
    /// appears to be all sorts of broken on 32-bit.
    Tristate disableNvLowLatency2 = Tristate::Auto;

    // Hides integrated GPUs if dedicated GPUs are
    // present. May be necessary for some games that
    // incorrectly assume monitor layouts.
    bool hideIntegratedGraphics = false;

    /// Clears all mapped memory to zero.
    bool zeroMappedMemory = false;

    /// Allows full-screen exclusive mode on Windows
    bool allowFse = false;

    /// Whether to enable tiler optimizations
    Tristate tilerMode = Tristate::Auto;

    /// Overrides memory budget for DXVK
    VkDeviceSize maxMemoryBudget = 0u;

    /// Whether to use custom sin/cos approximation
    Tristate lowerSinCos = Tristate::Auto;

    /// Enables implicit resolves that are used to
    /// deal with MSAA-related undefined behaviour.
    bool enableImplicitResolves = true;

    /// Enables NV_raw_access_chains extension on Nvidia
    bool enableNvRawAccessChains = true;

    /// Enables CUDA interop extensions if available
    bool enableNvCudaInterop = true;

    /// Enable present timing features
    bool enablePresentTiming = true;

    /// Enable descriptor update templates
    bool enableDescriptorUpdateTemplates = env::is32BitHostPlatform();

    /// Device name
    std::string deviceFilter;
  };

}
