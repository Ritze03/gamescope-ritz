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
- **Installing is `install.sh` (ritz-get spec v1), nothing else** (2026-10-24). The old
  interactive installer, `scripts/install-gamescope-ritz.sh` and
  `scripts/update-gamescope-ritz.sh` are gone; `scripts/gamescope-ritz-common.sh` stays
  because `gentle-build.sh` and `build-gamescope-ritz.sh` source it. `install.sh --install
  --yes` is what `ritz-get` runs on every install and update, and it never runs git
  itself: `ritz-get` has already checked out the clone and its submodules.
  - **Build.** `meson setup build/meson` (release, `-Doptimization=3 -Db_lto=true`,
    `-Denable_tests=false`), then `ninja` for `src/gamescope` and the WSI layer. The binary
    is copied to `build/gamescope-ritz` under a temporary name and `mv`ed into place, so a
    failed build leaves the old one working. A second run reconfigures (`meson setup
    --reconfigure`, which re-reads git for the embedded version, see below) and ninja only
    rebuilds what changed. *Why a copy and not the ninja output:* ninja relinks the output
    in place, and a build that fails half-way must not touch the binary a game is using.
    If the clone moved, `build/.srcdir` no longer matches and `build/meson` is rebuilt from
    scratch, because a meson tree holds absolute paths.
  - **Links.** `~/.local/bin/gamescope-ritz` and `/usr/local/bin/gamescope-ritz` (through
    `~/.local/share/ritz-get/links/gamescope-ritz`) point at `build/gamescope-ritz`. The
    system link is the one root step (first install only, `ROOT_LINK=yes` in the template).
    Nothing is ever written to `/usr/bin`. A `/usr/bin/gamescope-ritz` left by the old
    installer is not touched; `install.sh` prints a note that it may be deleted.
  - **Dependencies.** Every tool and pkg-config module is checked up front and reported as
    `missing: TOOL (try: pacman:PKG dnf:PKG apt:PKG)` with exit 3. The list is hand-kept in
    `install.sh`'s `check_tools`; keep it in step with the `dependency()` calls in
    `meson.build`, `src/meson.build`, `protocol/meson.build` and `layer/meson.build`.
    wlroots is OK when a system 0.20 or 0.19 exists or `subprojects/wlroots` is checked out
    (then the libraries that build needs are checked instead); OpenVR likewise, with
    `cmake` for the submodule build. *Why hand-kept now:* the scraper of the old installer
    needed bash and a distro-specific hint format, and the spec wants package names for
    three package managers. The optional libraries (DRM, SDL2, libcap, PipeWire, libeis,
    libavif) only produce a `note:` when absent.
  - **setcap is not done.** `CAP_SYS_NICE` would be lost on every rebuild, so `install.sh`
    only prints the command.
- **This fork installs its OWN Vulkan WSI layer, under its own name** (2026-09-22).
  `layer/meson.build` builds `libVkLayer_RITZ_gamescope_wsi_<family>.so` with the layer
  name `VK_LAYER_RITZ_gamescope_wsi_<family>`, activated by
  `ENABLE_GAMESCOPE_RITZ_WSI` and suppressed by `DISABLE_GAMESCOPE_RITZ_WSI`. `install.sh`
  links it, user level and without root, into `$XDG_DATA_HOME/vulkan/implicit_layer.d/`
  (the `.so` and a manifest generated into `build/` whose `library_path` is absolute into
  the clone), and `--remove` takes the links away again. `install.sh` passes
  `--prefix=$HOME/.local -Ddatadir=$XDG_DATA_HOME` to meson so that the manifest path baked
  into the binary (`GAMESCOPE_RITZ_WSI_LAYER_JSON`) is exactly the link it makes.
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
- **The embedded version can go stale, and the scripts are what prevent it.**
  `src/meson.build` derives `k_szRitzCommit` / `k_szRitzPatchDate` with `run_command()`
  feeding a `configure_file()`; both run at *configure* time, and meson re-runs configure
  only when a `meson.build` changes — git HEAD moving is invisible to it. Every script
  here, and `install.sh`, always run `meson setup --reconfigure`, so a scripted build
  always re-reads git. A bare
  `ninja -C <dir>` in a hand-made build directory does **not**, and silently produces a
  binary reporting an old commit. `gcr_report_version()` prints the embedded version
  after every `gentle-build.sh` build and warns when it disagrees with HEAD, which turns
  that silent case into a visible one. *Why not fix it in meson:* `vcs_tag()` /
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
  so were the old `install.sh`'s `--extras`/`--no-extras` flags and its prompt. **Why:** that
  prompt defaulted to *no*, while `meson.build` baked the same path in as `SCRIPT_DIR`
  and `Script.cpp` loaded it at startup — so taking the default silently produced an
  install with no known-displays database at all, reported only as a `warnf` in a log
  nobody reads. A step whose default answer breaks the product should not exist.

### FrameGen submodule (2026-10-04)

The frame-generation library is a git submodule, `subprojects/FrameGen`
(github.com/Ritze03/frame-gen-ritz, MIT), pinned at FrameGen main `db569d0` (UI protection, motion blur (PR #2), the pacing planner (PR #3), the lag-spike buffer (PR #4), video timestamps (PR #5), video/HDR formats (PR #6) and the pacing fixes (PR #7: idle real-rate ticks set `Decision::skip`, depth/buffer held until the interval is known; gamescope already honours `d.skip`, no code change); gamescope uses PR #4 and, since 2026-10-04, the Rgb10 and RgbaF16 formats of PR #6, not NV12 / P010 or `OnFrame`) **A fresh checkout or worktree
needs `git submodule update --init subprojects/FrameGen`** before it will build.

Meson's sandbox forbids handing files under `subprojects/` to the parent project
(`Sandbox violation: Tried to grab file ... from a nested subproject`), and FrameGen has
no root `meson.build`, so `subproject()` is impossible. Instead its 20 shaders (the
interpolation's 11, UI protection's six, `ui_*.comp`, which share the include
`ui_common.glsl`, and motion blur's three, `blur_lookup`/`blur_error`/`blur_resolve`, which
share `blur_common.glsl`; the depfile re-runs glslang when either changes; the test-only
`gpu/blendbench.comp` is not built) are compiled with `custom_target()` (same glslang flags and `--vn <name>_spv` embedded headers
as gamescope's own) using absolute paths (since PR #6 the library can also build per-format shader variants, `<name>_<fmt>_spv`, which `framegen.cpp` includes behind `__has_include`; gamescope builds the **`rgb10` and `f16`** variants -- the 10 colour shaders each, `-DFG_FORMAT_RGB10=1` / `-DFG_FORMAT_F16=1`, `src/meson.build`'s `framegen_colour_shaders` loop -- and none for NV12 / P010; it also compiles its own `shaders/cs_fg_copy.comp` twice more with `-DDST_FORMAT=rgb10_a2` / `rgba16f` into `cs_fg_copy_rgb10.h` / `cs_fg_copy_f16.h`. `Why` per-format variants and not one shader: the storage format is a compile-time qualifier of the image declaration. See [frame-generation](frame-generation.md#hdr-and-10-bit-games)),, and `framegen.cpp` is compiled through the
wrapper `src/FrameGen/FrameGenLib.cpp`, which `#include`s it. `Why:` bumping the submodule
still pulls upstream library work, with no copy to drift. `fgtest` is not built. The
pacer is the library's header-only `gpu/pacing.h` (included by `FrameGenHost.cpp` and
`steamcompmgr.cpp` by relative path, the sandbox rule again) and its tests live in the
library (`pacingtest`); gamescope no longer has pacing tests. See
[frame-generation](frame-generation.md).

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

## Gentle test builds for agents (`scripts/gentle-build.sh`)

Every agent working in this repo does its own throwaway "does it compile, do the tests
pass" build, each in its own worktree, and `build-gamescope-ritz.sh`'s defaults are
tuned for the opposite job — the one, optimal, LTO'd `build-release/` binary the user
actually launches games through. Running several of *those* full ~670-target builds at
once, on a desktop the user is actively gaming on, was reported as lagging the whole
machine. `scripts/gentle-build.sh` is the agent-facing answer: same full build, a
separate `build-agent/` dir, tuned to disturb the desktop as little as possible. **Use
it — not a raw `ninja`/`meson compile`, not `build-release/` — for every agent test
build**; see `CLAUDE.md`'s "Building (agents)" bullet.

**`Why:` the levers changed mid-implementation, and it matters which ones actually
work.** The first draft capped the build's memory (`systemd-run`'s `MemoryHigh`) on the
theory that a big compile evicts the user's RAM working set. The user corrected that
directly: *"I wasn't talking about RAM. I have enough RAM. That doesn't matter. It's the
CPU cache that is being hit hard, which is pretty slow on my 10900K."* That's a
different resource with different rules: this machine (i9-10900K) has 10 cores, SMT
**off** (`lscpu -e`: one thread per core — check yours, don't assume), and **one 20MB L3
cache shared by every core over the ring**, with no Intel CAT/resctrl to partition it
(verified: no `cat_l3` in `/proc/cpuinfo`, no `/sys/fs/resctrl` on this machine —
consumer Comet Lake doesn't support it). Memory limits do nothing for that, and neither
does CPU pinning — a compiler on core 7 still evicts the game's lines from the
one L3 every core shares. What actually helps a *shared, unpartitionable* L3:

1. **Fewer concurrent compilers, first and foremost.** Every `cc1plus` instance streams
   a multi-MB working set through that single 20MB L3; several running at once is what
   actually thrashes it. `gentle-build.sh` defaults to `-j1` and refuses `--jobs` above
   `2`.
2. **A hard CPU duty-cycle cap**, `systemd-run -p CPUQuota=` (`--cpu-quota`, default
   `50%`): even one compiler running flat-out competes with the game continuously.
   Throttling it to run only half the time gives the game uncontested stretches to
   refill its own working set — `SCHED_IDLE`/`nice` alone don't do this, since they only
   step aside when a core would otherwise sit idle, not cap how hard a process runs
   *while* it holds the core.
3. **Less total compiler work**: no LTO (`-Db_lto=false` — LTO's whole-program link is
   still this project's single worst spike, memory or cache), `-g0` (skip debug info),
   `-fuse-ld=lld` when a trial link with it succeeds (lower memory *and* a measurably
   faster link than bfd — less time spent hammering the cache). `-Doptimization` was
   measured, not guessed: timing a from-scratch, single-core compile of this project's
   largest translation unit (`src/rendervulkan.cpp`, 6.6k lines) put `-O1` at 4.83s
   against `-O2`'s 4.86s — noise, not a real win — so the script keeps `-O2`. The GCC
   `--param ggc-min-*` flags from the first (RAM-focused) draft are gone: they cost real
   compile time and were never shown to move L3 pressure (no `perf`, no `cat_l3` on this
   machine to even measure that), so they're dropped rather than kept on faith.
4. **Avoid full rebuilds altogether where possible.** The single biggest total-cache-time
   cost is several agent worktrees each doing a from-scratch ~670-target build.
   `gentle-build.sh` reuses one fixed, persistent `build-agent/` per worktree
   (`gcr_meson_configure` reconfigures an existing dir in place rather than wiping it),
   so a second run in the same worktree is incremental. It also auto-detects `ccache`
   (installed on this machine, `/usr/bin/ccache` 4.14) on `PATH` and wires it into
   `CC`/`CXX` for the configure when present, with two env vars set so the reuse works
   **across worktrees, not just within one**: `CCACHE_BASEDIR=<repo root>` and
   `CCACHE_NOHASHDIR=1`. *Why both are needed:* every agent worktree is a separate
   checkout at a different absolute path but with identical relative layout, and this
   project's own compile commands already pass relative include paths (`-Isrc
   -I../src ...`, confirmed via `compile_commands.json`) — `CCACHE_BASEDIR` strips each
   worktree's own absolute prefix before hashing so the same relative path from two
   worktrees lands on the same cache key, and `CCACHE_NOHASHDIR` stops the compilation's
   own working directory (which varies with the build dir, same as the worktree does)
   from being folded into that key regardless. `ccache`'s default 5G cache is raised to
   15G (`ccache --set-config=max_size=15G`, run idempotently by the script itself, no
   separate one-time step to remember) — the user has 63G free on this machine and asked
   for headroom for several worktrees' worth of objects; 15G is the middle of a
   reasonable 10-20G range without getting close to filling the disk. If a `build-agent/`
   predates ccache being installed, the script notices (a marker file records whether
   the last configure had it) and wipes+reconfigures once — meson only reads `CC`/`CXX`
   at a build dir's FIRST configure, and a plain `--reconfigure` does not re-read them,
   so without this check a pre-ccache build dir would silently never pick it up.

Measured on this machine (10-core i9-10900K, `-j1`, `--cpu-quota 50%`, pinned to cores
6-9 (history: the script pinned then), the user actively running CS2 through a separate gamescope-ritz instance the whole
time): a from-scratch (empty ccache) full build of all 679 ninja targets, including
`tests/gamescope_tests`, took **14m43s wall** (7m3s user, 19s sys — the gap between wall
and user time IS the CPUQuota throttle doing its job); `meson test` equivalent
(`gamescope_tests` run directly under the same gentle wrapper) passed **all 588 test
cases, 15,953,549 assertions**, in 3s. `ccache -s` after that cold run already showed
79/533 hits (14.8%) — several subprojects (`libliftoff`'s four example binaries) compile
the same shared source under identical flags, so even a "cold" run gets some reuse
within itself. The systemd scope's limits were confirmed live via `systemctl --user show
-p CPUQuotaPerSecUSec -p AllowedCPUs run-<id>.scope` while the build ran (`500ms` and
`6-9` respectively; historical, the script no longer sets `AllowedCPUs`), not just assumed from the flags passed.

A second full build into a **fresh** `build-agent2/` directory (simulating a second
agent worktree building the same commit, cache warm from the first run) took **18s
wall** for the same 679 targets plus the same full test pass — a ~49x speedup. Its own
533 cacheable compiler calls hit the cache **533/533 (100%)**; the cumulative `ccache -s`
after both runs read `Hits: 612/1066 (57.4%)`, `Misses: 454/1066` (the 454 misses are
entirely from the first, genuinely-cold run). Total on-disk cache size after both builds:
19MB (`du -sh ~/.cache/ccache`), nowhere near the 15G cap — this project's non-cacheable
steps (linking, shader/font/protocol generation) dominate a warm build's remaining time,
not the compiler cache's own size. This is the `CCACHE_BASEDIR`/`CCACHE_NOHASHDIR`
cross-worktree design working as intended: `build-agent2/` never shared a build directory
with the first run, only the object cache, which is exactly the "different worktree,
same commit" case this was built for.

```sh
scripts/gentle-build.sh                       # full gentle build -> build-agent/
scripts/gentle-build.sh --test                 # build, then run gamescope_tests gently
scripts/gentle-build.sh --jobs 2 --cpu-quota 70%
scripts/gentle-build.sh --target src/gamescope # just the binary
```

`Why:` (2026-10-07) the script no longer pins CPUs (`taskset`, cgroup `AllowedCPUs=` and
`GENTLE_CPUS` are gone): the user now enforces core affinity system-wide (*"No need to
manually pin cores anymore. I have it enforced system wide now."*), so a second pin in
the script was redundant. Pinning never helped the shared L3 anyway.

Full option/knob reference: `scripts/gentle-build.sh --help` (the script's own header
comment; kept in sync there rather than duplicated in full here) and `scripts/README.md`'s
own section on it.

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
