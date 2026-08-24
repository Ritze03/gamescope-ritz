# Changelog

Everything in this file is this fork's own work. `gamescope-ritz`'s base commit is exactly
upstream `ValveSoftware/gamescope` HEAD (`fcc1341`) — upstream's own history is not
reconstructed here; see [`superdoc/README.md`](superdoc/README.md) for the architecture that
history produced. The fork's own history begins **2026-08-21**, with the planning commit that
laid down `superdoc/` and the settings-overlay project plan.

Dated blocks follow [`superdoc/claude-instructions/documentation-version-policy.md`](superdoc/claude-instructions/documentation-version-policy.md):
`YYYY-MM-DD`, newest on top, one block per calendar day, no version numbers. That policy
governs a single date-ordered sequence; it does not say what to do when a whole feature
branch's work is not on `master` yet. Rather than invent a versioning convention to paper over
that, this file uses **two sections**, each internally newest-date-first: `master` (what
shipped) and `feature/overlay-e2` (an unmerged branch off `master`, whose own dates overlap and
extend past `master`'s). Read this as a documented reading of the policy for a two-thread
history, not a departure from it.

Issue numbers (`#N`) refer to this repository's GitHub issue tracker.

---

## feature/overlay-e2 — unmerged, not on master

This branch replaces the settings overlay's entire UI layer: the floating-panel-and-dock
architecture built out below becomes a single fixed slab (rail + sheet + inspector) driven by a
**registry** — every setting is declared once, in one place, and the shell draws, hit-tests and
persists it from that declaration. The stated goal is to make the "control renders but is wired
to nothing" class of bug (see `#25`, `#68` below, and more of the same on this branch)
structurally harder to write, by removing the second place a control's geometry could be
described.

**Status as of the tip (`bb5d39b`, 2026-08-24): unmerged, and known-incomplete.** The legacy
floating-panel UI was deleted outright in P5 (2026-08-23) — there is no flag to fall back to —
so this branch is the only place the settings overlay exists once you're on it. A dedicated
[conformance audit](superdoc/planning/redesign/round-2/e2-inspector-plus/CONFORMANCE-AUDIT.md)
measured the built shell against the approved mockup pixel-for-pixel and counted **24
divergences, 14 of them unexplained by any recorded decision** — concentrated in the Inspector
(specified content and actions never written), the shell chrome (title bar, footer, breadcrumb
still placeholder), and a handful of invented control kinds on the Monitor/Appearance sheet that
break the uniform row height. The pre-deletion
[shell test report](superdoc/planning/redesign/round-2/e2-inspector-plus/SHELL-TEST-REPORT.md)
separately found and fixed 20 defects before P5 shipped. None of this is exposed to a user who
hasn't checked out this branch, since master is unaffected.

### 2026-08-24

- **The sheet could not scroll at all — the scrollbar moved and the content stayed put.**
  Every body in this shell paints at an absolute `y` (that is what keeps the row grammar
  identical between sheet and Inspector), but ImGui scrolls a child by moving its *cursor*,
  not by translating the draw list. `DrawSheetBody` laid out from the region's fixed screen
  coordinate and told ImGui nothing about how tall the result was, so it was nailed to the
  screen; the rows' own hit-boxes pushed a scroll *range* into existence, which is why a
  thumb slid along a scrollbar that drove nothing. The Inspector had the same bug and had it
  fixed inline in P3b — the sheet, the largest scrolling region in the product, was simply
  never one of the places those four lines were written. The arithmetic is now one named,
  imgui-free `ScrollView` in `Layout.cpp` that all three scrolling bodies call, and it is
  unit-tested: a fix that has to be remembered per region is a fix that is missing from the
  next region.
- **Escape now closes the overlay.** It used to unwind one level per press (palette →
  drawer → inline expansion → overlay), so getting back to the game from a fresh open took
  three presses and each one silently rearranged the shell instead of leaving. Escape now
  dismisses only something genuinely *in front* of the shell — the command palette, an open
  dropdown, a text field mid-edit, or an armed destructive action — and closes the overlay
  from everywhere else. An armed action is disarmed unconditionally and first, so it can
  never survive an Escape and fire on a later press. The footer legend reads `Esc close`,
  and the explain page's back crumb now names `^/`, the key that actually returns. This
  matches the launcher, which already gave the game straight back on Escape.
- **The rebuilt UI was almost entirely unclickable, for one missing call.** `DrawEntryRow`'s
  full-width row selector had no `SetNextItemAllowOverlap()`. A comment in the code asserted
  ImGui resolves hover to the last item added; it does not — `ItemHoverable` rejects a later
  item while an earlier one still holds `HoveredId`/`ActiveId`. So the row selector won every
  hit test, and every control inside a row — switches, sliders, segmented controls, steppers,
  chip banks, the composite band, the anchor grid, the hue rail, every Inspector row — was dead
  to the mouse. Two more bugs compounded it: a "micro-pump" frame submitted no UI so any press
  it consumed was invisible to every widget, and pointer position was queued *after* the click,
  so ImGui applied the press at the stale position. Separately, ImGui's own keyboard navigation
  had been running uninterrupted since P1 because a per-frame enable in `AddLayer()` ran before
  the disable the shell issued after — so nav's own mouse-hover suppression had been fighting
  the pointer the whole time. Fixed together; the shell's own navigation model now runs alone.
- Added `overlay_e2_pointer` and `overlay_e2_get` console commands completing a **safe**
  synthetic-input story for this shell: both append to the overlay's own input queue, which
  nothing outside the overlay reads, so driving or reading back the UI from a script cannot
  reach a game window. (`ydotool`/real pointer injection remains off-limits per project policy.)
- The overlay now opens on **Right Ctrl** (tap, on release) and the command palette on
  **Left+Right Ctrl**, replacing the earlier hotkey — verified through wlserver's real hotkey
  path rather than the overlay's own queue, "which provably cannot test a hotkey", per the merge
  commit; that gap is exactly how a prior `KEY_SLASH` binding bug (2026-08-23) went undetected.
- Raised the type scale's small end at the user's request (Title 11→13, Section 10.5→12, Label
  14→15) — the same kind of ask as `#23` on master, where the origin sizes move rather than a
  multiplier being applied, so the departure from the mockup's own scale is recorded in `SPEC.md`.
- Moved VRR, Allow Tearing and Force Grab Cursor into a new **General** area above Upscaling, at
  the user's direct instruction, correcting where an earlier decision (D13.1) had filed them.
  Only the UI grouping changed — config keys and entry ids are untouched, and Force Grab Cursor
  was re-verified live rather than assumed, having once before been a control that rendered
  correctly while doing nothing (`#68`).

### 2026-08-23

- **P5: the legacy floating-panel UI is deleted and this shell becomes the settings overlay.**
  5,191 lines removed against 140 added in the deletion commit (5,520 against 703 across the
  whole P1–P5 phase) — the dock, five floating windows, all drag/resize/z-order/tiling, six
  panel bodies, two settings panels, and nine of the ten old custom widgets, all found dead by
  `-Wunused-function` once their call sites were cut. The `overlay_e2` ConVar that gated the
  branch is removed rather than kept as a no-op switch, on the reasoning that a flag with one
  reachable value isn't a flag. Before deleting, a handful of bugs were fixed so nothing lost its
  fallback: the rail didn't scroll (so areas were unreachable at 2.0×), a colour param's blue
  channel could be nudged by an unrelated arrow-key binding, and a Meter control printed blank in
  the command palette — the "value renders wrong or not at all" bug recurring for a third time,
  now behind one shared accessor. The FPS HUD itself was proved untouched: it gained ten lines
  across the whole phase, all of them comments.
- Built the rail's real icon set (11 icons on a constexpr 24-unit grid, unit-testable geometry)
  and the sheet's multi-column layout that `Solve()` had been computing since P2 but nothing
  drew — **the seventh instance in this codebase's own history of a value that rendered
  correctly and did nothing** (after `#25`'s frame limiter and `#68`'s force-grab-cursor on
  master, and more below). Also built inline parameter expansion, which a stale comment had
  claimed already existed.
- Fixing the 2.0× layout where an open Inspector drawer overlapped the entire sheet control
  column (making every control in it unreachable) surfaced three more defects only a real
  keypress could find: `KEY_SLASH` was missing from the keycode table, so `Ctrl+/` was
  unreachable from an actual keyboard though `/` still typed fine; a dropdown list never
  rendered at all because ImGui closes a popup whose parent window isn't focused, and the slab
  is deliberately `NoBringToFrontOnFocus`; and Left/Right were dead keys on the sharpness slider,
  because its 21-notch real range hid behind a declared step size tuned for a 0–100 scale.
  `overlay_e2_key` was added so real keypresses — not synthetic focus injection — could be
  driven from a script, which is how these were caught.
- Shipped the **command palette** (`Ctrl+K`), searching all 102 live registered settings/params
  with no new declaration needed at any call site — confirming the registry design's own goal.
- Ported the Monitor and Log areas and built the "composite" control kind (an N×44-tall band,
  e.g. the placement anchor grid) that earlier phases had avoided shipping by downgrading it to
  a plain 9-option list. Found and fixed: the Log's row kinds ("Bank" and "Text") drew nothing at
  all — an unhandled-enumerator bug, the same shape as `#25`/`#68` — and setting `display_scale`
  from the console aborted the compositor by rebuilding the font atlas from the console thread
  rather than the render thread (this recurred; see 2026-08-22 below).
- Ported Audio (the one area with a dynamic, not fixed, set of rows — streams come and go, so
  rows are keyed by PipeWire node id rather than slot position) and Config, and fixed the
  Inspector's scroll, which had never worked: its content laid out from a fixed screen
  coordinate instead of the child window's own scrolled cursor.
- Ported Display and Shaders into the registry, one-to-one against every existing config key,
  and found two more instances of the same "renders, does nothing" shape: `BeginDisabled` was
  never reaching the custom draw-list atoms, so disabled controls painted at full brightness
  regardless; and a downgraded dropdown-turned-Choice had no dropdown to open at all — the third
  occurrence of that exact bug after `#25` and `#68`.
- **P1–P2: the UI kit foundation and the shell itself, built behind the `overlay_e2` flag**, off
  by default so `master`'s dock stayed byte-for-byte unchanged while this was built alongside it.
  The foundation makes the drawn-vs-hit-tested divergence that caused `#23`, `#42`, `#47` and
  `#49` on master structurally impossible: one `ImRect` per control atom, so the rectangle handed
  to ImGui's hit-testing is the same rectangle the painter draws, with no second copy to drift.

---

## master

The settings overlay as shipped: an ImGui-based panel system (Display, Shaders, Audio, Config,
System Monitor/FPS HUD, notifications) composited as a `FrameInfo_t` layer, plus roughly 30
issues found and fixed against the initial build. This is the UI `feature/overlay-e2` above
replaces; nothing below is affected by that branch until it merges.

### 2026-08-23

- **System Monitor** (formerly "FPS HUD") reached its final shape for this thread: every
  module's minimum width now shrinks to its own measured content instead of a flat 186px floor
  that "turned out to protect nothing, which was checked rather than assumed" (`#80`); a media
  track title is truncated with a real ellipsis so one long song name can no longer stretch the
  whole module stack two or three times wider than its siblings (`#77`); the master toggle was
  renamed "Show System Monitor" and FPS got its own switch, since the master toggle had been
  doing double duty (`#70`); a toggle was added for the numeric frametime readout, independent of
  its graph (`#71`); every module now renders at the width of the widest enabled one (`#72`); and
  FPS gained a switch to hide its own label (`#73`).
- **Fixed a genuine input-safety bug**: `DrainInputQueue` accepted every raw pointer-motion
  coordinate from wlserver unchecked. A spurious event carrying a coordinate a full output-width
  off — normalised X of exactly `-1.0`, or a `-8388608` relative delta — landing mid-drag made
  ImGui reassign focus to whichever panel happened to sit at that position; captured live once,
  pressing one panel's title bar moved a different panel instead. Both absolute and delta events
  are now validated *before* they can perturb the cursor, since clamping the final position
  after the fact still hands ImGui a legitimate-looking coordinate for a real, wrong place
  (`#65`, `#75`, `#76`). The originating bad value itself was never explained — the fix stands on
  its own regardless.
- Fixed panel windows shrinking while being dragged toward the right or bottom edge of the
  screen: the existing resize-clamp from `#58` ran on every frame, not only during an actual
  resize-grip drag, so a title-bar move chased its own advancing position every frame (`#74`).
- Converted the remaining checkbox call sites to switches, for visual consistency — no site in
  this UI has a genuine multi-select list, so the checkbox visual carried no meaning it needed
  (`#60`).
- Split System Monitor's settings into six tabs (General plus one per module) instead of one long
  scroll the user called "clunky, unordered and bloated" (`#59`), and routed all eight of its
  sliders through the shared slider helper that the rest of the UI already used, since these were
  the outlier the user pointed at (`#61`). Wrote `slider-widget-spec.md`, measuring the shared
  slider's geometry from a real render by pixel-sampling a screenshot, and brightened the slider
  rail/mark greys for readability (`#62`).
- Stopped the nested-mode host cursor from drawing a second, ghost cursor alongside ImGui's own
  while the overlay has input — suppressed the host's cursor instead of the overlay's, since in
  embedded and grabbed-nested modes ImGui's is the *only* cursor that exists (`#69`).
- Moved the HDR toggle onto its own HDR tab; widened the frame limiter to 0 or a 10–480 range
  with a real gap rather than a plain 0–480 continuum, since below 10fps the overlay itself is
  too slow to drive; and **fixed Force Grab Cursor, which had shipped rendering correctly and
  doing nothing** — `g_bForceRelativeMouse` was read every frame but only ever acted on at
  backend startup, so a live toggle in the UI reached nothing until
  `steamcompmgr_set_force_relative_mouse` was added to push the mode immediately (`#66`, `#67`,
  `#68`).
- A second round of window-geometry fixes: equal dock margin above and below (`#64`); growth now
  stops at the screen/dock edge instead of overshooting and letting the on-screen clamp yank the
  whole window back, which had made the opposite edge appear to move (`#58`); resizing restricted
  to the bottom-right grip only (`#56`); and a hard 250×250 pixel floor rather than a
  scale-multiplied one, since a scaled floor would shrink to 125px at 0.5× — too small for a
  title bar and its own label (`#57`).
- The Audio panel now resolves stream names the way `ncpamixer`/`pactl` do —
  `application.name`, then `media.name`, then the raw `wpctl` label — instead of showing raw
  technical identifiers like a Bluetooth input's `bluez_input.98_0D_AF...` (`#63`).

### 2026-08-22

This is the day the initial build's milestones (M0–M9) landed, and also the day most of the
first wave of post-build fixes went in — the fork's most active single day.

**Milestone build-out (`#1`–`#16`, `#20`, `#21`):** vendored ImGui and `nlohmann::json`; a
layered global/per-game config system with atomic writes (M0, `#3`); the ImGui render shell
composited as a `FrameInfo_t` layer (M1, `#4`); keyboard and pointer input capture into the
overlay with careful press/release routing so a key held across an open/close toggle still
releases to whichever side received the press (M2, `#5`–`#7`); a live Display/Shaders options
panel (M3); an always-on FPS display reading the game's own per-frame timing rather than the
racy shared `mangoapp` struct (M4, `#9`); a PipeWire volume-control backend and its panel,
matching sandboxed/Proton games by process tree plus a name fallback (M5, `#8`, `#10`); a
combined Vibrancy+Pre-Sharpen ReShade effect, toggled by uniform rather than shader swap to
avoid a synchronous recompile (M6, `#11`); full config persistence and per-game override UI (M7,
`#12`); IBM Plex Sans/Mono typography (M8 part 1, `#13`); custom ImDrawList widget rendering for
switches and checkboxes, built on ImGui's own `ButtonBehavior` so keyboard nav and
`BeginDisabled` behave identically to stock widgets (M8 part 2, `#14`); window/dock chrome (M8
part 3, `#15`); and an 11-icon SVG set (`#16`). Also fixed along the way: orphaned `.tmp` config
files left behind by a killed process are now swept on load, but only once the writing process
is confirmed dead (`#21`), and ReShade was recompiling its pipeline **every frame** under
FSR/NIS because the preemptive-upscale path ran it a second time with a mismatched cache key —
measured at 3,216 recompiles in 12 seconds before the fix, 2 after (`#20`, filed "Critical").

**GAMESCOPE panel and the frame-limiter bug (`#25`):** renamed the DISPLAY panel to GAMESCOPE
and split it into Upscaling/Display/Frame-Limiter/HDR tabs. The frame limiter moved and
displayed a value but never changed the actual framerate — because `paint_all()` calls
`update_app_target_refresh_cycle()` **every single frame**, which unconditionally zeroes
`g_nSteamCompMgrTargetFPS` and restores it only from a different global the panel wasn't
writing. So the panel's write reached the right X11 property, took effect for one frame, and was
silently stomped back to zero on the next. Fixed by routing through the same entry point the
Steam client itself uses. Verified live at two values against a 120Hz output's integer vblank
divisors, not just "the number changed."

**System Monitor build-out (`#27`, `#28`, `#29`, `#40`):** renamed from "FPS HUD", given a 3×3
placement grid and a module framework (`#27`); CPU, GPU and media-playback modules added,
degrading honestly ("GPU unavailable (no amdgpu)") rather than fabricating numbers, verified
against real load changes from `stress-ng` and extra `vkcube` instances (`#28`); styling options,
an Inverted (black-outline/white-fill) blend mode chosen for architectural reasons — the HUD
renders into its own transparent-cleared offscreen texture, so a literal destination-invert isn't
available on this compositing path — and per-module colour overrides (`#29`); and a 60-second
Statistics tab with rolling graphs (`#40`). `#29` and `#40` both restructured the same settings
panel from different starting points and collided on merge; the resolution is recorded because it
nearly dropped `#29`'s colour controls a second time, the same failure mode a previous merge had
already caused once.

**The UI-scale saga (`#23`, `#24`, `#38`, `#46`–`#49`, `#51`, `#52`, `#54`):** widening the UI
Scale slider from 0.8–1.4× to a real 0.5–2.0× range (`#24`) exposed that `display_scale` only
ever drove `FontGlobalScale` — text grew, but every hand-drawn pixel constant in the UI did not,
so segmented-control labels truncated mid-word ("linear" → "inea") at 2.0× (`#46`), panel windows
didn't resize with their own contents (`#47`), and default panel tiling didn't scale either
(`#49`). The underlying fix raised the baseline font/control sizes 20–25% and made control
geometry itself scale-aware (`#23`), rebuilt the ImGui font atlas at the effective scale instead
of resampling a fixed-size bake for crisp glyphs at any zoom (`#38`), and then found that
`FontGlobalScale` was being applied **on top of** the atlas's own already-scaled bake — a
scale-squared bug only visible once a non-default scale had been committed once (`#48`, and its
drag-preview counterpart `#54`). `#52`, reported as `display_scale` corrupting on save, did not
reproduce; the config read/write path round-trips exactly, and the likely cause was traced to an
unrelated screenshot tool producing non-uniformly-scaled PNGs used for the original measurement.
Deferring the font-atlas rebuild to the start of the next frame (`#51`) fixed a genuine
mid-frame-invalidation bug: rebuilding synchronously deletes glyph state that the same frame's
already-recorded draw commands still reference.

**Window/panel chrome (`#26`, `#31`–`#34`, `#42`, `#44`):** a real 34px title bar with drag,
shading and an unmistakable two-layer focus glow replacing a near-invisible hairline; resizable
panels opening at 1.5× their old size; panels clamped fully on-screen against a shrinking host
window; movement restricted to the title bar (right-click shades, middle-click closes, replacing
double-click); the dock hint text raised from barely-legible to 85% text opacity; and
notification placement replaced two separate segmented controls with one 3×3 grid, later reused
by `#27`'s own placement grid rather than building a second one (`#26`).

**Config safety and UX:** `#43`'s "stop deleting per-game config on override toggle-off" — the
toggle used to be a bare filesystem delete with no confirmation; it now flips a flag so the file
survives, with actual deletion moved behind an explicit, confirmed red button — was itself an
amendment to an earlier full-snapshot design decision, decided directly by the user. Also
shipped `#43`'s three approved config-UI recommendations: a title-bar layer badge showing
global/app-id/global-only, per-group reset links on the General tab, and a
`last_applied_profile` provenance readout.

**Also this day:** a hue-only accent colour picker deriving all ten accent tokens from one live
hue, validated by reproducing the spec's reference hex values before porting (`#37`); background
blur and darkening wired live, with darkening moved onto the compositor's own colour-transform
matrix path so it keeps working under blur (blur bypasses the code path darkening previously used
alone); a General-tab settings bug where any other panel's write could silently revert the
General tab back to stale defaults, from a routed-write path forwarding a caller's stale cached
copy; framerate-independent double-click timing, traced to queued input events carrying no
timestamp at all rather than to the click-detection logic itself, which was already correct; real
keyboard-layout text input via `xkbcommon` replacing a hardcoded US-QWERTY table; and a
double-toggling `Ctrl+Shift+O` hotkey caused by `wlr_keyboard_group`'s own event-forwarding
re-entering the hotkey handler under a second keyboard identity. Separately, the actual root
cause of a fullscreen VRR flicker under investigation for many rounds was finally found: both
overlay textures were written on the graphics queue family and sampled on the compute family
while created `VK_SHARING_MODE_EXCLUSIVE` with no ownership transfer between them — Vulkan leaves
the contents formally undefined in that case, and RADV genuinely reports separate queue families
on the hardware this was tested on. Fixed with a concurrent-sharing mode rather than the
double-buffering attempted (and reverted) earlier in the investigation, which only widened the
race window without closing it. Tooling also landed: a single `build-gamescope-ritz.sh` entry
point, an interactive installer/updater, and namespacing all data directories to
`share/gamescope-ritz` so this fork can never overwrite a co-installed packaged `gamescope`'s data
— a real hazard the old path had, caught before it shipped.

### 2026-08-21

The fork's history begins here: the planning commit that added `superdoc/`, the project plan,
the initial ~18-issue backlog, and the SVG icon set used throughout the UI above.

Most of that day's work is the milestone build-out summarized under 2026-08-22 above (the
merges themselves land with 2026-08-21 timestamps in several cases and 2026-08-22 in others,
reflecting when a milestone's branch was started versus merged — this file follows the merge
commit's own recorded date). Distinct from the build-out, this day carried a long **flicker
investigation**: a user-reported fullscreen VRR artifact was chased across many rounds and
several disproven hypotheses before the actual root cause (the queue-family sharing bug, fixed
and described under 2026-08-22) was found. Along the way: a rewritten test harness that could
actually see the bug class involved (native resolution, real `--fullscreen`, all backends, GPU
fault log-scanning) after the old one's 640×480 SDL-only default had passed clean on the exact
commit that failed at 1920×1080; confirmation that the flicker reproduces on pure upstream
`gamescope` at this fork's own base commit, ruling it out as this fork's regression; and,
separately, confirmation that the project's own test tooling had been defaulting to the SDL
backend, which has an unrelated, genuine upstream flicker bug of its own that real users never
hit because `gamescope` auto-selects the Wayland backend when nested under a Wayland session.
Several now-superseded planning documents from the dead ends in this investigation are kept
rather than deleted, since "the reasoning is kept, since the dead ends are instructive."
