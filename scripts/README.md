# Gamescope Script/Config Files

## Building gamescope-ritz

`build-gamescope-ritz.sh` is the single entry point for building — one command
instead of remembering the meson invocation or rediscovering this repo's
submodule/test quirks. `install-gamescope-ritz.sh` and
`update-gamescope-ritz.sh` (below) call the same shared build helpers in
`gamescope-ritz-common.sh`, so all three stay in sync. That shared helper
(`gcr_build`) also runs the actual `ninja` invocation under `nice -n 10` (plus
`ionice -c3`, idle I/O class, since LTO linking is I/O-heavy too) — niced
once there rather than at each call site, so it covers every build path
(including `remote-test.sh`'s local build) without needing to be repeated.
Niceness is inherited by every compiler/linker process ninja spawns, so a
build never contends with the user's games or desktop.

```sh
scripts/build-gamescope-ritz.sh              # release -> build-release/ (default)
scripts/build-gamescope-ritz.sh --debug       # debug   -> build/
scripts/build-gamescope-ritz.sh --test        # release build, then `meson test` (63/63)
scripts/build-gamescope-ritz.sh --clean --jobs 8
```

Options: `--release` (default), `--debug`, `--test`, `--clean`, `--jobs N`,
`-h`/`--help` (full details in the script's header comment).

- **Two build trees, on purpose.** `build-release/`
  (`--buildtype=release -Doptimization=3 -Db_lto=true`, ~5.4MB binary) and
  `build/` (`--buildtype=debug`, ~45MB binary) never overwrite each other.
  This project once lost real time chasing a VRR bug that only reproduced on
  an unoptimised `-O0` binary — which tree you're running should always be
  obvious, so the script always prints buildtype, build dir, and a warning
  banner on debug builds.
- **Submodules.** If `src/reshade`, `subprojects/wlroots`,
  `libdisplay-info`, `libliftoff`, or `SPIRV-Headers` aren't checked out yet,
  the script detects it and runs `git submodule update --init --recursive`
  before configuring, instead of letting meson fail with a confusing
  "Include dir reshade/source does not exist".
- **Tests.** `-Denable_tests=false` (the meson default override some briefs
  ask for) and passing tests are contradictory — you can't run a suite that's
  disabled. `--test` does the sane thing: configures with
  `-Denable_tests=true`, builds the test binary, and runs `meson test`.
- **Never runs as root** — a root-owned build directory would silently break
  a developer's normal non-root `meson compile`/`ninja` workflow afterwards.
- A build directory that can't be reconfigured cleanly (incompatible cached
  options) is wiped and reconfigured automatically; `--clean` forces this
  up front.

## Testing on the remote laptop rig

`remote-test.sh` builds locally, ships the binary to a dedicated CachyOS test laptop
over SSH, and runs it there against a real compositor — so overlay work can be verified
visually instead of only headlessly. See its own header comment for full usage
(`sync`, `run`, `screenshot`, `env` subcommands) and
`superdoc/features/build-and-tooling.md` for why it's built-here-ship-there rather than
building on the laptop.

```sh
scripts/remote-test.sh sync                      # build here, rsync the binary over
scripts/remote-test.sh run -- gamescope-ritz -w 1920 -h 1080 --backend wayland -- vkgears
scripts/remote-test.sh screenshot ~/shot.png
```

## Pixel regression: does the Inverted HUD still invert the pixel under it

`pixel-regression.sh` is a one-command headless pixel regression gate for the FPS
HUD/crosshair colour behaviour — inversion, the Fixed-mode colour, both outlines, and
the crosshair's geometry. It exists because "does the digit actually invert" has
regressed twice unnoticed (see `superdoc/meta/TERMINOLOGY.md`'s history and the
`grade-screenshots-not-checklists` memory note) and each time an agent re-did the
capture-and-sample dance by hand on the laptop. This is that dance, automated, with
**no visible window** on the desktop and no laptop round trip.

```sh
scripts/pixel-regression.sh                # run every check (~45s)
scripts/pixel-regression.sh --only outline # run one check by name
scripts/pixel-regression.sh --keep         # leave the last instance running for
                                            # manual `gamescopectl` poking; prints
                                            # the XDG_RUNTIME_DIR/socket to reach it
```

**How it sees pixels without touching the real desktop:** a private, invisible sway
(`WLR_BACKENDS=headless`, its own `XDG_RUNTIME_DIR`, no input devices — nothing any
real compositor can display) hosts a real nested `gamescope --backend wayland`, and
`gamescopectl screenshot "<path> 4"` against *that* instance captures the composited
HUD and crosshair. `gamescope`'s own `--backend headless` was tried first and rejected:
measured on this rig it captures **no** extra composited layer at all (see
`superdoc/features/cursor-pipeline.md`'s "Verified by direct X11 query" section) — a
property of this sandbox's headless Vulkan path, not of the feature under test. The
test client is `kitty` with matching `-o background=X -o foreground=X -o cursor=X`,
which paints a perfectly flat colour (xterm is the recipe `superdoc/features/
fps-display.md` documents, but it isn't installed on this desktop).

**Driving state:** the initial config (`fps_display.*`, `crosshair.*`) is a JSON file
the script writes into an isolated `XDG_CONFIG_HOME` — never the user's real
`~/.config/gamescope-ritz`. Everything that changes while an instance is running goes
through `overlay_e2_set <id> <value>` (the same binding a mouse click writes through)
and `fps_display_force <n>` (pins the HUD's displayed digit so a screenshot never races
real frame timing) — both ConCommands over `gamescopectl`, never OS input.

**Adding a check:** every threshold lives as a named constant at the top of the script
(background colours, tolerances, sample-box geometry) — tune those, never the sampler.
A check is a `check_*` bash function that calls `take_screenshot`, then one or more
`run_sampler <subcommand> ...` calls into `pixel_regression_sample.py` (subcommands:
`pixel`, `digit` — finds a glyph's fill colour in a box and asserts it, `blackcount` —
counts near-black pixels in a box, `line` — samples a ray of offsets from a centre
point, `line_blend` — as `line`, with the target computed from a colour and opacity
blended over the background through the HUD layer's premultiplied-then-coverage
blend). Register it in the `should_run`-gated dispatch near the bottom so `--only`
can select it, and give it a header comment naming the exact assertion (mirroring the
ones already there).

**Reading a failure:** `results.txt` under the run's own
`build-release/verify-shots/pixel-regression/<timestamp>/` directory has one line per
check — `STATUS  name  detail` — plus the PNG each check sampled. A `FAIL` line's
`detail` names the measured colour/pixel and what was expected; open the matching PNG
next to it to see why (crop and zoom with PIL rather than trusting the numbers alone —
see the `grade-screenshots-not-checklists` memory note). A `SKIP` line means `--only`
excluded that check, not that it failed. Exit code 2 (rather than 1) means a setup
problem — the binary is missing, sway/gamescopectl aren't found, or an instance never
came up — not a verdict on the feature; the log line above it says which.

Uses `python3`'s `PIL` (already installed on this machine; no new dependency was
added — the script fails loudly with a pointer back here if it's ever missing).

## Pointer regression: a locked pointer never gets an absolute event, and every movement carries relative motion

`pointer-regression.sh` is the same headless recipe as the pixel gate (private invisible
sway, nested `gamescope --backend wayland`, `gamescopectl` ConCommands, no OS input) for the
input rules behind the two 2026-09-06 CS2 fixes: **a game holding a pointer lock must never
receive `wl_pointer.motion`**, the absolute-pointer re-sync after a mapping change must fire
exactly once per real change while unlocked, and **every movement must carry
`zwp_relative_pointer_v1` motion** so Xwayland's master pointer never changes device under a
game (the one that actually fixed CS2 -- SDL3 caches that device once, in the menu). Non-zero
exit gates a commit.

```sh
scripts/pointer-regression.sh                     # every check (~30s)
scripts/pointer-regression.sh --only locked-absolute
scripts/pointer-regression.sh --keep              # leave the last instance running
scripts/pointer-regression.sh --gamescope <bin>   # another binary, e.g. a pre-fix one
```

**The clients** are `build-release/tests/pointer_lock_client` (`tests/pointer_lock_client.c`,
built with `--test`): a real SDL2 window inside gamescope's Xwayland that, with `--lock`,
calls `SDL_SetRelativeMouseMode(SDL_TRUE)` -- what a first-person game does in play -- and
prints one `MOTION` line per `SDL_MOUSEMOTION` it receives (Xwayland republishes every
pointer event as XI2 raw motion and SDL reads raw valuators as deltas, so an absolute event
that reaches a locked client shows up here as a position-sized jump); and
`build-release/tests/pointer_grab_client_x11` (`tests/pointer_grab_client_x11.c`, needs SDL3):
the CS2-shaped one, native SDL3 with `--lock-after S` so it is a menu first, and an XI2 tap
that prints every raw event with its device (`RAW dev= src=`) and the master pointer's axis
mode as SDL3 cached it (`DEVICE ... axis0=rel|abs`). **The host mouse** for the `x11-host`
check is `build-release/tests/virtual_pointer_tool` (`tests/virtual_pointer_tool.c`), a
`zwlr_virtual_pointer_v1` client that plugs a pointer into the private sway, which otherwise
has no input devices, and stays alive for the run (sway drops the seat's pointer with the
last virtual one).

**What is driven:** `wlserver_debug_mouse_motion` (relative), `wlserver_debug_absolute_motion`
(an absolute host sample), `overlay_e2_set display.filter.scaler 4|0` (Stretch/Auto: a
mapping change with no resize), `steamcompmgr_debug_set_nested_mode "<w> <h> 0"` (a runtime
resolution change), `virtual_pointer_tool` for host absolute samples and host relative motion,
and `wlserver_pointer_stats` for the compositor-side counters
(`motions`, `motions_locked`, `relatives`, `resyncs`, `warps_suppressed_locked`) plus the
constraint state. A binary without `wlserver_pointer_stats` gets its compositor-side assertions
SKIPped and is judged on the client counts alone (how the pre-fix baseline was measured).

**Checks:** `locked-relative` (relative motion still reaches the locked client -- proves the
lock is real), `locked-absolute` (6 absolute samples: client +0, `motions_locked` 0),
`locked-mapping` (2 scaler toggles + 2 mode changes: `resyncs` +0, client +0, and
`warps_suppressed_locked` advanced, or the mapping never moved and the check is void),
`unlocked-resync` (one sample -> client +1; 2 s idle -> +0; Auto->Stretch -> exactly +1; the
same value again -> +0; a 5:4 nested mode -> +1), `x11-menu-lock` (CS2's order with the SDL3
client: 4 absolute samples to the menu, then relative mode, then 10 relative injections ->
the cached master axis is `rel`, all 10 arrive as `xrel=5.0`, `relatives >= motions`),
`x11-locked-abs` (the SDL3 client locked: 6 absolute samples -> +0), `x11-host` (the same
order through the nested backend's real host path with the virtual mouse, force-grab off:
menu samples arrive, the client locks, the host confirms and 10 host steps arrive as
`xrel=5.0`; then force-grab on: 5 more). The nested mode and the sample point are
deliberately non-degenerate -- a same-aspect mode under Auto does not move the mapping and
the output centre maps to the window centre under every scaler; both are pinned in
`tests/test_pointer_mapping.cpp`. Logs and `results.txt` land in
`build-release/verify-shots/pointer-regression/<timestamp>/`. Spec and measurements:
`superdoc/features/cursor-pipeline.md`, "Locked pointer => never an absolute event" and "The
device SDL3 remembers".

## Installing and updating gamescope-ritz

`install-gamescope-ritz.sh` and `update-gamescope-ritz.sh` (plus the
`gamescope-ritz-common.sh` helper library they share) install this fork to
**`/usr/bin/gamescope-ritz`** — never `/usr/bin/gamescope`, which both
scripts hard-refuse to touch, since that's the user's packaged, known-good
gamescope. See the header comment in each script for full option lists
(`--help` also prints it).

```sh
scripts/install-gamescope-ritz.sh      # interactive: symlink vs copy, builds
                                        # a release binary first if none exists
scripts/update-gamescope-ritz.sh       # git pull --ff-only, rebuild release,
                                        # refresh the install (copy mode only —
                                        # a symlink install is live immediately)
scripts/install-gamescope-ritz.sh --uninstall
```

Both build into a separate `build-release/` directory (`--buildtype=release
-Doptimization=3 -Db_lto=true`), leaving a developer's `build/` untouched.
Writing to `/usr/bin` uses `sudo` only for that one step, and the build
itself never runs as root.

These are unrelated to the `.lua` scripting system documented below — this
directory doubles as the home for both this fork's dev/ops scripts and the
Lua config scripts gamescope loads at runtime.

## ⚠️ Health Warning ⚠️

Gamescope scripting/configuration is currently experimental and subject to change massively.

Scripts and configs working between revisions is not guaranteed to work, it should at least not crash... probably.

## The Basics

Gamescope uses Lua for it's configuration and scripting system.

Scripts ending in `.lua` are executed recursively in alphabetical order from the following directories:
 - `/usr/share/gamescope-ritz`
 - `/etc/gamescope-ritz`
 - `$XDG_CONFIG_DIR/gamescope-ritz`

...and, as a fallback so an existing plain-gamescope script setup keeps working, also from the unnamespaced `/usr/share/gamescope`, `/etc/gamescope` and `$XDG_CONFIG_DIR/gamescope`.

You can develop easily without overriding your installation by setting `script_use_local_scripts` which will eliminate all of the above from being read, and instead read from `../config` of where Gamescope is run instead of those.

When errors are encountered, it will simply output that to the terminal. There is no visual indicator of this currently.

Things should mostly fail-safe, unless you actually made an egregious mistake in your config like setting the refresh rate to 0 or the colorimetry to all 0, 0 or something.

# Making modifications as a user

If you wish to make modifications that will persist as a user, simply make a new `.lua` file in `$XDG_CONFIG_DIR/gamescope-ritz` which is usually `$HOME/.config/gamescope-ritz` with what you want to change (`$HOME/.config/gamescope` also still works, as a fallback).

For example, to make the Steam Deck LCD use spec colorimetry instead of the measured colorimetry you could create the following file `~/.config/gamescope-ritz/my_deck_lcd_colorimetry.lua` with the following contents:

```lua
local steamdeck_lcd_colorimetry_spec = {
    r = { x = 0.602, y = 0.355 },
    g = { x = 0.340, y = 0.574 },
    b = { x = 0.164, y = 0.121 },
    w = { x = 0.3070, y = 0.3220 }
}

gamescope.config.known_displays.steamdeck_lcd.colorimetry = steamdeck_lcd_colorimetry_spec
```

and it would override that.

You could also place this in `/etc/gamescope-ritz` if you really want it to apply to all users/system-wide, but that would need root privelages.

# Features

Being able to set known displays (`gamescope.config.known_displays`)

The ability to set convars.

Hooks

# Examples

A script that will enable composite debug and force composition on and off every 60 frames.

```lua
my_counter = 0

gamescope.convars.composite_debug.value = 3

gamescope.hook("OnPostPaint", function()
    my_counter = my_counter + 1

    if my_counter > 60 then
        gamescope.convars.composite_force.value = not gamescope.convars.composite_force.value
        my_counter = 0
        warn("Changed composite_force to "..tostring(gamescope.convars.composite_force.value)..".")
    end
end)
```

# Hot Reloading?

Coming soon...
