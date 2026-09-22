vkroots_dep = dependency('vkroots')
dep_xcb = dependency('xcb')
dep_x11_xcb = dependency('x11-xcb')
wayland_client = dependency('wayland-client')

# THIS FORK'S LAYER HAS ITS OWN IDENTITY, and that is the whole point of the
# names below. Upstream's layer is called VK_LAYER_FROG_gamescope_wsi_<family>
# and activates on ENABLE_GAMESCOPE_WSI -- which is also exactly what the
# distro's own gamescope package installs. Two files claiming one layer name
# is a collision the Vulkan loader resolves by search order, so whichever
# gamescope you ran, you got whichever layer the loader happened to find
# first.
#
# That is not hypothetical: a Nobara/Fedora 44 box ran this compositor against
# the distro's OLDER layer and every Vulkan client died instantly --
# `swapchain_feedback` gained a 7th argument upstream in 6a4d150 (2024-12-04),
# the old layer sent six, and the compositor dropped the connection as a
# protocol error. See superdoc/features/build-and-tooling.md.
#
# So: distinct layer name, distinct .so filename, distinct enable/disable env
# vars. Both layers can now sit in the loader's search path without either
# shadowing the other, and steamcompmgr.cpp picks ours for OUR children by
# setting only our enable var -- leaving the distro layer dormant for us and
# untouched for the packaged gamescope.
layer_name    = 'VK_LAYER_RITZ_gamescope_wsi'
layer_file    = 'VkLayer_RITZ_gamescope_wsi'
layer_desc    = 'Gamescope-Ritz WSI (XWayland Bypass) Layer'
layer_enable  = 'ENABLE_GAMESCOPE_RITZ_WSI'
layer_disable = 'DISABLE_GAMESCOPE_RITZ_WSI'

gamescope_wsi_layer = shared_library(layer_file + '_' + build_machine.cpu_family(), 'VkLayer_FROG_gamescope_wsi.cpp', protocols_client_src,
  dependencies     : [ vkroots_dep, dep_xcb, dep_x11, dep_x11_xcb, glm_dep, wayland_client ],
  install          : true )

out_lib_dir = join_paths(prefix, lib_dir)

layer_json_name = layer_file + '.' + build_machine.cpu_family() + '.json'

configure_file(
    input         : 'VkLayer_FROG_gamescope_wsi.json.in',
    output        : layer_json_name,
    configuration : {
      'family'      : build_machine.cpu_family(),
      'lib_dir'     : out_lib_dir,
      'layer_name'  : layer_name,
      'layer_file'  : layer_file,
      'layer_desc'  : layer_desc,
      'enable_env'  : layer_enable,
      'disable_env' : layer_disable,
    },
    install       : true,
    install_dir   : join_paths(data_dir, 'vulkan', 'implicit_layer.d'),
)

# Where that JSON lands once installed. steamcompmgr.cpp tests for this exact
# file at startup to decide whether it can use our layer or must fall back to
# whatever the system provides -- see src/meson.build.
ritz_layer_json_path = join_paths(prefix, data_dir, 'vulkan', 'implicit_layer.d', layer_json_name)
ritz_layer_enable_env = layer_enable