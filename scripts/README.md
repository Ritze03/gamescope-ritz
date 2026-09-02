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
