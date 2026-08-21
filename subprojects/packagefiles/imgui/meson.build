project('imgui', 'cpp', version : '1.92.9b', default_options: ['cpp_std=c++17'])

# Dear ImGui ships no meson.build of its own -- this overlay follows the same
# pattern as subprojects/packagefiles/{stb,glm}/meson.build. Only the core
# library plus the Vulkan renderer backend are built: gamescope-ritz owns its
# own platform integration (there is no windowing backend to hand ImGui here,
# see superdoc/planning/SPEC.md's Architecture section), so none of the
# platform backends (GLFW/SDL/Win32/...) are compiled.

imgui_src = [
  'imgui.cpp',
  'imgui_draw.cpp',
  'imgui_tables.cpp',
  'imgui_widgets.cpp',
  'backends/imgui_impl_vulkan.cpp',
]

vulkan_dep = dependency('vulkan')

imgui_lib = static_library(
  'imgui',
  imgui_src,
  include_directories : include_directories('.', 'backends'),
  dependencies : [vulkan_dep],
  cpp_args : ['-Wno-unused-function'],
)

imgui_dep = declare_dependency(
  link_with : imgui_lib,
  include_directories : include_directories('.', 'backends'),
  dependencies : [vulkan_dep],
  version : meson.project_version(),
)

meson.override_dependency('imgui', imgui_dep)
