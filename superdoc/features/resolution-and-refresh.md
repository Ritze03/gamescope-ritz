# Resolution and refresh at runtime (nested mode)

The Display > **Resolution** area of the settings overlay (`src/Overlay/PanelDisplay.cpp`,
`RegisterResolution()`) changes two things while gamescope and the game keep running.
Tracker: `../planning/requests-2026-09-05.md` item 7 (the area), `../planning/requests-2026-09-06.md`
items 7–10 (its present shape). Phase A was live behaviour with no persistence; **Phase B**,
below, persists game resolution and refresh across a restart.

## The two things, and which are live

| | CLI | What it is | Live? | How |
|---|---|---|---|---|
| **Game resolution** | `-w/-h` | The RandR screen Xwayland reports to the game; gamescope scales it to the window. | Yes | `steamcompmgr_set_nested_mode()` |
| **Refresh** | `-r` | The paced (fake-vblank) rate and the mode's advertised Hz. `0` = follow the host. | Yes | same call; `g_nNestedRefresh` |

Nothing restarts: not gamescope, not Xwayland, not the game.

**The window size is no longer here.** A third group of rows (`display.output_size` and two
steppers) used to ask the host to resize gamescope's own window through
`INestedHints::RequestOutputSize()`. The user had it removed on 2026-09-06 (item 10:
*"Remove the 'WINDOW'/Window sizing part."*) — it was a request a tiled host could refuse,
and host window rules are the right tool for it. The interface and its SDL/Wayland
implementations are untouched (they are backend API, and the panel was only their caller);
what the host granted is still reported, as the Live state's `window` fact.

### Game resolution and refresh — `steamcompmgr_set_nested_mode(w, h, refresh_mHz)`

`src/steamcompmgr.cpp` (declared in `steamcompmgr.hpp`). It writes
`g_nNestedWidth/Height/Refresh`, tells the connector's `INestedHints` via
`OnNestedRefreshChanged()` (see the SDL gotcha), then — with the wlserver lock held — calls
the **existing** `wlserver_set_xwayland_server_mode(idx, w, h, mHz)` (`src/wlserver.cpp`),
the exact path the Steam Deck's `GAMESCOPE_XWAYLAND_MODE_CONTROL` root atom uses.

*Two callers, opposite lock states.* The Shell's setters run on the steamcompmgr thread
**without** the lock; `gamescopectl overlay_e2_set display.resolution.aspect N` (or
`display.resolution.size N`) reaches the same setter from `gamescope_private_execute()` (`src/wlserver.cpp`), which wlserver dispatches
**with** the lock already held. The lock is a plain non-recursive mutex, so the first
version — an unconditional `wlserver_lock()` — deadlocked the console path: the command never
returned and every later `gamescopectl` command queued behind it (laptop, 2026-09-05). The
function now takes the lock only if `!wlserver_is_lock_held()`, the same pattern
`wlserver_debug_key` / `wlserver_debug_mouse_button` use. The keyboard path (Left/Right on
the Choice row) reaches it too: `AdjustValue()` (`Registry.cpp`) writes the next option
through `Binding().Set()`, which is `SetAspectChoice()` / `SetSizeChoice()` → `ApplyNestedMode()`. *Why a new function and not the atom:* the atom handler deliberately does
not touch `g_nNestedWidth/Height` (Steam owns those on the Deck); the fork's UI must, because
cursor-scale ratios, the layer-shell configure size and the area's own read-back come from
them.

The chain, verified in code:

1. `wlr_output_state_set_custom_mode()` + commit on the headless output backing Xwayland.
2. Xwayland updates its RandR screen — `xrandr` inside gamescope (`DISPLAY=:N`) shows the new
   `WxH@Hz` — and the X server sends a `ConfigureNotify` on the **root** window.
3. `configure_win()` (`steamcompmgr.cpp`, the `ce->window == ctx->root` branch) stores
   `root_width/height`, re-arms placement, `MakeFocusDirty()`.
4. `determine_and_apply_focus()` force-resizes the focused **fullscreen** game window to
   `root_width x root_height` (the `win_has_game_id()` branch).

Refresh needs no chain: `vblankmanager.cpp` re-reads `g_nNestedRefresh` every cycle and the
per-frame body recomputes `g_SteamCompMgrAppRefreshCycle` from it. The advertised mode
refresh is `g_nNestedRefresh ? g_nNestedRefresh : g_nOutputRefresh`, as wlserver's startup
path does — a 0 Hz mode would be invalid.

With `--xwayland-count > 1` only servers `#1..` get the new mode; `#0` is Steam's and the
per-frame output-changed block keeps it at the output size.

### Window size — removed from the UI (2026-09-06)

`INestedHints::RequestOutputSize(w, h)` (`src/backend.h`, default no-op) is still implemented
by the SDL backend (parks the size in two atomics, pushes `GAMESCOPE_SDL_EVENT_RESIZE`, leaves
`FULLSCREEN_DESKTOP`, converts pixels to points, `SDL_SetWindowSize()`) and by the Wayland
backend (unsets fullscreen, writes `g_nOutputWidth/Height`, `RequestDecorCommit()` so the next
`Commit()` sends `libdecor_state_new(w,h)`). Nothing in the tree calls either any more. Details
of both live in [backend-sdl.md](backend-sdl.md) and [backend-wayland.md](backend-wayland.md).

### Force maximize nested window (`display.force_windows_fullscreen`)

Tracker: `../planning/requests-2026-09-08.md` item 3. A Quick toggles switch in
`display.general` (`src/Overlay/PanelDisplay.cpp`, alongside Adaptive sync / Allow
tearing / Force grab cursor), persisted as `gamescope.force_windows_fullscreen` and
mirroring the pre-existing `--force-windows-fullscreen` CLI flag (upstream's own, unused
by this fork's UI until now). See TERMINOLOGY.md's **Nested window** entry for what
"nested window" means here — a client window inside gamescope's own Xwayland session, not
gamescope's own outer window (**Nested mode**).

**What it actually does.** `xwayland_ctx_t::force_windows_fullscreen` (`steamcompmgr.cpp`)
is read in exactly two places: `determine_and_apply_focus()`'s `win_has_game_id()` branch
(a focused Steam-tracked game window) and `handle_desktop_window()` (every other mapped
top-level window). Both already treat "is fullscreen" and "the flag is set" identically —
either resizes the window to the nested canvas (`root_width x root_height`) instead of the
size the window itself requested (`sizeHintsSpecified` / `requestedWidth/Height`). The
toggle changes nothing about *how* those two functions resize a window; it only supplies
the boolean they already read.

**Genuinely live, not startup-only** — determined by reading every consumer before
wiring the row, per this feature's own precedent (issues #25 and #68 both shipped a
control that rendered and silently did nothing). Two things made this the small, safe
case rather than the "label it startup-only" case:

1. The flag already had a live path *upstream never used from this UI*: an external tool
   writing the `GAMESCOPE_FORCE_WINDOWS_FULLSCREEN` root-window X11 property is caught by
   a `PropertyNotify` handler (`steamcompmgr.cpp`) that writes the ctx's flag and calls
   `MakeFocusDirty()`.
2. `MakeFocusDirty()` bumps a serial every `global_focus_t` checks once per frame
   (`focus_t::IsDirty()`), on the very same steamcompmgr thread `PanelDisplay_Draw()`
   itself runs on (see this file's own "Thread safety" header comment) — so a dirtied
   focus reaches `determine_and_apply_focus()` the next frame, for every focused window,
   the exact same effect the atom path gets from an external tool.

So `steamcompmgr_set_force_windows_fullscreen( bool )` (new, `steamcompmgr.cpp`/`.hpp`)
just does in-process what the atom handler does for an external one: sets every currently
live Xwayland ctx's flag (there is normally one; more under `--xwayland-count`) and calls
`MakeFocusDirty()`. `steamcompmgr_get_force_windows_fullscreen()` reads ctx 0 back for the
row's own state and the area's Summary line. Measured (headless nested gamescope, a raw
Xlib client with fixed `WM_NORMAL_HINTS` so it takes the `handle_desktop_window()` branch):
toggling the row while that same client keeps running moves it between its own requested
size and the nested canvas size with no relaunch — `build-release/verify-shots/
force-maximize-2026-09-08/results.txt`.

**CLI still wins over config, the same way `-w/-h/-r` beat `nested_width/height/refresh_hz`
— but the mechanism differs because this flag's own CLI parse lives somewhere else.**
`-w/-h/-r` and their config fields are both read inside `main.cpp`, so "config first, CLI
after" is one function's own call order. `--force-windows-fullscreen` has never been
parsed there: it is parsed inside `steamcompmgr_main()`'s *own*, separate
`getopt_long()` pass over the same `argv` (`steamcompmgr.cpp`) — a second, independent
parse that several long-only flags this fork's UI doesn't touch already go through. So
the config value cannot be written straight into a live global the way `g_nNestedWidth`
is; instead `apply_ritz_config_to_startup_state()` (`main.cpp`, still runs before *both*
getopt passes) seeds a small bridge global, `g_bForceWindowsFullscreenStartup`, and
`steamcompmgr_main()`'s own local `bForceWindowsFullscreen` (previously always
`false`-initialized) now seeds from it before its getopt loop runs. Since the flag is
`no_argument` (`--force-windows-fullscreen` can only ever set it *true*, there is no
`--no-force-windows-fullscreen`), an explicit CLI flag still forces the local variable
`true` unconditionally regardless of what the config seed was — the same one-directional
"CLI wins" guarantee, just enforced across two getopt passes instead of one. Verified
(same headless run): config `false` + `--force-windows-fullscreen` on the command line
still starts the probe client maximized.

Also live-applied on every non-startup config reload path that already carries
`force_grab_cursor` (`ritz_apply_config_live()` in `main.cpp`, and
`PanelDisplay.cpp`'s `PushCachedSettingsToLiveState()`), so a profile switch or a
per-game config swap while running picks it up immediately, not only at the next
launch.

### The pointer follows the change

The absolute-pointer mapping (force-grab off) is rebuilt from the painted base layer every
frame, so it follows the resized window on its own; what did not follow was the client's
pointer, which kept the old game-space position until the next host motion. Since item 10
(2026-09-05) `update_touch_scaling()` re-syncs it from the last host sample the moment the
mapping changes -- exactly once per real change, and **never while the game holds a pointer
lock** (2026-09-06: a locked client reads relative motion only, and the re-sync's absolute
event reached CS2 in play as a jump to the host position, "mouse look drifting back to
centre"; `wlserver_mousewarp()` now refuses every warp while locked). Details, including the
force-grab toggle and the X-side limit for windows larger than the new root:
[cursor-pipeline.md](cursor-pipeline.md), "The absolute pointer under a stretched resolution"
and "Locked pointer => never an absolute event". Regression gate:
`scripts/pointer-regression.sh`.

### A game that keeps its window (2026-09-08)

A Wine game answers the force-resize above by putting its window back to its own size and
dropping `_NET_WM_STATE_FULLSCREEN` (measured with `tests/pointer_probe_win32.c`; it is how
Wine decides "fullscreen", so it is what every Proton game does). Since 2026-09-08 a focused
game window heading for a size **larger than the new screen** is resized to the root anyway
-- the X server confines the pointer to the root, so the part beyond it could be seen but
never pointed at, which was the Rust report. A window smaller than the new screen is left
alone. Details and the measurements: [cursor-pipeline.md](cursor-pipeline.md), "A window
larger than the screen".

## Honest limits (the help text says these too)

Not achievable at runtime in nested mode, and not promised anywhere in the UI or here:

- **Changing the host monitor's refresh.** A refresh above the host's rate means frames are
  paced faster than the screen can show them.
- **Guaranteeing a running game adopts the new mode.** Fullscreen windows are resized within
  a frame or two; windowed games with size hints keep their size; games that read the mode
  list once list the new mode only after a restart. Wording used: *"most games switch
  instantly; a few only list it after a restart."*
- `ui::Applies::NeedsRestart` exists in `Registry.h` but nothing renders it yet, so the
  caveat is a Facts line ("takes effect") and help text. Swap to the badge when the shell
  grows one.

## The SDL static gotcha

`SDLBackend.cpp` keeps `g_nOldNestedRefresh`, the focused refresh that `FOCUS_GAINED`
writes back over `g_nNestedRefresh` after `FOCUS_LOST` swapped in
`g_nNestedUnfocusedRefresh`. Set at SDL init only, it would silently revert a runtime refresh
change on the next focus regain. Fix: `INestedHints::OnNestedRefreshChanged(mHz)` (default
no-op) — SDL's implementation rewrites the static, now a `std::atomic<int>` because the SDL
thread reads it while the steamcompmgr thread writes it.

## UI rows (`display.resolution`, gated `AvailableWhen(GetNestedHints() != nullptr)`)

| id | kind | notes |
|---|---|---|
| `display.resolution.aspect` | Choice | Native (window size at the moment of the pick), 16:9, 4:3, 16:10, 21:9, Custom — picking a shape applies the closest size in it, see [One size row](#one-size-row-and-a-shape-that-applies) |
| `display.resolution.size` | Choice, `Dropdown()` | **One** row; its option list is the selected aspect's real sizes, served live by `Entry::OptionsFrom()` — **no Custom entry** (item 3, below). Native/Custom have no list, so it shows their one true entry and is disabled |
| `display.refresh` | Choice | Follow host, 60, 90, 120, 144, 165, 240, Custom |
| `display.refresh.custom` | Stepper | 24–500 Hz, `DisabledUnless` Custom |
| `display.resolution_facts` | Facts | `nested WxH@Hz · output WxH@Hz` (item 4, below), from `g_nNestedWidth/Height`, `g_nNestedRefresh` (host when 0), `g_nOutputWidth/Height`, `g_nOutputRefresh`; plus "takes effect" and "applied via" lines |

### One size row, and a shape that applies

Tracker items 7 and 8 (2026-09-06). The user picks the **shape** (Aspect), and the single
**Resolution** row below it offers that shape's sizes. This replaces the four
`display.resolution.preset_*` rows of 2026-09-05's item 13, on the user's instruction:
*"There shouldnt be individual resolution elements for the different aspect ratios. It should
only change the available option."*

*Why one row is possible now.* The old design's reason for four rows was that the Registry
copied a Choice's options once at registration (`Entry::m_Options`, filled by `Area::Choice()`)
and has no per-entry visibility gate — so "the list follows the aspect" had to be one Choice
per shape, each `DisabledUnless` its shape was live. `Entry::OptionsFrom()` (Registry.h, added
for this) makes the option set a **read** instead: a provider re-asked whenever the row is
drawn, searched or written, its answer cached and only re-assigned when it actually differs,
so a caller iterating `Options()` cannot have the vector reallocated under it. Labels are
still borrowed `const char *`, so a provider must return static storage —
`SizeOptionsForAspect()` hands out the same four static tables the four rows used.

The row is forced to `Dropdown()`: seven entries of "3840 x 2400" is exactly the unbounded
option set that guide's Dropdown-vs-segmented rule names.

*Why the four ids can just disappear.* None of them was ever a config key (`.Key()` was
never called on them) — only `nested_width/height/refresh_hz` are persisted. So an old config
carries nothing to migrate, and a stale `gamescopectl overlay_e2_set
display.resolution.preset_16_9 3` simply reports an unknown id instead of doing damage.
`tests/test_resolution.cpp` pins that: a profile file carrying all four keys plus the removed
window-size ones loads, and its stored 1440x1080 still classifies as "4:3 + 1440 x 1080".

*Native and Custom keep the row honest.* They are sizes in their own right rather than shapes
with a list, so the row shows their single true entry ("Window size" / "Custom") and greys out
with the reason.

*UPDATE, 2026-09-07 (tracker item 3): the four real lists no longer end in Custom.* The user:
*"There is an option for a custom resolution, but the custom selection should only be a part
of the aspect ratio. Setting Aspect ratio to custom should allow the editing of the custom
resolution. The resolution selection should still switch to custom, but it shouldnt be visible
to the user."* So Custom now lives **only** on the Aspect row — the Resolution dropdown for
16:9 / 4:3 / 16:10 / 21:9 offers real sizes and nothing else; there is no "Custom" entry sitting
among "1920 x 1080" and friends for the user to notice or pick.

- **What that means for a size on no list.** Before this, a live mode with the shape but not
  on its list (a launch-time `-w 1600 -h 1200`, a console/config value) reflected as
  "4:3 + Custom" — the Resolution row's own Custom entry standing in for the mismatch. That
  entry is gone, so there is nothing left to show it with; the size now flips the **Aspect**
  itself to Custom instead (see the reflection rule below). The user's own wording — "the
  resolution selection should still switch to custom, but it shouldnt be visible to the user" —
  is implemented literally: `s_nSizeChoice` internally still carries `kSizeCustom` in this
  state, it is simply never one of the *options* a real shape's dropdown offers.
- **The steppers' gate is now exactly "Aspect is Custom".** `ResolutionIsCustom()`
  (`PanelDisplay.cpp`) used to also fire for a "shape + Custom" live size; that state no longer
  exists, so the predicate collapsed to a single aspect comparison. Width/Height are editable
  exactly when Aspect reads Custom, disabled with the same reason otherwise
  ("pick Custom in Aspect above to type your own size").
- **The Resolution row itself while Aspect is Custom.** Kept as it already was: disabled,
  showing its own single "Custom" placeholder entry (`kSizeOptionsCustom`) — this is the
  clearer of the two options the task named (hide entirely vs. show-disabled), because it
  keeps the row physically present at a stable position in the Group rather than having rows
  above and below it shift depending on Aspect, and its disabled reason text
  ("pick a shape ... to choose a size") explains itself without the user needing to already
  know Custom moved.
- **A console/config write of `kSizeCustom` to a real shape's list is redirected, not
  rejected.** `overlay_e2_set display.resolution.size 0` while Aspect is 16:9 (say, from an
  old script) now calls `SetAspectChoice(kAspectCustom)` — the same seed-and-apply a genuine
  Aspect→Custom pick makes — rather than either landing on the now-impossible "shape + Custom"
  state or silently doing nothing.

*Picking a shape now APPLIES the closest size, measured by height.* **This reverses**
2026-09-05's "picking a shape applies nothing", on the user's own instruction of 2026-09-06:
*"When changing the aspect ratio, make it automatically pick the closest resolution (measured
by height)."* `ClosestByHeight()` (`src/Overlay/ResolutionPresets.h`) returns the entry whose
height is nearest the height on screen, ties going to the **wider** one, and the shape switch
then runs the exact path a click in the Resolution row would (`SetSizeChoice()`), so there is
one apply path and one persistence write.

- **Why height** — the user named the axis, and it is the axis that survives a shape change:
  1920x1080 → 4:3 lands on 1440x1080, the same vertical detail in a different shape.
- **Why ties go wider** — of two equally tall modes, the wider one shows more.
- **Why the old rule went** — with one row instead of four, a shape pick that changed nothing
  left the Resolution row offering a list that did not describe the picture on screen. The
  2026-09-05 rationale (don't change the resolution as a side effect of browsing) applied to a
  *browsable* shape row; the user has now decided the jump is what they want.

*Custom seeding.* Picking Custom — as the aspect, or inside a shape's list — seeds the
steppers from the live mode (which is the last size applied) and re-captures the lock ratio,
so "16:10, then Custom" starts at a 16:10 size with the lock holding 16:10. Picking Custom
therefore changes nothing until a stepper moves. A stepper move under "4:3 + Custom" stays
recorded as 4:3 (the lock holds the ratio); under the Custom shape it stays Custom.

### The pure half is a header

`src/Overlay/ResolutionPresets.h` holds the shapes, their size tables, `MatchSizePreset()`,
`NearestAspect()`, `ClosestByHeight()`, `ClassifyAspect()` and `FormatLiveLine()` — no ImGui,
no backend, no compositor — so `tests/test_resolution.cpp` can pin them directly. Same split as
`src/Overlay/CrosshairMath.h`. `PanelDisplay.cpp` keeps everything that touches live state.

### The reflection rule (`CurrentAspect()` / `CurrentSizeChoice()`)

The rows read the live `g_nNestedWidth/Height`, never a stored pick, so a fresh open and a
change from outside (Steam's atom, a host resize) both show the truth:

1. An explicit pick is trusted **while the live mode is still the one it was made against**
   (`s_nPickWidth/Height`, captured at pick time — after the apply for a size pick, so it is
   recorded against the size it applied). This is what keeps a browsed shape selected, and
   what tells "Native" from "16:9 + 1920 x 1080" in a 1920x1080 window. Picking a size sets
   the pick to that size's shape. The trust ends the moment the mode changes for any other
   reason — a CLI/config apply, a per-game switch, Steam's atom.
2. Otherwise the live size is classified by `ClassifyAspect()` (`ResolutionPresets.h`, pure and
   unit-tested): on any shape's list → that shape; equal to the output's own size → Native;
   anything else → Custom.

**UPDATE, 2026-09-07 (tracker item 3): no more "shape + Custom" in step 2.** Before this, a
size merely *close* to a shape's nominal ratio (the old `NearestAspect()`-based fallback, within
`kAspectTolerance` — 21:9 is nominally 64:27 = 2.37 so the 2.37–2.39 panel sizes and true 2.33
all qualified) classified as that shape, with the Resolution row's Custom entry standing in for
the mismatch. That entry is gone (see [One size row](#one-size-row-and-a-shape-that-applies)
above), so there is nothing left for the in-between state to display: a size that is not an
*exact* list entry and not the output's own size now classifies straight as the Custom
**aspect**. `ClassifyAspect()` is exactly this rule, extracted pure so
`tests/test_resolution.cpp` can pin "no exact match ⇒ Custom" (including cases the old
tolerance-based rule would have forgiven, like a true-21:9 2520×1080) without faking a live
nested/output global. `CurrentAspect()` in `PanelDisplay.cpp` is now just step 1 above plus a
call into `ClassifyAspect()` for step 2 — `NearestAspect()` itself is untouched and still used
(and tested) on its own terms, it is simply no longer part of this fallback.

So a persisted `nested_width/height` of 1280x960 reopens as Aspect 4:3, size 1280x960, with
no pick stored anywhere; a persisted 1300x975 (same ratio, no exact entry) now reopens as
Aspect **Custom** rather than "4:3 + Custom". Applying and persisting are unchanged: every path
ends in `ApplyNestedMode()` (Phase B below); `nested_*` write `0` for Native as before, keyed on
`s_nAspectChoice == kAspectNative`.

The Live state's **"Game sees" line is gone** (item 9, 2026-09-06: *"For the Live state,
remove the 'Game sees' part, but keep the rest."*) — both the fact and the summary's leading
half. The Xwayland root it read still drives the *area's* rail summary (`ResolutionSummary()`),
which is where a player looks for that number; the facts row keeps `takes effect` and
`applied via`. Labels use a plain `x`, not `×` — the overlay font is not known to carry that
glyph.

**UPDATE, 2026-09-07 (tracker item 4): the Live-state line itself.** The user: *'Use the
terminology "nested" and "output". Make the line like this: "nested
<nested_res>@<nested_refresh> · output <output_res>@<output_refresh>"'*. This replaces the old
`paced at R Hz · window WxH · host R Hz` line, and drops the three sub-facts it used to carry
underneath (`paced at`, `window`, `host refresh`) — the one line already says everything they
said, so keeping both would just be the same three numbers twice. `takes effect` and
`applied via` stay, since the new line doesn't cover them.

- **Why these words.** "nested" and "output" are the actual variable prefixes this feature uses
  everywhere else in the code (`g_nNestedWidth/Height/Refresh`, `g_nOutputWidth/Height/Refresh`)
  — the old line's "paced at"/"window"/"host" wording named the same numbers with three
  different words that agreed with neither each other nor the code. See TERMINOLOGY.md's
  "Nested resolution / refresh" and "Output resolution / refresh" entries.
  Pairing each side's own resolution with its own refresh, rather than the old flat
  `A Hz · B WxH · C Hz`, reads as two facts (what the game sees, what the host granted)
  instead of three loose numbers, and — unlike the old line — includes the nested
  *resolution*, which the old wording dropped entirely.
- **The formatter is pure.** `FormatLiveLine()` (`ResolutionPresets.h`) takes the six numbers
  (nested W/H/Hz, output W/H/Hz, refresh already converted to Hz by the caller) and returns the
  exact string; `tests/test_resolution.cpp` pins the wording directly rather than through a
  live nested/output global.

Embedded (DRM) is a different feature for a later phase — `GetModes()` plus the
dynamic-refresh atom — not a disabled copy of this area.

## Phase B — persistence

Game resolution and refresh survive a restart, and so does the Custom steppers' **Lock
aspect ratio** switch (2026-09-07); there is nothing else in the area to persist.

- **Write-back**: `ApplyNestedMode()` in `PanelDisplay.cpp` — the single write point for all
  three live values — also writes `GamescopeSettings::nested_width/height/refresh_hz` into the
  routed config (`config::EnqueueRoutedWrite()`) every time it runs, `0` meaning "as launched".
  Native resolution and Follow-host refresh both write `0`, not the live pixel size / Hz at the
  moment of the pick — `s_nAspectChoice == kAspectNative` is what tells "Native" apart from
  a Custom pick that happens to match the output size; Follow-host already arrives as `nRefreshmHz
  == 0` from `SetRefreshChoice()`, no extra check needed.
- **Startup apply**: `main.cpp`'s `apply_ritz_config_to_startup_state()` sets
  `g_nNestedWidth/Height` when both `nested_width` and `nested_height` are nonzero, and
  `g_nNestedRefresh` (mHz — converted from the schema's Hz via `ConvertHztomHz()`) when
  `nested_refresh_hz` is nonzero. This runs before the getopt loop in `main()`, so an explicit
  CLI `-w`/`-h`/`-r` overwrites it unconditionally and always wins — the ordering is not
  incidental, it is why this function is called where it is.

### Lock aspect ratio is persisted (`gamescope.nested_lock_aspect`, 2026-09-07)

`scripts/settings-audit.sh` found `display.resolution.width.lock_aspect` to be the one
registered row in the whole overlay with **no config field at all** — a file-static
`s_bLockAspect` in `PanelDisplay.cpp`, session-only by construction, so it silently came
back on at every launch.

`Decision: persist it,` as `GamescopeSettings::nested_lock_aspect` (default `true`), beside
the size it constrains. The argument for leaving it session-only is that it is a UI
convenience rather than a display setting — it changes how the two steppers *behave*, not
what the display does. The argument that won: the user cannot tell those apart from the
outside. It is a switch in the settings overlay, in the same row as the size, drawn beside
values that all persist; a switch that quietly resets every launch reads as a bug, not as a
category distinction. Cost is one bool in the schema and one already-existing sparse-diff
key, against a row that would otherwise have to be re-set every session by anyone who wants
the axes independent.

`What is NOT persisted:` the captured reference pair (`s_nLockRefWidth/Height`) the ratio is
derived from. That stays session-local and is re-derived lazily on the first edit after a
launch (`EnsureLockedAspectReference()`, which fires because `PickStillLive()` is false in a
fresh session) — from the *persisted* size, which is the pair on screen, which is exactly
what the switch's own rule says the locked ratio should be. Storing a ratio as well would
have added a second source of truth for a number that is always recoverable from the first.

`Pinned by:` `tests/test_config.cpp`'s *"gamescope.nested_lock_aspect round-trips and
defaults to on"*, and the audit's own row (`results-after-fix.txt`: `on-disk` and
`survives-restart` OK in all three situations, where before it changed nothing on disk).

## Related

- [backend-sdl.md](backend-sdl.md), [backend-wayland.md](backend-wayland.md) — the two
  `RequestOutputSize()` implementations, now with no caller.
- [steamcompmgr-focus.md](steamcompmgr-focus.md) — the force-resize the mode change relies on.
- [../planning/requests-2026-09-05.md](../planning/requests-2026-09-05.md) — item 7 scouting.
- [../planning/requests-2026-09-06.md](../planning/requests-2026-09-06.md) — items 7–10, the
  one-row reshape.
- [../planning/requests-2026-09-07.md](../planning/requests-2026-09-07.md) — items 3–4: Custom
  moves onto the Aspect row only, and the Live-state line's "nested"/"output" wording.
- [../planning/requests-2026-09-08.md](../planning/requests-2026-09-08.md) — item 3: Force
  maximize nested window.
