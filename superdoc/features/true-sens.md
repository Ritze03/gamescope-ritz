# True Sens

The player records their own mouse sensitivity once, as a hand distance per 360°
turn (a **Low** and a **High** one), and gets the same feel in every game through a
per-game multiplier on mouse movement.

`src/Overlay/TrueSens.{h,cpp}` (the pure maths and recorder, the mouse hook, the
per-frame tick, the interface the settings area calls and the debug commands).
Storage in `src/Config/` (`TrueSensGlobal`, `GameTrueSens`, global.json only).
Unit cases `tests/test_truesens.cpp` and the `true sens:` cases in
`tests/test_config.cpp`.

> **Status (2026-11-25):** maths, the two-click recorder, storage, the hooks, the tick, the interface, the
> debug commands, the Shell area, the hint at the top of the screen and the picture pair
> under it (frozen left half, live right half) exist and are tested.

## Why this exists

The owner's request (the feature request pasted 2026-11-25), verbatim where it fixes
a design choice:

- *"Store two global values, which are the "true sensitivity" of the player. One for
  "Low" and one for "High". These should be stored globally."*
- *"All of the values for this should be game specific and fully independent of the
  selected profile."* and, for a session where no game is known, *"shared unknown game
  entry."*
- *"Also let the player set a DPI, so he can change that, if he changes his DPI on the
  mouse (ignore that mice arent precise, when it comes to dpi's)"*
- *"The sensitivity multiplier should only be applied, when the cursor is not visible at
  the moment (to detect ingame from inventories)"*
- *"It should only calibrate the vertical sensitivity"*: read as: a 360° turn is sideways
  movement, so only that is measured, and the one multiplier is applied to both mouse
  directions (the lead's reading, confirmed).

**Why a multiplier on movement.** Every game turns a fixed number of mouse counts per
360° at its in-game setting. If the player's own comfortable turn is a fixed hand
distance, scaling the counts by `game counts / (distance x DPI)` makes the turn the
same hand distance in every game, with no need to find the right in-game number.

## The maths

All pure, in `gamescope::truesens` (`TrueSens.h`):

| Name | Meaning |
| --- | --- |
| `counts_360` | Mouse counts for one 360° turn at the game's in-game setting, measured with the multiplier off (per game). |
| `true_inches` | The player's hand distance per 360° = counts / DPI at recording time (global, Low and High). Shown as cm (x 2.54). |
| `TargetCounts` | `true_inches x dpi`: the counts one turn should take. |
| `Multiplier( game_counts, true_inches, dpi )` | `game_counts / (true_inches x dpi)`. **1.0** for any invalid input (0, negative, non-finite, no DPI); otherwise limited to **0.05x .. 20x** (D18). |
| `IngameSensForTarget( typed, game_counts, true_inches, dpi )` | The calculator: `typed x game_counts / target_counts`, nullopt for invalid input, not limited. Assumes the game's turn per count is proportional to its sensitivity value (true for most games, not all). |

`Why a distance, not counts:` the stored Low/High are `counts / DPI`, so changing the DPI
needs no new recording; only the target counts change. With one mouse only the ratio
matters, so a wrong DPI does no harm until the player changes it (default 800, limited to
100..32000).

`Why validation by range and a bit-pattern finiteness test:` the build uses `-ffast-math`,
which lets the compiler assume no NaN exists and delete a `std::isnan()` branch (a previous
worker found a unit test failing on exactly that). `IsFinite()` reads the exponent bits
instead, and everything else is `!( x > 0 )` style. The unit cases build their NaN and
infinity from raw bits for the same reason.

## Data model

Global (not per profile, not per game), `global.json` top-level `true_sens`, in memory
`ProfileAssignments::true_sens` (`config::TrueSensGlobal`):

| Key | Type | Default | Notes |
| --- | --- | --- | --- |
| `dpi` | int | 800 | Clamped 100..32000 on read and write. |
| `low_inches` | float | 0 (not recorded) | Hand distance per 360°, inches. Clamped 0..1000. |
| `high_inches` | float | 0 | Same. |

Per game, `profiles.games.<key>.true_sens` (`config::GameTrueSens`, a member of
`GameAssignment`):

| Key | Type | Default | Notes |
| --- | --- | --- | --- |
| `enabled` | bool | false | The per-game switch. |
| `counts_360` | float | 0 (not recorded) | Counts per turn at the game's setting, multiplier off. Clamped 0..10M. |
| `profile` | `"low"` / `"high"` | `"low"` | Which true sensitivity this game uses. An unknown string loads `"low"`. |
| `ingame_sens` | float | 0 (unset) | The calculator's typed in-game value, stored so the area only needs a row. |

```json
{
  "true_sens": { "dpi": 800, "low_inches": 12.5, "high_inches": 3.25 },
  "profiles": {
    "last_general": "Default",
    "games": {
      "252490": {
        "selected": "", "audio_node": "",
        "true_sens": { "enabled": true, "counts_360": 5000.0, "profile": "low", "ingame_sens": 0.8 }
      }
    }
  }
}
```

The `true_sens` object is written only once a value differs from its default; a game entry
whose True Sens values are all defaults (and that says nothing else) is not written
(`AssignmentsToJson`'s "says nothing" test includes `GameTrueSens::IsDefault()`). No schema
bump: absent means default, as for every additive key (see `config-system.md`).

**Out of every profile.** These values live outside the `Settings` sections, so a profile
file, a settings export/import (`profile-export.md`) and profile inheritance never carry
them. A unit case asserts no profile file contains `true_sens`.

### The key, and the shared unknown-game entry (D2)

The per-game key is `config::SessionTrueSensKey()`: `SessionGameVolumeKey()` (the app id,
else the launched program's basename, see `profiles.md`), and when neither is known the one
shared key `kSharedTrueSensKey` = `"/unknown-game"` for every unidentified session.

`Why that string:` it starts with `/`, which `SanitizeAppId()` rejects and a basename cannot
contain, so it can never collide with a real app id or program. Checked that nothing treats
it as a game for profile assignment: the only readers of `games` are lookups **by app id**
(`ResolveAssignedProfile`, `SelectProfile`), the rename/delete loops (they only touch
`selected`, which the shared entry never has), the schema-2 migration (builds entries from
`games/<AppId>.json` files) and the key-by-key accessors; the Profiles list is built from
profile files, not from the `games` keys.

## The hook

`gamescope::TrueSens_OnMotion( double &dx, double &dy, bool bCursorHidden )`, called from
`wlserver_mousemotion()` (`src/wlserver.cpp`) on the input thread with `wlserver_lock()`
held. The order of everything that scales or gates a relative mouse delta:

1. `dx/dy *= g_mouseSensitivity` (`--mouse-sensitivity`)
2. the settings overlay's capture gate (the Shell swallows motion; **returns**, so neither
   True Sens nor Zoom sees it)
3. `bCursorHidden = !bCursorHasImage || wlserver_pointer_is_locked()` (already computed there
   for Zoom; True Sens shares it)
4. **`TrueSens_OnMotion`** (this feature): multiplies by the game's multiplier as doubles and
   returns whether it scaled
5. `Zoom_MouseScale( bCursorHidden )` (Match mouse speed): multiplies by `1 / factor`
6. **`TrueSens_CarryWholeCounts( dx, dy, scaled || zoom != 1 )`** (D26): the one whole-count
   carry, the last step of the chain
7. `wlserver_perform_rel_pointer_motion( dx, dy )`

So the multiplier acts on what the game would otherwise receive and Zoom's own scaling
divides on top (D15; `zoom.md`, "Match mouse speed").

Inside step 4: while a recording is armed or running nothing is scaled (D11, D24; the carry is
dropped) and, only in the **recording** phase (between the two clicks) with the cursor hidden,
the unscaled delta is added to the accumulators; movement while armed is never summed. Else if the feature is
enabled for this game, a valid multiplier exists and the cursor is hidden, dx and dy are
multiplied by the cached multiplier; otherwise the delta is left alone.

**Dead band.** `truesens::Multiplier()` returns exactly 1.0 when the result is within
`kUnityBand` (0.001) of 1, so the hook's `s_bScale` stays off: a game recorded at the player's
own sensitivity is passed through untouched (no whole-count rounding, fractional deltas stay
fractional) and the multiplier row shows "1.00x". With Zoom's scale also 1 the delta is
byte-identical to True Sens off (checked headless: 50 fractional events 0.1234 / 0.0567 arrive
the same with the switch on at 1.00x and off). `Why:` rounding for a factor that is 1 within
rounding gains nothing.

`Why bCursorHidden and not wlserver.bCursorHidden:` the latter is also set by gamescope's
idle auto-hide, which would false-positive on a menu with a visible-but-idle cursor (the
same reason Zoom's "Only while the cursor is hidden" avoids it).

### The button hook (recording clicks, D24)

`gamescope::TrueSens_OnMousePress( bool bLeft, bool bCursorHidden )`, called from
`wlserver_dispatch_mouse_button()` (`src/wlserver.cpp`), the single overlay-vs-game fork, on
its **game branch only**, on the input thread with `wlserver_lock()` held. A click the Shell
or the Launcher captures never reaches it. The order on that branch:

1. `wlserver_ritz_mouse_hotkey( button, true )`: the keybind engine sees every press first,
   as before. If it swallows the press (Zoom's "Keep the button from the game"), that wins
   and True Sens is not asked: the click was Zoom's.
2. **`TrueSens_OnMousePress()`**: only for a press of `BTN_LEFT`, with
   `bCursorHidden = !wlserver.bCursorHasImage || wlserver_pointer_is_locked()` (the motion
   hook's condition), and not the Autoclicker's synthetic press
   (`Autoclicker_IsInjecting()`, read inside). While armed the press starts the recording,
   while recording it ends it; both return true: the press is put into
   `s_setSwallowedButtons` (Zoom's mechanism) and never forwarded.
3. otherwise the press goes to the seat as before.

`Why the keybind check first:` it must see every press anyway so its held-button state stays
true (the release path of a swallowed press also tells it). A left click bound to a chord
therefore still fires its action during a recording click (Zoom on a left-click chord then
spoils the recording, which the recorder reports); True Sens does not hide a click from the
keybind engine, it only hides it from the game.

**The release of a swallowed click.** The release branch looks at
`s_setMouseButtonsForwardedToOverlay` first (the press went to the game branch, so it is not
in there), then `s_setSwallowedButtons`, which holds it: the release is dropped (the keybind
engine still hears it). This does not depend on what the Shell did meanwhile: the second
click opens the Shell from the tick, and its release arrives after that, still matched by the
set, so it reaches neither the game nor the Shell as an unmatched release (checked headless:
the second click's release, delivered with the Shell open, is dropped and nothing stays held).

Only `wlserver_mousemotion()` is hooked for motion. The absolute path (`wlserver_mousewarp`,
`wlserver_touchmotion`) is untouched: while the cursor is visible in a window, motion
arrives as absolute positions and is correctly never scaled.

### Whole counts and the carry (D26, replaces D19)

The owner (2026-11-25): *"At some sensitivities, it takes a minimum speed to actually move
the mouse ingame. See if you can fix that through accumulation or something."*

**The mechanism (confirmed headless, 2026-11-25).** D19 kept the fractions: the scaled delta
was quantised to the `wl_fixed` grid (1/256 count) and only that remainder was carried. So at
a multiplier of 0.3 a hand moving one count per event delivered events of 0.3 (0.300781 on the
wire). Xwayland passes them on as XInput2 raw-motion valuators, which are doubles, so a
consumer that reads them as doubles (SDL3 does) keeps every fraction. A consumer that takes
whole counts per event (an engine with integer deltas, Wine's raw input path, any code that
casts) cuts or rounds each 0.3 to 0: **nothing moves until the hand is fast enough that one
event carries a whole count after scaling**, the owner's "minimum speed". Measured with the
test client (`tests/pointer_grab_client_x11.c`, `DRAW` / `SUMS` lines) on the build before the
change, 0.3x, 100 events of 1 count: the raw doubles summed to 30.0, an integer consumer that
truncates each event got **0**, one that rounds each event got **0**; 20 events of 100 gave 600
for both (fast movement was fine). The same happened to Zoom's Match mouse speed (see
`zoom.md`), and above 1 a rounding consumer got the wrong total (0.5x of 1 count rounds to 1:
twice the movement). A game that reads the doubles was never affected; what real games do is
not verified here (see "Limits").

**The rule.** The scaled movement is handed on in **whole counts** and the remainder is carried
into the next movement, per axis: `acc += scaled; out = trunc( acc ); acc -= out`
(`truesens::WholeCountCarry`). For any inputs, any multiplier and any change of multiplier,
`| sum(out) - sum(scaled in) | < 1` per axis at every point, so no movement is lost over time
and a slow hand arrives: 100 events of 1 count give 30 at 0.3x and 5 at 0.05x. Above 1 the
outputs are whole counts too (2.5x of 1 count alternates 2, 3, 2, 3).

`Why truncation toward zero and not round-to-nearest:` a count leaves only once a full count
has accrued, and a change of direction cannot emit one early. A wobble of +2 -2 at 0.3x nets
zero: truncation emits nothing, rounding emits +1 -1 every cycle (a jitter of the aim from
movement that cancels; a unit case counts 0 against 200 for 100 cycles). Both rules satisfy
the sum invariant; only truncation has no jitter. The cost is up to one count of latency for
the last fraction, which the carry returns with the next movement.

`Why a 1e-9 tolerance:` 100 x 0.3 sums to a hair under 30 in binary floating point; without it
the 30th count would arrive one event late. 1e-9 of a count is far below any real movement and
keeps the carry strictly under one.

**Where it sits: at the END of the chain, covering Zoom.** Zoom multiplies by `1 / factor`
after True Sens, so whole counts out of True Sens would become fractions again while zoomed,
and slow aiming while zoomed is where it matters most. So True Sens and Zoom scale as doubles
and ONE carry runs as the last step before `wlserver_perform_rel_pointer_motion()`
(`wlserver.cpp`, `wlserver_mousemotion()`). It is active when at least one of the two scaled
this event (True Sens returned true, or `Zoom_MouseScale()` returned something other than
1.0). When neither did, the delta passes through bit for bit: the host can deliver fractional
deltas (the nested backend's unaccelerated motion is `wl_fixed`), and those are never rounded.
The recorder sums its input before any of this, so recordings are unchanged.

**Carry reset.** The carry is dropped whenever scaling is not active for an event (cursor
visible, True Sens off or no multiplier and Zoom inactive or at 1.0x in its fade) and while a
recording runs (it starts from nothing; a recording spoiled by Zoom stores nothing anyway); while scaling stays active it is kept
across events. It is **not** reset when the factor changes: it is held in OUTPUT counts, so a
different multiplier or Zoom's per-frame factor during its fade needs no reset and loses
nothing (a unit case ramps 1.0 to 0.25 over 30 events: within one count of the exact total at
every step). Resetting on every factor change would throw the remainder away every frame.

**Limits of D26.** The first event after scaling starts can deliver nothing (the first count
needs a full count to accrue); at 0.05x that is up to 20 events of 1 count. Input that is not
finite is dropped for the event (0 out, carry untouched).

## Threading

The hooks (motion and button) run on the input thread and read/write only atomics (`s_eTarget`,
`s_iPhase`, the sums, `s_dMultiplier`, `s_bScale`) plus its own `WholeCountCarry` (touched by `TrueSens_OnMotion`'s
recording branch and `TrueSens_CarryWholeCounts` only): no config, no allocation, no
locks. `ConfigManager`'s mirror (`s_Assignments`) has no lock and belongs to the compositor
thread, so it is never touched from the hook.

`TrueSens_Tick()` (once a frame from `paint_all()`, beside `NullBinds_Tick()`) resolves the
key on its first call, loads the values and refreshes the atomics, so an enabled game entry
already in `global.json` gives its multiplier from the first frame without the settings
window ever being opened.

**The session key is resolved once.** `SessionTrueSensKey()` builds strings, so the getters use
the cached key; only a getter called before the first tick resolves one (provisionally), and
the first tick resolves again for good. Nothing can change afterwards: the app id comes from the
environment and is cached by the config, and the launch command is noted once by
`LaunchNestedChildren()` before the paint loop starts. Every setter writes through `ConfigManager` (coalesced
background writer, never synchronous disk I/O on the paint thread) and refreshes the atomics
at once. The per-game values are profile-independent, so no `ConfigGeneration()` tracking is
needed.

**Recording clicks (D24).** One atomic, `s_iPhase` (`truesens::Phase`: Idle, Armed, Recording,
Finished), is the whole hand-over. The button hook advances Armed -> Recording (zeroing the
sums right there: the hook owns them while a recording is active) and Recording -> Finished
and calls `force_repaint()`; it does nothing else (no picture, no config, no logging, no
allocation). Everything that needs the compositor thread happens in `TrueSens_Tick()` when it
sees the phase: **Recording** (first time) -> `TrueSens_ReferenceRequest()` (the capture must
be queued from the compositor thread); **Finished** -> `FinishByClicks()`: under the lock read
the sums and go Idle, `TrueSens_ReferenceClear()`, evaluate and store (`ApplyResult()`), then
`SettingsOverlay_SetVisible( true )` and `ui::shell::RequestArea( "system.true_sens" )` (the
order of the Friends/Mixer keys in `wlserver.cpp`: visible first, then the request). A second
click inside the same frame as the first leaves the tick seeing Finished straight away: the
picture is never requested and the recording ends with "too little movement" (whatever
moved in that frame). A Stop that wins the lock between the click and the tick cancels the
pending result (the tick finds the phase Idle and does nothing).

`TrueSens_StartRecording()`/`StopRecording()` take `wlserver_lock()` **unconditionally** around
changing the phase and the sums, so they exclude the hooks (they run with the lock held) and a
click or motion event is never half applied; the hooks are the single writers while a
recording is active. Only the compositor thread calls them: the Shell's Action callbacks (`RunKeyboard()`,
the pointer path, also what `overlay_e2_key enter` queues into the overlay's own input queue,
drained in the Shell's frame in `paint_all()`) and the debug commands (queued, run by
`TrueSens_Tick()`). The compositor thread never holds the lock there, and no Action can be
reached from the wlserver thread (`overlay_e2_set` has no setter for an Action; the palette
only jumps to one). `Why not wlserver_is_lock_held():` it is a global trylock probe ("somebody
holds the lock"), not "this thread holds it", so with the input thread inside
`wlserver_mousemotion()` it answered "held" and the caller skipped the lock and read the sums
unprotected. The conditional pattern of `resolution-and-refresh.md` is for setters that a
wlserver-thread console command also reaches; these two are not. A wlserver-thread caller would
deadlock (non-recursive mutex), which the header comment states.

## The recorder (D11, D12, D14, D15)

`Recorder` (pure) and the glue's atomics sum the **net signed sideways (X) movement** of one
turn while the cursor is hidden and the multiplier is off; the result is its absolute value,
so corrections back and forth cancel. Y and the sample count are kept for diagnostics only
(D3).

**The flow: two left clicks (D24).** States **idle -> armed -> recording -> idle**. The owner
(2026-11-25): *"Maybe a better design would be closing the UI on pressing record, then waiting
for a left click (top message says so) to get the beginning point and then pressing left click
again (message says that too), to end the 360 (open the gui again automatically)."* (This
replaced "reopen the settings and press Stop recording", the first flow, in which the Stop
button moved around while recording.)

1. A Record button **arms** (`TrueSens_StartRecording( Target )`) and closes the Shell.
   Refused while Zoom is active or a recording is armed/running (the Shell stays open and the
   reason is in "Last recording"). While armed no movement is summed and no picture exists.
2. The **first counting left click** starts the recording: the sums start at zero at that
   moment and the reference picture is taken then.
3. The **second counting left click** ends it: the tick evaluates and stores exactly as
   before (below), then the **Shell opens on the True Sens area**, also after a failed
   evaluation (the reason is in "Last recording").
4. **Stop recording is the cancel**: while armed or running it ends the recording and stores
   nothing; "Last recording" then reads "Recording cancelled. Nothing was stored." in the
   normal tone. The player reaches it by opening the Shell with its key during a recording
   (the Shell captures input, so no click counts and no movement is summed while it is open;
   closing it again without Stop leaves the recording in the state it was in).

What counts as a click: a **press of the left button, from the user, while the game holds the
mouse** (`!bCursorHasImage || pointer locked`, the motion hook's condition), on the game
branch (see "The button hook"). Anything else (right/middle button, a release, a click with a
visible cursor, the Autoclicker's synthetic press) changes nothing and goes where it would go
anyway. `truesens::StepOnButton()` is the pure state machine (unit-tested).

`Why both clicks are kept from the game:` the lead's addition: a shot's recoil or a weapon
swap would move the aim between the two marks. `Why only while the cursor is hidden:` a click
with a visible cursor is the player in a game menu, not a mark; it must neither advance the
recording nor be eaten (a game that needs a left click to grab the mouse is a limit, see
below). `Why the second click opens the Shell by itself:` the owner's wording; the result is
what the player wants to see next.

The rules of the result:

- **Result.** A valid recording of `Low` or `High` stores `low_inches`/`high_inches` =
  counts / DPI **and** the current game's `counts_360` = counts (D12: same measurement). A
  `Game` recording stores only `counts_360`. Every valid recording also resets the game's typed
  `ingame_sens` to 0: it belongs to the recording it was typed for, and `old_typed x new_counts /
  target` would be a wrong number, so the calculator asks for the value again. A failed
  recording changes nothing.
- **Too little movement (D14).** Fewer than 200 net counts (`kMinRecordingCounts`) stores
  nothing and gives a plain reason: no samples at all, "No mouse movement was seen. The game
  has to hold the mouse (cursor hidden) while you turn."; below the floor, "Too little mouse
  movement was seen...". 200 is far under any real turn (thousands of counts) and well over
  a nudge.
- **Zoom (D15).** Arming is refused while `Zoom_IsActive()`; if Zoom becomes active while
  armed or running (the hook notes it per event, the tick covers a chord between events) the
  recording is spoiled: the second click reports the reason and stores nothing.
- Counts arriving while the Shell is open are never summed (the capture gate returns first).
- True Sens' own scaling is off the whole time a recording is armed or running (D11); the
  whole-count carry is reset at arming and kept dropped.
- **The outcome carries a flag** (`TrueSens_LastOutcomeWarns()`): failures and refusals are a
  warning, stored / cancelled / none yet are not. The area's tone no longer parses the text.
- Input emulation (libei, IME, Steam Input) enters the same function and is summed too:
  "what the game receives".

## Limits and what is not supported

- Multiplier limited to **0.05x .. 20x** (D18); a Low/High never recorded, or a game with no
  `counts_360`, gives multiplier **1** (the game is untouched).
- **Proof is the test client's integer view, not a game.** The whole-count behaviour (D26) is
  verified with `tests/pointer_grab_client_x11.c`, which reports what an integer-reading
  consumer would get from the same X events; no Wine or native engine was run. Whether a given
  game reads whole counts or doubles is unknown here; whole counts are right for both.
- **Games that hide the cursor without locking the mouse are not supported.** Run in a
  window (nested), relative counts reach the program only while the host pointer is locked;
  such a game gets absolute positions instead, so no counts arrive and a recording reports
  "No mouse movement was seen...".
- **Counts are the desktop's values when run in a window.** The host may normalise them
  (libinput scales for mice whose DPI it knows); on the DRM path the deltas are accelerated.
  Recording and playing must use the same mouse; the DPI field is the DPI set on the mouse
  and only matters when it changes.
- **A game that needs a left click to hold the mouse** (a "click to resume" screen): that click
  happens with the cursor still visible, so it neither counts nor is eaten; the player clicks
  to get into the game first, and then the two recording clicks.
- A left click bound to a keybind chord still fires its action on a recording click (the
  keybind engine is asked first); only the game is spared the click.
- A recording must be exactly one 360° turn ending where it started; two turns store double.
- The calculator assumes turn per count proportional to the in-game value. After setting the
  calculated value in the game, switch True Sens off for that game (or record it again),
  otherwise the correction applies twice.
- Two gamescope instances running at once each rewrite `global.json` from their own copy, so a
  value recorded in one can be overwritten by the other's next write (the same existing
  behaviour as the per-game volume and the selected profile). Low and High are global, so this
  can lose a fresh recording.

## The interface (compositor thread)

Declared in `TrueSens.h`, for the settings area, hint and picture: recording state
(`TrueSens_RecordingActive` true while armed or recording, `_RecordingPhase`, `_RecordingTarget`,
`_RecordingNet`, `_LastOutcome`, `_LastOutcomeWarns`), `TrueSens_StartRecording( Target )`
(arms) / `TrueSens_StopRecording()` (cancels), `TrueSens_OnMousePress()` (the button hook), this
game's `Get/SetEnabled`, `Get/SetUseHigh`, `Get/SetIngameSens`, `GameCounts360`, the global
`Get/SetDpi`, `LowInches`, `HighInches`, and the derived read-only `Multiplier`,
`CalcIngameSens`, `LowText`, `HighText`, `GameText`, `MultiplierText`, `SessionKey`,
`SessionKeyShared`.

## Debug commands

Reached with `gamescopectl`, for headless tests (the area drives the same interface). They queue their
text and `TrueSens_Tick()` runs it on the compositor thread (a ConCommand runs on the
wlserver thread, which must not touch the config mirror), then prints the status:

| Command | Effect |
| --- | --- |
| `true_sens_record <low\|high\|game>` | **Arm** a recording exactly like the button, but leave the Shell alone (refused with a reason while Zoom is active or one is armed/running). |
| `true_sens_stop` | **Cancel** an armed or running recording; nothing is stored. There is deliberately no command that ends one with a result: the clicks do. |
| `true_sens_set "<field> <value>"` | `enabled 0\|1`, `profile low\|high`, `dpi N`, `ingame X`. One quoted argument through `gamescopectl`. |
| `true_sens_status` | Three `true_sens:` log lines: key and settings, multiplier and calculator, recording state (target, phase, net) and last outcome. |

Inject movement with `wlserver_debug_mouse_motion "<dx> <dy> [count]"` (enters at
`wlserver_mousemotion`) and a click with `wlserver_debug_mouse_button "272 1"` then `"272 0"`
(272 = left; enters at `wlserver_mousebutton()` like a real click, so it counts). A whole
recording headless: `true_sens_record low`, click, move, click.

## The area

`src/Overlay/TrueSensArea.cpp` (`TrueSens_RegisterArea`, declared in `TrueSens.h`), area id
`system.true_sens`, label **True Sens**, rail group INPUT after Null binds (D20). Summary
`on` / `off` (this game's switch); badge `per game`, or `shared` while the session has no
identified game (the values live outside every profile, so the default profile badge would
lie). Every row is a registry row: it is in the sheet, the Inspector and (when it has a
setter) the Launcher's search. Row ids are `true_sens.<name>`.

| Group | Row id | Kind | Label | Notes |
| --- | --- | --- | --- | --- |
| Top, full width | `tip` | Note | Tip | The owner's sentence (D8), 2 lines, above both columns. |
| Left, This game | `shared` | Facts | Game | "not identified, shared"; shown only while `TrueSens_SessionKeyShared()` (D2). |
| Left, This game | `enabled` | Switch | True Sens | This game's switch (D7 step 1). |
| Left, This game | `profile` | Choice | Sensitivity | Low / High, segmented (D1, D7 step 4). |
| Left, This game | `record_game` | Action | Record game sensitivity | verb "Record"; greyed while a recording is armed or running. |
| Left, This game | `game_value` | Facts | Game sensitivity | `TrueSens_GameText()`. |
| Left, This game | `multiplier` | Facts | Multiplier | "Off", "Record the game first", "Record Low first" or the value (D21); "1.00x" inside the dead band. |
| Left, Calculator | `ingame` | Text | In-game value | The typed in-game value (D17). |
| Left, Calculator | `calc` | Facts | Set in the game | `TrueSens_CalcIngameSens()` or what is missing (D9). |
| Right, True sensitivity | `dpi` | Stepper | Mouse DPI | 100..32000 (`kMinDpi`..`kMaxDpi`), step 50, default 800, can be typed. |
| Right, True sensitivity | `low` / `high` | Facts | Low / High | `TrueSens_LowText()` / `HighText()`, cm per 360°. |
| Right, True sensitivity | `record_low` / `record_high` | Action | Record Low / Record High | greyed while a recording is armed or running. |
| Right, Recording | `stop` | Action | Stop recording | verb "Stop"; the cancel; enabled only while armed or recording, else greyed ("No recording is running."). |
| Right, Recording | `recording` | Facts | Recording | always shown: "None" / "Low, waiting for the first click" / "Low, 1234 counts so far". |
| Right, Recording | `outcome` | Note | Last recording | `TrueSens_LastOutcome()` in full, 3 lines reserved, orange when nothing was stored; "No recording yet" before the first (see below). |

**Nothing in the area moves while a recording is armed or running (D23).** The owner
(2026-11-25): *"make sure that while recording, the controls in the GUI (especially the stop
recording button) doesnt move around."* Every row exists in every state at the same
position: no row is hidden or added because of the recording state (only `shared` depends on
something else, the session's game, which does not change). While a recording is armed or
running every row except **Stop recording** and **Recording** is **greyed** with the reason
"A recording is running. Stop recording cancels it." (`DisabledUnless( kNotRecording, ... )`
on each row; the Inspector shows the reason); the two Notes (the tip and Last recording) stay
readable and are not greyed. Stop recording is enabled only while armed or recording, else
greyed with "No recording is running.". `Why greyed and not hidden:` hiding the Record rows
and adding Stop (the first design) moved every control, which is what the owner complained
about.

**Record arms and closes the window (D6, D24).** A record Action calls
`TrueSens_StartRecording( target )` and, only when that succeeds,
`SettingsOverlay_SetVisible( false )` (the precedent of the Friends area's Join). The player
aims, left clicks, turns, left clicks again, and the window opens again by itself on this
area with the result (see "The recorder"). A refusal (Zoom active, already recording) keeps
the window open: the core has already put its reason into `TrueSens_LastOutcome()`, so the
**Last recording** row shows it (in the warning tone), with no second mechanism. The Launcher: a record Action is a row
the palette finds, but picking a row there only jumps the Shell to it (`PaletteJump`); an
Action is never run from the palette, so nothing closes twice and the player presses it in
the Shell.

**Row help texts** (plain language, shared by the three Record rows): closes the settings; aim
at a spot you can find again and left click; turn exactly once around to the same spot and
left click again; the settings open again with the result; both clicks are kept from the game
and only count while the game holds the mouse (cursor hidden); Stop recording cancels and
stores nothing.

**The Recording row** always shows a value (`truesens::RecordingStatusText()`): "None",
"Low, waiting for the first click", "Low, 3120 counts so far" (High / Game alike).

**The outcome in full.** One **Note** row (`true_sens.outcome`, Registry kind `Note`, see
[overlay-shell.md](overlay-shell.md#the-note-row)) of `Lines( 3 )`: the core's text, word-wrapped to
the column in the normal label font. The text is one to three sentences, the first what
happened, the next what to do ("Turn Zoom off before recording."); the longest, "Low stored:
... The game's value was stored too." and "No mouse movement was seen. The game has to hold
the mouse (cursor hidden) while you turn.", take two lines at 1920x1080 and also at the
narrowest two-column sheet (UI scale 1.1 at 1920x1080, a sheet of about 850 base units); the
third reserved line is spare for longer numbers and for a narrow one-column sheet. The tone is
orange (`NoteTone::Warn`) only when something went wrong (a failed recording or a refused
start) and the normal text colour for a stored value, for "Recording cancelled. Nothing was
stored." and for "No recording yet": the core says which with `TrueSens_LastOutcomeWarns()`
(the tone used to be guessed from " stored: " in the text). The Note keeps
the **previous** text while a new recording is armed or running and is replaced when the
new result arrives (it used to be emptied at the start); before the first recording it reads
"No recording yet". Its height is fixed by `Lines( 3 )`, so nothing below or beside it moves
with the text. `Why fixed:` the owner's rule above. The first version showed only the first clause, so the player never saw what
to do; the second split the text over up to eight Facts rows of 22 characters (short
right-aligned fragments, every row below moving when the text changed). A text longer than
three lines would end in "..." with the whole text in the Inspector (Configure page, and
Details "NOW").

**Why the multiplier row has its own wording.** `TrueSens_MultiplierText()` says "not
recorded" whenever no multiplier acts, also while the switch is off, which reads wrong
(D21). The row says what is missing, in the order the player fixes it: the game first, then
the chosen Low or High.

**The calculator (D9, D17).** The in-game field is a Text row (no decimal field exists). The
setter does the checking: `0.35` and `0,35` are accepted, spaces ignored, anything that is
not a positive finite number (also `abc`, `-1`, `0`) is refused with the message "Enter a
number above 0" drawn beside the field (`StatusWhen`), and the stored value stays as it was.
An empty field clears the value. `Entry::Validate()` is declared too, but the registry
only stores it: nothing in the Shell reads it, so a rejection cannot rely on it. The result
row is for the game's own chosen Low/High, not a second selector; its help says to switch
True Sens off for the game, or record it again, after setting the value in the game. A valid
recording of the game resets the typed value to 0 (the field is empty and the result row says
"Enter the in-game value"). The refused-entry message belongs to the stored value it was
raised against: `TrueSens_IngameGeneration()` changes with every stored change (valid entry,
recording), and `IngameStatus()` shows the message only while it is unchanged, so the warning
never sits beside a valid field.

**Two columns.** The owner: *"Make the UI page use two collumns."* `Columns( 2 )`, and
`GroupColumn()` pins the groups: left what belongs to this game (This game, then the
Calculator), right what is global and the recording state (True sensitivity: Mouse DPI, Low,
High and the record buttons, then Recording with the outcome). The sheet has two columns from
a base width of 840 (`kTwoColMin`, `Layout.h`), three from 1680; the area asks for two, so a
narrower sheet falls back to one column: checked, 1920x1080 has two columns and 1280x720 has
one (the groups then stack in declaration order: This game, True sensitivity, Recording,
Calculator, the tip still on top). The first version used one column because greedy packing
split the sequence; pinning removes that.

**The tip (D8).** The owner: *"At the top, the player should be told, that setting a low
ingame sensitivity is usually the most reliable way for using true sens in a game."* One
full-width **Note** (`true_sens.tip`, `.FullWidth().Lines( 2 )`) above the columns, in normal
case: "Setting a low in-game sensitivity is usually the most reliable way to use True Sens in a
game." It is one line at 1920x1080, at the narrowest two-column sheet and at 1280x720 (one
column); the second reserved line is for a narrow one-column sheet (a large UI scale), where
the sentence would otherwise end in "..." (the Inspector still shows it whole). `Why` a Note and not group captions: until 2026-11-25 there was no
running-text row, so the sentence was split across the captions of the two columns' first
groups (small, muted, uppercase, five rows apart in the one-column fallback), those groups
lost their real captions and the Low/High rows had to be labelled "Low (true sensitivity)" /
"High (true sensitivity)". The Note restores the captions "This game" and "True sensitivity"
and the plain labels Low and High. The first version showed "Tip: A low in-game sensitivity
works best" with the sentence only in the help, which the owner's wording ("the player should
be told") does not allow.

**Icon.** A ring with a centred dot and an arrowhead on its top rim (a turn), built from the
glyph set's own primitives (`Icons.cpp`).

## Recording hint

While a recording is armed or runs the Shell is closed (D6), so a lasting hint at the top
centre of the output says what is happening: two lines on a dark plate.

- Line 1 (accent colour): `Recording Low sensitivity` / `Recording High sensitivity` /
  `Recording game sensitivity`.
- Line 2 (light text colour) by phase: armed, `Aim at a spot you can find again, then left
  click`; recording, `Turn once around (360°) to the same spot, then left click again`. The
  plate is as wide as its text, so it changes width at the first click. The lines are built
  by the pure `truesens::HintLines( target, phase )` (unit-tested without ImGui); the chord
  parameters of the first flow (the Shell's key, the reserved chord) are gone, since nothing
  names a key any more.

**Where it is drawn: the HUD layer, not a notification.** `TrueSens_DrawHud( pDrawList, w, h,
flYOffset )` in `src/Overlay/TrueSensHud.cpp` is called by `FpsDisplay_AddLayer()` into the HUD
layer's background draw list, beside the crosshair, the readout and the frame generation stats
block (`fps-display.md`). `Why:` a toast dismisses itself after seconds and a recording can last
a minute; the HUD layer is drawn over the game, under the Shell, and frame generation does not
touch it. No second layer, no second ImGui context (layers are limited to 6).

**How it joins the layer.**
- *Predicate:* the layer exists when `TrueSens_RecordingActive()` is true, even with the FPS
  readout, the Crosshair and the stats block all off (`FpsDisplay_AddLayer()`, `bTrueSens`).
- *Redraw:* the text goes through the draw data, so `HashDrawData()` changes on its own when the
  hint appears or its text changes; nothing extra is hashed.
- *Disappearing:* when the recording stops and nothing else is on, the early return in
  `FpsDisplay_AddLayer()` stops submitting the layer (the old texture is simply not used). If
  the readout or Crosshair is on, the draw data loses the hint's vertices, the hash changes and
  the texture is redrawn. Both paths were checked on screen.
- *Repaint:* a static hint needs no animation, but the paint that makes it appear or vanish must
  happen on an idle client too, so `FpsDisplay_AddLayer()` stores the recording state in
  `s_bTrueSensForTimer` and the HUD's repaint timer thread keeps its 500 ms repaints going while
  it is set (the thread otherwise only ticks when the readout or Crosshair is on). `Why:` the
  debug commands run in `TrueSens_Tick()` after the HUD layer of that paint was built.
- *Thread:* the compositor thread, in `paint_all`, like the rest of the interface.

**Look and size.** Plate colour `palette::kSurfaceRgb` at ~88 % opacity, 1 px accent border, 8 px
corner radius (all x scale). Fonts from `Fonts.h`, which the HUD context carries in full:
`Style::Section` (Sans 500) 22 px for line 1, `Style::Label` (Sans 400) 17 px for line 2. Scale
= `clamp( output height / 1080, 0.8, 4 )`, so it reads the same at 1080p and 4K; if the text
would exceed 80 % of the width everything shrinks to fit. Top margin 2.5 % of the height; the plate
is about 27 % of the width at 1080p, far from the centre the player aims at. If the FPS readout is
anchored top-centre the plate starts below the readout's bottom edge (same formula as the stats
block's top-left rule); at any other anchor the readout (default top-right) does not meet it.

**The reference picture pair** is drawn by the same function into the same draw list, directly
below the plate (see "Reference picture"). `TrueSens_DrawHud()` returns the plate's bottom edge
in px (0 = nothing drawn).

Not checked: a real HDR (PQ) client; a 10-bit SDR client was.

## Reference picture

The owner's words: *"In the top, make sure that the "screenshot" is 2:1 ratio (width:height)
and the left side shows a 2x zoomed view of the middle part."* and, correcting the first
reading: *"Two square halves. Left is a screenshot of the captured starting point and right
is a live preview of the middle at a zoomed view (more precise)"* (D25). `Why:` a frozen
picture of the starting point alone leaves the player judging by memory; a live view of the
same part beside it turns "find the spot again" into "turn until the right half matches the
left", and 2x makes a small turn error visible.

**The picture is twice as wide as high, in two square halves of side S.** The left half is the
frozen picture of the starting point, the right half is a live view of the middle of the
screen; both show the same centre square of the game picture, 2x. It appears with the **first
left click** (the recording starts, D24; not at Record: while armed the player is still aiming
and nothing is held yet, so only the hint plate shows). If the frozen capture fails or has not
arrived yet the hint stays alone: no live half without the frozen half (a lone live view has
nothing to compare with). The live request starts in the same paint in which the frozen
picture first appears.

**Geometry: one pure helper, `truesens::ReferencePair( outW, outH, plateBottom, scale )`**
(`TrueSens.h`, unit-tested), used by the HUD drawing AND by the live request, so the halves can
never drift apart. All numbers are whole output pixels:
- S = 25 % of the output height, rounded to an even number (1080p: 270; 720p: 180; 1440p: 360;
  4K: 540). Picture = 2S x S, centred horizontally, top at the plate's bottom + 8 px x scale
  (1080p: x 690..1229, y 98..367). Left half = columns `[x, x+S)`, right half = `[x+S, x+2S)`.
- If the bottom would come closer than 8 % of the height to the screen centre, S shrinks (2:1
  kept, even); wider than the output, S shrinks to fit; below 24 px nothing is drawn.
- Drawn over it by the HUD layer: a border around the whole 2S x S (1 px accent at
  `max( 1, scale )`, like the plate's), a divider of the same colour and width at the first
  column of the right half, and the ring-with-dot centre mark on the centre of EACH half. The
  border covers the outermost pixel row/column, the divider the first column of the right half.

**What both halves show.** The centre square of the game picture whose side is 12.5 % of the
picture's HEIGHT (`kReferenceHeightFraction`), shown at S = 25 % of the OUTPUT height: 2x when
the game picture fills the output height, which is the normal case (same aspect for game and
output, or a game pillarboxed to the full height). Letterboxed or otherwise smaller than the
output, both halves still show the SAME part (the factor is then not exactly 2): the part is
defined in game-picture terms, not output terms. `Why:` the live half had to match the frozen
one by construction; defining both from the game picture's own height makes the frozen crop
(`ReferenceCrop()`: side = captured height / 8, centred) and the live source part the same
thing, with no relation to the output size to get out of step.

**Two paths, and why the live half goes through the composite.**
- *Frozen half:* an in-memory screenshot, below.
- *Live half:* the composite's zoom request (`FrameInfo_t::zoom`, see `zoom.md`, "Where the
  picture is made"), filled by `TrueSens_FillLiveRequest()` (`TrueSensHud.cpp`) in `paint_all()`
  right after `Zoom_FillRequest()` and built by `vulkan_composite()` as layer 1: one dispatch of
  `cs_zoom.comp` over an S x S rectangle, placed at the right half's whole-pixel origin, sampling
  the graded base layer (bundled effects included) bilinearly. `Why not a per-frame screenshot
  loop:` that is a capture and an upload every frame; the composite already has the graded
  base layer and builds the zoom layer in a single dispatch, and the layer follows every repaint
  of the game by itself, so nothing in the HUD hash changes per frame. The HUD layer is drawn
  ABOVE this layer, so the right half is transparent in the HUD: only border, divider and mark.
- The placed request (new `Zoom_t` fields `bPlaced`, `flPlaceX/Y/W/H`, `flSrcCenterX/Y`,
  `flSrcHeightFrac`) names the projection rectangle in output pixels and the source part as
  fractions of the base texture: centre = the frozen crop's centre (so the half-texel offset of
  an odd-sized crop in an even picture is matched exactly), height = the crop's height fraction.
  No outline ring (the HUD border and divider are the only edges: no doubled or mismatched edge),
  no factor, no circle. `bNeedsDestinationBlend` is set only while the live half is requested,
  exactly as Zoom does (the base must be IN the composite).
- *Frame generation:* the projection is built after the frame-generation substitution, so
  the live half shows the generated frame like the rest of the picture (the same as Zoom);
  the frozen half is a real frame. Not checked with generation on.

**It is not Zoom anywhere else.** The request reuses the composite's slot only; nothing of
Zoom's own state is touched:
1. `Zoom_IsActive()` (Zoom's `s_bActive`) stays false, so the recording never spoils itself
   (`s_bZoomSpoiled`); measured `zoom_spoiled=0` through a whole recording.
2. `Zoom_MouseScale()` and `Zoom_FillRequest()`'s `s_flLiveFactor` stay 1: the filler runs after
   `Zoom_FillRequest()` and writes only `FrameInfo_t::zoom`, never a Zoom atomic, so recorded
   counts and the (off while recording) multiplier are unchanged.
3. The click and pointer remapping of `zoom.md` ("Clicks land on what the projector shows")
   reads Zoom's geometry atomics (`s_bGeomValid` ...), written only by `Zoom_FillRequest()` when
   Zoom is wanted; the live half writes none, so no click is remapped.
4. Zoom's scroll-adjust (`s_flFactor`) and fade state (`s_flFadeProgress`) are never read or
   written. The filler skips (guard) if `zoom.bActive` is already set: the two cannot meet, since
   a recording refuses Zoom and Zoom pressed during one spoils it.

**When the composite cannot build the projection** (base not SDR, YCbCr, no base, no free layer
slot or no texture) the right half would be an empty hole. `vulkan_composite()` stores the
outcome of every placed request in `g_nPlacedZoomFailure` (0 built; 1 no usable base or no
texture; 2 no free layer slot) and calls `force_repaint()` when it changes. The HUD reads it each
paint: non-zero means it draws ONLY the frozen square, centred (`ReferenceSingleX()`), with border
and mark, no divider and no empty half, and the filler requests nothing, so the full composite is
no longer forced. It starts optimistic (0, reset by every `TrueSens_ReferenceClear()`), so the
normal case never flickers on the first frame; one `truesens: live view not built (<reason>)`
line per recording. The fallback is sticky for the rest of the recording.

**Capture (the frozen half): an in-memory screenshot.** The tick (once the first click started the recording) calls
`TrueSens_ReferenceRequest()` (`TrueSensHud.cpp`), which queues one
`CScreenshotManager::TakeScreenshot()` of type `BASE_PLANE_ONLY` with the new field
`GamescopeScreenshotInfo::pfnMemory` set (`steamcompmgr_shared.hpp`). `paint_all()` serves it in
the next paint through the ordinary screenshot block (`steamcompmgr.cpp`), which for a focused
game repaints the game window alone at the game's render size (`bRenderSizeScreenshot`), so the
Shell that is just closing, the HUD and the hint are never in it. With `pfnMemory` the block:
- captures 8-bit (`DRM_FORMAT_XRGB8888`, B8G8R8A8) whatever the path's extension would say;
- after `vulkan_wait()` calls `pfnMemory( pixels, w, h, pitch, ns, nullptr )` **synchronously on the
  compositor thread** (the pixels are only valid during the call), then does not start the
  encoder thread;
- on any failure calls it with `pixels == nullptr` and a reason: no focused game window, no free
  capture image, capture failed.

*Side effects of a user screenshot, all skipped:* the PNG/AVIF/NV12 file (and the "Unsupported
extension" error an empty path would give), the encoder thread, the "Screenshot saved" log line,
`gamescope_control_send_screenshot_taken` (the Wayland "screenshot taken" event that Steam turns
into its notification and message; it is sent only when `bWaylandRequested`, which this request
never sets) and the X11 screenshot-atom deletion (only when `bX11PropertyRequested`). The block
has no sound, clipboard or notification code of its own. Checked headless: no file anywhere, no
log line, no toast in the screenshots.

*Limits of the slot:* `CScreenshotManager` holds ONE pending request, so a user screenshot
requested in the same frame as the first click is replaced by it (or the reverse: this capture
is then never served and the hint stays alone). A request is also dropped silently if
`paint_all()` returns early before the block.

**Pixels to screen (frozen half, all on the compositor thread):**
1. `paint_all()` screenshot block: composite into a pooled linear texture, wait.
2. `Deliver()` (`TrueSensHud.cpp`, the callback): accepts only the answer to the newest request
   while a recording runs (a request counter, bumped by every request and by `Clear`, drops stale
   answers); crops with `truesens::ReferenceCrop()` (the centre square, side = height / 8: 135 px
   at 1080p, 270 at 4K, so no reduction is needed at the usual sizes); reduces by a whole factor with a box average
   if the crop is wider than the 768 px texture (`ReferenceDownscale()`); converts to
   RGBA into the texture's CPU pixels (alpha 255; a G of 0 beside non-zero R/B becomes 1, because
   the HUD's inverted colour mode reads G == 0 as its marker); repeats the last column and row
   once (so the filter at the edge never blends in the unused rest of the texture and the picture
   is sampled edge to edge, texel for texel against the live half); keeps the crop's centre and
   height as fractions of the capture for the live request; bumps the generation and calls
   `force_repaint()` (an idle client would not paint again by itself).
3. `TrueSens_DrawHud()` (HUD ImGui context): registers the texture in the current context once,
   queues the upload rectangle (`ImTextureDataQueueUpload`), draws `AddImage`, border, divider
   and marks.
4. `RenderAndSubmit()` in `FpsDisplay.cpp` uploads (the backend's own `vkQueueWaitIdle`).

**Lifetime.** One `ImTextureData` (768 x 768 RGBA, 2.25 MB CPU and GPU) for the whole process,
created by the first picture and reused by every later one as a sub-rectangle upload; it is not
freed when a recording ends. `Why:` the backend only destroys a texture inside a rendered frame
and the HUD layer draws no frame once nothing is on, so a free would hang until the next
recording; one fixed texture cannot grow with the number of recordings. What follows a recording
is `TrueSens_ReferenceClear()` (from the cancel and from the finish, so also failed and Zoom-spoiled ones); arming clears the old one too:
the picture stops being drawn, no live request is made, and a capture still on its way is
forgotten. A new recording clears first, so the old picture never shows under the new hint.
(Checked with the single square before the pair: 20 start/stop cycles headless: RSS
92404 -> 92452 kB, fds 75 -> 75, threads 18 -> 17, 21 pictures delivered; not repeated with the
pair, which adds no allocation of its own beyond the composite's pooled zoom texture.)

**Redraw.** The HUD layer is only re-rendered when `HashDrawData()` changes. `FpsDisplay.cpp`
folds `TrueSens_ReferenceGeneration()` into the hash seed (xor with `Crosshair_RasterGeneration()`
scaled by a large odd constant): a second picture under the same texture id adds no draw command,
so without it the layer would keep the first picture. A change of layout (pair, single, none)
changes the draw data (other rectangles, divider, marks) and so the hash by itself; the live half
is in the composite and needs no hash.

**Where there is no picture.**
- No focused game window (also when the capture or a capture image is unavailable): hint only,
  log `no reference picture: <reason>`. Driven headless only by a temporary forced switch
  (removed): an unmapped X11 window stays focused in gamescope, and with no client at all no paint
  happens, so the debug command never runs.
- HDR content: the capture goes through the same LUT path as a PNG screenshot (the SDR set, since
  the HDR set is chosen only for `.avif`), so it is tone-converted by the existing code and should
  show; the live half is not built for an HDR base (the composite's zoom block skips it), so the
  frozen square is shown alone. **Not checked** with a real PQ client. 8-bit and 10-bit SDR were.
- Frame generation: the frozen repaint is of the game window alone (the real game frame, not a
  generated one); checked with fixed 2x and an animated client: the picture shows a real frame.

**Cost** (headless, 1080p, warm shader cache): the frozen capture inside the paint takes 0.7..1.8 ms
(measured around the screenshot block); the texture upload 0.9..1.4 ms. The first capture of a
process took ~35 ms in two of the runs (pipeline and capture image created on first use) and
~1.1 ms in the others. Once per recording. The live half adds one full composite per paint
(`bNeedsDestinationBlend`) plus one S x S dispatch, only while it is shown; not measured.

**Measured (headless, 1920x1080, a detailed client picture: hashed 3x3 colour blocks, a grid, a
marker of known size):**
- Rectangle measured from the pixels: x 690..1229, y 98..367 (540 x 270), the divider at column
  960, exactly the helper's numbers (S = 270, 2:1). 1280x720: S = 180 at x 460, y 75 (the plate's real bottom
  is 68.4 px there, so y = 75). 1280x380 (a short window that makes the shrink rule apply): S =
  92 instead of 94, the halves still equal.
- Static game picture, mean absolute difference between the two halves (border, divider and
  marks excluded): **0.073** at 1080p (max 1 level out of 255, p99 0.7), 0.078 at 720p, 0.079
  at 1280x400, 0.019 at the shrunk 1280x380. The best of the nine one-pixel shifts is 0 in x and y
  at 1080p (a shift of one pixel gives 2.5..6.7), so the halves are aligned texel for texel and
  the colours (bundled-effects path, graded base vs screenshot path) agree.
- Zoom factor: a 70 px wide marker in the game appears 141 px wide in both halves (2.0x), and its
  42 px red height 84 px.
- Live: after moving the game's marker, the left half changed by 0.000 (mean absolute pixel
  difference between the two screenshots) and the right half by 27.3.
- A normal recording stores the right number (arm, click, +5000, click: `Low stored: 15.9 cm per
  360° (5000 counts at 800 DPI)`); `zoom_spoiled=0` during it, also with Zoom enabled but not held.
  Zoom pressed during a recording gives `zoom_spoiled=1` as before.
- The user's Zoom alone is as before: held at 1080p, a 540 x 540 circle centred with the 1 px
  ring at 2x; the released frame is pixel-identical to the frame before (max difference 0).
- After finish and after cancel the pair is gone and the frame is identical across the next
  screenshots (difference 0.000 in the former pair rectangle); while armed only the plate differs
  from the idle frame (changed pixels inside x 806..1113, y 27..89).
- The single-square fallback, forced by a temporary environment switch in the composite (removed):
  the frozen square alone, centred, with border and mark, no divider; log `live view not built (...)`
  once.

**Not checked:** a real HDR client, a real "no focused window" state, a user screenshot racing
the first click, frame generation on, the composite layer budget actually full.

## Tests

`tests/test_truesens.cpp` (`[truesens]`, header only): the multiplier (valid, every invalid
input, both limits), the calculator, conversions, the recorder (net with back and forth, Y
ignored, below the floor refused with a reason, spoiled by Zoom, restart), the whole-count
carry (the sum invariant at every step and whole outputs for 0.05, 0.3, 0.37, 0.5, 1.25, 2.5 and
20 over slow, burst, back-and-forth, diagonal, fractional and random sequences; slow movement
arrives; 2.5x alternates 2, 3; truncation against rounding on a wobble; a ramping factor; reset
and non-finite input) and the text helpers. `tests/test_config.cpp` (`true sens:`): round trip of the global values and a game
entry, an entry with only True Sens values is written and defaults are not, clamps and an
unknown `profile` string, the shared key, and that no profile file contains `true_sens`.

`truesens: reference picture crop` (the centre square with side = height / 8 at 1080p, 720p, 1440p,
4K, 5:4 and ultrawide, odd sizes, tiny pictures, every size stays inside, `ReferenceDownscale()`)
and `truesens: reference picture pair layout` (`ReferencePair()` at 1080p, 720p, 1440p and 4K, whole
pixels and an even side over a sweep of outputs and plate heights, the pair 2:1 and inside the
output, the shrink rule, the no-room rule, a narrow output, the single square's centring, 2x
against the crop's fraction) cover the pure helpers; capture, upload, drawing and the live half
are covered by the headless screenshots only.

`truesens: recording hint lines` covers `HintLines()` (each target, armed and recording text,
Idle in either argument), `truesens: the Recording row always has a value` covers
`RecordingStatusText()`, and three cases cover `StepOnButton()`, the click state machine: the
two counting clicks, every non-counting event (visible cursor, other button, release,
Autoclicker, in every phase) and a third click. The glue's cancel, the tick and the swallow in
`wlserver.cpp` are covered by the headless runs only. The draw path (`TrueSensHud.cpp`) and
the layer predicate are covered by the headless screenshots only.

The area (`TrueSensArea.cpp`) is not linked into the unit binary (like every area's
`*_RegisterArea`); its pieces are covered by the id/rail census in
`tests/test_overlay_ui.cpp` (area id list, rail-group table, per-group order, icon census)
and by the headless runs run headless: the Shell driven with
`overlay_e2_select` / `overlay_e2_key` / `overlay_e2_set`, screenshots, and the recorder
and calculator numbers read back through `overlay_e2_get` and `global.json`.
