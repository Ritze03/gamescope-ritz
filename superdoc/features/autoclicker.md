# Autoclicker

A paced train of synthetic mouse clicks, emitted while a key or mouse button is
held (or toggled), at 1–1000 clicks per second, into whichever mouse button the
game should receive. `src/Overlay/Autoclicker.{h,cpp}` (config, settings area,
the wanted flag, the worker thread), `keybinds::Action::Autoclicker`
(`src/Keybinds.cpp`; the chord), the feedback-loop guard in
`wlserver_ritz_mouse_hotkey()` (`src/wlserver.cpp`). Config
`config::AutoclickerSettings` (`src/Config/ConfigSchema.h`, JSON key
`autoclicker`, a normal per-profile section like `zoom`), settings area
`system.autoclicker` ("Autoclicker", in the rail's MISC group after Zoom).
Default **off**.

> **HIDDEN FROM THE UI since 2026-09-22, and that is the current state.** The
> code below all exists and is compiled in, but the area is registered with
> `AvailableWhen([]{ return false; })` (`Autoclicker.cpp`) and the `autoclicker`
> hotkey row is skipped in `PanelKeybinds.cpp`, so there is no rail entry, no
> palette row, no hotkey row and no way in from the shell. `Why:` the click
> train has never been tested against a real game, and an untested input
> injector is not something to hand a user by accident. Everything else — the
> config section, the chord, the worker, the guards, the tests — is live, so
> re-enabling it is deleting those two lines. A config edited by hand still
> works, which is how it gets tested.

The click is not a special path: `Emit()` calls `wlserver_mousebutton()`, the
same entry point every real backend uses (`OpenVRBackend.cpp` brackets its own
synthetic click the identical way), so the event reaches the game exactly where
a real one would — including, without the guard below, back into the keybind
engine that produced it.

Schema-wise the section is **additive**, so there is no schema bump: every field
has a compiled-in default and an older config that simply has no `autoclicker`
object resolves to them — the precedent `ZoomSettings` itself set.

## The settings

| Group | Row | Config field | Notes |
| --- | --- | --- | --- |
| Autoclicker | Enable autoclicker | `enabled` | Master switch, default **off**. The chord does nothing at all while this is off, and turning it off stops a run in progress. |
| | Autoclicker key | — | Read-only Facts row: the `autoclicker` action's chord, `Mouse4` by default. Rebound under **Keybinds**, like every other hotkey ([keybinds.md](keybinds.md)). |
| | Clicks per second | `cps` | 1–1000, step 1, unit `/s`, default **10**. Live: re-read every cycle, so a change takes effect on the next click rather than the next press. |
| | Activation | `mode` | `"hold"` (clicking while the chord is down) or `"toggle"` (press to start, press to stop). Int-backed Choice like `zoom.mode`: `overlay_e2_set autoclicker.mode 1` is Toggle. |
| | Button | `button` | Which button the **game** receives: `"left"` / `"right"` / `"middle"` → `BTN_LEFT` / `BTN_RIGHT` / `BTN_MIDDLE`. Int-backed Choice, default Left. |

Every row but the master switch and the read-only key row is
`DisabledUnless(enabled, "the autoclicker is off")`, so an off autoclicker greys
out its own knobs instead of offering settings that do nothing.

`Why the clicked button is independent of the chord:` clicking `LMB` from a
`Mouse4` chord is the normal case, not the exception — the button you press to
start it is not usually the button you want spammed. The Button row's help text
says so outright ("This is the button the GAME receives; it does not have to be
the key that starts the autoclicker").

`Why 1000 CPS is the ceiling:` at 1000 CPS the half-period is 500 µs, which is
already inside an ordinary kernel's own scheduler jitter — past that the pacing
stops being honest, so the slider ends where the number stops meaning something
rather than at an arbitrary round figure.

## The chord: a held action on `Mouse4`

`keybinds::Action::Autoclicker` (`autoclicker`, "Toggle autoclicker", default
`Mouse4`) is flagged `bHeld`, the same held-action rule the zoom introduced
([keybinds.md](keybinds.md)): it fires on the press that completes the chord as
a *subset* of the held keys (so it works mid-movement), only when not already
down, and the engine reports the release that breaks it
(`KeyResult::bReleased`). A mouse button is never swallowed — the game gets its
click as before, and unlike the zoom there is no `consume_button` opt-in here.

> **Why `Mouse4` by default:** the autoclicker wants a button the game is not
> already using, and the side button is exactly that. It collides with none of
> the other defaults (`RShift`, `Ctrl+Shift+O`, `LCtrl+RShift`,
> `Ctrl+Shift+Tab`, `RMB`) nor with the reserved `Ctrl+Alt+Shift+O`. With the
> master switch off by default the binding is inert until the user opts in.

`wlserver_check_ritz_keybinds()` routes both edges: the completing press calls
`Autoclicker_OnChord( true )`, the breaking release calls it with `false`.
`Autoclicker_OnChord()` is where hold and toggle are resolved, not in the
keybind engine — hold follows the chord (`SetWanted( bPressed )`), toggle flips
on the press and treats the release as a no-op. Same shape as the zoom, so the
engine only ever has to report an edge.

## The threading model

> **Why a dedicated worker thread and not one of the two tick sources already
> here.** The frame/paint loop caps at the display's refresh rate, so it
> structurally cannot reach the 1000 CPS the slider offers — a 240 Hz display
> tops out at 120 CPS with a 50% duty cycle. `wl_event_loop_add_timer()` has
> 1 ms granularity, which at 1000 CPS *is* the entire half-period: zero
> headroom. So the pacing is its own `std::thread`.

One worker for the process, created on the first activation
(`std::call_once`), detached, and parked on a condition variable whenever the
autoclicker is idle — one blocked futex and no wakeups when nothing is
clicking.

`Why it is never started and joined per activation:` `Autoclicker_OnChord()`
runs on the wlserver thread **with `wlserver_lock()` held** (a mouse chord
arrives through `wlserver_mousebutton()`, which asserts that lock), and the
worker takes that same lock for every click — so a join there would deadlock
against a worker waiting for the lock the joiner is holding.

The burst loop is press → sleep half a period → release → sleep half a period,
paced to an **absolute** `steady_clock` deadline:

- `Autoclicker_HalfPeriodNs( cps )` = `500'000'000 / clamp(cps, 1, 1000)` ns. It
  is a pure header-only function with no compositor dependencies, so it is unit
  testable with no link-time dependency on `Autoclicker.cpp` — the same
  arrangement `Zoom_StepFactor()` uses, for the same reason.
- Each deadline is computed from the **previous deadline, never from "now"**, so
  a late wake-up costs that one half-period instead of pushing every later click
  out with it.
- The 50% duty cycle is deliberate: a press and release with no measurable gap
  between them is missed by any game that polls button state rather than reading
  the event stream.
- The sleep is `condition_variable::wait_until()`, not a plain sleep, so a chord
  release wakes the worker immediately. An uninterruptible sleep would leave the
  button down for up to half a period after release — half a **second** at
  1 CPS.

**Lock order.** The wlserver thread takes `wlserver_lock()` and then the
module's `s_Mutex`; the worker takes `s_Mutex` and `wlserver_lock()` at
disjoint times, never nested (`Emit()` is called with `s_Mutex` released). So
`wlserver_lock → s_Mutex` is the only nesting that ever happens, and it cannot
deadlock. The settings the two threads read are mirrored into atomics by the
steamcompmgr thread whenever the config is (re)loaded, exactly as `Zoom.cpp`
does, because `config::` is single-threaded and the wlserver thread is not that
thread.

## The feedback-loop guard, and why it exists

`Autoclicker_IsInjecting()` is true only while the worker is inside its own
`wlserver_mousebutton()` call. `wlserver_ritz_mouse_hotkey()` returns early on
it.

This is not defensive plumbing — without it the feature eats itself. A
synthetic click really does re-enter the keybind engine, because it goes
through the same `wlserver_dispatch_mouse_button()` a real click does. With the
autoclicker's chord bound to `LMB` and its button set to Left, every click it
emits would re-fire the action that is emitting them — or, in toggle mode,
cancel it.

`Why the guard returns BEFORE the held-button bookkeeping, not after:` leaving a
synthetic press in `s_setHeldButtonSyms` would let it complete somebody *else's*
chord on the next real key. The early return is the whole guard, and its
position is the point of it.

`Why here and not at a lower entry point:` there is no lower entry point to
call. The game-path notify is inline in `wlserver_dispatch_mouse_button()`
together with the forwarded-button bookkeeping and the crosshair hook, so
bypassing this function would mean duplicating all three. Two lines here, or a
second copy of that branch that has to be kept in step forever.

## The stuck-button guarantee

A game left holding a mouse button down is the one failure mode here the user
cannot undo from inside the game, so the release is structural rather than a
code path that has to be remembered.

`ButtonHolder` is an RAII holder: the button is down between `Press()` and
`Release()`, and **anything** that leaves the burst scope while it is held emits
the matching release on the way out — the loop ending, the chord letting go
mid-press, an exception. There is no path out of `RunBurst()` that skips it.
`Release()` clears its own state *before* emitting, so a throw out of `Emit()`
cannot make the destructor release the same button twice.

What that covers, concretely:

- **Chord released mid-press** — the condition variable wakes the worker inside
  its half-period sleep, `RunBurst()` returns, and `~ButtonHolder` emits the
  release. Latency is the wake-up, not the remaining half-period.
- **Master switch turned off**, directly or by loading a profile that has it
  off — `EnsureConfigLoaded()` / `PersistAndRepaint()` call `SetWanted(false)`,
  which is the same stop path, rather than leaving it clicking with its own
  settings page saying "off".
- **The settings overlay opens mid-run** — while the overlay is capturing input,
  `wlserver_dispatch_mouse_button()` routes clicks into the overlay's own queue
  instead of the game, so an ungated autoclicker would sit there clicking the
  user's own settings UI. `RunBurst()` gates the **press** on
  `SettingsOverlay_IsCapturingInput()`; the release still runs every cycle, so a
  button that was down when the overlay came up is released on the next
  half-period. The pacing keeps running through the gate, so letting the overlay
  go resumes in rhythm rather than with a burst.

Two limits, both real and both deliberate:

- **Process teardown mid-click emits no release.** The worker is detached and
  outlives every deactivation; there is no stop flag signalled from
  `wlserver_shutdown()`, so a teardown while the button is down cannot run the
  holder's release. Marked `ponytail:` in the source with its upgrade path — a
  stop flag the worker also waits on, which needs a join site that is not under
  `wlserver_lock()`. The compositor and the game are both going down at that
  point anyway.
- **A focus boundary does not stop it.** `wlserver_clear_pressed_hotkeys()`
  releases a held *zoom* (`Zoom_OnChord(false)`) precisely because the release
  that would end it went elsewhere — it does **not** notify the autoclicker, so
  a held run keeps clicking across a focus change until a chord release
  arrives. Deliberate for a toggle (a toggled run is meant to survive), worth
  knowing for a hold.

## Where it is registered

`Autoclicker_RegisterArea()` is called from the Shell's MISC block
(`src/Overlay/UI/Shell.cpp`), next to `Zoom_RegisterArea()`, because both are
things a hotkey does to the game rather than pages you set up once. The rail
grouping is `{ "system.autoclicker", RailGroup::Misc }` in
`src/Overlay/UI/Registry.cpp`, straight after `system.zoom`. The icon
(`src/Overlay/UI/Icons.cpp`) is a mouse seen from above with its left button
filled solid — SPEC 8.0's "only where a fill carries meaning": the identity of
this area is "a button is being held", and a solid quarter is what says that at
12 px where a second outline would just be noise.

Area keywords are broad on purpose (`autoclicker auto click clicker turbo rapid
fire spam macro cps`), so the command palette finds it under whatever the user
calls it.

## Scripting

The rows are ordinary registry rows, so the usual convars reach them:

```
overlay_e2_set autoclicker.enabled 1
overlay_e2_set autoclicker.cps 20
overlay_e2_set autoclicker.mode 1      # Toggle
overlay_e2_set autoclicker.button 1    # Right
overlay_e2_get autoclicker.cps
```
