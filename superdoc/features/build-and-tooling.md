# Build & Tooling — Meson build, feature flags, SteamOS device scripts

The Meson build definition, its feature-flag surface, and the shell scripts used to
build, deploy, and reset Gamescope on a real SteamOS handheld/desktop device over SSH.

## How it works

- `meson.build` is the root build definition (C/C++20, `warning_level=2`). It pins two
  vendored dependencies via `force_fallback_for: 'libliftoff,vkroots'` and hard-fails the
  configure step if that list is trimmed (`meson.build:13-16`) — *Why:* those two
  subprojects are pulled in at specific forked/patched commits Gamescope depends on, so
  Meson silently falling back to a system package would be a correctness bug, not just a
  packaging inconvenience.
- **wlroots is probed in three steps** (`src/meson.build`, 2026-09-22): a system
  `wlroots-0.20`, then a system `wlroots-0.19`, then the pinned submodule (0.20.2) as
  meson's `fallback:`. *Why three separate `dependency()` calls* rather than
  `dependency()`'s multi-name form: the two minors need different version ranges, and
  the multi-name form needs meson >= 0.60 while this project declares >= 0.58. 0.19 is
  **system-only** — there is no second wrap, and none is worth adding until something
  actually needs to build 0.19 from source.
  - `install.sh`'s pre-flight check (`gcr_wlroots_candidates` / `gcr_check_wlroots`)
    scrapes those probes out of `src/meson.build` so it cannot drift from what the build
    really asks for, and accepts the first module pkg-config satisfies — the same order
    meson tries. *Why it flattens newlines first:* the scraper used to read line-by-line
    with `getline`, which broke the moment the 0.19 probe made the first `dependency()`
    a one-liner — it then reported the module name as `ifnotwlroots_dep.found()`.
- **`--update` re-execs itself when the pull changes the installer.** bash has already
  read `install.sh`, and `scripts/gamescope-ritz-common.sh` is sourced at startup —
  *before* the `git pull` — so an update that changes either one would otherwise run the
  **old** code against the **new** tree for the rest of that run. Seen for real: the
  commit making the WSI layer a default ninja target was pulled by an `--update` that
  then built with the previous `gcr_build()` and skipped the layer, so the fix looked
  like it had not worked. `do_update()` compares HEAD before and after the pull and
  `exec`s itself with the original argv; `GCR_REEXECED` guards against a loop.
- **This fork installs its OWN Vulkan WSI layer, under its own name** (2026-09-22).
  `layer/meson.build` builds `libVkLayer_RITZ_gamescope_wsi_<family>.so` with the layer
  name `VK_LAYER_RITZ_gamescope_wsi_<family>`, activated by
  `ENABLE_GAMESCOPE_RITZ_WSI` and suppressed by `DISABLE_GAMESCOPE_RITZ_WSI`. `install.sh`
  installs it beside the binary (`gcr_install_wsi_layer`), and `--remove` offers to take
  it away again.
  - **The bug it fixes.** The layer is loaded into the *game's* process, not gamescope's,
    and the compositor and the layer are two halves of one protocol that must be
    version-matched. Before this, the fork shipped no layer of its own, so games loaded
    whichever `VkLayer_FROG_gamescope_wsi` the distro's `gamescope` package provided.
    Upstream `6a4d150` ("mangoapp: plumb engineName", 2024-12-04) added a 7th argument
    (`vk_engine_name`) to `swapchain_feedback`; a layer built before that sends six, so
    the compositor read a short message, logged
    `[wayland] message too short, object (46), message swapchain_feedback(uuuuuus)`
    followed by `error in client communication`, and **dropped that client's
    connection**. From then on every surface failed with
    `[Gamescope WSI] Failed to get Wayland objects`, once per `xid`, forever. Diagnosed
    on Nobara/Fedora 44 where *every* Vulkan client — vkcube and Rocket League alike —
    started and instantly quit. It reproduced on stock gamescope built from source too,
    because the fault is the layer being old, not the compositor being this fork: the
    fork has never touched `protocol/gamescope-swapchain.xml` or `layer/`.
  - *Why a distinct name and not just installing over the distro's:* both manifests
    would claim `VK_LAYER_FROG_gamescope_wsi_<family>`, and the loader resolves a
    duplicate name by search order — `/usr/local/share/vulkan` is read before
    `/usr/share/vulkan`, so ours would silently shadow the distro's for the *packaged*
    gamescope as well, breaking it in the mirror-image direction. Distinct names let
    both sit in the search path, each dormant unless its own enable var is set. That
    also keeps the project's standing rule that the user's packaged gamescope is never
    disturbed.
  - **The fallback, and why it must exist.** `steamcompmgr.cpp` sets *only*
    `ENABLE_GAMESCOPE_RITZ_WSI` — but just when our manifest is really on disk, tested
    with `access()` against the path `src/meson.build` bakes in as
    `GAMESCOPE_RITZ_WSI_LAYER_JSON`. If it is absent (a binary copied somewhere without
    the layer, or `-Denable_gamescope_wsi_layer=false`) it sets upstream's
    `ENABLE_GAMESCOPE_WSI` instead, so the system layer is still used. Without that,
    a missing layer would mean *no* WSI layer at all — a worse regression than the skew
    this exists to fix.
  - `GAMESCOPE_RITZ_USE_SYSTEM_WSI=1` forces the fallback by hand. *Why an escape
    hatch:* the layer runs inside the game's process, so a bad one cannot be worked
    around from in there — the only place to turn it off is in the environment, before
    the game starts.
  - `run_wsi_layer_check()` reports which layer is in play. If ours is installed it says
    so and stops; otherwise it greps the system layer for the 7-argument signature
    literal `uuuuuus` that wayland-scanner bakes in, and warns when it is missing. *Why
    a grep and not a version comparison:* nothing in a `.so` says which gamescope built
    it, and the signature IS what has to match; `grep -a` rather than `strings(1)` so it
    needs no binutils.
  - **Installing the layer is never fatal.** It happens *after* the binary is already
    in place, so a failure there must not abort the run — `set -e` on a failed `sudo`
    would leave a working install half-finished and skip everything after it. Seen for
    real: `./install.sh --update` from a non-TTY shell, where sudo cannot prompt
    (*"a terminal is required to read the password"*), took down the rest of the update.
    It now warns, prints the two `sudo install` lines to finish by hand, and continues —
    the compositor's fallback means the install is still usable without it.
  - `gcr_remove_wsi_layer()` only ever deletes a manifest whose basename contains
    `RITZ_gamescope_wsi`, and `--remove` defaults that confirmation to **no** — unlike
    the binary, these are files in a shared system directory.
- **The program check is separate from the library check, and both are needed.**
  `gcr_check_build_tools()` covers `meson`, `cmake`, `ninja`, `pkg-config`, `git` and
  the glslang shader compiler; the pkg-config gate below covers the `dependency()`
  calls. *Why both:* a `find_program()` is invisible to a pkg-config scan — a Fedora 44
  report cleared every library check and then died at `src/meson.build:55` with
  "Program 'glslang glslangValidator' not found", because the shader compiler is a
  binary, not a module. Only two external programs are hard-required: `wayland-scanner`
  (which arrives with its own pkg-config module, so the library gate already covers it)
  and glslang. The rest of the `find_program()` calls are this repo's own Python
  scripts. Meson accepts *either* `glslang` or `glslangValidator`, so only the absence
  of both is a failure.
  - This one carries a small command→package map (Fedora ships `/usr/bin/ninja` in a
    package called `ninja-build`). *Why a map is acceptable here but not for the
    libraries:* five build-tool names that essentially never change, versus twenty-five
    library names across three distros that would rot — which is why the library list is
    scraped instead.
- **`install.sh` gates on the hard dependencies before meson runs**
  (`gcr_hard_pkgconfig_modules` / `gcr_check_pkgconfig_deps`, 2026-09-22). It scrapes
  every `dependency('...')` in `meson.build`, `src/meson.build`, `protocol/meson.build`
  and `layer/meson.build`, drops the ones gated by `required: false` or
  `required: get_option(...)`, and checks the rest with pkg-config. *Why scraped, not a
  hardcoded list:* a hand-kept copy drifts from meson.build and then lies — the same
  reasoning as the wlroots check above.
  - Three names are excluded by hand: `threads` (a meson builtin), `openvr_api` (a
    subproject dependency object) and `vkroots` (pinned to the vendored copy by
    `force_fallback_for`). `wlroots-*` is excluded too, having its own multi-version
    check.
  - *Why `required: true` must NOT be treated as optional:* `src/meson.build`'s
    `libinput` says it out loud, and a filter that drops every line matching "required"
    silently skips it. `dependency( 'luajit' )`'s space after the paren is the matching
    trap on the regex side. Both were live gaps in the first cut.
  - *Why it exists:* without it the failure lands hundreds of lines into `meson setup`.
    A real report (Fedora 44) died at `protocol/meson.build:7` with
    "Neither a subproject directory nor a wayland-protocols.wrap file was found" —
    after a full compiler probe and an openvr cmake configure — which reads like a build
    bug rather than a missing package.
  - On Fedora/RHEL the hint is exact, because dnf resolves `pkgconfig(foo)` virtual
    provides: the module name *is* the package name, so no lookup table is needed.
    pacman and apt get a file-search command instead. *Why no module→package map:* three
    distro tables is exactly the kind of thing that rots; the search command answers the
    same question and stays true on its own.
- **The embedded version can go stale, and the scripts are what prevent it.**
  `src/meson.build` derives `k_szRitzCommit` / `k_szRitzPatchDate` with `run_command()`
  feeding a `configure_file()`; both run at *configure* time, and meson re-runs configure
  only when a `meson.build` changes — git HEAD moving is invisible to it. Every script
  here funnels through `gcr_build()`, which always calls `gcr_meson_configure()`
  (`meson setup --reconfigure`), so a scripted build always re-reads git. A bare
  `ninja -C <dir>` in a hand-made build directory does **not**, and silently produces a
  binary reporting an old commit. `gcr_report_version()` prints the embedded version
  after every scripted build and warns when it disagrees with HEAD, which turns that
  silent case into a visible one. *Why not fix it in meson:* `vcs_tag()` /
  `build_always_stale` would, but no supported path is affected — see the `ponytail:`
  note on that function for the upgrade path.
- All boolean/feature options live in `meson_options.txt` (11 options total, read
  directly):
  | Option | Type | Meaning |
  | --- | --- | --- |
  | `pipewire` | feature | Screen capture via PipeWire (`meson_options.txt:1`) |
  | `rt_cap` | feature | Real-time threads + compute queues support (`:2`) |
  | `drm_backend` | feature | DRM Atomic Backend (`:3`) |
  | `sdl2_backend` | feature | SDL2 Window Backend (`:4`) |
  | `avif_screenshots` | feature | Saving `.AVIF` HDR screenshots (`:5`) |
  | `input_emulation` | feature | XTest/libei input emulation (`:6`) |
  | `enable_gamescope` | boolean (default `true`) | Build the Gamescope executable (`:7`) |
  | `enable_gamescope_wsi_layer` | boolean (default `true`) | Build the Gamescope Vulkan WSI layer (`:8`) |
  | `enable_openvr_support` | boolean (default `true`) | OpenVR integration (`:9`) |
  | `enable_tests` | boolean (default `true`) | Build unit tests (`:10`) |
  | `benchmark` | feature | Benchmark tools (`:11`) |
- Feature options resolve to real dependency lookups in `src/meson.build`: e.g.
  `eis_dep = dependency('libeis-1.0', required: get_option('input_emulation'))`
  (`src/meson.build:14`) feeds `-DHAVE_LIBEIS=@0@'.format(eis_dep.found().to_int())`
  (`src/meson.build:153`), the compile-time flag gating
  [input-emulation](input-emulation.md)'s `InputEmulation.cpp`. Similarly `drm_backend`,
  `sdl2_backend`, `avif_screenshots`, and `rt_cap` each map to a `dependency(...,
  required: get_option(...))` call (`src/meson.build:13,20,22,23`).
- `enable_gamescope_wsi_layer` and `enable_gamescope` gate whole build targets in
  `meson.build:97,101`; `enable_tests` only takes effect when `enable_gamescope` is also
  true (`meson.build:105`).
- **Nothing is installed beside the binary** (2026-09-09). There is no
  `meson.add_install_script` any more; `default_extras_install.sh`, which used to copy
  `scripts/`, `looks/` and the font licence into `share/gamescope-ritz`, is deleted, and
  so are `install.sh`'s `--extras`/`--no-extras` flags and its prompt. **Why:** that
  prompt defaulted to *no*, while `meson.build` baked the same path in as `SCRIPT_DIR`
  and `Script.cpp` loaded it at startup — so taking the default silently produced an
  install with no known-displays database at all, reported only as a `warnf` in a log
  nobody reads. A step whose default answer breaks the product should not exist.

### What is compiled into the binary, and by which rule

  All four use the same "generated header holding a byte array" shape `glsl_generator`
  already used for shaders — one build-time pattern, not four:

  | Data | Generator | Meson rule | Consumed by |
  |---|---|---|---|
  | Geist Sans/Mono `.ttf` | `src/Overlay/fonts/embed_font.py` | `font_embed_gen` | `Overlay/Fonts.cpp` |
  | `CHANGELOG.md` | `src/Overlay/embed_changelog.py` | `changelog_header` | `Overlay/PanelChangelog.cpp` |
  | `LICENSE`, `THIRD-PARTY-LICENSES.md`, `LICENSE-OFL.txt` | `embed_font.py` again, with a `g_Asset_` symbol prefix | `asset_embed_gen` | `Overlay/PanelChangelog.cpp` |
  | `scripts/00-gamescope/**.lua` | `src/Script/embed_scripts.py` | `bundled_scripts_header` | `Script/Script.cpp` |

- `embed_scripts.py` emits one byte array per `.lua` file plus an ordered table
  (`g_BundledScripts`). The **order is part of the contract**: `common/util.lua` and
  `common/modegen.lua` define globals the `displays/*.lua` files call at load time, so
  the generator reproduces `CScriptManager::RunFolder`'s traversal exactly — every `.lua`
  in a directory sorted, then every subdirectory sorted, recursively. An empty scripts
  tree fails the build loudly rather than shipping a binary with no display database.
  Adding a display file means adding it to `bundled_scripts_header`'s `depend_files`
  list; meson has no glob, and that is deliberate here.
- `SCRIPT_DIR` (`meson.build:80`, `$prefix/share/gamescope-ritz/scripts`) survives as a
  place a **packager** may put a replacement tree. Nothing fills it; when it is absent —
  the normal case — the embedded copies run. See
  [scripting-convars.md](scripting-convars.md) for the full precedence order.
- `looks/` is **not** embedded and never was loadable from an install: `cc_set_look` takes
  a path the caller supplies and `gamescope_control`'s `set_look` takes file descriptors,
  so no code ever searched an installed `looks` directory. It stays in the source tree as
  `.cube` files a user can point `set_look` at.
- The OFL requires (clause 2) that each copy of the Font Software distributed with other
  software contain the copyright notice and the licence, "either as stand-alone text
  files, human-readable headers or in the appropriate machine-readable metadata fields
  within text or binary files **as long as those fields can be easily viewed by the
  user**". The Geist glyph data is inside the binary, so the licence is too — and the
  About area prints it, which is what makes it "easily viewed". `LICENSE-OFL.txt` also
  stays in the source tree.

### SteamOS device tooling (`tools/`)

- `tools/build_and_install_on_steamos_device_remote.sh` sources
  `steamos_common_remote.sh` for connection/credential setup, rsyncs the tree to the
  device (`copy_to_steamos_device_rsync.sh`), then SSHes in and runs the on-device
  counterpart `build_and_install_on_steamos_device_local.sh` (which actually invokes
  Meson/ninja on the device itself).
- `tools/steamos_sessionctl_remote.sh` takes a `<verb> <device_ip> [password]`, rsyncs the
  tree, then SSHes in to run `steamos_sessionctl_local.sh <verb>` on-device — used to
  control the running Gamescope session (start/stop/restart-style verbs) without a local
  build step.
- `tools/reset_to_system_gamescope_remote.sh` / `reset_to_system_gamescope_local.sh`
  revert a device back to its stock, package-provided Gamescope instead of the locally
  built one — the rollback counterpart to the install scripts.
- `steamos_common_remote.sh` / `steamos_common_local.sh` and
  `steamos_password_helpers.sh` hold the shared connection/credential plumbing (device IP,
  SSH invocation via `envsshpass`, password handling) that every other `*_remote.sh`
  script sources first.
  *Why split into `_remote`/`_local` pairs:* the `_remote` script runs on the developer's
  machine and only handles getting bits onto the device and invoking SSH; the `_local`
  script is what actually executes on the SteamOS device itself, keeping the
  "what runs where" boundary explicit in the filenames rather than branching on hostname
  inside one script.

### Remote test laptop (`scripts/remote-test.sh`)

A second real-hardware rig, distinct from the SteamOS device tooling above: a personal
CachyOS laptop (Intel Kaby Lake-U / HD Graphics 620, `mo@192.0.2.167`, hostname
`mo-laptop`), reachable over SSH with key auth, running a live Hyprland session on
seat0/tty1. *Why it exists:* every overlay bug this project has hit took multiple failed
rounds because agents could only test headlessly on the maintainer's own working
machine — no visible overlay, no real force-grab as a launch flag, no soak testing, and
twice a test window landed on the maintainer's own display by accident. This laptop is a
real, dedicated Wayland session nobody else is using, reachable non-interactively.

*Why build-here-ship-there, not build-on-the-laptop:* the laptop's CPU is a low-power
U-series part; a full LTO release build there would take a long time and make the
machine unusable while it ran. The desktop already compiles at GCC's default `-march`
(generic x86-64 baseline — nothing in `meson.build` or `scripts/build-gamescope-ritz.sh`
sets `-march=native` or anything else; `/etc/makepkg.conf`'s `-march=native` only affects
`makepkg`, not a plain `meson`/`ninja` invocation), so the binary this produces already
runs on any x86_64 CPU including the laptop's — verified via `readelf -n` reporting `x86
ISA used: x86-64-baseline`. So the desktop builds once (fast, on real hardware the
maintainer games on anyway) and `scripts/remote-test.sh sync` ships the compiled binary
over rather than repeating the build on weaker hardware.

- `scripts/remote-test.sh sync [--no-build]` — builds locally
  (`nice -n 19`, release) and rsyncs `build-release/src/gamescope` to
  `~/gamescope-ritz-remote/gamescope-ritz` on the laptop, then runs `--version` there to
  confirm the transferred binary actually starts (catches both an ISA mismatch and
  missing shared libraries immediately, rather than mid-investigation later). There is
  nothing else to sync: the Lua scripts, the shader effects and the licence texts are all
  compiled into the binary, which is why the old `--extras` flag is gone. `gamescopectl`
  is not synced — it's
  a separate binary owned by the `gamescope-git` pacman package, already present on the
  laptop the same way it is on the desktop.
- `scripts/remote-test.sh run [--wait] -- <command...>` — runs a command on the laptop
  with `WAYLAND_DISPLAY`/`XDG_RUNTIME_DIR`/`HYPRLAND_INSTANCE_SIGNATURE` exported (read
  live from the running Hyprland process's own `/proc/<pid>/environ` on every call, since
  every `ssh host 'cmd'` is a fresh non-interactive shell that inherits none of the
  target session's environment) and the remote bin dir on `PATH`. Without `--wait` the
  command is launched via `setsid nohup ... & disown` so it survives the SSH session
  ending — the second SSH fact this script exists to hide: a long-running remote process
  dies with its SSH session unless explicitly detached.
- `scripts/remote-test.sh screenshot <remote-path> [local-path]` — runs `gamescopectl
  screenshot "<remote-path> 4"` (type `4` = `screen_buffer`; the default type truncates
  `zpos >= 2` and the overlay sits at `zpos 6`, and gamescopectl silently collapses
  trailing args if the path and type aren't one quoted argument — a quirk that has
  independently cost six prior investigations, so the script always does this for you)
  and `scp`s the result back.
- `scripts/remote-test.sh env` — prints the resolved `export` lines for a caller who
  wants to `ssh` in by hand instead.

**Known gap (2026-09-02):** the laptop is missing `wlroots0.20` (`libwlroots-0.20.so`),
so a synced binary currently fails its `--version` self-check with "shared libraries:
libwlroots-0.20.so: cannot open shared object file". Fix on the laptop: `sudo pacman -S
wlroots0.20` (present in the standard `extra` repo, not just the desktop's
`cachyos-extra-v3`). The Vulkan stack, by contrast, is already fine unverified-but-fine:
`vulkaninfo` itself isn't installed, but both `/usr/share/vulkan/icd.d/intel_icd.json`
(the modern `anv` driver, `libvulkan_intel.so`) and `intel_hasvk_icd.json` are present
with their driver libraries in place, and `vulkan-intel`/`vulkan-icd-loader` are
installed — nothing to install there.

## Unit tests (`tests/gamescope_tests`)

`tests/meson.build`'s `gamescope_tests` is a single Catch2 binary covering every unit test
in `tests/`, run per-tag via `meson test` or directly as `./build-release/tests/gamescope_tests
[tag]` (no args runs everything). 2026-09-15: **the binary isolates `XDG_CONFIG_HOME` itself**
— `tests/test_global_isolation.cpp` registers a Catch2 event listener
(`CATCH_REGISTER_LISTENER`) that points `XDG_CONFIG_HOME` at a fresh run-wide temp directory
before the first test case and removes it after the last, re-asserting it before every test
case in between so a fixture's teardown can never leave a later test case pointed at the real
config home. This exists because `tests/test_keybinds.cpp`'s held-action and mouse-chord test
cases called `SetChord()`/`ResetAll()` (which persist via `Keybinds.cpp`'s `PersistLocked()`)
with no config-home override anywhere in that file, so a full run of the suite quietly
rewrote the developer's real `~/.config/gamescope-ritz/global.json` twice — found with
`inotifywait -m ~/.config/gamescope-ritz` around a run of the binary. The per-file
`TempConfigHome` fixtures (`test_config.cpp`, `test_resolution.cpp`,
`test_overlay_profiles.cpp`, `test_effects_curve.cpp`) still work unmodified on top of this
listener, but their own destructors unconditionally `unsetenv()` rather than restoring the
prior value, so they do not nest as cleanly on their own as their comments assume — the
listener's per-test-case re-assertion is what actually closes that gap for any test file,
present or future, that forgets its own isolation.

**Ad-hoc scripts must isolate `XDG_CONFIG_HOME` themselves** — the binary's own isolation
covers only `gamescope_tests`. `scripts/effects-regression.sh` and `scripts/pixel-regression.sh`
already do this (each launches gamescope with its own `CONFIGHOME` exported as
`XDG_CONFIG_HOME`); any new script or tool that runs gamescope or links `Config/ConfigManager.cpp`
needs the same. `tests/steam_friends_live_probe` (built but never registered as a `meson test`,
per its own header) does not link `ConfigManager.cpp` and touches no config path, so it needs
no such isolation.

## Using it

Configure with `meson setup build -D<option>=<value>` for any flag above, then
`ninja -C build`. To iterate against a real SteamOS device, run
`tools/build_and_install_on_steamos_device_remote.sh <device_ip> [password]` from a dev
machine; to fall back to the stock system build, run
`tools/reset_to_system_gamescope_remote.sh`. To iterate against the remote test laptop
instead, run `scripts/remote-test.sh sync` then `scripts/remote-test.sh run -- ...`.

## Related links

- [input-emulation](input-emulation.md) — gated by the `input_emulation` feature flag.
- [screen-capture-pipewire](screen-capture-pipewire.md) — gated by the `pipewire` feature flag.
- [backend-drm](backend-drm.md) — gated by the `drm_backend` feature flag.
- [backend-sdl](backend-sdl.md) — gated by the `sdl2_backend` feature flag.
- [backend-openvr](backend-openvr.md) — gated by `enable_openvr_support`.
