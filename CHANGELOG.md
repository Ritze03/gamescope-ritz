# Changelog

All notable user-facing changes, newest first. Categories: 

**Added** (new features), 
**Fixed** (bug and behaviour fixes), 
**Removed** (things taken out), 
**Info** (notes worth knowing). 

The newest version below is the one this build reports.

## [0.3.5] – 2026-08-27

### Added
- **Accent hue now tints text and the main surface**: your chosen hue reaches
  the base text colour and the panel surface, alongside the accent controls
  that already followed it.
- **Darker inspector panel**: the right-hand inspector now reads as a dark,
  subtly tinted surface matching the left sidebar, instead of a light wash.
- **Wider notification scale range**: the notification size slider now spans 0.5x to
  2x, matching the UI scale slider's range.
- **New pointer while the mouse is grabbed**: a simple triangle outlined in your
  accent colour with a black inlay, so it stays readable over both dark and bright
  game content. It follows the accent-hue slider, and it looks the same whether or
  not the settings overlay is open. There is no setting for it.
- **New Cursor tab**: a Setup area for designing the overlay's own pointer — size,
  outline thickness, and outline/inlay colour, with an option to lock the outline
  to a fixed colour instead of following the accent hue. The controls are in place
  and save normally, but don't change the pointer's on-screen look yet.

### Fixed
- **Mouse movement reaches games again while the cursor is grabbed**: movement was
  not being delivered at all, so aiming and looking only updated when you clicked.
- **UI scale no longer resizes the overlay window**: adjusting UI scale now
  scales only the overlay's contents; the window itself stays a fixed
  size.
- **Notification text matches the rest of the overlay**: it used to render
  smaller than every other label.
- **Sliders no longer show `-0`** as a value distinct from `0`.
- **The toggle-switch knob is centred**: it used to sit 1px left of centre,
  and land differently on and off.
- **The slider handle's glow follows its rounded shape** instead of drawing
  as a hard square box over it.
- **Slider handle reaches the ends**: the handle now sits flush with the
  track's outer edges at minimum and maximum instead of stopping 2px short
  of each.
- **The font atlas rebuilds correctly after startup at the default 1.0x UI
  scale**: it used to keep the provisional pre-init bake instead of
  replacing it, matching every other scale.
- **Sharper overlay text**: overlay text is no longer softened and speckled
  at 1.0x UI scale — every text size now renders at its true pixel size
  instead of a resampled one.
- **Left Ctrl + Right Ctrl now closes the launcher again**, not only opens
  it.
- **The left sidebar's right border is 1px**, not a doubled 2px.
- **The close X no longer disappears**: it shows even when no tab is
  selected.
- **Selecting a Facts, Meter or Graph row keeps the Inspector on
  Configure** instead of switching it to Details.
- **Row values sit next to their control again**: switch, stepper and
  placement-grid rows used to strand the value in the middle of the row;
  it now sits one gutter to the left of the control, like sliders already
  did.

### Removed
- **The Shell settings tab and its Inspector/Layout options**: close the
  overlay with the X instead.
- **The startup toast's second line advertising the launcher bind.**
- **The bottom navigation-key legend**, replaced by a single
  `L Ctrl + R Ctrl — Launcher` hint.
- **Read-only rows** (status readouts, meters, graphs) no longer appear in
  launcher search.

### Info
- **The overlay's open/close fade is twice as fast.**
- **Three setting IDs moved to match the settings tree**:
  `display.sharpness` → `display.filter.sharpness`, `display.scaler` →
  `display.filter.scaler`, `audio.volume` → `audio.stream.volume`. These
  are not stored in your saved config, so nothing you saved breaks.
- **Every option's explanation was rewritten in plain language**, across
  Display, Shaders, Audio, Config and Monitor.

## [0.3.1] – 2026-08-24

### Added
- **Changelog area**: a new rail area showing this file with its headings,
  categories and entries laid out, alongside the three things that identify
  a build — the gamescope-ritz version, the upstream gamescope commit it is
  built on, and the date it was built from.
- **A version number**: gamescope-ritz now reports a semantic version, taken
  from the newest entry below, so the number in the overlay and the number
  in this file cannot disagree.
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
- **A notification's coloured edge sits inside the card**: it used to stand
  beside the card's rounded corners with a doubled seam, most visibly at
  larger notification sizes.
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
- **The launcher's search bar stays put** while you type. It is centred once,
  for a full list, and results now grow and shrink below it instead of
  sliding it up and down the screen.
- **Closing the launcher no longer flashes the full settings UI**: pressing
  Escape returns straight to the game instead of showing the whole overlay
  on the way out.

### Removed
- **Dock scale**: the dock it sized no longer exists, so the setting did
  nothing. Existing configs still load; the unused key is dropped the next
  time settings are saved.
- **The `settings` label in the title bar**, which restated the window's own
  purpose. The rule under the bar now follows your accent colour.

### Info
- **Notification scale moved** out of UI scale and into the Notifications
  group, beside toast placement and mute. Its saved value is unchanged.
- **New General area**: VRR, Allow Tearing and Force Grab Cursor moved above
  Upscaling. Config keys and entry ids are unchanged.
- **New console commands** `overlay_e2_pointer` and `overlay_e2_get` drive
  and read the overlay from a script without reaching any game window.
- **The overlay rebuild lives on `feature/overlay-e2`** until it merges;
  entries from 2026-08-23 on describe that branch where the two differ.

## [0.3.0] – 2026-08-23

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

## [0.2.0] – 2026-08-22

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

## [0.1.0] – 2026-08-21

### Info
- **The fork starts here**, with the planning commit that added `superdoc/`,
  the project plan and the initial backlog.
- **The base is upstream `ValveSoftware/gamescope` `fcc1341`.** Upstream's
  own history is not reproduced in this file.
