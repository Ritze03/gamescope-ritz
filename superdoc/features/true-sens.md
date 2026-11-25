# True Sens

The player records their own mouse sensitivity once, as a hand distance per 360°
turn (a **Low** and a **High** one), and gets the same feel in every game through a
per-game multiplier on mouse movement.

`src/Overlay/TrueSens.{h,cpp}` (the pure maths and recorder, the mouse hook, the
per-frame tick, the interface the settings area calls and the debug commands).
Storage in `src/Config/` (`TrueSensGlobal`, `GameTrueSens`, global.json only).
Unit cases `tests/test_truesens.cpp` and the `true sens:` cases in
`tests/test_config.cpp`.

> **Status (2026-11-25): the core (I1), the settings area (I2), the recording hint (I3)
> and the reference picture (I4).** Maths, recorder, storage, the hook, the tick, the
> interface, the debug commands, the Shell area, the hint at the top of the screen and the
> frozen picture under it exist and are tested.

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

Reached with `gamescopectl`, for headless tests (the area now drives the same interface). They queue their
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

`src/Overlay/TrueSensArea.cpp` (`TrueSens_RegisterArea`, declared in `TrueSens.h`), area id
`system.true_sens`, label **True Sens**, rail group INPUT after Null binds (D20). Summary
`on` / `off` (this game's switch); badge `per game`, or `shared` while the session has no
identified game (the values live outside every profile, so the default profile badge would
lie). Every row is a registry row: it is in the sheet, the Inspector and (when it has a
setter) the Launcher's search. Row ids are `true_sens.<name>`.

| Group | Row id | Kind | Label | Notes |
| --- | --- | --- | --- | --- |
| Info | `tip` | Facts | Tip | "A low in-game sensitivity works best"; the full D8 sentence is the help. |
| Info | `shared` | Facts | Game | "not identified, shared"; shown only while `TrueSens_SessionKeyShared()` (D2). |
| This game | `enabled` | Switch | True Sens | This game's switch (D7 step 1). |
| This game | `profile` | Choice | Sensitivity | Low / High, segmented (D1, D7 step 4). |
| This game | `record_game` | Action | Record game sensitivity | verb "Record"; hidden while recording. |
| This game | `game_value` | Facts | Game sensitivity | `TrueSens_GameText()`. |
| This game | `multiplier` | Facts | Multiplier | "Off", "Record the game first", "Record Low first" or the value (D21). |
| True sensitivity | `dpi` | Stepper | Mouse DPI | 100..32000 (`kMinDpi`..`kMaxDpi`), step 50, default 800, can be typed. |
| True sensitivity | `low` / `high` | Facts | Low / High | `TrueSens_LowText()` / `HighText()`, cm per 360°. |
| True sensitivity | `record_low` / `record_high` | Action | Record Low / Record High | hidden while recording. |
| Recording | `stop` | Action | Stop recording | verb "Stop"; shown only while recording (D21). |
| Recording | `recording` | Facts | Recording | "Low, 1234 counts so far"; shown only while recording. |
| Recording | `outcome` | Facts | Last recording | Short form of `TrueSens_LastOutcome()`; the full text is a Details line in the Inspector; shown after a recording or a refusal. |
| Calculator | `ingame` | Text | In-game value used | The typed in-game value (D17). |
| Calculator | `calc` | Facts | Set in the game | `TrueSens_CalcIngameSens()` or what is missing (D9). |

**Record closes the window (D6).** A record Action calls `TrueSens_StartRecording( target )`
and, only when that succeeds, `SettingsOverlay_SetVisible( false )` (the precedent of the
Friends area's Join). The player turns in the game, opens the settings again and presses
**Stop recording** (`TrueSens_StopRecording()`); nothing stops by itself. While the window is
open the mouse never reaches the recorder (the hook sits behind the overlay's capture gate).
A refusal (Zoom active, already recording) keeps the window open: the core has already put
its reason into `TrueSens_LastOutcome()`, so the **Last recording** row shows it, with no
second mechanism. The Launcher: a record Action is a row the palette finds, but picking a
row there only jumps the Shell to it (`PaletteJump`); an Action is never run from the
palette, so nothing closes twice and the player presses it in the Shell.

**Why the outcome row is short.** A Facts value is one right-bound line in the control zone,
the core's outcome is one or two sentences. The row shows the first clause ("Low stored: 15.9
cm per 360°", "No mouse movement was seen"), cut at the first parenthesis or full stop, and a
Details line (`Live`) carries the whole text in the Inspector.

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
True Sens off for the game, or record it again, after setting the value in the game.

**Info text (D8).** There is no plain-text row kind, so the tip is a Facts row at the top
with a short value and the full sentence as its help.

**Icon.** A ring with a centred dot and an arrowhead on its top rim (a turn), built from the
glyph set's own primitives (`Icons.cpp`).

## Recording hint (I3)

While a recording runs the Shell is closed (D6), so a lasting hint at the top centre of the
output says what is happening: two lines on a dark plate.

- Line 1 (accent colour): `Recording Low sensitivity` / `Recording High sensitivity` /
  `Recording game sensitivity`.
- Line 2 (light text colour): `Turn once around (360°) to the same spot, then press <chord> and
  Stop recording`. `<chord>` is `keybinds::ChordTextFor( Action::Shell )` (the primary Shell
  chord, not `shell_alt`); when that action is unbound the text names `ReservedChordText()`
  (`Ctrl+Alt+Shift+O`, which always opens the Shell). The lines are built by the pure
  `truesens::HintLines( target, chord, fallback )` (unit-tested without ImGui).

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

**The reference picture** is drawn by the same function into the same draw list, directly below
the plate (see "Reference picture (I4)"). `TrueSens_DrawHud()` returns the plate's bottom edge in
px (0 = nothing drawn).

Not checked: a real HDR (PQ) client; a 10-bit SDR client was.

## Reference picture (I4)

When a recording starts, the middle of the game picture is captured once and shown, frozen,
under the hint plate until the recording ends, so the player can compare it with what is in the
middle of the screen now and find the spot again (D6, D16). Where it cannot be taken the hint
stays alone: no placeholder box, one `truesens:` log line with the reason. A missing picture
never delays or breaks the recording.

**Capture: an in-memory screenshot.** `TrueSens_StartRecording()` (success only) calls
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
requested in the same frame as the Record press is replaced by it (or the reverse: this capture
is then never served and the hint stays alone). A request is also dropped silently if
`paint_all()` returns early before the block.

**Pixels to screen (all on the compositor thread):**
1. `paint_all()` screenshot block: composite into a pooled linear texture, wait.
2. `Deliver()` (`TrueSensHud.cpp`, the callback): accepts only the answer to the newest request
   while a recording runs (a request counter, bumped by every request and by `Clear`, drops stale
   answers); crops with `truesens::ReferenceCrop()`; reduces by a whole factor with a box average
   if the crop is wider than the 768 px texture (`ReferenceDownscale()`, 4K gives x2); converts to
   RGBA into the texture's CPU pixels (alpha 255; a G of 0 beside non-zero R/B becomes 1, because
   the HUD's inverted colour mode reads G == 0 as its marker); bumps the generation and calls
   `force_repaint()` (an idle client would not paint again by itself).
3. `TrueSens_DrawHud()` (HUD ImGui context): registers the texture in the current context once,
   queues the upload rectangle (`ImTextureDataQueueUpload`), draws `AddImage`, border and mark.
4. `RenderAndSubmit()` in `FpsDisplay.cpp` uploads (the backend's own `vkQueueWaitIdle`).

**Crop and placement.** Crop = the centre 25 % x 25 % of the captured picture. Drawn at 25 % x 25 %
of the output (the same scale as the live middle of the screen), centred horizontally, top at the
plate's bottom edge + 8 px x scale. At 1080p that is y 98..368 (34 % of the height). If the bottom
would come closer than 8 % of the height to the screen centre it shrinks keeping the aspect
(`truesens::ReferenceDisplay()`, below 24 px high nothing is drawn). A 1 px accent border
(`max( 1, scale )`, like the plate's) and a ring with a dark halo and a centre dot in the accent
colour mark the picture's centre: the spot the player aimed at. No caption of its own: the plate's
second line already says "to the same spot". Verified at 1280x720, 1920x1080 and 2560x1440.

**Lifetime.** One `ImTextureData` (768 x 768 RGBA, 2.25 MB CPU and GPU) for the whole process,
created by the first picture and reused by every later one as a sub-rectangle upload; it is not
freed when a recording ends. `Why:` the backend only destroys a texture inside a rendered frame
and the HUD layer draws no frame once nothing is on, so a free would hang until the next
recording; one fixed texture cannot grow with the number of recordings. What follows a recording
is `TrueSens_ReferenceClear()` (from `StopRecording()`, so also failed and Zoom-spoiled ones):
the picture stops being drawn and a capture still on its way is forgotten. A new recording clears
first, so the old picture never shows under the new hint. 20 start/stop cycles headless: RSS
92404 -> 92452 kB, fds 75 -> 75, threads 18 -> 17, 21 pictures delivered.

**Redraw.** The HUD layer is only re-rendered when `HashDrawData()` changes. `FpsDisplay.cpp`
folds `TrueSens_ReferenceGeneration()` into the hash seed (xor with `Crosshair_RasterGeneration()`
scaled by a large odd constant): a second picture under the same texture id adds no draw command,
so without it the layer would keep the first picture.

**Where there is no picture.**
- No focused game window (also when the capture or a capture image is unavailable): hint only,
  log `no reference picture: <reason>`. Driven headless only by a temporary forced switch
  (removed): an unmapped X11 window stays focused in gamescope, and with no client at all no paint
  happens, so the debug command never runs.
- HDR content: the capture goes through the same LUT path as a PNG screenshot (the SDR set, since
  the HDR set is chosen only for `.avif`), so it is tone-converted by the existing code and should
  show; **not checked** with a real PQ client. 8-bit and 10-bit SDR were.
- Frame generation: the repaint is of the game window alone (the real game frame, not a generated
  one); checked with fixed 2x and an animated client: the picture shows a real frame.

**Cost** (headless, 1080p, warm shader cache): the capture inside the paint takes 0.7..1.8 ms
(measured around the screenshot block); the texture upload 0.9..1.4 ms. The first capture of a
process took ~35 ms in two of the runs (pipeline and capture image created on first use) and
~1.1 ms in the others. Once per Record press, while the settings window closes.

**Not checked:** a real HDR client, a real "no focused window" state, a user screenshot racing
the Record press.

## Tests

`tests/test_truesens.cpp` (`[truesens]`, header only): the multiplier (valid, every invalid
input, both limits), the calculator, conversions, the recorder (net with back and forth, Y
ignored, below the floor refused with a reason, spoiled by Zoom, restart), the fraction carry
(sum property for 0.05, 0.37, 1, 2.5 and 20, no lost small moves, reset) and the text
helpers. `tests/test_config.cpp` (`true sens:`): round trip of the global values and a game
entry, an entry with only True Sens values is written and defaults are not, clamps and an
unknown `profile` string, the shared key, and that no profile file contains `true_sens`.

`truesens: reference picture crop` (even/odd sizes, centring, tiny pictures, every size stays
inside, `ReferenceDownscale()`) and `truesens: reference picture display size` (1080p and 4K
placement, shrinking when it would reach the centre, no room, wide aspect) cover the pure helpers;
capture, upload and drawing are covered by the headless screenshots only.

`truesens: recording hint lines` covers `HintLines()` (each target, the chord inserted, the
reserved-chord fallback for an empty chord, Idle). The draw path (`TrueSensHud.cpp`) and the
layer predicate are covered by the headless screenshots only.

The area (`TrueSensArea.cpp`) is not linked into the unit binary (like every area's
`*_RegisterArea`); its pieces are covered by the id/rail census in
`tests/test_overlay_ui.cpp` (area id list, rail-group table, per-group order, icon census)
and by the headless runs described in the step's report: the Shell driven with
`overlay_e2_select` / `overlay_e2_key` / `overlay_e2_set`, screenshots, and the recorder
and calculator numbers read back through `overlay_e2_get` and `global.json`.
