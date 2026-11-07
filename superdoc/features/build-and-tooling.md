# Build & Tooling — Meson build, feature flags, compiled-in set

The Meson build definition, its feature-flag surface, what is compiled into the binary and
the FrameGen submodule. **Agents build and test only with `scripts/gentle-build.sh` — see
[gentle-build.md](gentle-build.md)** (build dir `build-agent/`, never an LTO `build-release/`
build). Split out on 2026-11-07, each with its own page: [gentle-build.md](gentle-build.md),
[unit-tests.md](unit-tests.md), [steamos-device-tooling.md](steamos-device-tooling.md) (the
`tools/` scripts that build, deploy and reset Gamescope on a SteamOS device over SSH) and
[remote-test-rig.md](remote-test-rig.md) (`scripts/remote-test.sh`, the remote test laptop).

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
    on a Fedora-based distro where *every* Vulkan client — vkcube and Rocket League alike —
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

**Learned motion build (2026-10-26, branch `ml-fg-testing` only).** That branch pins the
submodule at FGR `4bae33a` (round 2: `6133a4a`, round 1: `fd5c163`), a commit that exists **only in the local
frame-gen-ritz repo** (its local `ml-fg-testing` branch), not on GitHub: a fresh clone must
first run `git -C subprojects/FrameGen fetch <path to the frame-gen-ritz checkout>
ml-fg-testing`, then `git submodule update --init subprojects/FrameGen`. The build adds the
ten `ml_*.comp` shaders (round 4 added `ml_planes` and `ml_feat`; `ml_dup`, `ml_guard` and `ml_planes` also as `rgb10` / `f16` variants) and a
`framegen_ml_weights` `custom_target()` that runs the library's `gpu/ml/embed_weights.py`
(via `python3 -I`, paths as plain strings because of the sandbox) over **all ten nets in
`LearnedNet` order** (`e2v3, e1v3, e2v4, e1v4, e2v5, e1v5, g2v2, g2v3, m2v2, m2v3`) to produce `ml_weights.h` in the build dir.
The `ml_*_spv.h` headers (`ml_conv`, `ml_zero`, `ml_guard`) and `ml_weights.h` must all exist
or the library, which probes them with `__has_include`, **compiles without error and silently
refuses Learned** (`kMlNotBuilt`; the host then runs V1 and the Status line says why): after
touching the list, check that `build-agent/src/` holds `ml_zero_spv.h`, `ml_guard_spv.h`,
`ml_guard_rgb10_spv.h`, `ml_guard_f16_spv.h` and an `ml_weights.h` with ten nets (`kMlWeights_e2v3` ... `kMlWeights_m2v3`), plus `ml_planes_spv.h`, `ml_feat_spv.h`, `ml_planes_rgb10_spv.h`, `ml_planes_f16_spv.h`. See
`frame-generation.md`'s "Learned motion".

Meson's sandbox forbids handing files under `subprojects/` to the parent project
(`Sandbox violation: Tried to grab file ... from a nested subproject`), and FrameGen has
no root `meson.build`, so `subproject()` is impossible. Instead its 20 shaders (the
interpolation's 11, UI protection's six, `ui_*.comp`, which share the include
`ui_common.glsl`, and motion blur's three, `blur_lookup`/`blur_error`/`blur_resolve`, which
share `blur_common.glsl`; the depfile re-runs glslang when either changes; the test-only
`gpu/blendbench.comp` is not built) are compiled with `custom_target()` (same glslang flags and `--vn <name>_spv` embedded headers
as gamescope's own) using absolute paths (since PR #6 the library can also build per-format shader variants, `<name>_<fmt>_spv`, which `framegen.cpp` includes behind `__has_include`; gamescope builds the **`rgb10` and `f16`** variants -- the 10 colour shaders each, `-DFG_FORMAT_RGB10=1` / `-DFG_FORMAT_F16=1`, `src/meson.build`'s `framegen_colour_shaders` loop -- and none for NV12 / P010; it also compiles its own `shaders/cs_fg_copy.comp` twice more with `-DDST_FORMAT=rgb10_a2` / `rgba16f` into `cs_fg_copy_rgb10.h` / `cs_fg_copy_f16.h`. `Why` per-format variants and not one shader: the storage format is a compile-time qualifier of the image declaration. See [frame-generation](frame-generation-renderer.md#hdr-and-10-bit-games)),, and `framegen.cpp` is compiled through the
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

## Using it

Configure with `meson setup build -D<option>=<value>` for any flag above, then
`ninja -C build`. To iterate against a real SteamOS device, run
`tools/build_and_install_on_steamos_device_remote.sh <device_ip> [password]` from a dev
machine; to fall back to the stock system build, run
`tools/reset_to_system_gamescope_remote.sh` ([steamos-device-tooling.md](steamos-device-tooling.md)).
To iterate against the remote test laptop instead, run `scripts/remote-test.sh sync` then
`scripts/remote-test.sh run -- ...` ([remote-test-rig.md](remote-test-rig.md)). Agents' own
test builds: [gentle-build.md](gentle-build.md); unit tests: [unit-tests.md](unit-tests.md).

## Related links

- [input-emulation](input-emulation.md) — gated by the `input_emulation` feature flag.
- [screen-capture-pipewire](screen-capture-pipewire.md) — gated by the `pipewire` feature flag.
- [backend-drm](backend-drm.md) — gated by the `drm_backend` feature flag.
- [backend-sdl](backend-sdl.md) — gated by the `sdl2_backend` feature flag.
- [backend-openvr](backend-openvr.md) — gated by `enable_openvr_support`.

## History

- **WSI layer (FROG)** (glossary detail): **WSI layer (FROG)** — `VkLayer_FROG_gamescope_wsi`, the Vulkan layer games load so their swapchain is redirected into gamescope's compositing pipeline. **Since 2026-09-22 this fork builds and installs its OWN**, `VK_LAYER_RITZ_gamescope_wsi` (`libVkLayer_RITZ_gamescope_wsi_<family>.so`, enable var `ENABLE_GAMESCOPE_RITZ_WSI`), rather than relying on the distro `gamescope` package's copy. `Why:` the layer is loaded into the **game's** process and is one half of a protocol whose other half is the compositor, so the two must be version-matched — a distro layer predating upstream `6a4d150` (2024-12-04, which added a 7th argument to `swapchain_feedback`) sends a short message, the compositor drops the client as a protocol error, and every Vulkan client on the machine starts and instantly quits with "Failed to get Wayland objects". A distinct layer *name* (not just a distinct file) is what lets ours coexist with the distro's instead of shadowing it, since the Vulkan loader resolves a duplicate name by search order. `steamcompmgr.cpp` sets only our enable var when our manifest is present and falls back to upstream's `ENABLE_GAMESCOPE_WSI` when it is not; `GAMESCOPE_RITZ_USE_SYSTEM_WSI=1` forces the fallback.
