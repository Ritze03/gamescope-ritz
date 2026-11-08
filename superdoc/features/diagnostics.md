# Compatibility (was Diagnostics)

The Shell's **Compatibility** area (`system.diagnostics`, labelled Diagnostics until 2026-11-08,
OTHER rail group, first) holds the machine-wide compatibility switches and the checks. The checks look at setup
outside gamescope that costs frames without printing an error, says what is wrong and what to
set, and has a **Recheck all** button. The warning can be silenced
(see [Silence warning](#silence-warning)). Code: `src/Overlay/Diagnostics.{h,cpp}` (runtime),
`src/Overlay/DiagnosticsLogic.h` (pure: parsing and state decisions,
`tests/test_diagnostics.cpp`), `src/Overlay/PanelDiagnostics.cpp` (area). One check today.

## The Compatibility area

Since 2026-11-08 the area (id `system.diagnostics`, kept, as About kept `system.changelog`) is
labelled **Compatibility** and is the first of the OTHER group (Compatibility, Log, About). Two
columns (`a.Columns( 2 )`, `GroupColumn`):

| Column | Group | Rows |
| --- | --- | --- |
| 0 | This machine | Block direct scanout, Compress output images (Wayland backend only, `ShownWhen`) |
| 0 | Recommended settings | **Pick recommended settings** [Apply], **Re-detect** [Detect] |
| 1 | Environment | read-only Facts Desktop, GPU, Backend, Recommended; then the Direct scanout check |
| 1 | Actions | Recheck all; then the check result as rows (shown when relevant): when scanout is Off, Direct scanout is off / Fix / Lua config / hyprlang config, else Detail (the reason sentence) |

`Why:` the user's idea (2026-11-08): machine-wide settings on the left, checks on the right, and
defaults derived from the environment instead of one fixed default. The two switches left Display >
General for it (General is one column again). The summary line is the Direct scanout headline. The
GPU fact is filled from `rendervulkan.cpp` (`Diagnostics_SetGpuName( props.deviceName )`). The rail
icon is unchanged (the pulse trace).

`Why:` the check result is rows, not a content body (user test 2026-11-08: *"The compatibility tab is
a single column instead of two."*). `Solve()` forces one column for an area with a content body
(`AreaIsUnsplittable`, `Shell.cpp`), so the former body under Recheck all collapsed the pinned
two-column layout. The text is the same; the area has no `Area::Content` now. Two columns also need
a sheet of at least `kTwoColMin` (840 base px, `UI/Layout.h`) after rail and inspector column.

### Restart to apply

Compress output images is latched at the first output image, so a change applies on the next
launch. While `Diagnostics_CompressOutputImages() != Diagnostics_CompressOutputImagesAtLaunch()` the
row shows an orange **Restart to apply** left of its value; switching back hides it. Mechanism:
`Entry::StatusWhen( fn )` (`Registry.h`; the Shell draws a non-empty result in `Role::WarnText` left
of the value and the label zone gives up the width). `Why:` the user: today you have to read the help
text to learn that it needs a restart. The help text still says "Applies on the next launch".

## Silence warning

The **Silence warning** switch sits in the Inspector's CONFIGURE page of the Direct scanout row,
under the "Hyprland render options" button (`Entry::InspectorSwitch()`, not a sheet row;
`Why:` the user marked that spot on a screenshot). Config
`overlay.diagnostics_silence_warning` in `global.json`, default off. On: no startup toast and no
yellow rail dot; the area still shows the true state and the fix text. `Why global:` the host
setup belongs to the machine, not to a game, so it is never in a profile. The decision is pure
(`Alerts( state, bSilenced )` and `AnyWarns( b, e, bSilenced )` in `DiagnosticsLogic.h`, tested in
`tests/test_diagnostics.cpp`); `Diagnostics_Silenced()` caches the setting in an atomic, and the
setter writes `global.json` and repaints, so the dot follows at once. The toast is decided when
it is shown, so silencing before it appears also suppresses it.

`Why a switch at all:` the warning was switched off 2026-11-06 to 2026-11-07 because direct
scanout on could crash the GPU (4 crashes in about 285 fullscreen entries, 0 in 183 with scanout
off). The crash was fixed 2026-11-07 (no WSI memory chain on output images under a host, see
[backend-wayland.md](backend-wayland.md#direct-scanout-under-a-host)), so the warning is back, and
users who deliberately keep scanout off can mute it. Picture artifacts on entering and leaving
fullscreen with scanout on are still open there.

`Why the fix text offers 1 or 2:` the choice is the user's. Hyprland's own wiki: 1 is enabled
("when there is only one fullscreen application on a screen"), 2 is auto, enabled with content type
'game'; it also advises turning it off if the fullscreen application shows graphical glitches. The
crash test ran with 1 only, and earlier zero-copy measurements used 2; the state check treats both
as On.

## Recommendations

The two global switches `overlay.diagnostics_block_scanout` (Block direct scanout) and
`overlay.compress_output_images` (Compress output images) are optional keys in
`global.json`. An ABSENT key is not "false": it takes the recommendation for the host
desktop. A key that is present is the user's own choice and is never changed except by
"Pick recommended settings". A save never writes an untouched switch
(`ConfigManager.cpp`), so absent stays absent.

Host desktop: `Diagnostics_HostDesktop()`, from the environment gamescope was launched
with (`Diagnostics_CaptureHostEnv()`, first thing in `main()`): `HYPRLAND_INSTANCE_SIGNATURE`
set, or `XDG_CURRENT_DESKTOP` (colon-separated list, case-insensitive, entry prefix match)
naming Hyprland, GNOME or KDE; otherwise Other. Pure logic in `DiagnosticsLogic.h`
(`DetectDesktop`).
Why: `main.cpp` overwrites `XDG_CURRENT_DESKTOP` with `gamescope` later, so only the
captured copy is the host's.

Recommendation table (`Recommend()`; the GPU plays no part):

| Desktop  | Block direct scanout | Compress output images |
|----------|----------------------|------------------------|
| Hyprland | Off                  | Off                    |
| GNOME    | Off                  | On                     |
| KDE/other| Off                  | On                     |

Why: Compress On blacked out fullscreen on Hyprland; the other desktops keep the shipped
defaults.

Resolution: the first call to any of the getters runs `ResolveCompat()` once (call_once):
detect the desktop, read `global.json`, resolve each absent key to the recommendation,
log it once (`compatibility: desktop ...`). `vulkan_compress_output_images()` calls
`Diagnostics_CompressOutputImages()` at the first output image, so the very first launch
already runs with the recommendation, no restart. The resolved value is not written to
`global.json`.
Why: a first launch under Hyprland must not render a black screen and ask for a restart.

Launch value: `Diagnostics_CompressOutputImagesAtLaunch()` is what the process started
with (what rendervulkan latched). The UI shows an orange "Restart to apply" while
`Diagnostics_CompressOutputImages()` differs from it, and clears it when switched back.
Why: Compress is latched at the first output image, which users otherwise only learn from
the help text.

Buttons: "Pick recommended settings" = `Diagnostics_ApplyRecommended()` (writes both keys,
making them present; Block live, Compress next launch; text from
`Diagnostics_RecommendationText()`, e.g. "Block scanout Off · Compress Off").
"Re-detect" = `Diagnostics_Redetect()` (changes no setting; the read-only facts
Desktop/Backend/GPU come from `Diagnostics_Facts()`; GPU reads "unknown" until
`Diagnostics_SetGpuName()` is called).
Re-detect only logs and repaints: the desktop is captured at launch, the GPU/backend facts are
read live, so its help says a session change shows after a restart.

Row defaults: both switch rows declare `.Default( Diagnostics_Recommended().x )`, so the Shell's
modified edge, the Inspector's "reset" and Ctrl+D compare against the recommendation for this
desktop (fixed at launch; registration runs after `Diagnostics_CaptureHostEnv()`). Why: a
constant default would mark an untouched row as modified on Hyprland. Known quirk: "reset" goes
through the row's setter, so it writes the key (making it present, at the recommended value);
it is a deliberate user action, so this does not break "a present key is only changed by the
user". On a backend where the switches are hidden (not Wayland) the "This machine" group has
no shown rows and the Shell draws no band for it (bands are emitted per shown row).

## Block direct scanout

The switch is described [below](#block-direct-scanout-switch-displayblock_direct_scanout).
While it is on, `WithBlock()` (pure, `DiagnosticsLogic.h`, `tests/test_diagnostics.cpp`) turns any
applicable result into state **Blocked** ("Blocked by gamescope", the host's own value still shown
in Details) when the result is *read*, so toggling needs no recheck. Blocked never warns: no
toast (`Diagnostics_Tick()`) and no rail dot (`Diagnostics_HasWarning()`), since "scanout is off,
turn it on" would be wrong; the area still states the truth. Switching it off brings the warning
back only if the state is Off. Not applicable and Pending are left alone.

## Hyprland direct scanout

Why it matters (when it works): under nested Wayland on Hyprland the host's own draw waits behind the game's GPU
job, so frames Frame generation makes are lost (shown about 160 uncapped). With
`render:direct_scanout = 2` and the output images of [backend-wayland.md](backend-wayland.md#direct-scanout-under-a-host)
the host puts gamescope's buffer on a plane and shown reaches 261-272. See
[frame-generation.md](frame-generation-host-scanout.md#why-the-host-loses-frames).

States (`ScanoutState`):

| State | When | Warns |
| --- | --- | --- |
| Not applicable | the backend cannot be scanned out by a host (`HostMayScanOutOutputImages()` false: DRM, headless, OpenVR, **SDL**) | no |
| Hyprland not detected | `HYPRLAND_INSTANCE_SIGNATURE` was not set when gamescope started | no |
| On | `render:direct_scanout` is 1 or 2 | no |
| Off | it is 0 | **yes**, unless silenced |
| Blocked | the user's Block direct scanout switch is on (shown instead of any applicable state above) | no |
| Could not check | no `hyprctl`, no answer in 1 s, non-zero exit, no such option, unparsable output, a value other than 0/1/2 | no |

The backend decides before Hyprland does, so headless under a Hyprland session says "Not
applicable". `Why SDL is "not applicable" and not checked:` the SDL backend presents through a
window swapchain the host composites (it round-trips composited frames, see
[backend-sdl.md](backend-sdl.md)), and only the Wayland backend allocates output images a host
can scan out; telling an SDL user to turn the option on would promise a gain that does not come.
`Why "could not check" never warns as if off:` a missing `hyprctl` or an unknown option is not
evidence that scanout is off, and a false alarm under a game is worse than a silent miss.

### How it is read

`hyprctl -j getoption render:direct_scanout` prints `{"option": "render:direct_scanout", "int": 1,
"set": true }`; an unknown option prints the bare text `no such option` with exit 0. (`hyprctl
dispatch` cannot read options.) Only `"int"` is read. The call is `posix_spawnp` with a fixed
argv, no shell, stdin and stderr from `/dev/null`, every fd above 2 closed, its own session,
killed after 1 s. It runs on a **worker thread** that publishes one struct under a mutex
(`Diagnostics_View()` copies it): the compositor paints at 280 Hz and a blocking read on that
path would stall it.

`Why the environment is snapshotted at the top of main():` hyprctl needs
`HYPRLAND_INSTANCE_SIGNATURE` and `XDG_RUNTIME_DIR`, which gamescope never touches, but gamescope
does `unsetenv("WAYLAND_DISPLAY")` and sets `DISPLAY`, `XDG_*` and more for its own game.
`Diagnostics_CaptureHostEnv()` keeps the launch environment, and both hyprctl and the browser run
with it. For the browser it also drops what only makes sense for the game: `LD_PRELOAD`,
`LD_LIBRARY_PATH`, `ENABLE_GAMESCOPE*`, `GAMESCOPE_*`, `SDL_VIDEODRIVER`, `STEAM_GAMESCOPE_*`,
`PRESSURE_VESSEL*`, and so on (`DropForBrowser()`).

### Warning

The check starts on the first paint with a sized output (`Diagnostics_Tick()`, called every frame
from `SettingsOverlay_AddLayer()`). When it finishes with **Off**, `Tick()` raises one warning
toast (`Notifications::Show()` is paint-thread only, so the worker only sets a flag):
`Why one per session:` a repeating toast under a game is worse than the loss, and the Diagnostics
row stays as the record. The **Silence warning** switch (above) mutes it; the
notification system's global mute also silences it. A manual **Recheck all** never toasts. The row
and the fix text stay until a recheck reads On.

### The area

Sheet rows: **Direct scanout** (status; Details: the value read and the reason) and **Recheck
all** (disabled while a run is going). The docs button is **not a sheet row**: it is the
Inspector-only verb `Entry::InspectorVerb()` of the Direct scanout row ("Hyprland render options",
chip **Open**, `https://wiki.hypr.land/configuring/core/config-options/#render`). It is drawn
under the title on that row's Inspector page (Details, which a Facts row opens on, and Configure)
and on the area's **Overview**, the page the area opens on, so it needs no row selected first.
Keyboard: Enter/Space on the focused Inspector row fires it (Overview: the area's first shown
verb). Shown while the check applies (state On, Off or Could not check); hidden when Pending,
Not applicable or Hyprland not detected. It is not a palette row (like every read-only row);
searching "diagnostics", "hyprland", "wiki" or "docs" finds the area, whose Overview carries it.
`Why:` the user, on the first version with the button in the sheet: *"It should only show this
button on the inspector rail on the right side"*. Below the rows, when Off: what is lost and the fix
(set `render:direct_scanout` to 1 or 2, with Lua and hyprlang snippets). For the other states the area shows the one-sentence
reason.

The link runs `xdg-open <url>` the same way (fixed argv, no shell, the host environment above,
detached in its own session). Gamescope is a subreaper, so the child is reaped with a
non-blocking `waitpid` on later ticks. Opening a browser over a fullscreen game may take focus
from it; nothing opens by itself.

### The warning dot on the rail

While any diagnostic warns (and Silence warning is off), the Shell's rail shows a plain filled yellow circle (`glyph::WarnDot()`,
`glyph::kWarnDotColor`, antialiased, no "!") on the **OTHER** group heading and on the
**Compatibility** row. `Why:` the first cut was a drawn warning triangle with a "!", which looked
improvised; the user: *"The badge looks really bad. It doesnt look proper. Just make it a yellow
circle instead of the fake warning symbol."* The colour is a literal yellow, not `Role::Warn`
(orange).
`Diagnostics_HasWarning()` is derived from the published results (`AnyWarns()` over them, in
`DiagnosticsLogic.h`), not a separate flag, so a future check counts as soon as its result is in
the published range, and it clears when a recheck finds the problem fixed. Only **Off** warns:
"Could not check" does not, the same rule as the toast. It is wired through the generic
`Area::WarnWhen(fn)`; the rail asks it each frame it is drawn (one short mutex-guarded struct
copy, no child process, no repaint request: the worker already calls `force_repaint()` when a
result lands).

How it looks per rail state (`Shell.cpp`, `DrawRail()`):

| Rail state | Heading (OTHER) | Diagnostics row |
| --- | --- | --- |
| Full rail, group closed | dot right after the name, left of the chevron | not drawn (group closed, the heading carries it) |
| Full rail, group open | same | dot right after the row title |
| Icon-only rail | small badge on the icon's top-right | small badge on the icon's top-right |

The dot is drawn as a filled circle, not a font character: the bundled faces bake only Basic
Latin and Latin-1 (`Fonts.cpp`), so a glyph such as U+26A0 would be a box. It is placed after the name and clamped before the
chevron, so heading width and the accordion animation are unchanged. Not on the HUD or the
launcher. `Why:` the user: *"if something is active in the diagnostics, it should show a warning
sign on the OTHER category and the Compatibility tab name, to tell the user, to look there."*

### Adding a check

`kChecks[]` in `Diagnostics.cpp` is a plain array of functions from `Facts` to a result; the
runner and Recheck go through it. Nothing else exists (no plugin interface) because one check
does not need it.

### Test hook

`GS_RITZ_DIAGNOSTICS_ASSUME_WAYLAND=1` makes the check treat the backend as applicable, so a
headless instance can run the whole path (spawn, timeout, toast, row) against a fake `hyprctl`
put early on `PATH`. It exists because the real condition needs a host compositor; it changes
nothing else.

## Block direct scanout switch (`display.block_direct_scanout`)

Default Off (absent key: the recommendation). Config `overlay.diagnostics_block_scanout` in `global.json` (the key kept its early name
from when the switch sat in Diagnostics). On, gamescope itself keeps a nested Wayland host from
putting its window on a display plane. It applies live: the setter (`Diagnostics_SetBlockScanout()`,
`src/Overlay/Diagnostics.cpp`) stores an atomic, writes `global.json` and calls `force_repaint()`;
the Wayland present path reads only `Diagnostics_BlockScanout()` (an atomic) every frame, and turning
it off detaches the buffer on the next frame. Mechanism:
[backend-wayland.md](backend-wayland.md#block-direct-scanout-from-gamescope).

**Global, not per profile.** `Why:` the host setup belongs to the machine, not to a game. The row
carries the global config key via `.Key( "overlay.diagnostics_block_scanout" )` (like Filter game
profiles); the Shell's per-profile dot and "Reset to inherited" only know settings keys, so the row
shows neither.

**Wayland only.** The row has `.ShownWhen( Diagnostics_ScanoutApplicable() )` (the Wayland backend,
`HostMayScanOutOutputImages()`; `GS_RITZ_DIAGNOSTICS_ASSUME_WAYLAND=1` forces it for tests). On SDL,
DRM, headless and OpenVR it is neither on the sheet nor in the command palette. An SDL window on a
Wayland desktop gets no row either: the blocker exists only in `CWaylandConnector::Present()`, so
there is nothing to block ([overlay-shell.md](overlay-shell.md#inspector-only-switch)'s row-level hide).

`Why:` a user on GNOME (Mutter) saw the whole picture, menu included, scrambled into fine block noise
after tabbing out and back in, and this switch cured it, so host direct scanout is involved; GNOME has no
reliable user switch for scanout, so the switch has to be on gamescope's side, both as a test and as a
workaround. The brief artifacts on entering and leaving fullscreen under Hyprland and GNOME are the
other reason ([TODO](../planning/TODO.md)). `Why it moved:` it first lived in the Inspector of the
Direct scanout row, then in Display > General (the user asked for a normal GUI feature); on
2026-11-08 it moved here, with the user's idea of machine-wide settings left and checks right.

While it is on the Direct scanout row reads "Blocked by gamescope" and nothing nags (no toast, no
yellow dot); see [Block direct scanout](#block-direct-scanout) below.

**Cost:** with scanout blocked the host composites again, so with Frame generation the shown rate falls
back (about 160 uncapped against 261-272 with scanout). The host must offer
`wp_single_pixel_buffer_manager_v1`; without it the switch does nothing and logs once.

**Not verified:** whether Hyprland or Mutter actually refuses scanout with the extra surface needs a
live test. Hyprland's `solitaryBlockedBy` `OPAQUE, SURFACES` supports it there; for Mutter the
single-surface requirement is from memory, not checked.

## Compress output images switch (`display.compress_output_images`)

Default **On** for a desktop that does not recommend otherwise (an absent key takes the
[recommendation](#recommendations)). `Why On:` the user: "DCC should be on by default since it is the default in GameScope."
On, the driver may pick an AMD DCC modifier for gamescope's output images, as upstream does. Off
restores the filter this fork added in fd24a76, in `CVulkanTexture::BInit()` (`src/rendervulkan.cpp`):
output images then never use a DCC modifier ([backend-wayland.md](backend-wayland.md#direct-scanout-under-a-host)).

**Applies on the next launch** (since 2026-11-08 the row shows an orange "Restart to apply" while
the value differs from the one this run started with, see [Restart to apply](#restart-to-apply)).
`vulkan_compress_output_images()` reads the setting once, at the
first output image, and keeps that value. `Why:` output images are allocated at startup and again on
a resize (`vulkan_remake_output_images()`); the host may still hold the old ones (see
`vulkan_select_free_output_image()`), so a live switch would either do nothing until a random resize or
reallocate images the host is scanning. The row says so. Config is loaded in `main()` long before
Vulkan starts, so the lazy `global.json` read is safe there.

**Global, Wayland only.** Same `.Key(...)` and `.ShownWhen( Diagnostics_ScanoutApplicable() )` as
Block direct scanout. `Why global:` the GPU and host belong to the machine, not to a game.

`Why the switch exists:` fd24a76 removed DCC from output images because, on RDNA 3 with Hyprland's
direct scanout on, the driver's pick is a compression the screen refuses and Hyprland reconfigures
the display on every frame (511 modesets in 6 s, shown frames down to 73-147). **Known cost of the
On default:** that modeset loop returns on such a setup until the switch is turned Off. Separately, a
user on an RDNA 4 GPU (GFX12 modifiers) under GNOME sees the picture scrambled into block noise while
the host scans out gamescope's output image; screenshots stay clean and Block direct scanout cures
it, so the image is intact and the plane's read of it is wrong. **Hypothesis, untested:** that DCC
(this switch On) cures it; the old filter was only ever tested on RDNA 3.

**Log line**, once at creation and again if a remake picks another modifier, switch On or Off:
`output image: format 0x<fourcc> modifier 0x<16 hex digits>, compress output images on|off`
(scope `vulkan`). The modifier is the number to ask an RDNA 4 user for.

## History

- **Compatibility / Diagnostics** (glossary detail): **Compatibility** (**Diagnostics** until 2026-11-08) — the Shell's `system.diagnostics` area, in the **OTHER** rail group (2026-11-05, first since 2026-11-08; `src/Overlay/PanelDiagnostics.cpp`, `Diagnostics.{h,cpp}`, pure half `DiagnosticsLogic.h`): checks of the setup *around* gamescope that cost the user frames without any error, each with a plain state and what to change, plus a **Recheck all** button. One check today: whether Hyprland has `render:direct_scanout` on, read with `hyprctl` off the paint thread and only when gamescope runs nested through the Wayland backend (anywhere else it says "Not applicable" and never warns). Its warnings (a once-per-session toast, and a yellow dot on the OTHER heading and the Diagnostics row of the rail, both added 2026-11-05) were off 2026-11-06 to 2026-11-07 while scanout on could crash the GPU, and can be muted with the **Silence warning** switch (global); the docs link is an Inspector-only button, not a sheet row. The user's own wording: *"Under the 'OTHER' category, add the tab 'Diagnostics'."* Distinct from the **Log** (what gamescope printed) and from the `Diagnostics` *group* inside the Log area (its buffer facts).
