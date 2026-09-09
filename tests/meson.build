catch2_dep = dependency('catch2-with-main', required: true)

srcdir = '../src'
unittest_src = []
foreach src : gamescope_core_src + ['Script/Script.cpp', 'convar_script.cpp', 'Config/AppId.cpp', 'Config/ConfigManager.cpp', 'Audio/Volume.cpp', 'Clipboard/ClipboardSync.cpp', 'Keybinds.cpp', 'SteamFriends.cpp']
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
foreach src : ['Overlay/UI/Tokens.cpp', 'Overlay/UI/Lane.cpp', 'Overlay/UI/Row.cpp', 'Overlay/UI/Band.cpp', 'Overlay/UI/Registry.cpp', 'Overlay/UI/CommandPalette.cpp', 'Overlay/UI/Layout.cpp', 'Overlay/UI/Colors.cpp', 'Overlay/UI/Icons.cpp', 'Overlay/UI/Controls.cpp', 'Overlay/Fonts.cpp', 'Overlay/Palette.cpp', 'Overlay/ChangelogParse.cpp']
	unittest_src += srcdir / src
endforeach

gamescope_tests = executable(
	'gamescope_tests',
	unittest_src + font_headers + [bundled_scripts_header] + ['test_convar.cpp', 'test_process.cpp', 'test_config.cpp', 'test_audio_volume.cpp', 'test_settings_overlay_input.cpp', 'test_overlay_ui.cpp', 'test_overlay_atoms.cpp', 'test_overlay_shell.cpp', 'test_overlay_palette.cpp', 'test_cursor_policy.cpp', 'test_changelog_parse.cpp', 'test_bundled_scripts.cpp', 'test_clipboard_sync.cpp', 'test_crosshair.cpp', 'test_overlay_profiles.cpp', 'test_fps_counter.cpp', 'test_pointer_mapping.cpp', 'test_resolution.cpp', 'test_effects_curve.cpp', 'test_keybinds.cpp', 'test_steam_friends.cpp'],
	include_directories: [srcdir, sol2_include],
	dependencies: [catch2_dep, luajit_dep, cap_dep, drm_dep, glm_dep, wlroots_dep, nlohmann_json_dep, imgui_dep, xkbcommon],
	cpp_args: gamescope_cpp_args,
)
test('convar', gamescope_tests, args: ['[convar]'])
test('process', gamescope_tests, args: ['[process]'])
test('config', gamescope_tests, args: ['[config]'])
test('audio_volume', gamescope_tests, args: ['[audio_volume]'])
test('settings_overlay_input', gamescope_tests, args: ['[settings_overlay_input]'])
test('overlay_ui', gamescope_tests, args: ['[overlay_ui]'])
test('overlay_atoms', gamescope_tests, args: ['[overlay_atoms]'])
test('overlay_shell', gamescope_tests, args: ['[overlay_shell]'])
test('overlay_palette', gamescope_tests, args: ['[overlay_palette]'])
test('cursor_policy', gamescope_tests, args: ['[cursor_policy]'])
test('changelog_parse', gamescope_tests, args: ['[changelog_parse]'])
test('bundled_scripts', gamescope_tests, args: ['[bundled_scripts]'])
test('clipboard_sync', gamescope_tests, args: ['[clipboard_sync]'])
test('crosshair', gamescope_tests, args: ['[crosshair]'])
test('overlay_profiles', gamescope_tests, args: ['[overlay_profiles]'])
test('fps_counter', gamescope_tests, args: ['[fps_counter]'])
test('pointer_mapping', gamescope_tests, args: ['[pointer_mapping]'])
test('effects_curve', gamescope_tests, args: ['[effects_curve]'])
test('resolution', gamescope_tests, args: ['[resolution]'])
test('keybinds', gamescope_tests, args: ['[keybinds]'])
test('steam_friends', gamescope_tests, args: ['[steam_friends]'])

# ---------------------------------------------------------------------------
#  The Steam client stand-ins, for test_steam_friends.cpp
# ---------------------------------------------------------------------------
# src/SteamFriends.cpp's loader has to survive five states a HEALTHY Steam is
# never in (superdoc/planning/steam-friends-join.md §6d), so there is no way to
# provoke them with the real client -- and the machine this fork is developed on
# is usually mid-match. These two libraries provoke them instead. Neither is
# installed, neither is linked into anything; the test dlopens them by path.
#
#   libsteamclient_stub.so  -- Steam-shaped: exports CreateInterface AND the
#                              Steam_* fingerprint symbols. Its behaviour is
#                              chosen by GS_RITZ_TEST_STEAMCLIENT_MODE.
#   libnotsteam_stub.so     -- exports CreateInterface and nothing else, which
#                              is the "that library is not Steam's client" path.
#
# They are built into this same directory as gamescope_tests, which is how the
# test finds them (from /proc/self/exe) with no env plumbing.
shared_library('steamclient_stub', ['steamclient_stub.cpp'], cpp_args: gamescope_cpp_args)
shared_library('notsteam_stub', ['steamclient_stub.cpp'], cpp_args: gamescope_cpp_args + ['-DSTUB_NOT_STEAM'])

# The live read check. Built, never registered as a test: it needs a Steam that
# is installed, running and signed in, so as a test it would fail on every
# machine without one. A fake built from the same declaration cannot prove the
# declaration is right -- this can, against the real client, read-only. See the
# file's own header.
steam_friends_live_src = []
foreach src : gamescope_core_src + ['Script/Script.cpp', 'convar_script.cpp', 'SteamFriends.cpp']
	steam_friends_live_src += srcdir / src
endforeach
executable(
	'steam_friends_live_probe',
	steam_friends_live_src + [bundled_scripts_header, 'steam_friends_live_probe.cpp'],
	include_directories: [srcdir, sol2_include],
	# convar_script.cpp reaches backend.h, which reaches drm_include.h -- hence
	# the graphics deps on a binary that draws nothing.
	dependencies: [luajit_dep, cap_dep, drm_dep, glm_dep, wlroots_dep, nlohmann_json_dep],
	cpp_args: gamescope_cpp_args,
)

# scripts/pointer-regression.sh's test client -- not a meson test (it needs a
# running gamescope), just built here so the script has a binary. See the
# file's own header.
if sdl2_dep.found()
	executable(
		'pointer_lock_client',
		['pointer_lock_client.c'],
		dependencies: [sdl2_dep],
	)
	# scripts/effects-regression.sh's synthetic-scene client, same shape.
	executable(
		'effects_scene_client',
		['effects_scene_client.c'],
		dependencies: [sdl2_dep],
	)
endif

# The host mouse for the same script: a zwlr_virtual_pointer_v1 client that
# plugs a pointer into the private sway so the nested backend's own host path
# (absolute samples, host lock, relative motion) runs for real. The protocol
# XML is a verbatim copy of wlroots' protocol/wlr-virtual-pointer-unstable-v1.xml
# (meson refuses to read a file out of a subproject); sway implements it.
wayland_client_tests_dep = dependency('wayland-client', required: false)
virtual_pointer_xml = files('wlr-virtual-pointer-unstable-v1.xml')
if wayland_client_tests_dep.found()
	virtual_pointer_proto = [
		custom_target(
			'wlr-virtual-pointer-unstable-v1-tests-protocol.c',
			input: virtual_pointer_xml,
			output: 'wlr-virtual-pointer-unstable-v1-protocol.c',
			command: [wayland_scanner, 'private-code', '@INPUT@', '@OUTPUT@'],
		),
		custom_target(
			'wlr-virtual-pointer-unstable-v1-tests-client-protocol.h',
			input: virtual_pointer_xml,
			output: 'wlr-virtual-pointer-unstable-v1-client-protocol.h',
			command: [wayland_scanner, 'client-header', '@INPUT@', '@OUTPUT@'],
		),
	]
	executable(
		'virtual_pointer_tool',
		['virtual_pointer_tool.c'] + virtual_pointer_proto,
		dependencies: [wayland_client_tests_dep],
	)
endif

# The Wine-shaped client for the same script: a Win32 program (Unity's
# cursor lock/confine/menu states, ChangeDisplaySettings, WM_INPUT) built with
# the mingw cross compiler and run with the system wine inside gamescope's
# Xwayland. Not a meson test; the script SKIPs its checks without the .exe or
# wine. See the file's own header.
mingw_cc = find_program('x86_64-w64-mingw32-gcc', required: false)
if mingw_cc.found()
	custom_target(
		'pointer_probe_win32',
		input: files('pointer_probe_win32.c'),
		output: 'pointer_probe_win32.exe',
		command: [mingw_cc, '-O1', '-Wall', '-mconsole', '-o', '@OUTPUT@', '@INPUT@', '-lgdi32', '-luser32'],
		build_by_default: true,
	)
endif

# The CS2-shaped client: native SDL3 relative mode under Xwayland, with an
# XI2 raw-event tap. See the file's own header.
sdl3_dep = dependency('sdl3', required: false)
if sdl3_dep.found()
	executable(
		'pointer_grab_client_x11',
		['pointer_grab_client_x11.c'],
		dependencies: [sdl3_dep, dep_x11, dep_xi],
	)
endif
