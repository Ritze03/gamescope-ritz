# True Sens

The player records their own mouse sensitivity once, as a hand distance per 360°
turn (a **Low** and a **High** one), and gets the same feel in every game through a
per-game multiplier on mouse movement.

`src/Overlay/TrueSens.{h,cpp}` (the pure maths and recorder, the mouse hook, the
per-frame tick, the interface the settings area calls and the debug commands).
Storage in `src/Config/` (`TrueSensGlobal`, `GameTrueSens`, global.json only).
Unit cases `tests/test_truesens.cpp` and the `true sens:` cases in
`tests/test_config.cpp`.

> **Status (2026-11-25): the core only (step I1 of 5).** Maths, recorder, storage,
> the hook, the tick, the interface and the debug commands exist and are tested.
> There is no settings area yet (I2), no on-screen hint (I3) and no reference
> picture (I4); until then the feature is driven with the debug commands below.

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
3. `bCursorHidden = !bCursorHasImage || wlserver_pointer_is_locked()` (hoisted so Zoom and
   True Sens share it)
4. **`TrueSens_OnMotion`** (this feature)
5. `Zoom_MouseScale( bCursorHidden )` (Match mouse speed)
6. `wlserver_perform_rel_pointer_motion( dx, dy )`

So the multiplier acts on what the game would otherwise receive and Zoom's own scaling
divides on top (D15; `zoom.md`, "Match mouse speed").

Inside: if a recording runs and the cursor is hidden, the unscaled delta is added to the
accumulators and **not scaled** (D11); else if the feature is enabled for this game, a valid
multiplier exists and the cursor is hidden, dx and dy are scaled by the cached multiplier
through the fraction carry; otherwise the delta is left alone and the carry is reset.

`Why bCursorHidden and not wlserver.bCursorHidden:` the latter is also set by gamescope's
idle auto-hide, which would false-positive on a menu with a visible-but-idle cursor (the
same reason Zoom's "Only while the cursor is hidden" avoids it).

Only `wlserver_mousemotion()` is hooked. The absolute path (`wlserver_mousewarp`,
`wlserver_touchmotion`) is untouched: while the cursor is visible in a window, motion
arrives as absolute positions and is correctly never scaled.

**Fractions (D19).** The scaled delta is quantised to 1/256 (the `wl_fixed` wire
resolution) by `FractionCarry`; what the quantising cuts off is carried into the next
movement per axis, and the carry is reset when the multiplier changes or the feature goes
inactive. Over any run of moves the sum of outputs equals the sum of inputs times the
multiplier within one quantum per axis, so a 0.05x multiplier loses no movement. Whether a
game's own input stack (Wine raw input, for instance) keeps sub-integer deltas is not
verified; an integer mode is a later option if a game proves to need it.

## Threading

The hook runs on the input thread and reads/writes only atomics (`s_eTarget`, the sums,
`s_dMultiplier`, `s_bScale`) plus its own `FractionCarry`: no config, no allocation, no
locks. `ConfigManager`'s mirror (`s_Assignments`) has no lock and belongs to the compositor
thread, so it is never touched from the hook.

`TrueSens_Tick()` (once a frame from `paint_all()`, beside `NullBinds_Tick()`) resolves the
key, loads the values on the first call and refreshes the atomics, so an enabled game entry
already in `global.json` gives its multiplier from the first frame without the settings
window ever being opened. Every setter writes through `ConfigManager` (coalesced
background writer, never synchronous disk I/O on the paint thread) and refreshes the atomics
at once. The per-game values are profile-independent, so no `ConfigGeneration()` tracking is
needed.

`TrueSens_StartRecording()`/`StopRecording()` take `wlserver_lock()` around resetting or
reading the sums (unless the caller already holds it), so a motion event is never half
summed; the hook is the single writer.

## The recorder (D11, D12, D14, D15)

`Recorder` (pure) and the glue's atomics sum the **net signed sideways (X) movement** of one
turn while the cursor is hidden and the multiplier is off; the result is its absolute value,
so corrections back and forth cancel. Y and the sample count are kept for diagnostics only
(D3). Rules:

- **Result.** A valid recording of `Low` or `High` stores `low_inches`/`high_inches` =
  counts / DPI **and** the current game's `counts_360` = counts (D12: same measurement). A
  `Game` recording stores only `counts_360`. A failed recording changes nothing.
- **Too little movement (D14).** Fewer than 200 net counts (`kMinRecordingCounts`) stores
  nothing and gives a plain reason: no samples at all, "No mouse movement was seen. The game
  has to hold the mouse (cursor hidden) while you turn."; below the floor, "Too little mouse
  movement was seen...". 200 is far under any real turn (thousands of counts) and well over
  a nudge.
- **Zoom (D15).** Starting a recording is refused while `Zoom_IsActive()`; if Zoom becomes
  active during one (the hook notes it per event, the tick covers a chord between events)
  the recording is spoiled: stop reports the reason and stores nothing.
- Counts arriving while the Shell is open are never summed (the capture gate returns first).
- Input emulation (libei, IME, Steam Input) enters the same function and is summed too:
  "what the game receives".

## Limits and what is not supported

- Multiplier limited to **0.05x .. 20x** (D18); a Low/High never recorded, or a game with no
  `counts_360`, gives multiplier **1** (the game is untouched).
- **Games that hide the cursor without locking the mouse are not supported.** Run in a
  window (nested), relative counts reach the program only while the host pointer is locked;
  such a game gets absolute positions instead, so no counts arrive and a recording reports
  "No mouse movement was seen...".
- **Counts are the desktop's values when run in a window.** The host may normalise them
  (libinput scales for mice whose DPI it knows); on the DRM path the deltas are accelerated.
  Recording and playing must use the same mouse; the DPI field is the DPI set on the mouse
  and only matters when it changes.
- A recording must be exactly one 360° turn ending where it started; two turns store double.
- The calculator assumes turn per count proportional to the in-game value. After setting the
  calculated value in the game, switch True Sens off for that game (or record it again),
  otherwise the correction applies twice.

## The interface (compositor thread)

Declared in `TrueSens.h`, for the settings area, hint and picture of the later steps:
recording state (`TrueSens_RecordingActive`, `_RecordingTarget`, `_RecordingNet`,
`_LastOutcome`), `TrueSens_StartRecording( Target )` / `TrueSens_StopRecording()`, this
game's `Get/SetEnabled`, `Get/SetUseHigh`, `Get/SetIngameSens`, `GameCounts360`, the global
`Get/SetDpi`, `LowInches`, `HighInches`, and the derived read-only `Multiplier`,
`CalcIngameSens`, `LowText`, `HighText`, `GameText`, `MultiplierText`, `SessionKey`,
`SessionKeyShared`.

## Debug commands

Reached with `gamescopectl`, for headless tests (no area exists yet). They queue their
text and `TrueSens_Tick()` runs it on the compositor thread (a ConCommand runs on the
wlserver thread, which must not touch the config mirror), then prints the status:

| Command | Effect |
| --- | --- |
| `true_sens_record <low\|high\|game>` | Start a recording (refused with a reason while Zoom is active or one runs). |
| `true_sens_stop` | Stop, store the result or log why nothing was stored. |
| `true_sens_set "<field> <value>"` | `enabled 0\|1`, `profile low\|high`, `dpi N`, `ingame X`. One quoted argument through `gamescopectl`. |
| `true_sens_status` | Three `true_sens:` log lines: key and settings, multiplier and calculator, recording state and last outcome. |

Inject movement with `wlserver_debug_mouse_motion "<dx> <dy> [count]"` (enters at
`wlserver_mousemotion`).

## Area (I2)

Not built yet.

## Recording hint (I3)

Not built yet.

## Reference picture (I4)

Not built yet.

## Tests

`tests/test_truesens.cpp` (`[truesens]`, header only): the multiplier (valid, every invalid
input, both limits), the calculator, conversions, the recorder (net with back and forth, Y
ignored, below the floor refused with a reason, spoiled by Zoom, restart), the fraction carry
(sum property for 0.05, 0.37, 1, 2.5 and 20, no lost small moves, reset) and the text
helpers. `tests/test_config.cpp` (`true sens:`): round trip of the global values and a game
entry, an entry with only True Sens values is written and defaults are not, clamps and an
unknown `profile` string, the shared key, and that no profile file contains `true_sens`.
