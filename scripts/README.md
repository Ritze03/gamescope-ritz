# Gamescope Script/Config Files

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
