#pragma once

// Engine side of bc250_dxvk_engine.h: the IBc250DxvkDevice object around DXVK's D3D11 device. This translation
// unit family never includes d3d10umddi.h (DXVK's util_gdi.h conflicts with the WDK); the shell translates DDI
// arguments into the WDK-free structures of the ABI header.
//
// Kto pod kim dołki kopie, ten sam w nie wpada: whoever digs pits under others falls into them himself.
// The runtime has validated every argument already; this layer translates, it does not second-guess.

#include <array>
#include <atomic>
#include <memory>
#include <vector>

#include "../d3d11/d3d11_device.h"
#include "../d3d11/d3d11_context_imm.h"

#include "bc250_dxvk_engine.h"

namespace dxvk::ddi {

  /**
   * \brief The shell's Log service as the process's DXVK log sink
   *
   * Registered from the start of CreateDevice until the device's final release.
   */
  class ShellLogSink {

  public:

    ShellLogSink(const BC250_DXVK_SHELL_SERVICES& Services);
    ~ShellLogSink();

    ShellLogSink             (const ShellLogSink&) = delete;
    ShellLogSink& operator = (const ShellLogSink&) = delete;

  private:

    void* m_shell;
    void (APIENTRY *m_log)(void *shell, UINT32 level, const char *message);

    static void Emit(void* context, LogLevel level, const char* line);

  };


  class Bc250DxvkDevice : public ComObject<IBc250DxvkDevice4> {

  public:

    Bc250DxvkDevice(
            D3D11DXGIDevice*                  pContainer,
      const BC250_DXVK_SHELL_SERVICES&        Services,
            std::unique_ptr<ShellLogSink>&&   LogSink);

    ~Bc250DxvkDevice();

    HRESULT STDMETHODCALLTYPE QueryInterface(
            REFIID                            riid,
            void**                            ppvObject) final;

    // E4: the final release reports a leaked D3D11 device as a nonzero count.
    ULONG STDMETHODCALLTYPE Release() final;

    HRESULT STDMETHODCALLTYPE GetD3D11Device(
            REFIID                            riid,
            void**                            ppDevice) final;

    HRESULT STDMETHODCALLTYPE GetImmediateContext(
            REFIID                            riid,
            void**                            ppContext) final;

    HRESULT STDMETHODCALLTYPE CreateShader(
      const BC250_DXVK_SHADER_DESC*           pDesc,
            REFIID                            riid,
            void**                            ppShader) final;

    HRESULT STDMETHODCALLTYPE CreateInputLayout(
      const BC250_DXVK_INPUT_LAYOUT*          pLayout,
            ID3D11InputLayout**               ppInputLayout) final;

    HRESULT STDMETHODCALLTYPE GetVertexFormat(
            DXGI_FORMAT                       Format,
            VkFormat*                         pVkFormat,
            UINT*                             pElementSize) final;

    HRESULT STDMETHODCALLTYPE GetImageCreateInfo(
      const D3D11_TEXTURE2D_DESC1*            pDesc,
            VkImageCreateInfo*                pInfo) final;

    HRESULT STDMETHODCALLTYPE CreateTexture2DFromImage(
      const D3D11_TEXTURE2D_DESC1*            pDesc,
            VkImage                           Image,
            ID3D11Texture2D**                 ppTexture) final;

    HRESULT STDMETHODCALLTYPE WaitForResourceIdle(
            ID3D11Resource*                   pResource) final;

    HRESULT STDMETHODCALLTYPE IsResourceBusy(
            ID3D11Resource*                   pResource,
            UINT                              Subresource) final;

    HRESULT STDMETHODCALLTYPE SubmitForPresent(
            ID3D11Resource*                   pSource,
            UINT                              Subresource) final;

    HRESULT STDMETHODCALLTYPE RotateResourceIdentities(
            ID3D11Resource* const*            ppResources,
            UINT                              Count) final;

    HRESULT STDMETHODCALLTYPE Blt(
      const BC250_DXVK_BLT*                   pBlt) final;

    HRESULT STDMETHODCALLTYPE Blt1(
      const BC250_DXVK_BLT1*                  pBlt) final;

    HRESULT STDMETHODCALLTYPE CreateTexture2DFromImage2(
      const D3D11_TEXTURE2D_DESC1*            pDesc,
      const VkImageCreateInfo*                pInfo,
            VkImage                           Image,
            ID3D11Texture2D**                 ppTexture) final;

    HRESULT STDMETHODCALLTYPE CheckFeatureSupportAtLevel(
            D3D_FEATURE_LEVEL                 FeatureLevel,
            D3D11_FEATURE                     Feature,
            void*                             pFeatureData,
            UINT                              FeatureDataSize) final;

    HRESULT STDMETHODCALLTYPE TrimMemory() final;

    HRESULT STDMETHODCALLTYPE TakeDeferredError() final;

  private:

    // View format lists handed out through GetImageCreateInfo's pNext; they live as long as the device.
    struct ViewFormatList {
      VkImageFormatListCreateInfo                           info = { VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO };
      std::array<VkFormat, DXGI_VK_FORMAT_FAMILY::MaxSize>  formats = { };
    };

    Com<D3D11DXGIDevice>        m_container;
    D3D11Device*                m_device  = nullptr;
    D3D11ImmediateContext*      m_context = nullptr;
    Rc<DxvkDevice>              m_dxvkDevice;
    BC250_DXVK_SHELL_SERVICES   m_services = { };
    std::unique_ptr<ShellLogSink> m_logSink;

    dxvk::mutex                                   m_viewFormatMutex;
    std::vector<std::unique_ptr<ViewFormatList>>  m_viewFormatLists;

    // End of the last SubmitFrame on DXVK's clock, in nanoseconds (0 before the first), and its thread
    std::atomic<int64_t>        m_lastPresent = { 0 };
    std::atomic<DWORD>          m_presentThread = { 0u };

    HRESULT CheckDeviceStatus() const;

    void SubmitFrame();

    // Deferred pipeline work within dxvk.inlinePipelineBudget; nothing if the budget is 0
    void CompileDeferred(DxvkDeferredScope Scope);

    // Whether CreateShader compiles pipeline libraries (dxvk.compileLibrariesOnCreate)
    bool CompileLibrariesOnCreate() const;

    // Info line with translation and deferred compile counts, at final release
    void LogShaderStats();

    // Blt and Blt1; the whole source subresource without pSourceRect
    HRESULT BltRegion(
            ID3D11Resource*                   pDestination,
            UINT                              DestinationSubresource,
      const RECT&                             DestinationRect,
            ID3D11Resource*                   pSource,
            UINT                              SourceSubresource,
      const RECT*                             pSourceRect,
            UINT                              Flags,
            UINT                              Rotation);

    bool CheckImageSupport(
      const VkImageCreateInfo&                info,
            VkImageTiling                     tiling) const;

    // CreateTexture2DFromImage(2); Tiling VK_IMAGE_TILING_MAX_ENUM lets DXVK choose as for its own images
    HRESULT WrapImage(
      const D3D11_TEXTURE2D_DESC1*            pDesc,
            VkImage                           Image,
            VkImageTiling                     Tiling,
            ID3D11Texture2D**                 ppTexture);

    const VkImageFormatListCreateInfo* GetViewFormatList(
      const DXGI_VK_FORMAT_FAMILY&            family);

  };


  /**
   * \brief Builds a DXBC container from a DDI shader
   *
   * The runtime passes the tokenized program and register-only signatures. The container carries
   * the program unchanged in a SHEX/SHDR chunk and signature chunks whose semantic names derive from
   * registers, and a valid DXBC hash, so that DXVK's regular shader path accepts it.
   * \param [in] pDesc DDI shader
   * \param [out] pContainer Container bytes
   * \param [out] pSoEntries Stream output declaration for D3D11, if any
   * \param [out] pSoNames Storage for the semantic names referenced by \c pSoEntries
   * \returns \c S_OK on success
   */
  HRESULT BuildShaderContainer(
    const BC250_DXVK_SHADER_DESC*             pDesc,
          std::vector<uint8_t>*               pContainer,
          std::vector<D3D11_SO_DECLARATION_ENTRY>* pSoEntries,
          std::vector<std::string>*           pSoNames);

  /**
   * \brief Program type of a DDI token stream
   * \returns D3D10_SB_TOKENIZED_PROGRAM_TYPE value, or ~0u
   */
  uint32_t GetProgramType(const UINT* pCode);

}
