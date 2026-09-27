# Null binds (WASD SOCD cleaning)

Two fixed pairs, **A/D** and **W/S**, each independently switchable. While a
pair is enabled, at most one of its two keys is ever let through to the game
at a time: "last-input priority" — of the pair's physically-held keys, the one
pressed most recently wins, and releasing the winner while the other is still
physically held re-presses the other. A **Delay** (0–50 ms) plus a
**Randomize +/-** jitter (0–20 ms) sit between the synthetic release of the
old winner and the synthetic press of the new one on every real switch.

`src/Overlay/NullBinds.{h,cpp}` (the pure engine, the config, the settings
area, the `wlserver_key()` hook and its worker thread). Config
`config::NullBindsSettings` (`src/Config/ConfigSchema.h`, JSON key
`null_binds`, a normal per-profile section like `zoom`/`autoclicker`),
settings area `system.null_binds` ("Null binds", rail MISC group). Default
**off**, and both pairs default **on** once the master switch is.

> **Status (2026-09-27): wired end to end.** `NullBinds_OnKey()` is called
> from the top of both `wlserver_key()` and `wlserver_handle_key()`
> (`src/wlserver.cpp`), the area is registered from `Shell.cpp`, and it sits
> in the rail's MISC group after the autoclicker. Turning the switch on in
> the settings, or shipping it on in a profile, now actually changes what
> the game receives.

## Why this exists, and why a delay and jitter at all

The user's own request, quoting the tool that inspired it
([wasiejen/Free-Snap-Tap](https://github.com/wasiejen/Free-Snap-Tap)):

> "I'll need you to add null binds for WASD. The user should be able to set a
> delay between activating the other key, so between the deactivation of the
> previously held key and the activation of the new key, plus the small
> randomizer just to make it feel a little more natural."

`Why not switch instantly:` Free-Snap-Tap documents its own delay existing
specifically so the resulting input still passes anti-cheat "if delays are
not set too short" — an instant, sub-millisecond release+press on every
direction change is a signature a heuristic can key on. The jitter is purely
cosmetic on top of that ("a little more natural"), not a second anti-cheat
measure.

`Why the settings-area help text warns about anti-cheat at all:` separately
from Free-Snap-Tap's own reasoning, Valve said in August 2024 that it kicks
players from Valve (VAC-secured) CS2 servers whose input looks like
snap-tap/SOCD cleaning, delay or no delay. This module cannot know whether a
given server enforces that, so the master switch's help text says so plainly
and points at this fork's own per-game profiles ([profiles.md](profiles.md))
as the way to keep it off just for a game that enforces it, rather than
everywhere.

## The engine, precisely

`gamescope::nullbinds::Engine` (`NullBinds.h`) is pure: no wlserver, no
ImGui, no `Config/` include, no I/O, no threads of its own. Every side effect
comes back as a `Result` — `emits` (send these now, in order) plus the
engine's current earliest pending deadline (`has_deadline`/`deadline_ms`,
recomputed fresh after every call) — for the caller to actually perform. It
is driven entirely by injected time (`uint64_t` milliseconds, one consistent
monotonic clock across every call) and an injected jitter source
(`JitterFn`, `uniform_int(-jitter_ms, jitter_ms)`; the default is a real
`std::mt19937` draw seeded from `std::random_device`, so production code
wires up nothing and tests inject a deterministic one instead).

Per pair, each key's physical hold state and press order are tracked; the
**desired winner** is whichever held key was pressed most recently (or
neither, if both are up). Whenever the desired winner changes:

- any earlier scheduled press for this pair that hasn't fired yet is
  cancelled first — rapid back-and-forth restarts the debounce rather than
  stacking presses;
- if a key is currently actually sent (down in-game) for this pair, it is
  released **immediately** — the two pair keys are never both down together
  in-game;
- if the pair was genuinely idle before this event (neither key held), the
  new winner is pressed **immediately** too — there was no other key to
  switch away from, so nothing to debounce;
- otherwise (a real switch, in either direction — a press while the other
  was already held, or a release that hands priority to the key still held)
  the new winner's press is scheduled `d = clamp(delay_ms + jitter, 0,
  kMaxDelayMs + kMaxJitterMs)` ms later instead. If `d` computes to 0 (delay
  0, jitter 0, or a jitter draw that clamps to 0), it is sent immediately
  too, right after the release — back-to-back, release first, with no
  wakeup scheduled for zero time.

A key that is **not** the winner produces no output at all: releasing the
currently-losing key (still physically held, never sent) changes nothing
about what is sent — this is the "loser release sends nothing" case.

`SetSettings()` applies new settings and reconciles immediately, in the same
call, whenever a pair's active state (master switch AND that pair's own
switch) changes:

- **going inactive** (feature or that one pair switched off): from this
  instant the two physical keys pass straight through, so `sent` is made to
  match `held` exactly, one key at a time — release any sent key no longer
  held, press any held key not yet sent. This is the whole of the "no stuck
  keys" guarantee on the way out: if both keys happen to be physically held
  at the moment of the toggle, both simply become sent, because with the
  feature off there is nothing left to arbitrate between them.
- **going active** (feature and that pair switched back on): the engine was
  not watching physical events while inactive, so it has no real
  press-recency to resume from. If a stale reconcile left both keys "sent"
  (both were physically held while off), the at-most-one-sent invariant is
  restored by releasing index 1 (D or S) — emitted, not just cleared
  internally, since the game already thinks that key is down — and keeping
  index 0 (A or W): an arbitrary but deterministic tie-break, **not** a
  correctness claim about which was pressed more recently. This path is a
  documented simplification and is not exercised by the test suite.

`tests/test_nullbinds.cpp` drives the engine directly with a fake clock and a
deterministic jitter source and covers: idle press goes out immediately; a
switch releases the old winner immediately and schedules the new one after
`delay`; releasing the winner while the other is held re-presses the other
after `delay`; releasing the loser sends nothing; a pending press is
cancelled (and never fires) if the desired winner changes again before its
deadline; jitter keeps the gap within `[delay-jitter, delay+jitter]` clamped
to `>= 0`; `delay=0, jitter=0` sends the release and press back-to-back;
turning the master switch off while both keys are held forwards both rather
than leaving one stuck; the two pairs are independent; and disabling one pair
leaves the other handled. `tests/test_config.cpp` covers the config
round-trip (every field, including the off-by-default master switch) and
that a stale/out-of-range `delay_ms`/`jitter_ms` is clamped to the slider's
range on load.

## The hook contract, as wired

```cpp
bool NullBinds_OnKey( uint32_t key, bool press, uint32_t time );
bool NullBinds_IsInjecting();
```

Called from the very **top** of `wlserver_key()` **and** of
`wlserver_handle_key()` (both in `src/wlserver.cpp`), on the wlserver
thread, with `wlserver_lock()` **already held** (both callers hold it before
either function's own body runs), and only when `!NullBinds_IsInjecting()`
**and** the settings overlay is not currently capturing the keyboard:

```cpp
if ( !gamescope::NullBinds_IsInjecting()
  && !gamescope::SettingsOverlay_IsCapturingKeyboard()
  && gamescope::NullBinds_OnKey( key, press, time ) )
    return;
```

Returning `true` means the physical event was fully handled here — from
inside this same call, and under the same held lock, `NullBinds_OnKey()` has
already called `wlserver_key()` itself (recursively, **not** re-locking:
the lock is already held) for every immediate synthetic key event the
engine produced. Because the hook sits before `wlserver_key()`'s own
xkb/pressed-array/hotkey/dispatch work and returns early when consumed, each
synthetic call runs that whole normal path on its own, so the client's
keyboard state stays consistent — the game sees clean, individually
dispatched press/release events, never a physical event that silently did
two things at once. Returning `false` means either the key is not part of an
enabled pair, the feature is off, or the overlay is capturing the keyboard;
the caller must process the physical event exactly as if the hook did not
exist.

`Why the guard is checked by the caller, not inside `NullBinds_OnKey()``:
the same shape as `Autoclicker_IsInjecting()`/`wlserver_ritz_mouse_hotkey()`
— a synthetic event re-entering this same hook would be reinterpreted as a
second physical press, so the check has to happen before the hook is even
called, at the one call site that matters.

### Two call sites, not one

`wlserver_key()` is not the only physical-keyboard entry point. The real
hardware keyboard group behind wlroots' **own** libinput backend (DRM/
embedded mode — gamescope owning the display directly with a real keyboard
attached; `wlserver_new_input()` groups every such device into
`wlserver.keyboard_group`) fires `wlserver_handle_key()`, which calls
straight into `wlserver_process_hotkeys()`/`wlserver_dispatch_key()` and
never through `wlserver_key()` at all. Without the same guarded call at its
own top, null binds would work nested (SDL/Wayland backend, the standalone
`LibInputHandler.cpp` context OpenVR/headless use, `InputEmulation.cpp`)
but silently do nothing for a real keyboard in embedded mode — the Steam
Deck's actual own usecase. Consuming there is safe even though it skips
that function's own xkb/hotkey/dispatch work for the real event: the GROUP
keyboard's `xkb_state` is already advanced by wlroots itself before the
listener runs (unlike the virtual keyboard device `wlserver_key()` advances
by hand), and every synthetic emit `NullBinds_OnKey()` produces goes out
through `wlserver_key()` on that same virtual device regardless of which
physical path saw the real key — one consistent synthetic-output path no
matter which entry point triggered it.

## The settings overlay's keyboard capture

Typing "ad" into the palette/Shell must never be delayed or reordered by
this module — the `!SettingsOverlay_IsCapturingKeyboard()` half of the
guard above is what keeps that true: while the overlay owns the keyboard,
A/D/W/S fall through to `wlserver_dispatch_key()` exactly like any other
key, and `NullBinds_OnKey()` is never called at all.

That leaves the engine's own internal model (which keys are physically
held, which are currently sent to the game) stale for whatever happened
during the capture window, and a `Delay` press that was already scheduled
before capture began could otherwise fire *during* it — the worker thread
doesn't know or care that the overlay is now open, and
`wlserver_dispatch_key()` decides overlay-vs-game at **delivery** time, not
at schedule time, so a late-firing synthetic "d" would land as a stray
character in whatever the user is typing.

`NullBinds_Tick()` (below) detects the **edge** where capturing starts and
calls `nullbinds::Engine::OnCaptureStart()`, which:

- releases (emits) any key this engine currently has sent to the game —
  "reconcile the game's sent state" — since the game will not receive any
  further physical event for it until capture ends;
- cancels any in-flight scheduled press outright, without firing it;
- resets both pairs to a clean idle slate (held, sent, desired all
  forgotten).

There is no matching "capture ended" action, and none is needed: the state
`OnCaptureStart()` leaves behind is already the correct starting point, so
the very next real physical press after capture ends is treated exactly
like any other "engine was idle" press (immediate, no debounce) — this is
"resume cleanly" for free. The one documented simplification this does not
chase: a key still physically held **through** the whole capture window
(the user opened the Shell while already holding a movement key and never
let go) has its eventual physical release, after capture ends, silently
swallowed as a no-op (the engine already believes it isn't held) — which
matches the eager release already issued, so the game never ends up with a
key stuck down, only a movement key held into a menu opening stops the
character moving a little early. `tests/test_nullbinds.cpp` covers
`OnCaptureStart()` releasing a sent key and resetting to idle, cancelling a
pending press without ever firing it, and being a no-op with nothing held
or sent.

## Settings loading, without ever opening the Shell

Like Zoom and Autoclicker before it, this module's hot path
(`NullBinds_OnKey()`) never touches `config::` directly — it reads only the
mirrored atomics `EnsureConfigLoaded()`/`Mirror()` keep current. Those two
functions used to run **only** from the settings area's own row callbacks
(getters/setters/`Summary()`), which meant a profile with
`null_binds.enabled=true` did nothing until the Shell was opened at least
once, and a profile switch (a game profile auto-selected at launch, say)
never reached the module at all unless the Shell happened to be redrawn
afterward.

`NullBinds_Tick()` closes that gap: called once a frame from
`steamcompmgr.cpp`, right next to `Zoom_FillRequest()` (same "every frame,
steamcompmgr thread, regardless of whether the Shell has ever been drawn"
cadence Zoom already established for exactly this problem), it calls
`EnsureConfigLoaded()` unconditionally and then checks the capture-edge
described above. `EnsureConfigLoaded()`'s own generation-keyed cache means
this is cheap on every frame where nothing changed (Zoom.cpp/
Autoclicker.cpp's own `config::ConfigGeneration()` comparison) — the actual
config read only happens on the frame a profile load or live edit actually
bumped the generation. The headless end-to-end check below drives this
directly: a profile with `null_binds.enabled=true` written into the config
**before** the process ever starts, with the Shell never opened, and the
very first physical A press is already cleaned.

Threading note: `NullBinds_Tick()` runs on the steamcompmgr thread, the
same thread the settings-overlay ImGui pass (and therefore every settings
row's own getter/setter) already runs on within `paint_all()` — so reading
and writing `s_Settings`/`s_bConfigLoaded` from both places needs no lock of
its own, the same reasoning `Zoom.cpp`'s own comment gives for its identical
setup.

## Threading and locking

`NullBinds_OnKey()` runs on the wlserver thread with `wlserver_lock()`
already held (the same precondition `Autoclicker_OnChord()`/`Zoom_OnChord()`
have), and touches the engine under one short-lived module mutex
(`s_Mutex`). A **delayed** emit (the press that lands after `Delay` +
jitter) is produced by a dedicated worker thread, copying
`Autoclicker.cpp`'s `WaitUntil()`/`Worker()` shape exactly: parked on a
condition variable whenever nothing is pending, paced against the engine's
own reported absolute deadline, created on first use via `std::call_once`
and never joined (`NullBinds_OnKey()` and the worker both need
`wlserver_lock()`, so a join could deadlock against a worker waiting for the
lock the joiner holds — same reasoning as Autoclicker's `EnsureThread()`).

**The one lock order** (copied from `Autoclicker.cpp:43-49`): the wlserver
thread takes `wlserver_lock()` — already held by `wlserver_key()`'s own
caller before `NullBinds_OnKey()` ever runs — and *then* `s_Mutex`, to touch
the engine. Every other caller of the engine (the worker thread, and a
settings row's setter on the UI/render thread applying a config change) takes
`s_Mutex` and `wlserver_lock()` at **disjoint** times, never nested: release
`s_Mutex` before ever calling `wlserver_lock()`/`wlserver_key()`. So
`wlserver_lock -> s_Mutex` is the only nesting that ever happens, from either
direction, and it cannot deadlock.

Timestamps: an immediate emit fired synchronously from inside
`NullBinds_OnKey()` reuses the **triggering event's own** `time` argument
(they all happen at that same instant, and the lock is already held so there
is no reason to call the clock again). A delayed emit from the worker thread
uses `get_time_in_milliseconds()`, exactly as `Autoclicker.cpp`'s `Emit()`
does for `wlserver_mousebutton()`, since there is no original event to reuse
by the time it fires.

`s_bInjecting` is the feedback-loop guard `NullBinds_IsInjecting()` reads —
true only while a NullBinds-produced synthetic key event is inside its own
`wlserver_key()` call, set around each individual emit (both the immediate
ones from `NullBinds_OnKey()` and the delayed ones from the worker thread).

**Config refresh, and why the hot path never touches it.** Like
`Zoom_OnChord()`/`Autoclicker_OnChord()`, `NullBinds_OnKey()` reads only a
mirrored atomic (`s_bEnabled`) and never calls into `config::` — a profile
load is not something a real-time keyboard-input path should ever be able to
block on, and `config::`'s own state is not designed to be read from the
wlserver thread. The settings the engine needs are mirrored from
`config::ResolvedSettings()` into the engine (and `s_bEnabled`) by
`EnsureConfigLoaded()`/`Mirror()`, called both from the settings area's own
row callbacks (getters, setters, `Summary()`) on the steamcompmgr thread
**and**, since 2026-09-27, once a frame from `NullBinds_Tick()` — see
"Settings loading, without ever opening the Shell" above for why the
per-frame call was added (this module no longer shares Autoclicker/Zoom's
"the Shell must be opened first" limitation; those two still do, and fixing
them the same way is future work outside this module's own scope).

## The settings

| Group | Row | Config field | Notes |
| --- | --- | --- | --- |
| Null binds | Enable null binds | `enabled` | Master switch, default **off**. Help text names the CS2/VAC anti-cheat risk and points at per-game profiles. |
| | A / D | `pair_ad` | Apply null binds to A and D. Default **on** (once the master switch is). |
| | W / S | `pair_ws` | Apply null binds to W and S. Default **on**. |
| | Delay | `delay_ms` | 0–50, step 1, unit `ms`, default **5**. Gap between the old key's synthetic release and the new key's synthetic press on a real switch. |
| | Randomize +/- | `jitter_ms` | 0–20, step 1, unit `ms`, default **3**. Added to `delay_ms`, plus or minus, per switch; the actual gap is `clamp(delay_ms + jitter, 0, 50+20)`. |

Every row but the master switch is `DisabledUnless(enabled, "null binds are
off")`.

## Config schema

`config::NullBindsSettings` (`ConfigSchema.h`), JSON key `null_binds`,
loaded/saved by `ConfigManager.cpp` next to `autoclicker`. **Additive, no
schema bump**: every field has a compiled-in default and an older config
simply has no `null_binds` object and resolves to them, the same precedent
`ZoomSettings`/`AutoclickerSettings` set. `delay_ms`/`jitter_ms` are clamped
to `[0, 50]`/`[0, 20]` on load, so a hand-edited or stale config can never
hand the engine an out-of-range value.

```json
"null_binds": {
    "enabled": false,
    "pair_ad": true,
    "pair_ws": true,
    "delay_ms": 5,
    "jitter_ms": 3
}
```
