# E2 — implementation log

What has actually been built in C++, phase by phase, and what a later phase can
rely on. `SPEC.md` and `API.md` remain the contract; this file records where the
code *departed* from them and why, so the next agent does not have to
re-derive it.

Phase plan: `../../AUTONOMOUS-DECISIONS.md` D10. Judgement calls taken during
P1: the same file, D11.

---

## 2026-08-23 — P3 part C, the composite band, Monitor and Log

The last of the area ports, plus the two pieces of debt the earlier parts left.
**`EscapeCount()` reaches 0** — no area is escaped, and `Escape()` itself now has
no call site anywhere in `src/`. Build clean; **67/67** meson tests
(`overlay_ui` 45 → 47, `overlay_atoms` 6 → 8).

Decisions taken without the user: `../../AUTONOMOUS-DECISIONS.md` **D15**.

### `Kind::Composite` — built, and what it is made of

D14.10 recorded that the shell rendered no composite at all: the kind was in the
taxonomy, `Band.cpp` had its geometry from P1, `controls::AnchorGrid()` existed,
and `DrawEntryRow` dropped the whole kind through `default: break`. That is why
P3b downgraded the anchor grid to a nine-option Choice and dropped the accent
hue's gradient — a control that registers correctly and draws nothing is exactly
issues #25 and #68.

| Piece | Where | Notes |
|---|---|---|
| `DrawCompositeBand()` | `Shell.cpp` | Deliberately shaped like `DrawEntryRow` (D15.1). Same fill, hairline, label/value split, disabled handling. |
| `LinesFor()` | `Shell.cpp` | The one answer to "how tall is this declaration". Both y-cursors (sheet, Configure) go through it, so they cannot disagree. |
| `CompositeValue()` | `Shell.cpp` | Clause 2's resolved value. The anchor's margins are read from the **Params**, so there is no second copy to drift. |
| `controls::Rail()` | `Controls.cpp` | A gradient track whose fill is **sampled** from a colour function, never interpolated between endpoints — issue #37's rule, so the strip cannot disagree with the accent it picks. |
| `controls::HueBody()` | `Controls.cpp` | Rail + eight 45° preset swatches. Swatches set the *same* value the rail does; they are shortcuts, not a second setting. |
| `controls::ColorBody()` | `Controls.cpp` | Swatch + L/C/H rails, sharing one `Rail()`. |
| `controls::GraphBody()` | `Controls.cpp` | Read-only sparkline, with the **two** graph conventions made explicit (see below). |
| `palette::ImU32ToOklch()` | `Palette.cpp` | The inverse of `OklchToImU32()`, so the Colour composite edits OKLCH while its binding stays a packed sRGB int. Round-trip test pins the two together. |

Registry changes the band required:

- **`BindingB()` / `DefaultValueB()`**, and `HasDefault`/`IsAtDefault`/
  `ResetToDefault` now read **both** axes. An anchor is one setting whose value
  is a pair; reading one binding made the sheet's D6 edge lie (D15.3).
- **`Entry::Samples()`** — the Graph's value, in exactly `Meter`'s shape
  (`std::function` returning a read, no setter anywhere), because a graph is
  read-only by construction, not by convention.

**Does the anchor grid earn its place now?** Yes, and the mockup's own framing is
why: it is a three-line band that reads as an ordinary row until the eye reaches
the control column, its value line says `bottom-right · 64 / 32` so the margins
never need two steppers on the sheet, and those margins live in Configure as
Params. The original critique's "weirdly placed, almost orphaned" is answered by
the band participating in the same four column lines as every switch above it.

**The 16:9 miniature (`e3-creative`) is a natural later addition, and the band is
why.** Everything that would change is inside one `case` of
`DrawCompositeBand`'s body switch: `Band.cpp` already right-binds a body rect and
guarantees its height, and the two-axis binding already carries "which anchor",
with the margins as Params that a drag would write. A miniature would be a new
`CompositeKind` plus one body function — no change to the band rule, the row
grammar, or any call site. Worth doing; out of scope here.

### `GraphBody` has two axis conventions, and neither is a call site's choice

`nAxisSlots == 0` rolls (newest right-aligned) — the frametime strip.
`nAxisSlots > 0` is a **fixed axis filled from the left**, with the remainder
left blank — issue #40's explicit requirement that a partially-filled 60-second
window must never read as a complete one. Right-aligning a handful of samples
across the full width is precisely what that issue forbade.

### D6's other half — the accent left edge

SPEC §1's row ink budget always gave the state-edge slot three values
(`Accent` / `Accent@45%` / nothing); only the first was ever drawn.
`StateEdgeColor()` is now the single place that decides which, for rows and for
bands. Selection outranks "differs", because the two share the slot and a
selected row has an open Inspector showing its reset link anyway. This closes
D14.8's "still open" note: the sheet can now answer *what have I changed here*
without opening every row in turn.

### Monitor — before and after

**Before** (`FpsDisplay_DrawSettingsPanel`, six tabs, issue #59). 25 controls:

| Tab | Controls |
|---|---|
| General | Show System Monitor · 3×3 placement grid · vertical margin · horizontal margin · module spacing · font size · blend mode · text opacity · backdrop · backdrop opacity · backdrop rounding · backdrop padding |
| FPS | FPS module · "FPS" label · frametime readout · frametime graph · percentile row · FPS colour |
| CPU / GPU / Media | module toggle + colour override, each |
| Statistics | 60s graphs: CPU load, GPU busy, GPU temp, GPU power, frame rate (+ warm-up readout) |

**After** (`FpsDisplay_RegisterArea`, one sheet, six groups). Every one of the 25
is present:

| Group | Rows |
|---|---|
| Monitor | `monitor.enabled` |
| Modules (`N / 7`) | `mod_fps` (+ Param `label` — #73) · `mod_frametime` · `mod_graph` · `mod_pct` · `mod_cpu` · `mod_gpu` · `mod_media` |
| Placement | `monitor.anchor` **Composite(Anchor)** + Params `margin_v`, `margin_h` |
| Appearance | `font_size` · `module_spacing` · `blend_mode` · `text_opacity` · `backdrop` (+ Params `opacity`, `rounding`, `padding`) |
| Module colours | `color_fps` · `color_cpu` · `color_gpu` · `color_media`, each **Composite(Color)** + Param `custom` |
| Diagnostics | `monitor.sampling` (Facts, 5 Live rows) · `monitor.frametime_graph` **Composite(Graph)** |
| Statistics | `stats_collect` · `stats_window` (Facts) · 5 **Composite(Graph)** rows |

Three deliberate re-homings, all in D15: #73's label became a Param so the band's
count keeps meaning *modules*; #29's colours became their own group because a
Composite cannot be a Param; #40's gating became a stated switch.

**#72 (uniform module width), #77 (media-title cap) and #80 (shrink-to-content)
are not settings and did not move.** They are HUD render behaviour in
`MeasureFpsModule()`/`DrawModule()`, above the line this port touched.

**Only the settings half moved.** The HUD keeps its own ImGui context, offscreen
texture and submission path, untouched. The legacy six-tab panel is also kept
**verbatim**, because `SettingsOverlay.cpp` still hosts it under `overlay_e2 0`
and that shell must stay byte-identical; both halves read and write the one
`s_Settings`, so neither can drift into a private copy of the config. Verified by
capture: the HUD under `overlay_e2 1` and under `overlay_e2 0` differ by 0.05% of
pixels in the HUD crop — the frame-rate digits.

### Log — before and after

**Before** (`PanelLog_DrawBody`, two tabs): Gamescope tab · Game tab (with a
"not capturing" explanation) · clipper'd line view with `[scope]` prefixes and
priority colouring · horizontal scroll · stick-to-bottom · **Copy to clipboard**
· line count.

**After** (`PanelLog_RegisterArea`, one area): everything above, plus filters that
did not exist:

| Group | Rows |
|---|---|
| Filter | `log.sources` **Bank** (gamescope, game) · `log.severity` **Bank** (error, warn, info, debug) · `log.filter` **Text** · `log.autoscroll` **Switch** |
| Diagnostics | `log.buffer` (Facts, 3 Live rows) · `log.copy` **Action, disabled** |
| body | `Area::Content()` — the captured text, clipper'd, scope-prefixed, severity-coloured, stick-to-bottom |

The two tabs became the two members of `log.sources` (D7's rule: one setting
whose value is a set). Filter state is **session-only and no config key was
added** — a filter that survived a restart would hide lines for a reason nobody
remembers setting.

### `Area::Content()`

The one genuinely new registry capability, and explicitly not `Escape()` renamed
— see D15.7. It hands the shell **data** (a function returning `ContentLine`s),
never a draw callback, so SPEC §5.2 clause 0 holds: a category file still cannot
place a pixel. A content area still declares ordinary rows, drawn above the body,
so "rows or content" is never the choice.

### Two defects found by running it

- **`Kind::Bank` and `Kind::Text` drew nothing.** Unhandled enumerators in
  `DrawEntryRow`'s switch. Both atoms had existed since P1 with tests; the Log is
  simply the first area to use either, so the taxonomy claimed eleven kinds while
  the shell rendered nine. Wired into `DrawSharedControl` so a promoted Param
  gets them on the same path (SPEC §5.3).
- **`overlay_e2_set overlay.display_scale 2.0` aborted the compositor.**
  `fonts::RebuildAll()` mutates font state and assumes the render thread inside a
  live frame; a registration setter is reachable from the **console thread**, so
  it cleared an atlas mid-draw and the next glyph needing a bake walked a freed
  `Sources` vector. **A/B'd against the P3b merge, which survives it** — the
  faulty call is P3b's; this work made it deterministic. Fixed with
  `fonts::RequestRebuild()` / `PumpRequestedRebuild()`, performed on the render
  thread beside the `ApplyPendingRebuild()` that already exists for the same
  class of problem. Right-aligned text also now falls back to left alignment once
  it no longer fits, because clipping the *head* of a value rendered
  `bottom-right · 64 / 32` as `.ght`.

### Known-open

`overlay_e2_set overlay.display_scale` changes the stored value and re-bakes the
atlas but does **not** re-scale the shell — the slab stays 1560 px where
`Slab::For()` says 2.0× gives 1728. P3b's row, out of this part's scope. The
band's 2.0× geometry is pinned by test instead (`band: the four clauses hold at
every display scale`).

### Can `Escape()` be deleted?

**Yes, now** — `EscapeCount()` is 0 and there is no `.Escape(` call site left in
`src/`. It was left in place deliberately: removing it means deleting
`Escape()`, `EscapeBody()`, `IsEscaped()`, `EscapeCount()`, `Law::Escaped`, the
shell's escape branch in `DrawSheetBody()` and three tests, which is an API
removal rather than an area port. **P5 should do it as one atomic change**; it is
trivially safe at any point from here, since nothing calls it.

---

## 2026-08-23 — P3 part B, Audio and Config, plus the Inspector's scroll

**Status: `audio.mixer` and the Config panel are real registrations.** `EscapeCount()`
**4 → 2** — only Monitor and Log still host a legacy body. The rail is now the **eleven**
items SPEC §8.1 names. No config key, value range, format or setter changed; the legacy
panels still draw unchanged under `overlay_e2 0`.

Judgement calls: `../../AUTONOMOUS-DECISIONS.md` **D14**.

### The Inspector could not scroll — fixed first, deliberately

P3a found it and left it. At 2.0× an entry on the six-parameter budget (adaptive
brightness, D13.4) overflowed the drawer and the last row clipped. Nothing failed; the
pixels just stopped.

The cause was **not** the row grammar. Each body laid out with an absolute `y` seeded from
the *region's* `y0` — a fixed screen coordinate — and nothing ever told ImGui how tall the
result was. So no scroll range existed, and a fixed origin would not have moved if one had.

Both halves are fixed inside the body child, **without a second layout path**:

- the origin is now the **child's own cursor**, from which ImGui has already subtracted the
  scroll offset. The existing absolute arithmetic therefore pans correctly — every row,
  control and hairline derives from it.
- each body **returns its bottom edge**, which becomes a `Dummy`, which is the content size
  ImGui measures a scroll range from.

*Rejected:* rewriting the bodies onto ImGui's cursor with `Dummy`/`SameLine` spacing. That
would leave one layout model in the sheet and another in the Inspector — precisely what
SPEC §5.3 forbids, since a promoted parameter has to land in the sheet unchanged.

`ui::ConfigureRowsHeight()` (`Layout.h`) makes the overflow a **number**, so "this body is
taller than its region" is a comparison a unit test makes with no window open. It is a
deliberate *lower bound* — the help paragraph needs font metrics — which is the safe
direction. Two shared constants (`shelltok::kSectionLine`, `kTitleLine`) replace the bare
`20`s and `24`s so the arithmetic and the drawing cannot drift.

**The margin was 0.8 px.** At 2.0× the six param rows alone came to 736.0 against a 736.8
body — which is exactly why only the *last* row clipped rather than the whole block.

### Dynamic areas: the registry's answer to a row set that changes

Audio is the first area whose rows are **not known when `RegisterAll()` runs**. A row
exists because an application is making a sound right now, and streams appear and disappear
while the overlay is open.

`ui::Area::Rebuilds( generation, builder )` declares such an area. The registry rebuilds it
when the generation moves, **releasing the ids the previous build claimed first**. This is
P1's deferred `Area::Repeat()`, landed — P1 noted it "needs the shell's frame loop to have a
topology-change hook"; `Registry::SyncDynamicAreas()`, called once at the top of `Draw()`,
is that hook.

**Rejected: a fixed pool of N positional slots**, each bound to "whichever stream is at
index *i*" and greyed when there are fewer. It needs no registry change at all — and it is
wrong. Slot identity would be *positional*, so a stream ending shifts every stream below it
up one slot and the slider under the pointer silently starts controlling a different
application. That is not cosmetic; it is the volume of the wrong program moving. Identity
has to come from the stream, so the row id does too: `audio.node.<pipewire-node-id>`.

How the four laws survive:

| Law | How |
|---|---|
| **ID uniqueness** | a rebuild releases before it builds; node ids are unique among live nodes |
| **The Prefix Law** | untouched — a rebuilt Entry mints Params through the same synthesis |
| **The Six Budget** | untouched — checked per Entry as it is built, exactly as at startup |
| **Help is required** | **the one that changes.** `SelfTest()` runs once after `RegisterAll()`, so a row built later never sees it. A rebuild now re-runs the help and prefix checks over its own area. |

**The consequence, stated rather than hidden:** a violation aborts (D11.6) and a rebuild
happens mid-session, so a malformed dynamic row is a **mid-session abort**, not a boot
failure. That is a real widening of when the guillotine can fall. It is acceptable only
because a dynamic row is *generated* — no human types one — so one unit test against a
fabricated stream list covers every row the generator will ever emit. Five do, including
the **empty** stream set, which is the common case at startup and the reason the help hole
needed closing at all: with no streams, the row-building code never runs.

`Law::Dynamic` keeps an area escaped **or** dynamic, never both — the same argument as
`Law::Escaped`.

### The setting inventory, before and after

Nothing was dropped. Prose status lines became `.Live()` facts (D13.5's precedent).

| Legacy panel / tab | Setting | Now | Config key (unchanged) |
|---|---|---|---|
| Audio | Game volume + Mute | `audio.volume` + **1 param** (only when detected) | — (live PipeWire state) |
| Audio | per-stream slider + mute (#36) | `audio.node.<id>` + **1 param**, one per live stream | — |
| Audio | manual picker combo + 2 buttons | `audio.stream`, one Choice; "Automatic" *is* the old clear | `audio.manual_node_binary` |
| Audio | *wpctl-missing / detection / multi-candidate / stale-override prose* | → `audio.server` `.Live()` ×6 | — |
| Audio | *greyed unpinnable combo rows + tooltip* | → `audio.server` `.Live()` "not pinnable" | — |
| Config ▸ Per-Game | Override Global Config | `setup.pergame` ▸ `config.override` | session routing |
| Config ▸ Per-Game | Copy another game's config | `config.copy` + `source` param | — |
| Config ▸ Per-Game | Delete Saved Config + modal | `config.delete`, armed by `Confirm()` | — |
| Config ▸ Per-Game | *app-id / "Editing:" / layer prose* | → **`Area::Badge`** + `config.routing` `.Live()` ×6 | — |
| Config ▸ Per-Game | profile picker + Apply | `setup.profiles` ▸ `profiles.list`, `profiles.apply` | — |
| Config ▸ Per-Game | new name field + Save | `profiles.name` (validated) + `profiles.save` | — |
| Config ▸ Per-Game | *last applied profile* (#43 #10) | → `profiles.facts` `.Live()` | `last_applied_profile` |
| Config ▸ General | Accent hue | `setup.appearance` ▸ `overlay.accent_hue` | `overlay.accent_hue` |
| Config ▸ General | Dock / Display / Notification scale | `overlay.display_scale` + **2 params** | `overlay.*_scale` |
| Config ▸ General | 4 opacity sliders | 4 rows under ▸ Transparency | `overlay.opacity_*` |
| Config ▸ General | Blur / Darkening | 2 rows under ▸ Backdrop | `overlay.background_*` |
| Config ▸ General | *config directory readout* | → `overlay.appearance_facts` `.Live()` | — |
| Config ▸ General | 4 per-group **reset links** | → **`Entry::ResetToDefault()`** in the Inspector | — |
| Config ▸ Notifications | 3×3 placement grid | `overlay.notification_placement`, one 9-option Choice | `overlay.notification_placement` |
| Config ▸ Notifications | Mute toggle | `notifications.muted` | `notifications.muted` |
| Config ▸ Notifications | Send test notification | `notifications.test` | — |

**Two affordances changed shape** (both in D14, with reasons): the accent **hue gradient
strip and swatch** are not carried over — the hue *setting* is, as a slider — and the
placement **3×3 grid** is a nine-option Choice, because `Kind::Composite` is in the taxonomy
but the shell does not render one yet, so an Anchor would register correctly and draw
nothing (issues #25, #68).

### Reset finally exists

D6 decided reset moves into the Inspector; **no phase had implemented it**, so E2 could not
reset anything at all, and migrating Config would have silently dropped its four per-group
links. It is per-row **and covers the row's parameters**, which is what makes it the
successor to a *group* link: the old "UI Scale" group *is* the `UI scale` row plus its dock
and notification params, so one reset restores exactly what the old link did. A row with no
declared `Default` shows no affordance rather than resetting to zero. Float comparison uses
a tolerance, or a config that had merely been saved and reloaded would light the link
forever.

D6's other half — the accent left edge marking "differs from default" **on the sheet** — is
still unimplemented, and is now the only way that decision is incomplete.

### Deletion is armed, never automatic

The user, after an agent wiped one of their configs: *"There can be a button for it, but
never delete configs automatically."* `Entry::Confirm( prompt )` makes the two-press flow a
property of the **declaration**: the first press swaps the verb and reddens the chip, only
the second performs it, and it disarms on a timeout so a walked-away-from overlay is never
one click from destroying a file.

It is in the registry rather than at the call site because a confirmation a call site has to
remember to build is one the next call site forgets — and a category file cannot open a
modal anyway (SPEC §5.2 clause 0). `config.delete` is the only action in the product that
destroys anything, and it exists only when there is a saved config to destroy.

### A crash, found by `overlay_e2_set`

A registration's setter is reachable from the **console thread**, which has no ImGui
context. `PushLiveTheme()` wrote `ImGui::GetIO().FontGlobalScale` unguarded, so
`overlay_e2_set overlay.display_scale 1.0` killed the compositor. That line is purely the
live **drag preview**, and a console write is not a drag, so it is now guarded; the value
still reaches the UI through `g_LiveTheme`, and `fonts::RebuildAll()` already tolerates a
null current context by design.

This is the second time D13.8's command has paid for itself, and it generalises: **any**
binding that touches ImGui state is now reachable off the draw thread.

### Verified

- `nice -n 19 ninja -C build` clean; **67/67** (`overlay_ui` 34 → 45, `overlay_shell` 16 →
  19, `config` 38 → 40).
- Live session under `scripts/with-gamescope-lock.sh`, `DISABLE_LSFG=1`, no `--backend sdl`,
  no pointer injection — driven entirely by `overlay_e2_*` ConVars and `grim -g` bounded to
  the tracked PID's own window.
- **Audio names, against real streams from two applications.** A `pw-play` stream with
  `application.name` deliberately **empty** and a `media.name` set (issue #63's bluetooth
  case) resolved to *"iPhone von Moritz (codec AAC)"* — the `media.name` tier rescuing it —
  rather than its raw `bluez_input.AA_BB_…` node name. mpv, TeamSpeak, Spotify and
  speech-dispatcher all named correctly alongside it. Audio was **silent** on purpose: the
  point is the metadata, not the sound.
- **A stream ending does not disturb the others.** With five rows on screen, killing mpv
  removed *its* row and left every other row's identity **and value** untouched (TeamSpeak
  97 %, Spotify 52 %), and the counts fell 5 → 4 streams, 7 → 6 rows. That is the property
  the positional-slot design would have broken.
- **All three badge values** seen on screen: `global` (no app id), `app 1174180` (override
  on) and `global only` (Appearance).
- **Inspector scrolling**, at 2.0× on adaptive brightness: the scrollbar appears and **Max
  gain**, the sixth parameter, is reachable. The whole body pans as one; the lane and row
  grammar are unchanged.
- **An existing config loads untouched, verified on disk not on screen.** A pre-E2
  `global.json` with eleven non-default values loads with every one intact **and with its
  mtime, size and bytes unchanged** — reading a config must not write one. A per-game file
  survives the override being turned off and is *restored* when it is turned back on; only
  the explicit delete removes it. Both are permanent tests in `tests/test_config.cpp`.
- **Legacy UI unchanged under `overlay_e2 0`.** Toggled off, on, and off again: every opaque
  UI region (panel title bar, tab strip, dock, hint line) is **pixel-identical**. A
  full-frame compare is meaningless here because vkcube animates behind a translucent
  overlay. The stronger evidence is the diff: the **only** line removed from any legacy file
  in this whole part is the `GetIO()` crash fix.

### Known rough edges, recorded rather than hidden

- **A dynamic area is empty until the overlay has been drawn once.** `SyncIfStale()` runs in
  `Draw()`, so `overlay_e2_select` from the console lists no `audio.node.*` rows until the
  overlay has been opened. Harmless for a user (the rows exist whenever they can be seen)
  but it surprises a script, which is why it is written down.
- **`audio.volume` and `profiles.list` exist conditionally** — the first only when detection
  resolved a game stream, the second only when a profile is saved. `overlay_e2_select`
  correctly reports "no such E2 row" otherwise; that is the registration being honest, not a
  failure.
- **The accent hue gradient and the placement grid** are the two affordances that lost
  fidelity. Both are listed above with their reasons.

---

## 2026-08-23 — P3 part A, Display and Shaders

**Status: `display.gamescope` and `image.shaders` are real registrations.** `EscapeCount()`
**6 → 4**. No config key, value range, format or setter changed — this is a presentation change,
and the legacy panels still draw unchanged under `overlay_e2 0`.

Judgement calls taken during P3 part A: `../../AUTONOMOUS-DECISIONS.md` **D13**.

### The setting inventory, before and after

Nothing was dropped. The two panels' 17 settings map 1:1; three pieces of *prose* became
`.Live()` facts rather than rows (D13.5), and the frame limiter's two controls became one
(D13.3).

| Legacy tab / group | Setting | Now | Config key (unchanged) |
|---|---|---|---|
| Upscaling | Filter | `display.upscaling` ▸ Scaling filter | `gamescope.filter` |
| Upscaling | Sharpness | ″ (disabled unless FSR/NIS) | `gamescope.sharpness` |
| Upscaling | Scaler | ″ | `gamescope.scaler` |
| Upscaling | *Steam-override banner* | → `display.upscaling_facts` `.Live()` | — |
| Display | Allow Tearing | `display.upscaling` ▸ Presentation | `gamescope.tearing_enabled` |
| Display | Force Grab Cursor | ″ | `gamescope.force_grab_cursor` |
| Display | VRR / Adaptive Sync | `display.frame_limiter` | `gamescope.vrr_enabled` |
| Frame Limiter | Unlimited **+** FPS Limit | `display.fps_limit`, **one** Stepper | `gamescope.fps_limit` |
| HDR | HDR | `display.hdr` ▸ Output | `gamescope.hdr_enabled` |
| HDR | SDR Gamut Wideness | ″ | `gamescope.sdr_gamut_wideness` |
| HDR | SDR-on-HDR Brightness | ″ | `gamescope.sdr_on_hdr_brightness_nits` |
| HDR | HDR Input Gain | `display.hdr` ▸ Input gain | `gamescope.hdr_input_gain` |
| HDR | SDR Input Gain | ″ | `gamescope.sdr_input_gain` |
| HDR | *app metadata strip* | → `display.hdr_facts` `.Live()` ×4 | — |
| HDR | *tonemap "deferred" note* | → `display.hdr_facts` `.Live()` | — |
| Shaders | Vibrancy + 2 controls | `image.shaders.vibrancy` + **2 params** | `reshade.vibrancy.*` |
| Shaders | Pre-Sharpen + 1 control | `image.shaders.presharpen` + **1 param** | `reshade.pre_sharpen.*` |
| Shaders | Adaptive Brightness + 6 | `image.shaders.adaptive_brightness` + **6 params** | `reshade.adaptive_brightness.*` |

### Tabs became areas, not groups

`display.gamescope` no longer exists. SPEC §8.1's rail is the product's only navigation and
`index.html` declares Upscaling, Frame limiter and HDR as separate rail items, so a four-tab
panel becomes three areas — a four-group sheet would be the same tab bar redrawn as headings.
The old **Display** tab has no successor: presentation joins Upscaling, VRR joins the limiter.
`display.output` from the mockup was deliberately **not** built — there are no such settings in
this codebase. D13.1, D13.2.

### The Six Budget, reported rather than routed around

**Adaptive brightness owns exactly six params — the budget, with zero headroom.** It fits. It is
called out in `PanelShaders.cpp` and shown to the user as `PARAMETERS 6 of 6`, because the next
parameter added to that effect is not an overflow to work around: it is the signal that the
effect has become a *category*. D13.4.

### What the kit gained (the shared prerequisite)

P2's shell could draw Switch, Choice, Action and Facts — the four kinds `setup.shell` used. A
populated registry needs more:

- **`DrawSharedControl()`**, one painter templated over `Entry` *and* `Parameter`. They are
  distinct types on purpose, but SPEC §5.3 requires a Param to render identically to the Entry it
  could be promoted into — one function over both is what guarantees that.
- **Slider, SliderInt, Stepper, Meter** rows; int-vs-float is decided by what the binding holds.
- **Group bands** (SPEC §2.5), including `GroupCount()`'s `n / m`, computed from the band's own
  switch rows so the number cannot be typed.
- **Units and zero-words** — `ZeroMeans("Unlimited")` replaces the value, the unit is appended.
  Never baked into a value by a call site.
- **`ui::ScopedDim`**, under `Col()`/`Accent()`. SPEC §3.13's "row × 0.55" includes the control,
  and the atoms paint on the draw list where ImGui's disabled alpha never reaches. D13.6.
- **The Choice dropdown**, for when the segmented group does not fit its lane. P2 ignored
  `bWantsPopup`; a five-option filter row in a narrow sheet needs it. D13.7.
- **`Parameter`'s read side** — `HasRange/Lo/Hi/StepSize/Unit/ZeroWord/UsesValue/DisabledReason`,
  matching `Entry`'s name for name. `Range()` now also resolves the kind to Slider.

### Verified

`ninja -C build` clean. `meson test -C build` **67/67**; `[overlay_ui]` 30 → **34** cases,
285 assertions. Mutation-checked: removing `Range()`'s kind resolution fails the new param test.

Launched under `scripts/with-gamescope-lock.sh` at display_scale **1.0× and 2.0×**, in CONFIGURE
and DETAILS, on all four migrated areas, against a temporary `XDG_CONFIG_HOME`. Group bands, the
`0 / 3` effect count, `PARAMETERS 6 of 6`, dimmed disabled rows with their amber reason, units
(`25 %`, `203 nits`, `1 x`, `30 fps`) and the Facts/Live blocks all render as the mockup shows.

**A binding was proven to drive the compositor, not just the UI.** Writing through
`overlay_e2_set` — the same `Binding().Set()` a click calls — moved gamescope's own frame pacing
every time:

```
display.fps_limit = 60  ->  Swapchain received new refresh cycle: 16.67ms
display.fps_limit = 30  ->  Swapchain received new refresh cycle: 33.33ms
display.fps_limit = 5   ->  clamped to 10, cycle 100.00ms   <- issue #67's floor holds
display.fps_limit = 0   ->  Swapchain received new refresh cycle: 3.57ms (uncapped)
```

That is issue #25's exact failure mode tested directly: the control writes, and this time the
value sticks.

### Known rough edges, recorded rather than hidden

- **The Inspector does not scroll.** At 2.0× the ladder gives a drawer, and Adaptive brightness's
  six params overflow its bottom edge — the last row is clipped. P2 never hit this because its
  one real area had three rows and no params. `DrawConfigure()` lays out with absolute `y` on the
  draw list rather than through ImGui's cursor, so a scrollbar is real work, not a flag. Belongs
  to the P3 part that has the most params, not to this one.
- **`display.gamescope` as an id is gone.** Anything scripted against
  `overlay_e2_select display.gamescope` needs the three new ids.

### Still deferred

Unchanged from P2's list: the eleven rail icons, `Ctrl+K`, multi-column sheets, a sixth baked font
style, `Cfg()`, `Repeat()`, `ui_lint`, `ui_snapshot`. The four remaining escaped areas
(`audio.mixer`, `system.monitor`, `system.log`, `setup.config`) are the later parts of P3.

---

## 2026-08-23 — P2, the shell

**Status: the shell exists and is off by default.** `overlay_e2 0` (the default) draws the
legacy dock and its five floating windows exactly as before; `overlay_e2 1` replaces all of
it. No on-disk config key or format changed.

Phase plan: `../../AUTONOMOUS-DECISIONS.md` D10. Judgement calls taken during P2: **D12**.

### What was added

| File | What it owns | ImGui? |
|---|---|---|
| `UI/Layout.h/.cpp` | the slab, SPEC §8.3's ladder, the region rects, mode selection, the mode-strip counters | **no** |
| `UI/Shell.h/.cpp` | the slab window, the three regions, the keyboard map, the registry | yes |
| `tests/test_overlay_shell.cpp` | `[overlay_shell]` — the ladder table, region arithmetic, mode selection, `Law::Escaped` | no |

Plus: `Area::Escape()` and `Law::Escaped` in `Registry.*`; the registry's read side
(`Verb()`, `Invoke()`, `SummaryText()`, `Scalar()`, `Area::Available()`,
`Registry::FindArea()`, `KindName()`); a `Panel*_DrawBody()` on each of the five panels; and
`chrome::EnsureThemeLoaded()`.

### The three-region split, and why it is shaped this way

ImGui here is **1.92.9b stock, non-docking** — no dock-space, no splitter. But the reason
the split is hand-rolled is not availability: **a splitter is stored, mutable, per-user
geometry**, which is the exact category of state whose bug history motivated the redesign.

- **One top-level window — the slab.** `NoMove | NoResize | NoCollapse | NoSavedSettings |
  NoBringToFrontOnFocus`, no title bar, positioned and sized every frame from `Layout.h`.
  Its geometry is a pure function of surface size and `display_scale`; there is no stored
  layout to corrupt, migrate or reset.
- **The regions are child windows inside it.** A child gives clipping and independent
  scrolling — all the split needs — and gives no drag, no resize and no z-order, which is
  all the split must never acquire.

**Shell state, in full:** selected area, selected row, host preference, mode override. No
window positions, no sizes, no z-order, no open/closed set. That list *is* the difference
between this and what it replaces.

**Z-order is bought with begin-order, deliberately.** The drawer must paint over the sheet;
ImGui renders every child after its parent's own commands, so an inspector background on the
slab's draw list loses to the sheet's child no matter how late it is drawn. The whole
inspector region is therefore one child begun *after* the sheet's. `ImDrawListSplitter` was
rejected — it would make paint order a thing every future edit has to know about.

### `Escape()` — the migration seam

```cpp
reg.Add( "display.gamescope", "Gamescope", Section::Display )
   .Escape( []{ PanelDisplay_DrawBody(); } );
```

The shell pushes the padding legacy code expects, runs the body in the sheet's child, pops.
Six areas use it. **It looks wrong on purpose** — the sheet head labels the category
`legacy body` in `WarnText` and Overview says the shell cannot see inside it. There is no
styling that tries to blend a legacy body into an E2 sheet, because a half-convincing blend
is how a migration stops being urgent.

Two limits, both `Law::Escaped`, both in `AUTONOMOUS-DECISIONS.md` D12.4:

- **Sheet only.** No Inspector equivalent, ever — that would be SPEC §5.2 clause 0's
  forbidden fifth generator.
- **Legacy or E2, never both.** Escaping a populated area and populating an escaped one both
  fire. The half-migrated area is the shape that survives P3 indefinitely.

`Registry::EscapeCount()` is what a future `ui_lint` reports as severity `migration`.
It is **6** today and P3 drives it to zero, deleting `Escape()` with the last one.

### What the shell registers

Seven areas in SPEC §8.1's three sections (after D8's fold): `display.gamescope`,
`image.shaders` | `audio.mixer`, `system.monitor`, `system.log` | `setup.config`,
`setup.shell`. The first six are escaped. `setup.shell` is genuinely E2 and exists so both
Inspector modes ship exercised rather than as dead code — see D12.3. **Every one of its
bindings is to shell runtime state or a ConVar, never to a config field.**

### Verified

`ninja -C build` clean. `meson test -C build` **67/67** — P1's 66 plus `overlay_shell`.

`[overlay_shell]` pins SPEC §8.3's worked table row by row across `display_scale`
0.5×–2.0×, the ladder's step *order*, host preference in both directions, the content cap on
columns, the slab's surface clamp and purity, region rects tiling the slab at four scales in
all three hosts, the drawer overlapping where a column does not, the spine carved out rather
than laid over, mode selection per kind, the derived counters, and `Law::Escaped` in both
directions. Verified to fail under a deliberate mutation (swapping the ladder's two steps
fails at 1.5×, the only scale that separates them).

Visually confirmed under `scripts/with-gamescope-lock.sh` at 1.0× and 2.0×, in all three
inspector hosts, in both modes, on an escaped area, and with the ConVar toggled off again —
the dock returns identically, with no leftover state. At 2.0× the ladder reproduces §8.3's
row exactly: slab 1728 × 929 px (864 × 464 base), rail 60, drawer 400, sheet 804, step 2.

### Deferred to P3, deliberately

- **SPEC §8.0's eleven rail icons.** The rail draws initials. D12.8.
- **The command palette, `Ctrl+K`.** It walks a populated registry; there is one real area.
- **Multi-column sheets.** `LadderResult::nColumns` is computed and tested; the sheet draws
  one column, because the only area with rows has three of them.
- **A sixth baked font style.** Six type roles map onto five. D12.11.
- **`Cfg()`, `Repeat()`, `ui_lint`, `ui_snapshot`.** Unchanged from P1's list — all need a
  populated registry.

### Known rough edges, recorded rather than hidden

- A legacy body wider than its sheet column gets a **horizontal scrollbar**. The panels were
  authored for a ~440-wide resizable window. An unreachable control is worse than a
  scrollbar in a region that will not have one after P3.
- **`Tab` region cycling is tracked but not yet wired to ImGui focus.** `s_eFocusRegion`
  moves; nothing reads it. Arrow-key navigation within a region is P3's, with the rows.

---

## 2026-08-23 — P1, the kit foundation

**Status: new code only. Nothing here is called by the running UI.** The only
existing files touched are `src/meson.build` and `tests/meson.build`, both for
build wiring.

### Where it lives, and why there

`src/Overlay/UI/` — the location `API.md`'s own "Proposed location" section
names, together with the filenames it lists. Deviating would have made every
path in the contract wrong on day one.

| File | What it owns | ImGui? |
|---|---|---|
| `Tokens.h/.cpp` | geometry, spacing scale, type roles, `display_scale`, motion | **no** |
| `Lane.h/.cpp` | SPEC §2.1's four columns, as pure arithmetic | no |
| `Row.h/.cpp` | `RowCtx` — the one right-bound allocator | ImRect only |
| `Band.h/.cpp` | SPEC §4.2's n × 44 composite band rule | ImRect only |
| `Registry.h/.cpp` | `Area` / `Entry` / `Parameter`, the four laws | **no** |
| `Colors.h/.cpp` | SPEC §7.1's colour roles | yes (accent is hue-live) |
| `Controls.h/.cpp` | the control atoms | yes |

**Why the split runs along the ImGui line.** Everything with real arithmetic or
a real rule in it is free of an ImGui context, a font atlas and the overlay's
live-theme globals. That is what makes the laws and the lane cheap to test —
`tests/test_overlay_ui.cpp` needs no window. Only the two files that pick
colours and hand rects to stock ImGui primitives need a context, and those are
covered by a headless harness (`tests/test_overlay_atoms.cpp`).

### The four laws, and where each is enforced

| Law | Enforced | How |
|---|---|---|
| **Prefix Law** (§5.2 cl. 2) | construction | `Param()` takes a *leaf*; the id is synthesised as `<parent>.<leaf>` and there is no other route. Re-checked in `Registry::SelfTest()` so a test can observe it. |
| **One Level** (§5.2 cl. 3) | **compile time** + runtime | `Parameter` has no nesting operation; its `.Param()` is a *sibling* factory on the parent Entry. A dot in a leaf — the only way to smuggle nesting into an id — is a runtime violation. |
| **Six Budget** (§5.2 cl. 3) | registration | the 7th `.Param()` reports `Law::SixBudget` and is not added. |
| **id uniqueness** | registration | one registry-wide claim list; areas, entries and params share the namespace, because the palette and `ui_snapshot` address all three by id. |
| **`Help()` required** (§5.2 cl. 1) | call + `SelfTest()` | empty/null text fires at the call; *never called at all* can only be decided once registration is over, so it fires in `SelfTest()`. |
| **Details is read-only** (§5.2 cl. 4) | **compile time** | `.Live()` takes `std::function<Fact()>` and has no `Bind` overload. |
| **`DisabledUnless` needs a reason** | **compile time** + runtime | no overload without the string; an empty string is the residual hole and is closed. |

**What happens when a law fires.** The project builds with `-fno-exceptions`,
so a violation cannot be thrown. The default handler prints and `abort()`s — a
malformed registry is a boot failure, not a runtime condition anyone handles.
`ui::LawRecorder` is the one seam that changes what happens *after* a violation
fires, never whether it fires; the tests install one so they can assert *which*
law caught the thing without terminating the binary. Nothing in the shipping
build constructs one.

### Left alignment is unrepresentable

`RowCtx` exposes `Place( widthBase )`, `PlacePx( widthPx )` and `PlaceFull()`.
None takes an x. Both public entry points route through one rect construction
whose `Max.x` is the lane's control edge, unconditionally, with no flag to
change it. There is no `Left()`, no `Centre()`, no `SameLine`, no
`SetCursorPosX`, and no atom touches ImGui's cursor — so a competing layout
system has nothing to act on even if someone called into it.

`SplitLabelZone()` returns the label rect and the value rect from **one**
subdivision, so the two cannot be computed inconsistently.

### The drawn-vs-hit-tested divergence (#23's bug class)

Two structural rules, both in `Controls.cpp`:

1. **One `ImRect` per atom.** `Begin()` registers the rect with `ItemAdd()` and
   hands the *same object* back for painting. An atom never recomputes its own
   geometry.
2. **The slider's handle is not a constant.** `SliderGrab()` is the only code
   in the kit that names a grab width. It pushes that width into
   `GrabMinSize`, calls `SliderBehavior()`, pops, and returns the grab rect
   `SliderBehavior` itself produced. `PaintSlider()` draws that rect and is
   never told how wide a handle should be. There is one number and the drawing
   code cannot see it, so a future edit moves the visible handle and the
   draggable target together or not at all.

The same shape covers the measured atoms: `MeasureCells()` is the single
function that decides how wide a segmented group or a chip bank is, and *both*
the "does it fit the lane" predicate and the per-cell layout read its output. A
control cannot be laid out to a width its own fit test never saw.

### Tests

| Suite | Covers |
|---|---|
| `tests/test_overlay_ui.cpp` (`[overlay_ui]`) | token derivation across `display_scale` 0.5–2.0, the hairline floor rule, lane arithmetic against SPEC §8.3's worked example, the allocator's right-edge invariant, the label/value split, band quantisation, and every law made to **fire** |
| `tests/test_overlay_atoms.cpp` (`[overlay_atoms]`) | ImGui run headlessly — no window, no renderer. Asserts the registered hit box equals the allocated rect at four scales, that every atom leaves the style/colour/ID/item-flag stacks balanced, that a click lands on the lane rect and *not* where a left-aligned control would have been, and that `Choice` downgrades to a dropdown on all four of its conditions |

Both suites were verified to fail under a deliberate mutation (making `Place()`
left-bound) before being accepted.

**The harness needs one hack, recorded here so it is not mistaken for a
pattern:** `palette::g_LiveTheme` is defined in `Chrome.cpp`, so the test binary
defines the storage itself rather than linking 1,600 lines of legacy dock
machinery to read one float. The kit never reads that global — `ui::SetScale()`
is its one scale input — so this only satisfies `Palette.cpp`'s accent
recomputation.

### Deferred to later phases, deliberately

Not omissions; each has a reason.

- **The dropdown popup and the text field's validation plumbing.** `index.html`
  itself calls the popup "shell furniture, not a control atom". The closed
  states of both atoms are complete; the popup, its clipper and the
  Configure-hosted validation message belong to the shell (P2).
- **`Area::Repeat()`** (dynamic collections). It is a rebuild *policy*, not a
  foundation law, and it needs the shell's frame loop to have a topology-change
  hook to attach to.
- **`Cfg()`** — schema-backed bindings that know their destination file.
  `AnyBind` covers pointer and getter/setter binding today; `Cfg()` is the
  config seam and lands with P2, where `ConfigManager`'s write queue is wired.
- **`Escape()`** (API.md §13's migration seam) — it exists to host legacy panel
  bodies inside the sheet, which requires the sheet.
- **`ui_lint` / `ui_snapshot` / `--host=inline`.** They walk a *populated*
  registry; there are no areas until P3.

### Judgement calls

Recorded in `../../AUTONOMOUS-DECISIONS.md` under **D11**, with the reasoning.
The two a reader is most likely to trip over:

- **`SPEC.md` §2.2's lane clamp is arithmetically self-contradictory** and was
  resolved against the mockup and §8.3's worked numbers. See `Lane.h`'s header
  comment for the full derivation.
- **The class is `ui::Parameter`, not `ui::Param`.** C++ forbids a member
  function whose name matches its enclosing class, and API.md §7's chained
  `.Param()` declarations require exactly that member. The concept and the laws
  are unchanged.
