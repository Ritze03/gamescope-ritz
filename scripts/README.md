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
