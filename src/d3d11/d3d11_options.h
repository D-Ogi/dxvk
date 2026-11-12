#pragma once

#include "../util/config/config.h"

#include "../dxgi/dxgi_options.h"

#include "../dxvk/dxvk_device.h"

#include "d3d11_include.h"

namespace dxvk {

  struct D3D11Options {
    D3D11Options(const Config& config);

    /// Force thread-group shared memory accesses to be volatile
    ///
    /// Workaround for compute shaders that read and
    /// write from the same shared memory location
    /// without explicit synchronization.
    bool forceComputeLdsBarriers = false;

    /// Force UAV synchronization insided compute shaders
    ///
    /// Workaround for compute shaders that access overlapping
    /// memory regions within a UAV without proper workgroup
    /// synchroniation. Will have a negative performance impact.
    bool forceComputeUavBarriers = false;

    /// Use relaxed memory barriers
    ///
    /// May improve performance in some games,
    /// but might also cause rendering issues.
    bool relaxedBarriers = false;

    /// Ignore graphics barriers
    ///
    /// May improve performance in some games,
    /// but might also cause rendering issues.
    bool relaxedGraphicsBarriers = false;

    /// Maximum tessellation factor.
    ///
    /// Limits tessellation factors in tessellation
    /// control shaders. Values from 8 to 64 are
    /// supported, other values will be ignored.
    int32_t maxTessFactor = 0;

    /// Anisotropic filter override
    ///
    /// Enforces anisotropic filtering with the
    /// given anisotropy value for all samplers.
    int32_t samplerAnisotropy = -1;

    /// Mipmap LOD bias
    ///
    /// Enforces the given LOD bias for all samplers.
    float samplerLodBias = 0.0f;

    /// Clamps negative LOD bias
    bool clampNegativeLodBias = false;

    /// Override maximum frame latency if the app specifies
    /// a higher value. May help with frame timing issues.
    int32_t maxFrameLatency = 0;

    /// Defer surface creation until first present call. This
    /// fixes issues with games that create multiple swap chains
    /// for a single window that may interfere with each other.
    bool deferSurfaceCreation = false;

    /// Enables sample rate shading by interpolating fragment shader
    /// inputs at the sample location rather than pixel center,
    /// unless otherwise specified by the application.
    bool forceSampleRateShading = false;

    /// Forces the sample count of all textures to be 1, and
    /// performs the required shader and resolve fixups.
    bool disableMsaa = false;

    /// Dynamic resources with the given bind flags will be allocated
    /// in cached system memory. Enabled automatically when recording
    /// an api trace.
    uint32_t cachedDynamicResources = 0;

    /// bc250 (BD-309): staging storage of at least this many bytes gets
    /// a VkDeviceMemory of its own, one kernel allocation that the driver
    /// can offer to the OS (DXGI OfferResources). 0 disables it.
    uint32_t offerDedicatedStagingSize = 1u << 20;

    /// bc250 (BD-373): GPU-only storage (DEFAULT and IMMUTABLE, not shared)
    /// of at least this many bytes gets a VkDeviceMemory of its own when it
    /// is created: one kernel allocation that the driver can offer to the
    /// OS (DXGI OfferResources), and that the UMD logging names at the
    /// creation. Smaller storage is suballocated, and its offer is a hint
    /// (an offer never moves storage). 0 disables the rule: then no
    /// GPU-only storage can be offered, except storage that the allocator
    /// made dedicated.
    uint32_t offerDedicatedSize = 1u << 20;

    /// Always lock immediate context on every API call. May be
    /// useful for debugging purposes or when applications have
    /// race conditions.
    bool enableContextLock = false;

    /// Whether to expose the driver command list feature. Enabled by
    /// default and generally beneficial, but some games may assume that
    /// this is not supported when running on an AMD GPU.
    bool exposeDriverCommandLists = true;

    /// Ensure that for the same D3D commands the output VK commands
    /// don't change between runs. Useful for comparative benchmarking,
    /// can negatively affect performance.
    bool reproducibleCommandStream = false;

    /// Whether to force a staging buffer for mapped images.
    /// Some games are broken and ignore row pitch.
    bool disableDirectImageMapping = false;

    /// Shader dump path
    std::string shaderDumpPath;
  };
  
}
