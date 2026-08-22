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
- `default_extras_install.sh` is a Meson install-time hook (invoked with `MESON_SOURCE_ROOT`/
  `MESON_INSTALL_PREFIX`/`DESTDIR` env vars set by Meson) that replaces any previously
  installed `scripts`/`looks`/`reshade`/`fonts` directories under `share/gamescope-ritz`
  (namespaced, not plain `share/gamescope` — this fork installs alongside a packaged
  `/usr/bin/gamescope` and must never touch its data dir; the script's own `rm -rf` calls
  are guarded to refuse any path outside `share/gamescope-ritz`) with the ones from the
  source tree, so a reinstall doesn't leave stale default assets behind. The `fonts/`
  entry (added by issue #53) installs only `LICENSE-OFL.txt` — the Geist Sans/Mono TTFs
  themselves are compiled directly into the binary (`src/Overlay/fonts/embed_font.py`),
  not copied at install time, but the OFL requires its license to travel with any
  redistribution of the font, and a compositor binary with the glyphs baked into its atlas
  counts, so the license is still installed to `share/gamescope-ritz/fonts/` alongside
  everything else.

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

## Using it

Configure with `meson setup build -D<option>=<value>` for any flag above, then
`ninja -C build`. To iterate against a real SteamOS device, run
`tools/build_and_install_on_steamos_device_remote.sh <device_ip> [password]` from a dev
machine; to fall back to the stock system build, run
`tools/reset_to_system_gamescope_remote.sh`.

## Related links

- [input-emulation](input-emulation.md) — gated by the `input_emulation` feature flag.
- [screen-capture-pipewire](screen-capture-pipewire.md) — gated by the `pipewire` feature flag.
- [backend-drm](backend-drm.md) — gated by the `drm_backend` feature flag.
- [backend-sdl](backend-sdl.md) — gated by the `sdl2_backend` feature flag.
- [backend-openvr](backend-openvr.md) — gated by `enable_openvr_support`.
