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

> **Status as landed (2026-09-27): the engine, config and settings area only
> — NOT YET WIRED into `wlserver_key()`.** `NullBinds_OnKey()` exists and is
> fully tested, but nothing in `src/wlserver.cpp` calls it yet; a later step
> adds the guarded call at the top of `wlserver_key()` (see "The hook
> contract" below) and registers the area from `Shell.cpp`. Until that
> lands, turning the switch on in a build that has it changes nothing a game
> sees.

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

## The hook contract

```cpp
bool NullBinds_OnKey( uint32_t key, bool press, uint32_t time );
bool NullBinds_IsInjecting();
```

The wiring step must call this from the very **top** of `wlserver_key()`
(`src/wlserver.cpp`), on the wlserver thread, with `wlserver_lock()`
**already held** (`wlserver_key()` itself asserts that), and only when
`!NullBinds_IsInjecting()`:

```cpp
if ( !gamescope::NullBinds_IsInjecting()
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
enabled pair or the feature is off; `wlserver_key()` must process the
physical event exactly as if the hook did not exist.

`Why the guard is checked by the caller, not inside `NullBinds_OnKey()``:
the same shape as `Autoclicker_IsInjecting()`/`wlserver_ritz_mouse_hotkey()`
— a synthetic event re-entering this same hook would be reinterpreted as a
second physical press, so the check has to happen before the hook is even
called, at the one call site that matters.

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
`EnsureConfigLoaded()`/`Mirror()`, called only from the settings area's own
row callbacks (getters, setters, `Summary()`) on the UI/render thread —
exactly the same generation-keyed caching `Autoclicker.cpp`/`Zoom.cpp` use,
and it shares their same limitation: a profile switch made while the shell
has never been drawn does not reach this module until something touches its
area again. This was not solved here because neither Autoclicker nor Zoom
solve it either; if it needs fixing, it needs fixing for all three at once
(e.g. wiring into `config::SetLiveApplyHook()`), which is out of this
module's own scope.

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
