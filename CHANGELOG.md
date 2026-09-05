# Changelog

All notable user-facing changes, newest first. Categories: 

**Added** (new features), 
**Fixed** (bug and behaviour fixes), 
**Removed** (things taken out), 
**Info** (notes worth knowing). 

The newest version below is the one this build reports.

## [0.6.0] – 2026-09-05

### Added
- **Nested resolution and refresh rate persist**: the game resolution and refresh rate
  chosen in Display > Resolution are remembered and reapplied on the next launch,
  unless an explicit `-w`/`-h`/`-r` flag overrides them.
- **Auto-save to profile**: an opt-in switch in Profiles that writes every setting you
  change straight into the active profile; off by default, so a profile only changes
  when you press Save.
- **Save changes to profile**: writes your current settings into the active profile,
  and the Status row now shows how many sections have changed since you applied it.
- **Rename and Delete profile**: manage the selected profile from the Profiles area;
  Delete asks for a second press, and never touches the settings you are running.
- **Clipboard sync is remembered**: the System tab's switch now survives a restart and
  can differ per game like every other setting.
- **Runtime resolution**: a new Display > Resolution area changes the resolution the
  game renders at while it runs, with presets from 720p to 4K, a custom size with an
  aspect lock, and no restart of gamescope or the game.
- **Runtime refresh rate**: the same area sets the refresh the game is paced at (60 to
  240 Hz or a custom value), or follows the host screen again.
- **Window size request**: the Resolution area can ask the desktop to resize the
  gamescope window; a tiling desktop may refuse, and the area shows the size it
  granted.
- **Smoothing update mode glides**: the HUD takes a reading once a second, glides to it
  over 0.3 s and holds still for 0.7 s, instead of easing continuously.
- **System tab**: a new System settings tab with a clipboard sync switch and a status
  row naming the protocol syncing with the host.
- **Profiles status**: the Profiles area now opens with what is in use -- which
  profile your settings came from, where edits are saved, and that every change is
  saved to disk immediately.
- **Restore previous settings**: using a profile keeps your previous settings as a
  backup, and a Restore button brings them back until you use another profile.
- **Per-game status**: the Per-game area now opens with the recognised game, whether
  it runs on its own settings, which profile they came from, and whether a saved
  file exists.
- **Start from profile**: one press in the Per-game area switches a game to its own
  settings and copies a chosen profile into them.
- **Crosshair**: a new Crosshair area draws a crosshair over the middle of the game
  -- four lines, an optional centre dot and an optional outline, each with its own
  colour and opacity -- so it stays sharp even when frame generation smears the
  game's own.
- **Crosshair 1px mode**: a line width of 1 is exactly one pixel wide with no soft
  edges, and the dot is a crisp square at every size.
- **Crosshair auto-hide**: optionally hides while the right mouse button is held,
  with Fade out, Focus or Shrink animations over a chosen time, and comes back the
  instant you let go.
- **Crosshair Apply scaling**: optionally stretches the crosshair exactly as the game
  is stretched, so a 4:3 game on a 16:9 screen gets the wide crosshair "stretch"
  players expect.
- **Adaptive Brightness**: the effect works again -- dark scenes brighten and
  bright scenes dim toward the target brightness over the chosen seconds, and it
  keeps tracking the scene while switched off so turning it back on is instant.

### Fixed
- **Clipboard sync setting takes effect from launch**: turning clipboard sync off now
  stays off from the moment gamescope starts, not only after the settings overlay
  has been opened once.
- **Using a profile asks before discarding unsaved changes**: only when your settings
  differ from the active profile and auto-save is off; otherwise it is one press.
- **Fewer disk writes while dragging a slider**: settings writes are now coalesced, so
  a drag costs one write per pause instead of one per tick.
- **FPS counter no longer shows 999**: the HUD now counts the game's frames instead of
  timing them, so a game that hands over two frames at once (frame generation, or
  simply out-running the compositor) reads correctly rather than pinning at 999.
- **FPS counter shows four- and five-digit rates**: the 999 ceiling is gone, and the
  box only grows wider when the number actually needs more digits.
- **First toast no longer stutters**: the notification system's one-time setup and
  font baking now happen once at launch, before the startup splash, instead of on
  the first notification shown mid-game; the toast also no longer re-creates its
  offscreen texture or compiles a composite shader on its first frame after the
  window was resized at start-up.
- **New profiles appear immediately**: a freshly saved profile shows up in the picker
  at once, pre-selected and confirmed by a toast, instead of after a restart.
- **Profile picker with no profiles**: the picker is present and explains itself
  before the first profile exists, instead of being missing until a restart.
- **Adaptive Brightness re-enable ramp**: re-enabling Adaptive Brightness as the
  only active Shaders effect on a changed scene no longer ramps over several
  seconds; it adapts instantly.
- **Adaptive Brightness defaults**: the Shaders panel's "reset to default" values now
  match the compiled-in ones (1.0 s brighten, 1.0 s darken, min gain 0.5, max gain
  2.0) instead of the stale 1.5 s / 2.5 s / 0.8 / 1.6.
- **Shader effects always work**: Vibrancy, Shadow Control, Pre-Sharpen and Adaptive
  Brightness are now built into gamescope-ritz itself instead of loaded from a shader
  file on disk, so an old or missing file can no longer make a control do nothing.
- **Screenshots match the screen**: a `gamescopectl screenshot` (types 3 and 4) now
  applies the Shaders-area effects exactly once, the same as the presented frame.
- **Saved effects apply at launch**: effects saved in your config are on from the
  first frame instead of the first time the Shaders area is opened.

### Info
- **Plainer profile wording**: "Apply profile" is now "Use this profile" and says it
  replaces your current settings; "Override global config" is now "Use separate
  settings for this game"; "Save current settings" is now "Save as new profile".
- **Pre-Sharpen uses RCAS**: the pre-scale sharpen now uses the same FSR RCAS filter
  as the Upscaling sharpness, which sharpens without ringing on hard edges; slider
  values are unchanged.
- **Shader effect names**: Pre-Sharpen, Adaptive Brightness and Shadow Control
  (formerly "Shadow lift") are now capitalised consistently; saved settings are
  unaffected.

### Removed
- **"Update every second" HUD mode**: folded into Smoothing, which now is a
  once-a-second reading; a saved "Update every second" setting loads as Smoothing.
- **Bundled `reshade/` shader folder**: no longer installed or shipped; your own
  ReShade `.fx` files via `--reshade-effect` keep working.

## [0.5.0] – 2026-09-04

### Added
- **Override game cursor**: a new opt-in Cursor setting that also replaces a
  cursor the game itself draws, not just one it never set. It still leaves a
  game's own crosshair alone while the pointer is locked, since there is no
  cursor layer to replace then.
- **Clipboard sync**: copy and paste now cross between games, gamescope's own
  windows and — when running nested — the rest of your desktop, updating the
  moment the clipboard changes rather than waiting for a window switch.
- **Shadow lift**: a new Shaders effect that brightens dark areas in dark games
  while leaving bright areas alone.

### Info
- **RGB colour pickers**: the cursor's outline and inlay colour pickers now use
  R/G/B sliders (0-255) instead of hue/saturation/lightness, which is easier to
  read for new users. The accent colour slider is unchanged.

### Fixed
- **Vibrancy is now a true multiplier**: the slider runs 0x (greyscale) to 3x
  (max boost) with 1x as unchanged, instead of an additive boost. An existing
  config's untouched value converts automatically, so it still opens looking the
  same rather than desaturated.
- **Copy to clipboard in the Log panel**: the button works now, putting the
  visible log lines on the real clipboard instead of nowhere.
- **Copying between games**: text copied in one game is now pastable in another
  when more than one Xwayland server is running.
- **Colour picker rail labels**: the cursor colour rails now show an R/G/B letter
  next to each strip instead of three unlabelled gradients.
- **Sub-pixel FPS outline**: outline sizes below 1px draw a faint outline again
  instead of nothing.
- **HUD outline alignment**: the FPS display's black outline no longer reads
  slightly up and to the left of the digits at larger font and outline sizes.

## [0.4.0] – 2026-09-03

### Added
- **HUD placement editor**: HUD settings now has an "Edit placement" button that opens
  a drag editor right over your game. Move the FPS, CPU, GPU, and Now Playing modules
  wherever you want them; each one snaps to the screen's edges and centre, and to the
  other modules, within a few pixels. Save keeps the new placement, Esc cancels.
- **HUD update mode**: choose Smoothing (eased, the default), Update every second
  (digits hold still), or Immediate (the latest frame, no easing).
- **HUD "Hide above X"**: the HUD can now hide itself while your frame rate stays
  comfortably above a threshold you set, and reappear once it drops.
- **HUD backdrop opacity**: the HUD's backdrop is now a single opacity slider, with 0
  turning it off entirely; it never rounds its corners.
- **HUD text colour modes**: Fixed keeps your accent colour and briefly inverts it
  during a lag spike; Inverted picks light or dark text to stay readable against its
  backdrop, which is also gentler on OLED screens.
- **Inverted HUD text now truly inverts the game underneath it**: Inverted mode no
  longer just picks light or dark text from the backdrop; it now inverts the actual
  game colours showing through each digit, with a safeguard so the text stays
  visible even over a plain mid-grey background.
- **HUD text shadow**: a small drop shadow behind the number, with a strength slider
  that turns it off at 0.
- **HUD lag-spike reaction**: a noticeably slow frame now briefly inverts the HUD's
  text colour (or tints its backdrop in Inverted mode) so a stutter is actually
  visible, not just felt.
- **HUD text outline**: a black outline around the number, sized from 0 to 4 pixels, so
  the readout stays legible over any background; 0 turns it off.
- **HUD lag spike detection switch**: the whole lag-spike reaction can now be turned
  off, leaving the number's colour and backdrop alone.

### Fixed
- **The HUD's module toggles work again**: Frame rate, Frametime readout, Frametime
  graph, Percentile row, CPU load, GPU load, Now playing, and the FPS module's "FPS"
  label switch now actually turn that part of the HUD on or off, instead of silently
  doing nothing.
- **HUD placement editor: modules in the top-left are grabbable again**: every module
  starts out placed there, and the editor's own Save/Cancel bar used to sit right on
  top of them, blocking the drag before it could start. The bar now sits bottom-centre
  and out of the way.
- **HUD placement editor: the live numbers no longer print through Save/Cancel**: while
  the editor is open, the running FPS/CPU/GPU readout now stays behind its own chrome
  instead of drawing over the buttons.
- **HUD placement editor: modules are now visible, readable targets**: each module is
  drawn as a proper outlined box with its name at the overlay's own text size and a
  minimum size big enough to see and aim at, instead of a few unreadable pixels in the
  top-left corner.
- **HUD placement editor: overlapping modules can each be picked up**: modules sitting
  in the same spot are now fanned out slightly and the smaller one wins a click, so a
  module hidden under a bigger one is no longer unreachable; the fan-out is only how
  they are drawn and never changes the placement that gets saved.
- **HUD placement editor: the game stays visible while you place things**: the
  background blur is switched off and the dimming is kept light for the duration of an
  edit, so you can see what you are placing the HUD over.
- **HUD placement editor: Save and Cancel are readable**: the editor's buttons and hint
  text now use the overlay's normal text size and scale with the overlay's display
  scale, instead of rendering at roughly ten pixels.
- **HUD placement editor: the screen's centre and margins are shown**: a faint centre
  cross and edge-margin frame are now always drawn, so it is clear where a module is
  being placed even before a snap guide appears.
- **The HUD shows something again on a fresh setup**: a config that has never had a HUD
  layout picked now shows the stock readout (frame rate, CPU, GPU, Now Playing) instead
  of a blank overlay, matching how the HUD looked before the layout editor existed.
- **FPS number is centred in its backdrop again**: a two-digit reading used to sit
  shoved against the right edge with an empty gutter on the left; the digits are now
  centred within the readout's stable-width box.
- **The FPS readout and toast notifications no longer draw over the settings
  overlay**: both now sit beneath the settings panel/Shell as intended, instead of
  covering it.
- **Inverted HUD text no longer inverts everything around the digits**: the outline
  now stays black and the backdrop draws in its normal colour, and turning backdrop
  opacity down to 0 really does remove the backdrop, even during a lag spike.
- **Inverted HUD text inverts the game again**: with a backdrop or an outline switched
  on it had stopped tracking what was behind it and always came out plain white.

### Removed
- **HUD performance modules**: CPU load, GPU load, the frametime readout, the frametime
  graph, the 1%/0.1% percentile row, and Now Playing are gone. The HUD is now a single
  FPS number, drawn cleanly over your game.
- **HUD placement editor**: the drag-and-drop layout editor is gone. HUD position is now
  set with a 9-point anchor picker plus horizontal and vertical margin sliders.
- **Named HUD layouts**: the HUD no longer supports separate saved layouts (such as
  "Simple" or "Advanced"). Placement is a single anchor and pair of margins instead.
- **HUD "FPS" unit label**: the readout is now a bare number, with no trailing "FPS"
  text next to it.
- **HUD text shadow**: the drop shadow behind the number is gone, replaced by the
  black outline above; an existing shadow setting is not carried over.

## [0.3.5] – 2026-08-27

### Added
- **Accent hue now tints text and the main surface**: your chosen hue reaches
  the base text colour and the panel surface, alongside the accent controls
  that already followed it.
- **Darker inspector panel**: the right-hand inspector now reads as a dark,
  subtly tinted surface matching the left sidebar, instead of a light wash.
- **Wider notification scale range**: the notification size slider now spans 0.5x to
  2x, matching the UI scale slider's range.
- **New pointer in the settings overlay**: a simple triangle outlined in your accent
  colour with a black inlay, so it stays readable over both dark and bright game
  content. It follows the accent-hue slider. Outside the overlay your game's own
  cursor is left alone.
- **New Cursor tab**: a Setup area for designing the overlay's own pointer — size,
  outline thickness, and outline/inlay colour, with an option to lock the outline
  to a fixed colour instead of following the accent hue. Every control now changes
  the pointer's on-screen look immediately.
- **"Use everywhere" cursor option**: off by default, so nothing changes unless you
  opt in. Turn it on and the overlay's pointer also becomes your game's default
  cursor, overlay open or closed — a game that sets its own cursor still shows
  that, this only replaces what's shown in its absence.
- **The Cursor tab has its own rail icon**: a small drawing of the pointer itself,
  instead of falling back to a plain "C".

### Fixed
- **Overlay text is no longer doubled**: every letter after the first in a label
  used to carry a faint ghost of itself about a pixel to the side, because glyph
  spacing was not aligned to whole pixels; text across the whole overlay is now
  crisp.
- **Ctrl+Shift+O opens the settings overlay again**: with the right-hand Shift it
  did nothing at all, because Left Ctrl + Right Shift already opened the launcher
  the moment Shift went down and the O then closed it again.
- **Left Ctrl + Right Shift no longer risks opening the overlay too**: both
  overlay hotkeys now act when Right Shift comes back up, so one press of the
  combo can only ever produce one surface.
- **Freeze after moving the mouse with Force Grab Cursor on**: with the cursor
  grabbed, sustained mouse movement steadily pinned the game's display buffers until
  the game stopped responding; they are now handed back correctly however long you
  play.
- **Right Ctrl opens the overlay again, without doubled text**: a modifier that got
  stuck as "held" — after switching windows, or by any other route — made every later
  lone Right Ctrl tap silently open the launcher instead of the settings overlay, and
  with both drawing at once every bit of on-screen text appeared doubled.
- **"Use everywhere" now shows the custom cursor with Force Grab Cursor off and
  the overlay open**: it previously fell back to the default pointer in that one
  combination while working correctly everywhere else.
- **Doubled mouse sensitivity while the cursor is grabbed is gone**: games that read
  raw mouse input were being sent every movement twice over, so look and aim were
  about twice as fast as they should be.
- **The overlay no longer freezes when the game stops drawing**: it now asks for its
  own frames, so moving the mouse, pressing a key and the open/close fade all show
  up immediately instead of waiting for the game to render something.
- **The FPS HUD keeps updating when the game goes idle**: it used to stall along
  with the game since it never asked for its own frames either; it now refreshes on
  its own roughly twice a second, so the readout doesn't freeze in a paused menu
  or a stalled loading screen.
- **A dropdown no longer opens under the wrong control**: in the narrow layout, a
  multiselect that had downgraded to a dropdown (e.g. Upscaling's Filter) could open
  its option list under the Inspector's copy of the same row instead of the one you
  clicked, showing whichever row's options happened to draw last. The Sheet and the
  Inspector now each own their own open/closed state for the same row.
- **Clicking an open dropdown's own control now closes it**: it previously reopened
  the same list instead of toggling it shut.
- **An open dropdown list now always closes on the next click**: clicking its own
  row, another row, empty space in the sheet, the inspector, the rail or the title
  bar all close it now; previously only some of those did, and the row that opened
  it was immune to every route.
- **Closing the Launcher with Left Ctrl + Right Ctrl keeps your search text**: reopen
  it the same way and what you typed is still there. Closing with Right Ctrl alone
  or Escape still clears it, as before.
- **Mouse movement reaches games again while the cursor is grabbed**: movement was
  not being delivered at all, so aiming and looking only updated when you clicked.
- **UI scale no longer resizes the overlay window**: adjusting UI scale now
  scales only the overlay's contents; the window itself stays a fixed
  size.
- **Notification text matches the rest of the overlay**: it used to render
  smaller than every other label.
- **Sliders no longer show `-0`** as a value distinct from `0`.
- **Stepping a float setting down and back up returns it exactly to where it
  started**: repeated arrow-key adjustment (in the launcher and the sheet
  alike) used to leave a tiny leftover instead of landing back on the exact
  original value.
- **A near-zero value never displays in scientific notation** (e.g.
  `-1.565e-07`): it now reads as a plain `0`.
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
- **The settings overlay now scales cleanly with your resolution**: its window
  is always 85% of the available width and height, instead of being capped at
  a fixed pixel size that read as too small on high-resolution displays and
  too large on low-resolution ones.
- **The pointer now clicks exactly at its drawn tip**: clicks used to land
  slightly inside the visible point, inset by roughly half the outline
  width, because the click point was set at the shape's underlying corner
  rather than the outline's actual outer edge.
- **Multi-line rows (Toast placement, Accent colour, and every other
  placement/colour/graph row) are now clickable across their whole
  height**: only the top line used to accept a click, leaving most of the
  row's visible area dead; the row's own grid cells and swatches still get
  first claim on a click, same as before.

### Removed
- **The Shell settings tab and its Inspector/Layout options**: close the
  overlay with the X instead.
- **The startup toast's second line advertising the launcher bind.**
- **The bottom navigation-key legend**, replaced by a single
  `L Ctrl + R Ctrl — Launcher` hint.
- **Read-only rows** (status readouts, meters, graphs) no longer appear in
  launcher search.
- **The HUD's old shared placement, margin, and module-spacing settings**:
  replaced by independent per-module positioning (see the Info note below).

### Info
- **The overlay's hotkeys moved off Ctrl**: the settings overlay now opens with
  **Right Shift** (tap) instead of Right Ctrl, and the launcher with
  **Left Ctrl + Right Shift** instead of Left Ctrl + Right Ctrl. Right Ctrl no
  longer does anything on its own.
- **Grabbed-cursor mouse speed is an upstream issue, not a fork change**: the
  attempted fix for doubled mouse speed under Force Grab Cursor has been taken back
  out, because the mouse code here is identical to stock gamescope's and the same
  doubling happens there; grabbing the cursor is simply something stock gamescope
  cannot switch on while it is running.
- **The overlay's open/close fade is twice as fast.**
- **Three setting IDs moved to match the settings tree**:
  `display.sharpness` → `display.filter.sharpness`, `display.scaler` →
  `display.filter.scaler`, `audio.volume` → `audio.stream.volume`. These
  are not stored in your saved config, so nothing you saved breaks.
- **Every option's explanation was rewritten in plain language**, across
  Display, Shaders, Audio, Config and Monitor.
- **The default cursor size is now 0.8x**, down from 1.0x. If you've already set
  your own size it is unaffected.
- **The performance HUD goes blank until you set up a new layout**: its
  modules (Frame Rate, CPU, GPU, Now Playing) now position independently
  instead of stacking together at one shared corner, so any placement you
  had is gone; there's no in-app way to set a new one yet, but a layout
  editor for it is coming.
- **The HUD's settings tab is renamed "HUD"**, from "Monitor," to avoid
  confusion with this project's own "Overlay" naming elsewhere.

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
