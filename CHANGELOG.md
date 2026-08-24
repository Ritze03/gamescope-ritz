# Changelog

All notable user-facing changes, newest first. Categories: **Added** (new
features), **Fixed** (bug and behaviour fixes), **Removed** (things taken
out), **Info** (notes worth knowing). Each date is also that build's
gamescope-ritz patch version.

## 2026-08-24

### Added
- **Changelog area**: a new rail area showing this file, the base gamescope
  commit, and the gamescope-ritz patch date.
- **Your system cursor in the overlay**: nested mode now uses your desktop's
  own cursor. The built-in arrow stays as the fallback for embedded, VR and
  pointer-grabbed modes, so there is still never zero cursor.
- **Log line numbers and timestamps**: selecting a line describes it in the
  Inspector, and gamescope and game output are interleaved in true arrival
  order rather than one stream after the other.
- **Log filters moved into the Inspector**: sources, severity, text filter,
  auto-scroll and capture diagnostics live there now, so the sheet is the
  log body at full height.

### Fixed
- **Text is bigger throughout the overlay**, most of all the Log and
  Changelog body.
- **Overlong text ends in `...`** instead of stopping mid-word.
- **The sheet scrolls**: the scrollbar used to move while the content stayed
  put.
- **Escape closes the overlay** instead of unwinding one level per press.
- **FSR sharpness ran backwards**: 0% selected maximum sharpening and 100%
  the minimum. Nothing on disk changed, but the stock setting now reads 90%
  rather than 10%.
- **Sliders land on round numbers**, each with a step suited to its own
  range, and at most about 100 positions per drag.
- **UI scale applies when you let go**, not mid-drag, so the track no longer
  slides out from under the pointer.
- **Sharpness is one setting again**, and resets to 0% when you change the
  upscaling filter.
- **Notification placement uses the 3x3 anchor grid** the System Monitor
  uses, instead of a nine-item dropdown.
- **The rebuilt UI is clickable**: switches, sliders, segmented controls,
  steppers, chips and every Inspector row were dead to the mouse.
- **The overlay opens on Right Ctrl** (tap) and the command palette on
  Left+Right Ctrl.

### Removed
- **The `settings` label in the title bar**, which restated the window's own
  purpose. The rule under the bar now follows your accent colour.

### Info
- **New General area**: VRR, Allow Tearing and Force Grab Cursor moved above
  Upscaling. Config keys and entry ids are unchanged.
- **New console commands** `overlay_e2_pointer` and `overlay_e2_get` drive
  and read the overlay from a script without reaching any game window.
- **The overlay rebuild lives on `feature/overlay-e2`** until it merges;
  entries from 2026-08-23 on describe that branch where the two differ.

## 2026-08-23

### Added
- **A rebuilt settings overlay**: one fixed slab (rail, sheet, inspector)
  replaces the floating panels and dock. Every setting is declared once and
  drawn, hit-tested and saved from that one declaration.
- **Command palette** (`Ctrl+K`): search all 102 settings and parameters
  from anywhere in the overlay.
- **System Monitor sizing**: each module shrinks to its own content, all
  modules render at the width of the widest enabled one, and a long media
  track title is truncated instead of stretching the whole stack.
- **System Monitor toggles**: separate switches for the monitor itself, for
  FPS, for the FPS label, and for the numeric frametime readout.

### Fixed
- **Force Grab Cursor did nothing**: the toggle rendered correctly but never
  reached the backend until a restart.
- **A stray pointer event could steal focus**: an out-of-range coordinate
  mid-drag made a press on one panel's title bar move a different panel.
- **Panels shrank while being dragged** toward the right or bottom edge.
- **Panel resizing** is restricted to the bottom-right grip, with a hard
  250x250 floor and equal dock margins above and below.
- **Audio stream names** resolve the way `pactl` does, instead of showing
  raw identifiers like `bluez_input.98_0D_AF...`.
- **`Ctrl+/` was unreachable** from a real keyboard, and dropdowns never
  opened at all.
- **Disabled controls** painted at full brightness.

### Removed
- **The floating-panel UI and its dock**: five windows, all drag, resize,
  z-order and tiling, and nine custom widgets. There is no flag to fall back
  to.
- **The `overlay_e2` flag** that gated the rebuild while it was built.

### Info
- **System Monitor settings are six tabs** (General plus one per module)
  instead of one long scroll.
- **Checkboxes are switches** everywhere; no site in this UI has a genuine
  multi-select list.
- **HDR has its own tab**, and the frame limiter is 0 or 10-480 rather than
  a plain 0-480 continuum.

## 2026-08-22

### Added
- **The settings overlay**: Display, Shaders, Audio, Config and System
  Monitor panels, composited by gamescope itself.
- **Layered configuration**: global and per-game settings with atomic
  writes, plus a per-game override UI.
- **Audio volume control**: per-stream PipeWire volume, matching sandboxed
  and Proton games by process tree.
- **System Monitor**: FPS, CPU, GPU and media modules on a 3x3 placement
  grid, with a 60-second statistics tab. A module says "GPU unavailable"
  rather than inventing a number.
- **Vibrancy and Pre-Sharpen**: a combined ReShade effect, toggled live.
- **Accent colour picker**: one hue drives all ten accent tokens.
- **Background blur and darkening**, both on the compositor's own colour
  path so they work together.
- **UI scale**: a real 0.5x-2.0x range, with the font atlas rebuilt at the
  effective scale for crisp text at any zoom.
- **Real keyboard-layout text input** via xkbcommon, replacing a hardcoded
  US-QWERTY table.

### Fixed
- **Fullscreen VRR flicker**: both overlay textures were written and sampled
  on different Vulkan queue families with no ownership transfer. This
  reproduces on upstream gamescope; it is not a fork regression.
- **The frame limiter did nothing**: it displayed a value that was silently
  zeroed again every frame.
- **ReShade recompiled its pipeline every frame** under FSR and NIS: 3,216
  recompiles in 12 seconds, now 2.
- **UI scale was applied twice**, squaring itself once a non-default scale
  had been saved.
- **Per-game config is no longer deleted** when you toggle the override off;
  deletion sits behind a confirmed button.
- **The General tab reverted to stale defaults** whenever another panel
  saved.
- **Double-click timing** depended on the framerate.
- **`Ctrl+Shift+O` toggled the overlay twice** per press.
- **Orphaned `.tmp` config files** are swept on load, once the writing
  process is confirmed dead.

### Info
- **Data directories are namespaced** to `share/gamescope-ritz`, so this
  fork cannot overwrite a co-installed packaged gamescope.
- **Build tooling**: one `build-gamescope-ritz.sh` entry point and an
  interactive installer/updater.

## 2026-08-21

### Info
- **The fork starts here**, with the planning commit that added `superdoc/`,
  the project plan and the initial backlog.
- **The base is upstream `ValveSoftware/gamescope` `fcc1341`.** Upstream's
  own history is not reproduced in this file.
