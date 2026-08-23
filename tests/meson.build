catch2_dep = dependency('catch2-with-main', required: true)

srcdir = '../src'
unittest_src = []
foreach src : gamescope_core_src + ['Script/Script.cpp', 'convar_script.cpp', 'Config/AppId.cpp', 'Config/ConfigManager.cpp', 'Audio/Volume.cpp']
	unittest_src += srcdir / src
endforeach

# The E2 overlay kit. Tokens/Lane/Row/Band/Registry are deliberately free of an
# ImGui context, a font atlas and the overlay's live-theme globals, so the
# parts of the design with real arithmetic and real laws in them are testable
# on their own (test_overlay_ui.cpp).
#
# Colors.cpp and Controls.cpp do need ImGui and the font set, and are covered
# by test_overlay_atoms.cpp, which runs ImGui headlessly -- no window, no
# renderer, no compositor. That is why imgui_dep and font_headers are here:
# ImRect's inline maths for the pure tests, and the real embedded Geist faces
# so the atoms measure text exactly as the shipping binary does.
foreach src : ['Overlay/UI/Tokens.cpp', 'Overlay/UI/Lane.cpp', 'Overlay/UI/Row.cpp', 'Overlay/UI/Band.cpp', 'Overlay/UI/Registry.cpp', 'Overlay/UI/Colors.cpp', 'Overlay/UI/Controls.cpp', 'Overlay/Fonts.cpp', 'Overlay/Palette.cpp']
	unittest_src += srcdir / src
endforeach

gamescope_tests = executable(
	'gamescope_tests',
	unittest_src + font_headers + ['test_convar.cpp', 'test_process.cpp', 'test_config.cpp', 'test_audio_volume.cpp', 'test_settings_overlay_input.cpp', 'test_overlay_ui.cpp', 'test_overlay_atoms.cpp'],
	include_directories: [srcdir, sol2_include],
	dependencies: [catch2_dep, luajit_dep, cap_dep, drm_dep, glm_dep, wlroots_dep, nlohmann_json_dep, imgui_dep],
	cpp_args: gamescope_cpp_args,
)
test('convar', gamescope_tests, args: ['[convar]'])
test('process', gamescope_tests, args: ['[process]'])
test('config', gamescope_tests, args: ['[config]'])
test('audio_volume', gamescope_tests, args: ['[audio_volume]'])
test('settings_overlay_input', gamescope_tests, args: ['[settings_overlay_input]'])
test('overlay_ui', gamescope_tests, args: ['[overlay_ui]'])
test('overlay_atoms', gamescope_tests, args: ['[overlay_atoms]'])
