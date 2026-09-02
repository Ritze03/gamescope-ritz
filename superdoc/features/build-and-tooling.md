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

- `scripts/remote-test.sh sync [--extras] [--no-build]` — builds locally
  (`nice -n 19`, release) and rsyncs `build-release/src/gamescope` to
  `~/gamescope-ritz-remote/gamescope-ritz` on the laptop, then runs `--version` there to
  confirm the transferred binary actually starts (catches both an ISA mismatch and
  missing shared libraries immediately, rather than mid-investigation later). `--extras`
  also syncs `scripts/`/`looks/`/`reshade/` for Lua-config or ReShade-effect testing;
  omitted by default since gamescope fails safe (not a crash) when those directories
  don't exist and most smoke tests don't need them. `gamescopectl` is not synced — it's
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
