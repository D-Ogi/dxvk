# bc250dxvk.dll: DXVK as the engine behind the BC-250 system D3D10/11 user-mode driver.
# The DLL contains the same D3D10/11 implementation as d3d11.dll plus the engine object in this directory,
# and exports one function (bc250dxvk.def). It never creates a swap chain or loads a Vulkan loader: the UMD
# shell hands it the hosted VkInstance and VkDevice (bc250_dxvk_engine.h).

ddi_src = [
  'ddi_engine.cpp',
  'ddi_device.cpp',
  'ddi_shader.cpp',
]

bc250dxvk_dll = shared_library('bc250dxvk', d3d11_engine_src + ddi_src,
    glsl_generator.process(d3d11_engine_shaders),
  dependencies        : [ lib_gdi32, lib_dxgi, dxvk_dep, dxbc_spirv_dep ],
  include_directories : dxvk_include_path,
  install             : true,
  vs_module_defs      : 'bc250dxvk.def',
  name_prefix         : '',
)

# Offline positive control: plays the UMD shell against the DLL on any Vulkan 1.3 GPU (engine_test.cpp).
# It uses only the ABI header, never DXVK internals, so it also checks that the header stands alone.
executable('bc250dxvk_engine_test', 'engine_test.cpp',
  dependencies        : [ cpp.find_library('d3dcompiler'), cpp.find_library('psapi') ],
  include_directories : include_directories('../../include/vulkan/include'),
  install             : false,
)
